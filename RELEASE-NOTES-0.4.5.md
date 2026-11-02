# RushVR 0.4.5

**Until Dawn: Rush of Blood in VR on a Windows PC, shown in a Quest 3 through Virtual Desktop.**
Bring your own copy of the game (US release, CUSA03683, version 1.00); nothing of it is included.

## What's new in 0.4.5

- **Level 2 (the pig room) no longer has long black streaks over the screen.** The level's model
  data was wrong on the graphics card from the start; the emulator now refreshes it the first time
  each part is drawn. (The 0.4.1 notes claimed a fix that was not one; this is it.)
- **The flashlight's shadows in level 1 are right**: no more extra lines, black patches or
  flicker. The cause was "PS4 Pro mode", which is now removed from the start-up window and always
  off. Nothing looked different in the textures with it off.

## What you need

A Meta Quest 3 with Virtual Desktop, a PS5 DualSense connected to the PC, a Windows 10/11 PC with a
Vulkan 1.3 graphics card (an RTX 4080 laptop was used), and the game. Set Virtual Desktop's frame
rate to 120, then start `RushVR.exe`.

Windows may warn about an unknown publisher (the file is not signed): choose "More info", then
"Run anyway". Your antivirus may also look twice at a program that unpacks other programs; the
source of everything inside is in this repository.

## Known problems

- The picture still looks pixelated or soft in places, in the headset and on the monitor.
- Small stutters now and then; add `env=SHADPS4_HITCH_PROBE=1` to `settings.txt` and the log
  records what the emulator was doing during every stall of 45 ms or more.
- The player's arms can look dark in the intro.
- The game sometimes stops after level 1. Levels 5 and later are untested.

Full list of changes: [CHANGELOG.md](CHANGELOG.md). Licensed GPL-2.0-or-later, built on
[shadPS4](https://github.com/shadps4-emu/shadPS4); the source of this exe is in this repository.
Not affiliated with Sony Interactive Entertainment.
