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

        private bool _isSearchButtonVisible = true;
        public bool IsSearchButtonVisible
        {
            get => _isSearchButtonVisible;
            set
            {
                _isSearchButtonVisible = value;
                OnSearchButtonVisibilityChanged?.Invoke(value);
            }
        }

        private bool _isCopilotButtonVisible = true;
        public bool IsCopilotButtonVisible
        {
            get => _isCopilotButtonVisible;
            set
            {
                _isCopilotButtonVisible = value;
                OnCopilotButtonVisibilityChanged?.Invoke(value);
            }
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

        public event Action<bool> OnSearchButtonVisibilityChanged;
        public event Action<bool> OnCopilotButtonVisibilityChanged;
        public bool IsFilesOpen { get; private set; }
        public bool IsFilesMinimized { get; private set; }
        public event Action<bool> OnFilesRequested;
        public event Action OnFilesStateChanged;
        public event Action OnFilesActivated;

        public void RequestFilesOpen(bool resetHome = false) => OnFilesRequested?.Invoke(resetHome);

        public void SetFilesWindowState(bool isOpen, bool isMinimized)
        {
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
            IsSettingsOpen = isOpen;
            IsSettingsMinimized = isMinimized;
            OnSettingsStateChanged?.Invoke();
        }
        public void ActivateSettings() => OnSettingsActivated?.Invoke();
        public void RequestWallpaper(string uri) => OnWallpaperRequested?.Invoke(uri);
    }
}
