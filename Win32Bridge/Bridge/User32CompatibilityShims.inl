namespace
{
    struct GuestDrawTextParams
    {
        UINT cbSize;
        int iTabLength;
        int iLeftMargin;
        int iRightMargin;
        UINT uiLengthDrawn;
    };
    using GuestWinEventProc = void (CALLBACK*)(HWINEVENTHOOK, DWORD, HWND, LONG, LONG, DWORD, DWORD);
    using GuestWndEnumProc = BOOL (CALLBACK*)(HWND, LPARAM);
    using GuestMonitorEnumProc = BOOL (CALLBACK*)(HMONITOR, HDC, LPRECT, LPARAM);
    using GuestHookProc = LRESULT (CALLBACK*)(int, WPARAM, LPARAM);
    struct GuestTrackMouseEvent { DWORD cbSize; DWORD dwFlags; HWND hwndTrack; DWORD dwHoverTime; };
    struct GuestScrollInfo { UINT cbSize; UINT fMask; int nMin; int nMax; UINT nPage; int nPos; int nTrackPos; };
    struct GuestFlashWindowInfo { UINT cbSize; HWND hwnd; DWORD dwFlags; UINT uCount; DWORD dwTimeout; };
    struct GuestAccel { BYTE fVirt; WORD key; WORD cmd; };
    struct GuestComboBoxInfo { DWORD cbSize; RECT rcItem; RECT rcButton; DWORD stateButton; HWND hwndCombo; HWND hwndItem; HWND hwndList; };
    struct GuestMenuBarInfo { DWORD cbSize; RECT rcBar; HMENU hMenu; HWND hwndMenu; BOOL fBarFocused : 1; BOOL fFocused : 1; };
    struct GuestIconInfo { BOOL fIcon; DWORD xHotspot; DWORD yHotspot; HBITMAP hbmMask; HBITMAP hbmColor; };
    constexpr UINT GuestGwHwndFirst = 0;
    constexpr UINT GuestGwHwndLast = 1;
    constexpr UINT GuestGwHwndNext = 2;
    constexpr UINT GuestGwHwndPrev = 3;
    constexpr UINT GuestGwOwner = 4;
    constexpr UINT GuestGwChild = 5;

    std::mutex g_registeredMessageLock;
    std::unordered_map<std::wstring, UINT> g_registeredMessages;
    UINT g_nextRegisteredMessage = 0xc000;
    std::mutex g_windowPropertyLock;
    std::unordered_map<ULONG_PTR, std::unordered_map<std::wstring, HANDLE>> g_windowProperties;
    std::mutex g_caretLock;
    HWND g_caretWindow = nullptr;
    POINT g_caretPosition{};
    SIZE g_caretSize{ 1, 1 };
    int g_caretShowCount = -1;
    std::atomic<int> g_cursorShowCount{ 0 };

    std::wstring PropertyKey(LPCWSTR name)
    {
        const ULONG_PTR value = reinterpret_cast<ULONG_PTR>(name);
        if (value <= 0xffff) return L"#" + std::to_wstring(value);
        std::wstring key = name ? name : L"";
        std::transform(key.begin(), key.end(), key.begin(), [](wchar_t value)
        {
            return static_cast<wchar_t>(towlower(value));
        });
        return key;
    }

    std::wstring AnsiToWide(LPCSTR text)
    {
        if (!text) return {};
        const int count = BridgeMultiByteToWideChar(CP_ACP, 0, text, -1, nullptr, 0);
        if (count <= 1) return {};
        std::wstring result(static_cast<size_t>(count), L'\0');
        BridgeMultiByteToWideChar(CP_ACP, 0, text, -1, &result[0], count);
        result.resize(static_cast<size_t>(count - 1));
        return result;
    }

    int WideToAnsi(LPCWSTR text, LPSTR buffer, int bufferCount)
    {
        if (!buffer || bufferCount <= 0) return 0;
        buffer[0] = '\0';
        if (!text) return 0;
        const int required = BridgeWideCharToMultiByte(CP_ACP, 0, text, -1, nullptr, 0, nullptr, nullptr);
        if (required <= 0) return 0;
        std::vector<char> converted(static_cast<size_t>(required));
        if (BridgeWideCharToMultiByte(CP_ACP, 0, text, -1, converted.data(), required,
            nullptr, nullptr) <= 0) return 0;
        const int copied = (std::min)(bufferCount - 1, required - 1);
        if (copied > 0) std::memcpy(buffer, converted.data(), static_cast<size_t>(copied));
        buffer[copied] = '\0';
        return copied;
    }

BOOL WINAPI BridgeEqualRect(const RECT* left, const RECT* right)
{
    return left && right && left->left == right->left && left->top == right->top &&
        left->right == right->right && left->bottom == right->bottom;
}

BOOL WINAPI BridgeSetRectEmpty(LPRECT rect)
{
    if (!rect) return FALSE;
    *rect = {};
    return TRUE;
}

BOOL WINAPI BridgeInflateRect(LPRECT rect, int x, int y)
{
    if (!rect) return FALSE;
    rect->left -= x; rect->right += x;
    rect->top -= y; rect->bottom += y;
    return TRUE;
}

BOOL WINAPI BridgeOffsetRect(LPRECT rect, int x, int y)
{
    if (!rect) return FALSE;
    rect->left += x; rect->right += x;
    rect->top += y; rect->bottom += y;
    return TRUE;
}

BOOL WINAPI BridgePtInRect(const RECT* rect, POINT point)
{
    return rect && point.x >= rect->left && point.x < rect->right &&
        point.y >= rect->top && point.y < rect->bottom;
}

BOOL WINAPI BridgeIntersectRect(LPRECT destination, const RECT* left, const RECT* right)
{
    if (!destination || !left || !right) return FALSE;
    destination->left = (std::max)(left->left, right->left);
    destination->top = (std::max)(left->top, right->top);
    destination->right = (std::min)(left->right, right->right);
    destination->bottom = (std::min)(left->bottom, right->bottom);
    if (destination->left >= destination->right || destination->top >= destination->bottom)
    {
        *destination = {};
        return FALSE;
    }
    return TRUE;
}

LPWSTR WINAPI BridgeCharLowerW(LPWSTR text)
{
    if (!text) return nullptr;
    if (reinterpret_cast<ULONG_PTR>(text) <= 0xffff)
        return reinterpret_cast<LPWSTR>(static_cast<ULONG_PTR>(
            towlower(static_cast<wchar_t>(reinterpret_cast<ULONG_PTR>(text)))));
    for (wchar_t* current = text; *current; ++current) *current = towlower(*current);
    return text;
}

BOOL WINAPI BridgeIsCharAlphaW(WCHAR value) { return iswalpha(value) != 0; }
BOOL WINAPI BridgeIsCharAlphaNumericW(WCHAR value) { return iswalnum(value) != 0; }
BOOL WINAPI BridgeIsCharLowerW(WCHAR value) { return iswlower(value) != 0; }

UINT WINAPI BridgeGetDoubleClickTime() { return 500; }
UINT WINAPI BridgeGetCaretBlinkTime() { return 530; }

int WINAPI BridgeGetWindowTextA(HWND window, LPSTR buffer, int count)
{
    const int length = BridgeGetWindowTextLengthW(window);
    if (length < 0 || !buffer || count <= 0) return 0;
    std::vector<wchar_t> wide(static_cast<size_t>(length) + 1);
    BridgeGetWindowTextW(window, wide.data(), static_cast<int>(wide.size()));
    return WideToAnsi(wide.data(), buffer, count);
}

UINT WINAPI BridgeGetDlgItemTextA(HWND parent, int identifier, LPSTR buffer, int count)
{
    const HWND child = BridgeGetDlgItem(parent, identifier);
    return child ? static_cast<UINT>(BridgeGetWindowTextA(child, buffer, count)) : 0;
}

UINT WINAPI BridgeGetDlgItemInt(HWND parent, int identifier, BOOL* translated, BOOL signedValue)
{
    wchar_t text[64]{};
    BridgeGetDlgItemTextW(parent, identifier, text, static_cast<int>(_countof(text)));
    wchar_t* end = nullptr;
    const unsigned long value = signedValue
        ? static_cast<unsigned long>(wcstol(text, &end, 10))
        : wcstoul(text, &end, 10);
    const BOOL success = end != text && *end == L'\0';
    if (translated) *translated = success;
    return success ? static_cast<UINT>(value) : 0;
}

BOOL WINAPI BridgeSetDlgItemInt(HWND parent, int identifier, UINT value, BOOL signedValue)
{
    wchar_t text[32]{};
    if (signedValue) swprintf_s(text, L"%d", static_cast<int>(value));
    else swprintf_s(text, L"%u", value);
    return BridgeSetDlgItemTextW(parent, identifier, text);
}

int WINAPI BridgeLoadStringA(HINSTANCE instance, UINT identifier, LPSTR buffer, int count)
{
    if (!buffer || count <= 0) return 0;
    wchar_t wide[4096]{};
    const int length = BridgeLoadStringW(instance, identifier, wide, static_cast<int>(_countof(wide)));
    return length > 0 ? WideToAnsi(wide, buffer, count) : 0;
}

int WINAPI BridgeMessageBoxA(HWND owner, LPCSTR text, LPCSTR caption, UINT type)
{
    const std::wstring wideText = AnsiToWide(text);
    const std::wstring wideCaption = AnsiToWide(caption);
    return BridgeMessageBoxW(owner, text ? wideText.c_str() : nullptr,
        caption ? wideCaption.c_str() : nullptr, type);
}

BOOL WINAPI BridgeAppendMenuA(HMENU menu, UINT flags, UINT_PTR identifier, LPCSTR text)
{
    if ((flags & (MF_BITMAP | MF_OWNERDRAW | MF_SEPARATOR)) != 0)
        return BridgeAppendMenuW(menu, flags, identifier, reinterpret_cast<LPCWSTR>(text));
    const std::wstring wide = AnsiToWide(text);
    return BridgeAppendMenuW(menu, flags, identifier, text ? wide.c_str() : nullptr);
}

int WINAPI BridgeWsPrintfW(LPWSTR buffer, LPCWSTR format, ...)
{
    if (!buffer || !format) return 0;
    va_list arguments;
    va_start(arguments, format);
    const int result = _vsnwprintf_s(buffer, 1024, _TRUNCATE, format, arguments);
    va_end(arguments);
    return result < 0 ? 0 : result;
}

int WINAPI BridgeGetClassNameW(HWND window, LPWSTR buffer, int count)
{
    GuestWindowManager* manager = CurrentManagerOrFail();
    DWORD error = ERROR_SUCCESS;
    const int result = manager ? manager->GetGuestClassName(window, buffer, count, &error) : 0;
    BridgeSetLastError(error);
    return result;
}

int WINAPI BridgeGetClassNameA(HWND window, LPSTR buffer, int count)
{
    wchar_t wide[256]{};
    return BridgeGetClassNameW(window, wide, static_cast<int>(_countof(wide))) > 0
        ? WideToAnsi(wide, buffer, count) : 0;
}

HWND WINAPI BridgeFindWindowExW(HWND parent, HWND after, LPCWSTR className, LPCWSTR title)
{
    GuestWindowManager* manager = CurrentManagerOrFail();
    DWORD error = ERROR_SUCCESS;
    const HWND result = manager ? manager->FindGuestWindowEx(parent, after, className, title, &error) : nullptr;
    BridgeSetLastError(error);
    return result;
}

BOOL WINAPI BridgeIsChild(HWND parent, HWND window)
{
    GuestWindowManager* manager = CurrentManagerOrFail();
    return manager && manager->IsGuestChild(parent, window);
}

HWND WINAPI BridgeSetParent(HWND window, HWND parent)
{
    GuestWindowManager* manager = CurrentManagerOrFail();
    DWORD error = ERROR_SUCCESS;
    const HWND result = manager ? manager->SetGuestParent(window, parent, &error) : nullptr;
    BridgeSetLastError(error);
    return result;
}

HWND WINAPI BridgeGetWindow(HWND window, UINT command)
{
    GuestWindowManager* manager = CurrentManagerOrFail();
    DWORD error = ERROR_SUCCESS;
    const HWND result = manager ? manager->GetGuestWindowRelationship(window, command, &error) : nullptr;
    BridgeSetLastError(error);
    return result;
}

BOOL InvokeGuestWindowEnumProc(GuestWndEnumProc callback, HWND window, LPARAM parameter)
{
    __try { return callback(window, parameter); }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        BridgeSetLastError(ERROR_EXCEPTION_IN_SERVICE);
        return FALSE;
    }
}

