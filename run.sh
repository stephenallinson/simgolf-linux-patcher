#!/bin/sh
# Launch SimGolf. Works from any directory (golf.exe must be started from its own folder).
#
#   ./run.sh                  gamescope, 800x600 at the largest whole-number scale (if gamescope is installed)
#   ./run.sh --no-gamescope   plain Wine virtual desktop
#
# Environment:
#   WINEPREFIX     prefix with SimGolf installed (default ~/.wine-simgolf)
#   SIMGOLF_DIR    game folder, if not the default install location
#   TERRGL32_LOG   Windows path for the GL shim's debug log, e.g. 'Z:\tmp\terrgl32.log'
#   SIMGOLF_RES    screen size, e.g. 1024x768 (experimental; needs hooks/sghooks.dll installed)
#   GAMESCOPE_ARGS gamescope scaling options (default "-S integer -F nearest": crisp, square pixels;
#                  "-F pixel" or "-F fsr" fill the screen height instead)
export WINEPREFIX="${WINEPREFIX:-$HOME/.wine-simgolf}"
dir=${SIMGOLF_DIR:-"$WINEPREFIX/drive_c/Program Files (x86)/EA Games/Firaxis Games/Sid Meiers SimGolf"}
[ -d "$dir" ] || dir="$WINEPREFIX/drive_c/Program Files/EA Games/Firaxis Games/Sid Meiers SimGolf"
cd "$dir" || { echo "error: game folder not found; set SIMGOLF_DIR" >&2; exit 1; }

res=${SIMGOLF_RES:-800x600}
w=${res%x*}
h=${res#*x}

if [ "$1" = "--no-gamescope" ] || ! command -v gamescope >/dev/null 2>&1; then
    exec wine explorer /desktop=simgolf,"$res" golf.exe
fi
# gamescope already gives the game its own display at that size, so skip Wine's
# virtual desktop: it stays open after golf.exe exits, and gamescope waits for
# it (and Wine's background services) forever. `wine golf.exe` returns when
# the game exits; then stop the prefix's leftover processes so gamescope closes.
# winedevice.exe can hang on shutdown and survive wineserver -k, so kill any
# left in this prefix.
# shellcheck disable=SC2086  # GAMESCOPE_ARGS is meant to split into options
exec gamescope -w "$w" -h "$h" -f ${GAMESCOPE_ARGS:--S integer -F nearest} -- sh -c '
    wine golf.exe
    wineserver -k
    sleep 1
    for pid in $(pgrep -f "^[A-Za-z]:.*winedevice\\.exe"); do
        tr "\0" "\n" < /proc/$pid/environ 2>/dev/null | grep -qx "WINEPREFIX=$WINEPREFIX" && kill -9 $pid
    done'
