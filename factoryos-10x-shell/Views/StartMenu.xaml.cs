using factoryos_10x_shell.Library.Models.InternalData;
using factoryos_10x_shell.Library.Services.Helpers;
using factoryos_10x_shell;
using factoryos_10x_shell.Library.Services.Managers;
using factoryos_10x_shell.Library.ViewModels;
using factoryos_10x_shell.Services.Helpers;
using factoryos_10x_shell.Services.Win32;
using factoryos_10x_shell.Library.Services.WebApps;
using Microsoft.Extensions.DependencyInjection;
using System;
using System.Collections.Generic;
using System.Collections.ObjectModel;
using System.Collections.Specialized;
using System.Globalization;
using System.Diagnostics;
using System.IO;
using System.Linq;
using System.Runtime.InteropServices.WindowsRuntime;
using System.Runtime.Serialization.Json;
using System.Threading.Tasks;
using Windows.ApplicationModel;
using Windows.ApplicationModel.Core;
using Windows.Devices.Input;
using Windows.UI.ViewManagement;
using Windows.Foundation;
using Windows.Foundation.Collections;
using Windows.Graphics.Imaging;
using Windows.Management.Deployment;
using Windows.Networking.Proximity;
using Windows.Storage;
using Windows.Storage.Streams;
using Windows.System;
using Windows.System.UserProfile;
using Windows.UI.Input.Preview.Injection;
using Windows.UI.Xaml;
using Windows.UI.Xaml.Controls;
using Windows.UI.Xaml.Controls.Primitives;
using Windows.UI.Xaml.Data;
using Windows.UI.Xaml.Input;
using Windows.UI.Xaml.Media;
using Windows.UI.Xaml.Media.Imaging;
using Windows.UI.Xaml.Navigation;
using static factoryos_10x_shell.Helpers.VisualHelper;
using static System.Net.Mime.MediaTypeNames;

namespace factoryos_10x_shell.Views
{
    public sealed partial class StartMenu : Page
    {

        private readonly IAppHelper appHelper;
        private readonly IWebAppService webAppService;
        private readonly IWindowManagerService windowManager;
        private readonly IStartManagerService startManager;
        private bool m_searching;
        private bool m_refreshQueued;
        private bool m_shortcutsLoading;
        private IReadOnlyList<StartIconModel> m_shortcuts = Array.Empty<StartIconModel>();
        private InputPane m_searchInputPane;
        public ObservableCollection<StartIconModel> FilteredApps { get; } = new ObservableCollection<StartIconModel>();
        public ObservableCollection<WebAppDefinition> FilteredWebApps { get; } = new ObservableCollection<WebAppDefinition>();

        public StartMenu()
        {
            this.InitializeComponent();
            Loaded += (s, e) =>
            {
                AppState.Instance.ShellSkinChanged += ApplyShellSkin;
                ApplyShellSkin();
            };
            Unloaded += (s, e) => AppState.Instance.ShellSkinChanged -= ApplyShellSkin;
            ActualThemeChanged += (s, e) => ApplyShellSkin();
            appHelper = App.ServiceProvider.GetRequiredService<IAppHelper>();
            webAppService = App.ServiceProvider.GetRequiredService<IWebAppService>();
            windowManager = App.ServiceProvider.GetRequiredService<IWindowManagerService>();
            startManager = App.ServiceProvider.GetRequiredService<IStartManagerService>();
            DataContext = App.ServiceProvider.GetRequiredService<StartMenuViewModel>();
            Loaded += (s, e) =>
            {
                UpdateSearchKeyboardPolicy();
                m_searchInputPane = InputPane.GetForCurrentView();
                m_searchInputPane.Showing += SearchInputPane_Showing;
                ViewModel.StartIcons.CollectionChanged += SearchSource_CollectionChanged;
                ViewModel.WebApps.CollectionChanged += SearchSource_CollectionChanged;
                RefreshSearchResults();
                _ = RefreshWin32ShortcutsAsync();
            };
            Unloaded += (s, e) =>
            {
                if (m_searchInputPane != null)
                    m_searchInputPane.Showing -= SearchInputPane_Showing;
                m_searchInputPane = null;
                ViewModel.StartIcons.CollectionChanged -= SearchSource_CollectionChanged;
                ViewModel.WebApps.CollectionChanged -= SearchSource_CollectionChanged;
            };
            RefreshSearchResults();
        }

