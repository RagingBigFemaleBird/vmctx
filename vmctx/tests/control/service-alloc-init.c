// SPDX-License-Identifier: GPL-2.0
/* Isolated QEMU /init only. An unpatched kernel is expected to panic.
 * Build statically with the matching vmctx UAPI, VMCTX_RUN_NR / VMCTX_CTL_NR,
 * and RUN_START / RUN_END from that kernel's vmctx_run_current symbol.
 * Requires FAILSLAB, FAULT_INJECTION_DEBUG_FS and STACKTRACE_FILTER.
 * No hardware backend, network, host disks or host fault injection needed.
 */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mount.h>
#include <sys/reboot.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <unistd.h>
#include <linux/vmctx.h>

static void finish(int pass)
{
	printf("SERVICE_ALLOC_AUDIT_%s\n", pass ? "PASS" : "FAIL");
	fflush(stdout);
	reboot(RB_POWER_OFF);
	_exit(1);
}

static void die(const char *why)
{
	perror(why);
	finish(0);
}

static void deadline(int sig)
{
	(void)sig;
	static const char msg[] = "SERVICE_ALLOC_AUDIT_TIMEOUT\n";
	ssize_t written = write(STDOUT_FILENO, msg, sizeof(msg) - 1);
	(void)written;
	reboot(RB_POWER_OFF);
	_exit(1);
}

static void set_file(const char *name, const char *value)
{
	int fd = open(name, O_WRONLY | O_CLOEXEC);
	if (fd < 0 || write(fd, value, strlen(value)) != (ssize_t)strlen(value))
		die(name);
	if (close(fd))
		die("close");
}

static long mm_live(void)
{
	long n = -1;
	FILE *f = fopen("/sys/module/kernel/parameters/vmctx_mm_live", "r");
	if (!f || fscanf(f, "%ld", &n) != 1 || fclose(f))
		die("mm_live");
	return n;
}

int main(void)
{
	struct vmctx_run_config cfg = {.backing_fd = -1, .shared_fd = -1};
	struct sigaction sa = {.sa_handler = deadline};
	char buf[80];
	int status, attached = 0;
	pid_t child;
	setvbuf(stdout, NULL, _IONBF, 0);
	if (getpid() != 1) {
		fprintf(stderr, "Run as /init in an isolated QEMU guest only.\n");
		return 2;
	}
	if (mount("proc", "/proc", "proc", 0, NULL) ||
	    mount("sysfs", "/sys", "sysfs", 0, NULL) ||
	    mount("debugfs", "/sys/kernel/debug", "debugfs", 0, NULL))
		die("mount");
	sigemptyset(&sa.sa_mask);
	if (sigaction(SIGALRM, &sa, NULL))
		die("sigaction");
	alarm(30);
	if (mm_live() != 0)
		die("initial mm count");
	errno = 0;
	long rc = syscall(VMCTX_RUN_NR, &cfg);
	if (rc != -1 || errno != ENODEV)
		die("backend absence control");
	puts("BACKEND_ABSENT_PASS");
	set_file("/sys/kernel/debug/failslab/ignore-gfp-wait", "N");
	snprintf(buf, sizeof(buf), "0x%lx", (unsigned long)RUN_START);
	set_file("/sys/kernel/debug/failslab/require-start", buf);
	snprintf(buf, sizeof(buf), "0x%lx", (unsigned long)RUN_END);
	set_file("/sys/kernel/debug/failslab/require-end", buf);
	set_file("/sys/kernel/debug/failslab/verbose", "2");
	int nth = open("/proc/self/fail-nth", O_RDWR | O_CLOEXEC);
	if (nth < 0)
		die("fail-nth");
	cfg.flags = VMCTX_FLAG_SERVICE | VMCTX_FLAG_WAIT_MONITOR;
	for (int i = 0; i < 5; i++) {
		puts("SERVICE_ALLOCATION_ARMING");
		if (pwrite(nth, "1", 1, 0) != 1)
			die("arm fail-nth");
		errno = 0;
		rc = syscall(VMCTX_RUN_NR, &cfg);
		int saved_errno = errno;
		ssize_t got = pread(nth, buf, sizeof(buf) - 1, 0);
		if (pwrite(nth, "0", 1, 0) != 1)
			die("disarm fail-nth");
		if (got <= 0)
			die("read fail-nth");
		buf[got] = 0;
		long live = mm_live();
		printf("SERVICE_FAILURE round=%d rc=%ld errno=%d remaining=%smm_live=%ld\n",
		       i, rc, saved_errno, buf, live);
		if (rc != -1 || saved_errno != ENOMEM || strcmp(buf, "0\n") || live != 0)
			finish(0);
	}
	if (close(nth))
		die("close fail-nth");
	/* A normal creation must still attach and retire after the failures. */
	child = fork();
	if (child < 0)
		die("fork");
	if (!child) {
		(void)syscall(VMCTX_RUN_NR, &cfg);
		_exit(125);
	}
	for (int i = 0; i < 2000; i++) {
		if (!syscall(VMCTX_CTL_NR, child, VMCTX_CTL_ATTACH, NULL)) {
			attached = 1;
			break;
		}
		usleep(1000);
	}
	long during = mm_live();
	if (kill(child, SIGKILL))
		die("kill service");
	if (waitpid(child, &status, 0) != child)
		die("reap service");
	long after = mm_live();
	printf("SERVICE_RETRY attached=%d mm_live=%ld/%ld killed=%d\n",
	       attached, during, after, WIFSIGNALED(status) && WTERMSIG(status) == SIGKILL);
	finish(attached && during == 1 && after == 0 &&
	       WIFSIGNALED(status) && WTERMSIG(status) == SIGKILL);
}
