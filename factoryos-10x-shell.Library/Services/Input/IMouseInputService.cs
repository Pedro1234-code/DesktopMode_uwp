using System;
using Windows.UI.Core;

namespace factoryos_10x_shell.Library.Services.Input
{
    /// <summary>
    /// Provides the application-wide mouse state exposed by UWP pointer events.
    /// </summary>
    public interface IMouseInputService
    {
        bool IsMouseDetected { get; }
        MouseInputSnapshot Current { get; }

        event EventHandler<MouseInputChangedEventArgs> InputChanged;

        void Attach(CoreWindow coreWindow);
        void Detach();
    }
}
