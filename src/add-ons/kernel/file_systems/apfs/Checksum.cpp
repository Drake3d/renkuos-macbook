/*
 * Copyright 2026, RenkuOS contributors.
 * Distributed under the terms of the MIT License.
 *
 * Object checksum. The Apple File System Reference ("obj_phys_t", o_cksum)
 * names the algorithm: Fletcher 64, over the object as 32-bit little-endian
 * words, modulo 2^32 - 1. The first 8 bytes (the checksum itself) are left
 * out, and the two 32-bit halves are chosen so the running sums over the
 * whole object come out as zero. This follows the standard Fletcher
 * definition; it was checked against objects in images made by macOS.
 */


#include "Checksum.h"

#include "apfs.h"


static const uint64 kModulus = 0xffffffffULL;


uint64
apfs_object_checksum(const void* object, size_t size)
{
	const uint32* words = (const uint32*)object;
	size_t count = size / sizeof(uint32);

	uint64 sum1 = 0;
	uint64 sum2 = 0;
	for (size_t i = APFS_MAX_CKSUM_SIZE / sizeof(uint32); i < count; i++) {
		sum1 = (sum1 + B_LENDIAN_TO_HOST_INT32(words[i])) % kModulus;
		sum2 = (sum2 + sum1) % kModulus;
	}

	uint64 check1 = kModulus - ((sum1 + sum2) % kModulus);
	uint64 check2 = kModulus - ((sum1 + check1) % kModulus);
	return (check2 << 32) | check1;
}


bool
apfs_object_checksum_valid(const void* object, size_t size)
{
	const apfs_obj_phys* header = (const apfs_obj_phys*)object;
	return header->Checksum() == apfs_object_checksum(object, size);
}
