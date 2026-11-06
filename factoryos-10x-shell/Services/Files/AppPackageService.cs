using System;
using System.IO;
using System.IO.Compression;
using System.Linq;
using System.Threading;
using System.Threading.Tasks;
using System.Xml;
using System.Xml.Linq;
using Newtonsoft.Json.Linq;
using Windows.Management.Deployment;
using Windows.Security.Cryptography.Certificates;
using Windows.Storage;
using Windows.Web.Http;
using Windows.Web.Http.Filters;
using Windows.Web.Http.Headers;

namespace factoryos_10x_shell.Services.Files
{
    internal sealed class LocalPortalConnectionException : Exception
    {
        internal LocalPortalConnectionException(Exception inner) : base("Could not connect to localhost", inner) { }
    }

    internal sealed class AppPackagePreview
    {
        public string Name, Publisher, Version, Description, Architecture, Capabilities;
        public string IdentityName, ApplicationId;
        public byte[] Logo;
        public bool HasLocalizedResources;
    }

    internal static class AppPackageService
    {
        private static readonly SemaphoreSlim InstallLock = new SemaphoreSlim(1, 1);
        private static readonly Uri Portal = new Uri("https://localhost:11443/");
        internal static bool IsPackage(StorageFile file) => file != null &&
            new[] { ".appx", ".appxbundle", ".msix", ".msixbundle" }.Contains(Path.GetExtension(file.Name).ToLowerInvariant());

        internal static Task<AppPackagePreview> ReadAsync(StorageFile file) => Task.Run(async () =>
        {
            using (var input = await file.OpenStreamForReadAsync())
            using (var archive = new ZipArchive(input, ZipArchiveMode.Read))
            {
                var bundle = Entry(archive, "AppxMetadata/AppxBundleManifest.xml");
                if (bundle == null) return ReadPackage(archive);
                var manifest = ReadXml(bundle);
                string architecture = Windows.ApplicationModel.Package.Current.Id.Architecture.ToString().ToLowerInvariant();
                var package = manifest.Descendants().Where(e => e.Name.LocalName == "Package" &&
                    string.Equals((string)e.Attribute("Type"), "application", StringComparison.OrdinalIgnoreCase))
                    .Where(e => string.Equals((string)e.Attribute("Architecture"), architecture, StringComparison.OrdinalIgnoreCase) ||
                        string.Equals((string)e.Attribute("Architecture"), "neutral", StringComparison.OrdinalIgnoreCase))
                    .OrderByDescending(e => string.Equals((string)e.Attribute("Architecture"), architecture, StringComparison.OrdinalIgnoreCase)).FirstOrDefault();
                if (package == null) throw new InvalidDataException("This bundle has no application package for " + architecture + ".");
                var payload = Entry(archive, (string)package.Attribute("FileName"));
                if (payload == null) throw new InvalidDataException("The bundle references a missing file: " + (string)package.Attribute("FileName"));
                if (payload.CompressedLength == payload.Length &&
                    long.TryParse((string)package.Attribute("Offset"), out long offset) && offset >= 0 &&
                    payload.Length <= input.Length && offset <= input.Length - payload.Length &&
                    MatchesPayload(input, offset, payload))
                {
                    using (var slice = new PackageSlice(input, offset, payload.Length))
                    using (var nested = new ZipArchive(slice, ZipArchiveMode.Read)) return ReadPackage(nested);
                }
                var temporary = await ApplicationData.Current.TemporaryFolder.CreateFileAsync("installer-preview.appx", CreationCollisionOption.GenerateUniqueName);
                try
                {
                    using (var source = payload.Open())
                    using (var destination = await temporary.OpenStreamForWriteAsync())
                        await source.CopyToAsync(destination);
                    using (var stream = await temporary.OpenStreamForReadAsync())
                    using (var nested = new ZipArchive(stream, ZipArchiveMode.Read)) return ReadPackage(nested);
                }
                finally
                {
                    try { await temporary.DeleteAsync(); }
                    catch (FileNotFoundException) { }
                }
            }
        });

