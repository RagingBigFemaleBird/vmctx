// SPDX-License-Identifier: GPL-2.0
/* Exercise native pagemap enumeration on a parked child with sparse private
 * memory and resident shared aliases, including multiple response batches. */
#define _GNU_SOURCE
#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <unistd.h>
#include "../../user/vmrproto.h"
#include "../../user/child-pages.h"
#define PG_OURS 1
static pid_t expected_child;
static unsigned ownership_writes;
typedef int source_id;
typedef const struct vmr_mm_binding *source_target;
static source_id source_target_id(source_target target){return (source_id)target->context;}
/* This enumeration control supplies resources for its own pipe-parked child.
 * The native source-context control separately exercises the opener's actual
 * identity proofs, including reuse on each side of open and exec. */
static int source_record_proc_open(source_id child, const char *leaf, uint64_t *mm)
{
	assert(child==expected_child);
	char path[96];
	snprintf(path,sizeof(path),"/proc/%d/%s",child,leaf);
	*mm=1;
	return open(path,O_RDONLY|O_CLOEXEC);
}
static int pg_set(source_target child, uint64_t page, int state)
{
	assert(child->context == (uint64_t)expected_child && child->mm==1 && !(page & 4095) && state == PG_OURS);
	ownership_writes++;
	return 0;
}
static void cow_mm_note_break(uint64_t mm, uint64_t page)
{
	assert(mm==1 && !(page & 4095));
}
#include "../../user/source-snapshot.h"
int main(void)
{
	const size_t pages = 1200;
	unsigned char *private = mmap(NULL, pages * 4096, PROT_READ | PROT_WRITE,
		MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	unsigned char *shared = mmap(NULL, 4096, PROT_READ | PROT_WRITE,
		MAP_SHARED | MAP_ANONYMOUS, -1, 0);
	assert(private != MAP_FAILED && shared != MAP_FAILED);
	for (size_t i = 0; i < pages; i += 2) private[i * 4096] = 1;
	shared[0] = 1;
	int gate[2], ready[2]; assert(!pipe(gate) && !pipe(ready));
	expected_child = fork(); assert(expected_child >= 0);
	if (!expected_child) {
		char c = 1; close(gate[1]); close(ready[0]);
		if (write(ready[1], &c, 1) != 1 || read(gate[0], &c, 1) != 1) _exit(2);
		_exit(0);
	}
	close(gate[0]); close(ready[1]);
	char c; assert(read(ready[0], &c, 1) == 1);
	const struct vmr_mm_binding target={.context=(uint64_t)expected_child,.mm=1,.epoch=1};
	struct vmr_mm_binding wrong=target;wrong.mm+=UINT64_C(1)<<32;
	struct vmr_child_pages refused;
	assert(source_child_pages(&wrong,0,&refused)==-ESTALE && !ownership_writes && !refused.count);
	uint64_t cursor = 0; unsigned found = 0, batches = 0;
	do {
		struct vmr_child_pages b;
		assert(!source_child_pages(&target, cursor, &b));
		assert(child_pages_valid(&b, cursor));
		for (uint32_t i = 0; i < b.count; i++) {
			assert(b.pages[i] != (uintptr_t)shared);
			if (b.pages[i] >= (uintptr_t)private && b.pages[i] < (uintptr_t)private + pages * 4096) {
				assert(!(((b.pages[i] - (uintptr_t)private) / 4096) & 1));
				found++;
			}
		}
		cursor = b.next; batches++;
	} while (cursor);
	assert(found == pages / 2 && batches >= 3);
	assert(write(gate[1], "x", 1) == 1); close(gate[1]); close(ready[0]);
	int status; assert(waitpid(expected_child, &status, 0) == expected_child && !status);
	munmap(private, pages * 4096); munmap(shared, 4096);
	puts("PASS: native child snapshot enumeration preserves sparse private ownership across batches and excludes shared mappings");
	return 0;
}
