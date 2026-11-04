# RushVR 0.5.0

**Until Dawn: Rush of Blood in VR on a Windows PC, shown in a Quest 3 through Virtual Desktop.**
Bring your own copy of the game (US release, CUSA03683, version 1.00); nothing of it is included.

## What's new in 0.5.0

- **One free edition: the Quest controllers are in.** The Motion edition (your Quest controllers
  playing as the game's two PlayStation Move wands) is now part of RushVR for everyone, with no licence
  file. Switch it on in the start-up window's "Controllers" card; unplug the DualSense first, or the game
  keeps using it. Sliders set the aim angle, how far your hands reach and how far the guns turn.
- Everything from 0.4.5 is in: no more streaks in level 2, and the flashlight's shadows in level 1 are right.

## What you need

A Meta Quest 3 with Virtual Desktop, a PS5 DualSense connected to the PC (or just the Quest controllers), a Windows 10/11 PC with a
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
- The Quest controller support has been tried by one person on one Quest 3: tell me what feels off.

Full list of changes: [CHANGELOG.md](CHANGELOG.md). Licensed GPL-2.0-or-later, built on
[shadPS4](https://github.com/shadps4-emu/shadPS4); the source of this exe is in this repository.
Not affiliated with Sony Interactive Entertainment.
