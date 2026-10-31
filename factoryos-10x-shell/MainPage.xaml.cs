using Windows.ApplicationModel.Core;
using Windows.UI.ViewManagement;
using Windows.UI;
using Windows.UI.Xaml.Controls;
using Windows.UI.Xaml.Input;
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
using System.IO;
using System.Text;
using System.Threading;
using System.Threading.Tasks;
using factoryos_10x_shell.Library.Services.Helpers;
using factoryos_10x_shell.Library.Services.Input;
using factoryos_10x_shell.Library.Services.WebApps;
using factoryos_10x_shell.Services.Helpers;
using factoryos_10x_shell.Services.Windowing;

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
        private const string CustomCursorEnabledSettingKey = "CoreShell.CustomCursorEnabled";

        private readonly IStartManagerService m_startManager;
        private readonly IActionCenterManagerService m_actionManager;
        private readonly IDesktopNavigator m_desktopNavigator;
        private readonly IDialogService m_dialogService;
        private readonly IMouseInputService m_mouseInput;
        private readonly IWindowManagerService m_windowManager;
        private readonly ShellWindowCoordinator m_shellWindowCoordinator;
        private readonly CoreCursor m_systemArrowCursor;
        private readonly CoreCursor m_resizeNwSeCursor;
        private readonly CoreCursor m_resizeNeSwCursor;
        private CoreCursor m_visibleSystemCursor;
        private Controls.WindowResizeCorner? m_hoveredResizeCorner;
        private readonly bool m_useXboxDesktopScale;
        private bool m_isNativeMouseCursorHidden;
        private int m_popupCursorRequests;
        private bool m_hasOpenXamlPopup;
        private bool m_isWindowActive = true;
        private bool m_hasMouseInput;
        private bool m_customCursorEnabled = true;
        private bool m_altTabHeld;
        private bool m_desktopInitialized;
        private readonly DispatcherTimer m_popupCursorTimer;
        private readonly DispatcherTimer m_uiResponsivenessTimer;
        private readonly Stopwatch m_uiResponsivenessClock = Stopwatch.StartNew();
        private static readonly SemaphoreSlim s_uiStutterLogGate = new SemaphoreSlim(1, 1);
        private readonly string m_uiStutterLogPath;
        private long m_lastUiResponsivenessTick;
        private long m_lastUiStutterLog;
        private int m_lastGen0Collections;
        private int m_lastGen1Collections;
        private int m_lastGen2Collections;
        private MouseInputChangeKind m_lastMouseInputKind;
        private long m_lastMouseInputAt;
        private double m_lastMouseX;
        private double m_lastMouseY;
        private string m_lastUiOperation = "none";
        private long m_lastUiOperationAt;
        private long m_lastUiOperationDuration;

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
            m_shellWindowCoordinator = App.ServiceProvider.GetRequiredService<ShellWindowCoordinator>();
            // CoreCursor is a WinRT object. Recreating it whenever the pointer
            // crosses an internal window boundary can synchronously activate
            // cursor resources on Xbox and visibly stall the UI thread.
            m_systemArrowCursor = new CoreCursor(CoreCursorType.Arrow, 0);
            m_resizeNwSeCursor = new CoreCursor(CoreCursorType.SizeNorthwestSoutheast, 0);
            m_resizeNeSwCursor = new CoreCursor(CoreCursorType.SizeNortheastSouthwest, 0);
            ContentRoot.AddHandler(UIElement.PointerMovedEvent,
                new PointerEventHandler(ContentRoot_PointerMoved), true);
            if (Windows.Storage.ApplicationData.Current.LocalSettings.Values.TryGetValue(
                    CustomCursorEnabledSettingKey, out object customCursorEnabled) &&
                customCursorEnabled is bool enabled)
            {
                m_customCursorEnabled = enabled;
            }


            // Titlebar
            var coreTitleBar = CoreApplication.GetCurrentView().TitleBar;
            coreTitleBar.ExtendViewIntoTitleBar = true;
            ApplicationViewTitleBar titleBar = ApplicationView.GetForCurrentView().TitleBar;
            titleBar.ButtonBackgroundColor = Colors.Transparent;

            // Bind the frame now, but defer constructing the full desktop tree
            // until after the first activation. Constructing MainDesktop,
            // StartMenu and all internal windows inside the launch call can
            // exceed Xbox's activation watchdog (0x8027025A).
            m_desktopNavigator.FrameContext = DesktopFrame;

            Window.Current.CoreWindow.KeyDown += CoreWindow_KeyDown;
            Window.Current.CoreWindow.Dispatcher.AcceleratorKeyActivated += Dispatcher_AcceleratorKeyActivated;
            m_mouseInput.Attach(Window.Current.CoreWindow);
            m_mouseInput.InputChanged += MouseInput_InputChanged;

            Loaded += MainPage_Loaded;
            Window.Current.SizeChanged += Window_SizeChanged;
            Window.Current.Activated += Window_Activated;
            // GetOpenPopups walks the XAML popup tree. Doing that from
            // CompositionTarget.Rendering stalled the UI thread every frame,
            // even while the Shell was otherwise idle. The custom cursor only
            // needs this monitor after a mouse has actually been used.
            m_popupCursorTimer = new DispatcherTimer
            {
                Interval = TimeSpan.FromMilliseconds(125)
            };
            m_popupCursorTimer.Tick += PopupCursorTimer_Tick;

            // This timer does no work during normal ticks. If the UI dispatcher
            // could not run it near its deadline, record the delay on a worker
            // thread so the diagnostic cannot itself hitch the pointer.
            m_uiStutterLogPath = Path.Combine(
                Windows.Storage.ApplicationData.Current.LocalFolder.Path,
                "Shell-ui-stutter.log");
            m_lastUiResponsivenessTick = m_uiResponsivenessClock.ElapsedMilliseconds;
            m_lastGen0Collections = GC.CollectionCount(0);
            m_lastGen1Collections = GC.CollectionCount(1);
            m_lastGen2Collections = GC.CollectionCount(2);
            m_uiResponsivenessTimer = new DispatcherTimer
            {
                Interval = TimeSpan.FromMilliseconds(100)
            };
            m_uiResponsivenessTimer.Tick += UiResponsivenessTimer_Tick;
            m_uiResponsivenessTimer.Start();

        }

        private void UiResponsivenessTimer_Tick(object sender, object e)
        {
            long now = m_uiResponsivenessClock.ElapsedMilliseconds;
            long elapsed = now - m_lastUiResponsivenessTick;
            m_lastUiResponsivenessTick = now;
            int gen0 = GC.CollectionCount(0);
            int gen1 = GC.CollectionCount(1);
            int gen2 = GC.CollectionCount(2);
            int gen0Delta = gen0 - m_lastGen0Collections;
            int gen1Delta = gen1 - m_lastGen1Collections;
            int gen2Delta = gen2 - m_lastGen2Collections;
            m_lastGen0Collections = gen0;
            m_lastGen1Collections = gen1;
            m_lastGen2Collections = gen2;

            // Xbox commonly coalesces this 100 ms timer onto four 30 Hz frames
            // (about 133 ms); that is timer quantization, not a UI stall. Keep
            // visible hitches of roughly 50 ms or more without turning normal
            // frame cadence into continuous file I/O.
            if (elapsed < 150 || now - m_lastUiStutterLog < 250) return;
            m_lastUiStutterLog = now;

            long delay = elapsed - 100;
            AppState state = AppState.Instance;
            string activeWindow = m_shellWindowCoordinator.ActiveWindow?.Kind.ToString() ?? "none";
            string windows =
                $"Files={WindowState(state.IsFilesOpen, state.IsFilesMinimized)}," +
                $"Notepad={WindowState(state.IsNotepadOpen, state.IsNotepadMinimized)}," +
                $"Settings={WindowState(state.IsSettingsOpen, state.IsSettingsMinimized)}," +
                $"Calculator={WindowState(state.IsCalculatorOpen, state.IsCalculatorMinimized)}," +
                $"Strawfox={WindowState(state.IsFirefoxOpen, state.IsFirefoxMinimized)}," +
                $"WebApps={m_windowManager.Windows.Count}";
            string line = $"{DateTimeOffset.Now:O} UI dispatcher stall: delay={delay} ms; " +
                $"tickElapsed={elapsed} ms; thread={System.Environment.CurrentManagedThreadId}; " +
                $"GC=({gen0},{gen1},{gen2}); gcDelta=({gen0Delta},{gen1Delta},{gen2Delta}); " +
                $"active={activeWindow}; windows=[{windows}]; " +
                $"start={m_startManager.IsStartOpen}; actionCenter={m_actionManager?.IsActionCenterOpen == true}; " +
                $"xamlPopup={m_hasOpenXamlPopup}; popupCursorRequests={m_popupCursorRequests}; " +
                $"lastMouse={m_lastMouseInputKind}@({m_lastMouseX:0},{m_lastMouseY:0}) " +
                $"{Math.Max(0, now - m_lastMouseInputAt)} ms ago; " +
                $"lastOperation={m_lastUiOperation}({m_lastUiOperationDuration} ms) " +
                $"{Math.Max(0, now - m_lastUiOperationAt)} ms ago";
            Debug.WriteLine("[ShellStutter] " + line);
            _ = AppendUiStutterLogAsync(line);
        }

        private static string WindowState(bool isOpen, bool isMinimized) =>
            !isOpen ? "closed" : isMinimized ? "minimized" : "visible";

        internal static void RecordUiOperation(string operation, long elapsedMilliseconds)
        {
            CurrentPage?.RecordUiOperationCore(operation, elapsedMilliseconds);
        }

        private void RecordUiOperationCore(string operation, long elapsedMilliseconds)
        {
            m_lastUiOperation = operation ?? "unknown";
            m_lastUiOperationDuration = elapsedMilliseconds;
            m_lastUiOperationAt = m_uiResponsivenessClock.ElapsedMilliseconds;
            if (elapsedMilliseconds < 16) return;

            string line = $"{DateTimeOffset.Now:O} Slow UI operation: " +
                $"name={m_lastUiOperation}; elapsed={elapsedMilliseconds} ms";
            Debug.WriteLine("[ShellStutter] " + line);
            _ = AppendUiStutterLogAsync(line);
        }

        private async Task AppendUiStutterLogAsync(string line)
        {
            await Task.Run(async () =>
            {
                await s_uiStutterLogGate.WaitAsync().ConfigureAwait(false);
                try
                {
                    var file = new FileInfo(m_uiStutterLogPath);
                    if (file.Exists && file.Length > 1024 * 1024)
                        File.WriteAllText(m_uiStutterLogPath,
                            "Log rotated at " + DateTimeOffset.Now.ToString("O") + System.Environment.NewLine);
                    File.AppendAllText(m_uiStutterLogPath, line + System.Environment.NewLine, Encoding.UTF8);
                }
                catch
                {
                    // Diagnostics must never affect the Shell.
                }
                finally
                {
                    s_uiStutterLogGate.Release();
                }
            }).ConfigureAwait(false);
        }

        private void MainPage_Loaded(object sender, RoutedEventArgs e)
        {
            ApplyDesktopScale();
            if (!m_desktopInitialized)
            {
                m_desktopInitialized = true;
                _ = Dispatcher.RunAsync(
                    Windows.UI.Core.CoreDispatcherPriority.Low,
                    () => m_desktopNavigator.DesktopNavigate(DesktopPageType.RootContentDesktop));
            }
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
            bool firstMouseInput = !m_hasMouseInput;
            double scale = m_useXboxDesktopScale ? XboxDesktopScale : 1.0;
            m_lastMouseInputKind = e.Kind;
            m_lastMouseInputAt = m_uiResponsivenessClock.ElapsedMilliseconds;
            m_lastMouseX = e.Snapshot.X;
            m_lastMouseY = e.Snapshot.Y;

            // PointerPoint coordinates use the physical window space. ContentRoot uses
            // the larger virtual desktop space when the Xbox scale is enabled.
            // A render transform moves the cursor without invalidating Canvas
            // measure/arrange on every raw pointer packet.
            MouseCursorTransform.X = e.Snapshot.X / scale;
            MouseCursorTransform.Y = e.Snapshot.Y / scale;
            double resizeHalfSize = ResizeCursor.Width / 2;
            ResizeCursorTransform.X = e.Snapshot.X / scale - resizeHalfSize;
            ResizeCursorTransform.Y = e.Snapshot.Y / scale - resizeHalfSize;
            m_hasMouseInput = true;

            // Popup-tree discovery is a compatibility fallback for controls
            // which do not call SetPopupCursorVisibility themselves. Walking
            // that tree continuously caused a visible UI-thread hitch every
            // 125 ms. Check once after an actual interaction instead. Explicit
            // flyout/dialog hooks still update the cursor immediately.
            if (firstMouseInput || e.Kind == MouseInputChangeKind.Pressed ||
                e.Kind == MouseInputChangeKind.Released)
            {
                m_popupCursorTimer.Stop();
                m_popupCursorTimer.Start();
            }

            // Width/height are set when the desktop scale changes, and cursor
            // visibility changes only when state changes. Reassigning all three
            // on every PointerMoved needlessly invalidated XAML layout.
            if (firstMouseInput)
                UpdateCursorPresentation();
        }

        // Flyouts and ContentDialogs are rendered in the UWP popup layer, above the
        // custom cursor canvas. Use the system arrow only for their lifetime.
        public static void SetPopupCursorVisibility(bool isPopupOpen)
        {
            CurrentPage?.SetPopupCursorVisibilityCore(isPopupOpen);
        }

        public static bool IsCustomCursorEnabled
        {
            get
            {
                return !Windows.Storage.ApplicationData.Current.LocalSettings.Values.TryGetValue(
                           CustomCursorEnabledSettingKey, out object value) ||
                       !(value is bool enabled) || enabled;
            }
        }

        public static void SetCustomCursorEnabled(bool enabled)
        {
            Windows.Storage.ApplicationData.Current.LocalSettings.Values[
                CustomCursorEnabledSettingKey] = enabled;
            if (CurrentPage != null)
            {
                CurrentPage.m_customCursorEnabled = enabled;
                CurrentPage.UpdateCursorPresentation();
            }
        }

        private void SetPopupCursorVisibilityCore(bool isPopupOpen)
        {
            m_popupCursorRequests = Math.Max(0, m_popupCursorRequests + (isPopupOpen ? 1 : -1));
            UpdateCursorPresentation();
        }

        private void PopupCursorTimer_Tick(object sender, object e)
        {
            // DispatcherTimer has no one-shot mode.
            m_popupCursorTimer.Stop();
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

        private void ContentRoot_PointerMoved(object sender, PointerRoutedEventArgs e)
        {
            Controls.WindowResizeCorner? corner = null;
            for (DependencyObject element = e.OriginalSource as DependencyObject;
                 element != null;
                 element = VisualTreeHelper.GetParent(element))
            {
                if (element is Border border &&
                    border.Tag is string tag &&
                    Enum.TryParse(tag, out Controls.WindowResizeCorner parsed) &&
                    Enum.IsDefined(typeof(Controls.WindowResizeCorner), parsed))
                {
                    corner = parsed;
                    break;
                }
                if (element == ContentRoot) break;
            }
            if (corner == m_hoveredResizeCorner) return;
            m_hoveredResizeCorner = corner;
            ResizeCursorRotation.Angle = corner == Controls.WindowResizeCorner.TopRight ||
                                         corner == Controls.WindowResizeCorner.BottomLeft ? 90 : 0;
            UpdateCursorPresentation();
        }

        private void UpdateCursorPresentation()
        {
            bool useSystemCursor = !m_customCursorEnabled || m_popupCursorRequests > 0 ||
                m_hasOpenXamlPopup || !m_isWindowActive;
            bool showResize = m_hoveredResizeCorner.HasValue && m_hasMouseInput &&
                !m_hasOpenXamlPopup && m_isWindowActive;
            Stopwatch cursorClock = Stopwatch.StartNew();
            bool cursorChanged = false;
            if (useSystemCursor)
            {
                MouseCursor.Visibility = Visibility.Collapsed;
                ResizeCursor.Visibility = Visibility.Collapsed;
                CoreCursor cursor = showResize
                    ? (m_hoveredResizeCorner == Controls.WindowResizeCorner.TopRight ||
                       m_hoveredResizeCorner == Controls.WindowResizeCorner.BottomLeft
                        ? m_resizeNeSwCursor : m_resizeNwSeCursor)
                    : m_systemArrowCursor;
                if (m_isNativeMouseCursorHidden || m_visibleSystemCursor != cursor)
                {
                    Window.Current.CoreWindow.PointerCursor = cursor;
                    m_isNativeMouseCursorHidden = false;
                    m_visibleSystemCursor = cursor;
                    cursorChanged = true;
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
                    m_visibleSystemCursor = null;
                    cursorChanged = true;
                }

                MouseCursor.Visibility = m_hasMouseInput && !showResize ? Visibility.Visible : Visibility.Collapsed;
                ResizeCursor.Visibility = showResize ? Visibility.Visible : Visibility.Collapsed;
            }

            cursorClock.Stop();
            if (cursorChanged && cursorClock.ElapsedMilliseconds >= 16)
            {
                string line = $"{DateTimeOffset.Now:O} Slow cursor transition: " +
                    $"target={(useSystemCursor ? "system" : "custom")}; " +
                    $"elapsed={cursorClock.ElapsedMilliseconds} ms; " +
                    $"popupCursorRequests={m_popupCursorRequests}; xamlPopup={m_hasOpenXamlPopup}";
                Debug.WriteLine("[ShellStutter] " + line);
                _ = AppendUiStutterLogAsync(line);
            }
        }

        private void UpdateMouseCursorSize(double scale)
        {
            double logicalCursorSize = MouseCursorPixelSize / scale;
            MouseCursor.Width = logicalCursorSize;
            MouseCursor.Height = logicalCursorSize;
            ResizeCursor.Width = MouseCursorPixelSize / scale;
            ResizeCursor.Height = MouseCursorPixelSize / scale;
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
