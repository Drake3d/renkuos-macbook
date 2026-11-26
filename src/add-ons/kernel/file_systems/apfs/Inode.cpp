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
#include "Volume.h"


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
	fReportedCompressed(false)
{
}


Inode::~Inode()
{
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
		char target[B_PATH_NAME_LENGTH];
		size_t length = sizeof(target);
		if (ReadLink(target, &length) == B_OK)
			fSize = length;
	}

	return B_OK;
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
	size_t wanted = *_length;
	*_length = 0;

	if (IsDirectory())
		return B_IS_A_DIRECTORY;
	if (pos < 0)
		return B_BAD_VALUE;
	if (IsCompressed()) {
		if (!fReportedCompressed) {
			INFORM("inode %" B_PRIdINO " is compressed; compressed files "
				"cannot be read yet\n", fID);
			fReportedCompressed = true;
		}
		return B_NOT_SUPPORTED;
	}
	if (pos >= fSize || wanted == 0)
		return B_OK;

	off_t end = pos + wanted;
	if (end > fSize || end < pos)
		end = fSize;

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
			target.hdr.Set(fPrivateID, APFS_TYPE_FILE_EXTENT);
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
				if (found->hdr.ObjectID() == fPrivateID
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
		if ((xattr->Flags() & APFS_XATTR_DATA_EMBEDDED) == 0)
			return B_NOT_SUPPORTED;

		size_t length = xattr->DataLength();
		if (sizeof(apfs_j_xattr_val) + length > valueLength)
			return B_BAD_DATA;
		if (length > *_length)
			return B_BUFFER_OVERFLOW;

		memcpy(data, xattr->xdata, length);
		*_length = length;
		return B_OK;
	}
}


status_t
Inode::ReadLink(char* buffer, size_t* _bufferSize)
{
	if (!IsSymlink())
		return B_BAD_VALUE;

	// The target is the SYMLINK_EA_NAME attribute: a null-terminated path.
	char target[B_PATH_NAME_LENGTH + 1];
	size_t length = sizeof(target) - 1;
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

	bool ignoreCase = !fDirectory->GetVolume()->IsCaseSensitive();
	status_t status = Rewind();
	if (status != B_OK)
		return status;

	// A linear scan; milestone 3 replaces it with a hashed search.
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
