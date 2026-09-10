# Kernel review: hypotheses and source-level defects

This is a working record, not a completed audit or attribution of browser
failures. AMD core/header and the common hardware module have received a first
full read. Intel core and headers have now also received a full first read,
including the complete files containing the project hooks. Comments
describing a previous experiment are not proof that current code is correct.

## AMD core

- `vmctx_run_current`: the service path does not acquire a backend, but its
  allocation-failure branch dereferenced `b->owner`. Both source trees now
  guard the module-reference release. A disposable QEMU run of the Intel
  kernel reproduced the NULL dereference at the first injected allocation;
  the repaired kernel returns ENOMEM in all five attempts, keeps mm_live=0,
  and then successfully attaches and retires a service context. AMD QEMU
  and physical-host validation of this repair are still pending.
- `vmctx_mm_forget`: makes a static slot available under `vmctx_mm_lock`, then
  destroys its XArray after unlocking. Slot reuse may initialize a new array
  before the old destroy. A held mm reference does not itself pin membership
  in the context registry. Review retirement, lookup and mm adoption together.
- `vmctx_mm_note`: registry exhaustion at 256 address spaces warns and continues.
  Memory fault guards depend on membership; failure must not disable coherence.
  Registry entries hold raw mm pointers, without an mm lifetime reference.
  A failed fork now demonstrably leaves a record past mm destruction. Native
  build processes later reuse it: kernel third-party refusal messages and
  straced E2BIG exec failures identify the same native task ids. The abandoned
  record can affect ordinary host execution, not just later vmctx guests.
- `vmctx_pgrec_set`: callers cannot see allocation errors. Claims sometimes
  report normally after the claim cannot be recorded. `PGACK` retries any CAS
  failure, not just a changed value. Allocation failure needs an explicit result.
- `vmctx_ctl_serve`: an old TRANSIT becomes REMOTE without an acknowledgment;
  HOME without a mapped page becomes REMOTE after enough requests. Neither
  elapsed time nor an ask count establishes where the current bytes are.
- `vmctx_ctl_land`: IF_ABSENT checks presence, drops the mmap lock, then writes
  via `access_process_vm`; another landing may intervene. It also pins an mm
  for the record but writes by task, allowing an exec to change the target mm.
- `vmctx_ctl_take` / `vmctx_take_locked`: a refused claim after zap explicitly
  loses the page. A failing copy to the monitor can also consume the only copy.
  Prechecks shrink a window but do not make destructive failure atomic.
- `vmctx_apply_map_rep` returns void; APPLYMAP reports success after failures.
  A bad backing descriptor can select anonymous backing. fd 0 is silently
  excluded. Mapping ranges and overflow handling need an ABI validation pass.
- `vmctx_ctl_impl`: ATTACH, RESUME and SYSCALL publish state without one shared
  transaction lock. WAIT does not exclude an event already marked `ev_taken`.
  Need concurrency tests for duplicate waiters, concurrent assisted calls,
  detach, monitor death and stale replies.
- `vmctx_report`: monitor loss enters predicates that already consider the
  wait complete; reconcile this with the successor-monitor comment. Fatal
  interruptions of killable timeout waits need actual elapsed-time accounting.
- `vmctx_ctl_takeobj`: removal occurs before the diagnostic re-copy. A failed
  later copy can return an error after destroying the object's page. The
  diagnostic's word scan can finish at PAGE_SIZE/8 after the earlier memcmp
  found a difference that changed again; its print then reads past the arrays.
- `vmctx_syscall_holds_one`: a CPU-time limit explicitly releases ownership
  protection while a call can still write. Review asynchronous kernel references
  separately from whether a task is currently sleeping.
- mmu notifier registration failure only warns despite unmap propagation being
  required. The TLB notifier is off by default and can skip atomic contexts.
  Flushing on the next guest entry does not stop an already running guest
  from using a translation invalidated by an unrelated memory-management path.

## Hardware module

- `vmx_make_current`: replacing the current VMCS marks the old one unowned
  without VMCLEAR on its owning CPU. Migration/destruction can then operate
  on an active VMCS without retiring it on that CPU. `cur` is also sampled
  before a waited remote IPI that can allow another CPU's clear request here.
