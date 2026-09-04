// SPDX-License-Identifier: GPL-2.0
/*
 * vmctx — AMD SVM / Intel VMX hardware backend for Linux VM contexts.
 *
 * Registers with the kernel's native VM-context core (kernel/vmctx.c) and
 * provides:
 *   - capability detection (VT-x / SVM) and /dev/vmctx debug ioctls
 *     (INFO, CREATE, DUMP, SELFTEST) with a full state dump;
 *   - a transient VMX-root / SVM-enable bring-up test;
 *   - a sandboxed VMRUN selftest (16-bit guest in a 16 KiB NPT window);
 *   - USERCODE mode: runs the calling process's own user code as a long-mode
 *     guest on the process's page tables, servicing guest page faults through
 *     the host fault path and dispatching guest syscalls through the kernel's
 *     generic syscall table (vmctx_dispatch_syscall) — no per-syscall code.
 *
 * Safety: SHUTDOWN is intercepted so a guest triple-fault cannot reset the
 * CPU; host FS/GS/TR/LDTR and syscall MSRs are preserved with VMSAVE/VMLOAD
 * around VMRUN; the bring-up test leaves global CPU state untouched.
 */
#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/fs.h>
#include <linux/miscdevice.h>
#include <linux/uaccess.h>
#include <linux/slab.h>
#include <linux/mutex.h>
#include <linux/list.h>
#include <linux/idr.h>
#include <linux/sched.h>
#include <linux/preempt.h>
#include <linux/mm.h>
#include <linux/mman.h>	/* MAP_ and PROT_ flags, for demand-backing a fault */
#include <linux/vmctx.h>
#include <asm/msr.h>
#include <asm/msr-index.h>
#include <asm/io.h>
#include <asm/processor-flags.h>
#include <asm/cpufeature.h>
#include <asm/svm.h>
#include <asm/vmx.h>
#include <asm/desc.h>
#include <asm/tlbflush.h>
#include <asm/segment.h>
#include <asm/ptrace.h>
#include <asm/fpu/api.h>
#include <asm/fpu/xcr.h>	/* xgetbv(), for the #GP report */

#include "vmctx_uapi.h"

#define VMCTX_PR "vmctx: "

/* ---- low-level helpers (self-contained; version-robust) --------------- */

static inline u64 rd_cr0(void){ u64 v; asm volatile("mov %%cr0,%0":"=r"(v)); return v; }
static inline u64 rd_cr3(void){ u64 v; asm volatile("mov %%cr3,%0":"=r"(v)); return v; }
static inline u64 rd_cr4(void){ u64 v; asm volatile("mov %%cr4,%0":"=r"(v)); return v; }
static inline void wr_cr4(u64 v){ asm volatile("mov %0,%%cr4"::"r"(v):"memory"); }

/* returns 0 on success, 1 on VMfail (CF/ZF set) */
static inline int do_vmxon(u64 pa)
{
	u8 err;
	asm volatile("vmxon %[pa]; setna %[err]"
		     : [err] "=q" (err)
		     : [pa] "m" (pa)
		     : "cc", "memory");
	return err;
}
static inline void do_vmxoff(void)
{
	asm volatile("vmxoff" ::: "cc", "memory");
}

static void vmctx_cpuid(u32 leaf, u32 subleaf, u32 *a, u32 *b, u32 *c, u32 *d)
{
	u32 ra = leaf, rb = 0, rc = subleaf, rd = 0;
	asm volatile("cpuid"
		     : "+a"(ra), "+b"(rb), "+c"(rc), "+d"(rd));
	*a = ra; *b = rb; *c = rc; *d = rd;
}

/* ---- global capability state (filled once at init) -------------------- */

struct vmctx_caps {
	u32 vendor;          /* enum vmctx_vendor */
	u32 caps;            /* VMCTX_CAP_* (present/enabled/ept-npt/...) */
	u64 vmx_basic;
	u64 feat_ctl;
	u64 vm_cr;
	u64 efer;
	char vendor_str[16];
};

static struct vmctx_caps g_caps;

/*
 * The guest's SSE/x87 registers are the task's own.
 *
 * Nothing in the VMCS or the VMCB carries them: whatever is in the CPU when the
 * guest is entered is what the guest gets, and whatever the guest leaves there
 * is what the host finds. That is exactly right for this design — the guest is
 * the process's user context, so its FPU registers *are* the process's — but
 * only if the process's registers are actually loaded when the guest runs.
 *
 * Between two guest entries the task blocks waiting for its monitor, so it is
 * scheduled away and back. Coming back, the kernel leaves the FPU registers
 * belonging to whoever ran in between and sets TIF_NEED_FPU_LOAD, meaning "this
 * task's registers are in memory, load them before returning to user". The
 * guest is entered before that happens, so it ran with another task's registers
 * — and its own writes to them were then discarded by the eventual reload.
 *
 * musl finds this instantly. It zeroes a new malloc meta with
 *
 *	pxor   %xmm0,%xmm0
 *	mov    %rax,ctx.avail_meta	<- this store page-faults
 *	movups %xmm0,(%r10)
 *
 * and a fault between the two lands the guest here. On the way back xmm0 holds
 * whatever the host left in it, so the "zeroed" meta comes back non-zero, and
 * musl's own assertion catches it a few instructions later and executes hlt —
 * which arrives as a guest #GP at an address in libc, with a heap that looks
 * corrupted by nothing.
 *
 * Load them, with preemption already disabled so they stay loaded until the
 * entry. Nothing is needed on the way out: the registers are then live and
 * owned by this task, so a context switch saves them and the return to user
 * mode keeps them, which is what "the guest is the process" requires.
 */
static inline void vmctx_load_task_fpu(void)
{
	fpregs_assert_state_consistent();
	if (test_thread_flag(TIF_NEED_FPU_LOAD))
		switch_fpu_return();
}

/* ---- per-context object ----------------------------------------------- */

struct vmctx {
	struct list_head link;
	int id;
	pid_t owner_pid;
	u32 state;           /* VMCTX_STATE_* */
	u32 flags;           /* effective VMCTX_CAP_* */

	void *vmcs;          /* VMCS (Intel) / VMCB (AMD) region page */
	u64 vmcs_pa;
	u64 vmcs_rev;

	/* host state captured at bring-up */
	u64 host_cr0, host_cr3, host_cr4, host_efer;

	u64 last_exit_reason;
	u64 last_exit_qual;
};

static DEFINE_MUTEX(g_lock);
static LIST_HEAD(g_contexts);
static DEFINE_IDR(g_idr);
static u32 g_nr_contexts;

static struct vmctx *ctx_find(u32 id)
{
	return idr_find(&g_idr, id);
}

/* ---- capability detection (safe: read-only MSR/CPUID) ----------------- */

static void detect_caps(void)
{
	u32 a, b, c, d;
	u64 val;

	memset(&g_caps, 0, sizeof(g_caps));

	/* CPUID vendor string: leaf 0 -> EBX,EDX,ECX */
	vmctx_cpuid(0, 0, &a, &b, &c, &d);
	memcpy(g_caps.vendor_str + 0, &b, 4);
	memcpy(g_caps.vendor_str + 4, &d, 4);
	memcpy(g_caps.vendor_str + 8, &c, 4);
	g_caps.vendor_str[12] = '\0';

	if (!rdmsrq_safe(MSR_EFER, &val))
		g_caps.efer = val;

	if (boot_cpu_has(X86_FEATURE_VMX)) {
		g_caps.vendor = VMCTX_VENDOR_INTEL;
		g_caps.caps |= VMCTX_CAP_PRESENT;

		if (!rdmsrq_safe(MSR_IA32_FEAT_CTL, &val)) {
			g_caps.feat_ctl = val;
			/* usable iff locked AND vmxon-outside-smx enabled,
			 * OR not locked at all (we could lock it ourselves). */
			if (!(val & FEAT_CTL_LOCKED) ||
			    (val & FEAT_CTL_VMX_ENABLED_OUTSIDE_SMX))
				g_caps.caps |= VMCTX_CAP_ENABLED;
		}
		if (!rdmsrq_safe(MSR_IA32_VMX_BASIC, &val))
			g_caps.vmx_basic = val;

		/* EPT / unrestricted-guest live in the secondary procbased
		 * controls MSR; probe conservatively. */
		if (!rdmsrq_safe(MSR_IA32_VMX_PROCBASED_CTLS2, &val)) {
			if (val & (1ULL << (32 + 1)))  /* enable EPT */
				g_caps.caps |= VMCTX_CAP_EPT_NPT;
			if (val & (1ULL << (32 + 7)))  /* unrestricted guest */
				g_caps.caps |= VMCTX_CAP_UNRESTRICTED;
		}
	} else if (boot_cpu_has(X86_FEATURE_SVM)) {
		g_caps.vendor = VMCTX_VENDOR_AMD;
		g_caps.caps |= VMCTX_CAP_PRESENT;

		if (!rdmsrq_safe(MSR_VM_CR, &val)) {
			g_caps.vm_cr = val;
			/* SVMDIS = bit 4; if clear (or lock bit3 clear) SVM is
			 * available. */
			if (!(val & (1ULL << 4)))
				g_caps.caps |= VMCTX_CAP_ENABLED;
		} else {
			/* VM_CR readable on all SVM parts; if it faults assume
			 * enabled and let bring-up be the real test. */
			g_caps.caps |= VMCTX_CAP_ENABLED;
		}
		/* Nested paging: CPUID 0x8000000A EDX bit 0. */
		vmctx_cpuid(0x8000000A, 0, &a, &b, &c, &d);
		if (d & 1)
			g_caps.caps |= VMCTX_CAP_EPT_NPT;
	} else {
		g_caps.vendor = VMCTX_VENDOR_NONE;
	}

	pr_info(VMCTX_PR "cpu=%s vendor=%u caps=0x%x vmx_basic=0x%llx feat_ctl=0x%llx vm_cr=0x%llx\n",
		g_caps.vendor_str, g_caps.vendor, g_caps.caps,
		g_caps.vmx_basic, g_caps.feat_ctl, g_caps.vm_cr);
}

/* ---- hardware bring-up (transient, per-cpu, opt-in) ------------------- */

/*
 * Enter VMX-root operation, VMPTR-format the VMCS region, then leave.
 * Runs pinned with preemption disabled. Returns 0 on success.
 */
static int bringup_intel(struct vmctx *c)
{
	u64 cr4, cr4_saved;
	u32 rev;
	int ret = 0;

	if (!c->vmcs)
		return -ENOMEM;

	/* VMCS revision id = bits [30:0] of IA32_VMX_BASIC. */
	rev = (u32)(g_caps.vmx_basic & 0x7fffffff);
	*(u32 *)c->vmcs = rev;         /* first dword of VMXON/VMCS region */
	c->vmcs_rev = rev;

	preempt_disable();

	c->host_cr0  = rd_cr0();
	c->host_cr3  = rd_cr3();
	cr4_saved    = rd_cr4();
	c->host_cr4  = cr4_saved;
	c->host_efer = g_caps.efer;

	/* CR4.VMXE must be set before VMXON. */
	cr4 = cr4_saved | X86_CR4_VMXE;
	wr_cr4(cr4);

	if (do_vmxon(c->vmcs_pa)) {
		ret = -EIO;                /* VMXON VMfail (locked out / nested) */
		wr_cr4(cr4_saved);
		goto out;
	}

	/* We are now in VMX-root operation. Nothing to launch yet (1b);
	 * this proves the bring-up path works. Leave cleanly. */
	do_vmxoff();
	wr_cr4(cr4_saved);            /* restore CR4.VMXE to prior value */

	c->flags |= VMCTX_CAP_BROUGHT_UP;
	c->state = VMCTX_STATE_BROUGHTUP;
out:
	preempt_enable();
	return ret;
}

/*
 * Enable EFER.SVME, point VM_HSAVE_PA at a host save page, then disable.
 * Proves SVM enable works without executing VMRUN (1b).
 */
static int bringup_amd(struct vmctx *c)
{
	u64 efer, vm_cr;
	void *hsave;
	int ret = 0;

	if (!c->vmcs)
		return -ENOMEM;

	hsave = (void *)get_zeroed_page(GFP_KERNEL);
	if (!hsave)
		return -ENOMEM;

	preempt_disable();

	c->host_cr0  = rd_cr0();
	c->host_cr3  = rd_cr3();
	c->host_cr4  = rd_cr4();

	if (rdmsrq_safe(MSR_VM_CR, &vm_cr) == 0 && (vm_cr & (1ULL << 4))) {
		ret = -EPERM;              /* SVMDIS set: disabled in BIOS */
		goto out;
	}
	if (rdmsrq_safe(MSR_EFER, &efer)) {
		ret = -EIO;
		goto out;
	}
	c->host_efer = efer;

	if (wrmsrq_safe(MSR_EFER, efer | EFER_SVME)) {
		ret = -EIO;                /* SVME enable faulted */
		goto out;
	}
	/* Set host save area (required before any VMRUN in 1b). */
	wrmsrq_safe(MSR_VM_HSAVE_PA, virt_to_phys(hsave));

	/* VMCB revision area: AMD VMCB has no revision id like VMCS; the
	 * first field is control bits. We simply record the allocation. */
	c->vmcs_rev = 0;

	/* Disable again to restore prior global state. */
	wrmsrq_safe(MSR_EFER, efer);

	c->flags |= VMCTX_CAP_BROUGHT_UP;
	c->state = VMCTX_STATE_BROUGHTUP;
out:
	preempt_enable();
	free_page((unsigned long)hsave);
	return ret;
}

static int ctx_bringup(struct vmctx *c)
{
	if (!(g_caps.caps & VMCTX_CAP_PRESENT))
		return -ENODEV;
	if (!(g_caps.caps & VMCTX_CAP_ENABLED))
		return -EPERM;

	/* Allocate the VMCS/VMCB region (4K, phys-contiguous). */
	if (!c->vmcs) {
		c->vmcs = (void *)get_zeroed_page(GFP_KERNEL);
		if (!c->vmcs)
			return -ENOMEM;
		c->vmcs_pa = virt_to_phys(c->vmcs);
	}

	if (g_caps.vendor == VMCTX_VENDOR_INTEL)
		return bringup_intel(c);
	if (g_caps.vendor == VMCTX_VENDOR_AMD)
		return bringup_amd(c);
	return -ENODEV;
}

/* ---- Milestone 1b step 0: SVM guest-entry selftest -------------------- */
/*
 * Enter AMD SVM guest mode with a tiny, sandboxed guest, run it, and take
 * the #VMEXIT. Safety properties:
 *   - NPT maps ONLY a 16 KiB guest-physical window to our own pages, so a
 *     stray guest access faults (NPF) back to us instead of touching host
 *     RAM. Nothing is identity-mapped.
 *   - SHUTDOWN and HLT are intercepted, so a guest triple-fault returns to
 *     us cleanly instead of resetting the physical CPU.
 *   - Host FS/GS/TR/LDTR and syscall MSRs are saved with VMSAVE and
 *     restored with VMLOAD around VMRUN (VMRUN does not preserve them);
 *     getting this wrong corrupts the host, so it is done explicitly.
 */

/* Guest payload, 16-bit real mode, at guest-physical 0:
 *   B8 34 12       mov ax, 0x1234
 *   A3 00 01       mov [0x0100], ax
 *   0F 01 D9       vmmcall
 */
static const u8 guest_payload[] = {
	0xB8, 0x34, 0x12,
	0xA3, 0x00, 0x01,
	0x0F, 0x01, 0xD9,
};

/* Counter-loop payload for the native process backend, 16-bit real mode:
 *   FF 06 00 01    inc word [0x0100]
 *   0F 01 D9       vmmcall            ; yield to host each iteration
 *   EB F7          jmp  -9            ; back to the inc
 * The host advances RIP past VMMCALL (3 bytes) each exit, so the guest
 * runs forever, incrementing [0x100], until the task is killed or hits
 * max_exits. This lets the VM-context *process* be observed, scheduled
 * and signalled like any other.
 */
static const u8 counter_payload[] = {
	0xFF, 0x06, 0x00, 0x01,
	0x0F, 0x01, 0xD9,
	0xEB, 0xF7,
};

#define NPT_PTE_P   (1ULL << 0)
#define NPT_PTE_RW  (1ULL << 1)
#define NPT_PTE_US  (1ULL << 2)
#define NPT_PTE_BITS (NPT_PTE_P | NPT_PTE_RW | NPT_PTE_US)

static void seg_set(struct vmcb_seg *s, u16 sel, u16 attrib, u32 limit, u64 base)
{
	s->selector = sel;
	s->attrib   = attrib;
	s->limit    = limit;
	s->base     = base;
}

/*
 * Guest general-purpose registers that VMRUN does NOT save/restore (unlike
 * RAX/RSP/RIP/RFLAGS which live in the VMCB). We load these into the CPU
 * before VMRUN and read them back after, so guest syscall arguments
 * (RDI/RSI/RDX/R10/R8/R9) are available to forward. Layout is mirrored by
 * fixed byte offsets in the VMRUN asm — do not reorder.
 */
/*
 * A guest that is neither faulting nor asking for anything.
 *
 * With host interrupts intercepted, a guest looping in userspace still exits on
 * every timer tick — so it is visibly alive and invisibly stuck, and the logs
 * show nothing at all. That is the least debuggable state the thing has: no
 * syscall, no fault, no message. Count consecutive interrupt-only exits and say
 * where it is, once, with the address it keeps coming back to.
 */
/*
 * A general-protection fault in the guest. Nothing here can fix one — it means
 * the guest executed something the CPU refused — but it can be named. Print
 * where it happened and the bytes that did it, which is the whole difference
 * between "unrecoverable #PF at 0xd0" and an instruction to go and look at.
 */
/*
 * Is the instruction the guest just trapped on actually SYSCALL?
 *
 * A guest's SYSCALL is disabled and arrives here as #UD -- that is how a syscall
 * leaves the guest at all. But #UD is not a private signal between the guest and
 * this code: any undefined opcode raises it, and `ud2` is two bytes exactly like
 * SYSCALL. Without this check the two are indistinguishable, and a program
 * executing ud2 does not get SIGILL -- it makes a system call, with whatever
 * number RAX happened to hold and whatever the argument registers happened to
 * contain, and then carries on past the instruction because RIP was stepped over
 * two bytes. tests/tr1.c raises one and reported ENOSYS coming back from a call
 * the program never made.
 *
 * Two bytes read from the address that just executed, so the page is present and
 * this is a user copy rather than a walk. The guest shares this task's address
 * space, so when the caller is the context itself -- which it is on every exit --
 * copy_from_user is the whole cost.
 *
 * Unreadable means something is wrong enough that guessing is worse than
 * stopping: it is reported as a bad #UD rather than run as a syscall.
 */
static bool vmctx_insn_is_syscall(struct task_struct *t, u64 rip)
{
	u8 b[2];

	if (t == current) {
		if (copy_from_user(b, (void __user *)(unsigned long)rip, sizeof(b)))
			return false;
	} else if (access_process_vm(t, rip, b, sizeof(b), 0) != sizeof(b)) {
		return false;
	}
	return b[0] == 0x0f && b[1] == 0x05;
}

/*
 * A #UD that is not a syscall. The guest raised a genuine invalid-opcode
 * exception -- ud2, or a real bad instruction -- and there is nowhere to send it
 * yet: nothing in this design delivers a fault to a guest as a signal. So it
 * ends the context, and it says what it found, because the alternative it
 * replaces was to execute it as a system call.
 */
static void vmctx_report_ud(struct task_struct *t, u64 rip, u64 rsp, u64 rax)
{
	unsigned char insn[16];
	int got;

	pr_warn(VMCTX_PR "guest #UD pid %d at rip 0x%llx rsp 0x%llx: not a SYSCALL\n",
		task_pid_nr(t), rip, rsp);
	got = access_process_vm(t, rip, insn, sizeof(insn), 0);
	if (got == sizeof(insn))
		pr_warn(VMCTX_PR "  insn @rip: %*ph (0f 0b is ud2)\n",
			(int)sizeof(insn), insn);
	else
		pr_warn(VMCTX_PR "  insn @rip: unreadable (%d)\n", got);
	pr_warn(VMCTX_PR "  rax=0x%llx -- had this been taken for a syscall, that is the call it would have made\n",
		rax);
}

/*
 * This backend has no notion of a signal, and must not acquire one.
 *
 * There used to be a vmctx_deliver_fault() here that turned a vector into a
 * signal with force_sig_fault(). It had no callers by the time it was removed,
 * and its comment still claimed it was what made tests/pf1.c and tests/gp1.c
 * work. It was not: an exception is reported to the owner as a raw vector and
 * the owner answers with a register set, which is the only arrangement that
 * survives the destination not being Linux. "SIGSEGV" is a fact about the
 * machine that owns the program, and nothing on this side is entitled to know
 * it.
 */

static void vmctx_report_gp(struct task_struct *t, u64 rip, u64 rsp, u64 err)
{
	struct pt_regs *r = task_pt_regs(t);
	unsigned char insn[16];
	int got;

	pr_warn(VMCTX_PR "guest #GP pid %d at rip 0x%llx rsp 0x%llx (error 0x%llx)\n",
		task_pid_nr(t), rip, rsp, err);
	/*
	 * The state that decides whether a VEX instruction is legal at all.
	 *
	 * The #GP that truncates a threaded run lands on `vpcmpeqb
	 * (%rdi),%ymm0,%ymm1` -- VEX-encoded AVX2 out of glibc's string code --
	 * with a canonical operand and an error code of zero. That combination
	 * is not an addressing fault; it is the processor refusing the encoding.
	 * AVX needs CR4.OSXSAVE and needs XCR0 to have both the SSE and the AVX
	 * state bits: XCR0 with AVX set and SSE clear is an illegal combination
	 * and raises exactly this. On SVM the guest uses the host's XCR0, so
	 * what matters is what this CPU holds at the moment the guest ran, which
	 * is knowable only here.
	 *
	 * Printed rather than reasoned about, because four explanations for this
	 * family of failure have already been argued convincingly and measured
	 * false.
	 */
	{
		u64 xcr0 = 0;

		if (boot_cpu_has(X86_FEATURE_XSAVE))
			xcr0 = xgetbv(0);
		pr_warn(VMCTX_PR "  host cr0=0x%lx cr4=0x%lx xcr0=0x%llx (xsave %d, avx %d, osxsave %d)\n",
			rd_cr0(), rd_cr4(), xcr0,
			boot_cpu_has(X86_FEATURE_XSAVE) ? 1 : 0,
			boot_cpu_has(X86_FEATURE_AVX) ? 1 : 0,
			(rd_cr4() & X86_CR4_OSXSAVE) ? 1 : 0);
	}
	got = access_process_vm(t, rip, insn, sizeof(insn), 0);
	if (got == sizeof(insn))
		pr_warn(VMCTX_PR "  insn @rip: %*ph\n", (int)sizeof(insn), insn);
	else
		pr_warn(VMCTX_PR "  insn @rip: unreadable (%d)\n", got);
	/*
	 * The registers, and the memory the faulting instruction was reading.
	 * A #GP on a hlt is what libc's assertions compile to, so the useful
	 * question is not "where" but "what did it find there" — and the
	 * pointer it disbelieved is in a register.
	 */
	pr_warn(VMCTX_PR "  rax=%016lx rbx=%016lx rcx=%016lx rdx=%016lx\n",
		r->ax, r->bx, r->cx, r->dx);
	pr_warn(VMCTX_PR "  rsi=%016lx rdi=%016lx rbp=%016lx r8 =%016lx\n",
		r->si, r->di, r->bp, r->r8);
	pr_warn(VMCTX_PR "  r9 =%016lx r10=%016lx r11=%016lx r12=%016lx\n",
		r->r9, r->r10, r->r11, r->r12);
	pr_warn(VMCTX_PR "  r13=%016lx r14=%016lx r15=%016lx\n",
		r->r13, r->r14, r->r15);
	{
		unsigned long ptrs[4] = { r->r11, r->r12, r->r13, r->bx };
		const char *nm[4] = { "r11", "r12", "r13", "rbx" };
		unsigned char buf[64];
		int i;

		for (i = 0; i < 4; i++) {
			if (ptrs[i] < 0x1000 || ptrs[i] > 0x7fffffffffffUL)
				continue;
			if (access_process_vm(t, ptrs[i], buf, sizeof(buf), 0) ==
			    sizeof(buf))
				pr_warn(VMCTX_PR "  [%s=0x%lx]: %*ph\n", nm[i],
					ptrs[i], (int)sizeof(buf), buf);
		}
	}
}

struct vmctx_spin {
	unsigned long ticks;   /* interrupt-only exits since the last real one */
	int  reported;
	u64  first_rip;
};

#define VMCTX_SPIN_LOUD 2000

static void vmctx_note_spin(struct task_struct *t, struct vmctx_spin *sp, u64 rip)
{
	if (!sp->ticks)
		sp->first_rip = rip;
	sp->ticks++;
	if (sp->ticks == VMCTX_SPIN_LOUD && !sp->reported) {
		sp->reported = 1;
		pr_warn(VMCTX_PR "pid %d: guest has taken %u interrupts with no "
			"syscall and no fault — spinning in userspace around "
			"rip 0x%llx (now 0x%llx)\n",
			task_pid_nr(t), VMCTX_SPIN_LOUD, sp->first_rip, rip);
	}
}

static inline void vmctx_spin_reset(struct vmctx_spin *sp)
{
	sp->ticks = 0;
	sp->reported = 0;
}

