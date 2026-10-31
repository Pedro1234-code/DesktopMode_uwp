using System;
using System.Collections.Generic;
using System.Diagnostics;
using System.IO;
using System.Linq;
using System.Threading;
using System.Threading.Tasks;
using Windows.Storage;

namespace factoryos_10x_shell.Services.Files
{
    internal static class FileTransferService
    {
        private sealed class Node
        {
            public IStorageItem Item;
            public string Name;
            public string OriginalPath;
            public ulong Size;
            public List<Node> Children;
        }

        private sealed class Transfer
        {
            public FileOperationKind Kind;
            public IProgress<FileOperationProgress> Progress;
            public CancellationToken Token;
            public ulong TotalBytes, CompletedBytes;
            public int TotalItems, CompletedItems;
            public byte[] Buffer;
            public Stopwatch Clock = Stopwatch.StartNew();
            private long m_lastReport = -1000;

            public void Begin()
            {
                Clock.Restart();
                m_lastReport = -1000;
            }

            public void Report(string item, bool force = false, bool preparing = false)
            {
                if (!force && Clock.ElapsedMilliseconds - m_lastReport < 100) return;
                m_lastReport = Clock.ElapsedMilliseconds;
                double percent = preparing ? -1 : Kind != FileOperationKind.Delete && TotalBytes > 0
                    ? CompletedBytes * 100d / TotalBytes
                    : TotalItems > 0 ? CompletedItems * 100d / TotalItems : 0;
                string summary = preparing ? TotalItems + " items found"
                    : CompletedItems + " of " + TotalItems + " items";
                if (!preparing && Kind != FileOperationKind.Delete)
                {
                    summary += " • " + SizeText(CompletedBytes) + " of " + SizeText(TotalBytes);
                    if (Clock.Elapsed.TotalSeconds >= 0.25 && CompletedBytes > 0)
                        summary += " • " + SizeText((ulong)(CompletedBytes / Clock.Elapsed.TotalSeconds)) + "/s";
                }
                Progress.Report(new FileOperationProgress
                {
                    Status = preparing ? "Calculating items..." : Kind == FileOperationKind.Copy ? "Copying..."
                        : Kind == FileOperationKind.Move ? "Moving..." : "Deleting...",
                    Item = item,
                    Summary = summary,
                    Percent = percent
                });
            }
        }

        internal static async Task<FileOperationResult> RunAsync(IReadOnlyList<IStorageItem> items,
            StorageFolder destination, FileOperationKind kind, IProgress<FileOperationProgress> progress,
            CancellationToken token)
        {
            var result = new FileOperationResult();
            var transfer = new Transfer { Kind = kind, Progress = progress, Token = token };
            var nodes = new List<Node>();
            try
            {
                foreach (IStorageItem item in RemoveNestedSelections(items))
                {
                    token.ThrowIfCancellationRequested();
                    try
                    {
                        if (kind != FileOperationKind.Delete && item is StorageFolder folder)
                            await ValidateDestinationAsync(folder, destination, token);
                        nodes.Add(await ScanAsync(item, transfer, 0));
                    }
                    catch (OperationCanceledException) { throw; }
                    catch (Exception error) { result.AddError(item.Name + ": " + error.Message); }
                }
                transfer.Begin();
                foreach (Node node in nodes)
                {
                    token.ThrowIfCancellationRequested();
                    try
                    {
                        transfer.Report(node.Name);
                        if (kind == FileOperationKind.Delete)
                            await DeleteAsync(node, transfer);
                        else
                            await CopyAsync(node, destination, transfer);
                        result.Succeeded.Add(node.Item);
                    }
                    catch (OperationCanceledException) { throw; }
                    catch (Exception error) { result.AddError(node.Name + ": " + error.Message); }
                }
            }
            catch (OperationCanceledException) { result.Cancelled = true; }
            return result;
        }

        private static IEnumerable<IStorageItem> RemoveNestedSelections(IReadOnlyList<IStorageItem> items)
        {
            List<StorageFolder> folders = items.OfType<StorageFolder>().ToList();
            return items.Distinct().Where(item => !folders.Any(parent =>
                !ReferenceEquals(parent, item) && IsDescendant(item.Path, parent.Path)));
        }

