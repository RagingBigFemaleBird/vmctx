# Native syscall admission work in progress

No userspace/wire integration has been made. Existing frozen user binaries are
still e82e1cd92adb/wire16. Their 75-case suite has one confirmed remaining class
of failure after the separately verified debug fix: ptrace-syscalls. The broad
GDB/strace and Firefox reruns remain pending.

AMD kernel candidate #204 built and booted from immutable inputs on D:
`logs/audit-20260907/admission204-inputs/`, with the exact prior sources in
`admission204-before/` and a candidate diff in `admission204-candidate.patch`.
The remote build lives in `/home/biwu/audit-20260907/`; driver
`build-admission204-amd.py` verifies old artifacts and every input before and
after a full kernel/modules build. Expected unique release:
`7.0.14-vmctx-audit-recall1-admission204`. Do not reuse #203 modules for this
changed task-structure ABI. Do not edit frozen inputs or overlap builds.

The source-native candidate introduces VMCTX_CTL_SYSCALL_GATE (45), with
nonblocking BEGIN, QUERY, COMMIT, CANCEL and numbered per-context tickets.
`vmctx/kernel/vmctx_syscall_gate.h` is the current shared local ABI, included
AFTER struct vmctx_syscall. BEGIN installs no extra arguments; it borrows the
canonical frame already installed through SETCPU. It runs native ptrace,
seccomp, audit and user-dispatch entry work, then parks on that same kernel
stack before dispatch. QUERY returns the admitted number and arguments;
COMMIT is the only path to dispatch. Native exit work follows exactly once.
Repeated BEGIN/COMMIT for a ticket read existing state, including after bad
copyout, and do not execute again. Old tickets fail ESTALE. State writes and
legacy assisted calls are excluded while pending. A referenced initiating
monitor task makes abandonment checks safe; detach is refused while pending.
Internal monitor operations retain the legacy untraced helper.

Linux 7.0's syscall_exit_to_user_mode_work performs only one-time exit work,
including rseq_debug_syscall_return. Its 6.18 counterpart ALSO enters the
return-to-user loop and must NOT be called here. The eventual 6.18 patch must
use its rseq_syscall plus syscall_exit_work portions without context transitions
or a nested return loop. Source control entry is already in kernel context.

Candidate native child publication is after copy_process and parent-TID writes,
before wake_up_new_task and before ptrace/vfork waits. It publishes host PID,
parent-namespace PID and actual mm-sharing to the gate. Publishing before wake
avoids a child exit/mm race. These fields do not authorize executor entry; child
initialization and source tracing stops still need a readiness contract.

`tests/control/syscall-gate.c` tests native ptrace entry rewrite to write, no
pre-commit effect, exclusion, read-only result-copy failure, duplicate commit,
ptrace exit result rewrite, skipped calls and stale-ticket rejection. It builds
warning-free using `admission204-control-include/`. Baseline #203 fails at BEGIN
with EINVAL as expected and cleans up; `syscall-gate-debug203-baseline.log`.
Candidate runtime verification passed (see continuation below).

## Requirements before userspace integration

- PREPARE must send the full captured CPU state/runtime and begin native entry
  admission. Source classifies the actual admitted call, not the incoming opaque
  number. Executor gets only semantic preparation flags and an opaque ticket.
  COMMIT must not retransmit the pre-tracer CPU state. Source keeps the admitted
  request across the executor's preparation and uses it for effects.
- Distinguish raw dispatch result from final tracer-visible AX. Candidate #204
  currently returns only final AX. A tracer can change mmap's exit result while
  the actual mapping remains at the kernel-returned address; mapping effects
  must use actual dispatch/committed metadata. Add a raw-dispatch result field
  in a follow-up candidate before integration, or use fully correlated committed
  native mapping records. Do not infer effects from the final visible AX.
- Remove vfork/clone/clone3 rewrites once admitting native calls. True vfork and
  ptrace fork stops need intermediate child publication while the parent is
  blocked, then later parent completion. Clone success is committed metadata,
  independent of a tracer changing its visible return value. Executor snapshot
  leases must cover child snapshot construction, then release so the child can
  execute while the parent waits. Allocation/thread-start failure must end the
  affected execution tree, not strand an unpublished child.
- A child may be ptrace-stopped before its first native service park. Initial
  GETCPU alone does not authorize execution. Wait for source readiness and apply
  the current full CPU state after native initial work/stops, including tracer
  changes, before release. Preserve the child's syscall exit tracing semantics.
- as_of currently equates TGID and mm and caches forever. CLONE_VM without
  CLONE_THREAD and vfork violate this. Use source mm lifetime identity (the
  committed access journal already exports one), with a separate live task
  representative. Audit callers that use as_of as a /proc PID before changing
  it. Exec changes mm identity; PID reuse must not join prior state.
- page_recall_all currently can clear/ACK after pull failure; preparation must
  fail closed before dispatch. Snapshot admission, source-owned child page
  enumeration and stable lineage must remain in one lifetime/order domain.
- Query/commit retries must not encounter the old 30-second assisted timeout.
  Native gate is nonblocking; userspace must poll with connection/owner checks
  and preserve valid long debugger stops without inventing a guest syscall
  result or resubmitting the call.

