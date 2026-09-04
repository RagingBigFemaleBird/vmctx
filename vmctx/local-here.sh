#!/bin/bash
# local-here.sh — both ends of a remote execution on *this* machine, over its
# own loopback. The counterpart of local-run.sh, for a machine that is running
# vmctx itself rather than being driven over ssh.
#
#   sudo -E ./local-here.sh /usr/bin/dillo http://127.0.0.1:8899/page.html
#
# Everything here belongs to this machine's kernel: vmctx-7.0.14.ko and a
# vmremote built with this kernel's syscall numbers. The tree also holds the
# artefacts for the *other* machine (6.18.35, syscalls 470/471) because
# remote-run.sh pushes those to it — mixing the two gives "invalid module
# format" at best and a guest calling the wrong syscall at worst, so they are
# named apart rather than swapped in place.
set -u
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
# In SHARED mode the leftover sweep is skipped BY DESIGN, so the whole safety
# contract is "a port of each run's own" -- and a caller who sets
# VMCTX_SHARED=1 without setting PORT has silently broken it: back-to-back
# runs on the default port let a dying run's vmremote reconnect into the NEXT
# run's vmhome, whose guest then runs on unserviced (invented-zero) pages and
# dies in libc on NULL globals. Measured: 3 of 10 back-to-back shell
# pipelines segfaulting at 0x2d8-off-a-NULL-FILE*, 0 of 10 once ports were
# distinct. So in shared mode an UNSET port is picked fresh: a free pair
# (PORT and PORT+1, the page channel) from a range no other harness uses.
if [ "${VMCTX_SHARED:-}" = 1 ] && [ -z "${PORT:-}" ]; then
	for _p in $(seq $((24000 + RANDOM % 800 * 4)) 4 27999); do
		ss -ltn 2>/dev/null | grep -qE ":($_p|$((_p + 1))) " || {
			PORT=$_p; break; }
	done
fi
PORT=${PORT:-9998}
KO=${KO:-$HOME/vmctx-7.0.14.ko}
# Both halves run on THIS machine, so source and destination kernels are the
# same release by construction -- the fact vmremote's vvar serving is gated
# on (the guest's vDSO reads destination-vvar bytes; the struct layouts must
# match, and same release proves it). Exported here so every loopback run
# gets live vDSO time without each caller knowing the mechanism; a caller
# may still override it (a cross harness feeds the real fact instead).
export VMCTX_SRC_RELEASE=${VMCTX_SRC_RELEASE:-$(uname -r)}
# Overridable like VMREMOTE below, so an arm can name the binary it means to
# measure. Without this a two-arm run of vmhome has to overwrite build/vmhome,
# which is root-owned and shared with every other run on the box.
VMHOME=${VMHOME:-$HERE/build/vmhome}
VMREMOTE=${VMREMOTE:-$HERE/build/vmremote-local}
OUT=${OUT:-/tmp/localrun}

