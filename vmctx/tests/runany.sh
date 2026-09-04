#!/bin/bash
# runany.sh <tag> <program> [args...] — run one program under vmctx, both ends
# on this machine, and leave its output where suite.sh looks for it.
#
# This lived only on 10.0.0.229 as /home/biwu/runany.sh, which is why suite.sh
# could not run one case from a fresh checkout: $RUNANY named a file that was
# not in the repository, and neither were three of the cases. Committed here,
# with the paths derived rather than written out, so it is the same script on
# every machine.
#
# The contract suite.sh depends on is exactly: run the program, and leave the
# guest's stdout at /tmp/tr/<tag>/guest.out. The rest — run.log, home.log,
# remote.log — is what makes a failure readable afterwards.
set -u
T=${1:?usage: runany.sh <tag> <program> [args...]}
shift
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
OUT=/tmp/tr/$T

# The run writes here directly rather than into a shared directory that is
# copied out afterwards. Two reasons, and the second is the one that matters:
# a copy leaves the previous case's output in place when a run dies before
# local-here.sh reaches its own rm, so the case is graded against it; and one
# shared directory cannot be used by two runs at once, which is the whole of
# what this script is for now.
mkdir -p "$OUT"
rm -f "$OUT"/guest.out "$OUT"/home.log "$OUT"/remote.log "$OUT"/run.log \
      "$OUT"/.timedout
# Said out loud, not graded around: if this user cannot write the output
# directory, the run's redirects fail silently and suite.sh then grades
# whatever a PREVIOUS run left there. Measured: one suite run under sudo
# left /tmp/tr root-owned, and the next three suites "passed" 105 cases
# against its stale outputs while every fresh run's output went nowhere --
# with the one case whose stale file was empty reported NO-OUTPUT, which is
# what pointed here. Refusing with runany's own harness-fault status makes
# the whole suite say NO-OUTPUT instead of lying quietly.
if [ ! -w "$OUT" ] || [ -e "$OUT"/guest.out ]; then
	echo "runany: cannot claim $OUT (unwritable, or leftovers this user" \
	     "cannot remove -- a previous run under another user?)" >&2
	exit 3
fi

cd "$HERE" || exit 1

# The module belongs to the person who built it, and this runs under sudo, where
# $HOME is /root. Defaulting to $HOME/vmctx-*.ko therefore named a file that does
# not exist, local-here.sh refused to start, and every case in the suite failed
# with an empty run.log -- which looks exactly like the whole system being
# broken. Fall back to the invoking user's home before $HOME.
if [ -z "${KO:-}" ]; then
	for k in "${SUDO_USER:+/home/$SUDO_USER}/vmctx-7.0.14.ko" \
		 "$HOME/vmctx-7.0.14.ko"; do
		[ -f "$k" ] && { KO=$k; break; }
	done
fi
if [ -z "${KO:-}" ] || [ ! -f "$KO" ]; then
	echo "== $T SKIPPED: no vmctx module found (set KO=)" >&2
	exit 2
fi

# A port of this run's own, so several may be in flight at once. PORT+1 is the
# page channel, so they are spaced by four; the caller passes a slot when it is
# running a pool, and a lone run keeps the old default.
PORT=${PORT:-9998}

KO="$KO" PORT="$PORT" OUT="$OUT" VMR_LIMIT=${VMR_LIMIT:-45} \
	./local-here.sh "$@" > "$OUT/run.log" 2>&1
rc=$?
chmod 644 "$OUT"/* 2>/dev/null
echo "== $T rc=$rc guest.out=$(wc -c < "$OUT/guest.out" 2>/dev/null || echo 0) lines=$(wc -l < "$OUT/guest.out" 2>/dev/null || echo 0)"

# This script's status is about the HARNESS, not about the program. $rc is the
# guest's own exit status, and several cases exit non-zero on purpose -- ex5
# returns 42 and pf3's children die by signal, so the guest exits 242. Reporting
# those as a harness fault makes the one signal that matters -- "the run never
# happened" -- indistinguishable from a test working as designed.
# 3: nothing ran, or it produced nothing.
[ -s "$OUT/guest.out" ] || exit 3
exit 0
