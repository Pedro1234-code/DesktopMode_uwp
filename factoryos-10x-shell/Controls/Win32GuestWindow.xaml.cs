using factoryos_10x_shell.Library.Services.Input;
using factoryos_10x_shell.Services.Win32;
using Microsoft.Extensions.DependencyInjection;
using System;
using System.ComponentModel;
using System.Threading.Tasks;
using Windows.Foundation;
using Windows.System.Profile;
using Windows.UI.ViewManagement;
using Windows.UI.Xaml;
using Windows.UI.Xaml.Controls;
using Windows.UI.Xaml.Input;
using Windows.UI.Xaml.Media;
using Win32Bridge;
using Newtonsoft.Json.Linq;

namespace factoryos_10x_shell.Controls
{
    public sealed partial class Win32GuestWindow : UserControl
    {
        private static readonly double s_pointerScale =
            string.Equals(AnalyticsInfo.VersionInfo.DeviceFamily, "Windows.Xbox", StringComparison.OrdinalIgnoreCase) ? 0.75 : 1.0;
        private readonly Win32WindowManagerService m_windowManager = Win32WindowManagerService.Instance;
        private IMouseInputService m_mouseInput;
        private Win32WindowModel m_window;
        private RuntimeSession m_session;
        private Point m_dragStart;
        private double m_startLeft;
        private double m_startTop;
        private bool m_dragging;
        private Point m_resizeStart;
        private double m_startWidth;
        private double m_startHeight;
        private WindowResizeCorner m_resizeCorner;
        private UIElement m_resizeCoordinateSpace;
        private bool m_resizing;
        private bool m_started;
        private bool m_closed;
        private bool m_pointerInsideRuntime;
        private bool m_runtimeCursorSuppressionApplied;
        private DispatcherTimer m_logTimer;
        private WebView m_terminal;
        private DispatcherTimer m_terminalTimer;
        private bool m_terminalReady, m_terminalBusy, m_terminalFinished;
        private string m_terminalPending = string.Empty;
        private readonly System.Collections.Generic.Queue<string> m_terminalInput = new System.Collections.Generic.Queue<string>();

        private void InitializeTerminal()
        {
            m_terminal = new WebView(WebViewExecutionMode.SeparateThread);
            m_terminal.ScriptNotify += Terminal_ScriptNotify;
            m_terminal.NavigationCompleted += Terminal_NavigationCompleted;
            RuntimeClientArea.Children.Insert(1, m_terminal);
            m_terminalTimer = new DispatcherTimer { Interval = TimeSpan.FromMilliseconds(33) };
            m_terminalTimer.Tick += Terminal_Tick;
            m_terminalTimer.Start();
            m_terminal.Navigate(new Uri("ms-appx-web:///Assets/Terminal/index.html"));
            UpdateRuntimeInput();
        }

        private void Terminal_NavigationCompleted(WebView sender, WebViewNavigationCompletedEventArgs args)
        {
            if (!args.IsSuccess && !m_closed)
            {
                StatusPanel.Visibility = Visibility.Visible;
                RuntimeProgress.IsActive = false;
                m_window.Status = "Terminal navigation failed: " + args.WebErrorStatus;
            }
        }

        private void Terminal_ScriptNotify(object sender, NotifyEventArgs args)
        {
            if (m_closed) return;
            try
            {
                var message = JObject.Parse(args.Value);
                if ((bool?)message["ready"] == true) m_terminalReady = true;
                if (message["input"] != null && !m_terminalFinished && m_window.IsActive && !m_windowManager.IsInputSuppressed)
                    m_terminalInput.Enqueue((string)message["input"]);
                int cols = (int?)message["cols"] ?? 0, rows = (int?)message["rows"] ?? 0;
                if (cols > 0 && rows > 0) m_session.SetConsoleSize((uint)cols, (uint)rows);
            }
            catch (Exception error)
            { factoryos_10x_shell.Services.Helpers.ShellDiagnostics.WriteDebug("Terminal message: " + error.Message); }
        }

        private async void Terminal_Tick(object sender, object args)
        { await PumpTerminalAsync(); }

        private async Task PumpTerminalAsync()
        {
            if (!m_terminalReady || m_terminalBusy || m_closed || m_terminal == null) return;
            m_terminalBusy = true;
            try
            {
                while (!m_terminalFinished && m_terminalInput.Count > 0 && m_session.FeedConsoleInput(m_terminalInput.Peek()))
                    m_terminalInput.Dequeue();
                m_terminalPending += m_session.DrainConsoleOutput();
                if (m_terminalPending.Length != 0)
                {
                    string batch = m_terminalPending;
                    await m_terminal.InvokeScriptAsync("bridgeOutput", new[] { batch });
                    m_terminalPending = m_terminalPending.Substring(batch.Length);
                }
                if (m_closed || m_terminal == null) return;
                await m_terminal.InvokeScriptAsync("bridgeMode", new[] { m_session.ConsoleInputMode().ToString() });
                if (m_closed || m_terminal == null) return;
                await m_terminal.InvokeScriptAsync("bridgeEnabled", new[] {
                    (!m_terminalFinished && m_window.IsActive && !m_windowManager.IsInputSuppressed) ? "true" : "false" });
            }
            catch (Exception error)
            { factoryos_10x_shell.Services.Helpers.ShellDiagnostics.WriteDebug("Terminal transport: " + error.Message); }
            finally { m_terminalBusy = false; }
        }

