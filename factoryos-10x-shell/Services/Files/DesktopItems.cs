using factoryos_10x_shell.Library.Models.InternalData;
using factoryos_10x_shell.Library.Services.Helpers;
using factoryos_10x_shell.Library.Services.WebApps;
using factoryos_10x_shell.Services.Helpers;
using factoryos_10x_shell.Services.Win32;
using Microsoft.Extensions.DependencyInjection;
using System;
using System.IO;
using System.Linq;
using System.Threading.Tasks;
using Windows.Storage;

namespace factoryos_10x_shell.Services.Files
{
    internal static class DesktopItems
    {
        internal const string FolderName = "Desktop";
        internal const string ShortcutExtension = ".dmslink";
        internal static event Action ShortcutCreated;

        internal static async Task<StorageFolder> GetFolderAsync() =>
            await ApplicationData.Current.LocalFolder.CreateFolderAsync(FolderName, CreationCollisionOption.OpenIfExists);

        internal static bool IsShortcut(StorageFile file) =>
            string.Equals(file?.FileType, ShortcutExtension, StringComparison.OrdinalIgnoreCase);

        internal static async Task CreateShortcutAsync(string id, string displayName)
        {
            if (string.IsNullOrWhiteSpace(id)) return;
            string name = string.Concat((displayName ?? "").Select(ch =>
                Path.GetInvalidFileNameChars().Contains(ch) ? '_' : ch)).Trim(' ', '.');
            if (string.IsNullOrEmpty(name)) name = "App";
            var folder = await GetFolderAsync();
            var file = await folder.CreateFileAsync(name + ShortcutExtension, CreationCollisionOption.GenerateUniqueName);
            try
            {
                await FileIO.WriteTextAsync(file, id);
            }
            catch
            {
                await file.DeleteAsync();
                throw;
            }
            ShortcutCreated?.Invoke();
        }

        internal static async Task<string> ReadShortcutIdAsync(StorageFile file)
        {
            if (!IsShortcut(file)) return null;
            try
            {
                if ((await file.GetBasicPropertiesAsync()).Size > 4096) return null;
                return (await FileIO.ReadTextAsync(file)).Trim();
            }
            catch (FileNotFoundException) { return null; }
        }

        internal static async Task<bool> OpenShortcutAsync(StorageFile file)
        {
            string id = await ReadShortcutIdAsync(file);
            if (string.IsNullOrWhiteSpace(id)) return false;
            if (id.StartsWith("web:", StringComparison.Ordinal))
            {
                var app = App.ServiceProvider.GetRequiredService<IWebAppService>().InstalledApps
                    .FirstOrDefault(candidate => "web:" + candidate.Id == id);
                if (app == null) return false;
                App.ServiceProvider.GetRequiredService<IWindowManagerService>().Open(app);
                return true;
            }

            var helper = App.ServiceProvider.GetRequiredService<AppHelper>();
            var model = helper.StartIcons.FirstOrDefault(app => string.Equals(app.AppId, id, StringComparison.OrdinalIgnoreCase));
            if (model == null && Win32StartShortcuts.IsShortcutId(id))
                model = await Win32StartShortcuts.ResolveAsync(id);
            return await LaunchAppAsync(model ?? new StartIconModel { AppId = id }, helper, false);
        }

        internal static async Task<bool> LaunchAppAsync(StartIconModel model, IAppHelper appHelper, bool resetFiles)
        {
            if (model == null) return false;
            if (model.Data is Win32StartShortcut shortcut)
                return Win32WindowManagerService.Instance.Open(shortcut.Executable, shortcut.Folder, model.IconSource,
                    shortcut.VirtualPath, shortcut.Arguments, shortcut.WorkingDirectory) != null;

            switch (model.AppId)
            {
                case "CoreShell.Files": AppState.Instance.RequestFilesOpen(resetFiles); return true;
                case "CoreShell.Notepad": AppState.Instance.RequestNotepadOpen(); return true;
                case "CoreShell.Settings": AppState.Instance.RequestSettingsOpen(); return true;
                case "CoreShell.Calculator": AppState.Instance.RequestCalculatorOpen(); return true;
                case "CoreShell.Firefox": AppState.Instance.RequestFirefoxOpen(); return true;
                default: return await appHelper.LaunchAppAsync(model);
            }
        }
    }
}