## September 9 continuation: #204 native gate verified

AMD #204 booted as `7.0.14-vmctx-audit-recall1-admission204`, boot identity
`f91dbc96-f411-4999-bcd0-4a622c1b526f`. All 6,891 modules and the fresh initrd
were built, hashed and checked for unresolved symbols before a one-time GRUB
boot. The default and previous boot images remain preserved. Manifests:
`admission204-build-complete.json` and `admission204-package-complete.json`.

Ten native checks passed with live kernel capture, watchdogs and zero leaked
contexts/references. The one lost-page increment is the deliberate failed TAKE
copyout regression. The new native admission control passed entry rewriting,
no pre-commit effect, native exit result rewriting, skipped calls, busy-slot
exclusion, bad-copy retry, duplicate commit and stale tickets. Evidence:
`admission204-amd-native-1/complete.json`, `rows.json` and `live-kmsg.log`.

Both backends now use separate assembly functions for guest GPR exchange.
This fixes frame-pointer tracking in the old nested inline BP save/restore and
adds native VM-exit return-buffer mitigation. Compile-time assertions cover
all assembly structure offsets. SVM restores host registers before STGI.
`vmx-entry2-inputs/inputs.json` is the frozen authored snapshot; both builds
are warning-free. AMD module SHA256:
`13818c7cb7d379a511ecab01b59e469853be6f27991d929c86da4ac34d806a19`.
Intel module SHA256:
`6375c27c145d1ff42fb87026b5a04b261cdbf83d67cc46524e278613c8d828ab`.
Intel #66 passed nine native controls again with this module, loaded from
`/root/audit-entry2/vmctx.ko`; evidence `entry2-intel-native-1/complete.json`.

The first assembly snapshot accidentally included generated `vmctx.mod.c` in
its input manifest; post-build verification rejected it. The replacement
snapshot excludes generated inputs and verifies every authored input before
and after building. Its additional SVM extraction resolved the final objtool
warning. Neither diagnostic was suppressed. #204's core still has the access
journal's oversized-frame warning; the single-buffer source fix is authored in
`kernel-patches/support/vmctx-access-core.h` for the next candidate.

## Next candidate and userspace work

Candidate #205 has built and packaged warning-free and a one-time boot was
requested. Frozen input and package manifests are `admission205-inputs/`,
`admission205-build-complete.json`, `admission205-package-complete.json`.
Its module is `694ea30214915c4f3d7f93ed8e67256aaead086f6a4ac1b8f3510dc17072d73d`.
It adds gate ABI 2's raw `dispatch_ret`, leaves the canonical gated frame in
place on completion, synchronizes FS/GS, seeds holds from admitted arguments,
and refuses GETCPU/BEGIN before a service child's initial native return work
reaches its park. Native tests `syscall-gate.c` and `syscall-gate-child.c`
cover these additions; the latter tests fork/vfork intermediate publication,
initial child stops, true vfork blocking and an exit-time AX rewrite to EPERM.
Runtime verification of #205 is pending at this update.

The authored `user/page-settle.h` now returns failure before COMMIT on a failed
pull, incomplete landing or absent source page without a surviving object
copy. Both shared/private paths install returned bytes before acknowledging,
and a failed page retains its loan. The fault-injection control
`tests/control/settle-failures.c` passed. A full userspace build from
`settle1-tree` succeeded but exposed an unused old `vma_covers` helper; that
helper has since been removed in the working source and needs a fresh build.
No modified userspace binary has been deployed. This helper's integration
still needs the per-page ownership/concurrency review: execution snapshot
leases stop guest instructions, not concurrent source fault-service transfers.
Do not call the larger snapshot protocol correct from its isolated test.

### #205 verification completed

AMD #205 boot identity is `df6c972d-f067-4881-92c5-5946d17c40e5`.
The nine established native cases plus ABI 2's syscall-gate case passed in
`admission205-amd-native-1/rows.json`. Both additional fork/vfork cases passed
with live kernel capture in `admission205-child-native-3/complete.json`.
Counters ended 0 contexts / 0 module references / 1 intentional TAKE-copyout
loss; no unexpected kernel diagnostics. Gate ABI 2 preserves the raw positive
child PID even when the tracer changes visible AX to -EPERM. True vfork stayed
RUNNING until the child ended, then produced native VFORK_DONE and SIGCHLD work.

The first child-control failures were oracle errors, not suppressed kernel
failures. `native-fork-stop-order.c` ran without vmctx on the same source kernel
and established that the initial child SIGSTOP already contains fork's return;
there is no separate child syscall-exit stop afterward. The corrected test
also services and explicitly counts the later native SIGCHLD delivery stop for
vfork completion. Original controls and failed-run evidence are retained in
`syscall-gate-child-205-original.c`, `syscall-gate-child-205b.c`, and native
runs 1/2. The current control is binary `syscall-gate-child-205c`.

