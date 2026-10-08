using System;
using System.Collections.Generic;
using System.Collections.ObjectModel;
using System.ComponentModel;
using System.Linq;
using System.Runtime.CompilerServices;
using Windows.UI.Xaml.Media;

namespace factoryos_10x_shell.Services.Windowing
{
    internal enum ShellWindowKind
    {
        WebApp,
        Win32App,
        Files,
        Notepad,
        Settings,
        Calculator,
        Firefox
    }

    internal sealed class ShellWindowReference
    {
        public ShellWindowReference(ShellWindowKind kind, object identity)
        {
            Kind = kind;
            Identity = identity ?? throw new ArgumentNullException(nameof(identity));
        }

        public ShellWindowKind Kind { get; }
        public object Identity { get; }
    }

    internal sealed class ShellWindowDescriptor
    {
        public ShellWindowKind Kind { get; set; }
        public object Identity { get; set; }
        public string Title { get; set; }
        public string Subtitle { get; set; }
        public string FallbackGlyph { get; set; }
        public ImageSource IconSource { get; set; }
        public bool IsActive { get; set; }
        public bool IsMinimized { get; set; }
    }

    internal sealed class ShellWindowItem : INotifyPropertyChanged
    {
        private string m_title;
        private string m_subtitle;
        private string m_fallbackGlyph;
        private ImageSource m_iconSource;
        private bool m_isActive;
        private bool m_isMinimized;

        public ShellWindowItem(ShellWindowDescriptor descriptor)
        {
            Kind = descriptor.Kind;
            Identity = descriptor.Identity ?? throw new ArgumentNullException(nameof(descriptor.Identity));
            Update(descriptor);
        }

        public ShellWindowKind Kind { get; }
        public object Identity { get; }
        public string Title { get => m_title; private set => Set(ref m_title, value); }
        public string Subtitle { get => m_subtitle; private set => Set(ref m_subtitle, value); }
        public string FallbackGlyph { get => m_fallbackGlyph; private set => Set(ref m_fallbackGlyph, value); }
        public ImageSource IconSource
        {
            get => m_iconSource;
            private set
            {
                if (!Set(ref m_iconSource, value)) return;
                OnPropertyChanged(nameof(IconOpacity));
                OnPropertyChanged(nameof(FallbackGlyphOpacity));
            }
        }
        public bool IsActive
        {
            get => m_isActive;
            private set
            {
                if (!Set(ref m_isActive, value)) return;
                OnPropertyChanged(nameof(ActiveIndicatorOpacity));
            }
        }
        public bool IsMinimized { get => m_isMinimized; private set => Set(ref m_isMinimized, value); }
        public double ActiveIndicatorOpacity => IsActive ? 1.0 : 0.0;
        public double IconOpacity => IconSource == null ? 0.0 : 1.0;
        public double FallbackGlyphOpacity => IconSource == null ? 1.0 : 0.0;

        public void Update(ShellWindowDescriptor descriptor)
        {
            Title = descriptor.Title;
            Subtitle = descriptor.IsMinimized ? "Minimized" : descriptor.Subtitle;
            FallbackGlyph = descriptor.FallbackGlyph;
            IconSource = descriptor.IconSource;
            IsActive = descriptor.IsActive;
            IsMinimized = descriptor.IsMinimized;
        }

        public event PropertyChangedEventHandler PropertyChanged;

        private bool Set<T>(ref T field, T value, [CallerMemberName] string propertyName = null)
        {
            if (Equals(field, value)) return false;
            field = value;
            OnPropertyChanged(propertyName);
            return true;
        }

        private void OnPropertyChanged([CallerMemberName] string propertyName = null) =>
            PropertyChanged?.Invoke(this, new PropertyChangedEventArgs(propertyName));
    }

    /// <summary>
    /// Maintains one activation history for all window families in the shell.
    /// Rendering and lifetime remain owned by the specialized window managers.
    /// </summary>
    internal sealed class ShellWindowCoordinator
    {
        private readonly List<ShellWindowReference> m_mru = new List<ShellWindowReference>();

