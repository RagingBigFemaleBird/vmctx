#!/bin/bash
# Run on the source. Keep suite.sh's grading and local-here.sh's source owner;
# remote-executor.py owns destination execution through native-run --report.
set -eu
T=${1:?usage: runany-x.sh <tag> <program> [args...]}
HERE=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
export OUT=/tmp/tr/$T
export VMR_REMOTE_LIMIT=${VMR_XLIMIT:-$(( ${VMR_LIMIT:-60} * 5 ))}
# Allow destination timeout, descendant reaping and report retrieval to finish
# before the source's executor owner expires.
export VMR_LIMIT=$((VMR_REMOTE_LIMIT + 45))
export VMREMOTE="$HERE/tests/remote-executor.py"
export VMCTX_SHARED=1
if test -e "$OUT/destination-cleanup.json"; then
    echo "runany-x: output already contains a destination report: $OUT" >&2
    exit 125
fi
rc=0
bash "$HERE/tests/runany.sh" "$@" || rc=$?
python3 - "$OUT/destination-cleanup.json" <<'CHECK' || exit 125
import json, sys
result = json.load(open(sys.argv[1]))
assert result['complete'] and not result['survivors'], result
assert not result['timed_out'] and not result['interrupted'], result
assert result['status'] == 0, result
CHECK
exit "$rc"
