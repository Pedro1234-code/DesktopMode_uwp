using factoryos_10x_shell.Library.Services.Managers;
using factoryos_10x_shell.Library.ViewModels;
using Microsoft.Extensions.DependencyInjection;
using System;
using System.Collections.Generic;
using System.Diagnostics;
using System.IO;
using System.Linq;
using System.Runtime.InteropServices.WindowsRuntime;
using Windows.Foundation;
using Windows.Foundation.Collections;
using Windows.Media.Core;
using Microsoft.Win32;
using Windows.UI.Xaml.Controls;
using Windows.UI.Xaml.Media.Imaging;
using Windows.Media.Playback;
using Windows.UI.Core;
using Windows.UI.Input;
using Windows.UI.Xaml;
using Windows.UI.Xaml.Controls.Primitives;
using Windows.UI.Xaml.Data;
using Windows.System;
using Windows.UI.Input.Preview.Injection;
using Windows.UI.ViewManagement;
using Windows.UI.Xaml.Input;
using Windows.UI.Xaml.Media;
using System.Runtime.InteropServices;
using Windows.UI.Xaml.Media.Animation;
using Windows.UI.Xaml.Navigation;
using Windows.System.UserProfile;
using Windows.Storage;
using Windows.Storage.Pickers;
using System.Threading.Tasks;
using factoryos_10x_shell.Services.Helpers;
using factoryos_10x_shell.Library.Services.WebApps;
using factoryos_10x_shell.Library.Models.InternalData;
using factoryos_10x_shell.Services.Win32;
using factoryos_10x_shell.Services.Windowing;
using factoryos_10x_shell.Services.Files;

namespace factoryos_10x_shell.Views
{
    public sealed partial class MainDesktop : Page
    {
        private Storyboard OpenStartStoryboard { get; set; }
        private Storyboard CloseStartStoryboard { get; set; }

        private Storyboard OpenActionStoryboard { get; set; }
        private Storyboard CloseActionStoryboard { get; set; }


        private readonly IStartManagerService m_startManager;
        private readonly IActionCenterManagerService m_actionManager;
        private readonly IWindowManagerService m_windowManager;
        private readonly IWebAppService m_webAppService;
        private readonly Win32WindowManagerService m_nativeWindowManager;
        private readonly ShellWindowCoordinator m_shellWindowCoordinator;
        private readonly List<factoryos_10x_shell.Controls.AppInstallerWindow> m_installers = new List<factoryos_10x_shell.Controls.AppInstallerWindow>();
        private int m_installerZIndex;
        private readonly FileOperationService m_fileOperations = FileOperationService.Instance;
        private readonly Dictionary<FileOperationModel, factoryos_10x_shell.Controls.FileOperationWindow> m_operationWindows =
            new Dictionary<FileOperationModel, factoryos_10x_shell.Controls.FileOperationWindow>();
        private int m_operationZIndex;
        private factoryos_10x_shell.Controls.FilesWindow FilesWindow;
        private factoryos_10x_shell.Controls.NotepadWindow NotepadWindow;
        private factoryos_10x_shell.Controls.SettingsWindow SettingsWindow;
        private factoryos_10x_shell.Controls.CalculatorWindow CalculatorWindow;
        private factoryos_10x_shell.Controls.FirefoxWindow FirefoxWindow;
        private bool m_startMenuCreated;
        private bool m_actionCenterCreated;
        private bool m_coordinatingActivation;
        private bool m_startupUpdateCheckStarted;
        private bool m_firefoxExternalDragging;
        private bool m_firefoxExternalResizing;
        private bool m_firefoxExternalMaximized;
        private factoryos_10x_shell.Controls.WindowResizeCorner m_firefoxExternalResizeCorner;
        private Point m_firefoxExternalPointerStart;
        private Rect m_firefoxExternalStartBounds;
        private Rect m_firefoxExternalRestoreBounds;
        private Visibility m_taskbarVisibilityBeforeFirefoxFullscreen = Visibility.Visible;
        private readonly BitmapImage m_filesIcon = new BitmapImage(new Uri("ms-appx:///Assets/Files/files.png"));
        private readonly BitmapImage m_notepadIcon = new BitmapImage(new Uri("ms-appx:///Assets/Notepad/notepad.png"));
        private readonly BitmapImage m_settingsIcon = new BitmapImage(new Uri("ms-appx:///Windows10x-js-main/Icons/WindowsSettings.png"));
        private readonly BitmapImage m_calculatorIcon = new BitmapImage(new Uri("ms-appx:///Assets/Calculator/CalculatorAppList.targetsize-48.png"));
        private readonly BitmapImage m_firefoxIcon = new BitmapImage(new Uri("ms-appx:///Assets/Firefox/firefox.png"));

        private const string FilesWindowIdentity = "CoreShell.Files";
        private const string NotepadWindowIdentity = "CoreShell.Notepad";
        private const string SettingsWindowIdentity = "CoreShell.Settings";
        private const string CalculatorWindowIdentity = "CoreShell.Calculator";
        private const string FirefoxWindowIdentity = "CoreShell.Firefox";

        public MainDesktop()
        {
            this.InitializeComponent();
            m_taskbarHideTimer.Tick += AutoHideTimerTick;
            Loaded += (s, e) => {
                AppState.Instance.TaskbarPreferencesChanged += TaskbarPreferencesChanged;
                Window.Current.CoreWindow.PointerMoved += AutoHidePointerMoved;
                Window.Current.CoreWindow.PointerExited += AutoHidePointerExited;
                TaskbarPreferencesChanged();
            };
            Unloaded += (s, e) => {
                AppState.Instance.TaskbarPreferencesChanged -= TaskbarPreferencesChanged;
                Window.Current.CoreWindow.PointerMoved -= AutoHidePointerMoved;
                Window.Current.CoreWindow.PointerExited -= AutoHidePointerExited;
                m_taskbarHideTimer.Stop();
            };
            DataContext = App.ServiceProvider.GetRequiredService<MainDesktopViewModel>();

            m_startManager = App.ServiceProvider.GetRequiredService<IStartManagerService>();
            m_startManager.StartVisibilityChanged += StartManager_StartVisibilityChanged;

            m_actionManager = App.ServiceProvider.GetRequiredService<IActionCenterManagerService>();
            m_actionManager.ActionVisibilityChanged += ActionCenterManager_ActionVisibilityChanged;
            m_windowManager = App.ServiceProvider.GetRequiredService<IWindowManagerService>();
            m_webAppService = App.ServiceProvider.GetRequiredService<IWebAppService>();
            m_shellWindowCoordinator = App.ServiceProvider.GetRequiredService<ShellWindowCoordinator>();
            m_windowManager.DesktopFocusRequested += WindowManager_DesktopFocusRequested;
            m_windowManager.TaskViewChanged += WindowManager_TaskViewChanged;
            WindowHost.DataContext = m_windowManager;
            WindowHost.ItemsSource = m_windowManager.Windows;
            m_nativeWindowManager = Win32WindowManagerService.Instance;
            NativeWindowHost.DataContext = m_nativeWindowManager;
            NativeWindowHost.ItemsSource = m_nativeWindowManager.Windows;
            m_nativeWindowManager.WindowsChanged += NativeWindowsChanged;
            m_nativeWindowManager.DesktopFocusRequested += WindowManager_DesktopFocusRequested;
            m_webAppService.AppsChanged += WebAppsChanged;
            m_fileOperations.Windows.CollectionChanged += (s, args) => FileOperationWindowsChanged();
            m_fileOperations.WindowsChanged += (s, args) => FileOperationWindowsChanged();
            m_fileOperations.ActivationRequested += ActivateFileOperation;
            factoryos_10x_shell.Controls.AppInstallerWindow.OpenRequested += OpenAppInstaller;
            Loaded += MainDesktop_Loaded;
            SizeChanged += MainDesktop_SizeChanged;

            LoadBackgroundImage();

            this.PointerPressed += OnPointerPressed;

            TaskbarFrame.NavigationFailed += TaskbarFrame_NavigationFailed;
            TaskbarFrame.Loaded += (sender, args) => LogTaskbarStartup(
                $"TaskbarFrame loaded; content={TaskbarFrame.Content?.GetType().Name ?? "null"}; " +
                $"height={TaskbarFrame.ActualHeight:0.##}; visibility={TaskbarFrame.Visibility}", null);
            try
            {
                if (!TaskbarFrame.Navigate(typeof(Default10xBar)) || TaskbarFrame.Content == null)
                {
                    LogTaskbarStartup("Navigation returned without taskbar content", null);
                    ShowFallbackTaskbar();
                }
                else
                {
                    LogTaskbarStartup("Default10xBar navigation succeeded", null);
                }
            }
            catch (Exception exception)
            {
                LogTaskbarStartup("Navigation threw", exception);
                ShowFallbackTaskbar();
            }

            InitStartOpen();
            InitStartClose();

            InitActionOpen();
            InitActionClose();

            App.MediaPlayer.Source = MediaSource.CreateFromUri(new Uri("ms-appx:///Assets/Sounds/BootUp.wav"));
            App.MediaPlayer.Play();

            AppState.Instance.OnBgChangeButtonVisibilityChanged += UpdateBgChangeButtonVisibility;
            AppState.Instance.OnFilesRequested += FilesRequested;
            AppState.Instance.OnFilesRevealRequested += FilesRevealRequested;
            AppState.Instance.OnFilesStateChanged += FilesStateChanged;
            AppState.Instance.OnFilesActivated += FilesActivated;
            AppState.Instance.OnNotepadRequested += NotepadRequested;
            AppState.Instance.OnNotepadStateChanged += NotepadStateChanged;
            AppState.Instance.OnNotepadActivated += NotepadActivated;
            AppState.Instance.OnSettingsRequested += SettingsRequested;
            AppState.Instance.OnSettingsStateChanged += SettingsStateChanged;
            AppState.Instance.OnSettingsActivated += SettingsActivated;
            AppState.Instance.OnCalculatorRequested += CalculatorRequested;
            AppState.Instance.OnCalculatorStateChanged += CalculatorStateChanged;
            AppState.Instance.OnCalculatorActivated += CalculatorActivated;
            AppState.Instance.OnFirefoxRequested += FirefoxRequested;
            AppState.Instance.OnFirefoxUrlRequested += FirefoxUrlRequested;
            AppState.Instance.OnFirefoxStateChanged += FirefoxStateChanged;
            AppState.Instance.OnFirefoxActivated += FirefoxActivated;
            AppState.Instance.OnWallpaperRequested += SetWallpaper;
            m_windowManager.WindowsChanged += WebWindowsChanged;
            TaskViewGrid.ItemsSource = m_shellWindowCoordinator.Windows;
            SynchronizeFileOperationWindows();
            SynchronizeShellWindows();
            UpdateBgChangeButtonVisibility(AppState.Instance.IsBgChangeButtonVisible);
            if (AppState.Instance.HasPendingFirefoxFile)
                FirefoxRequested();
            if (AppState.Instance.HasPendingFirefoxUrls)
                FirefoxUrlRequested();
        }

