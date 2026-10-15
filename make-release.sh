#!/bin/sh
# Build terrgl32.dll and sghooks.dll and package a release: dist/simgolf-linux-<version>.tar.gz and .zip.
# Contains only this project's files; no game files.
#
# Usage: ./make-release.sh [--test]
#   --test   also run shim/test/shimtest.exe under Wine (needs a working Wine with OpenGL)
set -e
cd "$(dirname "$0")"

version=$(sed -n 's/^#define TERRGL32_VERSION "\(.*\)"/\1/p' shim/terrgl32.c)
[ -n "$version" ] || { echo "error: no TERRGL32_VERSION in shim/terrgl32.c" >&2; exit 1; }
name=simgolf-linux-$version

shim/build.sh
hooks/build.sh
if [ "$1" = "--test" ]; then
    i686-w64-mingw32-gcc -O1 -Wall -o shim/test/shimtest.exe shim/test/shimtest.c -lopengl32 -lgdi32
    WINEDEBUG=-all wine shim/test/shimtest.exe "$(winepath -w shim/terrgl32.dll)"
fi

rm -rf "dist/$name"
mkdir -p "dist/$name/shim/test" "dist/$name/hooks" "dist/$name/tools"
cp README.md CHANGELOG.md LICENSE patch-terrain.sh run.sh "dist/$name/"
cp shim/terrgl32.dll hooks/sghooks.dll hooks/settings.ini "dist/$name/"
cp shim/terrgl32.c shim/terrgl32.def shim/build.sh "dist/$name/shim/"
cp shim/test/shimtest.c "dist/$name/shim/test/"
cp hooks/sghooks.c hooks/build.sh "dist/$name/hooks/"
cp tools/trace-run.sh "dist/$name/tools/"

cd dist
tar czf "$name.tar.gz" "$name"
rm -f "$name.zip"
if command -v zip >/dev/null 2>&1; then zip -qr "$name.zip" "$name"; fi
rm -f "$name.sha256"
sha256sum "$name".tar.gz $( [ -f "$name.zip" ] && echo "$name.zip") > "$name.sha256"
echo "Built dist/$name.tar.gz$( [ -f "$name.zip" ] && echo " and .zip")"
cat "$name.sha256"
