// SPDX-License-Identifier: GPL-2.0
/*
 * vmremote — the destination: lend this machine's CPU and memory to a program
 *            that belongs to another one (avm.md objective 5).
 *
 *   vmremote <home-ip[:port]> --net
 *
 * The program exists only at the source, and this command line says so: it
 * names where to connect and nothing else. The source loads the program, this
 * side is told where to start, and the guest's instructions then run on this
 * hardware. Nothing about the program is on this disk and it is never execve'd
 * here -- and now it is not even NAMED here, because a path is a source-side
 * idea and this machine has no filesystem the program can reach. PRINCIPLES 6a.
 *
 * This machine owns nothing a program can name — no pid, no descriptor, no
 * mapping, no credential, no namespace, no parentage, no signal state. Every
 * syscall is forwarded, including the ones that look local: the source performs
 * the mmap, so the address the guest holds is the source's; the source performs
 * the clone, so the pid the guest gets is the source's; arch_prctl is decided
 * there and the resulting register value is applied here. The test each of
 * those has to pass is that this machine may hand back every context at any
 * instant and the source carries on with only its own copy.
 *
 * Memory is not described to this side, ever — no ranges, no layout, no
 * mapping decisions. A context's memory is a shared object in which a guest
 * virtual address is its own offset, and this machine discovers memory by
 * faulting: the kernel backs the address from that object, the monitor asks the
 * source for the bytes, and they are installed. Two contexts handed the same
 * object see the same page at the same address, which is the whole of sharing
 * an address space — so a guest thread is a context handed its parent's object
 * and a guest process is one given a new one. Nothing here knows what a thread
 * is.
 *
 * A page is kept coherent rather than copied: it is shared (both sides may
 * read, neither may write) or exclusive (one side may write), and it moves by
 * being taken out of one side rather than duplicated into the other.
 *
 * `--net` is not optional: it is the only arrangement there is.
 */
#define _GNU_SOURCE
#include <sys/stat.h>
#include <poll.h>
#include "vmctx_executor_map.h"
#include "vmctx_quiesce.h"
#include <stdio.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <stdint.h>
#include <limits.h>
#include <sys/syscall.h>
#include <time.h>
#include <execinfo.h>

static uint64_t now_us(void);
static unsigned int page_sum(const void *p);
static void step_timeout(const char *what, uint64_t addr, uint64_t us);
#include <sys/socket.h>
#include <ucontext.h>
#include <sys/wait.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>

#include <fcntl.h>
#include <pthread.h>
#include <sys/mman.h>
#include <linux/falloc.h>	/* a hand-over must reach the object, not just the page tables */
#include <dirent.h>
#include <signal.h>
#include <sys/uio.h>
#include <sched.h>

#include "vmctx_uapi.h"	/* struct vmctx_run_config, shared with the kernel */
#include "vmrproto.h"
#include "page-receipt.h"
#include "map-trace.h"
#include "pgstate.h"	/* per-page ownership state machine (conserved token) */
#include "own.h"		/* shared-memory ownership channel (REDESIGN.md session 34); load-bearing since session 35: one transition per guest fault, see fault_from_home_mode() */
#include "deadline.h"	/* vmctx_deadline_enforce: the nested page budgets */
#include "monitor-maps.h"

/*
 * Which source this binary was built from, and it is printed rather than
 * inferred. Timestamps cannot answer the question on this tree: rsync
 * preserves mtimes, so a freshly synced source looks OLDER than the binary
 * beside it, make decides there is nothing to do, and the run measures the
 * previous change. That has happened to the module, to vmremote and to
 * vmhome, each time producing numbers that looked clean and described code
 * that was not running. See verify-build.sh, which compares this against the
 * tree.
 */
#ifndef VMCTX_SRC_ID
#define VMCTX_SRC_ID "unknown"
#endif

#ifndef __NR_vmctx_run
#define __NR_vmctx_run 470
#endif
#ifndef __NR_vmctx_ctl
#define __NR_vmctx_ctl 471
#endif

#define VMCTX_FLAG_USERCODE          1u
#define VMCTX_FLAG_EXIT_PROCESS      2u
#define VMCTX_FLAG_REDIRECT_SYSCALL  4u
#define VMCTX_FLAG_WAIT_MONITOR      16u
#define VMCTX_FLAG_REDIRECT_FAULT    8u
#define VMCTX_FLAG_RESTORE           32u

#define VMCTX_CTL_ATTACH  1
#define VMCTX_CTL_DETACH  2
#define VMCTX_CTL_WAIT    3
#define VMCTX_CTL_RESUME  4
#define VMCTX_CTL_GETREGS 5
#define VMCTX_CTL_SETREGS 6
#define VMCTX_CTL_PEEK    7
#define VMCTX_CTL_POKE    8
#define VMCTX_CTL_TAKE    9	/* remove a page and return its last contents */
#define VMCTX_CTL_PROTECT 10	/* leave it readable but not writable         */
/*
 * Hand a page over from the whole address space at once: unmapped from every
 * context that has it, copied, and removed from the backing object, with no
 * moment in between where any of them can write it. Refused with a negative
 * answer by a kernel that does not have it and by a range that is not
 * object-backed, and the per-context path below is the fallback for both.
 */
#define VMCTX_CTL_TAKEOBJ 13
/*
 * The read-claim counterpart of TAKEOBJ: write-protect a page in every context
 * of the address space at once, under the backing folio's lock (folio_mkclean's
 * rmap walk), so there is no gap between one sibling's protect and the next in
 * which a still-writable sibling advances a page the source is about to be
 * promised nobody here writes. With buf != 0 it also returns the bytes as of
 * the protect (one atomic protect-and-read); with buf == 0 it only protects.
 * Returns PAGE_SIZE protected, 0 if the object does not have the page, negative
 * on error. The per-context PROTECT loop it replaces was the LOSTWRITE gap.
 */
#define VMCTX_CTL_PROTECTOBJ 15
#define VMCTX_CTL_MAPOBJ 16	/* map the object's own folio into the context */
#define VMCTX_CTL_APPLYMAP 17	/* apply a mapping change to a sibling context */
#define VMCTX_CTL_TRYFAULT 20	/* would the target's own kernel handle an
				 * access at addr? 0 = yes, -EFAULT = no
				 * mapping reaches it. The construction
				 * repair's discriminator; a kernel without
				 * the op answers EINVAL and the repair
				 * simply never fires there.               */

#define VMCTX_EV_SYSCALL 1
#define VMCTX_EV_FAULT   2
#define VMCTX_ACT_SELF   0
#define VMCTX_ACT_DONE   1
#define VMCTX_ACT_KILL   2

struct vmctx_event {
	uint32_t type;
	int32_t  pid;
	uint64_t nr;
	uint64_t args[6];
	uint64_t fault_addr, fault_err;
	uint64_t rip, rsp, rflags;
};
/* struct vmctx_reply comes from vmctx_uapi.h -- see the note there. */
struct vmctx_mem { uint64_t addr, len, buf; };

/*
 * A page take retries while the kernel says EBUSY (a pin has yet to clear). A
 * legitimate pin clears in a few milliseconds; anything longer is a lock nobody
 * is releasing. VMR_TAKE_SLOW is when to say so out loud; VMR_TAKE_STUCK is when
 * to stop -- there is no correct "continue anyway", so it is a crash, not a
 * fallback. Both are counts of 1ms sleeps.
 */
#define VMR_TAKE_SLOW	30
#define VMR_TAKE_STUCK	500
struct vmctx_uregs {
	uint64_t rax, rbx, rcx, rdx, rsi, rdi, rbp, rsp;
	uint64_t r8, r9, r10, r11, r12, r13, r14, r15;
	uint64_t rip, rflags, orig_rax;
	uint64_t fs_base, gs_base;
};

static long execution_control_raw(pid_t selector,unsigned command,void *argument)
{ return syscall(__NR_vmctx_ctl,selector,command,argument); }
#include "linux-execution-context.h"
#include "linux-execution-proc.h"
#include "linux-execution-object.h"
#include "execution-target.h"
typedef const struct execution_target *memory_target;
static int ctx_id(memory_target target)
{return target ? (int)target->context->identity : 0;}
static long execution_context_ctl(memory_target pid,unsigned command,void *argument);
static long ctl(memory_target pid, unsigned cmd, void *arg);
#define VMCTX_CPU_CONTEXT memory_target
#include "cpu-state.h"
static struct vmctx_cpu_model guest_cpu_model;
static uint64_t guest_runtime_quantum;

static int execution_runtime(memory_target pid, unsigned op, struct vmctx_runtime *q)
{
	memset(q, 0, sizeof(*q));
	q->version = VMCTX_RUNTIME_ABI;
	q->size = sizeof(*q);
	q->op = op;
	if (op == VMCTX_RUNTIME_ARM) q->quantum_ns = guest_runtime_quantum;
	return cpu_state_ctl(pid, VMCTX_CTL_RUNTIME, q);
}

static int execution_arm(memory_target pid)
{
	struct vmctx_runtime q;
	return execution_runtime(pid, VMCTX_RUNTIME_ARM, &q);
}
static long snapshot_map_object(memory_target pid, void *arg);
static long snapshot_take_page(memory_target pid, unsigned cmd, void *arg);

static long ctl(memory_target pid, unsigned cmd, void *arg)
{
	/* MAPOBJ installs a writable PTE, including on a read fault in a
	 * sibling that has never mapped the page. Every caller must preserve
	 * pending snapshots before that permission can become visible. */
	if (cmd == VMCTX_CTL_MAPOBJ)
		return snapshot_map_object(pid, arg);
	if (cmd == VMCTX_CTL_TAKEOBJ || cmd == VMCTX_CTL_TAKE)
		return snapshot_take_page(pid, cmd, arg);
	return execution_context_ctl(pid, cmd, arg);
}
static ssize_t execution_writev(memory_target target,const struct iovec *local,
		size_t count,const struct iovec *remote)
{
	size_t total=0;
	for(size_t i=0;i<count;i++) {
		if(local[i].iov_len!=remote[i].iov_len || local[i].iov_len>SSIZE_MAX-total) {
			errno=EINVAL;return -1;
		}
		total+=local[i].iov_len;
	}
	ssize_t done=0;
	for(size_t i=0;i<count;i++) {
		if(!local[i].iov_len)continue;
		struct vmctx_mem memory={.addr=(uintptr_t)remote[i].iov_base,
			.buf=(uintptr_t)local[i].iov_base,.len=local[i].iov_len};
		long got=ctl(target,VMCTX_CTL_POKE,&memory);
		if(got<0)return done ? done : -1;
		if((uint64_t)got>memory.len) {errno=EPROTO;return done ? done : -1;}
		done+=got;
		if((size_t)got!=local[i].iov_len)return done;
	}
	return done;
}

#include "monitor-owner.h"

static int io_all(int wr, void *buf, size_t n);   /* fwd decl */
static int io_send2(const void *a, size_t na, const void *b, size_t nb);
static long poke(memory_target pid, uint64_t addr, const void *buf, uint64_t len);
/* Which caller's install this is, for the clobber detector; see poke(). */
static __thread int poke_site;
/* 1 = not yet read from the environment; 0 = off; 2 = every page; else the page.
 * Declared here because page_take_mode() consults it too, for the take-side
 * live-page check. */
static uint64_t clobber_watch = 1;
static uint64_t clobber_page(void);
static int page_read_as(memory_target ctx, uint64_t addr, void *buf);
static int pull_active(uint64_t addr);
static long obj_write(memory_target pid, uint64_t addr, const void *buf, uint64_t len);
/* Content history: see the block beside clobber_live_check(). */
static int hist_armed(uint64_t base);
static void hist_note(uint64_t base, const void *buf);
static void hist_check(uint64_t base, const void *buf, int site);

static void clobber_check(memory_target pid, uint64_t addr, const void *buf,
			  uint64_t len, int site);
static void obj_forget(memory_target pid, uint64_t addr, uint64_t len);
static long peek(memory_target pid, uint64_t addr, void *buf, uint64_t len);
static void drop_monitor_fds(int keep);
static int shared_obj;
static int backing_has(memory_target ctx, uint64_t addr);
static void tracep(uint64_t addr, memory_target pid, const char *what, long v);
static void plog(uint64_t addr, const char *fmt, ...) __attribute__((format(printf,2,3)));
static void page_provenance(memory_target pid, uint64_t page, const char *why);
static int monitor_mm_check(const char *when);
static void ack_installed(struct source_page_receipt *receipt,uint64_t base);
static void source_receipt_finish(struct source_page_receipt *receipt);
static void ack_flush(void);
/* Settle landings that went to the object because the poked context had no
 * mapping there (the netsurf-242 producer; see page_recall_all). */
static unsigned long n_settle_to_object;
/* Pages this side held whose bytes an mremap MOVE relocated into the object
 * at the new offsets (see reply_note_map case 25). */
static unsigned long n_mremap_moved;
/* Ranges the source's mm-mutation hook reported vacated, applied here. */
static unsigned long n_ranges_vacated;
static unsigned long n_vacate_overflow;
/*
 * INSTRUMENT for the hook's named-but-unphotographed ordering edges (the
 * steal window: a sibling connection's drain carrying another thread's
 * vacate, unordered against that thread re-mapping the range; and
 * vacate_pend spillover past 8 riding a later reply). A fault landing INSIDE
 * a recently vacated range is the signature both would leave -- the range
 * was re-mapped and touched while its vacate was still in flight, and the
 * vacate then punched live state. The ring keeps the last 32 vacates with
 * their application time; the fault entry checks it and names the first
 * hits with their age. A fault here is not itself a verdict (a legitimate
 * re-map of a freed range faults and is served fine) -- the AGE and the
 * cross-reference against vmhome's VACATE-drain print (which names the
 * draining call) are what decide.
 */
static struct { uint64_t start, end, t_us; } vac_recent[32];
static unsigned vac_recent_n;	/* free-running; % 32 = slot */
static pthread_mutex_t vac_recent_lock = PTHREAD_MUTEX_INITIALIZER;
static unsigned long n_fault_in_vacated;

/*
 * The other half of the ordering photograph: the last constructions (reply
 * map SETs), so an arriving vacate can be checked against them. The keep-zone
 * already excludes the vacate's OWN reply's construction; a vacate
 * overlapping a construction from a DIFFERENT recent reply is the steal
 * window or the spillover actually firing -- the punch would land on live
 * state. Counted and named; the count deciding whether the ordering fix
 * gets built.
 */
static struct { uint64_t start, end, t_us; } map_recent[32];
static unsigned map_recent_n;
static pthread_mutex_t map_recent_lock = PTHREAD_MUTEX_INITIALIZER;
static unsigned long n_vacate_late;

static void map_recent_note(uint64_t start, uint64_t end)
{
	pthread_mutex_lock(&map_recent_lock);
	map_recent[map_recent_n % 32].start = start;
	map_recent[map_recent_n % 32].end   = end;
	map_recent[map_recent_n % 32].t_us  = now_us();
	map_recent_n++;
	pthread_mutex_unlock(&map_recent_lock);
}

static void vacate_late_check(uint64_t start, uint64_t end)
{
	unsigned i, n;
	uint64_t now = now_us();

	pthread_mutex_lock(&map_recent_lock);
	n = map_recent_n < 32 ? map_recent_n : 32;
	for (i = 0; i < n; i++) {
		if (end <= map_recent[i].start || start >= map_recent[i].end)
			continue;
		if (now - map_recent[i].t_us > 500000)
			continue;	/* old construction; the vacate is
					 * almost certainly the older fact */
		n_vacate_late++;
		if (n_vacate_late <= 8)
			fprintf(stderr, "[vmremote] VACATE-LATE: vacate "
				"0x%llx-0x%llx overlaps a map SET "
				"0x%llx-0x%llx applied %llums ago by a "
				"DIFFERENT reply -- the ordering edge is "
				"real, and this punch lands on live state\n",
				(unsigned long long)start,
				(unsigned long long)end,
				(unsigned long long)map_recent[i].start,
				(unsigned long long)map_recent[i].end,
				(unsigned long long)((now -
						      map_recent[i].t_us) /
						     1000));
		break;
	}
	pthread_mutex_unlock(&map_recent_lock);
}

static void vac_recent_note(uint64_t start, uint64_t end)
{
	pthread_mutex_lock(&vac_recent_lock);
	vac_recent[vac_recent_n % 32].start = start;
	vac_recent[vac_recent_n % 32].end   = end;
	vac_recent[vac_recent_n % 32].t_us  = now_us();
	vac_recent_n++;
	pthread_mutex_unlock(&vac_recent_lock);
}

static void vac_recent_check(memory_target pid, uint64_t addr, uint64_t err)
{
	unsigned i, n;

	pthread_mutex_lock(&vac_recent_lock);
	n = vac_recent_n < 32 ? vac_recent_n : 32;
	for (i = 0; i < n; i++) {
		if (addr < vac_recent[i].start || addr >= vac_recent[i].end)
			continue;
		n_fault_in_vacated++;
		if (n_fault_in_vacated <= 8)
			fprintf(stderr, "[vmremote] FAULT-IN-VACATED: ctx %d "
				"0x%llx err 0x%llx inside 0x%llx-0x%llx "
				"vacated %llums ago -- re-mapped range, or an "
				"ordering edge firing\n", ctx_id(pid),
				(unsigned long long)addr,
				(unsigned long long)err,
				(unsigned long long)vac_recent[i].start,
				(unsigned long long)vac_recent[i].end,
				(unsigned long long)((now_us() -
						      vac_recent[i].t_us) /
						     1000));
		break;
	}
	pthread_mutex_unlock(&vac_recent_lock);
}

/*
 * Was `addr` inside any of the last 32 vacated ranges? A page that was
 * legitimately munmapped and re-mmapped (glibc's arena churn does this by the
 * dozen) reads back as a fresh zero page and MUST -- so a zero-fill there is
 * correct, not a lost store. This lets the lost-store detector exclude exactly
 * that case and fire only on a page nothing unmapped.
 */
static int vac_recent_covers(uint64_t addr)
{
	unsigned i, n;
	int hit = 0;

	pthread_mutex_lock(&vac_recent_lock);
	n = vac_recent_n < 32 ? vac_recent_n : 32;
	for (i = 0; i < n; i++)
		if (addr >= vac_recent[i].start && addr < vac_recent[i].end) {
			hit = 1;
			break;
		}
	pthread_mutex_unlock(&vac_recent_lock);
	return hit;
}
/*
 * The range an mremap relocation just wrote into the object (case 25), which
 * a vacate arriving in the SAME reply must not punch: under MREMAP_FIXED the
 * hook logs the replaced target's unmap, and the relocated bytes now live at
 * exactly those offsets. Mappings still go; the bytes stay.
 */
static __thread struct { uint64_t start, end; } vacate_keep;
static int as_members(memory_target pid, memory_target *out, int max);
static int backing_find(memory_target target);
static int initial_backing_find(pid_t ctx);
static int own_object_page(memory_target pid, uint64_t base, void *buf);
static unsigned long n_from_own_backing;
/* Pages a fork child got by breaking copy-on-write against its parent here. */
static unsigned long n_cow_broken_here;
/* ...and the ones handed straight to the source's own copy of the address space. */
static unsigned long n_cow_refused;	/* COWBREAK requests answered 'nothing owed' */
static unsigned long n_cow_own_served;	/* COWBREAKs answered with the copy's OWN page (a give landed between the source's GET and its break) */
static unsigned long n_get_chain_given;	/* GETs for a fork copy answered by giving from the chain there and then */
static unsigned long n_cow_given_to_home;
/*
 * Pages a copy was given by an ancestor FURTHER UP than its parent: a fork of
 * a fork inherits pages its own parent never held (the parent itself still
 * inherits them), and the nearest ancestor still under the fork's protection
 * is the one holding exactly what the copy inherited. See cow_give_from_chain.
 */
static unsigned long n_cow_chain_given;
static unsigned long n_cow_relaxed_given;	/* last-resort gives: both
						 * machines' records empty-handed,
						 * nearest READABLE ancestor won */
/* Write grants that needed no take: the page was here, only write-protected. */
static unsigned long n_grant_mapobj;
/*
 * Write grants that DID take: the take-and-put-back fallback in
 * page_make_writable(). NOTES §6.3 records this path re-creating the folio
 * from pre-take bytes outside ctl_lock -- the same class as the kernel's
 * measured read-poke-back loss -- but nothing counted how often it runs, so
 * "rare" was an assumption. The second counter is the conviction probe
 * (VMR_TAKEPUT_PROBE=1): the object's bytes moved between the take and the
 * poke, which is precisely the interleave that loses a sibling's store.
 */
static unsigned long n_grant_takeput, n_grant_takeput_raced;
/*
 * Faults answered from this address space's own object where the faulting
 * context had never itself installed the page -- a sibling context of the same
 * address space had. Before the gate was widened to address-space scope these
 * were fetched from the peer, over the sibling's live stores.
 */
static unsigned long n_as_sibling_local;
static long ctl(memory_target pid, unsigned cmd, void *arg);
static void mark_installed(memory_target pid, uint64_t addr, uint64_t len);
static void mark_handed_over(memory_target pid, uint64_t page);
static int page_present(memory_target pid, uint64_t addr);
/*
 * PRESENT / ABSENT / UNKNOWN — and it has to be three, because the two things
 * that ask have opposite safe answers when it cannot be told.
 *
 * A *fault* asks so it can decline: a page the guest already has is not one we
 * owe it. Answering "present" when unsure means declining, which means the
 * local kernel zero-fills, and the next fault finds the page genuinely present
 * and declines again — one unanswerable question and that page is zeros for the
 * rest of the run. So a fault must treat unknown as absent and serve it.
 *
 * The *page channel* asks the opposite way round: it is deciding whether the
 * guest has anything worth sending, and answering "absent" tells the shadow to
 * use its own copy instead. After a mirror is dropped that copy is a zero page.
 * So the channel must treat unknown as present and read it.
 *
 * Collapsing the two into one boolean fixes whichever caller the default suits
 * and breaks the other, which is exactly what happened: making it default to
 * absent stopped faults being starved and started the page channel answering
 * ABSENT for pages the guest had. An exec'd program's `openat` path arguments
 * arrived in the shadow as empty strings, every one returned ENOENT, and
 * glycin's image loader exited without finding a single library.
 */
#define PAGE_ABSENT  0
#define PAGE_PRESENT 1
#define PAGE_UNKNOWN (-1)
static int fault_from_home(memory_target pid, uint64_t vec, uint64_t addr, uint64_t err);
static int fault_from_home_mode(memory_target pid, uint64_t vec, uint64_t addr,
				uint64_t err, int supply_only);
static void shared_range_inherit(memory_target child, memory_target parent);

/* Actual payload length of the last completed call on this connection. */
static __thread uint64_t home_data_len;

/*
 * Did the last home_call reach the owner and come back, or did the transport
 * fail?
 *
 * home_call answers -1 for both, and the two mean opposite things. A short
 * write, a dropped connection or a reply that will not fit says nothing
 * whatever about the program; the owner's own "no" is a statement about it. A
 * caller that cannot tell them apart acts on an answer that was never given --
 * and this project has already paid for that once, when vmhome's dispatch
 * returned -ENOSYS for a request number it did not implement and this side read
 * it as "the program has no such mapping". Every fault on that path looked like
 * a violation, and a whole line of reasoning was built on three round trips
 * being mistaken for three disagreements.
 *
 * Beside the call rather than returned through it, for the reason home_regs is:
 * almost no caller needs it, and the ones that do are asking a question whose
 * wrong answer is destructive. Per thread, because several service threads talk
 * to the owner at once.
 */
static __thread int home_call_failed;
static __thread unsigned home_ended, home_ended_status;
static long home_call_target_runtime(uint64_t nr, const uint64_t args[6],
		      const void *indata, uint64_t inlen,
		      void *outdata, uint64_t outmax,
		      uint64_t epoch, uint64_t execution_ns,struct vmr_rsp *receipt,
                      const struct vmr_mm_binding *target)
{
	struct vmr_req rq = { .magic = VMR_MAGIC, .nr = nr, .datalen = inlen,
		.execution_epoch = epoch, .execution_ns = execution_ns };
	struct vmr_rsp rs;
	static __thread char scratch[256 * 1024];

	if(target)rq.target=*target;
	memcpy(rq.args, args, sizeof(rq.args));
	home_data_len = 0;
	home_ended = home_ended_status = 0;
	home_call_failed = 1;		/* cleared once a reply is in hand */
    if(vmr_memory_operation(nr) ? !vmr_binding_valid(&rq.target) :
       !vmr_binding_empty(&rq.target)) {errno=EPROTO;return -1;}
    if(io_send2(&rq,sizeof(rq),indata,inlen)<0)return -1;
    if(io_all(0,&rs,sizeof(rs))<0)return -1;
    if(rs.magic!=VMR_MAGIC || !vmr_rsp_bindings_valid(&rs) ||
       !vmr_memory_reply_matches(&rq,&rs) || !source_page_response_valid(&rq,&rs) || rs.ngen>16 ||
       rs.datalen>sizeof(scratch) || rs.datalen>outmax ||
       (rs.datalen && !outdata)) {errno=EPROTO;return -1;}
    /* Framing/identity and the complete body precede every caller-visible
     * byte or receipt. A truncated reply must leave its output untouched. */
    if(rs.datalen && io_all(0,scratch,rs.datalen)<0)return -1;
    if(rs.datalen)memcpy(outdata,scratch,rs.datalen);
	home_data_len = rs.datalen;
	home_ended = rs.ended;
	home_ended_status = rs.ended_status;
	home_call_failed = 0;		/* the owner answered; retval is its answer */
	if(receipt)*receipt=rs;
	return (long)rs.retval;
}

static long home_call_runtime(uint64_t nr,const uint64_t args[6],
        const void *indata,uint64_t inlen,void *outdata,uint64_t outmax,
        uint64_t epoch,uint64_t execution_ns,struct vmr_rsp *receipt)
{
    return home_call_target_runtime(nr,args,indata,inlen,outdata,outmax,
        epoch,execution_ns,receipt,NULL);
}

static long home_binding_call(const struct vmr_mm_binding *target,uint64_t nr,
        const uint64_t args[6],const void *indata,uint64_t inlen,
        void *outdata,uint64_t outmax,struct vmr_rsp *receipt)
{
    return home_call_target_runtime(nr,args,indata,inlen,outdata,outmax,0,0,receipt,target);
}

static long home_memory_call(memory_target target,uint64_t nr,const uint64_t args[6],
        const void *indata,uint64_t inlen,void *outdata,uint64_t outmax,
        struct vmr_rsp *receipt)
{
    if(!target) {errno=EPROTO;home_call_failed=1;return -1;}
    return home_binding_call(&target->source,nr,args,indata,inlen,outdata,outmax,receipt);
}

static long home_call(uint64_t nr, const uint64_t args[6],
		      const void *indata, uint64_t inlen,
		      void *outdata, uint64_t outmax)
{
	return home_call_runtime(nr, args, indata, inlen, outdata, outmax, 0, 0,NULL);
}

/*
 * Where the wall clock goes, in four buckets that add up to the run. Guessing
 * at this has been wrong every time it was tried.
 */
static double t_now(void)
{
	struct timespec t;

	clock_gettime(CLOCK_MONOTONIC, &t);
	return (double)t.tv_sec + (double)t.tv_nsec / 1e9;
}
static double t_start, t_first_entry, t_fault, t_syscall;
/* Teardown, phase by phase: 'everything else' was two seconds and
 * naming which two seconds is the only way to remove them. */
static double t_svc_end, t_reaped, t_kids, t_finished;
/*
 * Time the guest spends between events — its own execution, and any wait
 * the local kernel is serving. It is not fault time and not forwarded-
 * syscall time, so it lands in 'guest/other', where two seconds hid.
 */
static double t_wait, t_wait_max;
static unsigned long wait_max_after_nr;
static int wait_max_after_type = -1;
static unsigned long n_fault_ev;

/* ---- page service: hand out (and take back) the guest's pages ---------- */
/*
 * The machine executing the guest's syscalls needs its memory. It cannot be
 * given a list up front — which pages a syscall touches is only known once the
 * kernel runs it — so we serve them one at a time, and accept back the ones it
 * wrote. Runs in a thread so it works while the main loop waits for events;
 * any thread of the monitoring process may drive the context.
 */
static pid_t ctx_pid;

/*
 * Every context we are servicing. A guest that forks becomes several processes
 * here, and home must be able to name which one's memory it wants: the parent
 * and child diverge from the moment of the fork.
 */
/*
 * 256, append-only, and a row OUTLIVES its context BY DESIGN -- both
 * halves measured on tests/pf5 (75 fatal fork children through one guest).
 *
 * The cap: at 64 slots the table filled at child #64, ctx_claim() answered
 * "not mine" for every BRAND-NEW context from there, nobody serviced it,
 * its pages could not be supplied, and the child crashed in libc on
 * zero-filled data (0x2d8 off a null FILE*) instead of dying by its own
 * fault -- 12 consecutive wrong deaths, every round after the wall.
 *
 * The lifetime: recycling a row when service_context() returned was tried
 * and REVERTED. The source keeps asking about a context through its
 * teardown, and the page service routes an ask for a KNOWN-dead context
 * into the "this address space has ended, stop" answer; a FORGOTTEN one it
 * treats as an imposter and drops the channel -- measured as vmhome
 * reconnecting 51,000 times against "names context N, which this monitor
 * does not know" while the first dead child's teardown asked forever. The
 * rows are how the dead stay answerable, so they stay.
 */
#define MAX_CTX 256
typedef int execution_id;
static execution_id ctxs[MAX_CTX];
static unsigned char ctx_reaped[MAX_CTX]; /* history stays; execution has ended */
static struct linux_execution_context ctx_native[MAX_CTX];
static uint64_t ctx_source_context[MAX_CTX]; /* immutable source context, independent of MM */
static struct execution_context_binding ctx_memory[MAX_CTX];
static struct execution_mm_registry executor_mms=EXECUTION_MM_REGISTRY_INIT;
static int backing_new(const char *why);
static int execution_mm_object(void *unused)
{(void)unused;return backing_new("vmctx-mm");}
static struct execution_mm *source_mm_resolve(const struct vmr_mm_binding *source)
{
	if(!vmr_binding_valid(source)) {errno=EPROTO;return NULL;}
	return execution_mm_resolve(&executor_mms,source->mm,source->parent_mm,
		execution_mm_object,NULL);
}
static struct execution_mm *mm_record(uint64_t identity)
{
	struct execution_mm *mm=execution_mm_find(&executor_mms,identity);
	if(!mm) {fputs("vmremote: unknown immutable MM identity\n",stderr);abort();}
	return mm;
}
static int mm_backing_find(uint64_t identity)
{return mm_record(identity)->backing_fd;}
static int mm_backing_has(uint64_t identity,uint64_t address)
{
	off_t page=(off_t)(address&~UINT64_C(4095));
	off_t data=lseek(mm_backing_find(identity),page,SEEK_DATA);
	return data>=0 && data<page+4096;
}
static long mm_obj_write(uint64_t identity,uint64_t address,const void *bytes,uint64_t length)
{return pwrite(mm_backing_find(identity),bytes,(size_t)length,(off_t)address);}
static int mm_page_read(uint64_t identity,uint64_t address,void *bytes);
static long initial_object_control(void *opaque,unsigned command,void *argument)
{return linux_execution_control(opaque,command,argument);}
static void backing_note(pid_t ctx,int fd);
static int backing_find(memory_target target);
static int initial_backing_find(pid_t ctx);
static int nctxs;
static pthread_mutex_t ctx_lock = PTHREAD_MUTEX_INITIALIZER;

/* Initial source binding is published with membership. Readiness may only
 * confirm that exact birth; it cannot supply an identity to a visible context. */
static int ctx_bind_source(pid_t id,const struct vmr_mm_binding *binding)
{
	if(!vmr_binding_valid(binding)) {errno=EPROTO;return -1;}
	struct execution_context_binding *context=NULL;
	pthread_mutex_lock(&ctx_lock);
	for(int i=0;i<nctxs;i++)if(ctxs[i]==id) {context=&ctx_memory[i];break;}
	pthread_mutex_unlock(&ctx_lock);
	if(!context || !vmr_binding_equal(&execution_target_capture(context)->source,binding)) {
		errno=EPROTO;return -1;
	}
	return 0;
}

static memory_target ctx_target(execution_id id)
{
	struct execution_context_binding *context=NULL;
	pthread_mutex_lock(&ctx_lock);
	for(int i=0;i<nctxs;i++)if(ctxs[i]==id) {context=&ctx_memory[i];break;}
	pthread_mutex_unlock(&ctx_lock);
	if(!context) {errno=ESRCH;return NULL;}
	return execution_target_capture(context);
}

/* Source callbacks can precede native image publication. An observation
 * retains its MM object but cannot issue controls against an unrelated native
 * binding. Existing history resolves to the exact retained target. */
static memory_target ctx_observe_source(const struct vmr_mm_binding *binding)
{
    if(!vmr_binding_valid(binding)) {errno=EPROTO;return NULL;}
    struct execution_context_binding *context=NULL;
    pthread_mutex_lock(&ctx_lock);
    for(int i=0;i<nctxs;i++) if(ctx_source_context[i]==binding->context) {
        context=&ctx_memory[i];break;
    }
    pthread_mutex_unlock(&ctx_lock);
    if(!context) {errno=ESRCH;return NULL;}
    struct execution_mm *mm=source_mm_resolve(binding);
    return mm ? execution_target_observe(context,mm,binding) : NULL;
}

static int ctx_matches_source(pid_t id,const struct vmr_mm_binding *binding)
{
	int matches=0;
	pthread_mutex_lock(&ctx_lock);
	for(int i=0;i<nctxs;i++)if(ctxs[i]==id) {
		matches=ctx_source_context[i] && ctx_source_context[i]==binding->context;break;
	}
	pthread_mutex_unlock(&ctx_lock);
	return matches;
}

/* Records/descriptors outlive all workers. These positive monitor tokens are
 * allocated once per native context lifetime and never reused. Native names
 * stay inside the creation/proc adapters and do not key routing or storage. */
static int ctx_native_copy(pid_t pid,struct linux_execution_context *native)
{
	int found=0;
	pthread_mutex_lock(&ctx_lock);
	for(int i=0;i<nctxs;i++)if(ctxs[i]==pid) {
		*native=ctx_native[i];found=1;break;
	}
	pthread_mutex_unlock(&ctx_lock);
	if(!found)errno=ESRCH;
	return found;
}

struct execution_native_call {
	struct linux_execution_context native;
	unsigned command;
	void *argument;
};
static long execution_native_invoke(void *opaque)
{
	struct execution_native_call *call=opaque;
	return linux_execution_control(&call->native,call->command,call->argument);
}
static long execution_context_ctl(memory_target target,unsigned command,void *argument)
{
	if(!target)return execution_control_raw(0,command,argument);
	struct execution_native_call call={.command=command,.argument=argument};
	if(!ctx_native_copy(ctx_id(target),&call.native))return -1;
	/* WAIT is a lifetime observation and can block until source work ends.
	 * It must not retain a memory binding guard across that wait. */
	if(command==VMCTX_CTL_WAIT)return execution_native_invoke(&call);
	return execution_target_control(target,execution_native_invoke,&call);
}

/* Own an unreaped native child before allocating its protocol token. Backing
 * metadata becomes visible before membership, so a source request or snapshot
 * cannot find a published context without its object. Duplicate native context
 * identity returns the original token; PID reuse creates a distinct token. */
static execution_id ctx_register(pid_t native_pid,struct execution_mm *mm,
		const struct vmr_mm_binding *source,int *created)
{
	struct linux_execution_context native;
	*created=0;
	if(!execution_target_matches_mm(source,mm)) {errno=EPROTO;return -1;}
	if(linux_execution_open_child(native_pid,&native))return -1;
	struct linux_execution_object object={.control=initial_object_control,.opaque=&native};
	uint64_t native_epoch;
	if(linux_execution_object_verify(&object,mm->backing_fd,&native_epoch)) {
		int saved=errno;linux_execution_close(&native);errno=saved;return -1;
	}
	execution_id id=-1;
	pthread_mutex_lock(&ctx_lock);
	for(int i=0;i<nctxs;i++)if(ctx_native[i].identity==native.identity) {
		if(ctx_source_context[i]!=source->context || initial_backing_find(ctxs[i])!=mm->backing_fd)
			errno=EPROTO;
		else id=ctxs[i];
		goto done;
	}
	/* Source identities are lifetime names, including after native exit. Page
	 * callbacks resolve them without a native PID and must find one context. */
	for(int i=0;i<nctxs;i++)if(ctx_source_context[i]==source->context) {
		errno=EEXIST;goto done;
	}
	if(nctxs==MAX_CTX) {errno=ENOSPC;goto done;}
	id=nctxs+1;
	if(execution_context_binding_init(&ctx_memory[nctxs],id,mm,source,native_epoch)) {
		id=-1;goto done;
	}
	backing_note(id,mm->backing_fd);
	ctx_source_context[nctxs]=source->context;
	ctx_native[nctxs]=native;ctxs[nctxs++]=id;
	native=(struct linux_execution_context){.fd=-1,.exit_fd=-1};
	*created=1;
 done:
	pthread_mutex_unlock(&ctx_lock);
	int saved=errno;
	linux_execution_close(&native);
	if(id>0 && !*created && ctx_bind_source(id,source)) {id=-1;saved=errno;}
	if(id>0)map_trace("executor","initial-binding executor=%d source=%llu mm=%llu parent=%llu source_epoch=%llu native_epoch=%llu",
		id,(unsigned long long)source->context,(unsigned long long)source->mm,
		(unsigned long long)source->parent_mm,(unsigned long long)source->epoch,
		(unsigned long long)native_epoch);
	errno=saved;return id;
}


struct execution_proc_read {
	struct linux_execution_context native;
	uint64_t address,*word;
};
static long execution_pagemap_invoke(void *opaque)
{
	struct execution_proc_read *read=opaque;
	return linux_execution_pagemap_read(&read->native,read->address,read->word);
}
static int ctx_pagemap_read(memory_target target,uint64_t address,uint64_t *word)
{
	struct execution_proc_read read={.address=address,.word=word};
	if(!ctx_native_copy(ctx_id(target),&read.native))return -1;
	return (int)execution_target_control(target,execution_pagemap_invoke,&read);
}
static long execution_maps_invoke(void *opaque)
{return linux_execution_maps_snapshot(opaque);}
static FILE *ctx_maps_open(memory_target target)
{
	struct linux_execution_context native;
	if(!ctx_native_copy(ctx_id(target),&native))return NULL;
	int fd=(int)execution_target_control(target,execution_maps_invoke,&native);
	if(fd<0)return NULL;
	FILE *file=fdopen(fd,"r");
	if(!file) {int saved=errno;close(fd);errno=saved;}
	return file;
}

static int ctx_signal(pid_t pid,int sig)
{
	struct linux_execution_context native;
	if(!ctx_native_copy(pid,&native))return -1;
	return linux_execution_signal(&native,sig);
}

static void ctx_stop_all(void)
{
	struct linux_execution_context native[MAX_CTX];
	pthread_mutex_lock(&ctx_lock);
	int count=nctxs;
	memcpy(native,ctx_native,count*sizeof(*native));
	pthread_mutex_unlock(&ctx_lock);
	for(int i=0;i<count;i++)(void)linux_execution_signal(&native[i],SIGKILL);
}

/* Backend teardown is not native process exit. A retained pidfd remains an
 * exact observation even after reaping or reuse of the process's old name. */
static int ctx_execution_ended(pid_t pid,int publish)
{
	struct linux_execution_context native={.fd=-1,.exit_fd=-1};
	int ended=0;
	pthread_mutex_lock(&ctx_lock);
	for(int i=0;i<nctxs;i++)if(ctxs[i]==pid) {
		if(publish)ctx_reaped[i]=1;
		ended=ctx_reaped[i];native=ctx_native[i];break;
	}
	pthread_mutex_unlock(&ctx_lock);
	if(ended)return 1;
	if(native.exit_fd<0)return 0;
	return linux_execution_ended(&native,0)==1;
}

static int ctx_wait_execution_ended(pid_t pid)
{
	struct linux_execution_context native;
	return ctx_native_copy(pid,&native) && linux_execution_ended(&native,1000)==1;
}
static execution_id ctx_wait(execution_id id,int *status)
{
	struct linux_execution_context native;
	if(!ctx_native_copy(id,&native) || linux_execution_wait(&native,status))return -1;
	ctx_execution_ended(id,1);
	return id;
}
static pthread_mutex_t ctl_lock = PTHREAD_MUTEX_INITIALIZER;
static unsigned long n_pg_get, n_pg_put, n_pg_absent;
/* GETs answered "the token is in transit to you; ask again" -- see the
 * retained-copy branch in pg_serve_conn and VMR_PG_INFLIGHT. */
static unsigned long n_pg_inflight;
/*
 * The in-transit episode: consecutive GETs for one page that all find "the
 * asker holds it and nothing here can produce it". Begins at the first such
 * ask and ends at the LANDING, and the landing -- not a clock -- is the
 * discriminator: while this side's own fault service has a pull of the page
 * ACTIVE the answer is INFLIGHT (ask again), for as long as that takes;
 * with no pull active a young episode (1200ms grace) still answers
 * INFLIGHT; past the grace the retained copy is served. Every pure-clock
 * variant was measured wrong:
 *  - a 50ms cap lost to real crossings whose landing sat behind a 500ms
 *    pull_wait chain (40-41/48, the retained line in 6 of 7 failing runs);
 *  - a 1200ms cap still put 4 of 48 runs back on the stale copy;
 *  - NO cap took pg3 to 96/96 -- every pg3 crossing lands -- but hung sig1
 *    (killed at its 10s limit, all sub-tests already passed): the
 *    EVAPORATED-token case, where the bytes died with a context mid-service
 *    and no landing is ever coming, must eventually be answered with the
 *    retained copy, which for a dead token is the newest copy in existence.
 * The 1200ms grace is past two pull_waits and inside the asker's own 2s
 * fault deadline. Slots are approximate (64, hashed, unlocked): a collision
 * costs one mistimed fallback, never a hang.
 */
static struct { uint64_t pg, first_us; } infl_ans[64];

static void infl_ans_clear(uint64_t addr)
{
	unsigned sl = (addr >> 12) & 63;

	if (infl_ans[sl].pg == addr)
		infl_ans[sl].pg = 0;
}
static unsigned long n_pg_pulled;	/* pages pulled back from a shadow */
/*
 * ABSENT answers classified by what this side's own record said at the moment
 * it gave them, because that is what decides whether recording a loan for the
 * asking shadow is a fact or an inference. See page_lent_state().
 */
static unsigned long n_absent_unseen;	/* never held here: the start-up case  */
static unsigned long n_absent_was_guest;/* this side's record says the GUEST has
					 * it and this side cannot read it: a
					 * page lost on this side              */
static unsigned long n_absent_was_shadow;/* already out on loan: nothing new   */
/*
 * ABSENT answers from the source that were STALE AT ARRIVAL: between this
 * side's ask and the answer's arrival, this side's own page server handed the
 * page to the source (the lend record flipped to SHADOW), so "the source does
 * not hold it" stopped being true in flight. Believing one installs the
 * retained (old) bytes over the guest's live progress -- hx1's photographed
 * lost store. Answered by yielding and asking again; the source holds the
 * page now and the re-ask returns bytes.
 */
static unsigned long n_absent_stale_crossed;
/*
 * The source's fourth answer: "I handed that page to you." Counted apart from
 * ABSENT because the two license opposite things -- see VMR_CTXPAGE_NOTHOLDER.
 * Nothing is done differently on it; see where they are incremented for the
 * measurement that refuted doing anything.
 */
static unsigned long n_notholder;	/* the answer arrived                 */
static unsigned long n_notholder_landed;/* ...and a context here had the page */
static unsigned long n_notholder_lost;	/* ...and no context of this address
					 * space had it: the §5.1 pair        */
/* Pages written straight into the address space's object because no context
 * had mapped the address yet. */
static unsigned long n_put_to_object;
static unsigned long n_punch_fail;
/* Contexts that ended badly on their own, rather than with the guest. */
/* Replies that carried a new register set from the source. */
static unsigned long n_regs_from_home;
static unsigned long n_ctx_died;
/* contexts that ended while siblings of their address space were running */
static unsigned long n_end_with_live_siblings;
static int worst_ctx_status;
/*
 * Contexts this side ENDED on the source's word that their process had died
 * there (vmr_rsp.ended): the status the source reported, so the end is
 * reported home as the program's own death and never counted as a context
 * that died on this side (worst_ctx_status).
 */
static struct { pid_t pid; unsigned st; } src_ended[MAX_CTX];
static pthread_mutex_t src_ended_lock = PTHREAD_MUTEX_INITIALIZER;
static unsigned long n_src_ended, n_src_ended_ctx;

static void src_ended_note(pid_t pid, unsigned st)
{
	int i;

	pthread_mutex_lock(&src_ended_lock);
	for (i = 0; i < MAX_CTX; i++)
		if (src_ended[i].pid == pid || src_ended[i].pid == 0) {
			src_ended[i].pid = pid;
			src_ended[i].st = st;
			break;
		}
	pthread_mutex_unlock(&src_ended_lock);
}

static int src_ended_get(pid_t pid, unsigned *st)
{
	int i, r = 0;

	pthread_mutex_lock(&src_ended_lock);
	for (i = 0; i < MAX_CTX; i++)
		if (src_ended[i].pid == pid) {
			*st = src_ended[i].st;
			r = 1;
			break;
		}
	pthread_mutex_unlock(&src_ended_lock);
	return r;
}
static unsigned long n_declined, n_decl_prot, n_child_faults, n_refilled;
/* Which kind of protection fault was declined -- see fault_from_home(). */
static unsigned long n_decl_prot_free, n_decl_prot_whole, n_decl_prot_stuck;
/*
 * Protection faults the owner's kernel ruled LEGAL (VMR_EXC_LEGAL): a page
 * write-protected in this context's page table by a hand-over and never
 * re-granted here, because the sibling that pulled it back re-granted only
 * its own mapping. Granted in place; never a signal, never a decline.
 */
static unsigned long n_prot_legal_granted, n_prot_legal_owner, n_prot_legal_nogrant;
/*
 * Protection faults on a page that has been taken away since the guest took
 * them: the fault describes a mapping that no longer exists, so it is retried
 * rather than declined to the local kernel.
 */
static unsigned long n_decl_prot_stale;
static unsigned long n_decl_prot_lent;
static unsigned long n_pg_prefill;
/* ... of which the page could not be fetched before the partial write. */
static unsigned long n_pg_prefill_fail;
static int pvw_warned;
static int stack_dumped;

/*
 * A page that becomes present without anyone installing content is the bug we
 * are chasing: the guest then reads zeros and calls them code. VMREMOTE_WATCH
 * names one address, and every transition of that page is logged with what the
 * context was doing at the time, which identifies who materialised it.
 */
static uint64_t watch_addr;
static uint64_t watch_word, watch_val, last_ev_rip;
static int watch_was;

/*
 * Why is this address faulting over and over? Every party to it disagrees at
 * that point, so print what each of them actually says rather than what the
 * code believed: the raw pagemap word read through a *fresh* descriptor (the
 * cached one is a suspect), the VMA the address falls in, and whether the bytes
 * can be read at all. "The kernel faults on a page our own pagemap read calls
 * present" is a sentence no counter can produce.
 */
static int page_is_installed(memory_target pid, uint64_t page);
static int task_alive(memory_target pid);
static int shared_range_has(memory_target pid, uint64_t addr);
static off_t shared_obj_off(memory_target pid, uint64_t addr);
static int shared_obj_has(off_t off);
/* Shared pages served out of the object after the context that held them ended. */
static unsigned long n_shared_after_exit;
static long peek_at(memory_target pid, uint64_t addr, void *buf, uint64_t len,
		    const char *who);
static int pg_rw(int fd, void *buf, size_t n, int wr);

/* ---- single-writer coherence: who is holding a page ---------------------- */

/*
 * Coherence moves a page instead of copying it: when the shadow needs one it is
 * taken out of the guest, so there is one copy and one writer and no write set
 * to reconstruct afterwards. Two things are needed to make that work, and this
 * is the bookkeeping for both — which pages have gone, and to which shadow, so
 * the guest's next fault on one can ask for it back.
 */

/*
 * The page log's switches, declared here rather than beside pglog_init()
 * because the ownership record's own transitions are traced (page_lend_at(),
 * far above it) and a record that is only readable at the moment it is used is
 * exactly what made AUDIT XI §5.1 unreadable.
 *
 * VMR_PGLOG=1 for every page, VMR_PGLOG=0xADDR for one.
 */
static int      pglog_on;
static uint64_t pglog_page;

#define LENT_SLOTS (1u << 17)
struct loan {
	struct loan *next;
	uint64_t owner;		/* the shadow's pid; 0 once settled        */
	int      shared;	/* the guest kept a read-only copy too     */
	/*
	 * This side write-protected the guest's copy in order to see its next
	 * write, and nobody holds the page. It is not a loan and must not be
	 * recorded as one -- see the note in fault_from_home_mode()'s read
	 * branch, and the measurement that made writing "the source holds it
	 * shared" here the only way branch 6 could act on the fault at all.
	 */
	int      watched;
	uint64_t page;
	uint64_t as;
};
/* Buckets are an index, not a capacity limit. Full keys and chained records
 * preserve collisions, and removing a mapping cannot hide another loan. */
static struct loan *lent[LENT_SLOTS];
static pthread_mutex_t lent_lock = PTHREAD_MUTEX_INITIALIZER;
static unsigned long n_lent_vacated;
static size_t lent_live, lent_peak;

static unsigned lent_slot(uint64_t as, uint64_t page)
{
	return (unsigned)(((page >> 12) * 2654435761u) ^
			  ((as ^ (as >> 32)) * UINT64_C(2246822519))) % LENT_SLOTS;
}

/* Call with lent_lock held; no pointer may escape that critical section. */
static struct loan *loan_find(uint64_t as, uint64_t page, int create)
{
	struct loan *entry;
	unsigned slot;

	page &= ~UINT64_C(4095);
	slot = lent_slot(as, page);
	for (entry = lent[slot]; entry; entry = entry->next)
		if (entry->as == as && entry->page == page)
			return entry;
	if (!create)
		return NULL;
	entry = calloc(1, sizeof(*entry));
	if (!entry) {
		fputs("vmctx: cannot record a page loan; refusing to continue untracked\n", stderr);
		abort();
	}
	entry->as = as;
	entry->page = page;
	entry->next = lent[slot];
	lent[slot] = entry;
	if (++lent_live > lent_peak) {
		lent_peak = lent_live;
		if (lent_peak >= 8192 && !(lent_peak & (lent_peak - 1)))
			fprintf(stderr, "vmctx: loan table live=%zu record_bytes=%zu buckets=%u\n",
				lent_live, lent_live * sizeof(*entry), LENT_SLOTS);
	}
	return entry;
}

/* A scan that drops lent_lock must use copied keys, never live node pointers.
 * This snapshots bookkeeping only; callers must still acquire page ownership
 * before acting on it. as == 0 selects all address spaces for diagnostics. */
static struct loan *loan_snapshot(uint64_t as, size_t *count)
{
	struct loan *rows;
	size_t n = 0;

	pthread_mutex_lock(&lent_lock);
	for (unsigned i = 0; i < LENT_SLOTS; i++)
		for (struct loan *e = lent[i]; e; e = e->next)
			if (e->owner && (!as || e->as == as))
				n++;
	rows = n ? calloc(n, sizeof(*rows)) : NULL;
	if (n && !rows) {
		fputs("vmctx: cannot snapshot page loans; refusing an incomplete scan\n", stderr);
		abort();
	}
	n = 0;
	for (unsigned i = 0; i < LENT_SLOTS; i++)
		for (struct loan *e = lent[i]; e; e = e->next)
			if (e->owner && (!as || e->as == as)) {
				rows[n] = *e;
				rows[n++].next = NULL;
			}
	pthread_mutex_unlock(&lent_lock);
	*count = n;
	return rows;
}

/*
 * A vacate must clear the LOANS in its range too. lent[] is keyed by address
 * and was never cleared on unmap -- the documented trap ("lent[] is stale:
 * ask SEEK_DATA not the record") -- and the session-37 thread-list corpse is
 * what it costs when the range recycles: a V8 worker's stack is unmapped, a
 * new worker's stack lands in the hole, its write fault consults the OLD
 * tenant's loan ("lent to shadow N"), the recall dials a channel that died
 * with that shadow, and the miss zero-fills the LIVE stack -- SIGABRT one
 * frame later. The vacate is the one moment the record is knowably dead.
 */
static void lend_forget_range(uint64_t as, uint64_t start, uint64_t end)
{
	unsigned i;

	pthread_mutex_lock(&lent_lock);
	for (i = 0; i < LENT_SLOTS; i++) {
		struct loan **link = &lent[i], *entry;

		while ((entry = *link)) {
			if (entry->as == as && entry->page >= start && entry->page < end) {
				*link = entry->next;
				free(entry);
				lent_live--;
				n_lent_vacated++;
			} else {
				link = &entry->next;
			}
		}
	}
	pthread_mutex_unlock(&lent_lock);
}

/*
 * Retained page bytes -- this side's memory of every page it has EVER
 * transmitted, so such a page is never zero-filled.
 *
 * A pull and a serve of one page cross: the page is punched out of this side
 * on the way to the source while a writer here faults for it back, and for an
 * instant the page is nowhere -- punched here, still being claimed there. The
 * old code, finding that hole, invented a zero page over live data. That is
 * the hx2 lost write (the value sentinel pinned it to site8), and it is
 * forbidden outright: a page the protocol has ever moved belongs to the remote
 * and has real last-known bytes; only a page it has NEVER moved may be zeroed
 * once, as a genuine first touch of anonymous memory would be.
 *
 * This table is exactly that distinction, in the owner's own words: the first
 * time a page is asked, mark it "initialized and belongs to remote" and
 * remember its bytes; never invent it thereafter; forget it only on munmap.
 *
 * It is a safety net UNDER the coherence protocol, not a second copy of it. It
 * is read ONLY at the sites that would otherwise write zeros -- never by
 * page_read_as() and never by any serve -- so it can never make a stale page
 * look authoritative to the source or to a normal fault. A page faults on this
 * side only after it was served away, and a serve captures the page's current
 * bytes into this table, so the retained copy at fault time is the page's last
 * real contents (every writer's slot included), not a guess. Filled on
 * serve-out (get_send) and on install-from-source; consulted at the zero
 * install sites; forgotten on munmap.
 */
static uint64_t as_id(memory_target pid);		/* the address-space leader; defined below */

/*
 * ---------------------------------------------------------------------------
 * THE TAKE GENERATION: a per-(address space, page) count of this side's
 * captures of the page, and the one fact a copy needs to carry to be judged.
 *
 * Every copy of a page that leaves this side -- a take away to the source, a
 * take-and-put-back, the retained copy -- is stamped with the count as it
 * stood when the bytes were captured, and every write-grant bumps it too (the
 * guest is about to make every copy older). A copy that comes BACK (the
 * source's CTXPAGE answer) carries its stamp; a stamp below the current count
 * descends from an older capture than one this side has made since, so it is
 * stale by construction -- whichever of the thirteen paths delivered it.
 *
 * Why a count and not a record: serve_object_mapped() maps the object's folio
 * legitimately in two cases (a sibling holds it live; the shmat sole-holder
 * case, where refusing killed netsurf 4.9s in) and stalely in one (an arena
 * page taken to the source and re-served under churn), and neither
 * backing_has() nor as_any_present() can tell the three apart -- the arena
 * content-history (VMR_HIST_ARENA) photographed it handing back glibc-arena
 * words one write behind at site80 AND the source's pull doing the same at
 * site31. The bytes were captured by a take and reverted in storage; only
 * their age says which copy is current. This is the age, and it is the
 * destination's clock alone: the source never bumps it, it only echoes it.
 *
 * Reset with the mapping (range_vacated): a fresh mapping at the
 * same address starts at 0 on both sides, as pgown_forget_range does for the
 * source's record.
 * ---------------------------------------------------------------------------
 */
#define PGEN_SLOTS (1u << 16)
struct pgen_ent {
	uint64_t page;
	uint64_t as;
	uint32_t gen;
};
static struct pgen_ent pgen_tab[PGEN_SLOTS];
static pthread_mutex_t pgen_lock = PTHREAD_MUTEX_INITIALIZER;
static unsigned long n_pgen_full, n_pgen_bumps;
/* The check's outcomes; see pull_gen_check(). */
static unsigned long n_gen_checked, n_gen_stale, n_gen_repaired, n_gen_repaired_obj;
static unsigned long n_gen_unrepaired, n_gen_ahead, n_gen_stale_site[128];
static unsigned long n_retained_served_old;

/* Caller holds pgen_lock. */
static struct pgen_ent *pgen_find(uint64_t as, uint64_t page, int create)
{
	uint64_t h = (as * 1099511628211ULL) ^
		     ((page >> 12) * 14695981039346656037ULL);
	unsigned i;

	h ^= h >> 29;
	for (i = 0; i < PGEN_SLOTS; i++) {
		struct pgen_ent *e = &pgen_tab[(h + i) % PGEN_SLOTS];

		if (e->page == page && e->as == as)
			return e;
		if (!e->page) {
			if (!create)
				return NULL;
			e->page = page;
			e->as = as;
			e->gen = 0;
			return e;
		}
	}
	if (create)
		n_pgen_full++;
	return NULL;
}

static uint32_t pgen_get(memory_target ctx, uint64_t addr)
{
	uint64_t page = addr & ~(uint64_t)(VMR_PG_SIZE - 1);
	uint64_t as = as_id(ctx);
	struct pgen_ent *e;
	uint32_t g = 0;

	pthread_mutex_lock(&pgen_lock);
	e = pgen_find(as, page, 0);
	if (e)
		g = e->gen;
	pthread_mutex_unlock(&pgen_lock);
	return g;
}

/* A capture of the page happened: the count moves, and every older copy of
 * the page -- wherever it is -- is now stale. Returns the new count. */
static uint32_t pgen_bump(memory_target ctx, uint64_t addr)
{
	uint64_t page = addr & ~(uint64_t)(VMR_PG_SIZE - 1);
	uint64_t as = as_id(ctx);
	struct pgen_ent *e;
	uint32_t g = 0;

	pthread_mutex_lock(&pgen_lock);
	e = pgen_find(as, page, 1);
	if (e)
		g = ++e->gen;
	n_pgen_bumps++;
	pthread_mutex_unlock(&pgen_lock);
	return g;
}

/* The mapping is gone (or replaced): the page at this address is new memory
 * with no history. Walks the table whole, like pgown_forget_range. */
static void pgen_forget(memory_target ctx, uint64_t start, uint64_t end)
{
	uint64_t as = as_id(ctx);
	unsigned i;

	pthread_mutex_lock(&pgen_lock);
	for (i = 0; i < PGEN_SLOTS; i++) {
		struct pgen_ent *e = &pgen_tab[i];

		if (e->page && e->as == as && e->page >= start && e->page < end)
			e->gen = 0;
	}
	pthread_mutex_unlock(&pgen_lock);
}

/*
 * The trail: the last few thousand hand-over events of every page, so that
 * the FIRST stale serve can be read back to its producer instead of being
 * re-photographed with an armed page. One slot per event, a spinning index,
 * no print on the hot path; dumped per page when a generation check fails.
 */
#define TRAIL_SLOTS 16384
enum {
	TR_TAKE_AWAY = 1,	/* captured, leaving for the source (aux: punch) */
	TR_TAKE_PUT,		/* captured to be put straight back              */
	TR_GRANT,		/* write granted in place (MAPOBJ)               */
	TR_SERVE_GET,		/* bytes put on the wire for a GET/GETS (aux: shared) */
	TR_SERVE_RETAINED,	/* the retained copy put on the wire             */
	TR_SERVE_ABSENT,	/* GET answered ABSENT                           */
	TR_SERVE_INFLIGHT,	/* GET answered INFLIGHT                         */
	TR_PULL_BYTES,		/* CTXPAGE bytes arrived (aux: source's gen)     */
	TR_PULL_STALE,		/* ...and they were older than our count         */
	TR_PULL_REPAIRED,	/* ...replaced by the retained copy of gen=count */
	TR_OBJMAP,		/* serve_object_mapped mapped the object's folio */
	TR_INSTALL_RETAINED,	/* retained copy installed (aux: fill site)      */
	TR_INSTALL_ZERO,	/* a fresh page of zeros installed               */
	TR_VACATE,		/* range vacated (aux: soft)                     */
	TR_INST_HELD,		/* chunk install skipped: held elsewhere         */
	TR_INST_PRESENT,	/* chunk install skipped: already present        */
	TR_INST_NOTMINE,	/* chunk install skipped: installed, not absent  */
	TR_INST_CHUNK,		/* chunk install wrote it (process_vm_writev)    */
	TR_INST_POKE,		/* chunk install wrote it (per-page POKE)        */
	TR_INST_NOREGION,	/* the no-region install wrote it (aux: fill site) */
	TR_INST_DECLINED,	/* bytes went to the object; fault declined      */
	TR_SERVE_NOTAKE,	/* GET: the take found nothing after the read; what
				 * went on the wire is the PRE-WAIT read (aux: pull
				 * active)                                       */
	TR_PUT_LANDED,		/* PUT/PUTN/PUTV from the source wrote it (aux: op) */
	TR_TAKEPUT_POKE,	/* make-writable's put-back poke wrote it        */
};
static const char *trail_name(int k)
{
	switch (k) {
	case TR_TAKE_AWAY:       return "TAKE-AWAY";
	case TR_TAKE_PUT:        return "TAKE-PUT-BACK";
	case TR_GRANT:           return "WRITE-GRANT";
	case TR_SERVE_GET:       return "SERVE-GET";
	case TR_SERVE_RETAINED:  return "SERVE-RETAINED";
	case TR_SERVE_ABSENT:    return "SERVE-ABSENT";
	case TR_SERVE_INFLIGHT:  return "SERVE-INFLIGHT";
	case TR_PULL_BYTES:      return "PULL-BYTES";
	case TR_PULL_STALE:      return "PULL-STALE";
	case TR_PULL_REPAIRED:   return "PULL-REPAIRED";
	case TR_OBJMAP:          return "OBJECT-MAPPED";
	case TR_INSTALL_RETAINED:return "INSTALL-RETAINED";
	case TR_INSTALL_ZERO:    return "INSTALL-ZERO";
	case TR_VACATE:          return "VACATE";
	case TR_INST_HELD:       return "INST-SKIP-HELD";
	case TR_INST_PRESENT:    return "INST-SKIP-PRESENT";
	case TR_INST_NOTMINE:    return "INST-SKIP-NOTMINE";
	case TR_INST_CHUNK:      return "INST-CHUNK";
	case TR_INST_POKE:       return "INST-POKE";
	case TR_INST_NOREGION:   return "INST-NOREGION";
	case TR_INST_DECLINED:   return "INST-DECLINED";
	case TR_SERVE_NOTAKE:    return "SERVE-GET-NOTAKE";
	case TR_PUT_LANDED:      return "PUT-LANDED";
	case TR_TAKEPUT_POKE:    return "TAKEPUT-POKE";
	}
	return "?";
}
struct trail_ent {
	uint64_t us;
	uint64_t page;
	uint64_t as;
	int      kind;
	uint32_t gen;
	uint32_t aux;
	uint32_t sum;
	uint32_t word;
	int      word_valid;
	pid_t    tid;
};
static unsigned trail_word_offset = 4096;
static struct trail_ent trail_default[TRAIL_SLOTS];
static struct trail_ent *trail = trail_default;
static unsigned long trail_slots = TRAIL_SLOTS;
static unsigned long trail_n;
static pthread_mutex_t trail_lock = PTHREAD_MUTEX_INITIALIZER;

static void trail_note(memory_target ctx, uint64_t addr, int kind, uint32_t gen,
		       uint32_t aux, const void *bytes)
{
	pthread_mutex_lock(&trail_lock);
	unsigned long i = trail_n++;
	struct trail_ent *t = &trail[i % trail_slots];

	t->us = now_us();
	t->page = addr & ~(uint64_t)(VMR_PG_SIZE - 1);
	t->as = as_id(ctx);
	t->kind = kind;
	t->gen = gen;
	t->aux = aux;
	t->sum = bytes ? page_sum(bytes) : 0;
	t->word_valid = bytes && trail_word_offset <= VMR_PG_SIZE - sizeof(t->word);
	t->word = 0;
	if (t->word_valid) memcpy(&t->word, (const char *)bytes + trail_word_offset, sizeof(t->word));
	t->tid = (pid_t)own_tid();
	pthread_mutex_unlock(&trail_lock);
}

static void trail_dump(memory_target ctx, uint64_t addr)
{
	uint64_t page = addr & ~(uint64_t)(VMR_PG_SIZE - 1);
	uint64_t as = as_id(ctx);
	pthread_mutex_lock(&trail_lock);
	unsigned long end = trail_n;
	unsigned long i, n = 0;

	fprintf(stderr, "[vmremote] TRAIL of 0x%llx (as %d), oldest first:\n",
		(unsigned long long)page, (int)as);
	for (i = end > trail_slots ? end - trail_slots : 0; i < end; i++) {
		struct trail_ent *t = &trail[i % trail_slots];

		if (t->page != page || t->as != as)
			continue;
		fprintf(stderr, "[vmremote]   abs=%llu tid=%d %-16s gen=%u "
			"aux=%u sum=%08x word_valid=%d word=%u\n", (unsigned long long)t->us,
			(int)t->tid, trail_name(t->kind), t->gen, t->aux,
			t->sum, t->word_valid, t->word);
		n++;
	}
	if (!n)
		fprintf(stderr, "[vmremote]   (no events in the ring)\n");
	pthread_mutex_unlock(&trail_lock);
}

/*
 * The whole ring for one page, every address space, at the end of a run --
 * armed by VMR_TRAIL_PAGE=<addr>. A guest that records WHEN its store was
 * lost (tests/hx2 prints abs=... on this clock) is read against this: the
 * event at that instant is the install that reverted the page. No print on
 * the hot path; the ring is already kept.
 */
static void trail_dump_page(uint64_t addr)
{
	uint64_t page = addr & ~(uint64_t)(VMR_PG_SIZE - 1);
	pthread_mutex_lock(&trail_lock);
	unsigned long end = trail_n;
	unsigned long i, n = 0;

	fprintf(stderr, "[vmremote] TRAIL (all address spaces) of 0x%llx, "
		"oldest first:\n", (unsigned long long)page);
	for (i = end > trail_slots ? end - trail_slots : 0; i < end; i++) {
		struct trail_ent *t = &trail[i % trail_slots];

		if (t->page != page)
			continue;
		fprintf(stderr, "[vmremote]   abs=%llu as=%d tid=%d %-16s gen=%u "
			"aux=%u sum=%08x word_valid=%d word=%u\n", (unsigned long long)t->us,
			(int)t->as, (int)t->tid, trail_name(t->kind), t->gen,
			t->aux, t->sum, t->word_valid, t->word);
		n++;
	}
	if (!n)
		fprintf(stderr, "[vmremote]   (no events in the ring)\n");
	pthread_mutex_unlock(&trail_lock);
}

#define RETAIN_SLOTS (1u << 15)		/* 32768 pages; lazily allocated (~135 MB) */
struct retain_ent {
	uint64_t page;			/* page base, or 0 in an unused slot */
	uint64_t as;
	int      live;			/* 1 = holds bytes; 0 = tombstone (munmap'd) */
	uint32_t gen;			/* the take generation of `bytes` */
	char     bytes[VMR_PG_SIZE];
};
static struct retain_ent *retain_tab;
static pthread_mutex_t retain_lock = PTHREAD_MUTEX_INITIALIZER;
static unsigned long n_retain_put, n_retain_hit, n_retain_miss, n_retain_full;

static struct retain_ent *retain_find(uint64_t as, uint64_t page, int create)
{
	uint64_t h = (as * 1099511628211ULL) ^
		     ((page >> 12) * 14695981039346656037ULL);
	struct retain_ent *tomb = NULL;
	unsigned i;

	if (!retain_tab) {
		if (!create)
			return NULL;
		retain_tab = calloc(RETAIN_SLOTS, sizeof(*retain_tab));
		if (!retain_tab)
			return NULL;
	}
	for (i = 0; i < RETAIN_SLOTS; i++) {
		struct retain_ent *e = &retain_tab[(h + i) % RETAIN_SLOTS];

		if (e->page == page && e->as == as && (e->live || e->page))
			return e;		/* the slot for this key (live or tomb) */
		if (!e->page && !e->live) {
			if (!create)
				return NULL;	/* unused: key is absent */
			e = tomb ? tomb : e;	/* prefer reusing a tombstone */
			e->page = page;
			e->as = as;
			return e;
		}
		if (create && !e->live && !tomb)
			tomb = e;		/* remember a tombstone to reuse */
	}
	if (create && tomb) {
		tomb->page = page;
		tomb->as = as;
		return tomb;
	}
	return NULL;			/* table full */
}

/*
 * Remember this page's current bytes: it has now been transmitted. Keyed by
 * the address space (not the faulting context) so that a sibling thread which
 * later faults on the same shared page finds it -- which is exactly hx2, where
 * many writer threads of one address space share the page.
 */
static void retain_put(memory_target ctx, uint64_t addr, const void *bytes)
{
	uint64_t page = addr & ~(uint64_t)(VMR_PG_SIZE - 1);
	uint64_t as = as_id(ctx);
	struct retain_ent *e;

	/* Stamped with the count as it stands: the bytes are a capture of
	 * the page at this generation (the take that produced them has
	 * already bumped it). */
	uint32_t gen = pgen_get(ctx, page);

	pthread_mutex_lock(&retain_lock);
	e = retain_find(as, page, 1);
	if (e) {
		memcpy(e->bytes, bytes, VMR_PG_SIZE);
		e->live = 1;
		e->gen = gen;
		n_retain_put++;
	} else {
		n_retain_full++;
	}
	pthread_mutex_unlock(&retain_lock);
}

/*
 * If this page was ever transmitted, copy its last-known bytes into `out` and
 * return 1 (belongs to the remote -- must never be zeroed). Return 0 only for
 * a page never moved, which alone may be given the kernel's zeros once.
 */
static int retain_get_gen(memory_target ctx, uint64_t addr, void *out, uint32_t *gen)
{
	uint64_t page = addr & ~(uint64_t)(VMR_PG_SIZE - 1);
	uint64_t as = as_id(ctx);
	struct retain_ent *e;
	int hit = 0;

	pthread_mutex_lock(&retain_lock);
	e = retain_find(as, page, 0);
	if (e && e->live) {
		if (out)
			memcpy(out, e->bytes, VMR_PG_SIZE);
		if (gen)
			*gen = e->gen;
		hit = 1;
		n_retain_hit++;
	} else {
		n_retain_miss++;
	}
	pthread_mutex_unlock(&retain_lock);
	return hit;
}

static int retain_get(memory_target ctx, uint64_t addr, void *out)
{
	return retain_get_gen(ctx, addr, out, NULL);
}

/*
 * Forget a range on munmap: the pages are gone, so their retained bytes must
 * not resurrect a later mapping at the same address. Tombstone, not erase, so
 * the open-addressing probe chains survive.
 */
static void retain_forget(memory_target ctx, uint64_t start, uint64_t end)
{
	uint64_t as = as_id(ctx);

	if (end <= start)
		return;
	start &= ~(uint64_t)(VMR_PG_SIZE - 1);
	pthread_mutex_lock(&retain_lock);
	/* Iterate stored entries, not virtual pages. Sparse reservations can
	 * span terabytes; walking that range under this lock stops all page
	 * service even when only a handful of bytes have ever been retained.
	 * The allocation check shares the lock with retain_find's publisher. */
	if (retain_tab) for (unsigned i = 0; i < RETAIN_SLOTS; i++) {
		struct retain_ent *e = &retain_tab[i];

		if (e->live && e->as == as && e->page >= start && e->page < end)
			e->live = 0;
	}
	pthread_mutex_unlock(&retain_lock);
}

static unsigned long n_lent, n_recalled, n_recall_absent;
/* GETs where the page left this side between the read and the take. */
static unsigned long n_get_lost_race;
static unsigned long n_get_landed_racing;	/* takes missed, then the page landed: INFLIGHT, not fatal */
/* GETs answered INFLIGHT because this side's own pull of the page was in
 * flight while nothing here could read it (the record saying the guest's). */
static unsigned long n_pg_inflight_pull;
/* ...of which this many were answered ABSENT (the page left for the asker
 * through the other channel) and this many INFLIGHT (a pull of it was landing
 * here). */
static unsigned long n_get_lost_race_absent, n_get_lost_race_inflight;
static unsigned long n_get_from_object;	/* GETs served from the object when no
					 * context mapped the page (shmat-class
					 * give-backs land only in the object) */
/* GETs for a page this side had already handed to the asker. */
static unsigned long n_get_already_theirs;
/*
 * ...and the ones where the record said so but a context of the guest's own
 * address space HAD the page present -- the guest pulled it back and is the
 * live writer, the loan record is stale, and the "your own copy is current"
 * override would have thrown away the guest's current bytes and served the
 * retained copy (stale by every write since the last capture). hx2's lost
 * store rode exactly that: the writer read a value tens of thousands of rounds
 * old. Suppressed: the present context's bytes are served.
 */
static unsigned long n_get_theirs_but_present;
/* The 64-probe bound in the mark paths, which give up without saying so. */
/* Branch 6 cleared the mark and then could not make the page writable. */
static unsigned long n_watch_grant_nowrite;
/* GET-shared: protected the guest's copy, then failed to read it. */
static unsigned long n_prot_noread;
/* Upgrades of a page this side does not agree the shadow holds shared. */
static unsigned long n_upgrade_stale;
/*
 * A protection fault on a page the source holds shared: how it was answered.
 *
 * n_prot_recall_dead counts the old branch, which asked with page_recall() --
 * an operation that cannot succeed, because the channel it needs is never
 * registered (see page_recall_op()). The rest count the pull that replaces it,
 * split by what the source said, because "the source did not hold it after all"
 * is the case the old branch silently assumed always held and is the one number
 * that says how often it was right.
 */
/*
 * Off, and measured to be right that way. The pull replaces a recall that
 * cannot fire, which sounds like a repair and is not one: measured on pg3,
 * eight runs per arm alternating, the pull answered "the source did not hold
 * it after all" in 100% of cases -- 26449 of 26449, 33216 of 33216, 79631 of
 * 79631 -- and not once came back with bytes. So the source was not a holder
 * at the moment of the fault, the recall's failure cost nothing, and the pull
 * costs a network round trip tens of thousands of times a run for an answer
 * that is always the same. The failure rate did not move either.
 *
 * What the counters did find is the defect underneath both: lent[] says the
 * source is holding this page shared, 23000 to 80000 times a run, and the
 * source says it is not. The counters below still say so; they are read on the
 * fault path and reported at exit.
 *
 * The two switches that used to stand here -- VMR_PROT_PULL and VMR_FAKE_SHARE
 * -- are gone. Both were assigned at start-up and READ NOWHERE, so neither ever
 * changed anything, and every arm this file quotes for them measured one
 * behaviour against itself; the "spread" between those arms is this box's
 * run-to-run noise and nothing else. A switch that does not switch is worse
 * than no switch: it launders noise into a measurement. If either question is
 * worth asking again, it gets asked with a flag that is actually read, and the
 * numbers get taken again from scratch.
 */
static unsigned long n_pull_not_shared, n_watch_grant;
static unsigned long n_prot_pull, n_prot_pull_bytes, n_prot_pull_absent;
static unsigned long n_prot_pull_busy, n_prot_recall_dead;
static unsigned long n_shared, n_upgraded, n_downgraded, n_unprotected;
static unsigned long n_stale;	/* loans of contexts that have finished */
static unsigned long n_clear_miss, n_double_lend;
/*
 * Members of an address space that would not give up a page, or would not stop
 * writing it, while the page was being handed to a shadow as the only copy.
 */
/*
 * Hand-overs done as one address-space-wide operation, ones where the object
 * simply did not have the page, and ones that had to fall back to the
 * per-context sequence. If the fallback count is large the atomic path is not
 * being used and the old windows are all still there, which is exactly the
 * thing a run should not be able to hide.
 */
static unsigned long n_takeobj, n_takeobj_absent, n_takeobj_fallback;
static unsigned long n_takeobj_busy;
static unsigned long n_take_from_object;
static unsigned long n_zero_take;	/* all-zero captures of a transmitted page */
#define VMR_ZERO_PAGE_SUM 0x04000001u	/* page_sum() of 4096 zero bytes (the
					 * kernel's VMCTX_ZERO_PAGE_SUM) */
static unsigned long n_take_partial, n_protect_partial;

/*
 * One page may only change sides once at a time.
 *
 * Both machines want the same page at the same moment — the guest faults on it
 * while the shadow is asking for it — and each transition is several steps: ask
 * who holds it, move it, record the move. Interleaved, two of those produce a
 * page that is recorded as held by a shadow and present in the guest at once,
 * or absent from both. The second is what a guest sees as a fault it takes for
 * ever: the fault path finds the page lent, so it declines to install it, and
 * the loan it is deferring to was created after it had already recalled the
 * page. Recursive because the page service resolves faults of its own.
 */
static pthread_mutex_t coh_lock;

static void coh_lock_init(void)
{
	pthread_mutexattr_t at;

	pthread_mutexattr_init(&at);
	pthread_mutexattr_settype(&at, PTHREAD_MUTEX_RECURSIVE);
	pthread_mutex_init(&coh_lock, &at);
}

static __thread int coh_depth;
static __thread uint64_t coh_t0;	/* when the outermost enter took it   */
static __thread const char *coh_where;	/* what claimed it, for the SLOW note */

/*
 * The SLOW note below fires at RELEASE, which names the holder only after the
 * damage -- a fault the guest is taking right now goes unanswered behind this
 * lock, and by the time the holder lets go the kernel has killed the context.
 * These are the same facts published for a watcher thread (coh_watch_thread)
 * that can photograph the holder WHILE it blocks: its kernel stack and the
 * syscall it is sitting in, which is the one thing the release-time note can
 * never say. Written only at the outermost enter/leave, torn reads can only
 * produce a stale candidate, and the watcher re-reads before it prints.
 */
static volatile uint64_t coh_g_t0;	/* 0 = not held */
static volatile pid_t coh_g_tid;
static const char *volatile coh_g_where;

/*
 * A lock held longer than a hand-over needs is the bug, so say so the instant it
 * is released -- with who held it and for how long. 20ms is orders of magnitude
 * more than any local step (a take is microseconds, a poke likewise); the only
 * way to exceed it is to have held coh_lock across something that blocks, which
 * is exactly the design error this project is chasing.
 */
#define VMR_COH_SLOW_US 20000

static void coh_enter_at(const char *where)
{
	pthread_mutex_lock(&coh_lock);
	if (++coh_depth == 1) {
		coh_t0 = now_us();
		coh_where = where;
		coh_g_tid = (pid_t)own_tid();
		coh_g_where = where;
		coh_g_t0 = coh_t0;
	}
}
#define coh_enter() coh_enter_at(__func__)

static void coh_leave(void)
{
	if (coh_depth == 1) {
		uint64_t held = now_us() - coh_t0;

		coh_g_t0 = 0;
		if (held > VMR_COH_SLOW_US)
			fprintf(stderr, "[vmremote] SLOW coh_lock: held "
				"%lluus by %s (tid %d) -- a coherence lock "
				"held this long spans a blocking step; that "
				"is the locking defect\n",
				(unsigned long long)held,
				coh_where ? coh_where : "?",
				(int)own_tid());
	}
	coh_depth--;
	pthread_mutex_unlock(&coh_lock);
}

/*
 * Fully release coh_lock for a blocking op (a remote pull), however deeply this
 * thread has entered it, and return the depth for coh_restore(). A plain
 * coh_leave() drops only ONE level of the recursive lock -- so a pull nested
 * inside the page server (which, holding the lock, resolves faults of its own
 * via fault_from_home_mode) would run with the lock still held: the exact
 * deadlock pull_begin() exists to avoid (home GETs the same page and queues
 * behind the lock this thread holds), whose 1s-bound fallback forks the page.
 * Dropping ALL levels lets the other direction in; coh_restore() takes them back.
 */
static int coh_drop_all(void)
{
	int d = coh_depth;

	if (d > 0)
		coh_g_t0 = 0;
	while (coh_depth > 0) {
		coh_depth--;
		pthread_mutex_unlock(&coh_lock);
	}
	return d;
}

static void coh_restore(int d)
{
	while (d-- > 0) {
		pthread_mutex_lock(&coh_lock);
		if (++coh_depth == 1) {
			coh_t0 = now_us();	/* the pull is not held time */
			coh_where = "after-pull";
			coh_g_tid = (pid_t)own_tid();
			coh_g_where = "after-pull";
			coh_g_t0 = coh_t0;
		}
	}
}

/*
 * Photograph the coherence lock's holder while it is still blocking, once per
 * holding episode, capped. /proc/self/task/<tid>/stack is the kernel stack of
 * a thread of THIS process (vmremote runs as root under the harness), and
 * /proc/self/task/<tid>/syscall names the call it sleeps in with its
 * arguments; between them they name the blocking step directly, which the
 * release-time SLOW note structurally cannot. The watcher only reads and only
 * when a hold exceeds a second, so it costs nothing on a healthy run.
 */
static void *coh_watch_thread(void *unused)
{
	uint64_t last_dumped = 0;
	int dumps = 0;

	(void)unused;
	for (;;) {
		uint64_t t0 = coh_g_t0;

		usleep(100000);
		if (!t0 || t0 == last_dumped || dumps >= 8)
			continue;
		if (coh_g_t0 != t0 || now_us() - t0 < 1000000)
			continue;
		last_dumped = t0;
		dumps++;
		{
			pid_t tid = coh_g_tid;
			const char *w = coh_g_where;
			char path[64], buf[2048];
			int fd, n;

			fprintf(stderr, "[vmremote] COH-HELD %llums by %s "
				"(tid %d) and still held -- the holder now:\n",
				(unsigned long long)(now_us() - t0) / 1000,
				w ? w : "?", (int)tid);
			snprintf(path, sizeof(path),
				 "/proc/self/task/%d/syscall", (int)tid);
			fd = open(path, O_RDONLY);
			if (fd >= 0) {
				n = read(fd, buf, sizeof(buf) - 1);
				close(fd);
				if (n > 0) {
					buf[n] = 0;
					fprintf(stderr, "  syscall: %s", buf);
				}
			}
			snprintf(path, sizeof(path),
				 "/proc/self/task/%d/stack", (int)tid);
			fd = open(path, O_RDONLY);
			if (fd >= 0) {
				n = read(fd, buf, sizeof(buf) - 1);
				close(fd);
				if (n > 0) {
					buf[n] = 0;
					fprintf(stderr, "%s", buf);
				}
			}
		}
	}
	return NULL;
}

/*
 * Which address space a context belongs to.
 *
 * A page is held by an address space, not by a context: a guest thread is a
 * context of its own but shares its process's memory, so a page taken while
 * serving one thread is missing for all of them. Keyed by context, the record
 * was invisible to every sibling — they faulted, found nothing lent, and had
 * the page refilled from home's oracle behind the shadow's back. Two copies,
 * both live, and the one the syscalls were writing lost. th9 (two contexts
 * making forwarded syscalls at once) fails within a dozen lines that way, while
 * a single-threaded guest never notices.
 *
 * This used to be answered out of /proc, by Tgid, on the grounds that Linux
 * threads share one. That stopped being true the moment a thread context began
 * to exist because the source asked for one: the monitor makes it with
 * clone(..., SIGCHLD), so it is a process of its own and its Tgid is its own
 * pid. Every sibling looked like a separate address space again, and the
 * paragraph above describes what follows exactly.
 *
 * Measured on a hung th9 rather than reasoned about: two contexts, Tgid 77256
 * and Tgid 77264, both mapping backing inode 8482 — one address space reported
 * as two.
 *
 * The monitor does not have to ask this machine. It was told which context
 * shares an address space with which at the moment each was created, and that
 * record -- the lineage below -- is the answer. It also does not depend on the
 * destination being Linux, which reading /proc for it did.
 */

static uint64_t as_id(memory_target target)
{return target ? target->view.mm->identity : 0;}

/*
 * Every context running on one address space, the named one included.
 *
 * A page belongs to an address space, so taking it away or write-protecting it
 * has to happen in each of them. Doing it only in the context that was named
 * leaves the siblings with the page mapped and writable: they never fault on it
 * again, so they never learn that a shadow now holds it, and they read their own
 * copy for the rest of the run — while the machine that was given the page
 * writes the copy everyone else has stopped looking at.
 */
static int mm_members(uint64_t identity,memory_target *out,int max)
{
	struct execution_mm *mm=mm_record(identity);
	struct execution_context_binding *bindings[MAX_CTX];
	pthread_mutex_lock(&ctx_lock);
	int count=nctxs;
	for(int i=0;i<count;i++)bindings[i]=&ctx_memory[i];
	pthread_mutex_unlock(&ctx_lock);
	int n=0;
	for(int i=0;i<count;i++) {
		memory_target current=execution_target_capture(bindings[i]);
		if(current->view.mm!=mm || ctx_execution_ended(ctx_id(current),0))continue;
		if(n>=max) {fputs("vmremote: incomplete MM membership snapshot\n",stderr);abort();}
		out[n++]=current;
	}
	return n;
}
static int as_members(memory_target target,memory_target *out,int max)
{
	if(!target)return 0;
	int n=mm_members(target->view.mm->identity,out,max);
	for(int i=1;i<n;i++)if(out[i]->context==target->context) {
		memory_target first=out[0];out[0]=out[i];out[i]=first;break;
	}
	return n;
}

/*
 * One word of the program's memory, read in EVERY context of its address
 * space, at moments that are its own.
 *
 * A word several contexts update with an atomic read-modify-write is where a
 * copy that should have been a mapping shows itself: the arithmetic is the
 * oracle, because the program's own count is a number the run can be checked
 * against. `__nptl_nthreads' is such a word -- glibc's start_thread does
 * `lock subl $1' on it and calls exit(3) if it reaches zero -- and a lost
 * increment there makes a worker thread run the whole of the C library's exit
 * path, taking __exit_lock and never releasing it.
 *
 * Off unless VMR_WATCHWORD names an address, and then it is one peek per
 * context per event. Nothing here writes, and peek() cannot create the page it
 * reports on.
 */
static uint64_t watchword_addr(void)
{
	static uint64_t a = 1;

	if (a == 1) {
		const char *e = getenv("VMR_WATCHWORD");

		a = e ? strtoull(e, NULL, 0) : 0;
	}
	return a;
}

static void watchword_log(const char *where, memory_target ctx, long long nr,
			  uint64_t rip)
{
	uint64_t a = watchword_addr();
	memory_target mem[MAX_CTX];
	char line[512];
	int n, i, k = 0;

	if (!a)
		return;
	n = as_members(ctx, mem, MAX_CTX);
	for (i = 0; i < n && k < (int)sizeof(line) - 32; i++) {
		uint32_t w = 0xdeadbeef;
		int got = peek(mem[i], a, &w, 4) == 4;

		k += snprintf(line + k, sizeof(line) - k, " %d=%s%u",
			      ctx_id(mem[i]), got ? "" : "?", w);
	}
	/*
	 * The RIP, because the value alone cannot tell a lost update from a
	 * repeated one. glibc's start_thread does `lock subl' on this word and
	 * then rt_sigprocmask, once; a run in which the count drops twice for
	 * one thread has either lost an increment somewhere else or executed
	 * that block twice, and only the address the syscall was made from
	 * separates those two.
	 */
	fprintf(stderr, "[vmremote] WATCHWORD 0x%llx at %s (ctx %d, nr %lld, "
		"rip 0x%llx):%s\n",
		(unsigned long long)a, where, ctx_id(ctx), nr,
		(unsigned long long)rip, line);
}

/* Record a loan under its full address-space/page key. Clearing a loan keeps
 * its history as guest-owned; only a mapping retirement removes the record. */
static void as_owner_note(uint64_t as, uint64_t owner);
static void page_lend_at(memory_target ctx, uint64_t page, uint64_t owner, int shared,
			 const char *fn, int line)
{
	uint64_t as = as_id(ctx);
	struct loan *entry;
	uint64_t old_owner = 0;
	int old_known;

	page &= ~UINT64_C(4095);
	pthread_mutex_lock(&lent_lock);
	entry = loan_find(as, page, 0);
	old_known = entry != NULL;
	if (entry)
		old_owner = entry->owner;
	if (!entry && !owner) {
		n_clear_miss++;
	} else {
		if (!entry)
			entry = loan_find(as, page, 1);
		if (owner && old_owner && old_owner != owner) {
			n_double_lend++;
			if (n_double_lend <= 8)
				fprintf(stderr, "[vmremote] 0x%llx is lent to shadow %llu "
					"and is being lent to %llu as well\n",
					(unsigned long long)page,
					(unsigned long long)old_owner,
					(unsigned long long)owner);
		}
		if (owner && !old_owner)
			n_lent++;
		entry->owner = owner;
		entry->shared = shared;
		as_owner_note((uint64_t)as, owner);
	}
	pthread_mutex_unlock(&lent_lock);

	if (!pglog_on || (pglog_on == 2 && page != pglog_page))
		return;
	fprintf(stderr, "[LEND abs=%llu tid=%d] vmremote as=%d 0x%012llx "
		"holder %s(%llu) -> %s(%llu)%s (%s:%d)\n",
		(unsigned long long)now_us(), (int)own_tid(),
		(int)as, (unsigned long long)page,
		!old_known ? "none" : old_owner ? "shadow" : "guest",
		(unsigned long long)old_owner,
		owner ? "shadow" : "guest", (unsigned long long)owner,
		shared ? " shared" : "", fn, line);
}

#define page_lend_mode(c, p, o, s) \
	page_lend_at((c), (p), (o), (s), __func__, __LINE__)
#define page_lend(c, p, o) page_lend_at((c), (p), (o), 0, __func__, __LINE__)

/*
 * Does the guest still have a readable copy of this page? Only meaningful while
 * a shadow holds it: shared means both sides may read and neither may write.
 */
/*
 * Mark, or ask about, a page this side has write-protected purely to be told
 * when the guest writes it. Same table as the loans, deliberately: it is the
 * same question ("what did this side do to this page") asked of the same key,
 * and a second table keyed the same way is a second thing to keep in step.
 */
static void page_watch_set(memory_target ctx, uint64_t page, int on)
{
	uint64_t as = as_id(ctx);
	struct loan *entry;

	pthread_mutex_lock(&lent_lock);
	entry = loan_find(as, page, on != 0);
	if (entry)
		entry->watched = on;
	pthread_mutex_unlock(&lent_lock);
}

static int page_watched(memory_target ctx, uint64_t page)
{
	uint64_t as = as_id(ctx);
	struct loan *entry;
	int watched;

	pthread_mutex_lock(&lent_lock);
	entry = loan_find(as, page, 0);
	watched = entry && !entry->owner && entry->watched;
	pthread_mutex_unlock(&lent_lock);
	return watched;
}

static uint64_t page_holder(memory_target ctx, uint64_t page);

/*
 * Must this side's copy of the page be left alone?
 *
 * Two facts used to share one field. A pull is a *take*: VMR_OP_CTXPAGE removes
 * the page from the source, so the source is not a holder afterwards -- but a
 * shared loan was recorded anyway, because something had to stop the fetch
 * paths refilling the page over what the guest had since written. The lie was
 * load-bearing for that second job only.
 *
 * page_holder() answers ownership: who else has this page, and must it be
 * revoked before the guest may write. page_keep() answers refill: is this
 * side's copy the current one. A page this side protected in order to see the
 * next write is current here even though nobody holds it.
 */
static int page_keep(memory_target ctx, uint64_t page)
{
	return page_holder(ctx, page) != 0 || page_watched(ctx, page);
}

static int page_shared(memory_target ctx, uint64_t page)
{
	uint64_t as = as_id(ctx);
	struct loan *entry;
	int shared;

	pthread_mutex_lock(&lent_lock);
	entry = loan_find(as, page, 0);
	shared = entry && entry->owner && entry->shared;
	pthread_mutex_unlock(&lent_lock);
	return shared;
}

/*
 * What this side's record says about the page, with "never seen" told apart
 * from "the guest has it" -- which page_holder() cannot do, both being 0.
 *
 * The difference is the destination's half of AUDIT XII. A page with no entry
 * has never been handed over, so recording a loan for it when this side answers
 * ABSENT is the start-up case that record exists for. A page whose entry says
 * the guest holds it, that this side cannot read, is a page this side has LOST,
 * and writing "the shadow has it" over that is an inference about the other
 * machine (PRINCIPLES §1) which makes the loss permanent.
 */
#define LENT_UNSEEN 0
#define LENT_GUEST  1
#define LENT_SHADOW 2

static int page_lent_state(memory_target ctx, uint64_t page)
{
	uint64_t as = as_id(ctx);
	struct loan *entry;
	int state = LENT_UNSEEN;

	pthread_mutex_lock(&lent_lock);
	entry = loan_find(as, page, 0);
	if (entry)
		state = entry->owner ? LENT_SHADOW : LENT_GUEST;
	pthread_mutex_unlock(&lent_lock);
	return state;
}

/* Which shadow holds this page, or 0 if there is no active recorded loan. */
static uint64_t page_holder(memory_target ctx, uint64_t page)
{
	uint64_t as = as_id(ctx);
	struct loan *entry;
	uint64_t owner;

	pthread_mutex_lock(&lent_lock);
	entry = loan_find(as, page, 0);
	owner = entry ? entry->owner : 0;
	pthread_mutex_unlock(&lent_lock);
	return owner;
}

/*
 * The recall channels, one per shadow. A shadow opens it and says which pid it
 * is; from then on this side asks and the shadow answers, which is the inverse
 * of every other exchange on this port. Its own lock: a recall is a request and
 * a reply on one socket, and several guest threads fault at once.
 */
#define RECALL_MAX 64
static struct recall_ch {
	uint64_t owner;
	int fd;
	pthread_mutex_t lock;
} recall_ch[RECALL_MAX];

/*
 * Which shadows belong to which ADDRESS SPACE.
 *
 * A loan is keyed by (address space, page) -- see page_lend_at() -- but its
 * `owner` is a single shadow pid, and the recall channel is looked up by that
 * pid alone. So when the shadow that happened to take a page exits, its channel
 * closes and the page becomes unrecallable, even though the source's mm is
 * alive and every sibling thread of that program still maps it. The page is
 * then alive at NEITHER site: the destination gave it away, and the side that
 * has it can no longer be asked.
 *
 * Measured, and it is the whole of what is left in ws1 and netsurf -- the same
 * page in both, main's stack:
 *
 *   LOST: 0x7fffffffe000 was lent to shadow N (lent whole, its channel is
 *         gone), which no longer has it
 *   FATAL: A PAGE OF THE GUEST'S MEMORY IS ON NEITHER MACHINE
 *
 * and the zeros that follow are what netsurf reads back as a freed tcache
 * pointer ("free(): double free detected in tcache 2"), as a NULL string in
 * g_str_hash, and as -0x148 off a NULL base.
 *
 * The shadows of one address space share one mm, so ANY of them can answer for
 * a page of it. This records that association at the moment of the lend, where
 * both facts are in hand, so a recall whose owner has gone can be re-aimed at a
 * living sibling instead of being declared lost.
 */
#define AS_OWNERS 64
static struct { uint64_t as, owner; } as_owner[AS_OWNERS];
static int n_as_owner;

static void as_owner_note(uint64_t as, uint64_t owner)
{
	int i;

	if (!as || !owner)
		return;
	for (i = 0; i < n_as_owner; i++)
		if (as_owner[i].as == as && as_owner[i].owner == owner)
			return;
	if (n_as_owner < AS_OWNERS) {
		as_owner[n_as_owner].as = as;
		as_owner[n_as_owner].owner = owner;
		n_as_owner++;
	}
}
static int n_recall_ch;
static pthread_mutex_t recall_tbl_lock = PTHREAD_MUTEX_INITIALIZER;

static unsigned long n_recall_resited;	/* recalls re-aimed at a live sibling */

static struct recall_ch *recall_find(uint64_t owner)
{
	struct recall_ch *r = NULL;

	pthread_mutex_lock(&recall_tbl_lock);
	for (int i = 0; i < n_recall_ch; i++)
		if (recall_ch[i].owner == owner && recall_ch[i].fd >= 0) {
			r = &recall_ch[i];
			break;
		}
	pthread_mutex_unlock(&recall_tbl_lock);
	return r;
}

/*
 * Ask the shadow that holds this page for it back.
 *
 * Returns 1 with the page in buf, 0 if that shadow does not have it after all
 * (it was never taken, or has already gone back — not an error), or -1 if the
 * channel is gone, which means falling back to fetching the page the old way.
 */
static int page_recall_op(memory_target pid, uint64_t page, uint64_t owner, void *buf,
			  unsigned op)
{
	struct vmr_pgreq rq = {.magic=VMR_PG_MAGIC,.op=op,.addr=page,
        .target=pid->source,.owner=owner};
	struct recall_ch *ch = recall_find(owner);
	struct vmr_pgrsp rs;
	int ret = -1;

	/*
	 * The recorded shadow is gone. Ask a living one of the SAME ADDRESS
	 * SPACE instead: they share one mm, so the page is as much theirs to
	 * hand back, and a loan that outlives one thread is not a lost page.
	 * Without this the page is alive at neither site and the guest is given
	 * zeros for it -- see as_owner_note().
	 */
	if (!ch) {
		uint64_t as = (uint64_t)as_id(pid);
		int i;

		pthread_mutex_lock(&recall_tbl_lock);
		for (i = 0; i < n_as_owner && !ch; i++) {
			int j;

			if (as_owner[i].as != as || as_owner[i].owner == owner)
				continue;
			for (j = 0; j < n_recall_ch; j++)
				if (recall_ch[j].owner == as_owner[i].owner &&
				    recall_ch[j].fd >= 0) {
					ch = &recall_ch[j];
					break;
				}
		}
		pthread_mutex_unlock(&recall_tbl_lock);
		if (ch) {
			n_recall_resited++;
			rq.owner = ch->owner;
		}
	}
	if (!ch)
		return -1;
	pthread_mutex_lock(&ch->lock);
	if (pg_rw(ch->fd, &rq, sizeof(rq), 1) == 0 &&
	    pg_rw(ch->fd, &rs, sizeof(rs), 0) == 0 && vmr_page_reply_matches(&rq,&rs)) {
		if (rs.status == VMR_PG_ABSENT) {
			ret = 0;
			n_recall_absent++;
		} else if (pg_rw(ch->fd, buf, VMR_PG_SIZE, 0) == 0) {
			ret = 1;
			n_recalled++;
		}
	}
	pthread_mutex_unlock(&ch->lock);
	if (ret < 0) {
		/* A dead channel must not be asked again for every fault. */
		pthread_mutex_lock(&recall_tbl_lock);
		if (ch->fd >= 0) {
			close(ch->fd);
			ch->fd = -1;
		}
		pthread_mutex_unlock(&recall_tbl_lock);
	}
	return ret;
}

static int page_recall(memory_target pid, uint64_t page, uint64_t owner, void *buf)
{
	return page_recall_op(pid, page, owner, buf, VMR_PG_RECALL);
}

/*
 * Ask the shadow to keep the page but stop writing it, and hand over what it
 * holds. The cheap half of coherence: a page both machines only read never has
 * to move again.
 */
static int page_downgrade(memory_target pid, uint64_t page, uint64_t owner, void *buf)
{
	return page_recall_op(pid, page, owner, buf, VMR_PG_DOWNGRADE);
}

/*
 * Take a page out of the guest, so that the shadow about to receive it is the
 * only holder. Returns 1 with the bytes in buf, or 0 if the guest did not have
 * the page — in which case nothing was taken and the caller answers as it
 * always did, from the oracle or the file.
 */
/*
 * `away` distinguishes the two things a take is used for, and they need
 * different treatment now.
 *
 * A hand-over: the page is leaving for the source, which becomes its only
 * holder. That has to reach the memory, and for an object-backed context the
 * memory is the object -- clearing the page tables leaves the bytes in it and
 * the guest's next touch gets the identical page back (vmtake proves both, and
 * proves that punching the object fixes it).
 *
 * A take-and-put-back: page_make_writable() takes the page only to write it
 * straight back, to get a writable mapping. Punching the object there would
 * leave a hole between the two halves of one operation, and a sibling context
 * faulting into that window sees memory the address space has not lost.
 */
/*
 * `away` and `punch` are separate answers to separate questions, and merging
 * them was the bug.
 *
 *   away  -- remove the page from every context of this address space, which
 *            needs TAKEOBJ's unmap_mapping_range() rather than the per-context
 *            sequence.
 *   punch -- the page is LEAVING this side, so drop it from the backing object
 *            as well, or the guest keeps a copy of a page it has been told it
 *            lost.
 *
 * page_make_writable() wants the first and must not have the second: it takes
 * the page only to hand it straight back, and the old comment below argued the
 * punch "costs nothing here... the poke writes the bytes straight back into the
 * same object page, so the punch is undone before anything can observe it".
 *
 * Something can observe it. page_make_writable() runs WITHOUT ctl_lock -- on
 * purpose, measured: holding it there gave 15/3/2 against 18/2/0 and opened a
 * fault storm of 898323 faults in 4.5s -- while serve_object_mapped() holds
 * ctl_lock across exactly backing_has() + MAPOBJ. That lock excludes the
 * hand-over take, which punches under it, and excludes nothing here. So this
 * punch lands between the two halves of the other side's critical section,
 * MAPOBJ finds a hole, and -- having no FOLL_NOFAULT -- shmem_fault()
 * ALLOCATES a fresh zero folio and maps it over the live page. Nothing writes
 * a byte, so every install-side detector stays silent.
 *
 * Measured on hx2 with the live-page check armed: 98 of 553 no-install serves
 * handed back a folio below what had already changed hands, site80
 * (serve_object_mapped) 51 of them.
 *
 * Not fixed by locking this path, and not fixed by MAPOBJ growing FOLL_NOFAULT
 * -- both were tried; the first is the regression above and the second is
 * core-kernel code. Fixed by not punching a page that is not going anywhere.
 *
 * And do NOT go on to "fix" MAPOBJ by making it decline on a hole. That was
 * the next step and it is a dead end: an arm that made serve_object_mapped()
 * decline every time -- the strongest possible form of that change -- took hx2
 * from 15 pass / 4 fail to 0 pass / 16 fail, 59 stores lost against 1, with
 * "invented" ZERO in both arms. Mapping this side's own folio is not what
 * loses the stores; it is what prevents them. The losses live on the paths
 * that still copy.
 */
static int page_take_mode(memory_target pid, uint64_t page, void *buf, int away,
			  int punch)
{
	struct vmctx_mem m = { .addr = page, .len = VMR_PG_SIZE,
			       .buf = (uint64_t)(uintptr_t)buf };
	memory_target as[MAX_CTX];
	int n, i, got = 0, from_obj = 0;
	tracep(page, pid, "TAKE takes it away", 1);
	/*
	 * Out of every context of this address space, not just the one the
	 * request named. They share the backing object, so the page they map is
	 * one page: removing it from one context's page tables leaves the others
	 * pointing at it, still writable and never faulting again.
	 *
	 * The bytes are the same in each — it is the same page — so the first
	 * context that has it answers and the rest are asked only to let go.
	 * A context that does not have it writes nothing into buf and says so by
	 * returning less than a page.
	 */
	/*
	 * Three answers, not two.
	 *
	 * VMR_PG_SIZE is "it was here and it has gone". Zero is "there is
	 * nothing at that address in this context", which is the ordinary case
	 * -- a sibling that has never touched the page -- and no failure at
	 * all; the kernel says so deliberately, and ESRCH from a context that
	 * has already exited means the same thing, since a task with no mm
	 * cannot write anything. Anything else is a context of this address
	 * space that still has the page and would not give it up.
	 *
	 * That last case used to be indistinguishable from the second, because
	 * a single `got` was set by whichever context happened to succeed. The
	 * page then went to the shadow as its exclusive copy while a sibling
	 * still had it mapped and writable: two writers, which is the one thing
	 * this protocol exists to prevent, produced by an errno nobody looked
	 * at.
	 *
	 * It is still reported as a success, and that is deliberate rather than
	 * an omission. By the time a later context fails, an earlier one has
	 * already had the page zapped out of it and the object punched, so the
	 * bytes in buf are the only current copy there is; answering "nothing
	 * here" would have the source substitute its own stale copy and the
	 * guest's writes would be gone for certain, rather than at risk. The
	 * failure is counted and named instead. If this counter is ever
	 * non-zero the run's memory cannot be trusted, and the repair belongs
	 * where the take failed, not here.
	 */
	/*
	 * One operation, if this kernel has it.
	 *
	 * Everything below is the hand-over assembled out of per-context pieces:
	 * one VMCTX_CTL_TAKE per context, each zapping its own page tables and
	 * copying afterwards, and then a punch of the object at the end. Every
	 * join between those pieces is a moment when some context still has the
	 * page and can write it, and the copy that goes to the source is taken
	 * in one of those moments. VMCTX_CTL_TAKEOBJ does the whole thing under
	 * the object's folio lock instead, so there is no such moment.
	 *
	 * Kept as the fallback rather than removed: a range that is not backed
	 * by the object -- and an older kernel -- answers negatively, and for
	 * those this is still the only way to do it.
	 */
	if (away) {
		long r;
		int busy = 0;


		/*
		 * -EBUSY says the page cannot move yet, not that this path
		 * does not work: something on the other side can still reach
		 * it. Waiting is the answer; falling back to the per-context
		 * sequence would be doing by hand exactly what the kernel just
		 * refused to do.
		 */
		/*
		 * One second. A legitimate refusal (DMA pin, a syscall's
		 * write hold) clears in milliseconds; a second of EBUSY is a
		 * stuck step, and the fault waiting on this take has a 2s
		 * deadline in the kernel that this bound must fit inside.
		 * On expiry: say so with a backtrace -- the timeout is the
		 * diagnosis, not an inconvenience.
		 */
		uint64_t tk0 = now_us();

		while ((r = ctl(pid, VMCTX_CTL_TAKEOBJ, &m)) < 0 &&
		       errno == EBUSY && busy++ < VMR_TAKE_STUCK) {
			/*
			 * A long take points at a lock, not at a slow page: a
			 * legitimate pin (DMA, a syscall's write hold) clears in
			 * milliseconds. Say so the instant it stops being brief,
			 * with the state, rather than at the bound -- a delay IS
			 * the diagnosis.
			 */
			if (busy == VMR_TAKE_SLOW)
				fprintf(stderr, "[vmremote] SLOW TAKEOBJ 0x%llx "
					"ctx %d: still EBUSY after %dms -- a holder "
					"is not letting go; this is a locking "
					"problem, not a slow page\n",
					(unsigned long long)page, ctx_id(pid),
					VMR_TAKE_SLOW);
			usleep(1000);
		}
		if (busy)
			n_takeobj_busy++;
		if (r < 0 && errno == EBUSY) {
			/*
			 * No progress. The page cannot be taken because someone
			 * will not let go, and every "continue anyway" forks the
			 * page (two live copies -- the lost write). So stop here,
			 * loudly, with the state that names the stuck holder,
			 * rather than paper over a locking defect with a stale
			 * fallback.
			 */
			fprintf(stderr, "[vmremote] FATAL: TAKEOBJ 0x%llx ctx %d "
				"made no progress in %lluus (%d tries EBUSY). A "
				"lock or pin is stuck; this is a coherence-lock "
				"defect, not a slow path.\n",
				(unsigned long long)page, ctx_id(pid),
				(unsigned long long)(now_us() - tk0), busy);
			step_timeout("TAKEOBJ of", page, (uint64_t)busy * 1000);
			abort();
		}

		if (r == VMR_PG_SIZE) {
			/*
			 * ...through the same exit as every other successful
			 * take, and that is a fix rather than tidiness.
			 *
			 * This path used to `return 1` here, so it never reached
			 * the tail of this function -- and the tail is where
			 * clobber_check() seeds the detector's high-water mark
			 * from the bytes a hand-over actually captured. TAKEOBJ
			 * is the path essentially every hand-over takes (hx2:
			 * 90229 of 90229), so the mark was never seeded by a
			 * take at all: it was raised only by installs, which
			 * means the "did an install write something older than
			 * the guest had" test was comparing installs against
			 * installs. An instrument wired to the path nothing uses
			 * reports a zero it has not earned.
			 */
			n_takeobj++;
			got = 1;
			goto took;
		}
		if (r == 0) {
			/*
			 * The address space does not have the page, said by
			 * the object itself rather than inferred from a page
			 * table. Nothing to hand over and nothing to punch.
			 */
			n_takeobj_absent++;
			return 0;
		}
		/*
		 * TAKEOBJ aimed at `pid` returned an error other than EBUSY --
		 * -ENOENT is the one that matters, and it does NOT mean the
		 * address space lacks the page. TAKEOBJ locates the object by
		 * looking up `pid`'s OWN vma at this address (kernel
		 * vmctx_ctl_takeobj: vma_lookup -> !vma is -ENOENT), and a
		 * context that never faulted the page in has no vma there even
		 * though a sibling of the same address space holds it mapped.
		 * hx1 makes this the ordinary case, not a rare one: the context
		 * that made the forwarded read(2) is the one named for the take,
		 * but the writer threads are the ones that map the page. Since
		 * every context of an address space maps the SAME backing object,
		 * TAKEOBJ aimed at ANY holder runs unmap_mapping_range over that
		 * one object and evicts every sibling at once -- so retry it
		 * against each sibling until one that actually maps the page
		 * answers. Only if none do is this genuinely not object-backed,
		 * and only then does the per-context take below (which cannot
		 * move a page mapped by more than one context) get a turn.
		 */
		n = as_members(pid, as, MAX_CTX);
		{
			int myobj = backing_find(pid);

			for (i = 0; myobj >= 0 && i < n; i++) {
				int busy2 = 0;

				if (as[i] == pid)
					continue;
				/*
				 * Only a sibling on the SAME object. A post-fork
				 * snapshot can share an address by coincidence while
				 * living in a different memfd; TAKEOBJ against it
				 * would reach the wrong page.
				 */
				if (backing_find(as[i]) != myobj)
					continue;
				while ((r = ctl(as[i], VMCTX_CTL_TAKEOBJ, &m)) < 0 &&
				       errno == EBUSY && busy2++ < 1000)
					usleep(1000);
				if (busy2)
					n_takeobj_busy++;
				if (busy2 >= 1000)
					step_timeout("TAKEOBJ (sibling) of", page,
						     (uint64_t)busy2 * 1000);
				if (r == VMR_PG_SIZE) {
					n_takeobj++;
					got = 1;
					goto took;
				}
				if (r == 0) {
					n_takeobj_absent++;
					return 0;
				}
				/* -ENOENT again: not a holder either; keep looking. */
			}
		}
		n_takeobj_fallback++;
	}

	n = as_members(pid, as, MAX_CTX);
	for (i = 0; i < n; i++) {
		long r;
		int busy = 0;

		/*
		 * -EBUSY means wait, here as everywhere else. This loop did
		 * not, and pg3 measured what that costs: one refused take
		 * ("page busy #1: refs=1 want=0(+1) lru=1", a transient
		 * reference on an LRU-resident page) left got=0, the caller
		 * answered the source ABSENT, and the source filled the
		 * guest's *stack* page with zeros. 2.3ms later the guest
		 * returned from read(2) -- rax=0x12, the call had succeeded --
		 * through a return address that was now zero, and died
		 * fetching instructions at 0 with [rsp] all zeros.
		 */
		while ((r = ctl(as[i], VMCTX_CTL_TAKE, &m)) < 0 &&
		       errno == EBUSY && busy++ < 1000)
			usleep(1000);
		if (busy >= 1000)
			step_timeout("per-context TAKE of", page,
				     (uint64_t)busy * 1000);

		if (r == VMR_PG_SIZE) {
			got = 1;
		} else if (r < 0 && errno != ENOENT && errno != ESRCH) {
			int e = errno;	/* before anything else can set it */

			n_take_partial++;
			if (n_take_partial <= 8)
				fprintf(stderr, "[vmremote] could not take 0x%llx "
					"out of context %d (%s), one of %d "
					"sharing address space %d: it keeps a "
					"writable copy of a page that is being "
					"handed over as the only one\n",
					(unsigned long long)page, ctx_id(as[i]),
					strerror(e), n, (int)as_id(pid));
		}
	}
	/*
	 * The object is the address space's memory; a page table is a view of
	 * it. So "no context yielded the page" is not "this side does not have
	 * the page", and treating it as such is a hand-over that hands nothing
	 * over while the caller believes it did.
	 *
	 * It happens on the ordinary path, not a rare one. Taking a page from a
	 * context clears its PTE and leaves the bytes in the object -- that is
	 * what makes the context fault again -- so between a take and the next
	 * touch every context reports the page absent while the address space
	 * plainly holds it. page_read_as() has always known this and reads the
	 * object; the take did not, so a page in exactly that state was read
	 * successfully, refused by the take, and then discarded by the caller,
	 * which answered the source ABSENT and let it invent zeros.
	 *
	 * Measured, on pg1's join:
	 *
	 *   [PG 001366] 0x7ffff7ff5000 GET ... present=0 backing=1
	 *   [PG 001367] 0x7ffff7ff5000 GET take r=0 post=0 first8=0200000000000000
	 *
	 * -- the bytes are right there in the answer, and the take says there is
	 * nothing to take. The source zero-filled the page holding the word a
	 * joining thread was waiting on, and the program spun on the two copies
	 * disagreeing 106,624 times before the run was stopped.
	 *
	 * Only for a hand-over. page_make_writable() takes a page to put it
	 * straight back and wants "not present" to mean "leave it alone".
	 */
	if (!got && away) {
		int bf = backing_find(pid);

		if (bf < 0)
			bf = backing_find(pid);
		if (bf >= 0 && backing_has(pid, page) &&
		    pread(bf, buf, VMR_PG_SIZE,
			  (off_t)(page & ~0xfffUL)) == (ssize_t)VMR_PG_SIZE) {
			got = 1;
			from_obj = 1;
			n_take_from_object++;
			tracep(page, pid, "TAKE from the object", 1);
		}
	}
	if (got && punch) {
		int bf = backing_find(pid);

		if (bf < 0)
			bf = backing_find(pid);
		if (bf >= 0 &&
		    fallocate(bf, FALLOC_FL_PUNCH_HOLE | FALLOC_FL_KEEP_SIZE,
			      (off_t)page, VMR_PG_SIZE) < 0 &&
		    n_punch_fail++ < 8)
			fprintf(stderr, "[vmremote] could not punch 0x%llx out "
				"of the backing object: %s -- the guest keeps a "
				"copy of a page it has been told it lost\n",
				(unsigned long long)page, strerror(errno));
	}
took:
	if (got) {
		/*
		 * Did this hand-over capture what the guest had?
		 *
		 * The live-page check catches an INSTALL that writes over a page
		 * the address space is holding. This is the other end of the
		 * same question and the one thing that check cannot see: a TAKE
		 * that hands the source a copy older than a previous take
		 * already captured. Nothing installs anything then -- the bytes
		 * simply leave stale -- which is the shape of what was left of
		 * hx2 after the two copy sites were fixed: a loss with no
		 * LOSTWRITE line anywhere.
		 *
		 * It needs no read of the guest and costs nothing: a monotonic
		 * slot cannot fall on its own, so a take producing a value below
		 * the mark of what has already changed hands is a store this
		 * hand-over did not capture. Site 70 is the take. (Reading the
		 * guest's live page per hand-over was tried first and is not
		 * affordable -- hx2 hands this page over 90000 times in 20
		 * seconds and the run stops finishing.)
		 */
		clobber_check(pid, page & ~0xfffUL, buf, VMR_PG_SIZE, 70);
		hist_note(page & ~0xfffUL, buf);
		/*
		 * A capture of ALL ZEROS from a page this side has already
		 * transmitted is a page somebody emptied without a record:
		 * photographed in the hx2 pool (session 25) as TAKE-AWAY gen=76
		 * sum=04000001 after seventy-five real captures, served to the
		 * source, then installed over the real bytes by the generation
		 * repair (the retained copy of gen 76 WAS the zeros). Say where
		 * the take got them and what every member maps, the first few
		 * times; the trail follows.
		 */
		if (page_sum(buf) == VMR_ZERO_PAGE_SUM &&
		    retain_get(pid, page, NULL) && n_zero_take++ < 8) {
			memory_target mem[MAX_CTX];
			int nm = as_members(pid, mem, MAX_CTX), mi;

			fprintf(stderr, "[vmremote] ZERO-TAKE 0x%llx ctx %d "
				"(as %d) at count %u: captured an all-zero page "
				"the record says was transmitted before, from "
				"%s; members:", (unsigned long long)page,
				ctx_id(pid), (int)as_id(pid), pgen_get(pid, page),
				from_obj ? "the OBJECT (no context mapped it)"
					 : "a context's mapping (TAKEOBJ)");
			for (mi = 0; mi < nm; mi++)
				fprintf(stderr, " %d:present=%d", ctx_id(mem[mi]),
					page_present(mem[mi], page));
			fprintf(stderr, "; object has it now: %d; lent: %d "
				"holder %llu; pull active: %d\n",
				backing_has(pid, page), page_lent_state(pid, page),
				(unsigned long long)page_holder(pid, page),
				pull_active(page));
			trail_dump_page(page);
		}
		/*
		 * The capture moves the clock. Whether the bytes leave for
		 * the source (punch) or go straight back (page_make_writable),
		 * every copy of the page taken before this instant is now
		 * older than what this side holds, and a serve that brings
		 * one back is judged against this count.
		 */
		trail_note(pid, page, punch ? TR_TAKE_AWAY : TR_TAKE_PUT,
			   pgen_bump(pid, page), (uint32_t)punch, buf);
	}
	return got;
}

/* Taken to be given away: it must leave the object too. */
static int page_take_away(memory_target pid, uint64_t page, void *buf)
{
	return page_take_mode(pid, page, buf, 1, 1);
}

/* Taken to be handed straight back; the object keeps it. */
/*
 * Leave the guest a readable copy it may not write. Its next write arrives as a
 * protection fault, which is where ownership is settled again.
 */
/*
 * Write-protect a page across the whole address space at once, and -- if buf is
 * given -- return the bytes as of that protect.
 *
 * This is the read-claim the source makes before it is promised a shared copy,
 * and it is the counterpart of page_take_away()'s TAKEOBJ. The atomicity is the
 * point: the earlier form walked the contexts one at a time with VMCTX_CTL_
 * PROTECT, and between protecting one sibling and the next a still-writable
 * sibling could advance the page -- the write raising no fault because it was
 * still writable there -- so this side would send bytes one write old and
 * believe it was sharing. That gap was the LOSTWRITE: a write the guest made
 * and the source never saw.
 *
 * VMCTX_CTL_PROTECTOBJ closes it. It names the backing folio from any one
 * context that still maps the address (file + index are identical for every
 * sibling) and calls folio_mkclean() under the folio lock -- an rmap walk that
 * clears the write bit on every PTE mapping the folio in one go, with no
 * moment in between where any context can write it. With buf != 0 it also
 * copies the page out under that same lock, so the bytes cannot be one write
 * newer than the protection that is supposed to have frozen them.
 *
 * We try the members in turn only to find one that still has a vma over the
 * address (ENOENT/ESRCH => that context does not; try the next). A >= 0 answer
 * has already protected them all through the rmap, so we stop.
 *
 * Return: 1 protected (and buf, if given, holds the page); 0 otherwise. A page
 * the object does not have anywhere (folio not in cache => a 0 from the kernel)
 * is reported as a failure deliberately -- the caller answers the source ABSENT
 * and it falls back to its own copy, exactly as before. Widening "absent
 * everywhere" into success measured worse in the per-context form for reasons
 * that were never understood, and that reasoning is left untouched here.
 *
 * The watch mark is the record that *this side protected the page* so it will
 * see the next write; page_keep()/branch 6 act on it. Set here, at the one
 * place the protection is applied, rather than at the three call sites.
 */
static int page_protect_read(memory_target pid, uint64_t page, void *buf)
{
	struct vmctx_mem m = { .addr = page, .len = VMR_PG_SIZE,
			       .buf = (uint64_t)(uintptr_t)buf };
	memory_target as[MAX_CTX];
	int n, i;
	long r = -1;

	tracep(page, pid, "PROTECTOBJ makes it read-only across the object", 1);
	n = as_members(pid, as, MAX_CTX);
	for (i = 0; i < n; i++) {
		r = ctl(as[i], VMCTX_CTL_PROTECTOBJ, &m);
		if (r >= 0 || (errno != ENOENT && errno != ESRCH))
			break;
	}
	if (r == VMR_PG_SIZE) {
		page_watch_set(pid, page, 1);
		return 1;
	}
	if (r < 0) {
		int e = errno;

		if (e != ENOENT && e != ESRCH) {
			n_protect_partial++;
			if (n_protect_partial <= 8)
				fprintf(stderr, "[vmremote] could not object-wide "
					"write-protect 0x%llx in address space %d "
					"(%s): the source is about to be promised "
					"nobody here writes it\n",
					(unsigned long long)page,
					(int)as_id(pid), strerror(e));
		}
	}
	return 0;
}

static int page_protect(memory_target pid, uint64_t page)
{
	return page_protect_read(pid, page, NULL);
}

/*
 * Give the guest back the right to write a page it holds read-only. There is no
 * "unprotect": the page is taken out and written back, and writing it back
 * forces write access, which installs a fresh writable page. Doing it this way
 * means the kernel never hands out write permission to a page that is shared
 * for copy-on-write — it makes a private one, exactly as a real write fault
 * would.
 */
static int own_object_page(memory_target pid, uint64_t base, void *buf);

/* VMR_TAKEPUT_PROBE=1 arms the raced-put-back probe in page_make_writable. */
static int takeput_probe(void)
{
	static int on = -1;

	if (on < 0) {
		const char *e = getenv("VMR_TAKEPUT_PROBE");

		on = (e && !strcmp(e, "1")) ? 1 : 0;
	}
	return on;
}

static int page_make_writable(memory_target pid, uint64_t page)
{
	static __thread char buf[VMR_PG_SIZE];

	/*
	 * First, the operation that does not evict anyone.
	 *
	 * The take-and-put-back below uses TAKEOBJ, which unmaps the page from
	 * EVERY context of the address space and punches it out of the object.
	 * For a page this side owns and has merely write-protected to watch it,
	 * that is a hammer with a self-inflicted livelock in it: granting the
	 * write to one context evicts its siblings, a sibling then read-faults,
	 * pulls the page back and write-protects it again (site5w) -- which
	 * breaks the write permission just granted. Measured on netsurf's stack
	 * page: PROTECT and GRANT alternating 1:1, 37 grants and 38 protects in
	 * 47ms, with the guest making no progress and the watchdog ending it
	 * (exit 131). Siblings share ONE folio; a write by one is what the
	 * others are supposed to see, so evicting them is wrong on its face.
	 *
	 * VMCTX_CTL_MAPOBJ faults the page in writable (FOLL_WRITE) in this
	 * context only: it clears this side's own write-protection where the
	 * mapping allows writing, copies nothing, and leaves every sibling
	 * mapping alone. Where it cannot (a genuinely read-only mapping, or a
	 * page that is not there), the take-and-put-back below still runs.
	 */
	{
		struct vmctx_mem mw = { .addr = page, .len = VMR_PG_SIZE,
					.buf = 0 };

		/*
		 * NOT under ctl_lock, and that is measured rather than assumed.
		 *
		 * This has the same window serve_object_mapped() closes -- test
		 * that the page is present, then MAPOBJ, with a hand-over able
		 * to punch it in between and MAPOBJ allocating on the resulting
		 * hole -- and closing it the same way makes things WORSE. 20
		 * runs each: 18 pass / 2 fail / 0 no-output without the lock,
		 * 15 / 3 / 2 with it, and the two no-output runs are a new
		 * failure of their own: 898323 faults in 4.5 seconds against
		 * 448 syscalls, a fault storm ending in 242.
		 *
		 * The reason is the traffic. This path runs 77690 times in an
		 * hx2 run against a few dozen of the other; taking the lock the
		 * page service holds across a whole TAKEOBJ, that often, on the
		 * write-grant that every protected page needs, starves the very
		 * hand-overs it is trying to be atomic against. The exclusion
		 * has to come from somewhere that is not this lock, or the
		 * repair has to go where it belongs -- in MAPOBJ, which should
		 * map a folio the object has and refuse otherwise instead of
		 * allocating one.
		 *
		 * ...which kernel patch 0055 did: MAPOBJ maps only a folio the
		 * object holds and answers a hole as 0 (vmctx_mapobj_holes = 0
		 * over 96.8 M calls, session 35). The window this comment
		 * describes is closed in the kernel; the path stays unlocked.
		 */
		if (page_present(pid, page) == 1 &&
		    ctl(pid, VMCTX_CTL_MAPOBJ, &mw) == (long)VMR_PG_SIZE) {
			page_watch_set(pid, page, 0);
			n_grant_mapobj++;
			/* The guest may write now: every copy taken while it
			 * could not is about to be older than the page. */
			trail_note(pid, page, TR_GRANT, pgen_bump(pid, page),
				   0, NULL);
			return 1;
		}
	}
	tracep(page, pid, "MAKE-WRITABLE take-and-put-back", 1);
	/*
	 * The object-wide take, not the per-context one, and the reason is
	 * arithmetic rather than preference.
	 *
	 * VMCTX_CTL_TAKE unmaps the page from ONE context, so its precheck
	 * refuses whenever the folio is mapped more times than that one unmap
	 * removes -- `vmctx_folio_precheck(folio, 1, 1, "TAKE")`. Every context
	 * of an address space maps the backing object separately, so a guest
	 * with two threads has mapcount 2 and the per-context take can never
	 * succeed. It does not fail quickly either: -EBUSY means "wait and ask
	 * again", so the caller spins out its full second per context. The
	 * kernel says exactly this, once the counters are read:
	 *
	 *   vmctx: TAKE refused before the unmap: mapped 2 time(s), more than
	 *   the 1 this unmap removes, refs=4
	 *
	 * and in a failing th9 run vmctx_take_refused climbs by ~1700 against 0
	 * in every passing one.
	 *
	 * VMCTX_CTL_TAKEOBJ passes max_maps = -1 and calls
	 * unmap_mapping_range(), which removes every mapping at once -- so the
	 * mapcount test does not apply and cannot apply. That is what the
	 * `away` path uses.
	 *
	 * punch = 0 for exactly that reason. The punch belongs to a hand-over,
	 * and this is not one: the page is taken to be handed straight back, so
	 * the object must keep it.
	 *
	 * This used to pass away = 1 alone, which punched, on the reasoning that
	 * "the poke below writes the bytes straight back into the same object
	 * page, so the punch is undone before anything can observe it". It is
	 * observable, and serve_object_mapped() is what observes it -- see the
	 * split at page_take_mode().
	 */
	if (!page_take_mode(pid, page, buf, 1, 0))
		return 0;	/* not present: the next touch faults anyway */
	n_grant_takeput++;
	/*
	 * The conviction probe for the §6.3 window, armed by env because a
	 * pread here is a serialization this race-path must not pay by default
	 * (armed instruments have masked pg3 before; the counter above is free
	 * and always on). Between the TAKEOBJ above and the poke below nothing
	 * excludes a sibling: it can fault the object's folio back in (punch=0
	 * left the bytes) and write it, and the poke then restores the pre-take
	 * bytes over that write. If the object's copy no longer matches what
	 * the take captured, that interleave is happening RIGHT NOW.
	 */
	if (takeput_probe()) {
		static __thread char now[VMR_PG_SIZE];

		if (own_object_page(pid, page, now) &&
		    memcmp(now, buf, VMR_PG_SIZE) != 0) {
			n_grant_takeput_raced++;
			if (n_grant_takeput_raced <= 16)
				fprintf(stderr, "[vmremote] TAKEPUT-RACED "
					"0x%llx: the object's bytes moved "
					"between the write-grant's take and "
					"its put-back; the poke below writes "
					"pre-take bytes over a sibling's "
					"store (ctx %d)\n",
					(unsigned long long)page, ctx_id(pid));
		}
	}
	poke_site = 38;		/* make-writable take-and-put-back */
	if (poke(pid, page, buf, VMR_PG_SIZE) != (long)VMR_PG_SIZE) {
		poke_site = 0;
		return 0;
	}
	poke_site = 0;
	trail_note(pid, page, TR_TAKEPUT_POKE, pgen_get(pid, page), 0, buf);
	/* The protection is gone, so the fact it recorded is spent. */
	page_watch_set(pid, page, 0);
	return 1;
}

static int ctx_page_pull(memory_target pid, uint64_t base, void *buf,struct source_page_receipt *receipt);
#define SETTLE_CLAIM     3
#define SETTLE_BYTES     1
#define SETTLE_ABSENT    2
#define SETTLE_NOTHOLDER 6

#include "page-settle.h"

static inline int recall_register(uint64_t owner, int fd)
{
	int ok = 0;

	pthread_mutex_lock(&recall_tbl_lock);
	for (int i = 0; i < n_recall_ch; i++)
		if (recall_ch[i].owner == owner) {	/* a shadow that came back */
			if (recall_ch[i].fd >= 0)
				close(recall_ch[i].fd);
			recall_ch[i].fd = fd;
			ok = 1;
			break;
		}
	if (!ok && n_recall_ch < RECALL_MAX) {
		recall_ch[n_recall_ch].owner = owner;
		recall_ch[n_recall_ch].fd = fd;
		pthread_mutex_init(&recall_ch[n_recall_ch].lock, NULL);
		n_recall_ch++;
		ok = 1;
	}
	pthread_mutex_unlock(&recall_tbl_lock);
	return ok;
}

static void explain_stuck(memory_target pid, uint64_t addr, uint64_t err)
{
	uint64_t base = addr & ~0xfffUL, ent = 0;
	{
		struct vmctx_uregs g;

		if (ctl(pid, VMCTX_CTL_GETREGS, &g) == 0)
			fprintf(stderr, "[stuck] guest regs: rip 0x%llx rsp "
				"0x%llx fs_base 0x%llx rax 0x%llx rdi 0x%llx\n",
				(unsigned long long)g.rip,
				(unsigned long long)g.rsp,
				(unsigned long long)g.fs_base,
				(unsigned long long)g.rax,
				(unsigned long long)g.rdi);
	}
	char line[256];
	unsigned char b[16] = { 0 };
	long got;
	FILE *f;
	if(ctx_pagemap_read(pid,base,&ent))
		fprintf(stderr,"[stuck] context %d pagemap inspection failed: %s\n",
			ctx_id(pid),strerror(errno));

	fprintf(stderr, "[stuck] pid=%d addr=0x%llx err=0x%llx pagemap=0x%016llx "
		"(present=%d swapped=%d) installed=%d\n",
		ctx_id(pid), (unsigned long long)base, (unsigned long long)err,
		(unsigned long long)ent, !!(ent >> 63),
		!!(ent & ((uint64_t)1 << 62)), page_is_installed(pid, base));
	/*
	 * And THIS SIDE'S OWN RECORD of the page, which the report has never
	 * carried. The source prints its record when it refuses ("NOT-HOLDER,
	 * this side handed it over"); without the matching line from here, a
	 * page both machines say the other holds is unreadable from either log
	 * alone -- AUDIT XI 5.1 is exactly that, and it cost a day. All three
	 * are reads of state already kept, so this adds no window.
	 */
	fprintf(stderr, "[stuck] this side's record: holder=%llu (0 = the "
		"guest itself), watched=%d, the object %s the bytes\n",
		(unsigned long long)page_holder(pid, base),
		page_watched(pid, base),
		backing_has(pid, base) ? "HAS" : "does NOT have");

	f = ctx_maps_open(pid);
	while (f && fgets(line, sizeof(line), f)) {
		unsigned long a = 0, e = 0;

		if (sscanf(line, "%lx-%lx", &a, &e) == 2 && base >= a && base < e) {
			fprintf(stderr, "[stuck] VMA: %s", line);
			break;
		}
	}
	if (f)
		fclose(f);

	got = peek_at(pid, base, b, sizeof(b), "stuck");
	fprintf(stderr, "[stuck] read %ld bytes: %02x %02x %02x %02x %02x %02x "
		"%02x %02x\n", got, b[0], b[1], b[2], b[3], b[4], b[5], b[6],
		b[7]);
}

static void watch_check(memory_target pid, const char *what)
{
	int now;

	/*
	 * Always inspect the first context's copy of the page, whoever is
	 * asking. The question is who makes a page appear in the *parent*, and
	 * the suspect is work done on behalf of another context.
	 */
	(void)pid;
	if (!watch_addr || !ctx_pid)
		return;
	memory_target root=ctx_target(ctx_pid);
	now = page_present(root, watch_addr);
	if (now != watch_was) {
		/*
		 * Three states printed as three. PAGE_UNKNOWN is -1, so as a
		 * boolean it printed "present", and a watch that reports a page
		 * appearing at the moment its pagemap became unreadable sends
		 * the reader looking for whoever materialised it -- when
		 * nobody did.
		 */
		static const char * const st[] = { "absent", "present",
						   "unanswerable" };

		fprintf(stderr, "[vmremote] WATCH 0x%llx: %s -> %s (%s)\n",
			(unsigned long long)watch_addr,
			st[watch_was == PAGE_UNKNOWN ? 2 : (watch_was ? 1 : 0)],
			st[now == PAGE_UNKNOWN ? 2 : (now ? 1 : 0)], what);
		watch_was = now;
	}
	/*
	 * And what it holds, not only whether it exists. "Who made this page
	 * appear" was the old question; "who wrote this word" is the one that
	 * names a corruption, and the guest's rip when it changed is the
	 * answer.
	 */
	if (now == PAGE_PRESENT && watch_word) {
		uint64_t v = 0;

		/* All eight bytes or none: a word straddling a page this
		 * context does not have comes back half read now, and half a
		 * word compared against the last whole one reports a change
		 * that never happened. */
		if (peek(root, watch_word, &v, 8) == 8 && v != watch_val) {
			fprintf(stderr, "[vmremote] WATCH 0x%llx: %016llx -> "
				"%016llx (%s, guest rip 0x%llx)\n",
				(unsigned long long)watch_word,
				(unsigned long long)watch_val,
				(unsigned long long)v, what,
				(unsigned long long)last_ev_rip);
			watch_val = v;
		}
	}
}

/*
 * One page, followed everywhere it is touched. A page that ends up zero without
 * anybody fetching it cannot be explained from counters: what is needed is the
 * order of who asked about it and what each of them concluded.
 * Set VMREMOTE_TRACE_PAGE=0x... to follow one.
 */
static uint64_t traced_page;
static int trace_absent;
static int trace_put;		/* VMREMOTE_TRACE_PUT: every page written into the guest */
static int trace_all_faults;	/* VMREMOTE_TRACE_FAULTS: the first context's too */
static int trace_clone;		/* VMREMOTE_TRACE_CLONE: clone flags, and the namespaces in them */
static int syslog_all;		/* VMREMOTE_SYSLOG: every guest syscall, local or not */

static void tracep(uint64_t addr, memory_target pid, const char *what, long v)
{
	if (!traced_page || (addr & ~0xfffUL) != traced_page)
		return;
	fprintf(stderr, "[trace 0x%llx] pid=%d %s=%ld\n",
		(unsigned long long)traced_page, ctx_id(pid), what, v);
}

/*
 * The other end of the instrument vmhome carries: every movement of a page,
 * numbered, with this side's page table read afterwards.
 *
 * The two pulls are supposed to leave exactly one holder. Whether they do is a
 * question about two page tables, so both sides answer it the same way — by
 * reading their own — and the two logs are merged by address.
 *
 * VMR_PGLOG=1 for every page, VMR_PGLOG=0xADDR for one.
 */
static unsigned long pg_lseq;
static unsigned long n_take_kept;	/* taken away, still present here */

static void pglog_init(void)
{
	const char *e = getenv("VMR_PGLOG");

	if (!e || !*e)
		return;
	if (e[0] == '0' && (e[1] == 'x' || e[1] == 'X')) {
		pglog_page = strtoull(e, NULL, 16) & ~(uint64_t)0xfff;
		pglog_on = pglog_page ? 2 : 0;
	} else if (atoi(e)) {
		pglog_on = 1;
	}
}

/* Monotonic microseconds; see the vmhome twin. Ordering and duration are the
 * two facts a page trace exists to establish, and wall time answers neither. */
static uint64_t now_us(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (uint64_t)ts.tv_sec * 1000000ull + (uint64_t)ts.tv_nsec / 1000;
}

/*
 * The value of ONE WORD of the watched page, as a string for the page trace.
 *
 * `sum=` says two transfers differ; it cannot say which word differed or what
 * it became, and for a lock word that is the whole question -- a futex hangs on
 * a specific 32-bit value, not on a page's contents. VMR_PGLOG_OFF gives a
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
		const char *e = getenv("VMR_PGLOG_OFF");

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

/* A bounded wait that expires is a defect with an address; see the vmhome
 * twin. The timeout is the diagnosis, so it prints who, what, and where. */
static void step_timeout(const char *what, uint64_t addr, uint64_t us)
{
	void *bt[32];
	int n;

	fprintf(stderr, "[vmremote] TIMEOUT tid=%d %s 0x%llx after %llums; "
		"backtrace:\n", (int)own_tid(), what,
		(unsigned long long)addr, (unsigned long long)(us / 1000));
	n = backtrace(bt, 32);
	backtrace_symbols_fd(bt, n, 2);
}

/*
 * Pages whose bytes are in flight from home right now.
 *
 * The fault service used to hold the coherence lock across the whole remote
 * pull -- a lock held across a network round-trip. The other direction of the
 * same protocol (home GETting a page out of this side) then queued behind that
 * lock, and when the page home wanted was the one whose transfer the lock was
 * protecting, the two transfers deadlocked until a one-second bound broke the
 * cycle -- and the bound's fallback forked the page. Measured on dma1's stack
 * page: SERVE take busy=1001 us=1177163, then two live stacks.
 *
 * So the lock is released for the duration of the pull, and this table is
 * what makes that safe: the page being pulled is named here, and anyone else
 * who needs it -- a second fault on the same page, home's GET arriving at the
 * page server -- waits for the install to finish instead of double-pulling
 * (the second recall would find the shadow empty-handed and fall back to the
 * oracle's stale bytes) or answering from a page that is mid-move.
 *
 * The table is small because the in-flight set is small: one entry per fault
 * service thread at most. Overflow does not drop the marker silently -- it
 * says so and the puller keeps the lock across the pull instead, which is
 * slow and deadlock-prone but never wrong about ownership.
 */
#define PULL_MAX 16
/*
 * tid and t0 beside the address, because "a pull is in flight" is not the
 * question a stuck pull_wait raises -- "whose, and for how long" is. The
 * mutual timeout ws1 fails with is the source's 2s GET expiring against this
 * side's 500ms pull_wait on the SAME page, and without the holder named there
 * is no way to tell a slow pull from one that will never finish.
 */
static struct { uint64_t addr; int active; pid_t tid; uint64_t t0; }
	pull_tab[PULL_MAX];
static pthread_mutex_t pull_mx = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t  pull_cv = PTHREAD_COND_INITIALIZER;

/* 1 if marked; 0 if the table was full (caller must keep the lock). */
static unsigned long n_pull_overlap;

static int pull_begin(uint64_t addr)
{
	int i;

	pthread_mutex_lock(&pull_mx);
	/*
	 * INSTRUMENT (diagnosis): is this page already in flight to somebody
	 * else? pull_begin() has never asked -- it takes any free slot -- so two
	 * contexts of one address space can both be pulling the same page and
	 * both install what they got. Count it before changing anything.
	 */
	for (i = 0; i < PULL_MAX; i++)
		if (pull_tab[i].active &&
		    pull_tab[i].addr == (addr & ~(uint64_t)0xfff)) {
			n_pull_overlap++;
			break;
		}
	for (i = 0; i < PULL_MAX; i++)
		if (!pull_tab[i].active) {
			pull_tab[i].addr = addr & ~(uint64_t)0xfff;
			pull_tab[i].active = 1;
			pull_tab[i].tid = (pid_t)own_tid();
			pull_tab[i].t0 = now_us();
			pthread_mutex_unlock(&pull_mx);
			return 1;
		}
	pthread_mutex_unlock(&pull_mx);
	fprintf(stderr, "[vmremote] pull table full at 0x%llx; pulling under "
		"the lock instead\n", (unsigned long long)addr);
	return 0;
}

static void pull_end(uint64_t addr)
{
	uint64_t a = addr & ~(uint64_t)0xfff;
	int i;

	pthread_mutex_lock(&pull_mx);
	for (i = 0; i < PULL_MAX; i++)
		if (pull_tab[i].active && pull_tab[i].addr == a)
			pull_tab[i].active = 0;
	pthread_cond_broadcast(&pull_cv);
	pthread_mutex_unlock(&pull_mx);
}

/* Is this page's bytes in flight right now? For diagnosis only. */
static int pull_active(uint64_t addr)
{
	uint64_t a = addr & ~(uint64_t)0xfff;
	int i, on = 0;

	pthread_mutex_lock(&pull_mx);
	for (i = 0; i < PULL_MAX; i++)
		if (pull_tab[i].active && pull_tab[i].addr == a)
			on = 1;
	pthread_mutex_unlock(&pull_mx);
	return on;
}

/* Wait (bounded) while addr's bytes are in flight; returns ms waited. */
static int pull_wait(uint64_t addr, int max_ms)
{
	uint64_t a = addr & ~(uint64_t)0xfff;
	struct timespec dl;
	int i, hit, waited = 0;

	clock_gettime(CLOCK_REALTIME, &dl);
	dl.tv_sec += max_ms / 1000;
	dl.tv_nsec += (long)(max_ms % 1000) * 1000000L;
	if (dl.tv_nsec >= 1000000000L) {
		dl.tv_sec++;
		dl.tv_nsec -= 1000000000L;
	}
	pthread_mutex_lock(&pull_mx);
	for (;;) {
		pid_t who = 0;
		uint64_t since = 0;

		hit = 0;
		for (i = 0; i < PULL_MAX; i++)
			if (pull_tab[i].active && pull_tab[i].addr == a) {
				hit = 1;
				who = pull_tab[i].tid;
				since = pull_tab[i].t0;
			}
		if (!hit)
			break;
		waited = 1;
		if (pthread_cond_timedwait(&pull_cv, &pull_mx, &dl) != 0) {
			/*
			 * Name the pull rather than the wait. A pull that has
			 * been outstanding far longer than this wait is a pull
			 * the SOURCE has not answered -- which is the other
			 * half of the mutual timeout, and is a different defect
			 * from a pull that is merely slow.
			 */
			fprintf(stderr, "[vmremote] pull_wait gave up on "
				"0x%llx after %dms: the pull in flight was "
				"started by tid %d %llums ago\n",
				(unsigned long long)a, max_ms, (int)who,
				(unsigned long long)((now_us() - since) / 1000));
			step_timeout("pull_wait on", a,
				     (uint64_t)max_ms * 1000);
			break;
		}
	}
	pthread_mutex_unlock(&pull_mx);
	return waited;
}

/*
 * ---- shared-memory ownership channel: the receiving-side CLAIM (session 35) ----
 *
 * own.h is the substrate (REDESIGN.md "The shared-memory ownership channel").
 * Session 34 ran it as an observer at the two pull sites and measured the
 * receiving-side race directly: on hx2, ~48% of fetches found a SIBLING
 * already fetching the SAME page (pull_tab counts that as n_pull_overlap and
 * lets it race -- two copies on the wire, two installs, the lost store). This
 * is the load-bearing step: the whole service of a real guest fault is ONE
 * transition on the (as,page) entry, claimed in fault_from_home_mode() and
 * released on every exit; a sibling that finds it BUSY does not start a second
 * fetch -- it BLOCKS on the entry's futex until the first one LANDS, then
 * re-faults and finds the page (or claims the transition itself).
 *
 * Why the wrapper and not the pull sites (the 2a observer's placement):
 *   1. the observer landed right after ctx_page_pull returned, but the
 *      installs (serve_object_mapped / the pokes / obj_write) come AFTER, so a
 *      waiter woken there re-faulted INTO the claimer's install window and
 *      raced it anyway. The landing must be the install: one transition for
 *      the whole service.
 *   2. site 1's pull can answer ABSENT and fall through (after_recall) into
 *      the logic that holds site 2; a claim held across that finds
 *      BUSY-by-self at site 2. Claiming once, at the wrapper, cannot collide
 *      with itself.
 *
 * Deadlock freedom (keep this true): the waiter blocks holding NO locks -- the
 * only vec 14 / !supply_only caller is child_thread's fault loop, coh depth 0.
 * The claimer never waits on any entry (one thread services one fault).
 * Prefills (supply_only, from pg_serve_conn) use the page server's existing
 * pgstate reservation. Source GETs also reserve pgstate, so exception dispatch
 * must release the fault reservation first: source signal delivery can need
 * this same page. fault-dispatch.h enforces that boundary.
 *
 * The wait is bounded at 3s (under the kernel's 6s fault deadline) only so a
 * lost lander is a NAMED diagnosis, not a hang: on the bound the fault runs
 * UNGATED -- the pre-claim racing path -- and n_ownf_wait_timeout says so. It
 * must stay ~0; it is never an answer.
 */
static unsigned long n_ownf_claimed;	/* first faulter of a page: claimed the service */
static unsigned long n_ownf_waited;	/* a sibling found the service in flight and blocked on the landing */
static unsigned long n_ownf_woken;	/* ...and the landing woke it (it re-faults into the installed page) */
static unsigned long n_ownf_wait_timeout;/* ...and the 3s bound expired first: ran ungated (must be ~0) */
static unsigned long n_ownf_full;	/* entry table overflow (ran ungated, never invented) */
static unsigned long n_ownf_landed;	/* services released with the page PRESENT: token landed REMOTE */
static unsigned long n_ownf_aborted;	/* services released with the page still absent: transit cleared, site kept */
static unsigned long n_ownf_wake_absent;/* woken, yet no context had the page present: the claimer aborted */

#define OWNF_WAIT_US 3000000ll		/* a diagnosis bound, under the 6s kernel fault deadline */

enum { OWNF_OFF = 0, OWNF_CLAIMED = 1, OWNF_BUSY = 2 };

static pthread_once_t own_once = PTHREAD_ONCE_INIT;
static void own_do_init(void)
{
	if (own_init(-1) < 0)
		fprintf(stderr, "[vmremote] own_init failed: %s -- the "
			"ownership channel is inert this run (faults run "
			"ungated, the pre-claim racing path)\n",
			strerror(errno));
}
static void own_ready(void) { pthread_once(&own_once, own_do_init); }

/*
 * Claim the service of (ak, pk). OWNF_CLAIMED: this thread owns the transition
 * and must ownf_release() it on EVERY exit. OWNF_BUSY: a sibling's service is
 * in flight; *g is the gen to ownf_wait() on (read under the lock, so the
 * landing cannot slip past). OWNF_OFF: no channel or table full -- run ungated.
 */
static int ownf_claim(uint64_t ak, uint64_t pk, uint32_t *g)
{
	int r;

	own_ready();
	if (!own_seg)
		return OWNF_OFF;
	r = own_begin(ak, pk, OWN_T_TO_REMOTE, g);
	if (r == OWN_BEGIN_CLAIMED) {
		n_ownf_claimed++;
		return OWNF_CLAIMED;
	}
	if (r == OWN_BEGIN_BUSY)
		return OWNF_BUSY;
	n_ownf_full++;
	return OWNF_OFF;
}

/* Block until the sibling's service lands. 1 = woken by the landing (the
 * caller re-faults: the page is installed, or the transition is free to
 * claim); 0 = the bound expired (counted; the caller runs ungated). Call with
 * coh depth 0 only. */
static int ownf_wait(uint64_t ak, uint64_t pk, uint32_t g)
{
	n_ownf_waited++;
	if (own_wait(ak, pk, g, OWNF_WAIT_US)) {
		n_ownf_woken++;
		return 1;
	}
	n_ownf_wait_timeout++;
	return 0;
}

/* Release the service this thread claimed: if the page is now present in some
 * context of the address space the token LANDED here; otherwise the move did
 * not happen (a yield, a lost claim, ABSENT on both sides) and the transit is
 * cleared with the site unchanged. Either way every waiter is woken. */
static void ownf_release(uint64_t ak, uint64_t pk, int present)
{
	if (!own_seg)
		return;
	if (present) {
		own_land(ak, pk, OWN_REMOTE);
		n_ownf_landed++;
	} else {
		own_abort(ak, pk);
		n_ownf_aborted++;
	}
}

static void plog(uint64_t addr, const char *fmt, ...)
{
	static uint64_t t0;
	uint64_t t;
	va_list ap;
	char b[256];

	if (!pglog_on)
		return;
	if (pglog_on == 2 && (addr & ~(uint64_t)0xfff) != pglog_page)
		return;
	t = now_us();
	if (!t0)
		t0 = t;
	va_start(ap, fmt);
	vsnprintf(b, sizeof(b), fmt, ap);
	va_end(ap);
	/* The absolute monotonic microsecond merges this file with the
	 * source's; see the same field in vmhome's plog(). */
	fprintf(stderr, "[PG %06lu %7llu.%03llums abs=%llu tid=%d] 0x%012llx %s\n",
		__atomic_add_fetch(&pg_lseq, 1, __ATOMIC_RELAXED),
		(unsigned long long)((t - t0) / 1000),
		(unsigned long long)((t - t0) % 1000),
		(unsigned long long)t,
		(int)own_tid(),
		(unsigned long long)(addr & ~(uint64_t)0xfff), b);
}

/*
 * Which branch declined a fault, and how often.
 *
 * Declining hands the fault to the local kernel, and part XII §4 measured that
 * every `pg3` death has one and no passing run does -- 4 of 4 against 0 of 44.
 * But `n_declined` and its neighbours are a handful of counters over ten
 * `return 0` sites -- twelve once the two that answered for two different
 * reasons at once are split -- so "a fault was declined" has never named
 * *which* decline,
 * and two of these sites are deliberate and correct (the page was written into
 * the backing object first, and declining is how it gets mapped) while others
 * are the fall-through that installs zeros. PRINCIPLES §7: tag every branch
 * that can answer.
 *
 * The counter is keyed on the string literal's address, so a site added later
 * is counted by construction and cannot be forgotten in a table.
 */
static struct { const char *why; unsigned long n, known; } decl_sites[24];
static int n_decl_sites;
static unsigned long n_decl_all, n_decl_known;
static unsigned long n_chunk_retry, n_chunk_retry_ok;
/*
 * Chunk pages that landed in the backing OBJECT because this context had no
 * mapping for them yet (only the faulting page is backed when the fault is
 * reported). Each one is a round trip a later fault on that page will not
 * make: the object is the address space's memory here, and a fault on a page
 * the object holds is served by mapping it (serve_object_mapped). And the
 * count of chunk pages nothing could keep -- a loud loss, never a zero.
 */
static unsigned long n_chunk_via_object, n_chunk_lost, n_chunk_fetches;
static unsigned long n_chunk_populated;	/* neighbour pages made PRESENT by the reply */

/*
 * ...and, beside it, the one thing that says whether the decline can be a
 * fabrication at all.
 *
 * The local kernel answering a fault is not wrong in itself. For an address
 * this side has never had, its answer -- a fresh page -- is exactly what the
 * program's own kernel would have given, and the suite is full of those. It is
 * wrong for a page whose bytes exist somewhere: one this side installed, or one
 * it has lent out. The two are indistinguishable in the log until they are
 * asked apart, and this side already holds both records -- page_is_installed()
 * and page_lent_state() -- so the tag carries the answer rather than leaving it
 * to be guessed from the address.
 */
/*
 * Set when a fault was declined for a page this side KNOWS about. The caller
 * turns it into VMCTX_ACT_KILL: see the note in declined_at().
 */
/*
 * A fault for a page whose bytes exist somewhere and cannot be fetched is
 * fatal, unconditionally: counting and carrying on was measured once and is
 * not a supported mode.
 */
#define VMR_EXIT_INVENTED_PAGE 98
static unsigned long n_fatal_invent;
/*
 * The decline arriving now is "written-into-the-object": THIS fault service has
 * just put the page's current bytes into the backing object, and declining is
 * what maps them (the install could not go through the context -- a range the
 * program maps read-only, which GUP will not force-write when the mapping is
 * shared). The local kernel will serve those bytes, not zeros, so the
 * no-invention veto below must not fire: measured on netsurf, a dlopen'd
 * library's r-- segment page pulled 4096 real bytes from the source on every
 * retry, was vetoed every time on the strength of installed=1, answered DONE
 * with nothing mapped, and the watchdog killed the context at 40 (exit 131).
 */
static __thread int fresh_object_fill;
static unsigned long n_decl_object_fresh;	/* declines whose bytes this very
						 * service just put in the object */
static int fault_no_invent;
static uint64_t fault_no_invent_at;
static const char *fault_no_invent_why;
static int fault_no_invent_inst, fault_no_invent_lent;
static unsigned long n_decl_killed;
static unsigned long n_decl_shared_ok;	/* declines the object's own bytes
					 * answer: shared memory, not invention */

static int cons_covering(uint64_t as, uint64_t addr, uint64_t *start,
			 uint64_t *end, uint32_t *op, uint32_t *prot,
			 uint64_t *off, uint64_t *seq);
/*
 * A mapping change the fault's own reply should carry (applied at RESUME by
 * the faulting task itself -- a cross-task SET is refused by the kernel on
 * purpose). Defined here, used from the shared-range re-establishment, the
 * chunk populate, and declined_at's construction repair.
 */
static __thread struct { int op; uint64_t addr, len, off; unsigned prot; } fault_map;
static unsigned long n_cons_repaired;
/*
 * The VMCTX_PGS_* flags of the guest fault THIS thread is servicing right
 * now, with bit 31 as "set by the event loop" -- declined_at's repair must
 * know whether a VMA covers the faulting address IN THE FAULTING TASK, and
 * only the fault event carries that. Zero outside a fault service (the
 * page-supply threads decline through the same function), so the repair
 * cannot fire there.
 */
#define FAULT_PGS_VALID (1u << 31)
static __thread uint32_t fault_pgs;
static __thread unsigned int fault_vmaprot;

static int declined_at(const char *why, memory_target pid, uint64_t base, uint64_t addr,
		       uint64_t err)
{
	int inst = page_is_installed(pid, base);
	int lent = page_lent_state(pid, base);
	int known = inst || lent != LENT_UNSEEN;
	int i;

	/*
	 * Before declining: is this fault inside a construction the record
	 * says is ALIVE -- mapped by a reply and never legitimately vacated?
	 * Then the mapping SHOULD exist and the fault means a racing stale
	 * vacate punched it (check-then-punch is not atomic against
	 * note-then-RESUME; measured on fc1, ~3 runs in 8, always as this
	 * decline answered -14). The row is the repair: re-apply its own map
	 * on this fault's reply -- the same fault_map plumbing the shared-
	 * range re-establishment above uses -- and let the access retry into
	 * a normal serve. A row is killed by every vacate that legitimately
	 * applies over it, and a legitimate vacate always sees the row (the
	 * guest cannot name an address before the mmap's reply publishes
	 * it), so a live row is proof, not a guess. The same page repairing
	 * back-to-back means something else is wrong: fall through to the
	 * decline rather than livelock.
	 */
	{
		/*
		 * Keyed on the CONSTRUCTION (its row's stamp), not the
		 * address alone: fc1's churn thread is handed the same 8 KiB
		 * hole thousands of times a run, so "the same page again"
		 * counted repairs of unrelated incarnations and refused the
		 * fourth one an hour of mappings later (-14, exit 242). A new
		 * row is a new mapping and starts its own count; three
		 * repairs of ONE construction is still the livelock bound.
		 */
		static __thread struct { uint64_t as; uint64_t page, seq; int n; } rep_guard;
		uint64_t cs, ce, coff, cseq;
		uint32_t cop, cprot;
		uint64_t as = as_id(pid);

		/*
		 * Only when the faulting task has NO VMA at the address --
		 * asked of its own kernel AT THIS INSTANT (TRYFAULT), not of
		 * the fault event's flags: the racing punch usually lands
		 * BETWEEN the event (VMA still there, MAPPED set) and the
		 * declined fault's local service (VMA gone, -14), so the
		 * event's snapshot cannot see it. A mapped address that
		 * declines is the NORMAL object-serve path, and repairing it
		 * remapped the range on every such fault (measured: 60412
		 * repairs in one fc1 run when this gate was missing).
		 */
		struct vmctx_mem tf = { .addr = addr & ~0xfffULL,
					.len = 0, .buf = 0 };

		int have_row = (fault_pgs & FAULT_PGS_VALID) &&
			       cons_covering(as, addr, &cs, &ce, &cop, &cprot,
					     &coff, &cseq);
		/*
		 * TRYFAULT answers EFAULT for NO mapping and EACCES for a
		 * mapping this access is not allowed on -- and inside a LIVE
		 * construction row both mean the same thing: the task's
		 * mapping is not what the record says the range is. A
		 * PROT_NONE reservation (jemalloc's 1 MiB chunk, firefox)
		 * over which a SIBLING context later MAP_FIXED'd read-write
		 * leaves THIS context with the reservation's VMA -- a
		 * cross-task SET is refused by design, and this repair is the
		 * route that construction takes to every other member. Gated
		 * on EFAULT alone, the PROT_NONE case fell through to a local
		 * kernel that cannot serve it: firefox's main context died
		 * -14 reading a page its own thread had just mapped (measured
		 * at 0x7ffff771ffe0, three runs in a row).
		 */
		int trf = ctl(pid, VMCTX_CTL_TRYFAULT, &tf) < 0 ? errno : 0;
		int no_vma = have_row && (trf == EFAULT || trf == EACCES);

		/*
		 * INSTRUMENT beside the repair: a fault whose task has no VMA
		 * and NO live row is the shape the repair cannot reach --
		 * name it, because it decides where the next fix goes.
		 */
		if ((fault_pgs & FAULT_PGS_VALID) && !have_row &&
		    (trf == EFAULT || trf == EACCES)) {
			static unsigned long n_norow;

			if (++n_norow <= 8)
				fprintf(stderr, "[vmremote] NO-VMA fault 0x%llx "
					"(decline by %s, %s) has NO live "
					"construction row -- unrepairable\n",
					(unsigned long long)addr, why,
					trf == EACCES ? "a mapping without the "
					"right" : "no mapping");
		}
		if (no_vma &&
		    !(rep_guard.as == as && rep_guard.page == base &&
		      rep_guard.seq == cseq && rep_guard.n > 3)) {
			if (rep_guard.as == as && rep_guard.page == base &&
			    rep_guard.seq == cseq) {
				rep_guard.n++;
			} else {
				rep_guard.as = as;
				rep_guard.page = base;
				rep_guard.seq = cseq;
				rep_guard.n = 1;
			}
			fault_map.op   = cop;
			fault_map.addr = cs;
			fault_map.len  = ce - cs;
			fault_map.off  = coff;
			fault_map.prot = cprot;
			n_cons_repaired++;
			if (n_cons_repaired <= 8)
				fprintf(stderr, "[vmremote] REPAIR: fault 0x%llx "
					"(would decline by %s) is inside the live "
					"construction 0x%llx-0x%llx -- its mapping "
					"was punched by a racing stale vacate; "
					"re-applying the record's own map\n",
					(unsigned long long)addr, why,
					(unsigned long long)cs,
					(unsigned long long)ce);
			plog(base, "REPAIR: re-applied construction 0x%llx-0x%llx "
			     "instead of declining by %s",
			     (unsigned long long)cs, (unsigned long long)ce, why);
			return 1;
		}
	}

	for (i = 0; i < n_decl_sites; i++)
		if (decl_sites[i].why == why)
			break;
	if (i == n_decl_sites && n_decl_sites < (int)(sizeof(decl_sites) /
						      sizeof(decl_sites[0])))
		decl_sites[n_decl_sites++].why = why;
	if (i < (int)(sizeof(decl_sites) / sizeof(decl_sites[0]))) {
		decl_sites[i].n++;
		decl_sites[i].known += known;
	}
	n_decl_known += known;
	/*
	 * Said out loud, every time, with an absolute monotonic microsecond so
	 * it merges with the kernel's own line about what it then installed --
	 * dmesg's timestamp is the same clock. A decline arrives at most once
	 * or twice a pg3 run (48 passing runs had none at all), so there is
	 * nothing to rate-limit there; the suite has enough to need a cap, and
	 * the counters below carry what the cap hides.
	 */
	if (++n_decl_all <= 64)
		fprintf(stderr, "[vmremote] DECLINED-BY %s: 0x%llx (fault "
			"0x%llx err 0x%llx) abs=%llu -- the local kernel now "
			"answers this fault, for a page this side %s\n", why,
			(unsigned long long)base, (unsigned long long)addr,
			(unsigned long long)err,
			(unsigned long long)now_us(),
			!known ? "has never had (fresh memory: a page of "
				 "zeros is what the program's own kernel "
				 "would give it)" :
			inst && lent == LENT_SHADOW ?
				"INSTALLED AND HAS LENT OUT" :
			inst ? "HAS INSTALLED" :
			lent == LENT_SHADOW ? "HAS LENT TO A SHADOW" :
					      "RECORDS AS THE GUEST'S");
	plog(base, "DECLINED-BY %s (err 0x%llx) known=%d inst=%d lent=%d", why,
	     (unsigned long long)err, known, inst, lent);
	/*
	 * Declining hands the fault to the local kernel, and that is the right
	 * answer for exactly one of the states above: a page this side has
	 * NEVER had. There, zeros are what the program's own kernel would give
	 * it, and inventing them is not inventing anything.
	 *
	 * For the other three -- installed here, lent to a shadow, or recorded
	 * as the guest's -- it is the opposite. The record says the bytes exist;
	 * the local kernel does not have them and will supply a fresh page of
	 * zeros; and because the guest HAS a mapping at a stack or heap address,
	 * it does not even fault. It reads the zeros as its own memory.
	 *
	 * Measured, and this is the residual that survived the thread-pointer
	 * fix -- about one run in eleven, on the main stack, at the address pg3
	 * has always died on:
	 *
	 *   DECLINED-BY home-returned-nothing-for-the-chunk: 0x7fffffffe000
	 *     ... the local kernel now answers this fault, for a page this side
	 *     RECORDS AS THE GUEST'S
	 *
	 * with the guest's stack then reading eight words of zeros at rsp. The
	 * comment at one of these call sites calls declining "a segmentation
	 * fault", which is true only where this side has no mapping either.
	 *
	 * Ending the context on this was built and MEASURED FALSE as a rule: the
	 * suite went to 65 passed, 13 failed, and the cases it broke -- sm1,
	 * sm2, sm3, rd1 -- are the shared-memory ones, where the local kernel is
	 * the legitimate source because the bytes live in an object both
	 * machines map. "Known here" is too wide.
	 *
	 * That test is written now, and it is the one this comment asked for: a
	 * SHARED page whose bytes are IN THE OBJECT is not invented, because the
	 * object is the memory and the local kernel mapping it is the whole
	 * design. Asked of the object's own SEEK_DATA rather than of lent[],
	 * deliberately: lent[] is keyed by address and is never cleared when the
	 * program unmaps a range, so for a shared mapping at a reused address it
	 * names a loan that no longer exists -- the same stale record that broke
	 * sm1's posix-shm, and the reason the fill site above already refuses to
	 * believe it ("ownership is taken, not inferred; the object's own
	 * SEEK_DATA is").
	 *
	 * Measured on netsurf, which is what found this: 8507 declines by
	 * shared-from-own-object, every one of them reported INVENTED on the
	 * strength of lent[], and every one of them a shared page the object
	 * held. An instrument that cries wolf 8507 times hides the report that
	 * matters (PRINCIPLES §7), and this one cost a diagnosis before it was
	 * caught.
	 *
	 * What is left after the test is the private case: bytes that exist only
	 * on the other machine, replaced by zeros the guest reads as its own.
	 * n_decl_killed counts those.
	 */
	{
		off_t soff = shared_obj_off(pid, base);

		if (soff >= 0 && shared_obj >= 0 && shared_obj_has(soff)) {
			n_decl_shared_ok++;
			known = 0;
		}
	}
	/*
	 * The caller has just written the page's current bytes into the backing
	 * object and is declining precisely so the local kernel maps them. Real
	 * bytes, freshly fetched; nothing about this is invention, whatever the
	 * installed/lent record says about the page's history.
	 */
	if (fresh_object_fill) {
		n_decl_object_fresh++;
		known = 0;
		/*
		 * Consumed here, not in the per-fault reset: a page-service
		 * thread's supply_only call declines through here too and never
		 * passes service_context's reset, and a flag that outlives its
		 * decline excuses the NEXT one on the same thread.
		 */
		fresh_object_fill = 0;
	}
	if (known) {
		n_decl_killed++;
		fault_no_invent = 1;
		fault_no_invent_at = base;
		fault_no_invent_why = why;
		fault_no_invent_inst = inst;
		fault_no_invent_lent = lent;
		fprintf(stderr, "[vmremote] INVENTED 0x%llx: declined by %s, but "
			"this side %s -- the local kernel answers with zeros "
			"the guest cannot tell from its own memory\n",
			(unsigned long long)base, why,
			inst && lent == LENT_SHADOW ? "installed it AND lent it out" :
			inst ? "installed it" :
			lent == LENT_SHADOW ? "lent it to a shadow" :
					      "records it as the guest's");
	}
	return 0;
}

#define DECLINE(why) return declined_at((why), pid, base, addr, err)

/*
 * Every branch that declined, and how many times. Printed unconditionally --
 * "no faults were declined" is the statement 44 passing runs make, and a
 * counter that is only printed when it is non-zero cannot make it.
 */
static void decl_sites_report(void)
{
	int i;

	fprintf(stderr, "[vmremote] faults declined, by the branch that "
		"declined them: %lu, of which %lu were a page this side had "
		"installed or lent -- memory that exists somewhere, so what "
		"the local kernel puts there is invented\n",
		n_decl_all, n_decl_known);
	for (i = 0; i < n_decl_sites; i++)
		fprintf(stderr, "[vmremote]   %8lu (%lu known)  %s\n",
			decl_sites[i].n, decl_sites[i].known,
			decl_sites[i].why);
	/*
	 * The two halves of "known", which until now existed only as one line
	 * per page in a log thousands of lines long -- n_decl_killed was
	 * counted and never printed at all, so the run's own summary could not
	 * say whether a page had been invented. That is the failure this
	 * report was written to prevent, in the report itself.
	 */
	fprintf(stderr, "[vmremote] of the declines this side knew the page "
		"for: %lu were shared memory whose bytes ARE in the object, so "
		"the local kernel mapping them is the design; %lu were not, and "
		"for those the local kernel put zeros over memory that exists "
		"on the other machine\n",
		n_decl_shared_ok, n_decl_killed);
}

/* Present in any context of this address space, read out of the page tables. */
/* Mapping changes handed to a sibling context of the same address space. */
static unsigned long n_as_map_fanout;

/*
 * A guest's threads share one address space, so an mmap, munmap or mprotect by
 * one of them belongs to all of them. On the source that is automatic: its
 * threads share one mm. Here they do not -- every context is its own task with
 * its own mm, sharing only the backing object -- and the kernel applies a
 * reply's map_op to the task whose syscall it answered. So the change has to be
 * given to the others explicitly, or they go on mapping what the address space
 * no longer has.
 *
 * Measured on netsurf: a new thread set up its allocator arena (mmap 128MB, two
 * munmaps to trim it, one mprotect) and the MAIN thread died on the next page
 * fault its owner could not answer -- exit 242, reason 0x4e -- never having been
 * told any of it.
 *
 * as_members() is the right scope and only that scope: a forked child is NOT a
 * member (ctx_addrspace_of resolves to the parent only for a context born
 * sharing its backing object), so a fork's address space is never touched by
 * its parent's mappings.
 */
/* Each local adapter applies the source's permissions to its own context mm.
 * Protection updates include the caller and finish before its resume. SET is
 * still applied by the caller at resume; new sibling mappings remain lazy. */
static unsigned long n_as_map_not_fanned;

static int as_apply_map_siblings(memory_target pid, struct vmctx_reply *rep)
{
	memory_target as[MAX_CTX];
	int n, i;
	int protection = rep->map_op == VMCTX_MAP_PROT;

	if (rep->map_op != VMCTX_MAP_UNMAP && rep->map_op != VMCTX_MAP_ZAP && !protection) {
		n_as_map_not_fanned++;
		return 0;
	}
	n = as_members(pid, as, MAX_CTX);
	for (i = 0; i < n; i++) {
		struct vmctx_protection p = {
			.address = rep->map_addr, .length = rep->map_len,
			.protection = rep->map_prot,
		};
		if (as[i] == pid && !protection) continue;
		if (protection && as[i] != pid && ctx_execution_ended(ctx_id(as[i]), 0))
			continue;
		int applied = protection ? ctl(as[i], VMCTX_CTL_PROTECT_MM, &p) :
			ctl(as[i], VMCTX_CTL_APPLYMAP, rep);
		if (applied == 0)
			n_as_map_fanout++;
		else if (protection) {
			/* Reaping can finish after the member snapshot or ctl. */
			int error = errno;
			if ((error == ESRCH || error == EINVAL || error == EAGAIN) && as[i] != pid &&
			    ctx_wait_execution_ended(ctx_id(as[i])))
				continue;
			errno = error;
			fprintf(stderr, "[vmremote] permission update for context %d failed: %s\n",
				ctx_id(as[i]), strerror(errno));
			return -1;
		}
	}
	if (protection) rep->map_op = VMCTX_MAP_NONE;
	return 0;
}

static int as_any_present(memory_target pid, uint64_t addr)
{
	memory_target as[MAX_CTX];
	int n, i;

	n = as_members(pid, as, MAX_CTX);
	for (i = 0; i < n; i++)
		if (page_present(as[i], addr) == 1)
			return 1;
	return 0;
}

/*
 * Check what the loan table claims against what the page tables say.
 *
 * The whole model rests on one invariant, and nothing has ever tested it:
 *
 *   lent exclusive  -> no context of the address space has the page present,
 *                      so the guest cannot read or write it without faulting;
 *   lent shared     -> every context that has it has it read-only, so a write
 *                      by the guest arrives here as a protection fault.
 *
 * Only the first is checked. /proc/pid/pagemap reports whether a page is
 * present and not whether it may be written, so the shared half cannot be
 * tested from here without a kernel op -- and the exclusive half is the one
 * that matters: it is violated exactly when the guest can write a page the
 * source believes is the only copy.
 *
 * Everything else in this file is an argument that the invariant holds. Those
 * arguments have been wrong repeatedly, and the failures they produce -- a
 * buffer holding the previous call's contents -- are exactly what a broken
 * invariant looks like. So ask the page tables instead of arguing.
 *
 * VMREMOTE_CHECK_COHERENCE turns it on; it costs a pagemap read per context per
 * check, which is far too much for an ordinary run and nothing at all for a
 * debugging one.
 */
static int check_coherence;
static unsigned long n_inv_violations;
/*
 * The denominator. "0 violations" is worth nothing without "of N loans
 * examined" beside it -- this file has now twice reported a zero that meant
 * "the check never ran" (the clobber detector's chunk-install gap was the
 * other), and the rule is written down in several places precisely because it
 * keeps being learned again.
 */
static unsigned long n_coh_examined, n_coh_sweeps, n_coh_ctx_reads;

static void coh_check(uint64_t identity, uint64_t page, uint64_t owner, int shared,
		      const char *when)
{
	memory_target as[MAX_CTX];
	int n, i;

	if (!check_coherence || !owner)
		return;
	n = mm_members(identity, as, MAX_CTX);
	n_coh_examined++;
	for (i = 0; i < n; i++) {
		int st = page_present(as[i], page);

		n_coh_ctx_reads++;
		if (st != PAGE_PRESENT)
			continue;		/* absent: stronger than either */
		if (!shared && n_inv_violations++ < 24)
			fprintf(stderr, "[coherence] VIOLATION %s: 0x%llx is "
				"lent WHOLE to shadow %llu, and context %d "
				"still has it present\n", when,
				(unsigned long long)page,
				(unsigned long long)owner, ctx_id(as[i]));
	}
}


/*
 * Walk every live loan and check it. Called round-robin from the page service
 * so the table is covered without any one request paying for all of it.
 */
static void coh_sweep(void)
{
	size_t n, count;
	struct loan *loans;

	if (!check_coherence)
		return;
	n_coh_sweeps++;
	/*
	 * Under the coherence lock, so no page can be mid-move while it is
	 * examined. Without it the sweep races the page service and reports the
	 * ordinary transient between "the page has been poked into the guest"
	 * and "the loan has been cleared" -- which is not a violation, and
	 * would have been read as one.
	 */
	coh_enter();
	/*
	 * The whole table, every time. A round-robin slice looked cheaper and
	 * was worthless: the table has 131072 slots, a run has about a hundred
	 * live loans scattered across them, and a hundred requests sampling
	 * sixty-four slots each covers almost none of them. It reported zero
	 * violations without having looked at a single live entry.
	 */
	loans = loan_snapshot(0, &count);
	for (n = 0; n < count; n++)
		coh_check(loans[n].as, loans[n].page, loans[n].owner,
			  loans[n].shared, "on sweep");
	free(loans);
	coh_leave();
}


static unsigned long n_present_unknown;
static unsigned long n_present_gone;

static unsigned long n_declined_present;

/* Presence inspection never faults in guest bytes. Open/read/close is one
 * local operation; a cached native-MM descriptor cannot name an execution
 * object's binding epoch. Failure is UNKNOWN unless the retained task itself
 * is proven ended. ENOENT from a proc lookup alone is not that proof. */
static int page_present(memory_target pid, uint64_t addr)
{
	uint64_t ent;
	int result;
	if(!ctx_pagemap_read(pid,addr,&ent))
		result=!!(ent&((UINT64_C(1)<<63)|(UINT64_C(1)<<62)));
	else {
		int saved=errno;
		if(ctx_execution_ended(ctx_id(pid),0)) {
			__atomic_add_fetch(&n_present_gone,1,__ATOMIC_RELAXED);
			result=PAGE_ABSENT;
		} else {
			unsigned long count=__atomic_add_fetch(&n_present_unknown,1,__ATOMIC_RELAXED);
			if(count<=8 || (traced_page && (addr&~UINT64_C(0xfff))==traced_page))
				fprintf(stderr,"[vmremote] pagemap inspection failed for ctx %d "
					"(0x%llx): %s -- presence UNKNOWN\n",ctx_id(pid),
					(unsigned long long)addr,strerror(saved));
			result=PAGE_UNKNOWN;
		}
		errno=saved;
	}
	tracep(addr,pid,"page_present",result);
	return result;
}

static int sock_read_all(int fd, char *p, size_t n);

static int pg_rw(int fd, void *buf, size_t n, int wr)
{
	char *p = buf;

	if (!wr)
		return sock_read_all(fd, p, n);
	while (n) {
		/* A dead peer fails its operation, not the entire monitor. */
		ssize_t r = send(fd, p, n, MSG_NOSIGNAL);

		if (r == 0)
			return -1;
		if (r < 0) {
			if (errno == EINTR)
				continue;
			return -1;
		}
		p += r;
		n -= (size_t)r;
	}
	return 0;
}

/*
 * The page's bytes, from whichever context of this address space has them.
 *
 * A request names one context, and a page of a shared address space need not be
 * mapped in that one: a thread's first syscall hands the source a pointer to a
 * buffer its creator filled and it has not touched itself. Asking only the
 * named context answers "absent" for memory the address space plainly holds,
 * and the source then fills the page from the oracle — which for a thread's
 * stack holds nothing at all, so the program's output arrives as NULs.
 *
 * Returns 1 with the bytes in buf, or 0 if no context of this address space has
 * anything at that address.
 */
static int page_read_as(memory_target ctx, uint64_t addr, void *buf)
{
	memory_target as[MAX_CTX];
	int n = as_members(ctx, as, MAX_CTX), i, bf;

	for (i = 0; i < n; i++)
		if (peek(as[i], addr, buf, VMR_PG_SIZE) == (long)VMR_PG_SIZE)
			return 1;
	/* Settlement can leave the sole shared copy in its object with no
	 * context PTE. Its stable object/offset slot, rather than this MM's
	 * private object at the same VA, supplies that copy. A shared hole
	 * must not fall through to unrelated private storage. The caller's
	 * page reservation and ownership transition still govern delivery. */
	if (shared_range_has(ctx, addr)) {
		off_t off = shared_obj_off(ctx, addr);

		if (off >= 0 && shared_obj_has(off) &&
		    pread(shared_obj, buf, VMR_PG_SIZE, off) == (ssize_t)VMR_PG_SIZE) {
			n_from_own_backing++;
			tracep(addr, ctx, "pgservice:from-shared-object", 1);
			return 1;
		}
		return 0;
	}
	/*
	 * Nobody has it mapped, which is not the same as nobody having it.
	 *
	 * An address space's memory is its backing object, and a context's page
	 * table is a view of that object rather than the thing itself. Taking a
	 * page away from a context clears its PTE and leaves the bytes in the
	 * object -- that is what makes the context fault again -- so between the
	 * take and the next fault every context reports the page absent while the
	 * address space plainly holds it.
	 *
	 * Answering "absent" there hands the decision to the source, which fills
	 * the page from the program as it was *loaded*: zeros, for anything the
	 * program wrote itself. links formatted 408 bytes of a page into its own
	 * .bss and handed the address to write(2); the first 224 bytes were on a
	 * page a context still had and arrived, and the rest was on a page that
	 * had been taken, so the file ended in 184 NULs with the syscall
	 * reporting 408 bytes written.
	 *
	 * The object is read directly rather than through a context, because
	 * reading it through one would fault the page back in and undo the take.
	 */
	bf = backing_find(ctx);
	if (bf >= 0 && backing_has(ctx, addr) &&
	    pread(bf, buf, VMR_PG_SIZE, (off_t)(addr & ~0xfffUL)) ==
	    (ssize_t)VMR_PG_SIZE) {
		n_from_own_backing++;
		tracep(addr, ctx, "pgservice:from-own-object", 1);
		return 1;
	}
	/*
	 * Neither a context's page tables nor the object could produce the
	 * bytes, so this side has nothing to send.
	 *
	 * This used to answer with a page of zeros and report success, on the
	 * reasoning that a context whose pagemap did not say ABSENT must hold
	 * the page and disclaiming it would let the source substitute the
	 * oracle's copy. Both halves were wrong. "Not ABSENT" includes
	 * PAGE_UNKNOWN -- an unanswerable pagemap read, which after an execve is
	 * most of them -- so the test was passed by pages nobody had any
	 * evidence about at all. And what the caller does with a 1 is answer
	 * status 0 and send those four thousand zeros to the shadow, which
	 * writes them into the syscall's buffer as the guest's data: bytes this
	 * machine invented, presented as the program's memory.
	 *
	 * Answering 0 is what ABSENT is for. It tells the source to fall back
	 * to its own copy, which may be stale but is at least a record of
	 * something the program wrote, and it is the source that owns the
	 * program. There is no case here that is known to be zero: a page the
	 * address space genuinely holds as zeros reads back as zeros through
	 * the peek or the object above, and never reaches this line.
	 */
	return 0;
}

/*
 * Write bytes into an address space, through whichever of its contexts takes
 * them.
 *
 * A page coming back names the context that borrowed it, and that context can
 * be gone by the time it arrives: a thread's last act is to have the source
 * clear the word its creator is joining on, and that write is sent after the
 * thread has exited. Writing only into the named context loses it, silently,
 * and the joiner waits for ever.
 *
 * The contexts of one address space share a backing object, so a write into any
 * of them is a write all of them see. The named one is tried first: it is the
 * one the source was talking about, and it is usually alive and already has the
 * page.
 */
static long poke_as(memory_target ctx, uint64_t addr, const void *buf, uint64_t len)
{
	memory_target as[MAX_CTX];
	int n = as_members(ctx, as, MAX_CTX), i, bf;
	long r = -1;

	for (i = 0; i < n; i++) {
		r = poke(as[i], addr, buf, len);
		if (r > 0)
			return r;
	}
	/*
	 * No context could take it, and for a page the guest has never touched
	 * that is the normal case rather than an error: a context is given
	 * backing for an address when it faults there, so an address it has
	 * never reached exists in no context's page tables and POKE has nowhere
	 * to put the bytes. It fails, and the write is lost without a word.
	 *
	 * That is most of a program's input. curl's buffer is filled by the
	 * *source* -- recv writes into the shadow's mirror -- so the guest never
	 * touches those pages at all, and every page of a 19.5 KB body past the
	 * first came back as NULs the moment the source stopped keeping its own
	 * copy of them.
	 *
	 * Write the object instead. A context's memory *is* its backing object
	 * with the address as the offset, so this needs no context to have
	 * mapped anything: whenever one first faults there, the kernel maps the
	 * object and the bytes are already in it.
	 */
	bf = backing_find(ctx);
	if (bf >= 0) {
		ssize_t w = pwrite(bf, buf, (size_t)len, (off_t)addr);

		/*
		 * Who writes the object, and what. The traced page is served
		 * from this object when it holds data, so a write of zeros
		 * here is indistinguishable afterwards from the guest's own
		 * memory -- and site10 was measured serving exactly that.
		 */
		if (traced_page && (addr & ~0xfffUL) == traced_page) {
			unsigned char preview[8] = {0};
			if (len) memcpy(preview, buf, len < sizeof(preview) ? (size_t)len : sizeof(preview));
			fprintf(stderr, "[vmremote] OBJWRITE fd=%d 0x%llx+%llu "
				"-> %zd first8=%02x%02x%02x%02x%02x%02x%02x%02x\n",
				bf, (unsigned long long)addr,
				(unsigned long long)len, w,
				preview[0],
				preview[1],
				preview[2],
				preview[3],
				preview[4],
				preview[5],
				preview[6],
				preview[7]);
		}

		if (w == (ssize_t)len) {
			n_put_to_object++;
			return (long)w;
		}
	}
	return r;
}

static int mm_page_read(uint64_t identity,uint64_t address,void *bytes)
{
	memory_target members[MAX_CTX];
	int count=mm_members(identity,members,MAX_CTX);
	for(int i=0;i<count;i++)
		if(peek(members[i],address,bytes,VMR_PG_SIZE)==VMR_PG_SIZE)return 1;
	return mm_backing_has(identity,address) &&
		pread(mm_backing_find(identity),bytes,VMR_PG_SIZE,
			(off_t)(address&~UINT64_C(4095)))==VMR_PG_SIZE;
}

/*
 * Serve one page-channel connection. Returns 1 if the connection was adopted
 * for recalls and must be left open, 0 when it is finished with.
 */
static uint64_t forked_from(uint64_t as);
static int cow_was_given(uint64_t as, uint64_t page);
static int cow_is_protected(uint64_t as, uint64_t page);
static void cow_give_note(uint64_t as, uint64_t page);
static int cow_give_from_chain(uint64_t copy_as, uint64_t base);

static int pg_serve_conn(int c)
{
	static __thread char page[VMR_PG_SIZE];	/* one connection per thread */

	unsigned long nserved = 0;

	for (;;) {
		struct vmr_pgreq rq;
		struct vmr_pgrsp rs = { .magic = VMR_PG_MAGIC, .status = 0 };
		long r;

		nserved++;
		coh_sweep();

		if (pg_rw(c, &rq, sizeof(rq), 0) < 0) {
			/*
			 * The peer closed, or the read failed. Which of the two
			 * it is has to be said out loud: "the shadow closed"
			 * and "this end failed" send the investigation to
			 * opposite machines, and the quiet version of this line
			 * sent it to the wrong one for a day.
			 */
			int e = errno;

			fprintf(stderr, "[vmremote] page service: fd %d gives "
				"up after %lu requests: %s\n", c, nserved,
				e ? strerror(e) :
				"end of file -- the shadow closed its end");
			return 0;
		}
		if (rq.magic != VMR_PG_MAGIC) {
			/*
			 * Not a request at all -- the stream is out of step.
			 *
			 * That happens when the two sides disagree about the
			 * shape of a reply: an operation whose answer carries a
			 * page on one side and not the other leaves exactly one
			 * page of unread bytes behind, and everything after it is
			 * read at the wrong offset. Dropping the channel is
			 * right, but doing it silently is what made this look
			 * like a page that could not be supplied, in a context
			 * that had not even forked yet.
			 */
			fprintf(stderr, "[vmremote] page service: stream out of "
				"step -- expected a request, got magic %x op %u "
				"addr 0x%llx; dropping the channel\n",
				rq.magic, rq.op, (unsigned long long)rq.addr);
			return 0;
		}

        /* v20 accepts only the transfer operations emitted by the source.
         * Legacy unreserved writes/upgrades and unnamed-MM requests cannot
         * reach storage or ownership mutations. */
        if(rq.op!=VMR_PG_GET && rq.op!=VMR_PG_GETS && rq.op!=VMR_PG_COWBREAK) {
            fprintf(stderr,"vmremote: unsupported page operation %u\n",rq.op);
            return 0;
        }
        if((rq.addr & (VMR_PG_SIZE-1)) || rq.addr>INT64_MAX-VMR_PG_SIZE ||
           (rq.op==VMR_PG_COWBREAK ? rq.len>1 : rq.len!=0)) {
            fputs("vmremote: invalid page request range/flags\n",stderr);return 0;
        }
        memory_target who=ctx_observe_source(&rq.target);
        if(!who) {
            fprintf(stderr,"vmremote: page request has unknown/contradictory binding context=%llu mm=%llu epoch=%llu: %s\n",
                (unsigned long long)rq.target.context,(unsigned long long)rq.target.mm,
                (unsigned long long)rq.target.epoch,strerror(errno));
            return 0;
        }
        rs.target=rq.target;rs.addr=rq.addr;rs.op=rq.op;

		/* Every active page request owns the local transition before any
		 * record or storage mutation, including COW and ABSENT replies. The
		 * reservation outlives response stamping and socket transmission.
		 * On contention no page bytes or ownership facts are published. */
		struct pg_request_claim transfer
			__attribute__((cleanup(pg_request_release))) = {0};
		int gets = rq.op == VMR_PG_GET || rq.op == VMR_PG_GETS || rq.op == VMR_PG_COWBREAK;
		if (gets) {
			int prior = pg_try_claim(as_id(who), rq.addr,
				rq.op == VMR_PG_GETS ? PST_DOWNGRADING : PST_UNINSTALLING,
				&transfer.claim);
			if (prior < 0) {
				rs.status = VMR_PG_INFLIGHT;
				rs.why = VMR_INFLIGHT_PULL;
				rs.gen = pgen_get(who, rq.addr);
				memset(page, 0, sizeof(page));
				if (pg_rw(c, &rs, sizeof(rs), 1) < 0 ||
				    pg_rw(c, page, sizeof(page), 1) < 0) return 0;
				continue;
			}
			transfer.final = PST_INVALID;
		}

		/*
		 * A shadow that only wants to read a page. Give it a copy and
		 * leave the guest one, write-protected — neither may write it,
		 * so both may hold it, and a page both machines only read stops
		 * moving altogether.
		 */
		if (rq.op == VMR_PG_UPGRADE) {
			coh_enter();
			if (page_shared(who, rq.addr)) {
				pthread_mutex_lock(&ctl_lock);
				page_take_away(who, rq.addr, page);
				mark_handed_over(who, rq.addr);
				pthread_mutex_unlock(&ctl_lock);
				n_upgraded++;
			} else {
				/*
				 * The shadow says it holds this page for reading
				 * and wants to write it, and this side does not
				 * agree that it holds it at all -- its shared
				 * copy was taken back, by a recall this upgrade
				 * raced, and the guest has the page again.
				 *
				 * The line below records the loan regardless,
				 * which then says the shadow owns whole a page
				 * the guest is free to write. Whether that ever
				 * happens is the question this counts; in the
				 * runs measured so far it has not.
				 */
				n_upgrade_stale++;
				if (n_upgrade_stale <= 8)
					fprintf(stderr, "[vmremote] shadow %llu "
						"asks to upgrade 0x%llx, which "
						"it does not hold shared here "
						"(holder %llu); granting it "
						"would leave both sides "
						"owning the page\n",
						(unsigned long long)rq.owner,
						(unsigned long long)rq.addr,
						(unsigned long long)
						page_holder(who, rq.addr));
			}
			page_lend_mode(who, rq.addr, rq.owner, 0);
			coh_leave();
			if (pg_rw(c, &rs, sizeof(rs), 1) < 0) {
				/*
				 * Leaving here closes the channel, and the shadow
				 * on the other end sees end-of-file the next time
				 * it asks for a page -- with no indication that
				 * the cause was several requests earlier. Say
				 * which request gave up and why.
				 */
				fprintf(stderr, "[vmremote] page service: giving up "
					"on the channel while answering op %u for "
					"0x%llx (ctx %llu): %s\n", rq.op,
					(unsigned long long)rq.addr,
					(unsigned long long)rq.target.context,
					strerror(errno));
				return 0;
			}
			continue;
		}
		/*
		 * The source says this address space was copied from another
		 * and is still owed this page. Break copy-on-write here and
		 * hand it the copy's page -- which is a hand-over, not a
		 * duplicate: the bytes go there and this side keeps nothing for
		 * the copy, so the page exists once, on the machine that asked.
		 * The original keeps its own, which is what a break makes.
		 *
		 * Refused when nothing is owed -- the copy has been given this
		 * page before, or its object already holds one -- because the
		 * original's page may have been written since, and then its
		 * bytes are not older, they are wrong.
		 */
		if (rq.op == VMR_PG_COWBREAK) {
			uint64_t par = who ? forked_from(as_id(who)) : 0;
			uint64_t anc = 0;
			int ok = 0, hops = 0;

			coh_enter();
			/*
			 * The same guards cow_give_page() makes, for the same
			 * reasons: nothing is owed to a copy that has been
			 * given this page or has one of its own. But the
			 * ancestor that can vouch for the page is the NEAREST
			 * one still under the fork's protection, not
			 * necessarily the parent: a fork of a fork inherits
			 * pages its parent never held -- the parent itself
			 * still inherits them -- and an undiverged page has
			 * ONE set of inherited bytes for the whole subtree,
			 * because the first store anywhere comes through the
			 * break and gives first. Measured on bwrap (glycin's
			 * probe, the netsurf wall, session 42): the payload
			 * -- fork of the sandbox child, itself a fork of
			 * bwrap main -- was refused by its parent, served
			 * libc's .data from the pristine file, and died in
			 * __libc_fork's child bookkeeping writing through a
			 * NULL list head that main() had initialized long
			 * before either fork.
			 */
			if (par && !cow_was_given(as_id(who), rq.addr) &&
			    !backing_has(who, rq.addr))
				for (anc = par; anc;
				     anc = forked_from(anc), hops++)
					if (cow_is_protected(anc, rq.addr) &&
					    mm_page_read(anc, rq.addr, page)) {
						ok = 1;
						break;
					}
			/*
			 * rq.len == 1 is the source ESCALATING: its own
			 * routes (the child's kernel COW, its pagemap-walked
			 * ancestor chain) all came up empty, so the strict
			 * gate's "ask the source instead" points at a machine
			 * that has already said no. Serve the nearest
			 * ancestor page this side can READ, protect mark or
			 * not -- bytes-current-at-break (cow_give_page_mode's
			 * comment carries the argument) -- because the only
			 * remaining alternative is the zeros a fork copy is
			 * never owed.
			 */
			if (!ok && rq.len == 1 && par &&
			    !cow_was_given(as_id(who), rq.addr) &&
			    !backing_has(who, rq.addr))
				for (anc = par, hops = 0; anc;
				     anc = forked_from(anc), hops++)
					if (mm_page_read(anc, rq.addr, page)) {
						ok = 1;
						if (++n_cow_relaxed_given <= 8)
							fprintf(stderr,
								"[vmremote] 0x%llx for "
								"address space %d: "
								"LAST-RESORT give from "
								"ancestor %d (%d hop(s), "
								"no protect mark) -- the "
								"source escalated after "
								"its own routes failed\n",
								(unsigned long long)rq.addr,
								(int)as_id(who),
								(int)anc, hops);
						break;
					}
			if (ok) {
				cow_give_note(as_id(who), rq.addr);
				n_cow_given_to_home++;
				if (hops && ++n_cow_chain_given <= 8)
					fprintf(stderr, "[vmremote] 0x%llx for "
						"address space %d: given by "
						"ancestor %d, %d hop(s) above "
						"the parent, which never held "
						"it\n",
						(unsigned long long)rq.addr,
						(int)as_id(who), (int)anc,
						hops);
			}
			/*
			 * Nothing owed by an ancestor -- because the copy
			 * ALREADY HAS the page. The source asks for a break
			 * only after its GET was answered "nothing here", and
			 * between that answer and this ask the original's
			 * write-prep give landed the page in the copy's
			 * object (or the copy's own guest faulted it in from
			 * there). Refusing now is the one answer that is
			 * wrong for every reason on the list: the source
			 * fills the page with zeros, or serves a private
			 * file page from the pristine file -- libc's
			 * main_arena with every bin NULL, and the copy dies
			 * in _int_malloc reading bin->bk->fd at 0x18.
			 * Measured on progs pipes (~1 in 10 at session 46's
			 * speed): "already given 1, copy's object has it 1",
			 * then the dash subshell's segmentation fault.
			 *
			 * The page is the copy's own, so it is served the
			 * way its GET would have served it a moment later:
			 * this request becomes that GET -- the take, the
			 * loan record and the retained copy all happen in
			 * the one place they are defined.
			 */
			if (!ok && page_read_as(who, rq.addr, page)) {
				coh_leave();
				if (++n_cow_own_served <= 8)
					fprintf(stderr, "[vmremote] COWBREAK for "
						"0x%llx (as %d): the copy already "
						"holds the page (given since the "
						"source's GET was answered); "
						"served as that GET\n",
						(unsigned long long)rq.addr,
						(int)as_id(who));
				plog(rq.addr, "COWBREAK: the copy holds the page "
				     "itself -- re-dispatched as GET");
				rq.op = VMR_PG_GET;
				goto cow_own_get;
			}
			coh_leave();
			rs.status = ok ? 0 : VMR_PG_ABSENT;
			plog(rq.addr, "COWBREAK for ctx %d from %d -> %s",
			     ctx_id(who), (int)(ok ? anc : par),
			     ok ? "the copy's page" : "nothing owed");
			/*
			 * WHY it was refused, said out loud: "nothing owed"
			 * is four different answers, and the source fills the
			 * page with zeros on every one of them. tests/mf2 (a
			 * fork made by a worker thread) dies on exactly such a
			 * refusal of a page the parent plainly holds.
			 */
			if (!ok && ++n_cow_refused <= 16)
				fprintf(stderr, "[vmremote] COWBREAK refused for "
					"0x%llx (ctx %d, as %d, parent as %d): "
					"parent %s, fork-protected %d, already "
					"given %d, copy's object has it %d, "
					"parent readable %d\n",
					(unsigned long long)rq.addr, ctx_id(who),
					(int)as_id(who), (int)par,
					par ? "known" : "UNKNOWN",
					par ? cow_is_protected(par, rq.addr) : -1,
					cow_was_given(as_id(who), rq.addr),
					backing_has(who, rq.addr),
					par ? mm_page_read(par, rq.addr, page) : -1);
			/*
			 * A page follows either way -- the same framing rule
			 * every GET-shaped answer obeys (pg_get_op on the
			 * other side reads header + page unconditionally).
			 * This refusal used to send the header alone, and the
			 * asker then sat in read() for a page that was never
			 * coming until its 4 s socket bound fired: every
			 * fatal fork child's exception carried a COWBREAK of
			 * the scratch page (fresh in the child, never the
			 * parent's) and paid the whole 4 s -- pf3 12.5 s for
			 * three children, pf5 timing out the suite. Zeros,
			 * because a refusal's body must not leak whatever the
			 * probe left in the buffer.
			 */
			if (!ok)
				memset(page, 0, VMR_PG_SIZE);
			if (pg_rw(c, &rs, sizeof(rs), 1) < 0 ||
			    pg_rw(c, page, VMR_PG_SIZE, 1) < 0) {
				fprintf(stderr, "[vmremote] page service: giving "
					"up on the channel while answering a "
					"copy-on-write break for 0x%llx (ctx "
					"%llu): %s\n",
					(unsigned long long)rq.addr,
					(unsigned long long)rq.target.context,
					strerror(errno));
				return 0;
			}
			continue;
		}
cow_own_get:
		if (rq.op == VMR_PG_GET || rq.op == VMR_PG_GETS) {
			if (!who) {
				memset(page, 0, sizeof(page));
				if (pg_rw(c, &rs, sizeof(rs), 1) < 0 ||
				    pg_rw(c, page, VMR_PG_SIZE, 1) < 0) {
					fprintf(stderr, "[vmremote] page service: "
						"giving up on the channel while "
						"answering op %u for 0x%llx "
						"(ctx %llu): %s\n", rq.op,
						(unsigned long long)rq.addr,
						(unsigned long long)rq.target.context,
						strerror(errno));
					return 0;
				}
				continue;
			}
			/*
			 * Only answer for pages that exist. Reading an absent
			 * one would materialise it as zeros — permanently, so
			 * the guest never faults there again and runs whatever
			 * zero means at that address (0x00 0x00 decodes to a
			 * store through %rax, which is how this last showed
			 * up: a write to address 0 from the middle of libc).
			 */
			tracep(rq.addr, who, "pgservice:GET", 0);
			/*
			 * Reading the page is the test, not the pagemap.
			 *
			 * A page mapped here is this side's to supply: the source
			 * may not answer from its own copy, because the guest may
			 * have written this one and the source's copy is stale or
			 * was never filled. Answering ABSENT hands the decision
			 * back to the source, which fills the page with what it
			 * has -- zeros -- and the program's own data disappears.
			 * A write(2) of forty bytes arrived as forty NULs that
			 * way.
			 *
			 * The pagemap bit used to stand in for "the guest has
			 * something here". It cannot any more: backing is a
			 * shared object now rather than private anonymous memory,
			 * and a page of it can hold the guest's data while that
			 * bit says nothing useful about this process's view. So
			 * try to read it, and only call it absent if that fails
			 * *and* the address is positively not the guest's.
			 *
			 * As a *condition*, not as a branch of its own. An
			 * earlier version tested the read first and returned the
			 * bytes straight away when it succeeded, which skipped
			 * everything below -- including recording that the page
			 * had been lent and giving up this side's right to it.
			 * Both sides then believed they owned it and their copies
			 * drifted apart, which is a program printing its message
			 * followed by rubbish.
			 */
			/*
			 * Asked of the whole address space, not of the context
			 * the request names — see page_read_as(). A thread's
			 * buffers were written by whoever created it, so the
			 * named context is precisely the one that has not
			 * touched them.
			 */
			plog(rq.addr, "GET asked by owner %llu ctx=%d "
			     "present=%d backing=%d",
			     (unsigned long long)rq.owner, ctx_id(who),
			     as_any_present(who, rq.addr),
			     backing_has(who, rq.addr));
			/*
			 * ABSENT is a verdict, and it is only allowed under the
			 * coherence lock. The first read runs unlocked -- fine
			 * when it finds the page -- but "nothing here" can be a
			 * page in flight: the fault service pulls a page from
			 * home and pokes it into the guest under coh_enter(),
			 * and between the pull and the poke every context reads
			 * absent while the bytes sit in that thread's buffer.
			 *
			 * Measured (dma1, timed trace): home's write fault GETs
			 * the page in exactly that window, is told ABSENT, and
			 * its local kernel zero-fills the address for good --
			 * from then on every round of the test reads zeros and
			 * the run degrades to 200 bad in 200. The retry under
			 * the lock waits out the in-flight transfer and asks
			 * again; only a no that survives the lock is a no.
			 */
			uint64_t gt0 = now_us(), gt1 = 0, gt2 = 0, gt3 = 0;
			{
				int have = page_read_as(who, rq.addr, page);
				/*
				 * Has this side already given the asker this
				 * page? page_read_as() cannot tell, and that is
				 * the defect this answers.
				 *
				 * Its fallback reads the backing object
				 * directly, on purpose: a take clears a
				 * context's PTE and leaves the bytes in the
				 * object, so between a take and the next fault
				 * every context reports the page absent while
				 * the address space plainly holds it. But the
				 * bytes stay in the object after a hand-over
				 * too. "The object has bytes" is therefore true
				 * both when this side holds the page and when it
				 * has already handed it over, and only the loan
				 * record separates the two.
				 *
				 * Without asking it, a GET for a page the asker
				 * already holds read the object, believed it
				 * could supply the page, and went on to take it
				 * from local contexts that had correctly given
				 * it up. The take fails, and the failure is
				 * fatal by design -- the FATAL below refuses to
				 * answer ABSENT over real memory. Measured:
				 *
				 *   FATAL-WHO: asked=142031 as=142031 members=2
				 *     ctx 142031 alive=1 present=0 installed=1
				 *     ctx 142034 alive=1 present=0 installed=0
				 *     as_any_present=0 in_flight=0
				 *     installed_any=1 holder=142019 watched=0
				 *
				 * -- the holder is 142019, which is neither
				 * context: it is the shadow doing the asking.
				 * Not a race (in_flight=0, and the members are
				 * right); a page this side knowingly does not
				 * have.
				 *
				 * The honest answer is the one this protocol
				 * already has a name for in the other
				 * direction: you hold it, so your copy is the
				 * authority. That is what have = 0 says here.
				 */
				if (have && rq.owner &&
				    page_holder(who, rq.addr) == rq.owner) {
					/*
					 * The record says the asker holds it --
					 * but does a context of the guest's own
					 * address space have it PRESENT? Then the
					 * guest pulled it back and is the live
					 * writer; the loan record is stale, and
					 * page_read_as() above read the guest's
					 * CURRENT bytes from that context's PTE.
					 * Overriding to have=0 here would discard
					 * them and serve the retained copy, stale
					 * by every write since the last capture --
					 * hx2's lost store (a writer reading a
					 * value tens of thousands of rounds old).
					 *
					 * The override is for the OTHER shape,
					 * which the FATAL-WHO photograph names:
					 * as_any_present=0, the bytes only in the
					 * OBJECT, which holds them equally whether
					 * this side still has the page or handed
					 * it over -- there the record is the only
					 * discriminator and the asker's own copy
					 * is the current one. So gate the override
					 * on no context having the page: present
					 * means the guest, not the asker, holds
					 * the newest bytes.
					 */
					if (as_any_present(who, rq.addr)) {
						n_get_theirs_but_present++;
						plog(rq.addr, "GET: record says "
						     "handed to the asker (%llu) "
						     "but a guest context HAS the "
						     "page present -- the loan is "
						     "stale, serving the guest's "
						     "current bytes not the "
						     "retained copy",
						     (unsigned long long)rq.owner);
					} else {
						n_get_already_theirs++;
						plog(rq.addr, "GET for a page "
						     "already handed to the asker "
						     "(%llu); its own copy is the "
						     "current one",
						     (unsigned long long)rq.owner);
						have = 0;
					}
				}

				if (!have) {
					uint64_t ab0 = pglog_on ? now_us() : 0;

					/*
					 * A page mid-pull from home is absent
					 * here only for the length of the
					 * transfer; wait that out first, then
					 * re-ask under the lock, which also
					 * waits out an install in progress.
					 * Only a no that survives both is a no.
					 */
					pull_wait(rq.addr, 500);	/* strictly inside the peer's 4s socket bound: equal nested budgets made the outer expire whenever this fired -- pg3 3/3, SIGBUS at the fault deadline */
					gt1 = now_us();
					coh_enter();
					gt2 = now_us();
					have = page_read_as(who, rq.addr, page);
					/*
					 * A FORK COPY with nothing of its own at
					 * this address inherits the page from
					 * the nearest ancestor still under the
					 * fork's protection -- the same give the
					 * original's next write would make
					 * (write-prep), made now because the
					 * copy asked first. Without it the copy
					 * was answered ABSENT while its parent
					 * held the page a few lines away, and
					 * the source's own routes then raced the
					 * give (COWBREAK above). The fault path
					 * here has done this since session 42
					 * (PULL_COWBREAK); the page service, the
					 * route a copy's SOURCE-side fault takes,
					 * had not. Strict mode only: an ancestor
					 * without the protect mark may hold bytes
					 * newer than the fork, and that call is
					 * the source's to escalate.
					 */
					if (!have && forked_from(as_id(who)) &&
					    cow_give_from_chain(as_id(who), rq.addr)) {
						have = page_read_as(who, rq.addr, page);
						if (have && ++n_get_chain_given <= 8)
							fprintf(stderr, "[vmremote] GET "
								"0x%llx for fork copy %d: "
								"given from its ancestor "
								"chain here rather than "
								"answered ABSENT\n",
								(unsigned long long)rq.addr,
								(int)as_id(who));
						plog(rq.addr, "GET for a fork copy: "
						     "chain give here, have=%d", have);
					}
					gt3 = now_us();
					coh_leave();
					/*
					 * WHICH step took the time.
					 *
					 * The source's GET gives up at 2000ms
					 * and this wait is bounded at 500, so a
					 * GET that expires upstream spent its
					 * remaining second and a half somewhere
					 * AFTER the wait -- and "somewhere" is
					 * three candidates with very different
					 * repairs: the coherence lock, the
					 * read itself, or neither. Printed only
					 * when it is slow, so it costs a
					 * now_us() per served page.
					 */
					if (gt3 - gt0 > 300000)
						fprintf(stderr, "[vmremote] SLOW GET "
							"0x%llx: pull_wait %llums, "
							"coh_enter %llums, read %llums "
							"(have=%d)\n",
							(unsigned long long)rq.addr,
							(unsigned long long)(gt1 - gt0) / 1000,
							(unsigned long long)(gt2 - gt1) / 1000,
							(unsigned long long)(gt3 - gt2) / 1000,
							have);
					plog(rq.addr, "GET absent-recheck under "
					     "coh lock: have=%d us=%llu", have,
					     (unsigned long long)
					     (pglog_on ? now_us() - ab0 : 0));
				}
				if (have)
					goto get_have_page;
			}
			/*
			 * A context that has ended, and a shared range it was
			 * holding pages of.
			 *
			 * The context is gone, so no page table on this side
			 * can answer -- but a shared range's storage is not the
			 * context, it is the object, and the object is still
			 * here with the bytes the guest wrote into it. Nothing
			 * can be racing them: the writer has exited.
			 *
			 * This is rd1's `exit-writeback`, which is AUDIT part
			 * VII's second standing gap in its own words -- "a
			 * mapping whose process simply exits is never written
			 * back... by the time the shadow settles there is
			 * nobody left to ask for the pages". There is somebody
			 * left to ask; it is this object, and until now nothing
			 * asked it. The source asks at VMR_OP_FINISH, which it
			 * is already sent and which arrives while that
			 * context's shadow still has the mapping.
			 *
			 * Deliberately conditional on the context being *dead*.
			 * A live context's absent page is a page in flight or
			 * one the shadow holds, and answering that one out of
			 * the object serves a copy from before the last write
			 * -- the failure this whole path exists to remove.
			 */
			if (shared_range_has(who, rq.addr) &&
			    !task_alive(who)) {
				off_t soff = shared_obj_off(who, rq.addr);

				if (soff >= 0 && shared_obj >= 0 &&
				    shared_obj_has(soff) &&
				    pread(shared_obj, page, VMR_PG_SIZE,
					  soff) == (ssize_t)VMR_PG_SIZE) {
					n_shared_after_exit++;
					plog(rq.addr, "GET after the context "
					     "ended: served from the shared "
					     "object at off=%lld (sum=%08x)",
					     (long long)soff, page_sum(page));
					/*
					 * Straight to the send, and NOT via
					 * get_have_page: that path takes the
					 * page away from the context and
					 * records the loan, and a take against
					 * a context that has exited fails --
					 * whereupon it reaches the FATAL that
					 * exists to stop ABSENT being answered
					 * over real memory, and aborts the
					 * run. There is no ownership to move
					 * here: the holder has gone, and the
					 * object is the only copy.
					 */
					goto get_send;
				}
			}
			/*
			 * Nothing here is readable -- and the loan record says
			 * THE ASKER is holding the page. Those two facts
			 * together mean the asker was given the page and has
			 * lost its copy: the token evaporated on its side (a
			 * poke into a context that died mid-service, a fault
			 * service that recorded OURS and installed nothing).
			 * ABSENT is then the answer that finishes the job --
			 * it licenses the source to poke an EMPTY page over an
			 * address whose bytes existed moments ago, and the
			 * zeros land in the shared source mm where every
			 * sibling's forwarded syscall reads them. Measured on
			 * ws1's teardown, at the dying thread's own descriptor
			 * page:
			 *
			 *   ABSENT #2 for 0x7ffff6ff4000: present=0 backing=0
			 *     holder=1748279 -- the source will fill this
			 *     address itself
			 *   [vmhome] BACKMOVE serve-out ... the page got here
			 *     by: ABSENT, empty page supplied here
			 *
			 * -- and the guest then reads pd->list.prev as 0 and
			 * dies unlinking a thread from a list that was intact
			 * (the null-deref face of the lost store).
			 *
			 * The newest surviving copy of such a page is this
			 * side's retained bytes -- captured at the very serve
			 * that gave the asker the page, writer slots included.
			 * The fault path has served them for the same state
			 * since the retain table existed ("token in transit");
			 * this is the same safety net at the serve the asker
			 * actually consults. The loan stays as it is: the
			 * asker is being handed the page again.
			 */
			if (rq.owner &&
			    page_holder(who, rq.addr) == rq.owner &&
			    retain_get(who, rq.addr, page)) {
				/*
				 * Two different states wear this record, and
				 * only one of them may be answered with the
				 * retained copy.
				 *
				 * DEAD holder: the token evaporated (a poke
				 * into a context that died mid-service) --
				 * the ws1 teardown case this branch was built
				 * for. The retained bytes are the newest copy
				 * in existence; serve them.
				 *
				 * LIVE holder: the token is IN TRANSIT -- the
				 * asker's copy left microseconds ago via its
				 * own serve toward this side (a serve and a
				 * pull of one page crossing, the loss the
				 * conserved-token comment in vmhome names) and
				 * the install lands here imminently. Serving
				 * the retained copy is then STALE BYTES OVER A
				 * LIVE STACK: it is the line pg3's failing
				 * runs print at B's last round, at the join,
				 * where main's futex read and B's own cycle
				 * cross on one page. Answer INFLIGHT -- ask
				 * again -- and the asker's very next GET finds
				 * the landed page.
				 */
				/*
				 * The discriminator is THE LANDING, not the
				 * clock. This side's own fault service pulls
				 * the page's bytes in (pull_tab); while that
				 * pull is ACTIVE the landing is in flight and
				 * the only correct answer is ask-again, for as
				 * long as it takes -- pg3's join-window
				 * episodes run past any reasonable horizon
				 * (a 1200ms one put 4 of 48 runs back on the
				 * stale copy) yet always land (96/96 with no
				 * cap). With NO pull active the crossing has
				 * either not started (the young-episode grace
				 * below) or its bytes died with a context
				 * mid-service (sig1's teardown shape, hung by
				 * the no-cap arm) -- past the grace, the
				 * retained copy is the newest that exists. A
				 * dead asker gets it immediately (the ws1
				 * case this branch was built for). A pull
				 * stuck forever is bounded by the asker's own
				 * 2s fault deadline, loudly.
				 */
				unsigned sl = (rq.addr >> 12) & 63;
				uint64_t nowu = now_us();
				int transit = 0;

				if (task_alive(who)) {
					if (infl_ans[sl].pg != rq.addr) {
						infl_ans[sl].pg = rq.addr;
						infl_ans[sl].first_us = nowu;
					}
					if (pull_active(rq.addr)) {
						transit = 1;
						rs.why = VMR_INFLIGHT_PULL;
					} else if (nowu - infl_ans[sl].first_us <
						   1200000) {
						transit = 1;	/* the clock: why 0 */
					}
				}
				if (transit) {
					n_pg_inflight++;
					rs.status = VMR_PG_INFLIGHT;
					rs.gen = pgen_get(who, rq.addr);
					trail_note(who, rq.addr, TR_SERVE_INFLIGHT,
						   pgen_get(who, rq.addr), 0, NULL);
					plog(rq.addr, "GET: the asker holds this "
					     "page per the record and is asking "
					     "-- token in transit; answered "
					     "INFLIGHT (ask again), not the "
					     "retained copy");
					if (pg_rw(c, &rs, sizeof(rs), 1) < 0 ||
					    pg_rw(c, page, VMR_PG_SIZE, 1) < 0)
						return 0;
					continue;
				}
				infl_ans[sl].pg = 0;	/* episode over */
				n_pg_get++;
				plog(rq.addr, "GET: the asker was given this "
				     "page and can no longer produce it; served "
				     "its last transmitted bytes rather than "
				     "ABSENT (sum=%08x)", page_sum(page));
				/*
				 * Stamped with the generation of the retained
				 * BYTES, not the current count: if a capture
				 * has happened since they were retained, the
				 * source is being handed an old copy and its
				 * stamp says so -- a later serve of it back
				 * here is then refused by the check, instead
				 * of landing one write behind (pg3's join-band
				 * shape). Counted when the two differ.
				 */
				{
					uint32_t rg = 0, cg = pgen_get(who, rq.addr);

					(void)retain_get_gen(who, rq.addr, page, &rg);
					rs.gen = rg;
					if (rg != cg)
						n_retained_served_old++;
					trail_note(who, rq.addr, TR_SERVE_RETAINED,
						   rg, cg, page);
				}
				fprintf(stderr, "[vmremote] the asker holds "
					"0x%llx and cannot produce it (holder %s); "
					"serving the retained copy (gen %u, count "
					"%u) rather than ABSENT, which would "
					"become an empty page over live memory\n",
					(unsigned long long)rq.addr,
					task_alive(who) ? "alive, transit never "
					"landed" : "dead", rs.gen,
					pgen_get(who, rq.addr));
				if (pg_rw(c, &rs, sizeof(rs), 1) < 0 ||
				    pg_rw(c, page, VMR_PG_SIZE, 1) < 0)
					return 0;
				continue;
			}
			/*
			 * Nothing readable here, the record says the GUEST holds
			 * it, and this side's OWN pull of the page is in flight:
			 * the bytes left the source a moment ago toward a fault
			 * service here that has not installed them yet. That is
			 * the token in transit, and the protocol's word for it
			 * is INFLIGHT -- ask again; the landing decides.
			 *
			 * pull_wait() above waited 500ms for exactly this and
			 * gave up: the pull is blocked on the source's own take
			 * wait, which is 1000ms (the hold on a page an in-flight
			 * syscall can touch). ABSENT here told the source "use
			 * your own copy" for a page it had just handed over;
			 * its record said THEIRS, so it re-asked for 20ms and
			 * would then end the context -- and meanwhile this side
			 * retained the ABSENT's zeros as the page (fixed at
			 * get_send) and installed them over the live page when
			 * the source answered the pull's sibling ABSENT in turn.
			 * Photographed in the hx2 pool (session 25): SLOW GET
			 * pull_wait 500ms have=0, then ABSENT #1 present=0
			 * backing=0 holder=0, then INSTALL-RETAINED of the
			 * all-zero sum, a writer reading 0. INFLIGHT keeps the
			 * source asking (its GET loop re-asks every 500us) until
			 * the pull lands, whereupon the next GET takes the page.
			 */
			if (pull_active(rq.addr)) {
				n_pg_inflight_pull++;
				rs.status = VMR_PG_INFLIGHT;
				rs.gen = pgen_get(who, rq.addr);
				rs.why = VMR_INFLIGHT_PULL;
				trail_note(who, rq.addr, TR_SERVE_INFLIGHT,
					   pgen_get(who, rq.addr), 1, NULL);
				plog(rq.addr, "GET: nothing readable and the "
				     "record says the guest's, but this side's "
				     "own pull of the page is in flight; "
				     "answered INFLIGHT (ask again), not ABSENT");
				if (pg_rw(c, &rs, sizeof(rs), 1) < 0 ||
				    pg_rw(c, page, VMR_PG_SIZE, 1) < 0)
					return 0;
				continue;
			}
			{
				rs.status = VMR_PG_ABSENT;
				rs.gen = 0;
				memset(page, 0, sizeof(page));
				n_pg_absent++;
				trail_note(who, rq.addr, TR_SERVE_ABSENT,
					   pgen_get(who, rq.addr), 0, NULL);
				plog(rq.addr, "GET -> ABSENT (nothing readable "
				     "in this address space)");
				/*
				 * Unconditional, because ABSENT is the answer
				 * that licenses the source to invent zeros:
				 * every one of them must be attributable to a
				 * page the guest truly never touched.
				 */
				fprintf(stderr, "[vmremote] ABSENT #%lu for "
					"0x%llx (as %d): present=%d backing=%d "
					"holder=%llu -- the source will fill "
					"this address itself\n", n_pg_absent,
					(unsigned long long)rq.addr,
					(int)as_id(who),
					as_any_present(who, rq.addr),
					backing_has(who, rq.addr),
					(unsigned long long)
					page_holder(who, rq.addr));
				/*
				 * Say what was asked and what each answered.
				 * "Nothing here" is a claim about three separate
				 * records -- the page tables of every context on
				 * this address space, and the object those
				 * contexts are views of -- and which of them said
				 * no is the whole diagnosis. Without it this line
				 * sends the reader to the source, where the page
				 * is simply missing and nothing says why.
				 */
				{
					memory_target mem[MAX_CTX];
					int nm = as_members(who, mem, MAX_CTX), k;
					int bfd = backing_find(who);

					fprintf(stderr, "[vmremote] page service: "
						"0x%llx is in no context of address "
						"space %d (object fd %d, has=%d):",
						(unsigned long long)rq.addr,
						(int)as_id(who), bfd,
						backing_has(who, rq.addr));
					for (k = 0; k < nm; k++)
						fprintf(stderr, " ctx %d present=%d",
							ctx_id(mem[k]),
							page_present(mem[k], rq.addr));
					fprintf(stderr, "\n");
				}
				fprintf(stderr, "[vmremote] page service: ctx "
					"%d has nothing at 0x%llx (op %u); the "
					"shadow will use its own copy\n",
					ctx_id(who), (unsigned long long)rq.addr,
					rq.op);
				/*
				 * The guest has never touched this address, so
				 * the shadow will fill it from the copy home
				 * loaded the program into — and from that
				 * moment the shadow holds it, exactly as if it
				 * had been taken from here. Recording the loan
				 * anyway is what keeps the guest's first fault
				 * on that address a recall.
				 *
				 * ...for a page this side has NO record of,
				 * which is the start-up case the paragraph
				 * below is about. It is not true of a page
				 * whose own record here says the guest holds
				 * it: this side cannot read a page it believes
				 * it has, so the page is lost on this side, and
				 * writing "the shadow has it" over that is an
				 * inference about the other machine (PRINCIPLES
				 * §1) that makes the loss permanent -- the
				 * guest's next fault becomes a pull from a
				 * shadow that never received it, and this side
				 * stops consulting its own object, which is the
				 * one place the bytes may still be.
				 *
				 * It is also the destination's half of the way
				 * both machines came to answer ABSENT about one
				 * page within a millisecond, each side's record
				 * naming the other as holder (AUDIT XI §5.1);
				 * the source's half is PG_THEIRS on ITS absent
				 * path, and the two meet.
				 *
				 * Counted three ways either way, because a
				 * switch with no numbers under it is an
				 * opinion.
				 *
				 * Without this the two sides quietly diverge:
				 * the shadow writes its copy, the guest later
				 * faults, finds nothing lent, and is served the
				 * program's original bytes. Both copies are
				 * then live and neither is right. It happens
				 * constantly at start-up, because the pages a
				 * syscall touches first are the ones the guest
				 * has not reached yet.
				 */
				if (rq.owner) {
					int ls;

					coh_enter();
					ls = page_lent_state(who, rq.addr);
					if (ls == LENT_GUEST)
						n_absent_was_guest++;
					else if (ls == LENT_SHADOW)
						n_absent_was_shadow++;
					else
						n_absent_unseen++;
					if (ls == LENT_GUEST) {
						if (n_absent_was_guest <= 32)
							fprintf(stderr, "[vmremote] 0x%llx "
								"is recorded here as the "
								"guest's and cannot be read "
								"in any context of address "
								"space %d: this side has lost "
								"it, so the record is left "
								"alone rather than made to "
								"say the shadow has it\n",
								(unsigned long long)rq.addr,
								(int)as_id(who));
					} else {
						page_lend(who, rq.addr, rq.owner);
					}
					coh_leave();
				}
				goto get_send;
			}
get_have_page:
			if (rq.owner) {
				int want_shared = (rq.op == VMR_PG_GETS);
				uint64_t tk0 = pglog_on ? now_us() : 0;
				/* Request entry acquired the page before any read or
				 * ownership change. Keep it reserved until this response's
				 * retained bytes, generation and socket write are complete. */
				int sm_prev = transfer.claim.prior;

				coh_enter();
				/*
				 * A read is answered with a copy and the guest
				 * keeps a read-only one. A write moves the page
				 * outright: the shadow is about to be the only
				 * holder, so the guest must stop being one, and
				 * taking it is also what makes the guest fault
				 * next time it touches the address.
				 */
				pthread_mutex_lock(&ctl_lock);
				if (want_shared) {
					/*
					 * Protect first, then read. In that
					 * order the bytes cannot change between
					 * the two, because after the protect the
					 * guest cannot write the page at all.
					 *
					 * The read above decided whether the
					 * address space has the page; its bytes
					 * are not good enough to send. Anything
					 * read before the protect is a snapshot
					 * of a page the guest may still be
					 * writing, and the guest's write raises
					 * no fault while it is still writable --
					 * so this side would hold a copy one
					 * iteration old and believe it was
					 * sharing. th9's worker emitted
					 * "thread 36 ok" where 37 belonged, once
					 * or twice in a hundred runs, and the
					 * page's own record said "taken shared
					 * at call 55, and this is call 56".
					 *
					 * The original code re-read the page
					 * here for this reason. That was lost
					 * when the read moved out to cover the
					 * whole address space; taking it after
					 * the protect is the same guarantee
					 * without the window either version had.
					 */
					{
						int okp = page_protect(who, rq.addr);

						/* protect object-wide + atomically,
						 * then read the guest's own view.
						 * PROTECTOBJ freezes every mapping so
						 * the read below is not a write old; the
						 * read stays page_read_as() because the
						 * backing folio's bytes read as zeros
						 * for a page the guest holds otherwise,
						 * which failed pg1/pg2. */
						r = okp && page_read_as(who, rq.addr, page);
						if (okp && !r)
							n_prot_noread++;
					}
					if (r)
						n_shared++;
				} else {
					r = page_take_away(who, rq.addr, page);
					/*
					 * Ownership moved; drop the stale "we hold
					 * it" record. The per-page state is settled
					 * (to INVALID) after this block, off the
					 * sm_prev/sm_pk claimed above.
					 */
					if (r)
						mark_handed_over(who, rq.addr);
				}
				pthread_mutex_unlock(&ctl_lock);
				if (pglog_on) {
					int post = as_any_present(who, rq.addr);

					plog(rq.addr, "GET %s r=%ld post=%d us=%llu "
					     "sum=%08x%s first8=%02x%02x%02x%02x"
					     "%02x%02x%02x%02x",
					     want_shared ? "protect" : "take",
					     r, post,
					     (unsigned long long)(now_us() - tk0),
					     r ? page_sum(page) : 0,
					     r ? pgword(page) : "",
					     (unsigned char)page[0],
					     (unsigned char)page[1],
					     (unsigned char)page[2],
					     (unsigned char)page[3],
					     (unsigned char)page[4],
					     (unsigned char)page[5],
					     (unsigned char)page[6],
					     (unsigned char)page[7]);
					if (r && !want_shared && post)
						n_take_kept++;
				}
				if (r) {
					page_lend_mode(who, rq.addr, rq.owner,
						       want_shared);
					coh_check(as_id(who), rq.addr, rq.owner,
						  want_shared, "at lend");
				} else {
					/*
					 * The address space HAS this page --
					 * page_read_as() just read it -- and
					 * the take would not give it up. That
					 * is not absence, and answering ABSENT
					 * here made the source invent zeros
					 * over real memory: pg3's guest stack,
					 * measured, 2.3ms after a single
					 * refused take, return address zeroed,
					 * guest dead at rip 0. The take above
					 * now waits out -EBUSY, so reaching
					 * this point means a failure that
					 * waiting cannot fix.
					 *
					 * There is nothing honest left to send.
					 * Say everything known and stop, rather
					 * than hand the program a page of zeros
					 * where its own memory belongs.
					 */
					/*
					 * Before this is treated as fatal, ask
					 * again whether this side still holds
					 * the page -- because between the read
					 * above and the take just now it may
					 * have stopped holding it.
					 *
					 * page_read_as() answers from the
					 * backing object when no context has
					 * the page mapped, which is the normal
					 * state between a take and the next
					 * fault. A hand-over then punches that
					 * page out of the object (or TAKEOBJ's
					 * shmem_truncate_range does). So a
					 * concurrent hand-over of the same page
					 * leaves exactly this: the read saw it,
					 * the take found no context with it,
					 * and the object no longer has it.
					 *
					 * That is not "answering ABSENT over
					 * real memory" -- there is no real
					 * memory on this side any more, and
					 * ABSENT is simply true: the asker's own
					 * copy is the current one, which is what
					 * ABSENT means. Aborting here killed the
					 * run for a state the protocol already
					 * has a word for. Measured on pg3, which
					 * fails 5 of 40 this way while passing
					 * 200 of 200 natively.
					 *
					 * The errno the message prints is why
					 * this was hard to read: it is ENODATA
					 * from the peek() inside page_read_as()
					 * -- "not present" for one context --
					 * and has nothing to do with the take.
					 * It is honest about the wrong call.
					 */
					if (!as_any_present(who, rq.addr) &&
					    !backing_has(who, rq.addr)) {
						n_get_lost_race++;
						trail_note(who, rq.addr,
							   TR_SERVE_NOTAKE,
							   pgen_get(who, rq.addr),
							   (uint32_t)pull_active(rq.addr),
							   page);
						plog(rq.addr, "GET: the page left "
						     "this side between the read "
						     "and the take; ABSENT is now "
						     "the true answer");
						/*
						 * And it MUST NOT go out as the
						 * page. `page` still holds the
						 * read taken at the top -- BEFORE
						 * the claim wait and the take's
						 * own wait, each up to a second
						 * under load -- and this branch
						 * used to fall through to get_send
						 * with rs.status still 0: the
						 * pre-wait bytes went on the wire
						 * as a successful GET, stamped
						 * with the CURRENT count (the
						 * concurrent take had just bumped
						 * it), and were retained as the
						 * page's newest copy. Photographed
						 * in the hx2 pool (session 25): a
						 * writer reading its slot a second
						 * behind, the trail showing a
						 * SERVE-GET with no take before it
						 * 1.07s after the real TAKE-AWAY,
						 * then that copy served back under
						 * an equal stamp -- which is why
						 * the generation check could not
						 * see it. The summary line above
						 * this counter has always said
						 * "answered ABSENT"; now it does.
						 *
						 * Answered NOW, from what is known,
						 * with no further wait: this GET
						 * has already spent the claim wait
						 * and the take's -EBUSY wait, and
						 * the source's socket gives a GET
						 * two seconds -- re-running the
						 * decision from the top (tried
						 * first) added another pull_wait +
						 * claim + take, 48 of 256 pooled
						 * runs blew the two seconds, and
						 * the source ended the context
						 * ("the page channel failed and
						 * did not recover", exit 13).
						 *
						 * What is true: the page is in a
						 * pull landing here (pull_active:
						 * the token is in transit, answer
						 * INFLIGHT, the landing decides)
						 * or it left for the asker through
						 * the other channel (a concurrent
						 * GET took it): then the asker's
						 * own side holds or is about to
						 * hold it, which is what ABSENT
						 * means to the source -- its
						 * ABSENT path waits for a sibling
						 * fetch of the same page and
						 * finishes on ctx_present. Nothing
						 * is retained for either answer.
						 */
						coh_leave();
						if (sm_prev >= 0)
							transfer.final = (enum pg_st)sm_prev;
						if (pull_active(rq.addr)) {
							n_get_lost_race_inflight++;
							rs.status = VMR_PG_INFLIGHT;
							rs.gen = pgen_get(who, rq.addr);
							rs.why = VMR_INFLIGHT_PULL;
							trail_note(who, rq.addr,
								   TR_SERVE_INFLIGHT,
								   pgen_get(who, rq.addr),
								   2, NULL);
							if (pg_rw(c, &rs, sizeof(rs), 1) < 0 ||
							    pg_rw(c, page, VMR_PG_SIZE, 1) < 0)
								return 0;
							continue;
						}
						n_get_lost_race_absent++;
						rs.status = VMR_PG_ABSENT;
						rs.gen = 0;
						memset(page, 0, sizeof(page));
						trail_note(who, rq.addr,
							   TR_SERVE_ABSENT,
							   pgen_get(who, rq.addr),
							   2, NULL);
						goto get_send_noretain;
					} else if (backing_has(who, rq.addr) &&
						   own_object_page(who, rq.addr,
								   page)) {
						/*
						 * No context maps it; the OBJECT is
						 * the only holder. That is the normal
						 * state between an install and the
						 * next fault, made durable by a
						 * give-back PUT into an address space
						 * whose service mm has no mapping at
						 * the address (shmat: the one
						 * propagation gap). The object IS the
						 * address space's memory, so the take
						 * happens against IT: read the bytes,
						 * punch the page out, record the
						 * asker as the holder, serve.
						 * Aborting here killed netsurf 4.9s
						 * in, over bytes sitting in the
						 * object the whole time.
						 */
						int bf = backing_find(who);

						if (!want_shared) {
							/*
							 * A MOVE: the page leaves
							 * with the bytes. A shared
							 * read (GETS) leaves the
							 * object as it is -- both
							 * sides read-only -- and
							 * changes no record.
							 */
							if (bf >= 0)
								fallocate(bf,
									  FALLOC_FL_PUNCH_HOLE |
									  FALLOC_FL_KEEP_SIZE,
									  (off_t)rq.addr,
									  VMR_PG_SIZE);
							page_lend(who, rq.addr,
								  rq.owner);
							/* A capture like any other
							 * take: the clock moves. */
							hist_note(rq.addr, page);
							trail_note(who, rq.addr,
								   TR_TAKE_AWAY,
								   pgen_bump(who, rq.addr),
								   2, page);
						}
						n_get_from_object++;
						plog(rq.addr, "GET: no context maps "
						     "it; %s the OBJECT and served",
						     want_shared ? "copied from"
								 : "taken (read+punch) from");
						fprintf(stderr, "[vmremote] GET "
							"0x%llx: no context maps it; "
							"served from the address "
							"space's own object rather "
							"than aborting\n",
							(unsigned long long)rq.addr);
						if (sm_prev >= 0)
							transfer.final = want_shared ?
								  PST_SHARED :
								  PST_INVALID;
						coh_leave();
						goto get_send;
					} else {
					{
							/*
							 * Who was asked, and what each
							 * of them said. The message
							 * below names the address space
							 * and the errno; neither says
							 * whether the context holding
							 * the page was in the set that
							 * was asked, which is the whole
							 * question.
							 */
							memory_target mem[MAX_CTX];
							int mn = as_members(who, mem, MAX_CTX), mi;

							fprintf(stderr, "[vmremote] FATAL-WHO: "
								"asked=%d as=%d members=%d "
								"rq.owner=%llu holder=%llu "
								"same=%d\n",
								ctx_id(who), (int)as_id(who), mn,
								(unsigned long long)rq.owner,
								(unsigned long long)page_holder(who, rq.addr),
								page_holder(who, rq.addr) == rq.owner);
							for (mi = 0; mi < mn; mi++)
								fprintf(stderr, "[vmremote] FATAL-WHO:   "
									"ctx %d alive=%d present=%d installed=%d\n",
									ctx_id(mem[mi]),
									task_alive(mem[mi]),
									page_present(mem[mi], rq.addr),
									page_is_installed(mem[mi], rq.addr));
							fprintf(stderr, "[vmremote] FATAL-WHO: "
								"as_any_present=%d in_flight=%d "
								"installed_any=%d holder=%llu "
								"watched=%d\n",
								as_any_present(who, rq.addr),
								pull_active(rq.addr),
								page_is_installed(who, rq.addr),
								(unsigned long long)
								page_holder(who, rq.addr),
								page_watched(who, rq.addr));
						}
						/*
						 * The one benign shape this
						 * reaches, photographed by the
						 * s41l abort (hx2, 1 in ~500
						 * pool runs, exit 134 0.8 s
						 * in): every member answered
						 * ENOENT because the page was
						 * IN FLIGHT into one of them,
						 * and it LANDED between the
						 * last take attempt and this
						 * line -- the FATAL-WHO dump
						 * showed present=1 on a member
						 * the loop had already asked.
						 * That is not "nobody will
						 * hand it over"; it is "it
						 * just arrived, ask again",
						 * and the protocol has a word
						 * for it. The asker's INFLIGHT
						 * loop is time-bounded, so a
						 * state that never becomes
						 * takeable still surfaces
						 * honestly there rather than
						 * as this side's abort.
						 */
						if (as_any_present(who, rq.addr)) {
							n_get_landed_racing++;
							fprintf(stderr, "[vmremote] GET 0x%llx: "
								"the takes all missed but the "
								"page is PRESENT now -- it "
								"landed behind the loop; "
								"answered INFLIGHT (ask "
								"again), not a fatal\n",
								(unsigned long long)rq.addr);
							rs.status = VMR_PG_INFLIGHT;
							rs.gen = pgen_get(who, rq.addr);
							rs.why = VMR_INFLIGHT_PULL;
							if (sm_prev >= 0)
								transfer.final = (enum pg_st)sm_prev;
							coh_leave();
							if (pg_rw(c, &rs, sizeof(rs), 1) < 0 ||
							    pg_rw(c, page, VMR_PG_SIZE, 1) < 0)
								return 0;
							continue;
						}
						fprintf(stderr, "[vmremote] FATAL: 0x%llx "
							"is present in address space %d "
							"but no context would hand it "
							"over (last errno: %s). Refusing "
							"to answer ABSENT over real "
							"memory; the guest's own bytes "
							"are not this side's to "
							"invent.\n",
							(unsigned long long)rq.addr,
							(int)as_id(who),
							strerror(errno));
						step_timeout("GET take of", rq.addr, 0);
						coh_leave();
						abort();
					}
				}
				coh_leave();
				/*
				 * Settle the per-page state now the hand-off is
				 * done. A shared read that succeeded leaves both
				 * sides holding a read-only copy (SHARED); a take
				 * that succeeded moved the token off this side
				 * (INVALID); a give-up that failed never happened,
				 * so restore the prior stable state. Only if we
				 * actually claimed the page (sm_prev >= 0).
				 */
				if (sm_prev >= 0)
					transfer.final = r ? (want_shared ? PST_SHARED
								   : PST_INVALID)
						    : (enum pg_st)sm_prev;
			}
			/* Coherence off: page_read_as() has already answered
			 * with the bytes, and the branch above did nothing to
			 * them. */
get_send:
			/*
			 * The page is now transmitted: remember its exact bytes
			 * so it can never be zero-filled if a later fault for it
			 * crosses this hand-off and finds it momentarily nowhere.
			 * Captured here, at the serve, so the retained copy is
			 * the page's contents as they left -- every writer slot
			 * included -- not a stale guess.
			 *
			 * ONLY for an answer that served the page. This used to
			 * be unconditional, and the ABSENT answer arrives here
			 * with `page` memset to zero: those zeros were retained
			 * as the page's "last transmitted bytes", stamped with
			 * the current count, and the fault path's fill site 5
			 * ("retained, after ABSENT") later installed them over
			 * the live page. Photographed in the hx2 pool (session
			 * 25, trail): SERVE-ABSENT gen=32, then INSTALL-RETAINED
			 * gen=32 aux=5 sum=04000001 (the all-zero sum), then the
			 * next TAKE-AWAY capturing an all-zero page -- the
			 * writer's "read 0" loss, and pg3's thread-stack page
			 * read back as zeros (return address 0, exit 242). An
			 * ABSENT transmits nothing; the first-touch zeros that
			 * may be retained are the fault path's own (fill site 6,
			 * which retains what it installs).
			 */
			if (rs.status == 0)
				retain_put(who, rq.addr, page);
get_send_noretain:
			n_pg_get++;
			/* A normal serve is the landing an in-transit episode
			 * was waiting for. See infl_ans. */
			infl_ans_clear(rq.addr);
			/*
			 * The stamp the source keeps beside its copy and echoes
			 * on every serve back: the count as it stands after the
			 * take above (a take bumped it; a shared read, a
			 * served-after-exit object read and a served-ABSENT
			 * leave it). Read AFTER the take, so the copy on the
			 * wire and its stamp are the same capture.
			 */
			if (rs.status == 0) {
				rs.gen = pgen_get(who, rq.addr);
				trail_note(who, rq.addr, TR_SERVE_GET, rs.gen,
					   rq.op == VMR_PG_GETS, page);
			}
			if (pg_rw(c, &rs, sizeof(rs), 1) < 0 ||
			    pg_rw(c, page, VMR_PG_SIZE, 1) < 0)
				return 0;
		} else if (rq.op == VMR_PG_PUTN) {
			uint64_t n = rq.len;

			if (n == 0 || n > VMR_PG_SIZE)
				return 0;
			if (pg_rw(c, page, n, 0) < 0)
				return 0;
			/*
			 * Writing part of a page into a page that does not exist
			 * yet creates it — with the rest of it zero. The guest
			 * then never faults there again and reads zeros where
			 * its code or data should be, which is fatal and looks
			 * nothing like its cause: it surfaces later as a jump
			 * into a blank page. Only the changed bytes are sent, so
			 * fetch the page's real contents first and let the
			 * partial write land on top.
			 */
			/*
			 * Supply the page and nothing else, and look at the
			 * answer. This context has not faulted -- it is running
			 * -- so nothing here may reach the owner and come back
			 * with a register set to install in it; see
			 * fault_from_home_mode(). And a prefill that failed is
			 * worth a line, because the partial write below then
			 * lands on a page whose other bytes are this kernel's
			 * zeros, which is the failure the prefill exists to
			 * prevent.
			 */
			if (n < VMR_PG_SIZE &&
			    page_present(who, rq.addr) == PAGE_ABSENT) {
				n_pg_prefill++;
				if (!fault_from_home_mode(who, 14, rq.addr, 0x4, 1) &&
				    n_pg_prefill_fail++ < 8)
					fprintf(stderr, "[vmremote] page service: "
						"could not fetch 0x%llx before "
						"writing %llu bytes of it back "
						"(ctx %d); the rest of the page "
						"will be zeros\n",
						(unsigned long long)rq.addr,
						(unsigned long long)n, ctx_id(who));
			}
			tracep(rq.addr, who, "pgservice:PUTN-bytes", (long)n);
			pthread_mutex_lock(&ctl_lock);
			r = poke_as(who, rq.addr, page, n);
			pthread_mutex_unlock(&ctl_lock);
			trail_note(who, rq.addr, TR_PUT_LANDED,
				   pgen_get(who, rq.addr), rq.op, NULL);
			mark_installed(who, rq.addr, n);
			page_lend(who, rq.addr, 0);
			rs.status = (r > 0) ? 0 : -1;
			n_pg_put++;
			if (pg_rw(c, &rs, sizeof(rs), 1) < 0)
				return 0;
		} else if (rq.op == VMR_PG_PUTV) {
			/*
			 * Every changed run of one page in one message. The
			 * bytes are the same ones PUTN sends a request at a
			 * time; only the framing differs, so the guest sees
			 * exactly what it saw before — for one round trip per
			 * page instead of one per run.
			 */
			struct vmr_pgrun runs[VMR_PG_SIZE / 2];
			struct iovec liov[VMR_PG_SIZE / 2], riov[VMR_PG_SIZE / 2];
			uint64_t nruns = rq.len, total = 0;
			unsigned i;

			if (nruns == 0 || nruns > VMR_PG_SIZE / 2)
				return 0;
			if (pg_rw(c, runs, nruns * sizeof(runs[0]), 0) < 0)
				return 0;
			for (i = 0; i < nruns; i++) {
				if (!runs[i].len ||
				    runs[i].off >= VMR_PG_SIZE ||
				    runs[i].len > VMR_PG_SIZE - runs[i].off)
					return 0;
				total += runs[i].len;
			}
			if (total > VMR_PG_SIZE)
				return 0;
			for (i = 0; i < nruns; i++)
				if (pg_rw(c, page + runs[i].off, runs[i].len, 0) < 0)
					return 0;
			/*
			 * Same rule as PUTN: writing part of a page that does
			 * not exist yet creates it with the rest zero, and the
			 * guest never faults there again — so fetch what is
			 * really there first and let these bytes land on top.
			 */
			tracep(rq.addr, who, "pgservice:PUTV-bytes", (long)total);
			/* Supply-only and checked, exactly as PUTN above. */
			if (total < VMR_PG_SIZE &&
			    page_present(who, rq.addr) == PAGE_ABSENT) {
				n_pg_prefill++;
				if (!fault_from_home_mode(who, 14, rq.addr, 0x4, 1) &&
				    n_pg_prefill_fail++ < 8)
					fprintf(stderr, "[vmremote] page service: "
						"could not fetch 0x%llx before "
						"writing %llu bytes of it back "
						"(ctx %d); the rest of the page "
						"will be zeros\n",
						(unsigned long long)rq.addr,
						(unsigned long long)total,
						ctx_id(who));
			}
			for (i = 0; i < nruns; i++) {
				liov[i].iov_base = page + runs[i].off;
				liov[i].iov_len  = runs[i].len;
				riov[i].iov_base =
					(void *)(uintptr_t)(rq.addr + runs[i].off);
				riov[i].iov_len  = runs[i].len;
			}
			pthread_mutex_lock(&ctl_lock);
			clobber_check(who, rq.addr, page, VMR_PG_SIZE, 37);
			r = execution_writev(who, liov, nruns, riov);
			pthread_mutex_unlock(&ctl_lock);
			if (r < 0) {
				/* One crossing per run, but it still gets there. */
				r = 0;
				for (i = 0; i < nruns; i++)
					if (poke_as(who, rq.addr + runs[i].off,
						    page + runs[i].off,
						    runs[i].len) > 0)
						r += runs[i].len;
			}
			trail_note(who, rq.addr, TR_PUT_LANDED,
				   pgen_get(who, rq.addr), rq.op, NULL);
			mark_installed(who, rq.addr, VMR_PG_SIZE);
			page_lend(who, rq.addr, 0);
			/* The page landed here: any in-transit episode on it
			 * is over. See infl_ans. */
			infl_ans_clear(rq.addr);
			rs.status = (r > 0) ? 0 : -1;
			n_pg_put++;
			if (trace_put)
				fprintf(stderr, "[vmremote] PUTN into %d at 0x%llx "
					"(%lu bytes in %llu runs)\n", ctx_id(who),
					(unsigned long long)rq.addr, (unsigned long)r,
					(unsigned long long)nruns);
			if (pg_rw(c, &rs, sizeof(rs), 1) < 0)
				return 0;
		} else if (rq.op == VMR_PG_PUT) {
			if (pg_rw(c, page, VMR_PG_SIZE, 0) < 0)
				return 0;
			pthread_mutex_lock(&ctl_lock);
			r = poke_as(who, rq.addr, page, VMR_PG_SIZE);
			pthread_mutex_unlock(&ctl_lock);
			trail_note(who, rq.addr, TR_PUT_LANDED,
				   pgen_get(who, rq.addr), rq.op, page);
			mark_installed(who, rq.addr, VMR_PG_SIZE);
			page_lend(who, rq.addr, 0);
			/* The page landed here: any in-transit episode on it
			 * is over. See infl_ans. */
			infl_ans_clear(rq.addr);
			rs.status = (r > 0) ? 0 : -1;
			n_pg_put++;
			if (trace_put)
				fprintf(stderr, "[vmremote] PUT into %d at 0x%llx\n",
					ctx_id(who), (unsigned long long)rq.addr);
			if (pg_rw(c, &rs, sizeof(rs), 1) < 0)
				return 0;
		} else {
			/*
			 * An operation this service does not implement.
			 *
			 * Dropping the connection for it is right -- the stream
			 * has a page-sized body whose presence depends on the op,
			 * so guessing would desynchronise everything after it --
			 * but doing it silently is not. The shadow sees only
			 * end-of-file on its next request, for a page that has
			 * nothing to do with the cause, and the service that
			 * closed the channel appears in no log at all. That cost
			 * a long hunt through fork paths for a failure in the
			 * first context's ordinary startup.
			 */
			fprintf(stderr, "[vmremote] page service: unimplemented "
				"op %u for 0x%llx (ctx %llu, owner %llu) -- "
				"dropping the channel\n", rq.op,
				(unsigned long long)rq.addr,
				(unsigned long long)rq.target.context,
				(unsigned long long)rq.owner);
			return 0;
		}
	}
}

static void *pg_conn_thread(void *arg)
{
	int fd = *(int *)arg;
	int adopted;

	free(arg);
	fprintf(stderr, "[vmremote] page channel fd %d: service thread starts\n",
		fd);
	/*
	 * A connection adopted as a recall channel outlives this thread: the
	 * fault path owns it now and closing it here would take it away.
	 */
	adopted = pg_serve_conn(fd);
	fprintf(stderr, "[vmremote] page channel fd %d: service thread ends "
		"(%s)\n", fd,
		adopted ? "adopted for recalls, left open" : "closing it");
	if (!adopted)
		close(fd);
	return NULL;
}

/*
 * Whether the page service is listening yet. The source connects straight
 * back to it during VMR_OP_START, so START must not be sent before listen()
 * has returned -- and main() used to make sure of that with usleep(100000),
 * which was the whole of the 108 ms "setup" every run paid (tests/perf.sh,
 * session 38: wc natively 0.015 s, forwarded 0.56 s of which 0.108 s was
 * this sleep). Signalled here, waited for there.
 */
static pthread_mutex_t pg_up_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t pg_up_cv = PTHREAD_COND_INITIALIZER;
static int pg_up;		/* 1 listening, -1 could not */

static void pg_up_set(int v)
{
	pthread_mutex_lock(&pg_up_lock);
	pg_up = v;
	pthread_cond_broadcast(&pg_up_cv);
	pthread_mutex_unlock(&pg_up_lock);
}

static void *pg_thread(void *arg)
{
	int port = (int)(long)arg;
	struct sockaddr_in a;
	int s, one = 1;

	s = socket(AF_INET, SOCK_STREAM, 0);
	if (s < 0) {
		pg_up_set(-1);
		return NULL;
	}
	setsockopt(s, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
	memset(&a, 0, sizeof(a));
	a.sin_family = AF_INET;
	a.sin_addr.s_addr = htonl(INADDR_ANY);
	a.sin_port = htons(port);
	if (bind(s, (struct sockaddr *)&a, sizeof(a)) < 0 || listen(s, 4) < 0) {
		fprintf(stderr, "[vmremote] page service cannot listen on %d: %s\n",
			port, strerror(errno));
		close(s);
		pg_up_set(-1);
		return NULL;
	}
	fprintf(stderr, "[vmremote] serving the guest's pages on port %d\n", port);
	pg_up_set(1);
	for (;;) {
		int c = accept(s, NULL, NULL);

		if (c >= 0) {
			int one = 1;

			/* A strict request/response page channel is exactly the
			 * pattern Nagle plus delayed ACK punishes: small reply
			 * held back, peer's ACK on a timer. */
			setsockopt(c, IPPROTO_TCP, TCP_NODELAY, &one,
				   sizeof(one));
		}

		if (c < 0) {
			if (errno == EINTR)
				continue;
			break;
		}
		setsockopt(c, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
		/*
		 * A thread each. A forked guest has a shadow per context on the
		 * other machine, and each one asks for pages on its own
		 * connection: serving them one after another means the second
		 * shadow waits for the first to finish, which it never does —
		 * it is blocked on the page it is waiting for us to accept.
		 */
		{
			pthread_t th;
			int *fd = malloc(sizeof(*fd));

			/*
			 * Name it, so a channel that dies later can be traced
			 * back to when it was taken. A shadow that sees
			 * end-of-file has no way to say which connection it
			 * was, and the service that owns it does not notice.
			 */
			{
				char lp[64], tgt[96];
				ssize_t n;

				snprintf(lp, sizeof(lp), "/proc/self/fd/%d", c);
				n = readlink(lp, tgt, sizeof(tgt) - 1);
				tgt[n > 0 ? n : 0] = '\0';
				fprintf(stderr, "[vmremote] page channel "
					"accepted as fd %d = %s\n", c,
					n > 0 ? tgt : "?");
			}
			if (!fd) {
				fprintf(stderr, "[vmremote] page channel fd %d: "
					"out of memory for its thread; closing "
					"it, which the shadow will see as "
					"end-of-file\n", c);
				close(c);
				continue;
			}
			*fd = c;
			{
				int perr = pthread_create(&th, NULL,
							  pg_conn_thread, fd);

				if (perr == 0) {
					pthread_detach(th);
				} else {
					/*
					 * The last unlogged way to lose a page
					 * channel, and the one that looks least
					 * like itself: the shadow sees only
					 * end-of-file on its next request, for a
					 * page that has nothing to do with the
					 * cause, and nothing here says a word.
					 */
					fprintf(stderr, "[vmremote] page channel "
						"fd %d: cannot start its service "
						"thread: %s -- closing it, which "
						"the shadow will see as "
						"end-of-file\n", c, strerror(perr));
					free(fd);
					close(c);
				}
			}
			continue;
		}
	}
	close(s);
	return NULL;
}

/* ---- demand paging from home ------------------------------------------ */
/*
 * The general model. The remote machine knows nothing about files, ELF headers
 * or interpreters: home starts the program (its kernel does all the loading)
 * and stops it before the first instruction. We copy that address space's
 * *shape* here, then let the guest run and resolve every page it touches by
 * asking home for the bytes. Libraries the loader maps later arrive the same
 * way: the mmap of a home file descriptor is turned into anonymous memory here
 * and its pages are pulled from that descriptor on fault.
 */
/*
 * One entry per mapping in the oracle's map. This was 256, which is more than
 * a small program has and far fewer than a real one: links2 in graphical mode
 * has 493 mappings, dillo 187, /bin/sleep 49. Overflowing it dropped the
 * remaining regions *silently*, so every address above the cut looked like
 * memory nobody had mapped — the guest called into libc, got a zero-filled
 * page, executed the zeroes and died with an unrecoverable #PF at 0x0 four
 * instructions in. A cap that changes the answer must never be silent, hence
 * region_overflow() below.
 */
#define MAX_REGIONS 4096


/*
 * A range of the guest's address space, and nothing else about it.
 *
 * There used to be a descriptor, an offset and a file name here, because this
 * machine served a mapping's pages by reading the file itself. It does not any
 * more: the source performs every mapping, so a range is either one the source
 * has -- and its bytes are asked for by address with VMR_OP_CTXPAGE -- or one
 * whose zeros are the right answer, which is the only thing `local` still says.
 * Nothing here names a file, a descriptor or a path, and nothing needs to: an
 * address is the whole of what a page request has to carry.
 */
struct region {
	uint64_t start, end;
	uint64_t as;		/* the ADDRESS SPACE this range describes. The
				 * table was global: an exec in ONE process
				 * dropped every sibling's regions (firefox's
				 * wrapper-script children died on that), and a
				 * guest mmap in one process forgot a range that
				 * still described another's memory. */
	/*
	 * What the program was loaded with, straight out of the perms column of
	 * the map home sent. The layout parser used to read it into a buffer and
	 * never look at it again -- the same mistake vmhome made with the same
	 * column, and between them that is the whole reason a store into a
	 * program's own .text landed and nobody saw it.
	 */
	int      prot;
};


/*
 * There was a table here that remembered what each of the guest's descriptors
 * had been opened as, keyed by context and walked up the fork lineage, because
 * this machine decided which file a mapping's pages were read from. It is gone
 * with the reason for it. The destination has no descriptors of the guest's to
 * name, no paths to remember, and no opinion about which file anything is: the
 * source performs the open and the mmap, and a page is asked for by address.
 */

static struct region regions[MAX_REGIONS];
static int nregions;
static unsigned long n_pages_faulted;
static unsigned long n_recall_dead_ctx;	/* recall landed after its context exited; kept in the object */
/* ...and the same for a LIVE context with no mapping at the address yet. */
static unsigned long n_recall_via_object;

/*
 * Refuse to run rather than run on a map we know is incomplete: a truncated
 * map does not fail where it was truncated, it fails wherever the guest first
 * touches an address that fell off the end, which looks like memory corruption
 * anywhere in the program.
 */
static void region_overflow(void)
{
	static int said;

	if (!said) {
		said = 1;
		fprintf(stderr, "[vmremote] FATAL: more than %d regions in the "
			"oracle's map; it would be truncated and the guest "
			"would execute zero-filled pages. Raise MAX_REGIONS.\n",
			MAX_REGIONS);
	}
	_exit(3);
}

/*
 * Which contexts are one address space.
 *
 * This record was a table of mappings once, kept per context because two
 * address spaces diverge after a fork, with a mark saying how much of a
 * parent's a child had inherited. The mappings are gone with everything else
 * this machine used to believe about the guest's memory; the question they were
 * indexed by is not. A thread shares the address space it was created from, so
 * it is recorded as the leader and resolves to the leader; a fork copies it, so
 * it is its own. `mark` is now only which of those two a row is.
 *
 * It still matters exactly as much as it did. A page belongs to an address
 * space, not to a context, so a page taken from one thread is missing for all
 * of them -- and answering "which address space" from this machine's /proc,
 * which is where it came from before, would be the destination deciding
 * something out of a file only Linux has.
 */
static uint64_t forked_from(uint64_t identity)
{
	struct execution_mm *parent=mm_record(identity)->parent;
	return parent ? parent->identity : 0;
}

/* Admission publishes the completed birth ownership metadata to COW workers.
 * Resolving an MM/object alone is not a committed child snapshot. */
struct cow_child {struct execution_mm *mm;struct cow_child *next;};
static struct cow_child *cow_children;
static pthread_mutex_t cow_children_lock=PTHREAD_MUTEX_INITIALIZER;
static void cow_admit_mm(struct execution_mm *mm)
{
	if(!mm->parent)return;
	pthread_mutex_lock(&cow_children_lock);
	for(struct cow_child *c=cow_children;c;c=c->next)if(c->mm==mm) {
		pthread_mutex_unlock(&cow_children_lock);return;
	}
	struct cow_child *child=malloc(sizeof(*child));
	if(!child)abort();
	*child=(struct cow_child){mm,cow_children};cow_children=child;
	pthread_mutex_unlock(&cow_children_lock);
}

/*
 * ---- executing a copy-on-write break -----------------------------------
 *
 * None of this decides anything. The source decides -- it ran the real clone3,
 * it holds the relationships between address spaces, and it says so by answering
 * VMR_CTXPAGE_COWBREAK. What is here is the doing of it, which has to be here
 * because the page is here: the parent's copy is in this machine's object and
 * the copy that has to be made is a local read and a local write, with nothing
 * crossing the wire.
 *
 * A break makes TWO pages, and that is what a fork means: the writer keeps the
 * page it is standing on and the copy gets one of its own with the same bytes.
 * It is not one page live at two sites -- that is the state BEFORE the break,
 * and ending it is the whole purpose.
 *
 * The given flag is this side's record of what it has done: "address space C has
 * been given its own copy of page P". It exists so a break is never performed
 * twice over a page C has since written, and it records an action rather than
 * an inference (PRINCIPLES §1).
 */
#define COW_GIVEN_SLOTS 65536
struct cow_record {
	uint64_t as,page;
	int given,state;
	struct cow_record *next;
};
/* Bucket count is an index, never a limit on retained COW facts. */
static struct cow_record *cow_record_buckets[COW_GIVEN_SLOTS];
static size_t cow_record_count,cow_record_peak;
static pthread_mutex_t cow_state_lock=PTHREAD_MUTEX_INITIALIZER;
/*
 * And the other half: the pages a fork write-protected, so that the store that
 * ends the sharing is a fault this side can see.
 *
 * A mark of its own, and the first attempt reused the `watched` one instead --
 * which reads exactly right ("this side protected this page in order to see the
 * next write") and was wrong twice over. Branch 6 GRANTS a watched page's write
 * on sight, so a fork protecting a page the program had made read-only turned
 * the program's own segmentation fault into a successful store (pf3: "the fault
 * never happened; the access was served"). And page_keep() reads the same mark
 * as "this side's copy is the current one", so marking a whole address space at
 * a fork took every fetch path's own-object shortcut away from it at once.
 *
 * This mark says one thing and licenses nothing: it routes the fault to the
 * source, which decides whether the write may land at all.
 */

/*
 * Two states, because the mark answers two different questions and conflating
 * them cost mf1 and ns1 -- every fork from a THREADED parent.
 *
 *   COW_PROT_OWED  the fork write-protected this page and nothing has been
 *                  stored into it since, so it IS what the copy inherited.
 *                  cow_give_page() needs this one.
 *   COW_PROT_SPENT it has been stored into. The copies have had it, so there is
 *                  nothing left to give -- but the page is still READ-ONLY in
 *                  every OTHER context of this address space, because
 *                  PROTECTOBJ write-protects the folio in every mapping while
 *                  MAPOBJ re-grants only the one that faulted. Branch 6 needs
 *                  this one, or a sibling's store falls through to
 *                  DECLINE("write-to-an-unlent-read-only-page") -- which the
 *                  guest answers by faulting on the same address until the
 *                  watchdog ends it. Measured on mf1: four identical declines
 *                  at 0x4d4000 and then the guest gone, 6 runs of 6, against
 *                  5 of 6 passing before the fork protection existed.
 *
 * A slot is never freed once used: "was under a fork's protection" has to stay
 * true for every sibling that has not faulted yet, and there is no moment at
 * which the last of them can be known to have done so.
 */
#define COW_PROT_OWED	1
#define COW_PROT_SPENT	2
static unsigned long n_cow_given, n_cow_break_asked, n_cow_break_nopage;
static unsigned long n_cow_protect_pages, n_cow_protect_fail, n_cow_protect_runs;
static unsigned long n_cow_protect_unmapped, n_cow_give_failed;
/* ABSENT answers served by MAPPING this address space's own folio, not copying it. */
static unsigned long n_absent_mapped;
static unsigned long n_cow_writeprep, n_cow_writeprep_break;

static unsigned cow_given_slot(uint64_t as, uint64_t page)
{
	return (unsigned)(((page >> 12) * 2654435761u) ^
			  ((as ^ (as >> 32)) * UINT64_C(2246822519))) % COW_GIVEN_SLOTS;
}

/* Caller holds cow_state_lock. No live record pointer escapes it. */
static struct cow_record *cow_record_find(uint64_t as,uint64_t page,int create)
{
	page &= ~UINT64_C(4095);
	unsigned bucket=cow_given_slot(as,page);
	for(struct cow_record *r=cow_record_buckets[bucket];r;r=r->next)
		if(r->as==as && r->page==page)return r;
	if(!create)return NULL;
	if(!as || cow_record_count==SIZE_MAX)abort();
	struct cow_record *r=calloc(1,sizeof(*r));
	if(!r) {
		fputs("vmremote: cannot retain COW ownership/protection records; ending execution\n",stderr);
		abort();
	}
	*r=(struct cow_record){.as=as,.page=page,.next=cow_record_buckets[bucket]};
	cow_record_buckets[bucket]=r;
	if(++cow_record_count>cow_record_peak) {
		cow_record_peak=cow_record_count;
		if(cow_record_peak>=8192 && !(cow_record_peak&(cow_record_peak-1)))
			fprintf(stderr,"vmremote: COW records=%zu bytes=%zu buckets=%u\n",
				cow_record_peak,cow_record_peak*sizeof(*r),COW_GIVEN_SLOTS);
	}
	return r;
}

static int cow_was_given(uint64_t as,uint64_t page)
{
	pthread_mutex_lock(&cow_state_lock);
	struct cow_record *r=cow_record_find(as,page,0);
	int given=r && r->given;
	pthread_mutex_unlock(&cow_state_lock);
	return given;
}

static void cow_give_note(uint64_t as,uint64_t page)
{
	pthread_mutex_lock(&cow_state_lock);
	struct cow_record *r=cow_record_find(as,page,1);
	if(!r->given) {r->given=1;n_cow_given++;}
	pthread_mutex_unlock(&cow_state_lock);
}

static int cow_prot_state(uint64_t as,uint64_t page)
{
	pthread_mutex_lock(&cow_state_lock);
	struct cow_record *r=cow_record_find(as,page,0);
	int state=r ? r->state : 0;
	pthread_mutex_unlock(&cow_state_lock);
	return state;
}

static int cow_is_protected(uint64_t as,uint64_t page)
{return cow_prot_state(as,page)==COW_PROT_OWED;}

static int cow_was_protected(uint64_t as,uint64_t page)
{return cow_prot_state(as,page)!=0;}

static void cow_prot_set(uint64_t as,uint64_t page,int state)
{
	if(state!=0 && state!=COW_PROT_OWED && state!=COW_PROT_SPENT)abort();
	pthread_mutex_lock(&cow_state_lock);
	struct cow_record *r=cow_record_find(as,page,state!=0);
	if(r)r->state=state;
	pthread_mutex_unlock(&cow_state_lock);
}

/* Called only when retiring this MM's COW history. All other MM keys and
 * both flags survive collision-chain removal. Runtime MM retirement still
 * has to prove that no peer, descendant or transfer owns this history. */
static inline void cow_forget_mm(uint64_t as)
{
	pthread_mutex_lock(&cow_state_lock);
	for(unsigned i=0;i<COW_GIVEN_SLOTS;i++) {
		struct cow_record **link=&cow_record_buckets[i],*r;
		while((r=*link)) {
			if(r->as==as) {*link=r->next;free(r);cow_record_count--;}
			else link=&r->next;
		}
	}
	pthread_mutex_unlock(&cow_state_lock);
}

/* The address spaces that were copied from this one, by the same record. */
static uint64_t *cow_copies_of(uint64_t identity,size_t *count)
{
	struct execution_mm *parent=mm_record(identity);
	pthread_mutex_lock(&cow_children_lock);
	size_t n=0;
	for(struct cow_child *c=cow_children;c;c=c->next)if(c->mm->parent==parent) {
		if(n==SIZE_MAX/sizeof(uint64_t))abort();
		n++;
	}
	uint64_t *children=n ? malloc(n*sizeof(*children)) : NULL;
	if(n && !children)abort();
	size_t at=0;
	for(struct cow_child *c=cow_children;c;c=c->next)
		if(c->mm->parent==parent)children[at++]=c->mm->identity;
	pthread_mutex_unlock(&cow_children_lock);*count=n;return children;
}

/*
 * Give one copied address space its own page, out of the one it was copied
 * from. Written into that address space's OBJECT, which is its memory: the
 * context need not exist yet, need not have faulted, and need not be running.
 *
 * Refused for a page the copy already has of its own -- by this side's record of
 * having given it, or by the object holding it -- because that page may have
 * been written since and the parent's bytes are then not older, they are wrong.
 */
static int cow_give_page_mode(uint64_t copy_as, uint64_t from_as, uint64_t base,
			      int relaxed)
{
	static __thread char cbuf[VMR_PG_SIZE];

	if (cow_was_given(copy_as, base) || mm_backing_has(copy_as, base))
		return 0;
	/*
	 * ...and only while this side can still vouch that the page IS what the
	 * copy inherited.
	 *
	 * That is what the fork's write-protection is: a page carrying it has
	 * not been stored into since the fork, because a store would have
	 * faulted and come through the break. A page WITHOUT it is one this
	 * side cannot speak for -- it was not here when the fork happened and
	 * arrived afterwards, or it has already been through a break -- and
	 * serving it as "the original's page" hands the copy memory as of now.
	 *
	 * Measured on tests/ex4, which forks and then mallocs in the child: the
	 * child was given the original's heap page holding main_arena, read a
	 * `top` chunk pointer belonging to a heap the original had grown after
	 * the fork, and died in _int_malloc dereferencing it. The page the child
	 * needed was on the source all along, in its own real fork of the
	 * original's task, and asking for it is what this refusal makes happen.
	 *
	 * ...unless the asking has ALREADY happened and failed everywhere --
	 * which is what `relaxed` says, and only the ASKER can say it. The
	 * source escalates a COWBREAK to last-resort (rq.len == 1) after its
	 * own routes -- the child's kernel COW, the pagemap-walked ancestor
	 * chain -- came up empty, and this side then serves the nearest
	 * ancestor page it can READ, protected or not: bytes-current-at-
	 * break, the same semantics every give already has, against the
	 * alternative of zeros that a fork copy is never owed. ex4's hazard
	 * (bytes grown after the fork) cannot reach here: its page is served
	 * by the source's kernel COW long before anything escalates.
	 */
	if (!relaxed && !cow_is_protected(from_as, base))
		return 0;
	if (!mm_page_read(from_as, base, cbuf)) {
		n_cow_break_nopage++;
		if (n_cow_break_nopage <= 8)
			fprintf(stderr, "[vmremote] the source asked for a "
				"copy-on-write break of 0x%llx from address "
				"space %d and this side has no such page: "
				"address space %d is not given one rather than "
				"being given something invented\n",
				(unsigned long long)base, (int)from_as,
				(int)copy_as);
		return 0;
	}
	if (mm_obj_write(copy_as, base, cbuf, VMR_PG_SIZE) != (long)VMR_PG_SIZE)
		return 0;
	cow_give_note(copy_as, base);
	/*
	 * The copy's object now holds the page with no PTE over it -- the very
	 * state the fork-time protect pass marks OWED when it finds a page the
	 * object holds unmapped (its r == 0 arm). Mark it the same, because it
	 * answers the same two questions the same way: a copy OF THE COPY
	 * inherits exactly these bytes (cow_give_page can now serve it), and
	 * the copy's own first store spends the mark through the break like
	 * any other. Without this, the original's break reached its DIRECT
	 * copies only, and a grandcopy that faulted later found every ancestor
	 * either spent or empty-handed -- the pristine-file/zeros death one
	 * generation removed.
	 */
	cow_prot_set(copy_as, base, COW_PROT_OWED);
	plog(base, "copy-on-write broken here: address space %d now has its own "
	     "copy of address space %d's page (sum=%08x%s)%s", (int)copy_as,
	     (int)from_as, page_sum(cbuf), pgword(cbuf),
	     relaxed ? " [last-resort: unprotected ancestor]" : "");
	return 1;
}

static int cow_give_page(uint64_t copy_as, uint64_t from_as, uint64_t base)
{
	return cow_give_page_mode(copy_as, from_as, base, 0);
}

/*
 * Give a copy its page from the NEAREST ancestor that can vouch for it.
 *
 * A fork of a fork inherits pages its own parent has never held: at fork(C),
 * a page B itself still inherits from A is in neither B's page tables nor
 * B's object, so the protect pass marked nothing for (B, page) and "B owes C
 * nothing" is true of B's record and false of the chain. The bytes are not
 * ambiguous: an undiverged page has ONE set of inherited bytes for the whole
 * subtree, because the first store anywhere comes through the break and
 * gives first -- so the nearest ancestor still holding the fork's OWED mark
 * holds exactly what C inherited, and serving from it is the record
 * speaking, not a guess. Walking PAST an ancestor whose page will not serve
 * (unreadable, or never protected) is equally safe: any ancestor that does
 * pass cow_is_protected holds bytes unwritten since before every fork below
 * it.
 */
static int cow_give_from_chain_mode(uint64_t copy_as, uint64_t base, int relaxed)
{
	uint64_t anc;
	size_t hops = 0;

	if (cow_was_given(copy_as, base) || mm_backing_has(copy_as, base))
		return 0;
	for (anc = forked_from(copy_as); anc;
	     anc = forked_from(anc), hops++)
		if (cow_give_page_mode(copy_as, anc, base, relaxed)) {
			if (relaxed && ++n_cow_relaxed_given <= 8)
				fprintf(stderr, "[vmremote] 0x%llx for address "
					"space %d: LAST-RESORT give from "
					"ancestor %d (%zu hop(s) up, protect "
					"mark not required) -- both machines' "
					"records were empty-handed and a fork "
					"copy is never owed zeros\n",
					(unsigned long long)base,
					(int)copy_as, (int)anc, hops);
			else if (hops && ++n_cow_chain_given <= 8)
				fprintf(stderr, "[vmremote] 0x%llx for address "
					"space %d: given by ancestor %d, %zu "
					"hop(s) above the parent, which never "
					"held it\n", (unsigned long long)base,
					(int)copy_as, (int)anc, hops);
			return 1;
		}
	return 0;
}

static int cow_give_from_chain(uint64_t copy_as, uint64_t base)
{
	return cow_give_from_chain_mode(copy_as, base, 0);
}

/*
 * Every address space copied from this one that has not been given this page
 * gets it now, with the bytes that are there at this instant. Called where the
 * page is about to stop being what the copies inherited: a write by the address
 * space that owns it, or a hand-over that takes it off this machine.
 */
/*
 * Returns 0 if any copy that is still owed this page could not be given it.
 * The caller must then leave the mark OWED -- spending it would make the page
 * unreachable for that copy for ever, which is a page of zeros with a delay on
 * it -- and say so, because a break that cannot be performed is the one thing
 * here that loses memory.
 */
static int cow_break_copies(memory_target ctx,uint64_t base)
{
	uint64_t as=as_id(ctx);size_t count;
	/* The source retains lineage after earlier snapshots have settled and
	 * may ask for the same break again. SPENT records completed preservation;
	 * it cannot make the current bytes an outstanding fork snapshot. A later
	 * fork explicitly re-arms OWED before releasing the parent. */
	if (cow_prot_state(as,base)==COW_PROT_SPENT)return 1;
	uint64_t *children=cow_copies_of(as,&count);
	int all=1;
	for(size_t i=0;i<count;i++) {
		if(cow_was_given(children[i],base) || mm_backing_has(children[i],base))continue;
		if(!cow_give_page(children[i],as,base)) {
			all=0;n_cow_give_failed++;
			fprintf(stderr,"vmremote: cannot preserve child MM %llu page %llx before MM %llu writes\n",
				(unsigned long long)children[i],(unsigned long long)base,(unsigned long long)as);
		}
	}
	free(children);return all;
}

/* The source already published these private snapshot relationships. Mapping
 * an existing local folio writable is an architectural permission grant, not
 * an OS operation. Preserve every outstanding copy before making that grant;
 * a failed copy leaves protection intact and terminates the run. The recursive
 * coherence lock orders the copy, its record, and the PTE installation against
 * other local page service workers. No network operation runs in this scope. */
static long snapshot_map_object(memory_target pid, void *arg)
{
	const struct vmctx_mem *m = arg;
	uint64_t page = m->addr & ~0xfffULL;
	uint64_t as = as_id(pid);
	long result;

	coh_enter();
	if (cow_is_protected(as, page)) {
		if (!cow_break_copies(pid, page)) {
			fprintf(stderr, "vmremote: cannot preserve snapshot before MAPOBJ MM %llu page 0x%llx\n",
				(unsigned long long)as, (unsigned long long)page);
			_exit(97);
		}
		cow_prot_set(as, page, COW_PROT_SPENT);
		plog(page, "snapshot preserved before writable object mapping as=%llu", (unsigned long long)as);
	}
	result = execution_context_ctl(pid, VMCTX_CTL_MAPOBJ, arg);
	coh_leave();
	return result;
}

/* A handover permits the source kernel to write the page. Preserve the same
 * private snapshots as a local writable-map grant, before the last executor
 * copy leaves. The source must not manufacture a second child copy on receipt.
 * All native takes, including object-only and fallback paths, pass here. */
static long snapshot_take_page(memory_target pid, unsigned cmd, void *arg)
{
	const struct vmctx_mem *m = arg;
	uint64_t page = m->addr & ~UINT64_C(4095);
	uint64_t as = as_id(pid);
	long result;
	coh_enter();
	if (cow_is_protected(as, page) && !cow_break_copies(pid, page)) {
		fprintf(stderr, "vmremote: cannot preserve snapshot before TAKE MM %llu page 0x%llx\n",
			(unsigned long long)as, (unsigned long long)page);
		_exit(97);
	}
	result = execution_context_ctl(pid, cmd, arg);
	if (result == VMR_PG_SIZE) cow_prot_set(as, page, COW_PROT_SPENT);
	coh_leave();
	return result;
}

/*
 * The same, over a range that is about to cease to exist. Bounded by what the
 * fork actually protected: an address never marked is one this side cannot
 * vouch for anyway, and the lookup that says so costs no I/O.
 */
static void cow_break_copies_range(memory_target ctx,uint64_t start,uint64_t len)
{
	uint64_t as=as_id(ctx);size_t count;
	uint64_t *children=cow_copies_of(as,&count);
	if(len>UINT64_MAX-start)abort();
	for(uint64_t page=start;len && page<start+len;page+=VMR_PG_SIZE) {
		if(!cow_is_protected(as,page))continue;
		for(size_t i=0;i<count;i++) {
			if(cow_was_given(children[i],page) || mm_backing_has(children[i],page))continue;
			if(!cow_give_page(children[i],as,page)) {
				fputs("vmremote: cannot preserve child MM before range retirement\n",stderr);abort();
			}
		}
		cow_prot_set(as,page,COW_PROT_SPENT);
		if(page>UINT64_MAX-VMR_PG_SIZE)break;
	}
	free(children);
}

/*
 * Write-protect everything this machine holds of an address space, so that its
 * next store to any of it faults and can be routed through the break above.
 *
 * This is what a fork costs and it is not an optimisation to skip: without it
 * the parent goes on writing pages the copy has not been given, and the copy is
 * handed its parent's memory as of its own first touch instead of as of the
 * fork. A real kernel does exactly this and charges exactly the same -- one
 * fault per page actually written.
 *
 * The object is walked rather than /proc/pid/maps: an address space's memory IS
 * its object, the object's own SEEK_DATA says which pages it holds, and that
 * asks nothing about the context process, whose own mappings (vmremote's text,
 * its stack) are none of this. VMCTX_CTL_PROTECTOBJ refuses anything that is not
 * object-backed anyway, so the walk cannot reach them even by accident.
 */
static int region_snapshot(memory_target pid, uint64_t addr, struct region *out);

static int cow_protect_address_space(memory_target ctx)
{
	int fd = backing_find(ctx);
	unsigned long pages = 0;
	off_t off = 0;

	if (fd < 0)
		return -1;
	/*
	 * There was a VMR_FORK_PROTECT=0 arm here and it is gone, because it has
	 * done its job: it separated the write-protection from the rest of the
	 * break in one run, and said the protection was NOT what mf1 was dying
	 * of (with it off, mf1 was 0 of 6 rather than 3 of 6). Keeping it would
	 * be keeping "hand the copy memory as of its own first touch" as a
	 * runtime option, and correctness does not get a switch.
	 */
	n_cow_protect_runs++;
	for (;;) {
		off_t d = lseek(fd, off, SEEK_DATA), e, p;

		if (d < 0) {
			if (errno == ENXIO) break;
			return -1;
		}
		e = lseek(fd, d, SEEK_HOLE);
		if (e <= d) {
			if (e >= 0) errno = EIO;
			return -1;
		}
		for (p = d & ~(off_t)0xfff; p < e; p += VMR_PG_SIZE) {
			struct vmctx_mem m = { .addr = (uint64_t)p,
					       .len = VMR_PG_SIZE, .buf = 0 };
			long r = -1;

			/*
			 * A range the owner called SHARED is shared on purpose
			 * and across the fork on purpose -- that is what
			 * MAP_SHARED means -- so breaking it would be the defect
			 * rather than the repair. tests/sm1 fork-shared is the
			 * measurement: a page the child writes that the parent
			 * must see.
			 *
			 * Private read-only pages still belong to the snapshot.
			 * The source may no longer hold their relocated bytes,
			 * and a later permission change or handover can permit a
			 * write. Record the obligation while these bytes are
			 * available; current read-only permission cannot replace
			 * that history. Native protection grants no new access.
			 */
			if (shared_range_has(ctx, (uint64_t)p))
				continue;

			r = ctl(ctx, VMCTX_CTL_PROTECT_BACKING, &m);
			/*
			 * -EBUSY is "not yet", not "no" (PRINCIPLES §2): the
			 * kernel refuses to write-protect a page an in-flight
			 * syscall holds or a device has pinned, and both clear
			 * in milliseconds. A page left unprotected is one the
			 * original can write without the copy being given it
			 * first, so it is worth a bounded wait rather than a
			 * counter.
			 */
			for (int b = 0; r < 0 && errno == EBUSY && b < 100; b++) {
				usleep(1000);
				r = ctl(ctx, VMCTX_CTL_PROTECT_BACKING, &m);
			}
			if (r == (long)VMR_PG_SIZE) {
				/*
				 * Recorded with a mark of its own: this page is
				 * read-only because an address space was copied
				 * from this one, and nothing else. Branch 6
				 * reads it, asks the source what the store means
				 * and what is owed, and does what it is told --
				 * so the protection is spent by the first store
				 * and by nothing else, and a page nobody writes
				 * is never copied at all.
				 */
				cow_prot_set(as_id(ctx), (uint64_t)p,
					     COW_PROT_OWED);
				pages++;
			} else if (r == 0) {
				/*
				 * The object holds the page and no context maps
				 * it, so there is no PTE to write-protect --
				 * PROTECTOBJ says so with a 0 rather than an
				 * error. The page is still the copy's to inherit
				 * and still the original's to change, so it is
				 * marked anyway: the mark is what lets
				 * cow_give_page() serve it, and the first touch
				 * that maps it is a fault the serve path can
				 * route (see the write-through-the-object check
				 * there).
				 */
				cow_prot_set(as_id(ctx), (uint64_t)p,
					     COW_PROT_OWED);
				n_cow_protect_unmapped++;
			} else if (r < 0) {
				n_cow_protect_fail++;
				fprintf(stderr, "vmremote: snapshot cannot protect MM %llu page 0x%llx: %s\n",
					(unsigned long long)as_id(ctx), (unsigned long long)p, strerror(errno));
				return -1;
			}
			n_cow_protect_pages++;
		}
		off = e;
	}
	fprintf(stderr, "[vmremote] address space %d was copied: %lu page(s) of "
		"it write-protected, so the copy is given each one before the "
		"original changes it\n", (int)as_id(ctx), pages);
	return 0;
}


static pthread_mutex_t map_lock = PTHREAD_MUTEX_INITIALIZER;

/* A caller owns the copied description across callbacks. No pointer into the
 * mutable layout survives map_lock, and every writer takes the same lock. */
static int region_snapshot(memory_target pid, uint64_t addr, struct region *out)
{
	uint64_t as = as_id(pid);
	int found = 0;
	pthread_mutex_lock(&map_lock);
	for (int i = nregions - 1; i >= 0; i--)
		if (regions[i].as == as &&
		    addr >= regions[i].start && addr < regions[i].end) {
			if (out) *out = regions[i];
			found = 1;
			break;
		}
	pthread_mutex_unlock(&map_lock);
	return found;
}

static struct region *region_snapshot_all(memory_target target, int *count)
{
	uint64_t identity = as_id(target);
	pthread_mutex_lock(&map_lock);
	*count = 0;
	for (int i=0; i<nregions; i++)
		if (regions[i].as==identity && regions[i].start<regions[i].end) (*count)++;
	struct region *copy = *count ? malloc((size_t)*count * sizeof(*copy)) : NULL;
	if (*count && !copy) {fputs("vmremote: cannot snapshot layout\n",stderr);abort();}
	int n=0;
	for (int i=0; i<nregions; i++)
		if (regions[i].as==identity && regions[i].start<regions[i].end) copy[n++]=regions[i];
	pthread_mutex_unlock(&map_lock);
	return copy;
}


/*
 * A fork copies the address space, layout included. The child's regions are
 * the parent's AT THE FORK -- recorded, not shared: the two diverge from here
 * (the child's exec drops only its own).
 */
static void region_inherit(memory_target kid, memory_target parent)
{
	uint64_t kas = as_id(kid), pas = as_id(parent);
	int i, n;

	if (kas == pas)
		return;			/* a thread: same address space */
	pthread_mutex_lock(&map_lock);
	n = nregions;
	for (i = 0; i < n; i++) {
		if (regions[i].as != pas || regions[i].end <= regions[i].start)
			continue;
		if (nregions >= MAX_REGIONS)
			region_overflow();
		regions[nregions] = regions[i];
		regions[nregions].as = kas;
		nregions++;
	}
	pthread_mutex_unlock(&map_lock);
}

/*
 * There was an address allocator here, because this machine used to choose
 * where a library went: it rewrote the guest's mmap into an anonymous one and
 * had to name the address itself. It chooses nothing now. The source performs
 * the mmap, in its own address space, and the address it returns is the guest's.
 */
static unsigned n_chunk_dumps;
static unsigned chunk_dump_max = 12;	/* VMREMOTE_CHUNK_DUMPS */
/* VMREMOTE_CHILD_FAULTS; VMREMOTE_TRACE_FAULTS lifts it entirely. */
static unsigned long child_fault_max = 30;

/*
 * The perms column of a /proc/maps line, as PROT_* bits.
 *
 * "r-xp" and friends. The 'p'/'s' is not a protection and is ignored here; a
 * range with no permission at all comes back 0, which is PROT_NONE and is a
 * real answer -- a guard page is exactly that, and mapping it readable is how a
 * stack overflow used to scribble over whatever was below it instead of dying.
 */
static int prot_of_perms(const char *perms)
{
	int prot = 0;

	if (perms[0] == 'r')
		prot |= PROT_READ;
	if (perms[1] == 'w')
		prot |= PROT_WRITE;
	if (perms[2] == 'x')
		prot |= PROT_EXEC;
	return prot;
}

static void region_add(uint64_t as, uint64_t start, uint64_t end, int prot)
{
	pthread_mutex_lock(&map_lock);
	if (nregions >= MAX_REGIONS)
		region_overflow();
	regions[nregions].start = start;
	regions[nregions].end = end;
	regions[nregions].prot = prot;
	regions[nregions].as = as;
	nregions++;
	pthread_mutex_unlock(&map_lock);
}

/* The source serializes address ranges and access bits. Extra source
 * diagnostics in a layout line are ignored; no OS mapping name has meaning. */
static void region_parse_layout(uint64_t as, char *text)
{
	char *line = text;
	while (line && *line) {
		char *nl = strchr(line, '\n');
		unsigned long st, en;
		char perms[8];

		if (nl)
			*nl = '\0';
		if (sscanf(line, "%lx-%lx %7s", &st, &en, perms) == 3)
			region_add(as, st, en, prot_of_perms(perms));
		line = nl ? nl + 1 : NULL;
	}
}

/*
 * After a successful exec: the old regions are gone (the caller dropped them)
 * and the NEW image's map is fetched from home -- args[0]==1 asks for the
 * calling context's own current map, not the oracle's. Without this a fresh
 * child had NO region for [vvar], and its first vdso-data read went to the
 * source, which cannot read vvar on any kernel, and came back as the
 * program's own SIGSEGV -- six identical child deaths per firefox run at
 * rip 0x7ffff7fcdbe3, fault 0x7ffff7fb6188 err 0x4.
 */
static void relayout_from_home(memory_target pid)
{
	static __thread char text[192 * 1024];
	uint64_t a[6] = { 1, 0, 0, 0, 0, 0 };
	long r;

	r = home_memory_call(pid, VMR_OP_LAYOUT, a, NULL, 0, text, sizeof(text) - 1,NULL);
	if (r <= 0) {
		fprintf(stderr, "[vmremote] context %d: no relayout from home "
			"after its exec (%ld); every fault there now asks the "
			"source\n", ctx_id(pid), r);
		return;
	}
	text[r] = '\0';
	region_parse_layout(as_id(pid), text);
}

/*
 * There was a truncation rule here for the loader's over-reaching first mapping
 * of an object -- the one it makes to reserve the whole thing before mapping
 * each segment over it. It existed because this machine filled such a range
 * from the file at that range's own offset, and past the first segment that
 * offset means nothing. Nothing computes an offset into a file any more: the
 * bytes for an address come from the source's copy of that address, and the
 * source's own segment mappings already sit on top of its reservation.
 */

/*
 * Anonymous memory the guest maps over a range we were serving from a file.
 * A library's .bss is exactly this: the loader reserves the whole object, then
 * maps anonymous zero pages over the part that has no file backing. If we keep
 * answering those addresses from the file we hand the program whatever bytes
 * happen to live at that offset instead of zeroes — and a lock word that should
 * start at zero arrives non-zero, so the first lock attempt waits on a futex
 * for an unlock that can never come.
 */
/*
 * Forget any part of the program-as-loaded that the guest has mapped over.
 *
 * regions[] describes the address space home handed us at start-up. When the
 * guest maps something of its own at one of those addresses, the old entry is
 * no longer a description of anything -- but it is still what region_snapshot()
 * finds, and fault_note_prot() then stamps the *old* range's protection onto a
 * page of the new mapping. A thread stack that lands in what used to be a
 * read-only segment becomes read-only, the thread dies on its first push, and
 * the process that created it dereferences the null it gets back.
 *
 * There used to be a region_truncate_at() that did this for one case. This does
 * it for the general one, because a guest mapping is not obliged to line up
 * with anything.
 */
/* All region readers copy under map_lock; every writer takes that lock. */

static void region_forget_range(memory_target pid, uint64_t st, uint64_t en)
{
	uint64_t as = as_id(pid);
	int i;

	if (en <= st)
		return;
	pthread_mutex_lock(&map_lock);
	for (i = 0; i < nregions; i++) {
		struct region *r = &regions[i];

		if (r->as != as || r->end <= st || r->start >= en)
			continue;		/* another address space / no overlap */
		if (r->start >= st && r->end <= en) {
			r->start = r->end = 0;	/* wholly covered */
		} else if (r->start < st && r->end > en) {
			/*
			 * The new mapping is strictly inside this one, so the
			 * region becomes two: the part below it and the part
			 * above. Keeping only the part below -- which is what
			 * this did at first -- throws away the source's claim
			 * on every address above the new mapping, and a fault
			 * up there then finds no region at all. That is not a
			 * quiet loss: with no region the fault goes to the
			 * owner, the owner says the program may make the
			 * access, this side declines, and the local kernel
			 * answers it with a page of zeros. A thread whose
			 * stack pthread placed in the middle of a mapped range
			 * read zeros for everything above it and exited before
			 * its first round.
			 */
			if (nregions >= MAX_REGIONS)
				region_overflow();
			regions[nregions] = *r;
			regions[nregions].start = en;
			nregions++;
			r->end = st;
		} else if (r->start < st) {
			r->end = st;		/* keep the part below */
		} else {
			r->start = en;		/* keep the part above */
		}
	}
	pthread_mutex_unlock(&map_lock);
}

/*
 * There is no LOCAL kind of region and no [stack] special case. A stack is a
 * label in a text file, not a kind of memory: every address is asked of the
 * source alike, and the source's kernel -- not a maps parse -- says whether
 * an address is real (VMCTX_CTL_TRYFAULT). The 8-MiB-headroom server that
 * used to live here, its `local` region flag, and the fault-path branch that
 * served zeros for it are all gone; see git history for what each cost.
 */

/* Ask home to resolve one page and install it in the guest. */
/*
 * Resolve one guest fault from home.
 *
 * Only *missing* pages are ours to fill. If the page is already present
 * (x86 page-fault error bit 0), this is a protection fault — a write to a
 * read-only or copy-on-write page — and installing contents would not change
 * anything: the guest would fault on the same instruction forever. Those belong
 * to the local kernel, which does the COW.
 */
#define FAULT_CHUNK VMR_PG_SIZE /* one reservation, one source transfer */

/*
 * Which pages we have already installed. Fetching a chunk around a fault is
 * what makes start-up bearable, but a chunk may cover pages that are already
 * there — and those pages are not pristine any more: the loader has applied
 * relocations to them. Writing file contents over a relocated page replaces a
 * pointer with the zero that was in the file, which is exactly how a program
 * ends up dereferencing null in libc's start-up path.
 */
#define INSTALLED_SLOTS 65536 /* initial capacity */
enum installed_state { INST_EMPTY, INST_LIVE, INST_TOMB };
struct installed_page {
	uint64_t page;
	memory_target context;
	enum installed_state state;
};
static struct installed_page *installed;
static size_t installed_capacity, installed_live, installed_occupied;
static unsigned long installed_rehashes;
static pthread_mutex_t installed_lock = PTHREAD_MUTEX_INITIALIZER;

static size_t installed_hash(memory_target pid, uint64_t page, size_t capacity)
{
	return (((page >> 12) * 2654435761u) ^
		((uint64_t)(uintptr_t)pid * UINT64_C(2246822519))) & (capacity - 1);
}

/* Caller holds installed_lock; no pointer into the array escapes it.
 * Rehashing retains full keys and discards tombstones. Allocation failure
 * cannot be answered as "not installed": that permits overwriting live data. */
static void installed_rehash(size_t capacity)
{
	struct installed_page *next;

	if (!capacity || capacity > SIZE_MAX / sizeof(*next))
		goto failed;
	next = calloc(capacity, sizeof(*next));
	if (!next)
		goto failed;
	for (size_t i = 0; i < installed_capacity; i++) {
		struct installed_page *old = &installed[i];
		size_t at;

		if (old->state != INST_LIVE)
			continue;
		at = installed_hash(old->context, old->page, capacity);
		while (next[at].state == INST_LIVE)
			at = (at + 1) & (capacity - 1);
		next[at] = *old;
	}
	free(installed);
	installed = next;
	installed_capacity = capacity;
	installed_occupied = installed_live;
	installed_rehashes++;
	fprintf(stderr, "vmctx: installation table capacity=%zu live=%zu bytes=%zu rehashes=%lu\n",
		capacity, installed_live, capacity * sizeof(*next), installed_rehashes);
	return;
failed:
	fputs("vmctx: cannot grow the installation table; refusing an unrecorded install\n", stderr);
	abort();
}

/* Under installed_lock, with a nonempty allocation. Probe past tombstones
 * before reusing one: the same key may already appear later in the chain.
 * At most half the slots are occupied, so a terminating empty slot exists. */
static struct installed_page *installed_find(memory_target pid, uint64_t page, int *found)
{
	size_t at = installed_hash(pid, page, installed_capacity);
	struct installed_page *reuse = NULL;

	*found = 0;
	while (installed[at].state != INST_EMPTY) {
		struct installed_page *entry = &installed[at];

		if (entry->state == INST_LIVE && entry->context == pid &&
		    entry->page == page) {
			*found = 1;
			return entry;
		}
		if (entry->state == INST_TOMB && !reuse)
			reuse = entry;
		at = (at + 1) & (installed_capacity - 1);
	}
	return reuse ? reuse : &installed[at];
}

/* Query only; never create a record or fault in the page being checked. */
static int page_is_installed(memory_target pid, uint64_t page)
{
	int found = 0;

	pthread_mutex_lock(&installed_lock);
	if (installed_capacity)
		(void)installed_find(pid, page & ~UINT64_C(4095), &found);
	pthread_mutex_unlock(&installed_lock);
	return found;
}

static int page_installed(memory_target pid, uint64_t page)
{
	struct installed_page *entry;
	int found;

	page &= ~UINT64_C(4095);
	pthread_mutex_lock(&installed_lock);
	if (!installed_capacity)
		installed_rehash(INSTALLED_SLOTS);
	entry = installed_find(pid, page, &found);
	if (!found) {
		if (installed_occupied >= installed_capacity / 2) {
			/* Compact if deletions dominate; otherwise grow. */
			size_t cap = installed_capacity;

			if (installed_live >= cap / 4) {
				if (cap > SIZE_MAX / 2)
					installed_rehash(0); /* fails closed */
				cap *= 2;
			}
			installed_rehash(cap);
			entry = installed_find(pid, page, &found);
		}
		if (entry->state == INST_EMPTY)
			installed_occupied++;
		*entry = (struct installed_page) {
			.page = page, .context = pid, .state = INST_LIVE,
		};
		installed_live++;
	}
	pthread_mutex_unlock(&installed_lock);
	return found;
}

static void installed_remove(memory_target pid, uint64_t page)
{
	int found;

	pthread_mutex_lock(&installed_lock);
	if (installed_capacity) {
		struct installed_page *entry = installed_find(pid,
			page & ~UINT64_C(4095), &found);

		if (found) {
			entry->state = INST_TOMB;
			installed_live--;
		}
	}
	pthread_mutex_unlock(&installed_lock);
}

static inline void installed_forget_context(memory_target pid)
{
	pthread_mutex_lock(&installed_lock);
	for (size_t i = 0; i < installed_capacity; i++) {
		struct installed_page *entry = &installed[i];

		if (entry->state == INST_LIVE && entry->context == pid) {
			entry->state = INST_TOMB;
			installed_live--;
		}
	}
	pthread_mutex_unlock(&installed_lock);
}

/*
 * A page the syscall side wrote back is now the live copy — newer than the file
 * it came from. Record it, or a later chunk fetch would overwrite it with
 * pristine bytes and undo the write (a lock word reverting to "held" is how
 * that surfaces).
 */
static void mark_installed(memory_target pid, uint64_t addr, uint64_t len)
{
	for (uint64_t p = addr & ~0xfffUL; p < addr + len; p += 4096)
		(void)page_installed(pid, p);
}

/*
 * ...and the half that was missing: this side no longer holds the page.
 *
 * The record was set-only, so "installed" meant "installed at some point in
 * the past" for the rest of the run, and the refill test paired it with the
 * page tables -- which cannot be trusted for this, because vmctx_back_fault()
 * maps the faulting page from the backing object *before* the monitor is
 * asked, and a hand-over has punched a hole in that object. A hole reads as
 * zeros, so the page is PRESENT and empty, the pair "installed and present"
 * is satisfied, the refill is skipped, and the guest resumes on zeros.
 *
 * Measured on pg3: 1555 faults where the module read the guest's own mapping
 * as zero immediately after the monitor answered "served" with no data in the
 * reply, and a guest that returned from a successful read(2) through a zeroed
 * return address to rip 0.
 *
 * Presence is not ownership. This side owns a page's contents from the moment
 * it installs them until the moment it hands them over, and that second event
 * is what this records. After it, the page is the source's, and the next fault
 * on it must fetch -- whatever the page tables happen to say.
 */
static void mark_handed_over(memory_target pid, uint64_t page)
{
	/*
	 * The watch mark goes with the page.
	 *
	 * `watched` means "this side write-protected this page so it would see
	 * the next write to it" -- a statement about a page this side HAS. A
	 * hand-over is the moment it stops having it, so the mark stops being
	 * true here, and leaving it set is the record lying about the one thing
	 * it exists to record.
	 *
	 * It was left set, and merely MASKED while a loan existed:
	 * page_watched() returns `lent[slot].owner ? 0 : lent[slot].watched`, so
	 * the lie was invisible exactly as long as the loan lasted and surfaced
	 * the instant anything cleared the loan without the page coming back --
	 * which fault_from_home_mode does unconditionally when the source
	 * answers NOTHOLDER.
	 *
	 * The state that leaves, (holder 0, watched 1, present 0), is
	 * ABSORBING. The mark is cleared in exactly two places -- inside
	 * page_make_writable() after its poke succeeds, and on the protection-
	 * fault path -- and BOTH require the page to be present, while every
	 * path that could make it present again is gated on page_keep(), which
	 * is true because of the mark. Measured, that is where netsurf dies:
	 *
	 *   [stuck] present=0 installed=0
	 *   [stuck] this side's record: holder=0, watched=1, the object HAS
	 *           the bytes
	 *
	 * -- the destination refusing to read its own object, which is the one
	 * place the bytes are, because a stale mark says its copy is current.
	 *
	 * Cleared here rather than in page_lend_at(): this is the hand-over, the
	 * one event that means "gone from this side", and it is already the
	 * place installed[] is tombstoned for the same reason.
	 */
	page_watch_set(pid, page, 0);

	installed_remove(pid, page);
}

/*
 * Every page of a region whose contents live on the other machine that is
 * present here but was never installed by us. Nobody fetched it, so whatever
 * the guest reads there is this kernel's zeros. Naming those pages is the only
 * way to find who populated them: the act itself leaves no other trace.
 */
static char child_layout[VMR_LAYOUT_MAX];	/* defined below; named here */
static uint64_t sweep_hits[64];
static int n_sweep_hits;

static int sweep_hit(uint64_t start)
{
	int i;

	for (i = 0; i < n_sweep_hits; i++)
		if (sweep_hits[i] == start)
			return 1;
	return 0;
}

static void sweep_unfetched(memory_target who)
{
	int i, shown = 0, count;
	struct region *rows = region_snapshot_all(who, &count);
	unsigned long bad = 0;

	for (i = 0; i < count; i++) {
		uint64_t p;

		for (p = rows[i].start; p < rows[i].end; p += 4096) {
			/*
			 * Only a page this machine can say is present.
			 *
			 * PAGE_UNKNOWN is -1, which read as a boolean is true,
			 * so every page whose pagemap could not be read was
			 * swept as present-but-never-fetched -- and after an
			 * execve that is thousands of them. The sweep then names
			 * a list of addresses as evidence of a bug that may not
			 * exist, which is the most expensive kind of wrong
			 * output there is. Not knowing is not evidence.
			 */
			if (page_present(who, p) != PAGE_PRESENT)
				continue;
			{	/* installed by *any* context: the record is per
				 * pid, and a thread shares this address space */
				int c, got = 0;
                memory_target members[MAX_CTX];
                int member_count = as_members(who, members, MAX_CTX);

				for (c = 0; c < member_count && !got; c++)
					if (page_is_installed(members[c], p))
						got = 1;
				if (got || page_is_installed(who, p))
					continue;
			}
			bad++;
			if (n_sweep_hits < 64) {
				int k, seen = 0;

				for (k = 0; k < n_sweep_hits; k++)
					if (sweep_hits[k] == rows[i].start)
						seen = 1;
				if (!seen)
					sweep_hits[n_sweep_hits++] = rows[i].start;
			}
			if (shown++ < 400)
				fprintf(stderr, "[sweep] 0x%llx present but never "
					"fetched (region 0x%llx-0x%llx)\n",
					(unsigned long long)p,
					(unsigned long long)rows[i].start,
					(unsigned long long)rows[i].end);
		}
	}
	fprintf(stderr, "[sweep] %lu page(s) present but never fetched\n", bad);
	{
		/*
		 * And the line of the source's own map that each fell in, as
		 * the source wrote it. Echoed, not interpreted: this side reads
		 * two addresses out of it to match the region and prints the
		 * rest verbatim, exactly as it would print any other diagnostic
		 * string the owner sent.
		 */
		char *line = child_layout, *nl;

		while (line && *line) {
			unsigned long a = 0, b = 0;

			nl = strchr(line, '\n');
			if (sscanf(line, "%lx-%lx", &a, &b) == 2) {
				int i;

				for (i = 0; i < count; i++)
					if (rows[i].start >= a &&
					    rows[i].start < b && sweep_hit(rows[i].start)) {
						if (nl)
							*nl = '\0';
						fprintf(stderr, "[sweep] region is: %s\n", line);
						if (nl)
							*nl = '\n';
						break;
					}
			}
			line = nl ? nl + 1 : NULL;
		}
	}
	free(rows);
}

/*
 * For a context this monitor created itself (VMCTX_SOURCE_CLONE), the parent it
 * was cloned from. A forked child's memory is its parent's as of the fork, and
 * the parent is a context running here -- so a page the parent already holds is
 * fetched from it directly. Reading a page of a context it runs is something any
 * destination can do; it is what PEEK is, and what the page channel already
 * exposes to home. What a fork *means* -- which pages, and when -- stays decided
 * here rather than by the destination's kernel.
 *
 * Without this the child is served from the oracle, which holds the program as it
 * was loaded and has never seen a stack frame, so the child dies on the first
 * dereference with [rsp] all zeros.
 */
#define MAX_SRC_CLONE 4096
static struct { pid_t child, parent; } src_clone[MAX_SRC_CLONE];
static int nsrc_clone;
static unsigned long n_unmapped_served;
/*
 * Generic lost-write detector (task 13), high-water-mark form. hx2's writer
 * slots are monotonic counters, so the truth is a per-slot maximum ever seen.
 * clobber_check() raises the mark from any page this side reads/installs,
 * AND flags a value that would move a slot BELOW its mark -- the store being discarded, caught even when the value it overwrites is
 * a hole (a take already emptied the object, so a byte-vs-object compare cannot
 * see it). A small base-keyed table; best-effort under a coarse lock, which is
 * enough to name the site. site names the caller.
 */
#define HWM_N 256
static struct { uint64_t base; unsigned long hw[VMR_PG_SIZE / sizeof(long)]; }
	hwm_tab[HWM_N];
static pthread_mutex_t hwm_lock = PTHREAD_MUTEX_INITIALIZER;
/* The last thing that put content at the watched page, for SERVEDSTALE. */
static int last_write_site;
static unsigned long last_write_val, last_write_us;
static unsigned long last_take_val, last_take_us;
static unsigned long n_backmove[16];
static unsigned long n_backmove_worst;

/*
 * Which page the clobber detector is armed on, read from the environment once.
 *
 * It used to be initialised lazily inside clobber_scan(), which is only reached
 * from an install -- so the take-side check, which runs BEFORE any install in
 * the same call, always saw "not armed yet" and never ran once. An instrument
 * that arms itself only from one of its two call sites reports a zero it cannot
 * have earned.
 *
 * A specific page (the env is an address), every page (env=all: the per-word
 * monotonic filter keeps pointers, text and stack out, so the marks lock onto
 * counter slots on their own), or off.
 */
static uint64_t clobber_page(void)
{
	uint64_t w = clobber_watch;

	if (w == 1) {
		const char *e = getenv("VMR_CLOBBER");

		if (e && !strcmp(e, "all"))
			w = 2;
		else
			w = e ? (strtoull(e, NULL, 16) & ~0xfffUL) : 0;
		clobber_watch = w;
	}
	return w;
}

static void clobber_scan(uint64_t base, const void *buf, int site, int flag)
{
	const unsigned long *nw = (const unsigned long *)buf;
	int slot = -1, i, j;
	uint64_t watch = clobber_page();

	if (!watch || (watch != 2 && (base & ~0xfffUL) != watch))
		return;
	pthread_mutex_lock(&hwm_lock);
	for (i = 0; i < HWM_N; i++)
		if (hwm_tab[i].base == base) { slot = i; break; }
	if (slot < 0)
		for (i = 0; i < HWM_N; i++)
			if (hwm_tab[i].base == 0) {
				hwm_tab[i].base = base;
				slot = i;
				break;
			}
	if (slot < 0) { pthread_mutex_unlock(&hwm_lock); return; }
	/*
	 * Who last put content at this address, and what it was. When a later
	 * answer maps a folio that turns out to be stale, the question is where
	 * that folio came from -- and the only honest way to answer it is to
	 * have written it down at the time. One page is watched, so one slot is
	 * enough. flag distinguishes an install (clobber_check) from a take's
	 * seeding read (clobber_note).
	 */
	if (flag) {
		last_write_site = site;
		last_write_val  = nw[8];	/* +64: the first writer slot */
		last_write_us   = now_us();
	} else {
		last_take_val = nw[8];
		last_take_us  = now_us();
	}
	for (j = 0; j < (int)(VMR_PG_SIZE / sizeof(long)); j++) {
		unsigned long n = nw[j], hw = hwm_tab[slot].hw[j];

		/* Only monotonic-looking counters, to keep pointers/text out. */
		if (hw >= 1 && hw < (1UL << 32) && n < hw && flag) {
			n_backmove[site % 16]++;
			n_backmove_worst = (hw - n) > n_backmove_worst ?
					   (hw - n) : n_backmove_worst;
			if (n_backmove[site % 16] <= 8)
				fprintf(stderr, "[vmremote] BACKMOVE site%d 0x%llx"
					" +%d: hwm %lu -> install %lu (abs=%llu)\n",
					site, (unsigned long long)base, j * 8, hw, n,
					(unsigned long long)now_us());
		}
		if (n > hw && n < (1UL << 32))
			hwm_tab[slot].hw[j] = n;
	}
	pthread_mutex_unlock(&hwm_lock);
}

/* Raise the marks AND flag a backward move about to be installed. */
/*
 * The check the high-water mark cannot make, and the reason it cannot.
 *
 * clobber_scan()'s mark is a running MAXIMUM, and the only thing that raises it
 * is a take (clobber_check site 70 in page_take_mode). So an install that writes back
 * exactly the value the take recorded is never "backwards" -- `n < hw` is false
 * when n == hw -- however far the guest has advanced in between. That is
 * precisely hx2's loss: the guest stores four thousand more times while the page
 * is away, and the copy that comes back is the one that left. The detector was
 * structurally blind to the one case it was built for.
 *
 * So ask the guest instead of a memory of the guest: read what this address
 * space holds RIGHT NOW and compare it with what is about to be written over it.
 * A monotonic slot that would move backwards is a store about to be discarded,
 * named with the site that is doing it, at the instant it happens.
 *
 * Gated to one named page (VMR_CLOBBER=<addr>, not "all"), because it costs a
 * read of the guest's page per install. page_read_as() is the right read: it
 * takes the bytes from whichever context of the address space has them mapped,
 * which is exactly the sibling whose stores are at risk, and falls back to the
 * object rather than instantiating anything.
 */
static unsigned long n_live_checked, n_live_lost, n_live_lost_worst;
static unsigned long n_live_lost_site[16];
static int page_read_as(memory_target ctx, uint64_t addr, void *buf);

/* VMR_CLOBBER_REVERT: flag any word an install would change on a page the
 * address space is holding, not only a counter moving backwards. */
static int revert_mode = -1;

/* ---- content history: which hand-back gave the guest an older copy ----- */
/*
 * Every check before this one compares a VALUE -- a counter that must not move
 * down, a word that must not differ from what the address space holds. Neither
 * can see the failure ws1 is left with, because the words it dies on are glibc
 * locks that go 0 -> 1 -> 2 -> 0 and heap metadata that is not ordered at all.
 * A stale 2 landing on a fresh 0 is an increase; a stale chunk header is just a
 * different number.
 *
 * So compare against TIME instead of against value. For the watched page keep,
 * per word, the newest content anyone has observed and the value it had
 * immediately before that -- and when the guest is handed a page, flag any word
 * whose new value is not the newest but IS the one before it. That is precisely
 * "this hand-back is the copy from before the guest's last write", whichever
 * direction the number moved.
 *
 * Observations come from a TAKE: the bytes leaving the guest are the guest's own
 * and are authoritative. Checks go on every path that gives content back.
 *
 * Armed by VMR_CLOBBER=<page> like the rest; VMR_HIST=0 turns just this off.
 */
#define HIST_W (VMR_PG_SIZE / sizeof(unsigned long))
/*
 * Content history over a SET of watched pages, not one. The single-page form
 * needed the page's address in advance (VMR_CLOBBER=<addr>) -- fine for hx2's
 * fixed layout, useless for the browser's heap, where the corrupted page is a
 * glibc arena header whose address is different every run. So the watch
 * AUTO-ARMS on a signature instead of an address: secondary (mmap'd) glibc
 * arenas are HEAP_MAX_SIZE-aligned (64 MiB on x86-64), and their malloc_state
 * header and first chunks -- the pointers glibc dereferences, top/bins/
 * fastbins -- live in the first pages. A header served one write behind sends
 * glibc through a stale pointer, which is the netsurf/hp2 'mismatching
 * next->prev_size'. VMR_HIST_ARENA=1 turns the auto-arm on (off by default so
 * it never slows a gate); the explicit VMR_CLOBBER=<addr> page is watched too.
 */
#define HIST_SLOTS 256
static struct hist_slot {
	uint64_t      base;		/* 0 = free */
	unsigned long cur[HIST_W];
	unsigned long prev[HIST_W];
	unsigned long seq[HIST_W];
	int           primed;
} hist_tab[HIST_SLOTS];
static pthread_mutex_t hist_lock = PTHREAD_MUTEX_INITIALIZER;
static unsigned long hist_seq;
static unsigned long n_hist_notes, n_hist_checks, n_hist_revert;
static unsigned long n_hist_revert_site[128];
static unsigned long n_hist_watched;	/* distinct pages the auto-arm took on */
static int hist_on = -1, hist_arena = -1;

static int hist_auto(uint64_t base)
{
	const uint64_t ARENA  = 64ULL << 20;		/* HEAP_MAX_SIZE */
	const uint64_t WINDOW = 8 * VMR_PG_SIZE;	/* header + first chunks */

	if (hist_arena < 0)
		hist_arena = getenv("VMR_HIST_ARENA") ? 1 : 0;
	if (!hist_arena)
		return 0;
	/* The x86-64 mmap region, where secondary arenas live; the main arena
	 * is brk-based and lower, and is not 64 MiB-aligned anyway. */
	if (base < 0x700000000000ULL || base >= 0x800000000000ULL)
		return 0;
	return (base & (ARENA - 1)) < WINDOW;
}

static int hist_armed(uint64_t base)
{
	if (hist_on < 0) {
		const char *e = getenv("VMR_HIST");

		hist_on = (e && !strcmp(e, "0")) ? 0 : 1;
	}
	if (!hist_on)
		return 0;
	return (clobber_page() > 2 && base == clobber_page()) || hist_auto(base);
}

/* The slot for a watched page, allocated on first sight; NULL if the table is
 * full. Caller holds hist_lock. */
static struct hist_slot *hist_find(uint64_t base, int alloc)
{
	unsigned h = (unsigned)((base >> 12) * 2654435761u) % HIST_SLOTS;
	unsigned n;

	for (n = 0; n < HIST_SLOTS; n++) {
		struct hist_slot *s = &hist_tab[(h + n) % HIST_SLOTS];

		if (s->base == base)
			return s;
		if (!s->base) {
			if (!alloc)
				return NULL;
			s->base = base;
			n_hist_watched++;
			return s;
		}
	}
	return NULL;
}

/* The guest's own bytes, on their way out: this is the newest truth. */
static void hist_note(uint64_t base, const void *buf)
{
	const unsigned long *w = buf;
	struct hist_slot *s;
	unsigned j;

	if (!hist_armed(base))
		return;
	pthread_mutex_lock(&hist_lock);
	s = hist_find(base, 1);
	if (!s) {
		pthread_mutex_unlock(&hist_lock);
		return;
	}
	for (j = 0; j < HIST_W; j++) {
		if (s->primed && w[j] == s->cur[j])
			continue;
		s->prev[j] = s->primed ? s->cur[j] : w[j];
		s->cur[j]  = w[j];
		s->seq[j]  = ++hist_seq;
	}
	s->primed = 1;
	n_hist_notes++;
	pthread_mutex_unlock(&hist_lock);
}

/* Content about to be given to the guest, checked against that truth. */
static void hist_check(uint64_t base, const void *buf, int site)
{
	const unsigned long *w = buf;
	struct hist_slot *s;
	unsigned j;
	int said = 0;

	if (!hist_armed(base))
		return;
	pthread_mutex_lock(&hist_lock);
	s = hist_find(base, 0);
	if (!s || !s->primed) {
		pthread_mutex_unlock(&hist_lock);
		return;
	}
	n_hist_checks++;
	for (j = 0; j < HIST_W; j++) {
		if (w[j] == s->cur[j] || w[j] != s->prev[j])
			continue;
		if (!s->seq[j])
			continue;
		n_hist_revert++;
		n_hist_revert_site[site & 127]++;
		if (!said++ && n_hist_revert <= 16)
			fprintf(stderr, "[vmremote] STALESERVE site%d 0x%llx "
				"+%u: handing back 0x%lx, which is what this "
				"word held BEFORE the guest's last write "
				"(0x%lx, seq %lu of %lu) abs=%llu\n",
				site, (unsigned long long)base,
				(unsigned)(j * sizeof(unsigned long)),
				w[j], s->cur[j], s->seq[j], hist_seq,
				(unsigned long long)now_us());
	}
	pthread_mutex_unlock(&hist_lock);
}

static void clobber_live_check(memory_target pid, uint64_t base, const void *buf,
			       int site)
{
	static __thread char cur[VMR_PG_SIZE];
	const unsigned long *nw = (const unsigned long *)buf;
	const unsigned long *cw = (const unsigned long *)cur;
	int j, said = 0;

	if (clobber_page() <= 2 || base != clobber_page())
		return;
	if (revert_mode < 0)
		revert_mode = getenv("VMR_CLOBBER_REVERT") ? 1 : 0;
	if (!page_read_as(pid, base, cur))
		return;
	n_live_checked++;
	for (j = 0; j < (int)(VMR_PG_SIZE / sizeof(long)); j++) {
		unsigned long n = nw[j], c = cw[j];

		/*
		 * Two questions, and the second one is the one ws1 needs.
		 *
		 * The monotonic test below asks "would this install move a
		 * COUNTER backwards", which is right for hx2's slots and
		 * structurally blind to a lock. _dl_stack_cache_lock and
		 * __exit_lock -- the two words ws1 hangs on -- go 0 -> 1 -> 2 ->
		 * 0, so a stale 2 landing on top of a fresh 0 is an INCREASE and
		 * the filter skips it. Heap metadata is not monotonic in any
		 * direction at all, and that is the other way ws1 dies (a
		 * sysmalloc assertion on the top chunk).
		 *
		 * VMR_CLOBBER_REVERT=1 asks the question without the filter:
		 * does this install differ AT ALL from what the address space
		 * is holding right now? page_read_as() only answers when some
		 * context has the page, so every hit is an install landing on
		 * top of live memory -- which is the whole failure, whichever
		 * direction the value moved.
		 */
		if (revert_mode) {
			if (n == c)
				continue;
		} else if (c < 1 || c >= (1UL << 32) || n >= c) {
			continue;	/* monotonic counters only */
		}
		n_live_lost++;
		n_live_lost_site[site % 16]++;
		if (c - n > n_live_lost_worst)
			n_live_lost_worst = c - n;
		if (!said++ && n_live_lost <= 16)
			fprintf(stderr, "[vmremote] %s site%d 0x%llx +%d: "
				"the guest holds 0x%lx and this install writes "
				"0x%lx abs=%llu\n",
				revert_mode ? "REVERT" : "LOSTWRITE",
				site, (unsigned long long)base, j * 8, c, n,
				(unsigned long long)now_us());
	}
}

/*
 * A path that answers "served" WITHOUT installing anything, checked against the
 * mark of what has already changed hands.
 *
 * Every check so far looks at bytes being written. The paths that lose this one
 * write nothing: they map the address space's own folio, or find the page
 * already present, and tell the kernel the fault is answered. If the folio they
 * hand the guest is older than a value that has already been handed over, the
 * guest resumes on a stale page and no install was involved anywhere -- which is
 * exactly what every install-side check reporting zero has been saying.
 *
 * The mark is the take-seeded one (clobber_check site 70 at page_take_mode), so it is
 * per-run and per-page and lives on this side. The kernel's equivalent
 * (vmctx_watch_served) cannot be used for this: its high-water array is global,
 * keyed by address rather than address space, and never reset between runs, so
 * on this box it reports marks of 274 million and gaps of 2.8 billion for a test
 * whose counters reach 30 million.
 */
static unsigned long n_served_checked, n_served_stale, n_served_stale_worst;
static unsigned long n_served_stale_site[16];

static void clobber_served_check(memory_target pid, uint64_t base, int site)
{
	static __thread char cur[VMR_PG_SIZE];
	const unsigned long *cw = (const unsigned long *)cur;
	int slot = -1, i, j, said = 0;

	if (clobber_page() <= 2 || base != clobber_page())
		return;
	if (!page_read_as(pid, base, cur))
		return;
	n_served_checked++;
	pthread_mutex_lock(&hwm_lock);
	for (i = 0; i < HWM_N; i++)
		if (hwm_tab[i].base == base) { slot = i; break; }
	if (slot < 0) { pthread_mutex_unlock(&hwm_lock); return; }
	for (j = 0; j < (int)(VMR_PG_SIZE / sizeof(long)); j++) {
		unsigned long c = cw[j], hw = hwm_tab[slot].hw[j];

		if (hw < 1 || hw >= (1UL << 32) || c >= hw)
			continue;
		n_served_stale++;
		n_served_stale_site[site % 16]++;
		if (hw - c > n_served_stale_worst)
			n_served_stale_worst = hw - c;
		if (!said++ && n_served_stale <= 16) {
			/*
			 * The question this exists to answer: do the contexts
			 * of this address space agree with each other and with
			 * the object about what is at this address? They map
			 * the same object at the same offset, so they must --
			 * and if they do not, two physical pages exist for one
			 * address and the "stale folio" above is one of them.
			 * Asked of every context individually, plus the object
			 * read directly, because page_read_as() answers with
			 * the FIRST context that has it and so cannot show a
			 * disagreement at all.
			 */
			memory_target mem[MAX_CTX];
			int mn = as_members(pid, mem, MAX_CTX), mi;
			static __thread char one_ctx[VMR_PG_SIZE];
			int bf = backing_find(pid);

			fprintf(stderr, "[vmremote] DIVERGE 0x%llx +%d:",
				(unsigned long long)base, j * 8);
			for (mi = 0; mi < mn; mi++) {
				unsigned long v = 0;

				if (peek(mem[mi], base, one_ctx, VMR_PG_SIZE) ==
				    (long)VMR_PG_SIZE)
					v = ((unsigned long *)one_ctx)[j];
				else
					v = ~0UL;
				fprintf(stderr, " ctx%d=%lu", ctx_id(mem[mi]), v);
			}
			if (bf >= 0 &&
			    pread(bf, one_ctx, VMR_PG_SIZE, (off_t)base) ==
			    (ssize_t)VMR_PG_SIZE)
				fprintf(stderr, " object=%lu",
					((unsigned long *)one_ctx)[j]);
			fprintf(stderr, " mark=%lu\n", hw);
			fprintf(stderr, "[vmremote] SERVEDSTALE site%d 0x%llx "
				"+%d: answered served with no install and the "
				"guest's page reads %lu, below the %lu already "
				"handed over (%lu behind); the last install here "
				"was site%d with %lu, %lluus ago; the last take "
				"read %lu, %lluus ago\n",
				site, (unsigned long long)base, j * 8, c, hw,
				hw - c, last_write_site, last_write_val,
				(unsigned long long)(now_us() - last_write_us),
				last_take_val,
				(unsigned long long)(now_us() - last_take_us));
		}
	}
	pthread_mutex_unlock(&hwm_lock);
}

static void clobber_check(memory_target pid, uint64_t addr, const void *buf,
			  uint64_t len, int site)
{
	if (len >= VMR_PG_SIZE && !(addr & 0xfffUL)) {
		clobber_live_check(pid, addr & ~0xfffUL, buf, site);
		clobber_scan(addr & ~0xfffUL, buf, site, 1);
	}
}
/*
 * Times the source answered "I do not hold that page" -- the answer that
 * replaced a page of zeros, and the one number that says how often this side's
 * own copy was the right one after all.
 */
static unsigned long n_src_absent, n_src_absent_own, n_src_absent_kernel;
/* The broad lost-store detector: a page zero-filled that this side had
 * installed and nothing recently unmapped -- the died-242/hp2/netsurf-heap
 * family, named without an armed page. See fill_site 6. */
static unsigned long n_lost_store_zeroed;
/* The chunk fetch meeting NOTHOLDER: the source says we already have it. */
static unsigned long n_src_notholder, n_notholder_lost;
static unsigned long n_empty_averted;
/* Pages the context could not be written through, put into its object instead. */
static unsigned long n_served_via_object;
/* Faults the shadow says the program had no right to make. */
static unsigned long n_guest_fault;
/* ... and faults where the shadow could not be asked at all. */
static unsigned long n_mayaccess_lost;
/* ... of which the owner answered with somewhere else to carry on. */
static unsigned long n_exc_delivered;
/*
 * A program's own exceptions are handed to the side that owns it,
 * unconditionally: this is how a fault the program raised deliberately reaches
 * the handler it registered, and a program that handles its own faults is not
 * an exotic case. Delivery was once off, on the grounds that switching it on
 * made every test die on a fault at address 0x0 during start-up -- a fault
 * that does not exist: the report behind it printed an address and nothing
 * else, and printed whole, the only fault that reaches this path in tests/pf1
 * is the one the program raises on purpose.
 */
/* VMREMOTE_TRACE_AFTER_EXC: log the calls that follow a handover. */
static int trace_after_exc;
/* VMREMOTE_FAULT_STATE: log what the kernel says about each faulting page. */
static int trace_fault_state;
/* Legacy counter retained in reports; shared aliases map the common object. */
static unsigned long n_shared_moved;
/*
 * The shared object asked whether it really had the page, and what came of it.
 * Reported unconditionally at exit: "no shared range needed filling" is a
 * statement 22 fabricated zero pages a suite run used to make impossible, and a
 * counter printed only when non-zero cannot make it (AUDIT XIII §1).
 */
static unsigned long n_shared_hole;	  /* the object had no page there   */
static unsigned long n_shared_filled;	  /* ...and the owner supplied it   */
static unsigned long n_shared_hole_claim; /* waits for an unresolved transfer */

static void src_clone_note(pid_t child, pid_t parent)
{
	int i = __sync_fetch_and_add(&nsrc_clone, 1);

	if (i < MAX_SRC_CLONE) {
		src_clone[i].parent = parent;
		__atomic_store_n(&src_clone[i].child, child, __ATOMIC_RELEASE);
	}
}

static pid_t src_clone_parent(pid_t child)
{
	int n = __atomic_load_n(&nsrc_clone, __ATOMIC_ACQUIRE), i;

	for (i = 0; i < n && i < MAX_SRC_CLONE; i++)
		if (__atomic_load_n(&src_clone[i].child, __ATOMIC_ACQUIRE) == child)
			return src_clone[i].parent;
	return 0;
}

/*
 * Is this fault the program's own, and if so, what does its owner want done?
 *
 * The question is the same wherever the fault came from: this side asks the
 * owner whether the program may make that access at that address, and if the
 * answer is no, reports the vector and does whatever comes back. Returns 1 when
 * the owner answered with somewhere to carry on and the registers are installed,
 * 0 when the fault is not the program's or nothing could be done with it.
 *
 * One function because there is one question. Written out at the absent-page
 * branch only, a write to a page the program had mprotected read-only never
 * reached the owner at all: that fault arrives with the page *present*, takes
 * the coherence branch instead, and is declined to the local kernel -- which
 * cannot grant a write the program itself forbade, so the context dies on a
 * fault it had a handler for. The presence of the page is a fact about this
 * machine's page tables; whether the access is allowed is a fact about the
 * program, and only one side knows it.
 *
 * The coherence lock must NOT be held: building a signal frame writes to the
 * program's stack, and the owner fetches that page back through the page
 * service, which takes the same lock.
 */
/*
 * Ranges the guest holds that are mapped straight from the backing object at an
 * offset of their own -- shared memory. See reply_note_map().
 *
 * A fault in one of these is not a page to fetch from the owner. The mapping
 * already comes from the object, so the local kernel can service it, and the
 * page it attaches is the *same* page any other mapping of that slot attached:
 * which is the whole point. Fetching instead overwrites it -- tests/sm1 wrote a
 * string through the first mapping, faulted on the second, and had the owner's
 * copy of the file poked back over the string.
 */
#include "shared-layout.h"
#include "shared-pages.h"
#include "access-layout.h"

/*
 * A mapping the answer to a fault should carry.
 *
 * A context created by a fork starts with no mappings at all -- every page is
 * conjured from its own object by the fault path -- so a range that must come
 * from the shared object has to be told so, and its first touch is the only
 * moment anyone asks. Beside the call rather than returned through it, because
 * almost no fault has one. (Defined beside declined_at now, which also fills
 * it -- the construction repair.)
 */
static unsigned long n_prot_applied;	/* pages given the program's own protection */
struct region;
static void fault_note_prot(uint64_t base, const struct region *r);
static void shared_range_note(memory_target pid, uint64_t st, uint64_t len,
                              uint64_t off, unsigned int prot, uint64_t object_id)
{
	shared_layout_note(as_id(pid), st, st + len, off, prot, object_id);
}

static int shared_range_has(memory_target pid, uint64_t addr)
{
	return shared_layout_lookup(as_id(pid), addr, NULL);
}

static off_t shared_obj_off(memory_target pid, uint64_t addr)
{
	struct shared_range range;
	if (!shared_layout_lookup(as_id(pid), addr, &range)) return (off_t)-1;
	return shared_page_slot(shared_obj, range.object_id,
		range.off + ((addr & ~0xfffULL) - range.st), 0);
}

/*
 * Does the shared object actually hold this page, or is the slot a hole?
 *
 * backing_has()'s question, asked of the shared object instead of a context's
 * own one, and it has to be asked for the same reason: a memfd reads as zeros
 * wherever nothing was written, so "the mapping comes from the object" says
 * nothing about whether the object has the bytes. SEEK_DATA is the only thing
 * that tells a written page from a hole in one.
 */
static int shared_obj_has(off_t off)
{
	off_t d;

	if (shared_obj < 0 || off < 0)
		return 0;
	d = lseek(shared_obj, off, SEEK_DATA);
	return d >= 0 && d < off + VMR_PG_SIZE;
}

static void shared_range_inherit(memory_target child, memory_target parent)
{
	shared_layout_inherit(as_id(child), as_id(parent));
	access_inherit(as_id(child), as_id(parent));
}

static _Noreturn void source_memory_failed(memory_target target,uint64_t address,
        const char *operation,long result);

static int owner_takes_fault(memory_target pid, uint64_t vec, uint64_t base,
			     uint64_t addr, uint64_t err)
{
	struct vmr_cpu_state cpu;
	struct vmr_uregs u;
	uint64_t e[6] = { 0 };
	long g;

	(void)base;

	/*
	 * Only a memory fault has an address to ask about, and only a memory
	 * fault might not be the program's doing -- it may simply be a page
	 * this side has not fetched yet. Every other exception is the program's
	 * by construction: nothing this machinery does makes an instruction
	 * illegal or a segment wrong, so there is nothing to ask and the report
	 * goes straight over. Written as a test on the vector, not a list of
	 * which exceptions are "real".
	 */
	/*
	 * There is no question to ask before this any more.
	 *
	 * This used to ask the owner "may the program make this access"
	 * before reporting anything, because the side that owns the program
	 * could only answer from a copy of its address space and the answer
	 * had to be asked for separately. It is the same question as "is
	 * there a page here", and it is now answered by the same request:
	 * VMR_OP_CTXPAGE comes back with the bytes for an address the source
	 * has, and with EFAULT for one it has not. So the fetch is tried
	 * first and this is reached only when it found nothing -- which is
	 * exactly the case where the fault is the program's own.
	 */
	n_guest_fault++;
	/*
	 * What THIS context's own mm holds around the address, said before the
	 * owner is asked to rule. The firefox wrapper children die on a
	 * byte-identical read (0x7ffff7fb6188, rip in ld.so) through every
	 * region repair -- which means no region table explains it, and the
	 * one map nobody has looked at is the context's own inherited-from-
	 * vmremote address space. Printed for the first few page faults that
	 * reach this path, maps lines overlapping addr +/- 64K.
	 */
	if (vec == 14) {
		static int shown;

		if (shown++ < 6) {
			char ln[256];
			FILE *f;

			f = ctx_maps_open(pid);
			fprintf(stderr, "[vmremote] ctx %d mm around 0x%llx "
				"at the owner-ruled fault:\n", ctx_id(pid),
				(unsigned long long)addr);
			while (f && fgets(ln, sizeof(ln), f)) {
				unsigned long lo, hi;

				if (sscanf(ln, "%lx-%lx", &lo, &hi) == 2 &&
				    hi + 0x10000 > addr && lo < addr + 0x10000)
					fprintf(stderr, "[vmremote]   %s", ln);
			}
			if (f)
				fclose(f);
		}
	}
	if (n_guest_fault <= 8)
		fprintf(stderr, "[vmremote] fault 0x%llx: the program has no "
			"mapping there, or no right to that access; handing the "
			"exception to the side that owns it\n",
			(unsigned long long)base);
	/*
	 * Not this side's to interpret. It reports the vector, the error code,
	 * the address and the machine state, and does whatever the answer says.
	 */
	e[0] = vec;
	e[1] = err;
	e[2] = addr;
	/*
	 * Split, not chained: written as one condition a failing register read
	 * skips the send with no trace, and the request simply never arrives.
	 */
	if (cpu_state_ctl(pid, VMCTX_CTL_GETCPU, &cpu) < 0) {
		fprintf(stderr, "[vmremote] cannot read context %d's registers "
			"to report its exception: %s\n", ctx_id(pid), strerror(errno));
		return 0;
	}
	u = cpu.regs;
	/* Optional read-only architectural probe. The harness chooses the
	 * segment-relative offset for the source program being investigated;
	 * the executor assigns no ABI or library meaning to these words. */
	const char *probe_offset = getenv("VMR_FAULT_FS_OFFSET");
	if (probe_offset) {
		char *end;
		uint64_t offset = strtoull(probe_offset, &end, 0);
		fprintf(stderr, "[vmremote] FAULT-REGS ctx=%d rip=%llx "
			"rax=%llx rdi=%llx rsi=%llx rdx=%llx r8=%llx r9=%llx fs=%llx\n",
			ctx_id(pid), (unsigned long long)u.rip, (unsigned long long)u.rax,
			(unsigned long long)u.rdi, (unsigned long long)u.rsi,
			(unsigned long long)u.rdx, (unsigned long long)u.r8,
			(unsigned long long)u.r9, (unsigned long long)u.fs_base);
		if (end != probe_offset && !*end && offset <= 4096 &&
		    u.fs_base < (1ULL << 47) - 8192) {
			uint64_t addresses[] = {u.rdi, u.r9, u.fs_base + offset};
			for (unsigned j = 0; j < sizeof(addresses) / sizeof(addresses[0]); j++) {
				uint32_t words[16];
				uint64_t at = addresses[j];
				if (!at || at >= (1ULL << 47) - sizeof(words)) continue;
				long bytes = peek(pid, at, words, sizeof(words));
				fprintf(stderr, "[vmremote] FAULT-WORDS ctx=%d at=%llx bytes=%ld:",
					ctx_id(pid), (unsigned long long)at, bytes);
				for (long k = 0; k < bytes / (long)sizeof(words[0]); k++)
					fprintf(stderr, " %08x", words[k]);
				fputc('\n', stderr);
			}
			page_provenance(pid, (u.fs_base + offset) & ~0xfffULL,
					"fault segment-relative probe");
			trail_dump(pid, u.fs_base + offset);
		}
	}
	/*
	 * What the faulting instruction was actually looking at, read from the
	 * guest's own memory before anyone is asked to interpret the fault.
	 *
	 * This side is the only one that can. The owner reports the exception
	 * with rip and rsp, but the stack under rsp is frequently not on that
	 * machine at all -- measured: "the stack under it (0x7ffff7ff61d8, page
	 * is NOT here)" on every failing round -- and the program cannot report
	 * for itself, because saying anything costs a forwarded syscall through
	 * the machinery that is failing.
	 *
	 * Eight words, because the ones that matter are the return address and
	 * its neighbours: a general-protection fault on a `ret` means the word
	 * at rsp is not a code address, and printing it says whether it is
	 * zero, a plausible pointer, or somebody else's data.
	 */
	/*
	 * Not rate-limited like the line above it. A fault that reaches this
	 * point is being handed to the owner because this side could not answer
	 * it, and the ones that matter are the LAST ones a run takes, not the
	 * first eight -- a thread tearing down has faulted hundreds of times by
	 * then, so the cap that keeps the ordinary fault chatter readable also
	 * silenced this exactly when it was worth reading. Measured: 40 rounds
	 * hunting one signature produced no dump at all.
	 */
	{
		uint64_t w[8];
		int i, np = as_any_present(pid, u.rsp & ~0xfffULL);

		fprintf(stderr, "[vmremote] the guest's stack under rip 0x%llx "
			"rsp 0x%llx (page present in a context here: %d)\n",
			(unsigned long long)u.rip, (unsigned long long)u.rsp, np);
		if (peek(pid, u.rsp & ~7ULL, w, sizeof(w)) == (long)sizeof(w))
			for (i = 0; i < 8; i++)
				fprintf(stderr, "[vmremote]   [rsp+0x%02x] = "
					"0x%016llx%s\n", i * 8,
					(unsigned long long)w[i],
					w[i] == 0 ? "  <- ZERO" :
					/* canonical user address? */
					(w[i] >> 47) != 0 ? "  <- NOT CANONICAL"
							  : "");
		else
			fprintf(stderr, "[vmremote]   (the guest's own stack is "
				"unreadable here too)\n");
		{
			char ln[256];
			FILE *f;

			f = ctx_maps_open(pid);
			while (f && fgets(ln, sizeof(ln), f)) {
				unsigned long lo, hi;

				if (sscanf(ln, "%lx-%lx", &lo, &hi) == 2 &&
				    lo <= u.rsp && u.rsp < hi)
					fprintf(stderr, "[vmremote]   rsp's mapping: %s", ln);
			}
			if (f)
				fclose(f);
		}
		page_provenance(pid, u.rsp & ~0xfffULL, "at the fatal exception (rsp page)");
	}
	size_t cpu_len = vmr_cpu_wire_size(&cpu);
	if (!cpu_len) {
		fprintf(stderr, "[vmremote] invalid captured exception CPU state\n");
		return 0;
	}
	struct vmctx_runtime runtime;
	if (execution_runtime(pid, VMCTX_RUNTIME_READ, &runtime) < 0) {
		fprintf(stderr, "[vmremote] cannot capture exception execution time: %s\n", strerror(errno));
		return 0;
	}
	struct vmr_rsp reply;
	g = home_call_runtime(VMR_NR_EXCEPTION, e, &cpu, cpu_len, &cpu, sizeof(cpu),
		runtime.epoch, runtime.total_ns,&reply);
	/* A terminal reply has no CPU frame. Validate the saved task/MM before
	 * using any outcome, including LEGAL, and never infer death from errno. */
	if(home_call_failed || !vmr_binding_equal(&reply.binding,&pid->source))
		source_memory_failed(pid,addr,"exception reply",home_call_failed ? -errno:-ESTALE);
	if((!reply.ended && reply.ended_status) || (g && reply.datalen))
		source_memory_failed(pid,addr,"malformed exception outcome",-EPROTO);
	if(reply.ended) {
		if(reply.ended!=1 || g || reply.datalen)
			source_memory_failed(pid,addr,"malformed exception termination",-EPROTO);
		src_ended_note(ctx_id(pid),reply.ended_status);
		/* Only this task ended. Its MM peers retain their own lifetimes.
		 * A successful signal is not proof of exit: observe its pidfd before
		 * reporting the fault handled, so no missing-page resume is possible. */
		(void)ctx_signal(ctx_id(pid),SIGKILL);
		if(!ctx_wait_execution_ended(ctx_id(pid)))
			source_memory_failed(pid,addr,"stopping ended exception task",-EIO);
		n_src_ended++;n_src_ended_ctx++;
		map_trace("executor","exception ended context=%d source=%llu mm=%llu status=%u",
			ctx_id(pid),(unsigned long long)reply.binding.context,
			(unsigned long long)reply.binding.mm,reply.ended_status);
		return 1;
	}
	if (g == VMR_EXC_LEGAL) {
		/*
		 * Not an exception: the program's own kernel would handle the
		 * access (VMCTX_CTL_TRYFAULT answered 0 in its real address
		 * space). The caller decides what that means for the page it
		 * is standing on -- for a present-page write fault it is a
		 * protection the protocol left behind, and the write is
		 * granted. Returned as 2 so no caller mistakes it for "the
		 * owner's registers are installed, resume".
		 */
		fprintf(stderr, "[vmremote] the owner says the access at 0x%llx "
			"is LEGAL -- not the program's exception\n",
			(unsigned long long)addr);
		return 2;
	} else if (g != 0)
		fprintf(stderr, "[vmremote] the owner had no answer for the "
			"exception at 0x%llx (%ld)\n",
			(unsigned long long)base, g);
	if (g == 0) {
		if (vmr_cpu_wire_decode(&cpu, reply.datalen) ||
		    cpu_state_ctl(pid, VMCTX_CTL_SETCPU, &cpu) < 0) {
			fprintf(stderr, "[vmremote] the owner's answer for the "
				"exception at 0x%llx could not be installed: "
				"%s\n", (unsigned long long)base,
				strerror(errno));
			return 0;
		}
		n_exc_delivered++;
		return 1;
	}
	return 0;
}

/*
 * supply_only: fetch the page and nothing else.
 *
 * The ordinary caller is a context that has genuinely faulted and is stopped
 * waiting for an answer, so handing the fault to the owner and installing the
 * registers it sends back is exactly right -- that is how a program's own fault
 * reaches its own handler.
 *
 * The page service is not that caller. It calls in from a service thread, to
 * prefill a page before a partial write-back lands on it, about a context that
 * is running perfectly normally and has not faulted at all. Reaching the owner
 * from there means GETREGS, VMR_NR_EXCEPTION and SETREGS on a live context:
 * the page channel redirecting a running program's control flow because a byte
 * range happened to have no region on this side. Nothing about a write-back is
 * a fault of the program's, so in this mode a page that cannot be supplied is
 * simply not supplied, and the caller is told so.
 */
/*
 * Pull a page back out of the context at the source: the other half of the
 * two-pull protocol, asked for by address and naming nothing else.
 *
 * The four answers of vmrproto.h, kept apart, because acting on the wrong one
 * is how a guest comes to run somebody's zeros. They used to be two -- "4096
 * bytes" and "anything else" -- and the source, having no way to say "I do not
 * hold that page", said it with a page of zeros instead.
 */
#define PULL_BYTES  1	/* the bytes are in buf and the page is ours now      */
#define PULL_ABSENT 2	/* the source does not hold it; our own copy is it    */
#define PULL_CLAIM  3	/* the source is claiming it too; yield and fault on  */
#define PULL_NOMAP  4	/* the source has no such address: a real fault       */
#define PULL_FAILED 5	/* the question never got an answer                   */
#define PULL_COWBREAK 7	/* it is still the parent's: make the child its own  */
#define PULL_GONE   8	/* the owner's address space has ended: stop         */
#define PULL_LOCAL  9 /* local protected snapshot/object; no source receipt */
#define PULL_NOTHOLDER 6/* the source handed it to THIS side and has not been
			 * asked for it back: our own copy is not "the
			 * authority", it is the only copy                   */

/*
 * The same question with the copy-on-write refinement switched off: "this side
 * has tried the break and there was nothing to copy from, so answer as if no
 * fork had happened."
 *
 * It is what stops a page neither machine has ever written -- a library page the
 * original never touched, whose file is the right answer -- from being asked
 * for, told to break, failing to break, and asked for again for ever.
 */
static _Noreturn void source_memory_failed(memory_target target,uint64_t address,
        const char *operation,long result)
{
    fprintf(stderr,"[vmremote] unresolved source %s for context %llu MM %llu epoch %llu at %llx (result=%ld); stopping before any ownership or permission grant\n",
        operation,(unsigned long long)target->source.context,
        (unsigned long long)target->source.mm,(unsigned long long)target->source.epoch,
        (unsigned long long)address,result);
    ctx_stop_all();fflush(stderr);_exit(97);
}

static long source_page_fetch(memory_target target,const uint64_t args[6],
        void *bytes,size_t capacity,struct source_page_receipt *receipt)
{
    struct vmr_rsp reply;
    /* Receipt storage is initialized by its owning scope. A second fetch
     * cannot silently discard custody obtained by the first one. */
    if(receipt->episode && !receipt->ack_queued)
        source_memory_failed(target,receipt->base,"overwriting unsettled receipt",-EBUSY);
    *receipt=(struct source_page_receipt){0};
    long result=home_memory_call(target,VMR_OP_CTXPAGE,args,NULL,0,bytes,capacity,&reply);
    if(home_call_failed || (result<=0 && result!=VMR_CTXPAGE_ABSENT &&
       result!=VMR_CTXPAGE_CLAIMING && result!=VMR_CTXPAGE_NOTHOLDER &&
       result!=VMR_CTXPAGE_COWBREAK && result!=VMR_CTXPAGE_GONE &&
       result!=-EFAULT && result!=-EACCES))
        source_memory_failed(target,args[0],"page fetch",result);
    if(result<=0)return result;
    struct source_page_receipt saved={.target=reply.memory_target,
        .base=reply.memory_args[0],.episode=reply.page_episode,.count=reply.ngen,.taken=(uint32_t)reply.pages_taken};
    for(unsigned i=0;i<saved.count;i++) {
        saved.generation[i]=reply.pggen[i];
        saved.sum[i]=page_sum((const char *)bytes+i*VMR_PG_SIZE)&0xffff;
    }
    *receipt=saved;
    return result;
}

static long ctx_page_ask_nocow(memory_target pid,uint64_t base, void *buf,struct source_page_receipt *receipt)
{
	uint64_t q[6] = { base, VMR_PG_SIZE, 1, 0, 0, 0 };

	return source_page_fetch(pid,q,buf,VMR_PG_SIZE,receipt);
}

/* Capture counts are diagnostics, not page identity or custody. An address
 * can be reused, and a retained capture does not authorize replacing bytes
 * in a newly acquired source receipt. Report disagreement without changing
 * the reply; integration grading keeps every such report visible. */
static void pull_gen_check(memory_target pid, uint64_t base, const void *buf, uint32_t src_gen,
			   int site)
{
	uint32_t cnt = pgen_get(pid, base);
	uint32_t rg = 0;
	int have;

	n_gen_checked++;
	/* aux: the source's stamp in the low 24 bits, the receiving site above */
	trail_note(pid, base, TR_PULL_BYTES, cnt,
		   (src_gen & 0xffffff) | ((uint32_t)site << 24), buf);
	if (src_gen == cnt)
		return;
	if (src_gen > cnt) {
		/* The source claims a capture this side never made: a reset
		 * that did not reach both records. Counted, never acted on. */
		n_gen_ahead++;
		if (n_gen_ahead <= 8)
			fprintf(stderr, "[vmremote] GEN-AHEAD 0x%llx site%d: the "
				"source stamps gen %u, this side's count is %u "
				"-- a reset reached one record and not the "
				"other\n", (unsigned long long)base, site,
				src_gen, cnt);
		return;
	}
	n_gen_stale++;
	n_gen_stale_site[site & 127]++;
	have = retain_get_gen(pid, base, NULL, &rg);
	trail_note(pid, base, TR_PULL_STALE, cnt, src_gen, buf);
	if (n_gen_stale <= 16) {
		uint64_t q[6] = { base, 0, 0, 0, 0, 0 };

		fprintf(stderr, "[vmremote] STALE-SERVE 0x%llx ctx %d site%d: the "
			"source served a copy of gen %u; this side has captured "
			"the page %u time(s) (retained copy: %s, gen %u; object "
			"holds it: %s) -- diagnostic only; received bytes unchanged\n",
			(unsigned long long)base, ctx_id(pid), site, src_gen, cnt,
			have ? "yes" : "no", rg,
			backing_has(pid, base) ? "yes" : "no");
		trail_dump(pid, base);
		/* ...and the source's side of the same page's story. */
		(void)home_memory_call(pid, VMR_OP_STALETRAIL, q, NULL, 0, NULL, 0,NULL);
	}
	n_gen_unrepaired++;
}

/*
 * INSTRUMENT (session 40, the ~1% startup stall): a CTXPAGE round trip that
 * takes longer than a second is a stuck step on the source, and the pull it
 * stalls is what every sibling's fault then waits on (pull_wait) and what
 * the source's own GETs are refused INFLIGHT against -- until the 6 s
 * deadline ends the run. Its own cap, so the start-up chatter cannot spend
 * it: what is wanted is the answer the slow pull finally got, and how long.
 */
static unsigned long n_pull_slow;

static int ctx_page_pull_mode(memory_target pid, uint64_t base, void *buf, int nocow,
        struct source_page_receipt *receipt)
{
	uint64_t q[6] = { base, VMR_PG_SIZE, (uint64_t)nocow, 0, 0, 0 };
	uint64_t t0 = now_us();
	long g = source_page_fetch(pid,q,buf,VMR_PG_SIZE,receipt);
	int r;

	if (now_us() - t0 > 1000000 && ++n_pull_slow <= 8)
		fprintf(stderr, "[vmremote] SLOW PULL of 0x%llx by ctx %d: the "
			"source's CTXPAGE took %llu ms to answer %ld abs=%llu\n",
			(unsigned long long)base, ctx_id(pid),
			(unsigned long long)((now_us() - t0) / 1000), g,
			(unsigned long long)now_us());

	if (g == (long)VMR_PG_SIZE) {
		r = PULL_BYTES;
		n_pg_pulled++;	/* the return direction, which is this and not
				 * the recall channel; see the summary */
		pull_gen_check(pid, base, buf, receipt->count ? receipt->generation[0] : 0,
			       nocow ? 32 : 31);
	} else if (home_call_failed)
		r = PULL_FAILED;	/* not a statement about the program */
	else if (g == VMR_CTXPAGE_CLAIMING)
		r = PULL_CLAIM;
	else if (g == VMR_CTXPAGE_ABSENT)
		r = PULL_ABSENT;
	else if (g == VMR_CTXPAGE_NOTHOLDER)
		r = PULL_NOTHOLDER;
	else if (g == VMR_CTXPAGE_COWBREAK)
		r = PULL_COWBREAK;
	else if (g == VMR_CTXPAGE_GONE)
		r = PULL_GONE;
	else if(g==-EFAULT || g==-EACCES)
		r = PULL_NOMAP;
	else
		r = PULL_FAILED;
	if (pglog_on)	/* the arguments are a clock read and a checksum */
		plog(base, "CTXPAGE pull -> %ld (%s) us=%llu sum=%08x%s", g,
		     r == PULL_BYTES ? "bytes" : r == PULL_ABSENT ? "absent" :
		     r == PULL_NOTHOLDER ? "not-holder (it is ours and it is gone)" :
		     r == PULL_CLAIM ? "claiming" : r == PULL_NOMAP ? "no mapping" :
		     "unanswered",
		     (unsigned long long)(now_us() - t0),
		     g == (long)VMR_PG_SIZE ? page_sum(buf) : 0,
		     g == (long)VMR_PG_SIZE ? pgword(buf) : "");
	return r;
}

static unsigned long n_acks_sent;
/* Retain the landed-page receipt until the source confirms settlement. Flush
 * while the local transfer reservation and native fault event remain held;
 * a send, EOF, negative reply or malformed receipt grants no continuation. */
static __thread struct { uint64_t page,episode; int set; struct vmr_mm_binding target; } ack_pend;

static void ack_flush(void)
{
    if(!ack_pend.set)return;
    if(!vmr_binding_valid(&ack_pend.target) || !ack_pend.episode || (ack_pend.page&4095))_exit(97);
    uint64_t args[6]={ack_pend.page,ack_pend.episode};
    long result=home_binding_call(&ack_pend.target,VMR_OP_INSTALLED,args,NULL,0,NULL,0,NULL);
    if(home_call_failed || result) {
        fprintf(stderr,"[vmremote] page acknowledgement unconfirmed for MM %llu at 0x%llx: result=%ld transport_error=%d; stopping before release\n",
            (unsigned long long)ack_pend.target.mm,(unsigned long long)ack_pend.page,
            result,home_call_failed ? errno:0);
        _exit(97);
    }
    ack_pend.set=0;n_acks_sent++;
}

static void ack_installed(struct source_page_receipt *receipt,uint64_t base)
{
    if(!receipt || !vmr_binding_valid(&receipt->target) || !receipt->episode ||
       receipt->ack_queued || receipt->count!=1 || (base&4095) || base<receipt->base ||
       (base-receipt->base)/4096>=receipt->count) {
        fprintf(stderr,"[vmremote] missing page receipt for acknowledgement at 0x%llx\n",
            (unsigned long long)base);
        _exit(97);
    }
    unsigned index=(unsigned)((base-receipt->base)/4096);
    if(!(receipt->taken&(1U<<index)))_exit(97);
    if(ack_pend.set)ack_flush();
    ack_pend.target=receipt->target;
    ack_pend.page=base;
    ack_pend.episode=receipt->episode;
    ack_pend.set=1;
    receipt->ack_queued=1;
}

/* Every C return/goto out of a page-acquisition scope goes through here.
 * ACK is only queued by a branch with a complete landing; the destructor
 * never infers a landing or acknowledges bytes merely because they arrived.
 * Preserve the local reservation through confirmation, or stop the session
 * with source custody retained for its normal abandonment path. */
static void source_receipt_finish(struct source_page_receipt *receipt)
{
    if(!receipt->episode)return;
    int depth=coh_drop_all();
    if(!receipt->ack_queued) {
        fprintf(stderr,"[vmremote] unsettled page receipt at scope exit: context=%llu MM=%llu epoch=%llu page=%llx episode=%llu; no continuation granted\n",
            (unsigned long long)receipt->target.context,(unsigned long long)receipt->target.mm,
            (unsigned long long)receipt->target.epoch,(unsigned long long)receipt->base,
            (unsigned long long)receipt->episode);
        ctx_stop_all();_exit(97);
    }
    ack_flush();
    coh_restore(depth);
}

/*
 * The owner's address space has ended: this context must stop.
 *
 * Ending it HERE rather than being killed from outside is what keeps the rest
 * of the teardown intact -- child_thread()'s waitpid sees it, home is told with
 * VMR_OP_FINISH, and the owner runs the clear-tid clear and the wake a joiner
 * is waiting on (§16.4). Stop and cleanup are one path; there is nothing to
 * reclaim into, because what ended is the memory.
 *
 * AND IT WAITS FOR THE CONTEXT TO ACTUALLY BE DEAD before returning, which the
 * first version did not. It signalled and returned 1 -- "the fault is answered"
 * -- so the kernel resumed the guest on a page nobody had installed, and it ran
 * instructions against it in the window before the signal landed. That is how a
 * repair aimed at preventing invented pages came to produce wrong arithmetic:
 * 3 lost increments in 512 runs where the control had none.
 */
static unsigned long n_pull_gone;

static void ctx_stop_unservable(memory_target pid, uint64_t base)
{
	int i;

	n_pull_gone++;
	if (n_pull_gone <= 8)
		fprintf(stderr, "[vmremote] the owner's address space has ended "
			"(context %d asked for 0x%llx): stopping this context, "
			"through its own teardown so whatever waits on it is "
			"released\n", ctx_id(pid), (unsigned long long)base);
	ctx_signal(ctx_id(pid), SIGKILL);
	/*
	 * Bounded, and the bound is not a guess: the caller returns into the
	 * kernel, which resumes the guest, so anything still running here is a
	 * guest executing against a page that will never arrive.
	 */
	for (i = 0; i < 1000 && task_alive(pid); i++)
		usleep(1000);
}

static int ctx_page_pull(memory_target pid, uint64_t base, void *buf,struct source_page_receipt *receipt)
{
	int r = ctx_page_pull_mode(pid, base, buf, 0,receipt);

	if (r == PULL_GONE)
		ctx_stop_unservable(pid, base);
	return r;
}

/*
 * This context's own object, for a page the source has just said it does not
 * hold.
 *
 * The object is where every page this side ever installed lives, and after a
 * fork it holds the parent's memory as of the fork. When the source answers
 * ABSENT that object is the only other place the page's bytes can be -- and if
 * it does not have them either, nobody does and the local kernel's zeros are
 * exactly what the program's own kernel would have given it.
 *
 * Deliberately not gated on page_is_installed(). That record is per context,
 * and the case this exists for is the second context of one address space: a
 * guest thread faulting on the stack its creator built for it reads
 * installed=0 with the bytes sitting in the object it shares. Measured on pg3.
 */
/*
 * The object holds this page: MAP it into this context, do not copy it.
 *
 * own_object_page() reads the object into a buffer and the caller writes that
 * buffer back through the context's own mapping -- and for a page this address
 * space already holds, both ends of that are the SAME folio. It is a
 * read-modify-write of a page that sibling contexts are writing, so every store
 * they land between the pread and the process_vm_writev is discarded.
 *
 * That is hx2's lost write, named at last: the live-page check (VMR_CLOBBER on
 * one page) reports "site4 0x7ffff7ff6000 +88: the guest holds 3024085 and this
 * install writes 2988450 (35635 stores discarded)". 35 thousand stores is 1.5
 * milliseconds of one writer thread -- the time from the pread to the install,
 * with three siblings writing the page throughout.
 *
 * VMCTX_CTL_MAPOBJ is the repair, and it is the same one map-don't-copy already
 * made on the branch above this one; it was simply never applied to the arm the
 * source's ABSENT answer leads to. It runs the fault in this context so its page
 * table points at the object's folio -- the folio the siblings are writing -- so
 * there is one physical page for the address and nothing is copied over
 * anything.
 *
 * Refuses for a page already present in this context: that fault is a
 * protection fault, which is the owner's to decide, and mapping it again would
 * not clear a coherence write-protect.
 */
static void clobber_served_check(memory_target pid, uint64_t base, int site);

static int serve_object_mapped(memory_target pid, uint64_t base, int write)
{
	struct vmctx_mem mm2 = { .addr = base, .len = VMR_PG_SIZE, .buf = 0 };
	int ok;

	/*
	 * "Does the object have it" and "map it" must be ONE step.
	 *
	 * VMCTX_CTL_MAPOBJ faults the page in with get_user_pages_remote() and
	 * deliberately without FOLL_NOFAULT -- faulting it in is the whole point
	 * -- so on a shmem HOLE that fault does not fail, it makes a page:
	 * shmem_fault() allocates a fresh zero folio and inserts it. So between
	 * backing_has() saying the object holds the page and MAPOBJ mapping it,
	 * a hand-over that punches the page turns this call from "map what the
	 * address space has" into "invent a page of zeros and record it as
	 * installed". Nothing writes a byte, so every install-side detector is
	 * silent, and the guest resumes on zeros.
	 *
	 * Measured on hx2 with the divergence check armed, 275us after a take
	 * captured 23877584 from this very page:
	 *
	 *   DIVERGE 0x7ffff7ff6000 +72: ctx=0 ctx=0 ... object=0 mark=6800357
	 *   SERVEDSTALE site80 ... the guest's page reads 0, below the 6800357
	 *       already handed over; the last install here was site70 (the take)
	 *
	 * -- every context and the object all zero, right after the take that
	 * punched it.
	 *
	 * ctl_lock is the exclusion, and it is the right one rather than a new
	 * one: the punch happens inside page_take_mode(), which the page service
	 * calls with exactly this lock held. Holding it across the test and the
	 * map makes them atomic with respect to the only thing that can remove
	 * the page. The proper repair is in MAPOBJ itself -- it should map a
	 * folio the object already has and answer "not there" otherwise, instead
	 * of allocating one -- but that is core-kernel code, and on this box a
	 * core-kernel change needs a rebuild and a manual power cycle.
	 *
	 * HISTORICAL since kernel patch 0055 (session 35 re-read it): MAPOBJ now
	 * maps only a folio the object ALREADY HOLDS (filemap_lock_folio) and
	 * answers a hole as 0 bytes, counted in vmctx_mapobj_holes -- read 0 over
	 * 96.8 M calls. So the allocation described above cannot happen through
	 * this call any more, with or without the lock; the lock stays for the
	 * ordering it gives, not as the thing that prevents an invention. The
	 * GET-from-object punch in pg_serve_conn runs OUTSIDE ctl_lock and is
	 * harmless for the same reason.
	 */
	/*
	 * page_present() opens and reads /proc/<pid>/pagemap, and it is only a
	 * precondition -- it says this is an absent-page fault rather than a
	 * protection one, which cannot change under us here. It is asked
	 * OUTSIDE the lock so the critical section holds nothing but the two
	 * steps that must not be separated. The page service holds this same
	 * lock across a whole TAKEOBJ, so every instruction inside it is
	 * contention on the hottest path in the system.
	 */
	if (page_present(pid, base) == 1)
		return 0;
	/* MAPOBJ's snapshot wrapper takes coh_lock. Taking ctl_lock first
	 * would invert the GET path's coh_lock -> ctl_lock order. Native
	 * MAPOBJ itself makes the existing-folio test and install atomic. */
	coh_enter();
	ok = backing_has(pid, base) &&
	     ctl(pid, write ? VMCTX_CTL_MAPOBJ : VMCTX_CTL_MAPOBJ_READ,
		 &mm2) == (long)VMR_PG_SIZE;
	/* A read mapping preserves outstanding COW obligations and never grants
	 * a write. Retain that restriction until the normal write-fault path
	 * preserves snapshots and explicitly authorizes a writable PTE. */
	if (ok && !write)
		page_watch_set(pid, base, 1);
	coh_leave();
	if (!ok)
		return 0;
	mark_installed(pid, base, VMR_PG_SIZE);
	n_from_own_backing++;
	n_pages_faulted++;
	n_absent_mapped++;
	clobber_served_check(pid, base, 80);	/* mapped the object's folio */
	trail_note(pid, base, TR_OBJMAP, pgen_get(pid, base), 0, NULL);
	/*
	 * And the history check, which needs the bytes rather than a mark. This
	 * path installs NOTHING -- it maps the object's folio and answers -- so
	 * the only way to know what the guest was just given is to read it.
	 * That is the path page 0x4d1000 actually uses: it is installed once, at
	 * first touch, and thereafter moves by take and by object mapping.
	 */
	if (hist_armed(base)) {
		static __thread char seen[VMR_PG_SIZE];
		unsigned long rev0 = n_hist_revert;

		if (page_read_as(pid, base, seen))
			hist_check(base, seen, 80);
		/*
		 * The check just said the object's folio is one write behind
		 * the guest. That is only constructible if the object's folio
		 * and the folio some context is writing are TWO PHYSICAL PAGES
		 * -- so photograph the page tables at this instant: the PFN
		 * every member of the address space maps for this address.
		 * Two different PFNs is the split-brain, and which context
		 * holds which is the whole question.
		 */
		if (n_hist_revert != rev0) {
			memory_target mem[MAX_CTX];
			int nm = as_members(pid, mem, MAX_CTX), mi;

			for (mi = 0; mi < nm; mi++) {
				uint64_t ent = 0;
				if(!ctx_pagemap_read(mem[mi],base,&ent))
					fprintf(stderr, "[vmremote]   PFN ctx "
						"%d%s: present=%d pfn=0x%llx\n",
						ctx_id(mem[mi]),
						mem[mi] == pid ? " (asker)" : "",
						(int)((ent >> 63) & 1),
						(unsigned long long)
						(ent & ((1ULL << 55) - 1)));
			}
		}
	}
	return 1;
}

static int own_object_page(memory_target pid, uint64_t base, void *buf)
{
	int bf;

	if (!backing_has(pid, base))
		return 0;
	bf = backing_find(pid);
	if (bf < 0)
		return 0;
	return pread(bf, buf, VMR_PG_SIZE, (off_t)base) == (ssize_t)VMR_PG_SIZE;
}

/*
 * A claim the source won, answered by letting the guest fault again.
 *
 * The guest re-executes the instruction, faults on the same address, and asks
 * again -- by which time the source's own claim has landed and it is the holder
 * and can hand it over. That is the cheap side of the race by design: the
 * source is inside a kernel syscall with a context asleep behind it and must
 * not be made to wait, and this side has only a userspace round trip to lose.
 *
 * Bounded, because a yield that never ends is a livelock with no address in it.
 * Repeated replies only increase throttling; they never authorize a fallback.
 */
static unsigned long n_claim_yield, n_claim_gaveup;
static unsigned long n_handoff_yield;	/* faults that yielded to an in-flight take */

static int claim_yield_pid(memory_target pid, uint64_t base)
{
    static __thread uint64_t last_mm,last_page;
    static __thread unsigned long same;
    uint64_t mm=as_id(pid);

    if(mm!=last_mm || base!=last_page) {
        last_mm=mm;last_page=base;same=0;
    }
    if(++same<=2000) {
        n_claim_yield++;
        usleep(200);
        return 1;
    }
    /* A preexisting backing folio may be an older retained copy. Neither
     * its presence nor elapsed retries settle the source's current claim.
     * Yield and ask again; the native fault deadline bounds stalled work. */
    n_claim_gaveup++;
    if(n_claim_gaveup<=8)
        fprintf(stderr,"[vmremote] unresolved claim for MM %llu page 0x%llx after %lu replies; throttling without granting ownership\n",
            (unsigned long long)mm,(unsigned long long)base,same);
    same=0;
    usleep(2000);
    return 1;
}

static int fault_from_home_inner(memory_target pid, uint64_t vec, uint64_t addr,
				 uint64_t err, int supply_only, int source_allows_access);

/*
 * A chunk/page fetch from the source, with coh_lock dropped across the network
 * round trip and taken back after.
 *
 * The lock must not span a blocking step (coh_watch photographs the holder's
 * kernel stack sitting in a socket read; the SLOW report names it "the locking
 * defect"). Measured on netsurf: the source's own take of a page held by an
 * in-flight syscall is refused for a full second before it answers CLAIMING,
 * so a home_call for that page blocks ~1.18s -- and with coh_lock held the
 * whole time every other guest fault queues behind it into the kernel's 2s
 * deadline (exit 131). coh_drop_all() releases ALL nesting levels (a plain
 * coh_leave drops one, which leaves the lock held when this fault is nested
 * inside the page server), and only the local `page` buffer and the socket are
 * touched while it is dropped -- every judgment about shared object state is
 * re-made under the lock after the fetch returns.
 */
/* Single-page round-trip timing is diagnostic. It never expands the set of
 * pages captured without a held reservation and a confirmed landing. */
static __thread uint64_t ctxpage_rtt_us;
static unsigned long n_ra_rtt_samples;

static long ctxpage_fetch(memory_target pid,const uint64_t a[6], void *page, uint64_t pagemax,
        struct source_page_receipt *receipt)
{
	int cd = coh_drop_all();
	uint64_t t0 = now_us();
	long got = source_page_fetch(pid,a,page,pagemax,receipt);

	coh_restore(cd);
	if (a[1] == 4096 && got > 0) {
		uint64_t dt = now_us() - t0;

		ctxpage_rtt_us = ctxpage_rtt_us ? (ctxpage_rtt_us * 7 + dt) / 8 : dt;
		n_ra_rtt_samples++;
	}
	return got;
}

#include "fault-dispatch.h"

/* A missing shared-object page cannot be reconstructed from an address-keyed
 * retained copy or a local zero page. Stop the execution session on an
 * unresolved protocol error, including when called by the page server. */
static _Noreturn void shared_page_unavailable(memory_target pid, uint64_t base,
					     int answer, const char *why)
{
	fprintf(stderr, "[vmremote] shared page 0x%llx for context %d is "
		"unavailable: %s (answer=%d); stopping without supplying bytes\n",
		(unsigned long long)base, ctx_id(pid), why, answer);
	ctx_stop_all();
	ctx_signal(ctx_id(pid), SIGKILL);
	fflush(stderr);
	_exit(VMR_EXIT_INVENTED_PAGE);
}

static int fault_from_home_inner(memory_target pid, uint64_t vec, uint64_t addr,
				 uint64_t err, int supply_only, int source_allows_access)
{
	/*
	 * Per thread. The page service calls this too (to fill a page before a
	 * partial write-back lands on it), and a guest that forks has a service
	 * thread per context — so a single shared buffer lets one thread's
	 * chunk be written into the guest at another thread's address.
	 */
	char page[FAULT_CHUNK];
	struct source_page_receipt receipt
		__attribute__((cleanup(source_receipt_finish)))={0};
	struct region saved_region;
	const struct region *r;
	/* The faulting page had to be put into the backing object rather than
	 * through the address space; declining is what maps it. See below. */
	int via_object = 0;
	uint64_t base = addr & ~0xfffUL;
	uint64_t a[6] = { 0 };
	uint64_t start, len;
	long got;

	/*
	 * Everything below this is about supplying memory: find the region, ask
	 * the source for the page, install it. An exception that is not a memory
	 * fault has no page to supply and no address to look up -- all of it is
	 * meaningless for one -- so it goes straight to the owner and no further.
	 *
	 * The test is on the vector rather than on a list of exceptions this
	 * side knows about, because this side is not supposed to know about any
	 * of them. 14 is the one it must service; the rest are somebody's else's
	 * to interpret, whatever they turn out to be.
	 */
	if (vec != 14) abort(); /* non-page exceptions bypass page service */
	struct access_range access;
	if (access_lookup(as_id(pid), base, base + VMR_PG_SIZE, &access) &&
	    (access.denied ||
	     ((err & 2) && !(access.protection & VMR_PROT_WRITE)) ||
	     ((err & 16) && !(access.protection & VMR_PROT_EXEC)) ||
	     (!(err & (2 | 16)) && !(access.protection & VMR_PROT_READ)))) {
		if (supply_only) return 0;
		if (!source_allows_access) return FAULT_NEEDS_OWNER;
	}

	/* Having bytes does not authorize an access. A mapped PROT_NONE page
	 * can still be held in our object, and reads of it must reach the
	 * source's exception handler just as forbidden writes do. The local
	 * VMA can be stale, so only the source decides whether this is an
	 * exception; a legal answer continues through ordinary page service. */
	if (!supply_only && (fault_pgs & VMCTX_PGS_MAPPED) &&
	    (((err & 2) && !(fault_vmaprot & PROT_WRITE)) ||
	     ((err & 16) && !(fault_vmaprot & PROT_EXEC)) ||
	     (!(err & (2 | 16)) && !(fault_vmaprot & PROT_READ)))) {
		if (!source_allows_access) return FAULT_NEEDS_OWNER;
	}

	struct shared_range shared_mapping;
	if (shared_layout_lookup(as_id(pid), base, &shared_mapping)) {
		/* Every context maps the source's object at its assigned offset.
		 * Inherited and newly shared thread mappings use these same records.
		 * A fault installs only its own page; no peer is chosen by matching VA.
		 * Preserve the source's permissions, including read-only inheritance. */
		if (!supply_only &&
		    (((err & 2) && !(shared_mapping.prot & PROT_WRITE)) ||
		     ((err & 16) && !(shared_mapping.prot & PROT_EXEC)) ||
		     (!(err & (2 | 16)) && !(shared_mapping.prot & PROT_READ)))) {
			if (!source_allows_access) return FAULT_NEEDS_OWNER;
			return 0;
		}
		fault_map.op = VMCTX_MAP_SET_SHARED_PAGE;
		fault_map.addr = base;
		fault_map.len = VMR_PG_SIZE;
		off_t slot = shared_page_slot(shared_obj, shared_mapping.object_id,
			shared_mapping.off + base - shared_mapping.st, 1);
		if (slot < 0)
			shared_page_unavailable(pid, base, 0, "cannot allocate shared page identity");
		fault_map.off = slot;
		fault_map.prot = shared_mapping.prot;
		/*
		 * "The mapping comes from the object, so the local kernel
		 * attaches the object's page" is true only when the object HAS
		 * the page. Where it does not, what the local kernel attaches
		 * is a hole -- 4096 zeros nobody sent, which for a shmem object
		 * is the shared zero page (AUDIT XIII §4), installed for good
		 * over memory whose bytes exist somewhere else.
		 *
		 * Measured before this: one pass of the suite declined 70
		 * faults, 22 of which the kernel's own read-back showed the
		 * guest retrying on `sum 0x04000001` -- every one of the 22
		 * through this branch, and 7 of them over a page this side had
		 * installed or lent (AUDIT XIII §3). Two things are wrong in
		 * exactly the same way:
		 *
		 *   - a file's own contents, which live on the source and were
		 *     never written into this object by anyone (rd1
		 *     initial-guest / initial-forward);
		 *   - a page this side lent to the shadow, whose current bytes
		 *     are on the source and whose slot here is stale or
		 *     punched.
		 *
		 * Both are answered by the question this side already has a
		 * word for: ask the owner. The bytes go into the object at the
		 * offset the mapping reads from, and the decline that follows
		 * is then the *good* kind -- the same one
		 * `one-page-written-into-the-object` is, where the local
		 * kernel's service is how real bytes get mapped.
		 *
		 * The blanket repair -- "a decline ends the context" -- is
		 * refuted and not attempted: 45 of those 70 declines are how a
		 * page legitimately arrives, and ending the context on them
		 * breaks fm1 eighteen times in one run (AUDIT XIII §5).
		 * Nothing here is specific to a syscall or a test: it is the
		 * page rule, asked of the one object that could not answer it.
		 */
		/*
		 * The question is whether the object HAS the page, and nothing
		 * else. `page_lent_state() == LENT_SHADOW` was in this
		 * condition too -- "a page lent to the shadow may be stale
		 * here" -- and it was measured to be wrong, on sm1's
		 * `posix-shm`, which went from ok to reading an empty string:
		 * `lent[]` is keyed by address and is never cleared when the
		 * program unmaps one, and sm1 maps three different objects at
		 * the same address on purpose. So the record named a loan
		 * belonging to a mapping that no longer existed, the fetch
		 * overwrote the object slot the guest had just stored "posix"
		 * into, and both mappings of it then read zeros. A stale record
		 * is not evidence (PRINCIPLES §1: ownership is taken, not
		 * inferred); the object's own SEEK_DATA is.
		 */
		{
			off_t off = shared_obj_off(pid, base);

			if (off < 0 || shared_obj < 0)
				shared_page_unavailable(pid, base, 0,
							"mapping has no backing object");
			if (!shared_obj_has(off)) {
				static __thread char sp[VMR_PG_SIZE];
				int answer;

				n_shared_hole++;
				answer = ctx_page_pull(pid, base, sp,&receipt);
				if (answer == PULL_BYTES) {
					if (pwrite(shared_obj, sp, VMR_PG_SIZE,
						   off) ==
					    (ssize_t)VMR_PG_SIZE) {
						n_shared_filled++;
						mark_installed(pid, base,
							       VMR_PG_SIZE);
						ack_installed(&receipt,base);
						if (supply_only)
							ack_flush();
						plog(base, "shared object had "
						     "no page at off=%lld; "
						     "filled it from the owner "
						     "(sum=%08x)",
						     (long long)off,
						     page_sum(sp));
						/* DONE applies fault_map before retrying. SELF
						 * ignores it and could fault an inherited hole
						 * through the wrong backing object. */
						return !supply_only;
					}
					shared_page_unavailable(pid, base, answer,
							"received bytes could not be installed");
				}
				/* CLAIMING is an in-flight transfer. Release our local
				 * claim and retry the fault; no mapping or bytes change. */
				if (answer == PULL_CLAIM) {
					if (supply_only)
						shared_page_unavailable(pid, base, answer,
								"partial write has no complete page");
					n_claim_yield++;
					n_shared_hole_claim++;
					fault_map.op = 0;
					return 1;
				}
				if (answer == PULL_GONE) {
					fault_map.op = 0;
					return 1; /* ctx_page_pull already stopped the context */
				}
				if (answer == PULL_NOMAP && !supply_only) {
					fault_map.op = 0;
					if (!source_allows_access) return FAULT_NEEDS_OWNER;
					return 1;
				}
				/* ABSENT says the source is not the holder. It does not
				 * authorize initialization. A concurrent landing may have
				 * filled this object while the source answered. */
				if (answer == PULL_ABSENT && shared_obj_has(off))
					return !supply_only;
				shared_page_unavailable(pid, base, answer,
							"owner supplied no current page");
			}
		}
		return !supply_only;
	}

	r = region_snapshot(pid, addr, &saved_region) ? &saved_region : NULL;

	/*
	 * No settling from the parent here.
	 *
	 * An earlier version read a page the parent held out of the parent's
	 * address space on this machine and installed it in the child. It worked,
	 * and it was the wrong side: it made the destination the place a forked
	 * child's memory came from. Asking the source covers the same case --
	 * a child's shadow is a fork of its parent's and holds what the parent had
	 * pushed back, with the oracle behind it for anything untouched.
	 */

	/*
	 * Declining a fault hands it to the local kernel, which for these
	 * anonymous mappings means a zero page — permanently, and silently.
	 * Zeros in a text page are executable (0x00 0x00 stores through %rax),
	 * so a wrongly declined fault surfaces much later as a nonsense fault
	 * at a nonsense address. Every decline is therefore reported.
	 */
	if (err & 1) {			/* present: a protection fault */
		/*
		 * The guest writing a page it shares. Until coherence there was
		 * nothing to do with one of these — the guest's permissions were
		 * its own kernel's business — so they were counted and dropped.
		 * Now it is the moment ownership changes: the shadow is holding
		 * a readable copy, and it has to stop before the guest may
		 * change the page under it.
		 */
		uint64_t owner;
		int shared;

		coh_enter();
		owner = page_holder(pid, base);
		shared = owner ? page_shared(pid, base) : 0;
		if (owner) {
			/*
			 * The guest wants to write a page the source is holding
			 * -- a readable copy or the page outright -- so that
			 * copy has to stop existing before the write is granted.
			 *
			 * The test used to be `owner && shared`, and a page held
			 * outright (shared == 0) fell past every branch below to
			 * DECLINE("write-to-an-unlent-read-only-page"), which
			 * the guest answers by faulting on the same address
			 * until vmremote's own watchdog kills it (-ECANCELED,
			 * reported as exit 131). Nothing reached that hole while
			 * the ownership record was fabricated: the false loan
			 * was always recorded *shared*, so every write arrived
			 * here with shared == 1. Making the record honest made
			 * the hole reachable -- which is the third job the false
			 * loan turns out to have been doing. Not marking pages,
			 * as page_keep() assumed: routing writes.
			 *
			 * th9 reaches it because emit() writes from a stack
			 * buffer. Every write(1, ...) has home read the guest's
			 * stack to perform the syscall, which takes that page
			 * with shared == 0, and the guest's next store into the
			 * same buffer is the write with nowhere to go. Measured
			 * in a failing run: one DECLINED-BY
			 * write-to-an-unlent-read-only-page, both contexts
			 * ending -125, and NOTHOLDER zero -- so it was never the
			 * chunk path at all.
			 *
			 * Shared or held outright, the answer is the same and is
			 * what the rest of this branch already does: take the
			 * page back with the pull, then grant the write.
			 *
			 * Below is the history of the pull itself. It asked for it back
			 * with page_recall() -- and page_recall() cannot
			 * succeed: it needs a channel the shadow claims with
			 * VMR_PG_HELLO_RECALL, vmhome sends no such thing, and
			 * recall_find() therefore searches a table nothing
			 * registers in. The return value was discarded, the
			 * loan was cleared anyway, and the guest was granted an
			 * exclusive write to a page the other machine still had
			 * and still believed in.
			 *
			 * That is PRINCIPLES §1 exactly -- both sides holding
			 * one page, both able to write it -- and it is where
			 * pg3's "A N behind" run stops making progress: the
			 * last thing that happens to the page before the guest
			 * and the source diverge for the rest of the run is
			 * this branch.
			 *
			 * The pull is the operation that does work, and it is
			 * the same one every other path here uses:
			 * VMR_OP_CTXPAGE takes the page out of the context at
			 * the source and returns the bytes, so afterwards the
			 * source genuinely is not a holder. It is asked for
			 * here rather than after the recall, because a recall
			 * that cannot fire is not a first attempt at anything.
			 *
			 * This has no switch. It used to promise VMR_NO_PROT_PULL,
			 * which was never implemented -- the name appears nowhere
			 * else in this file -- so the "both arms on one boot" that
			 * sentence offered was never available to anyone.
			 */
			int taken = 0;

			{
				int pr;

				n_prot_pull++;
				coh_leave();
				pr = ctx_page_pull(pid, base, page,&receipt);
				coh_enter();
				if (pr == PULL_BYTES) {
					n_prot_pull_bytes++;
					/* Same rule as the read-fault install
					 * below: if this address space holds the
					 * page, the copy that just came back is
					 * the one that left, and writing it over
					 * the live folio discards every store the
					 * siblings have made since. */
					if (serve_object_mapped(pid, base, !!(err & 2))) {
						taken = 1;
					} else {
					poke_site = 30;	/* branch 6, after the pull */
					taken = poke(pid, base, page,
						     VMR_PG_SIZE) == (long)VMR_PG_SIZE;
					poke_site = 0;
					}
					if (taken)
						mark_installed(pid, base,
							       VMR_PG_SIZE);
				} else if (pr == PULL_ABSENT ||
					   pr == PULL_NOTHOLDER) {
					/*
					 * The source does not hold it after
					 * all, so the guest's own copy is the
					 * only one and making it writable is
					 * right. Counted apart: this is the
					 * case the old branch assumed always
					 * held.
					 */
					n_prot_pull_absent++;
				} else {
					n_prot_pull_busy++;
					/* No ownership transfer completed. In particular,
					 * CLAIMING cannot clear the source loan or grant a
					 * local write to its in-flight page. */
					coh_leave();fault_map.op=0;
					if(pr==PULL_CLAIM || pr==PULL_GONE)return 1;
					if(pr==PULL_NOMAP)return supply_only ? 0 : FAULT_NEEDS_OWNER;
					source_memory_failed(pid,base,"write recall",pr);
				}
				if(pr==PULL_BYTES && !taken)
					source_memory_failed(pid,base,"write recall landing",pr);
			}
			page_lend(pid, base, 0);
			if (taken || page_make_writable(pid, base)) {
				n_unprotected++;
				clobber_served_check(pid, base, 83);
				coh_leave();
				if (taken)	/* the pull's bytes landed */
					ack_installed(&receipt,base);
				do { plog(base, "SERVED-BY site3"); return 1; } while (0);
			}
		}
		/*
		 * A page this side protected in order to see this very write,
		 * and that nobody holds. Nothing has to be asked for and nobody
		 * has to be told: the guest is the only holder, so give it back
		 * the right to write and let it carry on. Without this the test
		 * below would send the fault to owner_takes_fault() and the
		 * program would be handed a signal it never earned.
		 */
		/*
		 * A store to a page a fork write-protected. What it means is
		 * not this side's to work out: it reports the fact -- "context
		 * X is about to write the page at A, which this side holds" --
		 * names no relationship, and does what the answer says.
		 *
		 * Three answers, and the third is why this cannot be a grant
		 * with a question attached to it:
		 *
		 *   COWBREAK  an address space copied from this one has not
		 *             been given this page. Give it the bytes that are
		 *             there NOW -- which is what makes them the bytes
		 *             as of the fork rather than as of this fault --
		 *             and then let the store land.
		 *   0         nothing is owed; the protection is spent and the
		 *             store lands.
		 *   -EACCES   the program may not write here at all. The fork's
		 *             protection is not the reason this faulted, and
		 *             granting it would turn the program's own
		 *             segmentation fault into a successful store. Fall
		 *             through to the owner, which is what answers that.
		 *
		 * This is the half that makes a copy's own COWBREAK correct.
		 * Without it the original's stores after the fork land on the
		 * one page both address spaces are still reading, and a copy
		 * that faults later is handed memory as of its fault rather
		 * than as of the fork. tests/cow1.c is that measurement.
		 */
		if (!owner && cow_was_protected(as_id(pid), base)) {
			uint64_t wq[6] = { base, VMR_PG_SIZE, 0, 0, 0, 0 };
			long w;

			n_cow_writeprep++;
			coh_leave();		/* no network under coh_lock */
			w = home_memory_call(pid, VMR_OP_WRITEPREP, wq, NULL, 0, NULL, 0,NULL);
			coh_enter();
			if(home_call_failed || (w!=0 && w!=VMR_CTXPAGE_COWBREAK &&
			   w!=-EACCES && w!=-EFAULT))
				source_memory_failed(pid,base,"write preparation",w);
			if(w==-EACCES || w==-EFAULT) {
				coh_leave();fault_map.op=0;
				return supply_only ? 0 : FAULT_NEEDS_OWNER;
			}
			if (!home_call_failed && w == VMR_CTXPAGE_COWBREAK) {
				n_cow_writeprep_break++;
				if (cow_break_copies(pid, base))
					w = 0;
				else
					source_memory_failed(pid,base,"preserving child snapshot",w);
			}
			if (w == 0) {
				/*
				 * The mark is spent only once the write-enable
				 * has actually worked, exactly as the watched
				 * one below is and for the same reason: cleared
				 * first, a failed grant leaves the page
				 * read-only with nothing left to explain why,
				 * and every later fault on it falls through to a
				 * decline the guest answers by faulting again
				 * until the watchdog ends it.
				 */
				if (page_make_writable(pid, base)) {
					if (w == 0)
						cow_prot_set(as_id(pid), base,
							     COW_PROT_SPENT);
					n_unprotected++;
					coh_leave();
					do { plog(base, "SERVED-BY site3c (the "
					     "store that ended the sharing a "
					     "fork made)"); return 1; } while (0);
				}
			}
		}
		if (!owner && page_watched(pid, base)) {
			n_watch_grant++;
			/*
			 * The mark is spent only once the write-enable has
			 * actually worked, and that order is the whole fix.
			 *
			 * It used to be cleared first. When page_make_writable()
			 * then failed, the page was left write-protected with
			 * neither an owner nor a mark -- and nothing downstream
			 * can answer a write fault in that state, so it falls to
			 * DECLINE("write-to-an-unlent-read-only-page"), which
			 * the guest answers by faulting on the same address
			 * until vmremote's watchdog kills it (-ECANCELED, seen
			 * as "exit 131"). One lost write-enable ends the run.
			 *
			 * Measured, 20 th9 runs on one binary: nineteen passed
			 * with this counter at 0 and no declines at all; the one
			 * failure was the one run where it was 1, and it had
			 * exactly one decline. The correlation is the whole of
			 * the evidence -- the counter was added before the fix
			 * and named the run that would fail.
			 *
			 * Leaving the mark on costs nothing: the page is still
			 * protected, so the guest faults again and arrives back
			 * here, where the write-enable can be retried. Clearing
			 * it early threw away the only record that explains why
			 * the page is read-only.
			 */
			if (!page_make_writable(pid, base))
				n_watch_grant_nowrite++;
			else if (page_watch_set(pid, base, 0), 1) {
				n_unprotected++;
				clobber_served_check(pid, base, 84);
				coh_leave();
				do { plog(base, "SERVED-BY site3w (the write "
				     "this side was watching for)");
				     return 1; } while (0);
			}
		}
		coh_leave();
		/*
		 * Before declining: is this the program's own fault rather than
		 * a coherence event? A page the program mprotected read-only
		 * faults here on a write, with no loan on it and nothing for
		 * the protocol to do -- and the local kernel cannot grant a
		 * write the program forbade itself. Same question, same
		 * handover as an absent page; the lock is already dropped.
		 */
		if (!owner && !supply_only) {
			if (!source_allows_access) return FAULT_NEEDS_OWNER;
			{
				/*
				 * THE OWNER'S KERNEL SAYS THE WRITE IS LEGAL.
				 * Then this present-page write fault is not the
				 * program's: it is a protection the protocol
				 * left in THIS context's page table. A hand-over
				 * (PROTECTOBJ) write-protects the folio in every
				 * sibling's mapping; the sibling that pulled the
				 * page back re-grants only its own (MAPOBJ), and
				 * spends the watch/loan for the whole address
				 * space -- so the next sibling's store arrives
				 * here with no owner, no watch and no mark, and
				 * used to be handed to the owner as an exception
				 * and declined to a page of zeros (hx2: 11 of 48
				 * pool runs, session 40). The guest is the only
				 * holder; give it back the right to write, in
				 * this context, under coh_lock, and only if no
				 * hand-over has claimed the page in the round
				 * trip it took to ask: an owner that appeared
				 * meanwhile is the owner branch's to handle on
				 * the re-fault.
				 */
				coh_enter();
				owner = page_holder(pid, base);
				if (owner) {
					n_prot_legal_owner++;
					coh_leave();
					return 1;	/* re-fault: owner branch */
				}
				if (page_make_writable(pid, base)) {
					n_prot_legal_granted++;
					n_unprotected++;
					clobber_served_check(pid, base, 85);
					coh_leave();
					do { plog(base, "SERVED-BY site4L (a "
					     "stale protection: the owner's "
					     "kernel allows the write)");
					     return 1; } while (0);
				}
				n_prot_legal_nogrant++;
				coh_leave();
			}
		}
		/*
		 * Declining is handing the fault to the local kernel, and for these
		 * mappings that means it simply grants the write: the guest changes
		 * the page and nothing tells the source. That is harmless only if
		 * the source has no copy of this page -- and the loan table is the
		 * record of whether it does. So say which of the three cases this
		 * was rather than counting them all as one number.
		 *
		 * "no loan" is the ordinary one: a page nobody borrowed, left
		 * read-only by a protect whose loan has since been cleared.
		 *
		 * The other two would be real breaks in the model. If the page is
		 * lent -- whole, or shared and the share could not be broken --
		 * then the source is holding a copy that the write about to be
		 * granted makes stale, and the next syscall that reads that buffer
		 * is served the source's copy without faulting.
		 *
		 * Measured over a hundred and twenty runs of th9, including every
		 * failing one: zero. All the declines are of the first kind. This
		 * is here to keep it that way, not because it has ever fired.
		 */
		if (!owner) {
			n_decl_prot_free++;
		} else {
			/*
			 * Measured, pg3, sixteen runs with one death: this line
			 * fired once, in the run that died, on the guest
			 * thread's stack page -- and not once in the fifteen
			 * that passed. It is a §1 break with a count on it: the
			 * source holds the page and the local kernel is about
			 * to grant the guest a write to it.
			 *
			 * Before that is allowed to happen, ask the one
			 * question that can make the fault meaningless: is the
			 * page still there? The fault says "present" as of when
			 * the guest took it, and a GET between then and now
			 * takes the page away -- which makes this a fault about
			 * a mapping that no longer exists. Declining THAT hands
			 * it to the local kernel, which for an anonymous
			 * address means do_anonymous_page() and a page of zeros
			 * installed for good over a live thread stack (part X
			 * §2b, part III E2). Retrying instead costs one fault
			 * and invents nothing: the guest faults again, finds
			 * the page absent, and is served the ordinary way.
			 */
			if (page_present(pid, base) != 1) {
				n_decl_prot_stale++;
				if (n_decl_prot_stale <= 8)
					fprintf(stderr, "[vmremote] the write to "
						"0x%llx by context %d faulted on a "
						"mapping that has since been taken "
						"away (shadow %llu holds it %s); the "
						"access retries rather than being "
						"handed to the local kernel, which "
						"would put an empty page there\n",
						(unsigned long long)base, ctx_id(pid),
						(unsigned long long)owner,
						shared ? "shared" : "whole");
				return 1;
			}
			if (shared)
				n_decl_prot_stuck++;
			else
				n_decl_prot_whole++;
			if (n_decl_prot_lent++ < 8)
				fprintf(stderr, "[vmremote] DECLINED a write to "
					"0x%llx by context %d, which shadow %llu "
					"holds (%s): the local kernel will grant "
					"it and the source's copy goes stale\n",
					(unsigned long long)base, ctx_id(pid),
					(unsigned long long)owner,
					shared ? "shared, and it could not be "
						 "taken back" : "whole");
		}
		n_decl_prot++;
		if (owner)
			DECLINE("write-to-a-page-the-source-holds");
		DECLINE("write-to-an-unlent-read-only-page");
	}
	/*
	 * From here on this page may change sides, so nobody else may be
	 * moving it. Every path out of here releases it again. The per-page
	 * ownership state (REQUESTING) was claimed by the fault_from_home_mode
	 * wrapper before this inner body ran; a take that races it waits on that.
	 */
	coh_enter();
	/*
	 * If another thread is mid-pull on this very page, wait for its
	 * install rather than starting a second pull: the second recall would
	 * find the shadow already empty-handed and fall back to stale bytes.
	 * After the wait the page is usually present (the install pokes the
	 * object every context shares), so the fault just retries.
	 */
	{
		int p;

		coh_leave();
		if (pull_wait(base, 500)) {	/* same nesting rule */
			p = page_present(pid, base);
			plog(base, "FAULT waited out a concurrent pull; "
			     "present=%d", p);
			if (p == 1)
				return 1;	/* installed; retry the access */
		}
		coh_enter();
	}
	/*
	 * Is the shadow holding this page? Then it is not missing, it has
	 * moved, and the answer is to ask for it back rather than to fetch a
	 * fresh copy from home's oracle — which would be the *old* contents and
	 * would throw away whatever the syscall side has written since.
	 *
	 * This is checked before the region lookup on purpose: a page that was
	 * taken is one this context had, so where it originally came from no
	 * longer matters, and a lent page in a region marked local would
	 * otherwise be declined and quietly zero-filled.
	 */
	{
		uint64_t owner = page_holder(pid, base);

		/* A watch is a local write restriction, not a source loan. A
		 * sibling can lack a PTE while this MM still owns the watched
		 * folio. Populate that same folio read-only for every access. A
		 * store then faults on its write restriction and follows the
		 * normal COW/watch grant path above. Asking the source can start a second
		 * transfer which the landing path then skips because of the watch. */
		if (page_watched(pid, base) &&
		    serve_object_mapped(pid, base, 0)) {
			coh_leave();
			plog(base, "watched local folio mapped with write restriction");
			return 1;
		}

		if (owner) {
			/*
			 * A read of a page the shadow holds outright: ask it to
			 * keep the page but stop writing it, and share. Only a
			 * write needs it moved, and the guest's write would
			 * arrive as a protection fault above.
			 */
			int want_shared = !(err & 2);
			int was_shared = page_shared(pid, base);
			int got;
			int yield = 0, src_absent = 0, not_holder = 0;
			/*
			 * Ask the machine that has it, which is the pull this
			 * protocol is built on, before trying to have it handed
			 * back over a channel that does not exist.
			 *
			 * A recall asks the holder to give the page up on
			 * request. vmhome opens no such channel and sends no
			 * HELLO_RECALL, so recall_find() searches a table
			 * nothing registers in and every one of these fails --
			 * 321 hand-overs and 0 recalls on the run that measured
			 * it. The failure then falls through to a path that can
			 * decline, and a declined fault on a page that has been
			 * punched out of the backing object is answered by the
			 * local kernel with a freshly allocated zero page. That
			 * page is present, so the guest never faults on the
			 * address again, never asks for it, and reads zeros for
			 * the rest of the run.
			 *
			 * Traced on tests/pg2.c at 0x7ffff7ff6000: thirteen
			 * rounds of "GET asked ... present=1" and "take r=1
			 * post=0" carrying each round's payload, then one LOST
			 * line, then no further activity on the page at all
			 * while the program read 0x00 for 287 rounds.
			 *
			 * VMR_OP_CTXPAGE is the operation for exactly this: it
			 * takes the page out of the context at the source and
			 * returns the bytes, so the two pulls are symmetric and
			 * one side holds it at a time. It needs no channel and
			 * no cooperation beyond the one already required.
			 */
			/*
			 * NOT under the coherence lock. The pull is a network
			 * round-trip, and home may at this same moment be
			 * GETting a page out of this side -- a request our own
			 * page server can only answer by taking this lock. The
			 * in-flight marker is what keeps the page safe while
			 * the lock is dropped; see pull_begin().
			 */
			{
				int marked = pull_begin(base);
				int cd = marked ? coh_drop_all() : 0;
				int pr;

				pr = ctx_page_pull(pid, base, page,&receipt);
				/*
				 * Four answers, four things to do, and only one
				 * of them is "here are the bytes".
				 *
				 * CLAIMING is the simultaneous claim: the
				 * source has marked this page and has a request
				 * for it outstanding. The rule is that the
				 * source wins, so this side gives up its own
				 * claim and lets the guest fault again -- one
				 * round trip, against a syscall held up on the
				 * other machine.
				 *
				 * ABSENT is the source saying it does not hold
				 * the page. It is not a recall failure and not
				 * a fault: it means this side's own copy is the
				 * current one, so there is nothing to ask back
				 * and nothing lost.
				 *
				 * NOTHOLDER is the fourth, and it is the
				 * opposite of ABSENT however alike they look:
				 * the source gave this page to THIS side, so
				 * "our own copy is the current one" stops being
				 * a fallback and becomes the statement that the
				 * only copy is the one we cannot find.
				 */
				if (pr == PULL_CLAIM) {
					yield = 1;
					got = 0;
				} else if (pr == PULL_BYTES) {
					got = 1;
				} else if (pr == PULL_ABSENT) {
					src_absent = 1;
					got = 0;
				} else if (pr == PULL_NOTHOLDER) {
					not_holder = 1;
					got = 0;
				} else {
					got = want_shared ?
						page_downgrade(pid, base, owner, page) :
						page_recall(pid, base, owner, page);
				}
				if (marked) {
					coh_restore(cd);
					pull_end(base);
				}
			}
			if (yield) {
				coh_leave();
				if (claim_yield_pid(pid, base))
					return 1;	/* fault on it again */
				/*
				 * The bound is up. Stop yielding and take the
				 * ordinary path, which will say what it finds
				 * rather than spin.
				 */
				coh_enter();
				src_absent = 1;
			}

			/*
			 * "Shared" here means the source keeps a readable copy
			 * and the guest keeps a write-protected one, so a page
			 * both sides only read stops moving. It is the right
			 * idea and this is not it.
			 *
			 * The bytes came from VMR_OP_CTXPAGE, and that
			 * operation is a *take*: ctx_page() removed the page
			 * from the source and recorded PG_THEIRS before
			 * answering. The source is not a holder any more. So
			 * recording it as one -- page_lend_mode(..., owner, 1)
			 * -- writes down a loan that was never made, and
			 * write-protecting the guest's copy protects it from a
			 * second writer that does not exist.
			 *
			 * What that costs, measured on pg3: the guest's next
			 * write to its own page is a protection fault, which
			 * finds the fabricated loan, calls a page_recall() that
			 * cannot fire (no such channel), clears the loan and
			 * makes the page writable again -- 23000 to 80000 times
			 * a run, and when the pull that replaced the recall was
			 * asked, the source said it did not hold the page in
			 * 100% of cases over eight runs. That is the counted
			 * defect of ARCHITECTURE.md §9.2a, and this line is
			 * where the record comes from.
			 *
			 * So a pull that returned the bytes is recorded for
			 * what it is: the guest holds the page and nobody else
			 * does. Real read-only sharing needs an operation that
			 * leaves the source a copy, which VMR_OP_CTXPAGE is
			 * not; until there is one, claiming to have done it is
			 * worse than not doing it.
			 *
			 * Removing the record *and* the protection with it
			 * measured worse -- pg3, eight runs per arm
			 * alternating, 7 of 8 against 4 of 8 -- which said
			 * which of the two was load-bearing. The write-protect
			 * is: it is the only thing that turns the guest's next
			 * write into a fault this side can see, and the loan
			 * was merely what routed that fault to "make it
			 * writable" instead of to owner_takes_fault(), which
			 * would hand the program a signal it never earned.
			 *
			 * So the protection was kept and the lie dropped, with
			 * a mark that says what is true -- *this side
			 * protected this page in order to see the next write*
			 * -- which branch 6 acts on directly. It is built, it
			 * is below, and it is OFF, because it measured worse:
			 *
			 *   pg3, 12 runs per arm alternating   6 of 12 : 6 of 12
			 *   pg2, 16 runs                       16 of 16
			 *   th9, 12 runs per arm alternating   12 of 12 : 9 of 12
			 *
			 * th9 is the one that decides it, and how it fails
			 * says why: the runs that broke lost the tail of their
			 * output -- "main=40 thread=38 done=0". The owner
			 * field is not only read by this branch. The chunk
			 * install loop skips a page `page_holder()` names, and
			 * so do several other paths; a page with no recorded
			 * holder is one the fetch path may refill from the
			 * source, over what the guest has since written. So
			 * the false loan is doing a second job as well -- it
			 * is the only thing marking these pages as not-to-be-
			 * refilled -- and removing it needs that job given
			 * somewhere honest to live first.
			 *
			 * The arms quoted here as "3 of 8 and 4 of 8" were
			 * literally identical: VMR_FAKE_SHARE was assigned and
			 * never read, so both runs took this same branch. That
			 * spread is this box's noise and says nothing about the
			 * honest form, which has therefore never been measured.
			 * The false loan below stays because the second job it
			 * does -- marking these pages not-to-be-refilled -- still
			 * has nowhere honest to live, and that is the thing to
			 * fix before this can be reopened.
			 */
			if (want_shared && got == 1) {
				/*
				 * Map, don't copy -- the rule the EXCLUSIVE arm
				 * below has carried since site35 was pinned, at
				 * the one install it was never applied to.
				 *
				 * The pull took the page out of the source, and
				 * the bytes in `page` are the copy that LEFT. If
				 * this address space is holding a live folio for
				 * the address -- a sibling context re-acquired it
				 * while the pull was in flight and the guest has
				 * been writing it since -- then that folio is the
				 * current page and the pulled copy is older by
				 * construction. Poking it back is a read-modify-
				 * write over live memory: every store the siblings
				 * landed in between is discarded.
				 *
				 * Measured, and it is the residual ws1 lost store
				 * named at the byte: with the live-page check armed
				 * on a failing run,
				 *
				 *   LOSTWRITE site31 0x7ffff7ff6000 +488: the
				 *   guest holds 0x40cd37 and this install writes
				 *   0x40cc91
				 *
				 * -- +488 is the return-address slot of the dying
				 * thread's own stack (ARCHITECTURE #22's slot), and
				 * the value written over it is one call older. The
				 * `ret` that follows pops the stale word and the
				 * guest re-executes start_thread's tail: the
				 * repeated `lock subl` on __nptl_nthreads, the
				 * count reaching zero early, and everything
				 * downstream of it (#21.1). The same install
				 * reverts the join-state word at +3304 from 2 to
				 * 1, which is the stale word the hang class (#25)
				 * sleeps on.
				 *
				 * serve_object_mapped() is the discriminator as
				 * well as the repair: the object's SEEK_DATA says
				 * whether the address space holds a live folio
				 * (every context maps the object MAP_SHARED, so a
				 * live sibling copy IS an object folio), and a
				 * punched hole -- nobody holds it, the pulled copy
				 * is the only one -- refuses the map, so the poke
				 * below still serves the genuine case. The loan is
				 * cleared either way: PULL_BYTES means the source
				 * gave the page up.
				 */
				if (serve_object_mapped(pid, base, !!(err & 2))) {
					page_lend(pid, base, 0);
					coh_leave();
					/* The hand-over is complete: this side is
					 * the holder (of a NEWER copy even); the
					 * wire bytes' sum is what the source
					 * stamped. */
					ack_installed(&receipt,base);
					do { plog(base, "the pull came back for a "
					     "page this address space is holding; "
					     "mapped the live folio instead of "
					     "installing the copy that left (read "
					     "fault)"); return 1; } while (0);
				}
				/*
				 * serve_object_mapped() refuses a page that is
				 * PRESENT in this context -- and that is the one
				 * way the guard above can miss: a concurrent
				 * install (a sibling's chunk fetch, another
				 * fault's poke) landed the page in THIS context
				 * while the pull was in flight. The fault is
				 * then already over, and the pulled copy is
				 * still the copy that left. Poking it would
				 * overwrite the newer install -- measured, with
				 * the first form of this guard in place:
				 *
				 *   LOSTWRITE site31 0x7ffff7ff6000 +488: the
				 *   guest holds 0x40d05b and this install
				 *   writes 0x40cc91
				 *
				 * -- so the answer is the one n_empty_averted's
				 * arm already gives for the same shape: nothing
				 * to supply, the access retries and finds the
				 * page.
				 */
				if (as_any_present(pid, base)) {
					page_lend(pid, base, 0);
					n_empty_averted++;
					coh_leave();
					ack_installed(&receipt,base);
					do { plog(base, "the pull came back for a "
					     "page a context of this address space "
					     "now maps; the copy that left is not "
					     "installed and the access retries");
					     return 1; } while (0);
				}
				/*
				 * The protection is kept and the lie is not.
				 *
				 * The write-protect is what makes the guest's
				 * next write to this page a fault this side can
				 * see, and removing it along with the false
				 * loan is what measured worse. So the page is
				 * protected exactly as before and marked
				 * *watched* -- this side protected it -- rather
				 * than recorded as a loan to a source that has
				 * just handed it over. Branch 6 acts on that
				 * mark and grants the write without asking
				 * anyone for anything.
				 */
				n_pull_not_shared++;
				poke_site = 31;	/* read fault, pull took it */
				if (poke(pid, base, page, VMR_PG_SIZE) == (long)VMR_PG_SIZE &&
				    page_protect(pid, base)) {
					page_lend(pid, base, 0);
					page_watch_set(pid, base, 1);
					mark_installed(pid, base, VMR_PG_SIZE);
					n_pages_faulted++;
					tracep(base, pid, "fault:watched", 1);
					coh_leave();
					ack_installed(&receipt,base);
					do { plog(base, "SERVED-BY site5w (read "
					     "fault, pull took the page, guest's "
					     "copy protected and watched)");
					     return 1; } while (0);
				}
				/*
				 * The protect did not come off. The page is
				 * still installed outright and the guest still
				 * owns it -- but the mark goes on anyway, and
				 * that distinction is the whole of what the
				 * false loan was doing here.
				 *
				 * "Watched" is two statements. One is about
				 * protection -- *this side protected the page
				 * so it would see the next write* -- and that
				 * one is indeed false now. The other is about
				 * whose copy is current, and it is true either
				 * way: the pull just took this page out of the
				 * source. page_keep() asks the second question,
				 * so leaving the mark off told every fetch path
				 * it was free to refill the page from a source
				 * that no longer has it.
				 *
				 * The old code recorded page_lend_mode(...,
				 * owner, 1) unconditionally and so never had
				 * this hole; the honest form grew one by
				 * setting the mark only where the protection
				 * succeeded. Measured on th9, 8 runs per arm,
				 * one binary, factors turned on one at a time:
				 * dropping the false loan was the only change
				 * that cost anything (6 of 8 against 8 of 8),
				 * and this is where it went.
				 */
				if (poke(pid, base, page, VMR_PG_SIZE) == (long)VMR_PG_SIZE) {
					page_lend(pid, base, 0);
					page_watch_set(pid, base, 1);
					mark_installed(pid, base, VMR_PG_SIZE);
					n_pages_faulted++;
					coh_leave();
					ack_installed(&receipt,base);
					do { plog(base, "SERVED-BY site5r (read "
					     "fault, pull took the page, could "
					     "not protect)"); return 1; } while (0);
				}
			}
			if (want_shared && got == 1) {
				n_downgraded++;
				if (poke(pid, base, page, VMR_PG_SIZE) == (long)VMR_PG_SIZE &&
				    page_protect(pid, base)) {
					page_lend_mode(pid, base, owner, 1);
					mark_installed(pid, base, VMR_PG_SIZE);
					n_pages_faulted++;
					tracep(base, pid, "fault:shared", 1);
					coh_leave();
					do { plog(base, "SERVED-BY site5"); return 1; } while (0);
				}
				/*
				 * Sharing did not come off, and the shadow is
				 * still holding a copy it believes is shared.
				 * Leaving it there would let the guest — whose
				 * copy is now writable, or absent — diverge from
				 * it. Take it away properly instead: one holder
				 * is always a safe state to fall back to.
				 */
				page_recall(pid, base, owner, page);
				if (poke(pid, base, page, VMR_PG_SIZE) == (long)VMR_PG_SIZE) {
					page_lend(pid, base, 0);
					mark_installed(pid, base, VMR_PG_SIZE);
					n_pages_faulted++;
					coh_leave();
					ack_installed(&receipt,base);
					do { plog(base, "SERVED-BY site6"); return 1; } while (0);
				}
			}
			page_lend(pid, base, 0);
			if (got == 1) {
				/*
				 * ...unless this address space is holding the
				 * page RIGHT NOW.
				 *
				 * A page cannot be alive at two sites, so if the
				 * object has a live folio for this address then
				 * this side is the holder and the copy just
				 * pulled is not newer than it -- it is the copy
				 * that left. Installing it is a
				 * read-modify-write over whatever the sibling
				 * contexts have written since, which is the one
				 * thing this must never do.
				 *
				 * Measured by the live-page check on hx2:
				 * "LOSTWRITE site35 +64: the guest holds
				 * 11036618 and this install writes 10968406
				 * (68212 stores discarded)". Mapping the folio
				 * this side already has costs nothing and loses
				 * nothing; the pulled bytes are dropped, which
				 * is right, because they are the older copy.
				 */
				if (serve_object_mapped(pid, base, !!(err & 2))) {
					page_lend(pid, base, 0);
					coh_leave();
					ack_installed(&receipt,base);
					do { plog(base, "the pull came back for a "
					     "page this address space is holding; "
					     "mapped the live folio instead of "
					     "installing the copy that left");
					     return 1; } while (0);
				}
				poke_site = 35;	/* pull install */
				if (poke(pid, base, page, VMR_PG_SIZE) == (long)VMR_PG_SIZE) {
					/*
					 * Does the guest see what was just
					 * installed? Every transfer's sums
					 * match end to end, yet the guest dies
					 * reading zeros -- so read the page
					 * back out of the context and compare.
					 * A mismatch here means the install
					 * did not land where the guest reads.
					 */
					if (pglog_on) {
						static __thread char rb[VMR_PG_SIZE];
						unsigned int a = page_sum(page), b = 0;

						if (peek(pid, base, rb,
							     VMR_PG_SIZE) ==
						    (long)VMR_PG_SIZE)
							b = page_sum(rb);
						plog(base, "INSTALL sum=%08x "
						     "readback=%08x%s", a, b,
						     a == b ? "" :
						     "  <<< MISMATCH");
					}
					mark_installed(pid, base, VMR_PG_SIZE);
					n_pages_faulted++;
					tracep(base, pid, "fault:recalled", 1);
					coh_leave();
					ack_installed(&receipt,base);
					do { plog(base, "SERVED-BY site7"); return 1; } while (0);
				}
				/*
				 * The context this fault belongs to may have
				 * EXITED while the recall was in flight: a guest
				 * thread faults on the shared page, the recall
				 * goes to the source, and the thread returns and
				 * dies before the bytes come back. The poke then
				 * fails with EINVAL on a pid that is gone --
				 * measured on hx1:
				 *
				 *   could not be installed into context 68304
				 *     (poke: Invalid argument)
				 *   [stuck] cannot open /proc/68304/pagemap:
				 *     No such process
				 *
				 * That is not corruption and not unrecoverable.
				 * The page belongs to the ADDRESS SPACE, whose
				 * memory is the backing object, not to the dead
				 * context's page tables -- so put the recalled
				 * bytes in the object, where any live sibling of
				 * the same address space will read them, and
				 * carry on. A dead context will not fault again;
				 * a live sibling will, and it must find the
				 * bytes the source handed back rather than the
				 * source's now-punched hole. obj_write() finds
				 * the object by address space, so a dead task's
				 * pid still resolves it as long as the address
				 * space lives; if the object is gone too, the
				 * whole address space has exited and nobody
				 * needs the page.
				 */
				/*
				 * The poke could not reach this context. That is
				 * not a reason to lose the bytes, and it does not
				 * matter whether the context is alive: a page
				 * belongs to the ADDRESS SPACE, and the address
				 * space's memory is its object. Writing them
				 * there puts them exactly where every context of
				 * it reads from, and the one that could not take
				 * the poke will map them on its next fault.
				 *
				 * The dead-context case below this used to be the
				 * only one allowed to do that, and a LIVE context
				 * whose poke failed was declared unrecoverable and
				 * abort()ed -- "there is no semantically correct
				 * continuation", which is contradicted by the
				 * branch it sits next to. There is one, it is the
				 * same one, and the state it was refusing to
				 * handle is ordinary: a context that exists but
				 * has no mapping at the address yet, which is
				 * every freshly created thread before it first
				 * touches its own stack. POKE answers EINVAL for
				 * that, not ESRCH, so task_alive() says "alive"
				 * and the abort fired.
				 *
				 * Measured on hx2: 3 of 12 runs ended
				 * "FATAL: recall of 0x7ffff6ff3000 ... (poke:
				 * Invalid argument)" 2.8 seconds in, while the
				 * writer threads were still starting -- a crash,
				 * with no output at all, on a thread stack page.
				 * That was the whole of hx2's no-output mode.
				 */
				if (obj_write(pid, base, page, VMR_PG_SIZE) ==
				    (long)VMR_PG_SIZE) {
					if (!task_alive(pid))
						n_recall_dead_ctx++;
					else
						n_recall_via_object++;
					mark_installed(pid, base, VMR_PG_SIZE);
					tracep(base, pid, "fault:recall-via-object", 1);
					coh_leave();
					ack_installed(&receipt,base);
					do { plog(base, "the recall could not be "
					     "poked into this context; the bytes "
					     "are in the address space's object, "
					     "where its next fault will find them");
					     return 1; } while (0);
				}

				/*
				 * Neither the context nor its object would take
				 * them. THAT is the unrecoverable one: the source
				 * no longer has the page, THESE are the only
				 * current bytes, and there is nowhere in this
				 * address space to put them. Stop loudly, with
				 * everything needed to find out why.
				 */
				fprintf(stderr, "[vmremote] FATAL: recall of "
					"0x%llx came back with the only copy of "
					"its bytes and could not be installed "
					"into context %d (poke: %s). This side "
					"cannot be correct after this; aborting "
					"with a dump.\n",
					(unsigned long long)base, ctx_id(pid),
					strerror(errno));
				explain_stuck(pid, base, err);
				{
					const unsigned char *b =
						(const unsigned char *)page;
					fprintf(stderr, "[vmremote] the recalled "
						"bytes (lost): %02x %02x %02x "
						"%02x %02x %02x %02x %02x sum "
						"%08x\n", b[0], b[1], b[2],
						b[3], b[4], b[5], b[6], b[7],
						page_sum(page));
				}
				fflush(stderr);
				abort();
			}
			/*
			 * The shadow was recorded as holding this page and does
			 * not have it, so the copy that was taken out of the
			 * guest is gone: what follows serves the oracle's
			 * original, losing every write made to it since. Say so
			 * — the alternative is to carry on with memory that is
			 * quietly out of date, which is the failure mode this
			 * whole protocol exists to remove.
			 */
			/*
			 * Not lost, and not a copy that has gone missing: the
			 * source has said in so many words that it is not the
			 * holder. The record said otherwise, so the record was
			 * stale -- the loan has just been cleared above -- and
			 * what follows serves this side's own copy, which is
			 * the current one. Counted, not shouted about.
			 */
			/*
			 * The source says it handed this page to this side,
			 * and what is done about it is nothing -- deliberately.
			 *
			 * Acting on it was tried first and measured second,
			 * which is the wrong order and the measurement said so:
			 * waiting and re-asking, and then ending the context,
			 * were both built and both removed. The numbers that
			 * argued for them (tens of thousands of these per run)
			 * were manufactured by two measurement loops running on
			 * one box at once -- see the note in local-here.sh
			 * about exactly that -- and on an idle machine the
			 * answer arrives ONCE in a pg3 run of 80000 hand-overs,
			 * and a context of this address space has the page.
			 *
			 * So what is left is an instrument, and it costs one
			 * page-table read on an answer that arrives once a run.
			 * It measures the number AUDIT XI §5.1 never had: how
			 * often the source says it handed a page over and NO
			 * context of this address space has it -- the two
			 * records naming each other, with a denominator instead
			 * of an anecdote. Whatever is supplied below for one of
			 * those is not the page, and if this counter ever moves
			 * that is where the repair goes.
			 */
			if (not_holder) {
				n_notholder++;
				if (as_any_present(pid, base)) {
					n_notholder_landed++;
				} else {
					n_notholder_lost++;
					if (n_notholder_lost <= 8)
						fprintf(stderr, "[vmremote] the source "
							"says it handed 0x%llx to this "
							"side and no context of address "
							"space %d has it: both records "
							"name the other machine as "
							"holder, so whatever is supplied "
							"below is not this page (context "
							"%d, err 0x%llx)\n",
							(unsigned long long)base,
							(int)as_id(pid), ctx_id(pid),
							(unsigned long long)err);
				}
				src_absent = 1;	/* behave exactly as before */
			}
			if (src_absent) {
				n_src_absent++;
				plog(base, "the source does not hold it; this "
				     "side's own copy is the current one");
				goto after_recall;
			}
			if (n_recall_absent < 8)
				fprintf(stderr, "[vmremote] LOST: 0x%llx was "
					"lent to shadow %llu (%s, %s), which no "
					"longer has it; region=%s err=0x%llx "
					"-> %s\n", (unsigned long long)base,
					(unsigned long long)owner,
					was_shared ? "lent shared" : "lent whole",
					got == 0 ? "it answered that it does not"
						 : "its channel is gone",
					r ? "the source" : "none",
					(unsigned long long)err,
					!r ? "the local kernel will zero-fill" :
					     "the source's copy");
			if (n_recall_absent < 8)
				fprintf(stderr, "[vmremote]   bookkeeping so "
					"far: %lu loans forgotten that were "
					"never recorded, %lu lent twice, %lu "
					"stale\n", n_clear_miss, n_double_lend,
					n_stale);
			tracep(base, pid, "fault:recall-absent", got);
after_recall:
			;
		}
	}
	tracep(base, pid, r ? "fault:region-ok" : "fault:no-region", (long)err);
	if (!r) {
		/*
		 * No region here, which under this model means nothing at all.
		 *
		 * The destination keeps no map: the source performed the mmap and
		 * the address the guest holds is the source's, and this machine is
		 * never told about it. So "no region" is not a reason to decline --
		 * declining hands the fault to the local kernel, which zero-fills,
		 * and the guest then runs blank memory in place of the source's.
		 * Ask the only side that knows.
		 *
		 * EFAULT is the real answer for a wild pointer: the source has not
		 * mapped it either, so the guest has earned its segmentation fault
		 * and gets it by declining.
		 */
		uint64_t q[6] = { base, VMR_PG_SIZE, 0, 0, 0, 0 };
		static __thread char one[VMR_PG_SIZE];
		long g = -1;
		int from_pull = 0;	/* `one` came from the source (PULL_BYTES) */
		int fill_site = 8;	/* which path filled `one`, for the clobber
					 * detector: 1=source pull, 2=own object,
					 * 3=retention, 4=own object after ABSENT,
					 * 5=retained after ABSENT, 6=a fresh empty
					 * page, 7=the object gained it during the
					 * wait, 8=nothing set it. Names the site
					 * that installs a stale/backward value. */

		/*
		 * Unless this context already has it.
		 *
		 * Asking home for a page the context holds itself is not merely
		 * a wasted round trip. After a fork the object holds the
		 * parent's memory, and for an address home has no region for --
		 * a heap the guest grew, whose pages exist only where the guest
		 * ran -- the answer comes back as the oracle's copy, which is
		 * to say zeros. Those zeros then land on top of the parent's
		 * memory.
		 *
		 * That is how the child's thread pointer came to read a canary
		 * of zero against the one main pushed before the fork, and be
		 * killed for smashing a stack that was intact.
		 */
		/*
		 * Everything below supplies bytes for an address the source has
		 * no region for -- from this context's own backing object, or
		 * from the program as it was loaded. That is right for an
		 * address the guest has not reached yet and wrong for one the
		 * program gave up: a write to a freed mapping then succeeds
		 * quietly, which tests/pf1.c reports as a fault that never
		 * happened.
		 *
		 * Telling those apart needs the side that owns the program to
		 * be asked, and an attempt at that is worth recording because
		 * its *result* was fiction. The question went out as a home_call
		 * with a reserved number; vmhome's dispatch treats any large
		 * number as an operation it might implement, did not, and
		 * returned -ENOSYS -- which the caller read as "the program has
		 * no such mapping". Every fault on this path therefore looked
		 * like a violation. Acting on it took the suite from nine
		 * passing to none, and the "three disagreements a run" it
		 * reported were three trips through this code, not three
		 * divergences. The addresses that were analysed as evidence of
		 * the two address spaces differing are simply the addresses
		 * that reach here.
		 *
		 * Asked again, correctly numbered, and reported only. Acting on
		 * it comes after there is evidence the answer is worth acting
		 * on -- which means seeing the shadow answer at all, which the
		 * first attempt never did.
		 */
		/*
		 * The owner is asked before this side invents anything. It holds
		 * the coherence lock at this point and the handover must not, so
		 * the lock is dropped across the question and retaken only if the
		 * page still has to be served.
		 */
		/*
		 * ...and only a page this side actually PUT there. backing_has()
		 * tests that the object COVERS the address, not that it holds the
		 * page: a memfd reads as zeros wherever nothing was written.
		 * Measured on pg3 -- the guest's first fault on its stack page
		 * (PG 000001, t=0.000ms, before any transfer existed) took this
		 * path, read 4096 zeros from an untouched object and served them
		 * as the program's stack, with no object write to that page
		 * anywhere in the run. page_is_installed() is the record of what
		 * this side really put there, and it is cleared on hand-over, so
		 * it means "ours, populated, still here". The fork case this path
		 * exists for satisfies it; everything else must ask the source.
		 */
		plog(base, "DECIDE own-object: holder=%llu backing_has=%d installed=%d",
		     (unsigned long long)page_holder(pid, base), backing_has(pid, base),
		     page_is_installed(pid, base));
		/*
		 * Address space, not context.
		 *
		 * This gate used to require page_is_installed(), which is keyed
		 * (page|1) ^ (pid << 48) -- PER CONTEXT. Sibling guest threads
		 * are separate contexts that map the one backing object
		 * separately while sharing its folio, so a page context B
		 * installed and is storing into reads installed=0 for context A.
		 * A's fault then fell through to the source, which is NOT the
		 * owner of that page: it answered with the copy it last saw, and
		 * installing that whole page over the live folio discarded every
		 * store B had made since -- not the newest store, which is what a
		 * hand-over race loses, but thousands of them.
		 *
		 * Measured on hx2, which is what named it: "0 one-behind, 0
		 * invented, 92 other", read-backs 1130, 8206 and 9646 rounds
		 * behind out of 1.28 billion. A one-behind count of zero is the
		 * proof it was never a hand-over race but a whole stale page
		 * reinstated -- the STALE_SAME the kernel stamp reports. Dropping
		 * the per-context term takes hx2 from FAIL on every run to PASS.
		 *
		 * backing_has() is the address-space-scoped test that was wanted
		 * all along, and the comment above (written when it only checked
		 * that the object COVERED the address) no longer describes it: it
		 * asks the object's own SEEK_DATA, so it is true only where the
		 * object really holds the page. That is exactly "this side owns
		 * it", because a hand-over punches the page out of the object --
		 * so after the source takes a page, SEEK_DATA reports a hole here
		 * and this branch correctly declines to answer from a stale copy.
		 * own_object_page() already refused the per-context record for
		 * the same reason, in its own words: "that record is per context,
		 * and the case this exists for is the second context of one
		 * address space".
		 */
		/*
		 * The page is in this address space's own object and this
		 * context has not faulted it in: MAP it, do not copy it.
		 *
		 * Every context of one address space maps the backing object
		 * MAP_SHARED at offset == the guest address, so for a given
		 * address they all map ONE folio. The bytes are therefore
		 * already in the right physical page; VMCTX_CTL_MAPOBJ just runs
		 * the fault in that context so its page table points at that
		 * same folio, and mark_installed() records that this context now
		 * maps it -- which is what a later hand-over serialises against.
		 *
		 * The branch below reads the object into a buffer and pokes it
		 * back through the context. For a page the address space already
		 * holds that is a read-then-write-back of the SAME folio: it
		 * copies a page onto itself, and every store a sibling context
		 * lands between the read and the write is destroyed by it. That
		 * is hx2's lost write -- 0 one-behind, 0 invented, read-backs
		 * over 9000 rounds stale -- and creating a second physical page
		 * for one shared address is exactly what must never happen.
		 *
		 * Only for a page ABSENT in this context. A present one is a
		 * protection fault, which is the owner's to decide; mapping it
		 * again would not clear a coherence write-protect and the guest
		 * would re-fault until the watchdog ended it.
		 */
		if (!page_keep(pid, base) && serve_object_mapped(pid, base, !!(err & 2))) {
			tracep(base, pid, "fault:mapped-own-object", 1);
			clobber_served_check(pid, base, 81);
			coh_leave();
			if (pglog_on == 2 && base == pglog_page)
				page_provenance(pid, base, "after own-object MAP");
			do { plog(base, "SERVED-BY own-object MAP (the object "
			     "holds it; this context now maps the same folio, "
			     "nothing copied)"); return 1; } while (0);
		}
		/*
		 * There was a fork copy-on-write branch here and it had to go.
		 *
		 * It read the parent's page and wrote it into the child's
		 * object, which is a SECOND LIVE COPY of one page on one
		 * machine -- the thing this whole design exists to prevent. A
		 * COW page is live at exactly one site at a time, and deciding
		 * that here also meant this side reasoning about fork
		 * parentage, which is the source's fact and not the
		 * destination's: a forked child is just an address space that
		 * faults and asks.
		 *
		 * The source ran the real clone3 and its kernel holds the true
		 * relationship, so it breaks copy-on-write and answers in
		 * ordinary terms; nothing here needs to know a fork happened.
		 */
		if (!page_keep(pid, base) && backing_has(pid, base)) {
			int bf = backing_find(pid);

			if (bf >= 0 && pread(bf, one, VMR_PG_SIZE,
					     (off_t)base) == (ssize_t)VMR_PG_SIZE) {
				g = (long)VMR_PG_SIZE;
				n_from_own_backing++;
				if (!page_is_installed(pid, base))
					n_as_sibling_local++;
				fill_site = 2;	/* served this side's own object */
				tracep(base, pid, "fault:from-own-object", 1);
			}
		}
		if (g < 0) {
			/*
			 * Drop the coherence lock across the remote pull, held
			 * safe by the in-flight marker -- the read-fault twin of
			 * the write-fault path above. Holding coh_lock across
			 * this network round-trip is the deadlock pull_begin()
			 * documents: home GETs this very page out of the page
			 * server, which can only answer by taking the lock this
			 * thread holds; the 2s bound then breaks the cycle and
			 * its fallback forks the page -- two live copies, the
			 * lost write hx2 counts. With the page marked, home's GET
			 * waits for the install (pg_serve_conn's pull_wait) rather
			 * than queueing behind the lock.
			 */
			int marked = pull_begin(base);
			int cd = marked ? coh_drop_all() : 0;
			int pr;

			pr = ctx_page_pull(pid, base, one,&receipt);
			/*
			 * The copy-on-write answer's fall-back is a SECOND
			 * round trip, and it belongs inside this window with
			 * the first one.
			 *
			 * Outside it the coherence lock is held again, and a
			 * network round trip under that lock is the deadlock
			 * pull_begin() exists to avoid: home GETs this very
			 * page out of the page server, which can only answer by
			 * taking the lock this thread is holding, and the 2s
			 * bound then breaks the cycle by forking the page.
			 * Measured on mf1: the child walked glibc's thread list
			 * after the fork, read a sibling's thread-control block
			 * as an invented empty page, and died in __libc_fork on
			 * `mov 0x3d8(%r15)` with r15 = 0.
			 */
			if (pr == PULL_COWBREAK &&
			    !(cow_give_from_chain(as_id(pid), base) &&
			      own_object_page(pid, base, one))) {
				n_cow_break_asked++;
				pr = ctx_page_pull_mode(pid, base, one, 1,&receipt);
				/* Same licence as the chunk path: the nocow
				 * re-ask proved the source empty-handed, so
				 * the strict walk's refusal no longer stands
				 * between the copy and its inheritance. */
				if (pr == PULL_ABSENT &&
				    cow_give_from_chain_mode(as_id(pid), base,
							     1) &&
				    own_object_page(pid, base, one)) {
					pr = PULL_LOCAL;
					n_cow_broken_here++;
					tracep(base, pid,
					       "fault:cow-break-last-resort", 1);
				}
			} else if (pr == PULL_COWBREAK) {
				n_cow_break_asked++;
				n_cow_broken_here++;
				tracep(base, pid, "fault:cow-break", 1);
				pr = PULL_LOCAL;
			}
			if (marked) {
				coh_restore(cd);
				pull_end(base);
			}
			(void)q;
			if (pr == PULL_CLAIM) {
				/*
				 * The source is claiming this page at the same
				 * moment, and by rule it wins. Let the guest
				 * fault on it again; by then the source will be
				 * the holder and will hand it over.
				 */
				coh_leave();
				if (claim_yield_pid(pid, base))
					return 1;
				coh_enter();
				pr = PULL_ABSENT;
			}
			if (pr == PULL_NOTHOLDER) {
				/*
				 * The twin of the instrument above, on the path
				 * that supplies a page for an address the
				 * source has no region for. This is the one
				 * that writes "EMPTY 0x7ffff7ff6000 (no
				 * region)" over a guest thread's stack -- and
				 * that line is in 32 of 32 passing runs as well
				 * as in every failing one, so it is where a
				 * fabrication would show, not evidence that one
				 * killed anything. Which of the two it is, is
				 * exactly what n_notholder_lost decides.
				 */
				n_notholder++;
				if (as_any_present(pid, base)) {
					/*
					 * A sibling of this address space holds
					 * it. Read the sibling's live bytes out of
					 * the shared object and serve THOSE. If the
					 * object is momentarily a hole -- a
					 * concurrent take punched it between the
					 * as_any_present check and this read -- do
					 * NOT fall through to the zero-fill below;
					 * the page belongs to the remote, so
					 * re-fault and wait, exactly as the
					 * not-present case does. Falling to
					 * PULL_ABSENT here was the residual site8
					 * clobber: own_object_page lost the race and
					 * zeros went in over live slots.
					 */
					n_notholder_landed++;
					if (serve_object_mapped(pid, base, !!(err & 2))) {
						coh_leave();
						do { plog(base, "the source handed "
						     "it to this side; a sibling has "
						     "the folio and this context now "
						     "maps it"); return 1; } while (0);
					}
					if (own_object_page(pid, base, one)) {
						pr = PULL_LOCAL;
					} else {
						coh_leave();
						usleep(200);
						return 1;	/* in transit; never invent */
					}
				} else {
					/*
					 * The source handed this page to OUR side
					 * and no local context holds it yet: it is
					 * IN TRANSIT, not absent. Fabricating a page
					 * here is forbidden -- it lands zeros over
					 * live memory (the site8 clobber the value
					 * sentinel pinned, hwm ~18M -> install 0).
					 * So re-fault and let the hand-over finish;
					 * paired with the source recording THEIRS on
					 * a first ABSENT (own_none), a page the
					 * remote owns is found on the next ask
					 * instead of re-zeroed. A page in transition
					 * is never absence.
					 */
					n_notholder_lost++;
					if (n_notholder_lost <= 8)
						fprintf(stderr, "[vmremote] 0x%llx "
							"handed to this side and not "
							"here yet: in transit -- "
							"re-faulting, refusing to "
							"invent\n",
							(unsigned long long)base);
					coh_leave();
					usleep(200);
					return 1;	/* retry; never invent */
				}
			}
			if (pr == PULL_BYTES || pr == PULL_LOCAL) {
				g = (long)VMR_PG_SIZE;
				from_pull = pr == PULL_BYTES;
				fill_site = from_pull ? 1:2;
				map_trace("executor","fault bytes context=%d mm=%llu page=%llx origin=%s episode=%llu",
					ctx_id(pid),(unsigned long long)as_id(pid),(unsigned long long)base,
					from_pull ? "source":"local",(unsigned long long)receipt.episode);
				/* Real bytes from the source: this page belongs
				 * to the remote and is now known here. Remember
				 * it, so a future crossing never zeros it. */
				retain_put(pid, base, one);
			} else if (pr == PULL_ABSENT) {
				/*
				 * The source does not hold it, so this side's
				 * own object is the only other place its bytes
				 * can be -- and for a thread's stack, or a page
				 * a forked child inherited, that is exactly
				 * where they are. If the object has nothing
				 * either then nobody has written the address
				 * yet and the local kernel's zeros are what the
				 * program's own kernel would have given it.
				 *
				 * This is the case pg3 died on: 0x7ffff7ff6000
				 * with backing_has=1 and installed=0, asked of
				 * the source, answered with zeros, and those
				 * zeros written over the frame the guest's
				 * pthread_create had built there.
				 */
				n_src_absent++;
				/*
				 * Map it rather than copy it. See
				 * serve_object_mapped(): copying is a
				 * read-modify-write of the folio the sibling
				 * contexts of this address space are writing,
				 * and it discards every store that lands in
				 * between.
				 */
				if (serve_object_mapped(pid, base, !!(err & 2))) {
					coh_leave();
					do { plog(base, "the source does not hold "
					     "it; this address space's own folio "
					     "does, and this context now maps it");
					     return 1; } while (0);
				}
				g = (long)VMR_PG_SIZE;
				if (own_object_page(pid, base, one)) {
					fill_site = 4;
					n_src_absent_own++;
					n_from_own_backing++;
					plog(base, "the source does not hold "
					     "it; this context's own object "
					     "does");
				} else {
					/*
					 * Nobody has written this address. An
					 * empty page is the right answer for
					 * exactly one reason and this is it:
					 * the owner of the memory has said in
					 * words that it does not hold the
					 * page, and fresh anonymous memory is
					 * what the program's own kernel would
					 * have given it.
					 *
					 * Written here rather than declined.
					 * Declining unbacks the mapping this
					 * fault created and hands the address
					 * to the local kernel, which for a
					 * context ends the run with -EFAULT --
					 * measured, and it killed the guest on
					 * its second fault.
					 */
					/*
					 * One question before an empty page is
					 * written, and it is the only one that can
					 * make writing one safe: has another thread of
					 * this side put the real page there while this
					 * one was asking?
					 *
					 * Both installs happen under the coherence
					 * lock, so under it the answer cannot change
					 * -- and a pull that is still in flight will
					 * install over these zeros afterwards, which is
					 * the right way round. What must never happen
					 * is zeros written over bytes that have already
					 * landed, and this is what stops it. The
					 * measured failure without it: 0x7ffff7ff6000,
					 * a guest thread's stack, emptied twice in a
					 * run after the source had answered ABSENT
					 * about a page it had just handed to the other
					 * context of the same address space.
					 */
					coh_leave();
					pull_wait(base, 500);
					coh_enter();
					if (page_present(pid, base) == 1) {
						n_empty_averted++;
						clobber_served_check(pid, base, 82);
						plog(base, "another thread installed it "
						     "while this one was about to write "
						     "an empty page; the access retries");
						coh_leave();
						return 1;
					}
					/*
					 * Before writing zeros, ask the shared
					 * object ONE more time. A SIBLING context of
					 * this address space may have installed the
					 * real page into the backing during the wait
					 * above -- page_present(pid) cannot see that
					 * (it is a different context's PTE) but the
					 * object, shared by every sibling, can. This
					 * is the hx2 lost write pinned by the value
					 * sentinel: site8 (below) wrote a whole ZERO
					 * page over four live writer slots because the
					 * source had answered ABSENT (it had handed
					 * the page to a sibling) and this recheck was
					 * missing, so an earlier TAKEOBJ's punched
					 * object looked empty.
					 *
					 * UNCONDITIONAL now; it sat behind
					 * VMR_EMPTY_RECHECK (default OFF) and the ws1
					 * teardown class walked through the open gate:
					 * a recycled thread stack's pd page, written by
					 * pthread_create in a sibling context during
					 * this very wait, was zeroed over ("EMPTY (no
					 * region): a true first touch" in the log of
					 * every one of ~30 corrupted runs, the guest
					 * then dereferencing 0x0/0x8/0x901 at
					 * 0x437716/0x40cdc5). Map before copy: the
					 * copy is a read-modify-write of the folio the
					 * siblings are running on, and with MAPOBJ
					 * refusing holes (kernel #125) the map cannot
					 * invent what the object lacks.
					 */
					if (serve_object_mapped(pid, base, !!(err & 2))) {
						n_empty_averted++;
						plog(base, "the shared object gained "
						     "the real page during the wait; "
						     "this context now maps it");
						coh_leave();
						return 1;
					}
					if (own_object_page(pid, base, one)) {
						fill_site = 7;
						n_empty_averted++;
						n_from_own_backing++;
						plog(base, "the shared object "
						     "gained the real page "
						     "during the wait; served "
						     "it, not zeros");
						goto empty_have_bytes;
					}
					/*
					 * An ABSENT that crossed this side's own
					 * serve of the page to the source is VOID:
					 * the lend record says SHADOW, meaning the
					 * source took the page (and punched this
					 * object) after the ask left. The retained
					 * fallback below would install bytes from
					 * before that whole journey -- photographed
					 * on hx1 as BACKMOVE site5, a writer reading
					 * its own counter thousands of rounds old.
					 * Yield and re-ask: the source holds the
					 * page NOW and answers with bytes.
					 */
					if (page_lent_state(pid, base) ==
					    LENT_SHADOW) {
						n_absent_stale_crossed++;
						plog(base, "ABSENT crossed this "
						     "side's serve of the page to "
						     "the source; stale at arrival "
						     "-- re-ask, not the retained "
						     "copy");
						coh_leave();
						if (claim_yield_pid(pid, base))
							return 1;
						coh_enter();
					}
					/*
					 * Before zeroing, ask the one record the
					 * object cannot keep across a punch: has
					 * this side EVER transmitted this page? If
					 * so it belongs to the remote and has real
					 * last-known bytes -- the token has
					 * momentarily evaporated (punched here,
					 * still claimed there), and inventing zeros
					 * over it is the hx2 lost write. Serve the
					 * retained bytes, never zeros.
					 */
					if (retain_get(pid, base, one)) {
						n_empty_averted++;
						fill_site = 5;	/* retained, after ABSENT */
						trail_note(pid, base, TR_INSTALL_RETAINED,
							   pgen_get(pid, base), 5, one);
						plog(base, "token in transit: served "
						     "this page's last transmitted "
						     "bytes, not zeros");
						goto empty_have_bytes;
					}
					/*
					 * A guard was tried here -- "if any context
					 * of this address space has the page live,
					 * re-fault rather than write zeros" -- and it
					 * is not kept: its counter fired ZERO times
					 * across twelve runs in which hx2 still
					 * reported an invented page ("1 zero; stored
					 * 9104894 read 0"). as_any_present() is false
					 * at the moment the zeros go in, so whatever
					 * produces them, it is not this arm reaching a
					 * page a sibling is holding. Recorded rather
					 * than left in: a check that never fires is
					 * not a safeguard, it is dead code that reads
					 * like one.
					 */
					fill_site = 6;	/* a fresh empty page */
					n_src_absent_kernel++;
					/*
					 * THE BROAD LOST-STORE DETECTOR (the
					 * died-242 / hp2 / netsurf-heap family).
					 * About to invent zeros for a page the
					 * source says it does not hold and that
					 * has no retained copy -- which is right
					 * for a genuine first touch, and a lost
					 * store when it is not. The discriminator
					 * that needs no armed page: THIS SIDE HAS
					 * INSTALLED THIS PAGE BEFORE
					 * (page_is_installed, a historical
					 * record) AND nothing recently unmapped it
					 * (vac_recent_covers excludes glibc's
					 * legitimate arena munmap+remap, which
					 * must read back zero). Both true = a page
					 * this side served, that was taken to the
					 * source and never handed back, being
					 * re-invented as zeros over the guest's
					 * writes. O(1), always on, non-masking --
					 * the detector the notes have asked for
					 * since the died-242 hunt. A verdict, not
					 * a guess: a page nothing unmapped cannot
					 * legitimately zero.
					 */
					if (page_is_installed(pid, base) &&
					    !vac_recent_covers(base)) {
						n_lost_store_zeroed++;
						if (n_lost_store_zeroed <= 16)
							fprintf(stderr,
								"[vmremote] "
								"LOST-STORE 0x%llx "
								"ctx %d: zero-filled "
								"a page this side "
								"had installed and "
								"nothing unmapped -- "
								"the source lost it "
								"and answered ABSENT "
								"(fault 0x%llx err "
								"0x%llx)\n",
								(unsigned long long)
								base, ctx_id(pid),
								(unsigned long long)
								addr,
								(unsigned long long)
								err);
					} else if (n_src_absent_kernel <= 32)
						fprintf(stderr, "[vmremote] "
							"EMPTY 0x%llx (no "
							"region): never "
							"transmitted -- a true "
							"first touch; zeroing "
							"once\n",
							(unsigned long long)base);
					memset(one, 0, VMR_PG_SIZE);
					/* Never transmitted before; it is now (as
					 * zeros). Mark it belongs-to-remote so it
					 * is never re-invented. */
					retain_put(pid, base, one);
					trail_note(pid, base, TR_INSTALL_ZERO,
						   pgen_get(pid, base), 6, NULL);
					plog(base, "the source does not hold "
					     "it and it was never transmitted; "
					     "empty, as the program's own kernel "
					     "would give it");
empty_have_bytes:		;
				}
			} else {
				g = -1;
			}
		}

		if (g == (long)VMR_PG_SIZE) {
			struct iovec liov = { one, VMR_PG_SIZE };
			struct iovec riov = { (void *)(uintptr_t)base, VMR_PG_SIZE };

			/* Clobber detector (task 13): would this install revert a
			 * monotonic slot? Tagged by which path filled `one`, so a
			 * backward move names the stale-serving site. */
			clobber_check(pid, base, one, VMR_PG_SIZE, fill_site);
			/*
			 * Content history too: this no-region install
			 * (process_vm_writev, below) is how an ARENA page is
			 * served, and it does NOT go through poke() -- the only
			 * other place hist_check runs -- so without this the
			 * arena auto-arm observes takes but never checks the
			 * serve that would revert one. site 8 = the no-region
			 * install.
			 */
			hist_check(base, one, 8);

			/*
			 * And POKE when that will not reach it, exactly as the
			 * chunked path below does. process_vm_writev needs the
			 * page writable, and a range the source mapped read-only
			 * -- a library's text, a file mapped PROT_READ -- is
			 * mapped read-only here too, because the reply to that
			 * mmap said so. So the page arrived, could not be put
			 * anywhere, and the fault was reported as "the source
			 * has no such address" with the source's own bytes sitting
			 * in the buffer. Every byte of a privately mapped file
			 * read as zero that way.
			 */
			poke_site = 36;	/* the no-region install */
			if (execution_writev(pid, &liov, 1, &riov) ==
			    (ssize_t)VMR_PG_SIZE ||
			    poke(pid, base, one, VMR_PG_SIZE) == (long)VMR_PG_SIZE) {
				poke_site = 0;
				mark_installed(pid, base, VMR_PG_SIZE);
				n_unmapped_served++;
				trail_note(pid, base, TR_INST_NOREGION,
					   pgen_get(pid, base), (uint32_t)fill_site,
					   one);
				coh_leave();
				if (fill_site == 1)	/* the source's pull filled it */
					ack_installed(&receipt,base);
				do { plog(base, "SERVED-BY site8"); return 1; } while (0);
			}
			if (obj_write(pid, base, one, VMR_PG_SIZE) ==
			    (long)VMR_PG_SIZE) {
				n_served_via_object++;
				trail_note(pid, base, TR_INST_DECLINED,
					   pgen_get(pid, base), (uint32_t)fill_site,
					   one);
				/*
				 * Retained BEFORE the decline, because the
				 * hand-off this decline makes is not atomic: a
				 * concurrent hand-over can punch the object
				 * between this write and the guest's retry
				 * mapping it, after which both machines
				 * truthfully answer absent and the run used to
				 * die 98 on bytes that existed (netsurf,
				 * FATAL-DUMP decl-why=one-page-written-into-
				 * the-object, present-any=0). With the bytes
				 * retained, the no-invention FATAL's last
				 * resort serves the real page instead.
				 */
				retain_put(pid, base, one);
				coh_leave();
				if (fill_site == 1)	/* the source's pull filled it */
					ack_installed(&receipt,base);
				/*
				 * Declined on purpose, and the one decline
				 * that is not a fall-through: the page's real
				 * bytes are in the backing object already, and
				 * the local kernel's service is what maps them.
				 * Tagged all the same, so the kernel's reading
				 * of the contents can be attributed to it.
				 */
				fresh_object_fill = 1;
				DECLINE("one-page-written-into-the-object");
			}
		}
		/*
		 * Nothing came back, so the source has no such address and this
		 * is the program's own fault. Asked here rather than before the
		 * fetch: the fetch IS the question, and asking it first cost a
		 * round trip on every page that simply had not been fetched
		 * yet.
		 */
		coh_leave();
		if (!supply_only && !source_allows_access) return FAULT_NEEDS_OWNER;
		/*
		 * Say WHICH no. "Nothing came back" is inferred from g, and g
		 * has more than two meanings: VMR_CTXPAGE_ABSENT is "neither
		 * machine has ever had this address", but CLAIMING is "the
		 * source wants it too, ask again" and a short or failed read is
		 * "the channel did not answer" -- and both of those are
		 * TRANSIENT. Calling any of them "no such address" turns a
		 * retryable state into a permanent verdict.
		 *
		 * And the verdict is not even the one the message claims. This
		 * decline is described as "a segmentation fault", which assumes
		 * the local kernel has no mapping either. For a stack page it
		 * does have one -- the guest mapped it -- so the local kernel
		 * does not fault, it supplies an anonymous page of ZEROS, and
		 * the program reads them as its own memory. Measured: a failing
		 * round's main stack, present in a context, eight words at rsp
		 * all zero.
		 */
		if (n_declined++ < 8)
			fprintf(stderr, "[vmremote] fault 0x%llx: %s (g=%ld) -- "
				"declining, which is a segmentation fault ONLY "
				"if this side has no mapping either\n",
				(unsigned long long)base,
				g == (long)VMR_PG_SIZE ?
				  "the source sent the page and it could not be "
				  "installed" : "the source did not supply it",
				g);
		if (g == (long)VMR_PG_SIZE)
			DECLINE("page-arrived-and-could-not-be-installed");
		/*
		 * The lost-page photograph (NOTES 6.1a). This decline zero-fills
		 * a page the guest mapped, so if the page ever HELD content --
		 * a hand-over is in the ring -- this is a lost page, not a true
		 * first touch. Dump its whole hand-over history across every
		 * address space at the instant it is declared gone, capped so a
		 * genuinely-absent page (start-up) does not flood a run.
		 */
		if (n_declined <= 8) {
			fprintf(stderr, "[vmremote] LOST-PAGE 0x%llx (as %d): "
				"declined as no-such-address; present=%d "
				"backing=%d holder=%llu -- history:\n",
				(unsigned long long)base, (int)as_id(pid),
				as_any_present(pid, base), backing_has(pid, base),
				(unsigned long long)page_holder(pid, base));
			trail_dump_page(base);
		}
		DECLINE("the-source-has-no-such-address");
	}

	/* Only the faulting page has a transfer reservation. Speculative capture
	 * would require separate reservations and confirmed landings for every
	 * neighbour, including pages whose mappings are read-only today. */
	start = base;
	len = VMR_PG_SIZE;

	watch_check(pid, ctx_id(pid) == ctx_pid ? "parent fault" : "child fault");
	{
		/*
		 * The same address, over and over, is a livelock: the page is
		 * served, the guest faults on it again, and neither side is
		 * making progress. Said once per address, because the fault log
		 * itself is capped and a loop outruns the cap immediately.
		 */
		static __thread uint64_t last;
		static __thread unsigned long same;

		if (base == last) {
			if (++same == 50)
				fprintf(stderr, "[vmremote] ctx %d faults on "
					"0x%llx over and over (%lu times, err "
					"0x%llx): it is not making progress\n",
					ctx_id(pid), (unsigned long long)base, same,
					(unsigned long long)err);
		} else {
			last = base;
			same = 0;
		}
	}
	if ((ctx_id(pid) != ctx_pid || trace_all_faults) &&
	    n_child_faults < child_fault_max) {
		n_child_faults++;
		fprintf(stderr, "[vmremote] ctx %d fault 0x%llx err 0x%llx -> "
			"the source (region 0x%llx-0x%llx)\n",
			ctx_id(pid), (unsigned long long)base, (unsigned long long)err,
			(unsigned long long)r->start, (unsigned long long)r->end);
	}
	/*
	 * This context's memory at the source. One question for every address,
	 * whatever is behind it there.
	 *
	 * There used to be two: an address in the program as it was loaded was
	 * asked for by address, and an address in something the guest had mapped
	 * since was asked for as a descriptor and an offset, because this machine
	 * had rewritten that mmap and kept the file's identity to read it back
	 * with. The second is gone. A mapping is the source's, made by the
	 * source, and its pages come back from the address space it made them in
	 * -- the shadow's, which the source's own kernel fills from the real file
	 * on first touch, or the oracle's behind it. Which of those answered, and
	 * whether a file was involved at all, is not visible here and does not
	 * need to be.
	 */
	/*
	 * Unless this context already has it. A forked child begins with a copy
	 * of its parent's memory, put into its object before it was let go, and
	 * that copy is the answer -- asking the source here would overwrite the
	 * parent's stack with the program's startup image, which is how ex5's
	 * child came to return into its own ELF header.
	 *
	 * Not for a page some shadow holds: that one is genuinely elsewhere, and
	 * fetching it is the recall.
	 */
	got = -1;
	/* Whether `page` holds what the SOURCE served on the last fetch, or a
	 * fallback's bytes (own object, retained copy, zeros) put there after
	 * the source answered ABSENT/NOTHOLDER/COWBREAK. Only the former is
	 * judged against the take generation; the latter was this side's own. */
	int from_src = 0;
	if (!page_keep(pid, base) && backing_has(pid, base)) {
		/*
		 * One page, not the chunk. The rest of a chunk may be holes
		 * here, and a hole reads as zeros -- installing those would put
		 * zeros over pages that have to come from the source.
		 */
		ssize_t rd;
		int bfd = backing_find(pid);
		int hole = 1;

		start = base;
		len = 4096;
		/*
		 * Map, do not copy -- and this is the arm the rule had never
		 * been applied to, though it is the busiest one in the system.
		 *
		 * Everything below reads the object into `page' with pread and
		 * lets the install at the end of this function write that
		 * buffer back through this context's mapping. For a page THIS
		 * ADDRESS SPACE already holds, both ends of that are the same
		 * folio: the object is mapped MAP_SHARED into every context on
		 * it, so the read and the write-back are a read-modify-write of
		 * a page the sibling contexts are running on, and every store
		 * they land in between is discarded.
		 *
		 * That is hx2's defect at sites 4 and 35, and serve_object_mapped()
		 * is the repair that was made for both -- it was simply never
		 * made here, on the precheck that answers a fault BEFORE the
		 * source is asked at all. Every context of a threaded program
		 * faulting on .data or .bss comes through this branch.
		 *
		 * Measured on ws1, which is a four-thread program whose threads
		 * do nothing but exist. glibc counts them in one word,
		 * __nptl_nthreads, with `lock addl' in pthread_create and
		 * `lock subl' in start_thread -- and start_thread runs the C
		 * library's exit() if its decrement reaches zero. With the
		 * count watched in every context at every syscall
		 * (VMR_WATCHWORD), a failing run reads 2 at the clone where a
		 * passing run reads 3: an increment was discarded. Four
		 * decrements then reach zero while main is still alive, a
		 * WORKER runs exit(), takes __exit_lock and leaves through
		 * _exit without releasing it -- and main's own exit() waits on
		 * that lock for ever. The page log names the window on the very
		 * page the count lives on:
		 *
		 *   [PG] 0x4cf000 own-object supplies it (len=4096)   64.283ms
		 *   [PG] 0x4cf000 SERVED-BY site10            64.296ms
		 *
		 * -- thirteen microseconds between the pread and the install,
		 * with main doing `lock addl' on that folio throughout.
		 *
		 * backing_has() above is the same SEEK_DATA hole test the
		 * fallback below performs, and serve_object_mapped() repeats it
		 * atomically with the map under ctl_lock, which is what stops a
		 * concurrent hand-over turning "map what the address space has"
		 * into "invent a page of zeros". The pread path stays as the
		 * fallback for the one case the map refuses -- a page already
		 * present in this context -- so nothing that used to be
		 * answered stops being answered.
		 */
		if (serve_object_mapped(pid, base, !!(err & 2))) {
			fault_note_prot(base, r);
			coh_leave();
			do { plog(base, "this address space's own folio has it; "
			     "this context now maps it rather than being given "
			     "a copy of it"); return 1; } while (0);
		}
		/*
		 * Does the object actually HOLD this page? A hand-over punches
		 * it out (shmem_truncate_range in TAKEOBJ), and a hole reads
		 * back as 4096 bytes of zeros with a perfectly successful
		 * pread -- so the read alone cannot tell "the parent's copy"
		 * from "nothing is here". Serving the hole is how pg3's guest
		 * came to resume on zeros: measured, one per run, tagged
		 * SERVED-BY site10, bytes printed as 00 00 00 00 00 00 00 00.
		 *
		 * SEEK_DATA answers it directly: the offset of the next data
		 * at or after start. If that is past this page, the page is a
		 * hole and the source is the only place its bytes exist.
		 */
		if (bfd >= 0) {
			off_t d = lseek(bfd, (off_t)start, SEEK_DATA);

			hole = (d < 0 || d >= (off_t)(start + len));
		}
		rd = hole ? -1 : pread(bfd, page, len, (off_t)start);
		if (!hole && rd == (ssize_t)len) {
			got = (long)len;
			n_from_own_backing++;
		} else if (hole) {
			plog(base, "own object has a HOLE here; asking the "
			     "source rather than serving zeros");
		}
	}
	if (got < 0) {
		a[0] = start;
		a[1] = len;
		got = ctxpage_fetch(pid,a, page, sizeof(page),&receipt);
		from_src = got > 0;
		plog(base, "CTXPAGE start=0x%llx len=%llu got=%ld",
		     (unsigned long long)start, (unsigned long long)len, got);
		/*
		 * A chunk stops at the first page the source does not hold, so
		 * a short answer is a boundary and not a failure. Install what
		 * came and no more -- the tail used to be zero-filled here and
		 * installed with it, which is the same fabrication as the one
		 * this change removes, arriving one page later.
		 */
		if (got > 0 && (uint64_t)got < len) {
			uint64_t have = (uint64_t)got & ~(uint64_t)4095;

			if (base >= start + have) {
				/*
				 * The faulting page is past where the source
				 * stopped, so the chunk answers nothing this
				 * fault needed. Ask for that one page, which
				 * gets a definite answer for it.
				 */
				start = base;
				len   = 4096;
				a[0]  = start;
				a[1]  = len;
				got = ctxpage_fetch(pid,a, page, sizeof(page),&receipt);
				from_src = got > 0;
				plog(base, "CTXPAGE re-asked for the faulting "
				     "page alone -> %ld", got);
			} else {
				len = have;
			}
		}
		/*
		 * ABSENT and CLAIMING are answers about the FIRST page of the
		 * request, and for a chunk that is not the page that faulted.
		 * A chunk is aligned downwards, so its first page is usually
		 * the deepest one in the range and the least likely to have
		 * been touched -- which makes "the source does not hold it" the
		 * commonest answer a chunk can get, and the least relevant one.
		 *
		 * Ask again for the page that actually faulted and act on the
		 * answer to that. Acting on the chunk's answer instead put an
		 * empty page over the guest's initial stack -- the page holding
		 * argc, argv and envp -- and the program started with argc=0
		 * and an empty argv[0]. Measured, on the first fault of the
		 * run.
		 */
		if (got < 0 && !home_call_failed && start != base &&
		    (got == VMR_CTXPAGE_ABSENT || got == VMR_CTXPAGE_CLAIMING ||
		     got == VMR_CTXPAGE_COWBREAK)) {
			start = base;
			len   = 4096;
			a[0]  = start;
			a[1]  = len;
			got = ctxpage_fetch(pid,a, page, sizeof(page),&receipt);
			from_src = got > 0;
			plog(base, "the chunk's first page is not the source's "
			     "to give; asked for the faulting page alone -> %ld",
			     got);
		}
		/*
		 * Checked on the path that calls VMR_OP_CTXPAGE DIRECTLY, not
		 * only in ctx_page_pull() -- a positive control showed the
		 * wrapper's handler never fires, because this is the path the
		 * failure takes. An unrecognised negative answer here falls
		 * through to "home returned nothing for the chunk", declines to
		 * the local kernel, and that INVENTS the page.
		 */
		if (got == VMR_CTXPAGE_GONE && !home_call_failed) {
			ctx_stop_unservable(pid, base);
			coh_leave();
			return 1;	/* the context is dead; nothing to serve */
		}
		if (got < 0 && !home_call_failed &&
		    got == VMR_CTXPAGE_CLAIMING) {
			coh_leave();
			if (claim_yield_pid(pid, base))
				return 1;	/* fault on it again */
			coh_enter();
			got = VMR_CTXPAGE_ABSENT;
		}
		if (got < 0 && !home_call_failed &&
		    got == VMR_CTXPAGE_COWBREAK) {
			/*
			 * The same instruction as in the no-region path, on the
			 * path a chunk fetch takes. It arrives here for exactly
			 * the addresses a forked child touches that the address
			 * space it was copied from had already touched -- its
			 * text, its stack, its heap, its relocated GOT -- so
			 * leaving it out was the whole of why the child died:
			 * -18 matched none of the tests below, fell through to
			 * "home returned nothing for the chunk", and was
			 * declined to a local kernel with nothing there.
			 *
			 * One page, not the chunk: the answer is about the page
			 * that was asked for, and a copy is entitled to the page
			 * it faulted on rather than to fifteen more of the
			 * original's.
			 */
			n_cow_break_asked++;
			start = base;
			len   = 4096;
			if (cow_give_from_chain(as_id(pid), base) &&
			    own_object_page(pid, base, page)) {
				got = (long)len;
				from_src = 0;
				n_cow_broken_here++;
				tracep(base, pid, "fault:cow-break", 1);
			} else {
				/*
				 * Nothing to copy from: an address neither
				 * machine has ever written. Ask again with the
				 * refinement off and take the ordinary answer,
				 * which for a file page is the file.
				 */
				got = ctx_page_ask_nocow(pid,base, page,&receipt);
				from_src = got > 0;
				/*
				 * ...and if the source, asked plainly, has
				 * NOTHING either, its emptiness is the
				 * licence the strict walk above lacked: give
				 * the nearest READABLE ancestor page, mark
				 * or no mark (bytes-current-at-break; see
				 * cow_give_page_mode).
				 */
				if (got == VMR_CTXPAGE_ABSENT &&
				    cow_give_from_chain_mode(as_id(pid), base,
							     1) &&
				    own_object_page(pid, base, page)) {
					got = (long)len;
					from_src = 0;
					n_cow_broken_here++;
					tracep(base, pid,
					       "fault:cow-break-last-resort", 1);
				}
			}
		}
		if (got < 0 && !home_call_failed &&
		    got == VMR_CTXPAGE_NOTHOLDER) {
			/*
			 * The source gave this page to THIS side and has not
			 * been asked for it back. That is the opposite of
			 * ABSENT however alike the two look as a negative
			 * number, and vmrproto.h says so at length: ABSENT
			 * licenses a fallback on our own object or, failing
			 * that, on zeros; NOTHOLDER says the only copy in
			 * existence is the one we are already holding.
			 *
			 * The chunk path never learned the difference. It
			 * tested ABSENT and CLAIMING and let everything else
			 * fall through to "home returned nothing", which
			 * declines -- and a declined fault on a page punched
			 * out of the backing object is answered by the local
			 * kernel with fresh zeros, or with -EFAULT when there
			 * is no VMA there at all. The single-page pull path
			 * has distinguished all four answers since it was
			 * written; only the chunk fetch around a fault did not.
			 *
			 * Found by instrumenting th9. One line named it:
			 *
			 *   DECLINED-BY home-returned-nothing-for-the-chunk:
			 *   0x4d5000 (fault 0x4d5e85 err 0x4) -- the local
			 *   kernel now answers this fault, for a page this
			 *   side RECORDS AS THE GUEST'S
			 *
			 * followed by "unrecoverable guest #PF at 0x4d5e85
			 * err 0x4 (-14)". The record was right and the answer
			 * was thrown away. It happens 218-293 times in a th9
			 * run, every one of them satisfied by the object this
			 * side already had.
			 */
			n_src_notholder++;
			start = base;
			len   = 4096;
			if (serve_object_mapped(pid, base, !!(err & 2))) {
				coh_leave();
				do { plog(base, "the source says it handed this "
				     "page to us; this context now maps the "
				     "address space's own folio");
				     return 1; } while (0);
			}
			if (own_object_page(pid, base, page)) {
				got = (long)len;
				from_src = 0;
				n_from_own_backing++;
				plog(base, "the source says it handed this page "
				     "to us; this context's own object has it");
			} else {
				/*
				 * Neither machine can produce it. That is an
				 * error and belongs to the owner (PRINCIPLES
				 * §3) -- never a page of zeros, which is what
				 * the fall-through gave it.
				 */
				n_notholder_lost++;
				fprintf(stderr, "[vmremote] LOST 0x%llx: the "
					"source says it handed this page over "
					"and this context's object does not "
					"have it -- no machine has it\n",
					(unsigned long long)base);
				coh_leave();
				if (!supply_only && !source_allows_access) return FAULT_NEEDS_OWNER;
				coh_enter();
				got = 0;	/* declined below, as a real fault */
			}
		}
		if (got < 0 && !home_call_failed &&
		    got == VMR_CTXPAGE_ABSENT) {
			/*
			 * The source does not hold this page. Its bytes are
			 * either in this context's own object or nowhere, and
			 * "nowhere" is a page the program has not written yet,
			 * which the local kernel fills exactly as the program's
			 * own kernel would.
			 */
			n_src_absent++;
			start = base;
			len   = 4096;
			/* Map, do not copy -- see serve_object_mapped(). */
			if (serve_object_mapped(pid, base, !!(err & 2))) {
				coh_leave();
				do { plog(base, "the source does not hold it; "
				     "this address space's own folio does, and "
				     "this context now maps it");
				     return 1; } while (0);
			}
			got   = (long)len;
			from_src = 0;
			if (own_object_page(pid, base, page)) {
				n_src_absent_own++;
				n_from_own_backing++;
				plog(base, "the source does not hold it; this "
				     "context's own object does");
			} else {
				/*
				 * See the twin in the no-region path: an empty
				 * page is right here and nowhere else, because
				 * the owner of the memory has said it does not
				 * hold this one and nothing has ever written
				 * it. Written rather than declined -- a decline
				 * ends the context.
				 */
				/*
				 * One question before an empty page is
				 * written, and it is the only one that can
				 * make writing one safe: has another thread of
				 * this side put the real page there while this
				 * one was asking?
				 *
				 * Both installs happen under the coherence
				 * lock, so under it the answer cannot change
				 * -- and a pull that is still in flight will
				 * install over these zeros afterwards, which is
				 * the right way round. What must never happen
				 * is zeros written over bytes that have already
				 * landed, and this is what stops it. The
				 * measured failure without it: 0x7ffff7ff6000,
				 * a guest thread's stack, emptied twice in a
				 * run after the source had answered ABSENT
				 * about a page it had just handed to the other
				 * context of the same address space.
				 */
				coh_leave();
				pull_wait(base, 500);
				coh_enter();
				if (page_present(pid, base) == 1) {
					n_empty_averted++;
					plog(base, "another thread installed it "
					     "while this one was about to write "
					     "an empty page; the access retries");
					coh_leave();
					return 1;
				}
				/*
				 * The twin of the no-region path's post-wait
				 * recheck: a sibling's install lands in the
				 * OBJECT, where this context's PTE check above
				 * cannot see it. Ask the object again before
				 * anything is invented over it.
				 */
				if (serve_object_mapped(pid, base, !!(err & 2))) {
					n_empty_averted++;
					plog(base, "the shared object gained the "
					     "real page during the wait; this "
					     "context now maps it");
					coh_leave();
					return 1;
				}
				if (own_object_page(pid, base, page)) {
					n_empty_averted++;
					n_from_own_backing++;
					plog(base, "the shared object gained the "
					     "real page during the wait; served "
					     "it, not zeros");
				} else if (page_lent_state(pid, base) ==
					   LENT_SHADOW) {
					/* Same stale-ABSENT rule as the no-region
					 * arm: the lend record flipped to SHADOW
					 * after the ask left, so the answer crossed
					 * this side's own serve to the source and is
					 * void. Re-ask; the source holds it now. */
					n_absent_stale_crossed++;
					plog(base, "ABSENT crossed this side's "
					     "serve of the page to the source; "
					     "stale at arrival -- re-ask, not "
					     "the retained copy");
					coh_leave();
					if (claim_yield_pid(pid, base))
						return 1;
					coh_enter();
					if (retain_get(pid, base, page)) {
						n_empty_averted++;
						trail_note(pid, base, TR_INSTALL_RETAINED,
							   pgen_get(pid, base), 11,
							   page);
						plog(base, "token in transit (yield "
						     "exhausted): served the last "
						     "transmitted bytes, not zeros");
					} else {
						memset(page, 0, 4096);
						retain_put(pid, base, page);
					}
				} else if (retain_get(pid, base, page)) {
					/* Ever transmitted -> belongs to remote,
					 * real last-known bytes; token in transit,
					 * not gone. Serve them, never zeros. */
					n_empty_averted++;
					trail_note(pid, base, TR_INSTALL_RETAINED,
						   pgen_get(pid, base), 10, page);
					plog(base, "token in transit: served this "
					     "page's last transmitted bytes, not "
					     "zeros");
				} else {
					n_src_absent_kernel++;
					if (n_src_absent_kernel <= 32)
						fprintf(stderr, "[vmremote] EMPTY "
							"0x%llx (region 0x%llx-0x%llx): "
							"never transmitted -- a true "
							"first touch; zeroing once\n",
							(unsigned long long)base,
							(unsigned long long)r->start,
							(unsigned long long)r->end);
					memset(page, 0, 4096);
					retain_put(pid, base, page);
					/*
					 * ...and RECORD THAT THIS SIDE NOW HOLDS
					 * IT, which is the half that was missing.
					 *
					 * Supplying a page without recording it
					 * leaves this side holding memory its own
					 * records deny. The consequence is not a
					 * lost write; it is a SECOND COPY. The
					 * owner recorded PG_THEIRS when it
					 * answered ABSENT -- correctly, this side
					 * is where the page went -- so when it
					 * later needs the address it asks here;
					 * this side, having written nothing down,
					 * answers that it does not hold it; and
					 * the owner then fills the address from
					 * its own kernel. Both machines end up
					 * with a page at that address and they
					 * disagree about its contents for the
					 * rest of the run.
					 *
					 * Measured on ws1, on a new thread's
					 * stack, which is exactly where a first
					 * touch happens:
					 *
					 *   [vmhome] ABSENT 0x7ffff77f5000 ... the take
					 *            found nothing and the pagemap agrees
					 *   [vmremote] EMPTY 0x7ffff77f5000: never
					 *            transmitted -- a true first touch
					 *   [vmhome] NOT-HOLDER 0x7ffff77f5000 ...
					 *            (state set at vmhome.c:6403, here=1)
					 *
					 * -- "the record says the other machine
					 * holds it, and it is present here
					 * anyway", printed by the owner about its
					 * own address space, immediately before
					 * the guest's pthread_join spun 134300
					 * times on a tid word the two machines
					 * read differently (1 here, 0 there).
					 *
					 * Every other branch that supplies a page
					 * marks it; this one memset it and moved
					 * on. Marking it makes the record true, so
					 * the owner's next ask is answered with
					 * these bytes instead of being met with a
					 * denial it then acts on.
					 */
					mark_installed(pid, base, VMR_PG_SIZE);
					plog(base, "the source does not hold it and "
					     "it was never transmitted; empty, as "
					     "the program's own kernel would give "
					     "it -- and recorded as held here");
				}
			}
		}
	} else {
		plog(base, "own-object supplies it (len=%llu)",
		     (unsigned long long)len);
	}
	if (getenv("VMREMOTE_DEBUG")) {
		/*
		 * The bytes, not just the address. Two runs can fault the same
		 * addresses in the same order and still differ in what arrives,
		 * and an address-only trace cannot tell those apart — which is
		 * exactly the case where one run works and the next one dies.
		 */
		unsigned long h = 1469598103934665603UL;

		for (long i = 0; i < got && i < (long)sizeof(page); i++)
			h = (h ^ (unsigned char)page[i]) * 1099511628211UL;
		fprintf(stderr, "[vmremote] fault 0x%llx -> got=%ld hash=%016lx\n",
			(unsigned long long)base, got, h);
	}
	/*
	 * The first bytes of what is about to be installed, so a wrong page can
	 * be told from a wrong address. It used to name the file and the offset
	 * they were read at, which this side no longer knows and no longer needs
	 * to: the address is the whole of what was asked for, so the address is
	 * the whole of what can be checked against the source.
	 */
	if (got > 16 && n_chunk_dumps < chunk_dump_max) {
		const unsigned char *b = (const unsigned char *)page;

		n_chunk_dumps++;
		fprintf(stderr, "[vmremote] served 0x%llx : "
			"%02x %02x %02x %02x %02x %02x %02x %02x\n",
			(unsigned long long)start,
			b[0], b[1], b[2], b[3], b[4], b[5], b[6], b[7]);
	}
	if (got <= 0) {
		/*
		 * Ask again before believing it. "Home returned nothing for the
		 * chunk" is not a verdict about the address, it is a report
		 * about one moment: the source can be mid-take, mid-claim, or
		 * simply busy, and every one of those answers differently a
		 * millisecond later. Treating the first no as final is what put
		 * a page of zeros over the guest's live stack.
		 *
		 * Bounded, and short. Each attempt is a LAN round trip of about
		 * half a millisecond, so eight of them is a few milliseconds --
		 * well inside the fault deadline that would otherwise kill this
		 * context anyway, and the alternative to spending them is
		 * corrupting the program.
		 *
		 * The coherence lock is NOT held here (coh_leave() ran above),
		 * which is what makes a round trip safe at this point; do not
		 * move this above that call.
		 */
		int tries;

		for (tries = 0; tries < 8 && got <= 0; tries++) {
			usleep(500);
			a[0] = start;
			a[1] = len;
			got = ctxpage_fetch(pid,a, page, sizeof(page),&receipt);
			from_src = got > 0;
			n_chunk_retry++;
		}
		if (got > 0) {
			n_chunk_retry_ok++;
			fprintf(stderr, "[vmremote] fault 0x%llx: home had "
				"nothing for the chunk and then did, after %d "
				"ask(s) -- the first no was a moment, not an "
				"address\n", (unsigned long long)base, tries);
		} else {
			fprintf(stderr, "[vmremote] fault 0x%llx: home returned "
				"%ld for 0x%llx+0x%llx (region 0x%llx-0x%llx) "
				"after %d asks\n",
				(unsigned long long)base, got,
				(unsigned long long)start, (unsigned long long)len,
				(unsigned long long)r->start,
				(unsigned long long)r->end, tries);
			coh_leave();
			DECLINE("home-returned-nothing-for-the-chunk");
		}
	}
	if ((uint64_t)got < len)
		memset(page + got, 0, len - got);
	/*
	 * The generation check, page by page, on the chunk path -- the
	 * region-arm twin of ctx_page_pull_mode()'s. Only for bytes the SOURCE
	 * served (from_src): a fallback's bytes -- own object, retained copy,
	 * zeros after an ABSENT -- are this side's own and carry no stamp to
	 * judge. (The first build judged them too and read every retained-copy
	 * install after an INTRANSIT heal as a stale serve of "gen 0".)
	 */
	if (got > 0 && from_src) {
		uint64_t o;

		for (o = 0; o + 4096 <= (uint64_t)got; o += 4096) {
			unsigned i = (unsigned)(o >> 12);

			pull_gen_check(pid, start + o, page + o,
				       i < receipt.count ? receipt.generation[i] : 0,
				       len == 4096 ? 33 : 34);
		}
	}

	/*
	 * Install the whole chunk in one call. The kernel's POKE moves a page at
	 * a time, so a 64 KiB chunk cost sixteen crossings plus sixteen address
	 * space walks; process_vm_writev takes the lot at once, and the guest's
	 * regions are mapped writable so no forced write is needed. Pages
	 * already installed are skipped by splitting the chunk into runs.
	 */
	{
		struct iovec liov[FAULT_CHUNK / 4096], riov[FAULT_CHUNK / 4096];
		int nio = 0, base_present = 0;
		uint64_t o;

		for (o = 0; o < len; o += 4096) {
			/*
			 * A page a shadow is holding must not be filled in from
			 * home's oracle. It is absent here because it was taken,
			 * not because it was never fetched, and the oracle's
			 * copy is the state it had before the syscall side ever
			 * touched it — installing that would undo every write
			 * made since and leave no trace of having done so. The
			 * guest will fault on it and recall it, one page at a
			 * time; only the page that faulted is worth a round trip
			 * now.
			 */
			if (page_keep(pid, start + o)) {
				tracep(start + o, pid, "install:held-elsewhere", 1);
				if (start + o == base)
					trail_note(pid, base, TR_INST_HELD,
						   pgen_get(pid, base),
						   (uint32_t)page_holder(pid, base),
						   NULL);
				continue;
			}
			/*
			 * A page this context's own object already holds is the
			 * context's memory, whether or not it has been touched
			 * yet. After a fork the object holds the parent's memory
			 * -- put there before the child existed -- and none of
			 * it is "installed" in the page-table sense until the
			 * child touches it.
			 *
			 * The check above the loop covers only the page that
			 * faulted. This is the rest of the chunk, and skipping
			 * it is not an optimisation: a chunk is filled from
			 * home, home has no copy of a heap the guest grew here,
			 * and the zeros that came back were written straight
			 * over the parent's memory. The child's thread pointer
			 * was in one of those pages, so it read a canary of
			 * zero against the one main had pushed before the fork,
			 * and glibc killed it for smashing a stack that was
			 * intact.
			 */
			if (backing_has(pid, start + o)) {
				int bf = backing_find(pid);

				/*
				 * Filled from the object rather than skipped:
				 * the page still has to be installed, or the
				 * guest faults on it again and again with
				 * nobody ever putting anything there. Only
				 * *what* is written changes -- its own memory
				 * instead of home's answer.
				 */
				/*
				 * Only what this side actually PUT there.
				 *
				 * backing_has() says the object covers the
				 * address, not that it holds the page, and an
				 * untouched memfd reads as zeros -- a perfectly
				 * successful 4096-byte read of nothing. This
				 * is where pg3 died: the guest's very first
				 * fault on its stack page took this path at
				 * t=0.000ms, before any transfer existed, and
				 * the zeros it read were installed and served
				 * as the program's stack. There was no object
				 * write to that page anywhere in the run.
				 *
				 * page_is_installed() is the record of what
				 * this side wrote and still holds (cleared on
				 * hand-over by mark_handed_over), so it is the
				 * question that was meant here. The fork case
				 * this path exists for -- a child reading the
				 * copy of its parent's stack placed in its
				 * object before it was let go -- satisfies it.
				 * Anything else must come from the source.
				 */
				if (bf >= 0 && page_is_installed(pid, start + o) &&
				    pread(bf, page + o, 4096,
					  (off_t)(start + o)) == 4096)
					tracep(start + o, pid,
					       "install:from-own-object", 1);
			}
			if (page_is_installed(pid, start + o)) {
				int st = page_present(pid, start + o);

				if (st == PAGE_PRESENT) {
					tracep(start + o, pid, "install:skipped-present", 1);
					if (start + o == base) {
						base_present = 1;
						trail_note(pid, base, TR_INST_PRESENT,
							   pgen_get(pid, base), 0,
							   NULL);
					}
					continue;
				}
				/*
				 * Refilling means writing over a page this
				 * context has already been given, so only a
				 * definite "absent" may authorise it. The
				 * guest owns whatever is there now: the loader
				 * adjusts a library's .dynamic in place —
				 * `d_ptr += l_addr`, in the writable segment we
				 * served — and refilling that page from the
				 * file puts the link-time value back. ld.so
				 * then relocates against a base of zero and
				 * dies dereferencing an address that was never
				 * meant to be one.
				 *
				 * "Cannot tell" used to be enough, which made
				 * every unanswerable pagemap read a chance to
				 * silently undo the guest's own writes. The
				 * page that faulted is the one exception, and
				 * only because the fault is proof it is absent.
				 */
				/*
				 * PAGE_UNKNOWN is -1, and -1 is truthy: read as
				 * a boolean it says "the guest has this page",
				 * which is the opposite of what it means. The
				 * faulting page is where that costs everything
				 * -- the fault is proof the guest could NOT
				 * read it, so "cannot tell" can never justify
				 * declining to fill it.
				 *
				 * Measured on pg3: a sibling context exits,
				 * this side asks its pagemap and gets ESRCH,
				 * the answer becomes UNKNOWN, this test read it
				 * as "not mine", the fault was answered with no
				 * data, and the guest resumed on the zeros
				 * vmctx_back_fault() had mapped from a punched
				 * object hole -- returning from a successful
				 * read(2) through a zeroed return address to
				 * rip 0. AUDIT A7 meeting AUDIT E3.
				 *
				 * The `start + o != base` term already said
				 * exactly this and its comment argued it; it
				 * only ever guarded the second clause.
				 */
				if (st == PAGE_UNKNOWN && start + o == base)
					n_refilled++;
				else if (st != PAGE_ABSENT && start + o != base) {
					tracep(start + o, pid, "install:not-mine", 1);
					continue;
				}
				n_refilled++;
			}
			tracep(start + o, pid, "install:writing", (long)got);
			if (start + o == base)
				trail_note(pid, base, TR_INST_CHUNK,
					   pgen_get(pid, base), (uint32_t)from_src,
					   page + o);
			/* extend the previous run when adjacent */
			if (nio && (uint64_t)riov[nio - 1].iov_base +
				   riov[nio - 1].iov_len == start + o) {
				riov[nio - 1].iov_len += 4096;
				liov[nio - 1].iov_len += 4096;
				continue;
			}
			if (nio >= (int)(FAULT_CHUNK / 4096))
				break;
			liov[nio].iov_base = page + o;
			liov[nio].iov_len  = 4096;
			riov[nio].iov_base = (void *)(uintptr_t)(start + o);
			riov[nio].iov_len  = 4096;
			nio++;
		}
		/*
		 * The faulting page is already present in this context's address
		 * space — so the fault cannot have been taken on it. It was taken
		 * on *another* address space that happens to use the same address.
		 *
		 * The kernel-side fault hook reports faults taken while a locally
		 * executed syscall touches user memory, and a syscall may touch an
		 * mm that is not the task's. execve is the case that hangs a
		 * browser: it builds the new program's argv/envp in a fresh
		 * bprm->mm whose temporary stack begins at STACK_TOP_MAX - PAGE
		 * (0x7fffffffe000 on x86_64), which for this guest is also a live
		 * page of its own stack. Answering DONE tells the kernel the
		 * memory was supplied; it retries, faults again on the mm we never
		 * touched, and neither side ever moves. The exec'd child never
		 * runs and whoever waits for it waits forever.
		 *
		 * Declining is both correct and general: memory this context
		 * already has is not memory we owe it, so whatever faulted is the
		 * local kernel's to fill. No syscall is named here — the test is
		 * on the memory, and it holds for anything else that faults on an
		 * address space belonging to someone else.
		 */
		/*
		 * "Present" now has two causes, and only one of them means the
		 * fault is somebody else's.
		 *
		 * The module gives an address physical backing *before* reporting
		 * the fault, because a monitor cannot create backing in another
		 * address space and the bytes have to go somewhere. So a
		 * first-touch page is already present by the time it gets here,
		 * and declining it as "not this context's" leaves it a page of
		 * zeros -- which the module then unmaps again, and the guest dies
		 * on an unrecoverable fault at an address the source could have
		 * filled. That is what broke the threaded tests.
		 *
		 * A page this monitor has installed before is the real case the
		 * check was written for: memory the context already has, faulted
		 * on by some other address space that shares the address (execve
		 * builds argv in a fresh mm whose stack starts at the same place).
		 * So ask whether we have ever filled it, not merely whether it is
		 * there.
		 */
		if (base_present && !page_is_installed(pid, base))
			base_present = 0;
		if (base_present) {
			static int said;

			/*
			 * Counted, not just sampled. Four messages and then
			 * silence is what hid the failure above: a decline is
			 * a page the guest does not get, so the total is the
			 * number that matters and the first few addresses are
			 * only for orientation.
			 */
			n_declined_present++;
			if (said++ < 4)
				fprintf(stderr, "[vmremote] fault 0x%llx is not "
					"this context's: the page is already "
					"present here, so the fault was taken on "
					"another address space -> local kernel\n",
					(unsigned long long)base);
			coh_leave();
			DECLINE("page-already-present-for-this-context");
		}
		if (nio) {
			ssize_t w = execution_writev(pid, liov, nio, riov);

			if (w > 0) {
				/*
				 * Record as installed only what actually
				 * landed, and only now.
				 *
				 * The test above used to be page_installed(),
				 * which inserts as a side effect of asking --
				 * so merely *considering* a page for this chunk
				 * marked it installed for ever. When the write
				 * then failed for it, and it does whenever the
				 * page has no mapping here yet, the page stayed
				 * zero and was never refetched, because every
				 * later fault on it found the record and
				 * skipped it. A guest stack came back as
				 * fourteen pages of zeros and the program died
				 * on `mov %rax,0x8(%rsi)` with rsi read out of
				 * one of them.
				 *
				 * process_vm_writev may also write less than it
				 * was given, so walk the entries against what
				 * it says it wrote rather than assuming.
				 */
				ssize_t left = w;
				int k;

				for (k = 0; k < nio && left > 0; k++) {
					uint64_t a0 = (uint64_t)(uintptr_t)riov[k].iov_base;
					size_t n = riov[k].iov_len;
					size_t done = (size_t)left < n ? (size_t)left : n;
					uint64_t q;

					for (q = 0; q + 4096 <= done; q += 4096) {
						mark_installed(pid, a0 + q, 4096);
						/*
						 * And the lost-write detector, which
						 * has never watched this path.
						 *
						 * Every armed clobber site is an
						 * install through poke() or a take.
						 * The CHUNK install goes through
						 * process_vm_writev and neither, so
						 * for a page this side fills in bulk
						 * -- which is most first touches, and
						 * every page ws1 uses -- the detector
						 * has no mark and never compares
						 * anything. Armed on ws1's slot page
						 * it reported "installs compared
						 * against the guest's LIVE page: 0":
						 * a zero with no denominator under
						 * it, which is the failure mode this
						 * project keeps re-learning.
						 *
						 * Site 71, beside site 70 (the take),
						 * because this is the other half of
						 * the same question: what a hand-over
						 * put back.
						 */
						clobber_check(pid, a0 + q,
							      (const char *)
							      liov[k].iov_base +
							      q, 4096, 71);
						hist_check(a0 + q,
							   (const char *)
							   liov[k].iov_base + q,
							   71);
					}
					left -= (ssize_t)done;
				}
				/*
				 * A SHORT write is the ordinary case for a
				 * chunk: process_vm_writev stops at the first
				 * page this context has no mapping for, which
				 * is every neighbour of a first touch. The
				 * prefix is installed and marked above; the
				 * rest goes through the per-page loop below,
				 * which lands what has no mapping in the
				 * object. (This used to return here, leaving
				 * the rest unmarked and fetched again later.)
				 */
				{
					size_t want = 0;

					for (k = 0; k < nio; k++)
						want += riov[k].iov_len;
					if ((size_t)w < want)
						goto per_page;
				}
				n_pages_faulted += len / 4096;
				fault_note_prot(base, r);
				coh_leave();
				/*
				 * The two-phase hand-over's ack, which this
				 * path never sent: a single-page serve commits
				 * IN TRANSIT at the source and only the ack
					 * settles it. Confirmation precedes reservation
					 * release and native RESUME; only for what the source
				 * served, and only if the faulting page was
				 * among what landed.
				 */
				if (from_src && len == 4096 && !base_present)
					ack_installed(&receipt,base);
				do { plog(base, "SERVED-BY site10"); return 1; } while (0);
			}
			/*
			 * A chunk this context cannot receive is a chunk not
			 * worth fetching.
			 *
			 * Only the faulting page is certain to have a mapping
			 * here -- the module backs one page before reporting
			 * the fault -- so on a first touch the other fifteen
			 * have nowhere to land and both process_vm_writev and
			 * POKE refuse them. That was survivable only because
			 * they used to be recorded as installed anyway and so
			 * never asked about again, which is the bug above: they
			 * stayed zero and the guest read them. Now that they
			 * are correctly left unrecorded, each one faults later
			 * and drags another 64 KiB across the network to use
			 * 4 KiB of it, and pg1 went from finishing in seconds
			 * to not finishing inside its 45-second limit.
			 *
			 * So stop guessing that the wide fetch will work. One
			 * refusal is enough to know it will not for this
			 * context, and a page at a time is what actually lands.
			 */
			/*
			 * Not a reason to stop fetching chunks (it used to
			 * clear chunk_ok for the run): a whole-chunk write
			 * fails whenever the FIRST page in address order has
			 * no mapping here, which a chunk below the faulting
			 * page always has. The per-page loop below is the
			 * install that works.
			 */
			if (!pvw_warned) {
				pvw_warned = 1;
				fprintf(stderr, "[vmremote] process_vm_writev: %s;"
					" falling back to per-page POKE\n",
					strerror(errno));
			}
		} else {
			fault_note_prot(base, r);
			coh_leave();
			return 1;	/* everything already there */
		}
	}
per_page:
	if (len > 4096)
		n_chunk_fetches++;
	/*
	 * Which pages of the chunk this service has VERIFIED the object holds
	 * -- poked, written into it, or already present -- so the reply may
	 * make them present (VMCTX_MAP_SET_POPULATE) and the guest never
	 * faults on them. A page skipped for any other reason (held by the
	 * source or watched: the object may be a punched hole there) breaks
	 * the run, because populating a hole would mint a zero folio.
	 */
	unsigned landed = 0;

	for (uint64_t o = 0; o < len; o += 4096) {
		/*
		 * An installed page is not refilled: the guest may have written
		 * it since, and home's copy would undo that. But "installed
		 * once" is not "present now" — the guest's own
		 * madvise(MADV_DONTNEED) runs on this machine and throws pages
		 * away, and a page discarded that way must be refilled or the
		 * guest reads zeros where its code used to be. Ask the page
		 * tables rather than trusting the record.
		 */
		if (page_keep(pid, start + o))
			continue;	/* held here or watched; see above */
		if (page_is_installed(pid, start + o)) {
			int st = page_present(pid, start + o);

			/* Same rule as the chunk path above: a page already
			 * given to this context is the guest's, and only a
			 * definite "absent" justifies writing over it. */
			if (st == PAGE_PRESENT) {
				landed |= 1u << (o >> 12);
				continue;
			}
			if (st != PAGE_ABSENT && start + o != base)
				continue;
			n_refilled++;
		}
		/*
		 * A neighbour this context has no mapping for yet: it cannot
		 * be poked (there is nothing to poke into -- only the faulting
		 * page is backed when the fault is reported), so it goes into
		 * the backing object, which is where this address space's
		 * memory lives on this machine and what every context of it
		 * maps. A later fault on the page finds it there and maps it
		 * (serve_object_mapped): the round trip this chunk bought.
		 * Recorded installed, because it is -- the object holds the
		 * bytes the source handed over (or copied), and a page the
		 * source TOOK would be lost otherwise, as the source's record
		 * says it is here. The faulting page itself keeps the poke
		 * path: it is backed, and the guest is waiting on it.
		 */
		if (start + o != base && page_present(pid, start + o) != 1) {
			if (obj_write(pid, start + o, page + o, 4096) == 4096) {
				mark_installed(pid, start + o, 4096);
				n_chunk_via_object++;
				landed |= 1u << (o >> 12);
				continue;
			}
			n_chunk_lost++;
			if (n_chunk_lost <= 8)
				fprintf(stderr, "[vmremote] CHUNK page 0x%llx "
					"(faulting page 0x%llx) could not be "
					"written into the object: %s -- if the "
					"source TOOK it, it is lost\n",
					(unsigned long long)(start + o),
					(unsigned long long)base,
					strerror(errno));
			continue;
		}
		if (poke(pid, start + o, page + o, 4096) == 4096) {
			landed |= 1u << (o >> 12);
			/*
			 * Landed, so now it is installed. Recording it before
			 * the write -- which is what the test above did when it
			 * was page_installed() -- makes a page that could not
			 * be written look like one the guest already owns, and
			 * it is then never fetched again.
			 */
			mark_installed(pid, start + o, 4096);
			if (start + o == base) {
				trail_note(pid, base, TR_INST_POKE,
					   pgen_get(pid, base), (uint32_t)from_src,
					   page + o);
				if (from_src && len == 4096)
					ack_installed(&receipt,base);
			}
		} else {
			/*
			 * Neither way in. Write the object the range is mapped
			 * from instead, and decline -- see obj_write().
			 *
			 * Only for the page that faulted. A neighbour POKE
			 * refuses is one this context has no mapping for, and
			 * bytes put in the object at its address are memory
			 * nobody asked for, which backing_has() then reports as
			 * the context's own so the address is never asked about
			 * again. The faulting page is different: the fault is
			 * proof the context wants it.
			 */
			if (start + o == base &&
			    obj_write(pid, base, page + o, 4096) == 4096) {
				n_served_via_object++;
				via_object = 1;
				continue;
			}
			/*
			 * The indentation here used to say this returned only
			 * for the page that faulted; it returned for any page
			 * of the chunk, and released the coherence state only
			 * for the base one — so a failure on a neighbouring
			 * page declined the fault that was actually being
			 * served and leaked the hold on the way out. gcc had
			 * been reporting it as -Wmisleading-indentation the
			 * whole time.
			 */
			fprintf(stderr, "[vmremote] POKE FAILED at 0x%llx "
				"(faulting page 0x%llx) -> stays zero\n",
				(unsigned long long)(start + o),
				(unsigned long long)base);
			if (start + o == base) {
				coh_leave();
				DECLINE("poke-of-the-faulting-page-failed");
			}
			continue;	/* a neighbour: the fault still stands */
		}
	}
	n_pages_faulted += len / 4096;
	/*
	 * The bytes for the faulting page went into the object rather than
	 * through the address space, so nothing has mapped it yet. Declining is
	 * what maps it: the module hands the fault to this kernel, which fills
	 * the page from the very object just written. Answering DONE would send
	 * the guest back to the same instruction to fault on the same absent
	 * page for ever.
	 */
	if (via_object) {
		coh_leave();
		if(from_src)ack_installed(&receipt,base);
		/* As above: the object holds the bytes and declining maps them. */
		fresh_object_fill = 1;
		DECLINE("chunk-written-into-the-object");
	}
	/*
	 * The reply maps the verified run around the faulting page from the
	 * object in one VMA with the range's own protection, populated: the
	 * neighbours are present before the guest runs again, so their first
	 * touch is not a VMEXIT and not a round trip -- which is the whole
	 * point of fetching them. Only the contiguous run of landed pages
	 * that contains the fault; a single page keeps the per-page PROT.
	 */
	if (len > 4096 && !fault_map.op && r &&
	    (landed & (1u << ((base - start) >> 12)))) {
		unsigned bi = (unsigned)((base - start) >> 12), lo = bi, hi = bi;

		while (lo > 0 && (landed & (1u << (lo - 1))))
			lo--;
		while (hi + 1 < (unsigned)(len >> 12) && (landed & (1u << (hi + 1))))
			hi++;
		if (hi > lo) {
			fault_map.op   = VMCTX_MAP_SET_POPULATE;
			fault_map.addr = start + ((uint64_t)lo << 12);
			fault_map.len  = (uint64_t)(hi - lo + 1) << 12;
			fault_map.off  = fault_map.addr;
			fault_map.prot = (unsigned)r->prot;
			n_prot_applied += hi - lo + 1;
			n_chunk_populated += hi - lo;
		}
	}
	fault_note_prot(base, r);
	coh_leave();
	do { plog(base, "SERVED-BY site11"); return 1; } while (0);
}

/*
 * Carry the range's protection back with the page.
 *
 * The context maps the first touch of any address read-write-execute --
 * vmctx_back_fault() in the module has nothing else it could do, since this
 * machine keeps no map and finds out what exists by faulting on it. That is
 * fine as somewhere to put the bytes and wrong as a permission: it leaves the
 * program's own .text writable, its .rodata writable and its guard pages
 * readable, and because the ordinary serve-the-page path never asks
 * VMR_NR_MAYACCESS -- that question is only reached when a page cannot be
 * served at all -- nothing downstream ever notices.
 *
 * So say what the range really is, in the same reply that answers the fault.
 * The three words the context already understands are enough: this is a
 * protection change over the page just installed. A write to a read-only page
 * then arrives as a protection fault instead of succeeding, which is the path
 * that does ask the owner, and the owner now has a truthful answer to give
 * because its own mirror carries the real permissions too. Neither half works
 * without the other, which is why they went in together.
 *
 * One page, not the whole chunk: only the faulting page is certain to have a
 * mapping here, and mprotect over a range with a hole in it fails outright and
 * changes nothing.
 *
 * Only for a range whose protection this side was told. That is the program as
 * the source loaded it, whose map carried a perms column; a range the guest
 * mapped itself was given its protection when its mmap was answered, and is
 * recorded here as read-write-execute precisely so this changes nothing.
 */
static void fault_note_prot(uint64_t base, const struct region *r)
{
	if (!r)
		return;
	if (fault_map.op)		/* the reply already says something */
		return;
	if (r->prot == (PROT_READ | PROT_WRITE | PROT_EXEC))
		return;			/* nothing to correct */
	fault_map.op   = VMCTX_MAP_PROT;
	fault_map.addr = base;
	fault_map.len  = 4096;
	fault_map.prot = (unsigned)r->prot;
	n_prot_applied++;
}

/* A context that really did fault: the owner may be told, and may answer. */
static int fault_from_home(memory_target pid, uint64_t vec, uint64_t addr, uint64_t err)
{
	return fault_from_home_mode(pid, vec, addr, err, 0);
}

/* ---- the child that becomes the context (net mode) ---------------------- */
static struct vmctx_run_config *child_cfg;
static char child_layout[VMR_LAYOUT_MAX];
static uint64_t child_entry, child_stack;

static int net_child(void *arg)
{
	(void)arg;
	/*
	 * Nothing is mapped here in advance.
	 *
	 * This used to walk the layout home sent and mmap every range so a fault
	 * would land somewhere it could be written into. That was the destination
	 * holding a copy of the source's address space -- told to it at startup,
	 * and stale from the first mmap the guest made afterwards.
	 *
	 * The context now starts with nothing, and every address arrives the same
	 * way: the guest touches it, the module backs it from this context's
	 * backing object, and the monitor fills it from the source. The entry point
	 * and the first stack push fault like anything else. It is also what makes
	 * memory shareable at all -- pages come from an object two contexts can
	 * both be handed, rather than private anonymous memory nobody else can see.
	 */
	drop_monitor_fds(child_cfg->backing_fd);
	child_cfg->entry = child_entry;
	child_cfg->stack = child_stack;
	{
		/*
		 * Say how the context ended. This is the guest's own exit
		 * status — the only place it exists, since exit/exit_group are
		 * context termination in the module and never reach home's log.
		 * Discarding it left "the guest stopped" indistinguishable from
		 * "the guest quit with an error".
		 *
		 * _exit() and not exit_group(): the guest's threads are contexts
		 * of this same process and must keep running.
		 */
		long ret = syscall(__NR_vmctx_run, child_cfg);

		fprintf(stderr, "[vmremote] guest context ended: vmctx_run -> %ld"
			"%s%s (enters %llu exits %llu syscalls %llu faults "
			"%llu)\n", ret, ret < 0 ? " errno=" : "",
			ret < 0 ? strerror(errno) : "",
			(unsigned long long)child_cfg->out_enters,
			(unsigned long long)child_cfg->out_exits,
			(unsigned long long)child_cfg->out_counter,
			(unsigned long long)child_cfg->out_faults);
		fflush(stderr);
		_exit(126);
	}
}

/* ---- socket plumbing --------------------------------------------------- */
/*
 * One connection to home per context being serviced. A forked guest gets its
 * own, because its syscalls must run against its own address space at home:
 * after a fork the two diverge, and running the child's syscalls in the
 * parent's mirror would answer plausibly and wrongly.
 */
static __thread int sock = -1;
static char home_host[64];
static int  home_port;

/*
 * The counters, on the way out through a signal.
 *
 * A run that is cut short by `timeout` is exactly the run whose numbers are
 * wanted — it is the one that did not finish — and until now that was the one
 * run that printed nothing at all, because every counter is reported at the end
 * of main and SIGTERM never gets there. Measuring how long a browser start
 * takes meant knowing where the time went in the runs that ran out of it.
 *
 * fprintf from a handler is not async-signal-safe. It is used anyway and the
 * reason is narrow: this handler's next act is _exit, so the only thing at risk
 * is this report's own output, and a garbled line is a better outcome than no
 * line. Nothing else in the program depends on it.
 */
static void report_counters(const char *why);

static void on_term(int sig)
{
	report_counters(sig == SIGTERM ? "SIGTERM" : "SIGINT");
	_exit(128 + sig);
}

/*
 * The monitor's own crash, said out loud with a backtrace.
 *
 * About 2 pooled ws1 runs in 1024 end rc=139: vmremote itself SIGSEGVs ~0.7s
 * in, with EMPTY logs -- the one failure class in the tree with no evidence
 * attached at all, unchanged through every arm ever measured. The rule that
 * fixed everything else here is "instrument it, never guess", and the only
 * instrument a self-crash allows is this: the faulting address and the return
 * stack, printed on the way down. backtrace_symbols_fd is not async-signal-
 * safe; the handler's next act is _exit, so the only thing at risk is this
 * report's own output, exactly as on_term argues above.
 */
static void on_crash(int sig, siginfo_t *si, void *uctx)
{
	void *bt[32];
	int n;

	ucontext_t *uc = uctx;

	fprintf(stderr, "[vmremote] FATAL %s at address %p (tid %ld) -- the "
		"MONITOR crashed, not the guest; backtrace:\n",
		sig == SIGSEGV ? "SIGSEGV" : "SIGBUS", si->si_addr,
		(long)own_tid());
	/*
	 * The faulting frame itself, from the signal context. backtrace()
	 * from inside a handler cannot cross the signal frame without unwind
	 * tables (this binary is static, -O2, no frame pointers), so every
	 * report before this one showed two frames -- on_crash and libc's
	 * __restore_rt -- and named nothing. Measured: 2 of 256 hx2 pool runs
	 * die this way at ~0.6s, same two addresses every time, and those two
	 * addresses are the handler. RIP names the site; addr2line on the
	 * same build resolves it.
	 */
	if (uc)
		fprintf(stderr, "[vmremote]   rip=0x%llx rsp=0x%llx rax=0x%llx "
			"rdi=0x%llx rsi=0x%llx err=0x%llx\n",
			(unsigned long long)uc->uc_mcontext.gregs[REG_RIP],
			(unsigned long long)uc->uc_mcontext.gregs[REG_RSP],
			(unsigned long long)uc->uc_mcontext.gregs[REG_RAX],
			(unsigned long long)uc->uc_mcontext.gregs[REG_RDI],
			(unsigned long long)uc->uc_mcontext.gregs[REG_RSI],
			(unsigned long long)uc->uc_mcontext.gregs[REG_ERR]);
	n = backtrace(bt, 32);
	backtrace_symbols_fd(bt, n, 2);
	/*
	 * And a core, when the limit allows one (local-here.sh raises it):
	 * the first crash photographed with RIP showed rip=0 -- a return or
	 * call to address zero with ctl()'s arguments still in rdi/rsi -- which
	 * no in-handler print can attribute; only the stack can. Re-raising
	 * with the default action writes the core to core_pattern and keeps
	 * the 128+sig status the harness classifies on.
	 */
	signal(sig, SIG_DFL);
	raise(sig);
	_exit(128 + sig);
}

/*
 * The last resort BEFORE the no-invention FATAL: the retained bytes.
 *
 * The FATAL is right to refuse zeros -- but the retain table holds every page
 * this side ever transmitted, captured at its last real crossing, and for the
 * one path measured producing this death the bytes were IN HAND moments
 * before: netsurf's "one-page-written-into-the-object" decline wrote the
 * page's real bytes into the backing object and a concurrent hand-over
 * punched them out before the guest's retry mapped them; both machines then
 * truthfully answer absent and the run died 98 on bytes that existed. Serving
 * the retained copy is not invention -- they are the page's last real
 * contents, the same answer the fault path's own "token in transit" arm has
 * always given -- so the kill is kept for the case with NO retained copy,
 * which is the only one where nothing real can be said.
 */
static unsigned long n_fatal_retained;

static int fatal_retained_rescue(memory_target pid, uint64_t pg)
{
	static __thread char rb[VMR_PG_SIZE];

	if (!retain_get(pid, pg, rb))
		return 0;
	if (poke(pid, pg, rb, VMR_PG_SIZE) != (long)VMR_PG_SIZE &&
	    obj_write(pid, pg, rb, VMR_PG_SIZE) != (long)VMR_PG_SIZE)
		return 0;
	mark_installed(pid, pg, VMR_PG_SIZE);
	n_fatal_retained++;
	fprintf(stderr, "[vmremote] 0x%llx was on neither machine, but its "
		"last transmitted bytes were retained; served THOSE (real "
		"bytes, not zeros) and the fault retries rather than ending "
		"the run\n", (unsigned long long)pg);
	return 1;
}

static int home_connect(void)
{
	struct sockaddr_in a;
	int fd, one = 1;

	fd = socket(AF_INET, SOCK_STREAM, 0);
	if (fd < 0)
		return -1;
	memset(&a, 0, sizeof(a));
	a.sin_family = AF_INET;
	a.sin_port = htons(home_port);
	if (inet_pton(AF_INET, home_host, &a.sin_addr) != 1 ||
	    connect(fd, (struct sockaddr *)&a, sizeof(a)) < 0) {
		close(fd);
		return -1;
	}
	setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
	return fd;
}

/*
 * Spin before blocking on a socket read; see vmhome's read_all(), the twin.
 *
 * Both halves of every message here end in a read(2) that sleeps -- the
 * service thread for the source's answer, the page service for the source's
 * next request -- and with the kernel spinning at its own four waits those two
 * were the last wakeups in a loopback call. Adaptive per thread (one thread
 * per socket): a wait inside the budget arms the next spin, one past it
 * disarms it, so a LAN channel measures one long wait and sleeps from then on.
 * VMCTX_SOCK_SPIN_US sets the budget; 0 disables it (the control arm).
 */
static unsigned sock_spin_us = 50;
static __thread int sock_spin_armed = 1;
static unsigned long n_sock_spin_hit, n_sock_spin_miss;

static int sock_read_all(int fd, char *p, size_t n)
{
	uint64_t t0 = 0;

	if (sock_spin_us) {
		t0 = now_us();
		if (sock_spin_armed) {
			for (;;) {
				ssize_t r = recv(fd, p, n, MSG_DONTWAIT);

				if (r > 0) {
					__atomic_add_fetch(&n_sock_spin_hit, 1,
							   __ATOMIC_RELAXED);
					p += r;
					n -= (size_t)r;
					break;
				}
				if (r == 0) {
					errno = ECONNRESET;
					return -1;
				}
				if (errno != EAGAIN && errno != EWOULDBLOCK &&
				    errno != EINTR) {
					if (errno == ENOTSOCK)
						break;
					return -1;
				}
				if (now_us() - t0 > sock_spin_us) {
					__atomic_add_fetch(&n_sock_spin_miss, 1,
							   __ATOMIC_RELAXED);
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
			return -1;
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
		sock_spin_armed = now_us() - t0 <= sock_spin_us;
	return 0;
}

static int io_all(int wr, void *buf, size_t n)
{
	char *p = buf;

	if (!wr)
		return sock_read_all(sock, p, n);
	while (n) {
		ssize_t r = send(sock, p, n, MSG_NOSIGNAL);

		if (r == 0)
			return -1;
		if (r < 0) {
			if (errno == EINTR)
				continue;
			return -1;
		}
		p += r;
		n -= (size_t)r;
	}
	return 0;
}

/*
 * A request header and its payload in ONE write, which is one TCP segment.
 *
 * Sent as two writes they were two segments (TCP_NODELAY is set on every
 * channel, so the first is not held for the second), and the far side's
 * reader -- which reads the header, then the payload -- woke twice per
 * request: once for the header, blocked again for the payload, and woke a
 * second time. Measured on the source with `perf sched timehist` during a
 * forwarded-getppid storm: vmhome's connection thread ran a 7 us stint (the
 * header read) followed by a full one on every call. The payload is small
 * (a register set, 168 bytes; a page on PUT), so it fits the same segment
 * as the header, and a short write is completed with io_all as before.
 */
static int io_send2(const void *a, size_t na, const void *b, size_t nb)
{
	struct iovec iov[2] = { { (void *)a, na }, { (void *)b, nb } };
	int i = 0, cnt = nb ? 2 : 1;

	while (i < cnt) {
		struct msghdr msg = { .msg_iov = iov + i,
			.msg_iovlen = (size_t)(cnt - i) };
		ssize_t w = sendmsg(sock, &msg, MSG_NOSIGNAL);

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

/*
 * Reading a page of the guest that is not there creates it, as zeros, and the
 * guest never faults on that address again. Under VMREMOTE_TRACE_ABSENT every
 * such read is reported — page_present() only reads pagemap, so the diagnosis
 * does not cause the thing it is diagnosing.
 */
/* PEEKs that answered fewer bytes than the present page they were asked
 * for: the page left between the pagemap read and the PEEK (a take), or the
 * kernel refused to fault a hole in. Each one used to be served as a full page. */
static unsigned long n_peek_short;

static long peek_at(memory_target pid, uint64_t addr, void *buf, uint64_t len,
		    const char *who)
{
	uint64_t off;
	long got = 0;

	if (trace_absent && len) {
		uint64_t p;

		for (p = addr & ~0xfffUL; p < addr + len; p += 4096)
			if (page_present(pid, p) != PAGE_PRESENT)
				fprintf(stderr, "[absent] %s reads 0x%llx "
					"(pid=%d, range 0x%llx+%llu) - the read "
					"stops there\n",
					who, (unsigned long long)p, ctx_id(pid),
					(unsigned long long)addr,
					(unsigned long long)len);
	}

	/*
	 * Page at a time, skipping any that is not there.
	 *
	 * Reading a page of the guest that does not exist yet *creates* it, as
	 * zeros, and the guest never faults on that address again — it reads
	 * zeros for the rest of the run. Nothing here is entitled to decide
	 * what the guest's memory contains, and most of these reads are not
	 * even load-bearing: the one that cost the most was a 95-byte read of a
	 * syscall's path argument, taken purely so the log could say which file
	 * was opened. The path was a string constant in libc, sharing its page
	 * with _itoa_lower_digits, and after the read every "%d" the program
	 * printed came out as a NUL byte. A browser cannot open ":99" when the
	 * 9s are NULs.
	 *
	 * An absent page is where the read *stops*. It used to be zero-filled
	 * without advancing the count, so the buffer came back fully populated
	 * and the return value was positive, and every caller tests only for
	 * "positive" — so invented zeros were indistinguishable from the
	 * guest's own bytes. A clone3 whose struct clone_args straddled an
	 * absent page read flags = 0, which is not "unknown" but a meaningful
	 * answer: no CLONE_THREAD, no CLONE_NEW*, no CLONE_CHILD_CLEARTID. The
	 * run then continued on flags nobody in the program had ever set, and
	 * nothing said so.
	 *
	 * So the count is what was really read: a caller that only needed a
	 * prefix (a NUL-terminated path, say) still gets it, and one that
	 * needed the whole thing can see that it did not get it. A first page
	 * that cannot be read is no bytes at all, which is -1.
	 *
	 * "Cannot tell" stops the read too. PEEKing a page whose presence is
	 * unanswerable is exactly the act that materialises it as zeros for the
	 * rest of the run, which is the thing the three-way answer exists to
	 * prevent — see PAGE_UNKNOWN.
	 */
	for (off = 0; off < len; ) {
		uint64_t page = (addr + off) & ~0xfffUL;
		uint64_t upto = page + 4096;
		uint64_t n = (addr + len < upto ? addr + len : upto) - (addr + off);

		if (page_present(pid, page) == PAGE_PRESENT) {
			struct vmctx_mem one = {
				.addr = addr + off, .len = n,
				.buf = (uint64_t)(uintptr_t)buf + off
			};
			long r = ctl(pid, VMCTX_CTL_PEEK, &one);

			if (r < 0)
				return got ? got : r;
			/*
			 * A SHORT answer is where the read stops too. The page
			 * was present a moment ago (the pagemap said so) and the
			 * PEEK found less than that: a take emptied it in
			 * between, or the kernel refused to fault a hole in
			 * (vmctx_foreign_file_refuse). Counting it as n bytes
			 * read -- which this did -- served whatever the buffer
			 * held before: a stale page presented as the context's
			 * current one, with every caller testing only "positive".
			 */
			if (r < (long)n) {
				n_peek_short++;
				got += r;
				if (!got) {
					errno = ENODATA;
					return -1;
				}
				return got;
			}
			got += (long)n;
		} else {
			if (!got) {
				errno = ENODATA;
				return -1;
			}
			return got;
		}
		off += n;
	}
	return got;
}

static long peek(memory_target pid, uint64_t addr, void *buf, uint64_t len)
{
	return peek_at(pid, addr, buf, len, "peek");
}

/*
 * Put bytes into a context's memory by writing the object they are read from.
 *
 * A range the source mapped read-only is mapped read-only here, because the
 * reply to that mmap said so -- and neither way of writing another process's
 * memory can reach it. process_vm_writev needs the page writable. POKE forces
 * the write, exactly as ptrace does, and Linux refuses a forced write to a
 * *shared* mapping outright (check_vma_flags: VM_MAYSHARE and no VM_WRITE is
 * -EFAULT whatever FOLL_FORCE says) -- and every range a context has is a
 * shared mapping of its backing object, because that is how the module gives a
 * context memory at all.
 *
 * So write the object. The context reads that range from this object at this
 * offset -- the module maps VMCTX_MAP_SET from it with the address as the
 * offset, and vmctx_back_fault() does the same for a first touch -- which is
 * the same rule backing_has() and the fault path already read by. The bytes
 * land in the one page the mapping and this write share, and the program's own
 * protection is left exactly as the owner set it.
 *
 * Not for a shared range: those live in the shared object at an offset the
 * owner interned, so the address is not the offset. They never reach here --
 * shared_range_has() answers them before the region lookup -- and this stays
 * addressed by context so it cannot start.
 */
/*
 * And forget a range the program has given up.
 *
 * The object keeps whatever was put in it, and the address is the offset -- so
 * a range the guest unmaps leaves its bytes sitting at exactly the offset the
 * *next* mapping of that address will read from. backing_has() then reports
 * the page as this context's own memory and the fault is answered out of the
 * object without ever asking the source: a fresh file mapping served the
 * contents of whatever used to live at that address. Punching the hole is what
 * makes "unmapped" mean unmapped.
 *
 * It matters much more than it used to. When this machine turned a file mapping
 * into anonymous memory of its own, a re-used address was re-used by a mapping
 * this side had also invented; now the source picks the addresses, it re-uses
 * them promptly, and the first thing it re-used one for was a library.
 */
static void obj_forget(memory_target pid, uint64_t addr, uint64_t len)
{
	int fd = backing_find(pid);

	if (!len) return;
	int result = -1;
	if (addr > INT64_MAX || len > INT64_MAX - addr) errno = EOVERFLOW;
	else if (fd >= 0) do {
		result = fallocate(fd, FALLOC_FL_PUNCH_HOLE | FALLOC_FL_KEEP_SIZE,
			(off_t)addr, (off_t)len);
	} while (result && errno == EINTR);
	if (result) {
		fprintf(stderr, "[vmremote] cannot discard 0x%llx+0x%llx from "
			"MM %llu: %s; stopping before stale bytes can be reused\n",
			(unsigned long long)addr, (unsigned long long)len,
			(unsigned long long)as_id(pid), strerror(errno));
		abort();
	}
}

static long obj_write(memory_target pid, uint64_t addr, const void *buf, uint64_t len)
{
	int fd = backing_find(pid);

	if (fd < 0)
		return -1;
	if (traced_page && (addr & ~0xfffUL) == traced_page) {
		unsigned char b[8] = {0};
		if (len) memcpy(b, buf, len < sizeof(b) ? (size_t)len : sizeof(b));

		fprintf(stderr, "[vmremote] OBJWRITE2 fd=%d 0x%llx+%llu "
			"first8=%02x%02x%02x%02x%02x%02x%02x%02x\n", fd,
			(unsigned long long)addr, (unsigned long long)len,
			b[0], b[1], b[2], b[3], b[4], b[5], b[6], b[7]);
	}
	clobber_check(pid, addr, buf, len, 91);
	return pwrite(fd, buf, (size_t)len, (off_t)addr);
}

/*
 * poke() had one tag for every call site, so "site90" named the operation and
 * not the decision behind it -- and the decision is the whole question when
 * what is being asked is which path installed a page over a live one.
 */
static long poke(memory_target pid, uint64_t addr, const void *buf, uint64_t len)
{
	struct vmctx_mem m = { .addr = addr, .len = len,
			       .buf = (uint64_t)(uintptr_t)buf };
	uint64_t a;

	for (a = addr & ~0xfffUL; a < addr + len; a += 4096)
		tracep(a, pid, "POKE writes over it", (long)len);
	clobber_check(pid, addr, buf, len, poke_site ? poke_site : 90);
	if (len == VMR_PG_SIZE)
		hist_check(addr & ~0xfffUL, buf, poke_site ? poke_site : 90);
	return ctl(pid, VMCTX_CTL_POKE, &m);
}

/*
 * The payload of the forwarded syscall this thread is servicing. Per thread,
 * because one thread services one context and several contexts forward writes
 * at the same time: shared, one context's bytes land in another's message. On a
 * stream socket that is not a lost write, it is a desynchronised protocol — the
 * X server read a corrupted request, took the next four bytes as a length, and
 * killed the connection with "fatal IO error 90 (Message too long)" five
 * requests in. A browser cannot draw anything after that.
 */
static __thread char databuf[256 * 1024];

/*
 * With the shadow, the rule is simply: this machine owns the guest's *memory*,
 * the other machine owns everything else. Anything that acts on the address
 * space or on the task itself must run here; every other syscall is sent as-is
 * — number and register arguments, nothing more. The shadow resolves pointer
 * arguments by faulting the pages in from us, so there is nothing to marshal
 * and no file descriptor namespace to maintain.
 */

/*
 * Nothing is served here, and there is no predicate for it.
 *
 * The destination owns nothing a program can name -- no pid, no descriptor, no
 * mapping, no credential, no namespace, no parentage, no signal state -- and it
 * must be able to stop serving at any instant, hand back every context it
 * holds, and leave the source carrying on with only its local copy. A call
 * answered here would leave state behind that the source does not have, so
 * there is nowhere for an exception to hide.
 *
 * The list used to have twenty-odd entries. Every one that was examined turned
 * out to be a defect rather than an optimisation: pids naming a machine the
 * program had never heard of; a forked child served the oracle's startup image
 * instead of its parent's memory, dying on its first dereference with a stack
 * of zeros; and no authoritative record of the guest's own mappings anywhere,
 * because mmap was answered here, so a child's address space had to be guessed
 * from this machine's /proc.
 *
 * It was then kept as a function returning 0, which is worse than nothing: dead
 * code that reads as a live decision, and a syscall log that labelled every
 * call "local" or "home" from an answer that was always "home".
 *
 * Adding an exception is not a decision to take in this file. Put the case to
 * the project owner and argue it explicitly first. "It is faster" and "it does
 * not work otherwise" are not arguments -- they describe machinery missing on
 * the source side.
 */

/*
 * What a memory syscall the source has just performed did to the address space,
 * in the terms a reply can carry.
 *
 * The numbers are the guest's own syscall numbers; there is no table of
 * behaviour here, only the three shapes an address-space change can take. A
 * call not listed leaves map_op at VMCTX_MAP_NONE and changes nothing.
 */
static void cow_break_copies_range(memory_target ctx, uint64_t start, uint64_t len);

/*
 * A range the source's kernel saw VACATED in the adopted mm (rs->vacate[],
 * fed by the mmu-notifier hook there). Give it the whole munmap treatment,
 * for every context of the address space: forked copies are given what they
 * are still owed FIRST (the punch below is where the bytes cease to exist),
 * then the object and the retained copies are punched so no later mapping at
 * these addresses can be resurrected from dead bytes, the layout record
 * forgets the range, and every member's mapping of it is removed.
 *
 * This replaces the per-syscall decode that used to live in reply_note_map's
 * munmap and mremap cases -- and covers what that decode structurally could
 * not: brk-shrink, MADV_DONTNEED/FREE, an mmap MAP_FIXED over live memory's
 * implicit unmap, io_uring, and every future path that touches the mm without
 * a case in a switch. The constructive direction (where a NEW mapping's bytes
 * come from) stays in reply_note_map: missing constructive information
 * degrades to a fault that asks; missing destructive information is a ghost.
 */
static void range_vacated_1(memory_target pid, uint64_t start, uint64_t end, int soft)
{
	struct vmctx_reply tr;
	memory_target mem[MAX_CTX];
	int nm, i;

	if (end <= start)
		return;
	coh_enter();
	vacate_late_check(start, end);
	cow_break_copies_range(pid, start, end - start);
	obj_forget(pid, start, end - start);
	retain_forget(pid, start, end);
	/*
	 * HARD vacates only. A DISCARD kills content, not relationships: the
	 * machine the loan names still holds its copy and still answers for
	 * the page, and clearing the record under it makes the next write
	 * fault grant locally against a holder that was never told -- two
	 * writers. Measured as fx1 flaking ~1/6 while this ran on soft
	 * vacates too, 0/10 without it.
	 */
	if (!soft)
		lend_forget_range(as_id(pid), start, end);
	/* New memory at these addresses has no history; the source's record
	 * (pgown_forget_range) restarts at 0 on the same reply. */
	pgen_forget(pid, start, end);
	trail_note(pid, start, TR_VACATE, 0, (uint32_t)soft, NULL);
	/*
	 * A DISCARD (soft: the source MADV_DONTNEEDed the range) is a
	 * statement about CONTENT, not about the mapping: the bytes are gone
	 * -- the punch above kills this side's copies, exactly as for an
	 * unmap, or an allocator that trusts DONTNEED-means-zero reads back
	 * its old arena (tests/md1 photographed 0xaa surviving) -- but the
	 * mapping and its layout records live on, and every member keeps its
	 * VMA with the PAGES dropped (ZAP), so the next touch refaults and
	 * asks the machine that owns the program for the fresh content.
	 */
	if (!soft) {
		region_forget_range(pid, start, end);
		shared_layout_forget(as_id(pid), start, end);
	}
	memset(&tr, 0, sizeof(tr));
	tr.map_op   = soft ? VMCTX_MAP_ZAP : VMCTX_MAP_UNMAP;
	tr.map_addr = start;
	tr.map_len  = end - start;
	nm = as_members(pid, mem, MAX_CTX);
	for (i = 0; i < nm; i++) {
		if (!ctl(mem[i], VMCTX_CTL_APPLYMAP, &tr)) continue;
		int saved = errno;
		if ((saved == ESRCH || saved == EINVAL) && mem[i] != pid &&
		    ctx_wait_execution_ended(ctx_id(mem[i]))) continue;
		fprintf(stderr, "[vmremote] committed range update failed for %d: %s\n",
			ctx_id(mem[i]), strerror(saved));
		abort();
	}
	/*
	 * The first ones are named so a run can be read for WHICH ranges the
	 * hook reported -- the proof the hook covers a case is the range
	 * itself appearing here (an mremap shrink's tail, a MAP_FIXED
	 * landing), not a counter that something fired.
	 */
	if (++n_ranges_vacated <= 16)
		fprintf(stderr, "[vmremote] %s by the source's mm hook: "
			"0x%llx-0x%llx (%d member(s) %s) [#%lu]\n",
			soft ? "discarded" : "vacated",
			(unsigned long long)start, (unsigned long long)end,
			nm, soft ? "zapped" : "unmapped", n_ranges_vacated);
	vac_recent_note(start, end);
	coh_leave();
}

/* Mapping records remain live until an authoritative unmap retires them.
 * They also supply mappings to sibling execution contexts on demand. A bounded
 * history cannot serve that purpose: Firefox exceeded 1024 live records.
 *
 * layout_order_lock serializes whole vacates with publication of constructions;
 * cons_lock protects storage and lookups. Never take layout_order_lock while
 * holding cons_lock. The current stamps are sampled by the source after calls,
 * not commit sequence numbers; they still need replacement by a source journal.
 */
struct construction {
	uint64_t start, end, seq, off, order;
	uint32_t op, prot;
	uint64_t as;
};
static struct construction *cons;
static size_t cons_n, cons_capacity;
static uint64_t cons_order;
static pthread_mutex_t cons_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t layout_order_lock = PTHREAD_MUTEX_INITIALIZER;
static unsigned long n_cons_noted, n_vacate_late_skipped;

/* Called with cons_lock held. Failure must stop execution before a mapping
 * effect is acknowledged; losing live metadata is not a valid fallback. */
static size_t cons_slot_locked(void)
{
	for (size_t i = 0; i < cons_n; i++)
		if (!cons[i].seq)
			return i;
	if (cons_n == cons_capacity) {
		size_t cap = cons_capacity ? cons_capacity * 2 : 128;
		struct construction *next = NULL;

		if (cap > cons_capacity && cap <= SIZE_MAX / sizeof(*next))
			next = calloc(cap, sizeof(*next));
		if (!next) {
			fprintf(stderr, "[vmremote] cannot retain live mapping records\n");
			abort();
		}
		if (cons_n)
			memcpy(next, cons, cons_n * sizeof(*next));
		free(cons);
		cons = next;
		cons_capacity = cap;
	}
	return cons_n++;
}

/* Caller holds layout_order_lock from before changing mapping metadata. */
static void cons_note_locked(memory_target pid, uint64_t start, uint64_t end, uint64_t seq,
		      uint32_t op, uint32_t prot, uint64_t off)
{
	uint64_t as = as_id(pid);
	size_t slot;

	if (!seq || end <= start)
		return;
	pthread_mutex_lock(&cons_lock);
	for (slot = 0; slot < cons_n; slot++)
		if (cons[slot].seq && cons[slot].as == as &&
		    cons[slot].start == start && cons[slot].end == end &&
		    (cons[slot].op == VMCTX_MAP_PROT) == (op == VMCTX_MAP_PROT))
			break;
	if (slot < cons_n && cons[slot].seq > seq)
		goto out; /* an older reply cannot replace newer recorded state */
	if (slot == cons_n)
		slot = cons_slot_locked();
	if (++cons_order == 0) {
		fprintf(stderr, "[vmremote] mapping publication counter exhausted\n");
		abort();
	}
	cons[slot] = (struct construction) {
		.as = as, .start = start, .end = end, .seq = seq,
		.op = op, .prot = prot, .off = off, .order = cons_order,
	};
	n_cons_noted++;
out:
	pthread_mutex_unlock(&cons_lock);
}

static int cons_newer(const struct construction *a, const struct construction *b)
{
	return a->seq > b->seq || (a->seq == b->seq && a->order > b->order);
}

/* Return only a span on which the selected record is current. Replaying an
 * entire older reservation over a newer submapping destroys the submapping.
 * An opaque (op 0) newer construction also masks an older replayable one. */
static int cons_covering(uint64_t as, uint64_t addr, uint64_t *start,
			 uint64_t *end, uint32_t *op, uint32_t *prot,
			 uint64_t *off, uint64_t *seq)
{
	struct construction best = {0};
	uint64_t lo, hi;

	pthread_mutex_lock(&cons_lock);
	for (size_t i = 0; i < cons_n; i++) {
		if (cons[i].as == as && cons[i].seq &&
		    cons[i].start <= addr && addr < cons[i].end &&
		    (!best.seq || cons_newer(&cons[i], &best)))
			best = cons[i];
	}
	if (!best.seq || !best.op) {
		pthread_mutex_unlock(&cons_lock);
		return 0;
	}
	lo = best.start;
	hi = best.end;
	for (size_t i = 0; i < cons_n; i++) {
		if (cons[i].as != as || !cons[i].seq || !cons_newer(&cons[i], &best))
			continue;
		if (cons[i].end <= addr && cons[i].end > lo)
			lo = cons[i].end;
		if (cons[i].start > addr && cons[i].start < hi)
			hi = cons[i].start;
	}
	*start = lo;
	*end = hi;
	*op = best.op;
	*prot = best.prot;
	*off = best.off + ((best.op == VMCTX_MAP_SET ||
			   best.op == VMCTX_MAP_SET_SHARED) ? lo - best.start : 0);
	*seq = best.seq;
	pthread_mutex_unlock(&cons_lock);
	return 1;
}

/* Remove only the overlapping portion of older records. Preserve both tails
 * and the object offset of the right tail. upto_seq == 0 retires all versions.
 * Ties retain the existing sampled-stamp rule until the journal replaces it. */
static void cons_kill(uint64_t as, uint64_t start, uint64_t end, uint64_t upto_seq)
{
	if (end <= start)
		return;
	pthread_mutex_lock(&cons_lock);
	size_t n = cons_n;
	for (size_t i = 0; i < n; i++) {
		struct construction row = cons[i];
		if (row.as != as || !row.seq || row.end <= start || row.start >= end ||
		    (upto_seq && row.seq >= upto_seq))
			continue;
		if (row.start < start && row.end > end) {
			/* Allocate before modifying either tail. It may move cons. */
			size_t right = cons_slot_locked();
			cons[i].end = start;
			cons[right] = row;
			cons[right].start = end;
			if (row.op == VMCTX_MAP_SET || row.op == VMCTX_MAP_SET_SHARED)
				cons[right].off += end - row.start;
		} else if (row.start < start) {
			cons[i].end = start;
		} else if (row.end > end) {
			cons[i].start = end;
			if (row.op == VMCTX_MAP_SET || row.op == VMCTX_MAP_SET_SHARED)
				cons[i].off += end - row.start;
		} else {
			cons[i].seq = 0;
		}
	}
	pthread_mutex_unlock(&cons_lock);
}

static void range_vacated_seq(memory_target pid, uint64_t start, uint64_t end, int soft,
			      uint64_t seq)
{
	uint64_t as = as_id(pid);

	if (!seq) {		/* no order known: the old behaviour */
		if (!soft)
			cons_kill(as, start, end, 0);
		range_vacated_1(pid, start, end, soft);
		return;
	}
	while (start < end) {
		uint64_t cs = 0, ce = 0, cseq = 0;
		size_t i, n;
		int hit = 0;

		/* The lowest-placed construction newer than this vacate that
		 * overlaps what is left of it. */
		pthread_mutex_lock(&cons_lock);
		n = cons_n;
		/*
		 * INSTRUMENT: the seqs at every overlap, both verdicts. The
		 * skip count staying ZERO through runs whose punches land on
		 * live mappings says the comparison itself never sees the
		 * killer as stale -- this prints what it saw instead.
		 */
		{
			static unsigned long n_ovl;

			for (i = 0; i < n; i++)
				if (cons[i].as == as &&
				    cons[i].op != VMCTX_MAP_PROT &&
				    cons[i].end > start &&
				    cons[i].start < end &&
				    (end - start >= 0x100000 ||
				     cons[i].end - cons[i].start >= 0x100000) &&
				    ++n_ovl <= 16)
					fprintf(stderr, "[vmremote] VAC-ORDER: "
						"vacate 0x%llx-0x%llx seq=%llu "
						"vs row 0x%llx-0x%llx seq=%llu "
						"(%s)\n",
						(unsigned long long)start,
						(unsigned long long)end,
						(unsigned long long)seq,
						(unsigned long long)cons[i].start,
						(unsigned long long)cons[i].end,
						(unsigned long long)cons[i].seq,
						cons[i].seq > seq ?
						"row newer: SKIP" :
						cons[i].seq ? "row older: punch"
							    : "row dead");
		}
		for (i = 0; i < n; i++) {
			/* A TIE is a newer construction; see cons_kill. */
			if (cons[i].as != as || cons[i].seq < seq ||
			    !cons[i].seq)
				continue;
			/*
			 * A PROTECTION row exists for the repair only, never
			 * for ordering: an mprotect's stamp is the counter at
			 * ITS drain, which can equal an unrelated munmap's
			 * event seq from another thread -- and unlike an mmap,
			 * whose address proves the unmap before it completed,
			 * nothing about an mprotect orders it against that
			 * unmap. Letting PROT rows keep vacates off took
			 * netsurf loopback from 2/3 to 0/3 (live libraries
			 * never unmapped here, "no such address" from the
			 * source later).
			 */
			if (cons[i].op == VMCTX_MAP_PROT)
				continue;
			if (cons[i].end <= start || cons[i].start >= end)
				continue;
			if (!hit || cons[i].start < cs) {
				cs = cons[i].start > start ? cons[i].start : start;
				ce = cons[i].end < end ? cons[i].end : end;
				cseq = cons[i].seq;
				hit = 1;
			}
		}
		pthread_mutex_unlock(&cons_lock);
		if (!hit) {
			if (!soft)
				cons_kill(as, start, end, seq);
			range_vacated_1(pid, start, end, soft);
			return;
		}
		if (cs > start) {
			if (!soft)
				cons_kill(as, start, cs, seq);
			range_vacated_1(pid, start, cs, soft);
		}
		n_vacate_late_skipped++;
		if (n_vacate_late_skipped <= 8)
			fprintf(stderr, "[vmremote] vacate seq %llu of 0x%llx-0x%llx "
				"arrived after construction seq %llu of "
				"0x%llx-0x%llx: the construction is newer and "
				"is left alone (the rest of the range is "
				"vacated)\n", (unsigned long long)seq,
				(unsigned long long)start, (unsigned long long)end,
				(unsigned long long)cseq, (unsigned long long)cs,
				(unsigned long long)ce);
		start = ce;
	}
}

/* Apply source-described effects. No guest OS ABI enters this function. */
static int reply_note_map(memory_target pid, struct vmctx_reply *rep,
                         const struct vmr_rsp *rs)
{
	const struct vmr_map_effect *map = &rs->mapping;
	uint64_t length;
	unsigned int prot;

	rep->map_op = VMCTX_MAP_NONE;
	if (map->kind == VMR_MAP_NONE)
		return 0;
	map_trace("executor", "map-reply ctx=%d as=%llu seq=%llu kind=%u flags=%u prot=%u range=%llx+%llx off=%llx",
		ctx_id(pid), (unsigned long long)as_id(pid), (unsigned long long)rs->mmseq,
		map->kind, map->flags, map->protection, (unsigned long long)map->address,
		(unsigned long long)map->length, (unsigned long long)map->object_offset);
	if (map->reserved || map->kind > VMR_MAP_MOVE ||
	    (map->flags & ~(VMR_MAP_SHARED | VMR_MAP_REPLACE)) ||
	    (map->protection & ~(VMR_PROT_READ | VMR_PROT_WRITE | VMR_PROT_EXEC)) ||
	    !map->length || map->length > UINT64_MAX - (VMR_PG_SIZE - 1) ||
	    (map->address & (VMR_PG_SIZE - 1)) ||
	    ((map->flags & VMR_MAP_SHARED) ?
	     (!map->object_id || (map->object_offset & (VMR_PG_SIZE - 1)) ||
	      map->object_offset > INT64_MAX || map->length > INT64_MAX - map->object_offset) :
	     map->object_id != 0))
		return -1;
	length = (map->length + VMR_PG_SIZE - 1) & ~(uint64_t)(VMR_PG_SIZE - 1);
	if (length > UINT64_MAX - map->address ||
	    (map->kind == VMR_MAP_SET && length > UINT64_MAX - map->object_offset))
		return -1;
	prot = ((map->protection & VMR_PROT_READ) ? PROT_READ : 0) |
	       ((map->protection & VMR_PROT_WRITE) ? PROT_WRITE : 0) |
	       ((map->protection & VMR_PROT_EXEC) ? PROT_EXEC : 0);
	switch (map->kind) {
	case VMR_MAP_SET:
		rep->map_op = VMCTX_MAP_SET;
		rep->map_addr = map->address;
		rep->map_len = length;
		rep->map_prot = prot;
		rep->map_off = map->object_offset;
        if ((map->flags & VMR_MAP_SHARED) && shared_obj < 0) return -1;
        /* A SET publishes a new mapping incarnation. Its old UNMAP may
         * arrive later and be correctly skipped by construction ordering,
         * so publication itself must retire every old byte/loan record and
         * native alias, including for private mappings. */
        range_vacated_1(pid, map->address, map->address + length, 0);
        if (map->flags & VMR_MAP_SHARED) {
            shared_range_note(pid, map->address, length, map->object_offset,
                              prot, map->object_id);
            rep->map_op = VMCTX_MAP_NONE;
        }
		return 0;
	case VMR_MAP_PROTECT:
		shared_layout_protect(as_id(pid), map->address, map->address + length, prot);
		rep->map_op = VMCTX_MAP_PROT;
		rep->map_addr = map->address;
		rep->map_len = length;
		rep->map_prot = prot;
		return 0;
	case VMR_MAP_MOVE: {
		uint64_t olda = map->prior_address;
		uint64_t oldl, newl = length;
		if ((olda & (VMR_PG_SIZE - 1)) ||
		    map->prior_length > UINT64_MAX - (VMR_PG_SIZE - 1))
			return -1;
		oldl = (map->prior_length + VMR_PG_SIZE - 1) &
		       ~(uint64_t)(VMR_PG_SIZE - 1);
		if (oldl > UINT64_MAX - olda)
			return -1;
		if (map->address != olda) {
			static __thread char mv[VMR_PG_SIZE];
			uint64_t o;

			if (map->flags & VMR_MAP_REPLACE) {	/* MREMAP_FIXED: the target
						 * range was REPLACED. Its old
						 * bytes and mappings must not
						 * survive to be read as the
						 * moved content -- and the
						 * arriving vacate for it will
						 * be SKIPPED (it is the
						 * keep-zone), so the replaced
						 * half is this case's own job,
						 * done BEFORE the relocation
						 * lands the moved bytes. */
				struct vmctx_reply tr;
				memory_target mem[MAX_CTX];
				int nm, i;

				obj_forget(pid, map->address, newl);
				retain_forget(pid, map->address,
					      map->address + newl);
				memset(&tr, 0, sizeof(tr));
				tr.map_op   = VMCTX_MAP_UNMAP;
				tr.map_addr = map->address;
				tr.map_len  = newl;
				nm = as_members(pid, mem, MAX_CTX);
				for (i = 0; i < nm; i++)
					if (ctl(mem[i], VMCTX_CTL_APPLYMAP, &tr) < 0)
						return -1;
			}
			for (o = 0; o < oldl && o < newl; o += VMR_PG_SIZE) {
				if (!backing_has(pid, olda + o) &&
				    !as_any_present(pid, olda + o))
					continue;
				if (page_read_as(pid, olda + o, mv) != 1 ||
				    obj_write(pid, map->address + o, mv,
					      VMR_PG_SIZE) != (long)VMR_PG_SIZE)
					return -1;
				n_mremap_moved++;
			}
			region_forget_range(pid, map->address,
					    map->address + newl);
			vacate_keep.start = map->address;
			vacate_keep.end   = map->address + newl;
			/* op 0: an mremap's landing orders vacates but cannot
			 * be replayed from here (its prot is the old range's,
			 * which this side never learned). */
			cons_note_locked(pid, map->address, map->address + newl,
				  rs->mmseq, 0, 0, 0);
		}
		return 0;
	}
	default:
		return -1;
	}
}

#include "access-journal.h"

/* Forward one syscall home. Returns 1 if it was forwarded (rep filled in),
 * 0 if it should be handled by this machine's kernel. */
/* A child context being stood up: its pid here and the source's pid for it. */
struct child_arg { execution_id id; uint64_t srcpid; struct vmr_mm_binding binding; int shared; };
static pthread_mutex_t child_lifetime_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t child_lifetime_changed = PTHREAD_COND_INITIALIZER;
static unsigned n_live_children;
struct child_source_pages { uint64_t *pages; size_t count; };
static memory_target spawn_forked_context(memory_target parent, uint64_t srcpid, int backing,
				  const struct vmr_mm_binding *birth,
				  const struct child_source_pages *source_pages);
static void *child_thread(void *arg);

#include "child-pages.h"

static int child_source_pages_read(const struct vmr_mm_binding *child, struct child_source_pages *out)
{
	uint64_t cursor = 0;
	do {
		struct vmr_child_pages batch;
		uint64_t args[6] = {child->context, cursor};
		long r = home_binding_call(child, VMR_OP_CHILD_PAGES, args, NULL, 0, &batch, sizeof(batch),NULL);
		if (home_call_failed || r != sizeof(batch) || !child_pages_valid(&batch, cursor))
			return -1;
		if (batch.count) {
			if (out->count > SIZE_MAX / sizeof(uint64_t) - batch.count) return -1;
			uint64_t *p = realloc(out->pages, (out->count + batch.count) * sizeof(*p));
			if (!p) return -1;
			out->pages = p;
			memcpy(p + out->count, batch.pages, batch.count * sizeof(*p));
			out->count += batch.count;
		}
		cursor = batch.next;
	} while (cursor);
	return 0;
}

/*
 * The channel to the source broke in the middle of a guest's syscall.
 *
 * Counted and named. The caller decides what to do about it; what it must not
 * do is carry on quietly, which is what a bare `return 0` bought.
 */
unsigned long vmr_fwd_broken;

/*
 * Which syscall each context is inside, while it is inside it.
 *
 * The syscall log prints a call when it COMPLETES ("sys_202 (...) -> 0"), so a
 * call that never completes is the one call that appears nowhere -- which is
 * precisely the one worth knowing about when a run hangs. n_sys and n_fwd are
 * process-wide, so "51 of 52 forwarded" at SIGTERM does not mean a call was
 * performed locally: it means one context was still waiting in forward() for a
 * reply that never came. This says which context, which call, and for how long.
 */
static struct {
	pid_t        pid;
	unsigned long nr;
	double       since;
} fwd_inflight[MAX_CTX];
static pthread_mutex_t fwd_inflight_mx = PTHREAD_MUTEX_INITIALIZER;

static void fwd_enter(pid_t pid, unsigned long nr)
{
	int i;

	pthread_mutex_lock(&fwd_inflight_mx);
	for (i = 0; i < MAX_CTX; i++)
		if (!fwd_inflight[i].pid) {
			fwd_inflight[i].pid   = pid;
			fwd_inflight[i].nr    = nr;
			fwd_inflight[i].since = t_now();
			break;
		}
	pthread_mutex_unlock(&fwd_inflight_mx);
}

static void fwd_leave(pid_t pid)
{
	int i;

	pthread_mutex_lock(&fwd_inflight_mx);
	for (i = 0; i < MAX_CTX; i++)
		if (fwd_inflight[i].pid == pid) {
			fwd_inflight[i].pid = 0;
			break;
		}
	pthread_mutex_unlock(&fwd_inflight_mx);
}

static void fwd_report_inflight(void)
{
	int i;

	pthread_mutex_lock(&fwd_inflight_mx);
	for (i = 0; i < MAX_CTX; i++)
		if (fwd_inflight[i].pid)
			fprintf(stderr, "[vmremote] context %d is STILL WAITING "
				"for the source to answer syscall %lu, %.3fs "
				"after sending it -- this is the call that "
				"never completed\n",
				(int)fwd_inflight[i].pid, fwd_inflight[i].nr,
				t_now() - fwd_inflight[i].since);
	pthread_mutex_unlock(&fwd_inflight_mx);
}

/*
 * A syscall that could not be forwarded ends the program. Here, now, loudly.
 *
 * There is no lesser answer available. The caller's reply is initialised
 * `{ .action = VMCTX_ACT_SELF }` and is only overwritten once a reply has
 * arrived, so simply returning from here would RESUME the context with SELF
 * still in it and THIS machine's kernel would perform the guest's syscall --
 * the one thing a destination must never do (PRINCIPLES §6: the destination
 * owns nothing; it has no files, no pids, no futex queues of the program's).
 * The damage is silent and unbounded: a local futex WAIT queues on a kernel
 * no waker will ever reach, a local write goes to a descriptor that means
 * something else here, and a local mmap changes an address space the source
 * believes it alone edits. A program that has had one syscall answered by the
 * wrong machine is not a program that can be allowed to keep running, and its
 * output is not evidence of anything.
 *
 * So: every context of the program is killed, the reason is printed in a form
 * nobody can mistake for a warning, and the process exits with a code of its
 * own (VMR_EXIT_NOT_FORWARDED) so a harness can tell this apart from the
 * program's own failures.
 */
#define VMR_EXIT_NOT_FORWARDED 97
static void __attribute__((noreturn)) execution_start_failed(pid_t pid, const char *step)
{
	fprintf(stderr, "[vmremote] FATAL: context %d failed during %s: %s; ending execution tree\n",
		pid, step, strerror(errno));
	ctx_stop_all();
	ctx_signal(pid, SIGKILL);
	fflush(NULL);
	_exit(VMR_EXIT_NOT_FORWARDED);
}

static int fwd_broken(memory_target pid, const struct vmctx_event *ev, const char *step)
{
	vmr_fwd_broken++;
	fflush(stdout);
	fprintf(stderr,
"[vmremote] ============================================================\n"
"[vmremote] FATAL: A GUEST SYSCALL COULD NOT BE FORWARDED TO THE SOURCE.\n"
"[vmremote] ============================================================\n"
"[vmremote] FATAL:   context      %d\n"
"[vmremote] FATAL:   syscall      %llu\n"
"[vmremote] FATAL:   failed while %s\n"
"[vmremote] FATAL:   errno        %s\n"
"[vmremote] FATAL:\n"
"[vmremote] FATAL: This machine owns no part of this program -- not its files,\n"
"[vmremote] FATAL: not its pids, not its futex queues. Performing the call here\n"
"[vmremote] FATAL: would answer it from the wrong machine, silently and for\n"
"[vmremote] FATAL: good. The program is being killed instead. Nothing it has\n"
"[vmremote] FATAL: printed should be trusted.\n"
"[vmremote] ============================================================\n",
		ctx_id(pid), (unsigned long long)ev->nr, step, strerror(errno));
	fflush(stderr);

	/*
	 * Every context, not just this one: they share the program's address
	 * space and its exit status, and one of them has now been cut off from
	 * the machine that owns them.
	 */
	ctx_stop_all();
	ctx_signal(ctx_id(pid), SIGKILL);
	fflush(NULL);
	_exit(VMR_EXIT_NOT_FORWARDED);
}

static int backing_find(memory_target target);
static int initial_backing_find(pid_t ctx);
static void obj_forget(memory_target pid, uint64_t addr, uint64_t len);

/* Exec publishes a fresh MM object and native binding. Previous objects,
 * layouts, ownership and COW ancestry remain attached to their MM lifetime
 * for surviving peers and callbacks that captured the previous target. */

#include "snapshot-quiesce.h"

static int forward_child(memory_target pid, const struct vmctx_event *ev,
		const struct vmr_rsp *publication, struct snapshot_lease *snapshot)
{
	const struct vmr_rsp rs=*publication;
		int thr = rs.child_shared_mm;
		if (!snapshot->serial)
			return fwd_broken(pid, ev, "an unprepared source child publication");
		if (rs.child_source<=0 || rs.child_source>INT_MAX)
			return fwd_broken(pid,ev,"an invalid retained source child token");
		uint64_t srcname=(uint64_t)rs.child_source;
		memory_target kid;
		struct child_source_pages source_pages = {0};
		if (!thr && child_source_pages_read(&rs.child_binding, &source_pages)) {
			free(source_pages.pages);
			return fwd_broken(pid, ev, "obtaining initial child page ownership");
		}

		kid = spawn_forked_context(pid, srcname,
					   thr ? backing_find(pid) : -1,
					   &rs.child_binding,&source_pages);
		free(source_pages.pages);

		if (kid) {
			struct child_arg *ca = malloc(sizeof(*ca));

			src_clone_note(ctx_id(kid), ctx_id(pid));
			/*
			 * The child's address space is a copy of the parent's,
			 * layout included -- without this, every fault in the
			 * child found no region and every protection question
			 * was answered from nothing.
			 */
			if (!thr)
				region_inherit(kid, pid);
			shared_range_inherit(kid, pid);
			if (!ca)
				execution_start_failed(ctx_id(kid), "allocating child service state");
			ca->id=ctx_id(kid);ca->shared=thr;
			ca->srcpid = srcname;
			ca->binding=rs.child_binding;
			pthread_t th;
			/* Reserve before pthread_create: the new service may finish
			 * before its creator next runs. The count covers its final
			 * source acknowledgement, not merely the guest task's life. */
			pthread_mutex_lock(&child_lifetime_lock);
			n_live_children++;
			pthread_mutex_unlock(&child_lifetime_lock);
			/* All snapshot data/metadata is installed. Release before the
			 * child can execute or a native vfork parent waits for it. */
			snapshot_end(snapshot);
			int error = pthread_create(&th, NULL, child_thread, ca);
			if (error) {
				free(ca);
				errno = error;
				execution_start_failed(ctx_id(kid), "starting child service thread");
			}
			pthread_detach(th);
		} else {
			return fwd_broken(pid, ev, "publishing the source child execution context");
		}
	return 1;
}

static int forward(memory_target *target, const struct vmctx_event *ev,
		   struct vmctx_reply *rep, unsigned long *n_fwd)
{
	memory_target pid=*target;
	int boundary = ev->type == VMCTX_EV_RUNTIME;
	struct vmr_req rq = { .magic = VMR_MAGIC,
		.nr = boundary ? VMR_OP_BOUNDARY : VMR_OP_EXECUTE,
		.call_nr = boundary ? 0 : ev->nr };
	struct vmr_rsp rs;
	struct vmr_cpu_state sendcpu;
	const void *sendbuf = NULL;
	uint64_t sendlen = 0;
	struct snapshot_lease snapshot __attribute__((cleanup(snapshot_end))) = {0};

	/*
	 * VMREMOTE_TRACE_AFTER_EXC: the first calls the guest makes once an
	 * exception has been handed over, with the address each was made from.
	 *
	 * A count alone cannot tell a handler that runs once from one that runs
	 * for ever -- "464035 mprotect calls" is the same number whether the
	 * program is looping in its handler, looping on the instruction that
	 * faulted, or looping somewhere else entirely, and those want different
	 * fixes. The instruction pointer distinguishes them.
	 */
	if (trace_after_exc && n_exc_delivered) {
		static __thread unsigned long shown;

		if (shown < 24) {
			shown++;
			fprintf(stderr, "[vmremote] after-exc %lu: call %llu from "
				"rip 0x%llx rsp 0x%llx (a0 0x%llx a1 0x%llx a2 0x%llx)\n",
				shown, (unsigned long long)ev->nr,
				(unsigned long long)ev->rip,
				(unsigned long long)ev->rsp,
				(unsigned long long)ev->args[0],
				(unsigned long long)ev->args[1],
				(unsigned long long)ev->args[2]);
		}
	}

	if (!boundary) memcpy(rq.args, ev->args, sizeof(rq.args));
	/*
	 * A call whose answer may be "carry on somewhere else" has to send the
	 * state it is carrying on from. rt_sigreturn is the case: the owner
	 * reads the frame the program is standing on and hands back the
	 * registers saved in it, and it needs the stack pointer to find it.
	 * Nothing here knows that is what call 15 does -- it knows only that
	 * some replies carry registers, so some requests must carry them too.
	 */
	/*
	 * The machine state goes with every call, and comes back with the
	 * answer.
	 *
	 * This used to be sent only for one syscall number, and the FS and GS
	 * bases were then handled by a second special case further down: this
	 * side tested for call 158 and for Linux's ARCH_SET_FS and ARCH_SET_GS,
	 * and took the value out of the guest's own argument register. Its
	 * comment claimed it was "told the value rather than deciding it",
	 * which was not true -- it was reading the guest's registers and
	 * deciding which system register to change, from constants that mean
	 * nothing off this operating system.
	 *
	 * A base is not thread-local storage and not a Linux idea. It is a
	 * system register, part of the architectural state, and the state
	 * belongs to the side that owns the program. So hand that side the
	 * whole state with the call and let it hand the whole state back. What
	 * a register means is never asked here.
	 */
	if (cpu_state_ctl(pid, VMCTX_CTL_GETCPU, &sendcpu) < 0)
		return fwd_broken(pid, ev, "capturing architectural CPU state");
	sendbuf = &sendcpu;
	sendlen = vmr_cpu_wire_size(&sendcpu);
	if (!sendlen)
		return fwd_broken(pid, ev, "invalid captured architectural CPU state");
	struct vmctx_runtime runtime;
	if (execution_runtime(pid, VMCTX_RUNTIME_READ, &runtime) < 0)
		return fwd_broken(pid, ev, "capturing execution CPU time");
	rq.execution_epoch = runtime.epoch;
	rq.execution_ns = runtime.total_ns;

	/* PREPARE carries the CPU frame once. Subsequent requests carry only
	 * the source ticket, so tracing changes cannot be overwritten between
	 * admission and dispatch. Polling leaves the connection free for child
	 * snapshot requests while a native vfork parent is still blocked. */
	if (!boundary) rq.nr=VMR_OP_PREPARE;
	rq.datalen=sendlen;
	uint64_t ticket=0;
	int prepared=0, child_seen=0;
	unsigned polls=0;
	fwd_enter(ctx_id(pid),(unsigned long)ev->nr);
	for (;;) {
		if (io_send2(&rq,sizeof(rq),sendbuf,rq.datalen)<0) {
			fwd_leave(ctx_id(pid)); return fwd_broken(pid,ev,"sending call admission/dispatch");
		}
		if (io_all(0,&rs,sizeof(rs))<0 || rs.magic!=VMR_MAGIC ||
		    !vmr_rsp_bindings_valid(&rs) || !vmr_binding_valid(&rs.binding) ||
		    !ctx_matches_source(ctx_id(pid),&rs.binding) ||
		    rs.datalen>sizeof(databuf) ||
		    (rs.datalen && io_all(0,databuf,rs.datalen)<0)) {
			fwd_leave(ctx_id(pid)); return fwd_broken(pid,ev,"reading call admission/dispatch");
		}
		if (boundary) break;
		if ((!rs.ticket && !rs.ended) || (ticket && ticket!=rs.ticket) ||
		    rs.call_reserved || rs.call_state<VMR_CALL_ENTERING || rs.call_state>VMR_CALL_COMPLETE) {
			fwd_leave(ctx_id(pid)); return fwd_broken(pid,ev,"an invalid source call ticket/state");
		}
		ticket=rs.ticket;
		if (rs.effects & VMR_EFFECT_CHILD) {
			if (child_seen || !prepared ||
			    (rs.call_state!=VMR_CALL_RUNNING && rs.call_state!=VMR_CALL_COMPLETE) ||
			    !forward_child(pid,ev,&rs,&snapshot)) {
				fwd_leave(ctx_id(pid)); return fwd_broken(pid,ev,"publishing a retained source birth");
			}
			child_seen=1;
		}
		if (rs.call_state==VMR_CALL_COMPLETE) break;
		if (rs.datalen || rs.ended || (rs.effects & ~VMR_EFFECT_CHILD) || rs.mapping.kind) {
			fwd_leave(ctx_id(pid)); return fwd_broken(pid,ev,"premature native call completion data");
		}
		rq=(struct vmr_req){.magic=VMR_MAGIC,.nr=VMR_OP_CALL_POLL,.ticket=ticket};
		sendbuf=NULL;
		if (rs.call_state==VMR_CALL_ADMITTED) {
			if (rs.prepare_flags & ~(VMR_PREPARE_SETTLE|VMR_PREPARE_SNAPSHOT)) {
				fwd_leave(ctx_id(pid)); return fwd_broken(pid,ev,"unsupported source preparation");
			}
			if (!prepared) {
				if ((rs.prepare_flags & VMR_PREPARE_SNAPSHOT) && snapshot_begin(pid,&snapshot)) {
					fwd_leave(ctx_id(pid)); return fwd_broken(pid,ev,"quiescing admitted source call");
				}
				if ((rs.prepare_flags & VMR_PREPARE_SETTLE) && page_recall_all(pid)) {
					fwd_leave(ctx_id(pid)); return fwd_broken(pid,ev,"settling admitted source call");
				}
				prepared=1;
			}
			rq.nr=VMR_OP_EXECUTE;
		} else if (rs.call_state==VMR_CALL_RUNNING && !prepared) {
			fwd_leave(ctx_id(pid)); return fwd_broken(pid,ev,"dispatch preceding preparation");
		}
		/* Fast completions need no sleep. Native tracing and blocking calls
		 * may remain pending indefinitely without an invented deadline. */
		if (++polls>8) usleep(1000);
	}
	fwd_leave(ctx_id(pid));
	rep->action = VMCTX_ACT_DONE;
	rep->retval = (uint64_t)rs.retval;
	/*
	 * A successful exec replaced the whole address space, and regions[]
	 * described the OLD one. With ASLR off the new image's interpreter
	 * lands at the SAME addresses, so region_snapshot() would stamp the previous
	 * program's protections onto the new program's pages -- measured on
	 * firefox: bash's ld.so had sealed its RELRO, the exec'd binary's
	 * ld.so wrote its relocations into that address, the stale region said
	 * read-only, and the write was escalated to the owner as the program's
	 * own fault (SIGSEGV, run over). region_forget_range() already guards
	 * the mmap version of this exact mistake -- its comment names the
	 * stamp -- and exec is the general case: drop everything. Post-exec
	 * faults take the ask-the-source path, and the new program's own
	 * forwarded mprotects re-establish real protections as it runs.
	 */
	/*
	 * The source says the process this context belongs to has DIED there
	 * (its service task is a zombie or gone: a sibling's kill(2), the
	 * program's own abort). Nothing of it may keep executing here: every
	 * context of its address space is ended, through the ordinary teardown
	 * (the child's own wait, FINISH to home), with the source's status
	 * recorded so the end is reported as the program's doing. Measured
	 * before this: netsurf's parent SIGKILLed a helper whose two contexts
	 * then spun on futex answered -EIO for the rest of the run (the page
	 * never painted); firefox's content process abort()ed and ran on.
	 */
	/*
	 * Only THIS context: the source's word is about the one service task
	 * that would have run this call. A process killed by a signal has every
	 * thread's task dead, and each of its contexts here gets its own word on
	 * its own next call; a single thread whose task ended is exactly one
	 * context. Ending the whole address space on one thread's word took a
	 * live netsurf down with its exiting helper threads (measured).
	 */
	if (rs.ended) {
		/* The native source completed this request by ending the task.
		 * It has no CPU frame to reinstall, but was still forwarded. */
		__atomic_add_fetch(n_fwd, 1, __ATOMIC_RELAXED);
		n_src_ended++;
		if (n_src_ended <= 16)
			fprintf(stderr, "[vmremote] ctx %d: its service task died "
				"on the source (%s %d); ending this context here "
				"on that word\n", ctx_id(pid),
				WIFSIGNALED(rs.ended_status) ? "signal" :
				"exit status",
				WIFSIGNALED(rs.ended_status) ?
				WTERMSIG(rs.ended_status) :
				WEXITSTATUS(rs.ended_status));
		src_ended_note(ctx_id(pid), rs.ended_status);
		n_src_ended_ctx++;
		ctx_signal(ctx_id(pid), SIGKILL);
		return 0;
	}
	struct vmr_cpu_state *replycpu = (struct vmr_cpu_state *)databuf;
	if (vmr_cpu_wire_decode(replycpu, rs.datalen))
		return fwd_broken(pid, ev, "a reply without complete CPU state");
	if(rs.effects&VMR_EFFECT_IMAGE) {
		struct execution_mm *mm=source_mm_resolve(&rs.binding);
		struct linux_execution_context native;
		if(!mm || !ctx_native_copy(ctx_id(pid),&native))
			return fwd_broken(pid,ev,"resolving the source image MM");
		struct linux_execution_object object={.control=initial_object_control,.opaque=&native};
		memory_target next=execution_target_publish(pid->context,mm,&rs.binding,
			linux_execution_object_replace,&object);
		if(!next || !next->view.epoch || next!=execution_target_capture(pid->context))
			return fwd_broken(pid,ev,"publishing the native image binding");
		pid=next;*target=next;
		map_trace("executor","image-binding context=%d mm=%llu source_epoch=%llu native_epoch=%llu",
			ctx_id(pid),(unsigned long long)pid->source.mm,
			(unsigned long long)pid->source.epoch,(unsigned long long)pid->view.epoch);
	} else if(!vmr_binding_equal(&pid->source,&rs.binding))
		return fwd_broken(pid,ev,"a source MM change without an image publication");
	if (cpu_state_ctl(pid, VMCTX_CTL_SETCPU, replycpu) < 0)
		return fwd_broken(pid, ev, "installing architectural CPU state");
	if(rs.effects&VMR_EFFECT_IMAGE)relayout_from_home(pid);
	if (rs.regs_valid)
		n_regs_from_home++;
	/*
	 * And say what it did to the address space, so the context can
	 * do the same to its own. Without this the change exists only in
	 * the shadow's mirror, and the guest's address space is built by
	 * the fault path instead -- which maps every address it is asked
	 * for read-write-execute.
	 */
	/* Publish shared-object identity, invalidations and ordering records in
	 * one critical section. Locking cons_note alone let an earlier unmap
	 * erase the new shared record between shared_range_note and cons_note;
	 * the fault path then installed received bytes into the private object.
	 * This lock orders local publication only. Source commit ordering and
	 * completion of native SET operations are separate protocol obligations. */
	pthread_mutex_lock(&layout_order_lock);
	if (reply_note_map(pid, rep, &rs) < 0) {
		pthread_mutex_unlock(&layout_order_lock);
		return fwd_broken(pid, ev, "applying source mapping effect");
	}
	/*
	 * Ranges the source's kernel saw vacated while the call ran -- the
	 * mm-mutation hook's report, whatever the call was. Applied AFTER
	 * reply_note_map so an mremap's relocation has landed its bytes
	 * (vacate_keep guards them); cleared after, so the guard covers
	 * exactly this reply.
	 */
	{
		/*
		 * The reply's own constructive map is the OTHER keep-zone
		 * source (mremap's relocation, set in case 25, is the first):
		 * an mmap that LANDS ON LIVE MEMORY -- ld.so placing a
		 * segment with MAP_FIXED over its reservation is the
		 * every-run case -- makes the kernel log the replaced
		 * mapping's unmap, so the same reply carries both the vacate
		 * and the mapping that supersedes it. What this reply
		 * constructs, its vacates must not touch.
		 */
		if (vacate_keep.end == vacate_keep.start &&
		    rs.mapping.kind == VMR_MAP_SET && rep->map_len) {
			vacate_keep.start = rep->map_addr & ~0xfffULL;
			vacate_keep.end   = (rep->map_addr + rep->map_len +
					     0xfffULL) & ~0xfffULL;
		}
		/* Retained journal consumption below replaces the destructive
		 * eight-entry reply queue. Source-local legacy metadata still
		 * drains its old log while that adapter is migrated. */
		if (rs.mapping.kind == VMR_MAP_SET && rep->map_len) {
			map_recent_note(rep->map_addr & ~0xfffULL,
					(rep->map_addr + rep->map_len +
					 0xfffULL) & ~0xfffULL);
			/* ...and its place in the order, for the vacates
			 * that arrive after it on other replies -- with
			 * enough of the construction to REPLAY it if a racing
			 * stale punch lands on it anyway (see declined_at's
			 * repair). */
			cons_note_locked(pid, rep->map_addr & ~0xfffULL,
				  (rep->map_addr + rep->map_len + 0xfffULL) &
				  ~0xfffULL, rs.mmseq, rep->map_op,
				  rep->map_prot, rep->map_off);
		}
		/*
		 * A PROTECTION change is a construction too, and the one a
		 * sibling context most often needs replayed: glibc grows a
		 * non-main arena by mprotect(rw) over its PROT_NONE
		 * reservation, jemalloc commits chunks the same way, and the
		 * change reaches only the calling context (a cross-task PROT
		 * is refused like a cross-task SET). A sibling's first touch
		 * of the grown page then faulted against ITS PROT_NONE
		 * mapping with no row to repair from ("NO-VMA ... a mapping
		 * without the right ... unrepairable", -14): netsurf's helper
		 * thread died there in every loopback run, the page one past
		 * the arena's old end. With the row, the repair replays the
		 * mprotect into the faulting task.
		 */
		if (rep->map_op == VMCTX_MAP_PROT && rep->map_len)
			cons_note_locked(pid, rep->map_addr & ~0xfffULL,
				  (rep->map_addr + rep->map_len + 0xfffULL) &
				  ~0xfffULL, rs.mmseq, rep->map_op,
				  rep->map_prot, 0);
	}
	pthread_mutex_unlock(&layout_order_lock);
    if (access_sync(pid))
        return fwd_broken(pid, ev, "applying committed access journal");
    vacate_keep.start = vacate_keep.end = 0;
	/*
	 * This is where calls 2 and 257 used to be read out of the guest's
	 * memory to learn what name each descriptor had been opened with, and
	 * call 3 to forget it again. Nothing on this side asks that question
	 * now, and asking it was never anything but the destination keeping a
	 * second, worse copy of the source's descriptor table.
	 */
	/*
	 * A register change decided at the source, applied here.
	 *
	 * arch_prctl sets a thread's FS or GS base. The source records the
	 * new value -- the register file is its own, like everything else --
	 * and this machine puts it into the hardware it is running. It is
	 * told the value rather than deciding it, which is the difference
	 * between applying state and owning it.
	 */

	/*
	 * Atomic because several service threads pass the same counter: plain
	 * ++ tears under parallel service and the run-end report then prints
	 * n_sys - n_fwd as 2^64-2 ("-2 syscalls NOT forwarded"), which reads
	 * like a re-issued call and is only a torn increment.
	 */
	__atomic_add_fetch(n_fwd, 1, __ATOMIC_RELAXED);
	return 1;
}


/*
 * Shared across contexts: with a forked guest several threads service several
 * contexts at once, and these describe the run as a whole.
 */
static unsigned long n_sys, n_fwd, n_runtime;

/*
 * Contexts still being serviced. A forked guest outlives its parent often
 * enough — the parent exits, the child keeps working — and tearing the process
 * down when the first context ends kills the threads servicing the rest, which
 * looks exactly like the child hanging.
 */


/* ---- following a guest that forks -------------------------------------- */

static void service_context(execution_id id);


/*
 * A backing object for one memory group.
 *
 * A memfd, sized to cover the guest's whole address range and left sparse, so a
 * guest virtual address can be used directly as the offset. Nothing is allocated
 * until a fault touches it. Contexts that must share memory are handed the same
 * one; a context that must not is handed its own.
 */
/*
 * One object for what the program shares across contexts.
 *
 * A context's own object is copied by a fork, which is what fork means, so a
 * range that must survive one cannot live there. Anonymous shared memory is
 * exactly that case: it has no name to share, only an address, and the same
 * address in two objects is two pages.
 */
static int shared_obj = -1;

#include "linux-execution-fds.h"

static int backing_new(const char *why)
{
	int fd = linux_execution_object_fd(why);

	if (fd < 0) {
		fprintf(stderr, "[vmremote] cannot make a backing object (%s): %s\n",
			why, strerror(errno));
		return -1;
	}
	/*
	 * Big enough for any guest address. Sparse: this reserves nothing, it
	 * only lets an address be used as an offset into the object.
	 */
	if (ftruncate(fd, 1L << 47) < 0) {
		fprintf(stderr, "[vmremote] cannot size the backing object: %s\n",
			strerror(errno));
		close(fd);
		return -1;
	}
	return fd;
}

/*
 * Which backing object each context runs on.
 *
 * A thread is a context handed its *parent's* object -- that is the whole of
 * sharing an address space, and the only way a thread differs from a process
 * here. So the object has to be findable from the parent's context id.
 */
#define MAX_BACKING 4096
static struct { pid_t ctx; int fd; } backing_of[MAX_BACKING];
static int nbacking;

static void backing_note(pid_t ctx, int fd)
{
	int i = __sync_fetch_and_add(&nbacking, 1);

	if (i < MAX_BACKING) {
		backing_of[i].fd = fd;
		__atomic_store_n(&backing_of[i].ctx, ctx, __ATOMIC_RELEASE);
	}
}

static int initial_backing_find(pid_t ctx)
{
	int n = __atomic_load_n(&nbacking, __ATOMIC_ACQUIRE), i;

	for (i = 0; i < n && i < MAX_BACKING; i++)
		if (__atomic_load_n(&backing_of[i].ctx, __ATOMIC_ACQUIRE) == ctx)
			return backing_of[i].fd;
	return -1;
}

static int backing_find(memory_target target)
{
	if(!target) {errno=ESRCH;return -1;}
	return target->view.mm->backing_fd;
}

/*
 * Does this context's memory object already hold this page?
 *
 * The object is the context's memory: an address is its own offset. A hole
 * reads as zeros and means nothing has ever been put there; data means
 * something has -- and after a fork that something is the parent's page,
 * copied in before the child ran.
 *
 * It is asked before fetching a page from the source, because for a forked
 * context the fetch is not merely wasteful but wrong: it replaces the parent's
 * memory with the program as it was loaded, and the child then runs on a
 * startup stack it never had.
 */
/*
 * A thread's object is asked the same question, and it used to be excluded.
 *
 * The argument for excluding it was that a shared object is a cache while its
 * parent and the shadow are both live, so the source is the authority — and the
 * evidence was a futex word the source had already cleared, which a joining
 * thread then spun on 134,722 times.
 *
 * That was the loan record failing, not the object. A page the shadow holds is
 * recorded as lent, and every caller here asks page_holder() first, so a page
 * out on loan is recalled and never read from the object. What was broken is
 * that the loan was recorded against a different address space than the one
 * asking: as_id() answered Tgid, and a thread context has its own. With the
 * loan visible to the whole address space, "the object has it and nobody has
 * borrowed it" means the destination holds the live copy, for a thread exactly
 * as for a fork.
 *
 * Excluding threads was not merely conservative, either. The object is mapped
 * MAP_SHARED into every context on it, so a page fetched from the source is
 * installed *over* the memory a sibling is using: th9's main context lost the
 * stack it was standing on to the worker's first fault and died in
 * __stack_chk_fail, on a stack nothing had smashed.
 */
static int backing_has(memory_target ctx, uint64_t addr)
{
	off_t page = (off_t)(addr & ~0xfffUL), d;
	int fd;

	fd = backing_find(ctx);
	if (fd < 0)
		return 0;
	d = lseek(fd, page, SEEK_DATA);
	return d >= 0 && d < page + 4096;
}


/* ---- creating a context for a guest fork, the generic way --------------- */
/*
 * The destination's half of a clone, with no Linux clone semantics in it.
 *
 * The source has already forked: that gave the child its pid, descriptors,
 * credentials and namespaces, by the rules of the machine the program believes
 * it is on. What remains is an execution context, and that needs exactly four
 * things a destination of any kind can do: make a process, map some address
 * ranges, start running at a given register state, and let a monitor drive it.
 * Nothing is copied here -- the source owns the pages, so the child starts with
 * every range mapped empty and faults them in, which is what an exec'd context
 * already does.
 *
 * A thread would differ in one respect only: the context would be a *thread* of
 * the process hosting the parent, so it would share the address space instead of
 * mapping its own. That needs the hosting process to create it, which the
 * monitor cannot do from outside, so it is not done here yet.
 */
struct spawn_arg {
	struct vmctx_run_config *cfg;
};

/*
 * Drop every descriptor the monitor was holding, keeping only what a context
 * needs to run.
 *
 * A context is made with clone(), so it inherits the monitor's whole descriptor
 * table -- including page-channel connections the monitor has accepted. A shadow
 * that then asks for a page over such a connection waits for ever: the only
 * process still holding that end is a context, which has one thread and is
 * running guest code, and will never read it. The shadow blocks in
 * handle_userfault, its guest never finishes the syscall, and whoever waits on
 * that guest waits too.
 *
 * A context needs its backing object and the standard three; nothing else here
 * belongs to it.
 */
/*
 * Runs in a freshly forked child of a HEAVILY THREADED process, and everything
 * it may call is decided by that one fact.
 *
 * spawn_forked_context() clones without CLONE_VM, so this child has one thread
 * where its parent had many -- and it inherits their locks in whatever state
 * they were in at the instant of the fork, with no owner left alive to release
 * them. Any function that takes a lock can therefore block here for ever.
 *
 * This ended in `fprintf(stderr, ...)`, which takes stdio's lock, and vmremote
 * writes to stderr from every thread it has. Measured: pthread_create failing
 * about 4 runs in 25 with EAGAIN, because the child hung in that fprintf, never
 * reached vmctx_run, and so never became a context -- VMCTX_CTL_ATTACH answers
 * EINVAL for a task whose ->vmctx is NULL (kernel/vmctx.c, "not a VM context"),
 * the parent retried until its budget ran out, and the guest was told its
 * thread could not be created. A log line cost the program a thread.
 *
 * Use native close_range and write only on failure. No stdio, allocation or
 * directory enumeration may occur between fork and vmctx_run.
 */
static void drop_monitor_fds(int keep)
{
	if (linux_execution_close_fds(keep, shared_obj)) {
		static const char message[] = "[vmremote] cannot close inherited monitor descriptors\n";
		if (write(2, message, sizeof(message)-1) < 0) {
			/* Reporting must not delay the child's failure exit. */
		}
		_exit(125);
	}
}

static int spawned_child(void *arg)
{
	struct spawn_arg *sa = arg;

	drop_monitor_fds(sa->cfg->backing_fd);
	/*
	 * Nor here. A forked context is built exactly like the first: with no
	 * mappings, so every page comes from its backing object on demand and its
	 * contents from the source.
	 */
	{
		/*
		 * Say why the context ended, as net_child does. Without this a
		 * context that vmctx_run refuses looks identical to one whose
		 * guest ran and exited: the task is simply gone, and the monitor
		 * reports "WAIT failed (task gone)" with no cause.
		 */
		long ret = syscall(__NR_vmctx_run, sa->cfg);
		int e = errno;

		/*
		 * out_counter is the kernel's own count of syscalls it serviced
		 * for this context, and it is the number that settles whose
		 * duplicate a duplicate is. The monitor counts the events it
		 * received; if that is larger, the extra one was made on this
		 * side, and if the two agree the guest really did execute the
		 * instruction twice. th9 loses a line to a write(2) that
		 * happens twice and nothing has yet said which.
		 */
		fprintf(stderr, "[vmremote] spawned context ended: vmctx_run -> "
			"%ld%s%s (enters %llu exits %llu syscalls %llu faults "
			"%llu status %d)\n", ret,
			ret < 0 ? " errno=" : "", ret < 0 ? strerror(e) : "",
			(unsigned long long)sa->cfg->out_enters,
			(unsigned long long)sa->cfg->out_exits,
			(unsigned long long)sa->cfg->out_counter,
			(unsigned long long)sa->cfg->out_faults,
			sa->cfg->out_status);
		fflush(stderr);
	}
	_exit(126);
}

/*
 * Returns the destination pid of the new context, or -1. The caller answers the
 * guest with the *source's* pid, not this one.
 */
/*
 * There was a backing_snapshot() here, and it is gone with the thing it did.
 *
 * It copied the parent's whole populated object into the child's at the fork --
 * every page of the address space duplicated, including the ones neither side
 * ever writes. What replaces it is §7.0a: the child's object starts empty, the
 * source decides which pages are still the parent's, and the break happens one
 * page at a time where the page already is. Kept as a comment rather than as an
 * unused function, because dead code reads as a live decision.
 */

/*
 * SETREGS, made to stick on a task that may not have slept yet.
 *
 * SETREGS writes rip/rsp and the GPRs into the task's pt_regs -- which live in
 * its kernel stack and survive anything -- and the FS/GS bases with
 * x86_fsbase_write_task(), WHICH DO NOT: a task still on a CPU carries its live
 * FSBASE in the register, and its next context switch SAVES that live value
 * over whatever was just written. A freshly spawned context is exactly that
 * task: between clone() and vmctx_run it runs a short stint of THIS process's
 * own userspace, with this process's thread pointer live, and ATTACH succeeding
 * only proves it has reached vmctx_run -- not that it has been switched out. A
 * SETREGS landing in that window is silently undone, the entry path then loads
 * the monitor's own thread pointer into the guest, and the guest's first
 * fs-relative load reads another program's TLS.
 *
 * Measured on ws1 under a pool (the residual "lost store"): 13 of 32 pooled
 * failures were a new thread dying in __ctype_init at rip 0x437716 with
 * fs_base 0x...580 -- a vmremote thread's own TCB -- while the correct pd sat
 * in r8; two more had fs_base equal to MAIN's, the zeroed-frame fallback. The
 * existing read-back checked rip/rsp, the two registers a switch cannot move.
 *
 * So: write, wait out any in-flight deschedule, read back, and require the
 * bases right TWICE with a sleep between. A task seen twice with the right
 * bases has had them loaded at a switch-in, and every switch-out from then on
 * saves those same values back. The retry counter is the detector: it is the
 * number of times the race actually fired.
 */
static unsigned long n_setregs_unstuck, n_setregs_lost;

static int setregs_settled(memory_target kid, const struct vmctx_uregs *u,
			   const char *who)
{
	struct vmctx_uregs back;
	int t, ok = 0, need_write = 1;

	for (t = 0; t < 400; t++) {
		if (need_write) {
			if (ctl(kid, VMCTX_CTL_SETREGS, (void *)u) < 0)
				return -1;
			need_write = 0;
		}
		usleep(300);
		if (ctl(kid, VMCTX_CTL_GETREGS, &back) < 0)
			return -1;
		/*
		 * A zero base is "leave it alone" -- the kernel's SETREGS skips
		 * the write (`if (u.fs_base) x86_fsbase_write_task(...)`), so
		 * demanding it read back zero is demanding the impossible. The
		 * first context is exactly that case: its fs at entry is the
		 * loader's don't-care, and the program installs its own with
		 * arch_prctl once it runs. Asked-zero is therefore satisfied
		 * by anything; a NONZERO ask is the one that must stick.
		 */
		if ((!u->fs_base || back.fs_base == u->fs_base) &&
		    (!u->gs_base || back.gs_base == u->gs_base) &&
		    back.rip == u->rip && back.rsp == u->rsp) {
			if (++ok == 2)
				return 0;
		} else {
			ok = 0;
			need_write = 1;
			n_setregs_unstuck++;
			if (n_setregs_unstuck <= 8)
				fprintf(stderr, "[vmremote] %s: SETREGS did not "
					"stick for %d (fs asked 0x%llx, reads "
					"back 0x%llx): the task had not slept "
					"and the switch saved its old base over "
					"the write; retrying\n", who, ctx_id(kid),
					(unsigned long long)u->fs_base,
					(unsigned long long)back.fs_base);
		}
	}
	n_setregs_lost++;
	fprintf(stderr, "[vmremote] %s: SETREGS never settled for %d after %d "
		"tries (fs asked 0x%llx, reads back 0x%llx)\n", who, ctx_id(kid),
		t, (unsigned long long)u->fs_base,
		(unsigned long long)back.fs_base);
	return -1;
}

static memory_target spawn_forked_context(memory_target parent, uint64_t srcpid, int backing,
				  const struct vmr_mm_binding *birth,
				  const struct child_source_pages *source_pages)
{
	/*
	 * One config and one clone argument per call, on this thread's stack.
	 *
	 * They used to be static, and this function is called from every
	 * service thread -- the first context's and every child_thread's. Two
	 * contexts forking at once shared one of each, so the second fork
	 * overwrote the first's backing_fd and flags between the first thread
	 * filling them in and its clone() reading them: a child handed another
	 * context's memory object, which is two guests writing one address
	 * space while each believes it is alone in it.
	 *
	 * The stack is the right lifetime and not merely the convenient one.
	 * The clone below carries no CLONE_VM, so the child's address space is
	 * a copy taken at the moment of the call -- this frame included -- and
	 * nothing this thread does afterwards can reach it. The child reads
	 * `sa` out of its own copy, so the frame may be popped as soon as
	 * clone() has returned, which is inside this function. Nothing needs to
	 * outlive it and there is nothing to free.
	 */
	struct vmctx_run_config ccfg;
	struct spawn_arg sa;
	struct vmctx_uregs u;
	void *cstack;
	pid_t native_kid;
	int i;

	/* Seed a provisional frame while the child's entry gate is closed. */
	if (ctl(parent, VMCTX_CTL_GETREGS, &u) < 0) {
		fprintf(stderr, "[vmremote] spawn: GETREGS %d: %s\n",
			ctx_id(parent), strerror(errno));
		return NULL;
	}
	u.rax = 0;
	u.orig_rax = (uint64_t)-1;
	/* This is only a valid provisional frame for an unreleased executor.
	 * The child service replaces the complete CPU state after its own source
	 * monitor has serviced initial native work, faults and ptrace stops. */

	/*
	 * No ranges are computed or handed over.
	 *
	 * This used to work out which of the parent's mappings were the guest's,
	 * by subtracting the monitor's own map from the context's, and pass them
	 * so the child could map them in advance. The child maps nothing now:
	 * every address arrives by faulting, is backed from the context's backing
	 * object, and is filled from the source.
	 */
	memset(&ccfg, 0, sizeof(ccfg));
	/*
	 * Its own object: a forked process must not see its parent's later
	 * writes, and separate backing is exactly that snapshot. A thread would
	 * be handed the parent's object instead, and nothing else would differ.
	 */
	/*
	 * A thread is handed its parent's object and shares its memory; a process
	 * is handed a new one and shares nothing, which is the fork snapshot.
	 */
	struct execution_mm *mm=source_mm_resolve(birth);
	if(!mm || (backing>=0 && backing!=mm->backing_fd)) {
		if(mm)errno=EPROTO;
		return NULL;
	}
	ccfg.backing_fd=mm->backing_fd;
	/* The same object, not a copy: that is the whole point of it. */
	ccfg.shared_fd = shared_obj;
	/*
	 * The child's memory exists before the child does.
	 *
	 * Only for a real fork: a thread was handed its parent's object
	 * outright -- that is what sharing an address space *is* here -- so
	 * there is nothing to copy. `backing` is what the caller passed: the
	 * parent's object for a thread, or -1 for a fork, in which case the
	 * object made just above starts out empty and is filled here.
	 *
	 * Done before the clone rather than after it, because "after" was not
	 * early enough: by the time the copy landed the context had already
	 * been created and had pages of its own, and what the guest went on to
	 * read was not what the object held.
	 */
	/*
	 * ...and whether it should, which is a question the child's first
	 * instructions can answer and nothing else can.
	 *
	 * The copy is only right if the object holds the parent's CURRENT
	 * memory. Where the guest holds a page and has written it, the object
	 * may hold what it held before that write -- and then the copy is worse
	 * than no copy at all, because a child whose object has stale bytes at
	 * an address never asks for them, while a child whose object has a hole
	 * there faults and is answered by its own shadow with the parent's copy.
	 * Measured on firefox: the child of the sandbox's fork pops its entry
	 * function off its stack, gets 0x2e8e6, and dies on an instruction fetch
	 * (err 0x14) at that address, byte-identical across six runs; the stack
	 * page is one the guest wrote (site3w) and never asked about again.
	 *
	 * A skip-the-copy switch answered that question, and the answer was
	 * not the one it was built to confirm: with the copy skipped the child
	 * still died on the same instruction fetch, at rip 0x0 instead of
	 * 0x2e8e6 -- it asks for its stack page and is answered with zeros. So
	 * the object was not the only stale source. The child's SHADOW did not
	 * have the page either, because at fork the owner had already handed
	 * it to the guest (the record reads THEIRS), and page_recall_all()
	 * settles only what the OWNER holds. Nothing settles what the GUEST
	 * holds, so the parent's current stack existed in exactly one place
	 * the child could not reach, and a page of zeros was what both of its
	 * sources had. The fork-settle instrument was built to prove exactly
	 * that, proved it, and is deleted (its ledger is in AUDIT.md); the
	 * copy itself is gone too -- see below.
	 */
	{
		if (backing < 0) {
			/*
			 * No fork-time settle of guest pages runs here any
			 * more (see the note where settle_guest_into_object
			 * used to live): a page the guest holds and the child
			 * needs arrives through the record -- the chain-give,
			 * the ancestor read, the escalated break -- not
			 * through a 492-page copy the measurement showed
			 * fixing nothing.
			 */
			/*
			 * Nothing is copied at a fork, and nothing here tracks
			 * one.
			 *
			 * This used to copy_file_range() the parent's whole
			 * populated object into the child's -- every page of the
			 * address space duplicated at the fork, including the
			 * pages neither side ever writes, which is two physical
			 * pages for one address and the opposite of what fork
			 * means. A later attempt replaced it with a lazy
			 * per-page copy plus a write-protect of the parent, and
			 * that was the same defect in slower motion: the copy
			 * still made the page live in two places at once.
			 *
			 * The child's object starts empty and stays that way
			 * until the child touches something, at which point it
			 * faults and asks the source like any other address
			 * space. The source ran the real clone3, so its kernel
			 * owns the copy-on-write relationship and breaks it
			 * there, where the page can only be live at one site.
			 */
		}
	}
	ccfg.flags = VMCTX_FLAG_USERCODE | VMCTX_FLAG_EXIT_PROCESS |
		     VMCTX_FLAG_REDIRECT_SYSCALL | VMCTX_FLAG_REDIRECT_FAULT |
		     VMCTX_FLAG_WAIT_MONITOR | VMCTX_FLAG_RESTORE;
	sa.cfg = &ccfg;

	/*
	 * A stack at a fixed address far from anything the guest uses, and a
	 * different one per context.
	 *
	 * mmap(NULL) puts it wherever the kernel likes, which is the ordinary
	 * mmap region -- inside the layout this child is about to map MAP_FIXED
	 * over. It then unmaps the stack it is standing on, never reaches
	 * vmctx_run, and the monitor's ATTACH answers EINVAL for two seconds
	 * because the task exists but never became a context. net_child has the
	 * same requirement and solves it the same way.
	 */
	/*
	 * The monitor's own address space must carry no guest mapping, because
	 * the clone below copies it into the new context. See
	 * as_apply_map_siblings(): a cross-task SET used to map the backing
	 * object into THIS process, and a fork child inherited its parent's
	 * folios through it. Checked here, at the one instant that inherits.
	 */
	if (monitor_mm_check("before cloning a context")) return NULL;
	{
		static unsigned long next_stack = 0x400000100000UL;
		unsigned long at;

		at = __sync_fetch_and_add(&next_stack, 1UL << 21);
		cstack = mmap((void *)at, 1UL << 20, PROT_READ | PROT_WRITE,
			      MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, -1, 0);
	}
	if (cstack == MAP_FAILED)
		return NULL;
	native_kid = clone(spawned_child, (char *)cstack + (1UL << 20), SIGCHLD, &sa);
	if (native_kid < 0) {
		fprintf(stderr, "[vmremote] spawn: clone: %s\n", strerror(errno));
		return NULL;
	}
	/*
	 * The context exists: record it before anything can ask about it.
	 *
	 * Its shadow opens a page channel the moment it starts, and its first
	 * request names this context. A request naming a context this monitor
	 * has never heard of is refused by dropping the channel -- so the
	 * shadow saw end-of-file, on a page with nothing to do with the cause,
	 * and the fork simply never came back. Nothing was logged at either
	 * end.
	 *
	 * This registration used to happen in the /proc scanners, which is to
	 * say where a context was *discovered* rather than where it was made.
	 * Making it here is both the fix and the model: a context comes into
	 * being because the source asked for one, and that is the moment it is
	 * known.
	 *
	 * Thread or process is not read from the clone flags. It is the same
	 * question as "whose memory is this", and that has already been decided
	 * one line up: a context handed its parent's backing object shares the
	 * parent's address space, and one given a new object does not.
	 */
	/*
	 * Which object holds this context's memory, recorded BEFORE the context
	 * is published rather than after.
	 *
	 * cow_admit_mm() is what makes the copy visible to cow_copies_of(), and from
	 * that instant the original's every store can be held up to give the
	 * copy a page. Registered after, obj_write() found no object for it,
	 * the give failed, and the store was let through anyway -- so the copy
	 * was never given a page it then read as zeros. Measured on mf1, whose
	 * four threads are writing their own stacks throughout the fork: the
	 * child walked glibc's thread list, read a sibling's control block as an
	 * invented empty page and died in __libc_fork on `mov 0x3d8(%r15)` with
	 * r15 = 0.
	 */

	/*
	 * 800ms, and the number is chosen against the OTHER budget rather than
	 * on its own. The guest's clone is answered after one second by the
	 * waiter below, and this used to retry for two -- so an attach that was
	 * merely slow lost the race and the guest was told EAGAIN for a thread
	 * that went on to exist. Measured: pthread_create failing 6 to 8 runs in
	 * 25, always EAGAIN, with the source's own thread creation never once
	 * exceeding 100ms.
	 *
	 * Nested, not equal: a bound that can outlast the thing waiting on it
	 * turns its own success into somebody else's failure. Same lesson as
	 * the page channel's 2s sitting inside the kernel's 2s fault deadline.
	 */
	for (i = 0; i < 800; i++) {
		if (monitor_attach(native_kid) == 0)
			break;
		usleep(1000);
	}
	if (i == 800) {
		fprintf(stderr, "[vmremote] spawn: cannot attach to %d: %s\n",
			native_kid, strerror(errno));
		kill(native_kid, SIGKILL);
		return NULL;
	}
	int created;
	execution_id token=ctx_register(native_kid,mm,birth,&created);
	if (token<0 || !created) {
		fprintf(stderr, "vmremote: cannot publish retained execution identity: %s\n",strerror(errno));
		kill(native_kid, SIGKILL);
		return NULL;
	}
	fprintf(stderr,"[vmremote] execution context %d retains native child %d\n",token,native_kid);
	memory_target kid=ctx_target(token);
	/* Publish identity and initial ownership together. A source-resident
	 * child page already has its fork snapshot; no ancestor may give it a
	 * second copy while a native child syscall writes the original. */
	coh_enter();
	if (backing < 0)
		for (size_t j = 0; j < source_pages->count; j++) {
			cow_give_note(as_id(kid), source_pages->pages[j]);
			page_lend(kid, source_pages->pages[j], srcpid);
		}

	cow_admit_mm(mm);
	/*
	 * The copy exists, so the original may no longer change under it.
	 *
	 * Done here rather than before the clone because there has to be a copy
	 * for the protection to be protecting anything, and done before the
	 * parent is answered -- it is stopped in its forwarded clone at this
	 * instant, which is the only moment its memory is guaranteed to be
	 * exactly what the copy inherited. Only for a real fork: a thread was
	 * handed the parent's own object and shares its memory, which is what
	 * sharing an address space IS, and there is nothing to break.
	 */
	if (backing < 0) {
		/* Execution leases exclude guest stores; the coherence lock also
		 * excludes local writable-map publication while the protection
		 * walk and its records are being installed. */
		int protect_failed = cow_protect_address_space(parent);
		if (protect_failed) {
			coh_leave();
			ctx_signal(ctx_id(kid), SIGKILL);
			return NULL;
		}
	}
	coh_leave();

	/*
	 * Does the child see the same first instruction its parent was standing
	 * on? Compared here rather than assumed, because a child resumed onto
	 * code it cannot read correctly executes whatever bytes are there --
	 * and firefox's sandbox child dies at rip 0x2e8e6 with rsp EXACTLY as
	 * spawned, which is what a jump through a register looks like: nothing
	 * pushed, nothing popped, and an address out of nowhere.
	 *
	 * The child's memory is a snapshot of the parent's backing object, and a
	 * page the guest holds need not be IN that object -- so its entry point
	 * is precisely the address most likely to be wrong and least likely to
	 * be noticed, since a wrong instruction is not a fault.
	 */
	{
		unsigned char pb[16], cb[16];
		long a1 = peek(parent, u.rip, pb, sizeof(pb));
		long a2 = peek(kid, u.rip, cb, sizeof(cb));

		if (a1 == (long)sizeof(pb) && a2 == (long)sizeof(cb) &&
		    memcmp(pb, cb, sizeof(pb)) != 0) {
			int b;

			fprintf(stderr, "[vmremote] spawn: the child's code at "
				"its entry 0x%llx DIFFERS from its parent's\n"
				"[vmremote]   parent:", (unsigned long long)u.rip);
			for (b = 0; b < 16; b++)
				fprintf(stderr, " %02x", pb[b]);
			fprintf(stderr, "\n[vmremote]   child :");
			for (b = 0; b < 16; b++)
				fprintf(stderr, " %02x", cb[b]);
			fprintf(stderr, "\n");
		} else if (a2 != (long)sizeof(cb)) {
			int b;

			/*
			 * Normal at this instant -- the child has no pages yet
			 * and will fault its entry in. What is worth printing is
			 * what the PARENT has there, because that is the
			 * instruction the child is supposed to execute, and a
			 * child that dies without ever pushing or popping has
			 * either executed something else or jumped through a
			 * register these bytes will name.
			 */
			fprintf(stderr, "[vmremote] spawn: child entry 0x%llx "
				"not yet mapped here; the parent has:",
				(unsigned long long)u.rip);
			for (b = 0; b < 16 && a1 == (long)sizeof(pb); b++)
				fprintf(stderr, " %02x", pb[b]);
			{
				/*
				 * And the word the `ret` in that sequence will
				 * pop. The parent is standing on this stack and
				 * has the right value; the child inherits the
				 * page, and if its copy differs it returns to
				 * whatever is there instead. That is the whole
				 * of "spawned at a good rip, died at 0x2e8e6
				 * having pushed nothing".
				 */
				uint64_t w = 0;

				if (peek(parent, u.rsp, &w, sizeof(w)) ==
				    (long)sizeof(w))
					fprintf(stderr, "  [parent's word at rsp "
						"0x%llx = 0x%llx -- this is the "
						"address the child must return "
						"to]",
						(unsigned long long)u.rsp,
						(unsigned long long)w);
			}
			{
				/*
				 * And what the CHILD's own object holds at that
				 * same address, read straight out of the file
				 * after the snapshot. The child never faults on
				 * this page -- it is in the object, so it just
				 * reads it -- which makes the object's word the
				 * one the program actually returns through.
				 */
				uint64_t ow = 0;

				unsigned char oc[16];
				int b;

				if (pread(ccfg.backing_fd, &ow, sizeof(ow),
					  (off_t)u.rsp) == (ssize_t)sizeof(ow))
					fprintf(stderr, "  [child's OBJECT at "
						"0x%llx = 0x%llx]",
						(unsigned long long)u.rsp,
						(unsigned long long)ow);
				/*
				 * And the CODE. The stack turned out to be
				 * right in the child's object, and rsp never
				 * moves before it dies -- so the `ret` never
				 * ran, and what it executed at its entry is the
				 * only thing left that can send it to 0x2e8e6.
				 */
				if (pread(ccfg.backing_fd, oc, sizeof(oc),
					  (off_t)u.rip) == (ssize_t)sizeof(oc)) {
					fprintf(stderr, "  [child's OBJECT code:");
					for (b = 0; b < 16; b++)
						fprintf(stderr, " %02x", oc[b]);
					fprintf(stderr, "%s]",
						(a1 == (long)sizeof(pb) &&
						 memcmp(oc, pb, 16) == 0) ?
						" SAME" : " DIFFERS <<<");
				}
			}
			fprintf(stderr, "  (rax=0x%llx rbx=0x%llx rcx=0x%llx "
				"rdx=0x%llx rsi=0x%llx rdi=0x%llx r8=0x%llx)\n",
				(unsigned long long)u.rax, (unsigned long long)u.rbx,
				(unsigned long long)u.rcx, (unsigned long long)u.rdx,
				(unsigned long long)u.rsi, (unsigned long long)u.rdi,
				(unsigned long long)u.r8);
		}
	}
	/*
	 * Settled, not merely issued: the old read-back here checked rip and
	 * rsp -- the two registers a context switch cannot move -- and never
	 * the FS base, the one it can. See setregs_settled() for the race and
	 * the measurement; the child must not be released until the bases have
	 * provably survived its first deschedule.
	 */
	if (setregs_settled(kid, &u, "spawn") < 0) {
		fprintf(stderr, "[vmremote] spawn: SETREGS %d: %s\n",
			ctx_id(kid), strerror(errno));
		ctx_signal(ctx_id(kid), SIGKILL);
		return NULL;
	}
	/*
	 * Let it start. WAIT_MONITOR holds vmctx_run until a monitor has
	 * attached *and* released it, exactly so the registers can be put in
	 * place first; without this the context never runs and the servicing
	 * thread sees nothing to do. The first context does the same after its
	 * own SETREGS.
	 */
	/*
	 * backing_note() used to be here, "before it is let go rather than
	 * after" -- recorded after the RESUME, the child's very first fault was
	 * answered "no object" and the page was fetched from the source over
	 * its inherited memory. It is earlier still now, above the ctx_born()
	 * that publishes the context, for the same reason one step further
	 * back: the original's stores start being held up for this copy the
	 * moment it is visible.
	 */

	/* The session owner remains attached. The child service installs the
	 * source CPU state before opening its entry gate; no unmonitored gap. */
	fprintf(stderr, "[vmremote] spawned context %d for the source's pid %llu "
		"(parent %d), rip %llx rsp %llx fs %llx gs %llx\n", ctx_id(kid),
		(unsigned long long)srcpid, ctx_id(parent),
		(unsigned long long)u.rip, (unsigned long long)u.rsp,
		(unsigned long long)u.fs_base, (unsigned long long)u.gs_base);
	watchword_log("spawned", parent, -1, u.rip);
	return kid;
}

/* Readiness is a source lifecycle state. Pending replies keep the channel
 * responsive and allow native ptrace stops without an arbitrary deadline. */
static long source_cpu_ready(pid_t executor, const uint64_t args[6],
			     struct vmr_cpu_state *cpu,struct vmr_mm_binding *binding)
{
	for (;;) {
		struct vmr_rsp receipt;
		long r = home_call_runtime(VMR_OP_CPUSTATE,args,NULL,0,cpu,sizeof(*cpu),0,0,&receipt);
		if (home_call_failed) return -1;
		if(!vmr_binding_valid(&receipt.binding)) {errno=EPROTO;return -1;}
		*binding=receipt.binding;
		if (r == VMR_CPU_PENDING) {
			if (home_data_len || home_ended) { errno = EPROTO; return -1; }
			if (executor > 0 && ctx_execution_ended(executor,0)) { errno = ESRCH; return -1; }
			usleep(1000);
			continue;
		}
		if (r == VMR_CPU_ENDED) {
			if (home_data_len || home_ended != 1) { errno = EPROTO; return -1; }
			return r;
		}
		if (home_ended) { errno = EPROTO; return -1; }
		return r;
	}
}

static void *child_thread(void *arg)
{
	execution_id id=((struct child_arg *)arg)->id;
	memory_target cpid=ctx_target(id);
	int born_shared=((struct child_arg *)arg)->shared;
	uint64_t srcpid = ((struct child_arg *)arg)->srcpid;
	struct vmr_mm_binding born=((struct child_arg *)arg)->binding;
	uint64_t a[6] = { 0 };

	free(arg);

	/*
	 * Its own connection, so the source gives it its own monitor and
	 * page/recall channel -- the child's faults and syscalls are served on a
	 * channel of its own, not the parent's, whether it shares the address
	 * space (a thread) or has its own (a process).
	 */
	sock = home_connect();
	if (sock < 0)
		execution_start_failed(ctx_id(cpid), "connecting child service to source");

	/*
	 * Announce the child to the source, which already made it by running the
	 * real clone. This only names it: a[0] is this side's context id for the
	 * child, a[1] is the source's pid for it from the clone reply. No layout,
	 * no clone flags, no ferried stack/tls/ctid -- the child already exists,
	 * correctly, by the rules of the machine that owns it.
	 */
	a[0] = (uint64_t)ctx_id(cpid);
	a[1] = srcpid;
	if (home_call(VMR_OP_CHILD, a, NULL, 0, NULL, 0) < 0) {
		fprintf(stderr, "[vmremote] child %d: home refused a monitor\n",
			ctx_id(cpid));
		execution_start_failed(ctx_id(cpid), "binding child service at source");
	}
	fprintf(stderr, "[vmremote] child context %d: own monitor at home "
		"(source pid %llu)\n", ctx_id(cpid), (unsigned long long)srcpid);

	/* The permanent owner attached before this context was published.
	 * Service threads of that same local monitor process are authorized.
	 * Keep entry gated until the source's complete CPU state is installed. */
	{
		struct vmctx_reply go = { .action = VMCTX_ACT_SELF };
		struct vmr_cpu_state cpu;
		struct vmr_mm_binding binding;
		long r = source_cpu_ready(ctx_id(cpid), a, &cpu,&binding);
		if (r == VMR_CPU_ENDED) {
			src_ended_note(ctx_id(cpid), home_ended_status);
			ctx_signal(ctx_id(cpid), SIGKILL);
			goto child_finished;
		}
		if (home_call_failed || r < 0 || (uint64_t)r != home_data_len ||
		    !vmr_binding_equal(&born,&binding) || binding.context!=srcpid ||
		    ctx_bind_source(ctx_id(cpid),&binding) ||
		    vmr_cpu_wire_decode(&cpu, home_data_len) ||
		    cpu_state_ctl(cpid, VMCTX_CTL_CPU_MODEL, &guest_cpu_model) < 0 ||
		    cpu_state_ctl(cpid, VMCTX_CTL_SETCPU, &cpu) < 0 ||
		    execution_arm(cpid) < 0) {
			fprintf(stderr, "[vmremote] child %d: CPU state unavailable (%ld, %s)\n",
				ctx_id(cpid), r, strerror(errno));
			execution_start_failed(ctx_id(cpid), "installing child CPU state");
		}
		if (ctl(cpid, VMCTX_CTL_RESUME, &go) < 0)
			execution_start_failed(ctx_id(cpid), "releasing child entry gate");
	}

	service_context(id);
child_finished:
	cpid=ctx_target(id);
	{
		uint64_t f[6] = { 0 };

		/*
		 * A context this monitor created is one of its own children, and
		 * VMCTX_FLAG_EXIT_PROCESS makes the process exit with the
		 * guest's status -- so reaping it yields exactly the status the
		 * guest asked for, and home can give its shadow the same one.
		 * Without this the shadow is SIGKILLed and the guest's parent is
		 * told its child was killed when it exited normally.
		 */
		if (src_clone_parent(ctx_id(cpid))) {
			int st = 0;

			if (ctx_wait(ctx_id(cpid), &st) != ctx_id(cpid))
				execution_start_failed(ctx_id(cpid),"reaping the retained execution task");
			{
				unsigned sst;
				int by_source = src_ended_get(ctx_id(cpid), &sst);

				if (by_source)
					st = (int)sst;
				f[0] = 1;
				f[1] = (uint64_t)(uint32_t)st;
				fprintf(stderr, "[vmremote] context %d ended with "
					"%s %d%s; telling home\n", ctx_id(cpid),
					WIFEXITED(st) ? "exit status" : "signal",
					WIFEXITED(st) ? WEXITSTATUS(st) : WTERMSIG(st),
					by_source ? " (its process died on the "
					"source; ended here on its word)" : "");
				/*
				 * WHO ELSE OF THIS ADDRESS SPACE IS STILL
				 * RUNNING, and it is a rule rather than a
				 * statistic.
				 *
				 * A context that ends by making the exit
				 * syscall ends ITSELF: the guest asked, and at
				 * that moment nothing of it is executing, so the
				 * owner can tear it down without asking anyone.
				 * A context that is TERMINATED -- by the fault
				 * deadline, by a signal, by this side giving up
				 * -- ends while the program may still be running
				 * elsewhere, and then the owner has to stop the
				 * rest and reclaim before it can tear anything
				 * down.
				 *
				 * The two are indistinguishable in the line
				 * above: 242 and 0 are both "exit status". So
				 * this counts only the terminations -- a
				 * non-zero status or a signal -- that happen
				 * while the address space is still running,
				 * which is the state the protocol has no word
				 * for. A clean status 0 with siblings alive is
				 * an ordinary thread finishing and is not it.
				 *
				 * The comment below this one asserts that "the
				 * guest's own exit goes through exit_group,
				 * which ends every context". On THIS side that
				 * is not true and cannot be: each guest thread
				 * is a separate process here, so exit_group ends
				 * only the context that issued it.
				 */
				{
					memory_target sib[MAX_CTX];
					int k, nsib, live = 0;

					nsib = as_members(cpid, sib, MAX_CTX);
					for (k = 0; k < nsib; k++)
						if (ctx_id(sib[k]) != ctx_id(cpid) &&
						    task_alive(sib[k]))
							live++;
					if (live && !(WIFEXITED(st) &&
						      WEXITSTATUS(st) == 0)) {
						n_end_with_live_siblings++;
						if (n_end_with_live_siblings <= 16)
							fprintf(stderr,
								"[vmremote] ...and it was TERMINATED "
								"(%s %d) with %d context(s) of its "
								"address space still running: the "
								"owner was never asked to stop them "
								"or to reclaim what this one held\n",
								WIFEXITED(st) ? "status" : "signal",
								WIFEXITED(st) ? WEXITSTATUS(st)
									      : WTERMSIG(st),
								live);
					}
				}
				/*
				 * A context that ends badly while the program
				 * carries on.
				 *
				 * The guest's own exit goes through exit_group,
				 * which ends every context; one context ending
				 * on its own with a bad status is a thread that
				 * *died* -- on a fault this side could not
				 * service, which the kernel reports as -EFAULT
				 * and which arrives here as 242.
				 *
				 * Nothing downstream notices. The shadow's
				 * serving thread ends, sh_thread_gone() clears
				 * the clone's child-tid word and wakes the
				 * futex, and the joining thread returns exactly
				 * as if the thread had finished its work. th9
				 * loses nine of its forty lines that way and
				 * still exits zero; only a test that counts its
				 * own output sees anything at all.
				 *
				 * Waking the joiner is right -- it would spin
				 * for ever otherwise, which is the bug that code
				 * exists to fix. What is wrong is that the run
				 * then reports success, so record it and let the
				 * exit status say so.
				 */
				/*
				 * ...and only a THREAD's. A context that LEADS
				 * its address space is a forked PROCESS, and a
				 * process's exit status is the program's own
				 * behaviour: the source delivers it to the
				 * parent's wait4, the guest handles it, and a
				 * child that exits 7 on purpose must not become
				 * the run's exit code -- measured as python's
				 * os.fork + _exit(7): guest stdout right, main
				 * context exiting 0, and the run still
				 * reporting 7 (progs.sh pyfork). A thread is a
				 * context that JOINED an existing space, and
				 * its death is the silent kind this record
				 * exists for.
				 */
				if ((!WIFEXITED(st) || WEXITSTATUS(st) != 0) &&
				    born_shared && as_id(cpid)==born.mm && !by_source) {
					n_ctx_died++;
					if (worst_ctx_status == 0)
						worst_ctx_status =
							WIFEXITED(st)
							? WEXITSTATUS(st)
							: 128 + WTERMSIG(st);
				}
			}
		}
		home_call(VMR_OP_FINISH, f, NULL, 0, NULL, 0);
	}
	close(sock);
	fprintf(stderr, "[vmremote] forked context %d done\n", ctx_id(cpid));
	pthread_mutex_lock(&child_lifetime_lock);
	if (!n_live_children) abort();
	n_live_children--;
	pthread_cond_broadcast(&child_lifetime_changed);
	pthread_mutex_unlock(&child_lifetime_lock);
	return NULL;
}

/*
 * Is this task still there to be serviced? Existing is not enough — a task that
 * has exited stays in /proc as a zombie until it is reaped, and waiting on one
 * of those would never end.
 */
static int task_alive(memory_target pid)
{
	return !ctx_execution_ended(ctx_id(pid),0);
}

/*
 * Service one context until it exits: answer its faults from home, forward the
 * syscalls that belong to home, and let the local kernel have the rest.
 *
 * A guest that forks becomes several contexts, each serviced by its own thread
 * with its own connection to home — so each one's syscalls run against its own
 * address space there.
 */
/*
 * The guest's own report of a lost store, photographed at the instant.
 *
 * tests/hx2 detects a loss by reading its slot straight back; the trail says
 * what the machinery did to the page but not what each CONTEXT was mapping
 * when the writer read the wrong value -- two members mapping two physical
 * pages is a split brain no per-page ring can show. So the guest, on a loss,
 * makes a getppid(2) with a marker in its first argument and the page, writer,
 * read and wanted values in the rest; this side sees every syscall before
 * forwarding it and answers the marker with this photograph: every member's
 * present bit and PFN for the page, the writer's slot as each member and the
 * object read it, the records, and the page's trail. getppid itself is then
 * forwarded as usual. Costs nothing until a loss has already happened.
 */
#define VMR_LOSS_MARK 0x4c4f5353ULL	/* "LOSS" */
static unsigned long n_loss_probe;

/*
 * INSTRUMENT (mf2, session 26): the provenance of THIS context's copy of a
 * page. A forked child is supposed to read its OWN object's folio, given to
 * it by cow_give_page() as the original's bytes as of the fork. The child's
 * death frame said otherwise -- its stack page carried the parent's post-fork
 * frames -- so this photographs, at the moments that matter, which physical
 * page each side maps and what each object holds: this context's PFN, its
 * object's sum and watched word, and for an address space copied from
 * another every member of the original's PFN and the original object's
 * bytes. A PFN the child shares with the original is two address spaces on
 * one folio, which is the one state a fork may never leave behind. Pages are
 * peeked only where the pagemap says they are present: a peek of an absent
 * page is a fault, and a fault on an object hole allocates the zero folio it
 * then reports (NOTES 4.2, trace_zero).
 */
static uint64_t pagemap_word(memory_target pid, uint64_t page)
{
	uint64_t ent=0;
	(void)ctx_pagemap_read(pid,page,&ent);
	return ent;
}

static void page_provenance(memory_target pid, uint64_t page, const char *why)
{
	static __thread char buf[VMR_PG_SIZE];
	memory_target mem[MAX_CTX];
	uint64_t ent, mypfn, par;
	int nm, mi, bf;

	page &= ~0xfffULL;
	ent = pagemap_word(pid, page);
	mypfn = (ent >> 63) ? (ent & ((1ULL << 55) - 1)) : 0;
	fprintf(stderr, "[vmremote] PROVENANCE %s: 0x%llx in ctx %d (as %llu) "
		"abs=%llu: present=%d pfn=0x%llx", why, (unsigned long long)page,
		ctx_id(pid), (unsigned long long)as_id(pid), (unsigned long long)now_us(),
		(int)((ent >> 63) & 1), (unsigned long long)mypfn);
	if ((ent >> 63) && peek(pid, page, buf, VMR_PG_SIZE) == (long)VMR_PG_SIZE)
		fprintf(stderr, " mapped sum=%08x%s", page_sum(buf), pgword(buf));
	bf = backing_find(pid);
	if (bf >= 0 && backing_has(pid, page) &&
	    pread(bf, buf, VMR_PG_SIZE, (off_t)page) == (ssize_t)VMR_PG_SIZE)
		fprintf(stderr, "; own object fd %d sum=%08x%s\n", bf,
			page_sum(buf), pgword(buf));
	else
		fprintf(stderr, "; own object fd %d: hole\n", bf);
	par = forked_from(as_id(pid));
	if (!par)
		return;
	nm = mm_members(par, mem, MAX_CTX);
	for (mi = 0; mi < nm; mi++) {
		uint64_t e2 = pagemap_word(mem[mi], page);
		uint64_t pfn = (e2 >> 63) ? (e2 & ((1ULL << 55) - 1)) : 0;

		fprintf(stderr, "[vmremote]   original as %llu member %d: present=%d "
			"pfn=0x%llx%s", (unsigned long long)par, ctx_id(mem[mi]),
			(int)((e2 >> 63) & 1), (unsigned long long)pfn,
			(mypfn && pfn == mypfn) ?
			" <- THE SAME FOLIO AS THE COPY" : "");
		if ((e2 >> 63) &&
		    peek(mem[mi], page, buf, VMR_PG_SIZE) == (long)VMR_PG_SIZE)
			fprintf(stderr, " sum=%08x%s", page_sum(buf), pgword(buf));
		fprintf(stderr, "\n");
	}
	bf = mm_backing_find(par);
	if (bf >= 0 && mm_backing_has(par, page) &&
	    pread(bf, buf, VMR_PG_SIZE, (off_t)page) == (ssize_t)VMR_PG_SIZE)
		fprintf(stderr, "[vmremote]   original's object fd %d sum=%08x%s\n",
			bf, page_sum(buf), pgword(buf));
	else
		fprintf(stderr, "[vmremote]   original's object fd %d: hole\n", bf);
}

/*
 * INVARIANT CHECK: the monitor's own mm holds no mapping of any guest object.
 * Every context is clone()d from this process without CLONE_VM, so whatever
 * this mm maps at a guest address becomes the context's -- and for a fork
 * child that is its PARENT's object at its own addresses. Scans /proc/self/maps
 * for the objects' names; counts, and names the first few. Cheap, and only at
 * a spawn.
 */
static unsigned long n_monitor_guest_vmas;

static int monitor_mm_check(const char *when)
{
	int count = monitor_guest_maps(own_fd(), stderr);
	if (!count) return 0;
	if (count > 0) {
		__atomic_add_fetch(&n_monitor_guest_vmas, count, __ATOMIC_RELAXED);
		errno = EUCLEAN;
	}
	fprintf(stderr, "[vmremote] monitor mapping check failed %s: %s\n",
		when, strerror(errno));
	return -1;
}

static void loss_probe(memory_target pid, uint64_t page, uint64_t writer, uint64_t got,
		       uint64_t want)
{
	memory_target mem[MAX_CTX];
	int nm, mi, bf;
	uint64_t slot = (page & ~0xfffULL) + 64 + (writer & 7) * 8, obj_word = 0;
	static __thread char obj[VMR_PG_SIZE];
	uint32_t rg = 0;
	int have;

	page &= ~0xfffULL;
	n_loss_probe++;
	fprintf(stderr, "[vmremote] LOSS-PROBE abs=%llu 0x%llx ctx %d (as %d): "
		"writer %llu stored %llu read %llu; count %u, lent %d holder %llu, "
		"pull active %d, watched %d, installed %d\n",
		(unsigned long long)now_us(), (unsigned long long)page, ctx_id(pid),
		(int)as_id(pid), (unsigned long long)writer,
		(unsigned long long)want, (unsigned long long)got,
		pgen_get(pid, page), page_lent_state(pid, page),
		(unsigned long long)page_holder(pid, page), pull_active(page),
		page_watched(pid, page), page_is_installed(pid, page));
	nm = as_members(pid, mem, MAX_CTX);
	for (mi = 0; mi < nm; mi++) {
		uint64_t ent = 0, w = 0;
		int pres=-1,pk;
		if(!ctx_pagemap_read(mem[mi],page,&ent))
			pres=(int)((ent>>63)&1);

		pk = peek(mem[mi], slot, &w, 8) == 8;
		fprintf(stderr, "[vmremote]   member %d%s: present=%d pfn=0x%llx "
			"slot=%s%llu\n", ctx_id(mem[mi]), mem[mi] == pid ? " (reporter)" : "",
			pres, (unsigned long long)(ent & ((1ULL << 55) - 1)),
			pk ? "" : "?", (unsigned long long)w);
	}
	bf = backing_find(pid);
	if (backing_has(pid, page) && bf >= 0 &&
	    pread(bf, obj, VMR_PG_SIZE, (off_t)page) == (ssize_t)VMR_PG_SIZE) {
		memcpy(&obj_word, obj + 64 + (writer & 7) * 8, 8);
		fprintf(stderr, "[vmremote]   object: has it, sum=%08x slot=%llu\n",
			page_sum(obj), (unsigned long long)obj_word);
	} else {
		fprintf(stderr, "[vmremote]   object: hole\n");
	}
	have = retain_get_gen(pid, page, obj, &rg);
	if (have) {
		memcpy(&obj_word, obj + 64 + (writer & 7) * 8, 8);
		fprintf(stderr, "[vmremote]   retained: gen %u sum=%08x slot=%llu\n",
			rg, page_sum(obj), (unsigned long long)obj_word);
	} else {
		fprintf(stderr, "[vmremote]   retained: none\n");
	}
	trail_dump_page(page);
}

static void service_context(execution_id id)
{
	unsigned long prev_nr = 0;
	int prev_type = -1;
	uint64_t last_addr = 0;
	unsigned long same = 0, last_recalls = 0, wait_esrch = 0;

	if (t_first_entry == 0)
		t_first_entry = t_now();
	for (;;) {
		memory_target pid=ctx_target(id);
		if(!pid) {fputs("vmremote: missing service target\n",stderr);abort();}
		struct vmctx_event ev;
		struct vmctx_reply rep = { .action = VMCTX_ACT_SELF };

		if (traced_page) {
			static int was = -1, nev;
			int now = page_present(pid, traced_page);

			nev++;
			if (now != was) {
				char ml[256];
				FILE *mf;

				fprintf(stderr, "[trace 0x%llx] present %d -> %d "
					"at ev#%d (pid=%d, prev ev type=%d nr=%llu "
					"rip=0x%llx)\n",
					(unsigned long long)traced_page, was, now,
					nev, ctx_id(pid), ev.type,
					(unsigned long long)ev.nr,
					(unsigned long long)ev.rip);
				/* Which VMA covers it now, in the guest itself? */
				mf = ctx_maps_open(pid);
				while (mf && fgets(ml, sizeof(ml), mf)) {
					unsigned long a = 0, b = 0;

					if (sscanf(ml, "%lx-%lx", &a, &b) == 2 &&
					    traced_page >= a && traced_page < b) {
						fprintf(stderr, "[trace] guest VMA: %s", ml);
						break;
					}
				}
				if (mf)
					fclose(mf);
				was = now;
			}
		}
		{
			double tw = t_now();
			long rc = ctl(pid, VMCTX_CTL_WAIT, &ev);
			double d = t_now() - tw;

			t_wait += d;
			if (rc == 0 && d > t_wait_max) {
				t_wait_max = d;
				wait_max_after_nr = prev_nr;
				wait_max_after_type = prev_type;
			}
			if (rc == 0) {
				last_ev_rip = ev.rip;
				if (d > 0.2)
					fprintf(stderr, "[vmremote] context %d: "
						"%.3fs of guest time after %s 0x%lx, "
						"ending at %s 0x%llx\n", ctx_id(pid), d,
						prev_type == VMCTX_EV_SYSCALL ?
						  "syscall" : prev_type == VMCTX_EV_FAULT ?
						  "fault at" : "start", prev_nr,
						ev.type == VMCTX_EV_SYSCALL ?
						  "syscall" : "fault at",
						(unsigned long long)(ev.type ==
						  VMCTX_EV_SYSCALL ? ev.nr :
						  ev.fault_addr));
				prev_nr = ev.type == VMCTX_EV_SYSCALL ?
					(unsigned long)ev.nr :
					(unsigned long)ev.fault_addr;
				prev_type = ev.type;
			}
			if (rc < 0) {
				if (errno == EINTR)
					continue;
				/*
				 * ESRCH does not have to mean the context has
				 * ended. A task that has just been cloned is in
				 * /proc before it has reached its first event
				 * and become something this monitor can wait
				 * on — so a thread that starts promptly gets
				 * ESRCH for a guest that is very much alive.
				 *
				 * Giving up here does not merely abandon it. It
				 * kills it: home ends a shadow with the
				 * connection that claimed it, so returning from
				 * here closes the connection and SIGKILLs a
				 * shadow whose guest is still running. Every
				 * later request to that shadow then blocks
				 * forever, because a dead shadow's channel has
				 * no end-of-file — the other end is held open
				 * by whichever shadow inherited it across a
				 * fork. That is how a sandbox froze the whole
				 * process tree with nothing in any log.
				 *
				 * So the question is whether the *task* is
				 * still there, not whether one call succeeded.
				 */
				/*
				 * EINVAL is the same race one step earlier, and
				 * it has to be treated the same way.
				 *
				 * The kernel answers ESRCH when there is no such
				 * task and when a context is dead, but EINVAL
				 * when the task is there and task->vmctx is not
				 * installed yet -- "not a VM context". A task
				 * that has just been cloned is exactly that: it
				 * is already in /proc, so task_alive() says yes,
				 * and it becomes a context a moment later.
				 *
				 * Only ESRCH was retried, so such a context was
				 * abandoned while its guest was alive, with the
				 * consequences the comment above describes:
				 * the connection closes, home SIGKILLs the
				 * shadow, and the guest task waits in
				 * vmctx_report for a reply that nobody will
				 * ever send. That is a browser whose threads
				 * sit in D for the rest of the run.
				 *
				 * Both cases still require the task to be
				 * alive, so a context that has genuinely
				 * finished ends here as it always did.
				 */
				if ((errno == ESRCH || errno == EINVAL) &&
				    task_alive(pid) && ++wait_esrch <= 2000) {
					if (wait_esrch == 1)
						fprintf(stderr, "[vmremote] "
							"context %d is not ready "
							"to be waited on yet "
							"(%s); it is alive, so "
							"waiting\n", ctx_id(pid),
							strerror(errno));
					usleep(1000);
					continue;
				}
				fprintf(stderr, "[vmremote] context %d: WAIT failed: %s "
					"(task %s, %lu retries)\n", ctx_id(pid),
					strerror(errno),
					task_alive(pid) ? "still alive" : "gone",
					wait_esrch);
				break;
			}
			wait_esrch = 0;
		}
		if (ev.type == VMCTX_EV_FAULT) {
			/* per context: two contexts fault independently */

			/*
			 * A page that keeps faulting because it keeps changing
			 * sides is not stuck — each fault was answered, and the
			 * next one is the other machine having wanted it back.
			 * Only a fault nobody resolved counts towards giving up.
			 *
			 * "Wanted it back" used to mean a recall, and n_recalled
			 * alone was the whole test. There is no recall channel
			 * any more: the source takes a page by asking for it on
			 * the page channel, so every run reports 0 recalled and
			 * this test could never reset. Forty faults on one
			 * address then killed the program however much work it
			 * had done between them.
			 *
			 * pg1 is the case that shows it, and it is not a corner:
			 * its loop is one write(2) per round through a buffer on
			 * its own stack, so every round is exactly one fault on
			 * one page and one hand-over of that page to the source.
			 * Measured, it died at round 43 of 400 with "giving up
			 * on 0x7fffffffe968" and a monotonic, untorn stamp --
			 * the coherence it was written to test was working, and
			 * the watchdog killed it for doing what it was built to
			 * do.
			 *
			 * n_pg_get is what "the other machine wanted it back"
			 * means now: a page went out on the page channel. A
			 * genuinely stuck fault serves nothing to the source
			 * between one fault and the next, so it still counts.
			 */
			/*
			 * A pull counts as progress too, and leaving it out is
			 * the same defect this test was already fixed for once.
			 *
			 * n_pg_get is a page SERVED to the source. It resets the
			 * count when the far side keeps wanting the page back.
			 * But a context whose page the source is holding does the
			 * opposite -- it PULLS, forty times, each one delivering
			 * a page and each one real work -- and none of that
			 * touched this test, so the watchdog killed it anyway.
			 * Measured on netsurf: two contexts of one address space
			 * on the guest's main stack page, one write-faulting and
			 * granting, the other read-faulting and pulling while the
			 * source held it; the puller was ended at 40 with the
			 * page changing hands the whole time.
			 *
			 * n_pages_faulted is a page installed from a pull.
			 *
			 * The yields belong here for a stronger reason: they are
			 * not an unresolved fault at all, they are this side
			 * answering "not yet, ask again" on purpose -- the source
			 * is mid-claim (claim_yield) or a hand-over is in flight
			 * and the page state is transient (n_handoff_yield).
			 * Each one deliberately re-faults the guest, so forty of
			 * them arrive in about eight milliseconds and the
			 * watchdog ended the context for doing exactly what it
			 * was designed to do. That is netsurf's remaining exit
			 * 131: the guest's main stack page, wanted by its own
			 * threads and by every forwarded syscall at once.
			 *
			 * A fault woken by a sibling's landing on the ownership
			 * channel (n_ownf_woken) is the same kind of deliberate
			 * re-fault: it was told to try again, not left unanswered.
			 *
			 * So "nothing served, nothing fetched, and nobody asked
			 * us to try again" is what now means stuck -- a fault
			 * nobody resolved, which is the only thing this was ever
			 * meant to catch.
			 */
			if (n_recalled + n_pg_get + n_pages_faulted +
			    n_claim_yield + n_handoff_yield + n_ownf_woken !=
			    last_recalls) {
				last_recalls = n_recalled + n_pg_get +
					       n_pages_faulted + n_claim_yield +
					       n_handoff_yield + n_ownf_woken;
				same = 0;
				last_addr = ev.fault_addr & ~0xfffUL;
			}
			if ((ev.fault_addr & ~0xfffUL) == last_addr) {
				if (++same == 20) {
					fprintf(stderr,
						"[vmremote] STUCK: 0x%llx faulting repeatedly, err=0x%llx rip=0x%llx region=%s\n",
						(unsigned long long)ev.fault_addr,
						(unsigned long long)ev.fault_err,
						(unsigned long long)ev.rip,
						region_snapshot(pid, ev.fault_addr, NULL) ?
						  "the source" : "none");
					explain_stuck(pid, ev.fault_addr, ev.fault_err);
					/*
					 * Arm the page log on the stuck page for
					 * the twenty retries left before the
					 * kill. The stuck address differs every
					 * run, so an env-set VMR_PGLOG can never
					 * be aimed at it in advance; the fault
					 * path's own SERVED-BY tags then name
					 * the branch that keeps answering this
					 * fault without the page appearing --
					 * which is the whole question.
					 */
					if (!pglog_on) {
						pglog_page = last_addr;
						pglog_on = 2;
						fprintf(stderr, "[vmremote] page "
							"log armed on 0x%llx for "
							"the remaining retries\n",
							(unsigned long long)
							last_addr);
					}
					fflush(stderr);
				}
				if (same > 40) {
					fprintf(stderr, "[vmremote] giving up on 0x%llx\n",
						(unsigned long long)ev.fault_addr);
					rep.action = VMCTX_ACT_KILL;
					ctl(pid, VMCTX_CTL_RESUME, &rep);
					break;
				}
			} else {
				last_addr = ev.fault_addr & ~0xfffUL;
				same = 0;
			}
			if (trace_fault_state) {
				static unsigned long shown;

				if (shown++ < 40)
					fprintf(stderr, "[vmremote] fault 0x%llx err 0x%llx: "
						"%s mapped=%d present=%d writable=%d vmaprot=0x%llx\n",
						(unsigned long long)ev.fault_addr,
						(unsigned long long)ev.fault_err,
						(ev.args[0] & VMCTX_PGS_UNKNOWN) ? "UNKNOWN" : "known",
						!!(ev.args[0] & VMCTX_PGS_MAPPED),
						!!(ev.args[0] & VMCTX_PGS_PRESENT),
						!!(ev.args[0] & VMCTX_PGS_WRITABLE),
						(unsigned long long)ev.args[1]);
			}
			watch_check(pid, "before servicing fault");
			{
				double t0 = t_now();

				fault_map.op = 0;
				fault_no_invent = 0;
				fresh_object_fill = 0;
				fault_pgs = (uint32_t)ev.args[0] |
					    FAULT_PGS_VALID;
				fault_vmaprot = (unsigned int)ev.args[1];
				if (fault_from_home(pid, ev.nr, ev.fault_addr, ev.fault_err))
					rep.action = VMCTX_ACT_DONE;
				else if (fault_no_invent &&
					 fatal_retained_rescue(pid, fault_no_invent_at)) {
					/* Real bytes went in; the access retries. */
					rep.action = VMCTX_ACT_DONE;
					fault_no_invent = 0;
				} else if (fault_no_invent) {
					/*
					 * UNRECOVERABLE. Not a decline, not a
					 * warning: the program ends here.
					 *
					 * The record says these bytes exist --
					 * this side installed the page, or lent
					 * it to a shadow, or holds it as the
					 * guest's -- and neither machine can
					 * produce them. Declining hands the
					 * fault to THIS kernel, which has no
					 * such bytes and supplies a fresh page
					 * of zeros; and because the guest has a
					 * mapping at that address it does not
					 * even fault again. It reads the zeros
					 * as its own memory, for ever, and
					 * nothing downstream can tell that it
					 * did. That is the one failure this
					 * design must never have (PRINCIPLES
					 * §3: memory that exists somewhere is
					 * never invented here).
					 *
					 * The measured objection to ending the
					 * context -- suite 65/13, sm1 sm2 sm3
					 * rd1 -- was against the WIDE rule,
					 * "known here", which counted a
					 * MAP_SHARED page whose bytes are in an
					 * object both machines map. For those
					 * the local kernel IS the legitimate
					 * source and declining invents nothing.
					 * declined_at() now asks the object's
					 * own SEEK_DATA and clears `known` for
					 * exactly that case, so what reaches
					 * here is the narrow set the old comment
					 * said was "not yet written": a PRIVATE
					 * page whose bytes exist only on the
					 * other machine.
					 *
					 * Nothing the program printed after
					 * such a page was served is evidence of
					 * anything, so it is not left running to
					 * print more.
					 */
					fflush(stdout);
					fprintf(stderr,
"[vmremote] ============================================================\n"
"[vmremote] FATAL: A PAGE OF THE GUEST'S MEMORY IS ON NEITHER MACHINE.\n"
"[vmremote] ============================================================\n"
"[vmremote] FATAL:   page      0x%llx\n"
"[vmremote] FATAL:   fault     0x%llx (err 0x%llx)\n"
"[vmremote] FATAL:   context   %d\n"
"[vmremote] FATAL:\n"
"[vmremote] FATAL: This side's record says those bytes exist. The source\n"
"[vmremote] FATAL: cannot produce them. Handing the fault to this kernel\n"
"[vmremote] FATAL: would answer a mapped address with zeros the guest\n"
"[vmremote] FATAL: cannot tell from its own memory -- silently, and for\n"
"[vmremote] FATAL: the rest of the run. The program is killed instead.\n"
"[vmremote] ============================================================\n",
						(unsigned long long)fault_no_invent_at,
						(unsigned long long)ev.fault_addr,
						(unsigned long long)ev.fault_err,
						ctx_id(pid));
					fflush(stderr);
					/*
					 * ROOT-CAUSE DUMP. The page is "known"
					 * here (installed or lent) yet on neither
					 * machine. Which decline brought it here,
					 * and what does the WHOLE address space
					 * look like at this instant -- who is
					 * alive, who holds the page, and does the
					 * owner think the address space has ended?
					 * This is the discriminator between "a
					 * sibling exists but the take/lookup
					 * missed it" and "the address space really
					 * ended under a still-faulting context".
					 */
					{
						memory_target mem[MAX_CTX];
						int mn = as_members(pid, mem, MAX_CTX), mi, liv = 0;
						uint64_t pg = fault_no_invent_at;

						fprintf(stderr, "[vmremote] FATAL-DUMP: decl-why=%s inst=%d lent=%d "
							"holder=%llu as=%d members=%d present-any=%d\n",
							fault_no_invent_why ? fault_no_invent_why : "(none)",
							fault_no_invent_inst, fault_no_invent_lent,
							(unsigned long long)page_holder(pid, pg),
							(int)as_id(pid), mn, as_any_present(pid, pg));
						for (mi = 0; mi < mn; mi++) {
							int al = task_alive(mem[mi]);

							liv += al;
							fprintf(stderr, "[vmremote] FATAL-DUMP:   ctx %d alive=%d present=%d installed=%d lent=%d\n",
								ctx_id(mem[mi]), al,
								page_present(mem[mi], pg),
								page_is_installed(mem[mi], pg),
								page_lent_state(mem[mi], pg));
						}
						fprintf(stderr, "[vmremote] FATAL-DUMP: live-members=%d (if >1 a sibling could have served it; "
							"if the holder is a DEAD member the page died with it)\n", liv);
						fflush(stderr);
					}
					n_fatal_invent++;
					{
						ctx_stop_all();
						ctx_signal(ctx_id(pid), SIGKILL);
						fflush(NULL);
						_exit(VMR_EXIT_INVENTED_PAGE);
					}
				}
				if (fault_map.op) {
					rep.map_op   = fault_map.op;
					rep.map_addr = fault_map.addr;
					rep.map_len  = fault_map.len;
					rep.map_off  = fault_map.off;
					rep.map_prot = fault_map.prot;
					fault_map.op = 0;
				}
				fault_pgs = 0;
				t_fault += t_now() - t0;
				n_fault_ev++;
			}
			watch_check(pid, "after servicing fault");

			/* Every landed-page receipt must be confirmed before this
			 * native event authorizes execution again. */
			ack_flush();
			if (ctl(pid, VMCTX_CTL_RESUME, &rep) < 0)
				break;
			continue;
		}
		if (ev.type == VMCTX_EV_RUNTIME) {
			if (!forward(&pid, &ev, &rep, &n_runtime)) break;
			if (rep.action != VMCTX_ACT_DONE)
				fwd_broken(pid, &ev, "invalid execution checkpoint reply");
		}
		if (ev.type == VMCTX_EV_SYSCALL) {
			int fwd;

			__atomic_add_fetch(&n_sys, 1, __ATOMIC_RELAXED);
			/*
			 * A program's identity comes off its initial stack:
			 * argv[0] and the auxv AT_EXECFN. If those describe
			 * this machine rather than the one the program was
			 * loaded on, the stack was not transplanted, and the
			 * program will look for its own files in the wrong
			 * place. Report it once, when the stack is paged in.
			 */
			/*
			 * With the context, because several run at once and an
			 * interleaved stream cannot be compared against a
			 * native strace without knowing whose call each is.
			 */
			/*
			 * There used to be a block here that read the string at
			 * execve's first argument and logged it as the program
			 * being run. It acted on nothing -- it was a diagnostic
			 * -- but it was still this side deciding that a guest
			 * pointer is a path, which is a fact about the source's
			 * operating system and not about executing instructions.
			 * On a destination that is not Linux it means nothing at
			 * all. The owner knows what it exec'd and can say so.
			 */
			if (syslog_all)
				fprintf(stderr, "[vmremote] %d syscall %llu "
					"args %llx %llx %llx\n", ctx_id(pid),
					(unsigned long long)ev.nr,
					(unsigned long long)ev.args[0],
					(unsigned long long)ev.args[1],
					(unsigned long long)ev.args[2]);
			if (!stack_dumped) {
				uint64_t argc = 0, ptr = 0;
				char sbuf[64] = { 0 };

				stack_dumped = 1;
				peek(pid, child_stack, &argc, 8);
				peek(pid, child_stack + 8, &ptr, 8);
				if (ptr)
					peek(pid, ptr, sbuf, sizeof(sbuf) - 1);
				fprintf(stderr, "[vmremote] guest initial stack: "
					"argc=%llu argv[0]=\"%s\"\n",
					(unsigned long long)argc, sbuf);
			}
			/*
			 * Every futex operation, not just the waits.
			 *
			 * A wait that never returns is only half a diagnosis:
			 * the question it raises is whether the WAKE that was
			 * supposed to end it was ever sent at all. Logging only
			 * the waits cannot tell "nobody woke it" from "the wake
			 * went somewhere else", and those have different causes
			 * and different fixes. So the op goes in the line, and
			 * so does the word this side can see.
			 */
			watchword_log("syscall", pid, (long long)ev.nr, ev.rip);
			if (ev.args[0] == VMR_LOSS_MARK)
				loss_probe(pid, ev.args[1], ev.args[2],
					   ev.args[3], ev.args[4]);
			/*
			 * mmap is forwarded like every other syscall.
			 *
			 * There was an arm here that looked at the guest's fd
			 * argument, decided from it that the mapping was
			 * file-backed, rewrote the guest's registers to make it
			 * anonymous, and ran it on this machine -- never
			 * forwarding it at all. Everything else this file has
			 * lost followed from that one decision: having mapped
			 * blank memory here, this side then had to serve the
			 * file's pages itself, so it had to know which file, so
			 * it had to remember what each descriptor was opened as
			 * and ask the source to hold those descriptors open.
			 *
			 * None of it was ever the destination's to know. The
			 * source performs the mmap on the real file, in the
			 * address space the guest believes it is running in, and
			 * the reply says what that did to the address space in
			 * the three words the context understands. What arrives
			 * here is a mapping that already has the file's contents
			 * in it, and pages of it are fetched by address like any
			 * other memory.
			 */

			{
				double t0 = t_now();

				fwd = forward(&pid, &ev, &rep, &n_fwd);
				t_syscall += t_now() - t0;
			}

			if (getenv("VMREMOTE_DEBUG"))
				fprintf(stderr, "[vmremote] nr=%llu a0=0x%llx a1=0x%llx a2=0x%llx a3=0x%llx a4=0x%llx a5=0x%llx -> %s\n",
					(unsigned long long)ev.nr,
					(unsigned long long)ev.args[0],
					(unsigned long long)ev.args[1],
					(unsigned long long)ev.args[2],
					(unsigned long long)ev.args[3],
					(unsigned long long)ev.args[4],
					(unsigned long long)ev.args[5],
					fwd ? "HOME" : "local");
			/*
			 * Belt and braces, and worth the two lines: SELF is the
			 * value the reply CARRIES IN, so any path that reaches
			 * the resume below without having replaced it hands the
			 * guest's syscall to this machine's kernel. fwd_broken()
			 * already ends the program on the failures it can name;
			 * this catches a path that forgets to say anything at
			 * all, which is the shape the original bug had.
			 */
			if (rep.action == VMCTX_ACT_SELF)
				fwd_broken(pid, &ev, "a reply that never "
					   "replaced the incoming SELF action");
		}
		/*
		 * Before the caller runs again: give the same address-space
		 * change to every other context of this address space. See
		 * as_apply_map_siblings(). Ordered first so a sibling can never
		 * run against a mapping the address space has already dropped.
		 */
		if (rep.map_op != VMCTX_MAP_NONE && as_apply_map_siblings(pid, &rep) < 0)
			fwd_broken(pid, &ev, "applying permissions to every execution context");
		if (ctl(pid, VMCTX_CTL_RESUME, &rep) < 0) {
			fprintf(stderr, "[vmremote] context %d: RESUME failed: %s\n",
				ctx_id(pid), strerror(errno));
			break;
		}
		/*
		 * The guest just forked. The child is already running — and
		 * already waiting on its first event, because it inherited this
		 * monitor from the kernel — so pick it up now.
		 */
		/*
		 * Nothing to do here. A context comes into being because the
		 * source asked for one -- spawn_forked_context() answers the
		 * clone -- and never because this monitor noticed a task it
		 * did not know about.
		 *
		 * What used to be here looked for the guest's children in
		 * /proc: a one-shot watcher per fork, plus a standing thread
		 * that rescanned every known context every 20ms forever. Both
		 * are wrong twice over. They adopt contexts nobody asked for,
		 * which is the destination creating something on its own; and
		 * they read /proc, which is a fact about this machine being
		 * Linux and not a fact about the guest.
		 *
		 * They also broke the page channels. That scan opens and closes
		 * a descriptor per context per pass, in the same table the page
		 * sockets live in, while accept() hands out numbers from it --
		 * and a freshly accepted channel would be closed by a scan that
		 * had just taken the same number. The shadow saw only
		 * end-of-file, on a page unrelated to the cause: a fork that
		 * never came back.
		 */
	}

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
	struct vmctx_run_config cfg;

	/*
	 * Asked before anything else, and answered without a target, a module
	 * or a source: verify-build.sh calls this to find out what is actually
	 * deployed here. It has to work on a machine where nothing is set up,
	 * which is exactly when the answer matters.
	 */
	if (argc > 1 && !strcmp(argv[1], "--build-id")) {
		printf("build %s vmctx_run=%d vmctx_ctl=%d\n",
		       VMCTX_SRC_ID, __NR_vmctx_run, __NR_vmctx_ctl);
		return 0;
	}

	const char *trace_offset = getenv("VMR_TRACE_WORD_OFFSET");
	if (trace_offset) {
		char *end;
		unsigned long n = strtoul(trace_offset, &end, 0);
		if (end != trace_offset && !*end && n <= VMR_PG_SIZE - 4)
			trail_word_offset = (unsigned)n;
		if (trail_word_offset < VMR_PG_SIZE) {
			trail_slots = 1u << 20;
			trail = calloc(trail_slots, sizeof(*trail));
			if (!trail) { perror("vmremote: diagnostic trail"); return 2; }
		}
	}
	t_start = t_now();

	int net_argc = 0;
	char **net_argv = NULL;
	static uint64_t init_regs[21];
	static struct vmr_cpu_state init_cpu;
	struct vmr_mm_binding init_binding;
	static char layout[VMR_LAYOUT_MAX];
	char host[128];
	int port = 9999;
	struct sockaddr_in a;
	pid_t native_pid;
	int st = 0, one = 1;

	/* Unbuffered: when a run hangs, the log must already be on disk. */
	setvbuf(stderr, NULL, _IONBF, 0);
	vmctx_deadline_enforce("vmremote", 6000);
	/* And the counters, for the run that is killed rather than finished. */
	signal(SIGTERM, on_term);
	signal(SIGINT, on_term);
	{
		/*
		 * SA_SIGINFO for the faulting address; NOT SA_RESETHAND-only --
		 * the handler _exits, so recursion cannot happen, and an
		 * altstack is deliberately not used: if the crash is a blown
		 * stack the backtrace attempt just dies, which is what would
		 * have happened anyway, with one line more than before.
		 */
		struct sigaction sa = { 0 };

		sa.sa_sigaction = on_crash;
		sa.sa_flags = SA_SIGINFO;
		sigaction(SIGSEGV, &sa, NULL);
		sigaction(SIGBUS, &sa, NULL);
	}

	/*
	 * Where to connect, and nothing else.
	 *
	 * This used to take the program and its arguments and ship the strings
	 * to the source, which execv'd them. It never opened or executed
	 * anything -- it could not -- but a path is a source-side idea, and
	 * putting one in THIS machine's interface asks its operator to name,
	 * on the side with no filesystem the program can reach, something only
	 * the other side can resolve. PRINCIPLES 6a: assume this machine could
	 * not open the program if it tried.
	 *
	 * So the source decides what runs. Trailing arguments are still
	 * accepted and ignored, with a word about it, because every script in
	 * the tree passes them today and a run that dies on its command line
	 * teaches nothing.
	 */
	if (argc < 3 || strcmp(argv[2], "--net")) {
		fprintf(stderr, "usage: %s <home-ip[:port]> --net\n"
			"  The program is named on the SOURCE's command line, "
			"not here: this machine has no files.\n", argv[0]);
		return 2;
	}
	if (argc > 3)
		fprintf(stderr, "[vmremote] ignoring %d trailing argument(s) "
			"starting with \"%s\": the source decides what runs\n",
			argc - 3, argv[3]);
	fprintf(stderr, "[vmremote] build %s; vmctx_run=%d vmctx_ctl=%d\n",
		VMCTX_SRC_ID, __NR_vmctx_run, __NR_vmctx_ctl);
	snprintf(host, sizeof(host), "%s", argv[1]);
	{
		char *c = strchr(host, ':');

		if (c) {
			*c = '\0';
			port = atoi(c + 1);
		}
	}

	fprintf(stderr, "[vmremote] connecting to %s\n", host);
	/*
	 * Refuse a name that does not fit rather than connect to a truncated
	 * one: every page and every syscall of the run would go to whatever
	 * that shorter name resolves to.
	 */
	if (strlen(host) >= sizeof(home_host)) {
		fprintf(stderr, "[vmremote] host name is longer than %zu bytes; "
			"refusing to connect to a truncated one\n",
			sizeof(home_host) - 1);
		return 1;
	}
	snprintf(home_host, sizeof(home_host), "%s", host);
	home_port = port;
	sock = socket(AF_INET, SOCK_STREAM, 0);
	memset(&a, 0, sizeof(a));
	a.sin_family = AF_INET;
	a.sin_port = htons(port);
	if (inet_pton(AF_INET, host, &a.sin_addr) != 1) {
		fprintf(stderr, "vmremote: bad address '%s'\n", host);
		return 1;
	}
	if (connect(sock, (struct sockaddr *)&a, sizeof(a)) < 0) {
		fprintf(stderr, "vmremote: connect %s:%d: %s\n", host, port,
			strerror(errno));
		return 1;
	}
	setsockopt(sock, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
	fprintf(stderr, "[vmremote] connected\n");

	if (getenv("VMREMOTE_CHUNK_DUMPS"))
		chunk_dump_max = (unsigned)atoi(getenv("VMREMOTE_CHUNK_DUMPS"));
	if (getenv("VMREMOTE_TRACE_PUT"))
		trace_put = 1;
	if (getenv("VMREMOTE_TRACE_FAULTS")) {
		trace_all_faults = 1;
		/*
		 * And lift the cap, because the switch says "trace
		 * faults" and the cap is 30. A links run takes 456, so
		 * the trace stopped a fourteenth of the way in and read
		 * as a complete account of the run -- "the guest never
		 * faults on this page" about a page it faults on
		 * normally. That sent one investigation to the wrong
		 * machine entirely. VMREMOTE_CHILD_FAULTS still
		 * overrides, below, for anyone who wants it capped.
		 */
		child_fault_max = ~0UL;
	}
	if (getenv("VMREMOTE_TRACE_AFTER_EXC"))
		trace_after_exc = 1;
	if (getenv("VMREMOTE_FAULT_STATE"))
		trace_fault_state = 1;
	if (getenv("VMREMOTE_CHECK_COHERENCE"))
		check_coherence = 1;
	if (getenv("VMREMOTE_TRACE_CLONE"))
		trace_clone = 1;
	if (getenv("VMREMOTE_SYSLOG"))
		syslog_all = 1;
	if (getenv("VMREMOTE_CHILD_FAULTS"))
		child_fault_max = strtoul(getenv("VMREMOTE_CHILD_FAULTS"), NULL, 0);
	pglog_init();
	map_trace_init();
	coh_lock_init();
	trace_absent = getenv("VMREMOTE_TRACE_ABSENT") != NULL;
	pm_qos_hold("vmremote");
	if (getenv("VMCTX_SOCK_SPIN_US"))
		sock_spin_us = (unsigned)atoi(getenv("VMCTX_SOCK_SPIN_US"));
	if (getenv("VMREMOTE_TRACE_PAGE"))
		traced_page = strtoull(getenv("VMREMOTE_TRACE_PAGE"),
				       NULL, 0) & ~0xfffUL;
	if (getenv("VMREMOTE_WATCH_WORD"))
		watch_word = strtoull(getenv("VMREMOTE_WATCH_WORD"),
				      NULL, 0);
	if (getenv("VMREMOTE_WATCH"))
		watch_addr = strtoull(getenv("VMREMOTE_WATCH"), NULL, 0) &
			     ~(uint64_t)0xfff;
	/* Kept only so the log can say which run this was, never sent. */
	net_argv = (argc > 3) ? &argv[3] : NULL;
	net_argc = (argc > 3) ? argc - 3 : 0;

	memset(&cfg, 0, sizeof(cfg));
	cfg.flags = VMCTX_FLAG_USERCODE | VMCTX_FLAG_EXIT_PROCESS |
		    VMCTX_FLAG_REDIRECT_SYSCALL | VMCTX_FLAG_WAIT_MONITOR |
		    VMCTX_FLAG_REDIRECT_FAULT | VMCTX_FLAG_RESTORE;

	{
		uint64_t a[6] = { 0 };
		char blob[4096];
		pthread_t th;

		/*
		 * The listener must exist before home starts the program: home
		 * creates its shadow during START and connects straight back for
		 * pages. ctx_pid is filled in once the context is attached; no
		 * page can be requested before then, because nothing has run.
		 */
		pthread_create(&th, NULL, pg_thread,
			       (void *)(long)(port + VMR_PG_PORT_OFFSET));
		pthread_detach(th);
		pthread_create(&th, NULL, coh_watch_thread, NULL);
		pthread_detach(th);
		/*
		 * Until the page service is listening (see pg_up): a bounded
		 * wait on the fact, where a 100 ms sleep used to stand in for
		 * it and was every run's whole start-up cost.
		 */
		{
			struct timespec dl;

			clock_gettime(CLOCK_REALTIME, &dl);
			dl.tv_sec += 2;
			pthread_mutex_lock(&pg_up_lock);
			while (!pg_up &&
			       pthread_cond_timedwait(&pg_up_cv, &pg_up_lock,
						      &dl) == 0)
				;
			pthread_mutex_unlock(&pg_up_lock);
		}
		long r;

		/*
		 * No payload. The source knows what it is running; this side is
		 * asking it to start, not telling it what to start.
		 */
		(void)blob;
		if(linux_execution_capabilities()) {
			perror("vmremote: retained execution context capability");return 1;
		}
		struct vmctx_runtime runtime;
		if (execution_runtime(0, VMCTX_RUNTIME_INFO, &runtime) < 0) {
			perror("vmremote: execution adapter CPU-time capability");
			return 1;
		}
		r = home_call(VMR_OP_RUNTIME_CAPS, a, NULL, 0,
			&guest_runtime_quantum, sizeof(guest_runtime_quantum));
		if (home_call_failed || r != sizeof(guest_runtime_quantum) ||
		    home_data_len != sizeof(guest_runtime_quantum) ||
		    guest_runtime_quantum < 1000000 || guest_runtime_quantum > 100000000) {
			fprintf(stderr, "vmremote: source CPU-time capability unavailable\n");
			return 1;
		}
		struct vmctx_quiesce gate = {.version = VMCTX_QUIESCE_ABI,
			.size = sizeof(gate), .op = VMCTX_QUIESCE_INFO};
		if (ctl(0, VMCTX_CTL_QUIESCE, &gate) < 0) {
			perror("vmremote: execution adapter snapshot capability");
			return 1;
		}
		struct vmctx_executor_map_caps map_caps = {0};
		const uint64_t required_maps = VMCTX_EXECUTOR_MAP_PROTECT_MM |
			VMCTX_EXECUTOR_MAP_SHARED_PAGE | VMCTX_EXECUTOR_MAP_READ_OBJECT |
			VMCTX_EXECUTOR_MAP_PRESERVE_WRITE;
		if (ctl(0, VMCTX_CTL_EXECUTOR_MAP_CAPS, &map_caps) < 0) {
			perror("vmremote: execution adapter mapping capabilities");
			return 1;
		}
		if (map_caps.version != VMCTX_EXECUTOR_MAP_ABI ||
		    map_caps.size != sizeof(map_caps) ||
		    (map_caps.features & required_maps) != required_maps) {
			fprintf(stderr, "vmremote: execution adapter lacks required mapping operations\n");
			return 1;
		}
		struct vmctx_cpu_model caps;
		if (ctl(0, VMCTX_CTL_CPU_CAPS, &caps) < 0) {
			perror("vmremote: CPU capabilities"); return 1;
		}
		r = home_call(VMR_OP_CPU_MODEL, a, &caps, sizeof(caps),
			&guest_cpu_model, sizeof(guest_cpu_model));
		if (r != sizeof(guest_cpu_model) ||
		    !vmctx_cpu_model_subset(&guest_cpu_model, &caps)) {
			fprintf(stderr, "[vmremote] CPU negotiation failed (%ld)\n", r);
			return 1;
		}
		fprintf(stderr, "[vmremote] negotiated generic x86: leaf1=%08x/%08x leaf7=%08x xcr0=%llx\n",
			guest_cpu_model.leaf1_ecx, guest_cpu_model.leaf1_edx,
			guest_cpu_model.leaf7_ebx, (unsigned long long)guest_cpu_model.xcr0);
		r = home_call(VMR_OP_START, a, NULL, 0, init_regs,
			      sizeof(init_regs));
		if (r <= 0) {
			fprintf(stderr, "[vmremote] home would not start its "
				"program (%ld)\n", r);
			return 1;
		}
		r = source_cpu_ready(0, a, &init_cpu,&init_binding);
		if (home_call_failed || r < 0 || (uint64_t)r != home_data_len ||
		    vmr_cpu_wire_decode(&init_cpu, home_data_len)) {
			fprintf(stderr, "[vmremote] source supplied no initial CPU state (%ld)\n", r);
			return 1;
		}
		r = home_binding_call(&init_binding, VMR_OP_LAYOUT, a, NULL, 0, layout, sizeof(layout) - 1,NULL);
		if (r <= 0) {
			fprintf(stderr, "[vmremote] no layout from home\n");
			return 1;
		}
		layout[r] = '\0';
		if ((size_t)r >= sizeof(layout) - 1) {
			fprintf(stderr, "[vmremote] FATAL: the oracle's map "
				"filled the %zu-byte layout buffer, so it is "
				"truncated. Raise VMR_LAYOUT_MAX.\n",
				sizeof(layout));
			return 1;
		}
		snprintf(child_layout, sizeof(child_layout), "%s", layout);
		child_entry = init_regs[16];
		child_stack = init_regs[7];
		fprintf(stderr, "[vmremote] home loaded '%s' and stopped it at "
			"rip=0x%llx; nothing about it exists on this machine\n",
			net_argc ? net_argv[0] : "the source's program",
			(unsigned long long)init_regs[16]);
	}

	fprintf(stderr, "[vmremote] running '%s' here; syscalls served by %s:%d\n",
		net_argc ? net_argv[0] : "the source's program", host, port);

	/*
	 * The child must be able to map the guest's stack, which sits where its
	 * own inherited stack does. So give it a stack of our choosing, far from
	 * anything the guest uses, and stop skipping regions.
	 */
	void *cstack = mmap((void *)0x400000000000UL, 1UL << 20,
			    PROT_READ | PROT_WRITE,
			    MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, -1, 0);

	if (cstack == MAP_FAILED) {
		perror("mmap child stack");
		return 1;
	}
	struct execution_mm *root_mm=source_mm_resolve(&init_binding);
	if(!root_mm) {perror("vmremote: retaining root MM");return 1;}
	cfg.backing_fd=root_mm->backing_fd;
	if (shared_obj < 0)
		shared_obj = backing_new("vmctx-shared");
	cfg.shared_fd = shared_obj;
	child_cfg = &cfg;
	native_pid = clone(net_child, (char *)cstack + (1UL << 20), SIGCHLD, NULL);
	if (native_pid < 0) {
		perror("clone");
		return 1;
	}

	for (int i = 0; i < 500; i++) {
		if (monitor_attach(native_pid) == 0)
			goto attached;
		usleep(1000);
	}
	fprintf(stderr, "[vmremote] could not attach: %s\n", strerror(errno));
	kill(native_pid, SIGKILL);
	waitpid(native_pid, NULL, 0);
	return 1;
attached:
	int created;
	execution_id root_id=ctx_register(native_pid,root_mm,&init_binding,&created);
	if(root_id<0 || !created) {
		perror("vmremote: registering root execution context");
		kill(native_pid,SIGKILL);waitpid(native_pid,NULL,0);return 1;
	}
	fprintf(stderr,"[vmremote] execution context %d retains native child %d\n",root_id,native_pid);
	ctx_pid=root_id;
	memory_target pid=ctx_target(root_id);

	/*
	 * The first context's object, so a thread of the first guest can be handed
	 * it like any other.
	 */
	{
		struct vmctx_uregs u;

		/* Every mapping home had is served by home until proven
		 * otherwise; the startup layout is the FIRST context's. */
		region_parse_layout(as_id(pid), layout);
		/*
		 * The [stack] special-case that used to live in this parse -- 8 MiB
		 * below the stack marked LOCAL, answered here with fresh zeros --
		 * is gone: a stack is a label in a text file, not a kind of
		 * memory, and inventing pages is the one thing this side must
		 * never do. The stack-clash probe it existed for is answered
		 * generically by the "no region here" branch, which asks the
		 * source. The parse itself is region_parse_layout() now, shared
		 * with the post-exec relayout.
		 */

		memset(&u, 0, sizeof(u));
		u.rax = init_regs[0];  u.rbx = init_regs[1];  u.rcx = init_regs[2];
		u.rdx = init_regs[3];  u.rsi = init_regs[4];  u.rdi = init_regs[5];
		u.rbp = init_regs[6];  u.rsp = init_regs[7];  u.r8  = init_regs[8];
		u.r9  = init_regs[9];  u.r10 = init_regs[10]; u.r11 = init_regs[11];
		u.r12 = init_regs[12]; u.r13 = init_regs[13]; u.r14 = init_regs[14];
		u.r15 = init_regs[15]; u.rip = init_regs[16];
		u.rflags = init_regs[17] | 0x202;
		u.orig_rax = (uint64_t)-1;
		u.fs_base = init_regs[19];	/* the program's TLS, as loaded */
		u.gs_base = init_regs[20];
		/*
		 * Settled, for the same reason as spawn_forked_context(): this
		 * child too ran a stint of vmremote's own userspace between
		 * clone() and vmctx_run, and a SETREGS that lands before its
		 * first deschedule loses the FS base to the switch's save.
		 */
		memcpy(&init_cpu.regs, &u, sizeof(u));
		if (cpu_state_ctl(pid, VMCTX_CTL_CPU_MODEL, &guest_cpu_model) < 0 ||
		    cpu_state_ctl(pid, VMCTX_CTL_SETCPU, &init_cpu) < 0 ||
		    execution_arm(pid) < 0) {
			perror("SETREGS");
			ctx_signal(ctx_id(pid), SIGKILL);
			return 1;
		}
		{
			struct vmctx_reply go = { .action = VMCTX_ACT_SELF };

			/* Registers are in place: let the guest start. */
			ctl(pid, VMCTX_CTL_RESUME, &go);
		}
		fprintf(stderr, "[vmremote] %d regions mapped empty; guest starts at "
			"0x%llx and will page in from home\n",
			nregions, (unsigned long long)u.rip);
	}
	fprintf(stderr, "[vmremote] context pid %d attached\n", ctx_id(pid));

	service_context(ctx_id(pid));
	t_svc_end = t_now();

	/*
	 * Reap the guest first, then give any forked context a moment. Waiting
	 * on the children before the guest meant that if a child's completion
	 * was ever missed, this process sat here for a full minute after the
	 * guest had already finished — a timeout indistinguishable from the
	 * guest itself hanging, which is exactly how it was misread.
	 */
	pid_t reaped;
	reaped = ctx_wait(ctx_id(pid), &st);
	if (reaped != ctx_id(pid)) { perror("wait execution context"); return 1; }
	/* Termination of the local execution task releases its resources. The
	 * source owns the program's native terminal result. */
	unsigned source_status;
	if (src_ended_get(ctx_id(pid), &source_status)) st = (int)source_status;
	t_reaped = t_now();
	/* A child service owns its count through the final source response.
	 * A reaped guest can still have valid teardown in progress. Wait for
	 * that completion instead of inferring a counter leak from task death. */
	pthread_mutex_lock(&child_lifetime_lock);
	while (n_live_children)
		pthread_cond_wait(&child_lifetime_changed, &child_lifetime_lock);
	pthread_mutex_unlock(&child_lifetime_lock);
	t_kids = t_now();
	{
		uint64_t a[6] = { 0 };

		/*
		 * Tell home how the guest ended. Home's log is the one that
		 * survives — this process's output is over an ssh session that
		 * may be cut short — and "produced no output" reads completely
		 * differently depending on whether the program exited 0 or died.
		 */
		a[0] = 1;
		a[1] = (uint64_t)(uint32_t)st;
		/*
		 * What does the machine that owns this memory think is at an
		 * address? Asked here because the oracle is still alive until
		 * OP_FINISH, and the guest is not — so this is the one moment
		 * the two copies of a byte can both be named. "The guest wrote
		 * this" and "we served this" look identical in a memory dump.
		 */
		if (getenv("VMREMOTE_ORACLE_DUMP")) {
			uint64_t q[6] = { 0 };
			char b[256];
			long got;

			q[0] = strtoull(getenv("VMREMOTE_ORACLE_DUMP"), NULL, 0);
			q[1] = sizeof(b);
			got = home_binding_call(&init_binding, VMR_OP_PAGE, q, NULL, 0, b, sizeof(b),NULL);
			for (long o = 0; o + 64 <= got; o += 64) {
				fprintf(stderr, "[vmremote] oracle 0x%llx: ",
					(unsigned long long)(q[0] + o));
				for (long i = 0; i < 64; i++)
					fprintf(stderr, "%02x ",
						(unsigned char)b[o + i]);
				fprintf(stderr, "\n");
			}
		}
		home_call(VMR_OP_FINISH, a, NULL, 0, NULL, 0);
		t_finished = t_now();
	}

	/* A map of what lives where, so a faulting address means something. */
	fprintf(stderr, "[vmremote] region map:\n");
	for (int i = 0; i < nregions; i++)
		fprintf(stderr, "  0x%012llx-0x%012llx\n",
			(unsigned long long)regions[i].start,
			(unsigned long long)regions[i].end);
	{
		/*
		 * The clobber detector's own numbers, which were counted and
		 * never printed. Its per-site print caps at eight, and a page
		 * of the guest's stack produces eight false positives at once
		 * (the monotonic filter cannot tell a counter from a value that
		 * happens to fall), so by the time a real backward install
		 * happens the site is silent. "No BACKMOVE lines" was therefore
		 * not evidence of anything -- PRINCIPLES §7, absence of evidence
		 * is evidence only if the check can still fire.
		 */
		int bi;
		unsigned long bt = 0;

		for (bi = 0; bi < 16; bi++)
			bt += n_backmove[bi];
		if (bt) {
			fprintf(stderr, "[vmremote] installs that moved a "
				"monotonic slot BACKWARD: %lu, worst by %lu, "
				"by site:", bt, n_backmove_worst);
			for (bi = 0; bi < 16; bi++)
				if (n_backmove[bi])
					fprintf(stderr, " site%d=%lu", bi,
						n_backmove[bi]);
			fprintf(stderr, "\n");
		}
	}
	if (n_recall_via_object)
		fprintf(stderr, "[vmremote] recalls that could not be poked into "
			"the context that asked and went into its address "
			"space's object instead: %lu -- a context that exists "
			"but has not mapped the address yet is ordinary, not "
			"fatal\n", n_recall_via_object);
	if (clobber_page() > 2) {
		int si;

		fprintf(stderr, "[vmremote] answers that served WITHOUT installing, "
			"checked against the guest's live page: %lu; of those %lu "
			"handed back a folio below what had already changed hands "
			"(worst %lu), by site:", n_served_checked, n_served_stale,
			n_served_stale_worst);
		for (si = 0; si < 16; si++)
			if (n_served_stale_site[si])
				fprintf(stderr, " site%d=%lu", si,
					n_served_stale_site[si]);
		fprintf(stderr, "\n");
	}
	fprintf(stderr, "[vmremote] ABSENT/NOTHOLDER answers served by MAPPING this "
		"address space's own folio rather than copying it into the guest: "
		"%lu -- each copy avoided is a read-modify-write of a page the "
		"sibling contexts are writing\n", n_absent_mapped);
	/*
	 * Both halves of the live-page check, printed whenever it is armed --
	 * including their zeros, because a zero here is the statement wanted and
	 * it is worth nothing without the denominator beside it.
	 */
	if (clobber_page() > 2) {
		int li;

		fprintf(stderr, "[vmremote] installs compared against the "
			"guest's LIVE page: %lu; of those %lu would have "
			"discarded a store (worst %lu stores), by site:",
			n_live_checked, n_live_lost, n_live_lost_worst);
		for (li = 0; li < 16; li++)
			if (n_live_lost_site[li])
				fprintf(stderr, " site%d=%lu", li,
					n_live_lost_site[li]);
		fprintf(stderr, "\n");
	}
	if (clobber_page() > 2 || n_hist_watched)
		fprintf(stderr, "[vmremote] content history (%s, %lu page(s) "
			"watched): %lu observation(s) of the guest's own bytes, "
			"%lu hand-back(s) checked, %lu word(s) handed back as the "
			"value from BEFORE the guest's last write%s\n",
			clobber_page() > 2 ? "explicit + arena auto-arm"
					   : "arena auto-arm",
			n_hist_watched, n_hist_notes, n_hist_checks, n_hist_revert,
			n_hist_checks ? "" :
			"  <<< nothing was checked: the denominator is zero");
	/*
	 * The generation check. "stale" is the number the whole stamp exists
	 * for: every one is a serve from the source of a copy older than a
	 * capture this side had already made, named by the site that received
	 * it (31 single-page pull, 32 nocow re-ask, 33 one-page chunk, 34
	 * multi-page chunk). A zero with a zero denominator is not a clean run.
	 */
	fprintf(stderr, "[vmremote] take generation: %lu bump(s), %lu served "
		"page(s) checked against the count, %lu STALE (gen below the "
		"count; by site 31:%lu 32:%lu 33:%lu 34:%lu), %lu repaired from "
		"the retained copy, %lu installed as they came, %lu ahead of the "
		"count, %lu retained copies served older than the count%s\n",
		n_pgen_bumps, n_gen_checked, n_gen_stale,
		n_gen_stale_site[31], n_gen_stale_site[32],
		n_gen_stale_site[33], n_gen_stale_site[34],
		n_gen_repaired, n_gen_unrepaired, n_gen_ahead,
		n_retained_served_old,
		n_gen_checked ? "" :
		"  <<< nothing was checked: the denominator is zero");
	{
		const char *tp = getenv("VMR_TRAIL_PAGE");

		if (tp && *tp)
			trail_dump_page(strtoull(tp, NULL, 0));
	}
	if (n_gen_repaired_obj || n_gen_unrepaired)
		fprintf(stderr, "[vmremote]   ...of the stale serves, %lu were "
			"repaired from this side's OWN live folio (the "
			"write-grant sub-case: newest bytes in the object, not "
			"at the source), %lu had nothing newer anywhere and "
			"installed as they came\n",
			n_gen_repaired_obj, n_gen_unrepaired);
	fprintf(stderr, "[vmremote] mutation order: %lu construction(s) recorded "
		"with their reply's sequence, %lu vacate(s) that arrived after a "
		"newer construction of the same range and were kept off it "
		"(VACATE-LATE, made harmless), %lu punched construction(s) "
		"REPAIRED at the next fault from their own row\n",
		n_cons_noted, n_vacate_late_skipped, n_cons_repaired);
	fprintf(stderr, "[vmremote] pulls that began for a page ALREADY in flight "
		"to another context of the same address space: %lu -- each one is "
		"two copies of one page on the wire and two installs of it\n",
		n_pull_overlap);
	fprintf(stderr, "[vmremote] PEEKs answered SHORT of the present page they "
		"were asked for (the page left between the pagemap read and the "
		"PEEK, or the kernel refused to fault a hole in): %lu -- each one "
		"used to be served as a full page of whatever the buffer held\n",
		n_peek_short);
	/*
	 * The shared-memory ownership channel, load-bearing since session 35:
	 * one transition per real guest fault, a sibling BLOCKS on the landing
	 * instead of fetching a second copy. `waited` is the receiving-side race
	 * the channel absorbs (session 34 measured ~12.6k per hx2 pool as BUSY);
	 * `wait_timeout` is the lost-lander diagnosis and must be ~0 -- each one
	 * ran a fault ungated, the old racing path. own_seg's own counters
	 * (begin/land/wait) are the substrate's; these are what the fault path saw.
	 */
	fprintf(stderr, "[vmremote] ownership channel (fault claim): %lu fault(s) "
		"claimed the (as,page) service, %lu found a sibling's service in "
		"flight and WAITED on the landing, %lu of those woken by it (%lu "
		"woke to a page still absent), %lu outlived the 3s bound and ran "
		"UNGATED (must be ~0), %lu entry-table full (ungated); released: "
		"%lu landed with the page present, %lu aborted still absent\n",
		n_ownf_claimed, n_ownf_waited, n_ownf_woken, n_ownf_wake_absent,
		n_ownf_wait_timeout, n_ownf_full, n_ownf_landed, n_ownf_aborted);
	if (own_seg)
		fprintf(stderr, "[vmremote] ownership channel (substrate): begin=%llu "
			"begin_busy=%llu land=%llu wait=%llu woken=%llu timeout=%llu "
			"full=%llu\n",
			(unsigned long long)own_seg->n_begin,
			(unsigned long long)own_seg->n_begin_busy,
			(unsigned long long)own_seg->n_land,
			(unsigned long long)own_seg->n_wait,
			(unsigned long long)own_seg->n_wait_woken,
			(unsigned long long)own_seg->n_wait_timeout,
			(unsigned long long)own_seg->n_full);
	fprintf(stderr, "[vmremote] fork copy-on-write, executed here: %lu break(s) "
		"asked for by the source on a fault, %lu page(s) actually copied "
		"to an address space that was copied from another, %lu asked for "
		"where this side had no such page either (and invented nothing); "
		"%lu write(s) reported to the source before they landed, %lu of "
		"which it held up for a copy; %lu address space(s) write-protected "
		"at a fork over %lu page(s), %lu of which could not be protected\n",
		n_cow_break_asked, n_cow_given, n_cow_break_nopage,
		n_cow_writeprep, n_cow_writeprep_break, n_cow_protect_runs,
		n_cow_protect_pages, n_cow_protect_fail);
	if (n_cow_give_failed)
		fprintf(stderr, "[vmremote] ...and %lu break(s) that could not be "
			"performed before the original's store: the copy is owed "
			"a page this side could not produce\n", n_cow_give_failed);
	if (n_cow_protect_unmapped)
		fprintf(stderr, "[vmremote] ...and %lu page(s) the object held "
			"that no context mapped: nothing to write-protect, so "
			"they are recorded owed and the first touch routes\n",
			n_cow_protect_unmapped);
	fprintf(stderr, "[vmremote] ...and %lu page(s) broken straight to the "
		"source, for a copy whose own task there faulted on memory it "
		"inherited\n", n_cow_given_to_home);
	if (n_cow_chain_given)
		fprintf(stderr, "[vmremote] ...and %lu page(s) a copy was given "
			"by an ancestor ABOVE its parent, which never held them "
			"(a fork of a fork inherits through the chain)\n",
			n_cow_chain_given);
	if (n_cow_relaxed_given)
		fprintf(stderr, "[vmremote] ...and %lu page(s) given LAST-RESORT "
			"from the nearest READABLE ancestor, no protect mark -- "
			"the asking machine's own routes had all failed and a "
			"fork copy is never owed zeros\n", n_cow_relaxed_given);
	if (n_get_chain_given || n_cow_own_served)
		fprintf(stderr, "[vmremote] ...and %lu GET(s) for a fork copy "
			"answered by giving from its chain on the spot, %lu "
			"break request(s) answered with the copy's OWN page "
			"(the give had landed between the source's GET and "
			"its break) -- each would have been ABSENT and zeros\n",
			n_get_chain_given, n_cow_own_served);
	fprintf(stderr, "[vmremote] chunk fetches the source answered NOTHOLDER "
		"(it had already handed the page over): %lu, of which %lu no "
		"machine could produce\n", n_src_notholder, n_notholder_lost);
	fprintf(stderr, "[vmremote] GETs for a page already handed to the asker, "
		"answered with its own copy instead of a take: %lu; %lu more the "
		"record called handed-over while a guest context HAD it present "
		"(stale loan) -- the guest's current bytes were served, not the "
		"retained copy\n",
		n_get_already_theirs, n_get_theirs_but_present);
	fprintf(stderr, "[vmremote] GETs where the page left this side between "
		"the read and the take: %lu -- never served their pre-wait read; "
		"%lu answered ABSENT (it left for the asker), %lu answered "
		"INFLIGHT (a pull of it was landing here)\n", n_get_lost_race,
		n_get_lost_race_absent, n_get_lost_race_inflight);
	fprintf(stderr, "[vmremote] GETs answered INFLIGHT because this side's own "
		"pull of the page was in flight past the 500ms wait (the record "
		"said the guest's; ABSENT would have been a lie): %lu\n",
		n_pg_inflight_pull);
	fprintf(stderr, "[vmremote] branch 6 cleared the watch and then could "
		"not make the page writable: %lu\n", n_watch_grant_nowrite);
	fprintf(stderr, "[vmremote] pages write-protected for a GET whose read "
		"then failed, so no loan was recorded for them: %lu\n",
		n_prot_noread);
	if (n_pg_prefill)
		fprintf(stderr, "[vmremote] %lu pages fetched before a partial "
			"write-back that would have created them blank\n",
			n_pg_prefill);
	if (n_refilled)
		fprintf(stderr, "[vmremote] %lu pages refilled after being "
			"discarded by the guest\n", n_refilled);
	if (getenv("VMREMOTE_SWEEP"))
		sweep_unfetched(ctx_target(ctx_pid));
	if (n_shared_moved)
		fprintf(stderr, "[vmremote] %lu shared pages moved between contexts\n",
			n_shared_moved);
	fprintf(stderr, "[vmremote] shared ranges whose object had no page: "
		"%lu; the owner supplied %lu, %lu retried an unresolved transfer\n",
		n_shared_hole, n_shared_filled, n_shared_hole_claim);
	fprintf(stderr, "[vmremote] faults declined to the local kernel: %lu "
		"(no region / local) + %lu protection + %lu already present "
		"(%lu of those unanswerable)\n",
		n_declined, n_decl_prot, n_declined_present,
		n_present_unknown);
	decl_sites_report();
	if (pglog_on)
		fprintf(stderr, "[vmremote] pages taken away that stayed "
			"present here (two holders): %lu\n", n_take_kept);
	if (n_notholder)
		fprintf(stderr, "[vmremote] the source answered \"I handed that "
			"page to you\" %lu time(s): %lu of them a context of "
			"this address space already had (the ordinary case -- a "
			"pull for a page a sibling holds) and %lu were on "
			"NEITHER machine, which is the pair AUDIT XI §5.1 names "
			"and the number it never had\n",
			n_notholder, n_notholder_landed, n_notholder_lost);
	if (n_decl_prot_stale)
		fprintf(stderr, "[vmremote] %lu write fault(s) were on a page "
			"taken away between the fault and its service, and were "
			"retried rather than declined to the local kernel\n",
			n_decl_prot_stale);
	fprintf(stderr, "[vmremote] a single-page fetch from the source costs "
		"%llu us here (%lu samples); each request retains one page reservation\n",
		(unsigned long long)ctxpage_rtt_us, n_ra_rtt_samples);
	if (n_chunk_fetches || n_chunk_via_object || n_chunk_lost)
		fprintf(stderr, "[vmremote] chunks: %lu multi-page fetches; %lu "
			"neighbour page(s) landed in the object, %lu of them made "
			"present by the reply (no fault when reached), %lu could "
			"not be kept\n", n_chunk_fetches, n_chunk_via_object,
			n_chunk_populated, n_chunk_lost);
	if (n_prot_legal_granted || n_prot_legal_owner || n_prot_legal_nogrant)
		fprintf(stderr, "[vmremote] protection faults the owner's kernel "
			"ruled LEGAL (a hand-over's protection left in this "
			"context's page table): %lu granted in place, %lu found "
			"an owner meanwhile and re-faulted, %lu could not be "
			"granted and were declined\n", n_prot_legal_granted,
			n_prot_legal_owner, n_prot_legal_nogrant);
	if (n_decl_prot)
		fprintf(stderr, "[vmremote] of the declined protection faults, %lu "
			"were on a page nobody had borrowed; %lu on one lent "
			"whole and %lu on one lent shared that could not be "
			"taken back -- those %lu let the guest write a page the "
			"source still holds\n", n_decl_prot_free,
			n_decl_prot_whole, n_decl_prot_stuck,
			n_decl_prot_whole + n_decl_prot_stuck);
	/*
	 * The ownership answers, which are the point of the protocol: how often
	 * the source said "not mine", where the page was found instead, and how
	 * often the two sides claimed the same page at once. A zero in the last
	 * of these is only believable beside a non-zero in the first.
	 */
	fprintf(stderr, "[vmremote] page ownership: the source did not hold %lu "
		"of the pages asked for (%lu came from this context's own "
		"object, %lu from the local kernel because nobody had written "
		"them); %lu simultaneous claims yielded to the source, %lu gave "
		"up waiting\n", n_src_absent, n_src_absent_own,
		n_src_absent_kernel, n_claim_yield, n_claim_gaveup);
	if (n_empty_averted)
		fprintf(stderr, "[vmremote] %lu empty page(s) not written "
			"because another thread had installed the real one\n",
			n_empty_averted);
	fprintf(stderr, "[vmremote] spawn SETREGS races: %lu write(s) undone by "
		"the child's own context switch and re-issued, %lu never "
		"settled -- each of the first kind was a guest one deschedule "
		"away from running on this monitor's thread pointer\n",
		n_setregs_unstuck, n_setregs_lost);
	fprintf(stderr, "[vmremote] pages served to home: %lu read, %lu written back, "
		"%lu absent (home used its own copy); %lu handed over out of "
		"the object because no context had them mapped\n",
		n_pg_get, n_pg_put, n_pg_absent, n_take_from_object);
	fprintf(stderr, "[vmremote] hand-overs: %lu atomic (whole address space "
		"at once), %lu the object did not have, %lu fell back to the "
		"per-context sequence, %lu waited for a page something here "
		"could still reach\n",
		n_takeobj, n_takeobj_absent, n_takeobj_fallback, n_takeobj_busy);
	/*
	 * Both of these were counted and then thrown away. The first is how
	 * often this machine answered a fault from the context's own object
	 * rather than asking the source -- which is the whole of what a forked
	 * child's memory is, and the number to watch if one ever runs on its
	 * parent's bytes again. The second is a fault at an address the source
	 * had no region for and supplied anyway.
	 */
	if (check_coherence)
		fprintf(stderr, "[vmremote] coherence invariant: %lu violation(s) "
			"over %lu loan(s) examined in %lu sweep(s), %lu context "
			"page-table read(s) (a page lent whole to a shadow that "
			"a context still had present)\n", n_inv_violations,
			n_coh_examined, n_coh_sweeps, n_coh_ctx_reads);
	fprintf(stderr, "[vmremote] answered without asking home: %lu from this "
		"context's own object, %lu for an address home has no region "
		"for; %lu pages written into the object because no context had "
		"reached the address yet\n", n_from_own_backing,
		n_unmapped_served, n_put_to_object);
	fprintf(stderr, "[vmremote] %lu write grant(s) needed no take: the page "
		"was already here and only write-protected, so it was faulted "
		"in writable in place -- no sibling context lost its mapping\n",
		n_grant_mapobj);
	fprintf(stderr, "[vmremote] %lu write grant(s) fell back to the "
		"take-and-put-back; %lu of those saw the object's bytes move "
		"between the take and the put-back (the §6.3 window, probe %s)\n",
		n_grant_takeput, n_grant_takeput_raced,
		takeput_probe() ? "armed" : "off");
	if (n_lost_store_zeroed)
		fprintf(stderr, "[vmremote] %lu LOST-STORE(s): a page this side "
			"had installed, that nothing recently unmapped, was "
			"zero-filled after the source answered ABSENT -- the "
			"died-242/heap-corruption producer, named\n",
			n_lost_store_zeroed);
	fprintf(stderr, "[vmremote] mremap moves: %lu page(s) this side held "
		"relocated into the object at the new offsets; settle landings "
		"into the object: %lu\n", n_mremap_moved, n_settle_to_object);
	fprintf(stderr, "[vmremote] ranges the source's mm-mutation hook "
		"reported vacated and this side unmapped/punched everywhere: "
		"%lu; hook-log overflows (ranges LOST): %lu; faults landing "
		"inside a recently vacated range: %lu (each is a re-mapped "
		"range or an ordering edge -- see FAULT-IN-VACATED); vacates "
		"overlapping a DIFFERENT recent reply's map: %lu (the "
		"ordering edge CONFIRMED firing -- see VACATE-LATE)\n",
		n_ranges_vacated, n_vacate_overflow, n_fault_in_vacated,
		n_vacate_late);
	fprintf(stderr, "[vmremote] %lu mapping change(s) given to a sibling "
		"context of the same address space\n", n_as_map_fanout);
	fprintf(stderr, "[vmremote] mapping changes NOT fanned out because the "
		"kernel cannot make them in another task's mm (SET/PROT; the "
		"siblings map on their own faults): %lu; guest mappings found in "
		"the monitor's own address space: %lu (must be 0)\n",
		n_as_map_not_fanned, n_monitor_guest_vmas);
	fprintf(stderr, "[vmremote] %lu fault(s) answered by this address "
		"space's own live folio: absent in the faulting context, "
		"present in a sibling -- this side is the holder, so the page "
		"was NOT re-fetched from the peer over the sibling's stores\n",
		n_as_sibling_local);
	if (n_served_via_object)
		fprintf(stderr, "[vmremote] %lu page(s) could not be written "
			"through the context at all -- a range the owner mapped "
			"read-only -- so the bytes went into its backing object "
			"and the fault was declined for this kernel to map\n",
			n_served_via_object);
	if (n_guest_fault)
		fprintf(stderr, "[vmremote] %lu fault(s) the program had no right "
			"to make; the owner answered %lu of them with somewhere "
			"else to carry on\n", n_guest_fault, n_exc_delivered);

	{
		fprintf(stderr, "[vmremote] coherence: %lu pages moved to a "
			"shadow, %lu pulled back from one; "
			"%lu shared with the guest, %lu of those downgraded "
			"from held, %lu taken back for a write here, %lu for a "
			"write there; %lu stale (the context that borrowed "
			"them had finished); %lu forgotten that were never "
			"recorded, %lu lent twice over\n",
			n_lent, n_pg_pulled, n_shared,
			n_downgraded, n_unprotected, n_upgraded, n_stale,
			n_clear_miss, n_double_lend);
		/*
		 * The recall CHANNEL, reported apart and only when it exists.
		 *
		 * These two used to be printed above as "recalled" and "already
		 * given back", beside the hand-over count, where they read as
		 * the return direction of the same traffic -- and they are
		 * structurally zero: vmhome opens no recall channel and sends
		 * no HELLO_RECALL, so recall_find() searches a table nothing
		 * registers in. "0 recalled over 78000 hand-overs" was read as
		 * evidence that the return direction never runs (AUDIT XI
		 * §5.1); the return direction runs on the page channel, as a
		 * pull, and is n_pg_pulled above -- 80014 of them in the run
		 * that reported 0 recalls. A counter that cannot fire must not
		 * sit where a reader will take its zero for a measurement
		 * (PRINCIPLES §7).
		 */
		if (n_recalled || n_recall_absent)
			fprintf(stderr, "[vmremote] recall channel: %lu "
				"recalled, %lu already given back\n",
				n_recalled, n_recall_absent);
		fprintf(stderr, "[vmremote] ABSENT answers by what this side's "
			"own record said: %lu never held here, %lu already out "
			"on loan, %lu recorded as the guest's and unreadable "
			"(a page lost on this side)\n", n_absent_unseen,
			n_absent_was_shadow, n_absent_was_guest);
		fprintf(stderr, "[vmremote] ABSENT answers that were stale at "
			"arrival (this side's own serve of the page to the "
			"source crossed them; re-asked, never the retained "
			"copy): %lu\n", n_absent_stale_crossed);
	}
	if (n_upgrade_stale)
		fprintf(stderr, "[vmremote] %lu upgrades were of a page this side "
			"did not agree the shadow held shared\n", n_upgrade_stale);
	/*
	 * Unconditional: the statement wanted here is how often the guest was
	 * granted a write to a page the source was holding, and a line printed
	 * only when a counter moved cannot make it.
	 */
	fprintf(stderr, "[vmremote] read faults whose pull took the page: %lu -- "
		"the guest's copy protected and watched rather than recorded as "
		"a loan to a source that had just handed it over; %lu of those "
		"writes then arrived and were granted\n",
		n_pull_not_shared, n_watch_grant);
	fprintf(stderr, "[vmremote] writes to a page the source held shared: %lu "
		"answered by asking for it back over a channel that does not "
		"exist; %lu by pulling it (%lu came with the bytes, %lu the "
		"source did not hold after all, %lu could not be had)\n",
		n_prot_recall_dead, n_prot_pull, n_prot_pull_bytes,
		n_prot_pull_absent, n_prot_pull_busy);
	{
		double total = t_now() - t_start;
		double setup = t_first_entry ? t_first_entry - t_start : 0;

		/*
		 * Fault and syscall time are summed over every context, and a
		 * forked guest's contexts are serviced by separate threads at
		 * the same time — so the two can add up to more than the wall
		 * clock. Saying "guest/other: -0.087s" instead of saying they
		 * overlapped is how an accounting artefact gets read as a bug.
		 */
		double rest = total - setup - t_fault - t_syscall;

		fprintf(stderr, "[vmremote] time: %.3fs total = %.3f setup + %.3f "
			"faults (%lu) + %.3f syscalls (%lu) + %.3f %s\n",
			total, setup, t_fault, n_fault_ev, t_syscall, n_fwd,
			rest < 0 ? -rest : rest,
			rest < 0 ? "overlapped (contexts serviced in parallel)"
				 : "guest/other");
		fprintf(stderr, "[vmremote] INSTALLED acks (confirmed): %lu sent for "
			"page episodes\n", n_acks_sent);
		fprintf(stderr, "[vmremote] socket reads that spun (%u us budget): "
			"%lu answered inside the spin, %lu ran out and slept\n",
			sock_spin_us, n_sock_spin_hit, n_sock_spin_miss);
		fprintf(stderr, "[vmremote] teardown: %.3f reap + %.3f children + "
			"%.3f finish + %.3f report\n",
			t_reaped - t_svc_end, t_kids - t_reaped,
			t_finished - t_kids, t_now() - t_finished);
		fprintf(stderr, "[vmremote] guest ran or waited %.3fs between "
			"events; longest single gap %.3fs, after %s 0x%lx\n",
			t_wait, t_wait_max,
			wait_max_after_type == VMCTX_EV_SYSCALL ? "syscall" :
			wait_max_after_type == VMCTX_EV_FAULT ? "fault at" :
			"event", wait_max_after_nr);
	}
	fprintf(stderr, "[vmremote] exited %d ; %lu syscalls, %lu forwarded home, "
		"%lu pages faulted in from home, %lu of them given the "
		"program's own protection\n",
		WIFEXITED(st) ? WEXITSTATUS(st) : -1, n_sys, n_fwd,
		n_pages_faulted, n_prot_applied);
	fprintf(stderr, "[vmremote] execution CPU checkpoints: %lu (source budget %llu ns)\n",
		n_runtime, (unsigned long long)guest_runtime_quantum);
	fwd_report_inflight();
	if (n_sys != n_fwd || vmr_fwd_broken)
		fprintf(stderr, "[vmremote] %ld syscall(s) were NOT forwarded (%lu "
			"broken channel); source completion was not recorded for "
			"these requests\n",
			(long)(n_sys - n_fwd), vmr_fwd_broken);
	/*
	 * A thread that died does not make the program's own exit status
	 * anything but zero -- the joiner was woken and main returned normally
	 * -- so say it here and answer with it. A run that lost a thread has
	 * not succeeded, whatever the guest thinks.
	 */
	if (n_ctx_died) {
		fprintf(stderr, "[vmremote] %lu context(s) ended on their own "
			"with a bad status (first was %d); the guest exited %d "
			"but a thread of it died and its joiner was woken as "
			"though it had finished\n", n_ctx_died,
			worst_ctx_status,
			WIFEXITED(st) ? WEXITSTATUS(st) : -1);
		if (WIFEXITED(st) && WEXITSTATUS(st) == 0)
			return worst_ctx_status ? worst_ctx_status : 1;
	}
	return WIFEXITED(st) ? WEXITSTATUS(st) : 1;
}

static void report_counters(const char *why)
{
	double total = t_now() - t_start;
	double rest  = total - t_fault - t_syscall;

	fprintf(stderr, "[vmremote] --- counters at %s ---\n", why);
	fprintf(stderr, "[vmremote] time: %.3fs total = %.3f faults (%lu) + "
		"%.3f syscalls (%lu) + %.3f %s\n",
		total, t_fault, n_fault_ev, t_syscall, n_fwd,
		rest < 0 ? -rest : rest,
		rest < 0 ? "overlapped (contexts serviced in parallel)"
			 : "guest/other");
	fprintf(stderr, "[vmremote] %lu syscalls, %lu forwarded home, "
		"%lu pages faulted in from home, %lu of them given the "
		"program's own protection\n",
		n_sys, n_fwd, n_pages_faulted, n_prot_applied);
	fprintf(stderr, "[vmremote] INSTALLED acks (confirmed): %lu page episodes\n", n_acks_sent);
	if (n_recall_resited || n_notholder_lost)
		fprintf(stderr, "[vmremote] recalls re-aimed at a live sibling of "
			"the same address space: %lu; pages no context of the "
			"address space had: %lu -- the second is a page alive "
			"at neither site, which is the failure the first "
			"prevents\n", n_recall_resited, n_notholder_lost);
	fprintf(stderr, "[vmremote] execution CPU checkpoints: %lu (source budget %llu ns)\n",
		n_runtime, (unsigned long long)guest_runtime_quantum);
	fwd_report_inflight();
	if (n_sys != n_fwd || vmr_fwd_broken)
		fprintf(stderr, "[vmremote] %ld syscall(s) were NOT forwarded (%lu "
			"broken channel); source completion was not recorded for "
			"these requests\n",
			(long)(n_sys - n_fwd), vmr_fwd_broken);
	fprintf(stderr, "[vmremote] faults declined to the local kernel: %lu "
		"(no region / local) + %lu protection + %lu already present "
		"(%lu of those unanswerable)\n",
		n_declined, n_decl_prot, n_declined_present,
		n_present_unknown);
	decl_sites_report();
	if (n_decl_prot_whole + n_decl_prot_stuck)
		fprintf(stderr, "[vmremote] %lu of those were writes to a page "
			"the source still holds\n",
			n_decl_prot_whole + n_decl_prot_stuck);
	/*
	 * The caps that change the answer, on the line where a run is judged.
	 *
	 * Each of these is a case where this side quietly stopped being able to
	 * say something true: a loan that was never written down, a page that
	 * will be refetched over the guest's own writes, a context that kept a
	 * page it was asked to give up, a clone whose arguments were read in
	 * part, a page the shadow was told about that could not be fetched
	 * first. None of them makes the run stop, and every one of them makes
	 * everything after it suspect -- so a run that reports any of these is
	 * not a run with a small blemish, it is a run whose memory cannot be
	 * argued about. Printed always, including the zeros: a counter that is
	 * only printed when it fires cannot be distinguished from one that was
	 * never reached.
	 */
	fprintf(stderr, "[vmremote] contexts stopped because the owner's address "
		"space had ended: %lu -- each would otherwise have been served "
		"an invented page and died on it, and each was stopped through "
		"its own teardown so what waited on it was released\n",
		n_pull_gone);
	fprintf(stderr, "[vmremote] contexts TERMINATED while other contexts of "
		"their address space were still running: %lu -- a context that "
		"ends itself leaves nobody behind, so each of these left the "
		"owner holding a live address space it was never asked to stop "
		"or to reclaim from\n", n_end_with_live_siblings);
	fprintf(stderr, "[vmremote] bookkeeping that changed an answer: "
		"%lu contexts would not yield a page, %lu would not "
		"stop writing one, "
		"%lu prefills failed, %lu faults where the owner could not be "
		"asked\n",
		n_take_partial, n_protect_partial,
		n_pg_prefill_fail, n_mayaccess_lost);
	fprintf(stderr, "[vmremote] guest ran or waited %.3fs between events; "
		"longest single gap %.3fs, after %s 0x%lx\n",
		t_wait, t_wait_max,
		wait_max_after_type == VMCTX_EV_SYSCALL ? "syscall" :
		wait_max_after_type == VMCTX_EV_FAULT ? "fault at" : "event",
		wait_max_after_nr);
	fflush(stderr);
}
