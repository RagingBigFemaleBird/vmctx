/*
 * mr1 — mremap's three ways of vacating memory, each checked for both halves
 * of the contract: the bytes that survive must MOVE, and the range that was
 * vacated must be GONE.
 *
 * This is the regression test for netsurf's exit-242 wall. Its realloc
 * mremap-shrank a CSS arena in place, and the vacated tail stayed mapped on
 * the machine running the program with the arena's old bytes in it — a ghost
 * a later GTK walk read as a function pointer, 8MiB of recursion, dead at
 * ~30s, byte-deterministic. The propagation layer had no case for mremap
 * because it was a switch on syscall numbers; it now has no cases at all —
 * the kernel's mm-mutation hook reports every vacated range, whatever call
 * vacated it — and this program is the direct proof that MMU_NOTIFY_UNMAP
 * covers all three shapes:
 *
 *   shrink in place   the tail beyond the new length is vacated
 *   move              the whole old range is vacated, bytes land at the new
 *   MREMAP_FIXED      the replaced target's OWN old bytes must not survive
 *                     under the moved content, and the source is vacated
 *
 * "Gone" is tested the way pf1 tests it: touch it, take the SIGSEGV, check
 * SEGV_MAPERR, leave by siglongjmp. A ghost mapping answers the touch with
 * stale bytes and no fault at all — hits stays 0, which is exactly the shape
 * the wall had.
 */
#define _GNU_SOURCE
#include <string.h>
#include <stdio.h>
#include <stdarg.h>
#include <signal.h>
#include <setjmp.h>
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

static volatile sig_atomic_t hits;
static void * volatile got_addr;
static volatile int got_code;
static sigjmp_buf out;

static void on_segv(int sig, siginfo_t *si, void *uc)
{
	(void)sig;
	(void)uc;
	hits++;
	got_addr = si->si_addr;
	got_code = si->si_code;
	siglongjmp(out, 1);
}

/* Touch addr expecting SEGV_MAPERR; 1 = it is gone, 0 = ghost or wrong kind */
static int gone(volatile char *addr)
{
	hits = 0;
	if (sigsetjmp(out, 1) == 0) {
		(void)*addr;
		return 0;	/* read succeeded: the mapping is a ghost */
	}
	return hits == 1 && (char *)got_addr == (char *)addr &&
	       got_code == SEGV_MAPERR;
}

/* Every byte of page i of p carries (tag + i); 1 = all as expected */
static int holds(const char *p, int pages, int tag)
{
	int i, j;

	for (i = 0; i < pages; i++)
		for (j = 0; j < PGSZ; j++)
			if (p[i * PGSZ + j] != (char)(tag + i))
				return 0;
	return 1;
}

static void fill(char *p, int pages, int tag)
{
	int i;

	for (i = 0; i < pages; i++)
		memset(p + i * PGSZ, tag + i, PGSZ);
}

