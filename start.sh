#!/usr/bin/env bash
# Start the LAN-boot servers: dnsmasq (proxy-DHCP + TFTP) and an HTTP server.
# Needs root for ports 67/69 and DHCP broadcast — re-execs itself with sudo.
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

if [[ $EUID -ne 0 ]]; then
    exec sudo "$0" "$@"
fi

# ---- Detect interface / addressing ------------------------------------
IFACE="${1:-$(ip -4 route show default | awk '{print $5; exit}')}"
if [[ -z "$IFACE" ]]; then
    echo "error: could not detect a default network interface; pass one: $0 <iface>" >&2
    exit 1
fi
CIDR="$(ip -4 -o addr show dev "$IFACE" | awk '{print $4; exit}')"
SERVER_IP="${CIDR%/*}"
PREFIX="${CIDR#*/}"

# Network address for the proxy dhcp-range
SUBNET="$(python3 -c "import ipaddress,sys;print(ipaddress.ip_interface('$CIDR').network.network_address)")"

echo "Interface : $IFACE"
echo "Server IP : $SERVER_IP/$PREFIX"
echo "Subnet    : $SUBNET (proxy-DHCP)"

# ---- WSL2 NAT warning --------------------------------------------------
if grep -qi microsoft /proc/version 2>/dev/null; then
    if [[ "$SERVER_IP" == 172.1[6-9].* || "$SERVER_IP" == 172.2?.* || "$SERVER_IP" == 172.3[01].* ]]; then
        cat >&2 <<EOF

!! WARNING: this looks like WSL2 in NAT mode ($SERVER_IP is a WSL-internal
!! address). PXE clients on your physical LAN CANNOT reach this machine.
!! Enable mirrored networking first — see README.md, section "WSL2".

EOF
    fi
fi

# ---- Generate dnsmasq.conf from template ------------------------------
sed -e "s|@IFACE@|$IFACE|g" \
    -e "s|@SUBNET@|$SUBNET|g" \
    -e "s|@SERVER_IP@|$SERVER_IP|g" \
    -e "s|@ROOT@|$ROOT|g" \
    "$ROOT/config/dnsmasq.conf.template" > "$ROOT/config/dnsmasq.conf"

"$ROOT/bin/dnsmasq" --test -C "$ROOT/config/dnsmasq.conf"

# ---- Stop any previous instance ---------------------------------------
"$ROOT/stop.sh" quiet || true
mkdir -p "$ROOT/logs"

# ---- HTTP server (kernel/initrd/modloop/apk repo) on :8080 ------------
cd "$ROOT/http"
nohup python3 -m http.server 8080 --bind 0.0.0.0 >> "$ROOT/logs/http.log" 2>&1 &
echo $! > "$ROOT/logs/http.pid"
cd "$ROOT"
sleep 0.5
if ! kill -0 "$(cat "$ROOT/logs/http.pid")" 2>/dev/null; then
    echo "error: HTTP server failed to start (see logs/http.log)" >&2
    exit 1
fi

# ---- dnsmasq (proxy-DHCP + TFTP) --------------------------------------
# Startup errors (e.g. a port grabbed on the Windows side of mirrored
# networking) go to logs/dnsmasq-start.err; runtime logs to logs/dnsmasq.log.
if ! "$ROOT/bin/dnsmasq" -C "$ROOT/config/dnsmasq.conf" \
        --pid-file="$ROOT/logs/dnsmasq.pid" 2> "$ROOT/logs/dnsmasq-start.err"; then
    echo "error: dnsmasq failed to start:" >&2
    cat "$ROOT/logs/dnsmasq-start.err" >&2
    exit 1
fi

echo
echo "LAN boot is up."
echo "  proxy-DHCP + TFTP : $SERVER_IP (ports 67/4011/69, via dnsmasq)"
echo "  HTTP              : http://$SERVER_IP:8080/  (boot script, kernel, packages)"
echo "  logs              : $ROOT/logs/"
echo
echo "Now enable PXE / network boot on the other machine and (re)boot it."
echo "Stop with: $ROOT/stop.sh"
