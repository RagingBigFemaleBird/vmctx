# HANDOFF — where the project stands, and how every session got here

One file, consolidated on 2026-09-04 from the nineteen per-session handoffs
(sessions 27–46b, 2026-08-21 → 2026-09-04). §0 is the state to resume from;
§1 is the session ledger, one entry per session, dense but complete on
what was directed, built, measured, refuted and kept; §2 is the standing
operational rules every session re-paid; §3 the open frontiers. The
originals are verbatim in `doc-archive/handoffs/HANDOFF-sessionNN.md`
(that is where a `§NN` citation elsewhere in the docs or the code resolves).
The other documents: RUNNING.md (operator's guide), ARCHITECTURE.md (the
map), AUDIT.md (every flow, the corner catalog, the bug ledger §10, the
switch reference §9), NOTES.md (the campaign history with the numbers
behind every refuted design), HISTORY.md (the arc), PITFALLS.md (the
traps), REDESIGN.md (the ownership design record).

## 0. START HERE — state as of 2026-09-04 (session 46b)

**Everything is green.** Loopback suite ×3 **120/0**; cross-machine suite
(.229 source → .30 destination) **40/0**; `tests/progs.sh` **69/0** with the
three KNOWN_GAPS (gpgsym, gdbbt, stracec) XFAIL as documented; pipes ×10
10/10 and fc1 ×6 6/6 standalone (the two residuals session 46 left, closed
in 46b). The browser demo (`browser-demo.sh`, links2 -g) is GREEN by
construction.

