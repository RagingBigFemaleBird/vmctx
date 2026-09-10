> Updated continuation: [runtime/native validation](continuation-20260909-green.md).
> #220 and the corrected HMM test driver pass the native campaigns.
> A stricter diagnostic check regrades lifetime37 and retire38 to 72/78;
> source LAND errors were missed in six cases each. A deterministic native
> capture/claim control reproduces the exclusion defect on #220. Patch0045,
> candidate #221, builds/packages without warnings; its one-time boot is
> starting. The historical results below are preserved.

# Page lifetime audit and replacement proposal

This continuation starts from the failed custody33 guest and the already
running protect219 kernel build. **Not all green.** Native #219 passes all 56
vmctx controls; quiet36 passes the 12 focused guests with zero live references,
transfer abandonment, deadline hits or lost pages on its fresh boot. The native
UFFD/collapse/mremap failures in the existing vmctx representation remain open.
Compilation, an inventory match, and a native substrate experiment are each
different from a validated vmctx ownership protocol.

Evidence base **B** is
`/mnt/d/VM/lan-boot-recovery-20260908/logs/audit-20260907`.

## Finding: there is no single page-exit callback

A page can stop being mapped without being freed, move to another address
without losing its identity, acquire another fork reference, migrate to another
folio, or remain pinned after every mapping disappears. A destructor covers
the final reference; it cannot implement permission revocation, COW, or mapping
retirement. A fault hook covers accesses; it cannot account for direct page
table insertion, mapping moves, or shared page-cache writes.

The present design exports a page by removing its PTE, while keeping custody
in a separate `(mm, virtual address)` record. Native MM therefore sees a hole
where vmctx sees a remote page. Adding callbacks for individual syscalls
cannot make those two representations agree. The measured probes already
demonstrate three independent consequences:

- UFFD COPY and ZERO accept the slot and create another source page.
- Collapse treats the slot as zero-fillable and publishes a huge mapping.
- MREMAP_FIXED moves native memory while the UNMAP observer erases custody.

The same representation loses native fork ancestry and permits late responses
to be confused with an address reused for another mapping. Existing retained
context handles solve task identity; they do not solve logical page identity.

## Recommended foundation: native folio residency and mapping observers

Investigate **HMM device-private memory**, with a `dev_pagemap` owned by the
source memory service, before designing another special PTE type. It already
represents CPU-inaccessible storage with a real `struct page`. The remote
executor would be the inaccessible storage/compute endpoint; it need not have
a Linux filesystem or interpret source files.

Use three distinct mechanisms:

1. Native folio references/rmap own page lifetime. `dev_pagemap_ops.folio_free`
   is the final-reference callback for a device-private folio. A driver-owned
   descriptor supplies an opaque logical identity and transfer state; the wire
   must not expose a PFN or rely on virtual address as identity.
2. Native migration moves residency. `migrate_vma_setup/pages/finalize` and
   `migrate_to_ram` coordinate inaccessible entries, pin exclusion, accounting,
   copying and rollback. The driver checks each page's actual migration result;
   a successful setup call alone does not grant ownership.
3. MMU interval notifications govern mapping permissions and access revocation.
   A mapping disappearing is different from the last reference disappearing.
   Native copy/move operations carry page identity; the observer invalidates
   executor translations and mapping capabilities.

This direction is supported by the current kernel source, rather than just a
similar API name. Linux's [HMM design](https://kernel.org/doc/html/latest/mm/hmm.html)
describes its move away from a separate device page structure toward native
page lifetimes. The frozen native implementation is the authority for the
specific behavior below. **The network service, revocation and shared-object
parts are still implementation obligations.**

## Reviewed boundaries

References use B/mm-review29, overlaid by B/mm-exit34-supplement and
`build/protect219-work/core`. The overlay contains the current native changes;
the repository's old `src/linux-7.0.14/kernel/vmctx.c` is not this baseline.

