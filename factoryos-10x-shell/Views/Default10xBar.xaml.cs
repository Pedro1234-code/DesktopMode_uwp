using factoryos_10x_shell.Controls;
using factoryos_10x_shell.Library.Models.InternalData;
using factoryos_10x_shell.Library.Services.Navigation;
using factoryos_10x_shell.Library.ViewModels;
using factoryos_10x_shell.Services.Helpers;
using factoryos_10x_shell.Services.Navigation;
using Microsoft.Extensions.DependencyInjection;
using System;
using System.Collections.Generic;
using System.Diagnostics;
using System.Linq;
using System.Runtime.InteropServices;
using System.Runtime.InteropServices.WindowsRuntime;
using System.Threading.Tasks;
using Windows.ApplicationModel;
using Windows.ApplicationModel.AppService;
using Windows.ApplicationModel.Core;
using Windows.Devices.Power;
using Windows.Foundation.Collections;
using Windows.Graphics.Imaging;
using Windows.Management.Deployment;
using Windows.Storage.Streams;
using Windows.System;
using Windows.UI;
using Windows.UI.Core;
using Windows.UI.Input.Preview.Injection;
using Windows.UI.ViewManagement;
using Windows.UI.Xaml;
using Windows.UI.Xaml.Controls;
using Windows.UI.Xaml.Data;
using Windows.UI.Xaml.Media;
using Windows.UI.Xaml.Media.Imaging;
using factoryos_10x_shell.Library.Services.WebApps;
using factoryos_10x_shell.Services.Win32;
using factoryos_10x_shell.Services.Windowing;
using factoryos_10x_shell.Services.Files;


namespace factoryos_10x_shell.Views
{
    public sealed partial class Default10xBar : Page
    {
        private readonly AppHelper _appHelper;
        private readonly IWindowManagerService _windowManager;
        private readonly IWebAppService _webAppService;
        private readonly Win32WindowManagerService _nativeWindowManager;
        private readonly ShellWindowCoordinator _shellWindowCoordinator;
        private bool? _appliedWindows11Skin;
        private BitmapImage _packageIconSource;
        private readonly BitmapImage _filesIconSource = new BitmapImage(new Uri("ms-appx:///Assets/Files/files.png"));
        private readonly BitmapImage _notepadIconSource = new BitmapImage(new Uri("ms-appx:///Assets/Notepad/notepad.png"));
        private readonly BitmapImage _settingsIconSource = new BitmapImage(new Uri("ms-appx:///Windows10x-js-main/Icons/WindowsSettings.png"));
        private readonly BitmapImage _calculatorIconSource = new BitmapImage(new Uri("ms-appx:///Assets/Calculator/CalculatorAppList.targetsize-48.png"));
        private readonly BitmapImage _firefoxIconSource = new BitmapImage(new Uri("ms-appx:///Assets/Firefox/firefox.png"));


        private void UpdateTrayDate()
        {
            if (Windows11TrayDate.Visibility == Visibility.Visible)
                Windows11TrayDate.Text = DateTime.Today.ToShortDateString();
        }

        private void ApplyShellSkin()
        {
            bool windows11 = AppState.Instance.UseWindows11Skin;
            bool dark = ActualTheme == ElementTheme.Dark;
            Height = TaskbarGrid.Height = TrayLayout.Height = ActionCenterButton.Height = windows11 ? 48 : 50;
            var visibility = windows11 ? Visibility.Visible : Visibility.Collapsed;
            Windows11TaskbarPlate.Visibility = visibility;
            var legacyVisibility = windows11 ? Visibility.Collapsed : Visibility.Visible;
            Windows10XTaskbarPlate.Visibility = legacyVisibility;
            Windows11StartButton.Visibility = visibility;
            Windows11TaskViewButton.Visibility = visibility;
            ApplyTaskbarSearchMode();
            Windows11TrayDate.Visibility = visibility;
            StartButton.Visibility = legacyVisibility;
            TaskViewButton.Visibility = legacyVisibility;
            TrayLayout.Width = ActionCenterButton.Width = windows11 ? 96 : 75;
            Windows11QuickSettingsButton.Visibility = visibility;
            TrayStatusIcons.Visibility = windows11 ? Visibility.Collapsed : Visibility.Visible;
            ApplyTaskbarButtonSkin(ActionCenterButton);
            ApplyTaskbarButtonSkin(Windows11QuickSettingsButton);
            TrayStatusIcons.Margin = windows11 ? new Thickness(3, 0, 0, 0) : new Thickness(0);
            TrayStatusIcons.HorizontalAlignment = windows11 ? HorizontalAlignment.Left : HorizontalAlignment.Right;
            TrayStatusIcons.VerticalAlignment = windows11 ? VerticalAlignment.Center : VerticalAlignment.Stretch;
            ClockText2.FontSize = windows11 ? 12 : 14;
            if (windows11)
            {
                Windows11TaskbarPlate.BorderBrush = new SolidColorBrush(dark
                    ? Color.FromArgb(36, 255, 255, 255)
                    : Color.FromArgb(32, 0, 0, 0));
                UpdateTrayDate();
            }
            if (_shellWindowCoordinator == null) return;
            if (_appliedWindows11Skin != windows11)
            {
                _appliedWindows11Skin = windows11;
                RefreshPinnedApps();
                RefreshFilesTaskbar(skinChanged: true);
            }
            UpdateInternalTaskbarVisualState();
            UpdateWebAppTaskbarVisualState();
            UpdateWin32TaskbarVisualState();
        }

