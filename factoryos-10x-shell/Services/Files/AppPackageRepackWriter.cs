using System;
using System.Collections.Generic;
using System.IO;
using System.IO.Compression;
using System.Linq;
using System.Security.Cryptography;
using System.Text;
using System.Xml;
using System.Xml.Linq;

namespace factoryos_10x_shell.Services.Files
{
    internal static class AppPackageRepackWriter
    {
        private const string ManifestName = "AppxManifest.xml";
        private const string BlockMapName = "AppxBlockMap.xml";
        private const string ContentTypesName = "[Content_Types].xml";
        private const string SignatureName = "AppxSignature.p7x";
        private const string CatalogName = "AppxMetadata/CodeIntegrity.cat";
        private const string BundleManifestName = "AppxMetadata/AppxBundleManifest.xml";
        private const int BlockSize = 65536;
        private static readonly XNamespace BlockMapNamespace = "http://schemas.microsoft.com/appx/2010/blockmap";
        private static readonly XNamespace ContentTypesNamespace = "http://schemas.openxmlformats.org/package/2006/content-types";
        private static readonly uint[] CrcTable = CreateCrcTable();

        private sealed class Record
        {
            internal byte[] Name;
            internal long Offset, Size;
            internal bool UseDescriptor;
            internal ushort Flags;
            internal uint Crc;
        }

        internal static void Write(Stream input, Stream output, string identityName, Func<byte[], byte[]> signHashes)
        {
            using (var archive = new ZipArchive(input, ZipArchiveMode.Read, true))
            using (var writer = new BinaryWriter(output, Encoding.UTF8, true))
            using (var sha = SHA256.Create())
            using (var fileHash = IncrementalHash.CreateHash(HashAlgorithmName.SHA256))
            {
                var entries = ReadEntries(archive);
                if (entries.ContainsKey(BundleManifestName))
                    throw new InvalidDataException("Repacking bundles is not supported.");
                var manifest = ReadXml(entries, ManifestName);
                RewriteIdentity(manifest, "Package", identityName);
                var ns = manifest.Root.Name.Namespace;
                if (manifest.Descendants().Any(element => element.Name.LocalName == "MainPackageDependency"))
                    throw new InvalidDataException("Repacking optional packages with a main package dependency is not supported.");
                manifest.Root.Element(ns + "Properties")?.Element(ns + "PublisherDisplayName")?.SetValue("TestPub");

                var types = ReadXml(entries, ContentTypesName);
                var blockMap = CreateBlockMap();
                var records = new List<Record>();
                var buffer = new byte[BlockSize];
                foreach (var pair in entries)
                {
                    if (IsFootprint(pair.Key)) continue;
                    using (var source = pair.Value.Open())
                        records.Add(WritePayload(writer, fileHash, sha, blockMap, pair.Key, source, pair.Value.Length, buffer));
                }
                records.Add(WriteXmlPart(writer, fileHash, sha, blockMap, ManifestName, manifest, buffer));
                FinishArchive(writer, fileHash, sha, records, blockMap, types, buffer, signHashes);
            }
        }

