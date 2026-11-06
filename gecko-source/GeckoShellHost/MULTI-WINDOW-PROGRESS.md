# Implementation progress

The integrated browser still has one Shell window. Multiple browser instances
and Strawfox window Web Apps are not enabled yet. No build was run.

Backups before edits: `Backups/Strawfox-multiwindow-2026-11-02/manifest.json`.
Engine edits are in `C:/firefox-source/engine/firefox`, outside this repository.

## Implemented in engine source

- Native HeadlessWidget assigns an ID on Create and unregisters on Destroy.
  Compositor recreation reuses this native window ID instead of allocating one.
- Registry v2 enumerates non-overlay compositor window IDs.
- Panel binding retains the COM reference per native ID until native Destroy,
  across compositor recreation. Binding does not allow changing an existing
  panel. COM release happens outside the frame mutex.
- EGL creation resolves the panel from the compositor/widget key. Unbound
  legacy windows retain their previous single-panel path.
- Per-ID resize, activation and RequestWindowClose commands resolve their
  target on Gecko's main thread and retain it across listener dispatch.
- Per-ID mouse, wheel, physical key and composition/text exports resolve their
  target in the input queue. Closed IDs are dropped instead of falling back to
  the active window. Legacy key/text entry points share the same implementation.

## Still required before enabling multiple hosts

- Browser creation with an explicit request ID and panel bound before compositor
  creation. Do not infer ownership by choosing whichever widget appears next.
- Registry readiness/closed/failure acknowledgements and capability negotiation.
- Isolate popup/frame composition, software fallback and ANGLE panel size,
  capacity/presentation/video callbacks (these still include global state).
- Route cursor, fullscreen, pointer-lock, text-focus and window commands by ID.
- Per-ID relative mouse, touch, file drag, URL navigation and download actions.
- Runtime-owned brokers and host subscriptions with independent view lifetimes.
- Shell window collection, taskbar/Task View integration and Web App launch mode
  migration that preserves the current Strawfox-tab behavior for existing apps.
- Build and runtime validation on Xbox, including GPU resize, window switching,
  independent closing, keyboard, fullscreen and simultaneous windows.

Registry version 2 only advertises the registry/command foundation, not complete
multi-window support. No existing host should create a second EngineView yet.

## Host and ANGLE stages

- Host GeckoHost.CreateBrowserWindow returns an independent GeckoWindow object with
  explicit input and lifecycle APIs; Shell controls do not use it yet.
- ANGLE size, composition scale, capacity and source-size transforms now use
  per-panel snapshots for managed windows, retaining the legacy defaults for
  the existing primary browser. The host publishes before native creation.
- NativeWindow11WinRT keeps its own metrics state alive while rendering, even
  after the registry entry is retired. Closing views release the registry entry
  after native destruction and retain the UI panel until that acknowledgement.
- Matching xul.dll, libGLESv2.dll and host binaries are required. Missing exports
  fail explicitly rather than opening a second view using singleton functions.
- Popup/frame ownership, fullscreen/cursor/pointer-lock/video callbacks,
  remaining per-ID input/navigation APIs and Shell/Web App UI remain pending.
