/*
 * Copyright 2026, RenkuOS contributors.
 * Distributed under the terms of the MIT License.
 *
 * Inodes, file data and directories.
 * Source: Apple File System Reference, revision 2020-06-22: "File-System
 * Objects" (j_key_t, j_inode_val_t, j_drec_hashed_key_t, j_drec_val_t,
 * j_xattr_key_t, j_xattr_val_t), "File-System Constants", "Data Streams"
 * (j_file_extent_key_t, j_file_extent_val_t, j_dstream_t), "Extended
 * Fields" (xf_blob_t, x_field_t: data follows the x_field_t array, each
 * item aligned to 8 bytes).
 */


#include "Inode.h"

#include "BTree.h"
#include "Container.h"
#include "Decmpfs.h"
#include "Volume.h"

#include <CRCTable.h>


// Longest file-system key: a hashed directory entry with a 1023-byte name
// (10-bit length) after its 12-byte header.
static const size_t kMaxKeyLength = 12 + 1024;


static status_t
copy_to_buffer(void* to, const void* from, size_t length)
{
#ifndef FS_SHELL
	if (IS_USER_ADDRESS(to))
		return user_memcpy(to, from, length);
#endif
	memcpy(to, from, length);
	return B_OK;
}


static status_t
fill_buffer_with_zeros(void* to, size_t length)
{
#ifndef FS_SHELL
	if (IS_USER_ADDRESS(to))
		return user_memset(to, 0, length);
#endif
	memset(to, 0, length);
	return B_OK;
}


int
apfs_compare_j_key(const void* _key, uint16 keyLength, const void* _target,
	void* /*context*/)
{
	if (keyLength < sizeof(apfs_j_key))
		return -1;

	const apfs_j_key* key = (const apfs_j_key*)_key;
	const apfs_j_key* target = (const apfs_j_key*)_target;

	if (key->ObjectID() != target->ObjectID())
		return key->ObjectID() < target->ObjectID() ? -1 : 1;
	if (key->Type() != target->Type())
		return key->Type() < target->Type() ? -1 : 1;
	return keyLength > sizeof(apfs_j_key) ? 1 : 0;
}


int
apfs_compare_file_extent_key(const void* _key, uint16 keyLength,
	const void* _target, void* context)
{
	int result = apfs_compare_j_key(_key, sizeof(apfs_j_key), _target,
		context);
	if (result != 0)
		return result;
	if (keyLength < sizeof(apfs_j_file_extent_key))
		return -1;

	const apfs_j_file_extent_key* key = (const apfs_j_file_extent_key*)_key;
	const apfs_j_file_extent_key* target
		= (const apfs_j_file_extent_key*)_target;
	if (key->LogicalAddress() != target->LogicalAddress())
		return key->LogicalAddress() < target->LogicalAddress() ? -1 : 1;
	return 0;
}


// #pragma mark - Inode


Inode::Inode(Volume* volume, ino_t id)
	:
	fVolume(volume),
	fID(id),
	fParentID(0),
	fPrivateID(0),
	fMode(0),
	fBSDFlags(0),
	fInternalFlags(0),
	fUserID(0),
	fGroupID(0),
	fLinkCount(1),
	fSize(0),
	fAllocatedSize(0),
	fCreationTime(0),
	fModificationTime(0),
	fChangeTime(0),
	fAccessTime(0),
	fReportedCompressed(false),
	fCache(NULL),
	fMap(NULL),
	fDecmpfs(NULL)
{
}


Inode::~Inode()
{
	if (fCache != NULL)
		file_cache_delete(fCache);
	if (fMap != NULL)
		file_map_delete(fMap);
	delete fDecmpfs;
}


/*static*/ void
Inode::_ToTimespec(uint64 nanoseconds, struct timespec& time)
{
	// "nanoseconds since January 1, 1970 at 0:00 UTC"
	time.tv_sec = nanoseconds / 1000000000;
	time.tv_nsec = nanoseconds % 1000000000;
}


