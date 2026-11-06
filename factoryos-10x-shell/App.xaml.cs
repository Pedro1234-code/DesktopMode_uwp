using factoryos_10x_shell.Library.Constants;
using factoryos_10x_shell.Library.Models.Hardware;
using factoryos_10x_shell.Library.Services.Environment;
using factoryos_10x_shell.Library.Services.Hardware;
using factoryos_10x_shell.Library.Services.Helpers;
using factoryos_10x_shell.Library.Services.Managers;
using factoryos_10x_shell.Library.Services.WebApps;
using Microsoft.Extensions.DependencyInjection;
using System;
using System.Collections.Generic;
using System.Collections.ObjectModel;
using System.Diagnostics;
using System.IO;
using System.Linq;
using System.Reflection;
using System.Runtime.InteropServices.WindowsRuntime;
using System.ServiceModel.Channels;
using System.Threading.Tasks;
using System.Text.RegularExpressions;
using System.Threading;
using Windows.ApplicationModel;
using Windows.ApplicationModel.AppService;
using Windows.Foundation.Collections;
using Windows.ApplicationModel.Activation;
using Windows.ApplicationModel.Core;
using Windows.ApplicationModel.ExtendedExecution;
using Windows.Foundation;
using Windows.Graphics.Imaging;
using Windows.Management.Deployment;
using Windows.Media.Playback;
using Windows.Storage.Streams;
using Windows.UI.ViewManagement;
using Windows.UI.Xaml;
using Windows.UI.Xaml.Controls;
using factoryos_10x_shell.Services.Helpers;
using Windows.UI.Xaml.Controls.Primitives;
using Windows.UI.Xaml.Data;
using Windows.UI.Xaml.Input;
using Windows.UI.Xaml.Media;
using Windows.UI.Xaml.Media.Imaging;
using Windows.ApplicationModel.Background;
using Windows.Storage;
using Windows.UI.Xaml.Navigation;
using Windows.UI;
using Win32Bridge;

namespace factoryos_10x_shell
{
    sealed partial class App : Application
    {
        public static MediaPlayer MediaPlayer;
        private bool m_protocolInstallOpen;


        [Obsolete]
        public App()
        {
            // The embedded Gecko package contains native helper images whose
            // base names (firefox, xpcshell, certutil, etc.) can be presented
            // to CoreCLR's assembly resolver while it scans the AppX root.
            // They are not managed dependencies and must never be loaded as
            // assemblies. Returning null here lets the native loader handle
            // them while avoiding repeated managed load attempts.
            AppDomain.CurrentDomain.AssemblyResolve += IgnoreNativeGeckoAssemblyProbe;
            this.InitializeComponent();
            this.UnhandledException += OnUnhandledException;
            this.Suspending += OnSuspending;
            this.Resuming += OnResuming;
            ApplicationView.PreferredLaunchWindowingMode = ApplicationViewWindowingMode.FullScreen;
            if (Windows.Storage.ApplicationData.Current.LocalSettings.Values.ContainsKey("IsBgChangeButtonVisible"))
            {
                AppState.Instance.IsBgChangeButtonVisible = (bool)Windows.Storage.ApplicationData.Current.LocalSettings.Values["IsBgChangeButtonVisible"];
            }

            MediaPlayer = BackgroundMediaPlayer.Current;

        }

        private static Assembly IgnoreNativeGeckoAssemblyProbe(object sender, ResolveEventArgs args)
        {
            string simpleName = args?.Name;
            if (!string.IsNullOrEmpty(simpleName))
            {
                int comma = simpleName.IndexOf(',');
                if (comma >= 0)
                    simpleName = simpleName.Substring(0, comma);

                switch (simpleName.Trim().ToLowerInvariant())
                {
                    case "firefox":
                    case "certutil":
                    case "default-browser-agent":
                    case "nmhproxy":
                    case "pingsender":
                    case "pk12util":
                    case "plugin-container":
                    case "xpcshell":
                        return null;
                }
            }

            return null;
        }

