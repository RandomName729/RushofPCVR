# Starts Until Dawn: Rush of Blood in the emulator for a headset connected to this PC (Virtual
# Desktop, or anything else with an OpenXR runtime), and tells what is going on while it runs.
# Started by "Play Rush of Blood.bat"; settings are in settings.txt next to this file, and the
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

# The Motion edition (the one with the Quest controllers playing as PlayStation Move wands) has a
# file "license.dat" next to this launcher, signed by the maker (ECDSA P-256; this launcher holds
# only the public key). Without a valid one the choice of controllers below stays switched off: the
# emulator of the free edition has no Move support in it, and the Motion emulator checks the same
# file itself. Changing the text of the file breaks the signature.
$licensePublicKey = "ce24b402a7cf69a13d6db787f873722ccb6302432069fc63ad7e0470cf3dfc41e3d2d62d23684aa3df447db9c9fc53364e7fb428d65d830a8d9cbad7d0b27592"
function Test-MotionEdition {
    try {
        if ($licensePublicKey -notmatch '^[0-9a-fA-F]{128}$') { return $false }
        $path = Join-Path $here "license.dat"
        if (-not (Test-Path -LiteralPath $path)) { return $false }
        $lines = @([IO.File]::ReadAllLines($path) | ForEach-Object { $_.Trim() } | Where-Object { $_ -ne "" })
        if ($lines.Count -lt 3) { return $false }
        $last = $lines[$lines.Count - 1]
        if (-not $last.StartsWith("signature=")) { return $false }
        $body = @($lines[0..($lines.Count - 2)])
        if ($body[0] -ne "RushVR-Motion-License-1" -or ($body -notcontains "edition=motion")) { return $false }
        $payload = [Text.Encoding]::UTF8.GetBytes(($body -join "`n"))
        $signature = [Convert]::FromBase64String($last.Substring(10))
        # A BCRYPT_ECCKEY_BLOB: the magic "ECS1" (ECDSA public P-256), the key size 32, X and Y.
        $blob = New-Object byte[] 72
        [BitConverter]::GetBytes([uint32]0x31534345).CopyTo($blob, 0)
        [BitConverter]::GetBytes([uint32]32).CopyTo($blob, 4)
        for ($i = 0; $i -lt 64; $i++) { $blob[8 + $i] = [Convert]::ToByte($licensePublicKey.Substring($i * 2, 2), 16) }
        $cng = [Security.Cryptography.CngKey]::Import($blob, [Security.Cryptography.CngKeyBlobFormat]::EccPublicBlob)
        $ecdsa = New-Object Security.Cryptography.ECDsaCng($cng)
        return [bool]$ecdsa.VerifyData($payload, $signature, [Security.Cryptography.HashAlgorithmName]::SHA256)
    } catch {}
    return $false
}
$motionEdition = Test-MotionEdition
# The version shown in the window's banner (the release scripts check that it is the release's).
$launcherVersion = "0.4.0"

# PS4 Pro mode ("neo_mode" in user\config.json): the emulator then tells the game it runs on a
# PlayStation 4 Pro, which gives sharper textures. The emulator reads it as it starts.
$configFile = Join-Path $here "user\config.json"
function Get-NeoMode {
    try {
        if (Test-Path -LiteralPath $configFile) {
            $found = [regex]::Match([IO.File]::ReadAllText($configFile), '"neo_mode"\s*:\s*(true|false)')
            if ($found.Success) { return ($found.Groups[1].Value -eq "true") }
        }
    } catch {}
    return $false
}
function Set-NeoMode([bool]$on) {
    try {
        if (-not (Test-Path -LiteralPath $configFile)) { return }
        $bytes = [IO.File]::ReadAllBytes($configFile)
        $skip = 0
        if ($bytes.Length -ge 3 -and $bytes[0] -eq 0xEF -and $bytes[1] -eq 0xBB -and $bytes[2] -eq 0xBF) { $skip = 3 }
        $text = [Text.Encoding]::UTF8.GetString($bytes, $skip, $bytes.Length - $skip)
        $word = if ($on) { "true" } else { "false" }
        $new = [regex]::Replace($text, '("neo_mode"\s*:\s*)(true|false)', ('${1}' + $word))
        if ($new -ne $text) {
            $body = [Text.Encoding]::UTF8.GetBytes($new)
            $all = New-Object byte[] ($skip + $body.Length)
            if ($skip -gt 0) { [Array]::Copy($bytes, 0, $all, 0, $skip) }
            [Array]::Copy($body, 0, $all, $skip, $body.Length)
            [IO.File]::WriteAllBytes($configFile, $all)
        }
    } catch {}
}

# The sizes an eye can be drawn at: the console's own (1152x1296: the game draws both eyes into
# one picture of 2304x1296) and larger, all the same shape (the height is 9/8 of the width).
$widths = @(1152, 1344, 1536, 1728, 1920, 2048, 2304, 2560, 2880)
$defaultWidth = 2304
function EyeHeight([int]$width) { return [int]($width * 9 / 8) }
$caps = @(120, 90, 72, 60, 45, 40, 36, 30)

Add-Type -AssemblyName System.Windows.Forms
Add-Type -AssemblyName System.Drawing
[System.Windows.Forms.Application]::EnableVisualStyles()

# --- the window ---------------------------------------------------------------------------------
# The controls of the window are drawn by the launcher itself (the small program below, built
# when the launcher starts). If that cannot be built, a message says why and the game starts with the saved settings.
$uiSource = @'
using System;
using System.Collections.Generic;
using System.Drawing;
using System.Drawing.Drawing2D;
using System.Drawing.Imaging;
using System.Runtime.InteropServices;
using System.Windows.Forms;

namespace RushUi
{
    public static class Theme
    {
        public static readonly Color Back = Color.FromArgb(9, 7, 8);
        public static readonly Color Card = Color.FromArgb(17, 14, 15);
        public static readonly Color Field = Color.FromArgb(29, 24, 25);
        public static readonly Color FieldHover = Color.FromArgb(45, 37, 38);
        public static readonly Color Line = Color.FromArgb(47, 35, 37);
        public static readonly Color Text = Color.FromArgb(205, 196, 187);
        public static readonly Color Dim = Color.FromArgb(128, 116, 112);
        public static readonly Color Accent = Color.FromArgb(150, 24, 32);
        public static readonly Color AccentHover = Color.FromArgb(184, 38, 44);
        public static readonly Color AccentDown = Color.FromArgb(108, 16, 22);
        public static readonly Color Blood = Color.FromArgb(66, 9, 13);
        public static readonly Color Edge = Color.FromArgb(64, 20, 24);

        // A colour between a and b: t = 0 is a, t = 1 is b.
        public static Color Mix(Color a, Color b, float t)
        {
            t = Math.Max(0f, Math.Min(1f, t));
            return Color.FromArgb(a.R + (int)((b.R - a.R) * t), a.G + (int)((b.G - a.G) * t), a.B + (int)((b.B - a.B) * t));
        }

        // Over a control that is switched off: lets it fade into the card behind it.
        public static void Fade(Graphics g, Control c)
        {
            if (c.Enabled) { return; }
            Color back = c.Parent != null ? c.Parent.BackColor : Back;
            using (SolidBrush b = new SolidBrush(Color.FromArgb(150, back)))
            {
                g.FillRectangle(b, 0, 0, c.Width, c.Height);
            }
        }

        public static GraphicsPath Round(RectangleF r, float radius)
        {
            float d = radius * 2f;
            GraphicsPath p = new GraphicsPath();
            p.AddArc(r.X, r.Y, d, d, 180, 90);
            p.AddArc(r.Right - d, r.Y, d, d, 270, 90);
            p.AddArc(r.Right - d, r.Bottom - d, d, d, 0, 90);
            p.AddArc(r.X, r.Bottom - d, d, d, 90, 90);
            p.CloseFigure();
            return p;
        }

        [DllImport("dwmapi.dll")]
        private static extern int DwmSetWindowAttribute(IntPtr hwnd, int attr, ref int value, int size);

        // A dark title bar in the colour of the window (Windows 10 and 11; ignored where it is not there).
        public static void DarkTitleBar(Form form)
        {
            try
            {
                IntPtr h = form.Handle;
                int on = 1;
                DwmSetWindowAttribute(h, 20, ref on, 4);
                int caption = Back.R | (Back.G << 8) | (Back.B << 16);
                DwmSetWindowAttribute(h, 35, ref caption, 4);
                int text = Text.R | (Text.G << 8) | (Text.B << 16);
                DwmSetWindowAttribute(h, 36, ref text, 4);
            }
            catch (Exception) { }
        }
    }

    // A rounded panel with a title.
    // Dirt: a square of speckle, pale and dark flecks, the same every time, laid over a surface.
    public static class Grit
    {
        private static Bitmap light;
        private static Bitmap heavy;
        private static readonly object gate = new object();

        private static Bitmap Build(int boost)
        {
            Bitmap bmp = new Bitmap(192, 192, PixelFormat.Format32bppArgb);
            Random rnd = new Random(1313);
            for (int y = 0; y < 192; y++)
            {
                for (int x = 0; x < 192; x++)
                {
                    int roll = rnd.Next(100);
                    Color c = Color.FromArgb(0, 0, 0, 0);
                    if (roll < 12) { c = Color.FromArgb(rnd.Next(5, 14) * boost, 255, 240, 230); }
                    else if (roll < 44) { c = Color.FromArgb(rnd.Next(8, 26) * boost, 0, 0, 0); }
                    bmp.SetPixel(x, y, c);
                }
            }
            return bmp;
        }

        public static void Overlay(Graphics g, Rectangle area, bool strong)
        {
            if (area.Width <= 0 || area.Height <= 0) { return; }
            try
            {
                lock (gate)
                {
                    if (light == null) { light = Build(1); heavy = Build(2); }
                }
                using (TextureBrush brush = new TextureBrush(strong ? heavy : light, WrapMode.Tile))
                {
                    g.FillRectangle(brush, area);
                }
            }
            catch (Exception) { }
        }
    }

    public class Card : Panel
    {
        private Font titleFont = new Font("Segoe UI Semibold", 8.5f);
        private Font summaryFont = new Font("Segoe UI", 8.5f);
        private string summary;
        private bool open = true;
        private bool headHover;
        public string Title { get; set; }
        // A card that can be folded: only its title row shows, with a one-line summary.
        public bool Foldable { get; set; }
        public event EventHandler Toggled;

        public string Summary { get { return summary; } set { summary = value; Invalidate(); } }
        public bool Open { get { return open; } set { open = value; Invalidate(); } }

        public Card()
        {
            SetStyle(ControlStyles.UserPaint | ControlStyles.AllPaintingInWmPaint |
                     ControlStyles.OptimizedDoubleBuffer | ControlStyles.ResizeRedraw, true);
            BackColor = Theme.Card;
        }

        // Capital letters with a little space between them; returns where it ended.
        private static int Spaced(Graphics g, string text, Font font, Color color, int x, int y, int gap)
        {
            foreach (char ch in text)
            {
                if (ch == ' ') { x += 6; continue; }
                string one = ch.ToString();
                TextRenderer.DrawText(g, one, font, new Point(x, y), color, TextFormatFlags.NoPadding | TextFormatFlags.Left);
                x += TextRenderer.MeasureText(g, one, font, new Size(200, 100), TextFormatFlags.NoPadding).Width + gap;
            }
            return x;
        }

