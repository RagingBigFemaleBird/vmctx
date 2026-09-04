// SPDX-License-Identifier: GPL-2.0
/*
 * ns1 — a child forked straight into a new user namespace must still be able to
 * run.
 *
 * `clone(CLONE_NEWUSER | SIGCHLD)` is a fork and a namespace creation in one
 * step, and it is how every sandbox asks "may I have a user namespace?" -- the
 * child is made, it reports, it exits. firefox's SandboxInfo does exactly this
 * before it will start a content process.
 *
 * Under vmctx that is not one operation but three, on two machines: the guest
 * clones, the destination has to build a context for the child, and the source
 * has to fork a shadow for it AND put that shadow in the matching namespace --
 * because the clone executes on the destination, so the namespace comes into
 * being over there while the address space it belongs to lives over here.
 * vmremote passes the namespace bits across for that reason
 * (guest_clone_ns_flags -> VMR_OP_FORKPREP -> the unshare in vmhome's fork path).
 *
 * Measured on firefox, six runs, byte-identical: the child of this exact call
 * dies on an INSTRUCTION FETCH (err 0x14) at an address it took off its own
 * inherited stack -- 0x2e8e6, with rsp on the main stack -- before it executes
 * a single syscall. The parent sees SIGSEGV and reports
 * "CanCreateUserNamespace() child process failure 0000008b".
 *
 * Neither ingredient alone reproduces it, which is why this test carries both.
 * mf1 forks a threaded parent with a plain fork() and passes; a bare
 * clone(CLONE_NEWUSER|SIGCHLD) from a single-threaded parent passes too. What
 * firefox actually does is the PAIR -- it creates its threads with clone3 and
 * CLONE_THREAD first, and asks for the namespace afterwards -- so the sibling
 * contexts exist, holding pages of the address space, at the moment the
 * namespace child is built. That combination is what this runs.
 *
 * The child's job is only to prove it can run: return through a call chain of
 * its own and read a canary written on the stack before the clone. Its exit
 * status carries the verdict, and the parent prints it, so a child that dies by
 * signal is reported as such rather than as silence.
 *
 * WHAT IT ACTUALLY CATCHES, measured rather than assumed. A failing round prints
 * nothing, which reads as a hang and is not one: the run ends in about four
 * seconds with
 *
 *   Fatal glibc error: malloc.c:2371 (sysmalloc): assertion failed:
 *   (old_top == initial_top (av) && old_size == 0) || ...prev_inuse (old_top)...
 *
 * -- glibc's own check that the heap's top chunk is intact. So the failure is
 * MEMORY CORRUPTION in the parent, not a child that cannot start and not a
 * deadlock. Every syscall after the abort answers -5, because the context is
 * being torn down and its monitor is gone; that EIO storm is the consequence and
 * reading it as the cause wastes a pass. The last events before it are an mmap
 * and an ABSENT for the fresh page, which is where to look.
 *
 * It is intermittent -- roughly one round in three to eight -- and it needs load
 * or the state of a box that has run other cases first. Run it in a loop rather
 * than once, and read home.log for the glibc line rather than trusting an empty
 * guest.out to mean "hung".
 *
 * CLONE_NEWUSER needs no privilege on a kernel with unprivileged user
 * namespaces, and the suite runs as root in any case. If the kernel refuses the
 * namespace outright the test says SKIP and passes -- the thing being tested is
 * what happens when the child EXISTS, not whether this box allows it.
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <sched.h>
#include <pthread.h>
#include <stdint.h>
#include <sys/syscall.h>
#include <sys/wait.h>

#define NTHREADS	4

static volatile int stop;

/* Deep enough to span pages, written every pass, so the sibling stacks are
 * live memory held by sibling contexts when the namespace child is made. */
static unsigned burn(int depth, unsigned acc)
{
	volatile unsigned pad[64];
	unsigned i;

	for (i = 0; i < 64; i++)
		pad[i] = acc + i;
	if (depth > 0)
		acc = burn(depth - 1, acc + pad[depth & 63]);
	return acc + pad[0];
}

static void *spin(void *arg)
{
	unsigned acc = (unsigned)(uintptr_t)arg;

	while (!stop)
		acc = burn(8, acc);
	return NULL;
}

#define CANARY	0x1abe11edu

static unsigned deep(const volatile unsigned *p, int d)
{
	if (d > 0)
		return deep(p, d - 1);
	return *p;
}

int main(void)
{
	volatile unsigned canary = CANARY;
	pthread_t th[NTHREADS];
	long kid;
	int st = 0, i, made = 0;

	/* The threads first, exactly as the browser has them first. */
	for (i = 0; i < NTHREADS; i++)
		if (pthread_create(&th[i], NULL, spin,
				   (void *)(uintptr_t)(i + 1)) == 0)
			made++;
	if (made != NTHREADS) {
		printf("FAIL could only start %d of %d threads\n", made,
		       NTHREADS);
		return 2;
	}
	usleep(50000);		/* let them dirty their own stacks */

	/*
	 * The legacy clone, with the flags firefox uses, so this is the same
	 * call and not a near relative: the low byte is the signal to send the
	 * parent on exit, and CLONE_NEWUSER is the whole point.
	 *
	 * Raw, because glibc's fork() cannot express it and clone(2)'s wrapper
	 * wants a child stack -- which would move the child off the stack it
	 * inherited and hide exactly the failure this exists to catch.
	 */
	kid = syscall(SYS_clone, (unsigned long)(CLONE_NEWUSER | SIGCHLD),
		      0UL, 0UL, 0UL, 0UL);
	if (kid < 0) {
		if (errno == EINVAL || errno == EPERM) {
			printf("SKIP clone(CLONE_NEWUSER) refused: %s\n",
			       strerror(errno));
			printf("PASS\n");
			return 0;
		}
		printf("FAIL clone: %s\n", strerror(errno));
		return 2;
	}
	if (kid == 0) {
		/*
		 * The child. It runs on the stack it inherited, in a namespace
		 * that did not exist a moment ago. Anything at all it manages
		 * to do is more than firefox's child manages.
		 */
		if (deep(&canary, 12) != CANARY)
			_exit(3);
		_exit(0);
	}

	if (waitpid((pid_t)kid, &st, 0) != (pid_t)kid) {
		printf("FAIL waitpid: %s\n", strerror(errno));
		stop = 1;
		return 2;
	}
	stop = 1;
	for (i = 0; i < NTHREADS; i++)
		pthread_join(th[i], NULL);
	if (WIFSIGNALED(st)) {
		printf("FAIL child died by signal %d -- it could not run the "
		       "stack it inherited through clone(CLONE_NEWUSER)\n",
		       WTERMSIG(st));
		return 1;
	}
	if (!WIFEXITED(st) || WEXITSTATUS(st) != 0) {
		printf("FAIL child exited %d (3 = wrong canary on its own "
		       "stack)\n", WIFEXITED(st) ? WEXITSTATUS(st) : -1);
		return 1;
	}
	printf("PASS\n");
	return 0;
}
