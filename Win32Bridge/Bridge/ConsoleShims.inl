namespace {
    bool CopyConsoleBytes(void* target, const void* source, size_t count)
    {
        __try { if (count) memcpy(target, source, count); return true; }
        __except(EXCEPTION_EXECUTE_HANDLER) { return false; }
    }
}
namespace Win32Bridge { namespace Bridge {
BOOL WINAPI BridgeGetCPInfo(UINT, LPCPINFO);
namespace {
    BOOL ConsoleRead(HANDLE handle, LPVOID buffer, DWORD count, LPDWORD read, LPVOID control, bool wide)
    {
        if (read) *read = 0;
        if ((!buffer && count) || !read) { SetGuestLastError(ERROR_INVALID_PARAMETER); return FALSE; }
        if (control) { SetGuestLastError(ERROR_NOT_SUPPORTED); return FALSE; }
        auto kernel = CurrentGuestKernelContext();
        if (!kernel) { SetGuestLastError(ERROR_INVALID_HANDLE); return FALSE; }
        std::vector<wchar_t> wideBuffer(wide ? count : 0);
        std::vector<char> byteBuffer(wide ? 0 : count);
        for (;;)
        {
            DWORD controlCode = 0;
            if (kernel->Console().TakeControl(&controlCode))
            {
                bool ignored = false, handled = false;
                auto handlers = kernel->Console().ControlHandlers(&ignored);
                if (controlCode == 0 && ignored) continue;
                for (auto callback : handlers)
                {
                    BOOL result = FALSE;
                    if (InvokeCrtFunction<BOOL>(&result, callback, controlCode) && result) { handled = true; break; }
                }
                if (!handled) RequestGuestExit(0xc000013au, false);
                SetGuestLastError(ERROR_OPERATION_ABORTED); return FALSE;
            }
            DWORD error = ERROR_SUCCESS, completed = 0;
            const bool result = wide ? kernel->Console().Read(handle, wideBuffer.data(), count, &completed, &error)
                : kernel->Console().ReadBytes(handle, byteBuffer.data(), count, &completed, &error);
            if (result)
            {
                const void* source = wide ? static_cast<const void*>(wideBuffer.data()) : static_cast<const void*>(byteBuffer.data());
                if (!CopyConsoleBytes(buffer, source, static_cast<size_t>(completed) * (wide ? sizeof(wchar_t) : 1)))
                { SetGuestLastError(ERROR_NOACCESS); return FALSE; }
                *read = completed; SetGuestLastError(ERROR_SUCCESS); return TRUE;
            }
            if (error != ERROR_NO_DATA) { SetGuestLastError(error); return FALSE; }
            // Console reads block without starving messages/synchronous calls
            // delivered to this guest thread. No console lock crosses callbacks.
            if (auto windows = CurrentGuestWindowManager())
            {
                GuestAbi::Message message{};
                unsigned dispatched = 0;
                while (dispatched++ < 64 && windows->PeekGuestMessage(&message, nullptr, 0, 0, 1))
                {
                    if (message.message == WM_QUIT)
                    {
                        windows->PostGuestQuitMessage(static_cast<int>(message.wParam));
                        SetGuestLastError(ERROR_OPERATION_ABORTED); return FALSE;
                    }
                    windows->TranslateGuestMessage(&message); windows->DispatchGuestMessage(&message, nullptr);
                }
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(8));
        }
    }
}
BOOL WINAPI BridgeAllocConsole()
{
    auto kernel = CurrentGuestKernelContext(); DWORD error = ERROR_INVALID_FUNCTION;
    const bool result = kernel && kernel->Console().Allocate(BridgeGetOEMCP(), &error);
    SetGuestLastError(error); return result ? TRUE : FALSE;
}
BOOL WINAPI BridgeFreeConsole()
{
    auto kernel = CurrentGuestKernelContext(); DWORD error = ERROR_INVALID_FUNCTION;
    const bool result = kernel && kernel->Console().Detach(&error);
    SetGuestLastError(error); return result ? TRUE : FALSE;
}
BOOL WINAPI BridgeReadConsoleW(HANDLE h, LPVOID b, DWORD n, LPDWORD r, LPVOID control)
{
    try
    {
        const BOOL result = ConsoleRead(h, b, n, r, control, true);
        if (!result) ExitGuestThreadOnShutdown();
        return result;
    }
    catch (const std::bad_alloc&) { SetGuestLastError(ERROR_NOT_ENOUGH_MEMORY); return FALSE; }
}
BOOL WINAPI BridgeReadConsoleA(HANDLE h, LPVOID b, DWORD n, LPDWORD r, LPVOID control)
{
    try
    {
        const BOOL result = ConsoleRead(h, b, n, r, control, false);
        if (!result) ExitGuestThreadOnShutdown();
        return result;
    }
    catch (const std::bad_alloc&) { SetGuestLastError(ERROR_NOT_ENOUGH_MEMORY); return FALSE; }
}
BOOL WINAPI BridgeWriteConsoleW(HANDLE h, const VOID* buffer, DWORD count, LPDWORD written, LPVOID reserved)
{
    if (written) *written = 0;
    if ((!buffer && count) || reserved) { SetGuestLastError(ERROR_INVALID_PARAMETER); return FALSE; }
    std::vector<wchar_t> copy(static_cast<size_t>(count));
    if (!CopyConsoleBytes(copy.data(), buffer, static_cast<size_t>(count) * sizeof(wchar_t)))
    { SetGuestLastError(ERROR_NOACCESS); return FALSE; }
    auto kernel = CurrentGuestKernelContext(); DWORD error = ERROR_INVALID_HANDLE;
    const bool result = kernel && kernel->Console().Write(h, copy.data(), count, &error);
    if (result)
    {
        if (written) *written = count;
        if (count) RuntimeDiagnostics::Record(L"CONSOLE OUTPUT: " + std::wstring(copy.data(), count));
    }
    SetGuestLastError(error); return result ? TRUE : FALSE;
}
BOOL WINAPI BridgeWriteConsoleA(HANDLE h, const VOID* buffer, DWORD count, LPDWORD written, LPVOID reserved)
{
    if (written) *written = 0;
    if ((!buffer && count) || reserved || count > INT_MAX) { SetGuestLastError(ERROR_INVALID_PARAMETER); return FALSE; }
    auto kernel = CurrentGuestKernelContext();
    if (!kernel || !kernel->Console().Owns(h)) { SetGuestLastError(ERROR_INVALID_HANDLE); return FALSE; }
    if (!count) return BridgeWriteConsoleW(h, nullptr, 0, written, nullptr);
    std::vector<char> bytes(static_cast<size_t>(count));
    if (!CopyConsoleBytes(bytes.data(), buffer, count)) { SetGuestLastError(ERROR_NOACCESS); return FALSE; }
    const UINT cp = kernel->Console().CodePage(true);
    const int size = BridgeMultiByteToWideChar(cp, 0, bytes.data(), static_cast<int>(count), nullptr, 0);
    if (size <= 0) return FALSE;
    std::vector<wchar_t> text(static_cast<size_t>(size));
    if (BridgeMultiByteToWideChar(cp, 0, bytes.data(), static_cast<int>(count), text.data(), size) != size) return FALSE;
    DWORD done = 0;
    if (!BridgeWriteConsoleW(h, text.data(), static_cast<DWORD>(size), &done, nullptr)) return FALSE;
    if (written) *written = count; return TRUE;
}
BOOL WINAPI BridgeSetConsoleMode(HANDLE h, DWORD mode)
{
    auto kernel = CurrentGuestKernelContext(); DWORD error = ERROR_INVALID_HANDLE;
    const bool result = kernel && kernel->Console().Mode(h, &mode, true, &error);
    SetGuestLastError(error); return result ? TRUE : FALSE;
}
UINT WINAPI BridgeGetConsoleCP()
{ auto kernel = CurrentGuestKernelContext(); return kernel ? kernel->Console().CodePage(false) : 0; }
UINT WINAPI BridgeGetConsoleOutputCP()
{ auto kernel = CurrentGuestKernelContext(); return kernel ? kernel->Console().CodePage(true) : 0; }
BOOL SetConsoleCodePage(UINT cp, bool output)
{
    if (!cp || cp == CP_OEMCP || cp == CP_MACCP || cp == CP_THREAD_ACP)
    { SetGuestLastError(ERROR_INVALID_PARAMETER); return FALSE; }
    CPINFO information{};
    if (!BridgeGetCPInfo(cp, &information)) return FALSE;
    auto kernel = CurrentGuestKernelContext();
    const bool result = kernel && kernel->Console().SetCodePage(output, cp);
    SetGuestLastError(result ? ERROR_SUCCESS : ERROR_INVALID_HANDLE); return result ? TRUE : FALSE;
}
BOOL WINAPI BridgeSetConsoleCP(UINT cp) { return SetConsoleCodePage(cp, false); }
BOOL WINAPI BridgeSetConsoleOutputCP(UINT cp) { return SetConsoleCodePage(cp, true); }

BOOL WINAPI BridgeGetConsoleScreenBufferInfo(HANDLE handle, ConsoleBufferInfo* output) {
    if(!output){SetGuestLastError(ERROR_INVALID_PARAMETER);return FALSE;}
    auto kernel=CurrentGuestKernelContext();ConsoleBufferInfo info{};DWORD error=ERROR_INVALID_HANDLE;
    bool ok=kernel&&kernel->Console().ScreenInfo(handle,&info,&error);
    if(ok&&!CopyConsoleBytes(output,&info,sizeof(info))){ok=false;error=ERROR_NOACCESS;}
    SetGuestLastError(error);return ok;
}
BOOL WINAPI BridgeSetConsoleCursorPosition(HANDLE h,ConsoleCoord point) {
    auto kernel=CurrentGuestKernelContext();DWORD error=ERROR_INVALID_HANDLE;
    bool ok=kernel&&kernel->Console().Cursor(h,point,&error);SetGuestLastError(error);return ok;
}
BOOL WINAPI BridgeSetConsoleTextAttribute(HANDLE h,WORD attributes) {
    auto kernel=CurrentGuestKernelContext();DWORD error=ERROR_INVALID_HANDLE;
    bool ok=kernel&&kernel->Console().Attributes(h,attributes,&error);SetGuestLastError(error);return ok;
}
BOOL ConsoleFill(HANDLE h,WORD value,bool attribute,DWORD count,ConsoleCoord point,LPDWORD written) {
    if(!written){SetGuestLastError(ERROR_INVALID_PARAMETER);return FALSE;}
    auto kernel=CurrentGuestKernelContext();DWORD error=ERROR_INVALID_HANDLE,done=0;
    bool ok=kernel&&kernel->Console().Fill(h,value,attribute,count,point,&done,&error);
    if(!CopyConsoleBytes(written,&done,sizeof(done))){ok=false;error=ERROR_NOACCESS;}
    SetGuestLastError(error);return ok;
}
BOOL WINAPI BridgeFillConsoleOutputCharacterW(HANDLE h,wchar_t value,DWORD count,ConsoleCoord point,LPDWORD written){return ConsoleFill(h,value,false,count,point,written);}
BOOL WINAPI BridgeFillConsoleOutputAttribute(HANDLE h,WORD value,DWORD count,ConsoleCoord point,LPDWORD written){return ConsoleFill(h,value,true,count,point,written);}
BOOL WINAPI BridgeScrollConsoleScreenBufferW(HANDLE h,const ConsoleRect* source,const ConsoleRect* clip,ConsoleCoord target,const ConsoleCell* fill) {
    ConsoleRect src{},clp{};ConsoleCell cell{};
    if(!source||!fill||!CopyConsoleBytes(&src,source,sizeof(src))||!CopyConsoleBytes(&cell,fill,sizeof(cell))||(clip&&!CopyConsoleBytes(&clp,clip,sizeof(clp)))){SetGuestLastError(ERROR_INVALID_PARAMETER);return FALSE;}
    auto kernel=CurrentGuestKernelContext();DWORD error=ERROR_INVALID_HANDLE;
    bool ok=kernel&&kernel->Console().Scroll(h,src,clip?&clp:nullptr,target,cell,&error);SetGuestLastError(error);return ok;
}
BOOL WINAPI BridgeFlushConsoleInputBuffer(HANDLE h) {
    auto kernel=CurrentGuestKernelContext();DWORD error=ERROR_INVALID_HANDLE;
    bool ok=kernel&&kernel->Console().FlushInput(h,&error);SetGuestLastError(error);return ok;
}
BOOL WINAPI BridgeSetConsoleTitleW(LPCWSTR text) {
    if(!text){SetGuestLastError(ERROR_INVALID_PARAMETER);return FALSE;}
    auto kernel=CurrentGuestKernelContext();bool ok=kernel&&kernel->Console().SetTitle(text);SetGuestLastError(ok?0:ERROR_INVALID_HANDLE);return ok;
}
DWORD WINAPI BridgeGetConsoleTitleW(LPWSTR output,DWORD count) {
    auto kernel=CurrentGuestKernelContext();if(!kernel||(!output&&count)){SetGuestLastError(ERROR_INVALID_PARAMETER);return 0;}
    auto title=kernel->Console().Title();DWORD copied=count?(std::min)(count-1,static_cast<DWORD>(title.size())):0;
    if(count){title.resize(copied);if(!CopyConsoleBytes(output,title.c_str(),(copied+1)*sizeof(wchar_t))){SetGuestLastError(ERROR_NOACCESS);return 0;}}
    SetGuestLastError(0);return copied;
}
HWND WINAPI BridgeGetConsoleWindow(){return nullptr;} // UWP-hosted console has no native HWND.
BOOL WINAPI BridgeSetConsoleCtrlHandler(BOOL(WINAPI* callback)(DWORD),BOOL add)
{
    auto kernel=CurrentGuestKernelContext();
    try { bool ok=kernel&&kernel->Console().SetControlHandler(callback,add!=FALSE); SetGuestLastError(ok?0:ERROR_INVALID_PARAMETER); return ok; }
    catch (const std::bad_alloc&) { SetGuestLastError(ERROR_NOT_ENOUGH_MEMORY); return FALSE; }
}
} }
