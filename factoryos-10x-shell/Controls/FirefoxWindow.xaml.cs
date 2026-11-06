using DesktopMode.Gecko;
using factoryos_10x_shell.Library.Services.Input;
using factoryos_10x_shell.Services.Helpers;
using Microsoft.Extensions.DependencyInjection;
using System;
using System.Collections.Generic;
using System.Linq;
using System.Threading.Tasks;
using Windows.ApplicationModel.DataTransfer;
using Windows.Foundation;
using Windows.Storage;
using Windows.UI.Core;
using Windows.UI.Xaml;
using Windows.UI.Xaml.Controls;
using Windows.UI.Xaml.Input;
using Windows.UI.Xaml.Media;

namespace factoryos_10x_shell.Controls
{
    public sealed partial class FirefoxWindow : UserControl
    {
        public event Action<bool> FullscreenChanged;
        public event Action<bool> ExternalWindowChanged;

        private static FirefoxWindow s_current;
        private GeckoHost m_host;
        private readonly List<StorageFolder> m_droppedUploadFolders = new List<StorageFolder>();
        private sealed class UploadDrag
        {
            public Task<string> Paths;
            public StorageFolder StagingFolder;
            public bool Cancelled;
            public bool Entered;
            public bool Dropped;
            public bool Dropping;
        }
        private UploadDrag m_uploadDrag;
        internal static void BeginFileDrag()
        {
            if (s_current != null)
                s_current.UploadDropTarget.IsHitTestVisible = true;
        }

        internal static void EndFileDrag()
        {
            if (s_current != null)
                s_current.UploadDropTarget.IsHitTestVisible = false;
            // A source completion notification must not cancel a Drop deferral
            // that is still resolving brokered StorageFiles.
            if (s_current?.m_uploadDrag?.Dropping != true)
                s_current?.CancelUploadDrag();
        }

        private async Task<string> PrepareUploadFilesAsync(UploadDrag drag, DataPackageView data)
        {
            StorageFolder folder = null;
            try
            {
                var items = await data.GetStorageItemsAsync();
                var files = items.OfType<StorageFile>().ToList();
                if (files.Count == 0) return null;
                var uploads = await ApplicationData.Current.TemporaryFolder.CreateFolderAsync(
                    "StrawfoxUploads", CreationCollisionOption.OpenIfExists);
                folder = await uploads.CreateFolderAsync(Guid.NewGuid().ToString("N"));
                drag.StagingFolder = folder;
                var paths = new System.Collections.Generic.List<string>();
                foreach (StorageFile file in files)
                {
                    if (drag.Cancelled) return null;
                    // Different selected folders can contain files with the
                    // same name. Isolate each copy so DOM File.name is preserved.
                    StorageFolder fileFolder = await folder.CreateFolderAsync(paths.Count.ToString());
                    StorageFile copy = await file.CopyAsync(fileFolder, file.Name, NameCollisionOption.FailIfExists);
                    paths.Add(copy.Path);
                }
                return drag.Cancelled ? null : string.Join("\n", paths);
            }
            finally
            {
                // Files delivered to DOM File objects must remain readable after
                // Drop returns: uploads and FileReader can start asynchronously.
                if (folder != null && drag.Cancelled && !drag.Dropped)
                    await DeleteUploadFolderAsync(folder);
            }
        }

        private static async Task DeleteUploadFolderAsync(StorageFolder folder)
        {
            if (folder == null) return;
            try { await folder.DeleteAsync(StorageDeleteOption.PermanentDelete); }
            catch (Exception) { }
        }

        private bool CanAcceptUpload(DragEventArgs e) => m_host?.IsStarted == true &&
            e.DataView.Contains(StandardDataFormats.StorageItems);

