#!/bin/sh
# Try to trigger the looping-sound bug and record it (video + game audio).
#
#   tools/xdrive/sound-test.sh NAME [fix|nofix] [WxH] [X Y] [SECONDS]
#
# Loads the 8th save on a private Xvfb display with the game's audio sent to
# a temporary silent sink, then left-clicks one spot of the course (X Y,
# default the middle) for SECONDS (default 60), clicking through notifications
# while golfers talk, and records 15 s more. "nofix" sets SGHOOKS_NO_SOUNDFIX.
# Writes ~/Videos/simgolf/NAME.mp4 and NAME.log (sghooks log with
# SGHOOKS_SNDLOG; "snd STUCK" lines mark a stream stuck with a refill pending).
name=${1:?name}
mode=${2:-fix}
res=${3:-800x600}
w=${res%x*}
h=${res#*x}
cx=${4:-$((w / 2))}
cy=${5:-$((h / 2 - 40))}
secs=${6:-60}
here=$(cd "$(dirname "$0")" && pwd)
out="$HOME/Videos/simgolf"
mkdir -p "$out"
rm -f "$out/$name.mp4" "$out/$name.log"

module=$(pactl load-module module-null-sink sink_name=sgtest sink_properties=device.description=sgtest) || exit 1
trap 'pactl unload-module "$module"' EXIT
export PULSE_SINK=sgtest SGHOOKS_SNDLOG=1
export SGHOOKS_LOG="Z:$(echo "$out/$name.log" | tr / '\\')"
if [ "$mode" = nofix ]; then export SGHOOKS_NO_SOUNDFIX=1; else unset SGHOOKS_NO_SOUNDFIX; fi

"$here/load-save.sh" "$res" :77 || exit 1
export DISPLAY=:77
ffmpeg -loglevel error -f x11grab -framerate 30 -video_size "${w}x${h}" -i :77 \
    -f pulse -i sgtest.monitor -c:v libx264 -preset veryfast -crf 20 -pix_fmt yuv420p \
    -c:a aac -b:a 160k "$out/$name.mp4" &
ff=$!
sleep 2
"$here/xdrive" key Escape
sleep 1
end=$(( $(date +%s) + secs ))
while [ "$(date +%s)" -lt "$end" ]; do
    "$here/xdrive" click "$cx" "$cy"
done
"$here/xdrive" move "$cx" "$cy"
sleep 15
kill -INT $ff
wait $ff 2>/dev/null
"$here/load-save.sh" stop >/dev/null
echo "$out/$name.mp4"
grep -a "STUCK" "$out/$name.log" || echo "no stuck streams logged"
