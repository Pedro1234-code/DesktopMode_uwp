using factoryos_10x_shell.Library.Events;
using factoryos_10x_shell.Library.Models.InternalData;
using factoryos_10x_shell.Library.Services.Helpers;
using System;
using System.Collections.Generic;
using System.Collections.ObjectModel;
using System.Diagnostics;
using System.Linq;
using System.Text;
using System.Threading.Tasks;
using Windows.ApplicationModel;
using Windows.ApplicationModel.Core;
using Windows.Foundation;
using Windows.Management.Deployment;
using Windows.Storage;
using Windows.Storage.Streams;
using Windows.System;
using Newtonsoft.Json;
using Windows.UI.Xaml.Media.Imaging;
using Windows.UI.StartScreen;
using Windows.UI.Xaml.Controls;
using Windows.UI.Xaml;
using Windows.Data.Xml.Dom;
using Windows.System.Profile;


namespace factoryos_10x_shell.Services.Helpers
{
    public class AppHelper : IAppHelper
    {
        private DispatcherQueue m_dispatcherQueue;

        private Size _logoSize;
        public ObservableCollection<StartIconModel> StartIcons { get; set; } = new ObservableCollection<StartIconModel>();
        public ObservableCollection<StartIconModel> TaskbarIcons { get; set; } = new ObservableCollection<StartIconModel>();

        private List<StartIconModel> _iconCache { get; set; }

        public PackageManager PackageManager { get; set; }

        private Dictionary<string, string> m_pkgFamilyMap;
        private readonly Dictionary<string, string> m_appUriByAumid;

        public AppHelper() 
        {
            m_dispatcherQueue = DispatcherQueue.GetForCurrentThread();
            PackageManager = new PackageManager();
            m_pkgFamilyMap = new Dictionary<string, string>(StringComparer.OrdinalIgnoreCase);
            m_appUriByAumid = new Dictionary<string, string>(StringComparer.OrdinalIgnoreCase);

            _logoSize = new Size(176, 176);
            _iconCache = new List<StartIconModel>();
        }

        public List<StartIconModel> LoadTaskbarPinnedApps()
        {
            var localSettings = ApplicationData.Current.LocalSettings;
            var list = new List<StartIconModel>();

            var pinned = localSettings.Values
                .Where(kv => kv.Key.StartsWith("PinnedApp_"))
                .OrderBy(kv => kv.Key) // garante ordem correta
                .Select(kv => kv.Value as ApplicationDataCompositeValue);
            
            foreach (var composite in pinned)
            {
                var model = new StartIconModel
                {
                    AppId = composite["AppId"] as string,
                    Aumid = composite["Aumid"] as string,
                    IconName = composite["IconName"] as string,
                    AppUri = composite["AppUri"] as string
                };

                list.Add(model);
            }

            return list;
        }


        public void SaveTaskbarPinnedApps(List<StartIconModel> apps)
        {
            var localSettings = ApplicationData.Current.LocalSettings;

            // Limpa apps antigos
            var keysToRemove = localSettings.Values.Keys.Where(k => k.StartsWith("PinnedApp_")).ToList();
            foreach (var key in keysToRemove)
            {
                localSettings.Values.Remove(key);
            }

            // Salva os novos
            for (int i = 0; i < apps.Count; i++)
            {
                var app = apps[i];
                var composite = new ApplicationDataCompositeValue
                {
                    ["AppId"] = app.AppId,
                    ["Aumid"] = app.Aumid,
                    ["IconName"] = app.IconName,
                    ["AppUri"] = app.AppUri
                };

                localSettings.Values[$"PinnedApp_{i}"] = composite;
            }
        }