struct svm_gctx {
	__u64 vmcb_pa;       /* 0   */
	__u64 host_vmcb_pa;  /* 8   */
	__u64 rbx, rcx, rdx, rsi, rdi, rbp;          /* 16,24,32,40,48,56 */
	__u64 r8, r9, r10, r11, r12, r13, r14, r15;  /* 64 .. 120         */
};

/*
 * One host write to the guest's RBP, for the ring in struct svm_run below.
 * `ptbp` is what pt_regs held at the last frame publish, so a value that came
 * from a monitor's SETREGS can be told apart from one this module computed.
 */
struct vmctx_bpw {
	u64 obp, nbp, osp, nsp, rip, ptbp;
	u32 tag;
};

/*
 * One syscall exit, for vmctx_note_sysexit() below.
 *
 * th9 fails with one write too many, the extra one carrying the previous
 * emission's bytes. RIP alone cannot separate "the instruction ran twice" from
 * "the loop went round again": its emit() has a single call site, so every
 * iteration leaves from the same address. What differs is the loop counter,
 * which lives in a callee-saved register across the call -- so the callee-saved
 * set is folded together and kept beside RIP and RSP.
 */
struct vmctx_sysmark {
	u64 rip, rsp, fold;
	u64 nr, arg1;		/* which call, and the buffer it was given */
	u64 entry;		/* what the guest was entered with           */
};

struct svm_run {
	void *hsave;       /* VM_HSAVE_PA target (VMRUN host-state save)    */
	void *host_vmcb;   /* VMSAVE/VMLOAD of host FS/GS/etc               */
	void *vmcb;        /* the guest VMCB                                */
	void *npt[4];      /* pml4, pdpt, pd, pt                            */
	void *code;        /* guest-physical 0x0000 window page            */
	void *stack;       /* guest-physical 0x1000                        */
	void *data;        /* guest-physical 0x2000                        */
	void *misc;        /* guest-physical 0x3000                        */
	struct svm_gctx gctx;  /* guest GPRs for USERCODE mode              */

	/*
	 * The task's pt_regs is kept as the canonical guest register frame
	 * between guest runs, so the kernel's existing machinery (signal
	 * delivery, syscall restart, ptrace, /proc) operates on guest state
	 * instead of a stale frame. entry_frame is the task's real user frame
	 * (the vmctx_run(2) call site), restored when the context ends.
	 */
	struct pt_regs entry_frame;
	int frame_saved;       /* entry_frame captured                      */
	struct vmctx_sysmark sysmark[16];  /* recent syscall exits           */
	u64 entry_rip;         /* what the VMCB held at the last VMRUN       */
	unsigned sysmark_i;
	int regs_live;         /* pt_regs now holds guest state             */
	/*
	 * Where the guest was at each of the last few exits. A guest that dies
	 * executing a zero page gives no clue how it got there: this shows
	 * whether it fell into the page from the instruction before it or
	 * arrived by a control transfer (a restored signal frame, a bad
	 * indirect call), which are different bugs.
	 */
	u64 riphist[8];
	/*
	 * And with what stack. A guest that dies reading a value its own frame
	 * should hold is two different defects depending on whether the *word*
	 * is wrong or the *frame pointer* is: the first is a page that changed
	 * machines with stale contents, the second is a register restored from
	 * the wrong snapshot, and the faulting instruction looks identical
	 * either way. rsp and rbp at each exit separate them, because the frame
	 * arithmetic of the function being executed relates the two.
	 */
	u64 rsphist[8];
	u64 rbphist[8];
	/*
	 * And what the guest's OWN mapping held at that rbp, at that exit.
	 *
	 * Part XI §5.2's fork, and the only thing left that can decide it. The
	 * death is a `pop %rbp` that produced the wrong value with the right
	 * word in front of it *at the time of the dump* -- and a dump is taken
	 * after the fact. Either the word was different when the pop read it
	 * (a stack page that changed machines mid-frame, which is priority
	 * one's family) or it was not and the register was lost in the GPR
	 * save around VMRUN. A ring of the word itself, sampled at every exit,
	 * is the difference between those two.
	 *
	 * Read with copy_from_user_nofault() so it can never instantiate the
	 * page it reports, and only while rsp is inside the page named by
	 * vmctx_bpwatch, so it costs nothing in every other run. bpwok says
	 * whether the read succeeded: "the page was not here at that exit" is
	 * itself an answer, and one a zero would hide.
	 */
	u64 bpwordhist[8];
	u8  bpwordok[8];
	u32 riphist_i;
	/*
	 * Part X §4 left a register-restore defect: rbp one frame old while rsp
	 * is current. rsp lives in the VMCB and rbp in r->gctx, so the two can
	 * only diverge if something on *this* side wrote gctx between an exit
	 * and the next entry -- nothing the guest does can produce it.
	 *
	 * That makes it answerable by bookkeeping alone: remember what the guest
	 * left in gctx.rbp at the exit, compare it with what it is about to be
	 * entered with, and record every difference together with the tag of
	 * whichever ptregs_to_guest() call site last ran and what pt_regs held
	 * when it did. A guest that pops its frame pointer and is then entered
	 * with the pre-pop value appears here as one line naming the writer.
	 * The rsp/rbp ring above cannot say it: it only sees exits, so a value
	 * changed between two exits looks the same whoever changed it.
	 */
	u64 bp_at_exit, sp_at_exit, ip_at_exit;
	int have_exited;
	u32 last_wtag;         /* which ptregs_to_guest() site ran last      */
	u64 wtag_ptbp;         /* and what pt_regs->bp held when it did      */
	struct vmctx_bpw bpw[16];
	u32 bpw_i;
	long last_sysnr;       /* syscall the guest just made, else -1      */
	u16  sysring[24];      /* recent guest syscall numbers (debug)      */
	u8   sysring_i;
	/*
	 * The second chance for a fault the monitor declined and this kernel
	 * then could not serve. The usual reason the local service fails is
	 * that the mapping vanished BETWEEN the monitor's decline and the
	 * service -- a racing stale vacate's punch (netsurf's font scan,
	 * tests/fc1) -- and by then the absence is a settled fact the
	 * monitor's construction record can recognize and repair. So the
	 * fault is REPORTED AGAIN, bounded per address so a monitor with no
	 * answer still ends the context rather than looping.
	 */
	u64 unrec_addr;
	u32 unrec_n;
	struct vmctx_spin spin;
};

static void svm_run_free(struct svm_run *r)
{
	int i;
	if (r->hsave)     free_page((unsigned long)r->hsave);
	if (r->host_vmcb) free_page((unsigned long)r->host_vmcb);
	if (r->vmcb)      free_page((unsigned long)r->vmcb);
	for (i = 0; i < 4; i++)
		if (r->npt[i]) free_page((unsigned long)r->npt[i]);
	if (r->code)  free_page((unsigned long)r->code);
	if (r->stack) free_page((unsigned long)r->stack);
	if (r->data)  free_page((unsigned long)r->data);
	if (r->misc)  free_page((unsigned long)r->misc);
}

static int svm_run_alloc(struct svm_run *r)
{
	int i;
	memset(r, 0, sizeof(*r));
	r->hsave     = (void *)get_zeroed_page(GFP_KERNEL);
	r->host_vmcb = (void *)get_zeroed_page(GFP_KERNEL);
	r->vmcb      = (void *)get_zeroed_page(GFP_KERNEL);
	r->code      = (void *)get_zeroed_page(GFP_KERNEL);
	r->stack     = (void *)get_zeroed_page(GFP_KERNEL);
	r->data      = (void *)get_zeroed_page(GFP_KERNEL);
	r->misc      = (void *)get_zeroed_page(GFP_KERNEL);
	for (i = 0; i < 4; i++)
		r->npt[i] = (void *)get_zeroed_page(GFP_KERNEL);
	if (!r->hsave || !r->host_vmcb || !r->vmcb || !r->code ||
	    !r->stack || !r->data || !r->misc ||
	    !r->npt[0] || !r->npt[1] || !r->npt[2] || !r->npt[3]) {
		svm_run_free(r);
		return -ENOMEM;
	}
	return 0;
}

/* Build an NPT that maps exactly guest-physical 0x0000..0x3fff to our four
 * data pages; every other GPA is unmapped (=> NPF on access). */
static void svm_build_npt(struct svm_run *r)
{
	u64 *pml4 = r->npt[0], *pdpt = r->npt[1], *pd = r->npt[2], *pt = r->npt[3];

	pml4[0] = virt_to_phys(pdpt) | NPT_PTE_BITS;
	pdpt[0] = virt_to_phys(pd)   | NPT_PTE_BITS;
	pd[0]   = virt_to_phys(pt)   | NPT_PTE_BITS;
	pt[0]   = virt_to_phys(r->code)  | NPT_PTE_BITS; /* GPA 0x0000 */
	pt[1]   = virt_to_phys(r->stack) | NPT_PTE_BITS; /* GPA 0x1000 */
	pt[2]   = virt_to_phys(r->data)  | NPT_PTE_BITS; /* GPA 0x2000 */
	pt[3]   = virt_to_phys(r->misc)  | NPT_PTE_BITS; /* GPA 0x3000 */
}

static void svm_build_vmcb(struct svm_run *r, const u8 *payload, size_t len)
{
	struct vmcb *v = r->vmcb;
	struct vmcb_control_area *c = &v->control;
	struct vmcb_save_area *s = &v->save;

	/* INTERCEPT_VMRUN is mandatory: AMD requires it set or VMRUN itself
	 * generates #VMEXIT(INVALID). Then VMMCALL (our exit) plus
	 * SHUTDOWN/HLT for safety. */
	c->intercepts[INTERCEPT_VMRUN / 32]    |= 1u << (INTERCEPT_VMRUN % 32);
	c->intercepts[INTERCEPT_VMMCALL / 32]  |= 1u << (INTERCEPT_VMMCALL % 32);
	c->intercepts[INTERCEPT_SHUTDOWN / 32] |= 1u << (INTERCEPT_SHUTDOWN % 32);
	c->intercepts[INTERCEPT_HLT / 32]      |= 1u << (INTERCEPT_HLT % 32);

	c->asid       = 1;                 /* must be non-zero            */
	c->tlb_ctl    = 1;                 /* flush this guest's TLB      */
	c->nested_ctl = SVM_NESTED_CTL_NP_ENABLE;
	c->nested_cr3 = virt_to_phys(r->npt[0]);
	c->clean      = 0;                 /* everything dirty            */

	/* Guest: 16-bit real mode at CS:IP = 0000:0000. */
	memcpy(r->code, payload, len);

	s->efer   = EFER_SVME;             /* required to be set in guest */
	s->cr0    = X86_CR0_ET;            /* real mode: PE=0, PG=0       */
	s->cr3    = 0;
	s->cr4    = 0;
	s->rflags = X86_EFLAGS_FIXED;      /* bit 1                       */
	s->rip    = 0;
	s->rsp    = 0x2000;
	s->rax    = 0;
	s->g_pat  = 0x0007040600070406ULL; /* power-on default PAT       */
	s->cpl    = 0;

	/* Real-mode segment descriptor caches (base 0, 64K limit). */
	seg_set(&s->cs, 0, 0x009b, 0xffff, 0); /* code, present, accessed */
	seg_set(&s->ds, 0, 0x0093, 0xffff, 0); /* data, present, writable */
	seg_set(&s->es, 0, 0x0093, 0xffff, 0);
	seg_set(&s->ss, 0, 0x0093, 0xffff, 0);
	seg_set(&s->fs, 0, 0x0093, 0xffff, 0);
	seg_set(&s->gs, 0, 0x0093, 0xffff, 0);
	seg_set(&s->gdtr, 0, 0, 0xffff, 0);
	seg_set(&s->idtr, 0, 0, 0xffff, 0);
	seg_set(&s->ldtr, 0, 0x0082, 0xffff, 0); /* LDT present           */
	seg_set(&s->tr,   0, 0x008b, 0xffff, 0); /* busy TSS present      */
}

/*
 * Enter the guest described by r->vmcb exactly once and return after the
 * #VMEXIT. Enables SVM and sets VM_HSAVE_PA on the current CPU for the
 * duration (both are per-CPU), and preserves host FS/GS/TR/LDTR + syscall
 * MSRs across VMRUN via VMSAVE/VMLOAD. Runs with preemption and IRQs off.
 * Returns 0, or -EIO if SVM could not be enabled.
 */
static int svm_vmrun_once(struct svm_run *r)
{
	u64 hsave_pa     = virt_to_phys(r->hsave);
	u64 host_vmcb_pa = virt_to_phys(r->host_vmcb);
	u64 vmcb_pa      = virt_to_phys(r->vmcb);
	u64 efer_saved, vm_hsave_saved;
	unsigned long flags;
	int ret = 0;

	preempt_disable();
	vmctx_load_task_fpu();
	local_irq_save(flags);

	rdmsrq_safe(MSR_EFER, &efer_saved);
	rdmsrq_safe(MSR_VM_HSAVE_PA, &vm_hsave_saved);
	if (wrmsrq_safe(MSR_EFER, efer_saved | EFER_SVME)) {
		ret = -EIO;
		goto out;
	}
	wrmsrq_safe(MSR_VM_HSAVE_PA, hsave_pa);

	asm volatile(
		"mov %[hv], %%rax\n\t"
		"vmsave %%rax\n\t"     /* host FS/GS/TR/LDTR/MSRs -> host_vmcb */
		"clgi\n\t"
		"mov %[gv], %%rax\n\t"
		"vmload %%rax\n\t"     /* guest FS/GS/etc <- guest vmcb        */
		"vmrun %%rax\n\t"      /* enter guest until #VMEXIT            */
		"vmsave %%rax\n\t"     /* guest FS/GS/etc -> guest vmcb        */
		"mov %[hv], %%rax\n\t"
		"vmload %%rax\n\t"     /* host FS/GS/etc <- host_vmcb          */
		"stgi\n\t"
		:
		: [gv] "r" (vmcb_pa), [hv] "r" (host_vmcb_pa)
		: "rax", "memory", "cc");

	wrmsrq_safe(MSR_VM_HSAVE_PA, vm_hsave_saved);
	wrmsrq_safe(MSR_EFER, efer_saved);
out:
	local_irq_restore(flags);
	preempt_enable();
	return ret;
}

/*
 * Like svm_vmrun_once() but also saves/restores the guest general-purpose
 * registers (RBX,RCX,RDX,RSI,RDI,RBP,R8-R15) around VMRUN via r->gctx, so a
 * guest that runs arbitrary code (USERCODE mode) has real register state.
 * Modelled on KVM's __svm_vcpu_run register handling. RBP is preserved by
 * hand (it may be the frame pointer); RBX/R12-R15 via the clobber list.
 */

/*
 * Count of CPUs currently inside a guest (see vmctx_kick_guests / the core's
 * vmctx_backend_invalidate). Maintained around the guest entry below; read by a
 * take on another CPU to decide whether a guest-ASID TLB shootdown is needed.
 */
static atomic_t vmctx_guests_running = ATOMIC_INIT(0);

/*
 * The hand-over generation: which takes/protects a CPU's guest TLB has heard
 * about.
 *
 * TLB_CONTROL is a ONE-SHOT command, not a mode. KVM re-arms it before the
 * entries that need a flush and clears it to DO_NOTHING after every run;
 * this module set it once at VMCB build and never wrote it again, which
 * guarantees nothing about any entry after the first. Every context shares
 * ASID 1, and an ASID-tagged translation lives in the physical TLB across
 * VMEXIT, across the host running, and across the next VMRUN. So the kick
 * (below) forced a guest OUT, and the guest came straight BACK IN on the
 * same stale entries -- and a guest that was parked in its monitor during
 * the take (pg3's exact shape: the page is pulled while its thread sits in
 * a forwarded syscall) was never kicked at all, re-entered with a writable
 * translation for a page this side had already unmapped, punched and handed
 * over, and silently read and wrote a dead folio. That is the one-behind.
 *
 * The repair pairs two ordered halves:
 *
 *   take side (vmctx_kick_guests): bump vmctx_tlb_gen, THEN read
 *   vmctx_guests_running and IPI whoever is inside a guest, waited.
 *
 *   entry side (svm_vmrun_once_gpr): atomic_inc(vmctx_guests_running),
 *   THEN read vmctx_tlb_gen; if this CPU has not flushed for this
 *   generation, arm tlb_ctl for THIS entry and record it.
 *
 * Both sides write with a full barrier before they read (atomic_inc and
 * atomic64_inc are full barriers on x86), so of any take and any entry, at
 * least one sees the other: either the take sees the guest running and the
 * waited IPI throws it out (its stores through the old translation land on
 * the still-held folio BEFORE smp_call_function returns, i.e. before the
 * take's copy -- captured, not lost), or the entry sees the new generation
 * and flushes before the guest runs an instruction. After the kick returns,
 * no CPU can ever use a pre-take translation again: entering flushes, and a
 * fresh walk finds the zapped PTE and faults to the owner. The copy and the
 * punch that follow are therefore reading and retiring a page no guest can
 * still reach -- which is what the take always claimed and never had.
 *
 * The generation flush is ADDITIVE to the standing build-time tlb_ctl=1,
 * never a replacement for it. The standing value has been flushing VMRUNs
 * all along and is what covers the invalidations this file is never told
 * about (exec relayout, apply_map's sibling zaps, POKE's copy-on-write
 * break, reclaim); the one attempt to clear it after each run and flush
 * only by generation killed 24 of 24 pg3 runs in half a second. See the
 * comment at the atomic_dec below the VMRUN.
 */
static atomic64_t vmctx_tlb_gen = ATOMIC64_INIT(1);
static DEFINE_PER_CPU(u64, vmctx_cpu_tlb_gen);
static unsigned long vmctx_gen_bumps;	/* takes/protects that retired the TLB */
static unsigned long vmctx_gen_flushes;	/* entries that flushed because of one */
module_param_named(gen_bumps, vmctx_gen_bumps, ulong, 0444);
module_param_named(gen_flushes, vmctx_gen_flushes, ulong, 0444);

/*
 * There was a flush_reassert knob here: force tlb_ctl=1/clean=0 on EVERY
 * VMRUN. Its own A/B was later found void -- the build-time tlb_ctl=1 that
 * nothing clears was already flushing essentially every entry, so the two
 * arms were the same code and the knob's "~3x fewer take-stale" reading was
 * noise or a second-order effect. The generation check below is the designed
 * replacement: it re-arms the flush exactly when a hand-over retired the TLB,
 * and the standing build-time value covers everything nothing announces.
 */

static int svm_vmrun_once_gpr(struct svm_run *r)
{
	u64 efer_saved, vm_hsave_saved;
	unsigned long flags, ctx;
	int ret = 0;

	r->gctx.vmcb_pa      = virt_to_phys(r->vmcb);
	r->gctx.host_vmcb_pa = virt_to_phys(r->host_vmcb);
	ctx = (unsigned long)&r->gctx;

	preempt_disable();
	vmctx_load_task_fpu();
	local_irq_save(flags);

	rdmsrq_safe(MSR_EFER, &efer_saved);
	rdmsrq_safe(MSR_VM_HSAVE_PA, &vm_hsave_saved);
	if (wrmsrq_safe(MSR_EFER, efer_saved | EFER_SVME)) {
		ret = -EIO;
		goto out;
	}
	wrmsrq_safe(MSR_VM_HSAVE_PA, virt_to_phys(r->hsave));

	/*
	 * What the guest is about to be entered with, captured from the VMCB
	 * itself rather than from anything that mirrors it.
	 *
	 * The exit already says where the SYSCALL was; this says where the
	 * processor was told to start. A #UD is a fault, so its exit RIP is the
	 * faulting instruction -- if that address equals the entry, the guest
	 * really did begin there, and the advance written after the previous
	 * syscall did not survive to this entry. If instead the entry is two
	 * bytes further on and the exit is not, the disagreement is in the
	 * reporting rather than in the state.
	 */
	r->entry_rip = ((struct vmcb *)r->vmcb)->save.rip;
	/*
	 * From here until the matching decrement this CPU holds guest TLB
	 * entries. atomic_inc is a full barrier, so a take on another CPU that
	 * has already unmapped the page and then reads this counter as non-zero
	 * knows a guest may still reach the page and IPIs to force it out; a take
	 * whose unmap lands before this increment is safe without a kick, because
	 * the generation check below arms the flush for this entry and the guest
	 * then faults on the gone PTE. (This comment used to say tlb_ctl=1 from
	 * the VMCB build did that flush on every VMRUN. It does not: TLB_CONTROL
	 * is a one-shot command, and nothing re-armed it -- see vmctx_tlb_gen.)
	 */
	atomic_inc(&vmctx_guests_running);
	/*
	 * AFTER the increment above, never before: the inc is the full barrier
	 * that pairs with vmctx_kick_guests()'s bump-then-read. Reading the
	 * generation first would let a take slide its bump AND its read of
	 * vmctx_guests_running between our read and our inc -- the take would
	 * see no guest and skip the IPI, and this entry would run on pre-take
	 * translations it never heard about. See vmctx_tlb_gen.
	 */
	{
		u64 g = (u64)atomic64_read(&vmctx_tlb_gen);

		if (__this_cpu_read(vmctx_cpu_tlb_gen) != g) {
			struct vmcb *_v = r->vmcb;

			_v->control.tlb_ctl = 1;	/* FLUSH_ALL_ASID, as built */
			_v->control.clean = 0;	/* re-read controls, no cache */
			__this_cpu_write(vmctx_cpu_tlb_gen, g);
			vmctx_gen_flushes++;
		}
	}
	asm volatile(
		"push %%rbp \n\t"
		"push %%rax \n\t"            /* ctx ptr saved on stack        */
		"mov 8(%%rax), %%rax \n\t"   /* host_vmcb_pa                  */
		"vmsave %%rax \n\t"
		"clgi \n\t"
		"mov (%%rsp), %%rax \n\t"    /* ctx                           */
		"mov 16(%%rax), %%rbx \n\t"
		"mov 24(%%rax), %%rcx \n\t"
		"mov 32(%%rax), %%rdx \n\t"
		"mov 40(%%rax), %%rsi \n\t"
		"mov 48(%%rax), %%rdi \n\t"
		"mov 56(%%rax), %%rbp \n\t"
		"mov 64(%%rax), %%r8 \n\t"
		"mov 72(%%rax), %%r9 \n\t"
		"mov 80(%%rax), %%r10 \n\t"
		"mov 88(%%rax), %%r11 \n\t"
		"mov 96(%%rax), %%r12 \n\t"
		"mov 104(%%rax), %%r13 \n\t"
		"mov 112(%%rax), %%r14 \n\t"
		"mov 120(%%rax), %%r15 \n\t"
		"mov 0(%%rax), %%rax \n\t"   /* vmcb_pa                       */
		"vmload %%rax \n\t"
		"vmrun %%rax \n\t"
		"vmsave %%rax \n\t"
		"mov (%%rsp), %%rax \n\t"    /* ctx                           */
		"mov %%rbx, 16(%%rax) \n\t"
		"mov %%rcx, 24(%%rax) \n\t"
		"mov %%rdx, 32(%%rax) \n\t"
		"mov %%rsi, 40(%%rax) \n\t"
		"mov %%rdi, 48(%%rax) \n\t"
		"mov %%rbp, 56(%%rax) \n\t"
		"mov %%r8, 64(%%rax) \n\t"
		"mov %%r9, 72(%%rax) \n\t"
		"mov %%r10, 80(%%rax) \n\t"
		"mov %%r11, 88(%%rax) \n\t"
		"mov %%r12, 96(%%rax) \n\t"
		"mov %%r13, 104(%%rax) \n\t"
		"mov %%r14, 112(%%rax) \n\t"
		"mov %%r15, 120(%%rax) \n\t"
		"mov 8(%%rax), %%rax \n\t"   /* host_vmcb_pa                  */
		"vmload %%rax \n\t"
		"stgi \n\t"
		"pop %%rax \n\t"             /* discard ctx                   */
		"pop %%rbp \n\t"
		: "+a" (ctx)
		:
		: "rbx", "rcx", "rdx", "rsi", "rdi",
		  "r8", "r9", "r10", "r11", "r12", "r13", "r14", "r15",
		  "memory", "cc");
	/*
	 * tlb_ctl is deliberately NOT cleared here. KVM clears it after every
	 * run and re-arms it before entries that need a flush -- but KVM also
	 * announces every invalidation to its MMU. Here the build-time
	 * tlb_ctl=1 that no one ever cleared has been flushing on VMRUNs all
	 * along, and that standing flush is what covers every invalidation
	 * this file is never told about (exec relayout, apply_map's sibling
	 * zaps, a POKE's copy-on-write break, reclaim). Clearing it was tried:
	 * 24 of 24 pg3 runs died inside half a second, guests executing their
	 * own stale stack bytes -- with MORE generation flushes firing than
	 * hand-overs, so the removed standing flush, not the added gen flush,
	 * was the difference. The generation check above stays as the
	 * guarantee for entries where the standing value has been consumed or
	 * cached; nothing may reduce the baseline.
	 */
	atomic_dec(&vmctx_guests_running);

	wrmsrq_safe(MSR_VM_HSAVE_PA, vm_hsave_saved);
	wrmsrq_safe(MSR_EFER, efer_saved);
out:
	local_irq_restore(flags);
	preempt_enable();
	return ret;
}

