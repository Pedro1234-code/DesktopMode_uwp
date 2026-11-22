using System;
using System.Linq;
using System.Threading;
using System.Threading.Tasks;
using Newtonsoft.Json.Linq;
using Windows.System.Profile;
using Windows.UI.Xaml;
using Windows.UI.Xaml.Controls;
using Windows.UI.Xaml.Media.Imaging;
using factoryos_10x_shell.Services.Helpers;

namespace factoryos_10x_shell.Controls
{
    public sealed partial class SettingsWindow
    {
        private CancellationTokenSource m_portalLoad;
        private JArray m_consoleSettings;
        private JArray m_networkAdapters;
        private string m_portalStatus = "Xbox Device Portal is unavailable.";

        private static bool IsPortalSystemPage(string page) =>
            page == "Display" || page == "Sound" || page == "Power & sleep";

        private void CancelPortalLoad() => m_portalLoad?.Cancel();

        private async void BeginPortalLoad()
        {
            CancelPortalLoad();
            if (AnalyticsInfo.VersionInfo.DeviceFamily != "Windows.Xbox") return;

            using (var load = new CancellationTokenSource())
            {
                m_portalLoad = load;
                m_consoleSettings = null;
                m_networkAdapters = null;
                m_portalStatus = "Connecting to Xbox Device Portal…";
                RefreshPortalControls();

                try
                {
                    var results = await Task.WhenAll(
                        DevicePortalSettingsService.ReadSettingsAsync(load.Token),
                        DevicePortalSettingsService.ReadNetworkAsync(load.Token));
                    if (load.IsCancellationRequested || Visibility != Visibility.Visible) return;

                    m_consoleSettings = results[0].Rows;
                    m_networkAdapters = results[1].Rows;
                    bool unreachable = results.All(result =>
                        result.Status == DevicePortalSettingsService.ReadStatus.Unreachable);
                    bool authRequired = results.Any(result =>
                        result.Status == DevicePortalSettingsService.ReadStatus.AuthenticationRequired);
                    m_portalStatus = unreachable ? "Could not connect to localhost."
                        : authRequired ? "Xbox Device Portal requires authentication."
                        : "This Xbox Device Portal does not provide data for this page.";
                    RefreshPortalControls();
                    if (unreachable || (authRequired && m_consoleSettings == null && m_networkAdapters == null))
                        await ShowPortalErrorAsync(unreachable, load.Token);
                }
                catch (OperationCanceledException) when (load.IsCancellationRequested) { }
                catch (Exception error)
                {
                    factoryos_10x_shell.Services.Helpers.ShellDiagnostics.WriteDebug("Settings Device Portal HRESULT: " + error.HResult);
                    if (!load.IsCancellationRequested)
                    {
                        m_portalStatus = "Xbox Device Portal is unavailable.";
                        RefreshPortalControls();
                    }
                }
                finally
                {
                    if (m_portalLoad == load) m_portalLoad = null;
                }
            }
        }

        private void RefreshPortalControls()
        {
            SetPortalItemEnabled(DisplayNav, SettingsFor("Display").Any());
            SetPortalItemEnabled(SoundNav, SettingsFor("Sound").Any());
            SetPortalItemEnabled(PowerNav, SettingsFor("Power & sleep").Any());
            SetPortalItemEnabled(NetworkStatusNav, m_networkAdapters != null);
            NetworkTile.IsEnabled = m_networkAdapters != null;
            NetworkTile.Opacity = NetworkTile.IsEnabled ? 1 : .45;
            UpdatePortalPage(DetailTitle.Text);
        }

        private static void SetPortalItemEnabled(ListViewItem item, bool enabled)
        {
            item.IsEnabled = enabled;
            item.Opacity = enabled ? 1 : .45;
        }

        private System.Collections.Generic.IEnumerable<JObject> SettingsFor(string page)
        {
            if (m_consoleSettings == null) return Enumerable.Empty<JObject>();
            return m_consoleSettings.OfType<JObject>().Where(setting =>
            {
                string category = TextValue(setting["Category"]).Trim();
                if (category.EndsWith("Settings", StringComparison.OrdinalIgnoreCase))
                    category = category.Substring(0, category.Length - "Settings".Length).TrimEnd();
                if (page == "Sound")
                    return string.Equals(category, "Sound", StringComparison.OrdinalIgnoreCase) ||
                           string.Equals(category, "Audio", StringComparison.OrdinalIgnoreCase);
                return string.Equals(category, page == "Power & sleep" ? "Power" : page,
                    StringComparison.OrdinalIgnoreCase);
            });
        }

