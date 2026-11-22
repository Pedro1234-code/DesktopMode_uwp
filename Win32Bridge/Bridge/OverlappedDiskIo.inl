namespace
{
    struct GuestDiskCompletionTarget
    {
        void* statusBlock = nullptr;
        void* context = nullptr;
        HANDLE event = nullptr;
        LPOVERLAPPED_COMPLETION_ROUTINE callback = nullptr;
        GuestNativeIoApc nativeCallback = nullptr;
        bool markPending = false;
    };

    struct GuestDiskRangeRequest
    {
        ULONGLONG length = 0;
        bool exclusive = false;
        bool pendingLock = false;
    };

    struct GuestDiskRequest
    {
        GuestStorageContext::DiskFile file;
        GuestIoCompletionBinding binding;
        GuestKernelContext::IoEvent event;
        GuestKernelContext::FileSignal fileSignal;
        std::weak_ptr<GuestKernelContext> owner;
        const GuestKernelContext* ownerIdentity = nullptr;
        DWORD thread = 0;
        GuestDiskCompletionTarget target;
        void* callerBuffer = nullptr;
        ULONGLONG offset = 0;
        bool writing = false;
        ULONG lockKey = 0;
        GuestDiskRangeRequest range;
        std::vector<BYTE> buffer;
        Concurrency::cancellation_token_source cancellation;
    };

    using DiskRequestPtr = std::shared_ptr<GuestDiskRequest>;
    std::mutex g_diskIoLock;
    std::list<DiskRequestPtr> g_diskRequests;

    auto FindGuestDiskRequest(const GuestKernelContext* owner, const void* statusBlock)
    {
        return std::find_if(g_diskRequests.begin(), g_diskRequests.end(),
            [owner, statusBlock](const DiskRequestPtr& request)
            { return request->ownerIdentity == owner && request->target.statusBlock == statusBlock; });
    }

    bool CancelGuestDiskRequests(const void* description, const GuestKernelContext* owner,
        DWORD threadId, LPOVERLAPPED overlapped)
    {
        std::lock_guard<std::mutex> lock(g_diskIoLock);
        bool matched = false;
        for (const auto& request : g_diskRequests)
        {
            if ((description && request->file.Identity() != description) ||
                (owner && request->ownerIdentity != owner) ||
                (threadId && request->thread != threadId) ||
                (overlapped && request->target.statusBlock != overlapped)) continue;
            matched = true;
            try { request->cancellation.cancel(); }
            catch (...) { /* The provider may already have completed the operation. */ }
        }
        return matched;
    }

    void CompleteGuestDiskRequest(const DiskRequestPtr& request, DWORD error, DWORD bytes)
    {
        const auto owner = request->owner.lock();
        {
            // Shutdown takes this lock before it lets the guest address space
            // go away. After shutdown starts, only the staging buffer is used.
            std::lock_guard<std::mutex> lock(g_diskIoLock);
            if (owner && !owner->IsStopping())
            {
                if (!request->writing && !error && bytes &&
                    !CopyConsoleBytes(request->callerBuffer, request->buffer.data(), bytes))
                {
                    error = ERROR_NOACCESS;
                    bytes = 0;
                }
                const auto& target = request->target;
                CompletePendingGuestFileIo(request->binding, target.statusBlock, target.context, target.event,
                    request->event, request->fileSignal, error, bytes);
                if (target.callback || target.nativeCallback)
                    QueueGuestIoCompletion(request->thread, {target.callback, error, bytes,
                        static_cast<LPOVERLAPPED>(target.statusBlock), target.nativeCallback, target.context, target.statusBlock});
            }
            const auto found = FindGuestDiskRequest(request->ownerIdentity, request->target.statusBlock);
            if (found != g_diskRequests.end()) g_diskRequests.erase(found);
        }
    }

    void CALLBACK RunGuestDiskRequest(PTP_CALLBACK_INSTANCE instance, void* parameter)
    {
        ::CallbackMayRunLong(instance);
        std::unique_ptr<DiskRequestPtr> job(static_cast<DiskRequestPtr*>(parameter));
        const auto& request = *job;
        const HRESULT apartment = ::RoInitialize(RO_INIT_MULTITHREADED);
        DWORD error = ERROR_GEN_FAILURE;
        DWORD bytes = 0;
        if (SUCCEEDED(apartment) || apartment == RPC_E_CHANGED_MODE)
        {
            try
            {
                const auto cancellation = request->cancellation.get_token();
                if (request->range.pendingLock)
                    error = GuestStorageContext::LockDiskRange(request->file, request->offset, request->range.length,
                        request->range.exclusive, false, request->lockKey, cancellation);
                else
                    error = GuestStorageContext::TransferDiskFile(request->file, request->writing, request->offset,
                        request->buffer.data(), static_cast<DWORD>(request->buffer.size()), bytes,
                        cancellation, false, request->lockKey);
            }
            catch (const std::bad_alloc&) { error = ERROR_NOT_ENOUGH_MEMORY; }
            catch (...) { error = ERROR_GEN_FAILURE; }
        }
        CompleteGuestDiskRequest(request, error, bytes);
        job.reset();
        if (SUCCEEDED(apartment)) ::RoUninitialize();
    }

    DWORD QueueGuestDiskRequest(GuestStorageContext& storage, HANDLE handle,
        const GuestStorageContext::DiskFile& file, bool writing, void* buffer, DWORD length,
        ULONGLONG offset, const GuestDiskCompletionTarget& target, ULONG lockKey = 0,
        const GuestDiskRangeRequest& range = {})
    {
        const auto kernel = CurrentGuestKernelContext();
        if (!kernel) return ERROR_INVALID_HANDLE;
        if (!target.statusBlock || (length && !buffer)) return ERROR_INVALID_PARAMETER;
        if (length > GuestStorageContext::MaximumTransferSize()) return ERROR_NOT_ENOUGH_MEMORY;
        if (!range.pendingLock)
        {
            const DWORD access = file.ValidateAccess(writing);
            if (access) return access;
        }
        try
        {
            auto request = std::make_shared<GuestDiskRequest>();
            request->file = file;
            request->binding = storage.IoCompletionBinding(handle);
            request->fileSignal = storage.LookupFileSignal(handle);
            if ((target.callback || target.nativeCallback) && request->binding.port) return ERROR_INVALID_PARAMETER;
            request->owner = kernel->shared_from_this();
            request->ownerIdentity = kernel;
            request->thread = BridgeGetCurrentThreadId();
            request->target = target;
            request->callerBuffer = buffer;
            request->offset = offset;
            request->writing = writing;
            request->lockKey = lockKey;
            request->range = range;
            request->buffer.resize(length);
            if (writing && length && !CopyConsoleBytes(request->buffer.data(), buffer, length)) return ERROR_NOACCESS;

            DWORD eventError = ERROR_SUCCESS;
            if (target.event && !kernel->CaptureIoEvent(target.event, request->event, &eventError)) return eventError;
            std::unique_ptr<DiskRequestPtr> job(new DiskRequestPtr(request));
            std::lock_guard<std::mutex> lock(g_diskIoLock);
            if (kernel->IsStopping()) return ERROR_OPERATION_ABORTED;
            if (storage.RetainFileDescription(handle).get() != file.Identity()) return ERROR_INVALID_HANDLE;
            if (FindGuestDiskRequest(kernel, target.statusBlock) != g_diskRequests.end()) return ERROR_INVALID_PARAMETER;

            const auto registered = g_diskRequests.insert(g_diskRequests.end(), request);
            if (target.markPending && !WriteGuestIoStatus(target.statusBlock, GuestIoPending, 0))
            {
                g_diskRequests.erase(registered);
                return ERROR_NOACCESS;
            }
            if (!::TrySubmitThreadpoolCallback(&RunGuestDiskRequest, job.get(), nullptr))
            {
                const DWORD error = ::GetLastError();
                g_diskRequests.erase(registered);
                if (target.markPending) WriteGuestIoStatus(target.statusBlock, GuestNtStatusForIoError(error), 0);
                return error;
            }
            job.release();
            request->fileSignal.Begin();
            request->event.Set(false);
            return ERROR_IO_PENDING;
        }
        catch (const std::bad_alloc&) { return ERROR_NOT_ENOUGH_MEMORY; }
    }

    DWORD SubmitGuestDiskRequest(GuestStorageContext& storage, HANDLE handle,
        const GuestStorageContext::DiskFile& file, bool writing, void* buffer, DWORD length,
        LPDWORD immediateBytes, LPOVERLAPPED overlapped, LPOVERLAPPED_COMPLETION_ROUTINE callback)
    {
        if (!overlapped) return ERROR_INVALID_PARAMETER;
        OVERLAPPED captured{};
        if (!CopyConsoleBytes(&captured, overlapped, sizeof(captured))) return ERROR_NOACCESS;
        const DWORD zero = 0;
        if (immediateBytes && !CopyConsoleBytes(immediateBytes, &zero, sizeof(zero))) return ERROR_NOACCESS;
        const ULONGLONG offset = (static_cast<ULONGLONG>(captured.OffsetHigh) << 32) | captured.Offset;
        const HANDLE event = callback ? nullptr : GuestOverlappedEvent(captured.hEvent);
        void* const context = (callback || (reinterpret_cast<ULONG_PTR>(captured.hEvent) & 1)) ? nullptr : overlapped;
        const GuestDiskCompletionTarget target{overlapped, context, event, callback, nullptr, true};
        return QueueGuestDiskRequest(storage, handle, file, writing, buffer, length, offset, target);
    }

    DWORD RequestGuestRangeLock(GuestStorageContext& storage, HANDLE handle, ULONGLONG offset, ULONGLONG length,
        bool exclusive, bool failImmediately, ULONG key, const GuestDiskCompletionTarget& target)
    {
        const auto kernel = CurrentGuestKernelContext();
        if (!kernel) return ERROR_INVALID_HANDLE;
        const auto file = storage.CaptureDiskFile(handle);
        const DWORD access = file.ValidateLockAccess();
        if (access) return access;
        if (length && length - 1 > (std::numeric_limits<ULONGLONG>::max)() - offset)
            return ERROR_INVALID_PARAMETER;
        const bool asynchronous = file.IsOverlapped();
        const auto binding = storage.IoCompletionBinding(handle);
        if (target.nativeCallback && binding.port) return ERROR_INVALID_PARAMETER;
        GuestKernelContext::IoEvent event;
        DWORD error = ERROR_SUCCESS;
        if (target.event && !kernel->CaptureIoEvent(target.event, event, &error)) return error;
        if (target.markPending && !WriteGuestIoStatus(target.statusBlock, GuestIoPending, 0)) return ERROR_NOACCESS;
        const auto fileSignal = storage.LookupFileSignal(handle);
        fileSignal.Begin();
        event.Set(false);
        storage.LockFileRange(handle, offset, length, exclusive, asynchronous || failImmediately, &error, key);
        if (error == ERROR_LOCK_VIOLATION && asynchronous && !failImmediately)
        {
            error = QueueGuestDiskRequest(storage, handle, file, false, nullptr, 0, offset, target, key,
                GuestDiskRangeRequest{length, exclusive, true});
            if (error == ERROR_IO_PENDING) return error;
        }
        const ULONG_PTR status = error == ERROR_LOCK_VIOLATION ? 0xc0000055u : GuestNtStatusForIoError(error);
        if (!WriteGuestIoStatus(target.statusBlock, status, 0)) error = ERROR_NOACCESS;
        event.Set(true);
        if (!asynchronous || !target.event) fileSignal.Complete();
        if (!error)
        {
            PostGuestFileCompletion(binding, target.statusBlock, target.context, target.event, 0, 0, false);
            if (asynchronous && target.nativeCallback)
                QueueGuestIoCompletion(BridgeGetCurrentThreadId(), {nullptr, 0, 0, nullptr,
                    target.nativeCallback, target.context, target.statusBlock});
        }
        return error;
    }
}

