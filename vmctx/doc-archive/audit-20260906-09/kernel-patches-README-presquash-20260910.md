> Current status and exact validation: [audit record](../audit/README.md).
> The series below is pending final consolidation into one patch per kernel
> baseline after all required gates pass. Older build-state notes are historical.

# The kernel-side changes, as patches

The active September 9 audit uses verified frozen native inputs rather than
the older local `src/linux-7.0.14` checkout. Source patch 0038 reproduces all
20 inputs of candidate #215 from verified #214 inputs without offsets or fuzz;
the proof is `episode215-patch-proof.json` in the recovery audit directory.
It adds thread-owned transfer custody, immutable replay after copyout failure,
and exact idempotent acknowledgements. It removes time-based ownership
settlement. The source/executor userspace protocol still requires integration;
see the latest audit handoff for runtime status before deploying guests.

Source patch 0039 adds explicit calling-thread custody abandonment before
waiting for native exit work. Unresolved grants poison their MMs and kill
their original source tasks; abandonment never proves delivery. Its patch
proof reproduces all 20 frozen #216 inputs. The complete 47-case native
campaign passed, including clear-TID teardown while the monitor stays alive.

`src/` is not tracked in this repository, so the kernel source is not here.
These are the vmctx changes to the kernel image itself, exported so they can be
read without cloning the kernel trees.

The audit carries working-tree patches 0002–0008 in each directory. Apply them
in numeric order after 0001. They cover failed-fork cleanup, the shmem helper
declaration, allocation-failure handling, complete CPU-state transfer and syscall
admission, source-kernel exception delivery, source-owned clocks, and an immutable
negotiated x86 CPU model. Rebuild the external module against the updated internal
headers.

The 7.0.14 source adapter also carries patch 0009: thread-owned recall tickets,
mapping invalidation of outstanding replies, exclusion of competing page
operations, source-native shmem initialization, and installation acknowledgements
for shared-page write grants. Updated vmhome uses source control 35 (RECALL).
The 6.18 execution adapter does not implement source recall and does not need
that local control. This patch does not replace the incomplete mapping journal
or the older page acknowledgement protocol; their remaining gaps are recorded
in the audit.

Patch 0010 in both series uses a fixed compatibility vendor with generic
family/model identifiers. The negotiated instruction mask is unchanged. This
allows loaders that require a recognized vendor to discover the baseline ISA;
the identity does not disclose either host's vendor. The external module includes
the canonical model header before Linux headers to prevent an older build tree's
inline CPUID helper from silently replacing it.

Source patch 0011 adds MAPPING (control 36), which queries the actual VMA and
returns a monitor-owned CLOEXEC reference to its underlying file. Shared-mapping
metadata no longer needs injected fstat/shmctl calls or guest scratch buffers.
The reference survives descriptor reuse and unmap and pins the inode while the
monitor uses it for object identity. This is a source-local API, not a wire fd
or a replacement for the pending committed-mutation journal.

Source patch 0012 makes newly opened `/proc/cpuinfo` descriptors report the
context's negotiated features and generic identity. It snapshots the immutable
model at open so a native I/O worker reading the descriptor cannot substitute
host capabilities. Unnegotiated service contexts receive EAGAIN. This does not
yet cover native descriptors inherited before adoption or other CPU data views.
Patch 0013 preserves read-only shared-file identity in the mapping query: Linux
clears VM_SHARED for an O_RDONLY file but retains VM_MAYSHARE. Private mappings
of the same file remain private. Patch 0014 applies the same classification to
page service and fault reports.

Patch 0015 adds protection of a referenced executor mm, skipping lazy holes and
returning unsupported-mapping or permission errors. Its native helper uses the
target mm and its MMU/TLB invalidation, without importing source kernel rules
into the executor. The new userspace applies permissions to every live member
before resuming the caller.

Patch 0016 adds explicit PROTECT_MM (37) and EXECUTOR_MAP_CAPS (38) controls.
The executor verifies the required local operations before START; the older
APPLYMAP success-without-effect behavior is never used for permission updates.
SET_SHARED_PAGE maps one shared-object page and installs only an existing folio,
holding the folio lock across lookup and PTE installation. A missing slot stays
absent for retry; this operation does not allocate substitute bytes. Native
controls cover read/write permissions, target-mm isolation, malformed requests,
and the empty-slot retry. These controls, structs, and operation numbers are
local adapter details and never enter the OS-independent wire mapping effect.

