# Continuation: runtime and native validation

Work remains active; do not treat this as an all-green result.
The corrected diagnostic oracle regrades both lifetime37 and retire38 full
suites to **72/78**. Earlier counts below describe the original checker.
Evidence base B is
`/mnt/d/VM/lan-boot-recovery-20260908/logs/audit-20260907`.

The previous full quiet36 suite completed 75/78, with `fx1`,
`mprotect-exit-race`, and `shared-snapshot-mutate` failing. Its seven counters
remained zero and there were no kernel failures, but those facts do not
override the runtime errors. Original failures remain in B/mm-quiet36-suite-2.

## Native state

Recovered and completed the already-running #220 build/package. Full kernel,
6,891 modules, external vmctx module, and initramfs checks pass. A one-time
GRUB entry preserves the default #181 and existing images. #220 passed all
56 native vmctx controls on boot 792a3378-8512-4d45-b13c-65404e698413,
B/hmm220-amd-native-1. Intentional controls account for one lost-copyout
event, one deadline event and three abandonment events; live resources zero.

HMM attempt 1 stopped before testing because a common wrapper binary differed
from the manifest. Attempt 2 used its own hash-verified wrapper and passed all
11 operation probes, including the previously failing fork. Unload then
panicked: upstream `test_hmm` lacked device.release. The off-host stream at
B/hmm37-offhost2-kmsg.log records `device_release` from `rmmod`; the complete
pre-cleanup rows and pstore are retained. No complete pass is credited to it.

The machine recovered through default #181 and returned to verified #220.
Current boot: 19414c6a-2fb4-4444-a8f1-a177cc7c114a.
Module B's remote counterpart `hmm220-native/vmctx.ko`, SHA256
b2ca0410865cd7db50031ac5fb2cc3039d803ff7aaa1875bd3aade55a0797f86.

Patch 0044 gives the static HMM devices a completion-based release, drains
device references before freeing resources, initializes chunks before cdev
publication, and cleans partial chunk allocation on failed setup. The module
is frozen in build/hmm38-module and B/hmm38-module-inputs.*. All 11 HMM probes
and unload pass (B/hmm38-native-1), with complete local/off-host log boundaries,
unchanged boot, zero native counters and no kernel failures. The byte oracle
and HMM permission assertion are unchanged. HMM is still a substrate proposal,
not an integrated vmctx ownership repair.

## Monitor candidate lifetime37

Two measured runtime fixes:

- Permission fanout accepts EAGAIN from a sibling only after that exact
  retained pidfd reports exit. EAGAIN for a live sibling or originating task,
  and unrelated native errors, still fail the committed update.
- COW_SPENT is completed local preservation. A repeated source lineage request
  cannot re-arm the snapshot or copy the parent's current bytes into old
  children. A new fork explicitly re-arms OWED. Missing OWED bytes still stop
  the grant. This does not resolve all historical COW provenance concerns.

Seven production-helper regressions pass. Frozen quiet36 fails the spent
snapshot and delayed-sibling-exit regressions, with complete owned cleanup;
the exact logs are B/lifetime37-*-baseline.*. Added cancellation diagnostics
to source recalls without changing their failure outcome. This identifies
whether an ABSENT reply followed native mapping invalidation.

Frozen source id 772a45d272e2, 286 files, three Werror executables,
complete host and table checks pass. B/mm-lifetime37-user-build-complete.json
and -user-tree.tar.gz. The generic build freshness, owned-run, oracle failure
injection, and module inventory self-tests also pass locally.

The first guest launcher stopped at SSH preflight during HMM panic recovery;
no guests ran. The updated launch B/mm-lifetime37-focused2-launch.py passed
fx1, mprotect-exit-race and the 128-round/four-observer shared-snapshot mutation
case on the recovered boot. Twelve further shared-snapshot mutation runs passed
in B/mm-lifetime37-recall-1. Both campaigns retained the same boot, zero
counters, complete owned cleanup and clean diagnostic/kernel oracles. The
intermittent recall failure is still unexplained; passing repeats do not
establish its cause or repair. The full 78-case suite PASSED in
B/mm-lifetime37-suite-1, launch mm-lifetime37-suite1-launch.py: 78/78,
zero lost pages or deadline hits, zero live/abandoned transfers, unchanged
boot, complete cleanup and no diagnostic/kernel failures. Its exact inputs
and binaries were checked before and after execution. A heavier diagnostic
campaign (1024 rounds, 16 mutators) failed on its third run in
B/mm-lifetime37-recall-stress-1. The byte oracle passed, but one stale-serve
diagnostic fired and the existing generation repair substituted retained
bytes. Seven counters stayed zero and cleanup completed; this is still a
failed run. The fourth run did not execute. The suite grading
failure-injection self-test also passed. No cross-host or browser validation
is claimed.

