#!/usr/bin/env bash
# Stop the LAN-boot servers started by start.sh.
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
QUIET="${1:-}"

stopped=0
for name in dnsmasq http; do
    pidfile="$ROOT/logs/$name.pid"
    [[ -f "$pidfile" ]] || continue
    pid="$(cat "$pidfile")"
    if kill -0 "$pid" 2>/dev/null; then
        kill "$pid" 2>/dev/null || sudo kill "$pid"
        stopped=1
        [[ "$QUIET" == quiet ]] || echo "stopped $name (pid $pid)"
    fi
    rm -f "$pidfile"
done
# Fallback: catch instances whose pidfile went stale
if pkill -f "python3 -m http.server 8080" 2>/dev/null; then stopped=1; fi
if pkill -f "dnsmasq -C $ROOT/config/dnsmasq.conf" 2>/dev/null; then stopped=1; fi

[[ "$QUIET" == quiet || $stopped == 1 ]] || echo "nothing running"
