#!/bin/bash
# xrun.sh <tag> <program-on-source> [args...] -- one run with the two halves on
# two different machines, which is the configuration the objective is about.
#
#   ./xrun.sh hx2 /home/biwu/tests/hx2
#
# SOURCE is 10.0.0.229 (AMD/SVM, 7.0.14-vmctx): it owns the program, its files
# and its memory, and it is the only machine here that can be a source -- home
# is WSL2 on a stock kernel and has no vmctx at all, which is why this two-target
# form exists rather than remote-run.sh.
# DESTINATION is 10.0.0.30 (Intel/VMX, 6.18.35-0-lts): it executes and has no
# files.
#
# This script is in the repository on purpose. Its predecessor lived only in a
# scratch directory, and when that went so did every cross-machine measurement
# anyone could reproduce -- the same failure runany.sh had.
#
# Syscall numbers differ per kernel (470/471 on 6.18.35, 472/473 on 7.0.14) and
# are NOT set here: vmhome probes for its own at start-up, and vmremote is
# compiled for the kernel it is deployed on. verify-build.sh checks both.
set -u
T=${1:?usage: xrun.sh <tag> <program-on-source> [args...]}
shift
PROG=${1:?usage: xrun.sh <tag> <program-on-source> [args...]}
shift
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "$HERE/.." && pwd)"
SRC=${SRC:-10.0.0.229}
DST=${DST:-10.0.0.30}
PORT=${PORT:-9971}
LIMIT=${VMR_LIMIT:-60}
OUT=${OUT:-/tmp/xr/$T}
K="-i $ROOT/config/id_vmctx -o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null \
   -o BatchMode=yes -o ConnectTimeout=8 -o LogLevel=ERROR"
SSRC="ssh $K biwu@$SRC"
SDST="ssh $K root@$DST"
mkdir -p "$OUT"; rm -f "$OUT"/*

# Both ends, before anything starts. A leftover vmhome on the source holds the
# port and the next run talks to the OLD one; a leftover vmremote on the
# destination keeps a context alive against a source that has gone.
$SSRC "SUDO_PASS=biwu SUDO_ASKPASS=\$HOME/askpass.sh sudo -A pkill -9 -x vmhome" 2>/dev/null || true
$SDST "pkill -9 -x vmremote" 2>/dev/null || true
sleep 1

# The source names the program; the destination is told where to connect and
# nothing else (PRINCIPLES 6a).
# The destination's kernel release rides along for the vDSO decision (see
# runany-x.sh): differing releases make vmhome hide the vDSO from the first
# image, so the guest's clocks go by forwarded syscall instead of by a
# vvar page whose layout the destination cannot supply.
DREL=$($SDST "uname -r" 2>/dev/null)
$SSRC "SUDO_PASS=biwu SUDO_ASKPASS=\$HOME/askpass.sh sudo -A \
       env ${DREL:+VMCTX_DEST_RELEASE=$DREL} \
       setsid nohup /home/biwu/lan-boot/vmctx/build/vmhome $PORT -v $PROG $* \
       > /tmp/xr.guest.out 2> /tmp/xr.home.log < /dev/null &" >/dev/null 2>&1
# Wait for it to be listening rather than sleeping a guess: a source that has
# not bound yet makes the destination fail to connect and the run reports a
# network problem that is really a race in this script.
for i in $(seq 1 40); do
	$SSRC "ss -ltn 2>/dev/null | grep -q ':$PORT '" && break
	sleep 0.25
done

timeout $((LIMIT + 20)) $SDST \
	"timeout -s TERM $LIMIT /usr/local/bin/vmremote $SRC:$PORT --net" \
	> "$OUT/remote.log" 2>&1
rc=$?

sleep 1
$SSRC "SUDO_PASS=biwu SUDO_ASKPASS=\$HOME/askpass.sh sudo -A pkill -x vmhome" 2>/dev/null || true
$SSRC "cat /tmp/xr.guest.out" > "$OUT/guest.out" 2>/dev/null
$SSRC "cat /tmp/xr.home.log"  > "$OUT/home.log"  2>/dev/null

echo "xrun $T: vmremote exit $rc; guest.out:"
cat "$OUT/guest.out"
[ -s "$OUT/guest.out" ] || echo "  (empty -- see $OUT/home.log and $OUT/remote.log)"
exit $rc
