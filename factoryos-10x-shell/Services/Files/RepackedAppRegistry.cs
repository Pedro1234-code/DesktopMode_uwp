using System;
using System.Linq;
using System.Threading;
using System.Threading.Tasks;
using Newtonsoft.Json.Linq;
using Windows.Storage;

namespace factoryos_10x_shell.Services.Files
{
    internal static class RepackedAppRegistry
    {
        private const string FileName = "RepackedApps.json";
        private const string ContainerName = "RepackedApps";
        private const string NameKey = "IdentityName";
        private const string PublisherKey = "SigningPublisher";
        private const string VersionKey = "InstalledVersion";
        private const string OriginalVersionKey = "OriginalInstalledVersion";
        private static string FamilyKey(bool repacked) => repacked ? "RepackedSourceFamily" : "OriginalFamily";

        private static readonly SemaphoreSlim LoadLock = new SemaphoreSlim(1, 1);
        private static JObject s_entries;
        private static string Key(string originalName) => originalName.ToUpperInvariant();

        internal static async Task InitializeAsync()
        {
            await LoadLock.WaitAsync();
            try
            {
                if (s_entries != null) return;
                var folder = ApplicationData.Current.LocalFolder;
                if (await folder.TryGetItemAsync(FileName) is StorageFile file)
                {
                    s_entries = JObject.Parse(await FileIO.ReadTextAsync(file));
                    return;
                }

                var entries = new JObject();
                if (ApplicationData.Current.LocalSettings.Containers.TryGetValue(ContainerName, out var legacy))
                {
                    foreach (var pair in legacy.Values)
                    {
                        if (!(pair.Value is ApplicationDataCompositeValue entry)) continue;
                        var migrated = new JObject();
                        foreach (var field in entry)
                            if (field.Value is string value) migrated[field.Key] = value;
                        entries[Key(pair.Key)] = migrated;
                    }
                    await SaveAsync(entries);
                }
                else s_entries = entries;
            }
            finally { LoadLock.Release(); }
        }

        internal static Version InstalledVersion(string originalName, bool repacked)
        {
            if (string.IsNullOrWhiteSpace(originalName)) return null;
            string version = (string)s_entries?[Key(originalName)]?[repacked ? VersionKey : OriginalVersionKey];
            return Version.TryParse(version, out var parsed) ? parsed : null;
        }

        // Called under the shared installer lock. A retry reuses the identity even if
        // deployment succeeded but the console went away before reporting its status.
        internal static async Task<string> ReserveIdentityAsync(string originalName)
        {
            if (string.IsNullOrWhiteSpace(originalName))
                throw new InvalidOperationException("The original package identity is missing.");

            string key = Key(originalName);
            var entry = (JObject)s_entries[key];
            if (entry?[NameKey] != null)
            {
                if (string.IsNullOrEmpty((string)entry[NameKey]) ||
                    !string.Equals((string)entry[PublisherKey], AppPackageSignature.Publisher, StringComparison.Ordinal))
                    throw new InvalidOperationException("The saved repack identity does not match the current signing configuration.");
                return (string)entry[NameKey];
            }

            string generatedName = AppPackageRepackService.NewIdentity();
            var updated = (JObject)s_entries.DeepClone();
            var updatedEntry = GetOrCreateEntry(updated, key);
            updatedEntry[NameKey] = generatedName;
            updatedEntry[PublisherKey] = AppPackageSignature.Publisher;
            await SaveAsync(updated);
            return generatedName;
        }

        internal static (string Name, string Family, Version Version, bool Repacked)[] GetInstalledPackages()
        {
            var entries = s_entries;
            return entries.Properties().SelectMany(entry => new[] { false, true }.Select(repacked =>
            {
                Version.TryParse((string)entry.Value[repacked ? VersionKey : OriginalVersionKey], out var version);
                return (Name: entry.Name, Family: (string)entry.Value[FamilyKey(repacked)], Version: version, Repacked: repacked);
            })).Where(entry => entry.Version != null).ToArray();
        }

        internal static async Task RecordInstallationAsync(string originalName, Version version, bool repacked, string originalFamily)
        {
            var updated = (JObject)s_entries.DeepClone();
            var entry = GetOrCreateEntry(updated, Key(originalName));
            entry[repacked ? VersionKey : OriginalVersionKey] = version.ToString();
            if (!string.IsNullOrEmpty(originalFamily)) entry[FamilyKey(repacked)] = originalFamily;
            await SaveAsync(updated);
        }

        private static JObject GetOrCreateEntry(JObject entries, string key)
        {
            if (entries[key] is JObject entry) return entry;
            entry = new JObject();
            entries[key] = entry;
            return entry;
        }

        private static async Task SaveAsync(JObject entries)
        {
            var folder = ApplicationData.Current.LocalFolder;
            var temporary = await folder.CreateFileAsync(FileName + ".tmp", CreationCollisionOption.ReplaceExisting);
            await FileIO.WriteTextAsync(temporary, entries.ToString());
            await temporary.RenameAsync(FileName, NameCollisionOption.ReplaceExisting);
            s_entries = entries;
        }
    }
}