        private static ZipArchiveEntry Entry(ZipArchive archive, string path) => archive.Entries.FirstOrDefault(e =>
            string.Equals(NormalizePackagePath(e.FullName), NormalizePackagePath(path), StringComparison.OrdinalIgnoreCase));

        private static string NormalizePackagePath(string path) => path?.Replace('\\', '/').TrimStart('/');

        private static bool MatchesPayload(Stream input, long offset, ZipArchiveEntry payload)
        {
            if (payload.Length < 64) return false;
            var expected = new byte[64];
            using (var source = payload.Open())
            {
                int read = 0;
                while (read < expected.Length)
                {
                    int count = source.Read(expected, read, expected.Length - read);
                    if (count == 0) return false;
                    read += count;
                }
            }
            input.Position = offset;
            for (int i = 0; i < expected.Length; i++) if (input.ReadByte() != expected[i]) return false;
            return true;
        }

        private sealed class PackageSlice : Stream
        {
            private readonly Stream m_input;
            private readonly long m_offset, m_length;
            private long m_position;
            internal PackageSlice(Stream input, long offset, long length) { m_input = input; m_offset = offset; m_length = length; }
            public override bool CanRead => true;
            public override bool CanSeek => true;
            public override bool CanWrite => false;
            public override long Length => m_length;
            public override long Position { get => m_position; set => Seek(value, SeekOrigin.Begin); }
            public override int Read(byte[] buffer, int offset, int count)
            {
                if (buffer == null) throw new ArgumentNullException(nameof(buffer));
                if (offset < 0 || count < 0 || offset > buffer.Length - count) throw new ArgumentOutOfRangeException();
                m_input.Position = m_offset + m_position;
                int read = m_input.Read(buffer, offset, (int)Math.Min(count, m_length - m_position));
                m_position += read;
                return read;
            }
            public override long Seek(long offset, SeekOrigin origin)
            {
                long position = checked((origin == SeekOrigin.Begin ? 0 : origin == SeekOrigin.Current ? m_position :
                    origin == SeekOrigin.End ? m_length : throw new ArgumentOutOfRangeException(nameof(origin))) + offset);
                if (position < 0 || position > m_length) throw new IOException("Seek outside the embedded package.");
                return m_position = position;
            }
            public override void Flush() { }
            public override void SetLength(long value) => throw new NotSupportedException();
            public override void Write(byte[] buffer, int offset, int count) => throw new NotSupportedException();
        }

        private static XDocument ReadXml(ZipArchiveEntry entry)
        {
            if (entry == null || entry.Length > 2 * 1024 * 1024) throw new InvalidDataException("Missing or oversized package manifest.");
            using (var stream = entry.Open())
            using (var reader = XmlReader.Create(stream, new XmlReaderSettings { DtdProcessing = DtdProcessing.Prohibit, XmlResolver = null, MaxCharactersInDocument = 2 * 1024 * 1024 }))
                return XDocument.Load(reader);
        }

