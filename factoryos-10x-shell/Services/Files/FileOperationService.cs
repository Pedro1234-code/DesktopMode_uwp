using System;
using System.Collections.Generic;
using System.Collections.ObjectModel;
using System.IO;
using System.Linq;
using System.Threading.Tasks;
using Windows.Storage;
using Windows.UI.Xaml;

namespace factoryos_10x_shell.Services.Files
{
    internal sealed class FileOperationService
    {
        internal static FileOperationService Instance { get; } = new FileOperationService();
        internal ObservableCollection<FileOperationModel> Windows { get; } = new ObservableCollection<FileOperationModel>();
        internal event EventHandler WindowsChanged;
        internal event Action<FileOperationModel> ActivationRequested;
        internal event Action<FileOperationModel> OperationCompleted;

        private FileOperationService() { }

        internal FileOperationModel StartTransfer(IReadOnlyList<IStorageItem> items, StorageFolder destination, FileOperationKind kind)
        {
            var sources = items.ToList();
            var operation = Create(kind == FileOperationKind.Copy ? "Copying items" : kind == FileOperationKind.Move ? "Moving items" : "Deleting items",
                destination == null ? string.Empty : "To: " + destination.DisplayName);
            if (destination != null) AddAffectedPath(operation, destination.Path);
            if (kind != FileOperationKind.Copy)
                foreach (IStorageItem item in sources)
                    if (!string.IsNullOrEmpty(item.Path)) AddAffectedPath(operation, Path.GetDirectoryName(item.Path));
            var progress = new Progress<FileOperationProgress>(operation.Update);
            operation.Completion = RunAsync(operation,
                () => Task.Run(() => FileTransferService.RunAsync(sources, destination, kind, progress, operation.Cancellation.Token)));
            return operation;
        }

        internal FileOperationModel StartExtraction(StorageFile archive, StorageFolder destination)
        {
            var operation = Create("Extracting " + archive.Name, "To: " + destination.DisplayName);
            AddAffectedPath(operation, destination.Path);
            var progress = ArchiveProgressFor(operation);
            operation.Completion = RunAsync(operation, async () =>
            {
                string password = null;
                while (true)
                {
                    operation.Cancellation.Token.ThrowIfCancellationRequested();
                    try
                    {
                        await ArchiveService.ExtractAsync(archive, destination, password, progress, operation.Cancellation.Token);
                        return new FileOperationResult();
                    }
                    catch (ArchivePasswordRequiredException)
                    {
                        Activate(operation);
                        password = await operation.RequestPasswordAsync();
                    }
                }
            });
            return operation;
        }

        internal FileOperationModel StartCompression(IReadOnlyList<IStorageItem> items, StorageFolder destination, string name, bool sevenZip)
        {
            var sources = items.ToList();
            var operation = Create("Creating " + name, "In: " + destination.DisplayName);
            AddAffectedPath(operation, destination.Path);
            var progress = ArchiveProgressFor(operation);
            operation.Completion = RunAsync(operation, async () =>
            {
                StorageFile output = null;
                try
                {
                    operation.Cancellation.Token.ThrowIfCancellationRequested();
                    output = await destination.CreateFileAsync(name, CreationCollisionOption.GenerateUniqueName);
                    await ArchiveService.CompressAsync(sources, output, sevenZip, progress, operation.Cancellation.Token);
                    return new FileOperationResult();
                }
                catch
                {
                    if (output != null) await output.DeleteAsync(StorageDeleteOption.PermanentDelete);
                    throw;
                }
            });
            return operation;
        }

        private static IProgress<ArchiveProgress> ArchiveProgressFor(FileOperationModel operation) =>
            new Progress<ArchiveProgress>(value => operation.Update(new FileOperationProgress
            {
                Status = "Processing archive...", Item = value.Item, Percent = value.Percent
            }));

        private FileOperationModel Create(string title, string location)
        {
            var operation = new FileOperationModel(title, location);
            Windows.Add(operation);
            Activate(operation);
            return operation;
        }

        private async Task<FileOperationResult> RunAsync(FileOperationModel operation, Func<Task<FileOperationResult>> action)
        {
            FileOperationResult result;
            try { result = await action(); }
            catch (OperationCanceledException) { result = new FileOperationResult { Cancelled = true }; }
            catch (Exception error)
            {
                result = new FileOperationResult();
                result.AddError(error.Message);
            }
            operation.Finish(result);
            operation.Cancellation.Dispose();
            WindowsChanged?.Invoke(this, EventArgs.Empty);
            OperationCompleted?.Invoke(operation);
            if (operation.CloseRequested)
                Windows.Remove(operation);
            else if (!result.Cancelled && result.Errors.Count == 0)
                _ = DismissCompletedAsync(operation);
            return result;
        }

        private async Task DismissCompletedAsync(FileOperationModel operation)
        {
            await Task.Delay(1200);
            if (!operation.IsRunning) Windows.Remove(operation);
        }

        internal void Activate(FileOperationModel operation)
        {
            if (!Windows.Contains(operation)) return;
            if (operation.Visibility != Visibility.Visible)
            {
                operation.Visibility = Visibility.Visible;
                WindowsChanged?.Invoke(this, EventArgs.Empty);
            }
            ActivationRequested?.Invoke(operation);
        }

        internal void Minimize(FileOperationModel operation)
        {
            operation.Visibility = Visibility.Collapsed;
            WindowsChanged?.Invoke(this, EventArgs.Empty);
        }

        internal void Close(FileOperationModel operation)
        {
            if (operation.IsRunning)
            {
                operation.CloseRequested = true;
                operation.Cancel();
            }
            else Windows.Remove(operation);
        }

        private static void AddAffectedPath(FileOperationModel operation, string path)
        {
            if (!string.IsNullOrEmpty(path) && !operation.AffectedFolderPaths.Contains(path))
                operation.AffectedFolderPaths.Add(path);
        }
    }
}
