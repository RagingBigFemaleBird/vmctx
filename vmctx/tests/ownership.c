/* SPDX-License-Identifier: GPL-2.0 */
/* Host-side ownership invariants: these need no vmctx kernel or lab machine.
 * Run each case in a fresh process; header-local tables start empty.
 * cc -O2 -Wall -Wextra -Wno-unused-function -pthread ownership.c -o ownership
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <stdatomic.h>
#include <sys/resource.h>
#include <sys/wait.h>
#include <unistd.h>

static int fail_pg_alloc;
static void *pg_test_calloc(size_t count, size_t size)
{
	return fail_pg_alloc ? NULL : calloc(count, size);
}
#define calloc pg_test_calloc
#include "../user/pgstate.h"
#undef calloc
#include "../user/own.h"

static int require(int condition, const char *message)
{
	if (!condition)
		fprintf(stderr, "FAIL: %s\n", message);
	return condition ? 0 : 1;
}

static int exclusion(void)
{
	const uint64_t as = 7, page = 0x12345000;
	int failed = 0;

	failed |= require(pg_try_claim(as, page, PST_TRANSFERRING) >= 0,
			  "the serving thread must acquire the page");
	failed |= require(pg_try_claim(as, page, PST_REQUESTING) < 0,
			  "a concurrent faulter must not acquire the page");
	failed |= require(pg_try_claim(as, page, PST_REQUESTING) < 0,
			  "a failed claimant must leave the incumbent's transfer locked");
	pg_settle(as, page, PST_INVALID);
	failed |= require(pg_try_claim(as, page, PST_REQUESTING) >= 0,
			  "the page must be acquirable after the incumbent settles");
	return failed;
}

static int capacity(void)
{
	const uint64_t as = 9, base = 0x20000000;
	const unsigned pages = 20000;
	int failed = 0;

	for (unsigned i = 0; i < pages; i++) {
		uint64_t page = base + (uint64_t)i * 4096;
		if (pg_try_claim(as, page, PST_TRANSFERRING) < 0) {
			fprintf(stderr, "FAIL: could not track live page %u\n", i);
			return 1;
		}
		if (pg_try_claim(as, page, PST_REQUESTING) >= 0) {
			fprintf(stderr, "FAIL: two simultaneous claims for page %u\n", i);
			return 1;
		}
	}
	/* Growth must preserve every previously acquired transfer. */
	for (unsigned i = 0; i < pages; i++) {
		uint64_t page = base + (uint64_t)i * 4096;
		failed |= require(pg_try_claim(as, page, PST_REQUESTING) < 0,
				  "table growth must retain existing claims");
		pg_settle(as, page, PST_INVALID);
		failed |= require(pg_try_claim(as, page, PST_REQUESTING) >= 0,
				  "settled entries must remain reusable");
		if (failed)
			return failed;
	}
	return 0;
}

static int pg_normalize(void)
{
	int failed = require(pg_try_claim(7, 0x12345003, PST_REQUESTING) >= 0,
			     "first transfer claim");
	failed |= require(pg_try_claim(7, 0x12345fff, PST_TRANSFERRING) < 0,
			  "all offsets in a page share one transfer claim");
	pg_settle(7, 0x12345000, PST_INVALID);
	failed |= require(pg_try_claim(7, 0x12345003, PST_REQUESTING) >= 0,
			  "aligned settlement releases the same page");
	return failed;
}

static pthread_barrier_t growth_barrier;
static atomic_int growth_failed;
static void *growth_worker(void *arg)
{
	const uint64_t as = *(unsigned *)arg + 100;
	const uint64_t base = 0x10000000;

	for (unsigned i = 0; i < 6000; i++) {
		if (pg_try_claim(as, base + (uint64_t)i * 4096,
				 PST_TRANSFERRING) < 0 ||
		    pg_try_claim(7, base, PST_REQUESTING) >= 0)
			atomic_store(&growth_failed, 1);
	}
	/* Every thread's earliest claims must survive the other threads' growth. */
	pthread_barrier_wait(&growth_barrier);
	for (unsigned i = 0; i < 6000; i++) {
		uint64_t page = base + (uint64_t)i * 4096;

		if (pg_try_claim(as, page, PST_REQUESTING) >= 0)
			atomic_store(&growth_failed, 1);
		pg_settle(as, page, PST_INVALID);
		if (pg_try_claim(as, page, PST_REQUESTING) < 0)
			atomic_store(&growth_failed, 1);
	}
	return NULL;
}

