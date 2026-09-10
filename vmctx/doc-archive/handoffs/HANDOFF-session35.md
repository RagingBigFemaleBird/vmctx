> **RETRACTED in session 36 (HANDOFF-session36.md):** the "foreign read
> REFUTED" item below (#2) was judged by `vmctx_ffile_invent`, which looks the
> folio up BEFORE the fault proceeds and cannot see a TAKEOBJ punch landing in
> between. A census at the allocation itself (kernel #150) named exactly that
> door — the monitor's PEEK — as the maker of every all-zero hx2 page, and
> #152/#153 close it. The rest of this file stands.

# HANDOFF — session 35 (2026-08-22): REDESIGN step 2b BUILT and GATED — the BUSY faulter BLOCKS on the landing; the chain is green or flat everywhere; the hx2 residual is a kernel-made zero folio whose door is NOT yet named (MAPOBJ, foreign-read and decline all REFUTED by counters)

Follows `HANDOFF-session34.md` ("Step 2b NEXT" was the spec; it was built as
specified). The owner's standing instruction applies: judge by the coherence
story, accept a flat 1:1 pass count, revert only on a WORSE failure mode.

## What was built (SRCID `e61a3d7e2aa6` on .229, kernel #149 unchanged)
The ownership-channel claim moved UP from the two `pull_begin` sites (the 2a
observer) to `fault_from_home_mode`, the wrapper every real guest fault goes
through (vec 14, `!supply_only`; the only caller is child_thread's fault loop,
coh depth 0):

- `ownf_claim(ak, pk, &g)` = `own_begin(TO_REMOTE)` → CLAIMED / BUSY / OFF.
- BUSY → `ownf_wait` = `own_wait(3 s)` → woken → `return 1` (the re-fault finds
  the page, or claims the now-free transition). Bound expiry → the fault runs
  UNGATED (the pre-claim racing path), `n_ownf_wait_timeout++` (must be ~0).
- CLAIMED → the existing pgstate dance unchanged → `fault_from_home_inner` →
  `present = as_any_present()` (reused for the settle) → `ownf_release`:
  `own_land(OWN_REMOTE)` if present, `own_abort` otherwise. EVERY exit
  releases, including the `pg_claim_prio < 0` yield.
- The four site-level `ownf_begin/ownf_end` hooks are gone; prefills stay
  ungated; `pg_serve_conn` never touches the entry; NO source gate.
- `n_ownf_woken` counts as fault progress in the STUCK watchdog (a wake is a
  deliberate "try again", like the yields already counted there).
- Summary line `[vmremote] ownership channel (fault claim): ...`; census.sh
  reads it (`ownf_claimed/waited/woken/wake_absent/timeout/full/landed/aborted`,
  `own_begin_busy`, `own_wait_timeout`).

## Results — chain `s35` (suite x1 + 48x8 hx2/pg3/ws1/sig1, graded by census)
| pool | s34obs (observer baseline) | s35 (2b) |
|------|---------------------------|----------|
| suite | 34/0 | 34/0 |
| hx2 | 46/48 | **42/48** (same mode, see below) |
| pg3 | 46/48 | **48/48** |
| ws1 | 47/48 | **48/48** |
| sig1 | 48/48 | **48/48** |

Channel lifecycle exact in all four pools: `woken == waited` (a handful short
only where a run ended mid-wait), **`wait_timeout` 0, `full` 0** everywhere.
- hx2: ~275 waits/run absorbed the ~263 BUSY/run the observer measured.
  **`pull_overlap` 6447 (48 runs) → 0** — the destination no longer puts two
  copies of one page on the wire. `claim_yields`/run 499 → 34: the storm
  SHRANK (session 30's source gate blew it up to ~94000/run; this is the
  opposite). ~35% of waiters wake to a page still absent (the claimer aborted:
  a yield or ABSENT) and simply claim on the re-fault.
- pg3/ws1/sig1: waits are rare (99 / 40 / 0) — consistent with session 34's
  finding that the receiving-side race is hx2-specific.

**hx2's 6 fails are ONE mode, identical to the baseline's 2:** `FAIL 1` — one
writer's store read back as ZERO (`1 zero, 0 behind`), page 0x7ffff7ff6000,
exit 0, no hang, no death, no deadline kill. Not a worse mode → 2b stays.

## The hx2 residual: what was established and what was REFUTED (with counters)
Every hx2 run prints `ZERO-TAKE` (page_take_mode's photograph of an all-zero
capture of a page the record says was transmitted) — loss or not, so unlike the
TRAIL (dumped only by LOSS-PROBE) it is a control. A ZERO-TAKE of the hx2 page
at count > 0 — "captured an all-zero page ... from a context's mapping
(TAKEOBJ)", every member present=0 afterwards, object has it now: 0 — appears in
**5 of 6 s35 fails and 0 of 42 s35 passes** (baseline s34obs: 0/2 fails, 2/46
passes; the later `s35n` pool: 1 in a PASSING run, 48/48). The trails agree:
`TAKE-AWAY sum=04000001` (an all-zero page leaves for the source), then the
source's copy comes back as `PULL-BYTES sum=64757773` or `c275ddd9` — the SAME
sums across unrelated runs = deterministic content: zero writer slots plus the
evacuators' 0x5a buffers. So a context had an ALL-ZERO folio mapped for the
hx2 page mid-run; the trail has NO userspace install event for it (the last
events before the zero take are a sibling's PULL-BYTES / OBJECT-MAPPED of real
bytes), so the zero folio was created by the KERNEL, not by an install.

**Refuted, each by a live counter (read on .229, kernel #149):**
1. **MAPOBJ allocating on a punched hole** — the mechanism `serve_object_mapped`'s
   comment describes. Patch 0055 already makes MAPOBJ map only a folio the
   object holds (`filemap_lock_folio`); `/sys/module/kernel/parameters/
   vmctx_mapobj_holes` = **0 over 96.8 M calls**. The userspace comments at
   `serve_object_mapped` and the write-grant MAPOBJ predate 0055 and are stale:
   neither the unlocked write-grant MAPOBJ nor the GET-from-object punch
   outside `ctl_lock` (pg_serve_conn ~5640, the one punch not under the lock)
   can invent through MAPOBJ any more.
2. **A foreign read faulting a hole in** (`peek_at`'s pagemap check → a take in
   between → `VMCTX_CTL_PEEK` = `access_process_vm` faults → `shmem_fault`
   allocates a zero folio): the kernel counts exactly this as
   `vmctx_ffile_invent` (143 since boot) with an A/B switch
   `vmctx_foreign_file_refuse` (default N; refusing was measured to break rd1
   twice — see the comment at kernel/vmctx.c ~880). Across a 48-run hx2 pool
   the counter did **not move** (143 → 143) while a ZERO-TAKE occurred; with
   the switch ON (`s35y`) there was nothing to refuse. Not hx2's producer.
3. **A declined fault** (monitor answers SELF → local kernel): vmremote's
   "faults declined" = 0 in every hx2 run; the kernel's `decl_*` module
   counters (vmctx module namespace) did not move across a pool.
4. **The mm-hook fallback** (`vmctx_anon_fault` → `vmctx_af_fallback`): not on
   the path at all — the guest's pages are MAP_SHARED of the backing memfd and
   that hook excludes VM_SHARED by design ("that mapping IS the answer").
5. ACT_DONE for a fault is "re-execute" (same as RETRY) — a woken waiter's
   `return 1` with nothing installed re-faults; it does not hand the fault to
   the local kernel.

**What remains open:** a zero folio entered a live context's object with no
userspace event and none of the instrumented kernel doors. Candidates not yet
instrumented: a kernel-mode fault inside the context (an assisted syscall's
copy_to_user on the object VMA goes to `handle_mm_fault` → `shmem_fault`
directly, no vmctx hook, no counter); a stale translation to a freed+reused
folio (the TAKEOBJ re-read detector saw 0 stale STORES, but a stale READ
mapping is not what it measures). The decisive next instrument is in the
KERNEL: count every `shmem_fault` allocation on a live context's backing object
by path (user #PF redirected→SELF / kernel-mode fault in the context / foreign
gup / MAPOBJ), with the faulting task and address — so the door is NAMED
before any fix. That is a core rebuild (#150) + reboot; authorized.

Note also: a zero take is NOT sufficient for the loss (it happens in passing
runs), and the loss needs the zero page to be the copy the writer reads from
next — which is timing. The 2b landing-wait changes that timing (siblings are
parked) and is the likeliest reason the arm's share shifted; it did not change
the rate (42/48 then 48/48 on the same build).

**A real defect found on the way, fixed in userspace (deployed+measured, see
below):** `peek_at` counted a SHORT `VMCTX_CTL_PEEK` (0 or fewer bytes than the
present page it was asked for — the page left between the pagemap read and the
PEEK, or the kernel refused a hole) as a FULL page read, serving whatever the
buffer held before. New counter `n_peek_short` (summary line "PEEKs answered
SHORT..."); this is very likely why "refusing at fault time" looked like it
lost rd1's store: the refused read was served as stale bytes.

## Also seen
- One hx2 smoke run (s35d, 1 of 8) died `FATAL: A PAGE OF THE GUEST'S MEMORY IS
  ON NEITHER MACHINE` at 0x419000 (text), exit-98 class (downstream of a lost
  page, per [[teardown-page-coherence-class]]); 0 in the 48x8 pools.

## Traps (new this session)
- torture.sh's dir-name count files every hx2/sig1 run as `storm.N` and the
  chain log prints "pass=1 fail=48": grade ONLY by census (`guest.out ^PASS$`).
- The TRAIL is dumped only by LOSS-PROBE: passing runs carry NO trail. A
  "takes=0 in every passing run" is absence of data, not a control.
- Box jobs: `export SUDO_ASKPASS=~/askpass.sh SUDO_PASS=biwu` (askpass.sh
  echoes `$SUDO_PASS`), and use the ABSOLUTE key path
  `/home/desktop/lan-boot/config/id_vmctx` — a `cd` in the ssh command line
  broke the relative path once.
- A python splice anchored on `s.index("static void plog(")` hit the forward
  declaration (line ~200), not the definition, and duplicated 3000 lines;
  anchor on the definition (`...)\n{`).

## Final gate — chain `s35p` (2b + the peek_at fix + the comment updates)
Box SRCID `3bf3b73bdbd6`, kernel #149. By census (guest.out ^PASS$):
suite 34/0 (x3 across the session's chains); **hx2 47/48, pg3 45/48, ws1
48/48, sig1 48/48**. pg3's 3 fails are the pre-existing fatal family (2x
"page on neither machine" exit-98, 1x exit-242), NOT this change: `n_peek_short`
read 0 in every pg3 AND hx2 run, so the peek_at fix is provably inert on the
load and the deaths are the known death-class variance (s34obs pg3 was 46/48
of the same family). 2b over four pools of two builds: hx2 42/48, 48/48,
46/48, 47/48 = 183/192 (95%) vs baseline 46/48 (96%) — flat, one FAIL-1
zero-read mode throughout, no hang, no worse mode. Coherence moved:
`pull_overlap` 6447->0 per pool, `claim_yields`/run 499->34, `wait_timeout`
and `full` 0 everywhere.

## State (COMMITTED, green)
- **Committed:** one squashed commit on `master`, `419f7f8` — author AND
  committer `Bi Wu <biwu85@gmail.com>`, `Co-Authored-By: Claude Fable 5`.
  Per [[commit-convention]] it FOLDS IN the previously-unpushed 26-28 tip
  (41af447), so the subject spans sessions 26-35; tree verified unchanged by
  the squash. **NOT pushed** (origin/master is 1 behind).
- Box `.229`: kernel #149, SRCID `3bf3b73bdbd6` = the committed tree
  (verify-build amd OK), idle, no leftover processes, `vmctx_foreign_file_refuse`
  restored to N. Nothing else armed.
- Tree contents of the commit: 2b (fault_from_home_mode claim + own.h wired
  load-bearing), the session-31 `as_any_present` GET fix, the `n_peek_short`
  short-PEEK fix, the two MAPOBJ comments marked historical, own.h, census.sh,
  gate.sh, REDESIGN.md, HANDOFF-session29..35.md.
- Memory: `session35-step2b-landing-wait` (START HERE), indexed in MEMORY.md.

## What the next session does
Two independent forward paths, neither started:
1. **The kernel instrument (the decisive one for the hx2 residual).** Rebuild
   the module/core (#150) + reboot (authorized) to count every `shmem_fault`
   allocation on a LIVE context's backing object BY PATH — user #PF
   redirected->SELF / a kernel-mode fault inside the context (an assisted
   syscall's copy_to_user on the object VMA hits handle_mm_fault->shmem_fault
   with NO vmctx hook and NO counter today) / foreign gup / MAPOBJ — with the
   faulting task and address. This NAMES the zero folio's door before any fix;
   all three userspace-visible doors are already refuted by counters.
2. **REDESIGN step 3:** migrate the source's `pgown`/`infl[]` onto the same
   `own.h` segment (the source claims a fetch as one transition, a second
   faulter waits on it) — closing the source-side half of the loss family.
   Then step 4 deletes the tie-break machinery the entry made unreachable.
   Judge by the coherence story; do NOT add a source-side FETCH GATE (regressed
   hx2 in sessions 30/31/32).
