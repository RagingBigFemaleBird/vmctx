# Executor address-space lifetime correction (in progress)

The source6 fx1 failure is an identity failure. An actual native vfork shares
its parent's MM, but the child's later exec binds only that task to a new MM.
The executor currently keys MM state by a native context PID and clears every
member of its old address-space group in exec_wipe. It punches the parent's
backing object and resets the parent's access journal cursor. The ensuing
ACCESS_READ ESTALE must not be retried as a transient error.

Required invariants:

1. A source context token, source native MM identity, execution task handle,
   and backing object are distinct identities. Native PID is only a lookup
   candidate. No old MM key may become the new MM of a task that execs.
2. Each MM has its own immutable identity, backing object, lineage, journal
   cursor, layout, ownership, retained bytes and page generations. Rebinding
   one context does not destroy these for remaining old-MM users or descendants.
3. A completed native birth publishes the child's actual MM identity. Shared
   MM does not mean shared native thread group. A group leader can exec while
   another CLONE_VM, non-CLONE_THREAD process continues in the old MM.
4. Native exec may access memory before its completion reply. Source page
   requests must name the MM they access; connection/context identity alone
   is insufficient. Every page reply, landing and ACK needs the identity of
   the same MM/transfer episode. No request that began on the old MM may land
   into a context's new MM just because its context token is unchanged.
5. Rebinding excludes old-epoch page operations across their whole operation,
   not merely an individual native POKE or map call. Waiting while holding
   coherence/claim locks that a source callback needs is forbidden. A blocked
   parent vfork must not hold a lease that prevents its child completing exec.
6. A missing native mapping, context exit, an active claim, and a retired MM
   are different facts. Retire state only from the corresponding native proof.

## Native object binding candidate #212

Frozen inputs are object212-inputs under the recovery audit directory.
AMD #212 now passes 29 native controls; Intel #69 passes 18. Production
MM registry/protocol integration remains pending.

The new OBJECT control retains struct file references independently of the
execution task's descriptor table. Initial bindings are retained before context
publication, inherited by reference, and dropped at final context retirement.
The backend fault mapper, reply mapper and PROTECT_BACKING use those references.

OBJECT REPLACE requires an existing private object, a distinct writable shmem
object, a monitor-owned execution stop, and an expected native binding epoch.
The CPU control mutex excludes resume/state changes, an object rwsem drains
individual executor memory controls, and final backend destruction takes that
rwsem too. Source service operations and WAIT do not take it. Lock order is
CPU control mutex -> object rwsem -> native mmap/folio locks. File reference
acquisition uses a short separate spinlock; no blocking operation runs under it.

The stopped-task predicate rejects native MM faults (which can hold mmap_lock),
assisted native calls, pending replies, runnable/on-CPU tasks and exiting tasks.
MM replacement requires exactly the target's MM reference plus the control's
own get_task_mm reference. An additional task or lease yields EBUSY before any
unmap. Under mmap_write_lock it removes only mappings of the old private or
shared objects, matching address_space identity even when an FD was reopened.
It never punches either old file. A partial unmap failure kills the target and
never reports success. The new binding is published only after complete removal.

Epoch+object identity allows a failed result-copyout to be retried without
unmapping already installed new-image pages. Reopened descriptors for the same
object are recognized; a stale epoch naming a different object is rejected.
This primitive does not by itself fence multi-control distributed transfers;
its ABI states the caller's requirement to exclude old-epoch operations.

The object-binding native regression compiles warning-free. It covers two
native MMs using the same old objects, an alias descriptor mapping, an additional
MM lease, bad result-copyout followed by actual guest reads, idempotent retry,
stale-object rejection and running-context rejection. It passes on both #212 and #69.

## Other validated and pending work

source6-pg3-budget-1 artifacts are retrieved locally. Strengthened pg3 finishes
both 20,000-round workers in 77.020 seconds, all 80,031 calls forwarded, no fatal
logs or native counter delta. Its suite default is now 120 seconds; explicit
VMR_LIMIT still overrides it. fx1/ns1 stop flags now use C atomics, joins are
checked, and waitpid retries EINTR. Strengthened fx1 passes natively on the local
host; these changes are not yet in a frozen integration candidate.

Remaining runtime failures are fx1, ns1 and group-exit. The last full source6
suite was 72/76. Page settlement still fails closed on PULL_CLAIM. Page transfer
claim/ACK ordering, executor MM registry/protocol integration, source lineage
bounds, cross-machine suite, program breadth, and GUI validation remain open.

## AMD #212 native result

Full kernel and 6891 modules built/packaged without warnings. Kernel core SHA
b929c7ed8a51d3a448be6ce135bb40d87d0471a1383a4ce173640aa2e7485e89; module
29913ba6e6e593b90cc2178dd150e6feec0663e5dd0171b2dd631166c043a93c. One-time
boot b67acc87-63c2-45e4-8564-8b2244ad0d40 is release
7.0.14-vmctx-audit-recall1-object212. Default GRUB and prior images preserved.
object212-amd-native-4 passes all 29 cases with clean scoped kernel logs and
zero residual MM/backend/context references; deliberate take loss and fault
deadline each increment exactly once. object-binding-212d is authoritative.

Preserved fixture failures: 212a declined a newly backed page with SELF, which
correctly undid its VMA. 212b uses MAPOBJ to install the existing retained folio,
but waited for a child while still holding its syscall event. 212c instruments
every control and proves all storage assertions completed before that teardown
hang. 212d answers outstanding test events and kills both tasks before reaping
either. No kernel change was made between these runs.

Intel candidate #69 built and packaged from frozen object69-inputs, with
all 3767 modules verified in a separate case-sensitive module directory. It also serializes
backend teardown with CPU controls; the old #68 core lacked that exclusion.
The shared object helper conditionally lists source-only controls absent from
the Intel core; AMD #212 frozen inputs are unchanged.

The new exec-shared-mm guest regression passes all four native shapes including
raw x86 vfork with no C child stack mutation. It is now included in suite.sh
(78 cases). The source6 baseline on AMD #212 fails as expected: status 97, ACCESS_READ
ESTALE on syscall 61 after the first shared-MM child exec. The scoped kernel
log and native counts remain clean. Artifacts: source6-exec-mm-baseline-1.

## Intel #69 native result and recovery service

object69-intel-native-1/complete.json records all 18 cases passing on boot
 a9a25b4b-70fe-4cd5-a554-e95b856217b5, release
6.18.35-0-lts-audit-object69. Module SHA
97843ddaf9ee0f25fbf28db215ac2177c20f3e085ce4b3f98d93816ff6b64143.
Live kernel capture spans module load and every case, with panic/watchdog checks
enabled. MM/backend references return to zero per case. Deliberate bad-copy
loss and fault deadline each increment exactly once and only in their case.

The prior recovery HTTP server was absent; PID 239803/session 89055 restores
port 8080 on the same D recovery http root. All 105 boot dependencies were
verified from Intel before loading the kernel. Missing kexec and then liblzma
stopped earlier attempts before module unload; those failed results are retained.
Installing kexec-tools and xz-libs from the official Alpine v3.24 repositories
made kexec --version succeed. The successful request/result and script are
object69-intel-kexec-*. Neither default recovery boot configuration was changed.

## Transfer exclusion review in progress

page_recall_all does not hold a per-page transfer reservation across its pull,
landing, acknowledgement and loan retirement. The snapshot lease stops native
execution but leaves page workers active. get_have_page also continues into
TAKE/protection when pg_claim_wait failed, merely skipping its final settle.
PULL_CLAIM cannot be treated as a settled loan or retried while excluding the
source GET that needs to complete it. The local claim API has no episode token,
and legacy UPGRADE/PUT handlers mutate records without a claim; the current
source does not send those operations. These findings require implementation
and failure-injection regression coverage before another integration candidate.


## Transfer1 and transfer2 measured results