## Scope and reading

Read the original spec and principles, latest handoff/audit conclusions,
current runtime paths for COW, page recall/transfer/receipt, context identity,
mapping fanout, snapshot protection, teardown, diagnostics and harnesses.
The repository has roughly 77,000 lines across current userspace, module and
control sources, plus core-kernel patches and archived history. This continuation
does not claim a fresh every-line reread of all of them. Earlier read ledgers
are historical evidence, not a substitute for reviewing current changes.

## Mapping retirement candidate retire38

The stress trail names MM1750/page7ffff47eb000: shared-page capture gen1,
then a private-page reply gen0 without an intervening executor retirement.
The private SET path punched its object but left retained bytes and loans;
its late UNMAP could then be skipped because the new construction was already
published. The shared SET path already ran full range retirement.

Two production regressions fail against lifetime37, with complete cleanup:
B/lifetime37-mapping-replace-baseline.* and -mapping-diagnostic-baseline.*.
The latter directly proves generation disagreement overwrites the received
buffer from retained storage. Both regressions pass on the working candidate.

retire38 runs the existing full retirement before publishing either private
or shared SET. Range retirement now orders local preservation, byte/loan
retirement and native unmapping against page workers through the coherence
mutex, inside the caller's layout order; this scope makes no network call.
The generation check accepts a const buffer and reports disagreement without
substituting bytes. Diagnostics and their failing integration oracle remain.
This does not establish mapping identity for all outstanding network replies,
nor repair native remote-pte absence or shared external writers.

Frozen retire38 source 529a5f6cc4f8 (287 input files) passes its three Werror
builds and full host/table checks. B/mm-retire38-user-build-complete.json.
Its fx1, mprotect-exit-race and shared-snapshot 128/four-mutator focused campaign
also passes, with unchanged boot, clean diagnostics, zero counters and complete
cleanup: B/mm-retire38-focused2-1.

The shared-snapshot observer now checks all 16 KiB of each fresh anonymous map
for zero before writing. This strengthens the byte oracle that previously
masked the generation substitution. The extra test was authored after the
runtime bundle was frozen; it is staged separately as B/snapshot-fresh38.c,
SHA256 1e5e0ec1b2d53ceca937490d87beec48459ce44abfb05154d861650e87704dc3.
The six-run stress and full-suite drivers hash this supplemental source and
compile it explicitly. They retain the unchanged 287-file runtime manifest;
the extra test does not change the three monitor executables. The native
baseline passes. All six 1024-round/16-mutator stress runs PASS in
B/mm-retire38-recall-stress-1: byte oracles and diagnostics clean, seven
counters zero, complete cleanup, unchanged boot. The full suite with the
strengthened fixture finishes 77/78 in B/mm-retire38-suite-1: an1 times out
after source LAND fails with EBUSY at 0x7ffff7ffe000. Every cleanup completes,
the seven counters remain zero and the boot is unchanged. Its separate copy
is B/retire38-an1-failure. This is a failed suite, despite passing stress.

The source fault owns PG_CLAIM, but outbound TRANSFER_CAPTURE obtains a recall
guard before inspecting that state. It reports CLAIMING while retaining the
guard until the separate CANCEL call. LAND can encounter that guard and fail
EBUSY instead of completing the incumbent claim. The working native control
tests LAND while the refused capture's ticket is still retained. It is frozen
in B/capture-claim221-inputs; baseline driver B/claim221-amd-baseline.py is
reproduces the defect on #220: CAPTURE=0/status4, LAND=-1/EBUSY while the
refused ticket is still retained. Cleanup leaves MM/backend/context/ticket
counts zero. B/claim221-amd-baseline-1/rows.json and its failed driver are
preserved. No native fix is validated yet. Existing run-diagnostics.py also
misses the specific LAND error text; the timeout still fails the suite.

