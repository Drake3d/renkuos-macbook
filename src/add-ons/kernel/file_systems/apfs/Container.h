/*
 * Copyright 2026, RenkuOS contributors.
 * Distributed under the terms of the MIT License.
 */
#ifndef APFS_CONTAINER_H
#define APFS_CONTAINER_H


#include "apfs.h"


class ObjectMap;


// Information about one volume of a container, for listing and choosing.
struct apfs_volume_info {
	uint32		index;				// position in nx_fs_oid
	apfs_oid_t	oid;				// virtual object id of the superblock
	apfs_paddr_t address;			// where the superblock is now
	char		name[APFS_VOLNAME_LEN];
	uint16		role;
	uint64		incompatible_features;
	uint64		flags;
};


// An APFS container (one partition): its newest valid checkpoint, the
// ephemeral objects listed there, and the container object map.
// Read only; all block access goes through ReadBlock().
class Container {
public:
								Container();
								~Container();

			status_t			Open(int device, off_t deviceSize,
									bool useBlockCache);
			void				Close();

			int					Device() const { return fDevice; }
			uint32				BlockSize() const { return fBlockSize; }
			uint64				BlockCount() const { return fBlockCount; }
			apfs_xid_t			TransactionID() const
									{ return fSuperBlock.o.TransactionID(); }
			const apfs_nx_superblock& SuperBlock() const
									{ return fSuperBlock; }
			ObjectMap*			GetObjectMap() const { return fObjectMap; }
			// Free blocks of the container (shared by all its volumes), from
			// the space manager; 0 if it could not be read.
			uint64				FreeBlocks() const { return fFreeBlocks; }

			// Reads one block into buffer (BlockSize() bytes).
			status_t			ReadBlock(apfs_paddr_t block, void* buffer);
			// Reads one block and checks the object header: checksum, and
			// the type if expectedType is not 0.
			status_t			ReadObject(apfs_paddr_t block, void* buffer,
									uint32 expectedType);

			// Finds where an ephemeral object of the current checkpoint is.
			status_t			ResolveEphemeral(apfs_oid_t oid,
									apfs_paddr_t* _address) const;

			uint32				CountVolumes() const;
			// Volume by position in nx_fs_oid (holes are skipped by caller).
			status_t			GetVolumeInfo(uint32 index,
									apfs_volume_info* info);
			// Reads a volume superblock (BlockSize() bytes) at its current
			// location and checks it.
			status_t			ReadVolumeSuperblock(uint32 index,
									void* buffer);

	static	status_t			Identify(int device,
									apfs_nx_superblock* superBlock);

private:
			status_t			_FindCheckpoint(int device,
									const apfs_nx_superblock& blockZero);
			status_t			_LoadCheckpointMaps(
									const apfs_nx_superblock& superBlock);
			void				_ReadFreeBlocks();
			bool				_IsValidSuperBlock(
									const apfs_nx_superblock* superBlock,
									bool checkpoint) const;

			int					fDevice;
			off_t				fDeviceSize;
			uint32				fBlockSize;
			uint64				fBlockCount;
			void*				fBlockCache;
			apfs_nx_superblock	fSuperBlock;

			apfs_checkpoint_mapping* fMappings;
			uint32				fMappingCount;

			ObjectMap*			fObjectMap;
			uint64				fFreeBlocks;
};


#endif	// APFS_CONTAINER_H
