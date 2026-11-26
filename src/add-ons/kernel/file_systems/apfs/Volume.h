/*
 * Copyright 2026, RenkuOS contributors.
 * Distributed under the terms of the MIT License.
 */
#ifndef APFS_VOLUME_H
#define APFS_VOLUME_H


#include "Container.h"


class BTree;
class ObjectMap;


extern fs_volume_ops gAPFSVolumeOps;
extern fs_vnode_ops gAPFSVnodeOps;


// One mounted APFS volume. Every mount opens the container for itself and
// picks one of its volumes.
class Volume {
public:
								Volume(fs_volume* volume);
								~Volume();

			status_t			Mount(const char* device, uint32 flags,
									const char* args);
			status_t			Unmount();

			fs_volume*			FSVolume() const { return fFSVolume; }
			dev_t				ID() const { return fFSVolume->id; }
			Container&			GetContainer() { return fContainer; }

			const char*			Name() const { return fInfo.name; }
			ino_t				RootID() const
									{ return APFS_ROOT_DIR_INO_NUM; }
			uint32				BlockSize() const
									{ return fContainer.BlockSize(); }
			const apfs_superblock& SuperBlock() const
									{ return *fSuperBlock; }
			bool				IsCaseSensitive() const;
			BTree*				FileSystemTree() const
									{ return fFileSystemTree; }

private:
			status_t			_ChooseVolume(const char* args,
									uint32* _index);
			void				_ListVolumes();

			fs_volume*			fFSVolume;
			int					fDevice;
			Container			fContainer;
			apfs_volume_info	fInfo;
			apfs_superblock*	fSuperBlock;
			ObjectMap*			fObjectMap;
			BTree*				fFileSystemTree;
};


const char* apfs_role_name(uint16 role);


#endif	// APFS_VOLUME_H
