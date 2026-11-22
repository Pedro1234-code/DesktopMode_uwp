"""Native comctl32 page contracts; no windows, PE guests, or bridge C++ build."""
import ctypes as c
from ctypes import wintypes as w
if c.sizeof(c.c_void_p) != 8 or c.sizeof(c.c_wchar) != 2:
    raise SystemExit("Windows x64 ctypes ABI required")
ctl = c.WinDLL("comctl32.dll")
Callback = c.WINFUNCTYPE(w.UINT, w.HWND, w.UINT, c.c_void_p)
class Page(c.Structure):
    _fields_ = [("size", w.DWORD), ("flags", w.DWORD), ("instance", w.HINSTANCE),
                ("template", c.c_void_p), ("icon", c.c_void_p), ("title", c.c_void_p),
                ("procedure", c.c_void_p), ("parameter", c.c_ssize_t), ("callback", Callback),
                ("reference", c.POINTER(w.UINT)), ("header_title", c.c_void_p),
                ("header_subtitle", c.c_void_p), ("activation", c.c_void_p), ("bitmap", c.c_void_p)]
class Header(c.Structure):
    _fields_ = [("size", w.DWORD), ("flags", w.DWORD), ("parent", w.HWND),
                ("instance", w.HINSTANCE), ("icon", c.c_void_p), ("caption", c.c_void_p),
                ("page_count", w.UINT), ("start", c.c_size_t), ("pages", c.c_void_p), ("callback", c.c_void_p)]
for name in ("CreatePropertySheetPageA", "CreatePropertySheetPageW"):
    fn = getattr(ctl, name); fn.argtypes = [c.c_void_p]; fn.restype = c.c_void_p
ctl.DestroyPropertySheetPage.argtypes = [c.c_void_p]; ctl.DestroyPropertySheetPage.restype = w.BOOL
passed = 0

def check(condition, label):
    global passed
    if not condition:
        raise AssertionError(label)
    passed += 1

check(c.sizeof(Page) == 104 and Page.callback.offset == 56 and Page.header_title.offset == 72, "PSP x64 ABI versions")
check(c.sizeof(Header) == 72 and Header.pages.offset == 56 and Header.callback.offset == 64, "PSH x64 union alignment")
for unicode in (False, True):
    for size in (72, 88, 96, 104):
        references = w.UINT(7)
        title = c.create_unicode_buffer("Title") if unicode else c.create_string_buffer(b"Title")
        template = c.create_unicode_buffer("Template") if unicode else c.create_string_buffer(b"Template")
        events = []
        @Callback
        def callback(window, message, raw):
            # Read only the V1 prefix when dwSize is V1: later bytes are not part
            # of that allocation. Titles are copied by the native implementation.
            pointer = c.c_void_p.from_address(raw + Page.title.offset).value
            text = c.wstring_at(pointer) if unicode else c.string_at(pointer)
            parameter = c.c_ssize_t.from_address(raw + Page.parameter.offset).value
            events.append((message, parameter, text, raw, references.value))
            if message == 0:
                c.c_ssize_t.from_address(raw + Page.parameter.offset).value = 44
            return 1
        page = Page(); page.size = size; page.flags = 0x80 | 0x40 | 0x8
        page.template = c.addressof(template); page.title = c.addressof(title)
        page.parameter = 33; page.callback = callback; page.reference = c.pointer(references)
        create = ctl.CreatePropertySheetPageW if unicode else ctl.CreatePropertySheetPageA
        handle = create(c.byref(page))
        check(handle and handle != c.addressof(page) and references.value == 8, f"Owned handle and reference: {unicode}/{size}")
        try:
            check([e[0] for e in events] == ([] if size == 72 else [0]), f"Versioned PSPCB_ADDREF: {unicode}/{size}")
            page.parameter = 99
            title.value = "Other" if unicode else b"Other"
            template.value = "Changed" if unicode else b"Changed"
        finally:
            result = ctl.DestroyPropertySheetPage(handle)
        check(result and references.value == 7 and events[-1][0] == 1, f"Release and reference decrement: {unicode}/{size}")
        expected_text = "Title" if unicode else b"Title"
        expected_parameter = 33 if size == 72 else 44
        check(events[-1][1] == expected_parameter and events[-1][2] == expected_text and events[-1][4] == 8,
              f"Stable private structure/text and release ordering: {unicode}/{size}")
        if size > 72:
            check(events[0][3] == events[-1][3], f"Callback pointer remains stable: {unicode}/{size}")
# PSP_USECALLBACK gates both notifications even when pfnCallback is non-null.
events = []
@Callback
def disabled_callback(window, message, raw):
    events.append(message); return 1
page = Page(); page.size = 104; page.template = 101; page.callback = disabled_callback
handle = ctl.CreatePropertySheetPageW(c.byref(page))
try:
    check(bool(handle) and not events, "Callback flag required on creation")
finally:
    if handle: ctl.DestroyPropertySheetPage(handle)
check(not events, "Callback flag required on destruction")
print(f"Passed {passed} native property-page contracts; bridge C++ adapters and visual/modeless behavior were not built or executed.")
