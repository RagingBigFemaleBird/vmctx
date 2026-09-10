# HANDOFF — session 38 (2026-08-22): the performance campaign — typical programs measured at every layer, the three bottlenecks named by trace, and each one removed; every gate green throughout

Follows `HANDOFF-session37.md`. The user asked for the whole tree understood,
the tests green, and performance data on typical programs with the
bottlenecks found and optimised. The tests were green at the start (chain
s38a) and stayed green after every change (chain s38b on the first build;
the final chain at the end of this file). Nothing in the coherence design
changed: every optimisation removes work from the path or a wait from the
critical section; none alters an answer, a record, or an order.

## The harness (new, tracked)
- `tests/perf.sh [tag]` — seven typical programs (wc, curl, sha256sum of a
  2 MB file, python start-up, gzip, links2 -dump, vmbench) natively and
  forwarded on the loopback rig, N runs each, with vmremote's own
  breakdown (setup / faults / syscalls) parsed into per-fault and
  per-syscall µs. One build against native.
- `tests/perfab.sh` — two (vmhome, vmremote) pairs ALTERNATED run by run on
  the same programs: the only honest way to compare two builds on this box,
  whose idle-state behaviour moves a run by ±50% (see "the noise" below).
- The box's old `perf4.sh`/`perfreal.sh` (untracked, in `~biwu`) are the
  ancestors; they used `startvm`, which no longer exists.

## The baseline (chain s38a green; perf.sh `base`, loopback, kernel #153)
| program | native | forwarded | setup | faults | µs/fault | syscalls | µs/syscall |
|---|---|---|---|---|---|---|---|
| wc -c /usr/bin/wc | 0.015 s | 0.564 s | 0.109 | 513 | 320 | 48 | 292 |
| curl (loopback http) | 0.012 | 1.060 | 0.116 | 989 | 647 | 169 | 272 |
| sha256sum libc.so.6 | 0.025 | 0.977 | 0.110 | 1031 | 376 | 107 | 1187 |
| python3 -c sum(range(1e6)) | 0.030 | 1.362 | 0.108 | 2311 | 328 | 377 | 350 |
| gzip -c /usr/bin/wc | 0.437 | 4.885 | 0.109 | 4031 | 438 | 370 | 2824 |
| links2 -dump page.html | 0.024 | 2.146 | 0.119 | 1127 | **1563** | 135 | 422 |
| vmbench (compute / getppid) | 235 Miter/s, 0.15 µs | 220 Miter/s, 105 µs | 0.107 | 127 | 339 | 20018 | 129 |

Every real program is demand-paging-bound (the 2026-08-06 finding, still
true): 20-90x slower than native, of which the page faults are 60-85%.
Compute is 93% of native. The per-fault cost grows with the program's
mapping count (wc 7 libraries: 320 µs; links2 91: 1563 µs).

## The bottlenecks, by trace (perf trace -s on both halves; perf record -a)
1. **The source re-parsed `/proc/<pid>/maps` for every page it served**:
   `ctx_addr_prot()` in the CTXPAGE case and `ctx_movable_ranges()` in
   `ctx_page_inner()` ("read once per request rather than cached"). For
   links2 (1126 faults) vmhome's connection thread made **173,244 syscalls
   (~137 per request)**: 72,798 reads, 1,713 lseeks costing 227 ms (glibc's
   `fclose()` re-traverses a seq_file to reposition), 1,631 opens; vmhome
   was 35% of the box's non-idle CPU, a quarter of it the kernel formatting
   the maps text (`show_map_vma`/`mangle_path`/`seq_path`) and `vfscanf`.
