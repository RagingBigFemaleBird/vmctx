// SPDX-License-Identifier: GPL-2.0
/*
 * fx1 -- fork, then exec a DYNAMIC binary, and require its clean exit.
 *
 * The reduction of netsurf's current wall (session 41f): glycin's loader
 * spawn -- and even a plain `bwrap .. /usr/bin/true` availability probe, and
 * a /bin/bash shim -- dies SIGSEGV moments after the exec. The photographed
 * corpse (browser-ns-nosbx run): the child's rip inside a page mapped
 * `rwxs /memfd:vmctx-fork` where the fresh image's ld.so text belongs -- the
 * post-exec relayout left a fork-object mapping over the new image. Every
 * exec test in the suite execs from the ORIGINAL context; none forks first
 * and execs a dynamically-linked program, which is what every real spawn
 * (posix_spawn, popen, g_spawn) does.
 *
 * Three shapes, each repeated: fork+exec /usr/bin/true (dynamic), fork+exec
 * /bin/sh -c exit (dynamic, with its own heap work), and vfork-style
 * fork+exec of /usr/bin/env (reads environ). The parent requires WIFEXITED
 * with status 0 -- a SIGSEGV child is the wall, named with its round.
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <pthread.h>
#include <sys/wait.h>

#define ROUNDS 8

static int fails;

static void one(int round, const char *name, char *const argv[])
{
	pid_t kid = fork();
	int st;

	if (kid < 0) {
		fprintf(stderr, "fx1: round %d %s: fork: %m\n", round, name);
		fails++;
		return;
	}
	if (kid == 0) {
		execv(argv[0], argv);
		_exit(127);
	}
	if (waitpid(kid, &st, 0) != kid) {
		fprintf(stderr, "fx1: round %d %s: waitpid: %m\n", round, name);
		fails++;
		return;
	}
	if (WIFEXITED(st) && WEXITSTATUS(st) == 0)
		return;
	fails++;
	if (WIFSIGNALED(st))
		fprintf(stderr, "fx1: round %d %s: died by signal %d\n",
			round, name, WTERMSIG(st));
	else
		fprintf(stderr, "fx1: round %d %s: exited %d (status 0x%x)\n",
			round, name, WIFEXITED(st) ? WEXITSTATUS(st) : -1, st);
}

/*
 * The shape glycin actually has: the fork happens on a WORKER THREAD of a
 * threaded process (Rust async pools, GLib worker threads), not on main.
 * That is mf1/mf2's fork-from-threaded-parent territory with exec stacked
 * on top -- the shape every g_spawn/posix_spawn in a GUI program takes.
 */
static void *spawn_from_thread(void *arg)
{
	int round = (int)(long)arg;
	char *true_argv[] = { "/usr/bin/true", NULL };
	char *sh_argv[]   = { "/bin/sh", "-c", "exit 0", NULL };

	one(round, "thread+true", true_argv);
	one(round, "thread+sh", sh_argv);
	return NULL;
}

static volatile int keep_spinning = 1;

static void *background_noise(void *arg)
{
	/* A busy sibling thread, so the fork happens while another thread
	 * is RUNNING -- the contended half of the mf1 shape. */
	volatile unsigned x = 0;

	(void)arg;
	while (keep_spinning)
		x++;
	return NULL;
}

int main(void)
{
	char *true_argv[] = { "/usr/bin/true", NULL };
	char *sh_argv[]   = { "/bin/sh", "-c", "exit 0", NULL };
	char *env_argv[]  = { "/usr/bin/env", "true", NULL };
	/*
	 * The netsurf corpse's real shape (browser-ns-nosbx): the EXEC'D
	 * child itself FORKS -- bwrap forks its sandbox child after the
	 * unshare, a shell forks for every command word -- and THAT
	 * grandchild died at ld.so text overlaid by /memfd:vmctx-fork.
	 * fork-then-exec passes; exec-then-fork is the question.
	 */
	char *shfork_argv[]  = { "/bin/sh", "-c", "/usr/bin/true && /usr/bin/true", NULL };
	char *shdeep_argv[]  = { "/bin/sh", "-c", "/bin/sh -c /usr/bin/true", NULL };
	pthread_t noise, worker;
	int round;

	setvbuf(stdout, NULL, _IONBF, 0);
	setvbuf(stderr, NULL, _IONBF, 0);

	for (round = 0; round < ROUNDS; round++) {
		one(round, "true", true_argv);
		one(round, "sh", sh_argv);
		one(round, "env", env_argv);
		one(round, "sh-forks", shfork_argv);
		one(round, "sh-deep", shdeep_argv);
	}

	pthread_create(&noise, NULL, background_noise, NULL);
	for (round = 0; round < ROUNDS; round++) {
		pthread_create(&worker, NULL, spawn_from_thread,
			       (void *)(long)(100 + round));
		pthread_join(worker, NULL);
	}
	keep_spinning = 0;
	pthread_join(noise, NULL);

	fprintf(stderr, "fx1: %d rounds x (5 main + 2 thread) fork/exec shapes, %d wrong\n",
		ROUNDS, fails);
	printf(fails ? "FAIL\n" : "PASS\n");
	return fails ? 1 : 0;
}
