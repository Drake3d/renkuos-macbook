/*
 * Copyright 2026, RenkuOS contributors.
 * Distributed under the terms of the MIT License.
 *
 * Test helper for craft_decmpfs.py (build host only): writes the raw LZVN
 * stream of stdin to stdout, using Apple's encoder from
 * https://github.com/lzfse/lzfse (BSD-3-Clause, not part of this tree):
 *   cc -O2 -I<lzfse>/src -o tools/lzvn_enc lzvn_enc.c \
 *       <lzfse>/src/lzvn_encode_base.c
 */
#include <stdio.h>
#include <stdlib.h>

#include "lzvn_encode_base.h"


int
main(void)
{
	static unsigned char in[1 << 20];
	static unsigned char out[(1 << 20) + 4096];
	size_t length = fread(in, 1, sizeof(in), stdin);
	void* work = malloc(lzvn_encode_scratch_size());
	size_t written = lzvn_encode_buffer(out, sizeof(out), in, length, work);
	if (written == 0) {
		fprintf(stderr, "lzvn_enc: no output\n");
		return 1;
	}
	fwrite(out, 1, written, stdout);
	return 0;
}