Source patch 0017 checks ownership under the page reservations before a monitor
POKE faults or writes memory. A completed recall does not protect a later write:
another SERVE can lend the page away between those calls. Such a POKE now returns
EAGAIN without touching the bytes; vmhome reacquires all affected pages before
retrying. The native recall control exercises this interleaving explicitly.

Source patch 0018 keeps the source's vmctx attachment through the native
robust-futex walk and clear-child-TID/wake in `exit_mm_release`. Execution tasks
still suppress these native accesses. The source adapter requires the local
SOURCE_EXIT_CAPS (39) capability before creating a guest and lets the native
kernel perform exit-memory processing in order; it no longer clears a join
word before that processing has completed. The robust-exit control covers
owner-death recovery. This source API carries no native exit ABI to the peer.

Source patch 0019 shares one refcounted MMU notifier per address space. Equal
address ranges no longer suppress distinct invalidations, and a concurrent
drain cannot split one event into several per-thread callbacks. Subscription
allocation failure ends the context before it can run without tracking.
`mm-log-state.c` checks repeated identical unmaps without intervening drains.
The optional `vmctx_map_trace` kernel parameter records source MM lifetimes,
mapping/protection commits and guard changes without faulting guest memory.
This instrumentation does not make the legacy destructive log a retained,
acknowledged commit journal; that protocol work remains necessary.

The external VMX module also invalidates its EPT pointer on the executing CPU
before every entry, including after migration and physical root reuse. It
checks local EPT format and INVEPT support before enabling that CPU's backend.
VMCS activity ends with VMCLEAR before preemption is re-enabled, so a task
cannot migrate with a VMCS still active on another CPU. These changes belong
to `vmctx/kernel/vmctx.c`; they neither change the wire CPU model nor require
the source adapter to implement Intel VMX.

VMRA (version 10) userspace requires GETCPU/SETCPU and CPU_CAPS/CPU_MODEL at both
endpoints and EXCEPTION/SOURCE_EXEC on its Linux source adapter. Each kernel
translates its own local state and page
markers; OS releases are not compared for protocol compatibility. Patch 0006
also adds TRYFAULT to the 6.18 destination adapter. Kernel builds and hardware
validation are recorded separately in the audit; these patches are not a claim
that the complete application or cross-machine run has passed.

CPU model version 1 intersects a portable instruction whitelist and source/
destination state support. Both SVM and VMX answer CPUID from this model; neither
passes host CPUID leaves to the program. Guest XCR0 follows the model, and host
XCR0 is restored before host interrupts or FPU saves. The module checks the
execution CPU at every entry, so migration to an incompatible CPU fails closed.
The Linux source adapter restricts ELF hardware flags, and userspace rejects the
native-loader bypass before starting a program. The model deliberately excludes
stateful extensions that need more implementation, including AVX-512, AMX, PKU,
CET and TSX. Canonical CPU-state transfer still uses each adapter's own XSAVE
layout; an OS implementation must not import another OS's native save area.

These are generated from pending source changes, not committed `format-patch`
output. Until those changes are committed, `format-patch HEAD` alone omits the
repairs. Preserve the pending patches when regenerating the committed baseline.

They are a **generated file, not a source of truth.** The tracked form is the
kernel repository; if the two disagree, the repository is right.

## Two trees, on purpose

There are two, and they are not the same kernel:

| directory | tree | baseline | role |
|---|---|---|---|
| `./` | `src/linux-6.18.35` | `ed9484e8f` | the Intel box (10.0.0.30), the destination |
| `linux-7.0.14/` | `src/linux-7.0.14` | `add83c849` | the AMD box (10.0.0.229), the source |

