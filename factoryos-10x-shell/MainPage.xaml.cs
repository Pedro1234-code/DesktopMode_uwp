using Windows.ApplicationModel.Core;
using Windows.UI.ViewManagement;
using Windows.UI;
using Windows.UI.Xaml.Controls;
using factoryos_10x_shell.Controls;
using System;
using Windows.Foundation;
using Windows.UI.Core;
using Windows.UI.Xaml;
using Windows.UI.Input;
using Windows.UI.Xaml.Media;
using factoryos_10x_shell.Library.Services.Managers;
using Microsoft.Extensions.DependencyInjection;
using factoryos_10x_shell.Library.Services.Navigation;
using factoryos_10x_shell.Views;
using Windows.System;
using Windows.System.Profile;
using System.Diagnostics;
using factoryos_10x_shell.Library.Services.Helpers;
using factoryos_10x_shell.Library.Services.Input;
using factoryos_10x_shell.Library.Services.WebApps;

namespace factoryos_10x_shell
{
    public sealed partial class MainPage : Page
    {
        public static MainPage CurrentPage { get; private set; }
        // Xbox UWP's TV-oriented logical scale makes the current shell overly large.
        // Render an expanded logical canvas at this scale to retain full-screen coverage.
        private const double XboxDesktopScale = 0.70;
        // The supplied XL cursor is rendered at the standard Windows pointer size.
        // Its hotspot is (0, 0).
        private const double MouseCursorPixelSize = 16.0;

        private readonly IStartManagerService m_startManager;
        private readonly IActionCenterManagerService m_actionManager;
        private readonly IDesktopNavigator m_desktopNavigator;
        private readonly IDialogService m_dialogService;
        private readonly IMouseInputService m_mouseInput;
        private readonly IWindowManagerService m_windowManager;
        private readonly bool m_useXboxDesktopScale;
        private bool m_isNativeMouseCursorHidden;
        private int m_popupCursorRequests;
        private bool m_hasOpenXamlPopup;
        private bool m_isWindowActive = true;
        private bool m_hasMouseInput;
        private bool m_altTabHeld;

        public MainPage()
        {
            this.InitializeComponent();
            CurrentPage = this;

            m_useXboxDesktopScale = string.Equals(
                AnalyticsInfo.VersionInfo.DeviceFamily,
                "Windows.Xbox",
                StringComparison.OrdinalIgnoreCase);

            m_startManager = App.ServiceProvider.GetRequiredService<IStartManagerService>();
            m_actionManager = App.ServiceProvider.GetService<IActionCenterManagerService>();
            m_desktopNavigator = App.ServiceProvider.GetRequiredService<IDesktopNavigator>();
            m_dialogService = App.ServiceProvider.GetService<IDialogService>();
            m_mouseInput = App.ServiceProvider.GetRequiredService<IMouseInputService>();
            m_windowManager = App.ServiceProvider.GetRequiredService<IWindowManagerService>();


            // Titlebar
            var coreTitleBar = CoreApplication.GetCurrentView().TitleBar;
            coreTitleBar.ExtendViewIntoTitleBar = true;
            ApplicationViewTitleBar titleBar = ApplicationView.GetForCurrentView().TitleBar;
            titleBar.ButtonBackgroundColor = Colors.Transparent;

            // Init frame
            m_desktopNavigator.FrameContext = DesktopFrame;
            m_desktopNavigator.DesktopNavigate(DesktopPageType.RootContentDesktop);

            Window.Current.CoreWindow.KeyDown += CoreWindow_KeyDown;
            Window.Current.CoreWindow.Dispatcher.AcceleratorKeyActivated += Dispatcher_AcceleratorKeyActivated;
            m_mouseInput.Attach(Window.Current.CoreWindow);
            m_mouseInput.InputChanged += MouseInput_InputChanged;

            Loaded += MainPage_Loaded;
            Window.Current.SizeChanged += Window_SizeChanged;
            Window.Current.Activated += Window_Activated;
            CompositionTarget.Rendering += CompositionTarget_Rendering;

        }

        private void MainPage_Loaded(object sender, RoutedEventArgs e)
        {
            ApplyDesktopScale();
        }

        private void Window_SizeChanged(object sender, WindowSizeChangedEventArgs e)
        {
            ApplyDesktopScale();
        }

        private void ApplyDesktopScale()
        {
            double scale = m_useXboxDesktopScale ? XboxDesktopScale : 1.0;
            DesktopScaleTransform.ScaleX = scale;
            DesktopScaleTransform.ScaleY = scale;
            UpdateMouseCursorSize(scale);

            if (scale == 1.0)
            {
                ContentRoot.Width = double.NaN;
                ContentRoot.Height = double.NaN;
                return;
            }

            var bounds = Window.Current.Bounds;
            ContentRoot.Width = Math.Ceiling(bounds.Width / scale);
            ContentRoot.Height = Math.Ceiling(bounds.Height / scale);
        }

