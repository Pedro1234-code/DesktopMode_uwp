using factoryos_10x_shell.Library.Models.InternalData;
using factoryos_10x_shell.Library.Services.WebApps;
using System.Collections.ObjectModel;
using System;
using System.ComponentModel;
using System.Linq;
using Windows.UI.Xaml;

namespace factoryos_10x_shell.Services.WebApps
{
    public sealed class WindowManagerService : IWindowManagerService
    {
        private int m_nextZIndex;
        private double m_workspaceWidth;
        private double m_workspaceHeight;
        private bool m_isTaskViewOpen;
        private bool m_changingWindowState;

        public ObservableCollection<WebAppWindowModel> Windows { get; } = new ObservableCollection<WebAppWindowModel>();
        public bool IsTaskViewOpen => m_isTaskViewOpen;
        public event EventHandler WindowsChanged;
        public event EventHandler DesktopFocusRequested;
        public event EventHandler TaskViewChanged;

        public void Open(WebAppDefinition app)
        {
            WebAppWindowModel existing = Windows
                .Where(window => window.App.Id == app.Id)
                .OrderByDescending(window => window.ZIndex)
                .FirstOrDefault();
            if (existing != null)
            {
                existing.Visibility = Visibility.Visible;
                Activate(existing);
                return;
            }

            OpenNewWindow(app);
        }

        public void OpenNewWindow(WebAppDefinition app)
        {
            var window = new WebAppWindowModel
            {
                App = app,
                Left = 80 + (Windows.Count * 28),
                Top = 56 + (Windows.Count * 28)
            };
            window.PropertyChanged += Window_PropertyChanged;
            Windows.Add(window);
            Activate(window);
        }

        public void ToggleFromTaskbar(WebAppDefinition app)
        {
            WebAppWindowModel visible = Windows
                .Where(window => window.App.Id == app.Id && window.Visibility == Visibility.Visible)
                .OrderByDescending(window => window.ZIndex)
                .FirstOrDefault();

            if (visible != null)
            {
                if (visible.IsActive) Minimize(visible);
                else Activate(visible);
                return;
            }

            WebAppWindowModel minimized = Windows
                .Where(window => window.App.Id == app.Id)
                .OrderByDescending(window => window.ZIndex)
                .FirstOrDefault();
            if (minimized != null)
            {
                minimized.Visibility = Visibility.Visible;
                Activate(minimized);
            }
            else OpenNewWindow(app);
        }

        public void ToggleTaskView()
        {
            m_isTaskViewOpen = !m_isTaskViewOpen;
            TaskViewChanged?.Invoke(this, EventArgs.Empty);
        }

        public void CloseTaskView()
        {
            if (!m_isTaskViewOpen) return;
            m_isTaskViewOpen = false;
            TaskViewChanged?.Invoke(this, EventArgs.Empty);
        }

        public void Activate(WebAppWindowModel window)
        {
            if (window == null || !Windows.Contains(window)) return;
            m_changingWindowState = true;
            foreach (WebAppWindowModel item in Windows) item.IsActive = item == window;
            window.Visibility = Visibility.Visible;
            window.ZIndex = ++m_nextZIndex;
            m_changingWindowState = false;
            WindowsChanged?.Invoke(this, EventArgs.Empty);
            CloseTaskView();
        }

        public void DeactivateAll()
        {
            m_changingWindowState = true;
            foreach (WebAppWindowModel item in Windows) item.IsActive = false;
            m_changingWindowState = false;
            WindowsChanged?.Invoke(this, EventArgs.Empty);
        }

        public void Minimize(WebAppWindowModel window)
        {
            if (window == null || !Windows.Contains(window)) return;
            m_changingWindowState = true;
            window.IsActive = false;
            window.Visibility = Visibility.Collapsed;
            WebAppWindowModel next = Windows
                .Where(item => item != window && item.Visibility == Visibility.Visible)
                .OrderByDescending(item => item.ZIndex)
                .FirstOrDefault();
            if (next != null) ActivateCore(next);
            m_changingWindowState = false;
            WindowsChanged?.Invoke(this, EventArgs.Empty);
            DesktopFocusRequested?.Invoke(this, EventArgs.Empty);
        }

        public void Close(WebAppWindowModel window)
        {
            if (window == null || !Windows.Contains(window)) return;
            m_changingWindowState = true;
            window.PropertyChanged -= Window_PropertyChanged;
            Windows.Remove(window);
            if (!Windows.Any(window => window.IsActive))
            {
                WebAppWindowModel next = Windows.Where(window => window.Visibility == Visibility.Visible)
                    .OrderByDescending(window => window.ZIndex).FirstOrDefault();
                if (next != null) ActivateCore(next);
            }
            m_changingWindowState = false;
            WindowsChanged?.Invoke(this, EventArgs.Empty);
            DesktopFocusRequested?.Invoke(this, EventArgs.Empty);
        }

        public void ToggleMaximize(WebAppWindowModel window)
        {
            if (!window.IsMaximized)
            {
                if (m_workspaceWidth <= 0 || m_workspaceHeight <= 0)
                    return;

                window.RestoreLeft = window.Left; window.RestoreTop = window.Top;
                window.RestoreWidth = window.Width; window.RestoreHeight = window.Height;
                window.Left = 0; window.Top = 0;
                window.Width = m_workspaceWidth; window.Height = m_workspaceHeight;
                window.IsMaximized = true;
            }
            else
            {
                window.Left = window.RestoreLeft; window.Top = window.RestoreTop;
                window.Width = window.RestoreWidth; window.Height = window.RestoreHeight;
                window.IsMaximized = false;
            }
            Activate(window);
        }

        public void SetWorkspaceBounds(double width, double height)
        {
            m_workspaceWidth = width;
            m_workspaceHeight = height;
            foreach (WebAppWindowModel window in Windows.Where(window => window.IsMaximized))
            {
                window.Width = width;
                window.Height = height;
            }
        }

        private void Window_PropertyChanged(object sender, PropertyChangedEventArgs e)
        {
            // Left/Top change continuously while dragging. These layout-only
            // updates must not recreate taskbar buttons.
            if (!m_changingWindowState &&
                (e.PropertyName == nameof(WebAppWindowModel.IsActive) ||
                e.PropertyName == nameof(WebAppWindowModel.Visibility)))
            {
                WindowsChanged?.Invoke(this, EventArgs.Empty);
            }
        }

        private void ActivateCore(WebAppWindowModel window)
        {
            foreach (WebAppWindowModel item in Windows) item.IsActive = item == window;
            window.Visibility = Visibility.Visible;
            window.ZIndex = ++m_nextZIndex;
        }
    }
}