namespace Win32Bridge
{
namespace Bridge
{
namespace
{
    BOOL StartGuestFileEx(HANDLE file, void* buffer, DWORD bytes, LPOVERLAPPED overlapped,
        LPOVERLAPPED_COMPLETION_ROUTINE callback, bool writing)
    {
        if (!callback)
        {
            SetGuestLastError(ERROR_INVALID_PARAMETER);
            return FALSE;
        }
        const auto storage = CurrentStorageOrFail();
        if (!storage) return FALSE;
        const auto captured = storage->CaptureDiskFile(file);
        if (!captured.IsOverlapped())
        {
            SetGuestLastError(captured.Identity() ? ERROR_INVALID_PARAMETER : ERROR_INVALID_HANDLE);
            return FALSE;
        }
        const DWORD result = SubmitGuestDiskRequest(*storage, file, captured, writing, buffer, bytes,
            nullptr, overlapped, callback);
        if (result == ERROR_IO_PENDING) return TRUE;
        SetGuestLastError(result);
        return FALSE;
    }
}

BOOL WINAPI BridgeReadFileEx(HANDLE file, void* buffer, DWORD bytes, LPOVERLAPPED overlapped,
    LPOVERLAPPED_COMPLETION_ROUTINE callback)
{
    return StartGuestFileEx(file, buffer, bytes, overlapped, callback, false);
}

BOOL WINAPI BridgeWriteFileEx(HANDLE file, const void* buffer, DWORD bytes, LPOVERLAPPED overlapped,
    LPOVERLAPPED_COMPLETION_ROUTINE callback)
{
    return StartGuestFileEx(file, const_cast<void*>(buffer), bytes, overlapped, callback, true);
}
}
}
