# RUNNING — the operator's guide

How to run a program under vmctx, on one machine or two; how to run the
test suites, the breadth campaign, the performance harness, and the
browser demo; how to read a run's artifacts; and how to rebuild and
redeploy each piece. ARCHITECTURE.md says what the system is; AUDIT.md
says how each piece works; this file says which command to type.

## 0. The lab

| machine | role | CPU | kernel | access |
|---|---|---|---|---|
| home (this checkout) | driver, builds | — (WSL2, no vmctx) | stock | you are here |
| 10.0.0.229 "the AMD box" | SOURCE and loopback lab | AMD (SVM) | 7.0.14-vmctx (own build) | `ssh -i config/id_vmctx biwu@10.0.0.229`; sudo needs `SUDO_PASS=biwu SUDO_ASKPASS=$HOME/askpass.sh sudo -A` |
| 10.0.0.30 "the Intel box" | DESTINATION (netboot, diskless) | Intel (VMX) | 6.18.35-0-lts | `ssh -i config/id_vmctx root@10.0.0.30` (no sftp — pipe over ssh) |

Terms: the **source** owns the program (files, pids, syscalls, memory of
record); the **destination** executes its instructions and owns nothing.
A **loopback** run puts both halves on one machine over 127.0.0.1; a
**cross** run puts vmhome on .229 and vmremote on .30.

Syscall numbers differ per kernel: `vmctx_run`/`vmctx_ctl` are 472/473
on 7.0.14 and 470/471 on 6.18.35. Each machine's binaries are built for
its own numbers; `verify-build.sh` checks the deployment end to end and
prints per-binary SRCIDs. **Trust nothing until it says
"everything deployed matches this tree".**

## 1. One program, loopback (the everyday run)

On the AMD box, as root, from `~/lan-boot/vmctx`:

```bash
sudo ./local-here.sh /usr/bin/wc /etc/hostname
```