Transfer1 (c646238fd94e) adds exact local page reservation episodes and places
GET/GETS/COWBREAK and snapshot settlement under those reservations. Incumbent
claims survive failed acquisitions; delayed finishes cannot clear new episodes.
Failed ACK sends terminate the connection's owner instead of discarding pending
receipt state. Ten host controls and focused ns1/group-exit controls passed.
Its full suite passed 75/78. The two exec/MM failures remain; pf4 exposed an
existing lock cycle now enforced by the server's new claim: source signal-frame
construction needed the same stack page held by the waiting fault dispatcher.
All 78 cleanup records complete with no survivors; pf4 timed out and added ten
native fault-deadline hits. No kernel crash or lost-page count occurred.

Transfer2 (228d29d65114) makes the inner page service return a distinct request
for source exception handling. The wrapper flushes a pending landed-page ACK,
finishes both local claims, clears pending map metadata, and only then dispatches
the native source exception. A legal-access answer starts the page service again
under a fresh reservation; no old local owner/storage observation crosses the
callback. Handled/failed exception replies never resume that old inner frame.
The actual wrapper is in user/fault-dispatch.h. Its host control makes a second
thread reserve and write the same memfd page during dispatch, then verifies the
legal-access restart reads the new bytes. All eleven host controls pass.

transfer2-focused-1 passes pf1, pf2, pf3, pf4, ns1, and all eight group-exit rounds
with stable boot, zero reference residuals, clean scoped kernel logs and no new
native deadline/lost-page deltas. Frozen inputs are transfer2-user-tree and its
input/build manifests. Full suite attempt 1 never started a guest: port 32800
exceeded suite.sh's allowed range. Attempt 2 runs at 32400 using verified prior
compiled guest hashes. That full result is still pending.

These changes do not make the distributed page protocol complete. Native source
TRANSIT still uses a 16-bit content checksum and time-based retirement; ACKs and
connection cleanup still need explicit episode ownership. Legacy PUT/UPGRADE
handlers remain unreserved, although the current source sends none. Readahead,
COW lineage, and all caches still need explicit immutable MM/transfer identities.

## Retained native execution handle candidates (unvalidated)

handle213-work and handle70-work extend the exact-task/context descriptor API
to execution contexts. CAPS distinguishes supported source and execution roles.
AMD keeps the previously validated source API; Intel advertises only execution
handles because it has no native source admission records. INFO remains readable
through owned descriptors after backend teardown and task reaping. All mutation
paths reject ended lifetimes. Live operations still check ptrace permissions and
monitor ownership; descriptor ownership holds a referenced monitor TGID, and a
control obtains its own task/context refs before releasing the descriptor lookup.

Native execution INFO reports context lifetime and MM registry identity, not a
process wait status. Its exit_status is explicitly zero; the native parent uses
wait for actual task termination. This avoids attributing an early context finish
to a native process exit that has not happened. Source status semantics remain
unchanged. Intel publishes terminal MM identity with release ordering before
readers may observe dead. The existing CPU/object teardown locks exclude memory
operations while backend destruction proceeds.

New execution-context control covers two independent descriptors, bad copyout,
inherited-FD refusal, GETCPU through the handle, source-only CHILD refusal,
retained terminal/MMINFO queries after reap, signal-by-handle, and forced native
PID reuse in a fresh PID namespace. object-binding gains a handles mode so actual
WAIT/RESUME, CPU controls, MAPOBJ and replacement/replay exercise FD selectors.
Both compile warning-free; neither has run on a supporting candidate yet.
Frozen inputs are handle213-inputs and handle70-inputs. Intel #70 is building;
AMD #213 awaits the end of the AMD guest suite and the serial native build gate.

Remaining native review findings: Intel still has historical in-band clear-TID
handling; production execution contexts should not own native source futex state.
Native MM registry allocation still has fixed capacity and unchecked trace-ID
exhaustion. These are not solved by descriptor lifetime and must not be mistaken
for validated MM retirement guarantees under those exceptional conditions.

## Transfer2 completion, descriptor inheritance and Intel #70 boot

transfer2-suite-2 completed 75/78. pf4 now passes without deadline hits. fx1 and
exec-shared-mm still expose source MM identity loss. an1 reached 34,943 calls
before its 60-second budget; the strengthened atomic-stop, checked-join and
named-mapping control completed all 20,000 rounds in 56.636 seconds with a
120-second budget. Its native control also passes. Both runs preserve zero new
native deadlines/loss, stable AMD boot and fully completed cleanup. suite.sh now
gives an1 120 seconds by default, retaining explicit caller budget overrides.

The actual drop_monitor_fds regression demonstrates a living child retaining a
monitor pipe above fd 4095. Native close_range excludes only the two memory
objects and standard descriptors, including high descriptors and duplicate
keepers. backing_new now uses MFD_CLOEXEC and duplicates low descriptors above
2. Both actual production regressions fail before and pass after the repair
(exec-fds-after2 and exec-fds-stdio-after2). The first after compilation failed
on a discarded write result under -Werror; the diagnostic path now handles that
result, and the successful after2 compilation is warning-free. No runtime
candidate containing this descriptor change has been frozen yet.

Intel #70 built and packaged all modules, checked all 105 network boot inputs,
and booted as 871c548a-81de-4730-8e19-4207f0fcd5aa. Its 21 native controls are
in progress. AMD #213 is frozen and unbuilt. These handles are not integrated
into production user MM identity handling yet.

## Retained handles validated; MM registry components

handle213-amd-native-1 passes 32 cases; handle70-intel-native-1 passes 21.
AMD #213 boot is 77cbb552-3543-4567-b110-f9826b807446. Both full campaigns
return MM/backend/handle counts to zero after every case, and increment loss
and deadline exactly once each only in their deliberate controls. Frozen
0036/0028 patches reproduce the reviewed inputs. Recovery defaults unchanged.

execution-mm.h separates immutable source MM identity and ancestry from mutable
per-context storage bindings. Each MM owns a separate object; records/objects
remain for the session until workers join. The source declares ancestry; an
unknown parent or contradictory second publication fails. Hash buckets have
no cardinality cap. A transfer saves its explicit MM view before a callback;
its later object landing still targets that record. A short binding read guard
covers each native mapping operation, while rebind takes the write guard and
changes only that context. Neither guard crosses a network/source-work wait.
The host control covers matching low 32-bit keys, failed allocation, idempotent
and concurrent publication, 1100-generation ancestry, surviving shared-MM peers,
and a delayed old-MM transfer across rebind.

linux-execution-object.h adapts the native OBJECT replacement. A bad result
copy can follow a successful commit; this is not an ordinary unchanged failure.
The adapter replays the same object/epoch while retaining the exclusive binding
guard, validates the native inode/device/epoch result, and refuses unresolved
commit state. object-binding-mm1 registry mode exercises real kernel copyout
failure, replay, retained native maps and a late old-MM write on both hosts.
The late old-MM control is rejected before the native adapter; PEEK confirms
new context bytes remain 33 and the surviving old peer receives 77.

AMD focused evidence: mm-registry-amd-native-1/complete.json. Intel's native
control passed but the template reused the preceding full run's kernel marker;
its grader included the older deliberate deadline. The exact final marker pair
contains no diagnostics, counters have zero deltas, and regrade.json preserves
both the failure explanation and record boundaries. Subsequent driver markers
include the unique output directory name. No kernel change or extra runtime
retry was used to make that result green.

These components are not integrated into production vmremote. Source memory
controls also need native expected-MM checks: INFO followed by a context-based
PEEK/TAKE/PGACK can race exec. Existing RECALL pins its MM after BEGIN, but BEGIN
has no expected-MM input. A source-local envelope must verify the expected MM
against the exact referenced mm used by each operation; a userspace precheck
cannot provide that guarantee. Execution binding guards solve the corresponding
executor object-epoch race, not this independent source-native race. No native
MM-fence candidate has been implemented/frozen yet.


## Source expected-MM candidate #214 built (native validation pending)

The new memory-mm regression pauses an actual kernel inner-argument copy using
userfaultfd, admits and completes source exec while that copy is blocked, then
resumes PEEK. On #213 the old MM metadata has already retired and PEEK returns
zero bytes from the replacement MM. memory-mm-legacy-1 preserves this expected
failure with complete zero-reference cleanup and no new loss/deadline counts.