        public ObservableCollection<StartIconModel> Windows11PinnedApps { get; } = new ObservableCollection<StartIconModel>();
        public ObservableCollection<StartIconModel> Windows11ListedApps { get; } = new ObservableCollection<StartIconModel>();
        public ObservableCollection<StartIconModel> Windows11RecentApps { get; } = new ObservableCollection<StartIconModel>();
        private readonly List<string> m_windows11RecentIds = new List<string>();
        private bool m_windows11ShowAll;
        private const string Windows11PinsSetting = "Windows11.StartPins";

        private List<string> GetWindows11PinIds()
        {
            if (ApplicationData.Current.LocalSettings.Values[Windows11PinsSetting] is string saved)
                return saved.Split(new[] { '\n' }, StringSplitOptions.RemoveEmptyEntries).ToList();
            return new List<string> { "CoreShell.Files", "CoreShell.Firefox", "CoreShell.Notepad",
                "CoreShell.Calculator", "CoreShell.Settings" };
        }

        private void UpdateWindows11Start()
        {
            if (!AppState.Instance.UseWindows11Skin) return;

            var apps = FilteredApps.Select(app => app.AppId == "CoreShell.Files" ? new StartIconModel
            {
                AppId = app.AppId,
                IconName = app.IconName,
                IconSource = (ImageSource)Windows.UI.Xaml.Application.Current.Resources["Windows11FilesIcon"],
                IconGlyph = string.Empty
            } : app).Concat(FilteredWebApps.Select(app => new StartIconModel
            {
                AppId = "web:" + app.Id,
                IconName = app.Name,
                IconSource = Uri.TryCreate(app.IconUri, UriKind.Absolute, out var icon)
                    ? new BitmapImage(icon) : null,
                IconGlyph = string.IsNullOrEmpty(app.IconUri) ? "\uE774" : string.Empty,
                Data = app
            })).OrderBy(app => app.IconName, StringComparer.CurrentCultureIgnoreCase).ToList();

            bool list = m_searching || m_windows11ShowAll;
            Windows11Pins.Visibility = list ? Visibility.Collapsed : Visibility.Visible;
            Windows11Recommended.Visibility = Windows11Pins.Visibility;
            Windows11AllApps.Visibility = list ? Visibility.Visible : Visibility.Collapsed;
            Windows11StartHeading.Text = m_searching ? "Search results" : list ? "All apps" : "Pinned";
            Windows11AppsToggle.Visibility = m_searching ? Visibility.Collapsed : Visibility.Visible;
            Windows11AppsToggle.Content = list ? "‹  Back" : "All apps  ›";
            Windows11NoResults.Visibility = list && apps.Count == 0 ? Visibility.Visible : Visibility.Collapsed;

            if (list)
            {
                Windows11ListedApps.Clear();
                foreach (var app in apps) Windows11ListedApps.Add(app);
                return;
            }

            Windows11PinnedApps.Clear();
            foreach (var id in GetWindows11PinIds())
            {
                var app = apps.FirstOrDefault(item => item.AppId == id);
                if (app != null) Windows11PinnedApps.Add(app);
            }
            Windows11RecentApps.Clear();
            foreach (var id in m_windows11RecentIds)
            {
                var app = apps.FirstOrDefault(item => item.AppId == id);
                if (app != null) Windows11RecentApps.Add(app);
            }
            Windows11RecentHint.Visibility = Windows11RecentApps.Count == 0 ? Visibility.Visible : Visibility.Collapsed;
        }

        private void Windows11AppsToggle_Click(object sender, RoutedEventArgs e)
        {
            m_windows11ShowAll = !m_windows11ShowAll;
            UpdateWindows11Start();
        }

