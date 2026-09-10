# Stable shared-object identity

Wire v15 (VMRF) carries an opaque object ID separately from its logical page
offset. The source Linux adapter pins the inode and interns its native identity;
no descriptor number, device or inode crosses the wire. Offset zero stays zero.
The executor assigns one stable slot per (object, page offset), under a mutex,
using a dynamically resized hash table. Sparse offsets and growth allocate only
requested pages. One native SHARED_PAGE install occurs per fault after the
existing ownership protocol supplies bytes. Identity does not prove ownership.

The shared-layout metadata preserves identity through splits, protection and
fork inheritance. Replaced mappings are retired before construction publication
under layout_order_lock; shared backing bytes remain available to other aliases.
Native cross-task UNMAP's historical void error return remains an architectural
gap. Object byte lifetime after truncation/last unmap, mremap, concurrent native
kernel writers and device-specific aliasing also need explicit treatment. This
is not evidence of complete io_uring coherence, even if Node workloads pass.

AMD #201 / userspace 67cac70eaa6e: shared-grow passes five repetitions; nine
related alias, snapshot, late-fork, writeback and locked-byte controls pass.
All have zero lost-page delta and complete cleanup with mm/module counts 0/0.
The full 69-case suite is 68/69: only ptrace-step fails. The 72-program breadth
campaign is 69/72: GPG and both Node cases pass; groffman, gdbbt and stracec fail.
Raw archives remain on AMD and under the recovery directory on D:.

After the user's explicit instruction to expand tests for discovered bugs,
pipe-stream and shared-resize were added to the authored suite (now 71 cases).
The earlier 68/69 result does not include them. pipe-stream passes natively and
ten remote repetitions but does not reproduce groff's corruption. shared-resize
passes natively; its remote baseline is pending. It tests that shrink/regrow
retires old bytes while retaining the same object identity, both with surviving
aliases and after all aliases have been unmapped.

The new shared-resize control passes its native baseline and three remote
repetitions on 67cac70eaa6e / AMD #201, with zero lost-page deltas and cleanup
0/0. It covers both surviving aliases and unmap-before-resize: data in retained
pages survives while pages removed by truncate read as zero after regrowth.
This observed result bounds the stale-slot concern; it does not establish all
external-writer or object-generation semantics. The full 71-case suite has not
yet been rerun. Raw evidence: shared-resize-baseline-1-raw.tar.gz.