        private void ApplyTaskbarPreferences()
        {
            TaskbarButtons.HorizontalAlignment = AppState.Instance.TaskbarAlignLeft
                ? HorizontalAlignment.Left : HorizontalAlignment.Center;
            ApplyTaskbarSearchMode();
        }

        private void ApplyTaskbarSearchMode()
        {
            var mode = AppState.Instance.Windows11TaskbarSearchMode;
            Windows11TaskbarSearch.Visibility = AppState.Instance.UseWindows11Skin && mode != TaskbarSearchMode.Hidden
                ? Visibility.Visible : Visibility.Collapsed;
            Windows11TaskbarSearch.SetSearchBoxMode(mode == TaskbarSearchMode.SearchBox);
        }

        internal event Action SearchRequested;

        private void Windows11TaskbarSearch_Click(object sender, RoutedEventArgs e) => SearchRequested?.Invoke();

        public Default10xBar()
        {
            this.InitializeComponent();
            ApplyTaskbarPreferences();
            Loaded += (s, e) =>
            {
                AppState.Instance.ShellSkinChanged += ApplyShellSkin;
                ApplyShellSkin();
            };
            Unloaded += (s, e) => AppState.Instance.ShellSkinChanged -= ApplyShellSkin;
            ActualThemeChanged += (s, e) => ApplyShellSkin();
            ClockText2.RegisterPropertyChangedCallback(TextBlock.TextProperty, (s, p) => UpdateTrayDate());
            Loaded += (s, e) => { ApplyTaskbarPreferences(); AppState.Instance.TaskbarPreferencesChanged += ApplyTaskbarPreferences; };
            Unloaded += (s, e) => AppState.Instance.TaskbarPreferencesChanged -= ApplyTaskbarPreferences;

            DataContext = App.ServiceProvider.GetRequiredService<Default10xBarViewModel>();
            _appHelper = App.ServiceProvider.GetRequiredService<AppHelper>();
            _windowManager = App.ServiceProvider.GetRequiredService<IWindowManagerService>();
            _webAppService = App.ServiceProvider.GetRequiredService<IWebAppService>();
            _nativeWindowManager = Win32WindowManagerService.Instance;
            _shellWindowCoordinator = App.ServiceProvider.GetRequiredService<ShellWindowCoordinator>();
            _shellWindowCoordinator.Windows.CollectionChanged += (s, e) => RefreshFilesTaskbar();

            Loaded += Default10xBar_Loaded; // Chamada após a view estar carregada
        }

        private async void Default10xBar_Loaded(object sender, RoutedEventArgs e)
        {
            await _appHelper.LoadPinnedTaskbarAppsAsync();
            RefreshPinnedApps();
            // Pin/unpin really changes the pinned collection, so rebuild that
            // panel only for this event. Internal window state changes below
            // must not pay this cost.
            _appHelper.TaskbarIcons.CollectionChanged += (s, args) =>
            {
                RefreshPinnedApps();
                RefreshFilesTaskbar();
                RefreshOpenWin32Apps();
            };
            _windowManager.Windows.CollectionChanged += (s, args) =>
            {
                RefreshOpenWebApps();
                UpdateWebAppButtons(PinnedWebAppsPanel);
            };
            _windowManager.WindowsChanged += (s, args) =>
            {
                UpdateWebAppTaskbarVisualState();
            };
            _webAppService.InstalledApps.CollectionChanged += (s, args) =>
                RefreshPinnedWebApps();
            _webAppService.AppsChanged += (s, args) =>
            {
                RefreshOpenWebApps();
                RefreshPinnedWebApps();
            };
            _nativeWindowManager.Windows.CollectionChanged += (s, args) => RefreshOpenWin32Apps();
            _nativeWindowManager.WindowsChanged += (s, args) => UpdateWin32TaskbarVisualState();
            FileOperationService.Instance.Windows.CollectionChanged += (s, args) => RefreshFilesTaskbar();
            FileOperationService.Instance.WindowsChanged += (s, args) => RefreshFilesTaskbar();
            AppState.Instance.OnFilesStateChanged += RefreshInternalTaskbar;
            AppState.Instance.OnNotepadStateChanged += RefreshInternalTaskbar;
            AppState.Instance.OnSettingsStateChanged += RefreshInternalTaskbar;
            AppState.Instance.OnCalculatorStateChanged += RefreshInternalTaskbar;
            AppState.Instance.OnFirefoxStateChanged += RefreshInternalTaskbar;
            // Activation only changes the highlight. Recreating every Button
            // here made all taskbar icons disappear/reappear on each click.
            _shellWindowCoordinator.StateChanged += (s, args) => UpdateInternalTaskbarVisualState();
            RefreshOpenWebApps();
            RefreshPinnedWebApps();
            RefreshOpenWin32Apps();
            RefreshFilesTaskbar();

            foreach (var pin in _appHelper.TaskbarIcons.Where(app => Win32StartShortcuts.IsShortcutId(app.AppId)).ToList())
                await ResolveWin32PinAsync(pin);
        }


