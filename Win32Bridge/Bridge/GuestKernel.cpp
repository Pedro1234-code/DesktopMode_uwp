#include "pch.h"
#include "Bridge\\GuestKernel.h"

#include <chrono>
#include <cwctype>
#include <limits>
#include <string>
#include <vector>

using namespace Win32Bridge::Bridge;

namespace
{
    thread_local GuestKernelContext* g_currentGuestKernel = nullptr;

    void SetWin32Error(DWORD* destination, DWORD value)
    {
        if (destination)
        {
            *destination = value;
        }
    }

    std::wstring NamedObjectKey(LPCWSTR name)
    {
        std::wstring key = name ? name : L"";
        for (auto& character : key)
            character = static_cast<wchar_t>(std::towlower(character));
        return key;
    }
}

std::mutex GuestKernelContext::s_namedObjectsLock;
std::unordered_map<std::wstring, std::weak_ptr<GuestKernelContext::ObjectRecord>>
    GuestKernelContext::s_namedObjects;

GuestKernelContext::~GuestKernelContext()
{
    CloseAll();
}

HANDLE GuestKernelContext::CreateEvent(bool manualReset, bool initialState, LPCWSTR name, DWORD* win32Error)
{
    const std::wstring nameKey = NamedObjectKey(name);
    if (name && nameKey.empty())
    {
        SetWin32Error(win32Error, ERROR_INVALID_NAME);
        return nullptr;
    }

    std::shared_ptr<ObjectRecord> object;
    bool alreadyExists = false;
    if (!nameKey.empty())
    {
        std::lock_guard<std::mutex> namedGuard(s_namedObjectsLock);
        const auto found = s_namedObjects.find(nameKey);
        if (found != s_namedObjects.end())
        {
            object = found->second.lock();
            if (!object)
            {
                s_namedObjects.erase(found);
            }
            else
            {
                std::lock_guard<std::mutex> objectGuard(object->lock);
                if (object->closed)
                {
                    object.reset();
                    s_namedObjects.erase(found);
                }
                else if (object->kind != ObjectKind::Event)
                {
                    SetWin32Error(win32Error, ERROR_INVALID_HANDLE);
                    return nullptr;
                }
                else
                {
                    alreadyExists = true;
                }
            }
        }
        if (!object)
        {
            object = std::make_shared<ObjectRecord>(ObjectKind::Event);
            object->manualReset = manualReset;
            object->signaled = initialState;
            s_namedObjects[nameKey] = object;
        }
    }
    else
    {
        object = std::make_shared<ObjectRecord>(ObjectKind::Event);
        object->manualReset = manualReset;
        object->signaled = initialState;
    }

    HANDLE handle = nullptr;
    if (!AddObject(object, &handle, win32Error))
    {
        return nullptr;
    }
    SetWin32Error(win32Error, alreadyExists ? ERROR_ALREADY_EXISTS : ERROR_SUCCESS);
    return handle;
}

HANDLE GuestKernelContext::OpenEvent(DWORD, BOOL, LPCWSTR name, DWORD* win32Error)
{
    const std::wstring nameKey = NamedObjectKey(name);
    if (nameKey.empty())
    {
        SetWin32Error(win32Error, ERROR_INVALID_NAME);
        return nullptr;
    }

    std::shared_ptr<ObjectRecord> object;
    {
        std::lock_guard<std::mutex> namedGuard(s_namedObjectsLock);
        const auto found = s_namedObjects.find(nameKey);
        if (found == s_namedObjects.end() || !(object = found->second.lock()))
        {
            if (found != s_namedObjects.end()) s_namedObjects.erase(found);
            SetWin32Error(win32Error, ERROR_FILE_NOT_FOUND);
            return nullptr;
        }
        std::lock_guard<std::mutex> objectGuard(object->lock);
        if (object->closed || object->kind != ObjectKind::Event)
        {
            SetWin32Error(win32Error,
                object->kind == ObjectKind::Event ? ERROR_FILE_NOT_FOUND : ERROR_INVALID_HANDLE);
            return nullptr;
        }
    }

    HANDLE handle = nullptr;
    if (!AddObject(object, &handle, win32Error)) return nullptr;
    SetWin32Error(win32Error, ERROR_SUCCESS);
    return handle;
}

