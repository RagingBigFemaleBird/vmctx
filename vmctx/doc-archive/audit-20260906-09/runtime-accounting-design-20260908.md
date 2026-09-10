# Execution CPU time — staged implementation, validation pending

The cpu-runtime control proves that source thread and process CPU clocks omit execution on the executor. Native: about 105 ms wall and CPU. Loopback: about 108 ms wall, 34 microseconds thread CPU. GPG uses CLOCK_THREAD_CPUTIME_ID and doubles its calibration work until timing out. Forwarding a clock syscall to the source is necessary but insufficient: the source task must own the remote execution accounting as well.

The wire must carry measured, cumulative execution CPU nanoseconds for each execution context, with a lifetime identity and monotonic, idempotent crediting. The executor must never decode clock IDs or source syscall numbers. Wall time, time waiting for faults/syscalls, and time descheduled are not CPU execution. Source OS adapters determine how those credits enter their accounting, timers, limits, and process lifetime rules.

Implementation and required invariants:

- Add a native runtime query/credit ABI, versioned and bounded, distinct from the fixed CPU-state ABI. READ obtains the executor context's cumulative measured execution. CREDIT accepts a context-lifetime identity and cumulative count on a parked source service task. Reject reversed counts, changed identities, invalid roles, reserved fields, unsupported adapters and overflow; equal repeated credits are no-ops. Copyout failure must not allow double credit on retry.
- The existing native execution_enter/leave pair encloses a hardware entry and exit-state capture, excluding source calls and page-service waits. Measure delta task_sched_runtime(current) outside vmctx_mm_lock to avoid rq-lock inversion. It excludes descheduling and, with scheduler IRQ accounting, interrupt CPU time. Document the inclusion of native entry/exit adapter costs. Check lock/lifetime ordering before adding fields to the context. Self-tests without a native context must stay unaffected.
- Extend the protocol request (new version) with the cumulative count and identity. Capture it before forwarding CPU state for ordinary calls and exceptions. The source credits before dispatch so CPU-clock calls observe completed execution. Children start new counts; exec preserves task CPU time. Do not place runtime metadata in XSAVE or infer it from source/target OS layouts.
- Linux must keep remote execution distinct from se.sum_exec_runtime: changing the scheduler's internal fair-share runtime would corrupt local scheduling. A per-task external execution counter can enter task_cputime, task_sched_runtime's public result, thread_group_cputime/read_sum_exec_runtime, task_cputime_adjusted, exit aggregation, and POSIX CPU-clock samples. Audit both virtual accounting configurations. Do not charge remote execution to a source host CPU's /proc/stat totals. Active group CPU-timer caches need consistent user-time and execution-time increments.
- Credit on calls alone does not support pure-compute CPU timers or limits. A bounded execution checkpoint must return CPU state and runtime to the source periodically (or at a source-provided execution budget). The source kernel accounts and processes its pending events, then returns architectural state. The executor must not know Linux signals, timer IDs, ptrace modes or native clock encodings. The common native run loop after a backend CONTINUE can surface the checkpoint; hardware external-interrupt exits already return there. This should use a distinct event and source boundary operation, not a fabricated syscall or exception.

Required discriminators: fixed computation advances thread/process CPU clocks; sleep and syscall/page waits do not become user computation; sibling work goes to the correct thread and group; fork resets child CPU totals, exec preserves them, wait/rusage accumulates exited children correctly; replayed credits do not count twice; pure computation triggers source CPU timers/limits without a guest syscall. Existing generic CPUID, full XSAVE, signal, exception, snapshot and mapping controls remain required on AMD and Intel.

Related observations requiring separate fixes:

- flock2's connection failure was SETCPU racing source sibling exit_group. Authored vmhome now reports the source lifecycle result before dispatch, sharing the bounded death check used after GETCPU. Ten focused flock runs plus group-exit, signal-state, exception-state, fork-late-sibling and the corrected lddnames oracle have passed with complete cleanup. Source task_dead_status's pre-existing SIGKILL fallback for already-reaped tasks still loses exact exit status and should ultimately be replaced by a retained native lifecycle record.
- Node's io_uring setup maps multiple extents of one source inode. sh_shared_slot reserves only the first observed extent, and rejects later growth with EOVERFLOW. Stable shared-object identity needs an extent model that can grow without aliasing other objects; arbitrary fixed reservations are not an architectural solution. io_uring also has kernel writers outside ordinary user-memory fault paths, so merely accepting its larger offset does not establish coherence.
- Source dispatch currently skips syscall entry/exit work for ptrace. Native source assistance must preserve stopped task registers and control-flow changes, including breakpoint exceptions and syscall tracing. Do not teach the executor Linux ptrace requests.


