# Packs the PC release of RushVR (Until Dawn: Rush of Blood in VR) into build\release\:
#   RushVR-<version>-PC-VR-Windows.zip   the play folder: the emulator you built, the launcher,
#                                        clean settings, and PkgTool for unpacking a game
#                                        package; no game, saves, logs or caches
#   SHA256SUMS.txt
#
#   powershell -ExecutionPolicy Bypass -File tools\make-release.ps1 -Version 0.1.0
#
# Before running it:
#   1. Build the emulator in Release:
#        cmake --build shadps4-arm64-main\Build\x64-Clang-Release --target shadps4
#      (-Exe says where else the shadps4.exe is.)
#   2. Put PkgTool where it is looked for: tools\pkgtool must hold PkgTool.exe, LibOrbisPkg.dll
#      and LICENSE.txt, from PkgTool-0.2.231.zip of
#      https://github.com/maxton/LibOrbisPkg/releases/tag/v0.2 (-PkgToolDir says where else).
param(
    [Parameter(Mandatory = $true)][string]$Version,
    [string]$Exe = "",
    [string]$PkgToolDir = "",
    [string]$RepoUrl = ""
)
$ErrorActionPreference = "Stop"
$root = Split-Path -Parent (Split-Path -Parent $MyInvocation.MyCommand.Path)
if ($Exe -eq "") { $Exe = Join-Path $root "shadps4-arm64-main\Build\x64-Clang-Release\shadps4.exe" }
if ($PkgToolDir -eq "") { $PkgToolDir = Join-Path $root "tools\pkgtool" }
if ($Version -notmatch '^\d+\.\d+\.\d+([-.][0-9A-Za-z.]+)?$') { throw "The version looks like 0.1.0 (or 0.1.0-beta1), not '$Version'." }

function Need([string]$path, [string]$what) {
    if (-not (Test-Path -LiteralPath $path)) { throw "Missing ${what}: $path" }
}
Need $Exe "emulator (build it first, see the top of this script)"
foreach ($name in "PkgTool.exe", "LibOrbisPkg.dll", "LICENSE.txt") {
    Need (Join-Path $PkgToolDir $name) "PkgTool file (see the top of this script)"
}
foreach ($path in "pc-vr\launch.ps1", "pc-vr\settings.txt", "run.bat", "Play Rush of Blood (DualSense).bat",
                  "pc-vr\template\user\config.json", "pc-vr\template\user\input_config\default.ini",
                  "pc-vr\template\user\input_config\global.ini", "LICENSE", "THIRD-PARTY-NOTICES.md") {
    Need (Join-Path $root $path) "file of the repository"
}

$utf8 = New-Object System.Text.UTF8Encoding($false)
$utf8bom = New-Object System.Text.UTF8Encoding($true)
function Write-Text([string]$path, [string]$text, $encoding = $utf8) {
    [IO.File]::WriteAllText($path, (($text -replace "`r`n", "`n") -replace "`n", "`r`n"), $encoding)
}

$name = "RushVR-$Version"
$out = Join-Path $root "build\release"
$pc = Join-Path $out "$name-PC-VR-Windows"
if (Test-Path -LiteralPath $out) { Remove-Item -LiteralPath $out -Recurse -Force }

# The first user's home: without it the emulator asks at its first start whether to move saves
# over from where an older shadPS4 kept them, in a message box behind the game.
foreach ($dir in "pc-vr\user\input_config", "games", "pc-vr\pkgtool",
                 "pc-vr\user\home\1000\savedata", "pc-vr\user\home\1000\trophy",
                 "pc-vr\user\home\1000\inputs") {
    New-Item -ItemType Directory -Force -Path (Join-Path $pc $dir) | Out-Null
}

