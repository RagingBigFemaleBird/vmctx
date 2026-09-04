// SPDX-License-Identifier: GPL-2.0
/*
 * sh2 — two threads of one program must see one page.
 *
 * The most basic thing an address space promises: if thread A stores to an
 * address, thread B loading that address sees the store. Nothing about threads
 * is interesting here; that guarantee is what "one address space" MEANS.
 *
 * It is worth a test of its own because of how this system provides it. On the
 * destination a guest thread is a CONTEXT: a separate task with its own mm.
 * They are not threads of one process there, and nothing in the hardware makes
 * them share anything. They share because every context of one address space is
 * handed the same backing object and maps it MAP_SHARED at the same offsets, so
 * one virtual address is one offset is one page. That is the whole mechanism --
 * "two contexts handed the same object see the same page at the same address,
 * which is the whole of sharing an address space" -- and it holds only as long
 * as every mapping really is shared. A single range that ends up MAP_PRIVATE in
 * each context gives each thread its own copy-on-write copy, and then two
 * threads of one program disagree about memory while every other test passes.
 *
 * So: several pages, in the regions that differ in how they are mapped -- the
 * heap, a fresh anonymous mmap, and the writable data of the program itself --
 * and for each, A stores, B loads, then B stores and A loads. Both directions,
 * because a mapping can be shared one way and stale the other if only one side
 * ever faults it in.
 *
 * The handoff is a futex-free spin on a volatile word, deliberately: a
 * pthread_mutex or a condvar would put the two threads through the source's
 * futex path, which moves pages for its own reasons and would let a broken
 * mapping pass because the lock traffic happened to carry the value across.
 * A bare spin makes the store itself the only thing that can communicate.
 *
 * Native this passes trivially: one mm, one page, nothing to arrange.
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <pthread.h>
#include <sys/mman.h>
#include <stdint.h>

#define ROUNDS	2000
#define NSLOT	3		/* heap, fresh mmap, .data */

static volatile unsigned long *slot[NSLOT];
static const char *slotname[NSLOT] = { "heap", "mmap", ".data" };

/* .data: written before main so it is a real, already-faulted data page. */
static unsigned long data_page[512] = { 1 };

static volatile int turn;	/* 0 = A stores, 1 = B stores */
static volatile int failed;
static volatile unsigned long seen_by_b[NSLOT], seen_by_a[NSLOT];

static void spin_until(volatile int *w, int v)
{
	unsigned long guard = 0;

	while (*w != v) {
		if (++guard > 4000000000UL) {	/* not a deadlock detector; a bound */
			failed = 1;
			return;
		}
		__asm__ __volatile__("pause" ::: "memory");
	}
}

static void *thread_b(void *arg)
{
	unsigned long i;
	int s;

	(void)arg;
	for (i = 1; i <= ROUNDS && !failed; i++) {
		spin_until(&turn, 1);
		if (failed)
			break;
		/* read what A stored */
		for (s = 0; s < NSLOT; s++)
			seen_by_b[s] = slot[s][0];
		/* store our own, and hand back */
		for (s = 0; s < NSLOT; s++)
			slot[s][1] = i * 1000003UL + (unsigned long)s;
		__atomic_store_n(&turn, 0, __ATOMIC_SEQ_CST);
	}
	return NULL;
}

int main(void)
{
	pthread_t b;
	unsigned long i;
	int s, bad = 0;
	unsigned long *heap, *anon;

	heap = malloc(4096 * 2);
	anon = mmap(NULL, 4096 * 2, PROT_READ | PROT_WRITE,
		    MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	if (!heap || anon == MAP_FAILED) {
		perror("alloc");
		return 2;
	}
	memset(heap, 0, 4096 * 2);
	memset(anon, 0, 4096 * 2);
	slot[0] = heap;
	slot[1] = anon;
	slot[2] = data_page;

	if (pthread_create(&b, NULL, thread_b, NULL) != 0) {
		perror("pthread_create");
		return 2;
	}

	for (i = 1; i <= ROUNDS && !failed; i++) {
		for (s = 0; s < NSLOT; s++)
			slot[s][0] = i * 2654435761UL + (unsigned long)s;
		__atomic_store_n(&turn, 1, __ATOMIC_SEQ_CST);
		spin_until(&turn, 0);
		if (failed)
			break;
		/* what B stored must be here */
		for (s = 0; s < NSLOT; s++) {
			unsigned long want = i * 1000003UL + (unsigned long)s;

			seen_by_a[s] = slot[s][1];
			if (seen_by_a[s] != want) {
				fprintf(stderr, "sh2: FAIL -- round %lu, %s: "
					"thread B stored 0x%lx and thread A "
					"loaded 0x%lx from the same address "
					"(%p). Two threads of one program are "
					"not sharing that page.\n",
					i, slotname[s], want, seen_by_a[s],
					(void *)&slot[s][1]);
				bad = 1;
				break;
			}
		}
		if (bad)
			break;
	}

	failed = 1;			/* release B however this ended */
	__atomic_store_n(&turn, 1, __ATOMIC_SEQ_CST);
	pthread_join(b, NULL);

	if (bad)
		return 1;
	printf("PASS\n");
	return 0;
}
