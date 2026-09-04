#!/bin/bash
# census.sh <torture-dir> -- which mechanisms fired, in the runs that passed
# and in the runs that failed, from a pool torture.sh kept whole (KEEP_PASS=1).
#
# A failing run's log alone cannot convict a mechanism: every tie-break in
# this tree fires in passing runs too, and the question is always the RATE in
# each arm. This reads the end-of-run summaries of both halves (home.log and
# remote.log) for every kept run, grades the run by guest.out (^PASS$ -- never
# by torture.sh's WATCHWORD grade, which mis-files every hx2 run as "lost"),
# and prints per-counter sums and the number of runs in which the counter was
# non-zero, for the passing and the failing runs separately.
#
#   ./tests/census.sh /home/biwu/torture.hx2.s29a
set -u
D=${1:?usage: census.sh <torture-dir>}
[ -d "$D" ] || { echo "no such dir: $D" >&2; exit 2; }

# name | file (h=home.log r=remote.log) | kind (n = a number captured by the
# sed expression's \1; c = a count of matching lines) | expression
COUNTERS='
restored|h|n|s/.*\[vmhome\] \([0-9]*\) page(s) put back after a refused take.*/\1/p
restore_stale|h|n|s/.*restores suppressed because a hand-over completed mid-refusal (the leftover was stale): \([0-9]*\).*/\1/p
intransit_healed|h|n|s/.*and \([0-9]*\) IN TRANSIT records aged past the 100ms grace.*/\1/p
intransit_stuck|h|n|s/.*\([0-9]*\) IN TRANSIT record(s) stayed unacked past the bound.*/\1/p
gate_fetched|h|n|s/.*fetch gate (one fetch per address space and page): \([0-9]*\) fetches claimed it.*/\1/p
gate_joined|h|n|s/.*fetch gate (one fetch per address space and page): [0-9]* fetches claimed it, \([0-9]*\) faults were finished.*/\1/p
gate_full|h|n|s/.*without a fetch of their own, \([0-9]*\) ran ungated.*/\1/p
gate_slow|h|n|s/.*ran ungated (no slot), \([0-9]*\) waits outlived.*/\1/p
landed_unexpected|h|n|s/.*; \([0-9]*\) gated fetches found the page ALREADY PRESENT.*/\1/p
prot_faults|h|n|s/.*must be 0); \([0-9]*\) protection faults reached the monitor.*/\1/p
get_inflight|h|n|s/.*GETs that crossed a serve of the same page (the far side answered .token in transit, ask again.): \([0-9]*\).*/\1/p
get_inflight_expired|h|n|s/.*live memory; \([0-9]*\) of them outlived the 1.5 s re-ask bound.*/\1/p
chan_lost|h|n|s/.*page-channel failures during a fault GET: [0-9]* recovered on a fresh connection, \([0-9]*\) terminal.*/\1/p
absent_lost|h|n|s/.*and \([0-9]*\) were on neither machine and ended their context.*/\1/p
absent_filled_lines|h|c|answered ABSENT and filled
pull_via_sibling|h|n|s/.*pulls-for-a-write whose named context could not take the install: \([0-9]*\) landed through a live sibling.*/\1/p
pull_lost|h|n|s/.*landed through a live sibling of the same mm, \([0-9]*\) were dropped with no sibling.*/\1/p
pull_returned|h|n|s/.*whose page was GIVEN BACK to the guest.s machine: \([0-9]*\).*/\1/p
fault_poke_failed|h|n|s/.*fault-path installs that failed in the faulting context: \([0-9]*\).*/\1/p
refused_but_here|h|n|s/.*requests refused because the record said the page was handed over, where this side was holding it anyway: \([0-9]*\).*/\1/p
sweep_pairs|h|n|s/.*the ownership sweep ran [0-9]* times and found \([0-9]*\) entr.*/\1/p
took_midflight|h|n|s/.*serves whose take found nothing because a concurrent serve took the page mid-flight: \([0-9]*\).*/\1/p
serve_gone_inflight|h|n|s/.*GONE-record refusals answered CLAIMING because this side.s own pull of the page was in flight: \([0-9]*\).*/\1/p
intransit_refused|h|n|s/.*acks ignored (the page had moved on), \([0-9]*\) asks answered CLAIMING while the page was on the wire.*/\1/p
take_gaveup|h|n|s/.*takes given up after a full second: \([0-9]*\).*/\1/p
retained_served|r|c|serving the retained copy
retained_alive|r|c|holder alive
gen_stale|r|n|s/.*take generation: [0-9]* bump(s), [0-9]* served page(s) checked against the count, \([0-9]*\) STALE.*/\1/p
gen_repaired|r|n|s/.*34:[0-9]*), \([0-9]*\) repaired from the retained copy.*/\1/p
gen_repaired_obj|r|n|s/.*of the stale serves, \([0-9]*\) were repaired from this side.s OWN live folio.*/\1/p
gen_unrepaired|r|n|s/.*repaired from the retained copy, \([0-9]*\) installed as they came.*/\1/p
inflight_pull|r|n|s/.*GETs answered INFLIGHT because this side.s own pull of the page was in flight past the 500ms wait.*: \([0-9]*\).*/\1/p
inflight_alive|r|n|s/.*GETs answered INFLIGHT for a LIVE holder with no landing in flight.*: \([0-9]*\).*/\1/p
lost_race_absent|r|n|s/.*never served their pre-wait read; \([0-9]*\) answered ABSENT.*/\1/p
claim_yields|r|n|s/.*; \([0-9]*\) simultaneous claims yielded to the source, \([0-9]*\) gave up waiting.*/\1/p
claim_giveups|r|n|s/.*; \([0-9]*\) simultaneous claims yielded to the source, \([0-9]*\) gave up waiting.*/\2/p
pull_overlap|r|n|s/.*pulls that began for a page ALREADY in flight to another context of the same address space: \([0-9]*\).*/\1/p
lost_store_lines|r|c|LOST-STORE
ownf_claimed|r|n|s/.*ownership channel (fault claim): \([0-9]*\) fault(s) claimed the (as,page) service.*/\1/p
ownf_waited|r|n|s/.*ownership channel (fault claim): [0-9]* fault(s) claimed the (as,page) service, \([0-9]*\) found a sibling.*/\1/p
ownf_woken|r|n|s/.*WAITED on the landing, \([0-9]*\) of those woken by it.*/\1/p
ownf_wake_absent|r|n|s/.*of those woken by it (\([0-9]*\) woke to a page still absent).*/\1/p
ownf_timeout|r|n|s/.*woke to a page still absent), \([0-9]*\) outlived the 3s bound.*/\1/p
ownf_full|r|n|s/.*ran UNGATED (must be ~0), \([0-9]*\) entry-table full.*/\1/p
ownf_landed|r|n|s/.*entry-table full (ungated); released: \([0-9]*\) landed with the page present.*/\1/p
ownf_aborted|r|n|s/.*landed with the page present, \([0-9]*\) aborted still absent.*/\1/p
own_begin_busy|r|n|s/.*ownership channel (substrate): begin=[0-9]* begin_busy=\([0-9]*\).*/\1/p
own_wait_timeout|r|n|s/.*ownership channel (substrate): .* timeout=\([0-9]*\) full=.*/\1/p
fatal_retained_lines|r|c|was on neither machine, but its last transmitted bytes were retained
deadline_kills|r|c|ended with exit status 131
decl_known|r|n|s/.*faults declined, by the branch that declined them: [0-9]*, of which \([0-9]*\) were a page this side had installed or lent.*/\1/p
'

