using System;
using System.Threading.Tasks;
using Windows.Foundation;
using Windows.Security.ExchangeActiveSyncProvisioning;
using Windows.Storage;
using Windows.Storage.Pickers;
using Windows.System.Profile;
using Windows.UI;
using Windows.UI.Xaml;
using Windows.UI.Xaml.Controls;
using Windows.UI.Xaml.Input;
using Windows.UI.Xaml.Media;
using Windows.UI.Xaml.Media.Imaging;
using Windows.UI.ViewManagement;
using factoryos_10x_shell.Services.Helpers;

namespace factoryos_10x_shell.Controls
{
    public sealed partial class SettingsWindow : UserControl
    {
        private bool m_dragging, m_resizing, m_maximized, m_initializing;
        private Point m_startPoint;
        private double m_startLeft, m_startTop, m_startWidth, m_startHeight;
        private double m_restoreLeft, m_restoreTop, m_restoreWidth, m_restoreHeight;

        public SettingsWindow()
        {
            InitializeComponent();
            m_initializing = true;
            SearchToggle.IsOn = AppState.Instance.IsSearchButtonVisible;
            CopilotToggle.IsOn = AppState.Instance.IsCopilotButtonVisible;
            BackgroundToggle.IsOn = AppState.Instance.IsBgChangeButtonVisible;
            m_initializing = false;
            PopulateDeviceInformation();
        }

        public void Open()
        {
            Visibility = Visibility.Visible;
            AppState.Instance.SetSettingsWindowState(true, false);
            AppState.Instance.ActivateSettings();
        }
        public void CloseFromTaskView() => Close();
        private void Close() { Visibility = Visibility.Collapsed; AppState.Instance.SetSettingsWindowState(false, false); ShowHome(); }
        private void Close_Click(object sender, RoutedEventArgs e) => Close();
        private void Minimize_Click(object sender, RoutedEventArgs e) { Visibility = Visibility.Collapsed; AppState.Instance.SetSettingsWindowState(true, true); }
        private void Root_PointerPressed(object sender, PointerRoutedEventArgs e) => AppState.Instance.ActivateSettings();
        private void Maximize_Click(object sender, RoutedEventArgs e)
        {
            Canvas host = VisualTreeHelper.GetParent(this) as Canvas; if (host == null) return;
            if (!m_maximized) { m_restoreLeft = Canvas.GetLeft(this); m_restoreTop = Canvas.GetTop(this); m_restoreWidth = Width; m_restoreHeight = Height; Canvas.SetLeft(this, 0); Canvas.SetTop(this, 0); Width = host.ActualWidth; Height = host.ActualHeight; m_maximized = true; }
            else { Canvas.SetLeft(this, m_restoreLeft); Canvas.SetTop(this, m_restoreTop); Width = m_restoreWidth; Height = m_restoreHeight; m_maximized = false; }
        }
        private void TitleBar_PointerPressed(object sender, PointerRoutedEventArgs e) { if (m_maximized) return; m_dragging = true; m_startPoint = e.GetCurrentPoint(VisualTreeHelper.GetParent(this) as UIElement).Position; m_startLeft = Canvas.GetLeft(this); m_startTop = Canvas.GetTop(this); TitleBar.CapturePointer(e.Pointer); }
        private void TitleBar_PointerMoved(object sender, PointerRoutedEventArgs e) { if (!m_dragging) return; Point p = e.GetCurrentPoint(VisualTreeHelper.GetParent(this) as UIElement).Position; Canvas.SetLeft(this, Math.Max(0, m_startLeft + p.X - m_startPoint.X)); Canvas.SetTop(this, Math.Max(0, m_startTop + p.Y - m_startPoint.Y)); }
        private void TitleBar_PointerReleased(object sender, PointerRoutedEventArgs e) { m_dragging = false; TitleBar.ReleasePointerCaptures(); }
        private void ResizeGrip_PointerPressed(object sender, PointerRoutedEventArgs e) { if (!m_maximized) { m_resizing = true; m_startPoint = e.GetCurrentPoint(this).Position; m_startWidth = Width; m_startHeight = Height; ResizeGrip.CapturePointer(e.Pointer); } }
        private void ResizeGrip_PointerMoved(object sender, PointerRoutedEventArgs e) { if (m_resizing) { Point p = e.GetCurrentPoint(this).Position; Width = Math.Max(720, m_startWidth + p.X - m_startPoint.X); Height = Math.Max(480, m_startHeight + p.Y - m_startPoint.Y); } }
        private void ResizeGrip_PointerReleased(object sender, PointerRoutedEventArgs e) { m_resizing = false; ResizeGrip.ReleasePointerCaptures(); }

