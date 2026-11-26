/*
 * Copyright 2026, RenkuOS contributors.
 * Distributed under the terms of the MIT License.
 *
 * Container superblock, checkpoint and ephemeral objects.
 * Source: Apple File System Reference, revision 2020-06-22, chapter
 * "Container", section "Mounting an Apple File System Partition" (steps
 * 1-7), and "nx_superblock_t", "checkpoint_map_phys_t".
 */


#include "Container.h"

#include "Checksum.h"
#include "ObjectMap.h"


Container::Container()
	:
	fDevice(-1),
	fDeviceSize(0),
	fBlockSize(0),
	fBlockCount(0),
	fBlockCache(NULL),
	fMappings(NULL),
	fMappingCount(0),
	fObjectMap(NULL)
{
	memset(&fSuperBlock, 0, sizeof(fSuperBlock));
}


Container::~Container()
{
	Close();
}


/*static*/ status_t
Container::Identify(int device, apfs_nx_superblock* superBlock)
{
	// Step 1: block zero holds a copy of the container superblock, possibly
	// an old one. It is enough to recognize the container.
	uint8 first[APFS_NX_MINIMUM_BLOCK_SIZE];
	if (read_pos(device, 0, first, sizeof(first)) != (ssize_t)sizeof(first))
		return B_IO_ERROR;

	const apfs_nx_superblock* candidate = (const apfs_nx_superblock*)first;
	if (candidate->Magic() != APFS_NX_MAGIC
		|| candidate->o.Type() != APFS_OBJECT_TYPE_NX_SUPERBLOCK)
		return B_BAD_VALUE;

	uint32 blockSize = candidate->BlockSize();
	if (blockSize < APFS_NX_MINIMUM_BLOCK_SIZE
		|| blockSize > APFS_NX_MAXIMUM_BLOCK_SIZE
		|| (blockSize & (blockSize - 1)) != 0)
		return B_BAD_VALUE;

	// The checksum covers the whole block.
	uint8* block = (uint8*)malloc(blockSize);
	if (block == NULL)
		return B_NO_MEMORY;
	MemoryDeleter blockDeleter(block);

	if (read_pos(device, 0, block, blockSize) != (ssize_t)blockSize)
		return B_IO_ERROR;
	if (!apfs_object_checksum_valid(block, blockSize))
		return B_BAD_DATA;

	memcpy(superBlock, block, sizeof(apfs_nx_superblock));
	return B_OK;
}


status_t
Container::Open(int device, off_t deviceSize, bool useBlockCache)
{
	apfs_nx_superblock blockZero;
	status_t status = Identify(device, &blockZero);
	if (status != B_OK)
		return status;

	fDevice = device;
	fDeviceSize = deviceSize;
	fBlockSize = blockZero.BlockSize();
	fBlockCount = blockZero.BlockCount();

	if (deviceSize > 0 && blockZero.BlockCount() * fBlockSize
			> (uint64)deviceSize) {
		INFORM("container (%" B_PRIu64 " blocks) is larger than the device "
			"(%" B_PRIdOFF " bytes)\n", blockZero.BlockCount(), deviceSize);
	}

	if (useBlockCache) {
		fBlockCache = block_cache_create(device, blockZero.BlockCount(),
			fBlockSize, true);
		if (fBlockCache == NULL)
			return B_NO_MEMORY;
	}

	status = _FindCheckpoint(device, blockZero);
	if (status != B_OK)
		return status;

	uint64 incompatible = fSuperBlock.IncompatibleFeatures();
	if ((incompatible & APFS_NX_INCOMPAT_VERSION2) == 0) {
		ERROR("container is not APFS version 2 (prerelease APFS from "
			"macOS 10.12 is not supported)\n");
		return B_NOT_SUPPORTED;
	}
	if ((incompatible & APFS_NX_INCOMPAT_FUSION) != 0) {
		ERROR("Fusion containers are not supported\n");
		return B_NOT_SUPPORTED;
	}

	// Step 6: the container object map (a physical object).
	fObjectMap = new(std::nothrow) ObjectMap(this);
	if (fObjectMap == NULL)
		return B_NO_MEMORY;

	status = fObjectMap->SetTo(fSuperBlock.ObjectMap());
	if (status != B_OK) {
		ERROR("cannot read the container object map: %s\n", strerror(status));
		return status;
	}

	TRACE("container: block size %" B_PRIu32 ", %" B_PRIu64 " blocks, "
		"checkpoint xid %" B_PRIu64 ", %" B_PRIu32 " ephemeral objects\n",
		fBlockSize, BlockCount(), TransactionID(), fMappingCount);
	return B_OK;
}