        private async void BrowserSurface_DragEnter(object sender, DragEventArgs e)
        {
            if (!CanAcceptUpload(e)) return;
            e.Handled = true;
            e.AcceptedOperation = DataPackageOperation.Copy;
            var deferral = e.GetDeferral();
            var drag = m_uploadDrag;
            try
            {
                if (drag == null)
                {
                    drag = new UploadDrag();
                    m_uploadDrag = drag;
                    drag.Paths = PrepareUploadFilesAsync(drag, e.DataView);
                }
                string paths = await drag.Paths;
                if (drag.Cancelled || m_uploadDrag != drag || string.IsNullOrEmpty(paths))
                {
                    e.AcceptedOperation = DataPackageOperation.None;
                    return;
                }
                Point point = e.GetPosition(BrowserSurface);
                if (!drag.Entered)
                    drag.Entered = m_host?.DragFiles(0, point.X, point.Y, paths) == true;
                if (!drag.Entered)
                {
                    e.AcceptedOperation = DataPackageOperation.None;
                    if (m_uploadDrag == drag) CancelUploadDrag();
                }
            }
            catch (Exception exception)
            {
                e.AcceptedOperation = DataPackageOperation.None;
                if (m_uploadDrag == drag) CancelUploadDrag();
                System.Diagnostics.Debug.WriteLine($"Upload drag preparation failed: 0x{exception.HResult:X8}");
            }
            finally { deferral.Complete(); }
        }

        private void BrowserSurface_DragOver(object sender, DragEventArgs e)
        {
            if (!CanAcceptUpload(e)) return;
            e.Handled = true;
            e.AcceptedOperation = m_uploadDrag != null ? DataPackageOperation.Copy : DataPackageOperation.None;
            e.DragUIOverride.Caption = "Upload files";
            if (m_uploadDrag?.Entered != true) return;
            Point point = e.GetPosition(BrowserSurface);
            m_host.DragFiles(1, point.X, point.Y, string.Empty);
        }

        private void BrowserSurface_DragLeave(object sender, DragEventArgs e)
        {
            if (m_uploadDrag?.Dropping == true) return;
            Point point = e.GetPosition(BrowserSurface);
            if (point.X >= 0 && point.Y >= 0 && point.X < BrowserSurface.ActualWidth &&
                point.Y < BrowserSurface.ActualHeight) return;
            CancelUploadDrag();
        }

        private void CancelUploadDrag()
        {
            var drag = m_uploadDrag;
            m_uploadDrag = null;
            if (drag == null) return;
            drag.Cancelled = true;
            if (drag.Entered) m_host?.DragFiles(2, 0, 0, string.Empty);
            // If file staging already finished, PrepareUploadFilesAsync has
            // already passed its cleanup point. Remove canceled uploads here;
            // an in-progress staging task performs the same cleanup in finally.
            if (!drag.Dropped && drag.Paths?.IsCompleted == true)
                _ = DeleteUploadFolderAsync(drag.StagingFolder);
        }

        private async void BrowserSurface_Drop(object sender, DragEventArgs e)
        {
            if (!CanAcceptUpload(e)) return;
            e.Handled = true;
            e.AcceptedOperation = DataPackageOperation.None;
            var deferral = e.GetDeferral();
            var drag = m_uploadDrag;
            Point point = e.GetPosition(BrowserSurface);
            try
            {
                if (drag == null)
                {
                    drag = new UploadDrag();
                    m_uploadDrag = drag;
                    drag.Paths = PrepareUploadFilesAsync(drag, e.DataView);
                }
                drag.Dropping = true;
                string paths = await drag.Paths;
                if (drag.Cancelled || m_uploadDrag != drag || string.IsNullOrEmpty(paths)) return;
                AppState.Instance.ActivateFirefox();
                if (!drag.Entered)
                    drag.Entered = m_host?.DragFiles(0, point.X, point.Y, paths) == true;
                // Even a fast enter-and-release must let Gecko hit-test the
                // target and negotiate dropEffect before receiving the drop.
                if (drag.Entered && m_host.DragFiles(1, point.X, point.Y, string.Empty) &&
                    m_host.DragFiles(3, point.X, point.Y, paths))
                {
                    e.AcceptedOperation = DataPackageOperation.Copy;
                    drag.Dropped = true;
                    drag.Entered = false;
                    if (drag.StagingFolder != null)
                        m_droppedUploadFolders.Add(drag.StagingFolder);
                }
            }
            catch (Exception exception)
            {
                System.Diagnostics.Debug.WriteLine($"Upload drop failed: 0x{exception.HResult:X8}");
            }
            finally
            {
                if (m_uploadDrag == drag) CancelUploadDrag();
                deferral.Complete();
            }
        }
        private bool m_hostAttached;
        private bool m_hostStarting;
        private bool m_hostFailed;
        private bool m_resizing;
        private bool m_dragging;
        private bool m_maximized;
        private bool m_fullscreen;
        private bool m_wasMaximizedBeforeFullscreen;
        private bool m_isWindowActive;
        private bool m_inputSuppressed;
        private bool m_nativeCursorRequested;
        private bool m_pointerInsideWindow;
        private bool m_externalWindowOpen;
        private bool m_initialWindowStateApplied;
        private readonly System.Collections.Generic.Queue<string> m_pendingUrls =
            new System.Collections.Generic.Queue<string>();
        private WindowResizeCorner m_resizeCorner;
        private Point m_startPoint;
        private double m_startWidth;
        private double m_startHeight;
        private double m_restoreLeft;
        private double m_restoreTop;
        private double m_restoreWidth;
        private double m_restoreHeight;
        private Point m_dragStartPointer;
        private double m_dragStartLeft;
        private double m_dragStartTop;
        private Thickness m_restoreHostMargin;
        private int m_restoreHostZIndex;
        private readonly IMouseInputService m_mouseInput;
        private readonly DispatcherTimer m_windowCommandTimer;
        private readonly DispatcherTimer m_viewportResizeTimer;
        private Canvas m_hookedHost;
        private FrameworkElement m_hookedScreen;

