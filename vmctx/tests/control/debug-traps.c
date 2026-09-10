// SPDX-License-Identifier: GPL-2.0
/* #DB carries an architectural cause; only the source assigns signal meaning. */
#define _GNU_SOURCE
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <ucontext.h>
#include <unistd.h>

extern char step_after[], ice_after[], ice_prefix_after[];
static volatile sig_atomic_t seen, failed;
static void trap(int signal, siginfo_t *info, void *context)
{
    ucontext_t *u = context;
    uintptr_t expected = (uintptr_t)(seen == 0 ? step_after :
                                     seen == 1 ? ice_after : ice_prefix_after);
    int code = seen ? TRAP_BRKPT : TRAP_TRACE;
    if (signal != SIGTRAP || info->si_code != code || seen > 2 ||
        u->uc_mcontext.gregs[REG_TRAPNO] != 1 ||
        u->uc_mcontext.gregs[REG_ERR] != 0 ||
        (uintptr_t)u->uc_mcontext.gregs[REG_RIP] != expected)
        failed = 1;
    u->uc_mcontext.gregs[REG_EFL] &= ~0x100; /* handler chooses to end stepping */
    seen++;
}
int main(void)
{
    struct sigaction action = {.sa_sigaction = trap, .sa_flags = SA_SIGINFO};
    if (sigemptyset(&action.sa_mask) || sigaction(SIGTRAP, &action, NULL)) return 2;
    alarm(10);
    __asm__ volatile("pushfq; orq $0x100, (%%rsp); popfq; nop; "
                     ".global step_after; step_after: nop; "
                     ".byte 0xf1; .global ice_after; ice_after: nop; "
                     ".byte 0x66, 0xf1; "
                     ".global ice_prefix_after; ice_prefix_after: nop"
                     ::: "memory", "cc");
    if (seen != 3 || failed) {
        fprintf(stderr, "FAIL: debug causes, frame or trap PC differ (seen=%d bad=%d)\n", seen, failed);
        return 1;
    }
    puts("PASS: TF and ICEBP return distinct source debug signals at the exact trap PC");
    return 0;
}