        private void System_Click(object sender, RoutedEventArgs e) { OpenDetail("System", "About"); }
        private void Personalization_Click(object sender, RoutedEventArgs e) { OpenDetail("Personalization", "Background"); }
        private void UpdateSecurity_Click(object sender, RoutedEventArgs e) { OpenDetail("Update & Security", "Windows Update"); }
        private void ShowHome()
        {
            HomePage.Visibility = Visibility.Visible;
            DetailPage.Visibility = Visibility.Collapsed;
            TitleText.Text = "Settings";
            HomeNavigation.SelectedIndex = 0;
            PersonalizationNavigation.SelectedItem = null;
            SystemNavigation.SelectedItem = null;
            UpdateSecurityNavigation.SelectedItem = null;
        }
        private void OpenDetail(string section, string page)
        {
            HomePage.Visibility = Visibility.Collapsed;
            DetailPage.Visibility = Visibility.Visible;
            SectionTitle.Text = section;
            TitleText.Text = "Settings";
            bool system = section == "System";
            bool updateSecurity = section == "Update & Security";
            SystemNavigation.Visibility = system ? Visibility.Visible : Visibility.Collapsed;
            UpdateSecurityNavigation.Visibility = updateSecurity ? Visibility.Visible : Visibility.Collapsed;
            PersonalizationNavigation.Visibility = !system && !updateSecurity ? Visibility.Visible : Visibility.Collapsed;
            HomeNavigation.SelectedItem = null;
            SelectDetail(page);
            SelectNavigationItem(page);
            DismissOnScreenKeyboard();
        }
        private void SettingsNavigation_SelectionChanged(object sender, SelectionChangedEventArgs e)
        {
            var item = (sender as ListView)?.SelectedItem as ListViewItem;
            string page = item?.Tag as string;
            if (string.IsNullOrEmpty(page)) return;

            if (page == "Home")
            {
                ShowHome();
                DismissOnScreenKeyboard();
                return;
            }

            HomeNavigation.SelectedItem = null;
            SelectDetail(page);
            DismissOnScreenKeyboard();
        }
        private void SelectNavigationItem(string page)
        {
            ListView navigation = page == "About"
                ? SystemNavigation
                : page == "Windows Update" ? UpdateSecurityNavigation : PersonalizationNavigation;
            foreach (object entry in navigation.Items)
            {
                var item = entry as ListViewItem;
                if (item?.Tag as string == page)
                {
                    navigation.SelectedItem = item;
                    break;
                }
            }
        }
        private void SelectDetail(string page)
        {
            BackgroundPanel.Visibility = page == "Background" ? Visibility.Visible : Visibility.Collapsed;
            ColorsPanel.Visibility = page == "Colors" ? Visibility.Visible : Visibility.Collapsed;
            LauncherPanel.Visibility = page == "Launcher" ? Visibility.Visible : Visibility.Collapsed;
            TaskbarPanel.Visibility = page == "Taskbar" ? Visibility.Visible : Visibility.Collapsed;
            AboutPanel.Visibility = page == "About" ? Visibility.Visible : Visibility.Collapsed;
            WindowsUpdatePanel.Visibility = page == "Windows Update" ? Visibility.Visible : Visibility.Collapsed;
            DetailTitle.Text = page;
        }
        private void CheckForUpdates_Click(object sender, RoutedEventArgs e) => UpdateStatusText.Text = "Update checks are not available yet in CoreShell.";

        private async void DismissOnScreenKeyboard()
        {
            try
            {
                InputPane.GetForCurrentView().TryHide();
                await Task.Delay(75);
                InputPane.GetForCurrentView().TryHide();
            }
            catch { }
        }

