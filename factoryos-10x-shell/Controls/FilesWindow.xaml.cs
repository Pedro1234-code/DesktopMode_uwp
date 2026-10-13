using System;
using System.Collections.ObjectModel;
using System.Collections.Generic;
using System.ComponentModel;
using System.Linq;
using System.Threading;
using System.Threading.Tasks;
using factoryos_10x_shell.Services.Files;
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
        private readonly List<FileEntry> m_clipboardEntries = new List<FileEntry>();
        private bool m_cutOperation;
        private FileEntry m_contextEntry;
        private readonly ObservableCollection<FileEntry> m_items = new ObservableCollection<FileEntry>();
        private readonly List<FileEntry> m_allItems = new List<FileEntry>();
        private bool m_dragging;
        private UIElement m_dragSurface;
        private bool m_resizing;
        private bool m_maximized;
        private WindowResizeCorner m_resizeCorner;
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
                IconSource = new Muxc.BitmapIconSource
                {
                    ShowAsMonochrome = false,
                    UriSource = new Uri("ms-appx:///Assets/Files/Windows11Icons/folders/folder.ico")
                },
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
            bool wasShowingSettings = SettingsPage.Visibility == Visibility.Visible;
            SettingsPage.Visibility = Visibility.Collapsed;
            Muxc.TabViewItem tab = FilesTabs.SelectedItem as Muxc.TabViewItem;
            FileTabState state = tab?.Tag as FileTabState;
            if (state == null || ReferenceEquals(state, m_activeTab)) return;

            if (!wasShowingSettings)
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
            SettingsPage.Visibility = Visibility.Collapsed;
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

        public async void OpenFolderAndSelect(string filePath)
        {
            if (string.IsNullOrWhiteSpace(filePath)) return;

            Visibility = Visibility.Visible;
            SettingsPage.Visibility = Visibility.Collapsed;
            AppState.Instance.SetFilesWindowState(true, false);
            AppState.Instance.ActivateFiles();

            try
            {
                StorageFile file = await StorageFile.GetFileFromPathAsync(filePath);
                StorageFolder folder = await file.GetParentAsync();
                await OpenFolderAsync(folder, folder.DisplayName, true);
                SelectFile(file.Path, file.Name);
                return;
            }
            catch { }

            // Downloads on Xbox are normally accessible through the same
            // FutureAccessList grant used by Gecko's FolderPicker. Resolve the
            // file from that grant when direct path activation is restricted.
            try
            {
                const string downloadToken = "Gecko.Downloads.v1";
                if (!StorageApplicationPermissions.FutureAccessList.ContainsItem(downloadToken)) return;
                StorageFolder folder = await StorageApplicationPermissions.FutureAccessList.GetFolderAsync(downloadToken);
                string name = System.IO.Path.GetFileName(filePath);
                StorageFile file = await folder.GetFileAsync(name);
                await OpenFolderAsync(folder, folder.DisplayName, true);
                SelectFile(file.Path, file.Name);
            }
            catch { }
        }

        private void SelectFile(string path, string name)
        {
            FileEntry entry = m_items.FirstOrDefault(item => item.File != null &&
                ((!string.IsNullOrEmpty(path) && string.Equals(item.File.Path, path, StringComparison.OrdinalIgnoreCase)) ||
                 string.Equals(item.File.Name, name, StringComparison.OrdinalIgnoreCase)));
            if (entry == null) return;
            ItemsList.SelectedItems.Clear();
            ItemsList.SelectedItem = entry;
            ItemsList.ScrollIntoView(entry);
            ItemsList.Focus(FocusState.Programmatic);
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
        private void ContextFlyout_Closed(object sender, object e)
        {
            m_contextEntry = null;
            MainPage.SetPopupCursorVisibility(false);
        }

        private void FileItem_RightTapped(object sender, RightTappedRoutedEventArgs e)
        {
            FrameworkElement element = sender as FrameworkElement;
            FileEntry entry = element?.DataContext as FileEntry;
            if (entry == null)
                return;

            // Bind this invocation to the item under the pointer instead of relying
            // on a possibly stale ListView selection.
            m_contextEntry = entry;
            if (!ItemsList.SelectedItems.Contains(entry))
            {
                ItemsList.SelectedItems.Clear();
                ItemsList.SelectedItem = entry;
            }

            MenuFlyout flyout = element.Resources["ItemContextMenu"] as MenuFlyout;
            MenuFlyoutSubItem archiveMenu = flyout?.Items
                .OfType<MenuFlyoutSubItem>()
                .FirstOrDefault(item => item.Text == "Archive");
            if (archiveMenu != null)
                archiveMenu.Visibility = ArchiveService.IsArchive(entry.File)
                    ? Visibility.Visible
                    : Visibility.Collapsed;
            flyout?.ShowAt(element, e.GetPosition(element));
            e.Handled = true;
        }

        private void ItemsList_RightTapped(object sender, RightTappedRoutedEventArgs e)
        {
            // Item right-clicks are handled above. Reaching here means that the
            // pointer is on the list background, so never reuse an old item.
            m_contextEntry = null;
            ItemsList.SelectedItems.Clear();

            ListView list = sender as ListView;
            MenuFlyout flyout = list?.Resources["BackgroundContextMenu"] as MenuFlyout;
            flyout?.ShowAt(list, e.GetPosition(list));
            e.Handled = true;
        }

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
            m_resizing = true;
            m_resizeCorner = WindowResize.CornerFromTag((sender as FrameworkElement)?.Tag);
            m_startPoint = e.GetCurrentPoint(null).Position;
            m_startLeft = Canvas.GetLeft(this); m_startTop = Canvas.GetTop(this);
            m_startWidth = Width; m_startHeight = Height;
            (sender as UIElement)?.CapturePointer(e.Pointer);
        }
        private void ResizeGrip_PointerMoved(object sender, PointerRoutedEventArgs e)
        {
            if (!m_resizing) return;
            Rect bounds = WindowResize.Calculate(m_resizeCorner, m_startPoint,
                e.GetCurrentPoint(null).Position,
                new Rect(m_startLeft, m_startTop, m_startWidth, m_startHeight), 480, 320);
            Canvas.SetLeft(this, bounds.X); Canvas.SetTop(this, bounds.Y);
            Width = bounds.Width; Height = bounds.Height;
        }
        private void ResizeGrip_PointerReleased(object sender, PointerRoutedEventArgs e) { m_resizing = false; (sender as UIElement)?.ReleasePointerCaptures(); }
        private async void Home_Click(object sender, RoutedEventArgs e)
        {
            SettingsPage.Visibility = Visibility.Collapsed;
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
            if (e.IsSettingsInvoked)
            {
                SettingsPage.Visibility = Visibility.Visible;
                return;
            }

            SettingsPage.Visibility = Visibility.Collapsed;
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

        private async void ResetAuthorizedFolders_Click(object sender, RoutedEventArgs e)
        {
            var accessList = StorageApplicationPermissions.FutureAccessList;
            List<string> filesTokens = accessList.Entries
                .Where(entry => entry.Token.StartsWith(MountPrefix, StringComparison.Ordinal))
                .Select(entry => entry.Token)
                .ToList();

            foreach (string token in filesTokens)
                accessList.Remove(token);

            ApplicationData.Current.LocalSettings.Values.Remove(RemovableMediaNameKey);
            RemovableMediaItem.Content = "Removable media";
            m_clipboardEntries.Clear();
            m_cutOperation = false;

            foreach (Muxc.TabViewItem tab in FilesTabs.TabItems.OfType<Muxc.TabViewItem>())
            {
                FileTabState state = tab.Tag as FileTabState;
                if (state == null) continue;

                state.CurrentFolder = ApplicationData.Current.LocalFolder;
                state.DisplayName = "Home";
                state.SearchText = string.Empty;
                state.SelectedNavigationItem = HomeNavigationItem;
                state.History.Clear();
                state.ForwardHistory.Clear();
                tab.Header = "Home";
            }

            m_currentFolder = ApplicationData.Current.LocalFolder;
            m_currentDisplayName = "Home";
            m_history.Clear();
            m_forwardHistory.Clear();
            await OpenFolderAsync(ApplicationData.Current.LocalFolder, "Home", false);

            ResetAuthorizedFoldersStatus.Text = filesTokens.Count == 0
                ? "There were no authorized folders to reset."
                : filesTokens.Count == 1
                    ? "1 authorized folder was reset."
                    : filesTokens.Count + " authorized folders were reset.";
            ResetAuthorizedFoldersStatus.Visibility = Visibility.Visible;
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
                        FallbackIcon = CreatePackagedIcon("folders/folder.ico"),
                        Folder = child,
                        DateModifiedText = FormatDate(child.DateCreated)
                    });
                foreach (StorageFile child in await folder.GetFilesAsync())
                {
                    BasicProperties properties = await child.GetBasicPropertiesAsync();
                    var entry = new FileEntry
                    {
                        Name = child.Name,
                        Kind = string.IsNullOrWhiteSpace(child.DisplayType) ? child.FileType : child.DisplayType,
                        SizeText = FormatSize(properties.Size),
                        DateModifiedText = FormatDate(properties.DateModified),
                        FallbackIcon = CreatePackagedIcon(GetFileIconPath(child)),
                        File = child
                    };
                    loadedItems.Add(entry);
                }

                // A slower storage provider must not overwrite the contents after the
                // user has already changed to another tab or navigated somewhere else.
                if (loadRevision != m_folderLoadRevision || !ReferenceEquals(targetTab, m_activeTab)) return;
                m_allItems.AddRange(loadedItems);
                foreach (FileEntry entry in loadedItems.Where(item => item.File != null &&
                    !string.Equals(item.File.FileType, ".exe", StringComparison.OrdinalIgnoreCase)))
                    _ = LoadItemThumbnailAsync(entry);
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
                // Executables always use the packaged Windows icon. Do not ask
                // the storage provider for an executable thumbnail.
                Win32WindowManagerService.Instance.Open(entry.File, m_currentFolder);
            }
            else if (entry.File != null && string.Equals(entry.File.FileType, ".txt", StringComparison.OrdinalIgnoreCase))
                AppState.Instance.RequestNotepadOpen(entry.File);
            else if (entry.File != null && string.Equals(entry.File.FileType, ".pdf", StringComparison.OrdinalIgnoreCase))
                AppState.Instance.RequestFirefoxOpen(entry.File);
            else if (entry.File != null && ArchiveService.IsArchive(entry.File))
                await OpenArchivePreviewAsync(entry.File);
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

        private static async Task LoadItemThumbnailAsync(FileEntry entry)
        {
            if (entry?.File == null || entry.Thumbnail != null) return;

            try
            {
                using (StorageItemThumbnail thumbnail = await entry.File.GetThumbnailAsync(
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
                // Some providers do not expose thumbnails. Keep the packaged Windows
                // 11 icon as the fallback in that case.
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

        private static string GetFileIconPath(StorageFile file)
        {
            string extension = (file?.FileType ?? string.Empty).ToLowerInvariant();
            if (extension == ".exe") return "files/windowprogram.ico";
            if (extension.Equals(".zip", StringComparison.OrdinalIgnoreCase) ||
                extension.Equals(".7z", StringComparison.OrdinalIgnoreCase) ||
                extension.Equals(".rar", StringComparison.OrdinalIgnoreCase)) return "folders/zip.ico";
            if (extension == ".png" || extension == ".jpg" || extension == ".jpeg" ||
                extension == ".gif" || extension == ".bmp" || extension == ".webp") return "files/image.ico";
            if (extension == ".mp4" || extension == ".mkv" || extension == ".avi" ||
                extension == ".mov" || extension == ".wmv") return "files/video.ico";
            if (extension == ".mp3" || extension == ".wav" || extension == ".flac" ||
                extension == ".m4a" || extension == ".wma") return "files/audio.ico";
            if (extension == ".doc" || extension == ".docx" || extension == ".rtf")
                return "files/office/worddocument.ico";
            if (extension == ".xls" || extension == ".xlsx" || extension == ".csv")
                return "files/office/excelsheet.ico";
            if (extension == ".ppt" || extension == ".pptx")
                return "files/office/powerpointpresent.ico";
            if (extension == ".txt" || extension == ".log") return "files/text.ico";
            if (extension == ".ini" || extension == ".config") return "files/configuration.ico";
            if (extension == ".url" || extension == ".htm" || extension == ".html") return "objects/globe.ico";
            if (extension == ".lnk") return "files/window.ico";
            return "files/generic.ico";
        }

        private static BitmapImage CreatePackagedIcon(string relativePath)
        {
            return new BitmapImage(new Uri("ms-appx:///Assets/Files/Windows11Icons/" + relativePath));
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

        private void Copy_Click(object sender, RoutedEventArgs e) => SetClipboard(sender, false);
        private void Cut_Click(object sender, RoutedEventArgs e) => SetClipboard(sender, true);

        private void SetClipboard(object sender, bool cut)
        {
            IReadOnlyList<FileEntry> entries = GetOperationEntries(sender);
            if (entries.Count == 0) return;
            m_clipboardEntries.Clear();
            m_clipboardEntries.AddRange(entries);
            m_cutOperation = cut;
        }

        private async void Paste_Click(object sender, RoutedEventArgs e)
        {
            if (m_currentFolder == null || m_clipboardEntries.Count == 0) return;

            var failed = new List<FileEntry>();
            foreach (FileEntry entry in m_clipboardEntries.ToList())
            {
                try
                {
                    if (entry.Folder != null)
                    {
                        if (entry.Folder.Path == m_currentFolder.Path)
                        {
                            failed.Add(entry);
                            continue;
                        }
                        if (m_cutOperation)
                            await MoveFolderAsync(entry.Folder, m_currentFolder);
                        else
                            await CopyFolderAsync(entry.Folder, m_currentFolder);
                    }
                    else if (entry.File != null)
                    {
                        if (m_cutOperation)
                            await entry.File.MoveAsync(m_currentFolder, entry.File.Name, NameCollisionOption.GenerateUniqueName);
                        else
                            await entry.File.CopyAsync(m_currentFolder, entry.File.Name, NameCollisionOption.GenerateUniqueName);
                    }
                }
                catch (Exception)
                {
                    failed.Add(entry);
                }
            }

            if (m_cutOperation)
            {
                m_clipboardEntries.Clear();
                m_clipboardEntries.AddRange(failed);
            }
            await OpenFolderAsync(m_currentFolder, m_currentDisplayName, false);
            if (failed.Count > 0)
                await ShowOperationErrorAsync("CoreShell could not paste " + failed.Count + (failed.Count == 1 ? " item here." : " items here."));
        }

        private async void Delete_Click(object sender, RoutedEventArgs e)
        {
            IReadOnlyList<FileEntry> entries = GetOperationEntries(sender);
            if (entries.Count == 0) return;
            string prompt = entries.Count == 1 ? "Delete " + entries[0].Name + "?" : "Delete these " + entries.Count + " items?";
            var dialog = new ContentDialog { Title = "Delete", Content = prompt, PrimaryButtonText = "Delete", CloseButtonText = "Cancel" };
            if (await dialog.ShowAsync() != ContentDialogResult.Primary) return;
            int failed = 0;
            foreach (FileEntry entry in entries)
            {
                try
                {
                    if (entry.Folder != null) await entry.Folder.DeleteAsync();
                    else if (entry.File != null) await entry.File.DeleteAsync();
                }
                catch (Exception) { failed++; }
            }
            await OpenFolderAsync(m_currentFolder, m_currentDisplayName, false);
            if (failed > 0)
                await ShowOperationErrorAsync("CoreShell could not delete " + failed + (failed == 1 ? " item." : " items."));
        }

        private async void MoveTo_Click(object sender, RoutedEventArgs e)
        {
            IReadOnlyList<FileEntry> entries = GetOperationEntries(sender);
            if (entries.Count == 0) return;
            var picker = new FolderPicker();
            picker.FileTypeFilter.Add("*");
            StorageFolder destination = await picker.PickSingleFolderAsync();
            if (destination == null) return;
            int failed = 0;
            foreach (FileEntry entry in entries)
            {
                try
                {
                    if (entry.Folder != null)
                        await MoveFolderAsync(entry.Folder, destination);
                    else if (entry.File != null)
                        await entry.File.MoveAsync(destination, entry.File.Name, NameCollisionOption.GenerateUniqueName);
                }
                catch (Exception) { failed++; }
            }
            await OpenFolderAsync(m_currentFolder, m_currentDisplayName, false);
            if (failed > 0)
                await ShowOperationErrorAsync("CoreShell could not move " + failed + (failed == 1 ? " item." : " items."));
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

        private async void ArchiveExtractHere_Click(object sender, RoutedEventArgs e)
        {
            FileEntry entry = GetEntry(sender);
            if (entry?.File == null || m_currentFolder == null) return;
            await ExtractArchiveWithPasswordAsync(entry.File, m_currentFolder);
        }

        private async void ArchiveExtractTo_Click(object sender, RoutedEventArgs e)
        {
            FileEntry entry = GetEntry(sender);
            if (entry?.File == null) return;
            var picker = new FolderPicker { SuggestedStartLocation = PickerLocationId.Downloads };
            picker.FileTypeFilter.Add("*");
            StorageFolder destination = await picker.PickSingleFolderAsync();
            if (destination != null) await ExtractArchiveWithPasswordAsync(entry.File, destination);
        }

        private async void ArchiveOpen_Click(object sender, RoutedEventArgs e)
        {
            FileEntry entry = GetEntry(sender);
            if (entry?.File != null) await OpenArchivePreviewAsync(entry.File);
        }

        private async void CompressZip_Click(object sender, RoutedEventArgs e) =>
            await CompressEntryAsync(GetEntry(sender), false);

        private async void CompressSevenZip_Click(object sender, RoutedEventArgs e) =>
            await CompressEntryAsync(GetEntry(sender), true);

        private async Task ExtractArchiveWithPasswordAsync(StorageFile archive, StorageFolder destination)
        {
            string password = null;
            while (true)
            {
                try
                {
                    await RunArchiveProgressAsync("Extracting " + archive.Name,
                        (progress, token) => ArchiveService.ExtractAsync(archive, destination, password, progress, token));
                    if (ReferenceEquals(destination, m_currentFolder) || destination.Path == m_currentFolder?.Path)
                        await OpenFolderAsync(m_currentFolder, m_currentDisplayName, false);
                    return;
                }
                catch (ArchivePasswordRequiredException)
                {
                    password = await RequestArchivePasswordAsync(archive.Name);
                    if (password == null) return;
                }
                catch (OperationCanceledException) { return; }
                catch (Exception error)
                {
                    await ShowOperationErrorAsync("The archive could not be extracted. " + error.Message);
                    return;
                }
            }
        }

        private async Task OpenArchivePreviewAsync(StorageFile archive)
        {
            string password = null;
            while (true)
            {
                try
                {
                    IReadOnlyList<ArchiveEntryInfo> entries = await ArchiveService.ListAsync(archive, password);
                    var list = new ListView
                    {
                        ItemsSource = entries,
                        DisplayMemberPath = nameof(ArchiveEntryInfo.DisplayText),
                        SelectionMode = ListViewSelectionMode.Single,
                        MinWidth = 520,
                        MaxHeight = 430
                    };
                    var content = new StackPanel { Spacing = 8 };
                    content.Children.Add(new TextBlock
                    {
                        Text = entries.Count + (entries.Count == 1 ? " item" : " items"),
                        Opacity = 0.7
                    });
                    content.Children.Add(list);
                    ContentDialogResult result = await new ContentDialog
                    {
                        Title = archive.Name,
                        Content = content,
                        PrimaryButtonText = "Extract all...",
                        CloseButtonText = "Close",
                        DefaultButton = ContentDialogButton.Close
                    }.ShowAsync();
                    if (result == ContentDialogResult.Primary)
                    {
                        var picker = new FolderPicker { SuggestedStartLocation = PickerLocationId.Downloads };
                        picker.FileTypeFilter.Add("*");
                        StorageFolder folder = await picker.PickSingleFolderAsync();
                        if (folder != null) await ExtractArchiveWithPasswordAsync(archive, folder);
                    }
                    return;
                }
                catch (ArchivePasswordRequiredException)
                {
                    password = await RequestArchivePasswordAsync(archive.Name);
                    if (password == null) return;
                }
                catch (Exception error)
                {
                    await ShowOperationErrorAsync("The archive could not be opened. " + error.Message);
                    return;
                }
            }
        }

        private async Task CompressEntryAsync(FileEntry entry, bool sevenZip)
        {
            if (entry == null || m_currentFolder == null) return;
            var selectedEntries = ItemsList.SelectedItems.Cast<FileEntry>().ToList();
            if (!selectedEntries.Contains(entry)) selectedEntries = new List<FileEntry> { entry };
            var items = selectedEntries
                .Select(item => (IStorageItem)item.File ?? item.Folder)
                .Where(item => item != null)
                .ToList();
            if (items.Count == 0) return;
            string extension = sevenZip ? ".7z" : ".zip";
            string baseName = selectedEntries.Count == 1
                ? System.IO.Path.GetFileNameWithoutExtension(entry.Name)
                : "Archive";
            StorageFile output = await m_currentFolder.CreateFileAsync(baseName + extension,
                CreationCollisionOption.GenerateUniqueName);
            try
            {
                await RunArchiveProgressAsync("Creating " + output.Name,
                    (progress, token) => ArchiveService.CompressAsync(
                        items, output, sevenZip, progress, token));
                await OpenFolderAsync(m_currentFolder, m_currentDisplayName, false);
            }
            catch (OperationCanceledException)
            {
                try { await output.DeleteAsync(); } catch { }
            }
            catch (Exception error)
            {
                try { await output.DeleteAsync(); } catch { }
                await ShowOperationErrorAsync("The archive could not be created. " + error.Message);
            }
        }

        private async Task RunArchiveProgressAsync(string title,
            Func<IProgress<ArchiveProgress>, CancellationToken, Task> operation)
        {
            var status = new TextBlock { Text = "Preparing...", TextTrimming = TextTrimming.CharacterEllipsis };
            var bar = new ProgressBar { Minimum = 0, Maximum = 100, IsIndeterminate = true };
            var panel = new StackPanel { Width = 420, Spacing = 12 };
            panel.Children.Add(status);
            panel.Children.Add(bar);
            var dialog = new ContentDialog { Title = title, Content = panel, CloseButtonText = "Cancel" };
            var cancellation = new CancellationTokenSource();
            dialog.CloseButtonClick += (s, args) =>
            {
                args.Cancel = true;
                status.Text = "Cancelling...";
                cancellation.Cancel();
            };
            IProgress<ArchiveProgress> progress = new Progress<ArchiveProgress>(value =>
            {
                status.Text = value.Item ?? string.Empty;
                bar.IsIndeterminate = value.Percent < 0;
                if (!bar.IsIndeterminate)
                    bar.Value = Math.Max(0, Math.Min(100, value.Percent));
            });

            IAsyncOperation<ContentDialogResult> shown = dialog.ShowAsync();
            await Task.Yield();
            try { await operation(progress, cancellation.Token); }
            finally
            {
                if (shown.Status == AsyncStatus.Started) dialog.Hide();
                try { await shown; } catch { }
                cancellation.Dispose();
            }
        }

        private static async Task<string> RequestArchivePasswordAsync(string archiveName)
        {
            var password = new PasswordBox { PlaceholderText = "Password", MinWidth = 320 };
            var dialog = new ContentDialog
            {
                Title = "Password required",
                Content = new StackPanel
                {
                    Spacing = 10,
                    Children =
                    {
                        new TextBlock { Text = archiveName + " is encrypted. Enter its password.", TextWrapping = TextWrapping.Wrap },
                        password
                    }
                },
                PrimaryButtonText = "Continue",
                CloseButtonText = "Cancel",
                DefaultButton = ContentDialogButton.Primary
            };
            return await dialog.ShowAsync() == ContentDialogResult.Primary ? password.Password : null;
        }

        private FileEntry GetEntry(object sender)
        {
            FileEntry senderEntry = (sender as FrameworkElement)?.DataContext as FileEntry;
            return m_contextEntry ?? senderEntry ?? ItemsList.SelectedItem as FileEntry;
        }

        private IReadOnlyList<FileEntry> GetOperationEntries(object sender)
        {
            FileEntry target = GetEntry(sender);
            if (target == null)
                return Array.Empty<FileEntry>();

            List<FileEntry> selected = ItemsList.SelectedItems
                .OfType<FileEntry>()
                .Distinct()
                .ToList();

            // A context menu opened on one of the selected rows operates on the
            // complete selection. A menu opened on another row targets only it.
            return selected.Contains(target) ? selected : new List<FileEntry> { target };
        }

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
            public ImageSource FallbackIcon { get; set; }
            public string SizeText { get; set; }
            public string DateModifiedText { get; set; }
            public StorageFolder Folder { get; set; }
            public StorageFile File { get; set; }
            public Visibility ArchiveVisibility => ArchiveService.IsArchive(File) ? Visibility.Visible : Visibility.Collapsed;
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
