using factoryos_10x_shell.Library.Models.InternalData;
using factoryos_10x_shell.Library.Services.WebApps;
using System;
using System.Collections.Generic;
using System.Collections.ObjectModel;
using System.Linq;
using System.Threading.Tasks;
using Windows.Storage;
using Windows.Storage.Streams;
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
            return Task.CompletedTask;
        }

        public async Task<WebAppDefinition> InstallAsync(string name, string startUri, bool useStrawfox = false)
        {
            await InitializeAsync();
            if (!Uri.TryCreate(startUri, UriKind.Absolute, out Uri uri) ||
                (uri.Scheme != Uri.UriSchemeHttp && uri.Scheme != Uri.UriSchemeHttps))
                throw new ArgumentException("Informe uma URL http ou https válida.", nameof(startUri));

            string displayName = string.IsNullOrWhiteSpace(name) ? uri.Host : name.Trim();
            var app = new WebAppDefinition { Id = Guid.NewGuid().ToString("N"), Name = displayName, StartUri = uri.AbsoluteUri, UseStrawfox = useStrawfox };
            InstalledApps.Add(app);
            Save();
            AppsChanged?.Invoke(this, EventArgs.Empty);
            if (useStrawfox)
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

        public async Task SaveFaviconAsync(WebAppDefinition app, string faviconUri)
        {
            if (app == null || !Uri.TryCreate(faviconUri, UriKind.Absolute, out Uri uri) ||
                (uri.Scheme != Uri.UriSchemeHttp && uri.Scheme != Uri.UriSchemeHttps)) return;

            try
            {
                using (var client = new HttpClient())
                {
                    IBuffer buffer = await client.GetBufferAsync(uri);
                    StorageFolder iconFolder = await ApplicationData.Current.LocalFolder.CreateFolderAsync("WebAppIcons", CreationCollisionOption.OpenIfExists);
                    StorageFile iconFile = await iconFolder.CreateFileAsync(app.Id + ".favicon", CreationCollisionOption.ReplaceExisting);
                    await FileIO.WriteBufferAsync(iconFile, buffer);
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
                    ["IconUri"] = app.IconUri,
                    ["IsPinned"] = app.IsPinned
                };
            }
        }
    }
}
