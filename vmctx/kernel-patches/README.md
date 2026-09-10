# The kernel-side changes, as patches

`src/` is not tracked in this repository, so the kernel source is not here.
These are the vmctx changes to the kernel image itself, exported so they can be
read without cloning the kernel trees. **One patch per tree** -- each kernel
repository is a vanilla baseline commit plus ONE vmctx commit, and the patch is
that commit, regenerated on every change. See "Regenerating" below.

They are a **generated file, not a source of truth.** The tracked form is the
kernel repository; if the two disagree, the repository is right.

## The two trees

There are two, and they are not the same kernel:

| tree | box | baseline | vmctx commit (2026-09-10) | build of record | patch |
|---|---|---|---|---|---|
| `src/linux-7.0.14` | 10.0.0.229, AMD/SVM, the SOURCE (and the loopback lab) | `add83c849` Vanilla Linux 7.0.14 | `adb1174f8` | **#222**, `7.0.14-vmctx-audit-recall1-terminal222` | `linux-7.0.14/0001-*.patch` |
| `src/linux-6.18.35` | 10.0.0.30, Intel/VMX, the netboot DESTINATION | `ed9484e8f` Vanilla Linux 6.18.35 (Alpine -lts config) | `d2a1974a4` | **#71**, `6.18.35-0-lts-audit-readobj71` | `0001-*.patch` (this directory) |

- `src/linux-6.18.35` has a remote (`vmctx-kernel.git`, branches `master` and
  `port-7014-coherence`, both at the vmctx commit).
- `src/linux-7.0.14` has **no remote** -- its `old` remote points at the
  sibling directory -- so `linux-7.0.14/0001-*.patch` is the only copy of that
  tree outside one working directory.
- The AMD box builds its kernel from its own copy of the tree. Since the
  September-2026 audit that copy is `~/audit-20260907/mapping-kernel-7.0.14`
  (what `/lib/modules/$(uname -r)/build` points at), NOT the old
  `~/vmctx-build/linux-7.0.14`, which is stale and not a git tree. On
  2026-09-10 every file of that build tree that differs from vanilla was
  hash-compared with `src/linux-7.0.14` HEAD: identical (38 modified vanilla
  files, 29 added vmctx files, plus `.config`). Two stray root-level scratch
  copies on the box (`memory.c`, `vmctx.c`) and an unreferenced
  `kernel/vmctx.h` are not part of the build and are not in the commit.
- The Intel #71 kernel was built HERE, from `src/linux-6.18.35` as it stands
  (`arch/x86/boot/bzImage` in that tree is the deployed image,
  sha256 `773f4e45...`); the frozen inputs are under the recovery drive's
  `logs/audit-20260907/readobj71-inputs`.

Both trees carry the CPU/exception adapters and signal-return work. Their
memory-ownership implementations differ substantially: 7.0.14 has the source
page registry, transfer custody and the mapping log; 6.18.35 is the execution
adapter. Adding a local control to both trees does not make their source-side
feature sets interchangeable; the protocol describes source-owned state
independently of either backend.

Do not "sync" one tree from the other. A previous attempt to do that by copying
files destroyed `src/linux-7.0.14/mm/memory.c`, which had to be reconstructed
and proved correct by reproducing the running box's md5.

## What the vmctx commit of each tree contains

The commit messages (in the patches) are the summary of record. In brief:

**Both trees:** the `vmctx_run`/`vmctx_ctl` syscalls (472/473 on 7.0.14,
470/471 on 6.18.35); context lifecycle, monitor event service, register and
memory access, page take/serve/land/protect operations, assisted syscalls;
fork/exec/exit integration (a failed fork releases the context); guarded memory
supply (never invent a page locally); complete architectural CPU state in each
adapter's own XSAVE layout with a negotiated generic x86 CPU model (CPUID from
the model, guest XCR0 following it, generic vendor identity); exceptions and
debug traps delivered in the source kernel; clocks with the source; execution
CPU-time accounting (RUNTIME/BOUNDARY, killable checkpoints); RESTORE bound to
the monitor's lifetime and released only by explicit RESUME; retained context
descriptors; retained/replaced execution objects under an epoch fence;
PROTECT_MM, EXECUTOR_MAP_CAPS, SET_SHARED_PAGE, MAPOBJ_READ; quiesce leases
with PROTECT_BACKING for snapshots; mprotect preserving existing write
restrictions (`MM_CP_PRESERVE_WRITE`).

