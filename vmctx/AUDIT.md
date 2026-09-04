# AUDIT — how each piece works, in detail

ARCHITECTURE.md is the map; this is the close reading: every table, every
protocol word, every decision order, every bound, every instrument, and the
invariant each one carries. §10 is the bug ledger — every named defect this
system has had, its mechanism, and the fix that killed it — and §11 is the
corner-case catalog: the shapes that were measured to matter, whether or not
they ever broke. Line numbers are avoided on purpose (they rot); identifiers
and site names are the anchors. History and refuted alternatives are
NOTES.md; the session-by-session trail is HANDOFF.md §1 (the session
ledger, 27 → 46b), with the originals under doc-archive/handoffs/.

## 1. The kernel core (one patch per tree; kernel-patches/ is the record)

### 1.1 The two syscalls

- `vmctx_run(cfg)` (472 on 7.0.14, 470 on 6.18.35): turn the calling task
  into a VM context. `vmctx_run_config.flags`: USERCODE (run user code, not
  a full VM), EXIT_PROCESS, REDIRECT_SYSCALL, REDIRECT_FAULT, WAIT_MONITOR
  (park until a monitor attaches AND releases), RESTORE (start from a
  monitor-installed register set).
- `vmctx_ctl(pid, op, arg)` (473/471). Ops (the ABI; the userspace copies
  of this list drifted once and PROTECT went unused for weeks — the
  constants live in vmhome.c/vmremote.c with that warning):
  ATTACH 1, DETACH 2, WAIT 3, RESUME 4, GETREGS 5, SETREGS 6, PEEK 7,
  POKE 8, TAKE 9, PROTECT 10, SYSCALL 11, ADOPT 12, TAKEOBJ 13,
  GET_CLEAR_TID 14, PROTECTOBJ 15, MAPOBJ 16, APPLYMAP 17, FUTEXWAKE 19,
  TRYFAULT 20, SERVE 21, LAND 22, MMLOG 23, PGACK 24, PGSCAN 25,
  PGSTATE 26, PGSET 27, LASTCHILD 28.

Semantics that carry invariants:

- **TAKE** removes one page from one context's page tables and returns its
  bytes. -EBUSY = "not yet" (a hold, a pin, DMA) and the caller WAITS; a
  refused take must never destroy the page (`vmctx_take_refused/lost/
  regained` count the kernel's own view). The take's COW handling breaks
  only a genuine share — `folio_mapcount > 1`, where do_wp_page must copy —
  judged by the NEW folio GUP returns, never the original's mapcount.
- **TAKEOBJ** is the take under the backing-object folio lock: zap every
  mapper, copy, punch, in one operation — no inter-piece window. -ENOENT
  means "this task has no vma there", NOT "the AS lacks the page": retry
  against each sibling. Essentially every hand-over takes this path.
- **PROTECT/PROTECTOBJ** leave a readable copy that cannot be written (PTE
  only; the program cannot observe the VMA), so the next write arrives as a
  protection fault where ownership is settled again.
- **MAPOBJ** maps what the object already holds and NEVER faults: a hole
  answers 0 bytes. The folio lock closes the map-vs-TAKEOBJ TOCTOU.
- **APPLYMAP**: only UNMAP and ZAP are cross-task; SET and PROT are REFUSED
  for a task that is not the caller — before that refusal a cross-task SET
  mapped the guest's object into the MONITOR's own mm, which every later
  clone()d context inherited (§10.7). A sibling maps and re-protects a
  range on its own next fault; a racing punch of a fresh SET is repaired
  from the construction record (§4.6).
- **TRYFAULT** asks: would this mm's own kernel handle an access at addr?
  0 = the address is real; only -EACCES/-EFAULT may be ruled faults. The
  kernel is the only authority on validity; /proc/maps text never is.
- **SYSCALL** performs one call in the context's own mm; one at a time per
  context (the assisted slot). **ADOPT** turns a ptrace-stopped task into a
  service context without letting it run. **LASTCHILD** reads the HOST-
  namespace pid of the task's last clone child, recorded in kernel_clone
  after alloc_pid — the assisted clone's return value is namespace-relative
  and names the wrong task from the monitor's side (§10.14).

### 1.2 The page record (per-mm xarray beside the page tables)

One entry per (mm, page), packed into an xarray value:
`state(4) | sum(16) | gen(32) | stamp(10 bits of jiffies)`. States:
NONE, HOME (this side holds it), REMOTE/THEIRS (handed over), CLAIM (a
fault is fetching it), TRANSIT (a serve's bytes are on the wire, ack
pending). Five ctls read and write it (SERVE/LAND/PGACK/PGSCAN/PGSTATE+
PGSET); the record is consulted UNDER the same locks that move pages, so
it cannot disagree with the page tables it stands beside.

**The SERVE decision table** (VMCTX_CTL_SERVE, the one gate every
hand-over passes):

- CLAIM → CLAIMING (the fault that claimed it will land it; a claim now
  always settles with the service that wrote it, §3.6).
- TRANSIT younger than `vmctx_pgrec_transit_ms` (100) → CLAIMING; older →
  the ack is lost, not late: HEALED to REMOTE (`vmctx_pgrec_healed`) and
  the serve proceeds as for REMOTE.
- REMOTE → ABSENT (nothing here to give; the asker's record decides).
- HOME/NONE, page present → the take (CAS the entry to TRANSIT sum 0,
  vmctx_take_locked, then the outcome CAS writes TAKEN+sum / COPIED /
  the revert). "Moved under us" mid-CAS → CLAIMING.
- **HOME but ABSENT in the mm** → CLAIMING for a bounded number of ASKS,
  counted in the entry's sum field (0xa500 + n, written through the same
  outcome CAS — a clock cannot age what every refusal restamps), and at
  255 consecutive absent-asks the claim is a GHOST: healed to REMOTE,
  answered ABSENT, the asker fills and holds the page exactly as the
  record then says (`vmctx_pgrec_home_healed`). This branch was the last
  eternal answer in the table and the stuck-page storm's whole mechanism
  (§10.20).
- Read-only ranges: CLAIM/TRANSIT → CLAIMING, REMOTE → ABSENT, else the
  bytes are copied (RELRO-sealed ranges ask the other machine; an unsealed
  file page is served from the file, which is current by definition).
- A syscall hold on the page → -EBUSY ("not yet", never a state change).

### 1.3 The mm change log (event-time stamps)

The mmu-notifier hook records vacated ranges per mm with a per-mm sequence
number assigned AT THE EVENT under the log's lock (`vac[].seq`; bumped on
drops too). MMLOG returns per-entry seq plus `cur`, the counter at the
drain read under the same lock. Userspace stamps were a drain-time race
(§10.15): two connections drain concurrently, the later stamp lands on the
earlier event, and a recycled range's stale vacate outranks the mmap that
superseded it. A construction's stamp EQUALS the last event's seq, so on
any overlap a TIE IS THE CONSTRUCTION'S — the kernel's own address
allocation proves the order (§10.16).

