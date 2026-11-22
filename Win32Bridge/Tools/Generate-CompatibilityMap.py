"""Generate the README coverage map from the reviewed milestones below."""
from html import escape
from pathlib import Path

# Each milestone is an independently reviewable capability.
# + implementation present, ~ partial or host-dependent, - planned/deferred.
AREAS = [
    ("PE loader", ["+ PE32+ mapping and relocations", "+ IAT, delay imports and forwarders",
                   "+ TLS and DLL lifecycle", "+ x64 unwind registration", "~ DLL search parity"]),
    ("Kernel32", ["+ Guest process creation", "+ Handle duplication and inheritance",
                  "+ Events, mutexes and timers", "+ Named pipes",
                  "+ Overlapped I/O and IOCP", "+ Byte-range locks", "~ File identity across rename"]),
    ("NTDLL", ["+ Native read and write", "+ Native lock and unlock",
               "+ Native I/O cancellation", "~ Native file information and object coverage"]),
    ("USER32", ["+ Virtual HWNDs and message queues", "+ Menus and accelerators",
                "+ Dialog navigation", "~ Modal focus and reentrancy", "~ Painting and resize fidelity"]),
    ("GDI32", ["+ HDC state and transforms", "+ Regions and clipping", "+ Paths and curves",
               "+ DirectWrite layout and text metrics", "~ DIB and raster-operation parity"]),
    ("COMCTL32", ["+ Built-in control classes", "+ Property sheets",
                  "~ Owner/custom draw and notifications", "~ ListView and TreeView fidelity", "- Accessibility coverage"]),
    ("COMDLG32 / Shell32", ["+ Virtual file picker", "+ Shell properties verb",
                           "+ Shortcut discovery and PE icons", "~ File-dialog behavior", "- Printing beyond stubs"]),
    ("Registry / Advapi32", ["+ Virtual keys and values", "+ Persistent machine and user hives", "- REGF compatibility"]),
    ("PE resources / SxS", ["+ Resource directory lookup", "+ Dialogs, menus and images",
                           "+ Manifests and activation contexts", "~ Guest assembly resolution",
                           "- Language fallback", "- External WinSxS store"]),
    ("CRT / C++", ["+ Startup, heap and stdio", "+ Formatting and scanning",
                   "~ x64 C++ exceptions and RTTI", "~ Locale and multibyte edge cases", "- Broader dynamic MSVCP/STL"]),
    ("Console", ["+ Guest screen buffers and input", "+ Shell Terminal frontend",
                 "+ Standard stream redirection", "~ Console code-page fidelity"]),
    ("D3D11 / DXGI", ["+ Windowed presentation and resize", "~ Context and resource identity",
                      "~ Shared textures and fences", "- Fullscreen-exclusive presentation"]),
    ("D3D12", ["+ Windowed swap chains and queues", "~ Shared fences and handles",
               "~ Device factory and host SDK selection", "- Device-loss recovery coverage"]),
    ("Networking", ["- Winsock coverage", "- HTTP and higher-level internet APIs"]),
]

# Additional areas and finer-grained milestones for desktop workloads.
EXTRA = {
    "PE loader": ["+ Section protections", "+ Ordinal exports", "+ API-set aliases",
                  "~ Failed load rollback and unload ordering", "~ Cross-module exception unwinding",
                  "- Managed PE and CLR hosting", "- x86/WOW64 guest execution (outside current target)"],
    "Kernel32": ["+ File mappings and directory notifications", "+ Guest environment and current directory",
                 "~ Extended startup attributes and jobs", "~ Named kernel-object namespaces and inheritance",
                 "~ Threadpool and alertable waits", "~ File metadata and reparse points",
                 "- Memory-mapped coherency across independent views", "- Windows service hosting"],
    "NTDLL": ["~ NT object manager namespace", "~ NT virtual memory and section semantics",
              "~ NT file information classes", "- ALPC", "- Full native process and thread introspection"],
    "USER32": ["+ Class atoms and extra bytes", "+ Update regions and paint lifecycle",
               "~ Capture, activation and Z-order", "~ Keyboard layouts and dead keys",
               "~ Clipboard interoperability", "~ DPI and monitor behavior",
               "- Raw Input coverage", "- Full desktop/window-station semantics"],
    "GDI32": ["+ AlphaBlend and TransparentBlt", "+ World transforms and advanced clipping",
              "~ Glyph indices, shaping and font fallback", "~ Metafiles and palettes",
              "~ MaskBlt, PlgBlt and unusual DIB layouts", "- Full ICM profile transforms"],
    "COMCTL32": ["+ Toolbar, Rebar, Tab and StatusBar", "+ ListView and TreeView base classes",
                 "~ Virtual ListView, groups and state images", "~ Label editing and drag notifications",
                 "~ Tooltip callbacks and theme parts", "~ Date, link, IP and extended combo controls"],
    "COMDLG32 / Shell32": ["~ PIDLs and namespace enumeration", "~ ShellExecute and file associations",
                          "~ Browse-for-folder, filters and validation", "- Explorer namespace extensions",
                          "- Thumbnail and preview handlers", "- Jump lists and taskbar COM integration"],
    "Registry / Advapi32": ["~ WOW64 views and registry notifications", "~ Security descriptors and access checks",
                           "- Registry transactions and recovery", "- Service Control Manager and service lifecycle",
                           "- Event Log integration"],
    "PE resources / SxS": ["+ Group icons and cursors", "+ Version resources and raw data",
                           "~ MUI satellite lookup", "~ Manifest dependency and assembly policy coverage"],
    "CRT / C++": ["+ Aligned allocation and descriptor redirection", "+ Formatted input and output",
                   "~ C++ exception pointers and cross-DLL ABI", "~ CRT locale data and restartable conversions",
                   "~ CRT process creation and child stdio", "- Further STL runtime versions and debug CRT coverage"],
    "Console": ["+ Input records and console buffers", "~ Control events and process groups",
                "~ VT input and output sequences", "- ConPTY guest contracts and multiple attached processes"],
    "D3D11 / DXGI": ["~ Adapter notifications and frame latency handles", "~ Deferred contexts and resource interop",
                     "- Swap-chain color spaces, HDR and tearing", "- Full DXGI output and monitor contracts"],
    "D3D12": ["~ Fence waits backed by guest events", "~ Agility SDK version and path selection",
              "- Residency and multi-adapter validation", "- D3D12 debug, tooling and capture interoperability"],
    "Networking": ["- DNS and adapter enumeration", "- TLS and proxy integration",
                   "- HTTP cancellation and async completion", "- WebSocket and network change notifications"],
}
for name, milestones in AREAS:
    milestones.extend(EXTRA.get(name, []))

