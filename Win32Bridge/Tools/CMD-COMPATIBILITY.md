# CMD import coverage

The supplied Win32Bridge-imports.txt was a preliminary binding snapshot.
Reports now distinguish preliminary/final binding and delay imports, and keep
asynchronous writes ordered so an older report cannot overwrite the final one.

Audit-GuestImports.py parses the bundled x64 CMD PE directly. The current audit
finds source-registration candidates for all 305 imports: 286 regular and 19
delay-load. cmd-import-audit.json records each symbol and the PE SHA-256.
This is a heuristic source audit, not observed address resolution, a successful
C++ build, or proof that CMD executes. DLL guards and linking need runtime
validation. The tool can audit other x64 PEs; the adapters have no executable-
filename checks.

## Implemented adapters

- UCRT private _o_* aliases resolve only through existing public adapters.
  CRT additions cover _pipe, _getch, locale helpers, _time32, _wpopen/_pclose,
  per-guest new-mode/new-handler allocation policy, guest invalid-parameter termination, x64 nonlocal unwind and
  executable TLS exit callbacks. Native CRT locale is restored on leaving the
  guest scope. Popen uses guest processes, handles and COMSPEC.
- Console cells, dimensions, cursor, attributes, fill, scroll, title and flush;
  cooked Ctrl+C queues process-local control callbacks consumed by console reads.
  Shutdown cancels waiting console/pipe reads while retaining output.
- Named mutexes/semaphores, guest thread lookup and token objects, threadpool
  timers with guest callback contexts and shutdown ordering, memory queries,
  guest path/environment search and additional kernel APIs.
- NT adapters for guest handles/files/tokens, DOS-to-NT path strings, file
  position/end-of-file, selected process/volume queries, and explicit NTSTATUS
  conversion. Root directory handles are supported without allowing root delete.
- Virtual identity/SID lookup, security descriptor owner extraction and disabled
  ETW providers. No host identity impersonation or enabled trace session.
- WinRT initialization uses virtual COM apartment accounting. API-set contracts
  map to virtual providers; delay resolution handles names/ordinals and hooks.
- Branding reads guest registry metadata. Console utility wrappers route stream
  enumeration, image queries and ShellExecute to existing guest implementations.
  WLDP named exports and ordinals 2/10 use the inspected modern Windows profile;
  private ordinal layouts are version-specific.

## Explicit limits

Resolving an import does not mean every operation of that API is supported.
WLDP/SAFER policy operations, network connections, VDM and symbolic links fail
explicitly. No trust/policy approval is simulated. Process attribute-list update
and unsupported NT information classes/FSCTLs return unsupported errors. There
is no guest PEB or complete NT kernel emulation.

Timers do not implement custom pool environments/cleanup groups or complete
callback-instance semantics. The serial scheduler has no separate pending
callback queue; waiting for callbacks preserves future timer firings. Shutdown
is cooperative; CPU-bound guest code
cannot be forcibly interrupted safely. Console Ctrl+C is dispatched during
console reads, not asynchronously to arbitrary process groups. Full VT parsing,
INPUT_RECORD/resize events, all console devices and child terminal-window
creation remain outside this import pass. Native system DLLs are not loaded as
unrestricted guest implementations.

## Verification without a build

- Static PE/source audit: 286 regular + 19 delay, zero uncovered symbol names.
- Native Windows reference probes: 27 UCRT, 19 console buffer and 29 NTSTATUS
  contracts passed. These probes exercise native Windows, not bridge C++.
- Terminal JavaScript syntax and mocked transport/input tests passed, including
  cooked Ctrl+C. These do not execute xterm rendering or UWP WebView.
- Generated export alias check: 3729 aliases from 20 ReactOS specs.
- Source delimiter, project XML, Python syntax and diff whitespace checks passed.
- Bridge C++ fixtures are integrated but have not been built or executed.

No C++/C# build and no CMD execution was performed by the agent. The user owns
builds and Xbox validation. A fresh final-stage runtime import report is needed
to verify actual binding and subsequent runtime behavior.

References: local ReactOS CRT startup/unwind and API specs, Windows SDK x64 ABI,
and native console/status reference behavior. Branding ownership was compared
with https://github.com/wine-mirror/wine/blob/master/dlls/winbrand/main.c.