HANDLE GuestKernelContext::CreateMutex(bool initialOwner, LPCWSTR name, DWORD* win32Error)
{
    if (name != nullptr)
    {
        SetWin32Error(win32Error, ERROR_NOT_SUPPORTED);
        return nullptr;
    }

    auto object = std::make_shared<ObjectRecord>(ObjectKind::Mutex);
    if (initialOwner)
    {
        object->owner = std::this_thread::get_id();
        object->recursion = 1;
    }

    HANDLE handle = nullptr;
    if (!AddObject(object, &handle, win32Error))
    {
        return nullptr;
    }
    return handle;
}

HANDLE GuestKernelContext::CreateSemaphore(LONG initialCount, LONG maximumCount, LPCWSTR name, DWORD* win32Error)
{
    if (name != nullptr || maximumCount <= 0 || initialCount < 0 || initialCount > maximumCount)
    {
        SetWin32Error(win32Error, name != nullptr ? ERROR_NOT_SUPPORTED : ERROR_INVALID_PARAMETER);
        return nullptr;
    }

    auto object = std::make_shared<ObjectRecord>(ObjectKind::Semaphore);
    object->semaphoreCount = initialCount;
    object->semaphoreMaximum = maximumCount;
    HANDLE handle = nullptr;
    if (!AddObject(object, &handle, win32Error))
    {
        return nullptr;
    }
    return handle;
}

bool GuestKernelContext::SetEvent(HANDLE guestHandle, DWORD* win32Error)
{
    const auto object = Lookup(guestHandle);
    if (!object)
    {
        SetWin32Error(win32Error, ERROR_INVALID_HANDLE);
        return false;
    }

    bool manualReset = false;
    {
        std::lock_guard<std::mutex> guard(object->lock);
        if (object->closed || object->kind != ObjectKind::Event)
        {
            SetWin32Error(win32Error, ERROR_INVALID_HANDLE);
            return false;
        }
        object->signaled = true;
        manualReset = object->manualReset;
    }

    if (manualReset)
    {
        object->stateChanged.notify_all();
    }
    else
    {
        object->stateChanged.notify_one();
    }
    SetWin32Error(win32Error, ERROR_SUCCESS);
    return true;
}

bool GuestKernelContext::ResetEvent(HANDLE guestHandle, DWORD* win32Error)
{
    const auto object = Lookup(guestHandle);
    if (!object)
    {
        SetWin32Error(win32Error, ERROR_INVALID_HANDLE);
        return false;
    }

    std::lock_guard<std::mutex> guard(object->lock);
    if (object->closed || object->kind != ObjectKind::Event)
    {
        SetWin32Error(win32Error, ERROR_INVALID_HANDLE);
        return false;
    }
    object->signaled = false;
    SetWin32Error(win32Error, ERROR_SUCCESS);
    return true;
}

bool GuestKernelContext::ReleaseMutex(HANDLE guestHandle, DWORD* win32Error)
{
    const auto object = Lookup(guestHandle);
    if (!object)
    {
        SetWin32Error(win32Error, ERROR_INVALID_HANDLE);
        return false;
    }

    bool becameAvailable = false;
    {
        std::lock_guard<std::mutex> guard(object->lock);
        if (object->closed || object->kind != ObjectKind::Mutex)
        {
            SetWin32Error(win32Error, ERROR_INVALID_HANDLE);
            return false;
        }
        if (object->owner != std::this_thread::get_id() || object->recursion == 0)
        {
            SetWin32Error(win32Error, ERROR_NOT_OWNER);
            return false;
        }

        --object->recursion;
        if (object->recursion == 0)
        {
            object->owner = std::thread::id();
            becameAvailable = true;
        }
    }

    if (becameAvailable)
    {
        object->stateChanged.notify_one();
    }
    SetWin32Error(win32Error, ERROR_SUCCESS);
    return true;
}

bool GuestKernelContext::ReleaseSemaphore(HANDLE guestHandle, LONG releaseCount, LPLONG previousCount, DWORD* win32Error)
{
    const auto object = Lookup(guestHandle);
    if (!object || releaseCount <= 0)
    {
        SetWin32Error(win32Error, object ? ERROR_INVALID_PARAMETER : ERROR_INVALID_HANDLE);
        return false;
    }

    {
        std::lock_guard<std::mutex> guard(object->lock);
        if (object->closed || object->kind != ObjectKind::Semaphore)
        {
            SetWin32Error(win32Error, ERROR_INVALID_HANDLE);
            return false;
        }
        if (releaseCount > object->semaphoreMaximum - object->semaphoreCount)
        {
            SetWin32Error(win32Error, ERROR_TOO_MANY_POSTS);
            return false;
        }
        if (previousCount)
        {
            *previousCount = object->semaphoreCount;
        }
        object->semaphoreCount += releaseCount;
    }
    object->stateChanged.notify_all();
    SetWin32Error(win32Error, ERROR_SUCCESS);
    return true;
}

