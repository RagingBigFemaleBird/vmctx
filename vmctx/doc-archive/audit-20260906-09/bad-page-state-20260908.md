# Private file TAKE corrupts a racing mapper's reference count

Status: repair staged; live validation pending. This is a kernel ownership bug,
not a processor capability mismatch. Firefox remains gated on the tests.

## Evidence

AMD kernel `7.0.14-vmctx-audit-recall1-runtime199 #201` reported
`BUG: Bad page state in process take-file-race` at monotonic 14580.609034.
The folio had refcount 0, mapcount 0, and a non-null shmem mapping for
`memfd:take-file-race`. The failing thread was the independent mapper,
PID 97280. Its stack freed the page through `folios_put_refs`,
`free_pages_and_swap_cache`, `tlb_finish_mmu`, and `munmap`.
It subsequently stalled in `filemap_get_entry` / `shmem_get_folio_gfp`;
the kernel reported an RCU stall and unrelated blocked work. The user rebooted
the machine. Boot `f7cf856a-c490-429e-9a8f-cc77afb6f2e7` is the older default
kernel #181; the regression must not run there.

The complete previous-boot journal is preserved at
`/mnt/d/VM/lan-boot-recovery-20260908/logs/audit-20260907/amd201-crash-journal.log`.
The bad-page report starts at line 267023. Earlier focused breadth testing
also caught a private file TAKE losing its mapping after a late alias appeared:
`refs=3 want=2 mapcount=1 anon=0`. The `basejq` workload failed once in ten runs
with exactly one additional `vmctx_take_lost` event.

## Defect and ownership rule

The old claim first checked that no mappings remained, then froze
`folio_expected_ref_count(folio) + 1`. That helper includes mapcount, which can
change between the check and the freeze. A racing mapper's reference could
therefore authorize a freeze while that mapper still owned the page. Release
then recomputed the count, potentially dropping a reference after the mapper
unmapped. The old path held no folio lock, so swap-cache membership was also
unstable. These interleavings explain the observed premature free; the journal
does not identify the exact instruction ordering of that occurrence.

A private file PTE being its file-cache folio's sole current mapping is not
exclusive ownership of that folio. Another process can fault it immediately.
The native adapter must first resolve private COW and retain the anonymous page
actually installed by that write fault. Shared file mappings use the object
protocol; the private TAKE command refuses them before unmapping anything.

The repaired claim takes the folio lock and accepts only an order-zero,
non-KSM anonymous folio. Its expected count consists of the caller's GUP
reference and the optional swap-cache reference, whose membership is stable
under that lock. Mapping references never enter the expected count. A racing
mapping or GUP holder therefore prevents the atomic freeze. The exact frozen
count is retained and restored before unlocking; it is never recomputed.

The frozen bytes are copied to a preallocated kernel page. The folio is then
unfrozen and unlocked before any user-buffer copy, preventing a user fault from
sleeping while the original page is frozen. Existing fail-closed behavior for
destructive transfer failures and bad copyout remains required.

These are native adapter changes for both kernel versions. No native page flags,
Linux refcounts, syscall numbers, or signal meanings are added to the transport
or interpreted by an executor running another OS.

## Regression and validation gate

`tests/control/take-file-race.c` creates a SERVICE context and transfers 1,000
clean private memfd mappings while two threads repeatedly fault and unmap
independent private and shared aliases. Every byte must remain `0x69`. Every
destination copy faults a fresh anonymous monitor page. Additional cases verify
that read-only private and shared mappings are intact after refusal.

The original version reproduced the host crash on #201. It must next pass on
both corrected kernels with live kernel logging, no bad-page/RCU/lock warnings,
no lost transfer, and zero surviving contexts or module references. Existing
locked-page and intentionally bad-copyout controls must also pass. Kernel build
success alone does not meet this gate.

Patch series: 6.18 patch 0023 and 7.0 patch 0027. Separately, 6.18 patch 0022
and 7.0 patch 0026 stage native #DB delivery, with VMX ICEBP advancement and SVM
DR6 transport in the backend module. These do not implement the still-missing
native syscall tracing entry/exit protocol or hardware debug-register transport.
