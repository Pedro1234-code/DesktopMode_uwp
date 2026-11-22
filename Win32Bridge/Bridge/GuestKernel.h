#pragma once

#include <windows.h>
#include "Bridge/GuestConsole.h"
#include "Bridge/GuestPipes.h"
#include "Bridge/GuestThreadpoolTimers.h"

#include <condition_variable>
#include <chrono>
#include <array>
#include <cstddef>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>
#include <unordered_map>
#include <functional>

struct IUnknown;

namespace Win32Bridge
{
namespace Bridge
{
    class GuestIoCompletionPort;

    // Per-run, in-process kernel-object table.  The values handed to a guest
    // are tokens only; host event/mutex handles are never exposed to PE code.
    class GuestKernelContext final : public std::enable_shared_from_this<GuestKernelContext>
    {
        struct ObjectRecord;

    public:
        // The running process or thread owns this reference independently of
        // the handles through which callers observe its completion.
        class Completion final
        {
        public:
            void Finish(DWORD exitCode) const;
        private:
            friend class GuestKernelContext;
            std::shared_ptr<ObjectRecord> m_object;
        };

        class NativeWaitHandle final
        {
        private:
            friend class GuestKernelContext;
            std::weak_ptr<ObjectRecord> m_object;
        };

        class IoEvent final
        {
        public:
            void Set(bool signaled) const;
        private:
            friend class GuestKernelContext;
            std::shared_ptr<ObjectRecord> m_object;
        };

        class FileSignal final
        {
        public:
            void Begin() const;
            void Complete() const;
            void SuppressCompletion() const;
            void Retire() const;
        private:
            friend class GuestKernelContext;
            std::shared_ptr<ObjectRecord> m_object;
        };
        static FileSignal CreateFileSignal();

        // Win32 handle flag values are hidden by some UWP SDK partitions.
        static constexpr DWORD HandleInherit = 0x00000001u;
        static constexpr DWORD HandleProtectFromClose = 0x00000002u;

        GuestKernelContext() = default;
        GuestConsoleContext& Console() { return m_console; }
        GuestPipeContext& Pipes() { return m_pipes; }
        GuestThreadpoolTimers& Timers() { return m_timers; }
        ~GuestKernelContext();

        GuestKernelContext(const GuestKernelContext&) = delete;
        GuestKernelContext& operator=(const GuestKernelContext&) = delete;

