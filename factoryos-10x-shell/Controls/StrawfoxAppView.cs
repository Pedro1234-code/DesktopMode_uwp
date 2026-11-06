using DesktopMode.Gecko;
using factoryos_10x_shell.Library.Models.InternalData;
using factoryos_10x_shell.Library.Services.WebApps;
using System;
using System.Threading.Tasks;
using Windows.Devices.Input;
using Windows.Foundation;
using Windows.System;
using Windows.UI.Core;
using Windows.UI.Input;
using Windows.UI.ViewManagement;
using Windows.UI.Xaml;
using Windows.UI.Xaml.Controls;
using Windows.UI.Xaml.Media;
using factoryos_10x_shell.Services.Helpers;
using factoryos_10x_shell.Services.WebApps;

namespace factoryos_10x_shell.Controls
{
    // One view per window model. The native session owns a different Gecko
    // top-level window while the primary host owns the shared runtime.
    internal sealed class StrawfoxAppView : Grid, IDisposable
    {
        private readonly WebAppWindowModel model;
        private readonly IWindowManagerService manager;
        private readonly DispatcherTimer timer = new DispatcherTimer { Interval = TimeSpan.FromMilliseconds(100) };
        private readonly DispatcherTimer frameTimer = new DispatcherTimer { Interval = TimeSpan.FromMilliseconds(33) };
        private readonly TextBox textSink = new TextBox {
            Width = 1, Height = 1, Opacity = 0, IsHitTestVisible = false,
            HorizontalAlignment = HorizontalAlignment.Left, VerticalAlignment = VerticalAlignment.Top,
            IsSpellCheckEnabled = false, IsTextPredictionEnabled = false,
            PreventKeyboardDisplayOnProgrammaticFocus = true
        };
        private readonly bool[] buttons = new bool[3];
        private readonly bool[] keys = new bool[256];
        private CoreWindow core;
        private Views.MainDesktop desktop;
        private bool popupSuppressed;
        private bool nativeCursorRequested;
        private bool viewportSettled;
        private readonly Func<bool> requestClose;
        private GeckoWindow session;
        private bool disposed, hooked, starting, active, clearing, characterPending, wantsText;
        private uint textState;
        private long textRevision, pendingRevision;
        private DateTimeOffset deadline;
        private uint lastLoggedState = uint.MaxValue;
        private DateTimeOffset nextPendingLog;
        private double loggedWidth = -1, loggedHeight = -1;
        private bool? softwareFrames;
        private Point contextMenuPoint;
        private MenuFlyout contextMenu;
        private void ShowContextMenu(int flags)
        {
            contextMenu?.Hide();
            var menu = new MenuFlyout();
            contextMenu = menu;
            Action<string, string> add = (label, action) => {
                var item = new MenuFlyoutItem { Text = label };
                item.Click += (sender, args) => session?.ContextAction(action);
                menu.Items.Add(item);
            };
            if ((flags & 1) != 0) add("Copy", "copy");
            if ((flags & 2) != 0) add("Paste", "paste");
            if ((flags & 4) != 0) add("Save image as", "saveimage");
            if (menu.Items.Count == 0) return;
            menu.ShowAt(this, new Windows.UI.Xaml.Controls.Primitives.FlyoutShowOptions {
                Position = contextMenuPoint
            });
        }
        private void Log(string stage, string detail = "") => WebAppDiagnostics.Write(model, stage, detail);
        internal event Action<Exception> Failed;

        internal StrawfoxAppView(WebAppWindowModel model, IWindowManagerService manager)
        {
            this.model = model;
            this.manager = manager;
            Log("strawfox-view-created");
            requestClose = TryRequestClose;
            model.RequestNativeClose = requestClose;
            Background = new SolidColorBrush(Windows.UI.Colors.Transparent);
            Children.Add(textSink);
            textSink.TextChanging += TextChanging;
            Loaded += LoadedView;
            Unloaded += UnloadedView;
            SizeChanged += ViewSizeChanged;
            timer.Tick += Tick;
            frameTimer.Tick += FrameTick;
        }

