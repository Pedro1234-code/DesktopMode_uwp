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

    std::mutex g_registeredMessageLock;
    std::unordered_map<std::wstring, UINT> g_registeredMessages;
    UINT g_nextRegisteredMessage = 0xc000;

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
