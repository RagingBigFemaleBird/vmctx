# HANDOFF — session 32 (2026-08-21): the ownership DIRECTORY named; its first form (Stage D1) BUILT and REFUTED — the gate regresses hx2 a THIRD time; the real first step is P3, not a gate

Follows `HANDOFF-session31.md`. The owner's hint after session 31: *"you need a
better architecture for memory sharing and multi consumer/multi producer …
instances of vmhome and vmremote should share the memory on ownership, so that
you may consult who owns what page globally, at a much better speed and scale."*

## What I did with the hint
Read against the code, the hint names the actual defect class (not any single
P1–P5): **there is no single record that knows who owns a page.** Five records
across two machines each hold a private belief (`pgown`, `infl[]` on the source;
`lent[]`, `installed[]`/`retain`/`pgen`, `pgstate.h` on the destination), and
every disagreement is "decided" by a clock, a stamp, a retained copy or a yield
storm. I wrote the concrete form the hint takes across TWO machines (no shared
segment in production) into `REDESIGN.md` — a **home-based ownership directory**:
ONE directory at the source, one entry per (address space, page), at most one
transition in flight, waiters block on the entry's condvar (never poll / yield /
guess). The full state table and the three operations (home fault, guest pull,
dest GET serve) are in `REDESIGN.md` ("The ownership DIRECTORY (session 32)").

## Stage D1 (built, deployed, MEASURED, REFUTED, REVERTED)
The directory's first code form, on session 30's recovered fetch gate:
1. `infl_enter` — one fetch per (as,page); a second faulter joins the first's
   landing (session 30's code, recovered from its scratchpad; the honest-record
   hunk NOT taken — `PG_CLAIM` still marked).
2. NEW `infl_wait_fetch` (vmhome) — a guest pull for a page this side is
   fetching WAITS for the landing (3 s bound, single-page serves), then is
   served from the landed page by the ordinary take.
3. NEW `pg_claim_for_source` (vmremote) — a source GET preempts this side's own
   pending `PST_REQUESTING` of the page instead of spinning 1 s on it.

**Result on #149 (SRCID 62aaedade587), 48×8 graded by guest.out:**
- **hx2 36/48 — REGRESSED from the 46/48 baseline (−10).**
- pg3 47/48, ws1 48/48, sig1 48/48; suite 33/1 (the one pg3 = §6.1a split-brain).
- The `waited the 3 s bound (must be 0)` counter fired **7–14 per run**
  (60–76 s of serve-side stall per pool): `infl_wait_fetch` waits for THIS
  side's fetch to land, but that fetch is the GET looping on the destination's
  `INFLIGHT_PULL`, so it does not land in 3 s and the serve falls through to
  CLAIMING anyway — a stall added, the circle not broken.
- The losses moved but did not go: now `pull active 1`, a writer reading a value
  ~1.6 M rounds behind (the RECEIVING side installs/reads a stale copy while its
  own pull is in flight), where the baseline's were `read 0` (a context ended,
  zero-filled).

**The finding (a THIRD confirmation): any form built on the source FETCH GATE
regresses hx2.** Session 30's gate: 249→216. Session 31's gate+honest-record:
43/48. Session 32's gate+serve-wait+preempt: 36/48. The gate serialises the
source's fetches, which widens the window the claim storm rides; and the failure
is **P2 on the RECEIVING side**, which no source-side serialisation touches. The
directory's first *code* step must therefore be **P3 (the object backstop), NOT
a gate** — exactly what the session-31 handoff already said. Do P3 first.

## State handed off (box restored to known-good, nothing committed)
- Tree reverted to the session-31 baseline: `vmhome.c` at committed tip
  `41af447`; `vmremote.c` = tip + ONLY the `as_any_present` stale-loan fix
  (uncommitted, as session 31 left it); `torture.sh` keeps `KEEP_PASS`.
  The D1 experiment is saved in this session's scratchpad only
  (`gate+honest.diff`, `gate-only.diff`); do not resurrect it as-is.
- Box `.229`: kernel #149 unchanged; **redeployed and verified at the baseline**
  (SRCID `26c1cad44cf0`). Reconfirmed after the revert: **hx2 44/48** (the
  baseline's 44–47 band). NOT on the regressed D1.
- Docs: `REDESIGN.md` carries the directory design + the Stage-D1 RESULT;
  this handoff; durable memory updated.
- Untracked keepers: `tests/census.sh`, `tests/gate.sh`, `chain32.sh` idea
  (suite×1 + 48×8 hx2/pg3/ws1/sig1 + census — on the box as `/home/biwu/chain32.sh`).

## What the next session should do — P3, then the receiving-side entry
1. **P3 (token conservation / object backstop), on the clean baseline.** Make a
   live AS ALWAYS able to produce a page: every taken-buffer install lands the
   bytes in the address-space OBJECT (`obj_write`) as well as (or before) the
   context, so a sibling that faults during a transit reads the object's current
   bytes, never the stale RETAINED copy. Study `get_have_page` /
   `own_object_page` / `serve_object_mapped` / `shared_obj_*`; `serve` at
   `~vmremote.c:4900` already serves the object for a DEAD holder — extend it to
   the LIVE transit case with the object kept current. Gate hx2/pg3/ws1/sig1
   (ws1/sig1 are the dead-token cases the retained net is load-bearing for —
   do NOT weaken the dead path).
2. **P2 (retained-only-for-dead)** once P3 lands: serve the retained copy only
   for `!task_alive`; a LIVE holder gets the object's current bytes (safe now,
   because P3 guarantees the object HAS them — not INFLIGHT-forever, which hangs
   sig1).
3. Only after hx2 is green: the directory proper (one entry, one transition,
   condvar waiters), replacing the gate/claim/retained machinery. NOT a gate.

## Operational facts carried in (still true)
- Grade hx2/ws1/sig1 by `guest.out` (`^PASS$`), NOT torture's WATCHWORD (it
  mis-files: D1 ws1/sig1 read "32/48"/"1/48" by WATCHWORD but are 48/48 by
  guest.out). pools/suite via `setsid nohup … </dev/null &` over ssh; write to
  `/home/biwu` (never `/tmp`, 7.3 GB tmpfs); box key `config/id_vmctx`; sudo
  pass "biwu" via `~/askpass.sh`; `sync-amd.sh` is the only deploy path.
- Commit convention: ONE squash at the line that lands green, author+committer
  `Bi Wu <biwu85@gmail.com>`, keep `Co-Authored-By`. Nothing committed (not green).
