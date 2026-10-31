using factoryos_10x_shell.Library.Models.InternalData;
using factoryos_10x_shell.Library.Services.WebApps;
using factoryos_10x_shell.Library.Services.Input;
using Microsoft.Extensions.DependencyInjection;
using Newtonsoft.Json;
using System;
using Windows.Foundation;
using Windows.System.Profile;
using Windows.UI.Xaml;
using Windows.UI.Xaml.Controls;
using Windows.UI.Xaml.Input;
using Windows.UI.Xaml.Media;
using Windows.UI.ViewManagement;
using System.Threading.Tasks;

namespace factoryos_10x_shell.Controls
{
    public sealed partial class WebAppWindow : UserControl
    {
        private static readonly double s_pointerScale =
            string.Equals(AnalyticsInfo.VersionInfo.DeviceFamily, "Windows.Xbox", StringComparison.OrdinalIgnoreCase) ? 0.75 : 1.0;
        private IWindowManagerService m_windowManager;
        private IWebAppService m_webAppService;
        private IMouseInputService m_mouseInput;
        private WebAppWindowModel m_window;
        private Point m_dragStart;
        private double m_startLeft;
        private double m_startTop;
        private bool m_dragging;
        private Point m_resizeStart;
        private double m_startWidth;
        private double m_startHeight;
        private WindowResizeCorner m_resizeCorner;
        private bool m_resizing;
        private bool m_webViewClosed;
        private bool m_runtimeInitialized;
        private bool m_isLoaded;
        private bool m_initializingBrowser;
        private bool m_mouseSubscribed;
        private WebAppWindowModel m_navigatedWindow;
        private Exception m_browserInitializationException;
        private readonly DispatcherTimer m_browserWatchdog = new DispatcherTimer();
        private string m_browserStage;
        private bool m_browserStartupFailed;
        private bool m_loggedBrowserPlatform;

        public WebAppWindow()
        {
            InitializeComponent();
            DataContextChanged += WebAppWindow_DataContextChanged;
            Loaded += WebAppWindow_Loaded;
            Unloaded += WebAppWindow_Unloaded;
            Browser.PointerPressed += Browser_PointerPressed;
            Browser.NavigationCompleted += Browser_NavigationCompleted;
            Browser.NavigationStarting += Browser_NavigationStarting;
            Browser.Loaded += Browser_Loaded;
            Browser.CoreWebView2Initialized += Browser_CoreWebView2Initialized;
            m_browserWatchdog.Interval = TimeSpan.FromSeconds(60);
            m_browserWatchdog.Tick += BrowserWatchdog_Tick;
            TryInitializeRuntimeServices();
        }

        private void WebAppWindow_Loaded(object sender, RoutedEventArgs e)
        {
            // The XAML designer creates this control before App.ServiceProvider exists.
            // Resolve runtime-only services after the control has entered the visual tree.
            TryInitializeRuntimeServices();
            m_isLoaded = true;
            SubscribeMouse();
            if (m_window == null && DataContext is WebAppWindowModel window)
            {
                m_window = window;
                Width = window.Width;
                Height = window.Height;
                window.PropertyChanged += Window_PropertyChanged;
            }
            _ = EnsureBrowserAsync();
        }

        private void Browser_Loaded(object sender, RoutedEventArgs e)
        {
            // The parent's Loaded event can precede WebView2's own Loaded.
            _ = EnsureBrowserAsync();
        }

        private void Browser_CoreWebView2Initialized(Microsoft.UI.Xaml.Controls.WebView2 sender,
            Microsoft.UI.Xaml.Controls.CoreWebView2InitializedEventArgs args)
        {
            if (m_webViewClosed) return;
            m_browserInitializationException = args.Exception;
            if (args.Exception != null)
            {
                m_browserStartupFailed = true;
                ShowBrowserError(BrowserStartupErrorText(args.Exception));
                LogBrowserFailure($"core initialization failed: {args.Exception.GetType().Name}; HRESULT=0x{args.Exception.HResult:X8}");
            }
            else
            {
                m_browserStage = "core initialized; waiting for navigation";
                LogBrowserFailure("core initialized; coreAvailable=" + (sender.CoreWebView2 != null));
            }
        }

