#!/usr/bin/env python3
"""Launch runany-x on the source, retaining both hosts' process ownership."""
import json
import os
from pathlib import Path
import shlex
import signal
import subprocess
import sys
import uuid


def main():
    if len(sys.argv) < 3:
        raise SystemExit('usage: xrun.sh <tag> <program-on-source> [args...]')
    tag, program, *arguments = sys.argv[1:]
    source = os.environ.get('SRC', '10.0.0.229')
    source_user = os.environ.get('SRC_USER', 'biwu')
    source_tree = os.environ.get('SRC_VMCTX', '/home/biwu/lan-boot/vmctx')
    owner = os.environ.get('SOURCE_NATIVE_RUN', '/usr/local/bin/vmctx-native-run')
    limit = int(os.environ.get('VMR_LIMIT', '60'))
    if not 0 < limit <= 3400:
        raise ValueError('VMR_LIMIT must be 1..3400 seconds')
    # Never reuse a source output name, even if a caller reuses its local tag.
    identity = 'xrun-' + uuid.uuid4().hex
    out = Path(os.environ.get('OUT', '/tmp/xr/' + tag)).resolve()
    out.mkdir(parents=True, exist_ok=False)
    remote_out = '/tmp/tr/' + identity
    remote_report = '/tmp/vmctx-' + identity + '.json'
    root = Path(__file__).resolve().parents[2]
    ssh = ['ssh', '-i', os.environ.get('KEY', str(root / 'config/id_vmctx')),
           '-o', 'BatchMode=yes', '-o', 'StrictHostKeyChecking=no',
           '-o', 'UserKnownHostsFile=/dev/null', '-o', 'ConnectTimeout=8',
           '-o', 'ServerAliveInterval=5', '-o', 'ServerAliveCountMax=3',
           '-o', 'LogLevel=ERROR', source_user + '@' + source]

    def privileged(command):
        if source_user == 'root': return command
        return ('SUDO_PASS=biwu SUDO_ASKPASS=$HOME/askpass.sh sudo -A sh -c ' +
                shlex.quote(command))

    environment = dict(VMR_XLIMIT=str(limit), SRC_IP=source,
                       DST=os.environ.get('DST', '10.0.0.30'),
                       KEY=os.environ.get('SOURCE_KEY', source_tree + '/../config/id_vmctx'))
    # Explicit paths select the deployment; never overwrite running binaries.
    for name in ('KO', 'VMHOME', 'PORT', 'REMOTE_VMREMOTE', 'REMOTE_NATIVE_RUN'):
        if name in os.environ: environment[name] = os.environ[name]
    command = shlex.join([owner, '--control-stdin', '--report', remote_report,
        str(limit + 180), 'env', *[k + '=' + v for k, v in environment.items()],
        'bash', source_tree + '/tests/runany-x.sh', identity, program, *arguments])
    result = dict(success=False, source=source, output=remote_out,
                  source_report=remote_report, requested_tag=tag)
    (out / 'run.json').write_text(json.dumps(result, indent=2) + '\n')
    process = None

    def interrupted(signum, _frame):
        raise InterruptedError('launcher interrupted by signal ' + str(signum))

    signal.signal(signal.SIGTERM, interrupted)
    signal.signal(signal.SIGINT, interrupted)
    try:
        with (out / 'transport.log').open('wb') as log:
            process = subprocess.Popen(ssh + [privileged('exec ' + command)],
                                       stdin=subprocess.PIPE, stdout=log, stderr=subprocess.STDOUT)
            result['status'] = process.wait(timeout=limit + 200)
    except (OSError, subprocess.SubprocessError, InterruptedError) as exc:
        result['error'] = str(exc)
    finally:
        signal.signal(signal.SIGTERM, signal.SIG_IGN)
        signal.signal(signal.SIGINT, signal.SIG_IGN)
        try:
            if process is not None:
                process.stdin.close()
                result['status'] = process.wait(timeout=20)
            report = subprocess.check_output(ssh + [privileged('cat ' + shlex.quote(remote_report))],
                                             text=True, timeout=20)
            cleanup = json.loads(report)
            (out / 'source-cleanup.json').write_text(report)
            result['source_cleanup'] = cleanup
            # Keep root-owned guest output intact, without following archive
            # links or deleting remote evidence during failure handling.
            with (out / 'source-output.tar.gz').open('xb') as archive:
                subprocess.run(ssh + [privileged('tar -C ' + shlex.quote(remote_out) + ' -czf - .')],
                               stdout=archive, check=True, timeout=120)
            destination = json.loads(subprocess.check_output(ssh + [privileged(
                'cat ' + shlex.quote(remote_out + '/destination-cleanup.json'))], text=True, timeout=20))
            result['destination_cleanup'] = destination
            result['success'] = (not result.get('error') and
                result.get('status') == cleanup['status'] == 0 and
                all(row['status'] == 0 and row['complete'] and not row['survivors'] and not row['timed_out'] and
                    not row['interrupted'] for row in (cleanup, destination)))
        except (OSError, subprocess.SubprocessError, ValueError, KeyError) as exc:
            result['cleanup_error'] = str(exc)
        (out / 'result.json').write_text(json.dumps(result, indent=2) + '\n')
    print(json.dumps(result, indent=2))
    return 0 if result['success'] else 1


if __name__ == '__main__':
    raise SystemExit(main())
