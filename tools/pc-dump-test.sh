#!/usr/bin/env bash
# Runs the Windows build into the first level and keeps what the game has drawn there at full
# size, to compare what a setting does to the picture itself.
#
#   tools/pc-dump-test.sh <name> <dump after seconds> [NAME=value ...]
#
# The render targets of the moment (the two eyes' pictures among them) end up as PNG files in
# build/dev/dump/<name>/, with the log.
set -u
root=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
name=$1
after=$2
shift 2
user="$root/build/win-x64/user"
out="$root/build/dev/dump/$name"
script=${PC_DUMP_SCRIPT:-$root/build/dev/input-gametime.txt}

rm -rf "$out" "$user/rt_dump" "$user/dump_render_targets" "$user/screenshots"
mkdir -p "$out"
echo 1000000 > "$user/auto_shot_every"
# The flag the emulator looks for, raised when the game has got where it should be.
"$root/tools/after.sh" "$after" "$user/dump_render_targets" &
env SHADPS4_VR_DEMO=0 SHADPS4_DUMP_WIDTH=4096 SHADPS4_INPUT_SCRIPT="$script" "$@" \
  bash "$root/tools/run-win.sh" $((after + 12)) > /dev/null
wait
cp "$user"/rt_dump/* "$out"/ 2>/dev/null
cp "$root/build/win-x64/run-stdout.log" "$out/log.txt"
echo "$name: $(ls "$out" | wc -l) files in $out"
