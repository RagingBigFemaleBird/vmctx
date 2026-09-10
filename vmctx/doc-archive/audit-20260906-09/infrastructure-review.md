# Boot, deployment and harness review

These are source findings from the current audit. They are not completed
repairs or evidence that a particular browser failure came from these scripts.

- `tools/build-kernel.sh` publishes the modloop before its initramfs and
  kernel. It writes the initramfs and kernel directly to served paths, so a
  boot can fetch a partial file or mix generations. Missing boot modules only
  warn. `update-alpine.sh` similarly advances three boot files separately.
  Publish a verified, immutable release directory and switch the boot script
  only after the complete kernel/initramfs/module set is available.
- `mirror-apks.py` publishes the new index before its package closure exists.
  Dependency constraints are discarded, the first provider wins, conditional
  installation is not resolved, and missing providers only warn. Cache reuse
  checks size alone. A version-pinned repository and the package manager's
  solver should determine the verified closure before publication.
- `start.sh` leaves the HTTP server running if dnsmasq startup fails. The
  HTTP server binds all interfaces and dnsmasq runs as root. The boot path
  downloads unsigned custom kernel/module content over HTTP. These choices
  require an explicit lab trust boundary; they do not provide authenticated
  deployment to arbitrary machines on a network.
- `stop.sh` trusts PID files without checking process birth or executable
  identity, then falls back to broad name matching. `local-run.sh` and
  `remote-run.sh` kill project process names globally, clear dmesg and use
  shared output paths. Concurrent runs can end or overwrite each other.
  These scripts are unsuitable for preserving this audit's measurements.
- `local-run.sh` defaults to the AMD address but root login and the default
  470/471 destination binary. AMD uses 472/473. Module checksums do not prove
  ABI compatibility or identity of the running kernel integration. Remote
  commands also interpolate program arguments and environment values as
  shell source, losing their original argument boundaries.
- `setup-target.sh` and the boot local service download executable files
  directly into their final paths. Partial fetches can mix generations. The
  local service can log that vmctx loaded after both load attempts failed;
  an already loaded module may also be older than the fetched tools.
- `reboot-target.sh` can report success without proving that a reboot occurred:
  it ignores the reboot command's result and only waits for SSH availability.
  Compare boot ids and the intended kernel/module identities after observing
  the old boot disappear. Its default target discovery is limited to one
  hard-coded subnet and HTTP-log history.
- `browser-demo.sh` ignores the runner's status and grades a window plus a
  larger distinct-color count. Browser chrome alone can satisfy that check.
  Its XWD parser treats the color table as pixels and assumes four bytes per
  pixel. The shared D-Bus socket also allows runs to terminate each other's
  bus. The new browser audit instead requires page acknowledgments for the
  actual workload and records process exit and teardown outcomes.
- `fakebwrap/bwrap` intentionally removes sandboxing for selected image
  loaders. This is recorded behavior of the demo, not a passing sandbox test.
- `wedge.sh` treats the first syscall argument as a descriptor even for poll,
  futex and select shapes where it is a pointer or another kind of argument.
  Its attribution can be misleading. `wedge5.sh` parses `/proc/PID/stat` by
  whitespace, which is not valid for task names containing spaces, and picks
  only the first matching monitor. Neither yields an atomic process snapshot.

The AMD failed-fork control also exposed interference with native host exec.
Prior hardware runs on the same boot retained 28 mm records and old blocked
tasks. Stable before/after counts establish neither a clean host nor the
absence of stale pointers. Repeat the key native/guest controls after recovery
before using that host for timing comparisons. Local table benchmarks run on
the separate WSL kernel are unaffected by this AMD registry defect.
