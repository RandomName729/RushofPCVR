# RushVR

**Until Dawn: Rush of Blood (PS4 / PlayStation VR) in virtual reality on a Windows PC**, shown in a
Meta Quest 3 through Virtual Desktop (or in any other OpenXR headset), played from your own copy of
the game through a PS4 emulator.

The emulator is [shadPS4](https://github.com/shadps4-emu/shadPS4), to which this project adds what
the game needs from a PlayStation VR: the headset, its tracking and reprojection, the controller
the game expects, and fixes that let this one title run under emulation (its streaming copies, its
depth buffers, its timing and resolution).

> **No game files are included or distributed.** You need your own copy of Until Dawn: Rush of
> Blood, dumped from your own PlayStation 4.

## Status

**Early work in progress**, built and tested by one person on one PC and one Quest 3.

- The title screen, the intro and **levels 1 to 4** play through (the tester went as far as level
  4; later levels are untested). There are still visual glitches in the levels: skybox, some
  textures and parts of the interface.
- Controls: the **DualSense** is the controller (buttons, sticks, touchpad, motion sensors). The
  headset's own controllers do not play the game, and the PlayStation Move controllers are not
  supported in this version.
- Performance: on a high-end laptop (Core i9-14900HX, GeForce RTX 4080 Laptop) level 1 runs at about
  22 to 29 frames a second where the console runs 60. The headset keeps showing 90 pictures a second
  by turning the picture to follow your head, but the game itself is slower than it should be.
  Performance is the current main task.
- Sharpness: the console draws each eye at 1152 x 1296 pixels, and a Quest 3 shows that over a wide
  view, so the picture looks soft at that size. The `eye_width` setting (a slider in the start-up
  window) makes the game draw larger, up to 2880 x 3240 an eye. This is little tested: it
  costs graphics-card time and memory, and it may show problems at some sizes. The coloured
  corners around the game's two lens circles are blacked out (`mask`), so the field of view can
  stay at 120 for the smoothest picture without borders, and `fxaa` smooths the edges.
- Known picture problems: a few textures are wrong (a clean picture of another texture), and the
  effects that read the depth buffer as a colour are shaded differently from the console.
- Expect rough edges, and please report what you find (see "If something does not work").

## What you need

- **A Meta Quest 3** with [Virtual Desktop](https://www.vrdesktop.net/) (the app on the headset and
  the Streamer on the PC). Other OpenXR headsets may work; nobody has tried.
- **A PS5 DualSense controller**, connected to the PC itself (USB cable or Bluetooth paired with the
  PC, not with the headset: paired with the headset it reaches the PC without motion sensors and
  touchpad). The game is played with it alone: the headset's own controllers do not play.
- **Until Dawn: Rush of Blood, US release CUSA03683, version 1.00** (the European release,
  CUSA02350 version 1.00, has been reported to start and play as well), dumped from your own console
  and game: either as the game's folder (the one with `eboot.bin`, `sce_sys` and `sce_module` in
  it) or as the `.pkg` package made from the dump. A package downloaded from the PlayStation Store
  is encrypted and cannot be used. Other versions are untested.
- **A Windows 10 or 11 (64-bit) PC** with a graphics card that supports Vulkan 1.3, 16 GB of memory
  or more, and the
  [Microsoft Visual C++ Redistributable (x64)](https://aka.ms/vs/17/release/vc_redist.x64.exe) (the
  launcher says so if it is missing).

## Installing and playing

1. **Put the program in a folder with a short path**, e.g. `C:\Games\RushVR`. Some of the game's
   files have long names, and the emulator cannot open a file whose full path is longer than
   Windows' 260 characters.
2. **Put your copy of the game in its `games` folder**, anywhere in it: the game's folder or its
   `.pkg` file (a package is unpacked the first time you start, which takes a minute or so). Or
   leave the game where it is: when the launcher finds none, a window asks where it is and
   remembers the answer.
3. **Set up Virtual Desktop**: install the Streamer on the PC and, in its options, choose **VDXR**
   as the OpenXR runtime. In the headset, in Virtual Desktop's streaming settings, set the frame
   rate to **120**. (SteamVR can be used instead: choose it under "Headset connection" in the
   start-up window, with the headset connected to SteamVR first. It has been tested through
   level 4 and runs well.)
4. **Connect the DualSense to the PC.**
5. **Connect to the PC with Virtual Desktop**, then start **`Play Rush of Blood (DualSense).bat`**
   on the desktop you see in the headset. A small window lets you choose the headset connection (Virtual Desktop
   or SteamVR), the resolution, the field of view, edge smoothing and the corner mask; Play
   starts the game, and the headset switches to it after a few seconds.
6. In the game's own start-up question, choose the **DualShock** controller, not the Move
   controllers.

Your saves, settings and the log are in `pc-vr\user` next to the launcher: the log is
`pc-vr\user\log\shad_log.txt`, the saves are in `pc-vr\user\home`.

## Settings

Settings are in `pc-vr\settings.txt`, one `key=value` a line (the small window at start-up changes
the main ones and writes them there; `menu=0` stops it from coming up). Lines starting with `#` are
ignored, and each setting is explained in the file itself. The ones to know:

