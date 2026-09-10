# HANDOFF — session 28 (2026-08-21): the claim storm fixed, kernel #149, the residual re-shaped

Follows `HANDOFF-session27.md` (the review). This session BUILT the three
moves that review ranked, measured them on .229, and re-shaped the residual
with what the new instruments said. Box: `.229` on **#149**, userspace from
this tree, `verify-build.sh amd` clean.

## What was built

### 1. The crossing is decided by the landing (userspace, `vmhome.c`)
The "2 s race" was a claim storm, and the storm runs — kept whole for the
first time (`torture.sh` keeps a passing run with yields as `storm.N`) —
named it in the source's own lines:

    [vmhome] CLAIMING 0x7ffff7ff6000 for context 221071 (as=221065,
             state set at vmhome.c:2903, here=1)          (x2475)
    [vmhome] sweep: 0x7ffff7ff6000 is recorded as being claimed by
             vmhome.c:2903 and is present here as well -- both machines
             are holding it

Line 2903 is the fault service's `pg_set(PG_CLAIM)` before its GET. Two
source contexts fault on B's stack page at the pg3 join; the first's pull
lands it; the second's GET reaches a destination whose record correctly says
"the asker holds it" and answers INFLIGHT; the second then sits in
`pg_get_op`'s re-ask loop for the destination's whole 1200 ms grace with the
page marked CLAIM — so every guest pull for a page the source *already
holds* is refused CLAIMING (~2500 yields + 1 give-up per storm). The
existing "a landing beats arriving bytes" rule dropped the answer anyway —
after the storm. Nothing was waiting on work; a record was waiting on a
clock.

Fix: `pg_get_op(..., pid_t landed)` checks `ctx_present()` on every INFLIGHT
answer and returns `PG_GOT_LANDED` the moment a sibling has landed the page;
an absence fault whose page is present before the ask leaves is answered
DONE without asking. Protection faults pass 0. Counters:
`claims ended by a sibling's landing: N before the ask left, N on an
INFLIGHT answer`. No kernel change, no protocol change.

### 2. Kernel #149 (`src/linux-7.0.14` commit `63809af76`, patch 0071)
- **TAKEOBJ re-read detector** (the instrument NOTES §6.1a asked for): a
  kernel copy of the handed-over bytes, re-read after unmap + kick + copy +
  punch while the folio is still ours; a difference is a guest store through
  a translation the kick did not retire. Counted
  (`vmctx_takeobj_stale_store`), named in dmesg (offset, old/new word, cpu,
  pid), and the NEWER bytes are re-copied to the caller
  (`_stale_recopied`; 3 re-reads then `_stale_unresolved`).
- `VMCTX_MM_SLOTS` 64 → 256 (it had overflowed once on #148; an overflowed
  mm has every fault hook fall through).
- Built with `~/build-149.sh` (copies `~/vmctx-s28-vmctx.c` into the box's
  tree), `systemctl stop gdm` + reboot, module rebuilt with
  `MODULE=1 ./sync-amd.sh`. `core_pattern` re-set to `/tmp/core.%e.%p`.

### 3. The residual photographed (userspace, `vmhome.c`)
The ABSENT-fill print ("answered ABSENT and filled with an empty page") now
adds the fault's `err`, mapping `class`, this side's `record`, the
`/proc/<ctx>/maps` line at the address and `/proc/<ctx>/syscall`.

## What was measured