        public FirefoxWindow()
        {
            InitializeComponent();
            s_current = this;
            m_mouseInput = App.ServiceProvider.GetRequiredService<IMouseInputService>();
            m_mouseInput.InputChanged += MouseInput_InputChanged;
            Loaded += FirefoxWindow_Loaded;
            UpdateResizeGripVisibility();
            // Window commands are rare. Every tick crosses the managed/WinRT
            // boundary at least twice, so polling at rendering frequency can
            // steal enough UI-thread slices to make the Shell pointer hitch.
            // 100 ms is still immediate for title-bar buttons and popups; an
            // active native title-bar drag temporarily switches to 60 Hz below.
            m_windowCommandTimer = new DispatcherTimer { Interval = TimeSpan.FromMilliseconds(100) };
            m_windowCommandTimer.Tick += WindowCommandTimer_Tick;
            // A fullscreen transition changes the host margin, the window
            // border and the window bounds in separate XAML layout passes.
            // Publish only the settled viewport; Gecko already knows the
            // final screen rectangle before it announces DOM fullscreen.
            m_viewportResizeTimer = new DispatcherTimer
            {
                Interval = TimeSpan.FromMilliseconds(75)
            };
            m_viewportResizeTimer.Tick += ViewportResizeTimer_Tick;
        }

        public static bool SuspendRuntime()
        {
            if (s_current?.m_host?.IsStarted != true) return false;
            try
            {
                s_current.Suspend();
                return true;
            }
            catch
            {
                return false;
            }
        }

        public static void ResumeRuntime()
        {
            try { s_current?.Resume(); } catch { }
        }

        private readonly System.Threading.SemaphoreSlim m_sessionCreationLock = new System.Threading.SemaphoreSlim(1, 1);
        internal async Task<GeckoWindow> CreateAppWindowAsync(string url)
        {
            await m_sessionCreationLock.WaitAsync();
            bool warming = !AppState.Instance.IsFirefoxOpen;
            double opacity = Opacity;
            bool hitTest = IsHitTestVisible;
            try
            {
                if (warming) { Opacity = 0; IsHitTestVisible = false; Visibility = Visibility.Visible; }
                HookHostCanvas();
                UpdateLayout();
                EnsureHost();
                var deadline = DateTimeOffset.UtcNow.AddSeconds(30);
                while (DateTimeOffset.UtcNow < deadline)
                {
                    if (m_hostFailed) throw new InvalidOperationException("The shared Strawfox runtime failed to start.");
                    if (m_host?.IsStarted == true)
                    {
                        try { return m_host.CreateBrowserWindow(url, true); }
                        catch (Exception error) when (error.HResult == unchecked((int)0x8000000A)) { }
                    }
                    else if (!m_hostStarting) EnsureHost();
                    await Task.Delay(100);
                }
                throw new TimeoutException("Timed out starting the shared Strawfox runtime.");
            }
            finally
            {
                try
                {
                    if (warming && !AppState.Instance.IsFirefoxOpen)
                    {
                        m_host?.SetActive(false);
                        Visibility = Visibility.Collapsed;
                        m_pointerInsideWindow = false;
                        UpdateNativeCursorRequest();
                    }
                    Opacity = opacity;
                    IsHitTestVisible = hitTest;
                }
                finally { m_sessionCreationLock.Release(); }
            }
        }

