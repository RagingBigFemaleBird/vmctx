#!/bin/bash
# local-run.sh — the same remote-execution run as remote-run.sh, but with BOTH
# ends on the target machine, talking over its loopback.
#
#   TARGET=10.0.0.229 ./local-run.sh /usr/bin/elinks -dump /tmp/page.html
#
# The program has to exist on the target, since that machine is now both ends:
# `apk add elinks` gets one, and does not survive a reboot.
#
# Why: a run over the LAN is dominated by round-trip latency, and every cost
# inside vmremote/vmhome hides underneath it. Over loopback the network is
# ~30 us instead of ~500 us, so what is left is the work itself — the kernel's
# WAIT/RESUME handshake, pagemap lookups, userfaultfd, the shadow's syscalls.
# Optimise here, then confirm the win survives over the LAN.
#
# Same staleness rules as remote-run.sh: every binary is checksummed after the
# copy, the module must be the one just built, and a previous vmhome is killed
# before a new one starts. A stray vmhome produces intermittent failures that
# look exactly like real bugs.
set -u
cd "$(dirname "$0")/.."
TARGET=${TARGET:-10.0.0.229}
PORT=${PORT:-9999}
SSH="ssh -i config/id_vmctx -o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null -o ConnectTimeout=8 -o LogLevel=ERROR root@$TARGET"
LOG=/tmp/claude-1000/-home-desktop-lan-boot/local-run
mkdir -p "$LOG"

if [ $# -lt 1 ]; then echo "usage: $0 <program-on-target> [args...]" >&2; exit 2; fi

# --- refresh the target's copy of the module and tools -----------------------
$SSH 'pkill -9 vmremote; pkill -9 vmhome; sleep 1; rmmod vmctx 2>/dev/null; true'
for src in vmctx/kernel/vmctx.ko vmctx/build/vmremote vmctx/build/vmhome vmctx/build/dumpvm; do
	$SSH "cat > /tmp/$(basename $src); chmod +x /tmp/$(basename $src)" < "$src" || exit 1
done
for f in vmctx.ko vmremote vmhome; do
	a=$(md5sum "$(ls vmctx/kernel/$f vmctx/build/$f 2>/dev/null | head -1)" | cut -d' ' -f1)
	b=$($SSH "md5sum /tmp/$f | cut -d' ' -f1")
	[ "$a" = "$b" ] || { echo "checksum mismatch for $f ($a vs $b)" >&2; exit 1; }
done
$SSH 'for i in 1 2 3 4 5; do lsmod | grep -q "^vmctx" || break; rmmod vmctx 2>/dev/null; sleep 1; done
      lsmod | grep -q "^vmctx" && { echo "vmctx module still loaded and cannot be removed" >&2; exit 1; }
      insmod /tmp/vmctx.ko && ip link set lo up' || exit 1

# --- run both ends there, timing only the guest ------------------------------
# The guest's own output and the trace go to separate files for the same reason
# remote-run.sh keeps them apart: merged, a log line lands in the middle of the
# program's output and it no longer compares byte for byte against a native run.
# A fresh vmhome per repetition: its stdout is the guest's output, so runs that
# share one would append to each other and no single run could be compared byte
# for byte against a native one.
$SSH -tt "dmesg -c >/dev/null
	n=0
	while [ \$n -lt ${REPEAT:-1} ]; do
		n=\$((n+1))
		# Wait for the port to actually be free. A vmhome that still holds
		# it means the next one fails to bind and dies, and the run then
		# talks to the *previous* vmhome — whose oracle has gone — which
		# looks like a guest that crashes early rather than like a harness
		# fault. Waiting on pgrep does not work: vmhome leaves zombies,
		# and a zombie matches the name while holding nothing.
		# Not -x: busybox's pkill -x compares the whole command line, so
		# 'pkill -9 -x vmhome' matches nothing, reports success, and
		# leaves the old vmhome holding the port — after which the run
		# either measures the previous vmhome or gets no vmhome at all.
		# And SIGKILL, not SIGTERM: a shadow parked in a userfaultfd
		# fault sleeps TASK_KILLABLE and wakes for nothing else.
		pkill -9 vmhome 2>/dev/null
		for i in 1 2 3 4 5 6 7 8 9 10 11 12 13 14 15; do
			netstat -ltn 2>/dev/null | grep -q \":$PORT \" || break
			sleep 0.4
		done
		if netstat -ltn 2>/dev/null | grep -q \":$PORT \"; then
			echo \"port $PORT still held after 6s by:\"
			netstat -ltnp 2>/dev/null | grep \":$PORT \"
		fi
		rm -f /tmp/guest.out /tmp/home.log /tmp/remote.log
		env ${VMHOME_ENV:-} /tmp/vmhome $PORT -v $* > /tmp/guest.out 2> /tmp/home.log &
		sleep 0.5
		grep -q 'serving forwarded syscalls' /tmp/home.log || { echo \"run \$n: vmhome failed to start\"; cat /tmp/home.log; continue; }
		t0=\$(cut -d' ' -f1 /proc/uptime)
		${VMREMOTE_ENV:-} /tmp/vmremote 127.0.0.1:$PORT --net > /tmp/remote.log 2>&1
		rc=\$?
		t1=\$(cut -d' ' -f1 /proc/uptime)
		echo \"=== run \$n: exit \$rc, \$(awk -v a=\$t0 -v b=\$t1 'BEGIN{printf \"%.2f\", b-a}')s wall, guest output \$(md5sum < /tmp/guest.out | cut -c1-8) \$(wc -c < /tmp/guest.out)B ===\"
		grep -E '^\[vmremote\] (time|exited|pages served)' /tmp/remote.log
		grep -E 'PROFILE' /tmp/home.log | tail -1
		# Keep every run's logs: the failures are intermittent, and one
		# that is only noticed after the next run has overwritten them
		# cannot be diagnosed at all.
		cp /tmp/remote.log /tmp/remote-\$n.log; cp /tmp/home.log /tmp/home-\$n.log
	done
	echo '=== the guest output ==='; cat /tmp/guest.out
	echo '=== home ==='; tail -20 /tmp/home.log
	pkill -9 vmhome
	exit \$rc" 2>&1 | tee "$LOG/run.log"
rc=${PIPESTATUS[0]}
exit $rc