        internal static void WriteBundle(Stream input, Stream output, Stream childInput, Stream childOutput,
            string identityName, Func<byte[], bool, byte[]> signHashes, Action<int, int> progress)
        {
            using (var archive = new ZipArchive(input, ZipArchiveMode.Read, true))
            using (var writer = new BinaryWriter(output, Encoding.UTF8, true))
            using (var sha = SHA256.Create())
            using (var fileHash = IncrementalHash.CreateHash(HashAlgorithmName.SHA256))
            {
                var entries = ReadEntries(archive);
                var manifest = ReadXml(entries, BundleManifestName);
                RewriteIdentity(manifest, "Bundle", identityName);
                var types = ReadXml(entries, ContentTypesName);
                var ns = manifest.Root.Name.Namespace;
                var children = manifest.Root.Element(ns + "Packages")?.Elements(ns + "Package").ToList();
                if (children == null || children.Count == 0)
                    throw new InvalidDataException("The bundle has no embedded packages.");
                if (manifest.Root.Descendants().Any(element => element.Name.LocalName == "OptionalBundle"))
                    throw new InvalidDataException("Repacking bundles with optional bundle references is not supported.");

                var blockMap = CreateBlockMap();
                var records = new List<Record>();
                var buffer = new byte[BlockSize];
                var payloadNames = new HashSet<string>(StringComparer.OrdinalIgnoreCase);
                var decodedEntries = entries.ToDictionary(pair => Uri.UnescapeDataString(pair.Key),
                    pair => pair, StringComparer.OrdinalIgnoreCase);
                for (int i = 0; i < children.Count; i++)
                {
                    var child = children[i];
                    string name = ((string)child.Attribute("FileName"))?.Replace('\\', '/');
                    if (string.IsNullOrEmpty(name) ||
                        !decodedEntries.TryGetValue(Uri.UnescapeDataString(name), out var payload) ||
                        !payloadNames.Add(payload.Key))
                        throw new InvalidDataException("The bundle references a missing or duplicate embedded package: " + name);
                    progress?.Invoke(i + 1, children.Count);
                    childInput.SetLength(0);
                    childInput.Position = 0;
                    childOutput.SetLength(0);
                    childOutput.Position = 0;
                    using (var source = payload.Value.Open()) source.CopyTo(childInput);
                    childInput.Position = 0;
                    Write(childInput, childOutput, identityName, hashes => signHashes(hashes, false));
                    childOutput.Position = 0;

                    // Embedded packages are stored as ZIP members. Their own block
                    // maps validate the payload and the bundle records the data offset.
                    var record = WritePayload(writer, fileHash, sha, null, payload.Key, childOutput, childOutput.Length, buffer);
                    records.Add(record);
                    child.SetAttributeValue("Offset", record.Offset + LocalHeader(record).Length);
                    child.SetAttributeValue("Size", record.Size);
                }

                foreach (var pair in entries)
                {
                    if (payloadNames.Contains(pair.Key) || IsFootprint(pair.Key) ||
                        string.Equals(pair.Key, BundleManifestName, StringComparison.OrdinalIgnoreCase)) continue;
                    using (var source = pair.Value.Open())
                        records.Add(WritePayload(writer, fileHash, sha, blockMap, pair.Key, source, pair.Value.Length, buffer));
                }
                records.Add(WriteXmlPart(writer, fileHash, sha, blockMap, BundleManifestName, manifest, buffer));
                FinishArchive(writer, fileHash, sha, records, blockMap, types, buffer, hashes => signHashes(hashes, true));
            }
        }

        private static void RewriteIdentity(XDocument manifest, string rootName, string identityName)
        {
            var identity = manifest.Root?.Element(manifest.Root.Name.Namespace + "Identity");
            if (identity == null || manifest.Root.Name.LocalName != rootName ||
                string.IsNullOrWhiteSpace((string)identity.Attribute("Name")))
                throw new InvalidDataException("The package manifest has no valid identity.");
            identity.SetAttributeValue("Name", identityName);
            identity.SetAttributeValue("Publisher", AppPackageSignature.Publisher);
        }

        private static XElement CreateBlockMap() => new XElement(BlockMapNamespace + "BlockMap",
            new XAttribute("HashMethod", "http://www.w3.org/2001/04/xmlenc#sha256"));

        private static Record WriteXmlPart(BinaryWriter writer, IncrementalHash fileHash, HashAlgorithm sha,
            XElement blockMap, string name, XNode element, byte[] buffer)
        {
            using (var source = new MemoryStream(ToXmlBytes(element)))
                return WritePayload(writer, fileHash, sha, blockMap, name, source, source.Length, buffer);
        }

        private static void FinishArchive(BinaryWriter writer, IncrementalHash fileHash, HashAlgorithm sha,
            List<Record> records, XElement blockMap, XDocument types, byte[] buffer, Func<byte[], byte[]> signHashes)
        {
            if (types.Root?.Name != ContentTypesNamespace + "Types")
                throw new InvalidDataException("The package content types are invalid.");
            types.Root.Elements(ContentTypesNamespace + "Override").Where(element =>
                MatchesPart(element, CatalogName) || MatchesPart(element, SignatureName)).Remove();
            types.Root.Add(new XElement(ContentTypesNamespace + "Override", new XAttribute("PartName", "/" + SignatureName),
                new XAttribute("ContentType", "application/vnd.ms-appx.signature")));
            var blockMapBytes = ToXmlBytes(blockMap);
            using (var source = new MemoryStream(blockMapBytes))
                records.Add(WritePayload(writer, fileHash, sha, null, BlockMapName, source, source.Length, buffer));
            var typesBytes = ToXmlBytes(types);
            records.Add(WriteSmallEntry(writer, ContentTypesName, typesBytes, fileHash));

            // Authenticode hashes the directory as it would exist before adding the signature.
            var unsignedDirectory = CreateDirectory(records, writer.BaseStream.Position);
            byte[] hashes;
            using (var data = new MemoryStream())
            using (var digest = new BinaryWriter(data, Encoding.ASCII, true))
            {
                digest.Write(Encoding.ASCII.GetBytes("APPX"));
                WriteDigest(digest, "AXPC", fileHash.GetHashAndReset());
                WriteDigest(digest, "AXCD", sha.ComputeHash(unsignedDirectory));
                WriteDigest(digest, "AXCT", sha.ComputeHash(typesBytes));
                WriteDigest(digest, "AXBM", sha.ComputeHash(blockMapBytes));
                hashes = data.ToArray();
            }
            records.Add(WriteSmallEntry(writer, SignatureName, signHashes(hashes), null));
            writer.Write(CreateDirectory(records, writer.BaseStream.Position));
        }

