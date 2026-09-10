# REDESIGN — one writable copy, taken not inferred (session 29)

The owner's ruling: a page is writable at exactly one site; every write
negotiates an exclusive owner first; the generation stamp is a **debugging
aid and must never be a tie-breaker**. Everything that today picks between two
copies of one page — a clock, a stamp comparison, a retained-copy net, a
yield storm — is masking a moment when two copies existed, which the
invariant forbids. This document names every producer of that moment and the
fix that removes the producer, so the deciding machinery can be deleted rather
than tuned.

## Session 36 — the hx2 residual was a KERNEL door, closed; step 3 is next

The zero-read the channel (2b) could not remove was not an ownership race
at all: a monitor PEEK of a page being taken GUP-faulted the punched hole
and shmem allocated a zero folio in the object (kernel #150 census named
it, #152/#153 refuse it at the allocation — HANDOFF.md §36). With it
closed the chain is green: hx2 48/48, pg3 48/48, ws1 48/48, sig1 48/48,
suite 34/0. Step 3 (the source's `pgown`/`infl[]` onto the own.h segment)
and step 4 (delete the tie-breaks) stand as specified below; the remaining
declines (`self_kern lsvc`) and the source-side short PEEK are their cases.

## The shared-memory ownership channel (session 34) — the owner's correction

After sessions 30–33 built and refuted every *message* form of the directory
(a source fetch gate, a serve-wait, retained-for-dead — each relocated the loss
rather than removing it), the owner corrected the direction:

> *"You need to build shared memory channels among vmhome instances, and among
> vmremote instances so that pages are consulted correctly globally. If you
> have tried that, you probably built it wrong. Focus on the coherence story
> and do not take 'no improvement' as a rejection signal, as this big redesign
> has a lot of outstanding work to do. Please get the architecture correct
> first before rushing to judgements."*

The correction is precise: the directory of sessions 32–33 was **a protocol of
messages** — the source serialised fetches and answered pulls with a clock, and
"who owns this page" was still a question asked over TCP, per page, and decided
by a tie-break. What the owner asks for is **actual shared memory**: ownership
is a segment every local participant maps, consulted at a memory read, and a
transition BLOCKS ON A FUTEX in that segment rather than polling, yielding, or
guessing. And it must be built for **many** producers and consumers — many
vmhome and many vmremote — not for the 1:1 message reconciliation the rig
happens to run. So the substrate's correctness is judged by the coherence
story, **not** by whether the 1:1 hx2 pool moves (the owner said so outright).

### The process model this rests on (verified, not assumed)
Every earlier design floundered partly on a wrong picture of who shares what.
The code says, read this session:

- **Source `vmhome` is ONE process.** A `conn_thread` per connection, a
  `ctx_monitor` per service context, and the sweeper are all THREADS sharing
  `pgown_tab` + `infl[]` behind a mutex. The service contexts are the adopted
  program's own tasks — forked, parked in the kernel, running no coherence
  code.
- **Destination `vmremote` is ONE main process.** Each guest context is a
  `clone()`d SEPARATE process (no `CLONE_VM`: `spawned_child` / `net_child`).
  But each context is serviced by a dedicated `child_thread` **inside the main
  process** — the `VMCTX_CTL_WAIT` loop that calls `fault_from_home` — and
  `lent[]`/`installed[]`/`retain_tab`/`pgstate_tab`/`pull_tab`/`infl_ans` all
  live in that one main process, shared by the child threads. The context
  processes run guest code and touch no coherence table.

So *within* each machine the coherence state is already shared (by threads of
one process). The two gaps the owner names are therefore real and separate:
**(1)** the two machines' records are reconciled per page over the wire with a
tie-break — the whole loss family; **(2)** nothing is built for *more than one*
vmhome or vmremote — the scale the hint keeps naming. A shared-memory ownership
segment closes both: it is the same structure whether one process or several
map it, and it is the substrate on which a futex-blocking transition (the fix
for gap 1) is even expressible.

### The structure
One **ownership segment per machine** (a `memfd`, `MAP_SHARED`, created before
any context is cloned so every child inherits the mapping; threads share it for
free). Every local participant maps it; consulting ownership is a read of it.
The **source's segment is the authority** — the source is the home of every
page (it owns the program and wins every conflict by role, PRINCIPLES §6). The
**destination's segment is a coherent cache** of the home record plus the
destination's own kernel facts (its page tables, its object). Neither machine
literally maps the other's segment — in production they are two kernels, and a
segment shared only in loopback would be the forbidden loopback-only arm
(PRINCIPLES: no optional correctness). The wire remains, but it carries **only
token transitions**, never a per-page ownership query.

One entry per (address space, page):

```
own_ent(as, page):
    site    ∈ {NONE, HOME, REMOTE}          -- where the token is (HOME=source)
    transit ∈ {NONE, TO_HOME, TO_REMOTE}    -- at most ONE move in flight
    gen     : uint32                        -- bumped on every landing; the futex
                                               word a waiter sleeps on
    transit_owner, transit_us               -- diagnosis only, never a decider
```

A short per-table futex mutex guards slot allocation and the field reads/writes
(each microseconds). The long wait — a fault that finds a transition already in
flight — does NOT hold that mutex: it sleeps on the entry's `gen` word with
`FUTEX_WAIT`, and the party that lands the token bumps `gen` and `FUTEX_WAKE`s
it. This is the one thing the message directory could not do and the reason it
kept needing a clock: with a real futex there is a landing to wait *for*, so
"is the holder in transit or lost?" — the locally-indistinguishable question
that defeated sessions 32–33 — is answered by the entry, decisively, for every
waiter at once.

### The operations (all through the entry; none guess)
- **Home fault** (source absence fault). Read the entry. `transit≠NONE` → WAIT
  on `gen`, then re-decide. `REMOTE` → set `transit=TO_HOME`, GET from the
  destination, install, set `site=HOME transit=NONE`, bump+wake. `HOME` → serve
  from the source mm (a sibling landed it). `NONE` → the local kernel's zeros,
  `site=HOME` (first touch, once).
- **Guest pull** (`VMR_OP_CTXPAGE`). The destination asks the home. Home reads
  the entry. `transit≠NONE` → WAIT, re-decide. `HOME` → `transit=TO_REMOTE`,
  take from the source mm, send, `site=REMOTE` on the `INSTALLED` ack, bump+wake.
  `REMOTE` → "you hold it" — the destination resolves from its own kernel facts
  (a sibling's landing, its object; **P3** guarantees a live AS always produces
  the page, so this never falls back to a stale copy). `NONE` → ABSENT (fresh).
- **Destination GET serve.** Present in a context or the object → take and send.
  Nothing present, a transition to it in flight → the waiter is already parked
  on the home entry; the serve is the landing. Nothing present, dead holder →
  **P2** dead-token net (the retained copy, for `!task_alive` only). There is no
  live-holder retained serve and no grace clock: a live holder that cannot
  produce its page is a transition still in flight, and the entry says so.

### What it deletes
The 1200 ms grace clock; the 100 ms INTRANSIT heal; the 1.5 s INFLIGHT re-ask
loop; the 2000-yield claim throttle; the fault-path "landing/arriving" stamp
override; `gen_repair` and its arms; the retained-copy net for a live AS. The
generation stamp stays as an assertion only (PRINCIPLES §7). `pgown`, `infl[]`,
`lent[]`, `pgstate.h` collapse into the one segment (or become caches the
segment's transitions write). The stamp can no longer decide anything because
there is one record and one transition, and the second party blocks on it.

### Build order (staged; the baseline is never regressed to reach a stage)
1. **The substrate** (`user/own.h`, done this session): the shared segment, its
   process-shared futex mutex, the entry, and the `own_begin`/`own_land`/
   `own_wait` single-transition API. Compiles into both halves; NOT yet
   load-bearing. Additive — the passing baseline is untouched.
2. **The receiving side onto the entry**, WITHOUT the source fetch gate (the
   part that regresses hx2, three times proven): a guest fault and a sibling's
   read of one page cannot both act on it independently — the second WAITs on
   the entry's `gen` for the first's landing. This is P3's object backstop
   expressed as a transition, so the wait never has to fall back to a stale
   copy. Judge it by the coherence story (does a live holder ever get a copy
   picked by a clock?), not by the 1:1 pass count.
   - **2a DONE (session 34): the entry wired in as a receiving-side OBSERVER**
     (`ownf_begin`/`ownf_end` at the two `pull_begin` sites), deciding nothing,
     and measured on the 4-pool chain. Result: the substrate is provably
     correct at scale — hx2 pool **claimed=26407, landed=26407 (begin==land)**,
     no overflow, no stuck transit; and the receiving-side contention is real
     and hx2-specific — **~48% of hx2 fetches (12646, 44/48 runs) found a
     sibling already fetching the same page**, which pull_tab lets RACE, while
     pg3/ws1/sig1 = 0. This is the direct measurement that justifies the
     landing-wait, and it names hx2 as the pool the receiving-side entry fixes.
   - **2b BUILT (session 35): the BUSY faulter WAITS on the landing.** The
     claim moved UP from the two pull sites to `fault_from_home_mode`: the
     whole service of a real guest fault (pull AND install) is ONE transition
     on the (as,page) entry, released on every exit — `own_land(REMOTE)` if a
     context of the address space has the page present, `own_abort` otherwise
     (a yield, a lost claim, ABSENT) — so a waiter never wakes into the
     claimer's install window (the 2a observer landed after the pull, before
     the installs) and a claim cannot collide with itself (site 1's ABSENT
     falls through into site 2). A sibling that finds BUSY blocks on the
     entry's futex (`own_wait`, 3 s diagnosis bound) at coh depth 0 and
     re-faults; the bound's expiry runs the fault UNGATED and is counted
     (`n_ownf_wait_timeout`, must be ~0). Prefills stay ungated;
     `pg_serve_conn` never touches the entry (no cross-machine cycle); NO
     source gate. hx2 smoke 8×4 (SRCID `e61a3d7e2aa6`): 8/8 PASS; per run
     ~810 claims, ~275 waits, **woken == waited, timeout 0, full 0**;
     `pull_overlap` 6447 (s34obs, 48 runs) → **0** — the two-copies-on-the-wire
     race is gone from the destination; `claim_yields` per run 499 → 34 (the
     storm shrank; it did NOT re-explode as the session-30 source gate did).
     ~35% of waiters wake to a page still absent (the claimer aborted) and
     simply claim on the re-fault. **Full chain `s35` (48×8 by census):
     suite 34/0, pg3 48/48 (was 46), ws1 48/48 (was 47), sig1 48/48, hx2
     42/48 then 48/48 and 46/48 on further pools of the same build (136/144 =
     94%, baseline 96%) — flat, the same `FAIL 1` zero-read mode, no hang,
     no death; `wait_timeout` 0 and `full` 0 in every pool. 2b STAYS. The
     hx2 residual is a KERNEL-made zero folio whose door is not yet named:
     MAPOBJ (holes=0), the foreign-read invention (ffile_invent flat across
     a pool) and declines (0) are all refuted by counters —
     HANDOFF.md §35.** Next for the channel: step 3 (the source's
     pgown/infl[] onto the segment) — and, separately, the kernel instrument
     that names the zero folio's door.
3. **The source's ownership onto the same segment**: `pgown`/`infl[]` become
   reads and transitions of the entry; the claim window closes because a fetch
   is one transition, and a second faulter waits on it rather than raising a
   competing claim.
4. **Delete the tie-break machinery** the entry has made unreachable, and demote
   the stamp to an assertion (PRINCIPLES §7).

The sections below (the DIRECTORY, P1–P5) are the earlier framing and the
measured refutations that led here; they stand as the record. Where they say
"gate" or "clock", the shared-memory entry is what replaces it.

## The ownership DIRECTORY (session 32) — the owner's hint, made concrete

The owner's hint after session 31: *"instances of vmhome and vmremote should
share the memory on ownership, so that you may consult who owns what page
globally, at a much better speed and scale."* Read against the code, the hint
names the actual defect class, and it is not any one of P1–P5: it is that
**there is no single place that knows who owns a page.** Today five records,
on two machines, each hold a private belief about the same fact —

| side | record | what it says | written by |
|---|---|---|---|
| source | `pgown` (OURS/THEIRS/CLAIM/NONE/INTRANSIT + gen + sum) | where the page is, per the source | serves, fetches, acks, a 100 ms heal |
| source | `infl[]` | this side is fetching it | the fault service |
| destination | `lent[]` (owner/shared/watched) | who holds it, per the destination | takes, installs, settles |
| destination | `installed[]`, `retain_tab`, `pgen` | what was installed, last bytes sent, capture count | installs, serves |
| destination | `pgstate.h` (INVALID/SHARED/OWNED + 4 transients) | per-process hand-off lock | faults, GET serves |

— and every place two of them disagree is "decided" by a clock, a stamp, a
retained copy or a yield storm. Sessions 30–31 measured both halves of the
one-sided fix: gate the source's fetches (P1) and hx2 *regresses* (249→216,
the claim storm); stop the source claiming during a fetch and the storm dies
but hx2 does not recover (43/48) because the destination now answers from
ITS record (the retained copy). The failing runs of both pools, re-read this
session, are the same photograph every time, on `0x7ffff7ff6000`:

```
source:  context N faulted on P, the page channel failed and did not recover,
         and the record says the page exists (CLAIM). Ending the context
source:  P was handed to the guest's machine (THEIRS) and that machine now
         answers ABSENT for it ... on neither machine after 40 asks
source:  P answered ABSENT and filled with an empty page (record OURS)
guest:   writer read 0
```

That is a **circular wait between two records**: the source holds `PG_CLAIM`
on P while its GET loops on the destination's `INFLIGHT_PULL`; the
destination's pull of P is refused `CLAIMING` by that very claim, so its pull
stays "active" and it keeps answering `INFLIGHT_PULL`; at 1.5 s the source
gives up and ends the context; a dead context "holds nothing", so the next
ask is ABSENT, and ABSENT licenses zeros. Neither side is wrong by its own
record. There is simply no record that sees both.

### What "share the memory on ownership" can mean across two machines
In the loopback rig both halves run on one box and could literally map one
segment. Production is two kernels on two machines: no shared memory exists,
and a shared segment that works only in loopback would be an A/B arm of the
kind PRINCIPLES forbids (optional correctness). The form of the hint that
holds on both rigs is the classic one for multi-producer/multi-consumer
memory sharing — a **home-based directory**: ONE directory, at the source
(the home of every page: it owns the program, it wins every conflict by role,
PRINCIPLES §6), holding one entry per (address space, page) with exactly one
transition in flight at a time; the destination keeps no belief of its own,
only kernel facts (what its page tables and object hold) and the transition
it is currently executing. Every move goes through the entry; waiters block
on the entry (a condvar), never poll, never yield-and-refault, never guess.

```
entry(as, page):  where   ∈ {HOME, REMOTE, NONE}        -- the token's site
                  transit ∈ {none, TO_HOME, TO_REMOTE}  -- at most ONE
                  waiters: condvar; the transition owner broadcasts on landing
```

- **Home fault** (source absence fault): transit in flight → WAIT for it (a
  sibling's TO_HOME lands the page: refault, done; a TO_REMOTE acks: then
  fetch). REMOTE → TO_HOME, GET, install, HOME, broadcast. NONE → the local
  kernel's zeros, HOME (first touch, once).
- **Guest pull** (`VMR_OP_CTXPAGE`): transit in flight → WAIT for it, then
  decide again. HOME → TO_REMOTE, take, send, REMOTE on the `INSTALLED` ack.
  REMOTE → "you hold it" (the destination resolves from its kernel facts:
  a sibling's landing, its object). NONE → ABSENT (fresh).
- **Destination GET serve**: present in a context or in the object → take and
  send (its only job). Nothing present and its own pull in flight → that pull
  is queued at the home entry behind the very transition the GET is — so the
  GET preempts it (the source wins by role), never waits for it. Nothing
  present, nothing in flight → the token died here (P3): land the last
  bytes in the object or say LOST, named. The retained copy is the
  dead-token net only (P2), which is now safe without a clock because a live
  holder never asks while its own transit is in flight.

Speed: the directory is memory at home, one entry lookup per transition — no
`/proc/pid/pagemap` reads to decide ownership (they remain the PHYSICAL
check, asked once per landing), no 1.5 s INFLIGHT loops, no 1200 ms grace,
no yield storms. Scale: one entry per touched page, keyed (as, page) — the
256 address spaces of the mm table are 256 keys, not 256 tables.

### What it deletes (beyond the REDESIGN list)
`pgown`'s CLAIM as a refusal (a pull waits instead), the 1200 ms grace and the
`infl_ans` episode clock, the INFLIGHT re-ask loop's 1.5 s give-up (there is
no circle left to time out), `pg_claim_wait`'s 1 s spin on a requester,
`lent[]` as a decider (it becomes a cache the directory's messages write, then
nothing), `PG_GOT_LANDED` and both landing/arriving tie-breaks (already gone
with the gate).

### Stage D1 (session 32, BUILT): the directory's first form, at the source
Both halves of the entry's single-transition rule, on the session-30 gate:
1. `infl_enter` — one fetch per (as, page); a second faulter waits on the
   condvar and joins the landing (session 30's code, recovered from its
   scratchpad; the honest-record hunk NOT taken: `PG_CLAIM` is still marked,
   but nothing storms on it any more).
2. **A guest pull for a page this side is fetching WAITS for the landing**
   (`infl_wait_fetch`, 3 s bound, single-page serves) and is then served from
   the landed page by the ordinary take. No CLAIMING answer is given while a
   fetch is in flight, so there is no storm to widen, and the bytes the pull
   gets are the landing's, current by construction.
3. On the destination, a source GET **preempts** this side's own pending
   request of the page (`pg_claim_for_source`): the requester's pull is
   queued at the source behind this very GET's landing, so waiting for it to
   settle (the old 1 s `pg_claim_wait` spin) was waiting for ourselves.
Counters: `pulls ... waited for the landing ... / waited the 3 s bound (must
be 0)` (vmhome), `GETs whose take preempted this side's own pending request`
(vmremote).

### Stage D1 RESULT (session 32): REGRESSED hx2 — the gate is still a net negative; REVERTED
Deployed on #149 (SRCID 62aaedade587), gated 48×8 graded by guest.out:
**hx2 36/48 (baseline 46/48 — REGRESSED 10), pg3 47/48, ws1 48/48, sig1
48/48**; suite 33/1 (the one pg3 the residual §6.1a split-brain). The
`waited the 3 s bound (must be 0)` counter fired **7–14 per run**, 60–76
SECONDS of serve-side stall per pool: `infl_wait_fetch` waits for THIS side's
fetch of the page to land, but that fetch is itself the GET looping on the
destination's `INFLIGHT_PULL`, so it routinely does not land inside 3 s and
the serve falls through to CLAIMING anyway — a stall added, the circle not
broken. The losses moved but did not go: they are now `pull active 1`, a
writer reading a value ~1.6 M rounds behind (the receiving side installs /
reads a stale copy while its own pull is in flight), where the baseline's
were `read 0` (a context ended and zero-filled). **The lesson is session 30's
and session 31's, re-confirmed a third time: any form built on the source
FETCH GATE regresses hx2, because the gate serialises fetches and widens the
window the storm rides, and the failure is P2 on the RECEIVING side, which no
source-side serialisation touches.** D1 was reverted to the session-31
baseline (session-28 tip + the `as_any_present` fix, SRCID 26c1cad44cf0, hx2
46/48). The DIRECTORY DESIGN above stands as the plan; its first *code* form
must NOT reuse the gate. The next build is the receiving-side single-transition
entry: a guest fault and a sibling's read of one page cannot both read/install
it independently — the second WAITS on the entry for the first's landing —
implemented WITHOUT the source fetch gate (which is the part that regresses),
i.e. Stage 3's P3 object backstop (a live AS always produces the page, so the
receiving wait never has to fall back to a stale copy) is the real first step,
exactly as the session-31 handoff said. Do P3 before any gate.

