/*
 * Copyright 2026, RenkuOS contributors.
 * Distributed under the terms of the MIT License.
 */
#ifndef APFS_DEBUG_H
#define APFS_DEBUG_H


#ifdef TRACE_APFS
#	define TRACE(x...)	dprintf("apfs: " x)
#else
#	define TRACE(x...)
#endif

// Messages worth keeping in the syslog: mount results, refused volumes,
// and damaged metadata.
#define INFORM(x...)	dprintf("apfs: " x)
#define ERROR(x...)		dprintf("apfs: error: " x)


#endif	// APFS_DEBUG_H