        private static Dictionary<string, ZipArchiveEntry> ReadEntries(ZipArchive archive)
        {
            var entries = new Dictionary<string, ZipArchiveEntry>(StringComparer.OrdinalIgnoreCase);
            var decodedNames = new HashSet<string>(StringComparer.OrdinalIgnoreCase);
            foreach (var entry in archive.Entries)
            {
                string name = entry.FullName.Replace('\\', '/');
                if (name.EndsWith("/", StringComparison.Ordinal)) continue;
                var decoded = name.Split('/').Select(Uri.UnescapeDataString).ToArray();
                if (decoded.Any(part => part.Length == 0 || part == "." || part == ".." ||
                    part.IndexOfAny(new[] { '\0', ':', '/', '\\' }) >= 0))
                    throw new InvalidDataException("The package contains an invalid part name.");
                string decodedName = string.Join("/", decoded);
                if (!decodedNames.Add(decodedName))
                    throw new InvalidDataException("The package contains a duplicate part name.");
                if (!string.Equals(name, decodedName, StringComparison.Ordinal) &&
                    (IsFootprint(decodedName) || string.Equals(decodedName, BundleManifestName, StringComparison.OrdinalIgnoreCase)))
                    throw new InvalidDataException("The package contains an encoded metadata name.");
                entries.Add(name, entry);
            }
            return entries;
        }

        private static bool IsFootprint(string name) =>
            new[] { ManifestName, BlockMapName, ContentTypesName, SignatureName, CatalogName }
                .Contains(name, StringComparer.OrdinalIgnoreCase);

        private static bool MatchesPart(XElement element, string name) =>
            string.Equals((string)element.Attribute("PartName"), "/" + name, StringComparison.OrdinalIgnoreCase);

        private static XDocument ReadXml(Dictionary<string, ZipArchiveEntry> entries, string name)
        {
            if (!entries.TryGetValue(name, out var entry) || entry.Length > 2 * 1024 * 1024)
                throw new InvalidDataException("Missing or oversized package metadata: " + name);
            using (var stream = entry.Open())
            using (var reader = XmlReader.Create(stream,
                new XmlReaderSettings { DtdProcessing = DtdProcessing.Prohibit, XmlResolver = null, MaxCharactersInDocument = 2 * 1024 * 1024 }))
                return XDocument.Load(reader);
        }

        private static byte[] ToXmlBytes(XNode xml)
        {
            using (var data = new MemoryStream())
            {
                using (var writer = XmlWriter.Create(data, new XmlWriterSettings { Encoding = new UTF8Encoding(false) }))
                    xml.WriteTo(writer);
                return data.ToArray();
            }
        }

        private static Record WritePayload(BinaryWriter output, IncrementalHash hash, HashAlgorithm sha, XElement map,
            string name, Stream source, long size, byte[] buffer)
        {
            var record = MakeRecord(output, name, size, true);
            var header = LocalHeader(record);
            WriteHashed(output, hash, header, header.Length);
            var file = map == null ? null : new XElement(BlockMapNamespace + "File",
                new XAttribute("Name", Uri.UnescapeDataString(name).Replace('/', '\\')),
                new XAttribute("Size", size), new XAttribute("LfhSize", header.Length));
            uint crc = uint.MaxValue;
            for (long remaining = size; remaining > 0;)
            {
                int count = (int)Math.Min(buffer.Length, remaining);
                int offset = 0;
                while (offset < count)
                {
                    int read = source.Read(buffer, offset, count - offset);
                    if (read == 0) throw new InvalidDataException("Unexpected end of package part: " + name);
                    offset += read;
                }
                crc = UpdateCrc(crc, buffer, count);
                WriteHashed(output, hash, buffer, count);
                file?.Add(new XElement(BlockMapNamespace + "Block", new XAttribute("Hash", Convert.ToBase64String(sha.ComputeHash(buffer, 0, count)))));
                remaining -= count;
            }
            record.Crc = ~crc;
            var descriptor = Bytes(writer =>
            {
                writer.Write(0x08074B50u);
                writer.Write(record.Crc);
                writer.Write(size);
                writer.Write(size);
            });
            WriteHashed(output, hash, descriptor, descriptor.Length);
            if (file != null) map.Add(file);
            return record;
        }

