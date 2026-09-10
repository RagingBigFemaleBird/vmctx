# Test and helper audit

This records reviewed source and specific oracle checks. A test's historical
PASS is not proof that every mechanism claimed in its comments was exercised.

## Buffer consumption and lifetime

`lo1`, `dl1` and `hp2` use `/dev/null` writes to claim that the source read a
buffer or primed a file alias. Linux `drivers/char/mem.c:write_null` simply
returns the count. The native `control/kernel-buffer-read.c` check confirms
that a write of 64 bytes from PROT_NONE memory succeeds there, whereas the
same write to a pipe returns EFAULT and leaves it empty. The existing pipe
parts of dl1/hp2 still exercise real buffer accesses, but the alias-priming
and other null-write claims do not follow from their successful returns.

`lo1` also passed after an injected failed write or join. Its local repair
uses a nonblocking pipe, verifies all bytes the source consumed, requires the
worker's transfer and join to succeed, and then checks the surviving stack
page. `oracle-failures.py` compiles the actual test with link-time injection:
the old source passes both errors incorrectly; the new source rejects both
and passes its native positive control. This changes the workload; guest
validation and a new hardware baseline are pending. Results are preserved in
ignored `logs/audit-20260906/loan-oracle-{before,after}.json`.

## Remaining oracle gaps

- `hx1`/`hx2` can pass after an evacuator's pipe creation, write or read fails.
  They do not require evacuation progress or successful joins. hx2's sleep
  does not bound its later joins, so it is not unconditionally wall bounded.
- `hp2` can pass after pipe failure, an early failed null write, or failed
  pipe I/O; allocation failures also silently reduce work. Its paint after a
  pipe read overwrites the rest of that allocation before verifying it again.
- `dma1` breaks on failed lseek without adding a failure and reports the
  requested round count. Its shared fixed `/var/tmp` filename makes parallel
  runs race over truncation and unlink. Opening with O_DIRECT alone is not
  evidence of device DMA (for example, a filesystem may ignore that flag).
- `an1` reports whether the mapping is named but does not require naming to
  succeed. Its heuristic counter ratio and freeze threshold can also reflect
  scheduling. It does not establish linearizable reads.
- Many threaded cases use volatile stop/publication fields across threads
  without C atomic synchronization and ignore join failures. mf2 makes its
  sibling-stack assertion conditional on a pointer that need not be published
  before fork. Sleeps are not evidence that a worker reached its intended state.
- `ex1` ignores waitpid errors with an initially zero status. ex4 never
  propagates its worker failure or child status to its final return. ex5
  prints disagreement but returns zero. Their runner must grade the actual
  assertions as well as process status.
- `fm1` may pass if every fixture is unavailable; its offset mapping leaks.
  `fc1` requires scan progress but not the address reuse its claim relies on.
  `io1` can pass after lseek failure or zero completed reads. Its byte pattern
  repeats every 256 bytes, so it cannot identify a wrong page within a block.
- gp1 records si_code but does not assert the promised code. Signal tests
  using siglongjmp do not exercise an actual rt_sigreturn. mf1/mf2's tail-call
  recursion does not by itself establish the claimed depth of return frames.
- `pg1` writes a formatted stack buffer rather than the shared mapping it
  claims to hand to the source. pg2 can pass after early write failure once
  any sample succeeded. pg3 counts neither completed work on both sides nor
  failed writes, so it can pass with zero transfers. Its printed round count
  describes only main even when it stops the other worker early.
- `rf1` has the same null-write priming problem. Its claimed chunk geometry
  also does not establish chunk alignment. rf2 uses three fixed /tmp names,
  which collide in parallel runs. pk1's aligned boundary can leave its path
  below the stack array when the array begins within 48 bytes of that boundary;
  this can corrupt the control's own stack. Its comments correctly record
  that it did not discriminate the historical fix.
- `sh2` never checks the values B read from A, and a spin timeout sets failed
  without setting the final bad flag, permitting PASS after a timeout. Its
  volatile spin is not C atomic synchronization. Thread-output cases th9 and
  th12 ignore create/join failures and do not independently require both
  workers to make progress.
- `ns1` and `pid1` print PASS after EPERM/EINVAL skips. These results do not
  establish successful namespace creation or the behavior of its child. sm1
  uses a global POSIX-shm name and can hang on failed fork while retaining both
  pipe ends; some string operations there read 16 bytes from 15-byte literals.
  Shared-memory cases generally need stricter child reaping and error cleanup.
- `vd1` compares only coarse plausibility across sites and not one monotonic
  time domain across syscall/vDSO paths. A process cannot safely alternate two
  unrelated boot-relative clocks merely because each advances. stk1 touches
  an already reserved C frame upwards; it does not by itself establish a
  page-by-page downward stack-clash probe sequence.

## Helper tools

- vmmig and vmmon declare a 152-byte register record, while both kernels copy
  a 168-byte record including FS/GS. vmmig's GETREGS writes beyond its image
  header and SETREGS reads beyond its record. Migration also lacks full xstate,
  schema/CPU identity, complete-read enforcement and durable atomic image
  publication. Fixing only the struct size would not make migration complete.
- vmtake's content check tests endpoints and absence of zero, not every byte
  against the pattern. It spins without checking child death, ignores wait
  status and leaks its mappings between cases. Its expected guest exit also
  relies on the obsolete local-syscall mode. vmselftest's result is only the
  small SVM sandbox control, not program correctness.
- homeapp's arithmetic loop can be constant-folded. vmbench's compute and
  syscall extremes do not establish near-native browser performance; its
  guest clock is itself a mechanism under audit.

## Campaign harnesses

- census and several pools grade any PASS line without verified terminal
  child status. Missing counters can disappear from summaries; a count of
  matching diagnostics does not independently establish a cause. Fixed-address
  WATCHWORD diagnostics are not portable test assertions.
- gate/chain ignore intermediate failures and report completion anyway.
  prun still compiles vmremote directly, bypassing the new build fingerprint.
  Reused output directories and fixed port ranges can mix simultaneous runs.
- perf includes failed runs in medians, discards native errors, reuses append
  files across reruns and substitutes harness time for missing program time.
  Its governor cleanup replaces the HTTP cleanup trap and assumes every CPU
  had CPU0's original governor. perfab drops runner status and can shift parsed
  columns when accounting is missing. Neither is a valid speed gate as written.
- progs can pass equal failures or empty pipeline output; pipelines generally
  grade only their last process. gdb's unconditional echo is not evidence that
  a breakpoint was hit. Existing artifacts may survive reruns. Clock checks
  are weaker than their descriptions, and timeout1 compares an inner timeout
  with a harness timeout of the same status. Use explicit per-case success and
  completion predicates with fresh artifacts before accepting these grades.
- xrun/xbrowser repeat the broad-kill, shared-file and shell argument issues
  in infrastructure-review.md. xbrowser does not return its saved runner
  status and omits the destination-release vDSO choice used by xrun.