        private static AppPackagePreview ReadPackage(ZipArchive archive)
        {
            var manifest = ReadXml(Entry(archive, "AppxManifest.xml"));
            var identity = manifest.Root.Elements().FirstOrDefault(e => e.Name.LocalName == "Identity");
            if (identity == null) throw new InvalidDataException("The package has no identity.");
            var properties = manifest.Root.Elements().FirstOrDefault(e => e.Name.LocalName == "Properties");
            var visual = manifest.Descendants().FirstOrDefault(e => e.Name.LocalName == "VisualElements");
            Func<string, string> property = name => properties?.Elements().FirstOrDefault(e => e.Name.LocalName == name)?.Value;
            var result = new AppPackagePreview();
            result.IdentityName = (string)identity.Attribute("Name");
            result.ApplicationId = (string)manifest.Descendants().FirstOrDefault(e => e.Name.LocalName == "Application")?.Attribute("Id");
            Func<string, string, string> text = (value, fallback) => {
                if (value?.StartsWith("ms-resource:", StringComparison.OrdinalIgnoreCase) == true) { result.HasLocalizedResources = true; return fallback; }
                return string.IsNullOrWhiteSpace(value) ? fallback : value;
            };
            result.Name = text((string)visual?.Attribute("DisplayName") ?? property("DisplayName"), (string)identity.Attribute("Name"));
            result.Publisher = text(property("PublisherDisplayName"), (string)identity.Attribute("Publisher"));
            result.Description = text((string)visual?.Attribute("Description") ?? property("Description"), string.Empty);
            result.Version = (string)identity.Attribute("Version");
            result.Architecture = (string)identity.Attribute("ProcessorArchitecture") ?? "neutral";
            result.Capabilities = string.Join("\n", manifest.Descendants().Where(e => e.Parent?.Name.LocalName == "Capabilities")
                .Select(e => (string)e.Attribute("Name")).Where(n => !string.IsNullOrWhiteSpace(n)).Distinct());
            string logoPath = (string)visual?.Attribute("Square150x150Logo") ?? property("Logo");
            if (!string.IsNullOrWhiteSpace(logoPath) && !logoPath.StartsWith("ms-resource:", StringComparison.OrdinalIgnoreCase))
            {
                logoPath = logoPath.Replace('\\', '/');
                string stem = logoPath.Substring(0, logoPath.Length - Path.GetExtension(logoPath).Length);
                var logo = Entry(archive, logoPath) ?? archive.Entries.Where(e =>
                    e.FullName.Replace('\\', '/').StartsWith(stem + ".scale-", StringComparison.OrdinalIgnoreCase) &&
                    e.FullName.EndsWith(Path.GetExtension(logoPath), StringComparison.OrdinalIgnoreCase))
                    .OrderBy(e => Math.Abs(Scale(e.FullName) - 200)).FirstOrDefault();
                if (logo != null && logo.Length <= 8 * 1024 * 1024)
                    using (var source = logo.Open())
                    using (var memory = new MemoryStream()) { source.CopyTo(memory); result.Logo = memory.ToArray(); }
            }
            return result;
        }

        private static int Scale(string name)
        {
            var match = System.Text.RegularExpressions.Regex.Match(name, @"\.scale-(\d+)");
            return int.TryParse(match.Groups[1].Value, out int scale) ? scale : 100;
        }

