#!/bin/bash
# torture.sh <case> [runs] [jobs] -- run one case many times, several at once,
# and CLASSIFY every failure by its shape instead of counting them.
#
# WHY A POOL. A pool is a different load and therefore a different measurement
# (ARCHITECTURE §12a.1) -- compare a pool against a pool, never against a serial
# rate. It is also this tree's stress amplifier: ws1's remaining defects appear
# roughly ten times as often eight-at-a-time as they do serially, and a run costs
# an eighth of a second, so 512 runs take about a minute. That is the difference
# between a defect you can chase and one you cannot.
#
# WHY CLASSIFY. Four distinct defects fail this suite's threaded cases, and a
# pass/fail tally cannot separate them -- which is how a repair that removes one
# class while adding another has repeatedly looked like "no change". Each shape
# below is a different defect with a different repair:
#
#   lost   the program's own arithmetic went wrong: glibc's __nptl_nthreads
#          reached 0 (ARCHITECTURE §21), or a WORKER left through the C
#          library's exit() rather than start_thread's own syscall
#   heap   glibc's sysmalloc assertion -- the heap's top chunk read as garbage
#   tmo    the source's page GET expired: a SERVE take stalled past it (§23)
#   hang   the run reached its limit with the guest still running
#   died   the guest died of a signal or an unrecoverable fault
#   other  anything else that did not print PASS
#
# It also reports the mechanism counters, so a change can be judged on the
# mechanism and not only on the rate: contexts killed by the fault deadline, and
# the kernel's own copy-on-write breaks (vmctx_take_cow_broken -- the take-site
# break that replaced §23's monitor-asked one; the "stalled break" grep is gone
# with the message, because the stall itself is gone: the monitor no longer asks
# the context to do anything).
#
#   sudo ./tests/torture.sh ws1 512 8
#   sudo CASE_OK='^PASS$' ./tests/torture.sh pg2 256 8
set -u
T=${1:?usage: torture.sh <case> [runs] [jobs]}
N=${2:-512}
J=${3:-8}
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
TESTS=${TESTS:-/home/biwu/tests}
KO=${KO:-${SUDO_USER:+/home/$SUDO_USER}/vmctx-7.0.14.ko}
VMREMOTE=${VMREMOTE:-$HERE/build/vmremote-local}
VMHOME=${VMHOME:-$HERE/build/vmhome}
OUTD=${OUTD:-/tmp/torture.$T}
OK=${CASE_OK:-'^PASS$'}
WATCH=${WATCH:-0x4cf700}	# ws1's __nptl_nthreads; harmless elsewhere

cd "$HERE" || exit 1
[ "$(id -u)" = 0 ] || { echo "must run as root (the runner does)" >&2; exit 2; }
[ -x "$TESTS/$T" ] || { echo "no such case: $TESTS/$T" >&2; exit 2; }
for f in "$VMREMOTE" "$VMHOME"; do
	[ -x "$f" ] || { echo "no $f -- build it first" >&2; exit 2; }
done
lsmod | grep -q '^vmctx' || insmod "$KO" || exit 1
rm -rf "$OUTD"; mkdir -p "$OUTD"

one() {
	local o="$OUTD/run.$1"
	mkdir -p "$o"
	env VMCTX_SHARED=1 PORT=$((${PORT_BASE:-27000} + $2 * 4)) OUT="$o" KO="$KO" \
	    VMREMOTE="$VMREMOTE" VMHOME="$VMHOME" \
	    VMREMOTE_ENV="VMR_WATCHWORD=$WATCH ${VMR_EXTRA:-}" \
	    VMHOME_ENV="${VMHOME_EXTRA:-}" \
	    VMR_LIMIT=${VMR_LIMIT:-60} \
	    ./local-here.sh "$TESTS/$T" > "$o/run.log" 2>&1
}
declare -a slot_pid all_pids
launch() { local s; while :; do for s in $(seq 0 $((J-1))); do
	if [ -z "${slot_pid[$s]:-}" ] || ! kill -0 "${slot_pid[$s]}" 2>/dev/null; then
		one "$1" "$s" & slot_pid[$s]=$!; all_pids+=($!); return; fi
	done; sleep 0.2; done; }

cow0=$(cat /sys/module/kernel/parameters/vmctx_take_cow_broken 2>/dev/null); cow0=${cow0:-0}
t0=$(date +%s)
for i in $(seq 1 "$N"); do launch "$i"; done
# By pid, not a bare `wait': a bare wait also waits for anything else this shell
# started, which has hung a batch for ever before now.
wait "${all_pids[@]}" 2>/dev/null
t1=$(date +%s)