        private void UpdatePortalPage(string page)
        {
            bool isPortalPage = page == "Status" || IsPortalSystemPage(page);
            PortalPanel.Visibility = isPortalPage ? Visibility.Visible : Visibility.Collapsed;
            PortalRows.Children.Clear();
            if (!isPortalPage) return;

            if (page == "Status" && m_networkAdapters != null)
            {
                foreach (var adapter in m_networkAdapters.OfType<JObject>())
                {
                    var addresses = (adapter["IpAddresses"] as JArray)?.OfType<JObject>()
                        .Select(address => TextValue(address["IpAddress"]))
                        .Where(address => address.Length != 0);
                    var ip = addresses == null ? string.Empty : string.Join(", ", addresses);
                    string name = TextValue(adapter["Description"]);
                    if (name.Length == 0) name = TextValue(adapter["Name"]);
                    AddPortalRow(name.Length == 0 ? "Network adapter" : name,
                        ip.Length == 0 ? "No IP address reported." : ip);
                }
                PortalPageStatus.Text = PortalRows.Children.Count == 0
                    ? "No network adapters reported." : "Network properties";
                return;
            }

            foreach (var setting in SettingsFor(page))
            {
                string name = TextValue(setting["Name"]);
                if (name.Length != 0) AddPortalRow(name, TextValue(setting["Value"]));
            }
            PortalPageStatus.Text = PortalRows.Children.Count == 0
                ? m_portalStatus : "Current console settings (read-only)";
        }

        private static string TextValue(JToken token) =>
            token is JValue value ? Convert.ToString(value.Value) ?? string.Empty : string.Empty;

        private void AddPortalRow(string label, string value)
        {
            PortalRows.Children.Add(new StackPanel
            {
                Spacing = 4,
                Children =
                {
                    new TextBlock { Text = label, TextWrapping = TextWrapping.Wrap },
                    new TextBlock { Text = value, Opacity = .7, TextWrapping = TextWrapping.Wrap }
                }
            });
        }

        private async Task ShowPortalErrorAsync(bool unreachable, CancellationToken token)
        {
            var content = new StackPanel { Spacing = 14, MaxWidth = 360 };
            content.Children.Add(new TextBlock
            {
                Text = unreachable
                    ? "Settings could not reach Xbox Device Portal on localhost. Make sure Device Portal is enabled in Dev Home and that Desktop Mode has loopback access."
                    : "Xbox Device Portal denied access. Console settings are unavailable while the portal requires authentication.",
                TextWrapping = TextWrapping.Wrap
            });
            if (unreachable)
            {
                var qr = new Image
                {
                    Source = new BitmapImage(new Uri("ms-appx:///Assets/LOOPBACK_inst-1024.jpeg")),
                    Width = 160,
                    Height = 160,
                    HorizontalAlignment = HorizontalAlignment.Center
                };
                Windows.UI.Xaml.Automation.AutomationProperties.SetName(qr, "Instructions to fix the localhost connection");
                content.Children.Add(qr);
                content.Children.Add(new TextBlock
                {
                    Text = "Open this on your phone or PC and follow the instructions to fix.",
                    TextWrapping = TextWrapping.Wrap
                });
            }

            while (Windows.UI.Xaml.Media.VisualTreeHelper.GetOpenPopups(Window.Current)
                .Any(popup => popup.Child is ContentDialog))
                await Task.Delay(200, token);
            if (token.IsCancellationRequested || Visibility != Visibility.Visible) return;

            var dialog = new ContentDialog
            {
                Title = unreachable ? "Could not connect to localhost" : "Device Portal access denied",
                Content = content,
                CloseButtonText = "OK"
            };
            MainPage.SetPopupCursorVisibility(true);
            try { await dialog.ShowAsync(); }
            finally { MainPage.SetPopupCursorVisibility(false); }
        }
    }
}
