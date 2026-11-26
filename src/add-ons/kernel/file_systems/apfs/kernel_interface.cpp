/*
 * Copyright 2026, RenkuOS contributors.
 * Distributed under the terms of the MIT License.
 *
 * File system module hooks for APFS (read-only).
 *
 * Milestone 1: recognizes APFS containers, mounts one volume after reading
 * the container checkpoint, object maps and the volume's file-system tree
 * root, and lists every volume in the syslog.
 * Milestone 2: directories, stat, symlinks and reading uncompressed files.
 * Milestone 3: files go through the file cache and file map (read_pages,
 * io, get_file_map), so they can be memory-mapped and executed.
 * Milestone 4: compressed files (decmpfs) go through the file cache too;
 * read_pages and io decompress their pages.
 */


#include "system_dependencies.h"

#include <file_systems/fs_ops_support.h>

#include "Decmpfs.h"
#include "Inode.h"
#include "Volume.h"


#define APFS_IO_SIZE	65536


struct identify_cookie {
	uint64		block_count;
	uint32		block_size;
	char		name[APFS_VOLNAME_LEN];
};


struct dir_cookie {
	uint32				position;	// 0: ".", 1: "..", 2: entries
	DirectoryIterator*	iterator;
};


// #pragma mark - Scanning


static float
apfs_identify_partition(int fd, partition_data* partition, void** _cookie)
{
	apfs_nx_superblock superBlock;
	if (Container::Identify(fd, &superBlock) != B_OK)
		return -1;

	identify_cookie* cookie = new(std::nothrow) identify_cookie;
	if (cookie == NULL)
		return -1;
	cookie->block_count = superBlock.BlockCount();
	cookie->block_size = superBlock.BlockSize();
	strlcpy(cookie->name, "APFS container", sizeof(cookie->name));

	// Name the partition after its first volume, if the container can be
	// read; recognizing it does not depend on that.
	Container container;
	status_t status = container.Open(fd, partition->size, false);
	if (status == B_OK) {
		uint32 count = container.CountVolumes();
		for (uint32 i = 0; i < count; i++) {
			apfs_volume_info info;
			if (container.GetVolumeInfo(i, &info) == B_OK) {
				strlcpy(cookie->name, info.name, sizeof(cookie->name));
				break;
			}
		}
	}

	// Leave a trace in the syslog: recognizing a partition is otherwise
	// silent, and it is the first thing to check when a mount fails.
	INFORM("found a container on partition %" B_PRId32 ", %" B_PRIu64
		" blocks of %" B_PRIu32 " bytes, first volume \"%s\"%s\n",
		partition->id, cookie->block_count, cookie->block_size, cookie->name,
		status == B_OK ? "" : " (container could not be read)");

	*_cookie = cookie;
	return 0.8f;
}


static status_t
apfs_scan_partition(int fd, partition_data* partition, void* _cookie)
{
	identify_cookie* cookie = (identify_cookie*)_cookie;

	partition->status = B_PARTITION_VALID;
	partition->flags |= B_PARTITION_FILE_SYSTEM | B_PARTITION_READ_ONLY;
	partition->content_size = cookie->block_count * cookie->block_size;
	partition->block_size = cookie->block_size;
	partition->content_name = strdup(cookie->name);
	if (partition->content_name == NULL)
		return B_NO_MEMORY;

	return B_OK;
}


static void
apfs_free_identify_partition_cookie(partition_data* partition, void* _cookie)
{
	delete (identify_cookie*)_cookie;
}


// #pragma mark - Volume


