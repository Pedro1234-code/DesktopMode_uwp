using System;
using System.Globalization;
using System.Net.Http;
using System.Net.Http.Headers;
using System.Threading;
using System.Threading.Tasks;
using Windows.Storage;
using Windows.UI.Xaml.Controls;

namespace factoryos_10x_shell.Services.Helpers
{
    public enum UpdateCheckStatus
    {
        UpToDate,
        UpdateAvailable,
        Failed
    }

    public sealed class UpdateCheckResult
    {
        public UpdateCheckStatus Status { get; set; }
        public int CurrentVersion { get; set; }
        public int AvailableVersion { get; set; }
        public string ErrorMessage { get; set; }
    }

    public static class UpdateCheckService
    {
        public const int CurrentVersion = 20279;
        public const string VersionUrl =
            "https://github.com/Pedro1234-code/DesktopMode_uwp/releases/download/updt/version.txt";

        private const string AutomaticCheckSetting = "CoreShell.EnableAutomaticUpdateCheck";
        private static readonly SemaphoreSlim CheckLock = new SemaphoreSlim(1, 1);
        private static readonly HttpClient Client = new HttpClient
        {
            Timeout = TimeSpan.FromSeconds(20)
        };

        public static bool IsAutomaticCheckEnabled
        {
            get
            {
                object value = ApplicationData.Current.LocalSettings.Values[AutomaticCheckSetting];
                return !(value is bool) || (bool)value;
            }
            set => ApplicationData.Current.LocalSettings.Values[AutomaticCheckSetting] = value;
        }

        public static async Task<UpdateCheckResult> CheckAsync(bool showUpdateDialog)
        {
            await CheckLock.WaitAsync();
            try
            {
                int availableVersion;
                using (var request = new HttpRequestMessage(HttpMethod.Get, VersionUrl))
                {
                    request.Headers.CacheControl = new CacheControlHeaderValue
                    {
                        NoCache = true,
                        NoStore = true
                    };

                    using (HttpResponseMessage response = await Client.SendAsync(request))
                    {
                        response.EnsureSuccessStatusCode();
                        string text = (await response.Content.ReadAsStringAsync()).Trim();
                        if (!int.TryParse(text, NumberStyles.None, CultureInfo.InvariantCulture, out availableVersion))
                            throw new FormatException("The update server returned an invalid version number.");
                    }
                }

                var result = new UpdateCheckResult
                {
                    CurrentVersion = CurrentVersion,
                    AvailableVersion = availableVersion,
                    Status = availableVersion > CurrentVersion
                        ? UpdateCheckStatus.UpdateAvailable
                        : UpdateCheckStatus.UpToDate
                };

                if (result.Status == UpdateCheckStatus.UpdateAvailable && showUpdateDialog)
                    await ShowUpdateAvailableDialogAsync();

                return result;
            }
            catch (Exception ex)
            {
                return new UpdateCheckResult
                {
                    Status = UpdateCheckStatus.Failed,
                    CurrentVersion = CurrentVersion,
                    ErrorMessage = ex.Message
                };
            }
            finally
            {
                CheckLock.Release();
            }
        }

        private static async Task ShowUpdateAvailableDialogAsync()
        {
            var dialog = new ContentDialog
            {
                Title = "Update available",
                Content = "Update available. Check Xbox Desktop Mode GitHub to download.",
                CloseButtonText = "OK"
            };

            MainPage.SetPopupCursorVisibility(true);
            try
            {
                await dialog.ShowAsync();
            }
            finally
            {
                MainPage.SetPopupCursorVisibility(false);
            }
        }
    }
}
