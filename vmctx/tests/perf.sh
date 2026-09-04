#!/bin/bash
# perf.sh -- typical programs at two layers, native and forwarded loopback,
# with vmremote's own breakdown of where the forwarded time went.
#
# The question this answers is the one a user asks: how much slower is a real
# program under vmctx, and what is the slow part. vmbench answers only the
# two extremes (pure compute, pure syscall); real programs are short-lived and
# demand-paging bound, so the numbers that matter are wall clock and the
# per-fault / per-syscall cost vmremote reports at exit.
#
# Run as root on the box, on an IDLE machine (a pool beside it is a different
# load -- see NOTES: never compare an idle-box run against a loaded one):
#
#	sudo N=3 ./tests/perf.sh [tag]
#
# Output: one line per run, then a summary table per program. Everything is
# left under $OUTD (default /home/biwu/perf.<tag>) for re-reading.
set -u
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
TAG=${1:-$(date +%H%M%S)}
N=${N:-3}
OUTD=${OUTD:-/home/biwu/perf.$TAG}
PORT_BASE=${PORT_BASE:-31000}
KO=${KO:-/home/biwu/vmctx-7.0.14.ko}
HTTP_PORT=8099
WEB=${WEB:-/home/biwu/web}
cd "$HERE" || exit 1
[ "$(id -u)" = 0 ] || { echo "must run as root" >&2; exit 2; }
[ -x build/vmhome ] && [ -x build/vmremote-local ] || { echo "build first" >&2; exit 2; }
mkdir -p "$OUTD"

# A web server for curl and links2, on a port of this script's own.
( cd "$WEB" && exec python3 -m http.server $HTTP_PORT --bind 127.0.0.1 ) \
	> "$OUTD/http.log" 2>&1 &
HTTP=$!
trap 'kill $HTTP 2>/dev/null' EXIT
for _ in $(seq 20); do
	curl -s -o /dev/null http://127.0.0.1:$HTTP_PORT/page.html && break
	sleep 0.25
done

# The programs. Each is a shape a user actually runs: a tiny tool (wc), a
# network client (curl), a hashing pass over a 2 MB file (read(2)-heavy,
# buffer pages bouncing between the machines), an interpreter start-up
# (python: many pages, many syscalls), a compressor (compute over a file,
# output written back through forwarded write(2)), a text-mode browser
# (links2: a real program with a real page fetch), and vmbench's two extremes.
declare -A PROG
PROG[wc]="/usr/bin/wc -c /usr/bin/wc"
PROG[curl]="/usr/bin/curl -sS -o /dev/null http://127.0.0.1:$HTTP_PORT/page.html"
PROG[sha]="/usr/bin/sha256sum /usr/lib/x86_64-linux-gnu/libc.so.6"
PROG[py]="/usr/bin/python3 -c print(sum(range(1000000)))"
PROG[gzip]="/usr/bin/gzip -c /usr/bin/wc"
PROG[links]="/usr/bin/links2 -dump http://127.0.0.1:$HTTP_PORT/page.html"
PROG[vmbench]="$HERE/build/vmbench 100000000 20000"
ORDER=${PROGS:-wc curl sha py gzip links vmbench}

# bash's own clock, no fork: this box's date(1) is the uutils multi-call
# binary and costs ~10 ms to exec, which sat inside every native wall here.
now() { echo "$EPOCHREALTIME"; }
dur() { awk -v a="$1" -v b="$2" 'BEGIN{printf "%.3f", b-a}'; }

echo "=== perf $TAG: $(./build/vmhome --build-id 2>&1 | tail -1) kernel $(uname -r) $(uname -v | awk '{print $1}')  N=$N  $(date +%F\ %T)"
echo "load before: $(cut -d' ' -f1-3 /proc/loadavg)"
# The two machine settings that decide these numbers more than anything in
# the tree does, said on every run so no two tables are compared blind:
#
#   governor  schedutil reads a forwarded-syscall storm -- four tasks that
#             each sleep between every hand-over -- as a mostly idle box and
#             keeps the cores at their floor. Measured (session 46, one
#             boot, vmbench's getppid loop): 42-114 us/call under schedutil,
#             35-40 us/call under performance, the spread itself being the
#             governor. GOV=performance sets it for this run and restores
#             the box's own choice afterwards.
#   spin      the kernel's spin-before-sleep budget at its four wait sites
#             (vmctx_spin_us; 0 is the sleep-only control arm).
GOV_FILES=$(ls /sys/devices/system/cpu/cpu*/cpufreq/scaling_governor 2>/dev/null)
if [ -n "${GOV:-}" ] && [ -n "$GOV_FILES" ]; then
	GOV_WAS=$(cat /sys/devices/system/cpu/cpu0/cpufreq/scaling_governor)
	for f in $GOV_FILES; do echo "$GOV" > "$f"; done
	trap 'for f in $GOV_FILES; do echo "$GOV_WAS" > "$f"; done' EXIT
fi
echo "governor: $(cat /sys/devices/system/cpu/cpu0/cpufreq/scaling_governor 2>/dev/null || echo none)  vmctx_spin_us: $(cat /sys/module/kernel/parameters/vmctx_spin_us 2>/dev/null || echo none)"

