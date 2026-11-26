/*
 * Copyright 2026, RenkuOS contributors.
 * Distributed under the terms of the MIT License.
 *
 * On-disk structures of the Apple File System (APFS), read-only subset.
 *
 * Source: Apple File System Reference, revision 2020-06-22 (Apple, public
 * PDF). Section names are given beside each structure. All on-disk values
 * are little endian; read them through the accessors below.
 */
#ifndef APFS_H
#define APFS_H


#include "system_dependencies.h"


typedef uint64 apfs_oid_t;		// "oid_t"
typedef uint64 apfs_xid_t;		// "xid_t"
typedef int64 apfs_paddr_t;		// "paddr_t"


// #pragma mark - Objects ("Objects" chapter)


#define APFS_MAX_CKSUM_SIZE				8

#define APFS_OID_NX_SUPERBLOCK			1
#define APFS_OID_INVALID				0
#define APFS_OID_RESERVED_COUNT			1024

// "Object Type Masks"
#define APFS_OBJECT_TYPE_MASK			0x0000ffff
#define APFS_OBJECT_TYPE_FLAGS_MASK		0xffff0000
#define APFS_OBJ_STORAGETYPE_MASK		0xc0000000

// "Object Types"
#define APFS_OBJECT_TYPE_NX_SUPERBLOCK	0x00000001
#define APFS_OBJECT_TYPE_BTREE			0x00000002
#define APFS_OBJECT_TYPE_BTREE_NODE		0x00000003
#define APFS_OBJECT_TYPE_SPACEMAN		0x00000005
#define APFS_OBJECT_TYPE_OMAP			0x0000000b
#define APFS_OBJECT_TYPE_CHECKPOINT_MAP	0x0000000c
#define APFS_OBJECT_TYPE_FS				0x0000000d
#define APFS_OBJECT_TYPE_FSTREE			0x0000000e
#define APFS_OBJECT_TYPE_SNAPMETATREE	0x00000010

// "Object Type Flags"
#define APFS_OBJ_VIRTUAL				0x00000000
#define APFS_OBJ_EPHEMERAL				0x80000000
#define APFS_OBJ_PHYSICAL				0x40000000
#define APFS_OBJ_NOHEADER				0x20000000
#define APFS_OBJ_ENCRYPTED				0x10000000
#define APFS_OBJ_NONPERSISTENT			0x08000000


// "obj_phys_t": header at the start of every object.
struct apfs_obj_phys {
	uint8	cksum[APFS_MAX_CKSUM_SIZE];	// Fletcher 64
	uint64	oid;
	uint64	xid;
	uint32	type;
	uint32	subtype;

	uint64	Checksum() const
		{
			uint64 value;
			memcpy(&value, cksum, sizeof(value));
			return B_LENDIAN_TO_HOST_INT64(value);
		}
	apfs_oid_t	ObjectID() const { return B_LENDIAN_TO_HOST_INT64(oid); }
	apfs_xid_t	TransactionID() const
		{ return B_LENDIAN_TO_HOST_INT64(xid); }
	uint32	Type() const
		{ return B_LENDIAN_TO_HOST_INT32(type) & APFS_OBJECT_TYPE_MASK; }
	uint32	TypeFlags() const
		{ return B_LENDIAN_TO_HOST_INT32(type) & APFS_OBJECT_TYPE_FLAGS_MASK; }
	uint32	Subtype() const { return B_LENDIAN_TO_HOST_INT32(subtype); }
} _PACKED;


// "prange_t"
struct apfs_prange {
	int64	start_paddr;
	uint64	block_count;
} _PACKED;


// #pragma mark - Container ("Container" chapter)


// "nx_superblock_t" constants
#define APFS_NX_MAGIC					0x4253584e	// 'NXSB' on disk
#define APFS_NX_MAX_FILE_SYSTEMS		100
#define APFS_NX_EPH_INFO_COUNT			4
#define APFS_NX_NUM_COUNTERS			32

// "Block and Container Sizes"
#define APFS_NX_MINIMUM_BLOCK_SIZE		4096
#define APFS_NX_DEFAULT_BLOCK_SIZE		4096
#define APFS_NX_MAXIMUM_BLOCK_SIZE		65536
#define APFS_NX_MINIMUM_CONTAINER_SIZE	1048576