static int svm_selftest(struct vmctx_selftest *out)
{
	struct svm_run r;
	struct vmcb *v;
	u64 vmcb_pa;
	int ret;

	if (g_caps.vendor != VMCTX_VENDOR_AMD)
		return -ENOTSUPP;
	if (!(g_caps.caps & (VMCTX_CAP_PRESENT | VMCTX_CAP_ENABLED)))
		return -EPERM;

	ret = svm_run_alloc(&r);
	if (ret)
		return ret;

	svm_build_npt(&r);
	svm_build_vmcb(&r, guest_payload, sizeof(guest_payload));
	v = r.vmcb;
	vmcb_pa = virt_to_phys(r.vmcb);

	ret = svm_vmrun_once(&r);

	if (ret == 0) {
		out->exit_code   = v->control.exit_code;
		out->exit_info_1 = v->control.exit_info_1;
		out->exit_info_2 = v->control.exit_info_2;
		out->guest_rax   = v->save.rax;
		out->guest_rip   = v->save.rip;
		out->mem_marker  = *(u16 *)((u8 *)r.code + 0x100);
		out->vmcb_pa     = vmcb_pa;
		out->npt_pml4_pa = virt_to_phys(r.npt[0]);
		out->vendor      = g_caps.vendor;

		/* success == guest ran the payload and exited via VMMCALL */
		if (v->control.exit_code == SVM_EXIT_VMMCALL &&
		    (out->guest_rax & 0xffff) == 0x1234 &&
		    out->mem_marker == 0x1234)
			out->result = 0;
		else
			out->result = -1;

		pr_info(VMCTX_PR "selftest: exit_code=0x%x rax=0x%llx mem=0x%llx rip=0x%llx result=%d\n",
			out->exit_code, out->guest_rax, out->mem_marker,
			out->guest_rip, out->result);
	} else {
		pr_warn(VMCTX_PR "selftest: setup failed: %d\n", ret);
	}

	svm_run_free(&r);
	return ret;
}

/* ---- USERCODE mode: run the caller's user code as a long-mode guest --- */
/*
 * Dune-style: the guest runs in the calling process's own address space
 * (guest CR3 = the process page tables, NPT identity-maps physical RAM) at
 * CPL 3 with SYSCALL disabled, so every guest syscall faults #UD and is
 * forwarded to the host in the process's context. This first cut dispatches
 * on the syscall number (which VMRUN saves in the VMCB) and forwards write()
 * as a canned message + handles exit(); marshalling the remaining argument
 * registers (guest GPR save/restore) is the next step toward running
 * arbitrary programs.
 */
#include <linux/file.h>

#define GUEST_NR_WRITE       1
#define GUEST_NR_EXIT       60
#define GUEST_NR_EXIT_GROUP 231

#define SVM_EXIT_EXCP_UD (SVM_EXIT_EXCP_BASE + 6)   /* 0x46 */
#define SVM_EXIT_EXCP_PF (SVM_EXIT_EXCP_BASE + 14)  /* 0x4e */

/* x86 page-fault error code bits (EXITINFO1 for an intercepted #PF). */
#define VMCTX_PF_PRESENT (1ULL << 0)
#define VMCTX_PF_WRITE   (1ULL << 1)
#define VMCTX_PF_USER    (1ULL << 2)
#define VMCTX_PF_FETCH   (1ULL << 4)

static inline u64 rd_efer(void)
{
	u64 v = 0;
	rdmsrq_safe(MSR_EFER, &v);
	return v;
}

/* NPT that identity-maps the low 512 GiB of physical RAM with 1 GiB pages,
 * so guest-physical == host-physical for the process's real pages. */
static void svm_build_npt_identity(struct svm_run *r)
{
	u64 *pml4 = r->npt[0], *pdpt = r->npt[1];
	int i;

	memset(pml4, 0, PAGE_SIZE);
	pml4[0] = virt_to_phys(pdpt) | NPT_PTE_BITS;
	for (i = 0; i < 512; i++)
		pdpt[i] = ((u64)i << 30) | NPT_PTE_BITS | (1ULL << 7); /* PS=1GiB */
}

/* Physical address of a task's page tables, for the guest's CR3. */
static u64 guest_cr3_of(struct task_struct *t)
{
	if (t->mm)
		return virt_to_phys(t->mm->pgd);
	return rd_cr3();
}

/*
 * Refresh the guest's FS/GS bases from the task's current user bases. Real
 * programs set up TLS with arch_prctl(ARCH_SET_FS) (or set_thread_area); that
 * syscall is dispatched generically and updates the host task, so the guest
 * must pick the new base up or its TLS accesses fault. Done after every
 * syscall, with no knowledge of which syscall it was.
 */
static void svm_sync_tls_bases(struct vmctx_task *vc, struct svm_run *r)
{
	struct vmcb *v = r->vmcb;
	u64 fsb = 0, gsb = 0;

	/*
	 * The monitor's word first, and the live MSR only when no monitor has
	 * spoken.
	 *
	 * Reading the LIVE MSR here was a hole with a measured body count: it
	 * samples whatever base this CPU happens to carry, and a SETREGS from
	 * the monitor writes thread.fsbase WITHOUT reaching the MSR of a task
	 * already on a CPU -- x86_fsbase_write_task() cannot poke another
	 * CPU's register. A freshly spawned context that stayed on-CPU from
	 * its clone() through its RESUME therefore entered the guest with the
	 * MONITOR's own thread pointer (vmremote's TCB, 0x...580) while
	 * thread.fsbase read back correct -- invisible to every read-back the
	 * monitor could make. ws1 under a pool: new threads dying in
	 * __ctype_init on a null loaded through that base, ~2% of runs, every
	 * memory-coherence repair measuring rate-neutral because no memory was
	 * ever wrong.
	 *
	 * vc->guest_fsbase is the vmctx-owned copy SETREGS writes; no context
	 * switch and no CPU residency can clobber it. The live-MSR fallback
	 * remains for a context nothing has SETREGS'd a base into: the first
	 * context before the program's own arch_prctl, whose base is the
	 * loader's don't-care.
	 */
	if (vc && vc->fsgs_valid) {
		v->save.fs.base = vc->guest_fsbase;
		v->save.gs.base = vc->guest_gsbase;
		return;
	}
	rdmsrq_safe(MSR_FS_BASE, &fsb);
	rdmsrq_safe(MSR_KERNEL_GS_BASE, &gsb);
	v->save.fs.base = fsb;
	v->save.gs.base = gsb;
}

static void svm_build_vmcb_usercode(struct svm_run *r, u64 entry, u64 stack)
{
	struct vmcb *v = r->vmcb;
	struct vmcb_control_area *c = &v->control;
	struct vmcb_save_area *s = &v->save;
	unsigned int ud = INTERCEPT_EXCEPTION_OFFSET + 6;
	unsigned int pf = INTERCEPT_EXCEPTION_OFFSET + 14;
	unsigned int gp = INTERCEPT_EXCEPTION_OFFSET + 13;

	c->intercepts[INTERCEPT_VMRUN / 32]    |= 1u << (INTERCEPT_VMRUN % 32);
	c->intercepts[INTERCEPT_VMMCALL / 32]  |= 1u << (INTERCEPT_VMMCALL % 32);
	c->intercepts[INTERCEPT_SHUTDOWN / 32] |= 1u << (INTERCEPT_SHUTDOWN % 32);
	c->intercepts[INTERCEPT_INTR / 32]     |= 1u << (INTERCEPT_INTR % 32);
	/*
	 * The host's NMIs are the host's. Without this the guest swallows them:
	 * the watchdog and the perf counters stop getting their attention, and
	 * the NMI is delivered through a guest IDT whose base is zero. VMX asks
	 * for the same thing with pin-based NMI exiting.
	 */
	c->intercepts[INTERCEPT_NMI / 32]      |= 1u << (INTERCEPT_NMI % 32);
	c->intercepts[ud / 32]                 |= 1u << (ud % 32);   /* #UD  */
	c->intercepts[pf / 32]                 |= 1u << (pf % 32);   /* #PF  */
	/*
	 * #GP as well, not because it can be serviced but so that it says what
	 * it is. Uncaught, the guest tries to deliver it through an IDT it does
	 * not have — base zero — and faults reading the descriptor, which
	 * arrives here as an unrecoverable page fault at 0xd0. That is 16*13,
	 * the address of vector 13's gate, and it names the real exception only
	 * to someone who thinks to divide it by sixteen.
	 */
	c->intercepts[gp / 32]                 |= 1u << (gp % 32);   /* #GP  */
	/*
	 * And every other exception the program can raise, for the same reason
	 * rather than one at a time.
	 *
	 * The guest runs with no IDT of its own -- base zero -- so an exception
	 * this backend does not intercept is not handled by the guest either:
	 * it faults reading a gate that is not there, at sixteen times the
	 * vector, and what arrives is an unrecoverable page fault at an address
	 * that names the real exception only to someone who divides by sixteen.
	 * #GP did that at 0xd0 and int3 did it at 0x30, and each was found the
	 * same slow way. There is no third one to find: whatever the program
	 * raises, it is intercepted and reported.
	 *
	 * All but three, and each for its own reason. Vector 2 is the NMI, which
	 * belongs to the host: it is taken by INTERCEPT_NMI just below, not by
	 * the exception bitmap, because an NMI is not the program's exception to
	 * raise or to handle. 7 (#NM) is the FPU's business and this backend
	 * shares the host's FPU state; 18 (#MC) is a machine check, which is the
	 * hardware talking to the host and not the program's to handle.
	 *
	 * The comment here used to say vector 2 was "already intercepted as
	 * INTERCEPT_INTR's neighbour". Being the next value of an enum does not
	 * set a bit: INTERCEPT_NMI was never set at all, so a host NMI arriving
	 * while the guest ran was delivered *into* the guest, which has no IDT --
	 * an unrecoverable #PF at 0x10, sixteen times the vector, exactly the
	 * failure the paragraph above says there is no third instance of. There
	 * was; this is it.
	 */
	{
		unsigned int v;

		for (v = 0; v < 32; v++) {
			unsigned int b;

			if (v == 2 || v == 7 || v == 18)
				continue;
			b = INTERCEPT_EXCEPTION_OFFSET + v;
			c->intercepts[b / 32] |= 1u << (b % 32);
		}
	}

	c->asid       = 1;
	c->tlb_ctl    = 1;
	c->nested_ctl = SVM_NESTED_CTL_NP_ENABLE;
	c->nested_cr3 = virt_to_phys(r->npt[0]);
	c->clean      = 0;

	/* Long mode, sharing the process's address space. */
	s->cr0    = rd_cr0();
	s->cr3    = guest_cr3_of(current);    /* the process's page tables    */
	s->cr4    = rd_cr4();
	s->efer   = (rd_efer() & ~EFER_SCE) | EFER_SVME; /* SYSCALL -> #UD    */
	s->g_pat  = 0x0007040600070406ULL;
	s->rflags = X86_EFLAGS_FIXED;
	s->rip    = entry;
	s->rsp    = stack;
	s->rax    = 0;
	s->cpl    = 3;

	/* 64-bit user segments (L=1 code, DPL=3). */
	seg_set(&s->cs, 0x33, 0x0afa, 0xffffffff, 0);
	seg_set(&s->ss, 0x2b, 0x0cf3, 0xffffffff, 0);
	seg_set(&s->ds, 0x2b, 0x0cf3, 0xffffffff, 0);
	seg_set(&s->es, 0x2b, 0x0cf3, 0xffffffff, 0);
	{
		/* Give the guest the task's real user TLS bases, so ordinary
		 * compiled code (stack protector, errno, TLS) works in-guest.
		 * While in the kernel the user GS base lives in KERNEL_GS_BASE. */
		u64 fsb = 0, gsb = 0;

		rdmsrq_safe(MSR_FS_BASE, &fsb);
		rdmsrq_safe(MSR_KERNEL_GS_BASE, &gsb);
		seg_set(&s->fs, 0, 0x0cf3, 0xffffffff, fsb);
		seg_set(&s->gs, 0, 0x0cf3, 0xffffffff, gsb);
	}
	seg_set(&s->gdtr, 0, 0, 0xffff, 0);
	seg_set(&s->idtr, 0, 0, 0xffff, 0);
	seg_set(&s->ldtr, 0, 0x0082, 0xffff, 0);
	seg_set(&s->tr,   0, 0x008b, 0xffff, 0);
}

/*
 * Service a guest page fault generically: the guest runs on the process's own
 * page tables, so a not-present guest page is just an unpopulated page of this
 * mm. fixup_user_fault() runs the host's normal fault path (demand paging,
 * COW, file-backed readahead, …), after which we re-enter the guest and the
 * instruction is retried. No knowledge of what the guest was doing is needed.
 */
static void vmctx_note_sysexit(struct vmctx_sysmark *ring, unsigned *idx,
			       u64 rip, u64 rsp, u64 nr, u64 arg1, u64 entry,
			       const struct pt_regs *regs)
{
	struct vmctx_sysmark *m = &ring[*idx % 16];

	m->entry = entry;

	/*
	 * RIP is recorded but carries nothing on a static binary: glibc has one
	 * SYSCALL instruction that every call goes through, so every exit is
	 * from the same address. The call number is what separates a repeated
	 * write from a futex loop retrying, which is the difference between a
	 * defect and ordinary behaviour.
	 */
	m->rip  = rip;
	m->rsp  = rsp;
	m->nr   = nr;
	m->arg1 = arg1;
	m->fold = regs->bx ^ regs->bp ^ regs->r12 ^ regs->r13 ^ regs->r14 ^
		  regs->r15;
	(*idx)++;
}

static void vmctx_dump_sysexits(struct task_struct *t,
				const struct vmctx_sysmark *ring, unsigned idx)
{
	unsigned n = idx < 16 ? idx : 16;
	unsigned i;

	for (i = 0; i < n; i++) {
		const struct vmctx_sysmark *m = &ring[(idx - n + i) % 16];
		const struct vmctx_sysmark *p = i ? &ring[(idx - n + i - 1) % 16]
						  : NULL;

		pr_info(VMCTX_PR "  pid %d %s -%u: nr %llu entry 0x%llx exit 0x%llx rsp 0x%llx fold 0x%llx%s\n",
			task_pid_nr(t), m->nr == ~0ULL ? "fault  " : "syscall",
			n - i, m->nr, m->entry, m->rip,
			m->rsp, m->fold,
			(p && p->nr == m->nr && p->rsp == m->rsp &&
			 p->arg1 == m->arg1 &&
			 p->fold == m->fold) ? "   <-- SAME AS PREVIOUS" : "");
	}
}

static unsigned long vmctx_trace_zero;
module_param_named(trace_zero, vmctx_trace_zero, ulong, 0644);
static int vmctx_watch_sample_on;
module_param_named(watch_sample, vmctx_watch_sample_on, int, 0644);
MODULE_PARM_DESC(watch_sample, "also run the per-VMEXIT sampler (perturbs; off by default)");
static unsigned long vmctx_trace_zero_hits;
module_param_named(trace_zero_hits, vmctx_trace_zero_hits, ulong, 0644);
MODULE_PARM_DESC(trace_zero_hits, "served faults on trace_zero whose slots read zero");
MODULE_PARM_DESC(trace_zero, "guest page address to report when it reads zero after a served fault");

/*
 * How often the host changed the guest's RBP between an exit and the next
 * entry, and how often it did so while leaving RSP where the guest left it.
 *
 * The second number is the one Part X §4 is about: a frame pointer moved
 * without the stack pointer moving with it is not a register set being
 * restored, it is one register being lost. Readable at any time, from any run,
 * so its zero can be believed -- a dmesg line printed once per boot cannot be.
 */
static unsigned long vmctx_bp_hostwrite;
module_param_named(bp_hostwrite, vmctx_bp_hostwrite, ulong, 0444);
MODULE_PARM_DESC(bp_hostwrite, "host writes to the guest's RBP between an exit and the next entry");
static unsigned long vmctx_bp_lost;
module_param_named(bp_lost, vmctx_bp_lost, ulong, 0444);
MODULE_PARM_DESC(bp_lost, "of those, the ones that left RSP untouched: a lost register restore");
static unsigned long vmctx_bp_report = 32;
module_param_named(bp_report, vmctx_bp_report, ulong, 0644);
MODULE_PARM_DESC(bp_report, "print a line for the first N lost-restore events (0 = ring only)");
static unsigned long vmctx_bp_selftest;
module_param_named(bp_selftest, vmctx_bp_selftest, ulong, 0644);
MODULE_PARM_DESC(bp_selftest, "corrupt the instrument's own remembered rbp N times, so its zero can be believed");

/*
 * Part XI §5.2's instrument: the page whose exits carry the word at [rbp].
 * A page address (0 = off); see bpwordhist in struct svm_run for what it
 * decides and why it is gated.
 */
static unsigned long vmctx_bpwatch;
module_param_named(bpwatch, vmctx_bpwatch, ulong, 0644);
MODULE_PARM_DESC(bpwatch, "guest page: while rsp is in it, record the word at [rbp] at every exit");

/*
 * Part XII §7.1's instrument: what the local kernel actually installs when the
 * monitor declines a guest fault.
 *
 * Every `pg3` death has a fault declined to the local kernel and no passing run
 * does -- 4 of 4 against 0 of 44, the sharpest discriminator this workload has
 * produced. What that decline *does* was never measured: for an anonymous
 * address it is `do_anonymous_page()` and a page of zeros installed for good
 * (part X §2b, part III E2), and for a page written into the backing object
 * first (`n_served_via_object`) it is the real bytes arriving by a different
 * road. A log line says which code ran; only the contents say which of those
 * two happened, so the contents are what is recorded.
 *
 * `page_sum` is byte-for-byte vmremote's and vmhome's, so the numbers compare
 * across the three logs -- and an all-zero page is 0x04000001, which is the
 * fingerprint that has hidden a perfectly transferred page of nothing before.
 */
static unsigned long vmctx_decl_serviced;
module_param_named(decl_serviced, vmctx_decl_serviced, ulong, 0444);
MODULE_PARM_DESC(decl_serviced, "declined guest faults this kernel serviced itself");
static unsigned long vmctx_decl_zero;
module_param_named(decl_zero, vmctx_decl_zero, ulong, 0444);
MODULE_PARM_DESC(decl_zero, "...where the guest's own mapping then reads all-zero: memory nobody sent");
static unsigned long vmctx_decl_bytes;
module_param_named(decl_bytes, vmctx_decl_bytes, ulong, 0444);
MODULE_PARM_DESC(decl_bytes, "...where it reads real bytes: the page came from somewhere");
static unsigned long vmctx_decl_fresh;
module_param_named(decl_fresh, vmctx_decl_fresh, ulong, 0444);
MODULE_PARM_DESC(decl_fresh, "...of the all-zero ones, those the page was absent before: installed here");
static unsigned long vmctx_decl_unread;
module_param_named(decl_unread, vmctx_decl_unread, ulong, 0444);
MODULE_PARM_DESC(decl_unread, "...where the page could not be read back at all");
static unsigned long vmctx_decl_failed;
module_param_named(decl_failed, vmctx_decl_failed, ulong, 0444);
MODULE_PARM_DESC(decl_failed, "declined faults this kernel could not service either: the context ends");
static unsigned long vmctx_decl_probe = 1;
module_param_named(decl_probe, vmctx_decl_probe, ulong, 0644);
MODULE_PARM_DESC(decl_probe, "read the page back after a declined fault (0 = the control arm)");
static unsigned long vmctx_decl_report = 16;
module_param_named(decl_report, vmctx_decl_report, ulong, 0644);
MODULE_PARM_DESC(decl_report, "print a line for the first N declined faults");
static unsigned long vmctx_decl_selftest;
module_param_named(decl_selftest, vmctx_decl_selftest, ulong, 0644);
MODULE_PARM_DESC(decl_selftest, "run the same probe on the first N faults the monitor SERVED, so its silence can be believed");

/*
 * page_sum() of a page of zeros. Worth a name of its own: it is the value that
 * made "the checksums match" hide a perfectly transferred page of nothing
 * (PRINCIPLES §7), and it is what this instrument exists to notice.
 */
#define VMCTX_ZERO_PAGE_SUM 0x04000001u

/* vmremote's page_sum(), unchanged, so the three logs compare. */
static unsigned int vmctx_page_sum(const void *p)
{
	const u32 *w = p;
	unsigned int a = 1, b = 0;
	int i;

	for (i = 0; i < 1024; i++) {
		a += w[i];
		b += a;
	}
	return (b << 16) ^ a ^ (b >> 16);
}

/*
 * Read a guest page out of the guest's own mapping without instantiating it.
 *
 * copy_from_user_nofault() runs with the fault handler disabled, so an absent
 * page reports -EFAULT instead of becoming the zero page this instrument would
 * then report as the guest's memory -- the corollary PRINCIPLES §7 records
 * ("an instrument can manufacture what it reports"). It is also why the
 * "before" reading below is meaningful: absent reads as absent.
 */
static int vmctx_read_guest_page(u64 base, void *buf)
{
	return copy_from_user_nofault(buf, (void __user *)(unsigned long)base,
				      PAGE_SIZE);
}

/*
 * The probe, shared by the decline path and its positive control. Returns 0 if
 * the page was readable and fills *sum.
 */
static int vmctx_probe_page(u64 base, unsigned int *sum)
{
	void *buf;
	int rc;

	buf = kmalloc(PAGE_SIZE, GFP_KERNEL);
	if (!buf)
		return -ENOMEM;
	rc = vmctx_read_guest_page(base, buf);
	if (!rc)
		*sum = vmctx_page_sum(buf);
	kfree(buf);
	return rc;
}


static int vmctx_service_guest_fault(u64 addr, u64 error_code)
{
	struct mm_struct *mm = current->mm;
	unsigned int fault_flags = 0;
	bool unlocked = false;
	int ret;

	if (!mm)
		return -EFAULT;
	if (error_code & VMCTX_PF_WRITE)
		fault_flags |= FAULT_FLAG_WRITE;
	if (error_code & VMCTX_PF_FETCH)
		fault_flags |= FAULT_FLAG_INSTRUCTION;

	/*
	 * Always unlock. fixup_user_fault() never returns with the mmap_lock
	 * dropped — its own comment says so, and every path in it that sets
	 * *unlocked re-takes the lock first, "to pair with the unlock() in the
	 * callers". The flag means the lock was released at some point and any
	 * VMA pointer the caller held is stale; it does not mean the caller has
	 * been relieved of unlocking.
	 *
	 * Skipping the unlock therefore leaked a read lock every time a fault
	 * had to wait — which is to say whenever the page was not already
	 * resident. Nothing failed at that moment. The task simply carried a
	 * stray reader from then on, and the next guest syscall that wanted the
	 * address space for writing — munmap, mmap, brk — blocked against its
	 * own lock, forever, with no second party to deadlock with and nothing
	 * in any log. It took a browser that munmaps its receive buffer right
	 * after a fault that retried to make it reliable enough to catch, and
	 * sysrq-w showing exactly one blocked task to make it obvious.
	 */
	mmap_read_lock(mm);
	ret = fixup_user_fault(mm, (unsigned long)addr, fault_flags, &unlocked);
	mmap_read_unlock(mm);
	return ret;
}

/*
 * The declined fault, and the bytes it leaves behind.
 *
 * Both machine arms call this instead of vmctx_service_guest_fault() directly,
 * so there is exactly one place where "the monitor declined and this kernel
 * answered" happens and exactly one place it is recorded. The reading is taken
 * from the guest's own mapping on both sides of the service, because the
 * question is not which branch ran but what the instruction retries on.
 */
static int vmctx_declined_to_local(struct task_struct *t, int *in_local_service,
				   u64 addr, u64 err, u64 rip)
{
	u64 base = addr & PAGE_MASK;
	unsigned int pre_sum = 0, post_sum = 0;
	int pre_rc = 1, post_rc = 1;	/* 1 = not probed */
	const char *region = "not looked at";
	int ret;

	if (vmctx_decl_probe) {
		struct mm_struct *mm = t->mm;

		pre_rc = vmctx_probe_page(base, &pre_sum);
		/*
		 * A trylock, because this runs on the way to servicing a fault
		 * and must not be the thing that blocks. Its failure is said
		 * out loud rather than reported as "no region": "the lock was
		 * busy" and "there is no mapping here" are opposite answers and
		 * a zero would merge them.
		 */
		region = "could not be looked up (mmap_lock busy)";
		if (mm && mmap_read_trylock(mm)) {
			struct vm_area_struct *vma = find_vma(mm, base);

			if (!vma || vma->vm_start > base)
				region = "NO REGION HERE";
			else if (vma->vm_file)
				region = "a region backed by a file "
					 "(the context's object)";
			else if (vma->vm_flags & VM_SHARED)
				region = "an anonymous shared region";
			else
				region = "an anonymous private region "
					 "(do_anonymous_page's zeros)";
			mmap_read_unlock(mm);
		}
	}

	*in_local_service = 1;
	ret = vmctx_service_guest_fault(addr, err);
	*in_local_service = 0;

	/*
	 * Counted whether or not the probe ran, so the control arm
	 * (decl_probe=0) still says how many declines it had. A switch that
	 * turns off the counter as well as the instrument makes its own arm
	 * unreadable.
	 */
	vmctx_decl_serviced++;
	if (!vmctx_decl_probe)
		return ret;

	if (ret) {
		vmctx_decl_failed++;
	} else {
		post_rc = vmctx_probe_page(base, &post_sum);
		if (post_rc) {
			vmctx_decl_unread++;
		} else if (post_sum == VMCTX_ZERO_PAGE_SUM) {
			vmctx_decl_zero++;
			if (pre_rc)
				vmctx_decl_fresh++;
		} else {
			vmctx_decl_bytes++;
		}
	}

	if (vmctx_decl_serviced <= vmctx_decl_report)
		pr_warn(VMCTX_PR "pid %d: the monitor DECLINED the fault at 0x%llx (err 0x%llx, rip 0x%llx) and this kernel serviced it: rc=%d, the guest now retries on %s (sum 0x%08x, was %s sum 0x%08x); %s\n",
			task_pid_nr(t), (unsigned long long)base,
			(unsigned long long)err, (unsigned long long)rip, ret,
			post_rc ? "a page it cannot read" :
			  post_sum == VMCTX_ZERO_PAGE_SUM ?
			    "A PAGE OF ZEROS NOBODY SENT" : "real bytes",
			post_sum,
			pre_rc ? "absent," : "present,", pre_sum, region);
	return ret;
}

