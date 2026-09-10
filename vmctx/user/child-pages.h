/* SPDX-License-Identifier: GPL-2.0 */
#ifndef VMR_CHILD_PAGES_H
#define VMR_CHILD_PAGES_H
#include <stdint.h>
#include "vmrproto.h"

/* Validate an ordered page batch without interpreting source OS metadata. */
static inline int child_pages_valid(const struct vmr_child_pages *b, uint64_t cursor)
{
	uint64_t previous = cursor;
	if (b->count > VMR_CHILD_PAGES_MAX || b->reserved || (cursor & 4095)) return 0;
	for (uint32_t i = 0; i < b->count; i++) {
		uint64_t p = b->pages[i];
		if ((p & 4095) || p >= (UINT64_C(1) << 63) || p < previous) return 0;
		previous = p + 4096;
	}
	if (b->next && ((b->next & 4095) || b->next <= cursor ||
		b->next < previous || b->next >= (UINT64_C(1) << 63))) return 0;
	for (uint32_t i = b->count; i < VMR_CHILD_PAGES_MAX; i++)
		if (b->pages[i]) return 0;
	return 1;
}
#endif
