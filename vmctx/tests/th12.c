#define _GNU_SOURCE
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <pthread.h>

/*
 * Same line built two ways: one digit computed by hand into the buffer, one
 * produced by snprintf. If only the snprintf digit is lost, the fault is in
 * libc's conversion; if both are, it is the buffer or its transport.
 */
#define ROUNDS 30

static volatile int stop;

static void emit(const char *who, int i)
{
	char b[64];
	int n;

	b[0] = (char)('0' + (i % 10));	/* by hand */
	b[1] = ' ';
	n = 2 + snprintf(b + 2, sizeof b - 2, "%d %s\n", i % 10, who);
	if (write(1, b, (size_t)n) < 0)
		return;
}

static void *worker(void *p)
{
	int i = 0;

	(void)p;
	while (!stop)
		emit("thread", i++);
	return NULL;
}

int main(void)
{
	pthread_t t;
	int i;

	pthread_create(&t, NULL, worker, NULL);
	for (i = 0; i < ROUNDS; i++)
		emit("main", i);
	stop = 1;
	pthread_join(t, NULL);
	return 0;
}