What it does: rebuilds vmremote-local and vmhome from source (content-
hashed SRCID — mtimes lie under rsync), loads the module if needed,
sweeps leftovers from crashed runs (by /proc/*/comm, never pkill — pkill
takes the target's mmap lock and joins a stuck context in D-state),
starts vmhome on the port, starts vmremote against it, and reports
`=== exit N in X.XXXs ===` with vmremote's own time breakdown.

Outputs land in `$OUT` (default `/tmp/localrun`):

- `guest.out` — the program's stdout (the thing you came for);
- `guest.err` — the program's stderr (split from vmhome's by the
  `[vmhome]` prefix rule);
- `home.log` — vmhome: every forwarded syscall (`[vmhome <ctx>] name
  (args) -> ret`), fault-service decisions, the end-of-run accounting;
- `remote.log` — vmremote: faults served, pages moved, the counters.
  This is stderr of the far half; when a run dies, read it first.

Useful variables:

- `OUT=/tmp/mydir` — keep several runs' artifacts apart.
- `VMR_LIMIT=90` — the watchdog (seconds). The run is killed and swept
  (process-group kill; a guest's forks otherwise survive as load) and
  scored rc 124. Nothing of the harness is inside the measured wall.
- `VMCTX_SHARED=1 PORT=20000` — concurrent runs. Shared mode skips the
  global sweep and the rebuild, so the caller MUST build once first and
  give every run its own port pair (PORT and PORT+1). An unset PORT in
  shared mode picks a fresh free pair automatically, below the ephemeral
  range — a listen port above 32768 loses to outgoing source ports.
- `KO=` — the module path (default `$HOME/vmctx-7.0.14.ko`).

Programs are named by ABSOLUTE PATH (vmhome execv()s with no PATH
search). Arguments pass through verbatim.

## 2. One program, cross-machine

From this checkout (home):

```bash
./vmctx/xrun.sh mytag /usr/bin/wc /etc/services
```

runs vmhome on .229 and vmremote on .30, collects
`/tmp/xr/mytag/{guest.out,home.log,remote.log}` here. From the AMD box
itself, `./tests/runany-x.sh <tag> <prog> [args]` does the same with the
suite's artifact contract (`/tmp/tr/<tag>/guest.out`). Both feed the
destination's kernel release to vmhome (`VMCTX_DEST_RELEASE`) so the
vDSO decision (AUDIT §6) is made from fact.

Time budgets: the suite's 60 s is a loopback number; a LAN round trip is
~4.7× dearer, so runany-x multiplies `VMR_LIMIT` by 5 (`VMR_XLIMIT`
overrides outright).

## 3. The regression suite

On the AMD box:

```bash
sudo ./tests/suite.sh          # 3 rounds, serial
sudo ./tests/suite.sh 1        # one round (the quick gate)
sudo ./tests/suite.sh 3 -j 8   # a pool -- "does anything fail", not a rate
```

40 C tests in `tests/`, each a measured defect's reproducer, each graded
by its own predicate over `guest.out` — plus xfail bookkeeping: the two
surprises are a known gap that starts passing and anything else failing
at all. Failed cases keep their artifacts in `/tmp/tr/fail.<name>/`.
Serial by default on purpose: the page tests are races, and a pool is a
different machine load (compare rates only against same-load runs).

Cross-machine, the same suite unchanged:

```bash
RUNANY=./tests/runany-x.sh sudo ./tests/suite.sh 1
```

Run it over ssh with `setsid ... </dev/null` (an ssh-allocated tty dying
mid-run kills the round) and never overlap it with pools or perf runs.

## 4. The breadth campaign (real programs)

```bash
sudo ./tests/progs.sh mytag            # all ~70 cases
sudo ./tests/progs.sh mytag gcc make   # just these
```

Every case runs the program NATIVE and as a GUEST and grades output
equality, roundtrip bit-exactness, or rc — compilers, interpreters, JIT
runtimes (V8, YJIT, Lua), archives, git, TLS, crypto, ImageMagick,
sqlite, ptys, fifos, sockets, signal handling, the clock family.
Artifacts: `/home/biwu/progs.<tag>/<case>/` with `n.out`/`g.out`/
`launch.log` and the guest run dir under `gst/run/`. KNOWN_GAPS at the
top of the script names the standing families (currently gpgsym's
cputime wait and the two ptrace-semantics cases); a red line outside it
is always a surprise. Diagnostic tells when a pipeline case fails:
an exit `0x7f00` in home.log (a stage's exec failed) or a sha256 of
`e3b0c442...` (a producer stage wrote nothing).

## 5. Performance

```bash
sudo ./tests/perf.sh mytag
```

Native vs loopback for wc/curl/sha/python/gzip/links2/vmbench, three
runs each, with vmremote's own per-op accounting (never the harness
wall — the clock trap is on record). `tests/perfab.sh` is the two-arm
A/B form. Cross numbers: run the same programs through runany-x and read
the `[vmremote] time:` line; `build/vmbench N M` prints compute Miter/s
and µs/syscall directly. `GOV=performance` sets the cpufreq governor for
the run and restores it after; the table header prints the governor and
`vmctx_spin_us` on every run.

Current numbers (session 46, kernel #181, HANDOFF.md §46; loopback
medians of 5, schedutil unless said): compute 99.6% of native; forwarded
syscall **16 µs** (61 before the session; 15 under the performance
governor); page fault 29–74 µs/event (37–89 before). Walls: wc
0.007→0.041 s, curl 0.007→0.060, python3 1M-sum 0.021→0.174, sha256(libc)
0.010→0.135, gzip 0.43→1.22, links2 -dump 0.017→0.084 (before the
session: 0.051 / 0.083 / 0.313 / 0.151 / 1.345 / 0.114).

The two machine settings that move these more than anything in the tree:
the cpufreq governor (schedutil reads a sleep-per-hop protocol as an idle
box and clocks the cores down — 42–114 µs/syscall against 35–40 under
`performance` on the pre-spin path; the spins mostly cure that by keeping
the cores busy) and the three spin budgets (AUDIT §9). Rules: an idle
box (a pool beside a measurement is a different load), no uutils
coreutils inside any measured wall, and medians over ≥3 runs.

## 6. The browser demo

```bash
sudo ./browser-demo.sh                      # links2 -g, self-grading
sudo PROG=/usr/bin/netsurf ./browser-demo.sh
```

GREEN = a real window, a positive distinct-colour delta over the empty
root, zero unrecoverable faults (links2's +253 is the calibrated
baseline). The script removes the two environment tarpits outside the
guest: a native session dbus-daemon whose address the guest inherits,
and the fakebwrap PATH shim. Needs the box's X up (`systemctl start
gdm`); artifacts in `/tmp/browser-demo/` including before/after screen
dumps.

## 7. Building and deploying

**Userspace + tests + module to the AMD box** (from home):

```bash
cd vmctx && ./sync-amd.sh              # userspace + tests
MODULE=1 ./sync-amd.sh                 # ...and rebuild the module
./verify-build.sh                      # end-to-end proof
```

Binaries are DELETED before rebuilds (rsync preserves mtimes; a fresh
source looks older than the stale binary beside it). The module is
rebuilt against the box's own kernel tree and its vermagic checked; the
final verify must say everything matches.

**vmremote to the Intel box** (it has no compiler and no sftp):

```bash
make -s -C vmctx/user ../build/vmremote     # 6.18.35 numbers
ssh -i config/id_vmctx root@10.0.0.30 \
  'cat > /usr/local/bin/vmremote.new && chmod +x /usr/local/bin/vmremote.new \
   && mv /usr/local/bin/vmremote.new /usr/local/bin/vmremote \
   && /usr/local/bin/vmremote --build-id' < vmctx/build/vmremote
```

Keep a `.pre-<change>` aside before replacing.

**The source kernel on the AMD box.** The tree of record is
`src/linux-7.0.14` HERE (base commit + ONE vmctx commit; the patch in
`kernel-patches/` is regenerated on every change and is the only
off-box copy). The box builds from its own copy of the tree:

```bash
scp -i config/id_vmctx src/linux-7.0.14/kernel/vmctx.c \
    biwu@10.0.0.229:vmctx-build/linux-7.0.14/kernel/vmctx.c
scp -i config/id_vmctx src/linux-7.0.14/include/linux/vmctx.h \
    biwu@10.0.0.229:vmctx-build/linux-7.0.14/include/linux/vmctx.h   # when it changed
ssh ... 'sudo -A bash ~/build-181.sh'   # builds, installs vmlinuz+initrd PAIR
                                        # (a copy of build-173.sh per kernel; #181 = session 46)
ssh ... 'sudo -A systemctl stop gdm; sudo -A reboot'   # NVIDIA hangs without the stop
# wait; verify:  uname -v  (a build counter, NOT an identity -- SRCID is)
MODULE=1 ./sync-amd.sh && sudo insmod ~/vmctx-7.0.14.ko
```

Never restore half of /boot: `make install` produces a MATCHING
vmlinuz+initrd pair; a vmlinuz-only restore over a foreign initrd is
"unable to mount root" at the console. The stock Ubuntu kernel entries
are the fallback of record; `~/kernel-168-full/` holds a complete trio
(vmlinuz + initrd + System.map + config + modules tar). Keep experiment
kernels OUT of /boot — any stray `vmlinuz-*` there becomes a GRUB entry.

## 8. Reading a run (triage order)

1. `guest.out` — did the program answer?
2. The exit code against AUDIT §8 (97/98/124/131/242/251 each name a
   class).
3. `remote.log` tail — the counters name what moved and what was
   declined; `DECLINED-BY`, `LOST`, `STUCK`, `LAST-RESORT`,
   `SERVED-BY` lines carry the page-level story.
4. `home.log` (binary-safe: `grep -a`) — the syscall trail and the
   per-context close statuses (`status 0xf200` = 242 = a fault death;
   `0x7f00` = an exec failed).
5. dmesg on the box (sudo) — the kernel's corpse lines: the faulting
   rip/address, the last 16 syscalls/faults of the dying context, and
   the `vmctx:` counters (`vmctx_take_lost`, `fault_deadline_hits`,
   `pgrec_home_healed` must all explain themselves).
6. For a page's whole biography: re-run with `VMHOME_PGLOG=0xADDR` and
   `VMR_PGLOG=0xADDR` (AUDIT §9 lists every diagnostic). Instruments
   can mask races — never quote an armed run's numbers.

## 9. Etiquette (each rule was paid for)

- One machine operation at a time; never overlap build/install/reboot.
- Pools are not measurements; idle-box and loaded-box numbers never
  compare.
- Kill monitors BY PID, never pkill by name.
- /tmp is tmpfs and fills: pools and campaigns write under /home/biwu.
- Timeouts are diagnoses, not cleanup: keep the corpse.
- The suite must run as root; runs over ssh need setsid and </dev/null.
