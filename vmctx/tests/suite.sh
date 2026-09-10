#!/bin/bash
#
# The regression set, in the repository rather than in a home directory on the
# destination box. Every loop that has been used to measure this system lived
# only on 10.0.0.229, which meant a rebuilt box lost the tests along with the
# scratch files, and nothing recorded which cases a change was supposed to keep
# passing. This is that list.
#
# Run it on the destination, which is where local-here.sh puts both halves on
# one machine:
#
#	sudo ./suite.sh [rounds]
#
# It expects the test programs already built in $TESTS (see build below) and
# runany.sh alongside them.
#
# The page tests are here on purpose. th9 and its relatives never put two
# writers on one page, so for a long time nothing in the tree exercised the case
# the coherence protocol exists for; pg1/pg2/pg3 do, and pg2 is the one that
# found the recall window.
set -u

HERE=$(cd "$(dirname "$0")" && pwd)
TESTS=${TESTS:-/home/biwu/tests}
# Beside this file, not on one machine. $RUNANY used to default to
# /home/biwu/runany.sh, which is not in the repository -- so a fresh checkout
# could not run one case, and neither could a rebuilt box. Overridable, as it
# always was.
RUNANY=${RUNANY:-$HERE/runany.sh}
ROUNDS=${1:-3}
# How many cases to run at once. One is the old behaviour exactly.
#
# Nothing in the system was ever exclusive: a run is a (vmhome, vmremote) pair
# on a port of its own, and the module already serves many contexts at once
# because a threaded guest is many contexts. What made runs exclusive was
# local-here.sh -- it never killed the vmhome it started, so the only way it
# could get a clean machine was to kill every vmhome and vmremote on it and
# reload the module. Each run now takes its own vmhome down and VMCTX_SHARED=1
# skips the sweep, so a round is a pool rather than a queue.
#
# Left at 1 by default, deliberately. The page tests are races, and running
# eight of them beside each other is a different machine load and therefore a
# different measurement -- fine for "does anything fail", wrong for comparing a
# rate against a rate recorded serially. Pass -j N (or JOBS=N) when the question
# is the former.
JOBS=${JOBS:-1}
case ${2:-} in -j) JOBS=${3:-1};; -j*) JOBS=${2#-j};; esac
PORT_BASE=${PORT_BASE:-19000}

case "$ROUNDS:$JOBS:$PORT_BASE" in
  *[!0-9:]*|:*|*::*) echo 'suite: rounds, jobs and port base must be integers' >&2; exit 2;;
esac
[ "$JOBS" -gt 0 ] && [ "$PORT_BASE" -gt 0 ] &&
  [ $((PORT_BASE + JOBS * 4)) -lt 32768 ] || {
  echo 'suite: invalid job count or port range' >&2; exit 2; }
pass=0; fail=0; xfail=0; xpass=0