Patch `linux-7.0.14/0029-vmctx-admit-native-syscalls-before-dispatch.patch`
was exported from the exact #203-before/#205-built inputs. Applying it to a
separate before-tree reproduced every built source hash; evidence
`admission205-patch-check.log`. The updated settlement userspace builds
warning-free from frozen `settle2-tree`, source identity `95eb9d77ad54`.
Those binaries remain undeployed and retain wire16; admission is still native
only. Intel remains #66 with `vmx-entry2` module and e82e execution userspace.

Further integration constraints found during the child-path read:
- `cpu_state_ctl` currently limits EAGAIN to five seconds; a source child's
  valid initial ptrace stop needs connection-aware readiness polling without
  that deadline.
- Executor restore currently expires after five seconds even with a live
  monitor. Creating an executor before child readiness requires a lifecycle
  contract that preserves valid long stops without that expiry, or postpones
  executor creation while publishing/protecting its memory snapshot separately.
- Do not hold a snapshot lease while waiting for a debugger that may itself
  execute in the paused mm. Publish/protect the child snapshot, release the
  lease, and let source readiness gate only the child's eventual entry.

### Restore and monitor lifetime continuation

The `restore-lifetime.c` control reproduced the attached-restore expiry on
unmodified AMD #205. No-attach passed at 5,055 ms; a live attached unreleased
executor exited at the same five-second deadline. Both cases left zero
contexts/references and no new lost pages. Evidence: `restore205-baseline-1/`.
The old `restore-gate.c` intentionally asserted that old timeout and is retained
as historical evidence; the new control replaces it for the new contract.

The authored `support/vmctx-monitor-core.h` gives each monitor dereference a
referenced task or the ev_lock. Attachment/drop and dead-owner removal serialize
under that lock, including inheritance and diagnostic stack capture. A restore
retains its original owner task reference while waiting without a time limit
for explicit RESUME. Detach or owner exit ends it with ENOTCONN; an abandoned
unreleased restore cannot reattach to a successor. SIGKILL still ends it as
SIGKILL, and the initial unattached deadline remains five seconds. Initial
context publication now uses task_lock and exposes active only after setup;
control rejects an incompletely created context. Teardown publishes dead before
dropping the owner, preventing a late ATTACH from leaking a task reference.
These changes are authored for both kernels. They do not assert a complete audit
of all existing concurrent CPU-control-versus-backend-teardown operations.

Intel #67 built warning-free from `lifetime67-inputs/`; all 12,045 core symbol
CRCs are unchanged. Build/package manifests: `lifetime67-build-complete.json`,
`lifetime67-package-complete.json`. Module SHA256
`44a7e2c8e6d95d80ad834164bdb4aa02798b618df632b0bd9658f2ce1c659a92`.
Kernel source SHA256
`93dbc5bfb07ca4c6a71847a3eab6e34b715bc06ed23ee16aaea847318d97267b`.
Boot is in progress at this update. AMD authoring copy is `lifetime206-work/`;
it is not frozen/built yet. The shared support header is identical in both.

The userspace read found that n_live_children was decremented but never
incremented. Authored changes reserve the count before pthread_create, release
it after the final source reply and wait on a condition variable for actual
service completion. The old three-second no-live-task heuristic could abandon
valid teardown and is removed. Allocation, thread creation, connection, binding,
CPU installation and RESUME failures now terminate the execution tree instead
of stranding a gated child. These changes are undeployed and unbuilt. Source
child death during a valid ptrace stop still needs explicit wire lifecycle
handling; do not classify such death as an infrastructure failure in the new
admission protocol.

### Intel #68 verified; AMD #206 build in progress

Intel #68 booted as 6.18.35-0-lts-audit-debug65, boot identity
442e5aa1-b2b9-4b16-94ea-928ae4316ffa. Its full sixteen-case native run passed:
`lifetime68-intel-native-1/complete.json` and `rows.json`. Runtime cancellation
completed in 1 ms with no deadline increment; the positive fault-deadline case
took 6,106 ms and incremented the deadline counter exactly once. The monitor
race control executed 10,192,583 attach/drop/state/exit operations without an
unexpected errno. Counts ended 0 contexts / 0 module refs / 1 intentional
TAKE-copyout loss, with no unexpected kernel diagnostics. Module path is
/root/audit-lifetime68/vmctx.ko; SHA256
`d19621e14b377c1d6a89ba5c2481c4367dd5c4864d548537fc1c380214ffffe8`.
The boot package is `audit-20260907-lifetime68-repacked`; source hash is
`91da4ebb5397170a34c9035de102de97574f4623919a918676d983f1c8c25a82`.

#67's release control uncovered a separate uninterruptible taken-runtime-event
wait. The focused control captured vmctx_report -> vmctx_guest_step ->
exit_to_user_mode_loop with SIGKILL pending, then detached for cleanup. Evidence
`lifetime67-intel-native-2/runtime-cancel.log`. #68 fixes that wait and preserves
SIGKILL rather than translating it to a normal checkpoint failure. The restore
release control now always consumes and acknowledges its runtime checkpoint
with ACT_DONE; original control variants remain in the artifact directory.

