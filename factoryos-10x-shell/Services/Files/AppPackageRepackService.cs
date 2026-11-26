using System;
using System.IO;
using System.Text;
using System.Threading.Tasks;
using Windows.Security.Cryptography;
using Windows.Security.Cryptography.Core;
using Windows.Security.Cryptography.DataProtection;
using Windows.Storage;
using Windows.Storage.Streams;

namespace factoryos_10x_shell.Services.Files
{
    internal static class AppPackageRepackService
    {
        internal static bool IsBundle(StorageFile file)
        {
            string extension = Path.GetExtension(file.Name);
            return string.Equals(extension, ".appxbundle", StringComparison.OrdinalIgnoreCase) ||
                string.Equals(extension, ".msixbundle", StringComparison.OrdinalIgnoreCase);
        }

        internal static async Task<StorageFile> CreateAsync(StorageFile source, StorageFolder outputFolder, StorageFile certificate,
            string identity, IProgress<string> progress)
        {
            if (!AppPackageService.IsPackage(source)) throw new InvalidOperationException("Unsupported app package format.");
            bool isBundle = IsBundle(source);
            var provider = AsymmetricKeyAlgorithmProvider.OpenAlgorithm(AsymmetricAlgorithmNames.RsaSignPkcs1Sha256);
            var (key, created) = await GetSigningKeyAsync(provider);
            byte[] Sign(byte[] value) => ToArray(CryptographicEngine.Sign(key, CryptographicBuffer.CreateFromByteArray(value)));

            var publicKey = key.ExportPublicKey(CryptographicPublicKeyBlobType.Pkcs1RsaPublicKey);
            var serialHash = HashAlgorithmProvider.OpenAlgorithm(HashAlgorithmNames.Sha256).HashData(publicKey);
            var serial = new byte[16];
            Array.Copy(ToArray(serialHash), serial, serial.Length);
            var certBytes = AppPackageSignature.CreateCertificate(serial, ToArray(publicKey), created, Sign);
            await FileIO.WriteBytesAsync(certificate, certBytes);
            var package = await outputFolder.CreateFileAsync(identity + Path.GetExtension(source.Name).ToLowerInvariant(),
                CreationCollisionOption.FailIfExists);
            var childInputFile = isBundle ? await outputFolder.CreateFileAsync("bundle-source.tmp") : null;
            var childOutputFile = isBundle ? await outputFolder.CreateFileAsync("bundle-repacked.tmp") : null;
            progress.Report("Repacking and signing with the local TestPub certificate…");
            await Task.Run(async () =>
            {
                using (var input = await source.OpenStreamForReadAsync())
                using (var output = await package.OpenStreamForWriteAsync())
                {
                    if (!isBundle)
                    {
                        AppPackageRepackWriter.Write(input, output, identity,
                            hashes => AppPackageSignature.CreatePackageSignature(hashes, serial, certBytes, Sign));
                        return;
                    }

                    using (var childInput = (await childInputFile.OpenAsync(FileAccessMode.ReadWrite)).AsStream())
                    using (var childOutput = (await childOutputFile.OpenAsync(FileAccessMode.ReadWrite)).AsStream())
                        AppPackageRepackWriter.WriteBundle(input, output, childInput, childOutput, identity,
                            (hashes, bundle) => AppPackageSignature.CreatePackageSignature(hashes, serial, certBytes, Sign, bundle),
                            (part, count) => progress.Report($"Repacking bundle package {part} of {count}…"));
                }
            });
            return package;
        }

        internal static string NewIdentity()
        {
            const string prefix = "Microsoft.MicrosoftEdge.";

            var name = new StringBuilder(prefix);
            while (name.Length < prefix.Length + 8)
            {
                // Rejection sampling keeps each letter equally likely.
                uint value = CryptographicBuffer.GenerateRandomNumber();
                if (value < uint.MaxValue - uint.MaxValue % 26) name.Append((char)('a' + value % 26));
            }
            return name.ToString();
        }

        private static async Task<(CryptographicKey, DateTimeOffset)> GetSigningKeyAsync(AsymmetricKeyAlgorithmProvider provider)
        {
            var local = ApplicationData.Current.LocalFolder;
            const string keyName = "TestPub-AppInstaller.key";
            if (await local.TryGetItemAsync(keyName) is StorageFile stored)
            {
                var unprotected = await new DataProtectionProvider().UnprotectAsync(await FileIO.ReadBufferAsync(stored));
                var bytes = ToArray(unprotected);
                using (var input = new BinaryReader(new MemoryStream(bytes)))
                {
                    var date = new DateTimeOffset(input.ReadInt64(), TimeSpan.Zero);
                    var keyBytes = input.ReadBytes(bytes.Length - sizeof(long));
                    try
                    {
                        return (provider.ImportKeyPair(CryptographicBuffer.CreateFromByteArray(keyBytes),
                            CryptographicPrivateKeyBlobType.Pkcs8RawPrivateKeyInfo), date);
                    }
                    finally
                    {
                        Array.Clear(keyBytes, 0, keyBytes.Length);
                        Array.Clear(bytes, 0, bytes.Length);
                    }
                }
            }

            var key = provider.CreateKeyPair(2048);
            var created = DateTimeOffset.UtcNow;
            using (var data = new DataWriter())
            {
                data.ByteOrder = ByteOrder.LittleEndian;
                data.WriteInt64(created.UtcDateTime.Ticks);
                data.WriteBuffer(key.Export(CryptographicPrivateKeyBlobType.Pkcs8RawPrivateKeyInfo));
                var protectedKey = await new DataProtectionProvider("LOCAL=user").ProtectAsync(data.DetachBuffer());
                var pending = await local.CreateFileAsync(keyName + ".tmp", CreationCollisionOption.GenerateUniqueName);
                try
                {
                    await FileIO.WriteBufferAsync(pending, protectedKey);
                    await pending.RenameAsync(keyName, NameCollisionOption.FailIfExists);
                }
                catch
                {
                    try { await pending.DeleteAsync(StorageDeleteOption.PermanentDelete); }
                    catch { }
                    throw;
                }
            }
            return (key, created);
        }

        private static byte[] ToArray(IBuffer buffer)
        {
            CryptographicBuffer.CopyToByteArray(buffer, out byte[] bytes);
            return bytes;
        }
    }
}
