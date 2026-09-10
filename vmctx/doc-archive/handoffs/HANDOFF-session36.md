# HANDOFF — session 36 (2026-08-22): the hx2 zero folio's door NAMED by a kernel census and CLOSED at the allocation — every pool green (hx2 48/48, pg3 48/48, ws1 48/48, sig1 48/48, suite 34/0)

Follows `HANDOFF-session35.md`, whose first forward path this is: "count every
shmem_fault allocation on a live context's object BY PATH". It was built, it
named the door in the first pool, and the fix is three lines at the place the
folio is made.

## Kernels this session (src/linux-7.0.14; the box's tree is a copy, not a checkout)
| build | what | chain | suite | hx2 | pg3 | ws1 | sig1 |
|---|---|---|---|---|---|---|---|
| #149 (s35 baseline) | — | s35p | 34/0 | 47/48 | 45/48 | 48/48 | 48/48 |
| **#150** | the census + stamp + ZERO-TAKE naming; memory.c's `vmctx_fault_seen` hook (in git since session 27, never shipped) | s36a | 34/0 | 46/48 | 47/48 | 48/48 | 48/48 |
| #151 | refusal of a foreign READ of a hole, at the allocation, on ALL shmem | s36b | **33/1 (rd1)** | **48/48** | **48/48** | 48/48 | 48/48 |
| **#152** | the same, on kern-mounted shmem only (memfd, MAP_SHARED-anon; not tmpfs files) | s36c | 34/0 | **48/48** | **48/48** | 48/48 | 48/48 |
| #152 | confirmation | s36d | 34/0 | 47/48 (one exit-242 NULL deref, §below) | 48/48 | 48/48 | 48/48 |
| **#153** | the proven A/B switch DELETED (no optional correctness) | s36e | see the end of this file | | | | |

All chains: `tests/chain.sh <tag>` (now tracked — the box's untracked
`chain32.sh` plus kernel counters before/after every pool, dmesg streamed to
`/home/biwu/dmesg.<tag>.log`, per-pool `SHMEM-ALLOC`/`ZERO-TAKE` tallies),
48x8 pools graded by guest.out.

## The instrument (#150) — `vmctx_shmem_alloced()`
One hook at the one place every folio of a shmem object is created,
`shmem_get_folio_gfp()`'s `alloced:` label (mm/shmem.c), gated on
`vmctx_nr_active`. On a live context's object (an i_mmap walk bounded to the
index finds a context mm mapping it — plus a small cache of objects seen live)
it classifies by who asked — `self_user`, `self_kern` (with `lsvc` = inside
the declined-to-local service), `mapper` (a non-context task's own mapping,
identity-offset only), `foreign_write` (the install), `foreign_read`,
`file_write`/`file_falloc`/`file_other` — counts (`/sys/module/kernel/
parameters/vmctx_shm_*`), prints every non-install event (`SHMEM-ALLOC
<path>: page 0x.. created by pid P comm, addr, ip, sysnr, vmctx/lsvc/msys/
write`), and STAMPS a 65536-entry direct-mapped table keyed by (object, page)
with the last allocation. TAKEOBJ, when the page it captures is all zero,
looks the stamp up and prints who made the folio and how long ago
(`ZERO-TAKE 0x.. pid P (#n): the object's folio was created N ms ago by
<path> -- pid, comm, addr, ip, sysnr ...`), counted as
`vmctx_takeobj_zero{,_named,_unnamed}`.

**It fired and it was complete: 266 of 266 all-zero takes in s36a were named,
0 unnamed** (s36b 258/258, s36c 260/260, s36d 518/518).

## What it said (s36a, #150, no fix)
- Makers of all-zero pages: `foreign_write` 167 (ABSENT-fill installs of a
  zero page, mostly the stack top 0x7fffffffd000 — legitimate), and
  **`foreign_read` 5 — every one the hx2 page 0x7ffff7ff6000, every one
  `vmremote-local` at syscall 473 (`vmctx_ctl`) = `VMCTX_CTL_PEEK`, 0-1 ms
  before the take.** Both hx2 FAILs (`other.22`, `other.36`) had one; 1 of
  the 46 passes did. The zero folio is necessary; timing decides the loss.