        private factoryos_10x_shell.Controls.FilesWindow EnsureFilesWindow()
        {
            if (FilesWindow != null) return FilesWindow;
            Stopwatch clock = Stopwatch.StartNew();
            FilesWindow = new factoryos_10x_shell.Controls.FilesWindow { Visibility = Visibility.Collapsed };
            Canvas.SetLeft(FilesWindow, 160);
            Canvas.SetTop(FilesWindow, 80);
            FilesWindowHost.Children.Add(FilesWindow);
            clock.Stop();
            MainPage.RecordUiOperation("Window.Create.Files", clock.ElapsedMilliseconds);
            return FilesWindow;
        }

        private factoryos_10x_shell.Controls.NotepadWindow EnsureNotepadWindow()
        {
            if (NotepadWindow != null) return NotepadWindow;
            Stopwatch clock = Stopwatch.StartNew();
            NotepadWindow = new factoryos_10x_shell.Controls.NotepadWindow { Visibility = Visibility.Collapsed };
            Canvas.SetLeft(NotepadWindow, 210);
            Canvas.SetTop(NotepadWindow, 105);
            NotepadWindowHost.Children.Add(NotepadWindow);
            clock.Stop();
            MainPage.RecordUiOperation("Window.Create.Notepad", clock.ElapsedMilliseconds);
            return NotepadWindow;
        }

        private factoryos_10x_shell.Controls.SettingsWindow EnsureSettingsWindow()
        {
            if (SettingsWindow != null) return SettingsWindow;
            Stopwatch clock = Stopwatch.StartNew();
            SettingsWindow = new factoryos_10x_shell.Controls.SettingsWindow { Visibility = Visibility.Collapsed };
            Canvas.SetLeft(SettingsWindow, 90);
            Canvas.SetTop(SettingsWindow, 45);
            SettingsWindowHost.Children.Add(SettingsWindow);
            clock.Stop();
            MainPage.RecordUiOperation("Window.Create.Settings", clock.ElapsedMilliseconds);
            return SettingsWindow;
        }

        private factoryos_10x_shell.Controls.CalculatorWindow EnsureCalculatorWindow()
        {
            if (CalculatorWindow != null) return CalculatorWindow;
            Stopwatch clock = Stopwatch.StartNew();
            CalculatorWindow = new factoryos_10x_shell.Controls.CalculatorWindow { Visibility = Visibility.Collapsed };
            Canvas.SetLeft(CalculatorWindow, 185);
            Canvas.SetTop(CalculatorWindow, 70);
            CalculatorWindowHost.Children.Add(CalculatorWindow);
            clock.Stop();
            MainPage.RecordUiOperation("Window.Create.Calculator", clock.ElapsedMilliseconds);
            return CalculatorWindow;
        }

        internal bool IsStrawfoxInputBlocked => m_startManager.IsStartOpen || m_actionManager.IsActionCenterOpen || m_windowManager.IsTaskViewOpen;

        internal System.Threading.Tasks.Task<DesktopMode.Gecko.GeckoWindow> CreateStrawfoxAppWindowAsync(string url)
            => EnsureFirefoxWindow().CreateAppWindowAsync(url);

        private factoryos_10x_shell.Controls.FirefoxWindow EnsureFirefoxWindow()
        {
            if (FirefoxWindow != null) return FirefoxWindow;
            Stopwatch clock = Stopwatch.StartNew();
            FirefoxWindow = new factoryos_10x_shell.Controls.FirefoxWindow { Visibility = Visibility.Collapsed };
            Canvas.SetLeft(FirefoxWindow, 70);
            Canvas.SetTop(FirefoxWindow, 25);
            FirefoxWindow.FullscreenChanged += FirefoxWindow_FullscreenChanged;
            FirefoxWindow.ExternalWindowChanged += FirefoxWindow_ExternalWindowChanged;
            FirefoxWindowHost.Children.Insert(0, FirefoxWindow);
            clock.Stop();
            MainPage.RecordUiOperation("Window.Create.Strawfox", clock.ElapsedMilliseconds);
            return FirefoxWindow;
        }

        private void EnsureStartMenu()
        {
            if (m_startMenuCreated) return;
            Stopwatch clock = Stopwatch.StartNew();
            m_startMenuCreated = true;
            StartMenuFrame.Navigate(typeof(StartMenu));
            clock.Stop();
            MainPage.RecordUiOperation("Shell.Create.StartMenu", clock.ElapsedMilliseconds);
        }

        private void EnsureActionCenter()
        {
            if (m_actionCenterCreated) return;
            Stopwatch clock = Stopwatch.StartNew();
            m_actionCenterCreated = true;
            ActionCenterFrame.Navigate(typeof(ActionCenterHome));
            clock.Stop();
            MainPage.RecordUiOperation("Shell.Create.ActionCenter", clock.ElapsedMilliseconds);
        }

        private void FirefoxWindow_ExternalWindowChanged(bool open)
        {
            if (!open)
            {
                FirefoxExternalWindow.Visibility = Visibility.Collapsed;
                return;
            }

            UIElement content = FirefoxWindow.ExternalContent;
            if (content != null && VisualTreeHelper.GetParent(content) == null)
                FirefoxExternalSurface.Children.Add(content);

            FirefoxExternalWindow.Visibility = Visibility.Visible;
            FirefoxWindow.SetExternalActive(true);
            FirefoxExternalWindow.UpdateLayout();
            FirefoxWindow.SetExternalViewport(
                FirefoxExternalSurface.ActualWidth,
                FirefoxExternalSurface.ActualHeight);
            AppState.Instance.ActivateFirefox();
        }

        private void FirefoxExternalWindow_PointerPressed(object sender, PointerRoutedEventArgs e)
        {
            FirefoxWindow.SetExternalActive(true);
            AppState.Instance.ActivateFirefox();
        }