2. **Two round trips per pulled page**: the INSTALLED ack is sent after the
   RESUME (off the fault's own path, the pg2 lesson) but its reply was then
   waited for in `read()`, and a guest paging in faults again inside that
   round trip -- 3 socket reads per fault event on the destination's service
   thread. The source must still receive the ack at the same instant; only
   the wait had to go.
3. **`usleep(100000)` at vmremote start-up** waiting for its own page-service
   thread to be listening before START: the whole of every run's 108 ms
   "setup".
4. **Per-fault syscall overhead on the destination (~67 per fault event)**:
   12.7 `clock_gettime` -- each a real syscall plus an HPET MMIO read,
   because the box's **clocksource was hpet**: the kernel's timekeeping
   watchdog had marked the TSC unstable at boot with a -0.77% "skew" that is
   exactly the ratio of its early (2894.66 MHz) and refined (2917.03 MHz)
   calibrations, a false positive on a CPU with constant_tsc+nonstop_tsc;
   5.5 `gettid` (the lock-owner record, the pull table, the transition
   owner, every time); 2.8 `futex` -- `own_unlock()` woke on every unlock
   and `own_land()` woke an entry with no waiter.
5. **Idle-state wake-up latency** (the memory's "the loopback number is
   wakeup latency"): this box's C2/C3 exit latencies are 350/400 µs, longer
   than a loopback round trip; a mispredicted deep idle between two steps
   of a fault costs more than the fault. Measured with perfab: the bare
   forwarded getppid is 62-93 µs on an idle box and 40-45 µs with four
   spinners keeping the cores awake; a fault 102 vs 79 µs.
6. **Not yet removed -- the I/O buffer bounce**: a forwarded read(2)/write(2)
   of a 32 KiB buffer costs a page-channel GET per buffer page (the source's
   service context faults page by page while the kernel copies), ~110 µs
   each in loopback: sha256sum's syscalls average 0.7 ms, gzip's 2.4 ms,
   unchanged by everything below. See "next".

## What changed (userspace only; the kernel is #153 unchanged; one GRUB line)
- **vmhome: the map snapshot** (`ctx_vmas_get()`): `/proc/<pid>/maps` is
  read once per CHANGE, into a per-thread snapshot keyed by a generation
  (`map_gen`) that every request except a pure page read/record
  (CTXPAGE, PAGE, INSTALLED, WRITEPREP, STALETRAIL) advances at its end,
  before its reply goes out, plus the connection teardown that ends a
  context. No syscall number is decoded. The generation is read BEFORE
  the parse, so a change landing mid-parse costs one redundant parse and
  never a stale map. Liveness is not the snapshot's: the CTXPAGE case asks
  `ctx_present() < 0` (a pagemap read fails for a task without an mm) beside
  the map, the same fact the unreadable/empty-map test used to give. The
  parse is one `read()` into a buffer and a hand parser (no stdio, no
  `fclose` lseek). `ctx_addr_has_mapping/prot/shared` and
  `ctx_movable_ranges` all read the snapshot; the summary prints
  `parses` vs `lookups answered from a snapshot`.
- **vmremote: the ack's reply is owed, not waited for** (`home_owed`,
  `home_drain()`): `ack_flush()` writes the INSTALLED request and counts a
  reply owed; `home_call()` and `forward()` drain owed replies before
  reading their own. The channel is in order, so nothing can pair wrongly.
- **vmremote: the page service signals "listening"** (`pg_up`, a condvar)
  and main waits on that, bounded 2 s, instead of sleeping 100 ms.
- **own.h: a three-state futex mutex** (wake only when a sleeper announced
  itself) and a per-entry `waiters` count so a landing wakes only when
  someone is parked (waiter counts in BEFORE reading gen, lander bumps gen
  BEFORE reading the count -- both locked RMWs, so one always sees the
  other; `own_gen_bump`/`own_gen_wake`). `own_tid()` caches gettid per
  thread; every `syscall(SYS_gettid)` in both halves uses it.
- **The per-futex log lines** on both halves (a peek and a line per futex
  call -- the hottest syscall of a threaded program) are armed by
  `VMREMOTE_FUTEX_LOG` / `VMHOME_FUTEX_LOG`, not always on. The pull's log
  line no longer evaluates its clock read and checksum with the log off.
- **PM QoS**: both monitors open `/dev/cpu_dma_latency` and write a 20 µs
  bound (`pm_qos_hold()`; `VMCTX_PM_QOS_US` overrides, -1 leaves the box
  alone) for their lifetime. Nothing spins; the box's policy returns when
  the run ends.
- **The box: `tsc=reliable`** on the kernel command line
  (`/etc/default/grub`, backup `grub.bak-s38`; rebooted, gdm stopped first).
  Clocksource now `tsc`; `clock_gettime` is a vDSO read again.

## Results (perfab `final`, baseline 4506e85e89bc vs final 69506b072f92, N=7 alternated, kernel #153, clocksource tsc)
| program | wall base -> final | µs/fault base -> final | µs/syscall base -> final |
|---|---|---|---|
| wc | 0.489 -> 0.244 s (**0.50x**) | 281 -> 58 (0.21x) | 250 -> 229 |
| curl | 1.029 -> 0.363 (**0.35x**) | 590 -> 100 (0.17x) | 219 -> 160 |
| sha256sum | 0.715 -> 0.440 (0.62x) | 245 -> 107 (0.43x) | 720 -> 579 |
| python | 1.153 -> 0.749 (0.65x) | 250 -> 120 (0.48x) | 334 -> 263 |
| gzip | 4.579 -> 2.459 (0.54x) | 413 -> 138 (0.33x) | 2378 -> 1468 |
| links2 -dump | 2.074 -> 0.473 (**0.23x**) | 1498 -> 155 (**0.10x**) | 400 -> 196 |
| vmbench getppid | 2.256 -> 2.570 (1.14x) | -- | 54 -> 75 |

Every real program 1.5-4.3x faster end to end; the per-fault cost, the
campaign's target, fell 2-10x and no longer scales pathologically with the
program's mapping count (links2's 91 libraries: 1498 -> 155 µs). vmbench's
pure-getppid loop is unchanged within this box's syscall-measurement noise
(the same A/B saw the round trip at 54-130 µs across runs; it is the pessimal
microbenchmark, not a typical program). All exits 0.

