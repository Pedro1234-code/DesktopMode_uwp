#include "pch.h"
#include "Bridge\\GuestKernel.h"
#include "Bridge/GuestIoCompletionPort.h"
#include "Bridge/GuestStorage.h"
#include "Bridge/Kernel32Shims.h"

#include <algorithm>
#include <chrono>
#include <cwctype>
#include <limits>
#include <new>
#include <string>
#include <threadpoolapiset.h>
#include <vector>

using namespace Win32Bridge::Bridge;

namespace
{
    thread_local GuestKernelContext* g_currentGuestKernel = nullptr;
    thread_local GuestKernelScope* g_currentGuestKernelScope = nullptr;

    void SetWin32Error(DWORD* destination, DWORD value)
    {
        if (destination)
        {
            *destination = value;
        }
    }

    bool SynchronizationObjectName(LPCWSTR name, bool opening, std::wstring& key, DWORD* error)
    {
        if (!name || !*name)
        {
            if (opening) SetWin32Error(error, name ? ERROR_INVALID_HANDLE : ERROR_INVALID_PARAMETER);
            return !opening;
        }
        constexpr size_t MaxName = 32766;
        const size_t length = wcsnlen_s(name, MaxName + 1);
        if (length > MaxName)
        {
            SetWin32Error(error, ERROR_FILENAME_EXCED_RANGE);
            return false;
        }
        key.assign(name, length);
        const bool local = key.compare(0, 6, L"Local\\") == 0;
        const bool global = key.compare(0, 7, L"Global\\") == 0;
        const size_t prefix = local ? 6 : global ? 7 : 0;
        if (key.size() == prefix || key.find(L'\\', prefix) != std::wstring::npos)
        {
            SetWin32Error(error, key.size() == prefix ? ERROR_INVALID_NAME : ERROR_PATH_NOT_FOUND);
            return false;
        }
        if (local) key.erase(0, prefix);
        return true;
    }

    HRESULT GraphicsHandleName(LPCWSTR name, std::wstring& key)
    {
        if (!name || !*name || wcsnlen_s(name, MAX_PATH + 1) > MAX_PATH) return E_INVALIDARG;
        key = name;
        const bool global = key.compare(0, 7, L"Global\\") == 0;
        const bool local = key.compare(0, 6, L"Local\\") == 0;
        const size_t prefix = global ? 7 : local ? 6 : 0;
        if (key.size() == prefix || key.find(L'\\', prefix) != std::wstring::npos) return E_INVALIDARG;
        if (local) key.erase(0, prefix);
        return S_OK;
    }

    std::chrono::steady_clock::time_point DispatcherDeadline(DWORD milliseconds)
    {
        if (milliseconds == INFINITE) return (std::chrono::steady_clock::time_point::max)();
        return std::chrono::steady_clock::now() + std::chrono::milliseconds(milliseconds);
    }

    ULONGLONG CompletionKey(DWORD id, bool process)
    {
        return (static_cast<ULONGLONG>(id) << 1) | (process ? 1u : 0u);
    }

    ULONGLONG FileTimeValue(FILETIME time)
    {
        return (static_cast<ULONGLONG>(time.dwHighDateTime) << 32) | time.dwLowDateTime;
    }
}

std::mutex GuestKernelContext::s_namedObjectsLock;
std::unordered_map<std::wstring, std::weak_ptr<GuestKernelContext::ObjectRecord>>
    GuestKernelContext::s_namedObjects;
std::unordered_map<std::wstring, std::weak_ptr<GuestKernelContext::ObjectRecord>>
    GuestKernelContext::s_namedGraphicsHandles;
std::mutex GuestKernelContext::s_dispatcherLock;
std::condition_variable GuestKernelContext::s_dispatcherChanged;

GuestKernelContext::~GuestKernelContext()
{
    CloseAll();
}

GuestKernelContext::FileSignal GuestKernelContext::CreateFileSignal()
{
    FileSignal signal;
    signal.m_object = std::make_shared<ObjectRecord>(ObjectKind::File);
    signal.m_object->manualReset = true;
    signal.m_object->signaled = true;
    return signal;
}

void GuestKernelContext::FileSignal::Begin() const
{
    if (!m_object) return;
    std::lock_guard<std::mutex> lock(s_dispatcherLock);
    m_object->signaled = false;
}

void GuestKernelContext::FileSignal::Complete() const
{
    if (!m_object) return;
    std::lock_guard<std::mutex> lock(s_dispatcherLock);
    if (m_object->closed || m_object->suppressFileSignal) return;
    m_object->signaled = true;
    s_dispatcherChanged.notify_all();
}

void GuestKernelContext::FileSignal::SuppressCompletion() const
{
    if (!m_object) return;
    std::lock_guard<std::mutex> lock(s_dispatcherLock);
    m_object->suppressFileSignal = true;
    m_object->signaled = false;
}

void GuestKernelContext::FileSignal::Retire() const
{
    if (!m_object) return;
    std::lock_guard<std::mutex> lock(s_dispatcherLock);
    m_object->closed = true;
    m_object->signaled = false;
    s_dispatcherChanged.notify_all();
}

void GuestKernelContext::IoEvent::Set(bool signaled) const
{
    if (!m_object) return;
    std::lock_guard<std::mutex> lock(s_dispatcherLock);
    if (m_object->closed) return;
    m_object->signaled = signaled;
    if (signaled) s_dispatcherChanged.notify_all();
}

bool GuestKernelContext::CaptureIoEvent(HANDLE event, IoEvent& result, DWORD* error)
{
    DWORD access = 0;
    const auto object = Lookup(event, &access);
    if (!object || object->kind != ObjectKind::Event)
    {
        SetWin32Error(error, ERROR_INVALID_HANDLE);
        return false;
    }
    if (!(access & SynchronizationModifyAccess))
    {
        SetWin32Error(error, ERROR_ACCESS_DENIED);
        return false;
    }
    result.m_object = object;
    SetWin32Error(error, ERROR_SUCCESS);
    return true;
}

GuestIoCompletionPort::~GuestIoCompletionPort()
{
    ::CloseHandle(m_queue);
}

void GuestIoCompletionPort::CloseGuestHandles()
{
    std::lock_guard<std::mutex> lock(m_lock);
    m_closed.store(true);
    m_packets.clear();
}

bool GuestIoCompletionPort::IsReady() const
{
    return !IsClosed() && ::WaitForSingleObjectEx(m_queue, 0, FALSE) == WAIT_OBJECT_0;
}

LONG GuestIoCompletionPort::QueryDepth()
{
    std::lock_guard<std::mutex> lock(m_lock);
    return static_cast<LONG>((std::min)(m_packets.size(), static_cast<size_t>((std::numeric_limits<LONG>::max)())));
}

DWORD GuestIoCompletionPort::Post(const GuestIoPacket& packet)
{
    {
        std::lock_guard<std::mutex> lock(m_lock);
        if (IsClosed()) return ERROR_INVALID_HANDLE;
        ULONG_PTR id;
        do { id = ++m_nextPacket; } while (!id || m_packets.count(id));
        try { m_packets.emplace(id, packet); }
        catch (const std::bad_alloc&) { return ERROR_NOT_ENOUGH_MEMORY; }
        if (!::PostQueuedCompletionStatus(m_queue, 0, id, nullptr))
        {
            const DWORD error = ::GetLastError();
            m_packets.erase(id);
            return error;
        }
    }
    GuestKernelContext::NotifyAlertableWaiters();
    return ERROR_SUCCESS;
}

DWORD GuestIoCompletionPort::Dequeue(GuestIoPacket* packets, ULONG capacity, ULONG& removed, DWORD milliseconds)
{
    removed = 0;
    if (IsClosed()) return ERROR_ABANDONED_WAIT_0;
    OVERLAPPED_ENTRY entries[MaximumBatch]{};
    ULONG count = 0;
    const BOOL received = ::GetQueuedCompletionStatusEx(m_queue, entries, capacity, &count, milliseconds, FALSE);
    const DWORD error = received ? ERROR_SUCCESS : ::GetLastError();
    std::lock_guard<std::mutex> lock(m_lock);
    if (IsClosed()) return ERROR_ABANDONED_WAIT_0;
    if (!received) return error;
    for (ULONG index = 0; index < count; ++index)
    {
        const auto found = m_packets.find(entries[index].lpCompletionKey);
        if (found == m_packets.end()) continue;
        packets[removed++] = found->second;
        m_packets.erase(found);
    }
    return removed ? ERROR_SUCCESS : WAIT_TIMEOUT;
}

HANDLE GuestKernelContext::CreateIoPort(DWORD concurrency, DWORD* win32Error, LPCWSTR name,
    bool inherit, DWORD access, const NativeObjectNameOptions* nativeName)
{
    const HANDLE queue = ::CreateIoCompletionPort(INVALID_HANDLE_VALUE, nullptr, 0, concurrency);
    if (!queue)
    {
        SetWin32Error(win32Error, ::GetLastError());
        return nullptr;
    }
    std::shared_ptr<GuestIoCompletionPort> port;
    try
    {
        port = std::make_shared<GuestIoCompletionPort>(queue);
        auto object = std::make_shared<ObjectRecord>(ObjectKind::IoCompletionPort);
        object->manualReset = true;
        object->ioPort = port;
        return CreateSynchronizationObject(object, name, access, inherit, win32Error, nullptr, nativeName);
    }
    catch (const std::bad_alloc&)
    {
        if (!port) ::CloseHandle(queue);
        SetWin32Error(win32Error, ERROR_NOT_ENOUGH_MEMORY);
        return nullptr;
    }
}

