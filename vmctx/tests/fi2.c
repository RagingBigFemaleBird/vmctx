/*
 * fi2 — descriptors, and what a fork does to them.
 *
 * fi1 covers bytes and offsets on one descriptor in one process: round trips,
 * positioned I/O, a buffer straddling a page, a 16-page call, readv/writev,
 * O_APPEND, fstat and ftruncate. What it never does is have two descriptors, or
 * two processes, refer to the same open file.
 *
 * That is the interesting part for this design, because a descriptor is one of
 * the things the destination is not allowed to own. Every fd the guest holds
 * names an open file on the machine that owns the program, and the sharing
 * rules are that machine's: dup(2) shares a file *description*, so the two
 * numbers share one offset; fork(2) shares it too, so parent and child share
 * one offset; and open(2) twice does not, so those two do not. If the
 * destination were keeping any state of its own about descriptors, these are
 * the cases where it would disagree.
 *
 *   dup-offset     dup(2) then read on one fd; the other's offset moved too.
 *   dup2-offset    same through dup2 onto a chosen number.
 *   reopen-apart   the same file opened twice has two independent offsets.
 *   fork-offset    a child's read moves the parent's offset. This is the one a
 *                  forwarded fork has to get right: the child's context is
 *                  forked from the parent's while its descriptors are still
 *                  exactly what the child will inherit.
 *   fork-append    parent and child both O_APPEND-write; both writes survive
 *                  and neither overwrites the other.
 *   closed-is-bad  reading a closed descriptor gives EBADF rather than data.
 *                  The destination refuses some closes to protect its own
 *                  plumbing (sh_fd_guard), so "close then use" must still
 *                  behave for a descriptor that really is the guest's.
 *   unlinked-open  a file unlinked while open stays readable through the fd.
 *   short-at-eof   a read that runs off the end returns what is there, not
 *                  what was asked for, and not zeros.
 *
 * write(2) for the report, as elsewhere.
 */
#define _GNU_SOURCE
#include <string.h>
#include <stdio.h>
#include <stdarg.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <stdlib.h>
#include <sys/wait.h>
#include <sys/stat.h>

static int fails;
static char path[] = "/tmp/fi2.XXXXXX";

static void say(const char *s)
{
	if (write(1, s, strlen(s)) < 0)
		_exit(1);
}

static void sayf(const char *fmt, ...)
{
	char b[256];
	va_list ap;

	va_start(ap, fmt);
	vsnprintf(b, sizeof(b), fmt, ap);
	va_end(ap);
	say(b);
}

static void check(const char *name, int ok, const char *fmt, ...)
{
	char b[256];
	va_list ap;

	va_start(ap, fmt);
	vsnprintf(b, sizeof(b), fmt, ap);
	va_end(ap);
	if (ok) {
		sayf("%-14s ok    %s\n", name, b);
		return;
	}
	fails++;
	sayf("%-14s FAIL  %s\n", name, b);
}

/* Put the file back to a known 26 bytes before each case, so a case that
 * fails does not decide the next one. */
static int reset_file(void)
{
	int fd = open(path, O_RDWR | O_TRUNC);

	if (fd < 0)
		return -1;
	if (write(fd, "abcdefghijklmnopqrstuvwxyz", 26) != 26) {
		close(fd);
		return -1;
	}
	close(fd);
	return 0;
}

static void case_dup_offset(void)
{
	int a, b;
	char x[4] = { 0 }, y[4] = { 0 };
	off_t off_b;

	if (reset_file() < 0) {
		check("dup-offset", 0, "reset: %s", strerror(errno));
		return;
	}
	a = open(path, O_RDONLY);
	if (a < 0) {
		check("dup-offset", 0, "open: %s", strerror(errno));
		return;
	}
	b = dup(a);
	if (b < 0) {
		close(a);
		check("dup-offset", 0, "dup: %s", strerror(errno));
		return;
	}
	if (read(a, x, 4) != 4) {
		close(a); close(b);
		check("dup-offset", 0, "read a: %s", strerror(errno));
		return;
	}
	off_b = lseek(b, 0, SEEK_CUR);
	/* b must be at 4 and its read must continue where a stopped. */
	if (read(b, y, 4) != 4) {
		close(a); close(b);
		check("dup-offset", 0, "read b: %s", strerror(errno));
		return;
	}
	close(a);
	close(b);
	check("dup-offset", off_b == 4 && memcmp(x, "abcd", 4) == 0 &&
			    memcmp(y, "efgh", 4) == 0,
	      "b-offset=%lld a=%.4s b=%.4s (want 4, abcd, efgh)",
	      (long long)off_b, x, y);
}

