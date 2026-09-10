# Context and address-space lifetime in admission integration

This is a design review record, not an implemented or validated API. The live
source is #206, native gate ABI 2; wire16 still does not use native admission.

The source currently caches PID -> TGID in as_of() forever. That conflates task
identity, thread groups and mm lifetime. Native vfork and non-thread CLONE_VM
share mm without sharing TGID; exec creates a new mm and can leave non-thread
CLONE_VM peers in the old one. Numeric PID reuse can also match an old entry.
The source access journal already exports a monotonically allocated mm trace
identity. It should name an address space; a separately validated live context
must be selected whenever a native operation needs a task.

Relevant code paths to change together:

- vmhome as_of/as_tab/as_live_ctx, as_life_ref/unref/has_ended.
- ctx_monitor caches as once for its entire life. Rebind on actual mm change;
  do not mark a shared mm ended just because one service thread finishes.
- fork_rel stores AS keys but returns them as PIDs. Its parent/children helpers
  feed native page operations and ancestry walks. Preserve lineage identities
  independently of the availability of a live task representative.
- fork_rel_forget assumes pid==as. Exec needs to drop the old mm's relationship
  only when appropriate; one leaving thread must not erase its siblings' state.
- CHILD_PAGES compares the parent's AS key; ctx_write_prep/ctx_page_inner and
  source fault service also consume fork_rel results.
- conn_is_thread is TGID membership for native teardown; do not replace it with
  mm equality. Mapping sharing comes from native gate child_shared_mm.
- The HOLDERS diagnostic currently uses /proc/as_of(pid)/task. Enumerate the
  correct thread group or registered mm members, without treating an AS key as
  a PID. Tests must cover native CLONE_VM without CLONE_THREAD and exec split.

There is also a task-lifetime issue to resolve before calling the new wire
contract complete. Gate QUERY currently looks up the parent by numeric PID and
requires a live target->vmctx. If the parent dies after kernel_clone has created
a child but before the monitor polls the metadata, normal teardown drops that
lookup and the child publication can be lost. This follows from the current
lookup/teardown paths; a focused parent-death regression has not yet run. Keeping
only a user PID cache does not make this durable and check-then-use does not
prevent numeric PID reuse between checking and issuing a native control.

A concrete candidate design for review is a source-local context descriptor:

1. A descriptor retains the exact task and vmctx krefs; controls on it cannot
   resolve to a reused PID. Preserve monitor-process authorization independently
   of the active vc->monitor, so read-only completion/death queries can work
   after vmctx_finish drops the active monitor.
2. Ordinary controls require a live context. Read-only gate/death queries may
   inspect the retained terminal state. Preserve native exit status and committed
   child metadata even if the parent never reaches COMPLETE.
3. kernel_clone captures references to the child task/context before wakeup,
   alongside the already recorded native PID and actual mm sharing. A monitor
   query can export a child descriptor while executing in the monitor's own fd
   table. Do not install a monitor fd from the source task's fork hook.
4. Descriptor allocation/copyout must not leak on EFAULT or create different
   children on retry. Hold the birth record until publication is acknowledged
   or the owning connection ends. Release references on next-ticket retirement
   and descriptor close with a bounded lifetime, not a permanent global list.
5. ACCESS_LOG/RECALL currently bypass the main resolved-target path and do their
   own PID lookup. They must use the same retained task/context identity rather
   than quietly retaining a PID race underneath the new descriptor API.
6. Wire context IDs and AS IDs remain opaque monotonic lifetime identifiers;
   neither native fd numbers nor namespace PIDs should be reusable wire keys.
   Native PIDs remain diagnostic metadata or guest-visible results where needed.

A descriptor-backed ioctl, or a carefully specified negative selector in the
existing local ctl entry point, can avoid a new syscall number. The latter
would require centralized target resolution and auditing all special control
paths. No option has been authored yet; do not treat this proposal as completed.
Useful native controls: fd lifetime/close/type checks, bad-copy allocation,
parent death between fork event and publication, child death before readiness,
repeated query/commit, descriptor access after task teardown and MM change on
exec. The native ABI 2 tests currently cover ordinary fork/vfork tracing and
raw-versus-visible return results, but not these additional terminal races.

## Candidate #207 authored, build underway (not validated)

The source-only candidate now implements local descriptors with ctl 46,
`VMCTX_CONTEXT_ABI=1`, in kernel/vmctx_context.h and the support header
vmctx-context-core.h. OPEN resolves a positive native PID once. A subsequent
native ctl selector `-(fd + 1)` resolves the retained task/context, never a new
PID lookup. Files are close-on-exec, restricted to their opening monitor's exact
referenced TGID, and still pass the native ptrace credential check. INFO returns
an opaque monotonic context identity, stable diagnostic native PID, current MM
trace identity, initial readiness or retained terminal status/MM identity.