        private static bool IsButtonSource(DependencyObject source, DependencyObject stop)
        {
            for (DependencyObject current = source; current != null && current != stop;
                 current = VisualTreeHelper.GetParent(current))
            {
                if (current is Button) return true;
            }
            return false;
        }

        private void FirefoxExternalTitleBar_PointerPressed(object sender, PointerRoutedEventArgs e)
        {
            if (m_firefoxExternalMaximized || IsButtonSource(e.OriginalSource as DependencyObject, FirefoxExternalTitleBar))
                return;
            m_firefoxExternalDragging = true;
            m_firefoxExternalPointerStart = e.GetCurrentPoint(FirefoxWindowHost).Position;
            m_firefoxExternalStartBounds = new Rect(
                Canvas.GetLeft(FirefoxExternalWindow), Canvas.GetTop(FirefoxExternalWindow),
                FirefoxExternalWindow.Width, FirefoxExternalWindow.Height);
            FirefoxExternalTitleBar.CapturePointer(e.Pointer);
            e.Handled = true;
        }

        private void FirefoxExternalTitleBar_PointerMoved(object sender, PointerRoutedEventArgs e)
        {
            if (!m_firefoxExternalDragging) return;
            Point current = e.GetCurrentPoint(FirefoxWindowHost).Position;
            Canvas.SetLeft(FirefoxExternalWindow, Math.Max(0,
                m_firefoxExternalStartBounds.X + current.X - m_firefoxExternalPointerStart.X));
            Canvas.SetTop(FirefoxExternalWindow, Math.Max(0,
                m_firefoxExternalStartBounds.Y + current.Y - m_firefoxExternalPointerStart.Y));
        }

        private void FirefoxExternalTitleBar_PointerReleased(object sender, PointerRoutedEventArgs e)
        {
            m_firefoxExternalDragging = false;
            FirefoxExternalTitleBar.ReleasePointerCaptures();
        }

        private void FirefoxExternalMinimize_Click(object sender, RoutedEventArgs e)
        {
            FirefoxExternalWindow.Visibility = Visibility.Collapsed;
            FirefoxWindow.SetExternalActive(false);
        }

        private void FirefoxExternalMaximize_Click(object sender, RoutedEventArgs e)
        {
            if (!m_firefoxExternalMaximized)
            {
                m_firefoxExternalRestoreBounds = new Rect(
                    Canvas.GetLeft(FirefoxExternalWindow), Canvas.GetTop(FirefoxExternalWindow),
                    FirefoxExternalWindow.Width, FirefoxExternalWindow.Height);
                Canvas.SetLeft(FirefoxExternalWindow, 0);
                Canvas.SetTop(FirefoxExternalWindow, 0);
                FirefoxExternalWindow.Width = FirefoxWindowHost.ActualWidth;
                FirefoxExternalWindow.Height = FirefoxWindowHost.ActualHeight;
                FirefoxExternalMaximizeGlyph.Text = "\uE923";
            }
            else
            {
                Canvas.SetLeft(FirefoxExternalWindow, m_firefoxExternalRestoreBounds.X);
                Canvas.SetTop(FirefoxExternalWindow, m_firefoxExternalRestoreBounds.Y);
                FirefoxExternalWindow.Width = m_firefoxExternalRestoreBounds.Width;
                FirefoxExternalWindow.Height = m_firefoxExternalRestoreBounds.Height;
                FirefoxExternalMaximizeGlyph.Text = "\uE922";
            }
            m_firefoxExternalMaximized = !m_firefoxExternalMaximized;
        }

        private void FirefoxExternalClose_Click(object sender, RoutedEventArgs e)
        {
            FirefoxExternalWindow.Visibility = Visibility.Collapsed;
            FirefoxWindow.CloseExternalWindow();
        }

        private void FirefoxExternalSurface_SizeChanged(object sender, SizeChangedEventArgs e) =>
            FirefoxWindow?.SetExternalViewport(e.NewSize.Width, e.NewSize.Height);

        private void FirefoxExternalResize_PointerPressed(object sender, PointerRoutedEventArgs e)
        {
            if (m_firefoxExternalMaximized) return;
            m_firefoxExternalResizing = true;
            m_firefoxExternalResizeCorner = factoryos_10x_shell.Controls.WindowResize.CornerFromTag(
                (sender as FrameworkElement)?.Tag);
            m_firefoxExternalPointerStart = e.GetCurrentPoint(null).Position;
            m_firefoxExternalStartBounds = new Rect(
                Canvas.GetLeft(FirefoxExternalWindow), Canvas.GetTop(FirefoxExternalWindow),
                FirefoxExternalWindow.Width, FirefoxExternalWindow.Height);
            (sender as UIElement)?.CapturePointer(e.Pointer);
            e.Handled = true;
        }

        private void FirefoxExternalResize_PointerMoved(object sender, PointerRoutedEventArgs e)
        {
            if (!m_firefoxExternalResizing) return;
            Rect bounds = factoryos_10x_shell.Controls.WindowResize.Calculate(
                m_firefoxExternalResizeCorner, m_firefoxExternalPointerStart,
                e.GetCurrentPoint(null).Position, m_firefoxExternalStartBounds, 480, 320);
            Canvas.SetLeft(FirefoxExternalWindow, bounds.X);
            Canvas.SetTop(FirefoxExternalWindow, bounds.Y);
            FirefoxExternalWindow.Width = bounds.Width;
            FirefoxExternalWindow.Height = bounds.Height;
        }

        private void FirefoxExternalResize_PointerReleased(object sender, PointerRoutedEventArgs e)
        {
            m_firefoxExternalResizing = false;
            (sender as UIElement)?.ReleasePointerCaptures();
        }

        private void TaskbarFrame_NavigationFailed(object sender, NavigationFailedEventArgs e)
        {
            LogTaskbarStartup("Default10xBar navigation failed", e.Exception);
            // The desktop can still offer access to Start and Settings.
            e.Handled = true;
        }

        private void LogTaskbarStartup(string stage, Exception exception)
        {
            while (exception?.InnerException != null)
                exception = exception.InnerException;
            string detail = exception == null
                ? "none"
                : $"{exception.GetType().FullName} HRESULT=0x{exception.HResult:X8}";
            string line = $"{DateTimeOffset.UtcNow:O} {stage}: {detail}";
            Debug.WriteLine("[ShellStartup] " + line);
            try
            {
                string path = Path.Combine(ApplicationData.Current.LocalFolder.Path, "Shell-startup.log");
                _ = Task.Run(() =>
                {
                    try { File.AppendAllText(path, line + System.Environment.NewLine); }
                    catch (Exception) { /* Logging must never prevent startup. */ }
                });
            }
            catch (Exception) { /* Logging must never prevent startup. */ }
        }

        private void ShowFallbackTaskbar()
        {
            var bar = new Grid
            {
                Height = 50,
                VerticalAlignment = VerticalAlignment.Bottom,
                Background = new SolidColorBrush(Windows.UI.Color.FromArgb(255, 36, 36, 36))
            };
            var buttons = new StackPanel
            {
                Orientation = Orientation.Horizontal,
                HorizontalAlignment = HorizontalAlignment.Center
            };
            var start = new Button { Content = "Start", Width = 90, Height = 50 };
            start.Click += (sender, args) =>
                m_startManager.RequestStartVisibilityChange(!m_startManager.IsStartOpen);
            var settings = new Button { Content = "Settings", Width = 90, Height = 50 };
            settings.Click += (sender, args) => AppState.Instance.RequestSettingsOpen();
            buttons.Children.Add(start);
            buttons.Children.Add(settings);
            bar.Children.Add(buttons);
            TaskbarFrame.Content = bar;
        }

        private void UpdateBgChangeButtonVisibility(bool isVisible)
        {
            BgChangebutton.Visibility = isVisible ? Visibility.Visible : Visibility.Collapsed;

        }

        private void FilesRequested(bool resetHome)
        {
            EnsureFilesWindow();
            if (resetHome) FilesWindow.OpenHome();
            else FilesWindow.Restore();
        }

        private void FilesRevealRequested(string path)
        {
            EnsureFilesWindow();
            FilesWindow.OpenFolderAndSelect(path);
        }

        private void FilesActivated()
        {
            ActivateShellWindow(ShellWindowKind.Files, FilesWindowIdentity);
        }