        private async Task LaunchWindows11AppAsync(StartIconModel app)
        {
            if (app == null) return;
            if (!string.IsNullOrEmpty(app.AppId))
            {
                m_windows11RecentIds.Remove(app.AppId);
                m_windows11RecentIds.Insert(0, app.AppId);
                if (m_windows11RecentIds.Count > 6) m_windows11RecentIds.RemoveAt(6);
            }
            m_windows11ShowAll = false;
            if (app.Data is WebAppDefinition webApp)
            {
                windowManager.Open(webApp);
                startManager.RequestStartVisibilityChange(false);
            }
            else await LaunchStartAppAsync(app);
            UpdateWindows11Start();
        }

        private async void Windows11App_Click(object sender, RoutedEventArgs e) =>
            await LaunchWindows11AppAsync((sender as FrameworkElement)?.DataContext as StartIconModel);

        private void Windows11App_RightTapped(object sender, RightTappedRoutedEventArgs e)
        {
            if (!(sender is FrameworkElement target) || !(target.DataContext is StartIconModel app)) return;
            var menu = new MenuFlyout();
            menu.Opened += ContextFlyout_Opened;
            menu.Closed += ContextFlyout_Closed;
            var ids = GetWindows11PinIds();
            if (!string.IsNullOrEmpty(app.AppId))
            {
                bool pinned = ids.Contains(app.AppId);
                var pin = new MenuFlyoutItem { Text = pinned ? "Unpin from Start" : "Pin to Start" };
                pin.Click += (s, args) =>
                {
                    if (pinned) ids.Remove(app.AppId);
                    else if (!ids.Contains(app.AppId)) ids.Add(app.AppId);
                    ApplicationData.Current.LocalSettings.Values[Windows11PinsSetting] = string.Join("\n", ids);
                    UpdateWindows11Start();
                };
                menu.Items.Add(pin);
            }
            if (app.Data is WebAppDefinition webApp)
            {
                var pin = new MenuFlyoutItem { Text = "Pin to taskbar", DataContext = webApp };
                pin.Click += PinWebApp_Click;
                menu.Items.Add(pin);
                var open = new MenuFlyoutItem { Text = webApp.NewInstanceText, DataContext = webApp };
                open.Click += NewWebAppWindow_Click;
                menu.Items.Add(open);
                var uninstall = new MenuFlyoutItem { Text = "Uninstall", DataContext = webApp };
                uninstall.Click += UninstallWebApp_Click;
                menu.Items.Add(uninstall);
            }
            else if (!(app.Data is Win32StartShortcut))
            {
                var pin = new MenuFlyoutItem { Text = "Pin to taskbar", DataContext = app };
                pin.Click += PinToTaskbar_Click;
                menu.Items.Add(pin);
            }
            if (menu.Items.Count == 0) return;
            menu.ShowAt(target);
            e.Handled = true;
        }

        private void ApplyShellSkin()
        {
            bool windows11 = AppState.Instance.UseWindows11Skin;
            bool dark = ActualTheme == ElementTheme.Dark;
            var skinVisibility = windows11 ? Visibility.Visible : Visibility.Collapsed;
            var legacyVisibility = windows11 ? Visibility.Collapsed : Visibility.Visible;

            Windows11StartPlate.Visibility = skinVisibility;
            Windows10XStartPlate.Visibility = legacyVisibility;
            Windows11StartBody.Visibility = skinVisibility;
            Windows10XContent.Visibility = legacyVisibility;
            Windows10XStartBody.Visibility = legacyVisibility;
            LegacyFooterSpacer.Visibility = legacyVisibility;
            AppsHeading.Text = windows11 ? "Apps" : "My apps";
            StartSurface.CornerRadius = new CornerRadius(windows11 ? 12 : 4);
            SearchRow.Height = new GridLength(windows11 ? 76 : 120);
            SearchPlate.CornerRadius = new CornerRadius(windows11 ? 20 : 3);
            AppSearchBox.PlaceholderText = windows11 ? "Search for apps" : "Search the web or your devices";
            AppListShow.CornerRadius = new CornerRadius(windows11 ? 5 : 10);
            PowerButton.Width = SettingsButton.Width = windows11 ? 48 : 75;

            if (windows11)
            {
                StartSurface.MinHeight = 0;
                Windows11StartPlate.BorderBrush = new SolidColorBrush(dark
                    ? Windows.UI.Color.FromArgb(42, 255, 255, 255)
                    : Windows.UI.Color.FromArgb(28, 0, 0, 0));
                Windows11BottomBar.Background = new SolidColorBrush(dark
                    ? Windows.UI.Color.FromArgb(30, 0, 0, 0)
                    : Windows.UI.Color.FromArgb(90, 255, 255, 255));
                UpdateWindows11Start();
            }
        }