        internal static bool IsDescendant(string path, string parent)
        {
            return !string.IsNullOrEmpty(path) && !string.IsNullOrEmpty(parent) &&
                path.StartsWith(parent.TrimEnd('\\', '/') + "\\", StringComparison.OrdinalIgnoreCase);
        }

        private static async Task ValidateDestinationAsync(StorageFolder source, StorageFolder destination, CancellationToken token)
        {
            if (destination == null) throw new InvalidOperationException("No destination folder was selected.");
            if (source.IsEqual(destination) || IsDescendant(destination.Path, source.Path))
                throw new InvalidOperationException("A folder cannot be copied or moved into itself or one of its subfolders.");
            if (!string.IsNullOrEmpty(source.Path) && !string.IsNullOrEmpty(destination.Path)) return;
            for (StorageFolder parent = destination; parent != null; parent = await parent.GetParentAsync())
            {
                token.ThrowIfCancellationRequested();
                if (source.IsEqual(parent))
                    throw new InvalidOperationException("A folder cannot be copied or moved into itself.");
            }
        }

        private static async Task<Node> ScanAsync(IStorageItem item, Transfer transfer, int depth)
        {
            transfer.Token.ThrowIfCancellationRequested();
            if (depth > 256) throw new IOException("The folder tree is too deep.");
            var node = new Node { Item = item, Name = item.Name, OriginalPath = item.Path };
            transfer.TotalItems++;
            transfer.Report(node.Name, preparing: true);
            if (item is StorageFile file)
            {
                if (transfer.Kind != FileOperationKind.Delete)
                    node.Size = (await file.GetBasicPropertiesAsync()).Size;
                transfer.TotalBytes += node.Size;
            }
            else if (item is StorageFolder folder)
            {
                node.Children = new List<Node>();
                uint index = 0;
                while (true)
                {
                    transfer.Token.ThrowIfCancellationRequested();
                    IReadOnlyList<IStorageItem> children = await folder.GetItemsAsync(index, 128);
                    if (children.Count == 0) break;
                    foreach (IStorageItem child in children)
                        node.Children.Add(await ScanAsync(child, transfer, depth + 1));
                    index += (uint)children.Count;
                }
            }
            return node;
        }

        private static async Task CopyAsync(Node node, StorageFolder destination, Transfer transfer)
        {
            transfer.Token.ThrowIfCancellationRequested();
            EnsureSourceLocation(node);
            transfer.Report(node.Name);
            if (node.Item is StorageFile file)
            {
                if (transfer.Kind == FileOperationKind.Move && !string.IsNullOrEmpty(file.Path) &&
                    string.Equals(Path.GetDirectoryName(file.Path), destination.Path, StringComparison.OrdinalIgnoreCase))
                {
                    transfer.CompletedBytes += node.Size;
                }
                else if (transfer.Kind == FileOperationKind.Move && SameVolume(file.Path, destination.Path))
                {
                    await file.MoveAsync(destination, node.Name, NameCollisionOption.GenerateUniqueName);
                    transfer.CompletedBytes += node.Size;
                }
                else
                {
                    await CopyFileAsync(file, node, destination, transfer);
                    if (transfer.Kind == FileOperationKind.Move)
                    {
                        transfer.Token.ThrowIfCancellationRequested();
                        EnsureSourceLocation(node);
                        await file.DeleteAsync();
                    }
                }
            }
            else if (node.Item is StorageFolder folder)
            {
                if (transfer.Kind == FileOperationKind.Move && !string.IsNullOrEmpty(folder.Path) &&
                    string.Equals(Path.GetDirectoryName(folder.Path), destination.Path, StringComparison.OrdinalIgnoreCase))
                {
                    CompleteChildren(node, transfer);
                }
                else
                {
                    StorageFolder copy = await destination.CreateFolderAsync(node.Name, CreationCollisionOption.GenerateUniqueName);
                    foreach (Node child in node.Children) await CopyAsync(child, copy, transfer);
                    if (transfer.Kind == FileOperationKind.Move)
                    {
                        EnsureSourceLocation(node);
                        await DeleteEmptyFolderAsync(folder, transfer.Token);
                    }
                }
            }
            transfer.CompletedItems++;
            transfer.Report(node.Name);
        }

