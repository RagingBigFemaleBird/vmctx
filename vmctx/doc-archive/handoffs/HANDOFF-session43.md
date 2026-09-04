# Session 43 — box recovery, the vDSO fix both ways, the breadth campaign doubled, and what it measured

Directive: resume work, get everything sorted out, expand test cases across
every program findable, and add test cases for bugs found. Continues
HANDOFF-session42.md (which ends with the box DOWN at "unable to mount root"
and the vDSO-emulation plan).

## The recovery (first hour)

The box came back reachable on the stock kernel (7.0.0-29-generic — someone
selected it at the console, exactly as the 42d recovery plan said). Then:

- `/boot/vmlinuz-171-keep` was sitting in /boot and had become GRUB's FIRST
  advanced entry — the `.known-good in /boot hijacks GRUB` trap (PITFALLS),
  measured a second time with a different filename. Moved to
  `~/kernels-aside/` before anything else.
- The box's build tree diverged from the local git tree in exactly ONE file:
  `kernel/vmctx.c` carried the #171 documentation comment in
  `vmctx_copy_task` (the fork-wipe map). Functionally identical to #168. The
  comment was ADOPTED into the local tree — the 7.0.14 vmctx commit is
  amended (now `e7f9fbd79`, was 20bf9dc40) and the kernel patch regenerated —
  rather than overwritten on the box.
- `build-168.sh` rebuilt and `make install`ed a MATCHING vmlinuz+initrd pair
  (the whole 42d failure was a vmlinuz-only restore over a foreign initrd).
  The bzImage did not relink (nothing to compile: the tree's last full build
  was already this source), so **`uname -v` says #172 while the code is
  exactly #168+comment**. Do not chase the number: SRCID/verify-build is the
  real identity, and the next real change will relink to #173.
- Backup discipline fixed: `~/kernel-168-full/` holds vmlinuz + initrd +
  System.map + config + a tar of /lib/modules/7.0.14-vmctx — a recovery
  image, not half of one. The stock kernels stay the fallback of record.
- gdm stopped, rebooted into 7.0.14-vmctx, module loaded, verify-build all
  ok, loopback suite **39/0** before any new work landed.

## The vDSO fix, both arms (userspace only, as planned)

The 42d plan, built and measured. A guest's clocks were wrong because the
vDSO reads the vvar page, which as guest memory is a frozen snapshot or an
invented zero; time(2) measured 0, TLS said "certificate not yet valid"
(curl exit 60, the curltls case).

**Arm 1 — same kernel release (loopback): the destination serves its OWN
vvar/vdso.** vmremote:

- `region_parse_layout` now reads the pathname column and marks
  [vdso]/[vvar]/[vvar_vclock] regions (`region.vclass`).
- At startup it records its OWN [vvar]/[vvar_vclock]/[vdso] from
  /proc/self/maps (`own_vvar_find`).
- A read fault inside a vvar-class region is answered with the
  destination's own live page bytes (fault_from_home_inner, before the
  coherence machinery — no other holder can exist). Counted as
  `n_vvar_served`, plogged "SERVED-BY own-vvar".
- Gate: `vvar_emul_decide()` — on when VMCTX_SRC_RELEASE == own
  `uname -r` (local-here.sh exports it), forceable with VMCTX_VVAR_EMUL.
  The guest's rdtsc reads the dest TSC directly (backend never writes
  tsc_offset, verified in 42d), so dest-vvar calibration is exactly right.

**[vdso] TEXT is served the same way** — a discovery, not the plan: the plan
said "keep vdso text from the source", but the source CANNOT serve it — a
guest read of the vdso range answers ABSENT and lands as invented zeros
(measured with a probe: AT_SYSINFO_EHDR image all-zero, magic 00000000, so
a static guest's libc rejects the vDSO and falls back to syscalls; ldd
printed the vdso with no soname — the lddnames diff). Same release = same
kernel image = bit-identical vdso, symbol offsets included, which is what
the first image's loader resolved against. After the change the probe reads
the REAL vdso (magic 7f454c46, page sums equal native, soname at +0x432).

**Arm 2 — differing releases (cross to .30): vmhome hides the vDSO from the
first image.** The 42d fallback, rebuilt properly this time:

