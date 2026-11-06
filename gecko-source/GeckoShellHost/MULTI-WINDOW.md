# Integrated Strawfox: multiple windows

Status: source investigation and implementation design. Multi-window support is
not implemented. Do not enable a window Web App option against the current ABI.

## Ownership

Use one Gecko/XRE runtime and profile per Shell process. Each Shell browser or
Web App window owns a different Gecko top-level browser window, identified by a
monotonically assigned ID. IDs must not be reused during the process lifetime.
Closing one window must close its DOM browser window, not reset the shared
session or terminate XRE. Profile cookies and login sessions remain shared.

Normal Strawfox windows retain Firefox chrome and tabs. Web App windows use
Desktop Mode caption controls and hide Firefox navigation/tab chrome. Both must
participate in Task View, taskbar, activation, minimize and maximize independently.

## Current singleton boundaries verified in source

- `Controls/FirefoxWindow.xaml.cs`: `s_current`, static drag target, singleton
  lifetime, AppState activation/fullscreen and pending URL dispatch.
- `Views/MainDesktop.xaml.cs`: one FirefoxWindow, one Task View identity and one
  external window surface. Existing browser input suppression is global.
- `Services/WebApps/WindowManagerService.cs`: UseStrawfox bypasses the window
  collection and calls RequestFirefoxUrl, creating a tab.
- `GeckoHost.cpp`: g_runtimeStarted, global command queue, reveal/wallpaper
  paths and process-wide callbacks. These cannot belong to each new host.
- `client/EngineView.cpp`: global clipboard/UI callbacks, pointer-lock state,
  keyboard and mouse subscriptions, main/external frame exports.
- Engine `widget/headless/HeadlessWidget.cpp`: sShownWidget, sExternalWidget,
  global text-input state and popup collection.
- Engine `widget/headless/HeadlessCompositorWidget.cpp`: sPanel,
  gecko_w10m_egl_native_window(), sMainWidget/sMain, shared front buffer and
  frame serial. The current surface cannot be retargeted between live windows.

## Required native ABI

Introduce a versioned multi-window ABI alongside the existing single-window
exports. The existing host must keep using its current path until both sides
implement the complete new ABI. Probe the version/capabilities before opening
a second window; never silently route to single-window exports.

Every window operation carries a window ID: create/open URL, bind panel,
resize/DPI, activate, pointer/button/wheel/relative mouse, keyboard/text,
drag/drop and close. Creation is asynchronous and acknowledges the ID only
after Gecko has created the associated top-level widget.

Callbacks carry the originating ID: ready, title, icon, window commands,
fullscreen, pointer lock, cursor, text-input state and closed. Deliver ordered
events through a runtime-owned queue, then dispatch by ID on the XAML thread.
Ignore events for closed IDs. Do not hold the queue lock while invoking Gecko
or XAML. A window's shutdown must release its input handlers and graphics
references after in-flight rendering has stopped using them.

Gecko objects are accessed on the Gecko main thread. XAML objects are accessed
on their dispatcher. The compositor/render thread uses lifetime-safe graphics
bindings associated with its compositor widget, not a mutable global panel.

## Rendering and popup constraints

Retain GPU rendering for each top-level window. Extend EGL/ANGLE native-window
binding to resolve the panel belonging to the compositor's window ID. Review
all other process-global ANGLE panel size/capacity/present callbacks at the same
time. A software capture or a shared surface reparented between windows is not
an equivalent implementation.

Small Firefox menus and tooltips belong to their originating window, including
their frame buffers and input transforms. Browser/site-created top-level windows
must enter the registry without overwriting another browser's widget. Keep the
existing site-popup-to-tab policy unless explicitly changed.

Fullscreen and pointer lock belong to the active window only. Switching windows
releases pointer lock. Escape first unlocks the pointer; a later Escape can leave
fullscreen. Closing/minimizing a fullscreen window restores Shell UI reliably.

## Shell integration and migration

Use a window collection/model rather than duplicating AppState singleton flags.
Assign each instance a stable identity for the window coordinator and Task View.
Adapt taskbar grouping and Start's New window command to that collection.

Persist Web App launch mode explicitly: WebView2 window, Strawfox tab (existing
UseStrawfox behavior), or Strawfox window. Migrate existing saved records without
changing their behavior. A Strawfox window uses its installed name/favicon and
its own model while sharing the runtime service with ordinary browser windows.

## Validation before enabling

- Two ordinary windows and two different Web Apps visible simultaneously.
- Independent GPU frames, DPI/resize, keyboard, mouse, menus and file uploads.
- Closing any instance leaves all remaining instances usable; reopening works.
- Task View, taskbar grouping, z-order and minimizing restore the intended ID.
- Fullscreen/pointer-lock/escape sequence does not affect another instance.
- Authorized downloads, Show in folder, wallpaper, clipboard and microphone
  permissions continue through the runtime-owned brokers.
- Engine/host ABI mismatch reports a clear capability error without creating a
  second single-window host or disturbing the existing window.

Builds and Xbox validation remain user-run.
