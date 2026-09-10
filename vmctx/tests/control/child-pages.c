// SPDX-License-Identifier: GPL-2.0
/* Reject source snapshot metadata that could seed the wrong page or loop. */
#include <assert.h>
#include <stdio.h>
#include "../../user/child-pages.h"
int main(void)
{
	struct vmr_child_pages b = {0};
	assert(child_pages_valid(&b, 0));
	b.count = 2; b.pages[0] = 0x1000; b.pages[1] = 0x9000; b.next = 0xa000;
	assert(child_pages_valid(&b, 0x1000));
	b.pages[1] = 0x1000; assert(!child_pages_valid(&b, 0x1000));
	b.pages[1] = 0x9001; assert(!child_pages_valid(&b, 0x1000));
	b.pages[1] = 0x9000; b.next = 0x9000; assert(!child_pages_valid(&b, 0x1000));
	b.next = 0; assert(child_pages_valid(&b, 0x1000));
	assert(!child_pages_valid(&b, 0x2000));
	b.count = VMR_CHILD_PAGES_MAX + 1; assert(!child_pages_valid(&b, 0));
	b.count = 2; b.reserved = 1; assert(!child_pages_valid(&b, 0));
	b.reserved = 0; b.pages[2] = 0x10000; assert(!child_pages_valid(&b, 0));
	b.pages[2] = 0; b.pages[1] = UINT64_C(1) << 63; assert(!child_pages_valid(&b, 0));
	b = (struct vmr_child_pages){.next = 0x1000};
	assert(!child_pages_valid(&b, 0x1000));
	puts("PASS: child page publication rejects duplicate, unordered and malformed ownership metadata");
	return 0;
}