- The patch happens AT THE EXEC STOP, before PTRACE_CONT to the program
  entry — ld.so reads auxv as its first act and caches AT_SYSINFO_EHDR, so
  a patch at the program's entry changes a word nobody reads again. The
  stack walk is [argc][argv…][0][envp…][0][auxv]; AT_SYSINFO_EHDR (33)
  becomes AT_IGNORE (26). /proc/pid/auxv keeps the original (the AT_ENTRY
  read is unaffected).
- `oracle_layout` drops [vdso]/[vvar*] lines — FIRST image only. An exec'd
  image gets a fresh vDSO and fresh auxv nothing patches, so its regions
  must stay served (zeros today) or its libc jumps into an undescribed
  range. Exec'd-image clocks on a mismatched destination are a KNOWN GAP;
  the fix there is patching auxv at the exec stop, not a wrong-layout page.
- Gate: `hide_vdso()` — on when VMCTX_DEST_RELEASE != own release;
  runany-x.sh and xrun.sh now ask the destination for `uname -r` and pass
  it. VMCTX_HIDE_VDSO overrides.

**Verified:** vd1 (below) PASSes under emulation (all clocks live and
correct, 0 forwarded clock calls at fault time), PASSes under forced hiding
(clocks by forwarded syscall), and a DYNAMIC binary under hiding prints
true epoch (perl `time()` == native date — the exec-stop patch point is
what makes the dynamic case work). curltls: FAIL rc 60 → **PASS**.

**...and what the first suite gate then caught: a snapshot's coarse clocks
FREEZE.** fc1 failed 3/3 rounds (NO-OUTPUT at the 60 s limit) where the
morning's pre-change gate was 39/0. Its main loop is
`while (time(NULL) - t0 < SECONDS)` — and BEFORE this session a static
guest's zeroed vdso made libc reject the vDSO and fall back to the real
syscall by ACCIDENT, so time advanced. With a REAL vdso, __vdso_time reads
the served vvar copy, whose coarse fields only the KERNEL'S page ever
advances — the guest's copy is frozen at install, `time(NULL) - t0 == 0`
forever, and fc1 spun to the kill. The fine clocks were never affected
(they self-advance via rdtsc). Fix: the owner's own plan step 5 — a
refresher thread re-pokes every installed VVAR-class page (never [vdso]
text) from the destination's live page 3x/s, by plain ctl (home_call is
not thread-safe and is not used); slots drop with their context. The
re-poke is not atomic against a concurrent guest reader and the seq word
lands first, so a coarse read can catch a sub-second wobble at 3/s — the
airtight fix is MAPPING the live page instead of copying it (kernel work,
"map, don't copy", on the ledger). vd1 grew the check the instant-
correctness asserts could not make: time() must ADVANCE within ~2.5 s.

- **tests/vd1.c** (suite, after fc1, NOT xfail): every user-reachable clock
  — time(), gettimeofday, REALTIME, REALTIME_COARSE, MONOTONIC advance
  across a 50 ms sleep, MONOTONIC_COARSE vs fine — judged against
  syscall(SYS_time), the one answer that cannot come from the vDSO.
  Tolerance 60 s (the defect is off by an epoch, and cross-machine the
  vDSO is the dest's wall clock vs the source's syscall).

## The exit-status bug (found by the new pyfork case, FIXED)

A guest that forks a child which exits non-zero ON PURPOSE reported the
CHILD's status as the run's exit code: python os.fork + child `_exit(7)` +
parent waits, prints, exits 0 → guest stdout right, vmhome closing context
0 with status 0x0, and the run still exiting 7. vmremote's "a thread died,
the run has not succeeded" rule (worst_ctx_status) counted EVERY context
ending non-zero. The discriminator was already in hand: `as_id(cpid) ==
cpid` means the context LEADS its address space — a forked PROCESS, whose
status is program behaviour delivered to its parent's wait4 — while a
context that JOINED a space is a thread, whose death is the silent kind the
rule exists for. Fixed in the forked-context service thread; pyfork passes;
th9-style thread-death reporting unchanged. Note the 242 family still
surfaces through the guest's own shell ($? of a died pipeline stage).

## The breadth campaign: 30 → 65 cases

