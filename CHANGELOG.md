# Changelog

All notable changes to RushVR, newest first. RushVR is shadPS4 plus a VR layer and the fixes that
let Until Dawn: Rush of Blood run under emulation.

## 0.3.0 - 2026-10-17

The big ones: the game now runs at the speed it was made for, at a steady 50 to 60 frames a
second instead of 22 to 29, with the captions in step with the voice and no more garbled textures.

### Timing and speed

- **The game's clock is right.** Rush of Blood reads the processor's time stamp counter itself
  and assumes it ticks at 1.594 GHz; a PC's ticks faster (2.4 GHz on the test PC), so every game
  timer ran about 1.5 times too fast: captions finished several seconds before the voice did, and
  animations hurried. The 201 places in the game that read the counter now get a counter at the
  speed the game expects. Captions, animations, the ride and the voice stay together.
- **A steady frame rate, set by you.** The game only starts a frame when the emulator tells it
  the headset has refreshed, so the emulator now steers how often it says so until the game
  lands on the rate you choose. New **Frame rate / "Aim for"** choice in the start-up window
  (`target_fps` in `settings.txt`): **60 by default**, exactly every other refresh with Virtual
  Desktop at 120 Hz. At 90 Hz use 45, at 80 Hz use 40. In level 1 at 120 Hz (eye width 2304) the
  game ran at a median of about 50 frames a second, mostly between 43 and 60 (it was 22 to 29
  before).
- **Less stutter.** High-resolution timers for the emulator's waits (short waits used to round up
  to Windows' 15.6 ms tick), the system clock at 0.5 ms while the game runs, Windows' power
  throttling off for the emulator, the game on the fast cores of a hybrid processor (when at
  least 8 of them), and higher priority for the game, graphics and picture threads. Set
  `SHADPS4_NO_HOST_TUNING=1` or `SHADPS4_ALL_CORES=1` (as `env=` lines) to switch these off.
- **A 3 ms stall per frame is gone.** The graphics thread waited on two flags nobody ever sets,
  giving up after 3 ms each time. It now learns that after 32 tries and no longer waits.

### Picture

- **Garbled textures fixed.** The game streams the smaller levels of a texture first and
  expects the largest not to be looked at until it has loaded; the emulator looked at it anyway
  and showed other textures' data (wrong colours, noise, a floor made of other pictures). Textures
  whose largest level is not there are now shown from the next one down. Surfaces that were
  garbled are now clean; some are a little softer than they could be (see below).
- **New: Sharper textures (anisotropic filtering)**, 16x by default (`aniso`, 0 to 16, in the
  start-up window). Floors and walls seen at a slant stay sharp.
- **New: PS4 Pro mode** toggle in the start-up window, on by default (`neo_mode` in
  `user\config.json`). The game is told it runs on a PlayStation 4 Pro and shows sharper textures.
- **New: Smooth edges (FXAA)** is back in the start-up window (`fxaa`, default Normal, 0.65).
- The PC window shows one eye's view as a rectangle that fills the window (`SHADPS4_PC_WINDOW`
  `left`, `right` or `both`, and `SHADPS4_PC_WINDOW_ZOOM`), instead of a circle in the middle.
- Advanced, off by default: `env=SHADPS4_VR_LAYER_SCALE=0.5` hands Virtual Desktop a
  smaller pre-shrunk picture (a little smoother, some say).

### Stability

- Fixed a crash in level 2 (a reference into the texture cache that went stale when it grew).
  Needs a longer play test: reports from long sessions are welcome.
- The floor in the intro no longer comes up as noise: a texture file read now also invalidates
  the cache after the read, not only before it.

### Launcher

- A new look for the start-up window: dark, with the settings in cards (Headset, Picture,
  Picture quality, Frame rate), sliders, switches and one-click Play. If it cannot be built on a
  PC, the plain window is used.
- Settings in the window: headset connection, resolution of each eye, field of view, sharper
  textures, smooth edges, black corners (mask), frame rate, PS4 Pro mode.
- The release's `settings.txt` starts with `target_fps=60`, `aniso=16` and `fxaa=0.65`.

### Known problems

- The picture still looks pixelated or soft in places in the headset even at a high resolution.
  The emulator's own picture is sharp in a debug dump, so Virtual Desktop's stream, the game's
  half-resolution texture levels and fixed-size pieces of the game's picture may all play a part.
  Worth trying in Virtual Desktop: a higher bitrate and HEVC 10-bit or AV1.
- The game can freeze for a second or more now and then (the headset runtime takes long to
  accept a picture). More often at high resolutions.
- Only the DualSense plays the game. Native Quest controller support is the next big task.
- Levels 5 and later are untested. Heavy scenes still run below 60.

## 0.1.1

The first public version: the title screen, intro and levels 1 to 4 play with the DualSense in
a Quest 3 through Virtual Desktop (or SteamVR), 22 to 29 frames a second, with a start-up
window for the headset connection, the resolution of each eye (1152 to 2880), the field of view
and the corner mask.
