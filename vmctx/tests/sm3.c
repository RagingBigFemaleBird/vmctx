/*
 * sm3 -- the case sm2 cannot distinguish.
 *
 * Two processes map one file by name, but at DIFFERENT addresses. sm2 maps at
 * whatever mmap picks, which is the same address in both processes, so a page
 * moved from one context to the other by address makes it look shared even when
 * the two mappings read from two different objects. Fixed at two addresses,
 * only real object sharing can pass.
 */
#define _GNU_SOURCE
#include <string.h>
#include <stdio.h>
#include <stdarg.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <stdlib.h>
#include <signal.h>
#include <sys/mman.h>
#include <sys/wait.h>

#define PGSZ 4096
static char path[] = "/tmp/sm3f.XXXXXX";

static void say(const char *fmt, ...)
{
	char b[256];
	va_list ap;

	va_start(ap, fmt);
	vsnprintf(b, sizeof(b), fmt, ap);
	va_end(ap);
	if (write(1, b, strlen(b)) < 0)
		_exit(1);
}

static volatile unsigned char *map_at(unsigned long where)
{
	int fd = open(path, O_RDWR);
	void *p;

	if (fd < 0)
		return NULL;
	p = mmap((void *)where, PGSZ, PROT_READ | PROT_WRITE,
		 MAP_SHARED | MAP_FIXED, fd, 0);
	close(fd);
	return p == MAP_FAILED ? NULL : p;
}

int main(void)
{
	int down[2], up[2], fd, st = 0, ok;
	volatile unsigned char *p;
	pid_t kid;
	char c = 'x';

	fd = mkstemp(path);
	if (fd < 0 || ftruncate(fd, PGSZ) != 0) {
		say("mkstemp/ftruncate: %s\nFAIL\n", strerror(errno));
		return 1;
	}
	close(fd);
	if (pipe(down) != 0 || pipe(up) != 0) {
		say("pipe: %s\nFAIL\n", strerror(errno));
		return 1;
	}
	kid = fork();
	if (kid < 0) {
		say("fork: %s\nFAIL\n", strerror(errno));
		return 1;
	}
	if (kid == 0) {
		close(down[1]);
		close(up[0]);
		p = map_at(0x60000000UL);	/* a different address */
		if (!p)
			_exit(2);
		if (read(down[0], &c, 1) != 1)
			_exit(3);
		if (p[0] != 0xA1)
			_exit(4);
		p[2048] = 0xC2;
		if (write(up[1], &c, 1) != 1)
			_exit(5);
		if (read(down[0], &c, 1) != 1)
			_exit(6);
		_exit(0);
	}
	close(down[0]);
	close(up[1]);
	p = map_at(0x50000000UL);
	if (!p) {
		say("parent could not map: %s\nFAIL\n", strerror(errno));
		kill(kid, SIGKILL);
		waitpid(kid, &st, 0);
		unlink(path);
		return 1;
	}
	p[0] = 0xA1;
	if (write(down[1], &c, 1) != 1 || read(up[0], &c, 1) != 1) {
		say("sequencing failed\nFAIL\n");
		kill(kid, SIGKILL);
		waitpid(kid, &st, 0);
		unlink(path);
		return 1;
	}
	ok = (p[2048] == 0xC2);
	(void)!write(down[1], &c, 1);
	waitpid(kid, &st, 0);
	unlink(path);

	say("child-saw-parent  %s\n",
	    (WIFEXITED(st) && WEXITSTATUS(st) != 4) ? "ok" : "FAIL");
	say("parent-saw-child  %s (byte %02x, want c2)\n",
	    ok ? "ok" : "FAIL", p[2048]);
	if (!WIFEXITED(st) || (WEXITSTATUS(st) != 0 && WEXITSTATUS(st) != 4))
		say("child exited oddly: status 0x%x\n", st);
	ok = ok && WIFEXITED(st) && WEXITSTATUS(st) == 0;
	say(ok ? "PASS\n" : "FAIL\n");
	return ok ? 0 : 1;
}
