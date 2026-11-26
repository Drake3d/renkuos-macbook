/*
 * Copyright 2026, RenkuOS contributors.
 * Distributed under the terms of the MIT License.
 */
#ifndef APFS_UNICODE_H
#define APFS_UNICODE_H


#include "apfs.h"


// File names as APFS compares and hashes them. Volumes made by macOS
// 10.13 and later are normalization-insensitive: names compare as their
// canonical decomposition (NFD), and on case-insensitive volumes after
// full Unicode case folding first (so "Straße" equals "STRASSE"). The
// directory-entry hash is a CRC-32C over these code points. Checked against
// the hashes macOS stored for names in many scripts, on case-sensitive and
// case-insensitive volumes.

// Room for the normalized form of any name of up to 255 UTF-8 bytes.
#define APFS_NORMALIZED_NAME_MAX	1024


struct apfs_unicode_mapping {
	uint32	code_point;
	uint16	index;
	uint8	length;
};

struct apfs_unicode_class_range {
	uint32	first;
	uint32	last;
	uint8	combining_class;
};


// Writes the normalized code points of the null-terminated UTF-8 \a name
// to \a output (APFS_NORMALIZED_NAME_MAX entries) and their count to
// *_count. B_BAD_VALUE for a name that is not valid UTF-8.
status_t	apfs_normalize_name(const char* name, bool fold, uint32* output,
				size_t* _count);

// The 22-bit directory-entry hash of normalized code points.
uint32		apfs_name_hash(const uint32* codePoints, size_t count);


#endif	// APFS_UNICODE_H
