# CUI terminal frontend

RuntimeSession.IsConsoleApplication reads the prepared PE Subsystem. The shell
creates a local UWP WebView for CUI applications; GUI applications keep the GDI
surface. The terminal uses pinned xterm 4.18.0 and fit addon 0.5.0, distributed
under their included MIT licenses. No FluentTerminal source was incorporated.
Assets are bundled locally and no CDN, host process, ConPTY or WinPTY is used.

RuntimeSession exposes input, output, input mode and dimensions. GuestRuntime
publishes a separate atomic shared kernel snapshot after console startup, so
UI polling does not race kernel construction. Output delivery is serialized;
failed script calls keep the drained batch for retry. Startup input is queued
until the console exists. Calls into the WebView pass script arguments rather
than concatenating guest output into JavaScript code. CUI native GDI input is
disabled to avoid delivering keystrokes to both backends.

The renderer fits its cells to window resize and reports columns/rows to the
shared console session. Input supports basic cooked lines with backspace and
paste, or raw characters/sequences when LINE_INPUT is disabled. ECHO_INPUT
controls local echo. Completed CUI processes retain output and an exit marker
until their window closes. Unload/close stops polling and releases the WebView.

Core screen-buffer/cursor/attribute/fill/scroll APIs now update an in-memory
cell grid and emit corresponding terminal sequences. Processed cooked Ctrl+C
clears the frontend line and queues a process-local control event; console
reads invoke handlers without holding the console lock. This is cooperative
control delivery, not full asynchronous process-group signaling.

Not complete: console resize INPUT_RECORD events, full VT parsing and cooked
line navigation/history, title synchronization and new-console child window
creation. This frontend does not establish that a specific cmd.exe version can
run. WebView lifecycle, focus, fonts, long output and keyboard input need testing
on Xbox. No C++/C# build or runtime execution was performed by the agent.

Vendored asset SHA-256:
{
  "fit.js": "f120d81c79b8069bbc362f9bf186f8724980a1c21afb3460f781e94e6db06bd1",
  "LICENSE-fit": "e256f01188af527e4d06d21d06fbf785ae9c50d4b328bf03cbe0ba7f0aa4228f",
  "LICENSE-xterm": "b569f629d00f2626a8100df2a1798210535621e42164dfd426a6fe5aac7b0ccd",
  "xterm.css": "fead161ee663305de6c070b2bab9a97ff55ae16ad981482563dbd3294e9f61ef",
  "xterm.js": "da9065ac3b6465fc00feed6c92f9bb45a44fce6e62d6c59577765104bdd6b582"
}

Static validation passed for project/XAML XML, asset inclusion, source delimiters
and diff whitespace. Test-ConsoleFrontend.js passed isolated mock transport
contracts for readiness/dimensions, editing, paste CRLF, cooked escape filtering,
raw input, echo/disabled input, literal output and resize. node --check passed
for terminal.js. These checks do not execute xterm rendering or UWP WebView.