MEMORY command 48 captures the task MM and verifies the expected full-width
identity before consuming the inner argument. It pins both mm_users and the
source metadata record, and passes the captured MM explicitly through every
allowed helper. No exec lock or ambient identity override is used. The copy can
wait while exec proceeds; its eventual result still describes the old MM.
RECALL BEGIN takes its independent ticket pin before the envelope releases its
own pin. CPU/lifecycle/executor-only operations are refused. Wrong-MM calls
must return ESTALE before even reading a bad inner pointer.

mmfence214-inputs are frozen with core SHA
87d4e1aee662dd92a18ca06d7721b777f8bff69a8c3d780769f5f5a6095c80ed.
Kernel/full modules/external module built without warnings; packaging is active.
Patch 0037 exactly reproduces all 18 core hashes from the before snapshot,
without fuzz or offset. New module SHA
8770d005c2eb5d3b05a1e0949ce623fa7500de8eb4ab4cfdadc288c80e7770cc.
No boot or supporting native result is claimed yet. All 13 current host
controls pass. Production source/executor MM and wire migration remains open.


## Source fence #214 validated

AMD #214 boot 90e1489a-bf8f-481a-bedd-12f2cc380f18 passes all 34 native
controls. The paused PEEK reports old_live_during_copy=1, returns 8 and the
original 0x123456789abcdef0 bytes; the old MM retires after the call. The
full scoped kernel log is clean. Each case ends with mm/ref/handles=0; only
the deliberate TAKE copyout and fault-deadline cases increment their counters.

The new source-memory.h adapter and source_record_memory retain the caller's
expected MM unchanged through a borrowed context reference. source-context's
selector arithmetic now also represents fd INT_MAX without signed overflow.
The native regression memory-mm-214b uses these actual helpers and the retained
source registry; memory-adapter-amd-native-1 passes with no counter deltas.
Production source memory call sites still require explicit operation identities.
The native input snapshot is untouched; canonical support headers now match it.

Runtime migration must distinguish immutable operation targets from the context's
current binding. Each operation saves its source MM before any callback; later
controls either use a binding of that exact MM or return stale, and direct
object writes retain the old MM object. A completion or page request from an
older context epoch may find its old target, but cannot rebind a newer context
backwards. No binding guard can span source/network waits. Native PIDs remain
local adapter details; context, MM and operation/transfer identities must not
be interchangeable numeric keys. The current production caches still truncate
MM keys, and the next actual-table regression measures those aliases directly.


## Production cache identity and COW storage controls

The production loan, generation, retained-page, COW, layout, construction and
journal cache keys now retain the full 64-bit MM identity. The actual-table
regression uses two identities with equal low 32 bits and fails all ten checks
before the change; every check passes afterward. This does not yet change
as_id(), which still derives legacy context/PID lineage, or complete routing.

COW given/protection records now share dynamically allocated chained records
under one mutex. No collision or record count silently discards state. The
old code loses 64 of 128 colliding records and 65536 of 131072 capacity records;
the new code preserves every record. Retirement isolates the exact MM; eight
concurrent workers and allocation-failure controls pass. Allocation failure
ends the monitor before it could acknowledge state that was not recorded.

Source 605a410e8402 builds vmhome, vmremote and vmremote-local warning-free,
passes 13 host controls and all 36 actual-table controls. Every table cleanup
record is complete, status zero, with no timeout, error or survivors. The
check-tables Makefile target retains executable, logs, hashes and cleanup
records. Frozen source/build evidence is cow-cache-validated-user. No runtime
candidate containing these changes has been deployed; full integration still
requires immutable operation identities and replacement of exec_wipe.


## Production execution handle routing and native proc identity

The execution runtime now retains a vmctx context descriptor and a pidfd for
 each registered task. Ordinary controls, including the MAPOBJ/TAKE/TAKEOBJ
snapshot wrappers, select the descriptor. The monitor-owner thread performs
initial native attach only for a fresh owned, unreaped child; ordinary ctl no
longer contains a numeric ATTACH exception. Root registration precedes all CPU
state installation and entry release. No registry lock spans a native control.
Signals and task-exit observations use the retained pidfd, including the interval
between backend teardown and native task exit. An unreadable /proc stat is no
longer treated as proof of death. Descriptors remain until the session ends;
children close inherited monitor descriptors through the previously tested helper.

The production proc adapter permits only MM-bound maps/pagemap/mem files. It
pins the proc root, proves that its PID namespace matches the monitor, and
validates the retained context identity, current native name and MM identity
before and after open. Every executor pagemap/map read now uses that adapter,
including diagnostics and the legacy exec walk. No file bytes are consumed
until the second identity check succeeds. This fences native task/MM lookup;
object replacement within one native MM still needs the execution binding guard.

execution-routing.c includes actual vmremote.c, rather than duplicating its
routing logic. It covers unpublished-context refusal, duplicate registration,
retained control/exit, forced native PID reuse, old snapshot controls and
signals, replacement during proc open, and a mismatched proc PID namespace.
exec-route1 has two native passes on each host, all 13 host/36 table passes,
and seven focused guest passes (pf1/2/3/4/5, ns1 and group-exit) with stable boot,
clean kernel logs and zero new loss/deadline/reference residuals. Its source ID
is 0bfbad734192. The frozen exec-route3 revision adds the proc adapter and removes
the ordinary ATTACH escape; all four AMD native cases and all 49 host/table
controls pass. Intel's initial route3 launch failed at SSH preflight, before any
native control; its unchanged #70 boot was verified through AMD, and retry is
in progress. The full route3 guest suite has not started yet.

Frozen evidence: exec-route{1,2,3}-inputs and corresponding input manifests;
exec-route1-user-tree/build manifest and retrieved exec-route1-focused-1;
execution-routing-{amd,intel}-native-* and the retained launch-failure record.
Route2's Intel executable was compiled but not run. Native kernels and modules
are unchanged from the validated #214/#70 pair.

The runtime still uses legacy numeric context keys and a 256-record limit,
rejecting a reused PID before publication. Retained control routing makes that
refusal safe; it does not complete monitor-local context-token allocation, MM
identity, immutable operation targets, source memory call-site fencing, or
transfer receipts. exec_wipe still invalidates a shared old MM and must be
replaced with a single context binding change before fx1/exec-shared-mm can pass.


## Runtime migration boundary after retained native routing

The remaining runtime conversion must carry an immutable operation target
through the call graph. A target contains a stable context reference plus a
copied execution_mm_view and the source binding epoch. Native controls validate
that saved view under the binding guard. Object reads/writes name the saved
MM's backing FD directly. A context's current binding may change while a
request waits for source work; recomputing as_id(context) afterward is invalid.
Do not implement an ambient TLS MM override or hold a binding/coherence guard
across a network wait to avoid changing function signatures.

Context IDs should be monitor-allocated tokens, independent of native PIDs.
The remaining native creation boundaries are the two clone/attach paths; the
remaining native reap boundaries are child_thread and main (plus failed initial
attach cleanup). These must translate through the retained native adapter.
The initial backing record must be ready before publishing a context, and
as_members must snapshot context views for one exact MM. A stale member can
be skipped only after its changed binding or ended lifetime is established;
a control error alone cannot establish either fact. Retaining the old MM's
object does not require retaining a native executor whose binding has changed.

The source publishes context/MM/parent-MM/binding-epoch on root readiness,
child birth, completion and native memory requests. Each request/reply/ACK
carries its saved MM and transfer identity. Native source memory controls use
source_record_memory with that identity; selecting a live representative is a
separate operation and never changes the requested MM. Historical bindings
must remain answerable to delayed operations, and an older publication must
not move the context's current binding backward. source_record_info currently
caches monotonic observations but returns the raw concurrent query; binding
publication needs a synchronized snapshot of the accepted record.

