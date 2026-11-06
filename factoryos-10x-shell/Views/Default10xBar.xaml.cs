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
        private readonly BitmapImage _filesIconSource = new BitmapImage(new Uri("ms-appx:///Assets/Files/files.png"));
        private readonly BitmapImage _notepadIconSource = new BitmapImage(new Uri("ms-appx:///Assets/Notepad/notepad.png"));
        private readonly BitmapImage _settingsIconSource = new BitmapImage(new Uri("ms-appx:///Windows10x-js-main/Icons/WindowsSettings.png"));
        private readonly BitmapImage _calculatorIconSource = new BitmapImage(new Uri("ms-appx:///Assets/Calculator/CalculatorAppList.targetsize-48.png"));
        private readonly BitmapImage _firefoxIconSource = new BitmapImage(new Uri("ms-appx:///Assets/Firefox/firefox.png"));


        private void ApplyTaskbarAlignment()
        { TaskbarButtons.HorizontalAlignment = AppState.Instance.TaskbarAlignLeft ? HorizontalAlignment.Left : HorizontalAlignment.Center; }

        public Default10xBar()
        {
            this.InitializeComponent();
            ApplyTaskbarAlignment();
            Loaded += (s, e) => { ApplyTaskbarAlignment(); AppState.Instance.TaskbarPreferencesChanged += ApplyTaskbarAlignment; };
            Unloaded += (s, e) => AppState.Instance.TaskbarPreferencesChanged -= ApplyTaskbarAlignment;

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
            };
            _windowManager.Windows.CollectionChanged += (s, args) =>
            {
                RefreshOpenWebApps();
                RefreshPinnedWebApps();
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
                var button = new Button
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
                    stack.Children.Add(image);
                }
                button.Content = stack;

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
                        bool launched = await _appHelper.LaunchAppAsync(model);
                        if (!launched)
                            System.Diagnostics.Debug.WriteLine($"App não iniciado: {model.AppId} / {model.AppUri}");
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
                            RefreshPinnedApps(); 
                        }
                    };

                    flyout.Items.Add(unpinItem);
                    flyout.ShowAt(button, e.GetPosition(button));
                };


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
                var button = new Button
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
                var button = new Button
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
                PinnedWebAppsPanel.Children.Add(button);
            }
        }

        private void RefreshOpenWin32Apps()
        {
            OpenWin32AppsPanel.Children.Clear();
            foreach (Win32WindowModel window in _nativeWindowManager.Windows)
            {
                bool isActive = window.IsActive && window.Visibility == Visibility.Visible;
                var button = new Button
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
                OpenWin32AppsPanel.Children.Add(button);
            }
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
            }
        }

        private void UpdateWin32TaskbarVisualState()
        {
            foreach (Button button in OpenWin32AppsPanel.Children.OfType<Button>())
            {
                if (!(button.Tag is Win32WindowModel window)) continue;
                bool isActive = window.IsActive && window.Visibility == Visibility.Visible;
                button.Background = new SolidColorBrush(
                    Color.FromArgb(isActive ? (byte)80 : (byte)48, 70, 130, 180));
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
                Source = _filesIconSource,
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
                if (button.Tag is FileOperationModel operation)
                {
                    button.Background = new SolidColorBrush(Color.FromArgb(
                        _shellWindowCoordinator.IsActive(ShellWindowKind.FileOperation, operation) ? (byte)80 : (byte)48, 70, 130, 180));
                    continue;
                }
                string appId = (button.Tag as StartIconModel)?.AppId ?? button.Tag as string;
                if (!string.IsNullOrEmpty(appId) &&
                    appId.StartsWith("CoreShell.", StringComparison.OrdinalIgnoreCase))
                {
                    button.Background = GetPinnedAppBackground(appId);
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

        private void RefreshFilesTaskbar()
        {
            RefreshNotepadTaskbar();
        }

        private void RefreshNotepadTaskbar()
        {
            OpenFilesPanel.Children.Clear();
            foreach (var item in _shellWindowCoordinator.Windows.Where(item => item.Kind == ShellWindowKind.AppInstaller))
            {
                var installer = item.Identity as factoryos_10x_shell.Controls.AppInstallerWindow;
                if (installer == null) continue;
                var button = new Button {
                    Width = 48, Height = 48, Margin = new Thickness(2),
                    Style = (Style)Application.Current.Resources["TaskbarButtonStyle"],
                    Content = new Image { Width = 28, Height = 28, Stretch = Stretch.Uniform,
                        Source = new BitmapImage(new Uri("ms-appx:///Assets/Files/FluentIcons/Package.png")) }
                };
                ToolTipService.SetToolTip(button, "App Installer — " + installer.PackageTitle);
                button.Click += (s, e) => installer.ActivateFromTaskbar();
                OpenFilesPanel.Children.Add(button);
            }
            foreach (FileOperationModel operation in FileOperationService.Instance.Windows)
            {
                var operationButton = new Button
                {
                    Width = 48, Height = 48, Margin = new Thickness(2), Tag = operation,
                    Style = (Style)Application.Current.Resources["TaskbarButtonStyle"],
                    Content = new FontIcon { FontFamily = new FontFamily("Segoe MDL2 Assets"), Glyph = "\uE895", FontSize = 22 }
                };
                ToolTipService.SetToolTip(operationButton, operation.Title);
                operationButton.Click += (s, e) => FileOperationService.Instance.Activate(operation);
                OpenFilesPanel.Children.Add(operationButton);
            }
            if (AppState.Instance.IsFilesOpen && !_appHelper.TaskbarIcons.Any(app => app.AppId == "CoreShell.Files"))
            {
                var filesButton = new Button { Width = 48, Height = 48, Margin = new Thickness(2), Tag = "CoreShell.Files", Style = (Style)Application.Current.Resources["TaskbarButtonStyle"], Background = GetPinnedAppBackground("CoreShell.Files"), Content = CreateFilesIcon(28) };
                ToolTipService.SetToolTip(filesButton, "Files");
                filesButton.Click += (sender, args) => AppState.Instance.RequestFilesOpen();
                OpenFilesPanel.Children.Add(filesButton);
            }
            if (AppState.Instance.IsNotepadOpen && !_appHelper.TaskbarIcons.Any(app => app.AppId == "CoreShell.Notepad"))
            {
                var button = new Button { Width = 48, Height = 48, Margin = new Thickness(2), Tag = "CoreShell.Notepad", Style = (Style)Application.Current.Resources["TaskbarButtonStyle"], Background = GetPinnedAppBackground("CoreShell.Notepad"), Content = CreateNotepadIcon(27) };
                ToolTipService.SetToolTip(button, "Notepad");
                button.Click += (sender, args) => AppState.Instance.RequestNotepadOpen();
                OpenFilesPanel.Children.Add(button);
            }
            if (AppState.Instance.IsSettingsOpen && !_appHelper.TaskbarIcons.Any(app => app.AppId == "CoreShell.Settings"))
            {
                var settingsButton = new Button { Width = 48, Height = 48, Margin = new Thickness(2), Tag = "CoreShell.Settings", Style = (Style)Application.Current.Resources["TaskbarButtonStyle"], Background = GetPinnedAppBackground("CoreShell.Settings"), Content = CreateSettingsIcon(25) };
                ToolTipService.SetToolTip(settingsButton, "Settings");
                settingsButton.Click += (sender, args) => AppState.Instance.RequestSettingsOpen();
                OpenFilesPanel.Children.Add(settingsButton);
            }
            if (AppState.Instance.IsCalculatorOpen && !_appHelper.TaskbarIcons.Any(app => app.AppId == "CoreShell.Calculator"))
            {
                var calculatorButton = new Button { Width = 48, Height = 48, Margin = new Thickness(2), Tag = "CoreShell.Calculator", Style = (Style)Application.Current.Resources["TaskbarButtonStyle"], Background = GetPinnedAppBackground("CoreShell.Calculator"), Content = CreateCalculatorIcon(24) };
                ToolTipService.SetToolTip(calculatorButton, "Calculator");
                calculatorButton.Click += (sender, args) => AppState.Instance.RequestCalculatorOpen();
                OpenFilesPanel.Children.Add(calculatorButton);
            }
            if (AppState.Instance.IsFirefoxOpen && !_appHelper.TaskbarIcons.Any(app => app.AppId == "CoreShell.Firefox"))
            {
                var firefoxButton = new Button { Width = 48, Height = 48, Margin = new Thickness(2), Tag = "CoreShell.Firefox", Style = (Style)Application.Current.Resources["TaskbarButtonStyle"], Background = GetPinnedAppBackground("CoreShell.Firefox"), Content = CreateFirefoxIcon(26) };
                ToolTipService.SetToolTip(firefoxButton, "Strawfox");
                firefoxButton.Click += (sender, args) => AppState.Instance.RequestFirefoxOpen();
                OpenFilesPanel.Children.Add(firefoxButton);
            }
            UpdateInternalTaskbarVisualState();
        }
    }
}
