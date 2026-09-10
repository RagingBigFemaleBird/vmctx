# Userspace review — ongoing

These are source findings and hypotheses, not an attribution of Firefox's
failure. Review coverage is recorded separately in read-ledger.json. Original
source findings below are retained to explain the audit. The dated audit record
tracks the repairs and their exact test/build status: loan storage, transfer
claim growth/exclusion, installed-page storage, source hashing, build identity
and the SVM second-report register reply now have local changes. The wider
ownership and lifecycle findings remain open.

- Destination `lend_forget_range` zeroes an open-addressed slot, while all
  lookups stop at the first empty slot. `control/page-tables.c loan-delete`
  reproduces losing the lookup of another live loan behind the removed one.
  The table's packed key also retains only 16 bits of the address-space pid;
  pids separated by 131072 have identical buckets and keys for the same page.
  `control/page-tables.c loan-identity` reproduces that alias (owners 42/42
  instead of 41/42). Full `(address-space identity, page)` equality and safe
  deletion are needed.
- `page_take_mode(..., away=1, punch=0)` does not pass its `punch` argument to
  TAKEOBJ. The successful TAKEOBJ branch jumps past the userspace punch gate,
  but the kernel already removed the folio whenever `vmctx_takeobj_punch` is
  enabled (it is Y on AMD). Its comments claiming this leaves the object
  populated do not describe the effective operation. The subsequent put-back
  also accepts any positive write length; it does not require a whole page.
- The take-generation table resets counts on unmap/exec without a mapping
  incarnation in replies, never frees slots, and returns generation zero on
  capacity failure. The retained-data path must not treat that as freshness.
- `pull_tab` has no address-space identity, allows duplicate claims and ends
  every claim of an address when any one pull finishes. Overflow falls back
  to holding the recursive coherence mutex across the network request.
- The trail ring reserves a slot by incrementing its published count before
  filling plain fields; concurrent readers can observe partial or overwritten
  events. The coherence watcher uses volatile fields without a synchronized
  snapshot. Neither provides a reliable total order for proving a race.
- `as_apply_map_siblings` only propagates UNMAP and ZAP, ignoring failures.
  Skipping protection reductions leaves already writable/executable sibling
  mappings able to access memory without a fault; later fault repair cannot
  enforce the source's mprotect revocation semantics.
- Page-channel UPGRADE grants success even when the requester is not the
  recorded shared holder or the take fails. COWBREAK records a give before
  transmission succeeds. After a shared-page pull, `page_recall_all` can ACK
  bytes without installing them; whole-page failures also clear the loan.
  These are ownership transitions that need transactional failure handling.
- PUTV counts partial successful fallback writes as complete runs, accepts
  any positive total as success, and publishes installation/loan changes
  even on failure. A total length of 4096 does not prove full page coverage
  because overlapping runs are accepted. PUT has the same short-write and
  premature metadata publication problems.
- Fork range breaks ignore failed gives, then spend the protection mark.
  Child enumeration silently caps at 64. Fork protection walks the object
  while peers may change it, treats SEEK_DATA errors as completion, and does
  not propagate protection failures. The claimed fork-time snapshot needs
  an explicit linearization point spanning both machines and all writers.
- `cow_give_page_mode` writes before rechecking exec, then punches the address
  when it detects exec. Either operation can race a new image's own page.
  Its post-write check cannot undo an overwrite of newer bytes.
- `region_of` returns a mutable global-table pointer without map_lock; the
  refresher, fault readers and some layout writers race region changes.
  vvar refresh also checks the region separately from its write, so an unmap
  can recycle the address between those steps. Copying a live vvar page does
  not preserve its seqlock protocol, and equal uname releases do not prove
  identical private vDSO data layouts or clock namespaces.
- Installed-page keys have the same truncated-pid identity problem as loans.
  Insertion reuses a tombstone before searching for an existing key farther
  in the chain, allowing duplicate records and incomplete handover deletion.
  Query reads also race plain writes under installed_lock.
