using System;
using System.Threading.Tasks;
using Windows.Foundation;
using Windows.Storage;
using Windows.Storage.Pickers;
using Windows.System;
using Windows.UI.Xaml;
using Windows.UI.Xaml.Controls;
using Windows.UI.Xaml.Input;
using Windows.UI.Xaml.Media;
using factoryos_10x_shell.Services.Helpers;

namespace factoryos_10x_shell.Controls
{
    public sealed partial class NotepadWindow : UserControl
    {
        private StorageFile m_currentFile;
        private bool m_isDirty;
        private bool m_ignoreTextChanges;
        private bool m_dragging, m_resizing, m_maximized;
        private Point m_startPoint;
        private double m_startLeft, m_startTop, m_startWidth, m_startHeight;
        private double m_restoreLeft, m_restoreTop, m_restoreWidth, m_restoreHeight;

        public NotepadWindow() => InitializeComponent();

        public void Open()
        {
            Visibility = Visibility.Visible;
            AppState.Instance.SetNotepadWindowState(true, false);
            AppState.Instance.ActivateNotepad();
            Editor.Focus(FocusState.Programmatic);
        }

        public async Task OpenFileAsync(StorageFile file)
        {
            if (file == null || !await ConfirmDiscardOrSaveAsync()) return;
            try
            {
                m_currentFile = file;
                SetDocumentText(await FileIO.ReadTextAsync(file));
                m_isDirty = false;
                UpdateTitle();
                Open();
            }
            catch { await ShowMessageAsync("Couldn't open this text file."); }
        }

        public async void CloseFromTaskView() => await CloseAsync();

        private async Task CloseAsync()
        {
            if (!await ConfirmDiscardOrSaveAsync()) return;
            Visibility = Visibility.Collapsed;
            AppState.Instance.SetNotepadWindowState(false, false);
            m_currentFile = null;
            SetDocumentText(string.Empty);
            m_isDirty = false;
            UpdateTitle();
        }

        private async void Close_Click(object sender, RoutedEventArgs e) => await CloseAsync();
        private void Minimize_Click(object sender, RoutedEventArgs e) { Visibility = Visibility.Collapsed; AppState.Instance.SetNotepadWindowState(true, true); }
        private void Root_PointerPressed(object sender, PointerRoutedEventArgs e) => AppState.Instance.ActivateNotepad();

        private void Maximize_Click(object sender, RoutedEventArgs e)
        {
            Canvas host = VisualTreeHelper.GetParent(this) as Canvas;
            if (host == null) return;
            if (!m_maximized)
            {
                m_restoreLeft = Canvas.GetLeft(this); m_restoreTop = Canvas.GetTop(this); m_restoreWidth = Width; m_restoreHeight = Height;
                Canvas.SetLeft(this, 0); Canvas.SetTop(this, 0); Width = host.ActualWidth; Height = host.ActualHeight; m_maximized = true;
            }
            else
            {
                Canvas.SetLeft(this, m_restoreLeft); Canvas.SetTop(this, m_restoreTop); Width = m_restoreWidth; Height = m_restoreHeight; m_maximized = false;
            }
        }

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
            Canvas.SetLeft(this, Math.Max(0, m_startLeft + point.X - m_startPoint.X)); Canvas.SetTop(this, Math.Max(0, m_startTop + point.Y - m_startPoint.Y));
        }
        private void TitleBar_PointerReleased(object sender, PointerRoutedEventArgs e) { m_dragging = false; TitleBar.ReleasePointerCaptures(); }
        private void ResizeGrip_PointerPressed(object sender, PointerRoutedEventArgs e) { if (!m_maximized) { m_resizing = true; m_startPoint = e.GetCurrentPoint(this).Position; m_startWidth = Width; m_startHeight = Height; ResizeGrip.CapturePointer(e.Pointer); } }
        private void ResizeGrip_PointerMoved(object sender, PointerRoutedEventArgs e) { if (m_resizing) { Point p = e.GetCurrentPoint(this).Position; Width = Math.Max(480, m_startWidth + p.X - m_startPoint.X); Height = Math.Max(320, m_startHeight + p.Y - m_startPoint.Y); } }
        private void ResizeGrip_PointerReleased(object sender, PointerRoutedEventArgs e) { m_resizing = false; ResizeGrip.ReleasePointerCaptures(); }

