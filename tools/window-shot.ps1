# Lists the visible top-level windows, or saves a picture of the one whose title matches.
#   powershell -File tools/window-shot.ps1                       list windows
#   powershell -File tools/window-shot.ps1 <title regex> <png>   picture of the first match
param([string]$Title = "", [string]$Out = "")

Add-Type -AssemblyName System.Drawing
Add-Type @"
using System;
using System.Text;
using System.Collections.Generic;
using System.Runtime.InteropServices;
public static class Win {
    public delegate bool EnumProc(IntPtr hwnd, IntPtr lparam);
    [DllImport("user32.dll")] public static extern bool EnumWindows(EnumProc proc, IntPtr lparam);
    [DllImport("user32.dll")] public static extern bool IsWindowVisible(IntPtr hwnd);
    [DllImport("user32.dll", CharSet = CharSet.Unicode)]
    public static extern int GetWindowText(IntPtr hwnd, StringBuilder text, int count);
    [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr hwnd, out RECT rect);
    [DllImport("user32.dll")] public static extern bool GetClientRect(IntPtr hwnd, out RECT rect);
    [DllImport("user32.dll")] public static extern bool ClientToScreen(IntPtr hwnd, ref POINT point);
    [DllImport("user32.dll")] public static extern uint GetWindowThreadProcessId(IntPtr hwnd, out uint pid);
    [DllImport("user32.dll")] public static extern bool PrintWindow(IntPtr hwnd, IntPtr hdc, uint flags);
    [DllImport("user32.dll")] public static extern bool IsIconic(IntPtr hwnd);
    [StructLayout(LayoutKind.Sequential)] public struct RECT { public int Left, Top, Right, Bottom; }
    [StructLayout(LayoutKind.Sequential)] public struct POINT { public int X, Y; }
    public static List<IntPtr> All() {
        var list = new List<IntPtr>();
        EnumWindows((h, l) => { if (IsWindowVisible(h)) list.Add(h); return true; }, IntPtr.Zero);
        return list;
    }
    public static string TitleOf(IntPtr h) {
        var text = new StringBuilder(512);
        GetWindowText(h, text, 512);
        return text.ToString();
    }
}
"@

$windows = [Win]::All() | ForEach-Object {
    $rect = New-Object Win+RECT
    [void][Win]::GetWindowRect($_, [ref]$rect)
    $processId = 0
    [void][Win]::GetWindowThreadProcessId($_, [ref]$processId)
    [pscustomobject]@{
        Handle = $_; Title = [Win]::TitleOf($_); Pid = $processId
        Process = (Get-Process -Id $processId -ErrorAction SilentlyContinue).ProcessName
        Width = $rect.Right - $rect.Left; Height = $rect.Bottom - $rect.Top
        Minimized = [Win]::IsIconic($_)
    }
} | Where-Object { $_.Title -ne "" }

if ($Title -eq "") {
    $windows | Format-Table Process, Pid, Width, Height, Minimized, Title -AutoSize
    exit 0
}

$window = $windows | Where-Object { $_.Title -match $Title -or $_.Process -match $Title } |
    Select-Object -First 1
if ($null -eq $window) {
    Write-Output "no window matches '$Title'"
    exit 1
}
$client = New-Object Win+RECT
[void][Win]::GetClientRect($window.Handle, [ref]$client)
$width = [Math]::Max(1, $client.Right)
$height = [Math]::Max(1, $client.Bottom)
$bitmap = New-Object System.Drawing.Bitmap $width, $height
$graphics = [System.Drawing.Graphics]::FromImage($bitmap)
$hdc = $graphics.GetHdc()
# 1 = client area only, 2 = also what the window draws with the GPU
[void][Win]::PrintWindow($window.Handle, $hdc, 3)
$graphics.ReleaseHdc($hdc)
$bitmap.Save($Out, [System.Drawing.Imaging.ImageFormat]::Png)
Write-Output ("{0} ({1}): {2}x{3} -> {4}" -f $window.Title, $window.Process, $width, $height, $Out)
