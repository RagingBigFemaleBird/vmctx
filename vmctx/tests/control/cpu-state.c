// SPDX-License-Identifier: GPL-2.0
/* Kernel CPU-state adapter: round trip, malformed input, and exclusion from
 * an in-flight assisted syscall. Run: cpu-state <run nr> <ctl nr>.
 * cc -O2 -Wall -Wextra -static -pthread -D__EXPORTED_HEADERS__ \
 *    -I src/linux-7.0.14/include/uapi cpu-state.c -o cpu-state
 */
#define _GNU_SOURCE
#include <cpuid.h>
#include <errno.h>
#include <pthread.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/prctl.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <unistd.h>
#include <linux/vmctx.h>

static volatile sig_atomic_t child, expired;
static long ctl_nr;
static int read_status;
static char byte;
static struct vmctx_syscall read_call;
static struct vmctx_cpu_state original, wanted, actual, invalid;

static void deadline(int sig)
{
	(void)sig;
	expired = 1;
	if (child > 0)
		kill(child, SIGKILL);
}
static int cpu_ctl(unsigned op, void *s)
{
	for (unsigned i = 0; i < 10000 && !expired; i++) {
		int r = syscall(ctl_nr, child, op, s);
		if (!r || errno != EAGAIN)
			return r;
		usleep(100);
	}
	errno = ETIMEDOUT;
	return -1;
}
static int matches(void)
{
	if (cpu_ctl(VMCTX_CTL_GETCPU, &actual))
		return 0;
	return actual.regs.r12 == wanted.regs.r12 &&
	       !actual.regs.fs_base && !actual.regs.gs_base &&
	       !memcmp(actual.xstate, wanted.xstate, 2) &&
	       !memcmp(actual.xstate + 24, wanted.xstate + 24, 4) &&
	       !memcmp(actual.xstate + 160, wanted.xstate + 160, 256) &&
	       actual.xstate_size >= 840 &&
	       !memcmp(actual.xstate + 576, wanted.xstate + 576, 264);
}
static void *blocking_call(void *unused)
{
	(void)unused;
	read_status = syscall(ctl_nr, child, VMCTX_CTL_SYSCALL, &read_call);
	return NULL;
}
int main(int argc, char **argv)
{
	int fds[2], pass = 0, status, attached = 0, launched = 0;
	pthread_t thread;
	unsigned eax, ebx, ecx, edx;
	pid_t parent = getpid();
	struct sigaction sa = {.sa_handler = deadline};
	struct vmctx_run_config cfg = {
		.flags = VMCTX_FLAG_SERVICE | VMCTX_FLAG_WAIT_MONITOR,
		.backing_fd = -1, .shared_fd = -1,
	};
	if (argc != 3 || pipe(fds) || sigaction(SIGALRM, &sa, NULL))
		return 2;
	long run_nr = strtol(argv[1], NULL, 10);
	ctl_nr = strtol(argv[2], NULL, 10);
	__cpuid_count(0xd, 0, eax, ebx, ecx, edx);
	if ((eax & 7) != 7) {
		fputs("fixture requires x87/SSE/AVX support\n", stderr);
		return 2;
	}
	child = fork();
	if (child < 0)
		return 2;
	if (!child) {
		if (prctl(PR_SET_PDEATHSIG, SIGKILL) || getppid() != parent)
			_exit(125);
		syscall(run_nr, &cfg);
		_exit(126);
	}
	alarm(15);
	for (unsigned i = 0; i < 2000 && !expired; i++) {
		if (!syscall(ctl_nr, child, VMCTX_CTL_ATTACH, NULL)) {
			attached = 1;
			break;
		}
		usleep(1000);
	}
	if (!attached || cpu_ctl(VMCTX_CTL_GETCPU, &original))
		goto done;
	struct vmctx_cpu_model model, bad_model;
	if (syscall(ctl_nr, 0, VMCTX_CTL_CPU_CAPS, &model) ||
	    !vmctx_cpu_model_valid(&model)) goto done;
	bad_model = model; bad_model.reserved = 1;
	if (cpu_ctl(VMCTX_CTL_CPU_MODEL, &bad_model) != -1 || errno != EINVAL) goto done;
	bad_model = model; bad_model.leaf7_ebx |= 1U << 16;
	if (cpu_ctl(VMCTX_CTL_CPU_MODEL, &bad_model) != -1 || errno != EINVAL) goto done;
	if (cpu_ctl(VMCTX_CTL_CPU_MODEL, &model)) goto done;
	bad_model = model;
	bad_model.leaf1_ecx = 0; bad_model.leaf7_ebx = 0; bad_model.xcr0 = 3;
	if (cpu_ctl(VMCTX_CTL_CPU_MODEL, &bad_model) != -1 || errno != EPERM) goto done;
	puts("PASS: kernel rejects malformed models and freezes the accepted CPU contract");
	wanted = original;
	wanted.regs.r12 = UINT64_C(0xdeadface76543210);
	wanted.regs.fs_base = wanted.regs.gs_base = 0;
	memset(wanted.xstate, 0, sizeof(wanted.xstate));
	uint16_t fcw = 0x0f7f;
	uint32_t mxcsr = 0x7f80, id = 2, size = 256;
	uint64_t active = 7;
	memcpy(wanted.xstate, &fcw, 2);
	memcpy(wanted.xstate + 24, &mxcsr, 4);
	memcpy(wanted.xstate + 512, &active, 8);
	memcpy(wanted.xstate + 576, &id, 4);
	memcpy(wanted.xstate + 580, &size, 4);
	for (unsigned i = 0; i < 256; i++) {
		wanted.xstate[160 + i] = (unsigned char)(i * 37 + 11);
		wanted.xstate[584 + i] = (unsigned char)(i * 71 + 19);
	}
	wanted.xstate_size = 840;
	if (cpu_ctl(VMCTX_CTL_SETCPU, &wanted) || !matches())
		goto done;
	puts("PASS: GPR, zero FS/GS, x87 control, all XMM and YMM halves round trip");
	for (unsigned i = 0; i < 3; i++) {
		invalid = wanted;
		if (i == 0)
			invalid.xstate[519] |= 0x80; /* unsupported component 63 */
		else if (i == 1)
			invalid.xstate_size = 576; /* active YMM component missing */
		else
			invalid.xstate[580]--; /* component's size is wrong */
		if (cpu_ctl(VMCTX_CTL_SETCPU, &invalid) != -1 ||
		    (errno != EINVAL && errno != EOPNOTSUPP) || !matches())
			goto done;
	}
	puts("PASS: unsupported/truncated/malformed state rejected without mutation");
	read_call = (struct vmctx_syscall){ .nr = SYS_read,
		.args = {fds[0], (uintptr_t)&byte, 1} };
	if (pthread_create(&thread, NULL, blocking_call, NULL))
		goto done;
	launched = 1;
	/* Establish admission by observing exclusion, without assuming a delay
	 * is enough to put the service into its blocking read. */
	int busy = 0;
	for (unsigned i = 0; i < 2000 && !expired; i++) {
		if (cpu_ctl(VMCTX_CTL_GETCPU, &actual) < 0 && errno == EBUSY) {
			busy = 1;
			break;
		}
		usleep(1000);
	}
	struct vmctx_syscall second = {.nr = SYS_getpid};
	if (!busy || cpu_ctl(VMCTX_CTL_SETCPU, &wanted) != -1 || errno != EBUSY ||
	    cpu_ctl(VMCTX_CTL_CPU_MODEL, &model) != -1 || errno != EBUSY ||
	    syscall(ctl_nr, child, VMCTX_CTL_SYSCALL, &second) != -1 || errno != EBUSY ||
	    syscall(ctl_nr, child, VMCTX_CTL_EXCEPTION, &second) != -1 || errno != EBUSY)
		goto done;
	if (write(fds[1], "x", 1) != 1 || pthread_join(thread, NULL))
		goto done;
	launched = 0;
	if (read_status || read_call.ret != 1 || !matches())
		goto done;
	puts("PASS: blocked syscall excludes state writes and competing requests");
	pass = 1;
done:
	if (!pass)
		fprintf(stderr, "FAIL: CPU-state control errno=%d (%s) timeout=%d\n",
			errno, strerror(errno), (int)expired);
	kill(child, SIGKILL);
	if (launched)
		pthread_join(thread, NULL);
	pid_t reaped;
	do { reaped = waitpid(child, &status, 0); } while (reaped < 0 && errno == EINTR);
	alarm(0);
	close(fds[0]);
	close(fds[1]);
	return pass && !expired && reaped == child ? 0 : 1;
}
