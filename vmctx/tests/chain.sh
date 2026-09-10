#!/bin/bash
# chain.sh <tag> -- the session-32..36 measurement chain, tracked: suite x1,
# then 48x8 pools of hx2 pg3 ws1 sig1, each kept whole and graded by census
# (guest.out ^PASS$ -- NEVER by torture.sh's dir names, which file every hx2
# and sig1 run as a failure), with the kernel's counters read before and after
# every pool and dmesg streamed to disk for the whole chain.
#
# Run as root on the box, in a session of its own (see running-the-suite-over-
# ssh): setsid nohup ./tests/chain.sh s36a < /dev/null > /home/biwu/chain.s36a.out 2>&1 &
# Done when /home/biwu/chain.<tag>.done exists. Everything lands under
# $GATE_HOME (default /home/biwu -- a disk, never the tmpfs /tmp).
set -u
TAG=${1:?usage: chain.sh <tag>}
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
GH=${GATE_HOME:-/home/biwu}
RUNS=${RUNS:-48}
JOBS=${JOBS:-8}
CASES=${CASES:-hx2 pg3 ws1 sig1}
P=/sys/module/kernel/parameters
export TESTS=${TESTS:-/home/biwu/tests}
cd "$HERE" || exit 1
[ "$(id -u)" = 0 ] || { echo "must run as root" >&2; exit 2; }
rm -f "$GH/chain.$TAG.done"
echo $$ > /tmp/chain.pid

# The must-stay-zero and the naming counters (kernel #150: vmctx_shm_* is the
# census of folios the kernel created in a live context's object, by path;
# takeobj_zero* says whether each all-zero capture's maker was named).
counters() {
	local k
	for k in take_lost take_refused mm_overflow takeobj_stale_store \
		 fault_deadline_hits ffile_invent mapobj_holes \
		 shm_live shm_self_user shm_self_kern shm_self_kern_lsvc shm_mapper \
		 shm_foreign_write shm_foreign_read shm_file_write shm_file_falloc \
		 shm_file_other shm_large takeobj_zero takeobj_zero_named \
		 takeobj_zero_unnamed; do
		printf '%s=%s ' "$k" "$(cat $P/vmctx_$k 2>/dev/null || echo -)"
	done
	echo
}
graded() {	# pass/fail of a kept pool, the way census grades it
	local d=$1 p=0 f=0
	for r in "$d"/*/; do
		[ -f "$r/remote.log" ] || continue
		if grep -qax PASS "$r/guest.out" 2>/dev/null; then p=$((p+1)); else f=$((f+1)); fi
	done
	echo "$p/$((p+f))"
}
dm_notes() {	# kernel lines of note since $1 (a dmesg line count)
	local from=$1 f="$GH/dmesg.$TAG.log"
	tail -n +$((from + 1)) "$f" 2>/dev/null | grep -ac 'SHMEM-ALLOC' | sed 's/^/shmem_alloc_lines=/' | tr '\n' ' '
	tail -n +$((from + 1)) "$f" 2>/dev/null | grep -ac 'ZERO-TAKE' | sed 's/^/zero_take_lines=/' | tr '\n' ' '
	tail -n +$((from + 1)) "$f" 2>/dev/null | grep -ac 'CHANGED after\|claim refused AFTER\|deadline' | sed 's/^/other_lines=/'
}

echo "=== chain $TAG starts $(date +%T) $(./build/vmhome --build-id 2>&1 | tail -1) kernel $(uname -v)"
echo "leftovers: $(for f in /proc/[0-9]*/comm; do c=$(cat "$f" 2>/dev/null); case "$c" in vmhome|vmremote-local) echo -n "${f#/proc/} ";; esac; done)"
dmesg --follow -T > "$GH/dmesg.$TAG.log" 2>&1 &
DM=$!
sleep 1
echo "counters before: $(counters)"

SKIP_BUILD=1 ./tests/suite.sh 1 > "$GH/suite.$TAG.log" 2>&1
grep -a "suite:\|FAIL\|XPASS\|NO-OUTPUT\|MISSING" "$GH/suite.$TAG.log"
echo "counters after suite: $(counters)"

for case in $CASES; do
	from=$(wc -l < "$GH/dmesg.$TAG.log")
	echo "=== $case pool ${RUNS}x$JOBS $(date +%T)"
	rm -rf "$GH/torture.$case.$TAG"
	OUTD="$GH/torture.$case.$TAG" CASE_OK='^PASS$' WATCH=0 KEEP_PASS=1 \
		./tests/torture.sh "$case" "$RUNS" "$JOBS" > "$GH/torture.$case.$TAG.log" 2>&1
	echo "$case: $(graded "$GH/torture.$case.$TAG") pass (graded by guest.out)"
	echo "counters after $case: $(counters)"
	echo "dmesg during $case: $(dm_notes "$from")"
	./tests/census.sh "$GH/torture.$case.$TAG" > "$GH/census.$case.$TAG.txt" 2>&1
	head -1 "$GH/census.$case.$TAG.txt"
done

kill "$DM" 2>/dev/null
echo "=== chain $TAG done $(date +%T)"
touch "$GH/chain.$TAG.done"
