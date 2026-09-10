#!/bin/bash
# xrun.sh <tag> <program-on-source> [args...]
# The source owns the program and its files; only connection coordinates go
# to the executor. Both hosts need native-run with --control-stdin/--report.
set -eu
HERE=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
exec python3 "$HERE/tests/cross-run.py" "$@"
