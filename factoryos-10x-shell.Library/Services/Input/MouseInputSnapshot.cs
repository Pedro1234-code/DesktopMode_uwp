namespace factoryos_10x_shell.Library.Services.Input
{
    /// <summary>
    /// The latest mouse sample in the CoreShell window's device-independent pixels.
    /// </summary>
    public sealed class MouseInputSnapshot
    {
        public double X { get; set; }
        public double Y { get; set; }
        public bool IsLeftButtonPressed { get; set; }
        public bool IsMiddleButtonPressed { get; set; }
        public bool IsRightButtonPressed { get; set; }
        public bool IsXButton1Pressed { get; set; }
        public bool IsXButton2Pressed { get; set; }
    }
}
