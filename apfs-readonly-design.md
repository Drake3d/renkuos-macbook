# APFS file system: read-only design (write later)

Status: design only, nothing implemented (2026-11-25). Same approach as the
camera and sound drivers: write new MIT code, use Apple's published
specification as the main source, use GPL drivers for facts only, and record
where every fact came from.

Goal of this stage: mount unencrypted APFS volumes read-only in RenkuOS and
read every file, directory, symlink and attribute on them, including
compressed files. The code is laid out so write support can be added later
without a rewrite (section 9).

Values marked **(verify)** are from memory and must be checked against the
Apple File System Reference before code depends on them.

## 1. Sources

Primary (license-clean):
- **Apple File System Reference** (Apple, public PDF, developer.apple.com).
  On-disk structures, constants, algorithms. Every structure in the driver
  names the section it comes from.
- **Apple LZFSE** (github.com/lzfse/lzfse, BSD-3-Clause): reference LZFSE
  and LZVN decoders. BSD-3 is compatible with MIT; keep its copyright notice
  in the imported files.
- **Real images made on this Mac** with `hdiutil` / `diskutil` (section 8):
  ground truth for every structure.

Facts only, never code (GPL/LGPL):
- linux-apfs-rw (GPL-2.0), apfs-fuse (GPL-2.0), libfsapfs (LGPL-3.0). Use to
  answer "what does this field really do" when the reference is unclear;
  note the question and answer in this file.

## 2. Existing code to follow in the RenkuOS tree

- `src/add-ons/kernel/file_systems/xfs/` and `btrfs/`: read-mostly drivers
  built on B-trees. Use their split (Volume, Inode, BTree, DirectoryIterator,
  Attribute, kernel_interface.cpp) as the template.
- `btrfs/CRCTable.cpp`: a CRC32C table (APFS name hashes use CRC32C,
  **verify**). Share or copy it.
- `btrfs/Jamfile`: shows linking `kernel_libz.a` with
  `UseBuildFeatureHeaders zlib` (needed for zlib-compressed files).
- `src/tests/add-ons/kernel/file_systems/xfs/xfs_shell/`: host test shell
  that runs the real driver code on macOS against an image file. Make
  `apfs_shell` the same way; most testing happens here, not on the MacBook.
- Partition detection: the driver's `identify_partition` /
  `scan_partition` hooks (same as xfs/btrfs). GPT type GUID for an APFS
  container: `7C3457EF-0000-11AA-AA11-00306543ECAC` (**verify**).

## 3. On-disk layout (summary from the reference)

Container (one per partition) holds one or more volumes:

- Every metadata object starts with `obj_phys_t` (32 bytes): Fletcher-64
  checksum, object id, transaction id (xid), type, subtype.
- **Container superblock** (`nx_superblock_t`, magic `NXSB`) at block 0.
  Block 0 may be stale: the newest valid copy is in the checkpoint
  descriptor area (`nx_xp_desc_base`, `nx_xp_desc_blocks`). Pick the valid
  superblock with the highest xid.
- Object ids are **physical** (a block address), **ephemeral** (found via
  the checkpoint map) or **virtual** (looked up in an object map).
- **Object map** (`omap_phys_t`): a B-tree from (oid, xid) to physical
  address. The container has one; each volume has its own.
- **Volume superblock** (`apfs_superblock_t`, magic `APSB`): name, role,
  feature flags (case-insensitive, normalization-insensitive), the volume's
  object map, and the root of its file-system tree.
- **File-system tree**: one B-tree per volume holding every record, keyed
  by object id + record type (type in the top 4 bits **verify**): inode,
  directory entry, extended attribute, file extent, data stream, sibling
  link/map (hard links), crypto state, directory stats, snapshot name.
- Root directory is inode 2; the private directory is inode 3 (**verify**).
- File data: `j_file_extent` records map logical offset to physical blocks.
  Extents may be shared between files (clones); read-only does not care.
- **Snapshots**: a separate snapshot metadata tree; a snapshot is a volume
  superblock at an older xid. Read-only can mount the live state first and
  expose snapshots later.

B-tree nodes (`btree_node_phys_t`): flags (root, leaf, fixed-size keys),
a table of contents, a key area growing forward and a value area growing
backward; the root node ends with `btree_info_t` (key/value sizes, counts).
Non-leaf values are child object ids, virtual or physical depending on the
tree.

## 4. Driver structure

