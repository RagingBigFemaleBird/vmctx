#!/bin/bash
# ws1-armab.sh <runs-per-arm> -- alternate the two vmremote builds on ONE boot and
# classify every run, so "the fix helped" and "a new failure shape appeared"
# are both answered against a control taken in the same minutes.
#
#   A = with the map-don't-copy precheck   (build/vmremote-A, build/vmhome-A)
#   B = without it                          (build/vmremote-B, build/vmhome-B)
#
# THE TWO ARMS ARE NOT BUILT BY THIS SCRIPT, and it will not run without them.
# Build each from the tree you want in that arm, on the box that will run it,
# and name them by hand -- SRCID is a content hash compiled into every binary,
# so the two arms carry different ids and `--build-id' says which is which:
#
#     make -s -C user ../build/vmremote-local        # from the tree for arm A
#     cp build/vmremote-local build/vmremote-A; cp build/vmhome build/vmhome-A
#     git stash push -- user/vmremote.c              # ...and again for arm B
#     make -s -C user ../build/vmremote-local
#     cp build/vmremote-local build/vmremote-B; cp build/vmhome build/vmhome-B
#     git stash pop
#     md5sum build/vmremote-A build/vmremote-B       # they MUST differ
#
# The md5 line is not decoration. An arm built from a source rsync had not
# touched is the same binary twice, and two identical arms measure this box's
# noise while looking exactly like a result (ARCHITECTURE §9.2a).
#
# VMCTX_SHARED=1 below is what makes the named binaries actually run:
# local-here.sh otherwise rebuilds build/vmremote-local over them.
#
# Classified, not tallied: four shapes have been seen and a pass/fail count
# cannot separate them.
#   lost  -- __nptl_nthreads reached 0 / a WORKER left through exit_group
#   heap  -- glibc's sysmalloc assertion
#   tmo   -- the source's page GET timed out and it zero-filled
#   other -- anything else that did not print PASS
set -u
cd /home/biwu/lan-boot/vmctx
N=${1:-30}
declare -A pass fail lost heap tmo other
for a in A B; do pass[$a]=0; fail[$a]=0; lost[$a]=0; heap[$a]=0; tmo[$a]=0; other[$a]=0; done
for i in $(seq 1 "$N"); do
  for a in A B; do
	dmesg -C >/dev/null 2>&1
	VMCTX_SHARED=1 VMREMOTE=/home/biwu/lan-boot/vmctx/build/vmremote-$a \
	VMHOME=/home/biwu/lan-boot/vmctx/build/vmhome-$a \
	VMREMOTE_ENV="VMR_WATCHWORD=0x4cf700" VMR_LIMIT=25 \
		./tests/runany.sh ws1 /home/biwu/tests/ws1 >/dev/null 2>&1
	# Anchored to a context field; see ws1-oracle.sh for why a bare '=0'
	# convicts passing runs.
	z=$(grep -a WATCHWORD /tmp/tr/ws1/remote.log 2>/dev/null | grep -acE ' [0-9]+=0( |$)'); z=${z:-0}
	w=$(dmesg | grep -a 'ending via exit_group' | grep -ac 'serviced [0-9] syscalls'); w=${w:-0}
	h=$(grep -ac sysmalloc /tmp/tr/ws1/guest.err 2>/dev/null); h=${h:-0}
	t=$(grep -ac 'TIMEOUT.*remote GET' /tmp/tr/ws1/home.log 2>/dev/null); t=${t:-0}
	if grep -qa '^PASS$' /tmp/tr/ws1/guest.out 2>/dev/null && [ "$z" = 0 ]; then
		pass[$a]=$(( ${pass[$a]} + 1 ))
	else
		fail[$a]=$(( ${fail[$a]} + 1 ))
		if   [ "$z" -gt 0 ] || [ "$w" -gt 0 ]; then lost[$a]=$(( ${lost[$a]} + 1 ))
		elif [ "$h" -gt 0 ]; then heap[$a]=$(( ${heap[$a]} + 1 ))
		elif [ "$t" -gt 0 ]; then tmo[$a]=$(( ${tmo[$a]} + 1 ))
		else other[$a]=$(( ${other[$a]} + 1 )); fi
		d=/home/biwu/failures/armab/$a.$i; mkdir -p "$d"
		cp -f /tmp/tr/ws1/* "$d/" 2>/dev/null; dmesg > "$d/dmesg.txt" 2>/dev/null
	fi
  done
  printf '%s' "$i "
done
echo
printf 'arm  pass fail | lost heap tmo other\n'
for a in A B; do
  printf '%-4s %4d %4d | %4d %4d %3d %5d\n' "$a" "${pass[$a]}" "${fail[$a]}" \
	 "${lost[$a]}" "${heap[$a]}" "${tmo[$a]}" "${other[$a]}"
done