        public async Task<StartIconModel> CreateStartIconModelFromEntryAsync(AppListEntry entry, Package logoPackage = null)
        {
            var displayInfo = entry.DisplayInfo;
            // The Start Menu historically uses the package logo. It can differ from
            // DisplayInfo.GetLogo for packages with multiple app entries, so taskbar
            // restoration must use this same source as well.
            var logoRef = logoPackage != null
                ? logoPackage.GetLogoAsRandomAccessStreamReference(_logoSize)
                : displayInfo.GetLogo(_logoSize);
            var stream = await logoRef.OpenReadAsync();

            var bitmap = new BitmapImage();
            await bitmap.SetSourceAsync(stream);

            string appUserModelId = entry.AppUserModelId;
            string aumid = null;

            try
            {
                // Extrair AUMID real
                string[] parts = appUserModelId.Split('!');
                if (parts.Length == 2)
                {
                    string packageFamilyName = parts[0];
                    string appId = parts[1];
                    aumid = $"{packageFamilyName}!{appId}";
                }
                else
                {
                    Debug.WriteLine($"[ERRO] AppUserModelId inválido: {appUserModelId}");
                }
            }
            catch (Exception ex)
            {
                Debug.WriteLine($"[ERRO] Falha ao construir AUMID: {ex.Message}");
            }

            return new StartIconModel
            {
                AppId = appUserModelId,
                Aumid = aumid,
                IconName = displayInfo.DisplayName,
                IconSource = bitmap,
                AppUri = GetKnownAppUri(appUserModelId),
                Data = entry
            };
        }

        public async Task LoadPinnedTaskbarAppsAsync()
        {
            var localSettings = ApplicationData.Current.LocalSettings;
            var loaded = new List<StartIconModel>();
            bool requiresPinMigration = false;

            foreach (var key in localSettings.Values.Keys)
            {
                if (!key.StartsWith("PinnedApp_")) continue;

                var composite = localSettings.Values[key] as ApplicationDataCompositeValue;
                if (composite == null) continue;

                var appId = composite["AppId"] as string;
                var aumid = composite["Aumid"] as string;
                var iconName = composite["IconName"] as string;
                var appUri = composite["AppUri"] as string;

                // Files is a CoreShell window, not an installed UWP package. It has
                // no AppListEntry to recover, so restore its pin directly.
                if (string.Equals(appId, "CoreShell.Files", StringComparison.OrdinalIgnoreCase) ||
                    string.Equals(appId, "CoreShell.Notepad", StringComparison.OrdinalIgnoreCase) ||
                    string.Equals(appId, "CoreShell.Settings", StringComparison.OrdinalIgnoreCase) ||
                    string.Equals(appId, "CoreShell.Calculator", StringComparison.OrdinalIgnoreCase) ||
                    string.Equals(appId, "CoreShell.Firefox", StringComparison.OrdinalIgnoreCase))
                {
                    loaded.Add(new StartIconModel
                    {
                        AppId = appId,
                        // This is the integrated browser, not a Firefox package entry.
                        // Keep its Start name stable even for pins saved before the rename.
                        IconName = string.Equals(appId, "CoreShell.Firefox", StringComparison.OrdinalIgnoreCase)
                            ? "Strawfox"
                            : string.IsNullOrWhiteSpace(iconName)
                            ? (string.Equals(appId, "CoreShell.Notepad", StringComparison.OrdinalIgnoreCase) ? "Notepad" : string.Equals(appId, "CoreShell.Settings", StringComparison.OrdinalIgnoreCase) ? "Settings" : string.Equals(appId, "CoreShell.Calculator", StringComparison.OrdinalIgnoreCase) ? "Calculator" : string.Equals(appId, "CoreShell.Firefox", StringComparison.OrdinalIgnoreCase) ? "Firefox" : "Files")
                            : iconName
                    });
                    continue;
                }

                // Older pins did not persist the AUMID. AppId is a valid fallback
                // and lets us migrate those entries to the stable lookup path.
                var savedModel = new StartIconModel { AppId = appId, Aumid = aumid, IconName = iconName };
                var entry = !string.IsNullOrEmpty(aumid)
                    ? await GetAppListEntryFromAumidAsync(aumid)
                    : await GetAppListEntryFromModelAsync(savedModel);
                if (entry == null)
                {
                    Debug.WriteLine($"[ERRO] Não foi possível recuperar AppListEntry para {appId}");
                    continue;
                }

                var logoPackage = GetPackageFromAumid(entry.AppUserModelId);
                var model = await CreateStartIconModelFromEntryAsync(entry, logoPackage);

                // Keep a previously stored protocol only when it could not be
                // rediscovered during this session.
                model.AppUri = model.AppUri ?? appUri;
                requiresPinMigration |= string.IsNullOrEmpty(aumid);

                loaded.Add(model);
            }

            var dispatcher = Windows.ApplicationModel.Core.CoreApplication.MainView.CoreWindow.Dispatcher;

            await dispatcher.RunAsync(Windows.UI.Core.CoreDispatcherPriority.Normal, () =>
            {
                TaskbarIcons.Clear();
                foreach (var item in loaded)
                    TaskbarIcons.Add(item);
            });

            if (requiresPinMigration)
                SaveTaskbarPinnedApps(loaded);

            Debug.WriteLine($"[INFO] Loaded pinned apps count: {loaded.Count}");
        }



