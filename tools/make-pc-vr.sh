#!/usr/bin/env bash
# Puts the Windows build where "Play Astro Bot VR.bat" runs it from: pc-vr/shadps4.exe, with a
# user folder of its own (settings fit for playing, not for testing; saves; logs).
#   tools/make-pc-vr.sh            copy the emulator from build/win-x64, make what is missing
#   tools/make-pc-vr.sh --save     also copy the test build's save, if pc-vr has none yet
set -eu
root=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
python=${PYTHON:-$(cat "$root/tools/python.local" 2>/dev/null || command -v python3 || command -v python)}
dest="$root/pc-vr"

mkdir -p "$dest/user/log"
cp "$root/build/win-x64/shadps4.exe" "$dest/shadps4.exe"

# The emulator's own settings: what the test build has, with a log that goes to a file in the
# background instead of holding the game up on a console.
"$python" - "$(cygpath -m "$root/build/win-x64/user/config.json")" \
    "$(cygpath -m "$dest/user/config.json")" <<'EOF'
import json, os, sys
source, target = sys.argv[1], sys.argv[2]
config = json.load(open(target if os.path.exists(target) else source, encoding="utf-8"))
log = config.setdefault("Log", {})
log.update({"type": "file", "sync": False, "filter": "*:Info", "append": False,
            "size_limit": 52428800})
config.setdefault("GPU", {}).update({"present_mode": "Mailbox", "full_screen": False})
config.setdefault("Input", {}).update({"motion_controls_enabled": True,
                                       "background_controller_input": True})
config.setdefault("Vulkan", {}).update({"vkvalidation_enabled": False})
json.dump(config, open(target, "w", encoding="utf-8"), indent=2)
EOF

for name in users.json keys.json; do
    [ -f "$dest/user/$name" ] || cp "$root/build/win-x64/user/$name" "$dest/user/$name"
done
mkdir -p "$dest/user/input_config"
for name in default.ini global.ini; do
    [ -f "$dest/user/input_config/$name" ] ||
        cp "$root/build/win-x64/user/input_config/$name" "$dest/user/input_config/$name"
done

if [ "${1:-}" = "--save" ] && [ ! -d "$dest/user/home/1000/savedata/CUSA12392" ]; then
    mkdir -p "$dest/user/home/1000/savedata"
    cp -r "$root/build/win-x64/user/home/1000/savedata/CUSA12392" "$dest/user/home/1000/savedata/"
    echo "copied the test build's save"
fi
ls -la "$dest" "$dest/user"