BOOL WINAPI BridgeEnumChildWindows(HWND parent, GuestWndEnumProc callback, LPARAM parameter)
{
    GuestWindowManager* manager = CurrentManagerOrFail();
    if (!manager || !callback) { BridgeSetLastError(ERROR_INVALID_PARAMETER); return FALSE; }
    for (const HWND window : manager->SnapshotGuestWindows(parent, true))
        if (!InvokeGuestWindowEnumProc(callback, window, parameter)) return FALSE;
    BridgeSetLastError(ERROR_SUCCESS);
    return TRUE;
}

BOOL WINAPI BridgeEnumThreadWindows(DWORD, GuestWndEnumProc callback, LPARAM parameter)
{
    GuestWindowManager* manager = CurrentManagerOrFail();
    if (!manager || !callback) { BridgeSetLastError(ERROR_INVALID_PARAMETER); return FALSE; }
    for (const HWND window : manager->SnapshotGuestWindows(nullptr, false))
        if (!InvokeGuestWindowEnumProc(callback, window, parameter)) return FALSE;
    BridgeSetLastError(ERROR_SUCCESS);
    return TRUE;
}

HWND WINAPI BridgeGetActiveWindow()
{
    GuestWindowManager* manager = CurrentGuestWindowManager();
    return manager ? manager->GetGuestForegroundWindow() : nullptr;
}