**Deployed.** Userspace srcid **067691f06a4f** on both boxes (the same code as 125c8bc4b2ea, which passed every gate below; the id moved when this consolidation rewrote comment citations). .229 (AMD,
Ubuntu, source and loopback destination) runs kernel **7.0.14-vmctx #181**
(the session-46 spin-before-sleep kernel; `~/build-181.sh`), module
`~/vmctx-7.0.14.ko`, `build/` rebuilt by hand (`~/batch8.sh`'s recipe =
`batch6.sh`'s). .30 (Intel, netbooted Alpine, destination only) runs
**6.18.35-0-lts #56**, `/usr/local/bin/vmremote` + `vmhome` at
067691f06a4f, `/tmp/vmctx.ko`; `./verify-build.sh intel` all-ok. The
destination kernel did NOT get the spin (its two waits are the guest's
reply and CTL_WAIT; across the LAN the reply wait is 250 µs and would
disarm itself; a netboot kernel has no undo) — port the four-site change
by hand to `src/linux-6.18.35` if the cross number ever matters.

**Repo.** ONE root commit (this squash), author and committer
`Bi Wu <biwu85@gmail.com>`. Kernels: `src/linux-7.0.14` = vanilla
add83c849 + ONE vmctx commit (no remote — `kernel-patches/linux-7.0.14/`
is its only copy outside that directory); `src/linux-6.18.35` = vanilla
ed9484e8f + ONE vmctx commit (pushed to vmctx-kernel.git); both patches
regenerated and identical to the trees.

**Performance (kernel #181, loopback, schedutil; RUNNING.md §5 has the
table).** Forwarded syscall **16 µs** (61 at #180, 66–80 in the #168 era,
482 at #60); ~220 µs cross; page fault 29–74 µs loopback, ~390 cross;
compute 99.6% of native loopback, 98.5% cross. Walls: wc 0.041 s, curl
0.060, sha256 0.135, python 0.174, gzip 1.22, links2 -dump 0.084 (native
0.007–0.43). `GOV=performance` takes ~10% more off. Never quote a loopback
number without its governor (`perf.sh` prints it).

**What to do next, in order** (each with its cheap reproducer):

1. The lifecycle residual families named in REDESIGN ("in-flight state
   when a counterpart leaves"): lddnames' low-rate empty stream under
   load (~1/3 under a full campaign, 0 standalone), nodework's thread-list
   class (session 37; ~1/6; V8 worker churn is the cheapest reproducer),
   the session-37 join residual. REDESIGN step 3 territory, not point
   fixes.
2. gpgsym: a TIME-DOMAIN gap — forwarded cputime clocks
   (CLOCK_THREAD_CPUTIME_ID) report the PARKED source task's ~0
   consumption, npth's ticker exits, the agent never answers. Fix =
   cputime virtualization (account guest execution into the service
   task's clocks, or intercept cputime clockids with guest-side numbers).
   Designed (AUDIT §10.26), not built.
3. ptrace-inside-a-guest (gdbbt, stracec): forwards mechanically, means
   nothing semantically — the traced task on the source never RUNS. An
   owner design decision (AUDIT §11.8).
4. The exec'd-image clock gap on a MISMATCHED destination (cross only): an
   exec'd image gets a fresh vDSO nothing hides; the fix is the exec-stop
   auxv patch applied to guest execs.
5. Performance: the page fault (two channel hops + TAKE/PROTECT + install,
   30–75 µs) and the I/O-buffer bounce (a 32 KiB read(2) pulls 8 pages one
   fault each; gzip/sha syscalls 300–900 µs). Readahead is neutral on
   loopback (measured three times) and pays only across a LAN.
6. `vmctx_take_lost` rises by exactly +35 per progs campaign (0 across
   every suite and A/B): the source's TAKE of an anon page refused after
   the zap with ONE extra reference, pages 0x7ffff7fa6000–0x7ffff7fb5000,
   a 3-second burst coinciding with gpgsym. Not attributed.
7. Parked by the owner (session 42b): the dbus-launch double-fork wall;
   dillo's ld.so death (one photograph on file).

**Scripts on the box (all under /home/biwu, none tracked)** — the ones a
resumed session reaches for: `askpass.sh` (sudo), `build-1NN.sh` (kernel
builds; `build-181.sh` current), `batch6.sh`/`batch8.sh` (rebuild + pipes
×10 + fc1 ×6), `batch7.sh`/`batch9.sh` (suite ×3 + progs), `batch10.sh`
(cross suite), `perf*.sh`/`vab*.sh`/`spinab.sh` (the session-46 A/B
harnesses), `fc1trail.sh`/`pipestrail.sh` (page-trail runs), `chain32.sh`
(suite + the four 48×8 pools). Kept failing runs worth re-reading:
`~/b6.pipes.fail.5` (AUDIT #32's corpse), `~/b4.pipes.fail.2` and
`~/pipestrail` (#31), `~/fc1trail` (#30).

## 1. The session ledger (27 → 46b)

Each entry: the directive, what was built, what the gates said, what was
refuted and must not be retried, and what was left. The AUDIT §10 numbers
are the bug ledger entries.

### Session 27 (2026-08-21) — the fault-path review; nothing built
A review of the core kernel, the SVM module fault path and both halves
against the live #148 counters. Findings: (1) the "2 s race" is a CLAIM
VOLLEY — ~2500 CLAIMING yields per storm at a pthread-join, resolved not
by anyone winning but by the destination's 1200 ms INFLIGHT grace clock;
extending the fault deadline had treated the symptom. (2) The residual
242 deaths are invented zeros (a thread-stack page's content evaporates
in a sub-µs crossing race beneath every instrument; every crossing counter
0). (3) The KICK mmu-notifier (`vmctx_mn_enable`) was OFF — a latent hole
under memory pressure, dormant on the box (no reclaim/migration/THP
activity). (4) `vmctx_take_lost=2` and `vmctx_mm_overflow=1` on the live
kernel. Named next: the kernel TAKEOBJ re-read detector; make the crossing
decisive; raise `VMCTX_MM_SLOTS`.

### Session 28 — the claim storm fixed (kernel #149)
Built the review's three moves. **The crossing is decided by the landing**:
`pg_get_op` checks `ctx_present()` on every INFLIGHT answer and ends the
claim the moment a sibling has landed the page (`PG_GOT_LANDED`); the
INFLIGHT reply carries WHY (`VMR_INFLIGHT_PULL` vs the clock) and the
source ends a claim only on the clock kind, guarded by the destination's
capture count — the first form (end on any INFLIGHT) regressed hx2 250→235
and was corrected by A/B (final 249/256, session 26's band). Kernel #149:
the TAKEOBJ re-read detector (`vmctx_takeobj_stale_store`, read 0 over
~40 M hand-overs — the residual is NOT a stale-translation store) and
`VMCTX_MM_SLOTS` 64→256. pg3 pool 251→256/256 with 0 storms. The residual
photographed: the "0x7ffff77f2000 answered ABSENT" line is vmhome's OWN
rt_sigaction disposition probe through the scratch page, DOWNSTREAM of the
death; upstream is a zeroed pointer at the join (§6.1a split brain).

### Session 29 — the ownership redesign, Stage 1 in flight
The owner's ruling: **the generation stamp must never be a tie-breaker;
every place that picks between two copies of one page masks a state the
one-writable-copy invariant forbids; fix each producer at its source.**
REDESIGN.md written (producers P1–P5, four gated stages). Stage 1 (P1 =
the per-(AS,page) fetch gate `infl_enter`) coded in vmhome, not deployed.
Stopped at the owner's request.

### Session 30 — Stage 1 built, gated, and it REGRESSES hx2
P1 gate finished (fault path and `ctx_pull_page`), the fault-path tie-break
machinery deleted. Gate (new keepers `tests/gate.sh`, `tests/census.sh`,
`torture.sh KEEP_PASS`): suite 102/0, pg3 251/256 in band, **hx2 249→216**.
The census named it: `claim_yields` ~313→~94,000 per run — the gate
serialises the source's fetches and the deleted `PG_GOT_LANDED` early-out
had been doubling as the claim-window shortener. Two producers, the gate
addresses only pg3's. Not shippable; reverted.

### Session 31 — the claim-window fix built and REFUTED; the failure is P2
Experiment A (a DESTINATION fetch gate): `pull_overlap` 15,000→4,300 but
hx2 43/48, the join fired 0 times — a symptom, not the driver. Experiment
B (the HONEST RECORD: an absence fault records no `PG_CLAIM`): **the storm
died** (yields 84,000→210/run, pg3 48/48 clean) **and hx2 stayed 43/48** —
the storm is WASTE, not the failure. hx2's loss is **P2**: the destination
serving its RETAINED copy (stamped gen==count) to a LIVE holder, ~97,000
rounds stale. One fix KEPT: the stale-loan override gated on
`!as_any_present` (a guest context with the page present is the live
writer; serve its bytes) — hx2 47/48, pg3/ws1/sig1 48/48. Both experiments
reverted; the box back on the session-28 tip.

### Session 32 — the DIRECTORY named; its first form (D1) refuted
The owner's hint ("instances should share memory on ownership, consult who
owns what page globally") read against the code names the defect class:
FIVE records across two machines each hold a private belief, and every
disagreement is decided by a clock, a stamp, a retained copy or a yield
storm. REDESIGN.md's home-based ownership directory written. Stage D1
(gate + `infl_wait_fetch` serve-wait + `pg_claim_for_source` preempt):
**hx2 36/48 — the THIRD regression from a source fetch gate** (30: 216/256;
31: 43/48; 32: 36/48). Rule: never a source-side fetch gate; the first code
step is P3 (the object backstop).

### Session 33 — P2 (retained-only-for-dead) built, measured, REFUTED
The 1200 ms clock arm deleted; a live holder always gets bounded INFLIGHT.
ws1/sig1 48/48 (the old "INFLIGHT hangs sig1" fear was about the UNCAPPED
arm), hx2 flat 44/48 — but 2 of 4 fails became HANGS: the source records
OURS yet cannot produce, the destination answers INFLIGHT because the holder
is alive; neither side can produce the page. **The live/no-pull-active
retained arm is ONE arm that is BOTH hx2's stale-serve AND the escape from
a lost-token deadlock; locally indistinguishable.** Only a global record
resolves it. Reverted (a worse failure mode at a flat count is not a
keeper).

### Session 34 — the owner REDIRECTED to actual shared-memory channels; `own.h`
"You need to build shared memory channels among vmhome instances, and
among vmremote instances… if you have tried that, you probably built it
wrong… get the architecture correct first." The process model verified
(vmhome ONE process with threads; vmremote ONE main process, each guest
context a clone()d process serviced by a `child_thread` in main — state
already thread-shared per machine; the gaps are cross-machine
reconciliation and multi-instance scale). `user/own.h` written: a per-
machine memfd segment, a process-shared futex mutex, one transition per
(as,page), `own_begin/land/abort/wait` (a "positive gen means busy" bug
caught by a two-thread test). Step 2a: wired as a receiving-side OBSERVER —
`begin == land` exactly (26,407/26,407 over the hx2 pool); **hx2's
receiving-side contention measured for the first time: 48% of fetches
found a sibling already fetching the same page; pg3/ws1/sig1 0**.

### Session 35 — REDESIGN step 2b: the BUSY faulter blocks on the landing
The claim moved to `fault_from_home_mode` (the whole fault service is one
transition; wrapper level avoids self-collision). `pull_overlap` 6447→0
per pool, `claim_yields` 499→34/run, `wait_timeout` 0. hx2 flat
(183/192 over four pools vs 46/48 baseline) with ONE failure mode left: a
writer's store read back as ZERO. Established by counters (kernel #149):
MAPOBJ-on-a-hole refuted (`vmctx_mapobj_holes` 0 over 96.8 M), a foreign
read faulting a hole in "refuted" by `ffile_invent` (RETRACTED in 36 — the
counter looked before the punch), declines refuted. Real defect fixed:
`peek_at` counted a SHORT PEEK as a full page. Decisive next: count every
shmem allocation on a live object BY PATH, in the kernel.

### Session 36 — the zero folio's door NAMED and CLOSED (kernels #150–#153)
`vmctx_shmem_alloced()` at `shmem_get_folio_gfp`'s `alloced:` label, keyed
(object,page), stamps every folio's maker; TAKEOBJ names the maker of every
all-zero capture (266/266 named). Verdict: **`foreign_read` 5 — every one
the hx2 page, every one vmremote's `VMCTX_CTL_PEEK` 0–1 ms before the
take**: `peek_at` checks the pagemap, a TAKEOBJ punches the hole, the
PEEK's GUP allocates a zero folio the object then "holds". Fix
`vmctx_shmem_may_alloc()`: a FOREIGN READ fault on a hole of a context's
object answers -EFAULT (short read) — #151 on all shmem broke rd1 (a named
tmpfs file's hole is the file's zeros), #152 on the kernel-internal shm
mount only, #153 deletes the switch. **Zero zero-read failures over 192 hx2
runs**; hx2/pg3/ws1/sig1 48/48. Lesson: a counter that looks up state
BEFORE the operation is not a counter of the operation.

### Session 37 — hx2's ~0.7% residual named as the deep-teardown thread-list class; links2 RENDERS
`take_lost` is BENIGN (a sibling faulted the folio in during the
precheck→zap window; the bytes survive). The residual (1 in ~150 chain
runs, 0 in 1024 hx2-only runs) is a thread descriptor/stack page GENUINELY
GONE from both machines at pthread_join (LOST-PAGE trail dump added,
kept): `LOST: … lent to shadow N, its channel is gone`. A point fix
(retained copy on PULL_GONE) fired 0 times and was reverted. Browsers:
**links2 -g renders end to end**; netsurf's wall diagnosed as glycin's
bubblewrap image-decode sandbox hanging after pivot_root+seccomp
(namespaces run in the SOURCE's real namespace), proven by controls.

### Session 38 — the performance campaign (harness: `tests/perf.sh`, `perfab.sh`)
Baseline: every real program demand-paging-bound, 20–90× native, per-fault
cost scaling with mapping count (links2 1563 µs). Traced bottlenecks and
fixes: the source re-parsed `/proc/<pid>/maps` per page (173k syscalls per
links2 run) → a per-change snapshot; the INSTALLED ack's reply waited for →
owed; a 100 ms startup sleep → a condvar; `clock_gettime` was a SYSCALL
because the box's clocksource was hpet (a false TSC-unstable verdict at
boot) → `tsc=reliable`; futex wakes with no waiter → own.h three-state
mutex; PM QoS 20 µs. Result 1.5–4.3× end to end, per-fault 2–10× (links2
1498→155 µs). A source-side readahead was built and REVERTED (moves round
trips, does not remove them). Lesson: this box's idle states move a run
±50%; only an ALTERNATED A/B says whether a change helped.

### Session 39 — the page record moved INTO THE KERNEL (kernels #154/#155)
Owner: "reading /proc/<pid>/maps is not efficient… move the page tracking
into the kernel… don't be afraid to make big changes." One record per
(mm,page), an xarray beside the page tables (`state|sum|gen|stamp`;
NONE/HOME/REMOTE/CLAIM/TRANSIT), five ctls (SERVE, LAND, PGACK, PGSCAN,
PGSTATE/PGSET); the fault hook CLAIMS before the monitor is asked and a
sibling's fault waits in-kernel on the entry's waitqueue. vmhome 9.7k→6.8k
lines (pgown_tab, the sweeper, strail, infl[], the maps snapshot, the
serve-path retry ladder all deleted). A serve is ONE syscall where it was
five; per-fault cost 2–3× below session 38's. #155: a fault WAITS on a
TRANSIT page instead of stealing it. The ack-skip trap: skipping the
INSTALLED ack for "copied" pages left chunk-served TAKEN pages stuck
TRANSIT → every syscall looked 4 ms; final form acks exactly the taken
pages (a per-page bitmask in the reply). hx2 ~40/48 in the chain (the
session-37 class, now nameable), pg3/sig1 48/48.

### Session 40 — the hx2 pool death was a MISJUDGED EXCEPTION; the harness clock; both readaheads; the Intel port
`signal_generate` tracing found vmhome sending SIGSEGV 0.3–8 s in, delivered
12 s later: a writer's ordinary store on a present page (a stale
write-protection left by a sibling's pull) was handed to the owner, whose
`ctx_tryfault` answered "the kernel would handle it" — mapped to ACCERR and
the process killed; the destination declined and zero-filled. Fix
`VMR_EXC_LEGAL` for `(err & 0x13) == 0x3` only (a first form covering any
0 broke pf2/pf4: TRYFAULT does not model NX). Chain s40h: suite 34/0, hx2
**48/48**, pg3/ws1/sig1 48/48; #156 drops mmap_lock on the TRIED pass (a
three-way cycle; first chain with deadline hits 0); #157 `mmu_notifier_put`
(no SRCU wait per context exit). **The measurement was wrong**: every wall
before this carried ~100–200 ms of runner (uutils `timeout` polls at 100
ms; `ps`/`pgrep` walks) — `local-here.sh` clocks vmremote alone. Both
readaheads built (destination chunk + `SET_POPULATE` #158/#159, source
window inside assisted calls) and gated by the measured RTT (≥120 µs): a
wash on loopback, −7..−13% wall cross. A first form landing AFTER the
RESUME lost a store 1 gzip run in 7 (never land into a RUNNING context).
.30 carried to #55/#56; cross suite 34/34 then 68/68.

### Session 41 (a–g) — corner-flake tests, the sharing census, the namespace axis
`tests/io1` (forwarded-I/O integrity oracle, self-test forges the session-40
loss) and `tests/pf5` (75 fatal children). pf5 found: every fatal fork
child cost 4 s (the COWBREAK refusal sent no page body — the one GET-shaped
answer that did not) and the context table filling at 64 (`MAX_CTX`→256;
recycling rows was tried and REVERTED — dead rows are how the dead stay
answerable). #160: a dirty page-cache folio of a freshly linked binary
refuses to split until the flusher visits → `filemap_flush` kick (the
"first run after a relink stalls 8 s" mystery). Harness trap: a suite run
as the wrong user graded 105 STALE passes (runany refuses unwritable OUT
now). 41b: the in-band clear-tid clear suppressed (#161); the SHARING
CENSUS (#162/#163) splits faults on a context's mm by who made them. 41c/d:
foreign reads of missing pages were already refused; attributed foreign
WRITES (ptrace, /proc/pid/mem) now refused too (#164–#166); the 0082 read
refusal was dead code and removed. 41e: the exit-134 abort was a page that
landed between the take loop and the fatal → INFLIGHT. 41f: **fork copies
read pristine FILE pages** (bwrap ran `setuid(-1)`, the unparsed default)
→ the private-file branch asks COWBREAK first; `tests/fx1`. 41g: the
assisted clone inside a pid namespace returns the NS-RELATIVE pid and the
monitor addresses host pids — mapped, not coded.

### Session 42 (a–d) — netsurf's three walls; the breadth harness; the vDSO plan; a panic
Wall 1 (#167): `VMCTX_CTL_LASTCHILD` records the child's init-ns pid; the
reply names the child twice (`host_pid`, VMR5); `tests/pid1`. Wall 2: a
fork of a fork inherits through the CHAIN (`cow_give_from_chain`, given
pages re-marked OWED). Wall 3 (#168): vacate sequence numbers were assigned
at DRAIN time — the kernel now stamps events AT THE EVENT under the log's
lock and replies carry the counter; `tests/fc1`. Wall 3b (four more from
fc1): the cons ring evicted live rows (same-range rows replace), the tie
read backwards (a tie is the construction's), the REPAIR path (a decline
inside a live row replays its map SET; the module's second-chance report),
a skipped vacate must not keep the old tenant's bytes (obj_forget at SET).
**netsurf PAINTS and FETCHES**; chain s42a green everywhere; cross 39/0.
42b: OWNER'S RULING — the dbus-launch wall is PARKED; `browser-demo.sh`
(links2, self-grading, +253 colours) GREEN; recorded unpursued: a page
stuck in CLAIM/TRANSIT parks netsurf 2/3 (562k faults for one page);
dillo dies in ld.so. Perf on #168: cross syscall 482→259 µs. 42c:
`tests/progs.sh` (30 shapes, 25 green); the shared-port contract fixed
(ports below the ephemeral range). The fork-child page WIPE built and
reverted twice (#169 everything: every child died on pristine relocated
pages; #170 anon-only: spawn-heavy shapes wedged). 42d: the kernel-side
no-vDSO skip destabilised spawns and PANICKED the box; recovery left the
box DOWN at "unable to mount root" (a vmlinuz-only restore over a foreign
initrd). Plan written: vDSO emulation on vmremote.

### Session 43 (a–f) — recovery, the vDSO fix both arms, breadth doubled, the fork-wipe measured to its end
Recovery: the stock kernel from the console; a stray `/boot/vmlinuz-*-keep`
hijacking GRUB moved aside; a matching vmlinuz+initrd pair rebuilt; a real
recovery image kept. **vDSO**: same release → the destination serves its
OWN live vvar AND vdso text (the guest's rdtsc reads the destination's TSC;
the source cannot serve either page — a static guest's libc had been
rejecting a zeroed vDSO by accident) plus a 3×/s refresher because a
snapshot's coarse clocks FREEZE (fc1 3/3 NO-OUTPUT the day the vdso became
real); differing releases → vmhome hides the vDSO from the first image AT
THE EXEC STOP (ld.so caches AT_SYSINFO_EHDR as its first act); `tests/vd1`.
curltls PASS. The exit-status bug: a forked PROCESS's non-zero exit is
program behaviour, only a context that JOINED an address space is a thread
death. progs.sh 30→65 cases with KNOWN_GAPS. 43b: **the fork child's TLS
served as ZEROS** — the parent's page read THROUGH THE PARENT'S TASK
(`ctx_page_inner` on the parent pid); "let the kernel's COW serve it" was
built first and refuted (a page the other machine held at clone time is a
HOLE here; the COW of a hole is the zero page). 242-family taxonomy: exit
127 = the argv-stale exec hole; exit 251 = the orphan's service dies with
its parent's monitor. 43c: JIT runtimes (V8, YJIT, lua) PASS — runtime-
generated code works. 43d–f: the wipe rebuilt three more times (#173, #175,
#177): the serve is RECURSIVE under it → the source-side chain walks (two,
KEPT); an intermediate ancestor that never held the page → the ESCALATED
break (rq.len==1, relaxed give; KEPT); the toolchain then GREEN under the
wipe, but multi-child pipelines wedge in TEARDOWN ORDER — reverted a fifth
time (#178 = proven code). **Convergence: every remaining strict issue is
ONE lifecycle hole — nothing defines in-flight state when a counterpart
leaves.** Also kept: pg_connect fail-fast, the ACT_KILL escape.

### Session 44 (a–c) — THE LIFECYCLE DESIGN (kernel #179), the ghost claim (#180), re-measure
Design authority granted. (1) The tree is the service lifetime (main waits
for every forked service to drain, bounded by LIVENESS, audit-detected
leaks abandon). (2) Monitor succession: a DEAD monitor is the same as no
monitor yet — the event waits for a successor under the fault deadline;
each forked context's monitor is HANDED from the spawning thread to the
child's own service thread, so a guest ENTERS exactly when its watcher is
in place. (3) Claims and transits die with their wire (per-connection
transit tracking; settled THEIRS at close). **nc1 9/9 (first passes ever),
sort 9/9 (was 2/12), lddnames 6/6 — "argv-stale" WAS the clone-window
race; four kernel wipe builds had chased an ordering bug.** 44b: the GHOST
CLAIM — the serve table's HOME-and-absent CLAIMING was the last eternal
answer; refusals are ask-counted in the entry's sum field, 255 consecutive
→ REMOTE/ABSENT (`vmctx_pgrec_home_healed`); gpg's storm 267k→4.6k faults,
DEAD. gpgsym's remaining shape: npth's ticker exits (cputime clocks read
the parked task). 44c: perf on #180 (cross syscall 220 µs); the loan-vacate
fix corrected by its own A/B — **content dies soft; relationships die
hard** (a DONTNEED kills content, not loans; fx1 refuted the first cut).
progs 66/0 — the first zero-fail campaign. The GREAT SQUASH: repo = one
root commit, kernels base+1, docs redone (RUNNING new, AUDIT rewritten,
ARCHITECTURE §10–12, NOTES §0).

### Session 45 (2026-09-03) — the full audit; the VMX base-read bug (#27)
Everything read. ONE real bug: `vmx_run_usercode_once()` read `GUEST_FS_BASE`
/`GUEST_GS_BASE` (and the EPT GPA) AFTER `preempt_enable` — a preemption
migrates to a CPU whose VMCS is another context's, and the wrong base is
fed to the guest: the VMX twin of SVM bug #3. Reads moved inside
`vmx_enter_once` under `preempt_disable`. Both boxes redeployed; cross
40/0 with fs1 PASS validates it; loopback 119/120 (fc1); progs 69/0 (first
clean breadth). The AMD box came back by wake-on-LAN.

### Session 46 (2026-09-04) — kill the wakeups: 61 → 16 µs per forwarded call (kernel #181)
"Fully read everything… focus on performance." `perf sched timehist`: one
call was a ring of four tasks and SIX sleep/wake hops at 3–10 µs each;
vmhome's reader woke TWICE per request (header and payload as two
segments). The governor A/B: the SAME binaries 42–114 µs/call under
schedutil, 35–40 ±1 under `performance` — the spread every session called
noise. Three changes, each A/B'd arms alternating on one boot: one writev
per request (40→36); `vmctx_spin_us` adaptive spin-before-sleep at the
kernel's four vmctx waits (79–113→23–31); `VMCTX_SOCK_SPIN_US`
recv(MSG_DONTWAIT) spin-before-read on every channel socket (25→17.6).
Readahead on loopback measured neutral again (`VMCTX_RA_MIN_RTT_US`
tunable, default 120). Bugs: #28 perf.sh's native median glued five walls;
**#29 a spawned context could ENTER before its own monitor was attached**
(the spawner RESUMEd then DETACHed; the faster paths lost the race; pid1
cross) — the child's service thread RESUMEs only after its ATTACH. Gates:
loopback 119/120 (fc1), cross 40/0, progs 65–67 (pipes, empty-stream
flakes) — the residual families hit 2–3× more often at the new speed.

### Session 46b (2026-09-04) — "Fix fc1 and pipes so everything is green": three destination ordering bugs (#30–#32)
Each read from a kept failing run's trail, each in `user/vmremote.c`.
**#30 fc1**: `cons_lock` guarded the construction ring's words, not the
ORDER of a vacate's covering-row lookup against another thread's
note-then-map; a stale vacate punched the churn thread's live 8 KiB mapping,
and the REPAIR's livelock guard, keyed on an ADDRESS fc1 recycles every
round, refused the fourth repair (-14). `layout_order_lock` spans
`range_vacated`'s lookup+punch and `cons_note`; the guard is keyed on the
row's seq. VACATE-LATE 0→13–37/run; fc1 8/8, 6/6, 6/6. **#31 pipes exit
98 / SIGILL in tar**: a write-prep give landed in the copy's object between
`exec_wipe`'s punch and its lineage mark; the new image executed dash's
heap page as text. Mark first; a give that crossed the exec is undone.
3/6→9/10. **#32 pipes' dash-subshell segfault at 0x18 in `_int_malloc`**: a
fork copy's first ask through the PAGE SERVICE (source-side fault → GET)
never walked the COW chain → ABSENT; the source's strict COWBREAK then
raced the parent's give and was refused "already given, copy's object has
it"; zeros for an anon page (TLS), the pristine FILE for libc's `.data`
(main_arena's bins are NULL in the file image — the corpse). The GET path
gives from the chain (strict) before ABSENT; a COWBREAK whose copy already
holds the page is re-dispatched as that GET. 10/10. A native `setarch -R`
maps read settled that 0x7ffff7fb4000 (the page earlier failures named) is
a HOLE in dash's own layout — a downstream symptom. Gates: §0. Then this
consolidation and the squash.

## 2. The operational rules (each paid for; RUNNING.md and PITFALLS.md carry the full set)

- **Boxes.** .229 = `biwu@` + `/home/desktop/lan-boot/config/id_vmctx`
  (ABSOLUTE path — the tool's cwd drifts), sudo via `SUDO_PASS=biwu
  SUDO_ASKPASS=$HOME/askpass.sh sudo -A`. .30 = `root@`, the same key,
  `-o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null`, no sftp
  (deploy with `cat | ssh`). Home is WSL2: build and orchestration only.
- **Runs.** The suite and every run must run as ROOT (local-here.sh refuses
  otherwise — and a wrong-user suite GRADES STALE OUTPUTS). Launch over ssh
  with `setsid nohup … </dev/null &` and poll a done-marker; a foreground
  ssh dies with the session. One machine operation at a time. Pool output
  under /home/biwu, never /tmp (a 7.3 GB tmpfs). Grade by `guest.out`
  (`^PASS$`), never by a dir-name count. `VMCTX_SHARED=1` skips the rebuild
  AND the leftover sweep; a port per run, below the ephemeral range.
- **Killing.** By pid, or `pkill -x`; `pkill -f` matches the ssh command
  line and kills the session. Check for D-state leftovers after a pool.
- **Measuring.** Alternate arms on one boot; never quote a loopback number
  without its governor; per-op costs from vmremote's own accounting, never
  the harness wall; an instrument must be proved able to fire before its
  zero is believed; armed instruments widen windows (never quote an armed
  run); a counter that looks up state before the operation is not a
  counter of the operation.
- **Kernels.** .229's kernel is rebuilt freely (`build-1NN.sh`, stop gdm
  before rebooting); a stray vmlinuz in /boot hijacks GRUB; a recovery
  image is vmlinuz + initrd + modules or it is not one. .30 is netbooted
  (`tools/build-kernel.sh`, never a bare make; keep `.known-good` aside;
  kernel and modules ship together). Any `src/` change: regenerate
  `kernel-patches/` and amend the single kernel commit.
- **Commits.** ONE squashed commit after origin/master, author AND
  committer `Bi Wu <biwu85@gmail.com>`, the Co-Authored-By trailer kept.
- **Doubt.** Exceptions route to the owner raw. No address-specific code
  (validity = `VMCTX_CTL_TRYFAULT`). No optional correctness (delete the
  A/B arm once proved). Memory problems get memory-level fixes, never
  syscall-specific ones. A fixed test port fakes failures.

## 3. The open frontiers (the list §0 orders; the mechanisms)

| frontier | mechanism | reproducer |
|---|---|---|
| in-flight state when a counterpart leaves (REDESIGN) | a page lent to a context whose channel dies mid-transition; the session-37 thread-list class | nodework ~1/6; lddnames' empty stream under campaign load |
| gpgsym | cputime clocks report the parked source task | `progs.sh gpgsym`, 90 s |
| ptrace in a guest | the traced task never runs on the source | gdbbt, stracec |
| exec'd-image clocks on a mismatched destination | a fresh vDSO nothing hides | cross-only, any exec'd image's coarse clock |
| the page fault's cost | two hops + TAKE/PROTECT + install | `perf.sh` faults column |
| the I/O-buffer bounce | one fault per buffer page inside a forwarded read | sha/gzip syscall µs |
| `take_lost` +35 per campaign | an anon TAKE refused after the zap with one extra ref | any progs campaign, dmesg |
| dbus-launch (PARKED), dillo | double-fork + exec'd daemon threads; a loader-stage NULL | `local-here.sh dbus-launch --sh-syntax`; `PROG=/usr/bin/dillo browser-demo.sh` |
