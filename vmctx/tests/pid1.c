// SPDX-License-Identifier: GPL-2.0
/*
 * pid1 -- a payload forked INSIDE a fresh pid namespace must run.
 *
 * The reduction of netsurf's session-41f wall 2, and the exact shape of every
 * bwrap sandbox (glycin's included): the sandbox clones itself into
 * CLONE_NEWPID -- it is pid 1 in there -- and then FORKS the payload. That
 * inner fork is where the wall stood: the assisted clone's return value is
 * the pid as the PARENT'S namespace names it ("2"), and vmhome addressed the
 * source's service tasks by that number in the HOST namespace -- every ctl,
 * GETREGS and kill landed on host pid 2 (kthreadd), the real child parked in
 * vmctx_report with nobody ever adopting it, and the sandbox hung at its
 * payload fork forever. VMCTX_CTL_LASTCHILD (kernel #167) is the fix: the
 * clone records the child's host-ns pid on the parent, vmhome reads it back,
 * and the guest still sees its namespace's own numbers. This is the net.
 *
 * Three generations, all raw clones on the inherited stack (ns1's reasoning):
 * the outer parent clones the SANDBOX into CLONE_NEWUSER|CLONE_NEWPID; the
 * sandbox requires getpid() == 1, forks the PAYLOAD, requires the fork's
 * return to be the payload's NS pid (2), and reaps it by exactly that number;
 * the payload requires getpid() == 2, walks a canary, and exits with a value
 * the sandbox folds into its own. The old defect is a HANG at the payload
 * fork, so the sandbox arms an alarm: a round that times out fails loudly
 * with the generation that stalled, never silently.
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <sched.h>
#include <signal.h>
#include <stdint.h>
#include <sys/syscall.h>
#include <sys/wait.h>

#define ROUNDS	5
#define CANARY	0x1abe11edu

static unsigned deep(const volatile unsigned *p, int d)
{
	if (d > 0)
		return deep(p, d - 1);
	return *p;
}

/* The sandbox: pid 1 of a namespace that did not exist a moment ago. */
static void sandbox(void)
{
	volatile unsigned canary = CANARY;
	long payload;
	int st;

	/*
	 * The old defect is an eternal hang at the fork below (the child
	 * parks unadopted, the waitpid never answers). Convert it into a
	 * loud death the outer parent can name.
	 */
	alarm(20);

	if (getpid() != 1)
		_exit(10);	/* not pid 1: the namespace is not real */

	payload = syscall(SYS_clone, (unsigned long)SIGCHLD,
			  0UL, 0UL, 0UL, 0UL);
	if (payload < 0)
		_exit(11);
	if (payload == 0) {
		/* The payload: the task the wall never let exist. */
		if (getpid() != 2)
			_exit(12);	/* ns numbering leaked */
		if (deep(&canary, 12) != CANARY)
			_exit(13);
		_exit(42);
	}
	/*
	 * The fork's return must be the NS-relative pid -- the guest's view
	 * is the namespace's view, whatever the monitor uses underneath.
	 */
	if (payload != 2)
		_exit(14);
	if (waitpid((pid_t)payload, &st, 0) != (pid_t)payload)
		_exit(15);
	if (!WIFEXITED(st))
		_exit(16);	/* payload died by signal */
	_exit(WEXITSTATUS(st) == 42 ? 0 : 17);
}

int main(void)
{
	static const char *const why[] = {
		[10] = "sandbox getpid() != 1",
		[11] = "payload fork failed",
		[12] = "payload getpid() != 2",
		[13] = "payload canary wrong",
		[14] = "fork returned a non-namespace pid",
		[15] = "waitpid(payload) failed",
		[16] = "payload died by signal",
		[17] = "payload exited with the wrong value",
	};
	int round, fails = 0;

	for (round = 0; round < ROUNDS; round++) {
		long kid;
		int st;

		kid = syscall(SYS_clone,
			      (unsigned long)(CLONE_NEWUSER | CLONE_NEWPID |
					      SIGCHLD),
			      0UL, 0UL, 0UL, 0UL);
		if (kid < 0) {
			if (errno == EINVAL || errno == EPERM) {
				printf("SKIP clone(CLONE_NEWUSER|CLONE_NEWPID) "
				       "refused: %s\n", strerror(errno));
				printf("PASS\n");
				return 0;
			}
			printf("FAIL round %d: clone: %s\n", round,
			       strerror(errno));
			return 2;
		}
		if (kid == 0)
			sandbox();	/* never returns */

		if (waitpid((pid_t)kid, &st, 0) != (pid_t)kid) {
			printf("FAIL round %d: waitpid(sandbox): %s\n",
			       round, strerror(errno));
			fails++;
			continue;
		}
		if (WIFSIGNALED(st)) {
			fails++;
			printf("FAIL round %d: sandbox died by signal %d%s\n",
			       round, WTERMSIG(st),
			       WTERMSIG(st) == SIGALRM
			       ? " -- the payload fork HUNG (the parked-"
				 "unadopted-child wall)" : "");
			continue;
		}
		if (!WIFEXITED(st) || WEXITSTATUS(st) != 0) {
			int code = WIFEXITED(st) ? WEXITSTATUS(st) : -1;

			fails++;
			printf("FAIL round %d: sandbox exited %d (%s)\n",
			       round, code,
			       (code >= 10 && code <= 17 && why[code])
			       ? why[code] : "?");
		}
	}
	if (fails) {
		printf("FAIL %d of %d rounds\n", fails, ROUNDS);
		return 1;
	}
	printf("PASS\n");
	return 0;
}
