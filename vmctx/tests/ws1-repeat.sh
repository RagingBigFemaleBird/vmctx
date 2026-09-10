#!/bin/bash
# ws1-repeat.sh <runs> [jobs] -- the sharp oracle: catch the guest executing
# start_thread's tail TWICE, whether or not the program notices.
#
# WHY THIS EXISTS. ws1's residual failure is a thread that decrements glibc's
# `__nptl_nthreads' twice, which drives the count to zero while main is alive
# and makes a WORKER run the C library's exit(3) -- see tests/ws1-oracle.sh for
# the rest of that chain. Waiting for the count to reach zero is waiting for a
# coincidence on top of the defect, and it happens about once in five hundred
# runs. The repeat itself is the defect and can be seen directly.
#
# WHY TWO RIPS DECIDE IT. Between them start_thread is straight-line with no
# backward branch, and each has exactly one call site, so a context reporting
# either twice has re-executed code it cannot re-enter:
#
#     0x40ccf5   rt_sigprocmask before the start routine runs
#     0x40cdaa   rt_sigprocmask immediately after `lock subl $1,__nptl_nthreads'
#
# 0x40cc85 (set_robust_list) sits just above the window and is the control: it
# is reported ONCE in every capture so far, which is what bounds where the rip
# went back to. Re-derive all three after any rebuild of tests/ws1:
#
#     objdump -d tests/ws1 | sed -n '/<start_thread>:/,/^$/p'
#
# WHY THE POOL. Serially the event is about one run in a thousand; eight at a
# time it is one in five hundred and a run costs an eighth of a second, so a
# batch of 512 takes about a minute. ARCHITECTURE §12a.1 has the general point:
# a pool is a different load and therefore a different measurement, so compare a
# pool against a pool -- never against a serial rate.
#
# WHY dmesg IS STREAMED. The module prints a line per context end plus a
# sixteen-entry syscall-exit ring, and the kernel ring buffer holds about
# twenty-four runs of this test. Read once at the end, the evidence for the one
# interesting run is always the part that was overwritten. `dmesg --follow'
# loses nothing. That ring is the whole point of the capture: it is what shows
# the rip going backwards, and it is the only record of it.
set -u
N=${1:-512}; J=${2:-8}
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
TESTS=${TESTS:-/home/biwu/tests}
KO=${KO:-${SUDO_USER:+/home/$SUDO_USER}/vmctx-7.0.14.ko}
VMREMOTE=${VMREMOTE:-$HERE/build/vmremote-local}
VMHOME=${VMHOME:-$HERE/build/vmhome}
OUTD=${OUTD:-/tmp/ws1-repeat}
cd "$HERE" || exit 1
[ "$(id -u)" = 0 ] || { echo "must run as root (the runner does)" >&2; exit 2; }
for f in "$VMREMOTE" "$VMHOME"; do
	[ -x "$f" ] || { echo "no $f -- build it first" >&2; exit 2; }
done
lsmod | grep -q '^vmctx' || insmod "$KO" || exit 1
rm -rf "$OUTD"; mkdir -p "$OUTD"
dmesg -C >/dev/null 2>&1
dmesg -w >> "$OUTD/dmesg.txt" 2>/dev/null &
DMW=$!

one() {
	local o="$OUTD/run.$1"
	mkdir -p "$o"
	env VMCTX_SHARED=1 PORT=$((${PORT_BASE:-24000} + $2 * 4)) OUT="$o" KO="$KO" \
	    VMREMOTE="$VMREMOTE" VMHOME="$VMHOME" \
	    VMREMOTE_ENV="VMR_WATCHWORD=${WATCH:-0x4cf700} ${VMR_EXTRA:-}" \
	    VMR_LIMIT=${VMR_LIMIT:-60} \
	    ./local-here.sh "$TESTS/ws1" > "$o/run.log" 2>&1
}
declare -a slot_pid all_pids
launch() { local s; while :; do for s in $(seq 0 $((J-1))); do
	if [ -z "${slot_pid[$s]:-}" ] || ! kill -0 "${slot_pid[$s]}" 2>/dev/null; then
		one "$1" "$s" & slot_pid[$s]=$!; all_pids+=($!); return; fi
	done; sleep 0.2; done; }

t0=$(date +%s)
for i in $(seq 1 "$N"); do launch "$i"; done
# By pid, NOT a bare `wait': that waits for every background job, and
# `dmesg --follow' never exits -- the batch then hangs for ever having printed
# nothing, which is what happened the first time this was written.
wait "${all_pids[@]}" 2>/dev/null
t1=$(date +%s)
sleep 1; kill $DMW 2>/dev/null

rep=0; pass=0; fail=0
for i in $(seq 1 "$N"); do
	o="$OUTD/run.$i"; [ -d "$o" ] || continue
	r=$(grep -a WATCHWORD "$o/remote.log" 2>/dev/null |
	    grep -oE 'ctx [0-9]+, nr 14, rip 0x(40ccf5|40cdaa)' |
	    sort | uniq -c | awk '$1>1' | wc -l)
	p=0; grep -qa '^PASS$' "$o/guest.out" 2>/dev/null && p=1
	if [ "${r:-0}" -gt 0 ]; then rep=$((rep+1)); mv "$o" "$OUTD/REPEAT.$i.pass$p"
	elif [ "$p" = 1 ]; then pass=$((pass+1)); rm -rf "$o"
	else fail=$((fail+1)); mv "$o" "$OUTD/other.$i"; fi
done
echo "ws1 x$N, $J at a time, $((t1-t0))s: $pass clean, $rep with a REPEATED start_thread tail, $fail other failure"
echo "kept in $OUTD; the repeating context's kernel ring is in $OUTD/dmesg.txt:"
echo "    grep -a '<ctx> syscall\\|<ctx> fault' $OUTD/dmesg.txt | tail -12"