        public async Task LoadAppsAsync()
        {
            try
            {
                _iconCache.Clear(); // Clear the cache before reloading
                m_appUriByAumid.Clear();
                PackageManager packageManager = new PackageManager();
                IEnumerable<Package> packages = packageManager.FindPackagesForUser("");

                foreach (Package package in packages)
                {
                    if (!package.IsFramework && !package.IsResourcePackage && !package.IsStub && package.GetAppListEntries().FirstOrDefault() != null)
                    {
                        m_pkgFamilyMap[package.Id.FamilyName] = package.Id.FullName;
                        try
                        {
                            IReadOnlyList<AppListEntry> entries = package.GetAppListEntries();
                            foreach (AppListEntry entry in entries)
                            {
                                string appUri = await GetPackageProtocolUriAsync(package, entry.AppUserModelId);
                                if (!string.IsNullOrEmpty(appUri))
                                    m_appUriByAumid[entry.AppUserModelId] = appUri;

                                // Keep the existing Start Menu visual: use the package
                                // logo here and during taskbar restoration.
                                StartIconModel model = await CreateStartIconModelFromEntryAsync(entry, package);
                                model.AppUri = appUri;
                                _iconCache.Add(model);
                            }
                        }
                        catch (Exception ex)
                        {
                            Debug.WriteLine($"Error accessing logo for package {package.Id.FullName}: {ex.Message}");
                        }
                    }
                }

                // Keep the collection instance stable because StartMenu binds to
                // it before deferred app discovery completes.  The CoreShell
                // entries are injected by StartMenuViewModel and must survive
                // this refresh; clearing them was why the internal apps
                // disappeared after startup became asynchronous.
                List<StartIconModel> internalIcons = StartIcons
                    .Where(icon => icon.AppId != null &&
                                   icon.AppId.StartsWith("CoreShell.", StringComparison.OrdinalIgnoreCase))
                    .ToList();
                StartIcons.Clear();
                foreach (StartIconModel icon in internalIcons.OrderBy(icon => icon.AppId))
                    StartIcons.Add(icon);
                foreach (StartIconModel icon in _iconCache.OrderBy(icon => icon.IconName))
                    StartIcons.Add(icon);
            }
            catch (Exception ex)
            {
                Debug.WriteLine("LoadApps => Get: " + ex.Message);
            }
        }
        public Package PackageFromAumid(string aumid)
        {
            string[] aumidParts = aumid.Split('!');
            string packageFamilyName = aumidParts[0];

            if (m_pkgFamilyMap.TryGetValue(packageFamilyName, out string packageFullName))
            {
                return PackageManager.FindPackageForUser(string.Empty, packageFullName);
            }
            return null;
        }

        private Package GetPackageFromAumid(string aumid)
        {
            if (string.IsNullOrWhiteSpace(aumid))
                return null;

            string[] parts = aumid.Split('!');
            if (parts.Length == 0 || string.IsNullOrWhiteSpace(parts[0]))
                return null;

            return PackageManager.FindPackagesForUser(string.Empty, parts[0]).FirstOrDefault();
        }

        public async Task<AppListEntry> GetAppListEntryFromAumidAsync(string aumid)
        {
            var pm = new PackageManager();
            var packages = pm.FindPackagesForUserWithPackageTypes("", PackageTypes.Main);

            foreach (var package in packages)
            {
                try
                {
                    var entries = await package.GetAppListEntriesAsync();
                    foreach (var entry in entries)
                    {
                        if (entry.AppUserModelId == aumid)
                        {
                            return entry;
                        }
                    }
                }
                catch
                {
                    Debug.WriteLine($"[AppDisplayInfo] Failed to convert AUMID");
                }
            }

            return null;
        }

        public async Task<AppListEntry> GetAppListEntryFromModelAsync(StartIconModel model)
        {
            if (string.IsNullOrEmpty(model.AppId))
                return null;

            var pm = new PackageManager();
            var allPackages = pm.FindPackagesForUser(string.Empty);

            foreach (var pkg in allPackages)
            {
                IReadOnlyList<AppListEntry> entries = null;

                try
                {
                    entries = await pkg.GetAppListEntriesAsync();
                }
                catch
                {
                    continue;
                }

                foreach (var entry in entries)
                {
                    if (entry.AppUserModelId == model.AppId || entry.DisplayInfo.DisplayName == model.Name)
                    {
                        return entry;
                    }
                }
            }

            return null;
        }

