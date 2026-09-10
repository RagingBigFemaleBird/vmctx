# HANDOFF — session 30 (2026-08-21): Stage 1 of the ownership redesign, BUILT, GATED, and it REGRESSES hx2

Follows `HANDOFF-session29.md`. This session finished Stage 1 of the owner's
ownership redesign (`REDESIGN.md`: the generation stamp must never be a
tie-breaker; every place that picks between two copies of one page masks a
state the one-writable-copy invariant forbids, and each producer is fixed at
its source). Stage 1 = **P1, the per-(AS,page) fetch gate, plus the deletion of
the fault-path tie-break machinery the gate is meant to make dead.** It was
deployed on `.229` (#149) and gated. **The suite stayed green and pg3 held, but
hx2 regressed 249 → 216 of 256.** The measurement is clean and the cause is
named below; the decision it forces belongs to the next session, so per the
owner's steer (`stop and handoff after stage 1`) Stages 2–4 were NOT started.

## Repo / box state
- Committed tip **`41af447`** (sessions 26–28) — unchanged, NOT pushed. Box
  `.229` runs kernel **#149** (session-28 kernel, unchanged this session).
- **Working tree, UNCOMMITTED** (do not commit — Stage 1 regresses a gate):
  - `vmctx/user/vmhome.c` — Stage 1: the P1 fetch gate finished and the
    tie-break machinery deleted (diff below). Compiles clean; **deployed on
    the box** via `sync-amd.sh` (SRCID `1b2d42dd4d60`, verify-build all green).
    So the box currently runs the Stage 1 binaries, NOT the committed tip.
  - `vmctx/tests/torture.sh` — added `KEEP_PASS=1` (keep passing runs whole,
    as `pass.N`, so a census has the control arm).
  - `vmctx/tests/census.sh` — NEW (keeper). Reads both halves' exit summaries
    for every kept run, grades by `guest.out` (`^PASS$`), and tabulates which
    coherence mechanisms fired in the passing vs the failing runs.
  - `vmctx/tests/gate.sh` — NEW (keeper). One stage's whole gate: suite ×3,
    then hx2 and pg3 pools (256×8, every run kept, dmesg streamed, kernel
    counters before/after), then the census. `./tests/gate.sh <tag>`.
  - `vmctx/REDESIGN.md`, `vmctx/HANDOFF-session29.md` — still untracked (the
    design and last session's handoff).

## What Stage 1 changed in vmhome.c (done, deployed)
- **P1 fetch gate finished.** `infl[]` is a real per-(AS,page) gate:
  `infl_enter(as,base,pid)` returns 1 to the ONE thread that claims the slot
  and fetches, 0 to a second faulter of the same page once the first's fetch
  LANDS (present) — it never issues its own GET. Hardened with a bounded,
  self-naming wait (`n_fetch_gate_slow`, said out loud past 6 s). Both fault
  paths now enter it: `ctx_monitor()` (every fault — all faults this side sees
  are absence faults; a protection fault is counted as unexpected,
  `n_fault_prot_seen`) and **`ctx_pull_page()` (the write pull), the handoff's
  step 2** — split into `ctx_pull_page`/`ctx_pull_install` so the gate is
  released on every exit.
- **Deleted the tie-break machinery** (handoff step 1): `PG_GOT_LANDED` and the
  `landed` argument to `pg_get_op`; the "landing beats arriving bytes" drop
  (`n_pull_landed_raced`) and its stamp override (`n_landing_overridden`,
  `install_arriving`, trail kinds `ST_RECV_DROPPED`/`ST_RECV_OVERRODE`); the
  source-side claim-end-on-INFLIGHT check (`n_claim_landed_inflight`,
  `n_claim_landing_behind`). `pg_get_op`'s INFLIGHT arm is now a plain bounded
  re-ask (1.5 s, `n_get_inflight_expired`). The arriving GET bytes are simply
  installed, with a counted assertion (`n_fetch_landed_unexpected`) if the page
  is somehow present when the gated fetch lands.
- The generation stamp is now log-only on these paths (echoed for the
  destination's STALE-SERVE log; decides nothing here).

## The gate result (kernel #149 + Stage 1 userspace)
`./tests/gate.sh s29a` — full output in `/home/biwu/gate.s29a.out`; pools kept
whole in `/home/biwu/torture.{hx2,pg3}.s29a/` with `census.txt`.

- **suite ×3: 102 / 0** each round. Green, unchanged.
- **pg3 pool 256×8: 251 / 256** (2 died-242, 3 other). IN BAND with session 28
  (252/256 final form). The **gate is clean and cheap here**: census over the
  251 passing runs — `gate_fetched` ~80 k/run, `gate_joined` 29 TOTAL,
  `claim_yields` ~0, `refused_but_here` 0. The 2 deaths are the unchanged
  §6.1a residual (`0x7ffff77f2000 answered ABSENT`, the rt_sigaction scratch
  print downstream of a NULL deref — session 28's finding, not new).
- **hx2 pool 256×8: 216 / 256 — REGRESSED from 249** (session 26/28 band
  247–250). 40 fail: 3 died, 37 printed hx2's own `FAIL 1` (a detected lost
  store). 7 of the 40 hit the invented-page FATAL / retained rescue
  (`fatal_retained_lines`, `decl_known`).

## Why hx2 regressed — named by the census (this is the finding)
The census is decisive. Per PASSING hx2 run, Stage 1 vs session 28:

| counter | session 28 | Stage 1 | ×    |
|---|---|---|---|
| `claim_yields` (dest yields to source CLAIMING) | ~313 | **~94 000** | 300× |
| `refused_but_here` (source records THEIRS but still holds the page) | ~23 | **~94 000** | 4000× |

The two track each other exactly. **The claim storm re-exploded.** The gate
serializes the *two-source-threads-fetch-one-page* race (which is pg3's, and
pg3 proves the gate kills it — `gate_joined` and `claim_yields` ~0). But hx2's
producer is a DIFFERENT one: the *source-claim-window vs destination-write*
storm. The source marks `PG_CLAIM` before its GET and holds it across the whole
fetch; while `PG_CLAIM` is set, every destination `VMR_OP_CTXPAGE` for that
red-hot page is refused CLAIMING → the destination yields (`claim_yield`,
usleep 200) → re-faults → refused again → tens of thousands of times a run, and
every one of those is a both-holder window (`n_gone_but_here`) that can drop a
store.

The machinery Stage 1 DELETED — the `landed` early-out in `pg_get_op`
(`PG_GOT_LANDED`) and the pre-ask "already present → DONE" — was **doubling as
the claim-window shortener**: on hx2 it ended the source's `PG_CLAIM` fast when
a sibling had already landed the page, keeping the storm small (250/256).
Removing it without a replacement reintroduced the storm. The redesign's
premise — "the gate makes the tie-breaks unnecessary" — holds for pg3's
producer and is FALSE for hx2's: they are two different producers and the gate
addresses only the first.

**Conclusion: Stage 1 as specified is not shippable (it regresses a gate). The
gate (P1) is right and cheap — keep it — but the claim-window cost needs its
own source-level fix, and it must not be a re-added clock or a re-added
tie-break between two copies (the owner's rule).**

## What the next session should decide (do NOT just re-add the deleted code)
The direction session 27 already named: **make the crossing decisive** instead
of storming CLAIMING. Concretely, candidates to weigh (build against hx2 + pg3
+ suite, census both pools, arms alternating in one pool):
1. **Shrink the `PG_CLAIM` window at the source.** The source claims a page
   only to hand it right back to the destination; a destination write-CTXPAGE
   that arrives during that window need not storm. The gate already guarantees
   one fetch per page, so the source could hold `PG_CLAIM` for a far shorter
   span, or the serve could recognise "I am claiming this only to fetch it for
   the guest, and the guest is the one asking" and answer decisively rather
   than CLAIMING→yield→refault. This is the P1-consistent fix: the storm is a
   *producer* (a second copy is momentarily contended), not a tie-break to
   restore.
2. **If (1) does not hold hx2**, the honest reading is that the fetch gate
   alone is insufficient for the hot-single-page workload and the redesign's
   Stage 1 scope must grow to include the claim-window producer before its
   deletions are safe — or Stage 1's deletions are reverted (`git checkout
   vmctx/user/vmhome.c` returns to the session-28 tip, NO gate; the gate would
   then be re-derived on top).

Only after hx2 is back in band (≥ ~248/256) should Stages 2–4 begin. They are
UNTOUCHED this session:
- **Stage 2**: P4b `FOLL_NOFAULT` on the kernel read PEEK
  (`src/linux-7.0.14/kernel/vmctx.c:5563`, `access_process_vm(target,…,0)` →
  `…, FOLL_NOFAULT`) — kernel rebuild + reboot (`~/build-149.sh` pattern →
  build-150; note both userspace `ctx_peek` and vmremote `peek_at` already
  clamp to present pages, so P4b may be latent-only — measure `vmctx_*` foreign
  counters, do not assume it is the producer). P4a: delete the RESTORE
  resurrection (`n_take_restored`, vmhome ~7780) and route hand-overs through
  TAKEOBJ; `vmctx_take_lost` stays at its current 2 (did not move this
  session).
- **Stage 3**: P3 token conservation (object-write backstop) + P2 retained-only
  -for-a-dead-token + delete the 1200 ms grace clock (vmremote `pg_serve_conn`,
  the `nowu - first_us < 1200000` arm, ~4962). CAUTION: the session-28 census
  shows `retained_served` fired in 31 of 249 PASSING runs — the retained net is
  load-bearing today; do not delete it until P3 provably replaces it.
- **Stage 4**: stamp → assertion (`gen_repair()` off + delete its repair arms;
  the fault-path override is already deleted; delete the 100 ms INTRANSIT heal,
  `n_intransit_healed`). `pull_gen_check` keeps only its STALE-SERVE log.

## How to reproduce / continue
- The box runs the Stage 1 binaries now. To re-measure Stage 1:
  `setsid ./tests/gate.sh s29b </dev/null >/home/biwu/gate.s29b.out 2>&1 &`
  (root, on `.229`, in `~/lan-boot/vmctx`). Read `torture.*.s29b/census.txt`.
- To revert the box to the committed tip: `git checkout vmctx/user/vmhome.c`
  then `./sync-amd.sh`.
- Operational (unchanged, all re-paid this session): launch pools/suite with
  a `setsid nohup … </dev/null &` over ssh and POLL (a foreground ssh hangs
  ~2 min); pools write to `/home/biwu` (never `/tmp`, a 7.3 GB tmpfs); grade
  hx2 by `guest.out` not torture's WATCHWORD; `pgrep -f` self-matches the ssh
  command line, use `pgrep -x`; dmesg needs `sudo -A`.

## Known facts carried in (still true)
- `vmctx_take_lost = 2`, `mm_overflow = 0`, `takeobj_stale_store = 0` across
  the whole gate (the residual is NOT a stale-translation store, session 28).
- pg3's 2 residual deaths are the §6.1a split brain unchanged: `0x7ffff77f2000`
  ABSENT-filled, all crossing counters 0, the print downstream of the death.

## Commit convention (for whoever lands this line)
ONE squashed commit at the end, author+committer `Bi Wu <biwu85@gmail.com>`,
keep the `Co-Authored-By` trailer. Nothing is committed yet this session.