slot=0
for p in $ORDER; do
	cmd=${PROG[$p]}
	echo "--- $p: $cmd"
	# native, N runs: the program as the user runs it, same box, same files
	nat=""
	for i in $(seq $N); do
		t0=$(now); $cmd > "$OUTD/$p.native.$i.out" 2>/dev/null; t1=$(now)
		nat="$nat $(dur $t0 $t1)"
		# One wall per LINE: dur() prints no newline, and the first
		# version appended them into one line, so the summary's "native"
		# median was the five walls glued together (0.0080.0080.007...).
		echo "$(dur $t0 $t1)" >> "$OUTD/nat.$p"
	done
	echo "native   wall:$nat"
	# forwarded loopback, N runs, each on its own port and output directory
	for i in $(seq $N); do
		o="$OUTD/$p.fwd.$i"; rm -rf "$o"
		slot=$((slot + 1))
		t0=$(now)
		VMCTX_SHARED=1 PORT=$((PORT_BASE + slot * 4)) OUT="$o" KO="$KO" \
			VMR_LIMIT=${VMR_LIMIT:-120} ./local-here.sh $cmd > "$o.log" 2>&1
		rc=$?
		t1=$(now)
		# The harness's wall (vmhome start, its readiness poll at 100 ms
		# steps, local-here.sh's own housekeeping) is not the program's;
		# vmremote's life is, and local-here.sh reports it on its "===
		# exit" line with nothing of its own inside the clock (session
		# 40: the old enclosure carried ~90 ms of ps/pgrep and read wc as
		# 0.22 s against a 0.046 s vmremote). Both are kept: harness for
		# the runner's cost, wall for the program's.
		harness=$(dur $t0 $t1)
		wall=$(sed -n 's/.*=== exit [0-9]* in \([0-9.]*\)s.*/\1/p' "$o.log" | head -1)
		wall=${wall:-$harness}
		# vmremote's own accounting:
		#  time: T total = S setup + F faults (nf) + Y syscalls (ns) + R ...
		#  exited E ; NS syscalls, NF forwarded home, NP pages faulted in from home
		tl=$(grep -a -m1 '^\[vmremote\] time:' "$o/remote.log")
		el=$(grep -a -m1 '^\[vmremote\] exited' "$o/remote.log")
		total=$(echo "$tl" | sed -n 's/.*time: \([0-9.]*\)s total.*/\1/p')
		setup=$(echo "$tl" | sed -n 's/.* = \([0-9.]*\) setup.*/\1/p')
		tf=$(echo "$tl" | sed -n 's/.*+ \([0-9.]*\) faults (\([0-9]*\)).*/\1/p')
		nf=$(echo "$tl" | sed -n 's/.*+ \([0-9.]*\) faults (\([0-9]*\)).*/\2/p')
		ts=$(echo "$tl" | sed -n 's/.*+ \([0-9.]*\) syscalls (\([0-9]*\)).*/\1/p')
		ns=$(echo "$tl" | sed -n 's/.*+ \([0-9.]*\) syscalls (\([0-9]*\)).*/\2/p')
		np=$(echo "$el" | sed -n 's/.*, \([0-9]*\) pages faulted in.*/\1/p')
		same=$(cmp -s "$o/guest.out" "$OUTD/$p.native.1.out" && echo same || echo DIFFERS)
		per_f=$(awk -v t="${tf:-0}" -v n="${nf:-0}" 'BEGIN{printf "%.0f", n? t/n*1e6 : 0}')
		per_s=$(awk -v t="${ts:-0}" -v n="${ns:-0}" 'BEGIN{printf "%.0f", n? t/n*1e6 : 0}')
		printf "fwd %d    wall:%s (harness %s) total:%s setup:%s faults:%s (%s ev, %s pages, %s us/ev) syscalls:%s (%s, %s us/call) rc=%s out=%s\n" \
			"$i" "$wall" "$harness" "${total:-?}" "${setup:-?}" "${tf:-?}" "${nf:-?}" "${np:-?}" "$per_f" "${ts:-?}" "${ns:-?}" "$per_s" "$rc" "$same"
		echo "$p $i $wall ${total:-0} ${setup:-0} ${tf:-0} ${nf:-0} ${np:-0} ${ts:-0} ${ns:-0} $rc $same" >> "$OUTD/rows"
	done
	if [ "$p" = vmbench ]; then
		echo "native   $(grep -h -E 'compute|syscall' "$OUTD/vmbench.native.1.out" | tr '\n' ' ')"
		echo "fwd      $(grep -h -E 'compute|syscall' "$OUTD/vmbench.fwd.1/guest.out" | tr '\n' ' ')"
	fi
done

echo "=== summary (medians over $N fwd runs; native median wall beside)"
printf "%-8s %8s %8s %7s %8s %6s %6s %8s %6s %8s\n" prog native fwd setup faults nfev pages us/fev nsys us/sys
med() { sort -n | awk '{a[NR]=$1} END{print a[int((NR+1)/2)]}'; }
for p in $ORDER; do
	nw=$(med < "$OUTD/nat.$p")
	fw=$(awk -v p="$p" '$1==p{print $3}' "$OUTD/rows" | med)
	st=$(awk -v p="$p" '$1==p{print $5}' "$OUTD/rows" | med)
	tf=$(awk -v p="$p" '$1==p{print $6}' "$OUTD/rows" | med)
	nf=$(awk -v p="$p" '$1==p{print $7}' "$OUTD/rows" | med)
	np=$(awk -v p="$p" '$1==p{print $8}' "$OUTD/rows" | med)
	ts=$(awk -v p="$p" '$1==p{print $9}' "$OUTD/rows" | med)
	ns=$(awk -v p="$p" '$1==p{print $10}' "$OUTD/rows" | med)
	printf "%-8s %8s %8s %7s %8s %6s %6s %8.0f %6s %8.0f\n" "$p" "$nw" "$fw" "$st" "$tf" "$nf" "$np" \
		"$(awk -v t="$tf" -v n="$nf" 'BEGIN{print n? t/n*1e6 : 0}')" "$ns" \
		"$(awk -v t="$ts" -v n="$ns" 'BEGIN{print n? t/n*1e6 : 0}')"
done
echo "load after: $(cut -d' ' -f1-3 /proc/loadavg)"