        private static Record WriteSmallEntry(BinaryWriter output, string name, byte[] data, IncrementalHash hash)
        {
            var record = MakeRecord(output, name, data.Length, false);
            record.Crc = ~UpdateCrc(uint.MaxValue, data, data.Length);
            var header = LocalHeader(record);
            WriteHashed(output, hash, header, header.Length);
            WriteHashed(output, hash, data, data.Length);
            return record;
        }

        private static Record MakeRecord(BinaryWriter output, string name, long size, bool useDescriptor) => new Record
        {
            Name = Encoding.UTF8.GetBytes(name),
            Offset = output.BaseStream.Position,
            Size = size,
            UseDescriptor = useDescriptor,
            Flags = (ushort)((useDescriptor ? 8 : 0) | (name.Any(c => c > 127) ? 0x800 : 0))
        };

        private static byte[] LocalHeader(Record record) => Bytes(writer =>
        {
            writer.Write(0x04034B50u);
            writer.Write((ushort)(record.UseDescriptor ? 45 : 20));
            writer.Write(record.Flags);
            writer.Write((ushort)0); // stored
            writer.Write(0x00210000u); // DOS date for January 1, 1980
            writer.Write(record.Crc);
            writer.Write(record.UseDescriptor ? 0u : (uint)record.Size);
            writer.Write(record.UseDescriptor ? 0u : (uint)record.Size);
            writer.Write(checked((ushort)record.Name.Length));
            writer.Write((ushort)0); // extra field length
            writer.Write(record.Name);
        });

        private static byte[] CreateDirectory(List<Record> records, long start) => Bytes(writer =>
        {
            foreach (var record in records)
            {
                bool zip64 = record.UseDescriptor || record.Offset >= uint.MaxValue;
                writer.Write(0x02014B50u);
                writer.Write((ushort)45);
                writer.Write((ushort)(zip64 ? 45 : 20));
                writer.Write(record.Flags);
                writer.Write((ushort)0);
                writer.Write(0x00210000u);
                writer.Write(record.Crc);
                writer.Write(zip64 ? uint.MaxValue : (uint)record.Size);
                writer.Write(zip64 ? uint.MaxValue : (uint)record.Size);
                writer.Write(checked((ushort)record.Name.Length));
                writer.Write((ushort)(zip64 ? 28 : 0));
                writer.Write((ushort)0); // comment length
                writer.Write((ushort)0); // disk number
                writer.Write((ushort)0); // internal attributes
                writer.Write(0u); // external attributes
                writer.Write(zip64 ? uint.MaxValue : (uint)record.Offset);
                writer.Write(record.Name);
                if (zip64)
                {
                    writer.Write((ushort)1);
                    writer.Write((ushort)24);
                    writer.Write(record.Size);
                    writer.Write(record.Size);
                    writer.Write(record.Offset);
                }
            }
            long size = writer.BaseStream.Position;
            writer.Write(0x06064B50u);
            writer.Write(44L);
            writer.Write((ushort)45);
            writer.Write((ushort)45);
            writer.Write(0L); // disk numbers
            writer.Write((long)records.Count);
            writer.Write((long)records.Count);
            writer.Write(size);
            writer.Write(start);
            writer.Write(0x07064B50u);
            writer.Write(0u);
            writer.Write(start + size);
            writer.Write(1u);
            writer.Write(0x06054B50u);
            writer.Write(0u); // disk numbers
            writer.Write(ushort.MaxValue);
            writer.Write(ushort.MaxValue);
            writer.Write(uint.MaxValue);
            writer.Write(uint.MaxValue);
            writer.Write((ushort)0); // comment length
        });

        private static void WriteDigest(BinaryWriter writer, string tag, byte[] digest)
        {
            writer.Write(Encoding.ASCII.GetBytes(tag));
            writer.Write(digest);
        }

        private static void WriteHashed(BinaryWriter writer, IncrementalHash hash, byte[] bytes, int count)
        {
            writer.Write(bytes, 0, count);
            hash?.AppendData(bytes, 0, count);
        }

        private static byte[] Bytes(Action<BinaryWriter> write)
        {
            using (var data = new MemoryStream())
            {
                using (var writer = new BinaryWriter(data, Encoding.UTF8, true)) write(writer);
                return data.ToArray();
            }
        }

        private static uint[] CreateCrcTable()
        {
            var table = new uint[256];
            for (uint value = 0; value < table.Length; value++)
            {
                uint crc = value;
                for (int bit = 0; bit < 8; bit++)
                    crc = (crc >> 1) ^ ((crc & 1) == 0 ? 0u : 0xEDB88320u);
                table[value] = crc;
            }
            return table;
        }

        private static uint UpdateCrc(uint crc, byte[] data, int count)
        {
            for (int i = 0; i < count; i++) crc = (crc >> 8) ^ CrcTable[(crc ^ data[i]) & 0xFF];
            return crc;
        }
    }
}
