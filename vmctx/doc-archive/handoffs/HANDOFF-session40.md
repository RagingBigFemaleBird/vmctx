# HANDOFF — session 40 (2026-08-23): the hx2 death was a misjudged exception

Follows `HANDOFF-session39.md`. The directive: *"continue the optimization,
and make all tests green."* Session 39 left hx2 at 37–43/48 in the chain's
pool on kernel #155, read as the session-37 "deep teardown thread-list"
class. It was not. Every gate is green at the end of this session: chain
**s40h** — suite **34/0**, hx2 **48/48**, pg3 **48/48**, ws1 **48/48**,
sig1 **48/48** (s40g before it: 34/0, 47/48, 48, 48, 48).

## What the death actually was

Read from the failing runs of chain s39d, then from five instrumented 48×8
hx2 pools (s40a–s40d), in the order the evidence came:

1. **The `pull_lost` producer.** The census discriminator was `pull_lost`
   (10 of 11 failing runs, 0 of 37 passing): the source's pull-for-a-write
   took a thread-descriptor page from the destination and `VMCTX_CTL_LAND`
   answered **-EINVAL** — the ctl entry's "not a VM context" — for a task
   `kill(pid, 0)` still called alive. The "live sibling" fallback failed the
   same way. The only copy was dropped; the joiner later read zeros
   (`__nptl_deallocate_stack`, exit 242) or the source answered "no such
   address" (exit 131). Upstream of it: every forwarded syscall of the
   teardown (`rt_sigprocmask`, `madvise`, `futex`, `exit`) had already
   failed `ctx_call FAILED ... errno=Invalid argument alive=1`, and **every
   context's fault service had ended at once**, right after main's 20 s
   `clock_nanosleep` returned — main's included.
2. **The photograph.** `task_photo()` (new: `/proc/<pid>/stat` state +
   `wchan` + SigPnd/ShdPnd, printed on `fault service ends` and on
   `ctx_call FAILED`) showed every service context **in `do_exit`**: state
   D in `__synchronize_srcu` (the mmu-notifier unregister in
   `vmctx_finish`), I in `do_exit`, or R — the whole thread group exiting,
   with no kernel line saying why.
3. **The sender.** vmhome's own kill sites all fire at the end of a run;
   the kernel's vmctx code sends no SIGKILL. The `signal:signal_generate`
   tracepoint (it records the *sending* task) over a pool showed: a
   **vmhome conn thread sends SIGSEGV to the service process ~0.3–8 s into
   the run**, and the kernel **delivers it 12 s later** at the 20 s mark
   (`signal_deliver sig=11 sa_handler=0`). SIGSEGV is a core-dump signal:
   it stays pending until some thread dequeues it, and the service contexts
   park in `wait_event_killable`, which only SIGKILL wakes. When main's
   nanosleep completed and that thread returned toward user mode, the dump
   began, `zap_process` ended every thread (an untraced SIGKILL), apport
   ran, and the run's teardown proceeded against a corpse.
4. **The cause.** Every failing run carried, early, `[vmhome N] exception 14
   at 0x7ffff7ff60{40,48,50,58} becomes signal 11, which the program has
   not asked to handle; it is fatal (rip 0x401f97 ... err 0x7)` — a
   writer's ordinary store to its slot in the hx2 page (mapped rw-p) judged
   a fatal segmentation fault. 76 of 79 failing runs across s39d/s40a–d.
   On the destination the store had arrived as a **present-page write fault
   with no owner, no watch and no COW mark**: a hand-over's PROTECTOBJ
   write-protects the folio in EVERY sibling context's page table (each
   guest thread is a context with its own mm mapping the shared object);
   the sibling that pulls the page back re-grants only its own mapping
   (MAPOBJ) and spends the per-(as,page) watch/loan; the next sibling's
   store is a **stale protection**. The dest's protection-fault branch had
   no case for it and handed it to the owner (`owner_takes_fault`). The
   source's `ctx_deliver_exception` called `ctx_tryfault` only to choose
   `SEGV_ACCERR` vs `SEGV_MAPERR`; its **0 — "the kernel would handle the
   access"** — was mapped to ACCERR and the process killed. The uapi
   comment on `VMCTX_CTL_TRYFAULT` had already written the rule this broke:
   *"the owner may declare an address faulting only after asking its
   kernel to TRY the fault … second-guessing it manufactured a SIGSEGV the
   program never earned."* The destination, told "fatal", declined and
   zero-filled (`DECLINED-BY write-to-an-unlent-read-only-page` →
   `INVENTED`), so the program ran on for 12 s on a page of zeros while the
   signal waited.