HANDLE GuestKernelContext::OpenIoPort(DWORD access, BOOL inherit, LPCWSTR name, DWORD* win32Error,
    const NativeObjectNameOptions& nativeName)
{
    return OpenSynchronizationObject(ObjectKind::IoCompletionPort, access, inherit, name, win32Error, &nativeName);
}

std::shared_ptr<GuestIoCompletionPort> GuestKernelContext::LookupIoPort(HANDLE handle, DWORD* win32Error,
    DWORD requiredAccess, bool nativeErrors) const
{
    DWORD access = 0;
    const auto object = Lookup(handle, &access);
    if (!object)
    {
        const auto storage = nativeErrors && CurrentGuestKernelContext() == this ? CurrentGuestStorageContext() : nullptr;
        SetWin32Error(win32Error, storage && storage->OwnsFile(handle) ? ERROR_INVALID_FUNCTION : ERROR_INVALID_HANDLE);
        return {};
    }
    if (object->kind != ObjectKind::IoCompletionPort)
    {
        SetWin32Error(win32Error, nativeErrors ? ERROR_INVALID_FUNCTION : ERROR_INVALID_HANDLE);
        return {};
    }
    if (object->ioPort->IsClosed() || (access & requiredAccess) != requiredAccess)
    {
        SetWin32Error(win32Error, object->ioPort->IsClosed() ? ERROR_INVALID_HANDLE : ERROR_ACCESS_DENIED);
        return {};
    }
    SetWin32Error(win32Error, ERROR_SUCCESS);
    return object->ioPort;
}

bool GuestKernelContext::IsSynchronizationObject(ObjectKind kind)
{
    return kind == ObjectKind::Event || kind == ObjectKind::Mutex || kind == ObjectKind::Semaphore ||
        kind == ObjectKind::WaitableTimer || kind == ObjectKind::IoCompletionPort;
}

DWORD GuestKernelContext::SynchronizationAllAccess(ObjectKind kind)
{
    return STANDARD_RIGHTS_REQUIRED | SYNCHRONIZE | SynchronizationQueryAccess |
        (kind == ObjectKind::Mutex ? 0 : SynchronizationModifyAccess);
}

DWORD GuestKernelContext::MapSynchronizationAccess(ObjectKind kind, DWORD access, bool allowMaximum)
{
    DWORD mapped = access & ~(GENERIC_READ | GENERIC_WRITE | GENERIC_EXECUTE | GENERIC_ALL | MAXIMUM_ALLOWED);
    if (access & GENERIC_READ) mapped |= STANDARD_RIGHTS_READ | SynchronizationQueryAccess;
    if (access & GENERIC_WRITE)
        mapped |= STANDARD_RIGHTS_WRITE | (kind == ObjectKind::Mutex ? 0 : SynchronizationModifyAccess);
    if (access & GENERIC_EXECUTE) mapped |= STANDARD_RIGHTS_EXECUTE | SYNCHRONIZE;
    if ((access & GENERIC_ALL) || (allowMaximum && (access & MAXIMUM_ALLOWED)))
        mapped |= SynchronizationAllAccess(kind);
    return mapped;
}

std::shared_ptr<GuestKernelContext::ObjectRecord> GuestKernelContext::FindSynchronizationObjectLocked(
    const std::wstring& key, bool ignoreCase)
{
    // Callers hold the name registry lock. Filter retired objects before selecting a case variant.
    std::lock_guard<std::mutex> guard(s_dispatcherLock);
    const auto findLive = [](const std::weak_ptr<ObjectRecord>& weak)
    {
        auto object = weak.lock();
        return object && !object->closed ? object : nullptr;
    };
    const auto found = s_namedObjects.find(key);
    if (found != s_namedObjects.end())
        if (auto object = findLive(found->second)) return object;
    if (ignoreCase)
        for (const auto& entry : s_namedObjects)
            if (::CompareStringOrdinal(key.c_str(), -1, entry.first.c_str(), -1, TRUE) == CSTR_EQUAL)
                if (auto object = findLive(entry.second)) return object;
    return nullptr;
}

HANDLE GuestKernelContext::CreateSynchronizationObject(const std::shared_ptr<ObjectRecord>& candidate,
    LPCWSTR name, DWORD access, bool inherit, DWORD* win32Error, bool* created,
    const NativeObjectNameOptions* nativeName)
{
    std::wstring key;
    if (!SynchronizationObjectName(name, false, key, win32Error)) return nullptr;
    const DWORD granted = MapSynchronizationAccess(candidate->kind, access, true) & SynchronizationAllAccess(candidate->kind);
    HANDLE handle = nullptr;
    if (key.empty())
    {
        if (!AddObject(candidate, &handle, win32Error, inherit, granted)) return nullptr;
    }
    else
    {
        std::lock_guard<std::mutex> namesGuard(s_namedObjectsLock);
        const auto existing = FindSynchronizationObjectLocked(key, nativeName && nativeName->ignoreCase);
        if (existing)
        {
            if (nativeName && !nativeName->openIf)
            {
                SetWin32Error(win32Error, ERROR_ALREADY_EXISTS);
                return nullptr;
            }
            if (existing->kind != candidate->kind)
            {
                SetWin32Error(win32Error, nativeName ? ERROR_INVALID_FUNCTION : ERROR_INVALID_HANDLE);
                return nullptr;
            }
            DWORD error = ERROR_SUCCESS;
            if (AddObject(existing, &handle, &error, inherit, granted))
            {
                if (created) *created = false;
                SetWin32Error(win32Error, ERROR_ALREADY_EXISTS);
                return handle;
            }
            // A concurrent close can retire the named object before AddObject.
            if (error != ERROR_INVALID_HANDLE)
            {
                SetWin32Error(win32Error, error);
                return nullptr;
            }
        }
        // Allocate the registry entry before publishing a guest handle.
        auto& named = s_namedObjects[key];
        if (!AddObject(candidate, &handle, win32Error, inherit, granted))
        {
            s_namedObjects.erase(key);
            return nullptr;
        }
        named = candidate;
    }
    if (created) *created = true;
    SetWin32Error(win32Error, ERROR_SUCCESS);
    return handle;
}

HANDLE GuestKernelContext::OpenSynchronizationObject(ObjectKind kind, DWORD access, BOOL inherit,
    LPCWSTR name, DWORD* win32Error, const NativeObjectNameOptions* nativeName)
{
    try
    {
        std::wstring key;
        if (!SynchronizationObjectName(name, true, key, win32Error)) return nullptr;
        std::lock_guard<std::mutex> namesGuard(s_namedObjectsLock);
        const auto object = FindSynchronizationObjectLocked(key, nativeName && nativeName->ignoreCase);
        if (!object)
        {
            s_namedObjects.erase(key);
            SetWin32Error(win32Error, ERROR_FILE_NOT_FOUND);
            return nullptr;
        }
        if (object->kind != kind)
        {
            SetWin32Error(win32Error, nativeName ? ERROR_INVALID_FUNCTION : ERROR_INVALID_HANDLE);
            return nullptr;
        }
        const DWORD granted = MapSynchronizationAccess(kind, access, true);
        if ((!granted && !nativeName) || (granted & ~SynchronizationAllAccess(kind)))
        {
            SetWin32Error(win32Error, ERROR_ACCESS_DENIED);
            return nullptr;
        }
        HANDLE handle = nullptr;
        DWORD error = ERROR_SUCCESS;
        AddObject(object, &handle, &error, inherit != FALSE, granted);
        SetWin32Error(win32Error, error == ERROR_INVALID_HANDLE ? ERROR_FILE_NOT_FOUND : error);
        return handle;
    }
    catch (const std::bad_alloc&)
    {
        SetWin32Error(win32Error, ERROR_NOT_ENOUGH_MEMORY);
        return nullptr;
    }
}

HANDLE GuestKernelContext::CreateEvent(bool manualReset, bool initialState, LPCWSTR name, DWORD* win32Error,
    bool inherit, DWORD access)
{
    try
    {
        auto event = std::make_shared<ObjectRecord>(ObjectKind::Event);
        event->manualReset = manualReset;
        event->signaled = initialState;
        return CreateSynchronizationObject(event, name, access, inherit, win32Error);
    }
    catch (const std::bad_alloc&)
    {
        SetWin32Error(win32Error, ERROR_NOT_ENOUGH_MEMORY);
        return nullptr;
    }
}

HANDLE GuestKernelContext::OpenEvent(DWORD access, BOOL inherit, LPCWSTR name, DWORD* win32Error)
{ return OpenSynchronizationObject(ObjectKind::Event, access, inherit, name, win32Error); }

HANDLE GuestKernelContext::OpenMutex(DWORD access, BOOL inherit, LPCWSTR name, DWORD* win32Error)
{ return OpenSynchronizationObject(ObjectKind::Mutex, access, inherit, name, win32Error); }

HANDLE GuestKernelContext::OpenSemaphore(DWORD access, BOOL inherit, LPCWSTR name, DWORD* win32Error)
{ return OpenSynchronizationObject(ObjectKind::Semaphore, access, inherit, name, win32Error); }

HANDLE GuestKernelContext::OpenTimerObject(DWORD access, BOOL inherit, LPCWSTR name, DWORD* win32Error,
    const NativeObjectNameOptions* nativeName)
{ return OpenSynchronizationObject(ObjectKind::WaitableTimer, access, inherit, name, win32Error, nativeName); }

HANDLE GuestKernelContext::CreateTimerObject(bool manualReset, LPCWSTR name, DWORD* win32Error,
    bool inherit, DWORD access, const NativeObjectNameOptions* nativeName)
{
    try
    {
        auto timer = std::make_shared<ObjectRecord>(ObjectKind::WaitableTimer);
        timer->manualReset = manualReset;
        return CreateSynchronizationObject(timer, name, access, inherit, win32Error, nullptr, nativeName);
    }
    catch (const std::bad_alloc&)
    {
        SetWin32Error(win32Error, ERROR_NOT_ENOUGH_MEMORY);
        return nullptr;
    }
}