        private async void LoadedView(object sender, RoutedEventArgs args)
        {
            timer.Start();
            if (session != null || starting || disposed) return;
            starting = true;
            Log("host-warmup-start");
            try
            {
                DependencyObject ancestor = this;
                while (ancestor != null && !(ancestor is Views.MainDesktop))
                    ancestor = VisualTreeHelper.GetParent(ancestor);
                if (!(ancestor is Views.MainDesktop owner)) throw new InvalidOperationException("The browser window is not attached to the desktop.");
                desktop = owner;
                var created = await desktop.CreateStrawfoxAppWindowAsync(model.App.StartUri);
                if (disposed) { created.Close(); return; }
                session = created;
                Log("host-session-created", "nativeId=" + session.Id);
                if (!(session.Content is UIElement content)) throw new InvalidOperationException("Strawfox did not provide a window surface.");
                Children.Insert(0, content);
                Log("surface-attached", $"viewport={ActualWidth:F0}x{ActualHeight:F0}");
                deadline = DateTimeOffset.UtcNow.AddSeconds(30);
                await Dispatcher.RunAsync(CoreDispatcherPriority.Low, () => {
                    if (disposed) return;
                    UpdateLayout();
                    UpdateViewport();
                });
            }
            catch (Exception error) { Fail(error); }
            finally { starting = false; }
        }

        private void UnloadedView(object sender, RoutedEventArgs args)
        {
            timer.Stop();
            frameTimer.Stop();
            Log("view-unloaded");
            SetInactive();
            UnhookInput();
        }
        private void ViewSizeChanged(object sender, SizeChangedEventArgs args)
        {
            if (session != null && !disposed) UpdateViewport();
        }

        private void UpdateViewport()
        {
            try {
                bool initial = session?.Id == 0;
                if (ActualWidth > 0 && ActualHeight > 0) session?.SetViewport(ActualWidth, ActualHeight);
                if (ActualWidth != loggedWidth || ActualHeight != loggedHeight || initial && session?.Id > 0)
                {
                    Log("viewport", $"nativeId={session?.Id} size={ActualWidth:F0}x{ActualHeight:F0}");
                    loggedWidth = ActualWidth; loggedHeight = ActualHeight;
                }
                if (initial && session?.Id > 0) deadline = DateTimeOffset.UtcNow.AddSeconds(30);
            }
            catch (Exception error) { Fail(error); }
        }

        private void Tick(object sender, object args)
        {
            if (disposed || session == null) return;
            try
            {
                uint state = session.State;
                if (state != lastLoggedState)
                {
                    Log("native-state", $"nativeId={session.Id} state={state} error=0x{session.Error:X8}");
                    lastLoggedState = state;
                }
                if (state == 0 || state == 1)
                {
                    if (DateTimeOffset.UtcNow >= nextPendingLog)
                    {
                        Log("creation-pending", $"nativeId={session.Id} state={state} loaded={IsLoaded} visible={model.Visibility} " +
                            $"size={ActualWidth:F0}x{ActualHeight:F0} remainingMs={(deadline - DateTimeOffset.UtcNow).TotalMilliseconds:F0}");
                        nextPendingLog = DateTimeOffset.UtcNow.AddSeconds(5);
                    }
                    UpdateViewport();
                    if (!disposed && DateTimeOffset.UtcNow > deadline)
                        Fail(new TimeoutException("Strawfox could not create the app window in time (id=" + session.Id +
                            ", state=" + state + ", viewport=" + ActualWidth + "x" + ActualHeight + ")."));
                    return;
                }
                if (state == 3) { model.RequestNativeClose = null; manager.Close(model); return; }
                if (state == 4 || state == 5) { Fail(new InvalidOperationException("Strawfox window creation failed (0x" + session.Error.ToString("X8") + ").")); return; }
                if (!viewportSettled) { UpdateLayout(); UpdateViewport(); viewportSettled = true; Log("window-ready", "nativeId=" + session?.Id); }
                if (disposed) return;
                HookInput();
                UpdateSoftwareFrame();
                popupSuppressed = model.IsActive && VisualTreeHelper.GetOpenPopups(Window.Current).Count != 0;
                UpdateActivation();
                for (int i = 0; i < 16; i++)
                {
                    int command = session.TakeWindowCommand();
                    if (command == 0) break;
                    if (command >= 100 && command <= 107) { ShowContextMenu(command - 100); continue; }
                    Log("native-command", "command=" + command);
                    if (command == 1) manager.Minimize(model);
                    else if (command == 2 && !model.IsMaximized || command == 3 && model.IsMaximized) manager.ToggleMaximize(model);
                    else if (command == 4) { manager.Close(model); return; }
                }
                uint focus = active ? session.TextInputState : 0;
                wantsText = (focus & 1) != 0;
                if (focus != textState)
                {
                    textState = focus;
                    if (wantsText)
                    {
                        bool physical = new KeyboardCapabilities().KeyboardPresent != 0;
                        bool show = !physical && Windows.Gaming.Input.Gamepad.Gamepads.Count > 0;
                        textSink.PreventKeyboardDisplayOnProgrammaticFocus = !show;
                        textSink.Focus(FocusState.Programmatic);
                        if (show) InputPane.GetForCurrentView().TryShow();
                    }
                }
            }
            catch (Exception error) { Fail(error); }
        }

