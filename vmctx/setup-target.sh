#!/bin/sh
# Run this ON the PXE-booted target to fetch and load vmctx + tools.
#
# The booted Alpine derives its server from where it fetched boot.ipxe.
# Pass the server IP as $1, or set VMCTX_SERVER; defaults to the PXE
# next-server recorded by the initramfs if available.
set -e

SERVER="${1:-${VMCTX_SERVER:-}}"
if [ -z "$SERVER" ]; then
	# Alpine netboot leaves the repo URL here; reuse its host.
	SERVER="$(awk -F/ '/http/{print $3; exit}' /etc/apk/repositories 2>/dev/null | cut -d: -f1)"
fi
if [ -z "$SERVER" ]; then
	echo "usage: $0 <server-ip>   (could not auto-detect)" >&2
	exit 1
fi

BASE="http://$SERVER:8080/vmctx"
echo "== fetching vmctx from $BASE"
cd /tmp
for f in vmctx.ko dumpvm vmselftest vmmon vmremote vmmig vmhome; do
	wget -q "$BASE/$f" -O "$f"
done
chmod +x dumpvm vmselftest vmmon vmremote vmmig vmhome
mkdir -p /usr/local/bin
install -m755 dumpvm vmselftest vmmon vmremote vmmig vmhome /usr/local/bin/

echo "== loading vmctx.ko"
if rmmod vmctx 2>/dev/null; then echo "   (reloaded)"; fi
insmod /tmp/vmctx.ko

echo "== kernel says:"
dmesg | grep vmctx: | tail -5

echo
echo "Ready. Try:"
echo "  dumpvm            # show CPU virtualization capabilities"
echo "  vmselftest        # enter guest mode once and prove the hardware works"
echo "  dumpvm <id>       # full state dump of a context"
echo
echo "This machine is a DESTINATION: it executes, and owns nothing. It runs no"
echo "program of its own -- vmremote connects back to the source, which owns the"
echo "program, its files and its memory."