GuestKernelContext::WaitDeadline GuestKernelContext::DeadlineFromNtTime(LONGLONG dueTime)
{
    using Clock = std::chrono::steady_clock;
    using NtTicks = std::chrono::duration<ULONGLONG, std::ratio<1, 10'000'000>>;
    ULONGLONG ticks = 0;
    if (dueTime < 0) ticks = 0 - static_cast<ULONGLONG>(dueTime);
    else if (dueTime)
    {
        FILETIME time{};
        ::GetSystemTimeAsFileTime(&time);
        const ULONGLONG now = FileTimeValue(time);
        if (static_cast<ULONGLONG>(dueTime) > now) ticks = static_cast<ULONGLONG>(dueTime) - now;
    }
    const auto now = Clock::now();
    const auto limit = (WaitDeadline::max)();
    const auto available = std::chrono::duration_cast<NtTicks>(limit - now);
    return ticks <= available.count() ? now + std::chrono::duration_cast<Clock::duration>(NtTicks(ticks)) : limit;
}

GuestKernelContext::WaitableTimerSchedule::~WaitableTimerSchedule()
{
    if (!timer) return;
    ::SetThreadpoolTimer(timer, nullptr, 0, 0);
    ::WaitForThreadpoolTimerCallbacks(timer, TRUE);
    ::CloseThreadpoolTimer(timer);
}

void GuestKernelContext::WaitableTimerSchedule::Arm(WaitDeadline now)
{
    ULARGE_INTEGER expiration{};
    if (absoluteDue)
        expiration.QuadPart = absoluteDue;
    else
    {
        if (due == (WaitDeadline::max)()) return;
        using NtTicks = std::chrono::duration<LONGLONG, std::ratio<1, 10'000'000>>;
        const auto remaining = due - now;
        auto ticks = std::chrono::duration_cast<NtTicks>(remaining);
        if (ticks < remaining) ++ticks;
        expiration.QuadPart = 0 - static_cast<ULONGLONG>((std::max)(LONGLONG{1}, ticks.count()));
    }
    FILETIME deadline{expiration.LowPart, expiration.HighPart};
    ::SetThreadpoolTimer(timer, &deadline, 0, tolerableDelay);
}

void GuestKernelContext::WaitableTimerSchedule::RevokeApc()
{
    if (!apc) return;
    apc->valid.store(false);
    if (apc->pruneQueue) apc->pruneQueue(apc->threadId);
}

bool GuestKernelContext::RefreshWaitableTimerLocked(ObjectRecord& object, WaitDeadline now)
{
    const auto timer = object.timer.get();
    if (!timer || timer->elapsed) return false;
    FILETIME completed{};
    if (timer->absoluteDue)
    {
        ::GetSystemTimeAsFileTime(&completed);
        if (FileTimeValue(completed) < timer->absoluteDue) return false;
        timer->absoluteDue = 0;
        timer->due = now;
    }
    else if (now < timer->due) return false;

    object.signaled = true;
    if (timer->apc && timer->queue)
    {
        if (!FileTimeValue(completed)) ::GetSystemTimeAsFileTime(&completed);
        timer->queue(timer->apc, completed);
    }
    if (timer->period == std::chrono::steady_clock::duration::zero())
        timer->elapsed = true;
    else
    {
        const auto remainder = (now - timer->due) % timer->period;
        const auto untilNext = timer->period - remainder;
        const auto limit = (WaitDeadline::max)();
        timer->due = untilNext > limit - now ? limit : now + untilNext;
        timer->Arm(now);
    }
    s_dispatcherChanged.notify_all();
    return true;
}

void CALLBACK GuestKernelContext::WaitableTimerSchedule::OnTimer(PTP_CALLBACK_INSTANCE, void* context, PTP_TIMER)
{
    const auto timer = static_cast<WaitableTimerSchedule*>(context);
    const auto object = timer->object.lock();
    if (!object) return;
    std::lock_guard<std::mutex> guard(s_dispatcherLock);
    if (object->closed || object->timer.get() != timer || timer->elapsed) return;
    const auto now = std::chrono::steady_clock::now();
    // Re-arm a one-shot callback that arrived before the clock reached its deadline.
    if (!RefreshWaitableTimerLocked(*object, now)) timer->Arm(now);
}

bool GuestKernelContext::ArmTimerObject(HANDLE handle, LONGLONG dueTime, LONG period, ULONG tolerableDelay,
    std::shared_ptr<WaitableTimerApc> apc, TimerApcQueue queue, DWORD* win32Error, bool* previousState)
{
    DWORD access = 0;
    const auto object = Lookup(handle, &access);
    if (!object || object->kind != ObjectKind::WaitableTimer)
    {
        SetWin32Error(win32Error, object ? ERROR_INVALID_FUNCTION : ERROR_INVALID_HANDLE);
        return false;
    }
    if (!(access & SynchronizationModifyAccess) || period < 0)
    {
        SetWin32Error(win32Error, period < 0 ? ERROR_INVALID_PARAMETER : ERROR_ACCESS_DENIED);
        return false;
    }

    std::unique_ptr<WaitableTimerSchedule> next(new (std::nothrow) WaitableTimerSchedule);
    if (!next)
    {
        SetWin32Error(win32Error, ERROR_NOT_ENOUGH_MEMORY);
        return false;
    }
    next->object = object;
    if (dueTime > 0) next->absoluteDue = static_cast<ULONGLONG>(dueTime);
    else next->due = DeadlineFromNtTime(dueTime);
    next->period = std::chrono::milliseconds(period);
    next->tolerableDelay = tolerableDelay;
    next->apc = std::move(apc);
    next->queue = queue;
    next->timer = ::CreateThreadpoolTimer(&WaitableTimerSchedule::OnTimer, next.get(), nullptr);
    if (!next->timer)
    {
        SetWin32Error(win32Error, ::GetLastError());
        return false;
    }

    std::unique_ptr<WaitableTimerSchedule> replaced;
    {
        std::lock_guard<std::mutex> guard(s_dispatcherLock);
        if (object->closed)
        {
            SetWin32Error(win32Error, ERROR_INVALID_HANDLE);
            return false;
        }
        if (next->apc && g_currentGuestKernelScope)
        {
            auto& registrations = g_currentGuestKernelScope->m_timerApcs;
            registrations.erase(std::remove_if(registrations.begin(), registrations.end(), [](const auto& registration)
            {
                const auto apc = registration.apc.lock();
                return !apc || !apc->valid.load();
            }), registrations.end());
            try { registrations.push_back({object, next->apc}); }
            catch (const std::bad_alloc&)
            {
                SetWin32Error(win32Error, ERROR_NOT_ENOUGH_MEMORY);
                return false;
            }
        }
        if (previousState)
        {
            RefreshWaitableTimerLocked(*object, std::chrono::steady_clock::now());
            *previousState = object->signaled;
        }
        replaced = std::move(object->timer);
        if (replaced) replaced->RevokeApc();
        object->signaled = false;
        object->timer = std::move(next);
        object->timer->Arm(std::chrono::steady_clock::now());
    }
    SetWin32Error(win32Error, ERROR_SUCCESS);
    return true;
}

bool GuestKernelContext::CancelTimerObject(HANDLE handle, DWORD* win32Error, bool* currentState)
{
    DWORD access = 0;
    const auto object = Lookup(handle, &access);
    std::unique_ptr<WaitableTimerSchedule> cancelled;
    {
        std::lock_guard<std::mutex> guard(s_dispatcherLock);
        if (!object || object->closed)
        {
            SetWin32Error(win32Error, ERROR_INVALID_HANDLE);
            return false;
        }
        if (object->kind != ObjectKind::WaitableTimer)
        {
            SetWin32Error(win32Error, ERROR_INVALID_FUNCTION);
            return false;
        }
        if (!(access & SynchronizationModifyAccess))
        {
            SetWin32Error(win32Error, ERROR_ACCESS_DENIED);
            return false;
        }
        RefreshWaitableTimerLocked(*object, std::chrono::steady_clock::now());
        if (currentState) *currentState = object->signaled;
        cancelled = std::move(object->timer);
        if (cancelled) cancelled->RevokeApc();
    }
    SetWin32Error(win32Error, ERROR_SUCCESS);
    return true;
}

bool GuestKernelContext::QueryTimerObject(HANDLE handle, LONGLONG& remaining, bool& signaled, DWORD* win32Error)
{
    DWORD access = 0;
    const auto object = Lookup(handle, &access);
    std::lock_guard<std::mutex> guard(s_dispatcherLock);
    if (!object || object->closed)
    {
        SetWin32Error(win32Error, ERROR_INVALID_HANDLE);
        return false;
    }
    if (object->kind != ObjectKind::WaitableTimer)
    {
        SetWin32Error(win32Error, ERROR_INVALID_FUNCTION);
        return false;
    }
    if (!(access & SynchronizationQueryAccess))
    {
        SetWin32Error(win32Error, ERROR_ACCESS_DENIED);
        return false;
    }

    const auto now = std::chrono::steady_clock::now();
    RefreshWaitableTimerLocked(*object, now);
    signaled = object->signaled;
    const auto timer = object->timer.get();
    if (!timer || timer->elapsed)
        remaining = -static_cast<LONGLONG>(::GetTickCount64()) * 10'000;
    else if (timer->absoluteDue)
    {
        FILETIME systemTime{};
        ::GetSystemTimeAsFileTime(&systemTime);
        remaining = static_cast<LONGLONG>(timer->absoluteDue) - static_cast<LONGLONG>(FileTimeValue(systemTime));
    }
    else
    {
        using NtTicks = std::chrono::duration<LONGLONG, std::ratio<1, 10'000'000>>;
        remaining = std::chrono::duration_cast<NtTicks>(timer->due - now).count();
    }
    SetWin32Error(win32Error, ERROR_SUCCESS);
    return true;
}

