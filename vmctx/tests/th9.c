#define _GNU_SOURCE
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <pthread.h>

/*
 * Two contexts making forwarded syscalls at the same time. Each line is
 * self-describing, so a corrupted one is obvious without comparing runs.
 */
#define ROUNDS 40

static void emit(const char *who, int i)
{
	char b[64];
	int n = snprintf(b, sizeof b, "%s %d ok\n", who, i);

	if (write(1, b, (size_t)n) < 0)
		return;
}

static void *worker(void *p)
{
	int i;

	(void)p;
	for (i = 0; i < ROUNDS; i++)
		emit("thread", i);
	return NULL;
}

int main(void)
{
	pthread_t t;
	int i;

	pthread_create(&t, NULL, worker, NULL);
	for (i = 0; i < ROUNDS; i++)
		emit("main  ", i);
	pthread_join(t, NULL);
	if (write(1, "DONE\n", 5) < 0)
		return 1;
	return 0;
}
