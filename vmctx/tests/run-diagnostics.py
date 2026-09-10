#!/usr/bin/env python3
"""Reject monitor failures even when the guest prints PASS and exits zero.

Apply to ordinary integration runs. Deliberate fault-injection controls have
their own expected-diagnostic checks and must not use this success oracle.
"""
import argparse
import json
from pathlib import Path
import re

FATAL = re.compile(
    r"\[vm(?:home|remote)\] FATAL:|cannot reach the guest's page service|"
    r"page channel failed and did not recover|"
    r"Ending the context rather than zero-filling live memory|"
    r"executor ended before source context|"
    r"source adapter failed|admission failed source=|"
    r"could not install 0x[0-9a-f]+ fetched from the guest's machine|"
    r"page recall failed for context|STALE-SERVE 0x|"
    r"unsettled page receipt at scope exit|page acknowledgement unconfirmed|"
    r"unresolved source .*; stopping before any ownership or permission grant|"
    r"diagnostic loss:|"
    r"settle failed:|STALE SERVE of|page acknowledgement send failed|"
    r"finishing an unowned page transfer|cannot close inherited monitor descriptors|"
    r"BUG:|WARNING:|Oops:|Bad page state|Kernel panic|"
    r"rcu:.*stall|soft lockup|hard LOCKUP|"
    r"ending context after elapsed fault deadline"
)
COUNTERS = [
    re.compile(r"([1-9][0-9]*) terminal with the record saying the page exists"),
    re.compile(r"([1-9][0-9]*) were on neither machine and ended their context"),
    re.compile(r"([1-9][0-9]*) were dropped with no sibling to take them"),
    re.compile(r"STALE-SERVE report\(s\) from the guest's machine: ([1-9][0-9]*)"),
    re.compile(r"([1-9][0-9]*) syscall\(s\) were NOT forwarded"),
    re.compile(r"\(([1-9][0-9]*) broken channel\)"),
    re.compile(r"([1-9][0-9]*) context\(s\) ended on their own with a bad status"),
]


def failures(path):
    result = []
    for number, line in enumerate(Path(path).read_text(errors="replace").splitlines(), 1):
        if FATAL.search(line) or any(pattern.search(line) for pattern in COUNTERS):
            result.append(dict(path=str(path), line=number, diagnostic=line))
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("logs", type=Path, nargs="+")
    args = parser.parse_args()
    problems = [problem for path in args.logs for problem in failures(path)]
    print(json.dumps(dict(pass_=not problems, failures=problems), indent=2))
    return bool(problems)


if __name__ == "__main__":
    raise SystemExit(main())
