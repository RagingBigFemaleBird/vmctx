#!/bin/bash
# perfab.sh -- two (vmhome, vmremote) pairs, ALTERNATED run by run on the same
# programs, so the box's drift (C-states, a decaying load average, a background
# indexer) lands on both arms alike. perf.sh measures one build against native;
# this measures one build against another, which is the only honest way to
# say a change made the forwarded path faster (NOTES: compare arm with arm,
# alternated, never a run against a run recorded earlier).
#
#   sudo A=/path/to/vmhome-A:/path/to/vmremote-A B=...:... N=5 ./tests/perfab.sh [tag]
#
# A and B name a vmhome and a vmremote each (colon-separated). Reports, per
# program and arm, the median wall and the median per-fault / per-syscall cost
# from vmremote's own accounting, and the A/B ratio.
set -u
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
TAG=${1:-$(date +%H%M%S)}
N=${N:-5}
OUTD=${OUTD:-/home/biwu/perfab.$TAG}
PORT_BASE=${PORT_BASE:-33000}
KO=${KO:-/home/biwu/vmctx-7.0.14.ko}
HTTP_PORT=8098
WEB=${WEB:-/home/biwu/web}
A=${A:?A=vmhome:vmremote}; B=${B:?B=vmhome:vmremote}
A_ENV=${A_ENV:-}; B_ENV=${B_ENV:-}  # per-arm VMHOME_ENV/VMREMOTE_ENV additions
cd "$HERE" || exit 1
[ "$(id -u)" = 0 ] || { echo "must run as root" >&2; exit 2; }
mkdir -p "$OUTD"; rm -f "$OUTD/rows"

( cd "$WEB" && exec python3 -m http.server $HTTP_PORT --bind 127.0.0.1 ) \
	> "$OUTD/http.log" 2>&1 &
HTTP=$!
trap 'kill $HTTP 2>/dev/null' EXIT
for _ in $(seq 20); do
	curl -s -o /dev/null http://127.0.0.1:$HTTP_PORT/page.html && break
	sleep 0.25
done

declare -A PROG
PROG[wc]="/usr/bin/wc -c /usr/bin/wc"
PROG[curl]="/usr/bin/curl -sS -o /dev/null http://127.0.0.1:$HTTP_PORT/page.html"
PROG[sha]="/usr/bin/sha256sum /usr/lib/x86_64-linux-gnu/libc.so.6"
PROG[py]="/usr/bin/python3 -c print(sum(range(1000000)))"
PROG[gzip]="/usr/bin/gzip -c /usr/bin/wc"
PROG[links]="/usr/bin/links2 -dump http://127.0.0.1:$HTTP_PORT/page.html"
PROG[vmbench]="$HERE/build/vmbench 100000000 20000"
ORDER=${PROGS:-wc curl sha py gzip links vmbench}

now() { date +%s.%N; }
dur() { awk -v a="$1" -v b="$2" 'BEGIN{printf "%.3f", b-a}'; }
one() {	# arm prog i
	local arm=$1 p=$2 i=$3 pair cmd o t0 t1 tl el
	local aenv benv
	if [ "$arm" = A ]; then pair=$A; aenv=$A_ENV; else pair=$B; aenv=$B_ENV; fi
	cmd=${PROG[$p]}
	o="$OUTD/$p.$arm.$i"; rm -rf "$o"
	slot=$((slot + 1))
	t0=$(now)
	VMHOME=${pair%%:*} VMREMOTE=${pair##*:} VMCTX_SHARED=1 \
		PORT=$((PORT_BASE + slot * 4)) OUT="$o" KO="$KO" \
		VMHOME_ENV="$aenv" VMREMOTE_ENV="$aenv" \
		VMR_LIMIT=${VMR_LIMIT:-120} ./local-here.sh $cmd > "$o.log" 2>&1
	t1=$(now)
	tl=$(grep -a -m1 '^\[vmremote\] time:' "$o/remote.log")
	el=$(grep -a -m1 '^\[vmremote\] exited' "$o/remote.log")
	echo "$p $arm $i $(dur $t0 $t1) $(echo "$tl" | sed -n 's/.*time: \([0-9.]*\)s total = \([0-9.]*\) setup + \([0-9.]*\) faults (\([0-9]*\)) + \([0-9.]*\) syscalls (\([0-9]*\)).*/\1 \2 \3 \4 \5 \6/p') $(echo "$el" | sed -n 's/.*exited \([0-9-]*\) ;.*/\1/p')" >> "$OUTD/rows"
}

echo "=== perfab $TAG: A=$A B=$B N=$N kernel $(uname -r) $(uname -v | awk '{print $1}') clocksource $(cat /sys/bus/clocksource/devices/clocksource0/current_clocksource 2>/dev/null) $(date +%F\ %T)"
echo "A: $(${A%%:*} --build-id 2>&1 | tail -1) env=[$A_ENV]"
echo "B: $(${B%%:*} --build-id 2>&1 | tail -1) env=[$B_ENV]"
echo "load before: $(cut -d' ' -f1-3 /proc/loadavg)"
slot=0
for p in $ORDER; do
	for i in $(seq $N); do
		if [ $((i % 2)) = 1 ]; then one A $p $i; one B $p $i
		else one B $p $i; one A $p $i; fi
	done
	echo "$p done $(date +%T)"
done

med() { sort -n | awk '{a[NR]=$1} END{print a[int((NR+1)/2)]}'; }
col() { awk -v p="$1" -v a="$2" -v c="$3" '$1==p && $2==a {print $c}' "$OUTD/rows" | med; }
echo "=== summary (medians of $N alternated runs per arm; us/fault and us/syscall from vmremote's accounting)"
printf "%-8s %9s %9s %6s | %8s %8s %6s | %8s %8s %6s | %s\n" prog wallA wallB B/A faultA faultB B/A sysA sysB B/A exits
for p in $ORDER; do
	wa=$(col $p A 4); wb=$(col $p B 4)
	fa=$(awk -v p=$p '$1==p && $2=="A" && $8>0 {print $7/$8*1e6}' "$OUTD/rows" | med)
	fb=$(awk -v p=$p '$1==p && $2=="B" && $8>0 {print $7/$8*1e6}' "$OUTD/rows" | med)
	sa=$(awk -v p=$p '$1==p && $2=="A" && $10>0 {print $9/$10*1e6}' "$OUTD/rows" | med)
	sb=$(awk -v p=$p '$1==p && $2=="B" && $10>0 {print $9/$10*1e6}' "$OUTD/rows" | med)
	ex=$(awk -v p=$p '$1==p {print $2$11}' "$OUTD/rows" | sort | uniq -c | awk '{printf "%s x%d ", $2, $1}')
	printf "%-8s %9s %9s %6.2f | %8.0f %8.0f %6.2f | %8.0f %8.0f %6.2f | %s\n" "$p" "$wa" "$wb" \
		"$(awk -v a=$wa -v b=$wb 'BEGIN{print a? b/a : 0}')" "$fa" "$fb" \
		"$(awk -v a=$fa -v b=$fb 'BEGIN{print a? b/a : 0}')" "$sa" "$sb" \
		"$(awk -v a=$sa -v b=$sb 'BEGIN{print a? b/a : 0}')" "$ex"
done
echo "load after: $(cut -d' ' -f1-3 /proc/loadavg)"
