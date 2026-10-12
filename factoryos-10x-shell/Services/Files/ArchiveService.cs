using System;
using System.Collections.Generic;
using System.Diagnostics;
using System.IO;
using System.Linq;
using System.Threading;
using System.Threading.Tasks;
using SharpCompress.Archives;
using SharpCompress.Archives.Rar;
using SharpCompress.Archives.SevenZip;
using SharpCompress.Archives.Zip;
using SharpCompress.Common;
using SharpCompress.Readers;
using SharpCompress.Readers.Rar;
using SharpCompress.Readers.Zip;
using SharpCompress.Writers;
using Windows.Storage;

namespace factoryos_10x_shell.Services.Files
{
    internal sealed class ArchiveEntryInfo
    {
        public string Name { get; set; }
        public string Path { get; set; }
        public string Type { get; set; }
        public string Size { get; set; }
        public ulong SizeBytes { get; set; }
        public bool IsDirectory { get; set; }
        public string DisplayText => IsDirectory ? Path + "/" : Path + "    " + Size;
    }

    internal sealed class ArchiveProgress
    {
        public string Item { get; set; }
        public double Percent { get; set; }
    }

    internal sealed class ArchivePasswordRequiredException : Exception
    {
        public ArchivePasswordRequiredException(Exception inner)
            : base("This archive requires a password, or the supplied password is incorrect.", inner) { }
    }

    internal static class ArchiveService
    {
        internal static bool IsArchive(StorageFile file)
        {
            string extension = (file?.FileType ?? string.Empty).ToLowerInvariant();
            return extension == ".zip" || extension == ".7z" || extension == ".rar";
        }

        internal static Task<IReadOnlyList<ArchiveEntryInfo>> ListAsync(StorageFile file, string password)
        {
            return Task.Run<IReadOnlyList<ArchiveEntryInfo>>(() =>
            {
                try
                {
                    using (Stream input = file.OpenStreamForReadAsync().GetAwaiter().GetResult())
                    using (IArchive archive = OpenArchive(input, file.FileType, password))
                    {
                        return archive.Entries.Select(entry => new ArchiveEntryInfo
                        {
                            Path = NormalizeEntryPath(entry.Key),
                            Name = EntryName(entry.Key),
                            Type = entry.IsDirectory ? "File folder" : EntryType(entry.Key),
                            SizeBytes = entry.IsDirectory ? 0UL : ToUnsignedSize(entry.Size),
                            Size = entry.IsDirectory ? string.Empty : FormatSize(ToUnsignedSize(entry.Size)),
                            IsDirectory = entry.IsDirectory
                        }).Where(entry => !string.IsNullOrWhiteSpace(entry.Path)).ToList();
                    }
                }
                catch (Exception error) when (IsPasswordError(error))
                {
                    throw new ArchivePasswordRequiredException(error);
                }
            });
        }

        internal static Task ExtractAsync(StorageFile archiveFile, StorageFolder destination,
            string password, IProgress<ArchiveProgress> progress, CancellationToken cancellationToken)
        {
            return Task.Run(() =>
            {
                try
                {
                    // Enumerating a solid RAR once just to calculate its total can be
                    // nearly as expensive as extracting it. Keep RAR progress
                    // indeterminate and process the archive in a single pass.
                    ulong total = string.Equals(archiveFile.FileType, ".rar", StringComparison.OrdinalIgnoreCase)
                        ? 0
                        : GetUncompressedSize(archiveFile, password);
                    using (Stream input = archiveFile.OpenStreamForReadAsync().GetAwaiter().GetResult())
                    using (IReader reader = OpenReader(input, archiveFile.FileType, password))
                    {
                        ulong completed = 0;
                        var progressClock = Stopwatch.StartNew();
                        long lastProgressMilliseconds = -1000;
                        byte[] copyBuffer = new byte[1024 * 1024];

                        while (reader.MoveToNextEntry())
                        {
                            cancellationToken.ThrowIfCancellationRequested();
                            IEntry entry = reader.Entry;
                            string safePath = NormalizeEntryPath(entry.Key);
                            if (string.IsNullOrWhiteSpace(safePath)) continue;
                            ReportProgress(progress, safePath, completed, total, progressClock,
                                ref lastProgressMilliseconds, false);

                            if (entry.IsDirectory)
                            {
                                EnsureFolderAsync(destination, safePath).GetAwaiter().GetResult();
                                continue;
                            }

                            string parentPath = ParentPath(safePath);
                            StorageFolder parent = EnsureFolderAsync(destination, parentPath).GetAwaiter().GetResult();
                            StorageFile outputFile = parent.CreateFileAsync(EntryName(safePath), CreationCollisionOption.ReplaceExisting).AsTask().GetAwaiter().GetResult();
                            using (Stream source = reader.OpenEntryStream())
                            using (Stream output = outputFile.OpenStreamForWriteAsync().GetAwaiter().GetResult())
                            {
                                output.SetLength(0);
                                CopyWithProgress(source, output, ToUnsignedSize(entry.Size), ref completed,
                                    total, safePath, progress, cancellationToken, progressClock,
                                    ref lastProgressMilliseconds, copyBuffer);
                            }
                        }
                        progress?.Report(new ArchiveProgress { Item = "Completed", Percent = 100 });
                    }
                }
                catch (Exception error) when (IsPasswordError(error))
                {
                    throw new ArchivePasswordRequiredException(error);
                }
            }, cancellationToken);
        }

