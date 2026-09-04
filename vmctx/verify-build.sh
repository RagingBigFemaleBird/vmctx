#!/bin/bash
# verify-build.sh -- prove that what is deployed is what this tree says, on
# whichever target is named. Exits non-zero and says which check failed.
#
#   ./verify-build.sh amd     # 10.0.0.229, kernel 7.0.14-vmctx, syscalls 472/473
#   ./verify-build.sh intel   # 10.0.0.30,  kernel 6.18.35-0-lts, syscalls 470/471
#
# Every check here exists because its absence produced a wrong answer that
# looked right. In order:
#
#  1. SOURCE ID. rsync preserves mtimes, so a source freshly synced from home
#     looks OLDER than the binary beside it; make prints its banner, does
#     nothing, and the run measures the previous change. The build id is a hash
#     of the source content, compiled in and printed at start-up, so this
#     compares content with content and no timestamp is trusted anywhere.
#  2. VERMAGIC. The tree also holds a vmctx.ko built for the OTHER kernel and
#     rsynced here; with mtimes preserved make skips the rebuild and the copy
#     step deploys a module for the wrong kernel. It loads far enough to look
#     plausible.
#  3. SYSCALL NUMBERS. vmctx_run/vmctx_ctl are 470/471 on 6.18.35 and 472/473
#     on 7.0.14. A wrong number answers EINVAL, not ENOSYS -- so the failure
#     arrives later, as a broken run, not as "no such syscall".
#  4. LOADED. A module that is built and staged but not loaded, or a stale one
#     still loaded from a previous kernel, fails the same way as a bad build.
set -u
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "$HERE/.." && pwd)"
WHICH=${1:-amd}
rc=0
say()  { printf '  %-9s %s\n' "$1" "$2"; }
ok()   { say "ok" "$1"; }
bad()  { say "FAIL" "$1"; rc=1; }

case "$WHICH" in
amd)   TARGET=10.0.0.229; TUSER=biwu; WANT_KREL=7.0.14-vmctx;   WANT_RUN=472; WANT_CTL=473 ;;
intel) TARGET=10.0.0.30;  TUSER=root; WANT_KREL=6.18.35-0-lts;  WANT_RUN=470; WANT_CTL=471 ;;
*) echo "usage: $0 [amd|intel]" >&2; exit 2 ;;
esac
SSH="ssh -i $ROOT/config/id_vmctx -o StrictHostKeyChecking=no \
     -o UserKnownHostsFile=/dev/null -o BatchMode=yes -o ConnectTimeout=8 \
     -o LogLevel=ERROR $TUSER@$TARGET"

echo "== verifying $WHICH ($TARGET, expecting $WANT_KREL, syscalls $WANT_RUN/$WANT_CTL)"

SRCID=$(make -s -C "$HERE/user" print-srcid 2>/dev/null)
[ -n "$SRCID" ] || { echo "cannot compute a source id here" >&2; exit 2; }
echo "   this tree's source id: $SRCID"

if ! $SSH true 2>/dev/null; then
	bad "$TARGET is not reachable over ssh"
	exit 1
fi

KREL=$($SSH 'uname -r' 2>/dev/null)
[ "$KREL" = "$WANT_KREL" ] && ok "kernel $KREL" \
	|| bad "kernel is $KREL, expected $WANT_KREL"

# (4) the module, and that it is THIS kernel's
if $SSH 'lsmod' 2>/dev/null | grep -q '^vmctx'; then
	ok "vmctx module loaded"
else
	bad "vmctx module is not loaded on $TARGET"
fi

# (2) vermagic of the .ko that would be (or was) deployed
case "$WHICH" in
amd)   KO_REMOTE=/home/biwu/vmctx-7.0.14.ko ;;
intel) KO_REMOTE=/tmp/vmctx.ko ;;
esac
VM=$($SSH "modinfo $KO_REMOTE 2>/dev/null | awk '/^vermagic/{print \$2}'" 2>/dev/null)
if [ -z "$VM" ]; then
	say "note" "no $KO_REMOTE on the target to check vermagic against"
elif [ "$VM" = "$KREL" ]; then
	ok "$KO_REMOTE vermagic $VM matches the running kernel"
else
	bad "$KO_REMOTE vermagic is $VM but the kernel is $KREL"
fi

# (1) + (3): ask the deployed binaries themselves. Both halves print their
# build id and vmremote prints the syscall numbers it was compiled with.
case "$WHICH" in
amd)
	B=$($SSH '/home/biwu/lan-boot/vmctx/build/vmremote-local --build-id' 2>/dev/null)
	H=$($SSH '/home/biwu/lan-boot/vmctx/build/vmhome --build-id' 2>/dev/null)
	;;
intel)
	B=$($SSH '/usr/local/bin/vmremote --build-id' 2>/dev/null)
	H=$($SSH '/usr/local/bin/vmhome --build-id' 2>/dev/null)
	;;
esac

check_id() {   # $1 = label, $2 = a line containing "build <id>"
	local lbl=$1 line=$2 id
	id=$(printf '%s' "$line" | sed -n 's/.*build \([0-9a-f][0-9a-f]*\).*/\1/p')
	if [ -z "$id" ]; then
		bad "$lbl does not report a build id (built before verify-build, or not deployed)"
	elif [ "$id" = "$SRCID" ]; then
		ok "$lbl built from this tree ($id)"
	else
		bad "$lbl was built from $id, this tree is $SRCID -- STALE"
	fi
}
check_id "vmremote" "$B"
[ -n "$H" ] && check_id "vmhome  " "$H"

RUN=$(printf '%s' "$B" | sed -n 's/.*vmctx_run=\([0-9]*\).*/\1/p')
CTL=$(printf '%s' "$B" | sed -n 's/.*vmctx_ctl=\([0-9]*\).*/\1/p')
if [ -z "$RUN" ]; then
	bad "vmremote does not report its syscall numbers"
elif [ "$RUN" = "$WANT_RUN" ] && [ "$CTL" = "$WANT_CTL" ]; then
	ok "vmremote compiled for syscalls $RUN/$CTL"
else
	bad "vmremote compiled for $RUN/$CTL, this kernel wants $WANT_RUN/$WANT_CTL"
fi

echo
[ $rc = 0 ] && echo "== $WHICH: everything deployed matches this tree" \
	    || echo "== $WHICH: MISMATCH above -- do not trust numbers from this target"
exit $rc
