#!/bin/bash
# Walk the monitor's whole descendant tree by pid, not by name: a task inside
# execve has already taken the new program's comm, so anything that greps for
# the monitor's name is blind to exactly the case worth seeing.
set -u
root=$(pgrep -x vmremote-local | head -1)
[ -z "${root:-}" ] && { echo "no vmremote"; exit 0; }
todo="$root"; seen=""
while [ -n "$todo" ]; do
	p=${todo%% *}; rest=${todo#"$p"}; todo=${rest# }
	case " $seen " in *" $p "*) continue ;; esac
	seen="$seen $p"
	for t in /proc/$p/task/*; do
		[ -e "$t" ] || continue
		tid=${t##*/}
		comm=$(cat "$t/comm" 2>/dev/null)
		st=$(awk '{print $3}' "$t/stat" 2>/dev/null)
		w=$(cat "$t/wchan" 2>/dev/null)
		nr=$(cut -d' ' -f1 "$t/syscall" 2>/dev/null)
		printf '%s/%s %-16s %s %-26s nr=%s\n' "$p" "$tid" "$comm" "$st" "${w:-running}" "$nr"
		[ "$nr" = "59" ] || [ "$nr" = "322" ] || [ "$st" = "D" ] && {
			echo "    --- kernel stack ---"
			sed 's/^/    /' "$t/stack" 2>/dev/null | head -12
		}
		c=$(cat "$t/children" 2>/dev/null)
		todo="$todo $c"
	done
done
echo "=== shadow tree at the same instant ==="
for p in $(pgrep -x vmhome); do
  st=$(awk '{print $3}' /proc/$p/stat 2>/dev/null)
  pp=$(awk '{print $4}' /proc/$p/stat 2>/dev/null)
  echo "shadow $p (parent $pp) state=$st"
  for t in /proc/$p/task/*; do
     tid=${t##*/}
     w=$(cat "$t/wchan" 2>/dev/null); nr=$(cut -d' ' -f1 "$t/syscall" 2>/dev/null)
     printf '    %s %-28s nr=%s\n' "$tid" "${w:-running}" "$nr"
  done
done
