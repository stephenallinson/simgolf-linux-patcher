#!/bin/sh
# Launch SimGolf. Works from any directory (golf.exe must be started from its own folder).
#
#   ./run.sh                  gamescope, 800x600 scaled to fullscreen (if gamescope is installed)
#   ./run.sh --no-gamescope   plain Wine virtual desktop
#
# Environment:
#   WINEPREFIX     prefix with SimGolf installed (default ~/.wine-simgolf)
#   SIMGOLF_DIR    game folder, if not the default install location
#   TERRGL32_LOG   Windows path for the GL shim's debug log, e.g. 'Z:\tmp\terrgl32.log'
export WINEPREFIX="${WINEPREFIX:-$HOME/.wine-simgolf}"
dir=${SIMGOLF_DIR:-"$WINEPREFIX/drive_c/Program Files (x86)/EA Games/Firaxis Games/Sid Meiers SimGolf"}
[ -d "$dir" ] || dir="$WINEPREFIX/drive_c/Program Files/EA Games/Firaxis Games/Sid Meiers SimGolf"
cd "$dir" || { echo "error: game folder not found; set SIMGOLF_DIR" >&2; exit 1; }

if [ "$1" = "--no-gamescope" ] || ! command -v gamescope >/dev/null 2>&1; then
    exec wine explorer /desktop=simgolf,800x600 golf.exe
fi
exec gamescope -w 800 -h 600 -f -- wine explorer /desktop=simgolf,800x600 golf.exe