        internal static Task CompressAsync(IReadOnlyList<IStorageItem> items, StorageFile destination,
            bool sevenZip, IProgress<ArchiveProgress> progress, CancellationToken cancellationToken)
        {
            return Task.Run(() =>
            {
                List<ArchiveSource> sources = new List<ArchiveSource>();
                foreach (IStorageItem item in items)
                    CollectSources(item, item.Name, sources, cancellationToken);

                ulong total = sources.Aggregate<ArchiveSource, ulong>(0, (sum, item) => sum + item.Size);
                ulong completed = 0;
                using (Stream output = destination.OpenStreamForWriteAsync().GetAwaiter().GetResult())
                {
                    output.SetLength(0);
                    ArchiveType type = sevenZip ? ArchiveType.SevenZip : ArchiveType.Zip;
                    CompressionType compression = sevenZip ? CompressionType.LZMA2 : CompressionType.Deflate;
                    using (IWriter writer = WriterFactory.OpenWriter(
                        output,
                        type,
                        new WriterOptions(compression) { LeaveStreamOpen = true }))
                    {
                        foreach (ArchiveSource source in sources)
                        {
                            cancellationToken.ThrowIfCancellationRequested();
                            progress?.Report(new ArchiveProgress { Item = source.Path, Percent = total == 0 ? 0 : completed * 100d / total });
                            using (Stream input = source.File.OpenStreamForReadAsync().GetAwaiter().GetResult())
                                writer.Write(source.Path, input, source.File.DateCreated.DateTime);
                            completed += source.Size;
                        }
                    }
                }
                progress?.Report(new ArchiveProgress { Item = "Completed", Percent = 100 });
            }, cancellationToken);
        }

        private static ulong GetUncompressedSize(StorageFile file, string password)
        {
            using (Stream input = file.OpenStreamForReadAsync().GetAwaiter().GetResult())
            using (IArchive archive = OpenArchive(input, file.FileType, password))
                return archive.Entries
                    .Where(entry => !entry.IsDirectory)
                    .Aggregate<IArchiveEntry, ulong>(0, (sum, entry) => sum + ToUnsignedSize(entry.Size));
        }

        private static ReaderOptions ReaderOptions(string password, string extensionHint = null) => new ReaderOptions
        {
            Password = string.IsNullOrEmpty(password) ? null : password,
            LeaveStreamOpen = false,
            ExtensionHint = string.IsNullOrWhiteSpace(extensionHint) ? null : extensionHint.TrimStart('.')
        };

        private static IArchive OpenArchive(Stream input, string extension, string password)
        {
            ReaderOptions options = ReaderOptions(password, extension);
            switch ((extension ?? string.Empty).ToLowerInvariant())
            {
                case ".rar":
                    return RarArchive.OpenArchive(input, options);
                case ".7z":
                    return SevenZipArchive.OpenArchive(input, options);
                case ".zip":
                    return ZipArchive.OpenArchive(input, options);
                default:
                    return ArchiveFactory.OpenArchive(input, options);
            }
        }

        private static IReader OpenReader(Stream input, string extension, string password)
        {
            ReaderOptions options = ReaderOptions(password, extension);
            switch ((extension ?? string.Empty).ToLowerInvariant())
            {
                case ".rar":
                    return RarReader.OpenReader(input, options);
                case ".zip":
                    return ZipReader.OpenReader(input, options);
                default:
                    return ReaderFactory.OpenReader(input, options);
            }
        }