static status_t
apfs_mount(fs_volume* _volume, const char* device, uint32 flags,
	const char* args, ino_t* _rootID)
{
	Volume* volume = new(std::nothrow) Volume(_volume);
	if (volume == NULL)
		return B_NO_MEMORY;

	_volume->private_volume = volume;
	_volume->ops = &gAPFSVolumeOps;

	status_t status = volume->Mount(device, flags, args);
	if (status != B_OK) {
		delete volume;
		_volume->private_volume = NULL;
		return status;
	}

	Inode* root = new(std::nothrow) Inode(volume, volume->RootID());
	if (root == NULL)
		status = B_NO_MEMORY;
	else
		status = root->Init();
	if (status == B_OK && !root->IsDirectory())
		status = B_BAD_DATA;
	if (status != B_OK) {
		ERROR("volume \"%s\": cannot read the root directory: %s\n",
			volume->Name(), strerror(status));
		delete root;
		delete volume;
		_volume->private_volume = NULL;
		return status;
	}

	status = publish_vnode(_volume, root->ID(), root, &gAPFSVnodeOps,
		root->Mode() & S_IFMT, 0);
	if (status != B_OK) {
		delete root;
		delete volume;
		_volume->private_volume = NULL;
		return status;
	}

	*_rootID = root->ID();
	return B_OK;
}


static status_t
apfs_unmount(fs_volume* _volume)
{
	Volume* volume = (Volume*)_volume->private_volume;

	// Give back the reference publish_vnode() gave the root at mount time;
	// the VFS frees the root vnode through it (other mount vnodes are gone
	// by now). Without this the kernel asserts that the mount still has
	// vnodes ("vnodes.IsEmpty()" in fs_mount's destructor).
	put_vnode(_volume, volume->RootID());

	status_t status = volume->Unmount();
	delete volume;
	return status;
}


static status_t
apfs_read_fs_info(fs_volume* _volume, struct fs_info* info)
{
	Volume* volume = (Volume*)_volume->private_volume;
	Container& container = volume->GetContainer();

	info->flags = B_FS_IS_READONLY | B_FS_IS_PERSISTENT | B_FS_HAS_ATTR;
	info->io_size = APFS_IO_SIZE;
	info->block_size = container.BlockSize();
	// Volumes share the container's space; report the container size and
	// treat everything not allocated to this volume as unknown (free 0) for
	// now. The space manager is read in a later milestone.
	info->total_blocks = container.BlockCount();
	info->free_blocks = 0;

	strlcpy(info->volume_name, volume->Name(), sizeof(info->volume_name));
	strlcpy(info->fsh_name, "apfs", sizeof(info->fsh_name));
	return B_OK;
}


// #pragma mark - File map and paging


static status_t
apfs_get_file_map(fs_volume* _volume, fs_vnode* _node, off_t offset,
	size_t size, struct file_io_vec* vecs, size_t* _count)
{
	Inode* inode = (Inode*)_node->private_node;
	size_t index = 0;
	size_t max = *_count;

	if (offset < 0)
		return B_BAD_VALUE;

	while (size > 0 && offset < inode->Size()) {
		off_t diskOffset;
		off_t length;
		status_t status = inode->FindExtent(offset, &diskOffset, &length);
		if (status != B_OK)
			return status;
		if (length <= 0)
			return B_BAD_DATA;

		// Join runs that continue each other on disk (or holes).
		if (index > 0 && ((diskOffset == -1 && vecs[index - 1].offset == -1)
				|| (diskOffset != -1 && vecs[index - 1].offset != -1
					&& vecs[index - 1].offset + vecs[index - 1].length
						== diskOffset))) {
			vecs[index - 1].length += length;
		} else {
			if (index >= max) {
				*_count = index;
				return B_BUFFER_OVERFLOW;
			}
			vecs[index].offset = diskOffset;
			vecs[index].length = length;
			index++;
		}

		offset += length;
		size = (off_t)size > length ? size - length : 0;
	}

	*_count = index;
	return B_OK;
}


static status_t
iterative_io_get_vecs_hook(void* cookie, io_request* request, off_t offset,
	size_t size, struct file_io_vec* vecs, size_t* _count)
{
	Inode* inode = (Inode*)cookie;
	return file_map_translate(inode->FileMap(), offset, size, vecs, _count,
		inode->GetVolume()->BlockSize());
}


static status_t
iterative_io_finished_hook(void* cookie, io_request* request, status_t status,
	bool partialTransfer, size_t bytesTransferred)
{
	// Nothing to unlock: the volume never changes while mounted.
	return B_OK;
}


