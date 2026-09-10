/*
 * sig1 — signals that do not come from a fault.
 *
 * pf1, pf2, gp1, tr1 and pf3 all test signals the *processor* raised: a vector
 * comes out of the guest, the owner decides what it means, and the answer is a
 * register set. This tests the other half, which nothing covered: signals that
 * have no exception behind them at all.
 *
 * That half is a stricter test of the same rule. A fault at least starts on the
 * machine running the guest, so it is tempting to let that machine decide
 * something about it. A kill(2) starts and ends on the machine that owns the
 * program; the machine executing instructions has no part in it and no way to
 * know it happened. If it works, "a signal is a frame in the program's memory
 * and a register set" is the whole of the mechanism, and the destination needs
 * no concept of a signal to run a program that uses them.
 *
 *   self        raise(SIGUSR1) with a handler. The simplest case, and the one
 *               that says a signal can be delivered at a point the guest chose
 *               rather than at a fault.
 *   ordering    three signals raised in a row arrive in the order sent, and
 *               the handler sees each one once.
 *   mask        sigprocmask blocks SIGUSR2, it is raised, nothing runs, and it
 *               arrives on unblock -- so the mask is the program's and is
 *               honoured where the program's state lives.
 *   siginfo     SA_SIGINFO gives si_signo and a si_code of SI_USER (or
 *               SI_TKILL, which is what raise() gives on a threaded libc), and
 *               si_pid is this process. A signal carries data, not just a
 *               number.
 *   nested      a handler that raises a second, unblocked signal runs the
 *               second handler inside the first and comes back. Two frames on
 *               the guest's stack at once, and the return path through both.
 *   restart     SA_RESTART around a *forwarded blocking* syscall: a child
 *               signals the parent while it is asleep in read(2) on a pipe,
 *               and the read must resume and return the real bytes rather than
 *               EINTR or a short count. This is the case the kernel's own
 *               restart machinery would normally handle, and here the syscall
 *               is executing on another machine when the signal arrives.
 *   child       a signal from *another process* (the child kills the parent),
 *               which is the only case where the signal genuinely originates
 *               somewhere the guest's machine cannot see.
 *
 * write(2) rather than stdio throughout, for the reason pf1 gives: a case that
 * takes the context with it must not take the buffer too.
 */
#define _GNU_SOURCE
#include <string.h>
#include <stdio.h>
#include <stdarg.h>
#include <errno.h>
#include <signal.h>
#include <unistd.h>
#include <sys/wait.h>

static int fails;

static void say(const char *s)
{
	if (write(1, s, strlen(s)) < 0)
		_exit(1);
}

static void sayf(const char *fmt, ...)
{
	char b[256];
	va_list ap;

	va_start(ap, fmt);
	vsnprintf(b, sizeof(b), fmt, ap);
	va_end(ap);
	say(b);
}

static void check(const char *name, int ok, const char *fmt, ...)
{
	char b[256];
	va_list ap;

	va_start(ap, fmt);
	vsnprintf(b, sizeof(b), fmt, ap);
	va_end(ap);
	if (ok) {
		sayf("%-9s ok    %s\n", name, b);
		return;
	}
	fails++;
	sayf("%-9s FAIL  %s\n", name, b);
}

/*
 * volatile sig_atomic_t, and nothing else, for anything a handler touches: the
 * compiler is entitled to keep an ordinary variable in a register across the
 * point where the handler runs, and then the check reads the value from before
 * the signal and the test passes for the wrong reason.
 */
static volatile sig_atomic_t hits;
static volatile sig_atomic_t last_sig;
static volatile sig_atomic_t order[4];
static volatile sig_atomic_t n_order;
static volatile sig_atomic_t got_code;
static volatile sig_atomic_t got_pid_ok;
static volatile sig_atomic_t nested_depth;
static volatile sig_atomic_t nested_seen;

static void h_count(int sig)
{
	hits++;
	last_sig = sig;
	if (n_order < 4)
		order[n_order++] = sig;
}

static void h_info(int sig, siginfo_t *si, void *uc)
{
	(void)uc;
	hits++;
	last_sig = sig;
	got_code = si->si_code;
	got_pid_ok = (si->si_pid == getpid());
}

static void h_nested(int sig)
{
	hits++;
	if (sig == SIGUSR1) {
		nested_depth++;
		raise(SIGUSR2);		/* runs h_nested again, inside this one */
		nested_depth--;
	} else {
		nested_seen = nested_depth;
	}
}

/* Installed with SA_RESTART; does nothing but be delivered. */
static void h_nop(int sig)
{
	(void)sig;
	hits++;
}

static int install(int sig, void (*fn)(int), int flags)
{
	struct sigaction sa;

	memset(&sa, 0, sizeof(sa));
	sa.sa_handler = fn;
	sa.sa_flags = flags;
	sigemptyset(&sa.sa_mask);
	return sigaction(sig, &sa, NULL);
}

static void case_self(void)
{
	hits = 0;
	last_sig = 0;
	if (install(SIGUSR1, h_count, 0) != 0) {
		check("self", 0, "sigaction: %s", strerror(errno));
		return;
	}
	raise(SIGUSR1);
	check("self", hits == 1 && last_sig == SIGUSR1,
	      "hits=%d sig=%d (want 1, %d)", (int)hits, (int)last_sig, SIGUSR1);
}

