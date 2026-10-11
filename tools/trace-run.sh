#!/bin/sh
# Debug run: Wine relay trace of GDI calls interleaved with the terrgl32 shim's
# log, written to ~/simgolf-trace.log. Slow; load a map, do one thing, quit.
#
# The relay filter lives in the prefix registry. Set it once with:
#   wine reg add 'HKCU\Software\Wine\Debug' /v RelayInclude /t REG_SZ /f \
#     /d 'gdi32.BitBlt;gdi32.StretchBlt;gdi32.SelectObject;gdi32.CreateCompatibleDC;gdi32.CreateDIBSection;gdi32.DeleteDC;gdi32.TextOutA'
here=$(cd "$(dirname "$0")" && pwd)
TERRGL32_LOG=debug WINEDEBUG=+relay,+debugstr "$here/../run.sh" "$@" 2>&1 |
    grep -E 'gdi32\.|terrgl32 ' | grep -vE 'load_list|PE DLL' > "$HOME/simgolf-trace.log"
