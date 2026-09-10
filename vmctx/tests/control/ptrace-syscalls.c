// SPDX-License-Identifier: GPL-2.0
/* Native tracing must precede source call classification. The tracer changes
 * ordinary calls into mmap and fork, and suppresses another call entirely. */
#define _GNU_SOURCE
#include <errno.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <sys/mman.h>
#include <sys/ptrace.h>
#include <sys/syscall.h>
#include <sys/user.h>
#include <sys/wait.h>
#include <unistd.h>
#define MAP_MARK 0x13579001UL
#define FORK_MARK 0x13579002UL
#define SKIP_MARK 0x13579003UL
#define SKIP_RESULT 0x19283746UL

static void tracee(void)
{
    if (ptrace(PTRACE_TRACEME, 0, NULL, NULL) || raise(SIGSTOP)) _exit(2);
    long result = syscall(SYS_getppid, MAP_MARK);
    if (result < 65536) _exit(3);
    volatile uint64_t *page = (void *)result;
    page[0] = UINT64_C(0xfeed1234abcd5678);
    pid_t child = syscall(SYS_getppid, FORK_MARK);
    if (child < 0) _exit(4);
    if (!child) {
        if (page[0] != UINT64_C(0xfeed1234abcd5678)) _exit(5);
        page[0] = 19;
        _exit(19);
    }
    int status;
    if (waitpid(child, &status, 0) != child || !WIFEXITED(status) ||
        WEXITSTATUS(status) != 19 || page[0] != UINT64_C(0xfeed1234abcd5678)) _exit(6);
    if (syscall(SYS_getppid, SKIP_MARK) != SKIP_RESULT) _exit(7);
    if (munmap((void *)page, 4096)) _exit(8);
    _exit(0);
}
int main(void)
{
    alarm(20);
    pid_t child = fork();
    if (child < 0) return 2;
    if (!child) tracee();
    int status = 0, entering = 1, counts[3] = {0};
    if (waitpid(child, &status, 0) != child || !WIFSTOPPED(status) ||
        WSTOPSIG(status) != SIGSTOP ||
        ptrace(PTRACE_SETOPTIONS, child, NULL, (void *)(uintptr_t)PTRACE_O_TRACESYSGOOD)) goto fail;
    int inject = 0;
    for (;;) {
        if (ptrace(PTRACE_SYSCALL, child, NULL, (void *)(intptr_t)inject) ||
            waitpid(child, &status, 0) != child) goto fail;
        inject = 0;
        if (WIFEXITED(status)) break;
        if (!WIFSTOPPED(status)) goto fail;
        if (WSTOPSIG(status) == SIGCHLD) { inject = SIGCHLD; continue; }
        if (WSTOPSIG(status) != (SIGTRAP | 0x80)) goto fail;
        struct user_regs_struct r;
        if (ptrace(PTRACE_GETREGS, child, NULL, &r)) goto fail;
        if (entering && r.orig_rax == SYS_getppid) {
            if (r.rdi == MAP_MARK) {
                counts[0]++; r.orig_rax = SYS_mmap;
                r.rdi = 0; r.rsi = 4096; r.rdx = PROT_READ | PROT_WRITE;
                r.r10 = MAP_PRIVATE | MAP_ANONYMOUS; r.r8 = -1UL; r.r9 = 0;
            } else if (r.rdi == FORK_MARK) {
                counts[1]++; r.orig_rax = SYS_fork;
            } else if (r.rdi == SKIP_MARK) {
                counts[2]++; r.orig_rax = -1UL; r.rax = SKIP_RESULT;
            } else goto fail;
            if (ptrace(PTRACE_SETREGS, child, NULL, &r)) goto fail;
        }
        entering = !entering;
    }
    if (WEXITSTATUS(status) || counts[0] != 1 || counts[1] != 1 || counts[2] != 1) goto fail;
    puts("PASS: source syscall tracing classifies rewritten mmap and fork, and preserves skipped-call results");
    return 0;
fail:
    fprintf(stderr, "FAIL: ptrace syscall admission status=%x errno=%d mutations=%d/%d/%d\n",
            status, errno, counts[0], counts[1], counts[2]);
    kill(child, SIGKILL);
    while (waitpid(child, &status, 0) < 0 && errno == EINTR) {}
    return 1;
}
