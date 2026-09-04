# HANDOFF — session 34 (2026-08-21): the owner REDIRECTED the redesign to ACTUAL shared-memory ownership channels; the message-directory of sessions 32-33 was "built wrong"; substrate `own.h` written, compiles into both halves, deployed, baseline neutral

Follows `HANDOFF-session33.md`. The owner corrected the direction:

> *"You need to build shared memory channels among vmhome instances, and among
> vmremote instances so that pages are consulted correctly globally. If you
> have tried that, you probably built it wrong. Focus on the coherence story
> and do not take 'no improvement' as a rejection signal, as this big redesign
> has a lot of outstanding work to do. Please get the architecture correct
> first before rushing to judgements."*

## What the correction means (and what was built wrong)
Sessions 30-33 built the "directory" as a MESSAGE protocol — a source fetch
gate, a 3 s serve-wait, retained-for-dead — each graded on the 1:1 hx2 pool and
each REFUTED because it relocated the loss (stale-serve -> hang) rather than
removing it. The owner's redirect: the ownership record must be **actual shared
memory** — a segment every local participant maps and consults at a memory
read — and a transition must **block on a futex** in that segment, never poll /
yield / guess. And it must be built for **many** vmhome / vmremote instances
(scale), not the 1:1 message reconciliation the rig runs. Judge the substrate
by the coherence story, **not** by whether the 1:1 hx2 pool moves.

## The process model, verified this session (grounds the whole design)
- **Source `vmhome`: ONE process.** conn_thread / ctx_monitor / sweeper are
  THREADS sharing `pgown_tab`+`infl[]` behind a mutex. Service contexts are the
  adopted program tasks, parked in the kernel, running no coherence code.
- **Destination `vmremote`: ONE main process.** Each guest context is a
  `clone()`d SEPARATE process (no CLONE_VM). Each is serviced by a `child_thread`
  IN THE MAIN process (the `VMCTX_CTL_WAIT` loop -> `fault_from_home`, around
  vmremote.c:14825/15104). `lent`/`installed`/`retain`/`pgstate`/`pull_tab`/
  `infl_ans` all live in the main process, shared by the child threads. The
  context processes touch no coherence table.
So within each machine the state is already thread-shared; the two real gaps are
**(1)** the two machines' records reconciled per-page over the wire with a
tie-break (the loss family) and **(2)** nothing built for multiple instances
(scale). A shared-memory ownership segment closes both.

## What I built
1. **REDESIGN.md** — new top section "The shared-memory ownership channel
   (session 34)": the corrected architecture, the verified process model, the
   entry (`site∈{NONE,HOME,REMOTE}`, `transit∈{NONE,TO_HOME,TO_REMOTE}`, `gen`
   futex word), the three operations (home fault / guest pull / dest GET serve),
   what it deletes, and the staged build order. The DIRECTORY + P1-P5 sections
   below it stand as the record of what led here.
