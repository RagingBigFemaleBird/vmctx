#!/usr/bin/env python3
"""Run the executor through SSH with a destination-owned cleanup report.

Called by local-here.sh in place of vmremote. The source retains its normal
subreaper; the destination needs the native-run control built with --report.
Only the connection address crosses the wire, never the guest program path.
"""
import json
import os
from pathlib import Path
import shlex
import subprocess
import sys
import uuid


def main():
    root = Path(__file__).resolve().parents[2]
    out = Path(os.environ['OUT'])
    report = out / 'destination-cleanup.json'
    report.unlink(missing_ok=True)
    destination = os.environ.get('DST', '10.0.0.30')
    if '@' not in destination:
        destination = 'root@' + destination
    ssh = ['ssh', '-i', os.environ.get('KEY', str(root / 'config/id_vmctx')),
           '-o', 'BatchMode=yes', '-o', 'StrictHostKeyChecking=no',
           '-o', 'UserKnownHostsFile=/dev/null', '-o', 'ConnectTimeout=8',
           '-o', 'ServerAliveInterval=5', '-o', 'ServerAliveCountMax=3',
           '-o', 'LogLevel=ERROR', destination]
    binary = os.environ.get('REMOTE_VMREMOTE', '/usr/local/bin/vmremote')
    owner = os.environ.get('REMOTE_NATIVE_RUN', '/usr/local/bin/vmctx-native-run')
    limit = int(os.environ['VMR_REMOTE_LIMIT'])
    if not 0 < limit <= 3600:
        raise ValueError('destination timeout must be 1..3600 seconds')
    port = int(sys.argv[1].rsplit(':', 1)[1])
    address = os.environ.get('SRC_IP', '10.0.0.229') + ':' + str(port)
    directory = '/tmp/vmctx-cross-' + uuid.uuid4().hex
    subprocess.run(ssh + ['mkdir -m 700 ' + shlex.quote(directory)], check=True, timeout=15)
    remote_report = directory + '/cleanup.json'
    command = shlex.join([owner, '--control-stdin', '--report', remote_report, str(limit),
                         binary, address, *sys.argv[2:]])
    status = None
    result = {'complete': False, 'error': 'destination report missing',
              'directory': directory, 'destination': destination}
    process = None
    try:
        process = subprocess.Popen(ssh + ['exec ' + command], stdin=subprocess.PIPE)
        status = process.wait(timeout=limit + 15)
    finally:
        try:
            if process is not None:
                process.stdin.close()
                if status is None:
                    status = process.wait(timeout=15)
            raw = subprocess.check_output(ssh + ['cat ' + shlex.quote(remote_report)],
                                          timeout=15, text=True)
            result = json.loads(raw)
            result['transport_status'] = status
            result['complete'] &= status is not None and result['status'] == status
            report.write_text(json.dumps(result, indent=2) + '\n')
            if result['complete']:
                subprocess.run(ssh + ['rm ' + shlex.quote(remote_report) +
                                     ' && rmdir ' + shlex.quote(directory)],
                               check=True, timeout=15)
        finally:
            report.write_text(json.dumps(result, indent=2) + '\n')
    if not result['complete']:
        return 125
    if result['timed_out'] or result['interrupted']:
        (out / '.timedout').touch()
        return 124
    return status


if __name__ == '__main__':
    raise SystemExit(main())