        public Default10xBarViewModel ViewModel => (Default10xBarViewModel)this.DataContext;



        private void ActionCenterClick(object sender, RoutedEventArgs e)
        {

        }

        private void TaskViewButton_Click(object sender, RoutedEventArgs e)
        {
            _windowManager.ToggleTaskView();
        }

        private void RefreshPinnedApps()
        {
            PinnedAppsPanel.Children.Clear();


            foreach (var app in _appHelper.TaskbarIcons)
            {
                var button = new ShellTaskbarButton
                {
                    Width = 48,
                    Height = 48,
                    Margin = new Thickness(2),
                    Style = (Style)Application.Current.Resources["TaskbarButtonStyle"],
                    Background = GetPinnedAppBackground(app.AppId),
                    Tag = app
                };

                var stack = new StackPanel
                {
                    Orientation = Orientation.Vertical,
                    HorizontalAlignment = HorizontalAlignment.Center,
                    VerticalAlignment = VerticalAlignment.Center
                };

                if (app.AppId == "CoreShell.Files")
                {
                    stack.Children.Add(CreateFilesIcon(28));
                }
                else if (app.AppId == "CoreShell.Notepad")
                {
                    stack.Children.Add(CreateNotepadIcon(28));
                }
                else if (app.AppId == "CoreShell.Settings")
                {
                    stack.Children.Add(CreateSettingsIcon(25));
                }
                else if (app.AppId == "CoreShell.Calculator")
                {
                    stack.Children.Add(CreateCalculatorIcon(25));
                }
                else if (app.AppId == "CoreShell.Firefox")
                {
                    stack.Children.Add(CreateFirefoxIcon(26));
                }
                else
                {
                    var image = new Image { Width = 32, Height = 32 };
                    image.SetBinding(Image.SourceProperty, new Binding
                    {
                        Source = app,
                        Path = new PropertyPath(nameof(StartIconModel.IconSource)),
                        Mode = BindingMode.OneWay
                    });
                    if (Win32StartShortcuts.IsShortcutId(app.AppId))
                    {
                        var icon = new Grid { Width = 32, Height = 32 };
                        var placeholder = new FontIcon { Glyph = "\uE7C3", FontSize = 24 };
                        icon.Children.Add(placeholder);
                        icon.Children.Add(image);
                        placeholder.Visibility = image.Source == null ? Visibility.Visible : Visibility.Collapsed;
                        image.RegisterPropertyChangedCallback(Image.SourceProperty, (sender, property) =>
                            placeholder.Visibility = image.Source == null ? Visibility.Visible : Visibility.Collapsed);
                        stack.Children.Add(icon);
                    }
                    else
                    {
                        stack.Children.Add(image);
                    }
                }
                button.Content = stack;
                ToolTipService.SetToolTip(button, app.IconName);

                button.Click += async (s, e) =>
                {
                    var model = (s as Button)?.Tag as StartIconModel;
                    if (model != null)
                    {
                        if (model.AppId == "CoreShell.Files")
                        {
                            AppState.Instance.RequestFilesOpen();
                            return;
                        }
                        if (model.AppId == "CoreShell.Notepad")
                        {
                            AppState.Instance.RequestNotepadOpen();
                            return;
                        }
                        if (model.AppId == "CoreShell.Settings")
                        {
                            AppState.Instance.RequestSettingsOpen();
                            return;
                        }
                        if (model.AppId == "CoreShell.Calculator")
                        {
                            AppState.Instance.RequestCalculatorOpen();
                            return;
                        }
                        if (model.AppId == "CoreShell.Firefox")
                        {
                            AppState.Instance.RequestFirefoxOpen();
                            return;
                        }
                        if (Win32StartShortcuts.IsShortcutId(model.AppId))
                        {
                            var shortcut = await ResolveWin32PinAsync(model);
                            if (shortcut == null) return;
                            var existing = FindPinnedWin32Window(model);
                            if (existing != null)
                                _nativeWindowManager.ToggleFromTaskbar(existing);
                            else
                                _nativeWindowManager.Open(shortcut.Executable, shortcut.Folder, model.IconSource,
                                    shortcut.VirtualPath, shortcut.Arguments, shortcut.WorkingDirectory);
                            return;
                        }
                        bool launched = await _appHelper.LaunchAppAsync(model);
                        if (!launched)
                            factoryos_10x_shell.Services.Helpers.ShellDiagnostics.WriteDebug($"App não iniciado: {model.AppId} / {model.AppUri}");
                    }
                };

                button.RightTapped += (s, e) =>
                {
                    var flyout = new MenuFlyout();

                    var unpinItem = new MenuFlyoutItem { Text = "Unpin App" };
                    unpinItem.Click += (sender2, e2) =>
                    {
                        var model = (s as Button)?.Tag as StartIconModel;
                        if (model != null)
                        {
                            _appHelper.TaskbarIcons.Remove(model);
                            _appHelper.SaveTaskbarPinnedApps(_appHelper.TaskbarIcons.ToList());
                        }
                    };

                    flyout.Items.Add(unpinItem);
                    flyout.ShowAt(button, e.GetPosition(button));
                };


                ApplyTaskbarButtonSkin(button);
                PinnedAppsPanel.Children.Add(button);
            }

        }

