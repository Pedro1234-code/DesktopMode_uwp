using System.ComponentModel;
using System.Runtime.CompilerServices;

namespace factoryos_10x_shell.Library.Models.InternalData
{
    public sealed class WebAppDefinition : INotifyPropertyChanged
    {
        public string Id { get; set; }
        public string Name { get; set; }
        public string StartUri { get; set; }
        private string m_iconUri;
        private bool m_isPinned;

        public string IconUri { get => m_iconUri; set => Set(ref m_iconUri, value); }
        public bool IsPinned { get => m_isPinned; set => Set(ref m_isPinned, value); }

        public event PropertyChangedEventHandler PropertyChanged;

        private void Set<T>(ref T field, T value, [CallerMemberName] string propertyName = null)
        {
            if (Equals(field, value)) return;
            field = value;
            PropertyChanged?.Invoke(this, new PropertyChangedEventArgs(propertyName));
        }
    }
}
