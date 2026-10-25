# Builds the plain RushVR.exe (no files inside; it starts pc-vr\launch.ps1 next to it). For the
# single-file release with everything inside, use tools\make-single-exe.ps1. Source: pc-vr\launcher-stub\RushVR.cs.
# Needs nothing but Windows: the C# compiler that comes with .NET Framework 4 is used.
#   powershell -ExecutionPolicy Bypass -File tools\make-launcher-exe.ps1 [-Out build\RushVR.exe]
# An icon is used if pc-vr\launcher-stub\RushVR.ico exists.
param([string]$Out = "")

$ErrorActionPreference = "Stop"
$root = Split-Path -Parent (Split-Path -Parent $MyInvocation.MyCommand.Path)
if ($Out -eq "") { $Out = Join-Path $root "build\RushVR.exe" }
$source = Join-Path $root "pc-vr\launcher-stub\RushVR.cs"
$icon = Join-Path $root "pc-vr\launcher-stub\RushVR.ico"

$csc = Join-Path $env:WINDIR "Microsoft.NET\Framework64\v4.0.30319\csc.exe"
if (-not (Test-Path -LiteralPath $csc)) { $csc = Join-Path $env:WINDIR "Microsoft.NET\Framework\v4.0.30319\csc.exe" }
if (-not (Test-Path -LiteralPath $csc)) { throw "csc.exe of .NET Framework 4 was not found under $env:WINDIR\Microsoft.NET." }
if (-not (Test-Path -LiteralPath $source)) { throw "Missing $source" }

$outDir = Split-Path -Parent $Out
if (-not (Test-Path -LiteralPath $outDir)) { New-Item -ItemType Directory -Path $outDir | Out-Null }

$arguments = @("/nologo", "/target:exe", "/optimize+", "/platform:anycpu", "/r:System.IO.Compression.dll", "/r:System.IO.Compression.FileSystem.dll", "/out:$Out")
if (Test-Path -LiteralPath $icon) { $arguments += "/win32icon:$icon" }
$arguments += $source
& $csc @arguments
if ($LASTEXITCODE -ne 0) { throw "csc.exe failed (exit $LASTEXITCODE)." }
Write-Host "Built $Out ($((Get-Item -LiteralPath $Out).Length) bytes)" -ForegroundColor Green