**7.0.14 only (the source):** the per-mm page record (HOME/REMOTE/CLAIM/
TRANSIT) with in-kernel claim waits; the mm-mutation change log through one
refcounted MMU-notifier subscription per mm and the optional `vmctx_map_trace`;
the robust-futex/clear-tid exit path kept in order with memory service alive;
native syscall admission (BEGIN/QUERY/COMMIT tickets, committed children
published before the parent returns); MAPPING (the VMA and a monitor-owned file
reference); /proc/cpuinfo reporting the negotiated model; INFO/OPEN during
assisted work; ABI-2 MM-lifetime queries and exact-task SIGNAL; the expected-MM
MEMORY envelope; thread-owned transfer custody (reserve before capture, replay
bytes kept until the exact ACK, abandonment before exit waits, no time-based
settlement); capture refused over an incumbent CLAIM/TRANSIT (#221); read-only
private custody through forced COW then TAKE; terminal death and monitor
attachment read under one `ev_lock` section (#222); HMM owned device-private
write checks and a `test_hmm` release drain.

**6.18.35 only (the executor):** TRYFAULT; locked page transfers drained and
failed closed; only owned private page references frozen for transfer; fault
cancellation distinguished from the deadline; the executor-side ports of
MAPOBJ_READ and write-restriction preservation (its #71).

The external module (`vmctx/kernel/`, one source for both kernels: `vmctx.c`
+ `vmctx_backend.c` + the SVM/VMX entry `.S` files) is built against each
tree; its `srcversion` (`665E2B59F7ADD5DFE313DB5` for the source of record)
is what ties a LOADED module to this tree -- `verify-build.sh` checks it.
The Intel #71 module was built from these sources with the previous
`vmctx_executor_map.h` (sha256 `fe2b42a5...`; the tree's is `9c079cde...`,
which adds the PRESERVE_WRITE capability bit the executor advertises through
its kernel patch instead); the next Intel module build from this tree closes
that gap.

## The audit series that led here (2026-09-07 -> 09-09)

The single commit per tree was built up as a series of working-tree patches
during the September-2026 audit -- 0002-0046 for 7.0.14 (builds #182-#222),
0002-0029 for 6.18.35 (builds #62-#71) -- each proven to replay its frozen
candidate inputs byte-for-byte before the kernel was built and booted. Their
per-patch descriptions, build numbers and native-control counts are preserved
verbatim in
`../doc-archive/audit-20260906-09/kernel-patches-README-presquash-20260910.md`
(the last state of this README before the squash) and in the design ledgers
beside it; the raw build/boot/control evidence is on the recovery drive under
`logs/audit-20260907` (`terminal222-*`, `readobj71-*`, `*-patch-proof.json`).
The `support/` headers that accompanied the series were the tree's own
`kernel/vmctx-*-core.h` files at various steps and are now inside the patch.

## Regenerating

After **any** change under `src/`, regenerate -- in the same commit as the
change. Each tree is vanilla + ONE vmctx commit, so the whole delta is one
`format-patch -1`:

```bash
# 7.0.14 (the source box): baseline add83c849 + one commit
git -C src/linux-7.0.14 format-patch -1 -o /home/desktop/lan-boot/vmctx/kernel-patches/linux-7.0.14 HEAD

# 6.18.35 (the netboot destination): baseline ed9484e8f + one commit
git -C src/linux-6.18.35 format-patch -1 -o /home/desktop/lan-boot/vmctx/kernel-patches HEAD
```

Absolute `-o`: `format-patch` resolves a relative path against the kernel
tree, not the current directory. Delete the previous `0001-*` first when the
subject line changes, or two patches sit side by side and the count looks
plausible.

Prove the patch before trusting it -- without a second checkout:

```bash
cd src/linux-7.0.14
GIT_INDEX_FILE=/tmp/idx git read-tree add83c849
GIT_INDEX_FILE=/tmp/idx git apply --cached --check ../../vmctx/kernel-patches/linux-7.0.14/0001-*.patch
GIT_INDEX_FILE=/tmp/idx git apply --cached          ../../vmctx/kernel-patches/linux-7.0.14/0001-*.patch
[ "$(GIT_INDEX_FILE=/tmp/idx git write-tree)" = "$(git rev-parse HEAD^{tree})" ] && echo identical
```

(same for 6.18.35 with `ed9484e8f`). Both patches passed this on 2026-09-10.

Folding a new change into the single commit: commit it, then
`git reset --soft <baseline> && git commit` with the updated message, author
AND committer `Bi Wu <biwu85@gmail.com>` (the convention of the whole
project); push 6.18.35 to both remote branches with `--force-with-lease`.

## Why this exists at all

On 2026-08-03 eleven patches sat here looking complete while nine commits'
worth of kernel work -- the entire assisted-syscall architecture -- existed
only as uncommitted changes in `src/linux-7.0.14`, the kernel the source box had
been running and measured against for days. The count looked plausible and
nothing read the files. On 2026-09-09 the same shape recurred one level up:
the box's build tree was the verified source and the local git tree was
older -- the audit's handoff said so in as many words -- and the squash had to
start by pulling the box's files back. The lesson is not "remember to
regenerate": a directory of generated files that is *nearly* right is the
failure mode, and the check that catches it is a hash comparison of every
changed file between the tree that BUILT the running kernel and the tree that
is COMMITTED (the recipe is in HANDOFF.md §1).
