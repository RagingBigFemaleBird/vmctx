#!/bin/bash
# verify-build.sh -- prove that what is deployed is what this tree says, on
# whichever target is named. Exits non-zero and says which check failed.
#
#   ./verify-build.sh amd     # 10.0.0.229, kernel 7.0.14-vmctx-audit-*, syscalls 472/473
#   ./verify-build.sh intel   # 10.0.0.30,  kernel 6.18.35-0-lts-audit-*, syscalls 470/471
#
# AMD_KREL/AMD_KO/AMD_VMREMOTE/AMD_VMHOME and INTEL_KREL/INTEL_KO/INTEL_VMREMOTE
# name the deployment to check; the defaults are the deployment of record.
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

# The deployment of record (2026-09-10): each box runs an audit kernel built
# from the tree recorded in kernel-patches/README.md, with its module and the
# executor binaries under the audit directories rather than the old
# ~/vmctx-7.0.14.ko and /usr/local/bin. Override any of these when a different
# build is deployed; the defaults name what the two boxes run today.
case "$WHICH" in
amd)   TARGET=10.0.0.229; TUSER=biwu; WANT_RUN=472; WANT_CTL=473
       WANT_KREL=${AMD_KREL:-7.0.14-vmctx-audit-recall1-terminal222}
       KO_REMOTE=${AMD_KO:-/home/biwu/audit-20260907/terminal222-native/vmctx.ko}
       VMREMOTE_REMOTE=${AMD_VMREMOTE:-/home/biwu/lan-boot/vmctx/build/vmremote-local}
       VMHOME_REMOTE=${AMD_VMHOME:-/home/biwu/lan-boot/vmctx/build/vmhome} ;;
intel) TARGET=10.0.0.30;  TUSER=root; WANT_RUN=470; WANT_CTL=471
       WANT_KREL=${INTEL_KREL:-6.18.35-0-lts-audit-readobj71}
       KO_REMOTE=${INTEL_KO:-/root/audit-readobj71/vmctx.ko}
       VMREMOTE_REMOTE=${INTEL_VMREMOTE:-/root/audit-readobj71/vmremote-settle44}
       # The Intel box is a destination only; vmhome is not deployed there.
       VMHOME_REMOTE=${INTEL_VMHOME:-} ;;
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
VM=$($SSH "modinfo $KO_REMOTE 2>/dev/null | awk '/^vermagic/{print \$2}'" 2>/dev/null)
if [ -z "$VM" ]; then
	say "note" "no $KO_REMOTE on the target to check vermagic against"
elif [ "$VM" = "$KREL" ]; then
	ok "$KO_REMOTE vermagic $VM matches the running kernel"
else
	bad "$KO_REMOTE vermagic is $VM but the kernel is $KREL"
fi

# (5) SRCVERSION. A module file on disk says nothing about the module in
# memory: the box may have been insmod'ed from another path. srcversion is a
# hash of the module's own sources, so the loaded one is compared with the
# deployed file, and -- when this tree has built kernel/vmctx.ko -- with the
# module this tree's sources produce. That last comparison is the only check
# here that ties the LOADED module to THIS source; every other build id
# covers the userspace halves.
LOADED_SV=$($SSH 'cat /sys/module/vmctx/srcversion' 2>/dev/null)
FILE_SV=$($SSH "modinfo -F srcversion $KO_REMOTE" 2>/dev/null)
if [ -n "$LOADED_SV" ] && [ -n "$FILE_SV" ]; then
	[ "$LOADED_SV" = "$FILE_SV" ] && ok "loaded module srcversion $LOADED_SV is $KO_REMOTE" \
		|| bad "loaded module srcversion $LOADED_SV but $KO_REMOTE is $FILE_SV"
fi
if [ -f "$HERE/kernel/vmctx.ko" ] && [ -n "$LOADED_SV" ]; then
	TREE_SV=$(/usr/sbin/modinfo -F srcversion "$HERE/kernel/vmctx.ko" 2>/dev/null)
	[ "$TREE_SV" = "$LOADED_SV" ] && ok "loaded module built from this tree's kernel/ (srcversion $TREE_SV)" \
		|| bad "loaded module srcversion $LOADED_SV, this tree's kernel/vmctx.ko is $TREE_SV -- STALE"
fi

# (1) + (3): ask the deployed binaries themselves. Both halves print their
# build id and vmremote prints the syscall numbers it was compiled with.
B=$($SSH "$VMREMOTE_REMOTE --build-id" 2>/dev/null)
H=
[ -n "$VMHOME_REMOTE" ] && H=$($SSH "$VMHOME_REMOTE --build-id" 2>/dev/null)

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
[ -z "$VMHOME_REMOTE" ] && say "note" "vmhome is not deployed on a destination-only box; not checked"

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
