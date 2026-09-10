// SPDX-License-Identifier: GPL-2.0
/* Exercise real reservation contention and retained completion handles. A
 * delayed result must not release a later operation on the same page. */
#define _GNU_SOURCE
#include <assert.h>
#include <string.h>
#include "../../user/pgstate.h"

static uint64_t protected_writes;
static pthread_barrier_t start;
static void *writer(void *unused)
{
	(void)unused;
	int r = pthread_barrier_wait(&start);
	assert(!r || r == PTHREAD_BARRIER_SERIAL_THREAD);
	for (unsigned i = 0; i < 10000; i++) {
		struct pg_claim claim = {0};
		assert(pg_claim_wait(UINT64_C(1) << 48, 0x2000,
			PST_REQUESTING, 1000, &claim) >= 0);
		protected_writes++;
		pg_finish_required(&claim, PST_OWNED);
	}
	return NULL;
}

int main(void)
{
	alarm(30);
	const uint64_t mm = UINT64_C(1) << 48, page = 0x1000;
	struct pg_claim first = {0}, next = {0}, contender = {0};
	assert(pg_try_claim(mm, page + 9, PST_REQUESTING, &first) == PST_INVALID);
	struct pg_claim delayed = first;
	assert(pg_try_claim(mm, page, PST_UNINSTALLING, &contender) < 0 && errno == EBUSY);
	assert(!contender.episode);
	assert(pg_try_claim(mm, page, PST_UNINSTALLING, &first) < 0 && errno == EALREADY);
	assert(pg_finish(&first, PST_REQUESTING) < 0 && errno == EINVAL);
	assert(!pg_finish(&first, PST_SHARED) && !first.episode);
	assert(pg_try_claim(mm, page, PST_UNINSTALLING, &next) == PST_SHARED);
	assert(pg_finish(&delayed, PST_INVALID) < 0 && errno == ESTALE);
	assert(pg_try_claim(mm, page, PST_REQUESTING, &contender) < 0 && errno == EBUSY);
	/* Force several table moves while keeping the same claim alive. */
	for (unsigned i = 0; i < 20000; i++) {
		struct pg_claim other = {0};
		assert(pg_try_claim(mm + i + 1, page, PST_REQUESTING, &other) == PST_INVALID);
		pg_finish_required(&other, PST_OWNED);
	}
	assert(pg_finish(&delayed, PST_OWNED) < 0 && errno == ESTALE);
	struct pg_claim wrong = next;
	wrong.as++;
	assert(pg_finish(&wrong, PST_OWNED) < 0 && errno == ESTALE);
	assert(!pg_finish(&next, PST_INVALID));
	assert(pg_finish(&next, PST_INVALID) < 0 && errno == EINVAL);
	/* The same low bits of an MM id never alias its reservation. */
	assert(pg_try_claim(0, page, PST_REQUESTING, &next) == PST_INVALID);
	assert(pg_try_claim(mm, page, PST_DOWNGRADING, &contender) == PST_INVALID);
	pg_finish_required(&next, PST_SHARED);
	pg_finish_required(&contender, PST_OWNED);
	assert(!strcmp(pgst_name(PST_REQUESTING), "REQUESTING"));
	pthread_t threads[4];
	assert(!pthread_barrier_init(&start, NULL, 4));
	for (unsigned i = 0; i < 4; i++) assert(!pthread_create(&threads[i], NULL, writer, NULL));
	for (unsigned i = 0; i < 4; i++) assert(!pthread_join(threads[i], NULL));
	assert(!pthread_barrier_destroy(&start));
	assert(protected_writes == 40000);
	puts("PASS: transfer episodes reject stale completions across contention, MM identities and table growth; 40000 competing writes are conserved");
	return 0;
}
