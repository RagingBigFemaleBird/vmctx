/*
 * pf2 — more faults a program raises and handles itself, past the three in pf1.
 *
 * pf1 asks whether a fault reaches the handler at all. These are the shapes a
 * real program actually uses, each of which needs something more than that:
 *
 *   guard page        a PROT_NONE page below a region, touched by running off
 *                     the end of the region above it. This is what glibc puts
 *                     below every thread stack, so "protections do not reach
 *                     the guest" and "a stack overflow is silent" are the same
 *                     sentence.
 *   JIT               map anonymous memory writable, write code into it,
 *                     mprotect it to read+execute, and call it. Then mprotect
 *                     it back to writable and change it. A W^X page that never
 *                     became executable, or never stopped being writable, is
 *                     equally wrong and only the second is visible from inside.
 *   grow on demand    a large PROT_NONE reservation with pages made accessible
 *                     one at a time by the handler, which is how a heap that
 *                     must not move is built. Faults must arrive for each new
 *                     page and not for one already opened.
 *   si_addr precision the faulting address must be the byte touched, not the
 *                     page. A handler that grows a region by page uses it.
 *   nested            a fault taken *inside* the handler, on the alternate
 *                     stack, which is the arrangement a stack-overflow handler
 *                     is obliged to use.
 *   execute           a page mapped without PROT_EXEC, jumped to. The error
 *                     code says fetch rather than read, and si_addr is the
 *                     instruction, not the data.
 *
 * Every case prints before it starts and again with its result, because any of
 * them may end the context rather than deliver a signal.
 */
#define _GNU_SOURCE
#include <string.h>
#include <stdio.h>
#include <stdarg.h>
#include <stdlib.h>
#include <signal.h>
#include <setjmp.h>
#include <unistd.h>
#include <sys/mman.h>

#define PGSZ 4096

static int fails;

static void say(const char *s)
{
	if (write(1, s, strlen(s)) < 0)
		_exit(1);
}

static void sayf(const char *fmt, ...)
{
	char b[384];
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
	char b[288];
	va_list ap;

	va_start(ap, fmt);
	vsnprintf(b, sizeof(b), fmt, ap);
	va_end(ap);
	if (!ok)
		fails++;
	sayf("pf2 %s: %s -> %s\n", what, b, ok ? "ok" : "BAD");
}

static volatile sig_atomic_t hits;
static void * volatile got_addr;
static volatile int got_code;
static sigjmp_buf out;
static volatile int mode;	/* 0 = longjmp, 1 = open the faulting page */
static volatile int nested_depth;

#define MODE_LEAVE  0
#define MODE_OPEN   1
#define MODE_NEST   2

static void on_segv(int sig, siginfo_t *si, void *uc)
{
	(void)sig;
	(void)uc;
	hits++;
	got_addr = si->si_addr;
	got_code = si->si_code;

	if (mode == MODE_OPEN) {
		/* Open exactly the page that faulted, and return into it. */
		unsigned long pg = (unsigned long)si->si_addr & ~(unsigned long)(PGSZ - 1);

		mprotect((void *)pg, PGSZ, PROT_READ | PROT_WRITE);
		return;
	}
	if (mode == MODE_NEST && nested_depth == 0) {
		volatile char *deeper;

		/*
		 * A second fault while handling the first. The handler runs on
		 * the alternate stack, so this is the arrangement a real
		 * stack-overflow handler is in, and the inner fault must be
		 * delivered rather than turning into a double fault.
		 */
		nested_depth = 1;
		deeper = (volatile char *)0x1000;	/* certainly nobody's */
		mode = MODE_LEAVE;
		*deeper = 1;
		/* not reached: the inner fault leaves by siglongjmp */
	}
	siglongjmp(out, 1);
}