status_t
Inode::Init()
{
	uint32 blockSize = fVolume->BlockSize();
	uint8* value = (uint8*)malloc(blockSize);
	if (value == NULL)
		return B_NO_MEMORY;
	MemoryDeleter valueDeleter(value);

	apfs_j_key key;
	key.Set(fID, APFS_TYPE_INODE);
	uint16 valueLength = blockSize;
	status_t status = fVolume->FileSystemTree()->FindExact(&key,
		apfs_compare_j_key, NULL, value, &valueLength);
	if (status != B_OK)
		return status;

	if (valueLength < sizeof(apfs_j_inode_val)) {
		ERROR("inode %" B_PRIdINO ": record too short (%u bytes)\n", fID,
			valueLength);
		return B_BAD_DATA;
	}

	const apfs_j_inode_val* inode = (const apfs_j_inode_val*)value;
	fParentID = B_LENDIAN_TO_HOST_INT64(inode->parent_id);
	fPrivateID = B_LENDIAN_TO_HOST_INT64(inode->private_id);
	fCreationTime = B_LENDIAN_TO_HOST_INT64(inode->create_time);
	fModificationTime = B_LENDIAN_TO_HOST_INT64(inode->mod_time);
	fChangeTime = B_LENDIAN_TO_HOST_INT64(inode->change_time);
	fAccessTime = B_LENDIAN_TO_HOST_INT64(inode->access_time);
	fInternalFlags = B_LENDIAN_TO_HOST_INT64(inode->internal_flags);
	fLinkCount = B_LENDIAN_TO_HOST_INT32(inode->nchildren_or_nlink);
	fBSDFlags = B_LENDIAN_TO_HOST_INT32(inode->bsd_flags);
	fUserID = B_LENDIAN_TO_HOST_INT32(inode->owner);
	fGroupID = B_LENDIAN_TO_HOST_INT32(inode->group);
	fMode = B_LENDIAN_TO_HOST_INT16(inode->mode);

	if (IsDirectory()) {
		// nchildren counts entries; links are those plus "." and "..".
		fLinkCount += 2;
	}

	_ParseExtendedFields(inode->xfields,
		valueLength - sizeof(apfs_j_inode_val));

	if (IsCompressed()
		&& (fInternalFlags & APFS_INODE_HAS_UNCOMPRESSED_SIZE) != 0)
		fSize = B_LENDIAN_TO_HOST_INT64(inode->uncompressed_size);

	if (IsSymlink()) {
		// The size of a symlink is the length of its target.
		char* target = (char*)malloc(B_PATH_NAME_LENGTH);
		if (target == NULL)
			return B_NO_MEMORY;
		size_t length = B_PATH_NAME_LENGTH;
		if (ReadLink(target, &length) == B_OK)
			fSize = length;
		free(target);
	}

	// Compressed files (decmpfs): the size is in their compression header;
	// their pages are decompressed by the io/read_pages hooks.
	if (IsFile() && IsCompressed()) {
		fDecmpfs = new(std::nothrow) Decmpfs(this);
		if (fDecmpfs == NULL)
			return B_NO_MEMORY;
		status = fDecmpfs->Init();
		if (status == B_OK) {
			fSize = fDecmpfs->Size();
		} else {
			// Keep the node usable (stat, attributes); reads fail.
			delete fDecmpfs;
			fDecmpfs = NULL;
		}
	}

	// The file map needs the data on disk as is: uncompressed files only.
	if (IsFile() && (!IsCompressed() || fDecmpfs != NULL)) {
		fCache = file_cache_create(fVolume->ID(), fID, fSize);
		if (fCache == NULL)
			return B_NO_MEMORY;
	}
	if (IsFile() && !IsCompressed()) {
		fMap = file_map_create(fVolume->ID(), fID, fSize);
		if (fMap == NULL)
			return B_NO_MEMORY;
	}

	return B_OK;
}


