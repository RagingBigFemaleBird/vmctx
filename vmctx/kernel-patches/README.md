# The kernel-side changes, as patches

`src/` is not tracked in this repository, so the kernel source is not here.
These are the vmctx changes to the kernel image itself, exported so they can be
read without cloning the kernel trees.

They are a **generated file, not a source of truth.** The tracked form is the
kernel repository; if the two disagree, the repository is right.

## Two trees, on purpose

There are two, and they are not the same kernel:

| directory | tree | baseline | role |
|---|---|---|---|
| `./` | `src/linux-6.18.35` | `ed9484e8f` | the Intel box (10.0.0.30), the destination |
| `linux-7.0.14/` | `src/linux-7.0.14` | `add83c849` | the AMD box (10.0.0.229), the source |

`src/linux-6.18.35` has a remote
([`vmctx-kernel.git`](https://github.com/RagingBigFemaleBird/vmctx-kernel)).
**`src/linux-7.0.14` has none** — its only `remote` points at the other local
directory — so for that tree these patches are not a convenience copy, they are
the only copy outside one working directory.

The two have diverged, and the divergence is real rather than a sync that has
not happened yet:

- 7.0.14 carries the signal work (`vmctx_defer_signal_work()`, and running
  `arch_do_signal_or_restart()` inside `vmctx_do_monitor_syscall()` before the
  registers are snapshotted). 6.18.35 does not.
- That work only matters to whichever kernel holds a **service context**, and a
  destination holds none. So 6.18.35 not having it is correct while .30 is the
  destination, and is a defect the moment .30 is asked to be a source.

Do not "sync" one tree from the other. A previous attempt to do that by copying
files destroyed `src/linux-7.0.14/mm/memory.c`, which had to be reconstructed
and proved correct by reproducing the running box's md5.

## Regenerating

After **any** change under `src/`, regenerate — in the same commit as the change:

```bash
rm -f vmctx/kernel-patches/*.patch
git -C src/linux-6.18.35 format-patch --no-signature ed9484e8f..HEAD \
    -o "$PWD/vmctx/kernel-patches/"
rm -f vmctx/kernel-patches/linux-7.0.14/*.patch
git -C src/linux-7.0.14 format-patch --no-signature add83c849..HEAD \
    -o "$PWD/vmctx/kernel-patches/linux-7.0.14/"
```

The output path must be absolute: `-C` moves git into the kernel tree first, so
a relative `-o` writes the patches to `src/linux-6.18.35/vmctx/kernel-patches/`
and this directory keeps whatever it had. `format-patch` prints the names it
wrote either way, which is exactly what makes it look like it worked.

## Why this file says all that

This directory once held a single patch exported from a commit that a later
squash removed. It described 65 changed lines of the 1312 that actually differ
from vanilla, its filename named a different change from the one inside it, and
nothing said so — a stale generated file looks exactly like a current one.

That note was written, and it happened again anyway, in a worse form. Nine
kernel commits — `VMCTX_CTL_SYSCALL`, the service context, `VMCTX_CTL_ADOPT`,
the register change mask, per-page `vmctx_mprotect()`, the fault-around fix:
the entire assisted-syscall architecture — sat in `src/linux-6.18.35` unexported
while eleven stale patches sat here looking complete. At the same time the whole
of that architecture was **uncommitted** in `src/linux-7.0.14`, which is the
kernel the source box had actually been running and measured against for days.

Both are now exported and committed. The lesson is not "remember to regenerate":
it is that a directory of generated files that is *nearly* right is the failure
mode, because the count looks plausible and nothing reads them until the source
is gone.