        private void NotepadRequested()
        {
            EnsureNotepadWindow();
            StorageFile file = AppState.Instance.TakePendingNotepadFile();
            if (file == null) NotepadWindow.Open();
            else _ = NotepadWindow.OpenFileAsync(file);
        }

        private void NotepadActivated()
        {
            ActivateShellWindow(ShellWindowKind.Notepad, NotepadWindowIdentity);
        }

        private void SettingsRequested() => EnsureSettingsWindow().Open();

        private void SettingsActivated()
        {
            ActivateShellWindow(ShellWindowKind.Settings, SettingsWindowIdentity);
        }

        private void CalculatorRequested() => EnsureCalculatorWindow().Open();

        private void CalculatorActivated()
        {
            ActivateShellWindow(ShellWindowKind.Calculator, CalculatorWindowIdentity);
        }

        private void FirefoxUrlRequested()
        {
            var browser = EnsureFirefoxWindow();
            string url;
            while ((url = AppState.Instance.TakePendingFirefoxUrl()) != null)
                browser.OpenUrl(url);
        }

        private void FirefoxRequested()
        {
            EnsureFirefoxWindow();
            StorageFile file = AppState.Instance.TakePendingFirefoxFile();
            if (file == null) FirefoxWindow.Open();
            else FirefoxWindow.OpenFile(file);
        }

        private void FirefoxActivated()
        {
            ActivateShellWindow(ShellWindowKind.Firefox, FirefoxWindowIdentity);
            FirefoxWindow?.RestoreExternalWindowIfOpen();
        }

        private void FirefoxWindow_FullscreenChanged(bool fullscreen)
        {
            if (fullscreen)
            {
                m_taskbarVisibilityBeforeFirefoxFullscreen = TaskbarFrame.Visibility;
                TaskbarFrame.Visibility = Visibility.Collapsed;
            }
            else
            {
                TaskbarFrame.Visibility = m_taskbarVisibilityBeforeFirefoxFullscreen;
            }
            Canvas.SetZIndex(FirefoxWindowHost, fullscreen ? 40 : 2);
        }

        private void WebWindowsChanged(object sender, EventArgs e)
        {
            if (m_coordinatingActivation) return;
            SynchronizeShellWindows();

            WebAppWindowModel active = m_windowManager.Windows
                .Where(window => window.IsActive && window.Visibility == Visibility.Visible)
                .OrderByDescending(window => window.ZIndex)
                .FirstOrDefault();
            if (active != null) ActivateShellWindow(ShellWindowKind.WebApp, active);
            else RestoreMostRecentWindow();
        }

        private void NativeWindowsChanged(object sender, EventArgs e)
        {
            if (m_coordinatingActivation) return;
            SynchronizeShellWindows();

            Win32WindowModel active = m_nativeWindowManager.Windows
                .Where(window => window.IsActive && window.Visibility == Visibility.Visible)
                .OrderByDescending(window => window.ZIndex)
                .FirstOrDefault();
            if (active != null) ActivateShellWindow(ShellWindowKind.Win32App, active);
            else RestoreMostRecentWindow();
        }

        private void FilesStateChanged()
        {
            if (!AppState.Instance.IsFilesOpen || AppState.Instance.IsFilesMinimized)
                ReleaseInternalTextInputFocus();
            SynchronizeShellWindows();
            RestoreMostRecentWindow();
        }

        private void NotepadStateChanged()
        {
            if (!AppState.Instance.IsNotepadOpen || AppState.Instance.IsNotepadMinimized)
                ReleaseInternalTextInputFocus();
            SynchronizeShellWindows();
            RestoreMostRecentWindow();
        }

        private void SettingsStateChanged()
        {
            if (!AppState.Instance.IsSettingsOpen || AppState.Instance.IsSettingsMinimized)
                ReleaseInternalTextInputFocus();
            SynchronizeShellWindows();
            RestoreMostRecentWindow();
        }

        private void CalculatorStateChanged()
        {
            if (!AppState.Instance.IsCalculatorOpen || AppState.Instance.IsCalculatorMinimized)
                ReleaseInternalTextInputFocus();
            SynchronizeShellWindows();
            RestoreMostRecentWindow();
        }

        private void FirefoxStateChanged()
        {
            if (!AppState.Instance.IsFirefoxOpen || AppState.Instance.IsFirefoxMinimized)
                ReleaseInternalTextInputFocus();
            SynchronizeShellWindows();
            RestoreMostRecentWindow();
        }

        private async void ReleaseInternalTextInputFocus()
        {
            // Collapsing a subtree while one of its text controls still owns focus
            // makes Xbox transfer that focus through the text-input path.  That
            // delayed transfer can summon the virtual keyboard after the window
            // has already disappeared.  Give focus to a non-text desktop control
            // first and dismiss both the current and delayed InputPane requests.
            DesktopFocusSink.Focus(FocusState.Programmatic);
            HideOnScreenKeyboard();
            await Task.Delay(100);
            HideOnScreenKeyboard();
        }

        private static void HideOnScreenKeyboard()
        {
            try { InputPane.GetForCurrentView().TryHide(); }
            catch { }
        }

        private void WebAppsChanged(object sender, EventArgs e) => SynchronizeShellWindows();

        private void SynchronizeFileOperationWindows()
        {
            foreach (FileOperationModel removed in m_operationWindows.Keys.Where(item => !m_fileOperations.Windows.Contains(item)).ToList())
            {
                FileOperationWindowHost.Children.Remove(m_operationWindows[removed]);
                m_operationWindows.Remove(removed);
                m_shellWindowCoordinator.Remove(ShellWindowKind.FileOperation, removed);
            }
            foreach (FileOperationModel operation in m_fileOperations.Windows)
            {
                if (!m_operationWindows.TryGetValue(operation, out var window))
                {
                    window = new factoryos_10x_shell.Controls.FileOperationWindow { DataContext = operation };
                    window.SetBinding(UIElement.VisibilityProperty, new Binding
                    {
                        Source = operation, Path = new PropertyPath(nameof(FileOperationModel.Visibility)), Mode = BindingMode.OneWay
                    });
                    window.AddHandler(UIElement.PointerPressedEvent, new PointerEventHandler((s, e) => m_fileOperations.Activate(operation)), true);
                    Canvas.SetLeft(window, Math.Max(0, (FileOperationWindowHost.ActualWidth - 480) / 2 + m_operationWindows.Count * 22));
                    Canvas.SetTop(window, Math.Max(0, (FileOperationWindowHost.ActualHeight - 290) / 3 + m_operationWindows.Count * 22));
                    Canvas.SetZIndex(window, ++m_operationZIndex);
                    FileOperationWindowHost.Children.Add(window);
                    m_operationWindows.Add(operation, window);
                }
                window.MaxWidth = Math.Max(320, FileOperationWindowHost.ActualWidth);
                window.MaxHeight = Math.Max(140, FileOperationWindowHost.ActualHeight);
                Canvas.SetLeft(window, Math.Min(Canvas.GetLeft(window), Math.Max(0, FileOperationWindowHost.ActualWidth - window.Width)));
                Canvas.SetTop(window, Math.Min(Canvas.GetTop(window), Math.Max(0, FileOperationWindowHost.ActualHeight - 40)));
            }
        }

        private void FileOperationWindowsChanged()
        {
            ShellWindowReference active = m_shellWindowCoordinator.ActiveWindow;
            if (active?.Kind == ShellWindowKind.FileOperation && !IsWindowAvailable(active))
                ReleaseInternalTextInputFocus();
            SynchronizeFileOperationWindows();
            RestoreMostRecentWindow();
            SynchronizeShellWindows();
        }

        private void ActivateFileOperation(FileOperationModel operation)
        {
            if (operation == null) return;
            if (m_shellWindowCoordinator.IsActive(ShellWindowKind.FileOperation, operation)) return;
            SynchronizeFileOperationWindows();
            if (m_operationWindows.TryGetValue(operation, out var window))
                Canvas.SetZIndex(window, ++m_operationZIndex);
            ActivateShellWindow(ShellWindowKind.FileOperation, operation);
        }

