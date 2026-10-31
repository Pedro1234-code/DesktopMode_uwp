using System;
using System.Collections.Generic;
using System.Threading.Tasks;
using Windows.Devices.Input;
using Windows.Foundation;
using Windows.Storage;
using Windows.Storage.Pickers;
using Windows.System;
using Windows.UI.ViewManagement;
using Windows.UI.Xaml;
using Windows.UI.Xaml.Controls;
using Windows.UI.Xaml.Input;
using Windows.UI.Xaml.Media;
using factoryos_10x_shell.Services.Helpers;
using Muxc = Microsoft.UI.Xaml.Controls;

namespace factoryos_10x_shell.Controls
{
    public sealed partial class NotepadWindow : UserControl
    {
        private sealed class DocumentState
        {
            public StorageFile File;
            public string Text = string.Empty;
            public string SavedText = string.Empty;
            public int SelectionStart;
            public int SelectionLength;

            public string DisplayName => File?.Name ?? "Untitled";
            public bool IsDirty => !string.Equals(
                Text ?? string.Empty,
                SavedText ?? string.Empty,
                StringComparison.Ordinal);
        }

        private DocumentState m_activeDocument;
        private bool m_ignoreTextChanges;
        private bool m_switchingTabs;
        private bool m_nativeCursorRequested;
        private bool m_pointerInsideWindow;
        private bool m_dragging, m_resizing, m_maximized;
        private UIElement m_dragSurface;
        private WindowResizeCorner m_resizeCorner;
        private Point m_startPoint;
        private double m_startLeft, m_startTop, m_startWidth, m_startHeight;
        private double m_restoreLeft, m_restoreTop, m_restoreWidth, m_restoreHeight;

        public NotepadWindow()
        {
            InitializeComponent();
            AddDocumentTab(new DocumentState());
        }

        public void Open()
        {
            EnsureDocumentTab();
            Visibility = Visibility.Visible;
            AppState.Instance.SetNotepadWindowState(true, false);
            AppState.Instance.ActivateNotepad();
            Editor.PreventKeyboardDisplayOnProgrammaticFocus = HasPhysicalKeyboard();
            Editor.Focus(FocusState.Programmatic);
        }

        public async Task OpenFileAsync(StorageFile file)
        {
            if (file == null) return;

            Muxc.TabViewItem existingTab = FindTabForFile(file);
            if (existingTab != null)
            {
                SelectTab(existingTab);
                Open();
                return;
            }

            try
            {
                string text = await FileIO.ReadTextAsync(file);
                OpenDocument(file, text);
                Open();
            }
            catch { await ShowMessageAsync("Couldn't open this text file."); }
        }

        public async void CloseFromTaskView() => await CloseAsync();

        private async Task CloseAsync()
        {
            if (!await ConfirmAllDocumentsAsync()) return;
            CloseWindowWithoutPrompt();
        }

        private void CloseWindowWithoutPrompt()
        {
            DismissOnScreenKeyboard();
            Visibility = Visibility.Collapsed;
            m_pointerInsideWindow = false;
            UpdateNativeCursorRequest();
            AppState.Instance.SetNotepadWindowState(false, false);
            ResetDocuments();
        }

        private async void Close_Click(object sender, RoutedEventArgs e) => await CloseAsync();

        private void Minimize_Click(object sender, RoutedEventArgs e)
        {
            PersistActiveDocument();
            DismissOnScreenKeyboard();
            Visibility = Visibility.Collapsed;
            m_pointerInsideWindow = false;
            UpdateNativeCursorRequest();
            AppState.Instance.SetNotepadWindowState(true, true);
        }

        private void Root_PointerPressed(object sender, PointerRoutedEventArgs e) => AppState.Instance.ActivateNotepad();

        private void Root_PointerEntered(object sender, PointerRoutedEventArgs e)
        {
            m_pointerInsideWindow = true;
            UpdateNativeCursorRequest();
        }

        private void Root_PointerExited(object sender, PointerRoutedEventArgs e)
        {
            m_pointerInsideWindow = false;
            UpdateNativeCursorRequest();
        }

