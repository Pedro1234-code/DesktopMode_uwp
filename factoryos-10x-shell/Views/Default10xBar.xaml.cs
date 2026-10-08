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
using Windows.UI.Xaml.Media;
using Windows.UI.Xaml.Media.Imaging;
using factoryos_10x_shell.Library.Services.WebApps;
using factoryos_10x_shell.Services.Win32;
using factoryos_10x_shell.Services.Windowing;


namespace factoryos_10x_shell.Views
{
    public sealed partial class Default10xBar : Page
    {
        private readonly AppHelper _appHelper;
        private readonly IWindowManagerService _windowManager;
        private readonly IWebAppService _webAppService;
        private readonly Win32WindowManagerService _nativeWindowManager;
        private readonly ShellWindowCoordinator _shellWindowCoordinator;


        public Default10xBar()
        {
            this.InitializeComponent();

            DataContext = App.ServiceProvider.GetRequiredService<Default10xBarViewModel>();
            _appHelper = App.ServiceProvider.GetRequiredService<AppHelper>();
            _windowManager = App.ServiceProvider.GetRequiredService<IWindowManagerService>();
            _webAppService = App.ServiceProvider.GetRequiredService<IWebAppService>();
            _nativeWindowManager = Win32WindowManagerService.Instance;
            _shellWindowCoordinator = App.ServiceProvider.GetRequiredService<ShellWindowCoordinator>();

            AppState.Instance.OnSearchButtonVisibilityChanged += UpdateSearchButtonVisibility;
            AppState.Instance.OnCopilotButtonVisibilityChanged += UpdateCopilotButtonVisibility;

            UpdateSearchButtonVisibility(AppState.Instance.IsSearchButtonVisible);
            UpdateCopilotButtonVisibility(AppState.Instance.IsCopilotButtonVisible);

            Loaded += Default10xBar_Loaded; // Chamada após a view estar carregada
        }

