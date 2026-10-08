#!/usr/bin/env bash
# Does the sound survive its device going away and coming back? Plays the game (flat, no
# headset) on a named playback device, takes that device out of Windows for a few seconds
# and puts it back, and measures the level played on it before and after.
#   tools/pc-audio-device-test.sh "<device name>" [emulator exe] [seconds before the first measure]
# The device is audible meanwhile. Names: tools/audio-endpoint.ps1 list
set -u
root=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
device=${1:?device name}
exe=${2:-$root/build/win-x64/shadps4.exe}
warmup=${3:-60}
python=${PYTHON:-$(cat "$root/tools/python.local" 2>/dev/null || command -v python3 || command -v python)}
endpoint() {
    powershell -NoProfile -ExecutionPolicy Bypass -File "$(cygpath -w "$root/tools/audio-endpoint.ps1")" "$@" | tr -d '\r'
}

cd "$root/build/win-x64" || exit 1
config=user/config.json
cp "$config" "$config.audio-test"
restore() {
    endpoint show "$device"
    mv "$root/build/win-x64/$config.audio-test" "$root/build/win-x64/$config"
}
trap restore EXIT
"$python" - "$config" "${SETTING:-$device}" <<'EOF'
import json, sys
config = json.load(open(sys.argv[1], encoding="utf-8"))
config["Audio"]["sdl_main_output_device"] = sys.argv[2]
json.dump(config, open(sys.argv[1], "w", encoding="utf-8"), indent=2)
EOF

log=audio-device-test.log
SHADPS4_OPENXR=0 SHADPS4_VR_DEMO=1 timeout $((warmup + 85)) "$exe" \
    -g "$(cygpath -w "$root/games/CUSA12392/eboot.bin")" > "$log" 2>&1 &
# The level on every device there is: the sound is to be on the named one, and on the
# system's default while that is gone.
levels() {
    endpoint list | sed -n 's/^active *//p' | while IFS= read -r name; do
        printf '%s %s; ' "$(endpoint peak "$name" 3)" "$name"
    done
}
sleep "$warmup"
echo "before:           $(levels)"
# CONTROL=1 leaves the device alone: what the game plays at those times anyway.
[ -n "${CONTROL:-}" ] || endpoint hide "$device"
sleep 10
echo "while it is gone: $(levels)"
endpoint show "$device"
sleep 10
echo "after it is back: $(levels)"
sleep 8
echo "and later:        $(levels)"
wait
grep -a "Opened audio device\|went away\|is there: the sound\|not found, using default" "$log" |
    sed 's/^.*sdl_audio_out.cpp:[0-9]* //' | uniq -c
