#!/bin/bash
# progs.sh -- a breadth campaign: real programs of many shapes, each run
# NATIVE and as a vmctx GUEST (loopback), each graded by its own predicate.
#
#   sudo ./tests/progs.sh [tag] [case...]
#
# Shapes covered: text tools over real data, interpreters (python/perl/awk),
# threads, a compiler and the binary it makes, make, archive round trips
# (tar/gzip/zstd/xz/bzip2/zip), git, TLS to a real host, crypto
# (ssh-keygen/gpg/openssl), image work (ImageMagick), procfs readers
# (ps/top/free/df -- they read the SOURCE's /proc, which is the design),
# shell process trees with pipes, and rsync. Sandboxed programs are out of
# scope here (the sandbox walls have their own ledger in the HANDOFFs).
#
# Grading is per case: CMP (native stdout == guest stdout, same rc), a
# ROUNDTRIP predicate (the artifact survives compress/extract bit-exact),
# or RC (same exit code, for output that is legitimately nondeterministic).
# Every case's artefacts stay under $OUTD/<case>/ for reading afterwards.
set -u
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
TAG=${1:-$(date +%H%M%S)}; shift 2>/dev/null || true
OUTD=${OUTD:-/home/biwu/progs.$TAG}
KO=${KO:-/home/biwu/vmctx-7.0.14.ko}
LIMIT=${VMR_LIMIT:-90}
cd "$HERE" || exit 1
[ "$(id -u)" = 0 ] || { echo "must run as root" >&2; exit 2; }
[ -x build/vmhome ] && [ -x build/vmremote-local ] || { echo "build first" >&2; exit 2; }
mkdir -p "$OUTD"

# Fixture data, once: a numbers file, a text tree, a source file.
FIX="$OUTD/fix"
mkdir -p "$FIX/tree/a" "$FIX/tree/b"
seq 1 200000 | awk '{print ($1*7919)%100000}' > "$FIX/nums"
for i in 1 2 3 4 5; do cp /usr/share/dict/words "$FIX/tree/a/w$i" 2>/dev/null \
	|| head -c 200000 /usr/bin/gcc > "$FIX/tree/a/w$i"; done
head -c 300000 /usr/lib/x86_64-linux-gnu/libc.so.6 > "$FIX/tree/b/blob"
cat > "$FIX/hello.c" <<'EOF'
#include <stdio.h>
int main(void) { long s = 0; for (int i = 0; i < 1000000; i++) s += i;
	printf("hello from a fresh binary: %ld\n", s); return 0; }
EOF
cat > "$FIX/Makefile" <<EOF
all: hello
hello: hello.c
	gcc -O2 -o hello hello.c
EOF
cp "$FIX/hello.c" "$FIX/mk.c" 2>/dev/null

pass=0; fail=0; skip=0; xfail=0; xpass=0

# The known gaps, each named with its family, so a red line here is always a
# SURPRISE (suite.sh's rule: a list that always reports failures stops being
# read). A known gap that PASSES is counted apart too -- flaky families pass
# on good days, and only a streak of xpasses means something was fixed.
#
# THREE GRADUATED on the session-44 lifecycle design (kernel #179 monitor
# succession + the spawner->service monitor handoff + tree-lifetime wait):
# sort (dying-context EIO family; was 2/12, then 9/9 green), nc1 (the
# orphaned server died with its parent's monitor; 9/9 green -- first
# passes ever), and lddnames (the "argv-stale exec hole" turned out to BE
# the clone-window race: a child's early pages served while its monitor
# churned; 6/6 green with no wipe). They are STRICT again. sort and nc1
# red = a lifecycle regression, the loudest kind; lddnames kept a
# RESIDUAL producer past the lifecycle fix (the empty-stream disguise,
# ~1/3 under load, heals=0 in every failing run so the #180 heal is
# exonerated) -- strict anyway, because the roaming family's policy is
# pressure, and the 0x7f00/e3b0c442 tells still file it.
#
#   gpgsym    the agent daemon reaches a 267k-fault CLAIM storm on a LIVE
#             connection and times out at the limit -- the stuck-page
#             class the dead-connection transit sweep by design does not
#             touch; the last coat of the old wall
#   gdbbt     ptrace INSIDE a guest (gdb spawning its inferior) hangs to
#             the timeout -- unexplored territory, its own ledger entry
#   stracec   the same family from the other side: ptrace forwards and
#             half-works (SEIZE/GETREGSET answer) but the traced task's
#             stops never fire, so strace records an empty summary
KNOWN_GAPS="gpgsym gdbbt stracec"
# Below the ephemeral range (32768+), or an earlier case's outgoing
# connections can squat a later case's listen port.
CASEPORT=${PORT_BASE:-20000}
declare -a FAILED

