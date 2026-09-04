// SPDX-License-Identifier: GPL-2.0
/*
 * hp1 — a fork from a threaded parent must not change the parent's memory.
 *
 * The failure this exists for is already reproducible: a parent with four
 * threads calls fork(), the child does nothing but _exit(0), and the PARENT
 * dies with glibc's heap check
 *
 *   Fatal glibc error: malloc.c:2371 (sysmalloc): assertion failed:
 *   (old_top == initial_top (av) && old_size == 0) || ...prev_inuse(old_top)...
 *
 * about one run in five, with no namespace, no sandbox and no load. But that
 * assertion fires at the NEXT allocation that grows the arena, which can be far
 * from the damage and tells you nothing about which page went wrong or what is
 * in it. Eight mechanisms have been proposed for this and eight were refuted by
 * measurement; what has been missing every time is the address.
 *
 * So this does not rely on glibc noticing. It writes a pattern it can check
 * itself, and when the pattern breaks it says WHERE and WHAT, which is the
 * whole point of the test:
 *
 *   every 8-byte word holds (its own address ^ MAGIC)
 *
 * That choice is what makes the answer diagnostic rather than merely "wrong",
 * because each of the ways a page can be lost leaves a different residue and
 * they can be told apart on sight:
 *
 *   got == 0                     the page was INVENTED here -- somebody's
 *                                fault was answered with a fresh zero page
 *                                while the real bytes were elsewhere.
 *   got == (other_addr ^ MAGIC)  the page is ANOTHER page's contents: two
 *                                addresses resolved to one, and the offset
 *                                between them is printed, which names the
 *                                aliasing directly.
 *   got == stale/garbage         neither: the bytes are from some earlier
 *                                state or from another program's memory.
 *
 * The region is written BEFORE the fork and verified AFTER it, so anything that
 * changed belongs to the fork window and to nothing else. The threads spin on
 * their own stacks and never touch the heap, so the parent is the only writer
 * of these bytes in the whole program -- there is no data race to explain a
 * mismatch away with.
 *
 * It reports every distinct page it finds damaged, not just the first, because
 * "one page" and "a run of pages" are different bugs, and the count separates
 * a lost single transfer from a lost range.
 *
 * NOT IN THE SUITE, and what it has established so far is mostly negative --
 * which is the useful part, because this failure has now survived nine
 * plausible explanations.
 *
 * Run 24 times it fails 3 to 5 times, and in EVERY failure all three regions
 * verify clean or are never reached, because the program dies first. The deaths
 * are consistent and they are not in the parent's data:
 *
 *   FAIL: exception 14 at 0x3d8 (err 0x4, a read) then at 0x0 (err 0x6, write)
 *   FAIL: exception 13, general protection, at 0x0, rsp 0x7ffff7ef51b8
 *   FAIL: pthread_create returning failure for one of the four
 *
 * rsp on a THREAD's stack, faulting at zero or a small offset from it: a
 * pointer that should be valid is read as 0, inside the C library's thread
 * machinery, while the parent's heap, .data and .bss are all intact. So the
 * page that goes missing belongs to a SIBLING context, not to the one calling
 * fork, and it goes missing between the fork and the thread's next touch.
 *
 * Ruled out by measurement, each with an instrument that already existed:
 *   - the owner's 2s GET timeout            "remote GET of"        0 of 34 runs
 *   - the zero-fill fabrication as the path "fill it with zeros"   1 of 9 fails
 *   - page_recall_all losing a page         "LOST on settle"       0 of 24 runs
 *   - copy-served movable pages             6 per run, pass AND fail alike
 *
 * NEXT INSTRUMENT, and the reason this file stops here: the program has to
 * survive its own crash to say anything. A SIGSEGV handler on a sigaltstack --
 * so it still runs when the thread's own stack is the corrupted thing -- that
 * prints si_addr, rip, rsp and a dump of the words around rsp, turns each of
 * these deaths from "signal 11" into the actual bytes that were wrong. That is
 * the shape that solved the peek bug: stop ranking mechanisms and photograph
 * the page.
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <stdint.h>
#include <unistd.h>
#include <pthread.h>
#include <signal.h>
#include <ucontext.h>
#include <sys/wait.h>

#define NTHREADS	4
#define MAGIC		0x5a5a1234deadbeefULL
#define PG		4096UL
/* Big enough to span many pages and to make the arena grow, small enough that
 * verifying it is instant. */
#define WORDS		(1UL << 17)		/* 1 MiB */
#define GWORDS		(1UL << 13)		/* 64 KiB each */

