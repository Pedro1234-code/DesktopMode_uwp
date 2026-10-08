using System;
using System.Collections.ObjectModel;
using System.Collections.Generic;
using System.ComponentModel;
using System.Linq;
using System.Threading.Tasks;
using factoryos_10x_shell.Services.Win32;
using Windows.Storage;
using Windows.Storage.AccessCache;
using Windows.Storage.FileProperties;
using Windows.Storage.Pickers;
using Windows.System;
using Windows.UI.Core;
using Windows.UI.Xaml;
using Windows.UI.Xaml.Controls;
using Windows.UI.Xaml.Input;
using Windows.UI.Xaml.Media;
using Windows.UI.Xaml.Media.Imaging;
using Windows.Foundation;
using factoryos_10x_shell.Services.Helpers;
using factoryos_10x_shell;
using Muxc = Microsoft.UI.Xaml.Controls;

namespace factoryos_10x_shell.Controls
{
    public sealed partial class FilesWindow : UserControl
    {
        private const string MountPrefix = "CoreShell.Files.Mount.";
        private const string RemovableMediaNameKey = "CoreShell.Files.RemovableMediaName";
        private StorageFolder m_currentFolder;
        private string m_currentDisplayName = "Home";
        private Stack<FolderLocation> m_history = new Stack<FolderLocation>();
        private Stack<FolderLocation> m_forwardHistory = new Stack<FolderLocation>();
        private FileTabState m_activeTab;
        private bool m_switchingTabs;
        private int m_tabSelectionRevision;
        private int m_folderLoadRevision;
        private StorageFolder m_clipboardFolder;
        private StorageFile m_clipboardFile;
        private bool m_cutOperation;
        private readonly ObservableCollection<FileEntry> m_items = new ObservableCollection<FileEntry>();
        private readonly List<FileEntry> m_allItems = new List<FileEntry>();
        private bool m_dragging;
        private UIElement m_dragSurface;
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
            CreateInitialTab();
        }

        private void CreateInitialTab()
        {
            m_switchingTabs = true;
            Muxc.TabViewItem tab = CreateTab("Home", ApplicationData.Current.LocalFolder);
            FilesTabs.SelectedItem = tab;
            ActivateTabState((FileTabState)tab.Tag);
            m_switchingTabs = false;
        }

        private Muxc.TabViewItem CreateTab(string title, StorageFolder folder)
        {
            var state = new FileTabState
            {
                CurrentFolder = folder,
                DisplayName = title,
                SelectedNavigationItem = HomeNavigationItem
            };
            var tab = new Muxc.TabViewItem
            {
                Header = title,
                // Do not parse path markup in code here. XamlBindingHelper.ConvertValue
                // is not reliable for Geometry on every UWP target (notably Xbox) and
                // fails with E_INVALIDARG while the window is being opened.
                IconSource = new Muxc.SymbolIconSource { Symbol = Symbol.Folder },
                Tag = state
            };
            state.TabItem = tab;
            FilesTabs.TabItems.Add(tab);
            return tab;
        }

        private void ActivateTabState(FileTabState state)
        {
            m_activeTab = state;
            m_history = state.History;
            m_forwardHistory = state.ForwardHistory;
            m_currentFolder = state.CurrentFolder;
            m_currentDisplayName = state.DisplayName;
            LocationsNavigation.SelectedItem = state.SelectedNavigationItem ?? HomeNavigationItem;
        }

        private void SaveActiveTabState()
        {
            if (m_activeTab == null) return;
            m_activeTab.CurrentFolder = m_currentFolder;
            m_activeTab.DisplayName = m_currentDisplayName;
            m_activeTab.SearchText = SearchBox?.Text ?? string.Empty;
            m_activeTab.SelectedNavigationItem = LocationsNavigation.SelectedItem;
        }

