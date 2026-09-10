// SPDX-License-Identifier: GPL-2.0
/* Native source admission: canonical ptrace frame, no dispatch before commit,
 * skipped/rewritten calls, copyout retry and duplicate commit without replay. */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/prctl.h>
#include <sys/ptrace.h>
#include <sys/syscall.h>
#include <sys/user.h>
#include <sys/wait.h>
#include <unistd.h>
#include <linux/vmctx.h>

static volatile sig_atomic_t child, expired;
static long ctl_nr;
static struct vmctx_cpu_state original, cpu;
static const char message[] = "admitted exactly once";
static void deadline(int sig)
{
    (void)sig; expired = 1;
    if (child > 0) kill(child, SIGKILL);
}
static long ctl(unsigned op, void *arg)
{
    return syscall(ctl_nr, child, op, arg);
}
static int cpuctl(unsigned op, void *arg)
{
    for (unsigned i = 0; i < 10000 && !expired; i++) {
        int r = ctl(op, arg);
        if (!r || errno != EAGAIN) return r;
        usleep(100);
    }
    errno = ETIMEDOUT; return -1;
}
static int stopped(int expected)
{
    int status; pid_t pid;
    do { pid = waitpid(child, &status, 0); } while (pid < 0 && errno == EINTR);
    if (pid != child || !WIFSTOPPED(status) || WSTOPSIG(status) != expected) {
        fprintf(stderr, "stop: pid=%d status=%x expected=%d\n", pid, status, expected);
        return 0;
    }
    return 1;
}
static int gate(struct vmctx_syscall_gate *g, unsigned op)
{
    g->op = op;
    return ctl(VMCTX_CTL_SYSCALL_GATE, g);
}
static int gate_state(struct vmctx_syscall_gate *g, unsigned state)
{
    for (unsigned i = 0; i < 20000 && !expired; i++) {
        int r = gate(g, VMCTX_GATE_QUERY);
        if (!r && g->state == state) return 0;
        if (r && errno != EAGAIN) return -1;
        usleep(100);
    }
    errno = ETIMEDOUT; return -1;
}
#define CHECK(c) do { if (!(c)) { \
    fprintf(stderr,"FAIL line %d: %s errno=%d timeout=%d\n",__LINE__,#c,errno,expired); \
    goto done; } } while (0)

