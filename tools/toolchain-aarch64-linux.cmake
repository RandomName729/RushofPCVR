# Cross-compiles for Debian arm64 (the libc the Android runtime ships) with the portable clang in
# tools/llvm and the sysroot assembled by tools/mk-sysroot.mjs.
set(CMAKE_SYSTEM_NAME Linux)
set(CMAKE_SYSTEM_PROCESSOR aarch64)

get_filename_component(ASTRO_TOOLS "${CMAKE_CURRENT_LIST_DIR}" ABSOLUTE)
set(CMAKE_SYSROOT "${ASTRO_TOOLS}/sysroot-arm64")

# Hard links to clang.exe: the name selects the target, and aarch64-unknown-linux-gnu.cfg beside
# them adds the sysroot, so build steps that call the compiler without CMake's flags still
# cross-compile (libpng's configuration generator does).
set(CMAKE_C_COMPILER "${ASTRO_TOOLS}/llvm/bin/aarch64-linux-gnu-clang.exe")
set(CMAKE_CXX_COMPILER "${ASTRO_TOOLS}/llvm/bin/aarch64-linux-gnu-clang++.exe")
set(CMAKE_ASM_COMPILER "${ASTRO_TOOLS}/llvm/bin/aarch64-linux-gnu-clang.exe")
set(CMAKE_C_COMPILER_TARGET aarch64-linux-gnu)
set(CMAKE_CXX_COMPILER_TARGET aarch64-linux-gnu)
set(CMAKE_ASM_COMPILER_TARGET aarch64-linux-gnu)

set(CMAKE_AR "${ASTRO_TOOLS}/llvm/bin/llvm-ar.exe" CACHE FILEPATH "")
set(CMAKE_RANLIB "${ASTRO_TOOLS}/llvm/bin/llvm-ranlib.exe" CACHE FILEPATH "")
set(CMAKE_NM "${ASTRO_TOOLS}/llvm/bin/llvm-nm.exe" CACHE FILEPATH "")
set(CMAKE_STRIP "${ASTRO_TOOLS}/llvm/bin/llvm-strip.exe" CACHE FILEPATH "")
set(CMAKE_OBJCOPY "${ASTRO_TOOLS}/llvm/bin/llvm-objcopy.exe" CACHE FILEPATH "")
set(CMAKE_OBJDUMP "${ASTRO_TOOLS}/llvm/bin/llvm-objdump.exe" CACHE FILEPATH "")
set(CMAKE_LINKER_TYPE LLD)

set(CMAKE_FIND_ROOT_PATH "${CMAKE_SYSROOT}")
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)

# Nothing built here can run on the build machine.
set(CMAKE_CROSSCOMPILING_EMULATOR "")
