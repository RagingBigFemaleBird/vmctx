/*
 * fi1 — file operations, which under vmctx happen on a machine the program
 * cannot see.
 *
 * Every one of these is forwarded: the descriptor is the source's, the bytes
 * are the source's, and the offsets are kept there too. So the questions worth
 * asking are not about POSIX -- they are about whether the two sides agree
 * about a file's *state* after each call, and whether a buffer that crosses the
 * machine boundary crosses it whole.
 *
 *   write/read round trip     the bytes come back byte for byte
 *   the offset is one thing   interleaving read, write and lseek must move a
 *                             single shared offset, not one per machine
 *   pread/pwrite              positioned I/O must not move it at all
 *   a buffer of odd size      a length that is not a multiple of anything, and
 *                             a buffer deliberately spanning a page boundary,
 *                             because the buffer is fetched a page at a time
 *   large in one call         64 KiB in a single write and a single read, which
 *                             is sixteen pages of guest memory to be gathered
 *   truncate and stat         the size the source reports must be the size the
 *                             guest's own operations produced
 *   readv/writev              a scattered buffer, each piece a separate range
 *   append                    O_APPEND puts the offset at the end on the
 *                             machine holding the file, every time
 *
 * Self-checking: every case prints ok or BAD with what it saw, and the program
 * ends with PASS or FAIL. Written with write(2) rather than stdio for the same
 * reason as the fault tests -- a case that ends the context should still leave
 * a record of how far it got.
 */
#define _GNU_SOURCE
#include <string.h>
#include <stdio.h>
#include <stdarg.h>
#include <stdlib.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/uio.h>
#include <sys/mman.h>

#define BIG (64 * 1024)

static int fails;

static void say(const char *s)
{
	if (write(1, s, strlen(s)) < 0)
		_exit(1);
}

static void sayf(const char *fmt, ...)
{
	char b[512];
	va_list ap;
	int n;

	va_start(ap, fmt);
	n = vsnprintf(b, sizeof(b), fmt, ap);
	va_end(ap);
	if (n > 0)
		say(b);
}

static void check(const char *what, int ok, const char *fmt, ...)
{
	char b[384];
	va_list ap;

	va_start(ap, fmt);
	vsnprintf(b, sizeof(b), fmt, ap);
	va_end(ap);
	if (!ok)
		fails++;
	sayf("fi1 %s: %s -> %s\n", what, b, ok ? "ok" : "BAD");
}

/* A byte pattern that makes a shifted or truncated copy obvious. */
static void fill(char *p, size_t n, unsigned seed)
{
	size_t i;

	for (i = 0; i < n; i++)
		p[i] = (char)(seed + i * 7 + (i >> 8));
}