void
Container::Close()
{
	delete fObjectMap;
	fObjectMap = NULL;

	free(fMappings);
	fMappings = NULL;
	fMappingCount = 0;

	if (fBlockCache != NULL) {
		block_cache_delete(fBlockCache, false);
		fBlockCache = NULL;
	}
	fDevice = -1;
}


status_t
Container::ReadBlock(apfs_paddr_t block, void* buffer)
{
	if (block < 0 || (uint64)block >= BlockCount()) {
		ERROR("block %" B_PRId64 " is outside the container\n", block);
		return B_BAD_DATA;
	}

	if (fBlockCache != NULL) {
		const void* data = block_cache_get(fBlockCache, block);
		if (data == NULL)
			return B_IO_ERROR;
		memcpy(buffer, data, fBlockSize);
		block_cache_put(fBlockCache, block);
		return B_OK;
	}

	off_t offset = (off_t)block * fBlockSize;
	if (read_pos(fDevice, offset, buffer, fBlockSize) != (ssize_t)fBlockSize)
		return B_IO_ERROR;
	return B_OK;
}


status_t
Container::ReadObject(apfs_paddr_t block, void* buffer, uint32 expectedType)
{
	status_t status = ReadBlock(block, buffer);
	if (status != B_OK)
		return status;

	if (!apfs_object_checksum_valid(buffer, fBlockSize)) {
		ERROR("bad checksum in block %" B_PRId64 "\n", block);
		return B_BAD_DATA;
	}

	const apfs_obj_phys* header = (const apfs_obj_phys*)buffer;
	if (expectedType != 0 && header->Type() != expectedType) {
		ERROR("block %" B_PRId64 " has object type %#" B_PRIx32 ", expected "
			"%#" B_PRIx32 "\n", block, header->Type(), expectedType);
		return B_BAD_DATA;
	}
	return B_OK;
}


status_t
Container::ResolveEphemeral(apfs_oid_t oid, apfs_paddr_t* _address) const
{
	for (uint32 i = 0; i < fMappingCount; i++) {
		if (fMappings[i].ObjectID() == oid) {
			*_address = fMappings[i].Address();
			return B_OK;
		}
	}
	return B_ENTRY_NOT_FOUND;
}


uint32
Container::CountVolumes() const
{
	uint32 max = fSuperBlock.MaxFileSystems();
	if (max > APFS_NX_MAX_FILE_SYSTEMS)
		max = APFS_NX_MAX_FILE_SYSTEMS;
	return max;
}


status_t
Container::ReadVolumeSuperblock(uint32 index, void* buffer)
{
	if (index >= CountVolumes())
		return B_BAD_INDEX;

	// Step 8: volume superblocks are virtual objects of the container
	// object map.
	apfs_oid_t oid = fSuperBlock.FileSystem(index);
	if (oid == APFS_OID_INVALID)
		return B_ENTRY_NOT_FOUND;

	apfs_omap_val value;
	status_t status = fObjectMap->Lookup(oid, TransactionID(), &value);
	if (status != B_OK)
		return status;

	status = ReadObject(value.Address(), buffer, APFS_OBJECT_TYPE_FS);
	if (status != B_OK)
		return status;

	const apfs_superblock* volume = (const apfs_superblock*)buffer;
	if (volume->Magic() != APFS_MAGIC
		|| volume->o.ObjectID() != oid) {
		ERROR("volume %" B_PRIu32 ": not a volume superblock\n", index);
		return B_BAD_DATA;
	}
	return B_OK;
}


