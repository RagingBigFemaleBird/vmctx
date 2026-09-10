#!/usr/bin/env python3
"""Check that lo1 rejects a missing buffer transfer and a failed join.

Compile the actual test with link-time fault injection. The join shim waits
for the real worker before returning an error, so injected failures leave no
thread behind. No vmctx kernel, privileges or network are needed.
"""
import argparse
import json
from pathlib import Path
import subprocess
import tempfile

SHIM = r"""
#define _GNU_SOURCE
#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
ssize_t __real_write(int, const void *, size_t);
int __real_pthread_join(pthread_t, void **);
ssize_t __wrap_write(int fd, const void *p, size_t n)
{
    const char *fault = getenv("ORACLE_FAULT");
    if (n == 4096 && fault && !strcmp(fault, "write")) {
        fputs("INJECTED write failure\n", stderr);
        errno = EIO;
        return -1;
    }
    return __real_write(fd, p, n);
}
int __wrap_pthread_join(pthread_t thread, void **result)
{
    int rc = __real_pthread_join(thread, result);
    const char *fault = getenv("ORACLE_FAULT");
    if (!rc && fault && !strcmp(fault, "join")) {
        fputs("INJECTED join failure\n", stderr);
        return EINVAL;
    }
    return rc;
}
"""


def main():
    import os
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--source", type=Path,
                        default=Path(__file__).resolve().with_name("lo1.c"))
    args = parser.parse_args()
    results = []
    with tempfile.TemporaryDirectory(prefix="vmctx-oracle-") as name:
        directory = Path(name)
        shim = directory / "shim.c"
        binary = directory / "lo1"
        shim.write_text(SHIM)
        subprocess.run(["cc", "-O2", "-Wall", "-Wextra", "-static", "-pthread",
                        str(args.source), str(shim), "-Wl,--wrap=write",
                        "-Wl,--wrap=pthread_join", "-o", str(binary)], check=True)
        for fault in ("none", "write", "join"):
            result = subprocess.run([str(binary)], capture_output=True, text=True,
                                    env={**os.environ, "ORACLE_FAULT": fault}, timeout=10)
            if fault == "none":
                ok = result.returncode == 0 and "PASS" in result.stdout.splitlines()
            else:
                ok = (f"INJECTED {fault} failure" in result.stderr and
                      result.returncode != 0 and "PASS" not in result.stdout.splitlines())
            results.append({"fault": fault, "oracle_correct": ok,
                            "returncode": result.returncode,
                            "stdout": result.stdout, "stderr": result.stderr})
    print(json.dumps(results, indent=2))
    return 0 if all(row["oracle_correct"] for row in results) else 1


if __name__ == "__main__":
    raise SystemExit(main())
