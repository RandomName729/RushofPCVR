# Changelog

All notable changes to RushVR, newest first. RushVR is shadPS4 plus a VR layer and the fixes that
let Until Dawn: Rush of Blood run under emulation.

## 0.4.1 - 2026-10-22

The new start-up window and a fix for streaks of colour in level 2. (The paid Motion edition keeps
the version number 0.4.0, with these same fixes.)

- **Long streaks of colour covering the view in level 2 are fixed.** The game's texture files have
  long names (the cellar props, 200+ characters); unpacked in a deep folder their full path passed
  Windows' 260 characters and the emulator could not open them (the log showed "file does not
  exist" about 20 000 times), so those textures never loaded. The emulator now hands such paths
  to Windows in the long form (`SHADPS4_NO_LONG_PATHS=1` turns that off). A short folder such as
  `C:\Games\RushVR` is still the safest.
- **Motion edition: unplug the DualSense.** While a PlayStation controller is connected to the PC
  the game uses it and ignores the Quest controllers. The start-up window warns when one is
  connected (Retry after unplugging, or Ignore to play anyway). (Only the Motion edition has the
  Quest controllers, so this concerns that edition.)
- **New start-up window**: Low, Medium and High buttons for the picture, with settings that fold
  away (Picture quality, PC window); a "Picture size sent to the headset" choice; a "PC window"
  card to choose what the monitor shows (left eye, right eye, both eyes merged into one normal
  view, or the two circles) with a zoom; fewer controls and less text on screen at once.
- **Higher defaults**: Medium is 2304 pixels an eye (High 2880, Low 1536).

## 0.4.0 - 2026-10-21

Shadows that flickered are fixed, memory use at large picture sizes, and a few start-up window choices.

- **Shadows no longer flicker on and off.** The game draws its shadows into one large depth
  picture, a few tiles a frame, and clears each tile before drawing it. The emulator cleared the
  whole picture every time, so the shadows drawn earlier in the frame were wiped and which ones
  survived changed from frame to frame. Now only the tile's own area is cleared
  (`SHADPS4_FULL_DEPTH_CLEAR=1` brings the old behaviour back). The flashlight's own shadows can
  still jump or show extra lines.
- **Large `eye_width` (2688, 2880) no longer runs the PC out of memory.** The limit was the memory
  Windows can promise (RAM plus the page file), not the graphics card. The emulator now watches
  it and, when less than 3 GB is left, frees the pictures nothing has used for a while
  (`SHADPS4_COMMIT_GC_MB` changes the 3 GB, `SHADPS4_COMMIT_GC=0` turns it off). A bigger page
  file helps (see the README).
- **Memory lines in the log**: a `MEMSTAT` line every ten seconds (or when it grows by 256 MB)
  with the graphics card's use, the picture cache, and how much memory the game and the PC have
  (`SHADPS4_MEM_LOG=0` turns it off).
- **Sharpening** has its own choice in the start-up window (Picture quality), from Off to Maximum.
  Virtual Desktop sharpens too; the two add up, so lower one of them if the picture looks crunchy.
- **Black corners (mask)** has three tighter choices (0.9, 0.86, 0.82), which a narrow field of
  view needs to hide the coloured corners.
- Defaults: field of view 105 (sharp, with hardly a border) and mask 0.9.

## 0.3.6 - 2026-10-19

A new look for the start-up window. The emulator itself is that of 0.3.5.

- **Cinematic horror look**: near-black with dried-blood crimson, a banner with the name, blood
  running from the top edge, scratches and fine grit over the window, sharp-cornered cards with a
  crimson marker, the edition badge (FREE or MOTION) and the version.
- **Minimize**: the window has its own title bar now, with minimize and close buttons, and it
  can be dragged by the banner.
- **A status line** along the bottom shows what the game will start with: the game found, the
  headset connection, and the controllers (it follows the choices as you change them).
- **Smoother**: the window fades in, the switches slide, and the buttons glow as the pointer
  comes onto them.

## 0.3.5 - 2026-10-17

The start-up window and the start file, ahead of the Motion edition (the Quest controllers playing
as PlayStation Move wands), which is separate. The emulator itself is that of 0.3.0.

- **One start file.** `Play Rush of Blood.bat` replaces `run.bat` and
  `Play Rush of Blood (DualSense).bat`.
- **A "Controllers" card in the start-up window**: a switch for playing with the Quest controllers
  as Move wands and three sliders for them (aim angle, reach, turning, with a "Recommended"
  button). In this edition they are greyed out and do nothing: they come alive in the Motion
  edition, which has the emulator support for them (`controllers`, `move_pitch`, `move_reach` and
  `move_turn` in `settings.txt`).
- Disabled switches, sliders and buttons in the window now look disabled.
- **No second, plain window.** The launcher used to fall back to an older, plain settings window
  when the new one could not be built. That backup is gone: if the window cannot be built, a
  message says why and the game starts with the saved settings.

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
