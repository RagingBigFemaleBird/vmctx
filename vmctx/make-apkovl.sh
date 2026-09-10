#!/usr/bin/env bash
# Assemble the Alpine apkovl overlay that configures the netboot target:
#   - installs+enables sshd with our public key (passwordless root login)
#   - enables the 'local' service which auto-loads vmctx at boot
#   - sets hostname, requests default boot services
#
# Output: http/target.apkovl.tar.gz  (served to the target via apkovl= in
# boot.ipxe). Rebuild after changing anything under apkovl/.
set -euo pipefail
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "$HERE/.." && pwd)"
SRC="$HERE/apkovl"
OUT="$ROOT/http/target.apkovl.tar.gz"

if [ ! -f "$SRC/root/.ssh/authorized_keys" ]; then
	echo "error: $SRC/root/.ssh/authorized_keys missing (run ssh-keygen first)" >&2
	exit 1
fi

STAGE="$(mktemp -d)"
trap 'rm -rf "$STAGE"' EXIT
cp -a "$SRC/." "$STAGE/"

# Enable services via runlevel symlinks (targets resolve once pkgs install).
mkdir -p "$STAGE/etc/runlevels/default"
ln -sf /etc/init.d/sshd  "$STAGE/etc/runlevels/default/sshd"
ln -sf /etc/init.d/local "$STAGE/etc/runlevels/default/local"

# Permissions sshd is strict about.
chmod 700 "$STAGE/root" "$STAGE/root/.ssh"
chmod 600 "$STAGE/root/.ssh/authorized_keys"
chmod 755 "$STAGE/etc/local.d"/*.start
chmod 644 "$STAGE/etc/ssh/sshd_config"

# Root-owned, gzip tar (initramfs unpack keys off the .gz suffix).
tar --owner=0 --group=0 --numeric-owner -C "$STAGE" -czf "$OUT" .

echo "wrote $OUT"
tar -tzf "$OUT" | sed 's/^/  /'
