using System;
using Windows.Foundation;
using Windows.UI.Xaml;
using Windows.UI.Xaml.Controls;
using Windows.UI.Xaml.Input;
using Windows.UI.Xaml.Media;
using Windows.UI.Xaml.Navigation;
using factoryos_10x_shell.Services.Helpers;

namespace factoryos_10x_shell.Controls
{
    public sealed partial class CalculatorWindow : UserControl
    {
        private bool m_dragging;
        private bool m_resizing;
        private bool m_maximized;
        private Point m_startPoint;
        private double m_startLeft;
        private double m_startTop;
        private double m_startWidth;
        private double m_startHeight;
        private double m_restoreLeft;
        private double m_restoreTop;
        private double m_restoreWidth;
        private double m_restoreHeight;
        private bool m_calculatorLoaded;

        public CalculatorWindow()
        {
            InitializeComponent();
            CalculatorFrame.NavigationFailed += CalculatorFrame_NavigationFailed;
        }

        public void Open()
        {
            if (!m_calculatorLoaded)
            {
                m_calculatorLoaded = CalculatorFrame.Navigate(typeof(CalculatorApp.MainPage));
            }
            Visibility = Visibility.Visible;
            AppState.Instance.SetCalculatorWindowState(true, false);
            AppState.Instance.ActivateCalculator();
        }

        public void CloseFromTaskView() => Close();

        private void Close()
        {
            Visibility = Visibility.Collapsed;
            AppState.Instance.SetCalculatorWindowState(false, false);
        }

        private void Close_Click(object sender, RoutedEventArgs e) => Close();

        private void Minimize_Click(object sender, RoutedEventArgs e)
        {
            Visibility = Visibility.Collapsed;
            AppState.Instance.SetCalculatorWindowState(true, true);
        }

        private void Root_PointerPressed(object sender, PointerRoutedEventArgs e) => AppState.Instance.ActivateCalculator();

        private void CalculatorFrame_NavigationFailed(object sender, NavigationFailedEventArgs e)
        {
            e.Handled = true;
            m_calculatorLoaded = false;

            var errorPanel = new StackPanel
            {
                Margin = new Thickness(32),
                Spacing = 12
            };
            errorPanel.Children.Add(new TextBlock
            {
                Text = "Calculator could not be loaded",
                FontSize = 24,
                FontWeight = Windows.UI.Text.FontWeights.SemiBold
            });
            errorPanel.Children.Add(new TextBlock
            {
                Text = e.Exception?.ToString() ?? "Unknown Calculator navigation error.",
                TextWrapping = TextWrapping.Wrap,
                IsTextSelectionEnabled = true
            });
            CalculatorFrame.Content = errorPanel;
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
            Width = Math.Max(320, m_startWidth + point.X - m_startPoint.X);
            Height = Math.Max(450, m_startHeight + point.Y - m_startPoint.Y);
        }

        private void ResizeGrip_PointerReleased(object sender, PointerRoutedEventArgs e)
        {
            m_resizing = false;
            ResizeGrip.ReleasePointerCaptures();
        }
    }
}