status_t
Inode::FindExtent(off_t pos, off_t* _diskOffset, off_t* _length)
{
	if (pos < 0 || pos >= fSize)
		return B_BAD_VALUE;

	uint8* key = (uint8*)malloc(kMaxKeyLength);
	if (key == NULL)
		return B_NO_MEMORY;
	MemoryDeleter keyDeleter(key);

	uint32 blockSize = fVolume->BlockSize();
	off_t remaining = fSize - pos;

	apfs_j_file_extent_key target;
	target.hdr.Set(fPrivateID, APFS_TYPE_FILE_EXTENT);
	target.logical_addr = B_HOST_TO_LENDIAN_INT64(pos);

	apfs_j_file_extent_val value;
	uint16 keyLength = kMaxKeyLength;
	uint16 valueLength = sizeof(value);
	status_t status = fVolume->FileSystemTree()->FindLessOrEqual(&target,
		apfs_compare_file_extent_key, NULL, key, &keyLength, &value,
		&valueLength);
	if (status != B_OK && status != B_ENTRY_NOT_FOUND
		&& status != B_BUFFER_OVERFLOW)
		return status;

	const apfs_j_file_extent_key* found = (const apfs_j_file_extent_key*)key;
	if (status == B_OK && keyLength >= sizeof(apfs_j_file_extent_key)
		&& valueLength >= sizeof(apfs_j_file_extent_val)
		&& found->hdr.ObjectID() == fPrivateID
		&& found->hdr.Type() == APFS_TYPE_FILE_EXTENT
		&& (uint64)pos < found->LogicalAddress() + value.Length()) {
		uint64 within = pos - found->LogicalAddress();
		off_t length = value.Length() - within;
		if (length > remaining)
			length = remaining;

		if (value.PhysicalBlock() == 0) {
			// An extent without blocks reads as zeros.
			*_diskOffset = -1;
		} else {
			uint64 block = value.PhysicalBlock() + within / blockSize;
			if (block + (within % blockSize + length + blockSize - 1)
					/ blockSize > fVolume->GetContainer().BlockCount()) {
				ERROR("inode %" B_PRIdINO ": extent outside the container\n",
					fID);
				return B_BAD_DATA;
			}
			*_diskOffset = (off_t)block * blockSize + within % blockSize;
		}
		*_length = length;
		return B_OK;
	}

	// A hole: it lasts until the next extent, or to the end of the file.
	off_t holeLength = remaining;
	BTreeIterator iterator(fVolume->FileSystemTree());
	status = iterator.InitCheck();
	if (status == B_OK)
		status = iterator.Seek(&target, apfs_compare_file_extent_key, NULL);
	if (status == B_OK) {
		const uint8* nextKey;
		const uint8* nextValue;
		uint16 nextKeyLength;
		uint16 nextValueLength;
		if (iterator.GetNext(&nextKey, &nextKeyLength, &nextValue,
				&nextValueLength) == B_OK
			&& nextKeyLength >= sizeof(apfs_j_file_extent_key)) {
			const apfs_j_file_extent_key* next
				= (const apfs_j_file_extent_key*)nextKey;
			if (next->hdr.ObjectID() == fPrivateID
				&& next->hdr.Type() == APFS_TYPE_FILE_EXTENT
				&& next->LogicalAddress() > (uint64)pos
				&& (off_t)(next->LogicalAddress() - pos) < holeLength)
				holeLength = next->LogicalAddress() - pos;
		}
	}

	*_diskOffset = -1;
	*_length = holeLength;
	return B_OK;
}


status_t
Inode::ReadCompressed(off_t pos, uint8* buffer, size_t* _length)
{
	if (fDecmpfs == NULL) {
		*_length = 0;
		return B_NOT_SUPPORTED;
	}
	return fDecmpfs->Read(pos, buffer, _length);
}


status_t
Inode::Read(off_t pos, void* buffer, size_t* _length)
{
	if (fCache == NULL)
		return ReadAt(pos, buffer, _length);
	if (pos < 0)
		return B_BAD_VALUE;
	return file_cache_read(fCache, NULL, pos, buffer, _length);
}


void
Inode::_ParseExtendedFields(const uint8* data, size_t length)
{
	if (length < sizeof(apfs_xf_blob))
		return;

	// xf_used_data counts the field data only, not the x_field_t table in
	// front of it: on macOS-made volumes it equals the sum of the 8-byte
	// aligned data sizes. (The reference says it includes the table; real
	// records disagree, e.g. 2 fields with 10 + 40 bytes of data: 56.)
	const apfs_xf_blob* blob = (const apfs_xf_blob*)data;
	uint16 count = B_LENDIAN_TO_HOST_INT16(blob->xf_num_exts);
	size_t used = B_LENDIAN_TO_HOST_INT16(blob->xf_used_data);
	size_t tableSize = count * sizeof(apfs_x_field);
	if (sizeof(apfs_xf_blob) + tableSize + used > length) {
		ERROR("inode %" B_PRIdINO ": bad extended fields\n", fID);
		return;
	}

	const apfs_x_field* fields = (const apfs_x_field*)blob->xf_data;
	const uint8* fieldStart = blob->xf_data + tableSize;
	size_t offset = 0;
	for (uint16 i = 0; i < count; i++) {
		size_t size = B_LENDIAN_TO_HOST_INT16(fields[i].x_size);
		if (offset + size > used)
			return;

		const uint8* fieldData = fieldStart + offset;
		if (fields[i].x_type == APFS_INO_EXT_TYPE_DSTREAM
			&& size >= sizeof(apfs_j_dstream)) {
			apfs_j_dstream stream;
			memcpy(&stream, fieldData, sizeof(stream));
			fSize = B_LENDIAN_TO_HOST_INT64(stream.size);
			fAllocatedSize = B_LENDIAN_TO_HOST_INT64(stream.alloced_size);
		}

		offset += (size + 7) & ~(size_t)7;
	}
}


