#!/usr/bin/env bash
# Build the vmctx kernel module + tools and stage them where the netboot
# target can fetch them over HTTP (http/vmctx/).
set -euo pipefail
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "$HERE/.." && pwd)"

# liblzma / libcrypto built locally for the kernel tree live in src/local;
# put the system dir first so host tools (openssl) resolve correctly.
export LD_LIBRARY_PATH="/usr/lib/x86_64-linux-gnu:$ROOT/src/local/lib${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"

# The same suppression tools/build-kernel.sh applies, and for the same reason:
# without it scripts/setlocalversion appends "+" whenever HEAD carries no
# annotated tag, and the module's vermagic becomes 6.18.35-0-lts+ against a
# target running 6.18.35-0-lts. insmod then refuses it on the target, long after
# the build said it succeeded -- and the build says the vermagic out loud at the
# end, which is the only place the mismatch is visible. The kernel was built
# with LOCALVERSION empty, so the module must be too.
export LOCALVERSION=

echo "== building vmctx.ko (against src/linux-6.18.35)"
make -C "$HERE/kernel"

echo "== building the tools (static)"
# Deleted first, not merely rebuilt, and for the same reason sync-amd.sh does
# it: SRCID is a hash of EVERY source under user/, compiled into EVERY binary.
# So editing one file invalidates all of them -- and make cannot know that,
# because make compares mtimes and vmhome.c did not change. Measured: a
# vmremote.c edit restaged a current vmremote beside a vmhome still carrying
# the previous id, and verify-build reported the target STALE with no rebuild
# able to clear it.
rm -f $(for t in $(make -s -C "$HERE/user" --no-print-directory print-tools); \
        do printf '%s ' "$HERE/build/$t"; done)
make -C "$HERE/user"

echo "== staging into http/vmctx/"
DEST="$ROOT/http/vmctx"
mkdir -p "$DEST"
cp "$HERE/kernel/vmctx.ko" "$DEST/"
# Stage every tool, not a hand-maintained subset: a missing name here leaves a
# stale binary in http/vmctx/ that looks current and produces false results.
for t in $(make -s -C "$HERE/user" --no-print-directory print-tools); do
	cp "$HERE/build/$t" "$DEST/"
done
cp "$HERE/setup-target.sh" "$DEST/"

VM="$(/usr/sbin/modinfo "$HERE/kernel/vmctx.ko" | awk '/^vermagic/{print $2}')"
SRCID="$(make -s -C "$HERE/user" print-srcid)"
echo "== done. vmctx.ko vermagic: $VM, source id: $SRCID"
echo "   served at http://<server>:8080/vmctx/  (fetch with setup-target.sh)"

# A build that succeeded here says nothing about what the target is running.
# Every stale-artifact failure in this tree's history looked exactly like a
# successful build, so the build ends by asking the target rather than assuming.
# Advisory: an unreachable or not-yet-synced target is not a build failure.
if [ "${VERIFY:-1}" = 1 ]; then
	echo
	"$HERE/verify-build.sh" intel || echo "   (deploy to the target, then re-run: ./verify-build.sh intel)"
fi

