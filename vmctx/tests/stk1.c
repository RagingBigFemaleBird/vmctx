// SPDX-License-Identifier: GPL-2.0
/*
 * stk1 — the guest's stack must be able to grow.
 *
 * Every other case in this suite runs on the stack the loader already touched.
 * Nothing did this until /usr/bin/wc failed to run at all: glibc and Rust both
 * emit a stack-clash probe loop when a frame is larger than a page,
 *
 *	movq $0, (%rsp)		; touch this page
 *	cmp  %r11, %rsp		; reached the new frame yet?
 *	jne  .-...		;   no: keep walking down
 *	sub  $imm32, %rsp
 *
 * which walks rsp down a page at a time. The first probe below whatever the
 * loader touched faults on a page that has no VMA yet -- an ordinary
 * VM_GROWSDOWN stack fault, which the architecture's own fault handler answers
 * by expanding the stack.
 *
 * A VM context's faults do not go through that handler. They are serviced with
 * fixup_user_fault(), which looks the address up and returns -EFAULT when it
 * falls below the VMA: since Linux moved stack expansion into
 * lock_mm_and_find_vma(), no GUP-side path grows a stack. The guest therefore
 * dies the first time it needs a frame deeper than the one it started on, and
 * the kernel says so:
 *
 *	vmctx: usercode: unrecoverable guest #PF pid N at 0x... err 0x6 (-14)
 *	vmctx:   insn @rip: 48 c7 04 24 00 00 00 00 4c 39 dc 75 ec
 *
 * This walks 256 KiB down in page steps and writes a known value to each, then
 * reads them all back on the way out -- so it fails both when the growth is
 * refused and when a page grown into is quietly not the page written to.
 *
 * Deliberately not recursive: recursion needs calls the compiler may inline or
 * tail-optimise away, and the point is the memory, not the call depth.
 */
#include <stdio.h>
#include <string.h>

#define PGSZ  4096
#define PAGES 64			/* 256 KiB, well past one page */

/* Not static: the compiler must not see through to the whole array. */
volatile unsigned long stk1_sink;

/*
 * noinline so the frame is really made, and the array is touched by hand a
 * page at a time rather than by memset, so the access pattern is the probe
 * loop's and not a library's.
 */
__attribute__((noinline))
static int walk_down(void)
{
	volatile char deep[PAGES * PGSZ];
	int i, bad = 0;

	for (i = 0; i < PAGES; i++)
		deep[i * PGSZ] = (char)(i + 1);
	/* Read back in the other order, so a stale page cannot pass by luck. */
	for (i = PAGES - 1; i >= 0; i--)
		if (deep[i * PGSZ] != (char)(i + 1))
			bad++;
	stk1_sink = (unsigned long)&deep[0];
	return bad;
}

int main(void)
{
	int bad = walk_down();

	printf("stk1: %d page(s) of %d wrong after growing the stack %d KiB\n",
	       bad, PAGES, PAGES * PGSZ / 1024);
	if (bad) {
		printf("FAIL\n");
		return 1;
	}
	printf("PASS\n");
	return 0;
}