        HANDLE CreateEvent(bool manualReset, bool initialState, LPCWSTR name, DWORD* win32Error,
            bool inherit = false, DWORD access = GENERIC_ALL);
        HANDLE OpenCompletion(DWORD id, bool process, bool inherit, DWORD* error);
        HANDLE CreateToken(DWORD* error);
        bool IsToken(HANDLE handle) const;
        HANDLE OpenEvent(DWORD desiredAccess, BOOL inheritHandle, LPCWSTR name, DWORD* win32Error);
        HANDLE CreateMutex(bool initialOwner, LPCWSTR name, DWORD* win32Error, bool inherit = false, DWORD access = GENERIC_ALL);
        HANDLE OpenMutex(DWORD desiredAccess, BOOL inheritHandle, LPCWSTR name, DWORD* win32Error);
        HANDLE CreateSemaphore(LONG initialCount, LONG maximumCount, LPCWSTR name, DWORD* win32Error,
            bool inherit = false, DWORD access = GENERIC_ALL);
        HANDLE OpenSemaphore(DWORD desiredAccess, BOOL inheritHandle, LPCWSTR name, DWORD* win32Error);
        bool SetEvent(HANDLE guestHandle, DWORD* win32Error);
        bool CaptureIoEvent(HANDLE event, IoEvent& result, DWORD* win32Error);
        // Register an asynchronous native producer without exporting the guest token.
        HRESULT RegisterNativeEventSignal(HANDLE guestHandle, HRESULT (*registerSignal)(HANDLE, void*), void* context);
        HRESULT RegisterNativeEventNotification(HANDLE guestHandle, const void* owner, DWORD* cookie,
            const std::function<HRESULT(HANDLE, DWORD*)>& subscribe, std::function<HRESULT(DWORD)> unsubscribe);
        HRESULT UnregisterNativeEventNotification(const void* owner, DWORD cookie);
        void UnregisterNativeEventNotifications(const void* owner);
        // Takes ownership of a native wait-only handle. The caller serializes
        // repeated opens through the same source.
        HANDLE PublishNativeWaitHandle(NativeWaitHandle& source, HANDLE nativeHandle);
        // The dispatcher owns the native handle. A lease pins it for the
        // duration of a DirectX call even if another thread closes its token.
        HRESULT PublishGraphicsHandle(HANDLE nativeHandle, LPCWSTR name, bool inherit, HANDLE* output);
        HRESULT OpenGraphicsHandle(LPCWSTR name, HANDLE* output);
        std::shared_ptr<void> LeaseGraphicsHandle(HANDLE guestHandle) const;
        bool SetD3D12Factory(std::shared_ptr<IUnknown> factory);
        std::shared_ptr<IUnknown> D3D12Factory() const;
        bool ResetEvent(HANDLE guestHandle, DWORD* win32Error);
        bool ReleaseMutex(HANDLE guestHandle, DWORD* win32Error);
        bool ReleaseSemaphore(HANDLE guestHandle, LONG releaseCount, LPLONG previousCount, DWORD* win32Error);
        DWORD WaitForSingleObject(HANDLE guestHandle, DWORD milliseconds, DWORD* win32Error);
        DWORD WaitForMultipleObjects(DWORD count, const HANDLE* handles, bool waitAll, DWORD milliseconds,
            DWORD* win32Error, bool (*hasPendingApc)() = nullptr);
        DWORD SignalObjectAndWait(HANDLE signal, HANDLE wait, DWORD milliseconds, DWORD* win32Error,
            bool (*hasPendingApc)() = nullptr);
        using WaitDeadline = std::chrono::steady_clock::time_point;
        static WaitDeadline DeadlineFromNtTime(LONGLONG dueTime);
        struct WaitableTimerApc
        {
            using Procedure = void (WINAPI*)(void*, DWORD, DWORD);
            Procedure procedure = nullptr;
            void* parameter = nullptr;
            DWORD threadId = 0;
            void (*pruneQueue)(DWORD) = nullptr;
            std::atomic<bool> valid{true};
            std::atomic<bool> queued{false};
        };
        // Used with the dispatcher locked. The queue function only enqueues;
        // guest code runs later on the original alertable thread.
        using TimerApcQueue = void (*)(const std::shared_ptr<WaitableTimerApc>&, FILETIME);
        struct NativeObjectNameOptions
        {
            bool ignoreCase = false;
            bool openIf = false;
        };
        HANDLE CreateTimerObject(bool manualReset, LPCWSTR name, DWORD* win32Error,
            bool inherit = false, DWORD access = GENERIC_ALL, const NativeObjectNameOptions* nativeName = nullptr);
        HANDLE OpenTimerObject(DWORD access, BOOL inherit, LPCWSTR name, DWORD* win32Error,
            const NativeObjectNameOptions* nativeName = nullptr);
        bool ArmTimerObject(HANDLE handle, LONGLONG dueTime, LONG period, ULONG tolerableDelay,
            std::shared_ptr<WaitableTimerApc> apc, TimerApcQueue queue, DWORD* win32Error, bool* previousState = nullptr);
        bool CancelTimerObject(HANDLE handle, DWORD* win32Error, bool* currentState = nullptr);
        bool QueryTimerObject(HANDLE handle, LONGLONG& remaining, bool& signaled, DWORD* win32Error);
        static constexpr DWORD IoCompletionQuery = 0x1;
        static constexpr DWORD IoCompletionModify = 0x2;
        HANDLE CreateIoPort(DWORD concurrency, DWORD* win32Error, LPCWSTR name = nullptr,
            bool inherit = false, DWORD access = GENERIC_ALL, const NativeObjectNameOptions* nativeName = nullptr);
        HANDLE OpenIoPort(DWORD access, BOOL inherit, LPCWSTR name, DWORD* win32Error,
            const NativeObjectNameOptions& nativeName);
        std::shared_ptr<GuestIoCompletionPort> LookupIoPort(HANDLE handle, DWORD* win32Error,
            DWORD access = IoCompletionModify, bool nativeErrors = false) const;
        DWORD WaitForMultipleObjectsUntil(DWORD count, const HANDLE* handles, bool waitAll, WaitDeadline deadline,
            DWORD* win32Error, bool (*hasPendingApc)());
        DWORD SignalObjectAndWaitUntil(HANDLE signal, HANDLE wait, WaitDeadline deadline,
            DWORD* win32Error, bool (*hasPendingApc)());
        static void NotifyAlertableWaiters();
        bool ValidateWaitHandles(DWORD count, const HANDLE* handles, bool waitAll, DWORD* win32Error) const;
        bool CreateCompletionObject(DWORD objectId, bool process, Completion* completion, HANDLE* handle, DWORD* win32Error, bool inherit = false);
        bool GetExitCode(HANDLE guestHandle, bool process, LPDWORD exitCode, DWORD* win32Error) const;
        bool GetThreadId(HANDLE guestHandle, LPDWORD threadId, DWORD* win32Error) const;
        bool GetCompletionId(HANDLE guestHandle, bool process, LPDWORD id, DWORD* win32Error) const;
        bool Owns(HANDLE guestHandle) const { return Lookup(guestHandle) != nullptr; }
        bool HandleFlags(HANDLE guestHandle, DWORD* flags, DWORD mask, bool set, DWORD* win32Error);
        bool Duplicate(HANDLE guestHandle, bool inherit, HANDLE* duplicate, DWORD* win32Error,
            DWORD desiredAccess = 0, bool sameAccess = true);
        bool InheritHandles(const GuestKernelContext& parent, DWORD* win32Error);