So the session-37 "deep teardown thread-list coherence" reading was the
downstream of this: the thread-descriptor pages were lost because the
service process was already dead when the teardown asked it to land them.

## The fix (userspace only; build 15b046d5f4e3)

- `vmrproto.h`: **`VMR_EXC_LEGAL` (= 2)**, the third answer to
  `VMR_NR_EXCEPTION` beside "carry on with these registers" (0 + regs) and
  "fatal" (-EFAULT).
- `vmhome.c` `ctx_deliver_exception`, vector 14: if `ctx_tryfault` answers 0
  (asked through a live sibling when the context is gone) **and the fault
  is a write on a present page and not a fetch — `(err & 0x13) == 0x3`** —
  answer LEGAL (`n_exc_legal`, capped line) instead of killing. The first
  form (LEGAL for any 0) broke **pf2 no-exec and pf4 stack-exec** in chain
  s40f: a fetch fault (err 0x15) on a page without PROT_EXEC re-faulted for
  ever, because TRYFAULT does not model NX and the protocol never
  exec-protects anything. The protocol write-protects and nothing else, so
  that is the only shape LEGAL may cover.
- `vmremote.c` `owner_takes_fault` returns **2** on LEGAL (every caller now
  tests `== 1` for "registers installed, resume"); in the protection-fault
  branch, site4 on 2 re-takes `coh_lock`, re-checks `page_holder` (an owner
  that appeared during the round trip is the owner branch's on the
  re-fault: `n_prot_legal_owner`), and `page_make_writable`s in place
  (`n_prot_legal_granted`, clobber site 85, `SERVED-BY site4L`). Summary
  line: "protection faults the owner's kernel ruled LEGAL …". In every pool
  since, all LEGALs resolved through the owner branch (granted 0, owner
  14–16 per 48 runs).

## Gates

