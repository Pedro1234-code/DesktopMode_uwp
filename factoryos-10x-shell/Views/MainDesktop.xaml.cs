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
        private readonly Win32WindowManagerService m_nativeWindowManager;

        public MainDesktop()
        {
            this.InitializeComponent();
            DataContext = App.ServiceProvider.GetRequiredService<MainDesktopViewModel>();

            m_startManager = App.ServiceProvider.GetRequiredService<IStartManagerService>();
            m_startManager.StartVisibilityChanged += StartManager_StartVisibilityChanged;

            m_actionManager = App.ServiceProvider.GetRequiredService<IActionCenterManagerService>();
            m_actionManager.ActionVisibilityChanged += ActionCenterManager_ActionVisibilityChanged;
            m_windowManager = App.ServiceProvider.GetRequiredService<IWindowManagerService>();
            m_windowManager.DesktopFocusRequested += WindowManager_DesktopFocusRequested;
            m_windowManager.TaskViewChanged += WindowManager_TaskViewChanged;
            WindowHost.DataContext = m_windowManager;
            WindowHost.ItemsSource = m_windowManager.Windows;
            m_nativeWindowManager = Win32WindowManagerService.Instance;
            NativeWindowHost.DataContext = m_nativeWindowManager;
            NativeWindowHost.ItemsSource = m_nativeWindowManager.Windows;
            m_nativeWindowManager.WindowsChanged += NativeWindowsChanged;
            m_nativeWindowManager.DesktopFocusRequested += WindowManager_DesktopFocusRequested;
            Loaded += MainDesktop_Loaded;
            SizeChanged += MainDesktop_SizeChanged;

            LoadBackgroundImage();

            this.PointerPressed += OnPointerPressed;

            TaskbarFrame.Navigate(typeof(Default10xBar));
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
            AppState.Instance.OnFilesStateChanged += UpdateFilesTaskCard;
            AppState.Instance.OnFilesActivated += FilesActivated;
            AppState.Instance.OnNotepadRequested += NotepadRequested;
            AppState.Instance.OnNotepadStateChanged += UpdateNotepadTaskCard;
            AppState.Instance.OnNotepadActivated += NotepadActivated;
            AppState.Instance.OnSettingsRequested += SettingsRequested;
            AppState.Instance.OnSettingsStateChanged += UpdateSettingsTaskCard;
            AppState.Instance.OnSettingsActivated += SettingsActivated;
            AppState.Instance.OnWallpaperRequested += SetWallpaper;
            m_windowManager.WindowsChanged += WebWindowsChanged;
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
            m_windowManager.DeactivateAll();
            m_nativeWindowManager.DeactivateAll();
            // Files and Web Apps live in separate hosts. Raise the active host rather
            // than sending the inactive one below the wallpaper.
            Canvas.SetZIndex(WindowHost, 1);
            Canvas.SetZIndex(NativeWindowHost, 1);
            Canvas.SetZIndex(FilesWindowHost, 2);
            Canvas.SetZIndex(NotepadWindowHost, 1);
            Canvas.SetZIndex(SettingsWindowHost, 1);
        }

        private void NotepadRequested()
        {
            StorageFile file = AppState.Instance.TakePendingNotepadFile();
            if (file == null) NotepadWindow.Open();
            else _ = NotepadWindow.OpenFileAsync(file);
        }

        private void NotepadActivated()
        {
            m_windowManager.DeactivateAll();
            m_nativeWindowManager.DeactivateAll();
            Canvas.SetZIndex(WindowHost, 1);
            Canvas.SetZIndex(NativeWindowHost, 1);
            Canvas.SetZIndex(FilesWindowHost, 1);
            Canvas.SetZIndex(NotepadWindowHost, 2);
            Canvas.SetZIndex(SettingsWindowHost, 1);
        }

        private void SettingsRequested() => SettingsWindow.Open();

        private void SettingsActivated()
        {
            m_windowManager.DeactivateAll();
            m_nativeWindowManager.DeactivateAll();
            Canvas.SetZIndex(WindowHost, 1);
            Canvas.SetZIndex(NativeWindowHost, 1);
            Canvas.SetZIndex(FilesWindowHost, 1);
            Canvas.SetZIndex(NotepadWindowHost, 1);
            Canvas.SetZIndex(SettingsWindowHost, 2);
        }

        private void WebWindowsChanged(object sender, EventArgs e)
        {
            if (m_windowManager.Windows.Any(window => window.IsActive && window.Visibility == Visibility.Visible))
            {
                Canvas.SetZIndex(FilesWindowHost, 1);
                Canvas.SetZIndex(NotepadWindowHost, 1);
                Canvas.SetZIndex(SettingsWindowHost, 1);
                Canvas.SetZIndex(WindowHost, 2);
                Canvas.SetZIndex(NativeWindowHost, 1);
                m_nativeWindowManager.DeactivateAll();
            }
        }

        private void NativeWindowsChanged(object sender, EventArgs e)
        {
            if (m_nativeWindowManager.Windows.Any(window => window.IsActive && window.Visibility == Visibility.Visible))
            {
                m_windowManager.DeactivateAll();
                Canvas.SetZIndex(WindowHost, 1);
                Canvas.SetZIndex(FilesWindowHost, 1);
                Canvas.SetZIndex(NotepadWindowHost, 1);
                Canvas.SetZIndex(SettingsWindowHost, 1);
                Canvas.SetZIndex(NativeWindowHost, 2);
            }
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

        private void MainDesktop_Loaded(object sender, RoutedEventArgs e)
        {
            UpdateWorkspaceBounds();
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
            UpdateFilesTaskCard();
            UpdateNotepadTaskCard();
            UpdateSettingsTaskCard();
            UpdateNativeInputSuppression();
        }

        private void UpdateFilesTaskCard()
        {
            FilesTaskCard.Visibility = m_windowManager.IsTaskViewOpen && AppState.Instance.IsFilesOpen
                ? Visibility.Visible : Visibility.Collapsed;
        }

        private void UpdateNotepadTaskCard()
        {
            NotepadTaskCard.Visibility = m_windowManager.IsTaskViewOpen && AppState.Instance.IsNotepadOpen
                ? Visibility.Visible : Visibility.Collapsed;
        }

        private void UpdateSettingsTaskCard() => SettingsTaskCard.Visibility = m_windowManager.IsTaskViewOpen && AppState.Instance.IsSettingsOpen ? Visibility.Visible : Visibility.Collapsed;
        private void SettingsTaskCard_Tapped(object sender, TappedRoutedEventArgs e) { m_windowManager.CloseTaskView(); AppState.Instance.RequestSettingsOpen(); }
        private void SettingsTaskClose_Click(object sender, RoutedEventArgs e) => SettingsWindow.CloseFromTaskView();

        private void NotepadTaskCard_Tapped(object sender, TappedRoutedEventArgs e)
        {
            m_windowManager.CloseTaskView();
            AppState.Instance.RequestNotepadOpen();
        }

        private void NotepadTaskClose_Click(object sender, RoutedEventArgs e) => NotepadWindow.CloseFromTaskView();

        private void FilesTaskCard_Click(object sender, RoutedEventArgs e)
        {
            m_windowManager.CloseTaskView();
            AppState.Instance.RequestFilesOpen();
        }

        private void FilesTaskCard_Tapped(object sender, TappedRoutedEventArgs e)
        {
            m_windowManager.CloseTaskView();
            AppState.Instance.RequestFilesOpen();
        }

        private void FilesTaskClose_Click(object sender, RoutedEventArgs e)
        {
            FilesWindow.CloseFromTaskView();
        }

        private async void TaskViewWindow_Click(object sender, RoutedEventArgs e)
        {
            WebAppWindowModel window = (sender as FrameworkElement)?.Tag as WebAppWindowModel;
            if (window == null) return;

            await ActivateTaskViewWindowAsync(window);
        }

        private async void TaskViewGrid_ItemClick(object sender, ItemClickEventArgs e)
        {
            if (e.ClickedItem is WebAppWindowModel window)
                await ActivateTaskViewWindowAsync(window);
        }

        private async System.Threading.Tasks.Task ActivateTaskViewWindowAsync(WebAppWindowModel window)
        {
            m_windowManager.CloseTaskView();
            await Dispatcher.RunAsync(CoreDispatcherPriority.Normal, () => m_windowManager.Activate(window));
        }

        private void TaskViewClose_Click(object sender, RoutedEventArgs e)
        {
            if ((sender as FrameworkElement)?.DataContext is WebAppWindowModel window)
                m_windowManager.Close(window);
        }

        private async void NativeTaskViewGrid_ItemClick(object sender, ItemClickEventArgs e)
        {
            if (!(e.ClickedItem is Win32WindowModel window)) return;
            m_windowManager.CloseTaskView();
            await Dispatcher.RunAsync(CoreDispatcherPriority.Normal, () => m_nativeWindowManager.Activate(window));
        }

        private void NativeTaskViewClose_Click(object sender, RoutedEventArgs e)
        {
            if ((sender as FrameworkElement)?.DataContext is Win32WindowModel window)
                m_nativeWindowManager.Close(window);
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
            PointerPoint point = args.GetCurrentPoint(BackgroundWallpaper);

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
            m_nativeWindowManager.SetInputSuppressed(
                m_windowManager.IsTaskViewOpen || m_startManager.IsStartOpen || m_actionManager.IsActionCenterOpen);
        }
    }
}