- `vmx_release`: uses a local-CPU comparison without disabling preemption in
  every caller. Teardown can migrate between the comparison and local clear.
  CPU hotplug has no VMX lifecycle handler.
- `bringup_amd` overwrites VM_HSAVE_PA but does not restore it before freeing
  the temporary save page. The actual VMRUN paths do preserve that MSR.
- SVM and VMX identity maps assume low 512 GiB and 1 GiB leaf support. Feature
  reporting is not sufficient validation of each required paging capability.
  VMCS writes mostly discard their error return.
- SVM and VMX exclude #NM/#MC from exception interception despite having no
  guest IDT. Review against hardware rules and extended-state support.
- `trace_zero` and the served-page watch use faulting reads to observe guest
  memory. Observation can instantiate the missing page. Several zero-page
  diagnoses compare a checksum rather than testing all bytes; they are not
  proof of an all-zero page or proof that nobody legitimately created zeros.
- The SVM second-report register reply defect is fixed in the local module
  change and the isolated AMD module `9ABB054AF78D5E82734AA43`. The original
  module failed the second-report control while passing the first-report
  control; the fixed module passed ten repetitions of each, plus the 40-case
  hardware suite. See the dated audit record for the deployment evidence.

All fixes require focused controls and both relevant hardware regressions.

## Header and patch coverage

Both complete `sched.h`, `syscalls.h` and `mm.h` files are reviewed, using
the complete interversion diffs after the AMD reads. The AMD `VM_COPY_ON_FORK`
definition explicitly includes guard and userfaultfd write-protection metadata:
VMA ranges and backing bytes cannot reconstruct these page-table properties.
Intel lacks the AMD `VM_MAYBE_GUARD` flag; a cross-kernel representation must
describe the semantics rather than copy internal flags or structures blindly.
Folio mapcounts and expected reference counts are also documented as unstable
on a mapped folio. A sampled count cannot authorize a destructive transfer
without excluding new mappings and accesses through the rest of the operation.

The two shipped 0001 patches have also been covered completely. An audit helper
checked each postimage/context line against its recorded source SHA-256 and
committed source revision; 17,819 lines exactly match already reviewed source.
All 455 remaining lines, including metadata, deletions and a config excerpt,
were read directly. Evidence is `logs/audit-20260906/patch-coverage.json` and
its helper/remaining-line files. These are coverage checks, not runtime proof.

The complete NOTES history contains several diagnoses later withdrawn and
experiments that changed the scheduling or memory they observed. In particular,
zero post-punch re-read differences do not exclude accesses after the final
sample or through an unobserved mapping, and a checksum/capture counter does
not identify a mapping incarnation. The historical observation that a context
is parked also leaves sibling and asynchronous kernel writers to account for.
The measured signal, fork-state, mprotect-gap and guard-page failures remain
independent architectural obligations; none is yet attributed as the cause of
the current Firefox failure.

## Fork, exec and exit hooks

Both versions' complete fork/exec/exit paths have now been reviewed (Intel
through the full interversion diffs after reading AMD). A failed fork after
vmctx_copy_task never reached vmctx_task_exit. The isolated service control
reproduced one leaked mm record after an EFAULT pidfd output; both working
trees now release vmctx in the fork unwind path. This also routes failure of
vmctx_copy_task through exit_thread after successful copy_thread. The Intel
fork object compiles. AMD's isolated full kernel build and one-time test boot
pass; the unchanged failed-fork control passes five times with mm_live
0/1/1/0, versus the old kernel's leaking 28/29/30/29. Intel booted validation
remains pending.

Intel calls vmctx_exec_notify in begin_new_exec, before the ELF loader has
completed the new image. AMD moved it to successful exec completion so loader
faults in the nascent mm are not routed to the previous image. The Intel
placement must be reconciled with its mm-adoption guards before using it as a
source. Nonleader exec also exchanges task ids in de_thread; wire identities
based only on numeric task ids must not survive that exchange as though they
still name the same execution context.

