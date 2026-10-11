# Starts Until Dawn: Rush of Blood in the emulator for a headset connected to this PC (Virtual
# Desktop, or anything else with an OpenXR runtime), and tells what is going on while it runs.
# Started by "run.bat"; settings are in settings.txt next to this file, and the
# main ones can be chosen in a small window before the game starts.
param([string]$SettingsFile = "", [switch]$NoMenu)

$ErrorActionPreference = "Continue"
$here = Split-Path -Parent $MyInvocation.MyCommand.Path
$root = Split-Path -Parent $here
Set-Location $here
if ($SettingsFile -eq "") { $SettingsFile = Join-Path $here "settings.txt" }

function Say([string]$text, [string]$color = "Gray") { Write-Host $text -ForegroundColor $color }

# --- settings -------------------------------------------------------------------------------
function Read-Settings {
    $script:settings = [ordered]@{}
    $script:extraEnv = @()
    if (Test-Path $SettingsFile) {
        foreach ($line in Get-Content $SettingsFile) {
            $line = $line.Trim()
            if ($line -eq "" -or $line.StartsWith("#")) { continue }
            $at = $line.IndexOf("=")
            if ($at -lt 1) { continue }
            $key = $line.Substring(0, $at).Trim().ToLower()
            $value = $line.Substring($at + 1).Trim()
            if ($key -eq "env") { $script:extraEnv += $value } else { $script:settings[$key] = $value }
        }
    }
}
function Setting([string]$key, [string]$default = "") {
    if ($settings.Contains($key)) { return $settings[$key] }
    return $default
}
# Writes key=value into the settings file: in place of the line that sets it, or at the end.
function Save-Setting([string]$key, [string]$value) {
    $lines = @()
    if (Test-Path $SettingsFile) { $lines = @(Get-Content $SettingsFile) }
    $done = $false
    for ($i = 0; $i -lt $lines.Count; $i++) {
        if ($lines[$i] -match ("^\s*" + [regex]::Escape($key) + "\s*=")) {
            $lines[$i] = "$key=$value"
            $done = $true
        }
    }
    if (-not $done) { $lines += "$key=$value" }
    Set-Content -Path $SettingsFile -Value $lines -Encoding UTF8
}
Read-Settings

# The sizes an eye can be drawn at: the console's own (1152x1296: the game draws both eyes into
# one picture of 2304x1296) and larger, all the same shape (the height is 9/8 of the width).
$widths = @(1152, 1344, 1536, 1728, 1920, 2048, 2304, 2560, 2880)
$defaultWidth = 1152
function EyeHeight([int]$width) { return [int]($width * 9 / 8) }
$caps = @(120, 90, 72, 60, 45, 40, 36, 30)

Add-Type -AssemblyName System.Windows.Forms
Add-Type -AssemblyName System.Drawing
[System.Windows.Forms.Application]::EnableVisualStyles()

# A message box, on top of whatever else is on the desktop (which is what the headset shows).
# Returns the name of the button chosen: OK, Yes, No, Cancel.
function Show-Box([string]$text, [string]$buttons = "OK", [string]$icon = "Information",
                  [string]$default = "Button1") {
    $owner = New-Object System.Windows.Forms.Form
    $owner.TopMost = $true
    try {
        return [System.Windows.Forms.MessageBox]::Show($owner, $text, "RushVR", $buttons,
                                                       $icon, $default).ToString()
    } finally { $owner.Dispose() }
}

# Shows one of the launcher's windows and waits for it. In its normal size and in front, also
# when the launcher itself was started minimized (Windows would open its first window the same).
function Show-Form($form) {
    $form.Add_Shown({ $this.WindowState = "Normal"; $this.Activate() })
    return $form.ShowDialog()
}

# --- the game ---------------------------------------------------------------------------------
# The game is looked for in the games folder next to this one, as deep as three folders down.
# An unpacked game is a folder with eboot.bin in it; a package (.pkg) is unpacked first, once.
# game= in the settings names one that is elsewhere (its eboot.bin, its folder or its package),
# and when none is found a window asks where it is.
$gamesFolder = Join-Path $root "games"
$madeFor = "CUSA03683"
# The longest path of a file inside that game: the emulator cannot open a file whose whole path
# is longer than 259 characters.
$longestInside = 126
$zeroPasscode = "0" * 32
$unpackFolder = ".unpacking"

function Gigabytes([double]$bytes) { return ("{0:N1} GB" -f ($bytes / 1073741824.0)) }

# The folders under one, itself first and the nearest first, down to so many levels. What is
# inside an unpacked game, or one being unpacked, is left out.
function Get-Folders([string]$top, [int]$depth = 3) {
    $all = New-Object System.Collections.Generic.List[string]
    if (-not [System.IO.Directory]::Exists($top)) { return $all }
    $level = @($top)
    for ($i = 0; $i -le $depth -and $level.Count -gt 0; $i++) {
        $next = @()
        foreach ($folder in $level) {
            $all.Add($folder)
            if ([System.IO.File]::Exists([System.IO.Path]::Combine($folder, "eboot.bin"))) { continue }
            try {
                foreach ($sub in [System.IO.Directory]::GetDirectories($folder)) {
                    if ([System.IO.Path]::GetFileName($sub) -ne $unpackFolder) { $next += $sub }
                }
            } catch {}
        }
        $level = $next
    }
    return $all
}

# What a param.sfo says, by name (TITLE_ID, APP_VER, TITLE...): the texts only.
function Read-Sfo([string]$path) {
    $values = @{}
    try {
        $bytes = [System.IO.File]::ReadAllBytes($path)
        if ($bytes.Length -lt 20 -or [System.BitConverter]::ToUInt32($bytes, 0) -ne 0x46535000) {
            return $values
        }
        $keys = [System.BitConverter]::ToInt32($bytes, 8)
        $data = [System.BitConverter]::ToInt32($bytes, 12)
        $count = [System.BitConverter]::ToInt32($bytes, 16)
        for ($i = 0; $i -lt $count; $i++) {
            $at = 20 + 16 * $i
            $keyAt = $keys + [System.BitConverter]::ToUInt16($bytes, $at)
            $format = [System.BitConverter]::ToUInt16($bytes, $at + 2)
            $length = [System.BitConverter]::ToInt32($bytes, $at + 4)
            $valueAt = $data + [System.BitConverter]::ToInt32($bytes, $at + 12)
            $end = [Array]::IndexOf($bytes, [byte]0, $keyAt)
            $key = [System.Text.Encoding]::ASCII.GetString($bytes, $keyAt, $end - $keyAt)
            if ($format -eq 0x0204 -and $length -gt 0) {
                $values[$key] = [System.Text.Encoding]::UTF8.GetString($bytes, $valueAt, $length - 1)
            }
        }
    } catch {}
    return $values
}
function Get-GameInfo([string]$eboot) {
    $folder = [System.IO.Path]::GetDirectoryName($eboot)
    return Read-Sfo ([System.IO.Path]::Combine($folder, "sce_sys", "param.sfo"))
}

