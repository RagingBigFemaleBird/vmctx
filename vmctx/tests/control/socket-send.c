// SPDX-License-Identifier: GPL-2.0
/* Exercise the executor's real transport helpers without a vmctx kernel.
 * cc -O2 -pthread -I../../kernel socket-send.c -o socket-send
 */
#define main vmremote_program_main
#include "../../user/vmremote.c"
#undef main
#include <assert.h>

static int transmit(int helper, char *data, size_t length)
{
	switch (helper) {
	case 0: return pg_rw(sock, data, length, 1);
	case 1: return io_all(1, data, length);
	default: return io_send2(data, 37, data + 37, length - 37);
	}
}

static void *receive_payload(void *arg)
{
	int fd = *(int *)arg;
	unsigned char buf[997];
	size_t offset = 0;
	ssize_t n;
	while ((n = read(fd, buf, sizeof(buf))) > 0) {
		for (ssize_t i = 0; i < n; i++, offset++)
			assert(buf[i] == (unsigned char)(offset * 17 + 3));
	}
	assert(n == 0 && offset == 1024 * 1024);
	return NULL;
}

int main(void)
{
	int failed = 0;
	for (int helper = 0; helper < 3; helper++) {
		/* A disconnected page channel must fail this operation, not kill
		 * the monitor and every unrelated guest it services. */
		pid_t child = fork();
		assert(child >= 0);
		if (!child) {
			int fd[2];
			char data[80] = {0};
			assert(signal(SIGPIPE, SIG_DFL) != SIG_ERR);
			sigset_t unblocked;
			sigemptyset(&unblocked);
			sigaddset(&unblocked, SIGPIPE);
			assert(!pthread_sigmask(SIG_UNBLOCK, &unblocked, NULL));
			assert(!socketpair(AF_UNIX, SOCK_STREAM, 0, fd));
			sock = fd[0];
			close(fd[1]);
			errno = 0;
			int rc = transmit(helper, data, sizeof(data));
			_exit(rc == -1 && errno == EPIPE ? 0 : 1);
		}
		int status;
		assert(waitpid(child, &status, 0) == child);
		if (!WIFEXITED(status) || WEXITSTATUS(status)) {
			fprintf(stderr, "FAIL: socket helper %d closed-peer status=%#x\n",
				helper, status);
			failed = 1;
		}

		/* A small send buffer and a larger payload also cover completion
		 * of data split across multiple native socket operations. */
		int fd[2], small = 1024;
		pthread_t reader;
		char *data = malloc(1024 * 1024);
		assert(data && !socketpair(AF_UNIX, SOCK_STREAM, 0, fd));
		assert(!setsockopt(fd[0], SOL_SOCKET, SO_SNDBUF, &small, sizeof(small)));
		for (size_t i = 0; i < 1024 * 1024; i++) data[i] = i * 17 + 3;
		sock = fd[0];
		assert(!pthread_create(&reader, NULL, receive_payload, &fd[1]));
		assert(transmit(helper, data, 1024 * 1024) == 0);
		assert(!shutdown(sock, SHUT_WR));
		assert(!pthread_join(reader, NULL));
		close(fd[0]); close(fd[1]); free(data);
	}
	if (!failed) puts("PASS: executor socket writes survive closed peers and preserve payloads");
	return failed;
}