2. **`user/own.h`** — the substrate, compiled into BOTH halves exactly as
   pgstate.h is. A per-machine shared segment (memfd, MAP_SHARED, created before
   any clone so children inherit it), a process-shared futex mutex over slot
   alloc, and the single-transition API:
   - `own_init(make_fd)` create/attach; `own_mark_shared()` once cross-process.
   - `own_begin(as,page,to,&wait_gen)` -> CLAIMED | BUSY(+wait_gen) | FULL.
   - `own_land(as,page,new_site)` / `own_abort` / `own_forget` — bump gen +
     FUTEX_WAKE all.
   - `own_wait(as,page,gen,max_us)` — FUTEX_WAIT on the entry's gen; the bound
     is a diagnosis, never a decider.
   - shared counters in the header (n_begin/n_begin_busy/n_land/n_wait/...).
   **A real bug was found and fixed while validating it:** the first form
   returned "positive gen" to mean busy, which cannot express "busy, wait on
   gen 0" (before the first landing) and risks a lost wakeup; corrected to a
   status + out-param read UNDER the lock. Verified with a two-thread test: a
   waiter genuinely BLOCKS (begin_busy=1, wait=1) and the landing WAKES it
   (woken=1, elapsed = the mover's slow step), observing the correct site.

## State (baseline intact; substrate additive, NOT load-bearing)
- Source change is ONE line per half (the `#include "own.h"`). Nothing calls
  `own_init` yet — behaviour is identical to the baseline. The rest of the
  vmremote.c diff (60/8) is the pre-existing session-31 `as_any_present` fix.
- Both halves build clean locally AND on the box. Deployed via sync-amd.sh:
  **new SRCID `317c989eecf1`** (= baseline `26c1cad44cf0` + the two includes +
  own.h; behaviourally identical). Kernel #149 unchanged, module vermagic ok.
- Suite x1 neutrality check: see /tmp/suite.s34.log on the box (run to confirm
  the additive include does not perturb the baseline; NOT a pool grading).
- Nothing committed (correctly — not a graded green change).

## What the next session does (REDESIGN.md build order, step 2)
Wire the RECEIVING side onto the entry, WITHOUT the source fetch gate (the part
that regressed hx2 three times): a guest fault (`fault_from_home_inner`) and a
sibling's read of one page must not both act on it independently — the second
`own_begin`s, gets BUSY, and `own_wait`s on the entry's gen for the first's
landing, then re-decides. This is P3's object backstop expressed as a
transition, so the wait never falls back to a stale copy. Call `own_init(-1)`
in vmremote main before the first context is cloned, `own_mark_shared()` after
the first clone. Judge it by the coherence story (does a live holder ever get a
copy picked by a clock?), not the 1:1 pass count. Then step 3: the source's
`pgown`/`infl[]` onto the same segment; step 4: delete the tie-break machinery
the entry has made unreachable.

## Step 2a DONE: the entry wired in as a receiving-side OBSERVER, measured on the pools
The entry now drives its (as,page) transit lifecycle in lockstep with every
destination fetch (`ownf_begin`/`ownf_end` at the two `pull_begin` sites in
`fault_from_home_inner`), deciding nothing yet. Deployed SRCID `ea4e089842e4`,
ran the full 4-pool chain (`chain32.sh s34obs`). Two results:

**1. The substrate is provably correct at scale (the "architecture correct
first" validation).** Across the hx2 pool (48 runs): **claimed=26407,
landed=26407 — begin==land exactly**, full=0, no stuck transit. Same clean
lifecycle in every pool (pg3 79781/79781, ws1 46/45 — the one gap is the
`died.36` run killed mid-fetch, sig1 76/76). The channel mirrors the real
multi-producer fetch load completely, at memory speed, one transition per page.

**2. The receiving-side multi-producer contention is REAL and hx2-specific —
measured directly for the first time.** In the hx2 pool, **busy=12646 (~48% of
fetches) found a sibling ALREADY fetching the SAME page**, in 44 of 48 runs —
the P1 producer that `pull_tab` only counts (`n_pull_overlap`) and lets RACE
(two copies on the wire, two installs = the lost store). **pg3=0, ws1=0,
sig1=0** — their producers are not the receiving race (consistent with pg3/ws1/
sig1 being other families). Plus **desync=5504**: BUSY where `pull_active` was
false — the entry serialises the INSTALL phase after `pull_end` that `pull_tab`
releases too early, so the entry is a strictly better serialiser than pull_tab.

**Behaviour-neutral (observer only), by census guest.out:** hx2 46/48, pg3
46/48, ws1 47/48, sig1 48/48 — all within baseline noise; suite x1 34/0. The
box is at `ea4e089842e4` (baseline behaviour + the dormant observer).

## Step 2b NEXT (now justified by data): make the BUSY faulter WAIT on the landing
The 12646 hx2 BUSY cases are exactly what a landing-wait serialises. The design
was settled (by deadlock analysis, session 34 second half) but NOT yet coded:

**Placement: the claim moves UP to `fault_from_home_mode` (the wrapper), not
the two pull sites.** Two reasons, both discovered by analysis:
1. **The 2a observer lands too early.** It lands right after `ctx_page_pull`
   returns — but the installs (serve_object_mapped / poke site3/5w/5r/5/6/7 /
   obj_write) happen AFTER that point, so a waiter woken there refaults into
   the claimer's install window and races it anyway. The landing must be the
   INSTALL, i.e. the whole fault service = ONE transition.
2. **Reentrancy.** Site 1's pull can return ABSENT and fall through
   (`after_recall`) into the no-region/chunk logic containing site 2 — a
   site-level claim held across that would find BUSY-by-self at site 2 and
   3s-timeout every time. Wrapper-level claim avoids self-collision entirely.

**The wrapper shape** (replaces the current `fault_from_home_mode` body for
`vec==14 && !supply_only` — the only callers are child_thread's fault loop;
prefills from pg_serve_conn pass supply_only=1 and stay UNGATED, exactly as the
pgstate gate already excludes them):
```
ak=as_id(pid); pk=addr&~0xfff;
owf = ownf_begin(ak, pk, &g);           /* own_begin(TO_REMOTE) */
if (owf == BUSY) {
    woke = own_wait(ak, pk, g, 3s);     /* coh depth is 0 here: safe to block */
    if (woke) return 1;                  /* landed; the refault finds the page */
    n_ownf_wait_timeout++ (must be ~0); owf = OFF;  /* liveness: run UNGATED,
                                         the pre-2b racing path, counted loudly */
}
prev = pg_claim_prio(REQUESTING);        /* existing pgstate dance UNCHANGED */
if (prev < 0) { yield; if (owf==CLAIMED) ownf_release(ak,pk,0); return 1; }
r = fault_from_home_inner(...);
present = as_any_present(pid, pk);       /* reuse for the settle AND release */
settle (existing);
if (owf==CLAIMED) ownf_release(ak, pk, present);  /* land(OWN_REMOTE) if
                                         present else own_abort; EVERY exit */
return r;
```
Remove the four per-site `owf`/`ownf_begin`/`ownf_end` hooks from the two pull
sites (superseded), rework the helpers (OWNF_OFF/CLAIMED/BUSY + release), and
update the summary print (claimed/waited/woken/wait_timeout/full/released).

**Deadlock-freedom argument (verified against the call graph, keep it true):**
the waiter blocks at wrapper level holding NO locks (coh depth 0 — the only
vec14/!supply_only caller is child_thread's loop). The claimer never waits on
any entry (one thread services one fault; prefills are ungated). The page
server (`pg_serve_conn`) NEVER touches the entry — deliberately — so home's
GETs cannot queue behind a guest fault's claim: no cross-machine cycle. The
GET-serve-vs-guest-fault race stays with pgstate/pull_tab for now; converting
the GET serve onto the entry (with the source-wins preempt rule) is a LATER
stage. Timeout 3s < the 6s kernel fault deadline, and its expiry falls through
ungated (liveness over serialisation, counted). NO source-side gate anywhere —
that regressed hx2 three times (sessions 30/31/32).

Gate on suite x3 + the four 48x8 pools by census; judge by the coherence story
(n_ownf_waited should absorb the 12646 hx2 races; wait_timeout ~0; does a live
holder ever get a clock-picked copy?), and accept a flat 1:1 pass count (the
owner's instruction — do not revert on "no improvement"; only on a WORSE
failure mode, per P2's lesson).

## Operational facts (unchanged)
Box key `config/id_vmctx` from repo root; sudo pass "biwu" via ~/askpass.sh;
sync-amd.sh is the only deploy path; run pools/suite as ROOT, detached
(`setsid nohup ... </dev/null`), poll the log — a foreground ssh run dies with
the session. `sudo -A pkill -f vmhome` over ssh kills the ssh session; kill by
pid. Grade hx2/ws1/sig1 by census (`guest.out`, `^PASS$`), not the dir-name
count. Chain: `/home/biwu/chain32.sh <tag>` (suite x1 + 48x8 hx2/pg3/ws1/sig1).
Commit convention: ONE squash at the line that lands green, author+committer
`Bi Wu <biwu85@gmail.com>`, keep Co-Authored-By.