static void case_dup2_offset(void)
{
	int a, b = 33;
	char y[4] = { 0 };
	off_t off_b;

	if (reset_file() < 0) {
		check("dup2-offset", 0, "reset: %s", strerror(errno));
		return;
	}
	a = open(path, O_RDONLY);
	if (a < 0) {
		check("dup2-offset", 0, "open: %s", strerror(errno));
		return;
	}
	if (dup2(a, b) != b) {
		close(a);
		check("dup2-offset", 0, "dup2: %s", strerror(errno));
		return;
	}
	lseek(a, 10, SEEK_SET);
	off_b = lseek(b, 0, SEEK_CUR);
	if (read(b, y, 4) != 4) {
		close(a); close(b);
		check("dup2-offset", 0, "read: %s", strerror(errno));
		return;
	}
	close(a);
	close(b);
	check("dup2-offset", off_b == 10 && memcmp(y, "klmn", 4) == 0,
	      "b-offset=%lld b=%.4s (want 10, klmn)", (long long)off_b, y);
}

static void case_reopen_apart(void)
{
	int a, b;
	char y[4] = { 0 };

	if (reset_file() < 0) {
		check("reopen-apart", 0, "reset: %s", strerror(errno));
		return;
	}
	a = open(path, O_RDONLY);
	b = open(path, O_RDONLY);
	if (a < 0 || b < 0) {
		check("reopen-apart", 0, "open: %s", strerror(errno));
		if (a >= 0) close(a);
		if (b >= 0) close(b);
		return;
	}
	lseek(a, 10, SEEK_SET);
	if (read(b, y, 4) != 4) {
		close(a); close(b);
		check("reopen-apart", 0, "read: %s", strerror(errno));
		return;
	}
	close(a);
	close(b);
	/* b is a separate description: it must still be at 0. */
	check("reopen-apart", memcmp(y, "abcd", 4) == 0,
	      "b=%.4s (want abcd; klmn means the two share an offset)", y);
}

static void case_fork_offset(void)
{
	int fd;
	pid_t kid;
	char y[4] = { 0 };
	off_t after;
	int st = 0;

	if (reset_file() < 0) {
		check("fork-offset", 0, "reset: %s", strerror(errno));
		return;
	}
	fd = open(path, O_RDONLY);
	if (fd < 0) {
		check("fork-offset", 0, "open: %s", strerror(errno));
		return;
	}
	kid = fork();
	if (kid < 0) {
		close(fd);
		check("fork-offset", 0, "fork: %s", strerror(errno));
		return;
	}
	if (kid == 0) {
		char c[8];

		_exit(read(fd, c, 8) == 8 ? 0 : 1);
	}
	waitpid(kid, &st, 0);
	after = lseek(fd, 0, SEEK_CUR);
	if (read(fd, y, 4) != 4) {
		close(fd);
		check("fork-offset", 0, "parent read: %s", strerror(errno));
		return;
	}
	close(fd);
	check("fork-offset", WIFEXITED(st) && WEXITSTATUS(st) == 0 &&
			     after == 8 && memcmp(y, "ijkl", 4) == 0,
	      "child=%d parent-offset=%lld parent=%.4s (want 0, 8, ijkl)",
	      WIFEXITED(st) ? WEXITSTATUS(st) : -1, (long long)after, y);
}

