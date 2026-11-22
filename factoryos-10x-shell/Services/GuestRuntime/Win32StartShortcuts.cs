using factoryos_10x_shell.Library.Models.InternalData;
using System;
using System.Collections.Generic;
using System.IO;
using System.Linq;
using System.Text;
using System.Threading.Tasks;
using System.Text.RegularExpressions;
using Windows.Storage;
using Windows.UI.Xaml.Media;
using Win32Bridge;

namespace factoryos_10x_shell.Services.Win32
{
    internal sealed class Win32StartShortcut
    {
        public StorageFile Executable { get; set; }
        public StorageFolder Folder { get; set; }
        public string VirtualPath { get; set; }
        public string Arguments { get; set; }
        public string WorkingDirectory { get; set; }
    }

    internal static class Win32StartShortcuts
    {
        private const string AppIdPrefix = "CoreShell.Win32Shortcut.";

        internal static bool IsShortcutId(string appId) =>
            appId?.StartsWith(AppIdPrefix, StringComparison.OrdinalIgnoreCase) == true;
        private static readonly string[] Locations =
        {
            @"C:\Users\Default\AppData\Roaming\Microsoft\Windows\Start Menu",
            @"C:\ProgramData\Microsoft\Windows\Start Menu"
        };
        private static readonly Dictionary<string, string> GuestVariables =
            new Dictionary<string, string>(StringComparer.OrdinalIgnoreCase)
            {
                ["SystemDrive"] = "C:",
                ["SystemRoot"] = @"C:\Windows",
                ["windir"] = @"C:\Windows",
                ["ProgramFiles"] = @"C:\Program Files",
                ["ProgramW6432"] = @"C:\Program Files",
                ["ProgramData"] = @"C:\ProgramData",
                ["ALLUSERSPROFILE"] = @"C:\ProgramData",
                ["USERPROFILE"] = @"C:\Users\Default",
                ["APPDATA"] = @"C:\Users\Default\AppData\Roaming",
                ["LOCALAPPDATA"] = @"C:\Users\Default\AppData\Local"
            };

        public static async Task<IReadOnlyList<StartIconModel>> ReadAsync()
        {
            var report = new StringBuilder();
            try { return await ReadAsync(report); }
            catch (Exception exception)
            {
                report.AppendLine($"Scan failed (0x{exception.HResult:X8}): {exception}");
                throw;
            }
            finally
            {
                try
                {
                    if (factoryos_10x_shell.Services.Helpers.ShellDiagnostics.IsLoggingEnabled)
                    {
                        var log = await ApplicationData.Current.LocalFolder.CreateFileAsync(
                            "Win32Bridge-start-shortcuts.log", CreationCollisionOption.ReplaceExisting);
                        if (factoryos_10x_shell.Services.Helpers.ShellDiagnostics.IsLoggingEnabled)
                            await FileIO.WriteTextAsync(log, report.ToString());
                    }
                }
                catch (Exception exception)
                {
                    factoryos_10x_shell.Services.Helpers.ShellDiagnostics.WriteDebug(exception.Message);
                }
            }
        }

        private static async Task<IReadOnlyList<StartIconModel>> ReadAsync(StringBuilder report)
        {
            var entries = new List<StartIconModel>();
            if (!Win32WindowManagerService.Instance.IsAppSupportEnabled)
            {
                report.AppendLine("Win32 app support disabled.");
                return entries;
            }
            if (!(await ApplicationData.Current.LocalFolder.TryGetItemAsync("drive_c") is StorageFolder drive))
            {
                report.AppendLine("LocalState drive_c not found.");
                return entries;
            }
            string drivePath = drive.Path;

            var seen = new HashSet<string>(StringComparer.OrdinalIgnoreCase);
            foreach (string location in Locations)
            {
                report.AppendLine($"{location}: scanning.");
                var folders = new Stack<(string VirtualPath, string MenuPath)>();
                folders.Push((location, ""));
                while (folders.Count != 0)
                {
                    var folder = folders.Pop();
                    string folderPath = Path.Combine(drivePath, folder.VirtualPath.Substring(3));
                    IReadOnlyList<string> children;
                    IReadOnlyList<string> names;
                    try
                    {
                        var directory = await Task.Run(() => RuntimeSession.ListShellShortcutDirectory(folderPath));
                        children = directory?.Folders ?? Array.Empty<string>();
                        names = directory?.Shortcuts ?? Array.Empty<string>();
                        report.AppendLine($"{folder.VirtualPath}: {children.Count} subfolder(s), {names.Count} shortcut(s).");
                    }
                    catch (Exception exception)
                    {
                        report.AppendLine($"{folder.VirtualPath}: enumeration failed (0x{exception.HResult:X8}): {exception.Message}");
                        continue;
                    }
                    foreach (string child in children)
                        folders.Push((folder.VirtualPath + "\\" + child, folder.MenuPath + child + "\\"));
                    foreach (string name in names)
                    {
                        string menuPath = folder.MenuPath + name;
                        if (seen.Contains(menuPath)) continue;
                        var entry = await ReadEntryAsync(drive, folderPath, name, folder.VirtualPath, menuPath, report);
                        if (entry != null && seen.Add(menuPath)) entries.Add(entry);
                    }
                }
            }
            report.AppendLine($"Entries: {entries.Count}");
            return entries.OrderBy(entry => entry.IconName, StringComparer.CurrentCultureIgnoreCase).ToList();
        }

