/*
 * Copyright 2026, RenkuOS contributors.
 * Distributed under the terms of the MIT License.
 *
 * Mounting one volume of a container.
 * Source: Apple File System Reference, revision 2020-06-22, "Mounting an
 * Apple File System Partition" (steps 8-9), "apfs_superblock_t", "Volume
 * Roles", "Volume Flags", "Incompatible Volume Feature Flags".
 */


#include "Volume.h"

#include <DeviceOpener.h>

#include "BTree.h"
#include "ObjectMap.h"


const char*
apfs_role_name(uint16 role)
{
	switch (role) {
		case APFS_VOL_ROLE_NONE:		return "none";
		case APFS_VOL_ROLE_SYSTEM:		return "system";
		case APFS_VOL_ROLE_USER:		return "user";
		case APFS_VOL_ROLE_RECOVERY:	return "recovery";
		case APFS_VOL_ROLE_VM:			return "vm";
		case APFS_VOL_ROLE_PREBOOT:		return "preboot";
		case APFS_VOL_ROLE_INSTALLER:	return "installer";
		case APFS_VOL_ROLE_DATA:		return "data";
		case APFS_VOL_ROLE_BASEBAND:	return "baseband";
		case APFS_VOL_ROLE_UPDATE:		return "update";
		case APFS_VOL_ROLE_XART:		return "xart";
		case APFS_VOL_ROLE_HARDWARE:	return "hardware";
		case APFS_VOL_ROLE_BACKUP:		return "backup";
		case APFS_VOL_ROLE_SIDECAR:		return "sidecar";
		case APFS_VOL_ROLE_ENTERPRISE:	return "enterprise";
		case APFS_VOL_ROLE_PRELOGIN:	return "prelogin";
		default:						return "other";
	}
}


Volume::Volume(fs_volume* volume)
	:
	fFSVolume(volume),
	fDevice(-1),
	fSuperBlock(NULL),
	fObjectMap(NULL),
	fFileSystemTree(NULL)
{
	memset(&fInfo, 0, sizeof(fInfo));
}


Volume::~Volume()
{
	Unmount();
}


bool
Volume::IsCaseSensitive() const
{
	return (fSuperBlock->IncompatibleFeatures()
		& APFS_INCOMPAT_CASE_INSENSITIVE) == 0;
}


void
Volume::_ListVolumes()
{
	uint32 count = fContainer.CountVolumes();
	for (uint32 i = 0; i < count; i++) {
		apfs_volume_info info;
		if (fContainer.GetVolumeInfo(i, &info) != B_OK)
			continue;

		INFORM("  volume %" B_PRIu32 ": \"%s\", role %s, %s%s%s\n", i,
			info.name, apfs_role_name(info.role),
			(info.incompatible_features & APFS_INCOMPAT_CASE_INSENSITIVE)
				!= 0 ? "case-insensitive" : "case-sensitive",
			(info.flags & APFS_FS_UNENCRYPTED) == 0 ? ", encrypted" : "",
			(info.incompatible_features & APFS_INCOMPAT_SEALED_VOLUME) != 0
				? ", sealed" : "");
	}
}


status_t
Volume::_ChooseVolume(const char* args, uint32* _index)
{
	// Mount parameter "volume=<index or name>".
	const char* wanted = NULL;
	char buffer[APFS_VOLNAME_LEN];
	if (args != NULL) {
		const char* option = strstr(args, "volume=");
		if (option != NULL) {
			option += strlen("volume=");
			size_t length = strcspn(option, " ,;");
			if (length >= sizeof(buffer))
				length = sizeof(buffer) - 1;
			memcpy(buffer, option, length);
			buffer[length] = '\0';
			wanted = buffer;
		}
	}
#ifdef FS_SHELL
	// The FS shell mounts without parameters.
	if (wanted == NULL)
		wanted = getenv("APFS_VOLUME");
#endif

	uint32 count = fContainer.CountVolumes();
	int32 fallback = -1;
	for (uint32 i = 0; i < count; i++) {
		apfs_volume_info info;
		if (fContainer.GetVolumeInfo(i, &info) != B_OK)
			continue;

		if (wanted != NULL) {
			char* end;
			unsigned long index = strtoul(wanted, &end, 10);
			if ((*end == '\0' && end != wanted && index == i)
				|| strcmp(wanted, info.name) == 0) {
				*_index = i;
				return B_OK;
			}
			continue;
		}

		// Default: the first unencrypted volume that holds user data.
		if ((info.flags & APFS_FS_UNENCRYPTED) == 0)
			continue;
		if (info.role == APFS_VOL_ROLE_NONE || info.role == APFS_VOL_ROLE_DATA
			|| info.role == APFS_VOL_ROLE_USER) {
			*_index = i;
			return B_OK;
		}
		if (fallback < 0)
			fallback = i;
	}

	if (wanted != NULL) {
		ERROR("no volume \"%s\" in this container\n", wanted);
		return B_ENTRY_NOT_FOUND;
	}
	if (fallback < 0) {
		ERROR("no volume that can be mounted in this container\n");
		return B_ENTRY_NOT_FOUND;
	}
	*_index = fallback;
	return B_OK;
}


