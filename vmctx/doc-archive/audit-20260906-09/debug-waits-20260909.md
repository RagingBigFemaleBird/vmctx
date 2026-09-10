# Debug delivery and elapsed event deadlines

This is ongoing work. Full guest suite on AMD #202 / frozen userspace
`e82e1cd92adb` passed 73/75: `debug-traps` and `ptrace-syscalls` failed.
Focused `ptrace-step` and `uring-rings` passed. Every test released its contexts,
module references returned to zero, and the page-loss counter stayed unchanged.
Raw evidence is on D under
`VM/lan-boot-recovery-20260908/logs/audit-20260907/`, notably
`debug202-guest-gates-1-raw.tar.gz`, `debug202-guest-live-kmsg.log`, and
`debug202-userspace-complete.json` with executable SHA-256 identities.

## Debug traps

Intel module VM-exit capture omitted EXIT_QUALIFICATION for #DB and instruction
length for privileged software exceptions (ICEBP). These reads were fixed before
booting Intel #65. Intel SDM volume 3C specifies those fields:
https://cdrdv2-public.intel.com/825750/326019-sdm-vol-3c.pdf

AMD's #DB exception intercept does not intercept ICEBP. A dedicated instruction
intercept is required (AMD APM volume 2, 15.12.2 and 15.9):
https://docs.amd.com/api/khub/documents/sD1_QL~h4Afq2_tvzxqqSQ/content

The failing guest successfully delivered the TF trap, returned from its signal
handler to the correct `step_after` PC 0x4017b8, then faulted reading address 0x10
while executing ICEBP at 0x4017b9. That is the absent guest IDT's vector-1 gate.
The module's two warnings about a withdrawn/replayed syscall at rt_sigreturn
were false: a source response may legitimately replace RIP. The module now
checks its loaded RIP against the source-returned frame rather than assuming a
sequential syscall return. The source owns signal-return semantics.

The AMD module candidate enables INTERCEPT_ICEBP, uses hardware next_rip (with
length bounds) for its trap PC, and reports vector 1 with zero architectural
cause. It does not read stale DR6 for an instruction intercept. The permanent
`tests/control/debug-traps.c` regression now includes operand-size-prefixed
ICEBP as well as TF and plain ICEBP. The extended native control passed.

## Cancellation is not a deadline

`vmctx_report` used killable waits for taken FAULT events but ignored a fatal
signal after an immediate negative return. It incremented elapsed time by one
poll interval on every spin, generating a six-second deadline at zero elapsed
time. Native functional controls passed while this diagnostic was wrong.
The previous BUG/WARNING/Oops-only log scan did not catch the false deadline.

`tests/control/event-wait.c` takes an unanswered terminal fault and distinguishes
SIGKILL cancellation from an actual configured fault deadline. Intel #65
baseline: cancellation failed (0 ms, counter +1); real timeout passed (6,139 ms,
counter +1). Evidence: `event-wait-debug65-baseline/rows.json` and dmesg.

Patches 0024 (6.18) / 0028 (7.0.14) end fatal fault cancellation with ACT_KILL,
never RETRY or local fault service. Deadlines measure jiffies since publication.
A taken syscall's completion/ownership rules are unchanged. The timeout message
now describes the actual termination rather than claiming it delivers SIGBUS.

## Current verification

AMD #203 booted as `7.0.14-vmctx-audit-recall1-runtime199`, boot identity
`d9f4e03f-8808-4a98-a7f5-4cc72364f408`. It includes the elapsed-wait patch and
ICEBP module. Source inputs, prior images, symbol CRC comparisons, and artifact
hashes are saved in `debug203-inputs/`, `debug203-before/` on the AMD host and
`debug203-build-complete.json`. No default boot image was promoted.

Its cancellation regression passed: SIGKILL, 0 ms, deadline counter delta zero.
Full native and guest reruns are still in progress. Intel #66 is building from a
separate frozen snapshot with the same wait repair. No syscall-admission repair
has been implemented yet. Do not call this state green.

### Completed candidate verification

AMD #203 passed all nine native checks, including 1,000 contested private-file
TAKEs. Its real deadline control passed at 6,100 ms with exactly one counter
increment; cancellation passed at 0 ms with no increment. Extended debug traps,
ptrace single-step and io_uring passed in loopback and AMD-to-Intel execution.
Intel #66 booted as `6.18.35-0-lts-audit-debug65`, boot identity
`5c35fa0e-90bb-4068-be95-59cae01ef24e`, and passed the same nine native checks.
Each cross case ended at zero contexts/references and zero loss delta on both
hosts. Evidence: `debug203-guest-gates-1-raw.tar.gz`, `debug203-cross-driver.log`,
`debug66-intel-native-1/complete.json` and the associated live kernel logs.

Intel #66's symbol validation initially stopped on a stale `vmlinux.symvers`
left by the stock build. The completed #65 combined kernel/modules build wrote
`Module.symvers`, which was the authoritative table used by the deployed
modules. All 12,045 core entries in that table exactly match #66's generated
`vmlinux.symvers`; no differing/missing/extra entries. The manifest records this
comparison. Original boot files and the stale table are preserved.

The module now also fixes three format warnings, allocates the SVM selftest's
2.5 KiB run state off the kernel stack, and uses UNWIND_HINT_SAVE/RESTORE across
the VMX hardware exit continuation, matching native KVM's vmenter.S pattern.
A frozen build against Intel #66 passed with no compiler or objtool warnings:
`debug66-unwind-module/inputs.json`, `debug66-unwind-build.log`. That module has
not yet replaced the tested running modules; it is in the #204 candidate.