        protected override void OnPaint(PaintEventArgs e)
        {
            Graphics g = e.Graphics;
            g.Clear(Parent != null ? Parent.BackColor : Theme.Back);
            if (Width < 8 || Height < 8) { return; }
            g.SmoothingMode = SmoothingMode.AntiAlias;
            using (GraphicsPath p = Theme.Round(new RectangleF(0.5f, 0.5f, Width - 2f, Height - 2f), 4f))
            using (SolidBrush b = new SolidBrush(Theme.Card))
            using (Pen pen = new Pen(Theme.Line))
            {
                g.FillPath(b, p);
                g.DrawPath(pen, p);
            }
            using (Pen glint = new Pen(Color.FromArgb(46, Theme.Accent), 1f))
            using (Pen shade = new Pen(Color.FromArgb(120, 0, 0, 0), 1f))
            {
                g.DrawLine(glint, 10f, 1f, Width - 11f, 1f);
                g.DrawLine(shade, 5f, Height - 3f, Width - 6f, Height - 3f);
            }
            if (!string.IsNullOrEmpty(Title))
            {
                using (SolidBrush mark = new SolidBrush(Theme.Accent))
                {
                    g.FillRectangle(mark, 18f, 16f, 3f, 12f);
                }
                Spaced(g, Title.ToUpperInvariant(), titleFont, Theme.Text, 28, 15, 1);
                if (!Foldable || open)
                {
                    using (Pen rule = new Pen(Theme.Line, 1f))
                    {
                        g.DrawLine(rule, 18f, 40f, Width - 18f, 40f);
                    }
                }
                if (Foldable)
                {
                    Color tone = headHover ? Theme.Text : Theme.Dim;
                    float cx = Width - 28f;
                    float cy = 22f;
                    using (Pen chevron = new Pen(tone, 1.6f))
                    {
                        if (open)
                        {
                            g.DrawLines(chevron, new PointF[] { new PointF(cx - 5f, cy - 2f), new PointF(cx, cy + 3f), new PointF(cx + 5f, cy - 2f) });
                        }
                        else
                        {
                            g.DrawLines(chevron, new PointF[] { new PointF(cx - 2f, cy - 5f), new PointF(cx + 3f, cy), new PointF(cx - 2f, cy + 5f) });
                        }
                    }
                    if (!open && !string.IsNullOrEmpty(summary))
                    {
                        TextRenderer.DrawText(g, summary, summaryFont, new Rectangle(190, 0, Math.Max(1, Width - 190 - 46), 44),
                                              headHover ? Theme.Text : Theme.Dim,
                                              TextFormatFlags.Right | TextFormatFlags.VerticalCenter |
                                              TextFormatFlags.EndEllipsis | TextFormatFlags.NoPadding);
                    }
                }
            }
        }

        protected override void OnMouseMove(MouseEventArgs e)
        {
            base.OnMouseMove(e);
            bool over = Foldable && e.Y < 44;
            if (over != headHover)
            {
                headHover = over;
                Cursor = over ? Cursors.Hand : Cursors.Default;
                Invalidate();
            }
        }

        protected override void OnMouseLeave(EventArgs e)
        {
            base.OnMouseLeave(e);
            if (headHover)
            {
                headHover = false;
                Cursor = Cursors.Default;
                Invalidate();
            }
        }

        protected override void OnMouseDown(MouseEventArgs e)
        {
            base.OnMouseDown(e);
            if (Foldable && e.Button == MouseButtons.Left && e.Y < 44)
            {
                open = !open;
                Invalidate();
                if (Toggled != null) { Toggled(this, EventArgs.Empty); }
            }
        }

        protected override void Dispose(bool disposing)
        {
            if (disposing)
            {
                titleFont.Dispose();
                summaryFont.Dispose();
            }
            base.Dispose(disposing);
        }
    }

    public class DarkColors : ProfessionalColorTable
    {
        public override Color ToolStripDropDownBackground { get { return Theme.Field; } }
        public override Color MenuBorder { get { return Theme.Line; } }
        public override Color MenuItemBorder { get { return Theme.FieldHover; } }
        public override Color MenuItemSelected { get { return Theme.FieldHover; } }
        public override Color MenuItemSelectedGradientBegin { get { return Theme.FieldHover; } }
        public override Color MenuItemSelectedGradientEnd { get { return Theme.FieldHover; } }
        public override Color MenuItemPressedGradientBegin { get { return Theme.FieldHover; } }
        public override Color MenuItemPressedGradientMiddle { get { return Theme.FieldHover; } }
        public override Color MenuItemPressedGradientEnd { get { return Theme.FieldHover; } }
        public override Color ImageMarginGradientBegin { get { return Theme.Field; } }
        public override Color ImageMarginGradientMiddle { get { return Theme.Field; } }
        public override Color ImageMarginGradientEnd { get { return Theme.Field; } }
        public override Color SeparatorDark { get { return Theme.Line; } }
        public override Color SeparatorLight { get { return Theme.Line; } }
    }

    public class DarkRenderer : ToolStripProfessionalRenderer
    {
        public DarkRenderer() : base(new DarkColors()) { RoundedEdges = false; }
    }

    // A drop-down list in the window's own style.
    public class Dropdown : Control
    {
        private List<string> items = new List<string>();
        private int index = -1;
        private bool hover;
        private bool open;
        private int closedAt = -10000;
        private ContextMenuStrip menu;

        public event EventHandler SelectedIndexChanged;

        public Dropdown()
        {
            SetStyle(ControlStyles.UserPaint | ControlStyles.AllPaintingInWmPaint |
                     ControlStyles.OptimizedDoubleBuffer | ControlStyles.ResizeRedraw |
                     ControlStyles.Selectable, true);
            TabStop = true;
            Cursor = Cursors.Hand;
            Height = 34;
        }

        public List<string> Items { get { return items; } }

        public int SelectedIndex
        {
            get { return index; }
            set
            {
                if (value < -1 || value >= items.Count || value == index) { return; }
                index = value;
                Invalidate();
                if (SelectedIndexChanged != null) { SelectedIndexChanged(this, EventArgs.Empty); }
            }
        }

        protected override void OnPaint(PaintEventArgs e)
        {
            Graphics g = e.Graphics;
            g.Clear(Parent != null ? Parent.BackColor : Theme.Card);
            g.SmoothingMode = SmoothingMode.AntiAlias;
            RectangleF box = new RectangleF(0.5f, 0.5f, Width - 2f, Height - 2f);
            using (GraphicsPath p = Theme.Round(box, 7f))
            using (SolidBrush b = new SolidBrush((hover || open) ? Theme.FieldHover : Theme.Field))
            {
                g.FillPath(b, p);
                if (Focused && ShowFocusCues)
                {
                    using (Pen ring = new Pen(Theme.Accent, 1.5f)) { g.DrawPath(ring, p); }
                }
            }
            string s = (index >= 0 && index < items.Count) ? items[index] : "";
            TextRenderer.DrawText(g, s, Font, new Rectangle(12, 0, Width - 40, Height), Theme.Text,
                                  TextFormatFlags.VerticalCenter | TextFormatFlags.Left |
                                  TextFormatFlags.EndEllipsis | TextFormatFlags.NoPadding);
            float cx = Width - 18f;
            float cy = Height / 2f;
            using (Pen pen = new Pen(Theme.Dim, 1.8f))
            {
                pen.StartCap = LineCap.Round;
                pen.EndCap = LineCap.Round;
                pen.LineJoin = LineJoin.Round;
                g.DrawLines(pen, new PointF[] { new PointF(cx - 4f, cy - 2f), new PointF(cx, cy + 2f), new PointF(cx + 4f, cy - 2f) });
            }
        }

        protected override void OnMouseEnter(EventArgs e) { base.OnMouseEnter(e); hover = true; Invalidate(); }
        protected override void OnMouseLeave(EventArgs e) { base.OnMouseLeave(e); hover = false; Invalidate(); }
        protected override void OnGotFocus(EventArgs e) { base.OnGotFocus(e); Invalidate(); }
        protected override void OnLostFocus(EventArgs e) { base.OnLostFocus(e); Invalidate(); }

        protected override void OnClick(EventArgs e)
        {
            base.OnClick(e);
            Focus();
            if (unchecked(Environment.TickCount - closedAt) < 250) { return; }
            OpenMenu();
        }

        protected override bool IsInputKey(Keys keyData)
        {
            Keys k = keyData & Keys.KeyCode;
            if (k == Keys.Up || k == Keys.Down) { return true; }
            return base.IsInputKey(keyData);
        }

        protected override void OnKeyDown(KeyEventArgs e)
        {
            base.OnKeyDown(e);
            if (e.KeyCode == Keys.Down) { SelectedIndex = Math.Min(items.Count - 1, index + 1); e.Handled = true; }
            else if (e.KeyCode == Keys.Up) { SelectedIndex = Math.Max(0, index - 1); e.Handled = true; }
            else if (e.KeyCode == Keys.Space || e.KeyCode == Keys.F4) { OpenMenu(); e.Handled = true; }
        }

        private void OpenMenu()
        {
            if (open || items.Count == 0) { return; }
            if (menu != null) { menu.Dispose(); }
            menu = new ContextMenuStrip();
            menu.Renderer = new DarkRenderer();
            menu.BackColor = Theme.Field;
            menu.ForeColor = Theme.Text;
            menu.Font = Font;
            menu.ShowImageMargin = false;
            menu.ShowCheckMargin = false;
            menu.MinimumSize = new Size(Width, 0);
            for (int i = 0; i < items.Count; i++)
            {
                ToolStripMenuItem mi = new ToolStripMenuItem(items[i]);
                mi.Tag = i;
                mi.Padding = new Padding(4, 5, 4, 5);
                mi.ForeColor = (i == index) ? Theme.Accent : Theme.Text;
                mi.Click += OnPick;
                menu.Items.Add(mi);
            }
            menu.Closed += OnMenuClosed;
            open = true;
            Invalidate();
            menu.Show(this, new Point(0, Height + 4));
        }

        private void OnPick(object sender, EventArgs e)
        {
            ToolStripMenuItem mi = sender as ToolStripMenuItem;
            if (mi != null) { SelectedIndex = (int)mi.Tag; }
        }

        private void OnMenuClosed(object sender, ToolStripDropDownClosedEventArgs e)
        {
            open = false;
            closedAt = Environment.TickCount;
            Invalidate();
        }

        protected override void Dispose(bool disposing)
        {
            if (disposing && menu != null) { menu.Dispose(); }
            base.Dispose(disposing);
        }
    }

    // A slider with whole-number steps.
    public class Slider : Control
    {
        private int min = 0;
        private int max = 10;
        private int val = 0;
        private bool drag;
        private bool hover;

        public event EventHandler ValueChanged;

        public Slider()
        {
            SetStyle(ControlStyles.UserPaint | ControlStyles.AllPaintingInWmPaint |
                     ControlStyles.OptimizedDoubleBuffer | ControlStyles.ResizeRedraw |
                     ControlStyles.Selectable, true);
            TabStop = true;
            Cursor = Cursors.Hand;
            Height = 30;
        }

