/*
 * Copyright 2026, RenkuOS contributors.
 * Distributed under the terms of the MIT License.
 *
 * Reading macOS compressed files (decmpfs). See Decmpfs.h for the layouts
 * and how they were checked. LZVN and LZFSE come from Apple's BSD-licensed
 * reference decoder (lzfse/, unmodified); zlib from the kernel's zlib.
 */


#include "Decmpfs.h"

#include <zlib.h>

#include "Inode.h"
#include "Volume.h"

// Apple's decoder headers, kept unmodified: silence their warnings here.
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wsign-compare"
#pragma GCC diagnostic ignored "-Wcast-align"
extern "C" {
#include "lzfse.h"
#include "lzvn_decode_base.h"
}
#pragma GCC diagnostic pop


static const uint32 kChunkSize = APFS_DECMPFS_CHUNK_SIZE;
static const uint32 kNoChunk = 0xffffffff;

// Data kept in the attribute is decoded whole; the attribute holds at most
// XATTR_MAX_EMBEDDED_SIZE bytes, which do not expand without limit.
static const off_t kMaxAttributeDataSize = 16 * 1024 * 1024;


static void*
zlib_alloc(void* /*opaque*/, uInt items, uInt size)
{
	return malloc((size_t)items * size);
}


static void
zlib_free(void* /*opaque*/, void* address)
{
	free(address);
}


Decmpfs::Decmpfs(Inode* inode)
	:
	fInode(inode),
	fType(0),
	fSize(0),
	fHeader(NULL),
	fHeaderData(NULL),
	fHeaderSize(0),
	fHeaderAllocated(false),
	fFork(NULL),
	fChunkOffsets(NULL),
	fChunkSizes(NULL),
	fChunk(NULL),
	fChunkIndex(kNoChunk),
	fChunkLength(0),
	fInput(NULL),
	fScratch(NULL),
	fReported(false)
{
	mutex_init(&fLock, "apfs decmpfs");
}


Decmpfs::~Decmpfs()
{
	if (fHeaderAllocated)
		free(fHeaderData);
	free(fHeader);
	free(fFork);
	free(fChunkOffsets);
	free(fChunkSizes);
	free(fChunk);
	free(fInput);
	free(fScratch);
	mutex_destroy(&fLock);
}


bool
Decmpfs::_InAttribute() const
{
	return fType == APFS_DECMPFS_RAW_ATTRIBUTE
		|| fType == APFS_DECMPFS_ZLIB_ATTRIBUTE
		|| fType == APFS_DECMPFS_LZVN_ATTRIBUTE
		|| fType == APFS_DECMPFS_LZFSE_ATTRIBUTE;
}


bool
Decmpfs::IsSupported() const
{
	return _InAttribute() || fType == APFS_DECMPFS_ZLIB_FORK
		|| fType == APFS_DECMPFS_LZVN_FORK || fType == APFS_DECMPFS_LZFSE_FORK;
}


uint32
Decmpfs::_ChunkCount() const
{
	return (fSize + kChunkSize - 1) / kChunkSize;
}


status_t
Decmpfs::Init()
{
	fHeader = (apfs_attribute*)malloc(sizeof(apfs_attribute));
	if (fHeader == NULL)
		return B_NO_MEMORY;

	status_t status = fInode->FindAttribute(APFS_DECMPFS_ATTRIBUTE_NAME,
		fHeader);
	if (status != B_OK) {
		ERROR("inode %" B_PRIdINO ": compressed, but no %s attribute\n",
			fInode->ID(), APFS_DECMPFS_ATTRIBUTE_NAME);
		return B_BAD_DATA;
	}
	if (fHeader->size < APFS_DECMPFS_HEADER_SIZE
		|| fHeader->size > APFS_DECMPFS_HEADER_SIZE + kMaxAttributeDataSize) {
		ERROR("inode %" B_PRIdINO ": bad compression header\n", fInode->ID());
		return B_BAD_DATA;
	}

	if (fHeader->embedded) {
		fHeaderData = fHeader->data;
		fHeaderSize = fHeader->size;
	} else {
		// Kept in a data stream (seen on attributes written by tools).
		fHeaderData = (uint8*)malloc(fHeader->size);
		if (fHeaderData == NULL)
			return B_NO_MEMORY;
		fHeaderAllocated = true;
		size_t length = fHeader->size;
		status = fInode->ReadAttribute(*fHeader, 0, fHeaderData, &length);
		if (status == B_OK && length != (size_t)fHeader->size)
			status = B_BAD_DATA;
		if (status != B_OK) {
			ERROR("inode %" B_PRIdINO ": cannot read the compression "
				"header\n", fInode->ID());
			return status;
		}
		fHeaderSize = length;
	}

	uint32 magic;
	uint32 type;
	uint64 size;
	memcpy(&magic, fHeaderData, sizeof(magic));
	memcpy(&type, fHeaderData + 4, sizeof(type));
	memcpy(&size, fHeaderData + 8, sizeof(size));
	if (B_LENDIAN_TO_HOST_INT32(magic) != APFS_DECMPFS_MAGIC) {
		ERROR("inode %" B_PRIdINO ": bad compression header magic\n",
			fInode->ID());
		return B_BAD_DATA;
	}

	fType = B_LENDIAN_TO_HOST_INT32(type);
	fSize = B_LENDIAN_TO_HOST_INT64(size);
	if (fSize < 0)
		return B_BAD_DATA;
	if (_InAttribute() && fSize > kMaxAttributeDataSize)
		return B_BAD_DATA;
	return B_OK;
}