BusyBox setsid hid failing child exit statuses by returning its own successful
fork status. Native controls now use the statically built `native-run.c` session
owner, which propagates actual test status, enforces its bound, kills remaining
owned descendants and reaps them as subreaper. Failure/signal/deadline/orphan
oracles passed locally and on Intel (`native-run-208*-oracle.json`). The native
harness still requires PASS and rejects FAIL independently of exit status. Log
capture now records its remote PID/start time and explicitly ends that owner;
the older #67 capture left cat PID 3894, which was identified and ended.

Patches 0025/0026 (6.18) and 0030/0031 (7.0) reproduce the frozen source hashes
when applied independently to their exact predecessors. Evidence:
`lifetime-patch-checks.json`. AMD `lifetime206-inputs/` is now frozen and staged;
`build-lifetime206-amd.py` is active at this update. Do not overlap another
kernel/module build, packaging install or reboot with it. After completion,
run `package-lifetime206-amd.py`, verify its manifest, then
`boot-lifetime206-amd.py` for the one-time candidate. Live AMD is still #205.

Userspace child-lifetime/settlement changes build warning-free from
`lifetime1-user-tree`, source identity `618cc2993cee`, with all input hashes
rechecked after the build. The binaries remain undeployed and use wire16.
Protocol admission, source mm lifetime identity and meaningful child-readiness/
source-death handling are still outstanding. No all-green claim is made.

### AMD #206 native verification completed

AMD boot identity `4fafe1e6-9830-40bf-9587-d11477df7b78`, release
`7.0.14-vmctx-audit-recall1-lifetime206`. All nineteen native cases passed in
`lifetime206-amd-native-1/complete.json`. This includes the sixteen lifecycle,
CPU, memory-transfer and cancellation cases plus gate ABI 2 and native traced
fork/vfork. Counters ended 0 contexts / 0 refs / 1 intentional TAKE loss. Runtime
cancellation took 1 ms; the positive fault deadline took 6,131 ms. The concurrent
monitor control ran 3,234,236 operations without an unexpected error. Module
`/home/biwu/audit-20260907/lifetime206-native/vmctx.ko` has SHA256
`651d6525cb1396b1b6f50c338fd1b388491767faf224cc552024528711983535`.
The default boot remains #181; #206 was a one-time GRUB request. The driver's
last print still said #205, but its verified GRUB id and actual uname/boot id
are #206. No kernel build, packaging job or reboot remains active.

Frozen userspace `618cc2993cee` has now been staged at AMD
`/home/biwu/audit-20260907/lifetime1-user-tree` and Intel
`/root/audit-lifetime68/vmremote-618c`. Hashes were checked after transfer.
The first focused cross-machine case is running through
`lifetime1-cross-control.py fork-state 1`; the helper captures both kernel logs,
checks boot identities and zero resource/loss deltas, and uses native-run-208 to
preserve Intel's actual exit status. These binaries retain wire16.

Further source lifetime review is recorded in
`context-lifetime-design-20260909.md`. Its context-descriptor proposal is not
implemented. In particular, no parent-death-before-child-publication native
regression has yet run; the issue is identified from control lookup and teardown
code. Resolve MM identity and durable child publication while integrating native
admission, rather than claiming that the current PID/TGID cache provides them.

### Wire17 child readiness: observed nested-fork failure and candidate

The frozen wire16 lifetime1 cross `fork-state` passed. The next case,
`fork-nested-io`, failed with executor status 97 and clean resource/loss counters.
Source logs show child 9651 creation followed by the five-second CPU-state
capture timeout. The child can fault in initial native return work, but its own
monitor starts only after publication and CHILD adoption; waiting for readiness
before publication creates a circular dependency. Evidence is preserved on AMD
under `lifetime1-cross-fork-nested-io-1`, driver log in the local artifact base.

Wire17 candidate `2617f17f5e9d` publishes the gated child first, starts its source
monitor, then fetches complete canonical CPU state before executor RESUME.
CPUSTATE returns payload-free PENDING or ENDED lifecycle replies rather than
imposing a five-second deadline on native ptrace/signal work. The executor seeds
only a provisional parent frame while gated, then installs the child's full
state. Source death during readiness skips execution and preserves source status.
The source still uses legacy syscall execution; native admission is not wired.

The separate `ptrace-child-start.c` control passed natively on the local host:
initial traced child remained stopped for 7001 ms, then executed with the tracer's
R13 edit. It will be used as a guest regression; it has not yet passed remotely.
Frozen lifetime2 userspace built warning-free and all inputs and outputs were
hashed. Build evidence: `lifetime2-user-build-complete.json`. Deployment uses a
new tree and binary name; existing frozen lifetime1 artifacts remain intact.

### Wire17 focused verification and broad suite

All eight focused AMD #206 -> Intel #68 cases passed with frozen lifetime2
`2617f17f5e9d`: fork-state, fork-nested-io, fork-late-sibling,
thread-reuse-lock, ptrace-step, uring-rings, debug-traps-prefix and the new
ptrace-child-start (7033 ms stopped, final R13 edit preserved). Resource counts
and loss deltas are zero on both hosts; no unexpected kernel diagnostic.
The first series driver used abbreviated filenames late-sibling/uring; it
stopped before starting those cases. The corrected driver and results are
preserved as lifetime2-cross-series-1b. The syscall-rewrite control still fails:
status 0x300, no entry mutations observed. Native admission remains unwired.

