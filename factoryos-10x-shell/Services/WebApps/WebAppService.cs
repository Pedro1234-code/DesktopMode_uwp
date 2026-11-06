using factoryos_10x_shell.Library.Models.InternalData;
using factoryos_10x_shell.Library.Services.WebApps;
using System;
using System.Collections.Generic;
using System.Collections.ObjectModel;
using System.Linq;
using System.Threading.Tasks;
using Windows.Storage;
using Windows.Storage.Streams;
using Windows.UI.Xaml.Media.Imaging;
using Windows.Web.Http;

namespace factoryos_10x_shell.Services.WebApps
{
    public sealed class WebAppService : IWebAppService
    {
        private const string KeyPrefix = "CoreShellWebApp_";
        // Deliberately does not use KeyPrefix: Save() replaces every persisted
        // app entry carrying that prefix.
        private const string DefaultsSeededKey = "CoreShellDefaults_WebAppsSeeded_v1";
        private const string XboxDevicePortalId = "builtin-xbox-device-portal";
        private const string XboxDevicePortalUri = "https://localhost:11443/";
        private const string AppStoreSeededKey = "CoreShellDefaults_AppStoreSeeded_v1";
        private bool m_initialized;

        public ObservableCollection<WebAppDefinition> InstalledApps { get; } = new ObservableCollection<WebAppDefinition>();
        public event EventHandler AppsChanged;

        public Task InitializeAsync()
        {
            if (m_initialized) return Task.CompletedTask;
            m_initialized = true;
            var values = ApplicationData.Current.LocalSettings.Values;
            foreach (var pair in values.Where(pair => pair.Key.StartsWith(KeyPrefix)).OrderBy(pair => pair.Key))
            {
                var saved = pair.Value as ApplicationDataCompositeValue;
                if (saved == null) continue;
                string id = saved["Id"] as string;
                string startUri = saved["StartUri"] as string;
                InstalledApps.Add(new WebAppDefinition
                {
                    Id = id,
                    Name = saved["Name"] as string,
                    StartUri = startUri,
                    UseStrawfox = saved.TryGetValue("UseStrawfox", out object browser) && browser is bool strawfox && strawfox,
                    UseStrawfoxWindow = saved.TryGetValue("UseStrawfoxWindow", out object windowMode) && windowMode is bool separate && separate,
                    IconUri = saved["IconUri"] as string,
                    IsPinned = saved["IsPinned"] is bool isPinned && isPinned
                });
            }

            // Seed built-in web apps once per local profile. This keeps the app
            // installed by default while still respecting a later uninstall.
            if (!(values[DefaultsSeededKey] is bool defaultsSeeded && defaultsSeeded))
            {
                WebAppDefinition portal = InstalledApps.FirstOrDefault(app =>
                    string.Equals(app.Id, XboxDevicePortalId, StringComparison.OrdinalIgnoreCase) ||
                    string.Equals(app.StartUri, XboxDevicePortalUri, StringComparison.OrdinalIgnoreCase));
                if (portal == null)
                {
                    portal = new WebAppDefinition
                    {
                        Id = XboxDevicePortalId,
                        Name = "Xbox Device Portal",
                        StartUri = XboxDevicePortalUri
                    };
                    InstalledApps.Add(portal);
                }
                else
                {
                    portal.Name = "Xbox Device Portal";
                    portal.StartUri = XboxDevicePortalUri;
                }
                values[DefaultsSeededKey] = true;
                Save();
            }
            if (!(values[AppStoreSeededKey] is bool storeSeeded && storeSeeded))
            {
                const string storeUri = "https://discover.apps.chrome/";
                var store = InstalledApps.FirstOrDefault(app => app.Id == "builtin-app-store" ||
                    string.Equals(app.StartUri?.TrimEnd('/'), storeUri.TrimEnd('/'), StringComparison.OrdinalIgnoreCase));
                if (store == null)
                {
                    store = new WebAppDefinition { Id = "builtin-app-store" };
                    InstalledApps.Add(store);
                }
                store.Name = "App Store";
                store.StartUri = storeUri;
                store.IconUri = "ms-appx:///Assets/Firefox/app_mall_512.png";
                store.UseStrawfox = true;
                store.UseStrawfoxWindow = true;
                store.IsPinned = true;
                Save();
                values[AppStoreSeededKey] = true;
            }
            return Task.CompletedTask;
        }