        private void StopTerminal()
        {
            if (m_terminalTimer != null) { m_terminalTimer.Stop(); m_terminalTimer.Tick -= Terminal_Tick; m_terminalTimer = null; }
            if (m_terminal != null)
            {
                m_terminal.ScriptNotify -= Terminal_ScriptNotify;
                m_terminal.NavigationCompleted -= Terminal_NavigationCompleted;
                m_terminal.Navigate(new Uri("about:blank"));
                RuntimeClientArea.Children.Remove(m_terminal); m_terminal = null;
            }
        }

        public Win32GuestWindow()
        {
            InitializeComponent();
            DataContextChanged += Win32GuestWindow_DataContextChanged;
            Loaded += Win32GuestWindow_Loaded;
            Unloaded += Win32GuestWindow_Unloaded;
            m_windowManager.InputStateChanged += WindowManager_InputStateChanged;
        }

        private void Win32GuestWindow_DataContextChanged(FrameworkElement sender, DataContextChangedEventArgs args)
        {
            if (m_window != null) m_window.PropertyChanged -= Window_PropertyChanged;
            m_window = args.NewValue as Win32WindowModel;
            UpdateTitleIconPlaceholder();
            if (m_window == null) return;
            Width = m_window.Width;
            Height = m_window.Height;
            m_window.PropertyChanged += Window_PropertyChanged;
        }

        private void Win32GuestWindow_Loaded(object sender, RoutedEventArgs e)
        {
            if (App.HasServiceProvider && m_mouseInput == null)
            {
                m_mouseInput = App.ServiceProvider.GetRequiredService<IMouseInputService>();
                m_mouseInput.InputChanged += MouseInput_InputChanged;
            }
            if (!m_started && m_window != null)
            {
                m_started = true;
                _ = StartGuestAsync();
            }
        }

        private async Task StartGuestAsync()
        {
            string stage = "initializing the runtime";
            try
            {
                m_session = new RuntimeSession(RuntimeSurface, ApplyInitialGuestSize);
                m_logTimer = new DispatcherTimer { Interval = TimeSpan.FromSeconds(1) };
                m_logTimer.Tick += LogTimer_Tick;
                m_logTimer.Start();
                UpdateRuntimeInput();
                stage = "preparing the executable";
                bool prepared;
                if (string.IsNullOrEmpty(m_window.VirtualExecutablePath))
                {
                    prepared = await m_session.PrepareAsync(m_window.Executable,
                        m_window.ModuleSourceFolder);
                }
                else
                {
                    prepared = await m_session.PrepareShortcutAsync(m_window.Executable,
                        m_window.ModuleSourceFolder, m_window.VirtualExecutablePath,
                        m_window.Arguments ?? string.Empty, m_window.WorkingDirectory ?? string.Empty);
                }
                if (m_closed)
                {
                    StopLogTimer();
                    m_session.Close();
                    return;
                }
                if (!prepared)
                {
                    RuntimeProgress.IsActive = false;
                    StopLogTimer();
                    m_window.Status = string.IsNullOrWhiteSpace(m_session.LastError)
                        ? "The runtime could not prepare this executable."
                        : m_session.LastError;
                    return;
                }

                StatusPanel.Visibility = Visibility.Collapsed;
                if (m_session.IsConsoleApplication) InitializeTerminal();
                try
                {
                    var icon = m_session.LoadExecutableIcon();
                    if (icon != null && !m_window.KeepLaunchIcon) m_window.IconSource = icon;
                }
                catch (Exception exception)
                {
                    factoryos_10x_shell.Services.Helpers.ShellDiagnostics.WriteDebug("Win32 icon unavailable: " + exception.Message);
                }
                m_window.Status = "Desktop app";
                stage = "running the executable";
                int exitCode = await m_session.RunAsync();
                StopLogTimer();
                if (m_closed) return;
                if (exitCode == -1 && !string.IsNullOrWhiteSpace(m_session.LastError))
                {
                    m_terminalFinished = true;
                    StatusPanel.Visibility = Visibility.Visible;
                    RuntimeProgress.IsActive = false;
                    m_window.Status = m_session.LastError;
                    return;
                }
                if (m_terminal != null)
                {
                    m_terminalPending += m_session.DrainConsoleOutput();
                    m_terminalPending += "\r\n[Process exited: " + exitCode + "]\r\n";
                    m_terminalFinished = true;
                    m_window.Status = "Process exited: " + exitCode;
                    await PumpTerminalAsync();
                    return;
                }
                m_windowManager.Close(m_window);
            }
            catch (Exception exception)
            {
                if (m_closed) return;
                StatusPanel.Visibility = Visibility.Visible;
                RuntimeProgress.IsActive = false;
                StopLogTimer();
                m_terminalFinished = true;
                m_window.Status = $"Runtime error while {stage}: {exception.Message} (0x{exception.HResult:X8})";
            }
        }

