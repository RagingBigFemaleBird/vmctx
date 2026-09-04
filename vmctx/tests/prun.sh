#!/bin/bash
#
# prun.sh <case> <runs> [jobs] — one case, N times, J at a time, on this box.
#
# Every rate in AUDIT.md was measured by running one case sixteen times in a
# row, which is fifteen minutes of a box doing one thing. Nothing ever required
# that: a run is a (vmhome, vmremote) pair on a port of its own, and the module
# already serves many contexts at once because a threaded guest is many
# contexts. What made runs exclusive was local-here.sh — it never killed the
# vmhome it started, so its only way to get a clean machine was to kill every
# vmhome and vmremote on it and reload the module. Each run now takes its own
# vmhome down and VMCTX_SHARED=1 skips the sweep.
#
# The caveat is the whole reason this is a separate script rather than the
# default: **a pool changes the load, and the page tests are races.** A rate
# measured eight-at-a-time is not comparable with a rate measured serially, and
# comparing one against the other is the mistake this project keeps paying for.
# Compare a pool against a pool, on one boot, with the arms alternating — which
# is what the two-arm form below does.
#
#   ./prun.sh pg2 16 8              one arm, sixteen runs, eight at a time
#   ARM_A='VMHOME_PGLOG=1' ARM_B='' ./prun.sh pg2 16 8
#                                   two arms, alternating, same pool
#
set -u
T=${1:?usage: prun.sh <case> <runs> [jobs]}
N=${2:-8}
J=${3:-8}
HERE=$(cd "$(dirname "$0")" && pwd)
V=$(cd "$HERE/.." && pwd)
TESTS=${TESTS:-/home/biwu/tests}
PORT_BASE=${PORT_BASE:-23000}
OUTD=${OUTD:-/tmp/prun.$T}

# Built here, from the source in this checkout, for the same reason
# local-here.sh rebuilds vmremote on every run: rsync preserves mtimes, so an
# updated test looks older than its binary and no timestamp check catches it.
# A measurement of a test that was not the test in the tree is worth nothing.
if [ -f "$HERE/$T.c" ]; then
	gcc -O2 -Wall -static -pthread -o "$TESTS/$T" "$HERE/$T.c" -lrt || {
		echo "prun: $T.c does not compile" >&2; exit 2; }
fi
[ -x "$TESTS/$T" ] || { echo "prun: no such case: $TESTS/$T" >&2; exit 2; }

# Built once, here, and never inside a worker: several gcc's writing one output
# file while another worker executes it is a failure with no honest explanation.
# Kept out of the report but not thrown away: a build that fails prints its own
# log, and a build that only warns does not drown the numbers below in gcc.
BLOG=$(mktemp)
if ! ( cd "$V/user" && gcc -O2 -Wall -Wextra -static -fno-stack-protector -pthread \
	-I../kernel -D__NR_vmctx_run=472 -D__NR_vmctx_ctl=473 \
	-Wl,-Ttext-segment=0x20000000 -o "$V/build/vmremote-local" vmremote.c ) \
	> "$BLOG" 2>&1 || ! make -s -C "$V/user" ../build/vmhome >> "$BLOG" 2>&1; then
	echo "prun: cannot build:" >&2; cat "$BLOG" >&2; rm -f "$BLOG"; exit 1
fi
rm -f "$BLOG"
lsmod | grep -q '^vmctx' || insmod "${KO:-${SUDO_USER:+/home/$SUDO_USER}/vmctx-7.0.14.ko}" ||
	{ echo "prun: cannot load the module" >&2; exit 1; }

rm -rf "$OUTD"; mkdir -p "$OUTD"

one() {	# $1 = run index, $2 = slot, $3 = arm name, $4 = arm env
	local i=$1 s=$2 arm=$3 env=$4
	local o="$OUTD/$arm.$i"

	mkdir -p "$o"
	( cd "$V" &&
	  env $env VMCTX_SHARED=1 PORT=$((PORT_BASE + s * 4)) OUT="$o" \
		KO="${KO:-${SUDO_USER:+/home/$SUDO_USER}/vmctx-7.0.14.ko}" \
		VMR_LIMIT=${VMR_LIMIT:-120} \
		./local-here.sh "$TESTS/$T" > "$o/run.log" 2>&1 )
	if grep -qx PASS "$o/guest.out" 2>/dev/null; then
		echo "$arm PASS" >> "$OUTD/results"
	else
		echo "$arm FAIL" >> "$OUTD/results"
		echo "  $arm run $i: $(grep -vE '^\[|^$' "$o/guest.out" 2>/dev/null | tail -1)"
	fi
}

: > "$OUTD/results"
slot_pid=()
launch() {	# $1 = index, $2 = arm, $3 = env
	local s
	while :; do
		for s in $(seq 0 $((J - 1))); do
			if [ -z "${slot_pid[$s]:-}" ] ||
			   ! kill -0 "${slot_pid[$s]}" 2>/dev/null; then
				one "$1" "$s" "$2" "$3" &
				slot_pid[$s]=$!
				return
			fi
		done
		sleep 0.2
	done
}

t0=$(date +%s)
for i in $(seq 1 "$N"); do
	if [ -n "${ARM_A+x}" ] || [ -n "${ARM_B+x}" ]; then
		# Alternating, not blocked: a blocked arm has been read here as a
		# fix before, and the box's own state drifts within a session.
		launch "$i" a "${ARM_A:-}"
		launch "$i" b "${ARM_B:-}"
	else
		launch "$i" one ""
	fi
done
wait
t1=$(date +%s)

for arm in $(cut -d' ' -f1 "$OUTD/results" | sort -u); do
	p=$(grep -c "^$arm PASS$" "$OUTD/results")
	f=$(grep -c "^$arm FAIL$" "$OUTD/results")
	echo "== $T arm '$arm': $p passed, $f failed of $((p + f))"
done
echo "== $((t1 - t0))s wall, $J at a time; logs in $OUTD"
