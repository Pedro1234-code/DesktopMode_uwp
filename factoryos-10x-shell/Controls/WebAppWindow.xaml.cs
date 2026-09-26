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
        private bool m_resizing;
        private bool m_webViewClosed;
        private bool m_runtimeInitialized;

        public WebAppWindow()
        {
            InitializeComponent();
            DataContextChanged += WebAppWindow_DataContextChanged;
            Loaded += WebAppWindow_Loaded;
            Unloaded += WebAppWindow_Unloaded;
            Browser.PointerPressed += Browser_PointerPressed;
            Browser.NavigationCompleted += Browser_NavigationCompleted;
            TryInitializeRuntimeServices();
        }

        private void WebAppWindow_Loaded(object sender, RoutedEventArgs e)
        {
            // The XAML designer creates this control before App.ServiceProvider exists.
            // Resolve runtime-only services after the control has entered the visual tree.
            TryInitializeRuntimeServices();
        }

        private bool TryInitializeRuntimeServices()
        {
            if (m_runtimeInitialized || Windows.ApplicationModel.DesignMode.DesignModeEnabled) return m_runtimeInitialized;
            // Do not invoke App.ServiceProvider until it has been configured: its
            // getter intentionally throws when the application is not running.
            if (!App.HasServiceProvider) return false;
            m_runtimeInitialized = true;
            m_windowManager = App.ServiceProvider.GetRequiredService<IWindowManagerService>();
            m_webAppService = App.ServiceProvider.GetRequiredService<IWebAppService>();
            m_mouseInput = App.ServiceProvider.GetRequiredService<IMouseInputService>();
            m_mouseInput.InputChanged += MouseInput_InputChanged;
            return true;
        }

        private void WebAppWindow_DataContextChanged(FrameworkElement sender, DataContextChangedEventArgs args)
        {
            m_window = args.NewValue as WebAppWindowModel;
            if (m_window == null) return;
            Width = m_window.Width;
            Height = m_window.Height;
            m_window.PropertyChanged += Window_PropertyChanged;
            if (Uri.TryCreate(m_window.App.StartUri, UriKind.Absolute, out Uri uri)) Browser.Source = uri;
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
            m_resizeStart = e.GetCurrentPoint(this).Position;
            m_startWidth = m_window.Width;
            m_startHeight = m_window.Height;
            ResizeGrip.CapturePointer(e.Pointer);
        }

        private void ResizeGrip_PointerMoved(object sender, PointerRoutedEventArgs e)
        {
            if (!m_resizing) return;
            Point position = e.GetCurrentPoint(this).Position;
            m_window.Width = Math.Max(360, m_startWidth + position.X - m_resizeStart.X);
            m_window.Height = Math.Max(260, m_startHeight + position.Y - m_resizeStart.Y);
        }

        private void ResizeGrip_PointerReleased(object sender, PointerRoutedEventArgs e)
        {
            m_resizing = false;
            ResizeGrip.ReleasePointerCaptures();
        }

        private void ResizeGrip_PointerCaptureLost(object sender, PointerRoutedEventArgs e)
        {
            m_resizing = false;
        }

        private void MouseInput_InputChanged(object sender, MouseInputChangedEventArgs e)
        {
            if (m_window == null || m_window.Visibility != Visibility.Visible) return;
            double scale = string.Equals(AnalyticsInfo.VersionInfo.DeviceFamily, "Windows.Xbox", StringComparison.OrdinalIgnoreCase) ? 0.75 : 1.0;
            if (e.Kind == MouseInputChangeKind.Released)
            {
                m_dragging = false;
                m_resizing = false;
            }
            if (m_dragging)
            {
                MoveWindow(new Point(e.Snapshot.X / scale - m_window.Left, e.Snapshot.Y / scale - m_window.Top));
            }
        }

        private async void Browser_NavigationCompleted(Microsoft.UI.Xaml.Controls.WebView2 sender, Microsoft.Web.WebView2.Core.CoreWebView2NavigationCompletedEventArgs args)
        {
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
            if (m_mouseInput != null)
                m_mouseInput.InputChanged -= MouseInput_InputChanged;
            Browser.NavigationCompleted -= Browser_NavigationCompleted;
            Browser.Close();
        }

        private void WebAppWindow_Unloaded(object sender, RoutedEventArgs e) => ShutdownWebView();

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