        private void OpenAppInstaller(StorageFile package)
        {
            var existing = m_installers.FirstOrDefault(item => item.OpensPackage(package));
            if (existing != null) { ActivateInstaller(existing); return; }
            var window = new factoryos_10x_shell.Controls.AppInstallerWindow(package);
            m_installers.Add(window);
            window.MaxWidth = Math.Max(320, AppInstallerWindowHost.ActualWidth);
            window.MaxHeight = Math.Max(180, AppInstallerWindowHost.ActualHeight);
            Canvas.SetLeft(window, Math.Max(0, (AppInstallerWindowHost.ActualWidth - window.Width) / 2));
            Canvas.SetTop(window, Math.Max(0, (AppInstallerWindowHost.ActualHeight - window.Height) / 2));
            window.ActivationRequested += () => ActivateInstaller(window);
            window.Changed += () => { RestoreMostRecentWindow(); SynchronizeShellWindows(); };
            window.CloseRequested += () => {
                m_installers.Remove(window);
                AppInstallerWindowHost.Children.Remove(window);
                m_shellWindowCoordinator.Remove(ShellWindowKind.AppInstaller, window);
                RestoreMostRecentWindow();
                SynchronizeShellWindows();
            };
            AppInstallerWindowHost.Children.Add(window);
            ActivateInstaller(window);
        }

        private void ActivateInstaller(factoryos_10x_shell.Controls.AppInstallerWindow window)
        {
            if (!m_installers.Contains(window)) return;
            window.Visibility = Visibility.Visible;
            Canvas.SetZIndex(window, ++m_installerZIndex);
            ActivateShellWindow(ShellWindowKind.AppInstaller, window);
        }

        private void ActivateShellWindow(ShellWindowKind kind, object identity)
        {
            if (identity == null) return;
            Stopwatch clock = Stopwatch.StartNew();

            // PointerPressed bubbles from the entire contents of an internal
            // window. Do not run a complete synchronization cycle when the
            // user merely clicks inside the window that is already active.
            m_shellWindowCoordinator.Prune(IsWindowRegistered);
            if (m_shellWindowCoordinator.IsActive(kind, identity))
            {
                clock.Stop();
                MainPage.RecordUiOperation("Windowing.ActivateAlreadyActive", clock.ElapsedMilliseconds);
                return;
            }

            m_coordinatingActivation = true;
            try
            {
                m_shellWindowCoordinator.Activate(kind, identity);
                if (kind != ShellWindowKind.WebApp) m_windowManager.DeactivateAll();
                if (kind != ShellWindowKind.Win32App) m_nativeWindowManager.DeactivateAll();
                ApplyWindowLayers(kind);
                SynchronizeShellWindows();
            }
            finally
            {
                m_coordinatingActivation = false;
                clock.Stop();
                MainPage.RecordUiOperation("Windowing.Activate." + kind, clock.ElapsedMilliseconds);
            }
        }

        private void RestoreMostRecentWindow()
        {
            if (m_coordinatingActivation) return;

            m_shellWindowCoordinator.Prune(IsWindowRegistered);
            ShellWindowReference active = m_shellWindowCoordinator.ActiveWindow;
            if (active != null && IsWindowAvailable(active))
            {
                ApplyWindowLayers(active.Kind);
                return;
            }

            m_shellWindowCoordinator.ClearActive();
            ShellWindowReference next = m_shellWindowCoordinator.GetMostRecentAvailable(IsWindowAvailable);
            if (next == null)
            {
                ApplyWindowLayers(null);
                return;
            }

            ActivateWindowReference(next);
        }

        private bool IsWindowRegistered(ShellWindowReference window)
        {
            switch (window.Kind)
            {
                case ShellWindowKind.AppInstaller:
                    return window.Identity is factoryos_10x_shell.Controls.AppInstallerWindow installer && m_installers.Contains(installer);
                case ShellWindowKind.FileOperation:
                    return window.Identity is FileOperationModel operation && m_fileOperations.Windows.Contains(operation);
                case ShellWindowKind.WebApp:
                    return window.Identity is WebAppWindowModel webWindow && m_windowManager.Windows.Contains(webWindow);
                case ShellWindowKind.Win32App:
                    return window.Identity is Win32WindowModel nativeWindow && m_nativeWindowManager.Windows.Contains(nativeWindow);
                case ShellWindowKind.Files:
                    return AppState.Instance.IsFilesOpen;
                case ShellWindowKind.Notepad:
                    return AppState.Instance.IsNotepadOpen;
                case ShellWindowKind.Settings:
                    return AppState.Instance.IsSettingsOpen;
                case ShellWindowKind.Calculator:
                    return AppState.Instance.IsCalculatorOpen;
                case ShellWindowKind.Firefox:
                    return AppState.Instance.IsFirefoxOpen;
                default:
                    return false;
            }
        }

        private bool IsWindowAvailable(ShellWindowReference window)
        {
            if (!IsWindowRegistered(window)) return false;

            switch (window.Kind)
            {
                case ShellWindowKind.AppInstaller:
                    return ((factoryos_10x_shell.Controls.AppInstallerWindow)window.Identity).Visibility == Visibility.Visible;
                case ShellWindowKind.FileOperation:
                    return ((FileOperationModel)window.Identity).Visibility == Visibility.Visible;
                case ShellWindowKind.WebApp:
                    return ((WebAppWindowModel)window.Identity).Visibility == Visibility.Visible;
                case ShellWindowKind.Win32App:
                    return ((Win32WindowModel)window.Identity).Visibility == Visibility.Visible;
                case ShellWindowKind.Files:
                    return !AppState.Instance.IsFilesMinimized;
                case ShellWindowKind.Notepad:
                    return !AppState.Instance.IsNotepadMinimized;
                case ShellWindowKind.Settings:
                    return !AppState.Instance.IsSettingsMinimized;
                case ShellWindowKind.Calculator:
                    return !AppState.Instance.IsCalculatorMinimized;
                case ShellWindowKind.Firefox:
                    return !AppState.Instance.IsFirefoxMinimized;
                default:
                    return false;
            }
        }

        private void ActivateWindowReference(ShellWindowReference window)
        {
            switch (window.Kind)
            {
                case ShellWindowKind.AppInstaller:
                    ActivateInstaller((factoryos_10x_shell.Controls.AppInstallerWindow)window.Identity);
                    break;
                case ShellWindowKind.FileOperation:
                    m_fileOperations.Activate((FileOperationModel)window.Identity);
                    break;
                case ShellWindowKind.WebApp:
                    m_windowManager.Activate((WebAppWindowModel)window.Identity);
                    break;
                case ShellWindowKind.Win32App:
                    m_nativeWindowManager.Activate((Win32WindowModel)window.Identity);
                    break;
                case ShellWindowKind.Files:
                    AppState.Instance.RequestFilesOpen();
                    break;
                case ShellWindowKind.Notepad:
                    AppState.Instance.RequestNotepadOpen();
                    break;
                case ShellWindowKind.Settings:
                    AppState.Instance.RequestSettingsOpen();
                    break;
                case ShellWindowKind.Calculator:
                    AppState.Instance.RequestCalculatorOpen();
                    break;
                case ShellWindowKind.Firefox:
                    AppState.Instance.RequestFirefoxOpen();
                    break;
            }
        }

        private void ApplyWindowLayers(ShellWindowKind? activeKind)
        {
            Canvas.SetZIndex(WindowHost, activeKind == ShellWindowKind.WebApp ? 2 : 1);
            Canvas.SetZIndex(NativeWindowHost, activeKind == ShellWindowKind.Win32App ? 2 : 1);
            Canvas.SetZIndex(FilesWindowHost, activeKind == ShellWindowKind.Files ? 2 : 1);
            Canvas.SetZIndex(AppInstallerWindowHost, activeKind == ShellWindowKind.AppInstaller ? 2 : 1);
            Canvas.SetZIndex(FileOperationWindowHost, activeKind == ShellWindowKind.FileOperation ? 2 : 1);
            Canvas.SetZIndex(NotepadWindowHost, activeKind == ShellWindowKind.Notepad ? 2 : 1);
            Canvas.SetZIndex(SettingsWindowHost, activeKind == ShellWindowKind.Settings ? 2 : 1);
            Canvas.SetZIndex(CalculatorWindowHost, activeKind == ShellWindowKind.Calculator ? 2 : 1);
            Canvas.SetZIndex(
                FirefoxWindowHost,
                FirefoxWindow?.IsFullscreen == true ? 40 :
                activeKind == ShellWindowKind.Firefox ? 2 : 1);
            FirefoxWindow?.SetWindowActive(activeKind == ShellWindowKind.Firefox);
        }

