using System;
using System.Collections.ObjectModel;
using System.Collections.Generic;
using System.Threading.Tasks;
using factoryos_10x_shell.Services.Win32;
using Windows.Storage;
using Windows.Storage.AccessCache;
using Windows.Storage.Pickers;
using Windows.System;
using Windows.UI.Xaml;
using Windows.UI.Xaml.Controls;
using Windows.UI.Xaml.Input;
using Windows.UI.Xaml.Media;
using Windows.Foundation;
using factoryos_10x_shell.Services.Helpers;
using factoryos_10x_shell;

namespace factoryos_10x_shell.Controls
{
    public sealed partial class FilesWindow : UserControl
    {
        private const string MountPrefix = "CoreShell.Files.Mount.";
        private const string RemovableMediaNameKey = "CoreShell.Files.RemovableMediaName";
        private StorageFolder m_currentFolder;
        private string m_currentDisplayName = "Home";
        private readonly Stack<FolderLocation> m_history = new Stack<FolderLocation>();
        private StorageFolder m_clipboardFolder;
        private StorageFile m_clipboardFile;
        private bool m_cutOperation;
        private readonly ObservableCollection<FileEntry> m_items = new ObservableCollection<FileEntry>();
        private bool m_dragging;
        private bool m_resizing;
        private bool m_maximized;
        private Point m_startPoint;
        private double m_startLeft, m_startTop, m_startWidth, m_startHeight;
        private double m_restoreLeft, m_restoreTop, m_restoreWidth, m_restoreHeight;

        public FilesWindow()
        {
            InitializeComponent();
            ItemsList.ItemsSource = m_items;
            LocationsNavigation.SelectedItem = HomeNavigationItem;
            RestoreRemovableMediaName();
        }

        public async void OpenHome()
        {
            Visibility = Visibility.Visible;
            AppState.Instance.SetFilesWindowState(true, false);
            AppState.Instance.ActivateFiles();
            m_history.Clear();
            LocationsNavigation.SelectedItem = HomeNavigationItem;
            await OpenFolderAsync(ApplicationData.Current.LocalFolder, "Home", false);
        }

        public void Restore()
        {
            Visibility = Visibility.Visible;
            AppState.Instance.SetFilesWindowState(true, false);
            AppState.Instance.ActivateFiles();
        }

        public void CloseFromTaskView() => Close_Click(this, null);

        private void Close_Click(object sender, RoutedEventArgs e)
        {
            Visibility = Visibility.Collapsed;
            AppState.Instance.SetFilesWindowState(false, false);
        }
        private void Minimize_Click(object sender, RoutedEventArgs e)
        {
            Visibility = Visibility.Collapsed;
            AppState.Instance.SetFilesWindowState(true, true);
        }
        private void Maximize_Click(object sender, RoutedEventArgs e)
        {
            Canvas host = VisualTreeHelper.GetParent(this) as Canvas;
            if (host == null) return;
            if (!m_maximized)
            {
                m_restoreLeft = Canvas.GetLeft(this); m_restoreTop = Canvas.GetTop(this);
                m_restoreWidth = Width; m_restoreHeight = Height;
                Canvas.SetLeft(this, 0); Canvas.SetTop(this, 0);
                Width = host.ActualWidth; Height = host.ActualHeight;
                m_maximized = true;
            }
            else
            {
                Canvas.SetLeft(this, m_restoreLeft); Canvas.SetTop(this, m_restoreTop);
                Width = m_restoreWidth; Height = m_restoreHeight;
                m_maximized = false;
            }
        }

