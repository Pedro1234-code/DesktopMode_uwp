# Win32Bridge compatibility inventory

[Compatibility map](BRIDGE.md#compatibility-map)

The inventory tracks selected behaviors needed by desktop applications.
Implemented means an implementation is present in the source.
Partial includes incomplete, stubbed or host-dependent behavior.
Planned includes deferred work and capabilities outside the current target.
The counts do not represent Windows export coverage or a test pass rate.

| API or subsystem | Implemented | Partial | Planned | Total |
| --- | ---: | ---: | ---: | ---: |
| PE loader | 7 | 3 | 2 | 12 |
| Kernel32 | 8 | 5 | 2 | 15 |
| NTDLL | 3 | 4 | 2 | 9 |
| USER32 | 5 | 6 | 2 | 13 |
| GDI32 | 6 | 4 | 1 | 11 |
| COMCTL32 | 4 | 6 | 1 | 11 |
| COMDLG32 / Shell32 | 3 | 4 | 4 | 11 |
| Registry / Advapi32 | 2 | 2 | 4 | 8 |
| PE resources / SxS | 5 | 3 | 2 | 10 |
| CRT / C++ | 4 | 5 | 2 | 11 |
| Console | 4 | 3 | 1 | 8 |
| D3D11 / DXGI | 1 | 4 | 3 | 8 |
| D3D12 | 1 | 4 | 3 | 8 |
| Networking | 0 | 0 | 6 | 6 |
| OLE32 / COM | 0 | 3 | 3 | 6 |
| OLEAUT32 | 0 | 2 | 2 | 4 |
| COM drag and drop | 0 | 1 | 3 | 4 |
| IMM32 / TSF | 0 | 1 | 3 | 4 |
| Accessibility | 0 | 0 | 4 | 4 |
| UxTheme / DWM | 0 | 1 | 3 | 4 |
| Input devices | 1 | 1 | 3 | 5 |
| Windows Imaging | 0 | 1 | 2 | 3 |
| Direct2D / DirectWrite | 1 | 1 | 2 | 4 |
| Legacy graphics | 0 | 0 | 4 | 4 |
| Audio | 0 | 0 | 4 | 4 |
| Media | 0 | 0 | 4 | 4 |
| Crypto / Trust | 1 | 2 | 2 | 5 |
| Security / Tokens | 0 | 1 | 3 | 4 |
| MSI / SetupAPI | 0 | 0 | 4 | 4 |
| Printing | 0 | 1 | 3 | 4 |
| RPC / Services | 0 | 0 | 4 | 4 |
| Database / Data access | 0 | 0 | 3 | 3 |
| System integration | 0 | 0 | 4 | 4 |
| Web / Runtime hosting | 0 | 0 | 4 | 4 |
| Localization / NLS | 0 | 2 | 2 | 4 |
| Deployment validation | 0 | 2 | 2 | 4 |

## PE loader

- **Implemented** · PE32+ mapping and relocations
- **Implemented** · IAT, delay imports and forwarders
- **Implemented** · TLS and DLL lifecycle
- **Implemented** · x64 unwind registration
- **Partial** · DLL search parity
- **Implemented** · Section protections
- **Implemented** · Ordinal exports
- **Implemented** · API-set aliases
- **Partial** · Failed load rollback and unload ordering
- **Partial** · Cross-module exception unwinding
- **Planned** · Managed PE and CLR hosting
- **Planned** · x86/WOW64 guest execution (needs JIT)

## Kernel32

- **Implemented** · Guest process creation
- **Implemented** · Handle duplication and inheritance
- **Implemented** · Events, mutexes and timers
- **Implemented** · Named pipes
- **Implemented** · Overlapped I/O and IOCP
- **Implemented** · Byte-range locks
- **Partial** · File identity across rename
- **Implemented** · File mappings and directory notifications
- **Implemented** · Guest environment and current directory
- **Partial** · Extended startup attributes and jobs
- **Partial** · Named kernel-object namespaces and inheritance
- **Partial** · Threadpool and alertable waits
- **Partial** · File metadata and reparse points
- **Planned** · Memory-mapped coherency across independent views
- **Planned** · Windows service hosting

## NTDLL

- **Implemented** · Native read and write
- **Implemented** · Native lock and unlock
- **Implemented** · Native I/O cancellation
- **Partial** · Native file information and object coverage
- **Partial** · NT object manager namespace
- **Partial** · NT virtual memory and section semantics
- **Partial** · NT file information classes
- **Planned** · ALPC
- **Planned** · Full native process and thread introspection

## USER32

- **Implemented** · Virtual HWNDs and message queues
- **Implemented** · Menus and accelerators
- **Implemented** · Dialog navigation
- **Partial** · Modal focus and reentrancy
- **Partial** · Painting and resize fidelity
- **Implemented** · Class atoms and extra bytes
- **Implemented** · Update regions and paint lifecycle
- **Partial** · Capture, activation and Z-order
- **Partial** · Keyboard layouts and dead keys
- **Partial** · Clipboard interoperability
- **Partial** · DPI and monitor behavior
- **Planned** · Raw Input coverage
- **Planned** · Full desktop/window-station semantics

## GDI32

- **Implemented** · HDC state and transforms
- **Implemented** · Regions and clipping
- **Implemented** · Paths and curves
- **Implemented** · DirectWrite layout and text metrics
- **Partial** · DIB and raster-operation parity
- **Implemented** · AlphaBlend and TransparentBlt
- **Implemented** · World transforms and advanced clipping
- **Partial** · Glyph indices, shaping and font fallback
- **Partial** · Metafiles and palettes
- **Partial** · MaskBlt, PlgBlt and unusual DIB layouts
- **Planned** · Full ICM profile transforms

## COMCTL32

- **Implemented** · Built-in control classes
- **Implemented** · Property sheets
- **Partial** · Owner/custom draw and notifications
- **Partial** · ListView and TreeView fidelity
- **Planned** · Accessibility coverage
- **Implemented** · Toolbar, Rebar, Tab and StatusBar
- **Implemented** · ListView and TreeView base classes
- **Partial** · Virtual ListView, groups and state images
- **Partial** · Label editing and drag notifications
- **Partial** · Tooltip callbacks and theme parts
- **Partial** · Date, link, IP and extended combo controls

## COMDLG32 / Shell32

- **Implemented** · Virtual file picker
- **Implemented** · Shell properties verb
- **Implemented** · Shortcut discovery and PE icons
- **Partial** · File-dialog behavior
- **Planned** · Printing beyond stubs
- **Partial** · PIDLs and namespace enumeration
- **Partial** · ShellExecute and file associations
- **Partial** · Browse-for-folder, filters and validation
- **Planned** · Explorer namespace extensions
- **Planned** · Thumbnail and preview handlers
- **Planned** · Jump lists and taskbar COM integration

## Registry / Advapi32

- **Implemented** · Virtual keys and values
- **Implemented** · Persistent machine and user hives
- **Planned** · REGF compatibility
- **Partial** · WOW64 views and registry notifications
- **Partial** · Security descriptors and access checks
- **Planned** · Registry transactions and recovery
- **Planned** · Service Control Manager and service lifecycle
- **Planned** · Event Log integration

## PE resources / SxS

- **Implemented** · Resource directory lookup
- **Implemented** · Dialogs, menus and images
- **Implemented** · Manifests and activation contexts
- **Partial** · Guest assembly resolution
- **Planned** · Language fallback
- **Planned** · External WinSxS store
- **Implemented** · Group icons and cursors
- **Implemented** · Version resources and raw data
- **Partial** · MUI satellite lookup
- **Partial** · Manifest dependency and assembly policy coverage

## CRT / C++

- **Implemented** · Startup, heap and stdio
- **Implemented** · Formatting and scanning
- **Partial** · x64 C++ exceptions and RTTI
- **Partial** · Locale and multibyte edge cases
- **Planned** · Broader dynamic MSVCP/STL
- **Implemented** · Aligned allocation and descriptor redirection
- **Implemented** · Formatted input and output
- **Partial** · C++ exception pointers and cross-DLL ABI
- **Partial** · CRT locale data and restartable conversions
- **Partial** · CRT process creation and child stdio
- **Planned** · Further STL runtime versions and debug CRT coverage

## Console

- **Implemented** · Guest screen buffers and input
- **Implemented** · Shell Terminal frontend
- **Implemented** · Standard stream redirection
- **Partial** · Console code-page fidelity
- **Implemented** · Input records and console buffers
- **Partial** · Control events and process groups
- **Partial** · VT input and output sequences
- **Planned** · ConPTY guest contracts and multiple attached processes

## D3D11 / DXGI

- **Implemented** · Windowed presentation and resize
- **Partial** · Context and resource identity
- **Partial** · Shared textures and fences
- **Planned** · Fullscreen-exclusive presentation
- **Partial** · Adapter notifications and frame latency handles
- **Partial** · Deferred contexts and resource interop
- **Planned** · Swap-chain color spaces, HDR and tearing
- **Planned** · Full DXGI output and monitor contracts

## D3D12

- **Implemented** · Windowed swap chains and queues
- **Partial** · Shared fences and handles
- **Partial** · Device factory and host SDK selection
- **Planned** · Device-loss recovery coverage
- **Partial** · Fence waits backed by guest events
- **Partial** · Agility SDK version and path selection
- **Planned** · Residency and multi-adapter validation
- **Planned** · D3D12 debug, tooling and capture interoperability

## Networking

- **Planned** · Winsock coverage
- **Planned** · HTTP and higher-level internet APIs
- **Planned** · DNS and adapter enumeration
- **Planned** · TLS and proxy integration
- **Planned** · HTTP cancellation and async completion
- **Planned** · WebSocket and network change notifications

## OLE32 / COM

- **Partial** · Apartment initialization and activation
- **Partial** · In-proc class factories and COM identity
- **Partial** · IStream and structured storage
- **Planned** · Cross-apartment marshaling and GIT
- **Planned** · Out-of-process activation and RPC
- **Planned** · Running Object Table and monikers

## OLEAUT32

- **Partial** · BSTR, VARIANT and SAFEARRAY operations
- **Partial** · IDispatch and type metadata
- **Planned** · Type library registration and lookup
- **Planned** · Automation marshaling and coercion parity

## COM drag and drop

- **Partial** · Clipboard data transfer
- **Planned** · IDataObject formats and advisory connections
- **Planned** · IDropSource/IDropTarget lifecycle
- **Planned** · Virtual files and shell drag/drop

## IMM32 / TSF

- **Partial** · IMM32 startup stubs
- **Planned** · IME composition and candidate windows
- **Planned** · Text Services Framework text stores
- **Planned** · CJK input and reconversion

## Accessibility

- **Planned** · MSAA object providers
- **Planned** · UI Automation providers and events
- **Planned** · Focus navigation and screen-reader integration
- **Planned** · Accessible common controls

## UxTheme / DWM

- **Partial** · Common Controls activation and theme selection
- **Planned** · Theme classes, parts and state metrics
- **Planned** · Composition attributes, thumbnails and blur
- **Planned** · High-contrast and theme notifications

## Input devices

- **Implemented** · Host keyboard and pointer translation
- **Partial** · Gamepad-to-guest interaction
- **Planned** · DirectInput device enumeration
- **Planned** · XInput state and vibration coverage
- **Planned** · HID and device arrival lifecycle

## Windows Imaging

- **Partial** · WIC codec and bitmap adapters
- **Planned** · Metadata and color-profile coverage
- **Planned** · Additional encoders and stream contracts

## Direct2D / DirectWrite

- **Implemented** · Internal DirectWrite-backed text
- **Partial** · Guest-facing factory and object contracts
- **Planned** · Direct2D render targets and effects
- **Planned** · DirectComposition visual trees

## Legacy graphics

- **Planned** · DirectDraw surfaces and blits
- **Planned** · Direct3D 8/9 presentation
- **Planned** · OpenGL/WGL contexts and pixel formats
- **Planned** · Vulkan loader and presentation interop

## Audio

- **Planned** · MMDevice and WASAPI streams
- **Planned** · WinMM wave and MIDI APIs
- **Planned** · XAudio2 engines and voices
- **Planned** · DirectSound buffers and capture

## Media

- **Planned** · Media Foundation source readers and sinks
- **Planned** · DirectShow filter graphs
- **Planned** · Audio/video decoding and presentation
- **Planned** · Capture devices and streaming

## Crypto / Trust

- **Implemented** · Native random generation path
- **Partial** · BCrypt and legacy provider coverage
- **Partial** · Certificate stores and decoding
- **Planned** · Chain building and trust policy
- **Planned** · DPAPI and NCrypt key storage

## Security / Tokens

- **Partial** · Virtual access tokens and queries
- **Planned** · SID/ACL enforcement across objects
- **Planned** · Impersonation and privileges
- **Planned** · Credential Manager and SSPI

## MSI / SetupAPI

- **Planned** · MSI engine, product and component registration
- **Planned** · Installer repair and rollback
- **Planned** · Device and driver enumeration
- **Planned** · INF and device-install semantics

## Printing

- **Partial** · Print dialog stubs
- **Planned** · Print queues and spooler
- **Planned** · GDI and XPS print jobs
- **Planned** · Printer enumeration and configuration

## RPC / Services

- **Planned** · RPC runtime and bindings
- **Planned** · Service discovery and control
- **Planned** · Task Scheduler
- **Planned** · WMI queries and providers

## Database / Data access

- **Planned** · ODBC manager and driver registration
- **Planned** · OLE DB providers
- **Planned** · Cross-process data access integration

## System integration

- **Planned** · WTS sessions and power notifications
- **Planned** · ETW and application diagnostics APIs
- **Planned** · Device notifications and removable media
- **Planned** · System-wide hooks and service integration

## Web / Runtime hosting

- **Planned** · ActiveX and browser control hosting
- **Planned** · URLMon binding and protocol handlers
- **Planned** · Desktop WebView integration
- **Planned** · CLR hosting and managed runtime integration

## Localization / NLS

- **Partial** · Code-page conversion and locale data
- **Partial** · Case mapping and collation coverage
- **Planned** · User locale overrides
- **Planned** · Per-thread UI language and MUI selection

## Deployment validation

- **Partial** · Contract fixtures in source
- **Partial** · Representative guest-app workflows
- **Planned** · Repeatable Xbox regression matrix
- **Planned** · Published compatibility by build and app version