        private void ShowBrowserError(string message)
        {
            m_browserWatchdog.Stop();
            BrowserErrorText.Text = message;
            BrowserError.Visibility = Visibility.Visible;
        }

        private static string BrowserStartupErrorText(Exception exception)
        {
            if (exception.HResult == unchecked((int)0x80080005))
                return "The WebView2 runtime could not start its browser process (0x80080005). The page has not started loading.";
            return "The web app could not be started. Try again. Error: 0x" + exception.HResult.ToString("X8");
        }

        private void LogBrowserPlatform()
        {
            if (m_loggedBrowserPlatform) return;
            m_loggedBrowserPlatform = true;
            try
            {
                var info = AnalyticsInfo.VersionInfo;
                string version = "unavailable";
                if (ulong.TryParse(info.DeviceFamilyVersion, out ulong packed))
                    version = $"{(packed >> 48) & 0xffff}.{(packed >> 32) & 0xffff}.{(packed >> 16) & 0xffff}.{packed & 0xffff}";
                var packageVersion = Windows.ApplicationModel.Package.Current.Id.Version;
                LogBrowserFailure($"platform: family={info.DeviceFamily}; OS={version}; " +
                    $"packageVersion={packageVersion.Major}.{packageVersion.Minor}.{packageVersion.Build}.{packageVersion.Revision}; " +
                    $"memoryLimitMB={Windows.System.MemoryManager.AppMemoryUsageLimit / (1024 * 1024)}");
            }
            catch (Exception exception)
            {
                LogBrowserFailure($"platform query failed: HRESULT=0x{exception.HResult:X8}");
            }
        }

        private void BrowserWatchdog_Tick(object sender, object e)
        {
            if (m_webViewClosed) { m_browserWatchdog.Stop(); return; }
            ShowBrowserError("The web app is taking too long to load. Check your connection and try again.");
            LogBrowserFailure($"timeout: stage={m_browserStage}; loaded={Browser.IsLoaded}; coreAvailable={Browser.CoreWebView2 != null}; size={Browser.ActualWidth:F0}x{Browser.ActualHeight:F0}");
        }

        private bool TryInitializeRuntimeServices()
        {
            if (m_runtimeInitialized || Windows.ApplicationModel.DesignMode.DesignModeEnabled) return m_runtimeInitialized;
            // Do not invoke App.ServiceProvider until it has been configured: its
            // getter intentionally throws when the application is not running.
            if (!App.HasServiceProvider) return false;
            m_windowManager = App.ServiceProvider.GetRequiredService<IWindowManagerService>();
            m_webAppService = App.ServiceProvider.GetRequiredService<IWebAppService>();
            m_mouseInput = App.ServiceProvider.GetRequiredService<IMouseInputService>();
            m_windowManager.Windows.CollectionChanged += Windows_CollectionChanged;
            m_runtimeInitialized = true;
            SubscribeMouse();
            return true;
        }

        private void SubscribeMouse()
        {
            if (!m_isLoaded || m_mouseSubscribed || m_mouseInput == null || m_webViewClosed) return;
            m_mouseInput.InputChanged += MouseInput_InputChanged;
            m_mouseSubscribed = true;
        }

        private void Windows_CollectionChanged(object sender, System.Collections.Specialized.NotifyCollectionChangedEventArgs e)
        {
            if (m_window != null && !m_windowManager.Windows.Contains(m_window)) ShutdownWebView();
        }

        private void WebAppWindow_DataContextChanged(FrameworkElement sender, DataContextChangedEventArgs args)
        {
            if (m_window != null) m_window.PropertyChanged -= Window_PropertyChanged;
            m_window = args.NewValue as WebAppWindowModel;
            if (m_window == null) return;
            Width = m_window.Width;
            Height = m_window.Height;
            m_window.PropertyChanged += Window_PropertyChanged;
            _ = EnsureBrowserAsync();
        }

