<div align="center">

# RushVR

### *Until Dawn: Rush of Blood* in virtual reality on a Windows PC

Play the PlayStation VR game on a **Meta Quest 3** through Virtual Desktop (or any OpenXR headset),
from your own copy of the game, through a PS4 emulator.

[![Version](https://img.shields.io/badge/version-0.5.7-2f81f7?style=flat-square)](CHANGELOG.md)
[![License](https://img.shields.io/badge/license-GPL--2.0--or--later-3fb950?style=flat-square)](LICENSE)
[![Platform](https://img.shields.io/badge/platform-Windows%2010%20%2F%2011-8957e5?style=flat-square)](#requirements)
[![Headset](https://img.shields.io/badge/headset-Quest%203%20%C2%B7%20OpenXR-f0883e?style=flat-square)](#requirements)

**EVERYTHING WILL BE OUT FOR FREE AT THE v1.0 RELEASE**

**UPCOMING FLAT MODE SUPPORT: Game runs better while on flat mode, guns follow the head, keyboard/mouse and controller support, needs further fixes and testing before releasing publicly, but it is playable and early access is out on my Patreon!**

[Download](https://github.com/RandomName729/RushofPCVR/releases) &nbsp;·&nbsp;
[Quick start](#quick-start) &nbsp;·&nbsp;
[Settings](#settings) &nbsp;·&nbsp;
[Troubleshooting](#troubleshooting) &nbsp;·&nbsp;
[Changelog](CHANGELOG.md)

</div>

---

> [!IMPORTANT]
> **No game files are included or distributed.** You need your own copy of *Until Dawn: Rush of
> Blood*, dumped from your own PlayStation 4.

## What this is

RushVR is [shadPS4](https://github.com/shadps4-emu/shadPS4), a PlayStation 4 emulator, plus what
the game needs from a PlayStation VR: the headset, its tracking and reprojection, the controller
the game expects, and fixes that let this one title run under emulation (its streaming copies, its
depth buffers, its timing and resolution).

| | |
| --- | --- |
| **One file** | `RushVR.exe` is the whole download. It unpacks itself on first start and remembers your game. |
| **Start-up window** | Low / Medium / High picture buttons, field of view, sharpness, edge smoothing, frame rate, and what the monitor shows. |
| **Sharper than the console** | The game can draw up to 2880 x 3240 pixels an eye (the console draws 1152 x 1296). |
| **Made for Virtual Desktop** | Held to 60 fps against a 120 Hz stream (90 and 120 fps to try, experimental), with the console's own speed kept so captions stay with the voice. SteamVR works too. |
| **Your choice of controllers** | The **DualSense** (buttons, sticks, touchpad, motion sensors), or your **VR controllers** (Quest, Index, Vive, Windows Mixed Reality and others) playing as the game's two PlayStation Move wands, with reach, aim angle and turning sliders. |

## Status

> [!NOTE]
> **Early work in progress, version 0.5.7**, built and tested by one person on one PC and one
> Quest 3. Expect rough edges, and please report what you find
> ([Troubleshooting](#troubleshooting)).

**Works:** the title screen, the intro and **levels 1 to 4** play through. Later levels are untested.
On a high-end laptop (Core i9-14900HX, RTX 4080 Laptop) at 2304 pixels an eye the game ran at a
median of about 50 fps in level 1, mostly between 43 and 60.

**Known problems:**

- The picture can still look pixelated or soft in the headset, especially in dark scenes. The effects
  that read the depth buffer as a colour are shaded differently from the console, and textures whose
  largest level has not loaded show one level lower.
- Small stutters now and then, and the game can freeze for a second or more. See
  [Slow or stuttering](#troubleshooting) for how to help find the cause.
- The player's arms can look dark in the intro.
- The game sometimes stops after level 1.
- The Move controller support has been tried by one person on one Quest 3; other VR controllers (Index, Vive, Windows Mixed Reality) have bindings but are untested.

**Controllers:** the start-up window has a "Controllers" card. By default the game is played with the
**DualSense**. Pick **Move controllers** to play with your VR headset's own two controllers
instead: unplug the DualSense first (while one is connected the game uses it and ignores the VR
controllers), and in the game's own start-up question choose the Move controllers. The card's three
sliders set how high the guns are aimed, how far your hands reach and how far the guns turn. Pick
your **Controller type** (Meta Quest, PlayStation VR2 Sense, Valve Index, Windows Mixed Reality, HTC Vive or other) and press
**Buttons...** to choose which of your controller's buttons does what on the Move wands.

## Requirements

| You need | Details |
| --- | --- |
| **Headset** | A **Meta Quest 3** with [Virtual Desktop](https://www.vrdesktop.net/) (the app on the headset and the Streamer on the PC). Other OpenXR headsets may work; nobody has tried. |
| **Controllers** | A **PS5 DualSense** connected to the PC itself (USB, or Bluetooth paired with the PC, not the headset: paired with the headset it reaches the PC without motion sensors and touchpad), **or** just your VR headset's two controllers (see Status). |
| **Game** | *Until Dawn: Rush of Blood*, US release **CUSA03683, version 1.00**, dumped from your own console and game: the game's folder (the one with `eboot.bin`, `sce_sys` and `sce_module`) or the `.pkg` made from the dump. A package downloaded from the PlayStation Store is encrypted and cannot be used. The European (CUSA02350) and Asian English/Chinese/Korean (CUSA04006) releases, version 1.00, get the same speed and picture fixes, found in them by pattern: both have been reported to play. |
| **PC** | Windows 10 or 11 (64-bit), a graphics card with **Vulkan 1.3**, 16 GB of memory or more, and the [Microsoft Visual C++ Redistributable (x64)](https://aka.ms/vs/17/release/vc_redist.x64.exe) (the launcher says so if it is missing). |

## Quick start

1. **Download `RushVR.exe`** from [Releases](https://github.com/RandomName729/RushofPCVR/releases) and
   put it anywhere (a short folder such as `C:\Games\RushVR` is fine). The first start unpacks the
   emulator and its tools to `%LOCALAPPDATA%\RushVR` (set `RUSHVR_HOME` first to use another folder
   or drive) and opens the start-up window. To update, download the newer `RushVR.exe` and start it:
   your settings and saves stay. Windows may warn about an unknown publisher (the file is not
   signed): choose **More info**, then **Run anyway**.
2. **Show it your game.** The first time, choose **Show where it is...** and pick the `eboot.bin` in
   the game's folder, or the `.pkg` (unpacked once into the RushVR folder, about 13 GB, a minute or
   so). The game can stay where it is; RushVR remembers it. A short path (`C:\Games\Rush`) is the
   safest, though since 0.4.1 paths over Windows' 260 characters open too.
3. **Set up Virtual Desktop.** Install the Streamer on the PC and choose **VDXR** as its OpenXR
   runtime. In the headset, set the streaming frame rate to **120**. (SteamVR works instead: choose
   it under "Headset connection" with the headset connected to SteamVR first; tested through level 4.)
4. **Connect the DualSense to the PC** (or, for the VR controllers, unplug it and pick **Move controllers**
   in the "Controllers" card), then connect to the PC with Virtual Desktop and start
   `RushVR.exe` on the desktop you see in the headset.
5. **Pick your headset, a picture quality, and press Play.** Picking the headset sets the field of view
   and the controller type that suit it (you can still change both). The headset switches to the game after a few seconds.
6. In the game's own start-up question, choose the **DualShock** if you play with the DualSense, or the **Move controllers** if you play with the VR controllers.

Settings, saves and the log live in `%LOCALAPPDATA%\RushVR`: settings in `pc-vr\settings.txt`, the
log in `pc-vr\user\log\shad_log.txt`, saves in `pc-vr\user\home`.

## Settings

The start-up window changes the main ones and writes them to `pc-vr\settings.txt`, one `key=value`
a line (`menu=0` stops the window from coming up). Lines starting with `#` are ignored, and each
setting is explained in the file itself.

<details>
<summary><b>All settings</b></summary>

<br>

| Setting | What it does |
| --- | --- |
| `fov` | How much of the headset's field of view the game draws, in percent. **120 is the default and recommended: it fills the headset's view with no border** (100 is the PlayStation VR's own view: sharpest, with a dark border). The start-up window's **Your headset** choice sets it to suit the headset picked. |
| `fov_of` | What that percent is of: `psvr` (a PlayStation VR's own view, the default) or `headset`. |
| `runtime` | Which OpenXR runtime the game uses: `pc` (the one the PC has set up, the default), `virtualdesktop`, `steamvr`, or the full path of a runtime's `.json` file. Also in the start-up window. |
| `eye_width` | The width in pixels each eye is drawn at, 1152 to 2880 (default 2304). 1152 is what the console draws; larger is sharper, slower and takes more memory. The Low, Medium and High buttons are 1536, 2304 and 2880. |
| `target_fps` | The frame rate the game is held to, 20 to 120 (0 leaves the game's own timing). **60 is the default**, steady with Virtual Desktop at 120 Hz; use 45 at 90 Hz and 40 at 80 Hz. **90 and 120 are experimental**: the game keeps its speed and its captions stay with the voice, but the ride can feel a touch faster at times, and only a fast PC gets near 120. Also in the start-up window ("Aim for"). |
| `aniso` | Anisotropic filtering of the game's textures: 0, 2, 4, 8 or 16 (default). |
| `fxaa` | Smooths the picture's edges on its way to the headset: 0 off, 0.35 light, 0.65 normal (default), 1 strong. |
| `sharpen` | Sharpening of the picture on its way to the headset, 0 to 1. |
| `mask` | Blacks out the coloured corners outside the two lens circles (1 = the circles as drawn, 0 = off; 0.9 is the default, the choices go from 0.82 to 1.05). If the circle's edge looks cut off use a higher value (1.05); if some colour is left at the edge use a lower one. |
| `layer_scale` | The size of the picture handed to the headset's runtime, in percent of what the game draws: 100, 75 or 50 (when not set: 50 from an `eye_width` of 2048 up, 100 below). Shrinking it here, with a smoother filter than Virtual Desktop's, gives the PC and the stream less to carry, at some cost in sharpness. The Low, Medium and High buttons set it together with `eye_width`, `aniso`, `fxaa` and `sharpen`. |
| `pc_view` | What the game's window on the PC shows (the headset is not affected): `left` (default) or `right`, that eye's picture cropped to fill the window; `merged`, both eyes joined into one flat picture (things very close up can look doubled across the middle); `both`, the whole picture with both lens circles. |
| `pc_zoom` | How close in the cropped and merged views are, in percent: 100 (default) is the most that fits the window without black corners. |
| `msaa`, `antialias` | Multisampling limits for games that use it. Rush of Blood draws without multisampling, so they change nothing here. |
| `game` | The game's `eboot.bin`, its folder or its `.pkg`, when it is not in the `games` folder (set by the "Where is the game?" window). |
| `hands` | `0`: do not place the controller by your hands. |
| `env` | Extra environment variables for the emulator, as many lines as needed. `env=SHADPS4_HITCH_PROBE=1` turns on the stutter probe (see below). |

`fps`, `pace`, `real_time` and `dynamic` are in the file but do nothing for this game: it keeps its
own pace and picks its picture by itself. (`resolution` of older versions is no longer read: it is
`eye_width` now.)

</details>

## Troubleshooting

<details>
<summary><b>The game says it was not found</b></summary>

<br>

Choose **Show where it is...** in that window and pick the game's `eboot.bin` or its `.pkg`, or put
the game's folder or `.pkg` in the `games` folder of the RushVR folder and choose Look again.

</details>

<details>
<summary><b>Windows or an antivirus warns about RushVR.exe</b></summary>

<br>

The file is not signed, and it unpacks other programs, which some antivirus programs look at twice.
The source of everything inside is in this repository (see [Building from source](#building-from-source)).

</details>

<details>
<summary><b>Black or waiting in the headset</b></summary>

<br>

The game waits, black, while the headset is off the head or shows something else (Virtual Desktop's
view of the desktop, for one). Put the headset on and make sure Virtual Desktop is streaming;
`pause=0` stops the waiting.

</details>

<details>
<summary><b>The picture looks soft or pixelated</b></summary>

<br>

The console draws each eye at only 1152 x 1296 pixels. Raise `eye_width` (2304 is the default, 2880
the largest), and in Virtual Desktop try a higher bitrate and HEVC 10-bit or AV1. A smaller `fov`
(100 is the game's own view) puts the same pixels over fewer degrees: sharper, with a dark border.
`sharpen` can be raised a little.

</details>

<details>
<summary><b>Circles in the PC window</b></summary>

<br>

The picture the game draws holds one circle for each eye. The window shows one eye cropped to fill
it, like a normal game, unless `pc_view` ("PC window" in the start-up window) says `both` (the two
circles) or `merged` (both eyes joined into one picture).

</details>

<details>
<summary><b>Windows runs out of memory at a large eye_width</b></summary>

<br>

At 2688 and 2880 especially, the log says something like "Insufficient system resources", or the
game closes. Windows has a limit on memory it can promise (RAM plus the page file), and the game
asks for a lot of it. Give Windows a bigger page file (Settings, System, About, Advanced system
settings, Performance, Advanced, Virtual memory: a custom size of 32768 MB initial and 49152 MB
maximum on the system drive) and restart the PC. The emulator also frees pictures nothing has used
for a while when Windows has less than 3 GB of that left, and writes `MEMSTAT` lines to the log
every ten seconds.

</details>

<details>
<summary><b>Slow or stuttering</b></summary>

<br>

Lower `eye_width` (1152 is the console's size), and set "Aim for" to half of Virtual Desktop's frame
rate (60 at 120 Hz, 45 at 90 Hz, 40 at 80 Hz). To see where the time goes, add the line
`env=SHADPS4_FRAME_STATS=1` to `settings.txt` and play for a minute: the log then gets a line every
five seconds with draws and render passes per frame, how long the emulator waits for the graphics
card, and how busy each thread is. To help find stutters, add `env=SHADPS4_HITCH_PROBE=1` as well:
whenever 45 ms or more pass between two shown frames, the log records what each part of the
emulator was doing. The log is what to send.

</details>

**Reporting a problem:** open the start-up window's **Log** tab and press **Save a copy...** (choose
.zip to save all the logs in one file), and send it together with what you were doing. The logs are
in `pc-vr\user\log` (`shad_log.txt`, and `console-errors.txt` if there is one).

## Building from source

<details>
<summary><b>Build the emulator and the one-file RushVR.exe</b></summary>

<br>

The source is in `shadps4-arm64-main` (the emulator, with the VR layer in `src/core/vr`). On Windows,
with Visual Studio 2022 (C++ and CMake tools, and the Clang compiler) or any recent Clang, CMake and
Ninja:

```sh
git clone --recurse-submodules <this repository>
cd shadps4-arm64-main
cmake --build Build\x64-Clang-Release --target shadps4 -j 4
```

(Open the folder in Visual Studio to make the `x64-Clang-Release` configuration the first time; it is
described in `CMakeSettings.json`.) For development, copy the resulting `shadps4.exe` into `pc-vr`,
next to `launch.ps1` and `settings.txt`, and start the game with `Play Rush of Blood.bat`.

To make the one-file `RushVR.exe` that is released (Windows PowerShell, from the repository folder,
with `tools\pkgtool` holding PkgTool as `tools\make-release.ps1` describes):

```powershell
powershell -ExecutionPolicy Bypass -File tools\make-release.ps1 -Version 0.5.7
powershell -ExecutionPolicy Bypass -File tools\make-single-exe.ps1 `
    -From build\release\RushVR-0.5.7-PC-VR-Windows -Out build\single\RushVR-0.5.7.exe
```

The first command packs the play folder (the emulator, the launcher, PkgTool and the default
settings); the second puts that folder inside `RushVR.exe`, whose small C# starter is
`pc-vr\launcher-stub\RushVR.cs` (built with the compiler that comes with Windows). No game files go
in.

</details>

## License

RushVR is free software, licensed under the
[GNU General Public License, version 2 or (at your option) any later version](LICENSE)
(GPL-2.0-or-later), the license of the shadPS4 it is built on.

The libraries under `shadps4-arm64-main/externals` keep their own licenses (see
`shadps4-arm64-main/LICENSES`), and so does the [Khronos OpenXR SDK](https://www.khronos.org/openxr/)
(Apache-2.0).

*Until Dawn: Rush of Blood*, PlayStation and PlayStation VR are trademarks of Sony Interactive
Entertainment. This project is not affiliated with, endorsed or sponsored by Sony Interactive
Entertainment, Supermassive Games or Meta. It contains no game, firmware, keys or other copyrighted
console files.

## Thanks

- **The [shadPS4](https://github.com/shadps4-emu/shadPS4) team and contributors.** None of this
  would exist without their PlayStation 4 emulator: everything here is built on top of their years
  of work. Thank you!
- [zenithblue-oss/shadps4-arm64](https://github.com/zenithblue-oss/shadps4-arm64), whose tree of
  shadPS4 this project started from.
- [bigmak94/AstroQuest](https://github.com/bigmak94/AstroQuest), the PlayStation VR layer for
  another game that this port was begun from: the virtual headset, its tracking and the OpenXR
  output.
- [LibOrbisPkg](https://github.com/maxton/LibOrbisPkg), whose PkgTool unpacks game packages for the
  launcher.
- [The Khronos Group](https://www.khronos.org/) for OpenXR and Vulkan, and
  [Virtual Desktop](https://www.vrdesktop.net/) for the PC streaming path.
