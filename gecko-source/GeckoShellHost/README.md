# Embedded Firefox host

This Windows Runtime Component is the DesktopMode-owned copy of the native
host from `C:\firefox-source\app_ShellIntegration\GeckoW10m`.

Only the engine-facing host was brought over: `EngineView`, the Gecko
bootstrap, logging, download/file-picker brokerage, DRM bridge and video
overlay support. The standalone UWP application (`App.cpp`, `MainPage.cpp`,
manifest and package entry point) is intentionally not part of this project.
`C:\firefox-source\app` is not a source or build dependency and must remain
unchanged.

`GeckoHost` exposes one XAML `UIElement` to the C# Shell. Gecko/XRE and the
headless widget bridge are process-global, so the component deliberately owns
one runtime; Firefox continues to own its tabs and complete browser UI inside
that one DesktopMode window.

The Gecko checkout, object directory and compiler are machine-local settings.
Copy `GeckoBuild.local.props.example` to `GeckoBuild.local.props` and configure:

- `GeckoW10mSource=C:\firefox-source\engine\firefox`
- `GeckoW10mObj=C:\rw-obj`
- `GeckoW10mClangCl=C:\Program Files\LLVM\bin\clang-cl.exe`

`GeckoBuild.local.props` is ignored by Git. The values can alternatively be
provided as the MSBuild properties above or through `GECKO_W10M_SOURCE`,
`GECKO_W10M_OBJ` and `GECKO_W10M_CLANG_CL` environment variables.

`GeckoRuntimeFiles.props` is the explicit package manifest for the embedded
runtime. It intentionally selects the libraries, executables and resource
directories required by Firefox instead of packaging the complete live
`dist\bin` build tree. If Gecko gains a required runtime file, add it to that
manifest rather than restoring a recursive `dist\bin\**\*` include. The
selected files are copied into the DesktopMode package as architecture files,
outside the PRI resource index.

The component is intentionally x64-only, matching the current Gecko object
directory and the DesktopMode bundle target. NuGet restore supplies
`Microsoft.Windows.CppWinRT`, which generates the component projection from
`DesktopMode.Gecko.idl`.

Closing the internal window removes it from Task View/taskbar and stops its
render/input forwarding, but it does not try to tear down XRE. The embedded
Firefox runtime is process-global and is reused if the window is opened again;
an in-process Gecko restart is not a supported or safe lifetime boundary.

### Retained swap-chain buffers (resize diagnostic v6)

The integrated host publishes the Shell screen size separately from the current
viewport through `angle_uwp_set_panel_capacity`, using Gecko W10M's source-size
mode 2. ANGLE reserves the screen-sized swap-chain buffers and retains them while
the viewport fits. `SetSourceSize` and its corrective matrix are applied before
presenting the next frame. Input coordinates and the browser's CSS scale continue
to use the viewport, not the reserved capacity.

Unlike the upstream direct-back-buffer-only path, this adaptation also supports
the existing offscreen GPU copy: its texture and viewport track the window size,
and the copy writes to the top-left region of the retained buffers. The direct
rendering preference is not enabled by this change.

Logs name `capacity v6`, `resize v6 ... no ResizeBuffers`, and `source-size v6`
with the API result, returned source dimensions and device status. Missing the
capacity export means an older ANGLE library was packaged. A viewport exceeding
the actual buffer capacity still uses the instrumented native resize path.
If source-size/matrix application is rejected, capacity is disabled for that
surface and an exact-size chain is rebuilt; failed rebuilds retry at most once
per second. The v4 GPU-completion and v5 panel-detachment diagnostics remain on
the native resize path, not on source-size-only resizes.

This is an unvalidated adaptation for Xbox, not confirmation that the device
reset is fixed. Building the engine and repackaging the host are both required.

### WebAssembly compilation memory

The shared engine's `GECKO_W10M` path limits modules with a code section of at
least 16 MiB to two compilation-task slots per generator. Large asynchronous
buffered compile/instantiate jobs are scheduled one at a time; small promise
jobs retain their existing scheduling. Queued jobs do not occupy helper threads.
Streaming jobs are not serialized because their input can depend on other
workers, but their large module generators use the same two-slot limit.
Synchronous module constructors are not serialized across workers.

The JIT allocator decommits a successful writable allocation if the subsequent
executable-protection transition fails, before returning its pages to the pool.
Allocation refusals still report errors; the change does not increase memory
limits or disable Wasm optimizations or game workers.

Diagnostics include `wasm: large buffered compile started/finished`,
`wasm: large module batching`, and `jit: commit snapshot`. They record sizes,
timing and memory counters, not module URLs or bytecode. These are peak-memory
mitigations, not confirmation that Eaglercraft now loads. Rebuild the engine and
repackage each host to use them. No build or Xbox runtime validation was run.

### Parsec Web clipboard compatibility

The shared engine's built-in webcompat add-on injects
`injections/js/gecko-w10m-parsec-clipboard.js` at document start, in the page's
main world, only for `https://web.parsec.app`. A native `readText()` rejection
with `NotAllowedError` becomes empty text, allowing Matoya to signal its waiting
worker. Successful reads and other errors retain their existing behavior;
clipboard permissions and `writeText()` are unchanged. A denial is logged once
per document without clipboard contents or exception details.

Run the mocked regression checks with
`node tests/parsec-clipboard-compat.test.cjs <Firefox engine source directory>`
from this directory. They do not launch Firefox or validate Xbox behavior.
Repackage the updated shared engine resources for either host to use the shim;
changing or rebuilding the host alone does not update the built-in add-on.

### Memory growth diagnostics

The shared engine writes `memdiag:` counters and anonymized category totals to
the existing notes log. They distinguish process/app commit, allocator heap and
overhead, JS heap, retained documents, and stream buffers awaiting physical
release. Detailed reports run with headroom guards and no forced GC; report
paths, URLs, titles, and input are never logged by this diagnostic code.

See [MEMORY-INVESTIGATION.md](MEMORY-INVESTIGATION.md) for the log-set-22 timeline,
source findings, field definitions, and limitations. Rebuild the engine and
repackage the host to include these changes. No build or Xbox validation was run.
