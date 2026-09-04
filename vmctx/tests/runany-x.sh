#!/bin/bash
# runany-x.sh <tag> <program> [args...] -- runany.sh's contract, CROSS-MACHINE.
#
# Same contract suite.sh depends on: run the program, leave the guest's stdout
# at /tmp/tr/<tag>/guest.out, exit 3 if the run produced nothing. Because the
# contract is the same, the whole suite runs cross-machine unchanged:
#
#   RUNANY=./tests/runany-x.sh ./tests/suite.sh
#
# and every case keeps its own grading predicate instead of a second, weaker
# copy of it.
#
# Run this ON THE SOURCE (10.0.0.229): it owns the program and its memory, and
# it is the only machine on this LAN that can be one -- home is WSL2 on a stock
# kernel with no vmctx. The destination (10.0.0.30, Intel/VMX, 6.18.35) executes
# and has no files.
set -u
T=${1:?usage: runany-x.sh <tag> <program> [args...]}
shift
PROG=${1:?usage: runany-x.sh <tag> <program> [args...]}
shift
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
DST=${DST:-10.0.0.30}
SRC_IP=${SRC_IP:-10.0.0.229}
PORT=${PORT:-9971}
# The suite's time budget, scaled for the link.
#
# suite.sh sets VMR_LIMIT=60, a number chosen against loopback, where a
# forwarded syscall is 113-116us. Across the LAN the same call is 540-554us --
# 4.7x -- so a syscall-bound case does 4.7x less work in the same 60 seconds
# and is killed part-way through. That is not the case failing; it is the
# budget measuring the network. pg3 is the one that shows it: 20000 rounds on
# two threads is ~80000 forwarded calls, it reached ~13900 rounds in 60s with
# an EMPTY guest.out (it prints only at the end) and was scored NO-OUTPUT --
# and at 240s it finishes and PASSes, 0 behind, 0 ahead, 0 short.
#
# So the multiplier is the measured ratio, rounded up, and it is applied to
# whatever the caller asked for rather than replacing it. VMR_XLIMIT sets the
# cross-machine limit outright when an exact number is wanted.
LIMIT=${VMR_XLIMIT:-$(( ${VMR_LIMIT:-60} * 5 ))}
OUT=/tmp/tr/$T
KEY=${KEY:-$HERE/../config/id_vmctx}
SDST="ssh -i $KEY -o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null \
      -o BatchMode=yes -o ConnectTimeout=8 -o LogLevel=ERROR root@$DST"

mkdir -p "$OUT"; rm -f "$OUT"/guest.out "$OUT"/home.log "$OUT"/remote.log "$OUT"/run.log

# Both ends first. A leftover vmhome here holds the port and the next run talks
# to the OLD one; a leftover vmremote there keeps a context alive against a
# source that has gone. Both have produced "failures" that were neither end's.
pkill -9 -x vmhome 2>/dev/null
$SDST "pkill -9 -x vmremote" 2>/dev/null
# The page service is on PORT+1; wait for BOTH, and take a different pair rather
# than starting anyway -- an unbound port makes vmhome die on bind, and an empty
# guest.out is scored as a failure of the program. See local-here.sh.
port_busy() { ss -ltn 2>/dev/null | grep -qE ":($1|$(($1 + 1))) "; }
for i in $(seq 1 40); do port_busy "$PORT" || break; sleep 0.25; done
if port_busy "$PORT"; then
	for p in $(seq $((PORT + 2)) $((PORT + 60))); do
		port_busy "$p" || { PORT=$p; break; }
	done
fi

# The destination's kernel release, for the vDSO decision: matching releases
# let the destination serve its own vvar (live vDSO time); differing ones make
# vmhome hide the vDSO from the first image so clocks go by forwarded syscall.
# Asked of the destination itself, once per run, because a hardcoded value
# here is exactly the kind of record that drifts.
export VMCTX_DEST_RELEASE=${VMCTX_DEST_RELEASE:-$($SDST "uname -r" 2>/dev/null)}
setsid nohup "$HERE/build/vmhome" "$PORT" -v "$PROG" "$@" \
	> "$OUT/guest.out" 2> "$OUT/home.log" < /dev/null &
hp=$!
for i in $(seq 1 40); do port_busy "$PORT" && break; sleep 0.25; done

timeout $((LIMIT + 20)) $SDST \
	"timeout -s TERM $LIMIT /usr/local/bin/vmremote $SRC_IP:$PORT --net" \
	> "$OUT/remote.log" 2>&1
rc=$?
{ echo "== cross-machine run: source $SRC_IP -> destination $DST"; \
  echo "== vmremote exit $rc"; } > "$OUT/run.log"

sleep 0.5
kill "$hp" 2>/dev/null; pkill -x vmhome 2>/dev/null
$SDST "pkill -x vmremote" 2>/dev/null

# suite.sh reads the exit status to tell a harness fault from a test failure.
[ -s "$OUT/guest.out" ] || exit 3
exit 0