status_t
Decmpfs::_PrepareBuffers()
{
	size_t chunkBuffer = _InAttribute() ? (size_t)fSize : kChunkSize;
	if (chunkBuffer == 0)
		chunkBuffer = 1;

	if (fChunk == NULL) {
		fChunk = (uint8*)malloc(chunkBuffer);
		if (fChunk == NULL)
			return B_NO_MEMORY;
	}
	if (!_InAttribute() && fInput == NULL) {
		// A chunk stored raw is one byte longer than its data.
		fInput = (uint8*)malloc(kChunkSize + 64);
		if (fInput == NULL)
			return B_NO_MEMORY;
	}
	if ((fType == APFS_DECMPFS_LZFSE_ATTRIBUTE
			|| fType == APFS_DECMPFS_LZFSE_FORK) && fScratch == NULL) {
		fScratch = malloc(lzfse_decode_scratch_size() + 1);
		if (fScratch == NULL)
			return B_NO_MEMORY;
	}
	return B_OK;
}


status_t
Decmpfs::_LoadOffsets()
{
	if (fChunkOffsets != NULL)
		return B_OK;

	if (fFork == NULL) {
		fFork = (apfs_attribute*)malloc(sizeof(apfs_attribute));
		if (fFork == NULL)
			return B_NO_MEMORY;
		status_t status = fInode->FindAttribute(APFS_RESOURCE_FORK_NAME,
			fFork);
		if (status != B_OK) {
			ERROR("inode %" B_PRIdINO ": compressed, but no resource fork\n",
				fInode->ID());
			free(fFork);
			fFork = NULL;
			return B_BAD_DATA;
		}
	}

	uint32 count = _ChunkCount();
	fChunkOffsets = (uint32*)malloc(count * sizeof(uint32) + 1);
	fChunkSizes = (uint32*)malloc(count * sizeof(uint32) + 1);
	if (fChunkOffsets == NULL || fChunkSizes == NULL) {
		free(fChunkOffsets);
		free(fChunkSizes);
		fChunkOffsets = fChunkSizes = NULL;
		return B_NO_MEMORY;
	}

	status_t status = fType == APFS_DECMPFS_ZLIB_FORK
		? _LoadClassicFork(count) : _LoadOffsetTable(count);

	// Every chunk lies in the fork and is no bigger than a raw chunk.
	for (uint32 i = 0; status == B_OK && i < count; i++) {
		if (fChunkSizes[i] > kChunkSize + 64
			|| fChunkOffsets[i] > fFork->size
			|| fChunkSizes[i] > fFork->size - fChunkOffsets[i])
			status = B_BAD_DATA;
	}

	if (status != B_OK) {
		ERROR("inode %" B_PRIdINO ": bad compressed chunk table (type %"
			B_PRIu32 ")\n", fInode->ID(), fType);
		free(fChunkOffsets);
		free(fChunkSizes);
		fChunkOffsets = fChunkSizes = NULL;
	}
	return status;
}


//!	Types 8 and 12: offsets from the start of the fork, then the end.
status_t
Decmpfs::_LoadOffsetTable(uint32 count)
{
	size_t tableSize = (count + 1) * sizeof(uint32);
	uint32* table = (uint32*)malloc(tableSize);
	if (table == NULL)
		return B_NO_MEMORY;

	size_t length = tableSize;
	status_t status = fInode->ReadAttribute(*fFork, 0, table, &length);
	if (status == B_OK && length != tableSize)
		status = B_BAD_DATA;

	// Offsets grow and the first is the table size.
	for (uint32 i = 0; status == B_OK && i <= count; i++) {
		table[i] = B_LENDIAN_TO_HOST_INT32(table[i]);
		if ((i == 0 && table[0] != tableSize)
			|| (i > 0 && table[i] < table[i - 1]))
			status = B_BAD_DATA;
	}
	for (uint32 i = 0; status == B_OK && i < count; i++) {
		fChunkOffsets[i] = table[i];
		fChunkSizes[i] = table[i + 1] - table[i];
	}

	free(table);
	return status;
}


