// SPDX-License-Identifier: GPL-2.0
/* The Linux source adapter publishes a complete neutral construction. */
#define _GNU_SOURCE
#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include "../../user/source-map-effects.h"

int main(void)
{
	struct vmr_map_effect map;
	struct vmr_req rq = {.nr = SYS_mmap,
		.args = {0, 0x3000, PROT_READ | PROT_EXEC, MAP_PRIVATE, -1, 0}};
	source_map_effect(&rq, 0x120000, 0, 0, 0, &map);
	assert(map.kind == VMR_MAP_SET && map.address == 0x120000 &&
	       map.length == 0x3000 && map.object_offset == 0x120000 &&
	       map.protection == (VMR_PROT_READ | VMR_PROT_EXEC) && !map.flags);
	source_map_effect(&rq, -ENOMEM, 0, 0, 0, &map);
	assert(map.kind == VMR_MAP_NONE);
	rq.nr = SYS_shmat;
	rq.args[2] = SHM_RDONLY | SHM_EXEC;
	source_map_effect(&rq, 0x240000, 71, 0, 0x4000, &map);
	assert(map.kind == VMR_MAP_SET && map.flags == VMR_MAP_SHARED &&
	       map.protection == (VMR_PROT_READ | VMR_PROT_EXEC) &&
	       map.address == 0x240000 && map.object_offset == 0 && map.object_id == 71 &&
	       map.length == 0x4000);
	rq = (struct vmr_req){.nr = SYS_mremap,
		.args = {0x10000, 0x4000, 0x8000, MREMAP_FIXED | MREMAP_MAYMOVE, 0x30000}};
	source_map_effect(&rq, 0x30000, 0, 0, 0, &map);
	assert(map.kind == VMR_MAP_MOVE && map.flags == VMR_MAP_REPLACE &&
	       map.address == 0x30000 && map.length == 0x8000 &&
	       map.prior_address == 0x10000 && map.prior_length == 0x4000);
	rq = (struct vmr_req){.nr = SYS_mprotect,
		.args = {0x10000, 0x1000, PROT_NONE}};
	source_map_effect(&rq, 0, 0, 0, 0, &map);
	/* Permission effects come from committed kernel events, including
	 * partial errors; a successful return is not an authoritative event. */
	assert(map.kind == VMR_MAP_NONE);
	rq.nr = SYS_getpid;
	source_map_effect(&rq, 123, 0, 0, 0, &map);
	assert(map.kind == VMR_MAP_NONE);
	puts("PASS: source mapping effects include addresses, permissions, sharing and relocation");
	return 0;
}
