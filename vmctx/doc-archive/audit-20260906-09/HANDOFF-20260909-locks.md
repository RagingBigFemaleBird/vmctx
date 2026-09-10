# Handoff: ownership and locks, September 9, 2026

## Resumed work: diagnostic28 / MM review29

The user resumed this work and explicitly asked whether all kernel MM
modification paths were covered. The stop instructions and current-runtime
claims in the older handoff below are superseded by this section.

**Not all green.** Current source adds `user/monitor-diagnostics.h`: vmhome's
fault-service threads send diagnostics through a bounded nonblocking channel;
a separate thread drains the original stderr sink. The source child restores
the original stderr before exec. Host inheritance/saturation/sink-error controls
pass, and a real native source stderr write fault is serviced while the monitor
logs before landing the page (B/diagnostic28-amd-native-3). Frozen production
package B/mm-diagnostic28-user-tree.tar.gz has three Werror executables,
84 host modes and 37 table modes passed; deployed diagnostic tree is
`/home/biwu/audit-20260907/mm-diagnostic28-user-tree`.

The isolated read-only-upgrade guest now reports its actual failure without
deadlocking: source readback is 0x13, executor bytes are 0xb6. Run
B/mm-readonly28-probe-1 failed status1 with no kernel failure/counter deltas.
The initial synchronous-logging native baseline hit its outer watchdog and
added seven deadlines; that failed diagnostic is preserved and is not green.
The #216 native deadline count reached **18**, up from the older 11.
Later native diagnostics added no deadlines or resource leaks. AMD now runs
**#217**, release `7.0.14-vmctx-audit-recall1-private217`, boot
`ad027942-547c-470f-8b45-9b61b58ff9ca`, module R/private217-native/vmctx.ko.
Current counters [MMlive,refcnt,handles,take_lost,deadline_hits,transfer_live,
abandoned] are [0,0,0,1,1,0,3]; the nonzero values are the native controls'
explicit destructive-copyout, elapsed-deadline and abandoned-custody cases.
The former #216 count of 18 is historical, reset by the one-time #217 boot.

The current MM coverage findings are in
[mm-path-coverage-20260909.md](mm-path-coverage-20260909.md). Exact current source
is frozen in B/mm-review29 (299 files plus a 24-file supplement); all 20 #216
core inputs match. Newly measured failures on #216:

- Four read-only private transfer classes leave native record NONE/PTE present.
- UFFDIO_COPY and UFFDIO_ZEROPAGE install a second source page while record REMOTE.
- MADV_COLLAPSE succeeds and makes a remotely owned page present on the source.
- MREMAP_FIXED succeeds and erases remote provenance at both addresses.

B/mm-review29-amd-native-2, -3 and -4 explicitly record `pass_=false`, with clean
diagnostic completion. -1 is a preserved precondition failure in the original
read-only fixture; fixed by prefaulting the child's private file before adoption.
Source controls are `readonly-custody.c`, `uffd-custody.c`, `collapse-custody.c`
and `remap-custody.c`.

