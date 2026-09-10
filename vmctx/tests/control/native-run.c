// SPDX-License-Identifier: GPL-2.0
/* Native harness owner: native-run <seconds> <program> [args ...].
 * BusyBox setsid may fork and report its own success instead of the test's
 * status. Own the session explicitly and reap its descendants on completion. */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/prctl.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <unistd.h>
static volatile sig_atomic_t group, interrupted, cleaning;
static void stop(int sig)
{
	if (cleaning) _exit(124);
	interrupted = sig;
	if (group > 0) { kill(-group, SIGKILL); kill(group, SIGKILL); }
}
/* Session membership does not cover daemonized descendants. As their
 * subreaper, retain each adopted child until its pidfd has been signalled.
 * This single-threaded owner is the only reaper, so a listed child's PID
 * cannot be recycled before pidfd_open. Never search by process name. */
static int stop_children(void)
{
	char path[80];
	snprintf(path, sizeof(path), "/proc/self/task/%ld/children", (long)getpid());
	FILE *children = fopen(path, "r");
	if (!children) return -1;
	long child;
	int scanned;
	while ((scanned = fscanf(children, "%ld", &child)) == 1) {
		int handle = syscall(SYS_pidfd_open, (pid_t)child, 0);
		if (handle < 0) { fclose(children); return -1; }
		int sent = syscall(SYS_pidfd_send_signal, handle, SIGKILL, NULL, 0);
		int error = errno;
		close(handle);
		if (sent && error != ESRCH) { fclose(children); errno = error; return -1; }
	}
	int failed = scanned != EOF || ferror(children);
	fclose(children);
	return failed ? -1 : 0;
}
int main(int argc, char **argv)
{
	int report = -1, control_stdin = 0;
	while (argc >= 2) {
		if (!strcmp(argv[1], "--control-stdin")) {
			control_stdin = 1; argc--; argv++; continue;
		}
		if (argc < 3 || strcmp(argv[1], "--report")) break;
		if (report >= 0) return 2;
		/* A partial/absent report never proves cleanup. Keep this descriptor
		 * in the owner only, and publish success after the last child is reaped. */
		report = open(argv[2], O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
		if (report < 0) { perror("native-run: report"); return 2; }
		argc -= 2; argv += 2;
	}
	if (argc < 3) return 2;
	char *end;
	unsigned long seconds = strtoul(argv[1], &end, 10);
	if (*end || !seconds || seconds > 3600) return 2;
	struct sigaction sa = {.sa_handler = stop};
	if (sigaction(SIGALRM, &sa, NULL) || sigaction(SIGTERM, &sa, NULL) ||
	    sigaction(SIGINT, &sa, NULL) || prctl(PR_SET_CHILD_SUBREAPER, 1)) return 2;
	sigset_t blocked, old;
	sigemptyset(&blocked); sigaddset(&blocked, SIGALRM); sigaddset(&blocked, SIGTERM); sigaddset(&blocked, SIGINT);
	if (sigprocmask(SIG_BLOCK, &blocked, &old)) return 2;
	pid_t parent = getpid(), pid = fork();
	if (pid < 0) return 2;
	if (!pid) {
		if (report >= 0) close(report);
		if (control_stdin) {
			int input = open("/dev/null", O_RDONLY);
			if (input < 0 || dup2(input, STDIN_FILENO) < 0) _exit(125);
			if (input != STDIN_FILENO) close(input);
		}
		if (prctl(PR_SET_PDEATHSIG, SIGKILL) || getppid() != parent || setsid() < 0) _exit(125);
		struct sigaction def = {.sa_handler = SIG_DFL};
		sigaction(SIGALRM, &def, NULL); sigaction(SIGTERM, &def, NULL); sigaction(SIGINT, &def, NULL);
		sigprocmask(SIG_SETMASK, &old, NULL);
		execvp(argv[2], argv + 2); perror("native-run: exec"); _exit(127);
	}
	group = pid;
	alarm((unsigned)seconds);
	if (sigprocmask(SIG_SETMASK, &old, NULL)) { kill(-pid, SIGKILL); kill(pid, SIGKILL); return 2; }
	siginfo_t info = {0};
	int r;
	for (;;) {
		r = waitid(P_PID, pid, &info, WEXITED | WNOWAIT |
			   (control_stdin ? WNOHANG : 0));
		if (r && errno == EINTR) continue;
		if (r || info.si_pid) break;
		/* The SSH caller owns this control pipe. Data or EOF ends only
		 * this retained session; neither side signals a rediscovered PID. */
		struct pollfd input = {.fd = STDIN_FILENO, .events = POLLIN};
		int ready = poll(&input, 1, 20);
		if ((ready < 0 && errno != EINTR) ||
		    (ready > 0 && input.revents)) {
			stop(SIGTERM);
			control_stdin = 0;
		}
	}
	/* Keep the leader unreaped until the group is killed, so its numeric
	 * identity cannot be reused between the observation and cleanup. */
	cleaning = 1;
	kill(-pid, SIGKILL);
	group = 0;
	alarm(5);
	int status = 0;
	pid_t got;
	do { got = waitpid(pid, &status, 0); } while (got < 0 && errno == EINTR);
	if (r || got != pid) return 2;
	for (;;) {
		int other;
		if (stop_children()) return 2;
		pid_t p = waitpid(-1, &other, WNOHANG);
		if (p > 0 || (p < 0 && errno == EINTR)) continue;
		if (p < 0 && errno == ECHILD) break;
		if (p < 0) return 2;
		/* More children may be adopted after their parent exits. */
		usleep(1000);
	}
	alarm(0);
	int result = interrupted ? 124 : WIFEXITED(status) ? WEXITSTATUS(status) :
		WIFSIGNALED(status) ? 128 + WTERMSIG(status) : 2;
	if (report >= 0) {
		if (dprintf(report, "{\"complete\":true,\"survivors\":{},\"status\":%d,"
			    "\"timed_out\":%s,\"interrupted\":%d}\n", result,
			    interrupted == SIGALRM ? "true" : "false", interrupted) < 0 ||
		    fsync(report) || close(report)) return 2;
	}
	return result;
}
