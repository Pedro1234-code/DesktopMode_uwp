"""Native USER32 navigation/dialog contracts; creates only hidden windows.

This checks Windows reference behavior, not the bridge C++ implementation.
"""
import ctypes as c
from ctypes import wintypes as w
import struct

u = c.WinDLL('user32', use_last_error=True)
k = c.WinDLL('kernel32', use_last_error=True)
if c.sizeof(c.c_void_p) != 8:
    raise SystemExit('Windows x64 required')
k.GetModuleHandleW.argtypes = [w.LPCWSTR]; k.GetModuleHandleW.restype = w.HMODULE
u.CreateWindowExW.argtypes = [w.DWORD, w.LPCWSTR, w.LPCWSTR, w.DWORD,
    c.c_int, c.c_int, c.c_int, c.c_int, w.HWND, w.HMENU, w.HINSTANCE, c.c_void_p]
u.CreateWindowExW.restype = w.HWND
u.DestroyWindow.argtypes = [w.HWND]; u.DestroyWindow.restype = w.BOOL
u.GetNextDlgTabItem.argtypes = [w.HWND, w.HWND, w.BOOL]; u.GetNextDlgTabItem.restype = w.HWND
u.EnableWindow.argtypes = [w.HWND, w.BOOL]; u.EnableWindow.restype = w.BOOL
u.ShowWindow.argtypes = [w.HWND, c.c_int]; u.ShowWindow.restype = w.BOOL
u.SetWindowPos.argtypes = [w.HWND, w.HWND, c.c_int, c.c_int, c.c_int, c.c_int, w.UINT]
u.SetWindowPos.restype = w.BOOL
u.SetWindowLongPtrW.argtypes = [w.HWND, c.c_int, c.c_ssize_t]; u.SetWindowLongPtrW.restype = c.c_ssize_t
u.SendMessageW.argtypes = [w.HWND, w.UINT, c.c_size_t, c.c_ssize_t]; u.SendMessageW.restype = c.c_ssize_t
DlgProc = c.WINFUNCTYPE(c.c_ssize_t, w.HWND, w.UINT, c.c_size_t, c.c_ssize_t)
u.CreateDialogIndirectParamW.argtypes = [w.HINSTANCE, c.c_void_p, w.HWND, DlgProc, c.c_ssize_t]
u.CreateDialogIndirectParamW.restype = w.HWND
passed = 0
windows = []
module = k.GetModuleHandleW(None)

def check(condition, label):
    global passed
    if not condition:
        raise AssertionError(label)
    passed += 1

def make(parent=None, style=0, extended=0):
    window = u.CreateWindowExW(extended, 'STATIC', '', style, 0, 0, 60, 20,
        parent, None, module, None)
    if not window:
        raise c.WinError(c.get_last_error())
    windows.append(window)
    return window

# Hidden root: child WS_VISIBLE is sufficient for navigation, even though the
# entire subtree is not effectively screen-visible.
CHILD, VISIBLE, TAB, DISABLED, CONTROL_PARENT = 0x40000000, 0x10000000, 0x10000, 0x08000000, 0x10000
try:
    root = make(style=0x80000000)
    first = make(root, CHILD | VISIBLE | TAB)
    label = make(root, CHILD | VISIBLE)
    group = make(root, CHILD | VISIBLE | TAB, CONTROL_PARENT)
    nested_a = make(group, CHILD | VISIBLE | TAB)
    nested_b = make(group, CHILD | VISIBLE | TAB)
    last = make(root, CHILD | VISIBLE | TAB)
    get = u.GetNextDlgTabItem
    for source, target in [(None, first), (root, first), (first, nested_a),
                           (label, nested_a), (nested_a, nested_b), (nested_b, last), (last, first)]:
        check(get(root, source, False) == target, f'Forward tab: {source} -> {target}')
    check(not get(root, None, True) and not get(root, root, True), 'Previous with null/root is null')
    for source, target in [(first, last), (last, nested_b), (nested_b, nested_a), (nested_a, first)]:
        check(get(root, source, True) == target, 'Reverse traversal through control-parent')
    u.EnableWindow(nested_a, False)
    check(get(root, first, False) == nested_b, 'Dynamically disabled tab stop skipped')
    check(get(root, nested_a, False) == nested_b, 'Disabled current control retains its anchor')
    u.EnableWindow(group, False)
    check(get(root, first, False) == last, 'Disabled control-parent excludes descendants')
    u.EnableWindow(group, True); u.EnableWindow(nested_a, True)
    u.ShowWindow(group, 0)
    check(get(root, first, False) == last, 'Hidden control-parent excludes descendants')
    u.ShowWindow(group, 8)  # hidden root remains hidden
    check(get(root, first, False) == nested_a, 'Shown control-parent reenters traversal')
    u.SetWindowPos(last, None, 0, 0, 0, 0, 0x13)
    check(get(root, None, False) == last and get(root, last, False) == first, 'Current child Z order determines tab order')
    # Non-control-parent windows are not traversed, regardless of grandchildren.
    opaque = make(root, CHILD | VISIBLE | TAB)
    ignored = make(opaque, CHILD | VISIBLE | TAB)
    check(get(root, nested_b, False) == opaque, 'Opaque tab stop receives focus, not its child')
    check(get(root, opaque, False) == last, 'Traversal wraps after opaque control')

    @DlgProc
    def procedure(window, message, wp, lp):
        if message == 0x110:  # WM_INITDIALOG is a direct-return exception
            return 1
        if message == 0x8101:
            # Returning a raw PSM result is insufficient for a DLGPROC.
            return lp
        if message == 0x8102:
            u.SetWindowLongPtrW(window, 0, lp)
            return 1
        if message == 0x8103:
            u.SendMessageW(window, 0x8102, 0, 77)
            # Set after any nested callbacks: each dialog dispatch resets it.
            u.SetWindowLongPtrW(window, 0, lp)
            return 1
        return 0

    raw = c.create_string_buffer(struct.pack('<IIHhhhhHHH', 0x80000000, 0, 0, 0, 0, 100, 60, 0, 0, 0))
    dialog = u.CreateDialogIndirectParamW(module, raw, None, procedure, 0)
    check(bool(dialog), 'Hidden modeless dialog created')
    windows.append(dialog)
    check(u.SendMessageW(dialog, 0x8101, 0, 0x12345678) == 0, 'Raw DLGPROC return loses message value')
    for value in (0, 1, -1, 0x123456789):
        check(u.SendMessageW(dialog, 0x8102, 0, value) == value, 'DWLP_MSGRESULT preserves zero/sign/pointer width')
        check(u.SendMessageW(dialog, 0x8103, 0, value) == value, 'Outer result survives nested SendMessage')
finally:
    # Children are also destroyed with their parent; duplicate DestroyWindow
    # calls merely fail and do not touch any unrelated handles.
    for window in reversed(windows):
        u.DestroyWindow(window)
print(f'Passed {passed} native USER32 contracts; no bridge C++ or visible UI was executed.')
