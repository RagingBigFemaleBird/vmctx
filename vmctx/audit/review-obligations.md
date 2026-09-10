# Review obligations

This consolidates the historical kernel, userspace, test and infrastructure
reviews. Items are source concerns or incomplete proof obligations, not claims
that every old defect remains in current code. Passing focused repairs are
listed in [README.md](README.md). Retire an obligation only with current source
review and an effective control; old comments and successful workload samples
are insufficient. Full original findings and source read ledgers are preserved
in the verified archive named there.

## Native execution and lifetime

- Registry admission, allocation failure and retirement must be transactional:
  no leaked MM after failed fork, slot reuse before XArray destruction, silent
  capacity bypass, raw task/MM lifetime assumption or unpropagated allocation
  failure. ADOPT must serialize initialization against task exit/exec.
- WAIT, assisted calls, register writes, monitor attach/detach and terminal
  reads must refer to one retained event transaction. A timeout cannot permit
  reuse while the native operation continues. Include duplicate waiters,
  failed child/backend allocation, monitor succession, PID reuse and nonleader
  exec. #222 specifically repairs terminal authorization observation.
- Native source entry policy, ptrace/seccomp and true fork/vfork/CLONE_VM
  semantics must survive forwarding. Intel is currently an executor; its older
  source exec hook placement is not validated as a source implementation.
- Source exit begins before asynchronous io_uring/device references necessarily
  drain. Context completion cannot release page ownership on a task-scheduling
  or elapsed-time assumption. Clear-TID/futex handling needs private, shared,
  inode-backed and PI semantics; thread group is not general MM identity.
- Guest entry/exit must preserve full CPU state and clocks and satisfy rseq,
  RCU/context tracking, scheduler accounting and speculation contracts.
  Validate interrupt and nested-signal behavior, not just register roundtrips.
- VMX active-VMCS retirement, migration, waited-IPI races, preemption and CPU
  hotplug need a complete lifecycle proof. SVM initialization must restore
  VM_HSAVE_PA. Verify required physical address/large-leaf capabilities and
  VMCS operation errors; #NM/#MC behavior needs an architectural contract.

## Memory ownership

- Replace pte_none remote residency and establish the complete requirements in
  [ownership.md](ownership.md). This includes UFFD, THP, mremap/DONTUNMAP,
  fork, COW, discard, faults/GUP, migration, swap, KSM, NUMA, special mappings,
  native/device pins and secondary TLB revocation.
- TAKE/copyout/TAKEOBJ failure cannot destroy the sole recoverable page. Landing
  must be atomic for the retained MM/mapping and exclude replacement or exec;
  a sampled reference count, PTE or page-cache presence is not custody proof.
- Permission reductions, guards and partially failed mmap/mprotect/madvise
  operations must publish actual native committed effects. Guard state is not
  just VMA flags; stack/brk growth and direct MM mutations also require coverage.
- Shared objects include offset identity, lifetime, aliases, truncate/regrow,
  partial hole punching, external file I/O and async kernel/device writes.
  Private COW pages and shared page-cache contents have different semantics.
- Every transfer path must validate full lengths, nonoverlapping byte coverage,
  reply status and exact receipt. ACK follows complete storage; send attempts,
  counter thresholds, retained bytes or generations cannot confirm ownership.
  Include GET/GETS/UPGRADE/COWBREAK, PUT/PUTN/PUTV, prefetch, give-back and failures.
- Fork preservation must establish birth contents before grants, report failed
  gives and cover all children without silent caps. Never substitute current
  ancestor bytes for a missing fork snapshot. Async old-image work must be
  fenced before it can overwrite/punch a replacement image.
- Audit complete lock ordering across layout, coherence, page claims, ctl,
  backing objects and native MM locks. No network wait under locks callbacks
  need. Trace publication, volatile counters and mutable map pointers do not
  supply synchronization or a total order.
- Diagnostic reads may instantiate absent memory. Keep watches noninvasive,
  check all bytes where zeros are claimed, and never use arbitrary-word or
  lossy-table heuristics as independent proof of a lost store.

## Runtime and transport

- Retained context/MM/object identity must reach all routes, caches and error
  cleanup. No numeric PID/truncated-key alias, silent registry/table cap,
  forgotten tombstone chain, stale image reply or skipped allocation error.
- Child publication and monitor ownership must cover creation failures and
  completion. Descriptor/stack/backing/channel references must drain, including
  inherited descriptors above 1024 and double-fork/daemonized descendants.
- Framing errors and oversized/truncated replies end the connection. Diagnostic
  RPCs must preserve the original page receipt and CPU/mapping reply metadata.
  Connection teardown must retain or explicitly abandon unresolved custody.
- The lab transport has no general authenticated peer/session boundary.
  START/CHILD/control operations must not confer authority through a claimed
  PID. A production transport and session capability design remains separate.
- vDSO/vvar compatibility cannot be inferred from uname alone; one monotonic
  source clock domain must cover syscall and vDSO paths, including exec and
  time namespaces. Migration tools need complete versioned CPU/memory images,
  exact record sizes, complete reads and atomic publication.

## Test and operational coverage

- Require real kernel consumption (pipe/socket byte comparisons, not /dev/null),
  completed rounds from every worker, successful create/join/I/O/wait calls,
  verified child exit and complete byte oracles. Volatile publication, sleeps,
  equal failures, empty output or printed PASS cannot satisfy these claims.
- Remaining historical test concerns include hx1/hx2/hp2 evacuation progress,
  dma1/io1 I/O error/round reporting, ex1/ex4/ex5 child status, fm1 unavailable
  fixtures, rf1/rf2 priming/collisions, pk1 alignment, sh2 timeout/read oracle,
  th9/th12 worker checks, namespace skips, shared-memory cleanup, actual signal
  return and stack probing. Several other controls were strengthened; verify
  each exact current test before treating this list as closed.
- Grade source/executor diagnostics as well as stdout and status. A passed
  byte test with stale-serve, failed LAND, lost receipt or unresolved claim is
  still a failure. Normal browser exit and page acknowledgements are required;
  chrome, window existence, screenshots or killing a stuck browser do not pass.
- Each run owns its processes and unique files/port. Use pidfd or retained
  parent-child ownership, complete descendant reaping, exact boot/binary/input
  checks, scoped kernel capture and before/after live-resource checks.
  Historical global-kill/dmesg-clear wrappers are unsuitable for these gates.
- Package immutable complete kernel/initramfs/module releases and verify their
  closure before selecting them. A successful SSH connection is not proof of
  reboot. Keep default/recovery boots, compare boot IDs and avoid partial
  publication. Package mirrors need a package-manager-resolved verified closure.
- Performance requires successful runs, alternating same-boot arms, actual
  execution accounting and per-CPU governor restoration. Failed runs, armed
  traces, harness time and earlier kernel builds are not current speed gates.