- The mechanism: `peek_at()` checks the pagemap (PRESENT), a TAKEOBJ punches
  the hole, then the PEEK's `access_process_vm` GUP read-faults the hole and
  `shmem_fault` ALLOCATES a zero folio — the object now "holds" the page as
  zeros and `serve_object_mapped`/MAPOBJ map them as the address space's
  memory. Session 35's refutation of this very door was by the wrong
  counter: `vmctx_foreign_file_fault()` (`ffile_invent`) looks the folio up
  BEFORE the fault proceeds, so a folio present at that instant and punched
  a microsecond later passes it, and `shmem_get_folio_gfp`'s own `repeat`
  loop finds the hole and allocates. **`ffile_invent` 10 → 10 while
  `shm_foreign_read` 10 → 18** in one chain. The same is why the old
  `vmctx_foreign_file_refuse` arm "had nothing to refuse" (s35y).
- The source side has the same door: vmhome PEEKing index 0 of the guest's
  MAP_SHARED-anon objects (4 per chain). And 1-2 `self_kern lsvc` per chain
  = declined faults, the known decline class.
- `shm_mapper` ~1k/pool on #150 was an instrument imprecision: vmremote
  first-touching the own.h ownership segment (a memfd its contexts inherit,
  so the i_mmap walk matched it) at `own_begin` inlined in
  `fault_from_home_mode`. Refined in #151: a mapper counts only on an
  identity-offset VMA (the guest object's shape). 0 since.
- Refuted by config, for the record: shmem THP is `never` at every size on
  the box (`nr_shmem_hugepages 0`, mTHP `shmem_alloc 0`), so the
  large-folio / partial-punch-zeroing candidate is closed; the core's
  `take_large` note is about real-file page-cache folios.

## The fix (#151 → #152 → #153) — `vmctx_shmem_may_alloc()`
Asked in `shmem_get_folio_gfp()` right before it allocates (after the last
lookup, so the punch-in-between is seen): a FOREIGN READ fault
(`vma->vm_mm != current->mm`, `!FAULT_FLAG_WRITE`) on a context mm's object
returns -EFAULT — VM_FAULT_SIGBUS to GUP, a short count to
`access_process_vm`, which `peek_at` already handles (`n_peek_short`, the
session-35 fix) and vmhome's `ctx_peek` returns as a short read. Counted
`vmctx_shm_refused`, printed `SHMEM-REFUSED foreign_read: page 0x.. is a hole;
pid P comm is told so instead of a zero folio`. A foreign WRITE (the install)
and the context's own faults are untouched.

- **#151 refused on every shmem object and broke rd1** (suite 33/1 —
  `store-forward` and `exit-writeback`, the exact shape of the two earlier
  refusal experiments recorded at `vmctx_foreign_file_fault`'s comment).
  rd1 maps `/tmp/rd1.XXXXXX` and the box's /tmp is tmpfs: the owner's
  sh_file reclaim PEEKs the FILE's page before a forwarded write(2) to pull
  the guest's store over it; told "hole" it pulled nothing. A named tmpfs
  file's hole is the file's zeros by the file's own semantics and the file
  machinery owns its coherence; the anonymous object IS the memory.
- **#152 refuses only on the kernel-internal shm mount** (`SB_KERNMOUNT`:
  memfd_create objects and MAP_SHARED|MAP_ANONYMOUS), counting the let-through
  as `vmctx_shm_filed`. rd1 back (34/0), and the pools stayed clean:
  `refused` 12 per chain (9 vmremote on the hx2 page), `filed` 8 (all
  vmhome, the tmpfs file), `foreign_read` allocations **0 in every pool of
  s36b/c/d**, every all-zero take an install.
- **#153 deletes the switch** (`vmctx_shm_refuse_foreign_read`); the
  numbers stay in the comment at `vmctx_shm_refused`. The old racy
  `vmctx_foreign_file_refuse` switch is still in the tree (default N,
  inert): delete in the next tidy together with its comment's claims, which
  this session superseded.
- **Ported to `src/linux-6.18.35`** (the Intel destination's tree; compiled
  locally, `kernel/vmctx.o` + `mm/shmem.o`, NOT run — .30 is at #54 and was
  not touched). TAKEOBJ's zero check sits before its `kunmap_local` there
  (no `shadow` copy in that tree).

## The remaining non-green, for the record
- s36d hx2 `died.18`: exit 242, `fault 0x0 ... declining` — the guest
  dereferenced NULL 21 s in; the run's only decline is that NULL page, no
  SHMEM-REFUSED near it, no all-zero take of a text page. The pre-existing
  §6.1a family at ~1/200 hx2 runs (s35d saw one at 0x419000). Not this
  change's.
- `vmctx_take_lost` reached 1 in s36a and 2 in s36c (`TAKE 0x4b5000: claim
  refused AFTER the zap (#2)`, 11:48:36, while every s36c pool passed) —
  the must-stay-zero class recurs at ~1 per chain regardless of this
  change; the pg3 exit-98 deaths of s35p (3/48) did not recur in 4 chains.
