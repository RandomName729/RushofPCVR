#!/usr/bin/env bash
# Runs the Windows build with a given resting head position and keeps the pictures it shows,
# to compare what the game draws for different positions of the head in tracker space.
#
#   tools/pc-view-test.sh <name> <seconds> <"x,y,z" of the resting head, metres> [input script]
#
# The pictures (one every few seconds) end up in build/dev/view/<name>/, with the log.
set -u
root=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
name=$1
seconds=$2
origin=$3
script=${4:-$root/build/dev/input-start.txt}
user="$root/build/win-x64/user"
out="$root/build/dev/view/$name"

rm -rf "$user/screenshots" "$out"
mkdir -p "$out"
printf '{ "origin_offset": [%s] }\n' "$origin" > "$user/vr.json"
echo ${SHOT_EVERY:-300} > "$user/auto_shot_every"
SHADPS4_VR_DEMO=0 SHADPS4_INPUT_SCRIPT="$script" bash "$root/tools/run-win.sh" "$seconds" > /dev/null
rm -f "$user/vr.json"
# In the order they were taken.
n=0
for shot in "$user"/screenshots/*.png; do
  [ -f "$shot" ] || continue
  cp "$shot" "$out/$(printf '%03d' $n).png"
  n=$((n + 1))
done
cp "$root/build/win-x64/run-stdout.log" "$out/log.txt"
echo "$name: $n pictures in $out"