/*!	Fills pages of a compressed file: decompresses [pos, pos + *_numBytes)
	into the given kernel buffers.
*/
static status_t
read_compressed_pages(Inode* inode, off_t pos, const iovec* vecs,
	size_t count, size_t* _numBytes)
{
	size_t wanted = *_numBytes;
	size_t done = 0;
	for (size_t i = 0; i < count && done < wanted; i++) {
		size_t length = vecs[i].iov_len;
		if (length > wanted - done)
			length = wanted - done;

		size_t read = length;
		status_t status = inode->ReadCompressed(pos + done,
			(uint8*)vecs[i].iov_base, &read);
		if (status != B_OK) {
			*_numBytes = done;
			return done > 0 ? B_OK : status;
		}
		// Past the end of the file, pages are filled with zeros.
		if (read < length)
			memset((uint8*)vecs[i].iov_base + read, 0, length - read);
		done += length;
	}
	*_numBytes = done;
	return B_OK;
}


#ifndef FS_SHELL
static status_t
do_compressed_io(Inode* inode, io_request* request)
{
	off_t offset = io_request_offset(request);
	off_t length = io_request_length(request);

	uint8* buffer = (uint8*)malloc(APFS_IO_SIZE);
	if (buffer == NULL) {
		notify_io_request(request, B_NO_MEMORY);
		return B_NO_MEMORY;
	}

	status_t status = B_OK;
	off_t done = 0;
	while (done < length) {
		size_t chunk = length - done > APFS_IO_SIZE
			? APFS_IO_SIZE : length - done;
		size_t read = chunk;
		status = inode->ReadCompressed(offset + done, buffer, &read);
		if (status != B_OK)
			break;
		if (read < chunk)
			memset(buffer + read, 0, chunk - read);
		status = write_to_io_request(request, buffer, chunk);
		if (status != B_OK)
			break;
		done += chunk;
	}

	free(buffer);
	notify_io_request(request, status);
	return status;
}
#endif


static bool
apfs_can_page(fs_volume* _volume, fs_vnode* _node, void* _cookie)
{
	Inode* inode = (Inode*)_node->private_node;
	return inode->FileCache() != NULL;
}


static status_t
apfs_read_pages(fs_volume* _volume, fs_vnode* _node, void* _cookie,
	off_t pos, const iovec* vecs, size_t count, size_t* _numBytes)
{
	Volume* volume = (Volume*)_volume->private_volume;
	Inode* inode = (Inode*)_node->private_node;

	if (inode->Compression() != NULL)
		return read_compressed_pages(inode, pos, vecs, count, _numBytes);
	if (inode->FileMap() == NULL)
		return B_BAD_VALUE;

	uint32 vecIndex = 0;
	size_t vecOffset = 0;
	size_t bytesLeft = *_numBytes;
	status_t status;

	while (true) {
		file_io_vec fileVecs[8];
		size_t fileVecCount = 8;

		status = file_map_translate(inode->FileMap(), pos, bytesLeft,
			fileVecs, &fileVecCount, 0);
		if (status != B_OK && status != B_BUFFER_OVERFLOW)
			break;

		bool bufferOverflow = status == B_BUFFER_OVERFLOW;

		size_t bytes = bytesLeft;
		status = read_file_io_vec_pages(volume->Device(), fileVecs,
			fileVecCount, vecs, count, &vecIndex, &vecOffset, &bytes);
		if (status != B_OK || !bufferOverflow)
			break;

		pos += bytes;
		bytesLeft -= bytes;
	}

	return status;
}


static status_t
apfs_write_pages(fs_volume* _volume, fs_vnode* _node, void* _cookie,
	off_t pos, const iovec* vecs, size_t count, size_t* _numBytes)
{
	*_numBytes = 0;
	return B_READ_ONLY_DEVICE;
}


