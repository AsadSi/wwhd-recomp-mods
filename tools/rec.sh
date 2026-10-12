#!/bin/bash
# tools/rec.sh <shots dir> <out dir>: while the game runs, each screenshot it saves is shrunk to a 640px JPEG in <out
# dir> (named in the order they come, 0001.jpg...) and the full-size PNG deleted; the trace's "extras: frame" lines are
# the same order, so frame N is the Nth such line's step
mkdir -p "$2"; n=$(ls "$2" | wc -l | tr -d ' ')
while pgrep -x wwhd >/dev/null || [ -n "$(ls "$1"/*.png 2>/dev/null)" ]; do
  for f in $(ls -tr "$1"/*.png 2>/dev/null); do
    [ $(( $(date +%s) - $(stat -f %m "$f") )) -lt 1 ] && continue
    n=$((n + 1)); sips -s format jpeg -s formatOptions 60 -Z 640 "$f" --out "$2/$(printf %04d $n).jpg" >/dev/null 2>&1; rm -f "$f"
  done
  sleep 1
done