The native gate retains the committed child task and vc before wakeup; CHILD
exports that ticket's retained child into the monitor's fd table. Gate QUERY and
INFO/CHILD remain available through a descriptor after parent teardown. Ordinary
state operations refuse a dead or replaced vc. Repeated successful OPEN/CHILD
creates independently owned descriptors; failed copyout installs none. Birth
references retire on the next BEGIN or the final parent vc reference. RCU defers
final vc retirement so a long retained birth chain cannot recursively exhaust
kernel stack. A read-only vmctx_context_handles parameter measures open handles.

ACCESS_LOG and RECALL operate on the already resolved task; descriptor controls
cannot fall back to a numeric PID lookup. The zero-selector global access probe
and recall completion contract remain unchanged. The candidate also serializes
backend destruction with cpu_ctl_lock after publishing death/waking waiters,
and rechecks death under that lock before allowing a state operation to proceed.
It obtains an mm reference when binding its monitor rather than passing an
unreferenced target->mm. Failed-clone vc retirement drops an inherited monitor
reference as well.

Candidate core is `context207-work`, immutable build inputs `context207-inputs`.
The exact predecessor `context207-before` includes the previously unchanged
recall helper, whose SHA256 was verified against the running source tree:
e29c3b2f578c53a2e7680f1530929c5fdf9dc330e2edbace3c8d3920541466f4.
Do not edit frozen inputs or overlap another kernel/module build/package/reboot.

New native control `tests/control/context-lifetime.c` covers parent-first death,
child death/reap before export, and true vfork; it also checks descriptor type,
foreign process denial, duplicate identity, close-on-exec, bad-copy fd counts,
exact MM identity, retained final status and rejection after close/death.
It compiled warning-free as context-lifetime-207. The #206 baseline rejects OPEN
with EINVAL and cleans up; `context207-native206-baseline-b.log`. The earlier
baseline command named the wrapper without its controls/ directory and did not
run a test; its log is retained. Candidate runtime tests have NOT run yet.

This source-local lifetime API does not itself fix the wire protocol, source
as_of cache, non-thread CLONE_VM/exec sharing, portable termination encoding,
or the page-settlement concurrency issues listed above. The 6.18 port is also
still outstanding. Those obligations remain after native descriptor testing.

## #207 boot observations; #208 fixes built, packaging

#207 booted as 7.0.14-vmctx-audit-recall1-context207, boot identity
e36d5a75-f085-45c0-a367-dc16ffcbb179. Its backend was NEVER loaded. The new counter
used module_param_cb and created /sys/module/vmctx/parameters, occupying the
loadable backend's sysfs name. Boot validation caught the wrong location; no
insmod was attempted. The correction is __core_param_cb (core_param_cb without
the leading underscores also adds the source filename's prefix and is wrong).

Source-only testing requires no hardware module, so six native cases were
attempted with a control variant that reads the actual #207 counter path.
Gate ABI 2, traced fork and traced vfork passed. The first context lifetime
control failed before native fork: INFO reported the right id/readiness but
mm_identity=0. Instrumented repetition confirmed id=4, expected=4, ready=1,
mm=0. Both failed runs cleaned up 0 contexts/0 handles/0 lost pages.
Evidence: context207-source-native-1/2 on AMD, matching local driver logs.
The tests have not yet reached terminal child publication and do not validate it.

Inspection found an existing direct-SERVICE setup bug: vmctx_run_current always
called vmctx_mm_note(..., false), while ADOPT and child setup identify source
mm correctly. The initial direct service was consequently absent from the
source ownership/access journal. #208 passes the actual SERVICE flag. Its
context MM identity lookup also uses the mm lifetime table independently of
whether a page-ownership record is enabled. Canonical tests still require a
nonzero MM identity; they were not weakened.

Immutable context208-inputs now built without warnings (full kernel/modules and
backend), core SHA e8ac5fb9e2aabfcb51262ed8d27985d399bb938903b5f9a014928814c74a1c22.
Module SHA 3aaf4a8a05a55132cfcaa270416de65afaec6369d78bfddac3427ffcee1c403a,
release 7.0.14-vmctx-audit-recall1-context208. Packaging is in progress. Its
boot driver intentionally does not rmmod #207, which has no loaded backend.
Canonical context-lifetime-208 also adds forced PID reuse inside a fresh PID
namespace; no host PID allocator is changed. That fourth mode is unrun. Native
harness context208-amd-native.py will run nineteen established controls plus
four descriptor modes, with handle counts checked around every case.

The 6.18 direct-SERVICE setup has the same unconditional false flag; fix it when
porting the source admission/lifetime work. It is not changed in the working
6.18 kernel yet. No 6.18 source descriptor API or wire admission is implemented.

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
