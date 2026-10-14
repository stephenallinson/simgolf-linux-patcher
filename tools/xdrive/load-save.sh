#!/bin/sh
# Start SimGolf on a private Xvfb display at WxH, skip the intro and load the
# last save in the list, for testing the resolution hooks without a monitor.
#
#   tools/xdrive/load-save.sh 1280x720 [display]   (default display :77)
#   DISPLAY=:77 tools/xdrive/xdrive click X Y; DISPLAY=:77 import -window root shot.png
#   tools/xdrive/load-save.sh stop                 stop the game, restore saves
#
# The game autosaves while it runs, so the save folder (and top10.sve) is
# backed up before starting and restored by "stop" (or the next start).
res=${1:-1280x720}
disp=${2:-:77}
w=${res%x*}
h=${res#*x}
here=$(cd "$(dirname "$0")" && pwd)
x="$here/xdrive"
export WINEPREFIX="${WINEPREFIX:-$HOME/.wine-simgolf}"
dir=${SIMGOLF_DIR:-"$WINEPREFIX/drive_c/Program Files (x86)/EA Games/Firaxis Games/Sid Meiers SimGolf"}

backup="${XDG_CACHE_HOME:-$HOME/.cache}/simgolf-xdrive-saves"

restore() {
    [ -f "$backup/complete" ] || { rm -rf "$backup"; return 0; }
    rm -rf "$dir/Saved Games"
    cp -a "$backup/Saved Games" "$dir/Saved Games"
    [ -f "$backup/top10.sve" ] && cp -a "$backup/top10.sve" "$dir/top10.sve"
    rm -rf "$backup"
    echo "restored saved games"
}

wineserver -k 2>/dev/null
sleep 2
pkill -f "^Xvfb :7[0-9] " 2>/dev/null
restore
[ "$1" = stop ] && exit 0

[ -x "$x" ] || "$here/build.sh" || exit 1
mkdir -p "$backup"
cp -a "$dir/Saved Games" "$backup/Saved Games" || { echo "error: can't back up saved games" >&2; exit 1; }
if [ -f "$dir/top10.sve" ]; then cp -a "$dir/top10.sve" "$backup/top10.sve" || exit 1; fi
touch "$backup/complete"
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