        public async Task<WebAppDefinition> InstallAsync(string name, string startUri, bool useStrawfox = false, bool useStrawfoxWindow = false, byte[] favicon = null)
        {
            await InitializeAsync();
            if (!Uri.TryCreate(startUri, UriKind.Absolute, out Uri uri) ||
                (uri.Scheme != Uri.UriSchemeHttp && uri.Scheme != Uri.UriSchemeHttps))
                throw new ArgumentException("Informe uma URL http ou https válida.", nameof(startUri));

            string displayName = string.IsNullOrWhiteSpace(name) ? uri.Host : name.Trim();
            var app = new WebAppDefinition { Id = Guid.NewGuid().ToString("N"), Name = displayName, StartUri = uri.AbsoluteUri, UseStrawfox = useStrawfox || useStrawfoxWindow, UseStrawfoxWindow = useStrawfoxWindow };
            if (favicon != null)
            {
                StorageFolder folder = await ApplicationData.Current.LocalFolder.CreateFolderAsync("WebAppIcons", CreationCollisionOption.OpenIfExists);
                StorageFile file = await folder.CreateFileAsync(app.Id + ".favicon");
                await FileIO.WriteBytesAsync(file, favicon);
                app.IconUri = "ms-appdata:///local/WebAppIcons/" + file.Name;
            }
            InstalledApps.Add(app);
            Save();
            AppsChanged?.Invoke(this, EventArgs.Empty);
            if (favicon == null && (useStrawfox || useStrawfoxWindow))
                _ = SaveFaviconAsync(app, new Uri(uri, "/favicon.ico").AbsoluteUri);
            return app;
        }

        public void SetPinned(WebAppDefinition app, bool isPinned)
        {
            if (app == null) return;
            app.IsPinned = isPinned;
            Save();
            AppsChanged?.Invoke(this, EventArgs.Empty);
        }

        public void Uninstall(WebAppDefinition app)
        {
            if (app == null) return;
            InstalledApps.Remove(app);
            Save();
            AppsChanged?.Invoke(this, EventArgs.Empty);
        }

        public async Task ChangeIconAsync(WebAppDefinition app, StorageFile file)
        {
            if (!InstalledApps.Contains(app)) return;

            using (var stream = await file.OpenReadAsync())
            {
                var preview = new BitmapImage { DecodePixelWidth = 48 };
                await preview.SetSourceAsync(stream);
            }

            StorageFolder folder = await ApplicationData.Current.LocalFolder.CreateFolderAsync(
                "WebAppIcons", CreationCollisionOption.OpenIfExists);
            StorageFile icon = await file.CopyAsync(folder, app.Id + "-" + Guid.NewGuid().ToString("N") + file.FileType);
            if (!InstalledApps.Contains(app))
            {
                await icon.DeleteAsync();
                return;
            }

            string previousIcon = app.IconUri;
            app.IconUri = "ms-appdata:///local/WebAppIcons/" + Uri.EscapeDataString(icon.Name);
            Save();
            AppsChanged?.Invoke(this, EventArgs.Empty);

            if (previousIcon?.StartsWith("ms-appdata:///local/WebAppIcons/" + app.Id + "-", StringComparison.Ordinal) == true)
            {
                try
                {
                    var previousFile = await StorageFile.GetFileFromApplicationUriAsync(new Uri(previousIcon));
                    await previousFile.DeleteAsync();
                }
                catch { }
            }
        }

        public async Task SaveFaviconAsync(WebAppDefinition app, string faviconUri)
        {
            if (!InstalledApps.Contains(app) || !string.IsNullOrEmpty(app.IconUri) ||
                !Uri.TryCreate(faviconUri, UriKind.Absolute, out Uri uri) ||
                (uri.Scheme != Uri.UriSchemeHttp && uri.Scheme != Uri.UriSchemeHttps)) return;

            try
            {
                using (var client = new HttpClient())
                {
                    IBuffer buffer = await client.GetBufferAsync(uri);
                    StorageFolder iconFolder = await ApplicationData.Current.LocalFolder.CreateFolderAsync("WebAppIcons", CreationCollisionOption.OpenIfExists);
                    StorageFile iconFile = await iconFolder.CreateFileAsync(app.Id + ".favicon", CreationCollisionOption.ReplaceExisting);
                    await FileIO.WriteBufferAsync(iconFile, buffer);
                    if (!InstalledApps.Contains(app) || !string.IsNullOrEmpty(app.IconUri)) return;
                    app.IconUri = "ms-appdata:///local/WebAppIcons/" + app.Id + ".favicon";
                    Save();
                    AppsChanged?.Invoke(this, EventArgs.Empty);
                }
            }
            catch
            {
                // A failed favicon must not prevent the app itself from loading.
            }
        }

        private void Save()
        {
            var values = ApplicationData.Current.LocalSettings.Values;
            foreach (string key in values.Keys.Where(key => key.StartsWith(KeyPrefix)).ToList()) values.Remove(key);
            for (int index = 0; index < InstalledApps.Count; index++)
            {
                WebAppDefinition app = InstalledApps[index];
                values[KeyPrefix + index] = new ApplicationDataCompositeValue
                {
                    ["Id"] = app.Id,
                    ["Name"] = app.Name,
                    ["StartUri"] = app.StartUri,
                    ["UseStrawfox"] = app.UseStrawfox,
                    ["UseStrawfoxWindow"] = app.UseStrawfoxWindow,
                    ["IconUri"] = app.IconUri,
                    ["IsPinned"] = app.IsPinned
                };
            }
        }
    }
}