        protected async override void OnLaunched(LaunchActivatedEventArgs e)
        {
            ConfigureServices();
            PreloadServices();

            Frame rootFrame = Window.Current.Content as Frame;

            if (rootFrame == null)
            {
                rootFrame = new Frame();

                rootFrame.NavigationFailed += OnNavigationFailed;

                if (e.PreviousExecutionState == ApplicationExecutionState.Terminated)
                {

                }
                Window.Current.Content = rootFrame;
            }

            // The desktop must become visible without waiting for optional
            // discovery, storage preparation, or network-related services.
            // Those operations can be slow on Xbox and are not required to
            // render or interact with the Shell itself.
            if (e.PrelaunchActivated == false)
            {
                if (rootFrame.Content == null)
                {
                    rootFrame.Navigate(typeof(MainPage), e.Arguments);
                }
                Window.Current.Activate();
            }

            ApplicationView.PreferredLaunchWindowingMode = ApplicationViewWindowingMode.Maximized;
            _ = InitializeStartupServicesAsync();
        }

        private async Task InitializeStartupServicesAsync()
        {
            _ = InitializeStartAppsAsync();
            try
            {
                // Built-in web apps are shell state and do not depend on the
                // optional Win32 drive. Initialize them first so a runtime
                // preparation failure cannot hide Xbox Device Portal.
                await ServiceProvider.GetRequiredService<IWebAppService>().InitializeAsync();
                try
                {
                    await new RuntimeHost().EnsureDriveRootAsync();
                }
                catch (Exception runtimeError)
                {
                    Debug.WriteLine($"Win32 drive preparation failed: {runtimeError}");
                }
            }
            catch (Exception exception)
            {
                // App discovery is supplemental. A package, icon, or runtime
                // that cannot be inspected must not delay or terminate Shell
                // startup.
                Debug.WriteLine($"Deferred startup initialization failed: {exception}");
            }
        }

        private async Task InitializeStartAppsAsync()
        {
            try
            {
                await ServiceProvider.GetRequiredService<IAppHelper>().LoadAppsAsync();
            }
            catch (Exception error)
            {
                Debug.WriteLine($"Start Menu package discovery failed: {error}");
            }
        }

        private void OnUnhandledException(object sender, Windows.UI.Xaml.UnhandledExceptionEventArgs e)
        {
            // Keep this handler synchronous. Exceptions crossing a native/XAML boundary
            // can reach it without a Window.Current for the calling thread.
            e.Handled = true;
            // This is the Shell-wide handler. The Calculator diagnostic marker may
            // contain an old value even when the failing control is Files, Firefox,
            // or another internal app, so it must not label the reported failure.
            const string diagnosticStage = "Shell XAML operation";
            Debug.WriteLine($"Unhandled XAML exception during '{diagnosticStage}': {e.Exception}");

            Exception exceptionWithContext = new InvalidOperationException(
                $"Unhandled exception during: {diagnosticStage}. " +
                $"Original HRESULT: 0x{e.Exception.HResult:X8}.",
                e.Exception);

            try
            {
                Window currentWindow = Window.Current;
                if (currentWindow == null)
                {
                    Debug.WriteLine("FallbackErrorPage was not shown because this thread has no Window.Current.");
                    return;
                }

                if (currentWindow.Content is Frame rootFrame &&
                    !(rootFrame.Content is Views.FallbackErrorPage))
                {
                    rootFrame.Navigate(typeof(Views.FallbackErrorPage), exceptionWithContext);
                }
            }
            catch (Exception fallbackException)
            {
                // Reporting an error must not create another unhandled exception.
                Debug.WriteLine($"Could not display FallbackErrorPage: {fallbackException}");
            }
        }



        protected override async void OnActivated(IActivatedEventArgs args)
        {
            if (args.Kind == ActivationKind.File)
            {
                if (!HasServiceProvider)
                {
                    ConfigureServices();
                    PreloadServices();
                }

                Frame rootFrame = Window.Current.Content as Frame;
                if (rootFrame == null)
                {
                    rootFrame = new Frame();
                    rootFrame.NavigationFailed += OnNavigationFailed;
                    Window.Current.Content = rootFrame;
                }
                if (rootFrame.Content == null)
                    rootFrame.Navigate(typeof(MainPage));

                FileActivatedEventArgs fileArgs = (FileActivatedEventArgs)args;
                StorageFile pdf = fileArgs.Files.OfType<StorageFile>()
                    .FirstOrDefault(file => string.Equals(file.FileType, ".pdf", StringComparison.OrdinalIgnoreCase));
                if (pdf != null)
                    AppState.Instance.RequestFirefoxOpen(pdf);

                Window.Current.Activate();
                return;
            }

            if (args.Kind == ActivationKind.Protocol)
            {
                var protocolArgs = args as ProtocolActivatedEventArgs;
                if (string.Equals(protocolArgs?.Uri?.Scheme, "cros-apps", StringComparison.OrdinalIgnoreCase))
                {
                    try { await HandleWebAppProtocolAsync(protocolArgs.Uri); }
                    catch (Exception exception)
                    {
                        Debug.WriteLine($"Web App protocol activation failed: {exception.GetType().Name} (0x{exception.HResult:X8})");
                    }
                    return;
                }

                // Check if the app is already running
                if (Window.Current.Content == null)
                {
                    // The app is not running, close the app and launch again

                    var packageFamilyName = "MobileOSDev.CoreShell_d7x680j9yw8bm";
                    var pm = new Windows.Management.Deployment.PackageManager();
                    var packages = pm.FindPackagesForUser(string.Empty, packageFamilyName);

                    var foundPackage = packages.FirstOrDefault();
                    if (foundPackage != null)
                    {
                        var appListEntries = await foundPackage.GetAppListEntriesAsync();
                        var entry = appListEntries.FirstOrDefault();
                        if (entry != null)
                        {
                            bool success = await entry.LaunchAsync();
                        }
                    }


                }
                else
                {
                    // The app is running, bring it to the foreground
                    Window.Current.Activate();
                }

                Window.Current.Activate();
            }
        }