        private void Root_PointerPressed(object sender, PointerRoutedEventArgs e) => AppState.Instance.ActivateFiles();
        private void ContextFlyout_Opened(object sender, object e) => MainPage.SetPopupCursorVisibility(true);
        private void ContextFlyout_Closed(object sender, object e) => MainPage.SetPopupCursorVisibility(false);
        private void TitleBar_PointerPressed(object sender, PointerRoutedEventArgs e)
        {
            if (m_maximized) return;
            m_dragging = true; m_startPoint = e.GetCurrentPoint(VisualTreeHelper.GetParent(this) as UIElement).Position;
            m_startLeft = Canvas.GetLeft(this); m_startTop = Canvas.GetTop(this); TitleBar.CapturePointer(e.Pointer);
        }
        private void TitleBar_PointerMoved(object sender, PointerRoutedEventArgs e)
        {
            if (!m_dragging) return;
            Point point = e.GetCurrentPoint(VisualTreeHelper.GetParent(this) as UIElement).Position;
            Canvas.SetLeft(this, Math.Max(0, m_startLeft + point.X - m_startPoint.X));
            Canvas.SetTop(this, Math.Max(0, m_startTop + point.Y - m_startPoint.Y));
        }
        private void TitleBar_PointerReleased(object sender, PointerRoutedEventArgs e) { m_dragging = false; TitleBar.ReleasePointerCaptures(); }
        private void ResizeGrip_PointerPressed(object sender, PointerRoutedEventArgs e)
        {
            if (m_maximized) return;
            m_resizing = true; m_startPoint = e.GetCurrentPoint(this).Position;
            m_startWidth = Width; m_startHeight = Height; ResizeGrip.CapturePointer(e.Pointer);
        }
        private void ResizeGrip_PointerMoved(object sender, PointerRoutedEventArgs e)
        {
            if (!m_resizing) return;
            Point point = e.GetCurrentPoint(this).Position;
            Width = Math.Max(480, m_startWidth + point.X - m_startPoint.X);
            Height = Math.Max(320, m_startHeight + point.Y - m_startPoint.Y);
        }
        private void ResizeGrip_PointerReleased(object sender, PointerRoutedEventArgs e) { m_resizing = false; ResizeGrip.ReleasePointerCaptures(); }
        private async void Home_Click(object sender, RoutedEventArgs e)
        {
            m_history.Clear();
            LocationsNavigation.SelectedItem = HomeNavigationItem;
            await OpenFolderAsync(ApplicationData.Current.LocalFolder, "Home", false);
        }

        private async void Library_Click(object sender, RoutedEventArgs e)
        {
            string location = (sender as FrameworkElement)?.Tag as string;
            await OpenLibraryAsync(location);
        }

        private async void LocationsNavigation_ItemInvoked(NavigationView sender, NavigationViewItemInvokedEventArgs e)
        {
            string location = (e.InvokedItemContainer as NavigationViewItem)?.Tag as string;
            if (location == "Home")
            {
                m_history.Clear();
                await OpenFolderAsync(ApplicationData.Current.LocalFolder, "Home", false);
                return;
            }
            await OpenLibraryAsync(location);
        }

        private async Task OpenLibraryAsync(string location)
        {
            if (string.IsNullOrWhiteSpace(location)) return;

            StorageFolder folder = await GetMountedFolderAsync(location);
            if (folder == null)
            {
                string pickerInstructions = location == "Removable media"
                    ? "Choose the root of the removable drive. CoreShell will show its drive letter and label here."
                    : "To use " + location + ", choose the folder you want CoreShell Files to access. This permission is saved for future use.";
                var dialog = new ContentDialog
                {
                    Title = "Choose " + location,
                    Content = pickerInstructions,
                    PrimaryButtonText = "Choose folder",
                    CloseButtonText = "Cancel"
                };
                if (await dialog.ShowAsync() != ContentDialogResult.Primary) return;

                var picker = new FolderPicker();
                picker.FileTypeFilter.Add("*");
                folder = await picker.PickSingleFolderAsync();
                if (folder == null) return;
                StorageApplicationPermissions.FutureAccessList.AddOrReplace(MountPrefix + location, folder);
                if (location == "Removable media") SaveRemovableMediaName(folder);
            }
            await OpenFolderAsync(folder, GetLocationDisplayName(location, folder), true);
        }

        private async Task<StorageFolder> GetMountedFolderAsync(string location)
        {
            string token = MountPrefix + location;
            if (!StorageApplicationPermissions.FutureAccessList.ContainsItem(token)) return null;
            try { return await StorageApplicationPermissions.FutureAccessList.GetFolderAsync(token); }
            catch { StorageApplicationPermissions.FutureAccessList.Remove(token); return null; }
        }

        private async Task OpenFolderAsync(StorageFolder folder, string displayName, bool addToHistory = false)
        {
            if (folder == null) return;
            if (addToHistory && m_currentFolder != null)
                m_history.Push(new FolderLocation { Folder = m_currentFolder, DisplayName = m_currentDisplayName });
            m_currentFolder = folder;
            m_currentDisplayName = displayName;
            PathText.Text = displayName;
            m_items.Clear();
            try
            {
                foreach (StorageFolder child in await folder.GetFoldersAsync())
                    m_items.Add(new FileEntry { Name = child.Name, Kind = "Folder", Glyph = "\uE8B7", Folder = child });
                foreach (StorageFile child in await folder.GetFilesAsync())
                    m_items.Add(new FileEntry { Name = child.Name, Kind = child.FileType, Glyph = "\uE8A5", File = child });
            }
            catch (Exception)
            {
                await new ContentDialog { Title = "Folder unavailable", Content = "CoreShell no longer has access to this folder. Choose it again from the sidebar.", CloseButtonText = "OK" }.ShowAsync();
            }
        }