/* Publish guest registers into the task's pt_regs. */
static void guest_to_ptregs(struct svm_run *r, struct pt_regs *regs)
{
	struct vmcb *v = r->vmcb;

	regs->ip     = v->save.rip;
	regs->sp     = v->save.rsp;
	regs->ax     = v->save.rax;
	regs->flags  = v->save.rflags;
	regs->cs     = __USER_CS;
	regs->ss     = __USER_DS;
	/*
	 * Publish the syscall number so the kernel's restart machinery works on
	 * the guest: on -ERESTARTSYS et al, arch_do_signal_or_restart() puts the
	 * number back in AX and rewinds RIP by 2 — landing exactly on the guest's
	 * SYSCALL instruction, which we already stepped over. Without this the
	 * raw -ERESTARTSYS would leak to the guest as a bogus return value.
	 */
	regs->orig_ax = r->last_sysnr;
	regs->bx = r->gctx.rbx; regs->cx = r->gctx.rcx; regs->dx = r->gctx.rdx;
	regs->si = r->gctx.rsi; regs->di = r->gctx.rdi; regs->bp = r->gctx.rbp;
	regs->r8  = r->gctx.r8;  regs->r9  = r->gctx.r9;  regs->r10 = r->gctx.r10;
	regs->r11 = r->gctx.r11; regs->r12 = r->gctx.r12; regs->r13 = r->gctx.r13;
	regs->r14 = r->gctx.r14; regs->r15 = r->gctx.r15;
}

/*
 * Which call site published the frame back into the guest. Only for the
 * instrument below: a lost register restore has to be attributed to a writer,
 * and every one of these looks identical from inside the function.
 */
enum {
	WTAG_NONE = 0,
	WTAG_PREENTRY,		/* run_usercode(), before the next VMRUN     */
	WTAG_UD_REDIRECT,	/* an owner answered a #UD                   */
	WTAG_SYS_EXIT,		/* forwarded syscall, context ending         */
	WTAG_SYS_DONE,		/* forwarded syscall, normal return          */
	WTAG_PF_EXIT,		/* fault, owner declined and context ends    */
	WTAG_PF_SERVED,		/* fault, owner served it                    */
	WTAG_EXC_REDIRECT,	/* any other exception, owner answered       */
	WTAG_CLONE,		/* a child context built from its parent     */
};

/* Take back any changes the kernel made to the frame (signal delivery moves
 * RIP/RSP to a handler, syscall restart rewinds RIP, ptrace can poke regs). */
static void ptregs_to_guest_tag(struct svm_run *r, struct pt_regs *regs, u32 tag)
{
	struct vmcb *v = r->vmcb;

	r->last_wtag = tag;
	r->wtag_ptbp = regs->bp;

	v->save.rip    = regs->ip;
	v->save.rsp    = regs->sp;
	v->save.rax    = regs->ax;
	v->save.rflags = regs->flags | X86_EFLAGS_FIXED | X86_EFLAGS_IF;
	r->gctx.rbx = regs->bx; r->gctx.rcx = regs->cx; r->gctx.rdx = regs->dx;
	r->gctx.rsi = regs->si; r->gctx.rdi = regs->di; r->gctx.rbp = regs->bp;
	r->gctx.r8  = regs->r8;  r->gctx.r9  = regs->r9;  r->gctx.r10 = regs->r10;
	r->gctx.r11 = regs->r11; r->gctx.r12 = regs->r12; r->gctx.r13 = regs->r13;
	r->gctx.r14 = regs->r14; r->gctx.r15 = regs->r15;
}

/*
 * Physical backing for an address nothing here has mapped.
 *
 * The source owns the address space: it performed the mmap, and the address the
 * guest holds is the source's. This machine is not told about that mapping and
 * keeps no record of it -- it finds out by faulting. So a fault at an address
 * with no VMA here is not an error; it is the first touch of memory the source
 * has, and what is missing is somewhere to put the bytes. A monitor cannot
 * create that from outside an address space, but this runs as the faulting
 * task, so it can.
 *
 * From the context's shared object when it has one, with the faulting address
 * as the offset. Two contexts started with the same object see the same page at
 * the same address, so a write by one is seen by the other -- which is all that
 * sharing an address space means, and is how a guest thread gets its parent's
 * memory without this machine needing threads, CLONE_VM, or any notion of what
 * a thread is. A context given its own object shares with nobody, which is the
 * snapshot a fork wants. Nothing else about memory is communicated: no ranges,
 * no layout.
 *
 * Called *before* asking the monitor, and undone by vmctx_unback_fault() if the
 * monitor says the source has no such address. Getting that order wrong would
 * turn every wild pointer into valid zeroed memory and hide the segmentation
 * fault the program has earned.
 *
 * One function for both backends, and that is the point rather than tidiness.
 * It existed only in the SVM path. The Intel machine -- the one the objective is
 * actually about -- therefore had no backing for any fault at all: the monitor's
 * POKE of the faulting page had nowhere to land, and the guest died on its own
 * entry instruction with an unrecoverable #PF, on a machine whose log said only
 * that the fault could not be serviced.
 *
 * Returns 1 if a mapping was created and is the caller's to undo.
 */
static int vmctx_back_fault(struct task_struct *t, struct vmctx_task *vc, u64 addr)
{
	unsigned long pg = (unsigned long)addr & PAGE_MASK;
	struct file *bf;
	unsigned long got;
	int absent;

	mmap_read_lock(current->mm);
	absent = !find_vma_intersection(current->mm, pg, pg + PAGE_SIZE);
	mmap_read_unlock(current->mm);
	if (!absent)
		return 0;

	/*
	 * Strictly positive. Zero is not "no object" by accident of a zeroed
	 * struct -- it is standard input, and fget(0) succeeds, so a caller that
	 * simply memset its config was claiming stdin as the memory its guest
	 * runs on. Seven of the eight tools in this tree did exactly that for
	 * as long as the field existed. No fd worth backing a guest with is 0.
	 */
	bf = vc->backing_fd > 0 ? fget(vc->backing_fd) : NULL;
	got = vm_mmap(bf, pg, PAGE_SIZE, PROT_READ | PROT_WRITE | PROT_EXEC,
		      (bf ? MAP_SHARED : (MAP_PRIVATE | MAP_ANONYMOUS)) | MAP_FIXED,
		      bf ? pg : 0);
	if (bf)
		fput(bf);
	if (IS_ERR_VALUE(got)) {
		pr_warn(VMCTX_PR "cannot back 0x%llx for pid %d: %ld\n",
			addr, task_pid_nr(t), (long)got);
		return 0;
	}
	return 1;
}

/*
 * The monitor did not want the fault, so the source has no such address and the
 * page created above is a fiction. Take it away rather than let the guest run on
 * memory nobody owns.
 */
static void vmctx_unback_fault(u64 addr)
{
	vm_munmap((unsigned long)addr & PAGE_MASK, PAGE_SIZE);
}

static int vmctx_be_run_usercode_once(struct task_struct *t)
{
	struct vmctx_task *vc = t->vmctx;
	struct svm_run *r = vc->backend_priv;
	struct vmcb *v = r->vmcb;
	struct pt_regs *regs = task_pt_regs(t);
	u32 ec;
	int ret;

	/*
	 * Refresh the thread pointer before every entry, not only after a
	 * syscall. A restored or transplanted context has its FS base installed
	 * by the monitor (SETREGS) before it ever runs, and libc reaches
	 * everything thread-local through FS — so entering with the base this
	 * task happened to have faults on the guest's first TLS access.
	 */
	svm_sync_tls_bases(vc, r);

	/*
	 * Between the last exit and this entry the guest did not execute, so
	 * anything different in gctx.rbp was put there by this side. Record it
	 * with the writer's tag before the guest runs and overwrites the
	 * evidence. Comparing against the *exit* rather than against pt_regs is
	 * deliberate: it catches a write by any route, including one that never
	 * went through ptregs_to_guest_tag() at all.
	 */
	if (r->have_exited && r->gctx.rbp != r->bp_at_exit) {
		struct vmctx_bpw *w = &r->bpw[r->bpw_i % ARRAY_SIZE(r->bpw)];

		w->obp = r->bp_at_exit;
		w->nbp = r->gctx.rbp;
		w->osp = r->sp_at_exit;
		w->nsp = v->save.rsp;
		w->rip = r->ip_at_exit;
		w->ptbp = r->wtag_ptbp;
		w->tag = r->last_wtag;
		r->bpw_i++;
		vmctx_bp_hostwrite++;
		if (w->osp == w->nsp) {
			vmctx_bp_lost++;
			if (vmctx_bp_lost <= vmctx_bp_report)
				pr_warn(VMCTX_PR "pid %d: the guest's rbp changed from 0x%llx to 0x%llx between the exit at 0x%llx and the next entry, with rsp unchanged at 0x%llx; last frame publish was tag %u carrying pt_regs->bp 0x%llx\n",
					task_pid_nr(t), w->obp, w->nbp, w->rip,
					w->osp, w->tag, w->ptbp);
		}
	}

	ret = svm_vmrun_once_gpr(r);
	if (ret)
		return ret;

	/*
	 * The guest may move its own base while it runs (wrfsbase); the VMCB's
	 * save area is the truth at exit. Keep the vmctx-owned copy equal to
	 * it, so GETREGS answers with what the guest really has and the next
	 * entry restores exactly that. A context nothing has SETREGS'd stays
	 * on live-MSR semantics until a monitor speaks.
	 */
	if (vc->fsgs_valid) {
		vc->guest_fsbase = v->save.fs.base;
		vc->guest_gsbase = v->save.gs.base;
	}

	/*
	 * The guest just ran; sample its writer slots before anything on this
	 * side can move the page. No-op unless armed (vmctx_watch_gva) AND
	 * asked for separately.
	 *
	 * Separately, because this sampler is not free and not neutral: it does
	 * a get_user_pages_fast_only() and two copy_from_user_nofault()s on
	 * EVERY guest exit, and arming it changed hx2's verdict (one PASS and
	 * one death in four runs) and reported gaps of 2.8 billion -- its marks
	 * are keyed by address and not by address space, so every program that
	 * maps the same address feeds one 8-slot array. The take-seeded check
	 * (vmctx_watch_check at TAKE/TAKEOBJ) and the served check
	 * (vmctx_watch_served, the guest's own bytes at the instant a fault is
	 * answered) are sound and cost nothing when the page is not the armed
	 * one -- so vmctx_watch_gva now arms those two alone, and this is opt-in
	 * on top. An instrument that changes the outcome it measures has to be
	 * separable from the ones that do not.
	 */
#ifdef VMCTX_HAS_WATCH
	if (vmctx_watch_sample_on)
		vmctx_watch_sample();
#endif

	vc->enters++;
	vc->exits++;
	r->riphist[r->riphist_i % ARRAY_SIZE(r->riphist)] = v->save.rip;
	r->rsphist[r->riphist_i % ARRAY_SIZE(r->rsphist)] = v->save.rsp;
	r->rbphist[r->riphist_i % ARRAY_SIZE(r->rbphist)] = r->gctx.rbp;
	{
		u32 bi = r->riphist_i % ARRAY_SIZE(r->bpwordhist);
		u64 w = 0;

		r->bpwordhist[bi] = 0;
		r->bpwordok[bi] = 0;
		if (vmctx_bpwatch &&
		    (v->save.rsp & PAGE_MASK) == vmctx_bpwatch &&
		    !copy_from_user_nofault(&w, (void __user *)
					    (unsigned long)r->gctx.rbp,
					    sizeof(w))) {
			r->bpwordhist[bi] = w;
			r->bpwordok[bi] = 1;
		}
	}
	r->riphist_i++;
	r->bp_at_exit = r->gctx.rbp;
	r->sp_at_exit = v->save.rsp;
	r->ip_at_exit = v->save.rip;
	r->have_exited = 1;
	/*
	 * The positive control for the check above, and the reason its zero can
	 * be believed. It perturbs only the *remembered* value, never the
	 * guest's, so the run is unaffected -- and the check must then report a
	 * lost restore. A silent instrument and a working one are the same
	 * reading without this.
	 */
	if (vmctx_bp_selftest) {
		vmctx_bp_selftest--;
		r->bp_at_exit ^= 0x8;
	}
	ec = v->control.exit_code;
	vc->last_exit_reason = ec;

	/*
	 * A host interrupt or a host NMI. Both are held while GIF is clear and
	 * taken by the host the moment svm_vmrun_once_gpr()'s stgi runs, so
	 * there is nothing to do here but carry on -- and nothing to report to
	 * the owner, because neither is the program's.
	 */
	if (ec == SVM_EXIT_INTR || ec == SVM_EXIT_NMI) {
		r->last_sysnr = -1;
		vmctx_note_spin(t, &r->spin, v->save.rip);
		return VMCTX_RUN_CONTINUE;
	}

	if (ec == SVM_EXIT_EXCP_UD) {     /* guest SYSCALL -> #UD             */
		u64 nr = v->save.rax;
		u64 sysenter_rip = v->save.rip;	/* where the SYSCALL itself is */

		if (!vmctx_insn_is_syscall(t, v->save.rip)) {
			int rc;

			/*
			 * An instruction this machine will not run. Report it
			 * to whoever owns the program, exactly as a fault is
			 * reported: the vector, and nothing said about what
			 * it means.
			 *
			 * This arm used to end the context instead, and the
			 * comment here said delivering from it "does not work"
			 * -- the guest ended up in vmremote's own text and the
			 * monitor died. The reason was the frame: this arm
			 * never published the guest into pt_regs, so what the
			 * owner was shown, and what its answer was written over,
			 * was some earlier stop's registers. The page-fault arm
			 * did publish, which is the whole of why that one
			 * "worked" and this one did not.
			 */
			guest_to_ptregs(r, regs);
			rc = vmctx_redirect_fault(t, regs, 6, v->save.rip, 0);
			if (rc == 1) {
				ptregs_to_guest_tag(r, regs, WTAG_UD_REDIRECT);
				return VMCTX_RUN_CONTINUE;
			}
			if (rc < 0) {
				vc->exit_status = rc;
				return VMCTX_RUN_EXIT;
			}
			vmctx_report_ud(t, v->save.rip, v->save.rsp, nr);
			vc->exit_status = -EFAULT;
			return VMCTX_RUN_EXIT;
		}

		/*
		 * exit()/exit_group() are context termination, not syscalls to
		 * forward: they end the guest and hand the status back to
		 * vmctx_run(2). Everything else goes through the host's generic
		 * syscall table — no per-syscall code here.
		 */
		if (nr == GUEST_NR_EXIT || nr == GUEST_NR_EXIT_GROUP) {
			vc->exit_status = (int)r->gctx.rdi;   /* status in RDI */
			/*
			 * How many syscalls this side actually serviced, said at
			 * the one moment the number is final.
			 *
			 * The monitor counts the events it received, and when the
			 * two disagree the difference says which side invented the
			 * extra one -- the open question about th9's duplicated
			 * line. The userspace print meant to carry this never
			 * appears: the context does not get to run after vmctx_run
			 * returns, because the monitor has already torn down
			 * around it.
			 */
			/*
			 * WHICH of the two, and where from. A thread that
			 * returns from its start routine leaves through
			 * exit(60); a thread that runs the C library's exit()
			 * leaves through exit_group(231) having first taken
			 * every lock that function takes and released none of
			 * them. The two are indistinguishable in the monitor's
			 * "ended with exit status 0", and telling them apart is
			 * the whole of ws1's hang.
			 */
			pr_info(VMCTX_PR "pid %d: context ending via %s(%llu) status %d at rip 0x%llx, serviced %llu syscalls and %llu faults\n",
				task_pid_nr(t),
				nr == GUEST_NR_EXIT ? "exit" : "exit_group",
				(unsigned long long)nr, vc->exit_status,
				(unsigned long long)sysenter_rip,
				vc->counter, vc->faults);
			vmctx_dump_sysexits(t, r->sysmark, r->sysmark_i);
			return VMCTX_RUN_EXIT;
		}

		r->last_sysnr = (long)nr;
		vmctx_spin_reset(&r->spin);
		r->sysring[r->sysring_i] = (u16)nr;
		r->sysring_i = (r->sysring_i + 1) % ARRAY_SIZE(r->sysring);

		/*
		 * Publish the guest's registers into the task's own pt_regs and
		 * apply exactly what the SYSCALL instruction does: step over the
		 * 2-byte instruction and clobber RCX/R11. The kernel then reads
		 * its arguments from that frame — and syscalls that rewrite the
		 * frame (rt_sigreturn restores the whole pre-signal register set)
		 * take effect when we reload the guest from it below.
		 */
		guest_to_ptregs(r, regs);
		regs->ip     += 2;
		regs->cx      = regs->ip;
		regs->r11     = regs->flags;
		regs->orig_ax = nr;

		/*
		 * Objective 2: a monitor process may service this instead of the
		 * kernel. 0 = handle it here, 1 = the monitor already did (RAX
		 * set), <0 = terminate the context.
		 */
		/*
		 * Recorded from the VMCB, before the frame is touched: this is
		 * where the guest actually was when it issued the call.
		 */
		vmctx_note_sysexit(r->sysmark, &r->sysmark_i, v->save.rip,
				   v->save.rsp, nr, r->gctx.rsi, r->entry_rip,
				   regs);
		{
			u64 ip_before = regs->ip;

			ret = vmctx_redirect_syscall(t, regs);
			/*
			 * The monitor withdrew the event, so the core rewound
			 * the instruction and the guest will execute this
			 * syscall again. That is right only if nothing ran it;
			 * if something did, the call happens twice, and a
			 * write(2) that happens twice is a line of output that
			 * appears twice with no other trace anywhere in the
			 * run. Say when it happens, so the question "did the
			 * guest re-execute?" is answered by the log rather than
			 * inferred from a syscall count.
			 */
			if (regs->ip != ip_before)
				pr_warn_ratelimited(VMCTX_PR "pid %d: syscall %lu at 0x%llx was withdrawn; rip moved %+lld and the guest will run it again\n",
					task_pid_nr(t), (unsigned long)nr,
					ip_before, (long long)regs->ip - (long long)ip_before);
		}
		if (ret < 0) {
			vc->exit_status = ret;
			ptregs_to_guest_tag(r, regs, WTAG_SYS_EXIT);
			return VMCTX_RUN_EXIT;
		}
		if (ret == 0) {
			/*
			 * No monitor serviced the call. The destination must NEVER
			 * run a guest syscall through its own kernel -- every syscall
			 * belongs to the source. Fail closed, exactly as the fault
			 * path does on a lost monitor (-EIO), and end the context.
			 */
			vc->exit_status = -EIO;
			ptregs_to_guest_tag(r, regs, WTAG_SYS_EXIT);
			return VMCTX_RUN_EXIT;
		}
		svm_sync_tls_bases(vc, r);

		ptregs_to_guest_tag(r, regs, WTAG_SYS_DONE);
		/*
		 * The guest must now be standing *past* the SYSCALL it just
		 * made. Anything else means the advance was lost between here
		 * and the VMCB, and the next VMRUN re-executes the instruction
		 * -- which is exactly what th9's doubled write looks like from
		 * the outside: two exits at the same address, with the same
		 * registers, and no kernel rewind anywhere in between.
		 *
		 * Checked against the VMCB rather than pt_regs, because pt_regs
		 * is what was just copied out of and would agree with itself.
		 */
		if (v->save.rip != sysenter_rip + 2)
			pr_warn_ratelimited(VMCTX_PR "pid %d: syscall %lu left the guest at 0x%llx, not 0x%llx; it will run the instruction again\n",
				task_pid_nr(t), (unsigned long)nr, v->save.rip,
				sysenter_rip + 2);
		vc->counter++;                   /* syscalls serviced         */
		return VMCTX_RUN_CONTINUE;
	}

	if (ec == SVM_EXIT_EXCP_PF) {     /* guest page fault: demand paging */
		u64 err  = v->control.exit_info_1;
		u64 addr = v->control.exit_info_2;
		unsigned long pre0 = 0, pre1 = 0;
		int pre_ok = -1;
		int backed_here;

		r->last_sysnr = -1;
		vmctx_spin_reset(&r->spin);

		backed_here = vmctx_back_fault(t, vc, addr);

		/*
		 * Into the same ring as the syscalls, so the two interleave in
		 * one ordered record. A duplicated write is entered at the
		 * `call` rather than after its syscall, which is what being
		 * resumed at a *faulting* instruction looks like -- and whether
		 * a fault actually lands there between the two syscalls of an
		 * iteration is not answerable from a record that holds only
		 * syscalls. Marked with nr = ~0; the faulting address goes where
		 * the buffer pointer would.
		 */
		vmctx_note_sysexit(r->sysmark, &r->sysmark_i, v->save.rip,
				   v->save.rsp, ~0ULL, addr, r->entry_rip, regs);

		/*
		 * A stop is a stop: publish the guest's registers into the frame
		 * before asking anyone about it, exactly as the SYSCALL path
		 * does. The frame is how a whole register set is both shown and
		 * given back -- it is what GETREGS reads and what SETREGS
		 * writes -- so an owner that answers "the guest carries on
		 * somewhere else" has somewhere to say it, and one that answers
		 * "the memory is good now" changes nothing and the copy back is
		 * an identity.
		 *
		 * Without this the frame still held the guest as of the
		 * *previous* exit. That is why publishing it back was ruinous
		 * on its own: it rolled every fault's registers back one stop,
		 * ~1350 times a run.
		 */
		guest_to_ptregs(r, regs);

		/*
		 * What the page holds *before* the monitor is asked. With the
		 * "after" reading below this separates two very different
		 * failures: a monitor that answered "served" without writing
		 * anything (before == after == zero), and bytes that were
		 * installed and then lost (before non-zero, after zero).
		 */
		if (vmctx_trace_zero &&
		    (addr & PAGE_MASK) == vmctx_trace_zero) {
			unsigned long b[2] = { ~0UL, ~0UL };

			pre_ok = !copy_from_user(b, (void __user *)
						 (unsigned long)(addr & PAGE_MASK),
						 sizeof(b));
			pre0 = b[0];
			pre1 = b[1];
		}
		/* Objective 2: redirect the fault to a monitor if one wants it. */
		ret = vmctx_redirect_fault(t, regs, 14, addr, err);
		if (ret < 0) {
			vc->exit_status = ret;
			ptregs_to_guest_tag(r, regs, WTAG_PF_EXIT);
			return VMCTX_RUN_EXIT;
		}
		if (ret == 1) {
			vc->faults++;      /* monitor fixed it; retry the insn */
			/*
			 * The stale-read stamp check. The fault has just been
			 * served and the guest's page is present again; what it
			 * is about to READ is exactly what copy_from_user sees
			 * here. If a slot is below the mark seeded at take time
			 * (the live folio, the guest's true value), the serve
			 * handed back a copy older than what changed hands --
			 * the hx2 loss, at the instant it happens, which the
			 * VMEXIT sampler misses. Cheap-gated to the armed page.
			 */
#ifdef VMCTX_HAS_WATCH
			if (vmctx_watch_page() &&
			    (addr & PAGE_MASK) == (vmctx_watch_page() & PAGE_MASK)) {
				unsigned long sl[8];

				if (!copy_from_user(sl, (void __user *)
						    ((addr & PAGE_MASK) + 64),
						    sizeof(sl)))
					vmctx_watch_served(addr & PAGE_MASK, sl);
			}
#endif
			/*
			 * What did the guest actually get? Every userspace
			 * instrument agrees the bytes sent were right, yet
			 * pg3's guest reads zeros off a page it never faults
			 * on again. The mechanism left is this backing:
			 * vmctx_back_fault() maps the page from the object
			 * before the monitor is asked, and a hand-over punches
			 * a hole in that object, which reads as zeros. So read
			 * the bytes back out of the guest's own mapping here,
			 * after the monitor is done -- the last word on what
			 * the page holds before the instruction retries.
			 *
			 * Only for the page named in trace_zero, and only when
			 * it reads all-zero, so the log names the failure
			 * rather than drowning it.
			 */
			/*
			 * Read the SLOTS, not offset 0.
			 *
			 * This checked the first two words of the page and
			 * reported only when both were zero. For every page
			 * whose interesting words are not at offset 0 that is
			 * either always true or never true, and it is always
			 * true for the one page this was aimed at: tests/hx2
			 * puts its writer slots at +64 and leaves the head of
			 * the page zero for the whole run, so the check fired
			 * on every served fault and said nothing. An instrument
			 * that cannot distinguish the failure from the ordinary
			 * case is not evidence either way (PRINCIPLES §7).
			 *
			 * The slots are where a monotonic writer's value lives,
			 * so "served and the guest reads ZERO there" is the
			 * invented page, said at the instant the guest is about
			 * to retry on it, with the reply that produced it.
			 */
			if (vmctx_trace_zero &&
			    (addr & PAGE_MASK) == vmctx_trace_zero) {
				unsigned long v[8] = { ~0UL };
				int z = 0, k;
				unsigned long big = 0;

				/*
				 * Only a slot that has been written and now
				 * reads zero. A slot no writer owns is zero for
				 * the whole run, and counting those made this
				 * fire 161755 times in one run -- on hx2 four of
				 * the eight are never written at all. So a zero
				 * counts only alongside a sibling slot that is
				 * plainly past start-up, which is what makes
				 * "this one went back to zero" the report.
				 */
				if (!copy_from_user(v, (void __user *)
						    ((addr & PAGE_MASK) + 64),
						    sizeof(v))) {
					for (k = 0; k < 8; k++)
						if (v[k] > big && v[k] < (1UL << 32))
							big = v[k];
					if (big > 4096)
						for (k = 0; k < 8; k++)
							if (!v[k])
								z++;
				}
				if (z)
					vmctx_trace_zero_hits++;
				if (z && vmctx_trace_zero_hits <= 16)
					pr_warn("vmctx: pid %d: %d of 8 slots ZERO (before=%d/%016lx%016lx) after the monitor served 0x%llx (err 0x%llx) -- the page it retries on is not the page it was sent; reply map_op=%u addr=0x%llx len=0x%llx off=0x%llx prot=0x%x covers=%d\n",
						task_pid_nr(t), z, pre_ok, pre0, pre1,
						(unsigned long long)(addr & PAGE_MASK),
						(unsigned long long)err,
						vc->reply.map_op,
						(unsigned long long)vc->reply.map_addr,
						(unsigned long long)vc->reply.map_len,
						(unsigned long long)vc->reply.map_off,
						vc->reply.map_prot,
						(vc->reply.map_op != VMCTX_MAP_NONE &&
						 vc->reply.map_len &&
						 (addr & PAGE_MASK) >=
						 (vc->reply.map_addr & PAGE_MASK) &&
						 (addr & PAGE_MASK) <
						 (vc->reply.map_addr & PAGE_MASK) +
						 PAGE_ALIGN(vc->reply.map_len +
							    (vc->reply.map_addr &
							     ~PAGE_MASK))));
			}
			/*
			 * The positive control for the decline probe below.
			 * That probe's whole value is in a zero -- "no death
			 * ran on invented memory" -- and a zero from a check
			 * that never fires says nothing (PRINCIPLES §7). A
			 * fault the monitor *served* is a page whose contents
			 * are known good, so running the identical read and
			 * fingerprint on the first N of them shows the
			 * instrument works and what a real page looks like.
			 */
			if (vmctx_decl_selftest) {
				unsigned int sum = 0;
				int rc;

				vmctx_decl_selftest--;
				rc = vmctx_probe_page(addr & PAGE_MASK, &sum);
				pr_warn(VMCTX_PR "pid %d: decl_selftest: after the monitor SERVED 0x%llx the guest's own mapping reads rc=%d sum 0x%08x (%s)\n",
					task_pid_nr(t),
					(unsigned long long)(addr & PAGE_MASK),
					rc, sum,
					rc ? "unreadable" :
					sum == VMCTX_ZERO_PAGE_SUM ? "all zero" :
					"real bytes");
			}
			ptregs_to_guest_tag(r, regs, WTAG_PF_SERVED);
			return VMCTX_RUN_CONTINUE;
		}
		if (backed_here) {
			vmctx_unback_fault(addr);
			backed_here = 0;
		}

		/*
		 * The monitor declined this one, so letting this kernel
		 * answer it is deliberate. Say so, or the page-fault path
		 * reports the very same fault to the monitor again.
		 *
		 * And record what it puts there: see vmctx_declined_to_local().
		 */
		ret = vmctx_declined_to_local(t, &vc->in_local_service, addr, err,
					      v->save.rip);
		if (ret == 0) {
			/* Page is present now; retry the instruction (no RIP++). */
			vc->faults++;
			return VMCTX_RUN_CONTINUE;
		}
		/*
		 * The local kernel could not serve a fault the monitor
		 * declined -- and the usual reason is that the mapping
		 * vanished BETWEEN the decline and this service: a racing
		 * stale vacate's punch landed on a construction whose record
		 * the monitor never saw (netsurf's font scan; tests/fc1,
		 * ~3 runs in 8). By NOW the absence is a settled fact the
		 * monitor's construction record can recognize, so report the
		 * fault AGAIN and let the record repair it (declined_at's
		 * cons-row replay). Bounded per address: a monitor with no
		 * answer, or none attached, still ends the context below
		 * rather than looping.
		 */
		if ((r->unrec_addr != (addr & PAGE_MASK) || r->unrec_n < 2) &&
		    vc->monitor) {
			int again;

			if (r->unrec_addr != (addr & PAGE_MASK)) {
				r->unrec_addr = addr & PAGE_MASK;
				r->unrec_n = 0;
			}
			r->unrec_n++;
			again = vmctx_redirect_fault(t, regs, 14, addr, err);
			pr_warn_ratelimited(VMCTX_PR "usercode: pid %d: fault 0x%llx failed the local service after a decline (%d); SECOND report answered %d\n",
					    task_pid_nr(t),
					    (unsigned long long)addr, ret,
					    again);
			if (again == 1) {
				vc->faults++;
				return VMCTX_RUN_CONTINUE;
			}
			if (again < 0) {
				vc->exit_status = again;
				ptregs_to_guest_tag(r, regs, WTAG_PF_EXIT);
				return VMCTX_RUN_EXIT;
			}
			/* Declined again: the death below, with its dump. */
		}
		{
			struct vmcb_save_area *sa = &v->save;
			char ring[24 * 5 + 1];
			int i, n = 0;

			for (i = 0; i < (int)ARRAY_SIZE(r->sysring); i++) {
				int idx = (r->sysring_i + i) % ARRAY_SIZE(r->sysring);

				n += scnprintf(ring + n, sizeof(ring) - n, "%u ",
					       r->sysring[idx]);
			}
			pr_warn(VMCTX_PR "usercode: unrecoverable guest #PF pid %d at 0x%llx err 0x%llx (%d)\n",
				task_pid_nr(t), addr, err, ret);
			pr_warn(VMCTX_PR "  rip=0x%llx rsp=0x%llx rax=0x%llx rflags=0x%llx cpl=%u\n",
				sa->rip, sa->rsp, sa->rax, sa->rflags, sa->cpl);
			pr_warn(VMCTX_PR "  rdi=0x%llx rsi=0x%llx rdx=0x%llx rcx=0x%llx rbx=0x%llx rbp=0x%llx\n",
				r->gctx.rdi, r->gctx.rsi, r->gctx.rdx,
				r->gctx.rcx, r->gctx.rbx, r->gctx.rbp);
			pr_warn(VMCTX_PR "  r8=0x%llx r10=0x%llx r11=0x%llx fs_base=0x%llx gs_base=0x%llx cr3=0x%llx\n",
				r->gctx.r8, r->gctx.r10, r->gctx.r11,
				sa->fs.base, sa->gs.base, sa->cr3);
			pr_warn(VMCTX_PR "  recent guest syscalls (oldest->newest): %s\n", ring);
			{
				char rh[8 * 20 + 1];
				int k, m = 0;

				for (k = 0; k < (int)ARRAY_SIZE(r->riphist); k++) {
					u32 idx = (r->riphist_i + k) %
						  ARRAY_SIZE(r->riphist);

					m += scnprintf(rh + m, sizeof(rh) - m,
						       "%llx ", r->riphist[idx]);
				}
				pr_warn(VMCTX_PR "  recent guest RIPs at exit (oldest->newest): %s\n",
					rh);
			}
			{
				char rh[8 * 40 + 1];
				int k, m = 0;

				for (k = 0; k < (int)ARRAY_SIZE(r->rsphist); k++) {
					u32 idx = (r->riphist_i + k) %
						  ARRAY_SIZE(r->rsphist);

					m += scnprintf(rh + m, sizeof(rh) - m,
						       "%llx/%llx ",
						       r->rsphist[idx],
						       r->rbphist[idx]);
				}
				if (vmctx_bpwatch) {
					char bw[8 * 22 + 1];
					int j, q = 0;

					for (j = 0; j < (int)ARRAY_SIZE(r->bpwordhist); j++) {
						u32 idx = (r->riphist_i + j) %
							  ARRAY_SIZE(r->bpwordhist);

						q += scnprintf(bw + q, sizeof(bw) - q,
							       r->bpwordok[idx] ?
							       "%llx " : "-%llx- ",
							       r->bpwordhist[idx]);
					}
					pr_warn(VMCTX_PR "  the word at [rbp] in the guest's own mapping at those exits (-x- = the page was not readable then): %s\n",
						bw);
				}
				pr_warn(VMCTX_PR "  rsp/rbp at those exits: %s\n",
					rh);
			}

			/*
			 * And every time this side changed the guest's rbp
			 * while the guest was not running. The dying thread's
			 * symptom is one of these -- rbp moved, rsp did not --
			 * and the tag says which publish did it, which is the
			 * difference between a defect in the frame protocol and
			 * one in a monitor's answer.
			 */
			{
				int k;
				u32 n = r->bpw_i;

				if (!n)
					pr_warn(VMCTX_PR "  the host never changed this guest's rbp between an exit and an entry\n");
				for (k = 0; k < (int)ARRAY_SIZE(r->bpw) &&
					    k < (int)n; k++) {
					u32 idx = (n - 1 - k) % ARRAY_SIZE(r->bpw);
					struct vmctx_bpw *w = &r->bpw[idx];

					pr_warn(VMCTX_PR "  host wrote rbp -%d: 0x%llx -> 0x%llx, rsp 0x%llx -> 0x%llx%s, after the exit at 0x%llx, tag %u, pt_regs->bp 0x%llx\n",
						k + 1, w->obp, w->nbp, w->osp,
						w->nsp,
						w->osp == w->nsp ?
						" (RSP UNCHANGED: a lost restore)" : "",
						w->rip, w->tag, w->ptbp);
				}
			}

			/*
			 * Symbol arithmetic on the faulting RIP is guesswork:
			 * print the bytes actually there, and the top of the
			 * guest stack, so the instruction and its operands can
			 * be read off directly.
			 */
			{
				unsigned char insn[16];
				unsigned long stk[6];
				int got;

				got = access_process_vm(t, sa->rip, insn,
						       sizeof(insn), 0);
				if (got == sizeof(insn))
					pr_warn(VMCTX_PR "  insn @rip: %*ph\n",
						(int)sizeof(insn), insn);
				else
					pr_warn(VMCTX_PR "  insn @rip: unreadable (%d)\n",
						got);

				got = access_process_vm(t, sa->rsp, stk,
							sizeof(stk), 0);
				if (got == sizeof(stk))
					pr_warn(VMCTX_PR "  [rsp]: %016lx %016lx %016lx %016lx %016lx %016lx\n",
						stk[0], stk[1], stk[2], stk[3],
						stk[4], stk[5]);
				else
					pr_warn(VMCTX_PR "  [rsp]: unreadable (%d)\n",
						got);

				/*
				 * And the words the stack has already given up.
				 * A guest that dies at rip=0 got there through
				 * a `ret`, which read the word at rsp-8 -- the
				 * one word the window above cannot show, and
				 * the only one that says whether the return
				 * address was zero in memory or lost on the
				 * way to the program counter.
				 */
				got = access_process_vm(t, sa->rsp - sizeof(stk),
							stk, sizeof(stk), 0);
				if (got == sizeof(stk))
					pr_warn(VMCTX_PR "  [rsp-0x30]: %016lx %016lx %016lx %016lx %016lx %016lx  (the last is [rsp-8], what a `ret` just popped)\n",
						stk[0], stk[1], stk[2], stk[3],
						stk[4], stk[5]);
				else
					pr_warn(VMCTX_PR "  [rsp-0x30]: unreadable (%d)\n",
						got);

				/*
				 * And the same window around rbp. If the word
				 * the faulting instruction loaded out of its
				 * own frame is right here and wrong in the
				 * register, the memory is fine and the frame
				 * pointer is not -- which is a different
				 * defect from the one a stale page causes, and
				 * the two are indistinguishable without this.
				 */
				got = access_process_vm(t, r->gctx.rbp - 0xa0,
							stk, sizeof(stk), 0);
				if (got == sizeof(stk))
					pr_warn(VMCTX_PR "  [rbp-0xa0]: %016lx %016lx %016lx %016lx %016lx %016lx\n",
						stk[0], stk[1], stk[2], stk[3],
						stk[4], stk[5]);
				else
					pr_warn(VMCTX_PR "  [rbp-0xa0]: unreadable (%d)\n",
						got);

				/*
				 * And the saved-frame-pointer slot itself. If
				 * rbp is one frame stale, this is the word the
				 * callee's `pop %rbp` read: it either still
				 * holds the caller's rbp -- in which case the
				 * memory was right and the register restore
				 * was lost -- or it does not, and the stack
				 * word is what changed. Two causes, one line.
				 */
				got = access_process_vm(t, r->gctx.rbp, stk,
							sizeof(stk), 0);
				if (got == sizeof(stk))
					pr_warn(VMCTX_PR "  [rbp]: %016lx %016lx %016lx %016lx %016lx %016lx\n",
						stk[0], stk[1], stk[2], stk[3],
						stk[4], stk[5]);
				else
					pr_warn(VMCTX_PR "  [rbp]: unreadable (%d)\n",
						got);
			}
		}
		/*
		 * Nobody could supply this page, so the context ends. It is
		 * not turned into a signal here and must not be: which signal
		 * a fault becomes is the owner's operating system's rule, and
		 * the owner has already declined this one.
		 *
		 * The two comments that used to stand here described a signal
		 * selection (SIGBUS for a truncated mapping, SEGV with MAPERR
		 * or ACCERR otherwise) that no code in this file has ever
		 * performed.
		 */
		vc->exit_status = -EFAULT;
		return VMCTX_RUN_EXIT;
	}

	if (ec >= SVM_EXIT_EXCP_BASE && ec <= SVM_EXIT_EXCP_BASE + 31) {
		u64 vec = ec - SVM_EXIT_EXCP_BASE;
		int rc;

		/*
		 * Any exception, reported the same way. #UD and #PF are dealt
		 * with above because this machinery makes them mean something
		 * of its own -- a SYSCALL leaves as #UD, and a page absent here
		 * is a page that lives on the other machine -- and everything
		 * else arrives here meaning only what the program did.
		 *
		 * What delivering from this arm was once found to do -- put the
		 * context at 0x20003f20, inside the monitor's own text, kill
		 * the monitor, and leave the guest spinning 85094 times -- was
		 * the frame, not the exit. With no guest_to_ptregs() first, the
		 * owner was shown whatever registers the previous stop left and
		 * its answer was written over those.
		 */
		/*
		 * A trap is reported standing *after* the instruction, a fault
		 * standing on it. That is the architecture's own division, not
		 * a reading of what the exception means, and it is this side's
		 * to get right: the owner is told where the program is, and a
		 * program that resumes on its own int3 executes it again for
		 * ever. tests/tr1 does exactly that -- the handler ran, decided
		 * correctly, returned, and trapped again at the same address,
		 * eight thousand times.
		 *
		 * INT3 and INTO are the software traps. next_rip carries the
		 * address after the instruction when the decode assist supplies
		 * it, which is the width-independent way to say "past it" --
		 * int3 has a one-byte and a two-byte encoding.
		 */
		if ((vec == 3 || vec == 4) && v->control.next_rip)
			v->save.rip = v->control.next_rip;

		guest_to_ptregs(r, regs);
		/*
		 * No faulting address: vector 14 is the one exception this arm
		 * never sees, because SVM_EXIT_EXCP_PF is handled above. (This
		 * used to read `vec == 14 ? exit_info_2 : 0`, a condition that
		 * cannot be true here.)
		 */
		rc = vmctx_redirect_fault(t, regs, vec, 0,
					  v->control.exit_info_1);
		if (rc == 1) {
			ptregs_to_guest_tag(r, regs, WTAG_EXC_REDIRECT);
			return VMCTX_RUN_CONTINUE;
		}
		if (rc < 0) {
			vc->exit_status = rc;
			return VMCTX_RUN_EXIT;
		}
		if (vec == 13)
			vmctx_report_gp(t, v->save.rip, v->save.rsp,
					v->control.exit_info_1);
		else
			pr_warn(VMCTX_PR "guest exception %llu at rip 0x%llx "
				"rsp 0x%llx (error 0x%llx), and its owner had "
				"no answer for it\n", vec, v->save.rip,
				v->save.rsp, v->control.exit_info_1);
		vc->exit_status = -EFAULT;
		return VMCTX_RUN_EXIT;
	}
	pr_warn(VMCTX_PR "usercode: unexpected exit 0x%x (ending)\n", ec);
	vc->exit_status = -(int)ec;
	return VMCTX_RUN_EXIT;
}

