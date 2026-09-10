# Session 46 — the full read, then the wakeups: 61 → 16 µs per forwarded call

Directive (verbatim): "Fully read everything and review all code. EVERY
SINGLE LINE. DOUBT EVERYTHING. Get tests all green and continue to expand
viability of the program. Focus on performance optimizations. Give me
summary of performance results." Continues session 45 (HANDOFF-session45.md;
repo = one root commit `5a58904` plus session 45's uncommitted VMX fix,
which this session commits with its own work).

## What the read covered

Every line, again, this time with the question "where does the time go":
both docs sets (RUNNING/PRINCIPLES/ARCHITECTURE/AUDIT/NOTES/REDESIGN/
PITFALLS/HISTORY), the uapi and shared headers, the module (`vmctx/kernel/
vmctx.c`, SVM and VMX backends), both core-kernel trees (`src/linux-7.0.14`
8118 lines, `src/linux-6.18.35` 5612), `vmhome.c` (8089) and `vmremote.c`
(17977) in full, every script under `tests/` and beside them (suite, runany,
runany-x, progs, chain, gate, prun, torture, census, perf, perfab, the ws1
trio, sync/verify/build/xrun/browser/remote/setup/reboot/wedge), the small
tools, and all 48 test C sources. No correctness bug was found in the code
this time; one harness bug (AUDIT §10 #28) and the performance findings below.

## Where the time went (measured before anything was changed)

Baseline `tests/perf.sh s46base` (N=5, kernel #180, build 336659dc789e,
schedutil): forwarded getppid **61 µs/call median, 45–91 spread**; page
fault 37–89 µs/event; walls wc 0.051 s (native 0.008), curl 0.083, sha
0.151, py 0.313, gzip 1.345, links 0.114, vmbench 1.95; compute 235 vs
236.6 Miter/s native (99.3%).

Two instruments named the cost:

1. `perf sched timehist` during a getppid storm (`~/prof.sh`): one call is a
   ring of four tasks — the guest context (asleep in `vmctx_report`),
   vmremote's service thread (`VMCTX_CTL_WAIT`), vmhome's connection thread
   (`read(2)`, then `VMCTX_CTL_SYSCALL`), the service context (parked in
   `vmctx_guest_step`) — and every hop was a sleep and a wake at 3–10 µs of
   wake-to-run each, six hops per call. vmhome's thread also woke TWICE per
   request: vmremote sent the header and the register payload as two
   writes, two TCP segments, and the reader blocked between them.
2. The governor A/B (`~/vab2.sh`, one boot, arms alternating): the SAME
   binaries measured 42–114 µs/call under schedutil and 35–40 ±1 under
   `performance`. The spread that every session called noise was the
   frequency governor reading a sleep-per-hop protocol as an idle box.
   (The verbose per-call log line costs ~4.5 µs of the 35; left alone,
   it is the diagnostic the harness runs on.)

## The three changes, each A/B'd on one boot with arms alternating

| change | where | getppid µs/call (schedutil) | evidence |
|---|---|---|---|
| request header + payload in ONE writev (`io_send2`; PUT too) | vmremote, vmhome `ctx_put` | 40.2 → 36.2 (perf gov, 6/6) | one segment, one wakeup |
| **spin-before-sleep** at the kernel's four waits, `vmctx_spin_us`=100, adaptive per site and context, `need_resched()` bails | source kernel **#181** (`src/linux-7.0.14`, patch regenerated) | **79–113 → 23–31** (3.4x); perf gov 36 → 23 | `vmctx_spin_hits` 160k / misses ~30 per 40k-call run |
| **spin-before-read** on every channel socket, `VMCTX_SOCK_SPIN_US`=50, `recv(MSG_DONTWAIT)` probes, same adaptivity | vmremote `sock_read_all` (io_all + pg_rw), vmhome `read_all` | 25 → **17.6** (6/6) | each half prints hits/misses |

Adaptivity: a wait that ends within the budget arms the next spin, one that
outlives it disarms it — so a service context whose program computes for
seconds, or any wait across the LAN (250 µs+), measures one long wait and
sleeps from then on until a short one re-arms it. The kernel spin bails on
`need_resched()`, so a runnable task never waits behind a spinner; the
counters are core_params. Memory ordering: the four flag stores became
`smp_store_release`, the spin conditions `smp_load_acquire`.

Also measured and NOT adopted: readahead on loopback with the RTT gate
forced to 0 (`VMCTX_RA_MIN_RTT_US`, new tunable, default still 120) — sha and
py equal, gzip within noise, matching session 40's verdict on the old path.
The chunk's per-page cost (take + wire + install ~35 µs) is still about a
round trip.

## Final numbers (perf.sh N=5, kernel #181, build e3f32f053bd4, loopback)

schedutil (the box's default):

| prog | native | fwd wall | faults (µs/ev) | syscalls (µs/call) | before (s45 base) |
|---|---|---|---|---|---|
| wc | 0.007 | **0.041** | 513 (31) | 48 (146) | 0.051 |
| curl | 0.007 | **0.060** | 886 (29) | 118 (76) | 0.083 |
| sha256 libc | 0.010 | **0.135** | 1031 (43) | 107 (439) | 0.151 |
| python3 1M-sum | 0.021 | **0.174** | 2225 (34) | 331 (136) | 0.313 |
| gzip -c wc | 0.433 | **1.222** | 4041 (74) | 370 (932) | 1.345 |
| links2 -dump | 0.017 | **0.084** | 1075 (34) | 110 (145) | 0.114 |
| vmbench | 0.437 | **0.810** | 121 (50) | 20015 (**16**) | 1.95 (61) |

`GOV=performance`: wc 0.039, curl 0.059, sha 0.094, py 0.157, gzip 0.845,
links 0.081, vmbench 0.800 (15 µs/call; faults 33–42 µs). Compute: 227.9
Miter/s guest vs 231.0 native (98.7% in this run; 99.6% in the base).

What is left, in order: the page fault (30–75 µs: two channel hops plus
TAKE/PROTECT + install + records — the same ring, now without its wakeups),
the I/O-buffer bounce (gzip/sha syscalls are 300–900 µs because a 32 KiB
read(2) pulls 8 pages one fault each through the service context), and the
verbose line.

## The bug the gates found, and fixed (AUDIT §10 #29)

The first cross-machine suite on the fast path failed pid1 (1 of 5 rounds):
`child 8403: could not take its monitor over (Invalid argument, task gone)`
with the Intel dmesg naming an unrecoverable #PF at the payload's entry rip
(0x41b1bd, err 0x14). Session 44's spawn handoff RESUMEd the child from the
spawner and DETACHed right after, arguing the entry gate's re-test made the
order safe; that was a race on the guest's wake-up latency, and the faster
paths lost it. Fix in `spawn_forked_context`/`child_thread`: the spawner
sets registers and detaches WITHOUT releasing; the child's own service
thread RESUMEs after its ATTACH succeeded. pid1 cross 6/6 after; cross
suite 40/0.

## Gates (kernel #181, final binaries 37a4a19b9c46 on both boxes)

| gate | result | what failed |
|---|---|---|
| loopback suite ×3 (120) | **119/120**, then 118/120 on the earlier build | fc1 only (the open residual below; 1/3 and 2/3 rounds) |
| cross suite ×1 (40) | **40/0** (38/40 before the #29 fix: pid1, fc1) | — |
| progs (69 graded + 3 known gaps) | 65–67 pass | pipes (exit 98, 3/6 standalone), lddnames-family empty-stream flakes (shufdet's e3b0c442, gitwork's empty out, one sort output mismatch in 25 runs today) |

None of the residual failures is caused by the spin (A/B'd: fc1 2/6 with
the kernel spin vs 1/6 without; pipes 4/6 both arms; sort 12/12 either
userspace pair, `take_lost` +0) — they are the lifecycle/vacate-ordering
families already on the books (AUDIT §10 open residual; REDESIGN "in-flight
state when a counterpart leaves"; HANDOFF-session43 §44b's empty-stream
disguise), now hit more often because everything runs 2–3x faster. What the
session established about them:

- **fc1**: read the trail. The "faults on 0x4e1000 over and over" line is
  NOT the death (0x4e1000 is fc1's `.data` page ping-ponging per round, and
  the per-thread same-address counter counts that legitimate pattern); the
  death is the churn thread's write to a fresh 8 KiB mapping punched by a
  stale vacate that the cons/REPAIR path re-applies three times without it
  sticking (`unrecoverable #PF at 0x7ffff77f2000 err 0x6`). AUDIT §10 has
  the reading. Rate now ~1 in 2 standalone.
- **pipes** (exit 98, FATAL page 0x555555561000 on neither machine, fault
  err 0x7, then "address space ENDED"): the teardown family.
- **`vmctx_take_lost`** rose by exactly 35 in each progs campaign (0 across
  every A/B and the suite): the source's TAKE of an anon page refused after
  the zap with ONE extra reference (`refs=2 want=0(+1) mapcount=0 lru=1
  anon=1`, pages 0x7ffff7fa6000-0x7ffff7fb5000), in a 3-second burst that
  coincides with gpgsym's known claim storm. Not attributed further.

## Deploy state at the end

- .229: kernel **#181** (`~/build-181.sh`, a copy of build-173.sh),
  module + tools rebuilt by `MODULE=1 sync-amd.sh`, srcid 37a4a19b9c46
  (build/ rebuilt by hand after the #29 fix; verify-build amd all-ok).
- .30: `/usr/local/bin/vmremote` + `vmhome` at 37a4a19b9c46 (build.sh +
  `cat | ssh`), kernel 6.18.35 #56 unchanged — the destination kernel did
  NOT get the spin (its two waits are the guest's reply and CTL_WAIT; across
  the LAN the reply wait is 250 µs and would disarm itself; CTL_WAIT would
  save one wake per call ≈ 3% of a cross call; a netboot kernel has no undo).
  Follow-up if the cross number matters: port the same four-site change to
  `src/linux-6.18.35` by hand (never by copying files — README in
  kernel-patches).
- Harness: `perf.sh` native-median bug fixed (#28); prints governor + spin;
  `GOV=performance` option.

## The scripts left on the box (all under /home/biwu)

prof.sh (perf sched + cycles around one vmbench run), vab.sh/vab2.sh
(-v vs quiet, GOV=), vab3.sh (two vmremote builds), spinab.sh (kernel
spin A/B by core_param), vab4.sh (two (vmhome, vmremote) pairs with
per-arm env: ENV_A/ENV_B, PROGARGS), ab5-7.sh (fc1/sort/pipes rate A/Bs),
fc1trail.sh (fc1 with the page trail on 0x4e1000; its failing run is kept
in ~/fc1trail), batch1-4.sh (the measurement and gate chains).
Their outputs: prof.s46.out, vab*.out, spinab.*.out, ra.{sha,gzip,py}.out,
perf.s46{base,spin,spinperf,final,finalperf}.log, suite.s46.log,
progs.s46.log, xsuite.s46.log.

---

# Session 46b — "Fix fc1 and pipes so everything is green"

Directive (verbatim): "Fix fc1 and pipes so everything is green". Three
destination-side bugs, each read from a kept failing run's trail, each fixed
in one build and gated (AUDIT §10 #30–#32; all in `user/vmremote.c`):

1. **fc1 (#30)**: the lookup-then-punch race. `cons_lock` guarded the
   ring's words, not the order of "did a newer construction cover this
   vacate?" against "note my row, then apply my map". `layout_order_lock`
   now spans `range_vacated`'s lookup+punch and `cons_note`'s note; the
   REPAIR livelock guard is keyed on the row's seq (a recycled address
   starts a fresh budget). VACATE-LATE fires 13–37×/run where it fired 0;
   fc1 8/8, 6/6, 6/6 across three builds.
2. **pipes shape A (#31)**, exit 98 / SIGILL in the exec'd tar: a parent's
   write-prep give landed in the copy's object between `exec_wipe`'s punch
   and its lineage mark; the new image executed dash's heap page as text.
   The mark moves to the top of `exec_wipe`; `cow_give_page_mode` re-checks
   it after the object write and undoes a give that crossed the exec.
   pipes 3/6 → 9/10.
3. **pipes shape B (#32)**, the dash subshell's segfault at 0x18 in
   `_int_malloc`: the copy's first ask for an inherited page went through
   the PAGE SERVICE (the source's kernel found it absent in the copy's mm
   and vmhome asked GET), which never walked the COW chain — ABSENT while
   the parent held the page under protection — and the source's strict
   COWBREAK then raced the parent's give and was refused "already given,
   copy's object has it". Zeros for an anon page (the exec'ing children's
   TLS), the pristine file for libc's `.data` (main_arena's bins NULL in
   the file image = the 0x18 corpse). Fixes: the GET path gives from the
   chain (strict) before ABSENT; a COWBREAK whose copy already holds the
   page is re-dispatched as that GET (`goto cow_own_get`).

How the reading went, for the next reader: batch6's kept failure
(`~/b6.pipes.fail.5`) has the corpse; the decisive lines were the
destination's `COWBREAK refused ... already given 1, copy's object has it
1, parent readable 1` (for the TLS page of an earlier child, printed
before the cap) against the page service's `has=0` for the same page a
moment earlier, and `0x7ffff7fa0000 answered ABSENT and filled with an
empty page` on the source. A native `setarch -R dash` maps proved
0x7ffff7fb4000 (the page the earlier failures named) is a HOLE in dash's
own layout — that ask was a downstream symptom of the corrupted child,
not the cause; the cause was the page that was never served.

## Gates (kernel #181 on .229, 6.18.35 #56 on .30; srcid 125c8bc4b2ea on both)

| gate | result |
|---|---|
| pipes ×10 standalone (the shape-B reproducer) | **10/10** (3/6 before #31, 9/10 before #32; "given from its ancestor chain" 3–7×/run) |
| fc1 ×6 standalone | **6/6** (8/8 and 6/6 on the two earlier builds of the fix; 1 in 2 before #30) |
| loopback suite ×3 (120) | **120/0** |
| progs (69 graded + 3 known gaps) | **69/0** (gpgsym, gdbbt, stracec XFAIL as before) |
| cross suite ×1 (40) | **40/0** (xsuite.s46d.log, 4 min) |

`vmctx_take_lost` moved 70→105→140 across the two progs campaigns (the
same +35 per campaign as session 46 measured; gpgsym's claim storm, not
attributed further) and +0 across the suite runs.

## Deploy state at the end

- .229: kernel #181 unchanged; `build/` at 125c8bc4b2ea (batch8.sh's
  hand build, same recipe as batch6.sh).
- .30: `/usr/local/bin/vmremote` + `vmhome` at 125c8bc4b2ea (`VERIFY=0
  ./build.sh` here, `cat | ssh -i config/id_vmctx root@10.0.0.30`;
  `./verify-build.sh intel` all-ok). Kernel 6.18.35 #56 unchanged.
- Repo: ONE commit on top of `aa63c8b` (author and committer Bi Wu).
  No kernel change this round, so kernel-patches/ is untouched.

## Scripts and kept runs added this round (all under /home/biwu)

batch6.sh (rebuild + pipes ×10 + fc1 ×6; failures kept as
b6.pipes.fail.N — `b6.pipes.fail.5` is the #32 corpse), batch7.sh /
batch9.sh (suite ×3 + progs → suite.s46c/s46d.log, progs.s46c/s46d.log),
batch8.sh / batch8p.sh (the final build's pipes ×10, run as root — the
first launch as biwu was void: "must run as root"), batch10.sh (cross →
xsuite.s46d.log), nt.strace (native `setarch -R` strace of the pipes
pipeline: no process maps 0x7ffff7fb4000).
