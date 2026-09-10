/*
 * pf1 — a program that handles its own page faults.
 *
 * Nothing in the tree exercised this. Every fault a guest takes so far is one
 * the machinery is supposed to *hide* from it: a page that lives on the other
 * machine, fetched and installed, with the guest never told. This is the
 * opposite case — a fault the program raised deliberately and intends to
 * service itself, with a handler it registered, and the right answer is that it
 * gets the signal and carries on.
 *
 * Three kinds, because they take different paths:
 *
 *   not present    a PROT_NONE page read. The handler mprotects it and
 *                  returns, so the faulting instruction is retried and this
 *                  time succeeds. Recovery by *returning* is the case that
 *                  matters: it needs the whole signal frame to be right, since
 *                  the guest resumes on the instruction that faulted.
 *   not writable   a write to a PROT_READ page. Same recovery, but the fault
 *                  arrives with the page present, which is a different code
 *                  path on both machines.
 *   nowhere at all an address with no mapping under it. Nothing can fix that,
 *                  so the handler leaves by siglongjmp, and what is checked is
 *                  that the program is still running afterwards.
 *
 * Each case prints its own line with write(2) before the next one starts.
 * stdio would buffer them, and a case that kills the context takes the buffer
 * with it -- so the run would say nothing about how far it got, which is the
 * one thing worth knowing.
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
static void * volatile got_addr;	/* the pointer is volatile, not the target */
static volatile int got_code;
static sigjmp_buf out;
static volatile int leave_by_longjmp;
static char *fixme;		/* the page the handler is allowed to repair */

static void on_segv(int sig, siginfo_t *si, void *uc)
{
	(void)sig;
	(void)uc;
	hits++;
	got_addr = si->si_addr;
	got_code = si->si_code;
	if (leave_by_longjmp)
		siglongjmp(out, 1);
	/*
	 * Repair it and return. Returning from a SIGSEGV handler without having
	 * made the access possible re-runs the instruction and faults again,
	 * for ever -- so this must be the only path that returns.
	 */
	mprotect(fixme, PGSZ, PROT_READ | PROT_WRITE);
}

int main(void)
{
	struct sigaction sa;
	volatile int fails = 0;	/* survives siglongjmp */

	memset(&sa, 0, sizeof(sa));
	sa.sa_sigaction = on_segv;
	sa.sa_flags = SA_SIGINFO | SA_NODEFER;
	sigemptyset(&sa.sa_mask);
	if (sigaction(SIGSEGV, &sa, NULL) != 0 ||
	    sigaction(SIGBUS, &sa, NULL) != 0) {
		say("pf1: sigaction failed\nFAIL\n");
		return 1;
	}

	/* --- not present: read a PROT_NONE page, handler makes it readable. */
	{
		char *p = mmap(NULL, PGSZ, PROT_NONE,
			       MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
		volatile char v;

		if (p == MAP_FAILED) {
			say("pf1: mmap failed\nFAIL\n");
			return 1;
		}
		fixme = p;
		hits = 0;
		leave_by_longjmp = 0;
		v = *(volatile char *)p;	/* faults, handler repairs it */
		if (hits != 1 || v != 0 ||
		    (char *)got_addr != p || got_code != SEGV_ACCERR)
			fails++;
		sayf("pf1 not-present: %d fault(s), si_addr %s, si_code %d "
		     "(want 1, matching, %d), value %d -> %s\n",
		     (int)hits, ((char *)got_addr == p) ? "matches" : "WRONG",
		     got_code, SEGV_ACCERR, (int)v,
		     (hits == 1 && v == 0 && (char *)got_addr == p &&
		      got_code == SEGV_ACCERR) ? "ok" : "BAD");
		munmap(p, PGSZ);
	}

	/* --- not writable: store into a PROT_READ page, handler opens it. */
	{
		char *p = mmap(NULL, PGSZ, PROT_READ | PROT_WRITE,
			       MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);

		if (p == MAP_FAILED) {
			say("pf1: mmap failed\nFAIL\n");
			return 1;
		}
		memset(p, 0, PGSZ);
		mprotect(p, PGSZ, PROT_READ);
		fixme = p;
		hits = 0;
		leave_by_longjmp = 0;
		*(volatile char *)p = 0x5a;	/* faults, handler repairs it */
		if (hits != 1 || *(volatile char *)p != 0x5a ||
		    (char *)got_addr != p || got_code != SEGV_ACCERR)
			fails++;
		sayf("pf1 not-writable: %d fault(s), si_addr %s, si_code %d, "
		     "byte 0x%02x -> %s\n", (int)hits,
		     ((char *)got_addr == p) ? "matches" : "WRONG", got_code,
		     *(volatile unsigned char *)p,
		     (hits == 1 && *(volatile char *)p == 0x5a &&
		      (char *)got_addr == p && got_code == SEGV_ACCERR)
		     ? "ok" : "BAD");
		munmap(p, PGSZ);
	}

	/* --- nowhere at all: no mapping, so the handler has to leave. */
	{
		char *p = mmap(NULL, PGSZ, PROT_READ | PROT_WRITE,
			       MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
		int left = 0;

		if (p == MAP_FAILED) {
			say("pf1: mmap failed\nFAIL\n");
			return 1;
		}
		munmap(p, PGSZ);	/* now certainly nobody's */
		hits = 0;
		leave_by_longjmp = 1;
		if (sigsetjmp(out, 1) == 0)
			*(volatile char *)p = 1;
		else
			left = 1;
		if (!left || hits != 1 || (char *)got_addr != p ||
		    got_code != SEGV_MAPERR)
			fails++;
		sayf("pf1 unmapped: %d fault(s), si_addr %s, si_code %d "
		     "(want %d), left by longjmp %d -> %s\n", (int)hits,
		     ((char *)got_addr == p) ? "matches" : "WRONG", got_code,
		     SEGV_MAPERR, left,
		     (left && hits == 1 && (char *)got_addr == p &&
		      got_code == SEGV_MAPERR) ? "ok" : "BAD");
	}

	say(fails == 0 ? "PASS\n" : "FAIL\n");
	return fails == 0 ? 0 : 1;
}
