/* SPDX-License-Identifier: GPL-2.0 */
/*
 * vmctx — native "VM context" for Linux (avm.md project).
 *
 * Userspace/kernel shared interface. This header is included verbatim by
 * both the kernel module and the userspace tools, so it must stay
 * free of kernel- or libc-specific types.
 *
 * Milestone 1a scope: capability detection, context lifecycle, hardware
 * bring-up (VMX-root / SVM enable + VMCS/VMCB allocation) and a complete
 * state dump for debugging. Guest entry (VMLAUNCH/VMRUN) is Milestone 1b.
 */
#ifndef _VMCTX_UAPI_H
#define _VMCTX_UAPI_H

#include <linux/types.h>
#include <linux/ioctl.h>

#define VMCTX_DEVICE      "vmctx"
#define VMCTX_DEV_PATH    "/dev/vmctx"
#define VMCTX_ABI_VERSION 1

/* Virtualization technology detected on the running CPU. */
enum vmctx_vendor {
	VMCTX_VENDOR_NONE  = 0,  /* no hardware virtualization usable   */
	VMCTX_VENDOR_INTEL = 1,  /* Intel VT-x (VMX)                    */
	VMCTX_VENDOR_AMD   = 2,  /* AMD-V (SVM)                         */
};

/* Bit flags describing what the module found / can do on this CPU. */
#define VMCTX_CAP_PRESENT      (1u << 0) /* VMX/SVM feature bit set        */
#define VMCTX_CAP_ENABLED      (1u << 1) /* firmware-enabled (not locked off) */
#define VMCTX_CAP_EPT_NPT      (1u << 2) /* EPT (Intel) / NPT (AMD) usable */
#define VMCTX_CAP_UNRESTRICTED (1u << 3) /* unrestricted guest (Intel)     */
#define VMCTX_CAP_BROUGHT_UP   (1u << 4) /* root operation entered OK      */
#define VMCTX_CAP_BACKEND      (1u << 5) /* a backend is registered, so
                                          * vmctx_run(2) actually works;
                                          * the CPU having the feature does
                                          * not mean this driver can use it */

/*
 * VMCTX_IOC_INFO — global, no context needed. Reports what the CPU offers.
 */
struct vmctx_info {
	__u32 abi_version;     /* == VMCTX_ABI_VERSION                     */
	__u32 vendor;          /* enum vmctx_vendor                        */
	__u32 caps;            /* VMCTX_CAP_* bitmask                      */
	__u32 nr_contexts;     /* live contexts right now                 */
	__u64 vmx_basic;       /* IA32_VMX_BASIC (Intel), else 0          */
	__u64 feat_ctl;        /* IA32_FEAT_CTL (Intel), else 0           */
	__u64 vm_cr;           /* VM_CR MSR (AMD), else 0                 */
	__u64 efer;            /* IA32_EFER                                */
	char  vendor_str[16];  /* CPUID vendor, NUL-terminated            */
};

/*
 * VMCTX_IOC_CREATE — create a context bound to the calling process.
 * Returns a context id (>=0) in .id on success.
 */
/*
 * The argument to vmctx_run(2).
 *
 * Here, once, because it was hand-declared in each of the eight tools that use
 * it and only one of them was updated when it gained backing_fd. The other
 * seven went on passing the older layout, so the kernel read that field out of
 * what they still thought was out_enters -- zero, which is *not* "no object":
 * fd 0 is standard input, and vc->backing_fd >= 0 accepted it.
 *
 * Anything that calls vmctx_run(2) must include this rather than describe it
 * again.
 */
/*
 * Only for userspace: inside the kernel this comes from <uapi/linux/vmctx.h>,
 * which is the ABI's real home. Defining it again here would be a second copy
 * that can drift, which is the problem this is fixing.
 */
#ifndef __KERNEL__
struct vmctx_run_config {
	__u64 flags;		/* VMCTX_FLAG_*                              */
	__u64 max_exits;	/* 0 = run until killed; else stop after N   */
	__u64 entry;		/* USERCODE: guest entry RIP (user address)  */
	__u64 stack;		/* USERCODE: guest RSP (user address)        */
	/*
	 * The shared object this context's memory is backed from, in which a
	 * guest virtual address is its own offset. Two contexts started with
	 * the same object see the same page at the same address, which is what
	 * sharing an address space means; a context given its own shares with
	 * nobody, which is the snapshot a fork wants. 0 or negative means
	 * private anonymous backing, which shares with nothing.
	 */
	__s32 backing_fd;
	/*
	 * A second object, shared with every context of the same program.
	 *
	 * backing_fd is this context's own memory: a thread is handed its
	 * parent's, a process a copy of it, and that copy is what fork means.
	 * Shared memory is the one thing a fork must *not* copy, and an offset
	 * cannot express it -- two offsets in two different objects are two
	 * pages however they are numbered. So a range the owner calls shared is
	 * mapped from this one instead, which parent and child hold alike.
	 *
	 * -1 for a context that shares nothing.
	 */
	__s32 shared_fd;
	/* outputs, filled in by the kernel before returning: */
	__u64 out_enters;
	__u64 out_exits;
	__u64 out_counter;
	__u64 out_faults;
	__u32 out_exit_reason;
	__s32 out_status;
};

