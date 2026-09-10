#!/bin/bash
# browser-run.sh — run a browser as a vmctx guest on this machine's loopback,
# from a *clean* baseline, and capture the oracle's address-space map so that a
# guest fault RIP can be turned back into a function name.
#
# The clean baseline is not optional. A stray native browser from an earlier
# test, reparented to init, once owned the window I was about to call a
# success; and killing vmhome detaches ptrace from the oracle, which then
# resumes and renders natively — a false positive generator built into the
# setup. So: no browser processes, a fresh Xvfb with no windows, empty dmesg.
set -u
PROG=${PROG:-/usr/bin/netsurf}
# The browser's own arguments. links2 needs -g or it draws in the terminal and
# renders nothing on the display, which reads exactly like a failure to render.
ARGS=${ARGS:-}
URL=${URL:-http://127.0.0.1:8899/page.html}
OUT=${OUT:-/tmp/browser}
DISP=${DISP:-:99}

rm -rf "$OUT"; mkdir -p "$OUT"

# SIGKILL, not SIGTERM: a leftover oracle is ptrace-stopped and will not act
# on a catchable signal, so it survives and its window and its faults get
# attributed to the next run.
pkill -9 -x vmhome 2>/dev/null; pkill -9 -x vmremote-local 2>/dev/null
pkill -9 -x netsurf 2>/dev/null; pkill -9 -x dillo 2>/dev/null
pkill -9 -x Xvfb 2>/dev/null; pkill -9 -f 'http.server 8899' 2>/dev/null
# An X server killed with -9 never removes its lock file, and Ubuntu does not
# clear /tmp at boot — so the next server to want that display finds it held by
# a pid that died days ago. Left alone this is exactly how the desktop wedged:
# two locks owned by another user, which gdm could not remove, and Xwayland
# giving up after fifty refusals.
rm -f /tmp/.X99-lock /tmp/.X11-unix/X99
sleep 2

( cd /home/biwu/web && exec python3 -m http.server 8899 --bind 127.0.0.1 ) > $OUT/http.log 2>&1 &
Xvfb "$DISP" -screen 0 1280x1024x24 -extension MIT-SHM >/dev/null 2>&1 &
# Wait for the server to ANSWER, not for a timer: on a fresh boot 1.5s was
# not enough, the guest's first X write took EPIPE, and two runs were read
# as a vmctx regression before the control (native netsurf, same boot) put
# it on the harness.
for _ in $(seq 40); do
	DISPLAY=$DISP xwininfo -root >/dev/null 2>&1 && break
	sleep 0.5
done

# Prove the baseline rather than assume it.
echo "=== baseline ===" | tee "$OUT/baseline.txt"
echo "browser processes: $(pgrep -x netsurf | wc -l) netsurf, $(pgrep -x dillo | wc -l) dillo" | tee -a "$OUT/baseline.txt"
DISPLAY=$DISP xwininfo -root -children 2>&1 | tee -a "$OUT/baseline.txt"
# A dirty baseline makes every result meaningless, so stop rather than report one.
if [ "$(pgrep -x "$(basename "$PROG")" | wc -l)" != 0 ]; then
	echo "REFUSING TO RUN: $(basename "$PROG") is already running" >&2; exit 1
fi
# A screenshot of the EMPTY root, because the colour count at the end is only
# meaningful against one: a bare Xvfb root is not one flat colour (767 distinct
# 4-byte words on a 1280x1024 root here), so the number printed on its own reads
# as a drawn page and is not.
DISPLAY=$DISP xwd -root -silent > "$OUT/screen.before.xwd" 2>/dev/null
dmesg -C

# Follow the oracle's map for as long as it exists: libraries opened later
# (fontconfig dlopens a lot) are missing from a map taken only at startup.
(
	while :; do
		p=$(pgrep -x "$(basename "$PROG")" | while read -r q; do
			[ "$(awk '{print $3}' /proc/$q/stat 2>/dev/null)" = t ] && echo "$q"
		done | head -1)
		if [ -n "${p:-}" ] && [ -r "/proc/$p/maps" ]; then
			cp "/proc/$p/maps" "$OUT/oracle.maps.new" 2>/dev/null &&
				mv "$OUT/oracle.maps.new" "$OUT/oracle.maps"
			echo "$p" > "$OUT/oracle.pid"
		fi
		sleep 1
	done
) &
WATCH=$!

# Sample the DISPLAY while the browser is alive.
#
# This used to be read once, after the run. By then the browser has been killed
# and its window is gone with it, so the check could only ever report "0
# children" -- including for a run that had drawn a page and held it. A harness
# that cannot observe the thing it is measuring reports failure for success,
# and did: links2 -g ran the full 80 seconds, 625 forwarded syscalls, never
# died, and was written up as not rendering.
#
# So: sample every two seconds, keep the sample with the most children, and
# take a screenshot at the same moment. The peak is what matters -- a window
# that existed at any instant existed.
(
	best=0
	while :; do
		w=$(DISPLAY=$DISP xwininfo -root -children 2>/dev/null)
		n=$(printf '%s\n' "$w" | grep -cE '^ +0x')
		if [ "${n:-0}" -gt "$best" ]; then
			best=$n
			printf '%s\n' "$w" > "$OUT/windows.peak"
			echo "$n" > "$OUT/windows.peak.count"
		fi
		#
		# The screenshot is taken on ">=", not on ">", and that is the
		# whole difference between measuring a page and measuring a
		# window frame.
		#
		# A browser creates its window and paints it some time later, so
		# the child count peaks at the FIRST instant there is anything on
		# the display -- and it never rises again. Screenshotting only
		# when the count INCREASES therefore froze the picture at the
		# moment the window appeared, before a single glyph was drawn:
		# "1230x949 window" and "+0 colours" in the same report, which
		# reads as a browser that mapped a window and rendered nothing.
		# Taking it whenever the display is at least as populated keeps
		# the LATEST such sample, so the colour count is read from the
		# page rather than from its frame.
		if [ "${n:-0}" -ge "$best" ] && [ "${n:-0}" -gt 0 ]; then
			DISPLAY=$DISP xwd -root -silent > "$OUT/screen.xwd.new" 2>/dev/null &&
				mv "$OUT/screen.xwd.new" "$OUT/screen.xwd"
		fi
		sleep 2
	done
) &
WINWATCH=$!

cd /home/biwu/lan-boot/vmctx
export KO=/home/biwu/vmctx-7.0.14.ko DISPLAY=$DISP HOME=${RUNHOME:-/home/biwu}
# shellcheck disable=SC2086 -- $ARGS is a word list on purpose
timeout "${LIMIT:-180}" ./local-here.sh "$PROG" $ARGS "$URL" > "$OUT/run.log" 2>&1
echo "=== local-here exit $? ===" >> "$OUT/run.log"

kill $WATCH $WINWATCH 2>/dev/null
dmesg | grep -a vmctx > "$OUT/dmesg.txt"
DISPLAY=$DISP xwininfo -root -children > "$OUT/windows.after" 2>&1
echo "=== oracle map lines: $( { wc -l < "$OUT/oracle.maps"; } 2>/dev/null || echo 0) ==="
#
# What the BROWSER said, which is not what the monitor said.
#
# A forwarded write(2) to fd 2 is performed by the source's context task, whose
# fd 2 is vmhome's -- so the program's own complaints land in home.log among
# thousands of lines of vmhome diagnostics. netsurf's exit status 1 was called
# unexplained for a whole round while "X connection to :99 broken" sat in there.
# Everything vmhome prints is prefixed; whatever is not is the program talking.
if [ -s /tmp/localrun/guest.err ]; then
	cp /tmp/localrun/guest.err "$OUT/guest.err" 2>/dev/null
	echo "=== what the program itself said ==="
	sed 's/^/   /' "$OUT/guest.err"
else
	echo "=== what the program itself said: (nothing) ==="
fi
echo "=== windows, at their peak WHILE it ran: $(cat "$OUT/windows.peak.count" 2>/dev/null || echo 0) ==="
grep -E '^ +0x' "$OUT/windows.peak" 2>/dev/null || echo "   (none ever appeared)"
# Distinct colours in the root window, which is the difference between a window
# that exists and a window with a page drawn in it. An empty Xvfb root is one
# colour; links2 showing this page was measured at 253.
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
echo "=== distinct colours on the root: $(colours "$OUT/screen.before.xwd") before, $(colours "$OUT/screen.xwd") at the peak ==="
echo "    (the first is the empty display; only the difference means anything)"
echo "=== faults ==="; grep -a 'unrecoverable' "$OUT/dmesg.txt" || echo "(none)"