status_t
Inode::CheckPermissions(int accessMode) const
{
	if ((accessMode & W_OK) != 0)
		return B_READ_ONLY_DEVICE;

	return check_access_permissions(accessMode, fMode, fGroupID, fUserID);
}


status_t
Inode::ReadAt(off_t pos, void* buffer, size_t* _length)
{
	if (IsDirectory()) {
		*_length = 0;
		return B_IS_A_DIRECTORY;
	}
	if (IsCompressed()) {
		*_length = 0;
		if (!fReportedCompressed) {
			INFORM("inode %" B_PRIdINO " is compressed and its compression "
				"header could not be read\n", fID);
			fReportedCompressed = true;
		}
		return B_NOT_SUPPORTED;
	}
	return _ReadStream(fPrivateID, fSize, pos, buffer, _length);
}


/*!	Reads from a data stream: the file extents keyed by streamID (a file's
	private_id, or a stream-backed attribute's xattr_obj_id).
*/
status_t
Inode::_ReadStream(uint64 streamID, off_t size, off_t pos, void* buffer,
	size_t* _length)
{
	size_t wanted = *_length;
	*_length = 0;

	if (pos < 0)
		return B_BAD_VALUE;
	if (pos >= size || wanted == 0)
		return B_OK;

	off_t end = pos + wanted;
	if (end > size || end < pos)
		end = size;

	Container& container = fVolume->GetContainer();
	uint32 blockSize = container.BlockSize();
	uint8* block = (uint8*)malloc(blockSize);
	uint8* key = (uint8*)malloc(kMaxKeyLength);
	if (block == NULL || key == NULL) {
		free(block);
		free(key);
		return B_NO_MEMORY;
	}
	MemoryDeleter blockDeleter(block);
	MemoryDeleter keyDeleter(key);

	// The extent that covers the current position, if any.
	bool haveExtent = false;
	uint64 extentStart = 0;
	uint64 extentLength = 0;
	uint64 extentBlock = 0;

	uint8* to = (uint8*)buffer;
	status_t status = B_OK;
	while (pos < end) {
		if (!haveExtent || (uint64)pos < extentStart
			|| (uint64)pos >= extentStart + extentLength) {
			apfs_j_file_extent_key target;
			target.hdr.Set(streamID, APFS_TYPE_FILE_EXTENT);
			target.logical_addr = B_HOST_TO_LENDIAN_INT64(pos);

			apfs_j_file_extent_val value;
			uint16 keyLength = kMaxKeyLength;
			uint16 valueLength = sizeof(value);
			status = fVolume->FileSystemTree()->FindLessOrEqual(&target,
				apfs_compare_file_extent_key, NULL, key, &keyLength, &value,
				&valueLength);

			haveExtent = false;
			if (status == B_OK && keyLength >= sizeof(apfs_j_file_extent_key)
				&& valueLength >= sizeof(apfs_j_file_extent_val)) {
				const apfs_j_file_extent_key* found
					= (const apfs_j_file_extent_key*)key;
				if (found->hdr.ObjectID() == streamID
					&& found->hdr.Type() == APFS_TYPE_FILE_EXTENT
					&& (uint64)pos < found->LogicalAddress() + value.Length()) {
					haveExtent = true;
					extentStart = found->LogicalAddress();
					extentLength = value.Length();
					extentBlock = value.PhysicalBlock();
				}
			} else if (status != B_OK && status != B_ENTRY_NOT_FOUND
				&& status != B_BUFFER_OVERFLOW)
				break;
			status = B_OK;
		}

		size_t within = pos % blockSize;
		size_t chunk = blockSize - within;
		if ((off_t)chunk > end - pos)
			chunk = end - pos;

		if (haveExtent && extentBlock != 0) {
			if ((uint64)pos + chunk > extentStart + extentLength)
				chunk = extentStart + extentLength - pos;
			uint64 blockIndex = (pos - extentStart) / blockSize;
			status = container.ReadBlock(extentBlock + blockIndex, block);
			if (status != B_OK)
				break;
			status = copy_to_buffer(to, block + within, chunk);
		} else {
			// A hole in a sparse file, or an extent without blocks.
			if (haveExtent && (uint64)pos + chunk > extentStart + extentLength)
				chunk = extentStart + extentLength - pos;
			status = fill_buffer_with_zeros(to, chunk);
		}
		if (status != B_OK)
			break;

		to += chunk;
		pos += chunk;
		*_length += chunk;
	}

	// Report what was read; an error only if nothing was.
	if (*_length > 0)
		return B_OK;
	return status;
}