        private void ApplyInitialGuestSize(int width, int height)
        {
            if (m_closed || m_window == null) return;
            var border = WindowFrame.BorderThickness;
            double frameWidth = width + border.Left + border.Right;
            double frameHeight = height + TitleBar.ActualHeight + border.Top + border.Bottom;
            if (m_window.IsMaximized)
            {
                m_window.RestoreWidth = frameWidth;
                m_window.RestoreHeight = frameHeight;
                return;
            }
            if (m_resizing) return;
            m_window.Width = frameWidth;
            m_window.Height = frameHeight;
            UpdateLayout();
        }

        private void UpdateTitleIconPlaceholder()
        {
            TitleIconPlaceholder.Visibility = m_window?.IconSource == null ? Visibility.Visible : Visibility.Collapsed;
        }

        private void Window_PropertyChanged(object sender, PropertyChangedEventArgs e)
        {
            if (e.PropertyName == nameof(Win32WindowModel.IconSource)) UpdateTitleIconPlaceholder();
            if (e.PropertyName == nameof(Win32WindowModel.Width)) Width = m_window.Width;
            if (e.PropertyName == nameof(Win32WindowModel.Height)) Height = m_window.Height;
            if (e.PropertyName == nameof(Win32WindowModel.IsActive) || e.PropertyName == nameof(Win32WindowModel.Visibility))
            {
                UpdateRuntimeInput();
                UpdateRuntimeCursorSuppression();
            }
            FrameworkElement container = VisualTreeHelper.GetParent(this) as FrameworkElement;
            if (container == null) return;
            if (e.PropertyName == nameof(Win32WindowModel.ZIndex)) Canvas.SetZIndex(container, m_window.ZIndex);
            if (e.PropertyName == nameof(Win32WindowModel.Visibility)) container.Visibility = m_window.Visibility;
        }

        private void Root_PointerPressed(object sender, PointerRoutedEventArgs e) => m_windowManager.Activate(m_window);

        private void WindowManager_InputStateChanged(object sender, EventArgs e)
        {
            UpdateRuntimeInput();
            UpdateRuntimeCursorSuppression();
        }

        private void RuntimeClientArea_PointerEntered(object sender, PointerRoutedEventArgs e)
        {
            m_pointerInsideRuntime = true;
            UpdateRuntimeCursorSuppression();
        }

        private void RuntimeClientArea_PointerExited(object sender, PointerRoutedEventArgs e)
        {
            m_pointerInsideRuntime = false;
            UpdateRuntimeCursorSuppression();
        }

        private void UpdateRuntimeCursorSuppression()
        {
            bool shouldSuppress = m_terminal == null && m_pointerInsideRuntime && !m_closed && m_window != null &&
                m_window.Visibility == Visibility.Visible && !m_windowManager.IsInputSuppressed;
            if (shouldSuppress == m_runtimeCursorSuppressionApplied) return;

            m_runtimeCursorSuppressionApplied = shouldSuppress;
            MainPage.SetPopupCursorVisibility(shouldSuppress);
        }

        private void ReleaseRuntimeCursorSuppression()
        {
            m_pointerInsideRuntime = false;
            if (!m_runtimeCursorSuppressionApplied) return;

            m_runtimeCursorSuppressionApplied = false;
            MainPage.SetPopupCursorVisibility(false);
        }

        private void LogTimer_Tick(object sender, object e) => m_session?.FlushDiagnostics();

        private void StopLogTimer()
        {
            if (m_logTimer == null) return;
            m_logTimer.Stop();
            m_logTimer.Tick -= LogTimer_Tick;
            m_logTimer = null;
        }

        private void UpdateRuntimeInput()
        {
            m_session?.SetInputEnabled(m_terminal == null && m_window != null && m_window.IsActive &&
                m_window.Visibility == Visibility.Visible && !m_windowManager.IsInputSuppressed);
        }