        private bool TryRequestClose()
        {
            if (disposed || session?.State != 2) return false;
            if (!model.IsActive || model.Visibility != Visibility.Visible) manager.Activate(model);
            session.RequestClose();
            return true;
        }
        private void FrameTick(object sender, object args)
        {
            if (disposed || session == null || model.Visibility != Visibility.Visible) { frameTimer.Stop(); return; }
            try { UpdateSoftwareFrame(); }
            catch (Exception error) { Fail(error); }
        }
        private void UpdateSoftwareFrame()
        {
            bool software = model.Visibility == Visibility.Visible && session.UpdateFrame();
            if (model.Visibility == Visibility.Visible && softwareFrames != software)
            {
                softwareFrames = software;
                Log("frame-source", software ? "software" : "gpu-or-awaiting-first-frame");
            }
            if (software)
            {
                if (!frameTimer.IsEnabled) frameTimer.Start();
            }
            else frameTimer.Stop();
        }
        internal void UpdateActivation()
        {
            if (disposed) return;
            if (IsLoaded && model.Visibility == Visibility.Visible) { if (!timer.IsEnabled) timer.Start(); }
            else { timer.Stop(); frameTimer.Stop(); }
            bool next = model.IsActive && model.Visibility == Visibility.Visible && !popupSuppressed &&
                desktop?.IsStrawfoxInputBlocked != true;
            // Model changes can arrive repeatedly during startup. A pending
            // request cannot own input yet; log only effective transitions.
            if (next && session?.State != 2) next = false;
            if (next == active) return;
            Log("activation", $"requested={next} modelActive={model.IsActive} visible={model.Visibility} popupBlocked={popupSuppressed} shellBlocked={desktop?.IsStrawfoxInputBlocked}");
            if (!next) SetInactive();
            else { active = true; textSink.IsEnabled = true; session.SetActive(true); }
        }

        private void SetInactive()
        {
            if (!active && !wantsText && !nativeCursorRequested && !textSink.IsEnabled) return;
            if (active) session?.SetActive(false);
            active = wantsText = characterPending = false;
            SetNativeCursor(false);
            textState = 0;
            Array.Clear(keys, 0, keys.Length);
            Array.Clear(buttons, 0, buttons.Length);
            if (textSink.FocusState != FocusState.Unfocused)
            {
                try { InputPane.GetForCurrentView().TryHide(); } catch { }
            }
            textSink.IsEnabled = false;
        }