        public void Open()
        {
            Opacity = 1;
            IsHitTestVisible = true;
            Visibility = Visibility.Visible;
            HookHostCanvas();
            ApplyInitialWindowState();
            m_windowCommandTimer.Interval = TimeSpan.FromMilliseconds(100);
            if (!m_windowCommandTimer.IsEnabled)
                m_windowCommandTimer.Start();
            EnsureHost();
            AppState.Instance.SetFirefoxWindowState(true, false);
            AppState.Instance.ActivateFirefox();
            if (m_externalWindowOpen)
                ExternalWindowChanged?.Invoke(true);
        }

        public async void OpenFile(StorageFile file)
        {
            if (file == null) return;
            Open();
            try
            {
                StorageFolder cache = await ApplicationData.Current.LocalCacheFolder.CreateFolderAsync(
                    "FirefoxPdf", CreationCollisionOption.OpenIfExists);
                StorageFile readable = await file.CopyAsync(
                    cache, file.Name, NameCollisionOption.ReplaceExisting);
                m_pendingUrls.Enqueue(new System.Uri(readable.Path).AbsoluteUri);
            }
            catch
            {
                // Keep the direct path as a fallback on desktop systems where
                // the activation grant is also honored by native file access.
                m_pendingUrls.Enqueue(new System.Uri(file.Path).AbsoluteUri);
            }
            FlushPendingUrl();
        }

        public void OpenUrl(string url)
        {
            if (!Uri.TryCreate(url, UriKind.Absolute, out Uri uri) ||
                (uri.Scheme != Uri.UriSchemeHttp && uri.Scheme != Uri.UriSchemeHttps)) return;
            Open();
            m_pendingUrls.Enqueue(uri.AbsoluteUri);
            FlushPendingUrl();
        }

        private void FlushPendingUrl()
        {
            if (m_host?.IsStarted != true) return;
            while (m_pendingUrls.Count > 0)
            {
                m_host.OpenUrl(m_pendingUrls.Peek());
                m_pendingUrls.Dequeue();
            }
        }

        public void CloseFromTaskView() => Close();

        public bool IsFullscreen => m_fullscreen;

        public void SetWindowActive(bool active)
        {
            m_isWindowActive = active;
            // Activation does not make commands more frequent. Keep the idle
            // cadence stable; BeginWindowDrag is the only operation that needs
            // frame-rate polling and switches it temporarily below.
            if (!m_dragging)
                m_windowCommandTimer.Interval = TimeSpan.FromMilliseconds(100);
            UpdateHostInput();
        }

        public void SetShellInputSuppressed(bool suppressed)
        {
            m_inputSuppressed = suppressed;
            UpdateHostInput();
        }

        public void Suspend() => m_host?.Suspend();

        public void Resume() => m_host?.Resume();

        private async void EnsureHost()
        {
            if (m_hostFailed || m_hostStarting) return;

            m_hostStarting = true;

            try
            {
                if (!m_hostAttached)
                {
                    m_host = new GeckoHost();
                    if (!(m_host.Content is UIElement content))
                    {
                        throw new InvalidOperationException("The Gecko host did not provide a XAML UIElement surface.");
                    }

                    BrowserSurface.Children.Add(content);
                    // UploadDropTarget sits above the mixed XAML/DirectX tree
                    // only during a Files drag. Its hit target is unambiguous
                    // regardless of which compositor child currently presents.
                    m_hostAttached = true;
                    FlushPendingUrl();
                }

                await Dispatcher.RunAsync(CoreDispatcherPriority.Normal, () =>
                {
                    BrowserSurface.UpdateLayout();
                    if (!m_host.IsStarted && BrowserSurface.ActualWidth > 0 && BrowserSurface.ActualHeight > 0)
                        m_host.Start(BrowserSurface.ActualWidth, BrowserSurface.ActualHeight);
                    else
                        m_host.SetViewport(BrowserSurface.ActualWidth, BrowserSurface.ActualHeight);
                    UpdateGeckoScreen();
                    UpdateHostInput();
                    FlushPendingUrl();
                });

                // MainDesktop is created after the Shell's first activation on
                // Xbox. The first layout pass can therefore expose an
                // intermediate window size and never raise another useful
                // SizeChanged event. Re-read the settled size on the next
                // low-priority dispatcher pass so Gecko and ANGLE allocate the
                // framebuffer at the final physical resolution.
                await Dispatcher.RunAsync(CoreDispatcherPriority.Low, () =>
                {
                    if (m_host?.IsStarted != true) return;
                    BrowserSurface.UpdateLayout();
                    if (BrowserSurface.ActualWidth <= 0 || BrowserSurface.ActualHeight <= 0) return;
                    m_host.SetViewport(BrowserSurface.ActualWidth, BrowserSurface.ActualHeight);
                    UpdateGeckoScreen();
                });
            }
            catch (Exception exception)
            {
                ShowHostFailure(exception);
            }
            finally
            {
                m_hostStarting = false;
            }
        }