static void case_fork_append(void)
{
	int fd;
	pid_t kid;
	struct stat sb;
	int st = 0;
	char buf[64];
	int n, saw_p = 0, saw_c = 0, i;

	if (reset_file() < 0) {
		check("fork-append", 0, "reset: %s", strerror(errno));
		return;
	}
	fd = open(path, O_WRONLY | O_APPEND);
	if (fd < 0) {
		check("fork-append", 0, "open: %s", strerror(errno));
		return;
	}
	kid = fork();
	if (kid < 0) {
		close(fd);
		check("fork-append", 0, "fork: %s", strerror(errno));
		return;
	}
	if (kid == 0) {
		_exit(write(fd, "CCCC", 4) == 4 ? 0 : 1);
	}
	waitpid(kid, &st, 0);
	if (write(fd, "PPPP", 4) != 4) {
		close(fd);
		check("fork-append", 0, "parent write: %s", strerror(errno));
		return;
	}
	close(fd);

	if (stat(path, &sb) != 0) {
		check("fork-append", 0, "stat: %s", strerror(errno));
		return;
	}
	fd = open(path, O_RDONLY);
	n = fd < 0 ? -1 : (int)read(fd, buf, sizeof(buf));
	if (fd >= 0)
		close(fd);
	for (i = 0; i + 4 <= n; i++) {
		if (!memcmp(buf + i, "CCCC", 4)) saw_c = 1;
		if (!memcmp(buf + i, "PPPP", 4)) saw_p = 1;
	}
	/* Both appends must survive: 26 + 4 + 4. */
	check("fork-append", sb.st_size == 34 && saw_c && saw_p,
	      "size=%lld child-write=%d parent-write=%d (want 34, 1, 1)",
	      (long long)sb.st_size, saw_c, saw_p);
}

static void case_closed_is_bad(void)
{
	int fd;
	char c[4];
	ssize_t n;
	int e;

	if (reset_file() < 0) {
		check("closed-is-bad", 0, "reset: %s", strerror(errno));
		return;
	}
	fd = open(path, O_RDONLY);
	if (fd < 0) {
		check("closed-is-bad", 0, "open: %s", strerror(errno));
		return;
	}
	if (close(fd) != 0) {
		check("closed-is-bad", 0, "close: %s", strerror(errno));
		return;
	}
	errno = 0;
	n = read(fd, c, 4);
	e = errno;
	check("closed-is-bad", n < 0 && e == EBADF,
	      "read=%zd errno=%d (want -1, EBADF=%d)", n, e, EBADF);
}

static void case_unlinked_open(void)
{
	char tmp[] = "/tmp/fi2u.XXXXXX";
	int fd = mkstemp(tmp);
	char y[4] = { 0 };
	ssize_t n;

	if (fd < 0) {
		check("unlinked-open", 0, "mkstemp: %s", strerror(errno));
		return;
	}
	if (write(fd, "wxyz", 4) != 4 || lseek(fd, 0, SEEK_SET) != 0) {
		close(fd);
		unlink(tmp);
		check("unlinked-open", 0, "prepare: %s", strerror(errno));
		return;
	}
	if (unlink(tmp) != 0) {
		close(fd);
		check("unlinked-open", 0, "unlink: %s", strerror(errno));
		return;
	}
	n = read(fd, y, 4);
	close(fd);
	check("unlinked-open", n == 4 && memcmp(y, "wxyz", 4) == 0,
	      "read=%zd got=%.4s (want 4, wxyz)", n, y);
}

static void case_short_at_eof(void)
{
	int fd;
	char big[64];
	ssize_t n;

	if (reset_file() < 0) {
		check("short-at-eof", 0, "reset: %s", strerror(errno));
		return;
	}
	fd = open(path, O_RDONLY);
	if (fd < 0) {
		check("short-at-eof", 0, "open: %s", strerror(errno));
		return;
	}
	if (lseek(fd, 20, SEEK_SET) != 20) {
		close(fd);
		check("short-at-eof", 0, "lseek: %s", strerror(errno));
		return;
	}
	memset(big, 'Z', sizeof(big));
	n = read(fd, big, sizeof(big));	/* only 6 bytes left */
	close(fd);
	/*
	 * The bytes past the end must be untouched. A path that answered a
	 * short read by zero-filling the rest of the buffer would still return
	 * 6 here, so the buffer is what says whether it happened.
	 */
	check("short-at-eof", n == 6 && memcmp(big, "uvwxyz", 6) == 0 &&
			      big[6] == 'Z' && big[63] == 'Z',
	      "read=%zd got=%.6s tail=%c (want 6, uvwxyz, Z)",
	      n, big, big[6]);
}

int main(void)
{
	int fd = mkstemp(path);

	if (fd < 0) {
		sayf("mkstemp: %s\nFAIL\n", strerror(errno));
		return 1;
	}
	close(fd);

	case_dup_offset();
	case_dup2_offset();
	case_reopen_apart();
	case_fork_offset();
	case_fork_append();
	case_closed_is_bad();
	case_unlinked_open();
	case_short_at_eof();

	unlink(path);
	say(fails == 0 ? "PASS\n" : "FAIL\n");
	return fails == 0 ? 0 : 1;
}
