# Loads the functions of the PC launcher (pc-vr/launch.ps1), not its main flow, and tries the
# ones that look for the game on folder layouts made up for the purpose in build/launcher-test.
#   powershell -ExecutionPolicy Bypass -File tools/tests/launcher-test.ps1
# With the game in games/CUSA12392 its param.sfo is used for the layouts; without it, the
# checks that need one are left out.
$top = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
$launcher = Join-Path $top "pc-vr\launch.ps1"
Add-Type -AssemblyName System.Windows.Forms
$ast = [System.Management.Automation.Language.Parser]::ParseFile($launcher, [ref]$null, [ref]$null)
foreach ($function in $ast.FindAll({ $args[0] -is [System.Management.Automation.Language.FunctionDefinitionAst] }, $false)) {
    . ([scriptblock]::Create($function.Extent.Text))
}
$unpackFolder = ".unpacking"
$madeFor = "CUSA12392"
$realSfo = Join-Path $top "games\CUSA12392\sce_sys\param.sfo"
$haveSfo = [System.IO.File]::Exists($realSfo)
$base = Join-Path $top "build\launcher-test"
if ([System.IO.Directory]::Exists($base)) { [System.IO.Directory]::Delete($base, $true) }
$failed = 0
function Check([string]$what, $got, $want) {
    if ("$got" -eq "$want") { "ok    $what" } else { $script:failed++; "FAIL  $what`n      got:  $got`n      want: $want" }
}
function Make-Game([string]$folder, [bool]$withSfo) {
    [void][System.IO.Directory]::CreateDirectory($folder)
    [System.IO.File]::WriteAllText((Join-Path $folder "eboot.bin"), "x")
    if ($withSfo -and $haveSfo) {
        [void][System.IO.Directory]::CreateDirectory((Join-Path $folder "sce_sys"))
        [System.IO.File]::Copy($realSfo, (Join-Path $folder "sce_sys\param.sfo"))
    }
}
function Make-Package([string]$path, [string]$contentId, [int]$size) {
    [void][System.IO.Directory]::CreateDirectory([System.IO.Path]::GetDirectoryName($path))
    $bytes = New-Object byte[] $size
    $bytes[0] = 0x7F; $bytes[1] = 0x43; $bytes[2] = 0x4E; $bytes[3] = 0x54
    [System.Text.Encoding]::ASCII.GetBytes($contentId).CopyTo($bytes, 0x40)
    [System.IO.File]::WriteAllBytes($path, $bytes)
}

# a: the plain layout
$g = "$base\a\games"; Make-Game "$g\CUSA12392" $true
Check "a  games\CUSA12392\eboot.bin" (Find-Game $g) "$g\CUSA12392\eboot.bin"
if ($haveSfo) {
    $info = Get-GameInfo "$g\CUSA12392\eboot.bin"
    Check "a  param.sfo: serial" $info["TITLE_ID"] "CUSA12392"
    Check "a  param.sfo: version" $info["APP_VER"] "01.00"
    Check "a  param.sfo: name" $info["TITLE"] "ASTRO BOT Rescue Mission"
}

# b: a dump's folder inside a folder with brackets and spaces in its name
$g = "$base\b\games"; Make-Game "$g\[ABC] my dump (1)\CUSA12392-app0" $true
Check "b  two folders down, brackets in the name" (Find-Game $g) "$g\[ABC] my dump (1)\CUSA12392-app0\eboot.bin"

# c: two games, the one this is made for second
if ($haveSfo) {
    $g = "$base\c\games"; Make-Game "$g\Another" $false; Make-Game "$g\Zzz" $true
    Check "c  prefers CUSA12392 among two" (Find-Game $g) "$g\Zzz\eboot.bin"
}

# d: only the leftovers of an unpacking
$g = "$base\d\games"; Make-Game "$g\CUSA12392\.unpacking\files\uroot" $true
Check "d  half-unpacked game is not taken" (Find-Game $g) ""

# e: packages
$g = "$base\e\games"
$package = "$g\CUSA12392\[ABC]-Some.Game-CUSA12392-EUR-(1.00+)-PS4.pkg"
Make-Package $package "EP9000-CUSA12392_00-PLATFORMERVR00EU" 4096
Make-Package "$g\update.pkg" "EP9000-CUSA12392_00-PLATFORMERVR00EU" 1024
[System.IO.File]::WriteAllText("$g\notapackage.pkg", ("x" * 300))
Check "e  no unpacked game" (Find-Game $g) ""
Check "e  the largest package, brackets in its name" (Find-Package $g).FullName $package
Check "e  content id" (Read-PackageId $package) "EP9000-CUSA12392_00-PLATFORMERVR00EU"
Check "e  a file that is no package" (Read-PackageId "$g\notapackage.pkg") ""

# f: too deep, and an empty or missing games folder
$g = "$base\f\games"; Make-Game "$g\1\2\3\4" $true
Check "f  four folders down is not looked at" (Find-Game $g) ""
Check "f  missing folder" (Find-Game "$base\nowhere") ""
Check "f  missing folder, package" (Find-Package "$base\nowhere") ""

# g: what a path given by the player turns into
$g = "$base\a\games"
Check "g  a folder" (Use-Path "$base\a") "$g\CUSA12392\eboot.bin"
Check "g  an eboot.bin" (Use-Path "$g\CUSA12392\eboot.bin") "$g\CUSA12392\eboot.bin"
Check "g  nothing there" (Use-Path "$base\a\nothing.bin") ""

[System.IO.Directory]::Delete($base, $true)
"failed: $failed"
if ($failed -gt 0) { exit 1 }