        private async void New_Click(object sender, RoutedEventArgs e)
        {
            if (!await ConfirmDiscardOrSaveAsync()) return;
            m_currentFile = null; SetDocumentText(string.Empty); m_isDirty = false; UpdateTitle(); Editor.Focus(FocusState.Programmatic);
        }
        private async void Open_Click(object sender, RoutedEventArgs e)
        {
            if (!await ConfirmDiscardOrSaveAsync()) return;
            var picker = new FileOpenPicker { SuggestedStartLocation = PickerLocationId.DocumentsLibrary };
            picker.FileTypeFilter.Add(".txt");
            StorageFile file = await picker.PickSingleFileAsync();
            if (file == null) return;
            m_isDirty = false;
            await OpenFileAsync(file);
        }
        private async void Save_Click(object sender, RoutedEventArgs e) => await SaveAsync(false);
        private async void SaveAs_Click(object sender, RoutedEventArgs e) => await SaveAsync(true);
        private async Task<bool> SaveAsync(bool saveAs)
        {
            StorageFile file = m_currentFile;
            if (file == null || saveAs)
            {
                var picker = new FileSavePicker { SuggestedStartLocation = PickerLocationId.DocumentsLibrary, SuggestedFileName = m_currentFile?.DisplayName ?? "Untitled" };
                picker.FileTypeChoices.Add("Text document", new[] { ".txt" });
                file = await picker.PickSaveFileAsync();
                if (file == null) return false;
            }
            try { await FileIO.WriteTextAsync(file, Editor.Text); m_currentFile = file; m_isDirty = false; UpdateTitle(); return true; }
            catch { await ShowMessageAsync("Couldn't save this file."); return false; }
        }
        private void Undo_Click(object sender, RoutedEventArgs e) { if (Editor.CanUndo) Editor.Undo(); }
        private void Redo_Click(object sender, RoutedEventArgs e) { if (Editor.CanRedo) Editor.Redo(); }
        private void Editor_TextChanged(object sender, TextChangedEventArgs e) { if (!m_ignoreTextChanges) { m_isDirty = true; UpdateTitle(); } UpdateStatus(); }
        private async void Editor_KeyDown(object sender, KeyRoutedEventArgs e)
        {
            if (!Window.Current.CoreWindow.GetKeyState(VirtualKey.Control).HasFlag(Windows.UI.Core.CoreVirtualKeyStates.Down)) return;
            if (e.Key == VirtualKey.S) { e.Handled = true; await SaveAsync(false); }
            else if (e.Key == VirtualKey.O) { e.Handled = true; Open_Click(this, null); }
        }
        private void SetDocumentText(string text) { m_ignoreTextChanges = true; Editor.Text = text; m_ignoreTextChanges = false; UpdateStatus(); }
        private void UpdateTitle() { TitleText.Text = (m_isDirty ? "*" : "") + (m_currentFile?.Name ?? "Untitled") + " - Notepad"; }
        private void UpdateStatus() { int position = Editor.SelectionStart; string before = Editor.Text.Substring(0, Math.Min(position, Editor.Text.Length)); int line = before.Split('\n').Length; int column = position - before.LastIndexOf('\n'); StatusText.Text = "Ln " + line + ", Col " + column; }
        private async Task<bool> ConfirmDiscardOrSaveAsync()
        {
            if (!m_isDirty) return true;
            var dialog = new ContentDialog { Title = "Save changes?", Content = "Do you want to save your changes before continuing?", PrimaryButtonText = "Save", SecondaryButtonText = "Don't save", CloseButtonText = "Cancel" };
            ContentDialogResult result = await dialog.ShowAsync();
            return result == ContentDialogResult.Primary ? await SaveAsync(false) : result == ContentDialogResult.Secondary;
        }
        private async Task ShowMessageAsync(string message) => await new ContentDialog { Title = "Notepad", Content = message, CloseButtonText = "OK" }.ShowAsync();
    }
}