        private async Task EnsureBrowserAsync()
        {
            if (!m_isLoaded || !Browser.IsLoaded || m_webViewClosed || m_initializingBrowser || m_browserStartupFailed || m_window?.App == null ||
                m_navigatedWindow == m_window || Windows.ApplicationModel.DesignMode.DesignModeEnabled) return;
            m_initializingBrowser = true;
            try
            {
                var window = m_window;
                if (!Uri.TryCreate(window.App.StartUri, UriKind.Absolute, out Uri uri) ||
                    (uri.Scheme != Uri.UriSchemeHttp && uri.Scheme != Uri.UriSchemeHttps))
                    throw new ArgumentException("Invalid web app address.");
                m_browserInitializationException = null;
                LogBrowserPlatform();
                m_browserStage = "initializing core";
                m_browserWatchdog.Start();
                Browser.ApplyTemplate();
                // Queue the destination on the control before initialization.
                // WebView2 itself delivers Source once its core is ready.
                Browser.Source = uri;
                LogBrowserFailure($"initialization requested; loaded={Browser.IsLoaded}; size={Browser.ActualWidth:F0}x{Browser.ActualHeight:F0}");
                await Browser.EnsureCoreWebView2Async();
                if (m_webViewClosed || m_window != window) return;
                if (m_browserInitializationException != null) throw m_browserInitializationException;
                if (Browser.CoreWebView2 == null)
                    throw new InvalidOperationException("WebView2 initialization completed without a CoreWebView2 instance.");
                m_navigatedWindow = window;
            }
            catch (Exception exception)
            {
                if (!m_webViewClosed)
                {
                    m_browserStartupFailed = true;
                    ShowBrowserError(BrowserStartupErrorText(exception));
                    LogBrowserFailure($"initialization: {exception.GetType().Name}; HRESULT=0x{exception.HResult:X8}");
                }
            }
            finally { m_initializingBrowser = false; }
        }

        private void Browser_NavigationStarting(Microsoft.UI.Xaml.Controls.WebView2 sender,
            Microsoft.Web.WebView2.Core.CoreWebView2NavigationStartingEventArgs args)
        {
            m_browserStage = "navigation started";
            m_browserWatchdog.Start();
            LogBrowserFailure("navigation started");
            BrowserError.Visibility = Visibility.Collapsed;
        }

        private async void RetryBrowser_Click(object sender, RoutedEventArgs e)
        {
            if (m_webViewClosed || m_initializingBrowser) return;
            m_browserStartupFailed = false;
            BrowserError.Visibility = Visibility.Collapsed;
            if (m_navigatedWindow == m_window && Browser.CoreWebView2 != null)
            {
                try
                {
                    m_browserStage = "reload requested";
                    m_browserWatchdog.Start();
                    Browser.CoreWebView2.Reload();
                }
                catch (Exception exception)
                {
                    BrowserErrorText.Text = "The web app could not be reloaded. Close it and open it again.";
                    BrowserError.Visibility = Visibility.Visible;
                    LogBrowserFailure($"reload: {exception.GetType().Name}; HRESULT=0x{exception.HResult:X8}");
                }
            }
            else await EnsureBrowserAsync();
        }

        private static void LogBrowserFailure(string detail)
        {
            System.Diagnostics.Debug.WriteLine("[WebApps] " + detail);
            try
            {
                string path = System.IO.Path.Combine(Windows.Storage.ApplicationData.Current.LocalFolder.Path,
                    "WebApps-runtime.log");
                string line = $"{DateTimeOffset.UtcNow:O} {detail}{System.Environment.NewLine}";
                _ = Task.Run(() =>
                {
                    try { System.IO.File.AppendAllText(path, line); }
                    catch (Exception) { }
                });
            }
            catch (Exception) { }
        }

        private void Window_PropertyChanged(object sender, System.ComponentModel.PropertyChangedEventArgs e)
        {
            if (e.PropertyName == nameof(WebAppWindowModel.Width)) Width = m_window.Width;
            if (e.PropertyName == nameof(WebAppWindowModel.Height)) Height = m_window.Height;
            FrameworkElement container = VisualTreeHelper.GetParent(this) as FrameworkElement;
            if (container == null) return;
            if (e.PropertyName == nameof(WebAppWindowModel.ZIndex)) Canvas.SetZIndex(container, m_window.ZIndex);
            if (e.PropertyName == nameof(WebAppWindowModel.Visibility)) container.Visibility = m_window.Visibility;
        }

        private void Root_PointerPressed(object sender, PointerRoutedEventArgs e)
        {
            TryInitializeRuntimeServices();
            m_windowManager?.Activate(m_window);
        }

