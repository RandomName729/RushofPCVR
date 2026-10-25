# RushVR 0.4.2

**Until Dawn: Rush of Blood in VR on a Windows PC, shown in a Quest 3 through Virtual Desktop.**
Bring your own copy of the game (US release, CUSA03683, version 1.00); nothing of it is included.

## What's new in 0.4.2

- **One download: `RushVR.exe`.** Double-click it. The first start unpacks the emulator and its
  tools to `%LOCALAPPDATA%\RushVR`, then the start-up window opens and asks where your game is (the
  game folder, or a `.pkg` file it unpacks for you). It remembers the choice. Updating means
  downloading the newer `RushVR.exe` and starting it: your settings and saves stay.
- **Files in deep folders load.** Some of the game's files have very long names; in a deeply
  nested folder Windows refused to open them and their textures never loaded. The emulator now copes
  (a short folder such as `C:\Games\RushVR` is still the safest).
- **New start-up window** with Low / Medium / High picture buttons, folding sections, a choice of
  what the monitor shows, and a "Picture size sent to the headset" option.
- **Stutter probe** for testers: add `env=SHADPS4_HITCH_PROBE=1` to `settings.txt` and the log
  records what the emulator was doing during every stall of 45 ms or more.

## What you need

A Meta Quest 3 with Virtual Desktop, a PS5 DualSense connected to the PC, a Windows 10/11 PC with a
Vulkan 1.3 graphics card (an RTX 4080 laptop was used), and the game. Set Virtual Desktop's frame
rate to 120, then start `RushVR.exe`.

Windows may warn about an unknown publisher (the file is not signed): choose "More info", then
"Run anyway". Your antivirus may also look twice at a program that unpacks other programs; the
source of everything inside is in this repository.

## Known problems

- **Level 2 (the pig room): long black streaks can cover the screen.** Not fixed yet.
- The picture still looks pixelated or soft in places, in the headset and on the monitor.
- Small stutters now and then; the probe above is how we are tracking them down.
- The flashlight's shadows can jump or show extra lines; the player's arms can look dark in the intro.
- Levels 5 and later are untested.

Full list of changes: [CHANGELOG.md](CHANGELOG.md). Licensed GPL-2.0-or-later, built on
[shadPS4](https://github.com/shadps4-emu/shadPS4); the source of this exe is in this repository.
Not affiliated with Sony Interactive Entertainment.
