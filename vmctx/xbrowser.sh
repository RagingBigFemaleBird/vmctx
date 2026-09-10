#!/bin/bash
# xbrowser.sh — a browser as a guest, with the two halves on two machines.
#
#   ./xbrowser.sh                 # links2 -g, the working graphical baseline
#   PROG=/usr/bin/netsurf ./xbrowser.sh
#
# This is browser-run.sh's clean-baseline discipline applied to the
# configuration the objective is actually about: the SOURCE (10.0.0.229) owns
# the browser, its files, its fonts and its X connection, and the DESTINATION
# (10.0.0.30) executes it and owns nothing.
#
# Where the window appears follows from that and is not a choice. Every syscall
# the browser makes is forwarded to the source, so its connect(2) to the X
# server is made on the source and its display is the source's -- the
# destination never learns that an X server exists. So Xvfb runs on .229 and
# the proof of rendering is read there.
#
# Run this from the server (home); it drives both boxes over ssh.
set -u
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "$HERE/.." && pwd)"
SRC=${SRC:-10.0.0.229}
DST=${DST:-10.0.0.30}
PORT=${PORT:-9961}
PROG=${PROG:-/usr/bin/links2}
ARGS=${ARGS:--g}
URL=${URL:-http://127.0.0.1:8899/page.html}
DISP=${DISP:-:99}
LIMIT=${LIMIT:-120}
OUT=${OUT:-/tmp/xbrowser}
K="-i $ROOT/config/id_vmctx -o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null \
   -o BatchMode=yes -o ConnectTimeout=8 -o LogLevel=ERROR"
SSRC="ssh $K biwu@$SRC"
SDST="ssh $K root@$DST"
SUDO="SUDO_PASS=biwu SUDO_ASKPASS=\$HOME/askpass.sh sudo -A"

mkdir -p "$OUT"; rm -f "$OUT"/*
B=$(basename "$PROG")

echo "== baseline on the source ($SRC)"
# SIGKILL, not SIGTERM: a leftover oracle is ptrace-stopped and will not act on
# a catchable signal, so it survives, and its window and its faults are then
# attributed to this run. browser-run.sh learned this the expensive way.
#
# NEVER `pkill -f`. It matches on the full command line, and the command line it
# is running under -- the ssh command -- contains the pattern, so it kills its
# own session. The symptom is not an error: the ssh simply never returns, and
# this script sat at this line twice for as long as it was left. (It is also
# how this edit was first lost: a `pkill -f xbrowser.sh` run here killed the
# shell that was applying it.) Exact names only, and the one process with no
# distinctive name is killed by a pid it was made to write down.
$SSRC "$SUDO pkill -9 -x vmhome; $SUDO pkill -9 -x $B; $SUDO pkill -9 -x Xvfb;
       [ -f /tmp/xb.http.pid ] && $SUDO kill -9 \$(cat /tmp/xb.http.pid);
       $SUDO rm -f /tmp/.X99-lock /tmp/.X11-unix/X99 /tmp/xb.http.pid; true" >/dev/null 2>&1
$SDST "pkill -9 -x vmremote" >/dev/null 2>&1
sleep 2

# One daemon per ssh, each with all three descriptors redirected to a FILE.
#
# Two of them in one ssh does not return: the session channel stays open while
# anything in the remote process group still holds it, so the script sat there
# for seven minutes having started nothing it could see. xrun.sh backgrounds
# exactly one command per connection for this reason; this follows it.
#
# Both under sudo, and that is not tidiness. browser-run.sh runs as root and
# leaves /tmp/.X11-unix/X99 owned by root; an Xvfb started here as biwu then
# finds the display "already active", exits at once, and every later xwininfo
# reports no window -- a missing X server reads exactly like a browser that
# would not draw.
$SSRC "$SUDO rm -f /tmp/.X99-lock /tmp/.X11-unix/X99" >/dev/null 2>&1
$SSRC "cd /home/biwu/web && $SUDO setsid nohup python3 -m http.server 8899 \
       --bind 127.0.0.1 > /tmp/xb.http.log 2>&1 < /dev/null & echo \$! > /tmp/xb.http.pid" >/dev/null 2>&1
$SSRC "$SUDO setsid nohup Xvfb $DISP -screen 0 1280x1024x24 -extension MIT-SHM \
       > /tmp/xb.xvfb.log 2>&1 < /dev/null &" >/dev/null 2>&1
sleep 3

# Prove the X server is there before blaming the browser for not drawing on it.
if ! $SSRC "DISPLAY=$DISP xdpyinfo >/dev/null 2>&1"; then
	echo "REFUSING TO RUN: no X server on $SRC$DISP" >&2
	$SSRC "cat /tmp/xb.xvfb.log" >&2
	exit 1
fi
echo "   X server on $SRC$DISP is up"

# Prove the baseline rather than assume it: a browser already running owns a
# window this run would otherwise be credited with.
n=$($SSRC "pgrep -x $B | wc -l" 2>/dev/null)
if [ "${n:-0}" != 0 ]; then
	echo "REFUSING TO RUN: $B is already running on $SRC ($n)" >&2; exit 1
fi
# Count CHILDREN, not lines. "4 lines" is four lines of xwininfo's header on an
# empty root, which reads as a display that already has windows on it -- and a
# window already there is one this run would otherwise be credited with.
$SSRC "DISPLAY=$DISP xwininfo -root -children" > "$OUT/windows.before" 2>&1
# And a screenshot of the EMPTY root, because the colour count below is only
# meaningful against one. A bare Xvfb root is not one flat colour -- this pair
# of boxes measures 767 distinct 4-byte words on an empty 1280x1024 root -- so
# "767 colours" printed on its own reads as a drawn page and is not.
$SSRC "DISPLAY=$DISP xwd -root -silent > /tmp/xb.base.xwd 2>/dev/null" >/dev/null 2>&1
$SSRC "cat /tmp/xb.base.xwd" > "$OUT/screen.before.xwd" 2>/dev/null
before=$(grep -cE '^ +0x' "$OUT/windows.before" 2>/dev/null || true)
echo "   windows on the display before the run: ${before:-0}"
if [ "${before:-0}" != 0 ]; then
	echo "REFUSING TO RUN: $SRC$DISP already has ${before} window(s) on it" >&2
	cat "$OUT/windows.before" >&2
	exit 1
fi
$SSRC "$SUDO dmesg -C" >/dev/null 2>&1

echo "== source starts the browser under vmhome; destination executes it"
# sudo -E is rejected on that box, so the environment goes through env(1).
$SSRC "$SUDO setsid nohup env DISPLAY=$DISP HOME=/home/biwu \
       /home/biwu/lan-boot/vmctx/build/vmhome $PORT -v $PROG $ARGS '$URL' \
       > /tmp/xb.guest.out 2> /tmp/xb.home.log < /dev/null &" >/dev/null 2>&1
for i in $(seq 1 40); do
	$SSRC "ss -ltn 2>/dev/null | grep -q ':$PORT '" && break
	sleep 0.25
done

timeout $((LIMIT + 30)) $SDST \
	"timeout -s TERM $LIMIT /usr/local/bin/vmremote $SRC:$PORT --net" \
	> "$OUT/remote.log" 2>&1 &
VMR=$!

# Look at the display while it is still up: the window is gone the moment the
# browser is killed, so a check after the run can only ever report zero.
sleep $((LIMIT / 2))
$SSRC "DISPLAY=$DISP xwininfo -root -children" > "$OUT/windows.during" 2>&1
$SSRC "DISPLAY=$DISP xwd -root -silent > /tmp/xb.xwd 2>/dev/null; echo \$?" >/dev/null 2>&1
$SSRC "cat /tmp/xb.xwd" > "$OUT/screen.xwd" 2>/dev/null

wait $VMR; rc=$?

sleep 1
$SSRC "$SUDO pkill -x vmhome" >/dev/null 2>&1
$SSRC "cat /tmp/xb.guest.out" > "$OUT/guest.out" 2>/dev/null
$SSRC "cat /tmp/xb.home.log"  > "$OUT/home.log"  2>/dev/null
$SSRC "$SUDO dmesg | grep -a vmctx" > "$OUT/dmesg.txt" 2>/dev/null

echo
echo "== vmremote exit $rc"
echo "== windows on the source WHILE it ran:"
grep -E '^ +0x' "$OUT/windows.during" || echo "   (none -- no window was mapped)"
colours() {
	python3 -c '
import sys,struct
d=open(sys.argv[1],"rb").read()
hl=struct.unpack(">I",d[:4])[0]
px=d[hl:]
n=len(px)//4*4
print(len({px[i:i+4] for i in range(0,n,4)}))
' "$1" 2>/dev/null || echo "n/a"
}
echo "== distinct colours on the root: $(colours "$OUT/screen.before.xwd") before, $(colours "$OUT/screen.xwd") while it ran"
echo "   (the first is the empty display; only the difference means anything)"
echo "== unrecoverable faults:"; grep -a 'unrecoverable' "$OUT/dmesg.txt" || echo "   (none)"
echo "== artefacts in $OUT"
