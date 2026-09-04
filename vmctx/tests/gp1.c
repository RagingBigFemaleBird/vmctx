/*
 * gp1 — a program that handles its own general-protection faults.
 *
 * A #GP is not a page fault and does not go anywhere near the memory
 * machinery: there is no address to fetch, nothing to lend, and no monitor
 * decision to make. It is the guest asking the processor to do something it may
 * not, and on Linux the answer is SIGSEGV delivered to the program, which may
 * catch it. A guest under vmctx must get the same answer.
 *
 * Two ways to raise one from user code, deliberately, without corrupting
 * anything:
 *
 *   non-canonical   a load through an address with bits 63:48 neither all
 *                   zero nor all one. The processor raises #GP rather than
 *                   #PF, precisely because the address cannot be translated,
 *                   and Linux reports it as SIGSEGV with si_code SI_KERNEL and
 *                   si_addr 0 -- not the address, which is the tell that this
 *                   was a #GP and not a page fault.
 *   privileged      HLT at CPL 3. It touches no memory at all, so a run that
 *                   survives the first case and dies on this one says the
 *                   fault path, not the address, is what is unhandled.
 *
 * Neither is repairable, so the handler leaves by siglongjmp and the program
 * carries on. What is being checked is exactly that: the signal arrives, it
 * carries the right description, and the program is still running afterwards.
 *
 * Each case prints before the next begins, with write(2) rather than stdio, so
 * a case that ends the context still leaves a record of how far it got.
 */
#define _GNU_SOURCE
#include <string.h>
#include <stdio.h>
#include <stdarg.h>
#include <signal.h>
#include <setjmp.h>
#include <unistd.h>

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

	va_start(ap, fmt);
	n = vsnprintf(b, sizeof(b), fmt, ap);
	va_end(ap);
	if (n > 0)
		say(b);
}

static volatile sig_atomic_t hits;
static volatile int got_sig, got_code;
static void * volatile got_addr;
static sigjmp_buf out;

static void on_fault(int sig, siginfo_t *si, void *uc)
{
	(void)uc;
	hits++;
	got_sig  = sig;
	got_code = si->si_code;
	got_addr = si->si_addr;
	siglongjmp(out, 1);
}

int main(void)
{
	struct sigaction sa;
	volatile int fails = 0;		/* survives siglongjmp */
	volatile int left;

	memset(&sa, 0, sizeof(sa));
	sa.sa_sigaction = on_fault;
	sa.sa_flags = SA_SIGINFO | SA_NODEFER;
	sigemptyset(&sa.sa_mask);
	if (sigaction(SIGSEGV, &sa, NULL) != 0 ||
	    sigaction(SIGBUS, &sa, NULL) != 0 ||
	    sigaction(SIGILL, &sa, NULL) != 0) {
		say("gp1: sigaction failed\nFAIL\n");
		return 1;
	}

	/* --- a load through a non-canonical address: #GP, not #PF. */
	{
		volatile unsigned long *bad =
			(volatile unsigned long *)0xdead000000000000UL;
		unsigned long v = 0;

		/*
		 * Announced before it happens: a #GP ends the context rather
		 * than raising a signal, and then the only record of what the
		 * program was attempting is what it already said.
		 */
		say("gp1 non-canonical: about to load through 0xdead000000000000\n");
		hits = 0;
		got_sig = got_code = 0;
		got_addr = (void *)1;
		left = 0;
		if (sigsetjmp(out, 1) == 0)
			v = *bad;
		else
			left = 1;
		(void)v;
		if (!left || hits != 1 || got_sig != SIGSEGV || got_addr != NULL)
			fails++;
		sayf("gp1 non-canonical: %d fault(s), signal %d (want %d), "
		     "si_code %d, si_addr %p (want (nil)), left %d -> %s\n",
		     (int)hits, got_sig, SIGSEGV, got_code, got_addr, left,
		     (left && hits == 1 && got_sig == SIGSEGV &&
		      got_addr == NULL) ? "ok" : "BAD");
	}

	/* --- a privileged instruction at CPL 3: #GP with no address at all. */
	{
		say("gp1 privileged-hlt: about to execute hlt\n");
		hits = 0;
		got_sig = got_code = 0;
		left = 0;
		if (sigsetjmp(out, 1) == 0)
			__asm__ __volatile__("hlt");
		else
			left = 1;
		if (!left || hits != 1 || got_sig != SIGSEGV)
			fails++;
		sayf("gp1 privileged-hlt: %d fault(s), signal %d (want %d), "
		     "si_code %d, left %d -> %s\n", (int)hits, got_sig,
		     SIGSEGV, got_code, left,
		     (left && hits == 1 && got_sig == SIGSEGV) ? "ok" : "BAD");
	}

	say(fails == 0 ? "PASS\n" : "FAIL\n");
	return fails == 0 ? 0 : 1;
}