/*
 * Three regions, because the first version of this test checked only the heap,
 * found it perfectly intact in every failing run, and was therefore looking in
 * the wrong place. What the failures actually showed was pointers reading as
 * zero inside the C library's own state -- pthread_create returning failure,
 * faults at 0x8 and 0x3d8, a general-protection fault at 0 -- and in a static
 * binary that state does not live on the heap. It lives in .data and .bss.
 *
 * Those are different KINDS of memory to this system and are carried by
 * different code: .data is a writable PRIVATE FILE mapping of the executable,
 * .bss is anonymous, the heap is anonymous. Stamping all three and naming which
 * one broke is the difference between "memory was corrupted" and a lead.
 */
static uint64_t g_data[GWORDS] = { 1 };	/* initialised -> .data, file-backed */
static uint64_t g_bss[GWORDS];		/* uninitialised -> .bss, anonymous  */

struct region { const char *name; uint64_t *p; unsigned long n; };

/* Returns pages damaged; prints the residue of the first few. */
static unsigned long verify(const struct region *r)
{
	unsigned long i, bad = 0, pages = 0;
	uint64_t lastpage = ~0ULL;

	for (i = 0; i < r->n; i++) {
		uint64_t want = ((uint64_t)(uintptr_t)&r->p[i] ^ MAGIC);
		uint64_t got = r->p[i];
		uint64_t page;

		if (got == want)
			continue;
		bad++;
		page = (uint64_t)(uintptr_t)&r->p[i] & ~(PG - 1);
		if (page == lastpage)
			continue;
		lastpage = page;
		if (++pages > 4)
			continue;
		if (got == 0)
			fprintf(stderr, "hp1: %s page 0x%llx word %lu: ZEROS -- "
				"invented here, not transported (wanted 0x%llx)\n",
				r->name, (unsigned long long)page, i,
				(unsigned long long)want);
		else {
			uint64_t owner = got ^ MAGIC;

			fprintf(stderr, "hp1: %s page 0x%llx word %lu: got "
				"0x%llx wanted 0x%llx (as an address that is "
				"0x%llx, %+lld from here)\n", r->name,
				(unsigned long long)page, i,
				(unsigned long long)got,
				(unsigned long long)want,
				(unsigned long long)owner,
				(long long)owner - (long long)(uintptr_t)&r->p[i]);
		}
	}
	if (pages)
		fprintf(stderr, "hp1: %s DAMAGED: %lu word(s) over %lu page(s)\n",
			r->name, bad, pages);
	return pages;
}

static volatile int stop;

/*
 * The crash has to describe itself, because by the time anything else can look
 * the process is gone.
 *
 * Every failure of this test so far has been a signal, not a returned error:
 * "exception 14 at 0x3d8", "exception 13 at 0x0", with rsp on a thread's own
 * stack. The verification loop below never runs in those rounds, so the three
 * stamped regions prove nothing about them -- the program dies first, and all
 * that reaches the log is the number 11.
 *
 * So the handler runs ON A SEPARATE STACK (SA_ONSTACK, with sigaltstack armed
 * for every thread). That is the whole point: when the corrupted memory IS the
 * thread's stack, a handler on that stack cannot run, and the one piece of
 * evidence worth having is lost precisely in the case that matters.
 *
 * It writes with write(2) and a hand-rolled hex formatter rather than
 * fprintf -- not fastidiousness about async-signal-safety in a dying process,
 * but because the fault may be inside the C library's own state, and calling
 * back into that library to ask it to format a number is asking the broken
 * thing to explain itself.
 *
 * What it prints is chosen to distinguish the same three residues the stamped
 * regions distinguish: the faulting address, where the code was, and the words
 * around the stack pointer -- which is where the bad pointer was loaded from,
 * and therefore the page that was actually lost.
 */
#define ALTSZ (64 * 1024)	/* fixed: SIGSTKSZ is not a constant on modern glibc */
static char altstk[NTHREADS + 1][ALTSZ];
static volatile int altslot;

static void wr(const char *s2, unsigned long n)
{
	ssize_t r = write(2, s2, n);

	(void)r;
}

static void wrs(const char *s2)
{
	unsigned long n = 0;

	while (s2[n])
		n++;
	wr(s2, n);
}

static void wrx(unsigned long long v)
{
	static const char d[] = "0123456789abcdef";
	char b[19];
	int i;

	b[0] = '0'; b[1] = 'x';
	for (i = 0; i < 16; i++)
		b[2 + i] = d[(v >> ((15 - i) * 4)) & 0xf];
	wr(b, 18);
}

