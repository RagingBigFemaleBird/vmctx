# Validation and remaining work

**Not all green.** Written September 9, 2026 by the audit; the gate it names
below (all tests passing on both machines before any commit) was overridden by
the owner on September 10, 2026: the tree was committed and pushed as it
stood, with the kernels squashed to one commit per baseline and the scratch
history summarized (HANDOFF.md §1–§2; the originals under
`../doc-archive/audit-20260906-09/`). Nothing below was changed to look
better than it measured; the "Session 49 gates" section at the end records
what was rerun on the deployed inputs on September 10.

## Deployed and frozen inputs

| Role | Kernel / boot | Module |
| --- | --- | --- |
| AMD source and loopback executor, 10.0.0.229 | #222, 7.0.14-vmctx-audit-recall1-terminal222; fca91ceb-77d2-4bed-842d-b4f3037301d5 | terminal222-native/vmctx.ko; SHA256 254145de8bf70549e47f9f1440f33264a314376300d998e504350172bce22e13 |
| Intel executor, 10.0.0.30 | #71, 6.18.35-0-lts-audit-readobj71; e8e036f7-5adc-4000-b0c2-abd4ba2c2306 | /root/audit-readobj71/vmctx.ko; SHA256 b6b0cffa316885216fa8e185864744487331055a803b9d6234856902d62003bc |

Runtime settle44 is frozen as source 75915bd28abd (296 inputs), with three
warning-free executables, passing host/table controls, and eight shared storage
and settlement controls. Its executable hashes:

- vmhome: c6fd6dd13a8da00272fcab71da5e5856bf0aaca5703e1960eb62289b4f865dc0
- vmremote (Intel): 131a482e63378316a9f9cc3b802169f93b24a4fa6bef33745afe002935614973
- vmremote-local (AMD): 138bb8c7fe333408a52b35c5326f17db300baf045a78aee388c9d8372bda61d7

AMD uses `/home/biwu/audit-20260907/mm-settle44-user-tree`; Intel uses
`/root/audit-readobj71/vmremote-settle44`. The destination process owner is
`controls/native-run-descendants43`, SHA256
51fb77f95c6160a52a5ce3620d584621c4691508869f8f25217ae0f76c07dd20.
The preceding watchwrite40 source 26fd84585fd8 and settle43 source 54a4aa0ebf7c
remain frozen with their manifests and raw failing/passing evidence.

Campaign wrappers and compiled guest/control inputs are recorded separately.
Oracle41 additionally rejects unsettled receipts,
unconfirmed ACKs and unresolved source failures. Its SHA256 is
11075dd4d87aa1c3053e03c45f7e581b7133f60337611c163fefc3b266e25257.

## Results and outstanding gates

B below means `/mnt/d/VM/lan-boot-recovery-20260908/logs/audit-20260907`.
A root-created `*-raw.tar.gz` is the authoritative complete campaign output;
earlier unprivileged partial copies are not equivalent.

