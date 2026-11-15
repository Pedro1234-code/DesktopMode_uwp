using System;
using System.Threading;
using System.Threading.Tasks;
using Newtonsoft.Json;
using Newtonsoft.Json.Linq;
using Windows.Security.Cryptography.Certificates;
using Windows.Web.Http;
using Windows.Web.Http.Filters;

namespace factoryos_10x_shell.Services.Helpers
{
    internal static class DevicePortalSettingsService
    {
        private static readonly Uri Portal = new Uri("https://localhost:11443/");

        internal enum ReadStatus { Available, Unreachable, AuthenticationRequired, Unavailable }

        internal sealed class Result
        {
            internal ReadStatus Status { get; }
            internal JArray Rows { get; }

            internal Result(ReadStatus status, JArray rows = null)
            {
                Status = status;
                Rows = rows;
            }
        }

        internal static Task<Result> ReadSettingsAsync(CancellationToken token) =>
            ReadAsync("ext/settings", "Settings", token);

        internal static Task<Result> ReadNetworkAsync(CancellationToken token) =>
            ReadAsync("api/networking/ipconfig", "Adapters", token);

        private static async Task<Result> ReadAsync(string endpoint, string arrayName, CancellationToken token)
        {
            using (var timeout = CancellationTokenSource.CreateLinkedTokenSource(token))
            using (var filter = new HttpBaseProtocolFilter())
            {
                timeout.CancelAfter(TimeSpan.FromSeconds(5));
                filter.AllowUI = false;
                filter.AllowAutoRedirect = false;
                filter.CacheControl.ReadBehavior = HttpCacheReadBehavior.NoCache;
                filter.CacheControl.WriteBehavior = HttpCacheWriteBehavior.NoCache;
                // This client only sends requests to the console's loopback address.
                filter.IgnorableServerCertificateErrors.Add(ChainValidationResult.Untrusted);
                filter.IgnorableServerCertificateErrors.Add(ChainValidationResult.InvalidName);
                using (var client = new HttpClient(filter))
                {
                    bool receivedResponse = false;
                    try
                    {
                        using (var response = await client.GetAsync(new Uri(Portal, endpoint)).AsTask(timeout.Token))
                        {
                            receivedResponse = true;
                            if (response.StatusCode == HttpStatusCode.Unauthorized ||
                                response.StatusCode == HttpStatusCode.Forbidden)
                                return new Result(ReadStatus.AuthenticationRequired);
                            if (!response.IsSuccessStatusCode)
                                return new Result(ReadStatus.Unavailable);
                            var json = JObject.Parse(await response.Content.ReadAsStringAsync().AsTask(timeout.Token));
                            return json[arrayName] is JArray rows
                                ? new Result(ReadStatus.Available, rows)
                                : new Result(ReadStatus.Unavailable);
                        }
                    }
                    catch (JsonException)
                    {
                        return new Result(ReadStatus.Unavailable);
                    }
                    catch (Exception) when (!token.IsCancellationRequested)
                    {
                        return new Result(receivedResponse ? ReadStatus.Unavailable : ReadStatus.Unreachable);
                    }
                }
            }
        }
    }
}
