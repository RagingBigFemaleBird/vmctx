#!/bin/bash
# remote-run.sh — run a program that exists only on THIS machine (home) on the
# netboot target, with its syscalls forwarded back here.
#
#   ./remote-run.sh /usr/bin/wc /etc/hostname
#
# Everything is verified end to end each time, because stale binaries on either
# side have produced false results repeatedly: both vmhome and vmremote are
# checksummed after the copy, and the previous vmhome is killed before the new
# one starts (it runs as root and survives an unprivileged pkill).
set -u
cd "$(dirname "$0")/.."
TARGET=${TARGET:-10.0.0.229}
PORT=${PORT:-9999}
# Who to log in as, and what it takes to be root once there.
#
# The netboot target runs as root; a machine with its own installed OS does
# not, and refusing that one is not a property of the model -- it is a hard
# coded username. So the user is a variable, and the commands that need
# privilege say so rather than assuming they already have it. Without this the
# script simply could not drive 10.0.0.229 at all ("root@...: Permission
# denied"), which is a lot of machine to lose to one word.
#
# TARGET_USER overrides; otherwise root for the netboot target and the owner
# of the installed machine for anything else.
case "${TARGET_USER:-}" in
"") case "$TARGET" in
    10.0.0.30) TARGET_USER=root ;;
    *)         TARGET_USER=biwu ;;
    esac ;;
esac
SSH="ssh -i config/id_vmctx -o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null -o ConnectTimeout=8 -o LogLevel=ERROR $TARGET_USER@$TARGET"
# Privilege on the target: nothing when already root, sudo otherwise. -A so an
# unattended run answers from SUDO_ASKPASS rather than blocking on a terminal
# that is not there; the target's own askpass is named by TARGET_ASKPASS.
if [ "$TARGET_USER" = root ]; then
	RSUDO=""
else
	RSUDO="sudo -A"
fi
TARGET_ASKPASS=${TARGET_ASKPASS:-\$HOME/askpass.sh}
# The target's askpass prints $SUDO_PASS, so that has to exist over there. It
# comes from this environment and is never written here: a password in a file
# under version control is a password published. Export TARGET_SUDO_PASS in the
# calling shell for an unattended run, or leave it unset and let sudo ask.
RSUDO_ENV="SUDO_ASKPASS=$TARGET_ASKPASS"
[ -n "${TARGET_SUDO_PASS:-}" ] && RSUDO_ENV="$RSUDO_ENV SUDO_PASS='$TARGET_SUDO_PASS'"
HOME_IP=${HOME_IP:-$(ip -4 route get "$TARGET" | sed -n 's/.* src \([0-9.]*\).*/\1/p')}
LOG=/tmp/claude-1000/-home-desktop-lan-boot/remote-run
mkdir -p "$LOG"