| Native operation | Boundary examined | Replacement contract and limits |
| --- | --- | --- |
| Present private export | `mm/migrate_device.c:migrate_vma_setup`, `migrate_device_unmap`, `migrate_device_pages` | Migration isolates, unmaps and checks references before copy. Only pages still marked MIGRATE after the commit may be published remotely. Pins retain native access and must prevent export. |
| Anonymous first touch / zero page | `migrate_vma_collect`, `migrate_vma_insert_page` | Native empty-slot insertion races under the PTE lock. Only a confirmed new native anonymous slot authorizes initialization. Existing vmctx holes cannot be fed into this path as fresh memory. |
| CPU or foreign-MM access | `mm/memory.c:do_swap_page` | Device-private entry resolves through its page's `pgmap->ops->migrate_to_ram`, without depending on `current->vmctx`. The code takes a reference under PTL and calls outside PTL, with the folio locked and mmap lock held. |
| GUP, process_vm, ptrace/proc, kernel buffers | `mm/gup.c:follow_page_pte`, `faultin_page`, `__get_user_pages`, `gup_fast` | Nonpresent entries cannot yield a direct CPU pin; slow GUP faults them back. Existing pins exclude migration. A userspace receiver must fill a retained kernel transfer buffer, not reacquire the faulting MM's mmap lock. |
| Fork | `mm/memory.c:copy_nonpresent_pte` | Device-private entries get a folio reference, RSS/rmap duplication and COW read restriction. The parent entry is changed too. Remote write access must be revoked before that transition is exposed. CLONE_VM uses the same MM rather than a new page owner. |
| COW/write-protect | `do_wp_page`, `copy_nonpresent_pte`, `mprotect.c:change_pte_range` | Private read-only residency is not permanent immutability. A write either preserves native exclusivity or creates a separate logical page after a stable snapshot. Desired VMA rights alone never authorize executor writes. |
| Protection/pkeys | `mprotect_fixup`, `change_pte_range`, `huge_memory.c:change_huge_pmd/pud` | Device-private write entries are downgraded to readable entries. UFFD-WP remains separate metadata. The native-tested #219 fix makes executor PROTECT_MM preserve pre-existing PTE/PMD/PUD write restrictions. |
| mremap / stack relocation | `mm/mremap.c:move_ptes`, `move_normal_pmd/pud`, `move_page_tables` | Native code moves the software leaf entry, including its page reference. Virtual address changes do not change logical page identity. Whole page-table moves and partial rollback must be exercised, not only one PTE. |
| DONTUNMAP / address reuse | `move_page_tables`, VMA construction and zap paths | The destination inherits the moved entry; the old range follows native DONTUNMAP semantics. New mappings get new capabilities. Late delivery names the old identity and cannot populate the new slot. |
| UFFD COPY/ZERO/CONTINUE/POISON | `mm/userfaultfd.c:mfill_atomic_install_pte`, `mfill_atomic_pte_zeropage`, marker insertion | A device-private leaf occupies the slot. COPY accepts only none/appropriate UFFD marker, ZERO requires none. A remote entry is neither. All insertion variants and partial ranges remain test requirements. |
| UFFD MOVE | `move_pages_ptes` | The current native implementation refuses non-swap software entries with EFAULT before moving that leaf. HMM does not make MOVE universally supported. A successful prefix must remain native and the refused page must remain owned. |
| UFFD write protection | `uffd_wp_range`, `change_protection` | Preserve residency while changing write policy. A write-protection clear cannot bypass a remote/COW restriction. |
| THP collapse/khugepaged | `khugepaged_scan_pmd`, `__collapse_huge_page_swapin`, `__collapse_huge_page_isolate` | A device-private entry is not none/zero. The pre-collapse nonpresent path calls `do_swap_page`, so it must obtain actual bytes or fail collapse. Scan acceptance alone is not a copy or ownership grant. |
| THP split / PMD migration | `huge_memory.c`, `migrate_device.c`, `dev_pagemap_ops.folio_split` | Native large folios have extra split/reference obligations. First prototype should use order-zero remote folios; larger export must not be enabled until split, partial unmap and fork accounting are validated. |
| Reclaim, NUMA, compaction, KSM | `vmscan.c`, `migrate.c`, `rmap.c`, `ksm.c` | An inaccessible device-private page is not ordinary LRU data. Native migration/rmap operates on its descriptor, while CPU access migrates back. Once back in RAM, ordinary native reclaim/COW applies. No checksum selects a replacement copy. |
| munmap/MAP_FIXED/discard/guard/exec | `zap_nonpresent_ptes`, VMA teardown, madvise zap callers | Removal drops native rmap/RSS/reference state. Guard/discard/remap semantics stay distinct. A retained transfer may outlive the mapping, but cannot make it accessible again. |
| Failed fork / process exit / MM release | `fork.c` unwind, `exit_mmap`, `free_zone_device_folio` | Unwind drops copied references through native teardown. Final folio retirement calls `folio_free`; MM notification retires mapping capabilities. The descriptor/request holds the session until its own cleanup is complete. |
| Swap / swapoff / zswap | `do_swap_page`, `softleaf_is_swap`, `swapfile.c` | A device-private leaf is not a real swap slot. Do not route it through swap I/O or reuse a swap type without registering proper ownership. CPU-resident pages may later swap through normal MM. |
| Page-cache / shmem / SysV aliases | `filemap.c`, `shmem.c`, `truncate.c`, `page-writeback.c` | **Not solved by private HMM.** Shared bytes are reachable through other MMs and buffered/direct I/O. Source object identity and offset need a separate residency/revocation protocol. Private COW must preserve original file bytes. |
| DAX/PFNMAP/MIXEDMAP/hugetlb/device insertion | `remap_pfn_range`, `vm_insert_*`, `hugetlb.c`, DAX callers | HMM migration explicitly rejects several special VMAs, and device-private migration does not cover arbitrary file folios. Admission must make this explicit before publishing a grant; a fallback copy is not ownership. |
| AIO/io_uring/DMA/RDMA | GUP/pin callers, retained buffer registrations | Pins are independent of task and mapping lifetime. A stopped task or finished syscall does not prove DMA completion. Export waits for native pin release, or is refused without altering custody. |
| Executor secondary TLB | `vmctx_mn_arch_invalidate`, `vmctx_kick_guests`, SVM/VMX entry | **Still open.** Generic subscription defaults off and skips atomic calls. Local guest entry/exit needs a non-sleeping acknowledged revocation protocol. Merely enabling the current callback cannot establish safety. |

