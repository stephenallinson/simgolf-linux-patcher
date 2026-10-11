#!/bin/sh
# Patch your own copy of SimGolf's Terrain.dll to use the terrgl32 OpenGL shim,
# and install terrgl32.dll next to it. Run with --revert to undo.
#
# Usage: ./patch-terrain.sh [--revert] [game folder]
# The game folder defaults to the standard install location in $WINEPREFIX
# (or ~/.wine-simgolf if WINEPREFIX isn't set).
#
# Changes, both in Terrain.dll from patch 1.03:
#   0x63e19  PIXELFORMATDESCRIPTOR cColorBits 16 -> 32 (Wine has no 16-bit bitmap formats)
#   0x699d2  import name "OPENGL32.dll" -> "TERRGL32.dll" (load the shim instead)
set -e

ORIG_SHA256=378e61d2f2061f85e517eade8b64e92ecde00d30ab866c091d55cb737adce0a4
PFD_OFFSET=$((0x63e10))
COLOR_OFFSET=$((0x63e19))
IMPORT_OFFSET=$((0x699d2))

die() { echo "error: $*" >&2; exit 1; }
hex() { od -A n -t x1 -j "$2" -N "$3" "$1" | tr -d ' \n'; }

revert=0
if [ "$1" = "--revert" ]; then revert=1; shift; fi

here=$(cd "$(dirname "$0")" && pwd)
prefix=${WINEPREFIX:-$HOME/.wine-simgolf}
game=${1:-"$prefix/drive_c/Program Files (x86)/EA Games/Firaxis Games/Sid Meiers SimGolf"}
[ -d "$game" ] || game="$prefix/drive_c/Program Files/EA Games/Firaxis Games/Sid Meiers SimGolf"
[ -d "$game" ] || die "game folder not found; pass it as an argument"

terrain=$(find "$game" -maxdepth 1 -iname terrain.dll | head -n 1)
[ -n "$terrain" ] || die "Terrain.dll not found in $game"
backup="$terrain.orig"

if [ $revert = 1 ]; then
    [ -f "$backup" ] || die "no backup at $backup, nothing to revert"
    cp -p "$backup" "$terrain"
    rm -f "$game/terrgl32.dll" "$game/TERRGL32.dll"
    echo "Reverted $terrain from backup and removed terrgl32.dll."
    exit 0
fi

# Find the shim: next to this script in a release, or built in shim/.
shim="$here/terrgl32.dll"
[ -f "$shim" ] || shim="$here/shim/terrgl32.dll"
[ -f "$shim" ] || die "terrgl32.dll not found (build it with shim/build.sh)"

# Check we're looking at the right structure before touching anything.
pfd=$(hex "$terrain" $PFD_OFFSET 8)
color=$(hex "$terrain" $COLOR_OFFSET 1)
import=$(dd if="$terrain" bs=1 skip=$IMPORT_OFFSET count=12 2>/dev/null)
[ "$pfd" = "2800010028000000" ] || die "unexpected bytes at 0x63e10 ($pfd); this Terrain.dll isn't the 1.03 version this patch knows"

if [ "$color" = "20" ] && [ "$import" = "TERRGL32.dll" ]; then
    echo "Terrain.dll is already patched."
elif [ "$color" = "10" ] && [ "$import" = "OPENGL32.dll" ]; then
    sum=$(sha256sum "$terrain" | cut -d' ' -f1)
    [ "$sum" = "$ORIG_SHA256" ] || echo "warning: Terrain.dll checksum differs from the tested 1.03 file; the patched bytes match, continuing"
    [ -f "$backup" ] || cp -p "$terrain" "$backup"
    printf '\040' | dd of="$terrain" bs=1 seek=$COLOR_OFFSET conv=notrunc status=none
    printf 'TERRGL32' | dd of="$terrain" bs=1 seek=$IMPORT_OFFSET conv=notrunc status=none
    [ "$(hex "$terrain" $COLOR_OFFSET 1)" = "20" ] || die "verification failed at 0x63e19"
    [ "$(dd if="$terrain" bs=1 skip=$IMPORT_OFFSET count=12 2>/dev/null)" = "TERRGL32.dll" ] || die "verification failed at 0x699d2"
    echo "Patched $terrain (backup: $backup)."
else
    die "Terrain.dll is in an unknown state (cColorBits byte $color, import '$import'); restore the original and try again"
fi

cp "$shim" "$game/terrgl32.dll"
echo "Installed terrgl32.dll into $game."