HANDLE GuestKernelContext::CreateMutex(bool initialOwner, LPCWSTR name, DWORD* win32Error, bool inherit, DWORD access)
{
    try
    {
        auto mutex = std::make_shared<ObjectRecord>(ObjectKind::Mutex);
        const auto scope = g_currentGuestKernelScope;
        if (initialOwner)
        {
            if (scope) scope->m_ownedMutexes.reserve(scope->m_ownedMutexes.size() + 1);
            mutex->owner = std::this_thread::get_id();
            mutex->recursion = 1;
        }
        bool created = false;
        const HANDLE handle = CreateSynchronizationObject(mutex, name, access, inherit, win32Error, &created);
        if (handle && created && initialOwner && scope) scope->m_ownedMutexes.emplace_back(mutex);
        return handle;
    }
    catch (const std::bad_alloc&)
    {
        SetWin32Error(win32Error, ERROR_NOT_ENOUGH_MEMORY);
        return nullptr;
    }
}

HANDLE GuestKernelContext::CreateSemaphore(LONG initialCount, LONG maximumCount, LPCWSTR name, DWORD* win32Error,
    bool inherit, DWORD access)
{
    if (maximumCount <= 0 || initialCount < 0 || initialCount > maximumCount)
    {
        SetWin32Error(win32Error, ERROR_INVALID_PARAMETER);
        return nullptr;
    }
    try
    {
        auto semaphore = std::make_shared<ObjectRecord>(ObjectKind::Semaphore);
        semaphore->semaphoreCount = initialCount;
        semaphore->semaphoreMaximum = maximumCount;
        return CreateSynchronizationObject(semaphore, name, access, inherit, win32Error);
    }
    catch (const std::bad_alloc&)
    {
        SetWin32Error(win32Error, ERROR_NOT_ENOUGH_MEMORY);
        return nullptr;
    }
}

bool GuestKernelContext::CreateCompletionObject(
    DWORD objectId, bool process, Completion* completion, HANDLE* handle, DWORD* win32Error, bool inherit)
{
    if (!completion)
    {
        SetWin32Error(win32Error, ERROR_INVALID_PARAMETER);
        return false;
    }
    try
    {
        auto object = std::make_shared<ObjectRecord>(process ? ObjectKind::Process : ObjectKind::Thread);
        object->manualReset = true;
        object->objectId = objectId;
        const auto key = CompletionKey(objectId, process);
        std::lock_guard<std::mutex> guard(m_handlesLock);
        for (auto item = m_completionObjects.begin(); item != m_completionObjects.end();)
        {
            if (item->second.expired()) item = m_completionObjects.erase(item);
            else ++item;
        }
        if (!m_completionObjects.emplace(key, object).second)
        {
            SetWin32Error(win32Error, ERROR_ALREADY_EXISTS);
            return false;
        }
        if (handle && !AddObjectLocked(object, handle, win32Error, inherit))
        {
            m_completionObjects.erase(key);
            return false;
        }
        completion->m_object = std::move(object);
        SetWin32Error(win32Error, ERROR_SUCCESS);
        return true;
    }
    catch (const std::bad_alloc&)
    {
        SetWin32Error(win32Error, ERROR_NOT_ENOUGH_MEMORY);
        return false;
    }
}

void GuestKernelContext::Completion::Finish(DWORD exitCode) const
{
    if (!m_object) return;
    {
        std::lock_guard<std::mutex> guard(s_dispatcherLock);
        if (m_object->signaled) return;
        m_object->exitCode = exitCode;
        m_object->signaled = true;
    }
    s_dispatcherChanged.notify_all();
}

bool GuestKernelContext::GetExitCode(
    HANDLE guestHandle, bool process, LPDWORD exitCode, DWORD* win32Error) const
{
    const auto object = Lookup(guestHandle);
    if (!object || !exitCode)
    {
        SetWin32Error(win32Error, object ? ERROR_INVALID_PARAMETER : ERROR_INVALID_HANDLE);
        return false;
    }
    DWORD code;
    {
        std::lock_guard<std::mutex> guard(s_dispatcherLock);
        if (object->closed || object->kind != (process ? ObjectKind::Process : ObjectKind::Thread))
        {
            SetWin32Error(win32Error, ERROR_INVALID_HANDLE);
            return false;
        }
        code = object->signaled ? object->exitCode : STILL_ACTIVE;
    }
    *exitCode = code;
    SetWin32Error(win32Error, ERROR_SUCCESS);
    return true;
}

bool GuestKernelContext::GetThreadId(HANDLE guestHandle, LPDWORD threadId, DWORD* win32Error) const
{
    return GetCompletionId(guestHandle, false, threadId, win32Error);
}

bool GuestKernelContext::GetCompletionId(HANDLE guestHandle, bool process, LPDWORD id, DWORD* win32Error) const
{
    const auto object = Lookup(guestHandle);
    if (!object || !id)
    {
        SetWin32Error(win32Error, object ? ERROR_INVALID_PARAMETER : ERROR_INVALID_HANDLE);
        return false;
    }
    {
        std::lock_guard<std::mutex> guard(s_dispatcherLock);
        if (object->closed || object->kind != (process ? ObjectKind::Process : ObjectKind::Thread))
        {
            SetWin32Error(win32Error, ERROR_INVALID_HANDLE);
            return false;
        }
    }
    *id = object->objectId;
    SetWin32Error(win32Error, ERROR_SUCCESS);
    return true;
}

DWORD GuestKernelContext::SignalSynchronizationObjectLocked(const std::shared_ptr<ObjectRecord>& object,
    DWORD access, LONG releaseCount, LONG* previousCount)
{
    if (object->closed) return ERROR_INVALID_HANDLE;
    switch (object->kind)
    {
    case ObjectKind::Event:
        if (!(access & SynchronizationModifyAccess)) return ERROR_ACCESS_DENIED;
        object->signaled = true;
        break;
    case ObjectKind::Semaphore:
        if (!(access & SynchronizationModifyAccess)) return ERROR_ACCESS_DENIED;
        if (releaseCount > object->semaphoreMaximum - object->semaphoreCount) return ERROR_TOO_MANY_POSTS;
        if (previousCount) *previousCount = object->semaphoreCount;
        object->semaphoreCount += releaseCount;
        break;
    case ObjectKind::Mutex:
        if (object->owner != std::this_thread::get_id() || !object->recursion) return ERROR_NOT_OWNER;
        if (--object->recursion) return ERROR_SUCCESS;
        object->owner = std::thread::id();
        if (const auto scope = g_currentGuestKernelScope)
        {
            auto& owned = scope->m_ownedMutexes;
            owned.erase(std::remove_if(owned.begin(), owned.end(), [&](const auto& item)
            {
                const auto held = item.lock();
                return !held || held == object;
            }), owned.end());
        }
        break;
    default:
        return ERROR_INVALID_HANDLE;
    }
    s_dispatcherChanged.notify_all();
    return ERROR_SUCCESS;
}

bool GuestKernelContext::SignalEvent(const std::shared_ptr<ObjectRecord>& object)
{
    if (!object || object->kind != ObjectKind::Event) return false;
    std::lock_guard<std::mutex> guard(s_dispatcherLock);
    return SignalSynchronizationObjectLocked(object, SynchronizationModifyAccess, 1) == ERROR_SUCCESS;
}

bool GuestKernelContext::SignalHandle(HANDLE handle, ObjectKind kind, LONG releaseCount,
    LONG* previousCount, DWORD* win32Error)
{
    DWORD access = 0;
    const auto object = Lookup(handle, &access);
    if (!object || object->kind != kind || releaseCount <= 0)
    {
        SetWin32Error(win32Error, object && releaseCount <= 0 ? ERROR_INVALID_PARAMETER : ERROR_INVALID_HANDLE);
        return false;
    }

    LONG previous = 0;
    DWORD result;
    {
        std::lock_guard<std::mutex> guard(s_dispatcherLock);
        result = SignalSynchronizationObjectLocked(object, access, releaseCount, &previous);
    }
    if (result == ERROR_SUCCESS && previousCount) *previousCount = previous;
    SetWin32Error(win32Error, result);
    return result == ERROR_SUCCESS;
}

bool GuestKernelContext::SetEvent(HANDLE guestHandle, DWORD* win32Error)
{
    return SignalHandle(guestHandle, ObjectKind::Event, 1, nullptr, win32Error);
}

GuestKernelContext::NativeEventWait::~NativeEventWait()
{
    if (wait) ::CloseThreadpoolWait(wait);
    if (event) ::CloseHandle(event);
}

void CALLBACK GuestKernelContext::NativeEventWait::OnReady(PTP_CALLBACK_INSTANCE, void* context, PTP_WAIT,
    TP_WAIT_RESULT result)
{
    auto pending = static_cast<NativeEventWait*>(context);
    auto kernel = pending->kernel;
    std::lock_guard<std::mutex> lock(kernel->m_nativeEventWaitsLock);
    const auto entry = kernel->m_nativeEventWaits.find(pending);
    if (entry == kernel->m_nativeEventWaits.end()) return;
    if (result == WAIT_OBJECT_0 && !kernel->IsStopping())
    {
        const bool live = SignalEvent(pending->object.lock());
        if (pending->owner && live) ::SetThreadpoolWait(pending->wait, pending->event, nullptr);
    }
    if (pending->owner) return;
    // The one-shot wait has fired. The pool defers destroying it until this callback exits.
    kernel->m_nativeEventWaits.erase(entry);
}