int main(void)
{
	struct sigaction sa;
	volatile int fails = 0;

	memset(&sa, 0, sizeof(sa));
	sa.sa_sigaction = on_segv;
	sa.sa_flags = SA_SIGINFO | SA_NODEFER;
	sigemptyset(&sa.sa_mask);
	if (sigaction(SIGSEGV, &sa, NULL) != 0) {
		say("mr1: sigaction failed\nFAIL\n");
		return 1;
	}

	/* --- shrink in place: 8 pages down to 3, the 5-page tail vacated.
	 * The netsurf shape, exactly. */
	{
		char *p = mmap(NULL, 8 * PGSZ, PROT_READ | PROT_WRITE,
			       MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
		char *r;
		int kept, dead;

		if (p == MAP_FAILED) {
			say("mr1: mmap failed\nFAIL\n");
			return 1;
		}
		fill(p, 8, 0x40);
		r = mremap(p, 8 * PGSZ, 3 * PGSZ, 0);
		if (r != p) {
			say("mr1: in-place shrink did not stay put\nFAIL\n");
			return 1;
		}
		kept = holds(p, 3, 0x40);
		dead = gone(p + 3 * PGSZ);
		if (!kept || !dead)
			fails++;
		sayf("mr1 shrink: kept 3 pages %s, vacated tail %s "
		     "(%d fault(s), si_code %d) -> %s\n",
		     kept ? "intact" : "CORRUPT",
		     dead ? "gone" : "STILL READABLE (the 242 ghost)",
		     (int)hits, got_code, (kept && dead) ? "ok" : "BAD");
		munmap(p, 3 * PGSZ);
	}

	/* --- move: a blocker right after the range forces the grow to
	 * relocate; the old range is vacated, the bytes must arrive. */
	{
		char *p = mmap(NULL, 5 * PGSZ, PROT_READ | PROT_WRITE,
			       MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
		char *blk, *r;
		int moved, arrived, dead;

		if (p == MAP_FAILED) {
			say("mr1: mmap failed\nFAIL\n");
			return 1;
		}
		/* the blocker occupies the page after a 4-page range */
		blk = mmap(p + 4 * PGSZ, PGSZ, PROT_NONE,
			   MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, -1, 0);
		if (blk == MAP_FAILED) {
			say("mr1: blocker mmap failed\nFAIL\n");
			return 1;
		}
		fill(p, 4, 0x60);
		r = mremap(p, 4 * PGSZ, 8 * PGSZ, MREMAP_MAYMOVE);
		if (r == MAP_FAILED) {
			say("mr1: mremap move failed\nFAIL\n");
			return 1;
		}
		moved = (r != p);
		arrived = holds(r, 4, 0x60);
		dead = moved ? gone(p) : 1;
		if (!moved || !arrived || !dead)
			fails++;
		sayf("mr1 move: %s, bytes %s, old range %s "
		     "(%d fault(s), si_code %d) -> %s\n",
		     moved ? "relocated" : "DID NOT MOVE (blocker ignored)",
		     arrived ? "arrived" : "LOST",
		     dead ? "gone" : "STILL READABLE",
		     (int)hits, got_code,
		     (moved && arrived && dead) ? "ok" : "BAD");
		munmap(r, 8 * PGSZ);
		munmap(blk, PGSZ);
	}

	/* --- MREMAP_FIXED: land 2 source pages on a live 4-page target.
	 * The landed half shows the source's bytes, the target's own old
	 * bytes survive only in the half not landed on, the source is gone. */
	{
		char *src = mmap(NULL, 2 * PGSZ, PROT_READ | PROT_WRITE,
				 MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
		char *dst = mmap(NULL, 4 * PGSZ, PROT_READ | PROT_WRITE,
				 MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
		char *r;
		int landed, rest, dead;

		if (src == MAP_FAILED || dst == MAP_FAILED) {
			say("mr1: mmap failed\nFAIL\n");
			return 1;
		}
		fill(src, 2, 0x20);
		fill(dst, 4, 0x70);
		r = mremap(src, 2 * PGSZ, 2 * PGSZ,
			   MREMAP_MAYMOVE | MREMAP_FIXED, dst);
		if (r != dst) {
			say("mr1: MREMAP_FIXED did not land on target\nFAIL\n");
			return 1;
		}
		landed = holds(dst, 2, 0x20);
		rest = holds(dst + 2 * PGSZ, 2, 0x70 + 2);
		dead = gone(src);
		if (!landed || !rest || !dead)
			fails++;
		sayf("mr1 fixed: landed half %s, untouched half %s, source %s "
		     "(%d fault(s), si_code %d) -> %s\n",
		     landed ? "shows source bytes" : "WRONG BYTES",
		     rest ? "intact" : "CLOBBERED",
		     dead ? "gone" : "STILL READABLE",
		     (int)hits, got_code,
		     (landed && rest && dead) ? "ok" : "BAD");
		munmap(dst, 4 * PGSZ);
	}

	say(fails == 0 ? "PASS\n" : "FAIL\n");
	return fails == 0 ? 0 : 1;
}
