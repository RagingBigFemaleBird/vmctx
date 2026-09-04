# NOTES — what has been done, and what to avoid

This is the project's memory: the campaigns in order, every design that was
built and refuted (with the numbers that killed it), the measurements that
turned out to be void, and the operational rules that were each paid for with
a lost session. ARCHITECTURE.md says what the system is; AUDIT.md says how
each piece works; this file says how it got there and which paths are dead.

Rules that govern everything here are in PRINCIPLES.md. The two that shape
this file: a switch is deleted once its change is proved (the numbers stay in
a comment; the branch does not), and an instrument must be proved able to
fire before its zero is believed.

Code comments citing section-numbered documents ("AUDIT XI §5.1",
"ARCHITECTURE §22") refer to the pre-2026-08-19 documents, which these three
replaced; resolve them from git history (`git show 3f27f5b:vmctx/AUDIT.md`,
`git show 3f27f5b:vmctx/ARCHITECTURE.md`, `git show 3f27f5b:vmctx/README.md`,
`git show 3f27f5b:vmctx/HANDOFF.md`). The findings themselves are summarized
in §2–§4 below.

## 0. State as of 2026-08-25 (sessions 43-44; the definitive summary)

Everything below §1 is history that led here. The current system:

- **Gates** (session 46b, on the fast path, srcid 125c8bc4b2ea): suite
  loopback x3 **120/0**, tests/progs.sh **69/0** (+3 known gaps), cross
  **40/0**; the browser demo GREEN (links2 calibrated, session
  44). Session 46b closed the two residuals session 46 left: fc1's
  churn-thread 242 (AUDIT #30, the lookup-then-punch race under
  `layout_order_lock`) and pipes (#31 a COW give crossing the copy's
  exec; #32 a fork copy's first ask racing the parent's give -- the page
  service now gives from the COW chain, and a COWBREAK for a page the
  copy already holds is served as the GET it would have been).
  Known gaps, each with a named mechanism and a cheap reproducer:
  gpgsym (forwarded cputime clocks read the parked source task -- npth
  starves; AUDIT §10.26), gdbbt/stracec (ptrace-in-a-guest is mechanical,
  not semantic; AUDIT §11.8), nodework ~1/6 (the deep join-coherence
  class, REDESIGN step 3), lddnames' low-rate empty-stream residual.