### 1.4 Monitor life-cycle in the kernel

`vmctx_report` publishes an event and waits for the monitor's reply under
the fault deadline (6000 ms default, `vmctx_fault_deadline_ms`; 0 = never —
the "slow or stuck?" arm). Three monitor states:

- attached and alive → normal publish/wait;
- none yet → the event waits; WAIT_MONITOR contexts cannot even ENTER
  until a monitor has attached and released them, and a REDIRECT context
  with nobody attached at entry is REFUSED rather than allowed to
  improvise;
- **dead** → the same case as none: `vmctx_drop_monitor` and fall through
  to publish, so the event waits for a SUCCESSOR under the deadline. It
  used to return ACT_SELF here, which for a redirected context is an
  unrecoverable -14 on the first fault — the orphaned-daemon death
  (§10.21).

ATTACH installs a monitor only when there is none (EBUSY otherwise) and
wakes the entry gate; DETACH is the handoff's first half (§4.9).

### 1.5 vmctx_copy_task (fork of a service context)

The child inherits the run parameters, the SHARED object (same fd — that
is the point of it), and the parent's monitor ("there is no window in
which the monitor could attach instead: the child may fault before its
parent's clone() has even returned"). The child's pages are NOT wiped;
the comment above the decision carries the full four-build ledger of the
wipe (§11.6) and the serve-chain that answers a child's fault instead.

## 2. The module (kernel/vmctx.ko)

