/*
 * Copyright 2026, RenkuOS contributors.
 * Distributed under the terms of the MIT License.
 *
 * Unicode normalization (NFD) and full case folding of file names, from
 * tables generated out of the Unicode Character Database (UnicodeTables.h).
 * The algorithms are the ones in Unicode Standard Annex #15 (canonical
 * decomposition, Hangul syllables, canonical ordering).
 */


#include "Unicode.h"

#include <CRCTable.h>

#include "UnicodeTables.h"


// B_COUNT_OF is not there when built for the macOS test shell.
#define COUNT_OF(array) (sizeof(array) / sizeof((array)[0]))


// Hangul syllables decompose algorithmically (Unicode 3.12).
static const uint32 kHangulFirst = 0xac00;
static const uint32 kLeadFirst = 0x1100;
static const uint32 kVowelFirst = 0x1161;
static const uint32 kTrailFirst = 0x11a7;
static const uint32 kVowelCount = 21;
static const uint32 kTrailCount = 28;
static const uint32 kSyllableCount = 19 * kVowelCount * kTrailCount;


template<typename Entry>
static const Entry*
find_entry(const Entry* table, size_t count, uint32 codePoint)
{
	size_t low = 0;
	size_t high = count;
	while (low < high) {
		size_t middle = (low + high) / 2;
		if (table[middle].code_point < codePoint)
			low = middle + 1;
		else
			high = middle;
	}
	if (low < count && table[low].code_point == codePoint)
		return &table[low];
	return NULL;
}


static uint8
combining_class(uint32 codePoint)
{
	size_t low = 0;
	size_t high = COUNT_OF(kCombiningClasses);
	while (low < high) {
		size_t middle = (low + high) / 2;
		if (kCombiningClasses[middle].last < codePoint)
			low = middle + 1;
		else
			high = middle;
	}
	if (low < COUNT_OF(kCombiningClasses)
		&& kCombiningClasses[low].first <= codePoint)
		return kCombiningClasses[low].combining_class;
	return 0;
}


//!	Reads one code point; NULL for invalid UTF-8.
static const uint8*
next_code_point(const uint8* text, uint32* _codePoint)
{
	uint32 c = text[0];
	int length;
	uint32 minimum;
	if (c < 0x80) {
		*_codePoint = c;
		return text + 1;
	} else if ((c & 0xe0) == 0xc0) {
		length = 2;
		minimum = 0x80;
		c &= 0x1f;
	} else if ((c & 0xf0) == 0xe0) {
		length = 3;
		minimum = 0x800;
		c &= 0x0f;
	} else if ((c & 0xf8) == 0xf0) {
		length = 4;
		minimum = 0x10000;
		c &= 0x07;
	} else
		return NULL;

	for (int i = 1; i < length; i++) {
		if ((text[i] & 0xc0) != 0x80)
			return NULL;
		c = (c << 6) | (text[i] & 0x3f);
	}
	if (c < minimum || c > 0x10ffff || (c >= 0xd800 && c <= 0xdfff))
		return NULL;
	*_codePoint = c;
	return text + length;
}


//!	Appends the canonical decomposition of \a codePoint.
static bool
decompose(uint32 codePoint, uint32* output, size_t* _count)
{
	size_t count = *_count;
	if (codePoint >= kHangulFirst
		&& codePoint < kHangulFirst + kSyllableCount) {
		uint32 index = codePoint - kHangulFirst;
		uint32 trail = index % kTrailCount;
		if (count + 3 > APFS_NORMALIZED_NAME_MAX)
			return false;
		output[count++] = kLeadFirst + index / (kVowelCount * kTrailCount);
		output[count++] = kVowelFirst
			+ (index % (kVowelCount * kTrailCount)) / kTrailCount;
		if (trail != 0)
			output[count++] = kTrailFirst + trail;
		*_count = count;
		return true;
	}

	const apfs_unicode_mapping* entry = find_entry(kDecompositions,
		COUNT_OF(kDecompositions), codePoint);
	if (entry == NULL) {
		if (count + 1 > APFS_NORMALIZED_NAME_MAX)
			return false;
		output[count++] = codePoint;
	} else {
		if (count + entry->length > APFS_NORMALIZED_NAME_MAX)
			return false;
		for (uint8 i = 0; i < entry->length; i++)
			output[count++] = kDecompositionsPool[entry->index + i];
	}
	*_count = count;
	return true;
}


status_t
apfs_normalize_name(const char* name, bool fold, uint32* output,
	size_t* _count)
{
	size_t count = 0;
	const uint8* text = (const uint8*)name;
	while (*text != '\0') {
		uint32 codePoint;
		text = next_code_point(text, &codePoint);
		if (text == NULL)
			return B_BAD_VALUE;

		const apfs_unicode_mapping* folded = fold
			? find_entry(kCaseFolds, COUNT_OF(kCaseFolds), codePoint)
			: NULL;
		if (folded == NULL) {
			if (!decompose(codePoint, output, &count))
				return B_NAME_TOO_LONG;
		} else {
			for (uint8 i = 0; i < folded->length; i++) {
				if (!decompose(kCaseFoldsPool[folded->index + i], output,
						&count))
					return B_NAME_TOO_LONG;
			}
		}
	}

	// Canonical ordering: marks between two starters sorted by combining
	// class, keeping the order of equal classes (insertion sort; runs are
	// short).
	for (size_t i = 1; i < count; i++) {
		uint8 currentClass = combining_class(output[i]);
		if (currentClass == 0)
			continue;
		size_t j = i;
		uint32 current = output[i];
		while (j > 0) {
			uint8 previousClass = combining_class(output[j - 1]);
			if (previousClass <= currentClass)
				break;
			output[j] = output[j - 1];
			j--;
		}
		output[j] = current;
	}

	*_count = count;
	return B_OK;
}


uint32
apfs_name_hash(const uint32* codePoints, size_t count)
{
	// CRC-32C over the code points as UTF-32 little endian, without a
	// final null, started at ~0 and not complemented at the end: this, not
	// the reference's wording, matches the hashes macOS stores.
	uint32 crc = 0xffffffff;
	for (size_t i = 0; i < count; i++) {
		uint8 bytes[4] = {
			(uint8)codePoints[i], (uint8)(codePoints[i] >> 8),
			(uint8)(codePoints[i] >> 16), 0
		};
		crc = calculate_crc32c(crc, bytes, sizeof(bytes));
	}
	return crc & (APFS_J_DREC_HASH_MASK >> APFS_J_DREC_HASH_SHIFT);
}
