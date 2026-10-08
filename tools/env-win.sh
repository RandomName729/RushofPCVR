#!/usr/bin/env bash
# Source this from Git Bash to get a no-admin Windows x64 toolchain:
#   clang-cl / lld-link (tools/llvm) + MSVC CRT and Windows SDK from xwin (tools/winsdk).
TOOLS_UNIX=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
TOOLS_WIN=$(cygpath -m "$TOOLS_UNIX")

export PATH="$TOOLS_UNIX/llvm/bin:$TOOLS_UNIX/cmake/bin:$TOOLS_UNIX/ninja:$PATH"

SDK="$TOOLS_WIN/winsdk"
export INCLUDE="$SDK/crt/include;$SDK/sdk/include/ucrt;$SDK/sdk/include/um;$SDK/sdk/include/shared;$SDK/sdk/include/winrt;$SDK/sdk/include/cppwinrt"
export LIB="$SDK/crt/lib/x86_64;$SDK/sdk/lib/um/x86_64;$SDK/sdk/lib/ucrt/x86_64"

# MSYS would otherwise rewrite /flag style arguments into paths.
export MSYS2_ARG_CONV_EXCL='*'
export MSYS_NO_PATHCONV=1