| chain | build | suite | hx2 | pg3 | ws1 | sig1 |
|---|---|---|---|---|---|---|
| s39d (session 39 final) | 2e447c56add1 | 34/0 | 37/48 | 48 | 47 | 48 |
| pool s40e (fix, first form) | d7438c6a5066 | — | **48/48** | — | — | — |
| s40f (first form) | d7438c6a5066 | **32/2** (pf2, pf4) | 48 | 48 | 48 | 48 |
| s40g (`(err & 0x13)` rule) | 15b046d5f4e3 | 34/0 | 47/48 | 48 | 48 | 48 |
| s40h | 15b046d5f4e3 | **34/0** | **48/48** | **48/48** | **48/48** | **48/48** |
| s40k (kernel #156) | 7b76eff0e305 | 34/0 | 48/48 | 48/48 | 48/48 | 48/48 |
| pool s40l (#156, 192×8 hx2) | 7b76eff0e305 | — | **192/192** | — | — | — |
| s40m (kernel #157) | 7b76eff0e305 | 34/0 | 48/48 | 48/48 | 48/48 | 48/48 |
| s40n (#157, new harness clock) | 7b76eff0e305 | 34/0 | 48/48 | 48/48 | 48/48 | 48/48 |
| s40o (kernel #158, chunk readahead) | a8db424bd350 | 34/0 | 48/48 | 48/48 | 48/48 | 48/48 |
| s40p (both readaheads, RTT-gated: OFF here) | 21e41eea608f | 34/0 | 48/48 | 48/48 | 48/48 | 48/48 |
| s40q (both readaheads forced ON) | 52bda003073b | 34/0 | 48/48 | 48/48 | 48/48 | 48/48 |
| s40r (kernel #159, writable chunking ON) | dffbe0b3dc7f | 34/0 | 48/48 | 48/48 | 48/48 | 48/48 |
| s40s (split-floor window, ON) | c8c2fd3421c4 | 34/0 | 48/48 | 48/48 | 48/48 | 48/48 |

Kernel counters steady: `take_lost` 2→3 over the session (the benign
post-zap class), `takeobj_stale_store` 0, `mm_overflow` 0, record
`nomem` 0, `ack_stale` +2.

## The residual (~1 in 100 pool runs) — found and fixed in kernel #156

The `SLOW PULL`/`SLOW CTXPAGE` instruments caught it in pool s40j
(died.46, exit 131 at 6.8 s): every serve of the address space sat
**6.1 s inside `VMCTX_CTL_SERVE`** with the busy loop idle (`busy-waits 3,
gave up 0`) — three serves of the hx2 page, one of a thread stack, one of
main's stack, all released the instant the deadline killed the faulter.
Only a per-mm lock blocks every serve regardless of page: the ctl's
`mmap_read_lock`, queued (rwsem is fair) behind a writer — main's `mmap`
of a new thread stack — queued behind a reader: the **TRIED pass** of
`vmctx_anon_fault`, which reported the fault *with the lock held* while
the monitor waited for a sibling's landing; that sibling's GET was
answered INFLIGHT by the destination because the destination's own pull
of the page was the stuck serve. The kernel's comment above the first
pass names exactly this three-way cycle and the rule — "do not hold
mmap_lock across the wait for the monitor" — and the blocking TRIED pass
was the one place that still broke it.

**Kernel #156 (`kernel-patches/linux-7.0.14/0074-*.patch`):** the TRIED
pass drops the lock and reports off-lock exactly as the first pass does
(handle_userfault's rule: every pass). The SELF shortcut — the record
reads HOME on the retry, so the local fill is the answer and there is
nothing to re-ask — is kept *before* the drop, so a declined fault cannot
loop; a page the monitor supplied is present on the retry; only a page
that left again between landing and retry reports again, which is a new
hand-over. `vmctx_af_retry_tried` counts the second-pass drops.

**Chain s40k on #156:** suite 34/0, hx2 48/48, pg3 48/48, ws1 48/48, sig1
48/48 — and, for the first time in the record's life, a whole chain with
`fault_deadline_hits` **0**, `pgrec_wait_timeouts` **0**, `take_lost` **0**
(92,224 second-pass drops). Session 39's record is now also in the series
as `0073-*.patch` (the scratch tree's commit ffb7bd585), with #156 as
`0074` (its commit follows).

### The residual as first seen (kept for the shape)

s40g died.47, s39d died.7, s40a died.6: exit 131 at ~6.8 s, **no** `becomes
signal` line. Shape: a destination pull of the hx2 page stuck ~3.5 s inside
the source's `VMR_OP_CTXPAGE` at start-up (~0.3–0.8 s in, while main is
still spawning threads — `mmap` of new stacks, `clone3`); the sibling
writers wait on it (`pull_wait gave up … started by tid N 3500ms ago`), the
source's own GETs for its evacuator's fault are answered INFLIGHT because
that pull is active, and every side reaches the 6 s fault deadline. The
source's capped lines were spent on start-up chatter. Armed for it:
`SLOW PULL` (vmremote, a CTXPAGE round trip > 1 s, its own cap of 8) and
`SLOW CTXPAGE` (vmhome, the same from the serving end, with the busy-wait
counters). Candidate mechanism to check first: the fault's TRIED pass
reporting with `mmap_read_lock` held while a writer (main's mmap) queues
and the dest's SERVE queues behind it on the same rwsem — the three-way
cycle the `vmctx_anon_fault` comment describes, across machines.

## Kernel #157: teardown no longer waits an SRCU grace period per context

The session's photographs kept showing exiting contexts in state D at
`__synchronize_srcu` — `mmu_notifier_unregister()` in `vmctx_finish`, once
per context, on both machines. `kernel-patches/linux-7.0.14/0075-*.patch`
replaces it with `mmu_notifier_put()` + `ops->free_notifier`
(`vmctx_mn_free`): the subscription is unhooked immediately and the
container freed from the SRCU callback. The log callback reads only the
mm-keyed table, never the context, so nothing in the exit path needs to
wait; the kick subscription (the dormant A/B arm) keeps the synchronous
form because it shares the container. Chain **s40m** on #157: suite 34/0,
hx2/pg3/ws1/sig1 48/48, deadline hits 0, wait timeouts 0, no kernel
warnings.

## The measurement was wrong, and by more than the system

Every wall-clock number this project reported before today carried the
runner inside the clock. Found by reading vmremote's own `time:` line (wc:
0.046 s) against local-here.sh's `=== exit 0 in 0.14s` and perf.sh's
`wall 0.223`:

- `local-here.sh` took `t0` before spawning and `t1` after two `ps -o
  pgid` invocations, a `pgrep` and the group kill — ~90 ms of `/proc`
  walking.
- `perf.sh`'s `wall` wrapped the whole of `local-here.sh`: vmhome's start
  and its readiness poll at `sleep 0.1` steps.
- And **this box's coreutils are the uutils (Rust) multi-call binary**
  (`/usr/bin/timeout → /usr/lib/cargo/bin/coreutils/timeout`, 0.8.0):
  `timeout true` measured **114–121 ms** (it polls its child at 100 ms
  steps), and `env`, `date`, `cut`, `sleep` each cost ~10 ms to exec. That
  was the entire "0.11 s floor" every short program showed.

Fixed in the harness, no system change: `local-here.sh` clocks only
vmremote with bash's `$EPOCHREALTIME`, makes the process group with
util-linux `setsid` (pgid = pid, which the sweep kills), exports
`VMREMOTE_ENV` itself, and bounds the run with a watchdog subshell that
leaves `$OUT/.timedout` and answers rc 124 as `timeout` did (tested:
`VMR_LIMIT=2` on hx2 → `exit 124 in 2.018s`, five contexts swept, no
leftovers); `perf.sh` prints `wall` (vmremote's life, from the `=== exit`
line) and `harness` beside it, and its own `now` is `$EPOCHREALTIME`.
Chain **s40n** on the new harness: suite 34/0, all four pools 48/48.

**Honest numbers (kernel #157, idle box, medians of 5, `perf.s40e`):**

| prog | native | fwd wall | setup | faults (n, µs/ev) | syscalls (n, µs) |
|---|---|---|---|---|---|
| wc | 0.012–0.019 | **0.048** | 0.006 | 0.020 (513, 39) | 48, 125 |
| curl | ~0.020 | **0.094** | 0.009 | 0.041 (989, 41) | 169, 107 |
| sha256sum | ~0.028 | **0.129** | 0.006 | 0.051 (1031, 49) | 107, 430 |
| python | ~0.030 | **0.318** | 0.005 | 0.125 (2311, 54) | 377, 202 |
| gzip | 0.434 | **1.257** | 0.005 | 0.307 (4031, 76) | 370, 935 |
| links2 -dump | ~0.021 | **0.119** | 0.016 | 0.052 (1131, 46) | 135, 148 |
| vmbench getppid | — | — | — | — | 20018, **70** |

(the native column still carried one uutils `date` exec per run when this
table was taken; `perf.sh`'s clock is `$EPOCHREALTIME` now.) The per-fault
and per-syscall costs are session 39's; what changed is that wc is 48 ms,
not 290, because 240 ms of it was never the system. Compare per-event
costs across sessions, never old walls.

## Kernel #158 + the chunk path: fewer faults, honest about loopback

The 64 KiB chunk fetch the destination has always described never happened
past one chunk: only the faulting page is backed when a fault is reported
(`vmctx_back_fault` maps one page), so a chunk's neighbours had nowhere to
land, and the first refusal — the first fault of every run — cleared
`chunk_ok` for the run. Three changes, gated green by chain **s40o**:

- **Destination**: neighbours with no mapping land in the backing object
  (`obj_write` + `mark_installed`); a short `process_vm_writev` falls
  through to the per-page loop; the reply answers the fault with
  **`VMCTX_MAP_SET_POPULATE`** (kernel #158, patch 0076) over the verified
  run — one VMA from the object with the range's own protection, populated
  in the context's own task before the next VMRUN, so those pages never
  fault. Only pages this service verified the object holds (a `page_keep`
  page may be a punched hole; populating a hole would mint the session-36
  zero folio). Read-only ranges only (text/rodata/RO files): a writable
  chunk would take sixteen pages a forwarded syscall may be about to use.
- **Source**: a multi-page request answers *short* on a syscall hold
  instead of waiting up to 1 s per held neighbour (`ctx_serve_one`
  `wait_hold` = first page only).
- **Window**: forward readahead — two pages, doubled while faults arrive in
  sequence, reset otherwise, capped at 16. The fixed 64 KiB window fetched
  1043 neighbours for wc of which ~800 were never reached (~10 µs each of
  source copy, wire and object write), and made links2 slower.

Result: fault events wc 513→424, sha 1031→944, python 2311→1930, links2
1252→1040; bytes +30%; output identical; `n_chunk_lost` 0 over the chain.
**Wall on loopback: inside the box's noise** (perf.s40f medians: wc 0.053,
curl 0.081, sha 0.113, py 0.250, links 0.110, gzip 1.96 in a ±30% band),
because here a round trip costs about what a page copy does. The path was
designed for the cross-machine case (a LAN RTT per page), which is where
it should be measured (the Intel port is still behind, NEXT).

## The source-side readahead (the I/O-buffer bounce) — built, and gated by the wire's price

A forwarded read(2) into a 32 KiB buffer the guest holds faulted eight
times on the source, one GET round trip each. `prefetch_issue/settle` in
vmhome's fault service now read ahead inside an assisted call: the fault
service knows the context is inside a call (`incall_set/get` around
`VMCTX_CTL_SYSCALL`), the window is the page cache's (nothing on the first
fault, one page on the next sequential fault, then two, then four), the
window's GETs are written to the page channel *before* the fault's own GET
and their replies taken off the stream by `pg_get_op` just before the
fault's reply (the channel answers in request order; a reply names no
address), and every page is a real hand-over: CLAIMed in the record before
the ask, LANDed with IF_ABSENT, the record restored on any non-page answer,
the bytes given back (`pg_put`) if nothing here can take them.

**The lesson that cost the afternoon:** the first form landed the window
*after* the faulting page's RESUME, to overlap the context's copy. **1 gzip
run in 7 then produced a different file** — its read buffer's last page one
block stale: a landing concurrent with the context's own write of the same
page lost the store (`VMCTX_CTL_LAND`'s presence check and its write are two
steps; no existing pull path had ever landed into a *running* context). The
window now lands before the RESUME, with the context blocked on it: 42/42
gzip runs identical, the one-round-trip form kept. The per-page write
`gz.bad.N` decompress-and-`cmp -l` method (differing bytes land in exactly
one file page; `page/8` is the block, `page%8` the buffer page) and the
page logs on the *real* buffer page (the forwarded `read` line names it —
the first guess was gzip's window, a page the guest writes and the
readahead merely bounced) are what found it.

**And the honest measurement:** on loopback it does not pay. A window costs
a page each of take (~15 µs), wire and landing (~10 µs) on the fault's
critical path, against a ~40 µs round trip saved: sha measured +40% per
syscall, gzip −27%, the rest flat — exactly the destination chunk's
finding. So **both readaheads now measure their channel** (an EWMA of the
single-page round trip: `get_rtt_us` on the source, `ctxpage_rtt_us` on the
destination) and engage only above `RA_MIN_RTT_US` = 120 µs — a LAN, where a
round trip is several pages' worth. Loopback reads "a page GET round trip
costs 40 us … readahead OFF" and is back at the baseline (perf.s40j: wc
125 µs/syscall, getppid 63 µs). `VMHOME_READAHEAD=1/0` and `VMR_READAHEAD=1/0`
override the gate (torture.sh passes `VMHOME_EXTRA`/`VMR_EXTRA` through).

Gates: chain **s40p** (gated, OFF on loopback) and chain **s40q** (both forced
ON: 960 source windows, 8623 destination chunk fetches) — suite 34/0,
hx2/pg3/ws1/sig1 48/48 each.

## The Intel destination (.30) carried forward: kernel #55, cross-box green

.30 had been on its kernel #54 (built Aug 20, containing the 6.18.35 tree
only up to the 0065-twin) — unable to measure any of this. The scoping
insight: **.30 is only ever a destination** (home is WSL2 and cannot be a
source), and 0073 (the page record) and 0074 (the TRIED-pass drop) are
source-side; the 6.18 tree's one mmu-notifier arm is the dormant TLB kick,
so 0075 is moot there too. The port therefore reduced to: build the five
committed-but-unbuilt twins (0066–0072 era: cross-task munmap accounting,
the zero-page rule, cross-task SET/PROT refusal, the 6 s deadline, the
zero-folio census) plus a ported **0076 twin** (`VMCTX_MAP_SET_POPULATE`,
committed as e41ffce54 in `src/linux-6.18.35`).

The staging discipline held: the current (proven-booting) vmlinuz/initramfs/
modloop saved as `.known-good` first; `tools/build-kernel.sh` (never a bare
make — LOCALVERSION, matched modloop/initramfs); qemu preflight showed the
documented good pattern (init runs, e1000 loads from a module, the expected
modloop panic); `vmctx/build.sh` staged the module + static tools into
`http/vmctx/`; `reboot-target.sh 10.0.0.30` → **#55** up in ~35 s;
`setup-target.sh` refreshed the target; `verify-build.sh intel` fully green
(same source id as .229: both ends of the wire now run this tree).

**Cross-machine suite (.229 source → .30 destination,
`RUNANY=./tests/runany-x.sh ./tests/suite.sh`): 33/34, then 34/34, then
68/68 over two more rounds** — the one first-round failure was pf3's fpe
case (the #DE child died 242 with no exception line anywhere, module
silent), never seen again in 102 further case-runs: a flake to keep an eye
on, not a wall.

## Cross-box: the readaheads measured where they were designed to pay

A/B on the LAN (.229 → .30, kernel #55, 3 runs each; `VMHOME_READAHEAD=0`/
`VMR_READAHEAD=0` versus the auto gate, which reads the wire at ~300–430 µs
per round trip and switches both sides ON):

| prog | RA off | RA auto | what moved |
|---|---|---|---|
| wc | 0.362 s, 513 fault ev | 0.337 s, 428 ev | wall −7% |
| sha256sum | 0.955 s; syscalls 2.76 ms each | 0.831 s; **2.04 ms** | wall −13%, per-syscall −26% |
| gzip | 5.67 s; syscalls 4.76 ms each | 5.06 s; **3.11 ms** | wall −11%, per-syscall −35% |

The destination chunks cut the fault events (wc 513→428, sha 1031→949);
gzip's destination faults rise (4031→4751, the window's overshoot bouncing
its output buffer) and the total still wins. gzip's output verified
byte-identical against native on three readahead-ON cross runs — the
lost-store class stayed dead. A cross-box fault costs ~360 µs and a
forwarded getppid-class syscall ~450 µs here, so the per-event arithmetic
that made readahead a wash on loopback (35 µs of page work vs a 40 µs trip)
turns decisively: one round trip saved buys ten pages of work.

## Writable-range chunking (kernels #159/#56, patch 0077)

The last readahead half. Three earlier pieces made it sound — a taken page
that cannot be poked lands in the object (never silently dropped), a
multi-page serve answers short on a syscall hold, and the run is populated
in the context's own task — plus one new kernel piece:
**`SET_POPULATE` write-faults the run by hand** (patch 0077, kernels #159
on .229 and #56 on .30), because `mm_populate()` refuses shared-mapping
write faults and installed read-only entries, which would have cost a
writable chunk a protection exit per page.

The window is split by what a wasted page costs: a **read-only** neighbour
is a copy both sides keep (~15 µs against a ~400 µs trip — worth fetching
on speculation, floor 2 pages; evidence-first cut wc's chunks from 171 to
10 and gave back the win), while a **writable** neighbour is a TAKE the
source loses and a wrong guess bounces — fetched only on evidence (floor
1, the second sequential fault starts the window). A contended single page
(hx2's) never chunks: the same page twice is not a sequence.

Gates: chain **s40r** (writable chunking forced ON) and **s40s** (the
split-floor final form forced ON) — suite 34/0, hx2/pg3/ws1/sig1 48/48
each, wait_timeouts 0; cross suite on #159/#56 **34/34**; gzip cross-box
byte-identical. Cross measurements: python startup (its /tmp script had
been cleaned away, so both arms measured the same startup workload)
−6% wall with 356 writable chunks; wc back at 428 events/171 chunks;
sha/gzip as before.

## Instruments added this session (all kept)

- `task_photo()` in vmhome: state letter, wchan, SigPnd/ShdPnd of a service
  task — on `fault service ends (… -- ended by WAIT/RESUME: errno; the task
  now: alive=N state=X wchan=…)` and on `ctx_call FAILED`.
- `oracle_put` prints the service process's wait status. **Trap:** two conn
  threads race there at the end, so "had ALREADY ended … killed by signal
  9" is the loser seeing vmhome's own SIGKILL — it cost an hour as a false
  "early kill".
- `SLOW PULL` / `SLOW CTXPAGE` (above).
- How to find a signal's sender: `echo 1 >
  /sys/kernel/tracing/events/signal/signal_generate/enable` (filter
  `sig==9` etc.), run the pool, read `trace`. `res=1` = the group was
  already exiting. `zap_process`/`do_group_exit` SIGKILLs are not traced.

## Warts noted, not changed

- A genuinely fatal exception still ends the service process with
  `kill(pid, SIGSEGV)`, which is delivered only when a service thread next
  returns toward user mode (all of them park in killable waits). The guest
  side is ended by the destination regardless, so the suite's signal cases
  pass; but the program's parent sees the death late.
- The kernel's in-band clear-tid write at a service thread's exit
  (`vmctx_finish` → `put_user(0, ctid)`) is now redundant with vmhome's own
  `CLEAR-TID` poke a moment earlier, and costs one more bounce of the
  descriptor page per thread exit; rarely (1 in ~200 pool runs) its fault
  is unanswered for the 6 s deadline inside the dying thread
  (`vmctx_cleartid_failed` 128 over the session). The run is not affected
  (the joiner was already woken) — `fault_deadline_hits` 1 per chain is
  this. Candidate: drop one of the two, gated by ws1/sig1.

## Deployment state

- Box `.229`: kernel **#159** (`~/build-159.sh`; `~/kernel-15{5,6,7,8}-backup/`
  hold the previous sources and vmlinuz), clocksource tsc, GRUB backup
  `grub.bak-s38` still stands; module rebuilt for it. Box `.30`: kernel
  **#56** (the 6.18 tree at 553798c0f; `.known-good` images in http/ are
  the #54 set — still the recovery point). Userspace build
  **c8c2fd3421c4** everywhere (LEGAL fix, SLOW/HOLDERS instruments, both
  readaheads RTT-gated with force switches, writable chunking);
  `verify-build.sh amd` green once the first run loads the module. The
  scratch tree `src/linux-7.0.14` has commits for 0073–0076 (author Bi Wu)
  matching the box's `~/vmctx-build` source (kernel/vmctx.c and the uapi
  header, which `sync-amd.sh` does NOT carry — scp it with the .c). Outputs under `/home/biwu/`:
  `chain.s40{f,g,h}.out`, `torture.hx2.s40{a..i}`, `sigtrace.s40{c,d}.txt`.
- Uncommitted per convention: `user/vmhome.c`, `user/vmremote.c`,
  `user/vmrproto.h`, `local-here.sh`, `tests/perf.sh`, `NOTES.md`,
  `ARCHITECTURE.md`, `kernel-patches/linux-7.0.14/007{3,4,5}-*.patch`, this
  file (plus session 39's still-uncommitted set and tests/perf*.sh).

## NEXT

1. The pf3-fpe cross flake (1 in ~136 now): catch it with dmesg streaming
   on .30 when it next appears.
2. The in-band clear-tid redundancy at thread exit (a descriptor-page
   bounce per thread exit, the rare 6 s stall inside a dying thread);
   gate with ws1/sig1.
3. Cross-box pools (a torture.sh twin over runany-x) would give the
   readaheads a contended cross-box gate; today's cross gate is the suite.
2. The in-band clear-tid redundancy at thread exit (the "warts" above):
   one of the two clears can go, gated by ws1/sig1 48/48.
3. The Intel port (.30) is still at #54 and lacks the record, 0073–0075.
4. Commit: squash per the author/committer convention. 0073–0076 are
   in `kernel-patches/` (scratch-tree commits ffb7bd585 and successors);
   the uncommitted set is listed under "Deployment state".
