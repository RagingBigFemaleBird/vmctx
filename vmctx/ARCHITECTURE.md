# ARCHITECTURE — what vmctx is

vmctx makes the VM context a first-class Linux citizen: a real process whose
user code runs as a hardware guest (AMD SVM or Intel VMX), so that everything
that operates on a process — signals, ptrace, /proc, scheduling, fork —
operates on it, and so that a process can execute its user code on one
machine while its syscalls, files, and identity live on another.

The objective (avm.md) is met and measured: a program installed only on the
source machine executes its instructions on the destination's CPU at
98.5–99.7% of native compute speed, every syscall it makes is performed on
the source (one LAN round trip, 220 µs cross, 66–80 µs loopback; a page
fault ~390/40–90 µs), its output appears on the source, and real software
runs this way — compilers and their fresh binaries, JIT runtimes (V8, YJIT)
generating and executing their own code, shells, daemons' orphans, and
graphical browsers rendering and fetching. RUNNING.md is the operator's
guide; AUDIT.md holds the mechanism detail, every corner case, and the full
bug ledger; HISTORY.md the arc; NOTES.md the session-era history and the
refuted paths.

## 1. The pieces

- **Core-kernel patches** — ONE squashed patch per tree in
  `kernel-patches/` (linux-7.0.14 for the source box, the top-level patch
  for the 6.18.35 destination). They add two syscalls — `vmctx_run` and
  `vmctx_ctl` — plus the redirection hooks, the fault deadline, the
  take/protect/map operations, the syscall hold, the adoption machinery,
  the per-mm page record with its serve/land/ack/scan ctls, the
  event-time mm change log, and the monitor-succession rules. **The patch
  set is the only record**: `src/` holds the working git trees whose
  single vmctx commit regenerates it on every change.
