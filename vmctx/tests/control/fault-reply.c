// SPDX-License-Identifier: GPL-2.0
/* A monitor's register reply must survive either report of a guest fault.
 * Compile against the tested kernel's UAPI, for example:
 * cc -O2 -Wall -Wextra -static -I src/linux-7.0.14/include/uapi \
 *    vmctx/tests/control/fault-reply.c -o fault-reply
 * Run: fault-reply <vmctx_run nr> <vmctx_ctl nr> <first|second>
 * The first-report arm is the positive control. The second-report arm declines
 * a PROT_NONE access once, then changes RIP and three GPRs. Guest code checks
 * all three registers and reports its verdict as the argument of exit_group.
 */
#define _GNU_SOURCE
#include <errno.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <unistd.h>
#include <linux/vmctx.h>

void *fault_address;
extern void fault_guest_entry(void), fault_guest_repaired(void);
__asm__(".text\n"
        ".globl fault_guest_entry\n"
        "fault_guest_entry:\n"
        "mov fault_address(%rip), %rax\n"
        "mov (%rax), %rbx\n"
        "ud2\n"
        ".globl fault_guest_repaired\n"
        "fault_guest_repaired:\n"
        "mov $91, %edi\n"
        "cmp $0x1234, %rax\n"
        "jne 1f\n"
        "cmp $0x5678, %rbx\n"
        "jne 1f\n"
        "cmp $0x7654, %r12\n"
        "jne 1f\n"
        "mov $37, %edi\n"
        "1: mov $231, %eax\n"
        "syscall\n"
        "ud2\n");

static volatile sig_atomic_t child, timed_out;
static void timeout_handler(int sig)
{
    (void)sig;
    timed_out = 1;
    if (child > 0)
        kill(child, SIGKILL);
}

int main(int argc, char **argv)
{
    struct vmctx_run_config cfg = {0};
    struct sigaction sa = {.sa_handler = timeout_handler};
    long run_nr, ctl_nr;
    int reports = 0, want, status = 0, verdict = -1, attached = 0;
    void *stack;
    pid_t waited = 0;
    if (argc != 4 ||
        (strcmp(argv[3], "first") && strcmp(argv[3], "second"))) {
        fprintf(stderr, "usage: %s <run nr> <ctl nr> <first|second>\n", argv[0]);
        return 2;
    }
    run_nr = strtol(argv[1], NULL, 10);
    ctl_nr = strtol(argv[2], NULL, 10);
    if (run_nr <= 0 || ctl_nr <= 0)
        return 2;
    want = !strcmp(argv[3], "second") ? 2 : 1;
    fault_address = mmap(NULL, 4096, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    stack = mmap(NULL, 65536, PROT_READ | PROT_WRITE,
                 MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (fault_address == MAP_FAILED || stack == MAP_FAILED) {
        perror("mmap");
        return 2;
    }
    memset(stack, 0, 65536);
    cfg.flags = VMCTX_FLAG_USERCODE | VMCTX_FLAG_EXIT_PROCESS |
                VMCTX_FLAG_REDIRECT_SYSCALL | VMCTX_FLAG_REDIRECT_FAULT |
                VMCTX_FLAG_WAIT_MONITOR;
    cfg.max_exits = 10000;
    cfg.entry = (uintptr_t)fault_guest_entry;
    cfg.stack = (uintptr_t)stack + 65536 - 256;
    cfg.backing_fd = cfg.shared_fd = -1;
    sigemptyset(&sa.sa_mask);
    if (sigaction(SIGALRM, &sa, NULL))
        return 2;
    child = fork();
    if (child < 0)
        return 2;
    if (!child) {
        syscall(run_nr, &cfg);
        _exit(126);
    }
    alarm(15);
    for (int i = 0; i < 2000 && !timed_out; i++) {
        if (syscall(ctl_nr, child, VMCTX_CTL_ATTACH, NULL) == 0) {
            attached = 1;
            break;
        }
        usleep(1000);
    }
    while (attached && !timed_out) {
        struct vmctx_event ev;
        struct vmctx_reply reply = {.action = VMCTX_ACT_SELF};
        if (syscall(ctl_nr, child, VMCTX_CTL_WAIT, &ev))
            break;
        if (ev.type == VMCTX_EV_FAULT && ev.fault_addr == (uintptr_t)fault_address) {
            printf("report=%d rip=0x%llx\n", ++reports, (unsigned long long)ev.rip);
            fflush(stdout);
            if (reports == want) {
                struct vmctx_uregs regs;
                if (syscall(ctl_nr, child, VMCTX_CTL_GETREGS, &regs))
                    break;
                regs.rip = (uintptr_t)fault_guest_repaired;
                regs.rax = 0x1234;
                regs.rbx = 0x5678;
                regs.r12 = 0x7654;
                if (syscall(ctl_nr, child, VMCTX_CTL_SETREGS, &regs))
                    break;
                reply.action = VMCTX_ACT_DONE;
            } else if (reports > want) {
                break;
            }
        } else if (ev.type == VMCTX_EV_SYSCALL && ev.nr == 231) {
            verdict = (int)ev.args[0];
            break;
        }
        if (syscall(ctl_nr, child, VMCTX_CTL_RESUME, &reply))
            break;
    }
    /* WAIT can observe context teardown before do_exit has completed. Give
     * that exit time to finish; an immediate SIGKILL can replace its status. */
    for (int phase = 0; phase < 2 && waited == 0; phase++) {
        if (phase)
            kill(child, SIGKILL);
        for (int i = 0; i < 200; i++) {
            waited = waitpid(child, &status, WNOHANG);
            if (waited == child || (waited < 0 && errno == ECHILD))
                break;
            usleep(10000);
        }
    }
    alarm(0);
    if (waited == child && WIFEXITED(status))
        verdict = WEXITSTATUS(status);
    int pass = attached && !timed_out && reports == want && verdict == 37 && waited == child;
    printf("%s: %s report, reports=%d verdict=%d reaped=%d timeout=%d\n",
           pass ? "PASS" : "FAIL", argv[3], reports, verdict,
           waited == child, (int)timed_out);
    return pass ? 0 : 1;
}
