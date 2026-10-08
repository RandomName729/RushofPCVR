#!/usr/bin/env bash
# Cross-compiles the emulator core for the Quest (aarch64 Linux/glibc, FEX guest CPU).
#   tools/build-arm64.sh            configure if needed, then build
# Needs tools/sysroot-arm64 (tools/mk-sysroot.mjs), FEXCore built in
# shadps4-arm64-main/runtime/build/fexcore-smoke-build and the Windows build's font embed tool.
set -u
root=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
source "$root/tools/env-cross.sh"
R=$(cygpath -m "$root/shadps4-arm64-main")
SR=$(cygpath -m "$root/tools/sysroot-arm64")
B=$(cygpath -m "$root/build/arm64")

if [[ ! -f "$B/build.ninja" ]]; then
  cmake -S "$R" -B "$B" -G Ninja \
    -DCMAKE_TOOLCHAIN_FILE="$(cygpath -m "$root/tools/toolchain-aarch64-linux.cmake")" \
    -DCMAKE_INSTALL_PREFIX=/usr \
    -DX11_Xext_LIB:FILEPATH="$SR/usr/lib/aarch64-linux-gnu/libXext.so" \
    -DXEXT_LIB:FILEPATH="$SR/usr/lib/aarch64-linux-gnu/libXext.so" \
    -DIMGUI_FONT_EMBED_EXECUTABLE="$(cygpath -m "$root/build/win-x64/src/imgui/renderer/Dear_ImGui_FontEmbed.exe")" \
    -DCMAKE_BUILD_TYPE=Release -DCMAKE_CXX_SCAN_FOR_MODULES=OFF \
    -DENABLE_BACHATA_RUNTIME=ON -DENABLE_FEX_GUEST_CPU=ON \
    -DFEXCORE_GUEST_CPU_SOURCE_DIR="$R/runtime/sources/fex" \
    -DFEXCORE_GUEST_CPU_BUILD_DIR="$R/runtime/build/fexcore-smoke-build" \
    -DENABLE_USERFAULTFD=OFF -DENABLE_DISCORD_RPC=OFF -DENABLE_UPDATER=OFF -DENABLE_TESTS=OFF \
    -DSDL_X11_XCURSOR=OFF -DSDL_X11_XDBE=OFF -DSDL_X11_XINPUT=OFF -DSDL_X11_XFIXES=OFF \
    -DSDL_X11_XRANDR=OFF -DSDL_X11_XSCRNSAVER=OFF -DSDL_X11_XSHAPE=OFF -DSDL_X11_XSYNC=OFF \
    -DSDL_X11_XTEST=OFF -DSDL_WAYLAND=OFF \
    > "$(cygpath -m "$root/build/arm64-configure.log")" 2>&1 || { echo "configure failed, see build/arm64-configure.log"; exit 1; }
fi
cmake --build "$B" --target shadps4 > "$(cygpath -m "$root/build/arm64-build.log")" 2>&1
status=$?
echo "build exit=$status"
grep -a -n "error:" "$(cygpath -m "$root/build/arm64-build.log")" | head -20
tail -2 "$(cygpath -m "$root/build/arm64-build.log")" | cut -c1-300
exit $status
