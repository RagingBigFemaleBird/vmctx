/* SPDX-License-Identifier: GPL-2.0 */
/* Linux source adapter. Native syscall decoding belongs only on this side.
 * Partial failures and concurrent mutations require kernel commit records;
 * the successful-call constructions here are not such records. */
#ifndef VMCTX_SOURCE_MAP_EFFECTS_H
#define VMCTX_SOURCE_MAP_EFFECTS_H
#include <sys/mman.h>
#include <sys/shm.h>
#include <sys/syscall.h>
#include "vmrproto.h"

static inline uint32_t source_wire_protection(uint64_t prot)
{
	return ((prot & PROT_READ) ? VMR_PROT_READ : 0) |
	       ((prot & PROT_WRITE) ? VMR_PROT_WRITE : 0) |
	       ((prot & PROT_EXEC) ? VMR_PROT_EXEC : 0);
}

static inline void source_map_effect(const struct vmr_req *rq, long ret,
		uint64_t object_id, uint64_t object_offset, uint64_t shared_length,
		struct vmr_map_effect *map)
{
	*map = (struct vmr_map_effect){0};
	if ((unsigned long)ret >= (unsigned long)-4095L)
		return;
	switch (rq->nr) {
	case SYS_mmap:
		map->kind = VMR_MAP_SET;
		map->address = ret;
		map->length = rq->args[1];
		map->protection = source_wire_protection(rq->args[2]);
		break;
	case SYS_shmat:
		if (!object_id || !shared_length)
			return;
		map->kind = VMR_MAP_SET;
		map->address = ret;
		map->length = shared_length;
		map->protection = VMR_PROT_READ |
			((rq->args[2] & SHM_RDONLY) ? 0 : VMR_PROT_WRITE) |
			((rq->args[2] & SHM_EXEC) ? VMR_PROT_EXEC : 0);
		break;
	case SYS_mremap:
		map->kind = VMR_MAP_MOVE;
		map->address = ret;
		map->length = rq->args[2];
		map->prior_address = rq->args[0];
		map->prior_length = rq->args[1];
		if (rq->args[3] & MREMAP_FIXED)
			map->flags = VMR_MAP_REPLACE;
		return;
	default:
		return;
	}
	map->flags = object_id ? VMR_MAP_SHARED : 0;
	map->object_id = object_id;
	map->object_offset = object_id ? object_offset : map->address;
}
#endif