        // This is intentionally separate from storage's CloseFile: Kernel32's
        // CloseHandle shim tries this table first, then its existing file path.
        bool CloseHandle(HANDLE guestHandle, DWORD* win32Error);
        void CloseAll();
        void BeginShutdown();
        bool IsStopping() const { return m_stopping.load(); }
        bool WaitForStop(DWORD milliseconds);
        ULONG RegisterTraceProvider(const GUID& id, ULONGLONG* handle);
        ULONG UnregisterTraceProvider(ULONGLONG handle);
        ULONG WriteTraceEvent(ULONGLONG handle);
        ULONG SetTraceInformation(ULONGLONG handle, ULONG kind, const BYTE* data, ULONG length);

    private:
        friend class GuestKernelScope;
        struct TraceProvider { GUID id; std::vector<BYTE> traits; };
        std::mutex m_traceLock;
        std::unordered_map<ULONGLONG,TraceProvider> m_traceProviders;
        std::atomic<bool> m_stopping{false};
        std::mutex m_stopLock;
        std::condition_variable m_stopChanged;
        GuestThreadpoolTimers m_timers;
        GuestConsoleContext m_console;
        GuestPipeContext m_pipes;
        enum class ObjectKind
        {
            Event,
            NativeWait,
            GraphicsHandle,
            Token,
            Mutex,
            Semaphore,
            WaitableTimer,
            IoCompletionPort,
            File,
            Process,
            Thread
        };

        struct NativeWaitSource
        {
            ~NativeWaitSource();
            static void CALLBACK OnReady(PTP_CALLBACK_INSTANCE, void*, PTP_WAIT, TP_WAIT_RESULT);

            HANDLE handle = nullptr;
            PTP_WAIT wait = nullptr;
            bool armed = false;
        };

