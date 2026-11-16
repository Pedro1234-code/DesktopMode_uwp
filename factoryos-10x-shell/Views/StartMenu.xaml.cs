using factoryos_10x_shell.Library.Models.InternalData;
using factoryos_10x_shell.Library.Services.Helpers;
using factoryos_10x_shell;
using factoryos_10x_shell.Library.Services.Managers;
using factoryos_10x_shell.Library.ViewModels;
using factoryos_10x_shell.Services.Helpers;
using factoryos_10x_shell.Services.Files;
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
using System.Runtime.InteropServices;
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
using Windows.Globalization.Collation;
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
        private bool? m_windows11SearchStyle;
        private bool m_shortcutsLoading;
        private IReadOnlyList<StartIconModel> m_shortcuts = Array.Empty<StartIconModel>();
        private InputPane m_searchInputPane;
        public ObservableCollection<StartIconModel> FilteredApps { get; } = new ObservableCollection<StartIconModel>();
        public ObservableCollection<WebAppDefinition> FilteredWebApps { get; } = new ObservableCollection<WebAppDefinition>();

        public StartMenu()
        {
            this.InitializeComponent();
            Windows11PinsArea.AddHandler(UIElement.PointerWheelChangedEvent,
                new PointerEventHandler(Windows11Pins_PointerWheelChanged), true);
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
        private readonly CollectionViewSource m_windows11GroupedApps = new CollectionViewSource { IsSourceGrouped = true };
        private IReadOnlyList<StartIconModel> m_windows11SearchResults = Array.Empty<StartIconModel>();
        public ObservableCollection<StartIconModel> Windows11RecentApps { get; } = new ObservableCollection<StartIconModel>();
        private readonly List<string> m_windows11RecentIds = new List<string>();
        private bool m_windows11ShowAll;
        private bool m_windows11ShowAlphabet;
        private IReadOnlyList<Windows11AppGroup> m_windows11AppGroups = Array.Empty<Windows11AppGroup>();
        private readonly List<StartIconModel> m_windows11Pins = new List<StartIconModel>();
        private int m_windows11PinPage;
        private int m_windows11PinsPerPage = 18;
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
            Windows11PinnedScroll.Visibility = list ? Visibility.Collapsed : Visibility.Visible;
            if (m_searching || !m_windows11ShowAll) m_windows11ShowAlphabet = false;
            Windows11AlphabetView.Visibility = m_windows11ShowAlphabet ? Visibility.Visible : Visibility.Collapsed;
            Windows11AllApps.Visibility = list && !m_windows11ShowAlphabet ? Visibility.Visible : Visibility.Collapsed;
            Windows11StartHeading.Text = m_searching ? "Search results" : list ? "All apps" : "Pinned";
            Windows11AppsToggle.Visibility = m_searching ? Visibility.Collapsed : Visibility.Visible;
            Windows11AppsToggle.Padding = list ? new Thickness(6, 2, 7, 4) : new Thickness(7, 2, 6, 4);
            Windows11AppsToggleLabel.Text = list ? "Back" : "All apps";
            Windows11BackChevron.Visibility = list ? Visibility.Visible : Visibility.Collapsed;
            Windows11ForwardChevron.Visibility = list ? Visibility.Collapsed : Visibility.Visible;
            Windows11NoResults.Visibility = list && apps.Count == 0 ? Visibility.Visible : Visibility.Collapsed;

            if (list)
            {
                if (m_searching)
                {
                    m_windows11SearchResults = apps;
                    Windows11AllApps.ItemsSource = apps;
                }
                else
                {
                    var collation = new CharacterGroupings();
                    m_windows11AppGroups = apps
                        .GroupBy(app => GetWindows11AppGroupKey(app.IconName, collation), StringComparer.OrdinalIgnoreCase)
                        .OrderBy(group => group.Key == "&" ? 0 : group.Key == "#" ? 1 : group.Key == Windows11AppGroup.OtherScripts ? 3 : 2)
                        .ThenBy(group => group.Key, StringComparer.CurrentCultureIgnoreCase)
                        .Select(group => new Windows11AppGroup(group.Key, group))
                        .ToList();
                    m_windows11GroupedApps.Source = m_windows11AppGroups;
                    Windows11AllApps.ItemsSource = m_windows11GroupedApps.View;
                    if (m_windows11ShowAlphabet) UpdateWindows11Alphabet();
                }
                return;
            }

            m_windows11Pins.Clear();
            foreach (var id in GetWindows11PinIds())
            {
                var app = apps.FirstOrDefault(item => item.AppId == id);
                if (app != null) m_windows11Pins.Add(app);
            }
            ShowWindows11PinPage(m_windows11PinPage);
            Windows11RecentApps.Clear();
            foreach (var id in m_windows11RecentIds)
            {
                var app = apps.FirstOrDefault(item => item.AppId == id);
                if (app != null) Windows11RecentApps.Add(app);
            }
            Windows11RecentHint.Visibility = Windows11RecentApps.Count == 0 ? Visibility.Visible : Visibility.Collapsed;
        }

        private static string GetWindows11AppGroupKey(string name, CharacterGroupings collation)
        {
            if (string.IsNullOrWhiteSpace(name)) return "&";
            name = name.TrimStart();
            if (char.IsDigit(name, 0)) return "#";
            if (!char.IsLetter(name, 0)) return "&";

            string label = collation.Lookup(name);
            return !string.IsNullOrEmpty(label) && char.IsLetter(label, 0) ? label : Windows11AppGroup.OtherScripts;
        }


        private int Windows11PinPageCount =>
            Math.Max(1, (m_windows11Pins.Count + m_windows11PinsPerPage - 1) / m_windows11PinsPerPage);

        private void ShowWindows11PinPage(int page)
        {
            int count = Windows11PinPageCount;
            int next = Math.Max(0, Math.Min(page, count - 1));
            Windows11PinnedApps.Clear();
            foreach (var app in m_windows11Pins.Skip(next * m_windows11PinsPerPage).Take(m_windows11PinsPerPage))
                Windows11PinnedApps.Add(app);

            m_windows11PinPage = next;
            if (Windows11PinPager.Children.Count != count)
            {
                Windows11PinPager.Children.Clear();
                for (int index = 0; index < count; index++)
                {
                    var button = new Button
                    {
                        Tag = index, Width = 24, Height = 20, Padding = new Thickness(0),
                        Background = new SolidColorBrush(Windows.UI.Colors.Transparent),
                        BorderThickness = new Thickness(0), Content = new TextBlock { Text = "●", FontSize = 10 }
                    };
                    Windows.UI.Xaml.Automation.AutomationProperties.SetName(button, $"Pinned apps page {index + 1} of {count}");
                    button.Click += (sender, args) => ShowWindows11PinPage((int)((Button)sender).Tag);
                    Windows11PinPager.Children.Add(button);
                }
            }
            for (int index = 0; index < count; index++)
                ((TextBlock)((Button)Windows11PinPager.Children[index]).Content).Opacity = index == next ? 1 : 0.45;
            Windows11PinPager.Visibility = count > 1 ? Visibility.Visible : Visibility.Collapsed;
        }

        private void Windows11PinsArea_SizeChanged(object sender, SizeChangedEventArgs e)
        {
            if (e.NewSize.Width <= 24) return;
            int pageSize = 3 * Math.Max(1, Math.Min(6, (int)((e.NewSize.Width - 24) / 92)));
            if (pageSize == m_windows11PinsPerPage) return;
            int firstVisible = m_windows11PinPage * m_windows11PinsPerPage;
            m_windows11PinsPerPage = pageSize;
            ShowWindows11PinPage(firstVisible / pageSize);
        }

        private void Windows11Pins_PointerWheelChanged(object sender, PointerRoutedEventArgs e)
        {
            int delta = e.GetCurrentPoint(Windows11Pins).Properties.MouseWheelDelta;
            if (delta == 0) return;
            int page = m_windows11PinPage + (delta < 0 ? 1 : -1);
            if (page < 0 || page >= Windows11PinPageCount) return;
            ShowWindows11PinPage(page);
            e.Handled = true;
        }

        private void Windows11AlphabetHeader_Click(object sender, RoutedEventArgs e)
        {
            if (!(sender is Button header) || !m_windows11ShowAll || m_searching) return;
            m_windows11ShowAlphabet = true;
            Windows11AlphabetView.Visibility = Visibility.Visible;
            UpdateWindows11Alphabet(header.Tag as string);
            Windows11AllApps.Visibility = Visibility.Collapsed;
        }

        private void UpdateWindows11Alphabet(string focusedKey = null)
        {
            var available = m_windows11AppGroups.ToDictionary(group => group.Key, StringComparer.OrdinalIgnoreCase);
            var latin = "ABCDEFGHIJKLMNOPQRSTUVWXYZ".Select(letter => letter.ToString()).ToArray();
            var keys = new[] { "", "&", "#" }.Concat(latin)
                .Concat(m_windows11AppGroups.Select(group => group.Key)
                    .Where(key => key != "&" && key != "#" && key != Windows11AppGroup.OtherScripts &&
                                  !latin.Contains(key, StringComparer.OrdinalIgnoreCase)))
                .Concat(new[] { Windows11AppGroup.OtherScripts });

            var grid = Windows11AlphabetGrid;
            grid.Children.Clear();
            grid.RowDefinitions.Clear();
            grid.ColumnDefinitions.Clear();
            const int columns = 4;
            for (int column = 0; column < columns; column++)
                grid.ColumnDefinitions.Add(new ColumnDefinition { Width = GridLength.Auto });

            var style = (Style)Resources["Windows11StartEntryStyle"];
            int index = 0;
            foreach (string key in keys)
            {
                bool recent = key.Length == 0;
                bool otherScripts = key == Windows11AppGroup.OtherScripts;
                var button = new Button
                {
                    Tag = key, Style = style, Width = 52, Height = 52, Margin = new Thickness(2),
                    Padding = new Thickness(0), CornerRadius = new CornerRadius(4), IsEnabled = available.ContainsKey(key),
                    Content = new TextBlock
                    {
                        Text = recent ? "\uE823" : otherScripts ? "\uE774" : key,
                        FontFamily = new FontFamily(recent || otherScripts ? "Segoe MDL2 Assets" : "Segoe UI"),
                        FontSize = 20
                    }
                };
                Windows.UI.Xaml.Automation.AutomationProperties.SetName(button,
                    recent ? "Recently added" : otherScripts ? "Other scripts" : key);
                button.Click += Windows11AlphabetLetter_Click;
                if (index % columns == 0)
                    grid.RowDefinitions.Add(new RowDefinition { Height = GridLength.Auto });
                Grid.SetRow(button, index / columns);
                Grid.SetColumn(button, index % columns);
                grid.Children.Add(button);
                if (string.Equals(key, focusedKey, StringComparison.OrdinalIgnoreCase))
                    button.Focus(FocusState.Programmatic);
                index++;
            }
        }

        private void Windows11AlphabetLetter_Click(object sender, RoutedEventArgs e)
        {
            if (!(sender is Button button) || !(button.Tag is string key)) return;
            var group = m_windows11AppGroups.FirstOrDefault(item =>
                string.Equals(item.Key, key, StringComparison.OrdinalIgnoreCase));
            if (group == null || group.Count == 0) return;

            CloseWindows11Alphabet();
            Windows11AllApps.UpdateLayout();
            Windows11AllApps.ScrollIntoView(group[0], ScrollIntoViewAlignment.Leading);
        }

        private void Windows11AlphabetView_KeyDown(object sender, KeyRoutedEventArgs e)
        {
            if (e.Key != VirtualKey.Escape) return;
            e.Handled = true;
            CloseWindows11Alphabet();
        }

        private void CloseWindows11Alphabet()
        {
            m_windows11ShowAlphabet = false;
            Windows11AlphabetView.Visibility = Visibility.Collapsed;
            Windows11AllApps.Visibility = Visibility.Visible;
            Windows11AppsToggle.Focus(FocusState.Programmatic);
        }


        private void Windows11AppsToggle_Click(object sender, RoutedEventArgs e)
        {
            if (m_windows11ShowAlphabet)
            {
                CloseWindows11Alphabet();
                return;
            }

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
            else
            {
                var pin = new MenuFlyoutItem { DataContext = app };
                UpdateTaskbarPinMenuItem(pin, app);
                pin.Click += PinToTaskbar_Click;
                menu.Items.Add(pin);
            }
            var desktopShortcut = new MenuFlyoutItem { Text = "Create desktop shortcut", DataContext = app };
            desktopShortcut.Click += CreateDesktopShortcut_Click;
            menu.Items.Add(desktopShortcut);
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
            StartSurface.CornerRadius = new CornerRadius(windows11 ? 8 : 4);
            SearchRow.Height = new GridLength(windows11 ? 92 : 120);
            SearchPlate.Height = windows11 ? 40 : 48;
            SearchPlate.Margin = windows11 ? new Thickness(33, 14, 33, 0) : new Thickness(40, 0, 40, 0);
            SearchPlate.CornerRadius = new CornerRadius(windows11 ? 4 : 3);
            Windows11SearchBackground.Visibility = skinVisibility;
            Windows11SearchUnderline.Visibility = skinVisibility;
            SearchIcon.Margin = windows11 ? new Thickness(14, 0, 0, 0) : new Thickness(17, 16, 0, 0);
            SearchIcon.VerticalAlignment = windows11 ? VerticalAlignment.Center : VerticalAlignment.Top;
            SearchIcon.FontSize = windows11 ? 14 : 16;
            if (m_windows11SearchStyle != windows11)
            {
                m_windows11SearchStyle = windows11;
                if (windows11)
                {
                    AppSearchBox.ClearValue(FrameworkElement.StyleProperty);
                    AppSearchBox.BorderThickness = new Thickness(0);
                }
                else
                {
                    AppSearchBox.Style = (Style)Windows.UI.Xaml.Application.Current.Resources["TextBoxStyleBlack"];
                    AppSearchBox.ClearValue(Control.BorderThicknessProperty);
                }
            }
            AppSearchBox.Margin = windows11 ? new Thickness(6, 0, 8, 0) : new Thickness(10, 8, 10, 0);
            AppSearchBox.VerticalAlignment = windows11 ? VerticalAlignment.Center : VerticalAlignment.Top;
            AppSearchBox.PlaceholderText = windows11 ? "Type here to search" : "Search the web or your devices";
            AppListShow.CornerRadius = new CornerRadius(windows11 ? 5 : 10);
            PowerButton.Width = SettingsButton.Width = windows11 ? 48 : 75;

            if (windows11)
            {
                StartSurface.MinHeight = 0;
                Windows11SearchBackground.Background = new SolidColorBrush(dark
                    ? Windows.UI.Color.FromArgb(255, 31, 34, 40)
                    : Windows.UI.Color.FromArgb(255, 250, 250, 250));
                Windows11SearchBackground.BorderBrush = new SolidColorBrush(dark
                    ? Windows.UI.Color.FromArgb(42, 255, 255, 255)
                    : Windows.UI.Color.FromArgb(34, 0, 0, 0));
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

        internal void FocusTaskbarSearch()
        {
            if (!startManager.IsStartOpen || AppSearchBox.FocusState != FocusState.Unfocused) return;
            try
            {
                UpdateSearchKeyboardPolicy();
                ShellDiagnostics.RecordCheckpoint("Taskbar Search: requesting field focus");
                bool focused = AppSearchBox.Focus(FocusState.Programmatic);
                ShellDiagnostics.RecordCheckpoint($"Taskbar Search: field focus returned {focused}");
            }
            catch (COMException error)
            {
                ShellDiagnostics.RecordException("Taskbar Search: assigning focus to search field", error);
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
            if (!startManager.IsStartOpen || !UpdateSearchKeyboardPolicy()) return;
            // Let the platform finish assigning focus before interacting with the input pane.
            await Task.Delay(150);
            if (startManager.IsStartOpen && AppSearchBox.FocusState != FocusState.Unfocused && UpdateSearchKeyboardPolicy())
                TryHideSearchKeyboard();
        }

        private async void SearchInputPane_Showing(InputPane sender, InputPaneVisibilityEventArgs args)
        {
            if (!startManager.IsStartOpen || AppSearchBox.FocusState == FocusState.Unfocused || !UpdateSearchKeyboardPolicy()) return;
            await Dispatcher.RunAsync(Windows.UI.Core.CoreDispatcherPriority.Normal, () =>
            {
                if (startManager.IsStartOpen && AppSearchBox.FocusState != FocusState.Unfocused && UpdateSearchKeyboardPolicy())
                    TryHideSearchKeyboard();
            });
        }

        private void TryHideSearchKeyboard()
        {
            try
            {
                m_searchInputPane?.TryHide();
            }
            catch (COMException error)
            {
                ShellDiagnostics.RecordException("Start Search: dismissing input pane", error);
            }
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
                    await LaunchWindows11AppAsync(m_windows11SearchResults.FirstOrDefault());
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
            if (model == null) return;
            if (await DesktopItems.LaunchAppAsync(model, appHelper, true) || !(model.Data is Win32StartShortcut))
                startManager.RequestStartVisibilityChange(false);
        }

        private void PinToTaskbar_Click(object sender, RoutedEventArgs e)
        {
            if (!(sender is MenuFlyoutItem item) || !(item.DataContext is StartIconModel app) ||
                string.IsNullOrWhiteSpace(app.AppId)) return;

            var pinned = FindTaskbarPin(app);
            if (pinned == null)
                appHelper.TaskbarIcons.Add(app);
            else
                appHelper.TaskbarIcons.Remove(pinned);

            appHelper.SaveTaskbarPinnedApps(appHelper.TaskbarIcons.ToList());
        }

        private StartIconModel FindTaskbarPin(StartIconModel app) =>
            appHelper.TaskbarIcons.FirstOrDefault(item =>
                string.Equals(item.AppId, app.AppId, StringComparison.OrdinalIgnoreCase));

        private void UpdateTaskbarPinMenuItem(MenuFlyoutItem item, StartIconModel app) =>
            item.Text = FindTaskbarPin(app) == null ? "Pin to taskbar" : "Unpin from taskbar";

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

        private async void CreateDesktopShortcut_Click(object sender, RoutedEventArgs e)
        {
            if (!(sender is MenuFlyoutItem item)) return;
            try
            {
                if (item.DataContext is StartIconModel app)
                    await DesktopItems.CreateShortcutAsync(app.AppId, app.IconName);
                else if (item.DataContext is WebAppDefinition webApp)
                    await DesktopItems.CreateShortcutAsync("web:" + webApp.Id, webApp.Name);
            }
            catch (Exception error)
            {
                ShellDiagnostics.RecordException("Creating desktop shortcut", error);
                await new ContentDialog { Title = "Could not create desktop shortcut", CloseButtonText = "OK" }.ShowAsync();
            }
        }

        private void ContextFlyout_Opened(object sender, object e)
        {
            if (sender is MenuFlyout flyout)
            {
                foreach (var shortcut in flyout.Items.OfType<MenuFlyoutItem>().Where(item => Equals(item.Tag, "DesktopShortcut")))
                    shortcut.DataContext = (flyout.Target as FrameworkElement)?.DataContext;
            }
            if (!AppState.Instance.UseWindows11Skin && sender is MenuFlyout menu &&
                menu.Items.FirstOrDefault() is MenuFlyoutItem item &&
                ((menu.Target as FrameworkElement)?.DataContext ?? item.DataContext) is StartIconModel app)
            {
                item.DataContext = app;
                UpdateTaskbarPinMenuItem(item, app);
            }
            MainPage.SetPopupCursorVisibility(true);
        }
        private void ContextFlyout_Closed(object sender, object e) => MainPage.SetPopupCursorVisibility(false);

    }

    public sealed class Windows11AppGroup : List<StartIconModel>
    {
        public Windows11AppGroup(string key, IEnumerable<StartIconModel> apps) : base(apps)
        {
            Key = key;
        }

        internal const string OtherScripts = "Other scripts";
        public string Key { get; }
        public string Label => Key == OtherScripts ? "\uE774" : Key;
        public FontFamily LabelFontFamily => new FontFamily(Key == OtherScripts ? "Segoe MDL2 Assets" : "Segoe UI");
    }

}