HWND WINAPI BridgeGetLastActivePopup(HWND window)
{
    return BridgeIsWindow(window) ? window : nullptr;
}

BOOL WINAPI BridgeBringWindowToTop(HWND window)
{
    return BridgeSetWindowPos(window, HWND_TOP, 0, 0, 0, 0,
        SWP_NOMOVE | SWP_NOSIZE | SWP_SHOWWINDOW);
}

DWORD WINAPI BridgeGetWindowThreadProcessId(HWND window, LPDWORD processId)
{
    if (!BridgeIsWindow(window))
    {
        if (processId) *processId = 0;
        BridgeSetLastError(ERROR_INVALID_WINDOW_HANDLE);
        return 0;
    }
    if (processId) *processId = BridgeGetCurrentProcessId();
    BridgeSetLastError(ERROR_SUCCESS);
    return BridgeGetCurrentThreadId();
}

BOOL WINAPI BridgeSetPropW(HWND window, LPCWSTR name, HANDLE data)
{
    if (!BridgeIsWindow(window) || !name) { BridgeSetLastError(ERROR_INVALID_PARAMETER); return FALSE; }
    std::lock_guard<std::mutex> guard(g_windowPropertyLock);
    g_windowProperties[reinterpret_cast<ULONG_PTR>(window)][PropertyKey(name)] = data;
    BridgeSetLastError(ERROR_SUCCESS);
    return TRUE;
}

HANDLE WINAPI BridgeGetPropW(HWND window, LPCWSTR name)
{
    if (!name) return nullptr;
    std::lock_guard<std::mutex> guard(g_windowPropertyLock);
    const auto owner = g_windowProperties.find(reinterpret_cast<ULONG_PTR>(window));
    if (owner == g_windowProperties.end()) return nullptr;
    const auto property = owner->second.find(PropertyKey(name));
    return property == owner->second.end() ? nullptr : property->second;
}

HANDLE WINAPI BridgeRemovePropW(HWND window, LPCWSTR name)
{
    if (!name) return nullptr;
    std::lock_guard<std::mutex> guard(g_windowPropertyLock);
    const auto owner = g_windowProperties.find(reinterpret_cast<ULONG_PTR>(window));
    if (owner == g_windowProperties.end()) return nullptr;
    const auto property = owner->second.find(PropertyKey(name));
    if (property == owner->second.end()) return nullptr;
    const HANDLE value = property->second;
    owner->second.erase(property);
    if (owner->second.empty()) g_windowProperties.erase(owner);
    return value;
}

BOOL WINAPI BridgeCreateCaret(HWND window, HBITMAP bitmap, int width, int height)
{
    if (!BridgeIsWindow(window)) { BridgeSetLastError(ERROR_INVALID_WINDOW_HANDLE); return FALSE; }
    std::lock_guard<std::mutex> guard(g_caretLock);
    g_caretWindow = window;
    g_caretSize.cx = bitmap ? 0 : (std::max)(1, width);
    g_caretSize.cy = bitmap ? 0 : (std::max)(1, height);
    g_caretShowCount = -1;
    BridgeSetLastError(ERROR_SUCCESS);
    return TRUE;
}

BOOL WINAPI BridgeDestroyCaret()
{
    std::lock_guard<std::mutex> guard(g_caretLock);
    g_caretWindow = nullptr;
    g_caretShowCount = -1;
    BridgeSetLastError(ERROR_SUCCESS);
    return TRUE;
}

BOOL WINAPI BridgeSetCaretPos(int x, int y)
{
    std::lock_guard<std::mutex> guard(g_caretLock);
    if (!g_caretWindow) { BridgeSetLastError(ERROR_INVALID_FUNCTION); return FALSE; }
    g_caretPosition = { x, y };
    BridgeInvalidateRect(g_caretWindow, nullptr, FALSE);
    return TRUE;
}

