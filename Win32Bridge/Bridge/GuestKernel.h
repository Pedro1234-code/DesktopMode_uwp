#pragma once

#include <windows.h>

#include <condition_variable>
#include <cstddef>
#include <memory>
#include <mutex>
#include <thread>
#include <unordered_map>

namespace Win32Bridge
{
namespace Bridge
{
    // Per-run, in-process kernel-object table.  The values handed to a guest
    // are tokens only; host event/mutex handles are never exposed to PE code.
    // Named objects and guest thread creation deliberately remain outside this
    // first synchronization slice.
    class GuestKernelContext final
    {
    public:
        GuestKernelContext() = default;
        ~GuestKernelContext();

        GuestKernelContext(const GuestKernelContext&) = delete;
        GuestKernelContext& operator=(const GuestKernelContext&) = delete;

        HANDLE CreateEvent(bool manualReset, bool initialState, LPCWSTR name, DWORD* win32Error);
        HANDLE CreateMutex(bool initialOwner, LPCWSTR name, DWORD* win32Error);
        HANDLE CreateSemaphore(LONG initialCount, LONG maximumCount, LPCWSTR name, DWORD* win32Error);
        bool SetEvent(HANDLE guestHandle, DWORD* win32Error);
        bool ResetEvent(HANDLE guestHandle, DWORD* win32Error);
        bool ReleaseMutex(HANDLE guestHandle, DWORD* win32Error);
        bool ReleaseSemaphore(HANDLE guestHandle, LONG releaseCount, LPLONG previousCount, DWORD* win32Error);
        DWORD WaitForSingleObject(HANDLE guestHandle, DWORD milliseconds, DWORD* win32Error);
        DWORD WaitForMultipleObjects(DWORD count, const HANDLE* handles, bool waitAll, DWORD milliseconds, DWORD* win32Error);

        // This is intentionally separate from storage's CloseFile: Kernel32's
        // CloseHandle shim tries this table first, then its existing file path.
        bool CloseHandle(HANDLE guestHandle, DWORD* win32Error);
        void CloseAll();

    private:
        enum class ObjectKind
        {
            Event,
            Mutex,
            Semaphore
        };

        struct ObjectRecord
        {
            explicit ObjectRecord(ObjectKind value) : kind(value) {}

            const ObjectKind kind;
            std::mutex lock;
            std::condition_variable stateChanged;
            bool closed = false;

            // Event state.
            bool manualReset = false;
            bool signaled = false;

            // Mutex state.  A guest thread is represented by its bridge-host
            // thread identity, which preserves recursive acquisition.
            std::thread::id owner;
            unsigned int recursion = 0;

            // Semaphore state.
            LONG semaphoreCount = 0;
            LONG semaphoreMaximum = 0;
        };

        static constexpr size_t MaximumHandles = 1024;
        static constexpr ULONG_PTR FirstHandleToken = 0x40000000;

        bool AddObject(const std::shared_ptr<ObjectRecord>& object, HANDLE* guestHandle, DWORD* win32Error);
        std::shared_ptr<ObjectRecord> Lookup(HANDLE guestHandle) const;

        mutable std::mutex m_handlesLock;
        std::unordered_map<ULONG_PTR, std::shared_ptr<ObjectRecord>> m_handles;
        ULONG_PTR m_nextHandle = FirstHandleToken;
    };

    // Synchronization shims resolve their per-guest object table through TLS,
    // matching the storage and window scopes.  Future guest thread support can
    // explicitly propagate this context when it becomes available.
    GuestKernelContext* CurrentGuestKernelContext();

    class GuestKernelScope final
    {
    public:
        explicit GuestKernelScope(GuestKernelContext* context);
        ~GuestKernelScope();

        GuestKernelScope(const GuestKernelScope&) = delete;
        GuestKernelScope& operator=(const GuestKernelScope&) = delete;

    private:
        GuestKernelContext* m_previous;
    };
}
}
