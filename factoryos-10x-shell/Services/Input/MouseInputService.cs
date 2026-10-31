using System;
using factoryos_10x_shell.Library.Services.Input;
using Windows.Devices.Input;
using Windows.UI.Core;
using Windows.UI.Input;

namespace factoryos_10x_shell.Services.Input
{
    /// <summary>
    /// Adapts UWP's CoreWindow pointer stream into one mouse-only input source.
    /// XAML controls retain their normal pointer handling; this service is for
    /// shell-wide behaviour such as menus, virtual windows, and diagnostics.
    /// </summary>
    public sealed class MouseInputService : IMouseInputService
    {
        private CoreWindow m_coreWindow;
        private readonly MouseInputChangedEventArgs m_movedEvent;
        private readonly MouseInputChangedEventArgs m_pressedEvent;
        private readonly MouseInputChangedEventArgs m_releasedEvent;

        public bool IsMouseDetected { get; private set; }
        public MouseInputSnapshot Current { get; } = new MouseInputSnapshot();

        public event EventHandler<MouseInputChangedEventArgs> InputChanged;

        public MouseInputService()
        {
            // PointerMoved can arrive hundreds of times per second. Reuse event
            // objects backed by the shared Current snapshot instead of feeding
            // the GC with one short-lived allocation per raw mouse packet.
            m_movedEvent = new MouseInputChangedEventArgs(MouseInputChangeKind.Moved, Current);
            m_pressedEvent = new MouseInputChangedEventArgs(MouseInputChangeKind.Pressed, Current);
            m_releasedEvent = new MouseInputChangedEventArgs(MouseInputChangeKind.Released, Current);
        }

        public void Attach(CoreWindow coreWindow)
        {
            if (coreWindow == null)
                throw new ArgumentNullException(nameof(coreWindow));

            if (ReferenceEquals(m_coreWindow, coreWindow))
                return;

            Detach();
            m_coreWindow = coreWindow;
            m_coreWindow.PointerMoved += CoreWindow_PointerMoved;
            m_coreWindow.PointerPressed += CoreWindow_PointerPressed;
            m_coreWindow.PointerReleased += CoreWindow_PointerReleased;
            m_coreWindow.PointerWheelChanged += CoreWindow_PointerWheelChanged;
        }

        public void Detach()
        {
            if (m_coreWindow == null)
                return;

            m_coreWindow.PointerMoved -= CoreWindow_PointerMoved;
            m_coreWindow.PointerPressed -= CoreWindow_PointerPressed;
            m_coreWindow.PointerReleased -= CoreWindow_PointerReleased;
            m_coreWindow.PointerWheelChanged -= CoreWindow_PointerWheelChanged;
            m_coreWindow = null;
        }

        private void CoreWindow_PointerMoved(CoreWindow sender, PointerEventArgs args) => Update(args.CurrentPoint, MouseInputChangeKind.Moved);

        private void CoreWindow_PointerPressed(CoreWindow sender, PointerEventArgs args) => Update(args.CurrentPoint, MouseInputChangeKind.Pressed);

        private void CoreWindow_PointerReleased(CoreWindow sender, PointerEventArgs args) => Update(args.CurrentPoint, MouseInputChangeKind.Released);

        private void CoreWindow_PointerWheelChanged(CoreWindow sender, PointerEventArgs args)
        {
            PointerPoint point = args.CurrentPoint;
            Update(point, MouseInputChangeKind.WheelChanged, point.Properties.MouseWheelDelta);
        }

        private void Update(PointerPoint point, MouseInputChangeKind kind, int wheelDelta = 0)
        {
            if (point?.PointerDevice?.PointerDeviceType != PointerDeviceType.Mouse)
                return;

            PointerPointProperties properties = point.Properties;
            IsMouseDetected = true;
            Current.X = point.Position.X;
            Current.Y = point.Position.Y;
            Current.IsLeftButtonPressed = properties.IsLeftButtonPressed;
            Current.IsMiddleButtonPressed = properties.IsMiddleButtonPressed;
            Current.IsRightButtonPressed = properties.IsRightButtonPressed;
            Current.IsXButton1Pressed = properties.IsXButton1Pressed;
            Current.IsXButton2Pressed = properties.IsXButton2Pressed;

            MouseInputChangedEventArgs eventArgs;
            switch (kind)
            {
                case MouseInputChangeKind.Moved:
                    eventArgs = m_movedEvent;
                    break;
                case MouseInputChangeKind.Pressed:
                    eventArgs = m_pressedEvent;
                    break;
                case MouseInputChangeKind.Released:
                    eventArgs = m_releasedEvent;
                    break;
                default:
                    eventArgs = new MouseInputChangedEventArgs(kind, Current, wheelDelta);
                    break;
            }
            InputChanged?.Invoke(this, eventArgs);
        }
    }
}
