using factoryos_10x_shell.Library.Models.InternalData;
using factoryos_10x_shell.Library.Services.Helpers;
using factoryos_10x_shell;
using factoryos_10x_shell.Library.Services.Managers;
using factoryos_10x_shell.Library.ViewModels;
using factoryos_10x_shell.Services.Helpers;
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
        private InputPane m_searchInputPane;
        public ObservableCollection<StartIconModel> FilteredApps { get; } = new ObservableCollection<StartIconModel>();
        public ObservableCollection<WebAppDefinition> FilteredWebApps { get; } = new ObservableCollection<WebAppDefinition>();

        public StartMenu()
        {
            this.InitializeComponent();
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

        public StartMenuViewModel ViewModel => (StartMenuViewModel)this.DataContext;

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
            if (searching && !m_searching)
                StartSurface.MinHeight = StartSurface.ActualHeight;
            else if (!searching && m_searching)
                StartSurface.MinHeight = 0;
            m_searching = searching;
            bool Matches(string name) => !searching || CultureInfo.CurrentCulture.CompareInfo.IndexOf(
                name ?? string.Empty, query, CompareOptions.IgnoreCase | CompareOptions.IgnoreNonSpace) >= 0;

            FilteredApps.Clear();
            foreach (var app in ViewModel.StartIcons.Where(app => Matches(app.IconName))) FilteredApps.Add(app);
            FilteredWebApps.Clear();
            foreach (var app in ViewModel.WebApps.Where(app => Matches(app.Name))) FilteredWebApps.Add(app);

            ViewModel.AppsListGridHeight = searching || startManager.IsAppsListExpanded ? double.NaN : 310;
            AppListShow.Visibility = searching ? Visibility.Collapsed : Visibility.Visible;
            WebAppsSection.Visibility = searching && FilteredWebApps.Count == 0
                ? Visibility.Collapsed : Visibility.Visible;
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
                if (FilteredApps.Count > 0) await LaunchStartAppAsync(FilteredApps[0]);
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

        private void ContextFlyout_Opened(object sender, object e) => MainPage.SetPopupCursorVisibility(true);
        private void ContextFlyout_Closed(object sender, object e) => MainPage.SetPopupCursorVisibility(false);

    }
}