        private void HookInput()
        {
            if (hooked || disposed) return;
            core = Window.Current.CoreWindow;
            core.PointerPressed += OnCorePointerPressed;
            core.PointerReleased += OnCorePointerReleased;
            core.PointerMoved += OnCorePointerMoved;
            core.PointerWheelChanged += OnCorePointerWheel;
            core.KeyDown += OnCoreKeyDown;
            core.KeyUp += OnCoreKeyUp;
            core.CharacterReceived += OnCoreCharacterReceived;
            hooked = true;
        }
        private void UnhookInput()
        {
            if (!hooked) return;
            core.PointerPressed -= OnCorePointerPressed;
            core.PointerReleased -= OnCorePointerReleased;
            core.PointerMoved -= OnCorePointerMoved;
            core.PointerWheelChanged -= OnCorePointerWheel;
            core.KeyDown -= OnCoreKeyDown;
            core.KeyUp -= OnCoreKeyUp;
            core.CharacterReceived -= OnCoreCharacterReceived;
            hooked = false;
        }
        private bool TryPointerPoint(PointerEventArgs args, out Point local)
        {
            local = default(Point);
            if (disposed || args.CurrentPoint.PointerDevice.PointerDeviceType != PointerDeviceType.Mouse) return false;
            try {
                var inverse = TransformToVisual(null).Inverse;
                if (inverse == null) return false;
                local = inverse.TransformPoint(args.CurrentPoint.Position);
                return local.X >= 0 && local.Y >= 0 && local.X < ActualWidth && local.Y < ActualHeight;
            } catch { return false; }
        }
        private void OnCorePointerPressed(CoreWindow sender, PointerEventArgs args)
        {
            popupSuppressed = VisualTreeHelper.GetOpenPopups(Window.Current).Count != 0;
            if (!TryPointerPoint(args, out Point p) || session?.State != 2 || popupSuppressed || desktop?.IsStrawfoxInputBlocked == true) return;
            if (!OwnsPointer(args.CurrentPoint.Position)) return;
            if (!model.IsActive) manager.Activate(model);
            UpdateActivation();
            if (!active) return;
            var properties = args.CurrentPoint.Properties;
            bool[] down = { properties.IsLeftButtonPressed, properties.IsMiddleButtonPressed, properties.IsRightButtonPressed };
            for (int i = 0; i < 3; i++) if (down[i] && !buttons[i]) { buttons[i] = true; session.SendMouse(1, i, p.X, p.Y); }
        }
        private bool OwnsPointer(Point position)
        {
            // CoreWindow events also reach views obscured by another window.
            // Only the topmost hit may activate a browser and receive its press.
            foreach (var hit in VisualTreeHelper.FindElementsInHostCoordinates(position, Window.Current.Content))
            {
                DependencyObject current = hit;
                while (current != null)
                {
                    if (ReferenceEquals(current, this)) return true;
                    current = VisualTreeHelper.GetParent(current);
                }
                return false;
            }
            return false;
        }
        private void OnCorePointerReleased(CoreWindow sender, PointerEventArgs args)
        {
            if (args.CurrentPoint.PointerDevice.PointerDeviceType != PointerDeviceType.Mouse) return;
            UpdateActivation();
            if (!active) return;
            TryPointerPoint(args, out Point p);
            if (buttons[2]) contextMenuPoint = p;
            var properties = args.CurrentPoint.Properties;
            bool[] down = { properties.IsLeftButtonPressed, properties.IsMiddleButtonPressed, properties.IsRightButtonPressed };
            for (int i = 0; i < 3; i++) if (!down[i] && buttons[i]) { buttons[i] = false; session.SendMouse(2, i, p.X, p.Y); }
        }
        private void OnCorePointerMoved(CoreWindow sender, PointerEventArgs args)
        {
            if (!model.IsActive) { SetInactive(); return; }
            bool inside = TryPointerPoint(args, out Point p);
            UpdateActivation();
            SetNativeCursor(active && inside);
            if (active && (inside || buttons[0] || buttons[1] || buttons[2])) session.SendMouse(0, 0, p.X, p.Y);
        }
        private void OnCorePointerWheel(CoreWindow sender, PointerEventArgs args)
        {
            if (!model.IsActive) return;
            bool inside = TryPointerPoint(args, out Point p);
            UpdateActivation();
            if (!active || !inside) return;
            var props = args.CurrentPoint.Properties;
            session.SendWheel(p.X, p.Y, props.IsHorizontalMouseWheel ? props.MouseWheelDelta : 0,
                props.IsHorizontalMouseWheel ? 0 : props.MouseWheelDelta);
        }
        private uint Modifiers()
        {
            uint result = 0;
            if ((core.GetKeyState(VirtualKey.Shift) & CoreVirtualKeyStates.Down) != 0) result |= 1;
            if ((core.GetKeyState(VirtualKey.Control) & CoreVirtualKeyStates.Down) != 0) result |= 2;
            if ((core.GetKeyState(VirtualKey.Menu) & CoreVirtualKeyStates.Down) != 0) result |= 4;
            if (((core.GetKeyState(VirtualKey.LeftWindows) | core.GetKeyState(VirtualKey.RightWindows)) & CoreVirtualKeyStates.Down) != 0) result |= 8;
            return result;
        }
        private void SetNativeCursor(bool requested)
        {
            if (nativeCursorRequested == requested) return;
            nativeCursorRequested = requested;
            MainPage.SetPopupCursorVisibility(requested);
        }
        private static bool NonText(int code) => code < 32 || code >= 33 && code <= 46 || code >= 112 && code <= 135;
        private void OnCoreKeyDown(CoreWindow sender, KeyEventArgs args)
        {
            if (!model.IsActive) return;
            popupSuppressed = VisualTreeHelper.GetOpenPopups(Window.Current).Count != 0;
            UpdateActivation();
            if (!active) return;
            int code = (int)args.VirtualKey;
            if (code < 0 || code >= keys.Length) return;
            uint mods = Modifiers();
            wantsText = (session.TextInputState & 1) != 0;
            if (wantsText && textSink.FocusState == FocusState.Unfocused)
                textSink.Focus(FocusState.Programmatic);
            characterPending = false;
            if (wantsText && (mods & 14) == 0 && !NonText(code))
            {
                pendingRevision = textRevision;
                characterPending = args.KeyStatus.ScanCode != 0;
                return;
            }
            session.SendKey(true, code, mods, args.KeyStatus.WasKeyDown);
            keys[code] = true;
            args.Handled = true;
        }
        private void OnCoreKeyUp(CoreWindow sender, KeyEventArgs args)
        {
            UpdateActivation();
            int code = (int)args.VirtualKey;
            if (!active || code < 0 || code >= keys.Length || !keys[code]) return;
            keys[code] = false;
            session.SendKey(false, code, Modifiers(), false);
            args.Handled = true;
        }
        private void OnCoreCharacterReceived(CoreWindow sender, CharacterReceivedEventArgs args)
        {
            if (!characterPending) return;
            characterPending = false;
            if (!active || !wantsText || (Modifiers() & 14) != 0 || args.KeyStatus.ScanCode == 0) return;
            if (args.KeyCode < 32 || args.KeyCode == 127 || args.KeyCode > 0x10ffff) return;
            if (pendingRevision == textRevision)
            {
                textRevision++;
                session.SendText(args.KeyCode <= 0xffff
                    ? ((char)args.KeyCode).ToString() : char.ConvertFromUtf32((int)args.KeyCode));
            }
            args.Handled = true;
        }
        private void TextChanging(TextBox sender, TextBoxTextChangingEventArgs args)
        {
            if (clearing || sender.Text.Length == 0) return;
            string text = sender.Text;
            textRevision++;
            if (active && wantsText) session.SendText(text);
            clearing = true;
            sender.Text = string.Empty;
            clearing = false;
        }
        private void Fail(Exception error)
        {
            if (disposed) return;
            Log("failed", $"type={error.GetType().Name} hresult=0x{error.HResult:X8} nativeId={session?.Id} state={lastLoggedState}");
            Dispose();
            Failed?.Invoke(error);
        }
        public void Dispose()
        {
            if (disposed) return;
            Log("dispose-start", "nativeId=" + session?.Id);
            contextMenu?.Hide();
            contextMenu = null;
            disposed = true;
            if (ReferenceEquals(model.RequestNativeClose, requestClose)) model.RequestNativeClose = null;
            timer.Stop();
            timer.Tick -= Tick;
            frameTimer.Stop();
            frameTimer.Tick -= FrameTick;
            Loaded -= LoadedView;
            Unloaded -= UnloadedView;
            SizeChanged -= ViewSizeChanged;
            textSink.TextChanging -= TextChanging;
            UnhookInput();
            // Detach callbacks first: native cleanup must not leave this view
            // consuming CoreWindow input if a WinRT call fails during teardown.
            try { SetInactive(); }
            catch (Exception error) { System.Diagnostics.Debug.WriteLine("Strawfox input cleanup: " + error.HResult.ToString("X8")); }
            finally
            {
                active = wantsText = characterPending = false;
                SetNativeCursor(false);
            }
            try { session?.Close(); }
            catch (Exception error) { System.Diagnostics.Debug.WriteLine("Strawfox session cleanup: " + error.HResult.ToString("X8")); }
            session = null;
            Children.Clear();
            Log("dispose-complete", "inputDetached=" + !hooked);
        }
    }
}
