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
# Overridable like VMREMOTE below, so an arm can name the binary it means to
# measure. Without this a two-arm run of vmhome has to overwrite build/vmhome,
# which is root-owned and shared with every other run on the box.
VMHOME=${VMHOME:-$HERE/build/vmhome}
VMREMOTE=${VMREMOTE:-$HERE/build/vmremote-local}
OUT=${OUT:-/tmp/localrun}

# Source forks clear PDEATHSIG and can outlive vmhome. Keep every process of
# this run below a subreaper so timeout cleanup still owns those orphans.
if [ "${VMCTX_OWNED_RUN:-}" != "$PPID" ]; then
	exec python3 "$HERE/tests/owned-run.py" --report "$OUT/cleanup.json" \
		bash "$0" "$@"
fi

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

# A harness owns only its descendants. Never kill unrelated runs or reload a
# module underneath them. The caller builds/deploys modules; load an absent
# one here and verify the loaded source identity when that metadata exists.
if [ ! -d /sys/module/vmctx ]; then
	insmod "$KO" ${KOARGS:-} || { echo "cannot load $KO" >&2; exit 1; }
fi
if [ -r /sys/module/vmctx/srcversion ]; then
	wanted=$(modinfo -F srcversion "$KO") || exit 1
	loaded=$(cat /sys/module/vmctx/srcversion) || exit 1
	[ -n "$wanted" ] && [ "$loaded" = "$wanted" ] || {
		echo "loaded vmctx differs from $KO; deploy the intended module before running" >&2
		exit 1
	}
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
# The outer owned-run retains and terminates this run's tasks via pidfds.
trap 'exit 130' INT
trap 'exit 143' TERM
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

# The nested owner times and reaps the executor and all of its descendants.
# Its pidfds and recorded births avoid signalling a recycled process/group
# after wait. The outer owner retains the source until this teardown finishes.
ulimit -c unlimited 2>/dev/null
rm -f "$OUT/.timedout"
executor_options=()
if [ -n "${VMR_LIMIT:-}" ]; then
	executor_options=(--timeout "$VMR_LIMIT" --timeout-marker "$OUT/.timedout")
fi
t0=$EPOCHREALTIME
( [ -n "${VMREMOTE_ENV:-}" ] && export ${VMREMOTE_ENV}
  exec python3 "$HERE/tests/owned-run.py" --report "$OUT/executor-cleanup.json" \
	"${executor_options[@]}" "$VMREMOTE" 127.0.0.1:$PORT --net ) \
	> "$OUT/remote.log" 2>&1
rc=$?
t1=$EPOCHREALTIME

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
