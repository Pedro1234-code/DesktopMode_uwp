using DesktopMode.Gecko;
using factoryos_10x_shell.Library.Services.Input;
using factoryos_10x_shell.Services.Helpers;
using Microsoft.Extensions.DependencyInjection;
using System;
using Windows.Foundation;
using Windows.UI.Core;
using Windows.UI.Xaml;
using Windows.UI.Xaml.Controls;
using Windows.UI.Xaml.Input;
using Windows.UI.Xaml.Media;

namespace factoryos_10x_shell.Controls
{
    public sealed partial class FirefoxWindow : UserControl
    {
        public event Action<bool> FullscreenChanged;

        private static FirefoxWindow s_current;
        private GeckoHost m_host;
        private bool m_hostAttached;
        private bool m_hostStarting;
        private bool m_hostFailed;
        private bool m_resizing;
        private bool m_dragging;
        private bool m_maximized;
        private bool m_fullscreen;
        private bool m_wasMaximizedBeforeFullscreen;
        private bool m_isWindowActive;
        private bool m_inputSuppressed;
        private WindowResizeCorner m_resizeCorner;
        private Point m_startPoint;
        private double m_startWidth;
        private double m_startHeight;
        private double m_restoreLeft;
        private double m_restoreTop;
        private double m_restoreWidth;
        private double m_restoreHeight;
        private Point m_dragStartPointer;
        private double m_dragStartLeft;
        private double m_dragStartTop;
        private Thickness m_restoreHostMargin;
        private int m_restoreHostZIndex;
        private readonly IMouseInputService m_mouseInput;
        private readonly DispatcherTimer m_windowCommandTimer;

        public FirefoxWindow()
        {
            InitializeComponent();
            s_current = this;
            m_mouseInput = App.ServiceProvider.GetRequiredService<IMouseInputService>();
            m_mouseInput.InputChanged += MouseInput_InputChanged;
            m_windowCommandTimer = new DispatcherTimer { Interval = TimeSpan.FromMilliseconds(16) };
            m_windowCommandTimer.Tick += WindowCommandTimer_Tick;
            m_windowCommandTimer.Start();
        }

        public static bool SuspendRuntime()
        {
            if (s_current?.m_host?.IsStarted != true) return false;
            try
            {
                s_current.Suspend();
                return true;
            }
            catch
            {
                return false;
            }
        }

        public static void ResumeRuntime()
        {
            try { s_current?.Resume(); } catch { }
        }

        public void Open()
        {
            Visibility = Visibility.Visible;
            EnsureHost();
            AppState.Instance.SetFirefoxWindowState(true, false);
            AppState.Instance.ActivateFirefox();
        }

        public void CloseFromTaskView() => Close();

        public bool IsFullscreen => m_fullscreen;

        public void SetWindowActive(bool active)
        {
            m_isWindowActive = active;
            UpdateHostInput();
        }

        public void SetShellInputSuppressed(bool suppressed)
        {
            m_inputSuppressed = suppressed;
            UpdateHostInput();
        }

        public void Suspend() => m_host?.Suspend();

        public void Resume() => m_host?.Resume();

        private async void EnsureHost()
        {
            if (m_hostFailed || m_hostStarting) return;

            m_hostStarting = true;

            try
            {
                if (!m_hostAttached)
                {
                    m_host = new GeckoHost();
                    if (!(m_host.Content is UIElement content))
                    {
                        throw new InvalidOperationException("The Gecko host did not provide a XAML UIElement surface.");
                    }

                    BrowserSurface.Children.Add(content);
                    m_hostAttached = true;
                }

                await Dispatcher.RunAsync(CoreDispatcherPriority.Normal, () =>
                {
                    BrowserSurface.UpdateLayout();
                    if (!m_host.IsStarted && BrowserSurface.ActualWidth > 0 && BrowserSurface.ActualHeight > 0)
                        m_host.Start(BrowserSurface.ActualWidth, BrowserSurface.ActualHeight);
                    else
                        m_host.SetViewport(BrowserSurface.ActualWidth, BrowserSurface.ActualHeight);
                    UpdateGeckoScreen();
                    UpdateHostInput();
                });

                // MainDesktop is created after the Shell's first activation on
                // Xbox. The first layout pass can therefore expose an
                // intermediate window size and never raise another useful
                // SizeChanged event. Re-read the settled size on the next
                // low-priority dispatcher pass so Gecko and ANGLE allocate the
                // framebuffer at the final physical resolution.
                await Dispatcher.RunAsync(CoreDispatcherPriority.Low, () =>
                {
                    if (m_host?.IsStarted != true) return;
                    BrowserSurface.UpdateLayout();
                    if (BrowserSurface.ActualWidth <= 0 || BrowserSurface.ActualHeight <= 0) return;
                    m_host.SetViewport(BrowserSurface.ActualWidth, BrowserSurface.ActualHeight);
                    UpdateGeckoScreen();
                });
            }
            catch (Exception exception)
            {
                ShowHostFailure(exception);
            }
            finally
            {
                m_hostStarting = false;
            }
        }