static void segv(int sig, siginfo_t *si, void *ucv)
{
	ucontext_t *uc = ucv;
	unsigned long long rip = (unsigned long long)uc->uc_mcontext.gregs[REG_RIP];
	unsigned long long rsp = (unsigned long long)uc->uc_mcontext.gregs[REG_RSP];
	unsigned long long err = (unsigned long long)uc->uc_mcontext.gregs[REG_ERR];
	int i;

	wrs("hp1: SIGNAL "); wrx((unsigned long long)sig);
	wrs(" at "); wrx((unsigned long long)(uintptr_t)si->si_addr);
	wrs(" rip "); wrx(rip);
	wrs(" rsp "); wrx(rsp);
	wrs(" err "); wrx(err);
	wrs("\n");
	/*
	 * The stack around the fault. If rsp itself is unreadable this fault is
	 * about the stack rather than about something it pointed at, and the
	 * nested fault says so as loudly as any message could.
	 */
	if (rsp) {
		const unsigned long long *w = (const unsigned long long *)(uintptr_t)rsp;

		for (i = 0; i < 8; i++) {
			wrs("hp1:   [rsp+"); wrx((unsigned long long)(i * 8));
			wrs("] = "); wrx(w[i]);
			/* Is this word one of ours, mangled or intact? */
			wrs((w[i] ^ MAGIC) == (unsigned long long)(uintptr_t)&w[i]
			    ? "  <- our stamp, intact" : "");
			wrs(w[i] == 0 ? "  <- ZERO" : "");
			wrs("\n");
		}
	}
	_exit(9);
}

/* Armed per thread: the alternate stack is a per-thread property. */
static void arm_handler(void)
{
	stack_t ss;
	int slot = __atomic_fetch_add(&altslot, 1, __ATOMIC_RELAXED);

	if (slot > NTHREADS)
		return;
	ss.ss_sp = altstk[slot];
	ss.ss_size = sizeof(altstk[0]);
	ss.ss_flags = 0;
	sigaltstack(&ss, NULL);
}

/* Kept strictly on the thread's own stack: the heap must have exactly one
 * writer, so that a mismatch cannot be blamed on the program. */
static unsigned burn(int depth, unsigned acc)
{
	volatile unsigned pad[64];
	unsigned i;

	for (i = 0; i < 64; i++)
		pad[i] = acc + i;
	if (depth > 0)
		acc = burn(depth - 1, acc + pad[depth & 63]);
	return acc + pad[0];
}

static void *spin(void *arg)
{
	unsigned acc = (unsigned)(uintptr_t)arg;

	arm_handler();

	while (!stop)
		acc = burn(8, acc);
	return NULL;
}

int main(void)
{
	pthread_t th[NTHREADS];
	uint64_t *p;
	unsigned long i, bad = 0, pages = 0;
	int st = 0, made = 0, k;
	pid_t kid;

	{
		struct sigaction sa;

		memset(&sa, 0, sizeof(sa));
		sa.sa_sigaction = segv;
		sa.sa_flags = SA_SIGINFO | SA_ONSTACK;
		sigemptyset(&sa.sa_mask);
		sigaction(SIGSEGV, &sa, NULL);
		sigaction(SIGBUS, &sa, NULL);
		sigaction(SIGILL, &sa, NULL);
		arm_handler();
	}

	p = malloc(WORDS * sizeof(*p));
	if (!p) {
		printf("FAIL malloc\n");
		return 2;
	}
	for (i = 0; i < WORDS; i++)
		p[i] = (uint64_t)(uintptr_t)&p[i] ^ MAGIC;
	for (i = 0; i < GWORDS; i++) {
		g_data[i] = (uint64_t)(uintptr_t)&g_data[i] ^ MAGIC;
		g_bss[i]  = (uint64_t)(uintptr_t)&g_bss[i] ^ MAGIC;
	}

	for (k = 0; k < NTHREADS; k++)
		if (pthread_create(&th[k], NULL, spin,
				   (void *)(uintptr_t)(k + 1)) == 0)
			made++;
	if (made != NTHREADS) {
		printf("FAIL only %d of %d threads\n", made, NTHREADS);
		return 2;
	}
	usleep(50000);

	kid = fork();
	if (kid < 0) {
		printf("FAIL fork: %s\n", strerror(errno));
		return 2;
	}
	if (kid == 0)
		_exit(0);		/* the child does nothing at all */

	if (waitpid(kid, &st, 0) != kid) {
		printf("FAIL waitpid\n");
		stop = 1;
		return 2;
	}
	stop = 1;
	for (k = 0; k < NTHREADS; k++)
		pthread_join(th[k], NULL);

	{
		struct region rs[3] = {
			{ "heap(anon)", p, WORDS },
			{ ".data(file)", g_data, GWORDS },
			{ ".bss(anon)", g_bss, GWORDS },
		};

		for (k = 0; k < 3; k++)
			pages += verify(&rs[k]);
		bad = pages;
	}

	if (bad) {
		fprintf(stderr, "hp1: FAIL -- %lu page(s) changed across a fork "
			"this side never wrote; heap 0x%llx..0x%llx, .data "
			"0x%llx, .bss 0x%llx\n", pages,
			(unsigned long long)(uintptr_t)p,
			(unsigned long long)(uintptr_t)(p + WORDS),
			(unsigned long long)(uintptr_t)g_data,
			(unsigned long long)(uintptr_t)g_bss);
		return 1;
	}
	printf("PASS\n");
	return 0;
}