// "Incompatible Container Feature Flags"
#define APFS_NX_INCOMPAT_VERSION1		0x0000000000000001ULL
#define APFS_NX_INCOMPAT_VERSION2		0x0000000000000002ULL
#define APFS_NX_INCOMPAT_FUSION			0x0000000000000100ULL

// "Container Flags"
#define APFS_NX_CRYPTO_SW				0x0000000000000004ULL

// nx_xp_desc_blocks / nx_xp_data_blocks: the highest bit says the area is
// not contiguous (see "nx_xp_desc_base").
#define APFS_NX_XP_NONCONTIGUOUS		0x80000000

// GPT partition type of an APFS container ("Partition UUIDs"):
// 7C3457EF-0000-11AA-AA11-00306543ECAC


// "nx_superblock_t": container superblock. All fields 64-bit aligned.
struct apfs_nx_superblock {
	apfs_obj_phys	o;
	uint32	magic;
	uint32	block_size;
	uint64	block_count;
	uint64	features;
	uint64	readonly_compatible_features;
	uint64	incompatible_features;
	uint8	uuid[16];
	uint64	next_oid;
	uint64	next_xid;
	uint32	xp_desc_blocks;
	uint32	xp_data_blocks;
	int64	xp_desc_base;
	int64	xp_data_base;
	uint32	xp_desc_next;
	uint32	xp_data_next;
	uint32	xp_desc_index;
	uint32	xp_desc_len;
	uint32	xp_data_index;
	uint32	xp_data_len;
	uint64	spaceman_oid;
	uint64	omap_oid;
	uint64	reaper_oid;
	uint32	test_type;
	uint32	max_file_systems;
	uint64	fs_oid[APFS_NX_MAX_FILE_SYSTEMS];
	uint64	counters[APFS_NX_NUM_COUNTERS];
	apfs_prange	blocked_out_prange;
	uint64	evict_mapping_tree_oid;
	uint64	flags;
	int64	efi_jumpstart;
	uint8	fusion_uuid[16];
	apfs_prange	keylocker;
	uint64	ephemeral_info[APFS_NX_EPH_INFO_COUNT];
	uint64	test_oid;
	uint64	fusion_mt_oid;
	uint64	fusion_wbc_oid;
	apfs_prange	fusion_wbc;
	uint64	newest_mounted_version;
	apfs_prange	mkb_locker;

	uint32	Magic() const { return B_LENDIAN_TO_HOST_INT32(magic); }
	uint32	BlockSize() const { return B_LENDIAN_TO_HOST_INT32(block_size); }
	uint64	BlockCount() const { return B_LENDIAN_TO_HOST_INT64(block_count); }
	uint64	IncompatibleFeatures() const
		{ return B_LENDIAN_TO_HOST_INT64(incompatible_features); }
	uint32	DescriptorBlocks() const
		{ return B_LENDIAN_TO_HOST_INT32(xp_desc_blocks)
			& ~APFS_NX_XP_NONCONTIGUOUS; }
	bool	DescriptorContiguous() const
		{ return (B_LENDIAN_TO_HOST_INT32(xp_desc_blocks)
			& APFS_NX_XP_NONCONTIGUOUS) == 0; }
	uint32	DataBlocks() const
		{ return B_LENDIAN_TO_HOST_INT32(xp_data_blocks)
			& ~APFS_NX_XP_NONCONTIGUOUS; }
	bool	DataContiguous() const
		{ return (B_LENDIAN_TO_HOST_INT32(xp_data_blocks)
			& APFS_NX_XP_NONCONTIGUOUS) == 0; }
	apfs_paddr_t DescriptorBase() const
		{ return B_LENDIAN_TO_HOST_INT64(xp_desc_base); }
	apfs_paddr_t DataBase() const
		{ return B_LENDIAN_TO_HOST_INT64(xp_data_base); }
	uint32	DescriptorIndex() const
		{ return B_LENDIAN_TO_HOST_INT32(xp_desc_index); }
	uint32	DescriptorLength() const
		{ return B_LENDIAN_TO_HOST_INT32(xp_desc_len); }
	apfs_oid_t ObjectMap() const { return B_LENDIAN_TO_HOST_INT64(omap_oid); }
	uint32	MaxFileSystems() const
		{ return B_LENDIAN_TO_HOST_INT32(max_file_systems); }
	apfs_oid_t FileSystem(uint32 index) const
		{ return B_LENDIAN_TO_HOST_INT64(fs_oid[index]); }
	uint64	Flags() const { return B_LENDIAN_TO_HOST_INT64(flags); }
} _PACKED;


