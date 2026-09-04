#!/bin/bash
# sync-amd.sh -- put this tree on the AMD box (10.0.0.229), rebuild both halves
# and the module there, and then PROVE what is deployed came from here.
#
# The AMD box builds its own kernel module against its own copy of the 7.0.14
# tree (~/vmctx-build/linux-7.0.14), so two things have to travel: the module
# source, and any change to the core kernel's headers the module compiles
# against. Missing the second is silent and expensive -- the module's
# backend->invalidate initialiser is guarded on VMCTX_BACKEND_HAS_INVALIDATE, so
# a box whose header predates that macro builds a module with NO guest-ASID
# shootdown and says nothing about it at compile time. It says so at load time,
# which is why this script prints that line at the end.
#
# Binaries are deleted before the build, never merely rebuilt: rsync preserves
# mtimes, so a freshly synced source looks older than the binary beside it and
# make would decide there was nothing to do.
set -eu
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "$HERE/.." && pwd)"
TARGET=${TARGET:-10.0.0.229}
KTREE=${KTREE:-/home/biwu/vmctx-build/linux-7.0.14}
SSH="ssh -i $ROOT/config/id_vmctx -o StrictHostKeyChecking=no \
     -o UserKnownHostsFile=/dev/null -o BatchMode=yes -o LogLevel=ERROR biwu@$TARGET"

echo "== syncing tree to $TARGET"
rsync -az -e "ssh -i $ROOT/config/id_vmctx -o StrictHostKeyChecking=no \
      -o UserKnownHostsFile=/dev/null -o BatchMode=yes -o LogLevel=ERROR" \
      --exclude 'build/' --exclude '*.ko' --exclude '*.o' \
      "$HERE/" biwu@$TARGET:lan-boot/vmctx/

# A file DELETED here has to be deleted there, and a plain rsync cannot say so.
#
# The build id is a hash of every source under user/, so a source removed in
# this tree but still sitting on the box makes the box compute a DIFFERENT id
# from the same content -- verify-build then says STALE forever and no rebuild
# fixes it. That is exactly what deleting startvm/vmenter/vmcat/vmproc did.
#
# --delete is applied only to the two directories this tree wholly owns, and
# never to the whole vmctx/ directory: that box keeps scratch work beside the
# checkout (patches, a build/ of its own) which is not ours to remove.
echo "== removing sources this tree no longer has (user/, tests/)"
RSH="ssh -i $ROOT/config/id_vmctx -o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null -o BatchMode=yes -o LogLevel=ERROR"
for d in user tests; do
	rsync -az --delete -e "$RSH" --exclude '*.o' --exclude '*.ko' \
	      "$HERE/$d/" "biwu@$TARGET:lan-boot/vmctx/$d/"
done

echo "== syncing the core kernel header the module compiles against"
rsync -az -e "ssh -i $ROOT/config/id_vmctx -o StrictHostKeyChecking=no \
      -o UserKnownHostsFile=/dev/null -o BatchMode=yes -o LogLevel=ERROR" \
      "$ROOT/src/linux-7.0.14/include/linux/vmctx.h" \
      biwu@$TARGET:$KTREE/include/linux/vmctx.h

echo "== rebuilding userspace + tests on $TARGET"
# The rm may need root (leftovers from runs under sudo), but the REBUILD runs
# as biwu: built under sudo, /home/biwu/tests came out root-owned, the next
# biwu suite could not relink a single case ("cannot open output file:
# Permission denied") and graded every one against the stale binary -- and a
# suite run whose /tmp/tr was also root-owned graded stale OUTPUTS: three
# suites in a row reported 105 passes that never executed. The box builds
# and runs the suite as biwu; sudo is for the module and the harness's own
# insmod, never for files the suite must rewrite.
$SSH "SUDO_PASS=${AMD_SUDO_PASS:-biwu} SUDO_ASKPASS=\$HOME/askpass.sh sudo -A \
  rm -rf /home/biwu/tests /tmp/tr \
         /home/biwu/lan-boot/vmctx/build/vmhome \
         /home/biwu/lan-boot/vmctx/build/vmremote-local"
$SSH "cd /home/biwu/lan-boot/vmctx && TESTS=/home/biwu/tests ./tests/suite.sh 0 2>&1 | tail -3"

if [ "${MODULE:-0}" = 1 ]; then
	echo "== rebuilding the kernel module on $TARGET"
	# See amd-box-build-layout: the same directory holds a .ko for the OTHER
	# kernel, rsynced here with its mtime, so make would skip the rebuild and
	# the copy would deploy a module for the wrong kernel.
	$SSH "SUDO_PASS=${AMD_SUDO_PASS:-biwu} SUDO_ASKPASS=\$HOME/askpass.sh sudo -A bash -c '
	  cd /home/biwu/lan-boot/vmctx/kernel &&
	  rm -f vmctx.ko vmctx.o vmctx.mod.* && touch vmctx.c &&
	  make KDIR=$KTREE 2>&1 | tail -3 &&
	  modinfo ./vmctx.ko | awk \"/^vermagic/\" &&
	  cp -f ./vmctx.ko /home/biwu/vmctx-7.0.14.ko'"
fi

echo
"$HERE/verify-build.sh" amd
