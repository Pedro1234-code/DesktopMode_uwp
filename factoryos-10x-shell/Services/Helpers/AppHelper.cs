using factoryos_10x_shell.Library.Events;
using factoryos_10x_shell.Library.Models.InternalData;
using factoryos_10x_shell.Library.Services.Helpers;
using System;
using System.Collections.Generic;
using System.Collections.ObjectModel;
using System.Diagnostics;
using System.IO;
using System.Linq;
using System.Runtime.InteropServices.WindowsRuntime;
using System.Text;
using System.Threading;
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
        private const string LivePackageTrackingSetting = "CoreShell.LivePackageTrackingEnabled";
        private DispatcherQueue m_dispatcherQueue;

        private Size _logoSize;
        public ObservableCollection<StartIconModel> StartIcons { get; set; } = new ObservableCollection<StartIconModel>();
        public ObservableCollection<StartIconModel> TaskbarIcons { get; set; } = new ObservableCollection<StartIconModel>();

        private List<StartIconModel> _iconCache { get; set; }

        public PackageManager PackageManager { get; set; }

        private Dictionary<string, string> m_pkgFamilyMap;
        private readonly Dictionary<string, string> m_appUriByAumid;
        private readonly SemaphoreSlim m_appsReloadGate = new SemaphoreSlim(1, 1);
        private readonly object m_packageRefreshLock = new object();
        private static readonly SemaphoreSlim s_startMenuLogGate = new SemaphoreSlim(1, 1);
        private PackageCatalog m_packageCatalog;
        private CancellationTokenSource m_packageRefreshDebounce;
        private CancellationTokenSource m_iconMaterializationCancellation;
        private readonly string m_startMenuLogPath;
        private long m_catalogEventSequence;
        private long m_refreshRequestSequence;

        public bool IsLivePackageTrackingEnabled
        {
            get
            {
                return !ApplicationData.Current.LocalSettings.Values.TryGetValue(
                    LivePackageTrackingSetting, out object value) ||
                    !(value is bool enabled) || enabled;
            }
            set
            {
                ApplicationData.Current.LocalSettings.Values[LivePackageTrackingSetting] = value;
                if (value)
                    WatchPackageCatalog();
                else
                    StopWatchingPackageCatalog();
            }
        }

        private sealed class DiscoveredApp
        {
            public AppListEntry Entry { get; set; }
            public string AppUri { get; set; }
            public string AppUserModelId { get; set; }
            public string DisplayName { get; set; }
            public byte[] IconBytes { get; set; }
        }

        private sealed class DiscoveredPackage
        {
            public Package Package { get; set; }
            public string FamilyName { get; set; }
            public string FullName { get; set; }
            public List<DiscoveredApp> Apps { get; } = new List<DiscoveredApp>();
        }

        private sealed class PackageDiscoveryResult
        {
            public int PackageCount { get; set; }
            public int CandidatePackageCount { get; set; }
            public int AppEntryCount { get; set; }
            public int FailedPackageCount { get; set; }
            public List<DiscoveredPackage> Packages { get; } = new List<DiscoveredPackage>();
        }

        public AppHelper() 
        {
            m_dispatcherQueue = DispatcherQueue.GetForCurrentThread();
            PackageManager = new PackageManager();
            m_pkgFamilyMap = new Dictionary<string, string>(StringComparer.OrdinalIgnoreCase);
            m_appUriByAumid = new Dictionary<string, string>(StringComparer.OrdinalIgnoreCase);

            // Start renders these at 48x48. Asking every package for 176x176
            // multiplied I/O and decode work for no visible benefit.
            _logoSize = new Size(48, 48);
            _iconCache = new List<StartIconModel>();
            m_startMenuLogPath = Path.Combine(ApplicationData.Current.LocalFolder.Path, "StartMenu-app-refresh.log");
            if (IsLivePackageTrackingEnabled)
            {
                LogStartMenuRefresh("AppHelper created; package catalog monitoring is starting.");
                WatchPackageCatalog();
            }
            else
            {
                LogStartMenuRefresh("AppHelper created; real-time package catalog monitoring is disabled.");
            }
        }

        private void WatchPackageCatalog()
        {
            if (m_packageCatalog != null)
                return;

            try
            {
                m_packageCatalog = PackageCatalog.OpenForCurrentUser();
                m_packageCatalog.PackageInstalling += PackageCatalog_PackageInstalling;
                m_packageCatalog.PackageUpdating += PackageCatalog_PackageUpdating;
                m_packageCatalog.PackageUninstalling += PackageCatalog_PackageUninstalling;
                // PackageStatusChanged is noisy (license, servicing and other
                // non-visual status transitions) and used to trigger a full
                // package/logo enumeration on the UI thread. The completed
                // install/update/uninstall events above cover every change the
                // Start menu needs to display.
                LogStartMenuRefresh("PackageCatalog opened. Listening only to install/update/uninstall events.");
            }
            catch (Exception ex)
            {
                // Package enumeration still works even on editions where the
                // live catalog is unavailable.
                Debug.WriteLine("PackageCatalog => Open: " + ex.Message);
                LogStartMenuRefresh($"PackageCatalog open failed: {ex.GetType().Name}: {ex.Message}");
            }
        }

        private void StopWatchingPackageCatalog()
        {
            PackageCatalog catalog = m_packageCatalog;
            if (catalog == null)
                return;

            catalog.PackageInstalling -= PackageCatalog_PackageInstalling;
            catalog.PackageUpdating -= PackageCatalog_PackageUpdating;
            catalog.PackageUninstalling -= PackageCatalog_PackageUninstalling;
            m_packageCatalog = null;

            lock (m_packageRefreshLock)
            {
                m_packageRefreshDebounce?.Cancel();
                m_packageRefreshDebounce?.Dispose();
                m_packageRefreshDebounce = null;
            }
            LogStartMenuRefresh("Real-time package catalog monitoring stopped.");
        }

        private void PackageCatalog_PackageInstalling(PackageCatalog sender, PackageInstallingEventArgs args) =>
            OnPackageCatalogEvent("installing", args.IsComplete);

        private void PackageCatalog_PackageUpdating(PackageCatalog sender, PackageUpdatingEventArgs args) =>
            OnPackageCatalogEvent("updating", args.IsComplete);

        private void PackageCatalog_PackageUninstalling(PackageCatalog sender, PackageUninstallingEventArgs args) =>
            OnPackageCatalogEvent("uninstalling", args.IsComplete);

        private void OnPackageCatalogEvent(string eventName, bool isComplete)
        {
            long eventNumber = Interlocked.Increment(ref m_catalogEventSequence);

            // Incomplete deployment progress can be extremely noisy. Sampling it
            // keeps the diagnostic itself from becoming a source of UI stalls.
            if (isComplete || eventNumber <= 10 || eventNumber % 50 == 0)
                LogStartMenuRefresh($"Catalog event #{eventNumber}: {eventName}, complete={isComplete}.");

            if (isComplete)
                SchedulePackageRefresh(eventName + " complete");
        }

        private void SchedulePackageRefresh(string reason)
        {
            CancellationTokenSource debounce = new CancellationTokenSource();
            long requestNumber = Interlocked.Increment(ref m_refreshRequestSequence);
            bool replacedPendingRefresh;
            lock (m_packageRefreshLock)
            {
                replacedPendingRefresh = m_packageRefreshDebounce != null &&
                    !m_packageRefreshDebounce.IsCancellationRequested;
                m_packageRefreshDebounce?.Cancel();
                m_packageRefreshDebounce?.Dispose();
                m_packageRefreshDebounce = debounce;
            }
            LogStartMenuRefresh($"Refresh request #{requestNumber}: reason={reason}; replacedPending={replacedPendingRefresh}.");
            _ = RefreshPackagesAfterDelayAsync(debounce, requestNumber, reason);
        }

        private async Task RefreshPackagesAfterDelayAsync(
            CancellationTokenSource debounce,
            long requestNumber,
            string reason)
        {
            try
            {
                // Package deployment reports several closely-spaced state
                // changes. Wait for the catalog to settle before enumerating.
                await Task.Delay(1000, debounce.Token);
                if (debounce.IsCancellationRequested) return;

                bool enqueued = m_dispatcherQueue.TryEnqueue(async () =>
                {
                    if (!debounce.IsCancellationRequested)
                        await LoadAppsCoreAsync($"catalog request #{requestNumber} ({reason})");
                });
                LogStartMenuRefresh($"Refresh request #{requestNumber}: debounce elapsed; UI enqueue={enqueued}.");
            }
            catch (TaskCanceledException)
            {
                LogStartMenuRefresh($"Refresh request #{requestNumber}: canceled by a newer catalog event.");
            }
        }

        private void LogStartMenuRefresh(string message)
        {
            string line = $"{DateTimeOffset.Now:O} [thread={System.Environment.CurrentManagedThreadId}; ui={m_dispatcherQueue?.HasThreadAccess == true}] {message}";
            Debug.WriteLine("[StartMenuApps] " + line);

            // File I/O is serialized on worker threads. Diagnostics must never add
            // synchronous storage work to the UI path being measured.
            _ = Task.Run(async () =>
            {
                await s_startMenuLogGate.WaitAsync().ConfigureAwait(false);
                try
                {
                    var file = new FileInfo(m_startMenuLogPath);
                    if (file.Exists && file.Length > 2 * 1024 * 1024)
                        File.WriteAllText(m_startMenuLogPath, "Log rotated at " + DateTimeOffset.Now.ToString("O") + System.Environment.NewLine);
                    File.AppendAllText(m_startMenuLogPath, line + System.Environment.NewLine, Encoding.UTF8);
                }
                catch
                {
                    // Diagnostics are optional and must not affect the Shell.
                }
                finally
                {
                    s_startMenuLogGate.Release();
                }
            });
        }

        private static string GetPackageLogName(Package package)
        {
            try
            {
                return package?.Id?.Name ?? "<unknown>";
            }
            catch
            {
                // A package can disappear while an uninstall-triggered refresh is
                // enumerating it. Diagnostics must tolerate that race as well.
                return "<unavailable>";
            }
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
            bitmap.DecodePixelWidth = 48;
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
                            ? (string.Equals(appId, "CoreShell.Notepad", StringComparison.OrdinalIgnoreCase) ? "Notepad" : string.Equals(appId, "CoreShell.Settings", StringComparison.OrdinalIgnoreCase) ? "Settings" : string.Equals(appId, "CoreShell.Calculator", StringComparison.OrdinalIgnoreCase) ? "Calculator" : string.Equals(appId, "CoreShell.Firefox", StringComparison.OrdinalIgnoreCase) ? "Strawfox" : "Files")
                            : iconName
                    });
                    continue;
                }

                // Do not enumerate the complete package catalog once per pin.
                // The single background Start snapshot hydrates these models later.
                loaded.Add(new StartIconModel
                {
                    AppId = appId,
                    Aumid = aumid,
                    IconName = iconName,
                    AppUri = appUri
                });
            }

            var dispatcher = Windows.ApplicationModel.Core.CoreApplication.MainView.CoreWindow.Dispatcher;

            await dispatcher.RunAsync(Windows.UI.Core.CoreDispatcherPriority.Normal, () =>
            {
                TaskbarIcons.Clear();
                foreach (var item in loaded)
                {
                    // Discovery can finish before the taskbar is loaded. Reuse
                    // its model so an ongoing icon decode also updates the pin.
                    var discovered = FindDiscoveredApp(item);
                    if (discovered != null)
                    {
                        if (string.IsNullOrEmpty(discovered.AppUri))
                            discovered.AppUri = item.AppUri;
                        TaskbarIcons.Add(discovered);
                    }
                    else
                    {
                        TaskbarIcons.Add(item);
                    }
                }
            });

            Debug.WriteLine($"[INFO] Loaded pinned apps count: {loaded.Count}");
        }



        public Task LoadAppsAsync() => LoadAppsCoreAsync("startup/direct call");

        private Task<PackageDiscoveryResult> DiscoverPackagesAsync()
        {
            // PackageManager.FindPackagesForUser and Package.GetAppListEntries
            // are synchronous WinRT metadata calls. On Xbox they can take more
            // than half a second for a single package, so they must never run on
            // the XAML dispatcher.
            return Task.Run(async () =>
            {
                var result = new PackageDiscoveryResult();
                var packageManager = new PackageManager();
                IEnumerable<Package> packages = packageManager.FindPackagesForUser("");

                foreach (Package package in packages)
                {
                    result.PackageCount++;
                    if (package.IsFramework || package.IsResourcePackage || package.IsStub)
                        continue;

                    Stopwatch packageClock = Stopwatch.StartNew();
                    try
                    {
                        IReadOnlyList<AppListEntry> entries = package.GetAppListEntries();
                        if (entries == null || entries.Count == 0)
                            continue;

                        var discoveredPackage = new DiscoveredPackage
                        {
                            Package = package,
                            FamilyName = package.Id.FamilyName,
                            FullName = package.Id.FullName
                        };
                        result.CandidatePackageCount++;

                        foreach (AppListEntry entry in entries)
                        {
                            result.AppEntryCount++;
                            AppDisplayInfo displayInfo = entry.DisplayInfo;
                            discoveredPackage.Apps.Add(new DiscoveredApp
                            {
                                Entry = entry,
                                AppUserModelId = entry.AppUserModelId,
                                DisplayName = displayInfo.DisplayName,
                                IconBytes = await ReadLogoBytesAsync(displayInfo).ConfigureAwait(false),
                                AppUri = await GetPackageProtocolUriAsync(
                                    package, entry.AppUserModelId).ConfigureAwait(false)
                            });
                        }
                        result.Packages.Add(discoveredPackage);
                    }
                    catch (Exception ex)
                    {
                        result.FailedPackageCount++;
                        string packageName = GetPackageLogName(package);
                        Debug.WriteLine($"Error enumerating package {packageName}: {ex.Message}");
                        LogStartMenuRefresh($"Package failed: {packageName}; elapsed={packageClock.ElapsedMilliseconds} ms; error={ex.GetType().Name}: {ex.Message}");
                    }
                    finally
                    {
                        packageClock.Stop();
                        if (packageClock.ElapsedMilliseconds >= 100)
                            LogStartMenuRefresh($"Slow package: {GetPackageLogName(package)}; elapsed={packageClock.ElapsedMilliseconds} ms.");
                    }
                }

                return result;
            });
        }

        private async Task<byte[]> ReadLogoBytesAsync(AppDisplayInfo displayInfo)
        {
            try
            {
                var logoReference = displayInfo.GetLogo(_logoSize);
                using (var randomAccessStream = await logoReference.OpenReadAsync())
                using (Stream source = randomAccessStream.AsStreamForRead())
                using (var destination = new MemoryStream())
                {
                    await source.CopyToAsync(destination).ConfigureAwait(false);
                    return destination.ToArray();
                }
            }
            catch (Exception ex)
            {
                // A missing optional logo must not remove the application from Start.
                LogStartMenuRefresh($"App logo read failed: {ex.GetType().Name}: {ex.Message}");
                return null;
            }
        }

        private static string NormalizeAumid(string appUserModelId)
        {
            if (string.IsNullOrWhiteSpace(appUserModelId)) return null;
            string[] parts = appUserModelId.Split('!');
            return parts.Length == 2 ? parts[0] + "!" + parts[1] : null;
        }

        private StartIconModel FindDiscoveredApp(StartIconModel pinned)
        {
            if (pinned.AppId != null &&
                pinned.AppId.StartsWith("CoreShell.", StringComparison.OrdinalIgnoreCase))
                return null;

            return _iconCache.FirstOrDefault(candidate =>
                (!string.IsNullOrEmpty(pinned.Aumid) &&
                 string.Equals(candidate.Aumid, pinned.Aumid, StringComparison.OrdinalIgnoreCase)) ||
                (!string.IsNullOrEmpty(pinned.AppId) &&
                 string.Equals(candidate.AppId, pinned.AppId, StringComparison.OrdinalIgnoreCase)));
        }

        private void ScheduleIconMaterialization(IReadOnlyList<StartIconModel> models)
        {
            m_iconMaterializationCancellation?.Cancel();
            m_iconMaterializationCancellation?.Dispose();
            m_iconMaterializationCancellation = new CancellationTokenSource();
            _ = MaterializeIconsGraduallyAsync(models, m_iconMaterializationCancellation.Token);
        }

        private async Task MaterializeIconsGraduallyAsync(
            IReadOnlyList<StartIconModel> models,
            CancellationToken cancellationToken)
        {
            try
            {
                // Let the desktop, taskbar and first pointer frames settle first.
                await Task.Delay(750, cancellationToken);
                foreach (StartIconModel model in models)
                {
                    cancellationToken.ThrowIfCancellationRequested();
                    if (model.IconBytes == null || model.IconBytes.Length == 0) continue;

                    // One small decode per dispatcher turn prevents a package refresh
                    // from monopolizing XAML for several seconds.
                    await Task.Delay(32, cancellationToken);
                    try
                    {
                        var bitmap = new BitmapImage { DecodePixelWidth = 48 };
                        using (var source = new MemoryStream(model.IconBytes).AsRandomAccessStream())
                            await bitmap.SetSourceAsync(source);
                        cancellationToken.ThrowIfCancellationRequested();
                        model.IconSource = bitmap;
                    }
                    catch (OperationCanceledException)
                    {
                        throw;
                    }
                    catch (Exception ex)
                    {
                        LogStartMenuRefresh($"App icon decode failed: {ex.GetType().Name}: {ex.Message}");
                    }
                }
            }
            catch (OperationCanceledException)
            {
                // A newer package snapshot superseded this materialization pass.
            }
            catch (Exception ex)
            {
                LogStartMenuRefresh($"Incremental icon materialization failed: {ex.GetType().Name}: {ex.Message}");
            }
        }

        private async Task LoadAppsCoreAsync(string reason)
        {
            Stopwatch totalClock = Stopwatch.StartNew();
            Stopwatch gateClock = Stopwatch.StartNew();
            LogStartMenuRefresh($"Reload requested: reason={reason}; currentStartIcons={StartIcons.Count}.");
            await m_appsReloadGate.WaitAsync();
            gateClock.Stop();
            LogStartMenuRefresh($"Reload gate acquired after {gateClock.ElapsedMilliseconds} ms: reason={reason}.");

            try
            {
                Stopwatch metadataClock = Stopwatch.StartNew();
                PackageDiscoveryResult discovery = await DiscoverPackagesAsync();
                metadataClock.Stop();
                LogStartMenuRefresh(
                    $"Background package discovery finished: packages={discovery.PackageCount}; " +
                    $"candidates={discovery.CandidatePackageCount}; entries={discovery.AppEntryCount}; " +
                    $"failed={discovery.FailedPackageCount}; elapsed={metadataClock.ElapsedMilliseconds} ms.");

                Stopwatch materializationClock = Stopwatch.StartNew();
                _iconCache.Clear(); // Clear the cache before reloading
                m_appUriByAumid.Clear();
                m_pkgFamilyMap.Clear();

                foreach (DiscoveredPackage discoveredPackage in discovery.Packages)
                {
                    m_pkgFamilyMap[discoveredPackage.FamilyName] = discoveredPackage.FullName;
                    try
                    {
                        foreach (DiscoveredApp app in discoveredPackage.Apps)
                        {
                            if (!string.IsNullOrEmpty(app.AppUri))
                                m_appUriByAumid[app.AppUserModelId] = app.AppUri;

                            _iconCache.Add(new StartIconModel
                            {
                                AppId = app.AppUserModelId,
                                Aumid = NormalizeAumid(app.AppUserModelId),
                                IconName = app.DisplayName,
                                IconBytes = app.IconBytes,
                                AppUri = app.AppUri,
                                Data = app.Entry
                            });
                        }
                    }
                    catch (Exception ex)
                    {
                        discovery.FailedPackageCount++;
                        string packageName = GetPackageLogName(discoveredPackage.Package);
                        Debug.WriteLine($"Error accessing logo for package {packageName}: {ex.Message}");
                        LogStartMenuRefresh($"Package logo failed: {packageName}; error={ex.GetType().Name}: {ex.Message}");
                    }
                }
                materializationClock.Stop();
                MainPage.RecordUiOperation(
                    "StartMenu.BuildPackageModels", materializationClock.ElapsedMilliseconds);

                long discoveryMilliseconds = totalClock.ElapsedMilliseconds;
                LogStartMenuRefresh(
                    $"Discovery finished: packages={discovery.PackageCount}; candidates={discovery.CandidatePackageCount}; " +
                    $"entries={discovery.AppEntryCount}; failed={discovery.FailedPackageCount}; models={_iconCache.Count}; " +
                    $"elapsed={discoveryMilliseconds} ms.");

                // Keep the collection instance stable because StartMenu binds to
                // it before deferred app discovery completes.  The CoreShell
                // entries are injected by StartMenuViewModel and must survive
                // this refresh; clearing them was why the internal apps
                // disappeared after startup became asynchronous.
                List<StartIconModel> internalIcons = StartIcons
                    .Where(icon => icon.AppId != null &&
                                   icon.AppId.StartsWith("CoreShell.", StringComparison.OrdinalIgnoreCase))
                    .ToList();
                Stopwatch collectionClock = Stopwatch.StartNew();
                StartIcons.Clear();
                foreach (StartIconModel icon in internalIcons.OrderBy(icon => icon.AppId))
                    StartIcons.Add(icon);
                foreach (StartIconModel icon in _iconCache.OrderBy(icon => icon.IconName))
                    StartIcons.Add(icon);
                var iconsToMaterialize = new List<StartIconModel>();
                foreach (StartIconModel pinned in TaskbarIcons)
                {
                    if (pinned.AppId != null &&
                        pinned.AppId.StartsWith("CoreShell.", StringComparison.OrdinalIgnoreCase))
                        continue;

                    StartIconModel discovered = FindDiscoveredApp(pinned);
                    if (discovered == null) continue;

                    pinned.Data = discovered.Data;
                    pinned.AppUri = discovered.AppUri ?? pinned.AppUri;
                    pinned.IconBytes = discovered.IconBytes;
                    if (string.IsNullOrWhiteSpace(pinned.IconName))
                        pinned.IconName = discovered.IconName;
                    iconsToMaterialize.Add(pinned);
                }
                iconsToMaterialize.AddRange(_iconCache);
                ScheduleIconMaterialization(iconsToMaterialize.Distinct().ToList());
                collectionClock.Stop();
                LogStartMenuRefresh(
                    $"Observable collection rebuilt: internal={internalIcons.Count}; installed={_iconCache.Count}; " +
                    $"elapsed={collectionClock.ElapsedMilliseconds} ms.");
            }
            catch (Exception ex)
            {
                Debug.WriteLine("LoadApps => Get: " + ex.Message);
                LogStartMenuRefresh($"Reload failed: {ex.GetType().Name}: {ex.Message}");
            }
            finally
            {
                m_appsReloadGate.Release();
                totalClock.Stop();
                LogStartMenuRefresh(
                    $"Reload finished: reason={reason}; total={totalClock.ElapsedMilliseconds} ms; " +
                    $"uiThread={m_dispatcherQueue?.HasThreadAccess == true}; finalStartIcons={StartIcons.Count}.");
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
