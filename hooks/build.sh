#!/bin/sh
# Build sghooks.dll (32-bit) with mingw-w64.
set -e
cd "$(dirname "$0")"
i686-w64-mingw32-gcc -O2 -Wall -shared -o sghooks.dll sghooks.c -static-libgcc -lwinmm
