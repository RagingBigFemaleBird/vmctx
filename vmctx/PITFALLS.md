# vmctx — pitfalls

Every entry here was paid for at least once. The full stories are in
`doc-archive/` (pre-squash commit messages) and `HANDOFF.md`;
this is the checklist form. Read it before measuring, before deploying,
and before "fixing" anything flaky.

## Measurement

- **Instrument before declaring.** Prove an instrument can fire before
  trusting its zero — a confident zero usually means the check never ran.
  Run the positive control (selftests exist in io1, the decline probe,
  ZERO-TAKE) first.
- **Armed instruments change the system.** pg3 passes differently with
  instruments armed; the page log once manufactured the very state it
  reported on the take path. Measure with controls, and A/B with arms
  alternated run-by-run (`perfab.sh`), never against a run recorded
  earlier on a different box state.
- **Never compare old walls.** Pre-session-40 wall clocks carried
  100–200 ms of harness (runner forks, startup polls) inside the timed
  window. Compare per-fault / per-syscall costs from vmremote's own
  accounting, not wall clocks across harness versions.
- **Counting deaths is not instrumenting browsers.** A browser dying
  differently is not evidence about the defect; the harness once
  photographed the window frame instead of the page. Grade rendering by
  distinct-colour delta over the *measured* empty root (767 on this Xvfb,
  not 1; links2's page is +253), and prove the baseline empty first.
- **Suite results can be stale lies.** A suite run as the wrong user
  grades the previous run's outputs — three suites in a row once reported
  105 passes with zero cases executed. runany refuses leftovers now, but
  the rule stands: check `/tmp/tr` ownership, check for D-state leftovers
  (`ps -eo stat,comm | awk '$1 ~ /D/'`) before believing a round, and
  `pkill -x vmremote-local` after NO-OUTPUT rounds.
- **A fixed test port fakes failures.** Phantom "failed" runs came from
  port collisions between concurrent runs; pools space ports by four.
- **Dir-name counts lie; guest.out is the verdict.** torture.sh files
  every hx2/sig1 run as a failure by directory name; grade pools only by
  `^PASS$` in guest.out (chain.sh does).
- **Use the suite's own predicates.** ex4 prints THREAD-OK, not PASS;
  grading by an invented pattern scored passing cases as failures.
- **grep -c prints its 0 AND exits 1.** `x=$(grep -c ... || echo 0)`
  yields "0\n0" and breaks comparisons; the demo graded a green run RED
  on exactly this. And in a `&&` chain, `grep -cE "error|warning"` on a
  CLEAN build log prints 0, exits 1, and silently kills everything after
  it -- it ate a kernel deploy once.
- **Listen ports must sit BELOW the ephemeral range (32768-60999).** A
  harness listen port inside it loses a lottery against every outgoing
  connection's source port; the loser is vmremote's page-service bind,
  minutes later, in whichever case runs when the collision lands
  ("cannot reach the guest's page service ... Connection refused", the
  run parked to timeout). Pool bases live at 31000-; local-here's
  shared-mode auto-pick uses 24000-27999.
- **An ad-hoc sudo loop loses $KO.** Under sudo, $HOME is /root and
  local-here's KO default points nowhere; runany has the SUDO_USER
  fallback but a hand-written loop does not -- five "failures" on a
  fresh-booted box were insmod failing, and an entire kernel A/B was
  VOID because of it. Export KO=/home/biwu/vmctx-7.0.14.ko explicitly,
  and treat any 0-of-N result on a fresh boot as "did the module load"
  before believing it.
- **Never chain the reboot after the build.** `build && echo && reboot &`
  fires the reboot even when the build failed (and once mid-install:
  the box rebooted on the old kernel while make install was half-way).
  Verify "BUILD+INSTALL DONE" in a separate step, then reboot.
- **Relative scp paths + persistent cwd = shipping the wrong file.** The
  Bash tool's working directory persists across calls and drifts; a
  relative `kernel/vmctx.c` once shipped the MODULE source over the
  box's CORE tree (the build then failed on a userspace include).
  Absolute paths for every scp and every file argument.

## Operating the boxes

- **pkill -f kills the caller.** A `-f` pattern that appears in your own
  ssh command line kills your own session (measured at least twice).
  Kill by PID, or look holders up with lsof; kill monitors BY PID, never
  by name — a timeout is a diagnosis, not a cleanup problem.
- **rsync preserves mtimes; make then lies.** A freshly synced source
  looks older than the stale binary beside it. Every build path deletes
  outputs first; SRCID (a content hash compiled into every binary) plus
  `verify-build.sh` is the only proof of what a target runs. A plain
  `make` in the module directory deploys a `.ko` for the wrong kernel
  that loads far enough to look plausible — check vermagic.
- **The suite must run under sudo**, and the module belongs to the
  invoking user's home, not root's (`$HOME` under sudo is /root; the KO
  fallback handles it, but new scripts must too).
- **A vmlinuz-only backup is NOT a recovery image.** ~/kernel-168-backup
  held only the vmlinuz; restoring it over a newer build's initrd left a
  mismatched pair that could not mount root (the initrd carries the
  modules and root-mount logic the vmlinuz expects). A recovery image is
  (vmlinuz + initrd + modules) together, or it is the stock distro
  kernel. The stock Ubuntu kernel (7.0.0-30-generic) is the real
  fallback; select it at GRUB. Back up the initrd beside the vmlinuz or
  do not call it a backup.
- **Do not touch /boot piecemeal.** Let `make install` produce a matching
  vmlinuz+initrd pair; never hand-copy one half. Two boot failures this
  session came from /boot surgery, not from vmctx code.
- **Stop gdm before rebooting the AMD box** (NVIDIA hang), keep the
  previous vmlinuz aside before installing (`~/kernel-N-backup`), and
  never overlap build/install/reboot operations across machines.
- **The netboot kernel has no undo.** Stage a known-good aside before
  touching what 10.0.0.30 boots from; a stray `.known-good` file in
  /boot can hijack GRUB on the AMD box too.
- **ANY stray vmlinuz-\* in /boot hijacks GRUB, not just `.known-good`.**
  Measured a second time in session 43: `/boot/vmlinuz-171-keep` (a panic
  experiment "kept aside" INTO /boot) became the first Advanced menu
  entry and the default boot's kernel. Asides go under ~, never /boot.
- **The kbuild `#N` in `uname -v` is a BUILD COUNTER, not an identity.**
  After the session-43 recovery, `uname -v` says #172 while the code is
  exactly #168 (+ the #171 comment): the rebuild found nothing to
  recompile and did not relink. SRCID/verify-build.sh is the identity;
  the handoffs name kernels by feature-number with this caveat on file.
- **Runs are not exclusive.** A pool beside a measurement is a different
  load and a stress amplifier; an idle-box number and a loaded-box number
  are not comparable. One vmctx run per machine when it matters.
- **/tmp is tmpfs and fills.** Pools write to the disk home, not /tmp.
- **sudo needs askpass** on the AMD box (`SUDO_PASS=... SUDO_ASKPASS=~/askpass.sh sudo -A`);
  ssh is `biwu@` with the repo key only — probing other pairs trips the
  sshd rate limit.
- **Untracked scratch is a graveyard.** The box's ~100 ad-hoc scripts and
  the src/ trees are not in git; the kernel patch set (and now the
  doc-archive) is the only durable record. Anything worth keeping goes in
  the repo.

## Design rules bought with regressions

- **No tie-breaking stamps between two copies.** Three separate
  gate/clock designs (sessions 29–32) each regressed the hx2 oracle and
  were reverted: any clock, stamp, retained-serve or storm that *picks
  between two copies* masks a state the one-writable-copy invariant
  forbids. Fix the producer at the source of the extra copy instead.
  (Sequence stamps that order *events of one mm* are different — but they
  must be assigned by the kernel at the event; a drain-time stamp in
  userspace is a race, and on a tie the construction wins, because the
  kernel's address allocation already ordered the overlap.)
- **No syscall-specific and no address-specific fixes.** Memory problems
  get memory-level fixes; validity questions go to the owner's kernel
  (TRYFAULT), never to a maps parse or a special-cased address. Every
  syscall-number list for mm changes had a hole (mremap, brk-shrink,
  MADV_DONTNEED); the mm hook is the only complete reporter.
- **No optional correctness.** Once an A/B proves a fix, delete the
  switch and keep the numbers; dead switches invalidate every quoted A/B
  after them (fake_share/prot_pull were never read).
- **Refusal beats invention, everywhere.** A declined fault that the
  record says exists somewhere is never zero-filled; shmem holes, foreign
  readers, third-party writers, COWBREAK refusals — every "just serve
  something" path eventually surfaced as silent corruption far from the
  cause. Honest short reads and loud FATALs are cheaper.
- **A page follows either way.** Every GET-shaped protocol answer carries
  its page body even on refusal — one path that didn't cost 4 s per fatal
  fork child (the reader waits for a body that never comes).
- **Replies are memset whole.** A stack-allocated reply with only its old
  fields assigned tells the far side to install stack garbage.
- **-EBUSY means "not yet", never "no".** Take/serve paths retry it;
  mapping it to failure produced invented pages and killed runs.
- **The record is truth only if every transition writes it.** "was given
  before" style records must be written where the action happens (records
  of actions, not inferences); lent[] went stale and had to be replaced by
  asking SEEK_DATA.
- **Recycling context-table rows breaks the dead.** Dead rows are how
  ended address spaces stay answerable ("this AS has ended, stop");
  recycling them turned asks into reconnect storms. Grow the table
  instead.
- **Beware the fix that treats the symptom.** The claim-storm deadline
  extension, the "prompt death nudge" (made pf3 3× slower), the
  teardown retained-page point fix (0/5 chains) — each looked plausible
  and measured inert or worse. The photograph of the failing state, with
  counters before and after, decides.

## Protocol and kernel specifics

- **Namespace pids vs host pids.** A clone's return value is the pid as
  the *parent's namespace* names it; the monitor addresses init-ns pids.
  The kernel records the host pid for the monitor (VMCTX_CTL_LASTCHILD);
  the guest must keep seeing its namespace's numbers.
- **Syscall numbers differ per kernel** (470/471 on 6.18.35, 472/473 on
  7.0.14); a binary built for one calls the wrong one on the other, and
  a ctl op missing on an older kernel answers EINVAL *silently* — degrade
  explicitly, never assume.
- **Assisted syscalls bypass seccomp** (direct x64_sys_call dispatch):
  protocol-internal calls can't be killed by a guest-installed filter,
  and a guest's filter is likewise not enforced — a fidelity gap to
  remember, not a crash to chase.
- **Fresh mappings own no old bytes.** A new SET punches its range in the
  object before going live; a skipped stale vacate must protect the
  mapping but must NOT preserve the previous tenant's pages (a file
  mapping once read the neighbouring thread's 0x01 out of its first
  page).
- **Bounded rings evict what you'll need.** The construction ring lost
  live rows within milliseconds under mmap churn until same-range rows
  replaced in place; count evictions of live state, loudly.
- **The kernel's own instruments close cases userspace can't.** The
  TAKEOBJ re-read detector (0 over ~40M hand-overs) retired a whole
  theory; the ZERO-TAKE census names the maker of every all-zero capture.
  When userspace counters stall, put the question in the kernel where the
  state is.

## Process

- Oracles before fixes; reproducers before diagnoses; one change per
  build; gates (suite + pools, loopback + cross) before any claim of
  green. Exceptions route to the owner raw — a signal is a frame and
  registers, not a guess. And write the handoff before the context is
  gone: the next session starts from it.