        private void ShowHostFailure(Exception exception)
        {
            m_hostFailed = true;
            try { m_host?.SetActive(false); } catch { }
            UploadDropTarget.IsHitTestVisible = false;
            BrowserSurface.Children.Clear();
            BrowserSurface.Children.Add(new StackPanel
            {
                Margin = new Thickness(32),
                Spacing = 12,
                Children =
                {
                    new TextBlock { Text = "Strawfox could not be started", FontSize = 24 },
                    new TextBlock { Text = exception.ToString(), TextWrapping = TextWrapping.Wrap, IsTextSelectionEnabled = true }
                }
            });
        }

        private void UpdateHostInput()
        {
            bool active = AppState.Instance.IsFirefoxOpen && !AppState.Instance.IsFirefoxMinimized &&
                m_isWindowActive && !m_inputSuppressed && IsHitTestVisible && Visibility == Visibility.Visible;
            m_host?.SetActive(active);
            UpdateNativeCursorRequest();
        }

        private void UpdateNativeCursorRequest()
        {
            bool requested = AppState.Instance.IsFirefoxOpen && !AppState.Instance.IsFirefoxMinimized &&
                m_pointerInsideWindow && IsHitTestVisible && Visibility == Visibility.Visible;
            if (requested == m_nativeCursorRequested) return;

            m_nativeCursorRequested = requested;
            // Gecko supplies CSS cursor changes through CoreWindow. While its
            // window is under the pointer, let that native cursor replace the
            // Shell bitmap; restore the custom cursor after leaving the window.
            MainPage.SetPopupCursorVisibility(requested);
        }

        private void Close()
        {
            UploadDropTarget.IsHitTestVisible = false;
            CancelUploadDrag();
            foreach (StorageFolder folder in m_droppedUploadFolders)
                _ = DeleteUploadFolderAsync(folder);
            m_droppedUploadFolders.Clear();
            m_pendingUrls.Clear();
            if (m_fullscreen) SetFullscreen(false);
            SetWindowActive(false);
            // Closing is distinct from minimizing: stop the embedded host and
            // put Gecko into its suspended application state so media, timers
            // and page activity do not keep running behind the Shell.
            try { m_host?.Close(); } catch { }
            m_externalWindowOpen = false;
            m_initialWindowStateApplied = false;
            ExternalWindowChanged?.Invoke(false);
            Visibility = Visibility.Collapsed;
            m_pointerInsideWindow = false;
            m_windowCommandTimer.Stop();
            m_viewportResizeTimer.Stop();
            m_windowCommandTimer.Interval = TimeSpan.FromMilliseconds(100);
            UpdateHostInput();
            AppState.Instance.SetFirefoxWindowState(false, false);
        }

        private void Minimize()
        {
            if (m_fullscreen) SetFullscreen(false);
            SetWindowActive(false);
            Visibility = Visibility.Collapsed;
            m_pointerInsideWindow = false;
            ExternalWindowChanged?.Invoke(false);
            m_windowCommandTimer.Stop();
            m_viewportResizeTimer.Stop();
            m_windowCommandTimer.Interval = TimeSpan.FromMilliseconds(100);
            UpdateHostInput();
            AppState.Instance.SetFirefoxWindowState(true, true);
        }

        private void Root_PointerPressed(object sender, PointerRoutedEventArgs e) =>
            AppState.Instance.ActivateFirefox();

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

