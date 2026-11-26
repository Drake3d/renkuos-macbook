/*
 * Copyright 2026, RenkuOS contributors.
 * Distributed under the terms of the MIT License.
 */
#ifndef APFS_SYSTEM_DEPENDENCIES_H
#define APFS_SYSTEM_DEPENDENCIES_H


#ifdef FS_SHELL

#include "fssh_api_wrapper.h"
#include "fssh_auto_deleter.h"
#include "fssh_auto_locker.h"
#include "fssh_kernel_priv.h"

#else	// !FS_SHELL

#include <AutoDeleter.h>
#include <util/AutoLock.h>

#include <ByteOrder.h>
#include <Drivers.h>
#include <KernelExport.h>
#include <NodeMonitor.h>
#include <SupportDefs.h>
#include <TypeConstants.h>

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <new>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include <fs_cache.h>
#include <fs_info.h>
#include <fs_interface.h>
#include <fs_volume.h>
#include <io_requests.h>
#include <kernel.h>

#endif	// !FS_SHELL


#include "Debug.h"


#endif	// APFS_SYSTEM_DEPENDENCIES_H
