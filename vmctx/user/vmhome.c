// SPDX-License-Identifier: GPL-2.0
/*
 * vmhome — the "local host" half of remote execution (avm.md objective 5).
 *
 *   vmhome [port]            (default 9999)
 *
 * Runs on the machine you are sitting at, and owns everything about the
 * program except where its instructions execute. It loads the program, holds
 * its address space, its descriptors, its credentials and its identity, and
 * performs every one of its syscalls; the other machine supplies CPU and
 * decides nothing.
 *
 * ONE task holds the program, and every forwarded syscall is made BY that
 * task.
 *
 * That is the whole architecture and it is worth saying why. The program is
 * loaded here by this kernel — fork, PTRACE_TRACEME, execv, run the interpreter
 * to the program's entry point — and the task that comes out of that holds a
 * real address space with the program in it. VMCTX_CTL_ADOPT then turns that
 * task into a *service context*: it holds the address space and never executes
 * another instruction of its own, parking in the kernel on its way back to user
 * mode. VMCTX_CTL_SYSCALL makes it perform a syscall, in its own address space,
 * with its own descriptors, credentials and identity.
 *
 * So a pointer argument needs no marshalling at all. It already means what the
 * program meant by it, because the task making the call is the task that owns
 * the memory the pointer points into. There is no mirror to keep in step, no
 * userfaultfd, no page channel, no write-back and no question of whether an
 * access is permitted — the mappings are the program's own, so an access the
 * program may not make simply fails the syscall or arrives as an exception.
 *
 * A guest that forks needs a second service context, and gets one by forking
 * this one: the child of a task that holds the program holds the program too,
 * with exactly the descriptors and credentials a fork should inherit, and the
 * kernel hands it the same monitor. A guest thread is the same call with
 * CLONE_THREAD, so the futex it waits on is the same word its siblings wake.
 *
 * Syscalls are issued by number rather than through libc wrappers, so the bytes
 * the guest is handed back (struct stat, linux_dirent64, utsname) are exactly
 * the kernel ABI it expects, with no libc-layout mismatch.
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdarg.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <fcntl.h>
#include <signal.h>
#include <sys/syscall.h>
#include <time.h>
#include <execinfo.h>
#include <dirent.h>
#include <limits.h>
#include <linux/futex.h>
#include <sys/socket.h>
#include <sys/uio.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>

#include <sys/ptrace.h>
#include <sys/user.h>
#include <sys/wait.h>
#include <sys/personality.h>
#include <sys/prctl.h>
#include <sys/utsname.h>	/* hide_vdso compares kernel releases */
#include <sys/resource.h>
#include <sys/stat.h>

#include <pthread.h>
#include <sched.h>
#include <sys/mman.h>
#include <sys/ipc.h>
#include <sys/shm.h>

#include <stddef.h>	/* offsetof, for the signal frame */
#include "vmrproto.h"
#include "own.h"		/* shared-memory ownership channel (REDESIGN.md session 34); additive, not yet load-bearing */
#include "deadline.h"	/* vmctx_deadline_enforce: the nested page budgets */

/* See vmremote.c: the build this binary came from, printed, not inferred. */
#ifndef VMCTX_SRC_ID
#define VMCTX_SRC_ID "unknown"
#endif

#define MAXDATA (256 * 1024)

/*
 * Spin before blocking on a socket read, where the answer usually arrives in
 * microseconds.
 *
 * Every message on every channel here is answered by a thread that was asleep
 * in read(2): the connection thread waiting for the next forwarded call, the
 * fault path waiting for a page. Between the kernel's own spin-before-sleep at
 * its four vmctx waits (vmctx_spin_us) and this, a loopback call has no
 * sleep-and-wake left in it at all -- measured on the getppid loop, one boot:
 * the two socket wakeups were the last 5-10 us each of a ~25 us call.
 *
 * Adaptive, per thread (every socket here is served by one thread): a wait
 * that ended within the budget arms the next spin, one that outlived it
 * disarms it, so a channel across a LAN (250 us and up) spins once, measures
 * that, and sleeps from then on until a short wait arms it again. A recv with
 * MSG_DONTWAIT is the probe; a descriptor that is not a socket answers ENOTSOCK
 * and takes the plain read below. VMCTX_SOCK_SPIN_US sets the budget; 0
 * disables it (the control arm).
 */
static unsigned sock_spin_us = 50;
static __thread int sock_spin_armed = 1;
static unsigned long n_sock_spin_hit, n_sock_spin_miss;

static uint64_t spin_now_us(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (uint64_t)ts.tv_sec * 1000000ull + (uint64_t)ts.tv_nsec / 1000;
}

static int read_all(int fd, void *buf, size_t n)
{
	char *p = buf;
	uint64_t t0 = 0;

	if (sock_spin_us) {
		t0 = spin_now_us();
		if (sock_spin_armed) {
			for (;;) {
				ssize_t r = recv(fd, p, n, MSG_DONTWAIT);

				if (r > 0) {
					n_sock_spin_hit++;
					p += r;
					n -= (size_t)r;
					break;
				}
				if (r == 0)
					return 0;      /* peer closed */
				if (errno != EAGAIN && errno != EWOULDBLOCK &&
				    errno != EINTR) {
					if (errno == ENOTSOCK)
						break;	/* a plain read below */
					return -1;
				}
				if (spin_now_us() - t0 > sock_spin_us) {
					n_sock_spin_miss++;
					break;
				}
				__builtin_ia32_pause();
			}
		}
	}
	while (n) {
		ssize_t r = read(fd, p, n);

		if (r == 0)
			return 0;      /* peer closed */
		if (r < 0) {
			if (errno == EINTR)
				continue;
			return -1;
		}
		p += r;
		n -= (size_t)r;
	}
	if (sock_spin_us)
		sock_spin_armed = spin_now_us() - t0 <= sock_spin_us;
	return 1;
}

/* Two buffers, one write (a reply and its payload); loops on a short write. */
static int writev_all(int fd, const void *a, size_t na, const void *b, size_t nb)
{
	struct iovec iov[2] = { { (void *)a, na }, { (void *)b, nb } };
	int i = 0, cnt = nb ? 2 : 1;

	while (i < cnt) {
		ssize_t w = writev(fd, iov + i, cnt - i);

		if (w <= 0) {
			if (w < 0 && errno == EINTR)
				continue;
			return -1;
		}
		while (i < cnt && (size_t)w >= iov[i].iov_len) {
			w -= (ssize_t)iov[i].iov_len;
			i++;
		}
		if (i < cnt) {
			iov[i].iov_base = (char *)iov[i].iov_base + w;
			iov[i].iov_len -= (size_t)w;
		}
	}
	return 0;
}

static int write_all(int fd, const void *buf, size_t n)
{
	const char *p = buf;

	while (n) {
		ssize_t w = write(fd, p, n);

		if (w <= 0) {
			if (w < 0 && errno == EINTR)
				continue;
			return -1;
		}
		p += w;
		n -= (size_t)w;
	}
	return 0;
}
static const char *sysname(uint64_t nr)
{
	switch (nr) {
	case VMR_NR_read:       return "read";
	case VMR_NR_write:      return "write";
	case VMR_NR_close:      return "close";
	case VMR_NR_fstat:      return "fstat";
	case VMR_NR_lseek:      return "lseek";
	case VMR_NR_pread64:    return "pread64";
	case VMR_NR_writev:     return "writev";
	case VMR_NR_uname:      return "uname";
	case VMR_NR_getcwd:     return "getcwd";
	case VMR_NR_getdents64: return "getdents64";
	case VMR_NR_open:       return "open";
	case VMR_NR_stat:       return "stat";
	case VMR_NR_lstat:      return "lstat";
	case VMR_NR_sendfile:   return "sendfile";
	case VMR_NR_openat:     return "openat";
	case VMR_NR_newfstatat: return "newfstatat";
	case VMR_OP_START:      return "OP_start";
	case VMR_OP_LAYOUT:     return "OP_layout";
	case VMR_OP_PAGE:       return "OP_page";
	case VMR_OP_FINISH:     return "OP_finish";
	case VMR_OP_CHILD:      return "OP_child";
	default:                return NULL;	/* caller prints the number */
	}
}

/*
 * Which argument of a syscall is a path, if any. A log of numbers alone cannot
 * answer "what file did it look for", which is most of what a forwarded
 * syscall log is read for.
 */
static int path_arg(uint64_t nr)
{
	switch (nr) {
	case 2: case 4: case 6: case 21: case 59: case 87: case 89:
		return 0;	/* open, stat, lstat, access, execve, unlink,
				 * readlink */
	case 257: case 262: case 267: case 268:
		return 1;	/* openat, newfstatat, readlinkat, fchmodat */
	default:
		return -1;
	}
}


/* Always identify the call: an unnamed "?" in a log costs a debugging round. */
static const char *sysdesc(uint64_t nr)
{
	static __thread char buf[32];
	const char *n = sysname(nr);

	if (n)
		return n;
	snprintf(buf, sizeof(buf), "sys_%llu", (unsigned long long)nr);
	return buf;
}

/*
 * ---------------------------------------------------------------------------
 * vmctx_ctl(2): the four things a monitor needs of a service context.
 *
 * ADOPT turns the task that holds the loaded program into one. SYSCALL makes it
 * perform a call in its own address space. PEEK and POKE read and write that
 * address space, a page at a time, for the two things a syscall cannot express:
 * answering the other machine's request for a page, and pushing a signal frame
 * onto the program's stack.
 * ---------------------------------------------------------------------------
 */
#define VMCTX_CTL_ATTACH	1
#define VMCTX_CTL_WAIT		3
#define VMCTX_CTL_RESUME	4
#define VMCTX_CTL_GETREGS	5
#define VMCTX_CTL_SETREGS	6
#define VMCTX_CTL_PEEK		7
#define VMCTX_CTL_POKE		8
#define VMCTX_CTL_TAKE		9
/*
 * 10 and 13 were missing from this list, and the gap was not harmless.
 *
 * These constants are a hand-copy of include/uapi/linux/vmctx.h rather than an
 * include of it, so the kernel can grow an operation and this file never hears
 * about it. PROTECT is the one that mattered: the kernel has been able to take
 * write permission away from a single page -- PTE only, VMA untouched, so the
 * program cannot observe it -- since it was written, and its whole purpose is
 * "a write then arrives as a protection fault". A search of this file for it
 * found nothing and the obvious reading was that the owner had decided not to
 * protect its copies. It had not decided anything; it could not say the word.
 *
 * Added with the numbers the kernel actually uses. The drift is the defect --
 * see kernel-patches-are-the-only-record -- and the copy stays only because a
 * static binary that includes a kernel uapi header picks up much else with it.
 */
#define VMCTX_CTL_PROTECT	10
#define VMCTX_CTL_SYSCALL	11
/*
 * Drain the adopted mm's change log: ranges the kernel's mm-mutation hook
 * (mmu notifier) saw VACATED, whatever vacated them. The owner's ruling:
 * propagation follows the mm being touched, never a syscall-number list.
 */
#define VMCTX_CTL_MMLOG	21
#define VMCTX_MMLOG_MAX 16
/*
 * ent[].start bit 0: 0 = the mapping is GONE (unmap); 1 = the mapping LIVES
 * but its content was DISCARDED (MADV_DONTNEED) -- ownership resets, the
 * mapping's own records stay, and the destination drops pages, not mappings.
 */
#define VMCTX_MMLOG_DISCARD 1
struct vmctx_mmlog {
	uint32_t max;		/* in: capacity of ent[] (<= VMCTX_MMLOG_MAX) */
	uint32_t n;		/* out: entries drained                       */
	uint64_t dropped;	/* out: hook events lost to ring overflow
				 * since the last drain (reset on read)       */
	uint64_t cur;		/* out: the mm's event counter at this drain --
				 * the stamp this reply's constructions carry.
				 * Kernel #168: the hook stamps each event when
				 * it logs it, because a stamp assigned at
				 * drain time in userspace cannot order a
				 * recycled range's stale vacate against the
				 * mapping that superseded it.                */
	struct { uint64_t start, end, seq; } ent[VMCTX_MMLOG_MAX];
};
#define VMCTX_CTL_ADOPT		12
#define VMCTX_CTL_TAKEOBJ	13
#define VMCTX_CTL_GET_CLEAR_TID	14	/* the context's clear_child_tid word */
#define VMCTX_CTL_FUTEXWAKE	19	/* wake (mm, addr) waiters; needs no context */
#define VMCTX_CTL_TRYFAULT	20	/* would the mm's own kernel handle an
					 * access at addr? len bit 1 = write.
					 * 0 = yes (the address is real; serve
					 * it), -EACCES = mapping forbids it,
					 * -EFAULT = no mapping reaches it.
					 * The kernel is the only authority on
					 * address validity; maps text is not. */

/*
 * THE PAGE RECORD (kernel #154). Where each page of an adopted address space
 * is, kept by the kernel beside the page tables -- see the uapi header's
 * VMCTX_PG_* comment. This side keeps no table of its own any more: every
 * transition is one of these operations, made under the mm's lock together
 * with the page-table action it describes, and the fault hook claims a page
 * before the monitor is told about it.
 */
#define VMCTX_PG_NONE		0
#define VMCTX_PG_HOME		1
#define VMCTX_PG_REMOTE		2
#define VMCTX_PG_CLAIM		3
#define VMCTX_PG_TRANSIT	4
#define VMCTX_CTL_SERVE		22
#define VMCTX_SERVE_PROBE	(1u << 0)
#define VMCTX_SERVE_TAKEN	1
#define VMCTX_SERVE_COPIED	2
#define VMCTX_SERVE_ABSENT	3
#define VMCTX_SERVE_CLAIMING	4
#define VMCTX_SERVE_DENIED	5
#define VMCTX_SERVE_NOMAP	6
#define VMCTX_PGC_PRESENT	(1u << 4)
#define VMCTX_PGC_RO		(1u << 5)
struct vmctx_serve {
	uint64_t addr, buf;
	uint32_t flags, status, class, state, gen, sum;
};
#define VMCTX_CTL_LAND		23
#define VMCTX_LAND_IF_ABSENT	(1u << 0)
struct vmctx_land { uint64_t addr, buf; uint32_t gen, flags; };
#define VMCTX_CTL_PGACK		24
struct vmctx_pgack { uint64_t addr; uint32_t sum, _pad; };
#define VMCTX_CTL_PGSCAN	25
#define VMCTX_PGSCAN_MAX	64
struct vmctx_pgscan {
	uint64_t start, end;
	uint32_t mask, max, n, _pad;
	uint64_t page[VMCTX_PGSCAN_MAX];
};
#define VMCTX_CTL_PGSTATE	26
#define VMCTX_CTL_PGSET		27
struct vmctx_pgset { uint64_t addr; uint32_t state, gen; };
/*
 * The HOST-namespace pid of the target's last clone child (u64 out). The
 * assisted clone's return value is namespace-relative -- inside a sandbox's
 * fresh pid namespace it names a different task than every ctl and kill
 * issued from here, which address the init namespace. 0 = none recorded;
 * EINVAL = kernel predates the op (same-namespace behavior, fall back to
 * the clone's return value).
 */
#define VMCTX_CTL_LASTCHILD	28

#define VMCTX_REG_RAX		(1u << 0)

/* Events a service context raises, and the two answers it can be given. */
#define VMCTX_EV_FAULT		2
#define VMCTX_ACT_SELF		0	/* the local kernel supplies the page */
#define VMCTX_ACT_DONE		1	/* the monitor has supplied it        */
#define VMCTX_ACT_KILL		2	/* nobody can: end the context        */

/*
 * FAULT events only, args[2]: the class of the mapping the fault was taken
 * in, filled by the kernel's mm hook -- the one place the VMA is in hand.
 * Zero (no VALID bit) from an older kernel or a backend-reported fault, and
 * everything below then behaves exactly as it did before the bits existed.
 * See ctx_monitor(): the class is what lets a fault in a private file mapping
 * be answered from the file here when this side never handed the page over,
 * and pulled from the guest's machine when it did.
 */
#define VMCTX_PGC_VALID		(1u << 0)
#define VMCTX_PGC_FILE		(1u << 1)
#define VMCTX_PGC_SHARED	(1u << 2)
#define VMCTX_PGC_WRITE		(1u << 3)

/* Identical in layout and order to struct vmr_uregs; see vmrproto.h. */
struct vmctx_uregs {
	uint64_t rax, rbx, rcx, rdx, rsi, rdi, rbp, rsp;
	uint64_t r8, r9, r10, r11, r12, r13, r14, r15;
	uint64_t rip, rflags, orig_rax;
	uint64_t fs_base, gs_base;
};

struct vmctx_syscall {
	uint64_t nr;
	uint64_t args[6];
	int64_t  ret;
	uint32_t changed;
	uint32_t _pad;
	struct vmctx_uregs regs;
};

struct vmctx_mem {
	uint64_t addr;
	uint64_t len;
	uint64_t buf;
};

struct vmctx_event {
	uint32_t type;
	int32_t  pid;
	uint64_t nr;		/* FAULT: the exception vector             */
	uint64_t args[6];
	uint64_t fault_addr;
	uint64_t fault_err;
	uint64_t rip, rsp, rflags;
};

struct vmctx_reply {
	uint32_t action;
	uint32_t _pad;
	uint64_t retval;
	uint32_t map_op;
	uint32_t map_prot;
	uint64_t map_addr;
	uint64_t map_len;
	uint64_t map_off;
};

/*
 * Which number vmctx_ctl(2) is on the kernel this is running against.
 *
 * It is not the same on both kernels in this lab — 471 on 6.18.35, 473 on
 * 7.0.14, where 471 is rseq_slice_yield — and vmhome is built by one Makefile
 * for both. A wrong number here is not a build error: it is a call into an
 * unrelated syscall that returns something plausible, so it is settled at run
 * time against the kernel that will actually answer. 473 is tried first
 * precisely because 471 exists on both.
 */
static int vmctx_ctl_nr;

static void ctl_resolve(void)
{
	/* The probe below is the mechanism; the VMHOME_CTL_NR override it
	 * once carried was never set anywhere and is gone. A THIRD kernel's
	 * numbers belong in the probe, not in an env var. */
	/*
	 * -1 is never a pid, so a real vmctx_ctl answers ESRCH and anything
	 * else answers something that is not ESRCH.
	 */
	errno = 0;
	if (syscall(473, -1, VMCTX_CTL_ATTACH, NULL) < 0 && errno == ESRCH)
		vmctx_ctl_nr = 473;
	else
		vmctx_ctl_nr = 471;
	fprintf(stderr, "[vmhome] vmctx_ctl is syscall %d on this kernel\n",
		vmctx_ctl_nr);
	fprintf(stderr, "[vmhome] build %s\n", VMCTX_SRC_ID);
}

/*
 * The instrument for the one question the page log cannot answer.
 *
 * A page the guest was given by a TAKE comes back to this side present and
 * writable with no fault the monitor saw, no GET, and no change to the
 * ownership record -- measured, and it is what leaves both machines holding one
 * page. The page log says the page is here; it cannot say who put it here,
 * because every message it prints is written by a path that already knows about
 * coherence, and whatever is doing this does not.
 *
 * So watch the two ways a page can arrive on this side without faulting in the
 * context: an ioctl of this process's own (below -- PEEK, POKE and the takes all
 * reach the page through get_user_pages, and a POKE's FOLL_WRITE installs one),
 * and everything else (pgwatch_thread, which samples fast enough to bracket an
 * arrival between two syscalls of the trace). Between them, an arrival is either
 * attributed to a caller here or proved to have come from somewhere else.
 *
 * Diagnostic only: both are off unless VMHOME_PGLOG names a page, and the
 * sampler additionally needs VMHOME_PGWATCH.
 */
static void ctl_watch(pid_t pid, unsigned int cmd, const void *arg, int done,
		      long ret);

/*
 * INSTRUMENT (session 40): what the kernel says a service task IS at the
 * moment a ctl on it fails -- its state letter from /proc/<pid>/stat and
 * where it sleeps (wchan). "alive" by kill(pid, 0) is too coarse: it is
 * true for a task in do_exit that has already lost its vmctx and for a
 * zombie, and the difference between those is the whole diagnosis.
 */
static const char *task_photo(pid_t pid, char *buf, size_t len)
{
	char path[64], stat[512], wchan[64] = "?";
	char st = '?';
	FILE *f;

	snprintf(path, sizeof(path), "/proc/%d/stat", (int)pid);
	f = fopen(path, "r");
	if (f) {
		if (fgets(stat, sizeof(stat), f)) {
			char *rp = strrchr(stat, ')');

			if (rp && rp[1] == ' ')
				st = rp[2];
		}
		fclose(f);
	}
	snprintf(path, sizeof(path), "/proc/%d/wchan", (int)pid);
	f = fopen(path, "r");
	if (f) {
		if (!fgets(wchan, sizeof(wchan), f))
			snprintf(wchan, sizeof(wchan), "?");
		fclose(f);
	}
	{
		char pnd[64] = "", shd[64] = "", line[160];

		snprintf(path, sizeof(path), "/proc/%d/status", (int)pid);
		f = fopen(path, "r");
		if (f) {
			while (fgets(line, sizeof(line), f)) {
				if (!strncmp(line, "SigPnd:", 7))
					sscanf(line + 7, " %63s", pnd);
				else if (!strncmp(line, "ShdPnd:", 7))
					sscanf(line + 7, " %63s", shd);
			}
			fclose(f);
		}
		snprintf(buf, len, "state=%c wchan=%s sigpnd=%s shdpnd=%s", st,
			 wchan, pnd, shd);
	}
	return buf;
}

static long ctl(pid_t pid, unsigned int cmd, void *arg)
{
	long r;

	ctl_watch(pid, cmd, arg, 0, 0);
	r = syscall(vmctx_ctl_nr, pid, cmd, arg);
	ctl_watch(pid, cmd, arg, 1, r);
	return r;
}

/*
 * The context performs the call. Returns 0 and fills *rq, or -1 with errno set
 * if the context could not be asked at all.
 */
/*
 * Which contexts are inside an assisted syscall right now, for the fault
 * service's readahead (see prefetch_after()): a page a forwarded read(2) or
 * write(2) faults in is one of a run the call is about to touch in order,
 * and the next ones can be on their way while the call copies this one. Set
 * by the conn thread around the call, read by the fault-service thread.
 * Keyed by pid; a slot is reused when its context is gone.
 */
#define INCALL_SLOTS 512
static struct { pid_t pid; int in; } incall_tab[INCALL_SLOTS];
static pthread_mutex_t incall_lock = PTHREAD_MUTEX_INITIALIZER;

static void incall_set(pid_t pid, int in)
{
	int i, free_i = -1;

	pthread_mutex_lock(&incall_lock);
	for (i = 0; i < INCALL_SLOTS; i++) {
		if (incall_tab[i].pid == pid) {
			incall_tab[i].in = in;
			pthread_mutex_unlock(&incall_lock);
			return;
		}
		if (free_i < 0 && (!incall_tab[i].pid ||
				   (!incall_tab[i].in &&
				    kill(incall_tab[i].pid, 0) != 0)))
			free_i = i;
	}
	if (free_i >= 0) {
		incall_tab[free_i].pid = pid;
		incall_tab[free_i].in = in;
	}
	pthread_mutex_unlock(&incall_lock);
}

static int incall_get(pid_t pid)
{
	int i, in = 0;

	pthread_mutex_lock(&incall_lock);
	for (i = 0; i < INCALL_SLOTS; i++)
		if (incall_tab[i].pid == pid) {
			in = incall_tab[i].in;
			break;
		}
	pthread_mutex_unlock(&incall_lock);
	return in;
}

static int ctx_call(pid_t pid, struct vmctx_syscall *rq)
{
	int r;

	incall_set(pid, 1);
	r = ctl(pid, VMCTX_CTL_SYSCALL, rq) == 0 ? 0 : -1;
	incall_set(pid, 0);
	return r;
}

/*
 * The clear-tid word the context's clone recorded (CLONE_CHILD_CLEARTID), read
 * from the kernel that ran the real clone -- not parsed out of clone_args here.
 * Zero if the context named none. Used at teardown to clear it coherently and
 * wake a joiner; see the thread branch of the connection's exit path.
 */
/* Thread teardowns whose clear-tid word was, and was not, actually cleared. */
static unsigned long n_ctid_ok, n_ctid_bad;
static unsigned long n_ctid_unverified;	/* cleared; the page left with the joiner before the verify could read it */
/* unanswerable requests, split by whether the address space really had ended */
static unsigned long n_ctxpage_gone_ended, n_ctxpage_gone_transient;
/* requests answered "this address space has ended; stop" */
static unsigned long n_ctxpage_gone_told;
static unsigned long n_ctid_via_sibling, n_ctid_wake_sibling;
static unsigned long n_ctid_wake_late;	/* wakes that only landed on a re-arm (ordering race) */
/*
 * Pulls for a write whose named context could not take the install: the pulled
 * bytes are the only copy in existence at that instant, so they go in through a
 * live sibling of the same mm (n_pull_via_sibling) rather than being dropped.
 * n_pull_lost counts the ones nobody could take -- each is a page whose only
 * copy was discarded, recoverable afterwards only from the guest's machine's
 * retained bytes.
 */
static unsigned long n_pull_via_sibling, n_pull_lost, n_pull_returned;
/*
 * Page-channel failures during a fault GET: recovered by a fresh connection
 * (n_chan_regained), or terminal with the record saying the page EXISTS
 * (n_chan_lost) -- each of the second kind used to become a silent local
 * zero-fill over live memory and is now a named ACT_KILL.
 */
static unsigned long n_chan_regained, n_chan_lost;
/*
 * Forwarded calls the context could not be asked to run at all, split by the
 * refusal: EBUSY is the assisted slot still executing an earlier call (the
 * instant refusal of the slot contract), anything else is a dying context.
 * Each becomes a fabricated -EIO to the guest -- the netsurf paint storm.
 */
static unsigned long n_ctxcall_fail, n_ctxcall_busy, n_ctxcall_dead;
static unsigned long n_ret_eio;	/* -EIO written by the kernel completion */
/*
 * Vacated ranges drained from the mm-mutation hook, pending shipment in the
 * next syscall replies of this connection (8 ride per reply). Per-thread:
 * each connection is served by one thread, ctx_forward runs on it.
 */
static __thread struct { uint64_t start, end, seq; } vacate_pend[64];
static __thread unsigned vacate_pend_n;
static __thread uint32_t vacate_ovf_pend;
/*
 * The mutation sequence (vmr_rsp.mmseq): the KERNEL's per-mm event counter
 * (kernel #168). Each drained range carries the seq the hook stamped AT THE
 * EVENT, and last_mmseq is the counter at this thread's drain (vmctx_mmlog
 * .cur) -- the stamp its reply's constructions carry. The previous form --
 * one userspace counter advanced per drained range and per reply -- ordered
 * events by WHO DRAINED THEM, and two connection threads draining
 * concurrently could stamp a stale vacate newer than the mmap that
 * superseded it: netsurf's font scan (munmap, re-mmap into the recycled
 * hole) died on exactly that, the fresh mapping punched and its first read
 * answered -14.
 */
static __thread uint64_t last_mmseq;

/*
 * ---------------------------------------------------------------------------
 * SELF-MAP VACATE SUPPRESSION, the source half of the netsurf exit-242 wall.
 *
 * An mmap MAP_FIXED (ld.so placing a segment into its own PROT_NONE
 * reservation, the every-run case) UNMAPS its target range and MAPS it, in
 * one syscall. The kernel's mm hook logs the unmap as a vacate; the mmap's
 * own reply reconstructs the same range. When the two ride ONE reply the
 * destination's vacate_keep already voids the self-vacate -- but the change
 * log is per mm and any thread's forwarded call drains it, so a concurrent
 * call C can drain the vacate and ship it on a reply that lands on the
 * destination AFTER the mmap's construction, whereupon it punches and unmaps
 * the live segment. Photographed twice (VACATE-LATE 0x7ffff048a000-
 * 0x7ffff049c000, then a guest write into it answered -14, exit 242 at ~8s).
 *
 * A sequence number assigned at DRAIN cannot order it: the self-vacate is
 * drained by C at a moment later than the mmap's own reply stamp, so it
 * outranks its own construction. The fact that actually orders them lives at
 * the source and is known BEFORE the syscall: a MAP_FIXED names its exact
 * target range in its arguments. So the range is recorded here before the
 * call runs (hence before the hook can fire and before any C can drain the
 * vacate), and every drain -- this call's or a concurrent one's -- drops the
 * portion of a vacate covered by a live record. The record is removed after
 * this call's own drain, so a genuine later munmap of the same range is not
 * suppressed. Deterministic, no kernel change, no timestamp: whoever drains
 * the self-vacate does so while the record is live, because a drain that ran
 * after this call's drain would have found nothing left to drain.
 * ---------------------------------------------------------------------------
 */
#define SRCCONS_MAX 128
static struct srccons_ent {
	int      live;
	uint32_t as;
	uint64_t start, end;
} srccons[SRCCONS_MAX];
static pthread_mutex_t srccons_lock = PTHREAD_MUTEX_INITIALIZER;
static unsigned long n_srccons_pushed, n_srccons_suppressed;

static int srccons_push(uint32_t as, uint64_t start, uint64_t end)
{
	int i, idx = -1;

	if (end <= start)
		return -1;
	pthread_mutex_lock(&srccons_lock);
	for (i = 0; i < SRCCONS_MAX; i++)
		if (!srccons[i].live) {
			srccons[i].live = 1;
			srccons[i].as = as;
			srccons[i].start = start;
			srccons[i].end = end;
			idx = i;
			n_srccons_pushed++;
			break;
		}
	pthread_mutex_unlock(&srccons_lock);
	return idx;
}

static void srccons_pop(int idx)
{
	if (idx < 0)
		return;
	pthread_mutex_lock(&srccons_lock);
	srccons[idx].live = 0;
	pthread_mutex_unlock(&srccons_lock);
}

/*
 * Emit a vacate [vs, ve) into vacate_pend, minus the portions covered by any
 * live self-map record of this address space. Every survivor carries seq --
 * the kernel's event-time stamp for this one event (a split range is still
 * one event; its pieces share the stamp).
 */
static void vacate_emit(uint32_t as, uint64_t vs, uint64_t ve, int soft,
			uint64_t seq)
{
	uint64_t cur = vs;

	while (cur < ve) {
		uint64_t cs = 0, ce = 0;
		int i, hit = 0;

		pthread_mutex_lock(&srccons_lock);
		for (i = 0; i < SRCCONS_MAX; i++) {
			if (!srccons[i].live || srccons[i].as != as)
				continue;
			if (srccons[i].end <= cur || srccons[i].start >= ve)
				continue;
			if (!hit || srccons[i].start < cs) {
				cs = srccons[i].start > cur ? srccons[i].start : cur;
				ce = srccons[i].end < ve ? srccons[i].end : ve;
				hit = 1;
			}
		}
		pthread_mutex_unlock(&srccons_lock);
		if (!hit) {
			cs = ve;
			ce = ve;
		}
		if (cs > cur && vacate_pend_n <
		    sizeof(vacate_pend) / sizeof(vacate_pend[0])) {
			vacate_pend[vacate_pend_n].start = cur | (soft ? 1 : 0);
			vacate_pend[vacate_pend_n].end = cs;
			vacate_pend[vacate_pend_n].seq = seq;
			vacate_pend_n++;
		} else if (cs > cur) {
			vacate_ovf_pend++;
		}
		if (hit)
			n_srccons_suppressed++;
		cur = ce > cur ? ce : ve;
	}
}
static unsigned long n_vacates_shipped;


static uint64_t ctx_clear_tid(pid_t pid)
{
	uint64_t word = 0;

	if (pid <= 0 || ctl(pid, VMCTX_CTL_GET_CLEAR_TID, &word) != 0)
		return 0;
	return word;
}

/* One syscall, result only. */
static long ctx_sys(pid_t pid, uint64_t nr, uint64_t a0, uint64_t a1,
		    uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5)
{
	struct vmctx_syscall rq;

	memset(&rq, 0, sizeof(rq));
	rq.nr = nr;
	rq.args[0] = a0; rq.args[1] = a1; rq.args[2] = a2;
	rq.args[3] = a3; rq.args[4] = a4; rq.args[5] = a5;
	if (ctx_call(pid, &rq) < 0)
		return -EIO;
	return (long)rq.ret;
}

/*
 * ---------------------------------------------------------------------------
 * The instrument: every movement of a page, and who holds it afterwards.
 *
 * The single-writer rule is a claim about two page tables on two machines, and
 * an argument about it is worth nothing. This logs each transfer with a
 * sequence number and reads the *source's* page table before and after, out of
 * /proc/<ctx>/pagemap, so a serve that leaves the page here — two holders — is
 * a line in the log and a counter at the end rather than a deduction.
 *
 * VMHOME_PGLOG=1 for every page; VMHOME_PGLOG=0xADDR for one.
 * ---------------------------------------------------------------------------
 */
static int      pglog_on;		/* 0 off, 1 all, 2 one page */
static uint64_t pglog_page;

/*
 * There used to be a family of switches here whose own comment described them
 * as selecting "behaviour known to be wrong", one per change, so that a
 * regression could be pinned on a change rather than argued about.
 *
 * They are gone, and the reason is that the second half of that sentence never
 * happened. A switch earns its place while a change is being proved; once the
 * change is proved the switch is a permanently compiled branch, on the hottest
 * paths in the system, whose only reachable purpose is to put a known defect
 * back. It also multiplies the configuration space: with N of them there are
 * 2^N ways to run this and exactly one of them is correct, which is how a
 * measurement gets taken in the wrong arm by accident -- and this tree has
 * three separate instances of exactly that, two switches assigned and never
 * read, and a hot-path guard left off by default on the strength of a 5x
 * slowdown that turned out to be 3.5%.
 *
 * What replaces them is not a flag but the discipline that was supposed to
 * accompany them: prove the change with an arm that exists WHILE it is being
 * proved, quote the numbers in the comment where the change lives, and then
 * delete the arm. The numbers stay; the branch does not.
 */
/*
 * VMHOME_SIGPROBE: read the context's registers back, from outside it.
 *
 * Not a switch but a second instrument, and the one that found what sig1 was
 * dying of. Everything else here sees the context only through what it
 * reports; this reads /proc/<ctx>/syscall and VMCTX_CTL_GETREGS after each
 * forwarded call and at each fault, so where the task actually stands can be
 * compared with what it said. That is how "the call answered rip=0x4067bc" and
 * "the context now stands at rip=0x402070", the program's own signal handler,
 * came to be visible at once -- a signal this side had delivered into a frame
 * nobody was ever told about.
 *
 * Since that is fixed the two should agree, so a line from this is now a
 * regression rather than a clue.
 */
static int sigprobe;
/*
 * A page the remote has EVER asked for is recorded PG_THEIRS ("belongs to
 * remote") even when this side answers ABSENT and tells the remote to make its
 * own zero page. That is the authoritative state: a page the source has never
 * transmitted is uninitialised (the remote may zero it once); a page it has
 * transmitted -- or told the remote to create -- belongs to the remote and must
 * never be re-zeroed. Recording PG_NONE on ABSENT (the old default) let the
 * source re-tell the remote to zero a page it already owned, which the
 * destination then fabricated over live memory.
 *
 * The next ask for such a page is answered ABSENT again (the destination's own
 * object, retained copy, or in-flight landing then decides what it holds).
 * VMR_CTXPAGE_NOTHOLDER exists for exactly this answer and the destination
 * handles it, but this side has not sent it since the THEIRS recording landed;
 * see the note at the serve's GONE branch in ctx_page_inner().
 */
/*
 * The "invalid ELF header" two-holder failure, and three wrong answers to it.
 *
 * The symptom: firefox's loader reads every library's header into one reused
 * stack buffer, and for one library the guest validated a previous library's
 * bytes. The state behind it was real -- both machines holding one page, this
 * side writing it, the guest reading its own copy -- and was measured directly
 * (the page log shows "watched page present here pre=1 post=1" across the read,
 * 61 times in one run, and the ownership sweep reports the page as handed over
 * and present here at the same time).
 *
 * What the state was NOT, in three attempts, each killed by a measurement:
 *
 *   "This side kept a writable copy when it served the guest one, so make the
 *   serve write-protect it." The page is not copy-served. It is TAKE-served --
 *   "SERVE take ok pre=1 post=0", 150 times in the failing run -- so at the
 *   serve there is no copy here to protect.
 *
 *   "A forwarded call wrote a buffer this side happened to hold, so find the
 *   written extent and tell the guest to drop its copy." Wrong twice over. The
 *   extent has to be decoded from each syscall's arguments, which is file-shaped
 *   thinking about a memory problem and reaches exactly as far as the table --
 *   readv, recvmsg, an ioctl filling a struct and a device writing by DMA all
 *   have the same exposure. And "drop your copy" is the wrong operation: the
 *   destination's log says the guest had WRITTEN that page (site5w then site3w),
 *   so a bare invalidate would have destroyed the guest's write. The only
 *   correct invalidation is the one the coherence rule already names, VMR_PG_GET,
 *   which moves the bytes back before unmapping them there.
 *
 *   "The movable set truncates at 256 ranges, so the stack never reaches the
 *   take." A real latent defect -- /proc/maps is address-ordered, so the cut
 *   always drops the highest mappings and firefox reaches 482 -- but not this:
 *   the page is in the movable set and is taken, 150 times.
 *
 * What it actually was is in ctx_peek(): a read of the program's memory that
 * spilled past the page its caller had checked and INVENTED the next one, while
 * the record went on saying the guest held it. Not a coherence mechanism failing
 * to fire -- a page appearing on this side that the coherence machinery was
 * never told about, from a caller that only meant to read a string.
 */

/*
 * Whether a fault in a private file mapping whose page this side never handed
 * over is answered SELF -- the file here is the current copy, and the local
 * kernel serves it -- rather than fetched from the guest's machine. The fetch
 * is the wrong answer twice over for such a page: it costs a round trip for
 * bytes this side already has, and when the guest's machine answers ABSENT
 * (it has never seen the page either) the old path poked 4096 zeros over the
 * program's own file-backed constants.
 *
 * Decided AFTER the fetch, never before: the far machine's ABSENT is what
 * proves the file is current. Deciding it before -- serving the local file for
 * any page this side had not handed over -- served a stale copy of a writable
 * file page the guest had stored into, and cost sm2/sm3 the whole test.
 */
static unsigned long n_file_local;	/* answered from the file here        */
/*
 * RELRO ranges: read-only file ranges a forwarded mprotect sealed, the only
 * read-only file pages that can differ from the file. Declared here because
 * ctx_monitor() consults them; defined below with sh_file. See relro_note().
 */
struct sh_file_range;
static struct { uint64_t start, end; } relro[128];
static int relro_n;
static pthread_mutex_t relro_lock2 = PTHREAD_MUTEX_INITIALIZER;
static unsigned long n_relro_ranges, n_relro_asked, n_rofile_local;
static void relro_note(uint64_t addr, uint64_t len);
static int relro_has(uint64_t addr);
static void relro_forget(uint64_t addr, uint64_t len);
static unsigned long n_file_pulled;	/* handed over, pulled from the guest */
/*
 * A successful munmap or mmap clears this side's ownership record for the
 * pages of the range. An address a program unmaps and maps again is new
 * memory with an old record: a stale THEIRS there sends the fault service to
 * the guest's machine for a page it never saw, and 40 ABSENT answers later
 * the context is ended for a page that never existed.
 */
/*
 * Pages of a *movable* range served by the copy at the bottom of ctx_page()
 * rather than handed over, and how many of those were not here when it ran --
 * a peek that creates the page it reports. See the note at the call.
 */
/* Page requests that named a context which had already exited, and how many of
 * those a live sibling of the same address space could answer instead. */
static unsigned long n_ctxpage_gone, n_ctxpage_resited;
/* Requests whose context's /proc map had already gone, and how many a live
 * sibling of the same address space could answer instead. */
/* How often the movable set had to grow past its starting size. */
static unsigned long pg_seq;
static unsigned long n_take_busy;
/*
 * ...and what became of those. A page that is still here after the take is a
 * page this side would be a second holder of; see ctx_page(). Retaking usually
 * wins, and when it does not the page is not served at all.
 */
/* The record says the guest has it and this side has it too: two holders. */
/*
 * "Nothing served, and the address is mapped here" is answered ABSENT rather
 * than EFAULT: a mapped address is one the program may touch, and EFAULT there
 * killed programs whose page simply had not materialised on either side yet
 * (A VMA is not permission -- but it is also not a wall).
 */
static unsigned long n_serve_absent;	/* answered "this side does not hold it" */
static unsigned long n_serve_absent_fresh;	/* ...for a page nobody had ever held */
static unsigned long n_home_local;	/* faults the record answered locally (HOME) */
static unsigned long n_sibling_claim_waited;	/* faults that waited out a sibling's claim here */
/*
 * Serves whose take found nothing because a CONCURRENT serve of the same page
 * took it mid-flight (present at this serve's entry, gone at its take).
 * Answered CLAIMING, never ABSENT: the bytes are on the wire to the asker's
 * machine, and an ABSENT believed before that landing installs the retained
 * (stale) copy over the guest's live progress -- hx1's measured lost-store.
 */
/* GONE-record refusals intercepted because this process's own pull of the
 * page was in flight (infl[]): the THEIRS record is stale by microseconds
 * and ABSENT from it teaches the destination to use a copy it no longer
 * has. Answered CLAIMING. */
/* Refused-take restores SUPPRESSED because the record said a hand-over had
 * completed mid-refusal: the unmapped leftover was stale, and restoring it
 * minted a second, old copy the next take then served as genuine. */
/* The two-phase hand-over's own numbers: serves committed IN TRANSIT, acks
 * that settled one to THEIRS, acks ignored because the page had moved on
 * (sum mismatch or state no longer INTRANSIT -- a newer serve or a pull-back
 * overtook them), and serves refused CLAIMING because the page was in
 * transit to the asker's machine. */
static unsigned long n_intransit_committed, n_intransit_acked;
static unsigned long n_intransit_ack_stale, n_serve_intransit_refused;
static unsigned long n_vfork_as_fork;	/* vfork/CLONE_VFORK run as plain fork */
/*
 * A fault on a page this side's own record says it holds, answered ABSENT by
 * the other machine: neither side can produce it. Distinct from a fresh address
 * only since PG_NONE existed -- before it, an unseen page read OURS and the two
 * were the same number.
 */
/* Shared file pages pulled back before a forwarded call; sh_file_reclaim_held(). */
static unsigned long n_shfile_reclaimed;
/*
 * Why that number is what it is, which the number alone cannot say.
 *
 * ARCHITECTURE.md §9.1 derives -- from reading, not from measurement -- that
 * the reclaim is structurally empty: it pulls only pages whose ownership record
 * says THEIRS or CLAIM, and ctx_movable_ranges() excludes a MAP_SHARED file
 * range from the movable set, so ctx_page() never takes one of those pages and
 * never records it that way. A derivation is not evidence (PRINCIPLES §7), and
 * "0 pulled" is the same reading whether the loop never ran, ran over no
 * ranges, or ran over every page and found none in a state it acts on.
 *
 * So each step is counted apart, and all of them are printed at exit whatever
 * their values -- a counter shown only when non-zero cannot make the statement
 * "the loop ran over N pages and not one of them was ever handed over".
 */
static unsigned long n_shfile_ranges;	/* ranges sh_file_note() recorded      */
static unsigned long n_shfile_calls;	/* reclaim calls that had a range      */
static unsigned long n_shfile_pages;	/* pages the reclaim looked at         */
static unsigned long n_shfile_st[5];	/* ...by pg_state: OURS THEIRS CLAIM NONE
					 * INTRANSIT */
static unsigned long n_shfile_served;	/* CTXPAGE requests inside such a range */
static unsigned long n_shfile_nomov;	/* ...that ctx_movable_ranges() had no
					 * entry for, so no take was attempted
					 * and no hand-over could be recorded  */
static unsigned long n_serve_refused;	/* ...because it is handed over or claimed */
/*
 * Faults the guest's machine had nothing for, filled here with a page of this
 * side's own rather than left to do_anonymous_page(). Counted because the whole
 * argument for doing it is that the page must be an ordinary one the take can
 * move, and a zero here would mean the fill never ran.
 */
static unsigned long n_absent_filled;
/*
 * And the same faults split by what this side's own ownership record said the
 * page was, read before the claim is marked.
 *
 * ABSENT is only defensible for a page this side has never handed over: that is
 * memory the program has not touched, and its own kernel would have zero-filled
 * it. For a page recorded THEIRS this side handed the bytes to the other machine
 * and is now being told they are not there either -- so a page that existed a
 * moment ago exists nowhere, and filling it with zeros writes over the program's
 * own data (PRINCIPLES §3). The two are counted apart because the whole question
 * "is this zero-fill legitimate?" is that distinction and nothing else.
 */
static unsigned long n_absent_ours;	/* never handed over: fresh memory      */
static unsigned long n_absent_theirs;	/* handed over, and the other side says no */
/*
 * And what became of those. An ABSENT answer is a fact about the other machine,
 * not about the page: another thread of this side may be carrying the bytes
 * home at that very moment, and the two service contexts of a threaded guest
 * share one address space, so the page can appear here without this thread
 * doing anything at all. Counted apart because "how often was an empty page
 * about to be written over live memory" is the whole question.
 */
static unsigned long n_absent_averted;	/* ...and it landed: nothing invented   */
/* GET returned BYTES but a sibling's pull landed the page while the ask was in
 * flight: the arriving copy is the older one and is dropped. The source-side
 * half of "a landing beats arriving bytes" (the pg3 one-behind's producer:
 * the destination's retained copy, served after the INFLIGHT grace, poked over
 * the fresher landing at the join). */
static unsigned long n_pull_landed_raced;
/* ...and the arrivals whose stamp was NEWER than the landing's: the landing
 * was the stale copy, and the arriving bytes overwrote it. */
static unsigned long n_staletrail_asked;	/* VMR_OP_STALETRAIL received */
static unsigned long n_absent_regained;	/* a re-ask produced the bytes          */
static unsigned long n_absent_lost;	/* on neither machine: named, not filled */
/*
 * What a refusal costs, measured rather than assumed.
 *
 * A take that answers -EBUSY says "not yet". Whether that is survivable depends
 * on one fact and nothing else: is the page still here after the refusal? So
 * the pagemap is read before the take and again after the first refusal, and
 * the two are counted apart. n_take_ref_gone is the defect (AUDIT VIII.3) and
 * has to stay zero; the kernel counters it should agree with are
 * /sys/module/kernel/parameters/vmctx_take_{refused,lost,regained}.
 */
/*
 * A page that becomes present on this side without anyone supplying it.
 *
 * Every way this side is *meant* to acquire a page leaves a line: a fault
 * answered by ctx_monitor(), a pull, a poke. Measured on pg3, the run where
 * thread A's buffer stops changing hands, the last thing that happens to
 * 0x7fffffffe000 is a take that removes it ("take ok pre=1 post=0") and then,
 * on the very next forwarded call, "watched page present here pre=0 post=1" --
 * with no fault, no GET and no pull between the two, and both fault services
 * reporting 0 unanswered for the whole run. From that moment the guest never
 * faults on the page again and this side reads its own copy for ever.
 *
 * "No line" is not a measurement. These make it one: how often the watched page
 * goes from absent to present across a forwarded call, how many faults were
 * answered for it in that window, and what the page then holds -- because a
 * page the local kernel invented is zeros and a page that came back with its
 * previous contents is a different defect entirely.
 */
static unsigned long n_watch_faults;	/* faults answered for the watched page */
static unsigned long n_watch_appeared;	/* absent -> present across a call      */
static unsigned long n_watch_unasked;	/* ...with no fault answered in between  */
/* Read-only ranges: pages copied rather than taken, and the assertion. */
static unsigned long n_ro_served;
/* Where a served page fell: in the movable set, or outside it (copied). */
static unsigned long n_mov_hit, n_mov_miss;
/*
 * The fork copy-on-write record's own numbers. Declared here with the rest so
 * the exit report can print them whatever they are; a zero with a denominator
 * beside it is the statement wanted (PRINCIPLES §7).
 */
static unsigned long n_cow_child_told;	/* COWBREAK answered to a child's fault */
static unsigned long n_cow_parent_told;	/* ...to a parent about to write        */
static unsigned long n_cow_write_free;	/* parent writes with nothing owed      */
static unsigned long n_cow_pages_owed;	/* (child, page) pairs recorded broken  */
static unsigned long n_cow_write_ro;	/* write the program itself forbids     */
static unsigned long n_cow_kept_for_copy;/* pages given to a copy as they came back */
static unsigned long n_cow_self_cow;	/* inherited page absent everywhere:
					 * landed the parent's own current page
					 * read through its task (a fork copy
					 * is never owed zeros)               */

/* A take refused because the folio is a large one of a file; see the COW break. */
static unsigned long n_take_gaveup;

/*
 * The conserved-token serialiser (pgstate.h) was built here behind
 * VMHOME_PGTOKEN, measured, and removed. What it established stands: both
 * machines can enter PST_REQUESTING for one page at once (the table is
 * per-process, so a local mutex cannot arbitrate two machines), and the
 * kernel's 2s deadline then SIGBUSes a guest thread that may be holding a libc
 * lock -- the two-monitor deadlock photographed in dmesg, one page, opposite
 * directions. But turning the serve-side claim on regressed the suite 81/0 ->
 * 63/18 (sm2, sm3, an1, cow1, pg2, ex4, pg3, pf3 -- exactly the cases whose
 * pages change hands most), and on ws1 it traded 4 failures in 40 for 5 in 22:
 * the CLAIMING retry storm costs more than the deadlock it prevents. The
 * destination's half of the asymmetry (yield to the source, pgstate.h) is
 * unconditional in vmremote and stays; the ws1 hang itself was later
 * root-caused elsewhere (the join-word wake, then the guest fs_base race).
 */

/*
 * VMHOME_FUTEX_LOG=1: a peek and a line per forwarded futex call. An
 * instrument, so armed like the page log rather than paid on every call of
 * the hottest syscall a threaded program makes.
 */
static int futex_log;

static void pglog_init(void)
{
	const char *e = getenv("VMHOME_PGLOG");

	sigprobe    = getenv("VMHOME_SIGPROBE")    != NULL;
	futex_log   = getenv("VMHOME_FUTEX_LOG")   != NULL;
	if (!e || !*e)
		return;
	if (e[0] == '0' && (e[1] == 'x' || e[1] == 'X')) {
		pglog_page = strtoull(e, NULL, 16) & ~(uint64_t)4095;
		pglog_on = pglog_page ? 2 : 0;
	} else if (atoi(e)) {
		pglog_on = 1;
	}
}

/*
 * Present in the context's own page table, asked of the kernel rather than
 * inferred. Bit 63 of the pagemap entry; -1 if it cannot be read at all.
 *
 * Read-only, and it never faults anything in: /proc/pid/pagemap reports what is
 * mapped and creates nothing, which matters here because the whole failure this
 * measures is a page created by the act of looking at it.
 */
/*
 * Bumped on every successful exec. A /proc/<pid>/pagemap descriptor holds a
 * reference to the mm it was OPENED against, not to the pid: after an execve
 * the old address space stays alive behind the descriptor and keeps answering
 * -- so every cached presence test for the exec'd context reported the DEAD
 * generation's page tables as the truth (the destination has had
 * page_present_forget() on exec for the same reason since the beginning; this
 * side never did). The counter is global rather than per-pid because the
 * cache below is per-thread and an exec on one connection cannot reach the
 * other threads' descriptors; a rare spurious reopen on every thread is the
 * whole cost.
 */
static unsigned long mm_generation;

static int ctx_present(pid_t pid, uint64_t addr)
{
	/*
	 * The descriptor is kept, per thread and per context. This is asked
	 * once per page on the hand-over path, and an open() and a close() of
	 * /proc there is a syscall pair on the busiest step in the system.
	 */
	static __thread pid_t held_pid;
	static __thread int   held_fd = -1;
	static __thread unsigned long held_gen;
	char path[64];
	uint64_t ent = 0;
	ssize_t n;

	if (pid <= 0)
		return -1;
	if (held_fd >= 0 && (held_pid != pid ||
			     held_gen != __atomic_load_n(&mm_generation,
							 __ATOMIC_RELAXED))) {
		close(held_fd);
		held_fd = -1;
	}
	if (held_fd < 0) {
		snprintf(path, sizeof(path), "/proc/%d/pagemap", (int)pid);
		held_fd = open(path, O_RDONLY | O_CLOEXEC);
		if (held_fd < 0)
			return -1;
		held_pid = pid;
		held_gen = __atomic_load_n(&mm_generation, __ATOMIC_RELAXED);
	}
	n = pread(held_fd, &ent, sizeof(ent), (off_t)((addr / 4096) * 8));
	if (n != (ssize_t)sizeof(ent)) {
		close(held_fd);		/* the context may have gone */
		held_fd = -1;
		return -1;
	}
	return (ent >> 63) & 1;
}


/*
 * Microseconds on the monotonic clock, so every page-operation line carries
 * when it happened and the step lines can say how long they took. Wall time
 * answers neither question; ordering and duration are the two things a
 * page trace exists to establish.
 */
static uint64_t now_us(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (uint64_t)ts.tv_sec * 1000000ull + (uint64_t)ts.tv_nsec / 1000;
}

/*
 * Source-side high-water-mark stale-copy detector, the mirror of vmremote's
 * clobber (task 13). hx2's writer slots are monotonic, so their per-word maximum
 * ever seen is the truth; a page whose bytes this side is about to SERVE (send to
 * the other machine) or has just POKEd IN must never move a slot BELOW its mark,
 * because between atomic single-writer hand-offs the value can only rise. The
 * first BACKMOVE, by timestamp across both ends' logs (CLOCK_MONOTONIC is
 * system-wide, both ends on one box), names where the stale copy enters the
 * ping-pong. Gated by VMHOME_CLOBBER; zero cost off. Timestamped for cross-end
 * correlation with vmremote's BACKMOVE lines.
 */
#define SRC_HWM_N 256
static struct { uint64_t base; unsigned long hw[VMR_PG_SIZE / sizeof(long)]; }
	src_hwm[SRC_HWM_N];
static pthread_mutex_t src_hwm_lock = PTHREAD_MUTEX_INITIALIZER;
/*
 * How the watched page last came to be present in this side's task, so a
 * backward serve-out can name the arm that put it there rather than leaving it
 * to be guessed at. One page is watched, so one variable is enough.
 *   1 poked in from a GET that returned bytes
 *   2 ABSENT, and the page was already present here (nothing written)
 *   3 ABSENT, then regained on a re-ask
 *   4 ABSENT, fresh address, an empty page supplied here
 *   5 ABSENT, left to the local kernel (ACT_SELF)
 *   6 a read-only file page served locally without asking
 *   7 a private file page the guest's machine does not hold
 */
static int src_arrival;
static const char *src_arrival_name(int a)
{
	switch (a) {
	case 1: return "poked in from a GET";
	case 2: return "ABSENT, already present here";
	case 3: return "ABSENT, regained on a re-ask";
	case 4: return "ABSENT, empty page supplied here";
	case 5: return "ABSENT, left to the local kernel";
	case 6: return "read-only file, served locally";
	case 7: return "private file, served locally";
	case 8: return "bytes arrived, sibling's landing kept";
	}
	return "never recorded";
}
static unsigned long n_src_backmove[16];
static unsigned long n_src_backmove_worst;
static unsigned long n_src_clobber_calls[16];

static void src_clobber(uint64_t base, const void *buf, int site)
{
	const unsigned long *nw = (const unsigned long *)buf;
	int slot = -1, i, j;
	static uint64_t watch = 1;	/* 1 = uninit; 0 = off; else the page */

	if (watch == 1) {
		const char *e = getenv("VMHOME_CLOBBER");

		watch = e ? (strtoull(e, NULL, 16) & ~(uint64_t)4095) : 0;
	}
	if (!watch)
		return;
	base &= ~(uint64_t)4095;
	if (base != watch)		/* only the one page, no stack-noise false hits */
		return;
	/*
	 * The denominator. A BACKMOVE count of zero and "this never ran" are the
	 * same reading without it, and this tree has been fooled by that before.
	 */
	{
		unsigned long *calls = n_src_clobber_calls;

		if (++calls[site % 16] % 5000 == 1)
			fprintf(stderr, "[vmhome] CLOBBER-ARMED %s 0x%llx: "
				"call #%lu (instrument is running)\n",
				site == 1 ? "serve-out" : "poke-in",
				(unsigned long long)base, calls[site % 16]);
	}
	pthread_mutex_lock(&src_hwm_lock);
	for (i = 0; i < SRC_HWM_N; i++)
		if (src_hwm[i].base == base) { slot = i; break; }
	if (slot < 0)
		for (i = 0; i < SRC_HWM_N; i++)
			if (src_hwm[i].base == 0) {
				src_hwm[i].base = base;
				slot = i;
				break;
			}
	if (slot < 0) { pthread_mutex_unlock(&src_hwm_lock); return; }
	for (j = 0; j < (int)(VMR_PG_SIZE / sizeof(long)); j++) {
		unsigned long n = nw[j], hw = src_hwm[slot].hw[j];

		if (hw >= 1 && hw < (1UL << 32) && n < hw) {
			n_src_backmove[site % 16]++;
			if (hw - n > n_src_backmove_worst)
				n_src_backmove_worst = hw - n;
			if (n_src_backmove[site % 16] <= 8)
				fprintf(stderr, "[vmhome] BACKMOVE %s 0x%llx +%d: "
					"hwm %lu -> %lu (%lu behind); the page "
					"got here by: %s (abs=%llu)\n",
					site == 1 ? "serve-out" : "poke-in",
					(unsigned long long)base, j * 8, hw, n,
					hw - n, src_arrival_name(src_arrival),
					(unsigned long long)now_us());
		}
		if (n > hw && n < (1UL << 32))
			src_hwm[slot].hw[j] = n;
	}
	pthread_mutex_unlock(&src_hwm_lock);
}

/*
 * The value of ONE WORD of the watched page, as a string for the page trace.
 *
 * `sum=` says two transfers differ; it cannot say which word differed or what
 * it became, and for a lock word that is the whole question -- a futex hangs on
 * a specific 32-bit value, not on a page's contents. VMHOME_PGLOG_OFF gives a
 * byte offset within the watched page (0xc40 for _dl_stack_cache_lock, which
 * lives in the page at 0x4d5000), and every line that already prints a sum then
 * also prints that word -- so the trace carries the value's whole history
 * across hand-overs instead of a fingerprint of the page it sits in.
 *
 * Empty string when no offset is configured, so the lines are unchanged for
 * everyone who has not asked.
 */
static const char *pgword(const void *p)
{
	static __thread char buf[40];
	static int off = -2;

	if (off == -2) {
		const char *e = getenv("VMHOME_PGLOG_OFF");

		off = e ? (int)(strtoul(e, NULL, 0) & 0xffc) : -1;
	}
	if (off < 0 || !p)
		return "";
	snprintf(buf, sizeof(buf), " w[%03x]=%08x", off,
		 *(const unsigned int *)((const char *)p + off));
	return buf;
}

/*
 * A cheap content fingerprint for the page trace. Two transfers of "the same
 * page" with different sums are different bytes, and the first pair that
 * disagrees between the sender's line and the receiver's line is the guilty
 * step -- evidence, where first8 could not see past offset 0.
 */
static unsigned int page_sum(const void *p)
{
	const unsigned int *w = p;
	unsigned int a = 1, b = 0;
	int i;

	for (i = 0; i < 1024; i++) {
		a += w[i];
		b += a;
	}
	return (b << 16) ^ a ^ (b >> 16);
}

/*
 * ---------------------------------------------------------------------------
 * Which side owns a page: THE KERNEL'S RECORD (kernel #154).
 *
 * This side used to keep the answer in a table of its own (pgown_tab, a
 * 2^18-slot hash keyed by address space and page, with a lock, a sweeper, a
 * per-page trail, an in-flight table and a line number per transition) and
 * reconcile it, on every hand-over, with three facts only the kernel holds:
 * the mapping (read as /proc/maps text), presence (a pagemap read) and the
 * page itself (a PEEK of it, in case a refused take had destroyed it). None
 * of the four was atomic with any other, and the serve path was fifteen
 * hundred lines of retries for the interleavings that let through: a take
 * whose page was still present afterwards (a sibling's landing between the
 * zap and the check), a take that found nothing while the pagemap said
 * present, a serve that answered ABSENT while this side's own pull was in
 * flight, a refused take's leftover restored over a newer copy.
 *
 * The record now lives where those facts live: per adopted mm, in the
 * kernel, and every transition is made under the mm's lock together with
 * the page-table action it describes. VMCTX_CTL_SERVE answers the other
 * machine's "give me this page" in one step (mapping class, record, take
 * or copy, record update); the fault hook claims a page before the monitor
 * hears about the fault and parks a sibling's fault on the same page until
 * the landing; VMCTX_CTL_LAND installs what the monitor fetched and records
 * it; VMCTX_CTL_PGACK settles the two-phase hand-over. What remains here is
 * the vocabulary (the names below, kept for the log lines and the few
 * remaining readers of a state) and the wrappers.
 *
 *   OURS/HOME     this side holds it and may write it
 *   THEIRS/REMOTE handed over, or the other side filled it itself
 *   CLAIM         this side is fetching it (a fault of a service context)
 *   NONE          nothing recorded: neither machine has touched it
 *   INTRANSIT     handed over, not yet acknowledged installed over there
 * ---------------------------------------------------------------------------
 */
#define PG_OURS      VMCTX_PG_HOME
#define PG_THEIRS    VMCTX_PG_REMOTE
#define PG_CLAIM     VMCTX_PG_CLAIM
#define PG_NONE      VMCTX_PG_NONE
#define PG_INTRANSIT VMCTX_PG_TRANSIT

/*
 * Does the record say the page was handed over (or is being claimed)?
 * Everything that asks "is the page not this side's to serve".
 */
#define PG_GONE(st) ((st) == PG_THEIRS || (st) == PG_CLAIM || \
		     (st) == PG_INTRANSIT)

/*
 * The address space a service context belongs to, which is its thread group.
 * Read from the kernel once per task and remembered: a task's tgid never
 * changes, and this is consulted on every page that moves.
 */
struct as_ent { pid_t pid; uint32_t as; };
static struct as_ent as_tab[1024];
static int as_n;
static pthread_mutex_t as_lock = PTHREAD_MUTEX_INITIALIZER;

static uint32_t as_of(pid_t pid)
{
	char path[64], line[128];
	uint32_t as = (uint32_t)pid;
	FILE *f;
	int i, found = 0;

	if (pid <= 0)
		return 0;
	pthread_mutex_lock(&as_lock);
	for (i = 0; i < as_n; i++)
		if (as_tab[i].pid == pid) {
			as = as_tab[i].as;
			pthread_mutex_unlock(&as_lock);
			return as;
		}
	pthread_mutex_unlock(&as_lock);

	snprintf(path, sizeof(path), "/proc/%d/status", (int)pid);
	f = fopen(path, "r");
	if (f) {
		while (fgets(line, sizeof(line), f))
			if (!strncmp(line, "Tgid:", 5)) {
				as = (uint32_t)atoi(line + 5);
				found = 1;
				break;
			}
		fclose(f);
	}
	/*
	 * DO NOT record a guess. `as` defaults to the pid itself -- an address
	 * space of one -- and if /proc/<pid>/status could not be read, that
	 * default is not an answer, it is the absence of one.
	 *
	 * Caching it is worse than returning it: a context first looked up
	 * AFTER it exited is then filed for ever under an address space no
	 * other context shares, so every later question about its pages -- "who
	 * else can answer for this address space?" -- searches a set of one and
	 * finds nothing. That is why as_live_ctx() came back empty for main's
	 * own stack while main was plainly running, and the fault then fell
	 * through to EFAULT, LOST, INVENTED, FATAL.
	 *
	 * Unresolved is the honest state and it is cheap: the next call retries,
	 * and callers that cannot wait fall back on a context they know is
	 * alive.
	 */
	if (!found)
		return as;
	pthread_mutex_lock(&as_lock);
	if (as_n < (int)(sizeof(as_tab) / sizeof(as_tab[0]))) {
		as_tab[as_n].pid = pid;
		as_tab[as_n].as  = as;
		as_n++;
	}
	pthread_mutex_unlock(&as_lock);
	return as;
}

/*
 * THE LIFETIME OF AN ADDRESS SPACE, as a fact rather than an inference.
 *
 * "Can any live context of this address space read the page" is a LOOKUP, and a
 * lookup that fails is not the same statement as "this address space has
 * ended". A context mid-teardown, an as_of() that has not resolved yet, a
 * ctx0_pid that cannot answer for that address -- each of those makes the
 * lookup fail for a moment while the program is still perfectly alive.
 *
 * That difference is not academic; it is the whole of why three separate
 * attempts to stop a context the owner "could not serve" measured WORSE
 * (ARCHITECTURE §24.3). Each triggered on a failed lookup, and a failed lookup
 * is transient, so each of them killed threads that had work left and ws1 then
 * reported arithmetic that never happened.
 *
 * So the death is counted instead of inferred: every service context registers
 * when its fault service starts and deregisters when that service ends, and an
 * address space is ENDED when, and only when, the count that was once positive
 * reaches zero. `once positive' matters -- a count of zero for an address space
 * nothing has registered yet is not a death, it is a beginning.
 */
#define AS_LIFE_MAX 256
static struct { uint32_t as; int live; int ever; int ended; } as_life[AS_LIFE_MAX];
static int as_life_n;
static pthread_mutex_t as_life_lock = PTHREAD_MUTEX_INITIALIZER;
static unsigned long n_as_ended;	/* address spaces seen to end            */

static void as_life_ref(uint32_t as)
{
	int i;

	pthread_mutex_lock(&as_life_lock);
	for (i = 0; i < as_life_n; i++)
		if (as_life[i].as == as) {
			as_life[i].live++;
			as_life[i].ever = 1;
			as_life[i].ended = 0;
			pthread_mutex_unlock(&as_life_lock);
			return;
		}
	if (as_life_n < AS_LIFE_MAX) {
		as_life[as_life_n].as = as;
		as_life[as_life_n].live = 1;
		as_life[as_life_n].ever = 1;
		as_life[as_life_n].ended = 0;
		as_life_n++;
	}
	pthread_mutex_unlock(&as_life_lock);
}

/* Returns 1 if this was the last one, so the address space has just ended. */
static int as_life_unref(uint32_t as)
{
	int i, last = 0;

	pthread_mutex_lock(&as_life_lock);
	for (i = 0; i < as_life_n; i++)
		if (as_life[i].as == as) {
			if (--as_life[i].live <= 0) {
				as_life[i].live = 0;
				if (as_life[i].ever && !as_life[i].ended) {
					as_life[i].ended = 1;
					n_as_ended++;
					last = 1;
				}
			}
			break;
		}
	pthread_mutex_unlock(&as_life_lock);
	return last;
}

/*
 * Has this address space ENDED? Not "can I find a context for it right now" --
 * that question is asked by as_live_ctx() and its answer is transient.
 */
static int as_has_ended(uint32_t as)
{
	int i, ended = 0;

	pthread_mutex_lock(&as_life_lock);
	for (i = 0; i < as_life_n; i++)
		if (as_life[i].as == as) {
			ended = as_life[i].ended;
			break;
		}
	pthread_mutex_unlock(&as_life_lock);
	return ended;
}

/*
 * Any LIVE context of the same address space, preferring one that is not
 * `avoid`.
 *
 * The clear-tid word is the address space's memory, not the dying context's,
 * and on this side every service context of one guest process is a thread of
 * it -- they share an mm, so a write through any of them is a write all of
 * them see. The dying context is merely the obvious one to use, and it is the
 * one that cannot always be used.
 */
static pid_t as_live_ctx(pid_t like, pid_t avoid)
{
	uint32_t as = as_of(like);
	pid_t best = 0;
	int i;

	if (!as)
		return 0;
	pthread_mutex_lock(&as_lock);
	for (i = 0; i < as_n; i++) {
		char path[64];
		char probe[64];
		FILE *f;
		int readable;

		if (as_tab[i].as != as || as_tab[i].pid == avoid)
			continue;
		/*
		 * NEVER the task we are standing in, whatever the caller
		 * passed. A caller asking "who else can answer for this
		 * address space" that is handed back its own dead context has
		 * been told nothing, and it then asks the corpse a second time
		 * and believes the same answer:
		 *
		 *   context 912283's map could not be read for 0x7fffffffe000;
		 *   its live sibling 912283 says prot=-1
		 *
		 * -- which ends as LOST, then INVENTED, then FATAL, on main's
		 * own stack. That is ws1's remaining invention, in one line of
		 * its own log.
		 */
		if (as_tab[i].pid == like)
			continue;
		/*
		 * And kill(pid, 0) is not liveness here. It succeeds for a
		 * ZOMBIE -- the task is still there, its mm is not -- which is
		 * precisely the state this function exists to route around. The
		 * question being asked is "can this task answer for the address
		 * space", and the only honest test of that is whether its map
		 * can still be read.
		 */
		if (kill(as_tab[i].pid, 0) != 0)
			continue;
		snprintf(path, sizeof(path), "/proc/%d/maps", (int)as_tab[i].pid);
		f = fopen(path, "r");
		if (!f)
			continue;
		readable = fgets(probe, sizeof(probe), f) != NULL;
		fclose(f);
		if (!readable)
			continue;	/* alive as a task, gone as an mm */
		best = as_tab[i].pid;
		break;
	}
	pthread_mutex_unlock(&as_lock);
	return best;
}


static const char *pg_stname(int st)
{
	return st == PG_OURS ? "OURS" : st == PG_THEIRS ? "THEIRS" :
	       st == PG_CLAIM ? "CLAIM" : st == PG_NONE ? "NONE" :
	       st == PG_INTRANSIT ? "INTRANSIT" : "?";
}

static const char *serve_stname(unsigned st)
{
	return st == VMCTX_SERVE_TAKEN ? "TAKEN" :
	       st == VMCTX_SERVE_COPIED ? "COPIED" :
	       st == VMCTX_SERVE_ABSENT ? "ABSENT" :
	       st == VMCTX_SERVE_CLAIMING ? "CLAIMING" :
	       st == VMCTX_SERVE_DENIED ? "DENIED" :
	       st == VMCTX_SERVE_NOMAP ? "NOMAP" : "?";
}

/*
 * The record, read. -1 when the context cannot be asked (gone, or not a
 * service context's address space); the state otherwise, with the take
 * generation of the copy this side holds in *gen if asked.
 */
/*
 * The single-page transits THIS connection committed and has not yet seen
 * acked. A transit's INSTALLED ack arrives on the same connection whose
 * serve committed it, so when that connection ends, every entry still here
 * names a hand-over nobody will ever settle: the record holds TRANSIT, the
 * kernel's SERVE answers CLAIMING on it, and a sibling that faults the page
 * spins on "the source has been claiming this page and nothing has landed"
 * for the rest of the run -- the netsurf pre-paint stall, photographed in
 * session 42b as one page answering exactly that for 2001 faults. The
 * connection's close now settles its own orphans to THEIRS: the bytes went
 * toward that machine, its retained copy is the recovery path, and a page
 * that truly died with the wire ends in the honest handed-over-and-absent
 * death instead of an eternal claim. Per thread because a connection is a
 * thread; sized for the acks' round trip, with evictions counted -- an
 * evicted entry merely loses sweep coverage, never correctness.
 */
#define CONN_TRANSIT_MAX 64
static __thread struct { uint64_t page[CONN_TRANSIT_MAX]; int n; } conn_transit;
static unsigned long n_transit_swept, n_transit_evicted;

static void conn_transit_note(uint64_t page)
{
	int i;

	for (i = 0; i < conn_transit.n; i++)
		if (conn_transit.page[i] == page)
			return;
	if (conn_transit.n == CONN_TRANSIT_MAX) {
		memmove(conn_transit.page, conn_transit.page + 1,
			(CONN_TRANSIT_MAX - 1) * sizeof(uint64_t));
		conn_transit.n--;
		n_transit_evicted++;
	}
	conn_transit.page[conn_transit.n++] = page;
}

static void conn_transit_forget(uint64_t page)
{
	int i;

	for (i = 0; i < conn_transit.n; i++)
		if (conn_transit.page[i] == page) {
			conn_transit.page[i] =
				conn_transit.page[--conn_transit.n];
			return;
		}
}

static int pg_state_gen(pid_t pid, uint64_t page, uint32_t *gen)
{
	struct vmctx_serve sv;

	memset(&sv, 0, sizeof(sv));
	sv.addr = page & ~(uint64_t)(VMR_PG_SIZE - 1);
	if (pid <= 0 || ctl(pid, VMCTX_CTL_PGSTATE, &sv) != 0)
		return -1;
	if (gen)
		*gen = sv.gen;
	return (int)sv.state;
}

static int pg_state(pid_t pid, uint64_t page)
{
	int st = pg_state_gen(pid, page, NULL);

	return st < 0 ? PG_NONE : st;
}

static uint32_t pg_gen(pid_t pid, uint64_t page)
{
	uint32_t g = 0;

	pg_state_gen(pid, page, &g);
	return g;
}

/*
 * Write the record outright: the few transitions that have no page-table
 * action beside them here (a claim released after a failed channel, a copy
 * recorded given). Logged like every transition when the page log is armed.
 */
static void pg_set_at(pid_t pid, uint64_t page, int st, const char *fn, int line)
{
	struct vmctx_pgset ps;
	int old = -1;

	page &= ~(uint64_t)(VMR_PG_SIZE - 1);
	ps.gen = 0;
	old = pg_state_gen(pid, page, &ps.gen);
	ps.addr = page;
	ps.state = (uint32_t)st;
	if (pid > 0)
		ctl(pid, VMCTX_CTL_PGSET, &ps);
	if (!pglog_on || (pglog_on == 2 && page != pglog_page))
		return;
	fprintf(stderr, "[OWN abs=%llu tid=%d] vmhome ctx=%d 0x%012llx %s -> %s "
		"(%s:%d)\n", (unsigned long long)now_us(),
		(int)own_tid(), (int)pid, (unsigned long long)page,
		old < 0 ? "(none)" : pg_stname(old), pg_stname(st), fn, line);
}

#define pg_set(pid, page, st) pg_set_at((pid), (page), (st), __func__, __LINE__)

/*
 * Install a page fetched from the other machine and record this side its
 * holder, stamped with the generation the other machine sent: one step in
 * the kernel. if_absent: refuse (-EEXIST) when a page is already mapped
 * there -- a copy that landed by another path beats arriving bytes.
 */
static long ctx_land(pid_t pid, uint64_t page, const void *buf, uint32_t gen,
		     int if_absent)
{
	struct vmctx_land l = {
		.addr = page & ~(uint64_t)(VMR_PG_SIZE - 1),
		.buf = (uint64_t)(uintptr_t)buf,
		.gen = gen,
		.flags = if_absent ? VMCTX_LAND_IF_ABSENT : 0,
	};
	long r = ctl(pid, VMCTX_CTL_LAND, &l);

	return r < 0 ? -errno : r;
}

/*
 * The pages of [start, end) whose record is in `mask` (1 << PG_*), handed to
 * fn one by one. Returns how many; -1 if the context cannot be asked.
 */
static long pg_scan(pid_t pid, uint64_t start, uint64_t end, unsigned mask,
		    void (*fn)(pid_t, uint64_t, void *), void *arg)
{
	struct vmctx_pgscan sc;
	long n = 0;

	while (start < end) {
		uint32_t i;

		memset(&sc, 0, sizeof(sc));
		sc.start = start;
		sc.end = end;
		sc.mask = mask;
		sc.max = VMCTX_PGSCAN_MAX;
		if (ctl(pid, VMCTX_CTL_PGSCAN, &sc) != 0)
			return n ? n : -1;
		for (i = 0; i < sc.n; i++)
			fn(pid, sc.page[i], arg);
		n += sc.n;
		if (sc.n < VMCTX_PGSCAN_MAX)
			break;
		start = sc.page[sc.n - 1] + VMR_PG_SIZE;
	}
	return n;
}

static void plog(uint64_t addr, const char *fmt, ...);
static int ctx_maps_line(pid_t pid, uint64_t addr, char *out, size_t outlen);


/*
 * Every line this process writes carries its prefix, and the backtrace did not.
 *
 * local-here.sh separates the guest's own stderr from vmhome's by exactly that
 * prefix -- the two share fd 2, so "whatever is not prefixed is the program
 * speaking". backtrace_symbols_fd() writes straight to the descriptor, so a
 * timeout here put four bare "[0x2800281c]" lines into the run's guest.err and
 * they were read as the program's own words. A diagnostic that is attributed to
 * the thing it is diagnosing is worse than no diagnostic.
 */
static void step_timeout(const char *what, uint64_t addr, uint64_t us)
{
	void *bt[32];
	char **sym;
	int n, i;

	fprintf(stderr, "[vmhome] TIMEOUT tid=%d %s 0x%llx after %llums; "
		"backtrace:\n", (int)own_tid(), what,
		(unsigned long long)addr, (unsigned long long)(us / 1000));
	n = backtrace(bt, 32);
	sym = backtrace_symbols(bt, n);
	for (i = 0; i < n; i++)
		fprintf(stderr, "[vmhome]   %s\n",
			sym ? sym[i] : "(no symbol)");
	free(sym);
}

static void plog(uint64_t addr, const char *fmt, ...)
{
	static uint64_t t0;
	uint64_t t;
	va_list ap;
	char b[256];

	if (!pglog_on)
		return;
	if (pglog_on == 2 && (addr & ~(uint64_t)4095) != pglog_page)
		return;
	t = now_us();
	if (!t0)
		t0 = t;
	va_start(ap, fmt);
	vsnprintf(b, sizeof(b), fmt, ap);
	va_end(ap);
	/*
	 * The absolute monotonic microsecond is there so this log can be merged
	 * with the destination's. Both ends run on one machine under
	 * local-here.sh and both read CLOCK_MONOTONIC, so the two files sort
	 * into one timeline on this field -- which is the only way to see a
	 * transition on one side and the answer it produced on the other. The
	 * relative field stays because it is what a reader of one file wants.
	 */
	fprintf(stderr, "[pg %06lu %7llu.%03llums abs=%llu tid=%d] 0x%012llx %s\n",
		__atomic_add_fetch(&pg_seq, 1, __ATOMIC_RELAXED),
		(unsigned long long)((t - t0) / 1000),
		(unsigned long long)((t - t0) % 1000),
		(unsigned long long)t,
		(int)own_tid(),
		(unsigned long long)(addr & ~(uint64_t)4095), b);
}

/*
 * Did this ioctl put the watched page on this side?
 *
 * Asked as a difference across the call rather than inferred from the opcode,
 * because the opcode is not the answer: a PEEK is a read and installs nothing
 * when the page is here, and creates one when it is not (access_process_vm on
 * an absent address faults it in, and this thread is not the context, so its
 * fault is not redirected). Presence either side of the call says which
 * happened, for every op, without anyone having to have listed them.
 *
 * Only the ops that name a range, and only when that range covers the watched
 * page: two pagemap reads per matching call, on a path that already does one.
 */
static void ctl_watch(pid_t pid, unsigned int cmd, const void *arg, int done,
		      long ret)
{
	static __thread int pre;
	static __thread int armed;
	const struct vmctx_mem *m = arg;
	uint64_t first, last;
	int post;

	if (pglog_on != 2 || !arg)
		return;
	switch (cmd) {
	case VMCTX_CTL_PEEK:
	case VMCTX_CTL_POKE:
	case VMCTX_CTL_TAKE:
	case VMCTX_CTL_PROTECT:
	case VMCTX_CTL_TAKEOBJ:
		break;
	default:
		return;
	}
	first = m->addr & ~(uint64_t)4095;
	last  = (m->addr + (m->len ? m->len - 1 : 0)) & ~(uint64_t)4095;
	if (pglog_page < first || pglog_page > last) {
		if (!done)
			armed = 0;
		return;
	}
	if (!done) {
		pre = ctx_present(pid, pglog_page);
		armed = 1;
		return;
	}
	if (!armed)
		return;
	armed = 0;
	post = ctx_present(pid, pglog_page);
	if (pre == post)
		return;
	plog(pglog_page, "CTL %u over 0x%llx+%llu changed the watched page's "
	     "presence here: %d -> %d (ret=%ld) <<< THIS CALL %s IT",
	     cmd, (unsigned long long)m->addr, (unsigned long long)m->len,
	     pre, post, ret, post ? "INSTALLED" : "removed");
}

/*
 * Sample the watched page's presence fast enough to bracket an arrival between
 * two lines of the syscall trace.
 *
 * The ownership sweeper answers a different question and answers it every
 * 100 ms; the window this is looking for is the few milliseconds between a take
 * and the next forwarded call, so the sweeper misses it by an order of
 * magnitude -- measured: 0 sightings in a run whose page log shows the state 61
 * times. This samples one address, not the whole table, so it can afford to.
 *
 * It reads pagemap and nothing else. A peek here would create the page it is
 * looking for, which is the trap this whole instrument exists to avoid.
 */
static void *pgwatch_thread(void *unused)
{
	int last[64];
	unsigned i;

	(void)unused;
	for (i = 0; i < sizeof(last) / sizeof(last[0]); i++)
		last[i] = -2;
	for (;;) {
		int np = 0, k;
		struct as_ent snap[64];

		usleep(100);
		pthread_mutex_lock(&as_lock);
		for (k = 0; k < as_n && np < (int)(sizeof(snap) / sizeof(snap[0]));
		     k++)
			snap[np++] = as_tab[k];
		pthread_mutex_unlock(&as_lock);
		for (k = 0; k < np; k++) {
			int p;

			if (kill(snap[k].pid, 0) != 0)
				continue;
			p = ctx_present(snap[k].pid, pglog_page);
			if (p == last[k])
				continue;
			/*
			 * The record beside the transition, because "the page
			 * arrived" and "the record still says the other machine
			 * has it" are only interesting together: that pair is
			 * the state, and either half alone is ordinary.
			 */
			if (last[k] != -2)
				plog(pglog_page, "WATCH ctx %d: present %d -> %d "
				     "while the record says %s", (int)snap[k].pid,
				     last[k], p,
				     pg_stname(pg_state(snap[k].pid, pglog_page)));
			last[k] = p;
		}
	}
	return NULL;
}

/*
 * Read the context's memory. At most a page per call, as the ABI says.
 *
 * How many bytes it actually read, which is not the same as how many were
 * asked for. PEEK is access_process_vm() underneath, and that reports a count:
 * zero for an address the program has no mapping for. Both of these used to
 * answer "len" whenever the call did not fail outright, so an unmapped address
 * came back as a full page of whatever happened to be in the caller's buffer,
 * and the far side installed it and let the access through.
 *
 * pf1's third case is exactly that: it maps a page, unmaps it, and writes to
 * the address on purpose. Its handler never ran, because the write was served
 * with a page nobody had -- "0 fault(s) ... left by longjmp 0". An access the
 * program has no right to make must reach the program as a fault, and the only
 * thing that makes it one here is this side saying it has nothing.
 */
/*
 * A read of the program's memory must never CREATE the program's memory.
 *
 * PEEK is access_process_vm() underneath, and the task calling it is not the
 * context, so its fault is not redirected to the monitor: the local kernel just
 * installs a page. For an address whose current contents are on the other
 * machine that is not a read at all -- it invents a page here, out of the file
 * or out of zeros, and the ownership record still says the other machine holds
 * it. Both machines then have the page, neither knows, and the next thing this
 * side writes into it is read by nobody while the guest reads its own copy.
 *
 * Callers know this and guard: "read only when the page is already here". The
 * guard checks the address they were given. What it cannot check, because it
 * does not know the length, is the page AFTER it -- and a read of a struct or a
 * path from an address near the end of a page reaches into the next one. That is
 * not a corner: it is what killed firefox. Measured, with the call caught in the
 * act on the page it created:
 *
 *   CTL 7 over 0x7fffffff8fd0+96 changed the watched page's presence here:
 *   0 -> 1 (ret=96) <<< THIS CALL INSTALLED IT
 *   openat(..., 0x7fffffff8fd0, ...) /home/biwu/ff/libthai.so.0
 *   SYSCALL 0: watched page present here pre=1 post=1
 *   read(0x7, 0x7fffffff91e8, 0x340) -> 832
 *   -> libthai.so.0: invalid ELF header
 *
 * 0x8fd0 + 96 is 0x9030. The trace reads 96 bytes of path from a page it checked
 * and spills 0x30 of them into the page the guest had just been given, which the
 * peek then makes exist here. The comment at that call site names this exact
 * hazard -- "a trace that poisons the run it is tracing" -- and the guard was one
 * page short of implementing it.
 *
 * So the rule belongs here rather than at each caller, where it has to be got
 * right once instead of eight times: a peek stops at the end of the page it was
 * given unless the next page is already here. It returns short, which every
 * caller already handles, because a short peek is what "the rest is not on this
 * machine" has always looked like. The first page is left to the caller's own
 * guard: several callers legitimately peek a page they have just been handed.
 *
 * Proven, and then the arm was deleted rather than kept. Measured on one boot
 * with the presence probe made in BOTH arms so they differed by the clamp and
 * nothing else -- this workload's timing is load-bearing, so a switch that also
 * removed a pagemap read would have been two changes wearing one name:
 *
 *   clamp on    3/3 clean, 80 libraries loaded, 6 clamps taken
 *   clamp off   3/3 "libXrender.so.1: invalid ELF header", 55 libraries
 *
 * Deterministic both ways. That is what the arm was for; keeping it afterwards
 * would only leave a compiled-in way to put the defect back.
 */
static unsigned long n_peek_clamped;

static long ctx_peek(pid_t pid, uint64_t addr, void *buf, uint64_t len)
{
	struct vmctx_mem m;
	uint64_t room;
	long r;

	if (len > 4096)
		len = 4096;
	room = VMR_PG_SIZE - (addr & (VMR_PG_SIZE - 1));
	if (len > room && ctx_present(pid, addr + room) != 1) {
		n_peek_clamped++;
		len = room;
	}
	m.addr = addr;
	m.len  = len;
	m.buf  = (uint64_t)(uintptr_t)buf;
	r = ctl(pid, VMCTX_CTL_PEEK, &m);
	if (r < 0)
		return -errno;
	return r;
}

static long ctx_poke(pid_t pid, uint64_t addr, const void *buf, uint64_t len)
{
	struct vmctx_mem m;
	long r;

	if (len > 4096)
		len = 4096;
	m.addr = addr;
	m.len  = len;
	m.buf  = (uint64_t)(uintptr_t)buf;
	r = ctl(pid, VMCTX_CTL_POKE, &m);
	if (r < 0)
		return -errno;
	return r;
}

/* ---- the pages the guest has written ----------------------------------- */

/*
 * Reads of the program's memory here are right by construction: the task making
 * them holds the program's real address space. Its *writes* are not. The
 * program's stores execute on the machine running its instructions, into that
 * machine's copy, and nothing carries them back — so a page this side touches
 * may be one the guest has since written, and the current contents exist only
 * over there.
 *
 * The kernel no longer invents a zero page for one of these. VMCTX_CTL_ADOPT
 * sets VMCTX_FLAG_REDIRECT_FAULT, so the service context's faults are reported
 * to its monitor, which is this process. This is the half that answers them:
 * fetch the page from the machine running the guest, install it, and let the
 * access retry.
 *
 * VMR_PG_GET, not VMR_PG_GETS, and that choice is the whole of the coherence
 * rule.
 *
 * GET moves the page. The other machine hands over the bytes and takes the page
 * away from the guest, so afterwards exactly one machine can write it and it is
 * this one. GETS would leave the guest a write-protected copy; the guest's next
 * write is then answered over there by asking this side to give the page back,
 * over a recall channel that no longer exists — the shadow that used to hold
 * one is gone. That request fails, its failure is not checked, and the guest's
 * copy is quietly made writable again: two writers and no notification, which
 * is exactly the class of silence this whole change exists to remove.
 *
 * GET's failure mode is the opposite one, and it is benign and self-correcting.
 * The guest's next touch of a page taken from it faults, finds no recall
 * channel, says so, and falls back to asking this side for the page with
 * VMR_OP_CTXPAGE — which is the right answer, because this side is the holder.
 */
/*
 * A peer that has EXITED refuses every connect, and the callers that dial
 * here at teardown -- clear-tid pokes, settle pulls, late GETs -- each retry
 * a few times per page for a page that is never coming. Measured on the
 * fork-wipe campaign's pipes case: the guest finished, vmremote reported and
 * left, and vmhome then dialed the dead port 471 times over 85 seconds until
 * the harness watchdog converted a finished run into an rc-124 wedge. So the
 * refusals are COUNTED, and after enough consecutive ones the port is
 * declared gone once, loudly, and every later dial fails fast. Any success
 * resets it -- a peer that is merely slow to bind gets its retries.
 */
#define PG_GONE_AFTER 20
static _Atomic int pg_dead_port;
static _Atomic int pg_refused_n;

static int pg_connect(const char *peer, int port)
{
	struct sockaddr_in a;
	int fd, one = 1;

	if (pg_dead_port == port)
		return -1;
	fd = socket(AF_INET, SOCK_STREAM, 0);
	if (fd < 0)
		return -1;
	memset(&a, 0, sizeof(a));
	a.sin_family = AF_INET;
	a.sin_port   = htons(port);
	if (inet_pton(AF_INET, peer, &a.sin_addr) != 1 ||
	    connect(fd, (struct sockaddr *)&a, sizeof(a)) < 0) {
		if (errno == ECONNREFUSED && ++pg_refused_n >= PG_GONE_AFTER) {
			pg_dead_port = port;
			fprintf(stderr, "[vmhome] the guest's page service at "
				"%s:%d has refused %d consecutive connects -- "
				"the peer is gone; failing fast from here on\n",
				peer, port, (int)pg_refused_n);
		} else if (pg_refused_n < PG_GONE_AFTER) {
			fprintf(stderr, "[vmhome] cannot reach the guest's "
				"page service at %s:%d: %s\n", peer, port,
				strerror(errno));
		}
		close(fd);
		return -1;
	}
	pg_refused_n = 0;
	/* A strict request/response channel: Nagle plus delayed ACK is its
	 * worst case, and every fault of the program waits on it. */
	setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
	/*
	 * And a bounded one. Every read on this channel is one page or one
	 * short status, from a peer on the same LAN; seconds of silence is a
	 * stuck peer, not a slow page. Without this bound the measured
	 * failure is a fault service blocked here forever while its context
	 * sleeps in the kernel holding mmap_read_lock, wedging the box.
	 *
	 * Four, not two: the peer's GET handler legitimately waits
	 * pull_wait 500 + claim 1000 + take 1000 ms before it answers, and a
	 * bound EQUAL to or below the thing it bounds fires on a slow answer
	 * rather than a missing one -- measured as 2 of 256 hx2 pool runs
	 * ending in exit 13 ("remote GET of ... after 2009ms", "the page
	 * channel failed and did not recover"). This sits inside the kernel's
	 * fault deadline, which vmctx_deadline_enforce() holds at 6 s.
	 */
	{
		struct timeval tv = { .tv_sec = 4, .tv_usec = 0 };

		setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
	}
	fcntl(fd, F_SETFD, FD_CLOEXEC);
	return fd;
}

/*
 * One page out of the machine running the guest. 0 with the page in buf,
 * VMR_PG_ABSENT if the guest has never touched that address, or -1 if the
 * channel failed.
 *
 * ABSENT is not an error and not a page of zeros to be installed: it means the
 * guest has never written there, so the bytes over there are not the program's
 * and this side's own kernel is the right thing to ask.
 */
/*
 * op is VMR_PG_GET (move the page here, the guest loses it) or VMR_PG_GETS
 * (copy it, the guest keeps a write-protected one). GET is the rule; see the
 * note above pg_connect(). GETS is used for exactly one thing and it is safe
 * for exactly that thing: a READ-ONLY file mapping. GETS's danger is the
 * guest's next write to its retained copy needing a recall channel that does
 * not exist -- but a read-only mapping is never written (a store faults as a
 * protection fault long before), so that write never comes, and leaving the
 * guest its copy is what keeps a running program's own text and rodata from
 * being pulled out from under it every time a forwarded syscall reads a string.
 */
static unsigned long n_get_inflight;	/* GETs that waited out a crossing */
/* GET replies installed OVER a present landing because their stamp was
 * strictly newer and both stamps were known; see the drop rule. */
/* The take generation stamped on the last GET-class reply this thread read
 * (vmr_pgrsp.gen); the caller that installs the bytes records it with
 * pg_gen_set() at the install, and nowhere else. */
static __thread uint32_t last_get_gen;

/*
 * pg_get_op's third outcome, beside bytes and ABSENT: the page LANDED HERE
 * while the far side was answering INFLIGHT -- a sibling service thread's pull
 * of the same page installed it -- so the ask is over and its answer is moot.
 * Never on the wire; it is this side's own word for its own landing.
 */
#define PG_GOT_LANDED 3
/*
 * Asks that ended in a landing instead of an answer, by where the landing was
 * noticed: before the first ask left (the sibling had already installed the
 * page while this fault waited), or on an INFLIGHT answer inside pg_get_op.
 */
static unsigned long n_claim_landed_inflight;
static unsigned long n_claim_landing_behind;	/* present, clock-kind INFLIGHT, but the landing is behind the far side's count: kept asking */
/*
 * Session 28's A/B (hx2 x256, graded by guest.out; NOTES 2.21): ending the
 * claim on ANY INFLIGHT answer 235/256; on the clock kind only, with a
 * pre-ask "already present" check 243; without that check 246 (the control,
 * both off, 247); with the stamp guard below 249. The arms are gone; the
 * numbers stay.
 */
static int prefetch_read_replies(int fd);

/*
 * One-shot aux for the NEXT pg_get_op's rq.len (unused by every op except
 * PUTN's real length): a COWBREAK carrying 1 is the ESCALATED form -- "my
 * own routes failed; give from the nearest readable ancestor, mark or no
 * mark". Thread-local like the reply-side gen state, cleared on entry so a
 * retry loop keeps it and the next call does not inherit it.
 */
static __thread uint64_t pg_req_aux;

static int pg_get_op(int fd, uint32_t op, uint64_t addr, void *buf, uint64_t ctx,
		     pid_t landed)
{
	int tries = 0;
	uint64_t t_inflight0 = 0;
	uint64_t aux = pg_req_aux;

	pg_req_aux = 0;
again:
	{
		struct vmr_pgreq rq = { VMR_PG_MAGIC, op, addr, aux, ctx,
					(uint64_t)getpid() };
		struct vmr_pgrsp rs;

		if (write_all(fd, &rq, sizeof(rq)) < 0)
			return -1;
		/* A readahead window written before this request answers
		 * first (prefetch_issue); its replies are taken off the
		 * stream here so the next reply is this request's. */
		if (prefetch_read_replies(fd) < 0)
			return -1;
		if (read_all(fd, &rs, sizeof(rs)) <= 0)
			return -1;
		if (rs.magic != VMR_PG_MAGIC) {
			fprintf(stderr, "[vmhome] page 0x%llx: the reply is not ours "
				"(magic 0x%x, status %d) — the stream is out of step\n",
				(unsigned long long)addr, rs.magic, (int)rs.status);
			return -1;
		}
		/* A page follows either way; see pg_serve_conn() on the other side. */
		if (read_all(fd, buf, VMR_PG_SIZE) <= 0)
			return -1;
		last_get_gen = rs.gen;
		/*
		 * "The token is in transit to you; ask again." The far side's
		 * record says this side holds the page while this side is
		 * asking for it -- a serve and a pull of one page crossing in
		 * flight. The bytes land there within microseconds, so asking
		 * again is the whole answer; every caller of every GET-shaped
		 * op benefits from the retry living here, and none can misread
		 * the status. The far side answers INFLIGHT while the landing
		 * is genuinely in flight (a pull of the page active there, or
		 * a young episode inside its 1200ms grace), then falls back to
		 * its retained-copy net, so this loop normally ends with bytes
		 * or ABSENT well inside the 2s fault deadline; the 400-try
		 * bound here is only against a peer that misbehaves, and -1
		 * routes to the callers' existing record-based handling,
		 * never to invented zeros.
		 */
		if (rs.status == VMR_PG_INFLIGHT) {
			/*
			 * THE LANDING DECIDES, on this side too (session 28).
			 *
			 * INFLIGHT from the far side means "my record says YOU
			 * hold this page and I am not producing it": the
			 * commonest way for that to be true is that a SIBLING
			 * service thread's pull of the same page already landed
			 * it here -- two contexts of one address space faulting
			 * on one page (the pg3 join: B's stack page, main's
			 * futex word and B's own frame). The far side cannot
			 * see that landing; this side can, in one pagemap
			 * read. Without this check the asker sat here for the
			 * far side's whole 1200ms grace with the page marked
			 * CLAIM, and every guest pull for a page THIS SIDE
			 * ALREADY HELD was refused CLAIMING -- ~2500 yields a
			 * run, "CLAIMING ... here=1" for the whole storm, the
			 * sweep's "both machines are holding it", and a fault
			 * that ran up against the kernel's deadline. The
			 * "landing beats arriving bytes" rule below then
			 * dropped the answer anyway -- after the storm. Drop
			 * it now instead: the claim ends, the guest's next pull
			 * is served from the page this side holds, and the
			 * crossing is decided by who landed, not by a clock.
			 * Absence faults only (the caller passes 0 for a
			 * protection fault, whose page is present by nature).
			 */
			/*
			 * Only when the far side says it is its CLOCK holding
			 * the answer (rs.gen 0), never when its own pull of the
			 * page is landing there (VMR_INFLIGHT_PULL): that pull
			 * ends in a fresh take the GET then brings here, and
			 * the "arriving newer than the landing" override below
			 * needs those bytes -- measured: ending the claim on
			 * every INFLIGHT took the hx2 pool from 250 to 235 of
			 * 256 (the landings it kept were one take behind).
			 */
			/*
			 * ...and only when the landing is not BEHIND the far
			 * side's count: the INFLIGHT reply carries its current
			 * capture count of the page, and a landing stamped
			 * below it means the guest has had the page back and
			 * written it since -- NOTES 6.1a's split brain, where
			 * this side holds a stale present copy. There the
			 * fetch must go on: its arrival's stamp is what heals
			 * it ("arriving newer than the landing, installed over
			 * it"). Measured without this guard: hx2 243/256
			 * against 247 with the check off. An unstamped landing
			 * (0) is treated as behind.
			 */
			if (landed > 0 && rs.why != VMR_INFLIGHT_PULL &&
			    ctx_present(landed, addr) == 1) {
				uint32_t lg = 0;
				int lst = pg_state_gen(landed, addr, &lg);

				/*
				 * "Landed" means the record moved on from this
				 * side's claim -- a LAND or a POKE by another
				 * path wrote HOME -- not merely that a page is
				 * mapped there.
				 */
				if (lst == PG_OURS && lg != 0 && lg >= rs.gen) {
					n_claim_landed_inflight++;
					return PG_GOT_LANDED;
				}
				n_claim_landing_behind++;
			}
			/*
			 * Bounded by TIME, not by tries: INFLIGHT lasts as long
			 * as the peer's own pull of the page, which can sit
			 * behind this side's 1 s take hold, and 400 tries of
			 * 500us were 200 ms -- a budget that a legitimately
			 * in-flight page outlived. 1.5 s is above the hold and
			 * inside the 4 s socket bound and the 6 s deadline.
			 */
			if (tries == 0) {
				n_get_inflight++;
				t_inflight0 = now_us();
			}
			tries++;
			if (now_us() - t_inflight0 < 1500000) {
				usleep(500);
				goto again;
			}
			return -1;
		}
		return rs.status == VMR_PG_ABSENT ? VMR_PG_ABSENT : 0;
	}
}

static int pg_get(int fd, uint64_t addr, void *buf, uint64_t ctx)
{
	return pg_get_op(fd, VMR_PG_GET, addr, buf, ctx, 0);
}

/*
 * Give a page BACK: the undo of a take whose install here failed.
 *
 * A pull is take-then-install, and the two are on different machines, so the
 * install failing leaves the taken bytes as the ONLY copy in existence --
 * dropping them is how a live thread stack became a page on neither machine
 * (netsurf, 88 rescue cycles). The far side's PUT pokes the page back into the
 * address space it came from and clears the loan, so the next fault there
 * finds the real bytes.
 */
static int pg_put(int fd, uint64_t addr, const void *buf, uint64_t ctx)
{
	struct vmr_pgreq rq = { VMR_PG_MAGIC, VMR_PG_PUT, addr, 0, ctx,
				(uint64_t)getpid() };
	struct vmr_pgrsp rs;

	/* Header and page in one segment: one wakeup on the far side. */
	if (writev_all(fd, &rq, sizeof(rq), buf, VMR_PG_SIZE) < 0)
		return -1;
	if (read_all(fd, &rs, sizeof(rs)) <= 0)
		return -1;
	return (rs.magic == VMR_PG_MAGIC && rs.status == 0) ? 0 : -1;
}



/*
 * One of these per service context, because VMCTX_CTL_WAIT names a context and
 * a fault is reported to whoever is waiting on that one. A thread of the
 * monitoring process is what the kernel requires, and any thread of it will do
 * — which is why this can be a thread while another is blocked in a forwarded
 * syscall for the very context that is faulting.
 */
struct mon_arg {
	pid_t    pid;		/* the service context to wait on            */
	uint64_t ctx;		/* whose memory, in the other machine's terms */
	int      port;		/* this vmhome's port; pages are on port + 1  */
	char     peer[64];
	uint64_t ra_next;	/* readahead: where the last window ended     */
	unsigned ra_win;	/* ...and how many pages the next one gets    */
};

/*
 * READAHEAD FOR A FORWARDED SYSCALL'S BUFFER (session 40; session 38's
 * "I/O-buffer bounce"). A forwarded read(2) into a 32 KiB buffer the guest
 * holds faulted eight times here, one GET round trip each, serialised: the
 * call copies a page, faults on the next, waits for it. Every one of those
 * pages is going to be touched in order, and nothing about that is specific
 * to read(2): it is what a syscall does with a buffer. So after a page lands
 * for a context that is inside an assisted call, the pages after it are
 * asked for in one go -- the requests written back to back on the page
 * channel, the replies read in order -- and landed before the call reaches
 * them (or, if it reaches one first, its fault finds the CLAIM this wrote
 * and waits in the kernel for the landing, vmctx_pgrec_claim). The window
 * is the page cache's: nothing on the first fault, one page on the next
 * sequential one (this fault is where the last window ended), then two,
 * then four, back to nothing when they are not sequential -- so a buffer
 * of eight pages costs four round trips instead of eight, and a scattered
 * fault costs nothing extra. Every page is a real hand-over: CLAIMED in the record before the
 * ask, LANDed with IF_ABSENT (a copy that landed by another path wins), and
 * the record put back exactly as it was on any answer that is not a page.
 * Runs AFTER the faulting page's RESUME, so it overlaps the call's copy of
 * that page instead of delaying it.
 */
#define RA_MAX 4
static unsigned long n_ra_windows, n_ra_pages, n_ra_landed, n_ra_absent,
		     n_ra_inflight, n_ra_present, n_ra_failed, n_ra_eexist;

static int pg_put(int fd, uint64_t addr, const void *buf, uint64_t ctx);

/*
 * The window in flight on this thread's page channel: issued BEFORE the
 * fault's own GET (prefetch_issue), its replies read by pg_get_op() just
 * before the fault's reply -- the channel answers in request order and a
 * reply names no address, so the order is the whole protocol -- and landed
 * once the fault's page has (prefetch_settle), before the RESUME, while the
 * context is still blocked on it. One round trip carries the faulting page
 * and its window.
 */
static __thread struct {
	int      fd;		/* the channel the window was written to */
	unsigned n;		/* requests written, replies not yet read  */
	unsigned got;		/* replies read and waiting to be landed    */
	pid_t    pid;
	uint64_t q[RA_MAX];
	int      prior[RA_MAX];
	struct vmr_pgrsp rs[RA_MAX];
	char     buf[RA_MAX][VMR_PG_SIZE];
} ra;

/* The claims come off: nothing of this window will land. */
static void prefetch_abort(void)
{
	unsigned i;

	for (i = 0; i < ra.n + ra.got; i++)
		pg_set(ra.pid, ra.q[i], ra.prior[i]);
	if (ra.n || ra.got)
		n_ra_failed++;
	ra.n = 0;
	ra.got = 0;
	ra.fd = -1;
}

/*
 * THE WIRE'S PRICE, measured: a moving average of a single GET round trip
 * on this fault service's channel (fed by the fault service, from GETs with
 * no window in front of them). A window costs a page each of take, wire and
 * landing (~35 us here) to save one round trip, so it pays only where the
 * round trip costs clearly more than that -- a LAN -- and not on this box's
 * loopback, where the two are the same size and readahead measured sha at
 * +40% per syscall (session 40). Off until the channel has said its price.
 */
#define RA_MIN_RTT_US 120
/*
 * ...as a tunable, so the threshold can be measured against the channel it
 * gates rather than remembered from session 40's wake-heavy path (the
 * kernel's spin-before-sleep and the socket spins have since cut a loopback
 * round trip by 3x, which moves the price the gate compares against).
 * VMCTX_RA_MIN_RTT_US; 0 turns readahead on everywhere.
 */
static unsigned ra_min_rtt_us = RA_MIN_RTT_US;
static __thread uint64_t get_rtt_us;
static __thread unsigned long get_rtt_samples;

static void prefetch_note_rtt(uint64_t dt)
{
	get_rtt_us = get_rtt_us ? (get_rtt_us * 7 + dt) / 8 : dt;
	get_rtt_samples++;
}

static int prefetch_pays(void)
{
	/*
	 * The measured RTT decides, and nothing overrides it: the
	 * VMHOME_READAHEAD force lived here for the A/B that PROVED the
	 * gate (loopback -40% off, LAN pays) and was deleted with the
	 * question -- an unused force is how a quoted A/B goes stale.
	 */
	return get_rtt_samples >= 16 && get_rtt_us >= ra_min_rtt_us;
}

static void prefetch_issue(struct mon_arg *ma, int pg, uint64_t base)
{
	unsigned n = 0, i;

	if (ra.n || ra.got)
		prefetch_abort();	/* never two windows in flight */
	if (pg < 0 || !incall_get(ma->pid) || !prefetch_pays())
		return;
	/*
	 * Evidence first: the first fault of a run reads nothing ahead (a
	 * scattered fault inside a call -- a stat buffer, a path -- is the
	 * common case, and a speculative page there cost wc/curl/python
	 * 30-50% per syscall); the second sequential fault reads one page,
	 * then two, then four.
	 */
	if (base == ma->ra_next)
		ma->ra_win = ma->ra_win * 2 > RA_MAX ? RA_MAX
			   : (ma->ra_win ? ma->ra_win * 2 : 1);
	else
		ma->ra_win = 0;
	ma->ra_next = base + VMR_PG_SIZE;
	if (!ma->ra_win)
		return;
	for (i = 0; i < ma->ra_win; i++) {
		uint64_t a = base + (uint64_t)(i + 1) * VMR_PG_SIZE;
		int st = pg_state(ma->pid, a);

		if (st == PG_OURS) {	/* already here: the run is served */
			n_ra_present++;
			break;
		}
		if (st == PG_CLAIM || st == PG_INTRANSIT)
			break;		/* a transition in flight: not ours */
		ra.prior[n] = st;
		ra.q[n] = a;
		n++;
	}
	if (!n)
		return;
	ma->ra_next = ra.q[n - 1] + VMR_PG_SIZE;
	ra.pid = ma->pid;
	ra.fd = pg;
	for (i = 0; i < n; i++)
		pg_set(ma->pid, ra.q[i], PG_CLAIM);
	for (i = 0; i < n; i++) {
		struct vmr_pgreq rq = { VMR_PG_MAGIC, VMR_PG_GET, ra.q[i], 0,
					ma->ctx, (uint64_t)getpid() };

		if (write_all(pg, &rq, sizeof(rq)) < 0) {
			ra.n = i;	/* what was written is what is owed */
			prefetch_abort();
			return;
		}
	}
	ra.n = n;
	n_ra_windows++;
}

/* Called by pg_get_op() before it reads the fault's own reply. */
static int prefetch_read_replies(int fd)
{
	unsigned i;

	if (!ra.n || ra.fd != fd)
		return 0;
	for (i = 0; i < ra.n; i++) {
		if (read_all(fd, &ra.rs[i], sizeof(ra.rs[i])) <= 0 ||
		    ra.rs[i].magic != VMR_PG_MAGIC ||
		    read_all(fd, ra.buf[i], VMR_PG_SIZE) <= 0) {
			prefetch_abort();
			return -1;
		}
		n_ra_pages++;
	}
	ra.got = ra.n;
	ra.n = 0;
	return 0;
}

static void prefetch_settle(struct mon_arg *ma)
{
	unsigned i;

	if (ra.n) {		/* replies never read: the fault's GET failed */
		prefetch_abort();
		return;
	}
	for (i = 0; i < ra.got; i++) {
		struct vmr_pgrsp *rs = &ra.rs[i];

		if (rs->status == 0) {
			long lr = ctx_land(ma->pid, ra.q[i], ra.buf[i], rs->gen, 1);

			if (lr == (long)VMR_PG_SIZE) {
				n_ra_landed++;
				continue;
			}
			if (lr == -EEXIST) {	/* landed by another path */
				n_ra_present++;
				if (n_ra_eexist++ < 12)
					fprintf(stderr, "[vmhome] RA-EEXIST 0x%llx "
						"ctx %d: prior=%s record-now=%s\n",
						(unsigned long long)ra.q[i],
						(int)ma->pid, pg_stname(ra.prior[i]),
						pg_stname(pg_state(ma->pid, ra.q[i])));
				continue;
			}
			/*
			 * The bytes are the only copy and nothing here can
			 * take them: give the page back, as ctx_pull_page
			 * does, rather than drop it.
			 */
			n_ra_failed++;
			if (ra.fd >= 0 &&
			    pg_put(ra.fd, ra.q[i], ra.buf[i], ma->ctx) == 0)
				pg_set(ma->pid, ra.q[i], PG_THEIRS);
			else
				pg_set(ma->pid, ra.q[i], ra.prior[i]);
			continue;
		}
		if (rs->status == VMR_PG_ABSENT)
			n_ra_absent++;
		else
			n_ra_inflight++;
		pg_set(ma->pid, ra.q[i], ra.prior[i]);
	}
	ra.got = 0;
	ra.fd = -1;
}


/*
 * ---------------------------------------------------------------------------
 * Where a context's pages can be pulled from, for anything that is not a fault.
 *
 * A fault has the monitor thread and its channel to hand. A *write* into the
 * program's memory that this process makes itself has neither, and it needs the
 * same pull — see ctx_poke_owned(). So the route to the machine running the
 * guest is recorded per context when its monitor starts, and any thread can ask
 * for it.
 * ---------------------------------------------------------------------------
 */
struct ctx_route {
	pid_t    pid;
	uint64_t ctx;
	int      port;
	char     peer[64];
};
static struct ctx_route ctx_routes[512];
static int ctx_nroutes;
static pthread_mutex_t ctx_route_lock = PTHREAD_MUTEX_INITIALIZER;

static void ctx_route_add(pid_t pid, uint64_t ctx, const char *peer, int port)
{
	int i;

	pthread_mutex_lock(&ctx_route_lock);
	for (i = 0; i < ctx_nroutes; i++)
		if (ctx_routes[i].pid == pid)
			break;
	if (i == (int)(sizeof(ctx_routes) / sizeof(ctx_routes[0]))) {
		pthread_mutex_unlock(&ctx_route_lock);
		return;
	}
	ctx_routes[i].pid  = pid;
	ctx_routes[i].ctx  = ctx;
	ctx_routes[i].port = port;
	snprintf(ctx_routes[i].peer, sizeof(ctx_routes[i].peer), "%s",
		 peer && *peer ? peer : "127.0.0.1");
	if (i == ctx_nroutes)
		ctx_nroutes++;
	pthread_mutex_unlock(&ctx_route_lock);
}

static int ctx_route_find(pid_t pid, struct ctx_route *out)
{
	int i, ok = 0;

	pthread_mutex_lock(&ctx_route_lock);
	for (i = 0; i < ctx_nroutes; i++)
		if (ctx_routes[i].pid == pid) {
			*out = ctx_routes[i];
			ok = 1;
			break;
		}
	pthread_mutex_unlock(&ctx_route_lock);
	return ok;
}

static void sh_file_report(void);

/*
 * The fast sampler (pgwatch_thread), only when a page has been named and
 * asked for: it is one pagemap read per context every 100us, which is a
 * diagnostic's cost and not a run's. The ownership sweep that rode beside
 * it is gone with the table it swept: the record is the kernel's now.
 */
static void sweep_start(void)
{
	static pthread_mutex_t once = PTHREAD_MUTEX_INITIALIZER;
	static int started;
	pthread_t th;

	pthread_mutex_lock(&once);
	if (!started) {
		started = 1;
		if (pglog_on == 2 && getenv("VMHOME_PGWATCH") &&
		    pthread_create(&th, NULL, pgwatch_thread, NULL) == 0)
			pthread_detach(th);
	}
	pthread_mutex_unlock(&once);
}

static pid_t fork_rel_parent(pid_t child);
static void fork_rel_forget(pid_t child);
static int fork_rel_children(pid_t parent, pid_t *out, int max);
static int cow_is_broken(pid_t child, uint64_t page);
static void cow_note_break(pid_t child, uint64_t page);
static long ctx_page_inner(pid_t pid, uint64_t addr, uint64_t len, char *out,
			   size_t outmax);
#define FORK_REL_MAX	256
static unsigned long n_cow_from_parent;	/* pages taken by breaking the sharing */
static unsigned long n_cow_file_from_parent; /* private-FILE pages a fork copy took from the parent's diverged copy, not the file */


static void *ctx_monitor(void *arg)
{
	struct mon_arg *ma = arg;
	unsigned long served = 0, absent = 0, failed = 0, kill_asked = 0;
	char page[VMR_PG_SIZE];
	uint32_t as = as_of(ma->pid);
	int pg = -1;
	const char *end_why = "?";
	int end_errno = 0;
	char ph[128];

	/*
	 * This context is live from here until its fault service ends, and that
	 * pair is what gives the address space a lifetime. See as_life_ref().
	 */
	as_life_ref(as);

	for (;;) {
		struct vmctx_event ev;
		struct vmctx_reply rep;
		uint64_t base;
		int got = -1;
		int prior = PG_NONE, rec_on = 0, claimed_here = 0;
		uint32_t cls = 0;

		if (ctl(ma->pid, VMCTX_CTL_WAIT, &ev) != 0) {
			if (errno == EINTR)
				continue;
			end_why = "WAIT";
			end_errno = errno;
			break;		/* ESRCH: the context has gone */
		}

		memset(&rep, 0, sizeof(rep));
		rep.action = VMCTX_ACT_SELF;

		if (ev.type == VMCTX_EV_FAULT) {
			base = ev.fault_addr & ~(uint64_t)(VMR_PG_SIZE - 1);

			plog(base, "FAULT err=0x%llx rip=0x%llx pre=%d",
			     (unsigned long long)ev.fault_err,
			     (unsigned long long)ev.rip,
			     pglog_on ? ctx_present(ma->pid, base) : -1);
			if (sigprobe) {
				struct vmctx_uregs g;
				char sc[128] = "";
				int fd;

				snprintf(sc, sizeof(sc), "/proc/%d/syscall",
					 (int)ma->pid);
				fd = open(sc, O_RDONLY);
				sc[0] = '\0';
				if (fd >= 0) {
					ssize_t k = read(fd, sc, sizeof(sc) - 1);

					sc[k > 0 ? k - 1 : 0] = '\0';
					close(fd);
				}
				if (ctl(ma->pid, VMCTX_CTL_GETREGS, &g) == 0)
					fprintf(stderr, "[sigprobe] fault 0x%llx: "
						"context stands at rip=0x%llx "
						"rsp=0x%llx ax=0x%llx | %s\n",
						(unsigned long long)base,
						(unsigned long long)g.rip,
						(unsigned long long)g.rsp,
						(unsigned long long)g.rax, sc);
			}

			uint64_t g0 = pglog_on ? now_us() : 0;

			/*
			 * THE CLAIM IS THE KERNEL'S. The record said
			 * args[3] about this page before the fault hook
			 * claimed it (args[4] says there is a record: a
			 * service context's address space); from that
			 * instant every serve of the page answers CLAIMING
			 * and every sibling's fault on it is parked in the
			 * kernel until this thread lands it -- no marker
			 * table here, no 500 us polls, no "landing beats
			 * arriving bytes" race between two monitor threads
			 * fetching one page (only one can be). The landing
			 * is ctx_land() (install + record, one step) or the
			 * RESUME itself (SELF: the local kernel fills; DONE:
			 * the page is here by another path).
			 */
			rec_on = !!(ev.args[4] & VMCTX_PGC_VALID);
			prior = rec_on ? (int)ev.args[3] : PG_NONE;
			cls = (uint32_t)ev.args[2];

			/*
			 * A read-only file page that no forwarded mprotect has
			 * sealed cannot have diverged: the guest can never
			 * have written it, so the file here is its current
			 * bytes. Serve it locally and do not even ask -- this
			 * is the whole of what keeps the widened hook from
			 * touching a running program's text and rodata. Only
			 * RELRO ranges (relro_note(), from a forwarded
			 * mprotect to read-only) can differ, and only they
			 * are asked about.
			 */
			int ro_file = (cls & VMCTX_PGC_VALID) &&
				      (cls & VMCTX_PGC_FILE) &&
				      !(cls & VMCTX_PGC_SHARED) &&
				      !(cls & VMCTX_PGC_WRITE);
			if (ro_file && !PG_GONE(prior) && !relro_has(base)) {
				n_rofile_local++;
				src_arrival = 6;
				plog(base, "FAULT in a read-only file page no "
				     "mprotect has sealed: the file here is "
				     "current, served locally without asking");
				goto answered;	/* rep.action stays ACT_SELF */
			}
			/*
			 * The record says this side holds the page and the
			 * page table says it is not here: the local fill of a
			 * previous answer is landing (the retry path reports
			 * a declined fault twice -- the kernel's own
			 * loop-at-most-once), or this is a protection fault
			 * on a page that is here. Either way the local
			 * kernel's answer is the page; nothing to ask.
			 */
			if (rec_on && prior == PG_OURS) {
				n_home_local++;
				src_arrival = 5;
				plog(base, "FAULT on a page the record says "
				     "this side holds: the local kernel "
				     "answers");
				goto answered;
			}
			/*
			 * A sibling's claim stands (the non-retry path, where
			 * the kernel could not wait for it with the mm lock
			 * held): wait for its landing here, off every lock.
			 * Landed and present: nothing to supply. Landed as
			 * HOME with nothing here: its local fill follows.
			 * Gone (the sibling was killed): claim and fetch.
			 */
			if (rec_on && (prior == PG_CLAIM ||
				       prior == PG_INTRANSIT)) {
				int spin, stt = prior;

				/*
				 * The kernel could not wait for the transition
				 * (it held mmap_read_lock; waiting there for a
				 * landing that needs the same lock is the ws1
				 * three-way deadlock). Wait for it here, off
				 * every lock: a sibling's CLAIM lands HOME, a
				 * serve's TRANSIT is acked REMOTE or healed.
				 */
				n_sibling_claim_waited++;
				for (spin = 0; spin < 4000; spin++) {
					stt = pg_state(ma->pid, base);
					if (stt != PG_CLAIM && stt != PG_INTRANSIT)
						break;
					usleep(500);
				}
				if (ctx_present(ma->pid, base) == 1) {
					n_absent_averted++;
					src_arrival = 2;
					rep.action = VMCTX_ACT_DONE;
					goto answered;
				}
				if (stt == PG_OURS) {
					src_arrival = 5;
					goto answered;	/* SELF */
				}
				prior = (stt == PG_CLAIM ||
					 stt == PG_INTRANSIT) ? PG_NONE : stt;
				pg_set(ma->pid, base, PG_CLAIM);
				claimed_here = 1;
			}

			/*
			 * A RELRO read-only file page is COPIED, not moved
			 * (VMR_PG_GETS): the guest keeps executing/reading its
			 * copy and this side gets the current bytes. Safe
			 * because a read-only mapping is never written, so the
			 * retained copy never needs the recall channel GET was
			 * chosen to avoid. A writable file page still moves
			 * (GET) -- it can diverge and must have one writer.
			 */
			if (ro_file)
				n_relro_asked++;
			if (pg < 0)
				pg = pg_connect(ma->peer,
						ma->port + VMR_PG_PORT_OFFSET);
			if (pg >= 0) {
				uint64_t gt0 = now_us();

				if (!ro_file)
					prefetch_issue(ma, pg, base);
				got = pg_get_op(pg, ro_file ? VMR_PG_GETS
							    : VMR_PG_GET,
						base, page, ma->ctx,
						(ev.fault_err & 1) ? 0 : ma->pid);
				if (got == 0 && !ra.got)	/* no window: a clean sample */
					prefetch_note_rtt(now_us() - gt0);
				if (got < 0) {
					if (errno == EAGAIN ||
					    errno == EWOULDBLOCK)
						step_timeout("remote GET of",
							     base,
							     now_us() - gt0);
					prefetch_abort();
					close(pg);
					pg = -1;
				}
			}
			/*
			 * A channel failure is not an answer (PRINCIPLES §3):
			 * the ask is retried through a fresh connection,
			 * bounded well inside the fault deadline; what remains
			 * unanswerable after that is handled below by what the
			 * RECORD says the page is, never by silent zeros.
			 */
			if (got < 0) {
				int rt;

				for (rt = 0; rt < 30 && got < 0; rt++) {
					usleep(500);
					if (pg < 0)
						pg = pg_connect(ma->peer,
								ma->port + VMR_PG_PORT_OFFSET);
					if (pg < 0)
						continue;
					got = pg_get_op(pg, ro_file ? VMR_PG_GETS
								    : VMR_PG_GET,
							base, page, ma->ctx,
						(ev.fault_err & 1) ? 0 : ma->pid);
					if (got < 0) {
						close(pg);
						pg = -1;
					}
				}
				if (got >= 0 || got == VMR_PG_ABSENT)
					n_chan_regained++;
			}

			plog(base, "GET got=%d sum=%08x%s first8=%02x%02x%02x%02x"
			     "%02x%02x%02x%02x", got,
			     got == 0 ? page_sum(page) : 0,
			     got == 0 ? pgword(page) : "",
			     (unsigned char)page[0], (unsigned char)page[1],
			     (unsigned char)page[2], (unsigned char)page[3],
			     (unsigned char)page[4], (unsigned char)page[5],
			     (unsigned char)page[6], (unsigned char)page[7]);
			/* poke-in: the bytes just pulled from the other machine,
			 * about to be installed here. Below-mark = this side is
			 * receiving a stale copy from the other end. */
			if (got == 0)
				src_clobber(base, page, 2);

			if (got == PG_GOT_LANDED) {
				/*
				 * The ask ended in a landing, not an answer
				 * (pg_get_op's INFLIGHT check): the page was
				 * installed here by another path while the far
				 * side was saying "you hold it". The claim
				 * ends on the landing.
				 */
				plog(base, "GET answered INFLIGHT and the page "
				     "landed here meanwhile; the claim ends, "
				     "the access retries");
				rep.action = VMCTX_ACT_DONE;
				src_arrival = 8;
				served++;
			} else if (got == 0) {
				/*
				 * The landing: install + record, one step. For
				 * an absence fault a page already mapped there
				 * (landed by another path: a copy given to a
				 * forked child, a teardown write) beats the
				 * arriving bytes -- the kernel refuses the
				 * install (-EEXIST) and the access retries on
				 * what is there. A protection fault's page is
				 * present by nature; its land is unconditional.
				 */
				long lr = ctx_land(ma->pid, base, page,
						   last_get_gen,
						   !(ev.fault_err & 1));

				if (lr == (long)VMR_PG_SIZE) {
					rep.action = VMCTX_ACT_DONE;
					src_arrival = 1;
					served++;
					/*
					 * The page has just left the other
					 * machine, and with it the only copy
					 * any address space forked from this
					 * one could still have been given. So
					 * give it to them here, now, while the
					 * bytes are in hand -- see tests/cow1.
					 */
					{
						pid_t kids[FORK_REL_MAX];
						int nk = fork_rel_children(ma->pid, kids,
									   FORK_REL_MAX);
						int ki;

						for (ki = 0; ki < nk; ki++) {
							if (cow_is_broken(kids[ki], base))
								continue;
							if (pg_state(kids[ki], base) != PG_NONE)
								continue;
							if (ctx_land(kids[ki], base, page,
								     0, 1) != (long)VMR_PG_SIZE)
								continue;
							cow_note_break(kids[ki], base);
							n_cow_kept_for_copy++;
						}
					}
					if ((cls & VMCTX_PGC_VALID) &&
					    (cls & VMCTX_PGC_FILE) &&
					    !(cls & VMCTX_PGC_SHARED))
						n_file_pulled++;
				} else if (lr == -EEXIST) {
					n_pull_landed_raced++;
					plog(base, "GET returned bytes but the "
					     "page landed here by another path "
					     "meanwhile; the arriving copy is "
					     "dropped");
					rep.action = VMCTX_ACT_DONE;
					src_arrival = 8;
					served++;
				} else {
					fprintf(stderr, "[vmhome] context %d: "
						"could not install 0x%llx "
						"fetched from the guest's machine "
						"(%s); the context ends rather "
						"than run on a page that is "
						"nowhere\n", (int)ma->pid,
						(unsigned long long)base,
						strerror((int)-lr));
					rep.action = VMCTX_ACT_KILL;
				}
			} else if (got == VMR_PG_ABSENT &&
				   (cls & VMCTX_PGC_VALID) &&
				   (cls & VMCTX_PGC_FILE) &&
				   !(cls & VMCTX_PGC_SHARED) &&
				   !PG_GONE(prior) &&
				   fork_rel_parent(ma->pid) && pg >= 0 &&
				   pg_get_op(pg, VMR_PG_COWBREAK, base, page,
					     ma->ctx, 0) == 0 &&
				   ctx_land(ma->pid, base, page, last_get_gen,
					    0) == (long)VMR_PG_SIZE) {
				/*
				 * A private file page in a FORK COPY, and the
				 * PARENT's copy over there had diverged from
				 * the file. "Never handed over, so the file is
				 * current" is the CHILD's record talking, and
				 * for a fork child it lies: the parent parsed
				 * its options into .data before the clone, on
				 * the machine running it, and serving the
				 * child the file's pristine bytes hands it a
				 * program state from before main() ran.
				 * Measured on bwrap (the netsurf/glycin wall,
				 * session 41f): the sandbox child ran with
				 * as-loaded globals -- setuid(-1), the
				 * unparsed default -- then #GP'd on garbage.
				 * Same COWBREAK the anonymous fresh branch has
				 * had since tests/cow1; a refusal ("nothing
				 * owed": the parent never diverged the page)
				 * falls through to the file serve below, which
				 * is then genuinely current.
				 */
				cow_note_break(ma->pid, base);
				n_cow_file_from_parent++;
				plog(base, "FAULT in a fork copy's private file "
				     "mapping: the parent's DIVERGED copy taken "
				     "by copy-on-write break, not the file");
				rep.action = VMCTX_ACT_DONE;
				served++;
			} else if (got == VMR_PG_ABSENT &&
				   (cls & VMCTX_PGC_VALID) &&
				   (cls & VMCTX_PGC_FILE) &&
				   !(cls & VMCTX_PGC_SHARED) &&
				   !PG_GONE(prior)) {
				/*
				 * A private file page the far machine says it
				 * does not hold, and that this side never handed
				 * over: the file's own bytes are the current
				 * ones, and the local kernel serves them
				 * (ACT_SELF). Decided AFTER asking: the far
				 * machine's ABSENT is the proof the file is
				 * current, which "never handed over" alone is
				 * not -- and for a FORK COPY the branch above
				 * has already asked the parent's machine for a
				 * diverged copy and been told nothing is owed.
				 */
				n_file_local++;
				src_arrival = 7;
				plog(base, "FAULT in a private file mapping the "
				     "guest's machine does not hold and this side "
				     "never handed over: the local kernel serves "
				     "it from the file");
				/* rep.action stays VMCTX_ACT_SELF */
			} else if (got == VMR_PG_ABSENT) {
				/*
				 * "This side does not hold that page" is a fact
				 * about the other machine, not about the page:
				 * the two are the same thing only for an
				 * address the program has never touched. The
				 * record tells them apart.
				 *
				 *   NONE    fresh: zeros are the program's own
				 *           kernel's answer (unless this address
				 *           space is a fork copy -- then the page
				 *           is still the original's, over there)
				 *   GONE    handed over, and the holder says it
				 *           does not hold it: ask again, then
				 *           end the context -- never invent
				 */
				static char zeros[VMR_PG_SIZE];
				int tries;

				if (PG_GONE(prior)) {
					n_absent_theirs++;
					if (n_absent_theirs <= 32)
						fprintf(stderr, "[vmhome] context %d: 0x%llx was handed to the guest's machine (state %s) and that machine now answers ABSENT for it (rip 0x%llx); the bytes are not this side's to invent\n",
							(int)ma->pid,
							(unsigned long long)base,
							pg_stname(prior),
							(unsigned long long)ev.rip);
				}
				if (ctx_present(ma->pid, base) == 1) {
					n_absent_averted++;
					src_arrival = 2;
					plog(base, "GET says ABSENT but the page "
					     "is here by another path; the "
					     "access retries");
					rep.action = VMCTX_ACT_DONE;
				} else if (PG_GONE(prior)) {
					for (tries = 0; tries < 40 &&
					     got == VMR_PG_ABSENT; tries++) {
						usleep(500);
						if (pg < 0)
							pg = pg_connect(ma->peer,
									ma->port + VMR_PG_PORT_OFFSET);
						if (pg < 0)
							break;
						got = pg_get(pg, base, page,
							     ma->ctx);
						if (got < 0) {
							close(pg);
							pg = -1;
							break;
						}
						if (ctx_present(ma->pid, base) == 1)
							break;
					}
					if (got == 0 &&
					    ctx_land(ma->pid, base, page,
						     last_get_gen, 0) ==
					    (long)VMR_PG_SIZE) {
						n_absent_regained++;
						src_arrival = 3;
						rep.action = VMCTX_ACT_DONE;
						served++;
					} else if (ctx_present(ma->pid, base) == 1) {
						n_absent_averted++;
						src_arrival = 2;
						rep.action = VMCTX_ACT_DONE;
					} else {
						n_absent_lost++;
						fprintf(stderr, "[vmhome] context %d: 0x%llx is on neither machine after %d asks: this side handed it over and the guest's machine answers ABSENT for it. Ending the context rather than putting bytes there that nobody holds (rip 0x%llx)\n",
							(int)ma->pid,
							(unsigned long long)base,
							tries,
							(unsigned long long)ev.rip);
						rep.action = VMCTX_ACT_KILL;
					}
				} else {
					/*
					 * Fresh anonymous memory -- unless this
					 * address space began as a copy of
					 * another, in which case the page is
					 * still the original's, on the machine
					 * that holds the original: break the
					 * sharing there and take the copy's
					 * page (tests/cow1).
					 */
					if (fork_rel_parent(ma->pid) &&
					    pg >= 0 &&
					    pg_get_op(pg, VMR_PG_COWBREAK, base,
						      page, ma->ctx, 0) == 0 &&
					    ctx_land(ma->pid, base, page,
						     last_get_gen, 0) ==
					    (long)VMR_PG_SIZE) {
						cow_note_break(ma->pid, base);
						n_cow_from_parent++;
						plog(base, "GET says ABSENT for "
						     "a page this address space "
						     "inherited; broke "
						     "copy-on-write there and "
						     "took the copy's page");
						rep.action = VMCTX_ACT_DONE;
						served++;
						absent++;
						goto answered;
					}
					/*
					 * A FORK COPY is never owed zeros for
					 * an address it inherited. The zero-
					 * land below did run for this case,
					 * and the measured corpse is exact
					 * (progs dcpi, ~1/6): the guest's
					 * machine answers ABSENT for the
					 * child's TLS page, the COWBREAK
					 * above is refused ("parent readable
					 * 0"), zeros became the child's TLS,
					 * THREAD_SELF read NULL, and dc died
					 * writing self->robust_head at
					 * 0x2d8.
					 *
					 * Where the inheritance actually is:
					 * the PARENT's page, wherever it
					 * lives. "Let the child's own kernel
					 * COW serve it" is NOT that -- it was
					 * built here first and refuted by
					 * the very next corpse: a page the
					 * OTHER machine held at clone time is
					 * a HOLE in the parent's mm here, the
					 * child's mm copied that hole, and
					 * the kernel's COW of a hole is the
					 * zero page. So read the parent's
					 * own memory THROUGH ITS TASK: if
					 * the page is here (taken back for a
					 * syscall -- the measured case, the
					 * parent parked in wait4), the read
					 * returns it; if it is on the other
					 * machine, the read faults it home
					 * through the standard capture, which
					 * is the same pull any assisted
					 * syscall touching parent memory
					 * makes. Parent-current bytes rather
					 * than fork-time bytes -- the take
					 * mutated the page in place and the
					 * fork-time copy no longer exists
					 * anywhere (that destruction is its
					 * own ledger entry) -- but for an
					 * undiverged page they are the same
					 * bytes, and for TLS they are self-
					 * consistent pointers where zeros
					 * are a NULL THREAD_SELF.
					 */
					/*
					 * ...and "the parent's page" is the
					 * CHAIN's page: kernel #173 measured
					 * that under the fork wipe a direct
					 * parent's page may be absent at ITS
					 * level too, and reading it through
					 * the parent's task recursed the
					 * fault up the chain until the
					 * fallthrough was zeros again. So
					 * walk the chain the way the
					 * destination's cow_give_from_chain
					 * does, one step: the NEAREST
					 * ancestor whose mm actually HOLDS
					 * the page (ctx_present -- a pagemap
					 * read, never a fault) has the
					 * subtree's inherited bytes, because
					 * an ancestor without its own copy
					 * passes inheritance through
					 * unchanged. Only if no ancestor
					 * holds it here does the old
					 * single-level FAULTING read of the
					 * direct parent run -- the case
					 * where the page lives on the other
					 * machine under an ancestor the
					 * destination's own walk could not
					 * vouch for -- and that read cannot
					 * recurse more than one level,
					 * because the level below it is
					 * answered by this walk.
					 */
					if (fork_rel_parent(ma->pid)) {
						pid_t anc;
						int hops = 0, landed = 0;

						for (anc = fork_rel_parent(ma->pid);
						     anc && hops < 16;
						     anc = fork_rel_parent(anc),
						     hops++) {
							if (ctx_present(anc, base) != 1)
								continue;
							if (ctx_page_inner(anc, base,
									   VMR_PG_SIZE,
									   page,
									   VMR_PG_SIZE) !=
							    (long)VMR_PG_SIZE)
								continue;
							landed = ctx_land(ma->pid, base,
									  page, 0, 1) ==
								 (long)VMR_PG_SIZE;
							break;
						}
						if (!landed &&
						    ctx_page_inner(fork_rel_parent(ma->pid),
								   base, VMR_PG_SIZE,
								   page, VMR_PG_SIZE) ==
						    (long)VMR_PG_SIZE &&
						    ctx_land(ma->pid, base, page,
							     0, 1) ==
						    (long)VMR_PG_SIZE) {
							landed = 1;
							anc = fork_rel_parent(ma->pid);
							hops = -1; /* the faulting read */
						}
						/*
						 * Every route of this side's
						 * has now failed, which is
						 * the one fact that licences
						 * the ESCALATED break: the
						 * other machine may serve the
						 * nearest ancestor page it
						 * can READ, protect mark or
						 * not (its strict gate exists
						 * to route HERE, and here has
						 * answered no).
						 */
						if (!landed && pg >= 0) {
							pg_req_aux = 1;
							if (pg_get_op(pg,
								      VMR_PG_COWBREAK,
								      base, page,
								      ma->ctx, 0) == 0 &&
							    ctx_land(ma->pid, base,
								     page, 0, 1) ==
							    (long)VMR_PG_SIZE) {
								landed = 1;
								anc = 0;
								hops = -2; /* their side, last-resort */
							}
						}
						if (landed) {
							cow_note_break(ma->pid, base);
							n_cow_self_cow++;
							plog(base, "inherited page "
							     "absent everywhere: landed "
							     "ancestor %d's page (%d "
							     "hop(s) up)", (int)anc,
							     hops);
							if (n_cow_self_cow <= 16)
								fprintf(stderr,
									"[vmhome] context %d: "
									"0x%llx inherited, absent "
									"on the guest's machine, "
									"break refused -- landed "
									"ancestor %d's own page "
									"(%d hop(s) above, %s) "
									"(rip 0x%llx)\n",
									(int)ma->pid,
									(unsigned long long)base,
									(int)anc, hops < 0 ? 0 : hops,
									hops < 0 ? "by the faulting "
									"read" : "held here",
									(unsigned long long)ev.rip);
							rep.action = VMCTX_ACT_DONE;
							src_arrival = 9;
							served++;
							absent++;
							goto answered;
						}
					}
					/*
					 * Supplied here as an ordinary page
					 * rather than left to the local
					 * kernel, whose read fault would
					 * install the SHARED zero page --
					 * PG_reserved, unclaimable, every take
					 * of it refused for ever (pg2's
					 * livelock). The same 4096 zero bytes
					 * either way; this side says so and
					 * owns what it said.
					 */
					n_absent_ours++;
					if (ctx_land(ma->pid, base, zeros, 0, 1)
					    == (long)VMR_PG_SIZE) {
						plog(base, "GET says ABSENT -> supplied "
						     "an ordinary empty page here");
						if (n_absent_filled < 16) {
							char ml[512];

							ctx_maps_line(ma->pid, base,
								      ml, sizeof(ml));
							fprintf(stderr, "[vmhome] "
								"context %d: 0x%llx "
								"answered ABSENT and "
								"filled with an empty "
								"page (rip 0x%llx, err "
								"0x%llx, class 0x%x, "
								"record %s) maps: %s\n",
								(int)ma->pid,
								(unsigned long long)base,
								(unsigned long long)ev.rip,
								(unsigned long long)ev.fault_err,
								cls,
								pg_stname(prior), ml);
						}
						rep.action = VMCTX_ACT_DONE;
						n_absent_filled++;
						src_arrival = 4;
					} else if (ctx_present(ma->pid, base) == 1) {
						n_absent_averted++;
						src_arrival = 2;
						rep.action = VMCTX_ACT_DONE;
					} else {
						plog(base, "GET says ABSENT -> the local "
						     "kernel zero-fills this address "
						     "for good");
					}
				}
				absent++;
			} else if (PG_GONE(prior)) {
				/*
				 * The channel died and stayed dead through the
				 * retries, and the record says this page EXISTS
				 * over there. Letting SELF stand fills a live
				 * address with zeros, which is the invention
				 * PRINCIPLES §3 forbids. A named, counted death
				 * points at the channel; zeros point at nothing.
				 */
				n_chan_lost++;
				if (kill_asked < 4)
					fprintf(stderr, "[vmhome] context %d faulted on "
						"0x%llx (rip 0x%llx), the page channel "
						"failed and did not recover, and the "
						"record says the page exists (%s). "
						"Ending the context rather than zero-"
						"filling live memory\n", (int)ma->pid,
						(unsigned long long)base,
						(unsigned long long)ev.rip,
						pg_stname(prior));
				rep.action = VMCTX_ACT_KILL;
				/*
				 * ...and the ending must STICK. Measured on the
				 * fork-wipe campaign's pipes case: the peer had
				 * exited with this context's stack page, ACT_KILL
				 * was answered, and the SAME fault came back from
				 * WAIT for ever -- a finished run held at the
				 * watchdog's pleasure by a context that would not
				 * die politely. Three polite answers, then the
				 * impolite one, and the loop ends either way.
				 */
				if (++kill_asked >= 3) {
					fprintf(stderr, "[vmhome] context %d "
						"ignored %lu KILL answer(s); "
						"SIGKILL and closing its "
						"service\n", (int)ma->pid,
						kill_asked);
					/* A claim this thread wrote must not
					 * outlive its service (the answered:
					 * settle, which this break skips) --
					 * a leaked CLAIM is the stuck-page
					 * livelock every sibling then spins
					 * on. */
					if (claimed_here &&
					    pg_state(ma->pid, base) == PG_CLAIM)
						pg_set(ma->pid, base, prior);
					ctl(ma->pid, VMCTX_CTL_RESUME, &rep);
					kill(ma->pid, SIGKILL);
					end_why = "kill-refused";
					end_errno = 0;
					break;
				}
			} else {
				/*
				 * Said out loud every time. Answering SELF here
				 * hands the program a page nobody supplied --
				 * tolerable only for a page no record says
				 * anyone ever held (NONE), where the local
				 * kernel's zeros are what the program's own
				 * kernel would give a first touch.
				 */
				if (failed++ < 32)
					fprintf(stderr, "[vmhome] context %d "
						"faulted on 0x%llx (err 0x%llx, "
						"rip 0x%llx) and the guest's "
						"machine could not supply it; "
						"the local kernel will fill it "
						"with zeros\n", (int)ma->pid,
						(unsigned long long)base,
						(unsigned long long)ev.fault_err,
						(unsigned long long)ev.rip);
			}
answered:
			/*
			 * A claim this thread wrote itself (after a sibling's
			 * ended without a landing) is not the kernel's to
			 * land: settle it here by the outcome.
			 */
			if (claimed_here && pg_state(ma->pid, base) == PG_CLAIM)
				pg_set(ma->pid, base,
				       rep.action == VMCTX_ACT_KILL ? prior
								      : PG_OURS);
			if (pglog_on == 2 && base == pglog_page)
				__sync_fetch_and_add(&n_watch_faults, 1);
			plog(base, "FAULT-step ctx=%d got=%d action=%u us=%llu",
			     (int)ma->pid, got, rep.action,
			     (unsigned long long)(now_us() - g0));
		}

		/*
		 * The window lands BEFORE the RESUME: the context is blocked on
		 * this reply, so nothing of its own can touch the pages while
		 * they land. Landing them after the RESUME overlapped the
		 * context's copy of the page just served -- and one landing in
		 * ~20000 then overwrote a page the call had already written
		 * (gzip read its buffer's last page one block stale, 1 run in
		 * 7). The overlap is not worth a lost store; the one round trip
		 * for the faulting page and its window together is kept.
		 */
		prefetch_settle(ma);
		if (ctl(ma->pid, VMCTX_CTL_RESUME, &rep) != 0 && errno == ESRCH) {
			end_why = "RESUME";
			end_errno = errno;
			break;
		}
	}
	if (as_life_unref(as))
		fprintf(stderr, "[vmhome] address space %u has ENDED: its last "
			"service context is gone, so nothing here can read a "
			"byte of that program's memory again -- and this is a "
			"fact, not a lookup that happened to fail\n", as);
	if (pg >= 0)
		close(pg);
	fprintf(stderr, "[vmhome] context %d: fault service ends (%lu pages "
		"fetched, %lu the guest never touched, %lu unanswered); %lu of "
		"the untouched ones were filled here as ordinary pages rather "
		"than left to the local kernel's shared zero page -- ended by "
		"%s: %s; the task now: alive=%d %s\n",
		(int)ma->pid, served, absent, failed, n_absent_filled,
		end_why, strerror(end_errno), kill(ma->pid, 0) == 0,
		task_photo(ma->pid, ph, sizeof(ph)));
	fprintf(stderr, "[vmhome] private file mapping faults: %lu answered from "
		"the file here (never handed over -- no fetch, no zeros), %lu "
		"pulled back from the guest's machine (handed over; the file "
		"here was stale)\n", n_file_local, n_file_pulled);
	fprintf(stderr, "[vmhome] read-only file faults: %lu served locally "
		"without asking (unsealed -- the file is current), %lu asked of "
		"the guest's machine (%lu RELRO range(s) sealed by a forwarded "
		"mprotect)\n", n_rofile_local, n_relro_asked, n_relro_ranges);
	fprintf(stderr, "[vmhome] faults answered locally because the record "
		"already said this side holds the page: %lu; faults that found "
		"a sibling's claim standing and waited for its landing here: "
		"%lu\n", n_home_local, n_sibling_claim_waited);
	fprintf(stderr, "[vmhome] reads of the program's memory stopped at a page "
		"boundary rather than spilling into a page that is not here: %lu\n",
		n_peek_clamped);
	fprintf(stderr, "[vmhome] forwarded calls the context could not be asked "
		"to run (each a fabricated -EIO): %lu, of which %lu found the "
		"assisted slot busy and %lu a dying context; and %lu answers "
		"carried -EIO OUT OF THE EXECUTION itself\n",
		n_ctxcall_fail, n_ctxcall_busy, n_ctxcall_dead, n_ret_eio);
	fprintf(stderr, "[vmhome] vacated ranges from the mm-mutation hook "
		"shipped to the destination: %lu\n", n_vacates_shipped);
	fprintf(stderr, "[vmhome] of the pages the guest's machine answered "
		"ABSENT for, %lu had never been handed over by this side (fresh "
		"memory, zeros are the program's own kernel's answer) and %lu "
		"had been; %lu were here by the time the answer was read, %lu "
		"came back on a re-ask, and %lu were on neither machine and "
		"ended their context rather than being invented\n",
		n_absent_ours, n_absent_theirs, n_absent_averted,
		n_absent_regained, n_absent_lost);
	fprintf(stderr, "[vmhome] GETs whose bytes arrived after the page had "
		"landed here by another path: %lu dropped\n",
		n_pull_landed_raced);
	fprintf(stderr, "[vmhome] claims ended by a landing on an INFLIGHT "
		"answer: %lu; %lu INFLIGHTs with a present landing BEHIND the "
		"far side's count kept asking\n",
		n_claim_landed_inflight, n_claim_landing_behind);
	fprintf(stderr, "[vmhome] page record (the kernel's): %lu answers of "
		"\"this side does not hold it\", %lu of them because the record "
		"said the page was handed over or being claimed\n",
		n_serve_absent, n_serve_refused);
	fprintf(stderr, "[vmhome] two-phase hand-over: %lu single-page serves "
		"committed IN TRANSIT, %lu acked installed, %lu acks ignored "
		"(the page had moved on), %lu asks answered CLAIMING while "
		"the page was on the wire to the asker\n",
		n_intransit_committed, n_intransit_acked,
		n_intransit_ack_stale, n_serve_intransit_refused);
	fprintf(stderr, "[vmhome] socket reads that spun (%u us budget): %lu "
		"answered inside the spin, %lu ran out and slept\n",
		sock_spin_us, n_sock_spin_hit, n_sock_spin_miss);
	if (n_transit_swept || n_transit_evicted)
		fprintf(stderr, "[vmhome] ...and %lu unacked transit(s) settled "
			"THEIRS when their own connection closed (a claim must "
			"not outlive its wire); %lu tracking slot(s) evicted "
			"(those pages merely lost sweep coverage)\n",
			n_transit_swept, n_transit_evicted);
	fprintf(stderr, "[vmhome] self-map vacate suppression: %lu MAP_FIXED/"
		"MREMAP_FIXED range(s) recorded before the call, %lu vacate "
		"portion(s) dropped as an mmap's own self-unmap (the netsurf "
		"exit-242 ordering edge, killed at the source)\n",
		n_srccons_pushed, n_srccons_suppressed);
	fprintf(stderr, "[vmhome] STALE-SERVE report(s) from the guest's "
		"machine: %lu%s\n", n_staletrail_asked,
		n_staletrail_asked ? "  <<< this side served a copy older than "
		"a capture the other side had made" : "");
	sh_file_report();
	fprintf(stderr, "[vmhome] whole pages asked for: %lu handed over "
		"(taken), %lu copied (read-only, or a shared file's)\n",
		n_mov_hit, n_mov_miss);
	if (n_src_clobber_calls[1] || n_src_clobber_calls[2])
		fprintf(stderr, "[vmhome] clobber detector on the watched page: "
			"%lu serve-out(s), of which %lu handed back a value BELOW "
			"the mark; %lu poke-in(s), of which %lu arrived below it; "
			"worst backward move %lu\n",
			n_src_clobber_calls[1], n_src_backmove[1],
			n_src_clobber_calls[2], n_src_backmove[2],
			n_src_backmove_worst);
	fprintf(stderr, "[vmhome] fork copy-on-write: %lu page request(s) from a "
		"copied address space answered \"still the parent's, break it "
		"there\"; %lu write(s) by a parent held up to give a copy its "
		"page first, over %lu (address space, page) pair(s) recorded "
		"broken; %lu write(s) needed nothing and %lu were writes the "
		"program itself forbids\n",
		n_cow_child_told, n_cow_parent_told, n_cow_pages_owed,
		n_cow_write_free, n_cow_write_ro);
	fprintf(stderr, "[vmhome] ...and %lu page(s) handed to a copy as they came "
		"back from the other machine, because a page that leaves stops "
		"being what the copy inherited\n", n_cow_kept_for_copy);
	fprintf(stderr, "[vmhome] ...and %lu page(s) this side's own copy of a "
		"forked address space took by breaking the sharing on the other "
		"machine, rather than being given an empty page for memory it "
		"had inherited\n", n_cow_from_parent);
	if (n_cow_self_cow)
		fprintf(stderr, "[vmhome] ...and %lu inherited page(s) absent "
			"on the guest's machine landed from the PARENT's own "
			"current page, read through its task -- a fork copy "
			"is never owed zeros\n", n_cow_self_cow);
	fprintf(stderr, "[vmhome] takes refused by a syscall hold: %lu waited "
		"out; %lu given up after a full second (answered CLAIMING)\n",
		n_take_busy, n_take_gaveup);
	if (pglog_on == 2)
		fprintf(stderr, "[vmhome] the watched page became present here "
			"across a forwarded call %lu time(s), %lu of them with "
			"no fault answered for it in that window -- a page this "
			"side acquired without asking anyone\n",
			n_watch_appeared, n_watch_unasked);
	fprintf(stderr, "[vmhome] thread teardowns whose clear-tid word ended up "
		"cleared: %lu; that did not: %lu -- each of the second kind is a "
		"pthread_join that never returns; %lu were cleared through a "
		"sibling context because the dying one could not take the write, "
		"%lu woken through one for the same reason\n",
		n_ctid_ok, n_ctid_bad, n_ctid_via_sibling, n_ctid_wake_sibling);
	fprintf(stderr, "[vmhome] ...of the cleared, %lu had already handed "
		"the word's page to the woken joiner before the verify could "
		"read it back (cleared, unverifiable here)\n",
		n_ctid_unverified);
	fprintf(stderr, "[vmhome] pulls-for-a-write whose named context could not "
		"take the install: %lu landed through a live sibling of the same "
		"mm, %lu were dropped with no sibling to take them -- each of the "
		"second kind is a page whose only copy was discarded\n",
		n_pull_via_sibling, n_pull_lost);
	fprintf(stderr, "[vmhome] pulls whose install failed everywhere here and "
		"whose page was GIVEN BACK to the guest's machine: %lu\n",
		n_pull_returned);
	fprintf(stderr, "[vmhome] page-channel failures during a fault GET: %lu "
		"recovered on a fresh connection, %lu terminal with the record "
		"saying the page exists (context ended rather than zero-filled)\n",
		n_chan_regained, n_chan_lost);
	fprintf(stderr, "[vmhome] GETs that crossed a serve of the same page "
		"(the far side answered 'token in transit, ask again'): %lu\n",
		n_get_inflight);
	fprintf(stderr, "[vmhome] a page GET round trip costs %llu us on this "
		"channel (%lu samples); readahead %s\n",
		(unsigned long long)get_rtt_us, get_rtt_samples,
		prefetch_pays() ? "ON (the wire is the cost)" :
		"OFF (a round trip costs what a page costs; loopback)");
	fprintf(stderr, "[vmhome] readahead inside forwarded calls: %lu window(s), "
		"%lu page(s) asked for in them, %lu landed, %lu already here, "
		"%lu the guest's machine did not hold, %lu in flight there, "
		"%lu could not be kept\n", n_ra_windows, n_ra_pages,
		n_ra_landed, n_ra_present, n_ra_absent, n_ra_inflight,
		n_ra_failed);
	fprintf(stderr, "[vmhome] address space(s) seen to END (last service "
		"context gone): %lu; requests this side could not answer: %lu "
		"where the address space really had ended, %lu where it had NOT "
		"and the lookup was merely momentary -- the second kind is what "
		"three teardown repairs killed the program for; %lu request(s) "
		"were answered \"this address space has ended, stop\"\n",
		n_as_ended, n_ctxpage_gone_ended, n_ctxpage_gone_transient,
		n_ctxpage_gone_told);
	free(ma);
	return NULL;
}

static void ctx_monitor_start(pid_t pid, uint64_t ctx, const char *peer, int port)
{
	sweep_start();
	struct mon_arg *ma;
	pthread_t th;

	if (pid <= 0)
		return;
	ma = malloc(sizeof(*ma));
	if (!ma) {
		fprintf(stderr, "[vmhome] no fault service for context %d: out "
			"of memory\n", (int)pid);
		return;
	}
	ma->pid  = pid;
	ma->ctx  = ctx;
	ma->port = port;
	snprintf(ma->peer, sizeof(ma->peer), "%s", peer && *peer ? peer
							         : "127.0.0.1");
	ctx_route_add(pid, ctx, ma->peer, port);
	if (pthread_create(&th, NULL, ctx_monitor, ma) != 0) {
		fprintf(stderr, "[vmhome] no fault service for context %d: %s\n",
			(int)pid, strerror(errno));
		free(ma);
		return;
	}
	pthread_detach(th);
}

/*
 * Writing into the program's memory takes the page first.
 *
 * This is the defect the page log was built to find, and it is worth stating in
 * full because nothing about it is visible from either end alone.
 *
 * VMCTX_CTL_POKE is performed by *this* process on the context's address space,
 * so it does not go through the context's own fault path: the kernel's redirect
 * fires for faults taken by a task that is a context, and this process is not
 * one. A poke at an address the context has no page for therefore does what any
 * write to absent anonymous memory does — the local kernel allocates a page of
 * zeros and the write lands in it. The page is present here from that moment
 * on, so this side never faults on that address again and never asks for it;
 * and the machine running the guest still has its own copy, which nobody took
 * away. Two holders, one of them mostly zeros, and neither ever hears about it.
 *
 * Measured, on gp1:
 *
 *   [pg 000129] 0x7fffffffe000 POKE signal frame len=552 pre=0 post=1
 *   [vmhome 0] write (0x1, 0x7fffffffe7f0, 0x68) -> 104
 *
 * — a page created here by the poke of a signal frame at offset 0x718, and one
 * hundred and four bytes read back out of offset 0x7f0 of it, which the guest
 * had written and this side had never seen. They went to stdout as 104 NULs,
 * ahead of the program's own output, which is exactly the symptom. No fault
 * appears between those two lines because there was nothing left to fault on.
 *
 * The fix is the rule the rest of the protocol already follows: to write a page
 * you must first own it, and owning it means taking it from the other side.
 * That is the pull ctx_monitor() already performs on a fault, so this does the
 * same thing for the same reason — and after the poke the guest's next touch
 * faults, asks with VMR_OP_CTXPAGE, and is handed the page *including* what was
 * written into it. A signal frame reaches the program that way for the first
 * time; before this it was written into a page the program was not holding.
 *
 * ABSENT is not a failure: it means the guest has never touched the address, so
 * there is nothing to lose and the local kernel's zeros are what its own kernel
 * would have given it.
 */
static void sh_file_forget(pid_t pid, uint64_t addr, uint64_t len);

/*
 * The maps line covering an address, verbatim, for the failed-install
 * diagnostic below. "POKE failed" alone cannot distinguish its three causes --
 * no VMA there at all, a VMA without write permission (which FOLL_FORCE cannot
 * override for a MAP_SHARED mapping: there is no private copy to make), and a
 * writable VMA whose fault the kernel still refused -- and the three repairs
 * live in three different places.
 */
static int ctx_maps_line(pid_t pid, uint64_t addr, char *out, size_t outlen)
{
	char path[64], line[512];
	FILE *f;

	snprintf(path, sizeof(path), "/proc/%d/maps", (int)pid);
	f = fopen(path, "r");
	if (!f)
		return -1;
	while (fgets(line, sizeof(line), f)) {
		unsigned long a, b;

		if (sscanf(line, "%lx-%lx", &a, &b) != 2)
			continue;
		if (addr >= a && addr < b) {
			size_t l = strlen(line);

			if (l && line[l - 1] == '\n')
				line[l - 1] = 0;
			snprintf(out, outlen, "%s", line);
			fclose(f);
			return 1;
		}
	}
	fclose(f);
	snprintf(out, outlen, "(no mapping)");
	return 0;
}

static long n_pull_diag;

static __thread int pull_fd = -1;

static int ctx_pull_page(pid_t pid, uint64_t base)
{
	struct ctx_route rt;
	char page[VMR_PG_SIZE];
	long pr;
	int got, prior;

	if (!ctx_route_find(pid, &rt))
		return 0;	/* no machine to ask: nothing was ever lent */

	if (pull_fd < 0)
		pull_fd = pg_connect(rt.peer, rt.port + VMR_PG_PORT_OFFSET);
	if (pull_fd < 0)
		return 0;

	/*
	 * Marked before the request goes out; see ctx_monitor(). This is the
	 * one pull the kernel's fault hook does not claim for -- it is this
	 * process's own, for a write it is about to make -- so the claim is
	 * written here and settled here by the outcome.
	 */
	prior = pg_state(pid, base);
	pg_set(pid, base, PG_CLAIM);
	got = pg_get_op(pull_fd, VMR_PG_GET, base, page, rt.ctx, pid);
	if (got < 0) {
		pg_set(pid, base, prior == PG_CLAIM ? PG_NONE : prior);
		close(pull_fd);
		pull_fd = -1;
		return 0;
	}
	if (got == PG_GOT_LANDED) {
		/*
		 * The page landed here by another path while the far side was
		 * answering INFLIGHT: it is present, which is all the caller's
		 * write needs.
		 */
		plog(base, "PULL for a write: landed here by another path "
		     "while the ask was INFLIGHT; nothing to install");
		if (pg_state(pid, base) == PG_CLAIM)
			pg_set(pid, base, PG_OURS);
		return 1;
	}
	if (got == VMR_PG_ABSENT) {
		plog(base, "PULL for a write: the guest never touched it");
		pg_set(pid, base, PG_GONE(prior) ? prior : PG_OURS);
		return 0;
	}

	pr = ctx_land(pid, base, page, last_get_gen, 0);
	if (pr != (long)VMR_PG_SIZE) {
		/*
		 * The take has already happened: the guest's machine gave the
		 * page up to answer this pull, so the bytes in `page` are now
		 * the ONLY copy in existence. A page belongs to the ADDRESS
		 * SPACE, not to the context this caller happened to name --
		 * every service context of one guest process shares an mm
		 * here -- so the install goes through any live sibling (the
		 * commonest caller at this moment is the teardown write for a
		 * DYING context, so the named context failing is ordinary).
		 */
		pid_t alt = as_live_ctx(pid, pid);

		if (n_pull_diag++ < 24) {
			char ml[512];

			ctx_maps_line(pid, base, ml, sizeof(ml));
			fprintf(stderr, "[vmhome] LAND-DIAG ctx %d 0x%llx: "
				"ret=%ld (%s) maps: %s\n",
				(int)pid, (unsigned long long)base, pr,
				pr < 0 ? strerror((int)-pr) : "-", ml);
		}
		if (alt > 0 && ctx_land(alt, base, page, last_get_gen, 0) ==
		    (long)VMR_PG_SIZE) {
			n_pull_via_sibling++;
			plog(base, "PULL for a write: taken from the guest and "
			     "installed through live sibling %d (context %d "
			     "could not take it)", (int)alt, (int)pid);
			return 1;
		}
		/*
		 * Neither the named context nor a sibling can take the install,
		 * so UNDO THE TAKE: give the page back to the machine it came
		 * from. Its PUT pokes the bytes into the address space's object
		 * and clears the loan, so the guest's next fault finds them.
		 * Only while these bytes are still the only copy: the record
		 * still reads this side's CLAIM, so no serve has moved it.
		 */
		if (pull_fd >= 0 && pg_state(pid, base) == PG_CLAIM &&
		    pg_put(pull_fd, base, page, rt.ctx) == 0) {
			n_pull_returned++;
			pg_set(pid, base, PG_THEIRS);
			/*
			 * And stop asking: a pull whose install cannot land
			 * here (the service mm has no mapping -- shmat is the
			 * one address-space change that does not propagate)
			 * will fail identically every time, and the shared-file
			 * reclaim re-pulling it before every forwarded call is
			 * the netsurf one-page livelock.
			 */
			sh_file_forget(pid, base, VMR_PG_SIZE);
			plog(base, "PULL for a write: could not install; the "
			     "page was GIVEN BACK to the guest's machine");
			fprintf(stderr, "[vmhome] context %d: took 0x%llx for "
				"a write and could not install it anywhere "
				"here; gave it back to the guest's machine "
				"rather than dropping the only copy\n",
				(int)pid, (unsigned long long)base);
			return 0;
		}
		n_pull_lost++;
		fprintf(stderr, "[vmhome] context %d: took 0x%llx from the "
			"guest for a write and could not put it back (%s), and "
			"no live sibling could take it either -- the only copy "
			"of that page is being dropped; the guest's machine "
			"can serve its retained copy if asked again\n",
			(int)pid, (unsigned long long)base,
			strerror((int)-pr));
		return 0;
	}
	plog(base, "PULL for a write: taken from the guest and installed");
	return 1;
}

/*
 * Poke, having first taken every page the poke will touch.
 *
 * Unconditional rather than "only if this side has no page there". Whether a
 * page present here is one this side still owns is exactly the question the
 * whole protocol exists to answer, and answering it by looking at a page table
 * would be an assumption; a pull costs one round trip and settles it.
 */
static long ctx_poke_owned(pid_t pid, uint64_t addr, const void *buf,
			   uint64_t len)
{
	uint64_t p, last;

	if (len == 0)
		return 0;
	last = (addr + len - 1) & ~(uint64_t)(VMR_PG_SIZE - 1);
	for (p = addr & ~(uint64_t)(VMR_PG_SIZE - 1); p <= last;
	     p += VMR_PG_SIZE)
		ctx_pull_page(pid, p);
	return ctx_poke(pid, addr, buf, len);
}

/*
 * A page of the context's own memory to put an answer in.
 *
 * Some of what the owner has to know about a program is only reachable through
 * a syscall that writes it somewhere — the disposition of a signal, the size of
 * a shared segment, the identity of a mapped file. The call is made by the
 * context, so the somewhere has to be in the context's address space; this is
 * that somewhere, mapped once per context and remembered.
 */
struct ctx_ent {
	pid_t    pid;
	uint64_t scratch;
};
static struct ctx_ent ctx_ents[512];
static int ctx_nents;
static pthread_mutex_t ctx_ent_lock = PTHREAD_MUTEX_INITIALIZER;

#define CTX_SCRATCH_LEN 8192

static uint64_t ctx_scratch(pid_t pid)
{
	uint64_t got;
	long r;
	int i;

	pthread_mutex_lock(&ctx_ent_lock);
	for (i = 0; i < ctx_nents; i++)
		if (ctx_ents[i].pid == pid) {
			got = ctx_ents[i].scratch;
			pthread_mutex_unlock(&ctx_ent_lock);
			return got;
		}
	pthread_mutex_unlock(&ctx_ent_lock);

	r = ctx_sys(pid, SYS_mmap, 0, CTX_SCRATCH_LEN, PROT_READ | PROT_WRITE,
		    MAP_PRIVATE | MAP_ANONYMOUS, (uint64_t)-1, 0);
	if (r < 0 || (unsigned long)r >= (unsigned long)-4095L)
		return 0;
	got = (uint64_t)r;

	pthread_mutex_lock(&ctx_ent_lock);
	if (ctx_nents < (int)(sizeof(ctx_ents) / sizeof(ctx_ents[0]))) {
		ctx_ents[ctx_nents].pid = pid;
		ctx_ents[ctx_nents].scratch = got;
		ctx_nents++;
	}
	pthread_mutex_unlock(&ctx_ent_lock);
	return got;
}

static void ctx_scratch_forget(pid_t pid)
{
	int i;

	pthread_mutex_lock(&ctx_ent_lock);
	for (i = 0; i < ctx_nents; i++)
		if (ctx_ents[i].pid == pid) {
			ctx_ents[i] = ctx_ents[--ctx_nents];
			break;
		}
	pthread_mutex_unlock(&ctx_ent_lock);
}

/* ---- what this process is holding ------------------------------------- */

/* The service context that holds the program: the first one, and its parent. */
static pid_t ctx0_pid;
static int   ctx0_mem = -1;	/* /proc/<ctx0>/mem, for the program as loaded */

/*
 * Hide the vDSO from the FIRST image: its auxv's AT_SYSINFO_EHDR becomes
 * AT_IGNORE before the loader parses it, and [vdso]/[vvar]/[vvar_vclock] are
 * dropped from the layout the destination gets -- so libc answers every clock
 * with the real (forwarded) syscall. Correct on ANY destination at a round
 * trip per clock read; the fast path is the destination serving its OWN vvar
 * (vmremote's vvar_emul), which needs the two kernels' vdso data layouts to
 * match. So this turns on exactly when that one cannot: VMCTX_DEST_RELEASE
 * (the destination's `uname -r`, set by the cross-machine harness) differs
 * from this machine's own.
 *
 * FIRST image only, deliberately. A context that EXECs gets a fresh vDSO and
 * a fresh auxv from this kernel, nothing patches those, and its libc WILL
 * jump into the vDSO -- so its [vdso]/[vvar] must stay in the relayout it
 * asks for, or the jump lands in a range no region describes and the guest
 * dies there. An exec'd image on a mismatched destination therefore still
 * reads the frozen vvar (a known gap, HANDOFF.md §43); the fix for that
 * shape is patching auxv at the exec stop, not serving a wrong-layout page.
 */
static int hide_vdso(void)
{
	static int decided = -1;
	const char *dest;
	struct utsname u;

	if (decided >= 0)
		return decided;
	/* The release fact decides; the VMCTX_HIDE_VDSO force went with the
	 * loopback verification it existed for (vd1 under forced hiding). */
	dest = getenv("VMCTX_DEST_RELEASE");
	if (dest && uname(&u) == 0)
		decided = strcmp(dest, u.release) != 0;
	else
		decided = 0;
	if (decided)
		fprintf(stderr, "[vmhome] hiding the vDSO from the first "
			"image (destination release %s): clocks go by "
			"forwarded syscall\n", dest ? dest : "forced");
	return decided;
}

/*
 * How many guest contexts are still being served. The service contexts are the
 * only copy of the program's memory, and every context pages from one — so the
 * first context to finish must not take them away from the rest.
 */
static int n_contexts;
static pthread_mutex_t ctx_count_lock = PTHREAD_MUTEX_INITIALIZER;

/*
 * Which service context serves which guest context.
 *
 * A guest thread arrives on a connection of its own and names only the context
 * it shares memory with, so the leader has to be findable by that name — the
 * thread must become a thread of the leader's task and of no other, or it waits
 * on a futex nobody wakes and calls descriptors by numbers that mean something
 * else.
 */
struct ctx_map { uint64_t ctx; pid_t pid; };
static struct ctx_map ctx_map_tab[512];
static int ctx_map_n;
static pthread_mutex_t ctx_map_lock = PTHREAD_MUTEX_INITIALIZER;

static void ctx_map_add(uint64_t ctx, pid_t pid)
{
	int i;

	if (!ctx)
		return;
	pthread_mutex_lock(&ctx_map_lock);
	for (i = 0; i < ctx_map_n; i++)
		if (ctx_map_tab[i].ctx == ctx) {
			ctx_map_tab[i].pid = pid;
			pthread_mutex_unlock(&ctx_map_lock);
			return;
		}
	if (ctx_map_n < (int)(sizeof(ctx_map_tab) / sizeof(ctx_map_tab[0]))) {
		ctx_map_tab[ctx_map_n].ctx = ctx;
		ctx_map_tab[ctx_map_n].pid = pid;
		ctx_map_n++;
	}
	pthread_mutex_unlock(&ctx_map_lock);
}

static void ctx_map_forget(uint64_t ctx)
{
	int i;

	pthread_mutex_lock(&ctx_map_lock);
	for (i = 0; i < ctx_map_n; i++)
		if (ctx_map_tab[i].ctx == ctx) {
			ctx_map_tab[i] = ctx_map_tab[--ctx_map_n];
			break;
		}
	pthread_mutex_unlock(&ctx_map_lock);
}


/* This connection's context. */
static __thread uint64_t conn_ctx;
static __thread pid_t    conn_ctxpid;
static __thread int      conn_is_thread;
static __thread uint64_t conn_ctid;	/* the clone's clear-tid word, if a thread */
static __thread int      conn_guest_status;
static __thread int      conn_have_status;
static __thread char     last_path[96];

/* What the last forwarded call said about memory it mapped. */
static __thread uint32_t last_map_shared;
static __thread uint64_t last_map_off;
static __thread uint64_t last_map_len;

static int g_listen_fd = -1;
/*
 * Which machine is running the guest, and on which port this vmhome answers.
 * Its page service is that port plus one — the connection is made outwards from
 * here, because the machine that owns the memory listens and the machine that
 * wants a page asks.
 */
static int g_port = 9999;
static __thread char conn_peer[64];

static void shlog(const char *fmt, ...)
{
	va_list ap;

	fprintf(stderr, "[vmhome %llu] ", (unsigned long long)conn_ctx);
	va_start(ap, fmt);
	vfprintf(stderr, fmt, ap);
	va_end(ap);
}

/* ---- the program's own address space: the kernel's to judge ---------- */

/*
 * Ask the context's own kernel whether it would handle an access at this
 * address -- the only authority there is on what the address space's shape
 * makes reachable. The record of where pages ARE is consulted before this;
 * an address is declared faulting only when the record does not answer AND
 * the kernel says it cannot handle the access. 0 = handleable (the address
 * is real; the ordinary ABSENT path serves the page), -EACCES = a mapping
 * forbids the access, -EFAULT = no mapping reaches it, -ESRCH = the task is
 * gone (ask a live sibling -- a page belongs to the address space).
 *
 * An older kernel without the op answers EINVAL; that is said out loud ONCE
 * and returned as -EFAULT, because a loud wrong answer on a stale kernel is
 * findable and a silent fallback to parsing maps text is the defect this
 * replaced.
 */
static long ctx_tryfault(pid_t pid, uint64_t addr, int write)
{
	struct vmctx_mem m = { .addr = addr, .len = write ? 2 : 0, .buf = 0 };
	/*
	 * EINVAL is two different sentences and only one of them is about the
	 * kernel. From a live context it means the op is not compiled in (an
	 * unported kernel: every ctl answers EINVAL for an unknown cmd). From
	 * a context whose task has already detached its vmctx on the way out
	 * -- or whose pid has been REUSED by some unrelated process -- it
	 * means "not a VM context", the same race ESRCH names one step later
	 * (vmremote documents the pair at its ATTACH retry). Latching "no
	 * such op" off the second sentence EFAULTed every later fault of a
	 * LIVE address space: netsurf died 242 executing libgtk text that
	 * both machines still held, because one exiting twin's pid answered
	 * EINVAL and the latch poisoned the whole run. So the latch is only
	 * believed while TRYFAULT has never once succeeded on this kernel;
	 * after that, EINVAL is the context gone, and the caller's sibling
	 * routing owns it -- a page belongs to the address space.
	 */
	static int proven;

	if (ctl(pid, VMCTX_CTL_TRYFAULT, &m) == 0) {
		proven = 1;
		return 0;
	}
	if (errno == EINVAL) {
		static int said;

		if (proven)
			return -ESRCH;
		if (!said++)
			fprintf(stderr, "[vmhome] this kernel has no "
				"VMCTX_CTL_TRYFAULT: every unheld address "
				"will be ruled EFAULT. Port the kernel "
				"patches before trusting this run.\n");
		return -EFAULT;
	}
	if (errno == EACCES || errno == EFAULT)
		proven = 1;	/* the op ran and judged the address */
	return -errno;
}

/* ---- exceptions -------------------------------------------------------- */

/*
 * The frame this owner pushes. Laid out to match what its own kernel builds,
 * because the program's handler may read the context out of it and its libc
 * returns through it — so the shape is the owner's, not this protocol's.
 */
struct sh_sigctx {
	uint64_t r8, r9, r10, r11, r12, r13, r14, r15;
	uint64_t di, si, bp, bx, dx, ax, cx, sp, ip, flags;
	uint16_t cs, gs, fs, ss;
	uint64_t err, trapno, oldmask, cr2;
	uint64_t fpstate;
	uint64_t reserved1[8];
};
struct sh_ucontext {
	uint64_t uc_flags;
	uint64_t uc_link;
	uint64_t uc_stack_sp;
	uint32_t uc_stack_flags;
	uint32_t _pad;
	uint64_t uc_stack_size;
	struct sh_sigctx mc;
	uint64_t uc_sigmask[16];
};
/*
 * Field names deliberately not si_*: <signal.h> defines si_addr, si_code and
 * friends as macros onto an anonymous union, so a struct using those names does
 * not compile and the error points at the header rather than at this.
 */
struct sh_siginfo {
	int32_t sf_signo, sf_errno, sf_code;
	int32_t _pad0;
	uint64_t sf_addr;
	uint64_t _rest[12];
};
struct sh_rt_frame {
	uint64_t pretcode;
	struct sh_ucontext uc;
	struct sh_siginfo info;
};

/*
 * The kernel's struct sigaction, which is not libc's: rt_sigaction is issued by
 * number here, so what comes back is the kernel's layout.
 */
struct k_sigaction {
	uint64_t handler;
	uint64_t flags;
	uint64_t restorer;
	uint64_t mask;
};

struct k_stack {
	uint64_t ss_sp;
	int32_t  ss_flags;
	int32_t  _pad;
	uint64_t ss_size;
};

/*
 * The owner's answer to an exception the guest raised.
 *
 * This is the one place that knows what a hardware vector means, and it is on
 * the side that owns the program because the meaning is that side's operating
 * system's. The machine running the guest reports "vector 14, error 6, address
 * X" and nothing more; a different owner would map the same report differently,
 * and would build a different thing on the stack.
 *
 * What the program asked for is read out of the context, by asking the context
 * — its rt_sigaction was performed there, so the disposition is there and
 * nowhere else. That also removes an old trap: the disposition used to be read
 * out of the shadow, which had vmhome's own crash handlers installed, so a
 * program that had registered nothing looked like a program with a handler and
 * was resumed inside vmhome's text.
 *
 * Returns 1 with *out filled when the program should carry on somewhere else,
 * 0 when it has no handler and the fault is fatal, VMR_EXC_LEGAL (2) when
 * the program's own kernel would have handled the access -- no exception.
 */
/* Exceptions the guest's machine reported that the program's own kernel
 * would have handled (TRYFAULT 0): answered VMR_EXC_LEGAL, never a signal. */
static unsigned long n_exc_legal;

static int ctx_deliver_exception(pid_t pid, uint64_t vec, uint64_t err,
				 uint64_t addr, const struct vmr_uregs *in,
				 struct vmr_uregs *out)
{
	struct k_sigaction sa;
	struct sh_rt_frame f;
	unsigned long sp, frame;
	uint64_t scratch;
	int sig, code;
	long r;

	switch (vec) {
	/*
	 * Not from the reported error's present bit: that bit is the other
	 * machine's page tables talking about itself. The kernel's own
	 * verdict on the access (ctx_tryfault) is the classification:
	 * "a mapping forbids it" is ACCERR, "no mapping reaches it" is
	 * MAPERR, in the fault handler's own words rather than a reading of
	 * maps text.
	 *
	 * And 0 -- the kernel WOULD handle the access -- is not an exception
	 * at all, and must not become one. This comment used to say a
	 * handleable answer "cannot arrive here" and mapped it to ACCERR
	 * anyway; it arrives from the guest's machine whenever a hand-over
	 * has left a page write-protected in a sibling context's page table
	 * after the sibling that pulled it back re-granted only its own, and
	 * the next store there is a present-page write fault on a page
	 * nobody holds (VMR_EXC_LEGAL in vmrproto.h). Treated as fatal, it
	 * killed the service process with a SIGSEGV that sat pending until
	 * the next return to user mode, and every forwarded call in between
	 * failed -EINVAL: hx2's thread-descriptor pages, 11 of 48 pool runs.
	 * The uapi rule for TRYFAULT is exactly this: the owner may declare
	 * an address faulting only after its kernel has refused the access.
	 * A context that is gone is asked through a live sibling, because the
	 * address space is what is being asked about.
	 */
	case 14: {
		int write = (int)((err >> 1) & 1);
		long tf = ctx_tryfault(pid, addr, write);

		if (tf == -ESRCH) {
			pid_t alt = as_live_ctx(pid, pid);

			if (alt > 0)
				tf = ctx_tryfault(alt, addr, write);
		}
		/*
		 * LEGAL only in the one shape the protocol can manufacture: a
		 * WRITE to a PRESENT page (err bits 0 and 1), which is what a
		 * hand-over's write-protection looks like from the guest. A
		 * fetch (bit 4) or a read on a present page is never the
		 * protocol's doing -- it write-protects and nothing else -- and
		 * TRYFAULT does not model NX at all, so for those the kernel's
		 * "would handle a read" must not be read as "would execute":
		 * pf2's no-exec and pf4's stack-exec cases re-faulted for ever
		 * on exactly that (chain s40f, 2 suite failures).
		 */
		if (tf == 0 && (err & 0x13) == 0x3) {
			n_exc_legal++;
			if (n_exc_legal <= 8)
				shlog("exception 14 at 0x%llx (err 0x%llx, rip "
				      "0x%llx) is NOT the program's: its kernel "
				      "would handle the %s; answering LEGAL so "
				      "the guest's side grants it\n",
				      (unsigned long long)addr,
				      (unsigned long long)err,
				      (unsigned long long)in->rip,
				      write ? "write" : "read");
			return VMR_EXC_LEGAL;
		}
		sig = SIGSEGV;
		code = tf == -EFAULT ? SEGV_MAPERR : SEGV_ACCERR;
		break;
	}
	case 13: sig = SIGSEGV; code = SI_KERNEL; break;
	case 6:  sig = SIGILL;  code = ILL_ILLOPN; break;
	case 0:  sig = SIGFPE;  code = FPE_INTDIV; break;
	case 3:  sig = SIGTRAP; code = TRAP_BRKPT; break;
	default: sig = SIGSEGV; code = SI_KERNEL; break;
	}

	{
		uint64_t t0 = now_us(), t1;

		scratch = ctx_scratch(pid);
		t1 = now_us();
		if (t1 - t0 > 300000)
			shlog("SLOW exception step: ctx_scratch took %llu ms\n",
			      (unsigned long long)((t1 - t0) / 1000));
	}
	if (!scratch)
		return 0;

	memset(&sa, 0, sizeof(sa));
	{
		uint64_t t0 = now_us(), t1;

		r = ctx_sys(pid, SYS_rt_sigaction, (uint64_t)sig, 0, scratch,
			    8, 0, 0);
		if (r == 0)
			ctx_peek(pid, scratch, &sa, sizeof(sa));
		t1 = now_us();
		if (t1 - t0 > 300000)
			shlog("SLOW exception step: disposition read took %llu "
			      "ms (r=%ld)\n",
			      (unsigned long long)((t1 - t0) / 1000), r);
	}

	/*
	 * SIG_DFL and SIG_IGN are both fatal here, and SIG_IGN deliberately: a
	 * synchronous fault cannot be ignored, because returning would
	 * re-execute the faulting instruction and fault again for ever, which
	 * is why the kernel resets an ignored SIGSEGV to default rather than
	 * honouring it.
	 */
	if (sa.handler == 0 || sa.handler == 1) {
		shlog("exception %llu at 0x%llx becomes signal %d, which the "
		      "program has not asked to handle; it is fatal "
		      "(rip 0x%llx rsp 0x%llx err 0x%llx)\n",
		      (unsigned long long)vec, (unsigned long long)addr, sig,
		      (unsigned long long)(in ? in->rip : 0),
		      (unsigned long long)(in ? in->rsp : 0),
		      (unsigned long long)err);
		/*
		 * THIS side's own map at the ruled address, and the same line
		 * for the fork parent when there is one. The ruling rests
		 * entirely on what this task's mm holds there; when the guest
		 * dies byte-identically at one address across every fork child
		 * (firefox, 0x7ffff7fb6188), the question is whether the
		 * child's mm really lacks a VMA its parent's initial layout
		 * carried -- and only these two lines can answer it.
		 */
		{
			char ml[512];
			pid_t par = fork_rel_parent(pid);

			ctx_maps_line(pid, addr, ml, sizeof(ml));
			shlog("  this task's own map there: %s\n", ml);
			if (par > 0) {
				ctx_maps_line(par, addr, ml, sizeof(ml));
				shlog("  its fork parent %d's map there: %s\n",
				      (int)par, ml);
			}
		}
		/*
		 * And what the stack under it holds, because the register set
		 * alone has never been enough to say WHY.
		 *
		 * A fault at 0, or at a small offset from it, is a pointer that
		 * should have been valid and read as zero -- and the place it
		 * was read FROM is the stack. That is the page that was actually
		 * lost, and nothing else in this system is in a position to
		 * photograph it: the program cannot, because saying anything
		 * costs a forwarded syscall through the machinery that is
		 * failing. Measured, when a test tried: the handler was entered
		 * on its own alternate stack and still got no word out, while
		 * four contexts died around it.
		 *
		 * Present pages only, and the peek is the clamped one, so this
		 * cannot invent the memory it is reporting on -- which is the
		 * exact defect this instrument exists downstream of.
		 */
		if (in && in->rsp) {
			uint64_t sp = in->rsp & ~7ULL;
			uint64_t base = sp & ~(uint64_t)(VMR_PG_SIZE - 1);
			int here = ctx_present(pid, base);

			shlog("  the stack under it (0x%llx, page %s here):\n",
			      (unsigned long long)sp,
			      here == 1 ? "IS" : here == 0 ? "is NOT" : "unknown");
			if (here == 1) {
				uint64_t w[8];
				long g = ctx_peek(pid, sp, w, sizeof(w));
				int i, n = (int)(g / 8);

				for (i = 0; i < n; i++)
					shlog("    [rsp+0x%02x] = 0x%016llx%s\n",
					      i * 8, (unsigned long long)w[i],
					      w[i] == 0 ? "   <- ZERO" : "");
				if (n < 8)
					shlog("    (only %d word(s) readable: the "
					      "rest of the stack is not on this "
					      "machine)\n", n);
			}
		}
		/*
		 * And fatal means the context dies of it, here, now.
		 *
		 * The context *is* the program as far as this machine is
		 * concerned: it holds the pid, and the parent's wait4 is
		 * forwarded here. So the only way the program's parent can see
		 * "killed by SIGSEGV" is for this task to be killed by SIGSEGV.
		 * Set the disposition back to default first, in case the
		 * program had asked for it to be ignored.
		 */
		memset(&sa, 0, sizeof(sa));
		{
			uint64_t t0 = now_us(), t1;

			if (ctx_poke(pid, scratch, &sa, sizeof(sa)) > 0)
				ctx_sys(pid, SYS_rt_sigaction, (uint64_t)sig,
					scratch, 0, 8, 0, 0);
			t1 = now_us();
			if (t1 - t0 > 300000)
				shlog("SLOW exception step: disposition reset "
				      "took %llu ms\n",
				      (unsigned long long)((t1 - t0) / 1000));
		}
		fflush(NULL);
		kill(pid, sig);
		/*
		 * No nudge after the kill. One was tried here -- an assisted
		 * getpid to walk the service task through signal delivery,
		 * on the theory that pf5's ~4 s per fatal child was this
		 * kill sitting in the shared pending set -- and it made pf3
		 * three times SLOWER (12.5 s -> 42 s): the theory was wrong.
		 * The 4 s was a COWBREAK refusal answered without its page
		 * body (vmremote's one header-only GET-shaped reply), the
		 * asker blocked in read() until the socket bound fired. The
		 * death itself is not what the caller waits on: the verdict
		 * goes back to the guest's machine and the teardown arrives
		 * through the channel, so a pending-until-next-delivery
		 * signal costs milliseconds here, not seconds.
		 */
		return 0;
	}

	/*
	 * Where the frame goes. Below the red zone, or on the alternate stack
	 * if the program registered one and asked for it — a handler for a
	 * stack overflow has nowhere else to run.
	 */
	sp = (unsigned long)in->rsp - 128;
	if (sa.flags & SA_ONSTACK) {
		struct k_stack ss;

		memset(&ss, 0, sizeof(ss));
		if (ctx_sys(pid, SYS_sigaltstack, 0, scratch, 0, 0, 0, 0) == 0 &&
		    ctx_peek(pid, scratch, &ss, sizeof(ss)) > 0 &&
		    ss.ss_sp && !(ss.ss_flags & SS_DISABLE))
			sp = (unsigned long)ss.ss_sp + ss.ss_size;
	}
	frame = ((sp - sizeof(struct sh_rt_frame)) & ~15UL) - 8;

	if (ctx_tryfault(pid, frame, 1) != 0 ||
	    ctx_tryfault(pid, frame + sizeof(f), 1) != 0) {
		shlog("no room for a signal frame at 0x%lx: it is outside the "
		      "program's own memory, so the fault stays fatal\n", frame);
		return 0;
	}

	memset(&f, 0, sizeof(f));
	f.pretcode = sa.restorer;
	f.uc.mc.r8 = in->r8;   f.uc.mc.r9  = in->r9;   f.uc.mc.r10 = in->r10;
	f.uc.mc.r11 = in->r11; f.uc.mc.r12 = in->r12;  f.uc.mc.r13 = in->r13;
	f.uc.mc.r14 = in->r14; f.uc.mc.r15 = in->r15;
	f.uc.mc.di = in->rdi;  f.uc.mc.si = in->rsi;   f.uc.mc.bp = in->rbp;
	f.uc.mc.bx = in->rbx;  f.uc.mc.dx = in->rdx;   f.uc.mc.ax = in->rax;
	f.uc.mc.cx = in->rcx;  f.uc.mc.sp = in->rsp;   f.uc.mc.ip = in->rip;
	f.uc.mc.flags = in->rflags;
	f.uc.mc.err = err;     f.uc.mc.trapno = vec;   f.uc.mc.cr2 = addr;
	f.info.sf_signo = sig;
	f.info.sf_code  = code;
	f.info.sf_addr  = addr;

	{
		int p0 = pglog_on ? ctx_present(pid, frame) : -1;
		int p1 = pglog_on ? ctx_present(pid, frame + sizeof(f)) : -1;

		if (ctx_poke_owned(pid, frame, &f, sizeof(f)) !=
		    (long)sizeof(f)) {
			shlog("could not put a signal frame at 0x%lx; the fault "
			      "stays fatal\n", frame);
			return 0;
		}
		plog(frame, "POKE signal frame len=%zu pre=%d post=%d",
		     sizeof(f), p0, ctx_present(pid, frame));
		if ((frame & ~4095UL) != ((frame + sizeof(f)) & ~4095UL))
			plog(frame + sizeof(f), "POKE signal frame tail pre=%d "
			     "post=%d", p1, ctx_present(pid, frame + sizeof(f)));
	}

	*out = *in;
	out->rsp = frame;
	out->rip = sa.handler;
	out->rdi = (uint64_t)sig;
	out->rsi = frame + offsetof(struct sh_rt_frame, info);
	out->rdx = frame + offsetof(struct sh_rt_frame, uc);
	out->rax = 0;
	shlog("exception %llu at 0x%llx becomes signal %d; entering the "
	      "program's handler at 0x%llx with a frame at 0x%lx\n",
	      (unsigned long long)vec, (unsigned long long)addr, sig,
	      (unsigned long long)out->rip, frame);
	return 1;
}

/* ---- shared mappings --------------------------------------------------- */

/*
 * One offset in the backing object per shared page, whoever maps it.
 *
 * The far side names memory by address and takes a guest address to be its own
 * offset in the object. That is what makes two contexts started with one object
 * see the same page at the same address — a thread, on a machine with no
 * CLONE_VM — and it is also why two addresses can never be one page there.
 * Shared memory is exactly two addresses being one page.
 *
 * So a mapping that something else can also reach is given an offset of its
 * own, derived from what makes it the same mapping: the file it is of, and how
 * far into that file it starts. Two mappings of one file page ask for the same
 * (device, inode, file offset) and are handed the same slot, and the far side
 * maps both addresses onto it without being told why.
 *
 * Interned here, in the one process that sees every connection, so two guest
 * processes that map the same file by name are asking the same table and get
 * the same answer whether they are related or not.
 *
 * Slots are allocated above every address a program can hold, so they cannot
 * collide with the ordinary address-is-offset rule sharing the same object.
 */
#define SH_SHARED_BASE	(1ULL << 47)

struct sh_shared_slot {
	uint64_t dev, ino, off, len, slot;
};
static struct sh_shared_slot sh_shared[256];
static int sh_shared_n;
static uint64_t sh_shared_next = SH_SHARED_BASE;
static pthread_mutex_t sh_shared_lock = PTHREAD_MUTEX_INITIALIZER;

static uint64_t sh_shared_slot(uint64_t dev, uint64_t ino, uint64_t foff,
			       uint64_t len)
{
	int i;
	uint64_t slot = 0;

	pthread_mutex_lock(&sh_shared_lock);
	for (i = 0; i < sh_shared_n; i++) {
		if (sh_shared[i].dev == dev && sh_shared[i].ino == ino &&
		    sh_shared[i].off == foff) {
			slot = sh_shared[i].slot;
			pthread_mutex_unlock(&sh_shared_lock);
			return slot;
		}
	}
	if (sh_shared_n == (int)(sizeof(sh_shared) / sizeof(sh_shared[0]))) {
		pthread_mutex_unlock(&sh_shared_lock);
		return 0;
	}
	len = (len + 0xfffUL) & ~0xfffUL;
	slot = sh_shared_next;
	sh_shared_next += len ? len : 0x1000;
	sh_shared[sh_shared_n].dev  = dev;
	sh_shared[sh_shared_n].ino  = ino;
	sh_shared[sh_shared_n].off  = foff;
	sh_shared[sh_shared_n].len  = len;
	sh_shared[sh_shared_n].slot = slot;
	sh_shared_n++;
	pthread_mutex_unlock(&sh_shared_lock);
	fprintf(stderr, "[vmhome] shared mapping of dev %lu ino %lu at offset "
		"%llu takes object offset 0x%llx (%llu bytes)\n",
		(unsigned long)dev, (unsigned long)ino,
		(unsigned long long)foff, (unsigned long long)slot,
		(unsigned long long)len);
	return slot;
}

/*
 * The ranges that are a file, and the one thing this side cannot learn about
 * them by faulting.
 *
 * Every other kind of memory reaches this side because something here touches
 * it: a syscall reads through a pointer, the context faults, and the page is
 * taken from the machine that has it. A MAP_SHARED file mapping has a second
 * way of being observed that no fault can catch -- the file. msync(2) writes
 * back this side's page cache without reading the mapping, munmap(2) throws
 * the mapping away, and a plain read(2) of the file afterwards goes nowhere
 * near the address. So the program's stores, which happened where its
 * instructions run, are still over there, and the file keeps whatever it held.
 *
 * Measured on tests/sm2.c's file-writeback case: the guest stored 0xdeadbeef
 * through the mapping, msync and munmap were forwarded and both succeeded, and
 * the read that followed returned 00000000 -- with not one fault on the range
 * anywhere in the run, because nothing on this side ever read it.
 *
 * So the ranges are remembered, and taken back before the two calls after
 * which the file can be observed. Taking a page back is the ordinary pull, and
 * putting it into the context is an ordinary poke -- which, for a mapping that
 * *is* the file, dirties the page cache and makes the bytes the file's. No new
 * mechanism and nothing the far side is told: it is asked for a page, as
 * always, and has no idea why.
 */
struct sh_file_range {
	uint64_t start, end;
	uint32_t as;		/* the ADDRESS SPACE the range belongs to.
				 * The table was global and keyed by nothing:
				 * a range recorded for one process was walked
				 * for every context of every other, and the
				 * same numeric address in a forked child -- a
				 * thread stack there -- was pulled back "for
				 * coherence" and dropped when the put-back
				 * failed. netsurf's one-page livelock, 88
				 * cycles in 5s, was exactly that. */
};
static struct sh_file_range sh_file[64];
static int sh_file_n;
static pthread_mutex_t sh_file_lock = PTHREAD_MUTEX_INITIALIZER;

/*
 * Ranges a forwarded mprotect() has just turned read-only -- RELRO, and every
 * other "write it, then seal it" the loader does.
 *
 * This is the whole of when a *read-only* file page's contents can differ from
 * the file: it was writable, the guest stored into it on the far machine, and
 * then it was sealed. A page that has been read-only since its mmap cannot have
 * diverged -- the guest can never have written it -- so the file here is always
 * its current bytes and the local kernel serves it, with no question asked.
 *
 * That distinction is what keeps the widened fault hook from pulling a running
 * program's text and rodata home on every forwarded syscall: only the handful
 * of pages the loader sealed are asked about, which is exactly the set the
 * Firefox bug lives in. pg1/pg2/th9 name no such range and are never disturbed.
 * The table and its counters are declared up with the other instruments.
 */
static void relro_note(uint64_t addr, uint64_t len)
{
	uint64_t start = addr & ~0xfffUL;
	uint64_t end = (addr + len + 0xfffUL) & ~0xfffUL;
	int i;

	if (!len || end <= start)
		return;
	pthread_mutex_lock(&relro_lock2);
	for (i = 0; i < relro_n; i++)
		if (relro[i].start == start && relro[i].end == end)
			goto out;
	if (relro_n < (int)(sizeof(relro) / sizeof(relro[0]))) {
		relro[relro_n].start = start;
		relro[relro_n].end   = end;
		relro_n++;
		n_relro_ranges++;
		plog(start, "RELRO range 0x%llx-0x%llx recorded: a read-only "
		     "file page here may now differ from the file, so faults in "
		     "it are asked of the guest's machine",
		     (unsigned long long)start, (unsigned long long)end);
	}
out:
	pthread_mutex_unlock(&relro_lock2);
}

static int relro_has(uint64_t addr)
{
	uint64_t p = addr & ~0xfffUL;
	int i, hit = 0;

	pthread_mutex_lock(&relro_lock2);
	for (i = 0; i < relro_n; i++)
		if (p >= relro[i].start && p < relro[i].end) {
			hit = 1;
			break;
		}
	pthread_mutex_unlock(&relro_lock2);
	return hit;
}

static void relro_forget(uint64_t addr, uint64_t len)
{
	uint64_t lo = addr & ~0xfffUL;
	uint64_t hi = (addr + len + 0xfffUL) & ~0xfffUL;
	int i;

	if (!len)
		return;
	pthread_mutex_lock(&relro_lock2);
	for (i = 0; i < relro_n; ) {
		if (relro[i].start < hi && relro[i].end > lo)
			relro[i] = relro[--relro_n];
		else
			i++;
	}
	pthread_mutex_unlock(&relro_lock2);
}

static void sh_file_add_locked(uint32_t as, uint64_t start, uint64_t end)
{
	if (sh_file_n < (int)(sizeof(sh_file) / sizeof(sh_file[0]))) {
		sh_file[sh_file_n].start = start;
		sh_file[sh_file_n].end   = end;
		sh_file[sh_file_n].as    = as;
		sh_file_n++;
	}
}

static void sh_file_note(pid_t pid, uint64_t addr, uint64_t len)
{
	uint64_t start = addr & ~0xfffUL;
	uint64_t end = (addr + len + 0xfffUL) & ~0xfffUL;
	uint32_t as = as_of(pid);
	int i;

	if (!len)
		return;
	pthread_mutex_lock(&sh_file_lock);
	for (i = 0; i < sh_file_n; i++)
		if (sh_file[i].as == as &&
		    sh_file[i].start == start && sh_file[i].end == end)
			goto out;
	if (sh_file_n < (int)(sizeof(sh_file) / sizeof(sh_file[0]))) {
		sh_file_add_locked(as, start, end);
		n_shfile_ranges++;
		plog(start, "shared file range 0x%llx-0x%llx recorded for as %u",
		     (unsigned long long)start, (unsigned long long)end, as);
	}
out:
	pthread_mutex_unlock(&sh_file_lock);
}

/*
 * And forget one, once the mapping it describes is gone. An address does not
 * stay a file: the next mmap may hand the same page out for something else,
 * and a range remembered past its munmap would have that one taken back from
 * the guest for no reason.
 */
static void sh_file_forget(pid_t pid, uint64_t addr, uint64_t len)
{
	uint64_t lo = addr & ~0xfffUL;
	uint64_t hi = (addr + len + 0xfffUL) & ~0xfffUL;
	uint32_t as = as_of(pid);
	int i;

	if (!len)
		return;
	pthread_mutex_lock(&sh_file_lock);
	/*
	 * Any OVERLAP goes, with the non-overlapping remainders kept -- the old
	 * full-containment test left a partially munmapped range covering freed
	 * addresses, and left everything on a MAP_FIXED reuse, which is how a
	 * dead range came to cover a live thread stack.
	 */
	for (i = 0; i < sh_file_n; ) {
		uint64_t s = sh_file[i].start, e = sh_file[i].end;

		if (sh_file[i].as != as || e <= lo || s >= hi) {
			i++;
			continue;
		}
		sh_file[i] = sh_file[--sh_file_n];
		if (s < lo)
			sh_file_add_locked(as, s, lo);
		if (e > hi)
			sh_file_add_locked(as, hi, e);
		/* re-examine slot i: the swapped-in entry may overlap too */
	}
	pthread_mutex_unlock(&sh_file_lock);
}

static int ctx_pull_page(pid_t pid, uint64_t base);

/*
 * Take back every page of a shared file mapping that the given range touches.
 *
 * Called before the call, not after: munmap has no range left to ask about
 * afterwards, and msync has already written whatever this side held.
 */
static void sh_file_reclaim(pid_t pid, uint64_t addr, uint64_t len)
{
	uint64_t lo = addr & ~0xfffUL;
	uint64_t hi = (addr + len + 0xfffUL) & ~0xfffUL;
	int i;

	if (!len || pid <= 0)
		return;
	for (i = 0; ; i++) {
		uint64_t s, e, p;

		pthread_mutex_lock(&sh_file_lock);
		if (i >= sh_file_n) {
			pthread_mutex_unlock(&sh_file_lock);
			break;
		}
		if (sh_file[i].as != as_of(pid)) {
			pthread_mutex_unlock(&sh_file_lock);
			continue;
		}
		s = sh_file[i].start;
		e = sh_file[i].end;
		pthread_mutex_unlock(&sh_file_lock);
		if (e <= lo || s >= hi)
			continue;
		if (s < lo)
			s = lo;
		if (e > hi)
			e = hi;
		for (p = s; p < e; p += VMR_PG_SIZE)
			ctx_pull_page(pid, p);
	}
}

/*
 * Every page of a shared file mapping the guest is holding, before this side
 * reads any of them.
 *
 * The reclaim above is called for msync(2) and munmap(2) by number, under a
 * comment admitting it is a list of the calls after which the contents "can be
 * seen without the mapping". That list is wrong in the way PRINCIPLES §4 says
 * such lists are always wrong -- silently, by omission. The contents can be
 * seen by *any* forwarded call whose pointer argument lands in the range, and
 * the shadow cannot discover it the way it discovers every other page: a
 * MAP_SHARED file VMA is excluded from the fault hook in mm/memory.c on the
 * argument that "the second is the file, so the file is the right answer",
 * which is true only until the guest stores into it. Measured on rd1
 * store-forward: the guest wrote "READTHIS" through the mapping, a forwarded
 * write(2) read the shadow's own mapping, and the pipe carried eight zeros.
 *
 * So the question is asked of the ownership record instead of of the call
 * number: a page whose own record says this side handed it to the guest is a
 * page whose current contents are over there, and it is pulled back before the
 * call runs. A page this side still holds costs nothing and is not touched, so
 * a program that never writes through a shared file mapping never pays for
 * this, and one that does pays one round trip per page per hand-over.
 *
 */
static void sh_file_reclaim_pull(pid_t pid, uint64_t page, void *arg)
{
	(void)arg;
	n_shfile_pages++;
	if (ctx_pull_page(pid, page))
		n_shfile_reclaimed++;
}

static void sh_file_reclaim_held(pid_t pid)
{
	uint32_t as;
	int i;

	if (pid <= 0)
		return;
	pthread_mutex_lock(&sh_file_lock);
	i = sh_file_n;
	pthread_mutex_unlock(&sh_file_lock);
	if (!i)
		return;			/* no shared file mapping exists */

	as = as_of(pid);
	n_shfile_calls++;
	for (i = 0; ; i++) {
		uint64_t s, e;

		pthread_mutex_lock(&sh_file_lock);
		if (i >= sh_file_n) {
			pthread_mutex_unlock(&sh_file_lock);
			break;
		}
		if (sh_file[i].as != as) {
			pthread_mutex_unlock(&sh_file_lock);
			continue;
		}
		s = sh_file[i].start;
		e = sh_file[i].end;
		pthread_mutex_unlock(&sh_file_lock);
		/*
		 * THEIRS and CLAIM are the two records that say the bytes are
		 * not here; the kernel lists exactly those pages of the range
		 * (VMCTX_CTL_PGSCAN), so a range the guest never wrote costs
		 * one scan and no round trip.
		 */
		pg_scan(pid, s, e, (1u << PG_THEIRS) | (1u << PG_CLAIM),
			sh_file_reclaim_pull, NULL);
	}
}

/* Is this address inside a range recorded as a shared file mapping? */
static int sh_file_has(pid_t pid, uint64_t addr)
{
	uint32_t as = as_of(pid);
	int i, yes = 0;

	pthread_mutex_lock(&sh_file_lock);
	for (i = 0; i < sh_file_n; i++)
		if (sh_file[i].as == as &&
		    addr >= sh_file[i].start && addr < sh_file[i].end) {
			yes = 1;
			break;
		}
	pthread_mutex_unlock(&sh_file_lock);
	return yes;
}

/*
 * Everything the shared-file reclaim did, said whether or not it did anything.
 *
 * Printed unconditionally: the statement this instrument exists to make is a
 * zero with a denominator under it, and a line that appears only when a counter
 * moved cannot make it.
 */
static void sh_file_report(void)
{
	fprintf(stderr, "[vmhome] shared file mappings: %lu range(s) recorded; "
		"the reclaim ran %lu time(s) over %lu page(s) -- %lu here "
		"(OURS), %lu handed over (THEIRS), %lu being claimed, %lu "
		"never held (NONE) -- and pulled %lu back\n",
		n_shfile_ranges, n_shfile_calls, n_shfile_pages,
		n_shfile_st[PG_OURS], n_shfile_st[PG_THEIRS],
		n_shfile_st[PG_CLAIM], n_shfile_st[PG_NONE],
		n_shfile_reclaimed);
	fprintf(stderr, "[vmhome] the guest's machine asked for %lu page(s) "
		"inside those ranges; %lu of them were in no movable range "
		"here, so no take was attempted and no hand-over could be "
		"recorded for them\n", n_shfile_served, n_shfile_nomov);
}

/*
 * Did this call map memory something else can also reach, and if so what is
 * that memory called?
 *
 * Worked out from the call's own arguments, in the context that made it: the
 * flags say MAP_SHARED, the descriptor says which file, and the segment id says
 * which segment. Nothing here is a guess about the address space afterwards.
 */
static void note_shared_mapping(pid_t pid, const struct vmr_req *rq, long ret)
{
	uint64_t scratch;

	last_map_shared = 0;
	last_map_off    = 0;
	last_map_len    = 0;

	if (ret < 0)
		return;

	if (rq->nr == 30) {			/* shmat */
		struct shmid_ds ds;
		long r;

		scratch = ctx_scratch(pid);
		if (!scratch) {
			shlog("shmat at 0x%llx: no scratch page to ask this "
			      "segment's size with, so the far side is not "
			      "told the range is shared\n",
			      (unsigned long long)ret);
			return;
		}
		memset(&ds, 0, sizeof(ds));
		/*
		 * IPC_STAT, with no IPC_64 beside it, and that one bit is the
		 * whole of why System V shared memory has never worked here.
		 *
		 * A 64-bit libc does add IPC_64 to the command, and this used
		 * to add it too, "or the kernel answers in the layout of a
		 * struct nothing here declares". On x86-64 that is exactly
		 * backwards: shmctl(2) is a direct syscall, its own
		 * SYSCALL_DEFINE3 passes IPC_64 as the version by itself, and
		 * ksys_shmctl() then switches on the command with no mask --
		 * so IPC_STAT|IPC_64 is 0x102, matches no case, and falls to
		 * default: -EINVAL. The libc's bit belongs to the ipc(2)
		 * multiplexer, which is a different entry point.
		 *
		 * Measured on this kernel, outside vmctx entirely:
		 *
		 *   IPC_STAT|IPC_64 -> -1 errno=22 segsz=0
		 *   IPC_STAT        ->  0 errno=0  segsz=4096
		 *
		 * The reply is a shmid64_ds either way, which on this
		 * architecture is the struct shmid_ds declared here.
		 *
		 * The failure was silent by construction: an early return with
		 * nothing logged, so every shmat was reported to the far side
		 * as a call that mapped nothing, and the two attachments of one
		 * segment became two unrelated pages. Now it says so.
		 */
		r = ctx_sys(pid, SYS_shmctl, rq->args[0], IPC_STAT, scratch,
			    0, 0, 0);
		if (r != 0) {
			shlog("shmat at 0x%llx: shmctl(IPC_STAT) on segment %lld "
			      "says %ld, so this side cannot name the segment "
			      "and the far side is not told the range is "
			      "shared\n", (unsigned long long)ret,
			      (long long)rq->args[0], r);
			return;
		}
		if (ctx_peek(pid, scratch, &ds, sizeof(ds)) <= 0 ||
		    !ds.shm_segsz) {
			shlog("shmat at 0x%llx: segment %lld reports no size\n",
			      (unsigned long long)ret, (long long)rq->args[0]);
			return;
		}
		/* A segment's identity is its id and nothing else. */
		last_map_shared = 1;
		last_map_len = ((uint64_t)ds.shm_segsz + 0xfffUL) & ~0xfffUL;
		last_map_off = sh_shared_slot(0, (uint64_t)(unsigned)rq->args[0],
					      0, last_map_len);
	} else if (rq->nr == 9 && (rq->args[3] & MAP_SHARED)) {	/* mmap */
		/*
		 * Shared either way, but only a file gives an offset to share
		 * *at*. Two mappings of one file are one page reached by two
		 * addresses, and that needs a slot both can name. Anonymous
		 * shared memory is never aliased within a process, so it needs
		 * no slot; what it needs is for the far side to know the range
		 * is shared at all, because after a fork two contexts hold it.
		 */
		struct stat st;

		last_map_shared = 1;
		last_map_len    = rq->args[1];
		if ((int)rq->args[4] < 0)
			return;
		scratch = ctx_scratch(pid);
		if (!scratch)
			return;
		memset(&st, 0, sizeof(st));
		if (ctx_sys(pid, SYS_fstat, rq->args[4], scratch, 0, 0, 0, 0) != 0)
			return;
		if (ctx_peek(pid, scratch, &st, sizeof(st)) <= 0)
			return;
		last_map_off = sh_shared_slot((uint64_t)st.st_dev,
					      (uint64_t)st.st_ino,
					      rq->args[5], last_map_len);
		/*
		 * And remember it as a file, which is the one thing about it
		 * that no fault on this side will ever reveal. See
		 * sh_file_reclaim().
		 *
		 * Only if the mapping is WRITABLE. The registry exists to pull
		 * back pages whose contents the guest can change, and a
		 * MAP_SHARED range without PROT_WRITE cannot diverge: no store
		 * can go through it on either machine, and GUP will not
		 * force-write a shared mapping (there is no private copy to
		 * make), so the reclaim's install can never land here at all.
		 * Recording one anyway is what ate netsurf: every fontconfig
		 * cache is mmap(PROT_READ, MAP_SHARED), the copy-serve stamped
		 * its pages THEIRS through this record, and the reclaim then
		 * took each page from the guest before every forwarded call,
		 * failed the install (POKE-DIAG: ret=0 on "r--s"), and gave it
		 * back -- for ever. The maps-scan half of this registry
		 * (ctx_movable_ranges) has required "w" all along; this is the
		 * same filter at the mmap that the scan would apply one serve
		 * later. A later mprotect(PROT_WRITE) is caught by that scan.
		 */
		if (last_map_off && (rq->args[2] & PROT_WRITE))
			sh_file_note(pid, (uint64_t)ret, last_map_len);
	} else {
		return;
	}

	/*
	 * No slot left is no sharing: reported as shared with no offset the far
	 * side would read it as anonymous shared memory and give it the address
	 * as an offset, which is a different page in every process. Say nothing
	 * rather than something wrong.
	 */
	if (last_map_shared && !last_map_off && last_map_len &&
	    ((rq->nr == 30) || (rq->nr == 9 && (int)rq->args[4] >= 0))) {
		last_map_shared = 0;
		last_map_len    = 0;
	}
}

/* ---- forwarding -------------------------------------------------------- */

/*
 * The four calls that create a task. The destination forwards these like any
 * other syscall now, and the source runs the real one -- so its own kernel
 * parses CLONE_VM/CLONE_THREAD/stack/tls/ctid, makes the child (a thread of
 * this context or a new process), and parks it as a service context through the
 * fork hook every context is born from. See the clone reply built in serve().
 */
static int is_clone_nr(uint64_t nr)
{
	return nr == 56 /* clone */ || nr == 57 /* fork */ ||
	       nr == 58 /* vfork */ || nr == 435 /* clone3 */;
}

/*
 * One forwarded syscall: one VMCTX_CTL_SYSCALL against the task that owns the
 * program.
 *
 * The machine state comes with the call and goes back with the answer. It is
 * loaded into the context first because some calls are about it — rt_sigreturn
 * reads the frame the program is standing on, so it needs the program's stack
 * pointer, and a call that touches thread-local storage needs the program's FS
 * base. Nothing here knows which calls those are; the state simply travels with
 * the call, as it does in the other direction.
 *
 * What the call did to the machine state comes back as a mask. Almost every
 * call reports RAX and nothing else, and is answered with a return value;
 * anything more than RAX is a call that changed where the program is, and the
 * register set goes back so the far side can carry on with it.
 */
static long ctx_forward(pid_t pid, const struct vmr_req *rq,
			const struct vmr_uregs *in, struct vmr_uregs *out,
			int *out_valid, int verbose)
{
	struct vmctx_syscall c;

	last_path[0] = '\0';
	*out_valid = 0;

	if (pid <= 0)
		return -ENOSYS;

	if (in)
		ctl(pid, VMCTX_CTL_SETREGS, (void *)in);

	/*
	 * msync(2) and munmap(2) are the two calls after which a shared file
	 * mapping's contents can be seen without the mapping. Nothing here
	 * knows what either does beyond that -- both name a range, and a range
	 * that is a file has to be here before it is published. See
	 * sh_file_reclaim().
	 */
	if (rq->nr == SYS_msync || rq->nr == SYS_munmap)
		sh_file_reclaim(pid, rq->args[0], rq->args[1]);
	/*
	 * ...and the same thing without the list of calls. See
	 * sh_file_reclaim_held(): the two above are the ones after which the
	 * range can be read *without* the mapping, and every call at all can
	 * read it *through* one.
	 */
	sh_file_reclaim_held(pid);

	memset(&c, 0, sizeof(c));
	c.nr = rq->nr;
	memcpy(c.args, rq->args, sizeof(c.args));
	/*
	 * A MAP_FIXED (mmap) or MREMAP_FIXED (mremap) names its exact target
	 * range in its arguments, and it will UNMAP that range before mapping
	 * it -- a self-vacate the hook logs and a concurrent thread can ship
	 * out of order. Record the range now, BEFORE the call runs (so before
	 * the hook fires and before any drain sees the vacate), and remove it
	 * after this call's own drain; every drain in between drops the
	 * self-vacate. See srccons above.
	 */
	int cons_idx = -1;
	if (rq->nr == 9 && (rq->args[3] & 0x10) && rq->args[1]) {	/* MAP_FIXED */
		uint64_t s = rq->args[0] & ~0xfffULL;
		uint64_t e = (rq->args[0] + rq->args[1] + 0xfffULL) & ~0xfffULL;

		cons_idx = srccons_push(as_of(pid), s, e);
	} else if (rq->nr == 25 && (rq->args[3] & 2) && rq->args[2]) {	/* MREMAP_FIXED */
		uint64_t s = rq->args[4] & ~0xfffULL;
		uint64_t e = (rq->args[4] + rq->args[2] + 0xfffULL) & ~0xfffULL;

		cons_idx = srccons_push(as_of(pid), s, e);
	}
	/*
	 * vfork is executed as fork. The clone decision is this side's
	 * (clone-decision-is-the-sources), and CLONE_VFORK's contract --
	 * suspend the caller IN THE KERNEL until the child execs or exits --
	 * cannot be honoured by a service context: the caller here never runs
	 * the child, so the suspension never ends, the assisted call never
	 * completes, and every context behind it wedges. Measured on firefox's
	 * xdg-settings child: dash's command substitution vforks, and the run
	 * sat 139s at "STILL WAITING for the source to answer syscall 58".
	 *
	 * What a vfork child is FOR -- exec or _exit, nothing else -- needs
	 * only fork semantics under this protocol: the child's memory is a
	 * coherence-managed copy either way (the destination snapshots the
	 * backing object for any non-thread child), and the parent's "wait for
	 * the exec" collapses to the pipe/waitpid it was already going to do.
	 * CLONE_VM without CLONE_THREAD is the same contract spelled in flags,
	 * so it is stripped the same way; a real thread (CLONE_THREAD) keeps
	 * its shared mm untouched.
	 */
	if (c.nr == 58) {
		c.nr = 57;
		n_vfork_as_fork++;
	} else if (c.nr == 56 && (c.args[0] & 0x4000) /* CLONE_VFORK */) {
		c.args[0] &= ~(uint64_t)0x4000;
		if (!(c.args[0] & 0x10000))	/* !CLONE_THREAD */
			c.args[0] &= ~(uint64_t)0x100;	/* CLONE_VM */
		n_vfork_as_fork++;
	} else if (c.nr == 435 && c.args[1] >= 64 && c.args[1] <= 1024) {
		/*
		 * clone3's flags travel in guest memory, so the same contract
		 * has to be rewritten there: read the program's clone_args,
		 * strip CLONE_VFORK (and CLONE_VM when it is not a thread),
		 * and hand the call a scratch copy. firefox's posix_spawn is
		 * clone3(CLONE_VM|CLONE_VFORK) and sat 225s at "STILL WAITING
		 * for the source to answer syscall 435" without this.
		 */
		uint64_t scratch = ctx_scratch(pid);
		uint64_t fl = 0;

		/*
		 * The program's clone_args live in guest memory, which may be
		 * on the other machine -- a raw peek's foreign read is refused
		 * there by design, and gating the rewrite on that peek let a
		 * VFORK clone3 through raw: the service context sat in
		 * kernel_clone's vfork-wait for ever (photographed). Pull the
		 * page(s) first, exactly as ctx_poke_owned does for writes.
		 */
		{
			uint64_t p0 = c.args[0] & ~(uint64_t)(VMR_PG_SIZE - 1);
			uint64_t p1 = (c.args[0] + c.args[1] - 1) &
				      ~(uint64_t)(VMR_PG_SIZE - 1);

			ctx_pull_page(pid, p0);
			if (p1 != p0)
				ctx_pull_page(pid, p1);
		}
		if (scratch &&
		    ctx_peek(pid, c.args[0], &fl, sizeof(fl)) == sizeof(fl) &&
		    (fl & 0x4000)) {
			char ca[1024];
			long l = (long)c.args[1];

			if (ctx_peek(pid, c.args[0], ca, l) == l) {
				fl &= ~(uint64_t)0x4000;
				if (!(fl & 0x10000))
					fl &= ~(uint64_t)0x100;
				memcpy(ca, &fl, sizeof(fl));
				if (ctx_poke(pid, scratch, ca, l) == l) {
					c.args[0] = scratch;
					n_vfork_as_fork++;
				}
			}
		}
	}
	{
		/*
		 * Whether the watched page is present here across the call, and
		 * nothing about which call it is. A page this side gave away
		 * that is present again afterwards was created by the call
		 * rather than pulled back from the guest -- so the guest's copy
		 * was never taken, both sides hold it, and whatever the call
		 * wrote never reaches the program.
		 */
		int pre = (pglog_on == 2) ? ctx_present(pid, pglog_page) : -1;
		unsigned long fpre = n_watch_faults;

		if (ctx_call(pid, &c) < 0) {
			/*
			 * INSTRUMENT (netsurf paint campaign): the -EIO this
			 * fabricates was 8.25M of a 170s run's 8.25M syscalls
			 * -- almost every futex spinning on it -- and the errno
			 * that would say WHY was discarded here. -EBUSY is the
			 * assisted slot occupied by an earlier call (patch
			 * 0053's instant refusal); -ESRCH is a dying context.
			 * Name the first ones, and count both kinds.
			 */
			int e = errno;

			if (e == EBUSY)
				n_ctxcall_busy++;
			else
				n_ctxcall_dead++;
			if (++n_ctxcall_fail <= 24) {
				char ph[128];

				fprintf(stderr, "[vmhome] ctx_call FAILED: ctx "
					"%d nr=%llu errno=%s alive=%d %s (busy so "
					"far %lu, other %lu)\n", (int)pid,
					(unsigned long long)rq->nr,
					strerror(e), kill(pid, 0) == 0,
					task_photo(pid, ph, sizeof(ph)),
					n_ctxcall_busy, n_ctxcall_dead);
			}
			return -EIO;
		}
		/*
		 * INSTRUMENT (netsurf paint campaign, round 2): the run's 3.27M
		 * -5 answers were NOT this function's fabrication (111 counted
		 * above) -- they came back in c.ret from ctls that SUCCEEDED,
		 * i.e. the kernel's completion wrote -EIO into sys_ret for a
		 * syscall (futex, clock_nanosleep) that cannot return it.
		 * Name the first ones with the register state.
		 */
		if ((long)c.ret == -EIO) {
			n_ret_eio++;
			if (n_ret_eio <= 12)
				fprintf(stderr, "[vmhome] KERNEL-EIO: ctx %d "
					"nr=%llu args=0x%llx 0x%llx 0x%llx -> "
					"ret=-EIO from the execution itself "
					"(rip=0x%llx ax=0x%llx) [#%lu]\n",
					(int)pid, (unsigned long long)rq->nr,
					(unsigned long long)rq->args[0],
					(unsigned long long)rq->args[1],
					(unsigned long long)rq->args[2],
					(unsigned long long)c.regs.rip,
					(unsigned long long)c.regs.rax,
					n_ret_eio);
		}
		/*
		 * Drain the mm-mutation hook's change log. The call above may
		 * have unmapped memory -- by any name: munmap, mremap's move
		 * or shrink, MAP_FIXED over live memory, MADV_DONTNEED, brk
		 * going down -- and the kernel's notifier on the adopted mm
		 * logged every vacated range. This side's own records forget
		 * them here, and the ranges ride this call's reply so the
		 * destination does the same (rs.vacate in serve()). The log
		 * is per-MM: another thread's concurrent unmap may drain
		 * through this call's reply, which is fine -- the ranges are
		 * facts about the address space, not about the call.
		 */
		{
			struct vmctx_mmlog ml;
			int guard = 0;
			uint32_t i;

			do {
				memset(&ml, 0, sizeof(ml));
				ml.max = VMCTX_MMLOG_MAX;
				if (ctl(pid, VMCTX_CTL_MMLOG, &ml) != 0) {
					/*
					 * Loud, once: a kernel without the
					 * change log answers EINVAL, and a
					 * silent break here IS the failure
					 * mode this refactor must never have
					 * -- every vacated range ghosts on
					 * the destination and nothing says
					 * why (a confident zero means the
					 * check never ran).
					 */
					static int said;

					if (!said++)
						fprintf(stderr, "[vmhome] "
							"VMCTX_CTL_MMLOG "
							"FAILED (%s): no mm "
							"change log in this "
							"kernel -- UNMAP "
							"PROPAGATION IS "
							"DEAD\n",
							strerror(errno));
					break;
				}
				for (i = 0; i < ml.n && i < VMCTX_MMLOG_MAX; i++) {
					/*
					 * Bit 0 of start: 1 = DISCARD (the
					 * mapping lives, the content was
					 * MADV_DONTNEEDed away). A discard
					 * resets page ownership -- the next
					 * touch is a first touch -- but the
					 * mapping's own records (a RELRO
					 * seal, a shared-file range) describe
					 * the mapping, which is untouched.
					 */
					int soft = ml.ent[i].start &
						   VMCTX_MMLOG_DISCARD;
					uint64_t vs = ml.ent[i].start &
						      ~(uint64_t)VMCTX_MMLOG_DISCARD;
					uint64_t vl = ml.ent[i].end - vs;

					/*
					 * INSTRUMENT (netsurf -14 at
					 * 0x7ffff432400c): name the CALL each
					 * vacate drained after -- the
					 * deterministic early-startup death
					 * is a range the guest writes right
					 * after it was vacated, which cannot
					 * happen natively, so either the
					 * producer call re-maps it (and the
					 * reply's keep must cover it) or the
					 * range is not what it claims.
					 */
					{
						static unsigned long n_vlog;

						if (++n_vlog <= 32)
							fprintf(stderr,
								"[vmhome] VACATE%s 0x%llx-0x%llx drained after ctx %d nr=%llu args=0x%llx 0x%llx 0x%llx 0x%llx ret=0x%llx\n",
								soft ? "(discard)" : "",
								(unsigned long long)vs,
								(unsigned long long)ml.ent[i].end,
								(int)pid,
								(unsigned long long)rq->nr,
								(unsigned long long)rq->args[0],
								(unsigned long long)rq->args[1],
								(unsigned long long)rq->args[2],
								(unsigned long long)rq->args[3],
								(unsigned long long)c.ret);
					}
					if (!soft) {
						relro_forget(vs, vl);
						sh_file_forget(pid, vs, vl);
					}
					/*
					 * Emit it minus any range an mmap
					 * MAP_FIXED is (re)constructing right
					 * now -- the self-vacate that punched
					 * ld.so's live segment. The flag rides
					 * bit 0 of start; the sequence is the
					 * kernel's event-time stamp, carried
					 * whole across any srccons split.
					 */
					vacate_emit(as_of(pid), vs,
						    ml.ent[i].end, soft,
						    ml.ent[i].seq);
				}
				vacate_ovf_pend += (uint32_t)ml.dropped;
				/*
				 * This reply's stamp: the mm's event counter
				 * at the drain, read by the kernel under the
				 * same lock that stamps the events. Whatever
				 * the call constructed is newer than every
				 * event logged before this instant --
				 * whichever thread drains it, whenever it
				 * lands. The construction's address is
				 * unpublished until this reply, so no event
				 * that outranks it can precede the sample.
				 */
				if (ml.cur)
					last_mmseq = ml.cur;
			} while (ml.n == VMCTX_MMLOG_MAX && ++guard < 64);
		}
		if (sigprobe) {
			struct vmctx_uregs g;

			if (ctl(pid, VMCTX_CTL_GETREGS, &g) == 0 &&
			    (g.rip != c.regs.rip || g.rsp != c.regs.rsp))
				fprintf(stderr, "[sigprobe] call %llu: the call "
					"answered rip=0x%llx rsp=0x%llx, the "
					"context now stands at rip=0x%llx "
					"rsp=0x%llx ax=0x%llx\n",
					(unsigned long long)rq->nr,
					(unsigned long long)c.regs.rip,
					(unsigned long long)c.regs.rsp,
					(unsigned long long)g.rip,
					(unsigned long long)g.rsp,
					(unsigned long long)g.rax);
		}
		if (pglog_on == 2) {
			int post = ctx_present(pid, pglog_page);

			if (pre != post || post == 1)
				plog(pglog_page, "SYSCALL %llu: watched page "
				     "present here pre=%d post=%d",
				     (unsigned long long)rq->nr, pre, post);
			/*
			 * The transition this exists for, and the two facts
			 * that tell its causes apart: whether anything was
			 * asked for on this side's behalf while the call ran,
			 * and what the page holds now. Zeros mean the local
			 * kernel invented it; the previous contents mean it
			 * was never really gone.
			 */
			if (pre == 0 && post == 1) {
				unsigned long got_f = n_watch_faults - fpre;
				char pg[VMR_PG_SIZE];
				unsigned s = 0;

				n_watch_appeared++;
				if (!got_f)
					n_watch_unasked++;
				if (ctx_peek(pid, pglog_page, pg,
					     sizeof(pg)) == (long)sizeof(pg))
					s = page_sum(pg);
				plog(pglog_page, "APPEARED across call %llu: "
				     "%lu fault(s) answered for it in that "
				     "window, page now sums %08x%s",
				     (unsigned long long)rq->nr, got_f, s,
				     got_f ? "" :
				     "  <<< NOBODY SUPPLIED IT");
			}
		}
	}

	if (verbose) {
		int pa = path_arg(rq->nr);

		/*
		 * Read only when the page is already here. A peek of an absent
		 * page creates one -- this thread is not the context, so its
		 * fault is not redirected and the local kernel installs the
		 * file's (or zero) page for good. For a page the guest holds
		 * the current copy of, that plants a stale present page the
		 * context will then read without ever faulting: a trace that
		 * poisons the run it is tracing. The futex probe above has the
		 * same rule for the same reason.
		 */
		if (pa >= 0 && rq->args[pa] &&
		    ctx_present(pid, rq->args[pa]) == 1) {
			char buf[sizeof(last_path)];
			long g = ctx_peek(pid, rq->args[pa], buf, sizeof(buf));

			if (g > 0) {
				buf[sizeof(buf) - 1] = '\0';
				snprintf(last_path, sizeof(last_path), "%s", buf);
			}
		}
	}

	/*
	 * A futex that answers EAGAIN is two copies of one word disagreeing,
	 * said out loud.
	 *
	 * EAGAIN means "the word is not the value you passed". The program only
	 * waits on a value it has just read, so the value it passed is what its
	 * own machine's copy of that word says, and EAGAIN is this side's copy
	 * saying something else. That is the whole coherence failure in one
	 * number, and without this it appears as a program that spins.
	 *
	 * Read only when the page is already here: a peek of an absent page
	 * would create one, which is the failure this is looking for.
	 */
	if (pglog_on && rq->nr == SYS_futex && c.ret == -EAGAIN) {
		uint32_t here = 0;
		int have = ctx_present(pid, rq->args[0]);

		if (have == 1)
			ctx_peek(pid, rq->args[0], &here, sizeof(here));
		plog(rq->args[0], "FUTEX EAGAIN at 0x%llx: the guest passed %llu, "
		     "this side holds %u (page present here=%d)",
		     (unsigned long long)rq->args[0],
		     (unsigned long long)rq->args[2], here, have);
	}

	/*
	 * A successful mmap makes its range NEW memory whatever stood there --
	 * MAP_FIXED over a live mapping included -- so any shared-file range it
	 * overlaps is dead before the new mapping is classified. Forgetting
	 * after note_shared_mapping() would erase a range that call just
	 * recorded for this very mapping.
	 */
	/*
	 * mmap's constructive half only: what a NEW shared mapping is and
	 * where its bytes live. The destructive bookkeeping that used to sit
	 * here -- pgown/relro/sh_file forgets keyed on munmap, on mmap's
	 * return, and (latterly) on mremap's three vacate shapes -- is gone:
	 * the mm-mutation hook's drain in ctx_forward() forgets every vacated
	 * range generically, including the ones this list never had (brk
	 * shrink, MADV_DONTNEED, MAP_FIXED's implicit unmap). A switch on
	 * syscall numbers cannot enumerate how an mm loses memory; the
	 * kernel's notifier does not have to.
	 */
	note_shared_mapping(pid, rq, (long)c.ret);
	/*
	 * A successful mprotect() to a read-only protection (no PROT_WRITE)
	 * seals a range: this is where RELRO happens, and where a read-only
	 * file page starts being able to differ from the file. Record it so
	 * faults in it are asked of the guest's machine; a re-widening of the
	 * protection, an munmap or an mmap over it clears the record, because
	 * the page is then either writable again (asked as writable) or new.
	 */
	if (rq->nr == SYS_mprotect && c.ret == 0) {
		if (rq->args[2] & PROT_WRITE)
			relro_forget(rq->args[0], rq->args[1]);
		else
			relro_note(rq->args[0], rq->args[1]);
	}
	/* (munmap's and mmap-over's relro_forget now come from the hook drain,
	 * with every other way a sealed range can cease to exist.) */
	/*
	 * A successful exec replaced the whole address space, and every record
	 * this side keeps about it describes memory that no longer exists: an
	 * ownership entry saying a page of the OLD image is THEIRS refuses
	 * serves of the NEW image's page at the same address, a shared-file
	 * range of the old image reclaims pages of the new one, and the fork
	 * relationship is over -- the exec'd child is a copy of nobody, and
	 * answering COWBREAK about its pages serves it the parent's image.
	 * The destination wipes its own half on the same reply (exec_wipe).
	 */
	if ((rq->nr == 59 || rq->nr == 322) && c.ret == 0) {
		/* The page record is the new mm's own: fresh by construction. */
		sh_file_forget(pid, 0, (uint64_t)1 << 47);
		fork_rel_forget(pid);
		/*
		 * Every cached /proc descriptor is a reference to an mm, and
		 * this task's mm just ceased to be the program's. The pagemap
		 * caches revalidate against this counter; ctx0's mem
		 * descriptor is simply dropped -- what it reads is the image
		 * as ORIGINALLY loaded, and after an exec there is no such
		 * thing (the relayout is the map now, and pages come from the
		 * live mm like anyone else's).
		 */
		__atomic_add_fetch(&mm_generation, 1, __ATOMIC_RELAXED);
		if (pid == ctx0_pid && ctx0_mem >= 0) {
			close(ctx0_mem);
			ctx0_mem = -1;
		}
	}

	/*
	 * Anything beyond RAX is a call that moved the program, and the whole
	 * register set goes back so the far side can carry on with it.
	 *
	 * The thread pointer counts, and the mask cannot say so: FS and GS are
	 * part of the architectural state but pt_regs cannot express them, so
	 * there is no VMCTX_REG_ bit for either and they are compared instead.
	 * arch_prctl is the call that does it, and it reports only RAX --  so
	 * without this the source set its own thread pointer, said nothing, and
	 * the program went on computing every thread-local address from
	 * whatever the far side's FS happened to hold. Its very next call
	 * passed a pointer 1 GiB away from its own heap.
	 */
	if ((c.changed & ~VMCTX_REG_RAX) ||
	    (in && (c.regs.fs_base != in->fs_base ||
		    c.regs.gs_base != in->gs_base))) {
		memcpy(out, &c.regs, sizeof(*out));
		*out_valid = 1;
	}
	/*
	 * This call's drain has run; its own self-vacate is either shipped
	 * (suppressed) or gone. Release the record so a genuine later munmap
	 * of the same range is not mistaken for it.
	 */
	srccons_pop(cons_idx);
	return (long)c.ret;
}

/* ---- loading the program ----------------------------------------------- */

/*
 * The program, loaded here and stopped before it executes anything, and then
 * adopted as a service context.
 *
 * This kernel does all of the loading — the ELF, its interpreter, the initial
 * stack with argv/env/auxv, the relocations — so the other machine never needs
 * to understand any of it. What is left afterwards is a task holding a finished
 * address space, which is exactly what the owner of a program has to be.
 */
/*
 * The program this source runs, from ITS OWN command line.
 *
 * It used to arrive from the destination, which sent the strings it had been
 * started with and this side execv'd them. That worked and was still wrong: a
 * path is a source-side idea. The destination has no filesystem the program can
 * reach, no notion of an executable and no way to resolve a path -- see
 * PRINCIPLES 6a -- so asking its operator to name one puts a concept in the
 * interface of the machine least able to hold it, and makes a run depend on the
 * two machines agreeing about a namespace only one of them has.
 *
 * So the source decides what runs, the destination says only where to connect,
 * and VMR_OP_START carries nothing. Assume the destination could not open the
 * program if it tried: that is the machine this has to work on.
 */
static char **prog_argv;
static int prog_argc;

static long oracle_start(char *argv_blob, uint64_t len, void *out, size_t outmax)
{
	char *argv[64];
	struct user_regs_struct regs;
	char path[256];
	int n = 0, st;
	pid_t kid;

	if (ctx0_pid)
		return -EBUSY;
	/*
	 * Anything the far side sent is ignored on purpose, and said out loud
	 * once: a destination that names a program is a destination that has
	 * been told what a file is.
	 */
	if (len) {
		static int said;

		if (!said++)
			fprintf(stderr, "[vmhome] the destination sent %llu byte(s) "
				"of program name; ignoring them -- this side "
				"decides what runs\n", (unsigned long long)len);
	}
	(void)argv_blob;
	for (n = 0; n < prog_argc && n < 63; n++)
		argv[n] = prog_argv[n];
	argv[n] = NULL;
	if (!n) {
		fprintf(stderr, "[vmhome] no program on this side's command "
			"line; nothing to run\n");
		return -EINVAL;
	}

	kid = fork();
	if (kid < 0)
		return -errno;
	if (kid == 0) {
		ptrace(PTRACE_TRACEME, 0, 0, 0);
		/*
		 * Deterministic layout: the remote machine must be able to use
		 * the same addresses.
		 */
		personality(ADDR_NO_RANDOMIZE);
		/*
		 * And it dies with us. A service context parks in the kernel
		 * and can never run the program by itself, so this is not the
		 * guard PTRACE_O_EXITKILL was — it is only so that a vmhome
		 * killed outright does not leave the program's address space
		 * sitting in a task nobody will ever ask anything of.
		 */
		prctl(PR_SET_PDEATHSIG, SIGKILL, 0, 0, 0);
		execv(argv[0], argv);
		_exit(127);
	}
	if (waitpid(kid, &st, 0) < 0 || !WIFSTOPPED(st)) {
		return -EIO;
	}
	if (ptrace(PTRACE_SETOPTIONS, kid, 0, PTRACE_O_EXITKILL) < 0)
		fprintf(stderr, "[vmhome] cannot make the loading program die "
			"with us (%s)\n", strerror(errno));
	if (ptrace(PTRACE_GETREGS, kid, 0, &regs) < 0) {
		int e = errno;

		kill(kid, SIGKILL);
		waitpid(kid, NULL, 0);
		return -e;
	}
	/*
	 * The vDSO hiding has to happen HERE, at the exec stop, while the
	 * task still sits at the interpreter's entry: ld.so reads auxv as
	 * its first act and caches AT_SYSINFO_EHDR for the rest of the
	 * process's life, so a patch after "let the loader finish" below
	 * changes a stack word nobody will read again. The stack at this
	 * stop is [argc][argv...][0][envp...][0][auxv pairs...], and the
	 * patch is one word: the AT_SYSINFO_EHDR tag becomes AT_IGNORE.
	 * (The kernel's own copy -- /proc/pid/auxv -- keeps the original,
	 * which is exactly right: the AT_ENTRY read below still works.)
	 */
	if (hide_vdso()) {
		uint64_t sp = regs.rsp;
		long argc = ptrace(PTRACE_PEEKDATA, kid, (void *)sp, NULL);
		int lists;

		sp += 8 + (uint64_t)(argc + 1) * 8;	/* argc + argv[] + NULL */
		for (lists = 0; lists < 1; ) {		/* envp up to its NULL */
			long w = ptrace(PTRACE_PEEKDATA, kid, (void *)sp, NULL);

			sp += 8;
			if (w == 0)
				lists++;
			if (sp - regs.rsp > 512 * 1024)
				break;			/* not a sane stack */
		}
		for (;;) {				/* the auxv pairs */
			long tag = ptrace(PTRACE_PEEKDATA, kid, (void *)sp, NULL);

			if (tag == 0)			/* AT_NULL */
				break;
			if (tag == 33) {		/* AT_SYSINFO_EHDR */
				if (ptrace(PTRACE_POKEDATA, kid, (void *)sp,
					   (void *)26L) == 0)	/* AT_IGNORE */
					fprintf(stderr, "[vmhome] first image's "
						"AT_SYSINFO_EHDR -> AT_IGNORE "
						"(at rsp+0x%llx)\n",
						(unsigned long long)(sp - regs.rsp));
				else
					fprintf(stderr, "[vmhome] could not patch "
						"AT_SYSINFO_EHDR: %s\n",
						strerror(errno));
				break;
			}
			sp += 16;
			if (sp - regs.rsp > 512 * 1024)
				break;
		}
	}
	/*
	 * Let the dynamic loader finish here, then hand over the finished
	 * address space. Stopping at ld.so's entry means the other machine has
	 * to re-do the load itself — mapping libraries from files, honouring
	 * .bss, alignment and relocation — which is a great deal of ELF
	 * behaviour to re-implement correctly. Running to the program's own
	 * entry point instead leaves every library mapped and relocated by this
	 * machine's kernel and loader, so the other machine only has to copy
	 * memory. AT_ENTRY from auxv is the program's entry, already biased.
	 */
	{
		char apath[64];
		unsigned long auxv[2], entry = 0;
		int af;

		snprintf(apath, sizeof(apath), "/proc/%d/auxv", (int)kid);
		af = open(apath, O_RDONLY);
		while (af >= 0 && read(af, auxv, sizeof(auxv)) == (ssize_t)sizeof(auxv)) {
			if (auxv[0] == 0)
				break;
			if (auxv[0] == 9) {	/* AT_ENTRY */
				entry = auxv[1];
				break;
			}
		}
		if (af >= 0)
			close(af);

		if (entry) {
			long orig = ptrace(PTRACE_PEEKDATA, kid,
					   (void *)entry, NULL);

			if (orig != -1) {
				long brk_word = (orig & ~0xffL) | 0xccL;

				ptrace(PTRACE_POKEDATA, kid, (void *)entry,
				       (void *)brk_word);
				ptrace(PTRACE_CONT, kid, 0, 0);
				if (waitpid(kid, &st, 0) > 0 && WIFSTOPPED(st)) {
					ptrace(PTRACE_POKEDATA, kid,
					       (void *)entry, (void *)orig);
					if (ptrace(PTRACE_GETREGS, kid, 0,
						   &regs) == 0) {
						regs.rip = entry;
						ptrace(PTRACE_SETREGS, kid,
						       0, &regs);
						fprintf(stderr, "[vmhome] ran the loader; "
							"stopped at the program's entry 0x%lx\n",
							entry);
					}
				} else {
					fprintf(stderr, "[vmhome] the program did not reach "
						"its entry point; handing over the pre-load "
						"state\n");
				}
			}
		}
	}

	/*
	 * And from here it is a service context: it holds the program and never
	 * executes any of it. ADOPT arms that before the detach, so there is no
	 * window in which a task stopped at an entry point is simply let go —
	 * which is what a detached tracee does, and it ran the program natively
	 * on this machine once, printing the answer of a run that never
	 * happened.
	 */
	if (ctl(kid, VMCTX_CTL_ADOPT, NULL) != 0) {
		int e = errno;

		fprintf(stderr, "[vmhome] cannot adopt %d as a service context: "
			"%s\n", (int)kid, strerror(e));
		kill(kid, SIGKILL);
		waitpid(kid, NULL, 0);
		return -e;
	}
	if (ptrace(PTRACE_DETACH, kid, 0, 0) < 0)
		fprintf(stderr, "[vmhome] PTRACE_DETACH: %s\n", strerror(errno));

	ctx0_pid = kid;
	snprintf(path, sizeof(path), "/proc/%d/mem", (int)kid);
	ctx0_mem = open(path, O_RDONLY);

	/* Hand back the register set the program should start with. */
	{
		uint64_t u[21];

		u[0]=regs.rax; u[1]=regs.rbx; u[2]=regs.rcx; u[3]=regs.rdx;
		u[4]=regs.rsi; u[5]=regs.rdi; u[6]=regs.rbp; u[7]=regs.rsp;
		u[8]=regs.r8;  u[9]=regs.r9;  u[10]=regs.r10; u[11]=regs.r11;
		u[12]=regs.r12; u[13]=regs.r13; u[14]=regs.r14; u[15]=regs.r15;
		u[16]=regs.rip; u[17]=regs.eflags; u[18]=regs.orig_rax;
		/* The loader has set up TLS by now; the thread pointer is part
		 * of the state being handed over. */
		u[19]=regs.fs_base; u[20]=regs.gs_base;
		memcpy(out, u, sizeof(u) < outmax ? sizeof(u) : outmax);
	}
	fprintf(stderr, "[vmhome] %s is held by service context %d, "
		"rip=0x%llx rsp=0x%llx\n", argv[0], (int)kid,
		(unsigned long long)regs.rip, (unsigned long long)regs.rsp);
	return 21 * 8;
}

static long pid_layout(pid_t of, char *out, size_t outmax);

static long oracle_layout(char *out, size_t outmax)
{
	long n;

	if (!ctx0_pid)
		return -ESRCH;
	n = pid_layout(ctx0_pid, out, outmax);
	/*
	 * The first image's auxv was patched at spawn, so nothing in it can
	 * reach the vDSO -- and the layout must agree: a [vdso]/[vvar] range
	 * the destination knows about is a range it will serve, and the whole
	 * point of hiding is that those bytes are never served. Exec'd images
	 * keep theirs (see hide_vdso()); this filter is the FIRST map only.
	 */
	if (n > 0 && hide_vdso()) {
		char *rd = out, *wr = out;

		while (*rd) {
			char *nl = strchr(rd, '\n');
			size_t len = nl ? (size_t)(nl - rd) + 1 : strlen(rd);
			int drop = memmem(rd, len, "[vdso]", 6) ||
				   memmem(rd, len, "[vvar", 5);

			if (!drop && wr != rd)
				memmove(wr, rd, len);
			if (!drop)
				wr += len;
			rd += len;
		}
		*wr = '\0';
		n = wr - out;
	}
	return n;
}

/*
 * The layout of a NAMED service context -- what a context that has exec'd asks
 * for. The oracle is the FIRST program as loaded; after an exec the only true
 * map is the exec'd service context's own, and serving the oracle's instead is
 * how a fresh child was handed the previous image's protections.
 */
static long pid_layout(pid_t of, char *out, size_t outmax)
{
	char path[64];
	int fd;
	long n;

	if (of <= 0)
		return -ESRCH;
	snprintf(path, sizeof(path), "/proc/%d/maps", (int)of);
	fd = open(path, O_RDONLY);
	if (fd < 0)
		return -errno;
	/*
	 * procfs hands this out in chunks: a single read() returns only the
	 * first, silently dropping the rest of the map — including [stack],
	 * whose absence leaves the guest with no stack at all. Read to EOF.
	 */
	n = 0;
	for (;;) {
		ssize_t r = read(fd, out + n, outmax - 1 - (size_t)n);

		if (r < 0) {
			if (errno == EINTR)
				continue;
			break;
		}
		if (r == 0)
			break;
		n += r;
		if ((size_t)n >= outmax - 1) {
			fprintf(stderr, "[vmhome] FATAL: the program's map "
				"exceeds %zu bytes; a truncated map leaves the "
				"guest faulting on regions nobody described. "
				"Raise VMR_LAYOUT_MAX.\n", outmax);
			_exit(3);
		}
	}
	close(fd);
	out[n] = '\0';
	if (getenv("VMHOME_TRACE_LAYOUT"))
		fprintf(stderr, "[vmhome] program map:\n%s", out);
	return n;
}

/* The program as it was loaded, for the far side's first look at it. */
static long oracle_page(uint64_t addr, uint64_t len, char *out, size_t outmax)
{
	long n;

	if (ctx0_mem < 0)
		return -ESRCH;
	if (len > outmax)
		len = outmax;
	n = pread(ctx0_mem, out, len, (off_t)addr);
	return (n < 0) ? -errno : n;
}

/*
 * This context's memory as it is now, read out of the task that holds it.
 *
 * A failure here is information, not an error to paper over: an address the
 * source has not mapped reports EFAULT, which is a real segmentation fault for
 * the guest rather than an excuse to zero-fill. There is nothing left to guess
 * with — the mappings read are the program's own.
 */
static long ctx_page_inner(pid_t pid, uint64_t addr, uint64_t len, char *out,
			   size_t outmax);

/* ---- fork: who shares with whom, and where that has stopped ------------- */

/*
 * A fork leaves parent and child mapping the SAME memory until one of them
 * writes -- copy-on-write -- and there is exactly one place that knows it: here.
 * The source ran the real clone3, so its own kernel holds the relationship for
 * every page the source still has, and does copy-on-write on them itself. What
 * it cannot do is the same thing for the pages that are on the OTHER machine,
 * and those are most of the interesting ones: the destination gave the child an
 * object of its own, empty, so a page of the parent's that lives over there is
 * in neither of this side's two tasks.
 *
 * This is the record for exactly those. fork_rel[] is "B's address space began
 * as a copy of A's". cow_broken[] is where that has stopped being true: the
 * child has been given its own copy of that page, so the parent's is free to
 * change.
 *
 * Two questions are answered from it, and the second is what makes the first
 * correct:
 *
 *   the CHILD asks for a page this side does not hold  -> it is still the
 *      parent's, and the parent's copy is on the destination: COWBREAK.
 *   the PARENT is about to WRITE a page the destination holds -> if a child has
 *      not been given that page yet, it must be given the bytes that are there
 *      NOW, before the write lands: COWBREAK.
 *
 * Without the second, a child that faults late is handed its parent's memory as
 * of the fault instead of as of the fork, which is what tests/cow1.c measures.
 */
static struct { pid_t child, parent; } fork_rel[FORK_REL_MAX];
static int nfork_rel;
static pthread_mutex_t fork_rel_lock = PTHREAD_MUTEX_INITIALIZER;

#define COW_BROKEN_SLOTS 65536
static struct { pid_t child; uint64_t page; } cow_broken[COW_BROKEN_SLOTS];
static pthread_mutex_t cow_broken_lock = PTHREAD_MUTEX_INITIALIZER;

/*
 * Keyed by ADDRESS SPACE, not by context, and that is not a detail.
 *
 * Copy-on-write is a property of an address space: every thread of a forked
 * child inherits exactly what the child inherited, and a thread faulting on one
 * of those pages is the same event as the child faulting on it. Recorded by pid
 * instead, a child's THREAD had no parent, so nothing recognised its faults --
 * and the one place that mattered most was this side's own fault service, which
 * then reached "fresh anonymous memory the guest has never touched" and poked a
 * page of zeros over memory the fork had inherited. Measured on tests/ex4, whose
 * child creates a thread: the thread died reading a pointer out of one of those
 * pages, and the parent printed "child exited -1".
 *
 * as_of() is the thread group, read from the kernel that made it.
 */
static void fork_rel_note(pid_t child, pid_t parent)
{
	child = (pid_t)as_of(child);
	parent = (pid_t)as_of(parent);
	pthread_mutex_lock(&fork_rel_lock);
	if (nfork_rel < FORK_REL_MAX) {
		fork_rel[nfork_rel].child = child;
		fork_rel[nfork_rel].parent = parent;
		__atomic_store_n(&nfork_rel, nfork_rel + 1, __ATOMIC_RELEASE);
	}
	pthread_mutex_unlock(&fork_rel_lock);
}

/*
 * A child that has ended shares nothing with anybody. Forgetting it is not
 * tidiness: while the row is there every store the parent makes to a page of
 * its own costs a round trip to be told that nobody wants it.
 */
static void fork_rel_forget(pid_t child)
{
	int n = __atomic_load_n(&nfork_rel, __ATOMIC_ACQUIRE), i;

	/*
	 * Only when the address space itself ends. A thread of a forked child
	 * leaving does not end what the child inherited.
	 */
	if (child <= 0 || as_of(child) != (uint32_t)child)
		return;
	pthread_mutex_lock(&fork_rel_lock);
	for (i = 0; i < n; i++)
		if (fork_rel[i].child == child)
			fork_rel[i].child = 0;
	pthread_mutex_unlock(&fork_rel_lock);
}

static pid_t fork_rel_parent(pid_t child)
{
	int n = __atomic_load_n(&nfork_rel, __ATOMIC_ACQUIRE), i;

	if (child <= 0)
		return 0;
	child = (pid_t)as_of(child);
	for (i = 0; i < n; i++)
		if (fork_rel[i].child == child)
			return fork_rel[i].parent;
	return 0;
}

static int fork_rel_children(pid_t parent, pid_t *out, int max)
{
	int n = __atomic_load_n(&nfork_rel, __ATOMIC_ACQUIRE), i, k = 0;

	if (parent <= 0)
		return 0;
	parent = (pid_t)as_of(parent);
	for (i = 0; i < n && k < max; i++)
		if (fork_rel[i].parent == parent && fork_rel[i].child > 0)
			out[k++] = fork_rel[i].child;
	return k;
}

static unsigned cow_slot(pid_t child, uint64_t page)
{
	return (unsigned)(((page >> 12) * 2654435761u) ^
			  ((unsigned)child * 2246822519u)) % COW_BROKEN_SLOTS;
}

static int cow_is_broken(pid_t child, uint64_t page)
{
	unsigned i, n;
	int hit = 0;

	child = (pid_t)as_of(child);	/* the record is per address space */
	page &= ~(uint64_t)(VMR_PG_SIZE - 1);
	i = cow_slot(child, page);
	pthread_mutex_lock(&cow_broken_lock);
	for (n = 0; n < 64; n++) {
		unsigned k = (i + n) % COW_BROKEN_SLOTS;

		if (cow_broken[k].child == child && cow_broken[k].page == page) {
			hit = 1;
			break;
		}
		if (!cow_broken[k].child)
			break;
	}
	pthread_mutex_unlock(&cow_broken_lock);
	return hit;
}

/* "A keeps X, B has its own Y from now on." */
static void cow_note_break(pid_t child, uint64_t page)
{
	unsigned i, n;

	child = (pid_t)as_of(child);	/* the record is per address space */
	page &= ~(uint64_t)(VMR_PG_SIZE - 1);
	i = cow_slot(child, page);
	pthread_mutex_lock(&cow_broken_lock);
	for (n = 0; n < 64; n++) {
		unsigned k = (i + n) % COW_BROKEN_SLOTS;

		if (cow_broken[k].child == child && cow_broken[k].page == page)
			break;
		if (!cow_broken[k].child) {
			cow_broken[k].child = child;
			cow_broken[k].page = page;
			n_cow_pages_owed++;
			break;
		}
	}
	pthread_mutex_unlock(&cow_broken_lock);
}

/*
 * The destination is holding a page and one of its contexts is about to write
 * it. Everything that has to happen first, decided here and executed there.
 *
 * Today there is exactly one such thing: an address space that began as a copy
 * of this one and has not been given this page yet has to be given the bytes
 * that are there NOW -- before the write, which is what makes them the bytes as
 * of the fork rather than as of the fault. That is a copy-on-write break, and it
 * legitimately makes two pages: the writer keeps its own, the copy gets a second.
 *
 * "Is the program allowed to write here at all" is asked first and answered from
 * the program's own map, because it is the same question and only this side can
 * answer it. A write to a page the program has made read-only is the program's
 * own fault and must reach it as one; granting it in order to break sharing
 * would hide a signal the program has earned.
 */
static long ctx_write_prep(pid_t pid, uint64_t addr)
{
	uint64_t page = addr & ~(uint64_t)(VMR_PG_SIZE - 1);
	pid_t kids[FORK_REL_MAX];
	int n, i, owed = 0, prot;

	struct vmctx_serve sv;

	n = fork_rel_children(pid, kids, FORK_REL_MAX);
	memset(&sv, 0, sizeof(sv));
	sv.addr = page;
	sv.flags = VMCTX_SERVE_PROBE;
	prot = ctl(pid, VMCTX_CTL_SERVE, &sv) == 0 &&
	       sv.status != VMCTX_SERVE_NOMAP && (sv.class & VMCTX_PGC_RO);
	if (prot) {
		/*
		 * -EACCES, not 0, and the difference is a whole test.
		 *
		 * 0 means "nothing has to happen first", which the far side
		 * answers by granting the write -- and a fork write-protects
		 * every page it holds, read-only ones included, so a program
		 * that stores into its own read-only memory after forking would
		 * have that store quietly succeed. tests/pf3 says it in those
		 * words: "the fault never happened; the access was served".
		 *
		 * This says the protection the far side is looking at is not
		 * the fork's, so the fault is the program's own and belongs to
		 * this side to interpret as a signal.
		 */
		n_cow_write_ro++;
		plog(page, "WRITEPREP: the program may not write here; the "
		     "fault is its own");
		return -EACCES;
	}
	for (i = 0; i < n; i++)
		if (!cow_is_broken(kids[i], page)) {
			cow_note_break(kids[i], page);
			owed++;
		}

	if (!owed) {
		n_cow_write_free++;
		return 0;
	}
	n_cow_parent_told++;
	if (n_cow_parent_told <= 8)
		fprintf(stderr, "[vmhome] context %d is about to write 0x%llx "
			"and %d address space(s) copied from it have not been "
			"given that page: breaking copy-on-write there first\n",
			(int)pid, (unsigned long long)page, owed);
	plog(page, "WRITEPREP: %d copy/copies still owe this page; break first",
	     owed);
	return VMR_CTXPAGE_COWBREAK;
}

/*
 * Should this request be answered with the page, or with the instruction to
 * break copy-on-write where the page already is?
 *
 * Asked BEFORE the serve, and that placement is the whole of what it took to get
 * right. Refining the ANSWER -- turning an ABSENT into a COWBREAK afterwards --
 * catches only the requests this side answers with "not here", and the ones that
 * matter most are not those: for a page of a private FILE mapping this side does
 * not hold, ctx_page_inner() faults the page in from the FILE and serves that.
 * For a fresh context the file's bytes are exactly right. For an address space
 * that began as a copy of another, they are the program as it was LOADED, and
 * everything the original wrote into that page after loading is missing.
 *
 * Measured on tests/cow1: the child was served 0x4b1000..0x4b5000 from the file,
 * which is the loader's own relocation area before the loader ran. Its first
 * indirect call went through a PLT slot holding the address of that same PLT
 * entry, and the child span on `jmp *GOT` for the rest of the run -- with the
 * kernel naming it exactly: "guest has taken 2000 interrupts with no syscall and
 * no fault -- spinning in userspace around rip 0x401100".
 *
 * So the question asked is the one that decides it: does this side HOLD the
 * page, in this context's own task? A fork on this side is a real fork, so a
 * page the source still has is genuinely copy-on-write between the two tasks and
 * its own kernel keeps them apart -- nothing to do. A page it does not have is
 * on the other machine, in the address space this one was copied from, and only
 * that machine can produce it.
 *
 * Three terms, and each excludes a case that would otherwise be answered wrongly:
 *
 *   fork_rel_parent   this address space began as a copy of another. Nothing
 *                     else is affected at all.
 *   pg_state == NONE  nothing has ever been recorded about this page for this
 *                     address space -- not handed over, not claimed, not held.
 *                     Once anything has, the record is the authority and the
 *                     fork is over for that page.
 *   !ctx_present      this side does not hold it. Asked of the page tables
 *                     rather than by reading, because reading a page of a
 *                     context from a task that is not it CREATES one.
 *
 * `nocow` is the far side saying it has already tried the break and there was
 * nothing to copy from -- an address neither machine has ever written, whose
 * file is the right answer after all. Without it that page would be asked for,
 * refused, and asked for again for ever.
 */
static int cow_still_shared(pid_t pid, uint64_t addr, int nocow)
{
	uint64_t page = addr & ~(uint64_t)(VMR_PG_SIZE - 1);

	struct vmctx_serve sv;

	if (nocow || pid <= 0 || !fork_rel_parent(pid))
		return 0;
	/*
	 * The record, the mapping and presence, in one probe that moves
	 * nothing (VMCTX_SERVE_PROBE). A shared mapping is shared ACROSS the
	 * fork -- that is what MAP_SHARED means -- so there is nothing to
	 * break and a break would be the defect.
	 */
	memset(&sv, 0, sizeof(sv));
	sv.addr = page;
	sv.flags = VMCTX_SERVE_PROBE;
	if (ctl(pid, VMCTX_CTL_SERVE, &sv) != 0)
		return 0;
	if (sv.state != PG_NONE)
		return 0;
	if (sv.class & VMCTX_PGC_SHARED)
		return 0;
	if (sv.class & VMCTX_PGC_PRESENT)
		return 0;
	n_cow_child_told++;
	if (n_cow_child_told <= 8)
		fprintf(stderr, "[vmhome] 0x%llx is not here and context %d's "
			"address space began as a copy of %d's: telling the "
			"other machine to break copy-on-write where the page "
			"already is\n", (unsigned long long)page, (int)pid,
			(int)fork_rel_parent(pid));
	plog(page, "SERVE: still the original's; answered COWBREAK");
	return 1;
}

/*
 * The take generations of the pages ONE ctx_page() answer serves, in order,
 * for serve() to put on the wire (vmr_rsp.pggen). Per thread -- a connection
 * thread serves one request at a time. Filled where a page is committed to
 * `out`, from the page's own record: the stamp of the copy this side holds,
 * which is what the destination compares with its count. A page this side
 * produced itself (file, zeros) carries 0 -- never came from there.
 */
static __thread uint32_t ctxpage_gen[16];
static __thread uint32_t ctxpage_ngen;
/*
 * ...and which of them were TAKEN (bit i = page i left this side, recorded
 * IN TRANSIT, and wants the destination's INSTALLED ack); a copied page
 * wants none, and the destination skips the ack for it (vmr_rsp.map_off on
 * a CTXPAGE reply carries the mask). A multi-page request (the cold bulk of
 * a region) is settled here directly, as it always was: the ack would need
 * a payload.
 */
static __thread uint32_t ctxpage_taken;

static void serve_gen_note(uint64_t off, uint32_t gen)
{
	unsigned i = (unsigned)(off >> 12);

	if (i < 16) {
		ctxpage_gen[i] = gen;
		if (ctxpage_ngen < i + 1)
			ctxpage_ngen = i + 1;
	}
}

/*
 * A raw read of one page out of a task that HOLDS it -- gated on a pagemap
 * probe by the caller, so it never faults anything into being. Used only by
 * the chain serve below, where the whole point is to copy an ancestor's
 * bytes without taking its page or materializing one.
 */
static long chain_read_raw(pid_t anc, uint64_t addr, char *out)
{
	char path[64];
	long r;
	int fd;

	snprintf(path, sizeof(path), "/proc/%d/mem", (int)anc);
	fd = open(path, O_RDONLY);
	if (fd < 0)
		return -1;
	r = pread(fd, out, VMR_PG_SIZE, (off_t)addr);
	close(fd);
	return r;
}

static unsigned long n_cow_chain_served;

static long ctx_page(pid_t pid, uint64_t addr, uint64_t len, char *out,
		     size_t outmax, int nocow)
{
	long rc;

	ctxpage_ngen = 0;
	ctxpage_taken = 0;
	memset(ctxpage_gen, 0, sizeof(ctxpage_gen));
	if (cow_still_shared(pid, addr, nocow))
		return VMR_CTXPAGE_COWBREAK;
	rc = ctx_page_inner(pid, addr, len, out, outmax);
	/*
	 * ABSENT, for a fork copy, AFTER the other machine's own chain walk
	 * came up empty (nocow says so): the inheritance is on THIS side, in
	 * the nearest ancestor whose mm actually holds the page. Found by
	 * pagemap (ctx_present -- never a read, which would create a page),
	 * copied raw (no take: the ancestor keeps its page, which is what a
	 * COW break means), served with gen 0 like every page this side
	 * produces itself. An ancestor without its own copy passes
	 * inheritance through unchanged, so walking past it is correct --
	 * the same one-step rule as the destination's cow_give_from_chain.
	 * Without this, the destination's answer to the ABSENT was "a
	 * legitimate first touch, zeros" -- which for a fork child's TLS
	 * page was a NULL THREAD_SELF (kernel #173/#175's corpse, the wipe
	 * campaign's remaining producer).
	 */
	if (rc == VMR_CTXPAGE_ABSENT && nocow && len == VMR_PG_SIZE &&
	    fork_rel_parent(pid)) {
		pid_t anc;
		int hops = 0;

		for (anc = fork_rel_parent(pid); anc && hops < 16;
		     anc = fork_rel_parent(anc), hops++) {
			if (ctx_present(anc, addr) != 1)
				continue;
			if (chain_read_raw(anc, addr, out) !=
			    (long)VMR_PG_SIZE)
				continue;
			n_cow_chain_served++;
			if (n_cow_chain_served <= 16)
				fprintf(stderr, "[vmhome] 0x%llx for fork "
					"copy %d: absent everywhere the "
					"record reaches; served ancestor "
					"%d's own page (%d hop(s) up)\n",
					(unsigned long long)addr, (int)pid,
					(int)anc, hops);
			plog(addr, "SERVE: chain-served from ancestor %d "
			     "(%d hops)", (int)anc, hops);
			return (long)VMR_PG_SIZE;
		}
	}
	return rc;
}

/*
 * One page, asked of the kernel: VMCTX_CTL_SERVE, retried while an in-flight
 * syscall holds the page (-EBUSY is "not yet", PRINCIPLES §2). One second,
 * 1 ms steps -- the bound the take has always had here: a local take waits
 * out a syscall's hold, measured in milliseconds; a second of it is a stuck
 * step, and the remote fault waiting on this one has its own deadline above
 * us, so this bound must fit inside it. (A 200 ms yield was tried and
 * reverted: on the kernel whose holds refuse while a call runs, giving up to
 * CLAIMING at 200 ms took pg3 from 96/96 to 42/48.) -ESRCH: the context is
 * gone and a sibling must answer.
 */
/*
 * One kernel counter, by name. The serve's give-up diagnostic below prints
 * the DELTAS of the take/hold counters across its own 1 s busy loop: the
 * branch that answered EBUSY a thousand times is the one whose counter
 * moved by ~a thousand, and that split (syscall-hold vs take-refusal vs
 * serve-busy) is exactly what the startup-stall photograph (session 41:
 * task PARKED, syscall -1, record NONE, 8x1.07 s of EBUSY) could not say.
 */
static long kparam_read(const char *name)
{
	char path[128], buf[32];
	int fd;
	ssize_t n;

	snprintf(path, sizeof(path),
		 "/sys/module/kernel/parameters/%s", name);
	fd = open(path, O_RDONLY);
	if (fd < 0)
		return -1;
	n = read(fd, buf, sizeof(buf) - 1);
	close(fd);
	if (n <= 0)
		return -1;
	buf[n] = 0;
	return atol(buf);
}

static long ctx_serve_one(pid_t pid, uint64_t a, char *buf, struct vmctx_serve *sv,
			  int wait_hold)
{
	int busy = 0;
	long r;
	long c_hold = -1, c_ref = -1, c_sbusy = -1, c_pres = -1;

	memset(sv, 0, sizeof(*sv));
	sv->addr = a;
	sv->buf = (uint64_t)(uintptr_t)buf;
	/*
	 * The wait is for the page the asker is STOPPED on. A neighbour in a
	 * multi-page request (wait_hold 0) is answered short instead: the
	 * chunk is a prefetch, and a second spent on a page a forwarded
	 * syscall is touching -- sixteen times over, for a guest waiting on
	 * the first page -- is the stall session 40 measured on the serve.
	 */
	while ((r = ctl(pid, VMCTX_CTL_SERVE, sv)) < 0 && errno == EBUSY &&
	       wait_hold && busy++ < 1000) {
		if (busy == 1) {
			c_hold  = kparam_read("vmctx_take_hold");
			c_ref   = kparam_read("vmctx_take_refused");
			c_sbusy = kparam_read("vmctx_pgrec_serve_busy");
			c_pres  = kparam_read("vmctx_syscall_hold_present");
		}
		usleep(1000);
	}
	if (busy)
		n_take_busy++;
	if (r < 0 && errno == EBUSY) {
		/*
		 * Still held after a full second of asking -- by an in-flight
		 * syscall, per the kernel's own refusal lines in dmesg. There
		 * is no fallback: every way of "continuing anyway" was
		 * measured (a copy while keeping this one forked the stack;
		 * answering short broke fault delivery). CLAIMING is the
		 * truthful answer: this side holds the page and cannot give it
		 * up yet; the far side yields and asks again.
		 */
		if (n_take_gaveup < 3) {
			fprintf(stderr, "[vmhome] 0x%llx cannot be returned "
				"yet: context %d still refuses it after %d "
				"attempts over 1s. The kernel's 'refused, held "
				"by in-flight syscall' lines in dmesg name the "
				"holder. Answering \"ask again\" rather than "
				"serving a copy of a page both sides write. "
				"Refusal deltas over this loop: take_hold %+ld "
				"take_refused %+ld serve_busy %+ld "
				"hold_present %+ld\n",
				(unsigned long long)a, (int)pid, busy,
				kparam_read("vmctx_take_hold") - c_hold,
				kparam_read("vmctx_take_refused") - c_ref,
				kparam_read("vmctx_pgrec_serve_busy") - c_sbusy,
				kparam_read("vmctx_syscall_hold_present") - c_pres);
			step_timeout("SERVE take of", a, (uint64_t)busy * 1000);
		}
		/*
		 * INSTRUMENT (session 40, the ~1% start-up stall): the hold
		 * is the whole thread group's -- any sibling's in-flight
		 * assisted call that registered the page, or any RUNNING
		 * sibling call while the page is present -- so name every
		 * sibling's call and state at the give-up: /proc/<tid>/syscall
		 * says which call and (for a blocked one) where it sleeps,
		 * stat/wchan says running vs waiting in vmctx_report. The
		 * record's own state for the page rides along. Own cap.
		 */
		if (n_take_gaveup < 6) {
			char dir[64], path[96], line[256], ph[160];
			DIR *d;
			struct dirent *e;

			fprintf(stderr, "[vmhome] HOLDERS of 0x%llx (record %s) "
				"at the give-up, address space %u:\n",
				(unsigned long long)a, pg_stname(pg_state(pid, a)),
				as_of(pid));
			snprintf(dir, sizeof(dir), "/proc/%d/task", (int)as_of(pid));
			d = opendir(dir);
			while (d && (e = readdir(d))) {
				FILE *f;
				pid_t tid = (pid_t)atoi(e->d_name);

				if (tid <= 0)
					continue;
				snprintf(path, sizeof(path), "%s/%d/syscall", dir,
					 (int)tid);
				f = fopen(path, "r");
				line[0] = 0;
				if (f) {
					if (!fgets(line, sizeof(line), f))
						line[0] = 0;
					fclose(f);
				}
				line[strcspn(line, "\n")] = 0;
				fprintf(stderr, "[vmhome]   tid %d: %s | syscall: %.120s\n",
					(int)tid, task_photo(tid, ph, sizeof(ph)),
					line);
			}
			if (d)
				closedir(d);
		}
		n_take_gaveup++;
		sv->status = VMCTX_SERVE_CLAIMING;
		return 0;
	}
	return r < 0 ? -errno : 0;
}

/*
 * Bytes at an address in this context's address space, for the machine that
 * runs the program -- the source half of the hand-over (ARCHITECTURE §5).
 *
 * Serving a page gives it away, and that is the whole coherence rule. There
 * are two copies of the program's memory -- the one this task holds and the
 * one the machine running the program's instructions holds -- and either
 * side may write. Neither side can know what the other wrote, so the only
 * way for a read to be right is for exactly one side to have the page at a
 * time, and for the other to have to ask. Both halves of that are pulls:
 * this is one of them (the other machine faults, asks here, and the page
 * leaves this address space), and ctx_monitor() is the other (this side's
 * task faults, asks there with VMR_PG_GET, and the page comes back).
 *
 * Every question this used to answer by itself -- what kind of mapping is at
 * the address (the maps text), whether this side holds the page (the
 * pagemap), what the record says, whether a take succeeded and whether the
 * page was still here afterwards (two more pagemap reads), a copy kept in
 * case a refused take destroyed the page -- is the kernel's now, answered
 * under one lock by VMCTX_CTL_SERVE (the decision table is at its
 * definition). What is left here is the wire vocabulary: TAKEN and COPIED
 * are bytes; ABSENT, CLAIMING, DENIED and NOMAP are the four honest answers
 * vmrproto.h names, and -EFAULT is the last two when nothing at all was
 * served.
 *
 * A request that spans the end of a mapping is answered with the bytes that
 * do exist, and the caller learns where the memory stops from the short
 * length -- exactly as a short read does.
 */
static long ctx_page_inner(pid_t pid, uint64_t addr, uint64_t len, char *out,
			   size_t outmax)
{
	uint64_t off = 0;
	long rc;

	if (pid <= 0)
		return -ESRCH;
	if (len > outmax)
		len = outmax;
	/*
	 * The watched page's presence across a serve of some *other* address,
	 * so that a page reappearing here can be pinned to the thing that
	 * brought it back rather than guessed at from the next event.
	 */
	if (pglog_on == 2 && (pglog_page < addr || pglog_page >= addr + len) &&
	    ctx_present(pid, pglog_page) == 1)
		plog(pglog_page, "SERVE of 0x%llx+%llu: watched page is "
		     "present here", (unsigned long long)addr,
		     (unsigned long long)len);

	while (off < len) {
		uint64_t a = addr + off;
		uint64_t n = VMR_PG_SIZE - (a & (VMR_PG_SIZE - 1));
		struct vmctx_serve sv;
		long st;

		if (n > len - off)
			n = len - off;
		/*
		 * Whole pages only: a partial first or last page is served
		 * through a copy of the whole page, and the bytes the asker
		 * wanted are lifted out of it. The kernel serves pages; a
		 * sub-page read of a movable page would be a copy that keeps
		 * this side a holder, which is the one thing a serve of
		 * writable memory must not do -- and the destination asks
		 * for whole pages in every path that matters.
		 */
		if (n != VMR_PG_SIZE) {
			static __thread char whole[VMR_PG_SIZE];

			rc = ctx_serve_one(pid, a & ~(uint64_t)(VMR_PG_SIZE - 1),
					   whole, &sv, off == 0);
			if (rc < 0)
				return off ? (long)off : rc;
			if (sv.status != VMCTX_SERVE_TAKEN &&
			    sv.status != VMCTX_SERVE_COPIED)
				break;
			memcpy(out + off, whole + (a & (VMR_PG_SIZE - 1)), n);
			off += n;
			continue;
		}

		rc = ctx_serve_one(pid, a, out + off, &sv, off == 0);
		if (rc < 0) {
			plog(a, "SERVE: the context cannot be asked (%s)",
			     strerror((int)-rc));
			return off ? (long)off : rc;
		}
		st = sv.status;
		if (pglog_on)
			plog(a, "SERVE %s class=0x%x record-was=%s gen=%u sum=%04x",
			     serve_stname((unsigned)st), sv.class,
			     pg_stname((int)sv.state), sv.gen, sv.sum);
		if (sh_file_has(pid, a)) {
			n_shfile_served++;
			if (!(sv.class & VMCTX_PGC_SHARED))
				n_shfile_nomov++;
		}
		switch (st) {
		case VMCTX_SERVE_TAKEN:
			n_mov_hit++;
			/* serve-out: the bytes this side is about to send the
			 * other machine. If a slot here is below its mark,
			 * this side is serving a stale copy. */
			src_clobber(a, out + off, 1);
			serve_gen_note(off, sv.gen);
			if (len == VMR_PG_SIZE) {
				ctxpage_taken |= 1u << (off >> 12);
				n_intransit_committed++;
				conn_transit_note(a);
			} else {
				/* Cold bulk: settled THEIRS here, no ack. */
				struct vmctx_pgack ack = { .addr = a,
							   .sum = sv.sum };

				ctl(pid, VMCTX_CTL_PGACK, &ack);
			}
			off += VMR_PG_SIZE;
			continue;
		case VMCTX_SERVE_COPIED:
			n_mov_miss++;
			if (sv.class & VMCTX_PGC_RO)
				n_ro_served++;
			serve_gen_note(off, sv.gen);
			off += VMR_PG_SIZE;
			continue;
		case VMCTX_SERVE_CLAIMING:
			/*
			 * This side is fetching the page, has it on the wire
			 * to the asker unacknowledged, or is landing a local
			 * fill: ask again. What is served is served; a first
			 * page says so in its own word.
			 */
			n_serve_refused++;
			if (sv.state == PG_INTRANSIT)
				n_serve_intransit_refused++;
			if (off)
				goto done;
			return VMR_CTXPAGE_CLAIMING;
		case VMCTX_SERVE_ABSENT:
			/*
			 * Not this side's to give: the asker's own record
			 * decides what it holds (its object, a sibling's
			 * landing, or -- for an address nobody has written --
			 * its kernel's zeros).
			 */
			n_serve_absent++;
			if (PG_GONE((int)sv.state))
				n_serve_refused++;
			else if (n_serve_absent_fresh++ < 32)
				fprintf(stderr, "[vmhome] ABSENT 0x%llx for "
					"context %d: nothing here to take\n",
					(unsigned long long)a, (int)pid);
			if (off)
				goto done;
			return VMR_CTXPAGE_ABSENT;
		case VMCTX_SERVE_DENIED:
			/*
			 * The program may not touch this address, so neither
			 * may the machine running its instructions. Stopping
			 * here answers the request short -- EFAULT if it is
			 * the first page -- and the far side turns that into
			 * the exception that comes back here for this side to
			 * interpret.
			 */
			plog(a, "SERVE refused: the program has no access to "
			     "this range");
			goto done;
		case VMCTX_SERVE_NOMAP:
		default:
			goto done;
		}
	}
done:
	if (!off && len) {
		/*
		 * Nothing here answered: the address is a fault, and only the
		 * context's own kernel ruled it one (NOMAP or DENIED above,
		 * by the fault path's own lookup). The line is diagnosis, not
		 * decision; capped, a guard-page EFAULT is ordinary.
		 */
		static long n_efault_diag;

		if (n_efault_diag++ < 16) {
			char ml[512];

			ctx_maps_line(pid, addr, ml, sizeof(ml));
			fprintf(stderr, "[vmhome] EFAULT-DIAG ctx %d 0x%llx: "
				"maps: %s\n", (int)pid,
				(unsigned long long)addr, ml);
		}
		return -EFAULT;
	}
	return off ? (long)off : 0;
}

/*
 * One context is done with the program. The last one out kills what is left —
 * and only the last one.
 */
static void oracle_put(uint64_t ctx)
{
	int left;

	pthread_mutex_lock(&ctx_count_lock);
	left = n_contexts > 0 ? --n_contexts : 0;
	pthread_mutex_unlock(&ctx_count_lock);
	if (left) {
		fprintf(stderr, "[vmhome %llu] done with the program; %d "
			"context(s) still hold it\n",
			(unsigned long long)ctx, left);
		return;
	}
	if (ctx0_pid) {
		int st = 0;
		pid_t w;

		ctx_scratch_forget(ctx0_pid);
		/*
		 * INSTRUMENT (session 40): the service process is this
		 * process's child, so its wait status is the one record of
		 * HOW it ended -- a group exit's code, or the signal that
		 * killed it -- and a SIGKILL sent to a group already in
		 * do_exit changes neither. Read before it is thrown away.
		 */
		w = waitpid(ctx0_pid, &st, WNOHANG);
		if (w == ctx0_pid)
			fprintf(stderr, "[vmhome] service process %d had ALREADY "
				"ended when the last context let go: %s %d\n",
				(int)ctx0_pid,
				WIFSIGNALED(st) ? "killed by signal" : "exit status",
				WIFSIGNALED(st) ? WTERMSIG(st) : WEXITSTATUS(st));
		kill(ctx0_pid, SIGKILL);
		if (w != ctx0_pid && waitpid(ctx0_pid, &st, 0) == ctx0_pid)
			fprintf(stderr, "[vmhome] service process %d ended on "
				"the final SIGKILL: %s %d\n", (int)ctx0_pid,
				WIFSIGNALED(st) ? "killed by signal" : "exit status",
				WIFSIGNALED(st) ? WTERMSIG(st) : WEXITSTATUS(st));
		ctx0_pid = 0;
	}
	if (ctx0_mem >= 0) {
		close(ctx0_mem);
		ctx0_mem = -1;
	}
}

/* ---- the server -------------------------------------------------------- */

struct conn_arg {
	int  fd, verbose;
	char peer[64];
};

static void serve(int c, int verbose, const char *peer);

static void *conn_thread(void *arg)
{
	struct conn_arg *ca = arg;

	serve(ca->fd, ca->verbose, ca->peer);
	close(ca->fd);
	free(ca);
	return NULL;
}

static void serve(int c, int verbose, const char *peer)
{
	static __thread char in[MAXDATA], out[MAXDATA];
	unsigned long n = 0;
	int counted = 0;	/* this connection holds a reference on the program */

	snprintf(conn_peer, sizeof(conn_peer), "%s", peer ? peer : "127.0.0.1");
	conn_ctx = 0;
	conn_ctxpid = 0;
	conn_is_thread = 0;
	conn_ctid = 0;
	conn_have_status = 0;

	for (;;) {
		struct vmr_req rq;
		struct vmr_rsp rs;
		long ret = -ENOSYS;
		size_t outlen = 0;
		int regs_valid = 0;
		/*
		 * Held aside rather than written straight into the reply: the
		 * reply is memset immediately below, after the call that fills
		 * it, so anything put there would be zeroed before being sent —
		 * installing a register set of all zeros.
		 */
		struct vmr_uregs sregs;

		int got = read_all(c, &rq, sizeof(rq));

		if (got <= 0)
			break;
		if (rq.magic != VMR_MAGIC) {
			fprintf(stderr, "[vmhome] bad magic 0x%x\n", rq.magic);
			break;
		}
		if (rq.datalen > sizeof(in)) {
			fprintf(stderr, "[vmhome] payload too big\n");
			break;
		}
		if (rq.datalen && read_all(c, in, rq.datalen) <= 0)
			break;

		errno = 0;

		/*
		 * An exception the guest raised, which is not a syscall: what
		 * the owner needs in order to decide what the program should
		 * experience is the whole machine state, not six arguments.
		 */
		if (rq.nr == VMR_NR_EXCEPTION) {
			const struct vmr_uregs *u =
				rq.datalen >= sizeof(struct vmr_uregs)
				? (const struct vmr_uregs *)in : NULL;

			if (!u || conn_ctxpid <= 0) {
				ret = -EINVAL;
			} else {
				int d = ctx_deliver_exception(conn_ctxpid,
							      rq.args[0],
							      rq.args[1],
							      rq.args[2], u,
							      &sregs);

				if (d == VMR_EXC_LEGAL) {
					ret = VMR_EXC_LEGAL;
				} else if (d) {
					regs_valid = 1;
					ret = 0;
				} else {
					ret = -EFAULT;
				}
			}
			memset(&rs, 0, sizeof(rs));
			rs.magic   = VMR_MAGIC;
			rs.retval  = ret;
			rs.regs_valid = regs_valid;
			if (regs_valid)
				rs.regs = sregs;
			n++;
			if (write_all(c, &rs, sizeof(rs)) < 0)
				break;
			fflush(NULL);
			continue;
		}

		/*
		 * Every other syscall is performed by the task that owns the
		 * program. Nothing here knows what any particular one does, and
		 * no argument is marshalled: a pointer already means what the
		 * program meant by it.
		 */
		if (rq.nr < 0x1000 && conn_ctxpid > 0) {
			/*
			 * The other half of the futex picture.
			 *
			 * The destination logs the word as IT sees it when it
			 * sends the call; this logs the word as THIS side sees
			 * it just before performing it, and the two together
			 * are the whole question. A wait blocks here if this
			 * side's copy equals what the guest expected -- so if
			 * the two machines disagree about the word, one of them
			 * is waiting on a value the other has already moved
			 * past, and no wake can be right.
			 *
			 * Read present-only. A peek of an absent page CREATES
			 * it (see the SERVE ABSENT note), and an instrument
			 * that changes the state it measures is worse than no
			 * instrument at all.
			 */
			if (futex_log && rq.nr == 202 &&
			    ctx_present(conn_ctxpid, rq.args[0] & ~0xfffULL) == 1) {
				uint32_t hw = 0xdeadbeef;
				unsigned op = (unsigned)(rq.args[1] & 0x7f);

				if (ctx_peek(conn_ctxpid, rq.args[0], &hw, 4) == 4)
					fprintf(stderr, "[vmhome %llu] futex %s "
						"addr=0x%llx arg=%llu "
						"word-HERE=%u\n",
						(unsigned long long)conn_ctx,
						op == 0 ? "WAIT" : op == 1 ? "WAKE" :
						op == 5 ? "WAKE_OP" :
						op == 9 ? "WAIT_BITSET" :
						op == 10 ? "WAKE_BITSET" : "op?",
						(unsigned long long)rq.args[0],
						(unsigned long long)rq.args[2], hw);
			}
			ret = ctx_forward(conn_ctxpid, &rq,
					  rq.datalen >= sizeof(struct vmr_uregs)
					  ? (const struct vmr_uregs *)in : NULL,
					  &sregs, &regs_valid, verbose);
			if (verbose)
				fprintf(stderr, "[vmhome %llu] %-11s(0x%llx, 0x%llx, "
					"0x%llx, 0x%llx)%s%s -> %ld\n",
					(unsigned long long)conn_ctx,
					sysdesc(rq.nr),
					(unsigned long long)rq.args[0],
					(unsigned long long)rq.args[1],
					(unsigned long long)rq.args[2],
					(unsigned long long)rq.args[3],
					last_path[0] ? " " : "", last_path, ret);
			/*
			 * Zeroed whole, not field by field. The reply carries a
			 * register set that most replies do not, and a
			 * stack-allocated struct with only its old fields
			 * assigned would tell the far side to install whatever
			 * happened to be on the stack.
			 */
			memset(&rs, 0, sizeof(rs));
			rs.magic   = VMR_MAGIC;
			rs.retval  = ret;
			rs.regs_valid = regs_valid;
			if (regs_valid)
				rs.regs = sregs;
			rs.map_shared = last_map_shared;
			rs.map_off    = last_map_off;
			rs.map_len    = last_map_len;
			/*
			 * The vacated ranges the hook drained during this (or
			 * a concurrent) call. Up to 8 per reply; the rest ride
			 * the next ones -- the destination only ever LOSES
			 * ranges through vacate_overflow, which it reports
			 * loudly.
			 */
			{
				unsigned vi, take = vacate_pend_n > 8
						    ? 8 : vacate_pend_n;

				for (vi = 0; vi < take; vi++) {
					rs.vacate[vi].start =
						vacate_pend[vi].start;
					rs.vacate[vi].end = vacate_pend[vi].end;
					rs.vacate[vi].seq = vacate_pend[vi].seq;
				}
				rs.nvacate = take;
				rs.mmseq = last_mmseq;
				n_vacates_shipped += take;
				if (take) {
					memmove(vacate_pend, vacate_pend + take,
						(vacate_pend_n - take) *
						sizeof(vacate_pend[0]));
					vacate_pend_n -= take;
				}
				rs.vacate_overflow = vacate_ovf_pend;
				vacate_ovf_pend = 0;
			}
			/*
			 * A clone/clone3 the source just performed. Its own kernel
			 * built the child -- a thread of this context (shared mm) or
			 * a new process (its own copy) -- reading the real clone_args
			 * out of the program's own memory, so neither this side nor
			 * the destination parses a flag. Tell the destination, in
			 * neutral terms, to stand up an execution context for it:
			 *   retval        the child's pid, to answer the guest's clone
			 *   clone_thread  whether it shares this context's backing
			 *   regs          the frame the kernel prepared for the child
			 *                 -- RAX 0, RSP and FS as the clone set them.
			 * regs carries the CHILD's frame, not the parent's, so
			 * regs_valid is cleared: the parent that issued the clone
			 * carries on unchanged but for its return value, already in
			 * retval. The child's monitor is started when the child's own
			 * connection announces itself (VMR_OP_CHILD), keeping the
			 * page/recall channel destination-initiated as it is for every
			 * other context.
			 */
			if (is_clone_nr(rq.nr) && ret > 0) {
				struct vmr_uregs cg;
				pid_t kid = (pid_t)ret;
				uint64_t hp = 0;

				/*
				 * ret is the pid as the PARENT'S pid namespace
				 * names it -- the guest's right answer, kept in
				 * retval -- but everything THIS side does with
				 * the child (as_of, GETREGS, fork_rel, the
				 * OP_CHILD adoption, kill, /proc) addresses the
				 * HOST namespace. Inside a sandbox's fresh pid
				 * namespace (bwrap --unshare-pid) the two
				 * diverge: the payload fork returned "2", and
				 * host pid 2 is kthreadd, so the real child
				 * parked in vmctx_report with nobody ever
				 * adopting it. The kernel recorded the host
				 * view on the parent at kernel_clone(); read it
				 * back. A kernel without the ctl answers
				 * EINVAL and hp stays 0 -- same-namespace
				 * behavior, unchanged.
				 */
				if (ctl(conn_ctxpid, VMCTX_CTL_LASTCHILD,
					&hp) == 0 && hp > 0)
					kid = (pid_t)hp;
				if (kid != (pid_t)ret)
					fprintf(stderr, "[vmhome] clone: child "
						"is pid %ld in its namespace, "
						"host pid %d here\n",
						ret, (int)kid);
				rs.host_pid = kid;

				rs.clone_thread =
					(as_of(kid) == as_of(conn_ctxpid));
				/*
				 * A fork, not a thread: parent and child map the
				 * same memory until one writes. Record it -- the
				 * child's object on the other machine starts
				 * empty, so every page it has not written must be
				 * served from the parent, and this is the only
				 * place that knows the two are related.
				 */
				if (!rs.clone_thread) {
					fork_rel_note(kid, conn_ctxpid);
					fprintf(stderr, "[vmhome] fork: child %d "
						"shares the parent %d's memory "
						"until one of them writes\n",
						(int)kid, (int)conn_ctxpid);
				}
				rs.regs_valid = 0;
				/*
				 * Retried, because a single failure here is not a
				 * missing frame -- it is a WRONG frame. The reply
				 * struct was memset above, so a failed GETREGS
				 * sends fs_base 0, and for a THREAD the
				 * destination then falls back to the parent's
				 * base: every fs-relative access in the new
				 * thread reads its creator's TLS, which is the
				 * shared-thread-pointer corruption fs1 exists to
				 * catch. Measured twice in one pooled repeat
				 * batch (death frames with fs_base == main's
				 * static TCB). The child was created by this
				 * very process one line ago, so a failure is a
				 * transient of its birth and yields to a retry.
				 */
				{
					int gt;
					long gr = -1;

					for (gt = 0; gt < 200; gt++) {
						gr = ctl(kid,
							 VMCTX_CTL_GETREGS, &cg);
						if (gr == 0)
							break;
						usleep(1000);
					}
					if (gr == 0) {
						rs.regs = cg;
						if (gt)
							fprintf(stderr, "[vmhome] "
								"clone child %d: "
								"GETREGS needed %d "
								"retries\n",
								(int)kid, gt);
					} else {
						fprintf(stderr, "[vmhome] clone "
							"child %d: GETREGS failed "
							"(%s) after %d tries; the "
							"destination has no frame "
							"to start it\n", (int)kid,
							strerror(errno), gt);
					}
				}
			}
			n++;
			if (write_all(c, &rs, sizeof(rs)) < 0)
				break;
			fflush(NULL);
			continue;
		}

		switch (rq.nr) {
		case VMR_OP_START:
			ret = oracle_start(in, rq.datalen, out, sizeof(out));
			outlen = (ret > 0) ? (size_t)ret : 0;
			if (ret > 0) {
				pthread_mutex_lock(&ctx_count_lock);
				n_contexts++;
				pthread_mutex_unlock(&ctx_count_lock);
				counted = 1;
				conn_ctxpid = ctx0_pid;
				/*
				 * Before the first forwarded call, and so before
				 * the first fault: a context whose fault nobody
				 * is waiting for simply sleeps in the kernel.
				 */
				ctx_monitor_start(conn_ctxpid, conn_ctx,
						  conn_peer, g_port);
			}
			break;
		case VMR_OP_CHILD: {
			/*
			 * A child the source has already made. The guest cloned, the
			 * destination forwarded that clone like any other syscall, and this
			 * context's own kernel built the child -- a thread of a context
			 * (shared address space) or a new process -- returning its pid in the
			 * clone reply. That pid is args[1]; args[0] is the destination's
			 * context id for it. The child announces itself on its own connection
			 * so it gets its own monitor and page/recall channel, exactly as the
			 * first context does. Nothing is forked here and no clone flag is
			 * read: the child already exists, correctly, by the rules of the
			 * machine that owns the program.
			 */
			pid_t srcpid = (pid_t)rq.args[1];

			conn_ctx = rq.args[0];
			pthread_mutex_lock(&ctx_count_lock);
			n_contexts++;
			pthread_mutex_unlock(&ctx_count_lock);
			counted = 1;

			if (srcpid <= 0) {
				ret = -ESRCH;
				fprintf(stderr, "[vmhome] child context %llu: the "
					"destination named no source pid\n",
					(unsigned long long)conn_ctx);
				break;
			}
			conn_ctxpid = srcpid;
			/*
			 * Whether it is a thread of another context (it shares that
			 * context's thread group) or a process of its own, and -- if a
			 * thread -- the clear-tid word its clone named. Both come from the
			 * kernel that ran the real clone, never from a flag read here: the
			 * thread bit is whether the child shares a thread group, and the
			 * ctid is the word CLONE_CHILD_CLEARTID recorded, read back with
			 * ctx_clear_tid(). They are used only at teardown, to clear that
			 * word coherently and wake a joiner.
			 */
			conn_is_thread = (as_of(srcpid) != (uint32_t)srcpid);
			conn_ctid = conn_is_thread ? ctx_clear_tid(srcpid) : 0;
			ret = 0;
			ctx_map_add(conn_ctx, conn_ctxpid);
			ctx_monitor_start(conn_ctxpid, conn_ctx, conn_peer, g_port);
			fprintf(stderr, "[vmhome] child guest context %llu is service "
				"context %d (%s)\n", (unsigned long long)conn_ctx,
				(int)conn_ctxpid, conn_is_thread ? "thread" : "process");
			}
			break;
		case VMR_OP_LAYOUT:
			/*
			 * args[0]==1: the CALLING context's current map -- the
			 * post-exec relayout. Anything else keeps the original
			 * meaning, the oracle's map at start.
			 */
			if (rq.args[0] == 1 && conn_ctxpid > 0)
				ret = pid_layout(conn_ctxpid, out, sizeof(out));
			else
				ret = oracle_layout(out, sizeof(out));
			outlen = (ret > 0) ? (size_t)ret : 0;
			break;
		case VMR_OP_PAGE:
			ret = oracle_page(rq.args[0], rq.args[1], out, sizeof(out));
			outlen = (ret > 0) ? (size_t)ret : 0;
			break;
		case VMR_OP_CTXPAGE:
			/*
			 * args[2] is the far side saying it has already tried
			 * to break copy-on-write for this page and had nothing
			 * to copy from, so answer as if no fork had happened.
			 * See cow_still_shared().
			 *
			 * A page belongs to the ADDRESS SPACE, not to a task.
			 * This connection names the context that asked, and by
			 * the time the request arrives that context may have
			 * exited -- a thread that finished while the far side
			 * still had a fault outstanding for a page of the
			 * program's memory. The kernel answers -ESRCH for a
			 * task without an address space (a zombie included:
			 * kill(who, 0) is not the test); the threads of a
			 * program share one, so any live sibling answers the
			 * same question with the same answer, and failing one,
			 * the program's FIRST context -- main, alive as long as
			 * the program is, mapping everything a thread maps.
			 * Measured on ws1, before this: main's own stack
			 * declared missing (-14), and the guest read zeros.
			 */
			{
				pid_t who = conn_ctxpid > 0 ? conn_ctxpid : ctx0_pid;
				uint64_t cp0 = now_us();

				ret = who > 0 ? ctx_page(who, rq.args[0], rq.args[1],
							 out, sizeof(out),
							 (int)rq.args[2])
					      : -ESRCH;
				/*
				 * INSTRUMENT (session 40): a CTXPAGE that took
				 * over a second is the stuck step the far side's
				 * SLOW PULL line names from its end; this names
				 * it from this end -- what was answered, through
				 * which context, and how long the kernel's serve
				 * sat in its hold loop (n_take_busy before/after).
				 */
				if (now_us() - cp0 > 1000000) {
					static unsigned long n_slow;

					if (++n_slow <= 8)
						fprintf(stderr, "[vmhome %llu] SLOW "
							"CTXPAGE 0x%llx+%llu through "
							"context %d: %llu ms -> %ld "
							"(busy-waits so far %lu, "
							"gave up %lu) abs=%llu\n",
							(unsigned long long)conn_ctx,
							(unsigned long long)rq.args[0],
							(unsigned long long)rq.args[1],
							(int)who,
							(unsigned long long)((now_us() - cp0) / 1000),
							ret, n_take_busy,
							n_take_gaveup,
							(unsigned long long)now_us());
				}
				if (ret == -ESRCH || ret == -ENXIO) {
					pid_t alt = as_live_ctx(who, who);

					if (alt <= 0 && ctx0_pid > 0 &&
					    ctx0_pid != who)
						alt = ctx0_pid;
					n_ctxpage_gone++;
					if (alt > 0) {
						n_ctxpage_resited++;
						if (n_ctxpage_resited <= 8)
							fprintf(stderr, "[vmhome] context %d is gone; "
								"answering for 0x%llx through its live "
								"sibling %d of the same address space "
								"rather than saying the address does "
								"not exist\n", (int)who,
								(unsigned long long)rq.args[0],
								(int)alt);
						ret = ctx_page(alt, rq.args[0],
							       rq.args[1], out,
							       sizeof(out),
							       (int)rq.args[2]);
					}
				}
				if (ret == -ESRCH || ret == -ENXIO) {
					/*
					 * THE MEASUREMENT THAT DECIDES IT. The
					 * lookup has failed. Has the address
					 * space actually ended, or is this a
					 * moment in which nothing could be
					 * found? Only on the FACT is the far
					 * side told to stop; three repairs that
					 * could not tell those apart all
					 * measured worse (§24.3).
					 */
					if (as_has_ended(as_of(who)))
						n_ctxpage_gone_ended++;
					else
						n_ctxpage_gone_transient++;
					if (n_ctxpage_gone <= 8)
						fprintf(stderr, "[vmhome] context %d is gone and no "
							"sibling of its address space is left; 0x%llx "
							"cannot be answered by this side (address "
							"space %s)\n",
							(int)who,
							(unsigned long long)rq.args[0],
							as_has_ended(as_of(who))
							? "HAS ENDED" : "is still alive");
					if (as_has_ended(as_of(who))) {
						n_ctxpage_gone_told++;
						ret = VMR_CTXPAGE_GONE;
					} else {
						ret = VMR_CTXPAGE_CLAIMING;
					}
				}
			}
			outlen = (ret > 0) ? (size_t)ret : 0;
			/*
			 * ctx_page's answer is the answer, and the blanket
			 * "if (ret < 0 && errno) ret = -errno" below must not
			 * rewrite it.
			 */
			errno = 0;
			break;
		case VMR_OP_STALETRAIL:
			/*
			 * The destination was served a copy older than its own
			 * last capture of the page and is asking this side to
			 * say what it did with it. Diagnosis only.
			 */
			n_staletrail_asked++;
			fprintf(stderr, "[vmhome] the guest's machine reports a "
				"STALE SERVE of 0x%llx (it holds a newer capture "
				"than the copy this side served):\n",
				(unsigned long long)rq.args[0]);
			fprintf(stderr, "[vmhome]   record now: %s gen %u\n",
				pg_stname(pg_state(conn_ctxpid > 0 ? conn_ctxpid
							       : ctx0_pid,
						   rq.args[0])),
				pg_gen(conn_ctxpid > 0 ? conn_ctxpid : ctx0_pid,
				       rq.args[0]));
			ret = 0;
			errno = 0;
			break;
		case VMR_OP_WRITEPREP:
			/*
			 * The destination is holding a page and is about to let
			 * one of its contexts write it. It reports that and
			 * nothing else; what has to happen first is this side's
			 * to decide, because the relationships between address
			 * spaces are facts of the kernel that ran the clone.
			 */
			ret = ctx_write_prep(conn_ctxpid > 0 ? conn_ctxpid
							    : ctx0_pid,
					     rq.args[0]);
			errno = 0;	/* the answer is the answer; see CTXPAGE */
			break;
		case VMR_OP_INSTALLED:
			/*
			 * The two-phase hand-over's settle: the destination
			 * has INSTALLED the page a single-page serve put on
			 * the wire. args[1] echoes the low 16 bits of the
			 * served bytes' sum -- the episode guard: a late ack
			 * from an earlier hand-over of the same page must not
			 * retire a newer one, and the record's sum was stamped
			 * by the serve this ack answers. Anything that does not
			 * match is counted and ignored; the cost of ignoring is
			 * one bounded CLAIMING yield on the next ask, never a
			 * wrong page. No reply (VMR4): the channel is in order
			 * and nothing waits on this.
			 */
			{
				struct vmctx_pgack a = {
					.addr = rq.args[0] & ~(uint64_t)4095,
					.sum = (uint32_t)(rq.args[1] & 0xffff),
				};
				pid_t who = conn_ctxpid > 0 ? conn_ctxpid
							    : ctx0_pid;
				long r = who > 0 ? ctl(who, VMCTX_CTL_PGACK, &a)
						 : -1;

				if (r == 1)
					n_intransit_acked++;
				else
					n_intransit_ack_stale++;
				conn_transit_forget(a.addr);
			}
			n++;
			errno = 0;
			continue;	/* one-way: no reply goes back */
		case VMR_OP_MAPSYNC:
			/*
			 * Nothing to synchronise. There is one address space
			 * and it is the program's; the mapping calls were
			 * performed in it, so it is never out of step with
			 * itself.
			 */
			ret = 0;
			break;
		case VMR_OP_FINISH:
			/*
			 * The context has ended, and this is the last moment
			 * its shadow still has its mappings. A shared file
			 * range the guest was holding pages of has to come
			 * back now or never: nothing else on this side will
			 * ever read that mapping again, and munmap -- the only
			 * other place that reclaims one -- is a call the guest
			 * never made, because it simply exited.
			 *
			 * That is AUDIT VII's `rd1 exit-writeback`, and the
			 * "announce an exit before tearing the context down"
			 * it says the destination would have to do is this
			 * message, which the destination has always sent.
			 */
			if (conn_ctxpid > 0)
				sh_file_reclaim_held(conn_ctxpid);
			if (rq.args[0]) {
				int st = (int)(uint32_t)rq.args[1];

				conn_guest_status = st;
				conn_have_status = 1;

				fprintf(stderr, "[vmhome] guest ended: %s %d\n",
					WIFEXITED(st) ? "exit status" : "killed by signal",
					WIFEXITED(st) ? WEXITSTATUS(st)
						      : WTERMSIG(st));
			}
			if (counted) {
				counted = 0;
				oracle_put(conn_ctx);
			}
			ret = 0;
			break;
		default:
			ret = -ENOSYS;
			break;
		}
		if (ret < 0 && errno)
			ret = -errno;

		n++;
		/*
		 * A page request is the hottest message in the system and its
		 * answer is in the end-of-run summary (and VMHOME_PGLOG, per
		 * page); a line per request is two syscalls on that path.
		 */
		if (verbose && rq.nr != VMR_OP_CTXPAGE)
			fprintf(stderr, "[vmhome %llu] %-11s -> %ld%s\n",
				(unsigned long long)conn_ctx,
				sysdesc(rq.nr), ret,
				outlen ? " (+data)" : "");

		memset(&rs, 0, sizeof(rs));
		rs.magic   = VMR_MAGIC;
		rs.retval  = ret;
		rs.datalen = outlen;
		/* The stamps of the pages this answer serves; see ctxpage_gen. */
		if (rq.nr == VMR_OP_CTXPAGE && ret > 0) {
			rs.ngen = ctxpage_ngen;
			memcpy(rs.pggen, ctxpage_gen, sizeof(rs.pggen));
			rs.map_off = ctxpage_taken;	/* which pages want an ack */
		}
		/* Header and payload in one write: one wakeup on the far side. */
		if (writev_all(c, &rs, sizeof(rs), outlen ? out : NULL,
			       outlen) < 0)
			break;
		if (rq.nr != VMR_OP_CTXPAGE)
			fflush(NULL);
	}
	if (counted) {
		counted = 0;
		oracle_put(conn_ctx);
	}
	/*
	 * End this context with its connection, so it does not keep the guest's
	 * inherited descriptors open: the guest's own parent then waits forever
	 * for an end-of-file that cannot arrive, because the write end of the
	 * pipe it is reading is still held here.
	 *
	 * Not the first context, which is the program itself and is released by
	 * the reference count.
	 */
	/*
	 * An address space that has ended is a copy of nobody's. Left in the
	 * table, every store the parent makes to one of its own pages costs a
	 * round trip to be told that nothing is owed.
	 */
	if (conn_ctxpid > 0)
		fork_rel_forget(conn_ctxpid);
	if (conn_ctxpid > 0 && conn_ctxpid != ctx0_pid) {
		ctx_map_forget(conn_ctx);
		ctx_scratch_forget(conn_ctxpid);
		if (conn_is_thread) {
			/*
			 * A thread, so end just that task — and a signal cannot
			 * do it.
			 *
			 * tgkill names one task, but SIGKILL is not deliverable
			 * to one: every fatal signal is decided for the whole
			 * thread group, so the kernel sets SIGNAL_GROUP_EXIT and
			 * kills every task sharing the mm. Aiming it at the
			 * thread makes no difference at all. The program the
			 * source is holding therefore died the moment any guest
			 * thread finished.
			 *
			 * Measured on pg1, whose stamper thread returns while
			 * main is in pthread_join: the thread's connection
			 * closed, and in the next three lines main's forwarded
			 * futex came back -ERESTARTSYS, the monitor of main's
			 * own context reported "fault service ends" because its
			 * task had gone, and the program exited 242 with all
			 * four hundred of its rounds correct and none of its
			 * closing output. It looked exactly like a coherence
			 * failure in the join.
			 *
			 * exit(2) is the call that ends one task, and it is what
			 * a thread returning normally would have made.
			 *
			 * The clear-tid word has to be cleared here rather than
			 * left to the kernel, and that is the same rule again.
			 * CLONE_CHILD_CLEARTID makes the kernel write zero into
			 * the program's memory as the task dies -- a write by
			 * this side into a page the machine running the guest may
			 * be holding, made from inside do_exit where a fault
			 * cannot be redirected to a monitor and a page cannot be
			 * fetched. The zero lands in a page this side invents,
			 * the guest keeps its own copy with the old tid in it,
			 * and pthread_join reads that copy for ever.
			 *
			 * Measured: with the kernel left to do it, pg1 finished
			 * all four hundred rounds and then spun in
			 * futex(0x7ffff7ff5ce8, FUTEX_WAIT_BITSET, 1) -> -EAGAIN
			 * -- EAGAIN being the futex saying "that word is not the
			 * value you think", which is precisely two copies of one
			 * word disagreeing. 279468 forwarded syscalls in 45s and
			 * no progress.
			 *
			 * Written with ctx_poke_owned(), so the page is taken
			 * from the guest first and the guest's next read of it
			 * faults and gets the zero. Then a wake, for a joiner
			 * already blocked rather than spinning.
			 */
			if (conn_ctid) {
				uint32_t zero = 0, back = 0xdeadbeef;
				long pk, wk;
				int alive;

				/*
				 * Both of these were unchecked, and both are
				 * issued to conn_ctxpid -- the DYING thread's
				 * service context, which may already be gone.
				 * On the source every service context of one
				 * guest process shares an mm, so a poke that
				 * SUCCEEDS is visible to the joiner; a poke
				 * that silently failed leaves the joiner
				 * waiting on the old tid for ever, which is
				 * exactly the deadlock ws1 dies of.
				 *
				 * So: say whether the task was still there,
				 * what each call returned, and -- the only
				 * thing that actually settles it -- what the
				 * word reads afterwards. Read with ctx_peek,
				 * which clamps and cannot create the page it
				 * reports on.
				 */
				pid_t tgt = conn_ctxpid;

				alive = kill(conn_ctxpid, 0) == 0;
				pk = ctx_poke_owned(tgt, conn_ctid, &zero,
						    sizeof(zero));
				/*
				 * ...and if THAT context cannot take it, any
				 * other context of the same address space can.
				 *
				 * Measured, on the runs ws1 fails: the poke
				 * answers -ESRCH when the dying service context
				 * has already gone, and -EINVAL when it is
				 * still alive but never mapped this page --
				 * POKE's answer for "no mapping at that address
				 * yet". Both were unchecked, so the word was
				 * left holding the old tid and the joiner's
				 * FUTEX_WAIT never returned. That is the
				 * deadlock, and it is not about futex or about
				 * clone: it is a write aimed at one context
				 * when the memory belongs to the address space.
				 */
				if (pk != (long)sizeof(zero)) {
					pid_t alt = as_live_ctx(conn_ctxpid,
								conn_ctxpid);

					if (alt) {
						tgt = alt;
						pk = ctx_poke_owned(tgt,
								    conn_ctid,
								    &zero,
								    sizeof(zero));
						if (pk == (long)sizeof(zero))
							n_ctid_via_sibling++;
					}
				}
				/*
				 * The wake is performed ON THE MM, by the
				 * kernel, from this thread, and AIMED AT A LIVE
				 * MEMBER of the address space -- never at the
				 * dying thread's own context and never through
				 * a context's assisted-call slot.
				 *
				 * Two facts, both measured, force this shape.
				 * (1) VMCTX_CTL_FUTEXWAKE needs get_task_mm() of
				 * a live task; issued to the dying context it
				 * races that context's own exit and returns
				 * -ESRCH the moment it wins -- 462 of 500 re-arm
				 * attempts on the dying context returned -1.
				 * (2) The waiter is the JOINER, which is a live
				 * member sharing this exact mm, so a wake on any
				 * live member reaches it -- the handful that did
				 * land, landed that way.
				 *
				 * And it is RE-ARMED, because the join word is
				 * written-then-woken while the joiner's forwarded
				 * WAIT may not have registered on this side yet:
				 * a one-shot wake is a classic lost wake-up. A
				 * WAIT that arrives after the clear self-heals
				 * (it reads the coherent 0 and returns EAGAIN);
				 * the only unhealed case is a WAIT already asleep
				 * when the wake fired, and re-arming on a live
				 * member ends it. Bounded, and cheap when there
				 * is no waiter: it stops at the first success.
				 */
				{
					pid_t live = as_live_ctx(conn_ctxpid,
								 conn_ctxpid);
					struct vmctx_mem fwm = {
						.addr = conn_ctid,
						.len  = sizeof(uint32_t),
					};
					int tries;

					if (!live)
						live = tgt;
					wk = ctl(live, VMCTX_CTL_FUTEXWAKE, &fwm);
					/*
					 * Re-arm only if the first wake found no
					 * waiter: the joiner may still be in
					 * flight. Re-resolve the live member each
					 * pass -- workers are exiting, and the
					 * one alive now may not be next ms.
					 */
					for (tries = 0; wk == 0 && tries < 5;
					     tries++) {
						usleep(1000);
						live = as_live_ctx(conn_ctxpid,
								   conn_ctxpid);
						if (!live)
							break;
						wk = ctl(live,
							 VMCTX_CTL_FUTEXWAKE,
							 &fwm);
						if (wk > 0)
							n_ctid_wake_late++;
					}
					if (wk < 0 && errno == EINVAL)
						wk = ctx_sys(tgt, SYS_futex,
							     conn_ctid,
							     FUTEX_WAKE, INT_MAX,
							     0, 0, 0);
				}
				if (ctx_present(tgt, conn_ctid) != 1 ||
				    ctx_peek(tgt, conn_ctid, &back,
					     sizeof(back)) != (long)sizeof(back))
					back = 0xdeadbeef;
				/*
				 * The grading tells three states apart, and
				 * the middle one used to be shouted as the
				 * worst. A successful poke whose verify-read
				 * finds the page ABSENT here is the page
				 * already handed to the woken joiner -- the
				 * clear WORKED and its evidence left with it.
				 * 79 such lines per ws1 pool were printed as
				 * "NOT CLEARED ... waits for ever" while all
				 * 48 runs joined and passed; the line that
				 * must stay loud is a READABLE word still
				 * holding the old tid.
				 */
				if (pk == (long)sizeof(zero) && wk >= 0 &&
				    back == 0xdeadbeef) {
					n_ctid_unverified++;
					n_ctid_ok++;
				} else if (pk != (long)sizeof(zero) ||
					   wk < 0 || back != 0) {
					n_ctid_bad++;
					fprintf(stderr, "[vmhome] CLEAR-TID "
						"context %llu (service %d, alive=%d): "
						"poke->%ld wake->%ld word now 0x%x "
						"%s\n",
						(unsigned long long)conn_ctx,
						(int)conn_ctxpid, alive, pk, wk,
						back,
						back == 0 ? "(cleared, but a call failed)"
							  : "<<< NOT CLEARED: a joiner "
							    "waits on this for ever");
				} else {
					n_ctid_ok++;
				}
			}
			ctx_sys(conn_ctxpid, SYS_exit, 0, 0, 0, 0, 0, 0);
		} else if (conn_have_status && WIFEXITED(conn_guest_status)) {
			/*
			 * Exit with the guest's status, and do *not* reap it:
			 * the guest parent's wait4 is answered on this machine,
			 * so this process is the child that wait4 must find.
			 * Reaping it here would take the status away and the
			 * parent would be told ECHILD about a child that had
			 * just exited normally.
			 */
			fprintf(stderr, "[vmhome] context %llu closed: service "
				"context %d exits with the guest's status 0x%x, "
				"left for wait4\n", (unsigned long long)conn_ctx,
				(int)conn_ctxpid, (unsigned)conn_guest_status);
			ctx_sys(conn_ctxpid, SYS_exit_group,
				(uint64_t)WEXITSTATUS(conn_guest_status),
				0, 0, 0, 0, 0);
		} else if (conn_have_status && WIFSIGNALED(conn_guest_status)) {
			kill(conn_ctxpid, WTERMSIG(conn_guest_status));
		} else {
			fprintf(stderr, "[vmhome] context %llu closed: killing "
				"service context %d\n",
				(unsigned long long)conn_ctx, (int)conn_ctxpid);
			kill(conn_ctxpid, SIGKILL);
			waitpid(conn_ctxpid, NULL, WNOHANG);
		}
	}
	/*
	 * Transits die with their connection. Every page still listed here
	 * was committed IN TRANSIT by this connection's serves and never
	 * acked; nobody else's ack is coming (they arrive on the connection
	 * that served), so the record would hold TRANSIT for ever and every
	 * sibling fault on the page would be answered CLAIMING for the rest
	 * of the run. Settle each one still in TRANSIT to THEIRS: the bytes
	 * went toward that machine, whose retained copy is the recovery, and
	 * a page that truly died with the wire ends honestly instead of
	 * eternally.
	 */
	{
		pid_t who = conn_ctxpid > 0 ? conn_ctxpid : ctx0_pid;
		int i;

		for (i = 0; i < conn_transit.n; i++) {
			uint64_t pgaddr = conn_transit.page[i];

			if (who > 0 &&
			    pg_state(who, pgaddr) == PG_INTRANSIT) {
				struct vmctx_pgset ps = { .addr = pgaddr,
							  .state = PG_THEIRS };

				if (ctl(who, VMCTX_CTL_PGSET, &ps) == 0) {
					n_transit_swept++;
					if (n_transit_swept <= 16)
						fprintf(stderr, "[vmhome] "
							"0x%llx was IN TRANSIT "
							"on this closing "
							"connection, unacked -- "
							"settled THEIRS so the "
							"claim cannot outlive "
							"the wire\n",
							(unsigned long long)pgaddr);
				}
			}
		}
		conn_transit.n = 0;
	}
	fprintf(stderr, "[vmhome %llu] connection closed after %lu syscalls\n",
		(unsigned long long)conn_ctx, n);
}


/*
 * Hold the CPUs out of their deep idle states while this monitor runs.
 *
 * Every step of a forwarded fault or syscall is a wake-up of a thread that
 * was blocked waiting for it -- the kernel waking the service thread on a
 * VMEXIT, the peer waking on the socket, the guest task waking on RESUME --
 * and on this class of machine a core that went into C2/C3 between two such
 * steps takes 350-400 us to come back (cpuidle state exit latencies on the
 * AMD box), longer than the whole loopback round trip. Measured (session
 * 38, tests/perfab.sh): the bare forwarded getppid costs 62-93 us on an
 * idle box and 40-45 us with four spinners keeping the cores awake; a guest
 * fault 102 vs 79 us. The spinners are the wrong tool; PM QoS is the right
 * one: an open /dev/cpu_dma_latency with a bound written to it tells
 * cpuidle not to pick a state whose exit latency exceeds it, for exactly as
 * long as the descriptor is open -- the lifetime of this process. Nothing
 * spins, and the box's own policy returns the moment the run ends.
 *
 * VMCTX_PM_QOS_US overrides the bound (microseconds; -1 leaves the box
 * alone). Without root, or without cpuidle, the open fails and the box
 * keeps its own policy, which is also what a run on a machine that cannot
 * afford it should do.
 */
static int pm_qos_fd = -1;

static void pm_qos_hold(const char *who)
{
	const char *e = getenv("VMCTX_PM_QOS_US");
	int32_t us = e ? atoi(e) : 20;

	if (us < 0)
		return;
	pm_qos_fd = open("/dev/cpu_dma_latency", O_RDWR | O_CLOEXEC);
	if (pm_qos_fd < 0)
		return;
	if (write(pm_qos_fd, &us, sizeof(us)) != (ssize_t)sizeof(us)) {
		close(pm_qos_fd);
		pm_qos_fd = -1;
		return;
	}
	fprintf(stderr, "[%s] cpu idle exit latency bounded to %d us (PM QoS) "
		"while this monitor runs\n", who, (int)us);
}

int main(int argc, char **argv)
{
	int port;

	/* See vmremote.c: answered with nothing else set up. */
	if (argc > 1 && !strcmp(argv[1], "--build-id")) {
		printf("build %s\n", VMCTX_SRC_ID);
		return 0;
	}
	port = (argc > 1) ? atoi(argv[1]) : 9999;
	int ai = 2, verbose = 0;
	int s, one = 1;
	struct sockaddr_in a;

	if (argc > ai && !strcmp(argv[ai], "-v")) {
		verbose = 1;
		ai++;
	}
	prog_argv = &argv[ai];
	prog_argc = argc - ai;

	setvbuf(stderr, NULL, _IONBF, 0);
	signal(SIGPIPE, SIG_IGN);
	g_port = port;
	if (prog_argc <= 0)
		fprintf(stderr, "[vmhome] usage: %s <port> [-v] <program> "
			"[args...]  -- the program belongs on THIS side; the "
			"destination only says where to connect\n", argv[0]);
	else
		fprintf(stderr, "[vmhome] will run %s (%d argument(s))\n",
			prog_argv[0], prog_argc);
	pglog_init();
	pm_qos_hold("vmhome");
	if (getenv("VMCTX_SOCK_SPIN_US"))
		sock_spin_us = (unsigned)atoi(getenv("VMCTX_SOCK_SPIN_US"));
	if (getenv("VMCTX_RA_MIN_RTT_US"))
		ra_min_rtt_us = (unsigned)atoi(getenv("VMCTX_RA_MIN_RTT_US"));
	ctl_resolve();
	vmctx_deadline_enforce("vmhome", 6000);

	/*
	 * A program may hold a great many descriptors, and this process stands
	 * in for the machine that owns them. Take the hard limit — inherited by
	 * every service context, since they are forks of this.
	 */
	{
		struct rlimit rl;

		if (getrlimit(RLIMIT_NOFILE, &rl) == 0 && rl.rlim_cur < rl.rlim_max) {
			rl.rlim_cur = rl.rlim_max;
			setrlimit(RLIMIT_NOFILE, &rl);
		}
	}

	s = socket(AF_INET, SOCK_STREAM, 0);
	if (s < 0) {
		perror("socket");
		return 1;
	}
	setsockopt(s, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
	/*
	 * Not across exec. The program is this process forked and exec'd, so
	 * without this it inherits the listening socket and keeps the port
	 * bound for as long as it lives — which outlasts vmhome whenever vmhome
	 * is killed rather than shut down. The next vmhome then fails to bind,
	 * and a run started against it silently talks to the old one: a failure
	 * arriving by a route ps does not show, because the process holding the
	 * port is named after the guest's program.
	 */
	fcntl(s, F_SETFD, FD_CLOEXEC);
	g_listen_fd = s;
	(void)g_listen_fd;
	memset(&a, 0, sizeof(a));
	a.sin_family = AF_INET;
	a.sin_addr.s_addr = htonl(INADDR_ANY);
	a.sin_port = htons(port);
	if (bind(s, (struct sockaddr *)&a, sizeof(a)) < 0) {
		perror("bind");
		return 1;
	}
	if (listen(s, 4) < 0) {
		perror("listen");
		return 1;
	}
	fprintf(stderr, "[vmhome] serving forwarded syscalls on port %d\n", port);

	for (;;) {
		struct sockaddr_in ca;
		socklen_t cl = sizeof(ca);
		int c = accept(s, (struct sockaddr *)&ca, &cl);

		if (c < 0) {
			if (errno == EINTR)
				continue;
			perror("accept");
			break;
		}
		setsockopt(c, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
		fcntl(c, F_SETFD, FD_CLOEXEC);	/* the program must not hold it */
		fprintf(stderr, "[vmhome] context connected from %s\n",
			inet_ntoa(ca.sin_addr));

		{
			struct conn_arg *arg = malloc(sizeof(*arg));
			pthread_t th;

			if (!arg) {
				close(c);
				continue;
			}
			arg->fd = c;
			arg->verbose = verbose;
			snprintf(arg->peer, sizeof(arg->peer), "%s",
				 inet_ntoa(ca.sin_addr));
			if (pthread_create(&th, NULL, conn_thread, arg) == 0)
				pthread_detach(th);
			else {
				free(arg);
				close(c);
			}
		}
	}
	return 0;
}