exec_wipe must disappear as a memory-lifetime operation. The replacement creates
or resolves the source-declared new MM, replaces only that executor's private
object while it is stopped, and publishes the new binding. Old lineage, storage,
journal cursors and caches survive for old-MM peers and descendants. Access
journal acquisition must also stop holding layout_order_lock across source I/O;
fetch immutable batches first and serialize only application/commit, preserving
cursor/sequence identity and acknowledging only completed native updates.

Native source transfer receipts remain independently necessary. The current
16-bit checksum and age-based TRANSIT healing cannot prove which transfer an
ACK completes. Same-byte delayed ACK and failed metadata copyout require exact
native episode identity and explicit commit/replay semantics; longer timeouts
or more checksum bits do not establish those semantics.


## Route4 completion and monitor tokens in progress

Route3 (675181a959d3) completed 76/78 with only fx1 and exec-shared-mm failing;
both source logs show ACCESS_READ -116. All 78 cleanup records are complete
without survivors/timeouts; boot stable, clean kernel logs, zero new loss or
deadline counts. The strengthened an1 now passes in the broad suite. Evidence
is retrieved as exec-route3-suite-1, including its summary.json.

Route4 (1e3fe264545d) fixes a newly reproduced closed-stdio descriptor placement
failure: context and pidfd descriptors are normalized above 2 before publication.
The old native fixture fails with stdin closed; execution-stdio-baseline-intel-1
explicitly records expected_failure/status 1. All eight standard-descriptor
combinations pass after the fix, along with four other native modes on both
hosts, 13 host/36 table controls and seven focused guests. Route4 focused
artifacts are retrieved locally. Neither native kernel/module changed.

The current exec-token5-inputs snapshot begins monitor-local context tokens.
ctx_register allocates tokens under the registry lock, recognizes duplicate
native lifetime identity independently of native PID, and publishes backing
before membership. Reused native PIDs obtain distinct tokens. The native proc
adapter translates via its retained record's native name. Linux waitid(P_PIDFD)
reaps only the retained native task; stale waits cannot touch a replacement.
Both birth paths now replace their native local variable with the registered
token before exposing it to runtime helpers. Native startup failures still use
owned unreaped native children directly. Child reap errors now fail the session.
Native/host/build validation is in progress; no token5 guest deployment yet.
The fixed 256-context limit and context-based MM lineage still remain; this is
not the immutable MM/operation target migration and does not repair exec_wipe.


## Source publication and immutable memory target validation (September 9)

exec-token5 (642a70e5bc6f) completed 13 host/36 table controls, five native
routing modes on each host (lifetime, PID reuse, proc reuse, proc namespace,
standard descriptor closures), and seven focused guests. All scoped captures,
cleanup, native deltas and boot identities pass. Artifacts exec-token5-* and
execution-token-{amd,intel}-native-5 are retained in the recovery audit directory.

A deterministic source_record_info race reproduced an old query returning raw
stale MM/live state even after another query published a newer/terminal result.
source-record-baseline1 preserves both expected failures; fixed1 passes.
source-record-binding2 adds five modes: monotonic publication, terminal state,
query errors, allocation/epoch overflow, and 1101 immutable full-width binding
histories. Accepted INFO and epoch are returned under the same registry lock;
allocation failure leaves the prior publication and FD ownership intact.

mm-publication6 (6ea68c3b8e3c) introduces VMRJ v19 source MM publications and
caller-owned optional full response receipts. Root and child source context IDs
are validated independently of executor IDs. Child publication includes exact
birth MM/parent/epoch and readiness must match it. This describes the source
binding at publication time, not an implicit target for older memory requests.
19 host/36 table controls pass. Seven focused guests pass. The first ex1 driver
omitted required command arguments; its failure and harness-error.json remain.
Corrected mm-publication6-exec-2 passes ex1 and ex3 with exact output oracles.
The binding evidence shows one child moving through three MMs with epochs 1/2/3,
and a root moving through epochs 1/2. Native captures/counters/cleanup are clean.

mm-target7b (a83dbca286da) integrates source-memory-target.h into ACCESS_READ/ACK.
It captures MM/epoch before callbacks, resolves explicit legacy cursor MMs only
from retained binding history, and never refreshes a stale request to the new
MM. Candidate contexts are retained around INFO plus MEMORY; a raced exec is
fenced by the native expected-MM control. Once MEMORY runs there is no alternate
peer retry, including copyout faults that might follow an inner commit. Failure
to find a live peer is not an MM retirement proof. RELRO effects use the same
saved MM. Production startup now checks source MEMORY capabilities.

The actual helper host control covers surviving peers, INFO/MEMORY exec race,
EFAULT after commit, missing peers, forged epochs and unknown MMs. The native
memory-mm fixture uses the same helper during a userfaultfd-paused argument copy
across exec; it reads the original bytes and records the old MM live. Native
memory-target7b-amd-native-1 passes on #214. All three binaries build warning-free,
20 host controls and 36 owned table controls pass. mm-target7b-focused-1 passes
nine guests: pf4/pf1/pf2/pf3/ns1/group-exit/pf5/ex1/ex3, clean diagnostics,
cleanup, scoped kernel capture and native counter deltas. target7's initial
unused-wrapper Werror build failure remains preserved; only new target7b inputs
were built after removing those now-unused wrappers.

Next integration must replace executor MM identity and exec_wipe, not merely
reset journal cursors. Each operation carries a saved execution target containing
stable context, immutable MM pointer/native epoch and source MM binding. COW
storage uses the MM object directly; native controls use only an eligible saved
binding under a short read guard. Rebind publishes source and native epochs
atomically and changes one context, retaining the old object and ancestry.
No registry/binding/coherence guard may cross source callbacks or network waits.
Installed-page records must include binding identity, and bounded membership,
lineage and ancestry walks still need replacement.

Proc inspection is a prerequisite: the global cached pagemap FD and returned
raw maps FILE can retain native MM references beyond an operation. OBJECT
replacement uses the same native MM while changing its private object. A task
identity check alone cannot distinguish those epochs. Bound open/read/close to
a saved binding guard; maps consumers should receive detached immutable bytes,
not a live native-MM descriptor. Never classify a failed proc lookup as absence
without retained task-lifetime proof. This work is next, not yet implemented.


## Bounded proc inspection and atomic execution targets

mm-proc8 (973d66f9bed5) passes all three warning-free builds, 20 host/36 table
controls, five native routing modes on AMD and Intel, and nine focused guests.
linux-execution-proc.h consumes pagemap within open/read/close and returns maps
as a sealed memfd snapshot with a private descriptor above standard slots. No
native proc FD escapes. All executor consumers use these bounded operations;
the global cached pagemap descriptor and cache-reset calls are removed. Failed
inspection is UNKNOWN unless a retained native task handle proves exit.

proc-snapshot-amd-native-8 runs an identical actual ctx_maps_open regression
against target7b and proc8: obtain the stream, kill/reap the native task, then
consume its first byte. The baseline fails with ESRCH and no byte; fixed reads
the captured bytes. Both scoped logs and native counts are clean. Native routing
coverage additionally consumes the snapshot after forced PID reuse, verifies
seals/CLOEXEC/private descriptor placement, and proves namespace mismatch is
UNKNOWN rather than ABSENT. Intel's first SSH attempt failed before creating
an output directory; the verified retry completed all five cases. No kernel,
module, package or boot changes occurred.

execution-target.h now pairs immutable source binding publications with native
MM views. A per-context publication mutex excludes capture during the native
replacement and source publication gap. The native binding guard remains short
and separate; no network/source callback may run under it. Targets and their MM
storage remain valid after replacement. An observed source MM whose native
binding has not been installed has epoch zero and cannot control the current
native MM. Later publication creates a new immutable target; it never upgrades
an already retained observation in place. Contradictory source epoch history
fails. Allocation precedes native mutation; native refusal leaves the published
pair unchanged. Older responses resolve to old targets without rolling back.

