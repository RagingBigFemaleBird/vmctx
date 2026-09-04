# AVM: Application as a VM

An application is a VM context, and a VM context is a first-class Linux
process. That is the [`avm.md`](avm.md) spec, and this repository is its
implementation plus the lab it is tested on.

What it buys, demonstrated on real hardware: **a browser installed only on one
machine runs on another** — its code and memory resident there, its
instructions executing on that CPU — while every syscall it makes is executed
back on the first machine and its rendered output appears there. Byte-identical
to running it locally, at 99.9% of native compute speed. Verified on both
vendors, AMD (SVM) and Intel (VMX).

The work lives in **[vmctx/](vmctx/)** — kernel changes that make a VM context
a real process (`vmctx_run(2)`, `task_struct.vmctx`, `/proc` visibility), the
SVM and VMX backends, and the tools that carry a program's memory and syscalls
between machines. Start with **[vmctx/RUNNING.md](vmctx/RUNNING.md)** — the
operator's guide: how to run a program on one machine or two, the suites, the
performance harness, deployment, and how to read a run's artifacts. Then
[vmctx/ARCHITECTURE.md](vmctx/ARCHITECTURE.md) is the map,
[vmctx/AUDIT.md](vmctx/AUDIT.md) the close reading — every flow, corner case,
and the complete ledger of bugs found and fixed —
[vmctx/HISTORY.md](vmctx/HISTORY.md) the arc in ten campaigns,
[vmctx/PITFALLS.md](vmctx/PITFALLS.md) the trap catalog, and
[vmctx/NOTES.md](vmctx/NOTES.md) the session-era history with the numbers
behind every refuted design, and [vmctx/HANDOFF.md](vmctx/HANDOFF.md) the
state to resume from plus the session ledger (27 → 46b).

The rest of this file is **the lab**: a self-contained PXE server that netboots
the machines the contexts run on. Nothing about AVM requires netbooting — it is
simply how a kernel is put on a test machine here without installing anything
on it.

## What it boots

Alpine Linux (latest-stable) running entirely from RAM:

- **Kernel**: `vmlinuz-lts` — Linux **6.18.35 LTS** with Alpine's broad
  hardware/driver configuration (drivers load from `modloop-lts`).
- **Userland**: full busybox console + OpenRC + `apk` package manager.
  Log in as `root` (no password).
- **Offline-capable**: a local mirror of 93 Alpine packages
  ([http/alpine/main](http/alpine/main)) provides the root filesystem plus
  tools like `openssh`, `curl`, `tmux`, `nano`, `parted`, `e2fsprogs`,
  `tcpdump`, `pciutils`, `rsync` — install any of them on the booted
  machine with `apk add <name>`, no internet required.

## Quick start

```bash
./start.sh
```

(asks for your sudo password — ports 67/69 need root). Then on the other
machine: enable network/PXE boot in the firmware and boot. Works for both
BIOS and UEFI clients. Stop with `./stop.sh`.

## How it works

1. The client's PXE firmware broadcasts DHCP. Your router still assigns the
   IP address; dnsmasq (in **proxy-DHCP** mode — no changes to your LAN) adds
   the boot info on top and serves `undionly.kpxe` (BIOS) or `ipxe.efi`
   (UEFI) over TFTP.
2. That iPXE bootloader (built here with an embedded script — see
   [config/embed.ipxe](config/embed.ipxe)) chains to
   `http://<this-machine>:8080/boot.ipxe`.
3. [boot.ipxe](http/boot.ipxe) loads the kernel + initramfs over HTTP and
   boots with `modloop=` (drivers) and `alpine_repo=` (packages) pointing
   back at this server.

## Booting your own image instead

Drop your kernel and initramfs into [http/](http/) and edit the
`kernel_img` / `initrd_img` / `cmdline` variables at the top of
[http/boot.ipxe](http/boot.ipxe). The change takes effect on the next PXE
boot — no restart needed. Anything iPXE understands works (vmlinuz+initrd,
memdisk ISOs, etc.).

## WSL2 — required one-time setup

This machine runs WSL2. In the default NAT mode (current state: eth0 has a
172.18.x.x address) **LAN machines cannot reach WSL at all**, and DHCP
broadcasts never arrive. You must switch WSL to *mirrored networking*:

1. In Windows, edit `%UserProfile%\.wslconfig` and add:

   ```ini
   [wsl2]
   networkingMode=mirrored
   ```

2. Allow inbound traffic through the Hyper-V/WSL firewall (admin PowerShell):

   ```powershell
   Set-NetFirewallHyperVVMSetting -Name '{40E0AC32-46A5-438A-A0B2-2B479E8F2E90}' -DefaultInboundAction Allow
   ```

   (Or allow just UDP 67, 69, 4011 and TCP 8080.)