        private void Browser_PointerPressed(object sender, PointerRoutedEventArgs e)
        {
            TryInitializeRuntimeServices();
            m_windowManager?.Activate(m_window);
        }

        private void TitleBar_PointerPressed(object sender, PointerRoutedEventArgs e)
        {
            if (m_window == null || m_window.IsMaximized) return;
            TryInitializeRuntimeServices();
            m_dragging = true;
            Point position = e.GetCurrentPoint(this).Position;
            // Store the starting point in desktop coordinates. PointerMoved on the
            // title bar can stop once the pointer leaves it, so movement itself is
            // updated from the CoreWindow mouse stream below.
            m_dragStart = new Point(m_window.Left + position.X, m_window.Top + position.Y);
            m_startLeft = m_window.Left;
            m_startTop = m_window.Top;
            TitleBar.CapturePointer(e.Pointer);
            m_windowManager?.Activate(m_window);
        }

        private void TitleBar_PointerMoved(object sender, PointerRoutedEventArgs e)
        {
            MoveWindow(e.GetCurrentPoint(this).Position);
        }

        private void TitleBar_PointerReleased(object sender, PointerRoutedEventArgs e)
        {
            m_dragging = false;
            TitleBar.ReleasePointerCaptures();
        }

        private void TitleBar_PointerCaptureLost(object sender, PointerRoutedEventArgs e)
        {
            m_dragging = false;
        }

        private void ResizeGrip_PointerPressed(object sender, PointerRoutedEventArgs e)
        {
            if (m_window == null || m_window.IsMaximized) return;
            m_resizing = true;
            m_resizeCorner = WindowResize.CornerFromTag((sender as FrameworkElement)?.Tag);
            m_resizeStart = e.GetCurrentPoint(null).Position;
            m_startLeft = m_window.Left;
            m_startTop = m_window.Top;
            m_startWidth = m_window.Width;
            m_startHeight = m_window.Height;
            (sender as UIElement)?.CapturePointer(e.Pointer);
        }

        private void ResizeGrip_PointerMoved(object sender, PointerRoutedEventArgs e)
        {
            if (!m_resizing) return;
            Rect bounds = WindowResize.Calculate(m_resizeCorner, m_resizeStart,
                e.GetCurrentPoint(null).Position,
                new Rect(m_startLeft, m_startTop, m_startWidth, m_startHeight), 360, 260);
            m_window.Left = bounds.X; m_window.Top = bounds.Y;
            m_window.Width = bounds.Width; m_window.Height = bounds.Height;
            UpdateCanvasPosition();
        }

        private void ResizeGrip_PointerReleased(object sender, PointerRoutedEventArgs e)
        {
            m_resizing = false;
            (sender as UIElement)?.ReleasePointerCaptures();
        }

        private void ResizeGrip_PointerCaptureLost(object sender, PointerRoutedEventArgs e)
        {
            m_resizing = false;
        }

        private void MouseInput_InputChanged(object sender, MouseInputChangedEventArgs e)
        {
            if (m_window == null || m_window.Visibility != Visibility.Visible) return;
            if (e.Kind == MouseInputChangeKind.Released)
            {
                m_dragging = false;
                m_resizing = false;
            }
            if (m_dragging)
            {
                MoveWindow(new Point(e.Snapshot.X / s_pointerScale - m_window.Left, e.Snapshot.Y / s_pointerScale - m_window.Top));
            }
        }

        private async void Browser_NavigationCompleted(Microsoft.UI.Xaml.Controls.WebView2 sender, Microsoft.Web.WebView2.Core.CoreWebView2NavigationCompletedEventArgs args)
        {
            if (m_webViewClosed) return;
            LogBrowserFailure($"navigation completed: success={args.IsSuccess}; status={args.WebErrorStatus}; size={Browser.ActualWidth:F0}x{Browser.ActualHeight:F0}");
            if (!args.IsSuccess)
            {
                if (args.WebErrorStatus == Microsoft.Web.WebView2.Core.CoreWebView2WebErrorStatus.OperationCanceled) return;
                ShowBrowserError("This page could not be loaded (" + args.WebErrorStatus + "). Check your connection and try again.");
                LogBrowserFailure("navigation: " + args.WebErrorStatus);
                return;
            }
            m_browserWatchdog.Stop();
            m_browserStage = "navigation completed";
            BrowserError.Visibility = Visibility.Collapsed;
            await CaptureFaviconAsync();
        }