        internal static async Task<StartIconModel> ResolveAsync(string appId)
        {
            if (!IsShortcutId(appId) || !Win32WindowManagerService.Instance.IsAppSupportEnabled)
                return null;

            string menuPath = appId.Substring(AppIdPrefix.Length);
            string[] parts = menuPath.Split('\\');
            if (!menuPath.EndsWith(".lnk", StringComparison.OrdinalIgnoreCase) ||
                parts.Any(part => string.IsNullOrWhiteSpace(part) || part == "." || part == ".." ||
                                  part.IndexOfAny(Path.GetInvalidFileNameChars()) >= 0))
                return null;

            if (!(await ApplicationData.Current.LocalFolder.TryGetItemAsync("drive_c") is StorageFolder drive))
                return null;

            string relativeFolder = Path.GetDirectoryName(menuPath);
            foreach (string location in Locations)
            {
                string shortcutFolder = string.IsNullOrEmpty(relativeFolder) ? location : Path.Combine(location, relativeFolder);
                string folderPath = Path.Combine(drive.Path, shortcutFolder.Substring(3));
                var entry = await ReadEntryAsync(drive, folderPath, parts[parts.Length - 1],
                    shortcutFolder, menuPath, new StringBuilder());
                if (entry != null) return entry;
            }
            return null;
        }


        private static async Task<StartIconModel> ReadEntryAsync(StorageFolder drive, string folderPath, string name,
            string shortcutFolder, string menuPath, StringBuilder report)
        {
            try
            {
                var link = await Task.Run(() => RuntimeSession.ReadShellShortcutFile(folderPath, name));
                if (link == null)
                {
                    report.AppendLine($"{menuPath}: invalid Shell Link.");
                    return null;
                }
                foreach (string candidate in new[] { link.Target, link.RelativePath })
                {
                    string path = NormalizePath(candidate, shortcutFolder);
                    if (path == null || !path.EndsWith(".exe", StringComparison.OrdinalIgnoreCase)) continue;
                    string[] components = path.Substring(3).Split('\\');
                    var folder = await FindFolderAsync(drive, components.Take(components.Length - 1));
                    if (folder == null ||
                        !(await folder.TryGetItemAsync(components[components.Length - 1]) is StorageFile executable))
                    {
                        report.AppendLine($"{menuPath}: target missing at {path}.");
                        continue;
                    }

                    string workingDirectory = NormalizePath(link.WorkingDirectory, shortcutFolder);
                    if (workingDirectory != null &&
                        await FindFolderAsync(drive, workingDirectory.Substring(3).Split('\\')) == null)
                        workingDirectory = null;
                    report.AppendLine($"{menuPath}: added ({path}).");
                    return new StartIconModel
                    {
                        AppId = AppIdPrefix + menuPath,
                        IconName = Path.GetFileNameWithoutExtension(name),
                        IconGlyph = "\uE7C3",
                        Data = new Win32StartShortcut
                        {
                            Executable = executable,
                            Folder = folder,
                            VirtualPath = path,
                            Arguments = link.Arguments,
                            WorkingDirectory = workingDirectory
                        }
                    };
                }
                report.AppendLine($"{menuPath}: no accessible executable target (absolute='{link.Target}', relative='{link.RelativePath}').");
            }
            catch (Exception exception)
            {
                report.AppendLine($"{menuPath}: read failed (0x{exception.HResult:X8}): {exception.Message}");
            }
            return null;
        }

        public static async Task LoadIconsAsync(IReadOnlyList<StartIconModel> entries)
        {
            var icons = new Dictionary<string, ImageSource>(StringComparer.OrdinalIgnoreCase);
            foreach (var entry in entries)
            {
                var shortcut = (Win32StartShortcut)entry.Data;
                if (!icons.TryGetValue(shortcut.VirtualPath, out ImageSource icon))
                {
                    try { icon = await RuntimeSession.ReadExecutableIconAsync(shortcut.Executable); }
                    catch (Exception) { icon = null; }
                    icons[shortcut.VirtualPath] = icon;
                }
                if (icon == null) continue;
                entry.IconSource = icon;
                entry.IconGlyph = string.Empty;
            }
        }

        private static async Task<StorageFolder> FindFolderAsync(StorageFolder root, IEnumerable<string> components)
        {
            foreach (string component in components)
            {
                if (component.Length == 0) continue;
                root = await root.TryGetItemAsync(component) as StorageFolder;
                if (root == null) break;
            }
            return root;
        }

        private static string NormalizePath(string path, string baseDirectory)
        {
            if (string.IsNullOrWhiteSpace(path)) return null;
            path = Regex.Replace(path, "%([^%]+)%", match =>
                GuestVariables.TryGetValue(match.Groups[1].Value, out string value) ? value : match.Value);
            return path.IndexOf('%') >= 0 ? null : RuntimeSession.ResolveShortcutPath(path, baseDirectory);
        }
    }
}