declare -A psum pcnt fsum fcnt
names=()
while IFS='|' read -r name file kind expr; do
	[ -n "$name" ] || continue
	names+=("$name")
	psum[$name]=0; pcnt[$name]=0; fsum[$name]=0; fcnt[$name]=0
done <<< "$COUNTERS"

# The per-run lines, beside the pool when the pool is writable (it is root's
# when torture.sh ran under sudo), otherwise in a temp file named at the end.
OUT=$D/census.txt
touch "$OUT.tmp" 2>/dev/null || OUT=$(mktemp /tmp/census.XXXXXX)
: > "$OUT.tmp"

npass=0; nfail=0; nrun=0
for d in "$D"/*/; do
	d=${d%/}
	[ -f "$d/remote.log" ] || continue
	nrun=$((nrun + 1))
	if grep -qax PASS "$d/guest.out" 2>/dev/null; then
		k=pass; npass=$((npass + 1))
	else
		k=fail; nfail=$((nfail + 1))
	fi
	line="$k $(basename "$d")"
	while IFS='|' read -r name file kind expr; do
		[ -n "$name" ] || continue
		f=$d/home.log; [ "$file" = r ] && f=$d/remote.log
		if [ "$kind" = c ]; then
			v=$(grep -ac -- "$expr" "$f" 2>/dev/null)
		else
			v=$(sed -n "$expr" "$f" 2>/dev/null | tail -1)
		fi
		[ -n "$v" ] || v=-
		line="$line $name=$v"
		if [ "$v" != - ]; then
			if [ $k = pass ]; then
				psum[$name]=$(( ${psum[$name]} + v ))
				[ "$v" -gt 0 ] && pcnt[$name]=$(( ${pcnt[$name]} + 1 ))
			else
				fsum[$name]=$(( ${fsum[$name]} + v ))
				[ "$v" -gt 0 ] && fcnt[$name]=$(( ${fcnt[$name]} + 1 ))
			fi
		fi
	done <<< "$COUNTERS"
	echo "$line" >> "$OUT.tmp"
done
mv -f "$OUT.tmp" "$OUT"

echo "== $D: $nrun runs kept, $npass pass, $nfail fail (graded by guest.out)"
printf '%-22s %10s %8s   %10s %8s\n' counter pass_sum pass_runs fail_sum fail_runs
for name in "${names[@]}"; do
	[ "${psum[$name]}" = 0 ] && [ "${fsum[$name]}" = 0 ] && continue
	printf '%-22s %10d %8d   %10d %8d\n' "$name" "${psum[$name]}" \
		"${pcnt[$name]}" "${fsum[$name]}" "${fcnt[$name]}"
done
echo "(counters that were zero or absent in every run are not listed; per-run lines in $OUT)"
echo "-- failing runs:"
grep '^fail' "$OUT" 2>/dev/null | sed 's/ [a-z_]*=0//g; s/ [a-z_]*=-//g'
