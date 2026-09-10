// SPDX-License-Identifier: GPL-2.0
/*
 * pf5 -- pf3's fatal-signal contract, under repetition.
 *
 * pf3 runs each fatal fault once. The cross-machine campaign (session 40)
 * saw its fpe case miss ONCE in ~136 single runs: the #DE child died 242
 * with no exception line in the destination's log and the module silent --
 * a flake nobody has caught in the act, because one shot per run means one
 * sample per run. This case is the amplifier: every fatal shape, many times
 * in one run, so a 1-in-136 flake becomes a likely-per-run event and the
 * logs of THE run that caught it are the photograph.
 *
 * Each round forks three children; each child raises its fault the way a
 * real program would (the same forms as pf3: a store through NULL, ud2,
 * idiv by a zero the compiler cannot fold); the parent requires death BY
 * THE SIGNAL. Any other end -- a clean exit, the wrong signal, a wait
 * error -- is counted with the round number and the wait status, and the
 * verdict is a rate, not a first-failure stop.
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <sys/wait.h>

#define ROUNDS 25

static volatile char *null_ptr;
static volatile int zero;

static void fault_segv(void)
{
	*null_ptr = 1;
}

static void fault_ill(void)
{
	__asm__ __volatile__("ud2");
}

static void fault_fpe(void)
{
	int d = zero;

	__asm__ __volatile__("xorl %%edx, %%edx\n\t"
			     "movl $1, %%eax\n\t"
			     "idivl %0"
			     :
			     : "r"(d)
			     : "eax", "edx", "cc");
}

static int fails;

static void one(int round, const char *name, int want_sig, void (*fn)(void))
{
	pid_t kid;
	int st;

	kid = fork();
	if (kid < 0) {
		fprintf(stderr, "pf5: round %d %s: fork: %m\n", round, name);
		fails++;
		return;
	}
	if (kid == 0) {
		fn();
		/* Reaching here means the fault never fired. */
		_exit(9);
	}
	if (waitpid(kid, &st, 0) != kid) {
		fprintf(stderr, "pf5: round %d %s: waitpid: %m\n", round, name);
		fails++;
		return;
	}
	if (WIFSIGNALED(st) && WTERMSIG(st) == want_sig)
		return;
	fails++;
	if (WIFSIGNALED(st))
		fprintf(stderr, "pf5: round %d %s: died by signal %d, wanted "
			"%d\n", round, name, WTERMSIG(st), want_sig);
	else if (WIFEXITED(st) && WEXITSTATUS(st) == 9)
		fprintf(stderr, "pf5: round %d %s: the fault never happened; "
			"the access was served\n", round, name);
	else if (WIFEXITED(st))
		fprintf(stderr, "pf5: round %d %s: exited %d rather than "
			"dying (status 0x%x)\n", round, name,
			WEXITSTATUS(st), st);
	else
		fprintf(stderr, "pf5: round %d %s: wait status 0x%x\n",
			round, name, st);
}

int main(void)
{
	int round;

	setvbuf(stdout, NULL, _IONBF, 0);
	setvbuf(stderr, NULL, _IONBF, 0);
	null_ptr = 0;
	zero = 0;

	for (round = 0; round < ROUNDS; round++) {
		one(round, "segv", SIGSEGV, fault_segv);
		one(round, "ill", SIGILL, fault_ill);
		one(round, "fpe", SIGFPE, fault_fpe);
	}

	fprintf(stderr, "pf5: %d rounds x 3 fatal children, %d wrong\n",
		ROUNDS, fails);
	printf(fails ? "FAIL\n" : "PASS\n");
	return fails ? 1 : 0;
}
