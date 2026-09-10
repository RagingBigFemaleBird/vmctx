# Groff pipeline corruption

Full shared-identity breadth run groffman exits 0 with empty guest stdout,
while native stdout is a 1613-byte man-page excerpt. Its shell pipeline masks
an upstream failure; the output comparison correctly rejects the run. The
original oracle remains unchanged.

A separate diagnostic script only removes man stderr suppression. Two of five
repetitions fail, with complete cleanup and counts 0/0. One prints binary stack
bytes to stderr and grotty rejects an intermediate V command before its first
p command. Another emits troff cannot-break-line warnings and wrong stdout.
This is a byte/state corruption investigation, not a formatting normalization.

The authored pipe-stream control verifies every byte and neighboring guards in
a four-stage process pipeline, with both inherited and exec-replaced address
spaces, page-crossing reads, short writes, EOF and child statuses. It passes
natively and ten remote repetitions, so it is added coverage rather than a
reproducer of the groff defect. The initial groff-stages-1 driver supplied bare executable names to an API
that needs absolute paths; its nine launch failures are excluded. The corrected
groff-stages-2 campaign passes all nine byte comparisons: three repetitions
each of intermediate generation (25,602 bytes), rendering (12,303 bytes), and
their pipeline (12,303 bytes). This narrows the reproducer to a more complete
man pipeline; it does not clear the original intermittent corruption.

The native man process trace contains nested forks and an nroff vfork/exec
path. The first local-file campaign could not read a fixture below a private
home directory after man dropped privileges; that case is excluded. The
corrected man-stages-2 campaign passes explicit preprocessing and uncompressed
local man files 5/5 each, but name lookup fails 2/5 and the head pipeline 1/5.
Compressed local man files subsequently fail 2/8, while zsoelim and an explicit
complete rendering chain pass 8/8 each.

The resulting fork-nested-io regression fails immediately on v15: pipe2 writes
descriptors into the source child's stack, but the executor reads an old stack
and attempts write(0), which fails EBADF. Tracing identifies duplicate private
pages: source recall installed a child copy even though the executor had
already preserved that child's snapshot. The snapshot also needed preservation
before a parent page handover, since source kernel writes can follow it without
an executor user write fault. Merely enumerating source child pages did not fix
the reproducer; this unsuccessful intermediate change is retained as evidence.

Frozen e82e1cd92adb (wire v16) enumerates resident native private child pages
before publishing the executor child, preserves child snapshots before TAKE
or TAKEOBJ, removes the late duplicate source-child installation, and fixes a
ctl_lock/coh_lock inversion in serve_object_mapped. It passes fork-nested-io
10/10 and seven other focused controls. The original man/head diagnostic,
with only stderr suppression removed, passes 20/20 at 1613 identical bytes.
The complete 72-case suite passes 71, with only the existing ptrace-step failure.
These results do not establish correctness of all remaining COW lifetime paths.

Raw evidence: groff-diagnostic-1-raw.tar.gz, pipe-stream-baseline-1-raw.tar.gz,
shared-identity-breadth-1-raw.tar.gz under /home/biwu/audit-20260907. Fetched
copies live at /mnt/d/VM/lan-boot-recovery-20260908/logs/audit-20260907.
Additional archives there: fork-nested-io-baseline-1-raw.tar.gz,
fork-nested-io-baseline-2-raw.tar.gz, child-ownership-controls-1-raw.tar.gz,
child-ownership2-controls-1-raw.tar.gz, man-ownership2-1-raw.tar.gz, and
child-ownership2-suite-1-raw.tar.gz. Successful runs have complete cleanup,
native mm/module counts 0/0, and zero page-loss counter deltas.
