# Locked-page transfer failure

The byte-io control distinguishes ordinary from mlocked anonymous memory with
identical adjacent one-byte pipe reads. Native both pass. On AMD #200, remote
ordinary memory passes; mlocked memory returns 1 from read while keeping the
old sentinel. It has zero runtime checkpoints. GPG's passphrase-fd failure is
the same shape; direct passphrase encryption/decryption succeeds.

The source TAKE prechecks a folio, unmaps it, then freezes its exact reference
count before copying. Unmapping a locked page itself queues munlock_folio and
adds a deferred reference. The final claim skips drain for LRU folios, whereas
the precheck already allows draining them. The post-zap retry never drains
this new reference. It eventually drops its own pin and returns EBUSY after
losing the anonymous mapping. The HOME/absent retry counter eventually turns
that loss into an ABSENT answer; executor retained bytes hide the loss as stale
program data. Both operations need architectural treatment:

1. Drain native deferred ownership references before the exact-count claim,
   regardless of LRU membership. Keep the exact freeze and DMA-pin checks.
   This uses native memory-manager knowledge, not source syscall decoding at
   the executor. Audit lock ordering for both TAKE and TAKEOBJ callers.
2. A destructive transfer failure is not retryable absence. Preserve the page
   or terminate its owning context; never let it reach stale-copy or zero-fill
   fallback. Consider retained native transfer ownership or migration-entry
   rollback for general transient pins. Bad monitor copyout also needs the
   same preservation invariant. A sleep/retry-count increase is insufficient.
3. Preserve mlock residency semantics separately from byte coherence. The
   source adapter owns limits and native flags; executor residency requirements
   would need an explicit portable memory contract, not remote decoding of
   Linux mlock. Current tests establish byte corruption, not full distributed
   residency guarantees.

Validation: byte-io and GPG's original pipe-fed roundtrip; source bytes and
neighbors after each read; complete cleanup; delta of vmctx_take_lost must be
zero. Existing page, concurrent writer, pin and lifecycle tests must remain
green. Raw evidence: logs/audit-20260907/byte-io-baseline-1-raw.tar.gz.

## Repair and measured validation

AMD patch 0025 / Intel patch 0021 drain the post-unmap deferred references
before retrying the exact reference freeze. If a destructive TAKE still fails,
including failed monitor copyout, it returns ENOTRECOVERABLE and terminates
the source owner. It must not turn the destroyed mapping into retryable absence.
TAKEOBJ already retains its object folio when copyout fails. Neither helper
holds a folio lock across the drain; the exact-count and DMA-pin checks remain.

AMD #201 passes ten repetitions of byte-io, with zero lost-page deltas, and
the original pipe-fed GPG encryption/decryption roundtrip. All six native
CPU/runtime/restore controls pass. The new native take-copyout test verifies
both successful locked-page bytes and termination on bad destructive copyout;
that negative test intentionally increments vmctx_take_lost once. Subsequent
tests compare counter deltas, not an assumed zero boot total.

The full authored suite is 67/69 PASS on #201, with zero lost pages and final
mm/module counts 0/0. Shared-grow and ptrace-step remain failures. Raw archives
are runtime201-pages-1-raw.tar.gz, runtime-gpg-2-raw.tar.gz,
runtime201-native-1-raw.tar.gz and runtime-suite-2-raw.tar.gz under
logs/audit-20260907; runtime201-take-copyout-1.log records the native negative
control. Distributed locked residency and nondestructive rollback remain
separate work; these results establish byte preservation or explicit failure.
