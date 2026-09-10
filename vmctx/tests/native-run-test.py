#!/usr/bin/env python3
"""Exercise native-run status, detached descendants, timeout and SSH control EOF.

Run beneath owned-run.py so a failing baseline's orphans are also reaped.
Arguments name the owner and the compiled control/native-run-child.c fixture.
"""
import json
import os
from pathlib import Path
import signal
import subprocess
import sys
import tempfile
import time


def main():
    owner, fixture = map(str, map(Path.resolve, map(Path, sys.argv[1:3])))
    rows = []
    with tempfile.TemporaryDirectory(prefix='native-run-test-') as directory:
        root = Path(directory)
        sentinel = subprocess.Popen(['sleep', '60'])
        try:
            for mode in ('exit', 'eof', 'data', 'signal', 'deadline'):
                report, pidfile = root / (mode + '.json'), root / (mode + '.pid')
                process = subprocess.Popen([owner, '--control-stdin', '--report', str(report),
                    '2' if mode == 'deadline' else '20', fixture, str(pidfile),
                    'exit' if mode == 'exit' else 'wait'], stdin=subprocess.PIPE)
                try:
                    until = time.monotonic() + 5
                    while not pidfile.exists() or not pidfile.read_text().strip():
                        assert process.poll() is None and time.monotonic() < until
                        time.sleep(.005)
                    # Complete data is published before the leader can exit;
                    # a terminal owner report is the proof of reaping.
                    if mode == 'eof': process.stdin.close()
                    if mode == 'data': process.stdin.write(b'stop\n'); process.stdin.flush()
                    if mode == 'signal': process.send_signal(signal.SIGTERM)
                    status = process.wait(timeout=10)
                    row = json.loads(report.read_text())
                    assert row['complete'] and not row['survivors'], row
                    assert row['status'] == status == (7 if mode == 'exit' else 124), row
                    assert row['timed_out'] == (mode == 'deadline'), row
                    assert row['interrupted'] == (0 if mode == 'exit' else 14 if mode == 'deadline' else 15), row
                    assert sentinel.poll() is None, 'unrelated process was signalled'
                    rows.append(dict(mode=mode, **row))
                finally:
                    if not process.stdin.closed: process.stdin.close()
                    if process.poll() is None:
                        process.send_signal(signal.SIGTERM)
                        process.wait(timeout=10)
        finally:
            sentinel.terminate()
            sentinel.wait(timeout=5)
    print(json.dumps(rows, indent=2))


if __name__ == '__main__':
    main()