progs.sh grew 35 cases (every program findable on the box that makes a
gradeable shape): sortp1 (the sort bug's boundary control), cpprt (g++ +
std::thread + run the fresh binary), arlib (ar/ranlib static-lib link),
striprun, cpiort, tclsum, factor, dcpi, shufdet (deterministic shuf),
odx, filemg, elftools (readelf/nm/size/strings), edbatch, groffman
(man|col), bashsub (process substitution), xargspar (-P4 forks), findexec,
flock2, fifo1, nc1 (in-guest socket server+client), scriptpty (pty
allocation in a guest), bboxsh (busybox), iconv1, sums (sha512/b2/cksum),
lddnames, getentc, localec, duq, envmisc, killsig (self-signal + handler),
timeout1, pyfork, gdbbt (gdb runs its inferior — ptrace INSIDE a guest),
stracec (strace -cf), and the clock family datetime/sleepdelta/pytime.

First full run (pre-fix binaries): 58/67 pass. Notable PASSES: pty
allocation works (scriptpty), busybox, iconv, flock, fifos, xargs -P,
ELF tools all clean. stracec's first "pass" was FALSE — `head -1` of an
EMPTY st.txt is true; the predicate now requires the summary table, and
the case joins KNOWN_GAPS: its run forwards 15 ptrace calls that
half-work (SEIZE, GETREGSET -> 24 bytes, DETACH all answer against the
parked source-side task) but the stops strace counts by never fire, so
the summary records nothing. Together with gdbbt (below) that ledger
reads: ptrace-inside-a-guest forwards mechanically and means nothing
semantically — the traced task on the source never RUNS, so
stop/continue choreography cannot happen there.

**progs.sh now has KNOWN_GAPS** (suite.sh's rule: a list that always
reports failures stops being read; only surprises should be red):

- `sort` — deep-teardown thread churn, ~2/12, --parallel=1 (sortp1) passes.
- `gpgsym` — agent daemon double-fork parks; dbus-launch's ledger (parked).
- `lddnames` — **the argv-stale exec hole's best reproducer yet (~2/4)**:
  a fork child's stale page tables corrupt an exec's argument page; the
  exec'd ld.so reads garbage argv and exits 127 (0x7f00 in home.log), the
  ldd pipeline then produces empty stdout. The 42c fork-wipe fix for this
  was built twice and reverted twice (kernel #169/#170); the hole is open
  and documented in vmctx_copy_task's comment.
- `nc1` — the orphaned double-fork server dies in the 242 deep-teardown
  family before the client connects; ~5 s, near-deterministic, the
  family's CHEAPEST reproducer (sort needs 45 s and flakes).
- `gdbbt` — gdb inside a guest hangs to timeout. The photograph
  (progs.s43a/gdbbt): gdb forks its inferior, **zero ptrace syscalls are
  ever forwarded** (grep sys_101 = 0 across the whole home.log), the
  inferior runs to COMPLETION untraced (status 0x0, breakpoint never
  installed), and gdb then parks in `SLOW CTXPAGE 0x7ffff7c0e000 ...
  -> -11` retries every ~1 s forever — the stuck-CLAIM/TRANSIT
  protocol-lifecycle family, netsurf's pre-paint stall shape (42b
  finding 1). Two separate facts: where did the child's
  PTRACE_TRACEME go, and what wedged the page.

The flaky 242 family (gitwork/pipes/psproc some runs) is left STRICT on
purpose — it passes most runs, and its failures keep pointing at the
standing deep-teardown class rather than at anything new.

## FINAL GATES (s43j, after the 43b fork-TLS fix)

- Loopback suite **120/0** (40 × 3). Cross suite **40/0**.
- progs.s43j: **60 pass, 2 fail, 4 known-gap fails, 2 known-gap passes**
  — the best run on record (was 56/6 before the fix). Both strict fails
  are the roaming argv-stale exec hole (see the taxonomy above); every
  242-family strict failure is GONE.

## State at handoff — GATES (first push, pre-43b)

- **Loopback suite (s43i): 120 passed, 0 failed** — 40 cases (vd1 in,
  non-xfail) × 3 rounds, fc1 3/3 after the refresher.