        struct WaitableTimerSchedule
        {
            ~WaitableTimerSchedule();
            static void CALLBACK OnTimer(PTP_CALLBACK_INSTANCE, void*, PTP_TIMER);
            void Arm(WaitDeadline now);
            void RevokeApc();
            PTP_TIMER timer = nullptr;
            std::weak_ptr<ObjectRecord> object;
            WaitDeadline due{};
            ULONGLONG absoluteDue = 0;
            std::chrono::steady_clock::duration period{};
            ULONG tolerableDelay = 0;
            bool elapsed = false;
            std::shared_ptr<WaitableTimerApc> apc;
            TimerApcQueue queue = nullptr;
        };

        struct ObjectRecord
        {
            explicit ObjectRecord(ObjectKind value) : kind(value) {}

            const ObjectKind kind;
            bool closed = false;
            unsigned int handleReferences = 0;

            // Event state.
            bool manualReset = false;
            bool signaled = false;
            bool suppressFileSignal = false;
            std::unique_ptr<NativeWaitSource> nativeWait;
            std::unique_ptr<WaitableTimerSchedule> timer;
            std::shared_ptr<GuestIoCompletionPort> ioPort;
            std::shared_ptr<void> graphicsHandle;

            // Mutex state.  A guest thread is represented by its bridge-host
            // thread identity, which preserves recursive acquisition.
            std::thread::id owner;
            unsigned int recursion = 0;
            bool abandoned = false;

            // Semaphore state.
            LONG semaphoreCount = 0;
            LONG semaphoreMaximum = 0;

            DWORD objectId = 0;
            DWORD exitCode = STILL_ACTIVE;
        };
        struct HandleEntry
        {
            std::shared_ptr<ObjectRecord> object;
            DWORD flags;
            DWORD access;
        };

        struct NativeEventWait
        {
            ~NativeEventWait();
            static void CALLBACK OnReady(PTP_CALLBACK_INSTANCE, void*, PTP_WAIT, TP_WAIT_RESULT);

            GuestKernelContext* kernel;
            std::weak_ptr<ObjectRecord> object;
            HANDLE event = nullptr;
            PTP_WAIT wait = nullptr;
            const void* owner = nullptr;
            DWORD cookie = 0;
            DWORD nativeCookie = 0;
            std::function<HRESULT(DWORD)> unsubscribe;

            HRESULT Cancel();
        };

        HRESULT PrepareNativeEventWait(HANDLE guestHandle, DWORD flags, std::shared_ptr<ObjectRecord>& object,
            std::unique_ptr<NativeEventWait>& pending);
        void CancelNativeEventWaits();
        static bool SignalEvent(const std::shared_ptr<ObjectRecord>& object);
        static bool RefreshWaitableTimerLocked(ObjectRecord& object, WaitDeadline now);
        static DWORD SignalSynchronizationObjectLocked(const std::shared_ptr<ObjectRecord>& object, DWORD access,
            LONG releaseCount, LONG* previousCount = nullptr);
        bool SignalHandle(HANDLE handle, ObjectKind kind, LONG releaseCount, LONG* previousCount, DWORD* win32Error);
        std::mutex m_nativeEventWaitsLock;
        std::unordered_map<NativeEventWait*, std::unique_ptr<NativeEventWait>> m_nativeEventWaits;
        DWORD m_nextNativeNotificationCookie = 1;

        static constexpr size_t MaximumHandles = 1024;
        static constexpr ULONG_PTR FirstHandleToken = 0x40000000;