status_t
Inode::GetEmbeddedAttribute(const char* name, uint8* data, size_t* _length)
{
	apfs_attribute* attribute
		= (apfs_attribute*)malloc(sizeof(apfs_attribute));
	if (attribute == NULL)
		return B_NO_MEMORY;
	MemoryDeleter attributeDeleter(attribute);

	status_t status = FindAttribute(name, attribute);
	if (status != B_OK)
		return status;
	if (!attribute->embedded)
		return B_NOT_SUPPORTED;
	if ((size_t)attribute->size > *_length)
		return B_BUFFER_OVERFLOW;

	memcpy(data, attribute->data, attribute->size);
	*_length = attribute->size;
	return B_OK;
}


status_t
Inode::ReadAttribute(const apfs_attribute& attribute, off_t pos,
	void* buffer, size_t* _length)
{
	if (!attribute.embedded) {
		return _ReadStream(attribute.stream_id, attribute.size, pos, buffer,
			_length);
	}

	size_t wanted = *_length;
	*_length = 0;
	if (pos < 0)
		return B_BAD_VALUE;
	if (pos >= attribute.size)
		return B_OK;
	if ((off_t)wanted > attribute.size - pos)
		wanted = attribute.size - pos;

	status_t status = copy_to_buffer(buffer, attribute.data + pos, wanted);
	if (status != B_OK)
		return status;
	*_length = wanted;
	return B_OK;
}


status_t
Inode::FindAttribute(const char* name, apfs_attribute* attribute)
{
	BTreeIterator iterator(fVolume->FileSystemTree());
	status_t status = iterator.InitCheck();
	if (status != B_OK)
		return status;

	apfs_j_key target;
	target.Set(fID, APFS_TYPE_XATTR);
	status = iterator.Seek(&target, apfs_compare_j_key, NULL);
	if (status != B_OK)
		return status;

	size_t nameLength = strlen(name) + 1;
	while (true) {
		const uint8* key;
		const uint8* value;
		uint16 keyLength;
		uint16 valueLength;
		status = iterator.GetNext(&key, &keyLength, &value, &valueLength);
		if (status != B_OK)
			return status == B_ENTRY_NOT_FOUND ? B_ENTRY_NOT_FOUND : status;

		const apfs_j_xattr_key* xattrKey = (const apfs_j_xattr_key*)key;
		if (keyLength < sizeof(apfs_j_xattr_key)
			|| xattrKey->hdr.ObjectID() != (uint64)fID
			|| xattrKey->hdr.Type() != APFS_TYPE_XATTR)
			return B_ENTRY_NOT_FOUND;

		if (xattrKey->NameLength() != nameLength
			|| sizeof(apfs_j_xattr_key) + nameLength > keyLength
			|| memcmp(xattrKey->name, name, nameLength) != 0)
			continue;

		const apfs_j_xattr_val* xattr = (const apfs_j_xattr_val*)value;
		if (value == NULL || valueLength < sizeof(apfs_j_xattr_val))
			return B_BAD_DATA;

		if ((xattr->Flags() & APFS_XATTR_DATA_EMBEDDED) != 0) {
			size_t length = xattr->DataLength();
			if (sizeof(apfs_j_xattr_val) + length > valueLength
				|| length > sizeof(attribute->data))
				return B_BAD_DATA;
			attribute->embedded = true;
			attribute->stream_id = 0;
			attribute->size = length;
			memcpy(attribute->data, xattr->xdata, length);
			return B_OK;
		}

		if ((xattr->Flags() & APFS_XATTR_DATA_STREAM) != 0) {
			// xdata holds a j_xattr_dstream_t.
			apfs_j_xattr_dstream stream;
			if (sizeof(apfs_j_xattr_val) + sizeof(stream) > valueLength)
				return B_BAD_DATA;
			memcpy(&stream, xattr->xdata, sizeof(stream));
			attribute->embedded = false;
			attribute->stream_id
				= B_LENDIAN_TO_HOST_INT64(stream.xattr_obj_id);
			attribute->size = B_LENDIAN_TO_HOST_INT64(stream.dstream.size);
			return B_OK;
		}
		return B_BAD_DATA;
	}
}


status_t
Inode::ReadLink(char* buffer, size_t* _bufferSize)
{
	if (!IsSymlink())
		return B_BAD_VALUE;

	// The target is the SYMLINK_EA_NAME attribute: a null-terminated path.
	char* target = (char*)malloc(B_PATH_NAME_LENGTH + 1);
	if (target == NULL)
		return B_NO_MEMORY;
	MemoryDeleter targetDeleter(target);
	size_t length = B_PATH_NAME_LENGTH;
	status_t status = GetEmbeddedAttribute(APFS_SYMLINK_EA_NAME,
		(uint8*)target, &length);
	if (status != B_OK)
		return status;

	target[length] = '\0';
	length = strnlen(target, length);

	size_t copy = length < *_bufferSize ? length : *_bufferSize;
	memcpy(buffer, target, copy);
	*_bufferSize = length;
	return B_OK;
}