static status_t
apfs_io(fs_volume* _volume, fs_vnode* _node, void* _cookie,
	io_request* request)
{
	Volume* volume = (Volume*)_volume->private_volume;
	Inode* inode = (Inode*)_node->private_node;

#ifndef FS_SHELL
	if (io_request_is_write(request)) {
		notify_io_request(request, B_READ_ONLY_DEVICE);
		return B_READ_ONLY_DEVICE;
	}
#endif

#ifndef FS_SHELL
	if (inode->Compression() != NULL)
		return do_compressed_io(inode, request);
#endif

	if (inode->FileMap() == NULL) {
#ifndef FS_SHELL
		notify_io_request(request, B_BAD_VALUE);
#endif
		return B_BAD_VALUE;
	}

	return do_iterative_fd_io(volume->Device(), request,
		iterative_io_get_vecs_hook, iterative_io_finished_hook, inode);
}


// #pragma mark - Nodes


static status_t
apfs_get_vnode(fs_volume* _volume, ino_t id, fs_vnode* _node, int* _type,
	uint32* _flags, bool reenter)
{
	Volume* volume = (Volume*)_volume->private_volume;

	Inode* inode = new(std::nothrow) Inode(volume, id);
	if (inode == NULL)
		return B_NO_MEMORY;

	status_t status = inode->Init();
	if (status != B_OK) {
		delete inode;
		return status;
	}

	_node->private_node = inode;
	_node->ops = &gAPFSVnodeOps;
	*_type = inode->Mode() & S_IFMT;
	*_flags = 0;
	return B_OK;
}


static status_t
apfs_put_vnode(fs_volume* _volume, fs_vnode* _node, bool reenter)
{
	delete (Inode*)_node->private_node;
	return B_OK;
}


static status_t
apfs_lookup(fs_volume* _volume, fs_vnode* _directory, const char* name,
	ino_t* _vnodeID)
{
	Volume* volume = (Volume*)_volume->private_volume;
	Inode* directory = (Inode*)_directory->private_node;

	if (!directory->IsDirectory())
		return B_NOT_A_DIRECTORY;

	status_t status = directory->CheckPermissions(X_OK);
	if (status != B_OK)
		return status;

	ino_t id;
	if (strcmp(name, ".") == 0)
		id = directory->ID();
	else if (strcmp(name, "..") == 0) {
		// The root's parent (ROOT_DIR_PARENT) is not a real inode.
		id = directory->ID() == volume->RootID()
			? volume->RootID() : directory->ParentID();
	} else {
		DirectoryIterator iterator(directory);
		status = iterator.InitCheck();
		if (status == B_OK)
			status = iterator.Lookup(name, &id);
		if (status == B_ENTRY_NOT_FOUND)
			entry_cache_add_missing(volume->ID(), directory->ID(), name);
		if (status != B_OK)
			return status;
	}

	*_vnodeID = id;
	if (strcmp(name, ".") != 0 && strcmp(name, "..") != 0)
		entry_cache_add(volume->ID(), directory->ID(), name, id);

	void* node;
	return get_vnode(volume->FSVolume(), id, &node);
}


static status_t
apfs_access(fs_volume* _volume, fs_vnode* _node, int accessMode)
{
	Inode* inode = (Inode*)_node->private_node;
	return inode->CheckPermissions(accessMode);
}


static status_t
apfs_read_stat(fs_volume* _volume, fs_vnode* _node, struct stat* stat)
{
	Volume* volume = (Volume*)_volume->private_volume;
	Inode* inode = (Inode*)_node->private_node;

	memset(stat, 0, sizeof(*stat));
	stat->st_dev = volume->ID();
	stat->st_ino = inode->ID();
	stat->st_nlink = inode->LinkCount();
	stat->st_blksize = APFS_IO_SIZE;
	stat->st_uid = inode->UserID();
	stat->st_gid = inode->GroupID();
	stat->st_mode = inode->Mode();
	stat->st_size = inode->Size();
	stat->st_blocks = inode->AllocatedSize() / 512;

	inode->GetAccessTime(stat->st_atim);
	inode->GetModificationTime(stat->st_mtim);
	inode->GetChangeTime(stat->st_ctim);
	inode->GetCreationTime(stat->st_crtim);
	return B_OK;
}


