# HANDOFF — session 31 (2026-08-21): Stage 1's claim-window fix BUILT and REFUTED; the failure is P2, not the storm; go straight to Stage 3

Follows `HANDOFF-session30.md`. Session 30 built Stage 1 of the ownership
redesign (the per-(AS,page) fetch gate + deleting the fault-path tie-break
machinery), measured it regressing hx2 (249 → 216 of 256), and handed off with
the hypothesis that **"a decisive claim-window fix"** — shrink/stop the
`PG_CLAIM` storm without a re-added clock — would recover hx2. This session
BUILT that fix and REFUTED the hypothesis, then found the real cause, and — per
the redesign's own rule (`a stage that regresses a gate is fixed or reverted
before the next`) and the owner's candidate 2 — **reverted the tree and the box
to the session-28 committed tip (the known-good 249/252)**. The definitive
finding below redirects the next session from the claim-window to Stage 3.

## Repo / box state
- Base is committed tip **`41af447`** (sessions 26–28); the Stage-1 gate and both
  session-31 experiments were reverted off it. On top of that clean base sits
  ONE small, verified, UNCOMMITTED fix (the `as_any_present` gate below) in
  `vmctx/user/vmremote.c`. `vmctx/user/vmhome.c` is CLEAN. `tests/torture.sh`
  still carries `KEEP_PASS=1`; untracked keepers: `tests/census.sh`,
  `tests/gate.sh`, `REDESIGN.md`, `HANDOFF-session29/30/31.md`.
- Box `.229` runs kernel **#149** (unchanged) and the **session-28 tip + the
  `as_any_present` fix** (SRCID `26c1cad44cf0`, verify-build all green). It is a
  known-good-or-better base, NOT on the refuted gate/honest-record experiments.
- Nothing committed (not green yet — commit only when the line lands green). The
  two refuted experiments are saved in the session scratchpad only
  (`vmhome.honest-record.c`, `vmremote.destgate.c`); do not resurrect them.

## The one fix that was KEPT — a stale-loan record desync (vmremote.c ~4762)
The serve reads the current page (`page_read_as` → `have=1`) and then USED to
throw it away (`have=0`) whenever the loan record said the asker holds it
("your own copy is current"). That override is right ONLY when the bytes are
object-only (`as_any_present==0`, the FATAL-WHO shape: no context present, the
asker truly holds the current copy). When a guest CONTEXT has the page present,
the guest pulled it back and is the live writer — the loan record is stale — and
the override discarded the guest's current bytes and served the retained copy
(stale by every write since the last capture). **Fix: gate the override on
`!as_any_present(who, rq.addr)`; if a context has the page present, serve its
current bytes** (`n_get_theirs_but_present`). Measured (48×8, KEEP_PASS): **suite ×1 34/0**, hx2
46→**47/48**, pg3 **48/48**, ws1 **48/48**, sig1 **48/48** (the dead-token
retained net is untouched — it runs when NO context is present). The fix fires
rarely (~5 per pool), so it is a correct, safe down-payment on the P2/P4 record
desync, not the hx2 cure.

## The hx2 residual is MULTI-PRODUCER (this is why one patch does not green it)
Two distinct losers seen in 48-run pools, both on `0x7ffff7ff6000`:
- **P2 transit-window stale-retained serve** ("1 other"): the writer reads a
  value tens of thousands of rounds old. `page_read_as` returns 0 (no context
  present, object stale from the last capture), the record says the asker holds
  it, and the retained copy (last transmit = last capture) is served stale. The
  current bytes are only on the asker (a sibling that pulled the page) — the
  P1/gate producer, or a P3 object-current backstop. NOT the `have=0` override
  (that fix fires only when a context IS present).
- **startup invented-zero** ("1 zero", read 0, 0 rounds): a zero installed
  during the initial hand-in thrash (`lent 1 holder 0 installed 1`, retained
  serves 0). hx2.c already documents the startup thrash; this is a race there.

So hx2's ~2–4 % is not one bug. The holistic redesign (P1–P5 at source) is the
real fix; a couple of targeted patches move it by noise. Do NOT chase it with
more point fixes without first deciding the whole-crossing model.

## What was built and measured (both refuted as hx2 fixes)
Both were deployed on #149 via `sync-amd.sh` and measured on small pools (48×8,
KEEP_PASS, graded by guest.out, census over both halves).

### Experiment A — the DESTINATION fetch gate (completing P1 symmetrically)
`pull_overlap` (a vmremote counter: two guest threads of one address space both
pulling one page — "two copies on the wire and two installs") is the tightest
correlate of hx2's storm: control 44 → Stage-1 storm 20445. The source got the
P1 gate in Stage 1; the destination never did. So A made the destination WAIT
for an in-flight sibling pull to land/end (bounded 500 ms slices) instead of
giving up and double-pulling. Result: `pull_overlap` fell ~15000 → ~4300/run,
**but the storm and pass rate barely moved (hx2 43/48; `claim_yields` still
~84 000/run)** and the gate's join fired **0 times** — because under the storm
NOBODY can land the page (the source refuses everyone CLAIMING), so there is no
landing to join. pg3 stayed 48/48. Conclusion: `pull_overlap` is a symptom, not
the driver.

### Experiment B — the honest record (the claim-window fix session 30 asked for)
The storm is 100 % `vmhome.c:3061` — the source's FAULT path marking `PG_CLAIM`
*before* its GET, held across a contended ~4 ms GET, during which every
destination pull of the hot page is refused CLAIMING. B made that record HONEST:
an absence fault means this side holds NOTHING, so it does not record `PG_CLAIM`
(a claim to a page in neither of the two states a claim is about); the `infl[]`
slot still marks the in-flight fetch. Result: **the storm was KILLED** —
`claim_yields` 84 000 → 210/run, `refused_but_here` → ~0, `pull_overlap`
4300 → 138/run; **pg3 stayed 48/48 clean** (storm gone there too). This is the
right claim-window mechanism: no clock, no tie-break, owner-aligned.

**But hx2 did NOT recover — still 43/48.** Killing the storm even made hx2's
*fail mode* slightly worse: it pushed more source GETs into the far side's
INFLIGHT/grace path, so `retained_served` rose to ~5/run *in every run* (session
28 fired it in only ~12 % of runs), and one stale retained serve is the loss.
`get_inflight_expired` also appeared (~9/run) as the crossing moved from
"destination storms CLAIMING" to "source waits INFLIGHT 1.5 s then expires."

## The finding (this is the redirection)
**The storm is waste, not the failure.** hx2's failures are P2: the
DESTINATION serves its RETAINED copy (last transmitted bytes, stamped gen==count
so the generation check passes) to a LIVE holder, and those bytes are stale —
the guest wrote the page thousands of rounds *after* that capture without a new
take. The failing writer read back a value ~97 734 rounds old ("1 other", not
one-behind, not zero). This is a transit-window / record desync: the current
bytes are momentarily on neither machine's readable copy (in flight between a
source read-syscall pull and the guest's writes), the destination's lend record
says "the source holds it," and the retained copy is the destination's only
local answer — so it serves stale.

**P2 cannot be fixed standalone.** Serving the retained copy only for a dead
token and answering a LIVE holder with INFLIGHT (REDESIGN P2) is exactly the
`vmremote.c` arm the comment at the retained branch already refutes: *"sig1's
teardown shape, hung by the no-cap arm."* A live-but-can't-produce holder
(sig1's dying context) then waits forever. P2 needs P3 first: the object-write
backstop so a live AS can ALWAYS produce the page (from the object), whereupon
INFLIGHT resolves instead of hanging.

**The gate is a net negative and was reverted.** Stage 1's gate changed pg3 by
one run (252 → 251) and regressed hx2 by 33 (249 → 216); it serialises the
source's fetches, which is what widens the single held `PG_CLAIM` into the
storm. It also does not prevent the retained-serve loss (that is the
destination's answer to a GET the gate still lets through). Its one real
benefit — a second source context using a sibling's current copy instead of a
stale GET — is the P1 producer, but pg3 already reaches 252 without it via the
landing machinery, and the loss that remains is P2, on the DESTINATION.

## What the next session should do — Stage 3, on the session-28 tip
Skip the claim-window (this session proved it does not move the hx2 pass rate).
Go to **Stage 3 = P3 + P2**, the redesign's actual fix for hx2's loss:
- **P3 (token conservation, the backstop):** every take-install lands the bytes
  somewhere the next fault finds them — a live context of the AS, else the
  address-space OBJECT (`obj_write`), else put back to the source. With the
  object always current during a transit, the destination can answer a live
  holder from the object instead of the stale retained copy.
- **P2 (retained-only-for-dead):** with P3's backstop in place, serve the
  retained copy ONLY for `!task_alive`; a LIVE holder gets the object's current
  bytes (not INFLIGHT-forever, which hangs sig1 — that is why P3 comes first).
- Gate against **hx2, pg3, ws1, sig1** (ws1/sig1 are the dead-token cases the
  retained net is load-bearing for — `retained_served` fired in 31 of 249
  passing session-28 runs; do not weaken the dead path). `tests/gate.sh`.
- Study `get_have_page` / `serve_object_mapped` / `own_object_page` /
  `shared_obj_*` first — P3 lives in that object machinery, and `serve` at
  `~vmremote.c:4900` already serves the object for a DEAD holder
  (`shared_range_has && !task_alive`); P3 extends that to the LIVE transit case
  with the object kept current.

Only after hx2 (and pg3/ws1/sig1) is green should the gate be reconsidered — and
if re-derived, keep experiment B's honest record (no `PG_CLAIM` across a fetch)
so the gate never re-creates the storm.

## Known facts carried in (still true)
- `vmctx_take_lost=2`, `mm_overflow=0`, `takeobj_stale_store=0` (session 28).
- pg3's residual deaths are the §6.1a split-brain (`0x7ffff77f2000` ABSENT-fill
  downstream of a NULL deref), unchanged.
- Operational: pools/suite via `setsid nohup … </dev/null &` over ssh + poll
  (a foreground ssh hangs ~2 min); pools write to `/home/biwu` (never `/tmp`,
  7.3 GB tmpfs); grade hx2 by `guest.out` (`^PASS$`), NOT torture's WATCHWORD;
  `pgrep -x` (not `-f`, which self-matches the ssh line); dmesg needs `sudo -A`;
  the box key is `config/id_vmctx`, sudo pass "biwu" via `~/askpass.sh`.

## Commit convention
ONE squashed commit at the end of the line that lands green, author+committer
`Bi Wu <biwu85@gmail.com>`, keep the `Co-Authored-By` trailer. Nothing is
committed yet.