        private void RefreshOpenWebApps()
        {
            OpenWebAppsPanel.Children.Clear();
            foreach (var group in _windowManager.Windows.Where(window => !window.App.IsPinned).GroupBy(window => window.App.Id))
            {
                WebAppDefinition app = group.First().App;
                bool isActive = group.Any(window => window.IsActive && window.Visibility == Visibility.Visible);
                var button = new ShellTaskbarButton
                {
                    Width = 48,
                    Height = 48,
                    Margin = new Thickness(2),
                    Tag = app,
                    Style = (Style)Application.Current.Resources["TaskbarButtonStyle"],
                    Background = new SolidColorBrush(Color.FromArgb(isActive ? (byte)80 : (byte)48, 70, 130, 180))
                };
                ToolTipService.SetToolTip(button, app.Name);
                button.Content = CreateWebAppIcon(app);
                button.Click += (sender, args) => _windowManager.ToggleFromTaskbar((WebAppDefinition)((Button)sender).Tag);
                AddWebAppTaskbarMenu(button, app);
                UpdateRunningIndicator(button, true, isActive, windowCount: group.Count());
                OpenWebAppsPanel.Children.Add(button);
            }
        }

        private void RefreshPinnedWebApps()
        {
            PinnedWebAppsPanel.Children.Clear();
            foreach (var app in _webAppService.InstalledApps.Where(app => app.IsPinned))
            {
                bool isOpen = _windowManager.Windows.Any(window => window.App.Id == app.Id);
                bool isActive = _windowManager.Windows.Any(window => window.App.Id == app.Id && window.IsActive && window.Visibility == Visibility.Visible);
                var button = new ShellTaskbarButton
                {
                    Width = 48,
                    Height = 48,
                    Margin = new Thickness(2),
                    Tag = app,
                    Style = (Style)Application.Current.Resources["TaskbarButtonStyle"],
                    Background = isOpen
                        ? new SolidColorBrush(Color.FromArgb(isActive ? (byte)80 : (byte)48, 70, 130, 180))
                        : new SolidColorBrush(Colors.Transparent)
                };
                ToolTipService.SetToolTip(button, app.Name);
                button.Content = CreateWebAppIcon(app);
                button.Click += (sender, args) => _windowManager.ToggleFromTaskbar((WebAppDefinition)((Button)sender).Tag);
                AddWebAppTaskbarMenu(button, app);
                UpdateRunningIndicator(button, isOpen, isActive, windowCount: _windowManager.Windows.Count(window => window.App.Id == app.Id));
                PinnedWebAppsPanel.Children.Add(button);
            }
        }

        private void RefreshOpenWin32Apps()
        {
            OpenWin32AppsPanel.Children.Clear();
            var pinnedWindows = new HashSet<Win32WindowModel>();
            foreach (var pin in _appHelper.TaskbarIcons)
            {
                var window = FindPinnedWin32Window(pin);
                if (window != null) pinnedWindows.Add(window);
            }
            foreach (Win32WindowModel window in _nativeWindowManager.Windows)
            {
                if (pinnedWindows.Contains(window)) continue;
                bool isActive = window.IsActive && window.Visibility == Visibility.Visible;
                var button = new ShellTaskbarButton
                {
                    Width = 48,
                    Height = 48,
                    Margin = new Thickness(2),
                    Tag = window,
                    Style = (Style)Application.Current.Resources["TaskbarButtonStyle"],
                    Background = new SolidColorBrush(Color.FromArgb(isActive ? (byte)80 : (byte)48, 70, 130, 180)),
                    Content = CreateWin32AppIcon(window)
                };
                ToolTipService.SetToolTip(button, window.DisplayName);
                button.Click += (sender, args) =>
                    _nativeWindowManager.ToggleFromTaskbar((Win32WindowModel)((Button)sender).Tag);
                UpdateRunningIndicator(button, true, isActive);
                OpenWin32AppsPanel.Children.Add(button);
            }
            UpdatePinnedWin32Indicators();
        }