AREAS.extend([
    ("OLE32 / COM", ["~ Apartment initialization and activation", "~ In-proc class factories and COM identity",
                     "~ IStream and structured storage", "- Cross-apartment marshaling and GIT",
                     "- Out-of-process activation and RPC", "- Running Object Table and monikers"]),
    ("OLEAUT32", ["~ BSTR, VARIANT and SAFEARRAY operations", "~ IDispatch and type metadata",
                  "- Type library registration and lookup", "- Automation marshaling and coercion parity"]),
    ("COM drag and drop", ["~ Clipboard data transfer", "- IDataObject formats and advisory connections",
                           "- IDropSource/IDropTarget lifecycle", "- Virtual files and shell drag/drop"]),
    ("IMM32 / TSF", ["~ IMM32 startup stubs", "- IME composition and candidate windows",
                     "- Text Services Framework text stores", "- CJK input and reconversion"]),
    ("Accessibility", ["- MSAA object providers", "- UI Automation providers and events",
                        "- Focus navigation and screen-reader integration", "- Accessible common controls"]),
    ("UxTheme / DWM", ["~ Common Controls activation and theme selection", "- Theme classes, parts and state metrics",
                       "- Composition attributes, thumbnails and blur", "- High-contrast and theme notifications"]),
    ("Input devices", ["+ Host keyboard and pointer translation", "~ Gamepad-to-guest interaction",
                       "- DirectInput device enumeration", "- XInput state and vibration coverage",
                       "- HID and device arrival lifecycle"]),
    ("Windows Imaging", ["~ WIC codec and bitmap adapters", "- Metadata and color-profile coverage",
                         "- Additional encoders and stream contracts"]),
    ("Direct2D / DirectWrite", ["+ Internal DirectWrite-backed text", "~ Guest-facing factory and object contracts",
                                 "- Direct2D render targets and effects", "- DirectComposition visual trees"]),
    ("Legacy graphics", ["- DirectDraw surfaces and blits", "- Direct3D 8/9 presentation",
                         "- OpenGL/WGL contexts and pixel formats", "- Vulkan loader and presentation interop"]),
    ("Audio", ["- MMDevice and WASAPI streams", "- WinMM wave and MIDI APIs",
               "- XAudio2 engines and voices", "- DirectSound buffers and capture"]),
    ("Media", ["- Media Foundation source readers and sinks", "- DirectShow filter graphs",
               "- Audio/video decoding and presentation", "- Capture devices and streaming"]),
    ("Crypto / Trust", ["+ Native random generation path", "~ BCrypt and legacy provider coverage",
                        "~ Certificate stores and decoding", "- Chain building and trust policy",
                        "- DPAPI and NCrypt key storage"]),
    ("Security / Tokens", ["~ Virtual access tokens and queries", "- SID/ACL enforcement across objects",
                           "- Impersonation and privileges", "- Credential Manager and SSPI"]),
    ("MSI / SetupAPI", ["- MSI engine, product and component registration", "- Installer repair and rollback",
                        "- Device and driver enumeration", "- INF and device-install semantics"]),
    ("Printing", ["~ Print dialog stubs", "- Print queues and spooler", "- GDI and XPS print jobs",
                  "- Printer enumeration and configuration"]),
    ("RPC / Services", ["- RPC runtime and bindings", "- Service discovery and control",
                        "- Task Scheduler", "- WMI queries and providers"]),
    ("Database / Data access", ["- ODBC manager and driver registration", "- OLE DB providers",
                                "- Cross-process data access integration"]),
    ("System integration", ["- WTS sessions and power notifications", "- ETW and application diagnostics APIs",
                            "- Device notifications and removable media", "- System-wide hooks and service integration"]),
    ("Web / Runtime hosting", ["- ActiveX and browser control hosting", "- URLMon binding and protocol handlers",
                              "- Desktop WebView integration", "- CLR hosting and managed runtime integration"]),
    ("Localization / NLS", ["~ Code-page conversion and locale data", "~ Case mapping and collation coverage",
                             "- User locale overrides", "- Per-thread UI language and MUI selection"]),
    ("Deployment validation", ["~ Contract fixtures in source", "~ Representative guest-app workflows",
                               "- Repeatable Xbox regression matrix", "- Published compatibility by build and app version"]),
])