3. Restart WSL: `wsl --shutdown`, reopen, then run `./start.sh` again.
   With mirrored networking, eth0 carries your real LAN IP and PXE clients
   can find the server.

If mirrored mode isn't an option, run this folder from a real Linux box on
the LAN instead — it's fully portable (rebuild dnsmasq if the libc differs).

## Layout

| Path | Contents |
|---|---|
| `avm.md` | the spec this implements |
| `vmctx/` | AVM itself — kernel changes, the SVM/VMX backends, and the tools |
| `start.sh` / `stop.sh` | bring the boot services up / down |
| `bin/dnsmasq` | dnsmasq 2.91, compiled from source |
| `tftp/` | iPXE bootloaders (BIOS + UEFI, embedded chain script) |
| `http/` | boot script, kernel, initramfs, modloop, local apk mirror |
| `config/` | dnsmasq template (+ generated config), iPXE embed script |
| `tools/mirror-apks.py` | (re)build the offline package mirror; pass extra package names as args |
| `tools/update-alpine.sh` | pull the newest Alpine kernel/initramfs/modloop + refresh mirror |
| `src/` | dnsmasq, iPXE, xz sources (kept for rebuilds) |
| `logs/` | `dnsmasq.log`, `http.log`, pidfiles |

## Troubleshooting

- **Client sees no boot server**: check `logs/dnsmasq.log` — you should see
  `PXE(eth0) ... proxy` lines when the client broadcasts. Nothing there →
  network path problem (WSL NAT, firewall, or client on a different subnet).
- **iPXE loads but hangs at "chaining"**: port 8080 blocked, or the HTTP
  server died — check `logs/http.log` and `curl http://<ip>:8080/boot.ipxe`.
- **UEFI client ignores the offer**: some firmware is picky; try the
  alternative loader by changing `ipxe.efi` to `snponly.efi` in
  `config/dnsmasq.conf.template`.
- **Secure Boot must be disabled** on the client (iPXE binaries are unsigned).

## vmctx — the implementation

AVM itself lives in [vmctx/](vmctx/): kernel source changes that make a VM
context a **first-class Linux process** (`vmctx_run(2)` syscall,
`task_struct.vmctx`, `/proc` visibility), the SVM and VMX backends as a module,
and the tools. See [vmctx/ARCHITECTURE.md](vmctx/ARCHITECTURE.md),
[vmctx/AUDIT.md](vmctx/AUDIT.md) and [vmctx/NOTES.md](vmctx/NOTES.md) for the
architecture, the mechanism detail, and the project history.

The kernel modifications are tracked in their own git repo at
`src/linux-6.18.35` (vanilla 6.18.35 baseline, then the vmctx commits), and —
since `src/` is not tracked here — also exported as reviewable patches at
[vmctx/kernel-patches/](vmctx/kernel-patches/), which are generated and must be
regenerated whenever that tree changes. Build the module + tools with
`vmctx/build.sh`; rebuild the kernel with `tools/build-kernel.sh`.

## The kernel is built from source

The boot image is compiled here from vanilla kernel.org source
(`src/linux-6.18.35`) using Alpine's own kernel config (`src/config-lts`),
so it is driver-for-driver identical to Alpine's stock `-lts` kernel.
**To hack on the kernel**: edit the source, then

```bash
tools/build-kernel.sh
```

Incremental — only recompiles what changed, then regenerates and deploys
`http/vmlinuz-custom`, `http/initramfs-custom` (Alpine's initramfs plus an
overlay with our rebuilt boot modules) and `http/modloop-custom` (full
module tree + firmware). The next PXE boot picks them up; no server restart
needed. `tools/build-kernel.sh fresh` resets `.config` back to Alpine's.

Config deviations from stock Alpine (all build-environment driven, none
affect drivers or behavior): BTF debug info off (no pahole here), modules
unsigned (`modloop_verify=no` is passed on the kernel command line
accordingly), auto-generated signing key. Locally-built build deps
(libelf, OpenSSL libcrypto) live in `src/local/`.

`http/boot.ipxe` currently boots the `-custom` images; the stock Alpine
images are kept alongside as `*-lts` — point the three references in
`boot.ipxe` back at them to revert.

## Rebuilding the servers from source

```bash
# dnsmasq
cd src/dnsmasq-2.91 && make -j && cp src/dnsmasq ../../bin/

# iPXE (both loaders, with the embedded chain script)
cd src/ipxe/src && make -j bin/undionly.kpxe bin-x86_64-efi/ipxe.efi \
  EMBED=$PWD/../../../config/embed.ipxe \
  HOST_CFLAGS="-O2 -I$PWD/../../xz-install/include" \
  ZBIN_LDFLAGS="-L$PWD/../../xz-install/lib -llzma" NO_WERROR=1 \
  && cp bin/undionly.kpxe bin-x86_64-efi/ipxe.efi ../../../tftp/
```
