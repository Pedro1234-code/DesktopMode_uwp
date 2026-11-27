using System;
using System.Collections.Generic;
using System.IO;
using System.Linq;
using System.Net.Http;
using System.Security.Cryptography;
using System.Text;
using System.Threading;
using System.Threading.Tasks;
using System.Xml.Linq;
using Newtonsoft.Json.Linq;
using Windows.Storage;
using factoryos_10x_shell.Services.Files;

namespace factoryos_10x_shell.Services.Store
{
    internal static class StoreDelivery
    {
        private const string Endpoint = "https://fe3.delivery.mp.microsoft.com/ClientWebService/client.asmx";
        private sealed class DeliveryFile
        {
            internal XElement Identity, File;
            internal string Moniker => (string)File.Attribute("InstallerSpecificIdentifier");
        }

        private static IEnumerable<XElement> Elements(XContainer xml, string name) =>
            xml.Descendants().Where(e => e.Name.LocalName == name);

        private static async Task<XDocument> RequestAsync(string template, string endpoint, CancellationToken token, params string[] values)
        {
            var file = await StorageFile.GetFileFromApplicationUriAsync(new Uri("ms-appx:///Services/Store/" + template + ".xml"));
            var message = XDocument.Parse(string.Format(await FileIO.ReadTextAsync(file), values.Cast<object>().ToArray()));
            var now = DateTime.UtcNow;
            foreach (var e in Elements(message, "Created").Concat(Elements(message, "currentTime"))) e.Value = now.ToString("o");
            foreach (var e in Elements(message, "Expires")) e.Value = now.AddMinutes(5).ToString("o");
            foreach (var e in Elements(message, "MessageID")) e.Value = "urn:uuid:" + Guid.NewGuid();
            using (var body = new StringContent(message.ToString(), Encoding.UTF8, "application/soap+xml"))
            using (var response = await StoreCatalog.Client.PostAsync(endpoint, body, token).ConfigureAwait(false))
            {
                response.EnsureSuccessStatusCode();
                return XDocument.Parse(await response.Content.ReadAsStringAsync().ConfigureAwait(false));
            }
        }

        private static async Task<List<DeliveryFile>> ResolveAsync(JObject package, CancellationToken token)
        {
            var cookies = await RequestAsync("GetCookie", Endpoint, token);
            string cookie = Elements(cookies, "EncryptedData").First().Value;
            var sync = await RequestAsync("WUIDRequest", Endpoint, token, cookie, (string)package.SelectToken("FulfillmentData.WuCategoryId"), "");
            var files = new List<DeliveryFile>();
            // FE3 sends identity and file metadata in separate fragments with the same local update ID.
            foreach (var group in Elements(sync, "Xml").GroupBy(e => e.Parent.Elements().FirstOrDefault(n => n.Name.LocalName == "ID")?.Value))
            {
                token.ThrowIfCancellationRequested();
                if (group.Key == null) continue;
                var fragments = group.Select(e => XElement.Parse("<root>" + e.Value + "</root>")).ToArray();
                var identity = fragments.SelectMany(f => f.Elements("UpdateIdentity")).FirstOrDefault();
                if (identity == null) continue;
                foreach (var entry in fragments.SelectMany(f => f.Descendants("File")))
                {
                    var ext = Path.GetExtension((string)entry.Attribute("FileName") ?? "");
                    if (new[] { ".appx", ".appxbundle", ".msix", ".msixbundle" }.Contains(ext, StringComparer.OrdinalIgnoreCase))
                        files.Add(new DeliveryFile { Identity = identity, File = entry });
                }
            }
            var main = files.FirstOrDefault(f => f.Moniker == (string)package["PackageFullName"]);
            if (main == null) throw new InvalidDataException("The Store has no downloadable package for this version.");
            var selected = new List<DeliveryFile> { main };
            string arch = Windows.ApplicationModel.Package.Current.Id.Architecture.ToString();
            foreach (var dep in package["FrameworkDependencies"]?.Children() ?? Enumerable.Empty<JToken>())
            {
                string name = (string)dep["PackageIdentity"];
                ulong min = (ulong?)dep["MinVersion"] ?? 0;
                var match = files.Select(f => new { File = f, Parts = f.Moniker?.Split('_') })
                    .Where(f => f.Parts?.Length == 5 && f.Parts[0] == name && f.Parts[3].Length == 0 &&
                        (string.Equals(arch, f.Parts[2], StringComparison.OrdinalIgnoreCase) || f.Parts[2] == "neutral"))
                    .Select(f => new { f.File, Version = Version.TryParse(f.Parts[1], out var version) ? version : null })
                    .Where(f => f.Version != null && PackVersion(f.Version) >= min)
                    .OrderByDescending(f => f.Version).FirstOrDefault();
                if (match == null) throw new InvalidDataException("The Store did not return the required framework " + name + ".");
                if (!selected.Contains(match.File)) selected.Add(match.File);
            }
            return selected;
        }

