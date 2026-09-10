#!/usr/bin/env python3
"""Check that a kernel modloop preserves the built module names, including case."""
import argparse
from pathlib import Path
import subprocess


def require_names(expected, actual, location):
    missing, extra = expected - actual, actual - expected
    if missing or extra:
        raise ValueError(f"{location}: missing={sorted(missing)}, extra={sorted(extra)}")


def verify(tree, stage, image, release):
    expected = {"kernel/" + str(Path(line).with_suffix(".ko"))
                for line in (tree / "modules.order").read_text().splitlines()}
    if not expected:
        raise ValueError("Empty modules.order")
    actual = {str(p.relative_to(stage)) for p in (stage / "kernel").rglob("*.ko")}
    require_names(expected, actual, "module staging")
    listing = subprocess.check_output(["unsquashfs", "-ll", str(image)], text=True)
    prefix = "squashfs-root/modules/" + release + "/"
    packed = {line.split(prefix, 1)[1] for line in listing.splitlines()
              if prefix in line and line.endswith(".ko")}
    require_names(expected, packed, "modloop archive")
    # Windows staging silently replaced one of these modules with the other.
    # Check every case-colliding group byte for byte against the Linux stage.
    groups = {}
    for name in expected:
        groups.setdefault(name.casefold(), []).append(name)
    collisions = [group for group in groups.values() if len(group) > 1]
    for group in collisions:
        for name in group:
            data = subprocess.check_output(["unsquashfs", "-cat", str(image),
                                            "modules/" + release + "/" + name])
            if data != (stage / name).read_bytes():
                raise ValueError("Archive content mismatch: " + name)
    print(f"PASS: {len(expected)} module paths and {len(collisions)} case-colliding groups preserved")


def self_test():
    expected = {"kernel/xt_RATEEST.ko", "kernel/xt_rateest.ko"}
    require_names(expected, set(expected), "preserved")
    for damaged in [{"kernel/xt_RATEEST.ko"}, {"kernel/xt_rateest.ko"},
                    expected | {"kernel/stale.ko"}, set()]:
        try:
            require_names(expected, damaged, "damaged")
        except ValueError:
            continue
        raise AssertionError("Accepted damaged module inventory")
    print("PASS: collapsed, missing, and stale module inventories are rejected")


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--self-test", action="store_true")
    parser.add_argument("--tree", type=Path)
    parser.add_argument("--stage", type=Path)
    parser.add_argument("--image", type=Path)
    parser.add_argument("--release")
    args = parser.parse_args()
    if args.self_test:
        self_test()
    else:
        if not all([args.tree, args.stage, args.image, args.release]):
            parser.error("--tree, --stage, --image, and --release are required")
        verify(args.tree, args.stage, args.image, args.release)
