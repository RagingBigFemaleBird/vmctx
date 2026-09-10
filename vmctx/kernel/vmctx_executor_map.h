/* SPDX-License-Identifier: GPL-2.0 WITH Linux-syscall-note */
#ifndef _UAPI_VMCTX_EXECUTOR_MAP_H
#define _UAPI_VMCTX_EXECUTOR_MAP_H
#include <linux/types.h>

/* Linux execution-adapter API. These native operations and structures never
 * cross the network; the source still describes effects in vmr_map_effect. */
#define VMCTX_CTL_PROTECT_MM 37
#define VMCTX_CTL_EXECUTOR_MAP_CAPS 38 /* pid=0, output vmctx_executor_map_caps */
#define VMCTX_EXECUTOR_MAP_ABI 1U
#define VMCTX_EXECUTOR_MAP_PROTECT_MM (1ULL << 0)
#define VMCTX_EXECUTOR_MAP_SHARED_PAGE (1ULL << 1)
#define VMCTX_EXECUTOR_MAP_READ_OBJECT (1ULL << 2)
#define VMCTX_EXECUTOR_MAP_PRESERVE_WRITE (1ULL << 3)
/* PROTECT_MM updates VMA rights without upgrading write-protected PTEs.
 * Only a separate, ownership-authorized MAPOBJ may grant those writes. */
#define VMCTX_CTL_MAPOBJ_READ 50
/* MAPOBJ_READ takes vmctx_mem with one aligned page and buf=0. It maps only
 * an existing shmem folio, never allocates backing, and never grants write
 * access. An existing PTE keeps its permissions. A new PTE is write-protected
 * even in a writable VMA; execution permission follows the VMA. Returns a
 * page's size, 0 for an object hole, or a negative error. */
struct vmctx_executor_map_caps {
    __u32 version, size;
    __u64 features;
};
struct vmctx_protection {
    __u64 address, length; /* page aligned; holes skipped, overflow rejected */
    __u32 protection, reserved; /* native PROT_READ/WRITE/EXEC; reserved=0 */
};
/* Reply operation: map exactly one page from the shared object and install
 * only a folio that object already holds. A hole stays absent for a retry;
 * no empty replacement is allocated. Requires SHARED_PAGE capability. */
#define VMCTX_MAP_SET_SHARED_PAGE 7
#endif
