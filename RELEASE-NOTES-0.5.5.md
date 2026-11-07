# RushVR 0.5.5

**Until Dawn: Rush of Blood in VR on a Windows PC**, in a Meta Quest 3 through Virtual Desktop or another
OpenXR headset. Bring your own copy of the game (US release, CUSA03683, version 1.00); nothing of it is
included.

## What's new in 0.5.5

- **A much sharper picture.** The pixelated look is fixed: only the part of each eye's picture the
  headset shows (plus a margin for turning your head) is sent, at full size. The margin is a new
  choice under Picture quality.
- **Your headset**: a new choice at the top of the start-up window (Meta Quest 3, 3S, 2, Pro, Pico 4,
  PlayStation VR2, Valve Index, HTC Vive, Windows Mixed Reality) sets a field of view and controller
  type to suit it. The field of view is 120 by default. Only the Quest 3 is tuned by testing.
- **Any VR controllers as Move wands**: Quest, PS VR2 Sense, Index, Vive and Windows Mixed Reality
  controllers, with a **Buttons...** window to choose which button does what.
- **Better performance, and the right game speed at more frame rate choices**: the timing behind
  several "Aim for" choices was fixed, so the game, its captions and its animations keep their pace.
- **A clearer start-up window**: DualSense or Move controllers as two buttons, cards that open to the
  side instead of making the window longer, and a Mods tab (mod support is coming).

### Also fixed and improved

- Settings were not found or saved when the RushVR folder's path had square brackets in it
  (`RushVR [0.5]`, say): every choice fell back to its default. Fixed.
- The European release (CUSA02350) gets the same clock fix as the US one, so its captions should
  stay with the voice. It is found by the code around each place, and only used when all 201 places
  are found; untested on a European copy, so tell me how it goes.
- "Picture size sent to the headset" is Full size for Low, Medium and High, and the start-up window
  offers the crop margin and the headset's field of view directly.
- The Move controllers work whatever controllers the headset's runtime reports (Quest, PS VR2 Sense
  through SteamVR, Index, Vive, Windows Mixed Reality), and holding Start for a second still resets
  the view.
- The PS4 Pro mode that broke the flashlight's shadows stays off (since 0.4.5).

## What you need

A VR headset on a Windows 10/11 PC (a Meta Quest 3 with Virtual Desktop was used; a PS VR2 needs
Sony's PC adapter and SteamVR), a Vulkan 1.3 graphics card (an RTX 4080 laptop was used), your VR
controllers or a PS5 DualSense, and the game. Set Virtual Desktop's frame rate to 120, then start
`RushVR.exe`.

Windows may warn about an unknown publisher (the file is not signed): choose "More info", then
"Run anyway". Your antivirus may also look twice at a program that unpacks other programs; the
source of everything inside is in this repository.

## Known problems

- The game can close by itself now and then (seen once after a save); being looked at.
- Small stutters now and then; the player's arms can look dark in the intro.
- Headsets and controllers other than the Quest 3 and its controllers are untested.
- Levels 5 and later are untested.

Full list of changes: [CHANGELOG.md](CHANGELOG.md). Licensed GPL-2.0-or-later, built on
[shadPS4](https://github.com/shadps4-emu/shadPS4); the source of this exe is in this repository.
Not affiliated with Sony Interactive Entertainment.
