#!/bin/bash
# xprep.sh -- runs as root ON THE SOURCE (.229) before a cross-machine browser
# run: a fresh Xvfb :99 with x11vnc on it (so a person can drive the window
# from anywhere on the LAN), the demo's own session bus (GIO must never
# autolaunch dbus-launch -- the parked wall), the harness HTTP server, and a
# clean output directory. Mirrors browser-run.sh + browser-demo.sh.
set -u
OUT=${1:-/home/biwu/xbrowser}
PORT=${2:-9973}
DISP=${3:-:99}
BUS=/tmp/vmctx-xdemo-dbus
# Kill ONLY the vmhome that holds this run's port -- never every vmhome: other
# demos and experiments share the box (a global pkill here killed the user's
# running links2 demo once).
for p in $(ss -ltnp 2>/dev/null | awk -v P=":$PORT" '$4 ~ P"$" {print $0}' | grep -o 'pid=[0-9]*' | cut -d= -f2); do kill -9 $p 2>/dev/null; done
old=$(lsof -t "$BUS" 2>/dev/null); [ -n "$old" ] && kill $old 2>/dev/null
rm -f "$BUS"
rm -rf "$OUT" /home/biwu/xprof; mkdir -p "$OUT" /home/biwu/xprof; chown biwu:biwu "$OUT"
sleep 0.5
if [ "$DISP" = ":99" ]; then
# ONLY this display's server and its VNC: another Xvfb (:98, the loopback
# demo) is somebody else's -- a global pkill here took the user's netsurf
# down (session 47).
old=$(lsof -t /tmp/.X11-unix/X99 2>/dev/null); [ -n "$old" ] && kill -9 $old 2>/dev/null
for p in $(pgrep -f "x11vnc -display :99"); do kill -9 $p 2>/dev/null; done
rm -f /tmp/.X99-lock /tmp/.X11-unix/X99
Xvfb "$DISP" -screen 0 1280x1024x24 -extension MIT-SHM > "$OUT/xvfb.log" 2>&1 &
for i in $(seq 1 40); do DISPLAY=$DISP xwininfo -root >/dev/null 2>&1 && break; sleep 0.25; done
DISPLAY=$DISP xwininfo -root >/dev/null 2>&1 || { echo "xprep: Xvfb did not come up" >&2; exit 1; }
setsid nohup x11vnc -display "$DISP" -forever -shared -noshm -passwd vmctx -rfbport 5900 -noxdamage > "$OUT/x11vnc.log" 2>&1 < /dev/null &
fi
dbus-daemon --session --address="unix:path=$BUS" --fork --print-address > "$OUT/dbus.addr" 2> "$OUT/dbus.err" || { echo "xprep: dbus-daemon failed: $(cat $OUT/dbus.err)" >&2; exit 1; }
[ -S "$BUS" ] || { echo "xprep: bus socket missing: $(cat $OUT/dbus.err)" >&2; exit 1; }
pgrep -f 'http.server 8899' >/dev/null || \
	( cd /home/biwu/web && setsid nohup python3 -m http.server 8899 --bind 127.0.0.1 > "$OUT/http.log" 2>&1 < /dev/null & )
[ "$DISP" = ":99" ] && DISPLAY=$DISP xwd -root -silent > "$OUT/screen.before.xwd" 2>/dev/null
sleep 1
echo "xprep: display $DISP, x11vnc $(pgrep -x x11vnc | head -1 || echo -), bus $BUS, http 8899, port $PORT cleared"