## Locks, transactions, and failure semantics

The native fault callback can hold mmap_read_lock while waiting. A queued MM
writer makes a second monitor-side mmap_read_lock wait behind it, recreating
the previously observed cycle. The transport service must instead read/write
a kernel-owned, pinned request buffer by **request identity**, using no VMA
lookup, GUP, source folio lock or source mmap lock. The faulting kernel caller
performs the final native migration. Transport processing must also work when
the faulting task has no vmctx context, as with khugepaged or foreign GUP.

Reserve the descriptor and session before export. The transfer states are
LOCAL -> EXPORTING -> REMOTE -> RECALLING -> LOCAL, with RETIRING/FAILED terminal
paths. Export failure before publication restores the exact original native
page. After publication, lost ACK cannot mean LOCAL: retain inaccessible
custody until exact completion or session termination. Publication, permission
grant, and ACK each carry the logical identity and transaction identity.

Migration completion must not depend on the monitor's logging, a guest-held
file position lock, or the same session callback whose completion is awaited.
The diagnostic28 nonblocking sink addresses the measured logging dependency.

HMM is not a network fence. An interval callback must finish invalidation before
returning success. For nonblockable callbacks, only the native documented retry
contract permits refusal; do not sleep or silently queue work and return success.
The [native notifier rules](https://cdn.kernel.org/doc/html/latest/mm/mmu_notifier.html)
also require ordering for COW/PFN replacement. This needs a separate proof from
the final-reference cleanup path. The release callback may enqueue transport
retirement, but must retain all state needed until remote access has stopped.

## Runtime defect isolated during this audit

custody33 group-exit captured page `0x41f000`, ticket6031, but never ACKed it.
Earlier a sibling of the same MM had mapped the locally owned folio with a
write watch. The later read bypassed the local-object path because `page_keep`
includes watches, fetched from the source, then skipped its own landing for
the same watch. It stranded custody in TRANSIT.

watch34 now uses MAPOBJ_READ for a watched local folio before entering source
recall. It retains the watch and COW obligation and does not copy the folio.
Three production-path controls exercise read, region-read and executable-read.
The frozen custody33 baseline requests the page from the source and exits97
with complete owned cleanup; the corrected control maps locally, preserves
restrictions and emits no source request or ACK. Source id `fd07484299ab`
passes the Werror build and complete host/table checks. Its three read-only upgrade/recall/fork guests pass on #219. Group-exit
produces correct output and clean custody counters, but watch34 fails the
independent diagnostic-loss check. quiet36 includes the scoped receipt guard
and reports session aggregates when the last registered fault worker finishes,
retaining each worker's own result. All 12 focused guests then pass, including
group-exit, pf1–pf5, ns1 and exec chains. Frozen quiet36 source id is
`f54947b786f7`; it passes Werror builds and full host/table checks.

scope35 adds a common receipt destructor to both production acquisition scopes
(the fault handler and page settlement). Each return/goto out confirms an
explicitly queued landing ACK; an unconsumed source receipt stops the session
before releasing its local reservation. A second fetch cannot overwrite an
unsettled receipt. This guard never infers landing from received bytes. Two
production-path controls force both native and object landing to fail; the
watch34 baseline continues without an ACK and fails the control, whereas
scope35 exits97 before continuation. Both modes pass the full host checks.
This establishes a fail-closed exit invariant, not a complete kernel residency
protocol or proof that every branch chooses the correct logical page.

## Reproducible audit and validation

`tests/kernel-page-paths.py` inventories lexical boundaries, excluding comments
and strings, with file digests and exact source locations. The current report
is B/mm-exit34-inventory.json: 320 files, 2,982 matches across publication,
revocation, residency, identity, references, pins, objects and secondary MMU.
It is **not** a call graph, a claim to have read every line, or a coverage pass.
New kernel revisions require a refreshed inventory and semantic review.

`tests/control/hmm-custody.c` is a native substrate probe using the upstream
test_hmm driver. It checks actual bytes through recalls, move/DONTUNMAP, fork,
UFFD COPY/ZERO refusal, collapse, discard/replacement, protection and exit.
It does not exercise vmctx transport. Results must stay separate from the
native vmctx custody tests and from guest results.

After the candidate native controls: rerun readonly-return/upgrade/fork,
group-exit, the remaining focused guests, full suite, cross-host, breadth
programs and GUI. Preserve all earlier failed probes. No generation repair,
checksum match, relaxed ancestor search or elapsed wait can turn an unresolved
ownership transition into a pass.

## HMM substrate finding: owned read-only leaves bypass write faults

The unchanged native test_hmm driver builds without warnings. Its recall
probe passes through a kernel pipe write using the simulated device's modified
bytes. The next mode, fork, triggers `test_hmm.c:296`: hmm_range_fault returns
success with a valid, read-only PFN although the writer requested WRITE. With
panic_on_warn enabled, this produces a kernel panic and the preserved default
#181 boots. The #219 image and all packaged modules were verified, then #219
was restored through its existing one-time entry. Current recovery boot is
`8b01c7e8-e193-4e70-8d51-359a439ccacc`.

The exact source shows the cause in `mm/hmm.c:hmm_vma_handle_pte`: the owned
device-private shortcut returns its PFN without `hmm_pte_need_fault`. Fork
made that leaf read-only. The PMD shortcut has the same missing access check.
Patch0043 applies the existing requested-access policy before either shortcut
publishes a PFN. Native COW/mprotect resolves the write; residency ownership
alone is never a write grant. **This patch is prepared, not yet native-tested.**
The test driver and fork byte oracle are unchanged.

Evidence: B/hmm34-native-launch.log contains the successful recall result;
B/hmm34-crash-pstore/1788996820/001/dmesg.txt records the fork warning and
panic. The remote campaign directory was not durable before the immediate
panic; its recovery tar is partial and contains pstore only. No missing rows
are counted as passes. Future native probes must sync staged evidence before
execution and stream kernel messages off-host while running.

B/mm-quiet36-suite-1 is a harness failure before any guest ran: port33000
exceeded the suite's reserved port bound. The corrected attempt uses port32400
and separate B/mm-quiet36-suite-2 evidence. Full-suite results remain pending.