        private async System.Threading.Tasks.Task CaptureFaviconAsync()
        {
            if (m_webViewClosed || m_window?.App == null || Browser.CoreWebView2 == null || !string.IsNullOrEmpty(m_window.App.IconUri)) return;
            try
            {
                const string script = "(() => { const link = [...document.querySelectorAll('link[rel~=icon]')].find(x => x.href); return link ? new URL(link.href, location.href).href : new URL('/favicon.ico', location.origin).href; })()";
                string jsonResult = await Browser.CoreWebView2.ExecuteScriptAsync(script);
                string faviconUri = JsonConvert.DeserializeObject<string>(jsonResult);
                if (m_webAppService != null)
                    await m_webAppService.SaveFaviconAsync(m_window.App, faviconUri);
            }
            catch
            {
                // Some pages prevent script execution; their default globe remains.
            }
        }

        private async void Minimize_Click(object sender, RoutedEventArgs e)
        {
            TryInitializeRuntimeServices();
            m_windowManager?.Minimize(m_window);
            HideOnScreenKeyboard();

            // Xbox can request the keyboard only after the focus change caused by
            // collapsing the window, so dismiss it once more on the next UI turn.
            await Task.Delay(100);
            HideOnScreenKeyboard();
        }

        private static void HideOnScreenKeyboard()
        {
            try
            {
                InputPane.GetForCurrentView().TryHide();
            }
            catch
            {
                // InputPane is unavailable in the XAML designer.
            }
        }
        private void Maximize_Click(object sender, RoutedEventArgs e)
        {
            TryInitializeRuntimeServices();
            m_windowManager?.ToggleMaximize(m_window);
            UpdateCanvasPosition();
        }
        private void Close_Click(object sender, RoutedEventArgs e)
        {
            TryInitializeRuntimeServices();
            ShutdownWebView();
            m_windowManager?.Close(m_window);
        }

        public void ShutdownWebView()
        {
            if (m_webViewClosed) return;
            m_webViewClosed = true;
            m_browserWatchdog.Stop();
            if (m_mouseInput != null)
                m_mouseInput.InputChanged -= MouseInput_InputChanged;
            m_mouseSubscribed = false;
            if (m_windowManager != null) m_windowManager.Windows.CollectionChanged -= Windows_CollectionChanged;
            if (m_window != null) m_window.PropertyChanged -= Window_PropertyChanged;
            Browser.NavigationCompleted -= Browser_NavigationCompleted;
            Browser.NavigationStarting -= Browser_NavigationStarting;
            Browser.PointerPressed -= Browser_PointerPressed;
            Browser.Loaded -= Browser_Loaded;
            Browser.CoreWebView2Initialized -= Browser_CoreWebView2Initialized;
            Browser.Close();
        }

        private void WebAppWindow_Unloaded(object sender, RoutedEventArgs e)
        {
            m_isLoaded = false;
            m_dragging = m_resizing = false;
            if (m_mouseSubscribed)
            {
                m_mouseInput.InputChanged -= MouseInput_InputChanged;
                m_mouseSubscribed = false;
            }
            // Unloaded also occurs during visual-tree changes. Close() is final;
            // only call it after the actual window model has been removed.
            if (m_window == null || m_windowManager?.Windows.Contains(m_window) != true)
                ShutdownWebView();
        }

        private void UpdateCanvasPosition()
        {
            var container = VisualTreeHelper.GetParent(this) as UIElement;
            if (container == null || m_window == null) return;
            Canvas.SetLeft(container, m_window.Left);
            Canvas.SetTop(container, m_window.Top);
        }

        private void MoveWindow(Point localPointerPosition)
        {
            if (!m_dragging || m_window == null) return;
            double pointerX = m_window.Left + localPointerPosition.X;
            double pointerY = m_window.Top + localPointerPosition.Y;
            m_window.Left = Math.Max(0, m_startLeft + pointerX - m_dragStart.X);
            m_window.Top = Math.Max(0, m_startTop + pointerY - m_dragStart.Y);
            UpdateCanvasPosition();
        }
    }
}