cow1=$(cat /sys/module/kernel/parameters/vmctx_take_cow_broken 2>/dev/null); cow1=${cow1:-0}
pass=0; lost=0; heap=0; tmo=0; hang=0; died=0; other=0; storm=0
deadline=0
for i in $(seq 1 "$N"); do
	o="$OUTD/run.$i"; [ -d "$o" ] || continue
	dl=$(grep -ac 'ended with exit status 131' "$o/remote.log" 2>/dev/null); dl=${dl:-0}
	deadline=$((deadline + dl))

	z=$(grep -a WATCHWORD "$o/remote.log" 2>/dev/null |
	    grep -acE ' [0-9]+=0( |$)'); z=${z:-0}
	if grep -qaE "$OK" "$o/guest.out" 2>/dev/null && [ "$z" = 0 ]; then
		# A passing run's counters are the CONTROL for whatever the
		# failing runs show, and deleting them made every correlation
		# unmeasurable (the give-up count was read off failing runs
		# with no base rate to compare against). One line per run.
		{ printf 'pass run.%s: ' "$i"
		  grep -ah 'simultaneous claims\|GIVEUP-LANDED' "$o/remote.log" \
			2>/dev/null | tr '\n' '; '
		  grep -ah 'claims ended by a sibling' "$o/home.log" \
			2>/dev/null | tr '\n' '; '; echo; } >> "$OUTD/pass-evidence.txt"
		pass=$((pass+1))
		# A passing run that carried a claim STORM (simultaneous claims
		# yielded to the source > 0) is kept whole, as storm.N: the storm
		# is the "2s race" (session 27), and the one line above cannot
		# say WHICH of the source's CLAIMING sites sustained it -- the
		# source's own "state set at vmhome.c:LINE" lines can, and they
		# were being deleted with the run.
		if grep -ah 'simultaneous claims' "$o/remote.log" 2>/dev/null |
		   grep -qvE ' 0 simultaneous claims'; then
			storm=$((storm+1)); mv "$o" "$OUTD/storm.$i"
		elif [ "${KEEP_PASS:-0}" = 1 ]; then
			# The CONTROL arm, whole. A census of which mechanisms
			# fired in the runs that passed is what tells a producer
			# from a bystander (session 29: the refused-take RESTORE
			# fired in 111 of 249 passing hx2 runs, which no failing
			# run's log alone could have said). One pg3 run is ~12 MB
			# and 256 of them fit on /home; never on /tmp.
			mv "$o" "$OUTD/pass.$i"
		else
			rm -rf "$o"
		fi
		continue
	fi
	rc=$(sed -n 's/.*=== exit \([0-9]*\).*/\1/p' "$o/run.log" 2>/dev/null | head -1)
	if   [ "$z" -gt 0 ]; then lost=$((lost+1));  mv "$o" "$OUTD/lost.$i"
	elif grep -qa sysmalloc "$o/guest.err" 2>/dev/null; then
		heap=$((heap+1)); mv "$o" "$OUTD/heap.$i"
	elif grep -qa 'remote GET of' "$o/home.log" 2>/dev/null; then
		tmo=$((tmo+1));  mv "$o" "$OUTD/tmo.$i"
	elif [ "${rc:-0}" = 124 ]; then hang=$((hang+1)); mv "$o" "$OUTD/hang.$i"
	elif [ "${rc:-0}" = 242 ] || [ "${rc:-0}" -gt 128 ] 2>/dev/null; then
		died=$((died+1)); mv "$o" "$OUTD/died.$i"
	else other=$((other+1)); mv "$o" "$OUTD/other.$i"; fi
done

echo "$T x$N, $J at a time, $((t1-t0))s"
printf 'pass %d (%d of them with a claim storm, kept as storm.N) | lost %d  heap %d  tmo %d  hang %d  died %d  other %d\n' \
	"$pass" "$storm" "$lost" "$heap" "$tmo" "$hang" "$died" "$other"
echo "mechanism: $deadline context(s) killed by the fault deadline," \
     "$((cow1-cow0)) copy-on-write share(s) the kernel broke at the take"
echo "failures kept in $OUTD"
[ $((lost+heap+tmo+hang+died+other)) = 0 ]
