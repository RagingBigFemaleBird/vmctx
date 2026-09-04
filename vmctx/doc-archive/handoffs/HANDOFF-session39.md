# HANDOFF — session 39 (2026-08-23): the page record moved into the kernel

Follows `HANDOFF-session38.md`. The user's directive, after session 38's
performance campaign:

> *"Continue to optimize. Reading /proc/<pid>/maps is not very efficient. I'm
> feeling whether you need to move the page tracking into kernel itself, or
> find some other way. consulting multiple places and costing tens of
> microseconds on this is not OK. Read through entire code and design
> properly. Don't be afraid to make big changes."*

So the whole page-ownership machinery was moved into the kernel, beside the
page tables, and the userspace table and everything built to reconcile it
were deleted. This is the "big change" the owner licensed; it is also the
architecture the redesign campaign (sessions 29–37, `REDESIGN.md`) kept
circling — "ownership consulted at a memory read, a transition that blocks
on a futex" — finally put where the facts actually live.

## The problem, precisely (session 38's own measurements)

Every page a guest faults on is a hand-over, and each hand-over asked the
kernel the same three questions through three separate, non-atomic
interfaces: the mapping (`/proc/<pid>/maps` text), presence
(`/proc/<pid>/pagemap`), and the bytes (a PEEK). Session 38 had already cut
the maps parsing to a per-change snapshot, but the *serve path itself* still
did, per page: a pagemap read before the take, a `VMCTX_CTL_TAKE` loop, a
pagemap read after it (was the page still here — a sibling's landing between
the zap and the check), sometimes a peek, sometimes a re-take loop. Five
syscalls where the answer wanted one, and ~1500 lines of `ctx_page_inner()`
reconciling a userspace `pgown` table with a page table that moved under it.
The reconciliation is what all the retry ladders were: the record and the
kernel could disagree because nothing made them one thing.

## The redesign: one record, in the kernel, under one lock (kernel #154/#155)

**The record.** Each `vmctx_mm_slot` (already the per-address-space table the
mm hooks key on) gains a page record: an xarray, one entry per page that has
ever changed hands or been claimed, packed into the xarray value
(`state | sum | gen | stamp`). Kept only for a SERVICE context's address
space. Erased where the kernel releases the memory (the vacate hook), so
exec/munmap/DONTNEED need no userspace forget.

States: `NONE` (untouched), `HOME` (this side holds it), `REMOTE` (the
machine running the program holds it), `CLAIM` (a source fault is fetching
it), `TRANSIT` (handed over, INSTALLED ack pending).

