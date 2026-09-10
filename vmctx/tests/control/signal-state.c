// SPDX-License-Identifier: GPL-2.0
/* Signal entry must initialize FP/SIMD state; sigreturn must restore it.
 * cc -O2 -Wall -Wextra -static signal-state.c -o signal-state
 * Register snapshots surround the raw kill syscall with no intervening C.
 */
#define _GNU_SOURCE
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <unistd.h>

uint64_t signal_entry[2];
uint32_t signal_mxcsr, changed_mxcsr = 0x3f80;
volatile sig_atomic_t signal_seen;
extern void state_handler(int);
__asm__(".text\n"
        ".globl state_handler\n"
        "state_handler:\n"
        "movdqu %xmm15, signal_entry(%rip)\n"
        "stmxcsr signal_mxcsr(%rip)\n"
        "pcmpeqb %xmm15, %xmm15\n"
        "ldmxcsr changed_mxcsr(%rip)\n"
        "movl $1, signal_seen(%rip)\n"
        "ret\n");

int main(void)
{
	struct sigaction sa = {.sa_handler = state_handler};
	const uint64_t wanted[2] = {0x1937abcdef028465, 0xfedcba9876543210};
	uint64_t after[2];
	uint32_t original, selected, actual;
	long result;
	sigemptyset(&sa.sa_mask);
	if (sigaction(SIGUSR1, &sa, NULL))
		return 2;
	pid_t pid = getpid();
	__asm__ volatile("stmxcsr %0" : "=m"(original));
	selected = original | 0x6000;
	__asm__ volatile("movdqu %[wanted], %%xmm15\n\t"
			 "ldmxcsr %[selected]\n\t"
			 "syscall\n\t"
			 "movdqu %%xmm15, %[after]\n\t"
			 "stmxcsr %[actual]\n\t"
			 "ldmxcsr %[original]"
		: "=a"(result), [after] "=m"(after), [actual] "=m"(actual)
		: "0"(62L), "D"((long)pid), "S"((long)SIGUSR1),
		  [wanted] "m"(wanted), [selected] "m"(selected), [original] "m"(original)
		: "rcx", "r11", "xmm15", "memory", "cc");
	int entry_ok = signal_seen && !signal_entry[0] && !signal_entry[1] &&
		       signal_mxcsr == 0x1f80;
	int return_ok = after[0] == wanted[0] && after[1] == wanted[1] && actual == selected;
	int pass = !result && entry_ok && return_ok;
	printf("%s kill=%ld seen=%d entry=%s xmm=%016lx:%016lx mxcsr=%x "
	       "return=%s xmm=%016lx:%016lx mxcsr=%x expected=%x\n",
	       pass ? "PASS" : "FAIL", result, signal_seen, entry_ok ? "PASS" : "FAIL",
	       signal_entry[1], signal_entry[0], signal_mxcsr, return_ok ? "PASS" : "FAIL",
	       after[1], after[0], actual, selected);
	return pass ? 0 : 1;
}