        private void BrowserSurface_SizeChanged(object sender, SizeChangedEventArgs e)
        {
            if (m_host == null || !m_host.IsStarted)
            {
                EnsureHost();
                return;
            }

            // Keep only the final layout size. Fullscreen removes the border
            // and taskbar reservation separately, producing two sizes only a
            // few milliseconds apart on Xbox.
            m_viewportResizeTimer.Stop();
            m_viewportResizeTimer.Start();
        }

        private void ViewportResizeTimer_Tick(object sender, object e)
        {
            m_viewportResizeTimer.Stop();
            if (m_host?.IsStarted != true ||
                BrowserSurface.ActualWidth <= 0 || BrowserSurface.ActualHeight <= 0)
                return;

            try
            {
                m_host.SetViewport(BrowserSurface.ActualWidth, BrowserSurface.ActualHeight);
                UpdateGeckoScreen();
            }
            catch (Exception exception)
            {
                ShowHostFailure(exception);
            }
        }

        private void FirefoxWindow_Loaded(object sender, RoutedEventArgs e)
        {
            HookHostCanvas();
            ApplyInitialWindowState();
        }

        private void HookHostCanvas()
        {
            Canvas host = VisualTreeHelper.GetParent(this) as Canvas;
            if (host == null || ReferenceEquals(host, m_hookedHost)) return;
            if (m_hookedHost != null) m_hookedHost.SizeChanged -= HostCanvas_SizeChanged;
            if (m_hookedScreen != null) m_hookedScreen.SizeChanged -= ScreenRoot_SizeChanged;
            m_hookedHost = host;
            m_hookedHost.SizeChanged += HostCanvas_SizeChanged;
            m_hookedScreen = VisualTreeHelper.GetParent(host) as FrameworkElement;
            if (m_hookedScreen != null) m_hookedScreen.SizeChanged += ScreenRoot_SizeChanged;
        }

        private void HostCanvas_SizeChanged(object sender, SizeChangedEventArgs e)
        {
            if (m_maximized || m_fullscreen)
                ApplyMaximizedBounds();
        }

        private void ScreenRoot_SizeChanged(object sender, SizeChangedEventArgs e)
        {
            UpdateGeckoScreen();
        }

        private void ApplyInitialWindowState()
        {
            if (m_initialWindowStateApplied) return;
            SetMaximized(true);
            m_initialWindowStateApplied = m_maximized;
        }

        private void ApplyMaximizedBounds()
        {
            Canvas host = VisualTreeHelper.GetParent(this) as Canvas;
            if (host == null || host.ActualWidth <= 0 || host.ActualHeight <= 0) return;
            Canvas.SetLeft(this, 0);
            Canvas.SetTop(this, 0);
            Width = host.ActualWidth;
            Height = host.ActualHeight;
            UpdateResizeGripVisibility();
        }

        private void UpdateResizeGripVisibility()
        {
            Visibility visibility = (m_maximized || m_fullscreen)
                ? Visibility.Collapsed
                : Visibility.Visible;
            ResizeGripTopLeft.Visibility = visibility;
            ResizeGripTopRight.Visibility = visibility;
            ResizeGripBottomLeft.Visibility = visibility;
            ResizeGrip.Visibility = visibility;
        }

        private void SetMaximized(bool maximized)
        {
            if (m_maximized == maximized) return;

            Canvas host = VisualTreeHelper.GetParent(this) as Canvas;
            if (host == null) return;

            if (maximized)
            {
                m_restoreLeft = Canvas.GetLeft(this);
                m_restoreTop = Canvas.GetTop(this);
                m_restoreWidth = Width;
                m_restoreHeight = Height;
                m_maximized = true;
                ApplyMaximizedBounds();
            }
            else
            {
                Canvas.SetLeft(this, m_restoreLeft);
                Canvas.SetTop(this, m_restoreTop);
                Width = m_restoreWidth;
                Height = m_restoreHeight;
                m_maximized = false;
                UpdateResizeGripVisibility();
            }
        }

