// SPDX-License-Identifier: GPL-2.0
/* A native fork/vfork child exists before its parent can complete, but its
 * initial ptrace stops must finish before its CPU state can authorize entry. */
#define _GNU_SOURCE
#include <errno.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/prctl.h>
#include <sys/ptrace.h>
#include <sys/syscall.h>
#include <sys/user.h>
#include <sys/wait.h>
#include <unistd.h>
#include <linux/vmctx.h>

static volatile sig_atomic_t parent, kid, expired;
static long ctl_nr;
static unsigned sigchld_stops;
static struct vmctx_cpu_state cpu;
static void deadline(int sig)
{
    (void)sig; expired = 1;
    if (kid > 0) kill(kid, SIGKILL);
    if (parent > 0) kill(parent, SIGKILL);
}
static long ctl(pid_t pid, unsigned op, void *arg)
{
    return syscall(ctl_nr, pid, op, arg);
}
static int cpuctl(pid_t pid, unsigned op, void *arg)
{
    for (unsigned i = 0; i < 20000 && !expired; i++) {
        int r = ctl(pid, op, arg);
        if (!r || errno != EAGAIN) return r;
        usleep(100);
    }
    errno = ETIMEDOUT; return -1;
}
static int stopped(pid_t pid, int sig, unsigned event)
{
    int status = 0; pid_t got;
    do { got = waitpid(pid, &status, __WALL); } while (got < 0 && errno == EINTR);
    if (got != pid || !WIFSTOPPED(status) || WSTOPSIG(status) != sig ||
        (unsigned)status >> 16 != event) {
        fprintf(stderr, "stop: pid=%d got=%d status=%x expected=%d/%u\n",pid,got,status,sig,event);
        return 0;
    }
    return 1;
}
static int gate(struct vmctx_syscall_gate *g, unsigned op)
{
    g->op = op; return ctl(parent, VMCTX_CTL_SYSCALL_GATE, g);
}
static int state(struct vmctx_syscall_gate *g, unsigned expected)
{
    for (unsigned i = 0; i < 20000 && !expired; i++) {
        int r = gate(g, VMCTX_GATE_QUERY);
        if (!r && g->state == expected) return 0;
        if (r && errno != EAGAIN) return -1;
        if (expected == VMCTX_GATE_COMPLETE) {
            int status = 0;
            pid_t got = waitpid(parent, &status, __WALL | WNOHANG);
            if (got == parent) {
                fprintf(stderr, "completion stop: status=%x\n", status);
                if (!WIFSTOPPED(status) || WSTOPSIG(status) != SIGCHLD ||
                    (unsigned)status >> 16 ||
                    ptrace(PTRACE_CONT, parent, NULL, (void *)(uintptr_t)SIGCHLD)) {
                    errno = EPROTO; return -1;
                }
                sigchld_stops++;
            } else if (got < 0 && errno != EINTR) return -1;
        }
        usleep(100);
    }
    errno = ETIMEDOUT; return -1;
}
#define CHECK(c) do { if (!(c)) { \
    fprintf(stderr,"FAIL line %d: %s errno=%d timeout=%d\n",__LINE__,#c,errno,expired); \
    goto done; } } while (0)