        private void TitleBar_PointerPressed(object sender, PointerRoutedEventArgs e)
        {
            if (m_window == null || m_window.IsMaximized) return;
            m_dragging = true;
            Point position = e.GetCurrentPoint(this).Position;
            m_dragStart = new Point(m_window.Left + position.X, m_window.Top + position.Y);
            m_startLeft = m_window.Left;
            m_startTop = m_window.Top;
            TitleBar.CapturePointer(e.Pointer);
            m_windowManager.Activate(m_window);
        }

        private void TitleBar_PointerMoved(object sender, PointerRoutedEventArgs e) => MoveWindow(e.GetCurrentPoint(this).Position);
        private void TitleBar_PointerReleased(object sender, PointerRoutedEventArgs e) { m_dragging = false; TitleBar.ReleasePointerCaptures(); }
        private void TitleBar_PointerCaptureLost(object sender, PointerRoutedEventArgs e) => m_dragging = false;

        private void ResizeGrip_PointerPressed(object sender, PointerRoutedEventArgs e)
        {
            if (m_window == null || m_window.IsMaximized || !(sender is UIElement grip)) return;
            DependencyObject ancestor = VisualTreeHelper.GetParent(this);
            while (ancestor != null && !(ancestor is Canvas))
                ancestor = VisualTreeHelper.GetParent(ancestor);
            m_resizeCoordinateSpace = ancestor as UIElement;
            var point = e.GetCurrentPoint(m_resizeCoordinateSpace);
            if (!point.Properties.IsLeftButtonPressed || !grip.CapturePointer(e.Pointer)) return;
            m_windowManager.Activate(m_window);
            m_resizing = true;
            m_resizeCorner = WindowResize.CornerFromTag((sender as FrameworkElement)?.Tag);
            m_resizeStart = point.Position;
            m_startLeft = m_window.Left;
            m_startTop = m_window.Top;
            m_startWidth = m_window.Width;
            m_startHeight = m_window.Height;
            e.Handled = true;
        }

        private void ResizeGrip_PointerMoved(object sender, PointerRoutedEventArgs e)
        {
            if (!m_resizing) return;
            ResizeWindow(e.GetCurrentPoint(m_resizeCoordinateSpace).Position);
            e.Handled = true;
        }

        private void ResizeWindow(Point position)
        {
            Rect bounds = WindowResize.Calculate(m_resizeCorner, m_resizeStart,
                position,
                new Rect(m_startLeft, m_startTop, m_startWidth, m_startHeight), 480, 320);
            m_window.Left = bounds.X; m_window.Top = bounds.Y;
            m_window.Width = bounds.Width; m_window.Height = bounds.Height;
            UpdateCanvasPosition();
        }

        private void ResizeGrip_PointerReleased(object sender, PointerRoutedEventArgs e)
        {
            if (!m_resizing) return;
            m_resizing = false;
            (sender as UIElement)?.ReleasePointerCapture(e.Pointer);
            e.Handled = true;
        }
        private void ResizeGrip_PointerCaptureLost(object sender, PointerRoutedEventArgs e) => m_resizing = false;

        private void MouseInput_InputChanged(object sender, MouseInputChangedEventArgs e)
        {
            if (m_window == null || m_window.Visibility != Visibility.Visible) return;
            if (e.Kind == MouseInputChangeKind.Released) { m_dragging = false; }
            if (m_dragging) MoveWindow(new Point(e.Snapshot.X / s_pointerScale - m_window.Left, e.Snapshot.Y / s_pointerScale - m_window.Top));
        }

        private async void Minimize_Click(object sender, RoutedEventArgs e)
        {
            m_windowManager.Minimize(m_window);
            HideOnScreenKeyboard();
            await Task.Delay(100);
            HideOnScreenKeyboard();
        }

        private static void HideOnScreenKeyboard()
        {
            try { InputPane.GetForCurrentView().TryHide(); } catch { }
        }

        private void Maximize_Click(object sender, RoutedEventArgs e) { m_windowManager.ToggleMaximize(m_window); UpdateCanvasPosition(); }
        private void Close_Click(object sender, RoutedEventArgs e) => CloseWindow();

        public void CloseWindow()
        {
            if (m_closed) return;
            m_closed = true;
            ReleaseRuntimeCursorSuppression();
            StopLogTimer();
            StopTerminal();
            m_session?.Close();
            m_windowManager.Close(m_window);
        }

        private void Win32GuestWindow_Unloaded(object sender, RoutedEventArgs e)
        {
            m_closed = true;
            ReleaseRuntimeCursorSuppression();
            StopLogTimer();
            StopTerminal();
            if (m_mouseInput != null) m_mouseInput.InputChanged -= MouseInput_InputChanged;
            m_windowManager.InputStateChanged -= WindowManager_InputStateChanged;
            if (m_window != null) m_window.PropertyChanged -= Window_PropertyChanged;
            m_session?.Close();
        }

        private void UpdateCanvasPosition()
        {
            UIElement container = VisualTreeHelper.GetParent(this) as UIElement;
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