/*
 * Run the guest once, keeping the task's pt_regs as the canonical guest
 * register frame around it:
 *   - before entry, take back any changes the kernel made to the frame while
 *     we were out (signal delivery redirects RIP/RSP to a handler, syscall
 *     restart rewinds RIP, ptrace can poke registers);
 *   - after the exit *and* after any syscall/fault fixups, publish the guest
 *     state back into the frame.
 * Doing the publish at a single point after the fixups matters: syncing the
 * pre-syscall state would restore RIP onto the SYSCALL instruction and loop.
 */
static int vmctx_be_run_usercode(struct task_struct *t)
{
	struct svm_run *r = t->vmctx->backend_priv;
	struct pt_regs *regs = task_pt_regs(t);
	int ret;

	if (!r->frame_saved) {
		r->entry_frame = *regs;   /* the real vmctx_run(2) call frame */
		r->frame_saved = 1;
	}
	if (r->regs_live) {
		/*
		 * Whatever the kernel did to the frame while the guest was out.
		 * Signal delivery moves RIP to a handler, and the syscall
		 * restart machinery in arch_do_signal_or_restart() rewinds it
		 * by two -- onto the guest's SYSCALL instruction, which is
		 * exactly where a re-executed syscall would come from. That
		 * second one is invisible from here otherwise: it is the core's
		 * doing, not this module's, and the only evidence a run leaves
		 * is one extra syscall in the count. Report the move and let
		 * the size of it say which of the two it was.
		 */
		{
			struct vmcb *v = r->vmcb;

			/*
			 * Not on the first entry. A context that has never run
			 * has a VMCB rip of zero and a frame holding wherever it
			 * is to start -- a cloned context takes its parent's --
			 * so the two differ by construction and it is not the
			 * kernel having moved anything. Reporting it anyway put
			 * one line in every run of th9, passing and failing
			 * alike, which is a message that cannot distinguish
			 * anything and so says nothing.
			 */
			if (v->save.rip && regs->ip != v->save.rip)
				pr_warn_ratelimited(VMCTX_PR "pid %d: the kernel moved guest rip from 0x%llx to 0x%llx (%+lld) while it was out; orig_ax=%ld ax=0x%lx\n",
					task_pid_nr(t), v->save.rip, regs->ip,
					(long long)regs->ip - (long long)v->save.rip,
					(long)regs->orig_ax, (unsigned long)regs->ax);
		}
		ptregs_to_guest_tag(r, regs, WTAG_PREENTRY);
	}

	ret = vmctx_be_run_usercode_once(t);

	/*
	 * If the guest exec'd during this run, pt_regs now holds the entry frame
	 * that exec installed for the new program — which is exactly what the
	 * core uses to rebuild the guest. Publishing the old guest state over it
	 * would send the rebuilt guest to a stale RIP.
	 */
	if (!t->vmctx->needs_rebuild) {
		guest_to_ptregs(r, regs);
		r->regs_live = 1;
	}
	return ret;
}

static int vmctx_be_create_usercode(struct task_struct *t)
{
	struct vmctx_task *vc = t->vmctx;
	struct svm_run *r;
	int ret;

	r = kzalloc(sizeof(*r), GFP_KERNEL);
	if (!r)
		return -ENOMEM;
	ret = svm_run_alloc(r);
	if (ret) {
		kfree(r);
		return ret;
	}
	svm_build_npt_identity(r);
	svm_build_vmcb_usercode(r, vc->entry, vc->stack);
	r->last_sysnr = -1;

	/*
	 * Restore (migration): the guest must resume from a checkpointed
	 * register set rather than a fresh entry point. Treat the task's pt_regs
	 * as already holding guest state, so the first entry loads it — a
	 * monitor SETREGSes the saved registers while the context waits for it
	 * (VMCTX_FLAG_WAIT_MONITOR), and the guest continues from there.
	 */
	if (vc->flags & VMCTX_FLAG_RESTORE) {
		r->entry_frame = *task_pt_regs(current);
		r->frame_saved = 1;
		r->regs_live   = 1;
	}
	vc->backend_priv = r;
	pr_info(VMCTX_PR "usercode: pid %d guest entry 0x%llx stack 0x%llx cr3 0x%llx\n",
		task_pid_nr(t), vc->entry, vc->stack, virt_to_phys(r->npt[0]));
	return 0;
}

/* ---- native process backend (registered with the kernel core) --------- */
/*
 * These are called by kernel/vmctx.c on the vmctx_run(2) path, in the
 * context of the calling task. create() builds a persistent guest;
 * run() enters it once per call; the core loop handles scheduling and
 * signals between calls, which is what makes the VM context a normal,
 * killable, schedulable process.
 */
static int vmctx_be_create(struct task_struct *t)
{
	struct svm_run *r;
	int ret;

	if (g_caps.vendor != VMCTX_VENDOR_AMD ||
	    !(g_caps.caps & (VMCTX_CAP_PRESENT | VMCTX_CAP_ENABLED)))
		return -ENODEV;

	if (t->vmctx->flags & VMCTX_FLAG_USERCODE)
		return vmctx_be_create_usercode(t);

	r = kzalloc(sizeof(*r), GFP_KERNEL);
	if (!r)
		return -ENOMEM;
	ret = svm_run_alloc(r);
	if (ret) {
		kfree(r);
		return ret;
	}
	svm_build_npt(r);
	svm_build_vmcb(r, counter_payload, sizeof(counter_payload));
	t->vmctx->backend_priv = r;
	pr_info(VMCTX_PR "be_create: pid %d is now a VM context (vmcb 0x%llx)\n",
		task_pid_nr(t), (u64)virt_to_phys(r->vmcb));
	return 0;
}

static int vmctx_be_run(struct task_struct *t)
{
	struct vmctx_task *vc = t->vmctx;
	struct svm_run *r = vc->backend_priv;
	struct vmcb *v = r->vmcb;
	int ret;

	if (vc->flags & VMCTX_FLAG_USERCODE)
		return vmctx_be_run_usercode(t);

	ret = svm_vmrun_once(r);
	if (ret)
		return ret;

	vc->enters++;
	vc->exits++;
	vc->last_exit_reason = v->control.exit_code;

	if (v->control.exit_code == SVM_EXIT_VMMCALL) {
		v->save.rip += 3;   /* step past the 3-byte VMMCALL, then resume */
		vc->counter = *(u16 *)((u8 *)r->code + 0x100);
		return VMCTX_RUN_CONTINUE;
	}

	/* Any other #VMEXIT ends this guest. */
	vc->exit_status = -(int)v->control.exit_code;
	return VMCTX_RUN_EXIT;
}

static void vmctx_be_destroy(struct task_struct *t)
{
	struct svm_run *r = t->vmctx->backend_priv;

	/*
	 * Why a context ended, at the one point every way of ending passes
	 * through -- and only when it ended badly, so a passing run stays
	 * silent and a failing one says which branch it took.
	 *
	 * This is what found the livelock: `status -125` is the monitor's
	 * VMCTX_ACT_KILL arriving as -ECANCELED, which is vmremote's watchdog
	 * giving up on an address that faulted forty times. Nothing else in
	 * either log named it -- the guest looked like it had simply exited,
	 * because do_exit((status & 0xff) << 8) makes -125 indistinguishable
	 * from exit(131) to anything reading a wait status.
	 */
	if (t->vmctx->exit_status)
		pr_warn(VMCTX_PR "usercode: context pid %d ends: status %d (process exit %d), last exit reason 0x%llx, %llu enters %llu exits %llu syscalls %llu faults\n",
		task_pid_nr(t), t->vmctx->exit_status,
		t->vmctx->exit_status & 0xff,
		(unsigned long long)t->vmctx->last_exit_reason,
		(unsigned long long)t->vmctx->enters,
		(unsigned long long)t->vmctx->exits,
		(unsigned long long)t->vmctx->counter,
		(unsigned long long)t->vmctx->faults);

	/*
	 * If the context is ending and the task will return from vmctx_run(2),
	 * put its real user frame back (keeping the return value the core just
	 * stored in AX). When the guest *is* the process there is nothing to
	 * return to and the core exits the task instead.
	 */
	if (r && r->frame_saved && r->regs_live && !t->vmctx->exit_process) {
		struct pt_regs *regs = task_pt_regs(t);
		unsigned long ax = regs->ax;

		*regs = r->entry_frame;
		regs->ax = ax;
	}

	if (r) {
		svm_run_free(r);
		kfree(r);
		t->vmctx->backend_priv = NULL;
	}
}

