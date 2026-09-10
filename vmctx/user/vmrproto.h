/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Wire protocol between vmremote (on the machine running the VM context) and
 * vmhome (on the machine whose kernel should service the syscalls).
 *
 * The execution architecture is x86-64, with fixed-width little-endian fields.
 * The peers need not run the same operating system. OS semantics belong to
 * the source adapter; the destination consumes architectural state and effects.
 *
 * One request per guest syscall that is forwarded, one response back:
 *
 *   vmremote                                  vmhome
 *   --------                                  ------
 *   guest traps, syscall redirected
 *   PEEK any input buffers from the guest
 *   send rreq [+ input data]           ->
 *                                             execute the syscall for real
 *                                             (raw syscalls, so the kernel ABI
 *                                             layout of stat/dirent/utsname is
 *                                             exactly what the guest expects)
 *                                      <-     send rrsp [+ output data]
 *   POKE output data into guest memory
 *   RESUME the context with retval
 */
#ifndef _VMRPROTO_H
#define _VMRPROTO_H

#include <stdint.h>
#include "vmctx_cpu_model.h"
#include "memory-identity.h"

/*
 * "VMR2": vmr_rsp grew the vacate[] block (the mm-mutation hook's ranges), so
 * a peer still speaking VMR1 would mis-frame every reply. The magic makes the
 * mismatch die at the first message with its name on it, instead of as a
 * corrupted-looking run an hour later.
 */
/*
 * "VMR3": vmr_rsp carries the take-generation of every page a CTXPAGE reply
 * serves (pggen[]), and vmr_pgrsp the generation of the page it hands over.
 * See "The take generation" below.
 */
/*
 * "VMR4": VMR_OP_INSTALLED has no reply (the source's record is the kernel's,
 * settled as the ack is read; nothing waits), and the source never answers
 * a page request with a per-request log line. A VMR3 peer would wait for an
 * ack reply that never comes.
 *
 * "VMR5": vmr_rsp grew child_source (a clone reply names the child twice -- the
 * guest's namespace-relative pid in retval, the source machine's own name
 * for it in child_source). The struct's size changed, so a VMR4 peer misframes
 * every reply.
 */
/*
 * "VMR6": vmr_rsp grew ended/ended_status -- the source's word that the
 * context's SERVICE TASK HAS DIED (killed by a sibling's kill(2), or by its
 * own abort(3) through tgkill) while the guest still runs. Without it the
 * destination's contexts of that process spun for ever on fabricated -EIO
 * (netsurf cross-machine: the parent SIGKILLs a helper; firefox: a content
 * process aborts). The struct's size changed, so a VMR5 peer misframes.
 */
#define VMR_MAGIC 0x50524d56u	/* "VMRP": v25 exact retained source transfer episodes */

struct vmr_req {
	uint32_t magic;
	uint64_t nr;		/* protocol operation; never a guest syscall */
	uint64_t call_nr;	/* opaque guest identifier for OP_PREPARE */
	uint64_t ticket; /* source-issued call identity; zero for initial preparation */
	uint64_t execution_epoch, execution_ns; /* cumulative per-context CPU time */
	uint64_t args[6];	/* raw guest arguments (pointers are guest-side) */
	uint64_t datalen;	/* bytes of input payload following this struct */
	struct vmr_mm_binding target; /* saved operation binding; empty for lifecycle work */
};

/*
 * The guest's registers, in the order vmctx_uregs uses, so a reply can hand
 * back a whole control-flow change without naming what caused it.
 */
struct vmr_uregs {
	uint64_t rax, rbx, rcx, rdx, rsi, rdi, rbp, rsp;
	uint64_t r8, r9, r10, r11, r12, r13, r14, r15;
	uint64_t rip, rflags, orig_rax;
	uint64_t fs_base, gs_base;
};

/* Architectural XSAVE legacy region/header (576 bytes), followed by active
 * components >= 2 in ascending order: {u32 feature, u32 size, u8 data[size]}.
 * Each local adapter translates its CPUID-specific offsets to this format.
 * No OS signal frame or task structure is sent. Unsupported features fail.
 */
#define VMR_XSTATE_MAX 16384
struct vmr_cpu_state {
	uint32_t xstate_size, reserved;
	struct vmr_uregs regs;
	uint8_t xstate[VMR_XSTATE_MAX];
};
/* On the wire this ends after xstate_size bytes, not at buffer capacity. */
_Static_assert(sizeof(struct vmr_cpu_state) == 16560, "CPU adapter buffer layout");

/* Source OS adapters translate native operations into these effects. The
 * executor never derives them from an instruction's syscall number or result.
 * These construction records do not replace the committed mapping journal. */