- `fault_from_home_inner` can grant a write after a failed fork-copy give.
  Shared-page pulls classify failures other than BYTES as fresh shared holes,
  permitting zero fill without proving an unwritten page. The shared-peer
  search matches a virtual address without checking shared-object identity
  and offset, and records handover before a successful take/install.
- `pull_gen_check` performs a nested `home_call` for STALETRAIL, overwriting
  thread-local metadata from the page reply (including taken status, base,
  generations and registers). Subsequent checks and acknowledgments can use
  the diagnostic reply's metadata. ACK buffering clears entries before
  checking transmission success; chunk install paths do not acknowledge all
  received taken pages. Re-ask paths also require renewed length validation.
- LOST-STORE and monotonic-word diagnostics rely on lossy installed/mapping
  tables or assumptions about arbitrary application data. They are reports
  of inconsistent observations, not independent proof of a lost guest store.
  `verify_memory` itself issues destructive page requests without completing
  ownership receipt. Signal handlers call stdio and mutex-taking reports,
  which can deadlock if interrupted inside those operations.
- Construction ordering is a 1024-row ring that evicts live mappings based
  on an unproved bound on outstanding replies. Exact-range PROT records can
  replace SET records used to order unmaps; partial unmaps kill a whole
  construction row. A reply's drain sequence is not a general ordering of
  concurrent mapping operations. `layout_order_lock` does not cover all
  object punching and eventual kernel application of constructive replies.
- The mremap move path ignores failed reads/writes, then allows the old
  range to be punched. It does not implement MREMAP_DONTUNMAP semantics or
  preserve a general shared mapping's object/offset relationship. Fixed
  destination replacement ignores failed sibling unmaps.
- Exec wipe scans and modifies `/proc/PID/maps` while peers may be servicing
  the old or new image. It logs unmap failures and continues, retains loan
  rows as guest-owned, and does not retire construction rows or local transfer
  state by image incarnation. Late old-image operations need explicit fencing.
- `forward` skips reading an oversized reply payload but continues on the
  misframed stream. Missing request GETREGS and failed reply SETREGS only log
  or omit state. A destination spawn failure changes a successful source
  clone to EAGAIN without undoing the source child; malloc/pthread failures
  can leave the created context unserviced. The full child register frame
  is reduced to parent GPRs plus selected stack/TLS fields.
- `n_live_children` is decremented by child services but never incremented.
  Main therefore skips waiting for children and can exit while they still
  need service. Early child-thread returns also bypass normal teardown;
  blocking waitpid does not retry EINTR or bound a still-live failed context.
  `task_alive` checks a numeric PID without birth identity.
- Context stacks and private backing descriptors are not released on several
  creation-failure and completion paths. The backing registry caps at 4096
  without retirement. Fork children close only descriptors below 1024;
  higher inherited page-channel descriptors can keep dead sessions open.
  Error returns from vmctx_run still call stdio in a fork of a threaded process.
- Repeated-fault progress uses unsynchronized process-wide counters, so an
  unrelated context's work hides a stuck page. Several timing counters and
  first-entry timestamps race between threads; their summed values are not
  reliable profiling evidence. Initial service tracing reads an uninitialized
  event structure. Startup continues after page-listener timeout/failure and
  after backing allocation failure; these should fail before guest creation.

- Source `as_of` caches pid -> tgid without retirement or task identity. PID
  reuse, a worker exec with de_thread, and CLONE_VM without CLONE_THREAD need
  explicit treatment. A thread group is not a general address-space identity.
  The 1024-row cache silently stops recording new tasks.
- `as_live_ctx` holds the shared cache mutex while opening/reading another
  process's maps. That read can wait on mmap_lock held by a fault awaiting this
  monitor, blocking unrelated address-space lookups behind it.
- Source lifecycle counts start when monitor threads run and end when those
  threads stop. A newly cloned service task can exist before registration;
  zero monitors is not necessarily zero live tasks. The 256-row lifecycle
  table silently stops registering new address spaces and never retires rows.
  Source page routes similarly stop recording at 512 tasks.