/* ==================== Intel VMX backend ================================ */
/*
 * The same guest model as the SVM path above, on Intel hardware: the calling
 * process's own user code runs as a long-mode guest on the process's page
 * tables, with EPT identity-mapping guest-physical to host-physical so those
 * page tables mean the same thing on both sides. Guest page faults are the
 * host's own demand paging, and guest syscalls come out as #UD because the
 * guest runs with EFER.SCE clear.
 *
 * Three things differ from SVM and none of them are optional:
 *
 *   - CPUID exits unconditionally on VMX. There is no control to let a guest
 *     execute it natively, so it has to be emulated. Real programs reach it
 *     early — the dynamic loader resolves ifuncs by CPU feature — so a guest
 *     without this does not get as far as its first instruction from libc.
 *
 *   - VMX has no SYSCALL intercept either, which is why EFER.SCE is cleared
 *     and #UD is caught instead. That part the SVM path already does.
 *
 *   - Host and guest state that SVM keeps in the VMCB has to be written into
 *     the VMCS field by field, and the VMCS can only be touched while it is
 *     the current one. Everything the exit handler needs is therefore read out
 *     into the run struct before the VMCS is released.
 */

#define VMX_EPTP_WB          (6ULL)        /* memory type write-back        */
#define VMX_EPTP_WALK4       (3ULL << 3)   /* page-walk length 4            */
#define VMX_EPT_RWX          (0x7ULL)
#define VMX_EPT_MEMTYPE_WB   (6ULL << 3)
#define VMX_EPT_LARGE        (1ULL << 7)

/* VMCS field encodings used from inline asm, where a symbol will not do. */
#define VMX_HOST_RSP_ENC     0x6c14
#define VMX_HOST_RIP_ENC     0x6c16

static inline int vmx_vmclear(u64 pa)
{
	u8 err;

	asm volatile("vmclear %[pa]; setna %[err]"
		     : [err] "=qm"(err) : [pa] "m"(pa) : "cc", "memory");
	return err;
}

static inline int vmx_vmptrld(u64 pa)
{
	u8 err;

	asm volatile("vmptrld %[pa]; setna %[err]"
		     : [err] "=qm"(err) : [pa] "m"(pa) : "cc", "memory");
	return err;
}

static inline u64 vmcs_read(u32 field)
{
	u64 value = 0;

	asm volatile("vmread %[field], %[value]"
		     : [value] "=rm"(value) : [field] "r"((u64)field) : "cc");
	return value;
}

static inline int vmcs_write(u32 field, u64 value)
{
	u8 err;

	asm volatile("vmwrite %[value], %[field]; setna %[err]"
		     : [err] "=qm"(err)
		     : [value] "rm"(value), [field] "r"((u64)field)
		     : "cc");
	return err;
}

/*
 * Settle one control field against its capability MSR. Bits set in the low
 * half must be 1; bits clear in the high half must be 0. Asking for a control
 * the CPU does not allow is not a warning at entry time, it is a VM-entry
 * failure with a reason code, so let the MSR decide.
 */
static u32 vmx_adjust_ctl(u32 msr, u32 want)
{
	u64 v = 0;

	if (rdmsrq_safe(msr, &v))
		return want;
	return (want | (u32)v) & (u32)(v >> 32);
}

/* ---- VMX root operation, per CPU -------------------------------------- */
/*
 * Entered once per CPU and left on. Doing it around every guest entry would
 * be correct too and costs a VMXON/VMXOFF pair each time; this is the same
 * decision KVM makes, and for a guest that exits on every syscall the entry
 * path is worth keeping short.
 */
static void *vmx_vmxon_region[NR_CPUS];
static DEFINE_PER_CPU(bool, vmx_root_on);
/*
 * Which guest's VMCS is current on this CPU, or NULL. Releasing the VMCS after
 * every entry was simple and correct, but it costs a VMCLEAR, a VMPTRLD and a
 * VMLAUNCH where a VMRESUME would do — invisible when a syscall is a network
 * round trip, and a third of the exit cost when it is not.
 *
 * Keeping it loaded means this pointer has to stay true. Every VMPTRLD and
 * every VMCLEAR goes through the two helpers below, because a VMCLEAR that
 * leaves a stale pointer here is not a slow path, it is a guest resuming on
 * another guest's VMCS.
 */
static DEFINE_PER_CPU(struct vmx_run *, vmx_cur);

static int vmx_cpu_root_enter(void)	/* preemption disabled by caller */
{
	int cpu = smp_processor_id();
	void *reg = vmx_vmxon_region[cpu];
	u64 cr4, fixed0 = 0, fixed1 = ~0ULL;

	if (this_cpu_read(vmx_root_on))
		return 0;
	if (!reg)
		return -ENOMEM;

	*(u32 *)reg = (u32)(g_caps.vmx_basic & 0x7fffffff);   /* revision id */

	cr4 = rd_cr4();
	if (!(cr4 & X86_CR4_VMXE))
		wr_cr4(cr4 | X86_CR4_VMXE);

	rdmsrq_safe(MSR_IA32_VMX_CR4_FIXED0, &fixed0);
	rdmsrq_safe(MSR_IA32_VMX_CR4_FIXED1, &fixed1);
	if ((rd_cr4() & fixed0) != fixed0 || (rd_cr4() & ~fixed1))
		pr_warn(VMCTX_PR "vmx: CR4 0x%llx does not satisfy the fixed bits "
			"(0x%llx/0x%llx)\n", rd_cr4(), fixed0, fixed1);

	if (do_vmxon(virt_to_phys(reg))) {
		wr_cr4(cr4);
		return -EIO;
	}
	this_cpu_write(vmx_root_on, true);
	return 0;
}

static void vmx_cpu_root_leave(void *unused)
{
	if (this_cpu_read(vmx_root_on)) {
		do_vmxoff();
		this_cpu_write(vmx_root_on, false);
		wr_cr4(rd_cr4() & ~X86_CR4_VMXE);
	}
}

/* ---- the guest ---------------------------------------------------------- */

struct vmx_run;

struct vmx_gctx {
	u64 rax;	/* 0x00 */
	u64 rbx;	/* 0x08 */
	u64 rcx;	/* 0x10 */
	u64 rdx;	/* 0x18 */
	u64 rsi;	/* 0x20 */
	u64 rdi;	/* 0x28 */
	u64 rbp;	/* 0x30 */
	u64 r8;		/* 0x38 */
	u64 r9;		/* 0x40 */
	u64 r10;	/* 0x48 */
	u64 r11;	/* 0x50 */
	u64 r12;	/* 0x58 */
	u64 r13;	/* 0x60 */
	u64 r14;	/* 0x68 */
	u64 r15;	/* 0x70 */
	u64 fail;	/* 0x78 — VM entry failed rather than the guest exiting */
	/*
	 * 0x80 — VMLAUNCH has been done, so resume from here. It lives in this
	 * struct rather than being an asm memory operand because by the time
	 * the entry needs it, RBP and RSP hold the guest's values and the
	 * compiler would have addressed it through one of them.
	 */
	u64 launched;
};

struct vmx_run {
	void *vmcs;
	void *msr_bitmap;
	void *ept[2];		/* pml4, pdpt (1 GiB leaves)                 */
	struct vmx_gctx gctx;

	/*
	 * Guest state that changes per entry. Held here rather than read back
	 * out of the VMCS on demand, because the VMCS is only readable while it
	 * is the current one and it is released as soon as the entry returns.
	 */
	u64 rip, rsp, rflags, cr3, fs_base, gs_base;
	u64 gpa;		/* GUEST_PHYSICAL_ADDRESS of the last EPT exit */

	int loaded_cpu;		/* CPU where this VMCS is current, else -1   */
	/*
	 * What was last written into the VMCS for fields that seldom change.
	 * A VMCS field costs tens of cycles to write and these are the same
	 * value entry after entry — CR3 moves on exec, the TLS bases on
	 * arch_prctl, and neither happens in a loop. VMCLEAR writes the VMCS
	 * back to memory and VMPTRLD reads it in again, so the contents
	 * survive a move between CPUs and the cache stays true; it is
	 * invalidated on a reload anyway, which is rare and cheap.
	 */
	u64 vm_cr3, vm_fs, vm_gs, vm_host_fs;
	int vm_cached;

	/* What the last exit was, captured before the VMCS was released. */
	u32 exit_reason;
	u32 intr_info;
	u32 intr_error;
	u32 instr_len;
	u64 exit_qual;

	/* Same bookkeeping the SVM path keeps; see struct svm_run. */
	struct pt_regs entry_frame;
	int frame_saved;
	int regs_live;
	u64 riphist[8];
	/* rsp and rbp too; see the same fields in struct svm_run. */
	u64 rsphist[8];
	u64 rbphist[8];
	u32 riphist_i;
	long last_sysnr;
	u16 sysring[24];
	u8  sysring_i;
	/* The second-chance report; see the same fields in struct svm_run. */
	u64 unrec_addr;
	u32 unrec_n;
	struct vmctx_spin spin;
};

/* Give up this VMCS on the CPU that holds it. Must run on that CPU. */
static void vmx_clear_here(void *info)
{
	struct vmx_run *r = info;

	if (this_cpu_read(vmx_cur) == r) {
		vmx_vmclear(virt_to_phys(r->vmcs));
		this_cpu_write(vmx_cur, NULL);
	}
	r->loaded_cpu = -1;
	r->gctx.launched = 0;
}

static void vmx_release(struct vmx_run *r)
{
	if (r->loaded_cpu < 0)
		return;
	if (r->loaded_cpu == raw_smp_processor_id())
		vmx_clear_here(r);
	else
		smp_call_function_single(r->loaded_cpu, vmx_clear_here, r, 1);
}

/*
 * Make this VMCS current here. Returns 1 if it had to be loaded — which is also
 * when the next entry must be a VMLAUNCH, and when the per-CPU host state in
 * the VMCS describes some other CPU and has to be written again.
 */
static int vmx_make_current(struct vmx_run *r)
{
	int cpu = raw_smp_processor_id();
	struct vmx_run *cur = this_cpu_read(vmx_cur);

	if (cur == r && r->loaded_cpu == cpu)
		return 0;
	if (r->loaded_cpu >= 0 && r->loaded_cpu != cpu)
		smp_call_function_single(r->loaded_cpu, vmx_clear_here, r, 1);
	/* Whatever was current here is about to stop being so. */
	if (cur && cur != r)
		cur->loaded_cpu = -1;
	this_cpu_write(vmx_cur, NULL);
	if (vmx_vmclear(virt_to_phys(r->vmcs)) ||
	    vmx_vmptrld(virt_to_phys(r->vmcs)))
		return -1;
	this_cpu_write(vmx_cur, r);
	r->loaded_cpu = cpu;
	r->gctx.launched = 0;
	r->vm_cached = 0;
	return 1;
}

static void vmx_run_free(struct vmx_run *r)
{
	int i;

	vmx_release(r);	/* never free memory a CPU still points its VMCS at */

	if (r->vmcs)       free_page((unsigned long)r->vmcs);
	if (r->msr_bitmap) free_page((unsigned long)r->msr_bitmap);
	for (i = 0; i < 2; i++)
		if (r->ept[i]) free_page((unsigned long)r->ept[i]);
}

static int vmx_run_alloc(struct vmx_run *r)
{
	int i;

	memset(r, 0, sizeof(*r));
	r->vmcs       = (void *)get_zeroed_page(GFP_KERNEL);
	r->msr_bitmap = (void *)get_zeroed_page(GFP_KERNEL);
	for (i = 0; i < 2; i++)
		r->ept[i] = (void *)get_zeroed_page(GFP_KERNEL);
	if (!r->vmcs || !r->msr_bitmap || !r->ept[0] || !r->ept[1]) {
		vmx_run_free(r);
		return -ENOMEM;
	}
	*(u32 *)r->vmcs = (u32)(g_caps.vmx_basic & 0x7fffffff);  /* revision  */
	r->loaded_cpu = -1;
	return 0;
}

/*
 * EPT identity-mapping the low 512 GiB with 1 GiB leaves, so a guest-physical
 * address is the host-physical address it names. That is what lets the guest
 * run on the process's own page tables: the process's PTEs hold host-physical
 * frame numbers, and under this EPT they mean the same frames to the guest.
 */
static void vmx_build_ept_identity(struct vmx_run *r)
{
	u64 *pml4 = r->ept[0], *pdpt = r->ept[1];
	int i;

	memset(pml4, 0, PAGE_SIZE);
	pml4[0] = virt_to_phys(pdpt) | VMX_EPT_RWX;
	for (i = 0; i < 512; i++)
		pdpt[i] = ((u64)i << 30) | VMX_EPT_RWX | VMX_EPT_MEMTYPE_WB |
			  VMX_EPT_LARGE;
}

/*
 * Which page tables the guest runs on.
 *
 * Not mm->pgd, when page-table isolation is on. PTI splits every process's PGD
 * in two: the kernel-side copy the CPU uses in kernel mode, and a user-side
 * copy it switches to on the way out. The kernel-side copy deliberately marks
 * the user half no-execute, so that the kernel cannot be tricked into running
 * user code — and a guest handed that PGD cannot fetch its own first
 * instruction either. It faults on its entry point with the page present, the
 * access user-mode, and the fetch bit set: an NX violation on the program's own
 * text.
 *
 * The user-side copy maps exactly the same user memory without that, and the
 * guest needs nothing else — it never touches kernel addresses, and the host's
 * CR3 is restored from the VMCS on every exit. This is invisible on AMD, which
 * is not affected by Meltdown and does not turn PTI on: the same code that
 * works there fails here for a reason that has nothing to do with either
 * vendor's virtualization.
 */
static u64 vmx_guest_cr3_of(struct task_struct *t)
{
	pgd_t *pgd;

	if (!t->mm)
		return rd_cr3();
	pgd = t->mm->pgd;
#ifdef CONFIG_MITIGATION_PAGE_TABLE_ISOLATION
	/* The config symbol was CONFIG_PAGE_TABLE_ISOLATION before 6.12 and is
	 * CONFIG_MITIGATION_PAGE_TABLE_ISOLATION now. Guarding on the old name
	 * compiles this away in silence and leaves the guest on the PGD that
	 * cannot execute it. */
	if (boot_cpu_has(X86_FEATURE_PTI))
		pgd = kernel_to_user_pgdp(pgd);
#endif
	return virt_to_phys(pgd);
}

static void vmx_sync_tls_bases(struct vmctx_task *vc, struct vmx_run *r)
{
	u64 fsb = 0, gsb = 0;

	/*
	 * The monitor's word first; the live MSR only when no monitor has
	 * spoken. The SVM twin has the full story: the live MSR is whatever
	 * base this CPU carries, which for a freshly spawned context that
	 * never descheduled is the MONITOR's own thread pointer, invisible to
	 * every read-back because thread.fsbase reads correct throughout.
	 */
	if (vc && vc->fsgs_valid) {
		r->fs_base = vc->guest_fsbase;
		r->gs_base = vc->guest_gsbase;
		return;
	}
	rdmsrq_safe(MSR_FS_BASE, &fsb);
	rdmsrq_safe(MSR_KERNEL_GS_BASE, &gsb);
	r->fs_base = fsb;
	r->gs_base = gsb;
}

/* Everything in the VMCS that does not change from one entry to the next. */
static int vmx_build_vmcs(struct vmx_run *r, u64 entry, u64 stack)
{
	u32 pin, cpu_based, secondary, exit_ctl, entry_ctl;
	u64 basic = g_caps.vmx_basic;
	bool have_true = !!(basic & (1ULL << 55));
	struct desc_ptr dt;
	u64 v;

	/*
	 * The TRUE capability MSRs are the ones that let the default-1 controls
	 * be cleared. Without them CR3-load/store exiting cannot be turned off,
	 * and every CR3 write the guest never makes would still cost an exit.
	 */
	pin = vmx_adjust_ctl(have_true ? MSR_IA32_VMX_TRUE_PINBASED_CTLS
					: MSR_IA32_VMX_PINBASED_CTLS,
			     PIN_BASED_EXT_INTR_MASK | PIN_BASED_NMI_EXITING);
	cpu_based = vmx_adjust_ctl(have_true ? MSR_IA32_VMX_TRUE_PROCBASED_CTLS
					     : MSR_IA32_VMX_PROCBASED_CTLS,
				   CPU_BASED_ACTIVATE_SECONDARY_CONTROLS |
				   CPU_BASED_USE_MSR_BITMAPS);
	secondary = vmx_adjust_ctl(MSR_IA32_VMX_PROCBASED_CTLS2,
				   SECONDARY_EXEC_ENABLE_EPT |
				   SECONDARY_EXEC_UNRESTRICTED_GUEST |
				   SECONDARY_EXEC_ENABLE_RDTSCP);
	exit_ctl = vmx_adjust_ctl(have_true ? MSR_IA32_VMX_TRUE_EXIT_CTLS
					    : MSR_IA32_VMX_EXIT_CTLS,
				  VM_EXIT_HOST_ADDR_SPACE_SIZE |
				  VM_EXIT_SAVE_IA32_EFER |
				  VM_EXIT_LOAD_IA32_EFER);
	entry_ctl = vmx_adjust_ctl(have_true ? MSR_IA32_VMX_TRUE_ENTRY_CTLS
					     : MSR_IA32_VMX_ENTRY_CTLS,
				   VM_ENTRY_IA32E_MODE |
				   VM_ENTRY_LOAD_IA32_EFER);

	if (!(secondary & SECONDARY_EXEC_ENABLE_EPT)) {
		pr_err(VMCTX_PR "vmx: EPT unavailable; this backend needs it\n");
		return -ENODEV;
	}

	vmcs_write(PIN_BASED_VM_EXEC_CONTROL, pin);
	vmcs_write(CPU_BASED_VM_EXEC_CONTROL, cpu_based);
	vmcs_write(SECONDARY_VM_EXEC_CONTROL, secondary);
	vmcs_write(VM_EXIT_CONTROLS, exit_ctl);
	vmcs_write(VM_ENTRY_CONTROLS, entry_ctl);
	vmcs_write(VM_EXIT_MSR_STORE_COUNT, 0);
	vmcs_write(VM_EXIT_MSR_LOAD_COUNT, 0);
	vmcs_write(VM_ENTRY_MSR_LOAD_COUNT, 0);
	vmcs_write(VM_ENTRY_INTR_INFO_FIELD, 0);

	/* An empty bitmap: no MSR access is intercepted. The guest is CPL3 and
	 * cannot execute RDMSR/WRMSR anyway; this only keeps them off the exit
	 * path entirely. */
	memset(r->msr_bitmap, 0, PAGE_SIZE);
	vmcs_write(MSR_BITMAP, virt_to_phys(r->msr_bitmap));

	/*
	 * #UD is how a guest syscall arrives (EFER.SCE is clear below) and #PF
	 * is how demand paging arrives. The error-code mask/match pair are both
	 * zero so that every page fault exits, not a subset of them.
	 */
	/*
	 * Every exception the program can raise, not a chosen few: the guest
	 * has no IDT, so one left uncaught is not handled by the guest either
	 * but faults on a gate at sixteen times its vector. See the SVM side's
	 * note. All but the NMI (2), #NM (7) and #MC (18), which are the host's.
	 */
	vmcs_write(EXCEPTION_BITMAP,
		   0xffffffffu & ~((1u << 2) | (1u << 7) | (1u << 18)));
	vmcs_write(PAGE_FAULT_ERROR_CODE_MASK, 0);
	vmcs_write(PAGE_FAULT_ERROR_CODE_MATCH, 0);

	vmcs_write(EPT_POINTER,
		   virt_to_phys(r->ept[0]) | VMX_EPTP_WB | VMX_EPTP_WALK4);
	vmcs_write(VMCS_LINK_POINTER, ~0ULL);

	/* ---- host state: where the CPU lands on VM exit ------------------ */
	vmcs_write(HOST_CR0, rd_cr0());
	vmcs_write(HOST_CR3, rd_cr3());
	vmcs_write(HOST_CR4, rd_cr4() | X86_CR4_VMXE);
	vmcs_write(HOST_CS_SELECTOR, __KERNEL_CS);
	vmcs_write(HOST_SS_SELECTOR, __KERNEL_DS);
	vmcs_write(HOST_DS_SELECTOR, __KERNEL_DS);
	vmcs_write(HOST_ES_SELECTOR, __KERNEL_DS);
	vmcs_write(HOST_FS_SELECTOR, 0);
	vmcs_write(HOST_GS_SELECTOR, 0);
	vmcs_write(HOST_TR_SELECTOR, GDT_ENTRY_TSS * 8);
	native_store_gdt(&dt);
	vmcs_write(HOST_GDTR_BASE, dt.address);
	store_idt(&dt);
	vmcs_write(HOST_IDTR_BASE, dt.address);
	vmcs_write(HOST_TR_BASE, (unsigned long)this_cpu_ptr(&cpu_tss_rw));
	v = 0; rdmsrq_safe(MSR_FS_BASE, &v);        vmcs_write(HOST_FS_BASE, v);
	v = 0; rdmsrq_safe(MSR_GS_BASE, &v);        vmcs_write(HOST_GS_BASE, v);
	v = 0; rdmsrq_safe(MSR_IA32_SYSENTER_ESP, &v);
	vmcs_write(HOST_IA32_SYSENTER_ESP, v);
	v = 0; rdmsrq_safe(MSR_IA32_SYSENTER_EIP, &v);
	vmcs_write(HOST_IA32_SYSENTER_EIP, v);
	v = 0; rdmsrq_safe(MSR_IA32_SYSENTER_CS, &v);
	vmcs_write(HOST_IA32_SYSENTER_CS, (u32)v);
	vmcs_write(HOST_IA32_EFER, rd_efer());

	/* ---- guest state ------------------------------------------------- */
	vmcs_write(GUEST_CR0, rd_cr0());
	vmcs_write(GUEST_CR4, rd_cr4() | X86_CR4_VMXE);
	vmcs_write(CR0_GUEST_HOST_MASK, 0);
	vmcs_write(CR4_GUEST_HOST_MASK, 0);
	vmcs_write(CR0_READ_SHADOW, rd_cr0());
	vmcs_write(CR4_READ_SHADOW, rd_cr4());
	vmcs_write(GUEST_IA32_EFER, rd_efer() & ~EFER_SCE);  /* SYSCALL -> #UD */

	/* 64-bit user segments, exactly as the SVM path sets them. */
	vmcs_write(GUEST_CS_SELECTOR, 0x33);
	vmcs_write(GUEST_CS_BASE, 0);
	vmcs_write(GUEST_CS_LIMIT, 0xffffffff);
	vmcs_write(GUEST_CS_AR_BYTES, 0xa0fb);	/* type 11, S, DPL3, P, L, G */
	vmcs_write(GUEST_SS_SELECTOR, 0x2b);
	vmcs_write(GUEST_SS_BASE, 0);
	vmcs_write(GUEST_SS_LIMIT, 0xffffffff);
	vmcs_write(GUEST_SS_AR_BYTES, 0xc0f3);	/* type 3, S, DPL3, P, D, G  */
	vmcs_write(GUEST_DS_SELECTOR, 0x2b);
	vmcs_write(GUEST_DS_BASE, 0);
	vmcs_write(GUEST_DS_LIMIT, 0xffffffff);
	vmcs_write(GUEST_DS_AR_BYTES, 0xc0f3);
	vmcs_write(GUEST_ES_SELECTOR, 0x2b);
	vmcs_write(GUEST_ES_BASE, 0);
	vmcs_write(GUEST_ES_LIMIT, 0xffffffff);
	vmcs_write(GUEST_ES_AR_BYTES, 0xc0f3);
	vmcs_write(GUEST_FS_SELECTOR, 0);
	vmcs_write(GUEST_FS_LIMIT, 0xffffffff);
	vmcs_write(GUEST_FS_AR_BYTES, 0xc0f3);
	vmcs_write(GUEST_GS_SELECTOR, 0);
	vmcs_write(GUEST_GS_LIMIT, 0xffffffff);
	vmcs_write(GUEST_GS_AR_BYTES, 0xc0f3);
	/*
	 * LDTR may be unusable, TR may not: VM entry rejects a guest whose TR
	 * is not a usable 64-bit busy TSS, however little the guest cares.
	 */
	vmcs_write(GUEST_LDTR_SELECTOR, 0);
	vmcs_write(GUEST_LDTR_BASE, 0);
	vmcs_write(GUEST_LDTR_LIMIT, 0);
	vmcs_write(GUEST_LDTR_AR_BYTES, 0x10000);	/* unusable          */
	vmcs_write(GUEST_TR_SELECTOR, 0);
	vmcs_write(GUEST_TR_BASE, 0);
	vmcs_write(GUEST_TR_LIMIT, 0xffff);
	vmcs_write(GUEST_TR_AR_BYTES, 0x8b);		/* 64-bit busy TSS   */
	vmcs_write(GUEST_GDTR_BASE, 0);
	vmcs_write(GUEST_GDTR_LIMIT, 0xffff);
	vmcs_write(GUEST_IDTR_BASE, 0);
	vmcs_write(GUEST_IDTR_LIMIT, 0xffff);

	vmcs_write(GUEST_DR7, 0x400);
	vmcs_write(GUEST_ACTIVITY_STATE, 0);
	vmcs_write(GUEST_INTERRUPTIBILITY_INFO, 0);
	vmcs_write(GUEST_PENDING_DBG_EXCEPTIONS, 0);
	vmcs_write(GUEST_SYSENTER_CS, 0);
	vmcs_write(GUEST_SYSENTER_ESP, 0);
	vmcs_write(GUEST_SYSENTER_EIP, 0);

	r->rip    = entry;
	r->rsp    = stack;
	r->rflags = X86_EFLAGS_FIXED;
	r->cr3    = vmx_guest_cr3_of(current);
	vmx_sync_tls_bases(current->vmctx, r);
	return 0;
}