HRESULT GuestKernelContext::PrepareNativeEventWait(HANDLE guestHandle, DWORD flags, std::shared_ptr<ObjectRecord>& object,
    std::unique_ptr<NativeEventWait>& pending)
{
    DWORD access = 0;
    object = Lookup(guestHandle, &access);
    {
        std::lock_guard<std::mutex> guard(s_dispatcherLock);
        if (!object || object->closed || object->kind != ObjectKind::Event) return E_HANDLE;
        if (!(access & SynchronizationModifyAccess)) return E_ACCESSDENIED;
    }

    pending.reset(new (std::nothrow) NativeEventWait);
    if (!pending) return E_OUTOFMEMORY;
    pending->kernel = this;
    pending->object = object;
    // DXGI drivers may duplicate or inspect this private event before queueing it.
    pending->event = ::CreateEventExW(nullptr, nullptr, flags, EVENT_ALL_ACCESS);
    if (!pending->event) return HRESULT_FROM_WIN32(::GetLastError());
    pending->wait = ::CreateThreadpoolWait(&NativeEventWait::OnReady, pending.get(), nullptr);
    if (!pending->wait) return HRESULT_FROM_WIN32(::GetLastError());
    return S_OK;
}

HRESULT GuestKernelContext::RegisterNativeEventSignal(HANDLE guestHandle,
    HRESULT (*registerSignal)(HANDLE, void*), void* context)
{
    std::shared_ptr<ObjectRecord> object;
    std::unique_ptr<NativeEventWait> pending;
    const HRESULT prepared = PrepareNativeEventWait(guestHandle, CREATE_EVENT_MANUAL_RESET, object, pending);
    if (FAILED(prepared)) return prepared;

    std::lock_guard<std::mutex> lock(m_nativeEventWaitsLock);
    if (IsStopping()) return HRESULT_FROM_WIN32(ERROR_OPERATION_ABORTED);
    auto registration = pending.get();
    try { m_nativeEventWaits.emplace(registration, std::move(pending)); }
    catch (const std::bad_alloc&) { return E_OUTOFMEMORY; }

    const HRESULT status = registerSignal(registration->event, context);
    if (FAILED(status))
    {
        m_nativeEventWaits.erase(registration);
        return status;
    }
    if (::WaitForSingleObjectEx(registration->event, 0, FALSE) == WAIT_OBJECT_0)
    {
        SignalEvent(object);
        m_nativeEventWaits.erase(registration);
    }
    else
        ::SetThreadpoolWait(registration->wait, registration->event, nullptr);
    return status;
}

HRESULT GuestKernelContext::NativeEventWait::Cancel()
{
    ::SetThreadpoolWait(wait, nullptr, nullptr);
    const HRESULT status = unsubscribe ? unsubscribe(nativeCookie) : S_OK;
    ::WaitForThreadpoolWaitCallbacks(wait, TRUE);
    return status;
}

HRESULT GuestKernelContext::RegisterNativeEventNotification(HANDLE guestHandle, const void* owner, DWORD* cookie,
    const std::function<HRESULT(HANDLE, DWORD*)>& subscribe, std::function<HRESULT(DWORD)> unsubscribe)
{
    if (!cookie) return E_POINTER;
    *cookie = 0;
    if (!owner || !subscribe || !unsubscribe) return E_INVALIDARG;
    std::shared_ptr<ObjectRecord> object;
    std::unique_ptr<NativeEventWait> pending;
    const HRESULT prepared = PrepareNativeEventWait(guestHandle, 0, object, pending);
    if (FAILED(prepared)) return prepared;
    pending->owner = owner;
    pending->unsubscribe = std::move(unsubscribe);

    std::lock_guard<std::mutex> lock(m_nativeEventWaitsLock);
    if (IsStopping()) return HRESULT_FROM_WIN32(ERROR_OPERATION_ABORTED);
    if (!m_nextNativeNotificationCookie) return E_OUTOFMEMORY;
    const auto registration = pending.get();
    try { m_nativeEventWaits.emplace(registration, std::move(pending)); }
    catch (const std::bad_alloc&) { return E_OUTOFMEMORY; }
    const HRESULT status = subscribe(registration->event, &registration->nativeCookie);
    if (FAILED(status))
    {
        m_nativeEventWaits.erase(registration);
        return status;
    }
    registration->cookie = m_nextNativeNotificationCookie++;
    *cookie = registration->cookie;
    if (::WaitForSingleObjectEx(registration->event, 0, FALSE) == WAIT_OBJECT_0) SignalEvent(object);
    ::SetThreadpoolWait(registration->wait, registration->event, nullptr);
    return status;
}

HRESULT GuestKernelContext::UnregisterNativeEventNotification(const void* owner, DWORD cookie)
{
    if (!owner || !cookie) return E_INVALIDARG;
    std::unique_ptr<NativeEventWait> cancelled;
    {
        std::lock_guard<std::mutex> lock(m_nativeEventWaitsLock);
        const auto entry = std::find_if(m_nativeEventWaits.begin(), m_nativeEventWaits.end(), [&](const auto& item)
        {
            return item.second->owner == owner && item.second->cookie == cookie;
        });
        if (entry == m_nativeEventWaits.end()) return E_INVALIDARG;
        cancelled = std::move(entry->second);
        m_nativeEventWaits.erase(entry);
    }
    return cancelled->Cancel();
}

void GuestKernelContext::UnregisterNativeEventNotifications(const void* owner)
{
    if (!owner) return;
    for (;;)
    {
        std::unique_ptr<NativeEventWait> cancelled;
        {
            std::lock_guard<std::mutex> lock(m_nativeEventWaitsLock);
            const auto entry = std::find_if(m_nativeEventWaits.begin(), m_nativeEventWaits.end(),
                [owner](const auto& item) { return item.second->owner == owner; });
            if (entry == m_nativeEventWaits.end()) return;
            cancelled = std::move(entry->second);
            m_nativeEventWaits.erase(entry);
        }
        cancelled->Cancel();
    }
}

void GuestKernelContext::CancelNativeEventWaits()
{
    decltype(m_nativeEventWaits) cancelled;
    {
        std::lock_guard<std::mutex> lock(m_nativeEventWaitsLock);
        cancelled.swap(m_nativeEventWaits);
    }
    for (const auto& item : cancelled) item.second->Cancel();
}

GuestKernelContext::NativeWaitSource::~NativeWaitSource()
{
    if (wait)
    {
        ::SetThreadpoolWait(wait, nullptr, nullptr);
        ::WaitForThreadpoolWaitCallbacks(wait, TRUE);
        ::CloseThreadpoolWait(wait);
    }
    if (handle) ::CloseHandle(handle);
}

void CALLBACK GuestKernelContext::NativeWaitSource::OnReady(PTP_CALLBACK_INSTANCE, void* context, PTP_WAIT,
    TP_WAIT_RESULT result)
{
    auto object = static_cast<ObjectRecord*>(context);
    {
        std::lock_guard<std::mutex> lock(s_dispatcherLock);
        if (!object->nativeWait) return;
        object->nativeWait->armed = false;
        object->signaled = result == WAIT_OBJECT_0;
    }
    s_dispatcherChanged.notify_all();
}

HANDLE GuestKernelContext::PublishNativeWaitHandle(NativeWaitHandle& source, HANDLE nativeHandle)
{
    if (!nativeHandle) return nullptr;
    // Declare resources before the lock so thread-pool cleanup runs after unlock.
    NativeWaitSource incoming;
    incoming.handle = nativeHandle;
    std::shared_ptr<ObjectRecord> object;
    std::lock_guard<std::mutex> lock(m_handlesLock);
    if (IsStopping()) return nullptr;

    HANDLE guestHandle = nullptr;
    DWORD error = ERROR_INVALID_HANDLE;
    if (auto existing = source.m_object.lock())
    {
        if (AddObjectLocked(existing, &guestHandle, &error, false)) return guestHandle;
        if (error != ERROR_INVALID_HANDLE) return nullptr;
    }
    try
    {
        object = std::make_shared<ObjectRecord>(ObjectKind::NativeWait);
        object->nativeWait = std::make_unique<NativeWaitSource>();
    }
    catch (const std::bad_alloc&) { return nullptr; }
    object->nativeWait->handle = incoming.handle;
    incoming.handle = nullptr;
    object->nativeWait->wait = ::CreateThreadpoolWait(&NativeWaitSource::OnReady, object.get(), nullptr);
    if (!object->nativeWait->wait || !AddObjectLocked(object, &guestHandle, &error, false)) return nullptr;
    source.m_object = object;
    return guestHandle;
}