The old monitor-map diagnostic matched /memfd:vmctx-ownership, which is the
monitor's deliberately shared bookkeeping, not guest bytes. monitor-maps.h now
exempts that retained object's device/inode, not its name. The regression maps
a second object with the same name and verifies that it is rejected; invalid
metadata descriptors also fail. A forbidden map now prevents executor creation.
All nine check-host controls pass, including failed settlement and this guard.

Frozen lifetime3 `61f908a31e58` built warning-free and was staged/hash-verified on
both hosts. The new long-stop guest is included in suite.sh. The rebuilt native
AMD loopback suite finished **75/76 passed**; only ptrace-syscalls failed. All
76 cleanup reports are complete, counts ended 0/0, lost-page delta 0, boot stable,
and live kernel diagnostics clean. Evidence on AMD: lifetime3-suite-1/result.json.
The guest compile log has an mf2 warning about publishing a local stack address;
that test's synchronization/lifetime needs review. The user binaries have no
build warnings. Firefox and broad program reruns remain pending.

A lifetime3 cross preflight timed out during SSH banner exchange before any
workload started. Intel was subsequently reachable with the original #68 boot
identity and 0/0 counters. The retry uses run index 2, preserving the first log.

### Candidate #207 source identity work

See context-lifetime-design-20260909.md's candidate section. The source kernel
is building from immutable context207-inputs with the retained descriptor API.
Core compilation has completed without warnings and the full kernel/modules
build is still running. No candidate boot or runtime validation has occurred.
The live hosts remain AMD #206 / Intel #68. lifetime3's nested-fork cross retry
passed with the monitor guest-map counter now correctly zero.

mf2's guest-test warning identified two oracle defects: an unsynchronized shared
pointer/stop flag, and a sleep that allowed the sibling-stack assertion to be
skipped if publication was late. The working test now uses atomics, a readiness
barrier, mandatory pointer validation, and clears the published pointer before
its stack frame ends. It compiles with -Werror and passes natively locally as
mf2-lifetime4. It is not part of the earlier frozen 75/76 run and has not yet
been tested under vmctx. The old frozen test remains unchanged.

### #207 source-only failures and #208 rebuild

Read the latest context-lifetime-design-20260909.md section. Live AMD is #207
without a backend. Source-only gate/fork/vfork pass, but the new descriptor
control exposed a zero MM identity caused by direct SERVICE setup registering
an executor mm. A misplaced module-style sysfs counter was caught before module
load. #208 fixes both and is now built warning-free, packaging in progress.
The canonical descriptor controls, including isolated PID reuse, remain unrun
on #208. The last broader guest result remains lifetime3 on #206: 75/76.

## #208 native result and #209 terminal descriptor authorization

AMD #208 booted as 7.0.14-vmctx-audit-recall1-context208, boot ID
4cc98a31-2043-4665-923f-f540793eb0de, backend SHA256
3aaf4a8a05a55132cfcaa270416de65afaec6369d78bfddac3427ffcee1c403a.
Its first nineteen native controls passed. context-parent-first then failed:
INFO after SIGKILL/waitpid(parent) returned EPERM. Initial INFO now correctly
reported identity=3, native_pid=8377, mm_identity=25, READY. Cleanup left zero
contexts, backend references and handles; take_lost=1 was the deliberate
TAKE copyout case. Evidence: context208-amd-native-1/rows.json and
context208-native-driver.log. No complete manifest exists for this failed run.

The source of EPERM is native security/yama/yama_lsm.c:
yama_ptrace_access_check in relational mode explicitly rejects !pid_alive(child),
even for CAP_SYS_PTRACE. Live descriptor operations must retain this native
credential/LSM check. A retained terminal record, however, is an existing
capability belonging to the opening monitor TGID (a referenced struct pid),
not a new ptrace attachment. Candidate #209 bypasses ptrace only after the
context has published death; it rechecks death after ptrace denial to cover
an exit race. Existing outer and per-operation checks restrict terminal access
to INFO, CHILD export and gate QUERY; CPU/memory/RESUME and gate mutation fail.
Child descriptors still require ordinary ptrace checks for any live operation.

The native lifetime control now explicitly tests real-credential denial while
live, terminal INFO from the same owner after dropping credentials, foreign
process denial after death, rejected GETCPU/SETCPU/RESUME/BEGIN/COMMIT/CANCEL,
and stale CHILD tickets, in addition to the original lifetime/PID reuse cases.
No oracle was relaxed. Candidate #209 is BUILDING from frozen context209-inputs,
core SHA d43436b2a50ac7e48074916e2c211205868f51a0454929931727397a36dcf39f.
Control SHA 49e6bf22effeb4624a36bb69294dae07e940f042fbea5f71f88a4f6525029ba2.
Its build/package/boot scripts correctly expect the loaded, idle #208 backend.
No #209 runtime result is claimed yet.

## #209 native validation complete; userspace integration next

