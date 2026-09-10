#!/usr/bin/env python3
"""Run a local harness as a subreaper and clean up only its descendants."""
import argparse
import ctypes
import json
import os
from pathlib import Path
import signal
import subprocess
import time


def descendants():
    tasks = {}
    for p in Path('/proc').glob('[0-9]*/stat'):
        try:
            text = p.read_text()
            fields = text[text.rfind(')') + 2:].split()
            tasks[int(p.parent.name)] = (int(fields[1]), int(fields[19]))
        except (OSError, ValueError, IndexError):
            pass
    owned = {os.getpid()}
    while True:
        grown = owned | {pid for pid, (ppid, _) in tasks.items() if ppid in owned}
        if grown == owned:
            return {pid: tasks[pid][1] for pid in owned if pid != os.getpid() and pid in tasks}
        owned = grown


def terminate_owned(tasks):
    for pid, birth in tasks.items():
        handle = None
        try:
            # Bind the signal to the task before verifying the recorded birth.
            # A recycled pid never receives a signal intended for this run.
            handle = os.pidfd_open(pid)
            text = Path('/proc', str(pid), 'stat').read_text()
            now = int(text[text.rfind(')') + 2:].split()[19])
            if now == birth:
                signal.pidfd_send_signal(handle, signal.SIGKILL)
        except ProcessLookupError:
            pass
        except FileNotFoundError:
            pass
        finally:
            if handle is not None:
                os.close(handle)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--report', type=Path, required=True)
    parser.add_argument('--timeout', type=float)
    parser.add_argument('--timeout-marker', type=Path)
    parser.add_argument('command', nargs=argparse.REMAINDER)
    args = parser.parse_args()
    if not args.command:
        parser.error('missing command')
    if args.timeout is not None and (not 0 < args.timeout <= 43200):
        parser.error('timeout must be between zero and 43200 seconds')
    if ctypes.CDLL(None, use_errno=True).prctl(36, 1, 0, 0, 0):
        raise OSError(ctypes.get_errno(), 'PR_SET_CHILD_SUBREAPER')
    args.report.parent.mkdir(parents=True, exist_ok=True)
    args.report.unlink(missing_ok=True)
    interrupted = 0

    def stop(signum, _):
        nonlocal interrupted
        interrupted = signum

    signal.signal(signal.SIGTERM, stop)
    signal.signal(signal.SIGINT, stop)
    env = dict(os.environ, VMCTX_OWNED_RUN=str(os.getpid()))
    child = subprocess.Popen(args.command, env=env)
    deadline = time.monotonic() + args.timeout if args.timeout is not None else None
    timed_out = False
    while child.poll() is None and not interrupted:
        if deadline is not None and time.monotonic() >= deadline:
            timed_out = True
            if args.timeout_marker is not None:
                args.timeout_marker.touch()
            break
        time.sleep(.02)
    status = 124 if timed_out else 128 + interrupted if interrupted else child.returncode
    cleanup = {'signalled': {}, 'survivors': {}, 'error': None}
    until = time.monotonic() + 12
    try:
        while True:
            tasks = descendants()
            if not tasks:
                break
            cleanup['signalled'].update(tasks)
            terminate_owned(tasks)
            # Reap ordinary children through Popen first, then any orphaned
            # source tasks adopted when their monitor/parent exited.
            child.poll()
            while True:
                try:
                    pid, _ = os.waitpid(-1, os.WNOHANG)
                    if not pid:
                        break
                except ChildProcessError:
                    break
            if time.monotonic() >= until:
                break
            time.sleep(.02)
    except Exception as exc:
        cleanup['error'] = repr(exc)
    cleanup['survivors'] = descendants()
    cleanup['complete'] = not cleanup['survivors'] and cleanup['error'] is None
    cleanup['status'] = status
    cleanup['timed_out'] = timed_out
    args.report.write_text(json.dumps(cleanup, indent=2) + '\n')
    if not cleanup['complete']:
        print('owned-run: cleanup incomplete; see ' + str(args.report), flush=True)
        return 125
    return 128 - status if status is not None and status < 0 else status


if __name__ == '__main__':
    raise SystemExit(main())