**The five operations** (uapi ops 22–27):
- `VMCTX_CTL_SERVE` — the destination's "give me this page", answered in one
  step under `mmap_read_lock`: look the mapping up (grow a stack hole as the
  fault path would), classify it, consult the record, and by class copy the
  page (read-only, or a writable MAP_SHARED file: both machines may hold it)
  or TAKE it (writable private: record `TRANSIT` *before* the zap so nothing
  moves it in between, then zap+claim+copy). Answers
  TAKEN/COPIED/ABSENT/CLAIMING/DENIED/NOMAP. `VMCTX_SERVE_PROBE` reports the
  record + class + presence and moves nothing (the fork logic's question).
- `VMCTX_CTL_LAND` — install a page fetched from the other machine and record
  `HOME`, one step (POKE + record, stamped with the sender's generation);
  `IF_ABSENT` refuses when a page is already mapped there (a copy that landed
  by another path beats arriving bytes).
- `VMCTX_CTL_PGACK` — settle the two-phase hand-over: `TRANSIT`→`REMOTE` when
  the destination's `VMR_OP_INSTALLED` echoes the sum SERVE stamped.
- `VMCTX_CTL_PGSCAN` — the pages of a range whose record is in a state mask
  (the shared-file reclaim, instead of walking maps text).
- `VMCTX_CTL_PGSTATE` / `VMCTX_CTL_PGSET` — read / write one entry.

**The claim is the kernel's.** `vmctx_anon_fault` CLAIMs the faulting page in
the record *before* `vmctx_report` tells the monitor. From that instant every
`SERVE` of the page answers CLAIMING and every *sibling context's* fault on
it blocks in the kernel on the entry's waitqueue (`pg_wq`) — the retry path
waits there off every lock (the non-retry path, which holds `mmap_read_lock`,
reports the prior state so the monitor waits instead, avoiding the ws1
three-way deadlock). The monitor's answer releases it: `LAND`/POKE records
`HOME` and wakes; SELF records `HOME` (the local kernel's fill is the
answer); KILL/timeout puts it back. This is what the whole REDESIGN campaign
wanted the shared-memory segment to be — a real landing to wait *for* —
built where the page tables are instead of in a mapped memfd, so it is one
record and not two reconciled over the wire.

**vmhome shrank from ~9.7k to ~6.8k lines.** Deleted: `pgown_tab` (the
2^18-slot hash) and `pgown_lock`, the ownership `pgown_sweeper`, the `strail`
per-page trail, the `infl[]`/`infl_lock` in-flight table, the
`ctx_vmas_*`/`ctx_movable_ranges`/`ctx_addr_*` `/proc/maps` snapshot (session
38's optimisation, now unneeded), `theirs_note`, and the serve-path retry
ladder. `pg_state`/`pg_gen`/`pg_set`/`ctx_land`/`pg_scan` are thin wrappers
over the ctls. `ctx_page_inner()` is a loop over `VMCTX_CTL_SERVE`;
`ctx_monitor()` is a claim (kernel) + GET + `ctx_land`.

**Two protocol/latency follow-ups** rode with it:
- `VMR_OP_INSTALLED` is one-way (magic bumped VMR3→**VMR4**): the source
  settles `TRANSIT`→`REMOTE` the instant it reads the ack (`VMCTX_CTL_PGACK`)
  and nothing waits, so the ack no longer owes a reply (session 38's
  `home_owed`/`home_drain` deleted). The ack is sent for exactly the pages the
  source TOOK and skipped for copied ones -- the CTXPAGE reply carries a
  per-page taken bitmask in `map_off`, which `home_call` records with the
  reply's base for `ack_installed` to consult (see "the ack-skip trap").
- The source writes no per-request log line for a page (two syscalls off the
  hottest path) and pairs the reply header + payload into one `writev`.
- The kernel skips re-asking the monitor on the retry of a fault it already
  answered SELF (the record reads `HOME` on the TRIED pass — the local fill
  is the answer): `vmctx_af_home_local`.

## Results

**Gates (chain = suite ×1 + 48×8 pools of hx2/pg3/ws1/sig1, graded by
`guest.out`):**
- chain **s39a** (kernel #154, first build): suite **34/0**; pg3 **48/48**,
  ws1 **48/48**, sig1 **48/48**; hx2 **41/48** (then 40/48, 36/48 on two more
  pools — ~81%, below the ~94% baseline).
- The hx2 shortfall was a real regression the redesign introduced: a source
  fault could steal a page mid-SERVE (the record briefly `TRANSIT` before the
  take; the fault CASed `TRANSIT`→`CLAIM` and started a competing fetch —
  two transitions on one page). Fixed in **#155**: a fault WAITS on a
  `TRANSIT` page (it is leaving; wait for the hand-over to settle) instead of
  claiming it, and a serve wakes waiters on any non-TRANSIT outcome.
- chain **s39b** (kernel #155): suite **34/0**; pg3 **48/48**, ws1 **48/48**,
  sig1 **48/48**; hx2 **43/48** (up from 41). The record's own counters over
  the whole chain: **claims 5.35M, waits 254k, wait_timeouts 285, acks 5.35M
  with ack_stale 0, erased 433, nomem 0** — millions of hand-overs, every one
  coherent.
- chain **s39c** (kernel #155 + ack **always-send**): suite **33/1** (mf2),
  hx2 38/48, ws1 47/48, pg3/sig1 48/48, and `ack_stale=19956`, `take_lost=1`
  -- always-send's ~20k copied-page acks are noise that (via a sum-collision
  with a live TRANSIT) can wrongly settle. Superseded by the exactly-taken
  ack below; mf2 then passed 6/6 standalone.
- chain **s39d** (kernel #155 + ack **exactly-taken**, the final build):
  suite **34/0** (mf2 passes), pg3 **48/48**, sig1 **48/48**, ws1 **47/48**
  (one flaky miss), hx2 **37/48**. Record counters over the chain: claims
  10.2M, waits 389k, wait_timeouts **7**, acks 10.2M, **ack_stale +1** (vs
  always-send's +19957 -- the exactly-taken rule works), erased 834, nomem 0,
  `takeobj_stale_store` **0**, `take_lost` **1** (the rare post-zap class,
  1 in ~10M hand-overs, recovered). getppid ~33 us, wc/links2 faults ~55 us.

**Honest status of hx2.** Across s39a-d the hx2 torture pool ran 41/40/36 (#154),
43 (#155 ack-skip), 38 (always-send), 37 (exactly-taken) of 48 -- ~40/48
(~83%), modestly BELOW session-38's 48/48 on a fresh box. The deaths are the
session-37 deep-teardown thread-list class (a dying context's stack page a
joiner still needs; `err 0x7` write fault -> fatal SIGSEGV at
`__nptl_deallocate_stack`), not a coherence break (take_lost/stale_store ~0,
suite 34/0, mf2 6/6, pg3/sig1 48/48). The redesign BUILT the substrate
session 37 said this needs (REDESIGN step 3: the record knows which context
holds/claims a page, so a dead holder is now nameable) but does not yet act
on it: a dying context's CLAIM/TRANSIT entries are not cleaned, so a sibling
fault waits out the bound and runs ungated. That dead-holder policy is the
clear NEXT (below). The box was also heavily loaded all session (6 chains +
kernel builds); a fresh-box re-measure is worth doing before quantifying the
regression precisely.

`take_lost`, `takeobj_stale_store`, `mm_overflow` all **0** throughout. The
record's own counters (`/sys/module/kernel/parameters/vmctx_pgrec_*`) read
**claims == lands == acks**, with `erased`/`nomem`/`ack_stale`/`healed` 0 and
`wait_timeouts` a handful in ~90k claims.

**The performance win is the syscall count per page** — the deterministic
evidence session 38's trap note named, because this box's idle states move a
single wall-clock run by ±50%. The old SERVE made up to five syscalls a page;
`VMCTX_CTL_SERVE` makes one. The fault-service pull folds its claim into the
report the fault already makes and its install into `VMCTX_CTL_LAND`, one
syscall where the old path did a pagemap read + a POKE + several table
updates. A forwarded getppid on the final build is **~40-50 µs/call**, in
line with the base pair's ~37-89 µs — the redesign is not slower on the
syscall path — and faults are ~55-65 µs. And `perf.sh` (final build, medians
of 3) shows the per-fault cost — the campaign's target — 2-3× BELOW
session-38's already-optimised final, because a fault is now one
`VMCTX_CTL_SERVE`/`VMCTX_CTL_LAND` instead of a pagemap-read + take-loop +
reconciliation:

| prog | µs/fault s38 → s39 | µs/syscall s38 → s39 | fwd wall s38 → s39 |
|---|---|---|---|
| wc     |  58 → **39** | 229 → 125 | 0.24 → 0.29 (setup-bound) |
| curl   | 100 → **41** | 160 → 124 | 0.36 → 0.39 |
| sha    | 107 → **51** | 579 → 495 | 0.44 → 0.41 |
| python | 120 → **63** | 263 → 220 | 0.75 → 0.58 |
| gzip   | 138 → **69** | 1468 → 889 | 2.46 → **1.41** |
| links2 | 155 → **50** | 196 → 163 | 0.47 → **0.39** |

(wc's wall is fixed-setup-bound — its faults are 0.02 s of its 0.29 s; the
paging/compute-heavy programs are clearly faster. The I/O-buffer bounce —
session 38's standing next lever — is still the per-syscall tail on sha and
gzip, now 495/889 µs, down from 579/1468.)

### The ack-skip trap that cost most of this session

A near-shipped optimization — skipping the `VMR_OP_INSTALLED` ack for pages
the source merely COPIED — was WRONG and produced a spectacular
false-regression: forwarded syscalls measured **4 ms** (getppid) to **520 ms**
(wc), 40-500× the baseline, while faults stayed fast. The chain of causes,
worth remembering because each step was a plausible dead end:
- The ack-skip keyed on "was this page taken", a bit only the fault path set
  (`pull_last_taken`); a page served TAKEN via the CHUNK/region-prefill path
  had the bit clear, so its ack was **skipped**. Startup pulls most pages via
  chunks, so many taken pages **never got their INSTALLED ack** and stayed
  `TRANSIT` on the source's record.
- On kernel #154 that was invisible (a fault waited only on `CLAIM`). #155
  added `TRANSIT` to the wait (a fault must not steal a page mid-hand-over) —
  so a later *source* service-context fault on a stuck-`TRANSIT` page waited
  the full **6 s fault deadline**, then the userspace sibling-wait added ~2 s.
- And because a service context stuck in a page fault cannot run **any**
  forwarded syscall, that one 8 s stall serialized the *whole run* — which
  presented as "every forwarded syscall is 4 ms", inversely scaled with
  syscall count (wc's 48 syscalls looked worse per-call than vmbench's 2018).
- Traps that wasted time inside this: the box's `localsearch` indexer
  churning the pool output dirs (a red herring, ~19% CPU); assuming C-states
  (PM QoS on/off and 4 spinners both made **no** difference — it was a
  scheduler wake waiting a tick, not idle latency); assuming accumulated
  kernel state (a fresh reboot reproduced it); `strace` distorting the
  timing-sensitive take-retry (perf-trace attach extends syscall holds and
  trips SERVE's 1 s retry — never measure these paths under a tracer).
- The fix went through two forms. First `ack_installed` **always sent** (the
  source's `VMCTX_CTL_PGACK` is sum-guarded, so a copied-page ack is dropped
  as ack_stale): getppid returned to ~40 µs, but the pool then showed ~20k
  stale acks with an mf2 fail and take_lost=1 (a stale ack sum-colliding with
  a genuine TRANSIT is a wrong settle). The final form acks **exactly the
  taken pages** -- the CTXPAGE reply carries a per-page taken bitmask
  (`map_off`) that `home_call` records with the reply's base, and
  `ack_installed` sends iff this page's bit is set (and "when unsure, send",
  so a taken page is never wrongly skipped). getppid ~33 µs, 15 acks sent
  vs 84 copied skipped on a wc run, mf2 6/6.

The lesson (a PRINCIPLES-shaped one): an optimization that *omits* a
coherence message must be certain the omitted case is truly redundant on
**every** path that sends it, not just the one path in view -- and the safe
default when a fast local test cannot classify the case is to send, not skip.

## The zero-folio door is intact

Kernel #154/#155 are built on #153's tree, so `vmctx_shmem_may_alloc`
(session 36, the hx2 zero-folio door) is still there and still closed:
`shm_foreign_read` refusals fire, `takeobj_zero_named` accounts every
all-zero capture, `mapobj_holes` 0. The hx2 residual that remains is the
session-37 deep-teardown thread-list class (a thread stack page faulting
`err 0x7` → fatal SIGSEGV at `__nptl_deallocate_stack`), amplified by the
8-way pool; it needs REDESIGN step 3's dead-holder handling, not a point fix.

## Deployment state

- Box `.229`: kernel **#155** (`build-155.sh`; backup `~/kernel-153-backup/`
  holds the #153 vmctx.o and vmlinuz), clocksource `tsc`, module + both
  halves match the tree (`verify-build.sh amd` green), GRUB backup
  `grub.bak-s38` from session 38 still stands. The home tree's
  `src/linux-7.0.14/kernel/vmctx.c` matches the box's `~/vmctx-build` copy and
  the running #155 (md5 verified). A `vmctx_pgrec_wait_ms` param (cap the
  sibling-wait at 1 s instead of the 6 s deadline) was written and reverted:
  the always-send ack fix removed the stuck-TRANSIT that made the 6 s wait
  reachable, so it was not worth a rebuild; it is the obvious knob if a
  dead-claimer teardown wait ever needs capping (NEXT).
- Uncommitted per convention (commit only on request): `user/vmhome.c`,
  `user/vmremote.c`, `user/vmrproto.h`, `src/linux-7.0.14/kernel/vmctx.c`,
  `src/linux-7.0.14/include/linux/vmctx.h`,
  `src/linux-7.0.14/include/uapi/linux/vmctx.h`, `NOTES.md`,
  `ARCHITECTURE.md`, plus this file. (`user/own.h`, `NOTES/ARCHITECTURE` and
  the session-38 handoff/tests were already modified-uncommitted at the start
  of this session.) The kernel changes are the record's whole implementation
  (#154 the record + SERVE/LAND/PGACK/PGSCAN/PGSTATE/PGSET + the claim; #155
  the TRANSIT-wait) and live ONLY in `src/linux-7.0.14/` (untracked scratch,
  per [[kernel-patches-are-the-only-record]]) and on the box's `~/vmctx-build`
  tree -- **they want a `kernel-patches/linux-7.0.14/0073-*.patch` before the
  scratch tree is wiped.** When committing, squash the userspace per the
  author/committer convention and add 0073 for the kernel half.

## Traps met this session

- **A 0-byte binary from a racing rebuild.** `sync-amd.sh`'s build step and a
  separately-issued `local-here.sh` both touched `build/vmhome` and left it
  0 bytes; `--build-id` returned empty and the module looked unloaded.
  Rebuild with `rm -f build/vmhome build/vmremote-local && ./tests/suite.sh 0`
  and re-`verify-build.sh`. Never run a second build/run against the box
  while `sync-amd.sh` is mid-flight (the one-machine-operation rule).
- **Kernel counter forward-reference.** `vmctx_mm_live` is read by
  `vmctx_pgrec_slot`, which is defined above the counter's old spot; the
  build failed with "undeclared". The declaration was hoisted above the
  record block.
- The `ScheduleWakeup`/short-sleep chaining is blocked by the harness; poll a
  long-running box job with a single `run_in_background` loop instead.

## NEXT

- The hx2 residual is the teardown thread-list coherence (REDESIGN step 3):
  a dying context's pages a joiner still needs. The kernel record makes this
  cleaner to express now — a `REMOTE`/`TRANSIT` entry whose holder task is
  dead is exactly the "dead token" the retained-copy net was for, and the
  record knows the holder. Worth a dedicated pass.
- The I/O-buffer bounce (session 38's standing next lever): a forwarded
  read/write of a 32 KiB buffer still costs one page-channel GET per buffer
  page. A pipelined multi-page GET on the source's fault service; gate with
  the chain.
- The Intel port (.30) is still at #54 and does not have the record; a
  cross-box run needs the port carried forward (0055–0072 + the #154/#155
  record).