        private static ulong PackVersion(Version v) =>
            ((ulong)v.Major << 48) | ((ulong)v.Minor << 32) | ((ulong)v.Build << 16) | (uint)v.Revision;

        private static async Task<StorageFile> DownloadAsync(DeliveryFile item, StorageFolder folder, long completed,
            long total, IProgress<string> progress, CancellationToken token)
        {
            var xml = await RequestAsync("FE3FileUrl", Endpoint + "/secured", token, (string)item.Identity.Attribute("UpdateID"),
                (string)item.Identity.Attribute("RevisionNumber"), "");
            string digest = (string)item.File.Attribute("Digest");
            var location = Elements(xml, "FileLocation").FirstOrDefault(e => Elements(e, "FileDigest").Any(d => d.Value == digest));
            var url = location == null ? null : Elements(location, "Url").FirstOrDefault()?.Value;
            if (!Uri.TryCreate(url, UriKind.Absolute, out var uri) || (uri.Scheme != "https" && uri.Scheme != "http") ||
                !uri.Host.EndsWith(".delivery.mp.microsoft.com", StringComparison.OrdinalIgnoreCase))
                throw new InvalidDataException("No Microsoft delivery URL matched the package digest.");
            // FE3 authenticates the digest over HTTPS. Preserve the scheme of its signed delivery URL.
            string hash = item.File.Elements("AdditionalDigest").FirstOrDefault(d => (string)d.Attribute("Algorithm") == "SHA256")?.Value;
            if (hash == null) throw new InvalidDataException("The Store did not provide a SHA-256 digest.");
            var output = await folder.CreateFileAsync(Path.GetFileName((string)item.File.Attribute("FileName")));
            long expected = (long)item.File.Attribute("Size");
            using (var response = await StoreCatalog.Client.GetAsync(uri, HttpCompletionOption.ResponseHeadersRead, token).ConfigureAwait(false))
            using (var input = await response.EnsureSuccessStatusCode().Content.ReadAsStreamAsync().ConfigureAwait(false))
            using (var stream = await output.OpenStreamForWriteAsync().ConfigureAwait(false))
            using (var sha = SHA256.Create())
            {
                var buffer = new byte[128 * 1024];
                long written = 0;
                int count, lastPercent = -1;
                while ((count = await input.ReadAsync(buffer, 0, buffer.Length, token).ConfigureAwait(false)) != 0)
                {
                    written += count;
                    if (written > expected) throw new InvalidDataException("Package exceeded the advertised size.");
                    sha.TransformBlock(buffer, 0, count, buffer, 0);
                    await stream.WriteAsync(buffer, 0, count, token).ConfigureAwait(false);
                    int percent = (int)(100.0 * (completed + written) / total);
                    if (percent != lastPercent)
                    {
                        const double mib = 1024 * 1024;
                        progress.Report($"Downloading {(completed + written) / mib:N1} of {total / mib:N1} MB · {percent}%");
                    }
                    lastPercent = percent;
                }
                sha.TransformFinalBlock(Array.Empty<byte>(), 0, 0);
                if (written != expected || Convert.ToBase64String(sha.Hash) != hash)
                    throw new InvalidDataException("Package verification failed.");
            }
            return output;
        }

        internal static async Task<string> InstallAsync(JObject package, string user, string password, bool trustLocal, bool repackAndSign,
            IProgress<string> progress, Action beginDeployment, CancellationToken token)
        {
            progress.Report("Resolving packages…");
            var files = await Task.Run(() => ResolveAsync(package, token), token);
            long total = files.Sum(file => (long)file.File.Attribute("Size"));
            token.ThrowIfCancellationRequested();
            var folder = await ApplicationData.Current.TemporaryFolder.CreateFolderAsync("StoreDownload", CreationCollisionOption.GenerateUniqueName);
            try
            {
                var downloaded = new List<StorageFile>();
                long completed = 0;
                foreach (var file in files)
                {
                    downloaded.Add(await DownloadAsync(file, folder, completed, total, progress, token));
                    completed += (long)file.File.Attribute("Size");
                }
                var preview = await AppPackageService.ReadAsync(downloaded[0]);
                token.ThrowIfCancellationRequested();
                beginDeployment();
                return await AppPackageService.InstallAsync(downloaded[0], downloaded.Skip(1).ToArray(), user, password,
                    trustLocal, progress, identityName: preview.IdentityName, applicationId: preview.ApplicationId,
                    repackAndSign: repackAndSign, architecture: preview.Architecture, version: preview.Version,
                    originalIdentityName: preview.OriginalIdentityName, originalVersion: preview.OriginalVersion,
                    originalFamilyName: preview.OriginalFamilyName);
            }
            finally
            {
                try { await folder.DeleteAsync(StorageDeleteOption.PermanentDelete); }
                catch (Exception error) { Helpers.ShellDiagnostics.WriteDebug("Store temporary file cleanup failed: " + error.HResult); }
            }
        }
    }
}