status_t
Container::GetVolumeInfo(uint32 index, apfs_volume_info* info)
{
	uint8* block = (uint8*)malloc(fBlockSize);
	if (block == NULL)
		return B_NO_MEMORY;
	MemoryDeleter blockDeleter(block);

	status_t status = ReadVolumeSuperblock(index, block);
	if (status != B_OK)
		return status;

	const apfs_superblock* volume = (const apfs_superblock*)block;
	info->index = index;
	info->oid = volume->o.ObjectID();
	apfs_omap_val value;
	info->address = fObjectMap->Lookup(info->oid, TransactionID(), &value)
		== B_OK ? value.Address() : 0;
	strlcpy(info->name, (const char*)volume->volname, sizeof(info->name));
	info->role = volume->Role();
	info->incompatible_features = volume->IncompatibleFeatures();
	info->flags = volume->Flags();
	return B_OK;
}


bool
Container::_IsValidSuperBlock(const apfs_nx_superblock* superBlock,
	bool checkpoint) const
{
	if (superBlock->Magic() != APFS_NX_MAGIC
		|| superBlock->o.Type() != APFS_OBJECT_TYPE_NX_SUPERBLOCK
		|| superBlock->BlockSize() != fBlockSize
		|| !apfs_object_checksum_valid(superBlock, fBlockSize))
		return false;

	if (!checkpoint)
		return true;

	uint32 descBlocks = superBlock->DescriptorBlocks();
	return descBlocks != 0
		&& superBlock->DescriptorIndex() < descBlocks
		&& superBlock->DescriptorLength() >= 1
		&& superBlock->DescriptorLength() <= descBlocks
		&& superBlock->MaxFileSystems() <= APFS_NX_MAX_FILE_SYSTEMS;
}


status_t
Container::_FindCheckpoint(int device, const apfs_nx_superblock& blockZero)
{
	// Steps 2-4: scan the checkpoint descriptor area for the valid
	// superblock with the largest transaction id.
	if (!blockZero.DescriptorContiguous()) {
		ERROR("non-contiguous checkpoint descriptor areas are not "
			"supported yet\n");
		return B_NOT_SUPPORTED;
	}

	uint32 descBlocks = blockZero.DescriptorBlocks();
	apfs_paddr_t descBase = blockZero.DescriptorBase();
	if (descBlocks == 0 || descBase <= 0
		|| (uint64)descBase + descBlocks > blockZero.BlockCount()) {
		ERROR("bad checkpoint descriptor area (%" B_PRId64 ", %" B_PRIu32
			" blocks)\n", descBase, descBlocks);
		return B_BAD_DATA;
	}

	uint8* block = (uint8*)malloc(fBlockSize);
	if (block == NULL)
		return B_NO_MEMORY;
	MemoryDeleter blockDeleter(block);

	// Try candidates newest first; a checkpoint whose mapping blocks are
	// damaged is skipped in favor of an older one (step 5).
	apfs_xid_t tried = 0;
	for (uint32 attempt = 0; attempt < descBlocks; attempt++) {
		apfs_xid_t bestXid = 0;
		apfs_paddr_t bestBlock = -1;

		for (uint32 i = 0; i < descBlocks; i++) {
			if (ReadBlock(descBase + i, block) != B_OK)
				continue;

			const apfs_nx_superblock* candidate
				= (const apfs_nx_superblock*)block;
			if (!_IsValidSuperBlock(candidate, true))
				continue;

			apfs_xid_t xid = candidate->o.TransactionID();
			if ((tried == 0 || xid < tried) && xid > bestXid) {
				bestXid = xid;
				bestBlock = descBase + i;
			}
		}

		if (bestBlock < 0)
			break;

		ReadBlock(bestBlock, block);
		const apfs_nx_superblock* best = (const apfs_nx_superblock*)block;
		if (_LoadCheckpointMaps(*best) == B_OK) {
			memcpy(&fSuperBlock, best, sizeof(fSuperBlock));
			fBlockCount = fSuperBlock.BlockCount();
			TRACE("checkpoint: superblock in block %" B_PRId64 ", xid %"
				B_PRIu64 "\n", bestBlock, bestXid);
			return B_OK;
		}

		INFORM("checkpoint xid %" B_PRIu64 " is damaged, trying an older "
			"one\n", bestXid);
		tried = bestXid;
	}

	ERROR("no valid checkpoint found\n");
	return B_BAD_DATA;
}