        private async void ItemsList_ItemClick(object sender, ItemClickEventArgs e)
        {
            if (!(e.ClickedItem is FileEntry entry)) return;
            if (entry.Folder != null) await OpenFolderAsync(entry.Folder, entry.Folder.Name, true);
            else if (entry.File != null && string.Equals(entry.File.FileType, ".exe", StringComparison.OrdinalIgnoreCase))
                Win32WindowManagerService.Instance.Open(entry.File, m_currentFolder);
            else if (entry.File != null && string.Equals(entry.File.FileType, ".txt", StringComparison.OrdinalIgnoreCase))
                AppState.Instance.RequestNotepadOpen(entry.File);
            else if (entry.File != null) await Launcher.LaunchFileAsync(entry.File);
        }

        private async void Back_Click(object sender, RoutedEventArgs e)
        {
            if (m_history.Count == 0) return;
            FolderLocation previous = m_history.Pop();
            await OpenFolderAsync(previous.Folder, previous.DisplayName, false);
        }

        // The folders exposed by UWP are permission roots, so "Up" safely follows
        // the navigation path rather than attempting to access an unapproved parent.
        private void Up_Click(object sender, RoutedEventArgs e) => Back_Click(sender, e);

        private async void Refresh_Click(object sender, RoutedEventArgs e)
        {
            if (m_currentFolder != null)
                await OpenFolderAsync(m_currentFolder, m_currentDisplayName, false);
        }

        private async void NewFolder_Click(object sender, RoutedEventArgs e)
        {
            if (m_currentFolder == null) return;
            var nameBox = new TextBox { PlaceholderText = "Folder name" };
            var dialog = new ContentDialog
            {
                Title = "New folder", Content = nameBox,
                PrimaryButtonText = "Create", CloseButtonText = "Cancel"
            };
            if (await dialog.ShowAsync() != ContentDialogResult.Primary || string.IsNullOrWhiteSpace(nameBox.Text)) return;
            try
            {
                await m_currentFolder.CreateFolderAsync(nameBox.Text.Trim(), CreationCollisionOption.GenerateUniqueName);
                await OpenFolderAsync(m_currentFolder, m_currentDisplayName, false);
            }
            catch (Exception) { await ShowOperationErrorAsync("CoreShell could not create that folder."); }
        }

        private void Copy_Click(object sender, RoutedEventArgs e) => SetClipboard(GetEntry(sender), false);
        private void Cut_Click(object sender, RoutedEventArgs e) => SetClipboard(GetEntry(sender), true);

        private void SetClipboard(FileEntry entry, bool cut)
        {
            if (entry == null) return;
            m_clipboardFolder = entry.Folder;
            m_clipboardFile = entry.File;
            m_cutOperation = cut;
        }

        private async void Paste_Click(object sender, RoutedEventArgs e)
        {
            if (m_currentFolder == null || (m_clipboardFolder == null && m_clipboardFile == null)) return;
            try
            {
                if (m_clipboardFolder != null)
                {
                    if (m_clipboardFolder.Path == m_currentFolder.Path) return;
                    if (m_cutOperation)
                        await MoveFolderAsync(m_clipboardFolder, m_currentFolder);
                    else
                        await CopyFolderAsync(m_clipboardFolder, m_currentFolder);
                }
                else if (m_clipboardFile != null)
                {
                    if (m_cutOperation)
                        await m_clipboardFile.MoveAsync(m_currentFolder, m_clipboardFile.Name, NameCollisionOption.GenerateUniqueName);
                    else
                        await m_clipboardFile.CopyAsync(m_currentFolder, m_clipboardFile.Name, NameCollisionOption.GenerateUniqueName);
                }
                if (m_cutOperation) { m_clipboardFolder = null; m_clipboardFile = null; }
                await OpenFolderAsync(m_currentFolder, m_currentDisplayName, false);
            }
            catch (Exception) { await ShowOperationErrorAsync("CoreShell could not paste this item here."); }
        }

        private async void Delete_Click(object sender, RoutedEventArgs e)
        {
            FileEntry entry = GetEntry(sender);
            if (entry == null) return;
            var dialog = new ContentDialog { Title = "Delete", Content = "Delete " + entry.Name + "?", PrimaryButtonText = "Delete", CloseButtonText = "Cancel" };
            if (await dialog.ShowAsync() != ContentDialogResult.Primary) return;
            try
            {
                if (entry.Folder != null) await entry.Folder.DeleteAsync();
                else if (entry.File != null) await entry.File.DeleteAsync();
                await OpenFolderAsync(m_currentFolder, m_currentDisplayName, false);
            }
            catch (Exception) { await ShowOperationErrorAsync("CoreShell could not delete this item."); }
        }

