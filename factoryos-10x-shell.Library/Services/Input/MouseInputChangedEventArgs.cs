using System;

namespace factoryos_10x_shell.Library.Services.Input
{
    public enum MouseInputChangeKind
    {
        Moved,
        Pressed,
        Released,
        WheelChanged
    }

    public sealed class MouseInputChangedEventArgs : EventArgs
    {
        public MouseInputChangedEventArgs(MouseInputChangeKind kind, MouseInputSnapshot snapshot, int wheelDelta = 0)
        {
            Kind = kind;
            Snapshot = snapshot;
            WheelDelta = wheelDelta;
        }

        public MouseInputChangeKind Kind { get; }
        public MouseInputSnapshot Snapshot { get; }
        public int WheelDelta { get; }
    }
}
