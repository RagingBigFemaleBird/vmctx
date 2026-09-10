# demo/ -- the browser demo launchers (session 47)

Untracked scratch scripts that ran the session-47 demos, kept here so the
next session does not rebuild them. None of them ever kills another run's
vmhome or vmremote: `xprep.sh` kills by port, `lbrowser.sh` uses shared mode
with an explicit PORT. `browser-run.sh`/`browser-demo.sh` (one level up) do a
global `pkill -x vmhome` and must not run beside a demo.

| file | what |
|---|---|
| `xbrowser.sh` | cross-machine: vmhome on .229 (display :0 = the logged-in Xwayland, or :99 = Xvfb + x11vnc), vmremote on .30 |
| `xprep.sh` | its source-side prep, run as root on .229 (port-scoped kill, Xvfb/x11vnc/bus/http server) |
| `lbrowser.sh` | loopback (both halves on .229) on Xvfb :98, shared mode; `lbrowser0.sh` the same with `DISP`/`XAUTH` overrides for the real display |
| `noshm.c` | an LD_PRELOAD that hides MIT-SHM from an X client (`gcc -shared -fPIC -o libnoshm.so noshm.c`) -- tried against the display-:0 deaths, not the cause |

Environment that mattered: `OPENSSL_ia32cap=:~0x20000000` (cross-machine,
needed only with VMHOME_NATIVE_LOADER=1 -- with the loader in the guest, the
default since AUDIT #38, netsurf renders cross-machine without it), `CANBERRA_DRIVER=null GTK_IM_MODULE=gtk-im-context-simple
XMODIFIERS=@im=none NO_AT_BRIDGE=1` (a GNOME session's GTK modules; netsurf
still dies on the real display, PulseAudio's shared memory is the suspect),
firefox: `MOZ_DISABLE_*_SANDBOX=1 MOZ_ENABLE_WAYLAND=0 --no-remote
--new-instance --profile <fresh dir>`. A netsurf a person can click: run it
on :98 with `lbrowser.sh`, `x11vnc -display :98 -noshm -rfbport 5901`, and
`xtigervncviewer localhost:5901` on the logged-in display.
