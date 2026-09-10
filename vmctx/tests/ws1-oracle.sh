#!/bin/bash
# ws1-oracle.sh <N> -- run ws1 N times and grade it by the PROGRAM'S OWN
# ARITHMETIC rather than by whether it happened to notice.
#
# ws1 is four threads that exist and store one word each. Its printed verdict is
# a poor oracle: the run can lose a store and still say PASS, and it can hang
# with nothing printed for reasons that have nothing to do with its own check.
# glibc gives a better one for free.
#
# It counts live threads in `__nptl_nthreads' -- `lock addl $1' in
# pthread_create, `lock subl $1' in start_thread -- and start_thread calls the C
# library's exit(3) if its decrement reaches ZERO:
#
#     40cd6c: lock subl $0x1,0xc298c(%rip)   # 4cf700 <__nptl_nthreads>
#     40cd74: je     40cf4c
#     40cf4c: xor    %edi,%edi
#     40cf4e: call   404fd0 <exit>
#
# __run_exit_handlers takes __exit_lock and leaves through _exit; it never
# releases it, because on a correct machine nothing runs after it. So a single
# discarded increment makes a WORKER run exit(), and main's own exit() then
# waits on that lock for ever with its buffered output still in stdio.
#
# Two independent signs of that one event, both of which fire whether or not the
# program notices:
#
#   zero   -- any context reads the count as 0 while main is still alive
#   wexit  -- a WORKER context left through exit_group(231) rather than exit(60).
#             Main's does too, so they are told apart by how much work the
#             context did: main's has served dozens of syscalls, a worker's a
#             handful. The module prints both numbers on its context-end line.
#
# ADDR defaults to this tree's ws1 binary. Re-derive it after any rebuild:
#     nm tests/ws1 | grep __nptl_nthreads
# An address that no longer names that word reads 0xdeadbeef in every context,
# which the WATCHWORD line prints with a '?' in front of it -- a wrong address
# cannot masquerade as a clean run.
#
# No page logging here on purpose: VMR_PGLOG widens the take window
# (ARCHITECTURE §9.2c) and must never be the thing that decides whether anything
# happened. Arm it separately, on the runs this script convicts.
set -u
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$HERE" || exit 1
N=${1:-40}
TESTS=${TESTS:-/home/biwu/tests}
ADDR=${ADDR:-0x4cf700}
RUNANY=${RUNANY:-./tests/runany.sh}

[ "$(id -u)" = 0 ] || { echo "must run as root (the runner does)" >&2; exit 2; }

pass=0; fail=0; z=0; w=0; heap=0
for i in $(seq 1 "$N"); do
	dmesg -C >/dev/null 2>&1
	VMREMOTE_ENV="VMR_WATCHWORD=$ADDR" VMR_LIMIT=${VMR_LIMIT:-25} \
		"$RUNANY" ws1 "$TESTS/ws1" >/dev/null 2>&1
	# The pattern is anchored to a CONTEXT FIELD -- "<pid>=0" -- and not to
	# a bare "=0". stderr is unbuffered and shared, so another thread's line
	# lands in the middle of a WATCHWORD line often enough to matter, and
	# "page service: ... has=0): ctx N present=0" then makes a PASSING run
	# read as a lost increment. Measured: 2 of 512 pool runs convicted that
	# way, both of which printed PASS.
	zi=$(grep -a WATCHWORD /tmp/tr/ws1/remote.log 2>/dev/null |
	     grep -acE ' [0-9]+=0( |$)'); zi=${zi:-0}
	wi=$(dmesg | grep -a 'ending via exit_group' |
	     grep -ac 'serviced [0-9] syscalls'); wi=${wi:-0}
	hi=$(grep -ac 'sysmalloc' /tmp/tr/ws1/guest.err 2>/dev/null); hi=${hi:-0}
	[ "$zi" -gt 0 ] && z=$((z+1))
	[ "$wi" -gt 0 ] && w=$((w+1))
	[ "$hi" -gt 0 ] && heap=$((heap+1))
	if grep -qa '^PASS$' /tmp/tr/ws1/guest.out 2>/dev/null && [ "$zi" = 0 ]; then
		pass=$((pass+1)); printf .
	else
		fail=$((fail+1)); printf F
		d=${KEEP:-/tmp/ws1-oracle}/run.$i.z$zi.w$wi
		mkdir -p "$d" && cp -f /tmp/tr/ws1/* "$d/" 2>/dev/null
		dmesg > "$d/dmesg.txt" 2>/dev/null
	fi
done
echo
echo "ws1 x$N: $pass clean, $fail with a failure or a lost count"
echo "  runs where __nptl_nthreads reached 0 (an increment was lost): $z"
echo "  runs where a WORKER left through the C library's exit():      $w"
echo "  runs whose guest died on glibc's sysmalloc assertion:         $heap"
echo "  failures kept under ${KEEP:-/tmp/ws1-oracle}"