// "checkpoint_mapping_t"
struct apfs_checkpoint_mapping {
	uint32	type;
	uint32	subtype;
	uint32	size;
	uint32	pad;
	uint64	fs_oid;
	uint64	oid;
	uint64	paddr;

	uint32	Type() const
		{ return B_LENDIAN_TO_HOST_INT32(type) & APFS_OBJECT_TYPE_MASK; }
	uint32	Size() const { return B_LENDIAN_TO_HOST_INT32(size); }
	apfs_oid_t ObjectID() const { return B_LENDIAN_TO_HOST_INT64(oid); }
	apfs_paddr_t Address() const { return B_LENDIAN_TO_HOST_INT64(paddr); }
} _PACKED;


// "checkpoint_map_phys_t" and "Checkpoint Flags"
#define APFS_CHECKPOINT_MAP_LAST		0x00000001

struct apfs_checkpoint_map_phys {
	apfs_obj_phys	o;
	uint32	flags;
	uint32	count;
	apfs_checkpoint_mapping	map[];

	uint32	Flags() const { return B_LENDIAN_TO_HOST_INT32(flags); }
	uint32	Count() const { return B_LENDIAN_TO_HOST_INT32(count); }
} _PACKED;


// #pragma mark - Object maps ("Object Maps" chapter)


// "Object Map Value Flags"
#define APFS_OMAP_VAL_DELETED			0x00000001
#define APFS_OMAP_VAL_SAVED				0x00000002
#define APFS_OMAP_VAL_ENCRYPTED			0x00000004
#define APFS_OMAP_VAL_NOHEADER			0x00000008

// "omap_phys_t"
struct apfs_omap_phys {
	apfs_obj_phys	o;
	uint32	flags;
	uint32	snap_count;
	uint32	tree_type;
	uint32	snapshot_tree_type;
	uint64	tree_oid;
	uint64	snapshot_tree_oid;
	uint64	most_recent_snap;
	uint64	pending_revert_min;
	uint64	pending_revert_max;

	uint32	TreeType() const { return B_LENDIAN_TO_HOST_INT32(tree_type); }
	apfs_oid_t Tree() const { return B_LENDIAN_TO_HOST_INT64(tree_oid); }
} _PACKED;

// "omap_key_t"
struct apfs_omap_key {
	uint64	oid;
	uint64	xid;

	apfs_oid_t ObjectID() const { return B_LENDIAN_TO_HOST_INT64(oid); }
	apfs_xid_t TransactionID() const { return B_LENDIAN_TO_HOST_INT64(xid); }
} _PACKED;

// "omap_val_t"
struct apfs_omap_val {
	uint32	flags;
	uint32	size;
	int64	paddr;

	uint32	Flags() const { return B_LENDIAN_TO_HOST_INT32(flags); }
	uint32	Size() const { return B_LENDIAN_TO_HOST_INT32(size); }
	apfs_paddr_t Address() const { return B_LENDIAN_TO_HOST_INT64(paddr); }
} _PACKED;


// #pragma mark - B-trees ("B-Trees" chapter)


// "B-Tree Flags"
#define APFS_BTREE_UINT64_KEYS			0x00000001
#define APFS_BTREE_SEQUENTIAL_INSERT	0x00000002
#define APFS_BTREE_ALLOW_GHOSTS			0x00000004
#define APFS_BTREE_EPHEMERAL			0x00000008
#define APFS_BTREE_PHYSICAL				0x00000010
#define APFS_BTREE_NONPERSISTENT		0x00000020
#define APFS_BTREE_KV_NONALIGNED		0x00000040
#define APFS_BTREE_HASHED				0x00000080
#define APFS_BTREE_NOHEADER				0x00000100

