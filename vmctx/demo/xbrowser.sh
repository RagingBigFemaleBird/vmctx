#!/bin/bash
# xbrowser.sh -- a browser as a vmctx guest ACROSS the two boxes, with its
# window where a person can use it: the program, its files, its memory and
# its X connection are the SOURCE's (10.0.0.229); its instructions execute on
# the Intel DESTINATION (10.0.0.30). Modelled on ../xrun.sh, plus the
# program's environment and the display choice.
#
#   DISP=:0  EXTRA_ENV="XAUTHORITY=/run/user/1000/.mutter-Xwaylandauth.XXXX" \
#       PROG=/usr/bin/links2 ARGS="-g http://127.0.0.1:8899/page.html" \
#       LIMIT=43200 ./xbrowser.sh          # on the AMD box's logged-in screen
#   DISP=:99 PROG=/usr/bin/netsurf ./xbrowser.sh   # Xvfb + x11vnc on :5900
#
# xprep.sh must be on the source as /home/biwu/xprep.sh (root). It kills ONLY
# the vmhome holding this run's port: a global pkill here ended a demo a
# person was using (session 47). Cross-machine netsurf also needs
# EXTRA_ENV="OPENSSL_ia32cap=:~0x20000000": the first image's constructors
# run on the source (HANDOFF §0b), so OpenSSL probes the Ryzen's SHA-NI and
# the i7-7700K guest would execute sha1msg1.
set -u
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
SRC=10.0.0.229; DST=10.0.0.30
PORT=${PORT:-9973}
LIMIT=${LIMIT:-1800}
OUT=${OUT:-/home/biwu/xbrowser}
PROG=${PROG:-/usr/bin/netsurf}
ARGS=${ARGS:-http://127.0.0.1:8899/page.html}
DISP=${DISP:-:99}
BUS=/tmp/vmctx-xdemo-dbus
ENV="${EXTRA_ENV:-} GDK_BACKEND=x11 HOME=/root DISPLAY=$DISP DBUS_SESSION_BUS_ADDRESS=unix:path=$BUS PATH=/home/biwu/lan-boot/vmctx/fakebwrap:/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin"
K="-i $ROOT/config/id_vmctx -o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null -o BatchMode=yes -o ConnectTimeout=8 -o LogLevel=ERROR"
SSRC="ssh $K biwu@$SRC"; SDST="ssh $K root@$DST"
SUDO='SUDO_PASS=biwu SUDO_ASKPASS=$HOME/askpass.sh sudo -A'

$SSRC "$SUDO /home/biwu/xprep.sh $OUT $PORT $DISP" || exit 1
$SDST "pkill -9 -x vmremote" 2>/dev/null || true
sleep 1
$SSRC "$SUDO env $ENV setsid nohup /home/biwu/lan-boot/vmctx/build/vmhome $PORT -v $PROG $ARGS > $OUT/guest.out 2> $OUT/home.log < /dev/null &" >/dev/null 2>&1
for i in $(seq 1 40); do $SSRC "ss -ltn 2>/dev/null | grep -q ':$PORT '" && break; sleep 0.25; done
echo "=== source listening on $PORT; destination starts $(date +%T)"
timeout $((LIMIT + 20)) $SDST "timeout -s TERM $LIMIT /usr/local/bin/vmremote $SRC:$PORT --net" > "${LOGDIR:-/tmp}/$(basename $OUT).remote.log" 2>&1
rc=$?
echo "=== vmremote exit $rc $(date +%T)"
$SSRC "for p in \$(ss -ltnp 2>/dev/null | grep \":$PORT \" | grep -o \"pid=[0-9]*\" | cut -d= -f2); do $SUDO kill \$p; done; $SUDO sh -c 'old=\$(lsof -t $BUS 2>/dev/null); [ -n \"\$old\" ] && kill \$old; rm -f $BUS'" 2>/dev/null || true
echo XBROWSER-DONE
