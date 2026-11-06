# Shell integration: Strawfox app windows

The Start install dialog now has three modes: Edge WebView2, Strawfox app (tab),
and Strawfox app window. Existing saved apps retain their current mode because
the new UseStrawfoxWindow field defaults to false. Window-mode entries use the
existing installed-app favicon download, pinning and taskbar/Task View models.

WindowManagerService only routes tab-mode Strawfox apps to RequestFirefoxUrl.
Window-mode apps enter the normal window collection. WebAppWindow lazily creates
the chosen backend; it does not instantiate a WebView2 control for Strawfox.
Each StrawfoxAppView requests its own native GeckoWindow from the shared host.
Start's New window action can therefore create multiple models for the same app.

The shared runtime is warmed up with the primary browser view transparent and
without input if the user has not opened Strawfox. Warmup is serialized. Once
the session object is created, the primary view returns to its previous hidden
state without becoming a taskbar entry. Pending native requests remain separate
from the Shell window and are cancelled if the window closes during startup.

Native readiness now waits for browser delayed startup. The host confirms the
viewport once after readiness so initial XUL layout cannot keep the window at
an unrelated size. Per-panel ANGLE metrics are published before creation.

Mouse, physical keys, committed text and virtual-keyboard focus are scoped to
the active model. Shell overlays suppress input. The virtual keyboard is only
requested with a gamepad connected and no physical keyboard. Native cursor
visibility uses the Shell's existing cursor lease.

Normal close requests leave the Shell model present until native destruction
is confirmed, including the Task View and taskbar close actions. Closing the
primary browser does not background Gecko while managed windows remain alive.

Scope: browser popup behavior is unchanged at the user's request. DOM fullscreen
in these Web App windows stays within their Shell-owned viewport, avoiding the
primary browser's global fullscreen callback. Per-window relative pointer lock,
file drag/upload integration and full-desktop fullscreen are not added here.
Each view also supports the software-rendered root frame for its native ID.
Only software views run a 33 ms frame timer; unchanged frames are not copied.
The timer stops when minimized or disposed. A confirmed GPU presentation hides
and releases the software bitmap. This does not compose or isolate popups.

Backups: Backups/Strawfox-multiwindow-2026-11-02/stage-shell/manifest.json.
Validation performed: C# syntax parsing, XAML/project XML parsing, JS syntax and
diff checks. No build or Xbox runtime test was performed. Testing requires
matching rebuilt engine/ANGLE and host/Shell binaries.
