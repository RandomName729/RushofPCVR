# RushVR 0.5.7

**Until Dawn: Rush of Blood in VR on a Windows PC**, in a Meta Quest 3 through Virtual Desktop or another
OpenXR headset. Bring your own copy of the game (US CUSA03683, Europe CUSA02350 or Asia CUSA04006,
version 1.00); nothing of it is included.

## What's new in 0.5.7

- **Korean / Asian release support (CUSA04006, English/Chinese/Korean).** In this copy the game ran
  too fast (captions ahead of the voice, animations hurried) and the picture stayed at the console's
  size, because the speed fix and the sharper-picture fix were only switched on for the US and
  European releases. Both are now switched on for it as well. They are found in the game by the code
  around them, and nothing is changed unless all of it is found, so a copy where they don't match
  plays as before instead of breaking.
- **Other copies of Rush of Blood are tried too**: a copy with a serial not known to RushVR whose name
  says "Rush of Blood" (in English or Korean) gets the same search.
- **The start-up window accepts the European and Asian releases** (the status line is green for
  them), and the warning for a game folder whose path is too long now shows for every release.
- The log names the release it found at the start (`Until Dawn: Rush of Blood, ... release`).

### Testers wanted: the Korean / Asian copy

I don't have this copy, so this is untested. If you play it, please start a level, then open the
**Log** tab, choose **Save a copy...** and send the zip. Look for these lines:

- `Until Dawn: Rush of Blood, Asia (English, Chinese, Korean) release`
- `Rush of Blood draws ...` (the sharper picture is on), or `... not found in this build`
- the clock line: all places found (the right game speed), or `its places were not found`

If either says "not found", the log is exactly what I need to make it work for your copy.

Everything from 0.5.6 is included: the SteamVR (PS VR2) start-up crash fix, the Log tab, the much
sharper picture, the headset choice and any VR controllers as Move wands.

## What you need

A VR headset on a Windows 10/11 PC (a Meta Quest 3 with Virtual Desktop was used; a PS VR2 needs
Sony's PC adapter and SteamVR), a Vulkan 1.3 graphics card (an RTX 4080 laptop was used), your VR
controllers or a PS5 DualSense, and the game. Set Virtual Desktop's frame rate to 120, then start
`RushVR.exe`.

Windows may warn about an unknown publisher (the file is not signed): choose "More info", then
"Run anyway". Your antivirus may also look twice at a program that unpacks other programs; the
source of everything inside is in this repository.

## Known problems

- The Asian (Korean) release is untested; the European one has been reported to play.
- The game can close by itself now and then (seen once after a save); being looked at.
- Small stutters now and then; the player's arms can look dark in the intro.
- Headsets and controllers other than the Quest 3 and its controllers are untested.
- Levels 5 and later are untested.

Full list of changes: [CHANGELOG.md](CHANGELOG.md). Licensed GPL-2.0-or-later, built on
[shadPS4](https://github.com/shadps4-emu/shadPS4); the source of this exe is in this repository.
Not affiliated with Sony Interactive Entertainment.