        private void SynchronizeShellWindows()
        {
            Stopwatch clock = Stopwatch.StartNew();
            var descriptors = new List<ShellWindowDescriptor>();

            foreach (WebAppWindowModel window in m_windowManager.Windows)
            {
                descriptors.Add(new ShellWindowDescriptor
                {
                    Kind = ShellWindowKind.WebApp,
                    Identity = window,
                    Title = window.App?.Name ?? "Web app",
                    Subtitle = "Web app",
                    FallbackGlyph = "\uE774",
                    IconSource = CreateImageSource(window.App?.IconUri),
                    IsActive = window.IsActive && window.Visibility == Visibility.Visible,
                    IsMinimized = window.Visibility != Visibility.Visible
                });
            }

            foreach (Win32WindowModel window in m_nativeWindowManager.Windows)
            {
                descriptors.Add(new ShellWindowDescriptor
                {
                    Kind = ShellWindowKind.Win32App,
                    Identity = window,
                    Title = window.DisplayName,
                    Subtitle = string.IsNullOrWhiteSpace(window.Status) ? "Desktop app" : window.Status,
                    FallbackGlyph = "\uE7C3",
                    IconSource = window.IconSource,
                    IsActive = window.IsActive && window.Visibility == Visibility.Visible,
                    IsMinimized = window.Visibility != Visibility.Visible
                });
            }

            AddBuiltInWindowDescriptor(
                descriptors,
                ShellWindowKind.Files,
                FilesWindowIdentity,
                "Files",
                "File manager",
                "\uE8B7",
                m_filesIcon,
                AppState.Instance.IsFilesOpen,
                AppState.Instance.IsFilesMinimized);
            AddBuiltInWindowDescriptor(
                descriptors,
                ShellWindowKind.Notepad,
                NotepadWindowIdentity,
                "Notepad",
                "Text editor",
                "\uE70B",
                m_notepadIcon,
                AppState.Instance.IsNotepadOpen,
                AppState.Instance.IsNotepadMinimized);
            AddBuiltInWindowDescriptor(
                descriptors,
                ShellWindowKind.Settings,
                SettingsWindowIdentity,
                "Settings",
                "System settings",
                "\uE713",
                m_settingsIcon,
                AppState.Instance.IsSettingsOpen,
                AppState.Instance.IsSettingsMinimized);
            AddBuiltInWindowDescriptor(
                descriptors,
                ShellWindowKind.Calculator,
                CalculatorWindowIdentity,
                "Calculator",
                "Standard calculator",
                "\uE8EF",
                m_calculatorIcon,
                AppState.Instance.IsCalculatorOpen,
                AppState.Instance.IsCalculatorMinimized);
            AddBuiltInWindowDescriptor(
                descriptors,
                ShellWindowKind.Firefox,
                FirefoxWindowIdentity,
                "Strawfox",
                "Web browser",
                "\uE774",
                m_firefoxIcon,
                AppState.Instance.IsFirefoxOpen,
                AppState.Instance.IsFirefoxMinimized);

            foreach (FileOperationModel operation in m_fileOperations.Windows)
            {
                descriptors.Add(new ShellWindowDescriptor
                {
                    Kind = ShellWindowKind.FileOperation, Identity = operation,
                    Title = operation.Title, Subtitle = operation.IsRunning ? "File operation" : operation.Status,
                    IconSource = m_filesIcon, FallbackGlyph = "\uE8B7",
                    IsActive = m_shellWindowCoordinator.IsActive(ShellWindowKind.FileOperation, operation),
                    IsMinimized = operation.Visibility != Visibility.Visible
                });
            }
            foreach (var installer in m_installers)
                descriptors.Add(new ShellWindowDescriptor {
                    Kind = ShellWindowKind.AppInstaller, Identity = installer,
                    Title = "App Installer", Subtitle = installer.PackageTitle, FallbackGlyph = "\uE7B8",
                    IconSource = new BitmapImage(new Uri("ms-appx:///Assets/Files/FluentIcons/Package.png")),
                    IsActive = m_shellWindowCoordinator.IsActive(ShellWindowKind.AppInstaller, installer),
                    IsMinimized = installer.Visibility != Visibility.Visible
                });
            m_shellWindowCoordinator.Synchronize(descriptors);
            TaskViewEmptyState.Visibility = descriptors.Count == 0
                ? Visibility.Visible
                : Visibility.Collapsed;
            clock.Stop();
            MainPage.RecordUiOperation("Windowing.Synchronize", clock.ElapsedMilliseconds);
        }

        private void AddBuiltInWindowDescriptor(
            ICollection<ShellWindowDescriptor> descriptors,
            ShellWindowKind kind,
            object identity,
            string title,
            string subtitle,
            string fallbackGlyph,
            ImageSource iconSource,
            bool isOpen,
            bool isMinimized)
        {
            if (!isOpen) return;
            descriptors.Add(new ShellWindowDescriptor
            {
                Kind = kind,
                Identity = identity,
                Title = title,
                Subtitle = subtitle,
                FallbackGlyph = fallbackGlyph,
                IconSource = iconSource,
                IsActive = m_shellWindowCoordinator.IsActive(kind, identity) && !isMinimized,
                IsMinimized = isMinimized
            });
        }

        private static ImageSource CreateImageSource(string uri)
        {
            return !string.IsNullOrWhiteSpace(uri) && Uri.TryCreate(uri, UriKind.Absolute, out Uri iconUri)
                ? new BitmapImage(iconUri)
                : null;
        }

            private async void LoadBackgroundImage()
        {
            Windows.Storage.ApplicationDataContainer localSettings = Windows.Storage.ApplicationData.Current.LocalSettings;
            if (localSettings.Values.ContainsKey("backgroundImagePath"))
            {
                string imagePath = localSettings.Values["backgroundImagePath"].ToString();
                if (!string.IsNullOrEmpty(imagePath))
                {
                    if (imagePath.StartsWith("ms-appx:", StringComparison.OrdinalIgnoreCase))
                    {
                        BackgroundWallpaper.Source = new BitmapImage(new Uri(imagePath));
                        return;
                    }
                    var file = await StorageFile.GetFileFromPathAsync(imagePath);
                    if (file != null)
                    {
                        var bitmapImage = new BitmapImage();
                        using (var stream = await file.OpenAsync(FileAccessMode.Read))
                        {
                            await bitmapImage.SetSourceAsync(stream);
                        }
                        BackgroundWallpaper.Source = bitmapImage;
                    }
                }
            }
        }

        private async void SetWallpaper(string imagePath)
        {
            if (string.IsNullOrWhiteSpace(imagePath)) return;
            if (imagePath.StartsWith("ms-appx:", StringComparison.OrdinalIgnoreCase))
            {
                BackgroundWallpaper.Source = new BitmapImage(new Uri(imagePath));
                return;
            }

            try
            {
                StorageFolder wallpaperFolder = await ApplicationData.Current.LocalFolder.CreateFolderAsync(
                    "Wallpapers", CreationCollisionOption.OpenIfExists);
                StorageFile saved;

                if (Uri.TryCreate(imagePath, UriKind.Absolute, out Uri remoteUri) &&
                    (remoteUri.Scheme == Uri.UriSchemeHttp || remoteUri.Scheme == Uri.UriSchemeHttps))
                {
                    // Do not make Gecko synchronously decode and re-encode a
                    // potentially 4K image on its main thread. Download the
                    // original resource through WinRT and yield the Shell UI
                    // while network and storage complete.
                    using (var client = new Windows.Web.Http.HttpClient())
                    using (Windows.Web.Http.HttpResponseMessage response =
                        await client.GetAsync(remoteUri, Windows.Web.Http.HttpCompletionOption.ResponseHeadersRead))
                    {
                        response.EnsureSuccessStatusCode();
                        string mediaType = response.Content.Headers.ContentType?.MediaType;
                        string remoteExtension = string.Equals(mediaType, "image/jpeg", StringComparison.OrdinalIgnoreCase)
                            ? ".jpg"
                            : string.Equals(mediaType, "image/bmp", StringComparison.OrdinalIgnoreCase) ? ".bmp" : ".png";
                        saved = await wallpaperFolder.CreateFileAsync(
                            "Desktop Background" + remoteExtension, CreationCollisionOption.ReplaceExisting);
                        using (var input = await response.Content.ReadAsInputStreamAsync())
                        using (var output = await saved.OpenAsync(FileAccessMode.ReadWrite))
                        {
                            output.Size = 0;
                            await Windows.Storage.Streams.RandomAccessStream.CopyAsync(input, output);
                            await output.FlushAsync();
                        }
                    }
                }
                else
                {
                    StorageFile source = await StorageFile.GetFileFromPathAsync(imagePath);
                    string extension = source.FileType;
                    if (!string.Equals(extension, ".png", StringComparison.OrdinalIgnoreCase) &&
                        !string.Equals(extension, ".jpg", StringComparison.OrdinalIgnoreCase) &&
                        !string.Equals(extension, ".jpeg", StringComparison.OrdinalIgnoreCase) &&
                        !string.Equals(extension, ".bmp", StringComparison.OrdinalIgnoreCase))
                    {
                        extension = ".png";
                    }
                    saved = await source.CopyAsync(
                        wallpaperFolder, "Desktop Background" + extension, NameCollisionOption.ReplaceExisting);

                    // Gecko exports a unique PNG into the package's temporary
                    // directory. Once the durable LocalState copy exists it is
                    // safe to remove that hand-off file.
                    string temporaryPath = ApplicationData.Current.TemporaryFolder.Path;
                    if (source.Path.StartsWith(temporaryPath + "\\", StringComparison.OrdinalIgnoreCase))
                    {
                        try { await source.DeleteAsync(StorageDeleteOption.PermanentDelete); }
                        catch { }
                    }
                }

                ApplicationData.Current.LocalSettings.Values["backgroundImagePath"] = saved.Path;
                var bitmapImage = new BitmapImage();
                using (var stream = await saved.OpenAsync(FileAccessMode.Read))
                    await bitmapImage.SetSourceAsync(stream);
                BackgroundWallpaper.Source = bitmapImage;
            }
            catch
            {
                // Keep the current wallpaper if Gecko's temporary image is no
                // longer available or could not be decoded.
            }
        }

