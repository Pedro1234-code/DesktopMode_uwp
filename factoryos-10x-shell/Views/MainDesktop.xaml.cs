using factoryos_10x_shell.Library.Services.Managers;
using factoryos_10x_shell.Library.ViewModels;
using Microsoft.Extensions.DependencyInjection;
using System;
using System.Collections.Generic;
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
        private bool m_coordinatingActivation;
        private bool m_startupUpdateCheckStarted;
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
            Loaded += MainDesktop_Loaded;
            SizeChanged += MainDesktop_SizeChanged;

            LoadBackgroundImage();

            this.PointerPressed += OnPointerPressed;

            TaskbarFrame.Navigate(typeof(Default10xBar));
            FirefoxWindow.FullscreenChanged += FirefoxWindow_FullscreenChanged;
            StartMenuFrame.Navigate(typeof(StartMenu));
            ActionCenterFrame.Navigate(typeof(ActionCenterHome));

            InitStartOpen();
            InitStartClose();

            InitActionOpen();
            InitActionClose();

            App.MediaPlayer.Source = MediaSource.CreateFromUri(new Uri("ms-appx:///Assets/Sounds/BootUp.wav"));
            App.MediaPlayer.Play();

            AppState.Instance.OnBgChangeButtonVisibilityChanged += UpdateBgChangeButtonVisibility;
            AppState.Instance.OnFilesRequested += FilesRequested;
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
            AppState.Instance.OnFirefoxStateChanged += FirefoxStateChanged;
            AppState.Instance.OnFirefoxActivated += FirefoxActivated;
            AppState.Instance.OnWallpaperRequested += SetWallpaper;
            m_windowManager.WindowsChanged += WebWindowsChanged;
            TaskViewGrid.ItemsSource = m_shellWindowCoordinator.Windows;
            SynchronizeShellWindows();
            UpdateBgChangeButtonVisibility(AppState.Instance.IsBgChangeButtonVisible);
        }

        private void UpdateBgChangeButtonVisibility(bool isVisible)
        {
            BgChangebutton.Visibility = isVisible ? Visibility.Visible : Visibility.Collapsed;

        }

        private void FilesRequested(bool resetHome)
        {
            if (resetHome) FilesWindow.OpenHome();
            else FilesWindow.Restore();
        }

        private void FilesActivated()
        {
            ActivateShellWindow(ShellWindowKind.Files, FilesWindowIdentity);
        }

        private void NotepadRequested()
        {
            StorageFile file = AppState.Instance.TakePendingNotepadFile();
            if (file == null) NotepadWindow.Open();
            else _ = NotepadWindow.OpenFileAsync(file);
        }

        private void NotepadActivated()
        {
            ActivateShellWindow(ShellWindowKind.Notepad, NotepadWindowIdentity);
        }

        private void SettingsRequested() => SettingsWindow.Open();

        private void SettingsActivated()
        {
            ActivateShellWindow(ShellWindowKind.Settings, SettingsWindowIdentity);
        }

        private void CalculatorRequested() => CalculatorWindow.Open();

        private void CalculatorActivated()
        {
            ActivateShellWindow(ShellWindowKind.Calculator, CalculatorWindowIdentity);
        }

        private void FirefoxRequested() => FirefoxWindow.Open();

        private void FirefoxActivated()
        {
            ActivateShellWindow(ShellWindowKind.Firefox, FirefoxWindowIdentity);
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
            SynchronizeShellWindows();
            RestoreMostRecentWindow();
        }

        private void NotepadStateChanged()
        {
            SynchronizeShellWindows();
            RestoreMostRecentWindow();
        }

        private void SettingsStateChanged()
        {
            SynchronizeShellWindows();
            RestoreMostRecentWindow();
        }

        private void CalculatorStateChanged()
        {
            SynchronizeShellWindows();
            RestoreMostRecentWindow();
        }

        private void FirefoxStateChanged()
        {
            SynchronizeShellWindows();
            RestoreMostRecentWindow();
        }

        private void WebAppsChanged(object sender, EventArgs e) => SynchronizeShellWindows();

        private void ActivateShellWindow(ShellWindowKind kind, object identity)
        {
            if (identity == null) return;

            m_coordinatingActivation = true;
            try
            {
                m_shellWindowCoordinator.Prune(IsWindowRegistered);
                m_shellWindowCoordinator.Activate(kind, identity);
                if (kind != ShellWindowKind.WebApp) m_windowManager.DeactivateAll();
                if (kind != ShellWindowKind.Win32App) m_nativeWindowManager.DeactivateAll();
                ApplyWindowLayers(kind);
                SynchronizeShellWindows();
            }
            finally
            {
                m_coordinatingActivation = false;
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
            Canvas.SetZIndex(NotepadWindowHost, activeKind == ShellWindowKind.Notepad ? 2 : 1);
            Canvas.SetZIndex(SettingsWindowHost, activeKind == ShellWindowKind.Settings ? 2 : 1);
            Canvas.SetZIndex(CalculatorWindowHost, activeKind == ShellWindowKind.Calculator ? 2 : 1);
            Canvas.SetZIndex(
                FirefoxWindowHost,
                FirefoxWindow.IsFullscreen ? 40 :
                activeKind == ShellWindowKind.Firefox ? 2 : 1);
            FirefoxWindow.SetWindowActive(activeKind == ShellWindowKind.Firefox);
        }

        private void SynchronizeShellWindows()
        {
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
                "Firefox",
                "Web browser",
                "\uE774",
                m_firefoxIcon,
                AppState.Instance.IsFirefoxOpen,
                AppState.Instance.IsFirefoxMinimized);

            m_shellWindowCoordinator.Synchronize(descriptors);
            TaskViewEmptyState.Visibility = descriptors.Count == 0
                ? Visibility.Visible
                : Visibility.Collapsed;
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

        private void SetWallpaper(string imagePath)
        {
            if (string.IsNullOrWhiteSpace(imagePath)) return;
            if (imagePath.StartsWith("ms-appx:", StringComparison.OrdinalIgnoreCase))
            {
                BackgroundWallpaper.Source = new BitmapImage(new Uri(imagePath));
                return;
            }
            LoadBackgroundImage();
        }

        public Image BackgroundWallpaperControl => BackgroundWallpaper;

        private void MainDesktop_SizeChanged(object sender, SizeChangedEventArgs e)
        {
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
                case ShellWindowKind.WebApp:
                    m_windowManager.Close((WebAppWindowModel)window.Identity);
                    break;
                case ShellWindowKind.Win32App:
                    m_nativeWindowManager.Close((Win32WindowModel)window.Identity);
                    break;
                case ShellWindowKind.Files:
                    FilesWindow.CloseFromTaskView();
                    break;
                case ShellWindowKind.Notepad:
                    NotepadWindow.CloseFromTaskView();
                    break;
                case ShellWindowKind.Settings:
                    SettingsWindow.CloseFromTaskView();
                    break;
                case ShellWindowKind.Calculator:
                    CalculatorWindow.CloseFromTaskView();
                    break;
                case ShellWindowKind.Firefox:
                    FirefoxWindow.CloseFromTaskView();
                    break;
            }
        }

        private void TaskViewOverlay_PointerPressed(object sender, PointerRoutedEventArgs e)
        {
            if (e.OriginalSource == TaskViewOverlay)
                m_windowManager.CloseTaskView();
        }

        private void UpdateWorkspaceBounds()
        {
            // WindowHost shares the desktop's logical coordinate space, including the
            // Xbox desktop scale. Use its measured size rather than physical bounds.
            m_windowManager.SetWorkspaceBounds(
                WindowHost.ActualWidth,
                Math.Max(0, WindowHost.ActualHeight - 50));
            m_nativeWindowManager.SetWorkspaceBounds(
                NativeWindowHost.ActualWidth,
                Math.Max(0, NativeWindowHost.ActualHeight - 50));
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
            if (e.CurrentVisibility) { OpenStartStoryboard.Begin(); }
            else { CloseStartStoryboard.Begin(); }
            UpdateNativeInputSuppression();
        }

        private void ActionCenterManager_ActionVisibilityChanged(object sender, Library.Events.ActionCenterVisibilityChangedEventArgs e)
        {
            if (e.CurrentVisibility) { OpenActionStoryboard.Begin(); }
            else { CloseActionStoryboard.Begin(); }
            UpdateNativeInputSuppression();
        }

        private void UpdateNativeInputSuppression()
        {
            bool suppressed = m_windowManager.IsTaskViewOpen ||
                m_startManager.IsStartOpen || m_actionManager.IsActionCenterOpen;
            m_nativeWindowManager.SetInputSuppressed(suppressed);
            FirefoxWindow.SetShellInputSuppressed(suppressed);
        }
    }
}
