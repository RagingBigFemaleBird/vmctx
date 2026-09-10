#!/bin/bash
# lbrowser.sh <tag> <prog> [args...] -- a browser as a LOOPBACK guest on this
# box (.229) for experiments, WITHOUT the global kills browser-run.sh makes
# (its pkill -9 -x vmhome ends every other run on the box, including a demo
# a person is using). Shared mode, explicit port, Xvfb :98 of its own, the
# demo session bus, the fakebwrap shim. Runs as root.
#   sudo lbrowser.sh ns1 /usr/bin/netsurf http://127.0.0.1:8899/page.html
set -u
TAG=${1:?tag}; shift
PROG=${1:?prog}; shift
OUT=/home/biwu/lb.$TAG
PORT=${PORT:-28910}
LIMIT=${LIMIT:-400}
DISP=:98
BUS=/tmp/vmctx-lb-dbus
cd /home/biwu/lan-boot/vmctx || exit 1
rm -rf "$OUT"; mkdir -p "$OUT"
# display of its own (kill only ours: by the socket, not by name)
old=$(lsof -t /tmp/.X11-unix/X98 2>/dev/null); [ -n "$old" ] && kill -9 $old 2>/dev/null
rm -f /tmp/.X11-unix/X98 /tmp/.X98-lock
Xvfb "$DISP" -screen 0 1280x1024x24 -extension MIT-SHM > "$OUT/xvfb.log" 2>&1 &
for i in $(seq 1 40); do DISPLAY=$DISP xwininfo -root >/dev/null 2>&1 && break; sleep 0.25; done
old=$(lsof -t "$BUS" 2>/dev/null); [ -n "$old" ] && kill $old 2>/dev/null
rm -f "$BUS"
dbus-daemon --session --address="unix:path=$BUS" --fork || { echo "no bus" >&2; exit 1; }
pgrep -f 'http.server 8899' >/dev/null || \
	( cd /home/biwu/web && setsid nohup python3 -m http.server 8899 --bind 127.0.0.1 > /dev/null 2>&1 < /dev/null & )
DISPLAY=$DISP xwd -root -silent > "$OUT/screen.before.xwd" 2>/dev/null
# window + screenshot watch
( while true; do
	DISPLAY=$DISP xwininfo -root -children 2>/dev/null | grep -E '^ +0x' > "$OUT/windows.now" 2>/dev/null
	n=$(grep -c . "$OUT/windows.now" 2>/dev/null || echo 0)
	if [ "$n" -gt "$(cat "$OUT/windows.peak.count" 2>/dev/null || echo 0)" ]; then
		cp "$OUT/windows.now" "$OUT/windows.peak"; echo "$n" > "$OUT/windows.peak.count"; fi
	DISPLAY=$DISP xwd -root -silent > "$OUT/screen.xwd.new" 2>/dev/null && mv "$OUT/screen.xwd.new" "$OUT/screen.xwd"
	sleep 3
  done ) &
WATCH=$!
export KO=/home/biwu/vmctx-7.0.14.ko DISPLAY=$DISP HOME=/root
export DBUS_SESSION_BUS_ADDRESS="unix:path=$BUS" PATH="/home/biwu/lan-boot/vmctx/fakebwrap:$PATH"
export GDK_BACKEND=x11 VMCTX_SHARED=1 PORT
OUT="$OUT" VMR_LIMIT=$LIMIT ./local-here.sh "$PROG" "$@" > "$OUT/run.log" 2>&1
echo "=== local-here exit $? ===" >> "$OUT/run.log"
kill $WATCH 2>/dev/null
dmesg | grep -a vmctx | tail -200 > "$OUT/dmesg.txt"
cp /tmp/localrun/guest.err "$OUT/guest.err" 2>/dev/null
old=$(lsof -t "$BUS" 2>/dev/null); [ -n "$old" ] && kill $old 2>/dev/null; rm -f "$BUS"
old=$(lsof -t /tmp/.X11-unix/X98 2>/dev/null); [ -n "$old" ] && kill -9 $old 2>/dev/null
echo "=== $TAG done: windows peak $(cat "$OUT/windows.peak.count" 2>/dev/null || echo 0) ==="
cat "$OUT/windows.peak" 2>/dev/null
grep -a 'unrecoverable' "$OUT/dmesg.txt" | head -3
echo LB-DONE > "$OUT/done"