// #pragma mark - Files and read-only refusals
//
// The VFS calls several hooks without checking they exist (open, close,
// free_cookie, read, write, write_stat, ...). Every hook a read-only volume
// can be asked for is filled in; writes answer B_READ_ONLY_DEVICE.


static status_t
apfs_open(fs_volume* _volume, fs_vnode* _node, int openMode, void** _cookie)
{
	// Tracker and BDirectory open directories this way too.
	if ((openMode & O_RWMASK) != O_RDONLY || (openMode & O_TRUNC) != 0)
		return B_READ_ONLY_DEVICE;

	Inode* inode = (Inode*)_node->private_node;
	status_t status = inode->CheckPermissions(open_mode_to_access(openMode));
	if (status != B_OK)
		return status;

	*_cookie = NULL;
	return B_OK;
}


static status_t
apfs_close(fs_volume* _volume, fs_vnode* _node, void* _cookie)
{
	return B_OK;
}


static status_t
apfs_free_cookie(fs_volume* _volume, fs_vnode* _node, void* _cookie)
{
	return B_OK;
}


static status_t
apfs_read(fs_volume* _volume, fs_vnode* _node, void* _cookie, off_t pos,
	void* buffer, size_t* _length)
{
	Inode* inode = (Inode*)_node->private_node;
	if (inode->IsSymlink()) {
		*_length = 0;
		return B_BAD_VALUE;
	}
	return inode->Read(pos, buffer, _length);
}


static status_t
apfs_write(fs_volume* _volume, fs_vnode* _node, void* _cookie, off_t pos,
	const void* buffer, size_t* _length)
{
	*_length = 0;
	return B_READ_ONLY_DEVICE;
}


static status_t
apfs_write_stat(fs_volume* _volume, fs_vnode* _node, const struct stat* stat,
	uint32 statMask)
{
	return B_READ_ONLY_DEVICE;
}


static status_t
apfs_fsync(fs_volume* _volume, fs_vnode* _node, bool dataOnly)
{
	return B_OK;
}


static status_t
apfs_ioctl(fs_volume* _volume, fs_vnode* _node, void* _cookie, uint32 op,
	void* buffer, size_t length)
{
	return B_DEV_INVALID_IOCTL;
}


static status_t
apfs_read_symlink(fs_volume* _volume, fs_vnode* _node, char* buffer,
	size_t* _bufferSize)
{
	Inode* inode = (Inode*)_node->private_node;
	return inode->ReadLink(buffer, _bufferSize);
}


static status_t
apfs_create_symlink(fs_volume* _volume, fs_vnode* _directory,
	const char* name, const char* path, int mode)
{
	return B_READ_ONLY_DEVICE;
}


static status_t
apfs_link(fs_volume* _volume, fs_vnode* _directory, const char* name,
	fs_vnode* _node)
{
	return B_READ_ONLY_DEVICE;
}


static status_t
apfs_unlink(fs_volume* _volume, fs_vnode* _directory, const char* name)
{
	return B_READ_ONLY_DEVICE;
}


static status_t
apfs_rename(fs_volume* _volume, fs_vnode* _fromDirectory,
	const char* fromName, fs_vnode* _toDirectory, const char* toName)
{
	return B_READ_ONLY_DEVICE;
}


static status_t
apfs_create(fs_volume* _volume, fs_vnode* _directory, const char* name,
	int openMode, int perms, void** _cookie, ino_t* _newVnodeID)
{
	return B_READ_ONLY_DEVICE;
}


static status_t
apfs_create_dir(fs_volume* _volume, fs_vnode* _directory, const char* name,
	int perms)
{
	return B_READ_ONLY_DEVICE;
}


static status_t
apfs_remove_dir(fs_volume* _volume, fs_vnode* _directory, const char* name)
{
	return B_READ_ONLY_DEVICE;
}


// #pragma mark - Attributes
//
// macOS extended attributes appear as attributes of type B_RAW_TYPE, with
// their macOS names. Attributes the file system owns are hidden.