        public int Minimum { get { return min; } set { min = value; Invalidate(); } }
        public int Maximum { get { return max; } set { max = value; Invalidate(); } }

        public int Value
        {
            get { return val; }
            set
            {
                int v = Math.Max(min, Math.Min(max, value));
                if (v == val) { return; }
                val = v;
                Invalidate();
                if (ValueChanged != null) { ValueChanged(this, EventArgs.Empty); }
            }
        }

        protected override void OnPaint(PaintEventArgs e)
        {
            Graphics g = e.Graphics;
            g.Clear(Parent != null ? Parent.BackColor : Theme.Card);
            g.SmoothingMode = SmoothingMode.AntiAlias;
            float pad = 11f;
            float w = Width - 2f * pad;
            float cy = Height / 2f - 2f;
            float span = Math.Max(1, max - min);
            float tx = pad + w * (val - min) / span;
            using (GraphicsPath track = Theme.Round(new RectangleF(pad, cy - 2f, w, 4f), 2f))
            using (SolidBrush b = new SolidBrush(Theme.Field))
            {
                g.FillPath(b, track);
            }
            if (tx - pad >= 4f)
            {
                RectangleF part = new RectangleF(pad, cy - 2f, tx - pad, 4f);
                using (GraphicsPath fill = Theme.Round(part, 2f))
                using (LinearGradientBrush b = new LinearGradientBrush(part, Theme.Blood, Theme.AccentHover, 0f))
                {
                    g.FillPath(b, fill);
                }
            }
            if (max - min <= 24)
            {
                using (Pen tick = new Pen(Theme.Line, 1f))
                {
                    for (int i = min; i <= max; i++)
                    {
                        float x = pad + w * (i - min) / span;
                        g.DrawLine(tick, x, cy + 9f, x, cy + 12f);
                    }
                }
            }
            if (drag || hover || (Focused && ShowFocusCues))
            {
                using (SolidBrush halo = new SolidBrush(Color.FromArgb(55, Theme.Accent)))
                {
                    g.FillEllipse(halo, tx - 13f, cy - 13f, 26f, 26f);
                }
            }
            using (SolidBrush knob = new SolidBrush(Theme.Text))
            using (Pen ring = new Pen(Theme.Accent, 2f))
            {
                g.FillEllipse(knob, tx - 8f, cy - 8f, 16f, 16f);
                g.DrawEllipse(ring, tx - 8f, cy - 8f, 16f, 16f);
            }
            Theme.Fade(g, this);
        }

        protected override void OnEnabledChanged(EventArgs e) { base.OnEnabledChanged(e); Invalidate(); }

        private void SetFromX(int x)
        {
            float t = (x - 11f) / Math.Max(1f, Width - 22f);
            t = Math.Max(0f, Math.Min(1f, t));
            Value = min + (int)Math.Round(t * (max - min));
        }

        protected override void OnMouseDown(MouseEventArgs e)
        {
            base.OnMouseDown(e);
            Focus();
            drag = true;
            SetFromX(e.X);
            Invalidate();
        }

        protected override void OnMouseMove(MouseEventArgs e)
        {
            base.OnMouseMove(e);
            if (drag) { SetFromX(e.X); }
        }

        protected override void OnMouseUp(MouseEventArgs e)
        {
            base.OnMouseUp(e);
            drag = false;
            Invalidate();
        }

        protected override void OnMouseEnter(EventArgs e) { base.OnMouseEnter(e); hover = true; Invalidate(); }
        protected override void OnMouseLeave(EventArgs e) { base.OnMouseLeave(e); hover = false; Invalidate(); }
        protected override void OnGotFocus(EventArgs e) { base.OnGotFocus(e); Invalidate(); }
        protected override void OnLostFocus(EventArgs e) { base.OnLostFocus(e); Invalidate(); }

        protected override bool IsInputKey(Keys keyData)
        {
            Keys k = keyData & Keys.KeyCode;
            if (k == Keys.Left || k == Keys.Right || k == Keys.Up || k == Keys.Down ||
                k == Keys.Home || k == Keys.End) { return true; }
            return base.IsInputKey(keyData);
        }

        protected override void OnKeyDown(KeyEventArgs e)
        {
            base.OnKeyDown(e);
            switch (e.KeyCode)
            {
                case Keys.Left:
                case Keys.Down: Value = val - 1; e.Handled = true; break;
                case Keys.Right:
                case Keys.Up: Value = val + 1; e.Handled = true; break;
                case Keys.Home: Value = min; e.Handled = true; break;
                case Keys.End: Value = max; e.Handled = true; break;
            }
        }
    }

    // A switch with a text beside it.
    public class Toggle : Control
    {
        private bool on;
        private bool hover;
        private float t; // 0 = off, 1 = on, in between while the knob slides
        private System.Windows.Forms.Timer slide;

        public event EventHandler CheckedChanged;

        public Toggle()
        {
            SetStyle(ControlStyles.UserPaint | ControlStyles.AllPaintingInWmPaint |
                     ControlStyles.OptimizedDoubleBuffer | ControlStyles.ResizeRedraw |
                     ControlStyles.Selectable, true);
            TabStop = true;
            Cursor = Cursors.Hand;
            Height = 26;
        }

        public bool Checked
        {
            get { return on; }
            set
            {
                if (on == value) { return; }
                on = value;
                if (!IsHandleCreated) { t = on ? 1f : 0f; }
                else { StartSlide(); }
                Invalidate();
                if (CheckedChanged != null) { CheckedChanged(this, EventArgs.Empty); }
            }
        }

        private void StartSlide()
        {
            if (slide == null)
            {
                slide = new System.Windows.Forms.Timer();
                slide.Interval = 15;
                slide.Tick += OnSlide;
            }
            slide.Start();
        }

        private void OnSlide(object sender, EventArgs e)
        {
            float target = on ? 1f : 0f;
            if (t < target) { t = Math.Min(target, t + 0.2f); }
            else if (t > target) { t = Math.Max(target, t - 0.2f); }
            if (t == target) { slide.Stop(); }
            Invalidate();
        }

        protected override void OnPaint(PaintEventArgs e)
        {
            Graphics g = e.Graphics;
            g.Clear(Parent != null ? Parent.BackColor : Theme.Back);
            g.SmoothingMode = SmoothingMode.AntiAlias;
            float top = (Height - 20f) / 2f;
            Color fill = Theme.Mix(hover ? Theme.FieldHover : Theme.Field, Theme.Accent, t);
            using (GraphicsPath pill = Theme.Round(new RectangleF(0.5f, top, 37f, 20f), 10f))
            using (SolidBrush b = new SolidBrush(fill))
            {
                g.FillPath(b, pill);
                if (Focused && ShowFocusCues)
                {
                    using (Pen ring = new Pen(Theme.Accent, 1.5f)) { g.DrawPath(ring, pill); }
                }
            }
            float kx = 4f + 17f * t;
            using (SolidBrush knob = new SolidBrush(Theme.Text))
            {
                g.FillEllipse(knob, kx, top + 3f, 14f, 14f);
            }
            TextRenderer.DrawText(g, Text, Font, new Rectangle(48, 0, Math.Max(1, Width - 48), Height), Theme.Text,
                                  TextFormatFlags.VerticalCenter | TextFormatFlags.Left |
                                  TextFormatFlags.EndEllipsis | TextFormatFlags.NoPadding);
            Theme.Fade(g, this);
        }

        protected override void OnEnabledChanged(EventArgs e) { base.OnEnabledChanged(e); Invalidate(); }

        protected override void OnClick(EventArgs e)
        {
            base.OnClick(e);
            Focus();
            Checked = !on;
        }

        protected override void OnKeyDown(KeyEventArgs e)
        {
            base.OnKeyDown(e);
            if (e.KeyCode == Keys.Space) { Checked = !on; e.Handled = true; }
        }

        protected override void OnMouseEnter(EventArgs e) { base.OnMouseEnter(e); hover = true; Invalidate(); }
        protected override void OnMouseLeave(EventArgs e) { base.OnMouseLeave(e); hover = false; Invalidate(); }
        protected override void OnGotFocus(EventArgs e) { base.OnGotFocus(e); Invalidate(); }
        protected override void OnLostFocus(EventArgs e) { base.OnLostFocus(e); Invalidate(); }

        protected override void Dispose(bool disposing)
        {
            if (disposing && slide != null) { slide.Dispose(); }
            base.Dispose(disposing);
        }
    }

    // A rounded button; Primary is the one in the accent colour.
    public class FlatButton : Button
    {
        private bool hover;
        private bool down;
        private float glow; // 0 = at rest, 1 = under the pointer
        private System.Windows.Forms.Timer fader;
        private Font buttonFont = new Font("Segoe UI Semibold", 10f);
        public bool Primary { get; set; }

        public FlatButton()
        {
            SetStyle(ControlStyles.UserPaint | ControlStyles.AllPaintingInWmPaint |
                     ControlStyles.OptimizedDoubleBuffer | ControlStyles.ResizeRedraw, true);
            Cursor = Cursors.Hand;
        }

        private void StartFade()
        {
            if (fader == null)
            {
                fader = new System.Windows.Forms.Timer();
                fader.Interval = 15;
                fader.Tick += OnFade;
            }
            fader.Start();
        }

        private void OnFade(object sender, EventArgs e)
        {
            float target = hover ? 1f : 0f;
            if (glow < target) { glow = Math.Min(target, glow + 0.2f); }
            else if (glow > target) { glow = Math.Max(target, glow - 0.2f); }
            if (glow == target) { fader.Stop(); }
            Invalidate();
        }

        protected override void OnPaint(PaintEventArgs e)
        {
            Graphics g = e.Graphics;
            g.Clear(Parent != null ? Parent.BackColor : Theme.Back);
            g.SmoothingMode = SmoothingMode.AntiAlias;
            Color rest = Primary ? Theme.Accent : Theme.Field;
            Color over = Primary ? Theme.AccentHover : Theme.FieldHover;
            Color fill = down ? (Primary ? Theme.AccentDown : Theme.Line) : Theme.Mix(rest, over, glow);
            using (GraphicsPath p = Theme.Round(new RectangleF(0.5f, 0.5f, Width - 2f, Height - 2f), 8f))
            using (SolidBrush b = new SolidBrush(fill))
            {
                g.FillPath(b, p);
                if (Primary)
                {
                    using (Pen edge = new Pen(Color.FromArgb((int)(40 + 80 * glow), 255, 120, 130), 1f)) { g.DrawPath(edge, p); }
                }
                if (Focused && ShowFocusCues)
                {
                    using (Pen ring = new Pen(Primary ? Theme.Text : Theme.Accent, 1.5f)) { g.DrawPath(ring, p); }
                }
            }
            TextRenderer.DrawText(g, Text, buttonFont, new Rectangle(0, 0, Width, Height), Theme.Text,
                                  TextFormatFlags.HorizontalCenter | TextFormatFlags.VerticalCenter |
                                  TextFormatFlags.NoPadding);
            Theme.Fade(g, this);
        }