#define VMR_MAP_NONE 0
#define VMR_MAP_SET 1
#define VMR_MAP_PROTECT 2
#define VMR_MAP_MOVE 3
#define VMR_MAP_SHARED 1
#define VMR_MAP_REPLACE 2
#define VMR_PROT_READ 1
#define VMR_PROT_WRITE 2
#define VMR_PROT_EXEC 4
#define VMR_EFFECT_IMAGE 1
#define VMR_EFFECT_CHILD 2
struct vmr_map_effect {
	uint32_t kind, flags, protection, reserved;
	uint64_t address, length, object_offset, prior_address, prior_length;
	uint64_t object_id; /* opaque source lifetime identity; zero for private */
};
_Static_assert(sizeof(struct vmr_map_effect) == 64, "mapping effect wire layout");

/* Architectural access effects; no native syscall numbers, VMA flags or PTE
 * encoding cross this interface. An address-space lifetime owns the cursor.
 * READ is repeatable; ACK follows successful application on every member. */
#define VMR_ACCESS_MAX 16
#define VMR_ACCESS_UNMAP 1
#define VMR_ACCESS_DISCARD 2
#define VMR_ACCESS_PROTECT 3
#define VMR_ACCESS_DENY 4
#define VMR_ACCESS_ALLOW 5
#define VMR_ACCESS_CONSTRUCT 6
struct vmr_access_event {
	uint64_t sequence, start, end;
	uint32_t kind, protection;
};
struct vmr_access_batch {
	uint64_t identity, cursor, head, acknowledged;
	uint32_t count, reserved;
	struct vmr_access_event event[VMR_ACCESS_MAX];
};
_Static_assert(sizeof(struct vmr_access_batch) == 552, "access journal wire layout");

struct vmr_rsp {
	uint32_t magic;
	uint32_t call_state; /* VMR_CALL_* for preparation/dispatch polling */
	uint64_t ticket;
	uint32_t prepare_flags, call_reserved;
	/* A memory receipt describes the request that produced these bytes.
	 * The current binding publication below may already describe a new MM. */
	struct vmr_mm_binding memory_target;
	uint64_t memory_op, memory_args[6];
	/*
	 * A clone reply. Non-zero means the child shares the parent context's
	 * backing object (a thread); zero means it gets its own (a process).
	 * The source's own kernel decided this by running the real clone/clone3
	 * -- the destination reads no guest memory and parses no clone flags.
	 * Meaningful only when the forwarded call was a clone; zero otherwise.
	 */
	int32_t  child_shared_mm;
	int64_t  retval;	/* what the guest should see in RAX          */
	/* Opaque source registry token for a committed birth. It never denotes
	 * a native PID, MM identity, or guest syscall result. Zero means no birth. */
	int64_t child_source;
	uint64_t datalen;	/* bytes of output payload following         */
	/*
	 * And, when the owner decides the guest should carry on somewhere
	 * else, the registers to carry on with.
	 *
	 * This is the whole of how a signal is delivered, and it names no
	 * signal: the owner writes whatever frame its own system uses into the
	 * guest's memory -- which it can already do -- and hands back the
	 * register set that enters the handler. The far side loads registers
	 * and resumes. Returning from the handler is the same thing in
	 * reverse, and is likewise just a reply carrying registers.
	 */
	uint32_t regs_valid;
	/*
	 * Non-zero: the service task that would have run this call is DEAD on
	 * the source, proved by its retained native terminal record. ended_status
	 * is that record's wait status, including after native reaping. The
	 * destination ends only the executor for this source task; MM peers have
	 * their own lifetimes and require their own native terminal observations.
	 */
	uint32_t ended;
	uint32_t ended_status;
	uint32_t effects; /* VMR_EFFECT_*: source-decided lifecycle changes */
	uint64_t pages_taken; /* CTXPAGE data always requires acknowledgement */
	uint64_t page_episode; /* opaque full64 source custody identity; one page */
	struct vmr_map_effect mapping;
	struct vmr_uregs regs;
	/*
	 * Ranges the source's KERNEL saw vacated in the adopted mm while this
	 * call ran -- the mm-mutation hook (mmu notifier), not syscall decode.
	 * Whatever unmapped them (munmap, mremap's move or shrink, an mmap
	 * MAP_FIXED landing on live memory, MADV_DONTNEED, a call nobody has
	 * thought of), the memory those ranges described is GONE at the
	 * source, and the destination must unmap and punch its own copies or
	 * serve ghosts. The owner's ruling that forced this shape: propagation
	 * follows the mm being touched, never a list of syscall numbers --
	 * every hole in such a list (mremap was one, measured as netsurf's
	 * exit-242 wall) is stale memory somewhere.
	 *
	 * vacate_overflow counts hook events the source's ring could not hold
	 * (drained loudly on the far side; ranges lost this way are a defect
	 * to fix, never a shrug).
	 *
	 * Bit 0 of start (addresses are page-aligned, the bit is free):
	 * 0 = the mapping is GONE (unmap: remove the range, records and all);
	 * 1 = the mapping LIVES but its content was DISCARDED
	 * (MADV_DONTNEED: punch the content copies and ZAP the pages, keep
	 * the mapping and its records -- the next touch refetches fresh).
	 */
	uint32_t nvacate;
	uint32_t vacate_overflow;
	/*
	 * THE MUTATION SEQUENCE. The change log is per mm and any forwarded
	 * call of any thread drains it, so a range one context's mmap
	 * (MAP_FIXED over its own reservation -- ld.so placing a segment) made
	 * the kernel log as vacated can ride ANOTHER context's reply, and the
	 * two replies are applied by two threads on the far side in whatever
	 * order they land. Measured: the vacate of 0x7ffff419b000-0x7ffff419e000
	 * drained after a concurrent readlink, applied 1ms after the segment's
	 * own SET, punched the live RW segment ld.so was relocating; the first
	 * store into it was answered -14 (netsurf, exit 242 at 7.6s).
	 *
	 * So every drained range carries the sequence number it was assigned
	 * at its drain, and every reply carries mmseq, assigned AFTER its
	 * drain: a construction this reply makes is newer than every range
	 * anyone drained before it, including the range its own call vacated.
	 * The far side records what each reply constructs with its mmseq and
	 * never lets a vacate punch a construction whose stamp is above the
	 * vacate's. One monotonic counter on the source; no lock, no kernel
	 * change: "later than" is all that is asked of it.
	 */
	struct vmr_vacate { uint64_t start, end, seq; } vacate[8];
	uint64_t mmseq;
	/*
	 * THE TAKE GENERATION, source half. For a VMR_OP_CTXPAGE answer that
	 * carries bytes, pggen[i] is the generation of the i-th served page:
	 * the destination's own per-(address space, page) count of how many
	 * times it has captured that page (a take away, a take-and-put-back,
	 * a write-grant), as it stood when THIS copy left the destination --
	 * stamped on the GET reply that brought it here and kept beside the
	 * ownership record. 0 = the copy never came from the destination (a
	 * page this side filled itself, the file, a fresh mapping).
	 *
	 * The destination compares it with its current count. A copy whose
	 * generation is below the count descends from an OLDER capture than
	 * one the destination has made since: it is stale by construction,
	 * whatever path brought it -- the arena content-history caught both
	 * serve_object_mapped (site80) and the source's pull (site31) handing
	 * back glibc-arena words one write behind, and neither site could
	 * tell a current sole-holder copy from a stale leftover, because the
	 * bytes do not carry their age. This is the age.
	 */
	uint32_t ngen;
	uint32_t pggen[16];
	/* Current connection context at response publication, and the exact
	 * retained birth when EFFECT_CHILD is set. Neither is an implicit
	 * memory-operation target. Zero current binding precedes program start. */
	struct vmr_mm_binding binding, child_binding;
};