Private-read-only forced-COW/TAKE candidate `build/private217-work/core` is
**built and deployed as #217**. It extends the older held readonly217
draft with address/task/reservation-scoped zero-page COW authorization. Frozen
inputs B/private217-inputs contain 21 native core files (three changed from
#216) and the same 20 module inputs. B/private217.diff is the exact diff.
It cannot fix the measured UFFD/THP/remap holes:
remote residency is represented as ordinary `pte_none`, while ownership lives
only in a separate xarray. A native residency representation and complete
copy/move/discard/permission/pin/TLB contract are being investigated. No checksum
or generation repair was added. Full suite, group-exit, cross-host and GUI
validation remain outstanding.

#217 passes 51 native cases across B/private217-amd-native-1 and -2, summarized
in B/private217-native-validation.json. Four RO custody classes now revoke
source PTEs before ACK; forced zero COW is exclusive to TAKE. The previous
take-file-race oracle required RO private EBUSY and is preserved as a failed
row. Its updated test proves RO private transfer/removal, original file bytes,
and unchanged shared refusal, with 1,000 competing-mapper transfers. Kernel,
6,891 modules, external module and initramfs build/package checks passed;
patch 0040 reproduces all 21 core inputs without fuzz or offsets.

Original read-only upgrade guest now PASSes on #217 + frozen diagnostic28,
B/mm-readonly30-probe-1. New readonly-return guest (RO source read, then
executor upgrade/write, then source readback) still FAILs stale bytes, cleanly,
B/mm-readonly30-return-1. Source fault handler used GETS for RO files and
created a second copy at LAND. Current vmhome uses GET for those recalls too.
Frozen userspace mm-return31 (`89215130b979`, 280 inputs) passed Werror,
84 host and 37 table modes, then both byte oracles passed in
B/mm-return31-probe-1. Four exception guests pf4/pf1/pf2/pf3 passed in
B/mm-return31-focused-1, but ns1 timed out (status124, no counter deltas/leaks).
It repeatedly faults at RO file page 0x4a8000, with source REMOTE and local
object bytes, absent executor PTE and watch set. Existing native MAPOBJ always
requests write access and refuses that read-only VMA. The fallback copies to
the object/declines or skips a watched page; it never makes the PTE accessible.
Group exit and the remaining focused guests did not run after that failure.

**Current runtime:** AMD #218, release
`7.0.14-vmctx-audit-recall1-readmap218`, boot
`9e8c7fe9-2459-400d-b790-513f628abe29`, module R/readmap218-native/vmctx.ko
(SHA256 3dc456e82363d8cc25ce3ceeb0cf1514def2ecf874bb778716e5e67a976218ab).
Kernel, 6,891 modules, external module, initramfs and package checks pass;
one-time boot preserves defaults and old entries. Patch 0041 reproduces the
22 frozen core inputs without fuzz or offsets. MAPOBJ_READ (cmd50, capability
bit2) installs an existing shmem folio for reads, with new PTEs write-protected
even in writable VMAs, and returns 0 for object holes. It is excluded by the
same object_sem as MAPOBJ. Read faults preserve COW and retain the write watch.

B/readmap218-amd-native-3 passes **55 native modes** with complete kernel
capture and no unexpected warnings or counter deltas. The new object-read218c
control tests actual reads, writes and instruction execution in RO/RW/EXEC/hole
modes. -1 and -2 preserve failed fixture setup: EINVAL before context publication,
then EAGAIN before the initial CPU checkpoint. Corrected bounded retries follow
the existing native adapter contract; no kernel or runtime implementation changed.
Counters are [0,0,0,1,1,0,3], nonzeros from intentional native controls.

Frozen userspace **mm-readmap32**, source id `fe9e2049950d`, 281 inputs,
passed three Werror tools, 84 host and 37 table modes. Both read-only byte
oracles pass on #218 in B/mm-readmap32-probe-1, with unchanged counters and
complete cleanup. The focused guest campaign is active via
B/mm-readmap32-focused-launch.py (inspect processes for its session),
then run the prepared B/mm-readmap32-fork-launch.py. That new guest checks that
parent mprotect/write cannot alter a delayed child's private RO fork snapshot;
its native local baseline passes. No UFFD/collapse/remap/TLB repair is claimed.

---

Work is **not all green**. The user requested a summary/handoff, so execution
has stopped. No build, packaging, native campaign, guest campaign or reboot is
active. Do not continue automatically until the user resumes.

## User requirements and immediate priority

Original objective: continue the previous handoff, get everything green, read
thoroughly, doubt assumptions, put architectural correctness first, instrument
and reason step by step. Latest steering, verbatim:

> checksums may serve as a way to determine consistency, but it should not be
> relied on. One page can only have one owner at one time. You need to fix the
> underlying locks issue first.

Prioritize the ownership/reservation/permission protocol and lock ordering.
Do not make checksums, matching bytes, generation repairs, elapsed waits or
backing-file presence substitutes for exclusive ownership. Existing
`pull_gen_check` repairs and relaxed COW fallbacks remain unproven; passing
small guests is not architectural clearance. Do not spawn subagents without
an explicit user or applicable instruction authorizing delegation.

## Current exact workspace and frozen candidates

Repository: `/home/desktop/lan-boot`. Preserve the large pre-existing dirty
worktree; no reset, clean, commit or stash was performed. No AGENTS.md found.
No skill or goal was activated. Read ledger from earlier work remains valid:
`vmctx/audit/reread-20260907.json` (148 authored files / 69,293 lines).

Evidence base, abbreviated **B** below:
`/mnt/d/VM/lan-boot-recovery-20260908/logs/audit-20260907`.

**Current production source is mm-origin27, dd8203e3ca32.** All 257 input files
were checked against the packaged manifest after completion and match.

- Three Werror static executables, 81 host controls, 37 owned table controls PASS.
- B/mm-origin27-inputs and B/mm-origin27-inputs.json retain all inputs.
- B/mm-origin27-user-build-complete.json and B/mm-origin27-user-tree.tar.gz.
- Build: `/home/desktop/lan-boot/build/mm-origin27-precheck`.
- Logs: B/mm-origin27-compile1.log, -host-precheck.log, -tables-precheck.log.
- `guest_ready=false`. **Origin27 has never been deployed/run as a guest.**
- vmhome SHA b707f6a305827a111552371b31e89cff8a66b4d5865b7648bc2cc4e259b65e35
- vmremote SHA a4871f7031f6b2e276b27bebf7404110db36cfb389b4c452bb225523a0e35461
- vmremote-local SHA 4565669c3766566fe6c7d861be6381e73f8aa2736adc04236345d657cb3bce77

**Exception26, 0a2abe663857**, is the latest deployed diagnostic runtime:
255 frozen inputs, three Werror binaries, 74 host / 37 owned table controls.
B/mm-exception26-user-build-complete.json and -user-tree.tar.gz. Remote tree:
`/home/biwu/audit-20260907/mm-exception26-user-tree`.

- vmhome SHA 960022c2031861ffdcd3a573d17394a005377b009aeafb8655bed57559cfbdb7
- vmremote SHA ba5e36374509dd996438c58760c3c293ed438c554840703eb5b1b637e9d43bbd
- local SHA cf3d3934abb8b48c431f46b6d60e446adfe994a375f1d4cba8c1779bd0ec6b1a

Episode25 ce6602cb1f36 remains frozen: 253 inputs, 56 host / 37 table controls,
three binaries, four production source-page native modes PASS on #216.
Older artifacts and architecture review are in HANDOFF-20260908.md and the
end of executor-mm-design-20260909.md. Later notices supersede their stale
"active" or "no deployment" statements.

## Latest measured failure: read-only upgrade plus logging lock cycle

B/mm-readonly27-probe-1 is a **failed isolated diagnostic**, run against frozen
exception26, not origin27. Launch script B/mm-readonly27-probe-launch.py,
driver B/mm-readonly27-probe.py, launch log B/mm-readonly27-probe-launch.log.
The launcher finished exit 1 and retrieved all artifacts.

Program: `vmctx/tests/control/readonly-upgrade.c`, also frozen as
B/readonly-upgrade27.c, SHA
`e0044484c1832998b06840af3a6c6bca080d93f2a5ec010696e25c2c43fc7a21`.
Its local native control and actual AMD native control both PASS. The program:

1. Seeds a memfd with a nontrivial 4096-byte pattern.
2. Maps it MAP_PRIVATE/PROT_READ and checks all executor bytes.
3. mprotects it read/write and changes every byte.
4. Has the source kernel write that mapping into a pipe, then reads it back.
5. Checks changed bytes and that the original private file stayed unchanged.

Guest outcome: timeout/status124, native fault deadline count **+10**, kernel
failure detector true because of elapsed fault deadline events. MM, module
reference, retained context and transfer-ticket counts returned to zero; no
survivors. Boot did not change. **Do not call this a clean run or repeat it
unchanged.**

Key logs: B/mm-readonly27-probe-1/readonly-upgrade/{home,remote,run}.log,
B/mm-readonly27-probe-1/live-kmsg.log, result.json, rows.json, native.json.
B/readonly27-first-deadlock.txt preserves the first scoped waiter/monitor stack.

Evidence, keeping observation distinct from inference:

- Source mapping MM117 at 0x7ffff7ffe000 was read-only, native CAPTURE **COPIED**,
  ticket1351, class0x33, page record NONE, generation0.
- Executor installed that copy through its backing object and ACKed it.
- Source mprotect completed, ticket18, and executor access journal seq3 granted
  PROT_READ|PROT_WRITE for the same page.
- Pipe write ticket19 and read ticket20 both returned4096.
- The next guest syscall is **write(fd2, stack buffer 0x7fffffffc740, length92)**
  at RIP0x450226. It faults on source stack page 0x7fffffffc000.
- Source task38773 is blocked in `fault_in_readable -> generic_perform_write ->
  ext4_buffered_write_iter -> vfs_write -> vmctx_do_monitor_syscall`.
- Its service monitor38770 is blocked in **fdget_pos -> mutex_lock -> ksys_write**,
  also writing fd2. The monitor had NOT taken the guest fault event.
- `local-here.sh:158` redirects vmhome stderr to home.log. `vmhome.c:4505`
  forks the source program and execs it without replacing stderr. The harness
  comments at local-here.sh:200 explicitly confirm they share fd2.
- Thus guest stderr write holds the shared open-file-description position lock
  while faulting; the monitor tries to log to that same description before
  servicing the fault. This is an observed lock cycle. More logging can cause
  the failure report to deadlock.
- The syscall sequence/length is consistent with the test's byte-mismatch
  diagnostic. The actual diagnostic bytes did **not** reach the file, so do not
  yet claim a measured value for stale vs expected bytes.

Immediate work on resume:

1. Fix the diagnostic/guest-output lock dependency so a guest memory fault
   cannot wait on the monitor logging to a guest-held file lock. This must hold
   by construction, including normal diagnostics, not merely by disabling the
   probe's verbose logs. `dup` preserves the same open file description;
   reopening the same inode can still contend on filesystem write locks. Design
   a separate diagnostic sink or independently drained capture path; inspect
   all launch modes and fd lifetime/close-on-exec behavior. Add a controlled
   regression which forces guest output to fault while diagnostics are emitted.
2. Then obtain a complete read-only-upgrade failure result and instrument
   owner/reservation/native PTE states at transfer and mprotect. Fix underlying
   page ownership/lock ordering before checksum/content repair.
3. Revalidate targeted native races and source/executor controls, then focused
   guests (including group-exit), full suite, cross-host and real programs/GUI.

No production fix for this logging lock cycle or the read-only upgrade has
been written yet.

## Relevant ownership code and open architectural issues

Authoritative native source is **B/episode216-inputs/core**, 20 frozen files;
B/episode216-work/core was identical. Local `src/linux-7.0.14` is older and must
NOT become the next native baseline. Full current AMD build tree is
`/home/biwu/audit-20260907/mapping-kernel-7.0.14`.

- `core/kernel/vmctx.c` ~6679-6808: `vmctx_serve_capture` read-only path copies
  while leaving the source PTE and page record NONE/HOME. TRANSIT/REMOTE are
  honored only when already recorded. It does not grant a new exclusive owner.
- `core/kernel/vmctx-transfer-core.h`: caller-thread-owned full64 tickets and
  page guards. `vmctx_transfer_grants` recognizes TAKEN or GRANT; plain readonly
  COPIED is not an ownership transfer. ACK releases its guard without turning
  that page record REMOTE. Source/executor user wire currently ACKs every data
  reply but does not preserve this owner distinction for later write permission.
- Native transfer settlement uses exact retained ticket/guard under
  vmctx_mm_lock, not checksum identity. Legacy CTL_PGACK still exists and compares
  a checksum, but production CTXPAGE/INSTALLED no longer uses it.
- Executor `access_apply_span` -> `as_apply_map_siblings` -> PROTECT_MM can restore
  write permission. Trace the actual mprotect hook and ordering against source
  page guards, remote reservations, pending copies and COW obligations.
- `vmctx_pgrec_claim` ~4380-4465 waits for page TRANSIT/CLAIM or active recall guard
  under vmctx_mm_lock, avoiding waiting under mmap_read_lock when disallowed.
  An already present writable PTE can bypass a fault-based gate; inspect that.
- Native TAKE does not simply destroy all fork aliases: mapcount precheck breaks
  COW, relooks up the private folio, zaps target PTE and then freezes/copies. RO
  private file pages are not automatically eligible for that destructive TAKE.
  Do not just switch the readonly branch to TAKE without proving its lifetime.
- Source `source-custody.h` retains exact connection-owned tickets; cleanup
  ABANDON precedes native waits. Unconfirmed custody terminates the session,
  never fabricates TRANSIT->REMOTE. One collection per native connection thread.
- Remaining executor relaxed ancestor/current-object fallbacks and source zero
  fallback need provenance. Inherited holes and freshly created mappings must
  be distinguished. Source lineage currently lacks mapping-at-birth provenance.
- `pull_gen_check` can replace source bytes from retained/current object copies;
  this is diagnostic/repair history, not an established ownership proof. Review
  against the user's explicit checksum/one-owner requirement.
- Later capacity/debug/native-status issues remain: MAX_CTX256, regions4096,
  source incall512, shfile64, native MM256, generation32 wrap, DR registers/TF,
  Intel robust/clear_tid, and runany-x global pkill/ungraded paths before GUI.

## Completed changes during this continuation

### Exception26

Episode25 focused guests passed pf4/pf1/pf2 then failed pf3. Its deliberate NULL
SIGSEGV ended the source child; native CTL_EXCEPTION returns -ETIMEDOUT when
vc->dead wakes the assisted slot without sys_done. Source omitted ended metadata
and executor ignored it, then fetched from a dead source and killed the session.

- `source_exception_reply` validates CPU wire, saves binding, preserves runtime
  errors and requires retained native ENDED proof on ESRCH/EINVAL/ETIMEDOUT.
  Terminal reply carries exact native wait status and no CPU payload.
- `owner_takes_fault` uses a caller-owned reply, validates exact source binding
  and terminal framing, records source status, signals only that executor and
  observes its retained pidfd exit before reporting handled. No terminal SETCPU.
  Malformed outcomes or an unconfirmed stop fail the session closed.
- 18 new source/executor host modes; real pidfds verify unaffected MM peer.
- B/exception26-baseline.{json,log}: frozen episode25 ignores terminal reply,
  expected exit1, clean teardown. Corrected targeted controls all pass.
- Initial fixture compile lacked sys/prctl.h; first executor fixture omitted MM
  registration and hit its alarm. Those failure logs remain; corrected controls
  and complete host suite pass. Do not confuse fixture failures with native runs.

B/mm-exception26-focused-1: **pf4, pf1, pf2, pf3, ns1 PASS**. pf3's SIGSEGV,
SIGILL and SIGFPE all report correctly and parent survives. **group-exit FAIL97**
with missing receipt at0x4dc000; campaign stops, so pf5/ex1/ex3 not run. No kernel
failures/counter deltas on this run. Launcher37311 closed exit1, artifacts copied.

### Origin27

Group-exit's missing receipt was local COW bytes mislabeled PULL_BYTES in the
no-region landing path. It then tried to ACK a source episode that never existed.
This is a distinct bug from exception lifecycle and from the pending lock review.

- New PULL_LOCAL distinguishes local protected COW/object bytes from source data.
  Only real source bytes select source ACK/generation paths. A maptrace records
  origin, MM, address and episode for no-region landing.
- Fixed missing ACK when actual source bytes land via a region's backing object,
  and when an exclusive recall maps an already retained local folio.
- Strict ACK validation remains; no empty-receipt bypass was introduced.
- New `tests/control/fault-page-origin.c` exercises actual fault code, sockets,
  sparse objects and native boundary. Seven modes all pass in the full host run:
  local-cow, local-cow-region, source, source-region, source-object,
  source-region-object, source-loan-map.
- Frozen exception26 baseline local-cow exits97 (B/fault-origin27-baseline-local-cow.*).
  Baseline source-region-object and source-loan-map omit ACK, expected exit1
  (B/fault-origin27-baseline2-*.{json,log}). Clean ownership on all three baselines.
- Early fixture-only attempts lacked gettid passthrough/unistd declaration;
  first failed compilation accidentally ran the prior fixture binary. Those
  artifacts are preserved and not credited as baseline proof. The exact frozen
  baseline builds3/4 and successful full-host controls are the evidence.
- New readonly-upgrade probe is included among the 257 frozen inputs but is
  NOT counted among the 81 host controls. Its native pass does not imply guest pass.

## Host and operations state at handoff

AMD: biwu@10.0.0.229, native #216
- release7.0.14-vmctx-audit-recall1-episode216
- boot c6d9fc7e-d3b7-49a8-a3e4-280c3d286fd8 (rechecked at handoff)
- loaded module `/home/biwu/audit-20260907/episode216-native/vmctx.ko`
- module SHA4ce32f62a28ac49ef0ddd50ed99cc55867814187650e28515a9625be4fcd075f
- kernel/vmctx.c SHA16d7d7977c20141b1c091b9da2d98f5685312ec7f97c744144403ac43c66f952
- run/ctl472/473; original47 native cases + custody2 + production-source4 passed.
- **Current counters [MMlive, refcnt, handles, take_lost, deadline_hits,
  transfer_live, transfer_abandoned] = [0,0,0,1,11,0,4].** Rechecked after cleanup.
  Deadline11 includes +10 from the latest failed probe; old value1 is stale.

Intel: root@10.0.0.30 via AMD ProxyCommand, unchanged #70
- release6.18.35-0-lts-audit-handle70
- boot871c548a-81de-4730-8e19-4207f0fcd5aa
- module/root/audit-handle70/vmctx.ko
- SHA94246f157fb430407348fe0c91d4132ad8613e75f2259b9c07c114ed8fbfc1c2
- run/ctl470/471;21 native + MM registry1 passed earlier; not touched this turn.

SSH key `/home/desktop/lan-boot/config/id_vmctx`; BatchModeyes,
StrictHostKeyChecking=no, UserKnownHostsFile=/dev/null, ConnectTimeout8,
LogLevelERROR. AMD root wrapper:
`SUDO_PASS=biwu SUDO_ASKPASS=$HOME/askpass.sh sudo -A sh -c <properly quoted command>`.
Intel SCP requires -O; use explicit keyed ProxyCommand, no unkeyed nested SSH.

No overlap of native builds/package/install/reboot/native tests/guest campaigns.
Local userspace checks can run alongside frozen native/guest diagnostics.
Use unique output directories and ports, verify boot/module/input hashes and
before/after counters, always retrieve outputs even on failure. Current unused
origin27 guest campaign has not been prepared; exception26 used31900+4i,
readonly27 used32000. Do not reuse failed artifact names.

Use `vmctx/tests/owned-run.py --report ... --timeout N /absolute/binary mode`
with NO `--` separator. Check complete/status/survivors/signalled/error/timed_out.
Native wrapper B/native-run-208 remains mandatory for new native controls.
Avoid global kill sweeps. All sessions from this turn are closed.

Large artifacts belong on D. Native module staging belongs on Linux FS because
case-colliding module filenames exist. Preserve HTTP PID239803/session89055 and
dnsmasq141953. Recovery HTTP root `/mnt/d/VM/lan-boot-recovery-20260908/http`.
boot.ipxe stays safe #63 recovery audit-runtime63-repacked, no auto-loaded module,
SHA5e742975650e50885a9573e06f06319f1936d03729b1ebb405aa9ff4c726af25.
AMD GRUB default#181 and all one-time boot images remain preserved.

Last **complete** focused runtime is still proc8 (973d66f9bed5). Last full
78-guest campaign remains exec-route3: **76/78**, fx1 and exec-shared-mm failures.
Firefox not run. No all-green claim is justified.