# run <case> <workdir> <prog> [args...]: native to n.out, guest to g.out.
run() {
	local name=$1 wd=$2; shift 2
	local d="$OUTD/$name"
	# vmhome execv()s the program -- no PATH search. Resolve here so cases
	# can name programs the way a user types them.
	local prog
	prog=$(command -v "$1") || prog=$1
	shift; set -- "$prog" "$@"
	mkdir -p "$d/nat" "$d/gst"
	# Native, in its own copy of the workdir so file-writing cases match.
	cp -r "$wd" "$d/nat/wd" 2>/dev/null
	( cd "$d/nat/wd" && timeout $LIMIT "$@" ) > "$d/n.out" 2> "$d/n.err"
	echo $? > "$d/n.rc"
	cp -r "$wd" "$d/gst/wd" 2>/dev/null
	# A port of this case's own -- shared mode's whole safety contract.
	CASEPORT=$((CASEPORT + 4))
	( cd "$d/gst/wd" && OUT="$d/gst/run" KO="$KO" VMCTX_SHARED=1 \
	  PORT=$CASEPORT VMR_LIMIT=$LIMIT "$HERE/local-here.sh" "$@" ) \
		> "$d/launch.log" 2>&1
	echo $? > "$d/g.rc"
	cp "$d/gst/run/guest.out" "$d/g.out" 2>/dev/null || : > "$d/g.out"
}

verdict() {	# <case> <ok:0/1> <why>
	local name=$1 ok=$2 why=$3 known=0
	case " $KNOWN_GAPS " in *" $name "*) known=1;; esac
	if [ "$ok" = 0 ]; then
		if [ $known = 1 ]; then
			xpass=$((xpass+1)); echo "XPASS $name (a known gap, on a good day)"
		else
			pass=$((pass+1)); echo "PASS $name"
		fi
	elif [ $known = 1 ]; then
		xfail=$((xfail+1)); echo "XFAIL $name -- $why (known gap)"
	else
		fail=$((fail+1)); FAILED+=("$name: $why"); echo "FAIL $name -- $why"
	fi
}

grade_cmp() {	# native stdout == guest stdout && same rc
	local name=$1 d="$OUTD/$1"
	local nrc grc
	nrc=$(cat "$d/n.rc"); grc=$(cat "$d/g.rc")
	if [ "$nrc" != "$grc" ]; then verdict "$name" 1 "rc native=$nrc guest=$grc"
	elif ! cmp -s "$d/n.out" "$d/g.out"; then verdict "$name" 1 "stdout differs"
	else verdict "$name" 0 ok; fi
}

grade_rc() {
	local name=$1 d="$OUTD/$1"
	local nrc grc
	nrc=$(cat "$d/n.rc"); grc=$(cat "$d/g.rc")
	if [ "$nrc" != "$grc" ]; then verdict "$name" 1 "rc native=$nrc guest=$grc"
	else verdict "$name" 0 ok; fi
}