        private void FilesTabs_AddTabButtonClick(Muxc.TabView sender, object args)
        {
            Muxc.TabViewItem tab = CreateTab("Home", ApplicationData.Current.LocalFolder);
            sender.SelectedItem = tab;
        }

        private async void FilesTabs_SelectionChanged(object sender, SelectionChangedEventArgs e)
        {
            if (m_switchingTabs) return;
            Muxc.TabViewItem tab = FilesTabs.SelectedItem as Muxc.TabViewItem;
            FileTabState state = tab?.Tag as FileTabState;
            if (state == null || ReferenceEquals(state, m_activeTab)) return;

            SaveActiveTabState();
            ActivateTabState(state);
            int revision = ++m_tabSelectionRevision;
            string searchText = state.SearchText ?? string.Empty;
            await OpenFolderAsync(state.CurrentFolder ?? ApplicationData.Current.LocalFolder,
                string.IsNullOrWhiteSpace(state.DisplayName) ? "Home" : state.DisplayName, false);

            if (revision != m_tabSelectionRevision || !ReferenceEquals(state, m_activeTab)) return;
            SearchBox.Text = searchText;
            state.SearchText = searchText;
        }

        private void FilesTabs_TabCloseRequested(Muxc.TabView sender, Muxc.TabViewTabCloseRequestedEventArgs args)
        {
            CloseTab(sender, args.Tab);
        }

        private static void CloseTab(Muxc.TabView sender, Muxc.TabViewItem tab)
        {
            if (sender.TabItems.Count <= 1 || tab == null) return;

            int closedIndex = sender.TabItems.IndexOf(tab);
            bool wasSelected = ReferenceEquals(sender.SelectedItem, tab);
            sender.TabItems.Remove(tab);
            if (wasSelected && sender.TabItems.Count > 0)
                sender.SelectedIndex = Math.Max(0, Math.Min(closedIndex - 1, sender.TabItems.Count - 1));
        }

        private void FilesWindow_KeyDown(object sender, KeyRoutedEventArgs e)
        {
            CoreVirtualKeyStates control = Window.Current.CoreWindow.GetKeyState(VirtualKey.Control);
            if ((control & CoreVirtualKeyStates.Down) == 0) return;

            if (e.Key == VirtualKey.T)
            {
                FilesTabs.SelectedItem = CreateTab("Home", ApplicationData.Current.LocalFolder);
                e.Handled = true;
            }
            else if (e.Key == VirtualKey.W)
            {
                CloseTab(FilesTabs, FilesTabs.SelectedItem as Muxc.TabViewItem);
                e.Handled = true;
            }
        }

