// RushVR.exe: the one file to double-click.
//
// Packed build (made by tools\make-single-exe.ps1): the emulator, the launcher and its helper
// tools are inside this file as a zip. On first start, and whenever this file is a newer build,
// they are unpacked to %LOCALAPPDATA%\RushVR (or the folder named by the RUSHVR_HOME variable),
// then the launcher (pc-vr\launch.ps1) is started from there. The settings file and the user
// folder (saves, configuration, logs) are never overwritten by an update. The launcher asks for
// the game folder or .pkg and remembers it; nothing of the game is inside this file.
//
// Plain build (made by tools\make-launcher-exe.ps1, no zip inside): starts pc-vr\launch.ps1 from
// the folder this file is in, like "Play Rush of Blood.bat" does.
using System;
using System.Diagnostics;
using System.IO;
using System.IO.Compression;
using System.Reflection;
using System.Text;

internal static class RushVR
{
    private static string Quote(string text)
    {
        // Windows command-line quoting: backslashes before a quote are doubled, the quote escaped.
        var sb = new StringBuilder("\"");
        int slashes = 0;
        foreach (char c in text)
        {
            if (c == '\\') { slashes++; continue; }
            if (c == '"') { sb.Append('\\', slashes * 2 + 1).Append('"'); slashes = 0; continue; }
            sb.Append('\\', slashes).Append(c);
            slashes = 0;
        }
        sb.Append('\\', slashes * 2).Append('"');
        return sb.ToString();
    }

    private static int Fail(string message)
    {
        Console.Error.WriteLine(message);
        Console.Error.WriteLine("Press a key to close.");
        try { Console.ReadKey(true); } catch { }
        return 1;
    }

    private static string DataRoot()
    {
        string custom = Environment.GetEnvironmentVariable("RUSHVR_HOME");
        if (!string.IsNullOrEmpty(custom)) return Path.GetFullPath(custom);
        return Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData), "RushVR");
    }

    // Files that belong to the player and are only written when they do not exist yet.
    private static bool IsUserFile(string name)
    {
        string n = name.Replace('\\', '/').ToLowerInvariant();
        return n == "pc-vr/settings.txt" || n.StartsWith("pc-vr/user/");
    }

    private static void Unpack(Stream zipStream, string root)
    {
        string rootFull = Path.GetFullPath(root).TrimEnd('\\') + "\\";
        Directory.CreateDirectory(root);
        using (var zip = new ZipArchive(zipStream, ZipArchiveMode.Read))
        {
            foreach (ZipArchiveEntry entry in zip.Entries)
            {
                string target = Path.GetFullPath(Path.Combine(root, entry.FullName.Replace('/', '\\')));
                if (!target.StartsWith(rootFull, StringComparison.OrdinalIgnoreCase)) continue; // never leave the folder
                if (entry.FullName.EndsWith("/") || entry.Name.Length == 0)
                {
                    Directory.CreateDirectory(target); // empty folders matter (the emulator's user home)
                    continue;
                }
                if (IsUserFile(entry.FullName) && File.Exists(target)) continue;
                Directory.CreateDirectory(Path.GetDirectoryName(target));
                entry.ExtractToFile(target, true);
            }
        }
    }

    private static string ReadText(Stream stream)
    {
        using (var reader = new StreamReader(stream, Encoding.UTF8)) return reader.ReadToEnd().Trim();
    }

    private static int Main(string[] args)
    {
        try { Console.Title = "RushVR"; } catch { }
        Assembly self = Assembly.GetExecutingAssembly();
        string dir = AppDomain.CurrentDomain.BaseDirectory;

        using (Stream payload = self.GetManifestResourceStream("payload.zip"))
        {
            if (payload != null)
            {
                string stamp;
                using (Stream stampStream = self.GetManifestResourceStream("payload.stamp"))
                    stamp = stampStream != null ? ReadText(stampStream) : "unstamped";
                dir = DataRoot();
                string stampFile = Path.Combine(dir, ".rushvr-payload");
                string installed = File.Exists(stampFile) ? File.ReadAllText(stampFile).Trim() : "";
                if (installed != stamp || !File.Exists(Path.Combine(dir, "pc-vr", "launch.ps1")))
                {
                    Console.WriteLine(installed == "" ? "Setting up RushVR (first start only)..." : "Updating RushVR...");
                    try
                    {
                        Unpack(payload, dir);
                        File.WriteAllText(stampFile, stamp);
                    }
                    catch (Exception e)
                    {
                        return Fail("RushVR could not unpack itself into " + dir + ":\r\n" + e.Message +
                                    "\r\nClose the game and the emulator if they are running, and try again.");
                    }
                }
            }
        }

        string script = Path.Combine(dir, "pc-vr", "launch.ps1");
        if (!File.Exists(script))
            return Fail("RushVR could not find pc-vr\\launch.ps1 in " + dir + ".\r\n" +
                        "Keep RushVR.exe inside the folder you unpacked, with the pc-vr folder beside it.");

        var arguments = new StringBuilder("-NoProfile -ExecutionPolicy Bypass -File ");
        arguments.Append(Quote(script));
        foreach (string arg in args) arguments.Append(' ').Append(Quote(arg));

        var info = new ProcessStartInfo("powershell.exe", arguments.ToString())
        {
            UseShellExecute = false,
            WorkingDirectory = dir,
        };
        try
        {
            using (Process process = Process.Start(info))
            {
                process.WaitForExit();
                return process.ExitCode;
            }
        }
        catch (Exception e)
        {
            return Fail("RushVR could not start PowerShell: " + e.Message);
        }
    }
}
