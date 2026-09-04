#!/bin/bash
# native.sh — the control (run on the machine with the browser, e.g. 10.0.0.229)
# The control: netsurf on this machine, same URL, same Xvfb setup,
# no vmctx anywhere. Without it there is no way to tell a vmctx defect from
# something netsurf does on this box regardless.
pkill -9 -x netsurf; pkill -9 -x Xvfb; pkill -9 -x vmhome; pkill -9 -x vmremote-local
pkill -9 -x python3
rm -f /tmp/.X98-lock /tmp/.X11-unix/X98
sleep 2
( cd /home/biwu/web && exec python3 -m http.server 8899 --bind 127.0.0.1 ) >/tmp/nhttp.log 2>&1 &
Xvfb :98 -screen 0 1280x1024x24 -extension MIT-SHM >/dev/null 2>&1 &
sleep 3
DISPLAY=:98 /usr/bin/netsurf http://127.0.0.1:8899/page.html >/tmp/native.out 2>&1 &
NS=$!
sleep 25
echo "=== windows ==="; DISPLAY=:98 xwininfo -root -children 2>&1 | tail -4
echo "=== http hits ==="; cat /tmp/nhttp.log
echo "=== netsurf output ==="; head -8 /tmp/native.out
kill -9 $NS 2>/dev/null