HRESULT GuestKernelContext::PublishGraphicsHandle(HANDLE nativeHandle, LPCWSTR name, bool inherit, HANDLE* output)
{
    if (!nativeHandle || nativeHandle == INVALID_HANDLE_VALUE) return E_HANDLE;
    try
    {
        std::shared_ptr<void> handle(nativeHandle, [](void* value) { ::CloseHandle(value); });
        if (!output) return E_POINTER;
        *output = nullptr;
        std::wstring key;
        if (name)
        {
            const HRESULT validated = GraphicsHandleName(name, key);
            if (FAILED(validated)) return validated;
        }
        auto object = std::make_shared<ObjectRecord>(ObjectKind::GraphicsHandle);
        object->graphicsHandle = std::move(handle);
        DWORD error = ERROR_SUCCESS;
        std::unique_lock<std::mutex> namesGuard(s_namedObjectsLock, std::defer_lock);
        if (!key.empty()) namesGuard.lock();
        std::lock_guard<std::mutex> handlesGuard(m_handlesLock);
        if (IsStopping()) return HRESULT_FROM_WIN32(ERROR_OPERATION_ABORTED);
        if (key.empty())
            return AddObjectLocked(object, output, &error, inherit) ? S_OK : HRESULT_FROM_WIN32(error);

        // Reap closed names even when a caller creates a series of unique names.
        for (auto entry = s_namedGraphicsHandles.begin(); entry != s_namedGraphicsHandles.end();)
            if (entry->second.expired()) entry = s_namedGraphicsHandles.erase(entry);
            else ++entry;
        auto& named = s_namedGraphicsHandles[key];
        if (const auto existing = named.lock())
        {
            std::lock_guard<std::mutex> dispatcherGuard(s_dispatcherLock);
            if (!existing->closed) return HRESULT_FROM_WIN32(ERROR_ALREADY_EXISTS);
        }
        if (!AddObjectLocked(object, output, &error, inherit))
        {
            s_namedGraphicsHandles.erase(key);
            return HRESULT_FROM_WIN32(error);
        }
        named = object;
        return S_OK;
    }
    catch (const std::bad_alloc&) { return E_OUTOFMEMORY; }
}

HRESULT GuestKernelContext::OpenGraphicsHandle(LPCWSTR name, HANDLE* output)
{
    if (!output) return E_POINTER;
    *output = nullptr;
    try
    {
        std::wstring key;
        const HRESULT validated = GraphicsHandleName(name, key);
        if (FAILED(validated)) return validated;
        std::lock_guard<std::mutex> namesGuard(s_namedObjectsLock);
        std::lock_guard<std::mutex> handlesGuard(m_handlesLock);
        if (IsStopping()) return HRESULT_FROM_WIN32(ERROR_OPERATION_ABORTED);
        const auto entry = s_namedGraphicsHandles.find(key);
        const auto object = entry == s_namedGraphicsHandles.end() ? nullptr : entry->second.lock();
        DWORD error = ERROR_INVALID_HANDLE;
        if (object && AddObjectLocked(object, output, &error, false)) return S_OK;
        return HRESULT_FROM_WIN32(error == ERROR_INVALID_HANDLE ? ERROR_FILE_NOT_FOUND : error);
    }
    catch (const std::bad_alloc&) { return E_OUTOFMEMORY; }
}

std::shared_ptr<void> GuestKernelContext::LeaseGraphicsHandle(HANDLE guestHandle) const
{
    const auto object = Lookup(guestHandle);
    if (!object || object->kind != ObjectKind::GraphicsHandle) return {};
    std::lock_guard<std::mutex> guard(s_dispatcherLock);
    return object->closed ? nullptr : object->graphicsHandle;
}

bool GuestKernelContext::SetD3D12Factory(std::shared_ptr<IUnknown> factory)
{
    std::shared_ptr<IUnknown> previous;
    {
        std::lock_guard<std::mutex> guard(m_handlesLock);
        if (IsStopping()) return false;
        previous = std::move(m_d3d12Factory);
        m_d3d12Factory = std::move(factory);
    }
    return true;
}

std::shared_ptr<IUnknown> GuestKernelContext::D3D12Factory() const
{
    std::lock_guard<std::mutex> guard(m_handlesLock);
    return m_d3d12Factory;
}

bool GuestKernelContext::ResetEvent(HANDLE guestHandle, DWORD* win32Error)
{
    DWORD access = 0;
    const auto object = Lookup(guestHandle, &access);
    if (!object)
    {
        SetWin32Error(win32Error, ERROR_INVALID_HANDLE);
        return false;
    }

    std::lock_guard<std::mutex> guard(s_dispatcherLock);
    if (object->closed || object->kind != ObjectKind::Event)
    {
        SetWin32Error(win32Error, ERROR_INVALID_HANDLE);
        return false;
    }
    if (!(access & SynchronizationModifyAccess))
    {
        SetWin32Error(win32Error, ERROR_ACCESS_DENIED);
        return false;
    }
    object->signaled = false;
    SetWin32Error(win32Error, ERROR_SUCCESS);
    return true;
}

bool GuestKernelContext::ReleaseMutex(HANDLE guestHandle, DWORD* win32Error)
{
    return SignalHandle(guestHandle, ObjectKind::Mutex, 1, nullptr, win32Error);
}

bool GuestKernelContext::ReleaseSemaphore(HANDLE guestHandle, LONG releaseCount, LPLONG previousCount, DWORD* win32Error)
{
    return SignalHandle(guestHandle, ObjectKind::Semaphore, releaseCount, previousCount, win32Error);
}

bool GuestKernelContext::ResolveWaitHandles(
    DWORD count, const HANDLE* handles, bool waitAll, WaitObjects* objects, DWORD* win32Error) const
{
    if (!handles || !count || count > MAXIMUM_WAIT_OBJECTS)
    {
        SetWin32Error(win32Error, ERROR_INVALID_PARAMETER);
        return false;
    }
    const auto storage = CurrentGuestKernelContext() == this ? CurrentGuestStorageContext() : nullptr;
    for (DWORD index = 0; index < count; ++index)
    {
        DWORD access = 0;
        auto object = Lookup(handles[index], &access);
        if (!object && storage)
        {
            object = storage->LookupFileSignal(handles[index]).m_object;
            if (object) access = SYNCHRONIZE;
        }
        if (!object || object->kind == ObjectKind::Token || object->kind == ObjectKind::GraphicsHandle)
        {
            SetWin32Error(win32Error, ERROR_INVALID_HANDLE);
            return false;
        }
        if (!(access & SYNCHRONIZE))
        {
            SetWin32Error(win32Error, ERROR_ACCESS_DENIED);
            return false;
        }
        (*objects)[index] = std::move(object);
    }
    if (waitAll)
    {
        // Windows rejects aliases of the same dispatcher object for WAIT_ALL,
        // even when the numeric handles are different.
        for (DWORD index = 1; index < count; ++index)
        {
            if (std::find(objects->begin(), objects->begin() + index, (*objects)[index]) != objects->begin() + index)
            {
                SetWin32Error(win32Error, ERROR_INVALID_PARAMETER);
                return false;
            }
        }
    }
    return true;
}

bool GuestKernelContext::ValidateWaitHandles(DWORD count, const HANDLE* handles, bool waitAll, DWORD* win32Error) const
{
    WaitObjects objects;
    return ResolveWaitHandles(count, handles, waitAll, &objects, win32Error);
}

bool GuestKernelContext::IsWaitReady(const ObjectRecord& object, std::thread::id caller)
{
    if (object.ioPort) return object.ioPort->IsReady();
    if (object.kind == ObjectKind::Mutex)
        return object.owner == std::thread::id() || object.owner == caller;
    if (object.kind == ObjectKind::Semaphore) return object.semaphoreCount > 0;
    return object.signaled;
}

DWORD GuestKernelContext::AcquireWaitObjects(const WaitObjects& objects, DWORD first, DWORD last,
    bool waitAll, std::thread::id caller, DWORD* win32Error)
{
    auto scope = g_currentGuestKernelScope;
    size_t newMutexes = 0;
    for (DWORD index = first; index < last; ++index)
    {
        const auto& object = *objects[index];
        if (object.kind != ObjectKind::Mutex) continue;
        if (object.owner != caller) ++newMutexes;
        else if (object.recursion == (std::numeric_limits<unsigned int>::max)())
        {
            SetWin32Error(win32Error, ERROR_TOO_MANY_POSTS);
            return WAIT_FAILED;
        }
    }
    // Reserving ownership entries before changing any object keeps WAIT_ALL
    // atomic even when allocating memory for mutex ownership fails.
    if (scope && newMutexes)
    {
        try { scope->m_ownedMutexes.reserve(scope->m_ownedMutexes.size() + newMutexes); }
        catch (const std::bad_alloc&)
        {
            SetWin32Error(win32Error, ERROR_NOT_ENOUGH_MEMORY);
            return WAIT_FAILED;
        }
    }

    bool abandoned = false;
    for (DWORD index = first; index < last; ++index)
    {
        auto& object = *objects[index];
        if (object.kind == ObjectKind::Mutex)
        {
            if (scope && object.owner != caller) scope->m_ownedMutexes.emplace_back(objects[index]);
            object.owner = caller;
            ++object.recursion;
            abandoned = abandoned || object.abandoned;
            object.abandoned = false;
        }
        else if (object.kind == ObjectKind::Semaphore) --object.semaphoreCount;
        else if (!object.manualReset) object.signaled = false;
    }
    SetWin32Error(win32Error, ERROR_SUCCESS);
    return (abandoned ? WAIT_ABANDONED_0 : WAIT_OBJECT_0) + (waitAll ? 0 : first);
}

DWORD GuestKernelContext::WaitForSingleObject(HANDLE guestHandle, DWORD milliseconds, DWORD* win32Error)
{
    return WaitForMultipleObjects(1, &guestHandle, false, milliseconds, win32Error);
}

DWORD GuestKernelContext::WaitForMultipleObjects(
    DWORD count, const HANDLE* handles, bool waitAll, DWORD milliseconds, DWORD* win32Error,
    bool (*hasPendingApc)())
{
    return WaitForMultipleObjectsUntil(count, handles, waitAll, DispatcherDeadline(milliseconds), win32Error, hasPendingApc);
}