        public StartMenuViewModel ViewModel => (StartMenuViewModel)this.DataContext;

        internal async Task RefreshWin32ShortcutsAsync()
        {
            if (m_shortcutsLoading) return;
            m_shortcutsLoading = true;
            try
            {
                m_shortcuts = await Win32StartShortcuts.ReadAsync();
                RefreshSearchResults();
                await Win32StartShortcuts.LoadIconsAsync(m_shortcuts);
            }
            catch (Exception exception)
            {
                Debug.WriteLine("Start shortcut scan failed: " + exception.Message);
            }
            finally
            {
                m_shortcutsLoading = false;
            }
        }

        private bool UpdateSearchKeyboardPolicy()
        {
            bool physicalKeyboard = new KeyboardCapabilities().KeyboardPresent != 0;
            AppSearchBox.PreventKeyboardDisplayOnProgrammaticFocus = physicalKeyboard;
            return physicalKeyboard;
        }

        private void AppSearchBox_PointerPressed(object sender, PointerRoutedEventArgs e)
        {
            UpdateSearchKeyboardPolicy();
        }

        private async void AppSearchBox_GotFocus(object sender, RoutedEventArgs e)
        {
            if (!UpdateSearchKeyboardPolicy()) return;
            m_searchInputPane?.TryHide();
            // The platform can enqueue a keyboard request after the focus event.
            await Task.Delay(150);
            if (AppSearchBox.FocusState != FocusState.Unfocused && UpdateSearchKeyboardPolicy())
                m_searchInputPane?.TryHide();
        }

        private async void SearchInputPane_Showing(InputPane sender, InputPaneVisibilityEventArgs args)
        {
            if (AppSearchBox.FocusState == FocusState.Unfocused || !UpdateSearchKeyboardPolicy()) return;
            await Dispatcher.RunAsync(Windows.UI.Core.CoreDispatcherPriority.Normal, () =>
            {
                if (AppSearchBox.FocusState != FocusState.Unfocused && UpdateSearchKeyboardPolicy())
                    sender.TryHide();
            });
        }

        private async void SearchSource_CollectionChanged(object sender, NotifyCollectionChangedEventArgs e)
        {
            if (m_refreshQueued) return;
            m_refreshQueued = true;
            try
            {
                await Dispatcher.RunAsync(Windows.UI.Core.CoreDispatcherPriority.Low, () =>
                {
                    m_refreshQueued = false;
                    RefreshSearchResults();
                    (appHelper as factoryos_10x_shell.Services.Helpers.AppHelper)?.RecordStartMenuProjection(
                        FilteredApps, AppListGrid.Items.Count, m_searching);
                });
            }
            finally { m_refreshQueued = false; }
        }

        private void AppSearchBox_TextChanged(object sender, TextChangedEventArgs e)
        {
            if (appHelper != null && DataContext != null) RefreshSearchResults();
        }