# The unpacked game under a folder: the one this is made for, if there are several.
function Find-Game([string]$top) {
    $first = $null
    foreach ($folder in (Get-Folders $top)) {
        $eboot = [System.IO.Path]::Combine($folder, "eboot.bin")
        if (-not [System.IO.File]::Exists($eboot)) { continue }
        if ((Get-GameInfo $eboot)["TITLE_ID"] -eq $madeFor) { return $eboot }
        if ($null -eq $first) { $first = $eboot }
    }
    return $first
}

# The package under a folder: the largest, if there are several (a game's is larger than its
# updates').
function Find-Package([string]$top) {
    $largest = $null
    foreach ($folder in (Get-Folders $top)) {
        try { $files = [System.IO.Directory]::GetFiles($folder, "*.pkg") } catch { continue }
        foreach ($file in $files) {
            $info = New-Object System.IO.FileInfo($file)
            if ($null -eq $largest -or $info.Length -gt $largest.Length) { $largest = $info }
        }
    }
    return $largest
}

# What a package calls its content ("UP9000-CUSA03683_00-..."), or nothing if the file is not
# a PlayStation 4 package.
function Read-PackageId([string]$path) {
    try {
        $head = New-Object byte[] 128
        $stream = [System.IO.File]::OpenRead($path)
        try { $read = $stream.Read($head, 0, $head.Length) } finally { $stream.Dispose() }
        if ($read -lt $head.Length) { return $null }
        if ($head[0] -ne 0x7F -or $head[1] -ne 0x43 -or $head[2] -ne 0x4E -or $head[3] -ne 0x54) {
            return $null
        }
        return [System.Text.Encoding]::ASCII.GetString($head, 0x40, 36).Trim([char]0)
    } catch { return $null }
}

# The window shown while a package is unpacked. True when the unpacking ran to its end, false
# when it was cancelled (and stopped).
function Show-Unpacking($process, [string]$drive, [double]$freeBefore) {
    $form = New-Object System.Windows.Forms.Form
    $form.Text = "RushVR"
    $form.ClientSize = New-Object System.Drawing.Size(460, 132)
    $form.StartPosition = "CenterScreen"
    $form.FormBorderStyle = "FixedDialog"
    $form.MaximizeBox = $false
    $form.MinimizeBox = $false
    $form.ControlBox = $false
    $form.TopMost = $true
    $form.Font = New-Object System.Drawing.Font("Segoe UI", 9.5)

    $label = New-Object System.Windows.Forms.Label
    $label.Text = "Unpacking the game. This is done once and takes a minute or a few."
    $label.SetBounds(16, 14, 428, 22)
    $form.Controls.Add($label)
    $bar = New-Object System.Windows.Forms.ProgressBar
    $bar.Style = "Marquee"
    $bar.MarqueeAnimationSpeed = 30
    $bar.SetBounds(16, 44, 428, 18)
    $form.Controls.Add($bar)
    $state = New-Object System.Windows.Forms.Label
    $state.Name = "state"
    $state.SetBounds(16, 72, 300, 22)
    $form.Controls.Add($state)
    $cancel = New-Object System.Windows.Forms.Button
    $cancel.Name = "cancel"
    $cancel.Text = "Cancel"
    $cancel.SetBounds(356, 92, 88, 30)
    $form.Controls.Add($cancel)

    $timer = New-Object System.Windows.Forms.Timer
    $timer.Interval = 500
    $timer.Add_Tick({
        if ($process.HasExited) {
            $timer.Stop()
            $form.DialogResult = [System.Windows.Forms.DialogResult]::OK
            $form.Close()
            return
        }
        if ($freeBefore -ge 0) {
            try {
                $free = (New-Object System.IO.DriveInfo($drive)).AvailableFreeSpace
                $state.Text = (Gigabytes ([math]::Max(0, $freeBefore - $free))) + " unpacked"
            } catch {}
        }
    })
    $cancel.Add_Click({
        $timer.Stop()
        try { $process.Kill() } catch {}
        try { $process.WaitForExit(10000) | Out-Null } catch {}
        $form.DialogResult = [System.Windows.Forms.DialogResult]::Cancel
        $form.Close()
    })
    $form.Add_Shown({ $timer.Start() })
    $result = Show-Form $form
    $timer.Dispose()
    $form.Dispose()
    return ($result -eq [System.Windows.Forms.DialogResult]::OK)
}

