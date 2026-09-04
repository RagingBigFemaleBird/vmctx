/*
 * md1 — MADV_DONTNEED must actually discard.
 *
 * For a private anonymous mapping, madvise(MADV_DONTNEED) is a contract with
 * content: the next read of the range observes fresh zero-fill pages. glibc's
 * allocator leans on exactly this when it trims arenas — memory handed back
 * by DONTNEED is treated as zeroed when it is next carved up (calloc's
 * already-zero fast path), so a stale byte surviving a DONTNEED is not a
 * performance bug, it is a wrong answer that surfaces as heap corruption far
 * from here.
 *
 * Under vmctx the call is forwarded and the source's kernel zaps its own
 * pages — but the machine running the program still holds its copies, and
 * nothing in a syscall-number switch ever propagated the discard (the same
 * structural hole mremap sat in for the project's whole life, tests/mr1).
 * The mm-mutation hook's MMU_NOTIFY_UNMAP arm does not see DONTNEED either:
 * that path raises MMU_NOTIFY_CLEAR, which the hook must ignore (CLEAR also
 * fires for the coherence layer's own COW write-protects, where "forget this
 * range" would be wrong). So the discard needs its own kernel-side note from
 * the madvise path — and this test is both the photograph of the gap and the
 * regression fence for the fix.
 *
 * Three checks:
 *   zeros     fill, DONTNEED, read: every byte must be 0
 *   partial   DONTNEED only the middle; the flanks must keep their bytes
 *   reuse     write after DONTNEED, read it back: the range must be usable
 */
#define _GNU_SOURCE
#include <string.h>
#include <stdio.h>
#include <stdarg.h>
#include <unistd.h>
#include <sys/mman.h>

#define PGSZ 4096

static void say(const char *s)
{
	if (write(1, s, strlen(s)) < 0)
		_exit(1);
}

static void sayf(const char *fmt, ...)
{
	char b[256];
	va_list ap;
	int n;

	__builtin_va_start(ap, fmt);
	n = vsnprintf(b, sizeof(b), fmt, ap);
	__builtin_va_end(ap);
	if (n > 0)
		say(b);
}

/* First offset in [p, p+len) whose byte is not want; -1 if none. */
static long scan(const unsigned char *p, long len, unsigned char want)
{
	long i;

	for (i = 0; i < len; i++)
		if (p[i] != want)
			return i;
	return -1;
}

int main(void)
{
	int fails = 0;

	/* --- zeros: the whole point. */
	{
		unsigned char *p = mmap(NULL, 4 * PGSZ, PROT_READ | PROT_WRITE,
					MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
		long bad;

		if (p == MAP_FAILED) {
			say("md1: mmap failed\nFAIL\n");
			return 1;
		}
		memset(p, 0xaa, 4 * PGSZ);
		if (madvise(p, 4 * PGSZ, MADV_DONTNEED) != 0) {
			say("md1: madvise failed\nFAIL\n");
			return 1;
		}
		bad = scan(p, 4 * PGSZ, 0);
		if (bad >= 0)
			fails++;
		sayf("md1 zeros: %s%s\n",
		     bad < 0 ? "all 4 pages read 0 -> ok" : "STALE byte ",
		     bad < 0 ? "" : "(the discard never propagated) -> BAD");
		if (bad >= 0)
			sayf("md1 zeros: first stale at +0x%lx = 0x%02x "
			     "(want 0)\n", bad, p[bad]);
		munmap(p, 4 * PGSZ);
	}

	/* --- partial: only what was named is discarded. */
	{
		unsigned char *p = mmap(NULL, 3 * PGSZ, PROT_READ | PROT_WRITE,
					MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
		long b0, b1, b2;

		if (p == MAP_FAILED) {
			say("md1: mmap failed\nFAIL\n");
			return 1;
		}
		memset(p, 0x11, PGSZ);
		memset(p + PGSZ, 0x22, PGSZ);
		memset(p + 2 * PGSZ, 0x33, PGSZ);
		madvise(p + PGSZ, PGSZ, MADV_DONTNEED);
		b0 = scan(p, PGSZ, 0x11);
		b1 = scan(p + PGSZ, PGSZ, 0);
		b2 = scan(p + 2 * PGSZ, PGSZ, 0x33);
		if (b0 >= 0 || b1 >= 0 || b2 >= 0)
			fails++;
		sayf("md1 partial: flank %s, middle %s, flank %s -> %s\n",
		     b0 < 0 ? "kept" : "LOST",
		     b1 < 0 ? "zeroed" : "STALE",
		     b2 < 0 ? "kept" : "LOST",
		     (b0 < 0 && b1 < 0 && b2 < 0) ? "ok" : "BAD");
		munmap(p, 3 * PGSZ);
	}

	/* --- reuse: the discarded range takes fresh stores. */
	{
		unsigned char *p = mmap(NULL, 2 * PGSZ, PROT_READ | PROT_WRITE,
					MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
		long bad;

		if (p == MAP_FAILED) {
			say("md1: mmap failed\nFAIL\n");
			return 1;
		}
		memset(p, 0x55, 2 * PGSZ);
		madvise(p, 2 * PGSZ, MADV_DONTNEED);
		memset(p, 0x66, 2 * PGSZ);
		bad = scan(p, 2 * PGSZ, 0x66);
		if (bad >= 0)
			fails++;
		sayf("md1 reuse: %s -> %s\n",
		     bad < 0 ? "fresh stores read back" : "stores LOST",
		     bad < 0 ? "ok" : "BAD");
		munmap(p, 2 * PGSZ);
	}

	say(fails == 0 ? "PASS\n" : "FAIL\n");
	return fails == 0 ? 0 : 1;
}