Baseline (#148, this tree before the fixes), pg3 ×256 8-way:
**251 pass / 5 fail (all exit 242)**; 10 full storms (2412–2553 yields, 1
give-up each), 35 runs with a 2-yield blip. `take_lost`, `mm_overflow` did
not move.

On #149 + fixes:
- suite ×3: **102 passed, 0 failed** each round.
- pg3 pool ×256: **256/256** (was 251) — **0 storms** (max 1 yield, in 8 runs;
  0 give-ups; was 10 storms of ~2500 yields + 1 give-up and 35 two-yield
  blips), 7 runs ended a claim on a landing, 0 deadline kills, pool 1086 s
  (was 1250 s). Re-measured on the FINAL form: **252/256, 0 storms** (max 3
  yields, 0 give-ups); the 4 failures all carry the photograph below.
- hx2 pool ×256 with the FIRST form of the fix (claim ends on any INFLIGHT):
  **235/256 (20 FAIL 1, 1 no-output)** — a regression from 250 (session 26).
  The INFLIGHT reply now carries its reason (`VMR_INFLIGHT_PULL` vs clock)
  and the source ends a claim only on the clock kind; arms
  `VMHOME_LANDING=off|grace|all`, `VMHOME_LANDING_EARLY=0|1`. A/B on #149 (graded by guest.out): **off/0 control 247/7/2** (session 26 on #148: 250/6/0 → #149 is not the regressor); all/1 235/20/1; grace/1 243/13/0; grace/0 246/9/1 (control band: the
  pre-ask early check was the cost). Final form = INFLIGHT landing guarded by
  the destination's capture count carried in the INFLIGHT reply (`gen`) with
  the reason in `why`; early check off: **249/5/2** (session 26's band).
- `vmctx_takeobj_stale_store` = **0** over the suite and the pools (160k+
  hand-overs), including runs that died 242.

## What the residual is now
ALL pg3 deaths seen this session (5 baseline, 4 final pool, 1 suite run)
and hx2's 2 no-output runs carry one line, now photographed:

    [vmhome] context <B's service ctx>: 0x7ffff77f2000 answered ABSENT
             and filled with an empty page (rip 0x469cc2, err 0x2,
             class 0x9, record NONE) maps: 7ffff77f2000-7ffff77f6000 rw-p
             | syscall: 13 0xb 0x0 0x7ffff77f2000 0x8 ...

`rt_sigaction(SIGSEGV, NULL, oldact=<page>)` into a 16 KiB `rw-p` mapping:
that is **vmhome's own signal-disposition probe** (`ctx_sys(pid,
SYS_rt_sigaction, …, scratch, 8)`, vmhome.c ~4820, via `ctx_scratch()`),
which runs because a SIGSEGV is being routed — i.e. AFTER the guest has
already faulted at 0x0 (instruction fetch) or 0x8 (write). The line is
downstream of the death; the zero-fill of the monitor's never-touched
scratch page is correct (pre-touch it at creation to silence the noise).
The TAKEOBJ detector's zero over ~40 M hand-overs says the bytes were NOT
changed under a hand-over either. So the upstream defect is still what
§6.1a calls the split brain: a zeroed pointer word read at the pg3 join /
by an hx2 writer, with every crossing counter at 0. The stamp guard keeps
asking through it (`n_claim_landing_behind` ≈ 14 per hx2 pool) rather than
making it worse.

## Next moves
1. Trail the page that holds the zeroed word: arm `VMR_TRAIL_PAGE` /
   `VMHOME_TRAIL_PAGE` on 0x7ffff7ff6000 (B's top stack page, `pd`) for a
   pg3 pool and read the dying runs' trails (the photograph now says the
   fault is at 0x0/0x8 ~25 ms after B's exit-time `MADV_DONTNEED`).
2. Pre-touch vmhome's scratch mapping at creation so the probe's
   zero-fill line stops masquerading as a defect.
3. The KICK notifier (`vmctx_mn_enable=0`) is still off; dormant without
   memory pressure. A/B it in a pool before any run under pressure.
4. `vmctx_take_lost` reached 2 again during an hx2 pool (from 0 after the
   reboot) and its dmesg line had rotated by the time it was read: stream
   `sudo dmesg --follow` to /home during every pool.
5. torture.sh's default WATCHWORD (ws1's 0x4cf700) grades every hx2 run
   "lost": re-grade hx2 pools by `guest.out` (or pass `WATCH=`).

## Operational notes (added this session)
- Launching a setsid'd suite/pool through `sudo -A bash -c "... &"` over
  ssh leaves the ssh hanging ~2 min: run it `run_in_background` and poll.
- `runany exited 3` = empty `guest.out`. A suite's FIRST case landing on
  the 2% residual looks exactly like a broken kernel; run the case by hand
  before drawing that conclusion.
- `dmesg` needs sudo on .229. `kill -TERM -<pgid>` stops a setsid'd suite;
  its `vmremote-local` children survive and are killed by pid.