- **Cross suite .229→.30 (s43x): 40 passed, 0 failed** — vd1 passes
  cross via the HIDING arm (runany-x passes VMCTX_DEST_RELEASE; 6.18.35
  ≠ 7.0.14 → vmhome AT_IGNOREs the first image's auxv).
- **progs.s43i: 56 pass, 6 fail, 5 known-gap fails, 1 known-gap pass**
  (sqlt joined — sqlite3 now installed). All 6 strict fails are the ONE
  standing deep-teardown/242 family (pipes, basejq, tclsum, dcpi died
  0xf200; groffman/xargspar lost a pipe stage — statuses 0x100/0x300
  downstream). Family samples across the day's three full runs: 3 → 3 →
  6 fails; whether the vvar REFRESHER's unprompted 3x/s pokes amplify
  the family during teardown was A/B'd at session end (VMCTX_VVAR_EMUL
  1 vs 0 over pipes/basejq/tclsum/dcpi ×6) — result in the last section.
- Kernel #168 (uname says #172, see above) on .229; module + userspace
  deployed and verified (SRCID f1408c684c64); .30 has the same-SRCID
  vmremote at /usr/local/bin/vmremote (.pre-s43 aside kept).

## Session 43b (the "continue"): a 242-family producer NAMED and FIXED —
## the fork child's TLS zeros

The new cheap reproducer paid off the same day. `dash -c 'echo ... | dc'`
(the dcpi case) died 242 in 1-2 runs when driven directly; the corpse was
exact every time: the fork child dies at rip 0x7ffff7cf73be —
__libc_fork's child bookkeeping — writing address 0x2d8, which is
`self->robust_head` off a NULL THREAD_SELF: **the child's TLS page
(0x7ffff7fa0000, its fs_base page) read as zeros.**

The zeros had two producers, both now dead:

1. **vmhome's zero-land for a fork copy** (first fix): the source-side
   child task faults its TLS at the clone tail (CHILD_SETTID), vmhome
   asks the destination — ABSENT (no context of the child's AS has it),
   COWBREAK refused ("parent readable 0": the parent's current page is
   not on that machine either, it had been TAKEN here for dash's parked
   wait4) — and the fresh-anon fallthrough landed literal zeros in the
   child's mm, which the guest's GET then read as its TLS.
2. **"Let the kernel's own fork COW serve it" — built as fix 1 and
   REFUTED by the next corpse**: a page the DESTINATION held at clone
   time is a HOLE in the parent's source-side mm, the child's mm copied
   that hole, and the kernel's COW of a hole is the ZERO PAGE. (This is
   precisely why the #169/#170 "fork starts empty" wipe was pursued, and
   the serve-chain reason it failed.)

**The fix that holds:** the inheritance is the PARENT's page, wherever it
lives — vmhome reads it THROUGH THE PARENT'S TASK (`ctx_page_inner` on
the parent pid: if the page was taken here, the read returns it; if it is
on the other machine, the read faults it home through the standard
capture, the same pull any assisted syscall touching parent memory makes)
and lands those bytes in the child (`n_cow_self_cow`, cow_note_break).
Zeros remain only for a parent that truly has nothing anywhere. dcpi:
fails-in-1-2 → **24/24**, the fix line firing on exactly the photographed
page.

**Ledger addition (not fixed, now named):** the fork-time snapshot can be
DESTROYED — once the parent's page is taken to the source, source-side
mutation happens in place, invisibly to the destination's give-first
machinery, so "the parent's bytes as of the fork" can stop existing on
either machine. The fix above serves parent-CURRENT bytes (self-
consistent, and identical while undiverged); true fork-snapshot semantics
for taken pages is REDESIGN territory.

**The 242 family's taxonomy, complete after the fix** (6 rounds of the
six flakiest cases, post-fix): tclsum 6/6, dcpi 6/6, flock2 6/6 — the
TLS-zeros producer was their whole failure. What remains splits into
exactly two named, pre-existing holes:

- **exit 127 — the argv-stale exec hole** (basejq 2/6, pipes 1/6, and
  lddnames as always): an exec'd stage reads a corrupted argument page
  and dies "command not found". Kernel-blocked (the #169/#170 wipe
  story); unchanged by today's work. It ROAMS: in the final gate it
  appeared as shufdet/odx "stdout differs" — the hash-a-stream cases
  sha256'd an EMPTY stream (e3b0c442..., the empty hash) because the
  producer stage never exec'd; the tell is always one 0x7f00 exit in
  home.log. Pipeline cases stay strict; the 0x7f00 is how to file them.
