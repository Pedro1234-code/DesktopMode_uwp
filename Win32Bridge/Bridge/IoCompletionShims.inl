namespace Win32Bridge
{
namespace Bridge
{
namespace
{
    ULONG WINAPI GuestRtlNtStatusToDosError(LONG status);

    GuestKernelContext::WaitDeadline IoCompletionDeadline(DWORD milliseconds)
    {
        using Clock = std::chrono::steady_clock;
        return milliseconds == INFINITE ? (Clock::time_point::max)() :
            Clock::now() + std::chrono::milliseconds(milliseconds);
    }

    DWORD DequeueGuestIo(HANDLE handle, GuestIoPacket* packets, ULONG capacity,
        ULONG& removed, GuestKernelContext::WaitDeadline deadline, bool alertable, bool nativeErrors = false)
    {
        removed = 0;
        const auto kernel = CurrentGuestKernelContext();
        if (!kernel) return ERROR_INVALID_HANDLE;
        DWORD error = ERROR_SUCCESS;
        const auto port = kernel->LookupIoPort(handle, &error, GuestKernelContext::IoCompletionModify, nativeErrors);
        if (!port) return error;
        const DWORD thread = alertable ? BridgeGetCurrentThreadId() : 0;
        for (;;)
        {
            if (kernel->IsStopping()) return ERROR_OPERATION_ABORTED;
            bool hasApc = false;
            if (alertable)
            {
                std::lock_guard<std::mutex> guard(g_guestApcLock);
                hasApc = HasGuestApcLocked(thread);
            }
            // Bounded native waits observe guest close, APCs and shutdown
            // without exposing the private host completion-port handle.
            constexpr std::chrono::milliseconds InterruptCheck(32);
            const auto now = std::chrono::steady_clock::now();
            DWORD slice = 0;
            if (!hasApc && deadline > now)
            {
                const auto remaining = deadline - now;
                auto rounded = InterruptCheck;
                if (remaining < InterruptCheck)
                {
                    rounded = std::chrono::duration_cast<std::chrono::milliseconds>(remaining);
                    if (rounded < remaining) ++rounded;
                }
                slice = static_cast<DWORD>(rounded.count());
            }
            error = port->Dequeue(packets, capacity, removed, slice);
            if (error != WAIT_TIMEOUT) return error;
            if (hasApc) return WAIT_IO_COMPLETION;
            if (std::chrono::steady_clock::now() >= deadline) return WAIT_TIMEOUT;
        }
    }

    ULONG PublishGuestIoPacket(const GuestIoPacket& packet)
    {
        if (packet.publishStatus)
        {
            const bool published = WriteGuestIoStatus(packet.statusBlock, packet.status, packet.information);
            packet.fileSignal.Complete();
            if (!published) return static_cast<ULONG>(GuestNtStatusForIoError(ERROR_NOACCESS));
        }
        return packet.status;
    }
}

HANDLE WINAPI BridgeCreateIoCompletionPort(HANDLE file, HANDLE existing, ULONG_PTR key, DWORD concurrency)
{
    const auto kernel = CurrentKernelOrFail();
    if (!kernel) return nullptr;
    if (file == INVALID_HANDLE_VALUE && existing)
    {
        SetGuestLastError(ERROR_INVALID_PARAMETER);
        return nullptr;
    }
    DWORD error = ERROR_SUCCESS;
    const HANDLE handle = existing ? existing : kernel->CreateIoPort(concurrency, &error);
    if (!handle)
    {
        SetGuestLastError(error);
        return nullptr;
    }
    if (file == INVALID_HANDLE_VALUE) return handle;

    const auto port = kernel->LookupIoPort(handle, &error);
    const auto storage = CurrentGuestStorageContext();
    if (port && storage && storage->BindIoCompletionPort(file, port, key, &error)) return handle;
    if (!existing) kernel->CloseHandle(handle, nullptr);
    SetGuestLastError(error ? error : ERROR_INVALID_HANDLE);
    return nullptr;
}

BOOL WINAPI BridgePostQueuedCompletionStatus(HANDLE handle, DWORD bytes, ULONG_PTR key, LPOVERLAPPED overlapped)
{
    const auto kernel = CurrentKernelOrFail();
    if (!kernel) return FALSE;
    DWORD error = ERROR_SUCCESS;
    const auto port = kernel->LookupIoPort(handle, &error);
    if (port) error = port->Post(GuestIoPacket{bytes, key, overlapped});
    if (!error) return TRUE;
    SetGuestLastError(error);
    return FALSE;
}

BOOL WINAPI BridgeGetQueuedCompletionStatus(HANDLE port, LPDWORD bytes, PULONG_PTR key,
    LPOVERLAPPED* overlapped, DWORD milliseconds)
{
    ExitGuestThreadOnShutdown();
    const LPOVERLAPPED empty = nullptr;
    if (!bytes || !key || !overlapped)
    {
        SetGuestLastError(ERROR_INVALID_PARAMETER);
        return FALSE;
    }
    if (!CopyConsoleBytes(overlapped, &empty, sizeof(empty)))
    {
        SetGuestLastError(ERROR_NOACCESS);
        return FALSE;
    }
    GuestIoPacket packet{};
    ULONG removed = 0;
    DWORD error = DequeueGuestIo(port, &packet, 1, removed, IoCompletionDeadline(milliseconds), false);
    if (error == ERROR_OPERATION_ABORTED && !removed) ExitGuestThreadOnShutdown();
    if (removed)
    {
        const auto status = static_cast<LONG>(PublishGuestIoPacket(packet));
        error = status < 0 ? GuestRtlNtStatusToDosError(status) : ERROR_SUCCESS;
        const DWORD transferred = static_cast<DWORD>(packet.information);
        if (!CopyConsoleBytes(bytes, &transferred, sizeof(transferred)) ||
            !CopyConsoleBytes(key, &packet.key, sizeof(packet.key)) ||
            !CopyConsoleBytes(overlapped, &packet.overlapped, sizeof(packet.overlapped)))
            error = ERROR_NOACCESS;
    }
    if (!error) return TRUE;
    SetGuestLastError(error);
    return FALSE;
}

BOOL WINAPI BridgeGetQueuedCompletionStatusEx(HANDLE port, LPOVERLAPPED_ENTRY entries, ULONG capacity,
    PULONG removed, DWORD milliseconds, BOOL alertable)
{
    ExitGuestThreadOnShutdown();
    const ULONG empty = 0;
    if (!entries || !removed || !capacity)
    {
        SetGuestLastError(ERROR_INVALID_PARAMETER);
        return FALSE;
    }
    if (!CopyConsoleBytes(removed, &empty, sizeof(empty)))
    {
        SetGuestLastError(ERROR_NOACCESS);
        return FALSE;
    }
    constexpr ULONG Batch = GuestIoCompletionPort::MaximumBatch;
    GuestIoPacket packets[Batch]{};
    ULONG count = 0;
    DWORD error = DequeueGuestIo(port, packets, (std::min)(capacity, Batch), count,
        IoCompletionDeadline(milliseconds), alertable != FALSE);
    if (error == ERROR_OPERATION_ABORTED) ExitGuestThreadOnShutdown();
    if (error == WAIT_IO_COMPLETION) DispatchGuestApcs(BridgeGetCurrentThreadId());
    if (!error)
    {
        OVERLAPPED_ENTRY written[Batch]{};
        for (ULONG index = 0; index < count; ++index)
        {
            const auto& packet = packets[index];
            written[index] = OVERLAPPED_ENTRY{packet.key, packet.overlapped,
                PublishGuestIoPacket(packet), static_cast<DWORD>(packet.information)};
        }
        if (CopyConsoleBytes(entries, written, count * sizeof(*written)) &&
            CopyConsoleBytes(removed, &count, sizeof(count))) return TRUE;
        error = ERROR_NOACCESS;
    }
    SetGuestLastError(error);
    return FALSE;
}

BOOL WINAPI BridgeSetFileCompletionNotificationModes(HANDLE file, UCHAR modes)
{
    constexpr UCHAR Available = FILE_SKIP_COMPLETION_PORT_ON_SUCCESS | FILE_SKIP_SET_EVENT_ON_HANDLE;
    if (modes & ~Available)
    {
        SetGuestLastError(ERROR_INVALID_PARAMETER);
        return FALSE;
    }
    const auto storage = CurrentGuestStorageContext();
    DWORD error = ERROR_INVALID_HANDLE;
    if (storage && storage->SetIoCompletionModes(file, modes, &error)) return TRUE;
    SetGuestLastError(error);
    return FALSE;
}

BOOL WINAPI BridgeCancelIoEx(HANDLE file, LPOVERLAPPED overlapped)
{
    const auto storage = CurrentGuestStorageContext();
    const auto kernel = CurrentGuestKernelContext();
    const auto description = storage ? storage->RetainFileDescription(file) : nullptr;
    if (!kernel || !description)
    {
        SetGuestLastError(ERROR_INVALID_HANDLE);
        return FALSE;
    }
    if (CancelGuestFileRequests(description.get(), kernel, 0, overlapped)) return TRUE;
    SetGuestLastError(ERROR_NOT_FOUND);
    return FALSE;
}
}
}
