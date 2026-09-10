/*
 * rd1 -- the read direction of a MAP_SHARED file mapping.
 *
 * Not part of the tree's suite; a scratch probe for the two things the
 * file-writeback case does not reach.
 *
 *   store-then-forward  the guest stores into the mapping and then hands the
 *                       address to write(2). The bytes the descriptor receives
 *                       must be the ones stored, not whatever the file held.
 *   initial-guest       the guest reads the file's own bytes through the
 *                       mapping with its own instructions.
 *   initial-forward     ...and hands that address to write(2).
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <sys/mman.h>
#include <sys/wait.h>

#define PGSZ 4096
static int fails;

static void say(const char *s) { if (write(1, s, strlen(s)) < 0) _exit(1); }
static void sayf(const char *f, ...)
{
	char b[256]; va_list ap; va_start(ap, f);
	vsnprintf(b, sizeof(b), f, ap); va_end(ap); say(b);
}
static void check(const char *n, int ok, const char *f, ...)
{
	char b[256]; va_list ap; va_start(ap, f);
	vsnprintf(b, sizeof(b), f, ap); va_end(ap);
	if (!ok) fails++;
	sayf("%-16s %s  %s\n", n, ok ? "ok  " : "FAIL", b);
}

/* Send len bytes from p down a pipe and read them back. */
static int roundtrip(const void *p, size_t len, char *out)
{
	int fd[2];
	ssize_t n;

	if (pipe(fd) != 0)
		return -1;
	if (write(fd[1], p, len) != (ssize_t)len) {
		close(fd[0]); close(fd[1]);
		return -1;
	}
	n = read(fd[0], out, len);
	close(fd[0]); close(fd[1]);
	return n == (ssize_t)len ? 0 : -1;
}

int main(void)
{
	char path[] = "/tmp/rd1.XXXXXX";
	char got[9];
	int fd;
	unsigned char *p;

	fd = mkstemp(path);
	if (fd < 0 || ftruncate(fd, PGSZ) != 0) {
		say("rd1: mkstemp failed\nFAIL\n");
		return 1;
	}

	/* --- what the guest stores must be what a forwarded call reads. */
	p = mmap(NULL, PGSZ, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
	if (p == MAP_FAILED) {
		check("store-forward", 0, "mmap: %s", strerror(errno));
	} else {
		memcpy(p, "READTHIS", 8);
		memset(got, 0, sizeof(got));
		if (roundtrip(p, 8, got) != 0)
			check("store-forward", 0, "pipe round trip failed");
		else
			check("store-forward", memcmp(got, "READTHIS", 8) == 0,
			      "write(2) from the mapping delivered |%.8s|", got);
		munmap(p, PGSZ);
	}

	/* --- the file's own bytes, seen through a fresh mapping. */
	if (pwrite(fd, "INITIAL!", 8, 0) != 8) {
		check("initial", 0, "pwrite: %s", strerror(errno));
	} else {
		p = mmap(NULL, PGSZ, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
		if (p == MAP_FAILED) {
			check("initial", 0, "mmap: %s", strerror(errno));
		} else {
			memset(got, 0, sizeof(got));
			memcpy(got, p, 8);	/* the guest's own load */
			check("initial-guest", memcmp(got, "INITIAL!", 8) == 0,
			      "the program reads |%.8s| through the mapping",
			      got);
			memset(got, 0, sizeof(got));
			if (roundtrip(p, 8, got) != 0)
				check("initial-forward", 0, "round trip failed");
			else
				check("initial-forward",
				      memcmp(got, "INITIAL!", 8) == 0,
				      "write(2) from the mapping delivered "
				      "|%.8s|", got);
			munmap(p, PGSZ);
		}
	}
	/* --- a mapping nobody unmaps: the process ending is the write-back. */
	{
		char p2[] = "/tmp/rd1b.XXXXXX";
		int f2 = mkstemp(p2);
		pid_t kid;
		int st = 0;
		char buf[9] = { 0 };

		if (f2 < 0 || ftruncate(f2, PGSZ) != 0) {
			check("exit-writeback", 0, "mkstemp: %s", strerror(errno));
		} else {
			kid = fork();
			if (kid == 0) {
				unsigned char *q = mmap(NULL, PGSZ,
							PROT_READ | PROT_WRITE,
							MAP_SHARED, f2, 0);

				if (q == MAP_FAILED)
					_exit(2);
				memcpy(q, "ATTHEEND", 8);
				_exit(0);	/* no msync, no munmap */
			}
			waitpid(kid, &st, 0);
			if (pread(f2, buf, 8, 0) != 8)
				check("exit-writeback", 0, "pread: %s",
				      strerror(errno));
			else
				check("exit-writeback",
				      memcmp(buf, "ATTHEEND", 8) == 0,
				      "the file holds |%.8s| after the process "
				      "that mapped it ended", buf);
			close(f2);
		}
		unlink(p2);
	}

	close(fd);
	unlink(path);
	say(fails == 0 ? "PASS\n" : "FAIL\n");
	return fails == 0 ? 0 : 1;
}