static status_t
apfs_open_attr_dir(fs_volume* _volume, fs_vnode* _node, void** _cookie)
{
	Inode* inode = (Inode*)_node->private_node;
	AttributeIterator* iterator = new(std::nothrow) AttributeIterator(inode);
	if (iterator == NULL)
		return B_NO_MEMORY;

	status_t status = iterator->InitCheck();
	if (status != B_OK) {
		delete iterator;
		return status;
	}

	*_cookie = iterator;
	return B_OK;
}


static status_t
apfs_close_attr_dir(fs_volume* _volume, fs_vnode* _node, void* _cookie)
{
	return B_OK;
}


static status_t
apfs_free_attr_dir_cookie(fs_volume* _volume, fs_vnode* _node,
	void* _cookie)
{
	delete (AttributeIterator*)_cookie;
	return B_OK;
}


static status_t
apfs_read_attr_dir(fs_volume* _volume, fs_vnode* _node, void* _cookie,
	struct dirent* buffer, size_t bufferSize, uint32* _num)
{
	Volume* volume = (Volume*)_volume->private_volume;
	Inode* inode = (Inode*)_node->private_node;
	AttributeIterator* iterator = (AttributeIterator*)_cookie;

	*_num = 0;
	if (bufferSize <= offsetof(struct dirent, d_name))
		return B_BUFFER_OVERFLOW;

	size_t length = bufferSize - offsetof(struct dirent, d_name);
	if (length > B_ATTR_NAME_LENGTH)
		length = B_ATTR_NAME_LENGTH;

	status_t status = iterator->GetNext(buffer->d_name, &length);
	if (status == B_ENTRY_NOT_FOUND)
		return B_OK;
	if (status != B_OK)
		return status;

	buffer->d_dev = volume->ID();
	buffer->d_ino = inode->ID();
	buffer->d_reclen = offsetof(struct dirent, d_name) + length + 1;
	*_num = 1;
	return B_OK;
}


static status_t
apfs_rewind_attr_dir(fs_volume* _volume, fs_vnode* _node, void* _cookie)
{
	return ((AttributeIterator*)_cookie)->Rewind();
}


static status_t
apfs_create_attr(fs_volume* _volume, fs_vnode* _node, const char* name,
	uint32 type, int openMode, void** _cookie)
{
	return B_READ_ONLY_DEVICE;
}


static status_t
apfs_open_attr(fs_volume* _volume, fs_vnode* _node, const char* name,
	int openMode, void** _cookie)
{
	if ((openMode & O_RWMASK) != O_RDONLY || (openMode & O_TRUNC) != 0)
		return B_READ_ONLY_DEVICE;

	Inode* inode = (Inode*)_node->private_node;
	status_t status = inode->CheckPermissions(R_OK);
	if (status != B_OK)
		return status;

	// Hidden: the compressed data of a compressed file.
	if (inode->IsCompressed()
		&& (strcmp(name, APFS_DECMPFS_ATTRIBUTE_NAME) == 0
			|| strcmp(name, APFS_RESOURCE_FORK_NAME) == 0))
		return B_ENTRY_NOT_FOUND;

	apfs_attribute* attribute
		= (apfs_attribute*)malloc(sizeof(apfs_attribute));
	if (attribute == NULL)
		return B_NO_MEMORY;

	status = inode->FindAttribute(name, attribute);
	if (status != B_OK) {
		free(attribute);
		return status;
	}

	*_cookie = attribute;
	return B_OK;
}


static status_t
apfs_close_attr(fs_volume* _volume, fs_vnode* _node, void* _cookie)
{
	return B_OK;
}


static status_t
apfs_free_attr_cookie(fs_volume* _volume, fs_vnode* _node, void* _cookie)
{
	free(_cookie);
	return B_OK;
}


static status_t
apfs_read_attr(fs_volume* _volume, fs_vnode* _node, void* _cookie,
	off_t pos, void* buffer, size_t* _length)
{
	Inode* inode = (Inode*)_node->private_node;
	return inode->ReadAttribute(*(apfs_attribute*)_cookie, pos, buffer,
		_length);
}