        private async Task<Win32StartShortcut> ResolveWin32PinAsync(StartIconModel pin)
        {
            if (!_nativeWindowManager.IsAppSupportEnabled) return null;
            if (pin.Data is Win32StartShortcut cached) return cached;

            try
            {
                var entry = await Win32StartShortcuts.ResolveAsync(pin.AppId);
                if (!(entry?.Data is Win32StartShortcut shortcut) || !_appHelper.TaskbarIcons.Contains(pin))
                    return null;

                pin.Data = shortcut;
                RefreshOpenWin32Apps();
                _ = LoadPinnedWin32IconAsync(pin, entry);
                return shortcut;
            }
            catch (Exception error)
            {
                ShellDiagnostics.RecordException("Loading pinned Win32 shortcut", error);
                return null;
            }
        }

        private async Task LoadPinnedWin32IconAsync(StartIconModel pin, StartIconModel entry)
        {
            await Win32StartShortcuts.LoadIconsAsync(new[] { entry });
            pin.IconSource = entry.IconSource;
        }

        private Win32WindowModel FindPinnedWin32Window(StartIconModel pin)
        {
            if (!(pin.Data is Win32StartShortcut shortcut)) return null;
            return _nativeWindowManager.FindWindow(shortcut.Executable, shortcut.VirtualPath,
                shortcut.Arguments, shortcut.WorkingDirectory);
        }

        private void UpdatePinnedWin32Indicators()
        {
            foreach (var button in PinnedAppsPanel.Children.OfType<Button>())
            {
                if (!(button.Tag is StartIconModel pin) || !Win32StartShortcuts.IsShortcutId(pin.AppId)) continue;
                var window = FindPinnedWin32Window(pin);
                bool active = window?.IsActive == true && window.Visibility == Visibility.Visible;
                button.Background = window == null ? new SolidColorBrush(Colors.Transparent)
                    : new SolidColorBrush(Color.FromArgb(active ? (byte)80 : (byte)48, 70, 130, 180));
                UpdateRunningIndicator(button, window != null, active);
            }
        }

        private void ApplyTaskbarButtonSkin(Button button)
        {
            bool windows11 = AppState.Instance.UseWindows11Skin;
            var style = windows11 ? (Style)Resources["Windows11TaskbarAppStyle"]
                : (Style)Application.Current.Resources["TaskbarButtonStyle"];
            if (button.Style != style) button.Style = style;
            button.CornerRadius = new CornerRadius(windows11 ? 4 : 0);
            if (button is ShellTaskbarButton)
            {
                button.Width = windows11 ? 44 : 48;
                button.Height = 48;
                button.Margin = new Thickness(windows11 ? 0 : 2);
                if (button.Content is FrameworkElement icon)
                {
                    SetTaskbarIconSize(icon, windows11 ? 24 : 28);
                }
            }
            if (!windows11) button.ClearValue(Control.BorderBrushProperty);
        }

        private static void SetTaskbarIconSize(FrameworkElement icon, double size)
        {
            icon.Width = icon.Height = size;
            if (icon is Panel panel)
            {
                foreach (var child in panel.Children.OfType<FrameworkElement>())
                    SetTaskbarIconSize(child, size);
            }
        }

        private void UpdateRunningIndicator(Button button, bool isOpen, bool isActive, UIElement replacementIcon = null, int windowCount = 1)
        {
            if (replacementIcon != null)
                button.Content = replacementIcon;
            ApplyTaskbarButtonSkin(button);
            if (button is ShellTaskbarButton taskButton)
                taskButton.SetWindowState(isOpen, isActive, windowCount > 1);
        }
        private void UpdateWebAppTaskbarVisualState()
        {
            UpdateWebAppButtons(OpenWebAppsPanel);
            UpdateWebAppButtons(PinnedWebAppsPanel);
        }

        private void UpdateWebAppButtons(Panel panel)
        {
            foreach (Button button in panel.Children.OfType<Button>())
            {
                if (!(button.Tag is WebAppDefinition app)) continue;
                bool isOpen = _windowManager.Windows.Any(window => window.App.Id == app.Id);
                bool isActive = _windowManager.Windows.Any(window =>
                    window.App.Id == app.Id && window.IsActive &&
                    window.Visibility == Visibility.Visible);
                button.Background = isOpen
                    ? new SolidColorBrush(Color.FromArgb(isActive ? (byte)80 : (byte)48, 70, 130, 180))
                    : new SolidColorBrush(Colors.Transparent);
                UpdateRunningIndicator(button, isOpen, isActive, windowCount: _windowManager.Windows.Count(window => window.App.Id == app.Id));
            }
        }