- **The module** (`kernel/vmctx.c`, ~4.6k lines) — the hardware backends.
  SVM (VMCB, NPT-less: the guest uses the task's own CR3) and VMX. It owns
  VMRUN/VMLAUNCH, the guest register file, the guest-ASID TLB discipline,
  the kick (`backend->invalidate`), and the kernel-side instruments.
- **vmhome** (`user/vmhome.c`, ~8.1k lines) — the SOURCE half. Owns the
  program: starts it ptrace-stopped at entry, adopts it as a *service
  context* (`VMCTX_CTL_ADOPT`), performs its forwarded syscalls in its real
  address space, answers page requests, and services the guest's faults. It
  keeps NO page-ownership table and reads no `/proc/maps` on the page path:
  where each page is, is the kernel's record (§5, kernel #154), asked one
  op per request. Session 39 cut it from ~9.7k lines to this by deleting
  the userspace table, its sweeper, its trail, its in-flight table and the
  serve-path retry machinery those existed to reconcile.
- **vmremote** (`user/vmremote.c`, ~18k lines) — the DESTINATION half.
  Owns nothing (PRINCIPLES 6a: it has no concept of a file and never names
  the program). Stands up guest contexts, executes them, forwards their
  syscalls, and keeps its copy of memory coherent.
- **Small tools** (`user/`): dumpvm (capabilities + context dump), vmbench
  (the compute/syscall ratio), vmselftest (SVM guest-entry smoke test),
  vmthreads/homeapp (demo guests), vmmon (monitor demo), vmmig
  (checkpoint/restore demo), vmtake/vmsys (probes).
- **Harness** (`tests/`, `*.sh`): see §8.

## 2. The machines

| box | role | kernel | syscalls run/ctl |
|---|---|---|---|
| 10.0.0.229 (AMD, SVM) | source (and loopback test rig) | 7.0.14-vmctx (own build; ONE patch) | 472/473 |
| 10.0.0.30 (Intel, VMX) | destination | 6.18.35-0-lts (netboot; ONE patch, current) | 470/471 |
| home (WSL2) | build/orchestration only | stock | — |

The syscall numbers differ per kernel; a wrong number answers EINVAL, not
ENOSYS. The destination is disposable by design. The default test
configuration is LOOPBACK on .229: both halves on one box (`local-here.sh`),
which is where the whole suite and all the torture pools run. Cross-box runs
use `xrun.sh`/`runany-x.sh` and require the Intel port to be current.
Deployment and its proofs (SRCID, vermagic, syscall numbers) are
`sync-amd.sh` and `verify-build.sh`; the netboot tree under `http/` boots
.30 (no undo — known-good images are kept beside the live ones).

## 3. The life of a run

1. `vmremote <source>:<port> --net` connects to vmhome's syscall channel and
   opens the page channel (port+1).
2. `VMR_OP_START`: vmhome forks the program (named on ITS command line),
   ptrace-stops it at entry, adopts it with `VMCTX_CTL_ADOPT` — the task is
   now a *service context*: parked, its faults and syscalls redirected to
   vmhome, its address space the single authority for the program's memory.
3. `VMR_OP_LAYOUT` ships the as-loaded map. vmremote creates a guest context
   (`vmctx_run` with REDIRECT_SYSCALL|REDIRECT_FAULT|WAIT_MONITOR), backed
   by a per-address-space memfd **object** whose offsets equal guest virtual
   addresses. Text/data pages arrive on demand (`VMR_OP_PAGE` at first,
   `VMR_OP_CTXPAGE` once running).
4. The guest executes natively. EFER.SCE is clear, so SYSCALL raises #UD →
   VMEXIT → the module reports a syscall event to vmremote, which forwards
   it (`vmrproto` request) to vmhome; vmhome makes the service context
   perform the real call (`VMCTX_CTL_SYSCALL`) in the program's own address
   space; the reply installs the return value (and any register/mapping
   effects) and the guest resumes. Unforwardable = fatal (exit 97), never
   emulated.
5. Guest page faults VMEXIT to vmremote, which fills the page from its own
   object, or pulls it from the source (`VMR_OP_CTXPAGE`), subject to the
   ownership rules in §5. Faults in the service context (the source side,
   e.g. during a forwarded call) are serviced by vmhome's monitor thread,
   pulling from the destination (`VMR_PG_GET`) when the guest holds the
   page — and the kernel reports every such fault with `mmap_lock`
   DROPPED (`release_fault_lock` + `VM_FAULT_RETRY`, on the first pass and
   on the TRIED pass alike, kernel #156): a report made under the lock is
   the reader in a three-way cycle with a queued writer (a thread's stack
   `mmap`) and the destination's `VMCTX_CTL_SERVE` on the same rwsem. The
   one fault that never reports is the retry of a SELF-answered one (the
   record reads HOME: the local fill is the answer).
   Exceptions that are the program's own (a deliberate SIGSEGV) are
   delivered to the side that owns it — the source rules on them
   (`VMCTX_CTL_TRYFAULT`), and a signal is a frame pushed by POKE plus
   registers. The ruling has three answers, not two: the program's handler
   (registers to carry on with), fatal (the service process is killed with
   the signal), or **`VMR_EXC_LEGAL`** — the program's kernel would have
   handled the access, so it is not an exception at all. The third exists
   because the destination cannot tell the program's own protection fault
   from one the protocol left behind: a hand-over write-protects a page in
   every sibling context's page table and the sibling that pulls it back
   re-grants only its own, so the next sibling's store is a present-page
   write fault on a page nobody holds. LEGAL is answered only for that
   shape (a write to a present page, never a fetch — TRYFAULT does not
   model NX), and the destination grants the write in place (session 40).
6. clone/fork/execve are forwarded like everything else; the SOURCE runs the
   real clone3/execve (the clone decision is the source's; guest clone args
   are never read raw — the args page is pulled first). New service contexts
   pair with new guest contexts (`VMR_OP_CHILD`); threads share the object,
   forks get an empty object of their own (§6). vfork runs as plain fork.
7. Teardown: exit/exit_group end the context (the module names which call
   ended it); `VMR_OP_FINISH` releases the source side; the clear-tid write
   and futex wake run on the source, targeted at a LIVE member of the mm.
   `as_life` counts service contexts per address space; positive→zero is a
   latched event, and only that fact licenses `VMR_CTXPAGE_GONE`.

## 4. The syscall path, exactly

Guest SYSCALL → #UD VMEXIT → module writes the event (number, args, rip/rsp)
→ vmremote's context thread reads it → `vmrproto` request over TCP → vmhome
dispatches: the service context performs the call via `VMCTX_CTL_SYSCALL`
(one assisted call at a time per context; a blocking call blocks the slot).
Before the call runs, vmhome pulls back every shared-file page its record
says the guest holds — asked of the kernel record by `VMCTX_CTL_PGSCAN`
(`sh_file_reclaim_held`), not by walking maps text. While the call RUNS, the
kernel's **syscall hold** (patch 0062) refuses takes of every page the call
can touch: pages recorded by the fault-in registry (reads and writes), every
page present in the adopted mm (the presence rule), and the ARG-PAGE seeds —
held until the call returns or blocks, never for a fixed term; the take's
-EBUSY is answered by waiting (the 1s serve wait — the 200ms yield was
measured worse and reverted). The reply carries the return value, a register
change-mask (captured against the frame as handed to the call), any
mapping op (`VMR_OP_MAPSYNC`/APPLYMAP replay) the destination must apply so
constructive address-space changes propagate — and the **vacated ranges**
(`vmr_rsp.vacate[]`): after every call vmhome drains the kernel's mm-mutation
hook (`VMCTX_CTL_MMLOG`, patch 0063 — an mmu-notifier `invalidate_range_start`
filtered to `MMU_NOTIFY_UNMAP`, ring-buffered per mm), so every range the
adopted mm lost while the call ran arrives whatever vacated it, and the
destination gives each the full munmap treatment (`range_vacated()`:
cow-break for copies, object + retained-copy punch, layout forget, APPLYMAP
UNMAP to every member). This replaced the syscall-number decode of
munmap/mremap in `reply_note_map`, which structurally could not enumerate how
an mm loses memory (mremap's absence there was netsurf's exit-242 wall);
what remains of mremap handling is the old→new relocation pairing, the one
thing only the call can express.

**The waits along that path, and why they spin first (session 46).** A
loopback call is a ring of four tasks — the guest context (asleep in
`vmctx_report` for its reply), vmremote's service thread (in
`VMCTX_CTL_WAIT`), vmhome's connection thread (in `VMCTX_CTL_SYSCALL`, and
in `read(2)` for the next request) and the service context (parked in
`vmctx_guest_step`) — plus the two socket reads. Every hop was a sleep and
a wake: `perf sched timehist` measured 3–10 µs of wake-to-run per hop, and
the sleeping let schedutil clock the cores down. Kernel #181 spins at its
four waits for up to `vmctx_spin_us` (100) before sleeping, adaptively per
site and per context (a wait past the budget disarms the next spin;
`need_resched()` ends one at once); both halves spin on their socket reads
the same way (`VMCTX_SOCK_SPIN_US`, 50, `recv(MSG_DONTWAIT)` probes); and a
request's header and payload travel in one `writev` (they were two
segments, and the reader woke twice). Measured on the getppid loop, one
boot, arms alternating: 61 → 26 (kernel spin) → 17.6 µs (socket spin) per
forwarded call under schedutil; 160k of 160k waits answered inside the
spin. Cross-machine the LAN's 250 µs disarms the long waits by itself.

