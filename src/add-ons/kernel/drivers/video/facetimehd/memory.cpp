/*
 * Copyright 2026, RenkuOS Contributors.
 * Distributed under the terms of the MIT License.
 * SPDX-License-Identifier: MIT
 */


/*!	Hands out pieces of the camera's own memory (BAR 2, "S2 MEM"), which the
	host and the camera firmware share: the firmware image, its heap, the
	command and message buffers.

	The firmware addresses this memory by offset from its start, so every
	piece is described by its offset. Pieces are page aligned, like the
	Linux driver's [F]. There are only ever a few dozen of them, so a sorted
	array and a first-fit search are enough.
*/


#include "facetimehd.h"

#include <string.h>

#include <util/AutoLock.h>


#define ALIGNMENT		4096


static uint32
align_up(uint32 value)
{
	return (value + ALIGNMENT - 1) & ~(uint32)(ALIGNMENT - 1);
}


static status_t
insert(fthd_device* device, int32 index, uint32 offset, uint32 size)
{
	if (device->allocation_count >= FTHD_MAX_ALLOCATIONS)
		return B_NO_MEMORY;

	memmove(&device->allocations[index + 1], &device->allocations[index],
		(device->allocation_count - index) * sizeof(fthd_allocation));
	device->allocations[index].offset = offset;
	device->allocations[index].size = size;
	device->allocation_count++;
	return B_OK;
}


void
fthd_memory_init(fthd_device* device)
{
	mutex_init(&device->memory_lock, "facetimehd memory");
	device->allocation_count = 0;
}


void
fthd_memory_uninit(fthd_device* device)
{
	mutex_destroy(&device->memory_lock);
	device->allocation_count = 0;
}


/*!	Claims the piece at \a offset, which must be free.
*/
status_t
fthd_memory_reserve(fthd_device* device, uint32 offset, uint32 size)
{
	size = align_up(size);
	if (size == 0 || (offset & (ALIGNMENT - 1)) != 0
		|| (uint64)offset + size > device->s2_mem_size) {
		return B_BAD_VALUE;
	}

	MutexLocker locker(device->memory_lock);

	int32 index = 0;
	while (index < device->allocation_count
		&& device->allocations[index].offset < offset) {
		index++;
	}
	if (index > 0) {
		const fthd_allocation& before = device->allocations[index - 1];
		if (before.offset + before.size > offset)
			return B_BUSY;
	}
	if (index < device->allocation_count
		&& device->allocations[index].offset < offset + size) {
		return B_BUSY;
	}

	return insert(device, index, offset, size);
}


status_t
fthd_memory_alloc(fthd_device* device, uint32 size, uint32* _offset)
{
	size = align_up(size);
	if (size == 0)
		return B_BAD_VALUE;

	MutexLocker locker(device->memory_lock);

	uint64 candidate = 0;
	int32 index = 0;
	for (; index < device->allocation_count; index++) {
		const fthd_allocation& next = device->allocations[index];
		if (candidate + size <= next.offset)
			break;
		candidate = (uint64)next.offset + next.size;
	}
	if (candidate + size > device->s2_mem_size)
		return B_NO_MEMORY;

	status_t status = insert(device, index, (uint32)candidate, size);
	if (status != B_OK)
		return status;

	*_offset = (uint32)candidate;
	return B_OK;
}


void
fthd_memory_free(fthd_device* device, uint32 offset)
{
	MutexLocker locker(device->memory_lock);

	for (int32 i = 0; i < device->allocation_count; i++) {
		if (device->allocations[i].offset != offset)
			continue;

		device->allocation_count--;
		memmove(&device->allocations[i], &device->allocations[i + 1],
			(device->allocation_count - i) * sizeof(fthd_allocation));
		return;
	}

	FTHD_LOG("freeing unknown memory at 0x%08" B_PRIx32 "\n", offset);
}


/*!	S2 MEM is device memory, mapped uncached: it is copied in 32-bit
	accesses, like the memory check does.
*/
void
fthd_memory_copy_to(fthd_device* device, uint32 offset, const void* buffer,
	size_t length)
{
	if ((uint64)offset + length > device->s2_mem_size || (offset & 3) != 0) {
		FTHD_LOG("bad copy to S2 MEM at 0x%08" B_PRIx32 "\n", offset);
		return;
	}

	const uint8* source = (const uint8*)buffer;
	volatile uint32* target = (volatile uint32*)(device->s2_mem + offset);
	while (length >= 4) {
		uint32 value;
		memcpy(&value, source, 4);
		*target++ = value;
		source += 4;
		length -= 4;
	}
	if (length > 0) {
		uint32 value = *target;
		memcpy(&value, source, length);
		*target = value;
	}
}


void
fthd_memory_copy_from(fthd_device* device, uint32 offset, void* buffer,
	size_t length)
{
	if ((uint64)offset + length > device->s2_mem_size || (offset & 3) != 0) {
		FTHD_LOG("bad copy from S2 MEM at 0x%08" B_PRIx32 "\n", offset);
		memset(buffer, 0, length);
		return;
	}

	uint8* target = (uint8*)buffer;
	const volatile uint32* source
		= (const volatile uint32*)(device->s2_mem + offset);
	while (length >= 4) {
		uint32 value = *source++;
		memcpy(target, &value, 4);
		target += 4;
		length -= 4;
	}
	if (length > 0) {
		uint32 value = *source;
		memcpy(target, &value, length);
	}
}
