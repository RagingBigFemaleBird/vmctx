#!/usr/bin/env python3
"""Mirror a subset of the Alpine 'main' repository into http/alpine/main/x86_64.

Downloads the signed APKINDEX plus the full dependency closure of the
packages listed in WANTED, so a netbooted machine can build its root
filesystem and install tools without internet access.

Usage: python3 tools/mirror-apks.py [extra-package ...]
"""
import sys, tarfile, io, os, urllib.request

REPO = "https://dl-cdn.alpinelinux.org/alpine/latest-stable/main"
ARCH = "x86_64"
DEST = os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))),
                    "http", "alpine", "main", ARCH)

WANTED = [
    # what the netboot initramfs installs to build the RAM rootfs
    "alpine-base",
    # ssh access to the target (server binary + its OpenRC init script +
    # host-key generator + TLS runtime). The -openrc subpackage carries
    # /etc/init.d/sshd and is pulled by install_if upstream, which our
    # closure walk does not follow — so list it explicitly.
    "openssh", "openssh-server-common-openrc",
    "openssl", "ssl_client",
    # networking helpers apk may pull during boot
    "ifupdown-ng", "bridge",
    # useful console tools
    "curl", "wget", "nano", "less", "tmux",
    "e2fsprogs", "dosfstools", "parted", "util-linux", "lsblk",
    "pciutils", "usbutils", "ethtool", "tcpdump", "rsync", "grep",
    "coreutils", "findutils", "procps-ng", "kbd-bkeymaps",
]


def fetch(url):
    with urllib.request.urlopen(url) as r:
        return r.read()


def parse_index(raw):
    """Return (packages, providers): APKINDEX stanzas keyed by name/provides."""
    with tarfile.open(fileobj=io.BytesIO(raw)) as tf:
        text = tf.extractfile("APKINDEX").read().decode()
    packages, providers = {}, {}
    for stanza in text.split("\n\n"):
        fields = {}
        for line in stanza.splitlines():
            if len(line) > 2 and line[1] == ":":
                fields[line[0]] = line[2:]
        if "P" not in fields:
            continue
        name = fields["P"]
        packages[name] = fields
        providers.setdefault(name, name)
        for p in fields.get("p", "").split():
            pname = p.split("=")[0]
            providers.setdefault(pname, name)
    return packages, providers


def strip_constraint(dep):
    for sep in ("<", ">", "=", "~"):
        dep = dep.split(sep)[0]
    return dep


def closure(roots, packages, providers):
    seen, todo, missing = set(), list(roots), []
    while todo:
        name = strip_constraint(todo.pop())
        if name.startswith("!"):
            continue
        real = providers.get(name)
        if real is None:
            missing.append(name)
            continue
        if real in seen:
            continue
        seen.add(real)
        todo += packages[real].get("D", "").split()
    return seen, missing


def main():
    wanted = WANTED + sys.argv[1:]
    os.makedirs(DEST, exist_ok=True)
    print(f"Fetching APKINDEX from {REPO}/{ARCH} ...")
    raw = fetch(f"{REPO}/{ARCH}/APKINDEX.tar.gz")
    with open(os.path.join(DEST, "APKINDEX.tar.gz"), "wb") as f:
        f.write(raw)  # keep the signed original so apk's signature check passes
    packages, providers = parse_index(raw)

    names, missing = closure(wanted, packages, providers)
    for m in missing:
        print(f"  warning: no provider for '{m}' in main (skipped)")

    total = 0
    for i, name in enumerate(sorted(names), 1):
        f = packages[name]
        apk = f"{name}-{f['V']}.apk"
        path = os.path.join(DEST, apk)
        size = int(f.get("S", 0))
        total += size
        if os.path.exists(path) and os.path.getsize(path) == size:
            print(f"  [{i}/{len(names)}] {apk} (cached)")
            continue
        print(f"  [{i}/{len(names)}] {apk} ({size//1024} KiB)")
        data = fetch(f"{REPO}/{ARCH}/{apk}")
        with open(path, "wb") as fh:
            fh.write(data)
    print(f"Done: {len(names)} packages, {total/1024/1024:.1f} MiB -> {DEST}")


if __name__ == "__main__":
    main()
