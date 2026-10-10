using System;
using Windows.Foundation;

namespace factoryos_10x_shell.Controls
{
    internal enum WindowResizeCorner
    {
        TopLeft,
        TopRight,
        BottomLeft,
        BottomRight
    }

    internal static class WindowResize
    {
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
            bool fromLeft = corner == WindowResizeCorner.TopLeft || corner == WindowResizeCorner.BottomLeft;
            bool fromTop = corner == WindowResizeCorner.TopLeft || corner == WindowResizeCorner.TopRight;

            double width = fromLeft ? startBounds.Width - dx : startBounds.Width + dx;
            double height = fromTop ? startBounds.Height - dy : startBounds.Height + dy;
            width = Math.Max(minimumWidth, width);
            height = Math.Max(minimumHeight, height);

            double left = fromLeft ? startBounds.Right - width : startBounds.X;
            double top = fromTop ? startBounds.Bottom - height : startBounds.Y;

            // A corner dragged beyond the desktop edge stops at that edge while
            // retaining the opposite side of the window.
            if (fromLeft && left < 0)
            {
                width += left;
                left = 0;
            }
            if (fromTop && top < 0)
            {
                height += top;
                top = 0;
            }

            return new Rect(left, top, Math.Max(minimumWidth, width), Math.Max(minimumHeight, height));
        }
    }
}