The SVM/VMX backends: enter the guest, capture exits, publish syscall and
fault events. The backend never writes VMCB/VMCS tsc_offset — the guest's
rdtsc reads the HOST TSC directly, which is the fact the vDSO design
stands on (§6). Guest-ASID TLB shootdown is wired through the backend's
invalidate (`tlb_ctl` re-armed at entry on generation change — LOAD-
BEARING; removing it re-opens pg3's stale-TLB one-behind, §10.4). A fault
the monitor declined that the local kernel then cannot serve is REPORTED
AGAIN once, bounded per address (`unrec_*`) — the second chance that lets
the construction repair fire after a racing punch (§4.6).

## 3. vmhome — the source

Owns the program: its files, pids, credentials, namespaces, syscalls, and
the authoritative record of its memory. One service context per guest
task (fork children included), each held parked and never executing;
every context's events are serviced by a per-connection thread.

### 3.1 Startup and handover

fork → PTRACE_TRACEME → personality(ADDR_NO_RANDOMIZE) → execv. At the
exec stop (BEFORE the loader runs — ld.so reads auxv as its first act):
if the destination's kernel release differs (§6), the stack's
AT_SYSINFO_EHDR is patched to AT_IGNORE. Then a breakpoint at AT_ENTRY
runs the LOADER to completion natively — every library mapped, relocated,
TLS built by this kernel — and the finished address space is handed over:
ADOPT before DETACH (no window in which a detached tracee simply runs),
registers shipped (fs/gs bases included — the guest must not run on the
monitor's thread pointer, §10.3), /proc/maps text as the layout.

### 3.2 The fault-service flow (per context, the exact order)

For each VMCTX_EV_FAULT on a source-side service task:

1. Classify (VMCTX_CTL_SERVE probe: record state, class bits, presence).
2. Ask the destination: GET (VMR_PG_GET; GETS for sealed read-only file
   pages — a copy, the guest keeps executing its own). Channel failure →
   30 bounded retries on fresh connections; a port that has refused 20
   consecutive connects is declared DEAD and every later dial fails fast
   (§10.23).
3. Answers, in order of arrival:
   - bytes → LAND (install + record, one step; -EEXIST = a sibling's path
     landed it first, the arriving copy is dropped). The landing gives
     every fork copy still owed this page its bytes NOW — a page that
     leaves stops being what the copy inherited.
   - INFLIGHT → the token is in transit; ask again (the landing decides).
   - COWBREAK → this address space is a fork copy and the page is still
     the original's, over there: pg_get_op(VMR_PG_COWBREAK) takes the
     copy's page from the destination's chain walk (§4.7).
   - ABSENT, private file page, fork copy → the parent's DIVERGED copy is
     asked for first (COWBREAK); "nothing owed" falls through to the
     FILE, which is then genuinely current. Serving the file without
     asking hands a fork child the program as LOADED (§10.9).
   - ABSENT, record says GONE/THEIRS → re-ask up to 40× (the page may be
     mid-flight); still absent → END the context rather than zero-fill
     live memory. The KILL must STICK: a context that ignores three KILL
     answers is SIGKILLed and its service closed — the same fault
     re-arriving forever held a finished run for the watchdog (§10.24).
   - ABSENT, fresh anonymous, fork copy → the inheritance is found in
     this order, each step licensed by the failure of the one before:
     (a) the destination already refused the strict break;
     (b) the CHAIN WALK: the nearest ancestor whose mm HOLDS the page
         (ctx_present — a pagemap read, never a peek, which would create
         a page), copied raw and landed (§10.19);
     (c) the FAULTING read of the direct parent (pulls the page home
         through the standard capture if it lives on the other machine);
     (d) the ESCALATED break: re-ask the destination with rq.len == 1 —
         "my routes all failed" — and it may serve the nearest READABLE
         ancestor, protect mark or not (§4.7, §10.19);
     (e) only then zeros, landed as an ordinary page (the kernel's shared
         zero page is unclaimable and livelocks takes, §10.5).
   - ABSENT, fresh anonymous, not a copy → zeros as above (a first touch).
4. On the answered path: a claim this thread wrote is settled BY OUTCOME —
   and settled too on every break-out path (kill-escape included): a
   leaked CLAIM is the stuck-page livelock every sibling then spins on.

### 3.3 The two-phase hand-over and per-connection transits

A single-page serve commits the record to TRANSIT with the served sum;
the destination's INSTALLED ack (one-way, no reply) retires it via PGACK,
sum-guarded so a late ack from an earlier hand-over of the same page
cannot retire a newer one. Cold multi-page serves settle immediately
(an ack would need a payload). Every transit THIS connection commits is
tracked in a per-thread ring; acks arrive on the connection that served,
so at connection close every entry still IN TRANSIT names a hand-over
nobody will settle — each is settled THEIRS ("a claim must not outlive
its wire", §10.22). Slots evicted past 64 merely lose sweep coverage.

### 3.4 Forwarded syscalls

Every syscall the guest makes is executed by ITS OWN source task via the
assisted slot (one at a time per context). Pages the call touches are
pulled first; a syscall hold answers takes with -EBUSY ("not yet"). The
readahead window in front of a forwarded call is RTT-gated: it pays only
where a round trip clearly costs more than a take+wire+landing (~35 µs),
which is a LAN and never loopback (measured: sha +40%/syscall with it
forced on loopback). The gate has no override — the A/B that proved it
was deleted with the question.

### 3.5 clear-tid and the join

The service role's in-band CLONE_CHILD_CLEARTID clear is suppressed
(redundant with the teardown clear that always precedes the assisted
exit). At context end the tid word is cleared by poke — through a live
sibling when the owner is gone — and the joiner woken ON THE MM via a
live member, re-armed (bounded) because the joiner's WAIT may not have
registered yet: a one-shot wake is a classic lost wake-up. The wake must
never be aimed at the dying context (it races its own exit, -ESRCH 462
of 500 tries) nor pass through an assisted slot.

### 3.6 Connection close (the lifecycle's source half)

On a per-context connection's end: the guest's real exit status is
delivered (exit_group through the service task, left un-reaped for the
parent's wait4 — reaping it here would answer the parent ECHILD about a
child that exited normally); clear-tid runs as §3.5; unacked transits are
settled (§3.3); and the connection's claims died with their service (§3.2
step 4).

## 4. vmremote — the destination

Owns nothing a program can name — no files, pids, descriptors, or
namespaces. It executes instructions and serves memory, and must be able
to stop at any instant leaving the source carrying on. regions[] is the
program-as-handed-over per address space (start, end, prot, vclass), and
its only jobs are protection application and the vvar-class marking; a
range's bytes always come from the source by address.

### 4.1 fault_from_home (the destination's fault flow, exact order)

One (as,page) ownership transition per guest fault (claimed in the mode
wrapper, released on every exit — the step-2b landing wait):

1. Not vector 14 → the exception goes to the owner raw (frame + regs);
   nothing here interprets it.
2. Shared ranges serve themselves: the first fault applies the recorded
   SET via the fault's own reply; a shared page a peer holds is fetched
   from the peer; a HOLE in the shared object is filled from the owner
   (or is genuinely fresh — said out loud, because a shmem hole the
   kernel fills is the shared zero page, §10.5).
3. region_of(). A fault inside a vvar-class region, when emulation is on
   (§6), is answered with THIS machine's own vvar/vdso page — before any
   coherence machinery, because no other holder can exist.
4. Present-page (protection) faults: the write-to-a-held-page dance —
   pull the page back (the source stops being a holder), or grant a
   fork-protected page through WRITEPREP (COWBREAK → give the copies
   first), or grant a watched page. Declining a write on an unlent
   read-only page ends in the watchdog killing the context (131) — every
   decline is reported.
5. Absent-page faults: pull_wait (a concurrent pull's landing may already
   be answering it), then the holder dance (recall → pull → CLAIM yields,
   bounded), then the chunk fetch (FAULT_CHUNK = 16 pages; one page only
   for COWBREAK answers — a copy is entitled to the page it faulted on,
   not fifteen of the original's).
6. The last resorts, in order: cow_give_from_chain (strict, then RELAXED
   after the source's nocow re-ask proves it empty-handed), the
   construction repair (§4.6), retained-copy serve for a dead AS, and
   the loud decline.

### 4.2 The GET/page service (what this side answers the source)

Serve rules mirror §1.2 through the kernel ctl. Two userspace answers
carry their own invariants: a page the ASKER already holds per the record
is answered from the retained copy rather than ABSENT (ABSENT licenses
zeros over live memory); "nothing readable but my own pull is in flight"
answers INFLIGHT (ask again; the landing decides) — ABSENT there installed
retained zeros over the live page (§10.6).

### 4.3 Loans (lent[])

A page lent to a shadow is recorded per (as,page); `watched` marks a
write-protection this side installed to see the next write (not a loan).
The record is advisory and historically stale — the authority is SEEK_DATA
and the pagemap. Two hygiene rules keep it honest: a HARD vacate clears
the loans in its range (whole-slot memset — a half-cleared slot hands its
phantom `watched` to the next tenant), and ONLY hard: a DISCARD kills
content, not relationships — the machine the loan names still holds its
copy, and clearing the record under it grants the next write against a
holder that was never told ("content dies soft; relationships die hard",
§10.25). A loan whose recall fails on a dead channel falls through to the
source's copy when a region exists; the no-region zero-fill shape died
with the vacate rule.

### 4.4 range_vacated_1 (destructive changes, one path)

Every vacate the source's mm hook reports gets the whole munmap
treatment: give the fork copies what they are owed FIRST (the punch is
where the bytes cease to exist), punch the object and the retained
copies, forget the generation history and (hard only) the loans and the
layout record, then UNMAP/ZAP in every member. Soft (DISCARD) keeps the
mapping and the relationships and drops only content. The constructive
direction stays in reply_note_map: missing constructive information
degrades to a fault that asks; missing destructive information is a
ghost.

### 4.5 reply_note_map (constructive changes)

Mapping syscalls the source performed are applied from the reply at
RESUME by the faulting task itself (a cross-task SET is refused by
design). A fresh non-shared mmap's range is undefined-until-faulted BY
DEFINITION, so the object is punched for exactly that range BEFORE the
mapping goes live — everything under a new SET is the old tenant's
(§10.17). Same-range constructions REPLACE their cons-ring row (recycled
holes stop consuming slots); a live row evicted by wrap is counted.

### 4.6 The construction repair

A fault that would DECLINE, whose task has NO VMA at the address
(TRYFAULT at decline time — the racing punch usually lands between the
event and the local service), inside a LIVE construction row, is answered
with the row's own map SET replayed through the fault's reply. A live row
is proof, not a guess: every legitimate vacate sees and kills its rows,
because the guest cannot name an address before the mmap's reply
publishes it. The module's second-chance report (§2) covers the punch
that lands after the decline. Guarded against livelock (the same
construction ROW repairing back-to-back falls through — keyed on the
row's seq, not the address, since #10.30: an address a program recycles
every round starts a fresh budget with its fresh row).

The lookup-then-punch order is a lock, not a convention: `layout_order_lock`
is held across `range_vacated`'s covering-row lookup AND its punch, and
across `cons_note`'s note, so a vacate either sees the row (VACATE-LATE,
skipped) or lands before the row exists (the map re-applies over it).
`cons_lock` guards only the ring's words. Without the order lock the
punch landed on a live mapping between another thread's note and its map
(#10.30, fc1's churn thread).

### 4.7 The COW chain (fork inheritance on this side)

`cow_give_page_mode(copy, from, page, relaxed)`:
- never for a page the copy was already given or its object holds (the
  original's page may have been written since — those bytes are not
  older, they are WRONG; tests/ex4 measured the child dying in
  _int_malloc on the original's post-fork heap);
- strict mode requires the fork's write-protection to still stand — the
  page provably unwritten since the fork;
- relaxed mode (ONLY when the asker has said its own routes failed: the
  PG-channel ask carries rq.len == 1, or this side's own nocow re-ask
  came back ABSENT) serves the nearest READABLE ancestor page, mark or
  no mark: bytes-current-at-break against the zeros a fork copy is never
  owed. ex4's hazard cannot reach the relaxed arm: its page is served by
  the source's kernel COW long before anything escalates.
- a give that crossed the copy's exec is undone: `exec_wipe` marks the
  lineage FIRST (before the unmap and the punch) and `cow_give_page_mode`
  re-checks the mark after its object write (#10.31 — the old image's
  page in the new image's text);
- a given page is re-marked OWED for the receiver, so a copy OF the copy
  inherits the same bytes (a grandcopy that faulted later used to find
  every ancestor spent or empty-handed).
`cow_give_from_chain` walks up to 16 ancestors because a fork of a fork
inherits pages its own parent never held — an undiverged page has ONE set
of inherited bytes for the whole subtree, because the first store
anywhere comes through the break and gives first.

### 4.8 The vacate/claim instruments that stay armed (capped)

VACATE-LATE (a vacate overlapping a DIFFERENT recent reply's map — the
ordering edge, confirmed firing), FAULT-IN-VACATED (age-stamped ring of
the last 32 vacates), ROW-KILL (which vacate killed which construction
row), NO-VMA (a decline with no live row — unrepairable, names where the
next fix goes), LOST-STORE (zero-filling a page this side installed that
nothing unmapped — the broad detector: a page nothing unmapped cannot
legitimately zero).

### 4.9 The context lifecycle (the destination half)

- `spawn_forked_context`: clone(SIGCHLD) from the servicing thread; the
  child's own backing object (a fork snapshot), registers from the
  parent's GETREGS with the guest clone's rsp/tls ferried through
  (§10.2); ATTACH for the setup, settled SETREGS (the FS base must
  survive the first deschedule), RESUME (release) — then **DETACH**: the
  monitor is handed to the child's own service thread, whose ATTACH is
  what lets the guest enter. A guest can no longer outrun or outlive the
  service that answers for it (§10.21).
- `child_thread`: its own connection (the child's faults are served on a
  channel of its own), OP_CHILD announces by HOST pid, the ATTACH
  takeover (retried by LIVENESS — EBUSY is the spawner an instant from
  its detach), then service. WAIT's ESRCH/EINVAL during the birth window
  are the task not yet being a context — retried while the task is
  alive, never abandoned on a clock (§10.11).
- Main teardown: reap ctx0, then wait for EVERY forked context's service
  to drain, bounded by LIVENESS — while any counted child's task is
  genuinely alive the process stays to serve it; a positive count with
  no living child across three audits is the counter leak the old
  two-second bound existed for, and only that leaves early. A true
  daemon holds the process for the tree's lifetime; the harness limit is
  the outer bound, as timeout(1) would be natively (§10.21).

## 5. The wire protocol (vmrproto.h)

VMR5. The main channel: forwarded syscalls (number + registers, nothing
marshalled), CTXPAGE (bytes by address; COWBREAK/CLAIMING/ABSENT/
NOTHOLDER/GONE are the honest non-byte answers), LAYOUT (initial and
post-exec relayout — args[0]==1 asks for the calling context's own map),
WRITEPREP, INSTALLED (one-way ack), FINISH (the guest's real wait status
— home's log survives; an ssh session's may not), CHILD (a context comes
into being because the source asked; announced by host pid, registered
BEFORE anything can ask about it). The page channel (port+1): GET/GETS/
PUTN/COWBREAK/HELLO_RECALL, request struct `vmr_pgreq` — whose `len`
field, unused by every op but PUTN, carries the COWBREAK escalation flag.
A reply's page follows the header unconditionally (a refusal that sent
the header alone left the asker in read() until its socket bound fired).
`vmr_rsp.host_pid` names a clone's child in the init namespace; `mmseq`
carries the kernel's event-time stamps and `cur` the construction stamp.

## 6. The vDSO subsystem

The facts it stands on: the backend never writes tsc_offset (guest rdtsc
== destination host TSC), and the source cannot serve vvar OR vdso pages
by any userspace means (a probe reads all zeros; a static guest's libc
silently rejected the zero vdso and used syscalls — which is the accident
that hid the whole family, §10.18).

- Same kernel release on both machines (VMCTX_SRC_RELEASE, exported by
  local-here.sh, equals the destination's own uname): vmremote serves
  [vvar]/[vvar_vclock]/[vdso] faults from ITS OWN live pages — same
  release = the same kernel image = bit-identical vdso, symbol offsets
  included, which is what the first image's loader resolved against. The
  vvar DATA pages are re-poked 3×/s by a refresher thread (plain ctl —
  home_call is not thread-safe): a snapshot's coarse clocks FREEZE (the
  kernel's tick advances its own page, never a copy), and fc1's
  time()-bounded loop measured that as a 3/3 suite regression the day
  the vdso became real (§10.18). The refresher drops slots whose context
  died and whose region stopped being vvar-class (a guest may map over
  its vvar; the re-poke must not scribble on the new tenant).
- Differing releases (VMCTX_DEST_RELEASE from the cross harness): vmhome
  hides the vDSO from the FIRST image at the exec stop (AT_IGNORE) and
  drops [vdso]/[vvar*] from the first layout only — an exec'd image's
  fresh auxv is unpatched, so its regions must stay served or its libc
  jumps into an undescribed range. Clocks go by forwarded syscall:
  correct on any destination at a round trip per read. Exec'd-image
  clocks on a mismatched destination are a KNOWN GAP (the fix is the
  same patch at the guest's exec stop).
- tests/vd1.c gates both arms: every user-reachable clock against
  syscall(SYS_time) — the one answer that cannot come from the vDSO —
  plus time() must ADVANCE within ~2.5 s (the freeze check the
  instant-correctness asserts cannot make).
- Open (recorded): forwarded CPUTIME clocks report the parked source
  task (~0), not the guest's burn — gpg-agent's npth ticker schedules by
  CLOCK_THREAD_CPUTIME_ID, exits on the nonsense, and its timed event
  loop dies (§10.26). The fix is cputime virtualization in the monitor's
  accounting; the airtight vvar fix is mapping the live page instead of
  copying it ("map, don't copy") — both designed, neither built.

## 7. Bounds (every wait is bounded, and the bounds NEST)

Outermost: the kernel fault deadline 6000 ms. Inside it: the page GET
socket 4 s; inside that: the destination's pull_wait 500 ms + claim wait
1000 ms + take wait 1000 ms. TRANSIT heals at 100 ms
(vmctx_pgrec_transit_ms); the ghost-claim heal at 255 asks; the CLAIM
yield at 2001 faults throttles and persists (the deadline is the bound —
a blind own-copy fallback is the answer that can be wrong). A bound that
can outlast the thing waiting on it turns its own success into somebody
else's failure (the attach retry measured pthread_create EAGAIN 6-8 of
25 when its 2 s outlived the clone's 1 s answer). Liveness bounds
replaced clocks in three places (child WAIT engagement, ATTACH takeover,
main teardown drain): a clock cannot distinguish slow from dead; the
task's own existence can.

## 8. Exit codes and verdicts

- 97: an unforwardable syscall (forward failure is fatal — never run the
  program's calls on the wrong machine).
- 98: an unfetchable guest page (no invented pages; retained-copy serve
  is the last resort before it).
- 124: the harness watchdog (local-here's own clock; nothing of the
  harness is inside the measured wall).
- 131 = -ECANCELED: the fault watchdog ended a context (a page faulting
  forever with every answer counted as progress that never lands).
- 242 = -EFAULT: an unrecoverable guest fault (the kernel could not
  serve what the monitor declined).
- 251 = -EIO: an unserviced syscall or a context ended by its monitor's
  disappearance (should be extinct since the succession, §10.21).
- A forked process's deliberate exit status is ITS OWN: only a context
  that JOINED an address space (as_id != pid) is a thread whose death the
  run must report (§10.13).

## 9. The switch reference (complete; everything else was deleted)

Facts the harness feeds (never overridden):
- VMCTX_SRC_RELEASE — the source kernel's release; equality with the
  destination's own enables vvar/vdso serving (§6).
- VMCTX_DEST_RELEASE — the destination's release; inequality enables the
  first-image vDSO hiding (§6).
- VMCTX_PM_QOS_US — cpu idle exit latency bound while a monitor runs.

Performance tunables (session 46; each measured on one boot, arms
alternating, before its default was chosen — see HANDOFF.md §46):
- `vmctx_spin_us` (core_param, source kernel #181, default 100) — the
  kernel's spin-before-sleep budget at its four vmctx waits (the guest's
  reply wait in `vmctx_report`, `VMCTX_CTL_WAIT`, `VMCTX_CTL_SYSCALL`, and
  the service context's park in `vmctx_guest_step`); adaptive per site and
  per context (a wait past the budget disarms the next spin); 0 = sleep
  only (the control arm). `vmctx_spin_hits`/`_misses` are the evidence.
- VMCTX_SOCK_SPIN_US (both halves, default 50) — the userspace
  spin-before-read budget on every channel socket (MSG_DONTWAIT probes),
  same adaptivity; 0 = block only. Each half prints "socket reads that
  spun: N answered inside the spin, M ran out and slept".
- VMCTX_RA_MIN_RTT_US (both halves, default 120) — the measured-RTT
  threshold above which readahead (the chunk fetch / the in-call prefetch
  window) is used; 0 forces it on. Re-measured on the spinning path:
  still neutral on loopback (sha, py equal; gzip within noise), so the
  default stands.

Diagnostics, vmhome: VMHOME_PGLOG[=0xADDR|1] (+_OFF) per-page event log;
VMHOME_PGWATCH; VMHOME_CLOBBER (serve-out backward-move detector);
VMHOME_FUTEX_LOG; VMHOME_SIGPROBE (signal_generate tracepoint — finds a
signal's sender); VMHOME_TRACE_LAYOUT.

Diagnostics, vmremote: VMR_PGLOG (+_OFF), VMR_TRAIL_PAGE (per-page event
trail), VMR_WATCHWORD, VMR_HIST/VMR_HIST_ARENA (content history: which
hand-back gave an OLDER copy — value checks cannot see a lock going
0→1→2→0), VMR_CLOBBER (+VMR_CLOBBER_REVERT=1, the any-difference filter
mode of the same detector), VMR_TAKEPUT_PROBE, VMREMOTE_TRACE_FAULTS
(lifts the per-child fault print cap), VMREMOTE_CHILD_FAULTS,
VMREMOTE_CHUNK_DUMPS, VMREMOTE_TRACE_{CLONE,CONNECT,ABSENT,PUT,PAGE,
AFTER_EXC}, VMREMOTE_FAULT_STATE, VMREMOTE_CHECK_COHERENCE,
VMREMOTE_VERIFY_AT/VERIFY_FAULTS, VMREMOTE_WATCH/WATCH_WORD,
VMREMOTE_ORACLE_DUMP (what does the owner think is at this address, asked
at the one moment both copies can be named), VMREMOTE_SWEEP (exit-time
report of never-fetched pages), VMREMOTE_DEBUG, VMREMOTE_SYSLOG.

Deleted in the final review, each with its question settled:
VMREMOTE_FORK_SETTLE (the instrument that ruled the object copy out — the
492-page settle fixed nothing, three hypotheses dead), VMR_GEN_REPAIR
(the generation repair is unconditional), VMHOME_READAHEAD/VMR_READAHEAD
(the measured RTT decides), VMHOME_CTL_NR (the probe is the mechanism),
VMCTX_VVAR_EMUL and VMCTX_HIDE_VDSO (the release facts decide; forcing
either way serves wrong time or disables a proven fix).

## 10. The bug ledger (found, named, fixed — the mechanism of each)

1. **The lost store / coh_lock window**: the guest's ASID TLB was not
   synced with a page's departure; fix = mmu_notifier + kick, and
   tlb_ctl re-armed on generation change.
2. **The fabricated loan**: the recall channel's table was never
   registered into; page_keep/page_mark replaced the invented record.
   Rule: lent[] is advisory; SEEK_DATA and the pagemap are authority.
3. **Guests on the monitor's fs_base**: ws1's "lost stores" were another
   thread's; guest fs/gs are context-owned and shipped with the regs.
4. **pg3 one-behind**: a stale TLB translation surviving the take —
   VMR_PG_INFLIGHT (retained NEVER serves a live AS; the landing
   decides) + the hold rewrite.
5. **The shared zero page**: a shmem hole GUP'd by a monitor PEEK
   allocates the kernel's reserved zero folio — unclaimable, every take
   refused forever. MAPOBJ never faults; holes answer 0 bytes; ABSENT
   fills land ordinary pages.
6. **Retained zeros over live memory**: ABSENT during an in-flight pull
   retained the zeros and installed them on the sibling's answer —
   INFLIGHT is the answer while a pull is in flight.
7. **Monitor mm pollution**: a cross-task APPLYMAP SET landed in
   current->mm — the monitor's — and every later clone inherited the
   guest's mappings. Cross-task SET/PROT are refused in the kernel;
   monitor_mm_check stands as the invariant ("guest mappings found in
   the monitor's own address space: must be 0").
8. **The claim storm**: the source held PG_CLAIM over a page a sibling
   had landed; the INFLIGHT reply carries why + capture count and the
   claim window closed.
9. **The pristine-file serve**: "never handed over" is the CHILD's
   record talking — a fork copy's private file page may be the parent's
   DIVERGED copy (options parsed into .data before the clone); ask
   COWBREAK first, the file only after "nothing owed".
10. **The heap stale-serve family**: serve_object_mapped handed a stale
    glibc-arena folio; the per-(as,page) take-generation stamp
    (pgen_bump/pull_gen_check) detects and repairs from the retained
    copy — the repair is unconditional (§9).
11. **The abandoned birth**: WAIT's ESRCH/EINVAL for a just-cloned task
    were treated as death; a sandbox froze the whole tree with nothing
    in any log. Both are retried while the TASK is alive.
12. **The self-map vacate**: an mmap's own MAP_FIXED self-unmap arrived
    as a vacate ordered after the construction and punched it; source-
    side suppression (srccons) then the kernel's event-time stamps made
    the whole ordering airtight (§1.3).
13. **A fork child's exit code became the run's**: worst_ctx_status
    counted every context; only AS-joiners (threads) are the silent
    deaths the rule exists for.
14. **The pidns pid gap**: an assisted clone returns the NS-relative
    pid; the monitor addressed kthreadd. LASTCHILD records the init-ns
    pid; the guest's RAX keeps the namespace view.
15. **Drain-time vacate stamps**: two connections drain one per-mm log
    concurrently and the later stamp lands on the earlier event —
    stamps are assigned at the event, under the log's lock, in the
    kernel.
16. **The tie read backwards**: a construction's stamp equals the last
    event's seq; treating the tie as the vacate's win killed a fresh
    2 MiB row at birth. On overlap the tie is the construction's — the
    address allocation proves the order.
17. **The recycled hole's old bytes**: a skipped stale vacate preserved
    the previous tenant's bytes in the object; a fresh non-shared mmap's
    range is punched BEFORE the mapping goes live.
18. **The vDSO family**: three defects in one subsystem — the source
    cannot serve vvar (guests got zeros/frozen time; TLS said
    "certificate not yet valid"), the source cannot serve vdso TEXT
    either (static libcs rejected the zero image and hid the family by
    falling back), and a served snapshot's coarse clocks FREEZE (fc1's
    time()-bounded loop spun to the kill). Fixes: §6 (both arms + the
    refresher), gated by release facts.
19. **The fork-inheritance chain**: four builds of the child-page wipe
    each named a serve-chain gap — the pristine-file fallback (fixed by
    9), the RECURSIVE parent read (fixed by the pagemap-walked chain:
    the direct parent's page was wiped at ITS fork; kernel COW of a
    HOLE is the zero page — the very reason "let the child's kernel COW
    serve it" was refuted by its next corpse), and the strict give gate
    blind to grand-ancestor pages through empty-handed intermediates
    (fixed by the ESCALATED break, licensed only by the asker's proven
    failure). The wipe itself stays out: its remaining cost is the
    teardown order, not bytes (§11.6).
20. **The ghost claim**: the serve table's HOME-and-absent branch
    answered CLAIMING with no bound — "a local fill is landing" — while
    the fill it imagined was the guest fault its own refusals starved.
    169,826 refusals in one measured minute (gpg; netsurf's pre-paint
    stall is the same photograph). Healed by ask-count in the entry's
    sum field; 23 heals killed a 267k-fault storm to 4.6k.
21. **The lifecycle (three deaths, one design)**: the orphaned daemon
    (a forked context's monitor was the thread that SPAWNED it — the
    parent's service — dead an instant later; kernel: dead monitor ==
    missing monitor, events wait for a successor; userspace: the
    spawner detaches after the release and the child's own service
    attaches, so the guest enters exactly when its watcher is in
    place); the dying-context EIO family (sort's 251s — the same
    succession); and the two-second teardown abandon (main leaves with
    live children; now a liveness-bounded drain). nc1 9/9 first passes
    ever, sort 9/9 against 2/12, and lddnames 6/6 — the "argv-stale
    exec hole" four kernel builds chased was this clone-window race
    all along.
22. **Transits outliving their wire**: a serve's one-way ack died with
    a churned connection and the record held TRANSIT forever; per-
    connection tracking settles the orphans THEIRS at close.
23. **The dead-port dial loop**: a finished run held 85 s by 471
    connects to a port whose peer had exited; 20 consecutive refusals
    latch the port dead.
24. **The kill that would not stick**: ACT_KILL answered, the same
    fault re-arrived from WAIT forever; three polite answers then
    SIGKILL, with the claim settled before the break.
25. **Loans cleared on discard**: the first cut of the vacate-clears-
    loans rule ran on SOFT vacates too and fx1 refuted it at 1-in-6:
    a DONTNEED kills content, not relationships — the holder still
    answers for the page. Content dies soft; relationships die hard.
26. **The cputime domain** (open): forwarded CLOCK_*_CPUTIME reads
    report the parked source task; programs that schedule by their own
    cpu-time (npth) starve. Recorded with its corpse; the fix is
    accounting, not serving.
27. **The VMX base read after preempt** (Intel destination only):
    `vmx_run_usercode_once` read `GUEST_FS_BASE`/`GUEST_GS_BASE` back out
    of the VMCS *after* `vmx_enter_once` had already run `preempt_enable`
    — so a preemption in that window migrates the task and the read
    answers for whatever VMCS is current on the new CPU (a sibling
    context's, or none, which VMfails to 0). The wrong base was written
    into `vc->guest_fsbase` and fed to the guest on its next entry: a
    thread silently on another thread's TLS, the VMX twin of bug #3. The
    struct's own comment already said every VMCS field must be captured
    before release; these two (and the EPT diagnostic's
    `GUEST_PHYSICAL_ADDRESS`) were the exception. Fix = read them inside
    `vmx_enter_once` under `preempt_disable`, into `r->fs_base`/
    `r->gs_base`/`r->gpa`, and use those. SVM was always correct (it
    saves back from the VMCB, which is memory, not CPU-current state).

28. **perf.sh's native median was five walls glued together**: `dur()`
    prints no newline and its output was appended to `nat.<prog>` as one
    line, so the summary's "native" column read e.g. `0.0080.0080.007...`
    and its median was that string. Harness only; every quoted native wall
    before session 46 came from the per-run line, not the table. Fix =
    one wall per line. The table now also prints the governor and
    `vmctx_spin_us`, the two machine settings that decide the numbers
    (see #29's context in NOTES §5).

29. **A spawned context could enter before its own monitor was attached**
    (destination, `spawn_forked_context` / `child_thread`; found by pid1
    cross-machine once the paths got faster). Session 44's handoff was
    RESUME by the spawner, then DETACH, then the child's own service
    thread ATTACHes — argued safe "by order" because the entry gate wants a
    monitor AND the release, so a guest woken by the release would re-test
    the gate after the detach. That is a race on the guest's wake-up
    latency against the spawner's RESUME→DETACH gap, and session 46's
    faster paths lost it: the payload passed the gate on the spawner's
    release, the spawner detached, and its first instruction fetch was an
    unrecoverable #PF with no monitor (`could not take its monitor over
    (Invalid argument, task gone)`; the Intel dmesg names rip 0x41b1bd err
    0x14). Fix = the spawner sets registers and DETACHes without releasing;
    the child's service thread releases (RESUME) only after its ATTACH
    succeeded, so the gate cannot open until the monitor that will answer
    the first fault is the one attached. A child whose service never comes
    is ended by the kernel's 5 s gate timeout (or killed outright when the
    source refuses it a monitor).

30. **fc1's churn-thread 242: the lookup-then-punch race** (destination,
    `range_vacated` / `cons_note` / `declined_at`; session 46b, the
    session-46 open residual). `cons_lock` guarded the ring's WORDS, not
    the ORDER of two threads' compound operations: thread A (a vacate
    from the source's mm hook) did `range_vacated_seq`'s covering-row
    lookup, found no newer row, and went on to punch; thread B (the reply
    to the churn thread's mmap of the same 8 KiB) noted its row and
    applied its map in between. The punch then landed on the live mapping
    ("0 vacates kept off, N punched constructions REPAIRED"), and the
    REPAIR that exists for exactly this fell to its own livelock guard,
    keyed on the ADDRESS: an address fc1 recycles every round reached the
    guard's fourth repair and was declined to a kernel with no VMA
    (`unrecoverable #PF at 0x7ffff77f2000 err 0x6`, -14). Two fixes, one
    build: `layout_order_lock` is held across the whole lookup-and-punch
    in `range_vacated` and across `cons_note`'s note (so a vacate either
    sees the row and is skipped VACATE-LATE, or lands before the row
    exists and the map re-applies over it); the repair guard is keyed on
    the construction row's seq (`cons_covering` returns it), so a fresh
    row at a recycled address starts a fresh budget. VACATE-LATE now
    fires 13-37 times per fc1 run where it fired 0; fc1 8/8, then 6/6,
    then 6/6 on the final build. This closes the session-46 residual and
    the "ordering edge" the cons/REPAIR instruments had been reporting
    since #16/#17.

31. **A copy-on-write give crossing the copy's exec** (destination,
    `exec_wipe` / `cow_give_page_mode`; progs pipes' exit-98 shape, "guest
    executing zeros at 0x555555577840", tar's text page holding dash's
    heap bytes). `exec_wipe` unmapped the members and punched the copy's
    object FIRST and marked its lineage LAST; a parent's write-prep give
    that raced the exec wrote the pre-exec image's page into the object
    after the punch and before the mark, and the new image fetched it as
    its own text (an `add %al,(%rax)` sled, then SIGILL/242 or the FATAL
    98 when the source found the page on neither machine). Fix: the mark
    is set at the top of `exec_wipe`, and `cow_give_page_mode` re-checks
    the mark under `lineage_lock` AFTER its `obj_write` — a give that
    crossed the exec is undone (`obj_forget`) and reported ("crossed its
    exec: undone"). pipes 3/6 → 9/10 on this fix alone.

32. **A fork copy's first ask racing its parent's give** (destination page
    service, `VMR_PG_GET` and `VMR_PG_COWBREAK`; progs pipes' remaining
    ~1/10, the dash subshell's segmentation fault at 0x18 in
    `_int_malloc`). The destination's own fault path has given a copy its
    inherited page from the COW chain since session 42 (PULL_COWBREAK);
    the PAGE SERVICE — the route a copy's SOURCE-side fault takes (the
    source's kernel finds the page absent in the copy's mm, vmhome asks
    GET) — did not: it checked the copy's own contexts and object and
    answered ABSENT while the parent held the page under the fork's
    protection a few lines away. The source's own routes then RACED the
    parent's write-prep give: its strict COWBREAK ask arrived after the
    give had landed in the copy's object and was refused "nothing owed:
    already given 1, copy's object has it 1" — a refusal that is wrong for
    every reason on its list, because the copy HAS the page and nobody
    serves it. The source then zero-filled (an anon page: the child's TLS
    at 0x7ffff7fa0000, measured on the exec'ing children) or served the
    private FILE page from the pristine file (libc's `.data` with
    main_arena: every bin NULL in the file image, initialised only at
    run time — `victim = bin->bk = 0`, `bck = victim->bk` at 0x18, the
    corpse). Two fixes: (a) the GET path, for a copy with nothing of its
    own (after the under-lock re-check), calls `cow_give_from_chain`
    (strict — an ancestor without the protect mark is the source's call
    to escalate) and serves the given page; (b) a COWBREAK whose copy
    already holds the page is re-dispatched as the GET it would have
    been a moment later (`goto cow_own_get`), so the take, the loan
    record and the retained copy happen in the one place they are
    defined. Counted in the end-of-run summary ("GET(s) for a fork copy
    answered by giving from its chain on the spot ... break request(s)
    answered with the copy's OWN page").

Open residuals (documented, no fix landed): the lifecycle families named
in REDESIGN ("in-flight state when a counterpart leaves") — lddnames'
low-rate empty stream, nodework's thread-list class (session 37), gpgsym's
parked-clock npth wait (§10.26) — and the three KNOWN_GAPS in progs.sh.

## 11. The corner-case catalog (measured shapes, whether or not they broke)

1. **Guest stack growth** into an unmapped guard region — the fault is
   real and the region model must not answer it (131/242 discriminate).
2. **A VMA is not permission**: ABSENT vs EFAULT is the perms column;
   PROT_NONE is a real answer (a guard page mapped readable let a stack
   overflow scribble instead of dying).
3. **Threads are separate processes here**: exit_group ends only the
   context that issued it; "the guest's own exit ends every context" is
   true on the source and false on the destination, and every teardown
   rule must hold on both.
4. **The fork snapshot can be destroyed**: once the parent's page is
   taken to the source, source-side mutation is in-place and invisible
   to the give-first machinery; parent-current bytes are what remains
   (identical while undiverged). True fork-snapshot semantics for taken
   pages is REDESIGN territory.
5. **Recycled ranges everywhere**: thread stacks, fontconfig's mmap
   holes, malloc arenas — every record keyed by address must be cleared
   or replaced at the vacate, or the new tenant inherits the old one's
   story (rows replace; loans clear hard; generations forget; retained
   copies punch).
6. **The wipe ledger**: #169 (all pages) died on loader-diverged file
   pages served pristine; #170 (anon-only) wedged in the clone window;
   #173 died on the recursive parent read; #175 on the strict gate; and
   with all four causes fixed, #177 measured the remaining cost as
   teardown order under longer child lifetimes — the wipe rides on the
   lifecycle, and the argv-stale hole it chased died with the clone-
   window ordering instead (§10.21).
7. **Double-fork daemons**: the parent exits by design at the worst
   moment; every lifetime tied to the parent's anything is wrong. The
   tree, not the first context, is the run.
8. **ptrace inside a guest**: forwards mechanically (SEIZE/GETREGSET
   answer against the parked source task) and means nothing
   semantically — the traced task never RUNS there. gdb's inferior runs
   to completion untraced; strace records an empty summary. A design
   question, not a bug: what should ptrace of a parked task mean?
9. **Instruments mask**: an armed page log once manufactured the very
   state it reported on the take path; pg3 passes 12/12 alone and fails
   only beside concurrent pairs. Never quote an armed run's numbers as
   the system's.
10. **Ports**: a listen port inside the ephemeral range loses to an
    outgoing connection's source port; shared-mode runs each own a
    fresh pair below 32768. A fixed test port once faked failures.
11. **uutils coreutils on the box**: timeout/env/date are a Rust
    multicall binary costing ~10 ms per exec and 100 ms poll steps —
    keep them out of every measured wall.
12. **/proc/maps is chunked**: one read() returns the first chunk and
    silently drops [stack]; read to EOF or the guest has no stack.
13. **Binary logs**: home.log carries NULs (grep needs -a); vmremote's
    stderr is remote.log; the guest's own stderr is guest.err, split by
    the [vmhome]-prefix rule.

## 12. Invariants (the checklist)

- One writable copy of any page, ever; write-protect-driven coherence.
- No invented pages: an unfetchable page is fatal (98); zeros only for
  a first touch nobody has ever written, said out loud.
- The record first, the kernel second; the kernel is the only authority
  on address validity (TRYFAULT), never /proc text.
- No address-specific code; no syscall-specific fixes (memory problems
  get memory-level fixes). The vDSO's page-specific serving is the one
  owner-granted exception, justified by "the destination may be a
  different OS entirely".
- Ownership is taken, not inferred; a stale record is not evidence.
- Every wait bounded, bounds nested, liveness over clocks where death
  is what the bound detects.
- A claim, a transit, a loan must not outlive the wire/service/range
  that made it.
- A guest enters only when its own service is watching; the tree is the
  service lifetime; a dead monitor is a missing monitor.
- Exceptions route to the owner, raw. Forward failure is fatal.
- Instruments must be provable-able-to-fire, capped, and never trusted
  while armed; a confident zero usually means the check never ran.
- No optional correctness: an A/B arm is deleted the day its question
  settles, and the numbers move to this file.