        private void MouseInput_InputChanged(object sender, MouseInputChangedEventArgs e)
        {
            double scale = m_useXboxDesktopScale ? XboxDesktopScale : 1.0;
            double logicalCursorSize = MouseCursorPixelSize / scale;

            // PointerPoint coordinates use the physical window space. ContentRoot uses
            // the larger virtual desktop space when the Xbox scale is enabled.
            Canvas.SetLeft(MouseCursor, e.Snapshot.X / scale);
            Canvas.SetTop(MouseCursor, e.Snapshot.Y / scale);
            MouseCursor.Width = logicalCursorSize;
            MouseCursor.Height = logicalCursorSize;
            m_hasMouseInput = true;
            UpdateCursorPresentation();
        }

        // Flyouts and ContentDialogs are rendered in the UWP popup layer, above the
        // custom cursor canvas. Use the system arrow only for their lifetime.
        public static void SetPopupCursorVisibility(bool isPopupOpen)
        {
            CurrentPage?.SetPopupCursorVisibilityCore(isPopupOpen);
        }

        private void SetPopupCursorVisibilityCore(bool isPopupOpen)
        {
            m_popupCursorRequests = Math.Max(0, m_popupCursorRequests + (isPopupOpen ? 1 : -1));
            UpdateCursorPresentation();
        }

        private void CompositionTarget_Rendering(object sender, object e)
        {
            bool hasOpenPopup = VisualTreeHelper.GetOpenPopups(Window.Current).Count > 0;
            if (m_hasOpenXamlPopup == hasOpenPopup) return;

            m_hasOpenXamlPopup = hasOpenPopup;
            UpdateCursorPresentation();
        }

        private void Window_Activated(object sender, WindowActivatedEventArgs e)
        {
            bool isActive = e.WindowActivationState != CoreWindowActivationState.Deactivated;
            if (m_isWindowActive == isActive) return;

            m_isWindowActive = isActive;
            UpdateCursorPresentation();
        }

        private void UpdateCursorPresentation()
        {
            bool useSystemCursor = m_popupCursorRequests > 0 || m_hasOpenXamlPopup || !m_isWindowActive;
            if (useSystemCursor)
            {
                MouseCursor.Visibility = Visibility.Collapsed;
                if (m_isNativeMouseCursorHidden)
                {
                    Window.Current.CoreWindow.PointerCursor = new CoreCursor(CoreCursorType.Arrow, 0);
                    m_isNativeMouseCursorHidden = false;
                }
            }
            else
            {
                if (!m_isNativeMouseCursorHidden)
                {
                    // Match Bandit Launcher: the Windows cursor is hidden and our image
                    // is the only visible pointer while interacting with the Shell layer.
                    Window.Current.CoreWindow.PointerCursor = null;
                    m_isNativeMouseCursorHidden = true;
                }

                MouseCursor.Visibility = m_hasMouseInput ? Visibility.Visible : Visibility.Collapsed;
            }
        }

        private void UpdateMouseCursorSize(double scale)
        {
            double logicalCursorSize = MouseCursorPixelSize / scale;
            MouseCursor.Width = logicalCursorSize;
            MouseCursor.Height = logicalCursorSize;
        }

        private void Dispatcher_AcceleratorKeyActivated(CoreDispatcher sender, AcceleratorKeyEventArgs args)
        {
            if (m_altTabHeld && args.VirtualKey == VirtualKey.Tab &&
                (args.EventType == CoreAcceleratorKeyEventType.KeyUp ||
                 args.EventType == CoreAcceleratorKeyEventType.SystemKeyUp))
            {
                m_altTabHeld = false;
                args.Handled = true;
                return;
            }

            if (args.EventType == CoreAcceleratorKeyEventType.KeyDown ||
                args.EventType == CoreAcceleratorKeyEventType.SystemKeyDown)
            {
                var alt = Window.Current.CoreWindow.GetKeyState(VirtualKey.Menu);
                if (args.VirtualKey == VirtualKey.Tab && alt.HasFlag(CoreVirtualKeyStates.Down))
                {
                    if (!m_altTabHeld)
                    {
                        m_altTabHeld = true;
                        m_windowManager.ToggleTaskView();
                    }
                    args.Handled = true;
                    return;
                }

                var ctrl = Window.Current.CoreWindow.GetKeyState(VirtualKey.Control);
                if (ctrl.HasFlag(CoreVirtualKeyStates.Down))
                {
                    switch (args.VirtualKey)
                    {
                        case VirtualKey.Insert:
                            m_dialogService.OpenDebugMenu();
                            break;
                    }
                }
            }
        }

        private void CoreWindow_KeyDown(CoreWindow sender, KeyEventArgs args)
        {
            if (args.VirtualKey == Windows.System.VirtualKey.LeftWindows || args.VirtualKey == Windows.System.VirtualKey.RightWindows)
            {
                m_startManager.RequestStartVisibilityChange(!m_startManager.IsStartOpen);
                if (m_actionManager.IsActionCenterOpen)
                    m_actionManager.RequestActionVisibilityChange(false);
            }
        }
    }
}