Staging status (2026-09-08): native patches 0019 (6.18) / 0023 (7.0), the runtime UAPI and v14 userspace transport are authored. The accounting audit also includes task_cputime_adjusted's initial runtime sample, so parked tasks and wait/rusage retain credits. Executor enter/leave starts and joins a repeating hrtimer which kicks its execution task; this ensures VM exits on tickless CPUs without counting wall time as CPU time. The timer is inactive during page service and source calls. Checkpoints preserve AX and return the source's full CPU state. Protocol metadata is rejected on unrelated operations.

The first AMD #199 core build exposed the prior boolean request-kind field (changed to u8 for the third kind) and root-owned dependency files. It stopped before an image was produced. Its log is preserved as runtime199-kernel-build-1-permission.log. The corrected root build compiled the changed core and is now rebuilding all kernel modules under the distinct release 7.0.14-vmctx-audit-recall1-runtime199. No installation or remote runtime validation has happened yet. Kernel sources are frozen throughout the build; Intel builds run afterward.

New controls: runtime-state exercises native credit replay/epoch/copyout/role validation, source clocks and rusage, periodic checkpoints, parked-time exclusion and AX preservation. cpu-timers covers process/thread/virtual CPU timers, sleep exclusion and RLIMIT_CPU during pure computation. cpu-lifetime covers per-thread ownership, process totals, fork reset, exec persistence and wait/rusage. The latter two pass natively in WSL; remote validation is pending. The authored suite now has 66 cases; the last completed green frozen suite has 63.


A further control, shared-grow, passes natively and fails on frozen 4d47dc8c1699 / AMD #198. Source sh_shared_slot returns EOVERFLOW when an initially one-page memfd grows after a neighboring object has been assigned backing storage; executor exits 97. Complete cleanup and counters 0/0, raw shared-grow-baseline-1-raw.tar.gz. The authored suite now has 67 cases. Fixing this requires stable object/page identity that supports growing extents, not enlarging a reservation over the next object. Node additionally needs correctness for asynchronous io_uring kernel writers.

Read-only tracing investigation while the kernel builds: the strict GDB baseline reaches PTRACE_SINGLESTEP (source call 101 request 9), then executor RIP 0x555555555093 reports an exception rejected with -EINVAL and exits 242. Current source native exception admission excludes x86 #DB (vector 1). Source syscall entry/exit tracing work is also absent. Supporting debug execution requires correct architectural debug cause/state transport; do not infer a Linux signal on the executor. Evidence extracted under logs/audit-20260907/ptrace-baseline/.


Frozen userspace runtime-tree is b2f528aee9a8 for all three native ABI binaries. Host controls pass. runtime-old-kernel-1 on AMD #198 verifies that the executor refuses before source START when native runtime INFO is unavailable: source logs zero requests; guest output is empty; executor exits 1; complete cleanup and mm/refcount 0/0. This is an expected compatibility rejection, not a guest success. Raw runtime-old-kernel-1-raw.tar.gz is preserved.


ptrace-step is a minimal independent native PASS / remote FAIL discriminator. On frozen 4d47dc8c1699 / AMD #198 it reports child status 0xf200 (exit 242) on the first single-step, with complete cleanup and 0/0 counters. The authored suite is now 68 cases; frozen runtime-tree still contains 67 until this additional test is copied. A syscall-tracing repair must also handle tracers changing the syscall number/arguments: PREPARE cannot assume an incoming identifier stays unchanged, and mapping/clone effects must describe the actual source execution. Conservative snapshot preparation for traced tasks or a native post-entry admission phase is needed before allowing tracer-created fork/clone calls. Simply adding ptrace stops to direct dispatch is insufficient.


AMD #199 full kernel/module build completed successfully with unchanged config and verified input hashes. Release: 7.0.14-vmctx-audit-recall1-runtime199. Image SHA256 dc2cf763b409fa7fc99607fb975c9d04d58e193826c94797951372d73d1a1832; external module a407e512b6374433bdff52aa62fa2174e893ffbfcfa8e5738266ae28572d9ec2, matching distinct vermagic. Existing module format/frame/objtool warnings remain in the preserved build log. Module installation and fresh initramfs generation are running. Intel build is deliberately deferred until AMD native and focused CPU accounting controls validate the implementation, so a discovered defect can be corrected before another full build.