static inline int vmr_rsp_bindings_valid(const struct vmr_rsp *reply)
{
	if(!vmr_binding_empty(&reply->binding) && !vmr_binding_valid(&reply->binding))return 0;
	if(!(reply->effects & VMR_EFFECT_CHILD))return vmr_binding_empty(&reply->child_binding);
	if(!vmr_binding_valid(&reply->binding) || !vmr_binding_valid(&reply->child_binding) ||
	   reply->child_source<=0 || reply->child_binding.context!=(uint64_t)reply->child_source ||
	   reply->child_binding.context==reply->binding.context)return 0;
	if(reply->child_shared_mm)
		return reply->child_shared_mm==1 && reply->child_binding.mm==reply->binding.mm &&
			reply->child_binding.parent_mm==reply->binding.parent_mm;
	return reply->child_binding.mm!=reply->binding.mm &&
		reply->child_binding.parent_mm==reply->binding.mm;
}

/*
 * The oracle's memory map, as /proc/maps text, sized for real programs rather
 * than for the toys this was first tried on. A graphical browser is not
 * unusual at ~500 mappings and ~58 KiB of text; the 64 KiB this used to be left
 * ten percent of headroom, and going over it truncated the map in silence.
 * Both ends must agree, and both must stay under MAXDATA — a layout that does
 * not fit in one payload is not carried in one.
 */
#define VMR_LAYOUT_MAX (192 * 1024)

/*
 * Syscall numbers named here rather than by number at the call site.
 *
 * They do not describe a set that is forwarded and a set that is not: every
 * syscall the guest makes is forwarded to the machine that owns the program,
 * the memory-management calls included. mmap is performed at the source, and
 * what it did to the address space comes back in the reply and is applied to
 * the context by VMCTX_MAP_SET — the destination never decides a mapping for
 * itself.
 */