        private void UpdateNativeCursorRequest()
        {
            bool requested = m_pointerInsideWindow && Visibility == Visibility.Visible;
            if (requested == m_nativeCursorRequested) return;
            m_nativeCursorRequested = requested;
            MainPage.SetPopupCursorVisibility(requested);
        }

        private void Maximize_Click(object sender, RoutedEventArgs e)
        {
            Canvas host = VisualTreeHelper.GetParent(this) as Canvas;
            if (host == null) return;
            if (!m_maximized)
            {
                m_restoreLeft = Canvas.GetLeft(this); m_restoreTop = Canvas.GetTop(this); m_restoreWidth = Width; m_restoreHeight = Height;
                Canvas.SetLeft(this, 0); Canvas.SetTop(this, 0); Width = host.ActualWidth; Height = host.ActualHeight; m_maximized = true;
                MaximizeGlyph.Text = "\uE923";
            }
            else
            {
                Canvas.SetLeft(this, m_restoreLeft); Canvas.SetTop(this, m_restoreTop); Width = m_restoreWidth; Height = m_restoreHeight; m_maximized = false;
                MaximizeGlyph.Text = "\uE922";
            }
        }

        private void TitleBar_DoubleTapped(object sender, DoubleTappedRoutedEventArgs e)
        {
            m_dragging = false;
            m_dragSurface?.ReleasePointerCaptures();
            m_dragSurface = null;
            Maximize_Click(sender, e);
            e.Handled = true;
        }

