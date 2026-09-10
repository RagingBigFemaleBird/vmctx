# Session 45 — full audit, the VMX base-read bug, redeploy both boxes

Directive (verbatim): "completely audit this project and check if everything
makes sense and optimize and get all tests green. Question everything! Deep
read all code. Do NOT be afraid to make big architectural changes for
performance reasons or for correctness reasons." Continues session 44/44b/44c
(HANDOFF-session43.md holds that trail; repo = one root commit `5a58904`).

## What the deep read covered

Everything: both user halves in full (`vmhome.c` 8089 lines, `vmremote.c`
17977 lines), the module (`vmctx/kernel/vmctx.c` 4620), both core-kernel
trees (`src/linux-7.0.14` and `src/linux-6.18.35`, the source-side patch and
every hook), all shared headers, every test script and every test C case,
the small tools (vmmon/vmmig/vmtake/vmsys/vmthreads/vmbench/dumpvm/homeapp/
vmselftest), and every deploy/build/verify script. Notes went into the
running conversation; the durable findings are here and in AUDIT §10 #27.

## The one real bug found and fixed

**The VMX guest FS/GS base read after `preempt_enable`** — Intel destination
only, ledger #27. `vmx_run_usercode_once()` read `GUEST_FS_BASE` and
`GUEST_GS_BASE` back out of the VMCS *after* `vmx_enter_once()` had already
called `preempt_enable()`. A preemption in that window migrates the task to a
CPU where this VMCS is not current; `vmcs_read()` then answers for whatever
VMCS that CPU holds — a sibling context's, or none (a VMfail that reads 0) —
and the wrong base is written into `vc->guest_fsbase`, then fed to the guest
on its next entry. That is a guest thread silently running on another
thread's TLS: the VMX twin of the classic SVM bug #3 (ws1's "lost stores").
The same window covered the EPT-violation diagnostic's
`vmcs_read(GUEST_PHYSICAL_ADDRESS)`.

Fix (in `vmctx/kernel/vmctx.c`, VMX backend only): read the bases (and, for
an EPT exit, the GPA) inside `vmx_enter_once()` before `preempt_enable`, into
new/existing `r->fs_base` / `r->gs_base` / `r->gpa`; the save-back and the
diagnostic now use those. The struct's own comment already says every VMCS
field is captured before release — these were the exception. SVM was always
correct: it saves back from the VMCB, which is memory, not CPU-current state.
Verified: every remaining `vmcs_read` in the module is inside
`vmx_enter_once`, under `preempt_disable`. The reads sit after the
`r->gctx.fail` check, on the clean-exit path only.

Direct validation on the Intel box (cross-machine): **fs1 PASS** — it asks
each thread for its own FS base via `arch_prctl(ARCH_GET_FS)`, which is
exactly what a wrong base breaks; th9 ran 40 main + 40 thread lines.

## Build and deploy (both boxes, this tree)

- Module + tools rebuilt locally against `src/linux-6.18.35` and staged into
  `http/vmctx/` (`build.sh`); pushed to the Intel destination .30 and the
  module reloaded (`/usr/local/bin/vmremote`, `/tmp/vmctx.ko`). `verify-build
  intel` all-ok; the previous binaries kept aside as `*.pre-s45`.
- `MODULE=1 sync-amd.sh` rebuilt userspace, tests and the 7.0.14 module on
  the source .229 and copied it to `~/vmctx-7.0.14.ko`. Source id
  `336659dc789e` on both boxes; verify-build amd all-ok except "module not
  loaded" (the suite's own runany insmods it per run).
- The AMD box had been powered off at session start; a wake-on-LAN magic
  packet brought it back (it is on the LAN, MAC b0:25:aa:43:db:68).

## Test results

- **Loopback suite (`suite.sh 3`, source id 336659dc789e): 119/120.** The one
  failure is **fc1**, in round 2 only, with an empty guest.out. Reran fc1 x8
  serially: all 8 printed PASS in guest.out (graded green), 4 of 8 still
  returned rc 242 — the churn thread dies, main prints PASS first. See below.
- **Breadth campaign (`progs.sh`): 69 pass, 0 fail, 0 skip.** The three known
  gaps (gpgsym, gdbbt, stracec) XFAIL as documented. First clean zero-fail
  breadth run this tree records (was 67/2).
- **Cross-machine suite (`RUNANY=runany-x.sh suite.sh 1`, source .229 →
  destination .30): 40/0.** The FS/GS fix validated on real VMX hardware —
  every case, guest executing on the Intel box, syscalls forwarded to the
  AMD source. fs1 (per-thread base) and th9 (40+40 threaded) both green.

## fc1 — the residual left in place (AUDIT §10, after #27)

fc1 is the fontconfig mmap/munmap churn case: one thread storms anonymous
mmap+munmap while main maps/reads/unmaps three 2 MiB files. Under that vacate
storm the churn thread ends up stuck read-faulting a *program-image* page —
its own `.text`/`.rodata`, e.g. `0x4e1000` in region `0x4d2000-0x4e8000` —
which never lands: asked of the source over and over ("faults on 0x4e1000
over and over (50 times)"), no progress, then `-EFAULT` (exit 242). Main
usually reaches its `printf("PASS")` before that death (so the suite, which
grades on guest.out containing `^PASS$`, scores it green), but occasionally
the death precedes the print and guest.out is empty (the round-2 FAIL).

This is the vacate-ordering / deep-teardown family (#12, #16, #17, and the
session-37 thread-list class). The `cons`/REPAIR machinery catches the
`0x7fff…` range faults (4 REPAIRs and 8 VACATE-LATE skips in the failing
run, all made harmless) but not this contention on a shared program page
between the two live contexts. It was **not** touched: the producer needs an
instrumented reproduction to root-cause, and a speculative change to the
vacate machinery is exactly what has regressed other cases before
(PRINCIPLES: instrument before diagnosing; one thing per build; delete the
A/B arm once proved). It is a pre-existing flake, unrelated to the VMX fix
(which is Intel-only; the loopback suite runs on AMD/SVM).

## State of the world at handoff

- Repo: one root commit `5a58904`; the session-45 changes are the VMX fix in
  `vmctx/kernel/vmctx.c`, the AUDIT §10 #27 entry, and this file. **Not yet
  committed** — commit per convention (squash after origin/master into ONE
  commit, author+committer "Bi Wu <biwu85@gmail.com>").
- Kernel patches: the module change does **not** touch either core-kernel
  tree, so `kernel-patches/` is unchanged. (The bug and fix are entirely in
  the out-of-tree module.)
- Boxes: .229 (AMD/SVM source, 7.0.14-vmctx, 472/473) up and current; .30
  (Intel/VMX destination, 6.18.35-0-lts, 470/471) up, module + tools current
  with this tree, `*.pre-s45` asides kept.

## Next

- Confirm the cross suite's final tally (40/0 expected; the FS/GS fix should
  if anything help a threaded case on VMX, never hurt).
- fc1: if pursued, instrument the churn thread's 0x4e1000 fault path — is the
  source answering CLAIMING in a volley, or is the shared object being
  punched at a program address by a spurious vacate? Reproduce with
  `VMR_PGLOG`/`VMR_TRAIL_PAGE` aimed at the stuck address before changing
  anything.
- The kernel #179 monitor-succession fix (7.0.14) is **present in 6.18.35
  too** (verified: both trees have "monitor gone; the event waits for a
  successor", `vmctx_pgrec_land`, the entry-gate refusal, the killable-fault
  wait). No destination kernel rebuild is needed for it.