```
src/add-ons/kernel/file_systems/apfs/
  apfs.h               on-disk structures and constants (cite reference sections)
  Checksum.cpp/.h      Fletcher-64 object checksum
  Container.cpp/.h     container superblock, checkpoint scan, ephemeral map
  ObjectMap.cpp/.h     (oid, xid) -> paddr lookups, small cache
  BTree.cpp/.h         generic node reader, search, iterator (fixed and variable keys)
  Volume.cpp/.h        volume superblock, fs tree, feature flags, mount state
  Inode.cpp/.h         inode record, size, times, mode, data stream, extents
  DirectoryIterator.*  directory records, name hashing and comparison
  Attribute.*          extended attributes (inline and stream-backed)
  Decmpfs.cpp/.h       transparent decompression (zlib, LZVN, LZFSE)
  lzfse/               imported Apple LZFSE/LZVN decoder (BSD-3)
  kernel_interface.cpp file system module hooks
  Jamfile
src/tests/add-ons/kernel/file_systems/apfs/apfs_shell/
```

Hooks for read-only: identify/scan partition, mount/unmount, read_fs_info,
get_vnode/put_vnode, lookup, read_stat, open/close/free_cookie, read,
read_symlink, open_dir/read_dir/rewind_dir, open_attr_dir/read_attr_dir,
open_attr/read_attr/read_attr_stat, and the file cache / file map hooks
(`get_file_map`) so normal reads go through the block cache. Every
modifying hook returns `B_READ_ONLY_DEVICE`.

Several volumes in one container: first stage mounts one volume per
mount, chosen by a mount parameter (name or index; default: the first
volume whose role is none/data, **verify** role values).

## 5. Names

- Directory records use hashed keys: name length plus a hash of the name
  (CRC32C over the normalized name, **verify** exact steps), so lookup is a
  B-tree search, not a scan.
- Normalization-insensitive volumes (the default on macOS): compare names
  after Unicode normalization and, on case-insensitive volumes, case
  folding. This needs normalization and case-fold tables in the kernel.
  Plan: generate compact tables from the Unicode data files (Unicode
  license, permissive) with a build-time script; do not copy tables from
  GPL code.
- Fallback for the first milestone: exact-byte comparison plus a linear
  scan of the directory when the hashed lookup misses, so lookups work
  before the tables are done.

## 6. Compressed files (decmpfs)

Many macOS system files are stored compressed. The inode has the
compressed flag; the data is in the `com.apple.decmpfs` attribute (header
plus small data inline) or in the `com.apple.ResourceFork` attribute.
Compression types (**verify** numbers): uncompressed inline, zlib, LZVN,
LZFSE, each as "inline in the attribute" or "in the resource fork" in
chunks of 64 KiB. Reads decompress whole chunks into a small cache;
`read_stat` reports the uncompressed size from the decmpfs header.
zlib from the kernel's `kernel_libz.a`; LZVN/LZFSE from Apple's BSD code.

## 7. Not in the first stage

- Encrypted volumes (FileVault, software keys; no T2 on MacBookPro14,1):
  detect, refuse to mount with a clear syslog line. Later stage needs the
  keybag, password prompt and AES-XTS.
- Fusion drives (two devices in one container).
- Sealed system volumes: reading works like any volume; integrity checks
  are not verified.
- Write support (section 9).

## 8. Testing (on this Mac, before the MacBook)

Make images with macOS tools, then run `apfs_shell` against them:

```
hdiutil create -size 200m -fs APFS -volname Plain plain.dmg
hdiutil create -size 200m -fs "Case-sensitive APFS" -volname Case case.dmg
```

Fill them with: deep directories, long and Unicode names (NFC and NFD
forms of the same name), hard links, symlinks, sparse files, large files
(multi-extent), clones (`cp -c`), extended attributes, compressed files
(`ditto --hfsCompression`), and a snapshot. Compare `apfs_shell` listings
and file checksums with what macOS shows for the same image. Keep the
generator script and expected checksums in the test folder; keep the
images themselves out of git.

Then on the MacBook: a USB drive formatted APFS on this Mac, and (read
only) the MacBook's own macOS partition if it is still there.

## 9. Keeping the door open for write support

- All block access goes through one block-cache layer that knows the
  object type and checksums, so writing can add copy-on-write there.
- The B-tree code keeps search/iterate separate from node layout so
  insert/split/merge can be added beside it.
- Transaction ids are tracked everywhere objects are read (needed for
  copy-on-write and checkpoints later).
- Write support needs, at least: space manager (free space), checkpoint
  writing, object map updates, the reaper, B-tree modification, Fletcher-64
  on every written object, and careful crash testing. Estimate it
  separately once read-only is solid.

## 10. Milestones

1. Container: find the newest superblock, walk the checkpoint, list
   volumes (name, role, features) in `apfs_shell`. ~1 session.
2. Volume tree: list the root directory, stat files, read small
   uncompressed files. 1–2 sessions.
3. Full read path: extents and file cache, symlinks, hard links,
   attributes, hashed name lookup with normalization. 1–2 sessions.
4. Compressed files (zlib, LZVN, LZFSE). 1–2 sessions.
5. Build in the image (optional package like the camera), test on the
   MacBook with a USB drive, harden against corrupt images. ~1 session.

Rough total: 5–8 working sessions for read-only.