        private async void Default10xBar_Loaded(object sender, RoutedEventArgs e)
        {
            await _appHelper.LoadPinnedTaskbarAppsAsync();
            RefreshPinnedApps();
            _appHelper.TaskbarIcons.CollectionChanged += (s, args) => RefreshInternalTaskbar();
            _windowManager.Windows.CollectionChanged += (s, args) =>
            {
                RefreshOpenWebApps();
                RefreshPinnedWebApps();
            };
            _windowManager.WindowsChanged += (s, args) =>
            {
                RefreshOpenWebApps();
                RefreshPinnedWebApps();
            };
            _webAppService.InstalledApps.CollectionChanged += (s, args) =>
            {
                RefreshOpenWebApps();
                RefreshPinnedWebApps();
            };
            _webAppService.AppsChanged += (s, args) =>
            {
                RefreshOpenWebApps();
                RefreshPinnedWebApps();
            };
            _nativeWindowManager.Windows.CollectionChanged += (s, args) => RefreshOpenWin32Apps();
            _nativeWindowManager.WindowsChanged += (s, args) => RefreshOpenWin32Apps();
            AppState.Instance.OnFilesStateChanged += RefreshInternalTaskbar;
            AppState.Instance.OnNotepadStateChanged += RefreshInternalTaskbar;
            AppState.Instance.OnSettingsStateChanged += RefreshInternalTaskbar;
            AppState.Instance.OnCalculatorStateChanged += RefreshInternalTaskbar;
            AppState.Instance.OnFirefoxStateChanged += RefreshInternalTaskbar;
            _shellWindowCoordinator.StateChanged += (s, args) => RefreshInternalTaskbar();
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
                    stack.Children.Add(new Image { Width = 32, Height = 32, Source = app.IconSource });
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
                var newWindowItem = new MenuFlyoutItem { Text = "New window" };
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

        private static Image CreateFilesIcon(double size)
        {
            return new Image
            {
                Width = size,
                Height = size,
                Source = new BitmapImage(new Uri("ms-appx:///Assets/Files/files.png")),
                Stretch = Stretch.Uniform
            };
        }

        private static Image CreateNotepadIcon(double size)
        {
            return new Image
            {
                Source = new BitmapImage(new Uri("ms-appx:///Assets/Notepad/notepad.png")),
                Width = size,
                Height = size,
                HorizontalAlignment = HorizontalAlignment.Center,
                VerticalAlignment = VerticalAlignment.Center,
                Stretch = Stretch.Uniform
            };
        }

        private static Image CreateSettingsIcon(double size) => new Image { Source = new BitmapImage(new Uri("ms-appx:///Windows10x-js-main/Icons/WindowsSettings.png")), Width = size, Height = size, HorizontalAlignment = HorizontalAlignment.Center, VerticalAlignment = VerticalAlignment.Center, Stretch = Stretch.Uniform };

        private static Image CreateCalculatorIcon(double size) => new Image { Source = new BitmapImage(new Uri("ms-appx:///Assets/Calculator/CalculatorAppList.targetsize-48.png")), Width = size, Height = size, HorizontalAlignment = HorizontalAlignment.Center, VerticalAlignment = VerticalAlignment.Center, Stretch = Stretch.Uniform };

        private static Image CreateFirefoxIcon(double size) => new Image { Source = new BitmapImage(new Uri("ms-appx:///Assets/Firefox/firefox.png")), Width = size, Height = size, HorizontalAlignment = HorizontalAlignment.Center, VerticalAlignment = VerticalAlignment.Center, Stretch = Stretch.Uniform };

        private void RefreshInternalTaskbar()
        {
            RefreshFilesTaskbar();
            RefreshPinnedApps();
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
            if (AppState.Instance.IsFilesOpen && !_appHelper.TaskbarIcons.Any(app => app.AppId == "CoreShell.Files"))
            {
                var filesButton = new Button { Width = 48, Height = 48, Margin = new Thickness(2), Style = (Style)Application.Current.Resources["TaskbarButtonStyle"], Background = GetPinnedAppBackground("CoreShell.Files"), Content = CreateFilesIcon(28) };
                ToolTipService.SetToolTip(filesButton, "Files");
                filesButton.Click += (sender, args) => AppState.Instance.RequestFilesOpen();
                OpenFilesPanel.Children.Add(filesButton);
            }
            if (AppState.Instance.IsNotepadOpen && !_appHelper.TaskbarIcons.Any(app => app.AppId == "CoreShell.Notepad"))
            {
                var button = new Button { Width = 48, Height = 48, Margin = new Thickness(2), Style = (Style)Application.Current.Resources["TaskbarButtonStyle"], Background = GetPinnedAppBackground("CoreShell.Notepad"), Content = CreateNotepadIcon(27) };
                ToolTipService.SetToolTip(button, "Notepad");
                button.Click += (sender, args) => AppState.Instance.RequestNotepadOpen();
                OpenFilesPanel.Children.Add(button);
            }
            if (AppState.Instance.IsSettingsOpen && !_appHelper.TaskbarIcons.Any(app => app.AppId == "CoreShell.Settings"))
            {
                var settingsButton = new Button { Width = 48, Height = 48, Margin = new Thickness(2), Style = (Style)Application.Current.Resources["TaskbarButtonStyle"], Background = GetPinnedAppBackground("CoreShell.Settings"), Content = CreateSettingsIcon(25) };
                ToolTipService.SetToolTip(settingsButton, "Settings");
                settingsButton.Click += (sender, args) => AppState.Instance.RequestSettingsOpen();
                OpenFilesPanel.Children.Add(settingsButton);
            }
            if (AppState.Instance.IsCalculatorOpen && !_appHelper.TaskbarIcons.Any(app => app.AppId == "CoreShell.Calculator"))
            {
                var calculatorButton = new Button { Width = 48, Height = 48, Margin = new Thickness(2), Style = (Style)Application.Current.Resources["TaskbarButtonStyle"], Background = GetPinnedAppBackground("CoreShell.Calculator"), Content = CreateCalculatorIcon(24) };
                ToolTipService.SetToolTip(calculatorButton, "Calculator");
                calculatorButton.Click += (sender, args) => AppState.Instance.RequestCalculatorOpen();
                OpenFilesPanel.Children.Add(calculatorButton);
            }
            if (AppState.Instance.IsFirefoxOpen && !_appHelper.TaskbarIcons.Any(app => app.AppId == "CoreShell.Firefox"))
            {
                var firefoxButton = new Button { Width = 48, Height = 48, Margin = new Thickness(2), Style = (Style)Application.Current.Resources["TaskbarButtonStyle"], Background = GetPinnedAppBackground("CoreShell.Firefox"), Content = CreateFirefoxIcon(26) };
                ToolTipService.SetToolTip(firefoxButton, "Firefox");
                firefoxButton.Click += (sender, args) => AppState.Instance.RequestFirefoxOpen();
                OpenFilesPanel.Children.Add(firefoxButton);
            }
        }
        private void UpdateSearchButtonVisibility(bool isVisible)
        {
            SearchButton.Visibility = isVisible ? Visibility.Visible : Visibility.Collapsed;
        }

        private void UpdateCopilotButtonVisibility(bool isVisible)
        {
            CopilotButton.Visibility = isVisible ? Visibility.Visible : Visibility.Collapsed;
        }

        private async void CopilotButton_Click(object sender, RoutedEventArgs e)
        {
            var packageFamilyName = "MobileOSdev.CopilotPWA_d7x680j9yw8bm";
            var pm = new Windows.Management.Deployment.PackageManager();
            var packages = pm.FindPackagesForUser(string.Empty, packageFamilyName);

            var foundPackage = packages.FirstOrDefault();
            if (foundPackage != null)
            {
                var appListEntries = await foundPackage.GetAppListEntriesAsync();
                var entry = appListEntries.FirstOrDefault();
                if (entry != null)
                {
                    bool success = await entry.LaunchAsync();
                }
            }
        }
    }
}