- **exit 251 — the orphan's service dies with its parent's monitor**
  (nc1 6/6, and the mechanism under gpg-agent and the PARKED
  dbus-launch wall): a forked guest's destination task is created by
  `clone(SIGCHLD)` FROM ITS PARENT CONTEXT'S MONITOR PROCESS and is
  serviced by a thread inside it. When the parent guest exits — the
  double-fork daemon pattern does this immediately and on purpose — the
  parent's monitor teardown takes the still-live child's service with
  it: vmremote's wait sees ESRCH-then-"task gone" for a task the source
  still shows alive=1 state=R, fabricates exit 251, and the daemon is
  dead before its first syscall. nc1 photographs it in five seconds.
  The fix is structural (a forked context's dest task and service must
  survive its parent's teardown — its own process, or a handoff at
  parent exit) and sits inside the wall the owner PARKED in 42b, so it
  is RECORDED here and not built.

## Session 43c: JIT runtimes join the breadth harness (70 cases)

nodejs 22 (V8), ruby 3.3 (--yjit) and lua5.4 installed; four new cases —
nodejit (a hot BigInt loop V8 tiers up, + crypto), nodework (4 V8 worker
threads over eval'd code), rubyjit (YJIT hot loop), luasum. **All four
PASS on the first run.** This is the first shape in the harness where
the program WRITES the code it executes (mmap RW → write → mprotect RX →
jump): runtime code generation works end-to-end through the serve chain,
protections included.

Under load the node cases flake ~1/6–1/12 into the STANDING deep-teardown
thread-list class (session 37, REDESIGN step 3 — not new): the corpse
shows a page "LOST: lent to shadow N (lent whole, its channel is gone)"
zero-filled under a live V8 worker stack, one context dying SIGABRT, at
thread churn. V8's rapid worker create/join is likely that class's best
modern reproducer (sort needs 45 s and a 200k-line file; `progs.sh
nodework` needs seconds). Full-campaign progs.s43l: 63/3 — basejq
(roaming argv-stale), nodejit (this class), pyfork one-off timeout
(6/6 in isolation).

## Session 43d: the fork-wipe RE-MEASURED (#173) — refuted again, blocker named

43b's pull-through-parent closed one of the two holes that killed the
#169/#170 wipe, so the anon-only wipe was rebuilt as kernel #173 (zap
each private-anon VMA in vmctx_copy_task, SERVICE contexts only — a
dest-side fork child's mm is its monitor's own address space and must
never be wiped) and re-measured with today's serve chain:

- Basic forks and light pipelines: GREEN (dash pipelines, basejq,
  tclsum, dcpi, shufdet, odx all passed — the argv-stale shapes did not
  appear in the round).
- **gcc rc 1 (empty stderr), make rc 2, pipes and lddnames wedged to
  timeout — and the corpse is the fork-TLS NULL crash AGAIN** (ctx died
  0xf200 with COWBREAK refused for a TLS page, LOST-PAGE 0x0), WITHOUT
  the 43b line ever firing.

The named blocker: **the serve is RECURSIVE under the wipe.** A wiped
child's fault needs the parent's page; the parent's page was wiped at
ITS fork; reading it through the parent's task (43b) GUP-faults the
parent, whose service needs the grandparent's page — up the chain to
ctx0. vmhome's single-level parent read cannot satisfy that nested
chain, so the fallthrough is still zeros. (The destination's OWN chain
walk — cow_give_from_chain — solves exactly this shape one level at a
time on the other side; the source has no equivalent.) The wipe is
reverted (the running kernel is #168 code again, build counter #174;
the vmctx_copy_task comment carries this result), and the argv-stale
hole remains open with its price now exact: either the source learns to
serve a nested parent-chain fault, or the record walks the fork chain
in one step the way the destination already does.

## Session 43e: wipe iteration 3 (#175) — the LAST missing piece is
## destination-side, and two chain walks stay as keepers

The #173 refutation designed its own next step, so it was taken: vmhome
grew the SOURCE-side chain walk in two places — the fork-copy branch of
fault service (walk ancestors by pagemap, land the nearest present
page; the old faulting single-level read demoted to a last fallback,
which can no longer recurse because the level below it is answered by
the walk), and `ctx_page`'s chain serve (on ABSENT + nocow — i.e. after
the DESTINATION's own chain walk already refused — copy the nearest
present ancestor's page raw via /proc/anc/mem, present-gated, no take,
gen 0). The wipe was rebuilt as #175 on top.