        private void UpdateGeckoScreen()
        {
            if (m_host == null) return;
            // A screen and a viewport are different things. Gecko uses the
            // screen rectangle to synchronously size its widget before it
            // announces DOM fullscreen; the viewport is the current virtual
            // DesktopMode window and is published separately by SetViewport.
            // Treating the current viewport as the screen forced a second
            // resize in the middle of the fullscreen/video transition.
            FrameworkElement screen = m_hookedScreen;
            if (screen == null && m_hookedHost != null)
                screen = VisualTreeHelper.GetParent(m_hookedHost) as FrameworkElement;

            double width = screen?.ActualWidth ?? 0;
            double height = screen?.ActualHeight ?? 0;
            if (width <= 0 || height <= 0)
            {
                width = BrowserSurface.ActualWidth;
                height = BrowserSurface.ActualHeight;
            }

            if (width > 0 && height > 0)
                m_host.SetScreen(width, height);
        }

        private Point CurrentPointerInHost(Canvas host)
        {
            Point rootPoint = new Point(m_mouseInput.Current.X, m_mouseInput.Current.Y);
            GeneralTransform toRoot = host.TransformToVisual(null);
            Point origin = toRoot.TransformPoint(new Point(0, 0));
            Point unit = toRoot.TransformPoint(new Point(1, 1));
            double scaleX = unit.X - origin.X;
            double scaleY = unit.Y - origin.Y;
            if (Math.Abs(scaleX) < 0.001 || Math.Abs(scaleY) < 0.001) return rootPoint;
            return new Point((rootPoint.X - origin.X) / scaleX, (rootPoint.Y - origin.Y) / scaleY);
        }

        private void BeginWindowDrag()
        {
            if (m_fullscreen || !m_mouseInput.Current.IsLeftButtonPressed) return;
            Canvas host = VisualTreeHelper.GetParent(this) as Canvas;
            if (host == null) return;

            Point pointer = CurrentPointerInHost(host);
            if (m_maximized)
            {
                double horizontalRatio = Width > 0
                    ? Math.Max(0, Math.Min(1, pointer.X / Width))
                    : 0.5;
                SetMaximized(false);
                Canvas.SetLeft(this, Math.Max(0, Math.Min(
                    host.ActualWidth - Width, pointer.X - Width * horizontalRatio)));
                Canvas.SetTop(this, Math.Max(0, pointer.Y - 18));
            }

            m_dragging = true;
            m_dragStartPointer = pointer;
            m_dragStartLeft = Canvas.GetLeft(this);
            m_dragStartTop = Canvas.GetTop(this);
        }

        private void EndWindowDrag()
        {
            m_dragging = false;
            m_windowCommandTimer.Interval = TimeSpan.FromMilliseconds(100);
        }

        private void MouseInput_InputChanged(object sender, MouseInputChangedEventArgs e)
        {
            if (e.Kind == MouseInputChangeKind.Released)
            {
                EndWindowDrag();
                return;
            }
            if (!m_dragging || e.Kind != MouseInputChangeKind.Moved) return;
            Canvas host = VisualTreeHelper.GetParent(this) as Canvas;
            if (host == null) return;
            Point point = CurrentPointerInHost(host);
            Canvas.SetLeft(this, Math.Max(0, m_dragStartLeft + point.X - m_dragStartPointer.X));
            Canvas.SetTop(this, Math.Max(0, m_dragStartTop + point.Y - m_dragStartPointer.Y));
        }

        private void SetFullscreen(bool fullscreen)
        {
            if (m_fullscreen == fullscreen) return;
            Canvas host = VisualTreeHelper.GetParent(this) as Canvas;
            if (host == null) return;

            if (fullscreen)
            {
                m_wasMaximizedBeforeFullscreen = m_maximized;
                m_restoreHostMargin = host.Margin;
                m_restoreHostZIndex = Canvas.GetZIndex(host);
                host.Margin = new Thickness(0);
                Canvas.SetZIndex(host, 40);
                host.UpdateLayout();
                SetMaximized(true);
                Width = host.ActualWidth;
                Height = host.ActualHeight;
                WindowBorder.BorderThickness = new Thickness(0);
                WindowBorder.CornerRadius = new CornerRadius(0);
                ResizeGripTopLeft.Visibility = Visibility.Collapsed;
                ResizeGripTopRight.Visibility = Visibility.Collapsed;
                ResizeGripBottomLeft.Visibility = Visibility.Collapsed;
                ResizeGrip.Visibility = Visibility.Collapsed;
            }
            else
            {
                host.Margin = m_restoreHostMargin;
                Canvas.SetZIndex(host, m_restoreHostZIndex);
                host.UpdateLayout();
                if (m_wasMaximizedBeforeFullscreen)
                {
                    m_maximized = true;
                    Width = host.ActualWidth;
                    Height = host.ActualHeight;
                    ApplyMaximizedBounds();
                }
                else
                {
                    SetMaximized(false);
                }
                WindowBorder.BorderThickness = new Thickness(1);
                WindowBorder.CornerRadius = new CornerRadius(4);
            }

            m_fullscreen = fullscreen;
            // Update this only after the fullscreen flag changes. Otherwise a
            // restored window keeps its resize grips hidden because the old
            // fullscreen state is still observed above.
            UpdateResizeGripVisibility();
            FullscreenChanged?.Invoke(fullscreen);
            // FullscreenChanged also hides or restores the Shell taskbar. Let
            // that final parent layout join the same coalesced viewport update
            // instead of publishing another intermediate screen rectangle.
            m_viewportResizeTimer.Stop();
            m_viewportResizeTimer.Start();
        }

