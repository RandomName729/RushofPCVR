# Holds keys down in the Meta XR Simulator's window, which is how its controllers are worked by
# hand: N and M are the right controller's A and B, X and C the left one's X and Y, Tab its menu
# button, G and H the grips, F and Semicolon the sticks pressed in, the arrow keys (or I J K L)
# the left stick. Look:<dx>,<dy> turns the simulated head (the right mouse button held while
# the mouse moves). The window is brought to the front for the time it takes and the window that
# was in front before gets its place back. Nothing is typed unless the simulator's window really
# is in front.
#   powershell -File tools/xrsim-keys.ps1 <key>:<milliseconds> [<key>:<milliseconds> ...]
# Keys: Left Right Up Down W A S D R F Q E Space N M X C G H I J K L Tab Semicolon;
#       Wait:<milliseconds> does nothing for that long; A+B:<milliseconds> holds two at once;
#       Click:<x>,<y> clicks at that point of the window (pixels from its top left corner);
#       Scroll:<x>,<y>,<notches> turns the mouse wheel there (negative: down the page);
#       Mouse:right-down / Mouse:right-up hold and let go of the right mouse button
param([Parameter(ValueFromRemainingArguments = $true)][string[]]$Steps)

Add-Type @"
using System;
using System.Runtime.InteropServices;
public static class Keys {
    [DllImport("user32.dll")] public static extern IntPtr GetForegroundWindow();
    [DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr hwnd);
    [DllImport("user32.dll")] public static extern bool ShowWindow(IntPtr hwnd, int cmd);
    [DllImport("user32.dll")] public static extern bool IsIconic(IntPtr hwnd);
    [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr hwnd, out RECT rect);
    [DllImport("user32.dll")] public static extern bool SetCursorPos(int x, int y);
    [DllImport("user32.dll")] public static extern bool GetCursorPos(out POINT point);
    [DllImport("user32.dll")] public static extern void keybd_event(byte vk, byte scan, uint flags, UIntPtr extra);
    [DllImport("user32.dll")] public static extern uint MapVirtualKey(uint code, uint type);
    [DllImport("user32.dll")] public static extern void mouse_event(uint flags, int dx, int dy, uint data, UIntPtr extra);
    [DllImport("user32.dll")] public static extern uint GetWindowThreadProcessId(IntPtr hwnd, out uint pid);
    [DllImport("user32.dll")] public static extern bool AttachThreadInput(uint from, uint to, bool attach);
    [DllImport("kernel32.dll")] public static extern uint GetCurrentThreadId();
    [StructLayout(LayoutKind.Sequential)] public struct RECT { public int Left, Top, Right, Bottom; }
    [StructLayout(LayoutKind.Sequential)] public struct POINT { public int X, Y; }
}
"@

$codes = @{ Left = 0x25; Up = 0x26; Right = 0x27; Down = 0x28; Space = 0x20
            W = 0x57; A = 0x41; S = 0x53; D = 0x44; R = 0x52; F = 0x46; Q = 0x51; E = 0x45
            N = 0x4E; M = 0x4D; X = 0x58; C = 0x43; G = 0x47; H = 0x48; I = 0x49; J = 0x4A
            K = 0x4B; L = 0x4C; Tab = 0x09; Semicolon = 0xBA; B = 0x42; T = 0x54; U = 0x55
            Y = 0x59; Comma = 0xBC }

$process = Get-Process MetaXRSimulator -ErrorAction SilentlyContinue |
    Where-Object { $_.MainWindowHandle -ne 0 } | Select-Object -First 1
if ($null -eq $process) { Write-Output "the simulator has no window"; exit 1 }
$window = $process.MainWindowHandle
$before = [Keys]::GetForegroundWindow()
$cursor = New-Object Keys+POINT
[void][Keys]::GetCursorPos([ref]$cursor)

if ([Keys]::IsIconic($window)) { [void][Keys]::ShowWindow($window, 9) }
$pid2 = 0
$target = [Keys]::GetWindowThreadProcessId($before, [ref]$pid2)
$mine = [Keys]::GetCurrentThreadId()
[void][Keys]::AttachThreadInput($mine, $target, $true)
[void][Keys]::SetForegroundWindow($window)
[void][Keys]::AttachThreadInput($mine, $target, $false)
Start-Sleep -Milliseconds 300
if ([Keys]::GetForegroundWindow() -ne $window) {
    Write-Output "the simulator's window could not be brought to the front: nothing typed"
    exit 2
}