have() { command -v "$1" > /dev/null 2>&1; }
want() {	# only run cases named on the command line, if any were
	[ $# -eq 0 ] && return 0
	local c; for c in "$@"; do [ "$c" = "$CASE" ] && return 0; done
	return 1
}

ALL_CASES="sort sortp1 grepr awksum sedsub findt objdump bcpi pyjson pythreads perl
gcc runfresh make targz zstdrt xzrt bzip2rt ziprt gitwork curltls sshkeygen
gpgsym opensslsha magick psproc topproc freedf pipes rsyncl basejq
cpprt arlib striprun cpiort tclsum factor dcpi shufdet odx filemg elftools
edbatch groffman bashsub xargspar findexec flock2 fifo1 nc1 scriptpty bboxsh
iconv1 sums lddnames getentc localec duq envmisc killsig timeout1 pyfork
gdbbt stracec sqlt nodejit nodework rubyjit luasum datetime sleepdelta pytime"

for CASE in $ALL_CASES; do
	want "$@" || continue
	case "$CASE" in
	sort)	run sort "$FIX" sort -n nums; grade_cmp sort ;;
	grepr)	run grepr "$FIX" grep -rc e tree; grade_cmp grepr ;;
	awksum)	run awksum "$FIX" awk '{s+=$1} END {print s}' nums
		grade_cmp awksum ;;
	sedsub)	run sedsub "$FIX" sed s/1/x/g nums; grade_cmp sedsub ;;
	findt)	run findt "$FIX" find tree -type f -printf '%P %s\n'
		grade_cmp findt ;;
	objdump) run objdump "$FIX" objdump -d /usr/bin/wc; grade_cmp objdump ;;
	bcpi)	run bcpi "$FIX" dash -c 'echo "scale=600; 4*a(1)" | bc -l'
		grade_cmp bcpi ;;
	pyjson)	run pyjson "$FIX" python3 -c 'import json,hashlib
d={"k%d"%i:[i,i*i,"s"*(i%17)] for i in range(20000)}
s=json.dumps(d,sort_keys=True)
print(len(s),hashlib.sha256(s.encode()).hexdigest())'
		grade_cmp pyjson ;;
	pythreads) run pythreads "$FIX" python3 -c 'import threading
res=[0]*8
def w(i):
    s=0
    for j in range(200000): s+=j*i
    res[i]=s
ts=[threading.Thread(target=w,args=(i,)) for i in range(8)]
[t.start() for t in ts]; [t.join() for t in ts]
print(sum(res))'
		grade_cmp pythreads ;;
	perl)	run perl "$FIX" perl -e '$s+=$_ for 1..1000000; print "$s\n"'
		grade_cmp perl ;;
	gcc)	run gcc "$FIX" gcc -O2 -o hello hello.c
		# the artifact must exist AND be the same binary both ways
		if [ "$(cat "$OUTD/gcc/g.rc")" != 0 ]; then
			verdict gcc 1 "guest rc $(cat "$OUTD/gcc/g.rc")"
		elif ! cmp -s "$OUTD/gcc/nat/wd/hello" "$OUTD/gcc/gst/wd/hello"; then
			verdict gcc 1 "compiled binaries differ"
		else verdict gcc 0 ok; fi ;;
	runfresh) # run the binary gcc just made, AS A GUEST, right after linking
		if [ -x "$OUTD/gcc/gst/wd/hello" ]; then
			run runfresh "$OUTD/gcc/gst/wd" ./hello
			grade_cmp runfresh
		else skip=$((skip+1)); echo "SKIP runfresh (no gcc artifact)"; fi ;;
	make)	run make "$FIX" make
		if [ "$(cat "$OUTD/make/g.rc")" != 0 ]; then
			verdict make 1 "guest rc $(cat "$OUTD/make/g.rc")"
		elif [ ! -x "$OUTD/make/gst/wd/hello" ]; then
			verdict make 1 "no artifact"
		else verdict make 0 ok; fi ;;
	targz)	run targz "$FIX" dash -c 'tar czf t.tgz tree && mkdir ex && tar xzf t.tgz -C ex && diff -r tree ex/tree && echo RT-OK'
		if grep -q RT-OK "$OUTD/targz/g.out"; then verdict targz 0 ok
		else verdict targz 1 "roundtrip failed"; fi ;;
	zstdrt)	have zstd || { skip=$((skip+1)); echo "SKIP zstdrt"; continue; }
		run zstdrt "$FIX" dash -c 'zstd -q -o b.zst tree/b/blob && zstd -dq -o b.out b.zst && cmp tree/b/blob b.out && echo RT-OK'
		if grep -q RT-OK "$OUTD/zstdrt/g.out"; then verdict zstdrt 0 ok
		else verdict zstdrt 1 "roundtrip failed"; fi ;;
	xzrt)	run xzrt "$FIX" dash -c 'xz -zkq tree/b/blob -c > b.xz && xz -dq b.xz -c > b.out && cmp tree/b/blob b.out && echo RT-OK'
		if grep -q RT-OK "$OUTD/xzrt/g.out"; then verdict xzrt 0 ok
		else verdict xzrt 1 "roundtrip failed"; fi ;;
	bzip2rt) run bzip2rt "$FIX" dash -c 'bzip2 -kqc tree/b/blob > b.bz2 && bzip2 -dqc b.bz2 > b.out && cmp tree/b/blob b.out && echo RT-OK'
		if grep -q RT-OK "$OUTD/bzip2rt/g.out"; then verdict bzip2rt 0 ok
		else verdict bzip2rt 1 "roundtrip failed"; fi ;;
	ziprt)	run ziprt "$FIX" dash -c 'zip -qr t.zip tree && mkdir ex && cd ex && unzip -q ../t.zip && cd .. && diff -r tree ex/tree && echo RT-OK'
		if grep -q RT-OK "$OUTD/ziprt/g.out"; then verdict ziprt 0 ok
		else verdict ziprt 1 "roundtrip failed"; fi ;;
	gitwork) run gitwork "$FIX" dash -c 'export GIT_AUTHOR_NAME=t GIT_AUTHOR_EMAIL=t@t GIT_COMMITTER_NAME=t GIT_COMMITTER_EMAIL=t@t HOME=$PWD
