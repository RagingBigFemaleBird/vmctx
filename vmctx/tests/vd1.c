/* vd1 -- every user-reachable clock, judged against the forwarded syscall.
 *
 * The vDSO answers time from the vvar page, which under vmctx is guest
 * memory served from the source: a frozen snapshot at best, an invented
 * zero at worst. Session 42c measured the worst: a guest's time(2) read 0
 * and every TLS certificate was "not yet valid" (curl exit 60). A zeroed
 * vvar page happens to make clock_gettime() fall back to the real syscall
 * (clock_mode NONE), which is why date(1) and python survive -- but
 * __vdso_time() has NO fallback, and the COARSE clocks are vvar-only by
 * design. So this test asks each clock the way libc does (vDSO first) and
 * compares against the one answer that cannot come from the vDSO: the
 * syscall itself, which vmctx forwards to the source's kernel.
 *
 * The tolerance is deliberately loose (60 s) -- the fix serves the
 * DESTINATION's own live vvar page, so cross-machine a vDSO read is the
 * destination's wall clock while the syscall is the source's; two NTP'd
 * boxes agree far inside a minute, and the defect this photographs is off
 * by an EPOCH, not by seconds.
 */
#include <stdio.h>
#include <time.h>
#include <sys/time.h>
#include <sys/syscall.h>
#include <unistd.h>
#include <stdlib.h>

static int bad;

static void check(const char *what, long long got, long long want, long long tol)
{
	long long d = got - want;
	if (d < 0)
		d = -d;
	if (d > tol) {
		printf("BAD  %-18s %lld (truth %lld, off by %lld)\n",
		       what, got, want, d);
		bad = 1;
	} else {
		printf("ok   %-18s %lld\n", what, got);
	}
}

int main(void)
{
	long long truth = syscall(SYS_time, NULL);
	struct timespec ts, m1, m2;
	struct timeval tv;

	if (truth < 1500000000LL) {
		/* the reference itself is broken -- nothing else is judgeable */
		printf("BAD  SYS_time itself reads %lld\nFAIL\n", truth);
		return 1;
	}

	check("time()", (long long)time(NULL), truth, 60);

	if (gettimeofday(&tv, NULL) != 0) {
		printf("BAD  gettimeofday errno\n");
		bad = 1;
	} else {
		check("gettimeofday", (long long)tv.tv_sec, truth, 60);
	}

	if (clock_gettime(CLOCK_REALTIME, &ts) != 0)
		bad = 1;
	check("REALTIME", (long long)ts.tv_sec, truth, 60);

	if (clock_gettime(CLOCK_REALTIME_COARSE, &ts) != 0)
		bad = 1;
	check("REALTIME_COARSE", (long long)ts.tv_sec, truth, 60);

	/* Monotonic clocks have no cross-machine truth; what they owe is
	 * plausibility (a machine that booted, not the epoch) and ADVANCE. */
	if (clock_gettime(CLOCK_MONOTONIC, &m1) != 0)
		bad = 1;
	struct timespec nap = { 0, 50 * 1000 * 1000 };
	nanosleep(&nap, NULL);
	if (clock_gettime(CLOCK_MONOTONIC, &m2) != 0)
		bad = 1;
	long long adv = (m2.tv_sec - m1.tv_sec) * 1000000000LL
			+ (m2.tv_nsec - m1.tv_nsec);
	if (adv < 25 * 1000 * 1000 || adv > 5000000000LL) {
		printf("BAD  MONOTONIC advanced %lld ns across a 50 ms sleep\n",
		       adv);
		bad = 1;
	} else {
		printf("ok   MONOTONIC          advanced %lld ns\n", adv);
	}

	/*
	 * time() must ADVANCE, not merely be right once. A served vvar page
	 * is a snapshot, and a snapshot's coarse clocks freeze: fc1 measured
	 * it as `while (time(NULL) - t0 < SECONDS)` spinning the whole
	 * VMR_LIMIT (the refresher re-pokes the page 3x/s, so within ~2.5 s
	 * the second must roll). This is the check the instant-correctness
	 * ones above cannot make.
	 */
	{
		long long t1 = (long long)time(NULL);
		int waited;

		for (waited = 0; waited < 50; waited++) {
			struct timespec tick = { 0, 50 * 1000 * 1000 };

			if ((long long)time(NULL) > t1)
				break;
			nanosleep(&tick, NULL);
		}
		if ((long long)time(NULL) <= t1) {
			printf("BAD  time() FROZEN at %lld across %d ms\n",
			       t1, waited * 50);
			bad = 1;
		} else {
			printf("ok   time() advances    (after ~%d ms)\n",
			       waited * 50);
		}
	}

	if (clock_gettime(CLOCK_MONOTONIC_COARSE, &ts) != 0)
		bad = 1;
	/* Coarse monotonic must at least be in the running clock's
	 * neighbourhood -- a zeroed vvar answers 0 while its fine sibling
	 * (via the syscall fallback) answers the real uptime. */
	long long coarse_off = (long long)ts.tv_sec - (long long)m2.tv_sec;
	if (coarse_off < -60 || coarse_off > 60) {
		printf("BAD  MONOTONIC_COARSE  %lld vs fine %lld\n",
		       (long long)ts.tv_sec, (long long)m2.tv_sec);
		bad = 1;
	} else {
		printf("ok   MONOTONIC_COARSE  %lld\n", (long long)ts.tv_sec);
	}

	if (bad) {
		printf("FAIL\n");
		return 1;
	}
	printf("PASS\n");
	return 0;
}