| Gate | Result | Evidence under B |
| --- | --- | --- |
| AMD #222 full kernel, external module, initramfs and 6,891 modules | PASS, warning-free; initial unused-helper warning retained separately | terminal222-build-complete.json; terminal222-package-complete.json |
| AMD #222 native vmctx | PASS, 57 controls with complete per-case cleanup | terminal222-amd-native-1 |
| Retained QUERY vs native exit | #221 FAIL at iteration 872; #222 PASS, 2,000 exits / 429,051 successful queries, complete cleanup | terminal42-amd-baseline-1-raw.tar.gz; terminal42-amd-fixed-1-raw.tar.gz |
| Intel #71 package and native vmctx | PASS, 3,767 modules, 26 controls | readobj71-intel-native-1; readobj71 package manifests |
| HMM residency substrate | All 11 probes and unload PASS again using the #222 test module; vmctx integration still absent | hmm222-native-2-raw.tar.gz |
| AMD retire38 / #221 full guest suite | PASS, 78 strengthened cases | mm-claim39-suite-1 |
| watchwrite40 AMD-to-Intel full guest suite | #221 FAIL 77/78; #222 PASS 78/78, including detached thread reuse | mm-cross41-suite-1-raw.tar.gz; continue43-baseline-raw.tar.gz |
| settle44 shared memory / io_uring controls | PASS, sm1, sm2 and 256-submission SQ/CQ control in loopback and cross-host modes | settle44-focused-raw.tar.gz |
| retire38 snapshot stress | PASS, six runs of 1,024 rounds / 16 mutators, fresh-zero oracle | mm-retire38-recall-stress-1 |
| #221 breadth before watchwrite40 | FAIL, 68/72: GPG, CPIO, GDB, Node | mm-claim39-breadth-1-raw.tar.gz |
| watchwrite40 focused breadth | GCC and complete CPIO roundtrip PASS; explicit GPG agent shutdown PASS; GDB and Node FAIL | mm-watchwrite40-programs-1-raw.tar.gz; -2-raw.tar.gz; -3-raw.tar.gz |
| settle44 focused breadth | GCC, Node JIT and Node workers PASS; GDB FAIL (SIGFPE before main despite gdb exit 0) | settle44-focused-raw.tar.gz |
| Ptrace software breakpoint restore | Native PASS; guest FAIL twice: restored source word, repeated executor INT3 | mm-ptrace41-focused-1; mm-ptrace41-focused-2 |
| Native process owner | Five local detached-descendant/exit/EOF/data/signal/deadline modes PASS; older owner fails the regression | native-run-descendants42-validation.json; native-run-report40-descendants-baseline.log |
| Native remote residency on #222 | FAIL all four: UFFD COPY, UFFD ZERO, THP collapse, mremap; complete cleanup, unchanged counters | settle43-followup-raw.tar.gz |
| Firefox interaction and normal shutdown | Pending native, loopback and cross-host gates on current inputs | browser40-fixture; browser-audit.py |
| Final full suites, stress and breadth on final inputs | Pending | No final success claimed |

Intentional native negative controls account for cumulative lost-copyout=1,
fault-deadline=1 and abandoned-transfer=3 after the #222 native suite. Every
ordinary run must leave live MM/module/context/transfer counts zero and must
not increase those failure counts. On #221, the initial GPG failure added one
abandonment, making its old baseline 1/1/4. Do not compare across boots.

## Diagnosed changes and failures

Retire38 orders private mapping retirement like shared retirement, and removes
generation-based byte substitution. Watchwrite40 maps an existing watched
folio read-only even for a nonpresent write fault, then lets the normal write
fault preserve COW and grant access. Source capture now rejects an incumbent
CLAIM/TRANSIT before installing a guard. These have failing old controls and
passing replacement controls; none proves complete native MM coverage.

#222 reads death and monitor attachment under one ev_lock critical section.
The retained descriptor resolver separately establishes terminal ownership.
The old code could observe alive, race monitor detachment, then reject its
own retained QUERY. The same authorization rule covers retained CHILD queries.

The ptrace control writes INT3 into a private executable child page, stops,
restores the original source word and RIP, then receives INT3 at that same
instruction again. Foreign file GUP currently falls through without source
custody recall. This is a native ownership hole, not a breakpoint-specific
flush or checksum-repair opportunity.

Shared settlement and unmapped page-service reads now resolve the shared
object/offset slot. They cannot use private backing at the same VA. Settlement
acknowledges only a full landing into that exact slot and rejects mapping drift,
short writes, write errors and holes without a private fallback. The old code
fails three settlement and two page-service controls; the replacement passes
all eight controls, including negative receipt/ACK cases. The source-page trace
also demonstrates the real sequence: shared bytes land and the following GET
incorrectly checks only private backing. Correcting both paths restores sm1/sm2
and makes both unarmed Node workloads pass. This does not close ownership over
shared aliases in unrelated native MMs or asynchronous native/device writers.