#define VMR_NR_read		0
#define VMR_NR_write		1
#define VMR_NR_open		2
#define VMR_NR_stat		4
#define VMR_NR_lstat		6
#define VMR_NR_sendfile		40
#define VMR_NR_close		3
#define VMR_NR_fstat		5
#define VMR_NR_lseek		8
#define VMR_NR_pread64		17
#define VMR_NR_writev		20
#define VMR_NR_uname		63
#define VMR_NR_getcwd		79
#define VMR_NR_getdents64	217
#define VMR_NR_openat		257
#define VMR_NR_newfstatat	262

/*
 * Control operations, numbered above the syscall range. These implement the
 * general model: the remote machine holds no knowledge of files at all. Home
 * starts the program (so *its* kernel does the ELF loading, interpreter and
 * all) and stops it before its first instruction; that stopped process is a
 * memory oracle. The remote machine then runs the program and, whenever it
 * touches a page it does not have, asks home for the contents.
 */
#define VMR_OP_CPUSTATE 0x100d /* architectural state of this source context */
/* CPUSTATE returns a full architectural payload, or one of these payload-free
 * states. ENDED also carries the existing source termination metadata. */
#define VMR_CPU_PENDING 1
#define VMR_CPU_ENDED 2
#define VMR_OP_PREPARE 0x100e /* install CPU/runtime, begin native admission */
#define VMR_OP_EXECUTE 0x100f /* commit only the admitted ticket; no CPU payload */
#define VMR_PREPARE_SETTLE 1  /* settle outstanding page loans before call */
#define VMR_PREPARE_SNAPSHOT 2 /* hold AS execution through child publication */
#define VMR_OP_CPU_MODEL 0x1010 /* negotiate before START, architecture only */
#define VMR_OP_ACCESS_READ 0x1011 /* args: identity, acknowledged cursor */
#define VMR_OP_ACCESS_ACK 0x1012 /* args: identity, successfully applied cursor */
#define VMR_OP_RUNTIME_CAPS 0x1013 /* source supplies execution checkpoint budget */
#define VMR_OP_BOUNDARY 0x1014 /* CPU state/accounting, with no guest syscall */
#define VMR_OP_CHILD_PAGES 0x1015 /* source-owned private pages before child release */
#define VMR_OP_CALL_POLL 0x1016 /* query admission/dispatch; never dispatch itself */
#define VMR_CALL_ENTERING 1
#define VMR_CALL_ADMITTED 2
#define VMR_CALL_RUNNING 3
#define VMR_CALL_COMPLETE 4
#define VMR_CHILD_PAGES_MAX 256
struct vmr_child_pages {
	uint64_t next; /* next address to scan; zero when complete */
	uint32_t count, reserved;
	uint64_t pages[VMR_CHILD_PAGES_MAX];
};
#define VMR_OP_START	0x1000	/* start the program at home, stopped at entry */
#define VMR_OP_LAYOUT	0x1001	/* the oracle's memory map, as /proc/maps text */
#define VMR_OP_PAGE	0x1002	/* bytes at an address in the oracle           */
/*
 * Bytes at an address in *this context's* address space at the source.
 *
 * Not the oracle, which holds the program as it was loaded and is the right
 * answer only for a context that has just started. A running context's memory
 * lives in its shadow, and a forked one's address space is its parent's as of the
 * fork -- so a child asking the oracle gets a startup image and dies on its first
 * dereference.
 *
 * This is the primitive the model needs, because the destination keeps no map: on
 * a fault it asks whether the source has this address, and the answer and the
 * bytes come together. An address the source has not mapped reports itself as
 * EFAULT, which is a real segmentation fault for the guest rather than an excuse
 * to zero-fill.
 */
#define VMR_OP_CTXPAGE	0x1009
/*
 * The answers VMR_OP_CTXPAGE can give, and why "here are the bytes" cannot be
 * the only one.
 *
 * A page is either here or there, and it may be writable in one place only. So
 * the question "give me the page at this address" has four honest answers, not
 * two, and until this header carried all four the source had to make one up.
 *
 *   len            the bytes, and the page has left this side: the asker owns
 *                  it now and this side must fault to get it back.
 *   ABSENT         this side does not hold that page. Not "there is no such
 *                  address" and not "here are some zeros": the mapping exists,
 *                  its contents are not here, and the asker's own copy -- its
 *                  backing object, or its kernel's zeros for memory the program
 *                  has never touched -- is the authority.
 *   CLAIMING       this side is claiming the very same page at this instant:
 *                  it has been marked, an RPC for it is outstanding, and
 *                  handing it over now would leave two claims live. The source
 *                  wins that race by rule -- it is inside a kernel syscall and
 *                  must not be made to wait -- so the destination yields and
 *                  its guest simply faults again, which costs one round trip.
 *   NOTHOLDER      this side handed that page to the asker and has not been
 *                  asked for it back. It is NOT the same answer as ABSENT and
 *                  the difference is the whole of AUDIT part XII: ABSENT says
 *                  "your own copy is the authority", which licenses the asker
 *                  to fall back on its object or, failing that, on zeros. That
 *                  is true for an address nobody has ever written and false --
 *                  catastrophically -- for one this side gave the asker, whose
 *                  current contents were last written BY the asker. Measured:
 *                  the destination wrote an empty page over a live guest thread
 *                  stack on exactly that answer, and the thread's canary caught
 *                  it. A page neither machine can produce is an error routed to
 *                  the owner (PRINCIPLES §3), not a page of zeros.
 *   -EFAULT        the program has no mapping there at all, or no right to the
 *                  access. That is a real segmentation fault and belongs to the
 *                  owner to interpret.
 *
 * Measured, on tests/pg3: with only "bytes" and "EFAULT" available, a page the
 * source did not hold was answered with 4096 zeros and a success. The zeros
 * were transferred faithfully -- both ends agreed on the checksum of an empty
 * page -- and landed on a guest thread's live stack, whose return address
 * became 0. Six runs in six died at rip=0. ABSENT is that answer, said out
 * loud.
 */