Copy-Item -LiteralPath $Exe -Destination (Join-Path $pc "pc-vr\shadps4.exe")
Copy-Item -LiteralPath (Join-Path $root "pc-vr\launch.ps1") -Destination (Join-Path $pc "pc-vr\")
Copy-Item -LiteralPath (Join-Path $root "run.bat") -Destination $pc
Copy-Item -LiteralPath (Join-Path $root "Play Rush of Blood (DualSense).bat") -Destination $pc
Copy-Item -LiteralPath (Join-Path $root "pc-vr\template\user\config.json") -Destination (Join-Path $pc "pc-vr\user\")
Copy-Item -Path (Join-Path $root "pc-vr\template\user\input_config\*.ini") -Destination (Join-Path $pc "pc-vr\user\input_config\")
foreach ($file in "PkgTool.exe", "LibOrbisPkg.dll", "LICENSE.txt") {
    Copy-Item -LiteralPath (Join-Path $PkgToolDir $file) -Destination (Join-Path $pc "pc-vr\pkgtool\")
}
Write-Text (Join-Path $pc "pc-vr\pkgtool\README.txt") @"
PkgTool and LibOrbisPkg 0.2.231, by Maxton, unchanged: the launcher uses them to unpack a game
package (.pkg). Licensed under the GNU Lesser General Public License, version 3 (LICENSE.txt).
Source: https://github.com/maxton/LibOrbisPkg (release v0.2).
"@

# The settings file as documented: every line the launcher's window saved is left out, and the
# defaults of this release are put in their place.
$documented = Get-Content -LiteralPath (Join-Path $root "pc-vr\settings.txt") |
    Where-Object { $_ -notmatch '^[A-Za-z_]+=' }
$defaults = @(
    "",
    "# What this release starts with (the window at the start changes and saves them):",
    "resolution=1440",
    "fps=60",
    "fov=155",
    "fov_of=psvr",
    "menu=1"
)
Write-Text (Join-Path $pc "pc-vr\settings.txt") ((@($documented) + $defaults) -join "`n") $utf8bom

$source = if ($RepoUrl -ne "") { "The source is at $RepoUrl" } else { "The source is in the repository this package was published from" }
Write-Text (Join-Path $pc "games\PUT YOUR GAME HERE.txt") @"
Put your own copy of Until Dawn: Rush of Blood (US release CUSA03683, version 1.00) in this
folder, then start "Play Rush of Blood (DualSense).bat". Either form will do, anywhere in here:

- the game's folder, the one with eboot.bin in it, or
- the game's .pkg file: it is unpacked the first time, which takes a minute or so.
  (Only a package made from a dump of the game can be unpacked. One downloaded from the
  PlayStation Store is encrypted and cannot be used.)

If the game is somewhere else, just start: a window asks where it is.
"@
Write-Text (Join-Path $pc "README.txt") @"
RushVR $Version - Until Dawn: Rush of Blood in VR, played on this PC and shown in a Meta Quest
through Virtual Desktop.

1. Put your own copy of the game in the games folder: its folder (the one with eboot.bin in
   it) or its .pkg file, which is unpacked the first time. Or skip this: a window asks where
   the game is. Keep this folder's path short, e.g. C:\Games\RushVR: the emulator cannot open
   the game's files whose full path would be longer than 260 characters.
2. Virtual Desktop: install the Streamer on this PC and choose VDXR as the OpenXR runtime in
   its Options. In the headset, set Virtual Desktop's frame rate to 120 (Streaming settings).
3. Connect the DualSense to this PC (USB cable, or Bluetooth paired with the PC, not with the
   headset). Without a gamepad the headset's Touch controllers play.
4. Connect Virtual Desktop to this PC, then start "Play Rush of Blood (DualSense).bat". (If the
   Microsoft Visual C++ runtime is missing, it says so and offers Microsoft's download.)
5. In the game's own start-up question, choose the DualShock controller, not the Move
   controllers (they do not work yet).

This is an early version: the title screen, the intro and level 1 play through; later levels
are untested, and the game runs slower than on the console. Settings are in
pc-vr\settings.txt, the log in pc-vr\user\log\shad_log.txt, saves in pc-vr\user\home.

RushVR is free software under the GNU GPL, version 2 or later (LICENSE.txt), built on shadPS4.
$source
It contains no part of the game: use it only with a game you own.
"@
Write-Text (Join-Path $pc "LICENSE.txt") ([IO.File]::ReadAllText((Join-Path $root "LICENSE")))
Write-Text (Join-Path $pc "THIRD-PARTY-NOTICES.md") ([IO.File]::ReadAllText((Join-Path $root "THIRD-PARTY-NOTICES.md")))

# Nothing of the old project, and no other game's serial, may be left in what is shipped.
$stale = @()
Get-ChildItem -Path (Join-Path $pc "*") -Recurse -File -Include *.txt, *.md, *.ps1, *.bat, *.json, *.ini |
    ForEach-Object {
        if ($_.FullName -like "*\pkgtool\*") { return }
        $hit = Select-String -LiteralPath $_.FullName -Pattern "astro", "bigmak", "CUSA12392", "AstroQuest" -SimpleMatch -CaseSensitive:$false -List
        if ($hit) { $stale += $_.FullName.Substring($pc.Length + 1) }
    }
if ($stale.Count -gt 0) { throw "These files still mention the old project: $($stale -join ', ')" }

# The zip, with forward slashes in the names whatever the version of Windows PowerShell.
Add-Type -AssemblyName System.IO.Compression
Add-Type -AssemblyName System.IO.Compression.FileSystem
$zipPath = Join-Path $out "$name-PC-VR-Windows.zip"
$base = (Get-Item -LiteralPath $pc).FullName.TrimEnd('\')
$parent = Split-Path -Parent $base
$zip = [IO.Compression.ZipFile]::Open($zipPath, [IO.Compression.ZipArchiveMode]::Create)
try {
    foreach ($item in Get-ChildItem -LiteralPath $base -Recurse -Force) {
        $entry = $item.FullName.Substring($parent.Length + 1).Replace('\', '/')
        if ($item.PSIsContainer) {
            if (-not (Get-ChildItem -LiteralPath $item.FullName -Force)) { [void]$zip.CreateEntry($entry + "/") }
        } else {
            [void][IO.Compression.ZipFileExtensions]::CreateEntryFromFile($zip, $item.FullName, $entry, [IO.Compression.CompressionLevel]::Optimal)
        }
    }
} finally {
    $zip.Dispose()
}
$hash = (Get-FileHash -LiteralPath $zipPath -Algorithm SHA256).Hash.ToLower()
[IO.File]::WriteAllText((Join-Path $out "SHA256SUMS.txt"), "$hash  $name-PC-VR-Windows.zip`n", $utf8)
"{0}  {1:N1} MB" -f $zipPath, ((Get-Item -LiteralPath $zipPath).Length / 1MB)
"SHA-256 $hash"