/*
 * One VM entry. The VMCS is made current, the state that changes per entry is
 * written in, the guest runs until it exits, and everything the caller will
 * need is read back out before the VMCS is released again.
 *
 * Released every time, deliberately. A VMCS may only be current on one CPU,
 * and this task is schedulable between entries — so either the old CPU gets
 * an IPI to flush it, or it is flushed here while we are still on the CPU that
 * has it. The second is a few hundred cycles and no cross-CPU machinery.
 */
static int vmx_enter_once(struct vmx_run *r)
{
	unsigned long flags, ctx = (unsigned long)&r->gctx;
	u64 host_cr2;
	int ret = 0, reloaded;

	preempt_disable();
	vmctx_load_task_fpu();
	ret = vmx_cpu_root_enter();
	if (ret)
		goto out;
	reloaded = vmx_make_current(r);
	if (reloaded < 0) {
		ret = -EIO;
		goto out;
	}

	/* These three move on every exit and are always written. */
	vmcs_write(GUEST_RIP, r->rip);
	vmcs_write(GUEST_RSP, r->rsp);
	vmcs_write(GUEST_RFLAGS, r->rflags | X86_EFLAGS_FIXED);
	/* These three almost never do. */
	if (!r->vm_cached || r->vm_cr3 != r->cr3) {
		vmcs_write(GUEST_CR3, r->cr3);
		r->vm_cr3 = r->cr3;
	}
	if (!r->vm_cached || r->vm_fs != r->fs_base) {
		vmcs_write(GUEST_FS_BASE, r->fs_base);
		r->vm_fs = r->fs_base;
	}
	if (!r->vm_cached || r->vm_gs != r->gs_base) {
		vmcs_write(GUEST_GS_BASE, r->gs_base);
		r->vm_gs = r->gs_base;
	}

	/*
	 * Host state that belongs to *this* CPU, rewritten every entry. The GDT,
	 * the IDT, the TSS and the per-CPU GS base are all per-CPU, and the task
	 * is schedulable between entries — so the values captured when the VMCS
	 * was built describe whichever CPU happened to run that code. Getting
	 * these wrong is not a guest problem: the CPU reloads them on VM exit,
	 * and the host then takes its next interrupt through another CPU's IDT.
	 */
	if (reloaded) {
		struct desc_ptr dt;
		u64 v = 0;

		native_store_gdt(&dt);
		vmcs_write(HOST_GDTR_BASE, dt.address);
		store_idt(&dt);
		vmcs_write(HOST_IDTR_BASE, dt.address);
		vmcs_write(HOST_TR_BASE, (unsigned long)this_cpu_ptr(&cpu_tss_rw));
		vmcs_write(HOST_CR3, rd_cr3());
		rdmsrq_safe(MSR_GS_BASE, &v);
		vmcs_write(HOST_GS_BASE, v);
		/*
		 * And the FS base, which is the task's *user* TLS pointer while
		 * it is in the kernel. VM exit reloads FS base from this field,
		 * so a stale value here does not merely confuse the guest — it
		 * overwrites the running task's TLS pointer every time the guest
		 * exits. The program then sets its TLS with arch_prctl, has it
		 * silently reverted by its own next exit, and dies reading
		 * %fs:0x0. SVM never shows this: VMSAVE/VMLOAD around VMRUN save
		 * and restore the host's FS/GS for free, and VMX has no
		 * equivalent — the field has to be kept current by hand.
		 */
		v = 0;
		rdmsrq_safe(MSR_IA32_SYSENTER_ESP, &v);
		vmcs_write(HOST_IA32_SYSENTER_ESP, v);
	}
	/*
	 * ...but the host FS base belongs to the *task*, not the CPU, so it is
	 * written on every entry however long this VMCS has been sitting here.
	 * Writing it only when the VMCS moved leaves an exit restoring the TLS
	 * pointer from whenever that was, and the guest dies reading %fs:0x0 in
	 * its own start-up.
	 */
	{
		u64 fs = 0;

		rdmsrq_safe(MSR_FS_BASE, &fs);
		if (!r->vm_cached || r->vm_host_fs != fs) {
			vmcs_write(HOST_FS_BASE, fs);
			r->vm_host_fs = fs;
		}
	}
	r->vm_cached = 1;

	local_irq_save(flags);
	/* The guest's page faults write host CR2. Nothing here reads it — the
	 * faulting address comes from the exit qualification — but the host's
	 * own value has to survive the guest. */
	asm volatile("mov %%cr2, %0" : "=r"(host_cr2));

	/* This CPU is about to hold guest TLB entries; see the SVM path and
	 * vmctx_kick_guests(). VPID is disabled, so VM entry/exit flush the
	 * guest linear mappings -- a forced VMEXIT reloads the TLB, no INVVPID. */
	atomic_inc(&vmctx_guests_running);
	asm volatile(
		"push %%rbp \n\t"
		"push %%rax \n\t"		/* ctx; HOST_RSP points at it  */
		"mov $" __stringify(VMX_HOST_RSP_ENC) ", %%rdx \n\t"
		"vmwrite %%rsp, %%rdx \n\t"
		"lea 1f(%%rip), %%rcx \n\t"
		"mov $" __stringify(VMX_HOST_RIP_ENC) ", %%rdx \n\t"
		"vmwrite %%rcx, %%rdx \n\t"
		/* guest GPRs in, RAX last because it is the pointer */
		"mov 0x08(%%rax), %%rbx \n\t"
		"mov 0x10(%%rax), %%rcx \n\t"
		"mov 0x18(%%rax), %%rdx \n\t"
		"mov 0x20(%%rax), %%rsi \n\t"
		"mov 0x28(%%rax), %%rdi \n\t"
		"mov 0x30(%%rax), %%rbp \n\t"
		"mov 0x38(%%rax), %%r8  \n\t"
		"mov 0x40(%%rax), %%r9  \n\t"
		"mov 0x48(%%rax), %%r10 \n\t"
		"mov 0x50(%%rax), %%r11 \n\t"
		"mov 0x58(%%rax), %%r12 \n\t"
		"mov 0x60(%%rax), %%r13 \n\t"
		"mov 0x68(%%rax), %%r14 \n\t"
		"mov 0x70(%%rax), %%r15 \n\t"
		"cmpb $0, 0x80(%%rax) \n\t"
		"mov 0x00(%%rax), %%rax \n\t"
		"jne 3f \n\t"
		"vmlaunch \n\t"
		"jmp 4f \n\t"
		"3: vmresume \n\t"
		"4: \n\t"
		/* VM entry failed: never left the host, stack untouched. */
		"mov (%%rsp), %%rax \n\t"
		"movq $1, 0x78(%%rax) \n\t"
		"jmp 2f \n\t"
		"1: \n\t"			/* VM exit resumes here        */
		"push %%rax \n\t"		/* guest RAX                   */
		"mov 0x08(%%rsp), %%rax \n\t"	/* ctx                         */
		"mov %%rbx, 0x08(%%rax) \n\t"
		"mov %%rcx, 0x10(%%rax) \n\t"
		"mov %%rdx, 0x18(%%rax) \n\t"
		"mov %%rsi, 0x20(%%rax) \n\t"
		"mov %%rdi, 0x28(%%rax) \n\t"
		"mov %%rbp, 0x30(%%rax) \n\t"
		"mov %%r8,  0x38(%%rax) \n\t"
		"mov %%r9,  0x40(%%rax) \n\t"
		"mov %%r10, 0x48(%%rax) \n\t"
		"mov %%r11, 0x50(%%rax) \n\t"
		"mov %%r12, 0x58(%%rax) \n\t"
		"mov %%r13, 0x60(%%rax) \n\t"
		"mov %%r14, 0x68(%%rax) \n\t"
		"mov %%r15, 0x70(%%rax) \n\t"
		"pop %%rbx \n\t"		/* guest RAX off the stack     */
		"mov %%rbx, 0x00(%%rax) \n\t"
		"movq $0, 0x78(%%rax) \n\t"
		"2: \n\t"
		"pop %%rax \n\t"		/* discard ctx                 */
		"pop %%rbp \n\t"
		: "+a"(ctx)
		:
		: "rbx", "rcx", "rdx", "rsi", "rdi",
		  "r8", "r9", "r10", "r11", "r12", "r13", "r14", "r15",
		  "memory", "cc");
	atomic_dec(&vmctx_guests_running);

	asm volatile("mov %0, %%cr2" :: "r"(host_cr2));
	local_irq_restore(flags);

	if (r->gctx.fail) {
		u64 err = vmcs_read(VM_INSTRUCTION_ERROR);

		pr_warn_ratelimited(VMCTX_PR "vmx: VM entry failed, error %llu "
				    "(launched %llu)\n", err, r->gctx.launched);
		vmx_release(r);
		ret = -EIO;
		goto out;
	}

	/* Read everything the exit handler needs while the VMCS is still ours. */
	/*
	 * Read the reason, then only the fields that reason gives meaning to.
	 * All four were read unconditionally, and three of them are noise for
	 * the exit that happens most — an external interrupt, which carries no
	 * information at all.
	 */
	r->exit_reason = (u32)vmcs_read(VM_EXIT_REASON);
	r->intr_info = r->intr_error = r->instr_len = 0;
	r->exit_qual = 0;
	switch (r->exit_reason & 0xffff) {
	case EXIT_REASON_EXCEPTION_NMI:
		r->intr_info = (u32)vmcs_read(VM_EXIT_INTR_INFO);
		if (r->intr_info & (1u << 11))	/* error code valid */
			r->intr_error = (u32)vmcs_read(VM_EXIT_INTR_ERROR_CODE);
		if ((r->intr_info & 0xff) == 14)	/* #PF: faulting address */
			r->exit_qual = vmcs_read(EXIT_QUALIFICATION);
		/*
		 * How long the instruction was, for the exceptions where that
		 * is the only way past it.
		 *
		 * A trap is reported standing *on* the instruction here, and
		 * stepping over it needs its length -- which VMX supplies for
		 * software exceptions (type 6: INT3, INTO) and software
		 * interrupts (type 4), and for nothing else. It was read only
		 * for CPUID, so the field was zero for every exception and
		 * "advance past the trap" advanced by nothing: the guest
		 * resumed on its own int3 and executed it again for ever.
		 * tests/tr1 passes its ud2 case and then hangs on int3, which
		 * is the same shape the SVM path had before it learned to use
		 * next_rip.
		 */
		{
			u32 type = (r->intr_info >> 8) & 7;

			if (type == 6 || type == 4)
				r->instr_len = (u32)vmcs_read(VM_EXIT_INSTRUCTION_LEN);
		}
		break;
	case EXIT_REASON_CPUID:
		r->instr_len = (u32)vmcs_read(VM_EXIT_INSTRUCTION_LEN);
		break;
	case EXIT_REASON_EXTERNAL_INTERRUPT:
		break;
	default:
		r->exit_qual = vmcs_read(EXIT_QUALIFICATION);
		break;
	}
	r->rip         = vmcs_read(GUEST_RIP);
	r->rsp         = vmcs_read(GUEST_RSP);
	r->rflags      = vmcs_read(GUEST_RFLAGS);
	/*
	 * The guest's own FS/GS bases, read HERE, while the VMCS is still
	 * current on this CPU. The caller used to read them after this
	 * function returned -- after preempt_enable() -- and a preemption in
	 * that window can migrate the task: vmcs_read() then answers for
	 * whatever VMCS the new CPU holds (a sibling context's, or none, which
	 * VMfails and reads as 0), and the wrong base is written into
	 * vc->guest_fsbase and fed back to the guest on its next entry -- a
	 * thread silently running on another thread's TLS. The struct's own
	 * comment says why every VMCS field is captured before release; these
	 * two were the exception. The write-cache is refreshed with the same
	 * values so it stays a true record of the VMCS contents.
	 */
	r->fs_base = vmcs_read(GUEST_FS_BASE);
	r->gs_base = vmcs_read(GUEST_GS_BASE);
	r->vm_fs = r->fs_base;
	r->vm_gs = r->gs_base;
	if ((r->exit_reason & 0xffff) == EXIT_REASON_EPT_VIOLATION ||
	    (r->exit_reason & 0xffff) == EXIT_REASON_EPT_MISCONFIG)
		r->gpa = vmcs_read(GUEST_PHYSICAL_ADDRESS);
	r->gctx.launched = 1;
out:
	preempt_enable();
	return ret;
}

/* Publish guest registers into the task's pt_regs — see the SVM version. */
static void vmx_guest_to_ptregs(struct vmx_run *r, struct pt_regs *regs)
{
	regs->ip      = r->rip;
	regs->sp      = r->rsp;
	regs->ax      = r->gctx.rax;
	regs->flags   = r->rflags;
	regs->cs      = __USER_CS;
	regs->ss      = __USER_DS;
	regs->orig_ax = r->last_sysnr;
	regs->bx = r->gctx.rbx; regs->cx = r->gctx.rcx; regs->dx = r->gctx.rdx;
	regs->si = r->gctx.rsi; regs->di = r->gctx.rdi; regs->bp = r->gctx.rbp;
	regs->r8  = r->gctx.r8;  regs->r9  = r->gctx.r9;  regs->r10 = r->gctx.r10;
	regs->r11 = r->gctx.r11; regs->r12 = r->gctx.r12; regs->r13 = r->gctx.r13;
	regs->r14 = r->gctx.r14; regs->r15 = r->gctx.r15;
}

static void vmx_ptregs_to_guest(struct vmx_run *r, struct pt_regs *regs)
{
	r->rip    = regs->ip;
	r->rsp    = regs->sp;
	r->gctx.rax = regs->ax;
	r->rflags = regs->flags | X86_EFLAGS_FIXED | X86_EFLAGS_IF;
	r->gctx.rbx = regs->bx; r->gctx.rcx = regs->cx; r->gctx.rdx = regs->dx;
	r->gctx.rsi = regs->si; r->gctx.rdi = regs->di; r->gctx.rbp = regs->bp;
	r->gctx.r8  = regs->r8;  r->gctx.r9  = regs->r9;  r->gctx.r10 = regs->r10;
	r->gctx.r11 = regs->r11; r->gctx.r12 = regs->r12; r->gctx.r13 = regs->r13;
	r->gctx.r14 = regs->r14; r->gctx.r15 = regs->r15;
}

static void vmx_report_bad_fault(struct task_struct *t, struct vmx_run *r,
				 u64 addr, u64 err, int ret)
{
	char ring[24 * 5 + 1], rh[8 * 20 + 1], rh2[8 * 40 + 1];
	unsigned char insn[16];
	unsigned long stk[6];
	int i, n = 0, got;

	for (i = 0; i < (int)ARRAY_SIZE(r->sysring); i++) {
		int idx = (r->sysring_i + i) % ARRAY_SIZE(r->sysring);

		n += scnprintf(ring + n, sizeof(ring) - n, "%u ", r->sysring[idx]);
	}
	n = 0;
	for (i = 0; i < (int)ARRAY_SIZE(r->riphist); i++) {
		u32 idx = (r->riphist_i + i) % ARRAY_SIZE(r->riphist);

		n += scnprintf(rh + n, sizeof(rh) - n, "%llx ", r->riphist[idx]);
	}
	pr_warn(VMCTX_PR "vmx: unrecoverable guest #PF pid %d at 0x%llx err 0x%llx (%d)\n",
		task_pid_nr(t), addr, err, ret);
	pr_warn(VMCTX_PR "  rip=0x%llx rsp=0x%llx rax=0x%llx rflags=0x%llx\n",
		r->rip, r->rsp, r->gctx.rax, r->rflags);
	pr_warn(VMCTX_PR "  rdi=0x%llx rsi=0x%llx rdx=0x%llx rcx=0x%llx rbx=0x%llx rbp=0x%llx\n",
		r->gctx.rdi, r->gctx.rsi, r->gctx.rdx, r->gctx.rcx,
		r->gctx.rbx, r->gctx.rbp);
	pr_warn(VMCTX_PR "  r8=0x%llx r10=0x%llx r11=0x%llx fs_base=0x%llx gs_base=0x%llx cr3=0x%llx\n",
		r->gctx.r8, r->gctx.r10, r->gctx.r11, r->fs_base, r->gs_base,
		r->cr3);
	pr_warn(VMCTX_PR "  recent guest syscalls (oldest->newest): %s\n", ring);
	pr_warn(VMCTX_PR "  recent guest RIPs at exit (oldest->newest): %s\n", rh);
	n = 0;
	for (i = 0; i < (int)ARRAY_SIZE(r->rsphist); i++) {
		u32 idx = (r->riphist_i + i) % ARRAY_SIZE(r->rsphist);

		n += scnprintf(rh2 + n, sizeof(rh2) - n, "%llx/%llx ",
			       r->rsphist[idx], r->rbphist[idx]);
	}
	pr_warn(VMCTX_PR "  rsp/rbp at those exits: %s\n", rh2);

	got = access_process_vm(t, r->rip, insn, sizeof(insn), 0);
	if (got == sizeof(insn))
		pr_warn(VMCTX_PR "  insn @rip: %*ph\n", (int)sizeof(insn), insn);
	else
		pr_warn(VMCTX_PR "  insn @rip: unreadable (%d)\n", got);
	got = access_process_vm(t, r->rsp, stk, sizeof(stk), 0);
	if (got == sizeof(stk))
		pr_warn(VMCTX_PR "  [rsp]: %016lx %016lx %016lx %016lx %016lx %016lx\n",
			stk[0], stk[1], stk[2], stk[3], stk[4], stk[5]);
	else
		pr_warn(VMCTX_PR "  [rsp]: unreadable (%d)\n", got);
	/* And around rbp; see the SVM path for why the two are printed apart. */
	got = access_process_vm(t, r->gctx.rbp - 0xa0, stk, sizeof(stk), 0);
	if (got == sizeof(stk))
		pr_warn(VMCTX_PR "  [rbp-0xa0]: %016lx %016lx %016lx %016lx %016lx %016lx\n",
			stk[0], stk[1], stk[2], stk[3], stk[4], stk[5]);
	else
		pr_warn(VMCTX_PR "  [rbp-0xa0]: unreadable (%d)\n", got);
	/* And the saved-frame-pointer slot; see the SVM path. */
	got = access_process_vm(t, r->gctx.rbp, stk, sizeof(stk), 0);
	if (got == sizeof(stk))
		pr_warn(VMCTX_PR "  [rbp]: %016lx %016lx %016lx %016lx %016lx %016lx\n",
			stk[0], stk[1], stk[2], stk[3], stk[4], stk[5]);
	else
		pr_warn(VMCTX_PR "  [rbp]: unreadable (%d)\n", got);
}

static int vmx_run_usercode_once(struct task_struct *t)
{
	struct vmctx_task *vc = t->vmctx;
	struct vmx_run *r = vc->backend_priv;
	struct pt_regs *regs = task_pt_regs(t);
	u32 vector;
	int ret;

	vmx_sync_tls_bases(vc, r);
	r->cr3 = vmx_guest_cr3_of(t);

	ret = vmx_enter_once(r);
	if (ret)
		return ret;

	/*
	 * The SVM twin's exit save-back: the guest may move its own base.
	 * From the values vmx_enter_once() captured under preempt_disable,
	 * never a vmcs_read() here -- the VMCS may not be this CPU's any more.
	 */
	if (vc->fsgs_valid) {
		vc->guest_fsbase = r->fs_base;
		vc->guest_gsbase = r->gs_base;
	}

	vc->enters++;
	vc->exits++;
	r->riphist[r->riphist_i % ARRAY_SIZE(r->riphist)] = r->rip;
	r->rsphist[r->riphist_i % ARRAY_SIZE(r->rsphist)] = r->rsp;
	r->rbphist[r->riphist_i % ARRAY_SIZE(r->rbphist)] = r->gctx.rbp;
	r->riphist_i++;
	vc->last_exit_reason = r->exit_reason;

	switch (r->exit_reason & 0xffff) {
	case EXIT_REASON_EXTERNAL_INTERRUPT:
		/* The host's interrupt is still pending; it is taken as soon as
		 * interrupts come back on, which has already happened. */
		r->last_sysnr = -1;
		vmctx_note_spin(t, &r->spin, r->rip);
		return VMCTX_RUN_CONTINUE;

	case EXIT_REASON_CPUID: {
		/*
		 * Unconditional on VMX, so it has to be answered here. The
		 * dynamic loader asks before it resolves a single ifunc, which
		 * is to say before any real program gets going.
		 */
		u32 a, b, c, d;

		vmctx_cpuid((u32)r->gctx.rax, (u32)r->gctx.rcx, &a, &b, &c, &d);
		r->gctx.rax = a;
		r->gctx.rbx = b;
		r->gctx.rcx = c;
		r->gctx.rdx = d;
		r->rip += r->instr_len;
		r->last_sysnr = -1;
		return VMCTX_RUN_CONTINUE;
	}

	case EXIT_REASON_EXCEPTION_NMI:
		vector = r->intr_info & 0xff;

		if (vector == 6) {			/* #UD: guest SYSCALL */
			u64 nr = r->gctx.rax;

			/* The same check the SVM path makes; see
			 * vmctx_insn_is_syscall(). */
			if (!vmctx_insn_is_syscall(t, r->rip)) {
				int rc;

				/* Same road as the SVM arm; see the note there. */
				vmx_guest_to_ptregs(r, regs);
				rc = vmctx_redirect_fault(t, regs, 6, r->rip, 0);
				if (rc == 1) {
					vmx_ptregs_to_guest(r, regs);
					return VMCTX_RUN_CONTINUE;
				}
				if (rc < 0) {
					vc->exit_status = rc;
					return VMCTX_RUN_EXIT;
				}
				vmctx_report_ud(t, r->rip, r->rsp, nr);
				vc->exit_status = -EFAULT;
				return VMCTX_RUN_EXIT;
			}

			if (nr == GUEST_NR_EXIT || nr == GUEST_NR_EXIT_GROUP) {
				vc->exit_status = (int)r->gctx.rdi;
				/*
				 * How many syscalls this side actually serviced, said at
				 * the one moment the number is final.
				 *
				 * The monitor counts the events it received, and when the
				 * two disagree the difference says which side invented the
				 * extra one -- the open question about th9's duplicated
				 * line. The userspace print meant to carry this never
				 * appears: the context does not get to run after vmctx_run
				 * returns, because the monitor has already torn down
				 * around it.
				 */
				/* Which of the two, and where from; see the SVM arm. */
				pr_info(VMCTX_PR "pid %d: context ending via %s(%llu) status %d at rip 0x%llx, serviced %llu syscalls and %llu faults\n",
					task_pid_nr(t),
					nr == GUEST_NR_EXIT ? "exit" : "exit_group",
					(unsigned long long)nr, vc->exit_status,
					(unsigned long long)r->rip,
					vc->counter, vc->faults);
				return VMCTX_RUN_EXIT;
			}
			r->last_sysnr = (long)nr;
			vmctx_spin_reset(&r->spin);
			r->sysring[r->sysring_i] = (u16)nr;
			r->sysring_i = (r->sysring_i + 1) % ARRAY_SIZE(r->sysring);

			vmx_guest_to_ptregs(r, regs);
			regs->ip     += 2;	/* over the SYSCALL          */
			regs->cx      = regs->ip;
			regs->r11     = regs->flags;
			regs->orig_ax = nr;

			ret = vmctx_redirect_syscall(t, regs);
			if (ret < 0) {
				vc->exit_status = ret;
				vmx_ptregs_to_guest(r, regs);
				return VMCTX_RUN_EXIT;
			}
			if (ret == 0) {
				/*
				 * No monitor serviced the call. Fail closed -- the
				 * destination never runs a guest syscall through its
				 * own kernel. End the context, as the SVM arm does.
				 */
				vc->exit_status = -EIO;
				vmx_ptregs_to_guest(r, regs);
				return VMCTX_RUN_EXIT;
			}
			vmx_sync_tls_bases(vc, r);
			vmx_ptregs_to_guest(r, regs);
			vc->counter++;
			return VMCTX_RUN_CONTINUE;
		}

		if (vector == 14) {			/* #PF: demand paging */
			u64 addr = r->exit_qual;
			u64 err  = r->intr_error;
			int backed_here;

			r->last_sysnr = -1;
			vmctx_spin_reset(&r->spin);

			/*
			 * The same demand-backing the SVM path does, which this
			 * one simply never had. See vmctx_back_fault().
			 */
			backed_here = vmctx_back_fault(t, vc, addr);

			/* The frame carries the answer here too; see the SVM
			 * path's note on why it must be refreshed first. */
			vmx_guest_to_ptregs(r, regs);

			ret = vmctx_redirect_fault(t, regs, 14, addr, err);
			if (ret < 0) {
				vc->exit_status = ret;
				vmx_ptregs_to_guest(r, regs);
				return VMCTX_RUN_EXIT;
			}
			if (ret == 1) {
				vc->faults++;
				vmx_ptregs_to_guest(r, regs);
				return VMCTX_RUN_CONTINUE;
			}
			if (backed_here)
				vmctx_unback_fault(addr);
			/*
			 * The monitor declined this one, so letting this kernel
			 * answer it is deliberate. Say so, or the page-fault path
			 * reports the very same fault to the monitor again.
			 *
			 * And record what it puts there; see the SVM arm.
			 */
			ret = vmctx_declined_to_local(t, &vc->in_local_service,
						      addr, err, r->rip);
			if (ret == 0) {
				vc->faults++;
				return VMCTX_RUN_CONTINUE;
			}
			/*
			 * The second chance; the SVM arm says why (a racing
			 * stale vacate's punch between the decline and this
			 * service, repairable from the monitor's construction
			 * record once the absence is settled).
			 */
			if ((r->unrec_addr != (addr & PAGE_MASK) ||
			     r->unrec_n < 2) && vc->monitor) {
				int again;

				if (r->unrec_addr != (addr & PAGE_MASK)) {
					r->unrec_addr = addr & PAGE_MASK;
					r->unrec_n = 0;
				}
				r->unrec_n++;
				again = vmctx_redirect_fault(t, regs, 14, addr,
							     err);
				if (again == 1) {
					vc->faults++;
					pr_warn_ratelimited(VMCTX_PR "vmx: pid %d: fault 0x%llx failed the local service after a decline; the monitor REPAIRED it on the second report\n",
							    task_pid_nr(t),
							    (unsigned long long)addr);
					vmx_ptregs_to_guest(r, regs);
					return VMCTX_RUN_CONTINUE;
				}
				if (again < 0) {
					vc->exit_status = again;
					vmx_ptregs_to_guest(r, regs);
					return VMCTX_RUN_EXIT;
				}
			}
			vmx_report_bad_fault(t, r, addr, err, ret);
			vc->exit_status = -EFAULT;
			return VMCTX_RUN_EXIT;
		}

		if (((r->intr_info >> 8) & 7) != 2 && vector < 32) {
			int rc;

			/*
			 * Any exception but the NMI below, reported the same
			 * way. #UD and #PF are handled above because this
			 * machinery gives them a meaning of its own; the rest
			 * mean only what the program did. Same road as the SVM
			 * arm; see the note there.
			 */
			/*
			 * Past the instruction for a software trap; see the
			 * SVM arm's note. Type 6 in the interruption
			 * information is VMX's name for one.
			 */
			if (((r->intr_info >> 8) & 7) == 6)
				r->rip += r->instr_len;

			vmx_guest_to_ptregs(r, regs);
			rc = vmctx_redirect_fault(t, regs, vector, 0,
						  r->intr_error);
			if (rc == 1) {
				vmx_ptregs_to_guest(r, regs);
				return VMCTX_RUN_CONTINUE;
			}
			if (rc < 0) {
				vc->exit_status = rc;
				return VMCTX_RUN_EXIT;
			}
			if (vector == 13)
				vmctx_report_gp(t, r->rip, r->rsp, r->intr_error);
			else
				pr_warn(VMCTX_PR "vmx: guest exception %u at "
					"rip 0x%llx (error 0x%x), and its owner "
					"had no answer for it\n", vector,
					r->rip, r->intr_error);
			vc->exit_status = -EFAULT;
			return VMCTX_RUN_EXIT;
		}
		if (((r->intr_info >> 8) & 7) == 2) {
			/*
			 * An NMI, taken while the guest was running because we
			 * ask for NMI exiting — a guest must not swallow the
			 * host's NMIs, which is how the watchdog and the perf
			 * counters get their attention. Hand it to the host's
			 * own handler and carry on.
			 */
			asm volatile("int $2");
			r->last_sysnr = -1;
			return VMCTX_RUN_CONTINUE;
		}
		pr_warn(VMCTX_PR "vmx: guest exception vector %u (info 0x%x) at "
			"rip 0x%llx; ending\n", vector, r->intr_info, r->rip);
		vc->exit_status = -EFAULT;
		return VMCTX_RUN_EXIT;

	case EXIT_REASON_EPT_VIOLATION:
	case EXIT_REASON_EPT_MISCONFIG:
		/*
		 * The EPT identity-maps the low 512 GiB, so this means the
		 * guest reached a physical address outside it — not a guest
		 * bug, a mapping this backend does not cover.
		 */
		pr_warn(VMCTX_PR "vmx: EPT %s at gpa 0x%llx (qual 0x%llx) rip 0x%llx\n",
			(r->exit_reason & 0xffff) == EXIT_REASON_EPT_VIOLATION ?
				"violation" : "misconfiguration",
			r->gpa, r->exit_qual, r->rip);
		vc->exit_status = -EFAULT;
		return VMCTX_RUN_EXIT;

	default:
		pr_warn(VMCTX_PR "vmx: unexpected exit reason %u (qual 0x%llx) "
			"at rip 0x%llx; ending\n",
			r->exit_reason & 0xffff, r->exit_qual, r->rip);
		vc->exit_status = -(int)(r->exit_reason & 0xffff);
		return VMCTX_RUN_EXIT;
	}
}