`src/linux-6.18.35` has a remote
([`vmctx-kernel.git`](https://github.com/RagingBigFemaleBird/vmctx-kernel)).
**`src/linux-7.0.14` has none** — its only `remote` points at the other local
directory — so for that tree these patches are not a convenience copy, they are
the only copy outside one working directory.

Both trees carry signal-return work and the CPU/exception adapters. Their
memory ownership implementations differ substantially: 7.0.14 has the source
page registry and mapping log, while 6.18.35 is the Intel execution adapter.
Adding a local control to both trees does not make their source-side feature
sets interchangeable. The protocol must describe source-owned state independently
of either backend's implementation.

Do not "sync" one tree from the other. A previous attempt to do that by copying
files destroyed `src/linux-7.0.14/mm/memory.c`, which had to be reconstructed
and proved correct by reproducing the running box's md5.

The execution snapshot additions are source-series 0021/0022 and executor-series
0017/0018. Both implement quiesce ABI 2 (local control 41) and direct private
backing protection (42). A monitor-owned lease prevents hardware entry and
waits for earlier entries to exit; owner task exit cancels its leases. The SVM
and VMX module must explicitly register gate support. PROTECT_BACKING requires
the caller's lease, resolves the execution context's configured backing object,
and protects existing folios without requiring a VMA or allocating holes.
The protocol carries a source-selected snapshot preparation requirement; these
native APIs do not give the executor source syscall or clone-flag knowledge.

## Regenerating

After **any** change under `src/`, regenerate — in the same commit as the change:

```bash
rm -f vmctx/kernel-patches/*.patch
git -C src/linux-6.18.35 format-patch --no-signature ed9484e8f..HEAD \
    -o "$PWD/vmctx/kernel-patches/"
rm -f vmctx/kernel-patches/linux-7.0.14/*.patch
git -C src/linux-7.0.14 format-patch --no-signature add83c849..HEAD \
    -o "$PWD/vmctx/kernel-patches/linux-7.0.14/"
```

The output path must be absolute: `-C` moves git into the kernel tree first, so
a relative `-o` writes the patches to `src/linux-6.18.35/vmctx/kernel-patches/`
and this directory keeps whatever it had. `format-patch` prints the names it
wrote either way, which is exactly what makes it look like it worked.

## Why this file says all that

This directory once held a single patch exported from a commit that a later
squash removed. It described 65 changed lines of the 1312 that actually differ
from vanilla, its filename named a different change from the one inside it, and
nothing said so — a stale generated file looks exactly like a current one.

That note was written, and it happened again anyway, in a worse form. Nine
kernel commits — `VMCTX_CTL_SYSCALL`, the service context, `VMCTX_CTL_ADOPT`,
the register change mask, per-page `vmctx_mprotect()`, the fault-around fix:
the entire assisted-syscall architecture — sat in `src/linux-6.18.35` unexported
while eleven stale patches sat here looking complete. At the same time the whole
of that architecture was **uncommitted** in `src/linux-7.0.14`, which is the
kernel the source box had actually been running and measured against for days.

Both are now exported and committed. The lesson is not "remember to regenerate":
it is that a directory of generated files that is *nearly* right is the failure
mode, because the count looks plausible and nothing reads them until the source
is gone.

Runtime accounting (6.18 patch 0019 / 7.0 patch 0023) adds native controls 43
(RUNTIME) and 44 (BOUNDARY), with the versioned local ABI in
`vmctx/kernel/vmctx_runtime.h`. The executor measures cumulative execution CPU
nanoseconds, excluding descheduling and page/source service waits. Credits enter
the source task's public CPU clocks, exited-thread totals and active CPU-timer
caches, while its local scheduler runtime and host CPU accounting stay separate.
A source budget arms execution checkpoints; a native repeating timer forces exits
even on tickless CPUs. The source processes its CPU timers and pending work at
these boundaries, and returns complete architectural state without treating a
checkpoint as a syscall or overwriting AX with a synthetic return value.

These controls require a full kernel/module rebuild because internal task and
context layouts changed. Protocol v14 carries a cumulative count and opaque
executor lifetime identity with CPU-bearing requests. Both adapters must support
runtime accounting before source START. A different source OS implements its own
clock/timer accounting; no source clock IDs, timer IDs, signal numbers, or syscall
meanings are interpreted by the executor. Native and cross-machine validation
status is recorded in `vmctx/audit/README.md`; the
presence of these patches is not a statement that the expanded suite is green.

Initial restore fixes (6.18 patch 0020 / 7.0 patch 0024) preserve the installed AX
through native syscall return and clear restart state. RESTORE requires the
monitor wait gate; timeout or interruption before explicit release terminates
the executor, since SETCPU may already have replaced its native user frame.
Executor RESUME is serialized against CPU installation and publishes the release
with the matching memory ordering. Source service replies remain independent of
CPU-control locking to allow faults during a source syscall to be serviced.

Source patch 0029 adds nonblocking native syscall admission (control 45,
local gate ABI 2). BEGIN runs entry tracing/seccomp work on the canonical
frame, QUERY publishes the admitted call, and COMMIT dispatches once. Tickets
fence retries and stale requests; result-copy failures cannot replay dispatch.
Completion separates the raw syscall result from the final tracer-visible
registers. Native fork/vfork publish committed child identity before the parent
returns; GETCPU waits for a child's initial native tracing/signal work to reach
its service park. The source monitor must still keep valid debugger stops free
of userspace deadlines. The 7.0.14 native controls passed on candidate #205;
wire integration and the corresponding 6.18 source implementation are pending.
The access-journal support header also reuses its request buffer for its reply,
removing an oversized kernel stack frame without shortening input validation.

Patches 0025 / 0030 bind an unreleased restore to its original monitor's task
lifetime. Initial attachment still has a five-second deadline; after attachment,
only explicit RESUME authorizes entry. Detach or owner exit terminates the
restore, and a successor cannot revive its partially installed frame. The new
kernel/vmctx-monitor-core.h protects monitor task references under ev_lock and
removes unreferenced dereferences from owner checks, inheritance and diagnostics.
Initial context publication and attach-versus-teardown ordering are also fixed.

Patches 0026 / 0031 make taken runtime checkpoints killable. Fatal cancellation
ends execution with SIGKILL, does not repeat the boundary and does not count as
a fault deadline. Intel #68 passed sixteen native controls, including long
restore holds, explicit release, detach, owner exit, monitor reference races,
runtime/fault cancellation and a positive elapsed-deadline control. AMD #206
verification is pending. Exact patch application hashes are recorded in the
lifetime audit; these source-local changes do not complete wire admission.

Source patch 0032 retains task/context lifetimes behind monitor-owned descriptors.
It preserves terminal status, MM identity and committed child publication after
reap, rejects foreign monitor processes, and keeps live native ptrace permission
checks. ACCESS_LOG and RECALL use the already resolved task. Descriptor copyout
failure installs no file; final references retire through RCU. It also identifies
direct SERVICE contexts in the source ownership table and serializes backend
destruction with CPU control. AMD #209 passed 23 native controls, including parent
and child death in either order, true vfork, forced namespace PID reuse, live
credential denial, terminal mutation refusal and descriptor cleanup. This patch
does not itself migrate the userspace protocol or PID/MM caches. Applying it to
the verified predecessor reproduces the frozen #209 candidate contents.

Source patch 0033 permits INFO and OPEN while an assisted syscall waits for I/O.
Only committed child export needs the CPU mutex to protect ticket references.
Metadata snapshots use referenced task/MM objects and published terminal state;
a retirement window without a valid MM identity returns EAGAIN. AMD #210 passed
25 native controls, including blocked-read metadata access and the source
adapter's proc namespace, PID reuse and non-leader exec identity checks.
The patch applies without fuzz and reproduces the frozen #210 contents.

Source patch 0034 adds context ABI 2 native MM lifetime queries and exact-task
signals through retained context descriptors. ABI 1 OPEN/INFO/CHILD stays
compatible. ABI 2 returns PID/TGID names in the querying monitor's namespace;
these names select proc candidates and are validated against retained context
and MM identity. MM queries require initial-namespace CAP_SYS_PTRACE and count
pending native births and memory-operation references. SIGNAL uses the retained
task, never a numeric PID lookup. Patch application is byte-exact against #210;
#211 passed 26 native controls before the subsequent backend exit correction.
Wire v18 source admission is integrated in the production worktree. Expanded
integration validation remains in progress; see the September 9 audit ledger.

Patches 0035 (7.0.14) and 0027 (6.18.35) retain native execution backing
objects and add epoch-fenced object replacement. Replacement preserves other
MMs' objects, excludes execution and concurrent native memory controls, and
recognizes descriptor aliases by object identity. Copyout retry is idempotent.
The caller must separately exclude old-MM distributed page operations; this
primitive does not make a multi-message transfer atomic. Both patches apply
without fuzz and reproduce their frozen candidates byte for byte. AMD #212
passes 29 native controls; Intel #69 passes all 18 native controls.
The production executor MM registry and wire migration are still pending.


Source patch 0036 and execution patch 0028 extend retained native context
handles to execution contexts. Global CAPS distinguishes source and execution
roles. Owned terminal INFO retains context/MM identities through native task
reaping; it does not report a native wait status for an execution context.
Mutation controls reject ended lifetimes, and live controls retain monitor and
ptrace checks. All object, CPU, map and wait controls accept retained selectors.
The native regressions include descriptor inheritance ownership checks and
forced native PID reuse. AMD #213 passes 32 controls, Intel #70 passes 21;
see executor-mm-design-20260909.md for exact artifacts and remaining production
MM identity/transfer work. These results do not make the guest suite all-green.


Source kernel 7.0.14 patch 0037 adds the expected-MM MEMORY envelope. Candidate
#214 passes all 34 AMD native controls on boot
90e1489a-bf8f-481a-bedd-12f2cc380f18, including the paused inner-copy/exec
regression and retained execution object/MM registry control. Wrong-MM calls
are rejected before inner argument access for all 18 allowed memory commands.
The baseline on #213 retired the old metadata and read the replacement MM;
#214 retains the exact old MM and reads its original bytes after exec.
Full kernel, 6891 modules, and external module build/package are warning-free.
Patch replay reproduces all 18 core hashes without fuzz/offset. Full evidence
is mmfence214-amd-native-1; source adapter coverage is
memory-adapter-amd-native-1. This API does not complete the userspace protocol
identity migration or fix native transit ACK identity.

Source kernel 7.0.14 patch 0040 transfers custody of private pages which may
become writable, including when the VMA currently permits only reads or
execution. Native forced COW preserves VMA permissions and file/fork aliases;
TAKE then removes the source PTE and retains the transfer until ACK. COW of
an existing zero PTE requires the active TAKE reservation, address and calling
task, rather than a general monitor exemption. Patch replay reproduces all
21 frozen #217 core inputs without fuzz or offsets. The kernel, 6,891 modules,
and external module built and packaged without warnings. All 51 native controls
pass, including four RO custody classes and competing file mappings; the original
read-only-upgrade guest passes. A separate RO recall/upgrade guest exposed a
userspace GETS copy; switching source recalls to GET passes both byte oracles. This patch does not close userfaultfd,
THP collapse or remap ownership gaps; see mm-path-coverage-20260909.md.

Execution patch 0041 (7.0.14) adds MAPOBJ_READ and an advertised capability.
It maps an existing shmem folio without creating holes or granting write access,
including for read-only or execute-only VMAs. Read-populated PTEs in writable
VMAs remain write-protected; existing PTE permissions are preserved. Kernel
#218 passes 55 native controls, including actual read/write/execute/hole tests,
and both read-only custody/recall byte guests. The 22-file patch replay and full
kernel/module/package checks pass. Userspace mm-readmap32 uses fault intent to
choose read population or the existing snapshot-preserving writable grant.
Intel #71 now advertises this capability through executor patch 0029 below.
MM coverage remains incomplete.

Source patch 0043 fixes HMM's owned device-private PTE/PMD write checks.
Kernel #220 (`7.0.14-vmctx-audit-recall1-hmm220`) builds without warnings
and passes all 56 native vmctx controls. Its HMM fork/permission probes pass
where #219 returned a read-only PFN for a requested write. The independent
HMM campaign also exposed a missing device release callback in `test_hmm`.
Patch 0044 initializes memory before publishing the device and drains its
last device reference before freeing storage or unloading its callback;
failed initialization releases the chunk table too. The separately built
test module passes all 11 HMM probes and clean unload, with off-host kernel
capture and unchanged zero vmctx counters (B/hmm38-native-1). Patch 0044 is also
present in the verified #222 source/package. On #222 all 11 probes and clean
unload pass again (B/hmm222-native-2-raw.tar.gz).
Neither patch integrates HMM residency with vmctx transport.

Executor patch 0029 (Linux 6.18.35) ports MAPOBJ_READ and preservation of
existing PTE/PMD/PUD write protection during vmctx mprotect from the AMD
0041/0042 changes. Its replay reproduces all 21 frozen #71 core inputs;
the full kernel/module package preserves all 3,767 modules. Intel runs #71 and
passes 26 native controls. The #222-to-#71 watchwrite40 baseline passes all 78
guest suite cases; broader application and native MM gates remain open. See
the [validation record](../audit/README.md) for exact inputs and evidence.

Source patch 0045 rejects capture guard admission over an incumbent source
fault CLAIM or TRANSIT under the same MM lock used by fault admission. The
capture-claim native control reproduces #220 blocking LAND until a refused
capture ticket is cancelled. The 26-file replay is exact; #222 includes this
change and passes 57 native controls. Local LAND/POKE guard semantics remain.

Source patch 0046 checks terminal death and monitor attachment under the same
event lock. This preserves authorization for retained QUERY and CHILD reads
when native exit detaches the monitor. Against #221 the retained QUERY control
fails at native exit 872; #222 passes 2,000 exits and 429,051 successful queries
with no lost authorization. The exported three-file patch replays without
fuzz or offsets and reproduces all 26 predecessor core files in the verified
#222 snapshot. This exports the already tested kernel change; it does not
resolve the separate native residency and foreign GUP ownership gaps.