/*
 * The layout is the ABI, so a change that misses a caller is exactly the
 * failure this header exists to prevent. Say the size out loud, and fail the
 * build rather than the run.
 */
/*
 * What a reply may do to the guest's address space, besides answering.
 *
 * A syscall that changes memory runs on the machine that owns the program, and
 * its effect has to reach the machine the program runs on. Before this it did
 * not: mmap, mprotect and munmap were forwarded to the source and applied to
 * the shadow's mirror only, while the guest's address space was conjured a page
 * at a time by the fault path, which maps whatever is touched read-write-exec.
 * So a PROT_NONE page was readable and an unmapped address was memory; see
 * tests/pf1.c, which measures precisely that.
 *
 * Deliberately not a list of syscalls: the monitor says what happened to the
 * address space in these terms and the context applies it, so shmat and
 * anything else that maps memory need nothing new.
 */
/* What this side knows about a faulting page; see vmctx_event.args. */
#define VMCTX_PGS_MAPPED	(1u << 0)
#define VMCTX_PGS_PRESENT	(1u << 1)
#define VMCTX_PGS_WRITABLE	(1u << 2)
#define VMCTX_PGS_UNKNOWN	(1u << 3)

/*
 * FAULT events, args[2]: the class of the mapping the fault was taken in,
 * filled only by the kernel's own mm hook (a service context's fault on the
 * owning side). A backend-reported guest fault carries 0 here -- no VALID bit
 * -- and a monitor that does not know these bits reads 0 and is unaffected.
 * The full account is with the definitions in include/uapi/linux/vmctx.h.
 */
#define VMCTX_PGC_VALID		(1u << 0)
#define VMCTX_PGC_FILE		(1u << 1)
#define VMCTX_PGC_SHARED	(1u << 2)
#define VMCTX_PGC_WRITE		(1u << 3)

#define VMCTX_MAP_NONE		0  /* the reply changes no mapping            */
#define VMCTX_MAP_SET		1  /* map [addr,len) from the backing object  */
#define VMCTX_MAP_PROT		2  /* change protection of [addr,len)         */
#define VMCTX_MAP_UNMAP		3  /* remove [addr,len)                       */
/*
 * Map from the *shared* object rather than the context's own. Same three words
 * -- address, length, offset -- and the only difference is which object they
 * are read from, which is the whole of what makes two contexts share a page.
 */
#define VMCTX_MAP_SET_SHARED	4
#define VMCTX_MAP_ZAP		5  /* drop the PAGES of [addr,len), keep the
				    * mappings: the guest-side application of a
				    * source MADV_DONTNEED -- the next touch
				    * faults and refetches the fresh content   */
/*
 * VMCTX_MAP_SET, and the pages of [addr,len) are made PRESENT before the
 * context runs again (MAP_POPULATE): the monitor has just put every one of
 * them into the backing object -- a chunk fetched around one fault -- and a
 * page that is present does not fault, so the fifteen neighbours cost no
 * VMEXIT and no monitor round trip when the program reaches them. The
 * monitor may name ONLY pages it has verified the object holds: populating
 * a hole would have shmem allocate a zero folio in the object (the session
 * 36 door), which is the one thing no fetch path may do.
 */
#define VMCTX_MAP_SET_POPULATE	6

struct vmctx_reply {
	__u32 action;		/* VMCTX_ACT_*                              */
	__u32 _pad;
	__u64 retval;		/* SYSCALL + DONE: value returned to guest   */
	__u32 map_op;		/* VMCTX_MAP_*                              */
	__u32 map_prot;		/* PROT_* for SET and PROT                   */
	__u64 map_addr;
	__u64 map_len;
	/*
	 * Where in the backing object the range comes from. Everywhere else a
	 * guest address is its own offset, which makes two addresses two pages
	 * and aliasing impossible; shared memory is aliasing, and only the side
	 * holding the file knows two mappings are one page. See the kernel's
	 * copy of this header.
	 */
	__u64 map_off;
};

