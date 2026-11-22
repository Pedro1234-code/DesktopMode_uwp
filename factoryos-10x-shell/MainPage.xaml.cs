using Windows.ApplicationModel.Core;
using Windows.Devices.Input;
using Windows.UI.ViewManagement;
using Windows.UI;
using Windows.UI.Xaml.Controls;
using Windows.UI.Xaml.Input;
using factoryos_10x_shell.Controls;
using System;
using Windows.Foundation;
using Windows.Graphics.Display;
using Windows.UI.Core;
using Windows.UI.Xaml;
using Windows.UI.Input;
using Windows.UI.Input.Preview.Injection;
using Windows.UI.Xaml.Media;
using factoryos_10x_shell.Library.Services.Managers;
using Microsoft.Extensions.DependencyInjection;
using factoryos_10x_shell.Library.Services.Navigation;
using factoryos_10x_shell.Views;
using Windows.System;
using Windows.System.Profile;
using Windows.System.Threading;
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
        internal double DesktopScale => m_useXboxDesktopScale ? XboxDesktopScale : 1.0;
        private const double MouseCursorPixelSize = 16.0;
        private const int CursorAbsoluteGraceMilliseconds = 32;
        private const int CursorMoveConfirmationMilliseconds = 100;
        private const int CursorActivationMilliseconds = 200;
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
        private readonly CoreCursor m_resizeHorizontalCursor;
        private readonly CoreCursor m_resizeVerticalCursor;
        private CoreCursor m_appliedCursor;
        private MouseDevice m_cursorMouseDevice;
        private InputInjector m_cursorInjector;
        private InjectedInputMouseInfo[] m_cursorMovePacket;
        private bool m_customCursorActive;
        private bool m_customCursorPresented;
        private long m_cursorActivationDeadline;
        private bool m_customCursorReady;
        private bool m_customCursorBlocked;
        private bool m_cursorFramePending;
        private bool m_cursorPositionDirty;
        private bool m_cursorMoveAwaitingConfirmation;
        private long m_lastCursorAbsoluteMoveAt;
        private long m_cursorMoveDeadline;
        private Point m_cursorPosition;
        private Point m_cursorActivationOrigin;
        private Point m_cursorMoveOrigin;
        private double m_rawCursorDeltaX;
        private double m_rawCursorDeltaY;
        private Controls.WindowResizeCorner? m_hoveredResizeCorner;
        private DependencyObject m_lastResizePointerSource;
        private readonly bool m_useXboxDesktopScale;
        private int m_popupCursorRequests;
        private bool m_hasOpenXamlPopup;
        private bool m_isWindowActive = true;
        private bool m_customCursorEnabled = false;
        private bool m_altTabHeld;
        private bool m_desktopInitialized;
        private readonly DispatcherTimer m_popupCursorTimer;
        private readonly DispatcherTimer m_uiResponsivenessTimer;
        private readonly Stopwatch m_uiResponsivenessClock = Stopwatch.StartNew();
        private static readonly SemaphoreSlim s_uiStutterLogGate = new SemaphoreSlim(1, 1);
        private readonly string m_uiStutterLogPath;
        private long m_lastUiResponsivenessTick;
        private ThreadPoolTimer m_uiHangTimer;
        private long m_uiHangMonitoringStartedAt;
        private long m_lastReportedUiHangTick = -1;
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
            m_resizeHorizontalCursor = new CoreCursor(CoreCursorType.SizeWestEast, 0);
            m_resizeVerticalCursor = new CoreCursor(CoreCursorType.SizeNorthSouth, 0);
            ContentRoot.AddHandler(UIElement.PointerMovedEvent,
                new PointerEventHandler(ContentRoot_PointerMoved), true);
            // Temporarily disable the custom cursor for existing installations too.
            Windows.Storage.ApplicationData.Current.LocalSettings.Values[CustomCursorEnabledSettingKey] = false;

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

            Loaded += MainPage_Loaded;
            Unloaded += MainPage_Unloaded;
            Window.Current.SizeChanged += Window_SizeChanged;
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
            long elapsed = now - Interlocked.Exchange(ref m_lastUiResponsivenessTick, now);
            int gen0 = GC.CollectionCount(0);
            int gen1 = GC.CollectionCount(1);
            int gen2 = GC.CollectionCount(2);
            int gen0Delta = gen0 - m_lastGen0Collections;
            int gen1Delta = gen1 - m_lastGen1Collections;
            int gen2Delta = gen2 - m_lastGen2Collections;
            m_lastGen0Collections = gen0;
            m_lastGen1Collections = gen1;
            m_lastGen2Collections = gen2;

            if (!ShellDiagnostics.IsLoggingEnabled) return;

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
            factoryos_10x_shell.Services.Helpers.ShellDiagnostics.WriteDebug("[ShellStutter] " + line);
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
            if (!ShellDiagnostics.IsLoggingEnabled) return;
            Volatile.Write(ref m_lastUiOperation, operation ?? "unknown");
            m_lastUiOperationDuration = elapsedMilliseconds;
            m_lastUiOperationAt = m_uiResponsivenessClock.ElapsedMilliseconds;
            if (elapsedMilliseconds < 16) return;

            string line = $"{DateTimeOffset.Now:O} Slow UI operation: " +
                $"name={m_lastUiOperation}; elapsed={elapsedMilliseconds} ms";
            factoryos_10x_shell.Services.Helpers.ShellDiagnostics.WriteDebug("[ShellStutter] " + line);
            _ = AppendUiStutterLogAsync(line);
        }

        private async Task AppendUiStutterLogAsync(string line)
        {
            if (!ShellDiagnostics.IsLoggingEnabled) return;
            await Task.Run(async () =>
            {
                await s_uiStutterLogGate.WaitAsync().ConfigureAwait(false);
                try
                {
                    if (!ShellDiagnostics.IsLoggingEnabled) return;
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
            CurrentPage = this;
            try
            {
                m_cursorMouseDevice = MouseDevice.GetForCurrentView();
                if (m_cursorMouseDevice != null) m_cursorMouseDevice.MouseMoved += CursorMouse_Moved;
            }
            catch (Exception)
            {
                // Without the relative-motion event, keep the system cursor.
                m_cursorMouseDevice = null;
            }
            m_mouseInput.InputChanged += MouseInput_InputChanged;
            Window.Current.Activated += Window_Activated;
            Window.Current.VisibilityChanged += UiHangWindow_VisibilityChanged;
            UpdateUiHangMonitoring(Window.Current.Visible);
            ApplyDesktopScale();
            if (!m_desktopInitialized)
            {
                m_desktopInitialized = true;
                _ = Dispatcher.RunAsync(
                    Windows.UI.Core.CoreDispatcherPriority.Low,
                    () => m_desktopNavigator.DesktopNavigate(DesktopPageType.RootContentDesktop));
            }
        }

        private void MainPage_Unloaded(object sender, RoutedEventArgs e)
        {
            m_mouseInput.InputChanged -= MouseInput_InputChanged;
            if (m_cursorMouseDevice != null) m_cursorMouseDevice.MouseMoved -= CursorMouse_Moved;
            m_cursorMouseDevice = null;
            m_cursorInjector = null;
            m_cursorMovePacket = null;
            ResetCursorFrames();
            m_customCursorActive = false;
            m_customCursorPresented = false;
            m_customCursorReady = false;
            MouseCursor.Visibility = Visibility.Collapsed;
            Window.Current.Activated -= Window_Activated;
            Window.Current.VisibilityChanged -= UiHangWindow_VisibilityChanged;
            m_popupCursorTimer.Stop();
            if (ReferenceEquals(CurrentPage, this))
            {
                Window.Current.CoreWindow.PointerCursor = m_systemArrowCursor;
                CurrentPage = null;
            }
            m_appliedCursor = null;
            UpdateUiHangMonitoring(false);
        }

        private void UiHangWindow_VisibilityChanged(object sender, VisibilityChangedEventArgs e)
        {
            UpdateUiHangMonitoring(e.Visible);
        }

        private void UpdateUiHangMonitoring(bool visible)
        {
            if (!visible)
            {
                Interlocked.Exchange(ref m_uiHangTimer, null)?.Cancel();
                return;
            }

            if (m_uiHangTimer != null) return;

            Interlocked.Exchange(ref m_uiHangMonitoringStartedAt, m_uiResponsivenessClock.ElapsedMilliseconds);
            Volatile.Write(ref m_uiHangTimer,
                ThreadPoolTimer.CreatePeriodicTimer(CheckUiHeartbeat, TimeSpan.FromSeconds(1)));
        }

        private void CheckUiHeartbeat(ThreadPoolTimer timer)
        {
            if (!ShellDiagnostics.IsLoggingEnabled) return;
            long lastTick = Interlocked.Read(ref m_lastUiResponsivenessTick);
            long elapsed = m_uiResponsivenessClock.ElapsedMilliseconds -
                           Math.Max(lastTick, Interlocked.Read(ref m_uiHangMonitoringStartedAt));
            if (elapsed < 3000 ||
                !ReferenceEquals(Volatile.Read(ref m_uiHangTimer), timer) ||
                Interlocked.Read(ref m_lastUiResponsivenessTick) != lastTick)
                return;

            // A blocked dispatcher cannot write its own log. Write only once per missed heartbeat.
            long lastReported = Interlocked.Read(ref m_lastReportedUiHangTick);
            if (lastTick > lastReported &&
                Interlocked.CompareExchange(ref m_lastReportedUiHangTick, lastTick, lastReported) == lastReported)
            {
                ShellDiagnostics.RecordUiHang(elapsed, Volatile.Read(ref m_lastUiOperation));
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
            MouseCursor.Width = MouseCursor.Height = MouseCursorPixelSize / scale;
            if (m_customCursorPresented) PositionCustomCursor(m_cursorPosition.X, m_cursorPosition.Y);

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
            bool positionChanged = m_lastMouseX != e.Snapshot.X || m_lastMouseY != e.Snapshot.Y;
            m_lastMouseInputKind = e.Kind;
            m_lastMouseInputAt = m_uiResponsivenessClock.ElapsedMilliseconds;
            m_lastMouseX = e.Snapshot.X;
            m_lastMouseY = e.Snapshot.Y;

            if (m_customCursorActive && !m_cursorMoveAwaitingConfirmation &&
                (m_cursorPosition.X != m_lastMouseX || m_cursorPosition.Y != m_lastMouseY))
            {
                m_lastCursorAbsoluteMoveAt = m_lastMouseInputAt;
                m_rawCursorDeltaX = 0;
                m_rawCursorDeltaY = 0;
                m_cursorPosition = new Point(m_lastMouseX, m_lastMouseY);
                m_cursorPositionDirty = true;
                QueueCursorFrame();
            }

            if (e.Kind == MouseInputChangeKind.Moved && positionChanged &&
                m_customCursorEnabled && !m_customCursorReady && m_cursorMouseDevice != null)
            {
                m_customCursorReady = true;
                _ = Dispatcher.RunAsync(CoreDispatcherPriority.Normal, () =>
                {
                    if (ReferenceEquals(CurrentPage, this)) UpdateCursorPresentation();
                });
            }

            // Popup-tree discovery is a compatibility fallback for controls
            // which do not call SetPopupCursorVisibility themselves. Walking
            // that tree continuously caused a visible UI-thread hitch every
            // 125 ms. Check once after an actual interaction instead. Explicit
            // flyout/dialog hooks still update the cursor immediately.
            if (e.Kind == MouseInputChangeKind.Pressed || e.Kind == MouseInputChangeKind.Released)
            {
                m_popupCursorTimer.Stop();
                m_popupCursorTimer.Start();
            }

        }

        // Flyouts and ContentDialogs are rendered in the UWP popup layer, above the
        // custom cursor canvas. Use the system arrow only for their lifetime.
        public static void SetPopupCursorVisibility(bool isPopupOpen)
        {
            CurrentPage?.SetPopupCursorVisibilityCore(isPopupOpen);
        }

        public static bool IsCustomCursorEnabled => false;

        public static void SetCustomCursorEnabled(bool enabled)
        {
            Windows.Storage.ApplicationData.Current.LocalSettings.Values[
                CustomCursorEnabledSettingKey] = false;
            if (CurrentPage != null)
            {
                CurrentPage.m_customCursorEnabled = false;
                CurrentPage.m_customCursorReady = false;
                CurrentPage.m_customCursorBlocked = false;
                CurrentPage.UpdateCursorPresentation();
            }
        }

        private void SetPopupCursorVisibilityCore(bool isPopupOpen)
        {
            m_popupCursorRequests = Math.Max(0, m_popupCursorRequests + (isPopupOpen ? 1 : -1));
            // Browser CSS cursors share CoreWindow and can replace our cached cursor.
            m_appliedCursor = null;
            UpdateCursorPresentation();
        }

        private void PopupCursorTimer_Tick(object sender, object e)
        {
            bool hasOpenPopup = VisualTreeHelper.GetOpenPopups(Window.Current).Count > 0;
            // Keep checking only while an untracked popup is open. It can also
            // close by keyboard or code, without another mouse click.
            if (!hasOpenPopup) m_popupCursorTimer.Stop();
            if (m_hasOpenXamlPopup == hasOpenPopup) return;

            m_hasOpenXamlPopup = hasOpenPopup;
            UpdateCursorPresentation();
        }

        private void Window_Activated(object sender, WindowActivatedEventArgs e)
        {
            bool isActive = e.WindowActivationState != CoreWindowActivationState.Deactivated;
            if (m_isWindowActive == isActive) return;

            m_isWindowActive = isActive;
            m_customCursorReady = false;
            m_appliedCursor = null;
            m_lastResizePointerSource = null;
            m_hoveredResizeCorner = null;
            if (isActive) PopupCursorTimer_Tick(null, null);
            UpdateCursorPresentation();
        }

        private void ContentRoot_PointerMoved(object sender, PointerRoutedEventArgs e)
        {
            if (m_customCursorActive && (m_cursorMoveAwaitingConfirmation || !m_customCursorPresented) &&
                e.Pointer.PointerDeviceType == PointerDeviceType.Mouse)
            {
                Point position = e.GetCurrentPoint(null).Position;
                Point origin = m_cursorMoveAwaitingConfirmation ? m_cursorMoveOrigin : m_cursorActivationOrigin;
                if (position.X != origin.X || position.Y != origin.Y)
                {
                    m_cursorMoveAwaitingConfirmation = false;
                    m_cursorPosition = position;
                    m_cursorPositionDirty = true;
                    if (!m_customCursorPresented)
                    {
                        PositionCustomCursor(position.X, position.Y);
                        m_customCursorPresented = true;
                        MouseCursor.Visibility = Visibility.Visible;
                    }
                    QueueCursorFrame();
                }
            }

            var source = e.OriginalSource as DependencyObject;
            if (source != null && ReferenceEquals(source, m_lastResizePointerSource)) return;
            m_lastResizePointerSource = source;

            Controls.WindowResizeCorner? corner = null;
            for (DependencyObject element = source;
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
            if (corner.HasValue && (m_hasOpenXamlPopup || m_popupCursorRequests > 0))
                m_hasOpenXamlPopup = VisualTreeHelper.GetOpenPopups(Window.Current).Count > 0;
            m_hoveredResizeCorner = corner;
            UpdateCursorPresentation();
        }

        private void UpdateCursorPresentation()
        {
            bool canUseShellCursor = m_isWindowActive && !m_hasOpenXamlPopup;
            CoreCursor cursor = m_systemArrowCursor;
            if (canUseShellCursor && m_hoveredResizeCorner.HasValue)
                cursor = GetSystemResizeCursor(m_hoveredResizeCorner.Value);

            bool showCustom = ReferenceEquals(cursor, m_systemArrowCursor) &&
                canUseShellCursor && m_popupCursorRequests == 0 && m_customCursorEnabled &&
                m_customCursorReady && !m_customCursorBlocked && m_cursorMouseDevice != null;
            var coreWindow = Window.Current.CoreWindow;

            if (showCustom)
            {
                if (m_customCursorActive) return;
                m_cursorPosition = new Point(m_lastMouseX, m_lastMouseY);
                m_cursorActivationOrigin = m_cursorPosition;
                m_lastCursorAbsoluteMoveAt = m_uiResponsivenessClock.ElapsedMilliseconds;
                m_cursorActivationDeadline = m_lastCursorAbsoluteMoveAt + CursorActivationMilliseconds;
                try { coreWindow.PointerCursor = null; }
                catch (Exception)
                {
                    m_customCursorBlocked = true;
                    coreWindow.PointerCursor = m_appliedCursor = m_systemArrowCursor;
                    return;
                }
                m_appliedCursor = null;
                m_customCursorActive = true;
                QueueCursorFrame();
                return;
            }

            if (m_customCursorActive || !ReferenceEquals(m_appliedCursor, cursor))
            {
                coreWindow.PointerCursor = cursor;
                m_appliedCursor = cursor;
            }

            if (!m_customCursorActive) return;
            m_customCursorActive = false;
            m_customCursorPresented = false;
            MouseCursor.Visibility = Visibility.Collapsed;
            ResetCursorFrames();
        }

        private void PositionCustomCursor(double x, double y)
        {
            double scale = m_useXboxDesktopScale ? XboxDesktopScale : 1.0;
            MouseCursorTransform.X = x / scale;
            MouseCursorTransform.Y = y / scale;
        }

        private void CursorMouse_Moved(MouseDevice sender, MouseEventArgs args)
        {
            if (!m_customCursorActive) return;

            // Ignore raw packets while normal absolute movement is arriving.
            // This prevents applying the same movement a second time.
            if (!m_cursorMoveAwaitingConfirmation &&
                m_uiResponsivenessClock.ElapsedMilliseconds - m_lastCursorAbsoluteMoveAt < CursorAbsoluteGraceMilliseconds)
                return;

            var delta = args.MouseDelta;
            if (delta.X == 0 && delta.Y == 0) return;
            m_rawCursorDeltaX += delta.X;
            m_rawCursorDeltaY += delta.Y;
            QueueCursorFrame();
        }

        private void QueueCursorFrame()
        {
            if (m_cursorFramePending) return;
            m_cursorFramePending = true;
            CompositionTarget.Rendering += RenderCursorFrame;
        }

        private void RenderCursorFrame(object sender, object args)
        {
            CompositionTarget.Rendering -= RenderCursorFrame;
            m_cursorFramePending = false;
            if (!m_customCursorActive) return;

            if (m_cursorPositionDirty && m_customCursorPresented)
            {
                m_cursorPositionDirty = false;
                PositionCustomCursor(m_cursorPosition.X, m_cursorPosition.Y);
            }

            long now = m_uiResponsivenessClock.ElapsedMilliseconds;
            if (m_cursorMoveAwaitingConfirmation)
            {
                if (now < m_cursorMoveDeadline)
                    QueueCursorFrame();
                else
                    RestoreSystemCursor();
                return;
            }

            if (m_rawCursorDeltaX == 0 && m_rawCursorDeltaY == 0)
            {
                if (!m_customCursorPresented)
                {
                    if (now < m_cursorActivationDeadline)
                        QueueCursorFrame();
                    else
                    {
                        m_customCursorReady = false;
                        UpdateCursorPresentation();
                    }
                }
                return;
            }
            double dx = m_rawCursorDeltaX;
            double dy = m_rawCursorDeltaY;
            m_rawCursorDeltaX = m_rawCursorDeltaY = 0;

            try
            {
                var coreWindow = Window.Current.CoreWindow;
                Rect bounds = coreWindow.Bounds;
                if (bounds.Width < 1 || bounds.Height < 1)
                {
                    RestoreSystemCursor();
                    return;
                }

                var display = DisplayInformation.GetForCurrentView();
                double pixelsPerDip = display.RawPixelsPerViewPixel;
                if (pixelsPerDip <= 0) pixelsPerDip = 1;
                var target = new Point(
                    Math.Max(0, Math.Min(bounds.Width - 1, m_cursorPosition.X + dx / pixelsPerDip)),
                    Math.Max(0, Math.Min(bounds.Height - 1, m_cursorPosition.Y + dy / pixelsPerDip)));
                if (target.X == m_cursorPosition.X && target.Y == m_cursorPosition.Y)
                {
                    if (!m_customCursorPresented) QueueCursorFrame();
                    return;
                }

                m_cursorMoveOrigin = m_cursorPosition;
                m_cursorMoveDeadline = now + CursorMoveConfirmationMilliseconds;
                m_cursorMoveAwaitingConfirmation = true;
                // Let the Xbox input stack apply its own mouse scaling and use
                // the resulting routed XAML position as the source of truth.
                if (m_useXboxDesktopScale)
                {
                    if (!TryInjectCursorMove(target.X != m_cursorPosition.X ? dx : 0,
                                             target.Y != m_cursorPosition.Y ? dy : 0))
                    {
                        RestoreSystemCursor();
                        return;
                    }
                }
                else
                {
                    coreWindow.PointerPosition = new Point(bounds.X + target.X, bounds.Y + target.Y);
                }

                // Wait for the routed move before showing the new position.
                QueueCursorFrame();
            }
            catch (Exception)
            {
                RestoreSystemCursor();
            }
        }

        private bool TryInjectCursorMove(double dx, double dy)
        {
            if (m_cursorInjector == null)
            {
                m_cursorInjector = InputInjector.TryCreate();
                if (m_cursorInjector == null) return false;

                m_cursorMovePacket = new[]
                {
                    new InjectedInputMouseInfo { MouseOptions = InjectedInputMouseOptions.Move }
                };
            }

            m_cursorMovePacket[0].DeltaX = (int)dx;
            m_cursorMovePacket[0].DeltaY = (int)dy;
            m_cursorInjector.InjectMouseInput(m_cursorMovePacket);
            return true;
        }

        private void RestoreSystemCursor()
        {
            m_customCursorBlocked = true;
            UpdateCursorPresentation();
        }

        private void ResetCursorFrames()
        {
            if (m_cursorFramePending) CompositionTarget.Rendering -= RenderCursorFrame;
            m_cursorFramePending = false;
            m_cursorPositionDirty = false;
            m_cursorMoveAwaitingConfirmation = false;
            m_rawCursorDeltaX = m_rawCursorDeltaY = 0;
        }

        private CoreCursor GetSystemResizeCursor(Controls.WindowResizeCorner corner)
        {
            switch (corner)
            {
                case Controls.WindowResizeCorner.Left:
                case Controls.WindowResizeCorner.Right:
                    return m_resizeHorizontalCursor;
                case Controls.WindowResizeCorner.Top:
                case Controls.WindowResizeCorner.Bottom:
                    return m_resizeVerticalCursor;
                case Controls.WindowResizeCorner.TopRight:
                case Controls.WindowResizeCorner.BottomLeft:
                    return m_resizeNeSwCursor;
                default:
                    return m_resizeNwSeCursor;
            }
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