The owned execution-target9 host control passes a deliberately paused native
replacement, old callback landings, stale control refusal, surviving MM peers,
allocation/native failure, exact duplicate publication across 16 threads,
full-width colliding MM IDs, contradictory publications and epoch exhaustion.
Frozen source and result are execution-target9-inputs and execution-target9.json.
This component is not yet used by production vmremote. The next edit must wire
root/child initial source MM identity and object ownership before membership,
then carry saved targets through memory helpers/page workers and native guards.
The legacy exec_wipe and context-derived as_id must be replaced together; no
all-green result is claimed from the component or focused campaigns.

## Continuation: typed executor targets and storage review (target11, September 9)

Production memory helpers now take immutable `execution_target` pointers. Each
service iteration captures its target at entry; exec alone publishes a new
source/native binding and returns that target to the event loop. Native memory
and proc operations hold the matching binding guard, while WAIT is a lifetime
observation outside that guard. Old MM objects, layouts, COW ancestry and page
records remain retained; `exec_wipe` has been removed. MM-only COW reads/writes
use exact registry identities. COW child enumeration is dynamic and ancestry
walks no longer have the old 16-level cap. Three numeric process_vm_writev paths
now use native POKE through saved targets.

Initial registration (initial10) passed five native routing modes on each host.
Target11 now builds all three executables without warnings (`-Werror`, source
`d46e8cd13274`). The converted host/table fixtures use real memfds and canonical
target records, rather than integer-to-pointer casts. All 25 host controls and
36 owned table controls pass in `mm-target11-controls3.log`; native target11
validation is pending. No guest candidate has been deployed.

The storage review reproduced three defects against saved pre-fix code:
`mm-target11-storage-baseline-discard.json` exits 1 because ENOSYS was ignored;
`...-preview.json` records SIGSEGV for a one-byte buffer next to PROT_NONE;
`...-region.json` exits 1 because a retained pointer changed during a callback.
All fixed variants pass, as does a late-landing check that writes old storage
across exec and preserves the new image's installation/loan records. Every run
completed owned cleanup without survivors or timeouts. Failed hole punches now
abort before mapping publication; EINTR retries the same discard. Both object
write diagnostics bound their preview. Region readers copy under map_lock and
writers use that lock. Diagnostic walks take complete MM-scoped snapshots.
Format attributes on map_trace/plog exposed and corrected five malformed logs.

This is still an intermediate architecture, NOT a green runtime: page-channel
requests still implicitly select a current context; source helpers beyond the
journal still lack saved targets; TLS transfer metadata and checksum ACKs remain;
access_sync_locked still holds layout_order_lock across network READ/ACK. These
must be corrected before guests. Fixed context/region/other capacities, native
transfer episode fencing and the other open audit findings also remain.
Evidence is in the recovery audit directory; `mm-target11-inputs` freezes the
current source plus the six-mode native routing regression. mm-proc8 remains
the last focused validated runtime; the last full suite remains 76/78.

Native target11 validation completed: `execution-target-{amd,intel}-native-11`
contains six PASS modes on each host, including actual vector POKE, guarded
native OBJECT replacement, stale-vector refusal and old-object late landing.
Both boot IDs, native counters and kernel diagnostics remain clean. Intel's
first launch timed out before creating a result directory; that log is kept,
and retry1 completed all six cases. Frozen target11 artifacts and SHA manifest
are complete, explicitly marked `guest_ready=false`.

Journal12 removes the layout lock from network READ/ACK. Each immutable MM has
its own serialized cursor, initially naming the exact MM rather than zero.
The complete batch is validated before local application; only that application
holds layout_order_lock. ACK advances the cursor only after a matching response.
Any failure poisons that cursor instead of replaying an uncertain commit. The
wire response receipt is caller-owned; a concurrently published current binding
cannot retarget journal bytes. Forward releases local publication ordering
before fetching the journal. Source callbacks can therefore finish construction
work, and newer construction stamps are checked before old access effects apply.

`access-journal12-{baseline,fixed}-*.json` preserves five expected baseline
failures and five fixed successes: callbacks, newer callback constructions,
concurrent readers, malformed suffixes and failed ACKs. All owned cleanup is
complete. `mm-journal12-inputs` is frozen for three binary builds plus the expanded
30 host and 36 table checks; build/validation is in progress. Page wire targets,
source helper migration and transfer receipts still prevent guest deployment.

Journal12 completed: source `66730448ccd5`, three warning-free binaries,
30 host controls and 36 owned table controls. Artifact manifest/archive and
individual table reports are retained; no guest deployment.

Page13 changes the main magic to VMRK v20 and page magic to VMPI. Page requests
carry full saved MM/context/parent/epoch bindings; responses echo that binding,
address and operation. The source captures a fault binding once for all its
requests/retries and its readahead window. RECALL admission and page-state
probes use the native expected-MM fence. A non-fault recall captures before
BEGIN; commit remains bound by its native ticket. The executor observes an
explicit source binding in retained history, including object-only observations
before native publication. It never substitutes the context's current image.
Page ingress rejects unsupported legacy PUT/PUTN/PUTV/UPGRADE/HELLO and invalid
ranges before touching storage. The old handlers remain unreachable code and
should be removed when the obsolete recall infrastructure is cleaned up.

The page reader validates header target/address/op/reserved and status before
publishing a complete payload. Truncation and INFLIGHT timeout preserve caller
bytes and prior metadata. Readahead validates each reply's exact identity.
Expanded page-reply injections cover all binding fields, address, operation,
reserved bytes and short payloads. `transfer-old-target` sends a saved old-MM
request after binding replacement and verifies the incumbent old transfer,
old bytes/generation/loan and new object's independent contents survive.

Page13 prebuild `76d12d5f0a91` passes 30 host + 37 owned table controls;
`mm-page13-inputs` is freezing for an isolated three-binary validation.
The remaining main-channel memory operations still need explicit saved targets,
source helper migration and caller-owned transfer/ACK receipts. TLS metadata,
16-bit source transfer checksums, fixed capacity limits and earlier open items
remain. No full suite beyond 76/78, no target11/12/13 guests. Review also found
ctx_register currently permits duplicate source identities on distinct native
contexts; reject that before relying on unique source-to-executor lookup.

## Continuation: page13 completion, source identity14, source target15

Page13 (76d12d5f0a91) completed the frozen three-executable Werror build,
30 host and 37 owned table controls. Inputs, source IDs, output SHA256s,
cleanup reports and package are verified in mm-page13-user-build-complete.json.
No guest deployment.

Source context identity must remain unique across native context lifetimes,
including native exit. ctx_register now refuses a second native identity with
EEXIST before publishing membership. The extended production routing control
attempts the collision after native PID reuse and during a proc-open race.
Both baselines reproduce the missing rejection on AMD; fixed six-mode native
campaigns pass on each host, no MM/ref/handle leak, no lost/deadline delta, no
kernel diagnostic or boot change. Frozen inputs and complete reports are in
source-identity14-*. Intel initial SSH banner timeout preceded output-directory
creation. An unkeyed nested SSH probe failed authentication; the independently
verifying driver retry passed.

Source-target15 remains in progress. A distinct source_target type forces
memory helpers to carry an immutable binding. LAND/PEEK/SERVE/PGSTATE/PGSET/
PGSCAN/MAPPING/MMLOG use the expected-MM native envelope. Ancestor lookups
resolve recorded MM history before memory access. Source maps snapshots are
sealed memfds, bound to the requested MM and capped by VMR_LAYOUT_MAX; no proc
FD survives into the consumer. The permanent root oracle mem FD is removed.
Read-only proc representative selection validates the opened MM and closes
mismatches. Native memory commands select once and never replay after a
possibly committed failure; the old CTXPAGE sibling retry was removed.

VMRL v21 main requests now carry explicit targets; receipts echo the full
binding, operation and six arguments separately from current binding
publication. Response payload is staged before caller-visible writes. Journal,
layout, child snapshot, page and write-preparation calls retain operation
bindings. Source ingress validates recorded context/MM/epoch/ancestry before
native entry. compile5 passes two executables plus 30 host/37 table controls.
Subsequent edits preserve unknown PGSTATE errors and reject failed protection
probes; new production helper/framing regressions are building.

