using DesktopMode.Gecko;
using factoryos_10x_shell.Services.Helpers;
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
        private static FirefoxWindow s_current;
        private GeckoHost m_host;
        private bool m_hostAttached;
        private bool m_hostStarting;
        private bool m_hostFailed;
        private bool m_dragging;
        private bool m_resizing;
        private bool m_maximized;
        private bool m_isWindowActive;
        private bool m_inputSuppressed;
        private Point m_startPoint;
        private double m_startLeft;
        private double m_startTop;
        private double m_startWidth;
        private double m_startHeight;
        private double m_restoreLeft;
        private double m_restoreTop;
        private double m_restoreWidth;
        private double m_restoreHeight;

        public FirefoxWindow()
        {
            InitializeComponent();
            s_current = this;
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
                    BrowserSurface.Children.Add(m_host.Content);
                    m_hostAttached = true;
                }

                await Dispatcher.RunAsync(CoreDispatcherPriority.Normal, () =>
                {
                    BrowserSurface.UpdateLayout();
                    if (!m_host.IsStarted && BrowserSurface.ActualWidth > 0 && BrowserSurface.ActualHeight > 0)
                        m_host.Start(BrowserSurface.ActualWidth, BrowserSurface.ActualHeight);
                    else
                        m_host.SetViewport(BrowserSurface.ActualWidth, BrowserSurface.ActualHeight);
                    UpdateHostInput();
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
            SetWindowActive(false);
            Visibility = Visibility.Collapsed;
            AppState.Instance.SetFirefoxWindowState(false, false);
        }

        private void Close_Click(object sender, RoutedEventArgs e) => Close();

        private void Minimize_Click(object sender, RoutedEventArgs e)
        {
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
            }
            catch (Exception exception)
            {
                ShowHostFailure(exception);
            }
        }

        private void Maximize_Click(object sender, RoutedEventArgs e)
        {
            Canvas host = VisualTreeHelper.GetParent(this) as Canvas;
            if (host == null) return;

            if (!m_maximized)
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

            m_maximized = !m_maximized;
        }

        private void TitleBar_PointerPressed(object sender, PointerRoutedEventArgs e)
        {
            if (m_maximized) return;
            UIElement host = VisualTreeHelper.GetParent(this) as UIElement;
            m_dragging = true;
            m_startPoint = e.GetCurrentPoint(host).Position;
            m_startLeft = Canvas.GetLeft(this);
            m_startTop = Canvas.GetTop(this);
            TitleBar.CapturePointer(e.Pointer);
        }

        private void TitleBar_PointerMoved(object sender, PointerRoutedEventArgs e)
        {
            if (!m_dragging) return;
            UIElement host = VisualTreeHelper.GetParent(this) as UIElement;
            Point point = e.GetCurrentPoint(host).Position;
            Canvas.SetLeft(this, Math.Max(0, m_startLeft + point.X - m_startPoint.X));
            Canvas.SetTop(this, Math.Max(0, m_startTop + point.Y - m_startPoint.Y));
        }

        private void TitleBar_PointerReleased(object sender, PointerRoutedEventArgs e)
        {
            m_dragging = false;
            TitleBar.ReleasePointerCaptures();
        }

        private void ResizeGrip_PointerPressed(object sender, PointerRoutedEventArgs e)
        {
            if (m_maximized) return;
            m_resizing = true;
            m_startPoint = e.GetCurrentPoint(this).Position;
            m_startWidth = Width;
            m_startHeight = Height;
            ResizeGrip.CapturePointer(e.Pointer);
        }

        private void ResizeGrip_PointerMoved(object sender, PointerRoutedEventArgs e)
        {
            if (!m_resizing) return;
            Point point = e.GetCurrentPoint(this).Position;
            Width = Math.Max(640, m_startWidth + point.X - m_startPoint.X);
            Height = Math.Max(480, m_startHeight + point.Y - m_startPoint.Y);
        }

        private void ResizeGrip_PointerReleased(object sender, PointerRoutedEventArgs e)
        {
            m_resizing = false;
            ResizeGrip.ReleasePointerCaptures();
        }
    }
}