WIDTH, CARD_WIDTH, CARD_HEIGHT, GAP = 990, 308, 84, 13
ROWS = (len(AREAS) + 2) // 3
HEIGHT = 95 + ROWS * (CARD_HEIGHT + GAP) + 12
COLORS = {"+": "#44d7a8", "~": "#f5c568", "-": "#465775"}
STATUS = {"+": "Implemented", "~": "Partial", "-": "Planned"}


def build_svg():
    labels = [
        f"{name}: " + "; ".join(f"{STATUS[item[0]]} {item[2:]}" for item in milestones)
        for name, milestones in AREAS
    ]
    svg = [
        f'<svg xmlns="http://www.w3.org/2000/svg" width="{WIDTH}" height="{HEIGHT}" viewBox="0 0 {WIDTH} {HEIGHT}" role="img" aria-labelledby="title desc">',
        '<title id="title">Win32Bridge compatibility map</title>',
        f'<desc id="desc">{escape(". ".join(labels))}</desc>',
        f'<rect width="{WIDTH}" height="{HEIGHT}" rx="20" fill="#101a2d"/>',
        '<g font-family="system-ui, Segoe UI, sans-serif">',
        '<text x="26" y="40" font-size="25" font-weight="700" fill="#f2f6ff">Win32Bridge compatibility map</text>',
    ]
    for x, (status, color) in zip((26, 161, 258), COLORS.items()):
        svg.append(f'<rect x="{x}" y="62" width="12" height="12" rx="3" fill="{color}"/>')
        svg.append(f'<text x="{x + 20}" y="73" font-size="13" fill="#dae4fb">{STATUS[status]}</text>')
    for i, (name, milestones) in enumerate(AREAS):
        x = 20 + (i % 3) * (CARD_WIDTH + GAP)
        y = 95 + (i // 3) * (CARD_HEIGHT + GAP)
        implemented = sum(item.startswith("+") for item in milestones)
        svg.extend([
            f'<g><title>{escape(labels[i])}</title>',
            f'<rect x="{x}" y="{y}" width="{CARD_WIDTH}" height="{CARD_HEIGHT}" rx="10" fill="#1c2a43" stroke="#354665"/>',
            f'<text x="{x + 14}" y="{y + 29}" font-size="16" font-weight="650" fill="#f2f6ff">{escape(name)}</text>',
            f'<text x="{x + CARD_WIDTH - 14}" y="{y + 29}" text-anchor="end" font-size="14" fill="#dae4fb">{implemented}/{len(milestones)}</text>',
        ])
        segment = (CARD_WIDTH - 28) / len(milestones)
        for j, item in enumerate(sorted(milestones, key=lambda item: list(COLORS).index(item[0]))):
            svg.append(f'<rect x="{x + 14 + j * segment:.2f}" y="{y + 48}" width="{segment - 3:.2f}" height="17" rx="4" fill="{COLORS[item[0]]}"/>')
        svg.append('</g>')
    return "\n".join([*svg, '</g>', '</svg>', ''])


def build_details():
    lines = [
        "# Win32Bridge compatibility inventory",
        "",
        "[Compatibility map](BRIDGE.md#compatibility-map)",
        "",
        "The inventory tracks selected behaviors needed by desktop applications.",
        "Implemented means an implementation is present in the source.",
        "Partial includes incomplete, stubbed or host-dependent behavior.",
        "Planned includes deferred work and capabilities outside the current target.",
        "The counts do not represent Windows export coverage or a test pass rate.",
        "",
        "| API or subsystem | Implemented | Partial | Planned | Total |",
        "| --- | ---: | ---: | ---: | ---: |",
    ]
    for name, milestones in AREAS:
        counts = [sum(item.startswith(status) for item in milestones) for status in STATUS]
        lines.append(f"| {name} | {counts[0]} | {counts[1]} | {counts[2]} | {len(milestones)} |")
    for name, milestones in AREAS:
        lines.extend(["", f"## {name}", ""])
        lines.extend(f"- **{STATUS[item[0]]}** · {item[2:]}" for item in milestones)
    return "\n".join([*lines, ""])


if __name__ == "__main__":
    output = Path(__file__).resolve().parents[1] / "compatibility-progress.svg"
    output.write_bytes(build_svg().encode("utf-8"))
    (output.parent / "COMPATIBILITY.md").write_bytes(build_details().encode("utf-8"))

