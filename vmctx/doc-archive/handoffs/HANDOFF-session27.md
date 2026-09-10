# HANDOFF — session 27 (2026-08-21): fault-path review, no code changed

This session was a **review**, per the owner's steer: re-read the kernel and the
fault path to understand (a) how a fault can "race" for 2 s+ — the priority was
to understand the race, **not** to extend the deadline — and (b) whether there
are mm-mutation paths that are never caught. Nothing was built, no kernel was
rebuilt, no commit was made. `539d604` (session 26) remains the tip, local only.

Read: the core kernel (`src/linux-7.0.14/kernel/vmctx.c` + the mm/entry/futex/
exec hooks), the SVM module fault path (`vmctx/kernel/vmctx.c`), both userspace
halves, and — as evidence — the live `#148` counters and the archived session-26
failure captures on .229.

Box confirmed: local `src/linux-7.0.14` matches the box's build tree byte-for-
byte (only the two newest commits differ in the local checkout view); the box
runs `#148`, `vmctx_fault_deadline_ms=6000`, idle, `/tmp` clean.

## Findings, most to least actionable

### 1. The "2 s race" is a claim volley broken by a clock, not slow work
The owner's read is correct: extending the deadline treated a symptom. The pg3
pass-evidence on the box (`~/torture.pg3/pass-evidence.txt`) shows an **exact**
correlation across a 256-run pool:

- 7 runs carried a claim storm (`2540 / 2548 / 2598 … simultaneous claims
  yielded to the source`) **and those same 7 carried `1 gave up waiting`**;
- the other 247 runs had `0` yields and `0` give-ups.

A storm of ~2500 yields is ~1.3 s of the two machines volleying claims for ONE
page at the pthread-join, resolved not by anyone *winning* but by the
destination's **1200 ms INFLIGHT grace timer** (`pg_serve_conn` GET handler in
`vmctx/user/vmremote.c`) finally breaking the tie. That is why raising the
kernel deadline "works" — it guarantees the grace clock fires inside it. The
fault is not slow; ownership at a crossing is not *decisive*. The two-phase
hand-over (NOTES §2.14) started this and did not finish it: the volley is the
unfinished part. **Fix direction: make a crossing decisive (source wins
immediately, no volley), not a longer timer.**

### 2. The residual deaths are invented-zeros, not deadline kills
The archived pg3 failures (`~/torture.pg3/other.57`, `other.112`) are
**exit-242**, shape unambiguous:

    context 190653: 0x7ffff77f2000 answered ABSENT and filled with an empty
    page (rip 0x469cc2)
    guest ended: exit status 242

A thread-stack page crosses to the source and back, its content evaporates, the
source answers ABSENT, the destination invents zeros over live memory, the
thread RETs/derefs through a zeroed word and dies. **Every source-side crossing
counter reads 0 in the failing run** (`serves whose take found nothing: 0`,
`GONE-record CLAIMING: 0`, `take generation … STALE-SERVE: 0`, `GETs that
crossed a serve: 0`), so it is a sub-µs race beneath every instrument that
exists. This is the documented §6.1a / §2.16 residual. The proposed next
instrument — the **kernel TAKEOBJ re-read-after-copy detector** (re-read the
folio after the unmap+kick and before the truncate; if it differs from the
copy, a guest wrote through a stale translation after the unmap; count it and
photograph which CPU / context) — is still **unbuilt** and is the right way to
name it. Core-kernel change → rebuild + reboot of .229 (gdm stopped first).

### 3. Confirmed uncaught mm-mutation path — but dormant on this box
The change-log notifier logs **only `MMU_NOTIFY_UNMAP`** (`vmctx_mn_range_start`
in the core kernel), plus one explicit `MADV_DONTNEED` hook
(`src/linux-7.0.14/mm/madvise.c`, the `vmctx_note_discard` call). The **KICK
notifier** — `arch_invalidate_secondary_tlbs` (`vmctx_mn_arch_invalidate`), the
only thing that forces a *running* guest OUT when reclaim / migration /
compaction / THP-collapse / KSM / NUMA balancing clears one of its PTEs — is
registered **only when `vmctx_mn_enable=1`, and it is 0 on the live box**
(`/sys/module/kernel/parameters/vmctx_mn_enable=0`, `vmctx_mn_calls=0`).

