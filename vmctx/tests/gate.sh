#!/bin/bash
# gate.sh <tag> -- one stage's gate of the ownership redesign (REDESIGN.md):
# suite x3 serial, then the hx2 and pg3 pools (256 runs, 8 at a time), every
# run kept and graded by guest.out, dmesg streamed to disk for the whole
# chain, the kernel's must-stay-zero counters read before and after, and a
# census of the tie-break mechanisms over both pools at the end.
#
# Run as root on the box that runs both halves, in a session of its own:
#
#   setsid nohup ./tests/gate.sh s29a < /dev/null > /home/biwu/gate.s29a.out 2>&1 &
#
# Everything lands under $GATE_HOME (default /home/biwu -- a disk, never the
# 7.3 GB /tmp tmpfs, which two archived pools fill). Kill handle:
# kill -- -$(cat /tmp/gate.pid). Binaries are NOT rebuilt here: sync-amd.sh
# has already built them, and a pool execs build/vmhome per run, so a rebuild
# mid-gate would mix two builds in one measurement.
set -u
TAG=${1:?usage: gate.sh <tag>}
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
GH=${GATE_HOME:-/home/biwu}
TESTS=${TESTS:-/home/biwu/tests}
P=/sys/module/kernel/parameters
export TESTS

cd "$HERE" || exit 1
[ "$(id -u)" = 0 ] || { echo "must run as root" >&2; exit 2; }
echo $$ > /tmp/gate.pid

counters() {
	local k
	for k in take_lost take_refused mm_overflow takeobj_stale_store \
		 fault_deadline_hits foreign_refused ffile_invent \
		 zeropage_forbidden syscall_pages_lost \
		 shm_self_user shm_self_kern shm_foreign_read shm_large \
		 takeobj_zero takeobj_zero_named takeobj_zero_unnamed; do
		printf '%s=%s ' "$k" "$(cat $P/vmctx_$k 2>/dev/null)"
	done
	echo
}

stamp() { echo "=== $* $(date +%T)"; }

stamp "gate $TAG starts; build ids:"
"$HERE/build/vmhome" --build-id; "$HERE/build/vmremote-local" --build-id
echo "kernel: $(uname -v)"
echo "leftovers: $(for f in /proc/[0-9]*/comm; do c=$(cat "$f" 2>/dev/null); case "$c" in vmhome|vmremote-local) echo -n "${f#/proc/} ";; esac; done)"
echo "counters before: $(counters)"

dmesg --follow -T > "$GH/dmesg.$TAG.log" 2>&1 &
DM=$!

stamp "suite x3"
SKIP_BUILD=1 ./tests/suite.sh 3 > "$GH/suite.$TAG.log" 2>&1
grep -a "suite:\|FAIL\|XPASS\|NO-OUTPUT\|MISSING" "$GH/suite.$TAG.log"
echo "counters after suite: $(counters)"

for case in hx2 pg3; do
	stamp "$case pool 256 x 8"
	rm -rf "$GH/torture.$case.$TAG"
	OUTD="$GH/torture.$case.$TAG" CASE_OK='^PASS$' WATCH=0 KEEP_PASS=1 \
		./tests/torture.sh "$case" 256 8 > "$GH/torture.$case.$TAG.log" 2>&1
	cat "$GH/torture.$case.$TAG.log"
	echo "counters after $case: $(counters)"
	./tests/census.sh "$GH/torture.$case.$TAG"
done

kill "$DM" 2>/dev/null
echo "dmesg lines of note: $(grep -ac 'claim refused AFTER\|CHANGED after\|deadline\|STALE' "$GH/dmesg.$TAG.log")"
grep -a 'claim refused AFTER\|CHANGED after the unmap' "$GH/dmesg.$TAG.log" | head -5
stamp "gate $TAG done"
