namespace Win32Bridge { namespace Bridge {
namespace {
    bool PumpPipeWait()
    {
        if (auto windows = CurrentGuestWindowManager())
        {
            GuestAbi::Message message{};
            unsigned dispatched = 0;
            while (dispatched++ < 64 && windows->PeekGuestMessage(&message, nullptr, 0, 0, 1))
            {
                if (message.message == WM_QUIT)
                { windows->PostGuestQuitMessage(static_cast<int>(message.wParam)); return false; }
                windows->TranslateGuestMessage(&message); windows->DispatchGuestMessage(&message, nullptr);
            }
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(8)); return true;
    }
    BOOL TransferPipe(HANDLE handle, void* buffer, DWORD count, LPDWORD transferred, bool write)
    {
        DWORD total = 0, error = ERROR_INVALID_HANDLE;
        if (transferred && !CopyConsoleBytes(transferred, &total, sizeof(total)))
        { SetGuestLastError(ERROR_NOACCESS); return FALSE; }
        if (!buffer && count) { SetGuestLastError(ERROR_INVALID_PARAMETER); return FALSE; }
        auto kernel = CurrentGuestKernelContext();
        if (!kernel) { SetGuestLastError(error); return FALSE; }
        try
        {
            std::vector<BYTE> scratch((std::min)(count, static_cast<DWORD>(65536)));
            DWORD staged = 0, offset = 0;
            for (;;)
            {
                if (kernel->IsStopping()) { error=ERROR_OPERATION_ABORTED; break; }
                if (write && offset == staged)
                {
                    staged = (std::min)(count - total, static_cast<DWORD>(scratch.size())); offset = 0;
                    if (staged && !CopyConsoleBytes(scratch.data(), static_cast<const BYTE*>(buffer) + total, staged))
                    { error = ERROR_NOACCESS; break; }
                }
                DWORD done = 0;
                const bool result = write ? kernel->Pipes().Write(handle, count ? scratch.data() + offset : nullptr, staged - offset, &done, &error)
                    : kernel->Pipes().Read(handle, scratch.data(), static_cast<DWORD>(scratch.size()), &done, &error);
                if (write) { total += done; offset += done; }
                else if (result)
                {
                    if (!CopyConsoleBytes(buffer, scratch.data(), done)) { error = ERROR_NOACCESS; break; }
                    total = done;
                }
                if (result && (!write || total == count)) break;
                if (!result && error != ERROR_IO_PENDING) break;
                if (!result && !PumpPipeWait()) { error = ERROR_OPERATION_ABORTED; break; }
            }
        }
        catch (const std::bad_alloc&) { error = ERROR_NOT_ENOUGH_MEMORY; }
        if (transferred && !CopyConsoleBytes(transferred, &total, sizeof(total))) error = ERROR_NOACCESS;
        if (error == ERROR_OPERATION_ABORTED) ExitGuestThreadOnShutdown();
        SetGuestLastError(error); return error == ERROR_SUCCESS ? TRUE : FALSE;
    }
}
BOOL WINAPI BridgeCreatePipe(PHANDLE read, PHANDLE write, LPSECURITY_ATTRIBUTES security, DWORD size)
{
    if (!read || !write) { SetGuestLastError(ERROR_INVALID_PARAMETER); return FALSE; }
    SECURITY_ATTRIBUTES attributes{};
    if (security && (!CopyConsoleBytes(&attributes, security, sizeof(attributes)) || attributes.nLength != sizeof(attributes)))
    { SetGuestLastError(ERROR_INVALID_PARAMETER); return FALSE; }
    if (security && attributes.lpSecurityDescriptor)
    { SetGuestLastError(ERROR_NOT_SUPPORTED); return FALSE; }
    auto kernel = CurrentGuestKernelContext(); DWORD error = ERROR_INVALID_FUNCTION;
    HANDLE input = nullptr, output = nullptr;
    if (!kernel || !kernel->Pipes().Create(security && attributes.bInheritHandle, size, &input, &output, &error))
    { SetGuestLastError(error); return FALSE; }
    if (!CopyConsoleBytes(read, &input, sizeof(input)) || !CopyConsoleBytes(write, &output, sizeof(output)))
    {
        kernel->Pipes().Close(input, &error); kernel->Pipes().Close(output, &error);
        SetGuestLastError(ERROR_NOACCESS); return FALSE;
    }
    SetGuestLastError(ERROR_SUCCESS); return TRUE;
}
BOOL WINAPI BridgePeekNamedPipe(HANDLE handle, LPVOID buffer, DWORD capacity,
    LPDWORD copied, LPDWORD available, LPDWORD left)
{
    if (!buffer) capacity = 0;
    auto kernel = CurrentGuestKernelContext(); DWORD error = ERROR_INVALID_HANDLE;
    DWORD count = 0, total = 0, remaining = 0;
    try
    {
        std::vector<BYTE> bytes(capacity);
        if (!kernel || !kernel->Pipes().Peek(handle, bytes.data(), capacity, &count, &total, &error))
        { SetGuestLastError(error); return FALSE; }
        if (!CopyConsoleBytes(buffer, bytes.data(), count) ||
            (copied && !CopyConsoleBytes(copied, &count, sizeof(count))) ||
            (available && !CopyConsoleBytes(available, &total, sizeof(total))) ||
            (left && !CopyConsoleBytes(left, &remaining, sizeof(remaining))))
        { SetGuestLastError(ERROR_NOACCESS); return FALSE; }
    }
    catch (const std::bad_alloc&) { SetGuestLastError(ERROR_NOT_ENOUGH_MEMORY); return FALSE; }
    SetGuestLastError(ERROR_SUCCESS); return TRUE;
}
BOOL WINAPI BridgeGetHandleInformation(HANDLE handle, LPDWORD flags)
{
    if (!flags) { SetGuestLastError(ERROR_INVALID_PARAMETER); return FALSE; }
    auto kernel = CurrentGuestKernelContext(); DWORD value = 0, error = ERROR_INVALID_HANDLE;
    bool result = false;
    if (kernel && kernel->Pipes().Owns(handle)) result = kernel->Pipes().Flags(handle, &value, 0, false, &error);
    else if (kernel && kernel->Console().Owns(handle)) result = kernel->Console().Flags(handle, &value, 0, false, &error);
    else if (kernel && kernel->Owns(handle)) result = kernel->HandleFlags(handle, &value, 0, false, &error);
    else if (FileMappingHandleFlags(handle, value, 0, false, error)) result = error == ERROR_SUCCESS;
    else if (auto storage = CurrentGuestStorageContext()) result = storage->FileHandleFlags(handle, &value, 0, false, &error);
    if (result && !CopyConsoleBytes(flags, &value, sizeof(value))) error = ERROR_NOACCESS;
    SetGuestLastError(error); return result && error == ERROR_SUCCESS ? TRUE : FALSE;
}
BOOL WINAPI BridgeSetHandleInformation(HANDLE handle, DWORD mask, DWORD flags)
{
    auto kernel = CurrentGuestKernelContext(); DWORD error = ERROR_INVALID_HANDLE;
    bool result = false;
    if (kernel && kernel->Pipes().Owns(handle)) result = kernel->Pipes().Flags(handle, &flags, mask, true, &error);
    else if (kernel && kernel->Console().Owns(handle)) result = kernel->Console().Flags(handle, &flags, mask, true, &error);
    else if (kernel && kernel->Owns(handle)) result = kernel->HandleFlags(handle, &flags, mask, true, &error);
    else if (FileMappingHandleFlags(handle, flags, mask, true, error)) result = error == ERROR_SUCCESS;
    else if (auto storage = CurrentGuestStorageContext()) result = storage->FileHandleFlags(handle, &flags, mask, true, &error);
    SetGuestLastError(error); return result ? TRUE : FALSE;
}
BOOL WINAPI BridgeDuplicateHandle(HANDLE sourceProcess, HANDLE source, HANDLE targetProcess,
    LPHANDLE target, DWORD access, BOOL inherit, DWORD options)
{
    const bool closeSource = (options & 1u) != 0;
    DWORD error = ERROR_SUCCESS; HANDLE result = nullptr;
    auto kernel = CurrentGuestKernelContext();
    if (sourceProcess != BridgeGetCurrentProcess() || targetProcess != BridgeGetCurrentProcess() || !kernel)
        error = ERROR_NOT_SUPPORTED;
    else if (!target || (options & ~3u)) error = ERROR_INVALID_PARAMETER;
    else
    {
        const bool sameAccess = (options & 2u) != 0;
        if (!DuplicateFileMappingHandle(source, access, inherit, sameAccess, result, error))
        {
            if (!sameAccess)
            {
                if (kernel->Owns(source)) kernel->Duplicate(source, inherit != FALSE, &result, &error, access, false);
                else error = ERROR_NOT_SUPPORTED;
            }
            else if (kernel->Pipes().Owns(source)) kernel->Pipes().Duplicate(source, inherit != FALSE, &result, &error);
            else if (kernel->Console().Owns(source)) kernel->Console().Duplicate(source, inherit != FALSE, &result, &error);
            else if (kernel->Owns(source)) kernel->Duplicate(source, inherit != FALSE, &result, &error);
            else if (auto storage = CurrentGuestStorageContext()) storage->DuplicateFile(source, inherit != FALSE, &result, &error);
            else error = ERROR_INVALID_HANDLE;
        }
    }
    if (closeSource && sourceProcess == BridgeGetCurrentProcess()) BridgeCloseHandle(source);
    if (error == ERROR_SUCCESS && !CopyConsoleBytes(target, &result, sizeof(result)))
    { BridgeCloseHandle(result); error = ERROR_NOACCESS; }
    SetGuestLastError(error); return error == ERROR_SUCCESS ? TRUE : FALSE;
}
} }