BOOL WINAPI BridgeShowCaret(HWND window)
{
    std::lock_guard<std::mutex> guard(g_caretLock);
    if (!g_caretWindow || (window && window != g_caretWindow)) return FALSE;
    ++g_caretShowCount;
    BridgeInvalidateRect(g_caretWindow, nullptr, FALSE);
    return TRUE;
}

BOOL WINAPI BridgeHideCaret(HWND window)
{
    std::lock_guard<std::mutex> guard(g_caretLock);
    if (!g_caretWindow || (window && window != g_caretWindow)) return FALSE;
    --g_caretShowCount;
    BridgeInvalidateRect(g_caretWindow, nullptr, FALSE);
    return TRUE;
}

int WINAPI BridgeShowCursor(BOOL show)
{
    return show ? ++g_cursorShowCount : --g_cursorShowCount;
}

BOOL WINAPI BridgeGetKeyboardState(PBYTE state)
{
    if (!state) { BridgeSetLastError(ERROR_INVALID_PARAMETER); return FALSE; }
    for (int key = 0; key < 256; ++key) state[key] = static_cast<BYTE>(BridgeGetKeyState(key) >> 8);
    BridgeSetLastError(ERROR_SUCCESS);
    return TRUE;
}

LONG WINAPI BridgeGetMessageTime() { return static_cast<LONG>(BridgeGetTickCount()); }

HANDLE WINAPI BridgeGetClipboardData(UINT format)
{
    std::lock_guard<std::mutex> guard(g_menuLock);
    const auto found = g_clipboardData.find(format);
    if (found == g_clipboardData.end()) { BridgeSetLastError(ERROR_NOT_FOUND); return nullptr; }
    BridgeSetLastError(ERROR_SUCCESS);
    return found->second;
}

BOOL WINAPI BridgeAddClipboardFormatListener(HWND window) { return BridgeIsWindow(window); }
BOOL WINAPI BridgeRemoveClipboardFormatListener(HWND window) { return BridgeIsWindow(window); }
HWND WINAPI BridgeSetClipboardViewer(HWND window) { return BridgeIsWindow(window) ? nullptr : nullptr; }
BOOL WINAPI BridgeChangeClipboardChain(HWND remove, HWND) { return BridgeIsWindow(remove); }

HMONITOR WINAPI BridgeMonitorFromPoint(POINT point, DWORD flags)
{
    RECT screen{ 0, 0, GuestMetrics::CurrentScreenWidth(), GuestMetrics::CurrentScreenHeight() };
    return BridgePtInRect(&screen, point) || flags != MONITOR_DEFAULTTONULL
        ? reinterpret_cast<HMONITOR>(static_cast<ULONG_PTR>(1)) : nullptr;
}

HMONITOR WINAPI BridgeMonitorFromRect(const RECT* rect, DWORD flags)
{
    if (!rect) return nullptr;
    RECT intersection{};
    RECT screen{ 0, 0, GuestMetrics::CurrentScreenWidth(), GuestMetrics::CurrentScreenHeight() };
    return BridgeIntersectRect(&intersection, rect, &screen) || flags != MONITOR_DEFAULTTONULL
        ? reinterpret_cast<HMONITOR>(static_cast<ULONG_PTR>(1)) : nullptr;
}

BOOL WINAPI BridgeEnumDisplayMonitors(HDC dc, const RECT* clip, GuestMonitorEnumProc callback, LPARAM parameter)
{
    if (!callback) return FALSE;
    RECT screen{ 0, 0, GuestMetrics::CurrentScreenWidth(), GuestMetrics::CurrentScreenHeight() };
    if (clip)
    {
        RECT intersection{};
        if (!BridgeIntersectRect(&intersection, &screen, clip)) return TRUE;
        screen = intersection;
    }
    __try { return callback(reinterpret_cast<HMONITOR>(static_cast<ULONG_PTR>(1)), dc, &screen, parameter); }
    __except (EXCEPTION_EXECUTE_HANDLER) { BridgeSetLastError(ERROR_EXCEPTION_IN_SERVICE); return FALSE; }
}

BOOL WINAPI BridgeSystemParametersInfoA(UINT action, UINT parameter, PVOID value, UINT flags)
{
    return BridgeSystemParametersInfoW(action, parameter, value, flags);
}

HDC WINAPI BridgeGetWindowDC(HWND window) { return BridgeGetDC(window); }
HDC WINAPI BridgeGetDCEx(HWND window, HRGN, DWORD) { return BridgeGetDC(window); }

BOOL WINAPI BridgeValidateRect(HWND window, const RECT*)
{
    // The retained compositor has no exposed update-region object. Painting
    // consumes invalidation atomically, so validation is equivalent to an
    // immediate update of the current virtual surface.
    return BridgeUpdateWindow(window);
}

BOOL WINAPI BridgeRedrawWindow(HWND window, const RECT* update, HRGN, UINT flags)
{
    const BOOL erase = (flags & RDW_ERASE) != 0;
    if ((flags & RDW_INVALIDATE) != 0 && !BridgeInvalidateRect(window, update, erase)) return FALSE;
    return (flags & (RDW_UPDATENOW | RDW_ERASENOW)) != 0 ? BridgeUpdateWindow(window) : TRUE;
}

BOOL WINAPI BridgeTrackMouseEvent(GuestTrackMouseEvent* event)
{
    if (!event || event->cbSize < sizeof(GuestTrackMouseEvent))
    {
        BridgeSetLastError(ERROR_INVALID_PARAMETER);
        return FALSE;
    }
    return BridgeIsWindow(event->hwndTrack);
}

void WINAPI BridgeNotifyWinEvent(DWORD, HWND, LONG, LONG) {}

struct DeferredWindowPosition final
{
    struct Entry { HWND window; HWND after; int x; int y; int width; int height; UINT flags; };
    std::vector<Entry> entries;
};