        private static void CompleteChildren(Node node, Transfer transfer)
        {
            foreach (Node child in node.Children)
            {
                transfer.CompletedItems++;
                transfer.CompletedBytes += child.Size;
                if (child.Children != null) CompleteChildren(child, transfer);
            }
        }

        private static bool SameVolume(string source, string destination)
        {
            string root = Path.GetPathRoot(source ?? string.Empty);
            return !string.IsNullOrEmpty(root) && string.Equals(root,
                Path.GetPathRoot(destination ?? string.Empty), StringComparison.OrdinalIgnoreCase);
        }

        private static async Task CopyFileAsync(StorageFile file, Node node, StorageFolder destination, Transfer transfer)
        {
            StorageFile outputFile = null;
            bool complete = false;
            ulong copied = 0;
            try
            {
                using (Stream input = await file.OpenStreamForReadAsync())
                {
                    long expectedLength = input.CanSeek ? input.Length : -1;
                    if (expectedLength >= 0)
                        transfer.TotalBytes = transfer.TotalBytes - node.Size + (ulong)expectedLength;
                    transfer.Token.ThrowIfCancellationRequested();
                    outputFile = await destination.CreateFileAsync(node.Name, CreationCollisionOption.GenerateUniqueName);
                    using (Stream output = await outputFile.OpenStreamForWriteAsync())
                    {
                        output.SetLength(0);
                        byte[] buffer = transfer.Buffer ?? (transfer.Buffer = new byte[1024 * 1024]);
                        int count;
                        while ((count = await input.ReadAsync(buffer, 0, buffer.Length, transfer.Token)) > 0)
                        {
                            await output.WriteAsync(buffer, 0, count, transfer.Token);
                            transfer.CompletedBytes += (uint)count;
                            copied += (uint)count;
                            transfer.Report(node.Name);
                        }
                        if (expectedLength >= 0 && copied != (ulong)expectedLength)
                            throw new IOException("The source file changed while it was being copied.");
                        await output.FlushAsync(transfer.Token);
                    }
                }
                complete = true;
            }
            finally
            {
                if (!complete && outputFile != null)
                {
                    transfer.CompletedBytes -= copied;
                    try { await outputFile.DeleteAsync(StorageDeleteOption.PermanentDelete); }
                    catch (Exception error) { throw new IOException("The unfinished copy could not be removed: " + outputFile.Name, error); }
                }
            }
        }

        private static async Task DeleteAsync(Node node, Transfer transfer)
        {
            transfer.Token.ThrowIfCancellationRequested();
            EnsureSourceLocation(node);
            transfer.Report(node.Name);
            if (node.Item is StorageFolder folder)
            {
                foreach (Node child in node.Children) await DeleteAsync(child, transfer);
                EnsureSourceLocation(node);
                await DeleteEmptyFolderAsync(folder, transfer.Token);
            }
            else await node.Item.DeleteAsync();
            transfer.CompletedItems++;
            transfer.Report(node.Name);
        }

        private static async Task DeleteEmptyFolderAsync(StorageFolder folder, CancellationToken token)
        {
            token.ThrowIfCancellationRequested();
            if ((await folder.GetItemsAsync(0, 1)).Count != 0)
                throw new IOException("The source folder changed during the operation and was kept.");
            token.ThrowIfCancellationRequested();
            await folder.DeleteAsync();
        }

        private static void EnsureSourceLocation(Node node)
        {
            if (!string.IsNullOrEmpty(node.OriginalPath) &&
                !string.Equals(node.OriginalPath, node.Item.Path, StringComparison.OrdinalIgnoreCase))
                throw new IOException("The source was moved or renamed during the operation and was kept.");
        }

        private static string SizeText(ulong bytes)
        {
            string[] units = { "B", "KB", "MB", "GB", "TB" };
            double value = bytes;
            int unit = 0;
            while (value >= 1024 && unit < units.Length - 1) { value /= 1024; unit++; }
            return value.ToString(unit == 0 ? "0" : "0.0") + " " + units[unit];
        }
    }
}
