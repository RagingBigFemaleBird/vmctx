#!/bin/bash
# wedge.sh — who is waiting for whom, when a run stops making progress.
#
# The interesting half is not that a shadow is blocked — a shadow blocked in
# poll() is a guest waiting for input, which is normal — but *what it is
# waiting on*. A descriptor number means nothing on its own, so every blocked
# thread is reported with what /proc says its descriptor actually is.
set -u
say() { echo "=== $* ==="; }

say "vmremote-local threads by wchan"
for p in $(pgrep -x vmremote-local); do
	for t in /proc/$p/task/*; do
		echo "$(cat "$t/wchan" 2>/dev/null || echo running)"
	done
done | sort | uniq -c | sort -rn

say "shadows: $(pgrep -xc vmhome) processes"
for p in $(pgrep -x vmhome); do
	for t in /proc/$p/task/*; do
		tid=${t##*/}
		w=$(cat "$t/wchan" 2>/dev/null)
		case "$w" in
		*poll*|*pipe*|*epoll*|*sock*|*unix*|*futex*) ;;
		*) continue ;;
		esac
		# /proc/<tid>/syscall: nr arg0 arg1 ... — arg0 is the fd for
		# read/poll/epoll_wait/recvmsg, which is what we want named.
		read -r nr a0 _ < <(tr -d '\n' < "$t/syscall" 2>/dev/null)
		fd=$((a0))
		tgt=$(readlink "/proc/$p/fd/$fd" 2>/dev/null)
		printf '%s/%s %-28s syscall=%s fd=%s -> %s\n' \
			"$p" "$tid" "$w" "$nr" "$fd" "${tgt:-?}"
	done
done

say "what the guest exec'd (each is a new program)"
dmesg | grep -a 'guest entry' | tail -10

say "contexts still alive"
grep -a 'forked context' /tmp/localrun/remote.log 2>/dev/null | tail -6
