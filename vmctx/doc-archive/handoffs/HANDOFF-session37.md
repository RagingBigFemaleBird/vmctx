# HANDOFF — session 37 (2026-08-22): hx2's zero-read class stays fixed; its ~0.7% residual is NAMED as the deep teardown thread-list coherence (a point fix was tried and honestly reverted); a real browser (links2) RENDERS end-to-end on this tree; netsurf's wall is browser-internal

Follows `HANDOFF-session36.md`. The user asked to green hx2's residual (the
NULL-deref / take_lost) and then move to browsers.

## take_lost — BENIGN, not a corruption (asked, answered)
`vmctx_take_lost` fires ~2-4 per chain, always in PASSING pools, never at a
failure. Code path: it increments AFTER the zap with `mapcount=1` -- a sibling
context faulted the same MAP_SHARED memfd folio in during the precheck->zap
window. The zap removed only THIS context's PTE and never punched the object,
so the bytes survive in the sibling; the TAKE caller retries EBUSY up to
1000x1ms (vmremote.c:2392) or serves via the object. It never persisted to a
timeout. Do NOT widen the precheck window -- the kernel comment
(vmctx_folio_precheck) says it regresses. Left as-is.

## hx2 residual — the deep teardown thread-list coherence (REDESIGN frontier)
The session-36 zero-folio fix made hx2 48/48 in the clean case. The residual is
a ~0.7% death (1 in ~150 chain runs; **0 in 1024 hx2-ONLY runs** -- it needs
the full chain's suite-then-pools load; an isolated hx2 pool never reproduces
it). Photographed with a new **LOST-PAGE** trail dump added this session at the
fatal decline (vmremote.c ~10918; kept -- a cheap, capped diagnostic that names
the frontier). Two sub-modes, one root (a thread descriptor/stack page's
content is GENUINELY GONE from both machines at pthread_join):
- **exit -1, "no output"** (s373, and ws1 s381): the source says its address
  space ended (`ctx_page_pull`->`PULL_GONE`), `ctx_stop_unservable` SIGKILLs
  the still-live joiner before hx2 prints PASS. Trail: `LOST: 0x7ffff7ff5000
  was lent to shadow N (lent whole, its channel is gone)` + `the owner's
  address space has ended`.
- **exit 242, NULL-deref** (s384, and the s36 chains' 0x7ffff77f3000): a
  thread-list pointer reads 0 -> `mov %rax,0x8(%rsi)` rsi=0 -> write to 0x8 ->
  `DECLINED-BY the-source-has-no-such-address: 0x0 (err 0x6)`. Its RIP is
  always `0x4542e0` = glibc `__nptl_deallocate_stack` unlinking the exited
  thread from `stack_used`.

**A point fix was tried and REVERTED (honest):** serving the RETAINED copy on
`PULL_GONE` before the SIGKILL. Gate38 (5 chains): the new counter fired **0
times** while 2 deaths still occurred -- because by teardown the retained copy
is already forgotten (the join's munmap) AND the exit-242 sub-mode never
reaches PULL_GONE (it is the ABSENT-decline path). No regression (suite 34/0,
pools 48/48 bar the 2 deaths), but inert, so reverted per no-dead-code. The
content is truly lost; no fallback reconstructs it. **The real fix is REDESIGN
step 3** (source `pgown`/`infl[]` onto the own.h shared segment: a page must
not die when the context that pulled it exits while a sibling still needs it),
or a targeted "evacuate a thread-context's lent-whole pages into the object
before its channel closes" -- but prior teardown point-fixes were fragile;
instrument first. This is the same `LOST: ... lent to shadow N` the code names
at vmremote.c:1963 as "the whole of what is left in ws1 and netsurf."

## Browsers (loopback, kernel #153, this tree, browser-run.sh, display proved empty first)
| browser | window | page painted (root colours) | fetch | run | verdict |
|---|---|---|---|---|---|
| **links2 -g** | 2, "Links - AVM" 1230x949 | **767 -> 1020** | **GET /page.html 200** | clean | **RENDERS end-to-end** |
| netsurf-gtk | 2, "netsurf" 1000x700 | 767 -> 767 (none) | none | 108 s, 8923 fwd syscalls, **no crash, no faults** | window up, no fetch |

- **links2 fully renders** (fetches AND paints) on all of this session's fixes
  -- the current-tree confirmation of the cross-machine objective (last proved
  kernel #45). No faults, no program errors.
- **netsurf never opens a fetch socket -- DIAGNOSED (see
  `netsurf-no-fetch-is-glycin-sandbox` memory).** netsurf-gtk (GTK4) decodes
  images with **glycin**, which decodes each image in a **bubblewrap sandbox**
  (`bwrap ... --seccomp .. /usr/libexec/glycin-loaders/2+/glycin-image-rs`),
  triggered at STARTUP loading GTK's own UI icons -- before any page fetch.
  Under vmctx the sandbox is forwarded to the source: mount/pivot_root run in
  the SOURCE's real namespace (namespaces are not virtualized -> `/newroot` on
  the source), and the bwrap child HANGS after pivot_root + prctl(PR_SET_SECCOMP)
  and never exec's glycin-image-rs (child 652132's last real syscall is
  `prctl(0x16,0x2) -> 0`, then it stalls). netsurf's main loop blocks on the
  undelivered icons and never dispatches the page fetch -- no HTML GET, no
  socket. **Proven by controls:** native netsurf even with NO session D-Bus
  runs the SAME bwrap/glycin sandbox AND fetches+renders (window "AVM -
  NetSurf", GET /page.html 304, an established socket) -- so netsurf itself is
  fine; and vmctx with `GLYCIN_SANDBOX_MODE=disabled` removes ALL
  bwrap/seccomp/newroot activity but then decodes in-process and exits 242 at
  3.86 s (the same lost-page coherence class). The seccomp EFAULTs are probes,
  not the blocker. Two vmctx limitations: (1) no mount/user-namespace
  virtualization for a bubblewrap sandboxed child (the fork-from-threaded-parent
  shape); (2) glycin in-process decode hits the exit-242 coherence class.

## State
- Tree: the committed #150-#153 kernel work is unchanged; the only lan-boot
  change this session is the 17-line **LOST-PAGE** diagnostic in vmremote.c
  (kept), folded into the session commit. Kernel trees untouched this session.
- Box `.229`: kernel #153, this userspace (SRCID 4506e85e89bc), idle.
- Memory: `session37-teardown-thread-list-residual` (START HERE), indexed.

## What the next session does
1. **hx2 fully green = REDESIGN step 3** (own.h shared ownership for the
   source's pgown/infl), which also closes the ws1/netsurf teardown death --
   they are one bug. Do NOT re-try a retained/point fallback (inert, gate38).
2. **netsurf render** (diagnosed this session, not a fetch/socket bug):
   glycin's image-decode BUBBLEWRAP sandbox is the wall. Either virtualize
   mount/user namespaces so a guest's sandboxed child (clone+unshare+
   pivot_root) runs in its OWN namespace -- large -- or fix the in-process
   decode coherence class (REDESIGN) and ship `GLYCIN_SANDBOX_MODE=disabled`
   to the guest. Option 2 overlaps the hx2/REDESIGN work. links2 -g needs
   neither and renders today.