static int vmx_be_run_usercode(struct task_struct *t)
{
	struct vmx_run *r = t->vmctx->backend_priv;
	struct pt_regs *regs = task_pt_regs(t);
	int ret;

	if (!r->frame_saved) {
		r->entry_frame = *regs;
		r->frame_saved = 1;
	}
	if (r->regs_live)
		vmx_ptregs_to_guest(r, regs);

	ret = vmx_run_usercode_once(t);

	if (!t->vmctx->needs_rebuild) {
		vmx_guest_to_ptregs(r, regs);
		r->regs_live = 1;
	}
	return ret;
}

static int vmx_be_create(struct task_struct *t)
{
	struct vmctx_task *vc = t->vmctx;
	struct vmx_run *r;
	int ret;

	if (g_caps.vendor != VMCTX_VENDOR_INTEL ||
	    !(g_caps.caps & VMCTX_CAP_ENABLED))
		return -ENODEV;
	if (!(vc->flags & VMCTX_FLAG_USERCODE))
		return -ENOTSUPP;	/* the 16-bit sandbox modes are SVM-only */

	r = kzalloc(sizeof(*r), GFP_KERNEL);
	if (!r)
		return -ENOMEM;
	ret = vmx_run_alloc(r);
	if (ret) {
		kfree(r);
		return ret;
	}
	vmx_build_ept_identity(r);

	/* Building the VMCS means writing to it, which means making it current. */
	preempt_disable();
	ret = vmx_cpu_root_enter();
	if (!ret && vmx_make_current(r) < 0)
		ret = -EIO;
	if (!ret)
		ret = vmx_build_vmcs(r, vc->entry, vc->stack);
	/*
	 * Built, and now given up again. The saving from keeping a VMCS loaded
	 * is on the thousands of entries that follow, not on the first — and
	 * holding it here would carry it across into a different task, and
	 * possibly a different CPU, with the host state in it describing
	 * neither. The first entry reloads and writes that state; every entry
	 * after it resumes.
	 */
	vmx_release(r);
	preempt_enable();
	if (ret) {
		vmx_run_free(r);
		kfree(r);
		return ret;
	}
	r->last_sysnr = -1;

	if (vc->flags & VMCTX_FLAG_RESTORE) {
		r->entry_frame = *task_pt_regs(current);
		r->frame_saved = 1;
		r->regs_live   = 1;
	}
	vc->backend_priv = r;
	pr_info(VMCTX_PR "vmx: pid %d guest entry 0x%llx stack 0x%llx cr3 0x%llx "
		"(kernel pgd 0x%llx%s) eptp 0x%llx\n",
		task_pid_nr(t), vc->entry, vc->stack, r->cr3,
		t->mm ? (u64)virt_to_phys(t->mm->pgd) : 0ULL,
		boot_cpu_has(X86_FEATURE_PTI) ? ", pti" : "",
		virt_to_phys(r->ept[0]));
	return 0;
}

static int vmx_be_run(struct task_struct *t)
{
	if (!(t->vmctx->flags & VMCTX_FLAG_USERCODE))
		return -ENOTSUPP;
	return vmx_be_run_usercode(t);
}

static void vmx_be_destroy(struct task_struct *t)
{
	struct vmx_run *r = t->vmctx->backend_priv;

	if (r && r->frame_saved && r->regs_live && !t->vmctx->exit_process) {
		struct pt_regs *regs = task_pt_regs(t);
		unsigned long ax = regs->ax;

		*regs = r->entry_frame;
		regs->ax = ax;
	}
	if (r) {
		vmx_run_free(r);
		kfree(r);
		t->vmctx->backend_priv = NULL;
	}
}

/* ---- guest-ASID TLB shootdown (backend->invalidate) -------------------- */

/*
 * How many CPUs are inside a guest right now, each holding translations in its
 * own ASID-tagged TLB that a host TLB flush does not reach. Incremented around
 * the guest entry in both backends. A hand-over is not the fast path, so an
 * approximate read only ever costs one extra, harmless broadcast IPI.
 * (vmctx_guests_running is defined up beside the SVM entry that maintains it.)
 */
static unsigned long vmctx_kick_calls;   /* invalidate() reached            */
static unsigned long vmctx_kick_ipis;    /* ...with a guest actually running */
static unsigned long vmctx_kick_maxrun;  /* most guests seen running at once */
/*
 * The guest-ASID TLB shootdown that makes a writable->read-only downgrade tight:
 * after PROTECTOBJ/TAKEOBJ write-protect (or unmap) the folio in every mapping,
 * a sibling guest still running under VMRUN may hold the page WRITABLE in its own
 * ASID-tagged TLB and keep writing it, unfaulted, until its next VMEXIT -- a lost
 * write the host-TLB flush cannot reach. vmctx_kick_guests() (backend->invalidate)
 * is the fix and is called from every protect/take. UNCONDITIONAL: it is the
 * only mechanism that closes the window for two genuinely-parallel writers, and
 * it is free when no guest is running at the protect -- vmctx_kick_guests()
 * reads vmctx_guests_running and IPIs only when it is non-zero, so kick_ipis
 * stays 0 on workloads where the guest is stopped in the monitor during every
 * hand-off (hx1/hx2 loopback, and netsurf: measured maxrun=0). Its off-arm was
 * A/B'd twice and measured identical on one boot (20/24 vs 20/24; 3 vs 3
 * repeats per 2048) -- the switch is gone; the kick stays as ordering
 * correctness, not tuning.
 */
module_param_named(kick_calls, vmctx_kick_calls, ulong, 0444);
module_param_named(kick_ipis, vmctx_kick_ipis, ulong, 0444);
module_param_named(kick_maxrun, vmctx_kick_maxrun, ulong, 0444);

#ifdef VMCTX_BACKEND_HAS_INVALIDATE
static void vmctx_kick_noop(void *info) { }
#endif

/*
 * backend->invalidate: retire every guest translation older than this call.
 *
 * Two halves, in this order:
 *
 *   1. Bump vmctx_tlb_gen. Every guest ENTRY after this instant flushes the
 *      guest ASID before the guest runs an instruction (see the generation
 *      check in svm_vmrun_once_gpr). This is what covers the guest that is
 *      NOT running right now -- parked in its monitor across the take, the
 *      pg3/netsurf/ws1 shape -- whose stale translations survive in the
 *      physical TLB and which no IPI can reach. The old code had nothing
 *      for it: this comment used to claim "SVM tlb_ctl=1 flushes the ASID
 *      on every VMRUN", but TLB_CONTROL is a one-shot command that was
 *      written once at VMCB build and never re-armed.
 *
 *   2. IPI every CPU, waited, if any guest is inside VMRUN right now. A
 *      pending physical interrupt makes the guest #VMEXIT even with the
 *      guest's own IF clear (SVM INTERCEPT_INTR / VMX external-interrupt
 *      exiting). Broadcast, not targeted: the shared object is mapped by
 *      many single-mm contexts on unknown CPUs, and an IPI to a CPU that is
 *      not in a guest is a no-op. Waited on (wait=1) so that on return every
 *      guest that was running has taken at least one VMEXIT since the
 *      caller's unmap; its stores through the old translation landed on the
 *      still-held folio BEFORE this returns, i.e. before the caller's copy.
 *
 * The order is the correctness: the bump is a full barrier before the read
 * of vmctx_guests_running, pairing with the entry side's inc-then-read, so
 * a guest entering concurrently either gets the IPI or sees the new
 * generation. After this returns, no CPU can use a pre-call translation.
 */
#ifdef VMCTX_BACKEND_HAS_INVALIDATE
static void vmctx_kick_guests(struct task_struct *t, unsigned long addr)
{
	int n;

	vmctx_kick_calls++;
	atomic64_inc(&vmctx_tlb_gen);
	vmctx_gen_bumps++;
	/* atomic64_inc is a full barrier on x86; said explicitly so the
	 * bump-then-read pairing with the entry path is in the code, not
	 * only in the comment. */
	smp_mb__after_atomic();
	n = atomic_read(&vmctx_guests_running);
	if (n <= 0)
		return;
	if ((unsigned long)n > vmctx_kick_maxrun)
		vmctx_kick_maxrun = n;
	vmctx_kick_ipis++;
	smp_call_function(vmctx_kick_noop, NULL, 1);
}
#endif

/*
 * No .clone: the destination never builds a clone child. The guest's clone is
 * forwarded to the source, whose kernel makes the child; this machine only
 * stands up the execution context for it, through the ordinary create path.
 */
static struct vmctx_backend vmctx_vmx_backend = {
	.owner      = THIS_MODULE,
	.create     = vmx_be_create,
	.run        = vmx_be_run,
	.destroy    = vmx_be_destroy,
#ifdef VMCTX_BACKEND_HAS_INVALIDATE
	/*
	 * Only where the core kernel has the hook. 6.18.35 -- the Intel netboot
	 * target's kernel, held there deliberately -- does not, and without the
	 * guard this file does not compile against it at all ("struct
	 * vmctx_backend has no member named invalidate"), which is how the whole
	 * Intel half of the LAN stopped being buildable. It is not an optional
	 * behaviour and there is no switch for it: where the kernel can do the
	 * guest-ASID shootdown it always does, and where it cannot the module
	 * says so at load time rather than leaving it to be inferred.
	 */
	.invalidate = vmctx_kick_guests,
#endif
};

static void vmx_free_vmxon_regions(void)
{
	int cpu;

	for_each_possible_cpu(cpu) {
		if (vmx_vmxon_region[cpu]) {
			free_page((unsigned long)vmx_vmxon_region[cpu]);
			vmx_vmxon_region[cpu] = NULL;
		}
	}
}

static int vmx_alloc_vmxon_regions(void)
{
	int cpu;

	for_each_possible_cpu(cpu) {
		vmx_vmxon_region[cpu] = (void *)get_zeroed_page(GFP_KERNEL);
		if (!vmx_vmxon_region[cpu]) {
			vmx_free_vmxon_regions();
			return -ENOMEM;
		}
	}
	return 0;
}

static struct vmctx_backend vmctx_svm_backend = {
	.owner      = THIS_MODULE,
	.create     = vmctx_be_create,
	.run        = vmctx_be_run,
	.destroy    = vmctx_be_destroy,
#ifdef VMCTX_BACKEND_HAS_INVALIDATE
	/*
	 * Only where the core kernel has the hook. 6.18.35 -- the Intel netboot
	 * target's kernel, held there deliberately -- does not, and without the
	 * guard this file does not compile against it at all ("struct
	 * vmctx_backend has no member named invalidate"), which is how the whole
	 * Intel half of the LAN stopped being buildable. It is not an optional
	 * behaviour and there is no switch for it: where the kernel can do the
	 * guest-ASID shootdown it always does, and where it cannot the module
	 * says so at load time rather than leaving it to be inferred.
	 */
	.invalidate = vmctx_kick_guests,
#endif
};

/* ---- context lifecycle ------------------------------------------------ */

static struct vmctx *ctx_create(u32 flags, pid_t owner)
{
	struct vmctx *c;
	int id;

	c = kzalloc(sizeof(*c), GFP_KERNEL);
	if (!c)
		return ERR_PTR(-ENOMEM);

	c->owner_pid = owner;
	c->state = VMCTX_STATE_CREATED;
	c->flags = g_caps.caps & (VMCTX_CAP_PRESENT | VMCTX_CAP_ENABLED |
				  VMCTX_CAP_EPT_NPT | VMCTX_CAP_UNRESTRICTED);
	c->last_exit_reason = ~0ULL;

	id = idr_alloc(&g_idr, c, 0, 0, GFP_KERNEL);
	if (id < 0) {
		kfree(c);
		return ERR_PTR(id);
	}
	c->id = id;
	list_add_tail(&c->link, &g_contexts);
	g_nr_contexts++;
	return c;
}

static void ctx_destroy(struct vmctx *c)
{
	list_del(&c->link);
	idr_remove(&g_idr, c->id);
	if (c->vmcs)
		free_page((unsigned long)c->vmcs);
	g_nr_contexts--;
	kfree(c);
}

/* ---- ioctl ------------------------------------------------------------ */

static long vmctx_ioctl_info(void __user *arg)
{
	struct vmctx_info info;

	memset(&info, 0, sizeof(info));
	info.abi_version = VMCTX_ABI_VERSION;
	info.vendor      = g_caps.vendor;
	info.caps        = g_caps.caps;
	info.nr_contexts = g_nr_contexts;
	info.vmx_basic   = g_caps.vmx_basic;
	info.feat_ctl    = g_caps.feat_ctl;
	info.vm_cr       = g_caps.vm_cr;
	info.efer        = g_caps.efer;
	memcpy(info.vendor_str, g_caps.vendor_str, sizeof(info.vendor_str));

	return copy_to_user(arg, &info, sizeof(info)) ? -EFAULT : 0;
}

static long vmctx_ioctl_create(void __user *arg)
{
	struct vmctx_create req;
	struct vmctx *c;
	pid_t owner;
	int ret = 0;

	if (copy_from_user(&req, arg, sizeof(req)))
		return -EFAULT;

	owner = req.target_pid ? req.target_pid : task_pid_nr(current);

	c = ctx_create(req.flags, owner);
	if (IS_ERR(c))
		return PTR_ERR(c);

	if (req.flags & VMCTX_CREATE_FLAG_BRINGUP) {
		ret = ctx_bringup(c);
		if (ret) {
			pr_warn(VMCTX_PR "bringup failed for ctx %d: %d\n",
				c->id, ret);
			/* keep the context so dumpvm can show the failure */
		}
	}

	req.id = c->id;
	if (copy_to_user(arg, &req, sizeof(req)))
		return -EFAULT;
	return ret;   /* 0, or bring-up errno (context still exists) */
}

static long vmctx_ioctl_dump(void __user *arg)
{
	struct vmctx_dump d;
	struct vmctx *c;

	if (copy_from_user(&d, arg, sizeof(d)))
		return -EFAULT;

	c = ctx_find(d.id);
	if (!c)
		return -ENOENT;

	memset(&d, 0, sizeof(d));
	d.id               = c->id;
	d.state            = c->state;
	d.owner_pid        = c->owner_pid;
	d.vendor           = g_caps.vendor;
	d.vmcs_pa          = c->vmcs_pa;
	d.vmcs_rev         = c->vmcs_rev;
	d.last_exit_reason = c->last_exit_reason;
	d.last_exit_qual   = c->last_exit_qual;
	d.host_cr0         = c->host_cr0;
	d.host_cr3         = c->host_cr3;
	d.host_cr4         = c->host_cr4;
	d.host_efer        = c->host_efer;
	d.vmx_basic        = g_caps.vmx_basic;
	d.feat_ctl         = g_caps.feat_ctl;
	d.vm_cr            = g_caps.vm_cr;
	d.flags            = c->flags;

	return copy_to_user(arg, &d, sizeof(d)) ? -EFAULT : 0;
}

static long vmctx_ioctl_destroy(void __user *arg)
{
	struct vmctx *c;
	u32 id;

	if (copy_from_user(&id, arg, sizeof(id)))
		return -EFAULT;
	c = ctx_find(id);
	if (!c)
		return -ENOENT;
	ctx_destroy(c);
	return 0;
}

static long vmctx_ioctl_selftest(void __user *arg)
{
	struct vmctx_selftest st;
	int ret;

	memset(&st, 0, sizeof(st));
	ret = svm_selftest(&st);
	if (ret && ret != -1)
		st.result = ret;
	if (copy_to_user(arg, &st, sizeof(st)))
		return -EFAULT;
	return 0;   /* the result field carries pass/fail */
}

static long vmctx_ioctl(struct file *f, unsigned int cmd, unsigned long arg)
{
	void __user *uarg = (void __user *)arg;
	long ret;

	mutex_lock(&g_lock);
	switch (cmd) {
	case VMCTX_IOC_INFO:     ret = vmctx_ioctl_info(uarg);     break;
	case VMCTX_IOC_CREATE:   ret = vmctx_ioctl_create(uarg);   break;
	case VMCTX_IOC_DUMP:     ret = vmctx_ioctl_dump(uarg);     break;
	case VMCTX_IOC_DESTROY:  ret = vmctx_ioctl_destroy(uarg);  break;
	case VMCTX_IOC_SELFTEST: ret = vmctx_ioctl_selftest(uarg); break;
	default:                 ret = -ENOTTY;                    break;
	}
	mutex_unlock(&g_lock);
	return ret;
}

static const struct file_operations vmctx_fops = {
	.owner          = THIS_MODULE,
	.unlocked_ioctl = vmctx_ioctl,
	.compat_ioctl   = vmctx_ioctl,
};

static struct miscdevice vmctx_misc = {
	.minor = MISC_DYNAMIC_MINOR,
	.name  = VMCTX_DEVICE,
	.fops  = &vmctx_fops,
	.mode  = 0600,
};

static int __init vmctx_init(void)
{
	int ret;

	detect_caps();

	/*
	 * Said at load, because it is a difference between the two kernels this
	 * module is built against and not a setting anyone chose. Where the core
	 * has backend->invalidate the guest-ASID shootdown runs on every take and
	 * write-protect; where it does not, a guest can hold a stale translation
	 * in its own ASID-tagged TLB after a host-side unmap, and that is the
	 * coh_lock lost write. A run on such a kernel is still a run -- it is just
	 * not the same machine, and the log has to say which one it was.
	 */
#ifdef VMCTX_BACKEND_HAS_INVALIDATE
	pr_info(VMCTX_PR "guest-ASID shootdown: present (backend->invalidate), "
		"tlb_ctl re-armed at entry on generation change\n");
#else
	pr_warn(VMCTX_PR "guest-ASID shootdown: ABSENT -- this kernel has no "
		"backend->invalidate, so a take cannot reach a guest TLB; "
		"expect the coh_lock lost write on shared pages\n");
#endif

	ret = misc_register(&vmctx_misc);
	if (ret) {
		pr_err(VMCTX_PR "misc_register failed: %d\n", ret);
		return ret;
	}
	/* Register as the native VM-context backend so vmctx_run(2) works. */
	if (g_caps.vendor == VMCTX_VENDOR_AMD &&
	    (g_caps.caps & VMCTX_CAP_ENABLED)) {
		ret = vmctx_register_backend(&vmctx_svm_backend);
		if (ret)
			pr_warn(VMCTX_PR "backend registration failed: %d\n", ret);
		else
			g_caps.caps |= VMCTX_CAP_BACKEND;
	} else if (g_caps.vendor == VMCTX_VENDOR_INTEL &&
		   (g_caps.caps & VMCTX_CAP_ENABLED)) {
		ret = vmx_alloc_vmxon_regions();
		if (ret) {
			pr_warn(VMCTX_PR "vmx: no memory for VMXON regions: %d\n",
				ret);
		} else {
			ret = vmctx_register_backend(&vmctx_vmx_backend);
			if (ret) {
				pr_warn(VMCTX_PR "vmx: backend registration "
					"failed: %d\n", ret);
				vmx_free_vmxon_regions();
			} else {
				g_caps.caps |= VMCTX_CAP_BACKEND;
			}
		}
	} else {
		pr_info(VMCTX_PR "no usable backend (vendor/caps); vmctx_run unavailable\n");
	}

	pr_info(VMCTX_PR "loaded, /dev/%s ready (abi %d)\n",
		VMCTX_DEVICE, VMCTX_ABI_VERSION);
	return 0;
}

static void __exit vmctx_exit(void)
{
	struct vmctx *c, *tmp;

	vmctx_unregister_backend(&vmctx_svm_backend);
	if (g_caps.vendor == VMCTX_VENDOR_INTEL) {
		vmctx_unregister_backend(&vmctx_vmx_backend);
		/* Leave VMX root on every CPU that entered it, then give the
		 * VMXON regions back. In that order: the region is in use until
		 * VMXOFF has run on the CPU using it. */
		on_each_cpu(vmx_cpu_root_leave, NULL, 1);
		vmx_free_vmxon_regions();
	}

	mutex_lock(&g_lock);
	list_for_each_entry_safe(c, tmp, &g_contexts, link)
		ctx_destroy(c);
	mutex_unlock(&g_lock);

	misc_deregister(&vmctx_misc);
	idr_destroy(&g_idr);
	pr_info(VMCTX_PR "unloaded\n");
}

module_init(vmctx_init);
module_exit(vmctx_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("avm.md project");
MODULE_DESCRIPTION("Native VM context for Linux (M1a bring-up + M1b guest entry)");
MODULE_VERSION("0.2");
