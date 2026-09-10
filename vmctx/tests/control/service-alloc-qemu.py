#!/usr/bin/env python3
"""Run the real service allocation path in a disposable, diskless TCG VM."""
import argparse
import gzip
import hashlib
import json
from pathlib import Path
import re
import shutil
import stat
import subprocess
import time


def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def newc(entries):
    out = bytearray()
    for ino, (name, mode, data, rmajor, rminor) in enumerate(entries, 1):
        name = name.encode() + b"\0"
        fields = [ino, mode, 0, 0, 1, 0, len(data), 0, 0,
                  rmajor, rminor, len(name), 0]
        out += b"070701" + "".join(f"{n:08x}" for n in fields).encode() + name
        out += b"\0" * (-len(out) % 4)
        out += data
        out += b"\0" * (-len(out) % 4)
    return bytes(out)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--kernel-tree", type=Path, required=True)
    parser.add_argument("--out", type=Path, required=True)
    parser.add_argument("--run-nr", type=int, required=True)
    parser.add_argument("--ctl-nr", type=int, required=True)
    parser.add_argument("--expect", choices=["pass", "allocation-panic"], required=True)
    args = parser.parse_args()
    tree = args.kernel_tree.resolve()
    dest = args.out.resolve()
    dest.mkdir(parents=True, exist_ok=False)
    source = Path(__file__).with_name("service-alloc-init.c").resolve()
    config = (tree / ".config").read_text()
    for symbol in ["FAILSLAB", "FAULT_INJECTION", "FAULT_INJECTION_DEBUG_FS",
                   "FAULT_INJECTION_STACKTRACE_FILTER", "DEBUG_FS", "PROC_FS",
                   "SYSFS", "BLK_DEV_INITRD", "RD_GZIP"]:
        if f"CONFIG_{symbol}=y\n" not in config:
            raise RuntimeError(f"required configuration missing: {symbol}")
    if "CONFIG_RANDOMIZE_BASE=y\n" in config:
        raise RuntimeError("disable RANDOMIZE_BASE for fixed stack filter addresses")
    symbols = subprocess.check_output(["nm", "-S", str(tree / "vmlinux")], text=True)
    matches = re.findall(r"^([0-9a-f]+) ([0-9a-f]+) T vmctx_run_current$", symbols, re.M)
    if len(matches) != 1:
        raise RuntimeError("cannot identify exactly one vmctx_run_current symbol")
    start, size = [int(n, 16) for n in matches[0]]
    if size <= 0:
        raise RuntimeError("empty symbol range")
    compile_cmd = ["cc", "-O2", "-Wall", "-Wextra", "-Werror", "-static",
                   "-D__EXPORTED_HEADERS__", f"-DVMCTX_RUN_NR={args.run_nr}",
                   f"-DVMCTX_CTL_NR={args.ctl_nr}", f"-DRUN_START=0x{start:x}UL",
                   f"-DRUN_END=0x{start + size:x}UL", "-I", str(tree / "include/uapi"),
                   str(source), "-o", str(dest / "init")]
    subprocess.run(compile_cmd, check=True)
    entries = [(name, stat.S_IFDIR | 0o755, b"", 0, 0)
               for name in ["dev", "proc", "sys"]]
    entries += [("dev/console", stat.S_IFCHR | 0o600, b"", 5, 1),
                ("dev/null", stat.S_IFCHR | 0o666, b"", 1, 3),
                ("init", stat.S_IFREG | 0o755, (dest / "init").read_bytes(), 0, 0),
                ("TRAILER!!!", 0, b"", 0, 0)]
    (dest / "initramfs.gz").write_bytes(gzip.compress(newc(entries), mtime=0))
    shutil.copyfile(tree / "arch/x86/boot/bzImage", dest / "bzImage")
    shutil.copyfile(tree / ".config", dest / "kernel.config")
    cmd = ["qemu-system-x86_64", "-accel", "tcg", "-cpu", "max", "-smp", "2",
           "-m", "1024", "-display", "none", "-monitor", "none", "-serial", "stdio",
           "-nic", "none", "-no-reboot", "-kernel", str(dest / "bzImage"),
           "-initrd", str(dest / "initramfs.gz"), "-append",
           "console=ttyS0 rdinit=/init nokaslr panic_on_oops=1 panic=-1"]
    record = {"command": cmd, "compile_command": compile_cmd,
              "expected": args.expect, "kernel_tree": str(tree),
              "run_symbol": {"start": start, "size": size},
              "sha256": {str(p.relative_to(tree)): sha(p) for p in [
                  tree / "kernel/vmctx.c", tree / "kernel/fork.c",
                  tree / "include/linux/shmem_fs.h", tree / "vmlinux"]}}
    record["sha256"].update({p.name: sha(p) for p in [source, dest / "init",
        dest / "bzImage", dest / "initramfs.gz", dest / "kernel.config"]})
    (dest / "invocation.json").write_text(json.dumps(record, indent=2) + "\n")
    begin = time.monotonic()
    with (dest / "console.log").open("wb") as log:
        try:
            result = subprocess.run(cmd, stdout=log, stderr=subprocess.STDOUT, timeout=120)
            record["qemu_returncode"] = result.returncode
            record["timeout"] = False
        except subprocess.TimeoutExpired:
            record["qemu_returncode"] = None
            record["timeout"] = True
    record["elapsed_s"] = time.monotonic() - begin
    output = (dest / "console.log").read_text(errors="replace").replace("\r", "")
    rounds = re.findall(r"SERVICE_FAILURE round=(\d+) rc=-1 errno=12 remaining=0\nmm_live=0\n", output)
    panic = ("Kernel panic" in output and "vmctx_run_current" in output and
             "FAULT_INJECTION" in output and "NULL pointer dereference" in output)
    passed = (rounds == ["0", "1", "2", "3", "4"] and
              "BACKEND_ABSENT_PASS\n" in output and
              "SERVICE_RETRY attached=1 mm_live=1/0 killed=1\n" in output and
              "SERVICE_ALLOC_AUDIT_PASS\n" in output and
              output.count("FAULT_INJECTION") == 5 and
              not re.search(r"Kernel panic|BUG:|WARNING:|AUDIT_FAIL|AUDIT_TIMEOUT", output))
    record["correctness_pass"] = passed
    record["allocation_panic"] = panic
    record["verified_rounds"] = rounds
    record["expectation_met"] = (not record["timeout"] and record["qemu_returncode"] == 0
                                 and (passed if args.expect == "pass" else panic))
    (dest / "result.json").write_text(json.dumps(record, indent=2) + "\n")
    print(json.dumps({k: v for k, v in record.items() if k not in [
        "command", "compile_command", "sha256"]}, indent=2))
    return 0 if record["expectation_met"] else 1


if __name__ == "__main__":
    raise SystemExit(main())