The design leans entirely on the standing `tlb_ctl=1` flushing the guest ASID on
every VMRUN **entry** (`svm_vmrun_once_gpr`). That only closes the window on the
guest's *next natural VMEXIT*: a store the guest makes through its stale
ASID-TLB entry **between the PTE-clear and that exit is lost**. That is exactly
a "path that modifies mm and is never caught," and it matches the §6.1a
photograph ("a guest writer storing through a translation to a folio that is no
longer the object's… whatever replaced that folio did not kick").

It is **not what is biting now**, and the live vmstat proves it:
`pgmigrate_success=0, thp_collapse_alloc=0, compact_stall=0, pswpout=0`
(15 GB RAM, ~10 GB free → no memory pressure; shmem-THP `never`; single NUMA
node so `numa_pte_updates=0`). So it is a latent-correctness hole that would
corrupt under memory pressure or on a smaller destination box, not the current
residual producer. Note the deliberate kick (backend->invalidate on TAKE/
TAKEOBJ/PROTECT/PROTECTOBJ) DOES work — `INTERCEPT_INTR` is set in the VMCB
(module line 1267), so its broadcast IPI forces a running SVM guest to VMEXIT;
the hole is only the *undeliberate* PTE-clearers above.

`MADV_FREE` is likewise uncaught (only `MADV_DONTNEED` is hooked; MADV_FREE runs
`madvise_free_single_vma` under `MMU_NOTIFY_CLEAR`, which the log ignores), but
its old-or-zero contract makes it low-severity and it is unlikely to be a
current bug.

### 4. Two live counter anomalies (`/sys/module/kernel/parameters/`)
- **`vmctx_take_lost=2`** — the counter AUDIT/PRINCIPLES say must stay ZERO
  (a page unmapped, claim refused AFTER the zap, never served). Non-zero on the
  running kernel → the take precheck (`vmctx_folio_precheck`) is missing a case,
  twice this session. Per its own log line, "if this is ever non-zero the
  precheck is missing a case, and the case is in this line."
- **`vmctx_mm_overflow=1`** — the 64-slot mm-tracking table (`VMCTX_MM_SLOTS`)
  overflowed once under load. An overflowed mm has ALL its fault hooks fall
  through → silent zero-invention with nobody asked (do_anonymous_page). 64
  slots is tight for an 8-way pool of fork-heavy work; this is a correctness
  cliff, not just a stat. Consider raising `VMCTX_MM_SLOTS` or making overflow
  fatal-loud rather than a warn-and-fall-through.

Other live counters for context (cumulative over session 26's pools+browsers):
`vmctx_fault_deadline_hits=4412` (the deadline still fires a lot — these are the
residual failing runs + netsurf's blocked contexts, NOT a single workload; the
archived logs don't contain the individual lines, dmesg had rotated),
`vmctx_af_retry_killed=4377`, `vmctx_ffile_invent=78` (refuse off, so fell
through), `vmctx_zeropage_forbidden=117`, `vmctx_syscall_pages_lost=0`,
`vmctx_syscall_calls_lost=0` (good).

## Bottom line
Extending the deadline was a symptom fix. The real defect behind the "2 s race"
is a non-decisive crossing — a claim volley resolved by the 1200 ms grace clock.
The residual 242 deaths are a separate sub-µs crossing race that invents zeros.
The genuinely *uncaught* kernel mm path is the disabled KICK notifier
(`mn_enable=0`), latent only because this box has no memory pressure.

## Highest-value next moves (not started — review only)
1. **Name the crossing race**: build the TAKEOBJ re-read-after-copy detector in
   the kernel (core change, reboot). This is the only instrument that can catch
   the §6.1a/§2.16 sub-µs producer without an armed page that masks it.
2. **Make the crossing decisive**: kill the claim volley so a join-crossing is
   won at once by the source, instead of ~2500 yields timing out on the 1200 ms
   grace. This is the principled replacement for the deadline extension.
3. **Cheap hardening, independent of the above**: raise/guard `VMCTX_MM_SLOTS`
   (overflow = silent invention), and chase the live `vmctx_take_lost=2`
   (precheck missing a case). Consider enabling/measuring the KICK notifier
   before any run under memory pressure or on the Intel destination.

## Operational notes carried forward (unchanged from session 26)
- `.229` = AMD source, `biwu@` + `config/id_vmctx`; sudo via
  `SUDO_PASS=biwu SUDO_ASKPASS=$HOME/askpass.sh sudo -A`. The Bash tool resets
  cwd between calls — `cd /home/desktop/lan-boot` in the SAME call before using
  the relative key path, or ssh fails "Identity file not accessible".
- dmesg ring rotates fast (holds ~24 ws1 runs); stream `dmesg --follow` during a
  pool, don't read it after. Pools write to `~/torture.<case>` on /home (not
  /tmp — 7.3 GB tmpfs fills). Kernel #148 build script: `~/build-148.sh`;
  known-good #147 in `~/kernel-known-good/`.