        private void UpdateWin32TaskbarVisualState()
        {
            UpdatePinnedWin32Indicators();
            foreach (Button button in OpenWin32AppsPanel.Children.OfType<Button>())
            {
                if (!(button.Tag is Win32WindowModel window)) continue;
                bool isActive = window.IsActive && window.Visibility == Visibility.Visible;
                button.Background = new SolidColorBrush(
                    Color.FromArgb(isActive ? (byte)80 : (byte)48, 70, 130, 180));
                object content = button.Content;
                if (content is Grid plate && plate.Tag as string == "Windows11RunningIndicator")
                    content = plate.Children[0];
                var currentIcon = (content as Image)?.Source;
                UpdateRunningIndicator(button, true, isActive,
                    ReferenceEquals(currentIcon, window.IconSource) ? null : CreateWin32AppIcon(window));
            }
        }

        private static UIElement CreateWin32AppIcon(Win32WindowModel window)
        {
            if (window.IconSource != null)
            {
                return new Image
                {
                    Width = 26,
                    Height = 26,
                    Source = window.IconSource,
                    Stretch = Stretch.Uniform,
                    HorizontalAlignment = HorizontalAlignment.Center,
                    VerticalAlignment = VerticalAlignment.Center
                };
            }

            return new TextBlock
            {
                Text = "\uE7C3",
                FontFamily = new FontFamily("Segoe MDL2 Assets"),
                FontSize = 23,
                HorizontalAlignment = HorizontalAlignment.Center,
                VerticalAlignment = VerticalAlignment.Center
            };
        }

        private void AddWebAppTaskbarMenu(Button button, WebAppDefinition app)
        {
            button.RightTapped += (sender, args) =>
            {
                var flyout = new MenuFlyout();
                var newWindowItem = new MenuFlyoutItem { Text = app.NewInstanceText };
                newWindowItem.Click += (itemSender, itemArgs) => _windowManager.OpenNewWindow(app);
                flyout.Items.Add(newWindowItem);

                if (app.IsPinned)
                {
                    var unpinItem = new MenuFlyoutItem { Text = "Unpin from taskbar" };
                    unpinItem.Click += (itemSender, itemArgs) => _webAppService.SetPinned(app, false);
                    flyout.Items.Add(unpinItem);
                }
                flyout.ShowAt(button, args.GetPosition(button));
            };
        }

        private UIElement CreateWebAppIcon(WebAppDefinition app)
        {
            if (!string.IsNullOrWhiteSpace(app.IconUri) && Uri.TryCreate(app.IconUri, UriKind.Absolute, out Uri iconUri))
                return new Image { Width = 26, Height = 26, Source = new BitmapImage(iconUri), Stretch = Stretch.Uniform };
            return new TextBlock { Text = "\uE774", FontFamily = new FontFamily("Segoe MDL2 Assets"), FontSize = 20, HorizontalAlignment = HorizontalAlignment.Center, VerticalAlignment = VerticalAlignment.Center };
        }

        private Image CreateFilesIcon(double size)
        {
            return new Image
            {
                Width = size,
                Height = size,
                Source = AppState.Instance.UseWindows11Skin
                    ? (ImageSource)Application.Current.Resources["Windows11FilesIcon"]
                    : _filesIconSource,
                Stretch = Stretch.Uniform
            };
        }

        private Image CreateNotepadIcon(double size)
        {
            return new Image
            {
                Source = _notepadIconSource,
                Width = size,
                Height = size,
                HorizontalAlignment = HorizontalAlignment.Center,
                VerticalAlignment = VerticalAlignment.Center,
                Stretch = Stretch.Uniform
            };
        }

        private Image CreateSettingsIcon(double size) => new Image { Source = _settingsIconSource, Width = size, Height = size, HorizontalAlignment = HorizontalAlignment.Center, VerticalAlignment = VerticalAlignment.Center, Stretch = Stretch.Uniform };

        private Image CreateCalculatorIcon(double size) => new Image { Source = _calculatorIconSource, Width = size, Height = size, HorizontalAlignment = HorizontalAlignment.Center, VerticalAlignment = VerticalAlignment.Center, Stretch = Stretch.Uniform };

        private Image CreateFirefoxIcon(double size) => new Image { Source = _firefoxIconSource, Width = size, Height = size, HorizontalAlignment = HorizontalAlignment.Center, VerticalAlignment = VerticalAlignment.Center, Stretch = Stretch.Uniform };