Still required before guests: caller-owned transfer/ACK receipts (TLS remains),
full native episode IDs and copyout replay, admission pre/post-MM publication
review, source lineage/child completion obligations. The current migration
is not a new all-green runtime; full-suite evidence remains 76/78.


## Continuation: receipt16, permission17, admission18

Source15 completed and packaged (8c43d77d578d): three static Werror executables,
32 host controls, 37 owned table controls. Receipt16 (25974efc2a4e) likewise
completed/package with 33 host +37 tables. Permission17 (3d4efcf70c4a) completed
and packaged with 35 host +37 tables. Manifests mm-*-user-build-complete.json
verify all input/output hashes and owned cleanup; all guest_ready=false.
No guest runtime evidence supersedes proc8 / full suite 76/78.

VMRM v22 separates native errno from wire ownership outcomes: native ESTALE
(-116) previously collided with NOTHOLDER. VMR_CTXPAGE_FAILED=-4096 now stops
an unresolved memory operation. source_page_fetch validates page framing and
returns a caller-owned receipt with saved binding, base, count, generation,
taken mask and original received checksums. The old TLS receipt fields are
removed; nested fault buffers are automatic. ACK validates receipt range and
uses original wire bytes even after repair. A second fetch after MM replacement
cannot overwrite the first receipt. page-receipt exercises these production
paths, including rejected malformed ACK and memory failure. This remains a
checksum-based native protocol; it is NOT an exact native transfer episode.

Permission17 reproduces premature loan retirement on source CLAIMING and
preserves the loan until a complete landing. NOMAP routes to the owner;
other unresolved outcomes stop before permission changes. COW permission
refuses failed copy work explicitly and routes denied access before watch
fallback. The cow-denied baseline already reached the owner through a later
guard, so it is not evidence of a measured native unauthorized grant.
WRITEPREP no longer marks a child copy complete before any copy commits.
Both source obligation and ended-lineage baselines abort on their assertions;
fixed source-bound-helpers passes and all owned reports have no survivors.

Admission18 retains the first ADMITTED binding and actual arguments across
polls, preparation and dispatch. Later ADMITTED observations must match both.
Child lineage and inherited RELRO use the saved parent MM, never a fresh parent
lookup after dispatch. Completion captures one post-call binding, verifies
normal-call identity or successful exec replacement, and builds all memory
effects against it. source_publish_reply checks a completed binding instead
of relabeling its effects with a later INFO. ACCESS_LOG construction is queried
through source_memory_target_origin_call: this metadata belongs to the native
task, so an arbitrary peer of the same MM cannot supply it. Native expected-MM
fencing and returned MM identity are both checked.

The unused source vacate shipment queue and its self-map suppression table
are removed. Executor invalidations already use the retained per-MM access
journal. Source MMLOG still retires source-local mapping metadata, rejects
missing/malformed/overflowed records, and drains all batches. Mapping
classification and sequence now live in the call's reply, not TLS. Successful
exec no longer erases new-MM shared metadata populated by callbacks.

Production source-admission-binding controls pass seven modes: binding-drift,
args-drift, construction, publication, birth, completion, exec. Baselines
reproduce four missing rejection checks, plus parent-MM lineage failure;
ordinary completion passes. The initial baseline birth diagnostic's cleanup
hit an unimplemented SIGNAL in the fake boundary; the actual lineage error is
visible immediately before it. The fake now accepts native SIGNAL; a clean
baseline birth rerun is pending. The initial exec baseline assertion checks
new early binding publication, not an independently reproduced runtime bug.
Admission18 frozen three-binary/42-host/37-table build is in progress. No
native core or module change was made by these userspace steps.

Next: exact native source transfer episodes and retained replay after bad
copyout; remove age-based TRANSIT settlement, early bulk ACK, connection-close
ownership fabrication and transfer tracking eviction. Continue COW ancestry
and CLAIM landing evidence review, then native/focused/full/cross/program/GUI
validation. Do not label the campaign all-green from host controls.


Admission18 completion: source 15819ffeece9, all three Werror executables,
42 host controls, 37 owned table controls, verified package. Clean baseline
birth rerun exits 98 from the actual lineage error with no cleanup assertion,
timeout or survivor (source-admission18-baseline2-birth.*).

Native serve-episode19 confirms both outstanding protocol defects on AMD
#214. Identical-content transfers had sum 28127 with generations 0 and 73;
the old ACK incorrectly returned 1 and settled the newer transfer REMOTE.
An unacknowledged private transfer, after 250 ms, was answered ABSENT and
settled REMOTE. No ACK had been sent. Both expected failures cleaned up
completely with zero MM/ref/handle counts and no lost/deadline delta, kernel
diagnostic or boot change. Evidence: serve-episode19-amd-native-baseline/
complete.json, rows.json and live-kmsg.log. No native implementation change
has yet been applied; tests/control/serve-episode.c is the new native control.


## Native custody candidate #215 (building; not installed)

The new source-local control49 (vmctx_transfer.h) stages BEGIN, CAPTURE, READ,
ACK, CANCEL and FORGET. BEGIN allocates a unique full64 ticket and native MM /
context references before any destructive operation. CAPTURE binds the mapping
incarnation with the existing page-operation guard and invokes the production
serve engine once into preallocated kernel storage. The guard and immutable
result survive metadata/payload copyout errors. READ can replay; ACK requires
an offered result and settles only its exact ticket. ACK result is retained
until FORGET, so an old duplicate never acts on a newer same-address grant.
Legacy checksum ACKs cannot bypass an active guard. Age and ask-count healing
are removed. An untouched absent page is also a retained grant: it needs an
ACK after executor installation rather than assumed delivery.

Unacknowledged custody abandoned by its monitor thread marks the MM journal
ENOTRECOVERABLE and kills the original source task. It leaves TRANSIT closed,
releases its native references, and counts the event. Mapping invalidation
cancels a saved guard; READ cannot copy into a replacement mapping and CANCEL
can retire invalidated custody. No mutex or native MM lock crosses user copy.
Native owner-exit, failed-copy and repeated-address tests remain required.

Frozen episode215-inputs contains 20 core files +20 module inputs. It derives
from every verified mmfence214 native input; the local Linux source tree is
older and was NOT used as the native baseline. An editable work copy remains
in episode215-work/core. Sourceable kernel support is in
kernel-patches/support/vmctx-transfer-core.h, but the aggregate new patch has
not yet been exported. Full kernel/modules build is active on AMD under the
existing remote mapping-kernel-7.0.14 tree. Its driver saves every overwritten
input and old image/symbol/config in episode215-before. No install/reboot.

transfer-custody215-inputs freezes the six-mode native test and its headers;
transfer-custody215-amd compiles Werror. Modes cover pre-capture cancellation,
BEGIN/CAPTURE/READ bad copyout and replay, stale ACK with identical content,
mapping invalidation, absent-page grant, and monitor-thread abandonment.
The prepared episode215-amd-native.py runs these plus the legacy age test,
legacy recall control, and the established 34-case campaign. Source/executor
wire integration, all-page ACK, removal of connection-close ownership
fabrication, COW ancestor copy correctness, and full guest gates remain open.


## Native custody #215 validation

The one-time boot completed on AMD with boot ID
5313c6df-9187-45ae-b956-183cfb056432 and release
7.0.14-vmctx-audit-recall1-episode215. The full 42-case native campaign passed,
including all six new custody modes, legacy unacknowledged-age and recall,
and the 34 established regressions. The supplemental three-case campaign
passes failed ACK metadata copyout and idempotent replay, 80 simultaneous
retained tickets retired in reverse order, and monitor-thread abandonment
while the native source's clear-TID points into the captured page. Every case
returned MM/ref/handles/tickets to zero; lost=1 and deadline=1 were exactly the
intentional legacy failure cases, and each owner-abandonment case counted one.
No warning/oops/lockup, unexpected deadline, or boot change. Evidence:
episode215-amd-native-1/ and episode215b-amd-native-1/.

Patch 0038-vmctx-retain-source-transfer-custody.patch applies to the verified
#214 baseline without offsets/fuzz and reproduces all 20 frozen #215 core
inputs by SHA256. Support headers match the frozen files. The older local
src/ tree remains unchanged and must not be used as the current native source.
Proof: episode215-patch-proof.json and episode215-patch-apply.log.

