/* SPDX-License-Identifier: GPL-2.0 */
/* Source-issued, OS-neutral publication of a context's MM binding. Context
 * and MM IDs are independent opaque identities. Epoch orders bindings of one
 * context; it is not a native object epoch or a page-transfer receipt.
 * A publication describes the observed binding only. Memory operations must
 * carry their own saved target, which can outlive this binding's replacement. */
#ifndef VMR_MEMORY_IDENTITY_H
#define VMR_MEMORY_IDENTITY_H
#include <stdint.h>
struct vmr_mm_binding {
	uint64_t context, mm, parent_mm, epoch;
};
_Static_assert(sizeof(struct vmr_mm_binding)==32,"MM binding wire layout");
static inline int vmr_binding_valid(const struct vmr_mm_binding *binding)
{
	return binding->context && binding->mm && binding->epoch &&
		binding->parent_mm!=binding->mm;
}
static inline int vmr_binding_empty(const struct vmr_mm_binding *binding)
{ return !(binding->context | binding->mm | binding->parent_mm | binding->epoch); }
static inline int vmr_binding_equal(const struct vmr_mm_binding *a,
		const struct vmr_mm_binding *b)
{ return a->context==b->context && a->mm==b->mm && a->parent_mm==b->parent_mm && a->epoch==b->epoch; }
#endif