// "B-Tree Node Flags"
#define APFS_BTNODE_ROOT				0x0001
#define APFS_BTNODE_LEAF				0x0002
#define APFS_BTNODE_FIXED_KV_SIZE		0x0004
#define APFS_BTNODE_HASHED				0x0008
#define APFS_BTNODE_NOHEADER			0x0010

// "nloc_t"
#define APFS_BTOFF_INVALID				0xffff

struct apfs_nloc {
	uint16	off;
	uint16	len;

	uint16	Offset() const { return B_LENDIAN_TO_HOST_INT16(off); }
	uint16	Length() const { return B_LENDIAN_TO_HOST_INT16(len); }
} _PACKED;

// "kvloc_t": table of contents entry, variable-size keys or values.
struct apfs_kvloc {
	apfs_nloc	k;
	apfs_nloc	v;
} _PACKED;

// "kvoff_t": table of contents entry, fixed-size keys and values.
struct apfs_kvoff {
	uint16	k;
	uint16	v;

	uint16	KeyOffset() const { return B_LENDIAN_TO_HOST_INT16(k); }
	uint16	ValueOffset() const { return B_LENDIAN_TO_HOST_INT16(v); }
} _PACKED;

// "btree_node_phys_t"
struct apfs_btree_node_phys {
	apfs_obj_phys	o;
	uint16	flags;
	uint16	level;
	uint32	nkeys;
	apfs_nloc	table_space;
	apfs_nloc	free_space;
	apfs_nloc	key_free_list;
	apfs_nloc	val_free_list;
	uint64	data[];

	uint16	Flags() const { return B_LENDIAN_TO_HOST_INT16(flags); }
	uint16	Level() const { return B_LENDIAN_TO_HOST_INT16(level); }
	uint32	KeyCount() const { return B_LENDIAN_TO_HOST_INT32(nkeys); }
	bool	IsRoot() const { return (Flags() & APFS_BTNODE_ROOT) != 0; }
	bool	IsLeaf() const { return (Flags() & APFS_BTNODE_LEAF) != 0; }
	bool	IsFixedSize() const
		{ return (Flags() & APFS_BTNODE_FIXED_KV_SIZE) != 0; }
} _PACKED;

// "btree_info_fixed_t"
struct apfs_btree_info_fixed {
	uint32	flags;
	uint32	node_size;
	uint32	key_size;
	uint32	val_size;

	uint32	Flags() const { return B_LENDIAN_TO_HOST_INT32(flags); }
	uint32	NodeSize() const { return B_LENDIAN_TO_HOST_INT32(node_size); }
	uint32	KeySize() const { return B_LENDIAN_TO_HOST_INT32(key_size); }
	uint32	ValueSize() const { return B_LENDIAN_TO_HOST_INT32(val_size); }
} _PACKED;

// "btree_info_t": at the end of a root node.
struct apfs_btree_info {
	apfs_btree_info_fixed	fixed;
	uint32	longest_key;
	uint32	longest_val;
	uint64	key_count;
	uint64	node_count;

	uint64	KeyCount() const { return B_LENDIAN_TO_HOST_INT64(key_count); }
	uint64	NodeCount() const { return B_LENDIAN_TO_HOST_INT64(node_count); }
} _PACKED;


// #pragma mark - Volumes ("Volumes" chapter)


#define APFS_MAGIC						0x42535041	// 'APSB' on disk
#define APFS_MAX_HIST					8
#define APFS_VOLNAME_LEN				256
#define APFS_MODIFIED_NAMELEN			32

// "Inode Numbers"
#define APFS_ROOT_DIR_PARENT			1
#define APFS_ROOT_DIR_INO_NUM			2
#define APFS_PRIV_DIR_INO_NUM			3
#define APFS_SNAP_DIR_INO_NUM			6
#define APFS_MIN_USER_INO_NUM			16

// "Volume Flags"
#define APFS_FS_UNENCRYPTED				0x00000001ULL
#define APFS_FS_ONEKEY					0x00000008ULL

