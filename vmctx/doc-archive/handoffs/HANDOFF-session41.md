# Session 41 — the corner-flake tests, and the two walls they found

Continues HANDOFF-session40.md (everything through the commit **8e3ff2f** —
LEGAL exception, kernels #154–#159/#55–#56, both readaheads, perf harness).
Directive: *"commit and push, then continue optimization and whatever is
leftover with the tests. You may create new test cases to cover hard to
find corner flakes."* The push is done; this file is the after.

## The new cases

- **tests/io1.c** — forwarded-I/O buffer integrity oracle. 64×32 KiB file,
  `byte(b,off) = (37*b+off) & 0xff` (any fixed offset names its block), read
  back through ONE static buffer for 10 s, buffer poisoned 0xAA before every
  read; every byte verified, mismatches classified stale/unwritten/garbled
  with the stale page's source block NAMED. `IO1_SELFTEST=1` forges exactly
  the session-40 gzip failure (one page one block stale) and must FAIL naming
  block b-1 — the instrument's own proof. Native PASS, loopback PASS. This is
  the regression net over "never land a page into a RUNNING context".
- **tests/pf5.c** — pf3's fatal-signal contract under repetition: 25 rounds ×
  3 children (store to NULL, ud2, idiv-by-zero — pf3's exact forms), parent
  requires death BY the signal, failures counted with round numbers. Built to
  amplify the 1-in-136 cross fpe flake into a per-run event.
- Both wired into tests/suite.sh (pf5 after pf3, io1 after rd1).

## What pf5 found before it ever went green

**Wall 1 — every fatal fork child cost 4 seconds** (pf3 12.5 s for three
children; pf5 timed out the suite at 60 s). The trail, each step measured:

1. `SLOW exception step: disposition read took 4035 ms` (the step timers in
   `ctx_deliver_exception`) — the assisted `rt_sigaction(sig, 0, scratch, 8)`
   on the fresh fork-child service context.
2. `/proc/<tid>/syscall` sampling (sampler3.sh on the box): the child service
   task sat in `vmctx_report` from `vmctx_anon_fault` — its copy_to_user
   write to scratch 0x7ffff7ff5000 faulted and waited; vmhome's monitor
   thread for the child sat in **read(fd=12, buf, 0x1000)** the whole 4 s.
3. tcpdump -x on loopback: request op **0x0a = VMR_PG_COWBREAK** for the
   scratch page, reply **16 bytes, status ABSENT, NO page body**, then 3.98 s
   of source-side silence ended by the socket's 4 s SO_RCVTIMEO.

The defect: the **COWBREAK refusal was vmremote's one GET-shaped answer that
did not send the page body** — every other path obeys "a page follows either
way", and `pg_get_op()` reads header + 4096 unconditionally. The scratch page
of a fork child is exactly the page that takes this path every time: record
NONE, address space is a fork copy, parent never held it → refusal. Fixed in
`vmremote.c` (refusal sends a zeroed page). pf3 12.5 s → **0.40 s**.

Dead theory, removed: the "prompt death nudge" (assisted getpid after
`kill(pid, sig)`) — built on the pending-signal-wart theory, measured pf3
12.5 s → 42 s, i.e. WORSE. The wart costs milliseconds, not seconds: the
verdict returns over the channel and teardown follows it. The removal note in
`ctx_deliver_exception` says this out loud.

**Wall 2 — the destination's context table filled at 64.** With the 4 s gone,
pf5 ran in 9 s and FAILED 12/75: rounds 21–24, every child, **consecutively
from child #64**. `MAX_CTX` was 64 and `ctxs[]` is append-only; at the 65th
context `ctx_claim()` answers "not mine" for a brand-new context, nobody
services it, its pages cannot be supplied, and the child crashes in libc on
zero-filled data (segv at 0x2d8 off a NULL FILE*) instead of dying by its own
fault — then exits 242. No earlier test churned 76 processes through one
vmremote lifetime; that is precisely the corner pf5 was built for.

Fix: **MAX_CTX 64 → 256**. Recycling rows at end-of-service was tried and
**REVERTED, measured**: the page service routes an ask for a KNOWN-dead
context into the "this address space has ended, stop" answer, but drops the
channel for a context it does not know — with the row recycled, vmhome
reconnect-stormed 51,000 times against "names context N, which this monitor
does not know" and the run hung. Dead rows are how the dead stay answerable;
the comment above `MAX_CTX` carries both measurements.

pf5 after both fixes: **PASS 75/75**, walls 9.2–39.4 s (see below).

## The SLOW CTXPAGE busy-wait: FOUND and FIXED (kernel #160, patch 0078)

The ~8.6 s stall (pf5 39 s vs 9 s, pf3 24 s vs 0.4 s) was NOT fatal-child
churn and NOT the wait4 syscall hold. The trail:

1. The HOLDERS instrument (session 40) said: startup, ONE task in the AS,
   PARKED (`syscall: -1`, wchan=vmctx_guest_step, rip=_start), record NONE
   — nothing in-flight at all, yet EBUSY for 8×1.07 s.
2. A new give-up diagnostic in ctx_serve_one prints the KERNEL COUNTER
   DELTAS across its own 1 s busy loop: `take_hold +0 take_refused +1000
   serve_busy +1000 hold_present +0` — the take PRECHECK, not the hold.
3. dmesg named the folio: `TAKE 0x4b3000: split of order-1 folio refused
   (-16): refs=5 want=4 mapcount=1 anon=0`. 0x4b3000 is the binary's
   GNU_RELRO page; the folio is a large PAGE-CACHE folio of the freshly
   linked binary, and a DIRTY file folio refuses to split (the split must
   drop its buffers; dirty buffers refuse) until the flusher's timer visits
   the file — up to ~30 s after ld wrote it. Hence the correlation nobody
   could place: only the FIRST run after a deploy/relink stalls; 45
   steady-state runs never did.

Fix (#160, patch 0078): on a refused split of a dirty file folio, kick
`filemap_flush()` on the mapping — async, ratelimited to one kick per
half-second, the mapping read under the folio lock the split already held.
A/B on #160, four relink+first-run rounds: split refusals still occur
(`vmctx_take_split_fail` +1..+11 per round ≈ 11 ms), the kick fires each
round (`vmctx_take_split_flushed` 1→4), `SLOW CTXPAGE` count 0, walls flat
9.7–10.0 s. The class covers any freshly written mapped file — a
compile-then-run workflow, netsurf relocating a freshly built library —
not just the harness. The delta diagnostic in ctx_serve_one stays.

## Harness trap found on the way: a suite run as the WRONG USER grades stale outputs

**The suite must run under sudo** — local-here.sh refuses otherwise ("must
run as root (vmhome ptraces the program)"), and the chains do exactly that.
Run as biwu (this session's mistake, four suites in a row), every case's
local-here refused instantly — and the grading then read `/tmp/tr/<case>/
guest.out` from the LAST REAL (root) suite run: **105 stale passes reported,
zero cases executed, three suites in a row.** The only tell was pf5, whose
stale guest.out was empty (it had genuinely timed out back then, pre-fix):
it alone said NO-OUTPUT while everything else "passed". A wrong-user suite
against CLEAN /tmp/tr says 108× NO-OUTPUT — loud and honest — so the lie
needs leftovers, and leftovers are the normal state between runs.

Hardening that came out of it (all kept):
- `runany.sh` refuses to run when `$OUT` is unwritable or leftovers cannot
  be removed (exit 3 = harness fault), and `.timedout` joins its leftover
  rm list — a stale timeout marker must never grade a fresh run.
- `sync-amd.sh` wipes `/tmp/tr` on every sync (root), and rebuilds the
  tests as biwu with only the rm under sudo — mixed-owner ping-pong of
  `/home/biwu/tests` was what made "ld: cannot open output file" appear and
  disappear between syncs.

## State

- **All gates green.** Loopback suite (s41f) 108/0 with io1+pf5; the
  CROSS-MACHINE suite (s41x) 108/0 with dmesg on .30 clean — 225
  cross-machine fatal children without the fpe flake reappearing; chain
  s41g (on #159): suite 36/0 + hx2/pg3/ws1/sig1 all 48/48. That set is
  **pushed as 9e33726**. Chain s41h (on #160, gating patch 0078 + the
  delta diagnostic) ran after; its result and the second push are the
  session's last acts — check chain.s41h.out if this file predates them.
- pf3 0.40 s; pf5 PASS 75/75 at ~9.8 s flat (the 39 s outliers were the
  now-fixed dirty-folio stall); io1 PASS native+loopback, selftest proves
  the instrument.
- Changed, uncommitted: `user/vmremote.c` (COWBREAK body + MAX_CTX),
  `user/vmhome.c` (nudge removed, note kept; SLOW step timers stay),
  `tests/io1.c`, `tests/pf5.c`, `tests/suite.sh`, this file.
- Kernel: .229 at **#160** (patch 0078; ~/build-160.sh; ~/kernel-159-backup
  holds the previous vmlinuz). .30 stays at #56 (dest-only; 0078 is a
  source-side take-path fix — port only if .30 ever serves takes).

## NEXT

1. Finish the gates: loopback suite ×1, then a chain (suite + hx2/pg3/ws1/
   sig1 48×8 pools — vmremote's page-channel framing changed, so the pools
   are the gate), then the cross-machine suite (rebuild vmremote on .30 —
   remember VMCTX_SHARED=1 skips the rebuild).
2. pf5 cross-box is the fpe-flake hunter proper: run it with dmesg
   streaming on .30.
3. The SLOW CTXPAGE busy-wait above.
4. The clear-tid redundancy (HANDOFF-session40 NEXT 2) still stands.
5. Commit: squash onto 8e3ff2f per the convention (author AND committer
   "Bi Wu <biwu85@gmail.com>").

# Session 41b — the clear-tid redundancy, the sharing census, and browser status

Directive: *"continue with the clear-tid redundancy. Ensure tests are green
and give me status on the browsers. Also I feel netsurf sandbox issue is
more architectural than specific cases. If some task uses ANY part of the
MM of a vmctx context, it needs to be handled correctly for that memory
sharing. You may be missing some code to capture that sharing/mapping and
unmapping operations."*

## Clear-tid redundancy: FIXED (kernel #161, patch 0079)

The kernel's in-band CLONE_CHILD_CLEARTID clear on a dying SERVICE thread
was fully redundant: vmhome's teardown pokes the zero (sibling fallback),
re-arms the wake on a live member, verifies — and only THEN issues the
assisted exit. The in-band put_user afterwards re-took the descriptor page
from the joiner that had just read it (one needless bounce per thread
exit) and, when the fault service was itself mid-teardown, sat the whole
6 s deadline inside the dying thread (vmctx_cleartid_failed 128/session;
fault_deadline_hits ~1 per chain, always this). Suppressed for the
service role, runtime-flippable (`vmctx_cleartid_inband_on`); the pointer
is still NULLed so mm_release() cannot write outside the protocol; the
destination role was already suppressed. Gates on #161 (chain s41i):
suite 36/0, hx2/pg3/ws1/sig1 all 48/48, svc_deferred=557 exercised,
cleartid_failed=0, **fault_deadline_hits=0**.

Found on the way: the teardown verify printed "NOT CLEARED ... waits for
ever" for clears that WORKED but whose page had already left with the
woken joiner (79 such lines per ws1 pool even before the change, pk=4
wk=0 word=0xdeadbeef). vmhome now grades that state "cleared,
unverifiable here" (n_ctid_unverified); the loud line is reserved for a
READABLE word still holding the old tid.

## The sharing census (kernels #162/#163, patches 0080/0081)

The owner's rule, instrumented (numbers first, behavior unchanged):

- The fault redirect keys on current->vmctx, so a FOREIGN task faulting a
  context's mm falls to AF_NOMON and the local kernel INVENTS a page into
  the context's address space. That same fallback is load-bearing for the
  monitor's own POKE supplies — so the census splits vmctx_f_foreign by
  WHO: `vmctx_f_supplier` (the monitor's thread group, recorded in the mm
  registry at note/ATTACH time) vs `vmctx_f_third` (everything else),
  each third party named ratelimited in dmesg.
- The half no mm-keyed hook can see: a VM_SHARED mapping a context maps,
  touched through a foreign task's OWN mm (X SHM segments, memfds handed
  to sandbox helpers). Shared mappings are registered as a context faults
  them; a foreign mm's fault on the same mapping counts
  `vmctx_f_sharedobj_foreign` and names toucher + file. (The browser
  harness runs Xvfb with `-extension MIT-SHM`, i.e. it has been
  SIDESTEPPING this class; a links2 run with SHM enabled rendered
  identically with sharedobj=0 — links2's X driver does not use XShm.)

**First measurements (one links2 run + one netsurf run, loopback):**
- `pgrep` (the harness's own watch loops!) read-faults contexts' cmdline
  (stack-top) pages through /proc — ~2 faults per invocation, hundreds
  per run — and every one lands an INVENTED zero page in the context's
  mm. Any `ps`/`pgrep` on the box poisons context arg pages today. This
  is the user's predicted class, measured live.
- vmremote-local showed as "third party" until #163: guest mms are noted
  before ATTACH binds their monitor, so mon_tgid was 0 (patch 0081
  records it at ATTACH, first-binder-wins).

**NEXT (the real fix, after these numbers):** redirect-or-refuse foreign
faults on noted mms — supplier exempt, third parties either served
through the protocol (publish on a context of that mm, wait on pg_wq like
a sibling fault) or refused (SIGBUS/EFAULT: honest short reads beat
invented pages). And for shared objects: the write-protect fan-out via
the file's rmap reaches foreign vmas, but do_wp_page is still unhooked
for them — that is the remaining architectural door for X-SHM class
sharing.

## Browser status (kernel #162, build bb6b+)

- **links2 -g: WORKS, loopback and cross-machine.** Window 1230x949,
  +253 distinct colours over the empty root (exactly the historical
  rendered-page value), zero unrecoverable faults, both transports; the
  MIT-SHM-enabled variant identical.
- **netsurf: still the glycin sandbox startup stall** (session 37's
  diagnosis stands): creates its 10x10 stub window, never paints (+0
  colours), no faults, no third-party/shared-object census hits from its
  sandbox helpers — the hang is pre-paint, inside the bwrap child after
  pivot_root+seccomp, i.e. the namespace-virtualization axis, NOT an
  mm-sharing miss. The census now stands guard on the sharing axis for
  the day the sandbox gets further.

## Deployment

.229 kernel **#163** (build-16{1,2,3}.sh; kernel-1{59,60,61}-backup hold
prior vmlinuz). .30 unchanged at #56 (all three kernels are source-side
concerns). Patches 0079-0081 in kernel-patches/ and committed in the
scratch tree.

## Session 41c — the refusal lands (kernel #164, patch 0082)

A third party's fault on a MISSING page of a noted mm now answers
VM_FAULT_SIGBUS (EFAULT/short read to the reader) instead of letting the
local kernel invent a zero page into the context's memory. Conservative by
construction: only when the toucher can be ATTRIBUTED — the mm's recorded
monitor thread group is known and the toucher is not it, and never the
mm's own tasks (a service thread past vmctx_task_exit walking its robust
list is the mm's own, untouched). Runtime-flippable (vmctx_third_refuse),
counted (vmctx_third_refused). The shobj census also refreshes a
registration's mon_tgid recorded before ATTACH — 1912 self-touches of
memfd:vmctx-* per suite were drowning the real signal until that.

Gates on #164: suite s41k 108/0 (on #163) and chain s41l suite 36/0,
pg3/ws1/sig1 48/48, hx2 47/48 — the one miss is the standing rare
vmremote early self-death (SIGABRT 0.8 s in, its armed backtrace in
remote.log; NOTES §6.4's rc-139 family), with vmctx_third_refused=0
through the whole chain, so the new path was inert in that run. links2
re-rendered under active refusal (+253 colours).

**The named gap, next in line:** the census shows pgrep (260/boot), and
now Xvfb and gnome-shell too, faulting context mms whose slot carries
mon_tgid 0 — there the refusal correctly declines to act, so those
inventions still happen. The next instrument step is printing WHICH mm
class still registers with mon 0 (guest mms bind at ATTACH per 0081, so
something else does not), closing attribution, and letting the refusal
cover the /proc-reader class fully. After that: serving third parties
through the protocol if a legitimate consumer emerges, and the
do_wp_page hook for foreign shared-object vmas.

## Session 41d — the honest accounting, and the write-side door closed (kernels #165/#166, patch 0083)

**A correction the counters forced.** Session 41b/41c reported pgrep/ps
"inventing zero pages into context mms". WRONG for reads: the census
print fires at handle_mm_fault() entry, BEFORE do_anonymous_page's
existing `vmctx_foreign_refuse` door — which has been refusing every
foreign READ of a missing page all along (158/158 this boot;
vmctx_foreign_refused counts them; vmhome's PEEK already handles the
short reads). The reads were never invented. The 0082 refusal placed in
vmctx_anon_fault was DEAD CODE for the same reason in mirror: that hook
is guarded by current->vmctx, which a foreign task never has. Removed.

**What was genuinely open: foreign WRITES.** They fall through
unconditionally because the monitor's POKE/LAND supplies arrive by
exactly that path — so a third party's write (ptrace POKEDATA,
/proc/<pid>/mem, process_vm_writev) would have the local kernel mint a
page the protocol never placed. Now the registry's monitor attribution
(recorded at note, at ATTACH, and by the FIRST SERVICED CTL — the ctl
gate's own proof of monitorship, patch 0083's other half; not ATTACH
itself, which is gate-exempt) tells the two writers apart, and an
attributed non-monitor WRITE is refused like a read
(vmctx_third_refuse, default on; vmctx_third_refused). Unattributed
writes still fall through, counted and named by the census, whose line
now states exactly which of the three outcomes the fault got.

Attribution verified live on #165: the pgrep/Xvfb journal lines went
from "slot mon_tgid 0" to attributed within one links2 run once the
ctl-proof refresh landed. Gates on #166: chain s41m + links2 (see
chain.s41m.out; recorded by the closing commit).

## Session 41e — the exit-134 abort family decoded and retired

The s41l hx2 47/48 carried its own diagnosis, exactly as the armed
instruments promised. The FATAL-WHO photograph in died.2/remote.log:
every member of the address space answered the GET-take loop with ENOENT
— honestly, the page was in none of them at ask time — and then ctx 8551
showed present=1 installed=1 in the dump that runs AFTER the loop: the
page was IN FLIGHT and LANDED between the last take attempt and the
fatal. "No context would hand it over" was really "it just arrived, ask
again", which the protocol already has a word for. The abort() is now an
INFLIGHT answer (why=PULL, counter n_get_landed_racing, one loud line);
the asker's INFLIGHT loop is time-bounded, so a page that never becomes
takeable still surfaces honestly on the asker's side instead of as this
side's abort. The FATAL stays for the genuinely stuck shape (backing has
the page but the object read fails). Gate: chain s41n suite 36/0 +
hx2/pg3/ws1/sig1 all 48/48 (the race itself is ~1-in-500 pool runs; the
gate is no-regression plus the photograph-driven logic).

Remaining frontiers, documented not started: the glycin/bwrap namespace
axis (netsurf's pre-paint stall), the do_wp_page hook for foreign vmas of
shared objects (the shobj census stands guard), the unattributed-write
window (mon_tgid 0 before the first serviced ctl), and the rc-139
vmremote self-SIGSEGV — a DIFFERENT family from this exit-134 fatal, its
backtrace handler still armed.

# Session 41f — the namespace axis for netsurf: three walls, two down

Directive: continue the namespace axis for netsurf. The campaign reduced
"netsurf doesn't render" to a 43 ms reproducer and moved the wall twice.

## Wall 0, gone before we started: the coherence-class death

Session 37's "GLYCIN_SANDBOX_MODE=disabled dies exit-242 at 3.86 s" no
longer reproduces: netsurf now builds its real 1000x700 window, retitles
to "about:blank - NetSurf", and runs indefinitely — sessions 38–41's
fixes (the kernel record, LEGAL, COWBREAK framing, MAX_CTX) removed that
death. (Note: GLYCIN_SANDBOX_MODE does not exist in glycin 2.1.1 — the
sandbox selector is g_get_prgname()-based in the gdk-pixbuf bridge, no
env override. A fake-bwrap PATH shim, /home/biwu/fakebwrap/bwrap, is the
working sandbox-off lever for experiments. RUST_LOG=glycin=trace makes
glycin narrate its spawn into guest.err — the instrument that named
everything below.)

## Wall 1, FOUND AND FIXED: fork copies read pristine file pages

glycin's probe (`bwrap ... /usr/bin/true`) died SIGSEGV in 43 ms under
vmctx — reproducible with local-here directly. The trail: the bwrap
sandbox child ran `setuid(-1)` (the UNPARSED default of a global that
main() had set!) then #GP'd on garbage. The child's private-FILE pages
(bwrap's .data, parsed options) were served FROM THE FILE by the
"never handed over, so the file is current" branch — the CHILD's record
is fresh, but the PARENT diverged those pages on the guest's machine
before the fork, and the branch never asked. The anonymous fresh branch
has had a fork-parent COWBREAK since tests/cow1; the file-private branch
now has the same (n_cow_file_from_parent counts; a "nothing owed"
refusal still falls to the file serve, which is then genuinely current).
tests/fx1 (fork→exec dynamic, exec'd-child-forks, fork+exec from a
worker thread — every g_spawn/posix_spawn shape) is in the suite; note
those shapes PASSED even before this fix (the corpse needed bwrap's own
early-fork globals), so fx1 is the net, bwrap the reproducer.

## Wall 2, ADVANCED: the sandbox now builds itself, then hits the pidns gap

With wall 1 down the same reproducer went 242@43ms → hang@limit, and the
trail shows the sandbox child doing REAL work in the source service
child's own (forwarded-clone-created) namespaces: bind mounts succeed,
pivot_root returns 0, mountinfo walk, cleanup — the mount/user namespace
side WORKS by construction. The hang photograph (sampler3): the sandbox
child (pid 1 in its new pidns) FORKS the payload; the freshly created
source-side task parks in vmctx_report at clone-exit with nobody ever
adopting it, because the assisted clone — running INSIDE the pidns —
returned the NS-RELATIVE pid ("the source's pid 2", photographed in the
first netsurf run) and vmhome addresses service tasks by that number in
the HOST pid space: every ctl, kill and /proc access lands on the wrong
task (host pid 2 is kthreadd).

**The next work item, precisely:** host-pid translation for assisted
clones. The kernel's fork hook (which already copies vc to the child)
knows the child's host pid; surface it to vmhome — cleanest as a
per-parent "last forked child (host view)" readable by ctl right after
the assisted clone completes, or by translating in the clone reply while
leaving the guest's ns-relative return value untouched (two consumers,
two values). Everything downstream (OP_child, adoption, teardown) then
addresses the right task, and the bwrap payload fork should complete.
After that: seccomp's interaction with the assist path is the next
unknown (the filter now judges every forwarded syscall run in that
service task — including protocol-internal ctx_sys calls).

# Session 41g (IN PROGRESS — pid translation) — investigation notes, no code yet

Directive: do the pid translation fix, get netsurf green, then try firefox.
This session mapped the clone-reply data flow end to end but wrote NO code;
this is the handoff so the fix goes in cleanly next.

## The exact data flow of an assisted clone (the thing to change)

Reply struct is `struct vmr_rsp` in user/vmrproto.h: fields `clone_thread`
(int32), `retval` (int64 = what the guest sees in RAX), `regs_valid`, the
register frame, map fields. ONE pid value today (`retval`), and it does
double duty that the pidns breaks.

vmhome side, user/vmhome.c ~6664, `if (is_clone_nr(rq.nr) && ret > 0)`:
`ret` is the assisted clone's return in the SERVICE task. Uses of it:
  - `rs.clone_thread = (as_of((pid_t)ret) == as_of(conn_ctxpid))`
  - `fork_rel_note((pid_t)ret, conn_ctxpid)` — the fork-COW lineage
  - GETREGS retry loop reads regs of `(pid_t)ret`
  - `rs.retval = ret` (sent as the guest's RAX)
When the clone ran INSIDE a new pid namespace (bwrap after `--unshare-all`
→ CLONE_NEWPID), `ret` is the NS-RELATIVE pid (photographed: "source's
pid 2"). as_of/GETREGS/OP_child/kill/ /proc all then address the wrong
HOST task (host pid 2 = kthreadd), and the payload fork parks unadopted.

vmremote side, user/vmremote.c ~14185: `spawn_forked_context(pid,
(uint64_t)rs.retval, ...)` and `ca->srcpid = (uint64_t)rs.retval` — the
destination names the child by this pid on its own connection. The GUEST
must keep seeing its ns-relative pid (that is what getpid() etc. return
over there); only vmhome's OWN addressing needs the host pid.

## The fix (design, to implement next)

Two values, cleanly separated:
  1. `retval` stays the guest's ns-relative RAX — vmremote and the guest
     use it unchanged.
  2. Add `host_pid` to vmr_rsp (or a vmhome-local lookup): the HOST pid of
     the just-cloned service task, which vmhome uses for as_of, GETREGS,
     fork_rel, OP_child adoption, kill, /proc.
The kernel fork hook already copies vc to the child and KNOWS the child's
host pid (task_pid_nr in the init ns). Cleanest surface: a per-parent
"last assisted-clone child, host view" the monitor reads by ctl right
after the clone completes — OR have vmhome translate ns→host itself via
/proc/<service>/task/<tid>/children + status, but the ctl is race-free.
Note ctx_forward runs the clone in `conn_ctxpid`'s task; that task's OWN
pid is host-namespaced, only the RETURNED child pid is ns-relative, so
the translation is needed only for the return value.

WATCH: after pid translation, seccomp is the next unknown — glycin sets
`--seccomp <fd>` and the filter then judges every forwarded syscall the
service task runs, INCLUDING protocol-internal ctx_sys calls (getpid,
rt_sigaction). May need the assist path exempt or the filter installed
only around guest-driven syscalls.

## State at handoff
- Boxes: .229 kernel #166, userspace 689addf (pushed); .30 #56. All green:
  chain s41o suite 37/0 (fx1 in), hx2/pg3/ws1/sig1 48/48.
- netsurf: NOT yet green — advanced to wall 2 (the pidns pid gap above).
  links2 renders loopback + cross. The 43ms bwrap reproducer is the
  fast iteration loop; fake-bwrap shim + RUST_LOG=glycin=trace are the
  levers.
- firefox: NOT yet attempted this arc (session 12 notes: /usr/bin/firefox
  is the snap shim; real one /home/biwu/firefox/firefox, RUNHOME=/root).
- Nothing uncommitted in the repo; this handoff edit is the only change.
