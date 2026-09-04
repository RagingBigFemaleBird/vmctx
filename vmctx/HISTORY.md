# vmctx — how it got here

The development history, condensed after the 2026-08-24 squash. The full
pre-squash commit messages — every intermediate step, in order, with its
reasoning — are archived verbatim in `doc-archive/` (repo log and both
kernel logs). The per-session record is `HANDOFF.md` (the consolidated ledger; the
original per-session files are verbatim in `doc-archive/handoffs/`);
the standing rules distilled from all of it are in `PRINCIPLES.md`, the
current design in `ARCHITECTURE.md` and `REDESIGN.md`, and the traps in
`PITFALLS.md`. This file is the arc.

## What the project is

Run an unmodified Linux program so that its *instructions execute on one
machine* (the destination — hardware VM context, AMD/SVM or Intel/VMX)
while its *memory, files, and operating system live on another* (the
source, which loads the ELF, adopts the program's task, and answers every
page fault and syscall over a TCP protocol). Two lab targets: 10.0.0.229
(AMD, Ubuntu, self-built 7.0.14 kernel, also usable as its own loopback
destination) and 10.0.0.30 (Intel, netbooted Alpine 6.18.35, disposable —
it owns nothing). End goal: a graphical browser started on the source,
executed on the destination, near-native.

## The arcs, in order

**1. Foundations.** A VM context as a first-class Linux process: the
`vmctx_run`/`vmctx_ctl` syscalls, SVM and VMX backends, the monitor that
services a context's events, and remote execution split into vmhome
(source, owns the program) and vmremote (destination, owns nothing). The
early rulings that shaped everything after: the destination has no concept
of a file; every syscall is forwarded and executed in the program's own
task on the source (arguments only mean something there); the source runs
the real clone and the destination stands up execution contexts on
instruction, never by parsing clone flags.

**2. Coherence — the lost-write campaign.** The longest arc. Oracles were
built before fixes (ws1, hx1, hx2 — hx2 measures ~2 lost stores per
*billion*), and each producer was photographed before it was closed:
shared memory became single-writer with write-protect-driven coherence
and one writable copy; "map, don't copy" (MAPOBJ) replaced page copies
that forked ownership; the two-phase hand-over (PG_INTRANSIT + deferred
INSTALLED ack) made a page's machine-crossing atomic; the take-generation
stamp ended stale retained serves; the guest register file took ownership
of FS/GS bases after threads were found running on the monitor's thread
pointer. Three separate attempts to fix ordering with source-side gates
or clock-like tie-breakers regressed the oracles and were reverted — the
project's central negative result: *a stamp that picks between two copies
masks a state the one-writable-copy invariant forbids* (REDESIGN.md).

**3. Fork, exec, threads.** Fork went from an eager copy to real COW with
the break executed where the page lives; the adoption invariant rebuilt
exec and clone fault handling; exec wipes stale present pages and drops
the dead layout; cross-task APPLYMAP SET/PROT is refused in the kernel
after the monitor's own mm was found carrying guest mappings; a fork
copy's private file pages come from the parent's *diverged* copy, and a
fork of a fork inherits through the ancestor chain.

**4. Propagation by mm hook, not syscall decode.** The owner's ruling:
address-space destruction is reported by the kernel's mm-mutation hook
(mmu notifier change log) and applied as vacates, never enumerated by
syscall number — every hole in such a list (mremap was one) is stale
memory somewhere. The ordering of vacates against constructions was
eventually settled by the kernel stamping events *at the event* with a
per-mm counter, replies carrying the counter as the construction stamp,
and the destination keeping a construction record that can skip a stale
vacate and replay a punched mapping.

**5. The page record moves into the kernel.** The per-(mm,page) ownership
record became a kernel xarray beside the page tables, with SERVE / LAND /
PGACK / PGSCAN / PGSTATE / PGSET ctls; the fault hook claims a page before
the monitor is asked and a sibling's fault waits in-kernel. This halved
vmhome, took /proc/maps off the page path, and turned a serve from five
syscalls into one.