        internal static async Task<string> InstallAsync(StorageFile package, System.Collections.Generic.IReadOnlyList<StorageFile> dependencies,
            string username, string password, bool trustLocalCertificate, IProgress<string> progress,
            bool launchWhenReady = false, string identityName = null, string applicationId = null)
        {
            if (!await InstallLock.WaitAsync(0)) throw new InvalidOperationException("Another installation is already in progress.");
            try
            {
                using (var timeout = new CancellationTokenSource(TimeSpan.FromMinutes(20)))
                using (var filter = new HttpBaseProtocolFilter())
                {
                    filter.AllowAutoRedirect = false;
                    filter.AllowUI = false;
                    filter.CacheControl.ReadBehavior = HttpCacheReadBehavior.NoCache;
                    filter.CacheControl.WriteBehavior = HttpCacheWriteBehavior.NoCache;
                    if (trustLocalCertificate)
                    {
                        filter.IgnorableServerCertificateErrors.Add(ChainValidationResult.Untrusted);
                        filter.IgnorableServerCertificateErrors.Add(ChainValidationResult.InvalidName);
                    }
                    using (var client = new HttpClient(filter))
                    {
                        if (!string.IsNullOrWhiteSpace(username)) client.DefaultRequestHeaders.Authorization =
                            new HttpCredentialsHeaderValue("Basic", Convert.ToBase64String(System.Text.Encoding.UTF8.GetBytes(username + ":" + password)));
                        progress.Report("Connecting to Device Portal…");
                        HttpResponseMessage connection;
                        using (var connectTimeout = CancellationTokenSource.CreateLinkedTokenSource(timeout.Token))
                        {
                            connectTimeout.CancelAfter(TimeSpan.FromSeconds(15));
                            try { connection = await client.GetAsync(Portal).AsTask(connectTimeout.Token); }
                            catch (Exception error) { throw new LocalPortalConnectionException(error); }
                        }
                        using (connection) CheckResponse(connection);
                        var csrf = filter.CookieManager.GetCookies(Portal).FirstOrDefault(cookie => cookie.Name == "CSRF-Token");
                        if (csrf != null) client.DefaultRequestHeaders.TryAppendWithoutValidation("X-CSRF-Token", csrf.Value);
                        using (var state = await client.GetAsync(new Uri(Portal, "api/app/packagemanager/state")).AsTask(timeout.Token))
                            if ((int)state.StatusCode == 204) throw new InvalidOperationException("Device Portal is already installing another package. Wait for it to finish.");
                        using (var body = new HttpMultipartFormDataContent())
                        {
                            if (dependencies.Select(file => file.Name).Concat(new[] { package.Name }).Distinct(StringComparer.OrdinalIgnoreCase).Count() != dependencies.Count + 1)
                                throw new InvalidOperationException("The selected packages have duplicate file names. Select each dependency only once.");
                            await AddFileAsync(body, package);
                            foreach (var dependency in dependencies) await AddFileAsync(body, dependency);
                            progress.Report("Sending package to Device Portal…");
                            var uploadProgress = new Progress<HttpProgress>(value => {
                                ulong total = value.TotalBytesToSend.GetValueOrDefault();
                                if (total > 0)
                                    progress.Report("Sending package to Device Portal… " + Math.Min(100.0, value.BytesSent * 100.0 / total).ToString("F0") + "%");
                            });
                            using (var response = await client.PostAsync(new Uri(Portal, "api/app/packagemanager/package?package=" + Uri.EscapeDataString(package.Name)), body).AsTask(timeout.Token, uploadProgress))
                            {
                                CheckResponse(response);
                                var message = await response.Content.ReadAsStringAsync();
                                if (!string.IsNullOrWhiteSpace(message) && message.TrimStart().StartsWith("{")) CheckResult(JObject.Parse(message));
                            }
                        }
                        progress.Report("Installing…");
                        for (;;)
                        {
                            await Task.Delay(1000, timeout.Token);
                            using (var response = await client.GetAsync(new Uri(Portal, "api/app/packagemanager/state")).AsTask(timeout.Token))
                            {
                                if ((int)response.StatusCode == 204) continue;
                                CheckResponse(response);
                                CheckResult(JObject.Parse(await response.Content.ReadAsStringAsync()), true);
                                string registrationNotice = string.Empty;
                                string fullName = null;
                                string appId = applicationId;
                                try
                                {
                                    // Portal deployment runs in a service context. Success there
                                    // does not guarantee registration for this Shell's user.
                                    using (var packages = await client.GetAsync(new Uri(Portal, "api/app/packagemanager/packages")).AsTask(timeout.Token))
                                    {
                                        CheckResponse(packages);
                                        var installed = JObject.Parse(await packages.Content.ReadAsStringAsync())["InstalledPackages"]?.FirstOrDefault(
                                            item => !string.IsNullOrEmpty(identityName) &&
                                                ((string)item["PackageFullName"])?.StartsWith(identityName + "_", StringComparison.OrdinalIgnoreCase) == true);
                                        fullName = (string)installed?["PackageFullName"];
                                        if (string.IsNullOrEmpty(fullName)) throw new InvalidOperationException("The installed package could not be found for registration verification.");
                                        string relativeId = (string)installed["PackageRelativeId"];
                                        if (!string.IsNullOrEmpty(relativeId)) appId = relativeId;
                                    }
                                    progress.Report("Checking app registration…");
                                    await EnsureCurrentUserRegistrationAsync(fullName, progress, timeout.Token);
                                }
                                catch (Exception error)
                                {
                                    await LogRegistrationAsync("Registration verification failed; HRESULT=0x" + error.HResult.ToString("X8") + ".");
                                    registrationNotice = "The package was installed, but registration for the current Shell user could not be confirmed: " + error.Message;
                                }
                                if (launchWhenReady && !string.IsNullOrEmpty(appId) && string.IsNullOrEmpty(registrationNotice))
                                {
                                    try
                                    {
                                        progress.Report("Launching app…");
                                        Func<string, string> encode = value => Uri.EscapeDataString(Convert.ToBase64String(System.Text.Encoding.UTF8.GetBytes(value)));
                                        using (var launched = await client.PostAsync(new Uri(Portal, "api/taskmanager/app?appid=" + encode(appId) + "&package=" + encode(fullName)), new HttpStringContent(string.Empty)).AsTask(timeout.Token))
                                            CheckResponse(launched);
                                    }
                                    catch (Exception error) { return "The app was installed, but could not be launched: " + error.Message; }
                                }
                                return registrationNotice;
                            }
                        }
                    }
                }
            }
            catch (OperationCanceledException) { throw new TimeoutException("Device Portal did not finish in time. Installation may still be running; check the portal before trying again."); }
            finally { InstallLock.Release(); }
        }