| Setting | What it does |
| --- | --- |
| `fov` | How much of the headset's field of view the game draws, in percent. **120 is the default: the picture looks smoothest there, with no borders** (`mask` blacks out the coloured corners outside the game's lens-shaped circles). Less gives a sharper picture with a dark border; more draws a wider, softer view. |
| `fov_of` | What that percent is of: `psvr` (a PlayStation VR's own view, the default) or `headset`. |
| `runtime` | Which OpenXR runtime the game uses: `pc` (the one the PC has set up, the default), `virtualdesktop`, `steamvr`, or the full path of a runtime's `.json` file. Also in the start-up window. |
| `eye_width` | The width in pixels each eye is drawn at, 1152 to 2880 (the default is 1152). 1152 is what the console draws; larger is sharper in the headset, and slower, and takes more memory. |
| `fxaa` | Smooths the picture's edges on its way to the headset: 0 off, 0.35 light, 0.65 normal (default), 1 strong. Also in the start-up window. |
| `msaa`, `antialias` | Multisampling limits for games that use it. Rush of Blood draws without multisampling, so they change nothing here; use `fxaa` and a larger `eye_width` instead. |
| `sharpen` | Sharpening of the picture on its way to the headset, 0 to 1. |
| `mask` | Blacks out the coloured corners outside the two lens circles of the game's picture (1 = the circles as drawn, 0 = off). If the circle's edge looks cut off use 1.05; if some colour is left at the edge use 0.97. |
| `hands` | `0`: do not place the controller by your hands. |
| `env` | Extra environment variables for the emulator, as many lines as needed. |

`fps`, `pace`, `real_time` and `dynamic` are in the file but do
nothing for this game: it keeps its own pace and picks its picture by itself. (`resolution` of older
versions is no longer read: it is `eye_width` now.)

## If something does not work

- **The game says it was not found**: put the game's folder or `.pkg` in the `games` folder, or show
  the launcher where it is.
- **Black or waiting in the headset**: the game waits, black, while the headset is off the head or
  shows something else (Virtual Desktop's view of the desktop, for one). Put the headset on and
  make sure Virtual Desktop is streaming; `pause=0` stops the waiting.
- **The picture looks soft or pixelated**: the console draws each eye at only 1152 x 1296 pixels.
  Raise `eye_width` (the slider in the start-up window; 1152 is the default, 2880 the largest). A
  smaller `fov` (100 is the game's own view) puts the same pixels over fewer degrees: sharper, with a
  dark border. `sharpen` can be raised a little.
- **Four circles in the PC window**: the picture the game draws holds one circle for each eye;
  the window now shows that picture once (two circles). A Quest shows one circle in each eye.
- **Slow**: lower `eye_width` (1152 is the console's size) or `fxaa`. To see where the time goes, add the line
  `env=SHADPS4_FRAME_STATS=1` to `settings.txt` and play for a minute: the log then gets a line
  every five seconds with draws and render passes per frame, how long the emulator waits for the
  graphics card, and how busy each thread is.
- **Reporting a problem**: send `pc-vr\user\log\shad_log.txt` (and `console-errors.txt` from the
  same folder, if there is one) together with what you were doing.

## Building from source

The source is in `shadps4-arm64-main` (the emulator, with the VR layer in `src/core/vr`). On Windows,
with Visual Studio 2022 (C++ and CMake tools, and the Clang compiler) or any recent Clang, CMake
and Ninja:

```sh
git clone --recurse-submodules <this repository>
cd shadps4-arm64-main
cmake --build Build\x64-Clang-Release --target shadps4 -j 4
```

(Open the folder in Visual Studio to make the `x64-Clang-Release` configuration the first time; it
is described in `CMakeSettings.json`.) Copy the resulting `shadps4.exe` into `pc-vr`, next to
`launch.ps1` and `settings.txt`, and start the game with `Play Rush of Blood (DualSense).bat`.

## License

RushVR is free software, licensed under the
[GNU General Public License, version 2 or (at your option) any later version](LICENSE)
(GPL-2.0-or-later), the license of the shadPS4 it is built on.

The libraries under `shadps4-arm64-main/externals` keep their own licenses (see
`shadps4-arm64-main/LICENSES`), and so does the [Khronos OpenXR SDK](https://www.khronos.org/openxr/)
(Apache-2.0).

Until Dawn: Rush of Blood, PlayStation and PlayStation VR are trademarks of Sony Interactive
Entertainment. This project is not affiliated with, endorsed or sponsored by Sony Interactive
Entertainment, Supermassive Games or Meta. It contains no game, firmware, keys or other
copyrighted console files: use it only with software you own and have dumped yourself.

## Thanks

- **The [shadPS4](https://github.com/shadps4-emu/shadPS4) team and contributors.** None of this
  would exist without their PlayStation 4 emulator: everything here is built on top of their years
  of work. Thank you!
- [zenithblue-oss/shadps4-arm64](https://github.com/zenithblue-oss/shadps4-arm64), whose tree of
  shadPS4 this project started from.
- [bigmak94/AstroQuest](https://github.com/bigmak94/AstroQuest), the PlayStation VR layer for
  another game that this port was begun from: the virtual headset, its tracking and the
  OpenXR output.
- [LibOrbisPkg](https://github.com/maxton/LibOrbisPkg), whose PkgTool unpacks game packages for
  the launcher.
- [The Khronos Group](https://www.khronos.org/) for OpenXR and Vulkan, and
  [Virtual Desktop](https://www.vrdesktop.net/) for the PC streaming path.
