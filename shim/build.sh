#!/bin/sh
# Build terrgl32.dll (32-bit) with mingw-w64.
set -e
cd "$(dirname "$0")"
i686-w64-mingw32-gcc -O2 -Wall -shared -o terrgl32.dll terrgl32.c terrgl32.def \
    -lopengl32 -lgdi32 -static-libgcc -Wl,--enable-stdcall-fixup