## Required native MM work

The native source still encodes remote ownership as an ordinary absent PTE
plus a separate MM/address record. Measured native UFFD COPY/ZERO install into
that slot, THP collapse zero-fills it, and MREMAP_FIXED loses its provenance.
Evidence: mm-review29-amd-native-2, -3, -4. These remain failures even when
ordinary suites pass. HMM's passing substrate probes are not integration.

Follow [ownership.md](ownership.md): explicit retained page identity across
fork/move, atomic filled-folio landing, foreign GUP fault service without an
mmap-rwsem cycle, atomic-safe secondary TLB revocation on both SVM/VMX, and
object/offset ownership covering shared native and asynchronous writers.
The broader [review obligations](review-obligations.md) remain open until
current code and discriminating controls close them.

## Evidence preservation and finalization

The dated scratch documents, intermediate handoffs and read ledgers were
archived byte-for-byte before consolidation. Archive:
`scratch-documents-before-consolidation42.tar.gz`, SHA256
92631ee59414e9b6bfc07cfb89e94c88e4fc12811b2e2b2c60f1f9c47208d8b6.
Its companion JSON maps all 27 original paths to content hashes. The two
original review ledgers record historical reading, not a fresh read of every
current line. Original failures and rejected experiments remain in B.

The AMD git source tree was stale until September 10; `src/linux-7.0.14`
HEAD now equals the hash-verified AMD build tree (`~/audit-20260907/
mapping-kernel-7.0.14`, every changed file compared against vanilla) and
`src/linux-6.18.35` HEAD equals readobj71-inputs. Each is vanilla + one vmctx
commit and its regenerated patch replays to the identical tree
(`../kernel-patches/README.md`). Generated kernels, private browser/GPG
artifacts and raw logs stay off the repository (recovery drive B). The
commit messages describe the contracts and the exact completed gates and do
not claim that the failures above passed.

## Session 49 gates (September 10, 2026; same deployed inputs, no code change)

| Gate | Result |
| --- | --- |
| home: `make -C vmctx/user` (all tools, id 75915bd28abd); module against src/linux-6.18.35 (srcversion 665E2B59F7ADD5DFE313DB5 = loaded on both boxes) | PASS |
| home: `make -C vmctx/user check-host` | PASS (85 s) |
| AMD: `sync-amd.sh` rebuild + `verify-build.sh amd` (kernel, module file and loaded srcversion, both build ids) | PASS, all-ok |
| AMD: `check-host` on the box | PASS (114 controls) after raising the open-file limit; the login shell's 1024 fails the descriptor control at fd 8192 |
| AMD loopback suite ×1, 78 cases, serial, #222 + terminal222 module | **78 passed, 0 failed**, 5 min 14 s, counters [0,0,0,1,1,0,3] before and after, boot unchanged, no kernel warnings (2026-09-10 00:21–00:26). |
| AMD → Intel cross suite ×1 (`runany-x.sh`, `/root/audit-readobj71` executor and owner) | **78 passed, 0 failed**, 7 min 28 s (00:29–00:36), source counters [0,0,0,1,1,0,3] and destination counters [0,0,0,1,1] before and after, both boot ids unchanged, no kernel warnings on either side; every case's destination cleanup report complete with no survivors. |
| `tests/progs.sh` breadth | **68 pass, 1 fail, 0 skip; known gaps: gdbbt still failing (no breakpoint hit), gpgsym and stracec passing today** (7 min, 00:37–00:44, counters unchanged, boot unchanged). The one failure is **nodejit**: node exits 1 with empty output; remote.log says one context ended on its own with status 131 and its joiner was woken as though it had finished, home.log shows the service task killed by signal 9 -- the Node thread-teardown class the audit still listed (nodework passed this time). progs.sh returns non-zero for a known gap still failing, by design. |