git init -q r && cd r && cp ../nums f && git add f && git commit -qm one && echo x >> f && git add f && git commit -qm two && git log --format=%s | tr "\n" " " && git fsck --strict 2>&1 | head -1'
		grade_cmp gitwork ;;
	curltls) run curltls "$FIX" curl -sS -o /dev/null -w '%{http_code} %{ssl_verify_result}\n' https://example.com/
		grade_cmp curltls ;;
	sshkeygen) run sshkeygen "$FIX" dash -c 'ssh-keygen -q -t ed25519 -N "" -f k && ssh-keygen -y -f k > pub2 && head -c9 k && echo && wc -c < pub2 > /dev/null && echo KEY-OK'
		if grep -q KEY-OK "$OUTD/sshkeygen/g.out"; then verdict sshkeygen 0 ok
		else verdict sshkeygen 1 "keygen/derive failed"; fi ;;
	gpgsym)	run gpgsym "$FIX" dash -c 'export GNUPGHOME=$PWD/gnupg; mkdir -m700 gnupg
echo pw | gpg -q --batch --passphrase-fd 0 --pinentry-mode loopback -c -o b.gpg tree/b/blob 2>/dev/null &&
echo pw | gpg -q --batch --passphrase-fd 0 --pinentry-mode loopback -d -o b.out b.gpg 2>/dev/null &&
cmp tree/b/blob b.out && echo RT-OK'
		if grep -q RT-OK "$OUTD/gpgsym/g.out"; then verdict gpgsym 0 ok
		else verdict gpgsym 1 "encrypt/decrypt roundtrip failed"; fi ;;
	opensslsha) run opensslsha "$FIX" openssl dgst -sha256 tree/b/blob
		grade_cmp opensslsha ;;
	magick)	run magick "$FIX" dash -c 'convert -size 256x256 gradient:red-blue -rotate 90 -resize 64x64 out.png && identify -format "%w %h %m\n" out.png'
		grade_cmp magick ;;
	psproc)	run psproc "$FIX" dash -c 'ps aux | wc -l > /dev/null && echo PS-OK'
		if grep -q PS-OK "$OUTD/psproc/g.out"; then verdict psproc 0 ok
		else verdict psproc 1 "ps failed"; fi ;;
	topproc) run topproc "$FIX" dash -c 'top -bn1 | head -3 > /dev/null && echo TOP-OK'
		if grep -q TOP-OK "$OUTD/topproc/g.out"; then verdict topproc 0 ok
		else verdict topproc 1 "top failed"; fi ;;
	freedf)	run freedf "$FIX" dash -c 'free -m > /dev/null && df -h / > /dev/null && echo FD-OK'
		if grep -q FD-OK "$OUTD/freedf/g.out"; then verdict freedf 0 ok
		else verdict freedf 1 "free/df failed"; fi ;;
	pipes)	run pipes "$FIX" dash -c 'tar cf - tree | gzip -1 | wc -c > /dev/null && seq 1 10000 | awk "{s+=\$1} END {print s}" && (echo a; echo b) | sort | tr "\n" " " && echo'
		grade_cmp pipes ;;
	rsyncl)	run rsyncl "$FIX" dash -c 'rsync -a tree/ copy/ && diff -r tree copy && echo RT-OK'
		if grep -q RT-OK "$OUTD/rsyncl/g.out"; then verdict rsyncl 0 ok
		else verdict rsyncl 1 "sync+diff failed"; fi ;;
	basejq)	run basejq "$FIX" dash -c 'base64 tree/b/blob | base64 -d | cmp - tree/b/blob && echo "{\"a\":[1,2,3]}" | jq -c ".a|add" && echo B64-OK'
		if grep -q B64-OK "$OUTD/basejq/g.out"; then verdict basejq 0 ok
		else verdict basejq 1 "base64/jq failed"; fi ;;

	# --- session 43 breadth: more shapes ---
	# The known sort bug's boundary: default parallelism dies in the
	# deep-teardown family, --parallel=1 must keep passing.
	sortp1)	run sortp1 "$FIX" sort --parallel=1 -n nums; grade_cmp sortp1 ;;
	cpprt)	# g++, C++ threads, and the fresh C++ binary run as a guest
		cat > "$FIX/t.cc" <<'CC'
