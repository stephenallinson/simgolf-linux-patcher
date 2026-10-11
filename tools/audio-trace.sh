#!/bin/sh
# Debug run: Wine's DirectSound trace, written to ~/simgolf-dsound.log.
# Load a map, place one building, wait a few seconds while it loops, then quit.
here=$(cd "$(dirname "$0")" && pwd)
WINEDEBUG=+dsound,+timestamp "$here/../run.sh" "$@" > "$HOME/simgolf-dsound.log" 2>&1