        private void ShowHostFailure(Exception exception)
        {
            m_hostFailed = true;
            try { m_host?.SetActive(false); } catch { }
            BrowserSurface.Children.Clear();
            BrowserSurface.Children.Add(new StackPanel
            {
                Margin = new Thickness(32),
                Spacing = 12,
                Children =
                {
                    new TextBlock { Text = "Firefox could not be started", FontSize = 24 },
                    new TextBlock { Text = exception.ToString(), TextWrapping = TextWrapping.Wrap, IsTextSelectionEnabled = true }
                }
            });
        }

        private void UpdateHostInput()
        {
            m_host?.SetActive(
                m_isWindowActive && !m_inputSuppressed && Visibility == Visibility.Visible);
        }

        private void Close()
        {
            if (m_fullscreen) SetFullscreen(false);
            SetWindowActive(false);
            Visibility = Visibility.Collapsed;
            AppState.Instance.SetFirefoxWindowState(false, false);
        }

        private void Minimize()
        {
            if (m_fullscreen) SetFullscreen(false);
            SetWindowActive(false);
            Visibility = Visibility.Collapsed;
            AppState.Instance.SetFirefoxWindowState(true, true);
        }

        private void Root_PointerPressed(object sender, PointerRoutedEventArgs e) =>
            AppState.Instance.ActivateFirefox();

        private void BrowserSurface_SizeChanged(object sender, SizeChangedEventArgs e)
        {
            if (m_host == null || !m_host.IsStarted)
            {
                EnsureHost();
                return;
            }

            try
            {
                m_host.SetViewport(e.NewSize.Width, e.NewSize.Height);
                UpdateGeckoScreen();
            }
            catch (Exception exception)
            {
                ShowHostFailure(exception);
            }
        }

        private void SetMaximized(bool maximized)
        {
            if (m_maximized == maximized) return;

            Canvas host = VisualTreeHelper.GetParent(this) as Canvas;
            if (host == null) return;

            if (maximized)
            {
                m_restoreLeft = Canvas.GetLeft(this);
                m_restoreTop = Canvas.GetTop(this);
                m_restoreWidth = Width;
                m_restoreHeight = Height;
                Canvas.SetLeft(this, 0);
                Canvas.SetTop(this, 0);
                Width = host.ActualWidth;
                Height = host.ActualHeight;
            }
            else
            {
                Canvas.SetLeft(this, m_restoreLeft);
                Canvas.SetTop(this, m_restoreTop);
                Width = m_restoreWidth;
                Height = m_restoreHeight;
            }

            m_maximized = maximized;
        }

        private void UpdateGeckoScreen()
        {
            if (m_host?.IsStarted != true) return;
            // Gecko runs inside a virtual Shell window, not directly on the
            // physical desktop. Its screen rect must match the framebuffer it
            // owns. Passing the whole DesktopMode canvas here made chrome
            // popups (notably the hamburger menu) constrain themselves to a
            // wider screen than the browser window and appear displaced.
            if (BrowserSurface.ActualWidth > 0 && BrowserSurface.ActualHeight > 0)
                m_host.SetScreen(BrowserSurface.ActualWidth, BrowserSurface.ActualHeight);
        }

        private Point CurrentPointerInHost(Canvas host)
        {
            Point rootPoint = new Point(m_mouseInput.Current.X, m_mouseInput.Current.Y);
            GeneralTransform toRoot = host.TransformToVisual(null);
            Point origin = toRoot.TransformPoint(new Point(0, 0));
            Point unit = toRoot.TransformPoint(new Point(1, 1));
            double scaleX = unit.X - origin.X;
            double scaleY = unit.Y - origin.Y;
            if (Math.Abs(scaleX) < 0.001 || Math.Abs(scaleY) < 0.001) return rootPoint;
            return new Point((rootPoint.X - origin.X) / scaleX, (rootPoint.Y - origin.Y) / scaleY);
        }

        private void BeginWindowDrag()
        {
            if (m_maximized || m_fullscreen || !m_mouseInput.Current.IsLeftButtonPressed) return;
            Canvas host = VisualTreeHelper.GetParent(this) as Canvas;
            if (host == null) return;
            m_dragging = true;
            m_dragStartPointer = CurrentPointerInHost(host);
            m_dragStartLeft = Canvas.GetLeft(this);
            m_dragStartTop = Canvas.GetTop(this);
        }

