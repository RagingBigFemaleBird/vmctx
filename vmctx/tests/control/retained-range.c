// SPDX-License-Identifier: GPL-2.0
/* Large sparse address ranges must not monopolize the retained-page cache.
 * cc -O2 -pthread -I../../kernel retained-range.c -o retained-range
 */
#define main vmremote_program_main
#include "../../user/vmremote.c"
#undef main
#define TEST_MM_REGISTRY executor_mms
#include "execution-fixture.h"

int main(void)
{
	char page[VMR_PG_SIZE], got[VMR_PG_SIZE];
	memory_target a = test_target(101), b = test_target(102);
	uint64_t start = 1ULL << 40, end = start + (1ULL << 40);
	memset(page, 0x95, sizeof(page));
	/* Include a distinct address space and live entries on both sides of
	 * the half-open range. The range is large, its populated set is small. */
	retain_put(a, start - VMR_PG_SIZE, page);
	retain_put(a, start, page);
	retain_put(a, end - VMR_PG_SIZE, page);
	retain_put(a, end, page);
	retain_put(b, start, page);
	alarm(3);
	retain_forget(a, start + 1, end);
	alarm(0);
	assert(!retain_get(a, start, got));
	assert(!retain_get(a, end - VMR_PG_SIZE, got));
	assert(retain_get(a, start - VMR_PG_SIZE, got));
	assert(!memcmp(page, got, sizeof(page)));
	assert(retain_get(a, end, got) && !memcmp(page, got, sizeof(page)));
	assert(retain_get(b, start, got) && !memcmp(page, got, sizeof(page)));
	/* Empty and reversed ranges cannot discard anything. Reinserting a
	 * tombstoned key must preserve its new bytes. */
	retain_forget(a, end, end);
	retain_forget(a, end + VMR_PG_SIZE, end);
	assert(retain_get(a, end, got));
	memset(page, 0x3a, sizeof(page));
	retain_put(a, start, page);
	assert(retain_get(a, start, got) && !memcmp(page, got, sizeof(page)));
	alarm(3);
	retain_forget(a, 0, UINT64_MAX);
	alarm(0);
	assert(!retain_get(a, start, got) && !retain_get(a, end, got));
	assert(retain_get(b, start, got));
	puts("PASS: sparse retained ranges expire promptly without crossing address-space or range boundaries");
	return 0;
}