AMD now runs #209, release 7.0.14-vmctx-audit-recall1-context209, boot ID
8e72b4dc-972c-47f6-83b2-b5dc029dbe91. Backend is loaded from
/home/biwu/audit-20260907/context209-native/vmctx.ko, SHA256
0c415b79dcc736eb18b96132958a4accb01203decad17ef7a805e5358c618e2e. All 23 native controls passed, including four descriptor modes
with the new credential/mutation negatives. mm_live=0, backend refcnt=0,
context_handles=0; take_lost=1 is solely the deliberate bad-copy TAKE. Live
kernel capture found no unexpected diagnostics and exactly the intentional
fault deadline. Evidence: context209-amd-native-1/complete.json and rows.json.
Build/package manifests verify all frozen inputs, 6891 modules, image and initrd.
No build, package job, reboot or native test remains active at this update.

Source patch 0032-vmctx-retain-source-context-identities.patch is exported from
the verified #206 predecessor (context207-before) to frozen #209. Applying it
with --fuzz=0 reproduces every #209 file byte for byte; context209-patch-check.json.
Shared support access/recall headers now match the source's resolved-task helpers.
They are source-specific; the 6.18 execution core does not use either helper.

Correction to the previous 6.18 note: inspection confirms its vmctx_mm_note takes
only mm, not the source rec flag. It does NOT have the #207 direct-SERVICE false
flag bug. Native source admission/descriptors are still absent there; adding them
must use its one-time syscall exit work and preserve its distinct execution role.
This does not block using #68 as the executor with a source-local #209 adapter.

Additional integration reasoning, not yet implemented: a proc handle may be
opened without a PID-reuse race by bracketing the open with successful native
context OPEN+INFO lookups and comparing both opaque context and MM identities
to the retained handle, then using the proc file only after validation. PID
or /proc/stat start-time comparisons are insufficient. PID reuse cannot return
the same monotonic native context identity; exec cannot return the same MM
identity. Proc memory/map handles retain the opened mm. A non-leader exec can
change native PID, so the descriptor INFO contract needs current native identity
metadata, or a native exact-task resource operation; the current native_pid is
saved diagnostic metadata only. Do not silently use it as a forever-current PID.

The page-settlement audit additionally confirmed that page_recall_all's snapshot
lease does not serialize page-server transfers. GET's pg_claim_wait result can
be negative and it still enters the take/protect body; it merely avoids settling
the incumbent afterward. UPGRADE likewise updates loans without a matching
claim check. INSTALLED still uses a 16-bit checksum as an episode identifier;
ack_flush ignores send failure, and disconnect cleanup can settle a newer transit
based only on address. These remain concrete protocol/ordering obligations, not
validated fixes. No userspace changes were made for these findings yet.

## #210 metadata query exclusion regression

A native regression demonstrated a dependency omitted by the original descriptor
controls: a monitor thread holds cpu_ctl_lock across VMCTX_CTL_SYSCALL while the
source task blocks in read(2); another thread's INFO returned EBUSY (errno 16).
A source fault service must be able to query its context/MM during such work.
Evidence: context-query-209-baseline.log. Cleanup was 0 MM / 0 backend refs /
0 handles, unchanged loss count and #209 boot. New control context-query.c
establishes the competing lock by observing GETCPU exclusion, not by sleeping.

#210 removes the outer CPU lock from CONTEXT. INFO only reads referenced task/MM
and published terminal state; transient retirement without an MM ID returns
EAGAIN. OPEN retains the task/vc without reading mutable ticket references.
Only CHILD takes cpu_ctl_lock, rechecks live monitor ownership there, and exports
the referenced birth before releasing that lock. It does not nest the lock.
Frozen context210-inputs core SHA
5412e9da32a9e727e0f7d2befa9362c0669fa902fe99ff46d0c360db0623ef45,
support SHA 6570dcc64560082421487179cd1dcf41a80f6ef37991067591ec527c49b92c08.
The full kernel/modules/backend and package passed all input/output hashes with
no compiler warnings. Backend SHA
4ad00504e50fa262f600671c1a7347e46ce9ddde90b5ad4eea2f3e03dc1bd674.
AMD booted #210, release 7.0.14-vmctx-audit-recall1-context210, boot identity
bf14bc1e-9264-48c2-bb7c-979703f5d83f. Its 25-case native run is IN PROGRESS.

Authored user/source-context.h owns native context descriptors and opens source
proc resources only after pre/post OPEN+INFO proofs compare opaque context/MM
identities. Ordinary MM files bind an mm at open; task-resolving diagnostic files
also require validation after reading. The group PID is captured before initial
release solely as a fallback name for non-leader exec, and that candidate passes
the same context identity checks. Neither numeric name is a lifetime key.
It is not integrated into vmhome yet. Native control source-context.c uses actual
new PID namespaces and forces reuse before and after the resource open, checks
fresh context/MM identities, then creates a native thread and execs it to test
context continuity, MM replacement and lookup at the new group PID. This new
control is compiled warning-free; its #210 runtime result is pending.

## #210 validated; source adapter resource checks