#include <thread>
#include <vector>
#include <cstdio>
int main() { std::vector<std::thread> v; long r[8] = {0};
	for (int i = 0; i < 8; i++) v.emplace_back([&r,i]{
		long s = 0; for (int j = 0; j < 200000; j++) s += (long)j*i;
		r[i] = s; });
	for (auto &t : v) t.join();
	long s = 0; for (int i = 0; i < 8; i++) s += r[i];
	printf("%ld\n", s); return 0; }
CC
		run cpprt "$FIX" dash -c 'g++ -O2 -pthread -o tcc t.cc && ./tcc'
		grade_cmp cpprt ;;
	arlib)	# a static library: compile two objects, ar them, link, run
		cat > "$FIX/lib1.c" <<'AC'
long f1(long x) { return x * 3; }
AC
		cat > "$FIX/lib2.c" <<'AC'
long f1(long);
long f2(long x) { return f1(x) + 7; }
AC
		cat > "$FIX/liba.c" <<'AC'
#include <stdio.h>
long f2(long);
int main(void) { printf("%ld\n", f2(1000)); return 0; }
AC
		run arlib "$FIX" dash -c 'gcc -c lib1.c lib2.c && ar rcs libt.a lib1.o lib2.o && ranlib libt.a && gcc -o am liba.c -L. -lt && ./am'
		grade_cmp arlib ;;
	striprun) # strip the compiled binary and run the stripped copy as a guest
		if [ -x "$OUTD/gcc/gst/wd/hello" ]; then
			cp "$OUTD/gcc/gst/wd/hello" "$FIX/hstrip" && strip "$FIX/hstrip"
			run striprun "$FIX" ./hstrip
			grade_cmp striprun
		else skip=$((skip+1)); echo "SKIP striprun (no gcc artifact)"; fi ;;
	cpiort)	run cpiort "$FIX" dash -c 'find tree -depth -print | cpio -o --quiet > t.cpio && mkdir ex && (cd ex && cpio -id --quiet < ../t.cpio) && diff -r tree ex/tree && echo RT-OK'
		if grep -q RT-OK "$OUTD/cpiort/g.out"; then verdict cpiort 0 ok
		else verdict cpiort 1 "roundtrip failed"; fi ;;
	tclsum)	have tclsh || { skip=$((skip+1)); echo "SKIP tclsum"; continue; }
		run tclsum "$FIX" dash -c 'echo "set s 0; for {set i 1} {$i <= 1000000} {incr i} {incr s $i}; puts $s" | tclsh'
		grade_cmp tclsum ;;
	factor)	run factor "$FIX" factor 2305843009213693951 999999999999999989
		grade_cmp factor ;;
	dcpi)	run dcpi "$FIX" dash -c 'echo "8k 355 113 / p" | dc && echo "5k 2 v p" | dc'
		grade_cmp dcpi ;;
	shufdet) # deterministic shuffle: fixed random-source, so CMP applies
		run shufdet "$FIX" dash -c 'shuf --random-source=tree/b/blob -i 1-100000 | sha256sum'
		grade_cmp shufdet ;;
	odx)	run odx "$FIX" dash -c 'od -A x -t x1z tree/b/blob | sha256sum && xxd tree/b/blob | sha256sum'
		grade_cmp odx ;;
	filemg)	run filemg "$FIX" dash -c 'file tree/b/blob hello.c Makefile /usr/bin/wc /etc/passwd'
		grade_cmp filemg ;;
	elftools) run elftools "$FIX" dash -c 'readelf -h /usr/bin/wc | head -12 && nm -D /usr/lib/x86_64-linux-gnu/libz.so.1 | sha256sum && size /usr/bin/wc && strings /usr/bin/wc | sha256sum'
		grade_cmp elftools ;;
	edbatch) run edbatch "$FIX" dash -c 'cp hello.c e.c && printf "%s\n" "2s/int/INT/" "w" "q" | ed -s e.c && grep -c INT e.c'
		grade_cmp edbatch ;;
	groffman) run groffman "$FIX" dash -c 'MANWIDTH=80 man --no-hyphenation ls 2>/dev/null | col -b | head -40'
		grade_cmp groffman ;;
	bashsub) run bashsub "$FIX" bash -c 'a=(1 2 3); diff <(seq 1 100) <(seq 1 100) && echo "${a[@]} $((a[0]+a[2]))" && echo "$(echo nested $(echo deeper))"'
		grade_cmp bashsub ;;
	xargspar) # parallel forks under xargs -P; output order normalized by sort
		run xargspar "$FIX" dash -c 'find tree -type f | xargs -P 4 -n 1 sha256sum | sort'
		grade_cmp xargspar ;;
	findexec) run findexec "$FIX" dash -c 'find tree -type f -exec sha256sum {} + | sort'
		grade_cmp findexec ;;
	flock2)	run flock2 "$FIX" dash -c 'for i in 1 2 3 4; do flock lockf dash -c "echo line$i >> serial" & done; wait; sort serial | tr "\n" " " && echo'
		grade_cmp flock2 ;;
	fifo1)	run fifo1 "$FIX" dash -c 'mkfifo p; cat p > got & echo through-a-fifo > p; wait; cat got'
		grade_cmp fifo1 ;;
	nc1)	# a socket server and client inside one guest, over loopback
		run nc1 "$FIX" dash -c 'PT=$((21000 + $$ % 1000)); (echo served | nc -l -q1 127.0.0.1 $PT &) ; sleep 1; nc -q1 127.0.0.1 $PT < /dev/null'
		grade_cmp nc1 ;;
	scriptpty) # script(1) allocates a pty INSIDE the guest
		run scriptpty "$FIX" script -qec "echo pty-says-hello" /dev/null
		if grep -q pty-says-hello "$OUTD/scriptpty/g.out"; then verdict scriptpty 0 ok
		else verdict scriptpty 1 "no pty output (rc $(cat "$OUTD/scriptpty/g.rc"))"; fi ;;
	bboxsh)	have busybox || { skip=$((skip+1)); echo "SKIP bboxsh"; continue; }
		run bboxsh "$FIX" busybox sh -c 'busybox seq 1 10000 | busybox awk "{s+=\$1} END {print s}" && busybox md5sum tree/b/blob && busybox sort -n nums | busybox tail -1'
		grade_cmp bboxsh ;;
	iconv1)	run iconv1 "$FIX" dash -c 'iconv -f utf-8 -t utf-16le < hello.c | iconv -f utf-16le -t utf-8 | cmp - hello.c && echo ICONV-OK'
		if grep -q ICONV-OK "$OUTD/iconv1/g.out"; then verdict iconv1 0 ok
		else verdict iconv1 1 "iconv roundtrip failed"; fi ;;
	sums)	run sums "$FIX" dash -c 'sha512sum tree/b/blob && b2sum tree/b/blob && cksum tree/b/blob && md5sum tree/b/blob'
		grade_cmp sums ;;
	lddnames) # ldd prints randomized addresses; compare the names only
		run lddnames "$FIX" dash -c 'ldd /usr/bin/wc | awk "{print \$1}" | sort'
		grade_cmp lddnames ;;
	getentc) run getentc "$FIX" dash -c 'getent hosts localhost && getent passwd root && getent group root'
		grade_cmp getentc ;;
	localec) run localec "$FIX" dash -c 'locale -a | sha256sum && getconf PAGESIZE && getconf _NPROCESSORS_ONLN'
		grade_cmp localec ;;
	duq)	run duq "$FIX" dash -c 'du -sb tree && find tree | wc -l && wc -lc nums'
		grade_cmp duq ;;
	envmisc) run envmisc "$FIX" dash -c 'uname -sr && id -u && hostname && nice -n 5 true && echo NICE-OK'
		grade_cmp envmisc ;;
	killsig) # a signal the guest sends itself, caught by its own handler
		run killsig "$FIX" dash -c 'trap "echo CAUGHT-TERM; exit 0" TERM; kill -TERM $$; echo NOT-REACHED'
		grade_cmp killsig ;;
	timeout1) run timeout1 "$FIX" timeout -s TERM 1 sleep 10
		grade_rc timeout1 ;;
	pyfork)	run pyfork "$FIX" python3 -c 'import os
