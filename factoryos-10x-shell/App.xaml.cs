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
            try
            {
                await new RuntimeHost().EnsureDriveRootAsync();
                IAppHelper appHelper = ServiceProvider.GetRequiredService<IAppHelper>();
                await appHelper.LoadAppsAsync();
                await ServiceProvider.GetRequiredService<IWebAppService>().InitializeAsync();
            }
            catch (Exception exception)
            {
                // App discovery is supplemental. A package, icon, or runtime
                // that cannot be inspected must not delay or terminate Shell
                // startup.
                Debug.WriteLine($"Deferred startup initialization failed: {exception}");
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
            if (args.Kind == ActivationKind.Protocol)
            {
                var protocolArgs = args as ProtocolActivatedEventArgs;

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