**6. Performance.** Measured layer by layer with `tests/perf.sh` /
`perfab.sh` / `vmbench`: compute runs at 99.6% of native (loopback) and at
the destination's own native speed cross-machine; a forwarded syscall or
page fault costs one round trip (62–82 µs loopback; 259 µs cross against a
213 µs wire floor, down from 482 µs before the serve-path work); RTT-gated
readahead pays only where the wire does.

**7. Third-party access.** A census at handle_mm_fault split faults on a
context's memory by who made them; foreign reads of missing pages were
already refused, attributed third-party writes (ptrace, /proc/pid/mem,
process_vm_writev) became refusals too, and the census still names what
remains unattributed.

**8. Namespaces and the browsers.** links2 renders on both transports and
is the calibrated demo (+253 colours). netsurf fell wall by wall:
fontconfig mapping, dlopen boundary page, heap-metadata coherence, the
glycin/bwrap sandbox — whose pid namespace exposed that an assisted
clone's return value is namespace-relative while the monitor addresses
host pids (fixed by the kernel recording the host pid for the monitor,
with the guest still seeing its namespace's numbers) — then the recycled-
hole vacate ordering (arc 4's conclusion). netsurf now paints and fetches;
its remaining stall is a page stuck in CLAIM/TRANSIT when a churned
connection dies mid-transition (recorded, parked), as is dbus-launch's
loader-family death and dillo's ld.so death. `browser-demo.sh` is the
self-grading no-sandbox demo, green by construction.

**9. Time.** A guest's vDSO read the vvar page as ordinary guest memory —
and the source cannot serve vvar OR vdso text by any userspace means, so
the guest got zeros (a static guest's libc silently rejected the vDSO and
used syscalls; a dynamic guest's resolved pointers executed real text over
zero data and time(2) read 0, killing TLS). The fix is two arms chosen by
fact: same kernel release, the destination serves its OWN live
vvar/vdso — bit-identical image, and the guest's rdtsc already reads the
destination's TSC (the backend never writes tsc_offset) — with a 3x/s
refresher re-poking the data pages, because a snapshot's coarse clocks
freeze (fc1's time-bounded loop measured that as a 3/3 suite regression
the day the vdso became real); differing releases, the source hides the
vDSO from the first image at the exec stop (ld.so caches AT_SYSINFO_EHDR
as its first act) and clocks go by forwarded syscall. tests/vd1.c owns
both holes: every clock against the forwarded syscall, plus time() must
ADVANCE.

**10. Breadth as the bug-finder.** tests/progs.sh runs real programs
native-vs-guest, each with its own predicate, and its expansion to 66
shapes is what keeps finding the next defect: pyfork caught a forked
child's deliberate exit code becoming the run's status (fixed — only a
context that JOINED an address space is a thread death), lddnames pinned
the argv-stale exec hole to a 2-in-4 reproducer, nc1 compressed the
deep-teardown 242 family to five seconds, and gdbbt/stracec photographed
that ptrace-inside-a-guest forwards mechanically but means nothing
semantically (the traced task on the source never runs). Its KNOWN_GAPS
ledger names each standing family so only surprises read as failures.
The five-second reproducer then finished the job the same day: the 242
family resolved into three named producers — a fork child's TLS served
as ZEROS (fixed: the inheritance is the parent's page read through its
own task; "let the kernel's COW serve it" was built first and refuted,
because a page the other machine held at clone time is a hole here and
the COW of a hole is the zero page), the roaming argv-stale exec hole
(kernel-blocked), and the orphaned daemon's service dying with its
parent's monitor process (the parked double-fork wall, mechanism now
exact).

## Where things stand

Gates all green: the 40-case suite loopback and cross-machine, the
hx2/pg3/ws1/sig1 pools 48/48 each with take_lost=0, and the 66-shape
program breadth harness with its known-gap families named. The kernel work
ships as one patch per tree in `kernel-patches/` (7.0.14 for the AMD box,
6.18.35 for the netboot target). The open frontiers are listed at the end
of `HANDOFF.md` (§0 and §3).