int main(void)
{
	char path[] = "/tmp/fi1.XXXXXX";
	char *big, *back;
	struct stat st;
	int fd;
	ssize_t n;

	fd = mkstemp(path);
	if (fd < 0) {
		say("fi1: mkstemp failed\nFAIL\n");
		return 1;
	}

	big  = malloc(BIG);
	back = malloc(BIG);
	if (!big || !back) {
		say("fi1: malloc failed\nFAIL\n");
		return 1;
	}

	/* --- write then read back, plainly. */
	{
		const char *msg = "the quick brown fox jumps over the lazy dog";
		char got[64] = { 0 };
		size_t len = strlen(msg);

		n = write(fd, msg, len);
		lseek(fd, 0, SEEK_SET);
		n = (n == (ssize_t)len) ? read(fd, got, sizeof(got)) : -1;
		check("round-trip", n == (ssize_t)len &&
		      memcmp(got, msg, len) == 0,
		      "wrote %zu, read %zd, %s", len, n,
		      memcmp(got, msg, len) == 0 ? "identical" : "DIFFERENT");
	}

	/* --- one offset, not one per machine. */
	{
		off_t a, b, c;

		a = lseek(fd, 0, SEEK_CUR);
		b = lseek(fd, 10, SEEK_SET);
		n = read(fd, big, 5);
		c = lseek(fd, 0, SEEK_CUR);
		check("shared offset", b == 10 && n == 5 && c == 15,
		      "after 43 bytes at %lld, seek 10 gave %lld, read 5 left %lld "
		      "(want 15)", (long long)a, (long long)b, (long long)c);
	}

	/* --- pread/pwrite must not move it. */
	{
		off_t before, after;
		char one = 'Z', got = 0;

		before = lseek(fd, 0, SEEK_CUR);
		n = pwrite(fd, &one, 1, 3);
		if (n == 1)
			n = pread(fd, &got, 1, 3);
		after = lseek(fd, 0, SEEK_CUR);
		check("positioned io", n == 1 && got == 'Z' && before == after,
		      "pwrite/pread at 3 gave '%c', offset %lld -> %lld",
		      got ? got : '?', (long long)before, (long long)after);
	}

	/* --- a length that is not round, from a buffer that spans two pages. */
	{
		char *page = mmap(NULL, 8192, PROT_READ | PROT_WRITE,
				  MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
		char *span;
		char cmp[300];
		const size_t odd = 277;

		if (page == MAP_FAILED) {
			say("fi1: mmap failed\nFAIL\n");
			return 1;
		}
		span = page + 4096 - 100;	/* 100 bytes in one page, 177 in the next */
		fill(span, odd, 0x31);
		n = pwrite(fd, span, odd, 4096);
		memset(cmp, 0, sizeof(cmp));
		if (n == (ssize_t)odd)
			n = pread(fd, cmp, odd, 4096);
		check("page-spanning buffer", n == (ssize_t)odd &&
		      memcmp(cmp, span, odd) == 0,
		      "%zu bytes straddling a page boundary, read %zd, %s",
		      odd, n, memcmp(cmp, span, odd) == 0 ? "identical"
							  : "DIFFERENT");
		munmap(page, 8192);
	}

	/* --- 64 KiB in one call each way. */
	{
		size_t i, bad = 0;

		fill(big, BIG, 0x5a);
		n = pwrite(fd, big, BIG, 65536);
		memset(back, 0, BIG);
		if (n == BIG)
			n = pread(fd, back, BIG, 65536);
		for (i = 0; i < BIG; i++)
			if (back[i] != big[i]) {
				bad++;
				if (bad == 1)
					sayf("fi1 large: first difference at "
					     "byte %zu (0x%02x vs 0x%02x)\n",
					     i, (unsigned char)back[i],
					     (unsigned char)big[i]);
			}
		check("large single call", n == BIG && bad == 0,
		      "%d bytes each way, read %zd, %zu bytes differ",
		      BIG, n, bad);
	}

	/* --- the size the other machine reports. */
	{
		off_t want = 65536 + BIG;
		int r;

		/*
		 * Sequenced, not folded into the check() call. The size and the
		 * call that fills it in are separate arguments there, and C does
		 * not say which is evaluated first -- so the first version read
		 * st_size from before the fstat and reported "fstat says 0" next
		 * to "ok".
		 */
		r = fstat(fd, &st);
		check("stat size", r == 0 && st.st_size == want,
		      "fstat says %lld, want %lld", (long long)st.st_size,
		      (long long)want);
		n = ftruncate(fd, 1000);
		r = fstat(fd, &st);
		check("truncate", n == 0 && r == 0 && st.st_size == 1000,
		      "ftruncate to 1000 gave %zd, fstat says %lld", n,
		      (long long)st.st_size);
	}

	/* --- scattered buffers, each piece its own range. */
	{
		char a[] = "alpha", b[] = "bravo", c[] = "charlie";
		char got[32] = { 0 };
		struct iovec wv[3], rv[3];
		char g1[5], g2[5], g3[7];

		wv[0].iov_base = a; wv[0].iov_len = 5;
		wv[1].iov_base = b; wv[1].iov_len = 5;
		wv[2].iov_base = c; wv[2].iov_len = 7;
		lseek(fd, 0, SEEK_SET);
		n = writev(fd, wv, 3);
		lseek(fd, 0, SEEK_SET);
		rv[0].iov_base = g1; rv[0].iov_len = 5;
		rv[1].iov_base = g2; rv[1].iov_len = 5;
		rv[2].iov_base = g3; rv[2].iov_len = 7;
		if (n == 17)
			n = readv(fd, rv, 3);
		memcpy(got, g1, 5);
		memcpy(got + 5, g2, 5);
		memcpy(got + 10, g3, 7);
		check("readv/writev", n == 17 &&
		      memcmp(got, "alphabravocharlie", 17) == 0,
		      "17 bytes in three pieces, read %zd, got |%.17s|", n, got);
	}

	/* --- O_APPEND, whose offset is decided where the file is. */
	{
		int afd = open(path, O_WRONLY | O_APPEND);
		char tail[8] = { 0 };

		n = ftruncate(fd, 4);
		n = (n == 0 && afd >= 0) ? write(afd, "END!", 4) : -1;
		if (afd >= 0)
			close(afd);
		if (n == 4)
			n = pread(fd, tail, 8, 0);
		check("append", n == 8 && memcmp(tail + 4, "END!", 4) == 0,
		      "after truncate to 4, appending 4 gave a file of %zd, "
		      "tail |%.4s|", n, tail + 4);
	}

	close(fd);
	unlink(path);
	sayf("fi1: %d case(s) bad\n", fails);
	say(fails == 0 ? "PASS\n" : "FAIL\n");
	return fails == 0 ? 0 : 1;
}