HANDLE WINAPI BridgeBeginDeferWindowPos(int count)
{
    auto positions = new (std::nothrow) DeferredWindowPosition();
    if (!positions) { BridgeSetLastError(ERROR_NOT_ENOUGH_MEMORY); return nullptr; }
    if (count > 0) positions->entries.reserve(static_cast<size_t>(count));
    return reinterpret_cast<HANDLE>(positions);
}

HANDLE WINAPI BridgeDeferWindowPos(HANDLE handle, HWND window, HWND after, int x, int y,
    int width, int height, UINT flags)
{
    auto positions = reinterpret_cast<DeferredWindowPosition*>(handle);
    if (!positions || !BridgeIsWindow(window)) { BridgeSetLastError(ERROR_INVALID_PARAMETER); return nullptr; }
    try { positions->entries.push_back({ window, after, x, y, width, height, flags }); }
    catch (...) { BridgeSetLastError(ERROR_NOT_ENOUGH_MEMORY); return nullptr; }
    return handle;
}

BOOL WINAPI BridgeEndDeferWindowPos(HANDLE handle)
{
    std::unique_ptr<DeferredWindowPosition> positions(reinterpret_cast<DeferredWindowPosition*>(handle));
    if (!positions) { BridgeSetLastError(ERROR_INVALID_HANDLE); return FALSE; }
    for (const auto& entry : positions->entries)
        if (!BridgeSetWindowPos(entry.window, entry.after, entry.x, entry.y,
            entry.width, entry.height, entry.flags)) return FALSE;
    return TRUE;
}

std::mutex g_hookLock;
ULONG_PTR g_nextHook = 0x7fff9000;
std::unordered_map<ULONG_PTR, GuestHookProc> g_hooks;

HANDLE WINAPI BridgeSetWindowsHookExW(int, GuestHookProc procedure, HINSTANCE, DWORD)
{
    if (!procedure) { BridgeSetLastError(ERROR_INVALID_PARAMETER); return nullptr; }
    std::lock_guard<std::mutex> guard(g_hookLock);
    const ULONG_PTR token = ++g_nextHook;
    g_hooks[token] = procedure;
    return reinterpret_cast<HANDLE>(token);
}

BOOL WINAPI BridgeUnhookWindowsHookEx(HANDLE hook)
{
    std::lock_guard<std::mutex> guard(g_hookLock);
    return g_hooks.erase(reinterpret_cast<ULONG_PTR>(hook)) != 0;
}

LRESULT WINAPI BridgeCallNextHookEx(HANDLE, int, WPARAM, LPARAM) { return 0; }

struct GuestScrollState final
{
    int minimum = 0;
    int maximum = 100;
    UINT page = 0;
    int position = 0;
    int trackPosition = 0;
    bool visible = true;
};
std::mutex g_scrollLock;
std::unordered_map<ULONG_PTR, std::array<GuestScrollState, 2>> g_scrollStates;

int ScrollIndex(int bar) { return bar == SB_VERT ? 1 : 0; }

int WINAPI BridgeSetScrollInfo(HWND window, int bar, const GuestScrollInfo* source, BOOL redraw)
{
    if (!BridgeIsWindow(window) || !source || source->cbSize < sizeof(GuestScrollInfo) ||
        (bar != SB_HORZ && bar != SB_VERT && bar != SB_CTL))
    {
        BridgeSetLastError(ERROR_INVALID_PARAMETER);
        return 0;
    }
    int result = 0;
    {
        std::lock_guard<std::mutex> guard(g_scrollLock);
        auto& state = g_scrollStates[reinterpret_cast<ULONG_PTR>(window)][ScrollIndex(bar)];
        if (source->fMask & SIF_RANGE) { state.minimum = source->nMin; state.maximum = (std::max)(source->nMin, source->nMax); }
        if (source->fMask & SIF_PAGE) state.page = source->nPage;
        if (source->fMask & SIF_POS) state.position = source->nPos;
        if (source->fMask & SIF_TRACKPOS) state.trackPosition = source->nTrackPos;
        const int maximumPosition = state.page > 0
            ? (std::max)(state.minimum, state.maximum - static_cast<int>(state.page) + 1)
            : state.maximum;
        state.position = (std::max)(state.minimum, (std::min)(state.position, maximumPosition));
        result = state.position;
    }
    if (bar == SB_CTL) BridgeSendMessageW(window, 0x00e0u, static_cast<WPARAM>(result), redraw);
    else if (redraw) BridgeInvalidateRect(window, nullptr, FALSE);
    BridgeSetLastError(ERROR_SUCCESS);
    return result;
}

BOOL WINAPI BridgeGetScrollInfo(HWND window, int bar, GuestScrollInfo* target)
{
    if (!BridgeIsWindow(window) || !target || target->cbSize < sizeof(GuestScrollInfo) ||
        (bar != SB_HORZ && bar != SB_VERT && bar != SB_CTL))
    {
        BridgeSetLastError(ERROR_INVALID_PARAMETER);
        return FALSE;
    }
    std::lock_guard<std::mutex> guard(g_scrollLock);
    const auto found = g_scrollStates.find(reinterpret_cast<ULONG_PTR>(window));
    const GuestScrollState state = found == g_scrollStates.end()
        ? GuestScrollState{} : found->second[ScrollIndex(bar)];
    if (target->fMask & SIF_RANGE) { target->nMin = state.minimum; target->nMax = state.maximum; }
    if (target->fMask & SIF_PAGE) target->nPage = state.page;
    if (target->fMask & SIF_POS) target->nPos = state.position;
    if (target->fMask & SIF_TRACKPOS) target->nTrackPos = state.trackPosition;
    BridgeSetLastError(ERROR_SUCCESS);
    return TRUE;
}

int WINAPI BridgeGetScrollPos(HWND window, int bar)
{
    GuestScrollInfo info{};
    info.cbSize = sizeof(info); info.fMask = SIF_POS;
    return BridgeGetScrollInfo(window, bar, &info) ? info.nPos : 0;
}

