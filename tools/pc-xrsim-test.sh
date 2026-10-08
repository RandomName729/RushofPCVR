#!/usr/bin/env bash
# Runs the Windows build with the Meta XR Simulator standing in for the headset: the emulator
# talks to it through OpenXR exactly as it talks to Virtual Desktop's runtime, without anybody
# wearing anything. The system's own OpenXR runtime is left alone (XR_RUNTIME_JSON only counts
# for the process started here).
#
#   tools/pc-xrsim-test.sh <name> <seconds> [NAME=value ...]
#
# Settings (environment):
#   XRSIM_HZ=<rate>              refresh rate of the simulated display (72, 80, 90, 120)
#   XRSIM_RECORD=<at>:<seconds>  record what the simulator's compositor shows, from second <at>
#   SHADPS4_INPUT_SCRIPT=<file>  scripted controller input, as for tools/run-win.sh
#
# Log, the recording and a few pictures cut out of it end up in build/dev/xrsim/<name>/.
set -u
root=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
name=$1
seconds=$2
shift 2
out="$root/build/dev/xrsim/$name"
simulator='C:\Program Files\MetaXRSimulator\v207.0\meta_openxr_simulator.json'
ffmpeg="/c/Program Files/Virtual Desktop Streamer/ffmpeg.exe"

rm -rf "$out"
mkdir -p "$out"
metavr xrsim app ensure-running > "$out/frontend.txt" 2>&1

env XR_RUNTIME_JSON="$simulator" "$@" bash "$root/tools/run-win.sh" "$seconds" > /dev/null &
game=$!

# The simulator's frontend learns of the game once its OpenXR session exists.
attached=0
for _ in $(seq 1 60); do
    if metavr xrsim runtime list --format json --timeout 2 2>/dev/null | grep -q '"'; then
        attached=1
        break
    fi
    sleep 1
done
echo "runtime attached: $attached" >> "$out/frontend.txt"

if [ "$attached" = 1 ] && [ -n "${XRSIM_HZ:-}" ]; then
    metavr xrsim device refresh-rate --rate "$XRSIM_HZ" --wait >> "$out/frontend.txt" 2>&1
fi
if [ "$attached" = 1 ] && [ -n "${XRSIM_RECORD:-}" ]; then
    at=${XRSIM_RECORD%%:*}
    length=${XRSIM_RECORD##*:}
    sleep "$at"
    metavr xrsim record --output "$(cygpath -w "$out/compositor.mp4")" --duration "$length" \
        --overwrite >> "$out/frontend.txt" 2>&1
    metavr xrsim runtime status >> "$out/frontend.txt" 2>&1
    metavr xrsim runtime fps >> "$out/frontend.txt" 2>&1
fi

wait "$game"
cp "$root/build/win-x64/run-stdout.log" "$out/log.txt"
if [ -f "$out/compositor.mp4" ] && [ -x "$ffmpeg" ]; then
    "$ffmpeg" -loglevel error -y -i "$(cygpath -w "$out/compositor.mp4")" -vf fps=1 \
        "$(cygpath -w "$out")\\shot-%02d.png" 2>> "$out/frontend.txt"
fi
grep -a -c "" "$out/log.txt"
ls "$out"
