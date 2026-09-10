#!/usr/bin/env python3
"""Exercise the suite's actual grading functions with failed-run artifacts."""
import json
from pathlib import Path
import subprocess
import tempfile

suite = Path(__file__).with_name('suite.sh').read_text()
# Load the definitions without starting a build or any lab workloads.
functions = suite[:suite.index('run_case()')]
rows = []
with tempfile.TemporaryDirectory(prefix='vmctx-suite-oracle-') as tmp:
    base = Path(tmp)
    for scenario, records, expected_pass, expected_fail in [
        ('complete', 'one 1\ntwo 1\n', 2, 0),
        ('missing-worker', 'one 1\n', 1, 1),
        ('duplicate-worker', 'one 1\none 1\ntwo 1\n', 1, 1),
        ('malformed-worker', 'one PASS\ntwo 1\n', 1, 1),
    ]:
        (base/'results').write_text(records)
        (base/'kinds').write_text('one\ntwo\n')
        body = functions + '''
RESULTS=$1; KINDS=$2
finish_round
printf 'COUNTS %s %s\\n' "$pass" "$fail"
'''
        # suite's positional argument is ROUNDS during definition loading.
        body = 'results_path=$1; kinds_path=$2; set -- 0\n' + body.replace('RESULTS=$1; KINDS=$2', 'RESULTS=$results_path; KINDS=$kinds_path')
        r = subprocess.run(['bash', '-c', body, 'suite-oracle', str(base/'results'), str(base/'kinds')], capture_output=True, text=True, timeout=5)
        ok = r.returncode == 0 and f'COUNTS {expected_pass} {expected_fail}\n' in r.stdout
        rows.append(dict(scenario=scenario, passed=ok, stdout=r.stdout, stderr=r.stderr))
    # The zero-round build-only mode must reject a failed monitor build.
    (base/'tests').mkdir()
    (base/'user').mkdir()
    (base/'tests/suite.sh').write_text(suite)
    (base/'user/Makefile').write_text('.PHONY: ../build/vmremote-local ../build/vmhome\n../build/vmremote-local ../build/vmhome:\n\t@false\n')
    r = subprocess.run(['bash', str(base/'tests/suite.sh'), '0'], capture_output=True, text=True,
                       env={'PATH': '/usr/bin:/bin', 'TESTS': str(base/'bins')}, timeout=5)
    rows.append(dict(scenario='zero-round-build-failure', passed=r.returncode != 0,
                     status=r.returncode, stdout=r.stdout, stderr=r.stderr))
print(json.dumps(rows, indent=2))
raise SystemExit(not all(r['passed'] for r in rows))