        private void RefreshSearchResults()
        {
            string query = (AppSearchBox.Text ?? string.Empty).Trim();
            bool searching = query.Length != 0;
            // Filtering changes the content's desired height. Keep the menu
            // at its pre-search height, with results scrolling inside it.
            if (searching && !m_searching && !AppState.Instance.UseWindows11Skin)
                StartSurface.MinHeight = StartSurface.ActualHeight;
            else if (!searching && m_searching)
                StartSurface.MinHeight = 0;
            m_searching = searching;
            bool Matches(string name) => !searching || CultureInfo.CurrentCulture.CompareInfo.IndexOf(
                name ?? string.Empty, query, CompareOptions.IgnoreCase | CompareOptions.IgnoreNonSpace) >= 0;

            FilteredApps.Clear();
            foreach (var app in ViewModel.StartIcons.Concat(m_shortcuts).Where(app => Matches(app.IconName)))
                FilteredApps.Add(app);
            FilteredWebApps.Clear();
            foreach (var app in ViewModel.WebApps.Where(app => Matches(app.Name))) FilteredWebApps.Add(app);

            ViewModel.AppsListGridHeight = searching || startManager.IsAppsListExpanded ? double.NaN : 310;
            AppListShow.Visibility = searching ? Visibility.Collapsed : Visibility.Visible;
            WebAppsSection.Visibility = searching && FilteredWebApps.Count == 0
                ? Visibility.Collapsed : Visibility.Visible;
            UpdateWindows11Start();
            NoSearchResults.Visibility = searching && FilteredApps.Count == 0 && FilteredWebApps.Count == 0
                ? Visibility.Visible : Visibility.Collapsed;
        }

        private async void AppSearchBox_KeyDown(object sender, KeyRoutedEventArgs e)
        {
            if (e.Key == VirtualKey.Escape)
            {
                AppSearchBox.Text = string.Empty;
                e.Handled = true;
            }
            else if (e.Key == VirtualKey.Enter && !string.IsNullOrWhiteSpace(AppSearchBox.Text))
            {
                e.Handled = true;
                if (AppState.Instance.UseWindows11Skin && m_searching)
                    await LaunchWindows11AppAsync(Windows11ListedApps.FirstOrDefault());
                else if (FilteredApps.Count > 0) await LaunchStartAppAsync(FilteredApps[0]);
                else if (FilteredWebApps.Count > 0)
                {
                    windowManager.Open(FilteredWebApps[0]);
                    startManager.RequestStartVisibilityChange(false);
                }
            }
        }


        private async void AppButton_Click(object sender, RoutedEventArgs e)
        {
            await LaunchStartAppAsync(((FrameworkElement)sender).DataContext as StartIconModel);
        }

        private async Task LaunchStartAppAsync(StartIconModel model)
        {
            if (model != null)
            {
                if (model.Data is Win32StartShortcut shortcut)
                {
                    var window = Win32WindowManagerService.Instance.Open(shortcut.Executable,
                        shortcut.Folder, model.IconSource, shortcut.VirtualPath,
                        shortcut.Arguments, shortcut.WorkingDirectory);
                    if (window != null) startManager.RequestStartVisibilityChange(false);
                    return;
                }
                if (model.AppId == "CoreShell.Files")
                {
                    AppState.Instance.RequestFilesOpen(true);
                    startManager.RequestStartVisibilityChange(false);
                    return;
                }
                if (model.AppId == "CoreShell.Notepad")
                {
                    AppState.Instance.RequestNotepadOpen();
                    startManager.RequestStartVisibilityChange(false);
                    return;
                }
                if (model.AppId == "CoreShell.Settings")
                {
                    AppState.Instance.RequestSettingsOpen();
                    startManager.RequestStartVisibilityChange(false);
                    return;
                }
                if (model.AppId == "CoreShell.Calculator")
                {
                    AppState.Instance.RequestCalculatorOpen();
                    startManager.RequestStartVisibilityChange(false);
                    return;
                }
                if (model.AppId == "CoreShell.Firefox")
                {
                    AppState.Instance.RequestFirefoxOpen();
                    startManager.RequestStartVisibilityChange(false);
                    return;
                }
                await appHelper.LaunchAppAsync(model);
                startManager.RequestStartVisibilityChange(false);

            }
        }

        private void PinToTaskbar_Click(object sender, RoutedEventArgs e)
        {
            if (sender is MenuFlyoutItem item &&
                item.DataContext is StartIconModel app)
            {
                if (app.Data is Win32StartShortcut) return;
                if (!appHelper.TaskbarIcons.Any(i => i.AppId == app.AppId))
                {
                    appHelper.TaskbarIcons.Add(app);
                    appHelper.SaveTaskbarPinnedApps(appHelper.TaskbarIcons.ToList());
                    Debug.WriteLine($"Salvando: AppId={app.AppId} / Aumid={app.Aumid ?? "NULL"} / Nome={app.IconName}");
                }
            }
        }

