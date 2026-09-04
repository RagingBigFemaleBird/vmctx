#!/bin/bash
# browser-demo.sh -- the NO-SANDBOX browser demo, with a verdict.
#
# Runs a browser as a vmctx guest against the harness's own HTTP server and
# GRADES the run: GREEN means the browser put up a window, DREW a page in it
# (positive distinct-colour delta over the empty root), and no unrecoverable
# fault fired while it ran. Everything else is browser-run.sh unchanged
# (clean baseline, Xvfb, window watch, colour count, dmesg).
#
# The default program is links2 -g: a real graphical browser with NO sandbox
# and no D-Bus, the demo that is green by construction on both transports
# (rendering baseline +253 colours since session 6; re-validated session 41
# and here). PROG/ARGS select others:
#
#   sudo ./browser-demo.sh                                  # links2 -g
#   sudo PROG=/usr/bin/netsurf ARGS= ./browser-demo.sh      # netsurf
#   sudo PROG=/usr/bin/dillo  ARGS= ./browser-demo.sh       # dillo
#
# For the GTK/glycin path (netsurf) the two environment tarpits are removed
# OUTSIDE the guest rather than fought inside it -- both recorded in
# HANDOFF.md §42:
#
#  1. GTK's D-Bus AUTOLAUNCH: with no DBUS_SESSION_BUS_ADDRESS, every GDBus
#     consumer forks dbus-launch -- the parked next wall. A REAL session bus
#     is started NATIVELY (the daemon is not a guest; only the browser is)
#     and the guest gets its address; its connect(2) is an ordinary
#     forwarded syscall. Measured working: zero dbus-launch forks in the
#     guest with the address set.
#  2. glycin's bwrap image-loader sandbox: fakebwrap/ first in PATH execs
#     the loader directly (GlySandboxSelector::NotSandboxed from outside
#     the library -- glycin 2.1.1 has no env override, session 41f).
#
# Known state of the non-default programs (session 42b, recorded not
# pursued): netsurf renders (5488 colours at LIMIT=400) but intermittently
# parks behind a page STUCK IN CLAIM/TRANSIT at the source ("claiming for
# 2001 faults in a row"); dillo dies at 0x8 inside ld.so while linking.
set -u
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
BUS=/tmp/vmctx-demo-dbus
export PROG=${PROG:-/usr/bin/links2}
export ARGS=${ARGS--g}
OUT=${OUT:-/tmp/browser-demo}

[ "$(id -u)" = 0 ] || { echo "must run as root (browser-run.sh does)" >&2; exit 2; }

# A fresh session bus of the demo's own. Killed by socket holder (lsof), not
# by pkill -f: a -f pattern that appears in a caller's own command line
# kills the caller (measured, again, setting this up).
old=$(lsof -t "$BUS" 2>/dev/null)
[ -n "$old" ] && kill $old 2>/dev/null
rm -f "$BUS"
dbus-daemon --session --address="unix:path=$BUS" --fork || {
	echo "browser-demo: dbus-daemon did not start" >&2; exit 1; }
export DBUS_SESSION_BUS_ADDRESS="unix:path=$BUS"
export PATH="$HERE/fakebwrap:$PATH"

LIMIT=${LIMIT:-180} OUT="$OUT" "$HERE/browser-run.sh"
rc=$?

old=$(lsof -t "$BUS" 2>/dev/null)
[ -n "$old" ] && kill $old 2>/dev/null
rm -f "$BUS"

# ---- the verdict, from the run's own artefacts -----------------------------
colours() {
	python3 -c '
import sys,struct
d=open(sys.argv[1],"rb").read()
hl=struct.unpack(">I",d[:4])[0]
px=d[hl:]
n=len(px)//4*4
print(len({px[i:i+4] for i in range(0,n,4)}))
' "$1" 2>/dev/null || echo 0
}
before=$(colours "$OUT/screen.before.xwd")
peak=$(colours "$OUT/screen.xwd")
wins=$(cat "$OUT/windows.peak.count" 2>/dev/null || echo 0)
# grep -c prints its 0 AND exits 1 on no match, so `|| echo 0` here produced
# "0\n0" and failed the comparison -- the demo graded itself RED over its own
# plumbing on a run with +253 colours and no faults.
faults=$(grep -ac 'unrecoverable' "$OUT/dmesg.txt" 2>/dev/null)
faults=${faults:-0}
echo
if [ "$wins" -gt 0 ] && [ "$peak" -gt "$before" ] && [ "$faults" = 0 ]; then
	echo "=== DEMO GREEN: $PROG drew a page ($((peak - before)) colours over" \
	     "the empty root, $wins window(s), 0 unrecoverable faults) ==="
	exit 0
fi
echo "=== DEMO RED: windows=$wins colours=$before->$peak" \
     "unrecoverable-faults=$faults (see $OUT) ==="
exit 1
