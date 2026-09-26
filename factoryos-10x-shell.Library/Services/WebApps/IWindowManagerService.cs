using factoryos_10x_shell.Library.Models.InternalData;
using System;
using System.Collections.ObjectModel;

namespace factoryos_10x_shell.Library.Services.WebApps
{
    public interface IWindowManagerService
    {
        ObservableCollection<WebAppWindowModel> Windows { get; }
        bool IsTaskViewOpen { get; }
        event EventHandler WindowsChanged;
        event EventHandler DesktopFocusRequested;
        event EventHandler TaskViewChanged;
        void Open(WebAppDefinition app);
        void OpenNewWindow(WebAppDefinition app);
        void ToggleFromTaskbar(WebAppDefinition app);
        void ToggleTaskView();
        void CloseTaskView();
        void Activate(WebAppWindowModel window);
        void DeactivateAll();
        void Minimize(WebAppWindowModel window);
        void ToggleMaximize(WebAppWindowModel window);
        void Close(WebAppWindowModel window);
        void SetWorkspaceBounds(double width, double height);
    }
}