## The invariant (PRINCIPLES §1, ARCHITECTURE §5)
Exactly one writable copy of any page. Ownership moves only through an atomic
kernel claim (`vmctx_folio_claim` freezes the refcount iff nothing else can
reach the folio). Userspace never decides a page "looks free"; it asks. A
refused claim means WAIT (§2), never a substitute answer. A page that exists
somewhere and cannot be fetched is a **named failure**, never invented (§3).

If the invariant holds, two stamps can never disagree — the holder's copy is
current by definition. So every stamp comparison, every retained serve, every
"landing beats arriving" tie-break marks a state the invariant already
forbids. The fix is upstream of the comparison, always.

## The five producers of a second copy, and the fix for each

### P1 — Two fetches of one page in flight (the claim storm)
Two service threads of one address space fault on one page and BOTH issue a
`VMR_OP_CTXPAGE` / `VMR_PG_GET`. Nothing serialises them, so the page can move
between the two answers and a comparison is needed to pick a winner — the
"landing beats arriving bytes" override, and the pg3 storm (the second fetch
sits in a 1200 ms CLAIM refusing every guest pull of a page the source
already holds).

**Fix: a per-(AS,page) fetch gate on the source.** `infl[]` is promoted from
a leaf marker (consulted only when *answering* the other machine) to a real
gate: the first faulter for a page claims it and fetches; a second faulter for
the same page BLOCKS on a condvar until the first lands the page, then
refaults and finds it present — its fault is simply over, no second GET is
ever issued. One fetch per page per AS. This removes the storm at its source
and removes the fault-path override's reason to exist.