BOOL WINAPI BridgeGetScrollRange(HWND window, int bar, LPINT minimum, LPINT maximum)
{
    GuestScrollInfo info{};
    info.cbSize = sizeof(info); info.fMask = SIF_RANGE;
    if (!BridgeGetScrollInfo(window, bar, &info)) return FALSE;
    if (minimum) *minimum = info.nMin;
    if (maximum) *maximum = info.nMax;
    return TRUE;
}

BOOL WINAPI BridgeSetScrollRange(HWND window, int bar, int minimum, int maximum, BOOL redraw)
{
    GuestScrollInfo info{};
    info.cbSize = sizeof(info); info.fMask = SIF_RANGE;
    info.nMin = minimum; info.nMax = maximum;
    BridgeSetScrollInfo(window, bar, &info, redraw);
    return BridgeGetLastError() == ERROR_SUCCESS;
}

BOOL WINAPI BridgeShowScrollBar(HWND window, int bar, BOOL show)
{
    if (!BridgeIsWindow(window)) return FALSE;
    std::lock_guard<std::mutex> guard(g_scrollLock);
    auto& states = g_scrollStates[reinterpret_cast<ULONG_PTR>(window)];
    if (bar == SB_BOTH) states[0].visible = states[1].visible = show != FALSE;
    else states[ScrollIndex(bar)].visible = show != FALSE;
    BridgeInvalidateRect(window, nullptr, FALSE);
    return TRUE;
}

BOOL WINAPI BridgeScrollWindow(HWND window, int, int, const RECT*, const RECT*)
{
    return BridgeInvalidateRect(window, nullptr, FALSE);
}

BOOL WINAPI BridgeSetLayeredWindowAttributes(HWND window, COLORREF, BYTE, DWORD)
{
    return BridgeIsWindow(window);
}

std::atomic<ULONG_PTR> g_lockedUpdateWindow{ 0 };
BOOL WINAPI BridgeLockWindowUpdate(HWND window)
{
    const ULONG_PTR requested = reinterpret_cast<ULONG_PTR>(window);
    if (!requested) { g_lockedUpdateWindow.store(0); return TRUE; }
    ULONG_PTR expected = 0;
    return g_lockedUpdateWindow.compare_exchange_strong(expected, requested) || expected == requested;
}

BOOL WINAPI BridgeFlashWindowEx(GuestFlashWindowInfo* info)
{
    return info && info->cbSize >= sizeof(GuestFlashWindowInfo) && BridgeIsWindow(info->hwnd);
}

INT_PTR WINAPI BridgeDialogBoxIndirectParamW(HINSTANCE instance, const void* data,
    HWND parent, DLGPROC procedure, LPARAM parameter)
{
    return ShowGuestDialogFromTemplate(instance, data, parent, procedure, parameter,
        true, nullptr);
}

HWND WINAPI BridgeCreateDialogIndirectParamW(HINSTANCE instance, const void* data,
    HWND parent, DLGPROC procedure, LPARAM parameter)
{
    HWND created = nullptr;
    const INT_PTR result = ShowGuestDialogFromTemplate(instance, data, parent, procedure,
        parameter, false, &created);
    return result < 0 ? nullptr : created;
}

BOOL WINAPI BridgeInsertMenuW(HMENU menu, UINT item, UINT flags, UINT_PTR identifier, LPCWSTR text)
{
    GuestMenuItemInfoW info{};
    info.cbSize = sizeof(info);
    info.fMask = kMiimId | kMiimFtype | kMiimState;
    info.wID = static_cast<UINT>(identifier);
    info.fType = flags;
    info.fState = flags;
    if (flags & MF_POPUP) { info.fMask |= kMiimSubmenu; info.hSubMenu = reinterpret_cast<HMENU>(identifier); }
    if (text && !(flags & (MF_BITMAP | MF_OWNERDRAW | MF_SEPARATOR)))
    {
        info.fMask |= kMiimString;
        info.dwTypeData = const_cast<LPWSTR>(text);
        info.cch = static_cast<UINT>(wcslen(text));
    }
    return BridgeInsertMenuItemW(menu, item, (flags & MF_BYPOSITION) != 0, &info);
}

BOOL WINAPI BridgeModifyMenuW(HMENU menu, UINT item, UINT flags, UINT_PTR identifier, LPCWSTR text)
{
    GuestMenuItemInfoW info{};
    info.cbSize = sizeof(info);
    info.fMask = kMiimId | kMiimFtype | kMiimState;
    info.wID = static_cast<UINT>(identifier);
    info.fType = flags;
    info.fState = flags;
    if (flags & MF_POPUP) { info.fMask |= kMiimSubmenu; info.hSubMenu = reinterpret_cast<HMENU>(identifier); }
    if (text && !(flags & (MF_BITMAP | MF_OWNERDRAW | MF_SEPARATOR)))
    {
        info.fMask |= kMiimString;
        info.dwTypeData = const_cast<LPWSTR>(text);
        info.cch = static_cast<UINT>(wcslen(text));
    }
    return BridgeSetMenuItemInfoW(menu, item, (flags & MF_BYPOSITION) != 0, &info);
}

BOOL WINAPI BridgeSetMenuItemBitmaps(HMENU menu, UINT item, UINT flags, HBITMAP unchecked, HBITMAP checked)
{
    GuestMenuItemInfoW info{};
    info.cbSize = sizeof(info); info.fMask = kMiimCheckmarks;
    info.hbmpUnchecked = unchecked; info.hbmpChecked = checked;
    return BridgeSetMenuItemInfoW(menu, item, (flags & MF_BYPOSITION) != 0, &info);
}

int WINAPI BridgeFrameRect(HDC dc, const RECT* rect, HBRUSH brush)
{
    if (!rect) return 0;
    RECT edge{ rect->left, rect->top, rect->right, rect->top + 1 };
    BridgeFillRect(dc, &edge, brush);
    edge = { rect->left, rect->bottom - 1, rect->right, rect->bottom };
    BridgeFillRect(dc, &edge, brush);
    edge = { rect->left, rect->top + 1, rect->left + 1, rect->bottom - 1 };
    BridgeFillRect(dc, &edge, brush);
    edge = { rect->right - 1, rect->top + 1, rect->right, rect->bottom - 1 };
    BridgeFillRect(dc, &edge, brush);
    return 1;
}