AMD #199 booted with the rebuilt module. Native cpu-instructions, quiesce-state
and cpu-state pass; runtime-state's source half passes credit replay, epoch/range
checks, bad-copy retry, source CPU clocks, rusage and boundary AX preservation.
The executor half fails at its first instruction: installed AX is
0x123456789abcdef, but capture at #UD shows AX=0 while R12 retains the marker.
The native vmctx_run return passes through Linux's syscall wrapper, which writes
its zero return over the installed AX. This is a pre-existing initial restore
bug, exposed by the new exact-state control. Raw runtime199-native-1-raw.tar.gz
and runtime-state-instrumented-1.log are retained; counters return to 0/0.

A separate diagnostic seeds AX in its first guest instruction to isolate the
periodic checkpoint mechanism. It passes all 12 checkpoints, SETCPU updates,
parked-time exclusion and poisoned reply-return checks. It does not replace the
original failing test. Evidence: runtime-checkpoint-isolation-1.log.

A second discriminator, restore-gate, attaches and installs CPU state but never
issues RESUME. #199 leaves the executor live after its five-second wait expires
(native demand fault can prevent the counter from advancing). The test fails
and cleans up to 0/0. The gate must not time out into execution. Returning a
native error through an already replaced register/TLS/FPU frame would also be
incorrect; an incomplete restore must terminate the executor task.

Patches 0024 (7.0) / 0020 (6.18) preserve restored AX through native syscall
return, clear orig_ax restart semantics, require WAIT_MONITOR for RESTORE,
terminate unreleased restores, and serialize executor RESUME against CPU control
with release/acquire publication. Source service RESUME remains independent:
source syscall dispatch may be waiting for a fault that requires this reply.
No source syscall meanings are added to the executor. AMD #200 is built and a
one-time test boot requested. Its internal layouts and ABI are unchanged from
#199; all 14,519 built-in symbol records match #199's complete Module.symvers.
The first verification correctly stopped on an old standalone vmlinux.symvers
left from July; verification against the complete #199 symbol table resolves
that stale-artifact discrepancy without ignoring CRC differences. The #199
module and initramfs are reused, and its boot image is retained. #200 validation
is pending; Intel remains #62 and has not been built during the AMD build.

AMD #200 boot 00dd7696-62c8-430b-bc40-f6cc42affdbc passes all six native rows:
cpu-instructions, quiesce-state, cpu-state, runtime-state, runtime-state with
initial AX=-512, and restore-gate (ETIMEDOUT process status, zero guest stores).
All seven focused loopback controls also pass, including process/thread/virtual
CPU timers and pure-compute RLIMIT_CPU, fork reset, exec persistence and exited
child rusage. Compute: 134.39 ms wall, 127.80 ms thread CPU; sleep: 203.58 ms wall,
0.169 ms thread CPU. Every run cleans up to mm/refcount 0/0. Evidence:
runtime200-native-1-raw.tar.gz and runtime-loop-1-raw.tar.gz.

GPG's original pipe-fed test now completes in about six seconds with exit 2
instead of timing out during calibration. Diagnostic stderr says Invalid
passphrase. Directly supplying the same literal test passphrase completes both
encryption and decryption, with strict native/guest exit zero, byte comparison,
RT-OK and cleanup 0/0. That diagnostic does not replace the failing pipe test.
Raw runtime-gpg-1, runtime-gpg-diagnostic-1 and runtime-gpg-direct-1 archives are
preserved. progs.sh now keeps GPG stderr instead of suppressing the evidence.

The new byte-io discriminator passes natively. Remote ordinary memory passes;
mlocked anonymous memory fails on its first one-byte read: return is 1, but the
buffer still contains its old 0xa5 byte. It executes zero runtime checkpoints,
so this is independent of periodic CPU accounting. The native TAKE log shows
claim refused AFTER the zap, refs=2 versus expected 1 (want=0 + own reference),
mapcount=0, LRU=1, anonymous. The source then returns HOME/absent CLAIMING until
its 255-ask healing branch returns ABSENT, and the executor uses a stale retained
copy. Raw byte-io-baseline-1-raw.tar.gz includes both the baseline and page trace.

Read-only root-cause audit: zap of a locked page calls munlock_folio, which takes
a reference in the per-CPU mlock batch. vmctx_folio_precheck can drain batches
before the zap, but vmctx_folio_claim only drains on !LRU, incorrectly excluding
the reference newly created by this very unmap. mm/swap.c's lru_add_drain_all
includes mlock batches; the final exact-count freeze must drain on a mismatch
regardless of LRU state. Its post-unmap failure must also not be translated into
permission to use an old copy. Kernel repair is pending while Intel's full #63
build is frozen; no kernel files are being edited during that build. The
expanded authored and runtime test suite now includes byte-io (69 cases) and is
running on AMD #200. The last completed fully green suite remains frozen 63.
