using System.ComponentModel;
using System.Runtime.CompilerServices;
using Windows.UI.Xaml;

namespace factoryos_10x_shell.Library.Models.InternalData
{
    public sealed class WebAppWindowModel : INotifyPropertyChanged
    {
        private double m_left;
        private double m_top;
        private double m_width = 860;
        private double m_height = 560;
        private int m_zIndex;
        private Visibility m_visibility = Visibility.Visible;
        private bool m_isMaximized;
        private bool m_isActive;

        public WebAppDefinition App { get; set; }
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

        public event PropertyChangedEventHandler PropertyChanged;

        private void Set<T>(ref T field, T value, [CallerMemberName] string propertyName = null)
        {
            if (Equals(field, value)) return;
            field = value;
            PropertyChanged?.Invoke(this, new PropertyChangedEventArgs(propertyName));
        }
    }
}
