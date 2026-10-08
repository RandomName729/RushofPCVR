#!/usr/bin/env bash
# Plays the same scripted walk through the first level on the monitor at two frame rates and
# keeps a picture every two seconds by the clock from each, to see whether the game does the
# same things at the same moments when it draws faster than the console did.
#
#   tools/pc-rate-compare.sh <name> "<NAME=value ...>"      (settings of the run to compare)
#
# The reference run is the game's own way (60 frames a second). Pictures and logs end up in
# build/dev/rate/<name>/{reference,test}/, contact sheets next to them.
set -u
root=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
name=$1
settings=${2:-}
user="$root/build/win-x64/user"
out="$root/build/dev/rate/$name"
seconds=${RATE_SECONDS:-332}
shots=${RATE_SHOTS:-2,264}

run() {
    local label=$1
    shift
    rm -rf "$user/screenshots" "$out/$label"
    mkdir -p "$out/$label"
    echo 1000000 > "$user/auto_shot_every"
    env SHADPS4_OPENXR=0 SHADPS4_VR_DEMO=0 SHADPS4_SHOT_SECONDS="$shots" SHADPS4_FRAME_STATS=1 \
        SHADPS4_TITLE_RESOLUTION=6 SHADPS4_INPUT_SCRIPT="$root/build/dev/input-playcheck.txt" "$@" \
        bash "$root/tools/run-win.sh" "$seconds" > /dev/null
    n=0
    for shot in "$user"/screenshots/*.png; do
        [ -f "$shot" ] || continue
        cp "$shot" "$out/$label/$(printf '%03d' $n).png"
        n=$((n + 1))
    done
    cp "$root/build/win-x64/run-stdout.log" "$out/$label/log.txt"
    node "$root/tools/contact.mjs" "$out/$label.png" --cols 8 --width 240 "$out/$label"/*.png | tail -1
}

run reference
# shellcheck disable=SC2086
run test $settings