DWORD GuestKernelContext::WaitForMultipleObjectsUntil(DWORD count, const HANDLE* handles, bool waitAll,
    WaitDeadline deadline, DWORD* win32Error, bool (*hasPendingApc)())
{
    WaitObjects objects;
    if (!ResolveWaitHandles(count, handles, waitAll, &objects, win32Error)) return WAIT_FAILED;

    std::unique_lock<std::mutex> lock(s_dispatcherLock);
    return WaitForResolvedObjects(objects, count, waitAll, deadline, lock, win32Error, hasPendingApc);
}

DWORD GuestKernelContext::SignalObjectAndWait(HANDLE signal, HANDLE wait, DWORD milliseconds,
    DWORD* win32Error, bool (*hasPendingApc)())
{
    return SignalObjectAndWaitUntil(signal, wait, DispatcherDeadline(milliseconds), win32Error, hasPendingApc);
}

DWORD GuestKernelContext::SignalObjectAndWaitUntil(HANDLE signal, HANDLE wait, WaitDeadline deadline,
    DWORD* win32Error, bool (*hasPendingApc)())
{
    DWORD access = 0;
    const auto source = Lookup(signal, &access);
    if (!source)
    {
        const auto storage = CurrentGuestKernelContext() == this ? CurrentGuestStorageContext() : nullptr;
        SetWin32Error(win32Error, storage && storage->OwnsFile(signal) ? ERROR_INVALID_FUNCTION : ERROR_INVALID_HANDLE);
        return WAIT_FAILED;
    }
    WaitObjects objects;
    if (!ResolveWaitHandles(1, &wait, false, &objects, win32Error)) return WAIT_FAILED;

    std::unique_lock<std::mutex> lock(s_dispatcherLock);
    if (CurrentGuestKernelContext() == this && IsStopping())
    {
        SetWin32Error(win32Error, ERROR_OPERATION_ABORTED);
        return WAIT_FAILED;
    }
    if (objects[0]->closed || source->closed)
    {
        SetWin32Error(win32Error, ERROR_INVALID_HANDLE);
        return WAIT_FAILED;
    }
    if (!IsSynchronizationObject(source->kind) || source->kind == ObjectKind::WaitableTimer ||
        source->kind == ObjectKind::IoCompletionPort)
    {
        // The NT entry point distinguishes a type mismatch from an invalid handle.
        SetWin32Error(win32Error, ERROR_INVALID_FUNCTION);
        return WAIT_FAILED;
    }
    const DWORD result = SignalSynchronizationObjectLocked(source, access, 1);
    if (result != ERROR_SUCCESS)
    {
        SetWin32Error(win32Error, result);
        return WAIT_FAILED;
    }
    return WaitForResolvedObjects(objects, 1, false, deadline, lock, win32Error, hasPendingApc);
}

DWORD GuestKernelContext::WaitForResolvedObjects(const WaitObjects& objects, DWORD count, bool waitAll,
    WaitDeadline deadline, std::unique_lock<std::mutex>& lock,
    DWORD* win32Error, bool (*hasPendingApc)())
{
    const bool infinite = deadline == (std::chrono::steady_clock::time_point::max)();
    const bool cancellable = CurrentGuestKernelContext() == this;
    const auto caller = std::this_thread::get_id();
    const auto end = objects.begin() + count;
    for (;;)
    {
        if (cancellable && IsStopping())
        {
            SetWin32Error(win32Error, ERROR_OPERATION_ABORTED);
            return WAIT_FAILED;
        }
        if (std::any_of(objects.begin(), end, [](const auto& object) { return object->closed; }))
        {
            SetWin32Error(win32Error, ERROR_INVALID_HANDLE);
            return WAIT_FAILED;
        }
        const auto now = std::chrono::steady_clock::now();
        for (auto entry = objects.begin(); entry != end; ++entry)
            if ((*entry)->timer) RefreshWaitableTimerLocked(**entry, now);
        if (hasPendingApc && hasPendingApc()) return WAIT_IO_COMPLETION;
        const auto ready = [caller](const auto& entry)
        {
            auto& object = *entry;
            if (object.nativeWait && !object.signaled && !object.nativeWait->armed &&
                ::WaitForSingleObjectEx(object.nativeWait->handle, 0, FALSE) == WAIT_OBJECT_0)
            {
                object.signaled = true;
                s_dispatcherChanged.notify_all();
            }
            return IsWaitReady(object, caller);
        };
        if (waitAll)
        {
            if (std::all_of(objects.begin(), end, ready))
                return AcquireWaitObjects(objects, 0, count, true, caller, win32Error);
        }
        else
        {
            const auto found = std::find_if(objects.begin(), end, ready);
            if (found != end)
            {
                const auto index = static_cast<DWORD>(found - objects.begin());
                return AcquireWaitObjects(objects, index, index + 1, false, caller, win32Error);
            }
        }
        if (!infinite && std::chrono::steady_clock::now() >= deadline)
        {
            SetWin32Error(win32Error, ERROR_SUCCESS);
            return WAIT_TIMEOUT;
        }
        for (auto entry = objects.begin(); entry != end; ++entry)
        {
            auto& object = **entry;
            if (!object.nativeWait || object.signaled || object.nativeWait->armed) continue;
            object.nativeWait->armed = true;
            ::SetThreadpoolWait(object.nativeWait->wait, object.nativeWait->handle, nullptr);
        }
        if (infinite) s_dispatcherChanged.wait(lock);
        else s_dispatcherChanged.wait_until(lock, deadline);
    }
}

void GuestKernelContext::NotifyAlertableWaiters()
{
    // APC producers release the APC queue lock before taking this lock.
    std::lock_guard<std::mutex> guard(s_dispatcherLock);
    s_dispatcherChanged.notify_all();
}

bool GuestKernelContext::HandleFlags(HANDLE guestHandle, DWORD* flags, DWORD mask, bool set, DWORD* win32Error)
{
    constexpr DWORD validFlags = HandleInherit | HandleProtectFromClose;
    if (!flags || (set && (mask & ~validFlags)))
    {
        SetWin32Error(win32Error, ERROR_INVALID_PARAMETER);
        return false;
    }
    const auto token = reinterpret_cast<ULONG_PTR>(guestHandle);
    std::lock_guard<std::mutex> guard(m_handlesLock);
    const auto found = m_handles.find(token);
    if (found == m_handles.end())
    {
        SetWin32Error(win32Error, ERROR_INVALID_HANDLE);
        return false;
    }
    if (set) found->second.flags = (found->second.flags & ~mask) | (*flags & mask);
    else *flags = found->second.flags;
    SetWin32Error(win32Error, ERROR_SUCCESS);
    return true;
}

bool GuestKernelContext::Duplicate(HANDLE guestHandle, bool inherit, HANDLE* duplicate, DWORD* win32Error,
    DWORD desiredAccess, bool sameAccess)
{
    const auto token = reinterpret_cast<ULONG_PTR>(guestHandle);
    std::lock_guard<std::mutex> guard(m_handlesLock);
    const auto found = m_handles.find(token);
    if (found == m_handles.end())
    {
        SetWin32Error(win32Error, ERROR_INVALID_HANDLE);
        return false;
    }
    const auto object = found->second.object;
    DWORD access = found->second.access;
    if (!sameAccess)
    {
        if (!IsSynchronizationObject(object->kind))
        {
            SetWin32Error(win32Error, ERROR_NOT_SUPPORTED);
            return false;
        }
        access = MapSynchronizationAccess(object->kind, desiredAccess, false) & SynchronizationAllAccess(object->kind);
    }
    return AddObjectLocked(object, duplicate, win32Error, inherit, access);
}

bool GuestKernelContext::InheritHandles(const GuestKernelContext& parent, DWORD* win32Error)
{
    if (this == &parent)
    {
        SetWin32Error(win32Error, ERROR_INVALID_PARAMETER);
        return false;
    }
    std::unique_lock<std::mutex> target(m_handlesLock, std::defer_lock);
    std::unique_lock<std::mutex> source(parent.m_handlesLock, std::defer_lock);
    std::lock(target, source);
    if (!m_handles.empty() || !m_completionObjects.empty())
    {
        SetWin32Error(win32Error, ERROR_INVALID_PARAMETER);
        return false;
    }

    try
    {
        decltype(m_handles) inherited;
        decltype(m_completionObjects) completions;
        for (const auto& item : parent.m_handles)
        {
            if (!(item.second.flags & HandleInherit) || item.second.object->kind == ObjectKind::IoCompletionPort)
                continue;
            inherited.emplace(item);
            const auto& object = item.second.object;
            if (object->kind == ObjectKind::Process || object->kind == ObjectKind::Thread)
                completions.emplace(CompletionKey(object->objectId, object->kind == ObjectKind::Process), object);
        }
        {
            std::lock_guard<std::mutex> guard(s_dispatcherLock);
            for (const auto& item : inherited) ++item.second.object->handleReferences;
        }
        m_handles.swap(inherited);
        m_completionObjects.swap(completions);
    }
    catch (const std::bad_alloc&)
    {
        SetWin32Error(win32Error, ERROR_NOT_ENOUGH_MEMORY);
        return false;
    }

    SetWin32Error(win32Error, ERROR_SUCCESS);
    return true;
}

bool GuestKernelContext::CloseHandle(HANDLE guestHandle, DWORD* win32Error)
{
    const auto token = reinterpret_cast<ULONG_PTR>(guestHandle);
    std::shared_ptr<ObjectRecord> object;
    {
        std::lock_guard<std::mutex> guard(m_handlesLock);
        const auto found = m_handles.find(token);
        if (found == m_handles.end() || (found->second.flags & HandleProtectFromClose))
        {
            SetWin32Error(win32Error, ERROR_INVALID_HANDLE);
            return false;
        }
        object = std::move(found->second.object);
        m_handles.erase(found);
    }
    ReleaseHandle(object);
    SetWin32Error(win32Error, ERROR_SUCCESS);
    return true;
}

