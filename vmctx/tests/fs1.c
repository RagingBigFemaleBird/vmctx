// SPDX-License-Identifier: GPL-2.0
/*
 * fs1 — every thread must have its own thread pointer.
 *
 * A thread's TLS is established BY the clone that makes it: CLONE_SETTLS, with
 * the base in an argument. Forward that clone without the flag or the argument
 * and the new thread silently keeps its creator's pointer -- and nothing
 * complains, because nothing ever asks a thread what its own base is.
 *
 * Under vmctx it did exactly that, on both sides at once: the source built the
 * service thread with `clone(..., tls = 0)`, and the destination copied the
 * PARENT's fs_base onto the new context because its register set came from a
 * GETREGS of the parent. Every thread of a guest ran on the main thread's TLS
 * block.
 *
 * That is not a small thing. The C library keeps its malloc arena pointer, its
 * errno, and the thread and stack-cache lists behind that pointer, so all
 * threads shared one set of them and raced on them. It surfaced as memory
 * corruption with no visible cause -- a zeroed list linkage in
 * __nptl_deallocate_stack, a non-canonical return address on the ret in
 * __res_thread_freeres, pthread_create refusing, glibc's own sysmalloc
 * assertion -- about one run in five, and eleven separate page-transport
 * explanations were proposed and refuted before anybody asked this question.
 *
 * So this asks it, in the only way that gets a true answer:
 * arch_prctl(ARCH_GET_FS), from inside each thread. NOT pthread_self(), which
 * reads %fs itself and would cheerfully echo the same wrong base back four
 * times -- the first version of this check did that and had to be replaced.
 *
 * The verdict: every thread's base must differ from the main thread's and from
 * every other thread's. Native it passes trivially. It needs no fork, no
 * namespace and no load.
 *
 * It WAITS for the threads rather than sleeping at them, and that is not
 * fastidiousness. The first version slept 100ms and then read the four bases,
 * which is a race the moment thread start-up is slower than the sleep -- and
 * under vmctx every thread is a context built across two machines, so it often
 * is. Measured: 10 failures in 30, every one of them "thread N never reported a
 * base", i.e. the test failing rather than the system. A flaky case in the suite
 * is worse than no case, because it teaches the reader to discount a red line.
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdint.h>
#include <unistd.h>
#include <sched.h>
#include <pthread.h>
#include <sys/syscall.h>
#include <asm/prctl.h>

#define NT 4

static volatile int stop;
static volatile unsigned long fsb[NT];

static void *th(void *x)
{
	long k = (long)x;
	unsigned long v = 0;

	syscall(SYS_arch_prctl, ARCH_GET_FS, &v);
	fsb[k] = v;
	while (!stop)
		sched_yield();
	return NULL;
}

int main(void)
{
	pthread_t t[NT];
	unsigned long mv = 0;
	long i, j;
	int bad = 0;

	syscall(SYS_arch_prctl, ARCH_GET_FS, &mv);
	for (i = 0; i < NT; i++)
		if (pthread_create(&t[i], NULL, th, (void *)i) != 0) {
			printf("FAIL could not start thread %ld\n", i);
			return 2;
		}
	/*
	 * Wait for every thread to have answered, with a bound generous enough
	 * that only a genuinely stuck thread reaches it. Ten seconds is far more
	 * than a thread needs even here, and a test that waits for its subject
	 * cannot report a slow start as a wrong answer.
	 */
	for (i = 0; i < 10000; i++) {
		int ready = 0;

		for (j = 0; j < NT; j++)
			if (fsb[j])
				ready++;
		if (ready == NT)
			break;
		usleep(1000);
	}

	for (i = 0; i < NT; i++) {
		if (fsb[i] == 0) {
			printf("FAIL thread %ld never reported a base\n", i);
			bad++;
			continue;
		}
		if (fsb[i] == mv) {
			printf("FAIL thread %ld has the MAIN thread's base "
			       "0x%lx -- its TLS is not its own\n", i, mv);
			bad++;
		}
		for (j = 0; j < i; j++)
			if (fsb[i] == fsb[j]) {
				printf("FAIL thread %ld shares thread %ld's "
				       "base 0x%lx\n", i, j, fsb[i]);
				bad++;
			}
	}
	stop = 1;
	for (i = 0; i < NT; i++)
		pthread_join(t[i], NULL);

	if (bad)
		return 1;
	printf("PASS\n");
	return 0;
}