//!	Type 4: the 'cmpf' resource of a classic resource fork.
status_t
Decmpfs::_LoadClassicFork(uint32 count)
{
	uint32 header[4];
	size_t length = sizeof(header);
	status_t status = fInode->ReadAttribute(*fFork, 0, header, &length);
	if (status != B_OK)
		return status;
	if (length != sizeof(header))
		return B_BAD_DATA;
	uint32 dataOffset = B_BENDIAN_TO_HOST_INT32(header[0]);
	uint32 dataLength = B_BENDIAN_TO_HOST_INT32(header[2]);
	if (dataOffset < sizeof(header) || dataLength < 8
		|| dataOffset > fFork->size || dataLength > fFork->size - dataOffset)
		return B_BAD_DATA;

	// The resource's length, then its chunk count.
	uint32 start[2];
	length = sizeof(start);
	status = fInode->ReadAttribute(*fFork, dataOffset, start, &length);
	if (status != B_OK)
		return status;
	if (length != sizeof(start))
		return B_BAD_DATA;
	uint32 resourceLength = B_BENDIAN_TO_HOST_INT32(start[0]);
	uint32 resourceCount = B_LENDIAN_TO_HOST_INT32(start[1]);
	if (resourceLength > dataLength - 4 || resourceCount != count
		|| resourceLength < 4 + (uint64)count * 8)
		return B_BAD_DATA;

	size_t tableSize = count * 2 * sizeof(uint32);
	uint32* table = (uint32*)malloc(tableSize + 1);
	if (table == NULL)
		return B_NO_MEMORY;
	uint32 resource = dataOffset + 4;
	length = tableSize;
	status = fInode->ReadAttribute(*fFork, resource + 4, table, &length);
	if (status == B_OK && length != tableSize)
		status = B_BAD_DATA;

	for (uint32 i = 0; status == B_OK && i < count; i++) {
		uint32 offset = B_LENDIAN_TO_HOST_INT32(table[i * 2]);
		uint32 size = B_LENDIAN_TO_HOST_INT32(table[i * 2 + 1]);
		if (offset < 4 + count * 8 || offset > resourceLength
			|| size > resourceLength - offset) {
			status = B_BAD_DATA;
			break;
		}
		fChunkOffsets[i] = resource + offset;
		fChunkSizes[i] = size;
	}

	free(table);
	return status;
}


status_t
Decmpfs::_DecodeLZVN(const uint8* input, size_t inputLength, uint8* output,
	size_t outputLength)
{
	// A chunk that starts with 0x06 (the LZVN end-of-stream opcode) holds
	// its data uncompressed after that byte.
	if (inputLength > 0 && input[0] == 0x06) {
		if (inputLength - 1 < outputLength)
			return B_BAD_DATA;
		memcpy(output, input + 1, outputLength);
		return B_OK;
	}

	lzvn_decoder_state state;
	memset(&state, 0, sizeof(state));
	state.src = input;
	state.src_end = input + inputLength;
	state.dst = output;
	state.dst_begin = output;
	state.dst_end = output + outputLength;
	lzvn_decode(&state);

	if ((size_t)(state.dst - output) != outputLength)
		return B_BAD_DATA;
	return B_OK;
}


status_t
Decmpfs::_DecodeLZFSE(const uint8* input, size_t inputLength, uint8* output,
	size_t outputLength)
{
	size_t decoded = lzfse_decode_buffer(output, outputLength, input,
		inputLength, fScratch);
	if (decoded != outputLength)
		return B_BAD_DATA;
	return B_OK;
}


status_t
Decmpfs::_DecodeZlib(const uint8* input, size_t inputLength, uint8* output,
	size_t outputLength)
{
	// 0xff (low nibble 0xf, not a zlib method) marks data stored raw.
	if (inputLength > 0 && (input[0] & 0x0f) == 0x0f) {
		if (inputLength - 1 < outputLength)
			return B_BAD_DATA;
		memcpy(output, input + 1, outputLength);
		return B_OK;
	}

	z_stream stream;
	memset(&stream, 0, sizeof(stream));
	stream.zalloc = zlib_alloc;
	stream.zfree = zlib_free;
	stream.next_in = (Bytef*)input;
	stream.avail_in = inputLength;
	stream.next_out = output;
	stream.avail_out = outputLength;

	if (inflateInit(&stream) != Z_OK)
		return B_NO_MEMORY;
	int result = inflate(&stream, Z_FINISH);
	size_t decoded = outputLength - stream.avail_out;
	inflateEnd(&stream);

	if ((result != Z_STREAM_END && result != Z_OK) || decoded != outputLength)
		return B_BAD_DATA;
	return B_OK;
}


