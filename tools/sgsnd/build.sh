#!/bin/sh
# Build sgsnd.dll (32-bit) with mingw-w64.
set -e
cd "$(dirname "$0")"
i686-w64-mingw32-gcc -O2 -Wall -shared -o sgsnd.dll sgsnd.c sgsnd.def \
    -lole32 -luuid -ldxguid -static-libgcc -Wl,--enable-stdcall-fixup