## 5. The memory model — a single writer

Two machines each hold a copy of parts of one address space; correctness is
exactly one writable copy of any page at a time, and the other side must
ask. Both directions are PULLS; nothing is pushed or diffed.

- **Ownership is taken, not inferred** (PRINCIPLES §1), and on the source it
  is **the kernel's record, beside the page tables** (kernel #154, session
  39): one entry per page of an adopted address space
  (HOME/REMOTE/CLAIM/TRANSIT/NONE), packed into a per-mm xarray, and every
  transition is made *under the mm's lock together with the page-table
  action it describes*. vmhome no longer keeps a table of its own or reads
  `/proc/<pid>/maps` to serve a page: it asks one op. The destination's
  `lent[]`/`installed[]`/object state is still its own (its memory is a
  backing object, not an mm).
- **The hand-over**: the destination faults → asks `VMR_OP_CTXPAGE` → vmhome
  calls **`VMCTX_CTL_SERVE`**, which under one lock looks the mapping up
  (growing a stack hole as the fault path would), classifies it, consults
  the record, and either copies the page (read-only, or a writable
  MAP_SHARED file — both machines may hold it) or TAKEs it (writable private,
  recording TRANSIT before the zap so nothing can move it in between);
  the answer is TAKEN/COPIED/ABSENT/CLAIMING/DENIED/NOMAP in one round trip.
  The source faulting → the kernel's fault hook **CLAIMs** the page before
  the monitor is told, so a sibling context's fault on the same page WAITS
  on the entry's futex (`pg_wq`) instead of racing a second fetch; the
  monitor's `VMR_PG_GET` brings it back and **`VMCTX_CTL_LAND`** installs it
  and records HOME in one step. A fault on a TRANSIT page waits for the
  hand-over to settle rather than stealing it. -EBUSY (a syscall hold) means
  "not yet", answered by bounded waiting; a refused take never destroys the
  page (the claim-before-zap is in `vmctx_ctl_take`).
