"""Copyright 2026, RenkuOS contributors. Distributed under the MIT License.

Builds decmpfs-compressed files by hand on macOS, to check layouts against
macOS itself: if macOS reads the file back as the original data, the layout
is right. Used for the APFS driver's compressed-file support.

usage: craft_decmpfs.py <type> <input> <output>
types: 1 (raw in xattr), 3 (zlib in xattr), 30 (type 3 stored raw),
       7 (LZVN in xattr), 70 (type 7 stored raw), 80 (type 8 with a raw
       chunk), 11 (LZFSE in xattr), 12 (LZFSE in resource fork),
       4 (zlib in a classic resource fork: macOS refuses these hand-built
       files, though the chunk table matches real ones; for real type 4
       files copy an App Store app with ditto --preserveHFSCompression)
LZVN needs the lzvn_enc helper (lzvn_enc.c) built next to this script as
tools/lzvn_enc; LZFSE uses macOS's compression_tool.
"""
import os, struct, subprocess, sys, tempfile, zlib

CHUNK = 65536


def lzfse(data):
    with tempfile.TemporaryDirectory() as d:
        i, o = os.path.join(d, 'in'), os.path.join(d, 'out')
        open(i, 'wb').write(data)
        subprocess.run(['compression_tool', '-encode', '-a', 'lzfse',
            '-i', i, '-o', o], check=True)
        return open(o, 'rb').read()


def lzvn(data):
    return subprocess.run([os.path.join(os.path.dirname(os.path.abspath(__file__)),
        'tools', 'lzvn_enc')], input=data, capture_output=True, check=True).stdout


def header(kind, size):
    # magic 'fpmc', compression type, uncompressed size
    return struct.pack('<4sIQ', b'fpmc', kind, size)


def offset_table_fork(chunks):
    # u32 offsets (little endian) from the start of the fork: one per chunk,
    # then the end; the first offset is the table's own size.
    table = 4 * (len(chunks) + 1)
    offsets, pos = [], table
    for c in chunks:
        offsets.append(pos)
        pos += len(c)
    offsets.append(pos)
    return struct.pack('<%dI' % len(offsets), *offsets) + b''.join(chunks)


def zlib_fork(chunks):
    # Classic resource fork: 256-byte header (big endian), data section at
    # 0x100 = u32 BE data length, then u32 LE block count and (offset,
    # size) pairs relative to 0x104, then the blocks; a 50-byte map last.
    count = len(chunks)
    table = 4 + 8 * count
    entries, pos = [], table
    for c in chunks:
        entries.append(struct.pack('<II', pos, len(c)))
        pos += len(c)
    blocks = struct.pack('<I', count) + b''.join(entries) + b''.join(chunks)
    data_len = len(blocks)
    data = struct.pack('>I', data_len) + blocks
    map_off = 0x100 + len(data)
    head = struct.pack('>IIII', 0x100, map_off, len(data), 50)
    resmap = head + bytes(8) + struct.pack('>HHH', 0, 28, 50 - 18) + \
        struct.pack('>H', 0) + b'cmpf' + struct.pack('>HH', 0, 10) + \
        struct.pack('>hHI', 1, 0xffff, 0) + bytes(4)
    resmap = resmap[:50].ljust(50, b'\0')
    return head + bytes(0x100 - 16) + data + resmap


def main():
    kind, src, dst = int(sys.argv[1]), sys.argv[2], sys.argv[3]
    data = open(src, 'rb').read()
    chunks = [data[i:i + CHUNK] for i in range(0, len(data), CHUNK)]

    fork = None
    if kind == 1:
        xattr = header(1, len(data)) + data
    elif kind == 3:
        xattr = header(3, len(data)) + zlib.compress(data, 9)
    elif kind == 7:
        xattr = header(7, len(data)) + lzvn(data)
    elif kind == 70:
        # type 7 stored raw: 0x06, then the data uncompressed
        xattr = header(7, len(data)) + b'\x06' + data
    elif kind == 80:
        # type 8 with one chunk stored raw (0x06 + data), the rest LZVN
        xattr = header(8, len(data))
        fork = offset_table_fork([b'\x06' + chunks[0]] + [lzvn(c) for c in chunks[1:]])
    elif kind == 30:
        # type 3 stored raw: 0xff, then the data uncompressed
        xattr = header(3, len(data)) + b'\xff' + data
    elif kind == 11:
        xattr = header(11, len(data)) + lzfse(data)
    elif kind == 12:
        xattr = header(12, len(data))
        fork = offset_table_fork([lzfse(c) for c in chunks])
    elif kind == 4:
        xattr = header(4, len(data))
        fork = zlib_fork([zlib.compress(c, 9) for c in chunks])
    else:
        sys.exit('unknown type')

    if os.path.exists(dst):
        os.remove(dst)
    open(dst, 'wb').close()
    if fork is not None:
        subprocess.run(['xattr', '-wx', 'com.apple.ResourceFork', fork.hex(),
            dst], check=True)
    subprocess.run(['xattr', '-wx', 'com.apple.decmpfs', xattr.hex(), dst],
        check=True)
    subprocess.run(['chflags', 'compressed', dst], check=True)
    os.chmod(dst, 0o644)


main()