int main(int argc, char **argv)
{
    if (argc != 3) return 2;
    int pass = 0, status, fds[2] = {-1,-1};
    long run_nr = strtol(argv[1], NULL, 10);
    ctl_nr = strtol(argv[2], NULL, 10);
    struct sigaction sa = {.sa_handler = deadline};
    void *readonly = MAP_FAILED;
    CHECK(!sigaction(SIGALRM, &sa, NULL));
    CHECK(!pipe2(fds, O_NONBLOCK));
    pid_t parent = getpid();
    struct vmctx_run_config cfg = {.flags = VMCTX_FLAG_SERVICE | VMCTX_FLAG_WAIT_MONITOR,
                                  .backing_fd = -1, .shared_fd = -1};
    child = fork();
    CHECK(child >= 0);
    if (!child) {
        if (prctl(PR_SET_PDEATHSIG, SIGKILL) || getppid() != parent ||
            ptrace(PTRACE_TRACEME, 0, NULL, NULL) || raise(SIGSTOP)) _exit(125);
        syscall(run_nr, &cfg);
        _exit(126);
    }
    alarm(20);
    CHECK(stopped(SIGSTOP));
    CHECK(!ptrace(PTRACE_SETOPTIONS, child, NULL,
                 (void *)(uintptr_t)(PTRACE_O_TRACESYSGOOD | PTRACE_O_EXITKILL)));
    CHECK(!ptrace(PTRACE_CONT, child, NULL, NULL));
    for (unsigned i = 0; ctl(VMCTX_CTL_ATTACH, NULL); i++) {
        CHECK(i < 2000 && !expired); usleep(1000);
    }
    struct vmctx_cpu_model model;
    CHECK(!syscall(ctl_nr, 0, VMCTX_CTL_CPU_CAPS, &model));
    CHECK(!cpuctl(VMCTX_CTL_CPU_MODEL, &model));
    CHECK(!cpuctl(VMCTX_CTL_GETCPU, &original));
    cpu = original;
    cpu.regs.orig_rax = SYS_getppid;
    cpu.regs.rdi = 0x13579001;
    CHECK(!cpuctl(VMCTX_CTL_SETCPU, &cpu));
    /* A parked service does not execute instructions. BEGIN supplies the
     * wakeup at which its native return loop processes this requested stop. */
    CHECK(!kill(child, SIGSTOP));
    struct vmctx_syscall_gate g = {.version = VMCTX_SYSCALL_GATE_ABI,
                                  .size = sizeof(g), .ticket = 1};
    CHECK(!gate(&g, VMCTX_GATE_BEGIN));
    CHECK(stopped(SIGSTOP));
    CHECK(!ptrace(PTRACE_SYSCALL, child, NULL, NULL));
    CHECK(stopped(SIGTRAP | 0x80));
    struct user_regs_struct regs;
    CHECK(!ptrace(PTRACE_GETREGS, child, NULL, &regs));
    CHECK(regs.orig_rax == SYS_getppid && regs.rdi == 0x13579001);
    CHECK((long)regs.rax == -ENOSYS);
    regs.orig_rax = SYS_write; regs.rdi = fds[1];
    regs.rsi = (uintptr_t)message; regs.rdx = sizeof(message);
    regs.r12 = UINT64_C(0x123456789abcdef0);
    CHECK(!ptrace(PTRACE_SETREGS, child, NULL, &regs));
    CHECK(!ptrace(PTRACE_SYSCALL, child, NULL, NULL));
    CHECK(!gate_state(&g, VMCTX_GATE_ADMITTED));
    CHECK(g.call.nr == SYS_write && g.call.args[0] == (uint64_t)fds[1] &&
          g.call.args[1] == (uintptr_t)message && g.call.args[2] == sizeof(message));
    char bytes[64];
    CHECK(read(fds[0], bytes, sizeof(bytes)) == -1 && errno == EAGAIN);
    CHECK(cpuctl(VMCTX_CTL_SETCPU, &cpu) == -1 && errno == EBUSY);
    CHECK(ctl(VMCTX_CTL_SETREGS, &cpu.regs) == -1 && errno == EBUSY);
    struct vmctx_syscall internal = {.nr = SYS_getpid};
    CHECK(ctl(VMCTX_CTL_SYSCALL, &internal) == -1 && errno == EBUSY);
    struct vmctx_syscall_gate wrong = g; wrong.ticket = 2;
    CHECK(gate(&wrong, VMCTX_GATE_BEGIN) == -1 && errno == EBUSY);
    readonly = mmap(NULL, 4096, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    CHECK(readonly != MAP_FAILED);
    g.op = VMCTX_GATE_BEGIN; memcpy(readonly, &g, sizeof(g));
    CHECK(!mprotect(readonly, 4096, PROT_READ));
    CHECK(ctl(VMCTX_CTL_SYSCALL_GATE, readonly) == -1 && errno == EFAULT);
    CHECK(!gate_state(&g, VMCTX_GATE_ADMITTED));
    CHECK(read(fds[0], bytes, sizeof(bytes)) == -1 && errno == EAGAIN);
    CHECK(!mprotect(readonly, 4096, PROT_READ | PROT_WRITE));
    g.op = VMCTX_GATE_COMMIT; memcpy(readonly, &g, sizeof(g));
    CHECK(!mprotect(readonly, 4096, PROT_READ));
    CHECK(ctl(VMCTX_CTL_SYSCALL_GATE, readonly) == -1 && errno == EFAULT);
    CHECK(stopped(SIGTRAP | 0x80));
    CHECK(!ptrace(PTRACE_GETREGS, child, NULL, &regs));
    CHECK(regs.orig_rax == SYS_write && regs.rax == sizeof(message));
    regs.rax = 0x19283746;
    CHECK(!ptrace(PTRACE_SETREGS, child, NULL, &regs));
    CHECK(!ptrace(PTRACE_SYSCALL, child, NULL, NULL));
    CHECK(!gate_state(&g, VMCTX_GATE_COMPLETE));
    CHECK(g.dispatch_ret == sizeof(message));
    CHECK(!cpuctl(VMCTX_CTL_GETCPU, &cpu));
    CHECK(cpu.regs.rax == 0x19283746 && cpu.regs.r12 == UINT64_C(0x123456789abcdef0));
    CHECK(g.call.ret == 0x19283746 && g.call.regs.rax == 0x19283746 &&
          g.call.regs.r12 == UINT64_C(0x123456789abcdef0));
    CHECK(read(fds[0], bytes, sizeof(bytes)) == sizeof(message) &&
          !memcmp(bytes, message, sizeof(message)));
    CHECK(!gate(&g, VMCTX_GATE_COMMIT) && g.state == VMCTX_GATE_COMPLETE);
    CHECK(!gate(&g, VMCTX_GATE_BEGIN) && g.state == VMCTX_GATE_COMPLETE);
    CHECK(read(fds[0], bytes, sizeof(bytes)) == -1 && errno == EAGAIN);
    puts("PASS: native entry rewrite precedes dispatch; exit rewrite and copyout retries preserve one execution");
    cpu = original; cpu.regs.orig_rax = SYS_getppid; cpu.regs.rdi = 0x13579003;
    CHECK(!cpuctl(VMCTX_CTL_SETCPU, &cpu));
    g.ticket = 2;
    CHECK(!gate(&g, VMCTX_GATE_BEGIN));
    CHECK(stopped(SIGTRAP | 0x80));
    CHECK(!ptrace(PTRACE_GETREGS, child, NULL, &regs));
    CHECK(regs.orig_rax == SYS_getppid && regs.rdi == 0x13579003);
    regs.orig_rax = -1UL; regs.rax = 0x55667788;
    CHECK(!ptrace(PTRACE_SETREGS, child, NULL, &regs));
    CHECK(!ptrace(PTRACE_SYSCALL, child, NULL, NULL));
    CHECK(!gate_state(&g, VMCTX_GATE_ADMITTED));
    CHECK((int64_t)g.call.nr == -1);
    CHECK(!gate(&g, VMCTX_GATE_COMMIT));
    CHECK(stopped(SIGTRAP | 0x80));
    CHECK(!ptrace(PTRACE_GETREGS, child, NULL, &regs));
    CHECK(regs.rax == 0x55667788);
    CHECK(!ptrace(PTRACE_SYSCALL, child, NULL, NULL));
    CHECK(!gate_state(&g, VMCTX_GATE_COMPLETE));
    CHECK(g.call.ret == 0x55667788 && g.dispatch_ret == 0x55667788);
    wrong = g; wrong.ticket = 1;
    CHECK(gate(&wrong, VMCTX_GATE_COMMIT) == -1 && errno == ESTALE);
    puts("PASS: native skipped-call result and stale-ticket fencing");
    pass = 1;
done:
    if (child > 0) {
        kill(child, SIGKILL);
        while (waitpid(child, &status, 0) < 0 && errno == EINTR) {}
    }
    alarm(0);
    if (readonly != MAP_FAILED) munmap(readonly, 4096);
    if (fds[0] >= 0) close(fds[0]);
    if (fds[1] >= 0) close(fds[1]);
    return pass && !expired ? 0 : 1;
}