status_t
Decmpfs::_Decode(const uint8* input, size_t inputLength, uint8* output,
	size_t outputLength)
{
	switch (fType) {
		case APFS_DECMPFS_RAW_ATTRIBUTE:
			if (inputLength < outputLength)
				return B_BAD_DATA;
			memcpy(output, input, outputLength);
			return B_OK;
		case APFS_DECMPFS_ZLIB_ATTRIBUTE:
		case APFS_DECMPFS_ZLIB_FORK:
			return _DecodeZlib(input, inputLength, output, outputLength);
		case APFS_DECMPFS_LZVN_ATTRIBUTE:
		case APFS_DECMPFS_LZVN_FORK:
			return _DecodeLZVN(input, inputLength, output, outputLength);
		case APFS_DECMPFS_LZFSE_ATTRIBUTE:
		case APFS_DECMPFS_LZFSE_FORK:
			return _DecodeLZFSE(input, inputLength, output, outputLength);
		default:
			return B_NOT_SUPPORTED;
	}
}


status_t
Decmpfs::_LoadChunk(uint32 index)
{
	if (index == fChunkIndex)
		return B_OK;
	fChunkIndex = kNoChunk;

	status_t status = _PrepareBuffers();
	if (status != B_OK)
		return status;

	if (_InAttribute()) {
		// One "chunk": the whole file, from the data after the header.
		status = _Decode(fHeaderData + APFS_DECMPFS_HEADER_SIZE,
			fHeaderSize - APFS_DECMPFS_HEADER_SIZE, fChunk, fSize);
		if (status != B_OK)
			return status;
		fChunkLength = fSize;
		fChunkIndex = 0;
		return B_OK;
	}

	status = _LoadOffsets();
	if (status != B_OK)
		return status;
	if (index >= _ChunkCount())
		return B_BAD_VALUE;

	size_t inputLength = fChunkSizes[index];
	size_t length = inputLength;
	status = fInode->ReadAttribute(*fFork, fChunkOffsets[index], fInput,
		&length);
	if (status == B_OK && length != inputLength)
		status = B_BAD_DATA;
	if (status != B_OK)
		return status;

	size_t outputLength = kChunkSize;
	if ((off_t)index * kChunkSize + (off_t)outputLength > fSize)
		outputLength = fSize - (off_t)index * kChunkSize;

	status = _Decode(fInput, inputLength, fChunk, outputLength);
	if (status != B_OK)
		return status;

	fChunkLength = outputLength;
	fChunkIndex = index;
	return B_OK;
}


status_t
Decmpfs::Read(off_t pos, uint8* buffer, size_t* _length)
{
	size_t wanted = *_length;
	*_length = 0;

	if (!IsSupported()) {
		if (!fReported) {
			INFORM("inode %" B_PRIdINO ": compression type %" B_PRIu32
				" is not supported\n", fInode->ID(), fType);
			fReported = true;
		}
		return B_NOT_SUPPORTED;
	}
	if (pos < 0)
		return B_BAD_VALUE;
	if (pos >= fSize || wanted == 0)
		return B_OK;
	if ((off_t)wanted > fSize - pos)
		wanted = fSize - pos;

	MutexLocker locker(fLock);

	while (*_length < wanted) {
		off_t offset = pos + *_length;
		uint32 index = _InAttribute() ? 0 : offset / kChunkSize;
		off_t chunkStart = _InAttribute() ? 0 : (off_t)index * kChunkSize;

		status_t status = _LoadChunk(index);
		if (status != B_OK) {
			if (status == B_BAD_DATA && !fReported) {
				ERROR("inode %" B_PRIdINO ": damaged compressed data (type %"
					B_PRIu32 ", chunk %" B_PRIu32 ")\n", fInode->ID(), fType,
					index);
				fReported = true;
			}
			return *_length > 0 ? B_OK : status;
		}

		size_t within = offset - chunkStart;
		size_t copy = fChunkLength - within;
		if (copy > wanted - *_length)
			copy = wanted - *_length;
		memcpy(buffer + *_length, fChunk + within, copy);
		*_length += copy;
	}
	return B_OK;
}