        private void PopulateDeviceInformation()
        {
            var version = AnalyticsInfo.VersionInfo;
            bool isXbox = string.Equals(version.DeviceFamily, "Windows.Xbox", StringComparison.OrdinalIgnoreCase);
            string productName = null;
            string sku = null;
            string manufacturer = null;
            string friendlyName = null;
            string operatingSystem = null;

            try
            {
                var device = new EasClientDeviceInformation();
                productName = device.SystemProductName;
                sku = device.SystemSku;
                manufacturer = device.SystemManufacturer;
                friendlyName = device.FriendlyName;
                operatingSystem = device.OperatingSystem;
            }
            catch { }

            string model = FirstAvailable(sku, productName);
            string deviceName = isXbox ? DescribeXbox(model, productName, sku) : FirstAvailable(friendlyName, model, "Windows device");
            DeviceNameValue.Text = deviceName;
            ManufacturerValue.Text = FirstAvailable(manufacturer, isXbox ? "Microsoft Corporation" : null, "Unknown");
            ModelValue.Text = FirstAvailable(model, isXbox ? deviceName : null, "Unknown");
            SystemTypeValue.Text = isXbox ? "Xbox / Windows UWP" : string.Format("{0} / Windows UWP", version.DeviceFamily);

            ulong familyVersion;
            ulong.TryParse(version.DeviceFamilyVersion, out familyVersion);
            ushort major = (ushort)(familyVersion >> 48);
            ushort minor = (ushort)(familyVersion >> 32);
            ushort build = (ushort)(familyVersion >> 16);
            ushort revision = (ushort)familyVersion;
            string windowsName = major == 10 ? "Windows 10" : string.Format("Windows {0}.{1}", major, minor);
            EditionValue.Text = isXbox ? windowsName + " / XboxOS" : FirstAvailable(operatingSystem, windowsName);
            BuildValue.Text = string.Format("{0}.{1}.{2}.{3}", major, minor, build, revision);
        }

        private static string DescribeXbox(string model, string productName, string sku)
        {
            string identifier = string.Format("{0} {1} {2}", model, productName, sku).ToLowerInvariant();
            if (identifier.Contains("series x") || identifier.Contains("anaconda")) return "Xbox Series X";
            if (identifier.Contains("series s") || identifier.Contains("lockhart")) return "Xbox Series S";
            if (identifier.Contains("one x")) return "Xbox One X";
            if (identifier.Contains("one s")) return "Xbox One S";
            if (identifier.Contains("xbox one")) return "Xbox One";
            return FirstAvailable(productName, sku, model, "Xbox");
        }

        private static string FirstAvailable(params string[] values)
        {
            foreach (string value in values)
                if (!string.IsNullOrWhiteSpace(value)) return value;
            return string.Empty;
        }
        private void Wallpaper_Click(object sender, RoutedEventArgs e) { string uri = (sender as FrameworkElement)?.Tag as string; if (!string.IsNullOrWhiteSpace(uri)) SetWallpaper(uri); }
        private async void BrowseWallpaper_Click(object sender, RoutedEventArgs e)
        {
            var picker = new FileOpenPicker { SuggestedStartLocation = PickerLocationId.PicturesLibrary }; picker.FileTypeFilter.Add(".jpg"); picker.FileTypeFilter.Add(".jpeg"); picker.FileTypeFilter.Add(".png");
            StorageFile file = await picker.PickSingleFileAsync(); if (file != null) SetWallpaper(file.Path);
        }
        private void SetWallpaper(string uri)
        {
            ApplicationData.Current.LocalSettings.Values["backgroundImagePath"] = uri;
            WallpaperPreview.Source = new BitmapImage(new Uri(uri));
            AppState.Instance.RequestWallpaper(uri);
        }
        private void Accent_Click(object sender, RoutedEventArgs e)
        {
            string color = (sender as FrameworkElement)?.Tag as string; if (string.IsNullOrEmpty(color)) return;
            ApplicationData.Current.LocalSettings.Values["CoreShell.AccentColor"] = color;
        }
        private void LauncherSize_Click(object sender, RoutedEventArgs e) => ApplicationData.Current.LocalSettings.Values["CoreShell.LauncherSize"] = (sender as FrameworkElement)?.Tag as string;
        private void SearchToggle_Toggled(object sender, RoutedEventArgs e) { if (!m_initializing) { AppState.Instance.IsSearchButtonVisible = SearchToggle.IsOn; ApplicationData.Current.LocalSettings.Values["IsSearchButtonVisible"] = SearchToggle.IsOn; } }
        private void CopilotToggle_Toggled(object sender, RoutedEventArgs e) { if (!m_initializing) { AppState.Instance.IsCopilotButtonVisible = CopilotToggle.IsOn; ApplicationData.Current.LocalSettings.Values["IsCopilotButtonVisible"] = CopilotToggle.IsOn; } }
        private void BackgroundToggle_Toggled(object sender, RoutedEventArgs e) { if (!m_initializing) { AppState.Instance.IsBgChangeButtonVisible = BackgroundToggle.IsOn; ApplicationData.Current.LocalSettings.Values["IsBgChangeButtonVisible"] = BackgroundToggle.IsOn; } }
    }
}