- `conn_transit` is a 64-entry list of addresses without a transaction ID.
  Closing the connection changes unacknowledged transits to THEIRS; a send
  attempt does not establish receipt. Review recovery bytes and durable
  acknowledgment ownership before trusting the resulting state.
- `pg_state` changes failure to read the authoritative record into PG_NONE.
  `pg_set_at` ignores failed writes and uses a separate read/write sequence.
  Correctness decisions cannot treat either as an atomic successful claim.
- Source `pg_get_op` treats statuses other than ABSENT and INFLIGHT as successful
  bytes. Validate all reply statuses, lengths and transaction association.
- `prefetch_issue` reads states then overwrites them with claims without CAS.
  On the i-th failed request write, abort restores only i claims although n
  were acquired. Earlier complete replies can also contain the only copy of a
  moved page: `prefetch_read_replies` discards them if a later reply fails.
  `prefetch_settle` discards an EEXIST reply without proving the present bytes
  are at least as current. Failed give-back can revert metadata while losing
  the received bytes. These need failure injection, including partial streams.
- Destination `get_have_page` continues the destructive take after
  `pg_claim_wait` fails. It settles the local page state before retained-copy
  publication and transmission; the generation is read later. Audit whether
  another transfer can intervene and stamp older bytes with a newer generation.
- Destination PUTN records an installation/lending change even if the write
  fails or writes short; a failed partial-page prefill merely logs that the
  remainder will be zeros and continues. PUTV and the remaining write paths
  still require the same review.
- `ctx_poke_owned` ignores all pull failures before writing. `ctx_pull_page`
  also reports a missing route as though no page had been lent. These paths
  can manufacture bytes after a failed ownership acquisition.
- `ctx_monitor_start` leaves readahead fields uninitialized and fails to
  propagate allocation/thread-creation failure to the child start protocol.
- The handwritten synchronous signal frame omits saved signal masks, alternate
  stack metadata and extended CPU state. It does not apply SA_RESETHAND or
  handler masking and can reuse the top of an already active alternate stack.
  Compare with the source kernel's actual signal delivery, including nested
  handlers, sigreturn, FP/SIMD state and stack bounds.
- Shared-file interning matches only exact starting offsets, so overlapping
  mappings at different offsets do not share a canonical object page. A later
  longer mapping at an existing offset can exceed its allocated slot and
  overlap the next object. The 256-slot overflow silently removes sharing.
  dev/inode reuse and IPC namespaces also need explicit object lifetime.
- RELRO tracking is global by virtual address, with whole-row deletion on
  partial overlap; one address space's unmap can erase another's protection
  history. Read-only file state must not rely on this lossy history.
- `ctx_forward` substitutes fork for vfork and strips CLONE_VM from some
  clone requests. This changes sharing and parent-suspension semantics;
  successful posix_spawn examples do not establish syscall equivalence.
- Failed `ctx_call` returns before `srccons_pop`, leaking a construction
  suppression entry that may hide later unrelated unmaps. Mapping records
  are changed after separate assisted calls, while peers may observe them.
- Fork bookkeeping records a child as having received its copy before the
  remote break succeeds. The fixed COW table stops recording after 64 probes,
  and child relationships are never compacted after retirement. Ancestor
  recovery explicitly substitutes current bytes for an unavailable fork-time
  snapshot. That is a change to fork semantics, not correctness recovery.
- The first-load path truncates arguments to 63. Later execs keep a potentially
  incompatible vDSO/vvar on unlike kernels. Failed ptrace/map reads can still
  produce partial-success replies; a full failure cleanup audit remains.
- The source listener binds all interfaces with no peer authentication or
  binding between a connection and its announced source pid. CHILD can name
  an arbitrary pid, START can be repeated concurrently, and disconnect
  teardown acts on those identities. A session must own every context it can
  control; this needs an explicit trusted transport and identity design.
- Existing ws1 checks pthread_create but ignores pthread_join failures, so its
  claim that joining proves all writes finished is not established. Its
  historical oracle also guesses worker identity from syscall count and uses
  fixed binary addresses. Neither should be reused without validation.