BOOL WINAPI BridgeDrawFocusRect(HDC dc, const RECT* rect)
{
    return BridgeFrameRect(dc, rect, BridgeGetSysColorBrush(COLOR_WINDOWTEXT)) != 0;
}

BOOL WINAPI BridgeDrawEdge(HDC dc, LPRECT rect, UINT edge, UINT flags)
{
    if (!rect) return FALSE;
    const int result = BridgeFrameRect(dc, rect, BridgeGetSysColorBrush(
        (edge & (BDR_SUNKENOUTER | BDR_SUNKENINNER)) ? COLOR_BTNSHADOW : COLOR_BTNHIGHLIGHT));
    if ((flags & BF_ADJUST) != 0)
    {
        ++rect->left; ++rect->top; --rect->right; --rect->bottom;
    }
    return result != 0;
}

BOOL WINAPI BridgeDrawFrameControl(HDC dc, LPRECT rect, UINT, UINT)
{
    return BridgeDrawEdge(dc, rect, EDGE_RAISED, BF_RECT);
}

int WINAPI BridgeToAscii(UINT virtualKey, UINT, const BYTE* state, LPWORD characters, UINT)
{
    if (!characters) return 0;
    UINT character = BridgeMapVirtualKeyW(virtualKey, 2);
    if (!character) return 0;
    if (character >= L'A' && character <= L'Z' && (!state || (state[VK_SHIFT] & 0x80) == 0))
        character = static_cast<UINT>(towlower(static_cast<wchar_t>(character)));
    characters[0] = static_cast<WORD>(character);
    return 1;
}

struct GuestAcceleratorTable final { std::vector<GuestAccel> entries; };

HANDLE WINAPI BridgeCreateAcceleratorTableW(GuestAccel* entries, int count)
{
    if (!entries || count <= 0) { BridgeSetLastError(ERROR_INVALID_PARAMETER); return nullptr; }
    auto table = new (std::nothrow) GuestAcceleratorTable();
    if (!table) { BridgeSetLastError(ERROR_NOT_ENOUGH_MEMORY); return nullptr; }
    try { table->entries.assign(entries, entries + count); }
    catch (...) { delete table; BridgeSetLastError(ERROR_NOT_ENOUGH_MEMORY); return nullptr; }
    return reinterpret_cast<HANDLE>(table);
}

BOOL WINAPI BridgeDestroyAcceleratorTable(HANDLE table)
{
    if (!table) return FALSE;
    delete reinterpret_cast<GuestAcceleratorTable*>(table);
    return TRUE;
}

BOOL WINAPI BridgeGetComboBoxInfo(HWND window, GuestComboBoxInfo* info)
{
    if (!info || info->cbSize < sizeof(GuestComboBoxInfo) || !BridgeIsWindow(window)) return FALSE;
    RECT client{};
    if (!BridgeGetClientRect(window, &client)) return FALSE;
    info->rcItem = client;
    info->rcButton = client;
    info->rcButton.left = (std::max)(client.left, client.right - BridgeGetSystemMetrics(SM_CXVSCROLL));
    info->stateButton = 0;
    info->hwndCombo = window;
    info->hwndItem = nullptr;
    info->hwndList = nullptr;
    return TRUE;
}

BOOL WINAPI BridgeGetMenuBarInfo(HWND window, LONG objectId, LONG itemId, GuestMenuBarInfo* info)
{
    if (!info || info->cbSize < sizeof(GuestMenuBarInfo) || !BridgeIsWindow(window) ||
        objectId != OBJID_MENU) return FALSE;
    RECT client{};
    if (!BridgeGetClientRect(window, &client)) return FALSE;
    POINT origin{};
    BridgeClientToScreen(window, &origin);
    info->rcBar = { origin.x, origin.y, origin.x + client.right,
        origin.y + GuestMetrics::MenuHeight };
    info->hMenu = BridgeGetMenu(window);
    info->hwndMenu = window;
    info->fBarFocused = BridgeGetFocus() == window;
    info->fFocused = itemId > 0 ? TRUE : FALSE;
    return info->hMenu != nullptr;
}

int WINAPI BridgeGetUpdateRgn(HWND window, HRGN, BOOL erase)
{
    if (!BridgeIsWindow(window)) return ERROR;
    if (erase) BridgeUpdateWindow(window);
    return NULLREGION;
}

void WINAPI BridgeMouseEvent(DWORD, DWORD, DWORD, DWORD, ULONG_PTR)
{
    // Host pointer events already enter through CoreWindow. Synthetic desktop
    // injection cannot escape the package and therefore intentionally has no
    // host-side effect.
}

HICON WINAPI BridgeCreateIconIndirect(GuestIconInfo* info)
{
    if (!info) { BridgeSetLastError(ERROR_INVALID_PARAMETER); return nullptr; }
    GuestWindowManager* manager = CurrentManagerOrFail();
    if (!manager) return nullptr;
    MiniGdi::Surface* source = info->hbmColor
        ? manager->Gdi().GetBitmapSurface(FromGuestObject(info->hbmColor)) : nullptr;
    if (!source && info->hbmMask)
        source = manager->Gdi().GetBitmapSurface(FromGuestObject(info->hbmMask));
    if (!source) { BridgeSetLastError(ERROR_INVALID_HANDLE); return nullptr; }
    return StoreGuestIconPixels(*source);
}

