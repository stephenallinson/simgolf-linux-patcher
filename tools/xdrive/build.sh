#!/bin/sh
# Build xdrive (native, needs libX11 and libXtst).
cd "$(dirname "$0")" && cc -O2 -Wall -o xdrive xdrive.c -lX11 -lXtst
