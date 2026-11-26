/*
 * Copyright 2026, RenkuOS contributors.
 * Distributed under the terms of the MIT License.
 */
#ifndef APFS_INODE_H
#define APFS_INODE_H


#include "apfs.h"


class BTreeIterator;
class Decmpfs;
class Volume;


// An extended attribute found by name: its value is either stored in the
// record (embedded) or in a data stream of its own.
struct apfs_attribute {
	bool		embedded;
	uint64		stream_id;
	off_t		size;
	uint8		data[APFS_XATTR_MAX_EMBEDDED_SIZE];
};


// Key comparisons for file-system records ("File-System Objects": object
// id, then type, then type-specific fields).

// Equal when id and type match and the key has nothing beyond j_key_t;
// longer keys of the same id and type sort after the target. Seeking to
// (id, type) with it lands on the first record of that kind.
int apfs_compare_j_key(const void* key, uint16 keyLength, const void* target,
	void* context);

// For j_file_extent_key_t targets: id, type, then logical address.
int apfs_compare_file_extent_key(const void* key, uint16 keyLength,
	const void* target, void* context);


// A file-system object (file, directory, symlink, ...), read from its
// inode record.
class Inode {
public:
								Inode(Volume* volume, ino_t id);
								~Inode();

			status_t			Init();

			Volume*				GetVolume() const { return fVolume; }
			ino_t				ID() const { return fID; }
			ino_t				ParentID() const { return fParentID; }
			uint64				PrivateID() const { return fPrivateID; }

			mode_t				Mode() const { return fMode; }
			bool				IsDirectory() const
									{ return (fMode & APFS_S_IFMT)
										== APFS_S_IFDIR; }
			bool				IsFile() const
									{ return (fMode & APFS_S_IFMT)
										== APFS_S_IFREG; }
			bool				IsSymlink() const
									{ return (fMode & APFS_S_IFMT)
										== APFS_S_IFLNK; }
			bool				IsCompressed() const
									{ return (fBSDFlags
										& APFS_BSD_UF_COMPRESSED) != 0; }

			off_t				Size() const { return fSize; }
			off_t				AllocatedSize() const
									{ return fAllocatedSize; }
			uid_t				UserID() const { return fUserID; }
			gid_t				GroupID() const { return fGroupID; }
			int32				LinkCount() const { return fLinkCount; }

			void				GetCreationTime(struct timespec& time) const
									{ _ToTimespec(fCreationTime, time); }
			void				GetModificationTime(
									struct timespec& time) const
									{ _ToTimespec(fModificationTime, time); }
			void				GetChangeTime(struct timespec& time) const
									{ _ToTimespec(fChangeTime, time); }
			void				GetAccessTime(struct timespec& time) const
									{ _ToTimespec(fAccessTime, time); }

			status_t			CheckPermissions(int accessMode) const;

			// Regular files are read through the file cache; uncompressed
			// ones also have a file map (their data is on disk as is).
			void*				FileCache() const { return fCache; }
			void*				FileMap() const { return fMap; }
			Decmpfs*			Compression() const { return fDecmpfs; }

			// Decompresses part of a compressed file into a kernel buffer.
			status_t			ReadCompressed(off_t pos, uint8* buffer,
									size_t* _length);

			// Where file offset pos is on the device: *_diskOffset is a byte
			// offset on the container's device, or -1 for a hole;
			// *_length is how far that run goes (not past the file's end).
			status_t			FindExtent(off_t pos, off_t* _diskOffset,
									off_t* _length);

			// Reads through the file cache when there is one. buffer may be
			// a userland address.
			status_t			Read(off_t pos, void* buffer, size_t* _length);
			// Reads extent by extent without the file cache.
			status_t			ReadAt(off_t pos, void* buffer,
									size_t* _length);
			status_t			ReadLink(char* buffer, size_t* _bufferSize);

			// Finds an extended attribute stored in the record itself.
			status_t			GetEmbeddedAttribute(const char* name,
									uint8* data, size_t* _length);

			// Finds an extended attribute, embedded or stream-backed.
			status_t			FindAttribute(const char* name,
									apfs_attribute* attribute);
			// buffer may be a userland address.
			status_t			ReadAttribute(const apfs_attribute& attribute,
									off_t pos, void* buffer, size_t* _length);

private:
	static	void				_ToTimespec(uint64 nanoseconds,
									struct timespec& time);
			void				_ParseExtendedFields(const uint8* data,
									size_t length);
			status_t			_ReadStream(uint64 streamID, off_t size,
									off_t pos, void* buffer, size_t* _length);

			Volume*				fVolume;
			ino_t				fID;
			ino_t				fParentID;
			uint64				fPrivateID;
			mode_t				fMode;
			uint32				fBSDFlags;
			uint64				fInternalFlags;
			uid_t				fUserID;
			gid_t				fGroupID;
			int32				fLinkCount;
			off_t				fSize;
			off_t				fAllocatedSize;
			uint64				fCreationTime;
			uint64				fModificationTime;
			uint64				fChangeTime;
			uint64				fAccessTime;
			bool				fReportedCompressed;
			void*				fCache;
			void*				fMap;
			Decmpfs*			fDecmpfs;
};


// The entries of one directory, in on-disk order.
class DirectoryIterator {
public:
								DirectoryIterator(Inode* directory);
								~DirectoryIterator();

			status_t			InitCheck() const { return fStatus; }

			// name receives a null-terminated UTF-8 name; *_length is the
			// buffer size in, the name length (without the null) out.
			status_t			GetNext(char* name, size_t* _length,
									ino_t* _id);
			// Makes the next GetNext() return the last entry again (for a
			// caller whose buffer was full).
			void				Unget() { fPending = fHaveLast; }
			status_t			Rewind();

			// Finds an entry by name: exactly, or ignoring ASCII case on a
			// case-insensitive volume. ASCII names use the name hash; other
			// names are found by scanning the directory.
			status_t			Lookup(const char* name, ino_t* _id);

private:
			status_t			_ReadEntry();
			status_t			_LookupHashed(const char* name, ino_t* _id);
			status_t			_LookupScan(const char* name, ino_t* _id);

			Inode*				fDirectory;
			BTreeIterator*		fIterator;
			bool				fHashedNames;
			status_t			fStatus;

			// The last entry read, for Unget().
			char				fLastName[1024];
			size_t				fLastLength;
			ino_t				fLastID;
			uint32				fLastHash;
			bool				fHaveLast;
			bool				fPending;
};


// The extended attributes of one node, in on-disk order. Attributes the
// file system owns (XATTR_FILE_SYSTEM_OWNED, such as a symlink's target)
// are skipped.
class AttributeIterator {
public:
								AttributeIterator(Inode* inode);
								~AttributeIterator();

			status_t			InitCheck() const { return fStatus; }
			status_t			GetNext(char* name, size_t* _length);
			status_t			Rewind();

private:
			Inode*				fInode;
			BTreeIterator*		fIterator;
			status_t			fStatus;
};


#endif	// APFS_INODE_H