- **Performance** (kernel #181, session 46, HANDOFF.md §46):
  compute 99.6% loopback / 98.5% cross; forwarded syscall **16 µs**
  loopback (61 at #180; 66-80 in the #168 era; 482 at #60) / ~220 µs
  cross; fault 29-74 µs loopback / ~390 cross. The session's three
  changes -- spin-before-sleep at the kernel's four vmctx waits
  (`vmctx_spin_us`), spin-before-read on every channel socket
  (VMCTX_SOCK_SPIN_US), and one segment per request (writev) -- are
  all about the wakeups, which were the whole loopback cost; the
  frequency governor was the rest (NOTES §5 Measuring).
- **The big designs since the 2026-08-23 state**: the vDSO subsystem
  (dest serves its own vvar+vdso on release match, 3x/s refresher;
  first-image hiding at the exec stop on mismatch; vd1 gates both), the
  LIFECYCLE (tree = service lifetime, monitor succession + handoff,
  claims/transits/loans die with what made them -- three failure classes
  dead at once), the GHOST-CLAIM heal (the serve table's last eternal
  answer, ask-counted), the escalated COW break + the two source-side
  chain walks (the fork-inheritance chain complete), and the switch
  purge (seven A/B arms deleted with their settled questions; the
  complete reference is AUDIT §9).
- **The docs**: RUNNING.md (operator's guide) / ARCHITECTURE.md (map,
  §10-12 new) / AUDIT.md (redone: flows, corner cases, the full bug
  ledger §10, the switch reference §9) / HISTORY.md (the arc in ten
  campaigns) / PITFALLS.md (the trap catalog) / HANDOFF.md §43
  (sessions 43-44 in full detail).

## 1. State as of 2026-08-23

- **Session 41b (HANDOFF.md §41, second half) -- clear-tid
  redundancy dead, the sharing census built, browsers measured.** The
  service role's in-band CLONE_CHILD_CLEARTID clear (kernel #161, 0079)
  was redundant with vmhome's teardown clear that always precedes the
  assisted exit; suppressing it removed a per-thread-exit descriptor-page
  bounce and the rare 6 s stall (fault_deadline_hits 0 across the gate
  chain; flip back via vmctx_cleartid_inband_on). Kernels #162/#163
  (0080/0081) add the OWNER-RULE census: every fault on a context's mm by
  a foreign task is split supplier-vs-third (vmctx_f_supplier/f_third,
  third parties NAMED in dmesg), and VM_SHARED mappings a context maps
  are registered so a foreign mm's touch on the same object counts
  (vmctx_f_sharedobj_foreign). First numbers: pgrep/ps read-faulting a
  context's cmdline pages INVENTS zero pages into its mm (hundreds per
  harness run -- the measured form of "any task using ANY part of a
  context's mm must be handled"); the redirect-or-refuse fix is designed,
  not yet built. Browsers: links2 -g renders loopback AND cross (+253
  colours, 0 unrecoverable faults); netsurf still stalls pre-paint in
  glycin's bwrap sandbox (namespace axis, not mm-sharing -- the census
  is clean there).

- **Session 41 (HANDOFF.md §41) -- two corner-flake tests built, and
  each found a wall.** `tests/io1` (forwarded-I/O buffer oracle: a stale
  page NAMES its source block; IO1_SELFTEST proves the instrument) guards
  the "never land into a RUNNING context" rule; `tests/pf5` (75 fatal fork
  children, death BY the signal required) amplified two defects out of
  hiding. (1) The **COWBREAK refusal was the one GET-shaped reply without a
  page body** -- pg_get_op reads header+4096 unconditionally, so every
  fatal fork child's disposition read blocked the full 4 s socket bound on
  the scratch page (fresh in the child, never the parent's): pf3 12.5 s ->
  0.40 s once the refusal padded with zeros. (2) The destination's
  `ctxs[]` (MAX_CTX) filled at the 65th context of one run and
  `ctx_claim()` abandoned every later child unserviced -> 64 -> 256;
  recycling rows at service end was tried and REVERTED (dead rows route
  late asks into "AS ended, stop"; a forgotten context's channel is
  DROPPED -- 51k reconnect storm). The assisted-getpid "prompt death"
  nudge measured 3x WORSE and is gone. The SLOW CTXPAGE
  busy-wait fell too (kernel #160, patch 0078): the 8x1.07 s startup
  stall was a DIRTY large page-cache folio of the freshly linked binary
  refusing to split (dirty buffers refuse) until the flusher's timer ran
  -- only the first run after a deploy ever stalled. The take path now
  kicks filemap_flush() on the refused mapping (ratelimited); A/B: the
  refusals shrink from 8 s to ~11 ms, vmctx_take_split_flushed counts the
  kicks, and ctx_serve_one's give-up now prints the take/hold counter
  DELTAS across its own busy loop -- the instrument that named the branch.
  Harness rules: the suite runs
  under sudo, and runany.sh now refuses an output dir it cannot claim
  rather than letting stale outputs grade a run that never happened.
- **Session 40 (HANDOFF.md §40) -- hx2's pool death on #155 was a
  MISJUDGED EXCEPTION, not a coherence break; all gates green.** The
  directive: continue the optimization and make all tests green. hx2 stood
  at 37-43/48 after session 39. Traced end to end with the photograph
  instruments (the task's /proc state at "fault service ends", the service
  process's wait status, and the `signal:signal_generate` tracepoint, which
  names a signal's SENDER): every failing run began with a writer's
  ordinary store to the hx2 page arriving on the destination as a
  present-page write fault (err 0x7) with no owner, no watch and no COW
  mark -- a hand-over's PROTECTOBJ write-protects the folio in EVERY
  sibling context's page table, the sibling that pulls the page back
  re-grants only its own mapping (MAPOBJ) and spends the per-(as,page)
  watch, so the next sibling's store is a STALE protection. The dest's
  fault path had no branch for it and handed it to the owner as an
  exception; the source's `ctx_deliver_exception` called TRYFAULT only to
  choose ACCERR vs MAPERR, mapped its **0 ("the kernel would handle it")**
  to ACCERR and `kill(SIGSEGV)`ed the service process -- against the uapi
  rule written on TRYFAULT itself. SIGSEGV is a core-dump signal and the
  service contexts park in killable (SIGKILL-only) waits, so it sat
  PENDING for up to 12 s and landed when main's nanosleep returned at the
  20 s mark: the dump's zap ended every context at once (state D in
  `__synchronize_srcu`, `t->vmctx` NULL), every forwarded teardown call
  failed -EINVAL, the clear-tid pokes failed, and `pull_lost` dropped the
  thread-descriptor pages (the session-37 "deep teardown" reading was this
  defect's downstream). 76 of 79 failing runs over five pools carried the
  early `becomes signal 11` line. **Fix: `VMR_EXC_LEGAL`** -- the source
  answers "legal" when TRYFAULT says 0 AND the fault is a write on a
  present page (`(err & 0x13) == 0x3`; the first form, LEGAL for any 0,
  broke pf2 no-exec and pf4 stack-exec, whose fetch faults TRYFAULT cannot
  judge), and the destination re-takes coh_lock, re-checks the owner and
  grants the write in place (site4L, `n_prot_legal_*`). Userspace only.
  **Gates: chain s40g suite 34/0, pg3/ws1/sig1 48/48, hx2 47/48; chain
  s40h suite 34/0, hx2/pg3/ws1/sig1 48/48.** Residual ~1/100 pool runs: a
  destination pull of the hx2 page stuck ~3.5 s in the source's CTXPAGE at
  start-up (main still spawning threads), after which every side hits the
  6 s fault deadline (exit 131, no `becomes signal`). The SLOW PULL / SLOW
  CTXPAGE instruments (own caps) then photographed it: every serve of the
  address space sat 6.1 s INSIDE `VMCTX_CTL_SERVE` with the busy loop idle
  -- only a per-mm lock blocks every serve regardless of page -- i.e.
  `mmap_read_lock` queued behind a writer (main's mmap of a new thread
  stack) queued behind a reader: the TRIED pass of `vmctx_anon_fault`,
  which reported with the lock HELD while the monitor waited for a
  sibling's landing, whose GET the destination answered INFLIGHT because
  its pull was the stuck serve. The kernel's own comment names this cycle
  and the rule ("do not hold mmap_lock across the wait for the monitor");
  the blocking TRIED pass broke it. **Kernel #156 (patch 0074): the TRIED
  pass drops the lock as the first pass does** (handle_userfault's rule: on
  every pass), with the SELF shortcut (record HOME on the retry = the local
  fill is the answer) kept BEFORE the drop so a declined fault cannot loop.
  Chain s40k on #156: suite 34/0, hx2/pg3/ws1/sig1 48/48, and for the first
  time a whole chain with `fault_deadline_hits` 0, `pgrec_wait_timeouts` 0,
  `take_lost` 0 (`vmctx_af_retry_tried` 92224 second-pass drops); a 192x8
  hx2 pool 192/192. **Kernel #157 (patch 0075)**: the mm change-log
  notifier is `mmu_notifier_put` at context exit instead of
  `mmu_notifier_unregister` -- no `synchronize_srcu` in the exiting task
  (the D-state every photograph of a teardown showed); chain s40m green.
  **And the measurement was wrong by more than the system** (see 4.2):
  every wall before today carried the runner -- ps/pgrep inside
  local-here.sh's clock, vmhome's start and a 100 ms poll inside perf.sh's,
  and this box's uutils coreutils: `timeout true` is 114-121 ms (a 100 ms
  child poll) and `env`/`date`/`cut`/`sleep` ~10 ms each to exec. The
  harness now clocks only vmremote with `$EPOCHREALTIME`, uses util-linux
  `setsid` + a bash watchdog (rc 124 kept) and exports the env itself.
  Honest numbers (#157, idle, medians of 5): **wc 48 ms** (native ~12-19),
  curl 94, sha 129, links2 119, python 318, gzip 1257 (native 434),
  getppid 70 us; per-fault 39-76 us and per-syscall unchanged from session
  39 -- wc was never 290 ms. Compare per-event costs across sessions, never
  old walls. Chain s40n on the new harness green. **Kernel #158 (patch
  0076) + chunk readahead**: the destination's 64 KiB chunk never landed
  past one chunk (only the faulting page is backed; the first refusal
  cleared `chunk_ok` for the run); neighbours now land in the backing
  object and the reply maps the verified run from it, populated
  (`VMCTX_MAP_SET_POPULATE`, read-only ranges only, never a page the object
  is not known to hold), with a forward readahead window (2 pages, doubled
  on sequential faults, capped 16; the fixed window wasted ~800 of 1043
  pages on wc). Fault events wc 513->424, sha 1031->944, python 2311->1930;
  loopback wall inside the noise (a round trip here costs a page copy);
  the cross-machine case is where it pays. Chain s40o green. **The
  source-side readahead** (the I/O-buffer bounce): vmhome's fault service
  reads ahead inside an assisted call -- the window's GETs written before
  the fault's own, their replies taken off the stream first (the channel
  answers in order), CLAIM/LAND IF_ABSENT/restore per page, landed BEFORE
  the RESUME. Its first form landed after the RESUME to overlap the
  context's copy and **1 gzip run in 7 read its buffer's last page one
  block stale** (see 4.2: a landing into a running context loses a
  store); before-RESUME 42/42 identical. On loopback neither readahead
  pays (a page of take+wire+land costs what a round trip costs; sha +40%,
  gzip -27%), so **both are gated on the channel's measured round trip**
  (`RA_MIN_RTT_US` 120; loopback reads 40 us and stays OFF at the
  baseline; a LAN engages). `VMHOME_READAHEAD`/`VMR_READAHEAD` force the
  gate. Chains s40p (gated) and s40q (forced on) green. **The Intel
  destination (.30) carried to its kernel #55 and the CROSS-MACHINE suite
  is green** -- the scoping insight: .30 is only ever a DESTINATION, so the
  source-side patches (0073 the record, 0074 the lock drop) and 0075 (its
  one mn arm is the dormant kick) need no port; the five committed twins
  (0066-0072 era) were built and a 0076 twin committed (e41ffce54).
  Discipline: known-good saved for all three images, tools/build-kernel.sh
  only, qemu preflight, reboot-target.sh, verify green on ONE source id at
  both ends. Cross suite 33/34 then 34/34 then 68/68 (the one miss: pf3's
  fpe, #DE child died 242 with the module silent, never repeated). **Cross
  A/B of the readaheads: a LAN round trip is ~300-430 us, both gates
  self-enable, and gzip's forwarded-syscall time falls 35% (4.76 to
  3.11 ms/call), sha 26%, walls -7..-13%, gzip byte-identical over three
  readahead-ON runs** -- the loopback wash and the LAN win are the same
  arithmetic with the wire's price changed, which is what the RTT gate
  encodes. **Writable-range chunking (kernels #159/#56, patch 0077):**
  SET_POPULATE write-faults the run by hand (mm_populate refuses
  shared-mapping write faults -- read-only entries would cost a protection
  exit per page); the window's floor splits by cost (RO: speculative 2 --
  a copy both sides keep; writable: evidence-first 1 -- a TAKE the source
  loses), and a contended page never chunks (the same page twice is not a
  sequence). Chains s40r/s40s forced-ON green; cross suite 34/34 on the
  new kernels.
- **Session 39 (HANDOFF.md §39) -- the page record moved into the
  kernel.** The owner's directive: reading `/proc/<pid>/maps` and consulting
  several places per page, at tens of microseconds, is not acceptable; move
  page tracking into the kernel or find another way; read the whole tree and
  redesign, big changes welcome. Done. **Where each page of an adopted
  address space is now lives in the kernel, beside the page tables** (kernel
  **#154/#155**): one entry per page (HOME/REMOTE/CLAIM/TRANSIT/NONE) in a
  per-mm xarray on the `vmctx_mm_slot`, and every transition is made under
  the mm's lock together with the page-table action it names. Five new ctls:
  `VMCTX_CTL_SERVE` (answer the destination's "give me this page" in ONE step
  -- mapping lookup with stack-grow, class, record, take-or-copy, record
  update, all under one lock; the whole of the old `ctx_page_inner()` +
  `ctx_movable_ranges()` + the pagemap/peek reconciliation), `VMCTX_CTL_LAND`
  (install a fetched page and record HOME, one step -- POKE + record),
  `VMCTX_CTL_PGACK` (settle the two-phase hand-over: TRANSIT->REMOTE on the
  destination's INSTALLED ack, sum-guarded), `VMCTX_CTL_PGSCAN` (the pages of
  a range in a state-mask, for the shared-file reclaim), `VMCTX_CTL_PGSTATE`/
  `VMCTX_CTL_PGSET` (read/write one entry). The kernel's fault hook
  (`vmctx_anon_fault`) CLAIMs a page before the monitor is told, so a sibling
  context's fault on the same page WAITS in the kernel on the entry's futex
  (`pg_wq`) rather than racing a second fetch -- the receiving-side race
  session 34 measured, and the source-side claim storm, both gone by
  construction; the reply's LAND/SELF/KILL is the release. **vmhome dropped
  from ~9.7k to ~6.8k lines**: the whole `pgown_tab` hash, the ownership
  sweeper, the `strail` page trail, the `infl[]` in-flight table, the
  `/proc/<pid>/maps` snapshot (`ctx_vmas_*`, session 38's own optimisation)
  and the fifteen-hundred-line serve-path retry ladder are deleted -- they
  existed to reconcile a userspace table with three kernel facts read
  separately, and there is one fact now. Records are erased where the kernel
  releases the memory (the vacate hook), so exec/munmap/DONTNEED need no
  userspace forget. Also: `VMR_OP_INSTALLED` is now one-way (VMR4 magic; the
  source settles the record as it reads the ack, nothing waits), the ack is sent for
  EXACTLY the pages the source TOOK (the CTXPAGE reply's `map_off` bitmask,
  consulted on both the fault and the region-chunk paths) and skipped for
  copied pages -- the two wrong versions each cost a session-lesson: a
  fault-path-only skip left chunk-taken pages stuck in TRANSIT and, with
  #155's TRANSIT-wait, made a later source fault wait the 6 s deadline
  (forwarded syscalls 4 ms-520 ms); always-send fixed the speed but its
  ~20k stale acks correlated with an mf2 fail + take_lost=1 in the pool.
  Exactly-taken is fast AND clean; the source's `VMCTX_CTL_PGACK` is
  sum-guarded and address-keyed, so "when unsure, send" is the safe default.
  The source writes no per-request log line for a page. **Gates: chain s39a
  (kernel #154, first build) suite 34/0, pg3/ws1/sig1 48/48, hx2 41/48; the
  hx2 shortfall was a fault stealing a page mid-SERVE (TRANSIT->CLAIM), fixed
  in #155 so a fault WAITS on a TRANSIT page -- s39b suite 34/0, pg3/ws1/sig1
  48/48, hx2 43/48; s39c (always-send ack) regressed to 33/1 + hx2 38 with
  ~20k stale acks, so the ack is now sent for EXACTLY the taken pages; s39d
  (final: exactly-taken) suite 34/0, pg3/sig1 48/48, ws1 47/48, hx2 37/48,
  ack_stale +1, take_lost 1 over 10M hand-overs.** hx2's teardown pool
  (~40/48 across the session) is modestly BELOW session-38's 48/48 -- the
  session-37 deep-teardown class, which the record's substrate now makes
  tractable (a dead holder is nameable) but which this session does NOT yet
  fix (dying contexts' claims are not cleaned): REDESIGN step 3 is the top
  NEXT. `take_lost`/`takeobj_stale_store`/`mm_overflow` 0 throughout;
  the record's own counters (`vmctx_pgrec_*`) show claims==lands==acks with
  erased/nomem/ack_stale 0. **The measurable win is the syscall COUNT per
  page** (the deterministic evidence, per session 38's trap): the old SERVE
  did up to five syscalls a page (pagemap pre, TAKE loop, pagemap post,
  peeks); the new one does one, and the fault-service pull folds its claim
  and install into the report/RESUME the fault already makes. Next lever
  unchanged from session 38: the I/O-buffer bounce (a page-channel GET per
  buffer page).
- **Session 38 (HANDOFF.md §38) -- the performance campaign.** Typical
  programs measured at every layer (`tests/perf.sh`: one build against
  native; `tests/perfab.sh`: two builds ALTERNATED run by run, the only
  honest comparison on a box whose idle states move a run by +-50%). Every
  real program is demand-paging-bound, 20-90x native; three bottlenecks
  named by `perf trace -s` on both halves and removed, no kernel change:
  (1) vmhome re-parsed `/proc/<pid>/maps` twice per page served (~137
  syscalls per request; links2 with 91 libraries paid 1.5 ms per fault) --
  now a per-thread snapshot keyed on `map_gen`, bumped at the end of every
  request that is not a pure page read/record, liveness asked of
  `ctx_present() < 0` beside it; (2) the INSTALLED ack's reply blocked the
  service thread before the next fault -- now owed and drained
  (`home_drain()`) before the next reply; (3) vmremote's start-up
  `usleep(100000)` (the whole 108 ms "setup") -- now a condvar on the page
  service's listen(). Then the per-fault syscall overhead (~67 per fault):
  gettid x5 (cached, `own_tid()`), futex x3 (own.h's mutex is three-state
  and a landing wakes only counted waiters), and clock_gettime x13 that
  were real syscalls because **the box's clocksource was hpet** (the TSC
  watchdog's false positive at boot: `tsc=reliable` in GRUB, rebooted).
  And wake-up latency: the box's C2/C3 exit is 350-400 us, so both
  monitors now hold `/dev/cpu_dma_latency` at 20 us (PM QoS) for their
  lifetime -- no spinning. The per-futex log lines are armed
  (`VMREMOTE_FUTEX_LOG`/`VMHOME_FUTEX_LOG`), not always on. **Result
  (perfab N=5, baseline -> final): wc 0.47->0.25 s, curl 1.05->0.38,
  sha256sum 0.77->0.46, python 1.27->0.63, gzip 4.46->2.17, links2 -dump
  2.07->0.47, getppid round trip 101->55 us; per fault wc 250->62 us,
  links2 1474->187 us.** Gates: chain s38a (baseline) and s38b (first
  build) suite 34/0, hx2/pg3/ws1/sig1 48/48 each; the final chain is in the
  handoff. **The next lever is the I/O-buffer bounce**: a forwarded
  read/write of a 32 KiB buffer costs a page-channel GET per buffer page
  (~110 us loopback, a wire RTT cross-machine; sha's syscalls 523 us,
  gzip's 1254 us after everything else) -- a readahead-style pipelined GET
  on the source's fault service, gated by the chain because it touches
  the coherence path.
- **Session 37 (HANDOFF.md §37).** hx2's zero-read class stays fixed
  (session 36); its RESIDUAL ~0.7% death is NAMED as the deep teardown
  thread-list coherence -- a thread descriptor/stack page whose content is
  genuinely lost from BOTH machines at pthread_join. Two sub-modes (exit -1
  no-output via `PULL_GONE`->SIGKILL of the live joiner; exit 242 NULL-deref
  at glibc `__nptl_deallocate_stack` rip 0x4542e0). It needs the full chain's
  load -- **0 in 1024 hx2-only runs**. A point fix (serve the retained copy on
  `PULL_GONE`) was tried and REVERTED: gate38 (5 chains) showed it fired 0
  times while 2 deaths still occurred -- the content is truly gone, no fallback
  reconstructs it. The real fix is REDESIGN step 3 (own.h shared ownership),
  which is also the ws1/netsurf teardown death (one bug, named at
  vmremote.c:1963). A cheap capped **LOST-PAGE** trail dump was kept.
  `take_lost` proven BENIGN (a sibling faults the memfd folio in during the
  precheck->zap window; bytes survive, caller retries). **Browsers (loopback,
  #153): links2 -g RENDERS end-to-end** (window 1230x949, root colours
  767->1020, GET /page.html 200) -- the current-tree confirmation of the
  objective. **netsurf-gtk** never opens a fetch socket: DIAGNOSED as GTK4's
  glycin image-decode BUBBLEWRAP sandbox, which hangs at startup under vmctx
  (mount/user namespaces not virtualized; the sandbox child stalls after
  pivot_root+seccomp before exec'ing the loader), blocking the main loop before
  page navigation. Native netsurf (even bus-less) runs the same sandbox and
  fetches+renders; `GLYCIN_SANDBOX_MODE=disabled` under vmctx instead exits 242
  (the coherence class). Not a netsurf or networking bug.
- **Session 36 (HANDOFF.md §36).** The hx2 residual's kernel-made
  zero folio (session 35's open door) was NAMED by a census at the one
  place every shmem folio is created (kernel **#150**, `vmctx_shmem_alloced()`
  in `shmem_get_folio_gfp`: by path, with a per-(object,page) stamp that
  lets TAKEOBJ name the maker of every all-zero page it captures — 266/266
  named) and CLOSED where it opens (**#152/#153**, `vmctx_shmem_may_alloc()`):
  a monitor's `VMCTX_CTL_PEEK` of a page whose folio a TAKEOBJ punched
  between the pagemap check and the read GUP-faulted the hole and shmem
  ALLOCATED a zero folio — the object then held zeros as the address
  space's memory. Now a foreign READ of a hole in a kern-mounted shmem
  object (memfd, MAP_SHARED-anon — not a tmpfs file, which broke rd1 on
  #151) answers -EFAULT, a short read the monitors already handle. Session
  35's refutation of this door was by `ffile_invent`, which looks the folio
  up BEFORE the fault and so cannot see the punch in between (10 → 10 while
  the census counted 18): count where the thing happens. **Gates (chains of
  suite + 48x8 hx2/pg3/ws1/sig1): #150 34/0 46/47/48/48; #152 34/0
  48/48/48/48 and 34/0 47/48/48/48 (the miss an exit-242 NULL deref of the
  §6.1a family); #153 (switch deleted) at the end of the handoff.**
  `tests/chain.sh` is the tracked chain. Port compiled into
  `src/linux-6.18.35`, not run. Kernel counters `vmctx_shm_*`,
  `vmctx_takeobj_zero*`, `vmctx_shm_refused/filed`.
- **Session 28 (see §2.21, §2.22, §6.1a).** The "2 s race" of session 27's
  review was a claim storm and is gone: a source fault service held
  `PG_CLAIM` over a page a SIBLING's pull had already landed, for the
  destination's whole 1200 ms INFLIGHT grace, refusing every guest pull of
  a page the source already held. The claimant now ends its claim on the
  landing (`pg_get_op` checks `ctx_present()` on each INFLIGHT answer; an
  absence fault on a present page is not asked at all) — vmhome.c only.
  Kernel **#149** (patch 0071): TAKEOBJ re-reads the folio after the punch
  and re-copies if it changed (`vmctx_takeobj_stale_store`), and the mm
  table holds 256 address spaces. **Gates on #149 + this userspace: suite
  102/0 ×3; pg3 pool 256/256 (was 251) with 0 storms (was 10 of ~2500
  yields + 1 give-up) and the pool 13% faster; re-measured on the final
  form 252/256 with 0 storms (max 3 yields, 0 give-ups); hx2 pool 249/5/2
  (session 26: 250/6/0; the first form of the fix had regressed it to 235
  — §2.21 has the A/B).
  `vmctx_takeobj_stale_store` = 0 over 20.9 M hand-overs including runs
  that died** — the residual 242 is NOT a store through a stale
  translation. Its shape is now one line (§6.1a): B's service context,
  `0x7ffff77f2000 answered ABSENT and filled with an empty page (rip
  0x469cc2)`, 16 KiB below the range glibc's thread-exit `MADV_DONTNEED`
  discarded, then a NULL deref ~25 ms later; the print now carries the
  mapping and the forwarded call. `vmctx_take_lost` did not recur.
- **Session 26 (see §2.19, §6.5).** The fork-from-threaded-parent wall
  (mf2; the shape of firefox's sandbox child and netsurf's glycin forks)
  was the MONITOR's own address space carrying the guest's mappings: a
  mapping change fanned out to sibling contexts (`VMCTX_CTL_APPLYMAP`) was
  applied by the kernel's SET/PROT arms to `current->mm` — vmremote's —
  and every context clone()d from the monitor afterwards inherited those
  VMAs, a fork child among them, which then mapped its PARENT's object at
  its own thread-stack addresses. Fix: vmremote fans out only UNMAP/ZAP;
  kernel patch 0069 (**#148**) refuses a cross-task SET/PROT;
  `monitor_mm_check()` asserts the invariant at every spawn. mf2 is a
  regular suite case. New probe: `page_provenance()` (PFN + object bytes
  of a page in this context and in the address space it was copied from).
  Session 25's "as of the break" reading of mf2 was wrong; §6.5 keeps the
  photograph that refuted it. A second change re-nested the page budgets
  (§2.20): the kernel fault deadline was 2000 ms while the destination's
  GET handler can legitimately wait 2.5 s inside it, so a slow GET under
  pool load ended its context (exit 13) — the deadline default is now
  6000 ms (patch 0070/0040), the tools raise a lower live value at
  start-up, the GET socket bound is 4 s and the source's INFLIGHT re-ask
  loop is time-bounded. **Final gates on #148 with both changes: suite
  102/0 ×3 (34 cases); hx2 pool 250/256 — 0 no-output (the exit-13 class
  is GONE, was 5), the 6 remaining all the §6.1a TLB/split-brain residual
  (3 zero-reads, 3 stale one-behind); pg3 pool 254/256 (2 × rip 0 from the
  thread stack page, §6.1a); 10 fault-deadline kills across both pools,
  all legitimate. netsurf still runs the full 109 s and maps its 1000×700
  window (`http.log` empty — the session-25 pre-fetch wall, unchanged),
  with `PROVENANCE` now in the log for its fork children.**
- **Session 25 (see §2.18, §6.1a).** The hx2 pool's residual was three
  answers of the destination's page-service GET handler that were not the
  page (a lost-race serve of the pre-wait read; ABSENT retaining zeros;
  ABSENT while this side's own pull was in flight) — fixed in
  `vmremote.c`'s `pg_serve_conn()`, no kernel change, protocol unchanged.
  Numbers on kernel #147 + this userspace pair: **suite 31/0 (hx2 and ns1
  XPASS, both promoted to regular cases); hx2 pool 248/256 (was 242 — and
  the zero-loss class 10 → 1); pg3 pool 256/256 (was 253)**. The remaining ~2% is a different class, photographed
  by the new `LOSS-PROBE`: the source holding a page its record says it
  handed over (§6.1a). New instruments: hx2's loss stamps + marker syscall,
  `VMR_TRAIL_PAGE`/`VMHOME_TRAIL_PAGE`, `ZERO-TAKE`, RIP in `on_crash` (and
  a core). NEVER arm the module's `trace_zero` (§4.2).

### State as of 2026-08-20

- **Config:** .229 (AMD, source) runs kernel `7.0.14-vmctx` **#148**
  (session 26; patches 0001–0069: 0069 refuses a cross-task SET/PROT,
  0063 the mm-mutation-hook change log, 0064 the
  MADV_DONTNEED discard, 0066 the cross-task munmap accounting fix,
  0067 the zero-page forbid for adopted mms, **0068 the take's
  post-COW-break re-lookup — the netsurf dlopen boundary page**), module
  loaded;
  userspace is the **VMR2** pair (vmr_rsp carries vacate[], bit 0 =
  discard). .30 (Intel, dest) runs `6.18.35-0-lts` **#54** (patches
  0001–0038 committed to that tree — 0037 the munmap accounting, 0038
  the zero-page forbid — but **NOT YET built/netbooted**; the series is
  BATCH-carried, not 1:1 with 7.0.14's, and lacks source-side machinery
  like 0051's break-cow-at-take: a .30-as-source needs the real port).
- **The map_count desync is FIXED (0066, kernel #145).** The zero-sum
  ±1 accounting error the handoff's #1 open thread named:
  `vms_complete_munmap_vmas()` decremented `current->mm`'s
  map_count/total_vm/locked_vm/*_vm for the vmas an unmap removed, and
  vmctx's `VMCTX_CTL_APPLYMAP` unmaps out of ANOTHER task's mm (the
  monitor tearing a guest context's stale mapping after a forwarded
  munmap or exec), so the monitor's counters were charged for vmas that
  left the guest's tree. Convicted by a controlled experiment: mu1/mu2/
  fi1 (real forwarded munmap → cross-task UNMAP → do_munmap) desynced,
  th9's MADV_DONTNEED (ZAP, removes no vma) did not, stk1/cow1 (no
  vacate) did not. Fix: account to `vms->vma->vm_mm`. Verified booted —
  mu1/mu2/fi1 desync 2→0, all still PASS. It corrupted total_vm/
  locked_vm of the monitor too, so it was a live suspect for the
  teardown-churn band.
- **Propagation of destruction is the kernel's** (the owner's ruling):
  reply_note_map's munmap/mremap decode is deleted; the source's mmu
  notifier (MMU_NOTIFY_UNMAP only) feeds a per-mm ring drained after every
  forwarded call (`VMCTX_CTL_MMLOG`) and shipped in the reply
  (`vmr_rsp.vacate[]`); the destination's `range_vacated()` gives each
  range the full munmap treatment. tests/mr1 (in the suite) is the direct
  proof it covers mremap's three vacate shapes.
- **MADV_DONTNEED propagates** (0064): tests/md1 photographed the gap (a
  DONTNEEDed arena read back its 0xaa under the guest — the heap-corruption
  seed wherever an allocator trusts DONTNEED-means-zero); the madvise path
  itself notes the discard (CLEAR cannot be logged wholesale), a SOFT ring
  entry (bit 0) makes the destination punch content and ZAP pages
  (`VMCTX_MAP_ZAP` = do_madvise on the member mm) while mappings and their
  records stay. md1 is in the suite.
- **Numbers to trust (2026-08-20 night, kernel #147 + the session-23
  userspace pair):** suite **90/0 ×3** (dl1 now a passing case, not an
  xfail; ns1 the only XPASS); **dl1 torture 96/96** (0 deadline kills,
  19104 COW-breaks at the take — the 0068 path exercised clean under
  8-way load); **pg2 255, 256, 256 of 256** (hang class DEAD, was 4,1,0
  on #145); **pg3 95, 95, 95 of 96** (one-behind 10/288 → 1/288 after
  the landing-beats-arriving-bytes fix). Residual census §2.15/§2.16:
  died-242 ~2/288, quiet one-behind 1/288, pg2 startup ABSENT-fill
  1/768. ws1/hx1 not re-measured on this pair — quote §2.14-era numbers
  with that caveat.
- **Browsers:** the netsurf dlopen boundary-page take-refusal (7–9s
  death, 'claim refused AFTER the zap refs=3 want=2 anon=0') is FIXED at
  kernel #147 (patch 0068, §6.1) — its deterministic test tests/dl1 is a
  passing suite case. **Re-probe on #147: netsurf now runs 21.6s (was
  7–9s), MAPS ITS WINDOW ('netsurf' 10x10, never did before), 0
  after-zap refusals, 5038 syscalls** — the wall moved. The new wall is
  LATER and a different class: glibc heap-metadata corruption
  (`malloc(): mismatching next->prev_size (unsorted)`) → null deref at
  0x0 (242) — a stale page served over live heap metadata, the
  died-242/pg3 coherence family (§2.15–2.16), not a take refusal. A SLOW
  coh_lock (31ms across a blocking step) also reappeared in the run —
  watch it. The paint block (session-21 assisted-futex EIO storm) is
  further out still. Still open and separate: vmremote's own rc=139
  self-SIGSEGV seen in one probe (§6.4; the armed backtrace handler
  produced nothing, so core_pattern is set to /tmp/core.%e.%p on the box
  — non-persistent, re-set after any reboot — to catch the next core).
  firefox maps its window; its own-PIE-text -14 fetch is its standing
  thread. The two mmu-notifier traps from landing the change-log hook
  remain in this bullet's git history.
- **Performance:** guest compute 95–100% of native (99.9% measured on the
  Intel box; 96.4% on .229/#146 with the standing guest-ASID flush); a
  forwarded syscall costs one LAN round trip (~200–482µs). Measured
  loopback on #146 (vmbench, no LAN): **165µs/call**, of which ~124µs is
  the monitor service chain (WAIT wakeup → TCP loopback → vmhome →
  adopted-task syscall → reply → RESUME) and ~40µs guest entry/exit — the
  wire is NOT the bulk; cutting it means event-path work (busy-poll WAIT,
  batching), a designed-experiment campaign, not an obvious tweak.
  TCP_NODELAY is set on every channel and both halves build -O2.

## 2. The campaign digest, in order

Each entry is one campaign; the mechanism it left behind is described in
AUDIT.md. Numbers are quoted where they decided something.

1. **Backends and remote execution.** SVM and VMX guest entry for a plain
   process's user code; syscalls leave via #UD (EFER.SCE clear); monitor
   redirection (WAIT/RESUME, PEEK/POKE, GETREGS/SETREGS); vmmon/vmmig proved
   monitoring and checkpoint/restore; vmhome/vmremote split the monitor
   across two machines. The destination's own-machine tools (startvm,
   vmenter, vmcat, vmproc) were later **deleted**: the destination owns
   nothing (PRINCIPLES 6a) and never names the program.
2. **The shadow removed.** Early design kept a "shadow" copy of the program
   on the destination. Replaced by adoption: the source adopts the real
   ptrace-stopped task as a service context, and its address space is the
   single authority. No shadow exists anywhere now.
3. **Map, don't copy (hx2 root cause).** Copying pages onto their own folio
   (a pread of the backing object written back with process_vm_writev — a
   13µs read-modify-write over a page three sibling contexts are running on)
   lost stores. `VMCTX_CTL_MAPOBJ` maps the object's folio instead; the
   repair was applied arm by arm as each copy site was convicted (sites 4,
   35, branch 7, the chunk path). hx2's divergence went 6.8M→4267 when
   MAPOBJ stopped inventing pages for shmem holes (kernel #125, patch 0055:
   `filemap_lock_folio` only, holes answer 0 — measured `holes=0` across
   2.4M calls, so that door was real but not the residual's producer).
4. **The coherence audit.** Every forwarded syscall's memory effects audited;
   the write-back direction (downgrade, upgrade, recall) built; the kick
   (`backend->invalidate`, waited broadcast IPI) turned on by default. The
   recall channel exists but vmhome opens none: the return direction runs on
   the page channel as pulls (n_pg_pulled), and the recall counters were
   moved out of the summary so their structural zero stops reading as a
   measurement.
5. **Fork is not a copy.** Eager fork copies (whole-object copy_file_range,
   then a lazy per-page variant) both made pages live in two places and were
   removed. A fork child's object starts EMPTY; the source ran the real
   clone3 and its kernel owns copy-on-write; `VMR_PG_COWBREAK` /
   `VMR_CTXPAGE_COWBREAK` move a page's break to the machine that holds it.
   The take's COW break is judged by the NEW folio returned from GUP, not
   the original's mapcount (kernel #127 — a private FILE page's page-cache
   folio keeps its high mapcount forever and the break was disbelieved:
   "mapped 30 time(s)").
6. **ws1: the lost-store oracle, three acts.**
   - *Act 1 — site 1:* branch 10's own-object precheck copied a live folio
     onto itself; `serve_object_mapped()` fixed it (A/B alternating in one
     pool: lost increments 4→0, sysmalloc asserts 4→0; 497/512 vs 483/512).
   - *Act 2 — the register:* four sessions of memory repairs measured
     rate-neutral because no page was ever wrong. 13 of 32 pooled deaths had
     **fs_base inside vmremote's own image**: a spawned guest ran on the
     MONITOR's thread pointer — the entry path read the live MSR, and
     SETREGS cannot reach a task already on a CPU. Fix: the vmctx register
     file owns guest FS/GS (kernel #126, patch 0056) + `setregs_settled()`
     at both spawn sites. 52 fails/2048 → 13–17/2048, REPEAT 2→0.
   - *Act 3 — the hang:* a forwarded futex WAIT slept on a stale source copy
     of the futex word and ate wakes; the clear-tid wake was aimed at the
     DYING thread's context (get_task_mm races its exit — 462/500 re-arms
     -ESRCH). The wake now targets a LIVE member of the mm (as_live_ctx),
     re-armed a few times. Hang 13→1–3 per 1024.
   - The ~0.5% residual is TRUE stack-word zeroing (saved-pd slot reads 0,
     ret slots 0, one rip of ASCII pid digits); frames preserved. It is the
     token-drop thread of §6.3.
7. **Teardown and the 98 class.** Exit-98 ("a page on neither machine") is
   DOWNSTREAM of a null-pointer deref caused by a lost store — never a
   teardown-reclaim bug; two reclaim designs were built and reverted (§3).
   What shipped: `as_life` refcount (positive→zero is a latched EVENT), and
   `VMR_CTXPAGE_GONE` sent only on that fact, with the answered context
   ending through its OWN teardown so FINISH and clear-tid still run. The
   rate is unchanged by design — the path only fires inside runs already
   failing; what changed is that the guest is never resumed on a page nobody
   installed.
8. **The browser campaigns.** In order of the walls that fell: RELRO
   seal-timing (ask only about mprotect-sealed read-only file ranges);
   sh_file made AS-keyed + overlap-trimming + gated on PROT_WRITE at the
   mmap (a fontconfig `r--s` mapping was the whole "netsurf MAP_SHARED"
   wall); failed pulls give the page back gated on PG_OURS (ungated =
   pg3 backmove); regions[] made per-AS with post-exec RELAYOUT; exec wipes
   the destination's copy of the AS (exec_wipe + cross-task UNMAP, kernel
   #129) — the byte-identical post-exec child SIGSEGVs died; the ADOPTION
   INVARIANT (kernel #132, patch 0061): redirection only for the adopted mm
   (`vc->noted_mm`), exec notify at COMPLETION — the ELF loader's own writes
   during execve were being answered from the previous generation, and the
   exec'd child booted ld.so on a pre-exec `_rtld_global`; mm_generation
   invalidates vmhome's /proc fd caches across exec; vfork/CLONE_VFORK run
   as plain fork (the assisted vfork suspends the service context forever);
   clone3's args page is PULLED first (a raw peek of a page on the dest is
   refused, and the gate then read garbage). End state: firefox maps its
   window; netsurf's wall remains (§6.1).
9. **No address-specific code (owner's rule).** All stack-special code
   removed (kernel #130, patch 0060): the dead `r->local` headroom branch,
   `vmctx_grow_guest_stack`, `VMCTX_CTL_GROW`. Address validity is asked of
   the owner's kernel — `VMCTX_CTL_TRYFAULT` (0 = real, -EACCES/-EFAULT are
   the only fault rulings, in the kernel's own words). Record first, kernel
   second; /proc/maps text is never an authority on validity.
10. **The syscall hold rewritten (kernels #133–#135, patch 0062).** The
    owner's rule implemented exactly: a page a forwarded syscall touches is
    held while the call CAN touch it — until it returns or blocks, never for
    a fixed term. Coverage = the fault-in registry (reads AND writes, one
    clock) + the PRESENCE rule (every page present in the adopted mm while a
    call actively runs) + ARG-PAGE seeding; lifetime = task_is_running ||
    ev_pending, released on block or return; the safety net bounds the
    call's own CPU time, never wall age (a wall-age form fired on a
    clock_nanosleep that slept its 2s). Take/protect paths re-ask on the
    brink. Closed the mid-call-steal class outright (armed pools 167/168
    with the hold counters doing the work; unarmed pooled effect 9% fail vs
    15% holds-off).
11. **The TLB, settled (kernel #136-era finding + the gen re-arm).** The
    build-time `tlb_ctl=1` that nothing clears has been flushing the guest
    ASID on essentially every VMRUN all along, and that standing flush is
    LOAD-BEARING — it covers every invalidation nothing announces (exec
    relayout, apply_map sibling zaps, POKE's COW break, reclaim). Clearing
    it KVM-style and flushing only by generation killed 24/24 pg3 runs in
    0.5s. What shipped: the standing value untouched + a generation re-arm
    (`vmctx_tlb_gen` bumped by the kick, per-CPU seen-generation, entry
    re-arms `tlb_ctl`/`clean=0` on change) so the flush the system depends
    on is guaranteed rather than an accident of an uncleared field.
12. **VMR_PG_INFLIGHT (the pg3 one-behind, root cause and fix).** At B's
    last round main's pthread_join joins the traffic on B's stack page (the
    futex word lives there); a serve and a pull of that page CROSS; the
    dest's GET handler found "the record says the ASKER holds it" and served
    the RETAINED copy — one write older. The retained copy is NEVER an
    answer for a LIVE address space: the GET is answered INFLIGHT (ask
    again), and THE LANDING — a pull of the page active, or a young episode
    inside a 1200ms grace — decides, not a clock. Retained remains the
    answer for a DEAD holder (the ws1-teardown case the net was built for).
    Result: pg3 96/96 across two unarmed pools, suite 84/0.
13. **The browser trade table (final config).** Scoping the holds to
    running-calls-only (#136/#137) removed every SLOW coh_lock line and
    netsurf still died at its historical time — **starvation is not the
    browser wall**. The 200ms serve-yield (give up to CLAIMING instead of
    waiting out a held call) took pg3 96/96→42/48 and RAISED netsurf's
    stalls — **the wait is the design**. Final: **kernel #138
    (holds-full + arg seeds) + the 1s serve wait**: pg3 48/48, sig1 12/12,
    netsurf at the historical wall with starvation gone.
14. **The hand-over crossing campaign (2026-08-20): four producers
    photographed, root-caused, eliminated; the two-phase hand-over shipped.**
    hx1 (the one-page adversarial reproducer, page 0x7ffff7ff6000 -- the SAME
    address as pg3's B-stack and ws1's futex word in this fixed layout) was
    instrumented with CLOBBER + both page logs, and each armed failing run's
    merged timeline was read backwards from its first BACKMOVE. Four distinct
    producers of "a writer reads back its own counter thousands of rounds
    old", each fixed and each verified gone in the next photograph:
    (1) a serve whose take found nothing because a CONCURRENT serve took the
    page mid-flight answered ABSENT -- now CLAIMING (n_serve_took_midflight);
    (2) the dest believed an ABSENT that had CROSSED its own serve of the
    page to the source (lend record flipped to SHADOW after the ask left) --
    now yields and re-asks (n_absent_stale_crossed);
    (3) home's GONE/THEIRS record refusal answered ABSENT while home's OWN
    pull of the page was in flight (a concurrent serve had stomped its CLAIM)
    -- now CLAIMING via infl[] (n_serve_gone_inflight);
    (4) the refused-take RESTORE resurrected a stale pre-take copy -- first
    over a completed hand-over (record gate), then 3us after a fresh pull's
    install (infl gate) -- now suppressed in both interleaves
    (n_take_restore_stale).
    Underneath all four: the record could not say IN TRANSIT. **The two-phase
    hand-over** (named twice before as the fix and deferred) now can:
    a single-page serve commits PG_INTRANSIT, the destination acks the
    landing with VMR_OP_INSTALLED (sum-guarded against late acks), and until
    the ack -- or a 100ms self-heal for lost acks -- every judgment answers
    "ask again", never ABSENT. Chunk serves stay plain THEIRS (cold bulk).
    The claim give-up's blind own-copy fallback ("the answer that can be
    wrong") is gone: the exit from a claim storm is THE LANDING (backing_has)
    or the fault deadline.
    **Two traps paid for on the way:** (a) the ack was first sent INSIDE the
    fault service -- one extra RTT before every pulled fault's answer -- and
    that latency alone broke pg2 in-suite (starvation: "stamp reached 0", the
    stamper never winning the page; HEAD control 40/40 vs 1-in-15; every new
    counter ZERO in the failing run). Deferred past the RESUME (ack_flush),
    pg2 went 0-failures-in-60 and the suite 84/0 x3. (b) pg2 reproduced only
    under runany.sh, not under bare local-here.sh -- replicate the harness
    exactly before declaring no-repro.
    **Numbers:** suite 84/0 x3 consecutive; ws1 254/256 (best recorded; was
    249/256 same morning); pg3 93-94/96 unchanged; hx1 writer-loss 8/24 ->
    2/24 (the "other" hx1 class -- mutual-timeout churn under its adversarial
    one-page load -- swings 5-18/24 with load and was never green).
15. **The pg3 one-behind, root-caused with controls (2026-08-20, session 23).**
    The residual "B exactly one behind, exactly once, always in the last ~350
    rounds" was convicted by a counter correlation with a control arm the
    harness previously deleted (torture.sh now records each passing run's
    claim/give-up evidence): every one-behind run carried EXACTLY ONE claim
    give-up (~2650 yields, ~1.4s) and EXACTLY ONE destination retained-copy
    serve of B's stack page; 186 passing runs carried ZERO of either. The
    yield distribution is bimodal (passing runs: 0-1 yields; failing: ~2650)
    -- the "storm" is not load, it is a join-time crossing LIVELOCK: main's
    pthread_join fault claims the page (PG_CLAIM) and GETs it from the dest;
    the dest's record says the ASKER holds it, answers INFLIGHT; B's pull
    meanwhile gets CLAIMING from the source's standing claim; nothing
    progresses until the dest's 1200ms INFLIGHT grace expires and its
    retained copy -- one write behind -- breaks the tie. The CLAIMING lines
    print here=1 throughout: a sibling monitor thread's pull had ALREADY
    landed the current copy at the source mid-storm, and ctx_monitor's
    got==0 path POKED the arriving retained bytes over that fresher landing
    unconditionally (the sweep's "contents DIFFER (now e81c, served 25e1)"
    is the same event from the record's side). The destination has had the
    rule "a live folio beats arriving bytes" since hx2's sites 4/35; the
    source did not. **Fix: the source-side half of the same rule** -- in
    ctx_monitor, arriving GET bytes for an ABSENCE fault (err bit 0 clear)
    are DROPPED when the page is present by the time they arrive
    (`n_pull_landed_raced`, src_arrival 8, ACT_DONE); protection faults keep
    the poke, whose forced write is what resolves them. A blind poke over a
    present live page is a single-writer violation whichever bytes are
    newer. **Gates: suite 90/0 ×3 (0 known gaps still failing); pg3 95,
    96, 94 of 96 with the one-behind class at ZERO across 288 runs
    (baselines same day, same kernel #145: 92/88/91 and 92/94/91 with 3-7
    one-behinds per round); nine full storms appear in PASSING runs'
    evidence — every one survived. The 96/96 is the first perfect unarmed
    pool since the band appeared.**
    Also fixed on the way: n_sys/n_fwd were plain ++ on thread-shared
    counters (the run-end "-2 syscalls NOT forwarded" was a torn increment,
    not a re-issued call) -- now relaxed atomics; the §6.3 take-and-put-back
    write-grant fallback got its missing counter (n_grant_takeput) and an
    env-gated raced-put-back probe (VMR_TAKEPUT_PROBE=1).
    Still open from the same window: the died-242 shape (~1/96: RET to 0
    with rsp inside B's stack page; source answered ABSENT off a plain
    THEIRS record -- the INTRANSIT-heal suspect was REFUTED, healed=0 in
    both died runs) and the minority no-storm one-behind (dest
    n_get_lost_race crossing, ~1/10 of failures).
16. **pg2's hang class: the unremovable zero page (2026-08-20, session 23,
    kernel #146 / patch 0067).** pg2 torture on the one-behind-fixed pair:
    252, 255, 256 of 256 -- the only failure mode left was HANG (rc=124,
    the guest's own 60s limit; fault-deadline kills = 0). Every hang is one
    signature: the guest prints only its header (the stamper never executes
    its first instruction; main waits in pthread_join to the limit), and
    the source's serve of the stamper's stack-top page cycles "was taken
    and is still present here after 200 more takes" -> CLAIMING forever.
    dmesg names it: **"TAKE 0x7ffff7ff5000: this is the shared zero page
    (pfn ...): it can never be claimed, and every retry will be refused
    the same way"** -- the kernel diagnosed the terminal state and still
    returned plain -EBUSY, so the serve retried what it was told can never
    succeed, and the 2s deadline never fired because each CLAIMING answer
    completes a fault service. The zero page got installed by a read fault
    on the source at an address whose real bytes live on the destination
    (main built the thread descriptor there in guest execution) -- the
    monitor-bypassed read the take's own comment names ("the repair
    belongs where the zero page was installed"). ABSENT-at-the-take was
    already tried and measured worse (10/16 vs 12/16, deaths went silent);
    the shipped repair is at the INSTALL: `mm_forbids_zeropage()` hooked
    (the s390/KVM door) so every road to the zero page -- anon fault, THP,
    migration, userfaultfd -- hands an adopted mm an ordinary zeroed folio
    a later take can move. Lockless predicate (one READ_ONCE of
    vmctx_mm_live on an idle box; racy 64-slot scan when live -- false
    negative = today's behavior, false positive = one real folio);
    `vmctx_zeropage_forbidden` counts firings. **Gates on #146: suite 90/0
    ×3; pg2 255, 256, 256 of 256 — the hang class is DEAD (was 4, 1, 0 on
    #145 same day), the counter fired 58 times across the battery; pg3 95,
    95, 95 of 96 (the one-behind fix holds, two more storms survived —
    eleven of eleven across both builds).** The one pg2 died (1/768, exit
    242 at 1.28s, main dereferencing ~0 at startup) is the corruption the
    livelock was MASKING: a startup fill of one never-touched page after
    the destination answered ABSENT for an address it held real bytes for
    — the ABSENT-fill branch is now loud (address + rip, always). What
    remains after both fixes, all named with reproducers: the died-242
    (~1/96·2/3: NOT-HOLDER off plain THEIRS at the join → B RETs through
    a zeroed stack word at page+0x68; identical on #145 and #146, so NOT
    zero-page-class), the pg2 startup ABSENT-fill (~1/768, same family as
    died-242's zero word — likely ONE defect: the dest answering ABSENT
    for a page it holds), and a QUIET one-behind variant (1/288: zero
    yields, zero give-ups, zero crossing counters — no current instrument
    names it; it was invisible under the dominant producer's 10x rate).
    **The died-242 hunt's negative results (paid for, do not re-walk):**
    a 3×96 battery with VMR_PGLOG on B's page went 288/288 — the page log
    MASKS both residual shapes (the armed-pools lesson again), so they
    are sub-µs races in the normal take/pull/serve cycle. In the unarmed
    died runs every named fill path shows ZERO pre-death: no retained
    serve (n_empty_averted 0), no mid-run EMPTY, no give-up, no crossing
    counter, and the only DECLINEs are 0x0/0x8 — the post-corruption
    faults themselves. The shape predates the landing-gate fix (same
    ~1/96 rate on both sides of it). Next instrument, per the session-21
    prediction: a source-side SEQUENCE STAMP carried in the protocol
    (serve/take generation per page), so a stale install names itself
    without serializing anything — the hist/clobber family cannot do
    this without masking.
17. **This tidy pass (2026-08-19).** Every proven-default A/B switch and
    every dead experiment removed, behavior identical (list in §4.3);
    the `VMHOME_ENV` delivery bug in three launcher scripts fixed
    (`VMHOME_TRACE_SEL=${VMHOME_ENV:-1}` assigned a variable nothing reads —
    env intended for vmhome was NEVER delivered; any past "vmhome env arm"
    quoted from those scripts measured the default); dead `clobber_note()`
    and `ctx_map_find()` deleted; stale INFLIGHT/50ms comments corrected.
    **Finding recorded:** since the THEIRS-recording change (`own_none`
    const 0), the source answers ABSENT where it once sent
    `VMR_CTXPAGE_NOTHOLDER`; the destination still handles NOTHOLDER but
    never receives it. Every green number above was measured with ABSENT,
    so ABSENT is the proven configuration — but if the destination is ever
    again caught fabricating a fresh page for an address the source records
    THEIRS, re-arming NOTHOLDER at the serve's GONE branch is the ready-made
    experiment (see the comment there in vmhome.c).

18. **The hx2 pool residual was the page service answering with bytes that
    were not the page (2026-08-21, session 25).** Session 24 left hx2's pool
    at 242/256 and called the rest a within-epoch race the generation stamp
    could not order. It was not. Three instruments named the producers
    without an armed page: tests/hx2 stamps each loss on CLOCK_MONOTONIC
    (the trail's own clock) and makes a marker `getppid(2)` that vmremote
    answers with a `LOSS-PROBE` (every member's present bit/PFN/slot, the
    object's and the retained copy's slot, the page's trail); `VMR_TRAIL_PAGE`
    / `VMHOME_TRAIL_PAGE` dump a page's whole trail on both machines at exit;
    new trail kinds SERVE-GET-NOTAKE, PUT-LANDED, TAKEPUT-POKE. Read against
    the loss stamps, three GET-handler answers stood out, all in
    `pg_serve_conn()`:
    (A) **the lost-race serve** — `page_read_as()` reads the page at the top,
    the handler then waits (claim 1000ms, TAKEOBJ -EBUSY 1000ms), and when
    the take finds nothing (the other channel's GET took it) the branch fell
    through to `get_send` with status 0: the PRE-WAIT bytes went out as a
    successful GET stamped with the CURRENT count and were retained as the
    newest copy — a writer reading 4k–1M rounds behind with 0 STALE, because
    the stamp was equal. The summary line had always said "answered ABSENT".
    (B) **ABSENT retained zeros** — the ABSENT answer memsets the page and
    reached `get_send`, whose `retain_put` was unconditional: zeros retained
    as "last transmitted bytes" at gen==count, installed later by fill site 5
    (retained, after ABSENT) over the live page — the "read 0" losses, and
    pg3's residual (rip 0, rsp in the thread's stack page, "on neither
    machine, served retained bytes"). (C) **ABSENT while this side's own pull
    was in flight** — `pull_wait` gives up at 500ms, the source's take-wait
    is 1000ms, so a page landing here was declared ABSENT (present=0
    backing=0 holder=guest). Fixes: `retain_put` only for a served page
    (`get_send_noretain`); the lost race answers IMMEDIATELY — INFLIGHT if a
    pull is active, else ABSENT (the asker's sibling has it; the source's
    ABSENT path waits for a sibling fetch and finishes on `ctx_present`);
    ABSENT → INFLIGHT while `pull_active`. And on the SOURCE, "a landing
    beats arriving bytes" is overridden when BOTH stamps are known and the
    arriving copy's is strictly newer (the far side's 1200ms retained-copy
    fallback can land an older copy first; `RECV-DROPPED gen=79 aux=78`
    220µs before a loss; a drop with arriving > landing was in 4 of 6
    failing pool runs and 14 of 246 passing ones) — session 24's unguarded
    form regressed pg3 because an unstamped landing reads 0, so an
    unstamped or equal landing keeps the drop. **A first version that re-ran the
    handler's decision on a lost race REGRESSED to 48/256 exit-13:** the
    re-run repeats 2.5s of waits and the source's GET socket gives 2s
    (`SO_RCVTIMEO`), "the page channel failed and did not recover", context
    ended. Do not retry inside the GET handler. **Measured (hx2 ×256,
    8-way, graded by guest.out): 240/11/5 → 246/6/4 (A+B+C) → 248/4/4 (with
    the source-side override); zero-losses 10 → 1; pg3 pool 253 → 256/256;
    suite 93/0 ×3 with hx2 and ns1 XPASS every round (both promoted).** What the LOSS-PROBE shows of the rest is a different class
    (§6.1a): the SOURCE holding a page its record says it handed over
    ("requests refused ... where this side was holding it anyway: 7.5k–12.7k"
    per failing run) and a guest writer storing through a translation to a
    folio that is no longer the object's — the split-brain/TLB class, not
    the GET handler's.

19. **The monitor's own mm carried the guest's mappings (2026-08-21,
    session 26).** tests/mf2 — a fork by a worker thread — died every run,
    and session 25 pinned it on `cow_give_page()` serving the parent's
    stack page as of the break. Page-logging that page with the word at the
    saved canary's offset (`VMR_PGLOG_OFF=0xf98`) showed the give was RIGHT
    (canary intact at the give and at the source's take 400µs later) and the
    child nevertheless died on its parent's `__run_postfork_handlers` frame,
    written after the give. A new probe, `page_provenance()`, photographed
    the child's PTE on THE SAME FOLIO as the parent's forker context while
    the child's own object held the right bytes; `/proc/<monitor>/maps` then
    showed `7ffff57f0000-7ffff77f4000 rw-s /memfd:vmctx-guest` in vmremote
    itself. The kernel's `vmctx_apply_map_rep()` handled UNMAP/ZAP for a
    task that is not the caller but ran SET through `vm_mmap()` and PROT
    through `do_mprotect_pkey()` — both on `current->mm`, the monitor's when
    fanned out by `VMCTX_CTL_APPLYMAP`. Every context is clone()d from the
    monitor without CLONE_VM, so each inherited those VMAs: harmless for a
    thread (same object), fatal for a fork child (the PARENT's object at its
    own addresses). Only a threaded parent has cross-task fan-outs, which is
    why only forks from threaded parents died. **Fix:** vmremote fans out
    only UNMAP and ZAP (a sibling maps and re-protects on its own next
    fault); kernel patch 0069 (#148) refuses SET/PROT for `t != current`
    (`vmctx_apply_map_remote_refused`); `monitor_mm_check()` scans the
    monitor's maps at every spawn ("must be 0"). **Measured:** mf2 0/N →
    PASS every run; suite 99/0 ×3 with mf2 XPASS in every round (promoted).
    The session-22 cross-task munmap accounting bug (0066) was the UNMAP
    twin of this one.

20. **The page budgets re-nested (2026-08-21, session 26, second
    change).** The hx2 pool's last no-output class was exit 13: "remote GET
    of 0x7ffff7ff6000 after 2009ms" (once 3617ms), then "the page channel
    failed and did not recover", the context ended. The budgets did not
    nest: the kernel's fault deadline was 2000 ms (outermost), the source's
    GET socket `SO_RCVTIMEO` 2 s (equal to it), and inside THAT the
    destination's GET handler may legitimately wait pull_wait 500 + claim
    1000 + take 1000 ms — and under pool load (8-way, load 60 on 16 cores)
    the middle budget overran the outer one on 2 of 256 runs. Fix: the
    kernel default goes to 6000 ms (patch 0070 / 6.18 port 0040), both
    tools raise a lower live value to 6000 at start-up through the
    core_param (`vmctx_deadline_enforce()`, user/deadline.h — vmhome.c
    cannot include common.h's vmctx_uapi.h, it carries private struct
    copies), the GET socket bound goes to 4 s, and the source's INFLIGHT
    re-ask loop is bounded by time (1.5 s; it was 400 × 500 µs = 200 ms,
    less than the 1 s take hold an in-flight page can sit behind).
    Measured: see §1.

21. **The claim storm: a crossing decided by the landing, not by a clock
    (2026-08-21, session 28).** Session 27 read the "2 s race" as a ~2500-
    yield claim volley at the pg3 join, broken by the destination's 1200 ms
    INFLIGHT grace — and that the deadline extension (§2.20) had only made
    room for the clock to fire. Session 28 kept the storm runs whole
    (`torture.sh` now keeps a passing run with yields as `storm.N`) and the
    source's own lines named it: `CLAIMING 0x7ffff7ff6000 for context B
    (state set at vmhome.c:2903, here=1)` ×2475, and the sweep's "recorded
    as being claimed by vmhome.c:2903 and is present here as well — both
    machines are holding it". Line 2903 is the fault service's
    `pg_set(PG_CLAIM)` before its GET. Two source contexts fault on B's
    stack page at the join; the first's pull LANDS it; the second's GET
    reaches a destination whose record (correctly) says "the asker holds
    it" and answers INFLIGHT; the second sits in `pg_get_op`'s re-ask loop
    for the whole grace with the page marked CLAIM — so every guest pull
    for a page the source already holds is refused CLAIMING. The "landing
    beats arriving bytes" rule then dropped the answer anyway, after the
    storm. **Fix (vmhome.c, no kernel, no protocol):** the landing decides
    on the source side too — an absence fault whose page is present before
    the ask leaves is answered DONE without asking
    (`n_claim_landed_early`), and `pg_get_op` checks `ctx_present()` on
    every INFLIGHT answer and returns `PG_GOT_LANDED` the moment a sibling
    has landed the page (`n_claim_landed_inflight`); the claim ends, the
    guest's next pull is served from the page this side holds. Protection
    faults pass 0 (their page is present by nature). **Baseline on #148
    (pg3 ×256, 8-way): 10 full storms (2412–2553 yields + 1 give-up each),
    35 runs with a 2-yield blip, 5 failures all exit 242 at teardown
    (fault at 0x8 write / 0x0 ifetch: a zeroed list/function pointer, no
    ABSENT-fill, all crossing counters 0 — the §6.1a class).** With the
    fix, on #149: pg3 ×256 **256/256, 0 storms** (max 1 yield in 8 runs, 0
    give-ups), 7 runs ended a claim on a landing, the pool 1086 s vs
    1250 s. The 1200 ms grace stays for the dead-token case (sig1's
    teardown); it is no longer what decides a live crossing.
    **Then the hx2 pool regressed, 250 → 235/256 (20 FAIL 1, 1 no-output),
    with the first form of the fix** — ending the claim on ANY INFLIGHT
    answer. In hx2 that check fires 30–50× per run (no storm anywhere), and
    the pre-existing "arriving newer than the landing, installed over it"
    override was in 16 of the 20 failing runs against 32 of 235 passing:
    the short-circuit suppresses exactly the fetch that override needs.
    The destination knows WHY it answers INFLIGHT — its own pull of the
    page is landing there (a fresh take follows: keep asking) or only its
    young-episode clock holds the answer (the retained copy follows, never
    newer than a landing the asker has) — so the INFLIGHT reply now says
    which (`vmr_pgrsp.gen = VMR_INFLIGHT_PULL` / 0) and the source ends its
    claim only on the clock kind. pg3's storm is the clock kind (its pulls
    are answered CLAIMING and ended within the RTT). Measurement arms
    `VMHOME_LANDING=off|grace|all`, `VMHOME_LANDING_EARLY=0|1` (delete once
    settled). A/B on #149, hx2 ×256 8-way, graded by guest.out: **off/0
    (control) 247 pass / 7 FAIL / 2 no-output** (session 26's band on #148:
    250/6/0 — so #149 is not the regressor); all/1 (first form) 235/20/1;
    grace/1 243/13/0 — still short, with the clock-kind landing firing
    ~130× per run. The missing guard is the STAMP: the source-side "landing
    beats arriving bytes" rule drops an arrival only after comparing stamps
    ("arriving newer than the landing, installed over it" heals the §6.1a
    split brain, where this side holds a stale present copy while the guest
    writes), and skipping the fetch skips that. So the INFLIGHT reply now
    carries the destination's current capture count (`vmr_pgrsp.gen`) with
    the reason in `vmr_pgrsp.why`, and the source ends its claim on a
    landing only when `why` is the clock AND its landing's stamp is not
    behind that count (nothing captured since the landing; the storm's
    case, where the guest cannot write while refused). A landing behind the
    count keeps asking (`n_claim_landing_behind`). The pre-ask early check
    has no such guard and is off by default — measured: grace/0 **246/9/1**,
    the control band, so the early check was the cost. Final form (stamp
    guard + early off): **249/5/2** — the session's best, in session 26's
    band (250/6/0); 14 clock-kind INFLIGHTs with a present landing behind
    the count kept asking.

22. **TAKEOBJ re-reads the folio after the punch; 256 mm slots (kernel
    #149, patch 0071, session 28).** The instrument §6.1a asked for: after
    the unmap, the kick, the copy and the punch — with the folio still ours
    by reference and lock — TAKEOBJ re-reads it against a kernel copy of
    what it handed over. A difference is a guest that stored through a
    translation the kick did not retire, counted
    (`vmctx_takeobj_stale_store`), named in dmesg (offset, old/new word,
    cpu, pid), and the NEWER bytes are re-copied to the caller
    (`_stale_recopied`; three re-reads, then `_stale_unresolved`) — a
    detector that also repairs. `VMCTX_MM_SLOTS` 64 → 256: the table had
    overflowed once on the live box (`vmctx_mm_overflow=1`), and an
    overflowed mm has every fault hook fall through (silent invention).
    Also carried into session 28 from the #148 counters: `vmctx_take_lost=2`
    (the must-stay-zero take precheck miss) did NOT recur across the 256-run
    baseline pool with dmesg streamed — no capture yet.

## 3. Refuted designs — measured worse, do not retry

Each row was built, measured, and reverted or removed. The numbers are the
reason; retrying one without new evidence repeats a paid experiment.

| design | measurement that killed it |
|---|---|
| Conserved-token serialiser on the source (VMHOME_PGTOKEN) | suite 81/0 → 63/18 (sm2/sm3/an1/cow1/pg2/ex4/pg3/pf3 — exactly the page-motion cases); ws1 4/40 fails → 5/22. The CLAIMING retry storm costs more than the two-monitor deadlock it removes. The destination's yield half is unconditional and stays. |
| Source-side preemption of its own transients | 63/18 — the transient may be another thread of this process mid-hand-off; stealing it duplicates or drops the token |
| 200ms serve-yield (answer CLAIMING instead of waiting out a held call) | pg3 96/96 → 42/48; netsurf SLOW-stall count UP (9 vs 2) — the yield-refault dance churns against hold states the 1s wait outlasts; the dest retries the whole ask so total stall is conserved |
| Retained copy served to a LIVE address space | THE pg3 one-behind itself: the retained line sits in 6/7 failing unarmed runs vs 1/8 passing |
| INFLIGHT with any time cap | 50ms-gap 47/48, 50ms-page 40/48, 50ms-serve-ended 41/48, 1200ms cap 44/48 — every arm whose cap could fire put the retained line back into the failing runs. Only pull-active-or-grace (no cap while a landing is in flight) reached 96/96 |
| INFLIGHT with NO fallback ever (no-cap even with no landing coming) | pg3 96/96 but sig1 hung (evaporated token: the bytes died with a context mid-service; no landing is coming and the retained copy is the newest that exists) |
| VMR_PG_STOP (source pushes "stop" to the dest) | 982/1024 vs 991/1024 control, twice; the extra failures were WRONG ANSWERS (exit 1) — a killed context never sends FINISH, so clear-tid never runs and a joiner waits forever |
| GONE answered as "fault served" (return 1) | the kernel resumed the guest on a page nobody installed: 3 lost increments/512 vs 0. The shipped form signals, then WAITS for the task to be dead before returning |
| Teardown reclaim, three designs | one enumerated the HOST monitor's /proc maps, not the guest's; one assumed COW-private pages (cowbreaks=0 in the 98 runs refuted it); loan-reclaim of a dying context measured worse — the wrong unit (the pages that matter are the survivors') |
| Eager fork copy (whole-object, then lazy per-page + write-protect) | two physical pages for one address; the lazy form was the same defect in slower motion. The child's object starts empty |
| COW break via monitor-side madvise(MADV_POPULATE_WRITE) | 2s mutual-timeout stalls (the context's own fault waits on the machine blocked on the take); four repair forms each cost ≥ what they saved (own thread → EIO; +lock → exit 124; POKE own bytes → 4 lost increments/512; MAPOBJ → 50 vs 8 deaths, foreign-fault inventions). Kernel #127+#0051 break exactly the mapcount>1 share at the take's own refusal; everything else clears on -EBUSY retry |
| Broad BREAKCOW (break on every take) | take-COW-break is §23-closed: break ONLY the genuine share; ws1's other refusals are refcount transients |
| Correcting the both-holder record from the sweep (SWEEP_FIX) | pass rate unchanged — by the time the sweep sees the pair, both machines really hold the page; the record is the symptom |
| Take-vs-sibling-fetch exclusion (TAKE_EXCL) | never fired: n_take_waited 0 in 12/12, pass rate 4/12 vs 6/12 (noise). The race is not the producer |
| Lend-on-ABSENT for a guest-held page (VMR_ABSENT_LEND) | recording a loan for a page this side has LOST writes fiction into the loan table; the record is left saying "lost", loudly |
| vmctx_foreign_file_refuse | fails rd1 3/3 — a hole in the object is also how a held-but-not-resident page is read back |
| DMA-completion hoist (wait for pinned pages before the take) | measured worse; the hold's -EBUSY wait is the design |
| take_drain_always (lru_add_drain_all on every take) | 10 timeouts either way in 1152/arm — not the mechanism |
| fault_deadline 8000ms | halves the timeout class without removing it — a mitigation that hides the defect; deliberately not enabled |
| flush_reassert (force tlb_ctl/clean every entry) | its A/B was VOID — the standing build-time tlb_ctl=1 made both arms the same code; the "~3x fewer take-stale" was noise. Superseded by the gen re-arm; knob removed |
| kick_enable off (A/B) | identical (20/24 vs 20/24 same boot; 3 vs 3 repeats per 2048) — the kick is kept ON as ordering correctness, not as a tuning knob; switch removed |
| Guard "re-fault if any sibling has the page live" before zeroing | fired ZERO times in twelve runs that still invented a page — a check that never fires is dead code that reads like a safeguard |
| Abandon the take's retry loop when this side starts claiming | 0 firings in ~2300 runs — the retry loop is not what waits |
| Believing an ABSENT for a handed-over page | zero-fills live memory (DECLINE invents zeros). PG_GONE + ABSENT = re-ask 40×, then a named, counted KILL — never zeros |
| EFAULT for "nothing served, mapped here" | each answer kills a different program; ABSENT + TRYFAULT is the shipped rule (a VMA is not permission, but it is also not a wall) |

## 4. Void measurements, instrument lessons, and removed switches

### 4.1 Measurements that were retracted

- **The flush_reassert A/B** — both arms were the same code (§3).
- **The "VMEXIT-scan stale guest PTE"** — an instrument race; the sound
  check found 0 orphans (valid instrument form is in kernel #91's notes).
- **The teardown-race /proc photograph** — the stack line read as "faulting,
  nobody takes it" had `vmctx_redirect_syscall` under it: a context waiting
  correctly inside a forwarded futex. `ctx_route_del()` was never needed.
- **The 21-vs-15 kick A/B (thirteenth session)** — a different boot; did not
  replicate on one boot (20/24 vs 20/24).
- **`vmctx_af_noretry = 61264` as a lock story** — 1931 stack samples had
  not one rwsem/mmap_lock/take frame. Check a lock story against a stack
  before rebuilding a kernel for it.
- **Every A/B ever quoted for a dead switch** — `fake_share`, `prot_pull`
  were assigned and never read; the 2s page-channel timeout had no override;
  and (this pass) any "VMHOME_ENV arm" launched through the launcher scripts
  measured the default, because the env was never delivered.

### 4.2 Instrument lessons (each cost a diagnosis)

- **"alive" is not a state; photograph the task.** `kill(pid, 0)` is true
  for a task in do_exit and for a zombie; what decided session 40 was
  `/proc/<pid>/stat`'s state letter + `wchan` (D in `__synchronize_srcu` =
  the mmu-notifier unregister inside vmctx_finish = the whole group is
  exiting) read at the moment the ctl failed (`task_photo()`).
- **A signal's sender is in the `signal:signal_generate` tracepoint**
  (`/sys/kernel/tracing/events/signal/`), not in any log: it records the
  sending task's comm/pid, and `res=1` means the group was already dying.
  The zap in a core dump / `do_group_exit` sets SIGKILL directly and is NOT
  traced. `kernel.print-fatal-signals` names only the victim.
- **The last-context wait status is a racing read:** two conn threads call
  `oracle_put` at the end, so "the service process had ALREADY ended: killed
  by signal 9" there is vmhome's own SIGKILL seen by the loser. It looked
  like an early kill for an hour.
- **The runner was in the clock, and the runner is slow here.** Every
  pre-session-40 wall (perf.sh, `=== exit N in Xs`) enclosed ps/pgrep/kill
  and vmhome's start+poll, and this box's `/usr/bin/timeout` is uutils
  0.8.0 (a 100 ms child poll: `timeout true` = 114 ms), `env`/`date`/`cut`/
  `sleep` the same 10 MB binary at ~10 ms an exec. wc read 0.22 s for a
  0.046 s vmremote. Clock with `$EPOCHREALTIME`; never put a coreutils
  exec inside a measured interval on this box; read vmremote's own `time:`
  line beside any wall.
- **Never land a page into a RUNNING context.** `VMCTX_CTL_LAND`'s
  IF_ABSENT check and its write are two steps; every pull path lands while
  the context is blocked on the monitor. The readahead that landed after
  the RESUME, to overlap the context's copy, overwrote a page the call had
  just written once in ~20000 landings -- gzip's buffer page one block
  stale, 1 run in 7, found by decompressing the bad output and `cmp -l`
  against the input (every differing byte in one file page; page/8 is the
  read, page%8 the buffer page). Land before the RESUME, always.
- **Pipelining pays only where the wire is the cost.** Measure the round
  trip before reading ahead: on this box a GET costs 40 us and a page of
  take+wire+land ~35 us, and both readaheads measured neutral-to-negative
  here (sha +40%) while they are the right design for a LAN. The gate is
  the measurement (`RA_MIN_RTT_US`), not a build flag.
- **A coredump signal waits for a return to user mode.** `kill(pid,
  SIGSEGV)` on a process whose threads all sit in `wait_event_killable`
  delivers when one of them next completes a syscall -- twelve seconds
  later in hx2 -- so the cause and the death are far apart in every log.
- **An instrument can manufacture what it reports:** VMHOME_PGLOG writes to
  a file on the take path and widened the window it was watching — 130
  both-holder sightings with the log on, 0 with it off. The sweep (a
  counter) replaced it.
- **Armed pools mask pg3:** VMR_CLOBBER's page_read_as per install
  serializes the exact window (167/168 armed was the INSTRUMENT passing);
  PGLOG alone also masks (48/48). Only unarmed pools measure pg3.
- **A confident zero usually means the check never ran** — prove an
  instrument can fire (selftest knobs: bp_selftest, decl_selftest,
  clobber's seeding path) before believing its silence.
- **The high-water mark cannot see "came back equal":** a take that returns
  exactly the value a previous take captured passes `n < hw`. The content
  HISTORY (VMR_HIST, before-value comparison) is the sound form.
- **A fixed test port faked failures in every past A/B** (phantom runs);
  **a stale artefact grades the previous run** (rate.sh as non-root wrote
  nothing and re-graded old remote.logs: "40/40 futex-hang" in 2s).
- **A bare `=0` grep convicts passing runs** (interleaved stderr); anchor
  patterns to a field.
- **Build the control arm by removing ONLY the hunk under test** — a
  `git stash` control also removed the instrument (arm B could not be
  convicted at all); check `strings` for the instrument's survival.
- **A knob left at its experimental value poisons everything after:** a
  killed script left `vmctx_fault_deadline_ms=0`; check the parameter
  (should be 2000) before believing a round.
- **browser-run.sh screenshotted on window-count INCREASE** — always the
  unpainted instant; anything measured with the old harness under-reports
  rendering. And vmhome's backtrace wrote unprefixed lines that the harness
  filed under "what the program said".
- **The kernel ring holds ~24 ws1 runs** — read at the end it has always
  overwritten the interesting one; stream `dmesg --follow` during pools.
- **Line-numbered log tags go stale** — the SERVED-BY tags now carry site
  names only; a tag that names a line number is wrong within a week.
- **The module's `trace_zero` probe MANUFACTURES the zero page it reports
  (2026-08-21):** it reads the guest page with `copy_from_user()` — a
  faulting read — so on a take's punched hole `shmem_fault()` allocates a
  zero folio in the object and the guest runs on it. Armed on hx2's page,
  every run of a 256-pool failed with 3–6 losses per writer, 14,989 "hits",
  and writers 100× faster (coherence gone). Leave it 0; any kernel-side
  read of a guest page must be FOLL_NOFAULT (PEEK is; `decl_probe`'s
  `vmctx_read_guest_page` must be checked the same way before its numbers
  are believed).

### 4.3 Switches removed in the tidy pass (2026-08-19)

All were proven defaults whose off-arm restored a known defect, or dead
experiments. Behavior is unchanged; the numbers live in comments at each
site. vmhome: ABSENT_WAIT, SHFILE_OWN (rd1 0/2→2/2), SHFILE_SCAN,
ABSENT_MAPPED, FILE_LOCAL, FORGET_UNMAP, ABSENT_ZERO, SHFILE_SYNC,
KEEP_PAGES, TAKE_EXCL, SWEEP_FIX, PGTOKEN (with the whole source-side token
machinery; pgstate.h claim/settle survives on the destination where it is
unconditional). vmremote: VMCTX_NO_COHERENT, VMR_ABSENT_LEND,
VMREMOTE_INVENT_FATAL, VMREMOTE_NO_DELIVER_EXCEPTIONS, VMREMOTE_FORK_SNAPSHOT
(dead — the copy it gated was already gone). Module: flush_reassert,
kick_enable. What remains is instruments only — the inventory with purposes
is AUDIT.md §8.

## 5. Operational rules (each paid for)

**Machines.** .229 = AMD source (`biwu@`, key `config/id_vmctx`; sudo needs
`SUDO_ASKPASS=$HOME/askpass.sh`, pass `biwu`); .30 = Intel dest (`root@`,
netboot — NO UNDO: keep a known-good vmlinuz/modloop aside, pre-flight in
qemu, kernel and modules ship TOGETHER or MODVERSIONS bricks the box); home
(WSL2) can never be a source (no vmctx kernel). vmhome needs sudo on the
box it runs on. Kernel changes and reboots of .229 are authorized. One
machine operation at a time — never overlap build/install/reboot.

**Building.** `sync-amd.sh` is the only deployment path (deletes binaries
before building — rsync preserves mtimes and make would skip; `MODULE=1`
also rebuilds the .ko; a plain `make` in the box's kernel dir deploys the
wrong .ko). `verify-build.sh amd|intel` proves SRCID, vermagic, syscall
numbers, module loaded — but it checks vermagic, NOT content: after a module
rebuild it says ok while the old module is still loaded. **rmmod before
measuring any module change** (torture.sh/local-here.sh only
`lsmod || insmod` — they never reload). Kernel patches under
`kernel-patches/` are the ONLY record of core-kernel changes; src/ is
untracked and drifts.

**Running.** The suite MUST run under sudo (as a user it fails 84/84 with
runany exit 3, and a sudo browser run leaves root-owned test binaries that
break the next user build). Over ssh: `setsid` + `</dev/null`. Check before
believing any round: D-state leftovers on both boxes
(`ps -eo pid,stat,comm | awk '$2~/D/'`), module refcount, stale vmremote
processes, `vmctx_fault_deadline_ms` = 2000. Kill monitors BY PID — `pkill
-f` matches the ssh session's own command line and kills it (use `pkill -x
vmremote-local`, `pkill -x vmhome`). Runs are not exclusive: a pool is a
different load and a stress amplifier — compare pool against pool, arms
alternating in ONE pool (`ARM_A=... ARM_B=... prun.sh`), never pool against
serial. 1024 runs at ~2% has σ≈4.5: 998 vs 1008 is one arm. VMCTX_SHARED=1
skips sweep/reload/rebuild — use it for repeated rounds on one build only.

**Pool output lives on DISK, never on /tmp.** .229's /tmp is a 7.3 GB tmpfs;
a pg3 run writes ~12 MB of logs and a trail-armed hx2 pool ~1.5 GB, so two
archived pools plus a running one fill it — at which point every further run
dies instantly with empty output and torture.sh files it under `other`
(measured 2026-08-21: a pg3 pool "111 pass, 144 other", the 144 all
zero-byte run.logs from the minute /tmp filled). Run pools with
`OUTD=/home/biwu/torture.<case>` and archive under
`/home/biwu/torture-archive/`; `df /tmp` before believing a pool.

**Measuring.** The cpufreq governor is part of the measurement: schedutil
reads a forwarded-syscall storm (every hop a sleep) as an idle box and
clocks the cores to their floor — the pre-spin getppid loop measured
42–114 µs/call under schedutil and 35–40 under `performance`, one boot,
arms alternating (session 46). `perf.sh` prints the governor and
`vmctx_spin_us` in its header; `GOV=performance` pins it for the run.
Since kernel #181 the spins keep the cores busy and schedutil mostly
follows (16 vs 15 µs/call), but a table without its governor is not
comparable to another. Variance IS a race — audit locks first, never
average it away. Timeouts are diagnoses. Exceptions route to the owner and are reported
raw (a signal is frame+regs). Memory problems get memory-level fixes — never
a syscall-specific patch, never address-class special cases (owner's rule).
Every wait is bounded; every counter prints whatever its value; a counter
shown only when non-zero cannot state "it never happened".

**Committing.** Squash everything after origin/master into ONE commit,
author AND committer `Bi Wu <biwu85@gmail.com>`.

**Graders.** ex4 prints THREAD-OK and th9 prints DONE, not PASS — prun.sh's
generic `^PASS$` grep grades both 0-for-N (measured: th9 0/32 by prun,
32/32 by suite.sh's predicate on the same runs). Use suite.sh's own
predicates. runany exit 3 = no output (not a test verdict). torture.sh's
`other rc=1` is vmhome's own exit after a worker's 242, not ws1's FAIL
printf.

## 6. Open frontiers, in order

### 6.1 The netsurf wall

**Native renders (control): the vmctx crash is OUR injected corruption.**
`native.sh` (no vmctx, same box/Xvfb/URL) maps a 1000x700 "AVM - NetSurf"
window and fetches page.html. So every vmctx death below is a defect we inject.

**FIXED (2026-08-20, kernel #147, patch 0068) — the dynamic loader's
boundary page.** netsurf died 7–9s in ld.so's relocation of a freshly
dlopened library. tests/dl1 reduces it: the same file page mapped twice
(the tail of one PT_LOAD and the head of the next — a real ELF boundary
page), read (the addend), written (the relocation), pumped through
forwarded syscalls, remapped fresh each round as every dlopen does.

**Root cause (and the red herring that hid it).** The VACATE-LATE +
FAULT-IN-VACATED instruments fire every round, which pointed at an
ordering bug — but they are NOT the producer (the instrument even
self-describes as 'not a verdict'). Two scratch variants isolated it:
dl1a (double-map + forwarded write, NO remap) PASSES → coherence is fine;
dl1b (remap each round, guest store only, NO forwarded write) DIES → the
remap of the doubly-mapped file page is the trigger, and its dmesg is
'TAKE claim refused AFTER the zap: refs=3 want=2 mapcount=1 anon=0' every
round with cow_broken counting up. The real bug was in vmctx_ctl_take:
after vmctx_break_cow_share COW-breaks the boundary page to a fresh
private ANON folio, page/folio still named the OLD file page-cache folio
(refs pinned by the cache and by the read-only text alias, anon=0), so
the re-precheck AND the zap/claim acted on a folio the address no longer
mapped → refused forever → 2s deadline (131), or when a forwarded write
had landed, the pull served pristine file bytes over it (wrong data,
FAIL). Fix: re-run get_user_pages_remote after a successful break so
page/folio/vma name the fresh anon copy. **dl1: FAIL 199/200 → PASS
200/200; after-zap refusals in a run 618 → 0; dl1 torture 96/96 (19104
clean COW-breaks); suite 90/0 ×3.** No change-log/protocol touch — the
ordered-change-log design in earlier commits was never built and was the
wrong fix. **Browser payoff: netsurf on #147 runs 21.6s (was 7–9s) and
maps its window; the next wall is heap-metadata corruption, a different
(coherence) class — see the Browsers bullet in §1.**

**THE NEXT WALL, reproduced (2026-08-20): glibc heap-metadata corruption,
tests/hp2.** netsurf's `malloc(): mismatching next->prev_size` is chunk
metadata served stale/invented. hp2 (four threads: malloc/free churn +
forwarded syscalls on the heap buffers) is CLEAN as a single run but
under the pool (8×, the pg3 stress) dies ~6/64, and the deaths name the
mechanism: glibc's multi-arena growth does a large munmap (an ~18MB
VACATE ending at the 0x7ffff0000000 arena base), then a fault on a page
INSIDE that re-used range is answered ABSENT by the source ("the take
found nothing and the pagemap agrees") and the destination fills fresh
ZEROS (DECLINED-BY one-page-written-into-the-object). Zeros over live
arena memory is exactly how a chunk header becomes mismatching
prev_size. This is the SAME stale/invented family as the died-242 and
the pg2 startup ABSENT-fill (§2.15–2.16, §6.3): the source answering
ABSENT for a page that has been re-mapped/re-used, and the destination
inventing zeros rather than re-asking. hp2 is the fast gate (run pooled;
clean serially, so not in the suite — like ws1).

**Two surgical hypotheses RULED OUT (do not re-walk):** (1) the dlopen
take-refusal — FIXED at #147/0068; hp2's deaths are no longer take
refusals (0 after-zap). (2) the fill_site-6 lost-store — a page
zero-filled that this side had installed and nothing recently unmapped.
A broad detector was built for exactly this (`n_lost_store_zeroed` =
page_is_installed && !vac_recent_covers, commit bc1806d) and ran CLEAN
on an hp2 pool (0 hits, 3 died), so the fill_site-6 zeros are NOT the
producer — they land on genuine first-touch / recently-vacated pages,
correctly. **What the deaths actually show:** `DECLINED-BY one-page-
written-into-the-object 0x7fffeffff000 (fault 0x7fffeffffcd4)` — a write
to a WILD address inside glibc's trimmed-away arena head (unmapped
natively → native SIGSEGV; under vmctx the DECLINE zero-fills it and the
write "succeeds"), the DOWNSTREAM symptom of a corrupted pointer, not the
root. **The root is upstream and is a STALE serve (not a zero-fill — the
detector proves that): the arena header served one write behind.**

**PRODUCER NAMED (2026-08-20, the arena content-history instrument,
commits 9053054+48540a5).** The single-page VMR_HIST was generalized to a
hashed table that AUTO-ARMS on a signature not an address: secondary
glibc arenas are 64 MiB (HEAP_MAX_SIZE) aligned, so `hist_auto()` watches
the first 8 pages of every 64 MiB-aligned page in the mmap region;
`VMR_HIST_ARENA=1` turns it on (off by default). Proven to fire (single
hp2: 32 pages watched, 2555 checks, 0 reverts — clean). Run on NETSURF
(which reproduces the real corruption; hp2's POOL deaths are a separate
early channel/arena-startup bug), it printed STALESERVE on a 64 MiB-
aligned secondary arena 0x7fffec000000:
  - site80 0x7fffec000000 +48: a counter 0x200000002 → 0x200000000
  - site31 0x7fffec005000 +792: a POINTER 0x7fffec04c850 → 0x0
  - site80 +2648/+3664: path strings reverting to zero/garbage
**site80 is serve_object_mapped** — the 'map the object's folio' path
(hx2's own fix) — handing back an object folio ONE WRITE BEHIND the
context's live arena page. A pointer served as 0 that way is exactly the
null/wild deref glibc dies on. The premise 'this AS holds a live folio,
so the object IS that folio' holds for hx2's fixed layout but BREAKS for
an arena page taken to the source and re-served under churn: the object
folio and the folio the guest now writes are two physical pages, and
serve_object_mapped maps the stale one.

**Why there is NO safe narrow fix at serve_object_mapped (analyzed
2026-08-20; do not ship the obvious gate).** serve_object_mapped
legitimately maps the object in THREE cases and only one is the bug:
(1) hx2/ws1 — a sibling holds the page live via the object folio (MAP_
SHARED) → correct; (2) the shmat/give-back SOLE-HOLDER case — the object
is the only copy, NO context present → correct (the comment records
'aborting here killed netsurf 4.9s in, over bytes sitting in the
object'); (3) the arena stale-leftover — object has an OLD copy → serves
stale (the bug). The obvious gate `&& as_any_present()` would make (2)
refuse — regressing the shmat case — so it is UNSAFE. Distinguishing (2)
current-sole-holder from (3) stale-leftover needs to know whether the
object's copy is current, which neither backing_has nor as_any_present
can tell — it needs a GENERATION stamp on the object page.
**And even a correct serve_object_mapped gate would not fix it**: the
instrument named the stale serve at BOTH site80 (object) AND site31 (the
SOURCE's pulled bytes), so the guest's write was captured by a take and
reverted in STORAGE — the root is the upstream hand-over/persistence race
(the same family the landing-beats-arriving-bytes fix, 053861a, started),
not the object serve. Fixing site80 alone just moves the staleness to
site31. **The real fix is the generation stamp** (per-(as,page) take
generation, so a serve can refuse any copy older than the last captured
take — object OR source), a substantial change to the coherence core;
build it against hx2 + ws1 + pg3 + the arena content-history, never
speculatively. Tools ready: hp2, the LOST-STORE detector, VMR_HIST_ARENA
(all committed).

**THE GENERATION STAMP — BUILT AND IT KILLS THE STALE-SERVE FAMILY
(2026-08-21, session 24). Protocol VMR2→VMR3, VMPG→VMPH.** A per-(address
space, page) count on the DESTINATION of how many times it has CAPTURED the
page — `pgen_bump()` on every take-away, take-and-put-back and write-grant.
Every copy that leaves the destination is stamped with the count as it stood
(`retain_ent.gen`, `vmr_pgrsp.gen`, `vmr_rsp.pggen[16]`); the source keeps
the stamp beside its copy (`pgown_ent.gen`), never bumps it (the destination's
clock alone), and echoes it on every serve; `pull_gen_check()` refuses any
served copy whose stamp is below the current count and installs the RETAINED
copy (whose gen == count) instead. Reset with the mapping (`pgen_forget` in
range_vacated + exec_wipe, the twin of pgown_forget_range). The install
paths stamp the landed copy (`pg_gen_set`) so the destination can judge it
later. (A GENERATION override of pg3's source-side landing decision was TRIED
and REVERTED the same session: replacing "a landing beats arriving bytes" with
"install the arriving copy when its stamp looks newer" regressed pg3 to 10/232
in an 8-way pool — the landing's pg_gen is not reliably set, so a landing via
a path without `pg_gen_set` reads 0 and every arriving copy looked newer and
was wrongly installed. The stamp's job is the DESTINATION's `pull_gen_check`;
the source-side landing race is pg3's and keeps its proven 053861a heuristic.)
`VMR_GEN_REPAIR=0` keeps the check, disables the repair (measurement arm;
delete once proved). **MEASURED on netsurf: 37 STALE
serves caught (all site33, the chunk pull of the main stack 0x7fffffffe000),
all repaired from the retained copy; a later clean run: 11101 pages checked,
0 stale.** The two content trails (`trail_note`/`trail_dump` on the dest,
`strail_note`/`strail_dump` + `VMR_OP_STALETRAIL` on the source) name a stale
serve's producer on BOTH timelines at once, no armed page needed.

**AND THE WALL MOVED — the exit-242 was TWO defects, and the second is the
VACATE ordering edge (FIXED, session 24).** With the gen stamp clean, netsurf
still died 242 at ~8s on a guest write into 0x7ffff048a000-0x7ffff049c000
answered -14. Root: an mmap MAP_FIXED (ld.so placing a segment into its own
PROT_NONE reservation) UNMAPS+MAPS its range in one syscall; the kernel
mm-hook logs the unmap as a vacate; a CONCURRENT thread's forwarded call
drains that vacate and ships it on a reply that lands AFTER the mmap's own
construction — punching and unmapping the live segment. A drain-time sequence
number cannot order it (the self-vacate is drained later than the mmap's own
reply stamp, so it outranks its own construction). **Fix = source-side
self-map suppression** (`srccons` ring + `vacate_emit` in vmhome): a
MAP_FIXED/MREMAP_FIXED names its target range in its ARGS, so the range is
recorded BEFORE the syscall runs — before the hook fires, before any drain
sees the vacate — and every drain drops the vacate portion covered by a live
record; the record is released after the call's own drain, so a genuine later
munmap of the same range is not suppressed. Deterministic, no kernel change.
(A remote seq machinery — `vmr_rsp.mmseq`, `cons_note`, `range_vacated_seq` —
was also added but fires 0× because of the drain-time seq problem; harmless,
may catch genuine cross-range reorders.) **MEASURED: netsurf 8s→24s, MAPS ITS
WINDOW, 0 unrecoverable faults, 85 of 87 self-vacates dropped; the death moved
to a LATER wall — a thread SIGKILLed at ~24s (guest exit -1, first bad status
137).** CAVEAT: pure suppression drops the self-vacate's cleanup
(obj_forget/retain_forget/pgen_forget/APPLYMAP); safe for ld.so's PROT_NONE
reservation (nothing written there), but for a MAP_FIXED OVER LIVE WRITTEN
DATA it could leave stale object pages — if a mmap-heavy suite case regresses,
make the construction self-cleaning (a range_reset_for_construction in
reply_note_map's SET paths). **The ~24s thread death (exit 137) did not recur on the session-25 pair
(§2.18):** netsurf ran the whole 120s harness limit (killed by it, exit 143;
the one "unrecoverable #PF" in dmesg is at teardown, after "monitor gone"),
mapped BOTH its windows — the 10x10 and the 1000x700 "netsurf" main window
(session 24 saw only the 10x10) — but `http.log` is EMPTY: it never issued the
page request, and the root's colour count did not move (767 → 767). **The
wall is now a pre-fetch stall, not a death:** the session-21 assisted-futex /
blocking-call class (the "paint block" below), to be read from the
STILL-WAITING entries and the futex lines of a longer run (browser-run.sh
default LIMIT=180; `OUT=/home/biwu/browser-s25` holds this run).

**Three death modes, all ~28-37s / ~5.6k forwarded syscalls, each a distinct
mechanism:**

- **131 — a coh_lock held 1.18s across a blocking chunk-fetch. FIXED.** The
  coh_watch photograph caught `fault_from_home_inner` blocked in a socket read
  while holding coh_lock; the SLOW report named it "the locking defect". The
  source's serve-take of a page held by an in-flight syscall is refused for a
  full second (the pg3-load-bearing 1s wait) before it answers CLAIMING, so the
  dest's `home_call(VMR_OP_CTXPAGE)` blocked that whole second WITH coh_lock
  held, queuing every other guest fault into the kernel's 2s deadline. Fix:
  `ctxpage_fetch()` wraps the four chunk-fetch home round trips in
  `coh_drop_all()`/`coh_restore()` (the proven pull-path pattern; the install
  code re-validates object state after). Measured: SLOW coh_lock 0 in 4/4
  netsurf runs (was firing repeatedly), the 131 death mode gone, suite 84/0.
  The fix is on the DEST lock, NOT the source's 1s wait, because that wait is
  load-bearing for pg3 (the 200ms yield was measured 96/96->42/48).

- **242 — heap-pointer corruption (STANDING WALL).** The stack at death is a
  textbook infinite recursion: a repeating 0x20-byte frame
  `{arg=0x7ffff0089010 (the 128MB glibc arena base +0x10), arg, saved-fp,
  ret=0x7ffff6352195}`, frame pointers marching up. A corrupted function
  pointer sent execution INTO the arena (rwx heap-as-code) and it recursed
  until the 8MB stack (0x7fffff7fe000-0x7ffffffde000) exhausted; the push past
  the rlimit faults at 0x7fffff7feff8 err 0x6 and the source correctly rules
  SIGSEGV. This is the symptom; the wrong pointer was written to a heap word
  earlier. NOT a single hot page (unlike hx1), so a single-page STALESERVE/HIST
  arm cannot catch it.

- **141 — SIGPIPE** (the X connection dies), a downstream of the same
  corruption reaching the X protocol stream.

**The DECLINE-as-fresh suspect, re-measured:** the runs that die 242/141 had
ZERO DECLINED-BY lines (so it is not the sole producer), but the 131 run had
MANY "one-page-written-into-the-object ... fresh memory: zeros" declines on
arena pages 0x7ffff4xxx -- still a candidate for the 242 heap-zeroing.

**Next instrument for the 242 (not built):** the corruption is a wrong value on
an unknown heap word, so a broad detector is needed. Two options: (a) a cheap
per-page rolling-checksum stale-serve check (O(1) memory/page, catches a page
whose served sum reverts to a previously-served one); (b) map guest heap pages
rw (not rwx) so the wild JUMP faults immediately at the corruption's
consequence -- caller frame intact, register state naming what jumped -- rather
than 8MB of recursion later (invasive: the guest's own text shares the object
mapping, so this needs per-range prot, not a blanket change).

### 6.1-historic The earlier netsurf readings (exit 242 at ~27–33s)

The seventeenth-session shape: window maps, then an EXEC fetch (err 0x14) of
a non-present page inside a known ~196KB rwx region, the source answering
-14, eight asks, exit 242 — a function pointer corrupted EARLIER on some
heap page, the wild call the symptom later. The "DECLINED … fresh memory: a
page of zeros" cluster minutes earlier was the standing suspect.

**Re-measured on the final #138+INFLIGHT pair (2026-08-19, four runs):**
three runs die BYTE-IDENTICALLY — a WRITE fault (err 0x6) at
0x7fffff7feff8, the 8MiB stack-rlimit boundary, i.e. the wild-call →
repeating-frames → stack-exhaustion end of the same family — at ~5.6k
forwarded syscalls / ~33s; one run dies 141 (SIGPIPE) at 31s. And **all four
runs had ZERO DECLINED-BY lines**: the DECLINE-as-fresh suspect does not
precede the current deaths, so it cannot be the (only) producer on this
pair. The new determinism (same fault address, syscall count within ±2%)
makes the corruption reproducible — instrument the corrupted pointer's page,
not the death.

**The next instrument (designed, not built):** content-history on the
DECLINE-as-fresh path. The two vmremote EMPTY paths (fill_site 5/6 in the
no-region arm, and the region-arm twin) already check `retain_get` before
zeroing and mark what they zero with `retain_put`; the kernel-side DECLINE
(`declined_at` → the guest kernel's do_anonymous_page) is the arm whose
"fresh" claim is unverified. Record the claim at the DECLINE; when any later
serve/take of that page carries non-zero bytes the guest never wrote through
the protocol, the claim was false and the page is named. A counter plus one
page of state, not a print. Start by arming VMR_CLOBBER on the arena page
and reading which fill path writes it. Related: the decl_* module counters
(decl_fresh vs decl_bytes) already split the decline's outcome and
decl_selftest proves the probe fires.

### 6.1a The source holding a page it recorded as handed over (the pools' last 2%)

After §2.18 the hx2 pool fails ~6/256, all photographed by the LOSS-PROBE.
Two shapes, one family:

- **All members absent, object a hole, retained slot == the stale value
  read** (4 of 6): the writer was served a copy as old as the retained one.
  The SOURCE's own counters in those runs: "requests refused because the
  record said the page was handed over, where this side was holding it
  anyway: 7534 / 8047 / 12688 — both machines holding one page", and "the
  ownership sweep found 77–176 entries recorded as handed over whose page
  was here anyway". One producer is visible in the trail: `RESTORE` (a take
  refused after it had already unmapped, put back) right after a `RECV` —
  the page is put back present while the record stays THEIRS/INTRANSIT;
  every later CTXPAGE is refused NOT-HOLDER → ABSENT → the destination
  falls back to the retained copy.
- **Reporter and siblings map the SAME folio as the object, whose slot is
  stale, yet the writer had just stored 138k rounds newer** (lost.243): the
  store went through a guest-ASID translation to a folio that is no longer
  the object's. That run's writers made 352M rounds (17× normal) and the
  page changed hands 28 times (vs ~200): a split brain for most of the run.
  The kick covers vmctx's own takes; whatever replaced that folio did not
  kick, or the kick did not reach the CPU the writer was spinning on.

- **A zero read while the page was AWAY, with no monitor event** (build-2
  pool run.15): the writer read 0 between `TAKE-AWAY gen=11` and that
  take's `PULL-BYTES`, 1ms after the take; nothing in either trail at that
  instant and the monitor's decline counters at 0. The load never reached
  the monitor: it went through a translation that survived the take's
  `unmap_mapping_range`. The take's copy had the pre-unmap bytes, the
  folio was then truncated, and the guest's later stores/loads hit freed
  memory — zeros or a reused page. This is the same mechanism as the
  split-translation photograph above, seen from the other side.
- **A TAKE-AWAY that captures an ALL-ZERO page** right after a real one
  (hx2 run.80 of pool 2: gen 75 real, gen 76 `sum=04000001` 500ms later;
  pg3 died.70: gen 78734 real, gen 78735 zeros 12ms later), then served,
  retained, and installed by the generation repair over the real bytes
  that arrive next (the repair is faithful; the capture was the lie). The
  take found a zero folio in the object: `page_read_as` is gated on
  SEEK_DATA, TAKEOBJ uses `filemap_lock_folio` (no allocation), PEEK is
  `copy_from_user_nofault`, MAPOBJ is 0055 (no allocation), declines 0 —
  so the zero folio was ALLOCATED by something that faults the object in
  without the monitor: a guest context faulting through a stale
  translation after the truncate is one candidate (the fault that refills
  the PTE is the kernel's, on the hole). `ZERO-TAKE` now photographs the
  members and the source (object vs TAKEOBJ) when it happens again.

The kick is wired as `.arch_invalidate_secondary_tlbs` (patch 0042/0063),
which the core calls after the PTEs are cleared; `vmctx_kick_guests` bumps
`vmctx_tlb_gen` and IPIs running guests, waited. So the ordering is right
on paper and the hole is narrower than "no flush": a CPU that is inside the
guest but not counted in `vmctx_guests_running` at the read, an IPI that
does not exit the guest, or a mapping whose mm has no kick notifier
registered (`vmctx_mn_enable` is conditional in 0063 — check that every
guest context's mm has `m->kick`). **The instrument to build is in the
kernel's TAKEOBJ: after the unmap+kick and the copy, re-read the folio
before the truncate; if it differs from the copy, a guest wrote through a
stale translation after the unmap — count it, and photograph which CPU /
context (`vmctx_guests_running`, the per-CPU tlb_gen) it was.** That is a
core-kernel change (rebuild + reboot of .229, gdm stopped first).

**Session 28 addendum.** The TAKEOBJ re-read detector (§2.22) is built and
read **0** over ~40 M hand-overs on #149, including pg3 runs that died
242: the residual is NOT a guest store through a stale translation under
the hand-over. Every pg3 death this session (5 of the #148 baseline pool,
4 of the final pool, 1 suite run) carries one line — `context <B's service
ctx>: 0x7ffff77f2000 answered ABSENT and filled with an empty page (rip
0x469cc2 …)` — and the photograph the print now carries names it:
`maps: 7ffff77f2000-7ffff77f6000 rw-p | syscall: 13 0xb 0x0 0x7ffff77f2000
0x8` = `rt_sigaction(SIGSEGV, NULL, oldact=<page>)` — which is **vmhome's
own signal-disposition probe** (`ctx_sys(pid, SYS_rt_sigaction, sig, 0,
scratch, 8)`, vmhome.c ~4820, through the context's 16 KiB scratch
mapping, `ctx_scratch()`). It runs because a SIGSEGV is being routed, i.e.
AFTER the guest has already faulted at 0x0 (ifetch) / 0x8 (write): the
line is downstream of the death, not its cause, and the zero-fill of the
monitor's own never-touched scratch page is correct (pre-touching the
scratch at creation would silence it). hx2's two no-output runs are the
same probe on the same kind of death. What remains upstream is unchanged:
a zeroed list/function pointer read at the pg3 join (and by an hx2 writer),
with every userspace crossing counter at 0, the source record THEIRS/OURS
consistent, and the kernel detector at 0 — the §6.1a split-brain class
the stamp guard of §2.21 now keeps asking through
(`n_claim_landing_behind`: 14 per hx2 pool). Next instrument: photograph
the faulting guest's *previous* page movement for the page holding the
zero word — arm `VMR_TRAIL_PAGE`/`VMHOME_TRAIL_PAGE` on 0x7ffff7ff6000
(B's top stack page, where `pd` lives) in a pool and read the trail of the
dying runs.

Tools in place: LOSS-PROBE, the two trails, ZERO-TAKE (an all-zero capture
of a transmitted page), the source's sweep counters. Start with the RESTORE
path's record (why THEIRS survives a put-back) and with a PFN photograph of
the source mm when it refuses "holding it anyway". Also open and measured
once each per 256: a 2s GET timeout (the destination's GET handler can wait
pull 500 + claim 1000 + take 1000 ms; the source's socket gives 2s — exit
13), a monitor self-crash at ~0.5s with rip=0 rax=0 rdi=pid rsi=17
(APPLYMAP) — a ret/call to zero; on_crash now re-raises so a core is
written (local-here.sh raises the limit; core_pattern is /tmp/core.%e.%p on
.229, non-persistent) — and a teardown wild write to 0x8 (exit 242).

### 6.5 fork from a threaded parent: FIXED (session 26) — the monitor's own mm carried the guest's mappings

`tests/mf2` (a fork made BY A WORKER THREAD of a threaded process — glib's
pool, glycin's image sandbox that netsurf forks, firefox's sandbox probe;
mf1 forks from main) failed every run through session 25 and is a regular
suite case from session 26. Session 25 read it as `cow_give_page()` serving
the parent's page as of the break; that reading was wrong, and the
photograph that killed it is worth keeping.

**What was photographed (session 26).** Page-logging the forking thread's
stack page (`VMR_PGLOG=0x7ffff5fef000 VMR_PGLOG_OFF=0xf98`, the offset of
`_Fork`'s saved canary) showed the break was RIGHT: the parent's first
post-fork store faulted (WRITEPREP → COWBREAK), `cow_give_page()` wrote the
page into the child's object with the canary intact, and the source's take
of the parent's page 400 µs later still carried the canary. Yet the child's
death frame held the parent's `__run_postfork_handlers` frame — written
AFTER the give. A new probe, `page_provenance()` (PFN of the page in this
context, its object's bytes, and for a copied address space every member
of the original's PFN plus the original's object), said why in one line:

    PROVENANCE after own-object MAP: ... present=1 pfn=0x20de07 mapped
      sum=681f1539 w[f98]=00081cf4; own object fd 21 sum=8adcc11e w[f98]=e0520b00
      original as 531667 member 531695: present=1 pfn=0x20de07
      <- THE SAME FOLIO AS THE COPY

The child's object held the right bytes; the child's page table mapped the
PARENT's folio. Its VMA at the thread-stack range was backed by the
parent's object — and `/proc/<monitor>/maps` showed where it came from:
**vmremote itself** carried `7ffff57f0000-7ffff77f4000 rw-s
/memfd:vmctx-guest`, the parent's thread stacks, in its own address space.

**Root cause.** `VMCTX_CTL_APPLYMAP` fans one context's mapping change out
to its siblings, and the kernel's `vmctx_apply_map_rep()` handled UNMAP
and ZAP for `t != current` but ran SET through `vm_mmap()` and PROT through
`do_mprotect_pkey()` — both act on **current->mm**, and current is the
MONITOR. So every SET fanned out to a sibling mapped the guest's backing
object into vmremote's own mm at the guest address (the sibling never got
it, and mapped the range lazily on its own fault, which is why nothing
looked broken). Every context clone()d from the monitor afterwards — no
CLONE_VM, so a copy of the monitor's mm — inherited those VMAs. A thread
context shares the same object anyway, harmless; a FORK child's VMA at
those ranges pointed at its parent's object, so it read and wrote its
parent's live memory there while its own object, correctly given the
fork-time bytes, sat unread. Only a threaded parent has cross-task
fan-outs, which is why only forks from threaded parents died — the
firefox sandbox child and netsurf's glycin forks included.

**Fix.** Two layers. vmremote's `as_apply_map_siblings()` fans out only
UNMAP and ZAP (`n_as_map_not_fanned` counts the rest; a sibling maps and
re-protects a range on its own next fault, in its own task). Kernel
patch 0069 (#148) refuses a SET or PROT for a task that is not the caller
(`vmctx_apply_map_remote_refused`), so nothing can map into the monitor's
mm again. An invariant check, `monitor_mm_check()` at every spawn, scans
`/proc/self/maps` for guest objects and reports any ("must be 0" in the
exit summary). mf2: 0/N → PASS every run; the child's stack, heap and a
sibling's word are its own.

**Kept as instruments:** `page_provenance()` (PGLOG-gated at the break
and the own-object MAP; unconditional at a fatal exception and at the
SIGABRT probe), the SIGABRT frame+live-canary probe, the COWBREAK-refusal
reason. The `0x7ffff57ee000 ... parent readable 0` refusal noted in
session 25 was downstream of this (the abort's unwind).

### 6.2 firefox first paint

The soak config that reached 229s/1.14M syscalls: the REAL binary
`/home/biwu/firefox/firefox` with `RUNHOME=/root` (browser-run.sh takes
RUNHOME; `/usr/bin/firefox` is Ubuntu's snap shim and can only beg for
snapd). First paint is gated on forwarded-futex-wake + throughput
(~200µs/syscall); the end-of-run STILL-WAITING entries are ordinary blocking
calls of a live browser.

**Re-measured on the final pair (2026-08-19, two runs, browser-run.sh
config):** high variance — one run died at 8s (guest exit 1, ~2k syscalls),
one ran to the 300s limit with no window mapped. BOTH had a context die on
an instruction FETCH (err 0x14) inside the main binary's own PIE text
(0x5555556xxxxx / 0x555555753f70) answered -14 by the source — a text page
of the program's own binary the source could not serve. That shape is new
enough to chase on its own (the binary's text should always be servable from
the file); it may be the same corruption family as netsurf's or a distinct
exec/regions defect. The twelfth session's 229s/window-mapping run predates
the INFLIGHT pair; re-establish its exact soak invocation before comparing.

### 6.3 The destination-side token drop

Much of this class fell to the 2026-08-20 crossing campaign (§2.14): the
four ABSENT/restore lies are gone and the two-phase hand-over closes the
wire window. What remains: ws1's last ~1% (2/256), pg3's 2-3/96 join-band
residual (its 0x0-fetch shape persists — re-photograph on the two-phase
build), and hx1's load-dependent mutual-timeout churn. Still-unfixed related
defects, recorded: ctx_monitor's failed GET still POKEs the thread buffer's
stale bytes (seen once, 118ms, pre-exec — harmless there but real);
page_make_writable's take-and-put-back (poke_site=38) re-creates the folio
from pre-take bytes outside ctl_lock — same class as the kernel's measured
read-poke-back loss.

### 6.4 The rest

- **ws1 torture at scale** on the final pair (the INFLIGHT no-cap logic and
  the hold rewrite both touch its teardown class — re-measure before
  assuming it moved). take_lost=1 was seen once (a file page, refs=4 want=3,
  during a passing pool) — the pre-existing post-zap class; watch it.
- **mf1** (fork from a threaded parent) is the standing xfail; three
  explanations refuted so far.
- **th9 / hx2 standalone** flakiness (hx2 standalone 14–18/20 while suite
  XPASS; th9 flaky on master too).
- **Intel port**: kernels 0055–0062 + the INFLIGHT userspace pair, with the
  netboot known-good discipline, before any cross-box run.
- **rc=139 vmremote self-SIGSEGV** (~2/1024, 0.7s in): the backtrace handler
  is armed; the next occurrence carries its own diagnosis.