        private void TitleBar_PointerPressed(object sender, PointerRoutedEventArgs e)
        {
            if (m_maximized) return;
            m_dragging = true;
            m_startPoint = e.GetCurrentPoint(VisualTreeHelper.GetParent(this) as UIElement).Position;
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

        private void ResizeGrip_PointerPressed(object sender, PointerRoutedEventArgs e) { if (!m_maximized) { m_resizing = true; m_resizeCorner = WindowResize.CornerFromTag((sender as FrameworkElement)?.Tag); m_startPoint = e.GetCurrentPoint(null).Position; m_startLeft = Canvas.GetLeft(this); m_startTop = Canvas.GetTop(this); m_startWidth = Width; m_startHeight = Height; (sender as UIElement)?.CapturePointer(e.Pointer); } }
        private void ResizeGrip_PointerMoved(object sender, PointerRoutedEventArgs e) { if (m_resizing) { Rect bounds = WindowResize.Calculate(m_resizeCorner, m_startPoint, e.GetCurrentPoint(null).Position, new Rect(m_startLeft, m_startTop, m_startWidth, m_startHeight), 480, 320); Canvas.SetLeft(this, bounds.X); Canvas.SetTop(this, bounds.Y); Width = bounds.Width; Height = bounds.Height; } }
        private void ResizeGrip_PointerReleased(object sender, PointerRoutedEventArgs e) { m_resizing = false; (sender as UIElement)?.ReleasePointerCaptures(); }

        private void DocumentTabs_AddTabButtonClick(Muxc.TabView sender, object args) => AddDocumentTab(new DocumentState());

        private void DocumentTabs_SelectionChanged(object sender, SelectionChangedEventArgs e)
        {
            if (m_switchingTabs) return;
            PersistActiveDocument();
            if (DocumentTabs.SelectedItem is Muxc.TabViewItem tab && tab.Tag is DocumentState document)
                LoadDocument(document);
        }

        private async void DocumentTabs_TabCloseRequested(Muxc.TabView sender, Muxc.TabViewTabCloseRequestedEventArgs args)
        {
            await CloseTabAsync(args.Tab);
        }

        private void New_Click(object sender, RoutedEventArgs e) => AddDocumentTab(new DocumentState());

        private async void Open_Click(object sender, RoutedEventArgs e)
        {
            DismissOnScreenKeyboard();
            var picker = new FileOpenPicker { SuggestedStartLocation = PickerLocationId.DocumentsLibrary };
            picker.FileTypeFilter.Add(".txt");
            StorageFile file = await picker.PickSingleFileAsync();
            if (file != null) await OpenFileAsync(file);
        }

        private async void Save_Click(object sender, RoutedEventArgs e) => await SaveAsync(false);
        private async void SaveAs_Click(object sender, RoutedEventArgs e) => await SaveAsync(true);
        private async void CloseTab_Click(object sender, RoutedEventArgs e)
        {
            if (DocumentTabs.SelectedItem is Muxc.TabViewItem tab) await CloseTabAsync(tab);
        }

        private async Task<bool> SaveAsync(bool saveAs)
        {
            if (m_activeDocument == null) return false;
            PersistActiveDocument();
            StorageFile file = m_activeDocument.File;
            if (file == null || saveAs)
            {
                DismissOnScreenKeyboard();
                var picker = new FileSavePicker { SuggestedStartLocation = PickerLocationId.DocumentsLibrary, SuggestedFileName = file?.DisplayName ?? "Untitled" };
                picker.FileTypeChoices.Add("Text document", new[] { ".txt" });
                file = await picker.PickSaveFileAsync();
                if (file == null) return false;
            }
            try
            {
                await FileIO.WriteTextAsync(file, m_activeDocument.Text);
                m_activeDocument.File = file;
                m_activeDocument.SavedText = m_activeDocument.Text ?? string.Empty;
                UpdateTitle();
                return true;
            }
            catch { await ShowMessageAsync("Couldn't save this file."); return false; }
        }

        private void Undo_Click(object sender, RoutedEventArgs e) { if (Editor.CanUndo) Editor.Undo(); }
        private void Redo_Click(object sender, RoutedEventArgs e) { if (Editor.CanRedo) Editor.Redo(); }
        private void Cut_Click(object sender, RoutedEventArgs e) => Editor.CutSelectionToClipboard();
        private void Copy_Click(object sender, RoutedEventArgs e) => Editor.CopySelectionToClipboard();
        private void Paste_Click(object sender, RoutedEventArgs e) => Editor.PasteFromClipboard();
        private void SelectAll_Click(object sender, RoutedEventArgs e) => Editor.SelectAll();

        private void WordWrap_Click(object sender, RoutedEventArgs e)
        {
            Editor.TextWrapping = WordWrapMenuItem.IsChecked ? TextWrapping.Wrap : TextWrapping.NoWrap;
            ScrollViewer.SetHorizontalScrollBarVisibility(Editor, WordWrapMenuItem.IsChecked ? ScrollBarVisibility.Disabled : ScrollBarVisibility.Auto);
        }

        private void StatusBar_Click(object sender, RoutedEventArgs e) => StatusBar.Visibility = StatusBarMenuItem.IsChecked ? Visibility.Visible : Visibility.Collapsed;

        private void Editor_TextChanged(object sender, TextChangedEventArgs e)
        {
            if (!m_ignoreTextChanges && m_activeDocument != null)
            {
                m_activeDocument.Text = Editor.Text;
                UpdateTitle();
            }
            UpdateStatus();
        }

        private void Editor_SelectionChanged(object sender, RoutedEventArgs e)
        {
            if (!m_ignoreTextChanges && m_activeDocument != null)
            {
                m_activeDocument.SelectionStart = Editor.SelectionStart;
                m_activeDocument.SelectionLength = Editor.SelectionLength;
            }
            UpdateStatus();
        }

        private async void Editor_GotFocus(object sender, RoutedEventArgs e)
        {
            if (!HasPhysicalKeyboard()) return;
            try { InputPane.GetForCurrentView().TryHide(); } catch { }
            await Task.Delay(180);
            try { InputPane.GetForCurrentView().TryHide(); } catch { }
        }

        private async void Editor_KeyDown(object sender, KeyRoutedEventArgs e)
        {
            if (!Window.Current.CoreWindow.GetKeyState(VirtualKey.Control).HasFlag(Windows.UI.Core.CoreVirtualKeyStates.Down)) return;
            bool shift = Window.Current.CoreWindow.GetKeyState(VirtualKey.Shift).HasFlag(Windows.UI.Core.CoreVirtualKeyStates.Down);
            if (e.Key == VirtualKey.S) { e.Handled = true; await SaveAsync(shift); }
            else if (e.Key == VirtualKey.O) { e.Handled = true; Open_Click(this, null); }
            else if (e.Key == VirtualKey.N) { e.Handled = true; New_Click(this, null); }
            else if (e.Key == VirtualKey.W)
            {
                e.Handled = true;
                if (DocumentTabs.SelectedItem is Muxc.TabViewItem tab) await CloseTabAsync(tab);
            }
            else if (e.Key == VirtualKey.Tab)
            {
                e.Handled = true;
                SelectRelativeTab(shift ? -1 : 1);
            }
        }

        private void AddDocumentTab(DocumentState document)
        {
            PersistActiveDocument();
            var tab = new Muxc.TabViewItem { Tag = document, Header = DocumentHeader(document), IsClosable = true };
            m_switchingTabs = true;
            DocumentTabs.TabItems.Add(tab);
            DocumentTabs.SelectedItem = tab;
            m_switchingTabs = false;
            LoadDocument(document);
        }

        private void OpenDocument(StorageFile file, string text)
        {
            if (CanReuseActiveBlank())
            {
                m_activeDocument.File = file;
                m_activeDocument.Text = text ?? string.Empty;
                m_activeDocument.SavedText = m_activeDocument.Text;
                m_activeDocument.SelectionStart = 0;
                m_activeDocument.SelectionLength = 0;
                LoadDocument(m_activeDocument);
            }
            else
            {
                string loadedText = text ?? string.Empty;
                AddDocumentTab(new DocumentState
                {
                    File = file,
                    Text = loadedText,
                    SavedText = loadedText
                });
            }
        }

        private bool CanReuseActiveBlank() => DocumentTabs.TabItems.Count == 1 && m_activeDocument != null && m_activeDocument.File == null && !m_activeDocument.IsDirty && string.IsNullOrEmpty(m_activeDocument.Text);

        private void EnsureDocumentTab()
        {
            if (DocumentTabs.TabItems.Count == 0) AddDocumentTab(new DocumentState());
        }

        private void SelectTab(Muxc.TabViewItem tab)
        {
            if (tab == null) return;
            PersistActiveDocument();
            m_switchingTabs = true;
            DocumentTabs.SelectedItem = tab;
            m_switchingTabs = false;
            if (tab.Tag is DocumentState document) LoadDocument(document);
        }

        private void SelectRelativeTab(int offset)
        {
            int count = DocumentTabs.TabItems.Count;
            if (count < 2) return;
            int current = DocumentTabs.TabItems.IndexOf(DocumentTabs.SelectedItem);
            int next = (current + offset + count) % count;
            SelectTab(DocumentTabs.TabItems[next] as Muxc.TabViewItem);
        }

        private void LoadDocument(DocumentState document)
        {
            if (document == null) return;
            m_activeDocument = document;
            m_ignoreTextChanges = true;
            Editor.Text = document.Text ?? string.Empty;
            int start = Math.Min(document.SelectionStart, Editor.Text.Length);
            int length = Math.Min(document.SelectionLength, Editor.Text.Length - start);
            Editor.Select(start, length);
            m_ignoreTextChanges = false;
            UpdateTitle();
            UpdateStatus();
        }

        private void PersistActiveDocument()
        {
            if (m_activeDocument == null || m_ignoreTextChanges) return;
            m_activeDocument.Text = Editor.Text;
            m_activeDocument.SelectionStart = Editor.SelectionStart;
            m_activeDocument.SelectionLength = Editor.SelectionLength;
        }

        private async Task CloseTabAsync(Muxc.TabViewItem tab)
        {
            if (!(tab?.Tag is DocumentState document)) return;
            if (ReferenceEquals(document, m_activeDocument)) PersistActiveDocument();
            else SelectTab(tab);
            if (!await ConfirmDocumentCloseAsync(document)) return;

            int index = DocumentTabs.TabItems.IndexOf(tab);
            m_switchingTabs = true;
            DocumentTabs.TabItems.Remove(tab);
            m_switchingTabs = false;

            if (DocumentTabs.TabItems.Count == 0)
            {
                CloseWindowWithoutPrompt();
                return;
            }

            int nextIndex = Math.Min(index, DocumentTabs.TabItems.Count - 1);
            SelectTab(DocumentTabs.TabItems[nextIndex] as Muxc.TabViewItem);
        }

        private async Task<bool> ConfirmDocumentCloseAsync(DocumentState document)
        {
            if (document == null || !document.IsDirty) return true;
            DismissOnScreenKeyboard();
            var dialog = new ContentDialog
            {
                Title = "Save changes to " + document.DisplayName + "?",
                Content = "Your changes will be lost if you don't save them.",
                PrimaryButtonText = "Save",
                SecondaryButtonText = "Don't save",
                CloseButtonText = "Cancel"
            };
            ContentDialogResult result = await dialog.ShowAsync();
            if (result == ContentDialogResult.Primary) return await SaveAsync(false);
            return result == ContentDialogResult.Secondary;
        }

        private async Task<bool> ConfirmAllDocumentsAsync()
        {
            PersistActiveDocument();
            var tabs = new List<Muxc.TabViewItem>();
            foreach (object item in DocumentTabs.TabItems)
                if (item is Muxc.TabViewItem tab) tabs.Add(tab);

            foreach (Muxc.TabViewItem tab in tabs)
            {
                if (!(tab.Tag is DocumentState document) || !document.IsDirty) continue;
                SelectTab(tab);
                if (!await ConfirmDocumentCloseAsync(document)) return false;
            }
            return true;
        }

        private Muxc.TabViewItem FindTabForFile(StorageFile file)
        {
            string path = file?.Path;
            if (string.IsNullOrEmpty(path)) return null;
            foreach (object item in DocumentTabs.TabItems)
            {
                if (item is Muxc.TabViewItem tab && tab.Tag is DocumentState document &&
                    string.Equals(document.File?.Path, path, StringComparison.OrdinalIgnoreCase)) return tab;
            }
            return null;
        }

        private Muxc.TabViewItem FindTab(DocumentState document)
        {
            foreach (object item in DocumentTabs.TabItems)
                if (item is Muxc.TabViewItem tab && ReferenceEquals(tab.Tag, document)) return tab;
            return null;
        }

        private void ResetDocuments()
        {
            m_switchingTabs = true;
            DocumentTabs.TabItems.Clear();
            m_switchingTabs = false;
            m_activeDocument = null;
            m_ignoreTextChanges = true;
            Editor.Text = string.Empty;
            m_ignoreTextChanges = false;
            AddDocumentTab(new DocumentState());
        }

        private static string DocumentHeader(DocumentState document) => (document.IsDirty ? "*" : string.Empty) + document.DisplayName;

        private void UpdateTitle()
        {
            if (m_activeDocument == null) return;
            Muxc.TabViewItem tab = FindTab(m_activeDocument);
            if (tab != null) tab.Header = DocumentHeader(m_activeDocument);
        }

        private void UpdateStatus()
        {
            int position = Editor.SelectionStart;
            string before = Editor.Text.Substring(0, Math.Min(position, Editor.Text.Length));
            int line = before.Split('\n').Length;
            int column = position - before.LastIndexOf('\n');
            StatusText.Text = "Ln " + line + ", Col " + column;
        }

        private async Task ShowMessageAsync(string message) => await new ContentDialog { Title = "Notepad", Content = message, CloseButtonText = "OK" }.ShowAsync();

        private static bool HasPhysicalKeyboard()
        {
            try { return new KeyboardCapabilities().KeyboardPresent != 0; }
            catch { return false; }
        }

        private async void DismissOnScreenKeyboard()
        {
            try { InputPane.GetForCurrentView().TryHide(); } catch { }
            await Task.Delay(150);
            try { InputPane.GetForCurrentView().TryHide(); } catch { }
        }
    }
}