static void case_ordering(void)
{
	hits = 0;
	n_order = 0;
	install(SIGUSR1, h_count, 0);
	install(SIGUSR2, h_count, 0);
	raise(SIGUSR1);
	raise(SIGUSR2);
	raise(SIGUSR1);
	check("ordering", hits == 3 && n_order == 3 && order[0] == SIGUSR1 &&
			  order[1] == SIGUSR2 && order[2] == SIGUSR1,
	      "hits=%d seq=%d,%d,%d", (int)hits, (int)order[0], (int)order[1],
	      (int)order[2]);
}

static void case_mask(void)
{
	sigset_t block, old;
	int during;

	hits = 0;
	install(SIGUSR2, h_count, 0);
	sigemptyset(&block);
	sigaddset(&block, SIGUSR2);
	if (sigprocmask(SIG_BLOCK, &block, &old) != 0) {
		check("mask", 0, "sigprocmask: %s", strerror(errno));
		return;
	}
	raise(SIGUSR2);
	during = hits;			/* must still be 0: it is blocked */
	sigprocmask(SIG_SETMASK, &old, NULL);
	check("mask", during == 0 && hits == 1,
	      "during-block=%d after-unblock=%d (want 0, 1)", during, (int)hits);
}

static void case_siginfo(void)
{
	struct sigaction sa;

	hits = 0;
	got_code = 0;
	got_pid_ok = 0;
	memset(&sa, 0, sizeof(sa));
	sa.sa_sigaction = h_info;
	sa.sa_flags = SA_SIGINFO;
	sigemptyset(&sa.sa_mask);
	if (sigaction(SIGUSR1, &sa, NULL) != 0) {
		check("siginfo", 0, "sigaction: %s", strerror(errno));
		return;
	}
	raise(SIGUSR1);
	/*
	 * SI_USER from kill(2), SI_TKILL from the tgkill(2) that a threaded
	 * libc uses for raise(). Both are "a process sent this"; which one is
	 * the libc's business and not the point of the case.
	 */
	check("siginfo", hits == 1 && last_sig == SIGUSR1 &&
			 (got_code == SI_USER || got_code == SI_TKILL) &&
			 got_pid_ok,
	      "hits=%d code=%d pid-matches=%d", (int)hits, (int)got_code,
	      (int)got_pid_ok);
}

static void case_nested(void)
{
	hits = 0;
	nested_depth = 0;
	nested_seen = -1;
	install(SIGUSR1, h_nested, 0);
	install(SIGUSR2, h_nested, 0);
	raise(SIGUSR1);
	check("nested", hits == 2 && nested_seen == 1 && nested_depth == 0,
	      "hits=%d depth-at-inner=%d depth-after=%d (want 2, 1, 0)",
	      (int)hits, (int)nested_seen, (int)nested_depth);
}

/*
 * A signal arriving while the guest is asleep inside a syscall that is running
 * on the other machine. With SA_RESTART the read must come back with the bytes,
 * not EINTR and not short.
 */
static void case_restart(void)
{
	int fd[2];
	pid_t kid;
	char buf[8];
	ssize_t n;
	int st = 0;

	hits = 0;
	if (install(SIGUSR1, h_nop, SA_RESTART) != 0 || pipe(fd) != 0) {
		check("restart", 0, "setup: %s", strerror(errno));
		return;
	}
	kid = fork();
	if (kid < 0) {
		check("restart", 0, "fork: %s", strerror(errno));
		return;
	}
	if (kid == 0) {
		/*
		 * Signal the parent while it is certainly blocked, then give it
		 * the bytes. usleep is itself forwarded, which is fine -- what
		 * matters is only that the kill lands before the write.
		 */
		close(fd[0]);
		usleep(200000);
		kill(getppid(), SIGUSR1);
		usleep(100000);
		if (write(fd[1], "abcdefg", 7) != 7)
			_exit(1);
		close(fd[1]);
		_exit(0);
	}
	close(fd[1]);
	memset(buf, 0, sizeof(buf));
	n = read(fd[0], buf, 7);
	close(fd[0]);
	waitpid(kid, &st, 0);
	check("restart", n == 7 && memcmp(buf, "abcdefg", 7) == 0 && hits == 1,
	      "read=%zd errno=%d buf=%.7s hits=%d (want 7, abcdefg, 1)",
	      n, n < 0 ? errno : 0, buf, (int)hits);
}

/* A signal from another process, which is the only case the guest's machine
 * cannot possibly have observed. */
static void case_child(void)
{
	pid_t kid;
	int st = 0, waited = 0;

	hits = 0;
	last_sig = 0;
	install(SIGUSR2, h_count, 0);
	kid = fork();
	if (kid < 0) {
		check("child", 0, "fork: %s", strerror(errno));
		return;
	}
	if (kid == 0) {
		usleep(150000);
		kill(getppid(), SIGUSR2);
		_exit(0);
	}
	/* Sleep in a loop rather than once: the point is to be interrupted. */
	while (!hits && waited < 100) {
		usleep(50000);
		waited++;
	}
	waitpid(kid, &st, 0);
	check("child", hits == 1 && last_sig == SIGUSR2,
	      "hits=%d sig=%d after %d waits (want 1, %d)",
	      (int)hits, (int)last_sig, waited, SIGUSR2);
}

int main(void)
{
	case_self();
	case_ordering();
	case_mask();
	case_siginfo();
	case_nested();
	case_restart();
	case_child();

	say(fails == 0 ? "PASS\n" : "FAIL\n");
	return fails == 0 ? 0 : 1;
}