        public async Task<bool> LaunchAppAsync(StartIconModel model)
        {
            if (model == null)
                return false;

            // Edge's package activation/protocol path is unreliable on Xbox. A web
            // URL is routed by the shell to the system's default browser (Edge).
            if (IsMicrosoftEdge(model))
                return await Launcher.LaunchUriAsync(new Uri("https://google.com"));

            string appUri = model.AppUri ?? GetKnownAppUri(model.AppId);
            if (!string.IsNullOrWhiteSpace(appUri) && Uri.TryCreate(appUri, UriKind.Absolute, out Uri uri))
            {
                try
                {
                    bool launched = await Launcher.LaunchUriAsync(uri);
                    if (launched || IsXboxDevice())
                        return launched;
                }
                catch (Exception ex)
                {
                    Debug.WriteLine($"[AppUri] Launch failed for {appUri}: {ex.Message}");
                    if (IsXboxDevice())
                        return false;
                }
            }

            // AppListEntry is kept as the desktop fallback and for packages that do not
            // publish a URI protocol. Xbox prioritizes the URI path above.
            AppListEntry entry = model.Data as AppListEntry ?? await GetAppListEntryFromModelAsync(model);
            return entry != null && await entry.LaunchAsync();
        }

        private static bool IsMicrosoftEdge(StartIconModel model)
        {
            string identity = string.Join(" ", model.AppId, model.Aumid, model.IconName, model.Name);
            return identity.IndexOf("MicrosoftEdge", StringComparison.OrdinalIgnoreCase) >= 0 ||
                   identity.IndexOf("Microsoft Edge", StringComparison.OrdinalIgnoreCase) >= 0;
        }

        private string GetKnownAppUri(string appUserModelId)
        {
            if (!string.IsNullOrEmpty(appUserModelId) && m_appUriByAumid.TryGetValue(appUserModelId, out string appUri))
                return appUri;

            return null;
        }

        private static bool IsXboxDevice()
        {
            return string.Equals(
                AnalyticsInfo.VersionInfo.DeviceFamily,
                "Windows.Xbox",
                StringComparison.OrdinalIgnoreCase);
        }

        private static async Task<string> GetPackageProtocolUriAsync(Package package, string appUserModelId)
        {
            if (package == null || string.IsNullOrEmpty(appUserModelId))
                return null;

            try
            {
                StorageFile manifestFile = await package.InstalledLocation.GetFileAsync("AppxManifest.xml");
                string manifestText = await FileIO.ReadTextAsync(manifestFile);
                var manifest = new XmlDocument();
                manifest.LoadXml(manifestText);

                string appId = appUserModelId.Substring(appUserModelId.LastIndexOf('!') + 1);
                XmlNodeList nodes = manifest.GetElementsByTagName("*");
                for (uint index = 0; index < nodes.Length; index++)
                {
                    IXmlNode node = nodes.Item(index);
                    if (!string.Equals(node.NodeName, "uap:Protocol", StringComparison.OrdinalIgnoreCase) &&
                        !string.Equals(node.NodeName, "Protocol", StringComparison.OrdinalIgnoreCase))
                        continue;

                    if (!string.Equals(GetOwningApplicationId(node), appId, StringComparison.OrdinalIgnoreCase))
                        continue;

                    string scheme = node.Attributes?.GetNamedItem("Name")?.NodeValue as string;
                    if (!string.IsNullOrWhiteSpace(scheme) && Uri.TryCreate(scheme + ":", UriKind.Absolute, out Uri uri))
                        return uri.AbsoluteUri;
                }
            }
            catch (Exception ex)
            {
                Debug.WriteLine($"[AppUri] Could not read protocol for {package.Id.Name}: {ex.Message}");
            }

            return null;
        }

        private static string GetOwningApplicationId(IXmlNode node)
        {
            for (IXmlNode current = node.ParentNode; current != null; current = current.ParentNode)
            {
                if (string.Equals(current.NodeName, "Application", StringComparison.OrdinalIgnoreCase))
                    return current.Attributes?.GetNamedItem("Id")?.NodeValue as string;
            }

            return null;
        }
    }
}