int main(int argc, char **argv)
{
    if (argc != 4 || (strcmp(argv[3],"fork") && strcmp(argv[3],"vfork"))) return 2;
    int is_vfork = !strcmp(argv[3],"vfork"), pass = 0, status;
    long run_nr = strtol(argv[1], NULL, 10); ctl_nr = strtol(argv[2], NULL, 10);
    struct sigaction sa = {.sa_handler = deadline};
    CHECK(!sigaction(SIGALRM, &sa, NULL));
    pid_t monitor = getpid();
    struct vmctx_run_config cfg = {.flags = VMCTX_FLAG_SERVICE | VMCTX_FLAG_WAIT_MONITOR,
                                  .backing_fd = -1, .shared_fd = -1};
    parent = fork(); CHECK(parent >= 0);
    if (!parent) {
        if (prctl(PR_SET_PDEATHSIG, SIGKILL) || getppid() != monitor ||
            ptrace(PTRACE_TRACEME, 0, NULL, NULL) || raise(SIGSTOP)) _exit(125);
        syscall(run_nr, &cfg); _exit(126);
    }
    alarm(20);
    CHECK(stopped(parent, SIGSTOP, 0));
    CHECK(!ptrace(PTRACE_SETOPTIONS, parent, NULL, (void *)(uintptr_t)
                 (PTRACE_O_TRACESYSGOOD | PTRACE_O_TRACEFORK | PTRACE_O_TRACEVFORK |
                  PTRACE_O_TRACEVFORKDONE | PTRACE_O_EXITKILL)));
    CHECK(!ptrace(PTRACE_CONT, parent, NULL, NULL));
    for (unsigned i = 0; ctl(parent, VMCTX_CTL_ATTACH, NULL); i++) {
        CHECK(i < 2000 && !expired); usleep(1000);
    }
    struct vmctx_cpu_model model;
    CHECK(!ctl(0, VMCTX_CTL_CPU_CAPS, &model));
    CHECK(!cpuctl(parent, VMCTX_CTL_CPU_MODEL, &model));
    CHECK(!cpuctl(parent, VMCTX_CTL_GETCPU, &cpu));
    cpu.regs.orig_rax = is_vfork ? SYS_vfork : SYS_fork;
    CHECK(!cpuctl(parent, VMCTX_CTL_SETCPU, &cpu));
    CHECK(!kill(parent, SIGSTOP));
    struct vmctx_syscall_gate g = {.version = VMCTX_SYSCALL_GATE_ABI,
                                  .size = sizeof(g), .ticket = 1};
    CHECK(!gate(&g, VMCTX_GATE_BEGIN));
    CHECK(stopped(parent, SIGSTOP, 0));
    CHECK(!ptrace(PTRACE_SYSCALL, parent, NULL, NULL));
    CHECK(stopped(parent, SIGTRAP | 0x80, 0));
    CHECK(!ptrace(PTRACE_SYSCALL, parent, NULL, NULL));
    CHECK(!state(&g, VMCTX_GATE_ADMITTED));
    CHECK(g.call.nr == (uint64_t)(is_vfork ? SYS_vfork : SYS_fork));
    CHECK(!gate(&g, VMCTX_GATE_COMMIT));
    CHECK(stopped(parent, SIGTRAP, is_vfork ? PTRACE_EVENT_VFORK : PTRACE_EVENT_FORK));
    unsigned long event_pid = 0;
    CHECK(!ptrace(PTRACE_GETEVENTMSG, parent, NULL, &event_pid));
    kid = event_pid; CHECK(kid > 0);
    CHECK(!gate(&g, VMCTX_GATE_QUERY) && g.state == VMCTX_GATE_RUNNING);
    CHECK(g.child_host_pid == event_pid && g.child_pid == event_pid &&
          g.child_shared_mm == (unsigned)is_vfork);
    CHECK(stopped(kid, SIGSTOP, 0));
    CHECK(ctl(kid, VMCTX_CTL_GETCPU, &cpu) == -1 && errno == EAGAIN);
    struct user_regs_struct regs;
    CHECK(!ptrace(PTRACE_GETREGS, kid, NULL, &regs));
    regs.r13 = UINT64_C(0x1234432112344321);
    CHECK(!ptrace(PTRACE_SETREGS, kid, NULL, &regs));
    CHECK(!ptrace(PTRACE_SYSCALL, kid, NULL, NULL));
    /* Native fork has no separate child syscall-exit stop after this
     * SIGSTOP. Its next syscall stop would be a new entry, which a parked
     * service cannot execute. Compare native-fork-stop-order evidence. */
    CHECK(!cpuctl(kid, VMCTX_CTL_GETCPU, &cpu));
    CHECK(waitpid(kid, &status, __WALL | WNOHANG) == 0);
    CHECK(cpu.regs.rax == 0 && cpu.regs.r13 == UINT64_C(0x1234432112344321));
    CHECK(!ptrace(PTRACE_SYSCALL, parent, NULL, NULL));
    if (is_vfork) {
        usleep(100000);
        CHECK(!gate(&g, VMCTX_GATE_QUERY) && g.state == VMCTX_GATE_RUNNING);
        CHECK(!kill(kid, SIGKILL));
        CHECK(waitpid(kid, &status, __WALL) == kid && WIFSIGNALED(status) && WTERMSIG(status) == SIGKILL);
        kid = 0;
        CHECK(stopped(parent, SIGTRAP, PTRACE_EVENT_VFORK_DONE));
        CHECK(!ptrace(PTRACE_SYSCALL, parent, NULL, NULL));
    }
    CHECK(stopped(parent, SIGTRAP | 0x80, 0));
    CHECK(!ptrace(PTRACE_GETREGS, parent, NULL, &regs));
    CHECK(regs.rax == event_pid);
    regs.rax = -EPERM;
    CHECK(!ptrace(PTRACE_SETREGS, parent, NULL, &regs));
    CHECK(!ptrace(PTRACE_CONT, parent, NULL, NULL));
    CHECK(!state(&g, VMCTX_GATE_COMPLETE));
    CHECK(sigchld_stops == (unsigned)is_vfork);
    CHECK(g.dispatch_ret == (int64_t)event_pid && g.call.ret == -EPERM &&
          g.child_host_pid == event_pid && g.child_shared_mm == (unsigned)is_vfork);
    printf("PASS: native %s publishes committed child before parent completion; initial tracing gates child readiness and visible AX cannot erase creation\n",argv[3]);
    pass = 1;
done:
    if (kid > 0) kill(kid, SIGKILL);
    if (parent > 0) kill(parent, SIGKILL);
    while (waitpid(-1, &status, __WALL) > 0 || errno == EINTR) {}
    alarm(0);
    return pass && !expired ? 0 : 1;
}