void GuestKernelContext::ReleaseHandle(const std::shared_ptr<ObjectRecord>& object)
{
    std::unique_ptr<NativeWaitSource> native;
    std::unique_ptr<WaitableTimerSchedule> timer;
    {
        std::lock_guard<std::mutex> guard(s_dispatcherLock);
        if (--object->handleReferences || object->kind == ObjectKind::Process || object->kind == ObjectKind::Thread)
            return;
        object->closed = true;
        object->signaled = false;
        object->owner = std::thread::id();
        object->recursion = 0;
        object->semaphoreCount = 0;
        if (object->ioPort) object->ioPort->CloseGuestHandles();
        native = std::move(object->nativeWait);
        timer = std::move(object->timer);
        if (timer) timer->RevokeApc();
    }
    s_dispatcherChanged.notify_all();
}

void GuestKernelContext::CloseAll()
{
    BeginShutdown();
    CancelNativeEventWaits();
    m_timers.StopAll();
    { std::lock_guard<std::mutex> guard(m_traceLock); m_traceProviders.clear(); }
    m_console.CloseHandles();
    m_pipes.CloseAll();
    decltype(m_handles) handles;
    std::shared_ptr<IUnknown> factory;
    {
        std::lock_guard<std::mutex> guard(m_handlesLock);
        handles.swap(m_handles);
        factory = std::move(m_d3d12Factory);
        m_completionObjects.clear();
        m_nextHandle = FirstHandleToken;
    }
    for (const auto& item : handles) ReleaseHandle(item.second.object);
}

bool GuestKernelContext::AddObject(const std::shared_ptr<ObjectRecord>& object,
    HANDLE* guestHandle, DWORD* win32Error, bool inherit, DWORD access)
{
    std::lock_guard<std::mutex> guard(m_handlesLock);
    return AddObjectLocked(object, guestHandle, win32Error, inherit, access);
}

bool GuestKernelContext::AddObjectLocked(const std::shared_ptr<ObjectRecord>& object,
    HANDLE* guestHandle, DWORD* win32Error, bool inherit, DWORD access)
{
    if (!object || !guestHandle)
    {
        SetWin32Error(win32Error, ERROR_INVALID_PARAMETER);
        return false;
    }
    if (m_handles.size() >= MaximumHandles)
    {
        SetWin32Error(win32Error, ERROR_TOO_MANY_OPEN_FILES);
        return false;
    }

    // Keep the kernel token space disjoint from the virtual file table.
    // OVERLAPPED.hEvent uses the low bit of an event handle as a flag.
    for (size_t attempt = 0; attempt <= MaximumHandles; ++attempt)
    {
        const ULONG_PTR token = m_nextHandle;
        m_nextHandle += 4;
        if (m_nextHandle >= FirstHandleToken + 0x01000000) m_nextHandle = FirstHandleToken;
        if (m_handles.find(token) != m_handles.end()) continue;

        std::lock_guard<std::mutex> guard(s_dispatcherLock);
        if (object->closed)
        {
            SetWin32Error(win32Error, ERROR_INVALID_HANDLE);
            return false;
        }
        try
        {
            m_handles.emplace(token, HandleEntry{object, inherit ? HandleInherit : 0u, access});
        }
        catch (const std::bad_alloc&)
        {
            SetWin32Error(win32Error, ERROR_NOT_ENOUGH_MEMORY);
            return false;
        }
        ++object->handleReferences;
        *guestHandle = reinterpret_cast<HANDLE>(token);
        SetWin32Error(win32Error, ERROR_SUCCESS);
        return true;
    }

    SetWin32Error(win32Error, ERROR_TOO_MANY_OPEN_FILES);
    return false;
}

std::shared_ptr<GuestKernelContext::ObjectRecord> GuestKernelContext::Lookup(HANDLE guestHandle, DWORD* access) const
{
    const auto token = reinterpret_cast<ULONG_PTR>(guestHandle);
    std::lock_guard<std::mutex> guard(m_handlesLock);
    const auto found = m_handles.find(token);
    if (access) *access = found == m_handles.end() ? 0 : found->second.access;
    return found == m_handles.end() ? nullptr : found->second.object;
}

GuestKernelContext* Win32Bridge::Bridge::CurrentGuestKernelContext()
{
    return g_currentGuestKernel;
}

GuestKernelScope::GuestKernelScope(GuestKernelContext* context)
    : m_previous(g_currentGuestKernel),
      m_previousScope(g_currentGuestKernelScope)
{
    g_currentGuestKernel = context;
    g_currentGuestKernelScope = this;
}

GuestKernelScope::~GuestKernelScope()
{
    CancelOwnedTimerApcs();
    AbandonOwnedMutexes();
    g_currentGuestKernelScope = m_previousScope;
    g_currentGuestKernel = m_previous;
}

void GuestKernelScope::CancelOwnedTimerApcs()
{
    for (const auto& registration : m_timerApcs)
    {
        const auto object = registration.object.lock();
        const auto apc = registration.apc.lock();
        if (!object || !apc || !apc->valid.load()) continue;
        std::unique_ptr<GuestKernelContext::WaitableTimerSchedule> cancelled;
        {
            std::lock_guard<std::mutex> guard(GuestKernelContext::s_dispatcherLock);
            if (!object->timer || object->timer->apc != apc) continue;
            GuestKernelContext::RefreshWaitableTimerLocked(*object, std::chrono::steady_clock::now());
            cancelled = std::move(object->timer);
            cancelled->RevokeApc();
        }
    }
}

void GuestKernelScope::AbandonOwnedMutexes()
{
    if (m_ownedMutexes.empty()) return;
    {
        std::lock_guard<std::mutex> guard(GuestKernelContext::s_dispatcherLock);
        const auto caller = std::this_thread::get_id();
        for (const auto& item : m_ownedMutexes)
        {
            const auto object = item.lock();
            if (!object || object->closed || object->owner != caller) continue;
            object->owner = std::thread::id();
            object->recursion = 0;
            object->abandoned = true;
        }
        m_ownedMutexes.clear();
    }
    GuestKernelContext::s_dispatcherChanged.notify_all();
}

ULONG GuestKernelContext::RegisterTraceProvider(const GUID& id, ULONGLONG* handle) {
    static std::atomic<ULONGLONG> next{0x722000000000ull};
    const auto token=next.fetch_add(1);std::lock_guard<std::mutex> guard(m_traceLock);
    try{m_traceProviders.emplace(token,TraceProvider{id,{}});}catch(const std::bad_alloc&){return ERROR_NOT_ENOUGH_MEMORY;}
    *handle=token;return ERROR_SUCCESS;
}
ULONG GuestKernelContext::UnregisterTraceProvider(ULONGLONG handle){std::lock_guard<std::mutex> guard(m_traceLock);return m_traceProviders.erase(handle)?0:ERROR_INVALID_HANDLE;}
ULONG GuestKernelContext::WriteTraceEvent(ULONGLONG handle){std::lock_guard<std::mutex> guard(m_traceLock);return m_traceProviders.count(handle)?0:ERROR_INVALID_HANDLE;}
ULONG GuestKernelContext::SetTraceInformation(ULONGLONG handle,ULONG kind,const BYTE* data,ULONG length){
    std::lock_guard<std::mutex> guard(m_traceLock);auto found=m_traceProviders.find(handle);if(found==m_traceProviders.end())return ERROR_INVALID_HANDLE;
    if(kind!=2)return ERROR_NOT_SUPPORTED;
    if(!data||length<3||length>65535)return ERROR_INVALID_PARAMETER;USHORT size;memcpy(&size,data,sizeof(size));if(size!=length)return ERROR_INVALID_PARAMETER;
    try{found->second.traits.assign(data,data+length);}catch(const std::bad_alloc&){return ERROR_NOT_ENOUGH_MEMORY;}return 0;
}

HANDLE GuestKernelContext::OpenCompletion(DWORD id, bool process, bool inherit, DWORD* error)
{
    std::lock_guard<std::mutex> guard(m_handlesLock);
    const auto found = m_completionObjects.find(CompletionKey(id, process));
    const auto object = found == m_completionObjects.end() ? nullptr : found->second.lock();
    if (!object)
    {
        SetWin32Error(error, ERROR_INVALID_PARAMETER);
        return nullptr;
    }
    HANDLE handle = nullptr;
    return AddObjectLocked(object, &handle, error, inherit) ? handle : nullptr;
}

HANDLE GuestKernelContext::CreateToken(DWORD* error){auto object=std::make_shared<ObjectRecord>(ObjectKind::Token);HANDLE handle=nullptr;return AddObject(object,&handle,error)?handle:nullptr;}
bool GuestKernelContext::IsToken(HANDLE handle)const{auto object=Lookup(handle);if(!object)return false;std::lock_guard<std::mutex> guard(s_dispatcherLock);return object->kind==ObjectKind::Token&&!object->closed;}

void GuestKernelContext::BeginShutdown(){
    if(m_stopping.exchange(true))return;
    CancelGuestFileIo(nullptr, this);
    { std::lock_guard<std::mutex> guard(m_stopLock); m_stopChanged.notify_all(); }
    m_console.CancelReads();
    std::lock_guard<std::mutex> guard(s_dispatcherLock);
    s_dispatcherChanged.notify_all();
}
bool GuestKernelContext::WaitForStop(DWORD milliseconds){
    std::unique_lock<std::mutex> guard(m_stopLock);const auto stopped=[this]{return IsStopping();};
    if(milliseconds==INFINITE){m_stopChanged.wait(guard,stopped);return true;}
    return m_stopChanged.wait_for(guard,std::chrono::milliseconds(milliseconds),stopped);
}