_Static_assert(sizeof(struct vmctx_run_config) == 80,
	       "vmctx_run_config layout changed; every caller must be rebuilt");
/*
 * Same rule, and it matters more here: the kernel copies this in with
 * sizeof(), so a tool built against an older layout hands it a short buffer.
 * Three tools declared this struct by hand until it moved here.
 */
_Static_assert(sizeof(struct vmctx_reply) == 48,
	       "vmctx_reply layout changed; every caller must be rebuilt");
#endif	/* !__KERNEL__ */

#define VMCTX_CREATE_FLAG_BRINGUP (1u << 0) /* also enter root op + alloc VMCS/VMCB */

struct vmctx_create {
	__u32 flags;           /* VMCTX_CREATE_FLAG_*                      */
	__u32 id;              /* out: assigned context id                */
	__s32 target_pid;      /* process this context represents (0 = caller) */
	__u32 _pad;
};

/*
 * VMCTX_IOC_DUMP — dump one context's full state for debugging.
 * Set .id to the context id; kernel fills the rest.
 */
#define VMCTX_STATE_CREATED  0
#define VMCTX_STATE_BROUGHTUP 1  /* root op entered, VMCS/VMCB allocated */
#define VMCTX_STATE_ENTERED  2   /* (1b) guest entered at least once     */
#define VMCTX_STATE_EXITED   3   /* (1b) returned via VMEXIT             */
#define VMCTX_STATE_DEAD     4

struct vmctx_dump {
	__u32 id;              /* in: context id                          */
	__u32 state;           /* out: VMCTX_STATE_*                      */
	__s32 owner_pid;       /* out                                     */
	__u32 vendor;          /* out: enum vmctx_vendor                  */

	__u64 vmcs_pa;         /* out: physical addr of VMCS/VMCB (0 if none) */
	__u64 vmcs_rev;        /* out: VMCS revision id / VMCB reserved   */
	__u64 last_exit_reason;/* out: (1b) last VMEXIT reason, else ~0   */
	__u64 last_exit_qual;  /* out: (1b) exit qualification            */

	/* Host control registers captured at bring-up, for debugging. */
	__u64 host_cr0, host_cr3, host_cr4;
	__u64 host_efer;
	__u64 vmx_basic, feat_ctl, vm_cr;

	__u32 flags;           /* VMCTX_CAP_* effective for this context   */
	__u32 _pad;
};

/*
 * VMCTX_IOC_SELFTEST — Milestone 1b step 0: actually enter guest mode.
 * Builds a tiny sandboxed guest (NPT-mapped 16 KiB window), runs a known
 * payload via VMRUN, takes the #VMEXIT, and reports what happened. This is
 * the hardware proof that guest entry works, before running real programs.
 *
 * The canned guest executes (16-bit real mode):
 *     mov ax, 0x1234 ; mov [0x100], ax ; vmmcall
 * so on success: exit_code == 0x081 (VMMCALL), guest_rax low16 == 0x1234,
 * and mem_marker == 0x1234 (proves the guest read/wrote NPT-mapped memory).
 */
struct vmctx_selftest {
	__u32 result;       /* out: 0 = ran and exited as expected; <0 = errno */
	__u32 exit_code;    /* out: VMCB exit_code (0x081 VMMCALL expected)     */
	__u64 exit_info_1;  /* out: VMCB EXITINFO1                              */
	__u64 exit_info_2;  /* out: VMCB EXITINFO2                              */
	__u64 guest_rax;    /* out: guest RAX at exit (expect low16 0x1234)     */
	__u64 guest_rip;    /* out: guest RIP at exit                           */
	__u64 mem_marker;   /* out: word the guest wrote to guest-mem 0x100     */
	__u64 vmcb_pa;      /* out: physical addr of the VMCB used              */
	__u64 npt_pml4_pa;  /* out: physical addr of the NPT root               */
	__u32 vendor;       /* out: enum vmctx_vendor                          */
	__u32 _pad;
};

#define VMCTX_IOC_MAGIC 0xB7

#define VMCTX_IOC_INFO   _IOR(VMCTX_IOC_MAGIC, 1, struct vmctx_info)
#define VMCTX_IOC_CREATE _IOWR(VMCTX_IOC_MAGIC, 2, struct vmctx_create)
#define VMCTX_IOC_DUMP   _IOWR(VMCTX_IOC_MAGIC, 3, struct vmctx_dump)
#define VMCTX_IOC_DESTROY _IOW(VMCTX_IOC_MAGIC, 4, __u32) /* arg = id */
#define VMCTX_IOC_SELFTEST _IOWR(VMCTX_IOC_MAGIC, 5, struct vmctx_selftest)

#endif /* _VMCTX_UAPI_H */
