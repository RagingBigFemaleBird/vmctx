# HANDOFF — session 33 (2026-08-21): REDESIGN P2 (retained-only-for-dead) BUILT, MEASURED, REFUTED — the live/no-pull-active retained arm is ONE arm that is BOTH hx2's stale-serve AND the escape from a lost-token deadlock; locally indistinguishable; REVERTED

Follows `HANDOFF-session32.md`. The session-32 handoff said: do **P3 then P2**, NOT
the source fetch gate (which regressed hx2 three times). I read the baseline hx2
pool and implemented P2 in isolation first, because the baseline evidence pointed
straight at the P2 producer.

## What the baseline evidence showed (torture.hx2.baseconf, 44/48)
The loss on `0x7ffff7ff6000`, read off `home.log` + the page TRAIL:
- **home.log:** `serving the retained copy (gen 4 ... holder alive, transit never
  landed)` — the **P2 producer**: the destination's 1200ms young-episode CLOCK arm
  serving its RETAINED copy to a LIVE holder (stale by every write since capture).
- **the TRAIL** also showed the deeper producer: a `PULL-BYTES gen=8` carrying
  gen-7's sum — the SOURCE serving a stale copy on a pull, propagating through
  subsequent takes. Multi-producer, exactly as REDESIGN.md says.

## P2 built (vmremote.c pg_serve_conn retained branch, ~line 5000)
Deleted the 1200ms young-episode CLOCK arm. A LIVE holder that cannot produce the
page now ALWAYS gets INFLIGHT (ask again — the landing decides), bounded by the
source's own 1.5s GET loop; the RETAINED copy is served ONLY to a DEAD holder
(`!task_alive`). No source fetch gate. SRCID `3d0299bd0cf6`.

## RESULT (chain32.sh p2, 48x8, graded by guest.out)
| pool | P2 | baseline |
|---|---|---|
| hx2  | **44/48** | 44/48 |
| pg3  | 47/48 | 47/48 |
| ws1  | **48/48** | 48/48 |
| sig1 | **48/48** | 48/48 |

Two findings, one good and one decisive:
1. **The clock arm is NOT load-bearing and bounded-INFLIGHT does NOT hang sig1.**
   ws1 and sig1 stayed 48/48; the old fear ("INFLIGHT for a live holder hangs
   sig1's teardown") was about the UNCAPPED arm, not this bounded one. So the
   retained-for-dead principle is safe for the teardown cases.
2. **hx2 stayed 44/48 but the failure MODE got worse:** 2 of the 4 hx2 fails are
   now HANGS (`hang.10`, `hang.20`), where the baseline's were counted lost-stores.
   The hang is on a page the SOURCE records as **OURS** (it holds it) yet faults on
   and cannot produce — a genuine token loss — while the destination (P2) answers
   INFLIGHT because the holder is alive. Neither side can produce the page:
   deadlock. **The retained copy was the ESCAPE from that deadlock.**

## The decisive finding (4th confirmation of the directory)
The live-holder / **no-pull-active** retained arm is a SINGLE arm that is
simultaneously BOTH bugs:
- serving the retained copy to a writer whose token is **slowly in transit** =
  hx2's stale-serve lost-store;
- withholding it from a holder whose token is **lost** = this session's hang.

They are **locally indistinguishable** — the destination cannot tell "in transit,
landing soon" from "token evaporated, retained is the newest survivor" without the
landing signal. The deleted 1200ms clock was a tie-breaker guessing between them;
re-adding a longer clock is the same forbidden species (REDESIGN.md, sessions
27/28). Only the DIRECTORY (one record that knows the token's SITE) resolves it.
P2 in isolation just relocates the loss (stale-serve -> hang), net flat, so it is
NOT a keeper.

## State handed off (box restored to known-good, nothing committed)
- P2 edit REVERTED. `vmremote.c` = the session-31 baseline (committed tip 41af447
  + ONLY the uncommitted `as_any_present` stale-loan fix, diff 60/8 unchanged).
  `vmhome.c` at tip. `torture.sh` keeps KEEP_PASS.
- Box `.229`: redeployed + verified at the baseline **SRCID `26c1cad44cf0`**
  (kernel #149 unchanged). Not on the regressed/refuted P2 build.
- Docs: REDESIGN.md P2 section carries this RESULT; memory updated
  (session33-p2-retained-for-dead). Pools kept on the box:
  torture.{hx2,pg3,ws1,sig1}.p2 + census.*.p2.txt.

## What the next session should do
The point-fix search is exhausted (sessions 30-33: gate regresses; claim-window
kills the storm but not the loss; D1 gate+serve-wait regresses; P2 relocates loss
to a hang). Every remaining producer is the ONE unresolvable local ambiguity:
"is this live holder's token in transit or lost?" — answerable ONLY by a global
record. **Build the DIRECTORY proper** (REDESIGN.md "The ownership DIRECTORY"):
one entry per (as,page) at the source, one transition in flight, condvar waiters,
the destination keeping only kernel facts. Start from the RECEIVING side (a guest
fault and a sibling's read of one page block on the entry, not poll/guess),
WITHOUT the source fetch gate. This is a large multi-session build; stage-gate it
(suite x3 + the four 48x8 pools) and do not let a stage that adds a worse failure
mode stand, even at a flat pass count (P2's lesson).

## Operational facts carried in (still true)
- Grade hx2/ws1/sig1 by census (`guest.out`, `^PASS$`), NOT chain32/torture's
  dir-name count (it mis-files: P2 dir-names read hx2 1/48, sig1 1/48, ws1 22/49;
  census reads the true 44/48/48/48). Run the chain as ROOT (baseline pools are
  root-owned; a biwu run cannot rm them, mixing builds). `pkill -f vmhome` over
  ssh kills the ssh session — use `sudo -A pkill` in its own call, then reconnect.
  Box key `config/id_vmctx` from the repo ROOT; sudo pass "biwu" via ~/askpass.sh;
  sync-amd.sh is the only deploy path; write pools to /home/biwu (never /tmp).
- Commit convention: ONE squash at the line that lands green, author+committer
  `Bi Wu <biwu85@gmail.com>`, keep Co-Authored-By. Nothing committed (not green).
