# Native MM coverage audit, September 9, 2026

Status: **incomplete; native ownership invariants fail**. A passed host suite
does not establish coverage of Linux memory mutation. The private-read-only
TAKE candidate in `build/private217-work/core` built and booted as #217; it
passes the four private-custody probes and remains insufficient by itself.
Frozen inputs and exact diff are B/private217-inputs
and B/private217.diff. Its native COW authorization for an existing zero page
is scoped to the active reservation, address and synchronous calling task.

## Exact source and measured failures

Evidence directory (B):
`/mnt/d/VM/lan-boot-recovery-20260908/logs/audit-20260907`.

The #216 baseline was `7.0.14-vmctx-audit-recall1-episode216`, boot
`c6d9fc7e-d3b7-49a8-a3e4-280c3d286fd8`. AMD now runs #217,
`7.0.14-vmctx-audit-recall1-private217`, boot
`ad027942-547c-470f-8b45-9b61b58ff9ca`.
The #216 remote build tree was copied to **B/mm-review29**: 299 files,
SHA256 manifest B/mm-review29-manifest.json. It includes all mm/ and x86/mm
C/header files, all detected vmctx hook files, configuration, and selected
callers. All 20 frozen episode216 core inputs match the remote tree exactly.
This is a source inventory, **not a claim to have read all 299 files**.
References below are to that frozen tree, not the older repository src tree.
An additional 24 headers, callers and native MM documentation files are frozen
in B/mm-review29-supplement-complete.tar.gz with their own manifest. All module
inputs in the current workspace also match the #216 module input manifest.

Measured, with unchanged boot and no new deadlines, lost TAKEs, leaked MMs,
module references, context handles, or transfer tickets:

| Probe | Actual #216 result | Evidence |
| --- | --- | --- |
| Private read-only file / anonymous / zero / executable page transfer | All four return bytes and a receipt but leave record NONE and source PTE present | B/mm-review29-amd-native-2/rows.json |
| UFFDIO_COPY after native TAKE and ACK | Installs 4096 bytes; record remains REMOTE; source PTE becomes present | Same run, uffd-copy.log |
| UFFDIO_ZEROPAGE after native TAKE and ACK | Installs 4096 zeros; record remains REMOTE; source PTE becomes present | Same run, uffd-zero.log |
| MADV_COLLAPSE over a 2 MiB span with one transferred 4 KiB page | Returns success; transferred page's record remains REMOTE but source PTE becomes present | B/mm-review29-amd-native-3/collapse.log |
| MREMAP_FIXED of a transferred private page | Returns success; both old and destination records are NONE, losing remote provenance | B/mm-review29-amd-native-4/remap.log |
| Guest private read-only mapping, upgrade to RW, executor writes, source pipe write/read | Returns initial 0x13 rather than executor's 0xb6 | B/mm-readonly28-probe-1/readonly-upgrade/home.log |

These are failing correctness probes. Their diagnostic drivers explicitly
record `pass_=false` and separately record clean completion. The first
readonly native fixture failed before its measurement because fork may omit
private file PTEs without anon_vma. That failure is preserved in
B/mm-review29-amd-native-1. The corrected fixture prefaults the child before
adoption; it never writes/COWs the read-only file during setup.

## Private-custody repair validated on #217

Patch 0040 passes 51 distinct native controls (B/private217-native-validation.json).
All four RO classes now enter TRANSIT with absent source PTEs before ACK;
mprotect preserves REMOTE/absence, and file/fork aliases remain unchanged.
The zero case also checks that a guarded monitor POKE cannot use TAKE's COW
authorization. The updated file race completes 1,000 transfers under competing
file mappings. The old fixture's EBUSY assertion for private RO TAKE is preserved
as a failed run; its replacement checks the new custody semantics and unchanged
shared-TAKE refusal. Patch replay is byte-exact; build/package cover 6,891 modules.

With the unchanged diagnostic28 userspace, the original readonly-upgrade guest
passes (B/mm-readonly30-probe-1), with unchanged counters and complete cleanup.
A stronger guest which first reads the RO page from a source syscall then
upgrades/writes on the executor **fails**: initial 0x13 is read instead of 0xb6
(B/mm-readonly30-return-1). Its source recall uses GETS, leaving an executor
copy while LAND makes the source HOME. Current userspace changes those source
faults to destructive GET. Both byte oracles pass on mm-return31 in
B/mm-return31-probe-1, as do its Werror build, 84 host and 37 table modes.
The pf4/pf1/pf2/pf3 guests pass, but ns1 stalls on a private RO file page:
native MAPOBJ always requests write access and refuses that RO VMA while the
executor has the bytes and keeps retrying its absent PTE. The #218 candidate
adds MAPOBJ_READ, which installs the existing folio without granting a write
or spending the COW obligation. It and userspace mm-readmap32 are building;
runtime validation is pending. No UFFD, collapse, remap or generic secondary-TLB
fix is claimed.

