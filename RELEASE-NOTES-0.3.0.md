# RushVR 0.3.0

**Until Dawn: Rush of Blood in VR on a Windows PC, shown in a Quest 3 through Virtual Desktop.**
Bring your own copy of the game (US release, CUSA03683, version 1.00); nothing of it is included.

## What's new

- **Right speed, right sound.** The game's clock ran about 1.5 times too fast on a PC: captions
  ended before the voice, and animations hurried. Fixed, so captions, animations and voice now
  stay together.
- **50 to 60 frames a second** instead of 22 to 29. A new "Aim for" choice keeps the game on a rate
  that fits your headset: 60 with Virtual Desktop at 120 Hz (the default), 45 at 90 Hz, 40 at 80 Hz.
- **Smoother.** Short waits are no longer rounded up to Windows' 15.6 ms tick, the emulator runs
  on the fast cores of your processor, and a 3 ms stall in every frame is gone.
- **No more garbled textures** in the levels (floors and walls made of other pictures, noise).
- **Sharper textures:** 16x anisotropic filtering and a PS4 Pro mode switch (both on by default).
- **FXAA back** in the start-up window; the PC window now shows one eye filling the window.
- **A new start-up window**, dark and tidy, with the settings in cards.
- A crash in level 2 fixed.

## What you need

A Meta Quest 3 with Virtual Desktop, a PS5 DualSense connected to the PC, a Windows 10/11 PC with a
Vulkan 1.3 graphics card (an RTX 4080 laptop was used), and the game. Set Virtual Desktop's frame
rate to 120, then start `Play Rush of Blood (DualSense).bat`. See the README in the zip.

## Known problems

- The picture still looks pixelated or soft in places in the headset. Work continues.
- Now and then the game freezes for a second or more.
- Only the DualSense plays the game (native Quest controller support is next).
- Levels 5 and later are untested; a long session in level 2 and later still needs testing.

Full list of changes: [CHANGELOG.md](CHANGELOG.md). Licensed GPL-2.0-or-later, built on
[shadPS4](https://github.com/shadps4-emu/shadPS4). Not affiliated with Sony Interactive
Entertainment.
