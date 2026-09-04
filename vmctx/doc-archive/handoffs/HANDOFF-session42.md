# Session 42 — netsurf's three walls: the pidns pid, the fork-of-fork COW chain, and the vacate that outranked its successor

Continues HANDOFF-session41.md (session 41g mapped the assisted-clone
pid-namespace gap; nothing was coded). Directive: continue optimization and
fixes, get everything green, think architecturally.

## Wall 1 — the pidns pid gap: FIXED (kernel #167, patch 0084)

Exactly as 41g mapped it. An assisted clone that runs inside a pid
namespace returns the NS-RELATIVE pid — the guest's right answer, the
monitor's wrong name (host pid 2 is kthreadd; the bwrap payload fork parked
in vmctx_report unadopted, forever).

- Kernel: `vmctx_clone_noted()` — kernel_clone(), once the pid exists
  (vmctx_copy_task runs before alloc_pid, so it cannot), records the
  child's INIT-NS pid on the parent's vmctx_task. `VMCTX_CTL_LASTCHILD`
  (op 28) reads it back. One slot per task is enough: a task runs one
  syscall at a time and vmhome asks back-to-back with the clone.
- vmhome: after an assisted clone, reads the host pid and uses it for
  EVERYTHING this side does (as_of, GETREGS retry, fork_rel_note, and —
  via the reply — the destination's OP_CHILD announcement); `retval`
  keeps the ns view for the guest's RAX. Loud line when they diverge.
- Protocol: `vmr_rsp.host_pid` (a clone reply names the child twice);
  VMR_MAGIC bumped to **VMR5** (the struct's size changed — a VMR4 peer
  would misframe every reply).
- vmremote: `spawn_forked_context`/OP_CHILD announce by host_pid,
  falling back to retval when 0.
- **tests/pid1.c** (in suite after ns1): three generations, sandbox is
  pid 1 of CLONE_NEWUSER|CLONE_NEWPID, forks a payload that must run AND
  see ns numbering (getpid()==2, fork returns 2 — a fix that leaked host
  pids into the namespace would fail it too); the old hang is converted
  to a loud SIGALRM death. PASS under vmctx on first #167 run; home.log
  shows the translation live ("child is pid 2 in its namespace, host pid
  7811 here").

## Wall 2 — a fork of a fork inherits through the CHAIN: FIXED (userspace)

With wall 1 down, the same bwrap probe died SIGSEGV: the payload (fork of
the sandbox child, itself a fork of bwrap main) read libc's .data as the
FILE's pristine bytes and died in __libc_fork's child bookkeeping writing
through a NULL list head (+0x10) that main() had initialized long before
either fork. The parent had never held that page — it still inherited it
from the grandparent — so "the parent owes nothing" was true of the
parent's record and false of the chain, and the 41f file-serve fallback
handed the child pre-main() program state.

The invariant that makes the fix a record-read, not a guess: an undiverged
page has ONE set of inherited bytes for the whole subtree, because the
first store anywhere comes through the break and gives first. So:

- vmremote's COWBREAK handler and both internal PULL_COWBREAK sites now
  walk ancestors (`cow_give_from_chain`) to the NEAREST one still holding
  the fork's OWED mark — that ancestor holds exactly what the copy
  inherited. Counted (`n_cow_chain_given`) and named ("given by ancestor
  X, N hop(s) above the parent, which never held it").
- `cow_give_page()` now marks the GIVEN page OWED for the receiving copy
  — the same state the fork-time protect pass records for an object-held
  unmapped page — so a break that reaches the original's direct copies
  leaves the page reachable for grandcopies that fault later (without
  it, they found every ancestor either spent or empty-handed).

Gate: the glycin-shape probe `bwrap --ro-bind /usr /usr ... --unshare-all
--die-with-parent /usr/bin/true` under local-here: **rc=0**, with exactly
one chain-give (the libc page, 1 hop up).

## Wall 3 — the recycled hole's stale vacate: FIXED (kernel #168, patch 0085)

netsurf then ran its real startup work and died at the FONT SCAN:
munmap(font A), mmap(font B) → the kernel hands back the recycled hole,
and the first read of the fresh mapping answered -14 → exit 242; main
parked in a forwarded futex for the dead worker; the 10x10 stub window
forever. The dmesg corpse names it exactly: `mov (%rdx),%eax; bswap` at
rdx = the mapping's first byte (FreeType reading the sfnt magic), recent
syscalls `... munmap, openat, fcntl, fstat, mmap, close`, then the dead
read.

Root cause — the ordering stamp itself: vacate sequence numbers were
assigned at DRAIN time in vmhome, and the change log is per-mm, drained
by ANY connection's forwarded call. Two threads drain concurrently; the
later stamp lands on the earlier event; the recycled range's stale vacate
outranks the mmap that superseded it; and the destination's skip rule
("a vacate never touches a construction stamped newer") then LEGALLY
punches the live mapping in every member. The srccons machinery had
already conceded this in its own comment ("a sequence number assigned at
DRAIN cannot order it") but could only cover MAP_FIXED, whose target is
named in the call's arguments; a natural mmap's address is unpublished
until its reply.

Kernel #168: the hook stamps each event AT THE EVENT, under the log's
lock (`vac[].seq`, per-mm counter, bumped for dropped events too);
VMCTX_CTL_MMLOG returns per-entry seq plus `cur`, the counter at the
drain read under the same lock. vmhome ships the kernel's stamps
(vacate_pend[].seq) and uses `cur` as the reply's construction stamp
(last_mmseq); the userspace counter is deleted. Airtight for this shape
by construction: a construction's address is unpublished until its reply,
so no event that outranks it can precede the sample. The destination's
skip logic is unchanged — it finally gets stamps that mean what it
already assumed.

- **tests/fc1.c** (in suite after pid1): three same-size files of
  distinct bytes scanned fontconfig-style (mmap, read, munmap; the hole
  reuses) while a churn thread floods vacates past one reply's 8 and
  competes for the drain. A punched mapping dies loudly; a stale page
  names the file it belongs to by its own bytes. Native: 51k rounds, 92%
  hole reuse, PASS.

## Wall 3b — what fc1 then measured out of hiding (four more defects, one afternoon)

fc1 under the suite failed ~3 runs in 8 even on #168. Each fix below was
driven by an instrument that named the next one; the end state is fc1
24/24 and suite 39/0.

1. **The cons ring evicted live rows in milliseconds.** The churn thread's
   thousands of anon mmaps pushed every other construction row out of the
   1024-slot ring, so both the vacate skip and any repair went blind
   exactly when the race needed them. Fix: a construction of the SAME
   (as, start, end) REPLACES its row (recycled holes stop consuming
   slots); a live row evicted by wrap is now counted and named
   (n_cons_evicted_live -- zero since).

2. **The tie was read backwards.** A construction's stamp is the counter
   at its drain, which EQUALS the last event's seq -- so a construction
   right after event S carries S, and both the skip (`>`) and the row
   kill (`<=`) treated the tie as the vacate's win. Photographed
   (ROW-KILL: row seq=12975 killed by vacate seq=12975): the fresh 2 MiB
   row killed AT BIRTH by the churn's 8 KiB vacate of the very hole it
   landed in. On any overlap the kernel's own address allocation proves
   the order -- the mmap could only land there because the unmap had
   completed -- so a TIE IS THE CONSTRUCTION'S. Both comparisons strict
   now.

3. **The record repairs what the race still punches.** Check-then-punch
   (vacate) is not atomic against note-then-RESUME (SET), and no
   userspace lock can reach the SET application (it happens in the
   kernel at RESUME; a cross-task APPLYMAP SET is refused by design). So
   cons rows now carry op/prot/off -- enough to REPLAY the construction
   -- and a fault that would otherwise decline, whose task has NO VMA at
   the address (VMCTX_CTL_TRYFAULT, asked at decline time), inside a
   LIVE row, is answered with the row's own map SET via the existing
   fault_map plumbing (n_cons_repaired). A live row is proof, not a
   guess: every legitimate vacate sees and kills its rows, because the
   guest cannot name an address before the mmap's reply publishes it.
   And because the punch can land even AFTER the decline (between it and
   the local service), the MODULE grew the second chance: a fault the
   monitor declined that the local kernel then cannot serve is REPORTED
   AGAIN, once, bounded per address (svm_run/vmx_run.unrec_*) -- by
   then the absence is settled and the repair fires.

4. **A skipped vacate must not preserve the old tenant's BYTES.** With
   the mapping protected, the object still held the previous
   incarnation's pages, and fc1's oracle read the churn thread's 0x01
   out of its new FILE mapping ("neither file's bytes"). The vacate
   cannot tell the old tenant's pages from the new one's -- but the SET
   can: a fresh non-shared mmap's range is undefined-until-faulted by
   definition, so reply_note_map now punches the object for exactly that
   range BEFORE the mapping goes live (obj_forget at case 9). Everything
   under a new SET is the old tenant's; nothing the new one writes
   exists yet.

## netsurf after all of it: IT PAINTS

Deaths: GONE. The glycin bwrap sandbox chain runs to completion INSIDE
netsurf (worker forks bwrap, bwrap forks the pidns sandbox child, both
exit 0, worker reaps them). And at LIMIT=400 the stall resolved as SLOW,
NOT STUCK: the real 1000x700 "about:blank - NetSurf" window, root
colours 767 -> **5488** (links2's rendered baseline was +253), and the
harness's HTTP server logged netsurf's favicon GET -- the fetch stack is
up. Session 37's diagnosis ("never opens a fetch socket, glycin sandbox
hang") is closed end to end.

The slowness was the environment plus the walls' leftovers: dbus-launch
did not exist on the box (every autolaunch exec ENOENT, one failing fork
per GTK D-Bus consumer, thread churn between attempts), and GTK's icon
pixbufs still fail ("Could not load a pixbuf from icon theme"). dbus-x11
is INSTALLED now (1.16.2-2ubuntu4); the first with-dbus run's numbers
belong to the next session if this file predates them. The end-of-run
-14 I-fetch storm in a LIMIT'd run is teardown collateral (kill -KILL of
the group mid-flight; "monitor gone" contexts), not a wall.

## State

- **Chain s42a GREEN ON EVERYTHING**: suite 39/0 (pid1 + fc1 in),
  hx2 48/48, pg3 48/48, ws1 48/48, sig1 48/48; take_lost=0,
  fault_deadline_hits=0, every takeobj_zero named. fc1 standalone
  24/24 after the wall-3b campaign (was ~5 fails in 8).
- .229: kernel **#168** (build-16{7,8}.sh; kernel-16{6,7}-backup hold the
  prior vmlinuz); module rebuilt (vmctx_task grew a field at #167, and
  the second-chance report is module code).
- .30: kernel #56 unchanged (both kernel patches are source-side);
  vmremote/vmhome/dumpvm + the fresh module PUSHED and verify-build
  says "everything deployed matches this tree"
  (/usr/local/bin/vmremote.pre-s42 and /tmp/vmctx.ko.pre-s42 are the
  known-good asides).
- Kernel patches 0084 (#167 LASTCHILD) + 0085 (#168 event-time stamps)
  exported and committed in the scratch tree.

## Late results: the cross suite, and the next wall's 60-second reproducer

- **CROSS-MACHINE suite (s42a): 39 passed, 0 failed** (pid1 + fc1
  included), .30's dmesg clean, zero second-chance reports fired.
- dbus-x11 INSTALLED (netsurf's autolaunch churn needed the binary),
  and **dbus-launch UNDER VMCTX is the next wall's reproducer**: it
  gets far -- prints the session address, spawns the real dbus-daemon
  (its pid in the output) -- then exits 242 with two -14s at lib-range
  addresses (one read err 0x4, one I-fetch err 0x14); inside netsurf
  the same family shows twice at 0x55555555c060 err 0x6 (the exec'd
  binary's own .data, two different pids, one address). Reproducer:

      OUT=/tmp/dbl VMCTX_SHARED=1 timeout 60 \
          ./local-here.sh /usr/bin/dbus-launch --sh-syntax

  fx1's shapes pass, so this carries one fx1 does not: dbus-launch
  double-forks, keeps an X-watch babysitter, and the exec'd daemon
  starts its own threads.

# Session 42b — the ruling, the demo, and two findings recorded unpursued

**OWNER'S RULING: the dbus-launch wall is RECORDED and PARKED — do not
pursue. Demo another way: a browser without the sandbox.**

## The demo: browser-demo.sh, GREEN twice

`browser-demo.sh` (tracked, with `fakebwrap/` beside it) runs a browser
as a vmctx guest and GRADES itself: GREEN = a window, a positive
distinct-colour delta over the empty root, zero unrecoverable faults.
Default program **links2 -g** — a real graphical browser with no
sandbox and no D-Bus:

    === DEMO GREEN: /usr/bin/links2 drew a page (253 colours over the
        empty root, 2 window(s), 0 unrecoverable faults) ===

Twice in a row, rc=0. (+253 is the same rendered-page value the
harness has measured since session 6, so the verdict is calibrated.)
For the GTK path the script also removes both environment tarpits
OUTSIDE the guest: a NATIVE session dbus-daemon whose address the guest
inherits — measured: ZERO dbus-launch forks in the guest with the
address set — and the fake-bwrap PATH shim (glycin loader exec'd
directly; glycin 2.1.1 has no env override). `PROG=/usr/bin/netsurf`
selects netsurf under the same treatment.

## Finding 1 (recorded, NOT pursued): a page STUCK IN CLAIM/TRANSIT
parks netsurf

netsurf under the demo treatment (bus + shim, zero forks, zero
sandbox) still stalled pre-paint in 2 of 3 runs, and the run's own
report names the whole stall: **562,582 fault events, 213 of 228
seconds in faults, for only 8,520 pages fetched — and ONE page,
0x7ffff050c000, answered "the source has been claiming this page for
2001 faults in a row and nothing has landed here" for the entire run**,
with four contexts parked 224 s behind it in futex/poll. The fault-rate
storm appeared between nsrun9 (93k faults, PAINTED at LIMIT=400) and
nsrun10/nsdemo1 (600k faults, stuck) — the boxes differ by the
dbus-x11 install, but the stall itself is the stuck transition, not
dbus. The claim/land lifecycle in the kernel record looks covered
(vmctx_pgrec_land runs on every vmctx_redirect_fault exit); the
standing suspect is a **TRANSIT that never resolves** — a serve whose
one-way INSTALLED ack never arrives because the asking CONNECTION died
(netsurf churns short-lived threads; the pools that gate the record
never churn connections). The address sits in the same glib-arena
range family the VACATE-LATE instrument photographs. That is protocol-
lifecycle territory (what heals an unacked TRANSIT / an orphaned
CLAIM when its context's connection is gone) — REDESIGN-adjacent,
deliberately not patched here.

## Performance, re-measured on this build (kernel #168 / VMR5)

tests/perf.sh s42p (native vs loopback, N=3) plus the same programs
cross-machine via runany-x; per-op costs from vmremote's own accounting,
never the harness wall (the clock trap is on record). Raw tables:
/home/biwu/perf.s42p.log and /home/biwu/perfx.s42p.log on the box.

| layer            | compute (vmbench)          | syscall   | page fault  |
|------------------|----------------------------|-----------|-------------|
| native .229      | 236.3 Miter/s              | 0.15 us   | --          |
| loopback guest   | 235.3 Miter/s (99.6%)      | 62-82 us  | ~47 us      |
| cross .229->.30  | 224.6 (.30's own native)   | 259 us    | 320-430 us  |

The cross syscall round trip fell 482 -> 259 us against the #60-era
record (raw ping min 213 us is the wire floor -- sessions 38-42's
serve-path work is the difference). Real-program walls, median
native/loopback/cross: wc 0.010/0.055/0.24; sha256(libc)
0.009/0.133/0.75; python3 1M-sum 0.023/0.285/1.04; gzip 0.44/1.34/4.95;
links2 -dump 0.019/0.106/--. The shape: compute-bound work converges to
native (half of gzip's wall IS compute, at 1.00x); short-lived tools pay
pages x fault-cost + syscalls x round-trip at startup (wc 5x loopback,
24x cross).

## Finding 2 (recorded, NOT pursued): dillo dies in ld.so

`PROG=/usr/bin/dillo`: exit 242 at startup, exception 14 at 0x8 (read,
rip 0x7ffff7fd0ae1 = the dynamic loader; recent syscalls the
openat/fstat/mmap×4/close of library loading; rdi=0, a NULL link_map+8
deref). A loader-stage NULL in a fresh dynamic link — its own
investigation, one photograph on file (/tmp/dillodemo on the box).
1. The dbus-launch reproducer (PARKED): photograph the -14s, extend fx1
   with the failing shape, then netsurf with real dbus end to end.
2. The pixbuf/icon failure ("Could not load a pixbuf from icon theme"
   -- the glycin loader socket conversation is the thing to trace; note
   RUST_LOG never visibly reached the guest, check why).
3. firefox (session 12 notes: real binary /home/biwu/firefox/firefox,
   RUNHOME=/root; /usr/bin/firefox is the snap shim).
4. The VAC-ORDER / ROW-KILL / NO-VMA instruments are capped and cheap;
   they stay. srccons (self-map suppression) may now be redundant under
   kernel stamps — measure before deleting.

# Session 42c — the breadth campaign: 30 real programs, and what they measured

Directive: test more programs (sandbox excluded), fix outstanding bugs and
performance issues. tests/progs.sh is the harness: 30 shapes (text tools,
interpreters, threads, gcc+make+the fresh binary, archive round trips, git,
TLS, crypto, ImageMagick, procfs readers, shell pipelines, rsync), each run
native AND as a guest, each graded by its own predicate (output diff,
roundtrip bit-exactness, or rc).

## Fixed this session

- **The shared-port contract, made unbreakable.** VMCTX_SHARED=1 skips the
  leftover sweep BY DESIGN; the whole contract is "a port per run" -- and a
  caller leaving PORT unset broke it silently: a dying run's vmremote
  reconnected into the NEXT run's vmhome, whose guest then ran on
  unserviced pages (3 of 10 back-to-back pipelines dying at
  0x2d8-off-a-NULL-FILE*; 0 of 10 with distinct ports). local-here.sh now
  picks a fresh free port pair itself when shared mode has no explicit
  PORT. (progs.sh also numbers its cases' ports.)

- **No vDSO in guests (kernel #169+/vma.c + vmhome).** A guest's time
  belongs to the source and arrives by forwarded syscall; the vDSO read
  vvar as ordinary guest memory and __vdso_time() has NO fallback -- a
  guest's time(2) measured 0 and every TLS certificate was "not yet valid"
  (curl exit 60, X509 error 9; clock_gettime survived only because a bad
  vvar page makes IT fall back to the syscall). Exec'd service images get
  no vDSO from the kernel now; the monitor strips the first image's
  [vdso]/[vvar] from the layout and patches AT_SYSINFO_EHDR -> AT_IGNORE
  in its stack auxv. curl https:// end-to-end is green.

## Measured and reverted: the fork-child page wipe (twice)

The campaign exposed a real hole: a service fork child's copied page
tables carry the parent's source-side pages as of their last pull, with no
record rows, so an assisted syscall can read one with nobody asked --
photographed as an assisted execve reading the guest's argv page as
vmhome's own environ ("gzip: missing program name" with the campaign's
sudo command line as argv[0]). The principled fix -- the copy starts empty
of pages -- was BUILT AND MEASURED TWICE and reverted both times:

- Wiping everything (#169): every fork child died in __libc_fork's
  reclaim_stacks on a NULL _rtld_global read -- the file-serve fallback
  hands PRISTINE bytes for pages the LOADER diverged (relocations), which
  no fork parent "owes" (offset 0xfdd9c, the corpse is exact).
- Wiping anonymous only (#170): spawn-heavy programs (gcc, make, git,
  pipelines) wedged at 124s timeouts in the child's first faults.

The wipe is right in principle and blocked on two serve-chain frontiers:
(1) the file fallback must refuse pages the ORACLE diverged at load --
"never handed over" does not mean "the file is current" when ld.so wrote
relocations before any fork; (2) fault service in the clone-to-OP_CHILD
window. #171 reverts the wipe (the comment in vmctx_copy_task carries the
map); the argv-stale hole stays open, rare, and documented.

## Recorded, not fixed (each with a cheap reproducer now)

- **sort(1) with default parallelism: 2/12 pass, rc=251.** GNU sort's
  rapid thread create/join cycles hit the deep-teardown family: a futex
  forwarded into a context whose source task was already dying ("2 a
  dying context" in the EIO accounting), CLEAR-TID storms ("NOT CLEARED,
  joiner waits for ever", poke -3). `sort -n <200k-line file>` is the
  family's best reproducer yet -- 45 s, a stock coreutil.
  `--parallel=1` passes.
- **gpg -c hangs 90 s** (native passes): the agent daemon spawn --
  investigate where it parks (daemonize double-fork + socket).
- Assorted campaign flakes pending the s42i baseline re-run on #171.

# Session 42d — the program breadth campaign, a KERNEL PANIC, and the vDSO plan for next time

Directive arc: "test more programs, fix outstanding bugs and perf." Then,
after a panic: "propose vDSO emulation on vmremote -- source hands a sample
page, remote emulates it (updates tsc); treat the target as possibly a
DIFFERENT OS entirely, so page-specific emulation is a justified exception
to the no-page-specific-code rule."

## The breadth harness: tests/progs.sh (KEEP, committed)

30 real programs, each run NATIVE and as a vmctx GUEST, each graded by its
own predicate (stdout diff / roundtrip bit-exactness / rc). Shapes: text
tools, interpreters, threads, gcc+make+the fresh binary, tar/gzip/zstd/xz/
bzip2/zip round trips, git, curl TLS, ssh-keygen, gpg, openssl, ImageMagick,
ps/top/free/df, shell pipelines, rsync, base64/jq. Run:
`sudo ./tests/progs.sh <tag>`; artefacts under /home/biwu/progs.<tag>/.

## Bugs FIXED and kept (userspace only, all safe)

- **progs.sh PATH resolution**: vmhome execv()s with no PATH search, so
  cases resolve the program with `command -v` before handing it over.
- **local-here.sh shared-mode port**: VMCTX_SHARED=1 skips the leftover
  sweep, so the safety contract is "a port per run" -- but an UNSET PORT
  broke it silently: a dying run's vmremote reconnected into the next
  run's vmhome and its guest ran on invented-zero pages, dying in libc on
  NULL globals (3 of 10 back-to-back shell pipelines at 0x2d8-off-NULL;
  0 of 10 with distinct ports). Fixed two ways: local-here auto-picks a
  free port pair when shared mode has none, and BOTH the picker and
  progs.sh keep ports BELOW the ephemeral range (32768+) -- a listen port
  inside it loses to an outgoing connection's source port and vmremote's
  page-service bind is the loser, minutes later ("Connection refused",
  run parked to timeout). This second point is a REAL correctness fix for
  any concurrent harness.

## Baseline result (kernel #168, before the panic experiments)

progs.sh s42f: **25 of 30 green.** Passing includes gcc + make + running
the freshly-compiled binary, python threads, ALL archive round trips,
ImageMagick, ssh-keygen, openssl, git-less text tools, curl-under-shell.
The 5 failures split into two families:

1. **vDSO / clock** (curltls): a guest's time(2) reads 0 because the vDSO
   reads the vvar page, which on the destination is a frozen/zero copy and
   __vdso_time() has NO fallback -- every TLS cert "not yet valid", X509
   error 9. This is the one the vDSO plan below fixes.
2. **Deep-teardown thread churn** (sort default-parallel 2/12; gpg agent
   90s hang; some pipeline children): rapid thread create/join and
   daemonizing double-forks hit the CLEAR-TID / dying-context-EIO family
   ("NOT CLEARED, joiner waits for ever"; "2 a dying context"). BEST
   REPRODUCER YET for that standing class: `sort -n <200k-line file>`,
   ~45 s, a stock coreutil. `sort --parallel=1` passes. This is the same
   root as ws1/netsurf-stall (REDESIGN territory), NOT new.

## What PANICKED, and the lesson (kernels #169-#172 -- ALL REVERTED)

Two kernel-side ideas were built to fix family (1) and the argv-stale
hole; both are REVERTED and must not be rebuilt as-is:

- **fork copy starts empty of pages** (zap the child's inherited pages at
  vmctx_copy_task): #169 wiped everything -> every fork child died in
  reclaim_stacks on a NULL _rtld_global (the file fallback hands
  LOAD-diverged pages pristine); #170 wiped anon-only -> spawn-heavy
  programs wedged at 124s. Right in principle, blocked on two serve-chain
  frontiers (file pages diverged by ld.so's relocations; fault service in
  the clone->OP_CHILD window).
- **no vDSO for service images** (kernel arch/x86 arch_setup_additional_pages
  skip + vmhome auxv/layout hiding): the KERNEL-side skip destabilized
  every spawn-heavy shape (0/6 where 4/6 passed) AND a later build
  PANICKED the box (booted vanilla, recovered). The measured failure is
  the ORACLE's clock, and the oracle-side hiding (vmhome patches its own
  first image's auxv AT_SYSINFO_EHDR->AT_IGNORE + drops [vdso]/[vvar]
  from the layout) fixed curltls WITHOUT the kernel skip -- but was
  reverted too when bundled with the kernel change during recovery.

Recovery: /boot/vmlinuz-7.0.14-vmctx restored from ~/kernel-168-backup;
box runs #168. The 7.0.14 tree is reset to 20bf9dc40 (== #168 == the
on-disk patch); working tree and running kernel are coherent again.
~/kernel-168-backup is the known-good image; ~/build-16{9,70,71,72}.sh and
their vmlinuz are the panic experiments, keep OFF /boot.

## THE PLAN for next session: vDSO emulation on vmremote (owner's design)

Goal: a guest's vDSO clocks read correct, live time with NO forwarded
syscall, and NO kernel change. The enabling fact, VERIFIED: the backend
never writes VMCB tsc_offset (get_zeroed_page, no TSC code), so the
guest's rdtsc reads the DESTINATION host TSC directly. Therefore the
destination's OWN vvar page -- calibrated to the destination TSC by the
destination's own kernel -- is exactly what the guest's vDSO needs.

Design (ALL IN vmremote, no kernel risk):
1. The source already sends /proc/maps TEXT as the layout, and it NAMES
   [vdso]/[vvar]/[vvar_vclock]. Parse the pathname column in
   region_parse_layout (today it reads only addr+perms) and record which
   regions are vvar-class per address space.
2. At vmremote startup, read the destination's OWN [vvar]/[vvar_vclock]
   pages from /proc/self/maps (vmremote is a normal Linux process on the
   destination; its own vvar is live and TSC-calibrated).
3. When the guest faults inside a recorded vvar region, install the
   DESTINATION's own vvar page bytes (via fault_map/own-object, the same
   mechanism the shared-range re-establishment uses) instead of asking the
   source. The guest's vDSO code (from the source) + the destination's
   vvar data + the guest's rdtsc (== dest TSC) = correct self-advancing
   time, exactly as the destination's own userspace gets it.
4. [vdso] TEXT: keep serving from the source (the guest resolved its
   symbol offsets against the source's vDSO at startup; do not swap it).
5. Liveness: a single dest-vvar snapshot self-advances via rdtsc
   (mult/shift/cycle_last are the dest's own). NTP nudges the dest kernel
   makes to ITS vvar won't propagate; if drift matters over long runs, a
   periodic re-poke of the vvar page from the dest's live one is the
   "update tsc" the owner described. home_call is NOT thread-safe (shared
   fd), so any refresher must poke the guest via ctl, not call home.

CAVEAT to design against (owner's "different OS" point): step 3 assumes
the source's vDSO code and the destination's vvar have the SAME struct
layout. True for our Linux/Linux (7.0.14 source, 6.18.35 dest -- verify
the vdso_data offsets actually match before trusting the numbers; if they
differ, the guest gets WRONG time, worse than slow). For a genuinely
foreign-OS destination with no compatible vvar, the safe fallback is the
oracle-side vDSO hiding (vmhome drops [vdso]/[vvar] + AT_IGNORE the auxv)
so libc uses real forwarded syscalls -- correct but a round trip per
clock. Pick emulation when the layouts match, hiding when they do not;
the source knows both kernels and can decide.

FIRST STEP next session: a tiny guest probe that does rdtsc directly and
compares to the destination host's rdtsc (prove offset==0 empirically),
and a probe that dumps the guest's vvar vs the dest's vvar to CONFIRM the
layout match before building on it. Measure before trusting (PITFALLS).

## Gate state at handoff
- Running kernel #168, proven. suite/pools were 39/0 + 48/48x4 as of the
  session-42 push (fd7417b); NOT re-run after the port fix -- re-run the
  loopback suite once next session to confirm no regression from the
  local-here port change (it should be inert to the suite, which sets
  its own ports).
- Repo: progs.sh + local-here port fix + doc updates committed this
  session (clean message). Kernel trees unchanged from fd7417b's squash.

## BOX BOOT FAILURE (end of 42d) -- READ FIRST NEXT SESSION

The AMD box (10.0.0.229) is DOWN at "unable to mount root" and needs
CONSOLE recovery. Root cause is packaging, not vmctx code:

- ~/kernel-168-backup holds ONLY vmlinuz-7.0.14-vmctx -- never an initrd
  (the backup step only ever copied the vmlinuz). Restoring that vmlinuz
  over the initrd that build-172.sh's `make install` had regenerated left
  a vmlinuz/initrd pair that cannot mount root. "Restore the known-good
  kernel" was therefore only half a restore.
- The genuine known-good boot entry is the STOCK Ubuntu kernel
  (7.0.0-30-generic), which is intact in /boot and GRUB. Recover by
  selecting it at the GRUB menu (Advanced options -> 7.0.0-30-generic),
  as was done for the first panic.

RECOVERY PLAN, next session (do NOT boot 7.0.14-vmctx until done):
1. From the stock kernel, rebuild #168 cleanly from the coherent source
   tree (src/linux-7.0.14 @ 20bf9dc40 == the on-disk patch) with
   build-168.sh, so `make install` regenerates a MATCHING
   vmlinuz+initrd pair. Do not vmlinuz-only-restore ever again.
2. Fix the backup discipline: a recovery image is (vmlinuz, initrd,
   and the modules dir) together, or it is not a recovery image. Back up
   /boot/initrd.img-7.0.14-vmctx alongside the vmlinuz, or just keep the
   stock kernel as the only trusted fallback.
3. Only then resume the vDSO-emulation plan above (userspace/vmremote
   only -- it needs no kernel change, which is the whole point).

The repo and both kernel trees are in a clean, coherent state
(bf04240 pushed; 7.0.14 @ 20bf9dc40; 6.18.35 @ a572abfde). Nothing in
git needs repair -- only the box's /boot does, at the console.
