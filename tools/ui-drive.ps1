# Drives the PC launcher's windows from outside, the way a person would: waits for a window
# with a given title that shows a given text, prints what it says, saves a picture of it, and
# presses one of its buttons (by sending the button the click message, which needs no focus).
#   tools/ui-drive.ps1 [-Title <window title>] -Steps "text|picture.png|button|seconds|type", ...
# text:    what to wait for in the window ("" for any window of that title); "gone:<text>"
#          waits until no window shows it any more.
# button:  a message box's 6 (Yes), 7 (No), 1 (OK or Open), 2 (Cancel), or what a button of
#          the launcher's own windows says ("Look again", "Play"...); empty to press nothing.
# seconds: how long to wait (60).
# type:    a text put into the window's "File name" box before the button is pressed.
# Example, a package in the games folder, kept after unpacking, then the settings window:
#   tools/ui-drive.ps1 -Steps "Unpack it now?||6", "gone:Unpacking the game|||600",
#                             "unpacked and ready||7", "Resolution of each eye|menu.png|Play"
# The windows are real and on the desktop: whoever sits at the PC sees them and can click
# them first. (Do not start the launcher minimized or hidden for this: Windows then opens a
# process's first window the same way.)
param([string[]]$Steps, [string]$Title = "Astro Bot VR")
Add-Type -AssemblyName UIAutomationClient
Add-Type -AssemblyName UIAutomationTypes
Add-Type -AssemblyName System.Drawing
Add-Type -Namespace Win32 -Name Ui -MemberDefinition @"
[DllImport("user32.dll", CharSet = CharSet.Unicode)]
public static extern IntPtr SendMessage(IntPtr hWnd, uint msg, IntPtr wParam, string lParam);
[DllImport("user32.dll")]
public static extern bool PostMessage(IntPtr hWnd, uint msg, IntPtr wParam, IntPtr lParam);
[DllImport("user32.dll")]
public static extern bool PrintWindow(IntPtr hWnd, IntPtr hdc, uint flags);
"@
$auto = [System.Windows.Automation.AutomationElement]
$tree = [System.Windows.Automation.TreeScope]
$everything = [System.Windows.Automation.Condition]::TrueCondition

function Find-Window([string]$text) {
    $named = New-Object System.Windows.Automation.PropertyCondition($auto::NameProperty, $Title)
    foreach ($window in $auto::RootElement.FindAll($tree::Children, $named)) {
        try {
            # (The console of a launcher started by hand has the same title.)
            $class = $window.Current.ClassName
            if ($class -like "*CASCADIA*" -or $class -eq "ConsoleWindowClass") { continue }
            if ($text -eq "") { return $window }
            foreach ($element in $window.FindAll($tree::Descendants, $everything)) {
                if ($element.Current.Name -like "*$text*") { return $window }
            }
        } catch {}
    }
    return $null
}

foreach ($step in $Steps) {
    $parts = $step.Split("|")
    $text = $parts[0]
    $picture = ""; $button = ""; $type = ""; $seconds = 60
    if ($parts.Count -gt 1) { $picture = $parts[1] }
    if ($parts.Count -gt 2) { $button = $parts[2] }
    if ($parts.Count -gt 3 -and $parts[3] -ne "") { $seconds = [int]$parts[3] }
    if ($parts.Count -gt 4) { $type = $parts[4] }
    $deadline = (Get-Date).AddSeconds($seconds)
    if ($text.StartsWith("gone:")) {
        $text = $text.Substring(5)
        while ((Find-Window $text) -and (Get-Date) -lt $deadline) { Start-Sleep -Milliseconds 300 }
        if (Find-Window $text) { "TIMEOUT: '$text' is still shown after $seconds s"; exit 1 }
        "gone: '$text'"
        continue
    }
    $window = $null
    while (-not $window -and (Get-Date) -lt $deadline) {
        $window = Find-Window $text
        if (-not $window) { Start-Sleep -Milliseconds 200 }
    }
    if (-not $window) { "TIMEOUT: no window '$Title' showing '$text' within $seconds s"; exit 1 }
    Start-Sleep -Milliseconds 300
    "window '$Title' showing '$text':"
    $elements = $window.FindAll($tree::Descendants, $everything)
    foreach ($element in $elements) {
        $name = $element.Current.Name
        $class = $element.Current.ClassName
        if ($name -and ($class -like "*Static*" -or $class -like "*Button*" -or $class -like "*STATIC*" -or $class -like "*BUTTON*")) {
            "    [" + $element.Current.AutomationId + "] " + ($name -replace "`r?`n", " / ")
        }
    }
    if ($picture) {
        $r = $window.Current.BoundingRectangle
        if ([double]::IsInfinity($r.Width) -or $r.Width -lt 1) { "NO PICTURE: the window is minimized or hidden"; exit 1 }
        $bitmap = New-Object System.Drawing.Bitmap([int]$r.Width, [int]$r.Height)
        $graphics = [System.Drawing.Graphics]::FromImage($bitmap)
        $hdc = $graphics.GetHdc()
        [void][Win32.Ui]::PrintWindow([IntPtr]$window.Current.NativeWindowHandle, $hdc, 2)
        $graphics.ReleaseHdc($hdc)
        $bitmap.Save($picture, [System.Drawing.Imaging.ImageFormat]::Png)
        $graphics.Dispose(); $bitmap.Dispose()
    }
    if ($type) {
        $edit = $null
        # (A file dialog's "File name" box is the edit numbered 1148; it has others.)
        foreach ($element in $elements) {
            if ($element.Current.ClassName -eq "Edit" -and $element.Current.AutomationId -eq "1148") { $edit = $element; break }
        }
        if (-not $edit) {
            foreach ($element in $elements) { if ($element.Current.ClassName -eq "Edit") { $edit = $element; break } }
        }
        if (-not $edit) { "NO EDIT BOX in that window"; exit 1 }
        # WM_SETTEXT
        [void][Win32.Ui]::SendMessage([IntPtr]$edit.Current.NativeWindowHandle, 0x000C, [IntPtr]::Zero, $type)
        "    typed: $type"
        Start-Sleep -Milliseconds 300
    }
    if ($button) {
        # By its number (a message box's buttons) or by what it says (the launcher's own). A
        # message box's buttons do not take the automation's own "invoke".
        $target = $null
        foreach ($element in $elements) {
            if ($element.Current.AutomationId -eq $button -or $element.Current.Name -eq $button) { $target = $element; break }
        }
        if (-not $target) { "NO BUTTON '$button' in that window"; exit 1 }
        $name = $target.Current.Name
        # BM_CLICK, posted: the window may close before a sent message returns.
        [void][Win32.Ui]::PostMessage([IntPtr]$target.Current.NativeWindowHandle, 0x00F5, [IntPtr]::Zero, [IntPtr]::Zero)
        "    pressed: $name"
        Start-Sleep -Milliseconds 300
    }
}
"steps done"