        private void EndWindowDrag() => m_dragging = false;

        private void MouseInput_InputChanged(object sender, MouseInputChangedEventArgs e)
        {
            if (e.Kind == MouseInputChangeKind.Released)
            {
                EndWindowDrag();
                return;
            }
            if (!m_dragging || e.Kind != MouseInputChangeKind.Moved) return;
            Canvas host = VisualTreeHelper.GetParent(this) as Canvas;
            if (host == null) return;
            Point point = CurrentPointerInHost(host);
            Canvas.SetLeft(this, Math.Max(0, m_dragStartLeft + point.X - m_dragStartPointer.X));
            Canvas.SetTop(this, Math.Max(0, m_dragStartTop + point.Y - m_dragStartPointer.Y));
        }

        private void SetFullscreen(bool fullscreen)
        {
            if (m_fullscreen == fullscreen) return;
            Canvas host = VisualTreeHelper.GetParent(this) as Canvas;
            if (host == null) return;

            if (fullscreen)
            {
                m_wasMaximizedBeforeFullscreen = m_maximized;
                m_restoreHostMargin = host.Margin;
                m_restoreHostZIndex = Canvas.GetZIndex(host);
                host.Margin = new Thickness(0);
                Canvas.SetZIndex(host, 40);
                host.UpdateLayout();
                SetMaximized(true);
                Width = host.ActualWidth;
                Height = host.ActualHeight;
                WindowBorder.BorderThickness = new Thickness(0);
                WindowBorder.CornerRadius = new CornerRadius(0);
                ResizeGripTopLeft.Visibility = Visibility.Collapsed;
                ResizeGripTopRight.Visibility = Visibility.Collapsed;
                ResizeGripBottomLeft.Visibility = Visibility.Collapsed;
                ResizeGrip.Visibility = Visibility.Collapsed;
            }
            else
            {
                host.Margin = m_restoreHostMargin;
                Canvas.SetZIndex(host, m_restoreHostZIndex);
                host.UpdateLayout();
                if (m_wasMaximizedBeforeFullscreen)
                {
                    Width = host.ActualWidth;
                    Height = host.ActualHeight;
                }
                else
                {
                    SetMaximized(false);
                }
                WindowBorder.BorderThickness = new Thickness(1);
                WindowBorder.CornerRadius = new CornerRadius(4);
                ResizeGripTopLeft.Visibility = Visibility.Visible;
                ResizeGripTopRight.Visibility = Visibility.Visible;
                ResizeGripBottomLeft.Visibility = Visibility.Visible;
                ResizeGrip.Visibility = Visibility.Visible;
            }

            m_fullscreen = fullscreen;
            FullscreenChanged?.Invoke(fullscreen);
            UpdateGeckoScreen();
        }

        private void WindowCommandTimer_Tick(object sender, object e)
        {
            if (m_host == null || !m_host.IsStarted) return;

            int command;
            try
            {
                command = m_host.TakeWindowCommand();
            }
            catch
            {
                return;
            }

            switch (command)
            {
                case 1: Minimize(); break;
                case 2: SetMaximized(true); break;
                case 3: SetMaximized(false); break;
                case 4: Close(); break;
                case 5: BeginWindowDrag(); break;
                case 6: EndWindowDrag(); break;
                case 7: SetFullscreen(true); break;
                case 8: SetFullscreen(false); break;
            }
        }

        private void ResizeGrip_PointerPressed(object sender, PointerRoutedEventArgs e)
        {
            if (m_maximized) return;
            m_resizing = true;
            m_resizeCorner = WindowResize.CornerFromTag((sender as FrameworkElement)?.Tag);
            m_startPoint = e.GetCurrentPoint(null).Position;
            m_dragStartLeft = Canvas.GetLeft(this);
            m_dragStartTop = Canvas.GetTop(this);
            m_startWidth = Width;
            m_startHeight = Height;
            (sender as UIElement)?.CapturePointer(e.Pointer);
        }

        private void ResizeGrip_PointerMoved(object sender, PointerRoutedEventArgs e)
        {
            if (!m_resizing) return;
            Rect bounds = WindowResize.Calculate(m_resizeCorner, m_startPoint,
                e.GetCurrentPoint(null).Position,
                new Rect(m_dragStartLeft, m_dragStartTop, m_startWidth, m_startHeight), 640, 480);
            Canvas.SetLeft(this, bounds.X); Canvas.SetTop(this, bounds.Y);
            Width = bounds.Width; Height = bounds.Height;
        }

        private void ResizeGrip_PointerReleased(object sender, PointerRoutedEventArgs e)
        {
            m_resizing = false;
            (sender as UIElement)?.ReleasePointerCaptures();
        }
    }
}