// "Volume Roles"
#define APFS_VOLUME_ENUM_SHIFT			6
#define APFS_VOL_ROLE_NONE				0x0000
#define APFS_VOL_ROLE_SYSTEM			0x0001
#define APFS_VOL_ROLE_USER				0x0002
#define APFS_VOL_ROLE_RECOVERY			0x0004
#define APFS_VOL_ROLE_VM				0x0008
#define APFS_VOL_ROLE_PREBOOT			0x0010
#define APFS_VOL_ROLE_INSTALLER			0x0020
#define APFS_VOL_ROLE_DATA				(1 << APFS_VOLUME_ENUM_SHIFT)
#define APFS_VOL_ROLE_BASEBAND			(2 << APFS_VOLUME_ENUM_SHIFT)
#define APFS_VOL_ROLE_UPDATE			(3 << APFS_VOLUME_ENUM_SHIFT)
#define APFS_VOL_ROLE_XART				(4 << APFS_VOLUME_ENUM_SHIFT)
#define APFS_VOL_ROLE_HARDWARE			(5 << APFS_VOLUME_ENUM_SHIFT)
#define APFS_VOL_ROLE_BACKUP			(6 << APFS_VOLUME_ENUM_SHIFT)
#define APFS_VOL_ROLE_SIDECAR			(7 << APFS_VOLUME_ENUM_SHIFT)
#define APFS_VOL_ROLE_ENTERPRISE		(9 << APFS_VOLUME_ENUM_SHIFT)
#define APFS_VOL_ROLE_PRELOGIN			(11 << APFS_VOLUME_ENUM_SHIFT)

// "Incompatible Volume Feature Flags"
#define APFS_INCOMPAT_CASE_INSENSITIVE			0x00000001ULL
#define APFS_INCOMPAT_DATALESS_SNAPS			0x00000002ULL
#define APFS_INCOMPAT_ENC_ROLLED				0x00000004ULL
#define APFS_INCOMPAT_NORMALIZATION_INSENSITIVE	0x00000008ULL
#define APFS_INCOMPAT_INCOMPLETE_RESTORE		0x00000010ULL
#define APFS_INCOMPAT_SEALED_VOLUME				0x00000020ULL

// "wrapped_meta_crypto_state_t" (20 bytes, packed)
struct apfs_wrapped_meta_crypto_state {
	uint16	major_version;
	uint16	minor_version;
	uint32	cpflags;
	uint32	persistent_class;
	uint32	key_os_version;
	uint16	key_revision;
	uint16	unused;
} _PACKED;

// "apfs_modified_by_t"
struct apfs_modified_by {
	uint8	id[APFS_MODIFIED_NAMELEN];
	uint64	timestamp;
	uint64	last_xid;
} _PACKED;

// "apfs_superblock_t": volume superblock.
struct apfs_superblock {
	apfs_obj_phys	o;
	uint32	magic;
	uint32	fs_index;
	uint64	features;
	uint64	readonly_compatible_features;
	uint64	incompatible_features;
	uint64	unmount_time;
	uint64	fs_reserve_block_count;
	uint64	fs_quota_block_count;
	uint64	fs_alloc_count;
	apfs_wrapped_meta_crypto_state	meta_crypto;
	uint32	root_tree_type;
	uint32	extentref_tree_type;
	uint32	snap_meta_tree_type;
	uint64	omap_oid;
	uint64	root_tree_oid;
	uint64	extentref_tree_oid;
	uint64	snap_meta_tree_oid;
	uint64	revert_to_xid;
	uint64	revert_to_sblock_oid;
	uint64	next_obj_id;
	uint64	num_files;
	uint64	num_directories;
	uint64	num_symlinks;
	uint64	num_other_fsobjects;
	uint64	num_snapshots;
	uint64	total_blocks_alloced;
	uint64	total_blocks_freed;
	uint8	vol_uuid[16];
	uint64	last_mod_time;
	uint64	fs_flags;
	apfs_modified_by	formatted_by;
	apfs_modified_by	modified_by[APFS_MAX_HIST];
	uint8	volname[APFS_VOLNAME_LEN];
	uint32	next_doc_id;
	uint16	role;
	uint16	reserved;
	uint64	root_to_xid;
	uint64	er_state_oid;
	uint64	cloneinfo_id_epoch;
	uint64	cloneinfo_xid;
	uint64	snap_meta_ext_oid;
	uint8	volume_group_id[16];
	uint64	integrity_meta_oid;
	uint64	fext_tree_oid;
	uint32	fext_tree_type;
	uint32	reserved_type;
	uint64	reserved_oid;