        private async Task HandleWebAppProtocolAsync(Uri request)
        {
            if (m_protocolInstallOpen ||
                !string.Equals(request.Host, "install-app", StringComparison.OrdinalIgnoreCase) ||
                (request.AbsolutePath != "" && request.AbsolutePath != "/") ||
                string.IsNullOrEmpty(request.Query)) return;

            var packageIds = new WwwFormUrlDecoder(request.Query)
                .Where(entry => entry.Name == "package_id").Select(entry => entry.Value).ToList();
            if (packageIds.Count != 1 || !packageIds[0].StartsWith("web:", StringComparison.Ordinal) ||
                !Uri.TryCreate(packageIds[0].Substring(4), UriKind.Absolute, out Uri site) ||
                (site.Scheme != Uri.UriSchemeHttps && site.Scheme != Uri.UriSchemeHttp) ||
                string.IsNullOrEmpty(site.Host) || !string.IsNullOrEmpty(site.UserInfo)) return;

            m_protocolInstallOpen = true;
            try
            {
                if (!HasServiceProvider)
                {
                    ConfigureServices();
                    PreloadServices();
                }
                var frame = Window.Current.Content as Frame;
                if (frame == null)
                {
                    frame = new Frame();
                    frame.NavigationFailed += OnNavigationFailed;
                    Window.Current.Content = frame;
                }
                if (frame.Content == null)
                {
                    frame.Navigate(typeof(MainPage));
                    _ = InitializeStartupServicesAsync();
                }
                Window.Current.Activate();

                string name = site.IdnHost;
                if (name.StartsWith("www.", StringComparison.OrdinalIgnoreCase))
                    name = name.Substring(4);
                var metadata = await LoadWebAppMetadataAsync(site, name);
                name = metadata.Name;
                var icon = new Grid { Width = 48, Height = 48 };
                if (metadata.Icon != null)
                    icon.Children.Add(new Image { Source = metadata.Icon, Stretch = Stretch.Uniform });
                else
                    icon.Children.Add(new FontIcon { Glyph = "\uE774" });

                var panel = new StackPanel { Spacing = 8 };
                panel.Children.Add(icon);
                panel.Children.Add(new TextBlock { Text = name, FontSize = 20, TextWrapping = TextWrapping.Wrap });
                panel.Children.Add(new TextBlock { Text = site.GetLeftPart(UriPartial.Authority), TextWrapping = TextWrapping.Wrap });
                var dialog = new ContentDialog
                {
                    Title = "Install Web App",
                    Content = panel,
                    PrimaryButtonText = "Install app",
                    CloseButtonText = "Cancel"
                };

                MainPage.SetPopupCursorVisibility(true);
                try
                {
                    if (await dialog.ShowAsync() == ContentDialogResult.Primary)
                    {
                        var app = await ServiceProvider.GetRequiredService<IWebAppService>()
                            .InstallAsync(name, site.AbsoluteUri, useStrawfox: true, useStrawfoxWindow: true,
                                favicon: metadata.IconBytes);
                        ServiceProvider.GetRequiredService<IWindowManagerService>().Open(app);
                    }
                }
                finally { MainPage.SetPopupCursorVisibility(false); }
            }
            finally { m_protocolInstallOpen = false; }
        }