Candidate #221 adds capture-specific guard admission under vmctx_mm_lock,
rejecting an incumbent CLAIM/TRANSIT before installing any guard. Fault
admission uses that same lock. Local LAND/POKE guards keep their existing
behavior. Patch0045 replays byte-exactly over all 26 #220 core inputs. Frozen
inputs: build/claim221-work and B/claim221-inputs.tar.gz. The AMD full build
and packaging complete through B/claim221-build-launch.py: warning-free,
6,891 modules and initramfs verified. Module SHA256
b05c4ef6ed784f6dddd0b982beea1486f3834114a8d26f9930bfd444d600853b.
The one-time boot succeeds through B/boot-claim221-amd.py: #221 boot
9bc69d67-8344-48bd-bb93-5991eb055dd0. The 57-case native campaign PASSES in B/claim221-amd-native-1.
The incumbent LAND now completes while the rejected capture ticket is held. Default #181 and previous entries/images
remain. All seven affected guests PASS in B/mm-claim39-focused-1 with
clean diagnostics, complete cleanup, unchanged boot and no counter increases.
The full 78-case suite is now running in B/mm-claim39-suite-1.

Prepared follow-up gates use the unchanged retire38 source/binaries with a
separate diagnostic oracle B/claim39-run-diagnostics.py (SHA256
005eda9dd9e9e9f4b32729ab2d048896c996a2cad91b7612ee6f1ecd76c5a542).
Launchers: mm-claim39-focused, -suite, -breadth and -browser, each with
-launch.py. The seven focused cases are an1, dma1, fc1, hx2, pg2,
thread-reuse-lock and th9. Native negative controls will leave cumulative
lost/deadline/abandonment counts 1/1/3; follow-up gates require no increases
and zero live resources, on the exact native campaign boot. The full suite
retains the strengthened shared-snapshot fixture and all 78 cases.

Browser cleanup now binds a pidfd before comparing birth identity and sends
through that handle, closing its numeric-PID check/signal race. The two-file
supplement is B/browser39-fixture with its own manifest; runtime inputs stay
frozen. Its native and loopback validation is pending. Cross-browser process
ownership still needs review before use; no cross campaign is prepared by
these launchers.

The working diagnostic oracle now rejects the observed LAND error directly,
recall failures and direct STALE-SERVE messages even without a final summary.
Regrading finds six affected cases in each full suite. retire38: an1 (46 LAND
errors), dma1 (2), fc1 (1), hx2 (14), pg2 (2), thread-reuse-lock (1).
lifetime37: an1 (14), dma1 (1), fc1 (1), hx2 (15), th9 (1),
thread-reuse-lock (3). Both corrected suites are 72/78. The focused retire38
guests and all six strengthened stress runs remain clean under the new check.
Original manifests/results are preserved; the regrade and oracle verification
are in B/claim221-diagnostic-oracle.json. An initial check wrongly assumed a
single LAND error; reading all matches found 46 in an1. The actual repeated
failures remain visible. This oracle is supplemental to the frozen runtime
bundle and must be staged explicitly for future campaigns.

No breadth, browser or cross-host campaign has run in this continuation yet.
Prepared launchers must run sequentially after these gates, verifying the same
boot/module and complete cleanup.

## Intel read-object candidate #71

Intel still runs #70. Its exact frozen source/output hashes were verified
before preparing #71, 6.18.35-0-lts-audit-readobj71. The local build tree
src/linux-6.18.35 now contains candidate #71, not the deployed #70 source.
The previous inputs and build outputs remain in B/handle70-inputs and its
build-complete manifest; the pre-change verification is recorded in
B/readobj71-build-before.

Patch0029 ports read-only object population and preservation of old write
protection during vmctx mprotect to Linux 6.18.35. It adds the two executor
capabilities required by the current monitor. The 21-file replay is byte
exact; inputs are frozen in B/readobj71-inputs and its archive/manifest.
The external module sources remain the frozen #70 sources and are rebuilt
against the new kernel. The full kernel/module build completed through B/build-readobj71-intel.py.
B/readobj71-build-complete.json records the verified inputs and outputs.
Packaging is running through B/package-readobj71-intel.py. No boot defaults were changed,
and no #71 runtime or cross-host validation is claimed yet.