## Architectural finding

The native transfer removes a source PTE and leaves ownership only in a
separate per-MM/address xarray. To ordinary Linux MM code the resulting
`pte_none` means an empty slot. The two leaf fault hooks do not change that
meaning for UFFD insertion, THP collapse, mapping moves, or other direct page
table operations. UFFD and collapse reproduce the contradiction without any
network traffic or executing guest. A checksum cannot arbitrate this state.

Read-only copying is a second problem: the kernel does not transfer custody,
but the executor later applies source VMA write permissions to its copy.
Current VMA permission is not permanent immutability or page ownership.

A fix must represent remote residency in a form every relevant MM operation
preserves or explicitly resolves; changing only do_anonymous_page/do_fault,
or only the read-only branch, cannot establish that contract. A non-present
PTE representation is being evaluated. It needs defined copy/move/discard,
GUP, permission, fault, and teardown behavior before deployment. Reusing an
existing marker without those semantics would simply move the missing cases.

## Coverage matrix so far

“Hook exists” below means only that the cited hook was read. It does not mean
the path is correct. “No vmctx hook” is based on the full frozen source scan;
Linux's existing locking/notifiers still need an explicit ownership proof.

| Path | Read native boundary | Ownership coverage / remaining obligation |
| --- | --- | --- |
| Missing anonymous PTE | memory.c:5301 do_anonymous_page -> vmctx_anon_fault | Redirects current context only; foreign-mm branch refuses selected reads/writes. Dispatch eligibility depends on adopted mm. Does not cover all faults. |
| Missing private-file PTE | memory.c:6148 do_fault -> foreign hook / vmctx_anon_fault | Includes read-only private files by default; deliberately excludes shared files. Runtime switch can disable RO coverage. |
| Present-page COW / write-protect | memory.c:4180 do_wp_page -> kernel/vmctx.c:1318 vmctx_wp_fault | Mostly counters. Ordinary nonzero page returns FALLBACK without ownership arbitration. Runs under PTE spinlock: cannot wait on monitor there. |
| General fault entry | memory.c:6889 handle_mm_fault -> vmctx_fault_seen | Census only; no admission. Counter observes faults but cannot prevent unhooked fills. |
| Swap-in, NUMA and existing huge-PTE faults | memory.c:4783 do_swap_page; 6600 handle_pte_fault; 6720 __handle_mm_fault | Bypass leaf anon/private-file redirection. Single-page install restrictions exist, but existing huge PMDs and hugetlb have their own handlers. Ownership proof outstanding. |
| New THP fault / batched folio install | memory.c:6669 and vmctx_install_single | Suppresses several fault-time shortcuts. Does not suppress khugepaged or MADV_COLLAPSE. |
| THP collapse / khugepaged | khugepaged.c:531 isolate, 773 copy, 1078 collapse, madvise_collapse | **Measured violation.** pte_none is zero-filled, then a huge PMD becomes present. No vmctx guard. |
| UFFD COPY / ZERO / CONTINUE / POISON | userfaultfd.c:168 install; 349 zeropage; 704 mfill_atomic | **COPY/ZERO measured violations.** Checks native PTE occupancy, not remote ownership. Zero-page prohibition only changes which zero folio is allocated. |
| UFFD write-protect / resolve | userfaultfd.c:907 uffd_wp_range -> change_protection | Can change PTE access directly; no vmctx access event or page ownership gate. Marker interaction must be preserved. |
| UFFD MOVE | userfaultfd.c:1247 move_pages_ptes; 1766 move_pages | MMU_NOTIFY_CLEAR; destination insertion and source-hole interpretation bypass vmctx log. Must move logical residency as well as present PTEs. |
| Slow GUP / FOLL_FORCE | gup.c:802 follow_page_pte; 1200 check_vma_flags; 1354 __get_user_pages | Present pages can be pinned without faulting. FOLL_FORCE can write private read-only mappings. Exact-reference TAKE freeze protects against already held pins, but copied/read-loaned/shared pages need their own revocation proof. |
| Fast GUP / long-term pins | gup.c:3129 gup_fast; 3175 fallback | Walks present PTEs with interrupts disabled, no vmctx hook. Cannot add a blocking owner callback here. Must rely on page-table representation plus pin lifetime exclusion. |
| process_vm, ptrace, proc memory, AIO/io_uring/DMA | process_vm_access.c:73; GUP callers inventoried | Remote GUP can bypass fault policy on present PTEs. Full caller and retained-pin audit outstanding; unavailable bytes must not become invented bytes. |
| mprotect / pkey_mprotect | mprotect.c:696 fixup, 776 access_commit | Commit after PTE update, before outer tlb_finish_mmu. Journal reports native VMA rights; it does not acquire page custody. Executor access_apply_span can restore writes over a loan/COW restriction. **Guest stale-read measured.** |
| mmap / MAP_FIXED | vma.c:2745 __mmap_region; 2794 CONSTRUCT | Construction recorded; replacement UNMAP notifier cancels records. Failure/partial construction and old-ticket cancellation need end-to-end testing. |
| brk growth / stack expansion | vma.c:2892 do_brk_flags; 3116/3202 expand | No construction commit hooks in these paths. Mapping lookup may discover new space later, but no complete incarnation/permission proof. |
| mremap, including DONTUNMAP | mremap.c:795 move_page_tables -> UNMAP notifier | **Lost provenance measured with MREMAP_FIXED.** Source records erased as if destroyed; no native destination ownership transfer. Userspace successful-syscall map effect carries MOVE, which cannot describe all partial/concurrent outcomes. |
| munmap / teardown | vmctx.c:5680/5688 range_start/end; pgrec_erase_range:949 | UNMAP cancels retained guards and erases records; journal emits removal. Guard cancellation prevents late landing. Invalidation completion and all aliases still need proof. |
| MADV_DONTNEED[_LOCKED] | madvise.c:954-972 | Explicit discard before zap; successful commit after. Has ownership cancellation; transfer/mutation races still need validation. |
| MADV_FREE / pageout / reclaim | madvise.c:652-836; rmap.c CLEAR invalidations | No discard journal for FREE (not the same semantics as DONTNEED). Need prove lazy discard and future writes remain coherent; do not blindly convert CLEAR into discard. |
| MADV_REMOVE / hole-punch / truncate | madvise.c:1021 -> vfs_fallocate; truncate.c; shmem.c | Changes the shared object across mappings, not just one VMA. No object-level distributed ownership barrier demonstrated. More caller reading needed. |
| Guard install / remove | madvise.c:1136 / 1257 | Install only traces (including zero/zap path); remove emits ALLOW per marker. No DENY commit at install in this tree. PTE permission revocation must precede executor access. |
| Fork / CLONE_VM / exec | fork.c hooks; vmctx.c:3403,3497,3556; exec.c:1774 | Context identity/cleanup hooks present. New private mm gets a new empty page-record tree, no native copy of remote residency. Stale source PTE inheritance is explicitly left in place. CLONE_VM outside THREADGROUP also escapes holds_one's thread-group scan. |
| KSM / migration / compaction | ksm.c:1279,1385; rmap.c:1986,2417; migrate.c:332,436 | Linux MMU_NOTIFY_CLEAR and PTE replacements; no direct vmctx hooks. Existing exact-folio/pin checks may refuse TAKE but are not a secondary-TLB completion proof. |
| PFN/device/DAX/hugetlb insertion | memory.c:2358 insert_page_into_pte_locked, 3172 remap_pfn_range; hugetlb.c | Separate insertion/fault paths. Several vmctx control operations refuse special mappings, but initial/source mapping admission is not universally constrained. Not established as covered. |
| Guest TLB invalidation | x86/mm/tlb.c:1483; vmctx.c:5590; module vmctx_kick_guests | Manual TAKE/protect kicks exist. Generic secondary-TLB subscription defaults off and skips atomic contexts when enabled. Runtime enable/calls/unsafe all read 0 after the probes. Entry-time flushing cannot replace completion for a guest currently executing on a stale translation. Need atomic-safe invalidation contract. |

