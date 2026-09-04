# HANDOFF — session 29 (2026-08-21): the ownership redesign, Stage 1 in flight

Stopped mid-Stage-1 at the owner's request. This session started the structural
fix the owner ruled for: **the generation stamp must never be a tie-breaker;
every place that picks between two copies of one page is masking a state the
one-writable-copy invariant forbids, and each producer of that state must be
removed at its source.** The full design is in `vmctx/REDESIGN.md` — read it
first; it names the five producers (P1–P5) and the fix for each, and the four
gated stages.

## Repo / box state
- Committed tip **`41af447`** (sessions 26–28 squash) — NOT pushed. Box `.229`
  runs kernel **#149** with the session-28 userspace. `verify-build.sh amd`
  was clean at the start of this session.
- **Uncommitted working tree** (this session, Stage 1, NOT deployed, NOT
  measured):
  - `vmctx/REDESIGN.md` — new, the whole design.
  - `vmctx/user/vmhome.c` — modified: **P1 fetch gate installed and the fault
    path routed through it** (compiles clean, `gcc -fsyntax-only` silent).
    Nothing else touched yet.
- No kernel change this session. No commit. The box still runs the committed
  session-28 binaries, so a plain gate test needs a `sync-amd.sh` first.

## What Stage 1 has done in vmhome.c (done, compiles)
- `infl[]` promoted from a leaf marker to a **per-(as,page) fetch gate** with a
  `pthread_cond_t infl_cv`. New: `infl_enter(as, base, pid)` returns 1 if this
  thread must fetch (claimed the slot), 0 if a sibling's fetch landed the page
  while we waited (the fault is over). `infl_begin` now takes `as`; `infl_end`
  clears only this tid's slot and broadcasts. Counters `n_fetch_gated`,
  `n_fetch_joined`, `n_fetch_gate_full`.
- The fault handler: an **absence fault** (`!(ev.fault_err & 1)`) now calls
  `infl_enter`; on a join it answers `VMCTX_ACT_DONE` and `goto answered`
  (no fetch, no install, no second copy). A protection fault keeps the plain
  `infl_begin(as, base)` marker.

## What Stage 1 has NOT yet done (resume here, in order)
1. **Delete the fault-path tie-break machinery the gate makes dead** — this
   was the very next edit when stopped. Grep in vmhome.c for: `PG_GOT_LANDED`,
   `n_claim_landed_inflight`, `n_claim_landing_behind`, `n_pull_landed_raced`,
   `n_landing_overridden`, `install_arriving`, and `pg_get_op(..., landed)`'s
   `landed` parameter. With one fetch per (as,page) there is no concurrent
   fault fetch whose bytes race a sibling's landing, so the "landing beats
   arriving bytes" drop AND the stamp override are dead — remove them and
   revert `pg_get_op` to a plain bounded INFLIGHT wait (drop the `landed`
   arg and the `PG_GOT_LANDED` return). Keep the INFLIGHT *wait* (the genuine
   in-transit case still exists).
2. **Gate `ctx_pull_page`** (the monitor's own write-pull, ~vmhome.c:3990) on
   the same (as,page) via `infl_enter`, so a fault fetch and a teardown pull of
   one page cannot both fetch. Without this, deleting the drop in step 1 is
   unsafe (present==1 after a gated fetch could still come from a concurrent
   pull).
3. **Deploy + gate Stage 1**: `./vmctx/sync-amd.sh`, then suite ×3, pg3 pool
   ×256, hx2 pool ×256 (re-grade hx2 by guest.out — torture.sh's default
   WATCHWORD mis-grades every hx2 run "lost"). Expected: pg3 storms gone AND
   the override counters at 0; hx2 back in the 249–250 band or better. If hx2
   regresses, the gate is wrong at its source — do not re-add a clock.

## Stages 2–4 (not started; REDESIGN.md has the detail)
- **Stage 2**: P4b `FOLL_NOFAULT` on the kernel read PEEK
  (`src/linux-7.0.14/kernel/vmctx.c:5563`, `access_process_vm(target, m.addr,
  kbuf, len, 0)` → add `FOLL_NOFAULT`) so the monitor's own read never mints a
  zero page in a context's mm; **kernel rebuild + reboot** (`~/build-149.sh`
  pattern → make a build-150). P4a: delete the RESTORE resurrection
  (`n_take_restored`, vmhome.c ~7682) and route hand-overs through TAKEOBJ; the
  per-context TAKE stays only for anon/private where a refusal leaves the page
  intact. Gate `vmctx_take_lost` stays 0.
- **Stage 3**: P3 token conservation — a taken buffer is never dropped; install
  order live-ctx → the AS **object** (`obj_write`) → put back. Then P2: serve
  the retained copy ONLY for a dead token (`!task_alive`) and **delete the
  1200 ms grace clock** (vmremote `pg_serve_conn`, the `nowu - first_us <
  1200000` arm). The retained table stays as the dead-token net only.
- **Stage 4**: the stamp becomes a **debug assertion** — force `gen_repair()`
  off and delete its two repair arms (`n_gen_repaired`, `_obj`); delete the
  source-side override already removed in Stage 1; delete the 100 ms INTRANSIT
  heal (vmhome `n_intransit_healed`, ~7295) — an unacked INTRANSIT past a bound
  is a named failure, not a clock-driven flip. `pull_gen_check` keeps only its
  STALE-SERVE log.

## Known facts carried in (from session 28, still true)
- The residual pg3/hx2 242 is NOT a stale-translation store (kernel TAKEOBJ
  re-read detector `vmctx_takeobj_stale_store` = 0 over ~40 M hand-overs). The
  "0x7ffff77f2000 answered ABSENT and filled" line is vmhome's OWN
  rt_sigaction disposition probe through the context scratch page —
  **downstream** of the death. The upstream defect is a zeroed pointer at the
  join, all crossing counters 0: the §6.1a split brain, which is exactly what
  P1–P4 target.
- `vmctx_take_lost` reached 2 again during an hx2 pool; its dmesg line rotated
  before it was read. Stream `sudo dmesg --follow` to /home during every pool.
- Operational: launch pools/suite with `run_in_background` + poll (a setsid'd
  `sudo -A bash -c "... &"` over ssh hangs the ssh ~2 min); `dmesg` needs sudo;
  re-grade hx2 pools by `guest.out`; kill leftover `vmremote-local` by pid.

## To resume
`git status` shows only `vmhome.c` modified + `REDESIGN.md` untracked. Read
`REDESIGN.md`, then do steps 1–3 above, gate, and only then start Stage 2.
Commit convention: ONE squashed commit at the end (author+committer
`Bi Wu <biwu85@gmail.com>`, keep the Co-Authored-By trailer).