        protected override void OnEnabledChanged(EventArgs e) { base.OnEnabledChanged(e); Invalidate(); }
        protected override void OnMouseEnter(EventArgs e) { base.OnMouseEnter(e); hover = true; StartFade(); }
        protected override void OnMouseLeave(EventArgs e) { base.OnMouseLeave(e); hover = false; down = false; StartFade(); Invalidate(); }
        protected override void OnMouseDown(MouseEventArgs e) { base.OnMouseDown(e); down = true; Invalidate(); }
        protected override void OnMouseUp(MouseEventArgs e) { base.OnMouseUp(e); down = false; Invalidate(); }
        protected override void OnGotFocus(EventArgs e) { base.OnGotFocus(e); Invalidate(); }
        protected override void OnLostFocus(EventArgs e) { base.OnLostFocus(e); Invalidate(); }

        protected override void Dispose(bool disposing)
        {
            if (disposing)
            {
                buttonFont.Dispose();
                if (fader != null) { fader.Dispose(); }
            }
            base.Dispose(disposing);
        }
    }

    // The window's minimize and close buttons (Kind 0 = minimize, 1 = close).
    public class CaptionButton : Control
    {
        private bool hover;
        public int Kind { get; set; }

        public CaptionButton()
        {
            SetStyle(ControlStyles.UserPaint | ControlStyles.AllPaintingInWmPaint |
                     ControlStyles.OptimizedDoubleBuffer | ControlStyles.ResizeRedraw, true);
            Cursor = Cursors.Hand;
            TabStop = false;
        }

        protected override void OnPaint(PaintEventArgs e)
        {
            Graphics g = e.Graphics;
            Color back = hover ? (Kind == 1 ? Theme.Accent : Theme.FieldHover) : Theme.Back;
            g.Clear(back);
            g.SmoothingMode = SmoothingMode.AntiAlias;
            float cx = Width / 2f;
            float cy = Height / 2f;
            using (Pen pen = new Pen(hover ? Theme.Text : Theme.Dim, 1.4f))
            {
                if (Kind == 1)
                {
                    g.DrawLine(pen, cx - 5f, cy - 5f, cx + 5f, cy + 5f);
                    g.DrawLine(pen, cx - 5f, cy + 5f, cx + 5f, cy - 5f);
                }
                else
                {
                    g.DrawLine(pen, cx - 5f, cy + 3f, cx + 5f, cy + 3f);
                }
            }
        }

        protected override void OnMouseEnter(EventArgs e) { base.OnMouseEnter(e); hover = true; Invalidate(); }
        protected override void OnMouseLeave(EventArgs e) { base.OnMouseLeave(e); hover = false; Invalidate(); }
    }

    // The banner at the top: name, edition and version, and the grip for moving the window.
    public class Header : Control
    {
        [DllImport("user32.dll")]
        private static extern bool ReleaseCapture();
        [DllImport("user32.dll")]
        private static extern IntPtr SendMessage(IntPtr hWnd, int msg, IntPtr wParam, IntPtr lParam);

        private Font wordFont = new Font("Segoe UI Black", 24f);
        private Font smallFont = new Font("Segoe UI Semibold", 8f);
        private Font badgeFont = new Font("Segoe UI Semibold", 7.5f);
        public string Badge { get; set; }
        public bool BadgeSolid { get; set; }
        public string Version { get; set; }

        public Header()
        {
            SetStyle(ControlStyles.UserPaint | ControlStyles.AllPaintingInWmPaint |
                     ControlStyles.OptimizedDoubleBuffer | ControlStyles.ResizeRedraw, true);
        }

        // Text with a little space between the letters; returns where it ended.
        private static int Spaced(Graphics g, string text, Font font, Color color, int x, int y, int gap)
        {
            foreach (char ch in text)
            {
                string one = ch.ToString();
                TextRenderer.DrawText(g, one, font, new Point(x, y), color, TextFormatFlags.NoPadding | TextFormatFlags.Left);
                x += TextRenderer.MeasureText(g, one, font, new Size(200, 100), TextFormatFlags.NoPadding).Width + gap;
            }
            return x;
        }

        protected override void OnPaint(PaintEventArgs e)
        {
            Graphics g = e.Graphics;
            g.SmoothingMode = SmoothingMode.AntiAlias;
            Rectangle all = new Rectangle(0, 0, Width, Height);
            using (LinearGradientBrush wash = new LinearGradientBrush(all, Color.FromArgb(44, 7, 12), Theme.Back, 0f))
            {
                g.FillRectangle(wash, all);
            }
            Grit.Overlay(g, all, true);
            using (LinearGradientBrush grime = new LinearGradientBrush(all, Color.FromArgb(0, 0, 0, 0), Color.FromArgb(120, 0, 0, 0), 90f))
            {
                g.FillRectangle(grime, all);
            }
            Random rnd = new Random(77);
            // Blood run down from the top edge.
            using (SolidBrush drip = new SolidBrush(Color.FromArgb(150, 96, 12, 20)))
            {
                for (int k = 0; k < 5; k++)
                {
                    float dx = Width * (0.44f + 0.12f * k) + rnd.Next(-10, 10);
                    float len = 8f + rnd.Next(0, 20);
                    float w = 2f + rnd.Next(0, 2);
                    g.FillRectangle(drip, dx, 0f, w, len);
                    g.FillEllipse(drip, dx - 1f, len - 1f, w + 2f, w + 3f);
                }
            }
            // Scratches.
            using (Pen scratch = new Pen(Color.FromArgb(26, 255, 236, 226), 1f))
            {
                for (int k = 0; k < 9; k++)
                {
                    float sx = rnd.Next(0, Math.Max(1, Width - 80));
                    float sy = rnd.Next(4, Math.Max(5, Height - 6));
                    g.DrawLine(scratch, sx, sy, sx + 20 + rnd.Next(0, 60), sy + rnd.Next(-6, 7));
                }
            }
            // Claw marks, faint, behind the right half of the banner.
            using (Pen claw = new Pen(Color.FromArgb(40, Theme.Accent), 1.4f))
            {
                for (int k = 0; k < 4; k++)
                {
                    float x0 = Width * 0.52f + k * 16f;
                    g.DrawLine(claw, x0, 6f, x0 - 34f, Height - 16f);
                }
            }
            Spaced(g, "RUSHVR", wordFont, Color.FromArgb(22, 3, 5), 26, 10, 2);
            int x = Spaced(g, "RUSH", wordFont, Theme.Text, 24, 8, 2);
            x = Spaced(g, "VR", wordFont, Theme.Accent, x, 8, 2);
            Spaced(g, "UNTIL DAWN: RUSH OF BLOOD   \u00B7   PC VR", smallFont, Theme.Dim, 27, 56, 2);

            int bx = x + 12;
            if (!string.IsNullOrEmpty(Badge))
            {
                Size bs = TextRenderer.MeasureText(g, Badge, badgeFont, new Size(300, 40), TextFormatFlags.NoPadding);
                Rectangle pill = new Rectangle(bx, 20, bs.Width + 18, 20);
                using (GraphicsPath gp = Theme.Round(pill, 3f))
                {
                    if (BadgeSolid)
                    {
                        using (SolidBrush fill = new SolidBrush(Theme.Accent)) { g.FillPath(fill, gp); }
                    }
                    else
                    {
                        using (Pen outline = new Pen(Theme.Dim, 1f)) { g.DrawPath(outline, gp); }
                    }
                }
                TextRenderer.DrawText(g, Badge, badgeFont, new Rectangle(pill.X, pill.Y, pill.Width, pill.Height),
                                      BadgeSolid ? Theme.Text : Theme.Dim,
                                      TextFormatFlags.HorizontalCenter | TextFormatFlags.VerticalCenter | TextFormatFlags.NoPadding);
                bx = pill.Right + 10;
            }
            if (!string.IsNullOrEmpty(Version))
            {
                TextRenderer.DrawText(g, "v" + Version, smallFont, new Point(bx, 23), Theme.Dim,
                                      TextFormatFlags.NoPadding | TextFormatFlags.Left);
            }
            // The line under the banner: crimson fading out to the right.
            Rectangle rule = new Rectangle(0, Height - 2, Width, 2);
            using (LinearGradientBrush line = new LinearGradientBrush(rule, Theme.Accent, Theme.Back, 0f))
            {
                g.FillRectangle(line, rule);
            }
        }

        protected override void OnMouseDown(MouseEventArgs e)
        {
            base.OnMouseDown(e);
            if (e.Button == MouseButtons.Left && FindForm() != null)
            {
                ReleaseCapture();
                SendMessage(FindForm().Handle, 0xA1, (IntPtr)2, IntPtr.Zero);
            }
        }

        protected override void Dispose(bool disposing)
        {
            if (disposing)
            {
                wordFont.Dispose();
                smallFont.Dispose();
                badgeFont.Dispose();
            }
            base.Dispose(disposing);
        }
    }

    // The strip along the bottom: what the game will start with.
    public class StatusBar : Control
    {
        private string[] items = new string[0];
        private bool ok = true;
        private Font statusFont = new Font("Segoe UI", 8.5f);

        public StatusBar()
        {
            SetStyle(ControlStyles.UserPaint | ControlStyles.AllPaintingInWmPaint |
                     ControlStyles.OptimizedDoubleBuffer | ControlStyles.ResizeRedraw, true);
        }

        public string[] Items { get { return items; } set { items = value ?? new string[0]; Invalidate(); } }
        public bool Ok { get { return ok; } set { ok = value; Invalidate(); } }

        protected override void OnPaint(PaintEventArgs e)
        {
            Graphics g = e.Graphics;
            g.Clear(Color.FromArgb(12, 10, 11));
            Grit.Overlay(g, new Rectangle(0, 0, Width, Height), false);
            g.SmoothingMode = SmoothingMode.AntiAlias;
            using (Pen top = new Pen(Theme.Line, 1f)) { g.DrawLine(top, 0, 0, Width, 0); }
            using (SolidBrush dot = new SolidBrush(ok ? Color.FromArgb(86, 140, 84) : Color.FromArgb(196, 146, 44)))
            {
                g.FillEllipse(dot, 20f, Height / 2f - 4f, 8f, 8f);
            }
            string text = string.Join("   \u00B7   ", items);
            TextRenderer.DrawText(g, text, statusFont, new Rectangle(38, 0, Math.Max(1, Width - 52), Height), Theme.Dim,
                                  TextFormatFlags.VerticalCenter | TextFormatFlags.Left |
                                  TextFormatFlags.EndEllipsis | TextFormatFlags.NoPadding);
        }

        protected override void Dispose(bool disposing)
        {
            if (disposing) { statusFont.Dispose(); }
            base.Dispose(disposing);
        }
    }

    // The window itself: no system title bar (the banner is one), a thin crimson edge, rounded
    // corners where Windows 11 offers them, and a quick fade in.
    public class Window : Form
    {
        [DllImport("dwmapi.dll")]
        private static extern int DwmSetWindowAttribute(IntPtr hwnd, int attr, ref int value, int size);

        private System.Windows.Forms.Timer fade;

        public Window()
        {
            FormBorderStyle = FormBorderStyle.None;
            SetStyle(ControlStyles.UserPaint | ControlStyles.AllPaintingInWmPaint |
                     ControlStyles.OptimizedDoubleBuffer, true);
            Opacity = 0;
        }