        private static void CollectSources(IStorageItem item, string path, List<ArchiveSource> output, CancellationToken token)
        {
            token.ThrowIfCancellationRequested();
            StorageFile file = item as StorageFile;
            if (file != null)
            {
                ulong size = file.GetBasicPropertiesAsync().AsTask().GetAwaiter().GetResult().Size;
                output.Add(new ArchiveSource { File = file, Path = path.Replace('\\', '/'), Size = size });
                return;
            }

            StorageFolder folder = item as StorageFolder;
            if (folder == null) return;
            foreach (StorageFile child in folder.GetFilesAsync().AsTask().GetAwaiter().GetResult())
                CollectSources(child, path + "/" + child.Name, output, token);
            foreach (StorageFolder child in folder.GetFoldersAsync().AsTask().GetAwaiter().GetResult())
                CollectSources(child, path + "/" + child.Name, output, token);
        }

        private static async Task<StorageFolder> EnsureFolderAsync(StorageFolder root, string relativePath)
        {
            StorageFolder current = root;
            if (string.IsNullOrWhiteSpace(relativePath)) return current;
            foreach (string segment in relativePath.Split(new[] { '/' }, StringSplitOptions.RemoveEmptyEntries))
                current = await current.CreateFolderAsync(segment, CreationCollisionOption.OpenIfExists);
            return current;
        }

        private static void CopyWithProgress(Stream source, Stream destination, ulong entrySize,
            ref ulong completed, ulong total, string name, IProgress<ArchiveProgress> progress,
            CancellationToken token, Stopwatch progressClock, ref long lastProgressMilliseconds,
            byte[] buffer)
        {
            int read;
            ulong entryCompleted = 0;
            while ((read = source.Read(buffer, 0, buffer.Length)) > 0)
            {
                token.ThrowIfCancellationRequested();
                destination.Write(buffer, 0, read);
                entryCompleted += (ulong)read;
                ReportProgress(progress, name, completed + Math.Min(entryCompleted, entrySize),
                    total, progressClock, ref lastProgressMilliseconds, false);
            }
            completed += entrySize;
        }

        private static void ReportProgress(IProgress<ArchiveProgress> progress, string item,
            ulong completed, ulong total, Stopwatch clock, ref long lastReportMilliseconds, bool force)
        {
            if (progress == null) return;
            long now = clock.ElapsedMilliseconds;
            if (!force && now - lastReportMilliseconds < 125) return;
            lastReportMilliseconds = now;
            progress.Report(new ArchiveProgress
            {
                Item = item,
                Percent = total == 0 ? -1 : completed * 100d / total
            });
        }

        private static string NormalizeEntryPath(string value)
        {
            if (string.IsNullOrWhiteSpace(value)) return string.Empty;
            string path = value.Replace('\\', '/').TrimStart('/');
            string[] segments = path.Split(new[] { '/' }, StringSplitOptions.RemoveEmptyEntries);
            if (segments.Any(segment => segment == "." || segment == ".." || segment.Contains(":")))
                throw new InvalidDataException("The archive contains an unsafe path.");
            return string.Join("/", segments);
        }

        private static string ParentPath(string path)
        {
            int slash = path.LastIndexOf('/');
            return slash < 0 ? string.Empty : path.Substring(0, slash);
        }

        private static string EntryName(string path)
        {
            string normalized = (path ?? string.Empty).TrimEnd('/', '\\');
            int slash = Math.Max(normalized.LastIndexOf('/'), normalized.LastIndexOf('\\'));
            return slash < 0 ? normalized : normalized.Substring(slash + 1);
        }

        private static string EntryType(string path)
        {
            string extension = Path.GetExtension(path ?? string.Empty);
            return string.IsNullOrWhiteSpace(extension) ? "File" : extension.TrimStart('.').ToUpperInvariant() + " file";
        }

        private static bool IsPasswordError(Exception error)
        {
            for (Exception current = error; current != null; current = current.InnerException)
            {
                if (current is System.Security.Cryptography.CryptographicException) return true;
                string text = current.Message ?? string.Empty;
                if (text.IndexOf("password", StringComparison.OrdinalIgnoreCase) >= 0 ||
                    text.IndexOf("encrypted", StringComparison.OrdinalIgnoreCase) >= 0 ||
                    text.IndexOf("cryptographic", StringComparison.OrdinalIgnoreCase) >= 0)
                    return true;
            }
            return false;
        }

        private static string FormatSize(ulong bytes)
        {
            string[] units = { "B", "KB", "MB", "GB", "TB" };
            double value = bytes;
            int unit = 0;
            while (value >= 1024 && unit < units.Length - 1) { value /= 1024; unit++; }
            return unit == 0 ? bytes + " B" : value.ToString(value >= 10 ? "0" : "0.0") + " " + units[unit];
        }

        private static ulong ToUnsignedSize(long size) => size <= 0 ? 0UL : (ulong)size;

        private sealed class ArchiveSource
        {
            public StorageFile File { get; set; }
            public string Path { get; set; }
            public ulong Size { get; set; }
        }
    }
}