        static constexpr DWORD SynchronizationQueryAccess = 0x1;
        static constexpr DWORD SynchronizationModifyAccess = 0x2;
        static bool IsSynchronizationObject(ObjectKind kind);
        static DWORD SynchronizationAllAccess(ObjectKind kind);
        static DWORD MapSynchronizationAccess(ObjectKind kind, DWORD access, bool allowMaximum);
        static std::shared_ptr<ObjectRecord> FindSynchronizationObjectLocked(const std::wstring& key, bool ignoreCase);
        HANDLE CreateSynchronizationObject(const std::shared_ptr<ObjectRecord>& candidate, LPCWSTR name,
            DWORD access, bool inherit, DWORD* win32Error, bool* created = nullptr,
            const NativeObjectNameOptions* nativeName = nullptr);
        HANDLE OpenSynchronizationObject(ObjectKind kind, DWORD access, BOOL inherit, LPCWSTR name, DWORD* win32Error,
            const NativeObjectNameOptions* nativeName = nullptr);
        bool AddObject(const std::shared_ptr<ObjectRecord>& object, HANDLE* guestHandle, DWORD* win32Error,
            bool inherit = false, DWORD access = MAXDWORD);
        bool AddObjectLocked(const std::shared_ptr<ObjectRecord>& object, HANDLE* guestHandle, DWORD* win32Error,
            bool inherit, DWORD access = MAXDWORD);
        static void ReleaseHandle(const std::shared_ptr<ObjectRecord>& object);
        std::shared_ptr<ObjectRecord> Lookup(HANDLE guestHandle, DWORD* access = nullptr) const;
        using WaitObjects = std::array<std::shared_ptr<ObjectRecord>, MAXIMUM_WAIT_OBJECTS>;
        bool ResolveWaitHandles(DWORD count, const HANDLE* handles, bool waitAll, WaitObjects* objects, DWORD* win32Error) const;
        DWORD WaitForResolvedObjects(const WaitObjects& objects, DWORD count, bool waitAll,
            WaitDeadline deadline, std::unique_lock<std::mutex>& lock,
            DWORD* win32Error, bool (*hasPendingApc)());
        static bool IsWaitReady(const ObjectRecord& object, std::thread::id caller);
        static DWORD AcquireWaitObjects(const WaitObjects& objects, DWORD first, DWORD last, bool waitAll,
            std::thread::id caller, DWORD* win32Error);

        mutable std::mutex m_handlesLock;
        std::unordered_map<ULONG_PTR, HandleEntry> m_handles;
        std::shared_ptr<IUnknown> m_d3d12Factory;
        std::unordered_map<ULONGLONG, std::weak_ptr<ObjectRecord>> m_completionObjects;
        ULONG_PTR m_nextHandle = FirstHandleToken;

        static std::mutex s_namedObjectsLock;
        static std::unordered_map<std::wstring, std::weak_ptr<ObjectRecord>> s_namedObjects;
        static std::unordered_map<std::wstring, std::weak_ptr<ObjectRecord>> s_namedGraphicsHandles;
        // The dispatcher lock makes testing and consuming a wait set atomic,
        // even when another guest process has a handle to the same object.
        static std::mutex s_dispatcherLock;
        static std::condition_variable s_dispatcherChanged;
    };

    // The main guest thread and its workers bind their process's table in TLS.
    GuestKernelContext* CurrentGuestKernelContext();

    class GuestKernelScope final
    {
    public:
        explicit GuestKernelScope(GuestKernelContext* context);
        ~GuestKernelScope();

        GuestKernelScope(const GuestKernelScope&) = delete;
        GuestKernelScope& operator=(const GuestKernelScope&) = delete;
        void AbandonOwnedMutexes();

    private:
        friend class GuestKernelContext;
        void CancelOwnedTimerApcs();
        GuestKernelContext* m_previous;
        GuestKernelScope* m_previousScope;
        std::vector<std::weak_ptr<GuestKernelContext::ObjectRecord>> m_ownedMutexes;
        struct TimerRegistration
        {
            std::weak_ptr<GuestKernelContext::ObjectRecord> object;
            std::weak_ptr<GuestKernelContext::WaitableTimerApc> apc;
        };
        std::vector<TimerRegistration> m_timerApcs;
    };
}
}