#define VMR_CTXPAGE_ABSENT	(-61)	/* -ENODATA */
#define VMR_CTXPAGE_CLAIMING	(-11)	/* -EAGAIN  */
#define VMR_CTXPAGE_NOTHOLDER	(-116)	/* -ESTALE: you have it, not this side */
#define VMR_CTXPAGE_FAILED       (-4096) /* operation failed; no ownership or access conclusion */
/*
 * -EXDEV: "that page is still the parent's; break copy-on-write where it is."
 *
 * A forked child's page is the parent's until one of them writes. The SOURCE
 * owns that decision -- it ran the real clone3 and knows the relationship -- but
 * it does not have to own the BYTES to make it: when the destination is holding
 * the parent's page, asking the source to fetch it back first is what deadlocks
 * (the page server may not acquire for another context, and the parent's monitor
 * cannot act while the parent is blocked in a syscall).
 *
 * So the source answers with the decision instead of the data. Breaking
 * copy-on-write legitimately produces two pages -- the parent keeps X, the child
 * gets its own Y -- and the destination, which is holding X, is the side that
 * can make Y without moving anything between machines.
 *
 * It is the answer to two different questions, and both of them need it:
 *
 *   VMR_OP_CTXPAGE from the CHILD    "this side does not hold it and it is not
 *                                    yours to fill with zeros: it is still your
 *                                    parent's, and the parent's copy is on your
 *                                    machine. Take a copy of it."
 *   VMR_OP_WRITEPREP from the PARENT "before you let that write land, the child
 *                                    has not got its copy yet. Make it first."
 *
 * The second is what makes the first correct. Without it the parent's stores
 * after the fork reach the one page both address spaces are still reading, and
 * a child that faults later is handed memory as of its fault rather than as of
 * the fork.
 */
#define VMR_CTXPAGE_COWBREAK	(-18)
/*
 * VMR_CTXPAGE_GONE -- "this address space has ENDED; stop."
 *
 * Not "I could not find a context to answer with" -- that is a lookup, and a
 * lookup that fails is transient. This is said only when the source kernel
 * reports that the non-reused MM identity has no remaining native context or
 * memory-operation reference. A missing userspace monitor is not that proof.
 *
 * Said as an ANSWER rather than as a push, because a push has to name what to
 * stop and the owner cannot always name it -- the main connection carries
 * ctx 0, and an earlier version resolved "no context named" to "the first
 * context" and killed a whole LIVE address space. An answer reaches exactly the
 * context that asked, which is by construction the one that cannot be served.
 *
 * All three clauses are this one path. STOP: the asking context ends. CLEANUP:
 * it ends through its OWN teardown, so VMR_OP_FINISH is still sent and the
 * owner still performs the clear-tid clear and the wake a joiner needs
 * (§16.4) -- which is what the push version skipped, and why it hung. RECLAIM:
 * there is nothing to reclaim into; what has ended is the memory itself.
 */
#define VMR_CTXPAGE_GONE	(-19)
/*
 * "Context X is about to write the page at args[0], which the destination holds.
 * Is there anything that has to happen first?"
 *
 * The destination asks this and nothing else: it reports a write fault on a page
 * it is holding, names no relationship, and reads no meaning into the answer
 * beyond executing it. The source answers 0 ("nothing; carry on") or
 * VMR_CTXPAGE_COWBREAK ("an address space that copied this one has not been
 * given this page yet; give it a copy of what is there now, then let the write
 * land"). The source is the only side that can answer, because it ran the real
 * clone3, it knows which address spaces are copies of which, and it knows what
 * protections the program has on the address.
 *
 * Numbered as an operation rather than a syscall so vmhome's size-keyed dispatch
 * answers it here instead of executing it in the program's own task.
 */
