# Host window-session API (work in progress)

`GeckoHost.CreateBrowserWindow(url, appMode)` now returns a `GeckoWindow`, sharing
the already-started XRE runtime. It does not construct another EngineView or
replace the primary host's global callbacks. Old engines missing the required
exports fail with a capability error instead of using single-window exports.

This API is not yet connected to the Shell UI or Web App installation flow.
Do not enable it there until popup composition,
pointer-lock/fullscreen/cursor routing and software fallback are complete.

## View lifecycle

1. Attach `Content` to the intended Shell window.
2. Arrange the content, then call `SetViewport` with its local logical size.
   The request waits for nonzero arranged panel dimensions. Pixels use the
   display scale; callers must not apply the desktop transform again.
3. Poll `State`/`Error` asynchronously, without blocking the XAML dispatcher.
   State 2 acknowledges native window creation, not first-frame presentation
   or page load. Activate the session only once it is ready.
4. Route pointer coordinates local to Content, physical key transitions and
   committed Unicode text through SendMouse/Wheel/Key/Text. The Shell adapter
   must avoid sending text twice through keypress and text composition.
5. Deactivate the previous session before activating the next. Key and mouse
   button releases are sent to the old ID so held inputs cannot leak between
   windows. Keyboard and IME/virtual-keyboard handlers remain to be connected.
6. Poll TakeWindowCommand on the owning dispatcher: 1 minimize, 2 maximize,
   3 restore, 4 close. These commands belong only to that session.
7. Close requests native shutdown/cancellation. It does not close the primary
   browsing session or terminate XRE. The view must not report a confirmed
   close until State is terminal. Native beforeunload handling can delay or
   cancel actual shutdown; this still needs a Shell confirmation flow.

State values: 0 not requested/unknown, 1 pending, 2 created, 3 closed,
4 failed, 5 failed creation still tearing down its native widget. State 5
retains the UI panel until native Destroy completes. Retiring finalizers keep
a panel reference on its dispatcher and release the native request record
only in a terminal state. The retirement timer runs only while it has entries.

No build or Xbox runtime validation has been run.

Per-panel ANGLE metrics are now connected: the host publishes size, scale and
retained capacity before creation. Each native surface retains its own metrics
state across updates/unregistration. Legacy surfaces retain their old defaults.
The packaged libGLESv2.dll must implement window-metrics ABI v1.
