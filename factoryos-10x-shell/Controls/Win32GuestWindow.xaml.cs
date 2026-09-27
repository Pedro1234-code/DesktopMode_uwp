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

namespace factoryos_10x_shell.Controls
{
    public sealed partial class Win32GuestWindow : UserControl
    {
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
        private bool m_resizing;
        private bool m_started;
        private bool m_closed;
        private bool m_pointerInsideRuntime;
        private bool m_runtimeCursorSuppressionApplied;
        private DispatcherTimer m_logTimer;

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
            try
            {
                m_session = new RuntimeSession(RuntimeSurface);
                m_logTimer = new DispatcherTimer { Interval = TimeSpan.FromSeconds(1) };
                m_logTimer.Tick += LogTimer_Tick;
                m_logTimer.Start();
                UpdateRuntimeInput();
                bool prepared = await m_session.PrepareAsync(m_window.Executable, m_window.ModuleSourceFolder);
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
                m_window.Status = "Desktop app";
                int exitCode = await m_session.RunAsync();
                StopLogTimer();
                if (m_closed) return;
                if (exitCode == -1 && !string.IsNullOrWhiteSpace(m_session.LastError))
                {
                    StatusPanel.Visibility = Visibility.Visible;
                    RuntimeProgress.IsActive = false;
                    m_window.Status = m_session.LastError;
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
                m_window.Status = "Runtime error: " + exception.Message;
            }
        }

        private void Window_PropertyChanged(object sender, PropertyChangedEventArgs e)
        {
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
            bool shouldSuppress = m_pointerInsideRuntime && !m_closed && m_window != null &&
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
            m_session?.SetInputEnabled(m_window != null && m_window.IsActive &&
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
            m_window.Width = Math.Max(480, m_startWidth + position.X - m_resizeStart.X);
            m_window.Height = Math.Max(320, m_startHeight + position.Y - m_resizeStart.Y);
        }

        private void ResizeGrip_PointerReleased(object sender, PointerRoutedEventArgs e) { m_resizing = false; ResizeGrip.ReleasePointerCaptures(); }
        private void ResizeGrip_PointerCaptureLost(object sender, PointerRoutedEventArgs e) => m_resizing = false;

        private void MouseInput_InputChanged(object sender, MouseInputChangedEventArgs e)
        {
            if (m_window == null || m_window.Visibility != Visibility.Visible) return;
            double scale = string.Equals(AnalyticsInfo.VersionInfo.DeviceFamily, "Windows.Xbox", StringComparison.OrdinalIgnoreCase) ? 0.75 : 1.0;
            if (e.Kind == MouseInputChangeKind.Released) { m_dragging = false; m_resizing = false; }
            if (m_dragging) MoveWindow(new Point(e.Snapshot.X / scale - m_window.Left, e.Snapshot.Y / scale - m_window.Top));
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
            m_session?.Close();
            m_windowManager.Close(m_window);
        }

        private void Win32GuestWindow_Unloaded(object sender, RoutedEventArgs e)
        {
            m_closed = true;
            ReleaseRuntimeCursorSuppression();
            StopLogTimer();
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