# One case. $1 names the program, $2 is a shell test over $g (guest.out), and
# $3, if "xfail", marks a case known not to work yet.
#
# A suite that always reports failures stops being read, and deleting the cases
# that fail is worse -- so the known gaps are listed here with the rest and
# counted separately. What matters is the two surprises: a known gap that starts
# passing (something was fixed, and this list is out of date) and anything else
# failing at all.
# One case, run and graded. In a pool this is the body of a background job; the
# tag is the case's own name so two cases never share an output directory.
grade_case()
{
	local t=$1 check=$2 slot=$3 g ok=1 rc=0 default_limit=60
	# All 40,000 pg3 rounds take 77 seconds on the validated source6
	# build. an1 also needs more than 60 seconds with transfer ACK ordering.
	# Keep both complete 20,000-round oracles and give them their own budget.
	case "$t" in pg3|an1) default_limit=120;; esac

	VMR_LIMIT=${VMR_LIMIT:-$default_limit} VMCTX_SHARED=1 \
		PORT=$((PORT_BASE + slot * 4)) \
		"$RUNANY" "$t" "$TESTS/$t" >/dev/null 2>&1
	rc=$?
	g=/tmp/tr/$t/guest.out
	[ -f "$g" ] && [ ! -f "/tmp/tr/$t/.timedout" ] || ok=0
	[ $ok = 1 ] && { eval "$check" || ok=0; }
	# A harness fault and a test failure are different things, and grading a
	# case only on guest.out could not tell them apart -- a module that would
	# not load, a port still held, a vmremote that died at startup, all read
	# as "the test failed", and as a *pass* if a stale guest.out survived.
	# runany.sh's status says which: it is about the harness and not about the
	# program, so a case whose guest exits non-zero on purpose (ex5 returns 42,
	# pf3's children die by signal) does not come through here.
	# runany exits 3 when the run left no output at all. That is either a
	# harness fault or a guest that died before printing -- dma1's failing
	# rounds are the second -- so it is reported as what is certain (nothing
	# came out) rather than as a diagnosis.
	if [ $rc != 0 ]; then
		echo "  NO-OUTPUT $t -- the run produced nothing (runany exited $rc)"
		ok=0
	fi
	[ $ok = 1 ] || {
		mkdir -p /tmp/tr/fail.$t
		cp -f /tmp/tr/$t/* /tmp/tr/fail.$t/ 2>/dev/null
	}
	echo "$t $ok" >> "$RESULTS"
}

# Start a case, waiting for a slot if the pool is full. Slot N owns the ports
# from PORT_BASE + 4N, so two live runs never share one.
slot_pid=()
start_case()
{
	local t=$1 check=$2 xf=${3:-} s=0

	if [ ! -x "$TESTS/$t" ]; then
		echo "  MISSING $t -- no such binary in $TESTS; not run"
		echo "$t 0" >> "$RESULTS"
		echo "$t $xf" >> "$KINDS"
		return
	fi
	echo "$t $xf" >> "$KINDS"
	if [ "$JOBS" -le 1 ]; then
		grade_case "$t" "$check" 0
		return
	fi
	while :; do
		for s in $(seq 0 $((JOBS - 1))); do
			if [ -z "${slot_pid[$s]:-}" ] ||
			   ! kill -0 "${slot_pid[$s]}" 2>/dev/null; then
				grade_case "$t" "$check" "$s" &
				slot_pid[$s]=$!
				return
			fi
		done
		sleep 0.2
	done
}

# Drain the pool and tally, so the counters are only ever touched by the shell
# that owns them -- a background job cannot increment its parent's.
finish_round()
{
	local t ok xf

	wait
	slot_pid=()
	# Iterate the requested cases. A worker that vanished before publishing
	# a result is a failure, as are duplicate or malformed result records.
	while read -r t xf; do
		ok=$(awk -v n="$t" '$1 == n { count++; value=$2 }
			END { print (count == 1 && value == "1") ? "1" : "0" }' "$RESULTS")
		if [ "$xf" = xfail ]; then
			if [ "$ok" = 1 ]; then
				echo "  XPASS $t -- known gap now passes; update the suite"
				xpass=$((xpass+1))
			else
				xfail=$((xfail+1))
			fi
		elif [ "$ok" = 1 ]; then
			pass=$((pass+1))
		else
			fail=$((fail+1))
			echo "  FAIL $t"
		fi
	done < "$KINDS"
	: > "$RESULTS"
	: > "$KINDS"
}

run_case() { start_case "$@"; }

# A test that stops compiling used to be graded against its previous binary:
# stderr went to /dev/null, there was no `|| exit`, and $TESTS was never created,
# so on a machine without it every compile failed invisibly and every case failed
# for a reason nothing printed. Say which file, and stop.
build()
{
	local c b rc=0

	mkdir -p "$TESTS" || return 1

	# vmhome and vmremote too, once, here.
	#
	# local-here.sh rebuilds them on every run for a reason it spells out at
	# length -- rsync preserves mtimes, so an updated source looks older than
	# the binary and no timestamp check would catch it. VMCTX_SHARED=1 skips
	# that rebuild, because several gcc's writing one output file while
	# another worker executes it is its own failure. So the build has to
	# happen here instead, and leaving it out reintroduced exactly the
	# staleness those comments are about: a suite run measured a vmhome from
	# before the change it was run to measure.
	make -s -C "$HERE/../user" ../build/vmremote-local ../build/vmhome || {
		echo "suite: monitor pair does not build" >&2; return 1; }
	# The descriptor control dup2()s an object fd to 8192/8193, so the host
	# controls need more than a login shell's 1024 open files. The AMD box
	# gives biwu a soft limit of 1024 (hard 524288) and the control failed
	# there as "FAIL execution descriptors case 0" with nothing else wrong
	# (2026-09-10); home's WSL shell has 1048576 and never showed it.
	ulimit -n 16384 2>/dev/null || ulimit -n "$(ulimit -Hn)" 2>/dev/null || true
	make -s -C "$HERE/../user" check-host || {
		echo "suite: native monitor controls failed" >&2; return 1; }
	for c in "$HERE"/*.c "$HERE"/control/{fork-state,fork-late-sibling,fork-nested-io,signal-state,exception-state,debug-traps,source-clocks,cpu-runtime,cpu-timers,cpu-lifetime,ptrace-step,ptrace-syscalls,exit-source,exec-shared-mm,ptrace-child-start,opaque-call,cpu-model,shared-snapshot,mprotect-gap,guard-page,kernel-buffer-read,byte-io,pipe-stream,uring-rings,group-exit,shared-alias,shared-grow,shared-resize,shared-many,mprotect-thread,mprotect-exit-race,thread-reuse-lock,robust-exit}.c; do
		[ -f "$c" ] || continue
		b=$(basename "$c" .c)
		# tests/ownership.c is a host-side invariant check that no case
		# below runs; it is stale against user/pgstate.h (pg_try_claim
		# takes four arguments now) and does not compile. Left in the
		# tree for whoever brings it back, skipped here so it cannot
		# veto the suite's build (2026-09-10).
		[ "$b" = ownership ] && continue
		if ! gcc -O2 -Wall -static -pthread -o "$TESTS/$b" "$c" -lrt; then
			echo "suite: $b does not compile; the case below would be" \
			     "graded against whatever binary was there before" >&2
			rm -f "$TESTS/$b"
			rc=1
		fi
	done
	if ! gcc -O2 -Wall -static -pthread -DSNAPSHOT_OBSERVERS=4 -DSNAPSHOT_MUTATE=1 \
		-o "$TESTS/shared-snapshot-mutate" "$HERE/control/shared-snapshot.c"; then
		rm -f "$TESTS/shared-snapshot-mutate"
		rc=1
	fi
	if ! gcc -O2 -Wall -static -pthread -DTEST_DETACHED=1 \
		-o "$TESTS/thread-reuse-detached" "$HERE/control/thread-reuse-lock.c"; then
		rm -f "$TESTS/thread-reuse-detached"
		rc=1
	fi
	return $rc
}

if [ "${SKIP_BUILD:-}" != 1 ] && ! build; then
	echo "suite: build failed; no runtime result is valid for this source" >&2
	exit 1
fi

RESULTS=$(mktemp); KINDS=$(mktemp)
trap 'rm -f "$RESULTS" "$KINDS"' EXIT

for i in $(seq 1 "$ROUNDS"); do
	echo "round $i${JOBS:+ ($JOBS at a time)}"

	# Threads: two contexts making forwarded syscalls at the same time.
	run_case th9 \
	  '[ "$(grep -ac "^main   " $g)" = 40 ] &&
	   [ "$(grep -ac "^thread " $g)" = 40 ] && grep -qa "^DONE$" $g'
	run_case th12 '[ "$(grep -ac main $g)" = 30 ]'

	# Address space: a mapping made by a forwarded syscall must reach the guest.
	run_case loc2 \
	  'grep -qa "a1 b2 c3 d4" $g && grep -qa "5a 6b 7c 8d" $g'

	# fork, threads and timerfd in one program; then fork/wait4 pids.
	run_case ex4 'grep -qa "^THREAD-OK" $g && grep -qa "^child exited 0$" $g'
	run_case ex5 'grep -qa "^PIDS-AGREE" $g'

	# fork is copy-on-write, and neither side may see the other's writes.
	#
	# ex4 and ex5 fork and check that the child runs and that the pids agree;
	# neither looks at what the child's memory CONTAINS, so a destination that
	# copied the whole address space at the fork and one that gave the child
	# an empty object both passed them. This is the case that separates them,
	# and it separates the lazy schemes from each other too: the child must
	# see the address space AS OF THE FORK however much the parent writes
	# afterwards, which only holds if every way the parent's page can change
	# hands the old one to the child first.
	#
	# It found three separate defects: the chunk fetch could not execute a
	# COWBREAK answer at all (the child died 242 on its first inherited page);
	# the source's own fault service invented a zero page for memory the child
	# had inherited (clone(2) writing its tid through CLONE_CHILD_SETTID);
	# and a private file page the source did not hold was served from the
	# FILE, which for a forked child is the program as it was loaded -- the
	# child span for ever on a PLT slot the loader had not yet relocated.
	run_case cow1 'grep -qa "^PASS$" $g'

	# One page written from both sides -- what coherence is actually for.
	run_case pg1 'grep -qa "^PASS$" $g'
	run_case pg2 'grep -qa "^PASS$" $g'
	run_case pg3 'grep -qa "^PASS$" $g'
	# hx2: the reliable shared-page hand-over lost-write oracle. Wall-clock
	# bounded, so it always returns a verdict; it failed every run when it was
	# written (a counted lost write, or a coherence-crash kill) where hx1
	# caught it only ~40%, and was marked xfail as the open two-phase-hand-over
	# gap. It XPASSed every round of every suite since the take-generation
	# stamp (session 24) and the page service's non-page answers were fixed
	# (session 25, NOTES §2.18): a regular case now. Serially it passes; its
	# pooled rate (248/256, 8-way) is the stress gate, NOTES §6.1a.
	run_case hx2 'grep -qa "^PASS$" $g'

	# mf2: a fork made BY A WORKER THREAD of a threaded process -- the order
	# every real program uses (glib's pool, glycin's image sandbox that
	# netsurf forks, firefox's sandbox probe), and the one mf1 does not test
	# (mf1 forks from main). Failed every run until session 26: a mapping
	# change fanned out to sibling contexts (VMCTX_CTL_APPLYMAP) was applied
	# by the kernel to current->mm -- the MONITOR's -- so vmremote itself
	# came to map the parent's object at the thread-stack addresses, and
	# every context clone()d from it afterwards inherited that VMA. The fork
	# child read its parent's LIVE stack through it (the saved _Fork canary
	# long overwritten) while its own object held the right bytes. Fixed by
	# never fanning out SET/PROT (and a kernel guard, patch 0069): the
	# child's stack, heap and a sibling's word are its own. NOTES §6.5.
	run_case mf2 'grep -qa "^PASS$" $g'

	# Faults the program raises on itself and handles with its own handler.
	# Everything else about faults is machinery the guest never sees; these
	# are the ones it is supposed to. A program registers a handler, raises
	# the fault itself, and handles it: the destination reports a vector and
	# reads nothing into it, and the side that owns the program decides what
	# it means and answers with somewhere to carry on.
	run_case pf1 'grep -qa "^PASS$" $g'
	run_case pf2 'grep -qa "^PASS$" $g'
	run_case gp1 'grep -qa "^PASS$" $g'
	run_case tr1 'grep -qa "^PASS$" $g'

	# mremap's three vacate shapes (shrink tail, move, MREMAP_FIXED
	# replace): the bytes that survive must move, the vacated range must
	# be GONE -- a touch that reads stale bytes instead of faulting is the
	# ghost netsurf died on at ~30s (exit 242, byte-deterministic).
	# Propagation is the kernel's mm-mutation hook (VMCTX_CTL_MMLOG), not
	# a syscall-number switch; this is the direct proof it covers mremap.
	run_case mr1 'grep -qa "^PASS$" $g'

	# MADV_DONTNEED must actually discard: the next read of a private
	# anonymous range is zeros, and glibc's allocator trusts exactly that
	# when it trims arenas. The hook's UNMAP arm cannot see it (DONTNEED
	# raises CLEAR), so the madvise path notes it as a soft entry: the
	# destination punches content and ZAPs pages, mappings stay. Before
	# that existed this read back 0xaa -- a heap-corruption seed.
	run_case md1 'grep -qa "^PASS$" $g'

	# And the case none of those four covers: a fault the program has NOT
	# asked to handle, which must kill it. All four above register a handler
	# first, which is what made them pass over a source that could not do
	# this one: the guest's rt_sigaction ran in the source-side process and
	# replaced the handler vmhome had installed for its own death, so a
	# program that registered nothing left vmhome's handler in place, and
	# the fault was answered with an address inside vmhome's own text --
	# which is not part of the guest's address space at all.
	#
	# Measured then: native 3/3, and under vmctx all three children exited
	# 242 while the target's dmesg showed each of them dying at rip
	# 0x280623c8, inside vmhome's __dcigettext. See "The source side" in
	# AUDIT.md. The machinery named there (the shadow, sh_deliver_exception,
	# sh_watch_own_death) no longer exists; this case is what keeps the
	# property it was about, and it earned that twice -- it also caught a
	# signal-deferral change that held back SIGSEGV from a program that had
	# earned one, reporting "exited 242 rather than dying" again.
	run_case pf3 'grep -qa "^PASS$" $g'
	# pf3's contract under repetition: 25 rounds x 3 fatal children per run,
	# because the cross-machine campaign saw pf3's fpe miss once in ~136
	# single runs (a #DE child dead 242 with the module silent) -- one shot
	# per run cannot catch a flake of that rate in the act.
	run_case pf5 'grep -qa "^PASS$" $g'
	# fork/exec shapes: fork->exec (dynamic), exec'd child forking, and
	# fork+exec from a worker thread -- the spawn shapes every GUI
	# program takes (posix_spawn, g_spawn, shells). Built in session 41f
	# while reducing netsurf's glycin wall; all pass today and this keeps
	# them passing.
	run_case fx1 'grep -qa "^PASS$" $g'
	run_case exec-shared-mm 'grep -qa "^PASS: child exec, clone-vfork, raw vfork and parent exec preserve distinct MM lifetimes$" $g && ! grep -qa "FAIL" $g'

	# The protections the program was *loaded* with, as opposed to the ones
	# it asked for itself with mprotect. Nothing above covers them.
	#
	# Both halves are needed and both are now in. The source mirrors the real
	# permissions instead of parsing them out of the oracle's map and
	# throwing them away, so sh_may_access() has something true to answer
	# with; and the destination carries the range's protection back with the
	# page, as a VMCTX_MAP_PROT beside the bytes, so the first touch stops
	# being read-write-execute. A write to read-only text then arrives as a
	# protection fault, which is the path that does ask the owner.
	run_case pf4 'grep -qa "^PASS$" $g'

	# Signals that no exception produced: raise, masks, siginfo, nesting,
	# SA_RESTART around a forwarded blocking syscall, and a signal sent by
	# another process. The destination has no concept of a signal, so all of
	# this has to work as "a frame in the program's memory and a register
	# set" and nothing else.
	run_case sig1 'grep -qa "^PASS$" $g'

	# Files, whose descriptors, bytes and offsets all live on the source.
	run_case fi1 'grep -qa "^PASS$" $g'

	# And what a dup or a fork does to a descriptor: two fds or two processes
	# on one open file share an offset, two opens do not. If the destination
	# were keeping any state of its own about descriptors, this is where it
	# would disagree.
	run_case fi2 'grep -qa "^PASS$" $g'

	# Memory shared by something other than a thread. Threads share through
	# the backing object; everything else has to be made to work by the same
	# rule -- a syscall that maps memory on one machine maps it on the other.
	#
	# All four of its cases pass. Worth knowing what that does and does not
	# say: every one of them shares by inheritance or within one process, so
	# they are satisfied by two mappings landing on one page of the
	# destination's backing object. None of them ever looks at the file. sm2
	# below is the case that does.
	run_case sm1 'grep -qa "^PASS$" $g'

	# The same question asked the other way: two processes that share nothing
	# else and reach one page by *name* -- a file, a POSIX shm object, a
	# System V key -- rather than by inheriting a mapping. Only the machine
	# holding the file can know the two are one page.
	#
	# All four, file-writeback included, and its two halves were separate
	# defects found separately.
	#
	# The sysv-* cases: shmctl was issued as IPC_STAT|IPC_64, because a
	# 64-bit libc adds that bit. Issued as a syscall it must not be --
	# shmctl(2)'s own SYSCALL_DEFINE3 passes IPC_64 as a separate version
	# argument and ksys_shmctl() switches on the raw command, so 0x102
	# matched no case and returned -EINVAL. Measured outside vmctx
	# entirely: IPC_STAT|IPC_64 -> -1 errno=22, IPC_STAT -> 0 segsz=4096.
	# Every shmat was therefore reported as a call that mapped nothing, and
	# the two attachments of one segment became two unrelated pages -- in
	# silence, behind an early return that logged nothing.
	#
	# file-writeback: a MAP_SHARED file mapping is the one range that can
	# be observed *without* the mapping, so no fault on this side ever
	# brings the guest's stores home -- msync writes back this side's page
	# cache, munmap discards, and a read of the file goes nowhere near the
	# address. The range is now pulled back before those two calls and
	# poked into the context's own file mapping, which dirties the page
	# cache, which is the file. The destination learns nothing: it is asked
	# for a page, as always, and has no idea why.
	run_case sm2 'grep -qa "^PASS$" $g'

	# The case sm2 cannot distinguish, and the hard guard for cross-process
	# sharing. sm2's two processes map at whatever mmap picks, which is the
	# same address in both -- and shared_page_from_peer() moves a page
	# between contexts *by guest address*, so sm2 passed even when the two
	# mappings read from two different objects. Fixed at two different
	# addresses, only real object sharing can pass: measured FAIL before the
	# global interning and the move to the shared object, PASS after.
	run_case sm3 'grep -qa "^PASS$" $g'

	# The read direction of a shared file mapping. All four cases pass, so the
	# xfail marker is gone -- and the reason it is safe to remove is a mechanism
	# and a number rather than three lucky rounds, which is the mistake dma1's
	# marker below records.
	#
	#   initial-guest / initial-forward  the file's *pre-existing* contents,
	#     read through the mapping. The destination now asks its shared object
	#     whether it actually has the page (SEEK_DATA, not "the mapping comes
	#     from the object") and asks the owner where it does not. Fixed in
	#     AUDIT part XIV.
	#
	#   store-forward / exit-writeback  the guest stores through the mapping and
	#     hands the address to write(2), or simply exits. The source pulls back
	#     every shared-file page its own record says the guest holds before any
	#     forwarded call reads one -- and that record was never set for any such
	#     page, ever, because ctx_movable_ranges() leaves a MAP_SHARED file range
	#     out of the movable set, so ctx_page() never takes one and never writes
	#     the hand-over down. Measured before the fix: 3 ranges recorded, the
	#     reclaim walked 24 pages of them, 0 THEIRS, 0 pulled. A page of such a
	#     range served by copy is now recorded as the guest's -- which is a
	#     statement about currency, not location, and is exactly what the reclaim
	#     needs. Measured after: 3 THEIRS, 3 pulled, both cases ok.
	#
	# Proved with arms alternating on one boot (the shared-file THEIRS record
	# the only difference): 0 of 2 against 2 of 2, then 3 of 3 standalone and
	# 3 rounds of 3 under this suite. sm1/sm2/sm3/fm1 stayed 3 of 3 beside it.
	# The switch is gone; the record is unconditional now.
	run_case rd1 'grep -qa "^PASS$" $g'
	# The forwarded-I/O buffer integrity oracle (session 40): every byte of
	# a 2 MiB file read back through ONE buffer, verified and classified
	# (stale-from-block-N / unwritten / garbled). Holds shut the class the
	# readahead opened once -- a landing into a RUNNING context overwriting
	# a page the call had just written; gzip caught it 1 run in 7, nothing
	# in the suite would have.
	run_case io1 'grep -qa "^PASS$" $g'

	# A page pinned by a real device while the other machine claims it.
	#
	# Everything else here moves pages through the CPU. This one hands the
	# buffer to a storage controller with O_DIRECT, which pins it and returns
	# only when the device has written it -- so between the pin and the
	# completion the page tables say nothing about what is about to touch the
	# page, and taking it away does not stop the write.
	#
	# Measured: native 0 bad rounds out of 200, under vmctx 22 to 200 bad,
	# which is memory a device wrote into a page that had been given to the
	# other machine. vmctx_folio_may_move() asks about the DMA pin *before*
	# the zap now rather than after it (AUDIT parts IX and X), which is why
	# this XPASSes most rounds -- 3 of 3 in the runs recorded in parts X
	# through XIII.
	#
	# The marker is GONE, and this time with the sixteen runs the previous
	# removal was put back for. It was removed once on "XPASS 3 of 3" twice,
	# which is three rounds and not a rate, and dma1 then failed 2 rounds of
	# 6. Measured now, on kernel #101: **16 of 16 standalone**, and XPASS in
	# every round of five consecutive `suite.sh 3` runs (15 of 15). If it
	# fails again, put the marker back WITH the run that failed, not with a
	# judgement.
	run_case dma1 'grep -qa "^PASS$" $g' 

	# The guest's stack has to be able to grow. Nothing here covered it
	# until /usr/bin/wc would not run at all: every other case lives on the
	# stack the loader already touched, and this one walks 256 KiB down its
	# own in page steps, which is what a stack-clash probe loop does. It
	# found two separate defects -- fixup_user_fault() never grows a
	# VM_GROWSDOWN VMA, so a context could not grow a stack at all; and the
	# destination's headroom pages were written into the backing object
	# rather than through the guest's mapping, which installs no page and
	# livelocked until the watchdog killed the guest.
	run_case stk1 'grep -qa "^PASS$" $g'

	# A file mapped PRIVATE. Nothing in this list did that until the
	# destination stopped rewriting such a mapping into anonymous memory and
	# serving it itself -- the arm that removal replaced had no coverage at
	# all, and this found three separate defects in its replacement: a
	# source-side process that exited because Linux refuses userfaultfd on a
	# private file VMA (that mechanism is gone; the case it found is not), a
	# read-only range that could not be written by any of the three ways
	# this side installs a page, and a munmap that left its bytes at the
	# next mapping's offset in the object.
	run_case fm1 'grep -qa "^PASS$" $g'

	# dl1 -- the dynamic loader's boundary page: the same file page mapped
	# twice (the tail of one PT_LOAD, the head of the next), read then
	# written then pumped through forwarded syscalls, remapped fresh each
	# round as every dlopen does. This is netsurf's 7-9s death reduced to a
	# deterministic case: native PASS, vmctx FAIL 199/200 before kernel #147.
	# FIXED there (patch 0068): a take of the doubly-mapped file page
	# COW-breaks it to a private copy but then re-looked-up the fresh anon
	# folio instead of acting on the stale file page-cache one, which was
	# refused forever (refs=3 want=2 anon=0) and hit the fault deadline. The
	# VACATE-LATE instrument fires here but is NOT the producer -- it was the
	# red herring the isolation variants (dl1a/dl1b) cleared.
	run_case dl1 'grep -qa "^PASS$" $g'

	# The Firefox shape: a path string written into a private file mapping,
	# the range mprotected read-only (what a loader's RELRO does), the
	# pointer handed to open(2). The store moved the page to the guest's
	# machine; the owner's later fault on it is in a read-only private file
	# mapping -- the class its fault hook historically answered from the
	# file, which by then is stale. The mapped file's original content is
	# itself a path to a decoy file, so the failure mode names itself: a
	# stale refill OPENS THE DECOY. Fixed by the class-aware fault answer
	# (kernel hook widened to every private file mapping + the monitor
	# consulting its hand-over record) -- see vmhome's ctx_monitor().
	run_case rf2 'grep -qa "^PASS$" $g'

	# A NAMED anonymous mapping is still anonymous. jemalloc and glibc name
	# their arenas with prctl(PR_SET_VMA_ANON_NAME), which shows as
	# "[anon:...]" in maps; vmhome once read that "[" as a special file and
	# served the arena by copy, so a path the guest built in one was read
	# stale by a forwarded openat -- exactly how firefox died. an1 names its
	# own anon mapping and opens a decoy when the stale copy is served.
	# Fixed by classifying a mapping by its inode, not its name.
	run_case an1 'grep -qa "^PASS$" $g'

	# A page lent to a thread must outlive that thread. The loan is keyed by
	# (address space, page) but its owner is one shadow pid, so when that
	# shadow exits the recall channel goes and the page is recallable from
	# nowhere -- alive at neither site. main then reads back whatever the
	# local kernel invents. This is ws1's and netsurf's remaining failure
	# reduced to four steps and named at the byte; see tests/lo1.c.
	run_case lo1 'grep -qa "^PASS$" $g'

	# Failures under load belong in the graded set. Each run's watchdog
	# owns its process group and a timeout always fails the grade.
	run_case mf1 'grep -qa "^PASS$" $g'
	run_case fs1 'grep -qa "^PASS$" $g'
	run_case ws1 'grep -qa "^PASS$" $g'

	# The other half of the same search, and this one is clean. firefox's
	# sandbox does not use fork(2): it uses clone(CLONE_NEWUSER|SIGCHLD),
	# measured off the wire (VMREMOTE_TRACE_CLONE=1, flags=0x10000011). That
	# makes a child and a user namespace in one step, which under vmctx is
	# three operations across two machines -- the destination builds the
	# child context, and the source must fork a shadow AND put it in the
	# matching namespace, because the clone executes over there while the
	# address space lives over here. ns1 does exactly that call, with threads
	# already running as the browser has them.
	#
	# STILL xfail, and only just. The fork copy-on-write work (ARCHITECTURE
	# §7.0a) took it from a case that produced nothing under the suite to one
	# that XPASSes in 11 of the last 12 rounds, and 18 of 20 standalone. It
	# keeps the marker because ~1 round in 10 is not green: removing it would
	# make the suite go red intermittently and teach the reader to ignore the
	# line, which is the mistake dma1's history above records. What is left
	# is not the clone or the namespace -- it is the thread defect ws1 (2 of
	# 20), fs1 (1 of 10) and mf1 (2 of 10) share, a glibc abort out of
	# corruption around thread creation. Fix that and this goes green with
	# them; it is the last marker in this file that is about the fork path.
	#
	# It leaks too: a round that produces nothing leaves two vmremote-local
	# processes SPINNING, so `pkill -x vmremote-local` belongs after any
	# suite run that reports NO-OUTPUT here, and any timing measured before
	# that cleanup is against the wrong machine. (I claimed the opposite
	# here on one run's evidence and the next run disproved it -- the
	# leak follows the hang, not the test.) The leak is its own defect:
	# VMR_LIMIT is supposed to bound the run and does not bound this.
	#
	# The leak still matters and is now the reason a suite round can lie: a
	# leftover from an earlier run held a core and made round 1 of a suite
	# report th9, th12 and loc2 as NO-OUTPUT while rounds 2 and 3 were clean.
	# Check `ps -eo stat,comm | awk "$1 ~ /D/"` before believing a round.
	#
	# It started passing every round in session 24 and has every round since
	# (a regular case as of session 25). Check firefox before reading that as
	# the fork path holding under load: that is the blocker the browser is
	# sitting behind, and this is only its serial form.
	run_case ns1 'grep -qa "^PASS$" $g'

	# The pid-namespace half of the sandbox shape, session 41f's wall 2:
	# the sandbox is pid 1 of a fresh CLONE_NEWPID namespace and FORKS its
	# payload in there. The assisted clone then returns the NS-relative
	# pid ("2"), and before kernel #167 (VMCTX_CTL_LASTCHILD) vmhome
	# addressed the source's tasks by that number in the HOST namespace --
	# ctls and kills landed on kthreadd, the real child parked unadopted,
	# and bwrap (glycin, netsurf's image loader) hung at its payload fork
	# forever. pid1 is that exact three-generation shape with the hang
	# converted to a loud SIGALRM death; the guest's own view of the
	# numbering (pid 1, fork returns 2) is asserted too, so a fix that
	# leaked host pids into the namespace would also fail it.
	run_case pid1 'grep -qa "^PASS$" $g'

	# fontconfig's mapping pattern (netsurf's session-42 wall): mmap a
	# file, read, munmap, and the kernel hands the NEXT file the same
	# hole -- while a second thread's calls compete for the change-log
	# drain and flood enough vacates that they spill past one reply's 8.
	# Before kernel #168 the spilled vacate could arrive stamped NEWER
	# than the re-mmap's construction (the stamp was assigned at the
	# DRAIN -- a userspace race between connection threads), and the
	# destination then legally punched the live mapping: the first read
	# of the fresh font answered -14, exit 242. The hook now stamps
	# events as it logs them; fc1 fails as SIGSEGV/242 or as a stale
	# page naming the previous file's bytes.
	run_case fc1 'grep -qa "^PASS$" $g'

	# Clocks belong to the source OS. Source exec omits the native vDSO
	# so libc uses forwarded calls, independent of destination OS or release.
	run_case vd1 'grep -qa "^PASS$" $g'

	# Architectural state and source-kernel semantics, with native controls
	# in control/. These expose behavior the older smoke set did not test.
	run_case fork-state 'grep -qa "^PASS: fork preserves parent and child extended state$" $g && ! grep -qa "FAIL" $g'
	run_case fork-late-sibling 'grep -qa "^PASS: fork snapshot survives a sibling.*32 pages$" $g && ! grep -qa "FAIL" $g'
	run_case signal-state 'grep -qa "^PASS kill=0 seen=1 entry=PASS.*return=PASS" $g'
	run_case mprotect-gap 'grep -qa "^PASS" $g && ! grep -qa "FAIL" $g'
	run_case mprotect-thread 'grep -qa "^PASS: sibling permissions" $g && ! grep -qa "FAIL" $g'
	run_case mprotect-exit-race 'grep -qa "^PASS: 24 sibling exits" $g && ! grep -qa "FAIL" $g'
	run_case robust-exit 'grep -qa "^PASS: 32 native robust owner deaths" $g && ! grep -qa "FAIL" $g'
	run_case thread-reuse-lock 'grep -qa "^PASS: two creators reuse one stack for 64 joined threads across forks" $g && ! grep -qa "FAIL" $g'
	run_case thread-reuse-detached 'grep -qa "^PASS: two creators reuse one stack for 64 detached threads across forks" $g && ! grep -qa "FAIL" $g'
	run_case opaque-call 'grep -qa "^PASS: guest call identifiers cannot invoke transport operations$" $g'
	run_case cpu-model 'grep -qa "^PASS: generic CPUID, bounded XSAVE, XCR0, prefixed CPUID, fork/exec identity" $g && ! grep -qa "FAIL" $g'
	run_case cpu-runtime 'grep -qa "^PASS: CPU runtime compute" $g && ! grep -qa "FAIL" $g'
	run_case cpu-timers 'grep -qa "^PASS: source process/thread/virtual CPU timers" $g && ! grep -qa "FAIL" $g'
	run_case cpu-lifetime 'grep -qa "^PASS: CPU lifetime" $g && ! grep -qa "FAIL" $g'
	run_case shared-grow 'grep -qa "^PASS: shared growth" $g && ! grep -qa "FAIL" $g'
	run_case shared-resize 'grep -qa "^PASS:" $g && ! grep -qa "FAIL" $g'
	run_case ptrace-step 'grep -qa "^PASS: source ptrace single-step" $g && ! grep -qa "FAIL" $g'
	run_case ptrace-child-start 'grep -qa "^PASS: child stayed stopped for .* ms and entered with its final ptrace-edited CPU state$" $g && ! grep -qa "FAIL" $g'
	run_case ptrace-syscalls 'grep -qa "^PASS: source syscall tracing" $g && ! grep -qa "FAIL" $g'
	run_case exit-source 'grep -qa "^PASS: source ptrace suppresses exit" $g && ! grep -qa "FAIL" $g'
	run_case debug-traps 'grep -qa "^PASS: TF and ICEBP" $g && ! grep -qa "FAIL" $g'
	run_case uring-rings 'grep -qa "^PASS: independent io_uring rings" $g && ! grep -qa "FAIL" $g'
	run_case shared-many 'grep -qa "^PASS: 96 live read-only shared mappings" $g && ! grep -qa "FAIL" $g'
	run_case shared-snapshot 'grep -qa "^PASS: 128 sealed shared-memory snapshots, sibling source calls=" $g && ! grep -qa "FAIL" $g'
	run_case shared-snapshot-mutate 'grep -qa "^PASS: 128 sealed shared-memory snapshots, sibling source calls=" $g && ! grep -qa "FAIL" $g'
	run_case source-clocks 'grep -qa "^PASS: source clocks advance and stay authoritative across exec$" $g'
	run_case exception-state 'grep -qa "^PASS exception state: entry=1 return=1 frame=1 mask=1 stack=1 mask_return=1$" $g'
	run_case guard-page 'grep -qa "^PASS" $g && ! grep -qa "FAIL" $g'
	run_case kernel-buffer-read 'grep -qa "^PASS" $g && ! grep -qa "FAIL" $g'
	run_case byte-io 'grep -qa "^PASS: adjacent byte reads" $g && ! grep -qa "FAIL" $g'
	run_case fork-nested-io 'grep -qa "^PASS:" $g && ! grep -qa "FAIL" $g'
	run_case pipe-stream 'grep -qa "^PASS:" $g && ! grep -qa "FAIL" $g'
	run_case group-exit 'grep -qa "^PASS: 8 threaded child group exits preserve parent execution and status$" $g && ! grep -qa "FAIL" $g'
	run_case shared-alias 'grep -qa "^PASS: shared aliases preserve object identity across different file offsets and unmap$" $g && ! grep -qa "FAIL" $g'

	# NOT RUN: tests/pk1.c, the cross-page peek. It builds the geometry that
	# killed firefox -- an openat() path 48 bytes below a page boundary, the
	# read buffer above it, on the stack, 200 rounds -- and the clamp it is
	# meant to exercise fires on all 200. It still passes with the clamp OFF,
	# so it does not discriminate and is not in the suite: a case that is
	# green in both arms measures nothing and would report the bug fixed if
	# the fix were reverted. The clamp's own arms are unambiguous on the real
	# workload (firefox, one boot, identical probe cost in both: clamp on
	# 3/3 clean and 80 libraries, clamp off 3/3 "invalid ELF header" and 55),
	# so what pk1 is missing is whatever makes the spill INSTALL a page there
	# and not here -- the kernel's foreign-read refusal
	# (vmctx_foreign_anon_fault, reached only from do_anonymous_page) catches
	# it in pk1 and does not in firefox. Until that is understood pk1 stays
	# as the record of the geometry, run by hand.
	finish_round
done

echo "suite: $pass passed, $fail failed; $xfail known gaps still failing, $xpass now passing"
[ $fail = 0 ]