# Unpacks a package into the games folder, under the game's serial, and returns the eboot.bin
# there; nothing if it was not done. PkgTool (of LibOrbisPkg) does the work. It reads packages
# that are not encrypted, which is what a dump of a game is; what the PlayStation Store hands
# out is encrypted and cannot be unpacked by anything here.
function Expand-Package($package) {
    $contentId = Read-PackageId $package.FullName
    if ($null -eq $contentId) {
        [void](Show-Box ($package.FullName + "`n`nis not a PlayStation 4 package.") "OK" "Warning")
        return $null
    }
    $tool = $null
    foreach ($candidate in @((Join-Path $here "pkgtool\PkgTool.exe"),
                             (Join-Path $root "tools\pkgtool\PkgTool.exe"))) {
        if ([System.IO.File]::Exists($candidate)) { $tool = $candidate; break }
    }
    if ($null -eq $tool) {
        [void](Show-Box ("The game is here as a package:`n" + $package.FullName + "`n`nbut PkgTool, which unpacks packages, is missing from`n" + (Join-Path $here "pkgtool") + "`n`nUnzip the whole RushVR package again.") "OK" "Warning")
        return $null
    }
    $serial = "game"
    if ($contentId -match "[A-Z]{4}[0-9]{5}") { $serial = $Matches[0] }
    $target = Join-Path $gamesFolder $serial
    if ($target.Length + 1 + $longestInside -gt 259) {
        [void](Show-Box ("The game cannot be unpacked into`n" + $target + "`n`nThat path is too long: some of the game's files would have a path of more than 259 characters, which the emulator cannot open. Move the RushVR folder somewhere with a shorter path, for example C:\Games\RushVR, and start again.") "OK" "Warning")
        return $null
    }
    # What unpacking takes, by this game's own measure (12.5 GB out of a package of 6.9).
    $needed = $package.Length * 1.85
    $drive = [System.IO.Path]::GetPathRoot($target)
    $free = -1
    try { $free = (New-Object System.IO.DriveInfo($drive)).AvailableFreeSpace } catch {}
    if ($free -ge 0 -and $free -lt $needed) {
        [void](Show-Box ("Unpacking the game takes about " + (Gigabytes $needed) + ", and drive " + $drive + " has " + (Gigabytes $free) + " free.`n`nMake room, or move the RushVR folder to a drive that has it.") "OK" "Warning")
        return $null
    }
    $answer = Show-Box ("The game is here as a package:`n`n" + $package.Name + "   (" + (Gigabytes $package.Length) + ")`n`nIt has to be unpacked before it can be played. That is done once, takes a minute or a few, and about " + (Gigabytes $needed) + " in`n" + $target + "`n`nUnpack it now?") "YesNo" "Question"
    if ($answer -ne "Yes") { return $null }

    # Unpacked into a folder of its own first: what is left of an unpacking that did not finish
    # is never taken for the game.
    $work = Join-Path $target $unpackFolder
    try {
        if ([System.IO.Directory]::Exists($work)) { [System.IO.Directory]::Delete($work, $true) }
        [void][System.IO.Directory]::CreateDirectory($work)
    } catch {
        [void](Show-Box ("Could not write to`n" + $target + "`n`n" + $_.Exception.Message) "OK" "Warning")
        return $null
    }
    $errors = Join-Path $work "pkgtool-errors.txt"
    $arguments = "pkg_extract --passcode " + $zeroPasscode + " `"" + $package.FullName + "`" `"" + (Join-Path $work "files") + "`""
    Say ("Unpacking " + $package.FullName)
    $process = Start-Process -FilePath $tool -ArgumentList $arguments -PassThru -WindowStyle Hidden `
        -RedirectStandardOutput (Join-Path $work "pkgtool-output.txt") -RedirectStandardError $errors
    # (Without this the exit code is not to be had later.)
    $null = $process.Handle
    $finished = Show-Unpacking $process $drive $free

    $unpacked = Join-Path $work "files\uroot"
    if (-not [System.IO.Directory]::Exists($unpacked)) { $unpacked = Join-Path $work "files" }
    $failure = $null
    if (-not $finished) {
        $failure = "cancelled"
    } elseif ($process.ExitCode -ne 0 -or -not [System.IO.File]::Exists((Join-Path $unpacked "eboot.bin"))) {
        $why = ""
        try { $why = (@(Get-Content -LiteralPath $errors -ErrorAction Stop | Where-Object { $_.Trim() -ne "" })[0]) } catch {}
        $failure = "The package could not be unpacked.`n`nOnly a package made from a dump of the game can be: it is not encrypted. A package from the PlayStation Store is, and cannot be used.`n`n" + $package.FullName
        if ($why) { $failure += "`n`n(PkgTool: " + $why.Trim() + ")" }
    } else {
        try {
            # Into place: over anything of the same name that an earlier attempt left there.
            foreach ($item in [System.IO.Directory]::GetFileSystemEntries($unpacked)) {
                $to = Join-Path $target ([System.IO.Path]::GetFileName($item))
                if ([System.IO.Directory]::Exists($to)) { [System.IO.Directory]::Delete($to, $true) }
                elseif ([System.IO.File]::Exists($to)) { [System.IO.File]::Delete($to) }
                [System.IO.Directory]::Move($item, $to)
            }
            # What the package keeps outside its file system: the game's own description
            # (param.sfo, which tells the emulator what game this is), pictures, trophies.
            # Named ICON0_PNG, PLAYGO_CHUNK_DAT, TROPHY__TROPHY00_TRP... there; icon0.png,
            # playgo-chunk.dat, trophy/trophy00.trp in the game's sce_sys folder.
            $leftOut = @("DIGESTS", "ENTRY_KEYS", "IMAGE_KEY", "GENERAL_DIGESTS", "METAS",
                         "ENTRY_NAMES", "LICENSE_DAT", "LICENSE_INFO")
            foreach ($line in (& $tool pkg_listentries $package.FullName 2>$null)) {
                if ($line -notmatch '^0x[0-9A-Fa-f]+\s+0x[0-9A-Fa-f]+\s+[0-9A-Fa-f]+\s+(\d+)\s+(?:\d+\s+)?([A-Z0-9_]+)\s*$') { continue }
                $index = $Matches[1]
                $name = $Matches[2]
                if ($leftOut -contains $name -or $name.EndsWith("_DDS")) { continue }
                $file = $name.ToLower().Replace("__", "\")
                $at = $file.LastIndexOf("_")
                if ($at -gt 0) { $file = $file.Substring(0, $at) + "." + $file.Substring($at + 1) }
                if ($file.StartsWith("playgo_")) { $file = "playgo-" + $file.Substring(7) }
                $file = Join-Path (Join-Path $target "sce_sys") $file
                [void][System.IO.Directory]::CreateDirectory([System.IO.Path]::GetDirectoryName($file))
                & $tool pkg_extractentry --passcode $zeroPasscode $package.FullName $index $file 2>$null | Out-Null
            }
            if (-not [System.IO.File]::Exists((Join-Path $target "sce_sys\param.sfo"))) {
                $failure = "The package was unpacked, but its description of the game (param.sfo) could not be read from it.`n`n" + $package.FullName
            }
        } catch {
            $failure = "The package was unpacked, but the game could not be put in`n" + $target + "`n`n" + $_.Exception.Message
        }
    }
    try { [System.IO.Directory]::Delete($work, $true) } catch {}
    if ($failure) {
        if ($failure -ne "cancelled") { [void](Show-Box $failure "OK" "Warning") }
        return $null
    }
    Say ("Unpacked into " + $target)
    $answer = Show-Box ("The game is unpacked and ready.`n`nThe package is not needed any more:`n" + $package.FullName + "`n`nDelete it, to get " + (Gigabytes $package.Length) + " back? (Keep it if it is your only copy of the game.)") "YesNo" "Question" "Button2"
    if ($answer -eq "Yes") {
        try { [System.IO.File]::Delete($package.FullName) } catch {
            [void](Show-Box ("Could not delete the package:`n" + $_.Exception.Message) "OK" "Warning")
        }
    }
    return (Join-Path $target "eboot.bin")
}

# What a path named in the settings, or picked in the window, gives: the eboot.bin to run, or
# nothing.
function Use-Path([string]$path) {
    if ([System.IO.Directory]::Exists($path)) {
        $eboot = Find-Game $path
        if ($eboot) { return $eboot }
        $package = Find-Package $path
        if ($package) { return Expand-Package $package }
        return $null
    }
    if (-not [System.IO.File]::Exists($path)) { return $null }
    if ([System.IO.Path]::GetExtension($path) -ieq ".pkg") {
        return Expand-Package (New-Object System.IO.FileInfo($path))
    }
    return $path
}

