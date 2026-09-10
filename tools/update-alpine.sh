#!/usr/bin/env bash
# Refresh the Alpine netboot files (kernel/initramfs/modloop) and the local
# apk mirror to the newest latest-stable release.
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BASE=https://dl-cdn.alpinelinux.org/alpine/latest-stable/releases/x86_64/netboot

cd "$ROOT/http"
for f in vmlinuz-lts initramfs-lts modloop-lts; do
    echo "downloading $f ..."
    curl -fL -o "$f.new" "$BASE/$f" && mv "$f.new" "$f"
done
python3 "$ROOT/tools/mirror-apks.py"
echo "done."