All 25 native cases passed on #210 in context210-amd-native-4/complete.json.
Counts ended [mm_live=0, backend refcnt=0, take_lost=2], handles=0. The second
lost-page increment and second deadline hit were deliberate controls in the
full rerun; per-case deltas exclude them from every other case. No unexpected
kernel failure matched the scoped live capture. Source patch 0033 applies to
verified #209 without fuzz and exactly reproduces all frozen #210 contents.

The first proc control exposed a real namespace mismatch: native ctl resolved
PID 2 in the test namespace while inherited /proc/2/maps addressed the host.
The helper now pins the proc root, verifies procfs, opens both self/ns/pid and
1/ns/pid and compares retained namespace device/inode identities. Comparing
numeric self PIDs alone would not prove namespace equality. A mismatched proc
instance fails EXDEV before any named task data is read; openat stays on the
validated root despite a possible later overmount. The control verifies this
rejection, then mounts proc only in its private test mount/PID namespaces and
forces PID reuse on both sides of open. Non-leader exec then preserves context
identity, changes MM identity and resolves the surviving task at its former
group PID. Executable dev/inode equals stat(/bin/true); no pathname spelling is
assumed (the host uses the gnutrue symlink target).

Preserved failed evidence: context210-amd-native-1 first 24 native cases passed,
then proc fixture read inherited host PID 2; run 2 passed PID reuse and reached
exec, then its maps substring fixture assumed /bin/true rather than gnutrue.
Run 3's control passed but log validation reused the full run's marker and
counted an older deliberate deadline. Run 4 has an exclusive marker including
the output directory and per-case deadline-counter deltas; its 25-case complete
manifest is the authoritative result. No failed run was relabelled as complete.

No kernel build, package, reboot or native test is active. AMD #210 remains
loaded and idle; Intel #68 is unchanged. Source descriptor/proc helpers are not
yet wired into vmhome. The native gate, MM identity and pending birth integration
remain the next production changes, followed by page-transfer ordering and the
broad suite. Frozen user3 remains 75/76; nothing claims all-green userspace yet.

## #211 native MM lifetime and exact-task cleanup validated

#210 source-registry control also passed in context210-amd-native-5 before this
continuation. Its reference/tombstone foundation has now been extended for the
production migration; that migration is not yet runnable.

#211 adds context ABI 2 while accepting ABI 1 OPEN/INFO/CHILD unchanged. ABI 2
exports current PID/TGID lookup candidates in the monitor's own namespace,
MMINFO queries a non-reused native MM identity (initial-namespace CAP_SYS_PTRACE
required), and SIGNAL targets the retained task without reopening by PID.
MMINFO reports native MM record references, including committed children before
userspace publication and in-flight page/recall operations. It never derives MM
death from a callback count. Disabling native MM reference accounting is now
read-only at runtime and makes MMINFO unsupported rather than false-terminal.

Frozen context211-inputs/core kernel SHA
46a22c5d0a4f02481ca8518f43f94b02078dc9805da03e1cdadf0bfcac9eb033;
backend SHA 65b2e54a29bcafadf649a0a70af2ed16524fef2642831b502187afc45c6b9cf3.
Kernel/modules/backend built warning-free; all 6891 modules, depmod, initrd and
hashes verified. One-time boot #211 is 6504c963-3482-4b07-948b-47b8dcdae3d1.
All 26 native controls passed: context211-amd-native-2/complete.json, counts
[0 MM, 0 backend refs, 1 deliberate lost-copy test], handles zero; scoped live
kernel logs and per-case deadline deltas passed. The new source-context-211a
control tests namespace-relative CHILD export, exec MM retirement, pending
CLONE_VM without CLONE_THREAD surviving parent death, exact-handle SIGNAL and
PID-reuse rejection, plus registry ownership. Run 1 stopped during harness
setup because the newly root-created destination directory was not writable by
scp's user; no native case ran there. The directory ownership was corrected and
the failed setup preserved. Patch 0034 applies without fuzz to frozen #210 and
reproduces #211 byte for byte (context211-patch-check.json).

Current user work is UNFINISHED and UNDEPLOYED: source1-before preserves the
pre-migration tree. vmhome now has source_id references and separate 64-bit MM
identities, retained controls/signals, validated proc resources, and immutable
MM lineage. source-address-space.h and source-lineage.h are new. Bootstrap,
registry claims, and service references are being wired. A syntax check passed,
but old child publication still needs the native gate and wire state machine;
no userspace success is claimed. No kernel job or native test remains active.
AMD #211 stays loaded/idle; Intel #68 unchanged. Frozen user3 is still 75/76.

## Production source migration and exit ownership correction

Source3 (23fe3546d1a3) integrates retained source IDs, native MM identity,
namespace-validated proc resources, native admission tickets, pre-completion
child export and opaque child tokens into wire v18. All nine host controls
passed. All six focused guest stdout/status checks passed, including the
previous ptrace-syscalls blocker, seven-second ptrace child hold, nested fork
I/O, and thread-stack reuse. Those results are NOT an integration pass: every
source log contains page-service failures after executor-local exit. The new
run-diagnostics.py regrade rejects all six, including nonzero loss counters.
Evidence: source3-focused-1 and source3-focused-diagnostic-regrade.json.

