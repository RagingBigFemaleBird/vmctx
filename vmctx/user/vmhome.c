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
#include <sys/resource.h>
#include <sys/stat.h>

#include <pthread.h>
#include <sched.h>
#include <sys/mman.h>
#include <sys/ipc.h>
#include <sys/shm.h>

#include <stddef.h>	/* offsetof, for the signal frame */
#include "vmrproto.h"
#include "vmctx_recall.h"
#include "vmctx_mapping.h"
#include "vmctx_source_exit.h"
#include "source-map-effects.h"
#include "map-trace.h"
#include "own.h"		/* shared-memory ownership channel (REDESIGN.md session 34); additive, not yet load-bearing */
#include "deadline.h"	/* vmctx_deadline_enforce: the nested page budgets */
#include "monitor-diagnostics.h"

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

	if (!n)
		return 1;
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
				if (r == 0) {
					errno = ECONNRESET;
					return 0;
				}
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

		if (r == 0) {
			errno = ECONNRESET;
			return 0;
		}
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
#define VMCTX_SERVE_GRANT	(1u << 1)
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
	/* Negative selectors address retained descriptors. Probe the read-only
	 * global capability operation and validate its complete result instead
	 * of attributing a generic errno to one particular native syscall. */
	const int candidates[]={473,471};
	for (unsigned i=0;i<sizeof(candidates)/sizeof(candidates[0]);i++) {
		struct vmctx_source_exit_caps caps={0};
		if (!syscall(candidates[i],0,VMCTX_CTL_SOURCE_EXIT_CAPS,&caps) &&
		    caps.version==VMCTX_SOURCE_EXIT_ABI && caps.size==sizeof(caps) &&
		    (caps.features & VMCTX_SOURCE_EXIT_NATIVE_MM_RELEASE)) {
			vmctx_ctl_nr=candidates[i]; break;
		}
	}
	if (!vmctx_ctl_nr) {
		fprintf(stderr,"[vmhome] no supported native source control ABI\n");
		exit(2);
	}
	fprintf(stderr,"[vmhome] vmctx_ctl is syscall %d on this kernel\n",vmctx_ctl_nr);
	fprintf(stderr,"[vmhome] build %s\n",VMCTX_SRC_ID);
}

/* The only bridge allowed to address a native PID. Bootstrap uses it before
 * a context exists; production operations resolve a retained source token. */
static long source_control_raw(pid_t pid, unsigned int cmd, void *arg)
{ return syscall(vmctx_ctl_nr,pid,cmd,arg); }
#include "source-registry.h"
#include "source-address-space.h"
#include "source-memory-target.h"
#include "source-custody.h"
#include "source-transfer.h"
#include "page-receipt.h"

typedef const struct vmr_mm_binding *source_target;
static int source_binding_snapshot(source_id id,struct vmr_mm_binding *binding);
static long source_binding_memory(source_target target,unsigned command,void *argument);
static int source_binding_present(source_target target,uint64_t address);
static source_mm_id fork_parent_mm(source_mm_id child);
static source_id source_target_id(source_target target)
{ return target ? (source_id)target->context : 0; }

/* Entry points capture once. A nested memory helper never refreshes this
 * value after a native call or a peer callback. */
static struct vmr_mm_binding source_binding_required(source_id id)
{
    struct vmr_mm_binding target;
    if(source_binding_snapshot(id,&target) || !vmr_binding_valid(&target))
        source_adapter_failed("capturing operation MM binding");
    return target;
}

static int source_binding_proc_open(source_target binding,const char *leaf,
        source_id *representative)
{
    if(!binding || !vmr_binding_valid(binding) || binding->context>INT_MAX) {
        errno=EPROTO;return -1;
    }
    struct source_memory_target target={source_target_id(binding),binding->mm,binding->epoch};
    return source_memory_target_proc_open(&target,leaf,representative);
}

/* Return detached, sealed bytes. Holding a proc file through the consumer's
 * callbacks would retain a native MM beyond the local inspection operation. */
static FILE *source_maps(source_target target)
{
    int proc=source_binding_proc_open(target,"maps",NULL);
    if(proc<0)return NULL;
    int fd=memfd_create("vmhome-maps-snapshot",MFD_CLOEXEC|MFD_ALLOW_SEALING);
    int saved=errno;
    if(fd<0) {close(proc);errno=saved;return NULL;}
    if(fd<3) {
        int private=fcntl(fd,F_DUPFD_CLOEXEC,3);saved=errno;close(fd);fd=private;
        if(fd<0) {close(proc);errno=saved;return NULL;}
    }
    char bytes[4096];size_t total=0;
    for(;;) {
        ssize_t n;
        do {n=read(proc,bytes,sizeof(bytes));}while(n<0 && errno==EINTR);
        if(n<0)goto failed;
        if(!n)break;
        if((size_t)n>VMR_LAYOUT_MAX-total) {errno=EOVERFLOW;goto failed;}
        total+=(size_t)n;
        for(ssize_t at=0;at<n;) {
            ssize_t put=write(fd,bytes+at,(size_t)(n-at));
            if(put<0 && errno==EINTR)continue;
            if(put<=0) {if(!put)errno=EIO;goto failed;}
            at+=put;
        }
    }
    close(proc);proc=-1;
    if(lseek(fd,0,SEEK_SET)<0 ||
       fcntl(fd,F_ADD_SEALS,F_SEAL_WRITE|F_SEAL_GROW|F_SEAL_SHRINK|F_SEAL_SEAL)<0)
        goto failed;
    FILE *file=fdopen(fd,"r");
    if(!file)goto failed;
    return file;
 failed:
    saved=errno;if(proc>=0)close(proc);close(fd);errno=saved;return NULL;
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
static void ctl_watch(source_id pid, unsigned int cmd, const void *arg, int done,
		      long ret);

/*
 * INSTRUMENT (session 40): what the kernel says a service task IS at the
 * moment a ctl on it fails -- its state letter from /proc/<pid>/stat and
 * where it sleeps (wchan). "alive" by source_record_signal(pid, 0) is too coarse: it is
 * true for a task in do_exit that has already lost its vmctx and for a
 * zombie, and the difference between those is the whole diagnosis.
 */
static const char *task_photo(source_id pid, char *buf, size_t len)
{
	struct vmctx_context q;
	if (source_record_info(pid,&q)) {
		snprintf(buf,len,"context metadata unavailable: %s",strerror(errno));
		return buf;
	}
	char wchan[64]="?";
	(void)source_record_read_proc(pid,"wchan",wchan,sizeof(wchan));
	wchan[strcspn(wchan,"\n")]=0;
	snprintf(buf,len,"identity=%llu mm=%llu native=%u flags=%u status=%u wchan=%s",
		(unsigned long long)q.identity,(unsigned long long)q.mm_identity,
		q.native_pid,q.flags,q.exit_status,wchan);
	return buf;
}

static long ctl(source_id pid, unsigned int cmd, void *arg);
#include "cpu-state.h"
#include "vmctx_source_exit.h"
#include "vmctx_syscall_gate.h"
#include "vmctx_access.h"

static long ctl(source_id pid, unsigned int cmd, void *arg)
{
	long r;

	ctl_watch(pid, cmd, arg, 0, 0);
	r = source_record_ctl(pid, cmd, arg);
	int saved = errno;
	ctl_watch(pid, cmd, arg, 1, r);
	errno = saved;
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
static struct { source_id pid; int in; } incall_tab[INCALL_SLOTS];
static pthread_mutex_t incall_lock = PTHREAD_MUTEX_INITIALIZER;

static void incall_set(source_id pid, int in)
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
				    source_record_signal(incall_tab[i].pid, 0) != 0)))
			free_i = i;
	}
	if (free_i >= 0) {
		incall_tab[free_i].pid = pid;
		incall_tab[free_i].in = in;
	}
	pthread_mutex_unlock(&incall_lock);
}

static int incall_get(source_id pid)
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



/*
 * The clear-tid word the context's clone recorded (CLONE_CHILD_CLEARTID), read
 * from the kernel that ran the real clone -- not parsed out of clone_args here.
 * Zero if the context named none. Used only by the optional source diagnostic;
 * the native kernel performs the exit-time clear and wake.
 */
/* unanswerable requests, split by whether the address space really had ended */
static unsigned long n_ctxpage_gone_ended, n_ctxpage_gone_transient;
/* requests answered "this address space has ended; stop" */
static unsigned long n_ctxpage_gone_told;
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
/* Only the retained native terminal record proves task completion. */
static int task_dead_status(source_id pid, unsigned *st)
{
	struct vmctx_context q;
	if (source_record_info(pid,&q)) {
		if (errno != EAGAIN) source_adapter_failed("querying native terminal state");
		return 0;
	}
	if (!(q.flags & VMCTX_CONTEXT_ENDED)) return 0;
	*st=q.exit_status;
	return 1;
}

/* CPU state operations can race a sibling's exit_group. PF_EXITING becomes
 * visible before the zombie/reaped state; keep the context's channel until
 * the source can report that lifecycle result instead of breaking every
 * other context's connection to the program. This is never a fallback for
 * malformed CPU state or an unsupported native operation on a live task. */
static int cpu_task_ended(source_id pid, int error, unsigned *status)
{
	/* The native exception worker also returns ETIMEDOUT when its task
	 * exits without a CPU completion. The error alone proves nothing. */
	if (error != ESRCH && error != EINVAL && error != ETIMEDOUT)
		return 0;
	for (int retry = 0; retry < 100; retry++) {
		if (task_dead_status(pid, status))
			return 1;
		usleep(1000);
	}
	return 0;
}
static unsigned long n_ret_eio;	/* -EIO written by the kernel completion */
/* Executor invalidations travel through the retained per-MM access journal.
 * The legacy MMLOG is drained only to retire source-local mapping metadata;
 * no connection-owned queue may carry old-MM ranges into a later exec. */