// #pragma mark - DirectoryIterator


DirectoryIterator::DirectoryIterator(Inode* directory)
	:
	fDirectory(directory),
	fIterator(NULL),
	fStatus(B_OK),
	fLastLength(0),
	fLastID(0),
	fLastHash(0),
	fHaveLast(false),
	fPending(false)
{
	// Directory entries use hashed keys (j_drec_hashed_key_t) on volumes
	// that are case- or normalization-insensitive, which every volume made
	// by macOS 10.13 and later is; otherwise j_drec_key_t.
	uint64 features = directory->GetVolume()->SuperBlock()
		.IncompatibleFeatures();
	fHashedNames = (features & (APFS_INCOMPAT_CASE_INSENSITIVE
		| APFS_INCOMPAT_NORMALIZATION_INSENSITIVE)) != 0;

	if (!directory->IsDirectory()) {
		fStatus = B_NOT_A_DIRECTORY;
		return;
	}

	fIterator = new(std::nothrow) BTreeIterator(
		directory->GetVolume()->FileSystemTree());
	if (fIterator == NULL) {
		fStatus = B_NO_MEMORY;
		return;
	}
	fStatus = fIterator->InitCheck();
	if (fStatus == B_OK)
		fStatus = Rewind();
}


DirectoryIterator::~DirectoryIterator()
{
	delete fIterator;
}


status_t
DirectoryIterator::Rewind()
{
	fHaveLast = false;
	fPending = false;

	apfs_j_key target;
	target.Set(fDirectory->ID(), APFS_TYPE_DIR_REC);
	return fIterator->Seek(&target, apfs_compare_j_key, NULL);
}


status_t
DirectoryIterator::GetNext(char* name, size_t* _length, ino_t* _id)
{
	if (fStatus != B_OK)
		return fStatus;

	if (!fPending) {
		status_t status = _ReadEntry();
		if (status != B_OK) {
			fHaveLast = false;
			return status;
		}
	}
	fPending = false;

	if (fLastLength + 1 > *_length) {
		fPending = true;
		return B_BUFFER_OVERFLOW;
	}

	memcpy(name, fLastName, fLastLength);
	name[fLastLength] = '\0';
	*_length = fLastLength;
	*_id = fLastID;
	return B_OK;
}


status_t
DirectoryIterator::_ReadEntry()
{
	const uint8* key;
	const uint8* value;
	uint16 keyLength;
	uint16 valueLength;
	status_t status = fIterator->GetNext(&key, &keyLength, &value,
		&valueLength);
	if (status != B_OK)
		return status;

	const apfs_j_key* header = (const apfs_j_key*)key;
	if (keyLength < sizeof(apfs_j_key)
		|| header->ObjectID() != (uint64)fDirectory->ID()
		|| header->Type() != APFS_TYPE_DIR_REC)
		return B_ENTRY_NOT_FOUND;

	const uint8* entryName;
	size_t nameLength;
	if (fHashedNames) {
		const apfs_j_drec_hashed_key* drec
			= (const apfs_j_drec_hashed_key*)key;
		if (keyLength < sizeof(apfs_j_drec_hashed_key))
			return B_BAD_DATA;
		entryName = drec->name;
		nameLength = drec->NameLength();
		fLastHash = B_LENDIAN_TO_HOST_INT32(drec->name_len_and_hash)
			>> APFS_J_DREC_HASH_SHIFT;
		if (sizeof(apfs_j_drec_hashed_key) + nameLength > keyLength)
			return B_BAD_DATA;
	} else {
		// j_drec_key_t: j_key_t, then a 16-bit length.
		if (keyLength < sizeof(apfs_j_key) + sizeof(uint16))
			return B_BAD_DATA;
		uint16 length;
		memcpy(&length, key + sizeof(apfs_j_key), sizeof(length));
		nameLength = B_LENDIAN_TO_HOST_INT16(length);
		entryName = key + sizeof(apfs_j_key) + sizeof(uint16);
		if (sizeof(apfs_j_key) + sizeof(uint16) + nameLength > keyLength)
			return B_BAD_DATA;
	}

	// The stored length counts the final null.
	if (nameLength > 0 && entryName[nameLength - 1] == '\0')
		nameLength--;
	if (nameLength >= sizeof(fLastName))
		return B_BAD_DATA;

	if (value == NULL || valueLength < sizeof(apfs_j_drec_val))
		return B_BAD_DATA;
	const apfs_j_drec_val* drecValue = (const apfs_j_drec_val*)value;

	memcpy(fLastName, entryName, nameLength);
	fLastName[nameLength] = '\0';
	fLastLength = nameLength;
	fLastID = drecValue->FileID();
	fHaveLast = true;
	return B_OK;
}


