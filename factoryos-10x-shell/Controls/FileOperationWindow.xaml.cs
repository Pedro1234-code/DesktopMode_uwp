using System;
using factoryos_10x_shell.Services.Files;
using Windows.Foundation;
using Windows.UI.Xaml;
using Windows.UI.Xaml.Controls;
using Windows.UI.Xaml.Input;
using Windows.UI.Xaml.Media;

namespace factoryos_10x_shell.Controls
{
    public sealed partial class FileOperationWindow : UserControl
    {
        private bool m_dragging;
        private Point m_start;
        private double m_left, m_top;

        public FileOperationWindow() { InitializeComponent(); }
        private FileOperationModel Operation => DataContext as FileOperationModel;

        private void Close_Click(object sender, RoutedEventArgs e)
        {
            if (Operation != null) FileOperationService.Instance.Close(Operation);
        }

        private void Action_Click(object sender, RoutedEventArgs e)
        {
            if (Operation?.IsRunning == true) Operation.Cancel();
            else if (Operation != null) FileOperationService.Instance.Close(Operation);
        }

        private void Minimize_Click(object sender, RoutedEventArgs e)
        {
            if (Operation != null) FileOperationService.Instance.Minimize(Operation);
        }

        private void Password_Click(object sender, RoutedEventArgs e)
        {
            Operation?.SubmitPassword(ArchivePassword.Password);
            ArchivePassword.Password = string.Empty;
        }

        private void Title_PointerPressed(object sender, PointerRoutedEventArgs e)
        {
            if (!e.GetCurrentPoint(sender as UIElement).Properties.IsLeftButtonPressed) return;
            var host = VisualTreeHelper.GetParent(this) as Canvas;
            if (host == null) return;
            FileOperationService.Instance.Activate(Operation);
            m_start = e.GetCurrentPoint(host).Position;
            m_left = Canvas.GetLeft(this);
            m_top = Canvas.GetTop(this);
            m_dragging = (sender as UIElement)?.CapturePointer(e.Pointer) == true;
            e.Handled = true;
        }

        private void Title_PointerMoved(object sender, PointerRoutedEventArgs e)
        {
            var host = VisualTreeHelper.GetParent(this) as Canvas;
            if (!m_dragging || host == null) return;
            Point position = e.GetCurrentPoint(host).Position;
            Canvas.SetLeft(this, Math.Max(0, Math.Min(Math.Max(0, host.ActualWidth - ActualWidth), m_left + position.X - m_start.X)));
            Canvas.SetTop(this, Math.Max(0, Math.Min(Math.Max(0, host.ActualHeight - 40), m_top + position.Y - m_start.Y)));
        }

        private void Title_PointerReleased(object sender, PointerRoutedEventArgs e)
        {
            m_dragging = false;
            (sender as UIElement)?.ReleasePointerCaptures();
        }

        private void Title_PointerCaptureLost(object sender, PointerRoutedEventArgs e) => m_dragging = false;
    }
}