        public Image BackgroundWallpaperControl => BackgroundWallpaper;

        private void MainDesktop_SizeChanged(object sender, SizeChangedEventArgs e)
        {
            UpdateStartMenuAlignment();
            UpdateWorkspaceBounds();
        }

        private async void MainDesktop_Loaded(object sender, RoutedEventArgs e)
        {
            UpdateWorkspaceBounds();
            if (m_startupUpdateCheckStarted || !UpdateCheckService.IsAutomaticCheckEnabled) return;

            m_startupUpdateCheckStarted = true;
            await UpdateCheckService.CheckAsync(true);
        }

        private void WindowManager_DesktopFocusRequested(object sender, EventArgs e)
        {
            DesktopFocusSink.Focus(FocusState.Programmatic);
        }

        private void WindowManager_TaskViewChanged(object sender, EventArgs e)
        {
            Canvas.SetZIndex(TaskViewOverlay, m_windowManager.IsTaskViewOpen ? 20 : 0);
            TaskViewOverlay.Visibility = m_windowManager.IsTaskViewOpen
                ? Visibility.Visible
                : Visibility.Collapsed;
            SynchronizeShellWindows();
            UpdateNativeInputSuppression();
        }

        private async void TaskViewGrid_ItemClick(object sender, ItemClickEventArgs e)
        {
            if (!(e.ClickedItem is ShellWindowItem window)) return;
            m_windowManager.CloseTaskView();
            await Dispatcher.RunAsync(CoreDispatcherPriority.Normal, () => ActivateTaskViewWindow(window));
        }

        private void ActivateTaskViewWindow(ShellWindowItem window)
        {
            switch (window.Kind)
            {
                case ShellWindowKind.AppInstaller:
                    ActivateInstaller((factoryos_10x_shell.Controls.AppInstallerWindow)window.Identity);
                    break;
                case ShellWindowKind.FileOperation:
                    m_fileOperations.Activate((FileOperationModel)window.Identity);
                    break;
                case ShellWindowKind.WebApp:
                    m_windowManager.Activate((WebAppWindowModel)window.Identity);
                    break;
                case ShellWindowKind.Win32App:
                    m_nativeWindowManager.Activate((Win32WindowModel)window.Identity);
                    break;
                case ShellWindowKind.Files:
                    AppState.Instance.RequestFilesOpen();
                    break;
                case ShellWindowKind.Notepad:
                    AppState.Instance.RequestNotepadOpen();
                    break;
                case ShellWindowKind.Settings:
                    AppState.Instance.RequestSettingsOpen();
                    break;
                case ShellWindowKind.Calculator:
                    AppState.Instance.RequestCalculatorOpen();
                    break;
                case ShellWindowKind.Firefox:
                    AppState.Instance.RequestFirefoxOpen();
                    break;
            }
        }

        private void TaskViewClose_Click(object sender, RoutedEventArgs e)
        {
            if (!((sender as FrameworkElement)?.DataContext is ShellWindowItem window)) return;

            switch (window.Kind)
            {
                case ShellWindowKind.AppInstaller:
                    ((factoryos_10x_shell.Controls.AppInstallerWindow)window.Identity).CloseWindow();
                    break;
                case ShellWindowKind.FileOperation:
                    m_fileOperations.Close((FileOperationModel)window.Identity);
                    break;
                case ShellWindowKind.WebApp:
                    m_windowManager.Close((WebAppWindowModel)window.Identity);
                    break;
                case ShellWindowKind.Win32App:
                    m_nativeWindowManager.Close((Win32WindowModel)window.Identity);
                    break;
                case ShellWindowKind.Files:
                    FilesWindow?.CloseFromTaskView();
                    break;
                case ShellWindowKind.Notepad:
                    NotepadWindow?.CloseFromTaskView();
                    break;
                case ShellWindowKind.Settings:
                    SettingsWindow?.CloseFromTaskView();
                    break;
                case ShellWindowKind.Calculator:
                    CalculatorWindow?.CloseFromTaskView();
                    break;
                case ShellWindowKind.Firefox:
                    FirefoxWindow?.CloseFromTaskView();
                    break;
            }
        }

        private void TaskViewOverlay_PointerPressed(object sender, PointerRoutedEventArgs e)
        {
            if (e.OriginalSource == TaskViewOverlay)
                m_windowManager.CloseTaskView();
        }

        private readonly DispatcherTimer m_taskbarHideTimer = new DispatcherTimer { Interval = TimeSpan.FromMilliseconds(600) };
        private bool m_pointerOverTaskbar;
        private void TaskbarPreferencesChanged()
        {
            UpdateStartMenuAlignment();
            double bottom = AppState.Instance.TaskbarAutoHide ? 0 : 50;
            foreach (var host in new[] { AppInstallerWindowHost, FilesWindowHost, FileOperationWindowHost, NotepadWindowHost, SettingsWindowHost, CalculatorWindowHost, FirefoxWindowHost })
                host.Margin = new Thickness(0, 0, 0, bottom);
            m_taskbarHideTimer.Stop();
            SetTaskbarRevealed(!AppState.Instance.TaskbarAutoHide);
            UpdateWorkspaceBounds();
        }
        private void UpdateStartMenuAlignment()
        {
            bool alignLeft = AppState.Instance.TaskbarAlignLeft;
            StartMenuFrame.HorizontalAlignment = alignLeft
                ? HorizontalAlignment.Left : HorizontalAlignment.Stretch;
            // Preserve the stretched menu's width when anchoring it to the
            // left, including on desktops narrower than its maximum width.
            StartMenuFrame.Width = alignLeft
                ? Math.Max(0, Math.Min(StartMenuFrame.MaxWidth,
                    MainGrid.ActualWidth - StartMenuFrame.Margin.Left - StartMenuFrame.Margin.Right))
                : double.NaN;
        }
        private void SetTaskbarRevealed(bool visible)
        {
            TaskbarFrame.Opacity = visible ? 1 : 0;
            TaskbarFrame.IsHitTestVisible = visible;
        }
        private void AutoHidePointerMoved(CoreWindow sender, Windows.UI.Core.PointerEventArgs args)
        {
            if (!AppState.Instance.TaskbarAutoHide || FirefoxWindow?.IsFullscreen == true) return;
            var transform = MainGrid.TransformToVisual(null).Inverse;
            if (transform == null) return;
            var p = transform.TransformPoint(args.CurrentPoint.Position);
            bool inWidth = p.X >= 0 && p.X <= MainGrid.ActualWidth;
            bool atEdge = inWidth && p.Y >= MainGrid.ActualHeight - 3 && p.Y <= MainGrid.ActualHeight;
            m_pointerOverTaskbar = inWidth && p.Y >= MainGrid.ActualHeight - 50 && p.Y <= MainGrid.ActualHeight;
            if (atEdge) SetTaskbarRevealed(true);
            if (m_pointerOverTaskbar) m_taskbarHideTimer.Stop();
            else if (TaskbarFrame.Opacity > 0 && !m_taskbarHideTimer.IsEnabled) m_taskbarHideTimer.Start();
        }
        private void AutoHidePointerExited(CoreWindow sender, Windows.UI.Core.PointerEventArgs args)
        {
            m_pointerOverTaskbar = false;
            if (AppState.Instance.TaskbarAutoHide && TaskbarFrame.Opacity > 0) m_taskbarHideTimer.Start();
        }
        private void AutoHideTimerTick(object sender, object args)
        {
            if (m_startManager.IsStartOpen || m_actionManager.IsActionCenterOpen || m_windowManager.IsTaskViewOpen ||
                VisualTreeHelper.GetOpenPopups(Window.Current).Count > 0) return;
            m_taskbarHideTimer.Stop();
            if (AppState.Instance.TaskbarAutoHide && !m_pointerOverTaskbar) SetTaskbarRevealed(false);
        }

