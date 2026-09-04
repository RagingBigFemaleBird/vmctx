/*
 * ex1 — the smallest thing that does what a sandbox does: fork, exec a
 * dynamically linked program, and see whether it survives its own dynamic
 * loader. A browser reaches this only after four minutes of fontconfig; this
 * reaches it in half a second.
 *
 *   ex1 /bin/echo hello
 *   ex1 /usr/bin/bwrap --unshare-all --ro-bind / / /bin/echo hello
 *
 * The second form is the shape that matters: bwrap execs a second program
 * after it has built the sandbox, so the context execs twice.
 */
#include <stdio.h>
#include <errno.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/wait.h>

int main(int argc, char **argv)
{
	pid_t p;
	int st = 0;

	if (argc < 2) {
		printf("usage: ex1 <program> [args...]\n");
		return 2;
	}
	p = fork();
	if (p == 0) {
		execv(argv[1], argv + 1);
		/*
		 * errno, not a fixed code, and the parent's printed status does
		 * mean this child.
		 *
		 * This comment used to say the opposite -- that wait4 was
		 * forwarded, so the status described the *shadow* child rather
		 * than this one. That was true once and is not now: fork is a
		 * local syscall, so this child is a task on the machine running
		 * the guest and holds that machine's pid, and wait4 is local for
		 * the same reason. ex5 checks the three numbers agree.
		 */
		_exit(errno);
	}
	if (p < 0) {
		printf("fork failed\n");
		return 1;
	}
	waitpid(p, &st, 0);
	if (WIFEXITED(st))
		printf("child exited %d\n", WEXITSTATUS(st));
	else
		printf("child killed by signal %d\n", WTERMSIG(st));
	fflush(stdout);
	return WIFEXITED(st) && WEXITSTATUS(st) == 0 ? 0 : 1;
}