Verdict: light shapes green, gcc/make/pipes still down — and NEITHER
walk fired, because the failing page (`0x7ffff7fa7000`, the gcc
grandchild's TLS-arena page) exists ONLY in the FIRST context's guest
on the destination. No source ancestor holds it, and the destination's
`cow_give_from_chain` refuses it: the walk's gate wants
`cow_is_protected(ancestor)` — the fork-protect pass records the OWED
mark only where the parent's machine held the page AT FORK TIME, and
the INTERMEDIATE ancestor (gcc's first child) never held this page, so
the chain to ctx0 is invisible to the gate ("fork-protected 0").

**The wipe campaign's ledger is therefore complete: three serve-chain
requirements, two now built, one remaining** — the destination's
chain-give must vouch for a grand-ancestor's page through
intermediates that never held it (protect-mark anatomy / spent-state
surgery in vmremote's COWBREAK walk + the protect pass). That is a
bounded, single-machine, USERSPACE change — the next session's natural
opener, with `progs.sh w-list gcc make pipes` as its five-minute gate
on a #175-style build. The kernel is reverted again (#176 == the
proven code; vmctx_copy_task's comment now carries all three
iterations); both userspace walks STAY — each is a strictly better
fallback than the zero it replaces, wipe or no wipe.

## Session 43f: wipe iteration 4 (#177) — the serve chain COMPLETED, and
## every remaining issue collapsed into ONE frontier

The #175 blocker got its fix: the ESCALATED COW break. A COWBREAK whose
rq.len == 1 is the source saying "my own routes all failed" (the child's
kernel COW, the pagemap-walked ancestor chain — the only fact that
licences relaxing ex4's strict gate), and the destination then serves the
nearest ancestor page it can READ, protect mark or not
(`cow_give_page_mode(relaxed)`, `n_cow_relaxed_given`). Wired in all
three places the refusal could reach: the PG-channel COWBREAK handler
(rq.len==1), the dest's own chunk/pull fault paths (relaxed retry after
the nocow re-ask proves the source empty-handed), and vmhome's
fresh-anon branch (pg_req_aux escalation after both walks fail).

**Under the wipe (#177) the toolchain then ran GREEN — gcc, runfresh,
make, gitwork, basejq, dcpi, tclsum — which is the proof that the serve
chain is now complete for it: every inherited byte finds its way.** Two
more real fixes fell out of the same corpses, both kept:

- pg_connect fail-fast: a dead peer's port is declared gone after 20
  consecutive refusals (a finished run used to be held 85 s by 471
  dials of a dead port until the watchdog called it rc 124);
- the ACT_KILL escape: a context that ignores three KILL answers gets
  SIGKILL and its service closed (the same fault re-arrived from WAIT
  for ever against a dead channel).

What iteration 4 finally measured is that the wipe's remaining cost is
NOT bytes: the multi-child pipeline shapes (pipes, xargspar, flock2,
lddnames) wedge in the TEARDOWN ORDER — vmremote's monitor leaves while
the source still has forwarded calls in flight for live contexts (dash
parked in wait4, its stack page stranded THEIRS, its service then
unservable). That is the standing lifecycle frontier — the netsurf
CLAIM stall (42b), the session-37 join residual, the daemonize wall
(owner-PARKED), and now the wipe's last blocker are ONE protocol hole
wearing four coats: **nothing defines what happens to in-flight state
when a context's counterpart leaves.** The wipe is reverted (5th build,
#178 == the proven code; the vmctx_copy_task comment carries the whole
four-iteration map); the escalated-break machinery, fail-fast and
kill-escape all STAY, correct with or without it.

**Convergence statement: every remaining strict issue on this project's
ledger now reduces to that one lifecycle redesign** (plus the ptrace
semantics question, which is a design decision for the owner, and the
cross-only exec'd-image clock gap). The next real step is REDESIGN.md
work with the owner, as sessions 29–35 did for ownership: define
connection-death and monitor-exit semantics for in-flight pages, claims,
and forwarded calls. The reproducers are all cheap now: nc1 (5 s),
`progs.sh pipes` under a wipe build (deterministic), nodework (~1/6).

## Session 44 (2026-08-25, design authority granted): THE LIFECYCLE
## DESIGN — three classes dead in one change

The converged frontier ("nothing defines in-flight state when a
counterpart leaves") got its design, in four pieces:

1. **The tree is the service lifetime** (vmremote): after ctx0 is
   reaped, main waits for every forked context's service to drain,
   bounded by LIVENESS, not a clock (the old two-seconds-then-abandon
   printed its own confession); a leaked count with no living child
   task — the case the old bound existed for — is detected by audit
   (three scans, no live child) and only then abandons.
2. **Monitor succession** (kernel #179 + vmremote): a DEAD monitor is
   the same case as no monitor yet — vmctx_report drops it and
   publishes the event to wait for a successor under the fault
   deadline, instead of returning ACT_SELF (which for a redirected
   context is an unrecoverable -14 on first touch). And each forked
   context's monitor is HANDED from the spawning thread (the parent's
   service, gone an instant later when a subshell exits) to the child's
   own service thread: the spawner DETACHes after the release, the
   child's thread ATTACHes — so a guest ENTERS exactly when its own
   service is watching, and can never outrun or outlive its watcher.
3. **Claims and transits die with their wire** (vmhome): a service
   that breaks out mid-fault settles the PG_CLAIM it wrote; every
   single-page transit a connection commits is tracked per-connection
   (acks arrive on the connection that served) and any still IN
   TRANSIT at its close is settled THEIRS — a claim must not outlive
   its wire (the netsurf 2001-fault stall's mechanism).

**Measured, kernel #179 (uname #179), all on the proven no-wipe code:**

- **nc1 9/9 GREEN** — first passes ever; the orphaned server survives
  its parent (the daemonize wall's core mechanism, dead).
- **sort 9/9 GREEN** — historic rate 2/12; the dying-context EIO
  family's reproducer, dead.
- **lddnames 6/6 GREEN** — the surprise that rewrites a diagnosis: the
  "argv-stale exec hole" was the CLONE-WINDOW race all along (a child's
  early pages served while its monitor churned), and the
  guest-waits-for-its-own-service ordering closes it with NO wipe. Four
  kernel wipe builds chased a hole whose real fix was one ordering.
- pipes/dcpi/tclsum/basejq stable green; nodework ~2/3 (the session-37
  thread-list class persists — its LOST is a lent-page record, not a
  monitor lifecycle).
- **gpgsym remains**, shape now measured: not a death but a 267k-fault
  storm to the 90 s limit — the stuck-CLAIM class on a LIVE connection,
  which the dead-connection sweep by design does not touch. One coat
  of the old wall left.

## Session 44b: the GHOST CLAIM healed (kernel #180) — the storm class dies

gpg's storm gave the stuck-CLAIM class its deterministic reproducer, and
the armed page log gave the verdict in one run: 169,826 serves answering
`CLAIMING` with `record-was=OURS` — the serve table's HOME-and-absent
branch, which imagined "a local fill is landing" with NO bound, while
the fill it imagined was the guest fault its own refusals were starving.
It was the LAST eternal answer in the table (TRANSIT already heals by
age; CLAIM settles with its service since 44). The heal counts the
refusals in the entry's sum field — dead weight for a page with no bytes
to sum, written through the same outcome CAS every serve takes, because
a clock cannot work where every refusal restamps — and at 255
consecutive absent-asks declares the claim a ghost: REMOTE, ABSENT, the
asker fills and holds the page exactly as the record then says
(`vmctx_pgrec_home_healed`).

Measured: the storm is DEAD — 23 heals in one gpg run, faults 267k → 4.6k,
zero CLAIMING lines. gpgsym itself still times out, but the corpse is a
NEW, quieter shape: the agent is up, its socket exists, gpg connected —
and the agent ticks clock_nanosleep in a loop for 88 of 90 seconds while
gpg waits on the socket. That is application-level protocol progress
(gpg↔agent, npth's timed event loop), not a page state; it gets its own
fresh investigation. netsurf's pre-paint stall was this same storm
photograph and is expected healed (measured in the s45 gates).

## Session 44c: performance re-measured on kernel #180 (everything in),
## the gpgsym verdict, and the loan-vacate fix

**Performance (perf.sh s45p loopback + runany-x cross, per-op costs from
vmremote's own accounting; raw logs /tmp/perf.s45p.log on the box):**

| layer            | compute (vmbench)         | syscall     | page fault  |
|------------------|---------------------------|-------------|-------------|
| native .229      | 236.5 Miter/s             | 0.15 us     | --          |
| loopback guest   | 235.7 Miter/s (99.7%)     | 66-80 us    | 38-91 us    |
| cross .229->.30  | 221.4 (98.5% of .30's own)| **220 us**  | ~390 us     |

The cross syscall round trip is 220 us against 259 in s42p and 482 in
the #60 era — the lifecycle work cost nothing and the serve-path arc
keeps paying. Real-program walls, median native/loopback/cross:
wc 0.010/0.047/0.24; gzip 0.43/1.42/4.9; links2 -dump 0.028/0.109/--;
sha256 0.010/0.118/--; python-sum 0.032/0.243/--. Same shape as always:
compute converges to native; short-lived tools pay pages x fault-cost at
startup.

**gpgsym's final diagnosis — a TIME-DOMAIN gap, recorded not built:**
the agent is up, its socket exists, gpg's request lands (write -> 17)
and gpg parks on the reply; the agent's ticker thread reads
CLOCK_THREAD_CPUTIME_ID (clockid 3 in the log) about 9/s and then
EXITS mid-run, and with it npth's timed scheduling stops — the
remaining agent threads never run their turn, and the reply never
comes. Forwarded cputime clocks report the PARKED SOURCE TASK's
consumption (~0), not the guest's real burn on the destination: any
program that schedules by its own cpu-time reads nonsense. The fix is
cputime virtualization (account guest execution into the service
task's clocks, or intercept cputime clockids with guest-side numbers)
— kernel/monitor accounting work, designed here, not built tonight.

**The loan-vacate fix (built, then corrected by its own A/B):**
`range_vacated_1` clears lent[] loans in the vacated range
(`lend_forget_range`, n_lent_vacated, whole-slot memset — a
half-cleared slot hands its phantom `watched` to the next tenant) —
the documented "lent[] is stale" trap's repair, and the session-37
thread-list corpse's mechanism: a recycled V8 worker stack consulted
the OLD tenant's loan, dialed a dead shadow, and zero-filled the LIVE
stack. The first cut cleared on SOFT vacates too and fx1 refuted it at
~1/6 (0/10 without): a DONTNEED kills CONTENT, not RELATIONSHIPS — the
machine the loan names still holds its copy, and clearing the record
under it grants the next write against a holder that was never told.
**Content dies soft; relationships die hard.** fx1 12/12 with the
distinction; the LOST line is gone from every thread-churn corpse
(nodework's residual failures wear the shared-object-serve and
coh_lock signatures — the true REDESIGN-step-3 class).

## Open frontiers (unchanged ledger, plus the new reproducers)

1. Deep-teardown/242 family — REDESIGN-adjacent; now with nc1 (~5 s) as
   the cheapest reproducer and lddnames for the argv-stale sub-hole.
   The vvar REFRESHER is exonerated as an amplifier: A/B over the four
   flakiest cases (pipes/basejq/tclsum/dcpi, 6 iterations each arm) —
   VMCTX_VVAR_EMUL=1: 4 failures in 24 runs; =0: 9 in 24. The s43i
   spike (6 strict fails) was the family's ordinary variance under
   post-suite load, not the new pokes.
2. dbus-launch double-fork wall — PARKED by the owner (42b).
3. Exec'd-image clocks on a mismatched destination — known gap, fix is
   the exec-stop auxv patch applied to guest execs.
4. gdbbt — ptrace inside a guest, unexplored.
5. netsurf pre-paint CLAIMING stall — protocol-lifecycle (42b finding 1).
6. vvar liveness: DONE this session (the 3x/s refresher — fc1 forced it;
   see the vDSO section). The remaining sliver is the non-atomic re-poke
   vs a concurrent coarse reader (sub-second wobble, 3/s window); the
   airtight fix is the kernel mapping the destination's LIVE vdso-data
   page into the guest ("map, don't copy") instead of copying it.