#define VMR_OP_WRITEPREP	0x100a
/*
 * Confirm installation of one complete page. args[0] is the saved address;
 * args[1] is the exact full64 page_episode from its CTXPAGE reply. The source
 * retains native custody until this operation validates the saved MM binding,
 * address and episode, then acknowledges and forgets that native ticket.
 * All data replies need confirmation, including read-only copies and explicit
 * zero-page grants. A checksum or successful socket send is not delivery.
 * The executor retains its reservation and stopped fault until a valid empty
 * reply with retval 0 arrives. A lost/rejected/malformed reply is terminal.
 */
#define VMR_OP_INSTALLED	0x100b
/*
 * "I was served a page older than my own last capture of it -- print what
 * your records say happened to args[0]." Diagnosis only: the source answers
 * 0 and writes its per-page trail (receives, serves, drops, forgets, with
 * generations) to its log, beside the destination's own trail of the same
 * page, so a stale serve names its producer on both timelines at once.
 */
#define VMR_OP_STALETRAIL	0x100c

static inline int vmr_memory_operation(uint64_t op)
{
	switch(op) {
	case VMR_OP_LAYOUT: case VMR_OP_PAGE: case VMR_OP_CTXPAGE:
	case VMR_OP_WRITEPREP: case VMR_OP_INSTALLED: case VMR_OP_STALETRAIL:
	case VMR_OP_ACCESS_READ: case VMR_OP_ACCESS_ACK: case VMR_OP_CHILD_PAGES:
		return 1;
	default:return 0;
	}
}

static inline void vmr_memory_receipt(const struct vmr_req *request,struct vmr_rsp *reply)
{
	reply->memory_target=request->target;
	reply->memory_op=request->nr;
	for(unsigned i=0;i<6;i++)reply->memory_args[i]=request->args[i];
}

static inline int vmr_memory_reply_matches(const struct vmr_req *request,
		const struct vmr_rsp *reply)
{
	if(!vmr_memory_operation(request->nr)) {
		if(!vmr_binding_empty(&reply->memory_target) || reply->memory_op)return 0;
		for(unsigned i=0;i<6;i++)if(reply->memory_args[i])return 0;
		return vmr_binding_empty(&request->target);
	}
	if(!vmr_binding_valid(&request->target) || reply->memory_op!=request->nr ||
	   !vmr_binding_equal(&request->target,&reply->memory_target))return 0;
	for(unsigned i=0;i<6;i++)if(reply->memory_args[i]!=request->args[i])return 0;
	return 1;
}
#define VMR_OP_FINISH	0x1004	/* release the oracle                          */
#define VMR_OP_CHILD	0x1007	/* a child the source already made by running the
				 * guest's real clone: args[0] = its context id on
				 * the machine running it, args[1] = its pid on the
				 * source. The source answers by giving it its own
				 * monitor and page/recall channel. */
#define VMR_OP_MAPSYNC	0x1006	/* the guest's address space changed: here is
				 * its current map, mirror it in the shadow  */
/*
 * Numbered below 0x1000 on purpose.
 *
 * vmhome routes a request by size: anything under 0x1000 is a syscall the task
 * that owns the program performs, and anything above it is an operation vmhome
 * answers itself. A question meant for the program's own context that is
 * numbered like an operation never arrives -- it is answered -ENOSYS, and a
 * caller that reads that as a real answer gets fiction. 0xf02 is in the routed
 * range and is not a syscall number on any architecture this runs on.
 *
 * VMR_NR_MAYACCESS (0xf01) stood beside it and is gone. It asked the source
 * whether the program was allowed to touch an address, because the source's
 * copy of the address space could not be consulted any other way. There is no
 * copy now: the source performs the program's syscalls in the program's own
 * address space, so an access the program has no right to make simply fails the
 * call or arrives here as an exception, and "is there a page at this address"
 * is answered by asking for the page.
 */
/*
 * An exception the guest raised, handed to the side that owns the program.
 *
 * args[0] = vector, args[1] = error code, args[2] = faulting address, and the
 * payload is the guest's registers. The reply is the owner's decision: a
 * register set to carry on with, or nothing, which means it has no answer and
 * the context ends.
 *
 * The machine running the guest does not know what the vector means. Mapping a
 * hardware exception to what a program should experience is the owner's
 * operating system's business -- the same fault is a fatal signal on one system
 * and a segment grow on another -- and that mapping lives on the owner's side
 * and nowhere else.
 */
#define VMR_NR_EXCEPTION 0xf02
/*
 * The third answer: the owner's kernel would have handled the access. Asked
 * of VMCTX_CTL_TRYFAULT -- the fault path's own VMA lookup and access test
 * in the program's real address space -- before anything is called an
 * exception, because the machine running the guest cannot tell the program's
 * own protection fault from one the protocol left behind: a hand-over
 * write-protects a page in EVERY sibling context's page table, the sibling
 * that pulls it back re-grants only its own, and the next sibling's store
 * arrives as a present-page write fault on a page nobody holds. Measured on
 * hx2 (session 40): a writer's ordinary store judged fatal, the service
 * process killed by a SIGSEGV that then sat pending for twelve seconds, every
 * forwarded call of the teardown failing -EINVAL, and the thread-descriptor
 * pages lost -- 11 of 48 pool runs. With this answer the guest's side grants
 * the write it was always entitled to. Positive, so it cannot be confused
 * with an errno; 2, so the owner's own "carry on with these registers"
 * (1) stays distinct where both are returned.
 */