	uint32	Magic() const { return B_LENDIAN_TO_HOST_INT32(magic); }
	uint32	Index() const { return B_LENDIAN_TO_HOST_INT32(fs_index); }
	uint64	IncompatibleFeatures() const
		{ return B_LENDIAN_TO_HOST_INT64(incompatible_features); }
	uint64	AllocatedBlocks() const
		{ return B_LENDIAN_TO_HOST_INT64(fs_alloc_count); }
	uint32	RootTreeType() const
		{ return B_LENDIAN_TO_HOST_INT32(root_tree_type); }
	apfs_oid_t ObjectMap() const { return B_LENDIAN_TO_HOST_INT64(omap_oid); }
	apfs_oid_t RootTree() const
		{ return B_LENDIAN_TO_HOST_INT64(root_tree_oid); }
	uint64	FileCount() const { return B_LENDIAN_TO_HOST_INT64(num_files); }
	uint64	DirectoryCount() const
		{ return B_LENDIAN_TO_HOST_INT64(num_directories); }
	uint64	SymlinkCount() const
		{ return B_LENDIAN_TO_HOST_INT64(num_symlinks); }
	uint64	SnapshotCount() const
		{ return B_LENDIAN_TO_HOST_INT64(num_snapshots); }
	uint64	LastModificationTime() const
		{ return B_LENDIAN_TO_HOST_INT64(last_mod_time); }
	uint64	Flags() const { return B_LENDIAN_TO_HOST_INT64(fs_flags); }
	uint16	Role() const { return B_LENDIAN_TO_HOST_INT16(role); }
	bool	IsEncrypted() const
		{ return (Flags() & APFS_FS_UNENCRYPTED) == 0; }
} _PACKED;


// #pragma mark - File-system records ("File-System Objects", "File-System
// Constants", "Data Streams", "Extended Fields")


// "j_key_t"
#define APFS_OBJ_ID_MASK				0x0fffffffffffffffULL
#define APFS_OBJ_TYPE_MASK				0xf000000000000000ULL
#define APFS_OBJ_TYPE_SHIFT				60

// "j_obj_types"
#define APFS_TYPE_ANY					0
#define APFS_TYPE_SNAP_METADATA			1
#define APFS_TYPE_EXTENT				2
#define APFS_TYPE_INODE					3
#define APFS_TYPE_XATTR					4
#define APFS_TYPE_SIBLING_LINK			5
#define APFS_TYPE_DSTREAM_ID			6
#define APFS_TYPE_CRYPTO_STATE			7
#define APFS_TYPE_FILE_EXTENT			8
#define APFS_TYPE_DIR_REC				9
#define APFS_TYPE_DIR_STATS				10
#define APFS_TYPE_SNAP_NAME				11
#define APFS_TYPE_SIBLING_MAP			12
#define APFS_TYPE_FILE_INFO				13

struct apfs_j_key {
	uint64	obj_id_and_type;

	uint64	ObjectID() const
		{ return B_LENDIAN_TO_HOST_INT64(obj_id_and_type) & APFS_OBJ_ID_MASK; }
	uint8	Type() const
		{ return (B_LENDIAN_TO_HOST_INT64(obj_id_and_type) & APFS_OBJ_TYPE_MASK)
			>> APFS_OBJ_TYPE_SHIFT; }
	void	Set(uint64 id, uint8 type)
		{ obj_id_and_type = B_HOST_TO_LENDIAN_INT64((id & APFS_OBJ_ID_MASK)
			| ((uint64)type << APFS_OBJ_TYPE_SHIFT)); }
} _PACKED;

