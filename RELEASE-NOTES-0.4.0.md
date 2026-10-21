# RushVR 0.4.0

**Until Dawn: Rush of Blood in VR on a Windows PC, shown in a Quest 3 through Virtual Desktop.**
Bring your own copy of the game (US release, CUSA03683, version 1.00); nothing of it is included.

## What's new

- **Large picture sizes (`eye_width` 2688 and 2880) no longer run the PC out of memory.** The limit
  was the memory Windows can promise (RAM plus the page file). The emulator now frees pictures
  nothing has used for a while when that gets low. Set a bigger Windows page file for the largest
  sizes (see the README), and expect a lot of work for the graphics card.
- **Sharpening** has its own choice in the start-up window (Off to Maximum); **Black corners** has
  three tighter choices; the default field of view is 105 (sharp, hardly any border).
- A `MEMSTAT` line every ten seconds in the log shows where the memory goes.

## What you need

A Meta Quest 3 with Virtual Desktop, a PS5 DualSense connected to the PC, a Windows 10/11 PC with a
Vulkan 1.3 graphics card (an RTX 4080 laptop was used), and the game. Set Virtual Desktop's frame
rate to 120, then start `Play Rush of Blood.bat`. See the README in the zip.

## Known problems

- The picture still looks pixelated or soft in places in the headset. Work continues.
- Shadows (the flashlight's) can flicker or show extra lines.
- Now and then the game freezes for a second or more; the largest picture sizes can run slowly.
- Levels 5 and later are untested; a long session in level 2 and later still needs testing.

Full list of changes: [CHANGELOG.md](CHANGELOG.md). Licensed GPL-2.0-or-later, built on
[shadPS4](https://github.com/shadps4-emu/shadPS4). Not affiliated with Sony Interactive
Entertainment.