- **The answers** are a closed vocabulary (AUDIT §5): bytes, ABSENT (fresh —
  licenses the asker's own copy or first-touch zeros, and is recorded),
  CLAIMING (simultaneous claim — the source wins; the destination yields and
  refaults), COWBREAK (still the original's; break where the page is),
  INFLIGHT (token in transit; ask again — THE LANDING decides, not a clock),
  GONE (the address space has ended — latched fact only), NOTHOLDER
  (defined, handled, currently never sent — see NOTES §2.14).
- **Never invent a page** (PRINCIPLES §3). A page that exists somewhere and
  cannot be fetched is FATAL (exit 98) after the retain-table last resort;
  an ABSENT for a page the record says was handed over is re-asked, then
  killed loudly — never zero-filled. First touches are zeroed ONCE, recorded
  (`retain_put`), so they are never re-invented.
- **Shared file mappings** (writable MAP_SHARED) cannot be taken (the page
  cache holds them); they are copy-served, recorded THEIRS ("currency, not
  location"), and reclaimed before every forwarded call. Read-only file
  pages that no forwarded mprotect sealed are served from the local file
  without asking; only RELRO-sealed ranges can differ and only they are
  asked about (GETS — a copy, since a read-only page cannot diverge).
- **Coherence for MAP_SHARED between processes** is write-protect-driven
  with one writable copy (DOWNGRADE/UPGRADE on the page channel, pgstate.h
  claim/settle on the destination; the source never contends — the
  destination always yields, decided by role, not timing).

## 6. fork, threads, exec

- **Threads** share the address space: same object, own guest context, own
  stack at a fixed per-context address, guest FS/GS owned by the vmctx
  register file (kernel #126 — SETREGS cannot reach an on-CPU task's MSR).
- **fork**: the child's object starts EMPTY; nothing is copied. The source's
  kernel owns the COW relationship (it ran the real clone); `fork_rel[]` on
  the source records "B began as a copy of A" for the pages that live on the
  destination, and COWBREAK moves exactly those, once each (`cow_broken`),
  in both directions (child faults → break at the holder; parent about to
  write → give the child the bytes first).
- **exec**: the adoption invariant (kernel #132) — redirection applies only
  to the ADOPTED mm, and `vmctx_exec_notify` fires at exec COMPLETION, so
  the loader's own writes during execve run in the old mm untouched and the
  new image is never served stale pages. On the reply: the destination
  `exec_wipe`s its entire copy (cross-task UNMAP of every object VMA, whole
  object punched, every record disarmed); the source drops regions/pgown/
  sh_file/fork_rel for that AS and bumps `mm_generation` so /proc-fd caches
  revalidate; the destination re-fetches the new layout (`VMR_OP_LAYOUT`
  relayout form).

## 7. The TLB and the kick

The guest runs on the task's own CR3 under a guest ASID. Two ordered halves
guarantee no guest ever writes through a pre-take translation:

- take side (`vmctx_kick_guests`): bump `vmctx_tlb_gen`, THEN read
  `vmctx_guests_running`; IPI every CPU (waited) if any guest is inside
  VMRUN — its stores land on the still-held folio before the take's copy.
- entry side (`svm_vmrun_once_gpr`): `atomic_inc(guests_running)`, THEN read
  the generation; re-arm `tlb_ctl=1/clean=0` for this entry if it moved.

The build-time `tlb_ctl=1` that nothing clears is the STANDING flush and is
load-bearing: it covers every invalidation nothing announces (exec relayout,
apply_map sibling zaps, POKE's COW break, reclaim). Clearing it KVM-style
killed 24/24 pg3 runs in half a second. Nothing may reduce the baseline; the
generation check is the guarantee on top of it.

## 8. Tests and harnesses

- **suite.sh** — 30 cases, ~28 graded (hx2 and ns1 are marked known-gap and
  count as XPASS when green): th9 th12 loc2 ex4 ex5 cow1 pg1 pg2 pg3 hx2
  pf1 pf2 gp1 tr1 pf3 pf4 sig1 fi1 fi2 sm1 sm2 sm3 rd1 dma1 stk1 fm1 rf2
  an1 lo1 ns1. Three rounds = 84 graded. Each case's predicate is in
  suite.sh beside the story of what it measures. MUST run under sudo.
- **runany.sh / runany-x.sh** — one program, loopback / cross-box; exit 3
  means "no output", not a verdict.
- **prun.sh** — pooled runs, `ARM_A=/ARM_B=` alternating arms in ONE pool
  (a pool is a different load; compare pool with pool only).
- **torture.sh / ws1-oracle.sh / ws1-armab.sh / ws1-repeat.sh** — the ws1
  battery: classification by failure shape (lost/heap/tmo/hang/died/other),
  grading by the arithmetic, arm alternation, and the repeat-detector.
- **perf.sh / perfab.sh** — the performance harness: seven typical programs
  (wc, curl, sha256sum, python, gzip, links2 -dump, vmbench) natively and
  forwarded, with vmremote's setup/faults/syscalls breakdown; perfab.sh
  ALTERNATES two (vmhome, vmremote) pairs run by run, which is the only way
  to compare builds on a box whose idle states move a run by ±50%. Numbers
  and the bottleneck trail: HANDOFF.md §38.
- **browser-run.sh** (+ `native.sh` as the no-vmctx control on the same
  box/Xvfb/URL) — the browser gate: window count, colours, screenshot,
  http hits. links2/netsurf/firefox; RUNHOME selects the real firefox.
- **Instruments** are part of the architecture, not an afterthought: the
  page logs (VMR_PGLOG/VMHOME_PGLOG), the watchword, the clobber/content
  -history detectors, the module's decl_*/bp_* counters and sysexit ring,
  and the SIGPROBE register cross-check. The inventory with arming
  instructions is AUDIT §8. Instruments that can mask what they measure are
  flagged there (armed pools mask pg3 — only unarmed pools measure it).

## 9. Build and deployment integrity

Every binary compiles in a SRCID (a hash of every source under user/) and
prints it at startup; `verify-build.sh amd|intel` compares tree against
deployed binary, module vermagic against the running kernel, and the
syscall numbers, and checks the module is loaded. `sync-amd.sh` is the only
deployment path: it deletes binaries before building (rsync preserves
mtimes), `--delete`s only the directories this tree wholly owns, ships the
core-kernel header the module compiles against, and (`MODULE=1`) rebuilds
the .ko — which still must be `rmmod`/`insmod`ed to take effect. Static
binaries link at fixed text segments (vmremote 0x20000000, vmhome
0x28000000) so their addresses never collide with a guest's and a backtrace
resolves with addr2line.

## 10. The page record

The coherence record lives IN THE KERNEL, per mm, in an xarray beside the
page tables it describes: one packed value per page — state (NONE / HOME /
REMOTE / CLAIM / TRANSIT), a 16-bit sum, the 32-bit take generation, and a
jiffies stamp — read and written under the same locks that move pages, so
the record and the mapping cannot disagree. VMCTX_CTL_SERVE is the one gate
every hand-over passes (its decision table, with the two age/ask-count
heals that ended the eternal answers, is AUDIT §1.2); LAND installs bytes
and records them in one step; PGACK settles the two-phase hand-over,
sum-guarded per episode. Destructive address-space changes reach the
destination through the mm change log — stamped AT THE EVENT under the
log's lock, because a drain-time stamp is a race between connections — and
constructive ones ride each syscall's reply, applied at RESUME by the
faulting task itself. The tie rule is load-bearing: a construction's stamp
equals the last event's, and on overlap the tie is the construction's —
the kernel's own address allocation proves the order.

## 11. Time and the vDSO

The guest's rdtsc reads the destination's host TSC (the backend never
writes a TSC offset), and the source cannot serve vvar or vdso pages by
any userspace means. So time is decided by one fact the harness feeds:
when both kernels are the SAME release, the destination serves the guest's
[vvar]/[vvar_vclock]/[vdso] from its own live pages — bit-identical image,
correct calibration for the very TSC the guest reads — with a 3×/s
refresher keeping the coarse-clock fields ticking (a snapshot's coarse
clocks freeze; a time()-bounded loop measured that as a hang). When the
releases DIFFER, the source hides the vDSO from the first image at the
exec stop — before ld.so caches AT_SYSINFO_EHDR — and every clock is a
forwarded syscall: correct anywhere, a round trip per read. tests/vd1.c
gates both arms. The remaining time-domain gap is cputime: forwarded
CLOCK_*_CPUTIME reads report the parked source task, not the guest's burn
(AUDIT §10.26).

## 12. The context lifecycle

Three rules define what happens when a context's counterpart leaves, and
each replaced a measured death:

- **The tree is the service lifetime.** vmremote's main drains every
  forked context's service before leaving, bounded by liveness rather
  than a clock (the counter-leak case the old two-second bound existed
  for is detected by audit). A daemon's orphan keeps its service; the
  harness limit bounds a true daemon exactly as timeout(1) would.
- **A dead monitor is a missing monitor.** The kernel publishes the event
  and waits for a successor under the fault deadline instead of letting a
  redirected context improvise its own fault into -14. Userspace hands
  each forked context's monitor from the thread that spawned it to the
  thread that services it — the spawner detaches after the release, the
  child's own service attaches — so a guest ENTERS exactly when its
  watcher is in place and can never outrun or outlive it.
- **Claims, transits, and loans die with what made them.** A service that
  breaks out settles the claim it wrote; unacked transits are settled
  when their connection closes (acks arrive on the connection that
  served); a HARD vacate clears the loans in its range — and only hard:
  content dies soft, relationships die hard.

The measured result of the three: the orphaned-server, dying-context-EIO,
and clone-window classes all died at once (nc1 9/9 first-ever, sort 9/9
against 2/12, lddnames 6/6 with no kernel wipe), and the breadth campaign
recorded its first zero-failure run.