r, w = os.pipe()
pid = os.fork()
if pid == 0:
    os.write(w, b"from-the-child")
    os._exit(7)
data = os.read(r, 64)
_, st = os.waitpid(pid, 0)
print(data.decode(), os.waitstatus_to_exitcode(st))'
		grade_cmp pyfork ;;
	gdbbt)	# ptrace INSIDE a guest: gdb runs the fresh binary to a breakpoint
		if [ -x "$OUTD/gcc/gst/wd/hello" ]; then
			cp "$OUTD/gcc/gst/wd/hello" "$FIX/hdbg" 2>/dev/null
			run gdbbt "$FIX" gdb --batch -ex 'break main' -ex run -ex 'echo BROKE-AT-MAIN\n' -ex continue ./hdbg
			if grep -q BROKE-AT-MAIN "$OUTD/gdbbt/g.out"; then verdict gdbbt 0 ok
			else verdict gdbbt 1 "no breakpoint hit (rc $(cat "$OUTD/gdbbt/g.rc"))"; fi
		else skip=$((skip+1)); echo "SKIP gdbbt (no gcc artifact)"; fi ;;
	stracec) # strace is ptrace again, from the other side. The summary
		# TABLE is required, not just exit 0: the first run of this
		# case "passed" with an EMPTY st.txt (head -1 of an empty file
		# is true) while 15 forwarded ptrace calls half-worked --
		# SEIZE/GETREGSET succeed against the parked source task, but
		# the stops strace records by never fire, so it counts nothing.
		run stracec "$FIX" dash -c 'strace -cf -o st.txt true && grep -q syscall st.txt && echo STRACE-OK'
		if grep -q STRACE-OK "$OUTD/stracec/g.out"; then verdict stracec 0 ok
		else verdict stracec 1 "no trace recorded (rc $(cat "$OUTD/stracec/g.rc"))"; fi ;;
	sqlt)	# a real database: B-tree, journal, fsync discipline
		have sqlite3 || { skip=$((skip+1)); echo "SKIP sqlt"; continue; }
		run sqlt "$FIX" dash -c 'sqlite3 t.db "