        public async void OpenHome()
        {
            Visibility = Visibility.Visible;
            AppState.Instance.SetFilesWindowState(true, false);
            AppState.Instance.ActivateFiles();
            m_history.Clear();
            m_forwardHistory.Clear();
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
            m_startLeft = Canvas.GetLeft(this); m_startTop = Canvas.GetTop(this);
            m_dragSurface = sender as UIElement;
            m_dragSurface?.CapturePointer(e.Pointer);
        }
        private void TitleBar_PointerMoved(object sender, PointerRoutedEventArgs e)
        {
            if (!m_dragging) return;
            Point point = e.GetCurrentPoint(VisualTreeHelper.GetParent(this) as UIElement).Position;
            Canvas.SetLeft(this, Math.Max(0, m_startLeft + point.X - m_startPoint.X));
            Canvas.SetTop(this, Math.Max(0, m_startTop + point.Y - m_startPoint.Y));
        }
        private void TitleBar_PointerReleased(object sender, PointerRoutedEventArgs e)
        {
            m_dragging = false;
            m_dragSurface?.ReleasePointerCaptures();
            m_dragSurface = null;
        }
        private void TitleBar_DoubleTapped(object sender, DoubleTappedRoutedEventArgs e)
        {
            e.Handled = true;
            m_dragging = false;
            m_dragSurface?.ReleasePointerCaptures();
            m_dragSurface = null;
            Maximize_Click(sender, null);
        }
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
            m_forwardHistory.Clear();
            LocationsNavigation.SelectedItem = HomeNavigationItem;
            if (m_activeTab != null)
                m_activeTab.SelectedNavigationItem = HomeNavigationItem;
            await OpenFolderAsync(ApplicationData.Current.LocalFolder, "Home", false);
        }

        private async void Library_Click(object sender, RoutedEventArgs e)
        {
            string location = (sender as FrameworkElement)?.Tag as string;
            await OpenLibraryAsync(location);
        }

        private async void LocationsNavigation_ItemInvoked(NavigationView sender, NavigationViewItemInvokedEventArgs e)
        {
            NavigationViewItem selectedItem = e.InvokedItemContainer as NavigationViewItem;
            string location = selectedItem?.Tag as string;
            if (m_activeTab != null)
                m_activeTab.SelectedNavigationItem = selectedItem;
            if (location == "Home")
            {
                m_history.Clear();
                m_forwardHistory.Clear();
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
            FileTabState targetTab = m_activeTab;
            int loadRevision = ++m_folderLoadRevision;
            if (addToHistory && m_currentFolder != null)
            {
                m_history.Push(new FolderLocation { Folder = m_currentFolder, DisplayName = m_currentDisplayName });
                m_forwardHistory.Clear();
            }
            m_currentFolder = folder;
            m_currentDisplayName = displayName;
            if (m_activeTab != null)
            {
                m_activeTab.CurrentFolder = folder;
                m_activeTab.DisplayName = displayName;
                if (m_activeTab.TabItem != null)
                    m_activeTab.TabItem.Header = displayName;
            }
            PathText.Text = displayName;
            SearchBox.Text = string.Empty;
            m_items.Clear();
            m_allItems.Clear();
            try
            {
                var loadedItems = new List<FileEntry>();
                foreach (StorageFolder child in await folder.GetFoldersAsync())
                    loadedItems.Add(new FileEntry
                    {
                        Name = child.Name,
                        Kind = "Folder",
                        IconGlyph = GetFileIconGlyph("Folder"),
                        Folder = child,
                        DateModifiedText = FormatDate(child.DateCreated)
                    });
                foreach (StorageFile child in await folder.GetFilesAsync())
                {
                    bool isExecutable = string.Equals(child.FileType, ".exe", StringComparison.OrdinalIgnoreCase);
                    BasicProperties properties = await child.GetBasicPropertiesAsync();
                    var entry = new FileEntry
                    {
                        Name = child.Name,
                        Kind = string.IsNullOrWhiteSpace(child.DisplayType) ? child.FileType : child.DisplayType,
                        SizeText = FormatSize(properties.Size),
                        DateModifiedText = FormatDate(properties.DateModified),
                        IconGlyph = GetFileIconGlyph(GetFileIconKind(child, isExecutable)),
                        File = child
                    };
                    loadedItems.Add(entry);
                }

                // A slower storage provider must not overwrite the contents after the
                // user has already changed to another tab or navigated somewhere else.
                if (loadRevision != m_folderLoadRevision || !ReferenceEquals(targetTab, m_activeTab)) return;
                m_allItems.AddRange(loadedItems);
                foreach (FileEntry entry in loadedItems.Where(item => item.File != null &&
                    string.Equals(item.File.FileType, ".exe", StringComparison.OrdinalIgnoreCase)))
                    _ = LoadExecutableThumbnailAsync(entry);
                ApplyFilter();
            }
            catch (Exception)
            {
                if (loadRevision != m_folderLoadRevision || !ReferenceEquals(targetTab, m_activeTab)) return;
                await new ContentDialog { Title = "Folder unavailable", Content = "CoreShell no longer has access to this folder. Choose it again from the sidebar.", CloseButtonText = "OK" }.ShowAsync();
            }
        }

        private async void ItemsList_DoubleTapped(object sender, DoubleTappedRoutedEventArgs e)
        {
            await OpenEntryAsync(ItemsList.SelectedItem as FileEntry);
        }

        private async void ItemsList_KeyDown(object sender, KeyRoutedEventArgs e)
        {
            if (e.Key != VirtualKey.Enter) return;
            e.Handled = true;
            await OpenEntryAsync(ItemsList.SelectedItem as FileEntry);
        }

        private async Task OpenEntryAsync(FileEntry entry)
        {
            if (entry == null) return;
            if (entry.Folder != null) await OpenFolderAsync(entry.Folder, entry.Folder.Name, true);
            else if (entry.File != null && string.Equals(entry.File.FileType, ".exe", StringComparison.OrdinalIgnoreCase))
            {
                if (entry.Thumbnail == null)
                    await LoadExecutableThumbnailAsync(entry);
                Win32WindowManagerService.Instance.Open(entry.File, m_currentFolder, entry.Thumbnail);
            }
            else if (entry.File != null && string.Equals(entry.File.FileType, ".txt", StringComparison.OrdinalIgnoreCase))
                AppState.Instance.RequestNotepadOpen(entry.File);
            else if (entry.File != null) await Launcher.LaunchFileAsync(entry.File);
        }

        private async void Back_Click(object sender, RoutedEventArgs e)
        {
            if (m_history.Count == 0) return;
            if (m_currentFolder != null)
                m_forwardHistory.Push(new FolderLocation { Folder = m_currentFolder, DisplayName = m_currentDisplayName });
            FolderLocation previous = m_history.Pop();
            await OpenFolderAsync(previous.Folder, previous.DisplayName, false);
        }

        private async void Forward_Click(object sender, RoutedEventArgs e)
        {
            if (m_forwardHistory.Count == 0) return;
            if (m_currentFolder != null)
                m_history.Push(new FolderLocation { Folder = m_currentFolder, DisplayName = m_currentDisplayName });
            FolderLocation next = m_forwardHistory.Pop();
            await OpenFolderAsync(next.Folder, next.DisplayName, false);
        }

        // The folders exposed by UWP are permission roots, so "Up" safely follows
        // the navigation path rather than attempting to access an unapproved parent.
        private void Up_Click(object sender, RoutedEventArgs e) => Back_Click(sender, e);

        private static async Task LoadExecutableThumbnailAsync(FileEntry entry)
        {
            if (entry?.File == null || entry.Thumbnail != null) return;

            try
            {
                using (StorageItemThumbnail thumbnail = await entry.File.GetThumbnailAsync(
                    // ListView asks the shell for a generic list representation.  SingleItem asks
                    // for the executable's own associated icon, which is what we need here.
                    ThumbnailMode.SingleItem,
                    64,
                    ThumbnailOptions.UseCurrentScale | ThumbnailOptions.ResizeThumbnail))
                {
                    if (thumbnail == null) return;

                    var image = new BitmapImage();
                    await image.SetSourceAsync(thumbnail);
                    entry.Thumbnail = image;
                }
            }
            catch
            {
                // Some file providers and executables have no shell thumbnail.
                // The Files template leaves the existing placeholder visible.
            }
        }

        private async void Refresh_Click(object sender, RoutedEventArgs e)
        {
            if (m_currentFolder != null)
                await OpenFolderAsync(m_currentFolder, m_currentDisplayName, false);
        }

        private void SearchBox_TextChanged(object sender, TextChangedEventArgs e)
        {
            if (m_activeTab != null)
                m_activeTab.SearchText = SearchBox.Text ?? string.Empty;
            ApplyFilter();
        }

        private void ApplyFilter()
        {
            string query = SearchBox?.Text?.Trim() ?? string.Empty;
            m_items.Clear();
            foreach (FileEntry item in m_allItems.Where(item =>
                query.Length == 0 || item.Name.IndexOf(query, StringComparison.OrdinalIgnoreCase) >= 0))
            {
                m_items.Add(item);
            }
            UpdateStatus();
        }

        private void ItemsList_SelectionChanged(object sender, SelectionChangedEventArgs e) => UpdateStatus();

        private void UpdateStatus()
        {
            if (StatusText == null) return;
            int selected = ItemsList?.SelectedItems?.Count ?? 0;
            StatusText.Text = selected > 0
                ? selected + (selected == 1 ? " item selected" : " items selected")
                : m_items.Count + (m_items.Count == 1 ? " item" : " items");
        }

        private static string FormatDate(DateTimeOffset value) =>
            value == default(DateTimeOffset) ? string.Empty : value.ToString("g");

        private static string FormatSize(ulong bytes)
        {
            string[] units = { "B", "KB", "MB", "GB", "TB" };
            double value = bytes;
            int unit = 0;
            while (value >= 1024 && unit < units.Length - 1)
            {
                value /= 1024;
                unit++;
            }
            return unit == 0 ? bytes + " B" : value.ToString(value >= 10 ? "0" : "0.0") + " " + units[unit];
        }

        private static string GetFileIconKind(StorageFile file, bool isExecutable)
        {
            if (isExecutable) return "Open";
            string extension = file?.FileType ?? string.Empty;
            if (extension.Equals(".zip", StringComparison.OrdinalIgnoreCase) ||
                extension.Equals(".7z", StringComparison.OrdinalIgnoreCase) ||
                extension.Equals(".rar", StringComparison.OrdinalIgnoreCase)) return "Zip";
            if (extension.Equals(".url", StringComparison.OrdinalIgnoreCase)) return "Url";
            if (extension.Equals(".lnk", StringComparison.OrdinalIgnoreCase)) return "Shortcut";
            return "File";
        }

        private static string GetFileIconGlyph(string kind)
        {
            switch (kind)
            {
                case "Folder": return "\uE8B7";
                case "Zip": return "\uE7B8";
                case "Url": return "\uE71B";
                case "Shortcut": return "\uE8AD";
                case "Open": return "\uE8E5";
                default: return "\uE8A5";
            }
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

        private FileEntry GetEntry(object sender) =>
            (sender as FrameworkElement)?.DataContext as FileEntry ?? ItemsList.SelectedItem as FileEntry;

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

        private sealed class FileTabState
        {
            public StorageFolder CurrentFolder { get; set; }
            public string DisplayName { get; set; } = "Home";
            public string SearchText { get; set; } = string.Empty;
            public Stack<FolderLocation> History { get; } = new Stack<FolderLocation>();
            public Stack<FolderLocation> ForwardHistory { get; } = new Stack<FolderLocation>();
            public Muxc.TabViewItem TabItem { get; set; }
            public object SelectedNavigationItem { get; set; }
        }

        private sealed class FileEntry : INotifyPropertyChanged
        {
            public string Name { get; set; }
            public string Kind { get; set; }
            public string IconGlyph { get; set; }
            public string SizeText { get; set; }
            public string DateModifiedText { get; set; }
            public StorageFolder Folder { get; set; }
            public StorageFile File { get; set; }
            private ImageSource m_thumbnail;
            public ImageSource Thumbnail
            {
                get => m_thumbnail;
                set
                {
                    if (ReferenceEquals(m_thumbnail, value)) return;
                    m_thumbnail = value;
                    PropertyChanged?.Invoke(this, new PropertyChangedEventArgs(nameof(Thumbnail)));
                    PropertyChanged?.Invoke(this, new PropertyChangedEventArgs(nameof(IconVisibility)));
                }
            }
            public Visibility IconVisibility => Thumbnail == null ? Visibility.Visible : Visibility.Collapsed;
            public event PropertyChangedEventHandler PropertyChanged;
        }
    }
}