static status_t
apfs_write_attr(fs_volume* _volume, fs_vnode* _node, void* _cookie,
	off_t pos, const void* buffer, size_t* _length)
{
	*_length = 0;
	return B_READ_ONLY_DEVICE;
}


static status_t
apfs_read_attr_stat(fs_volume* _volume, fs_vnode* _node, void* _cookie,
	struct stat* stat)
{
	apfs_attribute* attribute = (apfs_attribute*)_cookie;
	stat->st_type = B_RAW_TYPE;
	stat->st_size = attribute->size;
	return B_OK;
}


static status_t
apfs_write_attr_stat(fs_volume* _volume, fs_vnode* _node, void* _cookie,
	const struct stat* stat, int statMask)
{
	return B_READ_ONLY_DEVICE;
}


static status_t
apfs_rename_attr(fs_volume* _volume, fs_vnode* _fromNode,
	const char* fromName, fs_vnode* _toNode, const char* toName)
{
	return B_READ_ONLY_DEVICE;
}


static status_t
apfs_remove_attr(fs_volume* _volume, fs_vnode* _node, const char* name)
{
	return B_READ_ONLY_DEVICE;
}


// #pragma mark - Directories


static status_t
apfs_open_dir(fs_volume* _volume, fs_vnode* _node, void** _cookie)
{
	Inode* inode = (Inode*)_node->private_node;
	if (!inode->IsDirectory())
		return B_NOT_A_DIRECTORY;

	status_t status = inode->CheckPermissions(R_OK);
	if (status != B_OK)
		return status;

	dir_cookie* cookie = new(std::nothrow) dir_cookie;
	if (cookie == NULL)
		return B_NO_MEMORY;

	cookie->position = 0;
	cookie->iterator = new(std::nothrow) DirectoryIterator(inode);
	if (cookie->iterator == NULL)
		status = B_NO_MEMORY;
	else
		status = cookie->iterator->InitCheck();
	if (status != B_OK) {
		delete cookie->iterator;
		delete cookie;
		return status;
	}

	*_cookie = cookie;
	return B_OK;
}


static status_t
apfs_close_dir(fs_volume* _volume, fs_vnode* _node, void* _cookie)
{
	return B_OK;
}


static status_t
apfs_free_dir_cookie(fs_volume* _volume, fs_vnode* _node, void* _cookie)
{
	dir_cookie* cookie = (dir_cookie*)_cookie;
	delete cookie->iterator;
	delete cookie;
	return B_OK;
}


static status_t
apfs_read_dir(fs_volume* _volume, fs_vnode* _node, void* _cookie,
	struct dirent* buffer, size_t bufferSize, uint32* _num)
{
	Volume* volume = (Volume*)_volume->private_volume;
	Inode* inode = (Inode*)_node->private_node;
	dir_cookie* cookie = (dir_cookie*)_cookie;

	uint32 maxCount = *_num;
	uint32 count = 0;

	while (count < maxCount && buffer != NULL) {
		size_t nameSpace = bufferSize > offsetof(struct dirent, d_name)
			? bufferSize - offsetof(struct dirent, d_name) : 0;
		size_t length;
		ino_t id;

		if (cookie->position < 2) {
			const char* name = cookie->position == 0 ? "." : "..";
			length = strlen(name);
			if (length + 1 > nameSpace) {
				if (count == 0)
					return B_BUFFER_OVERFLOW;
				break;
			}
			strlcpy(buffer->d_name, name, length + 1);
			if (cookie->position == 0)
				id = inode->ID();
			else {
				id = inode->ID() == volume->RootID()
					? volume->RootID() : inode->ParentID();
			}
			cookie->position++;
		} else {
			length = nameSpace;
			status_t status = cookie->iterator->GetNext(buffer->d_name,
				&length, &id);
			if (status == B_ENTRY_NOT_FOUND)
				break;
			if (status == B_BUFFER_OVERFLOW) {
				// GetNext() keeps the entry for the next call.
				if (count == 0)
					return B_BUFFER_OVERFLOW;
				break;
			}
			if (status != B_OK) {
				if (count == 0)
					return status;
				break;
			}
		}

		buffer->d_dev = volume->ID();
		buffer->d_ino = id;
		count++;

		buffer = next_dirent(buffer, length, bufferSize);
	}

