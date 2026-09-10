/*
 * pf3 — a fault the program has NOT asked to handle must kill it.
 *
 * Every fault test in this tree installs a handler first. pf1, pf2, gp1 and
 * tr1 all do, and they are right to: recovering by returning from a handler is
 * the hard case. But it means the ordinary case has never been tested at all —
 * a program that dereferences a wild pointer with the default disposition in
 * force, which is what almost every real fault is.
 *
 * That case is broken, and the shape of the break is why no existing test sees
 * it. The source decides what a vector means by asking its own process what
 * the disposition is (sh_deliver_exception, vmhome.c), and that process has
 * installed handlers of its own for exactly these signals so it can log its
 * death (sh_watch_own_death, vmhome.c). A guest that registers a handler
 * overwrites those, so the answer is right and pf1 passes. A guest that
 * registers nothing is told the program *does* have a handler — vmhome's —
 * and is resumed at that address, which is in vmhome's own text and is not
 * part of the guest's address space at all.
 *
 * So the failure is not "the signal was wrong". It is that the program does
 * not die, and instead jumps somewhere meaningless.
 *
 * Checked from the parent, because a correct result is the child's death:
 * fork, let the child fault with the default disposition, and read the wait
 * status. WIFSIGNALED with the right signal is the pass.
 *
 * Three cases, one per vector that reaches a different arm:
 *
 *   SIGSEGV   a write through a null pointer          (#PF, vector 14)
 *   SIGILL    ud2                                     (#UD, vector 6)
 *   SIGFPE    integer divide by zero                  (#DE, vector 0)
 *
 * Each is a separate child, because the first one that works ends it.
 *
 * write(2) rather than stdio for the same reason pf1 gives: a case that takes
 * the context with it must not take the buffer too.
 */
#define _GNU_SOURCE
#include <string.h>
#include <stdio.h>
#include <stdarg.h>
#include <errno.h>
#include <signal.h>
#include <unistd.h>
#include <sys/wait.h>

static int fails;

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

/*
 * Run one faulting child and say whether it died the way it should have.
 *
 * The child must not return from here under any circumstance: if the fault
 * does not happen, _exit(9) says so, and that is a different failure from
 * "died by the wrong signal" and worth telling apart.
 */
static void one(const char *name, int want_sig, void (*fault)(void))
{
	pid_t kid, got;
	int st = 0;

	kid = fork();
	if (kid < 0) {
		sayf("%-8s FAIL  fork: %s\n", name, strerror(errno));
		fails++;
		return;
	}
	if (kid == 0) {
		fault();
		/*
		 * Still here, so the access the machinery was supposed to
		 * refuse was served instead. That is the interesting failure:
		 * it means a page was supplied for an address the program has
		 * no right to.
		 */
		_exit(9);
	}

	got = waitpid(kid, &st, 0);
	if (got != kid) {
		sayf("%-8s FAIL  waitpid: %s\n", name, strerror(errno));
		fails++;
		return;
	}
	if (WIFSIGNALED(st) && WTERMSIG(st) == want_sig) {
		sayf("%-8s ok    died by signal %d, as it should\n",
		     name, want_sig);
		return;
	}
	fails++;
	if (WIFSIGNALED(st))
		sayf("%-8s FAIL  died by signal %d, wanted %d\n",
		     name, WTERMSIG(st), want_sig);
	else if (WIFEXITED(st) && WEXITSTATUS(st) == 9)
		sayf("%-8s FAIL  the fault never happened; the access was "
		     "served\n", name);
	else if (WIFEXITED(st))
		sayf("%-8s FAIL  exited %d rather than dying\n",
		     name, WEXITSTATUS(st));
	else
		sayf("%-8s FAIL  wait status 0x%x\n", name, st);
}

/*
 * volatile, and through a variable the compiler cannot fold: a null store it
 * can prove is undefined may be compiled to ud2, which would make the SIGSEGV
 * case silently test the SIGILL one.
 */
static volatile char *null_ptr;
static volatile int zero;

static void fault_segv(void)
{
	*null_ptr = 1;
}

static void fault_ill(void)
{
	__asm__ __volatile__("ud2");
}

/*
 * In assembly, because C will not reliably produce this. Written as
 * `volatile int r = 1 / d;` with d volatile and zero, gcc -O2 still folds the
 * division away and the child exits normally -- which the native control
 * caught on the first run, reporting "the fault never happened" for a case
 * that had nothing to do with vmctx. idiv with a zero divisor is #DE and
 * nothing is entitled to remove it.
 */
static void fault_fpe(void)
{
	int d = zero;

	__asm__ __volatile__("xorl %%edx, %%edx\n\t"
			     "movl $1, %%eax\n\t"
			     "idivl %0"
			     :
			     : "r"(d)
			     : "eax", "edx", "cc");
}

int main(void)
{
	null_ptr = 0;
	zero = 0;

	one("segv", SIGSEGV, fault_segv);
	one("ill", SIGILL, fault_ill);
	one("fpe", SIGFPE, fault_fpe);

	say(fails == 0 ? "PASS\n" : "FAIL\n");
	return fails == 0 ? 0 : 1;
}