        protected override CreateParams CreateParams
        {
            get
            {
                CreateParams cp = base.CreateParams;
                cp.ClassStyle |= 0x00020000; // a drop shadow
                cp.Style |= 0x00020000;      // the taskbar can minimize and restore it
                return cp;
            }
        }

        protected override void OnHandleCreated(EventArgs e)
        {
            base.OnHandleCreated(e);
            try
            {
                int round = 2;
                DwmSetWindowAttribute(Handle, 33, ref round, 4);
                int edge = Theme.Edge.R | (Theme.Edge.G << 8) | (Theme.Edge.B << 16);
                DwmSetWindowAttribute(Handle, 34, ref edge, 4);
            }
            catch (Exception) { }
        }

        protected override void OnPaint(PaintEventArgs e)
        {
            e.Graphics.Clear(Theme.Back);
            Grit.Overlay(e.Graphics, ClientRectangle, false);
            using (Pen edge = new Pen(Theme.Edge, 1f))
            {
                e.Graphics.DrawRectangle(edge, 0, 0, Width - 1, Height - 1);
            }
        }

        protected override void OnShown(EventArgs e)
        {
            base.OnShown(e);
            fade = new System.Windows.Forms.Timer();
            fade.Interval = 15;
            fade.Tick += OnFadeTick;
            fade.Start();
        }

        private void OnFadeTick(object sender, EventArgs e)
        {
            double next = Opacity + 0.12;
            if (next >= 1.0)
            {
                next = 1.0;
                fade.Stop();
            }
            Opacity = next;
        }

        protected override void Dispose(bool disposing)
        {
            if (disposing && fade != null) { fade.Dispose(); }
            base.Dispose(disposing);
        }
    }
}
'@
$script:modernUi = $false
$script:modernTried = $false
# Builds the controls, once, when the window is first wanted.
function Enable-ModernUi {
    if ($script:modernTried) { return }
    $script:modernTried = $true
    try {
        Add-Type -TypeDefinition $uiSource -ReferencedAssemblies @("System.Windows.Forms", "System.Drawing") -Language CSharp -ErrorAction Stop
        $script:modernUi = $true
    } catch {
        $script:windowError = $_.Exception.Message
    }
}

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

