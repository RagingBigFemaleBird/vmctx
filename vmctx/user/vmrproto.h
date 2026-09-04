/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Wire protocol between vmremote (on the machine running the VM context) and
 * vmhome (on the machine whose kernel should service the syscalls).
 *
 * Both ends are x86-64 Linux, so structs are exchanged in host layout and
 * byte order deliberately — this is a lab protocol, not a portable one.
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
 * "VMR5": vmr_rsp grew host_pid (a clone reply names the child twice -- the
 * guest's namespace-relative pid in retval, the source machine's own name
 * for it in host_pid). The struct's size changed, so a VMR4 peer misframes
 * every reply.
 */
#define VMR_MAGIC 0x35524d56u	/* "VMR5" */

struct vmr_req {
	uint32_t magic;
	uint64_t nr;		/* syscall number, or exception vector       */
	uint64_t args[6];	/* raw guest arguments (pointers are guest-side) */
	uint64_t datalen;	/* bytes of input payload following this struct */
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

struct vmr_rsp {
	uint32_t magic;
	/*
	 * A clone reply. Non-zero means the child shares the parent context's
	 * backing object (a thread); zero means it gets its own (a process).
	 * The source's own kernel decided this by running the real clone/clone3
	 * -- the destination reads no guest memory and parses no clone flags.
	 * Meaningful only when the forwarded call was a clone; zero otherwise.
	 */
	int32_t  clone_thread;
	int64_t  retval;	/* what the guest should see in RAX          */
	/*
	 * A clone reply names the child twice. retval is the pid as the
	 * GUEST must see it -- relative to the parent's pid namespace, which
	 * a sandbox's CLONE_NEWPID makes a small number like 2. host_pid is
	 * the pid as the SOURCE MACHINE names it, and it is the name the
	 * destination must announce the child by (VMR_OP_CHILD): that
	 * announcement is addressed to the source's own kernel, whose ctls,
	 * kills and /proc reads know nothing of the guest's namespace.
	 * Zero when the call was not a clone (fall back to retval).
	 */
	int64_t  host_pid;
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
	 * Non-zero when the call mapped memory that something else can also
	 * reach, and map_off is where in the backing object it must come from.
	 *
	 * The far side names memory by address: a guest address is its own
	 * offset in the object, so two addresses are two pages. That is right
	 * for ordinary memory and wrong for shared memory, which is one page
	 * under two addresses. Only this side can tell the difference -- it
	 * holds the file the two mappings are of -- so it says so, and the far
	 * side maps what it is told without knowing why.
	 */
	uint32_t map_shared;
	uint64_t map_off;
	/*
	 * How much was mapped, when the call itself does not say. mmap is asked
	 * for a length and the far side reads it off the request; shmat is
	 * asked for a segment, and how big that segment is is a fact only this
	 * side holds. So it is reported rather than inferred, and the far side
	 * never learns that System V shared memory exists.
	 */
	uint64_t map_len;
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
};

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
 * lookup that fails is transient. This is said only when the owner's refcount
 * of live service contexts for the address space has gone from positive to
 * zero, which is an event and cannot be momentary (see as_life_ref()).
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
 * The destination's half of the two-phase hand-over: "the page you served me
 * is INSTALLED here now." args[0] = page, args[1] = low 16 bits of the
 * installed bytes' page_sum, echoing what the serve recorded -- the episode
 * guard, so a late ack from a previous hand-over of the same page cannot
 * retire a newer one. The source commits a single-page serve as IN TRANSIT
 * (not handed over) and only this ack settles it; until then every judgment
 * about the page answers "ask again", never "absent" -- the crossing lies
 * hx1 photographed all had the bytes on the wire when the record was
 * consulted. A lost or late ack costs the asker a bounded yield, never a
 * stale page. Reply: retval 0, no payload.
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
#define VMR_PG_MAGIC 0x48504d56u	/* "VMPH": vmr_pgrsp carries gen */
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
	uint32_t magic;
	uint32_t op;
	uint64_t addr;
	uint64_t len;		/* VMR_PG_PUTN only */
	/*
	 * Which context's memory. A guest that forks has several, and after the
	 * fork their address spaces diverge: a page must come from, and go back
	 * to, the one whose syscall is being executed.
	 */
	uint64_t ctx;
	/*
	 * Which shadow is asking, so a page taken from the guest can be asked
	 * for again later. See VMR_PG_HELLO_RECALL: this is the shadow's pid,
	 * and it names an address space rather than a context.
	 */
	uint64_t owner;
};

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
	int32_t  status;	/* 0 ok, VMR_PG_ABSENT/INFLIGHT, or -errno   */
	/*
	 * The take generation of the page that follows (see vmr_rsp.pggen):
	 * the destination's count of captures of this page, after the one
	 * this reply is. A retained-copy serve carries the generation of the
	 * bytes it retained, not the current count, so the source learns how
	 * old what it is being given is. 0 with ABSENT; with INFLIGHT the
	 * answering side's current capture count (see VMR_INFLIGHT_PULL).
	 */
	uint32_t gen;
	uint32_t why;		/* INFLIGHT: VMR_INFLIGHT_PULL or 0; else 0   */
};

#endif /* _VMRPROTO_H */