        private async void PowerButton_click(object sender, RoutedEventArgs e)
        {
            await Launcher.LaunchUriAsync(new Uri("powerdialogcomponent:"));
        }

        private async void InstallWebAppButton_Click(object sender, RoutedEventArgs e)
        {
            var nameBox = new TextBox { PlaceholderText = "App name (optional)" };
            var urlBox = new TextBox { PlaceholderText = "https://example.com" };
            var browserBox = new ComboBox { Header = "Open with", HorizontalAlignment = HorizontalAlignment.Stretch };
            browserBox.Items.Add("Edge WebView2");
            browserBox.Items.Add("Strawfox app (tab)");
            browserBox.Items.Add("Strawfox app window");
            browserBox.SelectedIndex = 2;
            var panel = new StackPanel { Spacing = 10 };
            panel.Children.Add(nameBox);
            panel.Children.Add(urlBox);
            panel.Children.Add(browserBox);
            var dialog = new ContentDialog { Title = "Install web app", Content = panel, PrimaryButtonText = "Install", CloseButtonText = "Cancel" };
            ContentDialogResult installResult;
            MainPage.SetPopupCursorVisibility(true);
            try { installResult = await dialog.ShowAsync(); }
            finally { MainPage.SetPopupCursorVisibility(false); }
            if (installResult != ContentDialogResult.Primary) return;

            try
            {
                var app = await webAppService.InstallAsync(nameBox.Text, urlBox.Text, browserBox.SelectedIndex != 0, browserBox.SelectedIndex == 2);
                windowManager.Open(app);
                startManager.RequestStartVisibilityChange(false);
            }
            catch (ArgumentException exception)
            {
                await new ContentDialog { Title = "Invalid address", Content = exception.Message, CloseButtonText = "OK" }.ShowAsync();
            }
        }

        private void WebAppButton_Click(object sender, RoutedEventArgs e)
        {
            if (((FrameworkElement)sender).DataContext is WebAppDefinition app)
            {
                windowManager.Open(app);
                startManager.RequestStartVisibilityChange(false);
            }
        }

        private void FilesButton_Click(object sender, RoutedEventArgs e)
        {
            AppState.Instance.RequestFilesOpen(true);
            startManager.RequestStartVisibilityChange(false);
        }

        private void PinWebApp_Click(object sender, RoutedEventArgs e)
        {
            if (((FrameworkElement)sender).DataContext is WebAppDefinition app)
                webAppService.SetPinned(app, true);
        }

        private void NewWebAppWindow_Click(object sender, RoutedEventArgs e)
        {
            if (((FrameworkElement)sender).DataContext is WebAppDefinition app)
            {
                windowManager.OpenNewWindow(app);
                startManager.RequestStartVisibilityChange(false);
            }
        }

        private void UninstallWebApp_Click(object sender, RoutedEventArgs e)
        {
            if (!(((FrameworkElement)sender).DataContext is WebAppDefinition app)) return;
            foreach (var window in windowManager.Windows.Where(window => window.App.Id == app.Id).ToList())
                windowManager.Close(window);
            webAppService.Uninstall(app);
        }

        private void SettingsButton_click(object sender, RoutedEventArgs e)
        {
            AppState.Instance.RequestSettingsOpen();
            startManager.RequestStartVisibilityChange(false);
        }

        private void ContextFlyout_Opened(object sender, object e)
        {
            if (sender is MenuFlyout menu && menu.Items.FirstOrDefault() is MenuFlyoutItem item &&
                item.DataContext is StartIconModel app && app.Data is Win32StartShortcut)
            {
                menu.Hide();
                return;
            }
            MainPage.SetPopupCursorVisibility(true);
        }
        private void ContextFlyout_Closed(object sender, object e) => MainPage.SetPopupCursorVisibility(false);

    }
}