The production source adapter is now being implemented in source-transfer.h;
wire receipt integration, batch custody, COW ancestry and later guest gates
remain open. Native passes do not supersede the last full guest suite 76/78.


## Custody20 and native explicit teardown #216

Custody20 source aee03cc1bef7 is frozen and packaged: three Werror binaries,
43 host controls, 37 owned table controls, guest_ready=false. Source page
metadata is now a caller-owned source_page_receipt; neither nested helpers nor
a later request/MM replaces the earlier result. Internal partial destructive
page reads are rejected. The native source-transfer.h adapter retains exact
MM/address/ticket across errors, stages READ bytes before publication, and
never retries through a new MM representative after BEGIN. It distinguishes
CANCEL from delivery ACK, even when mapping invalidation allows cancellation.
Fault-injection controls cover uncertain capture/read/ACK/FORGET and malformed
READ metadata. Both initial and final cancellation-aware adapters passed real
native controls on #215 (episode215c/d-amd-native-1), without leaked counters.
The source still serves production CTXPAGE through the legacy native engine;
this adapter is prepared and tested, not yet integrated into that wire path.

Connection teardown needs to release its transfer guards before waiting for
native exit work. Candidate #216 adds ABANDON for the creating native thread
and a TEARDOWN capability. It uses the existing owner-exit semantics: poison
unresolved grant MMs, kill the original source tasks, and release all calling-
thread tickets. Repetition is idempotent; it never grants ownership. The source
must terminate failed-session execution rather than continue with a poisoned
MM, including any other process sharing it. This API does not claim selective
recovery of such peers. Its native regression points clear-TID into captured
memory, abandons custody, then reaps the source while the creating monitor
thread remains alive. This specifically excludes thread-exit cleanup as the
reason the wait succeeds. The #216 47-case campaign passed in full, including
all eleven custody modes. Every case returned MM/ref/handles/tickets to zero,
with only the intentional lost/deadline/abandonment deltas; boot unchanged.
Evidence: episode216-amd-native-1/complete.json. Its launcher exited 0.

#216 derives only from frozen #215 native inputs; the native delta is two
files, the transfer UAPI and core helper. Full build/package is warning-free,
all 6,891 modules verified, one-time boot completed with ID
c6d9fc7e-d3b7-49a8-a3e4-280c3d286fd8. Patch 0039 reproduces all 20 frozen core
inputs without offsets/fuzz. Local src/ remains older and untouched.

## Landing21: no authority from retry counts or partial writes

claim-landing21-baseline and claim-identity21-baseline both reproduce a false
landing after 2,001 CLAIMING responses: an existing backing folio made the
production helper return 0, allowing callers to treat CLAIM as ABSENT. No
transfer completed. The second baseline also demonstrates same-address retry
counts crossing full64 MM identities. Fixed controls both pass: retries stay
within their MM and page and only affect throttling, never ownership. An
actual next source answer is required to resume a different ownership path.

page-landing21-baseline reproduces the production rescue path marking an
entire page installed after native POKE committed 1/4096 or 4095/4096 bytes;
its object fallback is deliberately read-only so nothing completes the write.
The independent fake native boundary copies exactly that prefix. Fixed
controls reject both partial landings and accept 4096/4096. Nine page-grant/
landing predicates now require the complete page; raw POKE byte counts and
vector-write partial reporting remain truthful. This does not by itself prove
the retained-rescue path has current ownership; its older-copy fallback still
needs integration with exact custody before guests are ready.

landing21 source 969fc957a459 is frozen and packaged: three Werror executables,
46 host and 37 owned table controls passed; guest_ready=false. Evidence:
mm-landing21-user-build-complete.json. Build and package launchers exited 0.
Full64 wire transfer receipts, confirmed/all-page ACK, connection-close cleanup,
COW ancestor snapshot correctness and the remaining audit gates are still open.
No new guest runtime evidence; the last full suite remains 76/78.

## Connection custody22: explicit source lifetime

source-custody.h now owns a dynamic collection of native tickets for one
connection thread. It allocates before BEGIN, retains uncertain operations,
matches full binding/address/ticket on settlement, and frees only after native
FORGET. A stale ACK remains an error even when its cleanup succeeds. Failed
ABANDON closes admission before retaining the records for cleanup retry. A
foreign thread cannot abandon the owner's native tickets.

The expanded source-transfer host control passes 80 simultaneous receipts
across full64 MM identities with identical addresses/content, forged binding
and truncated-ticket rejection, failed capture/ACK/FORGET, and failed terminal
abandonment. source-custody22-amd-native-1 passes two real #216 modes: 80
captures followed by reverse retirement, stale ACK against a later same-page
episode, mapping-invalidated ACK, and source clear-TID teardown while the
creating monitor thread remains alive. No leaked MM/ref/handle/ticket, no
boot change or diagnostics, and exactly one intentional abandonment. Frozen
native control inputs: transfer-custody22-inputs (71 files); binary SHA256
5bdd27a43ccd5bfce0ed67872ab3cd7aed35ac4fcea17be7d3ccab88d20c8c48.

This collection is not yet used by production CTXPAGE. Confirmed wire ACK
transport is being implemented next; checksum identity, bulk settlement,
disconnect ownership fabrication, and COW fallbacks remain to be replaced.

## Confirmed22: acknowledgement transport before execution release

VMRN/v23 now requires a source reply to INSTALLED. The source returns a
native settlement failure instead of ignoring it; rejected ACKs retain their
connection tracking entry. The executor retains its pending receipt until a
valid empty successful reply arrives. EOF, source rejection, wrong full64
target, or an unexpected payload terminates the monitor. The source still
uses legacy checksum PGACK here, so this is not exact ticket integration.

ack-confirm22-baseline reproduces false success after the peer consumes the
complete request and rejects it: executor exit 0 instead of required 97.
All five fixed transport controls pass, with clean owned teardown. Expanded
source-bound and page-table controls verify native rejection reporting and
one confirmed send; fault-dispatch verifies confirmation under the incumbent
reservation, including page-server prefill. The last post-fault native RESUME
now follows ACK flush, rather than preceding it.

confirmed22 source cfbf5d360fab passes three Werror executables, 51 host and
37 owned table controls. Its exact current inputs were verified and frozen
after that successful build, without rerunning an unchanged suite. Actual
build path and this packaging provenance are recorded in
mm-confirmed22-user-build-complete.json; guest_ready=false. Package launcher
exited 0. There is no new guest evidence. Native ticket wire integration,
bulk settlement, connection-close semantics, and COW review remain open.

## Snapshot23: private read-only pages retain fork obligations

snapshot-readonly23-baseline reproduces the snapshot walk omitting a private
read-only page: no native protection, no OWED record, strict child copy refused,
and the child's object lacks the inherited bytes. A read-only mapping is not
proof that its bytes can be reloaded from the source file: relocations may
precede protection and the source may have transferred the page away.

The walk now includes all private object pages, preserving shared mappings'
exclusion. Native PROTECT_BACKING only removes write access and requires the
existing quiesce lease; it does not grant access. Fixed read-only and executable
controls copy the original bytes into the child's real sparse object and keep
them after parent bytes change. The shared exclusion control is included in
the frozen build. mm-snapshot23 source 8772a3ebf57a is packaged after three
Werror binaries, 54 host controls and 37 owned table controls passed. Build
and package launchers exited 0; guest_ready=false. No guest use.

Next wire integration should use the tested connection custody collection,
carry each opaque full64 source episode, and ACK through that collection.
Every returned data page, including a read-only copy, needs confirmed ACK so
mapping invalidation before publication cannot be ignored. Fresh anonymous
ABSENT grants need an explicit source-authorized zero-page landing; no old
retained buffer can establish those bytes. An ABSENT result for REMOTE means
the executor already owns the page and must not become zero-fill permission.

