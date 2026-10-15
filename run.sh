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
#   SIMGOLF_RES    screen size, e.g. 1280x720 (overrides Resolution= in the game folder's
#                  settings.ini; needs sghooks.dll installed)
#   settings.ini   [Display] Resolution and LockMouse=1 (keep the mouse in the window)
#   SIMGOLF_OUTPUT your monitor's size, e.g. 2560x1440, if run.sh guesses it wrong
#                  (default: the primary monitor's current mode, from xrandr)
#   GAMESCOPE_ARGS gamescope scaling options (default "-S integer -F nearest": crisp, square pixels;
#                  "-F pixel" or "-F fsr" fill the screen height instead)
export WINEPREFIX="${WINEPREFIX:-$HOME/.wine-simgolf}"
dir=${SIMGOLF_DIR:-"$WINEPREFIX/drive_c/Program Files (x86)/EA Games/Firaxis Games/Sid Meiers SimGolf"}
[ -d "$dir" ] || dir="$WINEPREFIX/drive_c/Program Files/EA Games/Firaxis Games/Sid Meiers SimGolf"
cd "$dir" || {
  echo "error: game folder not found; set SIMGOLF_DIR" >&2
  exit 1
}

# ini SECTION KEY: a value from the game folder's settings.ini (case-insensitive
# names, comments after ';' dropped)
ini() {
    [ -f settings.ini ] || return 0
    tr -d '\r' < settings.ini | awk -v sec="$1" -v key="$2" '
        /^[[:space:]]*\[/ { s = tolower($0); gsub(/[][[:space:]]/, "", s); insec = (s == tolower(sec)); next }
        insec { line = $0; sub(/;.*/, "", line); n = index(line, "=")
                if (n && tolower(substr(line, 1, n - 1)) ~ "^[[:space:]]*" tolower(key) "[[:space:]]*$") {
                    v = substr(line, n + 1); gsub(/^[[:space:]]+|[[:space:]]+$/, "", v); print v; exit } }'
}

# the screen size: SIMGOLF_RES, else [Display] Resolution in settings.ini
res=${SIMGOLF_RES:-$(ini Display Resolution)}
res=${res:-800x600}
# [Display] LockMouse=1: keep the mouse inside the game window (gamescope)
case $(ini Display LockMouse) in 1|[Yy]es|[Tt]rue|[Oo]n) lock="--force-grab-cursor" ;; *) lock= ;; esac
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
# gamescope's output size: SIMGOLF_OUTPUT, else the (primary) monitor's
# current mode. Nested in another Wayland compositor, gamescope otherwise
# keeps drawing at the game's size, and the compositor centres that small
# picture with black around it instead of scaling it up.
out=${SIMGOLF_OUTPUT:-}
if [ -z "$out" ] && command -v xrandr >/dev/null 2>&1; then
  out=$(xrandr --current 2>/dev/null | awk '/ connected primary/ {p = 1} p && /\*/ {print $1; exit}')
  [ -n "$out" ] || out=$(xrandr --current 2>/dev/null | awk '/\*/ {print $1; exit}')
fi
case $out in
[0-9]*x[0-9]*) output="-W ${out%x*} -H ${out#*x}" ;;
*) output= ;;
esac
# shellcheck disable=SC2086  # $output and GAMESCOPE_ARGS are meant to split into options
exec gamescope -w "$w" -h "$h" $output -f ${GAMESCOPE_ARGS:--S integer -F nearest} $lock -- sh -c '
    wine golf.exe
    wineserver -k
    sleep 1
    for pid in $(pgrep -f "^[A-Za-z]:.*winedevice\\.exe"); do
        tr "\0" "\n" < /proc/$pid/environ 2>/dev/null | grep -qx "WINEPREFIX=$WINEPREFIX" && kill -9 $pid
    done'
