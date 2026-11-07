# Builds the emulator from this checkout and puts it, with the current launcher, into a play copy
# (by default C:\UD ROB PC), so that the copy you play from always runs what is in the source.
# Since 0.5.0 there is one edition, so this replaces PRIVATE-paid-build\build-paid.ps1.
#
#   powershell -NoProfile -ExecutionPolicy Bypass -File tools\update-play-copy.ps1
#
#   -SkipBuild   only copy the shadps4.exe that is built now (no build)
#   -Play <dir>  another play copy (the folder that holds pc-vr\)
#
# Copied: shadps4.exe and launch.ps1 into <Play>\pc-vr. Never touched: settings.txt, the user
# folder (saves, config, logs, shader cache) and the game. Uncommitted changes are built too: this
# is for testing, the release scripts are for publishing.
param(
    [string]$Play = "C:\UD ROB PC",
    [switch]$SkipBuild
)
$ErrorActionPreference = "Stop"
$root = Split-Path -Parent (Split-Path -Parent $MyInvocation.MyCommand.Path)
$buildDir = Join-Path $root "shadps4-arm64-main\Build\x64-Clang-Release"
$builtExe = Join-Path $buildDir "shadps4.exe"
$launcher = Join-Path $root "pc-vr\launch.ps1"
$target = Join-Path $Play "pc-vr"

if (-not (Test-Path -LiteralPath $target)) { throw "The play copy's pc-vr folder is missing: $target" }

if (-not $SkipBuild) {
    cmake --build $buildDir --target shadps4
    if ($LASTEXITCODE -ne 0) { throw "The build failed (see above). The play copy was not changed." }
}
if (-not (Test-Path -LiteralPath $builtExe)) { throw "No emulator built yet: $builtExe" }

if (Get-Process -Name shadps4 -ErrorAction SilentlyContinue) {
    throw "The emulator is still running. Close the game and run this again."
}
Copy-Item -LiteralPath $builtExe -Destination (Join-Path $target "shadps4.exe") -Force
Copy-Item -LiteralPath $launcher -Destination (Join-Path $target "launch.ps1") -Force

$version = ""
$match = [regex]::Match([IO.File]::ReadAllText($launcher), '\$launcherVersion = "([^"]+)"')
if ($match.Success) { $version = " " + $match.Groups[1].Value }
Write-Host ""
Write-Host ("The play copy now has the emulator and launcher" + $version + " from this checkout:") -ForegroundColor Green
Get-Item -LiteralPath (Join-Path $target "shadps4.exe"), (Join-Path $target "launch.ps1") |
    Select-Object Name, Length, LastWriteTime | Format-Table -AutoSize | Out-String | Write-Host
