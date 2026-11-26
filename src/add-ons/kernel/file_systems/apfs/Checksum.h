/*
 * Copyright 2026, RenkuOS contributors.
 * Distributed under the terms of the MIT License.
 */
#ifndef APFS_CHECKSUM_H
#define APFS_CHECKSUM_H


#include "system_dependencies.h"


// Fletcher 64 checksum of an object, as stored in obj_phys_t.o_cksum.
uint64 apfs_object_checksum(const void* object, size_t size);

// True if the object's stored checksum matches its contents.
bool apfs_object_checksum_valid(const void* object, size_t size);


#endif	// APFS_CHECKSUM_H