Both do_exit hooks retire vmctx before synchronize_group_exit and before
io_uring_files_cancel. Page ownership cannot be retired on the assumption that
entering do_exit has already quiesced asynchronous kernel writes. The order
also makes vmctx teardown visible before PF_EXITING is set. These observations
reinforce the need for explicit lifecycle phases and retained kernel writer
references; no ordering repair for them has been validated yet.

## Intel core first read

- The service allocation failure also dereferences a NULL backend here.
  The allocation failure is now reproduced and repaired as recorded above.
  The mm registry has only 64 slots and continues after exhaustion. The same
  membership-dependent fault guards can therefore be disabled by scale.
- The restoring context's monitor wait needs to check `released` when its
  timeout expires, not merely monitor presence. An attached monitor is not
  evidence that it supplied the guest state.
- Clone setup can return success when a backend module reference cannot be
  acquired, leaving a child outside vmctx. A failed backend clone can leave an
  active child with NULL backend state. Failure cleanup also needs a monitor
  reference audit. Both paths require injected-failure controls.
- `vmctx_af_sys_hold` records nanoseconds but the non-retry fault path records
  jiffies in the same timestamp array. `vmctx_syscall_holds_one` interprets
  both as jiffies. A blocked task is also not proof that all kernel/device
  writers have released the page. These holds need an ownership contract,
  not just a unit correction or different timeout.
- `vmctx_page_present_in` walks page-table levels without mmap/RCU protection.
  Calling it diagnostic does not make concurrent page-table teardown safe.
- `vmctx_apply_map_rep` has the same false-success and descriptor issues as
  AMD. Remote PROT is explicitly refused, so existing sibling permissions can
  outlive the source's mprotect. The comment that mprotect changes nothing on
  an ENOMEM hole is false: both versions change preceding VMAs before
  returning the error. Native mprotect-gap controls pass, while the AMD guest
  wrongly completes a write to the first page (the source revoked its write
  permission). This now has a failing hardware regression.
- ADOPT publishes the new context before initializing active/noted-mm state;
  target exit or exec is not serialized by a task reference. GET/SETREGS also
  access a potentially running frame without an event transaction.
- WAIT can give the same event to multiple waiters. RESUME and assisted
  SYSCALL use unsynchronized shared fields; the 30-second syscall timeout can
  return while the syscall is still executing. A subsequent request can then
  reuse the fields for an operation whose completion has not been observed.
- PROTECTOBJ reports absence when the backing object contains an unmapped
  folio. Object presence and mapped-PTE presence are distinct facts. Its
  exclusion check covers the target's thread group, not every task sharing
  the backing object or mm through a non-thread CLONE_VM.
- TAKEOBJ relies on FOLL_PIN detection after unmap and ignores ordinary GUP
  references that may still write through kernel mappings. Folio locking alone
  does not serialize those accesses. MAPOBJ manually installs writable file
  PTEs and needs comparison against upstream userfaultfd, special PTE, and
  permission accounting requirements.
- The shared-object diagnostic cache holds eight unreferenced mapping pointers
  for 60 seconds. Reuse can misattribute a new object. Several counters and
  diagnostic fields are concurrently accessed without synchronization.

## Entry and extended register state

The raw-fork register control now reproduces the missing guest-child SIMD and
MXCSR inheritance in all five alternating AMD pairs. Native controls pass;
the guest parent keeps its state, and the guest child loses it. The destination
clones its monitor and only replaces GPRs. The native FPU clone helper therefore
copies the monitor's state correctly, which is the wrong logical parent.

Both entry-loop variants run guests inside exit-to-user work. AMD's rseq restart
check is outside the inner loop that continues while vmctx remains active;
per-return rseq and final architecture preparation can consequently be delayed
for the guest's entire lifetime. The module has no guest context-tracking entry/
exit calls. Audit scheduler accounting, RCU quiescent-state reporting, rseq and
speculation state at actual guest boundaries, including an interrupt while the
guest is running. These are source-level concerns pending dedicated controls.

The AMD `futex_wake_mm` hook was read with the full wait/wake file. Its private
hash lookup does use the key's mm (verified in futex/core.c), so it does not
accidentally search the monitor's private hash. It intentionally ignores PI
waiters and does not cover inode-backed shared futex keys. Its generic nr_wake
bound can be exceeded on the second key-form pass, though the only current
caller uses INT_MAX. The clear-tid path must not assume all possible futex keys
are represented by these two anonymous/private forms.