# A click into the picture, which is what takes the keys.
$rect = New-Object Keys+RECT
[void][Keys]::GetWindowRect($window, [ref]$rect)
$x = $rect.Left + [int](($rect.Right - $rect.Left) * 0.68)
$y = $rect.Top + [int](($rect.Bottom - $rect.Top) * 0.55)
[void][Keys]::SetCursorPos($x, $y)
[Keys]::mouse_event(0x0002, 0, 0, 0, [UIntPtr]::Zero)
[Keys]::mouse_event(0x0004, 0, 0, 0, [UIntPtr]::Zero)
Start-Sleep -Milliseconds 200

foreach ($step in $Steps) {
    $name, $time = $step.Split(":")
    if ($name -eq "Look") {
        # Turning the head: the right mouse button held down while the mouse moves, a little
        # at a time.
        $dx, $dy = $time.Split(",")
        $stepsLeft = 20
        [Keys]::mouse_event(0x0008, 0, 0, 0, [UIntPtr]::Zero)
        Start-Sleep -Milliseconds 120
        for ($i = 0; $i -lt $stepsLeft; $i++) {
            [Keys]::mouse_event(0x0001, [int]([int]$dx / $stepsLeft), [int]([int]$dy / $stepsLeft), 0, [UIntPtr]::Zero)
            Start-Sleep -Milliseconds 25
        }
        Start-Sleep -Milliseconds 120
        [Keys]::mouse_event(0x0010, 0, 0, 0, [UIntPtr]::Zero)
        Write-Output "looked by $dx,$dy"
        continue
    }
    if ($name -eq "Wait") { Start-Sleep -Milliseconds ([int]$time); continue }
    if ($name -eq "Mouse") {
        if ($time -eq "right-down") { [Keys]::mouse_event(0x0008, 0, 0, 0, [UIntPtr]::Zero) }
        if ($time -eq "right-up") { [Keys]::mouse_event(0x0010, 0, 0, 0, [UIntPtr]::Zero) }
        Start-Sleep -Milliseconds 150
        continue
    }
    if ($name -eq "Scroll") {
        $sx, $sy, $notches = $time.Split(",")
        [void][Keys]::SetCursorPos($rect.Left + [int]$sx, $rect.Top + [int]$sy)
        Start-Sleep -Milliseconds 80
        [Keys]::mouse_event(0x0800, 0, 0, [uint32]([int]$notches * 120 -band 0xffffffff), [UIntPtr]::Zero)
        Start-Sleep -Milliseconds 400
        Write-Output "scrolled by $notches at $sx,$sy"
        continue
    }
    if ($name -eq "Click") {
        $cx, $cy = $time.Split(",")
        [void][Keys]::SetCursorPos($rect.Left + [int]$cx, $rect.Top + [int]$cy)
        Start-Sleep -Milliseconds 80
        [Keys]::mouse_event(0x0002, 0, 0, 0, [UIntPtr]::Zero)
        Start-Sleep -Milliseconds 60
        [Keys]::mouse_event(0x0004, 0, 0, 0, [UIntPtr]::Zero)
        Start-Sleep -Milliseconds 300
        Write-Output "clicked at $cx,$cy"
        continue
    }
    $held = @($name.Split("+"))
    if (@($held | Where-Object { -not $codes.ContainsKey($_) }).Count -ne 0) {
        Write-Output "unknown key in $name"
        continue
    }
    if ([Keys]::GetForegroundWindow() -ne $window) { Write-Output "lost the front: stopping"; break }
    foreach ($key in $held) {
        $code = [byte]$codes[$key]
        # Arrow keys are "extended" keys. The key's place on the keyboard goes with it: some
        # programs read that and not what the key means.
        $flags = 0
        if ($code -ge 0x25 -and $code -le 0x28) { $flags = 1 }
        [Keys]::keybd_event($code, [byte][Keys]::MapVirtualKey($code, 0), $flags, [UIntPtr]::Zero)
    }
    Start-Sleep -Milliseconds ([int]$time)
    foreach ($key in $held) {
        $code = [byte]$codes[$key]
        $flags = 2
        if ($code -ge 0x25 -and $code -le 0x28) { $flags = 3 }
        [Keys]::keybd_event($code, [byte][Keys]::MapVirtualKey($code, 0), $flags, [UIntPtr]::Zero)
    }
    Start-Sleep -Milliseconds 150
    Write-Output "held $name for $time ms"
}

[void][Keys]::SetCursorPos($cursor.X, $cursor.Y)
if ($before -ne [IntPtr]::Zero) { [void][Keys]::SetForegroundWindow($before) }