// "j_inode_flags" (internal_flags)
#define APFS_INODE_IS_APFS_PRIVATE		0x00000001
#define APFS_INODE_HAS_SECURITY_EA		0x00000040
#define APFS_INODE_HAS_FINDER_INFO		0x00000100
#define APFS_INODE_IS_SPARSE			0x00000200
#define APFS_INODE_HAS_RSRC_FORK		0x00004000
#define APFS_INODE_HAS_UNCOMPRESSED_SIZE	0x00040000

// BSD flag of a file whose data is compressed (decmpfs). Not in the APFS
// reference; it is the public UF_COMPRESSED value of the macOS
// <sys/stat.h>. Reading such files comes with milestone 4.
#define APFS_BSD_UF_COMPRESSED			0x00000020

// "File Modes" (same values as POSIX)
#define APFS_S_IFMT						0170000
#define APFS_S_IFDIR					0040000
#define APFS_S_IFREG					0100000
#define APFS_S_IFLNK					0120000

// "j_inode_val_t"
struct apfs_j_inode_val {
	uint64	parent_id;
	uint64	private_id;
	uint64	create_time;
	uint64	mod_time;
	uint64	change_time;
	uint64	access_time;
	uint64	internal_flags;
	int32	nchildren_or_nlink;
	uint32	default_protection_class;
	uint32	write_generation_counter;
	uint32	bsd_flags;
	uint32	owner;
	uint32	group;
	uint16	mode;
	uint16	pad1;
	uint64	uncompressed_size;
	uint8	xfields[];
} _PACKED;

// "j_drec_hashed_key_t" (directory entries in hashed trees; every volume
// format since macOS 10.13 uses it)
#define APFS_J_DREC_LEN_MASK			0x000003ff
#define APFS_J_DREC_HASH_MASK			0xfffffc00
	// The reference prints 0xfffff400, which drops bit 10; a 22-bit hash
	// shifted left by 10 needs 0xfffffc00 (J_DREC_HASH_SHIFT is 10).
#define APFS_J_DREC_HASH_SHIFT			10

struct apfs_j_drec_hashed_key {
	apfs_j_key	hdr;
	uint32	name_len_and_hash;
	uint8	name[];

	uint16	NameLength() const
		{ return B_LENDIAN_TO_HOST_INT32(name_len_and_hash)
			& APFS_J_DREC_LEN_MASK; }
} _PACKED;

// "j_drec_val_t" and "Directory Entry File Types"
#define APFS_DREC_TYPE_MASK				0x000f
#define APFS_DT_UNKNOWN					0
#define APFS_DT_FIFO					1
#define APFS_DT_CHR						2
#define APFS_DT_DIR						4
#define APFS_DT_BLK						6
#define APFS_DT_REG						8
#define APFS_DT_LNK						10
#define APFS_DT_SOCK					12

struct apfs_j_drec_val {
	uint64	file_id;
	uint64	date_added;
	uint16	flags;
	uint8	xfields[];

	uint64	FileID() const { return B_LENDIAN_TO_HOST_INT64(file_id); }
	uint16	FileType() const
		{ return B_LENDIAN_TO_HOST_INT16(flags) & APFS_DREC_TYPE_MASK; }
} _PACKED;

// "j_xattr_key_t", "j_xattr_val_t", "j_xattr_flags"
#define APFS_XATTR_DATA_STREAM			0x0001
#define APFS_XATTR_DATA_EMBEDDED		0x0002
#define APFS_XATTR_FILE_SYSTEM_OWNED	0x0004
#define APFS_XATTR_MAX_EMBEDDED_SIZE	3804
#define APFS_SYMLINK_EA_NAME			"com.apple.fs.symlink"

struct apfs_j_xattr_key {
	apfs_j_key	hdr;
	uint16	name_len;
	uint8	name[];

	uint16	NameLength() const { return B_LENDIAN_TO_HOST_INT16(name_len); }
} _PACKED;

struct apfs_j_xattr_val {
	uint16	flags;
	uint16	xdata_len;
	uint8	xdata[];

	uint16	Flags() const { return B_LENDIAN_TO_HOST_INT16(flags); }
	uint16	DataLength() const { return B_LENDIAN_TO_HOST_INT16(xdata_len); }
} _PACKED;