        private static async Task<(string Name, BitmapImage Icon, byte[] IconBytes)> LoadWebAppMetadataAsync(Uri site, string name)
        {
            var candidates = new List<Uri>();
            using (var client = new System.Net.Http.HttpClient { Timeout = TimeSpan.FromSeconds(4) })
            {
                try
                {
                    using (var response = await client.GetAsync(site, System.Net.Http.HttpCompletionOption.ResponseHeadersRead))
                    using (var stream = await response.EnsureSuccessStatusCode().Content.ReadAsStreamAsync())
                    using (var timeout = new CancellationTokenSource(TimeSpan.FromSeconds(4)))
                    {
                        var buffer = new byte[65536];
                        int length = 0;
                        while (length < buffer.Length)
                        {
                            int count = await stream.ReadAsync(buffer, length, buffer.Length - length, timeout.Token);
                            if (count == 0) break;
                            length += count;
                        }
                        string html = System.Text.Encoding.UTF8.GetString(buffer, 0, length);
                        var title = Regex.Match(html, @"<title\b[^>]*>(.*?)</title\s*>", RegexOptions.IgnoreCase | RegexOptions.Singleline);
                        string decodedTitle = Regex.Replace(System.Net.WebUtility.HtmlDecode(title.Groups[1].Value), @"\s+", " ").Trim();
                        if (decodedTitle.Length > 0)
                            name = decodedTitle.Substring(0, Math.Min(120, decodedTitle.Length));

                        foreach (Match link in Regex.Matches(html, @"<link\b[^>]*>", RegexOptions.IgnoreCase))
                        {
                            string rel = ReadHtmlAttribute(link.Value, "rel");
                            if (!Regex.IsMatch(rel, @"(^|\s)icon(\s|$)|^apple-touch-icon$", RegexOptions.IgnoreCase)) continue;
                            if (Uri.TryCreate(response.RequestMessage.RequestUri, ReadHtmlAttribute(link.Value, "href"), out Uri iconUri) &&
                                (iconUri.Scheme == Uri.UriSchemeHttps || iconUri.Scheme == Uri.UriSchemeHttp))
                                candidates.Add(iconUri);
                        }
                    }
                }
                catch (Exception exception) when (exception is System.Net.Http.HttpRequestException ||
                    exception is OperationCanceledException || exception is IOException) { }

                candidates = candidates.Distinct().Take(3).ToList();
                candidates.Add(new Uri(site, "/favicon.ico"));
                candidates.Add(new Uri("https://www.google.com/s2/favicons?sz=64&domain=" + Uri.EscapeDataString(site.IdnHost)));
                foreach (Uri uri in candidates.Distinct())
                {
                    try
                    {
                        byte[] bytes = await client.GetByteArrayAsync(uri);
                        using (var stream = new MemoryStream(bytes))
                        {
                            var bitmap = new BitmapImage { DecodePixelWidth = 48 };
                            await bitmap.SetSourceAsync(stream.AsRandomAccessStream());
                            return (name, bitmap, bytes);
                        }
                    }
                    catch (Exception exception)
                    {
                        Debug.WriteLine($"Web App icon could not be loaded (0x{exception.HResult:X8}).");
                    }
                }
            }
            return (name, null, null);
        }

        private static string ReadHtmlAttribute(string tag, string attribute)
        {
            var match = Regex.Match(tag, @"\s" + attribute + @"\s*=\s*(?:""([^""]*)""|'([^']*)'|([^\s>]+))",
                RegexOptions.IgnoreCase);
            return System.Net.WebUtility.HtmlDecode(match.Groups.Cast<Group>().Skip(1)
                .FirstOrDefault(group => group.Success)?.Value ?? string.Empty);
        }

        void OnNavigationFailed(object sender, NavigationFailedEventArgs e)
        {
            throw new Exception("Failed to load Page " + e.SourcePageType.FullName);
        }

        private async void OnSuspending(object sender, SuspendingEventArgs e)
        {
            var deferral = e.SuspendingOperation.GetDeferral();
            try
            {
                // Give Firefox chrome time to flush its session and profile.
                // UWP may terminate the process without a later orderly exit.
                if (Controls.FirefoxWindow.SuspendRuntime())
                    await Task.Delay(2000);
            }
            finally
            {
                deferral.Complete();
            }
        }

        private void OnResuming(object sender, object e)
        {
            Controls.FirefoxWindow.ResumeRuntime();
        }


    }
}
