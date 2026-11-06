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
        private InputPane m_inputPane;
        private Microsoft.UI.Xaml.Controls.WebView2 Browser;
        private Microsoft.Web.WebView2.Core.CoreWebView2 m_edgeCore;
        private ulong m_navigationId;
        private bool m_pointerReleaseQueued;
        private StrawfoxAppView m_strawfoxView;

        public WebAppWindow()
        {
            InitializeComponent();
            DataContextChanged += WebAppWindow_DataContextChanged;
            Loaded += WebAppWindow_Loaded;
            Unloaded += WebAppWindow_Unloaded;

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
            SubscribeInputPane();
            SubscribeMouse();
            if (m_window == null && DataContext is WebAppWindowModel window)
            {
                m_window = window;
                Width = window.Width;
                Height = window.Height;
                window.PropertyChanged += Window_PropertyChanged;
            }
            LogBrowserFailure("view loaded; size=" + ActualWidth + "x" + ActualHeight);
            _ = EnsureBrowserAsync();
        }

        private void EnsureEdgeControl()
        {
            if (Browser != null) return;
            Browser = new Microsoft.UI.Xaml.Controls.WebView2();
            Browser.IsHitTestVisible = false;
            Browser.AddHandler(UIElement.PointerReleasedEvent, new PointerEventHandler(Browser_PointerIdle), true);
            Browser.AddHandler(UIElement.PointerMovedEvent, new PointerEventHandler(Browser_PointerIdle), true);
            Browser.PointerPressed += Browser_PointerPressed;
            Browser.NavigationCompleted += Browser_NavigationCompleted;
            Browser.NavigationStarting += Browser_NavigationStarting;
            Browser.Loaded += Browser_Loaded;
            Browser.CoreWebView2Initialized += Browser_CoreWebView2Initialized;
            BrowserContainer.Content = Browser;
        }

        private void EnsureStrawfoxControl()
        {
            if (m_strawfoxView != null) return;
            ReleaseEdgeControl();
            m_strawfoxView = new StrawfoxAppView(m_window, m_windowManager);
            m_strawfoxView.Failed += error => {
                ShowBrowserError("Strawfox could not open this app window. " + error.Message);
                LogBrowserFailure($"Strawfox window: {error.GetType().Name}; HRESULT=0x{error.HResult:X8}");
            };
            BrowserContainer.Content = m_strawfoxView;
        }

        private void ReleaseEdgeControl()
        {
            if (Browser == null) return;
            ReleaseBrowserPointer("control released");
            var browser = Browser;
            Browser = null;
            m_initializingBrowser = false;
            m_navigatedWindow = null;
            m_navigationId = 0;
            m_browserWatchdog.Stop();
            if (m_edgeCore != null) m_edgeCore.ProcessFailed -= Browser_ProcessFailed;
            m_edgeCore = null;
            browser.NavigationCompleted -= Browser_NavigationCompleted;
            browser.NavigationStarting -= Browser_NavigationStarting;
            browser.PointerPressed -= Browser_PointerPressed;
            browser.RemoveHandler(UIElement.PointerReleasedEvent, new PointerEventHandler(Browser_PointerIdle));
            browser.RemoveHandler(UIElement.PointerMovedEvent, new PointerEventHandler(Browser_PointerIdle));
            browser.Loaded -= Browser_Loaded;
            browser.CoreWebView2Initialized -= Browser_CoreWebView2Initialized;
            if (ReferenceEquals(BrowserContainer.Content, browser)) BrowserContainer.Content = null;
            try { browser.Close(); }
            catch (Exception error) { LogBrowserFailure($"control close: HRESULT=0x{error.HResult:X8}"); }
        }

        private void Browser_Loaded(object sender, RoutedEventArgs e)
        {
            if (!ReferenceEquals(sender, Browser)) return;
            // The parent's Loaded event can precede WebView2's own Loaded.
            LogBrowserFailure("view loaded; size=" + ActualWidth + "x" + ActualHeight);
            _ = EnsureBrowserAsync();
        }

        private void SubscribeInputPane()
        {
            if (m_inputPane != null || m_webViewClosed || Windows.ApplicationModel.DesignMode.DesignModeEnabled) return;
            try
            {
                m_inputPane = InputPane.GetForCurrentView();
                m_inputPane.Showing += BrowserInputPane_Showing;
            }
            catch (Exception error)
            {
                LogBrowserFailure($"keyboard policy unavailable: HRESULT=0x{error.HResult:X8}");
            }
        }

        private void UnsubscribeInputPane()
        {
            if (m_inputPane == null) return;
            m_inputPane.Showing -= BrowserInputPane_Showing;
            m_inputPane = null;
        }

        private bool ShouldDismissBrowserKeyboard()
        {
            if (!m_isLoaded || m_webViewClosed || m_window?.IsActive != true ||
                m_window.Visibility != Visibility.Visible) return false;
            if (Browser == null) return false;
            bool browserFocused = Browser.FocusState != FocusState.Unfocused;
            var focused = FocusManager.GetFocusedElement() as DependencyObject;
            while (!browserFocused && focused != null)
            {
                browserFocused = ReferenceEquals(focused, Browser);
                focused = VisualTreeHelper.GetParent(focused);
            }
            if (!browserFocused) return false;
            try { return new Windows.Devices.Input.KeyboardCapabilities().KeyboardPresent != 0; }
            catch { return false; }
        }

        private async void BrowserInputPane_Showing(InputPane sender, InputPaneVisibilityEventArgs args)
        {
            if (!ShouldDismissBrowserKeyboard()) return;
            try
            {
                sender.TryHide();
                // Xbox can finish the show request after the Showing callback.
                await Dispatcher.RunAsync(Windows.UI.Core.CoreDispatcherPriority.Normal, () =>
                {
                    if (ReferenceEquals(sender, m_inputPane) && ShouldDismissBrowserKeyboard()) sender.TryHide();
                });
            }
            catch (Exception error)
            {
                LogBrowserFailure($"keyboard dismissal failed: HRESULT=0x{error.HResult:X8}");
            }
        }

        private void Browser_CoreWebView2Initialized(Microsoft.UI.Xaml.Controls.WebView2 sender,
            Microsoft.UI.Xaml.Controls.CoreWebView2InitializedEventArgs args)
        {
            if (m_webViewClosed || !ReferenceEquals(sender, Browser)) return;
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
            if (Browser != null) Browser.IsHitTestVisible = false;
            ReleaseBrowserPointer("browser error");
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
            LogBrowserFailure($"timeout: stage={m_browserStage}; initializing={m_initializingBrowser}; controlAvailable={Browser != null}");
            m_browserStartupFailed = true;
            ReleaseEdgeControl();
            ShowBrowserError("The web app did not finish starting or loading. Select Try again to restart it.");
        }

        private void Browser_ProcessFailed(Microsoft.Web.WebView2.Core.CoreWebView2 sender,
            Microsoft.Web.WebView2.Core.CoreWebView2ProcessFailedEventArgs args)
        {
            if (m_webViewClosed || !ReferenceEquals(sender, m_edgeCore)) return;
            LogBrowserFailure($"process failed: kind={args.ProcessFailedKind}");
            switch (args.ProcessFailedKind)
            {
                case Microsoft.Web.WebView2.Core.CoreWebView2ProcessFailedKind.BrowserProcessExited:
                case Microsoft.Web.WebView2.Core.CoreWebView2ProcessFailedKind.RenderProcessExited:
                case Microsoft.Web.WebView2.Core.CoreWebView2ProcessFailedKind.RenderProcessUnresponsive:
                    m_browserStartupFailed = true;
                    ShowBrowserError("The WebView2 browser stopped responding or exited. Select Try again to restart it.");
                    break;
            }
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
            if (m_window != null && !ReferenceEquals(m_window, args.NewValue) && Browser != null)
            {
                ReleaseEdgeControl();
                m_browserStartupFailed = false;
                m_browserInitializationException = null;
                BrowserError.Visibility = Visibility.Collapsed;
            }
            m_window = args.NewValue as WebAppWindowModel;
            if (m_window == null) return;
            Width = m_window.Width;
            Height = m_window.Height;
            m_window.PropertyChanged += Window_PropertyChanged;
            LogBrowserFailure("view loaded; size=" + ActualWidth + "x" + ActualHeight);
            _ = EnsureBrowserAsync();
        }

        private async Task EnsureBrowserAsync()
        {
            if (!m_isLoaded || m_webViewClosed || m_window?.App == null || m_browserStartupFailed) return;
            if (m_initializingBrowser || m_navigatedWindow == m_window || Windows.ApplicationModel.DesignMode.DesignModeEnabled) return;
            var window = m_window;
            Microsoft.UI.Xaml.Controls.WebView2 browser = null;
            try
            {
                if (window.App.UseStrawfoxWindow) { EnsureStrawfoxControl(); return; }
                // Attaching the control may synchronously raise Loaded.
                m_initializingBrowser = true;
                EnsureEdgeControl();
                browser = Browser;
                if (!browser.IsLoaded) return;
                if (!Uri.TryCreate(window.App.StartUri, UriKind.Absolute, out Uri uri) ||
                    (uri.Scheme != Uri.UriSchemeHttp && uri.Scheme != Uri.UriSchemeHttps))
                    throw new ArgumentException("Invalid web app address.");
                m_browserInitializationException = null;
                LogBrowserPlatform();
                m_browserStage = "initializing core";
                m_browserWatchdog.Start();
                browser.ApplyTemplate();
                LogBrowserFailure($"initialization requested; loaded={browser.IsLoaded}; size={browser.ActualWidth:F0}x{browser.ActualHeight:F0}");
                await browser.EnsureCoreWebView2Async();
                if (m_webViewClosed || m_window != window || !ReferenceEquals(browser, Browser)) return;
                if (m_browserInitializationException != null) throw m_browserInitializationException;
                if (browser.CoreWebView2 == null)
                    throw new InvalidOperationException("WebView2 initialization completed without a CoreWebView2 instance.");
                m_edgeCore = browser.CoreWebView2;
                m_edgeCore.ProcessFailed += Browser_ProcessFailed;
                m_navigatedWindow = window;
                browser.IsHitTestVisible = true;
                m_browserStage = "navigation requested";
                m_edgeCore.Navigate(uri.AbsoluteUri);
            }
            catch (Exception exception)
            {
                if (!m_webViewClosed && m_window == window && (browser == null || ReferenceEquals(browser, Browser)))
                {
                    m_browserStartupFailed = true;
                    ShowBrowserError(BrowserStartupErrorText(exception));
                    LogBrowserFailure($"initialization: {exception.GetType().Name}; HRESULT=0x{exception.HResult:X8}");
                }
            }
            finally
            {
                if (browser == null || ReferenceEquals(browser, Browser)) m_initializingBrowser = false;
            }
        }

        private void Browser_NavigationStarting(Microsoft.UI.Xaml.Controls.WebView2 sender,
            Microsoft.Web.WebView2.Core.CoreWebView2NavigationStartingEventArgs args)
        {
            if (m_webViewClosed || m_browserStartupFailed || !ReferenceEquals(sender, Browser)) return;
            m_navigationId = args.NavigationId;
            m_browserStage = "navigation started";
            m_browserWatchdog.Start();
            LogBrowserFailure("navigation started");
            BrowserError.Visibility = Visibility.Collapsed;
        }

        private async void RetryBrowser_Click(object sender, RoutedEventArgs e)
        {
            if (m_webViewClosed || (m_initializingBrowser && !m_browserStartupFailed)) return;
            LogBrowserFailure("retry requested");
            m_browserStartupFailed = false;
            BrowserError.Visibility = Visibility.Collapsed;
            if (m_window?.App?.UseStrawfoxWindow == true)
            {
                m_strawfoxView?.Dispose();
                m_strawfoxView = null;
                BrowserContainer.Content = null;
                await EnsureBrowserAsync();
                return;
            }
            ReleaseEdgeControl();
            await EnsureBrowserAsync();
        }

        private void LogBrowserFailure(string detail)
        {
            factoryos_10x_shell.Services.WebApps.WebAppDiagnostics.Write(m_window, "backend", detail);
        }
        private void Window_PropertyChanged(object sender, System.ComponentModel.PropertyChangedEventArgs e)
        {
            if ((e.PropertyName == nameof(WebAppWindowModel.IsActive) && !m_window.IsActive) ||
                (e.PropertyName == nameof(WebAppWindowModel.Visibility) && m_window.Visibility != Visibility.Visible))
                ReleaseBrowserPointer("window inactive");
            if (e.PropertyName == nameof(WebAppWindowModel.Width)) Width = m_window.Width;
            if (e.PropertyName == nameof(WebAppWindowModel.Height)) Height = m_window.Height;
            m_strawfoxView?.UpdateActivation();
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

        private void ReleaseBrowserPointer(string reason)
        {
            var browser = Browser;
            if (browser == null) return;
            try
            {
                int captures = browser.PointerCaptures?.Count ?? 0;
                if (captures == 0) return;
                browser.ReleasePointerCaptures();
                LogBrowserFailure($"pointer capture released: reason={reason}; count={captures}");
            }
            catch (Exception error)
            {
                LogBrowserFailure($"pointer release failed: HRESULT=0x{error.HResult:X8}");
            }
        }

        private async void Browser_PointerIdle(object sender, PointerRoutedEventArgs e)
        {
            var browser = Browser;
            if (!ReferenceEquals(sender, browser) || browser == null || m_pointerReleaseQueued ||
                e.Pointer.PointerDeviceType != Windows.Devices.Input.PointerDeviceType.Mouse ||
                browser.PointerCaptures?.Count == 0) return;
            var properties = e.GetCurrentPoint(browser).Properties;
            if (properties.IsLeftButtonPressed || properties.IsMiddleButtonPressed || properties.IsRightButtonPressed ||
                properties.IsXButton1Pressed || properties.IsXButton2Pressed) return;
            m_pointerReleaseQueued = true;
            try
            {
                // Let WebView2 deliver its normal button-up before clearing stale capture.
                await Dispatcher.RunAsync(Windows.UI.Core.CoreDispatcherPriority.Low, () =>
                {
                    if (!ReferenceEquals(browser, Browser) || m_webViewClosed) return;
                    var point = Windows.UI.Input.PointerPoint.GetCurrentPoint(e.Pointer.PointerId).Properties;
                    if (!point.IsLeftButtonPressed && !point.IsMiddleButtonPressed && !point.IsRightButtonPressed &&
                        !point.IsXButton1Pressed && !point.IsXButton2Pressed)
                        ReleaseBrowserPointer("all mouse buttons released");
                });
            }
            catch (Exception error) { LogBrowserFailure($"pointer state check failed: HRESULT=0x{error.HResult:X8}"); }
            finally { m_pointerReleaseQueued = false; }
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
            if (m_webViewClosed || !ReferenceEquals(sender, Browser) || args.NavigationId != m_navigationId) return;
            LogBrowserFailure($"navigation completed: id={args.NavigationId}; success={args.IsSuccess}; status={args.WebErrorStatus}; size={sender.ActualWidth:F0}x{sender.ActualHeight:F0}");
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
            var browser = Browser;
            var window = m_window;
            if (m_webViewClosed || window?.App == null || browser?.CoreWebView2 == null || !string.IsNullOrEmpty(window.App.IconUri)) return;
            try
            {
                const string script = "(() => { const link = [...document.querySelectorAll('link[rel~=icon]')].find(x => x.href); return link ? new URL(link.href, location.href).href : new URL('/favicon.ico', location.origin).href; })()";
                string jsonResult = await browser.CoreWebView2.ExecuteScriptAsync(script);
                if (m_webViewClosed || !ReferenceEquals(browser, Browser) || m_window != window) return;
                string faviconUri = JsonConvert.DeserializeObject<string>(jsonResult);
                if (m_webAppService != null)
                    await m_webAppService.SaveFaviconAsync(window.App, faviconUri);
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
            if (m_strawfoxView != null) { m_windowManager?.Close(m_window); return; }
            ShutdownWebView();
            m_windowManager?.Close(m_window);
        }

        public void ShutdownWebView()
        {
            if (m_webViewClosed) return;
            LogBrowserFailure("backend shutdown");
            m_webViewClosed = true;
            UnsubscribeInputPane();
            m_browserWatchdog.Stop();
            if (m_mouseInput != null)
                m_mouseInput.InputChanged -= MouseInput_InputChanged;
            m_mouseSubscribed = false;
            if (m_windowManager != null) m_windowManager.Windows.CollectionChanged -= Windows_CollectionChanged;
            if (m_window != null) m_window.PropertyChanged -= Window_PropertyChanged;
            m_strawfoxView?.Dispose();
            m_strawfoxView = null;
            ReleaseEdgeControl();
            BrowserContainer.Content = null;
        }

        private void WebAppWindow_Unloaded(object sender, RoutedEventArgs e)
        {
            m_isLoaded = false;
            ReleaseBrowserPointer("view unloaded");
            UnsubscribeInputPane();
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
