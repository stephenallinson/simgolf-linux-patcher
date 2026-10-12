#!/bin/sh
# Start SimGolf on a private Xvfb display at WxH, skip the intro and load the
# last save in the list, for testing the resolution hooks without a monitor.
#
#   tools/xdrive/load-save.sh 1280x720 [display]   (default display :77)
#   DISPLAY=:77 tools/xdrive/xdrive click X Y; DISPLAY=:77 import -window root shot.png
#
# The game runs until `WINEPREFIX=~/.wine-simgolf wineserver -k`.
res=${1:-1280x720}
disp=${2:-:77}
w=${res%x*}
h=${res#*x}
here=$(cd "$(dirname "$0")" && pwd)
x="$here/xdrive"
export WINEPREFIX="${WINEPREFIX:-$HOME/.wine-simgolf}"
dir=${SIMGOLF_DIR:-"$WINEPREFIX/drive_c/Program Files (x86)/EA Games/Firaxis Games/Sid Meiers SimGolf"}

[ -x "$x" ] || "$here/build.sh" || exit 1
wineserver -k 2>/dev/null
sleep 2
pkill -f "^Xvfb $disp " 2>/dev/null
Xvfb "$disp" -screen 0 "${w}x${h}x24" -nolisten tcp >/dev/null 2>&1 &
sleep 2
(cd "$dir" && env -u WAYLAND_DISPLAY DISPLAY="$disp" SIMGOLF_RES="$res" setsid wine golf.exe >/dev/null 2>&1 &)
sleep 20
export DISPLAY="$disp"
# the menus are 800x600, centred
ox=$(( (w - 800) / 2 ))
oy=$(( (h - 600) / 2 ))
"$x" click 400 300; sleep 1
"$x" key Escape; sleep 3
"$x" key Escape; sleep 4
"$x" click $((ox + 180)) $((oy + 82)); sleep 4      # Continue Saved Game
"$x" click $((ox + 420)) $((oy + 233)); sleep 2     # 8th save in the list
"$x" click $((ox + 630)) $((oy + 555)); sleep 25    # load
"$x" move $((w / 2)) $((h / 2 + 40))
echo "loaded at $res on $disp"