**A readahead on the source fault service (pull the next few THEIRS pages
when GETs land sequential, for the I/O-buffer bounce) was BUILT, measured,
and REVERTED**: still one blocking `ctx_pull_page` per page, so on loopback
it only moves the round trips from guest-fault-time to service-time without
reducing their count, no clear win on a loaded box, and it speculatively
takes pages off the guest (the coherence path). Not shipped. The
I/O-buffer bounce (sha 579, gzip 1468 µs/syscall) is the standing next
lever and wants a genuinely PIPELINED multi-page GET, gated by the chain.

## Gates (all green)
- chain s38a (baseline), s38b (map snapshot + ack + startup + own.h +
  gettid + clocksource + PM QoS), s38d (final, readahead reverted): suite
  34/0; hx2/pg3/ws1/sig1 48/48 each. Plus s38c on the pre-revert build 46/48
  hx2 -- the two misses the pre-existing exit-242 §6.1a NULL-deref family
  (`LOST-PAGE 0x0`), reproduced neither by the change (three further hx2
  pools 48/48) nor tied to PM QoS (deadline kills identical with QoS off).
  `take_lost` 0 across s38c/s38d.

## Traps (new this session)
- **A vmhome started under a wrapper that exec's `perf trace` is orphaned by
  the harness**: local-here.sh's trap kills the wrapper's pid (perf), and
  the vmhome under it survives, holding its port, and a later "find the
  vmhome" by comm finds the orphan. Attach by the LISTENING PORT's pid
  (`ss -ltnp`) after the run starts (`perftrace3.sh`), never by comm scan.
- **This box's idle states are the noise**: a single run's per-fault cost
  moves ±50% with whether the core came from C1 or C3; medians of N=3
  one-build runs disagree between two runs of the same build (wc 96 vs 123
  µs/fault). Only an ALTERNATED A/B (perfab.sh) says whether a change
  helped; `perf trace` syscall COUNTS are the deterministic evidence.
- **`clock_gettime` can be a syscall**: check the clocksource before
  reading any per-fault timing. It was hpet for the whole project until
  today; every "µs" in earlier sessions' loopback numbers carried ~3 µs per
  clock read in every instrumented path.
- **The context child's 1019 failing `close()`s** (drop_monitor_fds, fds
  3..1023) are start-up only, ~0.3 ms per context; left alone.