DWORD GuestKernelContext::WaitForSingleObject(HANDLE guestHandle, DWORD milliseconds, DWORD* win32Error)
{
    const auto object = Lookup(guestHandle);
    if (!object)
    {
        SetWin32Error(win32Error, ERROR_INVALID_HANDLE);
        return WAIT_FAILED;
    }

    const std::thread::id caller = std::this_thread::get_id();
    std::unique_lock<std::mutex> lock(object->lock);
    if (object->closed)
    {
        SetWin32Error(win32Error, ERROR_INVALID_HANDLE);
        return WAIT_FAILED;
    }

    const auto canAcquire = [&object, &caller]()
        {
            if (object->closed)
            {
                return true;
            }
            if (object->kind == ObjectKind::Event)
            {
                return object->signaled;
            }
            if (object->kind == ObjectKind::Semaphore)
            {
                return object->semaphoreCount > 0;
            }
            return object->owner == std::thread::id() || object->owner == caller;
        };

    if (!canAcquire())
    {
        bool acquired = false;
        if (milliseconds == 0)
        {
            acquired = false;
        }
        else if (milliseconds == INFINITE)
        {
            object->stateChanged.wait(lock, canAcquire);
            acquired = true;
        }
        else
        {
            acquired = object->stateChanged.wait_for(
                lock,
                std::chrono::milliseconds(milliseconds),
                canAcquire);
        }

        if (!acquired)
        {
            SetWin32Error(win32Error, ERROR_SUCCESS);
            return WAIT_TIMEOUT;
        }
    }

    if (object->closed)
    {
        SetWin32Error(win32Error, ERROR_INVALID_HANDLE);
        return WAIT_FAILED;
    }

    if (object->kind == ObjectKind::Event)
    {
        if (!object->manualReset)
        {
            object->signaled = false;
        }
    }
    else if (object->kind == ObjectKind::Semaphore)
    {
        --object->semaphoreCount;
    }
    else
    {
        if (object->owner == caller)
        {
            if (object->recursion == (std::numeric_limits<unsigned int>::max)())
            {
                SetWin32Error(win32Error, ERROR_TOO_MANY_POSTS);
                return WAIT_FAILED;
            }
            ++object->recursion;
        }
        else
        {
            object->owner = caller;
            object->recursion = 1;
        }
    }

    SetWin32Error(win32Error, ERROR_SUCCESS);
    return WAIT_OBJECT_0;
}