# True when a PlayStation controller (DualSense, DualShock 4) is connected to this PC. The game
# prefers it over the Quest controllers, so with one plugged in the Quest controllers do nothing.
function Test-PlayStationPad {
    try {
        $found = @(Get-PnpDevice -PresentOnly -ErrorAction Stop | Where-Object {
            $_.Status -eq "OK" -and $_.InstanceId -like 'HID\*' -and
            $_.InstanceId -match 'VID_054C&PID_(0CE6|0DF2|05C4|09CC)|VID&0002054C_PID&(0CE6|0DF2|05C4|09CC)' })
        return ($found.Count -gt 0)
    } catch { return $false }
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

# --- the window (the new look) ----------------------------------------------------------------
function Read-Number([string]$key, [double]$default) {
    $number = 0.0
    if ([double]::TryParse((Setting $key "$default"), [System.Globalization.NumberStyles]::Float, [System.Globalization.CultureInfo]::InvariantCulture, [ref]$number)) { return $number }
    return $default
}
# The place in the values of the one nearest to what is set now.
function Find-Nearest([double[]]$values, [double]$now) {
    $best = 0
    $bestDistance = [double]::MaxValue
    for ($i = 0; $i -lt $values.Count; $i++) {
        $distance = [math]::Abs($values[$i] - $now)
        if ($distance -lt $bestDistance) { $best = $i; $bestDistance = $distance }
    }
    return $best
}
function New-Card([string]$title, [int]$x, [int]$y, [int]$w, [int]$h) {
    $card = New-Object RushUi.Card
    $card.Title = $title
    $card.SetBounds($x, $y, $w, $h)
    return $card
}
function New-Label($parent, [string]$text, [int]$x, [int]$y, [int]$w, [int]$h, $color,
                   [double]$size = 9.5, [string]$family = "Segoe UI", [string]$align = "TopLeft") {
    $label = New-Object System.Windows.Forms.Label
    $label.AutoSize = $false
    $label.Text = $text
    $label.SetBounds($x, $y, $w, $h)
    $label.ForeColor = $color
    $label.BackColor = $parent.BackColor
    $label.Font = New-Object System.Drawing.Font($family, [single]$size)
    $label.TextAlign = $align
    $parent.Controls.Add($label)
    return $label
}
function New-Dropdown($parent, [string[]]$names, [int]$selected, [int]$x, [int]$y, [int]$w) {
    $box = New-Object RushUi.Dropdown
    foreach ($name in $names) { [void]$box.Items.Add($name) }
    $box.SetBounds($x, $y, $w, 34)
    $box.SelectedIndex = $selected
    $parent.Controls.Add($box)
    return $box
}
function New-Slider($parent, [int]$minimum, [int]$maximum, [int]$value, [int]$x, [int]$y, [int]$w) {
    $slider = New-Object RushUi.Slider
    $slider.Minimum = $minimum
    $slider.Maximum = $maximum
    $slider.SetBounds($x, $y, $w, 30)
    $slider.Value = $value
    $parent.Controls.Add($slider)
    return $slider
}

function Show-MenuModern {
    $script:menuOutcome = "plain"
    try {
        $cText = [RushUi.Theme]::Text
        $cDim = [RushUi.Theme]::Dim
        $cAccent = [RushUi.Theme]::Accent

        # The presets of the picture quality (Low, Medium, High): the width of an eye, the textures,
        # the edge smoothing, the sharpening, and the size of the picture handed to the headset.
        $presetNames = @("Low", "Medium", "High")
        $presetWidth = @(1536, 2304, 2880)
        $presetAniso = @("4", "16", "16")
        $presetFxaa = @("0", "0.65", "0.65")
        $presetSharp = @("0", "0.3", "0.3")
        $presetLayer = @("75", "50", "50")
        $presetButtons = @()
        # Lights the button of the preset the choices are at (none, when they are at none of them).
        $markPresets = {
            if ($presetButtons.Count -eq 0) { return }
            for ($n = 0; $n -lt 3; $n++) {
                $same = ($widths[$resolution.Value] -eq $presetWidth[$n]) -and
                        ($anisoKeys[$anisoBox.SelectedIndex] -eq $presetAniso[$n]) -and
                        ($fxaaKeys[$fxaaBox.SelectedIndex] -eq $presetFxaa[$n]) -and
                        ($sharpKeys[$sharpBox.SelectedIndex] -eq $presetSharp[$n]) -and
                        ($layerKeys[$layerBox.SelectedIndex] -eq $presetLayer[$n])
                if ($presetButtons[$n].Primary -ne $same) {
                    $presetButtons[$n].Primary = $same
                    $presetButtons[$n].Invalidate()
                }
            }
        }
        $applyPreset = {
            param($n)
            $resolution.Value = [array]::IndexOf($widths, $presetWidth[$n])
            $anisoBox.SelectedIndex = [array]::IndexOf($anisoKeys, $presetAniso[$n])
            $fxaaBox.SelectedIndex = [array]::IndexOf($fxaaKeys, $presetFxaa[$n])
            $sharpBox.SelectedIndex = [array]::IndexOf($sharpKeys, $presetSharp[$n])
            $layerBox.SelectedIndex = [array]::IndexOf($layerKeys, $presetLayer[$n])
            & $markPresets
        }

        # The window draws its own title bar (the banner), so it can be minimized from there.
        # Its height is set by $layout below, which also moves things when a card is folded.
        $form = New-Object RushUi.Window
        $form.Text = "RushVR"
        $form.ClientSize = New-Object System.Drawing.Size(736, 700)
        $form.StartPosition = "CenterScreen"
        $form.TopMost = $true
        $form.BackColor = [RushUi.Theme]::Back
        $form.ForeColor = $cText
        $form.Font = New-Object System.Drawing.Font("Segoe UI", 9.5)
        $tips = New-Object System.Windows.Forms.ToolTip
        $tips.InitialDelay = 300
        $tips.AutoPopDelay = 15000

        # The banner: name, edition, version, and the minimize and close buttons.
        $header = New-Object RushUi.Header
        $header.SetBounds(1, 1, 734, 84)
        $header.Badge = $(if ($motionEdition) { "MOTION" } else { "FREE" })
        $header.BadgeSolid = $motionEdition
        $header.Version = $launcherVersion
        $form.Controls.Add($header)
        $minimizeButton = New-Object RushUi.CaptionButton
        $minimizeButton.Kind = 0
        $minimizeButton.SetBounds(642, 0, 46, 34)
        $header.Controls.Add($minimizeButton)
        $closeButton = New-Object RushUi.CaptionButton
        $closeButton.Kind = 1
        $closeButton.SetBounds(688, 0, 46, 34)
        $header.Controls.Add($closeButton)
        $minimizeButton.Add_Click({ $form.WindowState = [System.Windows.Forms.FormWindowState]::Minimized })
        $closeButton.Add_Click({ $form.DialogResult = [System.Windows.Forms.DialogResult]::Cancel })
        $tips.SetToolTip($minimizeButton, "Minimize")
        $tips.SetToolTip($closeButton, "Close")

        # Row 1: the headset connection, and the frame rate. (y 92, 150 high)
        $headset = New-Card "Headset" 24 92 332 150
        $form.Controls.Add($headset)
        [void](New-Label $headset "Connection (OpenXR runtime)" 18 50 296 20 $cDim 9)
        $runtimeNames = @("Automatic (as the PC has it set up)", "Virtual Desktop", "SteamVR")
        $runtimeKeys = @("pc", "virtualdesktop", "steamvr")
        $index = [array]::IndexOf($runtimeKeys, (Setting "runtime" "pc"))
        if ($index -lt 0) { $index = 0 }
        $runtimeBox = New-Dropdown $headset $runtimeNames $index 18 72 296
        $runtimeText = New-Label $headset "" 18 112 296 32 $cDim 8.5
        $updateRuntime = {
            $runtimeText.Text = switch ($runtimeBox.SelectedIndex) {
                1 { "Starts Virtual Desktop's streamer if needed. In the headset, connect it to this PC." }
                2 { "Starts SteamVR if needed: connect the headset to it first. Not tested with this game yet." }
                default { "Uses the runtime the PC has set (Virtual Desktop's streamer sets itself when it starts)." }
            }
        }
        $runtimeBox.Add_SelectedIndexChanged($updateRuntime)
        & $updateRuntime

        $rate = New-Card "Frame rate" 380 92 332 150
        $form.Controls.Add($rate)
        $targetKeys = @("0", "30", "35", "40", "45", "50", "60")
        $targetNames = @("Original timing (the game's own pace)", "30 a second", "35 a second", "40 a second (steady at 80 Hz)", "45 a second (steady at 90 Hz)", "50 a second", "60 a second (recommended, 120 Hz)")
        [void](New-Label $rate "Aim for" 18 50 296 20 $cDim 9)
        $targetBox = New-Dropdown $rate $targetNames (Find-Nearest ([double[]]$targetKeys) (Read-Number "target_fps" 60)) 18 72 296
        [void](New-Label $rate "60 suits Virtual Desktop at 120 Hz (45 at 90 Hz, 40 at 80 Hz). Lower is steadier on a slower PC." 18 112 296 32 $cDim 8.5)
        $tips.SetToolTip($targetBox, "The emulator holds the game to this rate. 60 is the game's own and looks smoothest with Virtual Desktop's frame rate at 120 (Streaming tab); at 90 or 80 use 45 or 40. Lower is steadier on a slower PC.")

        # Row 2: the picture, the two sliders side by side. (y 254, 148 high)
        $picture = New-Card "Picture" 24 254 688 148
        $form.Controls.Add($picture)
        [void](New-Label $picture "Quality" 290 11 120 20 $cDim 8.5 "Segoe UI" "TopRight")
        $presetTips = @(
            "The lightest of the three, for a PC that struggles: 1536 an eye, 4x textures, no edge smoothing or sharpening, and the picture to the headset at 75%. For a lighter one still, lower the resolution slider by hand (1152 is the console's own).",
            "The default: 2304 an eye, 16x textures, normal edge smoothing and sharpening, and the picture to the headset at 50%, which Virtual Desktop then shows smoothly. It wants a good graphics card.",
            "The sharpest and least pixelated: 2880 an eye, the largest. It needs a strong graphics card and a big Windows page file (see the README)."
        )
        for ($n = 0; $n -lt 3; $n++) {
            $presetButton = New-Object RushUi.FlatButton
            $presetButton.Text = $presetNames[$n]
            $presetButton.SetBounds((426 + $n * 84), 7, 76, 26)
            $picture.Controls.Add($presetButton)
            $tips.SetToolTip($presetButton, $presetTips[$n])
            $presetButtons += $presetButton
        }
        $presetButtons[0].Add_Click({ & $applyPreset 0 })
        $presetButtons[1].Add_Click({ & $applyPreset 1 })
        $presetButtons[2].Add_Click({ & $applyPreset 2 })
        [void](New-Label $picture "Resolution of each eye" 18 50 200 20 $cDim 9)
        $resolutionValue = New-Label $picture "" 218 50 120 20 $cAccent 9.5 "Segoe UI Semibold" "TopRight"
        $current = 0
        [void][int]::TryParse((Setting "eye_width" "$defaultWidth"), [ref]$current)
        $index = [array]::IndexOf($widths, $current)
        if ($index -lt 0) { $index = [array]::IndexOf($widths, $defaultWidth) }
        $resolution = New-Slider $picture 0 ($widths.Count - 1) $index 18 72 320
        $resolutionText = New-Label $picture "" 18 108 320 32 $cDim 8.5
        $update = {
            $w = $widths[$resolution.Value]
            $h = EyeHeight $w
            $times = ($w * $h) / (1152.0 * 1296.0)
            $what = if ($w -eq 1152) { "The console's own sharpness." } elseif ($w -gt 2048) { "{0:N1}x the console's pixels. Very demanding: about {1:N1} GB more memory." -f $times, (($times - 1) * 0.64) } else { "{0:N1}x the console's pixels. Start lower if the game stutters." -f $times }
            $resolutionValue.Text = "$w x $h"
            $resolutionText.Text = $what
        }
        $resolution.Add_ValueChanged($update)
        & $update

        [void](New-Label $picture "Field of view" 350 50 200 20 $cDim 9)
        $fovValue = New-Label $picture "" 550 50 120 20 $cAccent 9.5 "Segoe UI Semibold" "TopRight"
        $fovStart = [math]::Max(14, [math]::Min(32, [int]([int](Setting "fov" "105") / 5)))
        $fov = New-Slider $picture 14 32 $fovStart 350 72 320
        $fovText = New-Label $picture "" 350 108 320 32 $cDim 8.5
        $ofPsvr = (Setting "fov_of" "psvr") -eq "psvr"
        $updateFov = {
            $percent = $fov.Value * 5
            $fovValue.Text = "$percent%"
            if ($percent -eq 100) {
                $fovText.Text = $(if ($ofPsvr) { "The game's own view: the sharpest, with a dark border." } else { "All that the headset shows." })
            } elseif ($percent -eq 105 -and $ofPsvr) {
                $fovText.Text = "A little wider than the game's own: sharp, hardly any border."
            } elseif ($percent -eq 120 -and $ofPsvr) {
                $fovText.Text = "The smoothest picture, no border (the corners are black)."
            } elseif ($percent -eq 155 -and $ofPsvr) {
                $fovText.Text = "The Quest 3's own view: no lens corners showing."
            } elseif ($percent -gt 100) {
                $fovText.Text = "Wider than the game's: a softer picture, no border."
            } else {
                $fovText.Text = "Sharper, with a dark border."
            }
        }
        $fov.Add_ValueChanged($updateFov)
        & $updateFov

        # Row 3: picture quality, folded away until it is wanted: four choices in two columns.
        $quality = New-Card "Picture quality" 24 414 688 54
        $form.Controls.Add($quality)
        $anisoKeys = @("0", "2", "4", "8", "16")
        $anisoNames = @("Off (as the game draws)", "2x", "4x", "8x", "16x (recommended)")
        [void](New-Label $quality "Sharper textures (anisotropic)" 18 54 320 20 $cDim 9)
        $anisoBox = New-Dropdown $quality $anisoNames (Find-Nearest ([double[]]$anisoKeys) (Read-Number "aniso" 16)) 18 76 320
        $tips.SetToolTip($anisoBox, "Keeps floors and walls seen at a slant sharp instead of blurred. Costs a little of the graphics card's time.")

        $fxaaKeys = @("0", "0.35", "0.65", "1")
        $fxaaNames = @("Off", "Light", "Normal (recommended)", "Strong")
        [void](New-Label $quality "Smooth edges (FXAA)" 350 54 320 20 $cDim 9)
        $fxaaBox = New-Dropdown $quality $fxaaNames (Find-Nearest ([double[]]$fxaaKeys) (Read-Number "fxaa" 0.65)) 350 76 320
        $tips.SetToolTip($fxaaBox, "Smooths the jagged edges of the picture on its way to the headset. A larger resolution of each eye does it best.")

        $maskKeys = @("0", "0.82", "0.86", "0.9", "0.94", "0.97", "1", "1.05")
        $maskNames = @("Off: the coloured corners show", "Tightest (0.82)", "Very tight (0.86)", "Tighter still (0.9, recommended)", "Tighter (0.94)", "Tight (0.97)", "On (1.0): black corners", "Loose (1.05)")
        [void](New-Label $quality "Black corners (mask)" 18 124 320 20 $cDim 9)
        $maskBox = New-Dropdown $quality $maskNames (Find-Nearest ([double[]]$maskKeys) (Read-Number "mask" 0.9)) 18 146 320
        $tips.SetToolTip($maskBox, "Black instead of the coloured corners outside the lens circles. Tighter or Tight if some colour is left at the edge (the lower, the more is black; a narrow field of view needs a lower one); Loose if the picture's edge is cut off.")

        $sharpKeys = @("0", "0.15", "0.3", "0.5", "0.7", "1")
        $sharpNames = @("Off", "Light (0.15)", "Normal (0.3, recommended)", "Strong (0.5)", "Very strong (0.7)", "Maximum (1.0)")
        [void](New-Label $quality "Sharpening" 350 124 320 20 $cDim 9)
        $sharpBox = New-Dropdown $quality $sharpNames (Find-Nearest ([double[]]$sharpKeys) (Read-Number "sharpen" 0.3)) 350 146 320
        $tips.SetToolTip($sharpBox, "Sharpens the picture on its way to the headset. Virtual Desktop has its own sharpening (Streaming tab) that adds to this one: if the picture looks crunchy or blocky, lower either of them. With Virtual Desktop's at 50% or more, try Off or Light here.")

        # The size of the picture handed to the headset's runtime (SHADPS4_VR_LAYER_SCALE).
        $layerKeys = @("100", "75", "50")
        $layerNames = @("Full size (100%)", "Three quarters (75%)", "Half (50%)")
        $startLayer = Read-Number "layer_scale" 0
        if ($startLayer -le 0) {
            # Not set: 50 for a large picture, 100 for the others. (Older settings had it as an
            # env line, which is used if there is one.)
            $startLayer = $(if ($widths[$resolution.Value] -ge 2048) { 50 } else { 100 })
            foreach ($pair in $extraEnv) {
                $scaleNow = 0.0
                if ($pair -like "SHADPS4_VR_LAYER_SCALE=*" -and [double]::TryParse($pair.Substring(23), [System.Globalization.NumberStyles]::Float, [System.Globalization.CultureInfo]::InvariantCulture, [ref]$scaleNow)) {
                    $startLayer = $scaleNow * 100
                }
            }
        }
        [void](New-Label $quality "Picture size sent to the headset" 18 194 320 20 $cDim 9)
        $layerBox = New-Dropdown $quality $layerNames (Find-Nearest ([double[]]$layerKeys) $startLayer) 18 216 320
        [void](New-Label $quality "Smaller is lighter for the PC and the stream, and a little softer. A big picture looks smoother shrunk here than by Virtual Desktop." 350 212 320 44 $cDim 8.5)
        $tips.SetToolTip($layerBox, "The picture the game draws is shrunk to this part of its size before it goes to Virtual Desktop (or SteamVR), which would otherwise shrink it itself with a rougher filter. Smaller means less for the PC to send and the headset to receive; a large picture (2048 an eye and more) looks smoother shrunk here.")

        # What the folded card says: the four choices in a line.
        $anisoShort = @("off", "2x", "4x", "8x", "16x")
        $fxaaShort = @("off", "light", "normal", "strong")
        $sharpShort = @("off", "light", "normal", "strong", "very strong", "maximum")
        $updateQuality = {
            $quality.Summary = ("Textures " + $anisoShort[$anisoBox.SelectedIndex] + "  |  Edges " + $fxaaShort[$fxaaBox.SelectedIndex] + "  |  Sharpening " + $sharpShort[$sharpBox.SelectedIndex] + "  |  Size " + $layerKeys[$layerBox.SelectedIndex] + "%")
        }
        foreach ($box in @($anisoBox, $fxaaBox, $maskBox, $sharpBox, $layerBox)) { $box.Add_SelectedIndexChanged($updateQuality) }
        & $updateQuality
        foreach ($box in @($anisoBox, $fxaaBox, $sharpBox, $layerBox)) { $box.Add_SelectedIndexChanged($markPresets) }
        $resolution.Add_ValueChanged($markPresets)
        & $markPresets
        $quality.Foldable = $true
        $quality.Open = $false
        foreach ($c in $quality.Controls) { $c.Visible = $false }
        $tips.SetToolTip($quality, "Click to show or hide the picture quality choices.")

        # Row 4: what the window on the PC shows (a screen recording sees it), folded away too.
        $windowCard = New-Card "PC window" 24 480 688 54
        $form.Controls.Add($windowCard)
        $viewKeys = @("left", "right", "merged", "both")
        $viewNames = @("Left eye, cropped (a regular view)", "Right eye, cropped (a regular view)", "Both eyes, one regular view (blended)", "Both eyes, with the lens circles")
        $viewShort = @("left eye", "right eye", "both eyes, one view", "both eyes, circles")
        [void](New-Label $windowCard "The window on the PC shows" 18 54 320 20 $cDim 9)
        $viewIndex = [array]::IndexOf($viewKeys, (Setting "pc_view" "left"))
        if ($viewIndex -lt 0) { $viewIndex = 0 }
        $viewBox = New-Dropdown $windowCard $viewNames $viewIndex 18 76 320
        $tips.SetToolTip($viewBox, "What the window of the game on the PC shows, which is what a screen recording of it sees. The headset is not affected. A cropped eye fills the window like a normal game. Both eyes, one view, joins the left eye's picture into the right eye's across the middle: things very close up can look doubled there.")
        $viewText = New-Label $windowCard "" 18 116 320 32 $cDim 8.5
        [void](New-Label $windowCard "Zoom" 350 54 200 20 $cDim 9)
        $zoomValue = New-Label $windowCard "" 550 54 120 20 $cAccent 9.5 "Segoe UI Semibold" "TopRight"
        $zoomStart = [math]::Max(10, [math]::Min(24, [int][math]::Round((Read-Number "pc_zoom" 100) / 5)))
        $zoom = New-Slider $windowCard 10 24 $zoomStart 350 76 320
        $tips.SetToolTip($zoom, "How much of the picture the cropped views show. 100% is the most that fits the window without black corners; lower is closer in, higher shows more of the picture and brings black corners. Not used for the view with the lens circles.")
        $zoomText = New-Label $windowCard "100% fits the window with no black corners." 350 116 320 32 $cDim 8.5
        $updateView = {
            $viewText.Text = switch ($viewBox.SelectedIndex) {
                0 { "The left eye's picture, cropped to fill the window like a normal game." }
                1 { "The right eye's picture, cropped to fill the window." }
                2 { "Both eyes joined into one flat view. Things very close up can look doubled near the middle." }
                default { "Both eyes side by side, with the lens circles, as the headset gets them." }
            }
            $zoomNow = ($viewBox.SelectedIndex -ne 3)
            $zoom.Enabled = $zoomNow
            $zoomValue.ForeColor = $(if ($zoomNow) { $cAccent } else { $cDim })
            $zoomValue.Text = $(if ($zoomNow) { "" + ($zoom.Value * 5) + "%" } else { "" })
            $windowCard.Summary = ($viewShort[$viewBox.SelectedIndex] + $(if ($zoomNow) { "  |  zoom " + ($zoom.Value * 5) + "%" } else { "" }))
        }
        $viewBox.Add_SelectedIndexChanged($updateView)
        $zoom.Add_ValueChanged($updateView)
        & $updateView
        $windowCard.Foldable = $true
        $windowCard.Open = $false
        foreach ($c in $windowCard.Controls) { $c.Visible = $false }
        $tips.SetToolTip($windowCard, "Click to show or hide what the window on the PC shows.")

        # Row 5: controllers, the DualSense or the Quest controllers playing as Move wands (Motion edition).
        $moveHeight = $(if ($motionEdition) { 108 } else { 46 })
        $moveCard = New-Card "Controllers" 24 480 688 $moveHeight
        $form.Controls.Add($moveCard)
        $moveOn = New-Object RushUi.Toggle
        $moveOn.Text = $(if ($motionEdition) { "Quest controllers as Move wands" } else { "Quest controllers as Move wands (Motion edition)" })
        $moveOn.SetBounds(150, 8, 390, 26)
        $moveOn.Checked = ($motionEdition -and ((Setting "controllers" "dualsense") -eq "quest"))
        $moveOn.Enabled = $motionEdition
        $moveCard.Controls.Add($moveOn)
        $tips.SetToolTip($moveOn, "Plays with the Quest controllers, as the game's two PlayStation Move wands, instead of the DualSense. In the game's start-up question choose the Move controllers. Unplug the DualSense first: the game uses it instead when it is connected. Motion edition only.")
        $moveDefaults = @(35, 55, 65)
        $moveKeys = @("move_pitch", "move_reach", "move_turn")
        $moveTitles = @("Aim angle", "Reach", "Turning")
        $moveLimits = @(@(0, 90), @(5, 130), @(5, 130))
        $moveUnits = @(" degrees up", "%", "%")
        $moveTips = @(
            "Raises the in-game gun from the way the Quest controller points. A Quest controller's handle is tilted down from where you point it, so with 0 the guns follow the handle and point lower than you aim. Raise it if they point down when your controller is straight ahead, lower it if they point up. Recommended: 35.",
            "How far the in-game hands move when you move yours, measured from your head. 100% is as far as you really move. Lower it if the hands move too much. Recommended: 55%.",
            "How much the in-game guns turn when you turn your wrist, measured from level and facing where you face. 100% is as far as you really turn. Lower it if the aim is too touchy. Recommended: 65%."
        )
        $moveSliders = @()
        $moveValues = @()
        for ($i = 0; $i -lt 3; $i++) {
            $x = 18 + $i * 224
            $start = [int][math]::Round((Read-Number $moveKeys[$i] $moveDefaults[$i]))
            $start = [math]::Max($moveLimits[$i][0], [math]::Min($moveLimits[$i][1], $start))
            [void](New-Label $moveCard $moveTitles[$i] $x 50 100 20 $cDim 9)
            $moveValues += (New-Label $moveCard "" ($x + 100) 50 100 20 $cAccent 9.5 "Segoe UI Semibold" "TopRight")
            $slider = New-Slider $moveCard $moveLimits[$i][0] $moveLimits[$i][1] $start $x 70 200
            $tips.SetToolTip($slider, $moveTips[$i])
            $moveSliders += $slider
        }
        $moveUpdate = {
            for ($j = 0; $j -lt 3; $j++) {
                $moveValues[$j].Text = ("" + $moveSliders[$j].Value + $moveUnits[$j])
            }
        }
        foreach ($slider in $moveSliders) { $slider.Add_ValueChanged($moveUpdate) }
        & $moveUpdate
        $moveReset = New-Object RushUi.FlatButton
        $moveReset.Text = "Recommended"
        $moveReset.SetBounds(554, 8, 116, 28)
        $moveReset.Add_Click({
            for ($j = 0; $j -lt 3; $j++) { $moveSliders[$j].Value = $moveDefaults[$j] }
        })
        $moveCard.Controls.Add($moveReset)
        $tips.SetToolTip($moveReset, "Puts the three sliders back to what is recommended.")
        # The sliders can be moved only while the Quest controllers are chosen (and owned).
        $moveEnable = {
            $live = ($moveOn.Enabled -and $moveOn.Checked)
            foreach ($slider in $moveSliders) { $slider.Enabled = $live }
            $moveReset.Enabled = $live
            foreach ($label in $moveValues) { $label.ForeColor = $(if ($live) { $cAccent } else { $cDim }) }
        }
        $moveOn.Add_CheckedChanged($moveEnable)
        & $moveEnable
        # Without the Motion edition the card is only its title row and the switch.
        if (-not $motionEdition) {
            foreach ($c in $moveCard.Controls) { if ($c -ne $moveOn) { $c.Visible = $false } }
        }

        # The strip along the bottom: what the game will start with.
        $status = New-Object RushUi.StatusBar
        $status.SetBounds(1, 666, 734, 32)
        $form.Controls.Add($status)
        $runtimeShort = @("Automatic", "Virtual Desktop", "SteamVR")
        $updateStatus = {
            $gameText = "Game found"
            $gameOk = $true
            if ($info -and $info.Count -gt 0) {
                $gameText = "Game " + $info["TITLE_ID"] + " " + $info["APP_VER"]
                $gameOk = ($info["TITLE_ID"] -eq $madeFor -and $info["APP_VER"] -eq "01.00")
            }
            $status.Ok = $gameOk
            $status.Items = [string[]]@(
                $gameText,
                ("Headset: " + $runtimeShort[$runtimeBox.SelectedIndex]),
                ("Controllers: " + $(if ($moveOn.Checked) { "Quest, as Move wands" } else { "DualSense" }))
            )
        }
        $runtimeBox.Add_SelectedIndexChanged($updateStatus)
        $moveOn.Add_CheckedChanged($updateStatus)
        & $updateStatus

        # Bottom row.
        $neo = New-Object RushUi.Toggle
        $neo.Text = "PS4 Pro mode (sharper textures)"
        $neo.Checked = Get-NeoMode
        $neo.SetBounds(24, 600, 380, 26)
        $form.Controls.Add($neo)
        $tips.SetToolTip($neo, "The game is told it runs on a PlayStation 4 Pro, which gives sharper textures. Takes effect as the game starts.")
        $again = New-Object RushUi.Toggle
        $again.Text = "Show this window at every start"
        $again.Checked = (Setting "menu" "1") -ne "0"
        $again.SetBounds(24, 630, 380, 26)
        $form.Controls.Add($again)

        $play = New-Object RushUi.FlatButton
        $play.Text = "Play"
        $play.Primary = $true
        $play.SetBounds(592, 608, 120, 40)
        $play.DialogResult = [System.Windows.Forms.DialogResult]::OK
        $form.Controls.Add($play)
        $form.AcceptButton = $play
        $quit = New-Object RushUi.FlatButton
        $quit.Text = "Quit"
        $quit.SetBounds(476, 608, 104, 40)
        $quit.DialogResult = [System.Windows.Forms.DialogResult]::Cancel
        $form.Controls.Add($quit)
        $form.CancelButton = $quit

        # Where everything below the picture goes, which depends on whether the picture quality
        # card is open; the window grows and shrinks with it.
        $layout = {
            $y = 414
            $quality.SetBounds(24, $y, 688, $(if ($quality.Open) { 260 } else { 54 }))
            $y += $quality.Height + 12
            $windowCard.SetBounds(24, $y, 688, $(if ($windowCard.Open) { 168 } else { 54 }))
            $y += $windowCard.Height + 12
            $moveCard.SetBounds(24, $y, 688, $moveHeight)
            $y += $moveHeight + 14
            $neo.SetBounds(24, $y, 380, 26)
            $again.SetBounds(24, ($y + 30), 380, 26)
            $quit.SetBounds(476, ($y + 8), 104, 40)
            $play.SetBounds(592, ($y + 8), 120, 40)
            $form.ClientSize = New-Object System.Drawing.Size(736, ($y + 62 + 33))
            $status.SetBounds(1, ($form.ClientSize.Height - 33), 734, 32)
            # Keep the whole window on the screen when it grows.
            $area = [System.Windows.Forms.Screen]::FromControl($form).WorkingArea
            if ($form.Bottom -gt $area.Bottom) { $form.Top = [math]::Max($area.Top, ($area.Bottom - $form.Height)) }
            $form.Invalidate()
        }
        # Only one of the two folding cards is open at a time.
        $quality.Add_Toggled({
            if ($quality.Open) { $windowCard.Open = $false }
            foreach ($c in $quality.Controls) { $c.Visible = $quality.Open }
            foreach ($c in $windowCard.Controls) { $c.Visible = $windowCard.Open }
            & $layout
        })
        $windowCard.Add_Toggled({
            if ($windowCard.Open) { $quality.Open = $false }
            foreach ($c in $windowCard.Controls) { $c.Visible = $windowCard.Open }
            foreach ($c in $quality.Controls) { $c.Visible = $quality.Open }
            & $layout
        })
        & $layout

    } catch {
        $script:windowError = $_.Exception.Message
        return
    }

    $result = Show-Form $form
    if ($result -ne [System.Windows.Forms.DialogResult]::OK) { $script:menuOutcome = "quit"; return }
    Save-Setting "runtime" ($runtimeKeys[$runtimeBox.SelectedIndex])
    Save-Setting "eye_width" ($widths[$resolution.Value])
    Save-Setting "aniso" ($anisoKeys[$anisoBox.SelectedIndex])
    Save-Setting "fxaa" ($fxaaKeys[$fxaaBox.SelectedIndex])
    Save-Setting "mask" ($maskKeys[$maskBox.SelectedIndex])
    Save-Setting "sharpen" ($sharpKeys[$sharpBox.SelectedIndex])
    Save-Setting "layer_scale" ($layerKeys[$layerBox.SelectedIndex])
    # The picture size is a setting now: an env line for it from before would override it.
    if (@($extraEnv | Where-Object { $_ -like "SHADPS4_VR_LAYER_SCALE=*" }).Count -gt 0) {
        Set-Content -Path $SettingsFile -Value @(Get-Content $SettingsFile | Where-Object { $_ -notmatch '^\s*env\s*=\s*SHADPS4_VR_LAYER_SCALE\s*=' }) -Encoding UTF8
    }
    Save-Setting "target_fps" ($targetKeys[$targetBox.SelectedIndex])
    Save-Setting "fov" ($fov.Value * 5)
    Save-Setting "pc_view" ($viewKeys[$viewBox.SelectedIndex])
    Save-Setting "pc_zoom" ($zoom.Value * 5)
    if ($motionEdition) {
        Save-Setting "controllers" ($(if ($moveOn.Checked) { "quest" } else { "dualsense" }))
        for ($i = 0; $i -lt 3; $i++) { Save-Setting $moveKeys[$i] ("" + $moveSliders[$i].Value) }
    }
    Save-Setting "menu" ($(if ($again.Checked) { "1" } else { "0" }))
    Set-NeoMode $neo.Checked
    Read-Settings
    $script:menuOutcome = "play"
}

# True to go on and play, false to quit.
function Show-Menu {
    Enable-ModernUi
    if ($script:modernUi) {
        $script:menuOutcome = "plain"
        $null = Show-MenuModern
        if ($script:menuOutcome -eq "quit") { return $false }
        if ($script:menuOutcome -eq "play") { return $true }
    }
    # The window could not be built: say why, and start the game with the saved settings.
    $why = $(if ($script:windowError) { $script:windowError } else { "unknown" })
    [void](Show-Box ("The settings window could not be built:`n`n" + $why + "`n`nThe game starts with the settings saved in settings.txt.") "OK" "Warning")
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
if (-not [double]::TryParse((Setting "mask" "0.9"), [System.Globalization.NumberStyles]::Float, [System.Globalization.CultureInfo]::InvariantCulture, [ref]$mask)) { $mask = 0.9 }
$mask = [math]::Max(0.0, [math]::Min(2.0, $mask))
if ($mask -gt 0) { $env:SHADPS4_VR_MASK = $mask.ToString([System.Globalization.CultureInfo]::InvariantCulture) }
# aniso: anisotropic filtering of the game's textures (0 as the game draws them, 2 to 16).
$aniso = 16
if (-not [int]::TryParse((Setting "aniso" "16"), [ref]$aniso)) { $aniso = 16 }
$aniso = [math]::Max(0, [math]::Min(16, $aniso))
$env:SHADPS4_FORCE_ANISO = "$aniso"
# fxaa: smoothing of the picture's edges on its way to the headset (0 off, up to 1). The game
# draws without multisampling, so msaa and antialias have nothing to work on here; this does.
$fxaa = 0.65
if (-not [double]::TryParse((Setting "fxaa" "0.65"), [System.Globalization.NumberStyles]::Float, [System.Globalization.CultureInfo]::InvariantCulture, [ref]$fxaa)) { $fxaa = 0.65 }
$fxaa = [math]::Max(0.0, [math]::Min(1.0, $fxaa))
if ($fxaa -gt 0) { $env:SHADPS4_VR_FXAA = $fxaa.ToString([System.Globalization.CultureInfo]::InvariantCulture) }
# pc_view: what the window on the PC shows (the headset is not affected): left or right, one eye
# cropped to fill the window; merged, both eyes joined into one picture; both, the picture with both
# lens circles. pc_zoom, in percent: 100 fits the window with no black corners, less is closer in.
# layer_scale: the picture handed to the headset's runtime is this percent of the size it is drawn
# at (shrunk here, with a smoother filter than the runtime's); 100 leaves it as it is.
# Not set: 50 for a large picture (2048 an eye and more), 100 for the others.
$layerPercent = $(if ($width -ge 2048) { 50.0 } else { 100.0 })
$layerParsed = 0.0
if ([double]::TryParse((Setting "layer_scale" ""), [System.Globalization.NumberStyles]::Float, [System.Globalization.CultureInfo]::InvariantCulture, [ref]$layerParsed)) { $layerPercent = $layerParsed }
if ($layerPercent -lt 100.0 -and $layerPercent -ge 25.0) {
    $env:SHADPS4_VR_LAYER_SCALE = ($layerPercent / 100.0).ToString([System.Globalization.CultureInfo]::InvariantCulture)
}
$pcView = Setting "pc_view" "left"
if (@("left", "right", "merged", "both") -contains $pcView) { $env:SHADPS4_PC_WINDOW = $pcView }
$pcZoom = 100.0
if ([double]::TryParse((Setting "pc_zoom" "100"), [System.Globalization.NumberStyles]::Float, [System.Globalization.CultureInfo]::InvariantCulture, [ref]$pcZoom) -and $pcZoom -ne 100.0) {
    $env:SHADPS4_PC_WINDOW_ZOOM = ([math]::Max(30.0, [math]::Min(200.0, $pcZoom)) / 100.0).ToString([System.Globalization.CultureInfo]::InvariantCulture)
}
# Motion edition: the Quest controllers play as Move wands when "controllers=quest" (the window sets
# it); move_pitch is in degrees, move_reach and move_turn in percent. What is not set is left to the
# emulator's own recommended values.
Remove-Item -Path Env:SHADPS4_QUEST_MOVE -ErrorAction SilentlyContinue
if ($motionEdition -and (Setting "controllers" "dualsense") -eq "quest") { $env:SHADPS4_QUEST_MOVE = "1" }
if ($env:SHADPS4_QUEST_MOVE -eq "1") {
    # With a PlayStation controller connected the game keeps using it and ignores the Quest
    # controllers, so ask for it to be unplugged (or turned off) first.
    while (Test-PlayStationPad) {
        $answer = Show-Box ("A PlayStation controller (DualSense) is connected to this PC." + [Environment]::NewLine + [Environment]::NewLine +
            "The game uses it instead of your Quest controllers, so they will not work while it is connected." + [Environment]::NewLine +
            "Unplug the USB cable, or turn it off (hold the PS button until the light goes out)." + [Environment]::NewLine + [Environment]::NewLine +
            "Retry: check again.   Ignore: play anyway.   Abort: quit.") "AbortRetryIgnore" "Warning"
        if ($answer -eq "Retry") { continue }
        if ($answer -eq "Abort") { exit 0 }
        break
    }
}
if ($env:SHADPS4_QUEST_MOVE -eq "1") {
    $moveEnv = @(@("move_pitch", "SHADPS4_QUEST_MOVE_PITCH", 1.0), @("move_reach", "SHADPS4_QUEST_MOVE_SCALE", 0.01), @("move_turn", "SHADPS4_QUEST_MOVE_TURN", 0.01))
    foreach ($entry in $moveEnv) {
        $number = 0.0
        $raw = Setting $entry[0] ""
        if ($raw -ne "" -and [double]::TryParse($raw, [System.Globalization.NumberStyles]::Float, [System.Globalization.CultureInfo]::InvariantCulture, [ref]$number)) {
            Set-Item -Path ("Env:" + $entry[1]) -Value (($number * $entry[2]).ToString([System.Globalization.CultureInfo]::InvariantCulture))
        }
    }
}
if ((Setting "msaa") -ne "") { $env:SHADPS4_MAX_MSAA = Setting "msaa" }
if ((Setting "antialias" "1") -eq "0") { $env:SHADPS4_RESOLVE_AA = "0" }
if ((Setting "hands" "1") -eq "0") { $env:SHADPS4_XR_HANDS = "0" }
if ((Setting "predict_ms") -ne "") { $env:SHADPS4_XR_PREDICT_MS = Setting "predict_ms" }
if ((Setting "stick_touchpad" "1") -eq "0") { $env:SHADPS4_STICK_TOUCHPAD = "0" }
if ((Setting "surround" "1") -eq "0") { $env:SHADPS4_VIRTUAL_SURROUND = "0" }
if ((Setting "real_time" "1") -eq "0") { $env:SHADPS4_TITLE_TIMESTEP = "0" }
$fovSetting = Setting "fov" "105"
if ($fovSetting -ne "100") { $env:SHADPS4_VR_FOV = $fovSetting }
# fov_of: what fov is a percent of. headset: what the headset being worn shows, all of it at 100
# (the emulator asks the headset as it starts). psvr: a PlayStation VR's, as the game was made.
if ((Setting "fov_of" "psvr") -ne "psvr") { $env:SHADPS4_VR_FOV_OF = "headset" }
# fps: the most frames a second. (pace, the older way to say it: refreshes of the headset a
# frame is given, 1 or more.)
$env:SHADPS4_VR_FPS_CAP = Setting "fps" "60"
# target_fps: the rate the emulator steers the game to (20 to 60), by changing how often it tells
# the game the headset has refreshed; 0 leaves the game's own timing.
$target = 60
if (-not [int]::TryParse((Setting "target_fps" "60"), [ref]$target)) { $target = 60 }
if ($target -ge 20) { $env:SHADPS4_VR_TARGET_FPS = "$([math]::Min(60, $target))" }
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
if ($env:SHADPS4_QUEST_MOVE -eq "1") {
    Say "Move controllers: your Quest controllers play as the game's two PlayStation Move wands."
    Say "Unplug the DualSense (or turn it off) first: while one is connected the game uses it and the Quest controllers do nothing."
    Say "In the game's start-up question choose the Move controllers, then press the grip button"
    Say "(the MOVE button) on the controller you hold. Right hand = the first wand."
    Say "Trigger = T, grip = MOVE, A/X = cross, B/Y = circle, stick click = square,"
    Say "stick up = triangle, stick down = select, left menu = start (hold it for a second to reset the view)."
} else {
    Say "The DualSense: connect it to THIS PC (USB cable, or Bluetooth paired with the PC). Paired with"
    Say "the headset, it reaches the PC through Virtual Desktop without motion sensors or touchpad."
    Say "Where it is in the game comes from your hands: hand tracking on in the headset, and in"
    Say "Virtual Desktop's settings hand tracking forwarded to the PC."
    Say "Hold OPTIONS for a second (or press the PS button) to reset the view."
    Say "The DualSense is what plays the game: the headset's own controllers do not."
}
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