status_t
Container::_LoadCheckpointMaps(const apfs_nx_superblock& superBlock)
{
	// The checkpoint is nx_xp_desc_len blocks starting at nx_xp_desc_index
	// of the descriptor ring: mapping blocks, then the superblock last.
	uint32 descBlocks = superBlock.DescriptorBlocks();
	apfs_paddr_t descBase = superBlock.DescriptorBase();
	uint32 first = superBlock.DescriptorIndex();
	uint32 mapBlocks = superBlock.DescriptorLength() - 1;
	apfs_xid_t xid = superBlock.o.TransactionID();

	uint8* block = (uint8*)malloc(fBlockSize);
	if (block == NULL)
		return B_NO_MEMORY;
	MemoryDeleter blockDeleter(block);

	size_t perBlock = (fBlockSize - sizeof(apfs_checkpoint_map_phys))
		/ sizeof(apfs_checkpoint_mapping);
	apfs_checkpoint_mapping* mappings = (apfs_checkpoint_mapping*)malloc(
		(mapBlocks > 0 ? mapBlocks : 1) * perBlock
			* sizeof(apfs_checkpoint_mapping));
	if (mappings == NULL)
		return B_NO_MEMORY;
	MemoryDeleter mappingsDeleter(mappings);

	uint32 count = 0;
	bool sawLast = mapBlocks == 0;
	for (uint32 i = 0; i < mapBlocks; i++) {
		apfs_paddr_t address = descBase + (first + i) % descBlocks;
		status_t status = ReadObject(address, block,
			APFS_OBJECT_TYPE_CHECKPOINT_MAP);
		if (status != B_OK)
			return status;

		const apfs_checkpoint_map_phys* map
			= (const apfs_checkpoint_map_phys*)block;
		if (map->o.TransactionID() != xid || map->Count() > perBlock) {
			ERROR("checkpoint map in block %" B_PRId64 " does not belong to "
				"xid %" B_PRIu64 "\n", address, xid);
			return B_BAD_DATA;
		}

		memcpy(mappings + count, map->map,
			map->Count() * sizeof(apfs_checkpoint_mapping));
		count += map->Count();

		if ((map->Flags() & APFS_CHECKPOINT_MAP_LAST) != 0) {
			sawLast = true;
			break;
		}
	}

	if (!sawLast) {
		ERROR("checkpoint xid %" B_PRIu64 " has no last mapping block\n", xid);
		return B_BAD_DATA;
	}

	// Check the ephemeral objects can be read (step 5, read-only variant:
	// they are read again from the checkpoint when needed).
	for (uint32 i = 0; i < count; i++) {
		if (mappings[i].Size() != fBlockSize) {
			// Multi-block objects (a large space manager) are checked
			// when something reads them.
			continue;
		}
		status_t status = ReadObject(mappings[i].Address(), block, 0);
		if (status != B_OK)
			return status;
		if (((const apfs_obj_phys*)block)->ObjectID()
				!= mappings[i].ObjectID()) {
			ERROR("ephemeral object %" B_PRIu64 " not where the checkpoint "
				"says\n", mappings[i].ObjectID());
			return B_BAD_DATA;
		}
	}

	free(fMappings);
	fMappings = mappings;
	fMappingCount = count;
	mappingsDeleter.Detach();
	return B_OK;
}