DWORD GuestKernelContext::WaitForMultipleObjects(
    DWORD count,
    const HANDLE* handles,
    bool waitAll,
    DWORD milliseconds,
    DWORD* win32Error)
{
    if (!handles || count == 0 || count > MAXIMUM_WAIT_OBJECTS)
    {
        SetWin32Error(win32Error, ERROR_INVALID_PARAMETER);
        return WAIT_FAILED;
    }
    if (waitAll)
    {
        // Consuming auto-reset events/semaphores must be atomic for WAIT_ALL.
        // Keep that contract explicit until the multi-object scheduler exists.
        SetWin32Error(win32Error, ERROR_NOT_SUPPORTED);
        return WAIT_FAILED;
    }

    const auto started = std::chrono::steady_clock::now();
    for (;;)
    {
        for (DWORD index = 0; index < count; ++index)
        {
            DWORD singleError = ERROR_SUCCESS;
            if (WaitForSingleObject(handles[index], 0, &singleError) == WAIT_OBJECT_0)
            {
                SetWin32Error(win32Error, ERROR_SUCCESS);
                return WAIT_OBJECT_0 + index;
            }
            if (singleError != ERROR_SUCCESS)
            {
                SetWin32Error(win32Error, singleError);
                return WAIT_FAILED;
            }
        }
        if (milliseconds == 0)
        {
            SetWin32Error(win32Error, ERROR_SUCCESS);
            return WAIT_TIMEOUT;
        }
        if (milliseconds != INFINITE &&
            std::chrono::steady_clock::now() - started >= std::chrono::milliseconds(milliseconds))
        {
            SetWin32Error(win32Error, ERROR_SUCCESS);
            return WAIT_TIMEOUT;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
}

bool GuestKernelContext::CloseHandle(HANDLE guestHandle, DWORD* win32Error)
{
    const ULONG_PTR token = reinterpret_cast<ULONG_PTR>(guestHandle);
    std::shared_ptr<ObjectRecord> object;
    {
        std::lock_guard<std::mutex> guard(m_handlesLock);
        const auto found = m_handles.find(token);
        if (found == m_handles.end())
        {
            SetWin32Error(win32Error, ERROR_INVALID_HANDLE);
            return false;
        }
        object = found->second;
        m_handles.erase(found);
    }

    bool lastHandle = false;
    {
        std::lock_guard<std::mutex> guard(object->lock);
        if (object->handleReferences != 0) --object->handleReferences;
        lastHandle = object->handleReferences == 0;
        if (lastHandle)
        {
            object->closed = true;
            object->signaled = false;
            object->owner = std::thread::id();
            object->recursion = 0;
            object->semaphoreCount = 0;
        }
    }
    if (lastHandle) object->stateChanged.notify_all();
    SetWin32Error(win32Error, ERROR_SUCCESS);
    return true;
}

void GuestKernelContext::CloseAll()
{
    std::vector<std::shared_ptr<ObjectRecord>> objects;
    {
        std::lock_guard<std::mutex> guard(m_handlesLock);
        objects.reserve(m_handles.size());
        for (const auto& pair : m_handles)
        {
            objects.push_back(pair.second);
        }
        m_handles.clear();
        m_nextHandle = FirstHandleToken;
    }

    for (const auto& object : objects)
    {
        bool lastHandle = false;
        {
            std::lock_guard<std::mutex> guard(object->lock);
            if (object->handleReferences != 0) --object->handleReferences;
            lastHandle = object->handleReferences == 0;
            if (lastHandle)
            {
                object->closed = true;
                object->signaled = false;
                object->owner = std::thread::id();
                object->recursion = 0;
                object->semaphoreCount = 0;
            }
        }
        if (lastHandle) object->stateChanged.notify_all();
    }
}

bool GuestKernelContext::AddObject(
    const std::shared_ptr<ObjectRecord>& object,
    HANDLE* guestHandle,
    DWORD* win32Error)
{
    if (!object || !guestHandle)
    {
        SetWin32Error(win32Error, ERROR_INVALID_PARAMETER);
        return false;
    }

    std::lock_guard<std::mutex> guard(m_handlesLock);
    if (m_handles.size() >= MaximumHandles)
    {
        SetWin32Error(win32Error, ERROR_TOO_MANY_OPEN_FILES);
        return false;
    }

    // The bounded table makes wraparound exceedingly unlikely, but still
    // avoid handing out null or INVALID_HANDLE_VALUE after a long-lived run.
    for (size_t attempt = 0; attempt <= MaximumHandles; ++attempt)
    {
        const ULONG_PTR token = m_nextHandle++;
        if (m_nextHandle == 0 || m_nextHandle == static_cast<ULONG_PTR>(-1))
        {
            m_nextHandle = FirstHandleToken;
        }
        if (token == 0 || token == static_cast<ULONG_PTR>(-1) ||
            m_handles.find(token) != m_handles.end())
        {
            continue;
        }

        {
            std::lock_guard<std::mutex> objectGuard(object->lock);
            if (object->closed)
            {
                SetWin32Error(win32Error, ERROR_INVALID_HANDLE);
                return false;
            }
            ++object->handleReferences;
        }
        m_handles.emplace(token, object);
        *guestHandle = reinterpret_cast<HANDLE>(token);
        SetWin32Error(win32Error, ERROR_SUCCESS);
        return true;
    }

    SetWin32Error(win32Error, ERROR_TOO_MANY_OPEN_FILES);
    return false;
}

std::shared_ptr<GuestKernelContext::ObjectRecord> GuestKernelContext::Lookup(HANDLE guestHandle) const
{
    const ULONG_PTR token = reinterpret_cast<ULONG_PTR>(guestHandle);
    std::lock_guard<std::mutex> guard(m_handlesLock);
    const auto found = m_handles.find(token);
    return found == m_handles.end() ? nullptr : found->second;
}

GuestKernelContext* Win32Bridge::Bridge::CurrentGuestKernelContext()
{
    return g_currentGuestKernel;
}

GuestKernelScope::GuestKernelScope(GuestKernelContext* context)
    : m_previous(g_currentGuestKernel)
{
    g_currentGuestKernel = context;
}

GuestKernelScope::~GuestKernelScope()
{
    g_currentGuestKernel = m_previous;
}
