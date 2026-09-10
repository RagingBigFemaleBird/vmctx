/*
 * pf4 — the protections the program was *loaded* with.
 *
 * pf1 and pf2 test protections the guest asked for itself, with mprotect, after
 * it was running. Those work, because an mprotect is a syscall: it is executed
 * on the machine that owns the program and its effect is carried to the machine
 * running it. Nothing tests the other source of protection, which is most of
 * them: what the ELF said, applied by the loader before the program's first
 * instruction.
 *
 * That set was not carried at all. Both machines conjured the program's address
 * space as it was touched, and both conjured it read-write-execute:
 *
 *   the destination  vmctx_back_fault() in the module mapped the faulting page
 *                    PROT_READ|PROT_WRITE|PROT_EXEC before anyone was asked.
 *   the source       the layout parse pulled the "r-xp" out of an oracle's
 *                    /proc/maps into a `perms` buffer and never read it, then
 *                    mapped the range PROT_READ|WRITE|EXEC anyway.
 *
 * So the side that was asked "may the program do this?" answered from
 * permissions it had overwritten itself, and said yes to everything. A write
 * into a function's own code succeeded and was seen by nobody.
 *
 * Written when that was so, and kept because the property is what matters, not
 * the mechanism: the source side named here (an oracle, a mirror, a
 * may-access question asked over the wire) has since been removed entirely.
 * The destination now carries a range's protection back beside the page, so
 * the first touch is no longer read-write-execute and a write to read-only
 * text arrives as a protection fault -- which is the path that does ask the
 * owner.
 *
 * That matters beyond tidiness. glibc puts a PROT_NONE guard page below every
 * thread stack; if it is writable, a stack overflow scribbles into whatever is
 * below it instead of dying. And a program that can write its own text is one
 * whose text can be rewritten by any wild pointer.
 *
 * Four cases, each an access the loader's protections forbid:
 *
 *   text-write   store into the bytes of a function in this program
 *   rodata-write store into a const object the linker put in .rodata
 *   stack-exec   call into a buffer on the stack (NX)
 *   text-verify  after the text-write case, the function must still behave --
 *                which catches the failure where the write *was* allowed and
 *                actually corrupted the program, as distinct from being
 *                silently dropped
 *
 * A handler with siglongjmp, because a pass here means the access faulted and
 * the program has to survive to report it.
 */
#define _GNU_SOURCE
#include <string.h>
#include <stdio.h>
#include <stdarg.h>
#include <errno.h>
#include <signal.h>
#include <setjmp.h>
#include <unistd.h>
#include <sys/mman.h>

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

static void check(const char *name, int ok, const char *fmt, ...)
{
	char b[256];
	va_list ap;

	va_start(ap, fmt);
	vsnprintf(b, sizeof(b), fmt, ap);
	va_end(ap);
	if (ok) {
		sayf("%-13s ok    %s\n", name, b);
		return;
	}
	fails++;
	sayf("%-13s FAIL  %s\n", name, b);
}

static sigjmp_buf back;
static volatile sig_atomic_t hits;
static volatile sig_atomic_t got_sig;

static void on_fault(int sig, siginfo_t *si, void *uc)
{
	(void)si;
	(void)uc;
	hits++;
	got_sig = sig;
	siglongjmp(back, 1);
}

static void arm(void)
{
	struct sigaction sa;

	memset(&sa, 0, sizeof(sa));
	sa.sa_sigaction = on_fault;
	/*
	 * SA_NODEFER because these cases run one after another and each
	 * longjmps out of the handler, so the signal is never unblocked by a
	 * normal return from it.
	 */
	sa.sa_flags = SA_SIGINFO | SA_NODEFER;
	sigemptyset(&sa.sa_mask);
	sigaction(SIGSEGV, &sa, NULL);
	sigaction(SIGBUS, &sa, NULL);
}

/*
 * The function whose bytes the text-write case tries to change, and whose
 * answer the text-verify case checks afterwards.
 *
 * External linkage, `noipa` and `used` together, and all three are needed: as a
 * plain `static noinline` at -O2 gcc still folds canary_fn(1) to 18, drops the
 * body, and leaves the address reference in case_text_write dangling -- the
 * link fails with "undefined reference to canary_fn", which is what happened
 * the first time this was written. There must be real bytes in .text to try to
 * write to.
 */
__attribute__((noipa, used)) int canary_fn(int x);
__attribute__((noipa, used)) int canary_fn(int x)
{
	return x + 17;
}

/* In .rodata: const, initialised, and read through a volatile pointer so the
 * compiler cannot answer from the initialiser. */
static const unsigned long rodata_word = 0x5a5a5a5a5a5a5a5aUL;

static void case_text_write(void)
{
	volatile unsigned char *code = (volatile unsigned char *)(void *)canary_fn;

	hits = 0;
	if (sigsetjmp(back, 1) == 0) {
		*code = 0xc3;		/* ret -- would make canary_fn a no-op */
		check("text-write", 0,
		      "wrote into this program's own .text and nothing faulted");
		return;
	}
	check("text-write", hits == 1, "faulted with signal %d", (int)got_sig);
}

static void case_rodata_write(void)
{
	volatile unsigned long *p = (volatile unsigned long *)&rodata_word;

	hits = 0;
	if (sigsetjmp(back, 1) == 0) {
		*p = 0;
		check("rodata-write", 0,
		      "wrote into .rodata and nothing faulted");
		return;
	}
	check("rodata-write", hits == 1, "faulted with signal %d", (int)got_sig);
}

static void case_stack_exec(void)
{
	/*
	 * `ret`, on the stack. Calling it is harmless if the page is executable
	 * (which is the failure) and faults if it is not (which is the pass).
	 * volatile so the store is really made before the call.
	 */
	volatile unsigned char buf[16];
	int (*fn)(void);
	size_t i;

	for (i = 0; i < sizeof(buf); i++)
		buf[i] = 0xc3;

	hits = 0;
	fn = (int (*)(void))(void *)buf;
	if (sigsetjmp(back, 1) == 0) {
		fn();
		check("stack-exec", 0,
		      "executed code on the stack and nothing faulted");
		return;
	}
	check("stack-exec", hits == 1, "faulted with signal %d", (int)got_sig);
}

/*
 * Did the text-write case actually corrupt the program?
 *
 * Three outcomes are possible and they are different bugs. The write faulted
 * (correct). The write was dropped somewhere and the text is intact (wrong, but
 * harmless today). The write landed (wrong, and the program is now different
 * from the one that was loaded) -- and only this case can tell the third from
 * the second.
 */
static void case_text_verify(void)
{
	int r = canary_fn(1);

	check("text-verify", r == 18,
	      "canary_fn(1) = %d (want 18; 1 means its first byte became `ret`)",
	      r);
}

int main(void)
{
	arm();
	case_text_write();
	case_rodata_write();
	case_stack_exec();
	case_text_verify();

	say(fails == 0 ? "PASS\n" : "FAIL\n");
	return fails == 0 ? 0 : 1;
}