status_t
Volume::Mount(const char* deviceName, uint32 flags, const char* args)
{
	// DeviceOpener asks for O_NOCACHE. A disk image registered with
	// diskimage is opened through the file system that holds the file, and
	// some (FAT) refuse O_NOCACHE with B_UNSUPPORTED. The driver reads through
	// its own block cache, so a plain read-only open is fine as a fallback.
	DeviceOpener opener(deviceName, O_RDONLY);
	fDevice = opener.Device();
	if (fDevice >= 0) {
		// From here on Unmount() closes the device, also after a failed
		// mount.
		opener.Keep();
	} else {
		status_t firstError = fDevice;
		fDevice = open(deviceName, O_RDONLY);
		if (fDevice < 0) {
			status_t error = errno;
			ERROR("cannot open %s: %s\n", deviceName, strerror(error));
			return error;
		}
		INFORM("%s refused O_NOCACHE (%s), opened without it\n", deviceName,
			strerror(firstError));
	}

	off_t deviceSize = 0;
	{
		DeviceOpener sizer(fDevice, O_RDONLY);
		sizer.GetSize(&deviceSize);
	}

	status_t status = fContainer.Open(fDevice, deviceSize, true);
	if (status != B_OK)
		return status;

	INFORM("container on %s: %" B_PRIu64 " blocks of %" B_PRIu32 " bytes, "
		"checkpoint %" B_PRIu64 "\n", deviceName, fContainer.BlockCount(),
		fContainer.BlockSize(), fContainer.TransactionID());
	_ListVolumes();

	uint32 index;
	status = _ChooseVolume(args, &index);
	if (status != B_OK)
		return status;

	status = fContainer.GetVolumeInfo(index, &fInfo);
	if (status != B_OK)
		return status;

	fSuperBlock = (apfs_superblock*)malloc(fContainer.BlockSize());
	if (fSuperBlock == NULL)
		return B_NO_MEMORY;
	status = fContainer.ReadVolumeSuperblock(index, fSuperBlock);
	if (status != B_OK)
		return status;

	if (fSuperBlock->IsEncrypted()) {
		ERROR("volume \"%s\" is encrypted (FileVault); encrypted volumes "
			"are not supported yet\n", fInfo.name);
		return B_NOT_SUPPORTED;
	}

	// Step 9: the volume object map (physical) and its file-system tree
	// (virtual, looked up in the volume object map).
	fObjectMap = new(std::nothrow) ObjectMap(&fContainer);
	if (fObjectMap == NULL)
		return B_NO_MEMORY;
	status = fObjectMap->SetTo(fSuperBlock->ObjectMap());
	if (status != B_OK) {
		ERROR("volume \"%s\": cannot read its object map: %s\n", fInfo.name,
			strerror(status));
		return status;
	}

	fFileSystemTree = new(std::nothrow) BTree(&fContainer, fObjectMap,
		fContainer.TransactionID());
	if (fFileSystemTree == NULL)
		return B_NO_MEMORY;
	status = fFileSystemTree->SetTo(fSuperBlock->RootTree(),
		fSuperBlock->RootTreeType(), APFS_OBJECT_TYPE_FSTREE);
	if (status != B_OK) {
		ERROR("volume \"%s\": cannot read its file-system tree: %s\n",
			fInfo.name, strerror(status));
		return status;
	}

	INFORM("mounted volume %" B_PRIu32 " \"%s\" read-only: %" B_PRIu64
		" files, %" B_PRIu64 " directories, %" B_PRIu64 " symlinks, %"
		B_PRIu64 " snapshots; file-system tree has %" B_PRIu64 " records "
		"in %" B_PRIu64 " nodes\n", index, fInfo.name,
		fSuperBlock->FileCount(), fSuperBlock->DirectoryCount(),
		fSuperBlock->SymlinkCount(), fSuperBlock->SnapshotCount(),
		fFileSystemTree->Info().KeyCount(),
		fFileSystemTree->Info().NodeCount());

	return B_OK;
}


status_t
Volume::Unmount()
{
	delete fFileSystemTree;
	fFileSystemTree = NULL;
	delete fObjectMap;
	fObjectMap = NULL;
	free(fSuperBlock);
	fSuperBlock = NULL;

	fContainer.Close();
	if (fDevice >= 0) {
		close(fDevice);
		fDevice = -1;
	}
	return B_OK;
}