Source2 failed before guest startup because its syscall-number probe used a
negative selector as a nonexistent PID. Negative selectors now mean retained
FDs; source3 negotiates read-only SOURCE_EXIT_CAPS at selector zero instead.
Source4 failed -Werror on the now-unused legacy ctx_sys helper. Source5 removes
that helper and ctx_call, builds warning-free, and passes nine host controls.
Source5 ID e9a2be05814e; inputs and output hashes are in its complete manifest.

Both SVM and VMX interpreted Linux exit/exit_group before redirecting calls.
They now publish every SYSCALL frame to the source, including exits. A source
exit must run native entry policy and exit memory work while page service is
alive. FINISH waits for the retained native terminal record before ACK; an
unfinished source task is cancelled through its retained FD, not given a
fabricated exit syscall. The executor root result now uses source terminal
status as child results already did. A numeric root waitpid comparison was
also corrected to compare the native PID rather than the source token.

Frozen exit2 module C SHA 5047c86dae63f2614c79de16cd902a1afe6fe72e7732e69de03401a4ee87fcb2.
AMD module dc0c5142a70929ba9998b9a99aae86abee601c219af47c6eacac7625d31ffbef;
Intel module e864c93dfb995d1e42f66bd7a65dac64b20748b5d055c71a7e6ba67a3b1bbe61.
Both builds verified unchanged kernel sources/config/symbol/image hashes and
had no warnings. The first freezer incorrectly included generated vmctx.mod.c;
its post-build hash guard rejected the attempt, and no module was deployed
from it. exit2 excludes generated module C. The first new native exit-forward
control omitted CPU_MODEL setup and was correctly rejected before guest entry
with EPROTO; exit-forward-2 includes the required negotiation. On AMD it now
observes and resumes both exit identifiers; native ptrace/seccomp exit policy
also passes. Full native rerun is still in progress at this ledger entry.

## Exit ownership validated; broadened source6 results

exit2-amd-native-2 completed all 28 controls with zero residual MM/module/handle
references; exit2-intel-native-1 completed 17. Both scoped kernel logs are clean;
intentional TAKE copyout loss and elapsed fault deadline each increase only in
its positive control. Current AMD/Intel cumulative take_lost is 2 each and
fault_deadline_hits is 2 each. Boots remain #211/#68 with unchanged boot IDs.
Both hosts now have the exit2 modules loaded (paths exit2-native/vmctx.ko on
AMD and /root/audit-exit2/vmctx.ko on Intel).

Source5 exit-source integration produced correct ptrace/seccomp results with
clean exit memory service, but strict grading found three uncounted forwarded
terminal syscalls. Source6 counts source terminal completions as forwarded and
removes a misleading claim that missing counter entries ran on the executor.
Source6 ID b98e21ac45e5; vmhome SHA
436322ca1032531ba7bebcc829d8f0892a759f3e9e818c409ff4aa3d6a5a46b8;
vmremote SHA 74576c685b04e2c6685ca417c808775d43c489d2f6fa57689d28c5e06ac0417a;
local SHA 341493de714ae57d8b07266916f59f1023859c91f131baaee48245b4423fa9ce.
All nine host controls and seven focused guest controls passed. The focused
oracle rejects fatal source/executor diagnostics and checks every case's
native counters, cleanup, binary/input hashes and boot identity.

The full source6-suite-1 run is 72/76, no kernel diagnostics, no lost pages,
no fault-deadline delta, no residual MM/module/context handles. Four failures:

- pg3 reached 61,201 forwarded calls at the 60-second harness deadline, with
  122,498 serviced faults and no progress gap. Cancellation then closes page
  service and its subsequent failures are consequences of the timeout. The
  existing test could stop worker B early and ignore its completed round count.
  The repo now requires all 20,000 rounds on BOTH workers and checks join.
  Native strengthened control passed. source6-pg3-budget-1 with a 120-second
  budget PASSES all 40,000 rounds, zero behind/ahead/short and clean diagnostics.
- fx1 fails after actual vfork (nr 58) child publication. The child shares the
  executor's backing/address-space lineage with the parent; its exec_wipe resets
  the shared access cursor and memory metadata. The parent's next ACCESS_READ
  (wire op 4113) returns -116 ESTALE. The displayed EAGAIN at fwd_broken is stale
  errno, not the source response. Treating this as retry would hide an identity
  error. Needs explicit MM transition and a fresh executor backing binding;
  never rewrite native vfork as fork or clear a parent's object on child exec.
- ns1 and group-exit fail settlement of a loan with PULL_CLAIM (3), during an
  admitted clone3. This is an active transfer, not absent data or task death.
  Needs ordering/reservation work, not a success fallback or discarded loan.

Repo-only harness edits after source6 freeze: owned-run.py supports a monotonic
execution deadline and pidfd cleanup; its detached-child/interruption/deadline/
unrelated-process controls pass. local-here.sh removes global name-based kills
and module reloads, and replaces post-reap numeric group cleanup with the nested
owned runner. runany.sh now invokes run-diagnostics.py; suite.sh includes the
new exit-source regression. These changes are not yet in a runtime candidate.
No broad cross-machine or browser validation has run with the new wire yet.