#define VMR_EXC_LEGAL		2

/*
 * Page service. The syscall side of remote execution cannot marshal buffers by
 * hand — a syscall's pointer arguments refer to memory that lives on the other
 * machine, and the kernel executing the call will touch pages nobody enumerated
 * in advance. So the local side executes syscalls in a *shadow* process whose
 * address space mirrors the guest's but starts empty, backed by userfaultfd:
 * any page the kernel touches is fetched from the machine that owns it, and
 * pages the kernel wrote are pushed back when the syscall returns.
 *
 * This is a second, page-only channel: the machine running the guest listens,
 * and the machine executing syscalls asks.
 */
#define VMR_PG_PORT_OFFSET 1	/* page service listens on syscall port + 1 */
#define VMR_PG_MAGIC 0x49504d56u	/* "VMPI": explicit MM, address and operation receipts */
#define VMR_PG_GET 1		/* give me the page at addr                */
#define VMR_PG_PUT 2		/* here is the page at addr, take it back  */
#define VMR_PG_PUTN 3		/* here are len bytes at addr — only the ones
				 * the kernel actually changed. Sending a whole
				 * page back would also carry the bytes we
				 * fetched earlier, undoing anything the guest
				 * wrote elsewhere in that page (a lock word
				 * reverting to "held" is how this shows up).
				 * Superseded by PUTV below and no longer sent;
				 * still served, because a run carrying one run
				 * is what PUTV degenerates to and answering it
				 * costs nothing. */
/*
 * Every changed run of one page, in a single round trip.
 *
 * PUTN sends one run per request and waits for its reply, and the kernel's
 * writes into a page are not one contiguous run: a struct with untouched
 * padding, or a buffer whose new bytes happen to match the old ones, breaks
 * into a dozen or more. Measured on a loopback run, 248 pages went back as
 * 3850 requests — fifteen round trips per page, averaging 49 bytes each, and
 * that was the single largest cost in a forwarded syscall.
 *
 * This carries the same bytes, chosen the same way; only the framing changes,
 * so nothing is written that PUTN would not have written.
 *
 * Layout: vmr_pgreq{op=PUTV, addr=page base, len=number of runs}, then that
 * many vmr_pgrun, then their bytes concatenated in the same order.
 */
#define VMR_PG_PUTV 4
#define VMR_PG_SIZE 4096

/*
 * The page channel, used the other way round.
 *
 * Single-writer coherence needs the machine running the guest to be able to ask
 * for a page *back*, and to be able to ask at any moment — the guest faults on
 * its own schedule, not on the shadow's. The shadow is usually blocked inside a
 * syscall when that happens, so the request cannot be answered by whatever
 * thread is executing syscalls. It gets a thread and a connection of its own,
 * which is the mirror image of the page-service thread the guest's machine
 * already runs for exactly this reason.
 *
 * HELLO_RECALL is sent once, by the shadow, to say "this connection is for
 * recalls and I am owner N". Afterwards the direction is inverted on it: the
 * guest's machine sends RECALL, the shadow answers.
 *
 * The owner is the shadow's pid rather than a context id on purpose. A guest
 * thread has a context of its own but shares an address space with its process,
 * and it is the address space that holds the page — so a page must be recalled
 * from the process that has it, which is a question about the shadow and not
 * about which context asked.
 */
#define VMR_PG_HELLO_RECALL 5	/* shadow -> guest machine, once per shadow  */
#define VMR_PG_RECALL       6	/* guest machine -> shadow: give it back and
				 * stop holding it. Answered with a whole page,
				 * or VMR_PG_ABSENT if the shadow does not have
				 * it, which is not an error: it means the page
				 * was never taken or has already gone back. */

/*
 * Read-only sharing.
 *
 * "One writer" does not mean one holder. Both machines may hold a page at once
 * as long as neither can write it, and for a real program that is most of them:
 * a library's text, a constant table, a stack that a syscall's arguments point
 * into while the guest is running on it. With only "held" and "not held" every
 * one of those moves on nearly every fault, because a page read by one side has
 * to be taken from the other.
 *
 * So a fault says what it needs. A read takes a shared copy and leaves the
 * other side a read-only one; a write takes the page outright.
 */
#define VMR_PG_GETS      7	/* shadow -> guest machine: a readable copy;
				 * the guest keeps one too, write-protected */
#define VMR_PG_DOWNGRADE 8	/* guest machine -> shadow: write-protect your
				 * copy and give me the bytes. You keep it —
				 * unlike RECALL, which takes it away.       */