BOOL WINAPI BridgeGetIconInfo(HICON icon, GuestIconInfo* info)
{
    if (!info) return FALSE;
    MiniGdi::Surface source;
    if (!CopyGuestIconPixels(icon, &source)) return FALSE;
    GuestWindowManager* manager = CurrentManagerOrFail();
    if (!manager) return FALSE;
    const auto color = manager->Gdi().CreateBitmap(source.Width(), source.Height(), MiniGdi::Transparent);
    const auto mask = manager->Gdi().CreateBitmap(source.Width(), source.Height(), MiniGdi::OpaqueBlack);
    MiniGdi::Surface* target = manager->Gdi().GetBitmapSurface(color);
    if (color == MiniGdi::InvalidObject || mask == MiniGdi::InvalidObject || !target) return FALSE;
    target->Pixels() = source.Pixels();
    info->fIcon = TRUE;
    info->xHotspot = 0; info->yHotspot = 0;
    info->hbmColor = reinterpret_cast<HBITMAP>(static_cast<ULONG_PTR>(color));
    info->hbmMask = reinterpret_cast<HBITMAP>(static_cast<ULONG_PTR>(mask));
    return TRUE;
}

BOOL WINAPI BridgeUnregisterClassW(LPCWSTR className, HINSTANCE instance)
{
    GuestWindowManager* manager = CurrentManagerOrFail();
    DWORD error = ERROR_SUCCESS;
    const BOOL result = manager ? manager->UnregisterGuestClass(className, instance, &error) : FALSE;
    BridgeSetLastError(error);
    return result;
}

BOOL WINAPI BridgeWinHelpW(HWND, LPCWSTR, UINT, ULONG_PTR)
{
    BridgeSetLastError(ERROR_CALL_NOT_IMPLEMENTED);
    return FALSE;
}

HWND WINAPI BridgeChildWindowFromPoint(HWND parent, POINT point)
{
    return BridgeChildWindowFromPointEx(parent, point, 0);
}

HMENU WINAPI BridgeGetSystemMenu(HWND, BOOL)
{
    BridgeSetLastError(ERROR_SUCCESS);
    return nullptr;
}

UINT WINAPI BridgeRegisterWindowMessageW(LPCWSTR text)
{
    if (!text || !*text) { BridgeSetLastError(ERROR_INVALID_PARAMETER); return 0; }
    std::wstring key(text);
    std::transform(key.begin(), key.end(), key.begin(), [](wchar_t value)
    {
        return static_cast<wchar_t>(towlower(value));
    });
    std::lock_guard<std::mutex> guard(g_registeredMessageLock);
    const auto found = g_registeredMessages.find(key);
    if (found != g_registeredMessages.end()) return found->second;
    if (g_nextRegisteredMessage > 0xffff) { BridgeSetLastError(ERROR_NOT_ENOUGH_MEMORY); return 0; }
    const UINT message = g_nextRegisteredMessage++;
    g_registeredMessages.emplace(std::move(key), message);
    return message;
}

int WINAPI BridgeSetScrollPos(HWND window, int bar, int position, BOOL redraw)
{
    if (bar == SB_CTL)
        BridgeSendMessageW(window, 0x00e0u, static_cast<WPARAM>(position), redraw);
    else if (redraw)
        BridgeInvalidateRect(window, nullptr, FALSE);
    return 0;
}

HWND WINAPI BridgeCreateDialogParamW(
    HINSTANCE, LPCWSTR, HWND, DLGPROC, LPARAM)
{
    BridgeSetLastError(ERROR_CALL_NOT_IMPLEMENTED);
    return nullptr;
}

int WINAPI BridgeDrawTextExW(
    HDC dc, LPWSTR text, int count, LPRECT rect, UINT format, GuestDrawTextParams* parameters)
{
    if (parameters && parameters->cbSize >= sizeof(GuestDrawTextParams))
        parameters->uiLengthDrawn = count < 0 && text ? static_cast<UINT>(wcslen(text)) :
            static_cast<UINT>((std::max)(0, count));
    return BridgeDrawTextW(dc, text, count, rect, format);
}

HWND WINAPI BridgeGetAncestor(HWND window, UINT flags)
{
    if (!BridgeIsWindow(window)) return nullptr;
    if (flags == GA_PARENT) return BridgeGetParent(window);
    HWND current = window;
    HWND parent = nullptr;
    while ((parent = BridgeGetParent(current)) != nullptr) current = parent;
    return current;
}

HWND WINAPI BridgeFindWindowW(LPCWSTR, LPCWSTR)
{
    BridgeSetLastError(ERROR_SUCCESS);
    return nullptr;
}

BOOL WINAPI BridgeSetForegroundWindow(HWND window)
{
    if (!BridgeIsWindow(window)) { BridgeSetLastError(ERROR_INVALID_WINDOW_HANDLE); return FALSE; }
    BridgeSetFocus(window);
    BridgeSetLastError(ERROR_SUCCESS);
    return TRUE;
}

HWINEVENTHOOK WINAPI BridgeSetWinEventHook(
    DWORD, DWORD, HMODULE, GuestWinEventProc, DWORD, DWORD, DWORD)
{
    return reinterpret_cast<HWINEVENTHOOK>(static_cast<ULONG_PTR>(1));
}

BOOL WINAPI BridgeUnhookWinEvent(HWINEVENTHOOK hook)
{
    return hook ? TRUE : FALSE;
}

LPWSTR WINAPI BridgeCharNextW(LPCWSTR text)
{
    if (!text) return nullptr;
    return const_cast<LPWSTR>(*text ? text + 1 : text);
}

HKL WINAPI BridgeGetKeyboardLayout(DWORD)
{
    return reinterpret_cast<HKL>(static_cast<ULONG_PTR>(0x04090409));
}

HWND WINAPI BridgeGetForegroundWindow()
{
    GuestWindowManager* manager = CurrentGuestWindowManager();
    return manager ? manager->GetGuestForegroundWindow() : nullptr;
}

BOOL WINAPI BridgeMessageBeep(UINT) { return TRUE; }
BOOL WINAPI BridgeIsIconic(HWND) { return FALSE; }
BOOL WINAPI BridgeIsClipboardFormatAvailable(UINT) { return FALSE; }

HWND WINAPI BridgeSetActiveWindow(HWND window)
{
    const HWND previous = BridgeGetForegroundWindow();
    if (window) BridgeSetForegroundWindow(window);
    return previous;
}
}
