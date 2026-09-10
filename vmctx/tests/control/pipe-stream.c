// SPDX-License-Identifier: GPL-2.0
/* Regression for corruption in groff's process pipeline. Each reader checks
 * every source-written byte before transforming it. Exercise inherited and
 * exec-replaced address spaces, short reads, page boundaries and EOF/status. */
#define _GNU_SOURCE
#include <errno.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/prctl.h>
#include <sys/wait.h>
#include <unistd.h>
#define STAGES 4
#define TOTAL (256 * 1024 + 173)
static unsigned char inherited[3 * 4096];
static unsigned char pattern(size_t pos, unsigned stage)
{
	return (unsigned char)((pos * 131 + (pos >> 8) * 17 + 29) ^ (stage & 1 ? 0x5a : 0));
}
static int write_all(int fd, const unsigned char *p, size_t n)
{
	while (n) {
		ssize_t r = write(fd, p, n);
		if (r < 0 && errno == EINTR) continue;
		if (r <= 0) return -1;
		p += r; n -= r;
	}
	return 0;
}
static int stage_run(unsigned stage, int in, int out)
{
	unsigned char *buf = inherited + 4096 - 37;
	size_t pos = 0;
	for (;;) {
		memset(inherited, 0xa5 + stage, sizeof(inherited));
		ssize_t n = read(in, buf, 4096 + 113);
		if (n < 0 && errno == EINTR) continue;
		if (n < 0 || (size_t)n > TOTAL - pos) goto fail;
		if (!n) break;
		for (size_t i = 0; i < sizeof(inherited); i++) {
			if (i >= 4096 - 37 && i < 4096 - 37 + (size_t)n) continue;
			if (inherited[i] != (unsigned char)(0xa5 + stage)) {
				fprintf(stderr, "FAIL: pipeline stage=%u guard=%zu got=%02x\n", stage, i, inherited[i]);
				return 1;
			}
		}
		for (ssize_t i = 0; i < n; i++) {
			unsigned char want = pattern(pos + i, stage);
			if (buf[i] != want) {
				fprintf(stderr, "FAIL: pipeline stage=%u offset=%zu got=%02x want=%02x\n",
					stage, pos + i, buf[i], want);
				return 1;
			}
			buf[i] ^= 0x5a;
		}
		if (write_all(out, buf, n)) goto fail;
		pos += n;
	}
	if (pos != TOTAL) goto fail;
	return 0;
fail:
	fprintf(stderr, "FAIL: pipeline stage=%u position=%zu errno=%d\n", stage, pos, errno);
	return 1;
}
static int run_pipeline(int replace)
{
	int pipes[STAGES + 1][2];
	pid_t children[STAGES + 1], parent = getpid();
	for (int i = 0; i <= STAGES; i++) if (pipe(pipes[i])) return 1;
	memset(inherited, 0x3c, sizeof(inherited));
	for (int i = 0; i <= STAGES; i++) {
		children[i] = fork();
		if (children[i] < 0) return 1;
		if (children[i]) continue;
		if (prctl(PR_SET_PDEATHSIG, SIGKILL) || getppid() != parent) _exit(125);
		int in = i ? pipes[i - 1][0] : -1, out = pipes[i][1];
		for (int j = 0; j <= STAGES; j++) {
			if (pipes[j][0] != in) close(pipes[j][0]);
			if (pipes[j][1] != out) close(pipes[j][1]);
		}
		if (i) {
			if (replace) {
				char a[16], b[16], c[16];
				snprintf(a, sizeof(a), "%d", i - 1);
				snprintf(b, sizeof(b), "%d", in);
				snprintf(c, sizeof(c), "%d", out);
				execl("/proc/self/exe", "pipe-stream", "stage", a, b, c, NULL);
				_exit(126);
			}
			_exit(stage_run(i - 1, in, out));
		}
		unsigned char data[4096 + 211];
		for (size_t pos = 0; pos < TOTAL;) {
			size_t n = pos & 4096 ? 157 : sizeof(data);
			if (n > TOTAL - pos) n = TOTAL - pos;
			for (size_t j = 0; j < n; j++) data[j] = pattern(pos + j, 0);
			if (write_all(out, data, n)) _exit(1);
			pos += n;
		}
		_exit(0);
	}
	for (int i = 0; i <= STAGES; i++) {
		close(pipes[i][1]);
		if (i != STAGES) close(pipes[i][0]);
	}
	unsigned char buf[4096 + 73]; size_t pos = 0; int fail = 0;
	for (;;) {
		ssize_t n = read(pipes[STAGES][0], buf, sizeof(buf));
		if (n < 0 && errno == EINTR) continue;
		if (n <= 0) { if (n < 0) fail = 1; break; }
		for (ssize_t j = 0; j < n; j++)
			if (buf[j] != pattern(pos + j, STAGES)) fail = 1;
		pos += n;
	}
	close(pipes[STAGES][0]);
	for (int i = 0; i <= STAGES; i++) {
		int status = 0; pid_t p;
		do { p = waitpid(children[i], &status, 0); } while (p < 0 && errno == EINTR);
		if (p != children[i] || !WIFEXITED(status) || WEXITSTATUS(status)) {
			fprintf(stderr, "FAIL: pipeline exec=%d child=%d status=%x\n", replace, i, status);
			fail = 1;
		}
	}
	if (pos != TOTAL) fail = 1;
	if (fail) fprintf(stderr, "FAIL: pipeline exec=%d received=%zu expected=%u\n", replace, pos, TOTAL);
	return fail;
}
int main(int argc, char **argv)
{
	alarm(45);
	if (argc == 5 && !strcmp(argv[1], "stage"))
		return stage_run(strtoul(argv[2], NULL, 10), atoi(argv[3]), atoi(argv[4]));
	if (argc != 1) return 2;
	if (run_pipeline(0) || run_pipeline(1)) return 1;
	puts("PASS: process pipelines preserve every byte, guards and EOF across fork and exec");
	return 0;
}