#define VMR_PG_UPGRADE   9	/* shadow -> guest machine: I have a read-only
				 * copy and need to write it, so stop being a
				 * holder. No payload: the shadow's copy is
				 * already the current one.                  */
/*
 * "This address space was copied from another one, and it has not been given
 * this page yet. Break copy-on-write where the page is and send me the copy's
 * page."
 *
 * The source sends it and the destination executes it, exactly as with
 * VMR_CTXPAGE_COWBREAK -- this is the same instruction on the page channel,
 * for the case where it is the SOURCE's own task that faults on a page its
 * address space inherited. Answered with a whole page, or VMR_PG_ABSENT if the
 * copy is not owed that page (it has one of its own already) or the original's
 * page is not on that machine either.
 *
 * Without it the source's own fault service reaches "nobody has ever written
 * this address" for a page that plainly has been written -- by the address
 * space this one was copied from, on the other machine -- and supplies an empty
 * page. Measured: a forked child's clone(2) writes its tid into a page it
 * inherited, that page is zero-filled here, and the child then reads its whole
 * thread-control block as zeros and dies on its first dereference.
 */
#define VMR_PG_COWBREAK 10

struct vmr_pgrun {
	uint32_t off;		/* offset within the page  */
	uint32_t len;		/* bytes, none of them zero-length */
};

struct vmr_pgreq {
    uint32_t magic, op;
    uint64_t addr, len;
    struct vmr_mm_binding target; /* saved before the operation's first callback */
    uint64_t owner;
};
_Static_assert(sizeof(struct vmr_pgreq)==64,"page request wire layout");

/*
 * A page the guest has never touched is not present in its address space, and
 * asking for its contents must not invent an answer: reading absent memory
 * through the monitor allocates a zero page there, which the guest then
 * executes or reads as if it were real content. The owner of the memory says
 * ABSENT instead, and the asking side falls back to the copy it loaded from —
 * authoritative precisely because the guest never wrote it.
 */
#define VMR_PG_ABSENT 1		/* status: no such page; use your own copy   */
/*
 * The token is IN TRANSIT: the answering side's record says the ASKER holds
 * this page, and the asker is asking because its copy just left -- a serve
 * and a pull of one page crossing in flight, the loss the conserved-token
 * comment in vmhome names. The bytes land wherever the crossing lands them
 * within microseconds; the only correct answer is ASK AGAIN. Answering with
 * the retained (last-transmitted) copy here instead was pg3's terminal
 * one-behind: stale bytes injected over a live thread's stack at the join
 * window, because a transient was treated like the holder having died. The
 * destination answers INFLIGHT only while the holder's address space is
 * ALIVE, and for as long as the LANDING is in flight: while its own fault
 * service has a pull of the page active, ask-again is the only answer,
 * however long that takes (a fixed cap was measured wrong -- pg3 episodes
 * outlive any reasonable one, yet always land). With no pull active a young
 * episode (1200ms grace) still answers INFLIGHT; past that the crossing's
 * bytes died with a context mid-service, and the retained copy -- the
 * ws1-teardown last resort this branch was built for -- is the newest copy
 * in existence. A page of filler bytes follows the status like every GET
 * reply; the asker ignores it.
 */
#define VMR_PG_INFLIGHT 2
/*
 * With INFLIGHT (session 28) the reply carries two things the asker cannot
 * know by itself: vmr_pgrsp.why -- VMR_INFLIGHT_PULL if the answering side's
 * own pull of this page is landing there (a fresh take follows; keep
 * asking), 0 if only its young-episode clock holds the answer (its retained
 * copy follows) -- and vmr_pgrsp.gen, the answering side's CURRENT capture
 * count of the page. The source ends a claim on a landing it already has
 * only when why is 0 AND its landing's stamp is not behind that count:
 * nothing newer has been captured since the landing, so the fetch could
 * bring nothing the landing lacks. A landing behind the count (the split
 * brain of NOTES 6.1a: the source holding a copy while the guest writes)
 * keeps asking, and the arrival's stamp decides as before.
 */
#define VMR_INFLIGHT_PULL 1

struct vmr_pgrsp {
    uint32_t magic;
    int32_t status;
    uint32_t gen, why;
    struct vmr_mm_binding target;
    uint64_t addr;
    uint32_t op, reserved;
};
_Static_assert(sizeof(struct vmr_pgrsp)==64,"page response wire layout");
static inline int vmr_page_reply_matches(const struct vmr_pgreq *request,
        const struct vmr_pgrsp *reply)
{
    return reply->magic==VMR_PG_MAGIC && !reply->reserved &&
        vmr_binding_valid(&request->target) &&
        vmr_binding_equal(&request->target,&reply->target) &&
        reply->addr==request->addr && reply->op==request->op;
}

#endif /* _VMRPROTO_H */
