/*
 * tr1 — the other faults a program raises on itself, and one that is dangerous
 * here for a reason particular to this design.
 *
 *   #UD   ud2 -> SIGILL.
 *   #BP   int3 -> SIGTRAP. This one the handler *returns* from: the trap is
 *         reported after the instruction, so returning simply carries on. It
 *         is the cheapest check that a signal frame built over guest state
 *         resumes the guest correctly.
 *   #DE   integer divide by zero -> SIGFPE, si_code FPE_INTDIV. The handler
 *         cannot return (the instruction would divide by zero again), so it
 *         leaves by siglongjmp.
 *
 * #UD goes first deliberately. It is the case particular to this design, and
 * the two after it are vectors the backend does not intercept at all -- so
 * under vmctx they end the context, and a #UD case ordered after them would
 * never run.
 *
 * The last one is not routine. A guest's SYSCALL instruction is trapped as #UD
 * -- that is how a syscall leaves the guest at all -- and the backend reads the
 * exit reason, takes RAX as the syscall number, and steps RIP over two bytes.
 * ud2 is also two bytes and also raises #UD. So unless something distinguishes
 * them by their opcode, a program executing ud2 does not get SIGILL: it runs
 * whatever syscall RAX happened to hold and carries on past the instruction.
 *
 * That is why RAX is loaded with 0x7fff first. If the trap is mistaken for a
 * syscall, the syscall attempted is a number no kernel implements, so the only
 * consequence is ENOSYS -- rather than some arbitrary call made with whatever
 * registers were live. The test then reports exactly what happened: SIGILL, or
 * execution continuing with RAX holding an errno.
 *
 * Each case prints before the next begins, with write(2), so a case that ends
 * the context still leaves a record of how far it got.
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
static sigjmp_buf out;
static volatile int leave_by_longjmp;

static void on_trap(int sig, siginfo_t *si, void *uc)
{
	(void)uc;
	hits++;
	got_sig  = sig;
	got_code = si->si_code;
	if (leave_by_longjmp)
		siglongjmp(out, 1);
	/* #BP is reported after the instruction: returning just carries on. */
}

int main(void)
{
	struct sigaction sa;
	volatile int fails = 0;		/* survives siglongjmp */
	volatile int left;

	memset(&sa, 0, sizeof(sa));
	sa.sa_sigaction = on_trap;
	sa.sa_flags = SA_SIGINFO | SA_NODEFER;
	sigemptyset(&sa.sa_mask);
	if (sigaction(SIGFPE, &sa, NULL) != 0 ||
	    sigaction(SIGTRAP, &sa, NULL) != 0 ||
	    sigaction(SIGILL, &sa, NULL) != 0 ||
	    sigaction(SIGSEGV, &sa, NULL) != 0) {
		say("tr1: sigaction failed\nFAIL\n");
		return 1;
	}

	/*
	 * --- ud2: SIGILL, and it must not be mistaken for a syscall.
	 *
	 * RAX is set to a number no kernel implements before the trap, so that
	 * if it *is* mistaken for one the damage is an ENOSYS and not an
	 * arbitrary call. Reaching the line after the ud2 is the failure.
	 */
	{
		volatile unsigned long rax_after = 0;
		volatile int continued = 0;

		/*
		 * Announced before it happens. Any of these can end the context
		 * rather than raise a signal, and when it does the only record
		 * of what the program was attempting is what it already said.
		 */
		say("tr1 ud2: about to execute ud2\n");
		hits = 0;
		got_sig = got_code = 0;
		leave_by_longjmp = 1;
		left = 0;
		if (sigsetjmp(out, 1) == 0) {
			__asm__ __volatile__("movq $0x7fff, %%rax\n\t"
					     "ud2\n\t"
					     "movq %%rax, %0"
					     : "=r"(rax_after)
					     :
					     : "rax", "memory");
			continued = 1;
		} else {
			left = 1;
		}
		if (!left || continued || hits != 1 || got_sig != SIGILL)
			fails++;
		sayf("tr1 ud2: %d fault(s), signal %d (want %d), si_code %d, "
		     "left %d, ran past it %d (rax 0x%lx) -> %s\n", (int)hits,
		     got_sig, SIGILL, got_code, left, continued, rax_after,
		     (left && !continued && hits == 1 && got_sig == SIGILL)
		     ? "ok" : "BAD");
		if (continued)
			say("tr1: the ud2 was taken for a syscall -- a guest "
			    "cannot raise #UD on itself here\n");
	}

	/* --- int3: SIGTRAP, and the handler returns straight through it. */
	{
		int after = 0;

		say("tr1 int3: about to execute int3\n");
		hits = 0;
		got_sig = got_code = 0;
		leave_by_longjmp = 0;
		__asm__ __volatile__("int3");
		after = 1;
		if (!after || hits != 1 || got_sig != SIGTRAP)
			fails++;
		sayf("tr1 int3: %d trap(s), signal %d (want %d), si_code %d, "
		     "returned through it %d -> %s\n", (int)hits, got_sig,
		     SIGTRAP, got_code, after,
		     (after && hits == 1 && got_sig == SIGTRAP) ? "ok" : "BAD");
	}

	/* --- divide by zero: SIGFPE, and the handler has to leave. */
	{
		volatile int zero = 0;
		int d = zero;

		/*
		 * The division is written out rather than left to the compiler.
		 * Dividing by zero is undefined, and gcc is entitled to delete
		 * the expression instead of emitting the instruction -- which
		 * it does at -O2, so the plain C version raised nothing at all
		 * and the test passed by not testing.
		 */
		say("tr1 divide-by-zero: about to divide by zero\n");
		hits = 0;
		got_sig = got_code = 0;
		leave_by_longjmp = 1;
		left = 0;
		if (sigsetjmp(out, 1) == 0)
			__asm__ __volatile__("xorl %%edx, %%edx\n\t"
					     "movl $1, %%eax\n\t"
					     "divl %0"
					     : : "r"(d)
					     : "eax", "edx", "cc");
		else
			left = 1;
		if (!left || hits != 1 || got_sig != SIGFPE ||
		    got_code != FPE_INTDIV)
			fails++;
		sayf("tr1 divide-by-zero: %d fault(s), signal %d (want %d), "
		     "si_code %d (want %d), left %d -> %s\n", (int)hits,
		     got_sig, SIGFPE, got_code, FPE_INTDIV, left,
		     (left && hits == 1 && got_sig == SIGFPE &&
		      got_code == FPE_INTDIV) ? "ok" : "BAD");
	}

	say(fails == 0 ? "PASS\n" : "FAIL\n");
	return fails == 0 ? 0 : 1;
}
