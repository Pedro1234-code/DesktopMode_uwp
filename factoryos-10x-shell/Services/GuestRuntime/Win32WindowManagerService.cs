using System;
using System.Collections.ObjectModel;
using System.ComponentModel;
using System.Linq;
using Windows.Storage;
using Windows.UI.Xaml;
using Windows.UI.Xaml.Media;

namespace factoryos_10x_shell.Services.Win32
{
    public sealed class Win32WindowManagerService
    {
        private const string AppSupportEnabledSettingKey = "CoreShell.Win32AppSupportEnabled";
        private int m_nextZIndex;
        private double m_workspaceWidth;
        private double m_workspaceHeight;
        private bool m_inputSuppressed;
        private bool m_changingWindowState;

        private Win32WindowManagerService() { }

        public static Win32WindowManagerService Instance { get; } = new Win32WindowManagerService();
        public ObservableCollection<Win32WindowModel> Windows { get; } = new ObservableCollection<Win32WindowModel>();
        public event EventHandler WindowsChanged;
        public event EventHandler DesktopFocusRequested;
        public event EventHandler InputStateChanged;
        public bool IsInputSuppressed => m_inputSuppressed;
        public bool IsAppSupportEnabled
        {
            get
            {
                object value;
                return ApplicationData.Current.LocalSettings.Values.TryGetValue(
                    AppSupportEnabledSettingKey, out value) &&
                    value is bool enabled && enabled;
            }
            set => ApplicationData.Current.LocalSettings.Values[AppSupportEnabledSettingKey] = value;
        }

        public Win32WindowModel Open(StorageFile executable, StorageFolder sourceFolder, ImageSource iconSource = null)
        {
            if (!IsAppSupportEnabled || executable == null || sourceFolder == null) return null;
            Win32WindowModel existing = Windows.FirstOrDefault(item =>
                string.Equals(item.Executable?.Path, executable.Path, StringComparison.OrdinalIgnoreCase) &&
                !string.IsNullOrWhiteSpace(executable.Path));
            if (existing != null)
            {
                if (iconSource != null) existing.IconSource = iconSource;
                Activate(existing);
                return existing;
            }
            var window = new Win32WindowModel
            {
                Executable = executable,
                ModuleSourceFolder = sourceFolder,
                IconSource = iconSource,
                Left = 92 + Windows.Count * 28,
                Top = 58 + Windows.Count * 28
            };
            window.PropertyChanged += Window_PropertyChanged;
            Windows.Add(window);
            Activate(window);
            return window;
        }

        public void Activate(Win32WindowModel window)
        {
            if (window == null || !Windows.Contains(window)) return;
            m_changingWindowState = true;
            foreach (Win32WindowModel item in Windows) item.IsActive = item == window;
            window.Visibility = Visibility.Visible;
            window.ZIndex = ++m_nextZIndex;
            m_changingWindowState = false;
            WindowsChanged?.Invoke(this, EventArgs.Empty);
        }

        public void DeactivateAll()
        {
            m_changingWindowState = true;
            foreach (Win32WindowModel item in Windows) item.IsActive = false;
            m_changingWindowState = false;
            WindowsChanged?.Invoke(this, EventArgs.Empty);
        }

        public void ToggleFromTaskbar(Win32WindowModel window)
        {
            if (window == null || !Windows.Contains(window)) return;
            if (window.Visibility == Visibility.Visible && window.IsActive)
            {
                Minimize(window);
                return;
            }
            Activate(window);
        }

        public void Minimize(Win32WindowModel window)
        {
            if (window == null || !Windows.Contains(window)) return;
            m_changingWindowState = true;
            window.IsActive = false;
            window.Visibility = Visibility.Collapsed;
            Win32WindowModel next = Windows.Where(item => item != window && item.Visibility == Visibility.Visible)
                .OrderByDescending(item => item.ZIndex).FirstOrDefault();
            if (next != null) ActivateCore(next);
            m_changingWindowState = false;
            WindowsChanged?.Invoke(this, EventArgs.Empty);
            DesktopFocusRequested?.Invoke(this, EventArgs.Empty);
        }

        public void ToggleMaximize(Win32WindowModel window)
        {
            if (window == null) return;
            if (!window.IsMaximized)
            {
                if (m_workspaceWidth <= 0 || m_workspaceHeight <= 0) return;
                window.RestoreLeft = window.Left;
                window.RestoreTop = window.Top;
                window.RestoreWidth = window.Width;
                window.RestoreHeight = window.Height;
                window.Left = 0;
                window.Top = 0;
                window.Width = m_workspaceWidth;
                window.Height = m_workspaceHeight;
                window.IsMaximized = true;
            }
            else
            {
                window.Left = window.RestoreLeft;
                window.Top = window.RestoreTop;
                window.Width = window.RestoreWidth;
                window.Height = window.RestoreHeight;
                window.IsMaximized = false;
            }
            Activate(window);
        }

        public void Close(Win32WindowModel window)
        {
            if (window == null || !Windows.Contains(window)) return;
            m_changingWindowState = true;
            window.PropertyChanged -= Window_PropertyChanged;
            Windows.Remove(window);
            Win32WindowModel next = Windows.Where(item => item.Visibility == Visibility.Visible)
                .OrderByDescending(item => item.ZIndex).FirstOrDefault();
            if (next != null) ActivateCore(next);
            m_changingWindowState = false;
            WindowsChanged?.Invoke(this, EventArgs.Empty);
            DesktopFocusRequested?.Invoke(this, EventArgs.Empty);
        }

        public void SetWorkspaceBounds(double width, double height)
        {
            m_workspaceWidth = width;
            m_workspaceHeight = height;
            foreach (Win32WindowModel window in Windows.Where(item => item.IsMaximized))
            {
                window.Width = width;
                window.Height = height;
            }
        }

        public void SetInputSuppressed(bool suppressed)
        {
            if (m_inputSuppressed == suppressed) return;
            m_inputSuppressed = suppressed;
            InputStateChanged?.Invoke(this, EventArgs.Empty);
        }

        private void Window_PropertyChanged(object sender, PropertyChangedEventArgs e)
        {
            if (!m_changingWindowState &&
                (e.PropertyName == nameof(Win32WindowModel.IsActive) ||
                e.PropertyName == nameof(Win32WindowModel.Visibility) ||
                e.PropertyName == nameof(Win32WindowModel.Status) ||
                e.PropertyName == nameof(Win32WindowModel.IconSource)))
                WindowsChanged?.Invoke(this, EventArgs.Empty);
        }

        private void ActivateCore(Win32WindowModel window)
        {
            foreach (Win32WindowModel item in Windows) item.IsActive = item == window;
            window.Visibility = Visibility.Visible;
            window.ZIndex = ++m_nextZIndex;
        }
    }
}
