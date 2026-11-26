/*
 * Copyright 2026, RenkuOS contributors.
 * Distributed under the terms of the MIT License.
 */
#ifndef APFS_DECMPFS_H
#define APFS_DECMPFS_H


#include "apfs.h"


class Inode;
struct apfs_attribute;


// macOS transparent file compression (decmpfs). Not described in the APFS
// reference; every layout below was checked against macOS itself: files
// made by macOS (ditto, system files) and files built by hand that macOS
// then read back correctly.
//
// com.apple.decmpfs attribute: 16-byte header (magic 'fpmc', type,
// uncompressed size), then for the "in attribute" types the data.
// Resource-fork types keep the data in the com.apple.ResourceFork
// attribute: a table of 32-bit little-endian offsets (one per 64 KiB chunk,
// then the end; the first equals the table size), then the chunks.
#define APFS_DECMPFS_MAGIC				0x636d7066	// 'fpmc' on disk
#define APFS_DECMPFS_HEADER_SIZE		16
#define APFS_DECMPFS_CHUNK_SIZE			65536

#define APFS_DECMPFS_RAW_ATTRIBUTE		1	// uncompressed, in the attribute
#define APFS_DECMPFS_ZLIB_ATTRIBUTE		3	// zlib; 0xff first = stored raw
#define APFS_DECMPFS_ZLIB_FORK			4	// zlib in a classic fork (refused)
#define APFS_DECMPFS_LZVN_ATTRIBUTE		7	// LZVN; 0x06 first = stored raw
#define APFS_DECMPFS_LZVN_FORK			8	// LZVN chunks, offset table
#define APFS_DECMPFS_LZFSE_ATTRIBUTE	11	// LZFSE
#define APFS_DECMPFS_LZFSE_FORK			12	// LZFSE chunks, offset table

#define APFS_DECMPFS_ATTRIBUTE_NAME		"com.apple.decmpfs"
#define APFS_RESOURCE_FORK_NAME			"com.apple.ResourceFork"


class Decmpfs {
public:
								Decmpfs(Inode* inode);
								~Decmpfs();

			// Reads the header; the data is only read when needed.
			status_t			Init();

			uint32				Type() const { return fType; }
			off_t				Size() const { return fSize; }
			bool				IsSupported() const;

			// Decompresses into a kernel buffer.
			status_t			Read(off_t pos, uint8* buffer, size_t* _length);

private:
			status_t			_PrepareBuffers();
			status_t			_LoadOffsets();
			status_t			_LoadChunk(uint32 index);
			status_t			_Decode(const uint8* input, size_t inputLength,
									uint8* output, size_t outputLength);
			status_t			_DecodeLZVN(const uint8* input,
									size_t inputLength, uint8* output,
									size_t outputLength);
			status_t			_DecodeLZFSE(const uint8* input,
									size_t inputLength, uint8* output,
									size_t outputLength);
			status_t			_DecodeZlib(const uint8* input,
									size_t inputLength, uint8* output,
									size_t outputLength);

			bool				_InAttribute() const;
			uint32				_ChunkCount() const;

			Inode*				fInode;
			mutex				fLock;
			uint32				fType;
			off_t				fSize;

			// The decmpfs attribute (header and, for some types, the data):
			// embedded in its record or kept in a data stream.
			apfs_attribute*		fHeader;
			uint8*				fHeaderData;
			size_t				fHeaderSize;
			bool				fHeaderAllocated;
			// The resource fork attribute and its offset table.
			apfs_attribute*		fFork;
			uint32*				fOffsets;

			// The chunk decoded last, the compressed input, the LZFSE
			// decoder's work space.
			uint8*				fChunk;
			uint32				fChunkIndex;
			size_t				fChunkLength;
			uint8*				fInput;
			void*				fScratch;
			bool				fReported;
};


#endif	// APFS_DECMPFS_H
