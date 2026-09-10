# Native remote residency: design constraints

Design under review, **not implemented or validated**. The measured failures
and source references are in [mm-path-coverage-20260909.md](mm-path-coverage-20260909.md).
This document rejects fixes which make the new tests pass while retaining the
same ambiguous representation elsewhere.

## Required representation

An ordinary absent PTE must mean ordinary native absence. A page whose current
contents live remotely must have an explicit software leaf entry. The proposed
direction is a dedicated native remote-residency entry carrying an opaque,
retained logical page identifier, analogous in role to a special swap entry.
It must not masquerade as a swap-cache PFN, zero page, UFFD marker, or guard.

The logical page descriptor owns the residency/claim state and survives changes
to its virtual address. A VMA/address indexes a mapping of that descriptor; it
is not the descriptor's identity. A transfer ticket is a temporary capability
for one ownership transition, not the persistent identity of the page.

Do not add a marker flag while retaining `(mm, address)` as the only owner key.
That would prevent UFFD/COLLAPSE fills, but mremap would still lose the owner,
fork could lose its birth contents, and address reuse could accept a stale
reply. The descriptor's lifetime must be explicit before encoding it in a PTE.

## Operations and linearization points

| Operation | Required action before publishing success |
| --- | --- |
| Export local private page | Reserve descriptor; detach private COW/file aliases as needed; replace source PTE with remote entry; finish TLB/device revocation; freeze exact allowed references; capture bytes; retain transition until installation ACK. |
| Export untouched anonymous page | Prove native initialization from the mapping incarnation; install remote entry before returning an initialization grant. Absence alone is not this proof. |
| Acquire remote page | Claim descriptor; fetch from the recorded owner; construct the complete local folio; atomically replace the exact remote entry with the filled folio under PTE lock; settle the exact transition. |
| Read-only transfer | Preserve VMA permissions and still transfer custody, or implement a separately specified revocable read grant. Current RO permissions cannot imply a permanent immutable copy. |
| Permission change | Change desired VMA permissions; actual access remains bounded by ownership, read-grant and COW restrictions. Preserve remote entries; do not treat mprotect success as ownership. |
| mremap | Move the entry and descriptor reference together. The page identity is unchanged by a new virtual address. Rollback restores both. DONTUNMAP must follow its native old-range semantics explicitly. |
| Fork | Copy remote residency references with the native page-table operation, recording the child's COW relationship at the fork boundary. Copy-on-write at either execution site must split the logical page before granting writes. Failed fork must release these references. |
| Unmap/discard/replacement | Remove the old entry/reference and invalidate pending capabilities for that mapping incarnation. A reply cannot attach to a new mapping at the same VA. Discard differs from temporary migration/COW invalidation. |
| UFFD insertion | Remote entries occupy the slot, so COPY/ZERO/CONTINUE cannot replace them as holes. UFFD WP metadata must compose without erasing remote residency. MOVE must preserve identity or refuse before mutation. |
| THP collapse | A remote entry cannot be zero-filled as pte_none. Collapse must first resolve all necessary residency or refuse without changing the logical mapping. This applies to khugepaged and explicit collapse, not only faults. |
| GUP/pins | Present native folios follow native pin rules. Remote entries cause the residency fault path; existing pins block export until actual pin release. No timeout or task scheduling observation substitutes for that lifetime. |
| Teardown | Cancel outstanding requests, retire CPU/device access, and release entry/descriptor references even when the context/task setup never completed. Unconfirmed delivery terminates the owned session; it never invents a new owner. |

## Fault service and locks

Fault ownership must be attached to the MM/page request rather than only
`current->vmctx`. GUP, ptrace/proc access and kernel workers can fault an adopted
MM while a different task is current. The existing per-context event slot is
not automatically a safe queue for all such callers.

Fault redirection must occur before acquiring the PTE lock, or must release
that lock before waiting. On paths allowing VM_FAULT_RETRY, save immutable
request identifiers, release the VMA/mmap lock, obtain the page, and retry.
No VMA pointer may survive the drop without a distinct lifetime guarantee.

The no-ALLOW_RETRY GUP paths require a separate proof. Simply waiting while
holding mmap_read_lock recreates the measured fair-rwsem cycle when a writer
queues before the monitor's landing read lock. Simply dropping the lock
violates the caller's contract. An explicit request with a retained mapping
and a completion operation able to install under that request's protection is
one candidate; it must also prohibit nested fetch dependencies on the blocked
MM writer. This is an unresolved implementation obligation, not permission to
retain the old blocking branch.

Landing cannot clear a remote marker and then fault/GUP an empty slot. That
would reopen the UFFD/collapse hole between clearing and installation. Build
the complete private folio first, then replace the exact entry atomically.
Normal RSS/rmap/LRU accounting and file/private-COW semantics must be retained.

Low-level `set_pte_at` is not a safe generic refusal API: its callers may have
already changed rmap, RSS and references and its return type has no error.
Instrumenting violations there can help audit coverage, but cannot implement
ownership acquisition or rollback.

## Secondary TLBs

All relevant invalidations must reach executing guests, including COW,
mprotect, fork write-protection, migration and reclaim. Current generic
notifier arming is optional and the callback skips atomic contexts. The native
notifier contract explicitly permits calls under the PTE spinlock and forbids
sleeping. Setting the existing switch to 1 does not satisfy this contract.

The replacement needs an atomic-safe entry/exit protocol: publish an
invalidation before observing guest execution; force old translations out;
wait for an acknowledgement that requires no MM/PTE lock; and ensure a racing
entry either observes the invalidation or is forced out. A flush on the next
entry does not cover a guest already executing. Concurrent invalidators and
CPU hotplug must not deadlock or make an acknowledgement refer to a later CPU
incarnation. This must be tested in both SVM and VMX.

## Shared objects require additional ownership scope

A private-page PTE design does not govern source page-cache writes or aliases
in an unadopted MM. File reads/writes, hole-punch, truncate, shmem/SysV aliases,
and DMA can access the same object without faulting the adopted PTE. The
owner's identity and revocation must therefore include the shared object and
offset where applicable. Reclaiming around the monitor's forwarded syscalls
does not cover unrelated native processes or asynchronous writes.

Retaining native source file semantics is necessary. Replacing a shared file
folio with a private anonymous folio would break its aliases and file I/O.
This is a separate required protocol, not a reason to call shared copies
exclusive transfers. Unsupported mappings must be handled explicitly before
publishing a grant, without silently returning stale bytes.

## Validation before deployment

Use the failing native custody probes as regression oracles. Add successful
fault/landing and cancellation races against UFFD, mprotect, discard, move,
fork, pins and teardown; verify both descriptor state and native accessibility.
Include failed/partial mremap, fork allocation failure, address reuse, and
late reply delivery. Verify that source/private file aliases remain unchanged
where native COW requires it and shared object aliases observe the current
owner's bytes. Count no unconfirmed grants or abandoned live descriptors.

Only then run the existing native custody controls, targeted guest byte
oracles, group exit, the full suite, cross-host runs and real programs/GUI.
Keep all failures visible. Hashes and generation counters remain diagnostics,
never fallback authority.
