#!/bin/sh
# Debug run: record every DirectSound buffer the game writes, via the sgsnd
# proxy (tools/sgsnd; sound.dll's ole32.dll import must be renamed to sgsnd.dll),
# and log every file opened through mmioOpen with Wine timestamps.
# Output in /tmp/simgolf-audio: events.log, buf<N>.raw (PCM, format in the log), mmio.log.
rm -rf /tmp/simgolf-audio
mkdir -p /tmp/simgolf-audio
here=$(cd "$(dirname "$0")" && pwd)
SGSND_DUMP='Z:\tmp\simgolf-audio' WINEDEBUG=+timestamp,trace+mmio "$here/../run.sh" "$@" 2>&1 |
    grep --line-buffered -E 'mmioOpen|MMIO_Open' > /tmp/simgolf-audio/mmio.log