### P2 — The retained copy served to a LIVE holder (the 1200 ms grace)
The destination keeps the bytes it last transmitted and serves them, chosen by
a young-episode clock, when "the asker holds it per the record but cannot
produce it." For a genuinely dead token (a context that died mid-service
holding the only copy — ws1/sig1 teardown) that retained copy is the newest
that exists and is correct. For a LIVE holder it is a second copy picked by a
clock.

**Fix: serve the retained copy ONLY when the holder is provably dead
(`!task_alive`).** For a live holder, answer INFLIGHT and let P1's gate and
P3's conservation resolve it — a live holder that cannot produce a page it
records as its own is a P3/P4 token loss, fixed at the source. The grace-clock
arm is deleted; the retained table stays only as the dead-token net.

### P2 RESULT (session 33): BUILT in isolation, REFUTED — the arm is BOTH bugs at once; REVERTED
Built exactly as specified (delete the 1200ms clock arm; a LIVE holder always
gets INFLIGHT, bounded by the source's own 1.5s GET loop; retained served only
for `!task_alive`), NO source fetch gate. Deployed on #149 (SRCID 3d0299bd0cf6),
gated 48x8 by guest.out: **hx2 44/48 (baseline 44 — FLAT), pg3 47/48, ws1 48/48,
sig1 48/48.** Two findings:
1. **The clock arm is not load-bearing and bounded-INFLIGHT does NOT hang sig1**
   (ws1/sig1 stayed 48/48). The old fear was about the UNCAPPED arm; retained-
   for-dead is safe for the teardown cases.
