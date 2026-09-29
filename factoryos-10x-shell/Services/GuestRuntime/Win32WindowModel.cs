using System;
using System.ComponentModel;
using System.Runtime.CompilerServices;
using Windows.Storage;
using Windows.UI.Xaml;
using Windows.UI.Xaml.Media;

namespace factoryos_10x_shell.Services.Win32
{
    public sealed class Win32WindowModel : INotifyPropertyChanged
    {
        private double m_left;
        private double m_top;
        private double m_width = 900;
        private double m_height = 600;
        private int m_zIndex;
        private Visibility m_visibility = Visibility.Visible;
        private bool m_isMaximized;
        private bool m_isActive;
        private string m_status = "Preparing runtime…";
        private ImageSource m_iconSource;

        public Guid Id { get; } = Guid.NewGuid();
        public StorageFile Executable { get; set; }
        public StorageFolder ModuleSourceFolder { get; set; }
        public string DisplayName => Executable?.DisplayName ?? Executable?.Name ?? "Win32 app";
        public double RestoreLeft { get; set; }
        public double RestoreTop { get; set; }
        public double RestoreWidth { get; set; }
        public double RestoreHeight { get; set; }

        public double Left { get => m_left; set => Set(ref m_left, value); }
        public double Top { get => m_top; set => Set(ref m_top, value); }
        public double Width { get => m_width; set => Set(ref m_width, value); }
        public double Height { get => m_height; set => Set(ref m_height, value); }
        public int ZIndex { get => m_zIndex; set => Set(ref m_zIndex, value); }
        public Visibility Visibility { get => m_visibility; set => Set(ref m_visibility, value); }
        public bool IsMaximized { get => m_isMaximized; set => Set(ref m_isMaximized, value); }
        public bool IsActive { get => m_isActive; set => Set(ref m_isActive, value); }
        public string Status { get => m_status; set => Set(ref m_status, value); }
        public ImageSource IconSource { get => m_iconSource; set => Set(ref m_iconSource, value); }

        public event PropertyChangedEventHandler PropertyChanged;

        private void Set<T>(ref T field, T value, [CallerMemberName] string propertyName = null)
        {
            if (Equals(field, value)) return;
            field = value;
            PropertyChanged?.Invoke(this, new PropertyChangedEventArgs(propertyName));
        }
    }
}
