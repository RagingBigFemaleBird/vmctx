#!/usr/bin/env bash
# Reboot the PXE target and wait for it to come back up.
#
#   vmctx/reboot-target.sh [ip]
#
# The target netboots from this server, so a plain reboot picks up whatever
# kernel/module/tools are currently staged in http/. Waits for sshd, then
# reports the running kernel build so it is obvious which image came up.
set -euo pipefail
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "$HERE/.." && pwd)"
KEY="$ROOT/config/id_vmctx"
SSHOPT=(-i "$KEY" -o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null
	-o ConnectTimeout=5 -o BatchMode=yes)

IP="${1:-${TARGET:-}}"
if [ -z "$IP" ]; then
	# Targets announce themselves by fetching from our HTTP server. With more
	# than one on the LAN, "the last one seen" is whichever booted most
	# recently — so it would reboot a machine somebody else is using, and the
	# only sign would be their session dying. Name it, or say which.
	mapfile -t seen < <(grep -hoE '^10\.0\.0\.[0-9]+' "$ROOT/logs/http.log" 2>/dev/null |
	                    grep -v '^10\.0\.0\.27$' | sort -u)
	if [ "${#seen[@]}" -gt 1 ]; then
		echo "error: ${#seen[@]} targets have booted from this server:" >&2
		printf '  %s\n' "${seen[@]}" >&2
		echo "pass one, or set TARGET=<ip>" >&2
		exit 1
	fi
	IP="${seen[0]:-}"
fi
[ -n "$IP" ] || { echo "error: no target ip (pass one, or set TARGET=<ip>)" >&2; exit 1; }

before="$(ssh "${SSHOPT[@]}" "root@$IP" 'uname -v' 2>/dev/null || echo unknown)"
echo "rebooting $IP (was: $before)"
ssh "${SSHOPT[@]}" "root@$IP" 'reboot' >/dev/null 2>&1 || true

# Wait for it to go away, then come back.
sleep 5
for i in $(seq 1 90); do
	if ssh "${SSHOPT[@]}" "root@$IP" true 2>/dev/null; then
		after="$(ssh "${SSHOPT[@]}" "root@$IP" 'uname -v')"
		mod="$(ssh "${SSHOPT[@]}" "root@$IP" 'lsmod | grep -c "^vmctx"' || echo 0)"
		echo "up after ~$((i * 5))s: $after (vmctx loaded: $mod)"
		exit 0
	fi
	sleep 5
done
echo "error: target did not come back within 450s" >&2
exit 1