if [ $# -lt 1 ]; then echo "usage: $0 <program-on-home> [args...]" >&2; exit 2; fi

# --- refresh the target's copy of the module and tools -----------------------
$SSH "export $RSUDO_ENV; $RSUDO pkill -9 vmremote; sleep 1; $RSUDO rmmod vmctx 2>/dev/null; true"
# The target has no sftp-server, so scp is not available: pipe over ssh.
# A target whose kernel is not the one built here has a module of its own, and
# the one built here will not load on it ("Invalid module format"). TARGET_KO
# names that module, on the target; unset means the target runs this kernel and
# gets this module.
KO_ON_TARGET=/tmp/vmctx.ko
VMREMOTE_ON_TARGET=/tmp/vmremote
COPY_LIST="vmctx/kernel/vmctx.ko vmctx/build/vmremote vmctx/build/dumpvm"
if [ -n "${TARGET_KO:-}" ]; then
	KO_ON_TARGET=$TARGET_KO
	COPY_LIST="vmctx/build/vmremote vmctx/build/dumpvm"
fi
# ...and its own vmremote, for the same reason one step further on. The
# vmctx_ctl syscall has a different number in a different kernel, so a vmremote
# built here calls the wrong one on a target that is not running this kernel:
# "could not attach: Function not implemented", with the module loaded and
# everything else looking healthy. TARGET_VMREMOTE names the target's own.
if [ -n "${TARGET_VMREMOTE:-}" ]; then
	VMREMOTE_ON_TARGET=$TARGET_VMREMOTE
	COPY_LIST=$(echo "$COPY_LIST" | sed 's#vmctx/build/vmremote ##')
fi
for src in $COPY_LIST; do
	$SSH "cat > /tmp/$(basename $src); chmod +x /tmp/$(basename $src)" < "$src" || exit 1
done
for f in $(for c in $COPY_LIST; do basename $c; done); do
	a=$(md5sum "$(ls vmctx/kernel/$f vmctx/build/$f 2>/dev/null | head -1)" | cut -d' ' -f1)
	b=$($SSH "md5sum /tmp/$f | cut -d' ' -f1")
	[ "$a" = "$b" ] || { echo "checksum mismatch for $f ($a vs $b)" >&2; exit 1; }
done
# The module must be the one just copied: a loaded older build silently
# invalidates every result. Retry the removal (a dying context can hold it
# briefly) and refuse to run rather than test a stale module.
$SSH "export $RSUDO_ENV
      for i in 1 2 3 4 5; do lsmod | grep -q '^vmctx' || break; $RSUDO rmmod vmctx 2>/dev/null; sleep 1; done
      lsmod | grep -q '^vmctx' && { echo 'vmctx module still loaded and cannot be removed' >&2; exit 1; }
      $RSUDO insmod $KO_ON_TARGET && $RSUDO ip link set lo up" || exit 1

# --- (re)start the syscall server on home -----------------------------------
# Needs root: the syscall server ptraces the program and reads its memory.
# A cached credential is used if there is one, otherwise sudo prompts. For
# unattended runs set SUDO_ASKPASS to a helper that prints the password — never
# put the password in this file, where it would go straight into the history
# and from there into whatever remote this is pushed to.
SUDO=sudo
[ -n "${SUDO_ASKPASS:-}" ] && SUDO="sudo -A"
$SUDO -n true 2>/dev/null || $SUDO -v || {
	echo "error: need sudo to run vmhome (it ptraces the program and reads" >&2
	echo "       its memory). Run from a terminal, or set SUDO_ASKPASS." >&2
	exit 1; }
$SUDO pkill -x vmhome 2>/dev/null || true
sleep 0.3
# The guest's own output goes to vmhome's stdout and the trace to its stderr.
# Merged, a log line written by another thread lands in the middle of the
# program's output — which then differs from what it prints natively for no
# reason but the logging. Keep them apart so the guest's output can be compared
# byte for byte.
$SUDO sh -c "cd $PWD && env ${VMHOME_ENV:-} ./vmctx/build/vmhome $PORT -v $* > $LOG/guest.out 2> $LOG/home.log &"
sleep 0.6
grep -q 'serving forwarded syscalls' "$LOG/home.log" || {
	echo "vmhome failed to start:" >&2; cat "$LOG/home.log" >&2; exit 1; }

# --- run the guest on the target --------------------------------------------
# Time the guest itself, not the staging above it: copying the module and
# tools over ssh and checksumming both ends is most of a second, and quoting
# that as the cost of running a program remotely would overstate it.
t0=$(date +%s.%N)
$SSH -tt "export $RSUDO_ENV; $RSUDO dmesg -c >/dev/null; $RSUDO ${VMREMOTE_ENV:-} $VMREMOTE_ON_TARGET $HOME_IP:$PORT --net" > "$LOG/remote.log" 2>&1
rc=$?
t1=$(date +%s.%N)
guest_secs=$(awk -v a="$t0" -v b="$t1" 'BEGIN{printf "%.2f", b-a}')
echo "=== vmremote (exit $rc, guest ran in ${guest_secs}s on $TARGET) ==="; cat "$LOG/remote.log"
echo "=== the guest's output ($LOG/guest.out) ==="; cat "$LOG/guest.out"
echo "=== home ($LOG/home.log) ==="; tail -25 "$LOG/home.log"
echo "=== target dmesg ==="; $SSH "export $RSUDO_ENV; $RSUDO dmesg | grep -i vmctx | tail -30"
exit $rc