/*!	The directory-entry name hash for an ASCII name, as macOS computes it.
	The reference ("j_drec_hashed_key_t") says: NFD-normalize, encode as
	null-terminated UTF-32, CRC-32C, complement, keep 22 bits. On real
	volumes the stored hashes match a CRC-32C over the UTF-32 (little
	endian) code points WITHOUT the terminating null, started at 0xffffffff
	and not complemented at the end; case-insensitive volumes hash the
	case-folded name. Checked against every name in the test images,
	including NFC/NFD, Japanese, emoji and upper-case names.
	For ASCII, NFD changes nothing and case folding is lowercasing.
*/
static uint32
ascii_name_hash(const char* name, bool fold)
{
	uint32 crc = 0xffffffff;
	for (; *name != '\0'; name++) {
		uint8 c = (uint8)*name;
		if (fold && c >= 'A' && c <= 'Z')
			c += 'a' - 'A';
		uint8 utf32[4] = { c, 0, 0, 0 };
		crc = calculate_crc32c(crc, utf32, sizeof(utf32));
	}
	return crc & (APFS_J_DREC_HASH_MASK >> APFS_J_DREC_HASH_SHIFT);
}


static bool
is_ascii(const char* name)
{
	for (; *name != '\0'; name++) {
		if ((uint8)*name >= 0x80)
			return false;
	}
	return true;
}


// Hashed directory keys sort by object id, type, then name_len_and_hash as
// a number (so by hash, then length); seen in the test images.
static int
compare_drec_hashed(const void* _key, uint16 keyLength, const void* _target,
	void* context)
{
	int result = apfs_compare_j_key(_key, sizeof(apfs_j_key), _target,
		context);
	if (result != 0)
		return result;
	if (keyLength < sizeof(apfs_j_drec_hashed_key))
		return -1;

	uint32 key = B_LENDIAN_TO_HOST_INT32(
		((const apfs_j_drec_hashed_key*)_key)->name_len_and_hash);
	uint32 target = B_LENDIAN_TO_HOST_INT32(
		((const apfs_j_drec_hashed_key*)_target)->name_len_and_hash);
	if (key != target)
		return key < target ? -1 : 1;
	return 0;
}


static bool
names_match(const char* a, const char* b, bool ignoreCase)
{
	if (!ignoreCase)
		return strcmp(a, b) == 0;

	// ASCII case only for now; Unicode case folding comes with hashed
	// lookups (milestone 3).
	for (;; a++, b++) {
		char x = *a;
		char y = *b;
		if (x >= 'A' && x <= 'Z')
			x += 'a' - 'A';
		if (y >= 'A' && y <= 'Z')
			y += 'a' - 'A';
		if (x != y)
			return false;
		if (x == '\0')
			return true;
	}
}


status_t
DirectoryIterator::Lookup(const char* name, ino_t* _id)
{
	if (fStatus != B_OK)
		return fStatus;

	if (fHashedNames && is_ascii(name))
		return _LookupHashed(name, _id);
	return _LookupScan(name, _id);
}


status_t
DirectoryIterator::_LookupHashed(const char* name, ino_t* _id)
{
	bool ignoreCase = !fDirectory->GetVolume()->IsCaseSensitive();
	uint32 hash = ascii_name_hash(name, ignoreCase);

	apfs_j_drec_hashed_key target;
	target.hdr.Set(fDirectory->ID(), APFS_TYPE_DIR_REC);
	target.name_len_and_hash = B_HOST_TO_LENDIAN_INT32(
		hash << APFS_J_DREC_HASH_SHIFT);

	fHaveLast = false;
	fPending = false;
	status_t status = fIterator->Seek(&target, compare_drec_hashed, NULL);
	if (status != B_OK)
		return status;

	// Every entry whose name hashes the same comes next; compare those.
	char entry[B_FILE_NAME_LENGTH];
	while (true) {
		size_t length = sizeof(entry);
		ino_t id;
		status = GetNext(entry, &length, &id);
		if (status == B_BUFFER_OVERFLOW) {
			fPending = false;
			if (fLastHash != hash)
				return B_ENTRY_NOT_FOUND;
			continue;
		}
		if (status != B_OK)
			return status;
		if (fLastHash != hash)
			return B_ENTRY_NOT_FOUND;

		if (names_match(entry, name, ignoreCase)) {
			*_id = id;
			return B_OK;
		}
	}
}