- `self_kern lsvc` 1-6 per chain: declines. Known; the directory (REDESIGN
  step 3) is the answer, not a refusal.

## Traps (new this session)
- **A counter that looks up state before the operation is not a counter of
  the operation.** `ffile_invent` refuted the right door for a whole
  session. Count where the thing happens (the allocation), not where a hook
  can see it coming.
- `set -e -o pipefail` + `strings ... | grep -m1` ends the build script
  (SIGPIPE) BEFORE its "DONE" line, with the install already complete;
  `build-15x.sh` now writes `strings` to a file first. Check
  `/boot/vmlinuz-*` mtime and `strings` for the build number regardless.
- The box's `mm/memory.c` was one hunk behind git (the `vmctx_fault_seen`
  hook, session 27); `build-149.sh` only ever copied `kernel/vmctx.c`. The
  four sources now go over by scp and are md5-compared before a build;
  originals of the #149 tree are in `/home/biwu/kernel-150-backup/`.
- A detached chain launched inside `sudo -A bash -c "... &"` over ssh holds
  the ssh open; `& disown` + a `timeout` on the ssh is fine — the chain runs.
- `/tmp` on the box is tmpfs, so a test's "real file" under /tmp is shmem.
- The dmesg `ZERO-TAKE` line is the kernel's (TAKEOBJ); vmremote's
  userspace `ZERO-TAKE` print is a different, older instrument.

## State at the end of the session
See the final section appended after chain s36e. Kernel trees to commit:
`src/linux-7.0.14` (#150-#153, one commit) and `src/linux-6.18.35` (the
port), patches regenerated per `vmctx/kernel-patches/README.md`; lan-boot:
`tests/chain.sh`, `tests/gate.sh` counters, this file, NOTES §1, REDESIGN.md,
memory `session36-zero-folio-door-named`.

## What the next session does
1. **REDESIGN step 3** (unchanged from session 35): migrate the source's
   `pgown`/`infl[]` onto the own.h segment; then step 4 deletes the tie-break
   machinery. No source-side fetch gate. The declines (`self_kern lsvc`) and
   the source-side PEEK-of-a-hole (now a short read the source must answer
   from whoever holds the page) are the directory's cases.
2. Tidy: delete `vmctx_foreign_file_refuse` and rewrite
   `vmctx_foreign_file_fault()`'s comment (its "it never happens" and its
   rd1 attribution are both superseded); mark the session-35 "foreign-read
   REFUTED" lines in HANDOFF-session35/NOTES as retracted by §above.
3. The exit-242 NULL-deref family (§6.1a) and `take_lost` at ~1/chain.
4. Deploy #153's userspace-independent kernel to the Intel box when .30 is
   next rebuilt (the port is compiled, not run).

## Final gate — chain s36e on #153 (the switch deleted), box SRCID 3bf3b73bdbd6
**suite 34/0; hx2 47/48; pg3 48/48; ws1 48/48; sig1 48/48.** 260/260 all-zero
takes named, every one an install; `refused` 15 (11 vmremote on the hx2
page), `filed` 8; 0 splats; no leftovers. The hx2 miss is `died.39`: exit
242, `fault 0x0 ... declining` — the same NULL-deref family as s36d's
`died.18`. `take_lost` 2 (12:11:40 and 12:16:17, both inside passing pools).

**Over the four chains on the fix (s36b/c/d/e, 192 hx2 runs): ZERO
zero-read failures** (the `FAIL 1 ... 1 zero, 0 behind` mode that was 2/48
at the session-35 baseline and 2/48 on #150) **and zero foreign_read
allocations in any pool.** The residual is now one class, exit-242 NULL
deref, at ~1/100 hx2 runs, plus `take_lost` at ~2/chain — both pre-existing
and both §6.1a's frontier, not this door.

## State (COMMITTED)
- `src/linux-7.0.14` **5a1e236c7** (#150-#153 as one commit; the box runs
  it as #153, sources md5-identical), `src/linux-6.18.35` **6c1638a82** (the
  port; compiled, not run); both series regenerated into
  `vmctx/kernel-patches/` (41 + 72 patches).
- lan-boot: one squashed commit on `master` (folds the unpushed session 26-35
  commit per the convention), NOT pushed.
- Box `.229`: kernel #153, module + userspace from this tree, idle,
  `vmctx_foreign_file_refuse` N (inert), `core_pattern` /tmp/core.%e.%p.