        private void RefreshInternalTaskbar()
        {
            Stopwatch clock = Stopwatch.StartNew();
            RefreshFilesTaskbar();
            // Opening/minimizing an internal window changes only its running
            // button and active highlight. Rebuilding every pinned button here
            // forced XAML to recreate styles, flyouts and image decoders on the
            // UI thread, producing 80-500 ms stalls when an app was opened.
            UpdateInternalTaskbarVisualState();
            clock.Stop();
            MainPage.RecordUiOperation("Taskbar.RefreshInternal", clock.ElapsedMilliseconds);
        }

        private void UpdateInternalTaskbarVisualState()
        {
            UpdateInternalButtons(PinnedAppsPanel);
            UpdateInternalButtons(OpenFilesPanel);
        }

        private void UpdateInternalButtons(Panel panel)
        {
            foreach (Button button in panel.Children.OfType<Button>())
            {
                if (button.Tag is factoryos_10x_shell.Controls.AppInstallerWindow installer)
                {
                    UpdateRunningIndicator(button, true, _shellWindowCoordinator.IsActive(ShellWindowKind.AppInstaller, installer));
                    continue;
                }
                if (button.Tag is FileOperationModel operation)
                {
                    bool active = _shellWindowCoordinator.IsActive(ShellWindowKind.FileOperation, operation);
                    button.Background = new SolidColorBrush(Color.FromArgb(active ? (byte)80 : (byte)48, 70, 130, 180));
                    UpdateRunningIndicator(button, true, active);
                    continue;
                }
                string appId = (button.Tag as StartIconModel)?.AppId ?? button.Tag as string;
                if (!string.IsNullOrEmpty(appId) &&
                    appId.StartsWith("CoreShell.", StringComparison.OrdinalIgnoreCase))
                {
                    button.Background = GetPinnedAppBackground(appId);
                    var item = _shellWindowCoordinator.Windows.FirstOrDefault(window => Equals(window.Identity, appId));
                    UpdateRunningIndicator(button, item != null, item?.IsActive == true);
                }
            }
        }

        private Brush GetPinnedAppBackground(string appId)
        {
            bool isOpen = (appId == "CoreShell.Files" && AppState.Instance.IsFilesOpen)
                || (appId == "CoreShell.Notepad" && AppState.Instance.IsNotepadOpen)
                || (appId == "CoreShell.Settings" && AppState.Instance.IsSettingsOpen)
                || (appId == "CoreShell.Calculator" && AppState.Instance.IsCalculatorOpen)
                || (appId == "CoreShell.Firefox" && AppState.Instance.IsFirefoxOpen);

            bool isActive = (appId == "CoreShell.Files" && _shellWindowCoordinator.IsActive(ShellWindowKind.Files, appId))
                || (appId == "CoreShell.Notepad" && _shellWindowCoordinator.IsActive(ShellWindowKind.Notepad, appId))
                || (appId == "CoreShell.Settings" && _shellWindowCoordinator.IsActive(ShellWindowKind.Settings, appId))
                || (appId == "CoreShell.Calculator" && _shellWindowCoordinator.IsActive(ShellWindowKind.Calculator, appId))
                || (appId == "CoreShell.Firefox" && _shellWindowCoordinator.IsActive(ShellWindowKind.Firefox, appId));

            return isOpen
                ? new SolidColorBrush(Color.FromArgb(isActive ? (byte)80 : (byte)48, 70, 130, 180))
                : new SolidColorBrush(Colors.Transparent);
        }

        private void RefreshFilesTaskbar(bool skinChanged = false)
        {
            var items = new List<object>();
            foreach (var window in _shellWindowCoordinator.Windows)
                if (window.Kind == ShellWindowKind.AppInstaller && window.Identity is AppInstallerWindow openInstaller)
                    items.Add(openInstaller);
            items.AddRange(FileOperationService.Instance.Windows);

            var state = AppState.Instance;
            AddUnpinnedWindow(items, "CoreShell.Files", state.IsFilesOpen);
            AddUnpinnedWindow(items, "CoreShell.Notepad", state.IsNotepadOpen);
            AddUnpinnedWindow(items, "CoreShell.Settings", state.IsSettingsOpen);
            AddUnpinnedWindow(items, "CoreShell.Calculator", state.IsCalculatorOpen);
            AddUnpinnedWindow(items, "CoreShell.Firefox", state.IsFirefoxOpen);

            var buttons = OpenFilesPanel.Children;
            for (int index = buttons.Count - 1; index >= 0; index--)
                if (!items.Contains(((Button)buttons[index]).Tag))
                    buttons.RemoveAt(index);

            for (int index = 0; index < items.Count; index++)
            {
                object item = items[index];
                var button = index < buttons.Count ? (Button)buttons[index] : null;
                if (!Equals(button?.Tag, item))
                {
                    button = null;
                    for (int existing = index + 1; existing < buttons.Count; existing++)
                    {
                        var candidate = (Button)buttons[existing];
                        if (!Equals(candidate.Tag, item)) continue;
                        buttons.RemoveAt(existing);
                        button = candidate;
                        break;
                    }
                    if (button == null) button = CreateUnpinnedButton(item);
                    buttons.Insert(index, button);
                }

                if (item is AppInstallerWindow installer)
                {
                    string title = "App Installer — " + installer.PackageTitle;
                    if (!Equals(ToolTipService.GetToolTip(button), title))
                        ToolTipService.SetToolTip(button, title);
                }
                else if (skinChanged && Equals(item, "CoreShell.Files"))
                    button.Content = CreateFilesIcon(28);
            }
            UpdateInternalTaskbarVisualState();
        }

