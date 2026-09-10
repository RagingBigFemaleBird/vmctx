#!/usr/bin/env bash
# Build the LAN-boot Linux image from source and deploy it into http/.
#
# Produces, from src/linux-6.18.35 with Alpine's kernel config:
#   http/vmlinuz-custom    - the kernel (bzImage)
#   http/initramfs-custom  - original Alpine initramfs + our rebuilt boot modules
#   http/modloop-custom    - squashfs with the full rebuilt module tree
#                            (+ firmware carried over from the original modloop)
#
# Usage:
#   tools/build-kernel.sh          incremental build (after editing kernel source)
#   tools/build-kernel.sh fresh    reset .config from Alpine's config first
#
# To change kernel config: cd src/linux-6.18.35 && make menuconfig (needs
# ncurses) or scripts/config, then rerun this script.
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
KSRC="$ROOT/src/linux-6.18.35"
KVER="6.18.35-0-lts"        # must match LOCALVERSION below; initramfs+modloop key on it
JOBS="$(nproc)"
STAGE="$ROOT/src/stage"

# The kernel tree is now a git repo (so changes are tracked). Without this,
# scripts/setlocalversion appends a '+' to the version for a modified git
# tree, breaking the KVER match. Setting LOCALVERSION (even empty) suppresses
# that; CONFIG_LOCALVERSION still provides the "-0-lts" suffix.
export LOCALVERSION=

# Locally-built libelf (for objtool)
export PKG_CONFIG_PATH="$ROOT/src/local/lib/pkgconfig${PKG_CONFIG_PATH:+:$PKG_CONFIG_PATH}"
# System libdir first: our local dir must not shadow the system OpenSSL
# used by the `openssl` CLI (key generation); libelf only exists locally.
export LD_LIBRARY_PATH="/usr/lib/x86_64-linux-gnu:$ROOT/src/local/lib${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"

# ---- 1. Configure ------------------------------------------------------
cd "$KSRC"
if [[ ! -f .config || "${1:-}" == fresh ]]; then
    echo "== Seeding .config from Alpine's config-6.18.35-0-lts"
    cp "$ROOT/src/config-lts" .config
    # Adjustments for building outside Alpine (no pahole / OpenSSL headers
    # here; module signing and BTF are not needed for netboot):
    scripts/config \
        --set-str LOCALVERSION "-0-lts" \
        --disable DEBUG_INFO_BTF --disable DEBUG_INFO_DWARF5 \
        --enable DEBUG_INFO_NONE \
        --disable MODULE_SIG --disable MODULE_SIG_ALL \
        --set-str SYSTEM_TRUSTED_KEYS "" \
        --set-str MODULE_SIG_KEY "certs/signing_key.pem" \
        --set-str SYSTEM_REVOCATION_KEYS "" \
        --disable MODULE_COMPRESS --disable MODULE_COMPRESS_ALL \
        --disable MODULE_COMPRESS_GZIP
fi
make -s olddefconfig

# ---- 2. Compile --------------------------------------------------------
echo "== Building kernel + modules with $JOBS jobs"
make -j"$JOBS" bzImage modules

ACTUAL="$(make -s kernelrelease)"
if [[ "$ACTUAL" != "$KVER" ]]; then
    echo "error: built kernelrelease '$ACTUAL' != expected '$KVER'" >&2
    echo "(initramfs and modloop are keyed on the version string; fix LOCALVERSION)" >&2
    exit 1
fi

echo "== Installing modules to staging"
rm -rf "$STAGE"
make -s INSTALL_MOD_PATH="$STAGE" INSTALL_MOD_STRIP=1 modules_install

# ---- 3. modloop (full module tree + firmware) --------------------------
echo "== Building modloop-custom"
MLDIR="$ROOT/src/modloop-root"
rm -rf "$MLDIR"
mkdir -p "$MLDIR/modules"
cp -a "$STAGE/lib/modules/$KVER" "$MLDIR/modules/"
rm -f "$MLDIR/modules/$KVER/build" "$MLDIR/modules/$KVER/source"

# Firmware blobs: reuse the set shipped in Alpine's original modloop
if [[ ! -d "$ROOT/src/modloop-firmware/modules/firmware" ]]; then
    echo "   extracting firmware from original modloop (one-time)"
    unsquashfs -q -d "$ROOT/src/modloop-firmware" "$ROOT/http/modloop-lts" 'modules/firmware' >/dev/null
fi
cp -a "$ROOT/src/modloop-firmware/modules/firmware" "$MLDIR/modules/"

mksquashfs "$MLDIR" "$ROOT/http/modloop-custom.new" -comp xz -noappend -quiet
mv "$ROOT/http/modloop-custom.new" "$ROOT/http/modloop-custom"

# ---- 4. initramfs (original + overlay with our rebuilt boot modules) ---
# Rather than unpacking/repacking Alpine's initramfs (fragile as non-root),
# append a second cpio archive: the kernel unpacks both, later files win.
echo "== Building initramfs-custom"
OVL="$ROOT/src/initramfs-overlay"
rm -rf "$OVL"
mkdir -p "$OVL/usr/lib/modules/$KVER"

# Same boot-critical module subset as Alpine's initramfs, from our build
zcat "$ROOT/http/initramfs-lts" | cpio -it --quiet \
    | grep -E "^usr/lib/modules/$KVER/kernel/.*\.ko$" > "$OVL/.modlist"
missing=0
while IFS= read -r path; do
    rel="${path#usr/lib/modules/$KVER/}"
    src="$STAGE/lib/modules/$KVER/$rel"
    if [[ -f "$src" ]]; then
        mkdir -p "$OVL/$(dirname "$path")"
        cp "$src" "$OVL/$path"
    else
        echo "   warning: $rel not produced by this build (skipped)"
        missing=$((missing+1))
    fi
done < "$OVL/.modlist"
total="$(wc -l < "$OVL/.modlist")"
echo "   modules: $((total-missing))/$total from new build"

cp "$STAGE/lib/modules/$KVER"/modules.{order,builtin,builtin.modinfo} \
   "$OVL/usr/lib/modules/$KVER/"
depmod -b "$OVL/usr" -F "$KSRC/System.map" "$KVER"

( cd "$OVL" && find usr | cpio -o -H newc -R 0:0 --quiet | gzip -9 ) \
    > "$ROOT/src/initramfs-overlay.cpio.gz"
cat "$ROOT/http/initramfs-lts" "$ROOT/src/initramfs-overlay.cpio.gz" \
    > "$ROOT/http/initramfs-custom"

# ---- 5. Kernel image ---------------------------------------------------
cp "$KSRC/arch/x86/boot/bzImage" "$ROOT/http/vmlinuz-custom"

echo
echo "== Done: $ACTUAL"
ls -la "$ROOT/http/vmlinuz-custom" "$ROOT/http/initramfs-custom" "$ROOT/http/modloop-custom"
echo
echo "boot.ipxe must point at the -custom files (kernel_img/initrd_img/modloop)."
