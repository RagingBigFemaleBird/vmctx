// SPDX-License-Identifier: GPL-2.0
/* An instruction exception must use the source kernel's signal ABI, including
 * its FPU frame, automatic masks, and alternate-stack state. Run on x86-64/AVX.
 * cc -O2 -Wall -Wextra -static exception-state.c -o exception-state
 */
#define _GNU_SOURCE
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <ucontext.h>
#include <unistd.h>

uint8_t entry_ymm[32], returned_ymm[32];
uint32_t entry_mxcsr, returned_mxcsr;
uint16_t entry_fcw, returned_fcw;
static const uint8_t pattern[32] = {
    0x11,0x22,0x33,0x44,0x55,0x66,0x77,0x88,
    0x99,0xaa,0xbb,0xcc,0xdd,0xee,0xf0,0x01,
    0x81,0x72,0x63,0x54,0x45,0x36,0x27,0x18,
    0xf9,0xea,0xdb,0xcc,0xbd,0xae,0x9f,0x80
};
static const uint32_t original_mxcsr = 0x7f80, handler_mxcsr = 0x3f80;
static const uint16_t original_fcw = 0x0b7f;
static volatile sig_atomic_t seen, mask_ok, stack_ok, frame_ok;
static uintptr_t stack_base;
static size_t stack_bytes;

void exception_body(int sig, siginfo_t *info, void *context)
{
    ucontext_t *uc = context;
    sigset_t mask;
    stack_t current_stack;
    unsigned char local;
    if (sig != SIGILL || info->si_code != ILL_ILLOPN || ++seen != 1)
        _exit(91);
    if (sigprocmask(SIG_SETMASK, NULL, &mask) || sigaltstack(NULL, &current_stack))
        _exit(92);
    mask_ok = sigismember(&mask, SIGILL) && sigismember(&mask, SIGUSR1) &&
              sigismember(&mask, SIGUSR2);
    stack_ok = (uintptr_t)&local >= stack_base &&
               (uintptr_t)&local < stack_base + stack_bytes &&
               (current_stack.ss_flags & SS_ONSTACK);
    frame_ok = uc->uc_mcontext.fpregs &&
               uc->uc_mcontext.fpregs->mxcsr == original_mxcsr &&
               uc->uc_mcontext.fpregs->cwd == original_fcw &&
               !memcmp(&uc->uc_mcontext.fpregs->_xmm[15], pattern, 16) &&
               uc->uc_mcontext.gregs[REG_TRAPNO] == 6 &&
               uc->uc_mcontext.gregs[REG_RIP] == (greg_t)(uintptr_t)info->si_addr &&
               sigismember(&uc->uc_sigmask, SIGUSR2) &&
               !sigismember(&uc->uc_sigmask, SIGUSR1);
    uc->uc_mcontext.gregs[REG_RIP] += 2; /* resume after the deliberate UD2 */
    __asm__ volatile("vpcmpeqd %%ymm15, %%ymm15, %%ymm15\n\tldmxcsr %0"
                     : : "m"(handler_mxcsr) : "xmm15", "memory");
}

__asm__(".text\n"
        ".global exception_entry\n"
        ".type exception_entry,@function\n"
        "exception_entry:\n"
        "vmovdqu %ymm15, entry_ymm(%rip)\n"
        "stmxcsr entry_mxcsr(%rip)\n"
        "fnstcw entry_fcw(%rip)\n"
        "jmp exception_body\n"
        ".size exception_entry,.-exception_entry\n");
extern void exception_entry(int, siginfo_t *, void *);

int main(void)
{
    alarm(15);
    if (!__builtin_cpu_supports("avx")) {
        fputs("AVX required for the exception-state control\n", stderr);
        return 2;
    }
    stack_bytes = 65536;
    void *memory = mmap(NULL, stack_bytes, PROT_READ | PROT_WRITE,
                        MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (memory == MAP_FAILED) return 2;
    stack_base = (uintptr_t)memory;
    stack_t alt = {.ss_sp = memory, .ss_size = stack_bytes};
    struct sigaction action = {.sa_sigaction = exception_entry,
                               .sa_flags = SA_SIGINFO | SA_ONSTACK};
    sigset_t mask;
    if (sigemptyset(&action.sa_mask) || sigaddset(&action.sa_mask, SIGUSR1) ||
        sigemptyset(&mask) || sigaddset(&mask, SIGUSR2) ||
        sigprocmask(SIG_BLOCK, &mask, NULL) || sigaltstack(&alt, NULL) ||
        sigaction(SIGILL, &action, NULL)) return 2;
    __asm__ volatile("vmovdqu %3, %%ymm15\n\tldmxcsr %4\n\tfldcw %5\n\t"
                     "ud2\n\tvmovdqu %%ymm15, %0\n\tstmxcsr %1\n\tfnstcw %2"
                     : "=m"(returned_ymm), "=m"(returned_mxcsr), "=m"(returned_fcw)
                     : "m"(pattern), "m"(original_mxcsr), "m"(original_fcw)
                     : "xmm15", "memory");
    uint8_t zero[32] = {0};
    int entry_ok = !memcmp(entry_ymm, zero, 32) && entry_mxcsr == 0x1f80 && entry_fcw == 0x037f;
    int return_ok = !memcmp(returned_ymm, pattern, 32) && returned_mxcsr == original_mxcsr &&
                    returned_fcw == original_fcw;
    if (sigprocmask(SIG_SETMASK, NULL, &mask)) return 2;
    int mask_restored = sigismember(&mask, SIGUSR2) &&
                        !sigismember(&mask, SIGILL) && !sigismember(&mask, SIGUSR1);
    int pass = seen == 1 && entry_ok && return_ok && frame_ok && mask_ok && stack_ok && mask_restored;
    printf("%s exception state: entry=%d return=%d frame=%d mask=%d stack=%d mask_return=%d\n",
           pass ? "PASS" : "FAIL", entry_ok, return_ok, frame_ok, mask_ok, stack_ok, mask_restored);
    return pass ? 0 : 1;
}