        private void WindowCommandTimer_Tick(object sender, object e)
        {
            if (m_host == null || !m_host.IsStarted) return;

            bool externalOpen;
            try { externalOpen = m_host.IsExternalWindowOpen; }
            catch { externalOpen = false; }
            if (externalOpen != m_externalWindowOpen)
            {
                m_externalWindowOpen = externalOpen;
                ExternalWindowChanged?.Invoke(externalOpen);
            }

            int command;
            try
            {
                command = m_host.TakeWindowCommand();
            }
            catch
            {
                return;
            }

            switch (command)
            {
                case 1: Minimize(); break;
                case 2: SetMaximized(true); break;
                case 3: SetMaximized(false); break;
                case 4: Close(); break;
                case 5:
                    BeginWindowDrag();
                    if (m_dragging)
                        m_windowCommandTimer.Interval = TimeSpan.FromMilliseconds(16);
                    break;
                case 6:
                    EndWindowDrag();
                    break;
                case 7: SetFullscreen(true); break;
                case 8: SetFullscreen(false); break;
                case 9:
                    string path = m_host.TakeRevealPath();
                    if (!string.IsNullOrWhiteSpace(path))
                        AppState.Instance.RequestFilesReveal(path);
                    break;
                case 10:
                    string wallpaperPath = m_host.TakeWallpaperPath();
                    if (!string.IsNullOrWhiteSpace(wallpaperPath))
                        AppState.Instance.RequestWallpaper(wallpaperPath);
                    break;
            }
        }

        public UIElement ExternalContent => m_host?.ExternalContent as UIElement;

        public void SetExternalViewport(double width, double height)
        {
            if (width > 0 && height > 0)
                m_host?.SetExternalViewport(width, height);
        }

        public void SetExternalActive(bool active) => m_host?.SetExternalActive(active);

        public void RestoreExternalWindowIfOpen()
        {
            if (m_externalWindowOpen)
                ExternalWindowChanged?.Invoke(true);
        }

        public void CloseExternalWindow()
        {
            try { m_host?.CloseExternalWindow(); } catch { }
        }

        private void ResizeGrip_PointerPressed(object sender, PointerRoutedEventArgs e)
        {
            if (m_maximized || m_fullscreen) return;
            m_resizing = true;
            m_resizeCorner = WindowResize.CornerFromTag((sender as FrameworkElement)?.Tag);
            m_startPoint = e.GetCurrentPoint(null).Position;
            m_dragStartLeft = Canvas.GetLeft(this);
            m_dragStartTop = Canvas.GetTop(this);
            m_startWidth = Width;
            m_startHeight = Height;
            (sender as UIElement)?.CapturePointer(e.Pointer);
        }

        private void ResizeGrip_PointerMoved(object sender, PointerRoutedEventArgs e)
        {
            if (!m_resizing) return;
            Rect bounds = WindowResize.Calculate(m_resizeCorner, m_startPoint,
                e.GetCurrentPoint(null).Position,
                new Rect(m_dragStartLeft, m_dragStartTop, m_startWidth, m_startHeight), 640, 480);
            Canvas.SetLeft(this, bounds.X); Canvas.SetTop(this, bounds.Y);
            Width = bounds.Width; Height = bounds.Height;
        }

        private void ResizeGrip_PointerReleased(object sender, PointerRoutedEventArgs e)
        {
            m_resizing = false;
            (sender as UIElement)?.ReleasePointerCaptures();
        }
    }
}