Enabled in the frozen configuration: USERFAULTFD, THP (madvise default),
HUGETLBFS, KSM, migration, NUMA balancing, swap, device-private memory, DAX,
soft-dirty tracking, pkeys, AIO and io_uring. These paths cannot be dismissed
as compiled out. Runtime state and individual mapping admission still matter.

## Lock and lifetime obligations for the replacement

1. Reserve custody before publishing bytes or granting CPU/GUP access. A
   native record plus a separate sampled PTE is not an atomic proof.
2. Do not wait on a monitor while holding a PTE/folio spinlock, an MM write
   lock needed by landing, or a channel/layout lock needed by the callback.
3. Retain a distinct logical page identity through move/fork and transfer;
   virtual-address reuse must cancel the old transaction without accepting
   its late reply into the new mapping.
4. Combine desired VMA permissions with owner/read-loan/COW restrictions.
   An access journal is not permission to bypass those restrictions.
5. Complete all CPU and device translation/pin revocation before copying or
   freeing a page. An optional notifier or “flush on next entry” is insufficient.
6. Make unsupported mutation fail before changing native MM state. Low-level
   void PTE setters cannot safely become ad hoc refusal points after the
   caller has already changed rmap/RSS/reference accounting.
7. Keep checksums diagnostic. No byte/generation repair or backing-presence
   fallback may supply the missing custody proof.

Further work: finish the remaining callers, choose the representation and
lock protocol, implement in an isolated candidate, then run these native
invariants plus existing custody/recall/race tests before any full guest suite.