status_t
DirectoryIterator::_LookupScan(const char* name, ino_t* _id)
{
	bool ignoreCase = !fDirectory->GetVolume()->IsCaseSensitive();
	status_t status = Rewind();
	if (status != B_OK)
		return status;

	// Non-ASCII names need Unicode NFD normalization and case folding to
	// compute their hash, so they are found by scanning. Exact (byte)
	// matches are found; differently normalized spellings are not yet.
	char entry[B_FILE_NAME_LENGTH];
	ino_t caseMatch = -1;
	while (true) {
		size_t length = sizeof(entry);
		ino_t id;
		status = GetNext(entry, &length, &id);
		if (status == B_BUFFER_OVERFLOW) {
			// Longer than any name the caller can ask for; skip it.
			fPending = false;
			continue;
		}
		if (status != B_OK)
			break;

		if (strcmp(entry, name) == 0) {
			*_id = id;
			return B_OK;
		}
		if (ignoreCase && caseMatch < 0 && names_match(entry, name, true))
			caseMatch = id;
	}

	if (caseMatch >= 0) {
		*_id = caseMatch;
		return B_OK;
	}
	return status == B_ENTRY_NOT_FOUND ? B_ENTRY_NOT_FOUND : status;
}


// #pragma mark - AttributeIterator


AttributeIterator::AttributeIterator(Inode* inode)
	:
	fInode(inode),
	fIterator(NULL),
	fStatus(B_OK)
{
	fIterator = new(std::nothrow) BTreeIterator(
		inode->GetVolume()->FileSystemTree());
	if (fIterator == NULL) {
		fStatus = B_NO_MEMORY;
		return;
	}
	fStatus = fIterator->InitCheck();
	if (fStatus == B_OK)
		fStatus = Rewind();
}


AttributeIterator::~AttributeIterator()
{
	delete fIterator;
}


status_t
AttributeIterator::Rewind()
{
	apfs_j_key target;
	target.Set(fInode->ID(), APFS_TYPE_XATTR);
	return fIterator->Seek(&target, apfs_compare_j_key, NULL);
}


status_t
AttributeIterator::GetNext(char* name, size_t* _length)
{
	if (fStatus != B_OK)
		return fStatus;

	while (true) {
		const uint8* key;
		const uint8* value;
		uint16 keyLength;
		uint16 valueLength;
		status_t status = fIterator->GetNext(&key, &keyLength, &value,
			&valueLength);
		if (status != B_OK)
			return status;

		const apfs_j_xattr_key* xattrKey = (const apfs_j_xattr_key*)key;
		if (keyLength < sizeof(apfs_j_xattr_key)
			|| xattrKey->hdr.ObjectID() != (uint64)fInode->ID()
			|| xattrKey->hdr.Type() != APFS_TYPE_XATTR)
			return B_ENTRY_NOT_FOUND;

		size_t nameLength = xattrKey->NameLength();
		if (sizeof(apfs_j_xattr_key) + nameLength > keyLength)
			return B_BAD_DATA;
		const apfs_j_xattr_val* xattr = (const apfs_j_xattr_val*)value;
		if (value != NULL && valueLength >= sizeof(apfs_j_xattr_val)
			&& (xattr->Flags() & APFS_XATTR_FILE_SYSTEM_OWNED) != 0)
			continue;

		// The stored length counts the final null.
		if (nameLength > 0 && xattrKey->name[nameLength - 1] == '\0')
			nameLength--;

		// A compressed file's data lives in these two; macOS does not show
		// them either.
		if (fInode->IsCompressed()
			&& ((nameLength == strlen(APFS_DECMPFS_ATTRIBUTE_NAME)
					&& memcmp(xattrKey->name, APFS_DECMPFS_ATTRIBUTE_NAME,
						nameLength) == 0)
				|| (nameLength == strlen(APFS_RESOURCE_FORK_NAME)
					&& memcmp(xattrKey->name, APFS_RESOURCE_FORK_NAME,
						nameLength) == 0)))
			continue;

		if (nameLength + 1 > *_length) {
			// Longer than an attribute name can be here; skip it.
			continue;
		}

		memcpy(name, xattrKey->name, nameLength);
		name[nameLength] = '\0';
		*_length = nameLength;
		return B_OK;
	}
}
