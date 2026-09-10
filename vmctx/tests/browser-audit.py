#!/usr/bin/env python3
"""Native/loopback/cross-machine Firefox control with real X input and page acknowledgments.

Run on the source host. Each run owns its X server, fresh profile, HTTP server,
process sessions and output directory. No process-name kills or shared output.
The measured input latency includes X delivery, browser handling, two animation
frames and a local HTTP acknowledgment; it is not a compositor presentation time.
"""
import argparse
import ctypes
import hashlib
import http.server
import json
import os
from pathlib import Path
import select
import shlex
import signal
import socket
import subprocess
import threading
import time
import uuid


def proc_tree(root):
    tasks = {}
    for p in Path('/proc').iterdir():
        if not p.name.isdigit():
            continue
        try:
            text = (p / 'stat').read_text()
            fields = text[text.rfind(')') + 2:].split()
            tasks[int(p.name)] = (int(fields[1]), int(fields[19]))
        except (OSError, ValueError, IndexError):
            pass
    selected = {root}
    while True:
        grown = selected | {pid for pid, (ppid, _) in tasks.items() if ppid in selected}
        if grown == selected:
            return {pid: tasks[pid] for pid in selected if pid != root and pid in tasks}
        selected = grown


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('mode', choices=['native', 'loopback', 'cross'])
    parser.add_argument('--firefox', required=True, type=Path)
    parser.add_argument('--out', required=True, type=Path)
    parser.add_argument('--vmhome', type=Path, default=Path(__file__).resolve().parents[1] / 'build/vmhome')
    parser.add_argument('--vmremote', type=Path, default=Path(__file__).resolve().parents[1] / 'build/vmremote-local')
    parser.add_argument('--destination', help='SSH destination for cross mode')
    parser.add_argument('--remote-owner', default='/usr/local/bin/vmctx-native-run',
                        help='destination native-run with --report and --control-stdin')
    parser.add_argument('--identity', type=Path, help='SSH identity for cross mode')
    parser.add_argument('--source-address', help='source IPv4 address reachable from destination')
    parser.add_argument('--port', type=int, default=28610)
    parser.add_argument('--startup-timeout', type=float, default=120)
    parser.add_argument('--seconds', type=float, default=60)
    parser.add_argument('--quiet-source', action='store_true',
                        help='omit per-call source tracing while retaining errors and run counters')
    args = parser.parse_args()
    if args.mode != 'native' and os.geteuid() != 0:
        parser.error('guest modes require root for the service context')
    if not 1024 <= args.port < 32767:
        parser.error('protocol port pair must be below the ephemeral range')
    if args.seconds <= 0 or args.startup_timeout <= 0:
        parser.error('run duration and startup timeout must be positive')
    owner_limit = int(args.seconds + args.startup_timeout + 180)
    if owner_limit > 3600:
        parser.error('startup plus workload plus cleanup budget must fit in one hour')
    if args.mode == 'cross' and not all((args.destination, args.identity, args.source_address)):
        parser.error('cross mode requires --destination, --identity and --source-address')
    if args.mode == 'cross':
        socket.inet_pton(socket.AF_INET, args.source_address)
    args.out = args.out.resolve()
    args.out.mkdir(parents=True, exist_ok=False)
    profile = args.out / 'profile'
    profile.mkdir()
    # These preferences are supported by the tested Firefox distribution's
    # TelemetryReportingPolicy and aboutwelcome code. A fresh profile must not
    # put first-run chrome over the page receiving the measured X input.
    prefs = {'browser.aboutwelcome.enabled': False,
             'termsofuse.bypassNotification': True,
             'datareporting.policy.dataSubmissionPolicyBypassNotification': True,
             'datareporting.healthreport.uploadEnabled': False,
             'browser.shell.checkDefaultBrowser': False,
             'startup.homepage_welcome_url': '',
             'startup.homepage_welcome_url.additional': ''}
    (profile / 'user.js').write_text(''.join(
        f'user_pref({json.dumps(k)}, {json.dumps(v)});\n' for k, v in prefs.items()))
    # Keep all orphaned descendants attributable to this run for cleanup.
    if ctypes.CDLL(None, use_errno=True).prctl(36, 1, 0, 0, 0) != 0:
        raise OSError(ctypes.get_errno(), 'PR_SET_CHILD_SUBREAPER')

    token = uuid.uuid4().hex
    events = []
    event_lock = threading.Condition()
    page_bytes = Path(__file__).with_name('browser-workload.html').read_bytes()
    event_file = (args.out / 'events.jsonl').open('w')
    processes, workload_processes, files = [], [], [event_file]
    result = {'mode': args.mode, 'token': token, 'success': False, 'metrics': [],
              'clock': 'host monotonic', 'kernel': os.uname().release,
              'firefox': str(args.firefox), 'sandbox_disabled': True,
              'preferences': prefs,
              'harness_sha256': hashlib.sha256(Path(__file__).read_bytes()).hexdigest(),
              'workload_sha256': hashlib.sha256(page_bytes).hexdigest(),
              'display': 'isolated Xvfb, MIT-SHM disabled'}

    def counters():
        snapshot = {'host_s': time.monotonic(), 'parameters': {},
                    'module_parameters': {}, 'module_identity': {}}
        for path in Path('/sys/module/kernel/parameters').glob('vmctx_*'):
            try:
                snapshot['parameters'][path.name] = path.read_text().strip()
            except OSError:
                pass
        for path in Path('/sys/module/vmctx/parameters').glob('*'):
            try:
                snapshot['module_parameters'][path.name] = path.read_text().strip()
            except OSError:
                pass
        for name in ('srcversion', 'version', 'refcnt', 'taint'):
            try:
                snapshot['module_identity'][name] = Path('/sys/module/vmctx', name).read_text().strip()
            except OSError:
                pass
        for name in ('cpu', 'memory', 'io'):
            try:
                snapshot['pressure_' + name] = Path('/proc/pressure', name).read_text()
            except OSError:
                pass
        return snapshot

    remote_marker = '/tmp/vmctx-browser-' + token
    remote = None
    ssh = (['ssh', '-i', str(args.identity), '-o', 'BatchMode=yes',
            '-o', 'StrictHostKeyChecking=no', '-o', 'UserKnownHostsFile=/dev/null',
            '-o', 'ConnectTimeout=8', '-o', 'ServerAliveInterval=10',
            '-o', 'ServerAliveCountMax=3', '-o', 'LogLevel=ERROR', args.destination]
           if args.mode == 'cross' else [])

    def destination_run(script, timeout=15):
        return subprocess.run(ssh + ['sh -c ' + shlex.quote(script)],
                              capture_output=True, text=True, timeout=timeout, check=True).stdout

    def destination_snapshot():
        return destination_run('uname -a; cat /proc/sys/kernel/random/boot_id; '
            'for p in /sys/module/kernel/parameters/vmctx_* /sys/module/vmctx/parameters/* '
            '/sys/module/vmctx/refcnt /sys/module/vmctx/srcversion; do '
            '[ -f "$p" ] || continue; printf "%s=" "$p"; cat "$p"; done')

    def interrupted(signum, _frame):
        raise InterruptedError(f'run interrupted by signal {signum}')

    signal.signal(signal.SIGTERM, interrupted)
    signal.signal(signal.SIGINT, interrupted)

    class Handler(http.server.BaseHTTPRequestHandler):
        def log_message(self, *_):
            pass

        def do_GET(self):
            # Distinguish navigation reaching HTTP from script execution and
            # the page's ready acknowledgment during startup diagnosis.
            with event_lock:
                event = {'kind': 'http_request', 'path': self.path,
                         'host_s': time.monotonic()}
                events.append(event)
                event_file.write(json.dumps(event) + '\n')
                event_file.flush()
                event_lock.notify_all()
            self.send_response(200)
            self.send_header('Content-Type', 'text/html; charset=utf-8')
            self.send_header('Cache-Control', 'no-store')
            self.send_header('Content-Length', str(len(page_bytes)))
            self.end_headers()
            self.wfile.write(page_bytes)

        def do_POST(self):
            try:
                length = int(self.headers.get('Content-Length', 0))
            except ValueError:
                self.send_error(400)
                return
            if self.path != '/event' or not 0 < length <= 8192:
                self.send_error(400)
                return
            try:
                event = json.loads(self.rfile.read(length))
            except (ValueError, OSError):
                self.send_error(400)
                return
            if not isinstance(event, dict) or event.get('token') != token:
                self.send_error(403)
                return
            with event_lock:
                event['host_s'] = time.monotonic()
                events.append(event)
                event_file.write(json.dumps(event) + '\n')
                event_file.flush()
                event_lock.notify_all()
            self.send_response(204)
            self.end_headers()

    # One page produces small acknowledgments. A single serving thread also
    # lets teardown join every writer before closing the event log.
    class Server(http.server.HTTPServer):
        def get_request(self):
            connection, address = super().get_request()
            connection.settimeout(2)
            return connection, address

    server = Server(('127.0.0.1', 0), Handler)
    server_thread = threading.Thread(target=server.serve_forever, daemon=True)
    server_thread.start()
    env = dict(os.environ)
    env.update(HOME=str(args.out), MOZ_ENABLE_WAYLAND='0', GDK_BACKEND='x11',
               MOZ_DISABLE_CONTENT_SANDBOX='1', MOZ_DISABLE_RDD_SANDBOX='1',
               MOZ_DISABLE_GPU_SANDBOX='1', MOZ_DISABLE_GMP_SANDBOX='1',
               MOZ_CRASHREPORTER_DISABLE='1', NO_AT_BRIDGE='1')
    env.pop('XAUTHORITY', None)
    env.pop('DBUS_SESSION_BUS_ADDRESS', None)

    def launch(command, name, **kwargs):
        log = (args.out / (name + '.log')).open('wb')
        files.append(log)
        p = subprocess.Popen([str(x) for x in command], env=env,
                             stdin=kwargs.pop('stdin', subprocess.DEVNULL),
                             stdout=log, stderr=subprocess.STDOUT, start_new_session=True, **kwargs)
        processes.append(p)
        return p

    def event_after(kind, after, timeout, **expected):
        deadline = time.monotonic() + timeout
        with event_lock:
            while True:
                for name, process in workload_processes:
                    status = process.poll()
                    if status is not None:
                        raise RuntimeError(f'{name} exited {status} while waiting for {kind}')
                for event in events:
                    if (event['host_s'] >= after and event.get('kind') == kind and
                            all(event.get(k) == v for k, v in expected.items())):
                        return event
                left = deadline - time.monotonic()
                if left <= 0:
                    raise TimeoutError(f'no {kind} acknowledgment: {expected}')
                event_lock.wait(min(left, 0.5))

    def metric(name, started, event):
        row = {'kind': name, 'ms': (event['host_s'] - started) * 1000,
               'page': event['page']}
        result['metrics'].append(row)
        print(json.dumps(row), flush=True)

    def key(*parts):
        with (args.out / 'input-commands.jsonl').open('a') as trace:
            trace.write(json.dumps({'host_s': time.monotonic(), 'args': parts}) + '\n')
        subprocess.run(['xdotool', *parts], env=env, check=True, timeout=10,
                       stdout=subprocess.DEVNULL, stderr=subprocess.PIPE)

    try:
        rd, wr = os.pipe()
        try:
            launch(['Xvfb', '-displayfd', str(wr), '-screen', '0', '1280x1024x24',
                    '-extension', 'MIT-SHM', '-nolisten', 'tcp'], 'xvfb', pass_fds=(wr,))
            os.close(wr)
            wr = -1
            if not select.select([rd], [], [], 10)[0]:
                raise TimeoutError('Xvfb did not allocate a display')
            display = os.read(rd, 32).decode().strip()
            if not display.isdecimal():
                raise RuntimeError('Xvfb failed to start')
            env['DISPLAY'] = ':' + display
        finally:
            os.close(rd)
            if wr >= 0:
                os.close(wr)
        result['display_number'] = env['DISPLAY']
        url = f'http://127.0.0.1:{server.server_port}/?token={token}&page=0'
        firefox = [args.firefox.resolve(), '--no-remote', '--new-instance', '--profile', profile, url]
        result['before'] = counters()
        if args.mode == 'native':
            started = time.monotonic()
            workload_processes.append(('firefox', launch(['dbus-run-session', '--', *firefox], 'firefox')))
        else:
            for port in (args.port, args.port + 1):
                with socket.socket() as probe:
                    probe.bind(('127.0.0.1', port))
            result['builds'] = {'vmhome': subprocess.check_output(
                [str(args.vmhome), '--build-id'], text=True).strip()}
            if args.mode == 'cross':
                result['destination'] = args.destination
                result['destination_before'] = destination_snapshot()
                result['builds']['vmremote'] = destination_run(
                    shlex.quote(str(args.vmremote)) + ' --build-id').strip()
            else:
                result['builds']['vmremote'] = subprocess.check_output(
                    [str(args.vmremote), '--build-id'], text=True).strip()
            started = time.monotonic()
            home = launch(['dbus-run-session', '--', args.vmhome, str(args.port),
                           *([] if args.quiet_source else ['-v']), *firefox], 'home')
            until = time.monotonic() + 10
            while b'serving forwarded syscalls' not in (args.out / 'home.log').read_bytes():
                if home.poll() is not None or time.monotonic() >= until:
                    raise RuntimeError('vmhome did not start its listener')
                time.sleep(0.02)
            if args.mode == 'cross':
                remote_command = shlex.join([args.remote_owner, '--control-stdin',
                    '--report', remote_marker, str(owner_limit), str(args.vmremote),
                    f'{args.source_address}:{args.port}', '--net'])
                remote = launch(ssh + ['exec ' + remote_command], 'remote',
                                stdin=subprocess.PIPE)
            else:
                remote = launch([args.vmremote, f'127.0.0.1:{args.port}', '--net'], 'remote')
            workload_processes.extend([('home', home), ('remote', remote)])
        ready = event_after('ready', started, args.startup_timeout, page=0)
        metric('startup', started, ready)
        windows = subprocess.check_output(['xdotool', 'search', '--name', 'vmctx-audit-' + token],
                                          env=env, text=True, timeout=10).splitlines()
        if not windows:
            raise RuntimeError('page acknowledged but no titled X window exists')
        key('windowfocus', '--sync', windows[-1])
        deadline = time.monotonic() + args.seconds
        page = 0
        while time.monotonic() < deadline:
            # Autofocus may be overridden by browser chrome after navigation.
            # Aim using the page's geometry, then require a trusted pointer
            # event from the actual input before starting a typing sample.
            target_deadline = time.monotonic() + 15
            attempts = 0
            while True:
                aimed = time.monotonic()
                attempts += 1
                key('windowfocus', '--sync', windows[-1])
                # Older xdotool waits forever with --sync if already at the
                # requested coordinates. The page acknowledgment is our sync.
                key('mousemove', '--window', windows[-1],
                    str(ready['input_x']), str(ready['input_y']))
                key('click', '1')
                with (args.out / 'input-commands.jsonl').open('a') as trace:
                    state = subprocess.check_output(
                        ['xdotool', 'getmouselocation', 'getwindowfocus'],
                        env=env, text=True, timeout=5)
                    trace.write(json.dumps({'host_s': time.monotonic(), 'x_state': state}) + '\n')
                try:
                    targeted = event_after('pointer', aimed, 0.5, page=page, trusted=True)
                    if targeted.get('target') == 'entry':
                        break
                    # Chrome can change height after ready (e.g. the sandbox
                    # notification). Correct the aim from an actual X event's
                    # DOM coordinates, not a fixed toolbar-height assumption.
                    ready = dict(ready, input_x=ready['input_x'] + targeted['input_dx'],
                                 input_y=ready['input_y'] + targeted['input_dy'])
                    if time.monotonic() >= target_deadline:
                        raise RuntimeError('pointer did not reach the input')
                except TimeoutError:
                    if time.monotonic() >= target_deadline:
                        raise
            metric('input_ready', ready['host_s'], targeted)
            result['metrics'][-1]['click_attempts'] = attempts
            value = f'input-{page}-{token[:8]}'
            sent = time.monotonic()
            key('type', '--clearmodifiers', '--delay', '0', value)
            metric('input', sent, event_after('input', sent, 15, page=page, value=value))
            # Tab out of the input, then Page Down reaches the document.
            key('key', 'Tab')
            sent = time.monotonic()
            key('key', 'Next')
            scroll = event_after('scroll', sent, 15, page=page)
            if scroll.get('y', 0) <= 0:
                raise RuntimeError('scroll event did not move the document')
            metric('scroll', sent, scroll)
            time.sleep(1)
            page += 1
            next_url = url.rsplit('=', 1)[0] + '=' + str(page)
            key('key', '--clearmodifiers', 'ctrl+l')
            key('type', '--clearmodifiers', '--delay', '0', next_url)
            sent = time.monotonic()
            key('key', 'Return')
            ready = event_after('ready', sent, 30, page=page)
            metric('navigation', sent, ready)
        # Exercise normal application teardown so a completed UI workload
        # cannot hide unfinished custody behind the harness's emergency kill.
        key('key', '--clearmodifiers', 'ctrl+q')
        name, workload = workload_processes[-1]
        result['application_exit'] = workload.wait(timeout=45)
        if result['application_exit']:
            raise RuntimeError(f'{name} exited {result["application_exit"]} after browser quit')
        result['success'] = True
    except Exception as exc:
        result['error'] = str(exc)
        print(json.dumps({'error': str(exc)}), flush=True)
    finally:
        signal.signal(signal.SIGTERM, signal.SIG_IGN)
        signal.signal(signal.SIGINT, signal.SIG_IGN)
        result['after'] = counters()
        # Save state before ending any process. Never clear the kernel log.
        if args.mode == 'cross':
            try:
                result['destination_after'] = destination_snapshot()
                (args.out / 'destination-dmesg.txt').write_text(destination_run('dmesg'))
                if remote is not None:
                    # EOF stops the exact native owner even after transport
                    # loss. Its report appears only after descendant reaping.
                    remote.stdin.close()
                    remote.wait(timeout=20)
                    cleanup = json.loads(destination_run('cat ' + shlex.quote(remote_marker)))
                    result['destination_cleanup'] = cleanup
                    if not cleanup['complete'] or cleanup['survivors']:
                        raise RuntimeError('destination owner did not finish cleanup')
                    result['success'] &= (cleanup['status'] == remote.returncode == 0 and
                                          not cleanup['interrupted'] and not cleanup['timed_out'])
                    destination_run('rm ' + shlex.quote(remote_marker))
                result['destination_after_cleanup'] = destination_snapshot()
                live = destination_run('cat /sys/module/kernel/parameters/vmctx_mm_live '
                                       '/sys/module/vmctx/refcnt').split()
                result['destination_cleanup_complete'] = live == ['0', '0']
            except (OSError, subprocess.SubprocessError, ValueError, KeyError, RuntimeError) as exc:
                result['destination_cleanup_error'] = str(exc)
                result['destination_cleanup_complete'] = False
            if not result['destination_cleanup_complete']:
                result['success'] = False
        tasks = proc_tree(os.getpid())
        state = args.out / 'processes'
        state.mkdir()
        for pid, (_, birth) in tasks.items():
            dst = state / str(pid)
            dst.mkdir()
            (dst / 'identity.json').write_text(json.dumps({'pid': pid, 'start': birth}))
            # /proc/PID/maps takes mmap_lock and can wedge the observer
            # behind the very deadlock being diagnosed. These files do not
            # require walking or faulting the address space.
            for name in ('stat', 'status', 'stack', 'wchan'):
                try:
                    (dst / name).write_bytes((Path('/proc') / str(pid) / name).read_bytes())
                except OSError:
                    pass
        with (args.out / 'dmesg.txt').open('wb') as f:
            subprocess.run(['dmesg'], stdout=f, stderr=subprocess.DEVNULL)
        with (args.out / 'screen.xwd').open('wb') as f:
            try:
                subprocess.run(['xwd', '-root', '-silent'], env=env, stdout=f,
                               stderr=subprocess.DEVNULL, timeout=5)
            except (OSError, subprocess.TimeoutExpired):
                pass
        result['cleanup_errors'] = []
        # A child can be published after the diagnostic tree snapshot but
        # before its monitor exits. Rescan our subreaper's descendants until
        # empty, checking each birth identity before every signal. The bound
        # matches owned-run.py and allows native event teardown to finish.
        reap_until = time.monotonic() + 12
        cleanup_errors = {}
        result['cleanup_signalled'] = {}
        while True:
            pending = proc_tree(os.getpid())
            if not pending:
                break
            for pid in sorted(pending, reverse=True):
                handle = None
                try:
                    handle = os.pidfd_open(pid)
                    stat = (Path('/proc') / str(pid) / 'stat').read_text()
                    fields = stat[stat.rfind(')') + 2:].split()
                    if int(fields[19]) != pending[pid][1]:
                        continue
                    signal.pidfd_send_signal(handle, signal.SIGKILL)
                    result['cleanup_signalled'][pid] = pending[pid][1]
                except (ProcessLookupError, FileNotFoundError):
                    pass
                except OSError as exc:
                    cleanup_errors[pid] = {'pid': pid, 'error': str(exc)}
                finally:
                    if handle is not None:
                        os.close(handle)
            for p in processes:
                p.poll()
            while True:
                try:
                    reaped, _ = os.waitpid(-1, os.WNOHANG)
                    if not reaped:
                        break
                except ChildProcessError:
                    break
            if time.monotonic() >= reap_until:
                break
            time.sleep(0.02)
        result['cleanup_errors'] = list(cleanup_errors.values())
        result['after_cleanup'] = counters()
        result['surviving_descendants'] = proc_tree(os.getpid())
        # A setuid helper can reject our signal and then exit with its parent.
        # Retain that diagnostic; the final descendant inventory determines
        # whether cleanup actually left a process behind.
        result['cleanup_complete'] = not result['surviving_descendants']
        result['success'] &= result['cleanup_complete']
        server.shutdown()
        server.server_close()
        server_thread.join()
        for f in files:
            f.close()
        (args.out / 'result.json').write_text(json.dumps(result, indent=2) + '\n')
    print(json.dumps({'success': result['success'], 'out': str(args.out)}), flush=True)
    return 0 if result['success'] and result['cleanup_complete'] else 1


if __name__ == '__main__':
    raise SystemExit(main())
