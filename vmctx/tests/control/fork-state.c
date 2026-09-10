/* SPDX-License-Identifier: GPL-2.0 */
/* x86-64 fork must preserve the calling thread's extended register state.
 * Capture XMM15 and MXCSR directly after the raw syscall, before libc can
 * change them. The parent and child independently check their captured state.
 * cc -O2 -Wall -Wextra -static fork-state.c -o fork-state
 */
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <sys/wait.h>
#include <unistd.h>

int main(void)
{
    const uint64_t expected[2] = { UINT64_C(0x1937abcdef028465),
                                  UINT64_C(0xfedcba9876543210) };
    uint64_t observed[2] = { 0, 0 };
    uint32_t original_mxcsr, expected_mxcsr, observed_mxcsr = 0;
    long child;
    int status;

    __asm__ volatile("stmxcsr %0" : "=m" (original_mxcsr));
    /* Round toward zero, retaining the existing exception masks and flags. */
    expected_mxcsr = original_mxcsr | 0x6000;
    __asm__ volatile(
        "movdqu %[expected], %%xmm15\n\t"
        "ldmxcsr %[mxcsr]\n\t"
        "mov $57, %%eax\n\t"
        "syscall\n\t"
        "movdqu %%xmm15, %[observed]\n\t"
        "stmxcsr %[after]"
        : "=a" (child), [observed] "=m" (observed),
          [after] "=m" (observed_mxcsr)
        : [expected] "m" (expected), [mxcsr] "m" (expected_mxcsr)
        : "rcx", "r11", "xmm15", "memory");
    __asm__ volatile("ldmxcsr %0" : : "m" (original_mxcsr));

    if (child < 0) {
        fprintf(stderr, "FAIL: raw fork returned %ld\n", child);
        return 2;
    }
    int good = observed[0] == expected[0] && observed[1] == expected[1] &&
               observed_mxcsr == expected_mxcsr;
    printf("%s: %s xmm15=%016llx:%016llx mxcsr=%08x expected=%08x\n",
           good ? "PASS" : "FAIL", child ? "parent" : "child",
           (unsigned long long)observed[1], (unsigned long long)observed[0],
           observed_mxcsr, expected_mxcsr);
    fflush(stdout);
    if (!child)
        _exit(good ? 0 : 91);
    pid_t waited;
    do {
        waited = waitpid((pid_t)child, &status, 0);
    } while (waited < 0 && errno == EINTR);
    if (waited != child || !WIFEXITED(status) || WEXITSTATUS(status)) {
        fprintf(stderr, "FAIL: child did not pass the inherited-state check\n");
        return 1;
    }
    if (!good)
        return 1;
    puts("PASS: fork preserves parent and child extended state");
    return 0;
}
