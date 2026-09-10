#!/usr/bin/env python3
"""Verify normal exit and interruption reap detached, forked descendants."""
import json
import os
from pathlib import Path
import signal
import subprocess
import sys
import tempfile
import time


def main():
    harness = Path(__file__).with_name('owned-run.py')
    process = None
    sentinel = subprocess.Popen([sys.executable, '-c', 'import time; time.sleep(60)'])
    try:
        with tempfile.TemporaryDirectory(prefix='vmctx-owned-') as directory:
            root = Path(directory)
            fixture = root / 'fork.py'
            fixture.write_text('''import os, pathlib, sys, time
if os.fork() == 0:
    os.setsid()
    pending = pathlib.Path(sys.argv[1] + '.pending')
    pending.write_text(str(os.getpid()))
    pending.replace(sys.argv[1])
    time.sleep(60)
else:
    while not pathlib.Path(sys.argv[1]).exists(): time.sleep(.005)
    if sys.argv[2] == 'wait': time.sleep(60)
    sys.exit(7)
''')
            for mode in ('exit', 'wait', 'timeout'):
                pidfile, report = root / (mode + '.pid'), root / (mode + '.json')
                options = ['--timeout', '.5', '--timeout-marker', str(root/'timeout.marker')] if mode == 'timeout' else []
                process = subprocess.Popen([sys.executable, str(harness), '--report',
                    str(report), *options, sys.executable, str(fixture), str(pidfile), 'exit' if mode == 'exit' else 'wait'])
                until = time.monotonic() + 5
                while not pidfile.exists():
                    assert process.poll() is None and time.monotonic() < until
                    time.sleep(.01)
                descendant = int(pidfile.read_text())
                if mode == 'wait':
                    process.send_signal(signal.SIGTERM)
                assert process.wait(timeout=15) == {'exit': 7, 'wait': 143, 'timeout': 124}[mode]
                result = json.loads(report.read_text())
                assert result['complete'] and str(descendant) in result['signalled'], result
                assert result['timed_out'] == (mode == 'timeout'), result
                if mode == 'timeout': assert (root/'timeout.marker').is_file()
                assert not Path('/proc', str(descendant)).exists()
                assert sentinel.poll() is None, 'unrelated process was signalled'
        print('PASS: detached fork cleanup, interruption, deadline, exit status, unrelated process isolation')
    finally:
        if process is not None and process.poll() is None:
            process.send_signal(signal.SIGTERM)
            process.wait(timeout=20)
        sentinel.kill()
        sentinel.wait()


if __name__ == '__main__':
    main()