static int concurrent_growth(void)
{
	pthread_t threads[4];
	unsigned ids[4];

	alarm(15);
	if (pg_try_claim(7, 0x10000000, PST_TRANSFERRING) < 0 ||
	    pthread_barrier_init(&growth_barrier, NULL, 4))
		return require(0, "concurrent growth setup");
	for (unsigned i = 0; i < 4; i++) {
		ids[i] = i;
		if (pthread_create(&threads[i], NULL, growth_worker, &ids[i]))
			return require(0, "creating growth worker");
	}
	for (unsigned i = 0; i < 4; i++)
		if (pthread_join(threads[i], NULL))
			return require(0, "joining growth worker");
	pthread_barrier_destroy(&growth_barrier);
	alarm(0);
	return require(!atomic_load(&growth_failed),
		       "concurrent growth preserves exclusion and address-space identity");
}

static int allocation_failure(void)
{
	for (int grow = 0; grow < 2; grow++) {
		pid_t child = fork();
		int status;

		if (child < 0)
			return require(0, "fork for allocation failure");
		if (!child) {
			struct rlimit no_core = {0, 0};
			setrlimit(RLIMIT_CORE, &no_core);
			alarm(5);
			if (grow)
				for (unsigned i = 0; i < PGSTATE_N / 2; i++)
					pg_try_claim(7, 0x10000000 + (uint64_t)i * 4096,
						     PST_REQUESTING);
			fail_pg_alloc = 1;
			pg_try_claim(7, 0x700000000000, PST_REQUESTING);
			_exit(91); /* returning success or busy would hide the failure */
		}
		if (waitpid(child, &status, 0) != child ||
		    !WIFSIGNALED(status) || WTERMSIG(status) != SIGABRT)
			return require(0, "allocation failure must stop an untracked transfer");
	}
	return 0;
}

static int normalize(void)
{
	uint32_t gen = 0;
	int failed = 0;

	if (own_init(-1))
		return require(0, "ownership segment initialization");
	failed |= require(own_begin(7, 0x12345003, OWN_T_TO_REMOTE, &gen) ==
			  OWN_BEGIN_CLAIMED, "first unaligned page claim");
	failed |= require(own_begin(7, 0x12345003, OWN_T_TO_REMOTE, &gen) ==
			  OWN_BEGIN_BUSY, "repeat address must observe active claim");
	failed |= require(own_begin(7, 0x12345000, OWN_T_TO_REMOTE, &gen) ==
			  OWN_BEGIN_BUSY, "all addresses within a page name one claim");
	own_land(7, 0x12345000, OWN_REMOTE);
	failed |= require(own_begin(7, 0x12345fff, OWN_T_TO_HOME, &gen) ==
			  OWN_BEGIN_CLAIMED, "landing must release the normalized page");
	own_abort(7, 0x12345000);
	return failed;
}

static int scale(void)
{
	const uint64_t as = 17, base = 0x700000000000;
	const unsigned pages = 32768, rounds = 4;
	uint32_t gen;
	uint64_t start, elapsed;

	if (own_init(-1))
		return require(0, "ownership segment initialization");
	start = own_now_us();
	for (unsigned r = 0; r < rounds; r++) {
		for (unsigned i = 0; i < pages; i++) {
			uint64_t page = base + (uint64_t)i * 4096;
			if (own_begin(as, page, OWN_T_TO_REMOTE, &gen) != OWN_BEGIN_CLAIMED)
				return require(0, "claim during sequential-page workload");
			own_land(as, page, OWN_REMOTE);
		}
	}
	elapsed = own_now_us() - start;
	printf("ownership-scale pages=%u transitions=%u elapsed_us=%llu ns_per_transition=%.1f full=%llu\n",
	       pages, pages * rounds, (unsigned long long)elapsed,
	       elapsed * 1000.0 / (pages * rounds),
	       (unsigned long long)own_seg->n_full);
	return require(own_seg->n_begin == (uint64_t)pages * rounds &&
		       own_seg->n_land == own_seg->n_begin && !own_seg->n_full,
		       "every measured transition must be recorded and settled");
}

int main(int argc, char **argv)
{
	int rc;

	if (argc != 2) {
		fprintf(stderr, "usage: %s exclusion|capacity|normalize|pg-normalize|concurrent-growth|allocation-failure|scale\n", argv[0]);
		return 2;
	}
	if (!strcmp(argv[1], "exclusion")) rc = exclusion();
	else if (!strcmp(argv[1], "capacity")) rc = capacity();
	else if (!strcmp(argv[1], "normalize")) rc = normalize();
	else if (!strcmp(argv[1], "pg-normalize")) rc = pg_normalize();
	else if (!strcmp(argv[1], "concurrent-growth")) rc = concurrent_growth();
	else if (!strcmp(argv[1], "allocation-failure")) rc = allocation_failure();
	else if (!strcmp(argv[1], "scale")) rc = scale();
	else return 2;
	if (!rc)
		puts("PASS");
	return rc;
}