[ $# -ge 1 ] || { echo "usage: $0 <program> [args...]" >&2; exit 2; }
[ "$(id -u)" = 0 ] || { echo "must run as root (vmhome ptraces the program)" >&2; exit 2; }

mkdir -p "$OUT"
rm -f "$OUT"/guest.out "$OUT"/home.log "$OUT"/remote.log

# Always rebuild the local vmremote from source.
#
# It is not built by the Makefile (that one is for the *other* machine, with
# that machine's syscall numbers), so nothing else keeps it current — and rsync
# preserves mtimes, so an updated source looks OLDER than the binary and no
# timestamp check would catch it either. A stale one here cost hours: it still
# forwarded futex, so the shadow sat in futex_do_wait holding a reply the guest
# was waiting for, and the run looked exactly like a kernel bug on this machine.
# And no header may shadow the canonical one.
#
# vmremote.c says #include "vmctx_uapi.h", and a quoted include searches the
# including file's own directory before any -I. A stale copy left in user/ by an
# earlier layout therefore wins over kernel/, silently, and the build fails or --
# worse -- succeeds against the wrong ABI. One sat there for a day; the run it
# broke reported "0 passed, 20 failed" for every test at once with no guest
# output at all, and the compile error was buried in run.log where nothing looks.
if [ -e "$HERE/user/vmctx_uapi.h" ]; then
	echo "$HERE/user/vmctx_uapi.h shadows kernel/vmctx_uapi.h; remove it" >&2
	exit 1
fi

# Not in shared mode: several runs at once would be several gcc's writing one
# output file, and a binary half-written by one compiler while another executes
# it is a failure with no honest explanation anywhere. The caller that starts
# concurrent runs builds once, before the first of them, and says so by setting
# VMCTX_SHARED -- it is the same rebuild, taken out of the racing part.
if [ "${VMCTX_SHARED:-}" = 1 ]; then
	[ -x "$VMREMOTE" ] && [ -x "$VMHOME" ] || {
		echo "VMCTX_SHARED=1 but $VMREMOTE or $VMHOME is missing; the" >&2
		echo "caller must build them once before starting parallel runs" >&2
		exit 1; }
else
	( cd "$HERE/user" && gcc -O2 -Wall -Wextra -static -fno-stack-protector -pthread \
		-I../kernel -D__NR_vmctx_run=472 -D__NR_vmctx_ctl=473 \
		-DVMCTX_SRC_ID=\"$(make -s print-srcid)\" \
		-Wl,-Ttext-segment=0x20000000 -o "$VMREMOTE" vmremote.c ) || {
		echo "cannot build $VMREMOTE" >&2; exit 1; }

	# And vmhome, for the same reason: it is the other half of the same
	# protocol, and testing a new vmremote against last week's vmhome
	# measures neither.
	make -s -C "$HERE/user" ../build/vmhome || {
		echo "cannot build $VMHOME" >&2; exit 1; }
fi

# The module must be the one built for this kernel, and it must be freshly
# loaded: a stale one silently invalidates every result.
#
# And nothing from a previous run may still be alive. A run that hung leaves
# shadows behind — some in namespaces of their own, some zombies whose parent
# is another shadow — and they are not harmless: they hold the descriptors and
# the ports of the run that made them, so the next run inherits a world that
# does not match its own logs. Two of the thread tests appeared to regress that
# way, for long enough to be believed, when what had changed was the leftovers
# from the sandbox runs before them. So this loops until they are gone and
# refuses to start if they are not.
# By pid from /proc/*/comm, never by pkill/pgrep. Both of those read
# /proc/<pid>/cmdline, which takes the target's mmap lock -- and the exact
# leftover this loop exists to kill is a context asleep in the kernel holding
# that lock. pkill then joins it in D-state, this loop never finishes, and the
# machine reads load 6 at 0% CPU. Measured twice before it was believed.
find_by_comm() {
	local f c p
	for f in /proc/[0-9]*/comm; do
		c=$(cat "$f" 2>/dev/null) || continue
		case " $* " in *" $c "*) p=${f#/proc/}; echo "${p%/comm}";; esac
	done
}
# VMCTX_SHARED=1: this box is running other vmctx runs at the same time, so
# neither of the two steps below may be taken -- killing every vmhome and
# vmremote on the machine kills them, and rmmod/insmod pulls the module out from
# under them. Nothing in the system needs either: a run is a (vmhome, vmremote)
# pair on a port of its own with an output directory of its own, and the module
# already serves many contexts at once because a threaded guest is many
# contexts. Measured: 8 pairs at once, fi1 8 of 8, and sm1/pg2 4 of 4.
#
# What made runs exclusive was this script, and one line of it in particular:
# it never killed the vmhome it started, so every run left one behind and the
# next run's only way to get a clean machine was to kill them all. That is fixed
# below for both modes -- each run now takes its own down -- which is why the
# global sweep can be skipped rather than merely bypassed.
if [ "${VMCTX_SHARED:-}" = 1 ]; then
	lsmod | grep -q '^vmctx' || insmod "$KO" ${KOARGS:-} ||
		{ echo "cannot load $KO" >&2; exit 1; }
else
	for i in 1 2 3 4 5 6 7 8; do
		for p in $(find_by_comm vmhome vmremote-local vmremote); do
			kill -9 "$p" 2>/dev/null
		done
		sleep 0.5
		[ -z "$(find_by_comm vmhome vmremote-local)" ] && break
	done
	left=$(find_by_comm vmhome vmremote-local)
	if [ -n "$left" ]; then
		echo "processes from a previous run will not die (pids: $left):" >&2
		echo "refusing to run: its results would describe their world, not this one" >&2
		exit 1
	fi
	for i in 1 2 3 4 5; do
		lsmod | grep -q '^vmctx' || break
		rmmod vmctx 2>/dev/null
		sleep 1
	done
	lsmod | grep -q '^vmctx' && { echo "vmctx still loaded and will not unload" >&2; exit 1; }
	insmod "$KO" ${KOARGS:-} || { echo "cannot load $KO" >&2; exit 1; }
fi

# Wait for the port, for the reason local-run.sh spells out: a vmhome that
# still holds it means the next one dies and the run talks to the old one.
#
# BOTH ports, and then a different pair rather than a failure.
#
# This waited on $PORT alone, and the page service listens on $PORT+1
# (VMR_PG_PORT_OFFSET) -- so a previous run whose syscall port had drained
# while its page port had not passed the test and then died on bind. Worse, on
# expiry the loop simply fell through and started anyway, and what that
# produces is not an error anybody reads: vmhome exits with "bind: Address
# already in use", guest.out is EMPTY, and the run is scored as a silent
# no-output failure of whatever was being measured.
#
# It is not rare and it is not harmless. Measured over 20 back-to-back hx2
# runs: 2 no-output runs, BOTH of them this, neither of them a run of hx2 at
# all. Every A/B on this box has been carrying 1-4 such phantoms per arm, which
# is a large part of the run-to-run variance that made small effects
# unreadable.
port_busy() {
	ss -ltn 2>/dev/null | grep -qE ":($1|$(($1 + 1))) "
}
for i in $(seq 1 20); do
	port_busy "$PORT" || break
	sleep 0.4
done
if port_busy "$PORT"; then
	for p in $(seq $((PORT + 2)) $((PORT + 200))); do
		if ! port_busy "$p"; then
			echo "port $PORT (or $((PORT + 1))) still held by an" \
			     "earlier run; using $p instead" >&2
			PORT=$p
			break
		fi
	done
fi
if port_busy "$PORT"; then
	echo "no free port pair from $PORT upward -- refusing to start, because" \
	     "the alternative is an empty guest.out scored as a test failure" >&2
	exit 1
fi

# The program is named HERE, on the source's command line, because the source is
# the machine that has files. The destination is told where to connect and
# nothing else -- see PRINCIPLES 6a. This used to be the other way round: the
# destination was started with the program and shipped the strings over.
env ${VMHOME_ENV:-} "$VMHOME" "$PORT" -v "$@" > "$OUT/guest.out" 2> "$OUT/home.log" &
vmhome_pid=$!
# Take down the vmhome this run started, and only that one, however this script
# ends. It used to be left running: the next run's global sweep was the only
# thing that ever killed it, which is what made two runs on one box impossible
# and what made a leftover from a killed run poison the next one's results.
# By pid, never by name -- see find_by_comm above for why.
trap 'kill -9 "$vmhome_pid" 2>/dev/null' EXIT INT TERM
# Waited for rather than slept at: with several runs starting at once the fixed
# 0.6s was sometimes short, and a vmhome that had not printed yet was read as
# one that had failed to start.
for i in $(seq 1 60); do
	grep -q 'serving forwarded syscalls' "$OUT/home.log" 2>/dev/null && break
	kill -0 "$vmhome_pid" 2>/dev/null || break
	sleep 0.1
done
grep -q 'serving forwarded syscalls' "$OUT/home.log" || {
	echo "vmhome failed to start:" >&2; cat "$OUT/home.log" >&2; exit 1; }

# The limit belongs on vmremote and not on this script. `timeout` signals the
# process it starts, so a limit wrapped around the whole script kills the shell
# and leaves vmremote orphaned and running — and vmremote reports where a run's
# time went only when it is told to stop. The run that overruns is precisely the
# run those numbers are wanted for, so it has to be the one that gets the signal.
#
# Deliberately not named LIMIT: browser-run.sh already puts a `timeout $LIMIT`
# around this whole script, and reusing the name gives two timers set to the
# same value, with the outer one — the one that kills the shell and reports
# nothing — winning the race by however long this script took to start.
# Through env(1), because bash decides what is an assignment while it PARSES,
# before any expansion. `${VMREMOTE_ENV:-} "$VMREMOTE"` therefore does not set a
# variable at all: the value becomes the command NAME, and the run dies with
# "VMREMOTE_TRACE_FAULTS=1: command not found" inside remote.log where nothing
# looks. Every local measurement that named an instrument that way ran without
# it. (The identical construct in local-run.sh and remote-run.sh is fine, because
# there it is expanded into a string a *remote* shell then parses.)
# The limit bounds the process it names. It does not bound the guest's forks.
#
# `timeout` signals the command it starts, and a guest that forks makes MORE
# vmremote processes — one per forked context. When the limit fires, the one
# named dies and the others do not: measured, they are left with PPID 1, in
# vmremote's process group rather than this shell's, and in state R, spinning in
# user space. Two per overrunning round.
#
# That is not untidiness, it is a poisoned machine. Three of them were found
# holding this box at load 3.0, and every measurement taken afterwards — a
# suite, an A/B, a timing — was against that load without saying so. The run
# that overruns is usually the run being investigated, so the wreckage lands
# exactly where it does the most damage.
#
# So the group is swept after the wait. The group id is read BEFORE the wait,
# because afterwards there is no process left to ask, and it is compared with
# this shell's own group before anything is signalled: if `timeout` had not put
# the command in a group of its own, `kill -- -$pg` would kill this script and
# the suite running it. It does put it in one — that is why the survivors' PGID
# is not the shell's — and the comparison is what makes relying on that safe
# rather than assumed.
# A crash of the monitor itself leaves a core (core_pattern names it; the box
# sets /tmp/core.%e.%p), which is the only evidence a ret-to-zero can leave.
# The clock runs from the instant vmremote is started to the instant it has
# exited, and NOTHING of this script's own is inside it. It used to enclose
# two ps(1) invocations, a pgrep and the group kill below -- ~90 ms on an
# idle box and far more under a pool -- so every "=== exit N in Xs" and every
# perf.sh wall built on it carried the harness as if it were the system:
# wc read as 0.14-0.22 s when vmremote's whole life is 0.046 s (session 40).
#
# And no coreutils inside it either. This box's /usr/bin/timeout, env, date,
# cut and sleep are the uutils (Rust) multi-call binary: ~10 ms to exec each,
# and timeout(1) polls its child at 100 ms steps -- "timeout true" measured
# 114-121 ms, which was the whole 0.11 s floor every short program showed.
# So: the clock is bash's own $EPOCHREALTIME (no fork), the process group is
# made by util-linux setsid (vmremote becomes its own session leader, pgid =
# pid, which is what the sweep below kills), the environment is exported by
# this shell, and the limit is a watchdog subshell in a session of its own
# that leaves a marker before it kills -- rc 124, as timeout(1) answered.
ulimit -c unlimited 2>/dev/null
rm -f "$OUT/.timedout"
if [ -n "${VMR_LIMIT:-}" ]; then
	t0=$EPOCHREALTIME
	( [ -n "${VMREMOTE_ENV:-}" ] && export ${VMREMOTE_ENV}
	  exec setsid "$VMREMOTE" 127.0.0.1:$PORT --net ) \
		> "$OUT/remote.log" 2>&1 &
	vmr_wait=$!
	vmr_pg=$vmr_wait	# setsid: a new session, group id = its pid
	setsid bash -c 'sleep "$1"; touch "$2/.timedout"; kill -TERM -- -"$3" 2>/dev/null; sleep 2; kill -KILL -- -"$3" 2>/dev/null' \
		_ "$VMR_LIMIT" "$OUT" "$vmr_wait" < /dev/null > /dev/null 2>&1 &
	wd=$!
	wait "$vmr_wait"
	rc=$?
	t1=$EPOCHREALTIME
	kill -KILL -- -"$wd" 2>/dev/null
	[ -f "$OUT/.timedout" ] && rc=124
	my_pg=$(awk '{s=index($0,")"); split(substr($0,s+2),f," "); print f[3]}' \
		 /proc/$$/stat 2>/dev/null)
	if [ -n "$vmr_pg" ] && [ -n "$my_pg" ] && [ "$vmr_pg" != "$my_pg" ]; then
		left=$(pgrep -x vmremote-local 2>/dev/null | wc -l)
		kill -9 -- -"$vmr_pg" 2>/dev/null
		if [ "$left" -gt 0 ]; then
			sleep 0.2
			now=$(pgrep -x vmremote-local 2>/dev/null | wc -l)
			echo "=== swept $((left - now)) vmremote process(es) the limit" \
			     "left behind (group $vmr_pg) ==="
		fi
	fi
else
	t0=$EPOCHREALTIME
	( [ -n "${VMREMOTE_ENV:-}" ] && export ${VMREMOTE_ENV}
	  exec "$VMREMOTE" 127.0.0.1:$PORT --net ) > "$OUT/remote.log" 2>&1
	rc=$?
	t1=$EPOCHREALTIME
fi

# Milliseconds: /proc/uptime is centiseconds, which quantized every short
# program to 0.11 s; vmremote's own life for wc is ~50 ms (session 40).
echo "=== exit $rc in $(awk -v a=$t0 -v b=$t1 'BEGIN{printf "%.3f", b-a}')s ==="
echo "--- guest output ($OUT/guest.out) ---"; cat "$OUT/guest.out"
#
# The GUEST's stderr, separated from the monitor's.
#
# A forwarded write(2) to fd 2 is performed by the source's context task, whose
# fd 2 is vmhome's -- so the program's complaints land in home.log interleaved
# with vmhome's own diagnostics, thousands of lines of them. That is where
# netsurf's "X connection to :99 broken" sat while its exit status 1 was being
# called unexplained. Everything vmhome prints is prefixed; whatever is not is
# the program speaking, so the split costs one grep and makes the program's own
# words visible without VMHOME_PGLOG-sized reading.
grep -av '^\[vmhome\|^\[pg \|^\[PG \|^\[OWN\|^\[LEND\|^\[sigprobe\|^\[connect\]' \
     "$OUT/home.log" 2>/dev/null | grep -av '^$' > "$OUT/guest.err"
if [ -s "$OUT/guest.err" ]; then
	echo "--- guest stderr ($OUT/guest.err) ---"; cat "$OUT/guest.err"
fi
echo "--- vmremote ---"; grep -E '^\[vmremote\] (time|exited|pages served|faults declined)' "$OUT/remote.log"
exit $rc