// "j_file_extent_key_t", "j_file_extent_val_t"
#define APFS_J_FILE_EXTENT_LEN_MASK		0x00ffffffffffffffULL

struct apfs_j_file_extent_key {
	apfs_j_key	hdr;
	uint64	logical_addr;

	uint64	LogicalAddress() const
		{ return B_LENDIAN_TO_HOST_INT64(logical_addr); }
} _PACKED;

struct apfs_j_file_extent_val {
	uint64	len_and_flags;
	uint64	phys_block_num;
	uint64	crypto_id;

	uint64	Length() const
		{ return B_LENDIAN_TO_HOST_INT64(len_and_flags)
			& APFS_J_FILE_EXTENT_LEN_MASK; }
	uint64	PhysicalBlock() const
		{ return B_LENDIAN_TO_HOST_INT64(phys_block_num); }
} _PACKED;

// "j_dstream_t"
struct apfs_j_dstream {
	uint64	size;
	uint64	alloced_size;
	uint64	default_crypto_id;
	uint64	total_bytes_written;
	uint64	total_bytes_read;
} _PACKED;

// "j_xattr_dstream_t": the value of a stream-backed extended attribute.
struct apfs_j_xattr_dstream {
	uint64	xattr_obj_id;
	apfs_j_dstream	dstream;
} _PACKED;

// "xf_blob_t", "x_field_t", "Extended-Field Types"
#define APFS_INO_EXT_TYPE_SNAP_XID		1
#define APFS_INO_EXT_TYPE_DELTA_TREE_OID	2
#define APFS_INO_EXT_TYPE_DOCUMENT_ID	3
#define APFS_INO_EXT_TYPE_NAME			4
#define APFS_INO_EXT_TYPE_PREV_FSIZE	5
#define APFS_INO_EXT_TYPE_FINDER_INFO	7
#define APFS_INO_EXT_TYPE_DSTREAM		8
#define APFS_INO_EXT_TYPE_DIR_STATS_KEY	10
#define APFS_INO_EXT_TYPE_FS_UUID		11
#define APFS_INO_EXT_TYPE_SPARSE_BYTES	13
#define APFS_INO_EXT_TYPE_RDEV			14

struct apfs_xf_blob {
	uint16	xf_num_exts;
	uint16	xf_used_data;
	uint8	xf_data[];
} _PACKED;

struct apfs_x_field {
	uint8	x_type;
	uint8	x_flags;
	uint16	x_size;
} _PACKED;


// Offsets the reference and real images agree on; checked at compile time.
static_assert(sizeof(apfs_obj_phys) == 32, "obj_phys_t size");
static_assert(sizeof(apfs_checkpoint_mapping) == 40,
	"checkpoint_mapping_t size");
static_assert(sizeof(apfs_btree_node_phys) == 56, "btree_node_phys_t size");
static_assert(sizeof(apfs_btree_info) == 40, "btree_info_t size");
static_assert(sizeof(apfs_wrapped_meta_crypto_state) == 20,
	"wrapped_meta_crypto_state_t size");
static_assert(offsetof(apfs_nx_superblock, omap_oid) == 0xa0,
	"nx_omap_oid offset");
static_assert(offsetof(apfs_nx_superblock, fs_oid) == 0xb8,
	"nx_fs_oid offset");
static_assert(offsetof(apfs_superblock, omap_oid) == 0x80,
	"apfs_omap_oid offset");
static_assert(offsetof(apfs_superblock, volname) == 0x2c0,
	"apfs_volname offset");
static_assert(sizeof(apfs_j_inode_val) == 92, "j_inode_val_t size");
static_assert(sizeof(apfs_j_drec_val) == 18, "j_drec_val_t size");
static_assert(sizeof(apfs_j_file_extent_key) == 16,
	"j_file_extent_key_t size");
static_assert(sizeof(apfs_j_file_extent_val) == 24,
	"j_file_extent_val_t size");
static_assert(sizeof(apfs_j_dstream) == 40, "j_dstream_t size");


#endif	// APFS_H
