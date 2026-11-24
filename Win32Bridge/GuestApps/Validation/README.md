# Xbox validation suite

Open `C:\Windows\test_apps` in Desktop Mode Files. Each executable has its own
menu. The graphical apps use buttons. In ConsoleValidation, enter the test
number and press Enter. GraphicsContracts opens a renderer for the selected
case. Resize the window during rendering, use Space for first-frame tests,
and press Escape to return to test selection.

| Executable | Coverage |
| --- | --- |
| `CrtValidation.exe` | Dynamic CRT, aligned heap and realloc, format/scan, binary stdio, locale and Unicode, thread-local errno, RTTI, unwind and `exception_ptr`. Uses MSVC's FH3 exception format. |
| `SystemValidation.exe` | Registry create/reopen/delete, COM memory streams and Automation, virtual memory and environment expansion. |
| `ConsoleValidation.exe` | Cell access when exported, input mode, colors, VT sequences and Unicode through Desktop Mode Terminal. |
| `Win32Gallery.exe` | Edit, buttons, combo, progress, tabs, tree, ListView, context menus, GDI primitives and DIBs, resource dialog, property-sheet contracts and file picker. |
| `FileHandleContracts.exe` | File metadata, rename, replacement, locks, mapping and copy-on-write, overlapped and NT I/O, completion ports, loader exports, synchronization and child-process lifecycle. |
| `GraphicsContracts.exe` | D3D11/12 and DXGI presentation, resize, legacy swap chain, first-frame handoff, pipeline and readback, COM identities, sharing, GPU events and D3D11On12. |

In Win32Gallery, use **Repaint / change color** in the GDI scene to toggle the
shapes between blue and orange. The scene displays its client size and paint
count. **Property sheet assertions** and **Paint assertions** run separately.

| Gallery scene | What to try |
| --- | --- |
| **Lists, tree, tabs and context menu** | Expand the tree, switch tabs, edit a ListView label, and select a context-menu command. |
| **Inputs and buttons** | Edit multiline, password and combo fields, select multiple list rows, switch radio buttons, cycle the three-state checkbox, and drag the scrollbar. |
| **Sliders, dates and specialized input** | Move the trackbar to change the progress bar, use the up-down buddy, and edit the calendar, date, IP address and hotkey. |
| **Toolbar, header and status bar** | Drag the rebar gripper or a header, click a toolbar button or SysLink to change the status bar, select a ComboBoxEx item, and hover for the tooltip. |
| **Virtual ListView** | Scroll or jump to the last row of 10,000 callback-backed rows. The log records the first text request. |

The gallery requests Common Controls v6. A control whose window creation fails
is labeled **Unavailable** and logged with its class name and error code.
References include the [Microsoft Common Control Samples](https://learn.microsoft.com/en-us/cpp/mfc/common-control-sample-list?view=msvc-170),
[Rebar Sample](https://learn.microsoft.com/en-us/windows/win32/controls/rebar-sample), and
[Virtual ListView Sample](https://learn.microsoft.com/en-us/windows/win32/controls/vlistvw-sample).

App logs are UTF-8 and append to `C:\Windows\test_apps\logs\<app>.log`.
`BEGIN` records the selected test before execution. `PASS` and `FAIL` report
assertion outcomes. `MANUAL` means visual or interactive inspection is required.
`SKIP` means the optional contract is unavailable. `READY`, `EVENT` and `END`
report graphics initialization, observations and closure. File-contract failures
report the source line in hexadecimal. Collect the relevant app log alongside
the Runtime and imports logs from LocalState. An app that fails import binding
before entry has only the Runtime and imports logs.

The shell package places the staged executables under
`GuestRuntimeDefaults\Windows\test_apps`. The standard provisioning step seeds
`LocalState\drive_c\Windows\test_apps` and retains existing files. On desktop
Windows, the executables use `%TEMP%` for logs when the guest path is absent.

`Build-And-Stage.ps1` rebuilds the six Release x64 executables with Visual Studio
2022 and copies them to `GuestApps\Staged\TestApps` for the next shell package.