static uint64_t ctx_clear_tid(source_id pid)
{
	uint64_t word = 0;

	if (pid <= 0 || ctl(pid, VMCTX_CTL_GET_CLEAR_TID, &word) != 0)
		return 0;
	return word;
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
 * ctx_monitor() consults them; defined below with sh_file. See relro_mm_note().
 */
struct sh_file_range;
struct relro_range { uint64_t start, end; source_mm_id as; };
static struct relro_range *relro;
static size_t relro_n, relro_cap;
static pthread_mutex_t relro_lock2 = PTHREAD_MUTEX_INITIALIZER;
static unsigned long n_relro_ranges, n_relro_asked, n_rofile_local;
static int relro_has(source_target pid, uint64_t addr);
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
static unsigned long n_ctxpage_gone;
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
static int pglog_transfers_only;

static void pglog_init(void)
{
	const char *e = getenv("VMHOME_PGLOG");

	sigprobe    = getenv("VMHOME_SIGPROBE")    != NULL;
	futex_log   = getenv("VMHOME_FUTEX_LOG")   != NULL;
	pglog_transfers_only = getenv("VMHOME_PGLOG_TRANSFERS_ONLY") != NULL;
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
static int ctx_present(source_target target,uint64_t address)
{ return source_binding_present(target,address); }


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
static int pg_state_gen(source_target pid, uint64_t page, uint32_t *gen)
{
	struct vmctx_serve sv;

	memset(&sv, 0, sizeof(sv));
	sv.addr = page & ~(uint64_t)(VMR_PG_SIZE - 1);
	if (!pid || source_binding_memory(pid, VMCTX_CTL_PGSTATE, &sv) != 0)
		return -1;
	if (gen)
		*gen = sv.gen;
	return (int)sv.state;
}

static int pg_state(source_target pid, uint64_t page)
{
	int st = pg_state_gen(pid, page, NULL);

	return st;
}

static uint32_t pg_gen(source_target pid, uint64_t page)
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
static int pg_set_at(source_target pid, uint64_t page, int st, const char *fn, int line)
{
	struct vmctx_pgset ps;
	int old = -1;
	int result;

	page &= ~(uint64_t)(VMR_PG_SIZE - 1);
	ps.gen = 0;
	old = pg_state_gen(pid, page, &ps.gen);
	ps.addr = page;
	ps.state = (uint32_t)st;
	result = pid ? source_binding_memory(pid, VMCTX_CTL_PGSET, &ps) : -1;
	if (!pglog_on || (pglog_on == 2 && page != pglog_page))
		return result;
	fprintf(stderr, "[OWN abs=%llu tid=%d] vmhome ctx=%d 0x%012llx %s -> %s "
		"(%s:%d)\n", (unsigned long long)now_us(),
		(int)own_tid(), source_target_id(pid), (unsigned long long)page,
		old < 0 ? "(none)" : pg_stname(old), result ? "REFUSED" : pg_stname(st), fn, line);
	return result;
}

#define pg_set(pid, page, st) pg_set_at((pid), (page), (st), __func__, __LINE__)

/*
 * Install a page fetched from the other machine and record this side its
 * holder, stamped with the generation the other machine sent: one step in
 * the kernel. if_absent: refuse (-EEXIST) when a page is already mapped
 * there -- a copy that landed by another path beats arriving bytes.
 */
static void source_word_transfer(source_target pid, uint64_t addr, const void *buf,
		const char *stage, long result, uint32_t gen, int state);

static long ctx_land(source_target pid, uint64_t page, const void *buf, uint32_t gen,
		     int if_absent)
{
	struct vmctx_land l = {
		.addr = page & ~(uint64_t)(VMR_PG_SIZE - 1),
		.buf = (uint64_t)(uintptr_t)buf,
		.gen = gen,
		.flags = if_absent ? VMCTX_LAND_IF_ABSENT : 0,
	};
	long r = source_binding_memory(pid, VMCTX_CTL_LAND, &l);
	int saved_errno = errno;
	source_word_transfer(pid, l.addr, buf, "LAND", r < 0 ? -saved_errno : r, gen, -1);
	errno = saved_errno;
	return r < 0 ? -errno : r;
}

/*
 * The pages of [start, end) whose record is in `mask` (1 << PG_*), handed to
 * fn one by one. Returns how many; -1 if the context cannot be asked.
 */
static long pg_scan(source_target pid, uint64_t start, uint64_t end, unsigned mask,
		    void (*fn)(source_target, uint64_t, void *), void *arg)
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
		if (source_binding_memory(pid, VMCTX_CTL_PGSCAN, &sc) != 0)
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
static int ctx_maps_line(source_target pid, uint64_t addr, char *out, size_t outlen);


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
static void ctl_watch(source_id pid, unsigned int cmd, const void *arg, int done,
		      long ret)
{
	static __thread struct vmr_mm_binding target;
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
		target=source_binding_required(pid);
		pre = ctx_present(&target, pglog_page);
		armed = 1;
		return;
	}
	if (!armed)
		return;
	armed = 0;
	post = ctx_present(&target, pglog_page);
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
		source_id snap[64];

		usleep(100);
		size_t count=source_record_count();
		for (size_t id=1;id<=count && np<64;id++) {
			struct vmctx_context q;
			if (!source_record_info((source_id)id,&q) && !(q.flags & VMCTX_CONTEXT_ENDED))
				snap[np++]=(source_id)id;
		}
		for (k = 0; k < np; k++) {
			int p;

			if (source_record_signal(snap[k], 0) != 0)
				continue;
			struct vmr_mm_binding target=source_binding_required(snap[k]);
			p = ctx_present(&target, pglog_page);
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
				     "while the record says %s", (int)snap[k],
				     last[k], p,
				     pg_stname(pg_state(&target, pglog_page)));
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

static long ctx_peek(source_target pid, uint64_t addr, void *buf, uint64_t len)
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
	r = source_binding_memory(pid, VMCTX_CTL_PEEK, &m);
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

	if (pg_dead_port == port) {
		errno = ECONNREFUSED;
		return -1;
	}
	fd = socket(AF_INET, SOCK_STREAM, 0);
	if (fd < 0)
		return -1;
	memset(&a, 0, sizeof(a));
	a.sin_family = AF_INET;
	a.sin_port   = htons(port);
	if (inet_pton(AF_INET, peer, &a.sin_addr) != 1) {
		close(fd);
		errno = EINVAL;
		return -1;
	}
	if (connect(fd, (struct sockaddr *)&a, sizeof(a)) < 0) {
		int error = errno;
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
		errno = error;
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
 * Source faults use VMR_PG_GET to move custody here, including for read-only
 * files. Current VMA permissions do not make a retained executor copy
 * immutable: a later mprotect can grant writes. GETS previously left that
 * copy mapped while LAND made the source HOME, so the next permission
 * upgrade exposed two independent copies without an ownership fault.
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

static int source_binding_snapshot(source_id id,struct vmr_mm_binding *binding);
static long source_binding_memory(const struct vmr_mm_binding *binding,
        unsigned command,void *argument)
{
    if(!binding || !vmr_binding_valid(binding) || binding->context>INT_MAX) {errno=EPROTO;return -1;}
    struct source_memory_target target={(source_id)binding->context,binding->mm,binding->epoch};
    return source_memory_target_call(&target,command,argument);
}

/* Proc bytes are consumed only after the opened descriptor proves the saved
 * MM. A later INFO is a consistency check, never a replacement target. */
static int source_binding_present(const struct vmr_mm_binding *binding,uint64_t address)
{
    if(!binding || !vmr_binding_valid(binding) || binding->context>INT_MAX) {errno=EPROTO;return -1;}
    source_id id=0;
    uint64_t entry=0;
    int fd=source_binding_proc_open(binding,"pagemap",&id);
    if(fd<0)return -1;
    ssize_t got;
    do {got=pread(fd,&entry,sizeof(entry),(off_t)((address/4096)*8));}
    while(got<0 && errno==EINTR);
    int saved=errno;close(fd);
    if(got!=sizeof(entry)) {errno=got<0?saved:EIO;return -1;}
    struct vmctx_context after;
    if(source_record_info(id,&after))return -1;
    if(after.mm_identity!=binding->mm || (after.flags&VMCTX_CONTEXT_ENDED)) {errno=ESTALE;return -1;}
    return (entry>>63)&1;
}
static int source_binding_page_state(const struct vmr_mm_binding *binding,
        uint64_t address,uint32_t *generation)
{
    struct vmctx_serve query={.addr=address&~UINT64_C(4095)};
    if(source_binding_memory(binding,VMCTX_CTL_PGSTATE,&query))return -1;
    if(generation)*generation=query.gen;
    return query.state;
}

static int pg_get_op(int fd, uint32_t op, uint64_t addr, void *buf, const struct vmr_mm_binding *target,
		     source_id landed)
{
	int tries = 0;
    unsigned char incoming[VMR_PG_SIZE];
	uint64_t t_inflight0 = 0;
	uint64_t aux = pg_req_aux;
    if(!target) {errno=EPROTO;return -1;}
    const struct vmr_mm_binding wanted=*target;
    if(!vmr_binding_valid(&wanted)) {errno=EPROTO;return -1;}

	pg_req_aux = 0;
again:
	{
		struct vmr_pgreq rq = {.magic=VMR_PG_MAGIC,.op=op,.addr=addr,.len=aux,
            .target=wanted,.owner=(uint64_t)getpid()};
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
		if (!vmr_page_reply_matches(&rq,&rs)) {
			fprintf(stderr, "[vmhome] page 0x%llx: the reply is not ours "
				"(magic 0x%x, status %d) — the stream is out of step\n",
				(unsigned long long)addr, rs.magic, (int)rs.status);
			errno = EPROTO;
			return -1;
		}
		if (rs.status != 0 && rs.status != VMR_PG_ABSENT &&
		    rs.status != VMR_PG_INFLIGHT) {
			fprintf(stderr, "[vmhome] page 0x%llx: rejected reply status %d\n",
				(unsigned long long)addr, (int)rs.status);
			/* A remote native error number is opaque here. Only the
			 * protocol's explicit success status authorizes bytes. */
			errno = rs.status < 0 ? EREMOTEIO : EPROTO;
			return -1;
		}
        /* Failed or in-flight replies never overwrite the caller's bytes
         * or receipt metadata. Only a complete terminal frame is published. */
        if(read_all(fd,incoming,sizeof(incoming))<=0)return -1;
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
			    source_binding_present(&wanted, addr) == 1) {
				uint32_t lg = 0;
				int lst = source_binding_page_state(&wanted, addr, &lg);

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
			fprintf(stderr, "[vmhome] page 0x%llx: INFLIGHT did not settle "
				"after %d replies (ctx=%llu gen=%u why=%u)\n",
				(unsigned long long)addr, tries,
				(unsigned long long)wanted.context, rs.gen, rs.why);
			errno = ETIMEDOUT;
			return -1;
		}
		memcpy(buf,incoming,sizeof(incoming));
        last_get_gen=rs.gen;
		return rs.status == VMR_PG_ABSENT ? VMR_PG_ABSENT : 0;
	}
}

static int pg_get(int fd, uint64_t addr, void *buf, const struct vmr_mm_binding *target)
{
	return pg_get_op(fd, VMR_PG_GET, addr, buf, target, 0);
}

/*
 * One of these per service context, because VMCTX_CTL_WAIT names a context and
 * a fault is reported to whoever is waiting on that one. A thread of the
 * monitoring process is what the kernel requires, and any thread of it will do
 * — which is why this can be a thread while another is blocked in a forwarded
 * syscall for the very context that is faulting.
 */
struct mon_arg {
	struct source_record *record; /* independent service ownership */
	source_id    pid;		/* the service context to wait on            */
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
 * fault costs nothing extra. Each outstanding loan is reserved with a kernel recall ticket before the
 * request. Replies commit through that ticket before the faulting page is
 * resumed. A cancelled mapping rejects the old bytes; a non-page response
 * releases the reservation without initializing memory.
 */
#define RA_MAX 4
static unsigned long n_ra_windows, n_ra_pages, n_ra_landed, n_ra_absent,
		     n_ra_inflight, n_ra_present, n_ra_failed;

static _Noreturn void ctx_pull_failed(source_id pid, uint64_t base, const char *stage);

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
	source_id    pid;
	uint64_t q[RA_MAX];
	struct vmctx_recall ticket[RA_MAX];
    struct vmr_mm_binding target;
	struct vmr_pgrsp rs[RA_MAX];
	char     buf[RA_MAX][VMR_PG_SIZE];
} ra;

/* Losing an outstanding reply leaves ownership unresolved. */
static void prefetch_abort(void)
{
	if (ra.n || ra.got) {
		n_ra_failed++;
		errno = EIO;
		ctx_pull_failed(ra.pid, ra.q[0], "readahead reply");
	}
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

static void prefetch_issue(struct mon_arg *ma, int pg, uint64_t base, const struct vmr_mm_binding *target)
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
		int st = source_binding_page_state(target, a, NULL);
		if(st<0)ctx_pull_failed(ma->pid,a,"readahead page state");

		if (st == PG_OURS) {	/* already here: the run is served */
			n_ra_present++;
			break;
		}
		if (st == PG_CLAIM || st == PG_INTRANSIT)
			break;		/* a transition in flight: not ours */
		ra.ticket[n] = (struct vmctx_recall){ .addr = a, .op = VMCTX_RECALL_BEGIN };
		long admitted = source_binding_memory(target, VMCTX_CTL_RECALL, &ra.ticket[n]);
		if (admitted == 1 || (admitted < 0 && errno == EAGAIN))
			break;
		if (admitted < 0)
			ctx_pull_failed(ma->pid, a, "readahead admission");
		ra.q[n] = a;
		n++;
	}
	if (!n)
		return;
	ma->ra_next = ra.q[n - 1] + VMR_PG_SIZE;
	ra.pid = ma->pid;
    ra.target=*target;
	ra.fd = pg;
	for (i = 0; i < n; i++) {
		struct vmr_pgreq rq = {.magic=VMR_PG_MAGIC,.op=VMR_PG_GET,.addr=ra.q[i],
            .target=ra.target,.owner=(uint64_t)getpid()};

		if (write_all(pg, &rq, sizeof(rq)) < 0) {
			ctx_pull_failed(ma->pid, ra.q[i], "readahead request");
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
        struct vmr_pgreq request={.magic=VMR_PG_MAGIC,.op=VMR_PG_GET,
            .addr=ra.q[i],.target=ra.target};
		if (read_all(fd, &ra.rs[i], sizeof(ra.rs[i])) <= 0 ||
		    !vmr_page_reply_matches(&request,&ra.rs[i]) ||
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

	if (ra.n)
		prefetch_abort();
	for (i = 0; i < ra.got; i++) {
		struct vmr_pgrsp *rs = &ra.rs[i];
		struct vmctx_recall *ticket = &ra.ticket[i];

		if (rs->status == 0) {
			ticket->op = VMCTX_RECALL_COMMIT;
			ticket->buf = (uint64_t)(uintptr_t)ra.buf[i];
			ticket->gen = rs->gen;
			if (ctl(0, VMCTX_CTL_RECALL, ticket) != VMR_PG_SIZE)
				ctx_pull_failed(ma->pid, ra.q[i], "readahead commit");
			n_ra_landed++;
		} else {
			/* Speculation supplied no bytes. Preserve the loan and let
			 * an actual fault resolve it; this never initializes memory. */
			ticket->op = VMCTX_RECALL_CANCEL;
			if (ctl(0, VMCTX_CTL_RECALL, ticket) && errno != ESTALE)
				ctx_pull_failed(ma->pid, ra.q[i], "readahead cancellation");
			if (rs->status == VMR_PG_ABSENT)
				n_ra_absent++;
			else
				n_ra_inflight++;
		}
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
 * same pull — see ctx_pull_page(). So the route to the machine running the
 * guest is recorded per context when its monitor starts, and any thread can ask
 * for it.
 * ---------------------------------------------------------------------------
 */
struct ctx_route {
	source_id    pid;
	uint64_t ctx;
	int      port;
	char     peer[64];
};
static struct ctx_route ctx_routes[512];
static int ctx_nroutes;
static pthread_mutex_t ctx_route_lock = PTHREAD_MUTEX_INITIALIZER;

static void ctx_route_add(source_id pid, uint64_t ctx, const char *peer, int port)
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

static int ctx_route_find(source_id pid, struct ctx_route *out)
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

#include "source-lineage.h"
#define FORK_REL_MAX	256
static unsigned long n_cow_from_parent;	/* pages taken by breaking the sharing */
static unsigned long n_cow_file_from_parent; /* private-FILE pages a fork copy took from the parent's diverged copy, not the file */


/* Count registered fault workers before publishing their threads. Aggregate
 * counters are reported at quiescence, instead of repeating the entire session
 * report for every sibling during a group exit. Per-context results remain
 * unconditional. This count governs diagnostics only. */
static _Atomic unsigned fault_service_workers;

static void *ctx_monitor(void *arg)
{
	struct mon_arg *ma = arg;
	unsigned long served = 0, absent = 0, failed = 0, kill_asked = 0;
	char page[VMR_PG_SIZE];
		int pg = -1;
	const char *end_why = "?";
	int end_errno = 0;
	char ph[128];


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
            struct vmr_mm_binding fault_target;
            if(source_binding_snapshot(ma->pid,&fault_target))
                ctx_pull_failed(ma->pid,ev.fault_addr,"capturing fault MM");
			base = ev.fault_addr & ~(uint64_t)(VMR_PG_SIZE - 1);

			plog(base, "FAULT err=0x%llx rip=0x%llx pre=%d",
			     (unsigned long long)ev.fault_err,
			     (unsigned long long)ev.rip,
			     pglog_on ? ctx_present(&fault_target, base) : -1);
			if (sigprobe) {
				struct vmctx_uregs g;
				char sc[128] = "";
				(void)source_record_read_proc(ma->pid,"syscall",sc,sizeof(sc));
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
			if (ro_file && !PG_GONE(prior) && !relro_has(&fault_target, base)) {
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
				 * serve's TRANSIT is settled or its reservation ends.
				 */
				n_sibling_claim_waited++;
				for (spin = 0; spin < 4000; spin++) {
					stt = pg_state(&fault_target, base);
					if (stt != PG_CLAIM && stt != PG_INTRANSIT)
						break;
					usleep(500);
				}
				if (stt == PG_OURS && ctx_present(&fault_target, base) == 1) {
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
				if (pg_set(&fault_target, base, PG_CLAIM)) {
					rep.action = VMCTX_ACT_DONE;
					goto answered; /* retry without stealing the incumbent */
				}
				claimed_here = 1;
			}

			/* A read-only source fault also acquires exclusive custody.
			 * Its current permission cannot authorize an executor copy to
			 * survive LAND and become writable on a later mprotect. */
			if (ro_file)
				n_relro_asked++;
			if (pg < 0)
				pg = pg_connect(ma->peer,
						ma->port + VMR_PG_PORT_OFFSET);
			if (pg >= 0) {
				uint64_t gt0 = now_us();

				if (!ro_file)
					prefetch_issue(ma, pg, base, &fault_target);
				got = pg_get_op(pg, VMR_PG_GET,
						base, page, &fault_target,
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
					got = pg_get_op(pg, VMR_PG_GET,
							base, page, &fault_target,
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
				long lr = ctx_land(&fault_target, base, page,
						   last_get_gen,
						   !(ev.fault_err & 1));

				if (lr == (long)VMR_PG_SIZE) {
					rep.action = VMCTX_ACT_DONE;
					src_arrival = 1;
					served++;
					/* The executor preserves child snapshots before giving
					 * this page away. Installing another copy into a child
					 * here would bypass that child's ownership record and
					 * let its native syscalls write an invisible duplicate. */
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
				   as_live_mm(fork_parent_mm(fault_target.mm),0,0) && pg >= 0 &&
				   pg_get_op(pg, VMR_PG_COWBREAK, base, page,
					     &fault_target, 0) == 0 &&
				   ctx_land(&fault_target, base, page, last_get_gen,
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
				if (ctx_present(&fault_target, base) == 1) {
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
							     &fault_target);
						if (got < 0) {
							close(pg);
							pg = -1;
							break;
						}
						if (ctx_present(&fault_target, base) == 1)
							break;
					}
					if (got == 0 &&
					    ctx_land(&fault_target, base, page,
						     last_get_gen, 0) ==
					    (long)VMR_PG_SIZE) {
						n_absent_regained++;
						src_arrival = 3;
						rep.action = VMCTX_ACT_DONE;
						served++;
					} else if (ctx_present(&fault_target, base) == 1) {
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
					if (as_live_mm(fork_parent_mm(fault_target.mm),0,0) &&
					    pg >= 0 &&
					    pg_get_op(pg, VMR_PG_COWBREAK, base,
						      page, &fault_target, 0) == 0 &&
					    ctx_land(&fault_target, base, page,
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
					/* A current ancestor page cannot establish this child's
					 * fork snapshot. No ancestor TAKE or relaxed copy is
					 * allowed to manufacture a successful inheritance. */
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
					if (ctx_land(&fault_target, base, zeros, 0, 1)
					    == (long)VMR_PG_SIZE) {
						plog(base, "GET says ABSENT -> supplied "
						     "an ordinary empty page here");
						if (n_absent_filled < 16) {
							char ml[512];

							ctx_maps_line(&fault_target, base,
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
					} else if (ctx_present(&fault_target, base) == 1) {
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
					    pg_state(&fault_target, base) == PG_CLAIM)
						pg_set(&fault_target, base, prior);
					ctl(ma->pid, VMCTX_CTL_RESUME, &rep);
					source_record_signal(ma->pid, SIGKILL);
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
			if (claimed_here && pg_state(&fault_target, base) == PG_CLAIM)
				pg_set(&fault_target, base,
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

	if (pg >= 0)
		close(pg);
	fprintf(stderr, "[vmhome] context %d: fault service ends (%lu pages "
		"fetched, %lu the guest never touched, %lu unanswered); %lu of "
		"the untouched ones were filled here as ordinary pages rather "
		"than left to the local kernel's shared zero page -- ended by "
		"%s: %s; the task now: alive=%d %s\n",
		(int)ma->pid, served, absent, failed, n_absent_filled,
		end_why, strerror(end_errno), source_record_signal(ma->pid, 0) == 0,
		task_photo(ma->pid, ph, sizeof(ph)));
	fprintf(stderr, "[vmhome] a page GET round trip costs %llu us on this "
		"channel (%lu samples); readahead %s\n",
		(unsigned long long)get_rtt_us, get_rtt_samples,
		prefetch_pays() ? "ON (the wire is the cost)" :
		"OFF (a round trip costs what a page costs; loopback)");
	if (atomic_fetch_sub_explicit(&fault_service_workers,1,memory_order_acq_rel)==1) {
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
			"committed IN TRANSIT, %lu acked installed, %lu acks rejected "
			"(the page had moved on), %lu asks answered CLAIMING while "
			"the page was on the wire to the asker\n",
			n_intransit_committed, n_intransit_acked,
			n_intransit_ack_stale, n_serve_intransit_refused);
		fprintf(stderr, "[vmhome] socket reads that spun (%u us budget): %lu "
			"answered inside the spin, %lu ran out and slept\n",
			sock_spin_us, n_sock_spin_hit, n_sock_spin_miss);

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
		fprintf(stderr, "[vmhome] readahead inside forwarded calls: %lu window(s), "
			"%lu page(s) asked for in them, %lu landed, %lu already here, "
			"%lu the guest's machine did not hold, %lu in flight there, "
			"%lu could not be kept\n", n_ra_windows, n_ra_pages,
			n_ra_landed, n_ra_present, n_ra_absent, n_ra_inflight,
			n_ra_failed);
		fprintf(stderr, "[vmhome] native MM queries reporting ENDED: %lu; requests this side could not answer: %lu "
			"where the address space really had ended, %lu where it had NOT "
			"and the lookup was merely momentary -- the second kind is what "
			"three teardown repairs killed the program for; %lu request(s) "
			"were answered \"this address space has ended, stop\"\n",
			n_as_ended, n_ctxpage_gone_ended, n_ctxpage_gone_transient,
			n_ctxpage_gone_told);
	}
	source_record_put(ma->record);
	free(ma);
	return NULL;
}

static void ctx_monitor_start(source_id pid, uint64_t ctx, const char *peer, int port)
{
	sweep_start();
	struct mon_arg *ma=calloc(1,sizeof(*ma));
	if (!ma) source_adapter_failed("allocating fault service");
	ma->record=source_record_get(pid);
	if (!ma->record) { free(ma); source_adapter_failed("retaining fault service context"); }
	ma->pid=pid; ma->ctx=ctx; ma->port=port;
	snprintf(ma->peer,sizeof(ma->peer),"%s",peer && *peer ? peer : "127.0.0.1");
	ctx_route_add(pid,ctx,ma->peer,port);
	pthread_t thread;
	atomic_fetch_add_explicit(&fault_service_workers,1,memory_order_relaxed);
	int error=pthread_create(&thread,NULL,ctx_monitor,ma);
	if (error) {
		source_record_put(ma->record); free(ma); errno=error;
		source_adapter_failed("starting fault service");
	}
	pthread_detach(thread);
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
static void sh_file_forget(source_target pid, uint64_t addr, uint64_t len);

/*
 * The maps line covering an address, verbatim, for the failed-install
 * diagnostic below. "POKE failed" alone cannot distinguish its three causes --
 * no VMA there at all, a VMA without write permission (which FOLL_FORCE cannot
 * override for a MAP_SHARED mapping: there is no private copy to make), and a
 * writable VMA whose fault the kernel still refused -- and the three repairs
 * live in three different places.
 */
static int ctx_maps_line(source_target pid, uint64_t addr, char *out, size_t outlen)
{
	char line[512];
	FILE *f=source_maps(pid);
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

static __thread int pull_fd = -1;

/* Native source diagnostic: the kernel, not a guessed libc layout, supplies
 * the clear-TID address. Reads never initialize or claim the page. */
static void source_thread_word_probe(source_id pid, const char *stage)
{
	if (!getenv("VMH_THREAD_WORD_PROBE")) return;
	int saved_errno = errno;
	const struct vmr_mm_binding target=source_binding_required(pid);
	uint64_t word = ctx_clear_tid(pid);
	uint32_t value = 0;
	long n = word ? ctx_peek(&target, word, &value, sizeof(value)) : 0;
	fprintf(stderr, "[vmhome] THREAD-WORD %s ctx=%d at=%llx bytes=%ld value=%u abs=%llu\n",
		stage, pid, (unsigned long long)word, n, value,
		(unsigned long long)now_us());
	errno = saved_errno;
}

/* Optional trace keyed by native clone word addresses. The table is only a
 * diagnostic filter; it never controls ownership or page admission. */
static struct { source_mm_id as; uint64_t addr; } source_words[256];
static unsigned n_source_words;
static pthread_mutex_t source_words_lock = PTHREAD_MUTEX_INITIALIZER;

static void source_word_register(source_id pid, uint64_t addr)
{
	if (!getenv("VMH_WORD_TRANSFERS") || !addr || (addr & 4095) > 4092) return;
	source_mm_id as = as_of(pid);
	pthread_mutex_lock(&source_words_lock);
	for (unsigned i = 0; i < n_source_words; i++)
		if (source_words[i].as == as && source_words[i].addr == addr) {
			pthread_mutex_unlock(&source_words_lock);
			return;
		}
	if (n_source_words < 256) {
		source_words[n_source_words].as = as;
		source_words[n_source_words++].addr = addr;
	}
	pthread_mutex_unlock(&source_words_lock);
}

static void source_word_transfer(source_target pid, uint64_t addr, const void *buf,
		const char *stage, long result, uint32_t gen, int state)
{
	if (!getenv("VMH_WORD_TRANSFERS")) return;
	int saved_errno = errno;
	source_mm_id as = pid->mm;
	uint32_t value = 0;
	uint64_t word = 0;
	pthread_mutex_lock(&source_words_lock);
	for (unsigned i = 0; i < n_source_words; i++)
		if (source_words[i].as == as && (source_words[i].addr & ~4095ULL) == addr) {
			word = source_words[i].addr;
			break;
		}
	pthread_mutex_unlock(&source_words_lock);
	if (word) {
		if (buf) memcpy(&value, (const char *)buf + (word & 4095), sizeof(value));
		fprintf(stderr, "[vmhome] WORD-TRANSFER %s ctx=%d as=%llu word=%llx "
			"result=%ld state=%d gen=%u bytes=%d value=%u abs=%llu\n",
			stage, source_target_id(pid), as, (unsigned long long)word, result, state, gen,
			buf ? 4 : 0, value, (unsigned long long)now_us());
	}
	errno = saved_errno;
}

/* Optional source-Linux diagnostic of the native clone arguments. Parent
 * SETTID and child CLEARTID may name different words (as current glibc does).
 * Neither the execution adapter nor the wire protocol interprets this ABI. */
static void source_clone_words_probe(source_target saved, const struct vmctx_syscall *c,
				     const char *stage)
{
	if (!getenv("VMH_THREAD_WORD_PROBE")) return;
	if (c->nr != 56 && c->nr != 435) return;
	int saved_errno = errno;
	const struct vmr_mm_binding target=*saved;
	source_id pid=source_target_id(saved);
	uint64_t flags = c->args[0], parent = c->args[2], child = c->args[3];
	if (c->nr == 435) {
		uint64_t ca[4];
		if (c->args[1] < sizeof(ca) ||
		    ctx_peek(&target, c->args[0], ca, sizeof(ca)) != sizeof(ca)) {
			errno = saved_errno;
			return;
		}
		flags = ca[0]; child = ca[2]; parent = ca[3];
	}
	source_word_register(pid, parent);
	uint32_t word_gen = 0;
	int word_state = pg_state_gen(&target, parent, &word_gen);
	uint32_t pv = 0, cv = 0;
	long pn = parent ? ctx_peek(&target, parent, &pv, sizeof(pv)) : 0;
	long cn = child ? ctx_peek(&target, child, &cv, sizeof(cv)) : 0;
	fprintf(stderr, "[vmhome] CLONE-WORDS %s ctx=%d flags=%llx "
		"parent=%llx bytes=%ld value=%u child=%llx bytes=%ld value=%u "
		"ret=%lld abs=%llu state=%d gen=%u\n", stage, pid, (unsigned long long)flags,
		(unsigned long long)parent, pn, pv,
		(unsigned long long)child, cn, cv, (long long)c->ret,
		(unsigned long long)now_us(), word_state, word_gen);
	errno = saved_errno;
}

static _Noreturn void ctx_pull_failed(source_id pid, uint64_t base, const char *stage)
{
	fprintf(stderr, "[vmhome] page recall failed for context %d at 0x%llx "
		"during %s: %s; stopping without installing or discarding a reply\n",
		(int)pid, (unsigned long long)base, stage, strerror(errno));
	source_record_signal(pid, SIGKILL);
	fflush(stderr);
	_exit(98);
}

static int ctx_pull_page_mode(source_target pid, uint64_t base, int allow_gone)
{
	struct ctx_route rt;
	struct vmctx_recall recall = { .addr = base, .op = VMCTX_RECALL_BEGIN };
	char page[VMR_PG_SIZE];
	uint64_t began = now_us();
	int got = -1;
	long result;

	if (!ctx_route_find(source_target_id(pid), &rt))
		return 0; /* no execution endpoint has borrowed this context's memory */
	for (;;) {
		result = source_binding_memory(pid, VMCTX_CTL_RECALL, &recall);
		if (result == 1)
			return 0; /* no loan at this address in the current mapping */
		if (result == 0)
			break;
		/* Teardown may race the native task's exit before BEGIN admits
		 * a transfer. No ticket or bytes exist yet: return the failure
		 * to the caller, which must not perform its pending write. Once
		 * admitted, all transfer/commit failures still stop the run. */
		if (allow_gone && errno == ESRCH)
			return -ESRCH;
		if (errno != EAGAIN || now_us() - began > 6000000)
			ctx_pull_failed(source_target_id(pid), base, "claim admission");
		/* No user or kernel lock is held while another transfer settles. */
		usleep(100);
	}

	if (pull_fd < 0)
		pull_fd = pg_connect(rt.peer, rt.port + VMR_PG_PORT_OFFSET);
	if (pull_fd < 0)
		goto cancel_failed;
	got = pg_get_op(pull_fd, VMR_PG_GET, base, page, pid, source_target_id(pid));
	if (got < 0)
		goto cancel_failed;
	if (got == VMR_PG_ABSENT || got == PG_GOT_LANDED) {
		/* A ticket is issued only for a recorded loan. Neither answer
		 * supplies its bytes, so it cannot authorize a local fill. */
		errno = EPROTO;
		goto cancel_failed;
	}
	recall.op = VMCTX_RECALL_COMMIT;
	recall.buf = (uint64_t)(uintptr_t)page;
	recall.gen = last_get_gen;
	result = ctl(0, VMCTX_CTL_RECALL, &recall);
	if (result != VMR_PG_SIZE)
		ctx_pull_failed(source_target_id(pid), base, "ticket commit");
	plog(base, "PULL committed through ticket %llu", (unsigned long long)recall.ticket);
	return 1;

cancel_failed:
	{
		int saved = errno;
		recall.op = VMCTX_RECALL_CANCEL;
		long cancelled = ctl(0, VMCTX_CTL_RECALL, &recall);
		int cancel_error = cancelled < 0 ? errno : 0;
		fprintf(stderr, "[vmhome] recall failure MM=%llu page=%llx ticket=%llu "
			"reply=%d error=%d cancel=%ld cancel_error=%d\n",
			(unsigned long long)pid->mm, (unsigned long long)base,
			(unsigned long long)recall.ticket, got, saved, cancelled, cancel_error);
		errno = saved;
	}
	ctx_pull_failed(source_target_id(pid), base, "page transfer");
}

static int ctx_pull_page(source_target pid, uint64_t base)
{
	return ctx_pull_page_mode(pid, base, 0);
}

static source_id ctx0_source;
static pid_t ctx0_native_pid;

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
struct ctx_map { uint64_t ctx; source_id pid; };
static struct ctx_map ctx_map_tab[512];
static int ctx_map_n;
static pthread_mutex_t ctx_map_lock = PTHREAD_MUTEX_INITIALIZER;

static void ctx_map_add(uint64_t ctx, source_id pid)
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
static __thread source_id    conn_source;
static __thread char     last_path[96];

/* A call owns its mapping classification until completion publication. */
struct source_shared_mapping { uint64_t object, offset, length; };

static int g_listen_fd = -1;
/*
 * Which machine is running the guest, and on which port this vmhome answers.
 * Its page service is that port plus one — the connection is made outwards from
 * here, because the machine that owns the memory listens and the machine that
 * wants a page asks.
 */
static int g_port = 9999;
static __thread char conn_peer[64];

/* ---- the program's own address space: the kernel's to judge ---------- */

/* ---- source-kernel exception delivery --------------------------------- */
static unsigned long n_exc_legal;

static int source_runtime_credit(source_id pid, const struct vmr_req *rq)
{
	struct vmctx_runtime runtime = {
		.version = VMCTX_RUNTIME_ABI, .size = sizeof(runtime),
		.op = VMCTX_RUNTIME_CREDIT,
		.epoch = rq->execution_epoch, .total_ns = rq->execution_ns,
	};
	return cpu_state_ctl(pid, VMCTX_CTL_RUNTIME, &runtime);
}

/* The kernel owns signal frames and dispositions. The wire carries only the
 * hardware event and complete architectural CPU state. No signal layout is
 * manufactured by this monitor or interpreted by the execution machine. */
static int ctx_deliver_exception(source_id pid, uint64_t vec, uint64_t err,
				 uint64_t addr, struct vmr_cpu_state *cpu)
{
	struct vmctx_syscall call = { .nr = vec, .args = {err, addr} };

	if (cpu_state_ctl(pid, VMCTX_CTL_SETCPU, cpu) < 0)
		return -errno;
	if (ctl(pid, VMCTX_CTL_EXCEPTION, &call) < 0)
		return -errno;
	if (call.ret == VMR_EXC_LEGAL) {
		n_exc_legal++;
		return VMR_EXC_LEGAL;
	}
	if (call.ret)
		return (int)call.ret;
	if (cpu_state_ctl(pid, VMCTX_CTL_GETCPU, cpu) < 0)
		return -errno;
	/* The worker restores its borrowed GPRs after recording the result. */
	memcpy(&cpu->regs, &call.regs, sizeof(cpu->regs));
	return 1;
}

/* ---- shared mappings --------------------------------------------------- */

/* The source adapter interns its native file identity, pinning the inode for
 * the run. Only an opaque identity crosses the wire. Object offsets are kept
 * separately, so growth never reassigns old pages or overlaps another object.
 * Device mappings with kernel writers need an additional native coherence
 * contract; this registry alone cannot provide that contract. */
struct sh_shared_object {
	uint64_t dev, ino;
	int fd;
};
static struct sh_shared_object *sh_shared;
static size_t sh_shared_n;
static pthread_mutex_t sh_shared_lock = PTHREAD_MUTEX_INITIALIZER;

/* Consumes fd on every outcome. IDs are never reused within this session. */
static uint64_t sh_shared_identity(int fd, uint64_t foff, uint64_t len)
{
	struct stat st;
	uint64_t identity = 0;
	int error = 0;
	if (fstat(fd, &st)) { error = errno; goto close_fd; }
	if (!len || (foff & 4095) || foff > INT64_MAX ||
	    len > INT64_MAX - foff) { error = EOVERFLOW; goto close_fd; }
	pthread_mutex_lock(&sh_shared_lock);
	for (size_t i = 0; i < sh_shared_n; i++) {
		if (sh_shared[i].dev == (uint64_t)st.st_dev &&
		    sh_shared[i].ino == (uint64_t)st.st_ino) {
			identity = i + 1;
			goto unlock;
		}
	}
	if (sh_shared_n == SIZE_MAX / sizeof(*sh_shared)) {
		error = EOVERFLOW; goto unlock;
	}
	struct sh_shared_object *grown = realloc(sh_shared,
		(sh_shared_n + 1) * sizeof(*sh_shared));
	if (!grown) { error = ENOMEM; goto unlock; }
	sh_shared = grown;
	sh_shared[sh_shared_n++] = (struct sh_shared_object){
		.dev = st.st_dev, .ino = st.st_ino, .fd = fd};
	identity = sh_shared_n;
	fd = -1;
unlock:
	pthread_mutex_unlock(&sh_shared_lock);
close_fd:
	if (fd >= 0) close(fd);
	if (error) errno = error;
	return identity;
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
	source_mm_id as;		/* the ADDRESS SPACE the range belongs to.
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
static void relro_reserve(size_t n)
{
	if (n <= relro_cap) return;
	size_t cap = relro_cap ? relro_cap : 64;
	while (cap < n && cap <= SIZE_MAX / 2) cap *= 2;
	if (cap < n || cap > SIZE_MAX / sizeof(*relro)) abort();
	void *p = realloc(relro, cap * sizeof(*relro));
	if (!p) abort();
	relro = p; relro_cap = cap;
}

static void relro_mm_note(source_mm_id as, uint64_t addr, uint64_t len)
{
	uint64_t start = addr & ~0xfffULL, end = (addr + len + 0xfffULL) & ~0xfffULL;
	if (!len || end <= start) return;
	pthread_mutex_lock(&relro_lock2);
	for (size_t i = 0; i < relro_n; i++)
		if (relro[i].as == as && relro[i].start == start && relro[i].end == end)
			goto out;
	relro_reserve(relro_n + 1);
	relro[relro_n++] = (struct relro_range){start, end, as};
	n_relro_ranges++;
out:
	pthread_mutex_unlock(&relro_lock2);
}

static int relro_has(source_target pid, uint64_t addr)
{
	source_mm_id as = pid->mm;
	int hit = 0;
	pthread_mutex_lock(&relro_lock2);
	for (size_t i = 0; i < relro_n; i++)
		if (relro[i].as == as && addr >= relro[i].start && addr < relro[i].end) {
			hit = 1; break;
		}
	pthread_mutex_unlock(&relro_lock2);
	return hit;
}

static void relro_mm_forget(source_mm_id as, uint64_t start, uint64_t len)
{
	uint64_t end = start + len;
	if (!len || end <= start) return;
	pthread_mutex_lock(&relro_lock2);
	for (size_t i = 0; i < relro_n; ) {
		struct relro_range r = relro[i];
		if (r.as != as || r.end <= start || r.start >= end) { i++; continue; }
		if (r.start < start) {
			relro[i++].end = start;
			if (r.end > end) { relro_reserve(relro_n + 1); r.start = end; relro[relro_n++] = r; }
		} else if (r.end > end) relro[i++].start = end;
		else relro[i] = relro[--relro_n];
	}
	pthread_mutex_unlock(&relro_lock2);
}

static void relro_inherit(source_mm_id ca, source_mm_id pa)
{
	if (ca == pa) return;
	pthread_mutex_lock(&relro_lock2);
	size_t n = relro_n;
	relro_reserve(2 * n + 1);
	for (size_t i = 0; i < n; i++)
		if (relro[i].as == pa) { struct relro_range r = relro[i]; r.as = ca; relro[relro_n++] = r; }
	pthread_mutex_unlock(&relro_lock2);
}

static void sh_file_add_locked(source_mm_id as, uint64_t start, uint64_t end)
{
	if (sh_file_n < (int)(sizeof(sh_file) / sizeof(sh_file[0]))) {
		sh_file[sh_file_n].start = start;
		sh_file[sh_file_n].end   = end;
		sh_file[sh_file_n].as    = as;
		sh_file_n++;
	}
}

static void sh_file_note(source_target pid, uint64_t addr, uint64_t len)
{
	uint64_t start = addr & ~0xfffUL;
	uint64_t end = (addr + len + 0xfffUL) & ~0xfffUL;
	source_mm_id as = pid->mm;
	int i;

	if (!len)
		return;
	pthread_mutex_lock(&sh_file_lock);
	map_trace("source", "shared-note ctx=%d as=%llu range=%llx-%llx",
		source_target_id(pid), as, (unsigned long long)start, (unsigned long long)end);
	for (i = 0; i < sh_file_n; i++)
		if (sh_file[i].as == as &&
		    sh_file[i].start == start && sh_file[i].end == end)
			goto out;
	if (sh_file_n < (int)(sizeof(sh_file) / sizeof(sh_file[0]))) {
		sh_file_add_locked(as, start, end);
		n_shfile_ranges++;
		plog(start, "shared file range 0x%llx-0x%llx recorded for as %llu",
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
static void sh_file_forget(source_target pid, uint64_t addr, uint64_t len)
{
	uint64_t lo = addr & ~0xfffUL;
	uint64_t hi = (addr + len + 0xfffUL) & ~0xfffUL;
	source_mm_id as = pid->mm;
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
		map_trace("source", "shared-forget ctx=%d as=%llu range=%llx-%llx removes=%llx-%llx",
			source_target_id(pid), as, (unsigned long long)lo, (unsigned long long)hi,
			(unsigned long long)s, (unsigned long long)e);
		if (s < lo)
			sh_file_add_locked(as, s, lo);
		if (e > hi)
			sh_file_add_locked(as, hi, e);
		/* re-examine slot i: the swapped-in entry may overlap too */
	}
	pthread_mutex_unlock(&sh_file_lock);
}

static int ctx_pull_page(source_target pid, uint64_t base);

/*
 * Take back every page of a shared file mapping that the given range touches.
 *
 * Called before the call, not after: munmap has no range left to ask about
 * afterwards, and msync has already written whatever this side held.
 */
static void sh_file_reclaim(source_target pid, uint64_t addr, uint64_t len)
{
	uint64_t lo = addr & ~0xfffUL;
	uint64_t hi = (addr + len + 0xfffUL) & ~0xfffUL;
	int i;

	if (!len || !pid)
		return;
	for (i = 0; ; i++) {
		uint64_t s, e, p;

		pthread_mutex_lock(&sh_file_lock);
		if (i >= sh_file_n) {
			pthread_mutex_unlock(&sh_file_lock);
			break;
		}
		if (sh_file[i].as != pid->mm) {
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
		for (p = s; p < e; p += VMR_PG_SIZE) {
			int pulled = ctx_pull_page(pid, p);
			map_trace("source", "shared-reclaim ctx=%d page=%llx pulled=%d",
				source_target_id(pid), (unsigned long long)p, pulled);
		}
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
static void sh_file_reclaim_pull(source_target pid, uint64_t page, void *arg)
{
	(void)arg;
	n_shfile_pages++;
	int pulled = ctx_pull_page(pid, page);
	map_trace("source", "shared-held-reclaim ctx=%d page=%llx pulled=%d",
		source_target_id(pid), (unsigned long long)page, pulled);
	if (pulled)
		n_shfile_reclaimed++;
}

static void sh_file_reclaim_held(source_target pid)
{
	source_mm_id as;
	int i;

	if (!pid)
		return;
	pthread_mutex_lock(&sh_file_lock);
	i = sh_file_n;
	pthread_mutex_unlock(&sh_file_lock);
	if (!i)
		return;			/* no shared file mapping exists */

	as = pid->mm;
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
		 * A loan, recall or unacknowledged grant means the source may
		 * not read its physical copy. The kernel lists those pages
		 * (VMCTX_CTL_PGSCAN), so a range the guest never wrote costs
		 * one scan and no round trip.
		 */
		pg_scan(pid, s, e, (1u << PG_THEIRS) | (1u << PG_CLAIM) |
			(1u << PG_INTRANSIT),
			sh_file_reclaim_pull, NULL);
	}
}

/* Is this address inside a range recorded as a shared file mapping? */
static int sh_file_has(source_target pid, uint64_t addr)
{
	source_mm_id as = pid->mm;
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
static void note_shared_mapping(source_target pid, const struct vmr_req *rq, long ret,
		struct source_shared_mapping *map)
{
	struct vmctx_mapping m = {.addr = (uint64_t)ret};
	uint64_t offset;

	*map=(struct source_shared_mapping){0};
	if (ret < 0 || !(rq->nr == SYS_shmat ||
	    (rq->nr == SYS_mmap && (rq->args[3] & MAP_SHARED)))) return;
	/* This Linux source adapter asks its own kernel about the mapping that
	 * actually exists. A descriptor number from the request may already be
	 * reused by a sibling. Metadata never needs a buffer in guest memory. */
	if (source_binding_memory(pid, VMCTX_CTL_MAPPING, &m))
		ctx_pull_failed(source_target_id(pid), (uint64_t)ret, "source mapping query");
	if (!(m.flags & VMCTX_MAPPING_SHARED) || m.start > (uint64_t)ret ||
	    (uint64_t)ret - m.start >= m.len || m.fd < 0) {
		if (m.fd >= 0) close(m.fd);
		errno = ESTALE;
		ctx_pull_failed(source_target_id(pid), (uint64_t)ret, "source shared mapping identity");
	}
	map->length = rq->nr == SYS_mmap ? rq->args[1] :
		m.len - ((uint64_t)ret - m.start);
	if (map->length > m.len - ((uint64_t)ret - m.start) ||
	    m.offset > UINT64_MAX - ((uint64_t)ret - m.start)) {
		close(m.fd); errno = ESTALE;
		ctx_pull_failed(source_target_id(pid), (uint64_t)ret, "source mapping extent");
	}
	offset = m.offset + ((uint64_t)ret - m.start);
	map->object = sh_shared_identity(m.fd, offset, map->length);
	if (!map->object)
		ctx_pull_failed(source_target_id(pid), (uint64_t)ret, "source object identity");
	map->offset = offset;
	if (m.prot & PROT_WRITE)
		sh_file_note(pid, (uint64_t)ret, map->length);
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
static void ctx_forward_prepare(source_target saved, const struct vmr_req *rq)
{
	const struct vmr_mm_binding target=*saved;
	/*
	 * msync(2) and munmap(2) are the two calls after which a shared file
	 * mapping's contents can be seen without the mapping. Nothing here
	 * knows what either does beyond that -- both name a range, and a range
	 * that is a file has to be here before it is published. See
	 * sh_file_reclaim().
	 */
	if (rq->nr == SYS_msync || rq->nr == SYS_munmap)
		sh_file_reclaim(&target, rq->args[0], rq->args[1]);
	/*
	 * ...and the same thing without the list of calls. See
	 * sh_file_reclaim_held(): the two above are the ones after which the
	 * range can be read *without* the mapping, and every call at all can
	 * read it *through* one.
	 */
	sh_file_reclaim_held(&target);

}

static long ctx_forward(source_target saved, const struct vmr_req *rq,
			const struct vmctx_syscall_gate *gate, struct vmr_rsp *reply,
			struct vmr_uregs *out, int *out_valid, int verbose)
{
	const struct vmr_mm_binding target=*saved;
	source_id pid=source_target_id(saved);
	struct vmctx_syscall c=gate->call;
	/* Side effects follow the admitted operation's raw dispatch result.
	 * Tracer changes at syscall exit affect guest state, not those effects. */
	c.ret=gate->dispatch_ret;
	last_path[0]=0;
	*out_valid=0;

	{
		/*
		 * Whether the watched page is present here across the call, and
		 * nothing about which call it is. A page this side gave away
		 * that is present again afterwards was created by the call
		 * rather than pulled back from the guest -- so the guest's copy
		 * was never taken, both sides hold it, and whatever the call
		 * wrote never reaches the program.
		 */
		int pre = (pglog_on == 2 && !pglog_transfers_only) ?
			ctx_present(&target, pglog_page) : -1;
		unsigned long fpre = n_watch_faults;

		source_clone_words_probe(&target, &c, "after");
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
		 * them here. Executor invalidations use the retained access journal;
		 * no destructive range queue crosses a connection's MM replacement.
		 * Another thread may drain the same MM's log: the ranges describe
		 * that address space, independently of which call observed them.
		 */
		{
			struct vmctx_mmlog ml;
			uint32_t i;

			do {
				memset(&ml, 0, sizeof(ml));
				ml.max = VMCTX_MMLOG_MAX;
				if (source_binding_memory(&target, VMCTX_CTL_MMLOG, &ml) != 0)
					source_adapter_failed("draining source mapping metadata");
				if (ml.n>VMCTX_MMLOG_MAX || ml.dropped) {
					errno=EOVERFLOW;
					source_adapter_failed("incomplete source mapping metadata");
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
					if ((vs & (VMR_PG_SIZE-1)) ||
					    (ml.ent[i].end & (VMR_PG_SIZE-1)) || ml.ent[i].end<=vs) {
						errno=EPROTO;
						source_adapter_failed("invalid source mapping range");
					}

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
						map_trace("source", "vacate-drain ctx=%d as=%llu seq=%llu cur=%llu range=%llx-%llx after-call=%llu ret=%lld",
							pid, (unsigned long long)target.mm, (unsigned long long)ml.ent[i].seq,
							(unsigned long long)ml.cur, (unsigned long long)vs,
							(unsigned long long)ml.ent[i].end,
							(unsigned long long)rq->nr, (long long)c.ret);
						sh_file_forget(&target, vs, vl);
					}
				}
				reply->mmseq=ml.cur;

			} while (ml.n == VMCTX_MMLOG_MAX);
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
		if (pglog_on == 2 && !pglog_transfers_only) {
			int post = ctx_present(&target, pglog_page);

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
				if (ctx_peek(&target, pglog_page, pg,
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
		    ctx_present(&target, rq->args[pa]) == 1) {
			char buf[sizeof(last_path)];
			long g = ctx_peek(&target, rq->args[pa], buf, sizeof(buf));

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
		int have = ctx_present(&target, rq->args[0]);

		if (have == 1)
			ctx_peek(&target, rq->args[0], &here, sizeof(here));
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
	struct source_shared_mapping shared;
	note_shared_mapping(&target,rq,(long)c.ret,&shared);
	source_map_effect(rq,(long)c.ret,shared.object,shared.offset,shared.length,&reply->mapping);
	/* Exec starts a distinct MM. Neither the surviving old MM's metadata nor
	 * records already populated for the new MM may be erased here. */

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
	memcpy(out,&gate->call.regs,sizeof(*out));
	*out_valid=1;

	return (long)gate->call.ret;
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
/* VMHOME_NATIVE_LOADER=1: run the first image's loader natively to AT_ENTRY
 * (the old shortcut). Default 0: hand over at the exec stop. See oracle_start. */
static struct vmctx_cpu_model source_cpu_model;
static pthread_mutex_t cpu_model_lock = PTHREAD_MUTEX_INITIALIZER;
static __thread int conn_cpu_model_ready;

static long oracle_start(char *argv_blob, uint64_t len, void *out, size_t outmax)
{
	char *argv[64];
	struct user_regs_struct regs;
	int n = 0, st;
	pid_t kid;

	if (ctx0_source)
		return -EBUSY;
	if (!conn_cpu_model_ready) return -EPROTO;
	if (getenv("VMHOME_NATIVE_LOADER") && atoi(getenv("VMHOME_NATIVE_LOADER"))) {
		fprintf(stderr, "[vmhome] native loader cannot honor the negotiated CPU model\n");
		return -EOPNOTSUPP;
	}
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
		if (monitor_diagnostics_child())
			_exit(125);
		/* Set before exec, so the kernel omits vDSO/vvar and their auxv
		 * entry from every image, including descendants' later execs. */
		if (source_control_raw(getpid(), VMCTX_CTL_SOURCE_EXEC, NULL) < 0) {
			perror("vmhome: source exec policy");
			_exit(125);
		}
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
	/* No source user instruction runs: the loader and its IFUNC choices
	 * execute only after the negotiated model is installed in the guest. */
	/*
	 * And from here it is a service context: it holds the program and never
	 * executes any of it. ADOPT arms that before the detach, so there is no
	 * window in which a task stopped at an entry point is simply let go —
	 * which is what a detached tracee does, and it ran the program natively
	 * on this machine once, printing the answer of a run that never
	 * happened.
	 */
	if (source_control_raw(kid, VMCTX_CTL_ADOPT, NULL) != 0) {
		int e = errno;

		fprintf(stderr, "[vmhome] cannot adopt %d as a service context: "
			"%s\n", (int)kid, strerror(e));
		kill(kid, SIGKILL);
		waitpid(kid, NULL, 0);
		return -e;
	}
	if (source_control_raw(kid, VMCTX_CTL_CPU_MODEL, &source_cpu_model) < 0) {
		int e = errno;
		kill(kid, SIGKILL); waitpid(kid, NULL, 0);
		return -e;
	}
	struct source_context native={.fd=-1};
	source_id id=0;
	if (source_context_open(kid,&native) || !(id=source_record_add(&native))) {
		int e=errno;
		source_context_close(&native);
		kill(kid,SIGKILL); waitpid(kid,NULL,0);
		return -e;
	}
	if (ptrace(PTRACE_DETACH,kid,0,0)<0) {
		int e=errno;
		source_record_signal(id,SIGKILL); waitpid(kid,NULL,0);
		source_record_release(id);
		return -e;
	}
	ctx0_source=id;
	ctx0_native_pid=kid;

	/* Hand back the register set the program should start with. */
	{
		uint64_t u[21];

		u[0]=regs.rax; u[1]=regs.rbx; u[2]=regs.rcx; u[3]=regs.rdx;
		u[4]=regs.rsi; u[5]=regs.rdi; u[6]=regs.rbp; u[7]=regs.rsp;
		u[8]=regs.r8;  u[9]=regs.r9;  u[10]=regs.r10; u[11]=regs.r11;
		u[12]=regs.r12; u[13]=regs.r13; u[14]=regs.r14; u[15]=regs.r15;
		u[16]=regs.rip; u[17]=regs.eflags; u[18]=regs.orig_rax;
		/* With the native loader the thread pointer is already set up
		 * and is part of the state handed over; at the exec stop it is
		 * 0 and ld.so's arch_prctl, forwarded, installs it. */
		u[19]=regs.fs_base; u[20]=regs.gs_base;
		memcpy(out, u, sizeof(u) < outmax ? sizeof(u) : outmax);
	}
	fprintf(stderr, "[vmhome] %s is held by service context %d, "
		"rip=0x%llx rsp=0x%llx (%s)\n", argv[0], (int)kid,
		(unsigned long long)regs.rip, (unsigned long long)regs.rsp,
		"handed over at the exec stop: the loader runs in the guest");
	return 21 * 8;
}

static long pid_layout(source_target of, char *out, size_t outmax);


/*
 * The layout of a NAMED service context -- what a context that has exec'd asks
 * for. The oracle is the FIRST program as loaded; after an exec the only true
 * map is the exec'd service context's own, and serving the oracle's instead is
 * how a fresh child was handed the previous image's protections.
 */
static long pid_layout(source_target target,char *out,size_t outmax)
{
    if(!outmax) {errno=EINVAL;return -EINVAL;}
    FILE *maps=source_maps(target);
    if(!maps)return -errno;
    size_t n=fread(out,1,outmax-1,maps);
    int saved=ferror(maps) ? EIO : fgetc(maps)!=EOF ? EOVERFLOW : 0;
    fclose(maps);
    if(saved) {errno=saved;return -saved;}
    out[n]=0;
    if(getenv("VMHOME_TRACE_LAYOUT"))fprintf(stderr,"[vmhome] program map:\n%s",out);
    return (long)n;
}

/* The program as it was loaded, for the far side's first look at it. */
static long oracle_page(source_target target,uint64_t addr,uint64_t len,
        char *out,size_t outmax)
{
    if(len>outmax)len=outmax;
    return ctx_peek(target,addr,out,len);
}

/*
 * This context's memory as it is now, read out of the task that holds it.
 *
 * A failure here is information, not an error to paper over: an address the
 * source has not mapped reports EFAULT, which is a real segmentation fault for
 * the guest rather than an excuse to zero-fill. There is nothing left to guess
 * with — the mappings read are the program's own.
 */

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
#include "source-snapshot.h"

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
static long ctx_write_prep(source_target pid, uint64_t addr)
{
	uint64_t page = addr & ~(uint64_t)(VMR_PG_SIZE - 1);
	source_mm_id kids[FORK_REL_MAX];
	int n, i, owed = 0, prot;

	struct vmctx_serve sv;

	n = fork_rel_children(pid->mm, kids, FORK_REL_MAX);
	memset(&sv, 0, sizeof(sv));
	sv.addr = page;
	sv.flags = VMCTX_SERVE_PROBE;
	if(source_binding_memory(pid, VMCTX_CTL_SERVE, &sv))return VMR_CTXPAGE_FAILED;
	if(sv.status==VMCTX_SERVE_NOMAP)return -EFAULT;
	if(sv.status==VMCTX_SERVE_DENIED)return -EACCES;
	prot = sv.status != VMCTX_SERVE_NOMAP && (sv.class & VMCTX_PGC_RO);
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
		if (!cow_mm_is_broken(kids[i], page)) {
			/* This asks the executor to copy; it does not prove a copy
			 * committed there. Preserve the obligation until a real
			 * child ownership publication supersedes it. */
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
			source_target_id(pid), (unsigned long long)page, owed);
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
static int cow_still_shared(source_target pid, uint64_t addr, int nocow)
{
	uint64_t page = addr & ~(uint64_t)(VMR_PG_SIZE - 1);

	struct vmctx_serve sv;

	if (nocow || !pid || !fork_parent_mm(pid->mm))
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
	if (source_binding_memory(pid, VMCTX_CTL_SERVE, &sv) != 0)
		return VMR_CTXPAGE_FAILED;
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
			"already is\n", (unsigned long long)page, source_target_id(pid),
			(int)as_live_mm(fork_parent_mm(pid->mm),0,0));
	plog(page, "SERVE: still the original's; answered COWBREAK");
	return 1;
}

/* One saved MM/page and one connection-owned native ticket. No control may
 * select another representative after BEGIN. Non-delivery outcomes cancel
 * their ticket; bytes and explicit zero grants stay owned until wire ACK. */
static long ctx_page(struct source_custody *custody,source_target target,
        uint64_t address,uint64_t length,char *out,size_t capacity,int nocow,
        struct source_page_receipt *receipt)
{
    if(!receipt || !target)return VMR_CTXPAGE_FAILED;
    *receipt=(struct source_page_receipt){0};
    if(length!=VMR_PG_SIZE || (address&4095) || length>capacity ||
       address>INT64_MAX-length)return VMR_CTXPAGE_FAILED;
    int shared=cow_still_shared(target,address,nocow);
    if(shared<0)return shared;
    if(shared)return VMR_CTXPAGE_COWBREAK;
    struct source_custody_entry *entry=NULL;
    if(source_custody_begin(custody,target,address,&entry))
        return !entry && (errno==ESRCH || errno==ENXIO) ? -errno:VMR_CTXPAGE_FAILED;
    uint64_t episode=entry->transfer.ticket;
    if(source_transfer_capture(&entry->transfer)) {
        /* Only confirmed cancellation proves a refused capture changed no
         * ownership. Uncertain commits retain custody for terminal cleanup. */
        int error=errno;
        if(error==EBUSY && !source_custody_cancel(custody,target,address,episode)) {
            n_take_busy++;return VMR_CTXPAGE_CLAIMING;
        }
        return VMR_CTXPAGE_FAILED;
    }
    struct vmctx_transfer captured=entry->transfer.result;
    if(pglog_on)plog(address,"CAPTURE %s episode=%llu class=0x%x record-was=%s gen=%u",
        serve_stname(captured.status),(unsigned long long)episode,captured.class,
        pg_stname((int)captured.state),captured.gen);
    int data=captured.status==VMCTX_SERVE_TAKEN || captured.status==VMCTX_SERVE_COPIED;
    int zero=captured.status==VMCTX_SERVE_ABSENT && (captured.flags&VMCTX_SERVE_GRANT);
    if(data || zero) {
        if(source_transfer_read(&entry->transfer,out))return VMR_CTXPAGE_FAILED;
        if(zero)memset(out,0,VMR_PG_SIZE);
        *receipt=(struct source_page_receipt){.target=*target,.base=address,
            .episode=episode,.count=1,.taken=1};
        receipt->generation[0]=captured.gen;
        receipt->sum[0]=page_sum(out)&0xffff;
        if(captured.status==VMCTX_SERVE_TAKEN) {n_mov_hit++;src_clobber(address,out,1);}
        else n_mov_miss++;
        if(captured.class&VMCTX_PGC_RO)n_ro_served++;
        if(sh_file_has(target,address)) {
            n_shfile_served++;
            if(!(captured.class&VMCTX_PGC_SHARED))n_shfile_nomov++;
        }
        n_intransit_committed++;
        source_word_transfer(target,address,out,"CAPTURE",captured.status,captured.gen,captured.state);
        return VMR_PG_SIZE;
    }
    if(source_custody_cancel(custody,target,address,episode))return VMR_CTXPAGE_FAILED;
    switch(captured.status) {
    case VMCTX_SERVE_CLAIMING:
        n_serve_refused++;
        if(captured.state==PG_INTRANSIT)n_serve_intransit_refused++;
        return VMR_CTXPAGE_CLAIMING;
    case VMCTX_SERVE_ABSENT:
        n_serve_absent++;
        return captured.state==PG_THEIRS ? VMR_CTXPAGE_NOTHOLDER:VMR_CTXPAGE_ABSENT;
    case VMCTX_SERVE_DENIED:return -EACCES;
    case VMCTX_SERVE_NOMAP:return -EFAULT;
    default:return VMR_CTXPAGE_FAILED;
    }
}

static long ctx_page_ack(struct source_custody *custody,source_target target,
        uint64_t address,uint64_t episode)
{
    if(source_custody_ack(custody,target,address,episode)) {
        n_intransit_ack_stale++;return -(errno ? errno:EIO);
    }
    n_intransit_acked++;return 0;
}

/* Explicit cleanup precedes native exit/admission waits. Lost custody ends
 * the complete execution session, including MM peers on other connections;
 * it never changes TRANSIT into an invented successful delivery. */
static void source_connection_close(struct source_custody *custody)
{
    size_t unresolved=custody->count;
    for(struct source_custody_entry *entry=custody->pending;entry;entry=entry->next)
        fprintf(stderr,"[vmhome] closing custody context=%llu mm=%llu epoch=%llu page=%llx ticket=%llu captured=%d read=%d settled=%d status=%u\n",
            (unsigned long long)entry->target.context,(unsigned long long)entry->target.mm,
            (unsigned long long)entry->target.epoch,(unsigned long long)entry->transfer.address,
            (unsigned long long)entry->transfer.ticket,entry->transfer.captured,
            entry->transfer.read,entry->transfer.settled,entry->transfer.result.status);
    if(source_custody_abandon(custody))source_adapter_failed("abandoning connection custody");
    if(unresolved) {
        errno=ENOTRECOVERABLE;
        source_adapter_failed("connection closed with unconfirmed page custody");
    }
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
	if (ctx0_source) {
		int st = 0;
		pid_t w;

		/*
		 * INSTRUMENT (session 40): the service process is this
		 * process's child, so its wait status is the one record of
		 * HOW it ended -- a group exit's code, or the signal that
		 * killed it -- and a SIGKILL sent to a group already in
		 * do_exit changes neither. Read before it is thrown away.
		 */
		w = waitpid(ctx0_native_pid, &st, WNOHANG);
		if (w == ctx0_native_pid)
			fprintf(stderr, "[vmhome] service process %d had ALREADY "
				"ended when the last context let go: %s %d\n",
				(int)ctx0_source,
				WIFSIGNALED(st) ? "killed by signal" : "exit status",
				WIFSIGNALED(st) ? WTERMSIG(st) : WEXITSTATUS(st));
		source_record_signal(ctx0_source, SIGKILL);
		if (w != ctx0_native_pid && waitpid(ctx0_native_pid, &st, 0) == ctx0_native_pid)
			fprintf(stderr, "[vmhome] service process %d ended on "
				"the final SIGKILL: %s %d\n", (int)ctx0_source,
				WIFSIGNALED(st) ? "killed by signal" : "exit status",
				WIFSIGNALED(st) ? WTERMSIG(st) : WEXITSTATUS(st));
		ctx0_source = 0;
	}
}

static int source_binding_snapshot(source_id id,struct vmr_mm_binding *binding)
{
	*binding=(struct vmr_mm_binding){0};
	if(!id)return 0;
	struct vmctx_context info;
	uint64_t epoch;
	while(source_record_snapshot(id,&info,&epoch)) {
		if(errno!=EAGAIN)return -1;
		usleep(1000);
	}
	*binding=(struct vmr_mm_binding){.context=(uint64_t)id,
		.mm=info.mm_identity,.epoch=epoch,.parent_mm=fork_parent_mm(info.mm_identity)};
	if(!vmr_binding_valid(binding)) {errno=EPROTO;return -1;}
	return 0;
}

static int source_publish_reply(source_id id,struct vmr_rsp *reply)
{
	struct vmr_mm_binding observed;
	if(source_binding_snapshot(id,&observed))
		source_adapter_failed("publishing source MM binding");
	/* Completion has already constructed effects against its saved binding.
	 * A consistency query must never relabel those effects with a newer MM. */
	if(vmr_binding_valid(&reply->binding) &&
	   !vmr_binding_equal(&reply->binding,&observed)) {errno=ESTALE;return -1;}
	reply->binding=observed;
	map_trace("source","binding context=%llu mm=%llu parent=%llu epoch=%llu state=%u effects=%u",
		(unsigned long long)reply->binding.context,(unsigned long long)reply->binding.mm,
		(unsigned long long)reply->binding.parent_mm,(unsigned long long)reply->binding.epoch,
		reply->call_state,reply->effects);
	return 0;
}

#include "source-admission.h"

/* Exception delivery can complete by ending the native task instead of
 * producing a signal-handler CPU frame. Keep the request's binding and only
 * publish that outcome after observing the retained native terminal record. */
static int source_exception_reply(source_id id,const struct vmr_req *request,
        struct vmr_cpu_state *cpu,struct vmr_rsp *reply)
{
    *reply=(struct vmr_rsp){.magic=VMR_MAGIC,.retval=-EINVAL};
    if(id<=0 || vmr_cpu_wire_decode(cpu,request->datalen))return 0;
    if(source_binding_snapshot(id,&reply->binding))return -1;
    int result=source_runtime_credit(id,request) ? -errno :
        ctx_deliver_exception(id,request->args[0],request->args[1],request->args[2],cpu);
    reply->retval=result==1 ? 0:result;
    if(result<0 && cpu_task_ended(id,-result,&reply->ended_status)) {
        reply->ended=1;reply->retval=0;
        map_trace("source","exception ended context=%llu mm=%llu status=%u native_result=%d",
            (unsigned long long)reply->binding.context,
            (unsigned long long)reply->binding.mm,reply->ended_status,result);
    } else if(result==1) {
        reply->datalen=vmr_cpu_wire_size(cpu);
        if(!reply->datalen)reply->retval=-EPROTO;
    }
    return 0;
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
	int source_owned=0;
	struct source_admission admission={0};
    struct source_custody custody={0};

	snprintf(conn_peer, sizeof(conn_peer), "%s", peer ? peer : "127.0.0.1");
	conn_ctx = 0;
	conn_source = 0;

	for (;;) {
		struct vmr_req rq;
		struct vmr_rsp rs;
		long ret = -ENOSYS;
		size_t outlen = 0;
		struct vmr_mm_binding request_target={0};
        struct source_page_receipt page_receipt={0};
		int cpu_ended = 0;
		unsigned cpu_end_status = 0;

		int got = read_all(c, &rq, sizeof(rq));

		if (got <= 0)
			break;
		if (rq.magic != VMR_MAGIC) {
			fprintf(stderr, "[vmhome] bad magic 0x%x\n", rq.magic);
			break;
		}
		if ((rq.nr != VMR_OP_PREPARE && rq.nr != VMR_OP_BOUNDARY &&
		     rq.nr != VMR_NR_EXCEPTION && (rq.execution_epoch || rq.execution_ns)) ||
		    (rq.nr == VMR_OP_BOUNDARY && (rq.call_nr || rq.args[0] || rq.args[1] ||
		     rq.args[2] || rq.args[3] || rq.args[4] || rq.args[5]))) {
			fprintf(stderr, "[vmhome] malformed execution metadata\n");
			break;
		}
		if (rq.datalen > sizeof(in)) {
			fprintf(stderr, "[vmhome] payload too big\n");
			break;
		}
		if (rq.datalen && read_all(c, in, rq.datalen) <= 0)
			break;

        int memory=vmr_memory_operation(rq.nr);
        if(memory) {
            if(!vmr_binding_valid(&rq.target) || rq.target.context>INT_MAX ||
               !source_record_has_binding((source_id)rq.target.context,rq.target.mm,rq.target.epoch) ||
               fork_parent_mm(rq.target.mm)!=rq.target.parent_mm || conn_source<=0) {
                errno=EPROTO;break;
            }
            if(rq.nr==VMR_OP_CHILD_PAGES) {
                struct source_memory_target parent;
                if(!rq.target.parent_mm || rq.args[0]!=rq.target.context ||
                   source_memory_target_for_mm(conn_source,rq.target.parent_mm,&parent)) {
                    errno=EPROTO;break;
                }
            } else if(rq.target.context!=(uint64_t)conn_source) {errno=EPROTO;break;}
            request_target=rq.target;
            if(rq.nr==VMR_OP_CTXPAGE &&
               (rq.datalen || (rq.args[0]&4095) || rq.args[1]!=VMR_PG_SIZE ||
                rq.args[0]>INT64_MAX-rq.args[1] || rq.args[2]>1 ||
                rq.args[3] || rq.args[4] || rq.args[5])) {errno=EPROTO;break;}
        } else if(!vmr_binding_empty(&rq.target)) {errno=EPROTO;break;}
        /* A native syscall/exit wait must never hold this connection's
         * unconfirmed page guards. Nested fetch and ACK traffic may finish
         * existing landings; other work requires all of them settled. */
        if(custody.count && rq.nr!=VMR_OP_CTXPAGE && rq.nr!=VMR_OP_INSTALLED &&
           rq.nr!=VMR_OP_STALETRAIL) {
            fprintf(stderr,"[vmhome] context=%llu request=%lld rejected with %zu unsettled page transfers\n",
                (unsigned long long)conn_ctx,(long long)rq.nr,custody.count);
            errno=EPROTO;break;
        }
        errno=0;

		/*
		 * An exception the guest raised, which is not a syscall: what
		 * the owner needs in order to decide what the program should
		 * experience is the whole machine state, not six arguments.
		 */
		if (rq.nr == VMR_NR_EXCEPTION) {
			source_thread_word_probe(conn_source, "exception");
			struct vmr_cpu_state *cpu = (struct vmr_cpu_state *)in;
			if(source_exception_reply(conn_source,&rq,cpu,&rs))break;
			n++;
			if(source_publish_reply(conn_source,&rs))break;
			if (writev_all(c, &rs, sizeof(rs), cpu, rs.datalen) < 0)
				break;
			fflush(NULL);
			continue;
		}

		if (rq.nr==VMR_OP_PREPARE || rq.nr==VMR_OP_EXECUTE || rq.nr==VMR_OP_CALL_POLL) {
			if (conn_source<=0) break;
			struct vmr_cpu_state *cpu=(struct vmr_cpu_state *)in;
			int done=source_admission_step(conn_source,&admission,&rq,cpu,&rs);
			if (done>0 && !rs.ended)
				done=source_admission_complete(conn_source,&admission,&rs,cpu,verbose) ? -1 : 1;
			if (done<0) {
				unsigned status;
				if (!cpu_task_ended(conn_source,errno,&status)) {
					fprintf(stderr,"[vmhome] admission failed source=%d op=%llu ticket=%llu: %s\n",
						conn_source,(unsigned long long)rq.nr,(unsigned long long)rq.ticket,strerror(errno));
					break;
				}
				rs.ended=1; rs.ended_status=status; rs.datalen=0;
				rs.call_state=VMR_CALL_COMPLETE;
				incall_set(conn_source,0);
				admission.active=0;
			}
			n++;
			if(source_publish_reply(conn_source,&rs))break;
			if (writev_all(c,&rs,sizeof(rs),cpu,rs.datalen)<0) break;
			continue;
		}
		if (rq.nr==VMR_OP_BOUNDARY) {
			if (conn_source<=0 || admission.active || rq.ticket) break;
			struct vmr_cpu_state *cpu=(struct vmr_cpu_state *)in;
			struct vmctx_syscall call={0};
			memset(&rs,0,sizeof(rs)); rs.magic=VMR_MAGIC;
			if (vmr_cpu_wire_decode(cpu,rq.datalen) || source_runtime_credit(conn_source,&rq) ||
			    cpu_state_ctl(conn_source,VMCTX_CTL_SETCPU,cpu) ||
			    ctl(conn_source,VMCTX_CTL_BOUNDARY,&call) ||
			    cpu_state_ctl(conn_source,VMCTX_CTL_GETCPU,cpu)) {
				if (!cpu_task_ended(conn_source,errno,&rs.ended_status)) break;
				rs.ended=1;
			} else {
				memcpy(&cpu->regs,&call.regs,sizeof(cpu->regs));
				rs.retval=call.ret; rs.regs_valid=1;
				rs.datalen=vmr_cpu_wire_size(cpu);
				if (!rs.datalen) break;
			}
			n++;
			if(source_publish_reply(conn_source,&rs))break;
			if (writev_all(c,&rs,sizeof(rs),cpu,rs.datalen)<0) break;
			continue;
		}

		switch (rq.nr) {
		case VMR_OP_RUNTIME_CAPS: {
			struct vmctx_runtime runtime = {
				.version = VMCTX_RUNTIME_ABI, .size = sizeof(runtime),
				.op = VMCTX_RUNTIME_INFO,
			};
			if (rq.datalen || ctl(0, VMCTX_CTL_RUNTIME, &runtime) < 0) {
				ret = -EOPNOTSUPP; break;
			}
			memcpy(out, &runtime.quantum_ns, sizeof(runtime.quantum_ns));
			ret = outlen = sizeof(runtime.quantum_ns);
			break;
		}
		case VMR_OP_ACCESS_READ:
		case VMR_OP_ACCESS_ACK: {
			struct source_memory_target target={source_target_id(&request_target),
                request_target.mm,request_target.epoch};
			struct vmctx_access_log log = {
				.version = VMCTX_ACCESS_ABI, .size = sizeof(log),
				.op = rq.nr == VMR_OP_ACCESS_READ ? VMCTX_ACCESS_READ : VMCTX_ACCESS_ACK,
				.mm_id = rq.args[0], .cursor = rq.args[1],
			};
			struct vmr_access_batch batch = {0};
			if (rq.datalen || conn_source <= 0) { ret = -EINVAL; break; }
			if(rq.args[0]!=target.mm) {ret=-EPROTO;break;}
			log.mm_id=target.mm;
			if (source_memory_target_call(&target, VMCTX_CTL_ACCESS_LOG, &log) < 0) {
				ret = -errno; break;
			}
			if (log.mm_id!=target.mm || log.n > VMR_ACCESS_MAX || log.error) { ret = -EPROTO; break; }
			batch.identity = log.mm_id;
			batch.cursor = log.cursor;
			batch.head = log.head;
			batch.acknowledged = log.acked;
			batch.count = log.n;
			ret = 0;
			for (unsigned i = 0; i < log.n; i++) {
				struct vmctx_access_event *e = &log.event[i];
				struct vmr_access_event *w = &batch.event[i];
				w->sequence = e->seq; w->start = e->start; w->end = e->end;
				w->protection = source_wire_protection(e->prot);
				switch (e->kind) {
				case VMCTX_ACCESS_UNMAP: w->kind = VMR_ACCESS_UNMAP; break;
				case VMCTX_ACCESS_DISCARD: w->kind = VMR_ACCESS_DISCARD; break;
				case VMCTX_ACCESS_PROTECT: w->kind = VMR_ACCESS_PROTECT; break;
				case VMCTX_ACCESS_DENY: w->kind = VMR_ACCESS_DENY; break;
				case VMCTX_ACCESS_ALLOW: w->kind = VMR_ACCESS_ALLOW; break;
				case VMCTX_ACCESS_CONSTRUCT: w->kind = VMR_ACCESS_CONSTRUCT; break;
				default: ret = -EPROTO; break;
				}
				if (ret < 0) break;
				if (e->kind == VMCTX_ACCESS_PROTECT && !(e->prot & PROT_WRITE))
					relro_mm_note(target.mm, e->start, e->end - e->start);
				else if (e->kind == VMCTX_ACCESS_PROTECT || e->kind == VMCTX_ACCESS_UNMAP ||
					 e->kind == VMCTX_ACCESS_CONSTRUCT)
					relro_mm_forget(target.mm, e->start, e->end - e->start);
			}
			if (ret < 0) break;
			memcpy(out, &batch, sizeof(batch));
			ret = outlen = sizeof(batch);
			break;
		}
		case VMR_OP_CPU_MODEL: {
			struct vmctx_cpu_model caps, agreed;
			const struct vmctx_cpu_model *peer = (const void *)in;
			if (rq.datalen != sizeof(*peer) || !vmctx_cpu_model_valid(peer)) {
				ret = -EINVAL; break;
			}
			if (ctl(0, VMCTX_CTL_CPU_CAPS, &caps) < 0) {
				ret = -errno; break;
			}
			vmctx_cpu_model_intersect(&agreed, &caps, peer);
			if (!vmctx_cpu_model_valid(&agreed)) { ret = -EOPNOTSUPP; break; }
			pthread_mutex_lock(&cpu_model_lock);
			if (ctx0_source || (source_cpu_model.version &&
			    memcmp(&source_cpu_model, &agreed, sizeof(agreed)))) ret = -EBUSY;
			else {
				source_cpu_model = agreed;
				conn_cpu_model_ready = 1;
				memcpy(out, &agreed, sizeof(agreed));
				ret = outlen = sizeof(agreed);
			}
			pthread_mutex_unlock(&cpu_model_lock);
			break;
		}
		case VMR_OP_CHILD_PAGES: {
			struct vmr_child_pages batch;
			if (!rq.args[0] || rq.args[0] > INT_MAX || rq.datalen ||
			    rq.args[2] || rq.args[3] || rq.args[4] || rq.args[5] ||
			    rq.target.context!=rq.args[0]) {
				ret = -EINVAL; break;
			}
			ret = source_child_pages(&request_target, rq.args[1], &batch);
			if (ret) {
				unsigned status;
				if (task_dead_status((source_id)rq.args[0],&status) && as_has_ended(request_target.mm)) {
					memset(&batch,0,sizeof(batch)); ret=0;
				}
			}
			if (!ret) {
				memcpy(out, &batch, sizeof(batch));
				ret = outlen = sizeof(batch);
			}
			break;
		}
		case VMR_OP_CPUSTATE: {
			struct vmr_cpu_state *cpu = (struct vmr_cpu_state *)out;
			if (rq.datalen || conn_source <= 0) { ret = -EINVAL; break; }
			ret = ctl(conn_source, VMCTX_CTL_GETCPU, cpu);
			if (!ret) {
				outlen = vmr_cpu_wire_size(cpu);
				ret = outlen ? (long)outlen : -EPROTO;
			} else {
				int saved = errno;
				if (saved == EAGAIN)
					ret = VMR_CPU_PENDING;
				else if (cpu_task_ended(conn_source, saved, &cpu_end_status)) {
					cpu_ended = 1;
					ret = VMR_CPU_ENDED;
				} else ret = -saved;
				errno = 0;
			}
			break;
		}
		case VMR_OP_START:
			pthread_mutex_lock(&cpu_model_lock);
			ret = oracle_start(in, rq.datalen, out, sizeof(out));
			pthread_mutex_unlock(&cpu_model_lock);
			outlen = (ret > 0) ? (size_t)ret : 0;
			if (ret > 0) {
				pthread_mutex_lock(&ctx_count_lock);
				n_contexts++;
				pthread_mutex_unlock(&ctx_count_lock);
				counted = 1;
				conn_source = ctx0_source;
				if (source_record_claim(conn_source)) source_adapter_failed("claiming root context");
				source_owned=1;
				/*
				 * Before the first forwarded call, and so before
				 * the first fault: a context whose fault nobody
				 * is waiting for simply sleeps in the kernel.
				 */
				ctx_monitor_start(conn_source, conn_ctx,
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
			if (!rq.args[1] || rq.args[1]>INT_MAX || conn_source) {
				ret=-EINVAL; break;
			}
			source_id srcpid=(source_id)rq.args[1];
			if (source_record_claim(srcpid)) { ret=-errno; break; }
			conn_ctx=rq.args[0]; conn_source=srcpid;
			source_owned=1; counted=1;
			source_thread_word_probe(srcpid, "child-connect");
			ret = 0;
			ctx_map_add(conn_ctx, conn_source);
			ctx_monitor_start(conn_source, conn_ctx, conn_peer, g_port);
			fprintf(stderr, "[vmhome] child guest context %llu is service "
				"context %d\n", (unsigned long long)conn_ctx,
				(int)conn_source);
			}
			break;
        case VMR_OP_LAYOUT:
            ret=pid_layout(&request_target,out,sizeof(out));
            outlen=ret>0 ? (size_t)ret : 0;
            break;
		case VMR_OP_PAGE:
			ret = oracle_page(&request_target,rq.args[0], rq.args[1], out, sizeof(out));
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
				source_id who = conn_source > 0 ? conn_source : ctx0_source;
				uint64_t cp0 = now_us();

				ret = who > 0 ? ctx_page(&custody,&request_target, rq.args[0], rq.args[1],
							 out, sizeof(out),
							 (int)rq.args[2],&page_receipt)
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
					if (as_has_ended(request_target.mm))
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
							as_has_ended(request_target.mm)
							? "HAS ENDED" : "is still alive");
					if (as_has_ended(request_target.mm)) {
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
				pg_stname(pg_state(&request_target,
						   rq.args[0])),
				pg_gen(&request_target,
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
			ret = ctx_write_prep(&request_target,
					     rq.args[0]);
			errno = 0;	/* the answer is the answer; see CTXPAGE */
			break;
		case VMR_OP_INSTALLED: {
            /* Report the native result. A rejected ACK must not look like
             * successful delivery merely because the request arrived. */
            if(rq.datalen ||
               rq.args[2] || rq.args[3] || rq.args[4] || rq.args[5]) {
                ret=-EINVAL;
            } else {
                ret=ctx_page_ack(&custody,&request_target,rq.args[0],rq.args[1]);
            }
            errno=0;
            break;
        }
		case VMR_OP_MAPSYNC:
			/*
			 * Nothing to synchronise. There is one address space
			 * and it is the program's; the mapping calls were
			 * performed in it, so it is never out of step with
			 * itself.
			 */
			ret = 0;
			break;
		case VMR_OP_FINISH: {
            source_connection_close(&custody);
			/* Normal completion follows a source-published terminal record.
			 * An executor failure instead cancels the exact retained task.
			 * Keep the peer's page service alive through native exit work:
			 * acknowledgement follows the terminal fact, never precedes it. */
			unsigned status;
			if (conn_source <= 0) { ret = -EPROTO; break; }
			if (!task_dead_status(conn_source, &status)) {
				fprintf(stderr, "[vmhome] executor ended before source context %d; cancelling native task\n", conn_source);
				if (source_record_signal(conn_source, SIGKILL) && errno != ESRCH)
					source_adapter_failed("cancelling unfinished source task");
				while (!task_dead_status(conn_source, &status)) usleep(1000);
			}
			fprintf(stderr, "[vmhome] native source context %d ended: %s %d\n",
				conn_source, WIFEXITED(status) ? "exit status" : "signal",
				WIFEXITED(status) ? WEXITSTATUS(status) : WTERMSIG(status));
			ret = 0;
			errno = 0;
			break;
		}
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
        if(memory)vmr_memory_receipt(&rq,&rs);
		if (cpu_ended) {
			rs.ended = 1;
			rs.ended_status = cpu_end_status;
		}
		/* This request owns the complete page metadata being published. */
		if (rq.nr == VMR_OP_CTXPAGE && ret > 0) {
			rs.ngen = page_receipt.count;
			memcpy(rs.pggen, page_receipt.generation, sizeof(rs.pggen));
			rs.pages_taken = page_receipt.taken;
            rs.page_episode=page_receipt.episode;
		}
		/* Header and payload in one write: one wakeup on the far side. */
		if(source_publish_reply(conn_source,&rs))break;
		if (writev_all(c, &rs, sizeof(rs), outlen ? out : NULL,
			       outlen) < 0)
			break;
		if (rq.nr != VMR_OP_CTXPAGE)
			fflush(NULL);
	}
    source_connection_close(&custody);
	if (admission.active) source_adapter_failed("connection ended during native admission");
	/* A dropped connection cannot grant a successful native exit. Its task
	 * is cancelled through the retained reference; no numeric token is a PID. */
	if (conn_source > 0) {
		unsigned status;
		if (!task_dead_status(conn_source, &status))
			source_record_signal(conn_source, SIGKILL);
	}
	/* Native exit can still fault while its assisted call is pending. */
	if (conn_source > 0)
		(void)as_of(conn_source); /* retain lineage after task retirement */
	if (conn_source > 0 && conn_source != ctx0_source) {
		ctx_map_forget(conn_ctx);
	}
	if (counted) oracle_put(conn_ctx);
	if (source_owned) source_record_release(conn_source);
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
	if (monitor_diagnostics_start()) {
		perror("vmhome: diagnostic channel");
		return 2;
	}
	g_port = port;
	if (prog_argc <= 0)
		fprintf(stderr, "[vmhome] usage: %s <port> [-v] <program> "
			"[args...]  -- the program belongs on THIS side; the "
			"destination only says where to connect\n", argv[0]);
	else
		fprintf(stderr, "[vmhome] will run %s (%d argument(s))\n",
			prog_argv[0], prog_argc);
	pglog_init();
	map_trace_init();
	pm_qos_hold("vmhome");
	if (getenv("VMCTX_SOCK_SPIN_US"))
		sock_spin_us = (unsigned)atoi(getenv("VMCTX_SOCK_SPIN_US"));
	if (getenv("VMCTX_RA_MIN_RTT_US"))
		ra_min_rtt_us = (unsigned)atoi(getenv("VMCTX_RA_MIN_RTT_US"));
	ctl_resolve();
	if(source_memory_capabilities()) {
		perror("vmhome: source memory identity capabilities");return 2;
	}
	if(source_transfer_caps()) {
		perror("vmhome: retained source transfer capabilities");return 2;
	}
	struct vmctx_access_log access_caps = {
		.version = VMCTX_ACCESS_ABI, .size = sizeof(access_caps),
	};
	if (ctl(0, VMCTX_CTL_ACCESS_LOG, &access_caps) ||
	    access_caps.version != VMCTX_ACCESS_ABI ||
	    access_caps.size != sizeof(access_caps)) {
		fprintf(stderr, "vmhome: source kernel lacks committed access journal\n");
		return 2;
	}
	struct vmctx_source_exit_caps exit_caps = {0};
	if (ctl(0, VMCTX_CTL_SOURCE_EXIT_CAPS, &exit_caps) ||
	    exit_caps.version != VMCTX_SOURCE_EXIT_ABI ||
	    exit_caps.size != sizeof(exit_caps) ||
	    !(exit_caps.features & VMCTX_SOURCE_EXIT_NATIVE_MM_RELEASE)) {
		fprintf(stderr, "vmhome: source kernel lacks native exit memory service\n");
		return 2;
	}
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