# The window for when the game is nowhere to be found. Returns pick (show where it is), again
# (look again) or quit.
function Show-NotFound {
    $form = New-Object System.Windows.Forms.Form
    $form.Text = "RushVR"
    $form.ClientSize = New-Object System.Drawing.Size(600, 250)
    $form.StartPosition = "CenterScreen"
    $form.FormBorderStyle = "FixedDialog"
    $form.MaximizeBox = $false
    $form.MinimizeBox = $false
    $form.TopMost = $true
    $form.Font = New-Object System.Drawing.Font("Segoe UI", 9.5)

    $title = New-Object System.Windows.Forms.Label
    $title.Text = "Where is the game?"
    $title.Font = New-Object System.Drawing.Font("Segoe UI", 12, [System.Drawing.FontStyle]::Bold)
    $title.SetBounds(16, 14, 568, 26)
    $form.Controls.Add($title)
    $text = New-Object System.Windows.Forms.Label
    $text.Text = "Until Dawn: Rush of Blood was not found. RushVR does not contain the game: it plays your own copy of it.`n`nPut that copy in the games folder - either the game's folder (the one with eboot.bin in it) or its .pkg file - and choose Look again. Or leave it where it is and show where that is."
    $text.SetBounds(16, 50, 568, 96)
    $form.Controls.Add($text)
    $where = New-Object System.Windows.Forms.Label
    $where.Text = "The games folder: " + $gamesFolder
    $where.ForeColor = [System.Drawing.SystemColors]::GrayText
    $where.SetBounds(16, 150, 568, 40)
    $form.Controls.Add($where)

    $pick = New-Object System.Windows.Forms.Button
    $pick.Name = "pick"
    $pick.Text = "Show where it is..."
    $pick.SetBounds(16, 204, 150, 30)
    $pick.DialogResult = [System.Windows.Forms.DialogResult]::Yes
    $form.Controls.Add($pick)
    $open = New-Object System.Windows.Forms.Button
    $open.Name = "open"
    $open.Text = "Open the games folder"
    $open.SetBounds(174, 204, 170, 30)
    $open.Add_Click({
        try {
            [void][System.IO.Directory]::CreateDirectory($gamesFolder)
            Start-Process explorer.exe -ArgumentList ("`"" + $gamesFolder + "`"")
        } catch {}
    })
    $form.Controls.Add($open)
    $again = New-Object System.Windows.Forms.Button
    $again.Name = "again"
    $again.Text = "Look again"
    $again.SetBounds(392, 204, 96, 30)
    $again.DialogResult = [System.Windows.Forms.DialogResult]::Retry
    $form.Controls.Add($again)
    $quit = New-Object System.Windows.Forms.Button
    $quit.Name = "quit"
    $quit.Text = "Quit"
    $quit.SetBounds(496, 204, 88, 30)
    $quit.DialogResult = [System.Windows.Forms.DialogResult]::Cancel
    $form.Controls.Add($quit)
    $form.AcceptButton = $again
    $form.CancelButton = $quit

    $result = Show-Form $form
    $form.Dispose()
    if ($result -eq [System.Windows.Forms.DialogResult]::Yes) { return "pick" }
    if ($result -eq [System.Windows.Forms.DialogResult]::Retry) { return "again" }
    return "quit"
}

# The file picker: the game's eboot.bin or its package, wherever they are.
function Select-GameFile {
    $dialog = New-Object System.Windows.Forms.OpenFileDialog
    $dialog.Title = "Where is Until Dawn: Rush of Blood? Choose its eboot.bin, or its .pkg file"
    $dialog.Filter = "The game (eboot.bin, *.pkg)|eboot.bin;*.pkg|All files (*.*)|*.*"
    $dialog.CheckFileExists = $true
    if ([System.IO.Directory]::Exists($gamesFolder)) { $dialog.InitialDirectory = $gamesFolder }
    $owner = New-Object System.Windows.Forms.Form
    $owner.TopMost = $true
    try {
        if ($dialog.ShowDialog($owner) -eq [System.Windows.Forms.DialogResult]::OK) {
            return $dialog.FileName
        }
        return $null
    } finally {
        $owner.Dispose()
        $dialog.Dispose()
    }
}

# The eboot.bin to run, or nothing when the player gives up.
function Resolve-Game {
    $named = Setting "game"
    if ($named -ne "") {
        $eboot = Use-Path $named
        if ($eboot) { return $eboot }
        Say ("The settings name the game at " + $named + ", where it is not: looking in " + $gamesFolder) "Yellow"
    }
    while ($true) {
        $eboot = Find-Game $gamesFolder
        if ($eboot) { return $eboot }
        $package = Find-Package $gamesFolder
        if ($package) {
            $eboot = Expand-Package $package
            if ($eboot) { return $eboot }
        }
        $choice = Show-NotFound
        if ($choice -eq "pick") {
            $file = Select-GameFile
            if ($file) {
                $eboot = Use-Path $file
                if ($eboot) {
                    # In the games folder it is found again by itself; elsewhere it is noted.
                    $inGames = $eboot.StartsWith($gamesFolder + "\", [System.StringComparison]::OrdinalIgnoreCase)
                    if (-not $inGames) { Save-Setting "game" $eboot }
                    return $eboot
                }
            }
        } elseif ($choice -ne "again") {
            return $null
        }
    }
}

# The Microsoft Visual C++ runtime, which the emulator is built against.
function Test-Runtime {
    $system = [System.Environment]::SystemDirectory
    foreach ($name in @("vcruntime140.dll", "vcruntime140_1.dll", "msvcp140.dll",
                        "msvcp140_2.dll", "msvcp140_atomic_wait.dll")) {
        if (-not [System.IO.File]::Exists((Join-Path $system $name))) { return $false }
    }
    return $true
}

# --- the window -------------------------------------------------------------------------------
function Show-Menu {

    $form = New-Object System.Windows.Forms.Form
    $form.Text = "RushVR"
    $form.ClientSize = New-Object System.Drawing.Size(560, 488)
    $form.StartPosition = "CenterScreen"
    $form.FormBorderStyle = "FixedDialog"
    $form.MaximizeBox = $false
    $form.MinimizeBox = $false
    $form.TopMost = $true
    $form.Font = New-Object System.Drawing.Font("Segoe UI", 9.5)

    $y = 14
    $title = New-Object System.Windows.Forms.Label
    $title.Text = "Until Dawn: Rush of Blood - PC VR"
    $title.Font = New-Object System.Drawing.Font("Segoe UI", 12, [System.Drawing.FontStyle]::Bold)
    $title.SetBounds(16, $y, 520, 26)
    $form.Controls.Add($title)
    $y += 40

    # Headset connection.
    $label = New-Object System.Windows.Forms.Label
    $label.Text = "Headset connection (OpenXR runtime)"
    $label.Font = New-Object System.Drawing.Font("Segoe UI", 9.5, [System.Drawing.FontStyle]::Bold)
    $label.SetBounds(16, $y, 520, 20)
    $form.Controls.Add($label)
    $y += 24
    $runtimeNames = @("As the PC has it set up", "Virtual Desktop", "SteamVR")
    $runtimeKeys = @("pc", "virtualdesktop", "steamvr")
    $runtimeBox = New-Object System.Windows.Forms.ComboBox
    $runtimeBox.DropDownStyle = "DropDownList"
    foreach ($name in $runtimeNames) { [void]$runtimeBox.Items.Add($name) }
    $runtimeBox.SetBounds(16, $y, 240, 26)
    $index = [array]::IndexOf($runtimeKeys, (Setting "runtime" "pc"))
    if ($index -lt 0) { $index = 0 }
    $runtimeBox.SelectedIndex = $index
    $form.Controls.Add($runtimeBox)
    $y += 32
    $runtimeText = New-Object System.Windows.Forms.Label
    $runtimeText.SetBounds(16, $y, 530, 44)
    $form.Controls.Add($runtimeText)
    $updateRuntime = {
        $runtimeText.Text = switch ($runtimeBox.SelectedIndex) {
            1 { "Through Virtual Desktop's streamer, which is started if it is not running. In the headset, connect Virtual Desktop to this PC." }
            2 { "Through SteamVR, which is started if it is not running: the headset has to be connected to it first. Not tested with this game yet." }
            default { "Whichever runtime the PC has set as its OpenXR runtime (Virtual Desktop's streamer sets itself when it starts)." }
        }
    }
    $runtimeBox.Add_SelectedIndexChanged($updateRuntime)
    & $updateRuntime
    $y += 48

    # Resolution.
    $label = New-Object System.Windows.Forms.Label
    $label.Text = "Resolution of each eye"
    $label.Font = New-Object System.Drawing.Font("Segoe UI", 9.5, [System.Drawing.FontStyle]::Bold)
    $label.SetBounds(16, $y, 520, 20)
    $form.Controls.Add($label)
    $y += 22
    $resolution = New-Object System.Windows.Forms.TrackBar
    $resolution.Minimum = 0
    $resolution.Maximum = $widths.Count - 1
    $resolution.TickFrequency = 1
    $resolution.LargeChange = 1
    $resolution.SetBounds(12, $y, 530, 40)
    $current = 0
    [void][int]::TryParse((Setting "eye_width" "$defaultWidth"), [ref]$current)
    $index = [array]::IndexOf($widths, $current)
    if ($index -lt 0) { $index = [array]::IndexOf($widths, $defaultWidth) }
    $resolution.Value = $index
    $form.Controls.Add($resolution)
    $y += 42
    $resolutionText = New-Object System.Windows.Forms.Label
    $resolutionText.SetBounds(16, $y, 530, 38)
    $form.Controls.Add($resolutionText)
    $update = {
        $w = $widths[$resolution.Value]
        $h = EyeHeight $w
        $times = ($w * $h) / (1152.0 * 1296.0)
        $what = if ($w -eq 1152) { "the console's own: the sharpness of the PlayStation 4" } elseif ($w -gt 2048) { "{0:N1} times the pixels of the console's: very demanding, it needs a lot of graphics memory and about {1:N1} GB more memory" -f $times, (($times - 1) * 0.64) } else { "{0:N2} times the pixels of the console's: more work for the graphics card, and more memory" -f $times }
        $resolutionText.Text = "$w x $h pixels for each eye: $what. Start lower if the game stutters."
    }
    $resolution.Add_ValueChanged($update)
    & $update
    $y += 46

    # Field of view.
    $label = New-Object System.Windows.Forms.Label
    $label.Text = "Field of view"
    $label.Font = New-Object System.Drawing.Font("Segoe UI", 9.5, [System.Drawing.FontStyle]::Bold)
    $label.SetBounds(16, $y, 520, 20)
    $form.Controls.Add($label)
    $y += 22
    $fov = New-Object System.Windows.Forms.TrackBar
    $fov.Minimum = 14
    $fov.Maximum = 32
    $fov.TickFrequency = 1
    $fov.LargeChange = 1
    $fov.SetBounds(12, $y, 300, 40)
    $fov.Value = [math]::Max(14, [math]::Min(32, [int]([int](Setting "fov" "120") / 5)))
    $form.Controls.Add($fov)
    $fovText = New-Object System.Windows.Forms.Label
    $fovText.SetBounds(316, $y + 4, 230, 40)
    $form.Controls.Add($fovText)
    $ofPsvr = (Setting "fov_of" "psvr") -eq "psvr"
    $updateFov = {
        $percent = $fov.Value * 5
        if ($percent -eq 100) {
            $fovText.Text = $(if ($ofPsvr) { "100%: PlayStation VR's own" } else { "100%: all that the headset shows" })
        } elseif ($percent -eq 120 -and $ofPsvr) {
            $fovText.Text = "120%: the smoothest picture, no borders (the corners are black)"
        } elseif ($percent -eq 155 -and $ofPsvr) {
            $fovText.Text = "155%: the Quest 3's own, no lens corners in view"
        } elseif ($percent -gt 100) {
            $fovText.Text = "$percent% of it: the game draws a wider view (softer picture, no border)"
        } else {
            $fovText.Text = "$percent% of it: sharper, with a dark border"
        }
    }
    $fov.Add_ValueChanged($updateFov)
    & $updateFov
    $y += 46

    # Edges and corners.
    $label = New-Object System.Windows.Forms.Label
    $label.Text = "Picture quality"
    $label.Font = New-Object System.Drawing.Font("Segoe UI", 9.5, [System.Drawing.FontStyle]::Bold)
    $label.SetBounds(16, $y, 520, 20)
    $form.Controls.Add($label)
    $y += 26

    $fxaaValues = @("0", "0.35", "0.65", "1")
    $fxaaNames = @("Off", "Light", "Normal", "Strong")
    $label = New-Object System.Windows.Forms.Label
    $label.Text = "Smooth edges (FXAA)"
    $label.SetBounds(16, $y + 3, 170, 22)
    $form.Controls.Add($label)
    $fxaaBox = New-Object System.Windows.Forms.ComboBox
    $fxaaBox.DropDownStyle = "DropDownList"
    foreach ($name in $fxaaNames) { [void]$fxaaBox.Items.Add($name) }
    $fxaaBox.SetBounds(190, $y, 300, 26)
    $fxaaNow = 0.65
    [void][double]::TryParse((Setting "fxaa" "0.65"), [System.Globalization.NumberStyles]::Float, [System.Globalization.CultureInfo]::InvariantCulture, [ref]$fxaaNow)
    $best = 2
    $bestDistance = 1000.0
    for ($i = 0; $i -lt $fxaaValues.Count; $i++) {
        $distance = [math]::Abs([double]::Parse($fxaaValues[$i], [System.Globalization.CultureInfo]::InvariantCulture) - $fxaaNow)
        if ($distance -lt $bestDistance) { $best = $i; $bestDistance = $distance }
    }
    $fxaaBox.SelectedIndex = $best
    $form.Controls.Add($fxaaBox)
    $y += 32

    $maskValues = @("0", "0.94", "0.97", "1", "1.05")
    $maskNames = @("Off: the coloured corners show", "Tighter (0.94): if colour is still left at the edge", "Tight (0.97): if some colour is left at the edge", "On (1.0): black corners outside the lens circles", "Loose (1.05): if the picture's edge is cut off")
    $label = New-Object System.Windows.Forms.Label
    $label.Text = "Black corners (mask)"
    $label.SetBounds(16, $y + 3, 170, 22)
    $form.Controls.Add($label)
    $maskBox = New-Object System.Windows.Forms.ComboBox
    $maskBox.DropDownStyle = "DropDownList"
    foreach ($name in $maskNames) { [void]$maskBox.Items.Add($name) }
    $maskBox.SetBounds(190, $y, 300, 26)
    $maskNow = 1.0
    [void][double]::TryParse((Setting "mask" "1"), [System.Globalization.NumberStyles]::Float, [System.Globalization.CultureInfo]::InvariantCulture, [ref]$maskNow)
    $best = 3
    $bestDistance = 1000.0
    for ($i = 0; $i -lt $maskValues.Count; $i++) {
        $distance = [math]::Abs([double]::Parse($maskValues[$i], [System.Globalization.CultureInfo]::InvariantCulture) - $maskNow)
        if ($distance -lt $bestDistance) { $best = $i; $bestDistance = $distance }
    }
    $maskBox.SelectedIndex = $best
    $form.Controls.Add($maskBox)
    $y += 34

    $again = New-Object System.Windows.Forms.CheckBox
    $again.Text = "Show this window at every start"
    $again.Checked = (Setting "menu" "1") -ne "0"
    $again.SetBounds(16, $y, 300, 24)
    $form.Controls.Add($again)

    $play = New-Object System.Windows.Forms.Button
    $play.Text = "Play"
    $play.SetBounds(360, $y - 2, 88, 30)
    $play.DialogResult = [System.Windows.Forms.DialogResult]::OK
    $form.Controls.Add($play)
    $form.AcceptButton = $play
    $quit = New-Object System.Windows.Forms.Button
    $quit.Text = "Quit"
    $quit.SetBounds(456, $y - 2, 88, 30)
    $quit.DialogResult = [System.Windows.Forms.DialogResult]::Cancel
    $form.Controls.Add($quit)
    $form.CancelButton = $quit

    $result = Show-Form $form
    if ($result -ne [System.Windows.Forms.DialogResult]::OK) { return $false }
    Save-Setting "runtime" ($runtimeKeys[$runtimeBox.SelectedIndex])
    Save-Setting "eye_width" ($widths[$resolution.Value])
    Save-Setting "fxaa" ($fxaaValues[$fxaaBox.SelectedIndex])
    Save-Setting "mask" ($maskValues[$maskBox.SelectedIndex])
    Save-Setting "fov" ($fov.Value * 5)
    Save-Setting "menu" ($(if ($again.Checked) { "1" } else { "0" }))
    Read-Settings
    return $true
}

$emulator = Join-Path $here "shadps4.exe"
if (-not [System.IO.File]::Exists($emulator)) {
    [void](Show-Box ("The emulator, shadps4.exe, is missing from`n" + $here + "`n`nUnzip the whole RushVR package again. (Built from the source: run tools/make-pc-vr.sh.)") "OK" "Error")
    exit 1
}
if (-not (Test-Runtime)) {
    $answer = Show-Box "The emulator needs the Microsoft Visual C++ runtime, which is not installed on this PC.`n`nDownload its installer from Microsoft now? Run it, then start RushVR again." "YesNo" "Warning"
    if ($answer -eq "Yes") { Start-Process "https://aka.ms/vs/17/release/vc_redist.x64.exe" }
    exit 1
}

$game = Resolve-Game
if (-not $game) { exit 0 }
Say ("The game: " + $game)
$info = Get-GameInfo $game
if ($info.Count -eq 0) {
    Say "sce_sys\param.sfo is missing next to it: this is not a complete copy of the game, and the emulator may not know it." "Yellow"
} elseif ($info["TITLE_ID"] -ne $madeFor -or $info["APP_VER"] -ne "01.00") {
    Say ("This is " + $info["TITLE"] + ", " + $info["TITLE_ID"] + " version " + $info["APP_VER"] + ". RushVR is made for the US release, " + $madeFor + " version 01.00: with another, its fixes for the game's speed and picture do not apply, and it may not run.") "Yellow"
} elseif ([System.IO.Path]::GetDirectoryName($game).Length + 1 + $longestInside -gt 259) {
    [void](Show-Box ("The game is in`n" + [System.IO.Path]::GetDirectoryName($game) + "`n`nThat path is too long: some of the game's files have a path of more than 259 characters there, which the emulator cannot open, and the game would stop when it needs them. Move the folder somewhere with a shorter path, for example C:\Games\RushVR, and start again.") "OK" "Warning")
    exit 1
}

if (-not $NoMenu -and (Setting "menu" "1") -ne "0") {
    if (-not (Show-Menu)) { exit 0 }
}

# What the settings mean to the emulator.
# eye_width: the width of an eye (1152 the console's; larger ones are the game's picture grown,
# with the memory that takes, up to 2880).
$width = 0
if (-not [int]::TryParse((Setting "eye_width" "$defaultWidth"), [ref]$width)) { $width = $defaultWidth }
$width = [math]::Max(1152, [math]::Min(2880, [int]([math]::Round($width / 16) * 16)))
if ($width -gt 1152) { $env:SHADPS4_ROB_EYE_WIDTH = "$width" }
$env:SHADPS4_VR_SHARPEN = Setting "sharpen" "0.3"
# mask: the coloured corners outside the two lens circles go black (0 off, 1 as drawn).
$mask = 0.0
if (-not [double]::TryParse((Setting "mask" "1"), [System.Globalization.NumberStyles]::Float, [System.Globalization.CultureInfo]::InvariantCulture, [ref]$mask)) { $mask = 1.0 }
$mask = [math]::Max(0.0, [math]::Min(2.0, $mask))
if ($mask -gt 0) { $env:SHADPS4_VR_MASK = $mask.ToString([System.Globalization.CultureInfo]::InvariantCulture) }
# fxaa: smoothing of the picture's edges (0 off, up to 1). The game draws without multisampling, so
# msaa and antialias have nothing to work on here.
$fxaa = 0.65
if (-not [double]::TryParse((Setting "fxaa" "0.65"), [System.Globalization.NumberStyles]::Float, [System.Globalization.CultureInfo]::InvariantCulture, [ref]$fxaa)) { $fxaa = 0.65 }
$fxaa = [math]::Max(0.0, [math]::Min(1.0, $fxaa))
if ($fxaa -gt 0) { $env:SHADPS4_VR_FXAA = $fxaa.ToString([System.Globalization.CultureInfo]::InvariantCulture) }
if ((Setting "msaa") -ne "") { $env:SHADPS4_MAX_MSAA = Setting "msaa" }
if ((Setting "antialias" "1") -eq "0") { $env:SHADPS4_RESOLVE_AA = "0" }
if ((Setting "hands" "1") -eq "0") { $env:SHADPS4_XR_HANDS = "0" }
if ((Setting "predict_ms") -ne "") { $env:SHADPS4_XR_PREDICT_MS = Setting "predict_ms" }
if ((Setting "stick_touchpad" "1") -eq "0") { $env:SHADPS4_STICK_TOUCHPAD = "0" }
if ((Setting "surround" "1") -eq "0") { $env:SHADPS4_VIRTUAL_SURROUND = "0" }
if ((Setting "real_time" "1") -eq "0") { $env:SHADPS4_TITLE_TIMESTEP = "0" }
$fovSetting = Setting "fov" "120"
if ($fovSetting -ne "100") { $env:SHADPS4_VR_FOV = $fovSetting }
# fov_of: what fov is a percent of. headset: what the headset being worn shows, all of it at 100
# (the emulator asks the headset as it starts). psvr: a PlayStation VR's, as the game was made.
if ((Setting "fov_of" "psvr") -ne "psvr") { $env:SHADPS4_VR_FOV_OF = "headset" }
# fps: the most frames a second. (pace, the older way to say it: refreshes of the headset a
# frame is given, 1 or more.)
$env:SHADPS4_VR_FPS_CAP = Setting "fps" "60"
$pace = Setting "pace" ""
if ($pace -eq "1") { $env:SHADPS4_VR_FASTEST_PACE = "1"; $env:SHADPS4_VR_FPS_CAP = "" } elseif ($pace -ne "" -and $pace -ne "2") { $env:SHADPS4_VR_PACE = $pace }
if ((Setting "headset" "1") -eq "0") { $env:SHADPS4_OPENXR = "0" }
if ((Setting "pause" "1") -eq "0") { $env:SHADPS4_XR_PAUSE = "0" }
$env:SHADPS4_XR_WAIT = Setting "wait" "60"
foreach ($pair in $extraEnv) {
    $at = $pair.IndexOf("=")
    if ($at -ge 1) { Set-Item -Path ("Env:" + $pair.Substring(0, $at)) -Value $pair.Substring($at + 1) }
}

# --- what is there ----------------------------------------------------------------------------
Say "Until Dawn: Rush of Blood - PC VR" "Cyan"
if ($env:SHADPS4_ROB_EYE_WIDTH) {
    Say ("Each eye " + $env:SHADPS4_ROB_EYE_WIDTH + " x " + (EyeHeight ([int]$env:SHADPS4_ROB_EYE_WIDTH)) + " pixels (the console's: 1152 x 1296).")
}
# runtime: which OpenXR runtime the game uses. pc (or left out): the one the PC has set up
# (Virtual Desktop's, when its streamer was the last to set itself). virtualdesktop: Virtual
# Desktop's. steamvr: SteamVR's. Or the full path of a runtime's .json file.
function Find-OpenXrRuntime([string]$pattern) {
    foreach ($hive in @('HKLM:\SOFTWARE\Khronos\OpenXR\1\AvailableRuntimes', 'HKCU:\SOFTWARE\Khronos\OpenXR\1\AvailableRuntimes')) {
        try {
            foreach ($name in (Get-Item $hive -ErrorAction Stop).GetValueNames()) {
                if ($name -match $pattern -and (Test-Path $name)) { return $name }
            }
        } catch {}
    }
    return ""
}
$runtimeSetting = Setting "runtime"
if ($runtimeSetting -notin @("", "pc", "default") -and -not $env:XR_RUNTIME_JSON) {
    $json = ""
    if ($runtimeSetting -eq "steamvr") {
        $json = Find-OpenXrRuntime "steamxr"
        if ($json -eq "") {
            $roots = @()
            try { $roots += (Get-ItemProperty 'HKCU:\Software\Valve\Steam' -ErrorAction Stop).SteamPath } catch {}
            try { $roots += (Get-ItemProperty 'HKLM:\SOFTWARE\WOW6432Node\Valve\Steam' -ErrorAction Stop).InstallPath } catch {}
            $roots += "C:\Program Files (x86)\Steam"
            foreach ($root in @($roots)) {
                $vdf = Join-Path $root "steamapps\libraryfolders.vdf"
                if (Test-Path $vdf) {
                    foreach ($line in (Get-Content $vdf)) {
                        if ($line -match '"path"\s+"([^"]+)"') { $roots += ($Matches[1] -replace '\\\\', '\') }
                    }
                }
            }
            foreach ($root in $roots) {
                $candidate = Join-Path $root "steamapps\common\SteamVR\steamxr_win64.json"
                if (Test-Path $candidate) { $json = $candidate; break }
            }
        }
        if ($json -eq "") { Say "SteamVR was not found (is it installed through Steam?): using the runtime the PC has set up." "Yellow" }
    } elseif ($runtimeSetting -eq "virtualdesktop") {
        $json = Find-OpenXrRuntime "virtualdesktop"
        if ($json -eq "" -and (Test-Path "C:\Program Files\Virtual Desktop Streamer\VirtualDesktop.OpenXR.Runtime.json")) {
            $json = "C:\Program Files\Virtual Desktop Streamer\VirtualDesktop.OpenXR.Runtime.json"
        }
        if ($json -eq "") { Say "Virtual Desktop's runtime was not found (is the Streamer installed?): using the runtime the PC has set up." "Yellow" }
    } elseif (Test-Path $runtimeSetting) {
        $json = $runtimeSetting
    } else {
        Say "runtime=$runtimeSetting is not a file: using the runtime the PC has set up." "Yellow"
    }
    if ($json -ne "") {
        $env:XR_RUNTIME_JSON = $json
        if ($runtimeSetting -eq "steamvr" -and -not (Get-Process "vrserver" -ErrorAction SilentlyContinue)) {
            Say "Starting SteamVR..."
            try { Start-Process "steam://rungameid/250820" } catch {}
        }
    }
}
$runtime = ""
if ($env:XR_RUNTIME_JSON) {
    $runtime = $env:XR_RUNTIME_JSON
} else {
    try { $runtime = (Get-ItemProperty 'HKLM:\SOFTWARE\Khronos\OpenXR\1' -ErrorAction Stop).ActiveRuntime } catch {}
}
if ($runtime -eq "") {
    Say "No OpenXR runtime is set up on this PC: the game will only show on the monitor." "Yellow"
    Say "Virtual Desktop Streamer installs one (Options > OpenXR Runtime: VDXR)."
} else {
    Say "OpenXR runtime: $runtime"
    if ($runtime -match "virtualdesktop") {
        $streamer = Get-Process "VirtualDesktop.Streamer" -ErrorAction SilentlyContinue
        if (-not $streamer) {
            $exe = Join-Path (Split-Path -Parent (Split-Path -Parent $runtime)) "VirtualDesktop.Streamer.exe"
            if (Test-Path $exe) {
                Say "Starting Virtual Desktop Streamer..."
                Start-Process $exe
            } else {
                Say "Virtual Desktop Streamer is not running: start it, then connect from the headset." "Yellow"
            }
        }
    }
}
Say ""
if ($runtime -match "steam") {
    Say "In the headset: connect it to SteamVR (the SteamVR window shows when it is). The game moves into the headset by itself."
} else {
    Say "In the headset: connect Virtual Desktop to this PC. The game moves into the headset by itself."
}
if ($env:SHADPS4_OPENXR -ne "0" -and [int]$env:SHADPS4_XR_WAIT -gt 0) {
    Say ("The game waits up to " + $env:SHADPS4_XR_WAIT + " seconds for the headset before it starts on the monitor.")
}
Say "The DualSense: connect it to THIS PC (USB cable, or Bluetooth paired with the PC). Paired with"
Say "the headset, it reaches the PC through Virtual Desktop without motion sensors or touchpad."
Say "Where it is in the game comes from your hands: hand tracking on in the headset, and in"
Say "Virtual Desktop's settings hand tracking forwarded to the PC."
Say "Hold OPTIONS for a second (or press the PS button) to reset the view."
Say "The DualSense is what plays the game: the headset's own controllers do not."
Say "Close the game's window to quit."
Say ""
# The emulator asks Windows for about 14 GB at once (the console's memory, and what the larger
# pictures take). Where Windows cannot promise that much, the emulator stops as it starts.
$memoryShort = $false
try {
    $spare = (Get-CimInstance Win32_OperatingSystem -ErrorAction Stop).FreeVirtualMemory * 1024.0
    if ($spare -lt 16GB) {
        $memoryShort = $true
        Say ("Windows has " + (Gigabytes $spare) + " of memory left to hand out, and the emulator asks for about 14 GB: if the game does not start, close other programs and start again.") "Yellow"
        Say ""
    }
} catch {}

# --- run --------------------------------------------------------------------------------------
$logDir = Join-Path $here "user\log"
$log = Join-Path $logDir "shad_log.txt"
New-Item -ItemType Directory -Force -Path $logDir | Out-Null
if (Test-Path $log) { Copy-Item $log (Join-Path $logDir "shad_log.prev.txt") -Force }

# (In this console, with what it prints kept out of the way: a window style given here would
# also be the game window's.)
$startedAt = Get-Date
$process = Start-Process -FilePath $emulator -ArgumentList @("-g", "`"$game`"") -WorkingDirectory $here `
    -PassThru -NoNewWindow -RedirectStandardOutput (Join-Path $logDir "console.txt") `
    -RedirectStandardError (Join-Path $logDir "console-errors.txt")
# (Without this the exit code is not to be had later.)
$null = $process.Handle
$position = 0
$shown = @{}
function Show-Log {
    if (-not (Test-Path $log)) { return }
    try {
        $stream = [System.IO.File]::Open($log, 'Open', 'Read', 'ReadWrite')
    } catch { return }
    try {
        if ($stream.Length -lt $script:position) { $script:position = 0 }
        [void]$stream.Seek($script:position, 'Begin')
        $reader = New-Object System.IO.StreamReader($stream)
        while ($true) {
            $line = $reader.ReadLine()
            if ($null -eq $line) { break }
            if ($line -match '^\[Core\.Vr\] <(Info|Warning)> \([^)]*\) \S+ (?:\w+: )?(.*)$') {
                $warning = $Matches[1] -eq "Warning"
                $text = $Matches[2]
                # The lines that repeat every few seconds only once in a while.
                if ($text -match '^(The title (has|now takes) the player|Hands:|Virtual headset connected)') { continue }
                if ($text -match '^Headset: the title delivered') {
                    $script:reports++
                    if (($script:reports % 6) -ne 1) { continue }
                }
                if ($warning) { Say ("  " + $text) "Yellow" } else { Say ("  " + $text) }
            } elseif ($line -match '^\[Input\] <Info> \([^)]*\) \S+ (?:\w+: )?(Controller .*)$') {
                Say ("  " + $Matches[1])
            } elseif ($line -match '^\[Core\] <Info> \([^)]*\) \S+ (?:\w+: )?(The title draws at up to .*|The scene is drawn at .*|Frames are given .*)$') {
                Say ("  " + $Matches[1])
            } elseif ($line -match '<Critical>.*?: (.*)$') {
                $text = $Matches[1]
                if (-not $shown.ContainsKey($text)) { $shown[$text] = 1; Say ("  ! " + $text) "Red" }
            }
        }
        $script:position = $stream.Position
    } finally { $stream.Dispose() }
}
$reports = 0
while (-not $process.HasExited) {
    Start-Sleep -Milliseconds 700
    Show-Log
}
Show-Log
Say ""
if ($null -ne $process.ExitCode -and $process.ExitCode -ne 0) {
    Say ("The emulator ended with code " + $process.ExitCode + ". Its log is $log") "Yellow"
    if ($memoryShort -and ((Get-Date) - $startedAt).TotalSeconds -lt 30) {
        Say "It stopped as it started, and Windows was short of memory then (see above): close other programs and start again."
    }
    Read-Host "Press Enter to close"
} else {
    Say "The game was closed."
    Start-Sleep -Seconds 2
}
