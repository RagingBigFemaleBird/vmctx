/*
 * Do the pids a guest is handed actually name its children?
 *
 * This is the question behind where wait4 belongs. If fork were forwarded, the
 * child would be a process on the *source* and the pid the guest holds would
 * mean nothing on the machine it runs on -- answering wait4 locally would then
 * be nonsense, and it would have to be forwarded so the side that owns the pid
 * could answer.
 *
 * Three numbers have to agree: what fork returned to the parent, what the child
 * says its own pid is, and what wait4 reports having reaped. Plus the exit
 * status, because a wait4 that reaps the *wrong* child still returns a
 * plausible-looking pid.
 *
 * PIDS-DISAGREE is the expected result on the old model, and the test earning
 * its keep. getpid is answered at the source -- a pid is an identity in the
 * source's world and the destination owns nothing a program can name -- while a
 * local clone hands the guest the pid of the task the *destination's* fork made.
 * So the child correctly reports the number the source forked and the parent
 * holds a different one:
 *
 *     fork returned 354211, the child says it is 354210, wait4 reaped 354211
 *     PIDS-DISAGREE
 *
 * 354210 is what the source forked; 354211 is the destination's. Earlier this
 * test printed PIDS-AGREE, not because anything was right but because every
 * number it compared came from the same wrong machine. Under
 * VMCTX_SOURCE_CLONE the guest is told 354210 and the three agree.
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <sys/wait.h>

int main(void)
{
	int fds[2];
	pid_t kid, got;
	int st = 0;
	pid_t child_says = 0;

	if (pipe(fds) < 0) {
		perror("pipe");
		return 1;
	}
	kid = fork();
	if (kid < 0) {
		perror("fork");
		return 1;
	}
	if (kid == 0) {
		pid_t me = getpid();

		close(fds[0]);
		if (write(fds[1], &me, sizeof(me)) != (ssize_t)sizeof(me))
			_exit(1);
		close(fds[1]);
		_exit(42);
	}
	close(fds[1]);
	if (read(fds[0], &child_says, sizeof(child_says)) != (ssize_t)sizeof(child_says)) {
		printf("FAIL could not read the child's own pid\n");
		return 1;
	}
	got = wait4(kid, &st, 0, NULL);

	printf("fork returned %d, the child says it is %d, wait4 reaped %d, status %d\n",
	       (int)kid, (int)child_says, (int)got,
	       WIFEXITED(st) ? WEXITSTATUS(st) : -1);

	if (kid == child_says && got == kid && WIFEXITED(st) && WEXITSTATUS(st) == 42)
		printf("PIDS-AGREE wait4 belongs where fork is\n");
	else
		printf("PIDS-DISAGREE fork=%d child=%d wait4=%d status=%d\n",
		       (int)kid, (int)child_says, (int)got,
		       WIFEXITED(st) ? WEXITSTATUS(st) : -1);
	fflush(stdout);
	return 0;
}