	*_num = count;
	return B_OK;
}


static status_t
apfs_rewind_dir(fs_volume* _volume, fs_vnode* _node, void* _cookie)
{
	dir_cookie* cookie = (dir_cookie*)_cookie;
	cookie->position = 0;
	return cookie->iterator->Rewind();
}


// #pragma mark - Module


static uint32
apfs_get_supported_operations(partition_data* partition, uint32 mask)
{
	return 0;
}


static status_t
apfs_std_ops(int32 op, ...)
{
	switch (op) {
		case B_MODULE_INIT:
		case B_MODULE_UNINIT:
			return B_OK;
		default:
			return B_ERROR;
	}
}


fs_volume_ops gAPFSVolumeOps = {
	&apfs_unmount,
	&apfs_read_fs_info,
	NULL,				// write_fs_info
	NULL,				// sync
	&apfs_get_vnode,
};


fs_vnode_ops gAPFSVnodeOps = {
	// vnode operations
	&apfs_lookup,
	NULL,				// get_vnode_name: the VFS fallback is fine
	&apfs_put_vnode,
	NULL,				// remove_vnode: nothing is ever removed

	// VM file access
	&apfs_can_page,
	&apfs_read_pages,
	&apfs_write_pages,

	&apfs_io,
	NULL,				// cancel_io

	&apfs_get_file_map,

	&apfs_ioctl,
	NULL,				// set_flags
	NULL,				// select
	NULL,				// deselect
	&apfs_fsync,

	&apfs_read_symlink,
	&apfs_create_symlink,

	&apfs_link,
	&apfs_unlink,
	&apfs_rename,

	&apfs_access,
	&apfs_read_stat,
	&apfs_write_stat,
	NULL,				// preallocate

	// file operations
	&apfs_create,
	&apfs_open,
	&apfs_close,
	&apfs_free_cookie,
	&apfs_read,
	&apfs_write,

	// directory operations
	&apfs_create_dir,
	&apfs_remove_dir,
	&apfs_open_dir,
	&apfs_close_dir,
	&apfs_free_dir_cookie,
	&apfs_read_dir,
	&apfs_rewind_dir,

	// attribute directory operations
	&apfs_open_attr_dir,
	&apfs_close_attr_dir,
	&apfs_free_attr_dir_cookie,
	&apfs_read_attr_dir,
	&apfs_rewind_attr_dir,

	// attribute operations
	&apfs_create_attr,
	&apfs_open_attr,
	&apfs_close_attr,
	&apfs_free_attr_cookie,
	&apfs_read_attr,
	&apfs_write_attr,
	&apfs_read_attr_stat,
	&apfs_write_attr_stat,
	&apfs_rename_attr,
	&apfs_remove_attr,
};


static file_system_module_info sAPFSFileSystem = {
	{
		"file_systems/apfs" B_CURRENT_FS_API_VERSION,
		0,
		apfs_std_ops,
	},

	"apfs",					// short_name
	"Apple File System",	// pretty_name

	// DDM flags: read-only, nothing to change on disk
	0,

	// scanning
	apfs_identify_partition,
	apfs_scan_partition,
	apfs_free_identify_partition_cookie,
	NULL,					// free_partition_content_cookie

	&apfs_mount,

	// capability querying
	&apfs_get_supported_operations,

	NULL,					// validate_resize
	NULL,					// validate_move
	NULL,					// validate_set_content_name
	NULL,					// validate_set_content_parameters
	NULL,					// validate_initialize

	// shadow partition modification
	NULL,					// shadow_changed

	// writing
	NULL,					// defragment
	NULL,					// repair
	NULL,					// resize
	NULL,					// move
	NULL,					// set_content_name
	NULL,					// set_content_parameters
	NULL,					// initialize
	NULL,					// uninitialize
};


module_info* modules[] = {
	(module_info*)&sAPFSFileSystem,
	NULL,
};