The signal-state control now also fails on AMD hardware with the unchanged
destination userspace: the handler sees the interrupted XMM15 and MXCSR,
and its own replacements survive sigreturn. The same binary natively observes
initialized handler state and restored interrupted state. Both signal entry
and return therefore need the full extended-state contract, alongside fork.

## VMA mutation and rollback

Both complete mm/vma.c implementations have received a first read (Intel via
the full interversion diff). The existing cross-task unmap accounting change
uses the removed VMA's mm, consistently with page-table teardown. The empty
range case returns before calling that accounting helper.

The mapping replication contract must cover destructive failures beyond
mprotect. In both versions, preparing a MAP_FIXED replacement can clear PTEs
and invoke close callbacks before allocating or preparing the replacement.
If a later step fails, vms_abort_munmap_vmas deliberately leaves a hole because
the old mapping cannot be restored by calling open. Returning an mmap error
therefore does not establish that the old mapping survives. This is a source
finding; a focused hardware control has not yet been run. Stack expansion and
VMA merging/splitting also occur below the syscall boundary. Future mapping
events must describe committed effects with their ordering and image identity,
including failure paths, rather than reconstruct effects from return values.

The complete AMD madvise path was also read. Its DONTNEED discard hook is
per affected VMA, after range validation; madvise can cross an unmapped gap,
change VMAs on both sides, and still return ENOMEM. Guard installation adds
PTE markers while leaving VMA access flags intact. The current TRYFAULT only
checks those flags (and can expand the stack), so its success does not prove
that Linux would service a guard-page access. The change-log notifier records
UNMAP, and the explicit hook records DONTNEED; guard-marker semantics are not
represented by either. The focused guard-access control below checks this.

guard-page.c now reproduces the distinction on AMD: native read and write
both signal SEGV_MAPERR, while guest read and write both succeed. After guard
removal the guest also retains the old content where native execution sees
zeros. Both arms use the same binary; the guest exits 1, and all contexts
retire. The runtime failure is independent of the source-level explanation.

## Complete fault and shmem path review

Both memory.c and shmem.c implementations are now read in full (Intel via
the complete inter-version diffs). Linux's COW replacement clears and flushes
the old PTE before publishing the new page, and only then drops the old rmap
reference. Its MMU notifier covers this with MMU_NOTIFY_CLEAR. File truncation
and hole punching also reach CLEAR through zap_page_range_single, while fork
write protection uses PROTECTION_PAGE. A notifier that records only UNMAP
cannot describe all these semantic changes. Guest translations must stop
using revoked pages before those pages can be released or repurposed.

The single-page allocation checks do not constitute a complete proof that
every installation is singular. do_swap_page can reuse a large folio already
in swapcache and install several PTEs, independently of alloc_swap_folio's
vmctx_install_single check. Existing huge PMDs/PUDs also use different fault
paths than newly allocated huge mappings. Their reachability and ownership
under context adoption need explicit controls before enabling batching.

The shmem allocation guard is reached only after cache/swap lookup and after
SGP_READ/SGP_NOALLOC return for holes. Ordinary shmem reads therefore still
return zero bytes for a hole without consulting that guard. The new-folio
guard checks the requested index; allowable huge orders can subsequently
allocate neighboring indices, and vmctx_shmem_alloced records the resulting
folio rather than authorizing every neighbor beforehand. These facts limit
what the current guard and counters establish; neither is a transactional
ownership check for all accesses to the backing object.

Hole punching intentionally preserves private COW pages, while shrinking a
file removes even COW pages and repeats unmapping after truncation to catch
races. Partial-page punching can zero bytes without removing the entire
folio. A protocol operation that conflates backing-object discard, mapping
permission changes, and ownership transfer cannot inherit these semantics
from a single punch call. These are source findings, not newly measured
Firefox causes.

Both proc/array.c implementations were also fully reviewed. VmCtx is a
pointer-presence diagnostic, and /proc task state and child enumeration are
snapshots rather than execution-context synchronization primitives. The
runtime controls use before/after counters and process teardown in addition
to these snapshots.