The initial integration should request only the faulting page. Existing bulk
source ACK precedes delivery and executor paths discard or omit receipts for
prefetched pages; no speculative destructive capture is justified until all
returned pages retain reservations and confirmed landings. This reduces the
transaction to one stopped fault and one complete landing, with performance
to measure after correctness. Preserve all old frozen artifacts.

Remove the internal source ancestor TAKE and raw ancestor-read fallbacks when
integrating custody: current ancestor bytes do not prove fork-time content,
and transferring the parent's ownership while landing a child is invalid.
The child's native private snapshot and the executor's strict protected-copy
path are the supported provenance. Failed provenance must end the access,
never escalate to an unprotected ancestor or fabricated zeros. The wider COW
review still needs to prove all fork obligations and source/executor handovers.

Connection cleanup must call explicit native ABANDON before any source exit,
FINISH, admission-failure or oracle wait which may need a captured page. An
unresolved collection ends the complete failed execution session, including
MM peers; it never PGSETs TRANSIT to REMOTE. Native ABANDON is thread-wide,
so one connection collection must be its only native ticket owner on that
thread. Do not allow the fault-service ancestor path to create stray tickets.

Further review notes (not yet fixes): source lineage only records MM edges and
per-page broken flags; it does not currently distinguish a page inherited at
birth from a new mapping at the same address after birth. Blanket failure of
all fork-child ABSENT responses would reject valid new anonymous allocations.
Conversely, a readable ancestor cannot prove inherited bytes. Resolve that
provenance before replacing failures with zeros or removing fallback branches.
The read-only native SERVE path still copies while keeping its own page; trace
later permission changes and both endpoints' write-grant paths to verify that
these copies cannot become independent writable holders. No new runtime
counterexample has been measured for that specific transition yet.

The eager-SPENT snapshot_map_object note is not by itself proof of a defect:
the helper preserves existing child copies before native MAPOBJ, and a later
fork may re-arm OWED. Check actual obligations and a reproducer before changing
its ordering merely to match the state name. Existing comments contain claims
from obsolete experiments and must not substitute for current invariants.


## Single24: a reserved fault is one source transaction

The baseline single-page24 control entered source memory twice and returned
8192 bytes for a wide request. The fixed production source refuses before
any native memory operation. Executor selection now requests its faulting
page only; RTT remains diagnostic and cannot expand custody. The source bulk
ACK path is removed. Positive page replies must be exactly one complete page.
Three Werror binaries, 55 host controls and 37 owned table controls pass for
source 88d12d58f427; mm-single24 is frozen and packaged, guest_ready=false.

## Episode25 integration in progress

Production CTXPAGE now uses a stack-owned connection custody collection and
native BEGIN/CAPTURE/READ. VMRP/v25 carries one opaque full64 page_episode;
all data replies require a confirmed ACK, including read-only COPIED. Native
ABSENT+GRANT is explicit zero bytes under the retained ticket. ABSENT with
REMOTE becomes NOTHOLDER, not permission to reconstruct zeros. A refused
capture becomes CLAIM only after native CANCEL/FORGET succeeds. Uncertain
capture/read leaves custody for terminal cleanup.

The connection rejects new native work while receipts remain unconfirmed.
Cleanup calls native ABANDON before exit/admission waits; unresolved custody
terminates the complete source session and MM peers, never PGSETs delivery.
The source ancestor TAKE/raw-read/relaxed fallback was removed: current ancestor
bytes cannot prove the child's birth snapshot. The remaining zero decision
still needs mapping provenance; this is not a complete COW correctness claim.
The executor's verify_memory diagnostic was a destructive CTXPAGE caller that
threw away its receipt. It and its two environment-triggered calls were removed.

ack-episode25-baseline reproduces the wire using 000000000000beef where the
required episode is 800000010000beef. Updated transport controls preserve the
full identity across nested old/new MM fetches. Source production controls
exercise old-MM native selection, no early ACK, same-address tickets, read-only
invalidation, zero grants, busy cancellation, read-copyout failure, and explicit
ABANDON before source cancellation (trace ASS, then exit 98). Three Werror
executables and 56 host controls pass for ce6602cb1f36. Table and real-native
source-page modes are in progress; see active handoff. No guest deployment.

## Episode25 diagnostic and exception lifecycle failure

Episode25 source ce6602cb1f36 is packaged with 253 frozen inputs, three Werror
binaries, 56 host controls and 37 owned table controls. source-page25-amd-native-1
passes taken, readonly, zero and stale against actual production ctx_page/ACK
on native #216. mm-episode25-focused-1 passes pf4/pf1/pf2, then fails pf3.
Counters remain [0,0,0,1,1,0,4]; boot unchanged, kernel failure detector clear.

The source child 35947 dies by the intended NULL SIGSEGV (native wait status
139). Native CTL_EXCEPTION returns -ETIMEDOUT when vc->dead wakes its wait
without sys_done (frozen core kernel/vmctx.c around 9080-9135). This is not
proof of a timeout and not by itself terminal proof. The old source exception
branch emits -110 without ended metadata, and owner_takes_fault ignores ended
metadata entirely. Its subsequent dead-child page fetch fails, killing the
parent session. Fix in progress uses retained INFO proof, request binding,
caller-owned exception reply and an observed executor pidfd exit. Invalid
wire/error outcomes must not manufacture terminal success or reinstall CPU.

## Exception26: native task exit is an exception outcome

source_exception_reply decodes complete CPU state before native work, saves
its source binding, preserves runtime-credit errors, and queries the retained
native task on ESRCH/EINVAL/ETIMEDOUT. Only an ENDED record publishes status
and an empty terminal reply; a live task remains an error. Publication checks
that the saved MM binding has not drifted. Executor owner_takes_fault receives
caller-owned metadata, validates its exact saved source task/MM/epoch, rejects
contradictory framing and stops only the specified execution task. It observes
pidfd exit before success; a refused/unconfirmed stop fails the session closed.
No CPU state is installed for a terminal response. The exact source wait status
survives executor SIGKILL and is used by existing final teardown reporting.

Eighteen new host modes cover each native failure phase, delayed terminal
publication, live timeout, malformed request, CPU-handler success, binding drift,
full exception socket transport, real pidfd exit, unaffected MM peer, LEGAL,
wrong task/MM, invalid terminal values/body, and failed native stop. The same
executor test against frozen episode25 reproduces failure, exit 1 with no leaked
processes. The first fixture run had incomplete MM registration and was killed
by its alarm; owned cleanup killed two descendants and left no survivors. Its
failure artifact is preserved; corrected controls all pass with clean teardown.

mm-exception26 source 0a2abe663857 has 255 frozen inputs, three Werror binaries,
74 host controls and 37 owned table controls. Package guest_ready remains false;
isolated nine-case guest diagnostic is starting on unchanged native #216.

## Handoff after origin27 and read-only lock probe

See [the current handoff](/home/desktop/lan-boot/vmctx/audit/HANDOFF-20260909-locks.md) for exact artifacts and next steps.
Origin27 dd8203e3ca32 is frozen/packaged with 257 inputs, three Werror binaries,
81 host controls and 37 owned tables. PULL_LOCAL distinguishes local snapshots
from source custody; actual-source object and exclusive-local-map landings ACK.
Frozen baselines reproduce fabricated local ACK and two omitted actual ACKs.
No origin27 guest run has occurred. Exception26 focused guests pass pf4/pf1/pf2/
pf3/ns1, then group-exit fails at the local-COW receipt bug.

The user explicitly requires one page/one owner and underlying locks fixed
first; checksums are consistency diagnostics, not ownership authority.
Read-only-upgrade native oracles PASS, but exception26 guest probe times out
while reporting after the RO->RW transition and source pipe readback. Guest
fd2 write faults holding the shared file-position lock; vmhome logging waits
in fdget_pos on that same fd2 description and cannot service the fault.
This observed lock cycle prevents the actual mismatch message from completing.
Preserve mm-readonly27-probe-1 and readonly27-first-deadlock.txt; do not assert
unobserved stale byte values. AMD deadline counter increased1->11, other
counts clean, same boot. All operations are now stopped for requested handoff.