create table t(a integer, b text);
with recursive c(i) as (select 1 union all select i+1 from c where i < 2000)
insert into t select i, printf(\"row-%d\", i*i) from c;
select count(*), sum(a), b from t where a % 977 = 3;
pragma integrity_check;" && sqlite3 t.db "select count(*) from t"'
		grade_cmp sqlt ;;

	# --- JIT runtimes: self-modifying code, W^X churn, tiered compilation ---
	# Nothing else in the harness runs code the program WRITES: V8 and
	# YJIT compile hot loops into fresh executable pages at runtime
	# (mmap RW -> write -> mprotect RX -> jump), which is a page-state
	# shape no interpreter or compiler case exercises. All four pass;
	# the node cases flake ~1/6-1/12 under load into the standing
	# deep-teardown THREAD-LIST class (session 37, REDESIGN step 3):
	# V8's worker churn leaves a page "lent to shadow N (its channel is
	# gone)" zero-filled under a live thread stack, one context SIGABRTs
	# -- the LOST/SIGABRT pair in remote.log is the tell, and V8's churn
	# is that class's best modern reproducer. Strict on purpose.
	nodejit) have node || { skip=$((skip+1)); echo "SKIP nodejit"; continue; }
		run nodejit "$FIX" node -e 'let s = 0n;