## User delivery requirements

Commit and push only after all required tests pass on both machines. Squash
all kernel changes to one vmctx commit per vanilla kernel baseline and export
the corresponding single patch. Consolidate scratch notes and intermediate
steps into durable documentation and the commit message, then remove the
scratch documents from the repository. Preserve raw failure and validation
evidence off-repository before cleanup. No commit or push has occurred.

## Latest gates and watch-write correction

AMD #221 passes all 78 suite cases under the corrected oracle in
B/mm-claim39-suite-1: no counter increases, full cleanup, stable boot and clean
kernel logs. Intel #71 packages all 3,767 modules and 347 initramfs module
files. The recovery HTTP service was absent and was restarted on port8080;
boot preparation stopped before mutation until reachability and the kexec
package were restored. All 105 network boot dependencies then verified.
Intel boot e8e036f7-5adc-4000-b0c2-abd4ba2c2306 passes all 26 native controls,
including five read-object permission probes, with complete capture cleanup.

The AMD real-program campaign is still running. gpgsym fails with an
unsettled source receipt, exit97; no passing credit is given to its XFAIL
label. A local sibling's nonpresent WRITE bypasses the watched-object path
(which previously covered only reads/execute), fetches from the source and
can skip the new receipt's landing because the local watch remains. Two new
production controls reproduce that routing failure on frozen retire38.
watchwrite40 maps the same watched folio read-only for writes too: the next
protection fault follows the existing COW/watch grant path. It preserves
bytes and COW obligations, issues no source request, and grants no write in
this mapping step. Both new and three existing watched-page controls pass.
B/watchwrite40-{baseline,fixed}.json retain results; native GPG validation is
pending. The new frozen runtime build is in progress.

Cross harness cleanup now uses a destination native subreaper report emitted
only after all children are reaped. The source keeps its existing owned-run
path. native-run --report tests preserve child status, reap child groups,
record timeout, refuse existing reports and leave unrelated processes alone.
The new remote-executor.py/runany-x.sh are not yet cross-host validated.

watchwrite40 source26fd84585fd8 (289 inputs) passes three Werror builds and all
host/table controls. The original breadth run is 68/72 under strict grading:
gpgsym, cpiort, gdbbt and nodework fail. The complete root-owned raw archive is
B/mm-claim39-breadth-1-raw.tar.gz; the unprivileged SCP copy is incomplete
because generated test files are private, so do not treat it as the full
archive. Cumulative abandonment becomes4; live resources remain0.

B/mm-watchwrite40-programs-1-raw.tar.gz tests gcc plus the four failing programs
with 180-second diagnostic budgets. CPIO completes in92.058s with its entire
byte comparison passing; the old90s limit interrupted active 512-byte reads.
The working harness now defaults CPIO to180s while honoring explicit limits.
GPG prints RT-OK and has no new custody abandonment, but its persistent agent
keeps the guest process tree live until timeout. The working harness now ends
that agent through gpgconf scoped to its unique GNUPGHOME. GDB still receives
SIGFPE at _start+4; Node still fails settlement of page7ffff7fb6000 before clone3.
The separately hashed programs41-fixture contains the GPG cleanup, CPIO budget,
explicit runtime root and stricter direct receipt-failure oracle. Its GPG-only
rerun is active in B/mm-watchwrite40-programs-2.

Native ptrace-break40b passes locally: change a child private text byte to
INT3, observe the trap and RIP, restore/rewind/continue, and prove parent code
unchanged. Its guest discriminator has not run. Cleanup uses a retained pidfd.
Browser40 fixture now requires normal Ctrl+Q application exit and uses a
native remote owner with a held stdin control pipe. The owner's local tests
pass normal exit, cancellation, SSH-style EOF, deadline and descendant cleanup.
An Intel transfer attempt timed out before staging; a later read confirms
Intel's same#71 boot and zero live resources. Cross/browser validation pending.
