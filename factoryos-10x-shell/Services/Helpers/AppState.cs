using System;
using System.Collections.Generic;
using System.Linq;
using System.Text;
using System.Threading.Tasks;
using Windows.Storage;

namespace factoryos_10x_shell.Services.Helpers
{
    public class AppState
    {
        private static AppState _instance;
        public static AppState Instance => _instance ?? (_instance = new AppState());

        public event Action TaskbarPreferencesChanged;
        public bool TaskbarAutoHide
        {
            get => ApplicationData.Current.LocalSettings.Values["TaskbarAutoHide"] as bool? ?? false;
            set { ApplicationData.Current.LocalSettings.Values["TaskbarAutoHide"] = value; TaskbarPreferencesChanged?.Invoke(); }
        }
        public bool TaskbarAlignLeft
        {
            get => ApplicationData.Current.LocalSettings.Values["TaskbarAlignLeft"] as bool? ?? false;
            set { ApplicationData.Current.LocalSettings.Values["TaskbarAlignLeft"] = value; TaskbarPreferencesChanged?.Invoke(); }
        }

        private bool _isBgChangeButtonVisible;
        public bool IsBgChangeButtonVisible
        {
            get => _isBgChangeButtonVisible;
            set
            {
                if (_isBgChangeButtonVisible != value)
                {
                    _isBgChangeButtonVisible = value;
                    OnBgChangeButtonVisibilityChanged?.Invoke(_isBgChangeButtonVisible);
                }
            }
        }


        public event Action<bool> OnBgChangeButtonVisibilityChanged;

        public bool IsFilesOpen { get; private set; }
        public bool IsFilesMinimized { get; private set; }
        public event Action<bool> OnFilesRequested;
        public event Action<string> OnFilesRevealRequested;
        public event Action OnFilesStateChanged;
        public event Action OnFilesActivated;

        public void RequestFilesOpen(bool resetHome = false) => OnFilesRequested?.Invoke(resetHome);
        public void RequestFilesReveal(string path)
        {
            if (!string.IsNullOrWhiteSpace(path)) OnFilesRevealRequested?.Invoke(path);
        }

        public void SetFilesWindowState(bool isOpen, bool isMinimized)
        {
            if (IsFilesOpen == isOpen && IsFilesMinimized == isMinimized) return;
            IsFilesOpen = isOpen;
            IsFilesMinimized = isMinimized;
            OnFilesStateChanged?.Invoke();
        }

        public void ActivateFiles() => OnFilesActivated?.Invoke();

        public bool IsNotepadOpen { get; private set; }
        public bool IsNotepadMinimized { get; private set; }
        public event Action OnNotepadRequested;
        public event Action OnNotepadStateChanged;
        public event Action OnNotepadActivated;
        private StorageFile m_pendingNotepadFile;

        public void RequestNotepadOpen(StorageFile file = null)
        {
            m_pendingNotepadFile = file;
            OnNotepadRequested?.Invoke();
        }

        public StorageFile TakePendingNotepadFile()
        {
            StorageFile file = m_pendingNotepadFile;
            m_pendingNotepadFile = null;
            return file;
        }

        public void SetNotepadWindowState(bool isOpen, bool isMinimized)
        {
            if (IsNotepadOpen == isOpen && IsNotepadMinimized == isMinimized) return;
            IsNotepadOpen = isOpen;
            IsNotepadMinimized = isMinimized;
            OnNotepadStateChanged?.Invoke();
        }

        public void ActivateNotepad() => OnNotepadActivated?.Invoke();

        public bool IsSettingsOpen { get; private set; }
        public bool IsSettingsMinimized { get; private set; }
        public event Action OnSettingsRequested;
        public event Action OnSettingsStateChanged;
        public event Action OnSettingsActivated;
        public event Action<string> OnWallpaperRequested;

        public void RequestSettingsOpen() => OnSettingsRequested?.Invoke();
        public void SetSettingsWindowState(bool isOpen, bool isMinimized)
        {
            if (IsSettingsOpen == isOpen && IsSettingsMinimized == isMinimized) return;
            IsSettingsOpen = isOpen;
            IsSettingsMinimized = isMinimized;
            OnSettingsStateChanged?.Invoke();
        }
        public void ActivateSettings() => OnSettingsActivated?.Invoke();
        public void RequestWallpaper(string uri) => OnWallpaperRequested?.Invoke(uri);

        public bool IsCalculatorOpen { get; private set; }
        public bool IsCalculatorMinimized { get; private set; }
        public event Action OnCalculatorRequested;
        public event Action OnCalculatorStateChanged;
        public event Action OnCalculatorActivated;

        public void RequestCalculatorOpen() => OnCalculatorRequested?.Invoke();
        public void SetCalculatorWindowState(bool isOpen, bool isMinimized)
        {
            if (IsCalculatorOpen == isOpen && IsCalculatorMinimized == isMinimized) return;
            IsCalculatorOpen = isOpen;
            IsCalculatorMinimized = isMinimized;
            OnCalculatorStateChanged?.Invoke();
        }
        public void ActivateCalculator() => OnCalculatorActivated?.Invoke();

        public bool IsFirefoxOpen { get; private set; }
        public bool IsFirefoxMinimized { get; private set; }
        public event Action OnFirefoxRequested;
        public event Action OnFirefoxStateChanged;
        public event Action OnFirefoxActivated;
        private StorageFile m_pendingFirefoxFile;
        private readonly Queue<string> m_pendingFirefoxUrls = new Queue<string>();
        public event Action OnFirefoxUrlRequested;
        public bool HasPendingFirefoxUrls => m_pendingFirefoxUrls.Count > 0;

        public void RequestFirefoxUrl(string url)
        {
            if (!Uri.TryCreate(url, UriKind.Absolute, out Uri uri) ||
                (uri.Scheme != Uri.UriSchemeHttp && uri.Scheme != Uri.UriSchemeHttps)) return;
            m_pendingFirefoxUrls.Enqueue(uri.AbsoluteUri);
            OnFirefoxUrlRequested?.Invoke();
        }

        public string TakePendingFirefoxUrl() => m_pendingFirefoxUrls.Count == 0
            ? null : m_pendingFirefoxUrls.Dequeue();

        public void RequestFirefoxOpen(StorageFile file = null)
        {
            m_pendingFirefoxFile = file;
            OnFirefoxRequested?.Invoke();
        }

        public bool HasPendingFirefoxFile => m_pendingFirefoxFile != null;

        public StorageFile TakePendingFirefoxFile()
        {
            StorageFile file = m_pendingFirefoxFile;
            m_pendingFirefoxFile = null;
            return file;
        }
        public void SetFirefoxWindowState(bool isOpen, bool isMinimized)
        {
            if (IsFirefoxOpen == isOpen && IsFirefoxMinimized == isMinimized) return;
            IsFirefoxOpen = isOpen;
            IsFirefoxMinimized = isMinimized;
            OnFirefoxStateChanged?.Invoke();
        }
        public void ActivateFirefox() => OnFirefoxActivated?.Invoke();
    }
}