        private void AddUnpinnedWindow(List<object> items, string appId, bool isOpen)
        {
            if (isOpen && !_appHelper.TaskbarIcons.Any(app => app.AppId == appId))
                items.Add(appId);
        }

        private Button CreateUnpinnedButton(object item)
        {
            var button = new ShellTaskbarButton
            {
                Width = 48, Height = 48, Margin = new Thickness(2), Tag = item,
                Style = (Style)Application.Current.Resources["TaskbarButtonStyle"]
            };

            switch (item)
            {
                case AppInstallerWindow installer:
                    if (_packageIconSource == null)
                        _packageIconSource = new BitmapImage(new Uri("ms-appx:///Assets/Files/FluentIcons/Package.png"));
                    button.Content = new Image { Width = 28, Height = 28, Stretch = Stretch.Uniform, Source = _packageIconSource };
                    button.Click += (s, e) => installer.ActivateFromTaskbar();
                    break;
                case FileOperationModel operation:
                    button.Content = new FontIcon { FontFamily = new FontFamily("Segoe MDL2 Assets"), Glyph = "\uE895", FontSize = 22 };
                    ToolTipService.SetToolTip(button, operation.Title);
                    button.Click += (s, e) => FileOperationService.Instance.Activate(operation);
                    break;
                case "CoreShell.Files":
                    button.Content = CreateFilesIcon(28);
                    ToolTipService.SetToolTip(button, "Files");
                    button.Click += (s, e) => AppState.Instance.RequestFilesOpen();
                    break;
                case "CoreShell.Notepad":
                    button.Content = CreateNotepadIcon(27);
                    ToolTipService.SetToolTip(button, "Notepad");
                    button.Click += (s, e) => AppState.Instance.RequestNotepadOpen();
                    break;
                case "CoreShell.Settings":
                    button.Content = CreateSettingsIcon(25);
                    ToolTipService.SetToolTip(button, "Settings");
                    button.Click += (s, e) => AppState.Instance.RequestSettingsOpen();
                    break;
                case "CoreShell.Calculator":
                    button.Content = CreateCalculatorIcon(24);
                    ToolTipService.SetToolTip(button, "Calculator");
                    button.Click += (s, e) => AppState.Instance.RequestCalculatorOpen();
                    break;
                case "CoreShell.Firefox":
                    button.Content = CreateFirefoxIcon(26);
                    ToolTipService.SetToolTip(button, "Strawfox");
                    button.Click += (s, e) => AppState.Instance.RequestFirefoxOpen();
                    break;
            }
            return button;
        }
    }

    public sealed class Windows11SearchButton : Button
    {
        private bool _showSearchBox;

        internal void SetSearchBoxMode(bool showSearchBox)
        {
            _showSearchBox = showSearchBox;
            Width = showSearchBox ? 160 : 44;
            VisualStateManager.GoToState(this, showSearchBox ? "SearchBox" : "SearchIcon", false);
        }


        protected override void OnApplyTemplate()
        {
            base.OnApplyTemplate();
            SetSearchBoxMode(_showSearchBox);
        }
    }

    public sealed class ShellTaskbarButton : Button
    {
        private bool _isOpen;
        private bool _isActive;
        private bool _isGrouped;

        public void SetWindowState(bool isOpen, bool isActive, bool isGrouped)
        {
            if (_isOpen == isOpen && _isActive == isActive && _isGrouped == isGrouped)
                return;

            _isOpen = isOpen;
            _isActive = isActive;
            _isGrouped = isGrouped;
            UpdateWindowStates();
        }

        protected override void OnApplyTemplate()
        {
            base.OnApplyTemplate();
            UpdateWindowStates();
        }

        private void UpdateWindowStates()
        {
            VisualStateManager.GoToState(this, _isGrouped ? "MultipleWindows" : "SingleWindow", false);
            string state = !_isOpen ? "ClosedApp" : _isActive ? "ActiveApp" : "InactiveApp";
            VisualStateManager.GoToState(this, state, false);
        }
    }
}
