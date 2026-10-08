# Third-party notices

RushVR itself is licensed under GPL-2.0-or-later (see [LICENSE](LICENSE)). It is built on
[shadPS4](https://github.com/shadps4-emu/shadPS4), which has the same license, and the source of
everything in the release is this repository, at the tag of each release.

## What the PC package contains

| Part | License | Source |
| --- | --- | --- |
| The emulator (`pc-vr\shadps4.exe`) and the launcher (`launch.ps1`, `run.bat`, `Play Rush of Blood (DualSense).bat`) | GPL-2.0-or-later | this repository |
| PkgTool and LibOrbisPkg 0.2.231, by Maxton (`pc-vr\pkgtool`), unchanged, used by the launcher to unpack a game package | LGPL-3.0 | [github.com/maxton/LibOrbisPkg](https://github.com/maxton/LibOrbisPkg), release v0.2 |

## What is built into the emulator

The libraries the emulator is built with are in `shadps4-arm64-main/externals`, each with the
license of its own project; the texts of the licenses in use are in `shadps4-arm64-main/LICENSES`.
Among them: SDL3, Dear ImGui, fmt, spdlog, the Vulkan headers and loader, the Vulkan Memory
Allocator, glslang, FFmpeg, OpenAL Soft, libpng, zlib-ng, the Khronos OpenXR SDK (Apache-2.0),
Zydis, xbyak, Tracy and others. Where a license asks for it, the notice is kept in the source tree
next to the library.

## What is not included

The game. RushVR contains no game, firmware, keys or other copyrighted console files, and needs
your own copy of Until Dawn: Rush of Blood, dumped from your own PlayStation 4. Virtual Desktop is
not included either: it is a separate program by its own author.

Until Dawn: Rush of Blood, PlayStation and PlayStation VR are trademarks of Sony Interactive
Entertainment. This project is not affiliated with, endorsed or sponsored by Sony Interactive
Entertainment, Supermassive Games or Meta.

## Where the source is

Everything above that is licensed under the GPL or LGPL and is not by this project is available
from the places in the table. For the parts by this project, the source is this repository. If
something that should be here is missing, please open an issue.