2. **hx2 stayed flat but the failure MODE got worse**: 2 of the 4 hx2 fails
   became HANGS. The hang is on a page the SOURCE records as OURS (it holds it)
   yet faults on and cannot produce — a genuine token loss — while the
   destination answers INFLIGHT because the holder is alive: neither side can
   produce it, deadlock. **The retained copy was the ESCAPE from that deadlock.**

**The decisive lesson (4th confirmation):** the live-holder / no-pull-active
retained arm is a SINGLE arm that is BOTH bugs — serving the retained copy to a
writer whose token is slowly IN TRANSIT is hx2's stale-serve; withholding it
from a holder whose token is LOST is the hang. They are **locally
indistinguishable** (no landing signal), so any local rule — a clock, a stamp,
a retained serve — must be wrong for one of the two. Only the DIRECTORY (one
record that knows the token's SITE) tells them apart. P2 in isolation just
relocates the loss (stale-serve -> hang), net flat, and is NOT a keeper.
REVERTED to the session-31 baseline (SRCID 26c1cad44cf0). See
`HANDOFF.md §33`. Next: build the DIRECTORY proper, not another local arm.

### P3 — A taken buffer dropped (the dead token)
A take-back (the destination pulling a page for a write) removes the page from
the guest; the bytes are then only in the fetching thread's buffer until the
install. If the install fails — the commonest case is the clear-tid write into
a DYING context during teardown — the bytes are lost and the page is on
neither machine. Today: retry through a sibling, else PUT back, else the
retained net catches it.

**Fix: token conservation — a taken buffer is never dropped.** Install order,
each a real landing: a live context of the AS → the address-space OBJECT
(`obj_write`; the object outlives every context, so a later fault re-reads the
bytes) → put back to the source. With the object write always available, the
bytes land somewhere the next fault finds them, and no retained-copy backstop
is needed for a live AS.

### P4 — The source holding a page its record says it handed over
Failing runs print "requests refused because the record said the page was
handed over, where this side was holding it anyway: 7.5k–12.7k" and the sweep
finds pages present that the record calls THEIRS. Two producers:

- **P4a RESTORE.** A refused per-context TAKE that had already unmapped puts a
  stale pre-take copy back (`n_take_restored`). **Fix: a refused take must not
  have unmapped.** Route hand-overs through `TAKEOBJ` (atomic unmap+copy+punch
  under the folio lock — no leftover to restore). The per-context TAKE remains
  only for anon/private pages, where the kernel precheck refuses *before* the
  zap (the page stays intact) and record+page move together. Delete the
  RESTORE resurrection; `vmctx_take_lost` must stay 0 (a take that unmapped and
  then could not serve is the one bug this leaves, and it is already counted).

- **P4b The monitor's own read creates pages.** `VMCTX_CTL_PEEK` reads with
  `access_process_vm(target, …, 0)` — no `FOLL_NOFAULT`. Peeking a page that
  is not present in a context's mm therefore FAULTS IT IN, and because the
  faulting task is the vmhome thread (not the context) the vmctx redirect does
  not fire and `do_anonymous_page` mints a zero page: the page becomes present
  on the source, the record still says THEIRS, both machines hold it. **Fix
  (kernel one-liner): `FOLL_NOFAULT` on the read peek** — an absent page
  returns short, is never created. The write peek keeps `FOLL_FORCE` (it must
  install text/rodata) but is already gated on the page being present.

### P5 — The record flips before the bytes land (INTRANSIT heal)
The source marks THEIRS only after the destination's `VMR_OP_INSTALLED` ack —
correct, two-phase. But a 100 ms clock heals a "stale" INTRANSIT to THEIRS
(`n_intransit_healed`), a clock deciding ownership. **Fix: delete the heal; an
unacked INTRANSIT past a bound is a named, counted failure, not a silent
flip.** Lowest priority — measure the ack-loss rate first; the ack is sent
off the critical path (`ack_flush`) and may simply need to be reliable.

## The generation stamp — debug assertion only
`pull_gen_check` stays, but only to ASSERT: on every served page it compares
the source's stamp with the destination's capture count and, on a
disagreement, logs STALE-SERVE and fires the trail — a counted defect
(PRINCIPLES §7). It NEVER repairs from a second copy and NEVER breaks a tie:
`gen_repair()` is forced off and its repair arms deleted; the source-side
"arriving newer than the landing, installed over it" override is deleted; the
INFLIGHT reply keeps carrying `gen`/`why` for the LOG, read by nothing that
decides. A disagreement is a bug to be chased at its producer above, not
smoothed over here.

## Stage 1 RESULT (session 30): the gate is right for pg3, WRONG for hx2 as specified
Built, deployed on #149, gated (`tests/gate.sh`, census in `tests/census.sh`).
- suite ×3 **102/0**; pg3 pool **251/256** (in band — the gate is clean here:
  `gate_joined` 29 total, `claim_yields`/`refused_but_here` ~0 over 251 runs);
- hx2 pool **216/256 — REGRESSED from 249**. The census names it: `claim_yields`
  and `refused_but_here` (`n_gone_but_here`) both jumped ~300–4000× to ~94 000
  per run. **The claim storm re-exploded.** The gate serialises the
  two-source-threads-fetch race (pg3's producer) but NOT the source-claim-window
  vs destination-write storm (hx2's producer): the source holds `PG_CLAIM` across
  its whole fetch and the destination storms CLAIMING→yield→refault for the hot
  page. The DELETED machinery (`PG_GOT_LANDED`, the pre-ask "already present →
  DONE") had been doubling as the claim-window shortener that kept that storm
  small. **P1's premise — "the gate makes the tie-breaks unnecessary" — holds for
  pg3 and is false for hx2; they are two different producers.** The fix is a
  source-level claim-window shortener that is decisive, not a re-added clock or
  tie-break (session 27's "make the crossing decisive"). See
  `HANDOFF.md §30`. Stages 2–4 not started until hx2 is back in band.

## Stage 1 RESULT (session 31): the claim-window fix was BUILT and REFUTED — the failure is P2, not the storm; REVERTED
Session 30's "decisive claim-window fix" was built (the honest record: an
absence fault holds nothing, so it does NOT mark `PG_CLAIM` before its GET; the
`infl[]` slot still marks the in-flight fetch — no clock, no tie-break). It
**killed the storm** — hx2 `claim_yields` 84 000 → 210/run, `refused_but_here`
→ ~0, and pg3 stayed 48/48 clean. **But hx2 did NOT recover (43/48, worse than
the session-28 tip's 46/48).** The storm is WASTE, not the failure: killing it
merely pushed more GETs into the far side's INFLIGHT/grace path and RAISED
`retained_served` to ~5/run in every run. Also built: the DESTINATION fetch gate
(completing P1 symmetrically) — it cut `pull_overlap` ~15000 → ~4300 but its
join fired 0 times (under the storm nobody can land the page) and the pass rate
did not move. **The real failure is P2**: the destination serves its RETAINED
copy (stamped gen==count, so the generation check passes) to a LIVE holder, and
those bytes are stale by thousands of the guest's un-captured writes (the losing
writer read a value ~97 734 rounds old). **P2 cannot be fixed standalone** —
answering a live holder INFLIGHT is the arm the retained branch's own comment
already refutes ("sig1's teardown shape, hung by the no-cap arm"): it needs P3's
object backstop so a live AS can always produce the page. **The gate is a net
negative** (pg3 252→251, hx2 249→216; it serialises fetches, which widens the
storm) and was REVERTED with both experiments to the session-28 tip (`41af447`,
hx2 46/48 baseline). **Next: skip the claim-window; go to Stage 3 (P3+P2).** See
`HANDOFF.md §31`.

## Stages, each gated (suite ×3, pg3 pool ×256, hx2 pool ×256)
1. **P1 the fetch gate** + delete the fault-path stamp override. Kills the
   storm at its source; the biggest single change. *(SESSION 30: done, regressed
   hx2. SESSION 31: the claim-window fix that would make it safe was built and
   REFUTED — killing the storm does not move the hx2 pass rate, because the
   failure is P2 on the destination, not the storm. The gate is a net negative
   and was REVERTED. Do Stage 3 FIRST, on the session-28 tip; reconsider the
   gate only after hx2 is green.)*
2. **P4b `FOLL_NOFAULT`** (kernel, rebuild+reboot) + **P4a** delete RESTORE,
   route through TAKEOBJ. Removes the source's "holding it anyway".
3. **P3 token conservation** (object-write backstop) + **P2** retained-only-
   for-dead + delete the grace clock. Removes the live-holder retained serve.
4. **Stamp → assertion**: `gen_repair` off and repairs deleted, override
   deleted, **P5** heal deleted. The stamp can no longer decide anything.

A stage that regresses a gate is wrong at its own source and is fixed or
reverted before the next — never masked by re-tuning a downstream clock.

## What gets deleted when this is done
The 1200 ms grace clock; the 100 ms INTRANSIT heal; the fault-path
"landing/arriving" stamp override; `gen_repair` and its two repair arms; the
RESTORE resurrection; the 2000-yield claim throttle (no storm to throttle).
What remains: the atomic kernel claim, the two-phase hand-over with a real
ack, the per-(AS,page) fetch gate, the object as the always-available landing,
and the retained copy as the dead-token net only.
