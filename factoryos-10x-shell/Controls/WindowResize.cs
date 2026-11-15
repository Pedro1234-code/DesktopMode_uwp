using System;
using Windows.Foundation;
using Windows.UI.Xaml;
using Windows.UI.Xaml.Controls;
using Windows.UI.Xaml.Input;
using Windows.UI.Xaml.Media;

namespace factoryos_10x_shell.Controls
{
    internal enum WindowResizeCorner
    {
        TopLeft,
        TopRight,
        BottomLeft,
        BottomRight,
        Left,
        Top,
        Right,
        Bottom
    }

    internal static class WindowResize
    {
        internal static UIElement CoordinateSpace(DependencyObject window)
        {
            for (var parent = VisualTreeHelper.GetParent(window); parent != null;
                 parent = VisualTreeHelper.GetParent(parent))
            {
                if (parent is Canvas canvas) return canvas;
            }
            return null;
        }

        internal static bool TryCapture(object sender, PointerRoutedEventArgs e) =>
            sender is UIElement grip &&
            e.GetCurrentPoint(grip).Properties.IsLeftButtonPressed &&
            grip.CapturePointer(e.Pointer);

        internal static WindowResizeCorner CornerFromTag(object tag)
        {
            return Enum.TryParse(tag as string, out WindowResizeCorner corner)
                ? corner
                : WindowResizeCorner.BottomRight;
        }

        internal static Rect Calculate(
            WindowResizeCorner corner,
            Point startPointer,
            Point currentPointer,
            Rect startBounds,
            double minimumWidth,
            double minimumHeight)
        {
            double dx = currentPointer.X - startPointer.X;
            double dy = currentPointer.Y - startPointer.Y;
            bool fromLeft = corner == WindowResizeCorner.Left ||
                corner == WindowResizeCorner.TopLeft || corner == WindowResizeCorner.BottomLeft;
            bool fromTop = corner == WindowResizeCorner.Top ||
                corner == WindowResizeCorner.TopLeft || corner == WindowResizeCorner.TopRight;
            bool changesWidth = corner != WindowResizeCorner.Top && corner != WindowResizeCorner.Bottom;
            bool changesHeight = corner != WindowResizeCorner.Left && corner != WindowResizeCorner.Right;

            double width = changesWidth
                ? Math.Max(minimumWidth, startBounds.Width + (fromLeft ? -dx : dx))
                : startBounds.Width;
            double height = changesHeight
                ? Math.Max(minimumHeight, startBounds.Height + (fromTop ? -dy : dy))
                : startBounds.Height;

            double left = fromLeft ? startBounds.Right - width : startBounds.X;
            double top = fromTop ? startBounds.Bottom - height : startBounds.Y;

            return new Rect(left, top, width, height);
        }
    }
}