        private void UpdateWorkspaceBounds()
        {
            SynchronizeFileOperationWindows();
            foreach (var installer in m_installers)
            {
                installer.MaxWidth = Math.Max(320, AppInstallerWindowHost.ActualWidth);
                installer.MaxHeight = Math.Max(180, AppInstallerWindowHost.ActualHeight);
                Canvas.SetLeft(installer, Math.Max(0, Math.Min(Canvas.GetLeft(installer), AppInstallerWindowHost.ActualWidth - Math.Min(installer.Width, installer.MaxWidth))));
                Canvas.SetTop(installer, Math.Max(0, Math.Min(Canvas.GetTop(installer), AppInstallerWindowHost.ActualHeight - 36)));
            }
            // WindowHost shares the desktop's logical coordinate space, including the
            // Xbox desktop scale. Use its measured size rather than physical bounds.
            m_windowManager.SetWorkspaceBounds(
                WindowHost.ActualWidth,
                Math.Max(0, WindowHost.ActualHeight - (AppState.Instance.TaskbarAutoHide ? 0 : 50)));
            m_nativeWindowManager.SetWorkspaceBounds(
                NativeWindowHost.ActualWidth,
                Math.Max(0, NativeWindowHost.ActualHeight - (AppState.Instance.TaskbarAutoHide ? 0 : 50)));
        }

        private void OnPointerPressed(object sender, PointerRoutedEventArgs args)
        {
            // Bounds below are transformed to the XAML root. Read the pointer
            // in that same coordinate space; BackgroundWallpaper coordinates
            // are logical desktop coordinates and differ on Xbox because the
            // whole desktop is scaled to 70%. Mixing both spaces made every
            // click appear outside Start and dismissed it.
            PointerPoint point = args.GetCurrentPoint(null);

            if (m_actionManager.IsActionCenterOpen)
            {
                Rect actionCenterBounds = ActionCenterFrame.TransformToVisual(null).TransformBounds(new Rect(0, 0, ActionCenterFrame.ActualWidth, ActionCenterFrame.ActualHeight));

                if (!actionCenterBounds.Contains(point.Position))
                {
                    m_actionManager.RequestActionVisibilityChange(false);
                }
            }

            if (m_startManager.IsStartOpen)
            {
                Rect startMenuBounds = StartMenuFrame.TransformToVisual(null).TransformBounds(new Rect(0, 0, StartMenuFrame.ActualWidth, StartMenuFrame.ActualHeight));

                if (!startMenuBounds.Contains(point.Position))
                {
                    m_startManager.RequestStartVisibilityChange(false);
                }
            }
        }



        private async void BgChangebutton_Click(object sender, RoutedEventArgs e)
        {
            // Create a file picker
            FileOpenPicker openPicker = new FileOpenPicker();
            openPicker.ViewMode = PickerViewMode.Thumbnail;
            openPicker.SuggestedStartLocation = PickerLocationId.PicturesLibrary;
            openPicker.FileTypeFilter.Add(".jpg");
            openPicker.FileTypeFilter.Add(".jpeg");
            openPicker.FileTypeFilter.Add(".png");

            // Let the user pick a file
            StorageFile file = await openPicker.PickSingleFileAsync();
            if (file != null)
            {
                // Save the file path to local settings
                Windows.Storage.ApplicationDataContainer localSettings = Windows.Storage.ApplicationData.Current.LocalSettings;
                localSettings.Values["backgroundImagePath"] = file.Path;

                // Set the selected image as the source of the BackgroundWallpaper image
                var bitmapImage = new BitmapImage();
                using (var stream = await file.OpenAsync(FileAccessMode.Read))
                {
                    await bitmapImage.SetSourceAsync(stream);
                }
                BackgroundWallpaper.Source = bitmapImage;
            }
        }


        public MainDesktopViewModel ViewModel => (MainDesktopViewModel)this.DataContext;


        private void InitActionOpen()
        {
            DoubleAnimation slideInAnimation = new DoubleAnimation
            {
                From = 800,
                To = 0,
                Duration = new Duration(TimeSpan.FromSeconds(0.3)),
                EasingFunction = new CubicEase { EasingMode = EasingMode.EaseInOut }
            };

            Storyboard.SetTarget(slideInAnimation, ActionCenterTransform);
            Storyboard.SetTargetProperty(slideInAnimation, "Y");

            OpenActionStoryboard = new Storyboard();
            OpenActionStoryboard.Children.Add(slideInAnimation);
        }
        private void InitActionClose()
        {
            DoubleAnimation slideOutAnimation = new DoubleAnimation
            {
                From = 0,
                To = 800,
                Duration = new Duration(TimeSpan.FromSeconds(0.20)),
                EasingFunction = new QuadraticEase { EasingMode = EasingMode.EaseInOut }
            };

            Storyboard.SetTarget(slideOutAnimation, ActionCenterTransform);
            Storyboard.SetTargetProperty(slideOutAnimation, "Y");

            CloseActionStoryboard = new Storyboard();
            CloseActionStoryboard.Children.Add(slideOutAnimation);
        }

        private void InitStartOpen()
        {
            DoubleAnimation slideInAnimation = new DoubleAnimation
            {
                From = 800,
                To = 0,
                Duration = new Duration(TimeSpan.FromSeconds(0.3)),
                EasingFunction = new CubicEase { EasingMode = EasingMode.EaseInOut }
            };

            Storyboard.SetTarget(slideInAnimation, StartMenuTransform);
            Storyboard.SetTargetProperty(slideInAnimation, "Y");

            OpenStartStoryboard = new Storyboard();
            OpenStartStoryboard.Children.Add(slideInAnimation);
        }
        private void InitStartClose()
        {
            DoubleAnimation slideOutAnimation = new DoubleAnimation
            {
                From = 0,
                To = 800,
                Duration = new Duration(TimeSpan.FromSeconds(0.20)),
                EasingFunction = new QuadraticEase { EasingMode = EasingMode.EaseInOut }
            };

            Storyboard.SetTarget(slideOutAnimation, StartMenuTransform);
            Storyboard.SetTargetProperty(slideOutAnimation, "Y");

            CloseStartStoryboard = new Storyboard();
            CloseStartStoryboard.Children.Add(slideOutAnimation);
        }


        private void StartManager_StartVisibilityChanged(object sender, Library.Events.StartVisibilityChangedEventArgs e)
        {
            if (e.CurrentVisibility) { EnsureStartMenu(); OpenStartStoryboard.Begin(); }
            else { CloseStartStoryboard.Begin(); }
            UpdateNativeInputSuppression();
        }

        private void ActionCenterManager_ActionVisibilityChanged(object sender, Library.Events.ActionCenterVisibilityChangedEventArgs e)
        {
            if (e.CurrentVisibility) { EnsureActionCenter(); OpenActionStoryboard.Begin(); }
            else { CloseActionStoryboard.Begin(); }
            UpdateNativeInputSuppression();
        }

        private void UpdateNativeInputSuppression()
        {
            bool suppressed = m_windowManager.IsTaskViewOpen ||
                m_startManager.IsStartOpen || m_actionManager.IsActionCenterOpen;
            m_nativeWindowManager.SetInputSuppressed(suppressed);
            FirefoxWindow?.SetShellInputSuppressed(suppressed);
        }
    }
}
