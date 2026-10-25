# Builds ONE file, RushVR.exe, from a finished release folder: the emulator, the launcher, the
# helper tools and the default settings go inside it as a zip, and the exe unpacks them to
# %LOCALAPPDATA%\RushVR on first start (see pc-vr\launcher-stub\RushVR.cs). No game files go in.
#
#   powershell -ExecutionPolicy Bypass -File tools\make-single-exe.ps1 -From <release folder> [-Out <exe>]
#
# <release folder> is the unpacked release (the folder that holds pc-vr\ and, for the Motion
# edition, pc-vr\license.dat). Run it on the free pack for the free exe and on the Motion pack
# for the Motion exe; never put the Motion exe on a public page.
param(
    [Parameter(Mandatory = $true)][string]$From,
    [string]$Out = ""
)

$ErrorActionPreference = "Stop"
$root = Split-Path -Parent (Split-Path -Parent $MyInvocation.MyCommand.Path)
$From = (Resolve-Path -LiteralPath $From).Path.TrimEnd('\')
if ($Out -eq "") { $Out = Join-Path $root "build\single\RushVR.exe" }
$source = Join-Path $root "pc-vr\launcher-stub\RushVR.cs"
$icon = Join-Path $root "pc-vr\launcher-stub\RushVR.ico"

foreach ($need in "pc-vr\launch.ps1", "pc-vr\shadps4.exe") {
    if (-not (Test-Path -LiteralPath (Join-Path $From $need))) { throw "${From} does not hold ${need}: give the finished release folder." }
}
$csc = Join-Path $env:WINDIR "Microsoft.NET\Framework64\v4.0.30319\csc.exe"
if (-not (Test-Path -LiteralPath $csc)) { $csc = Join-Path $env:WINDIR "Microsoft.NET\Framework\v4.0.30319\csc.exe" }
if (-not (Test-Path -LiteralPath $csc)) { throw "csc.exe of .NET Framework 4 was not found under $env:WINDIR\Microsoft.NET." }

Add-Type -AssemblyName System.IO.Compression
Add-Type -AssemblyName System.IO.Compression.FileSystem

$work = Join-Path ([IO.Path]::GetTempPath()) ("rushvr-single-" + [guid]::NewGuid().ToString("N"))
New-Item -ItemType Directory -Path $work | Out-Null
try {
    $zipPath = Join-Path $work "payload.zip"
    $count = 0
    $zip = [IO.Compression.ZipFile]::Open($zipPath, [IO.Compression.ZipArchiveMode]::Create)
    try {
        foreach ($file in Get-ChildItem -LiteralPath $From -Recurse -File -Force) {
            $relative = $file.FullName.Substring($From.Length + 1).Replace('\', '/')
            $lower = $relative.ToLowerInvariant()
            # Not packed: the batch files (the exe replaces them), logs and dumps, and any game
            # files that sit in a games folder (only the notes there are kept).
            if ($lower.EndsWith(".bat")) { continue }
            if ($lower.StartsWith("pc-vr/user/log/") -or $lower.StartsWith("pc-vr/user/depth_dump/") -or
                $lower.StartsWith("pc-vr/user/eye_dump/") -or $lower.StartsWith("pc-vr/user/rt_dump/") -or
                $lower.StartsWith("pc-vr/user/captures/")) { continue }
            if ($lower.StartsWith("games/") -and -not $lower.EndsWith(".txt")) { continue }
            if ($lower.EndsWith(".zip") -or $lower.EndsWith(".pkg")) { continue }
            [void][IO.Compression.ZipFileExtensions]::CreateEntryFromFile($zip, $file.FullName, $relative, [IO.Compression.CompressionLevel]::Optimal)
            $count++
        }
        # Empty folders too (the emulator's first-user home must exist, or it asks about moving saves).
        foreach ($dir in Get-ChildItem -LiteralPath $From -Recurse -Directory -Force) {
            if (Get-ChildItem -LiteralPath $dir.FullName -Force) { continue }
            $relative = $dir.FullName.Substring($From.Length + 1).Replace('\', '/')
            $lower = $relative.ToLowerInvariant()
            if ($lower.StartsWith("pc-vr/user/log") -or $lower.StartsWith("pc-vr/user/depth_dump") -or
                $lower.StartsWith("pc-vr/user/eye_dump") -or $lower.StartsWith("pc-vr/user/rt_dump") -or
                $lower.StartsWith("pc-vr/user/captures")) { continue }
            [void]$zip.CreateEntry($relative + "/")
        }
    } finally { $zip.Dispose() }

    $hash = (Get-FileHash -LiteralPath $zipPath -Algorithm SHA256).Hash.ToLower()
    $stampPath = Join-Path $work "payload.stamp"
    [IO.File]::WriteAllText($stampPath, $hash.Substring(0, 24))

    $outDir = Split-Path -Parent $Out
    if (-not (Test-Path -LiteralPath $outDir)) { New-Item -ItemType Directory -Path $outDir | Out-Null }
    $arguments = @("/nologo", "/target:exe", "/optimize+", "/platform:anycpu",
                   "/r:System.IO.Compression.dll", "/r:System.IO.Compression.FileSystem.dll",
                   "/resource:$zipPath,payload.zip", "/resource:$stampPath,payload.stamp", "/out:$Out")
    if (Test-Path -LiteralPath $icon) { $arguments += "/win32icon:$icon" }
    $arguments += $source
    & $csc @arguments
    if ($LASTEXITCODE -ne 0) { throw "csc.exe failed (exit $LASTEXITCODE)." }

    $exeHash = (Get-FileHash -LiteralPath $Out -Algorithm SHA256).Hash.ToLower()
    Write-Host ("Built {0}: {1} files inside, {2:N1} MB" -f $Out, $count, ((Get-Item -LiteralPath $Out).Length / 1MB)) -ForegroundColor Green
    Write-Host "SHA-256 $exeHash"
} finally {
    Remove-Item -LiteralPath $work -Recurse -Force -ErrorAction SilentlyContinue
}
