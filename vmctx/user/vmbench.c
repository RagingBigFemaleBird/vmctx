// SPDX-License-Identifier: GPL-2.0
/*
 * vmbench — how fast does code run *inside* a VM context?
 *
 * Two loops, reported separately, because they answer different questions.
 *
 *   compute: arithmetic and memory, no syscalls. Nothing about it is
 *            intercepted, so it measures what the phrase "runs on the other
 *            machine" is worth: guest instructions execute on the hardware at
 *            hardware speed, or they do not.
 *
 *   syscall: getppid in a loop. Every one of these leaves the guest — #UD,
 *            VM exit, dispatch, VM entry — so it measures the cost of the
 *            boundary rather than of the code. It is the pessimal case on
 *            purpose: a real program does not spend its life in getppid.
 *
 * Run it natively, and again as the guest of a vmhome/vmremote pair; the ratio
 * of the compute figures is the answer to "near native", and the ratio of the
 * syscall figures is the price. (Loopback, tests/runany.sh, keeps the two
 * halves on one machine; tests/runany-x.sh puts them on two, which is where the
 * syscall figure stops being wakeup latency and becomes a LAN round trip.)
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <stdint.h>

static double now(void)
{
	struct timespec t;

	clock_gettime(CLOCK_MONOTONIC, &t);
	return (double)t.tv_sec + (double)t.tv_nsec / 1e9;
}

/* Enough state to defeat the optimiser without becoming a memory benchmark. */
static uint64_t compute(uint64_t iters)
{
	uint64_t a = 0x243f6a8885a308d3ULL, b = 0x13198a2e03707344ULL;
	static uint64_t tbl[4096];
	uint64_t i;

	for (i = 0; i < 4096; i++)
		tbl[i] = i * 0x9e3779b97f4a7c15ULL;
	for (i = 0; i < iters; i++) {
		a += b ^ (a >> 7);
		b = (b << 13) | (b >> 51);
		b += tbl[a & 4095];
		a ^= tbl[(b >> 17) & 4095];
	}
	return a ^ b;
}

int main(int argc, char **argv)
{
	uint64_t citer = (argc > 1) ? strtoull(argv[1], NULL, 0) : 200000000ULL;
	uint64_t siter = (argc > 2) ? strtoull(argv[2], NULL, 0) : 20000ULL;
	double t0, t1, t2;
	uint64_t sink, i;

	t0 = now();
	sink = compute(citer);
	t1 = now();
	for (i = 0; i < siter; i++)
		(void)getppid();
	t2 = now();

	printf("compute %llu iters in %.4f s  (%.1f Miter/s)\n",
	       (unsigned long long)citer, t1 - t0,
	       (double)citer / (t1 - t0) / 1e6);
	printf("syscall %llu calls  in %.4f s  (%.2f us/call)\n",
	       (unsigned long long)siter, t2 - t1,
	       (t2 - t1) / (double)siter * 1e6);
	printf("sink %llu\n", (unsigned long long)sink);
	return 0;
}
