# Principles: who owns a page, and what to do when you can't

These are the rules this project keeps relearning at measured cost. They are
written down so the next change is checked against them before it is made, not
after it is refuted. Each one cites where it was paid for.

## 1. A page has exactly one owner, and ownership is taken, not inferred

A page may change machines only through an **atomic claim made in the kernel**,
where the facts are: `vmctx_folio_claim()` freezes the folio's reference count
to zero if and only if it is exactly what the page cache, the mappings and the
caller account for, and fails if anything else — a CPU, another context, a
device with a DMA pin — can still reach it. That is the same primitive
`mm/migrate.c` uses before it moves a folio.

Reading a reference count, comparing it, and copying afterwards is not a claim;
it is the same question with a window after it, in which any reference can be
taken. `tests/dma1.c` is the measurement of that window: a device writing into
a page the other machine had already been told it owns. No care in the
page-table paths can prevent it — it has to be **refused before the hand-over**,
by asking whether anything can still reach the page.

Userspace never decides a page looks free. The kernel claims it or says why not.

## 2. Failing to acquire a page means WAIT

`-EBUSY` means **"not yet"**. It never means "no", and it never means "absent".

The answer to a refused claim is to wait and ask again — the pin that refused
it is a disk read measured in milliseconds, not a state. The answer is not:

- falling back to a weaker per-context sequence — that is doing by hand exactly
  what the kernel just refused to do, reopening the window the claim closed;
- answering the source ABSENT — the bytes exist, this side holds them;
- substituting any contents whatsoever.