        private async void MoveTo_Click(object sender, RoutedEventArgs e)
        {
            FileEntry entry = GetEntry(sender);
            if (entry == null) return;
            var picker = new FolderPicker();
            picker.FileTypeFilter.Add("*");
            StorageFolder destination = await picker.PickSingleFolderAsync();
            if (destination == null) return;
            try
            {
                if (entry.Folder != null)
                    await MoveFolderAsync(entry.Folder, destination);
                else if (entry.File != null)
                    await entry.File.MoveAsync(destination, entry.File.Name, NameCollisionOption.GenerateUniqueName);
                await OpenFolderAsync(m_currentFolder, m_currentDisplayName, false);
            }
            catch (Exception) { await ShowOperationErrorAsync("CoreShell could not move this item."); }
        }

        private async void Rename_Click(object sender, RoutedEventArgs e)
        {
            FileEntry entry = GetEntry(sender);
            if (entry == null) return;
            var nameBox = new TextBox { Text = entry.Name };
            var dialog = new ContentDialog
            {
                Title = "Rename", Content = nameBox,
                PrimaryButtonText = "Rename", CloseButtonText = "Cancel"
            };
            if (await dialog.ShowAsync() != ContentDialogResult.Primary || string.IsNullOrWhiteSpace(nameBox.Text)) return;
            try
            {
                if (entry.Folder != null) await entry.Folder.RenameAsync(nameBox.Text.Trim(), NameCollisionOption.GenerateUniqueName);
                else if (entry.File != null) await entry.File.RenameAsync(nameBox.Text.Trim(), NameCollisionOption.GenerateUniqueName);
                await OpenFolderAsync(m_currentFolder, m_currentDisplayName, false);
            }
            catch (Exception) { await ShowOperationErrorAsync("CoreShell could not rename this item."); }
        }

        private static FileEntry GetEntry(object sender) => (sender as FrameworkElement)?.DataContext as FileEntry;

        private string GetLocationDisplayName(string location, StorageFolder folder)
        {
            return location == "Removable media" ? GetRemovableMediaName(folder) : location;
        }

        private void RestoreRemovableMediaName()
        {
            object savedName;
            if (ApplicationData.Current.LocalSettings.Values.TryGetValue(RemovableMediaNameKey, out savedName))
                RemovableMediaItem.Content = savedName as string ?? "Removable media";
        }

        private void SaveRemovableMediaName(StorageFolder folder)
        {
            string name = GetRemovableMediaName(folder);
            ApplicationData.Current.LocalSettings.Values[RemovableMediaNameKey] = name;
            RemovableMediaItem.Content = name;
        }

        private static string GetRemovableMediaName(StorageFolder folder)
        {
            string path = folder?.Path ?? string.Empty;
            string root = System.IO.Path.GetPathRoot(path);
            string drive = string.IsNullOrWhiteSpace(root) ? "Removable media" : root.TrimEnd('\\');
            string label = string.IsNullOrWhiteSpace(folder?.DisplayName) ? folder?.Name : folder.DisplayName;
            return string.IsNullOrWhiteSpace(label) ? drive : drive + " - " + label;
        }

        // StorageFolder does not expose CopyAsync/MoveAsync on the UWP contract used
        // by this Xbox target. Recreate the folder tree using APIs that are available.
        private static async Task<StorageFolder> CopyFolderAsync(StorageFolder source, StorageFolder destination)
        {
            StorageFolder copy = await destination.CreateFolderAsync(source.Name, CreationCollisionOption.GenerateUniqueName);
            foreach (StorageFile file in await source.GetFilesAsync())
                await file.CopyAsync(copy, file.Name, NameCollisionOption.GenerateUniqueName);
            foreach (StorageFolder child in await source.GetFoldersAsync())
                await CopyFolderAsync(child, copy);
            return copy;
        }

        private static async Task MoveFolderAsync(StorageFolder source, StorageFolder destination)
        {
            await CopyFolderAsync(source, destination);
            await source.DeleteAsync();
        }

        private async Task ShowOperationErrorAsync(string message)
        {
            await new ContentDialog { Title = "Files", Content = message, CloseButtonText = "OK" }.ShowAsync();
        }

        private sealed class FolderLocation
        {
            public StorageFolder Folder { get; set; }
            public string DisplayName { get; set; }
        }

        private sealed class FileEntry
        {
            public string Name { get; set; }
            public string Kind { get; set; }
            public string Glyph { get; set; }
            public StorageFolder Folder { get; set; }
            public StorageFile File { get; set; }
        }
    }
}