int main(void)
{
	struct sigaction sa;
	/* Not SIGSTKSZ: it is not a constant in current glibc. */
	static char altstack[256 * 1024];
	stack_t ss;

	memset(&sa, 0, sizeof(sa));
	sa.sa_sigaction = on_segv;
	sa.sa_flags = SA_SIGINFO | SA_NODEFER | SA_ONSTACK;
	sigemptyset(&sa.sa_mask);

	ss.ss_sp = altstack;
	ss.ss_size = sizeof(altstack);
	ss.ss_flags = 0;
	if (sigaltstack(&ss, NULL) != 0 ||
	    sigaction(SIGSEGV, &sa, NULL) != 0 ||
	    sigaction(SIGBUS, &sa, NULL) != 0) {
		say("pf2: setup failed\nFAIL\n");
		return 1;
	}

	/* --- a guard page below a region, hit by walking off the end. */
	{
		char *base;
		volatile int left = 0;

		say("pf2 guard-page: about to walk off the end of a region\n");
		/* [guard][data] with the guard first, so walking *down* hits it. */
		base = mmap(NULL, 3 * PGSZ, PROT_READ | PROT_WRITE,
			    MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
		if (base == MAP_FAILED ||
		    mprotect(base, PGSZ, PROT_NONE) != 0) {
			check("guard-page", 0, "setup failed");
		} else {
			/*
			 * volatile, and one byte at a time. Without it gcc
			 * vectorises the descending walk and stores sixteen or
			 * thirty-two bytes at a time, starting below where the
			 * loop does -- so the first fault lands a page past the
			 * guard and the test measures the wrong address. It
			 * reported a fault at base-0xfff for a guard at base.
			 */
			volatile char *data = base + PGSZ;
			long i;

			hits = 0;
			mode = MODE_LEAVE;
			if (sigsetjmp(out, 1) == 0) {
				/* Walk backwards out of the data region. */
				for (i = 0; i > -(long)(2 * PGSZ); i--)
					data[i] = 1;
			} else {
				left = 1;
			}
			check("guard-page", left && hits == 1 &&
			      got_addr == (void *)(base + PGSZ - 1),
			      "walking down from the region faulted at %p, "
			      "guard ends at %p", got_addr,
			      (void *)(base + PGSZ - 1));
			munmap(base, 3 * PGSZ);
		}
	}

	/* --- W^X: write code, make it executable, run it, make it writable. */
	{
		/* mov eax, 0x2a; ret  -- returns 42 */
		static const unsigned char code1[] = { 0xb8, 0x2a, 0x00, 0x00, 0x00, 0xc3 };
		static const unsigned char code2[] = { 0xb8, 0x63, 0x00, 0x00, 0x00, 0xc3 };
		unsigned char *page;
		int (*fn)(void);
		int r1 = 0, r2 = 0;

		say("pf2 w-xor-x: writing code, then executing it\n");
		page = mmap(NULL, PGSZ, PROT_READ | PROT_WRITE,
			    MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
		if (page == MAP_FAILED) {
			check("w-xor-x", 0, "mmap failed");
		} else {
			memcpy(page, code1, sizeof(code1));
			if (mprotect(page, PGSZ, PROT_READ | PROT_EXEC) != 0) {
				check("w-xor-x", 0, "mprotect to r-x failed");
			} else {
				fn = (int (*)(void))page;
				r1 = fn();
				if (mprotect(page, PGSZ,
					     PROT_READ | PROT_WRITE) != 0) {
					check("w-xor-x", 0,
					      "mprotect back to rw- failed");
				} else {
					memcpy(page, code2, sizeof(code2));
					mprotect(page, PGSZ,
						 PROT_READ | PROT_EXEC);
					r2 = fn();
					check("w-xor-x", r1 == 42 && r2 == 99,
					      "the page returned %d then %d "
					      "(want 42 then 99)", r1, r2);
				}
			}
			munmap(page, PGSZ);
		}
	}

	/* --- a reservation opened one page at a time by the handler. */
	{
		char *res;
		const int pages = 8;
		int i, opened_faults;

		say("pf2 grow-on-demand: opening a reservation page by page\n");
		res = mmap(NULL, pages * PGSZ, PROT_NONE,
			   MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
		if (res == MAP_FAILED) {
			check("grow-on-demand", 0, "mmap failed");
		} else {
			hits = 0;
			mode = MODE_OPEN;
			for (i = 0; i < pages; i++)
				res[i * PGSZ] = (char)i;
			opened_faults = (int)hits;
			/* Touching them again must fault no further. */
			for (i = 0; i < pages; i++)
				res[i * PGSZ + 1] = (char)i;
			check("grow-on-demand",
			      opened_faults == pages && (int)hits == pages,
			      "%d pages took %d fault(s) to open and %d more "
			      "when touched again", pages, opened_faults,
			      (int)hits - opened_faults);

			/* And the bytes written through the handler survived. */
			for (i = 0; i < pages; i++)
				if (res[i * PGSZ] != (char)i)
					break;
			check("grow-on-demand contents", i == pages,
			      "%d of %d pages hold what was written after the "
			      "fault", i, pages);
			munmap(res, pages * PGSZ);
		}
	}

	/* --- si_addr is the byte, not the page. */
	{
		char *p;
		volatile int left = 0;

		say("pf2 si_addr-precision: faulting at a known offset\n");
		p = mmap(NULL, PGSZ, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS,
			 -1, 0);
		if (p == MAP_FAILED) {
			check("si_addr-precision", 0, "mmap failed");
		} else {
			hits = 0;
			mode = MODE_LEAVE;
			if (sigsetjmp(out, 1) == 0)
				p[1234] = 1;
			else
				left = 1;
			check("si_addr-precision",
			      left && got_addr == (void *)(p + 1234),
			      "touched %p, si_addr says %p",
			      (void *)(p + 1234), got_addr);
			munmap(p, PGSZ);
		}
	}

	/* --- a fault taken inside the handler, on the alternate stack. */
	{
		char *p;
		volatile int left = 0;

		say("pf2 nested: faulting inside the handler\n");
		p = mmap(NULL, PGSZ, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS,
			 -1, 0);
		if (p == MAP_FAILED) {
			check("nested", 0, "mmap failed");
		} else {
			hits = 0;
			nested_depth = 0;
			mode = MODE_NEST;
			if (sigsetjmp(out, 1) == 0)
				*p = 1;
			else
				left = 1;
			check("nested", left && hits == 2 && nested_depth == 1,
			      "%d fault(s) taken, depth %d (want 2 and 1)",
			      (int)hits, nested_depth);
			munmap(p, PGSZ);
		}
	}

	/* --- jumping into a page that is not executable. */
	{
		unsigned char *page;
		volatile int left = 0;

		say("pf2 no-exec: about to call a page without PROT_EXEC\n");
		page = mmap(NULL, PGSZ, PROT_READ | PROT_WRITE,
			    MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
		if (page == MAP_FAILED) {
			check("no-exec", 0, "mmap failed");
		} else {
			int (*fn)(void) = (int (*)(void))page;
			static const unsigned char ret_42[] = {
				0xb8, 0x2a, 0x00, 0x00, 0x00, 0xc3
			};

			memcpy(page, ret_42, sizeof(ret_42));
			hits = 0;
			mode = MODE_LEAVE;
			if (sigsetjmp(out, 1) == 0)
				fn();
			else
				left = 1;
			check("no-exec", left && hits == 1 &&
			      got_addr == (void *)page,
			      "calling a rw- page faulted %d time(s) at %p "
			      "(want the page itself, %p)", (int)hits,
			      got_addr, (void *)page);
			munmap(page, PGSZ);
		}
	}

	sayf("pf2: %d case(s) bad\n", fails);
	say(fails == 0 ? "PASS\n" : "FAIL\n");
	return fails == 0 ? 0 : 1;
}