        private static Task EnsureCurrentUserRegistrationAsync(string fullName, IProgress<string> progress, CancellationToken cancellation)
        {
            return Task.Run(async () =>
            {
                var manager = new PackageManager();
                bool registered = manager.FindPackageForUser(string.Empty, fullName) != null;
                await LogRegistrationAsync("Post-install package=" + fullName + "; currentUserRegistered=" + registered + ".");
                if (registered) return;

                progress.Report("Registering app for the current user…");
                DeploymentResult result = await manager.RegisterPackageByFullNameAsync(
                    fullName, new string[0], DeploymentOptions.None).AsTask(cancellation);
                if (result.ExtendedErrorCode != null && result.ExtendedErrorCode.HResult < 0)
                {
                    await LogRegistrationAsync("Current-user registration rejected; HRESULT=0x" + result.ExtendedErrorCode.HResult.ToString("X8") + ".");
                    throw new InvalidOperationException("Current-user registration failed (0x" + result.ExtendedErrorCode.HResult.ToString("X8") + "): " + result.ErrorText);
                }
                if (manager.FindPackageForUser(string.Empty, fullName) == null)
                    throw new InvalidOperationException("The package is still absent from the current user's package registry after registration.");
                await LogRegistrationAsync("Current-user registration verified: " + fullName + ".");
            }, cancellation);
        }

        private static async Task LogRegistrationAsync(string message)
        {
            System.Diagnostics.Debug.WriteLine("App Installer: " + message);
            try
            {
                var file = await ApplicationData.Current.LocalFolder.CreateFileAsync("AppInstaller-runtime.log", CreationCollisionOption.OpenIfExists);
                if ((await file.GetBasicPropertiesAsync()).Size > 1024 * 1024)
                    await FileIO.WriteTextAsync(file, string.Empty);
                await FileIO.AppendTextAsync(file, DateTimeOffset.Now.ToString("O") + " " + message + System.Environment.NewLine);
            }
            catch { /* Diagnostics must not change a deployment result. */ }
        }

        private static async Task AddFileAsync(HttpMultipartFormDataContent body, StorageFile file)
        {
            var content = new HttpStreamContent(await file.OpenReadAsync());
            content.Headers.ContentType = new HttpMediaTypeHeaderValue("application/octet-stream");
            body.Add(content, file.Name, file.Name);
        }

        private static void CheckResponse(HttpResponseMessage response)
        {
            int status = (int)response.StatusCode;
            if (status == 401 || status == 403) throw new UnauthorizedAccessException("Device Portal rejected the request. Check its username and password and whether access is enabled.");
            if (!response.IsSuccessStatusCode) throw new InvalidOperationException("Device Portal returned HTTP " + status + ".");
        }

        private static void CheckResult(JObject result, bool requireSuccess = false)
        {
            if ((bool?)result["Success"] == false || ((long?)result["Code"] ?? 0) != 0)
                throw new InvalidOperationException("Installation failed: " + (string)(result["Reason"] ?? result["CodeText"]) + " (" + result["Code"] + ")");
            if (requireSuccess && (bool?)result["Success"] != true) throw new InvalidOperationException("Device Portal returned an unrecognized installation result.");
        }
    }
}