Two kernels (#36, #38) refused nearly every hand-over and stalled the system to
0/8. The lesson of that was **not** "refusing is too dangerous, so hand the page
over anyway". It was that the *count being compared* was wrong by a constant
(+1 in every population, anonymous and file-backed alike — an accounting error,
not a holder), and the only way that was found was an instrument that reported
the real numbers instead of refusing. A refusal must always say what it saw
(`pr_warn "page busy": refs, want, mapcount, pinned, lru, anon`), because a
refusal with no numbers is indistinguishable from a wrong comparison.

If a wait genuinely exhausts, the honest outcome is a **named, counted
failure** of that hand-over — the repair belongs where the claim failed, never
in a substitute answer downstream.

## 3. Never invent an empty page

A page that cannot be produced is an **error, routed to the owner**. It is not
a page of zeros.

Fabricating memory to make a call succeed is the failure class this project's
own log documents more than any other, and every instance was silent while a
real error would have been loud:

- `VMR_OP_CTXPAGE` zero-fills where the protocol document promises EFAULT
  (AUDIT G2) — the exact place the protocol forbids it, in the exact words.
- `peek_at()` memsets the span it could not read and returns success; a
  `clone3` whose args straddle an absent page runs with fabricated flags
  (AUDIT A5).
- `page_read_as()` reports a zero page as the address space's memory when the
  pagemap could not even be read (AUDIT A6).
- A context with no monitor falls through to `do_anonymous_page()` inventing a
  zero page — the exact failure the design forbids (AUDIT E2).
- The take path once answered ABSENT for a page whose bytes were in the same
  log line that refused them, and the source invented zeros for the word a
  joining thread was spinning on (`vmremote.c`, the pg1 join).

The rule, stated once for every layer: **when this side cannot produce the
page, it says so, with an error; it does not say "here is the page" about
bytes that are not the page.** A crash points at itself; a fabricated page
points at nothing and costs days.

## 4. Memory problems get memory-level fixes

No syscall-specific special case may carry a coherence or ownership repair.
The kernel — which sees faults, mappings and reference counts — is where
memory facts live; a table of syscall numbers in userspace is a list of the
cases someone has met so far, and its omissions are silent (AUDIT part D: the
hand-maintained lists, each missing members). The one special case that exists
(`mmap` rewritten to anonymous) is the direct cause of a standing failure
(`sm1`, "File mmap becomes anonymous") and is recorded as a defect, not a
pattern to extend.

## 5. Instrument before diagnosing; boot before believing

Every confident symptom-level diagnosis in this line of work was refuted by
measurement: the NPT/TLB story, the two-phase take, SEEK_DATA, the VM_SHARED
exclusion, the LRU theory — five for five. Four changes were committed and
then reverted after measurement. One was committed as a fix without running
the suite and was a 20-failure-per-round regression for an hour.

So:

- A kernel change **compiles** is a statement about the compiler. It is
  committed as unverified until it has **booted** and been **measured**.
- Measure with controls: keep the passing runs; a "present in the failing run"
  means nothing until it is absent from the passing ones.
- A confident zero usually means the check never ran. Verify an instrument can
  fire before believing its silence.
- One vmctx run per machine; one machine operation (build / install / reboot)
  at a time, each confirmed complete before the next begins.
- Write measurement loops as files and review them; inline loops fail half the
  time and fail plausibly.

## 6. The destination owns nothing — and that is why the page rules hold

The destination is an execution engine: it runs code and holds pages, and at
any instant it may hand back every context and the source must carry on from
its local copy. Everything above derives from this. Ownership must be a real,
atomic, kernel-level fact precisely because *neither side is allowed to
believe a stale userspace picture of the other*; a page whose ownership is
"probably here" is a page two machines will eventually both write.

### 6a. The destination has no concept of a file

Stronger than "owns nothing", and stated separately because it is the rule most
easily broken by accident. The destination has no filesystem the program can
reach, no notion of an executable, a path, an inode or a mapping's backing. It
never opens the program, never `execve`s it, never reads its libraries. The
source loads the program, the source maps its files, and what crosses the wire
is **pages** — bytes at addresses — with nothing said about where they came
from.

Assume the destination is a machine that could not open the program's files if
it wanted to: a different operating system, a different filesystem, no disk.
Everything must still work, because nothing on that side is entitled to know
what a file is.

**What this forbids, concretely.** Any code on the destination that opens a path
belonging to the program, infers a mapping's contents from its backing file,
special-cases a filename, or asks a question whose answer only a filesystem
could give. Any protocol message that carries a path, an inode, or a file
offset as something the destination is meant to *interpret* rather than ferry.

**What it permits.** The destination reading `/proc/<pid>/{maps,pagemap,stat}`
about ITS OWN tasks — that is introspection of the engine, not of the program,
and it names addresses rather than files. And the SOURCE using its own
filesystem freely, which is the whole point of it being the source: `file_local`
answering a private-file fault from the source's own file is the design working,
not a violation, because the destination is not consulted and learns nothing.

**Audited, 2026-08-10.** Every filesystem call in `vmremote.c` is
`/proc/<pid>/maps`, `/proc/<pid>/pagemap`, `/proc/<pid>/stat` or
`/proc/self/fd/N`. There is no `execve`, and the program is never opened.

**Closed, 2026-08-10.** `vmremote` used to take the program on its command line
and ferry the strings to the source in `VMR_OP_START`. It never interpreted them
— it could not — but a path is a source-side concept with no business in the
destination's interface: it made the operator name, on the machine with no
files, something only the other machine could resolve. Now:

    vmhome <port> [-v] <program> [args...]    the source decides what runs
    vmremote <home-ip[:port]> --net           the destination only connects

`VMR_OP_START` carries no payload, and `oracle_start()` ignores anything sent
with it and says so once. Trailing arguments to `vmremote` are accepted and
ignored with a warning, because every script in the tree passed them and a run
that dies on its command line teaches nothing. Suite 75/0 across the change.

## 7. When something breaks, instrument it — never guess

**Whenever something breaks, build an instrument and find evidence. Do not
guess at what is wrong.** This is the rule that produced every real fix in
this tree, and every violation of it cost hours.

The record is not ambiguous. In one session twelve consecutive
symptom-level diagnoses were refuted by measurement — the NPT/TLB story,
the two-phase take, SEEK_DATA, the VM_SHARED exclusion, the LRU theory,
stale loans, punched holes, the presence test, the tri-state, the stale
pagemap handle, both own-object reads. Each was plausible, each was argued
from reading code, each was wrong. Every defect that was actually fixed was
named by an instrument first:

- the LRU-add batch, by a refusal that printed its own counts;
- the killed context, by a `dump_stack()` at a deadline;
- the wedge, by `/proc/<pid>/stack` of a task nobody could kill;
- the invented page, by a kernel probe reading the guest's own mapping
  after the monitor claimed to have served it;
- the fabricated zeros, by a page-content fingerprint on both ends of the
  wire — and by knowing that `page_sum()` of an all-zero page is
  `0x04000001`, which is how "the checksums match" hid a perfectly
  transferred page of nothing.

Corollaries, each learned the expensive way:

- **An instrument can manufacture what it reports.** A `copy_from_user()`
  probe on a `MAP_SHARED` object with a punched hole *allocates* the zero
  page it then reports. Prefer non-instantiating reads (`FOLL_NOFAULT`,
  `filemap_get_folio`) and run the control with the instrument off.
- **A log line tells you which code ran, not which bytes the program got.**
  Follow the data, not the control flow: fingerprint the buffer, then read
  it back from where the program will actually read it.
- **Tag every branch that can answer.** "Which of these eleven `return 1`
  paths answered?" is one run with tags and unanswerable without them.
- **Absence of evidence is evidence only if the check can fire.** Verify
  the instrument fires at all before believing its zero.