        public ShellWindowReference ActiveWindow { get; private set; }
        public ObservableCollection<ShellWindowItem> Windows { get; } = new ObservableCollection<ShellWindowItem>();
        public event EventHandler StateChanged;

        public bool IsActive(ShellWindowKind kind, object identity) =>
            ActiveWindow != null && Matches(ActiveWindow, kind, identity);

        public void Synchronize(IEnumerable<ShellWindowDescriptor> descriptors)
        {
            if (descriptors == null) throw new ArgumentNullException(nameof(descriptors));
            List<ShellWindowDescriptor> current = descriptors
                .Where(item => item != null && item.Identity != null)
                .ToList();

            for (int index = Windows.Count - 1; index >= 0; index--)
            {
                ShellWindowItem item = Windows[index];
                if (!current.Any(descriptor => Matches(item, descriptor.Kind, descriptor.Identity)))
                    Windows.RemoveAt(index);
            }

            foreach (ShellWindowDescriptor descriptor in current)
            {
                ShellWindowItem item = Windows.FirstOrDefault(candidate =>
                    Matches(candidate, descriptor.Kind, descriptor.Identity));
                if (item == null)
                {
                    item = new ShellWindowItem(descriptor);
                    Windows.Add(item);
                }
                else item.Update(descriptor);
            }

            ReorderTaskView();
            StateChanged?.Invoke(this, EventArgs.Empty);
        }

        public void Activate(ShellWindowKind kind, object identity)
        {
            if (identity == null) return;

            ShellWindowReference existing = m_mru.FirstOrDefault(item => Matches(item, kind, identity));
            if (ReferenceEquals(ActiveWindow, existing)) return;

            if (existing != null) m_mru.Remove(existing);
            else existing = new ShellWindowReference(kind, identity);

            m_mru.Insert(0, existing);
            ActiveWindow = existing;
            ReorderTaskView();
            StateChanged?.Invoke(this, EventArgs.Empty);
        }

        public void Remove(ShellWindowKind kind, object identity)
        {
            if (identity == null) return;
            ShellWindowReference existing = m_mru.FirstOrDefault(item => Matches(item, kind, identity));
            if (existing == null) return;

            m_mru.Remove(existing);
            if (ReferenceEquals(ActiveWindow, existing)) ActiveWindow = null;
        }

        public void Prune(Func<ShellWindowReference, bool> isAvailable)
        {
            if (isAvailable == null) throw new ArgumentNullException(nameof(isAvailable));

            for (int index = m_mru.Count - 1; index >= 0; index--)
            {
                if (isAvailable(m_mru[index])) continue;
                if (ReferenceEquals(ActiveWindow, m_mru[index])) ActiveWindow = null;
                m_mru.RemoveAt(index);
            }
        }

        public ShellWindowReference GetMostRecentAvailable(Func<ShellWindowReference, bool> isAvailable)
        {
            if (isAvailable == null) throw new ArgumentNullException(nameof(isAvailable));
            return m_mru.FirstOrDefault(isAvailable);
        }

        public void ClearActive()
        {
            if (ActiveWindow == null) return;
            ActiveWindow = null;
            StateChanged?.Invoke(this, EventArgs.Empty);
        }

        private static bool Matches(ShellWindowReference item, ShellWindowKind kind, object identity)
        {
            return item.Kind == kind &&
                (ReferenceEquals(item.Identity, identity) || Equals(item.Identity, identity));
        }

        private static bool Matches(ShellWindowItem item, ShellWindowKind kind, object identity)
        {
            return item.Kind == kind &&
                (ReferenceEquals(item.Identity, identity) || Equals(item.Identity, identity));
        }

        private void ReorderTaskView()
        {
            int destinationIndex = 0;
            foreach (ShellWindowReference reference in m_mru)
            {
                ShellWindowItem item = Windows.FirstOrDefault(candidate =>
                    Matches(candidate, reference.Kind, reference.Identity));
                if (item == null) continue;

                int sourceIndex = Windows.IndexOf(item);
                if (sourceIndex != destinationIndex) Windows.Move(sourceIndex, destinationIndex);
                destinationIndex++;
            }
        }
    }
}