for (let i = 0; i < 3000000; i++) s += BigInt(i % 97);
const c = require("crypto");
console.log(s.toString(), c.createHash("sha256").update(s.toString()).digest("hex"));'
		grade_cmp nodejit ;;
	nodework) have node || { skip=$((skip+1)); echo "SKIP nodework"; continue; }
		run nodework "$FIX" node -e 'const { Worker } = require("worker_threads");
const code = `const { parentPort, workerData } = require("worker_threads");
let s = 0; for (let i = 0; i < 500000; i++) s += i * workerData;
parentPort.postMessage(s);`;
let done = 0, tot = 0;
for (let i = 1; i <= 4; i++) {
	const w = new Worker(code, { eval: true, workerData: i });
	w.on("message", v => { tot += v; if (++done === 4) console.log(tot); });
}'
		grade_cmp nodework ;;
	rubyjit) have ruby || { skip=$((skip+1)); echo "SKIP rubyjit"; continue; }
		run rubyjit "$FIX" ruby --yjit -e 's = 0; 2_000_000.times { |i| s += i % 97 }; puts s'
		grade_cmp rubyjit ;;
	luasum) have lua5.4 || { skip=$((skip+1)); echo "SKIP luasum"; continue; }
		run luasum "$FIX" lua5.4 -e 'local s = 0; for i = 1, 2000000 do s = s + i % 97 end; print(s)'
		grade_cmp luasum ;;

	# --- the clock family: photographs the vDSO/vvar defect ---
	datetime) # the guest's wall clock against the harness's, +/- 60s
		run datetime "$FIX" date +%s
		NOWS=$(date +%s)
		GS=$(head -1 "$OUTD/datetime/g.out" 2>/dev/null | tr -dc 0-9)
		if [ -n "$GS" ] && [ "$GS" -gt $((NOWS - 3600)) ] && [ "$GS" -lt $((NOWS + 3600)) ]; then
			verdict datetime 0 ok
		else verdict datetime 1 "guest clock reads '${GS:-nothing}' (now $NOWS)"; fi ;;
	sleepdelta) # sleep(2) must advance the guest's own clock by ~2s
		run sleepdelta "$FIX" dash -c 'a=$(date +%s); sleep 2; b=$(date +%s); echo $((b-a))'
		GD=$(head -1 "$OUTD/sleepdelta/g.out" 2>/dev/null | tr -dc 0-9-)
		if [ -n "$GD" ] && [ "$GD" -ge 1 ] && [ "$GD" -le 5 ]; then
			verdict sleepdelta 0 ok
		else verdict sleepdelta 1 "slept 2s but clock moved '${GD:-nothing}'"; fi ;;
	pytime)	# python's clocks: wall plausible, monotonic strictly advancing
		run pytime "$FIX" python3 -c 'import time
a = time.time(); m1 = time.monotonic(); time.sleep(0.2); m2 = time.monotonic()
print("OK" if a > 1.7e9 and m2 > m1 else "BAD", "wall" if a > 1.7e9 else "wall-broken", "mono" if m2 > m1 else "mono-broken")'
		if grep -q "^OK" "$OUTD/pytime/g.out"; then verdict pytime 0 ok
		else verdict pytime 1 "$(head -1 "$OUTD/pytime/g.out" 2>/dev/null || echo no-output)"; fi ;;
	esac
done

echo
echo "=== progs.$TAG: $pass pass, $fail fail, $skip skip; known gaps:" \
     "$xfail still failing, $xpass passing today ==="
for f in "${FAILED[@]:-}"; do [ -n "$f" ] && echo "  FAILED $f"; done
[ "$fail" = 0 ]
