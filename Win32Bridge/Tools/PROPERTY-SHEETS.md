# Property sheet implementation status

This implements ordinary tabbed property sheets generically, from the guest's
resources or indirect dialog templates. It does not use application names,
command IDs from a particular PE, or the host's desktop comctl32 UI.

## Page ownership and ABI

- `CreatePropertySheetPageA/W` creates an opaque handle in a process-owned
  registry. The original PSP bytes (including extensions), used resource-name
  and title strings, and A/W callback representation are retained in a stable
  private allocation. `WM_INITDIALOG` receives that allocation, not a borrowed
  application stack pointer. Changes to its `lParam` remain visible later.
- PSP V1 is 72 bytes on x64. Its callback is at offset 56. The PSH V1 prefix is
  also 72 bytes, with its page-array union at offset 56 and callback at 64.
  This differs from the full, later-version structures.
- A sheet consumes supplied page handles; `DestroyPropertySheetPage` applies
  to unattached pages. Failed dynamic add/insert leaves the handle available to
  the caller. Removal destroys the page window and releases its page object.
- `PSPCB_ADDREF` is sent only for versions beyond V1, `PSPCB_CREATE` can reject
  window creation, and `PSPCB_RELEASE` runs once before the parent reference
  count is decremented. The private PSP pointer remains stable.
- `PSP_USEREFPARENT` and the page activation context retain their lifetimes.
  Callbacks execute outside the registry lock and through the C++/SEH guards.

## Sheet behavior

- `PropertySheetA/W` accepts either variable-sized inline PSP structures or an
  array of handles. Pages are created lazily; `PSP_PREMATURE` requests early
  creation. Page dimensions and fonts come from their parsed templates.
- Normal and modeless sheets retain a heap-owned runtime associated with the
  window. The runtime does not occupy application GWLP_USERDATA.
- `PSCB_PRECREATE` receives a writable extended dialog template. The resulting
  template is parsed again so style, size, font and control changes take
  effect. `PSCB_INITIALIZED` runs after page/tab initialization.
- `PSM_ADDPAGE`, `INSERTPAGE`, `REMOVEPAGE`, `SETCURSEL`, `SETCURSELID`, page/
  window/index/ID queries, `ISDIALOGMESSAGE`, `GETRESULT`, Apply/Cancel/Close,
  dirty state, title A/W, Help, and restart/reboot result requests are handled.
  Page-list mutations are rejected during protected page notifications.
- PSM values are written to DWLP_MSGRESULT before the DLGPROC reports them
  handled. This preserves zero, -1, Boolean values and full x64 HWNDs instead
  of accidentally returning zero for every message. Null HWND queries do not
  match lazy/uncreated pages.
- Page activation honors PSN_KILLACTIVE, PSN_QUERYINITIALFOCUS, PSN_SETACTIVE
  skip/ID redirects and TCN_SELCHANGING veto. Circular redirects are bounded;
  nested selection/Apply/Cancel and page-list mutation are rejected during
  protected callbacks and layout/selection transitions.
- Page rectangles are recomputed from the actual tab display area on each
  selection. On WM_SIZE the tab stretches and footer buttons remain anchored
  using their measured initial rectangles; their font-derived sizes survive.
- Cancel returns zero; accepted changes return IDOK (or the requested restart/
  reboot result). A modeless sheet signals completion through a null current
  page and PSM_GETRESULT; its application destroys the window, as on Win32.
- Process-exit cleanup suppresses callbacks into departing code, releases
  reference/activation bindings while memory is mapped, and preserves PSP data
  through DLL detach before dropping remaining modeless ownership.

## Validation and outstanding work

`Tools/Test-PropertyPages.py` performs 42 native comctl32 contract checks without
showing windows: x64 layout, versions V1–V4, A/W cloning, callback gating, stable
private data, mutable lParam, and reference-count/release ordering. It does not
execute the bridge implementation or validate its renderer.

`Tools/Test-DialogNavigation.py` adds 30 native USER32 contracts using hidden
windows: live Z order, nested control parents, hidden/disabled controls,
forward/reverse wrapping, and DLGPROC return values including nested messages.
The same dynamic tab traversal now serves GetNextDlgTabItem, WM_NEXTDLGCTL and
IsDialogMessage in the bridge. Showing child controls preserves focus;
activation preserves a valid focused descendant.

`GuestApps/DialogContracts` contains a separate PE integration fixture for
page handles/queries, selection veto/skipping, initial focus, Apply and modeless
completion/release. It also covers bridge-specific resize/reentry protections.
This fixture has been added but **has not been compiled or run**.

The C++ implementation has received source/XML/diff checks. The agent has not
built it or run the visual/modeless paths; those remain user-run verification.

Wizard/Aero Wizard navigation, watermarks/header artwork, full page icon/image
list integration, PSM_RECALCPAGESIZES, and version/theme-specific layout details
are still separate work. This document is not a claim of complete comctl32
compatibility or mature compatibility for arbitrary Win32 applications.

References: local ReactOS `dll/win32/comctl32/propsheet.c`; Windows SDK Prsht.h;
[Microsoft property sheet lifecycle](https://learn.microsoft.com/en-us/windows/win32/controls/property-sheets),
[modeless completion](https://learn.microsoft.com/en-us/windows/win32/controls/psm-getcurrentpagehwnd),
and [PSN_RESET](https://learn.microsoft.com/en-us/windows/win32/controls/psn-reset).
