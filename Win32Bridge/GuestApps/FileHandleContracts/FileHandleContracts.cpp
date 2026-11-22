// Integration PE: run unchanged on desktop Windows or through Win32Bridge.
// Zero exit means all checks passed; a nonzero exit identifies the failing line.
#include <windows.h>
#include <string>
#include <cwchar>
#include <cstdio>
#include <cstring>
#pragma comment(lib, "synchronization.lib")
namespace {
struct Handle
{
    HANDLE value = nullptr;
    ~Handle() { Reset(); }
    void Reset(HANDLE next = nullptr)
    { if (value && value != INVALID_HANDLE_VALUE) CloseHandle(value); value = next; }
};
struct TemporaryFile
{
    wchar_t path[MAX_PATH]{};
    ~TemporaryFile() { if (*path) DeleteFileW(path); }
};
struct View
{
    char* bytes = nullptr;
    ~View() { if (bytes) UnmapViewOfFile(bytes); }
};
#define REQUIRE(expression) do { if (!(expression)) return __LINE__; } while (false)
int ChildWriter(HANDLE ready)
{
    DWORD flags = 0;
    REQUIRE(GetHandleInformation(ready, &flags) && (flags & HANDLE_FLAG_INHERIT));
    REQUIRE(WaitForSingleObject(ready, 15000) == WAIT_OBJECT_0);
    const HANDLE output = GetStdHandle(STD_OUTPUT_HANDLE);
    wchar_t path[MAX_PATH]{};
    REQUIRE(output && output != INVALID_HANDLE_VALUE);
    REQUIRE(GetFileType(output) == FILE_TYPE_DISK);
    REQUIRE(GetFinalPathNameByHandleW(output, path, MAX_PATH, 0) > 0);
    Handle peer;
    peer.value = CreateFileW(path, GENERIC_READ, 0, nullptr, OPEN_EXISTING, 0, nullptr);
    const DWORD openError = GetLastError();
    REQUIRE(peer.value == INVALID_HANDLE_VALUE && openError == ERROR_SHARING_VIOLATION);
    DWORD done = 0;
    REQUIRE(WriteFile(output, "child", 5, &done, nullptr) && done == 5);
    return 0;
}

DWORD WINAPI ExitFromWorker(void*)
{
    Sleep(30);
    ExitProcess(73);
    return __LINE__;
}

struct ThreadExitInputs { HANDLE mainThread; HANDLE gate; };
ThreadExitInputs exitInputs{};

DWORD WINAPI OutliveMainThread(void* parameter)
{
    const auto& inputs = *static_cast<ThreadExitInputs*>(parameter);
    Handle mainThread;
    mainThread.value = inputs.mainThread;
    REQUIRE(WaitForSingleObject(mainThread.value, 10000) == WAIT_OBJECT_0);
    REQUIRE(WaitForSingleObject(inputs.gate, 10000) == WAIT_OBJECT_0);
    return 54;
}

int CheckThreadShutdown(const wchar_t* executable, bool exitMainThread)
{
    SECURITY_ATTRIBUTES security{ sizeof(SECURITY_ATTRIBUTES), nullptr, TRUE };
    Handle gate;
    std::wstring command = L"\"" + std::wstring(executable) + L"\"";
    if (exitMainThread)
    {
        gate.value = CreateEventW(&security, TRUE, FALSE, nullptr);
        REQUIRE(gate.value);
        command += L" --exit-main-thread " + std::to_wstring(reinterpret_cast<ULONG_PTR>(gate.value));
    }
    else command += L" --exit-worker";

    STARTUPINFOW startup{ sizeof(STARTUPINFOW) };
    PROCESS_INFORMATION process{};
    REQUIRE(CreateProcessW(executable, &command[0], nullptr, nullptr, exitMainThread, 0, nullptr, nullptr, &startup, &process));
    Handle primaryThread, processHandle;
    primaryThread.value = process.hThread;
    processHandle.value = process.hProcess;
    DWORD exit = 0;
    if (exitMainThread)
    {
        REQUIRE(WaitForSingleObject(primaryThread.value, 15000) == WAIT_OBJECT_0);
        REQUIRE(GetExitCodeThread(primaryThread.value, &exit) && exit == 13);
        REQUIRE(GetExitCodeProcess(processHandle.value, &exit) && exit == STILL_ACTIVE);
        REQUIRE(SetEvent(gate.value));
    }
    REQUIRE(WaitForSingleObject(processHandle.value, 15000) == WAIT_OBJECT_0);
    REQUIRE(GetExitCodeProcess(processHandle.value, &exit) && exit == (exitMainThread ? 54u : 73u));
    return 0;
}

DWORD WINAPI OwnMutexAndExit(void* mutex)
{
    return WaitForSingleObject(mutex, 5000) == WAIT_OBJECT_0 ? 0 : __LINE__;
}

struct AlertableWaitInputs { HANDLE ready; HANDLE first; HANDLE second; bool apcRan; };
AlertableWaitInputs alertableInputs{};

void CALLBACK MarkWaitApc(ULONG_PTR argument)
{
    reinterpret_cast<AlertableWaitInputs*>(argument)->apcRan = true;
}

DWORD WINAPI WaitForApc(void* argument)
{
    auto& inputs = *static_cast<AlertableWaitInputs*>(argument);
    const HANDLE objects[] = { inputs.first, inputs.second };
    REQUIRE(SetEvent(inputs.ready));
    REQUIRE(WaitForMultipleObjectsEx(2, objects, TRUE, 5000, TRUE) == WAIT_IO_COMPLETION);
    return inputs.apcRan ? 0 : __LINE__;
}

int CheckSynchronizationAccess()
{
    constexpr DWORD modify = 2;
    wchar_t eventName[100]{}, localName[100]{}, mismatchedEvent[100]{}, mutexName[100]{}, semaphoreName[100]{};
    char ansiEventName[100]{};
    const auto pid = static_cast<unsigned long>(GetCurrentProcessId());
    swprintf_s(eventName, L"Win32BridgeSync%luEvent", pid);
    swprintf_s(localName, L"Local\\Win32BridgeSync%luEvent", pid);
    swprintf_s(mismatchedEvent, L"Win32BridgeSync%luevent", pid);
    swprintf_s(mutexName, L"Win32BridgeSync%luMutex", pid);
    swprintf_s(semaphoreName, L"Win32BridgeSync%luSemaphore", pid);
    sprintf_s(ansiEventName, "Win32BridgeSync%luEvent", pid);

    SECURITY_ATTRIBUTES inheritable{sizeof(SECURITY_ATTRIBUTES), nullptr, TRUE};
    Handle event, setter, alias, duplicate, sameAccess;
    event.value = CreateEventExW(&inheritable, eventName, CREATE_EVENT_MANUAL_RESET, SYNCHRONIZE);
    REQUIRE(event.value);
    DWORD flags = 0;
    REQUIRE(GetHandleInformation(event.value, &flags) && (flags & HANDLE_FLAG_INHERIT));
    REQUIRE(!SetEvent(event.value) && GetLastError() == ERROR_ACCESS_DENIED);
    REQUIRE(!OpenEventW(SYNCHRONIZE, FALSE, mismatchedEvent) && GetLastError() == ERROR_FILE_NOT_FOUND);
    setter.value = OpenEventA(GENERIC_WRITE, FALSE, ansiEventName);
    alias.value = OpenEventW(GENERIC_EXECUTE, FALSE, localName);
    REQUIRE(setter.value && alias.value && SetEvent(setter.value));
    REQUIRE(WaitForSingleObject(setter.value, 0) == WAIT_FAILED && GetLastError() == ERROR_ACCESS_DENIED);
    REQUIRE(WaitForSingleObject(alias.value, 0) == WAIT_OBJECT_0);
    REQUIRE(!ResetEvent(alias.value) && GetLastError() == ERROR_ACCESS_DENIED);

    const HANDLE process = GetCurrentProcess();
    REQUIRE(DuplicateHandle(process, alias.value, process, &sameAccess.value, modify, FALSE, DUPLICATE_SAME_ACCESS));
    REQUIRE(!ResetEvent(sameAccess.value) && GetLastError() == ERROR_ACCESS_DENIED);
    REQUIRE(DuplicateHandle(process, alias.value, process, &duplicate.value, modify, FALSE, 0));
    REQUIRE(ResetEvent(duplicate.value) && WaitForSingleObject(event.value, 0) == WAIT_TIMEOUT);

    Handle mutex, mutexAlias, semaphore, releaser;
    mutex.value = CreateMutexExW(nullptr, mutexName, 1, SYNCHRONIZE);
    mutexAlias.value = OpenMutexW(SYNCHRONIZE, FALSE, mutexName);
    REQUIRE(mutex.value && mutexAlias.value && ReleaseMutex(mutexAlias.value));
    REQUIRE(WaitForSingleObject(mutex.value, 0) == WAIT_OBJECT_0 && ReleaseMutex(mutex.value));
    semaphore.value = CreateSemaphoreExW(nullptr, 0, 1, semaphoreName, 0, SYNCHRONIZE);
    releaser.value = OpenSemaphoreW(modify, FALSE, semaphoreName);
    REQUIRE(semaphore.value && releaser.value);
    REQUIRE(!ReleaseSemaphore(semaphore.value, 1, nullptr) && GetLastError() == ERROR_ACCESS_DENIED);
    REQUIRE(ReleaseSemaphore(releaser.value, 1, nullptr) && WaitForSingleObject(semaphore.value, 0) == WAIT_OBJECT_0);
    return 0;
}

struct TimerCallbackResult
{
    DWORD thread = 0;
    ULONGLONG time = 0;
};

void CALLBACK MarkTimerApc(void* parameter, DWORD low, DWORD high)
{
    const auto result = static_cast<TimerCallbackResult*>(parameter);
    if (!result) return;
    result->thread = GetCurrentThreadId();
    result->time = (static_cast<ULONGLONG>(high) << 32) | low;
}

DWORD WINAPI ArmTimerAndExit(void* parameter)
{
    LARGE_INTEGER due{};
    due.QuadPart = -10'000'000; // One second.
    return SetWaitableTimer(parameter, &due, 0, MarkTimerApc, nullptr, FALSE) ? 0 : GetLastError();
}

int CheckWaitableTimers()
{
    Handle manual, automatic, restricted, modifyOnly, event, worker;
    const char* name = "Local\\Win32BridgeFileHandleContractsTimer";
    manual.value = CreateWaitableTimerA(nullptr, TRUE, name);
    REQUIRE(manual.value && GetLastError() == ERROR_SUCCESS);
    restricted.value = OpenWaitableTimerW(SYNCHRONIZE, TRUE, L"Win32BridgeFileHandleContractsTimer");
    REQUIRE(restricted.value);
    DWORD handleFlags = 0;
    REQUIRE(GetHandleInformation(restricted.value, &handleFlags) && (handleFlags & HANDLE_FLAG_INHERIT));
    modifyOnly.value = OpenWaitableTimerA(TIMER_MODIFY_STATE, FALSE, name);
    REQUIRE(modifyOnly.value);
    automatic.value = CreateWaitableTimerExW(nullptr, nullptr, 0, TIMER_ALL_ACCESS);
    event.value = CreateEventW(nullptr, FALSE, TRUE, nullptr);
    REQUIRE(automatic.value && event.value);

    LARGE_INTEGER expired{}, soon{}, later{};
    expired.QuadPart = 1;
    soon.QuadPart = -200'000; // Twenty milliseconds.
    later.QuadPart = -30'000'000;
    REQUIRE(WaitForSingleObject(manual.value, 0) == WAIT_TIMEOUT);
    REQUIRE(!SetWaitableTimer(restricted.value, &expired, 0, nullptr, nullptr, FALSE) &&
        GetLastError() == ERROR_ACCESS_DENIED);
    REQUIRE(!CancelWaitableTimer(restricted.value) && GetLastError() == ERROR_ACCESS_DENIED);
    REQUIRE(WaitForSingleObject(modifyOnly.value, 0) == WAIT_FAILED && GetLastError() == ERROR_ACCESS_DENIED);
    REQUIRE(!SetWaitableTimer(modifyOnly.value, &expired, -1, nullptr, nullptr, FALSE) &&
        GetLastError() == ERROR_INVALID_PARAMETER);
    REQUIRE(SetWaitableTimer(modifyOnly.value, &expired, 0, nullptr, nullptr, FALSE));
    REQUIRE(WaitForSingleObject(restricted.value, 5000) == WAIT_OBJECT_0);
    REQUIRE(CancelWaitableTimer(manual.value) && WaitForSingleObject(manual.value, 0) == WAIT_OBJECT_0);
    REQUIRE(SetWaitableTimer(manual.value, &later, 0, nullptr, nullptr, FALSE));
    REQUIRE(WaitForSingleObject(manual.value, 0) == WAIT_TIMEOUT && CancelWaitableTimer(manual.value));

    const HANDLE set[] = {event.value, automatic.value};
    REQUIRE(SetWaitableTimerEx(automatic.value, &soon, 0, nullptr, nullptr, nullptr, 0));
    REQUIRE(WaitForMultipleObjects(2, set, TRUE, 5000) == WAIT_OBJECT_0);
    REQUIRE(WaitForSingleObject(automatic.value, 0) == WAIT_TIMEOUT);
    REQUIRE(WaitForSingleObject(event.value, 0) == WAIT_TIMEOUT);
    REQUIRE(SetWaitableTimer(automatic.value, &soon, 30, nullptr, nullptr, FALSE));
    REQUIRE(WaitForSingleObject(automatic.value, 5000) == WAIT_OBJECT_0);
    REQUIRE(WaitForSingleObject(automatic.value, 5000) == WAIT_OBJECT_0 && CancelWaitableTimer(automatic.value));

    TimerCallbackResult callback;
    REQUIRE(SetWaitableTimer(manual.value, &soon, 0, MarkTimerApc, &callback, FALSE));
    REQUIRE(WaitForSingleObject(manual.value, 5000) == WAIT_OBJECT_0 && callback.thread == 0);
    REQUIRE(CancelWaitableTimer(manual.value) && SleepEx(0, TRUE) == 0);
    REQUIRE(SetWaitableTimer(manual.value, &soon, 0, MarkTimerApc, &callback, FALSE));
    REQUIRE(SleepEx(5000, TRUE) == WAIT_IO_COMPLETION);
    REQUIRE(callback.thread == GetCurrentThreadId() && callback.time != 0);
    REQUIRE(WaitForSingleObject(manual.value, 0) == WAIT_OBJECT_0);

    worker.value = CreateThread(nullptr, 0, ArmTimerAndExit, automatic.value, 0, nullptr);
    REQUIRE(worker.value && WaitForSingleObject(worker.value, 5000) == WAIT_OBJECT_0);
    DWORD workerStatus = MAXDWORD;
    REQUIRE(GetExitCodeThread(worker.value, &workerStatus) && workerStatus == 0);
    REQUIRE(WaitForSingleObject(automatic.value, 1200) == WAIT_TIMEOUT);
    return 0;
}

int CheckNativeWaitableTimers()
{
    struct UnicodeString { USHORT length, maxLength; LPCWSTR buffer; };
    struct ObjectAttributes
    {
        ULONG length;
        HANDLE root;
        UnicodeString* name;
        ULONG flags;
        void* security;
        void* qos;
    };
    struct TimerInfo { LARGE_INTEGER remaining; BOOLEAN signaled; };
    using TimerCallback = void (CALLBACK*)(void*, DWORD, DWORD);
    struct TimerCoalescingInfo
    {
        LARGE_INTEGER due;
        TimerCallback callback;
        void* argument;
        void* wakeContext;
        ULONG period, tolerableDelay;
        BOOLEAN* previous;
    };
    static_assert(sizeof(ObjectAttributes) == 48 && sizeof(TimerInfo) == 16 &&
        sizeof(TimerCoalescingInfo) == 48, "NT x64 timer ABI");

    const HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
    using Create = LONG (WINAPI*)(HANDLE*, DWORD, const ObjectAttributes*, ULONG);
    using Open = LONG (WINAPI*)(HANDLE*, DWORD, const ObjectAttributes*);
    using Set = LONG (WINAPI*)(HANDLE, const LARGE_INTEGER*, TimerCallback, void*, BOOLEAN, LONG, BOOLEAN*);
    using SetEx = LONG (WINAPI*)(HANDLE, ULONG, const void*, ULONG);
    using Cancel = LONG (WINAPI*)(HANDLE, BOOLEAN*);
    using Query = LONG (WINAPI*)(HANDLE, ULONG, void*, ULONG, ULONG*);
    const auto create = reinterpret_cast<Create>(GetProcAddress(ntdll, "NtCreateTimer"));
    const auto open = reinterpret_cast<Open>(GetProcAddress(ntdll, "ZwOpenTimer"));
    const auto set = reinterpret_cast<Set>(GetProcAddress(ntdll, "ZwSetTimer"));
    const auto setEx = reinterpret_cast<SetEx>(GetProcAddress(ntdll, "NtSetTimerEx"));
    const auto cancel = reinterpret_cast<Cancel>(GetProcAddress(ntdll, "ZwCancelTimer"));
    const auto query = reinterpret_cast<Query>(GetProcAddress(ntdll, "NtQueryTimer"));
    REQUIRE(create && open && set && setEx && cancel && query);

    Handle timer, modifyOnly, event;
    REQUIRE(create(&timer.value, TIMER_ALL_ACCESS, nullptr, 0) == 0 && timer.value);
    SetLastError(ERROR_ALREADY_EXISTS);
    TimerInfo info{};
    ULONG returned = 99;
    REQUIRE(query(timer.value, 0, &info, sizeof(info) + 1, &returned) == static_cast<LONG>(0xc0000004u));
    REQUIRE(returned == 99);
    alignas(ULONG) BYTE misaligned[sizeof(info) + 1]{};
    REQUIRE(query(timer.value, 0, misaligned + 1, sizeof(info), &returned) == static_cast<LONG>(0x80000002u));
    REQUIRE(returned == 99);
    REQUIRE(query(timer.value, 0, &info, sizeof(info), &returned) == 0);
    REQUIRE(returned == sizeof(info) && !info.signaled && info.remaining.QuadPart <= 0);
    REQUIRE(GetLastError() == ERROR_ALREADY_EXISTS);
    REQUIRE(DuplicateHandle(GetCurrentProcess(), timer.value, GetCurrentProcess(), &modifyOnly.value,
        TIMER_MODIFY_STATE, FALSE, 0));
    REQUIRE(query(modifyOnly.value, 0, &info, sizeof(info), &returned) == static_cast<LONG>(0xc0000022u));
    event.value = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    REQUIRE(event.value && cancel(event.value, nullptr) == static_cast<LONG>(0xc0000024u));

    LARGE_INTEGER overdue{}, later{};
    overdue.QuadPart = 1;
    later.QuadPart = -50'000'000;
    BOOLEAN previous = TRUE;
    REQUIRE(set(timer.value, &overdue, nullptr, nullptr, FALSE, -1, &previous) == static_cast<LONG>(0xc00000f4u));
    REQUIRE(previous);
    REQUIRE(set(modifyOnly.value, &overdue, nullptr, nullptr, FALSE, 0, &previous) == 0 && !previous);
    REQUIRE(WaitForSingleObject(timer.value, 5000) == WAIT_OBJECT_0);
    REQUIRE(cancel(timer.value, &previous) == 0 && previous);
    auto invalidState = reinterpret_cast<BOOLEAN*>(static_cast<ULONG_PTR>(1));
    REQUIRE(cancel(timer.value, invalidState) == static_cast<LONG>(0xc0000005u));
    REQUIRE(query(timer.value, 0, &info, sizeof(info), nullptr) == 0 && info.signaled);
    REQUIRE(set(timer.value, &later, nullptr, nullptr, FALSE, 0, &previous) == 0 && previous);
    REQUIRE(query(timer.value, 0, &info, sizeof(info), nullptr) == 0 &&
        !info.signaled && info.remaining.QuadPart > 0);

    TimerCallbackResult result;
    TimerCoalescingInfo coalescing{};
    coalescing.due.QuadPart = -200'000;
    coalescing.callback = MarkTimerApc;
    coalescing.argument = &result;
    coalescing.tolerableDelay = 5;
    coalescing.previous = &previous;
    REQUIRE(setEx(timer.value, 1, &coalescing, sizeof(coalescing)) == static_cast<LONG>(0xc0000003u));
    REQUIRE(setEx(timer.value, 0, &coalescing, sizeof(coalescing) - 1) == static_cast<LONG>(0xc0000004u));
    REQUIRE(setEx(timer.value, 0, &coalescing, sizeof(coalescing)) == 0 && !previous);
    REQUIRE(SleepEx(5000, TRUE) == WAIT_IO_COMPLETION && result.thread == GetCurrentThreadId() && result.time);
    REQUIRE(set(timer.value, &later, nullptr, nullptr, FALSE, 0, invalidState) == 0);
    REQUIRE(WaitForSingleObject(timer.value, 0) == WAIT_TIMEOUT);

    const auto suffix = std::to_wstring(GetCurrentProcessId());
    const auto path = L"\\BaseNamedObjects\\Win32BridgeNativeTimer" + suffix;
    const auto byteLength = static_cast<USHORT>(path.size() * sizeof(wchar_t));
    UnicodeString name{byteLength, byteLength, path.c_str()};
    ObjectAttributes attributes{sizeof(ObjectAttributes), nullptr, &name, 0, nullptr, nullptr};
    Handle named, alias;
    const LONG namedStatus = create(&named.value, TIMER_ALL_ACCESS, &attributes, 0);
    // A native AppContainer may deny direct access to the global object directory.
    if (namedStatus == static_cast<LONG>(0xc0000022u)) return 0;
    REQUIRE(namedStatus == 0 && named.value);
    REQUIRE(create(&alias.value, TIMER_QUERY_STATE, &attributes, 1) == static_cast<LONG>(0xc0000035u) && !alias.value);
    attributes.flags = 0x80; // OBJ_OPENIF
    REQUIRE(create(&alias.value, TIMER_QUERY_STATE, &attributes, 1) == 0x40000000L && alias.value);
    alias.Reset();
    const auto mixedCase = L"\\BaseNamedObjects\\WIN32BRIDGENATIVETIMER" + suffix;
    name.buffer = mixedCase.c_str();
    REQUIRE(open(&alias.value, TIMER_QUERY_STATE, &attributes) == static_cast<LONG>(0xc0000034u) && !alias.value);
    attributes.flags = 0x42; // OBJ_CASE_INSENSITIVE | OBJ_INHERIT
    REQUIRE(open(&alias.value, TIMER_QUERY_STATE, &attributes) == 0 && alias.value);
    DWORD flags = 0;
    REQUIRE(GetHandleInformation(alias.value, &flags) && (flags & HANDLE_FLAG_INHERIT));
    REQUIRE(set(named.value, &overdue, nullptr, nullptr, FALSE, 0, nullptr) == 0);
    REQUIRE(WaitForSingleObject(named.value, 5000) == WAIT_OBJECT_0);
    REQUIRE(query(alias.value, 0, &info, sizeof(info), nullptr) == 0 && info.signaled);
    return 0;
}

void CALLBACK MarkHandoffApc(ULONG_PTR parameter)
{
    *reinterpret_cast<bool*>(parameter) = true;
}

struct InitOnceTestState
{
    void* context;
    DWORD attempts = 0;
};

BOOL CALLBACK InitializeOnceTest(PINIT_ONCE, void* parameter, void** context)
{
    auto& state = *static_cast<InitOnceTestState*>(parameter);
    if (++state.attempts == 1)
    {
        SetLastError(ERROR_ACCESS_DENIED);
        return FALSE;
    }
    if (context) *context = state.context;
    return TRUE;
}

struct AddressWaitTestState
{
    using Wait = LONG (WINAPI*)(volatile void*, const void*, SIZE_T, const LARGE_INTEGER*);
    Wait nativeWait;
    HANDLE entered;
    volatile LONG value = 0;
};

DWORD WINAPI WaitOnTestAddress(void* parameter)
{
    auto& state = *static_cast<AddressWaitTestState*>(parameter);
    LARGE_INTEGER timeout{};
    timeout.QuadPart = -30'000'000;
    const LONG expected = 0;
    if (!SetEvent(state.entered)) return GetLastError();
    while (InterlockedCompareExchange(&state.value, 0, 0) == expected)
    {
        const LONG status = state.nativeWait(&state.value, &expected, sizeof(expected), &timeout);
        if (status) return static_cast<DWORD>(status);
    }
    return 0;
}

int CheckThreadSynchronization()
{
    const HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
    using Wake = void (WINAPI*)(void*);
    using TryShared = BOOLEAN (WINAPI*)(PSRWLOCK);
    using SleepSrw = LONG (WINAPI*)(PCONDITION_VARIABLE, PSRWLOCK, const LARGE_INTEGER*, ULONG);
    using InitCriticalSection = LONG (WINAPI*)(LPCRITICAL_SECTION);
    using Begin = LONG (WINAPI*)(PINIT_ONCE, ULONG, void**);
    using Execute = LONG (WINAPI*)(PINIT_ONCE, PINIT_ONCE_FN, void*, void**);
    const auto nativeWait = reinterpret_cast<AddressWaitTestState::Wait>(GetProcAddress(ntdll, "RtlWaitOnAddress"));
    const auto nativeWake = reinterpret_cast<Wake>(GetProcAddress(ntdll, "RtlWakeAddressAll"));
    const auto tryShared = reinterpret_cast<TryShared>(GetProcAddress(ntdll, "RtlTryAcquireSRWLockShared"));
    const auto sleepSrw = reinterpret_cast<SleepSrw>(GetProcAddress(ntdll, "RtlSleepConditionVariableSRW"));
    const auto initCriticalSection = reinterpret_cast<InitCriticalSection>(GetProcAddress(ntdll, "RtlInitializeCriticalSection"));
    const auto begin = reinterpret_cast<Begin>(GetProcAddress(ntdll, "RtlRunOnceBeginInitialize"));
    const auto exec = reinterpret_cast<Execute>(GetProcAddress(ntdll, "RtlRunOnceExecuteOnce"));
    REQUIRE(nativeWait && nativeWake && tryShared && sleepSrw && initCriticalSection && begin && exec);

    LARGE_INTEGER poll{};
    volatile LONG value = 1;
    LONG expectation = 0;
    SetLastError(ERROR_ALREADY_EXISTS);
    REQUIRE(WaitOnAddress(&value, &expectation, sizeof(expectation), 0));
    REQUIRE(GetLastError() == ERROR_ALREADY_EXISTS);
    REQUIRE(!WaitOnAddress(&value, &expectation, 3, 0) && GetLastError() == ERROR_INVALID_PARAMETER);
    expectation = value;
    REQUIRE(!WaitOnAddress(&value, &expectation, sizeof(expectation), 0) && GetLastError() == ERROR_TIMEOUT);
    REQUIRE(nativeWait(&value, &expectation, sizeof(expectation), &poll) == WAIT_TIMEOUT);
    REQUIRE(GetLastError() == ERROR_TIMEOUT);
    nativeWake(const_cast<LONG*>(&value));

    Handle entered, thread;
    entered.value = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    REQUIRE(entered.value);
    AddressWaitTestState waiter{nativeWait, entered.value};
    thread.value = CreateThread(nullptr, 0, WaitOnTestAddress, &waiter, 0, nullptr);
    REQUIRE(thread.value);
    const DWORD workerReady = WaitForSingleObject(entered.value, 5000);
    InterlockedExchange(&waiter.value, 1);
    WakeByAddressSingle(const_cast<LONG*>(&waiter.value));
    REQUIRE(WaitForSingleObject(thread.value, 5000) == WAIT_OBJECT_0 && workerReady == WAIT_OBJECT_0);
    DWORD threadStatus = MAXDWORD;
    REQUIRE(GetExitCodeThread(thread.value, &threadStatus) && threadStatus == 0);

    CONDITION_VARIABLE condition;
    SRWLOCK lock;
    InitializeConditionVariable(&condition);
    InitializeSRWLock(&lock);
    REQUIRE(tryShared(&lock));
    const LONG srwStatus = sleepSrw(&condition, &lock, &poll, CONDITION_VARIABLE_LOCKMODE_SHARED);
    const BOOL acquiredUnexpectedly = TryAcquireSRWLockExclusive(&lock);
    if (acquiredUnexpectedly) ReleaseSRWLockExclusive(&lock);
    ReleaseSRWLockShared(&lock);
    REQUIRE(srwStatus == WAIT_TIMEOUT && !acquiredUnexpectedly);
    WakeConditionVariable(&condition);

    CRITICAL_SECTION section{};
    REQUIRE(initCriticalSection(&section) == 0);
    const BOOL enteredSection = TryEnterCriticalSection(&section);
    BOOL conditionResult = TRUE;
    DWORD conditionError = ERROR_SUCCESS;
    if (enteredSection)
    {
        conditionResult = SleepConditionVariableCS(&condition, &section, 0);
        conditionError = GetLastError();
        LeaveCriticalSection(&section);
    }
    DeleteCriticalSection(&section);
    REQUIRE(enteredSection && !conditionResult && conditionError == ERROR_TIMEOUT);

    INIT_ONCE once;
    InitOnceInitialize(&once);
    alignas(4) DWORD storedContext = 42;
    InitOnceTestState initialization{&storedContext};
    void* actualContext = nullptr;
    REQUIRE(begin(&once, INIT_ONCE_CHECK_ONLY, &actualContext) == static_cast<LONG>(0xc0000001u));
    REQUIRE(begin(&once, 0, &actualContext) == 0x103);
    REQUIRE(InitOnceComplete(&once, 0, &storedContext));
    REQUIRE(exec(&once, InitializeOnceTest, &initialization, &actualContext) == 0);
    REQUIRE(actualContext == &storedContext && initialization.attempts == 0);

    INIT_ONCE retry = INIT_ONCE_STATIC_INIT;
    REQUIRE(!InitOnceExecuteOnce(&retry, InitializeOnceTest, &initialization, &actualContext));
    REQUIRE(GetLastError() == ERROR_ACCESS_DENIED && initialization.attempts == 1);
    SetLastError(ERROR_ALREADY_EXISTS);
    REQUIRE(InitOnceExecuteOnce(&retry, InitializeOnceTest, &initialization, &actualContext));
    REQUIRE(GetLastError() == ERROR_ALREADY_EXISTS && initialization.attempts == 2);
    REQUIRE(actualContext == &storedContext);
    return 0;
}

struct FileApcTestResult
{
    DWORD error = MAXDWORD;
    DWORD bytes = 0;
    DWORD thread = 0;
};

void CALLBACK RecordFileApc(DWORD error, DWORD bytes, LPOVERLAPPED overlapped)
{
    const auto result = reinterpret_cast<FileApcTestResult*>(overlapped->hEvent);
    result->error = error;
    result->bytes = bytes;
    result->thread = GetCurrentThreadId();
}

int CheckOverlappedDiskIo(const wchar_t* temporaryPath)
{
    TemporaryFile scratch;
    REQUIRE(GetTempFileNameW(temporaryPath, L"bai", 0, scratch.path));
    Handle file;
    file.value = CreateFileW(scratch.path, GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ,
        nullptr, OPEN_EXISTING, FILE_FLAG_OVERLAPPED, nullptr);
    REQUIRE(file.value != INVALID_HANDLE_VALUE);

    FileApcTestResult completion;
    OVERLAPPED request{};
    request.hEvent = reinterpret_cast<HANDLE>(&completion); // ReadFileEx/WriteFileEx leave hEvent to the caller.
    REQUIRE(WriteFileEx(file.value, "abc", 3, &request, RecordFileApc));
    REQUIRE(completion.thread == 0 && SleepEx(5000, TRUE) == WAIT_IO_COMPLETION);
    REQUIRE(completion.error == 0 && completion.bytes == 3 && completion.thread == GetCurrentThreadId());

    completion = {};
    request.Offset = MAXDWORD;
    request.OffsetHigh = MAXDWORD;
    REQUIRE(WriteFileEx(file.value, "de", 2, &request, RecordFileApc));
    REQUIRE(SleepEx(5000, TRUE) == WAIT_IO_COMPLETION && completion.error == 0 && completion.bytes == 2);

    completion = {};
    request.Offset = 0;
    request.OffsetHigh = 0;
    char text[6]{};
    REQUIRE(ReadFileEx(file.value, text, 5, &request, RecordFileApc));
    REQUIRE(completion.thread == 0 && SleepEx(5000, TRUE) == WAIT_IO_COMPLETION);
    REQUIRE(completion.error == 0 && completion.bytes == 5 && memcmp(text, "abcde", 5) == 0);

    LARGE_INTEGER position{};
    position.QuadPart = 2;
    REQUIRE(SetFilePointerEx(file.value, position, nullptr, FILE_BEGIN));
    OVERLAPPED positioned{};
    positioned.Offset = 1;
    REQUIRE(ReadFile(file.value, text, 3, nullptr, &positioned) || GetLastError() == ERROR_IO_PENDING);
    DWORD bytes = 0;
    REQUIRE(GetOverlappedResultEx(file.value, &positioned, &bytes, 5000, FALSE) && bytes == 3);
    REQUIRE(memcmp(text, "bcd", 3) == 0);
    LARGE_INTEGER zero{};
    REQUIRE(SetFilePointerEx(file.value, zero, &position, FILE_CURRENT) && position.QuadPart == 2);

    OVERLAPPED eof{};
    eof.Offset = 99;
    REQUIRE(!ReadFile(file.value, text, 3, nullptr, &eof));
    if (GetLastError() == ERROR_IO_PENDING)
        REQUIRE(!GetOverlappedResultEx(file.value, &eof, &bytes, 5000, FALSE));
    REQUIRE(GetLastError() == ERROR_HANDLE_EOF);

    Handle event, eventAlias;
    event.value = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    REQUIRE(event.value && DuplicateHandle(GetCurrentProcess(), event.value, GetCurrentProcess(), &eventAlias.value,
        0, FALSE, DUPLICATE_SAME_ACCESS));
    OVERLAPPED signaled{};
    signaled.hEvent = event.value;
    REQUIRE(ReadFile(file.value, text, 5, nullptr, &signaled) || GetLastError() == ERROR_IO_PENDING);
    event.Reset();
    const BOOL canceled = CancelIoEx(file.value, &signaled);
    REQUIRE(canceled || GetLastError() == ERROR_NOT_FOUND);
    REQUIRE(WaitForSingleObject(eventAlias.value, 5000) == WAIT_OBJECT_0);
    const BOOL read = GetOverlappedResult(file.value, &signaled, &bytes, FALSE);
    REQUIRE((read && bytes == 5) || (canceled && GetLastError() == ERROR_OPERATION_ABORTED));
    return 0;
}

int CheckIoCompletionPorts(const wchar_t* temporaryPath)
{
    Handle port, alias, thread;
    port.value = CreateIoCompletionPort(INVALID_HANDLE_VALUE, nullptr, 0, 1);
    REQUIRE(port.value);
    REQUIRE(WaitForSingleObject(port.value, 0) == WAIT_TIMEOUT);
    REQUIRE(!CreateIoCompletionPort(INVALID_HANDLE_VALUE, port.value, 0, 0) &&
        GetLastError() == ERROR_INVALID_PARAMETER);
    REQUIRE(PostQueuedCompletionStatus(port.value, 7, 101, nullptr));
    OVERLAPPED posted{};
    posted.Internal = 42;
    REQUIRE(PostQueuedCompletionStatus(port.value, 8, 202, &posted));
    REQUIRE(WaitForSingleObject(port.value, 0) == WAIT_OBJECT_0);
    REQUIRE(DuplicateHandle(GetCurrentProcess(), port.value, GetCurrentProcess(), &alias.value, 0, FALSE,
        DUPLICATE_SAME_ACCESS));
    port.Reset();

    OVERLAPPED_ENTRY entries[2]{};
    ULONG count = 0;
    REQUIRE(GetQueuedCompletionStatusEx(alias.value, entries, 2, &count, 0, FALSE) && count == 2);
    REQUIRE(entries[0].dwNumberOfBytesTransferred == 7 && entries[0].lpCompletionKey == 101 && !entries[0].lpOverlapped);
    REQUIRE(entries[1].dwNumberOfBytesTransferred == 8 && entries[1].lpCompletionKey == 202 &&
        entries[1].lpOverlapped == &posted && posted.Internal == 42);
    REQUIRE(WaitForSingleObject(alias.value, 0) == WAIT_TIMEOUT);

    thread.value = OpenThread(THREAD_SET_CONTEXT, FALSE, GetCurrentThreadId());
    bool apcRan = false;
    REQUIRE(thread.value && QueueUserAPC(MarkHandoffApc, thread.value, reinterpret_cast<ULONG_PTR>(&apcRan)));
    REQUIRE(PostQueuedCompletionStatus(alias.value, 0, 404, nullptr));
    REQUIRE(GetQueuedCompletionStatusEx(alias.value, entries, 2, &count, 0, TRUE) &&
        count == 1 && entries[0].lpCompletionKey == 404 && !apcRan);
    REQUIRE(!GetQueuedCompletionStatusEx(alias.value, entries, 2, &count, 5000, TRUE) &&
        GetLastError() == WAIT_IO_COMPLETION && count == 0 && apcRan);

    TemporaryFile scratch;
    REQUIRE(GetTempFileNameW(temporaryPath, L"bcp", 0, scratch.path));
    Handle synchronous, file, fileAlias, event;
    constexpr DWORD ShareAll = FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE;
    synchronous.value = CreateFileW(scratch.path, GENERIC_READ, ShareAll, nullptr, OPEN_EXISTING, 0, nullptr);
    REQUIRE(synchronous.value != INVALID_HANDLE_VALUE);
    REQUIRE(!CreateIoCompletionPort(synchronous.value, alias.value, 800, 0) && GetLastError() == ERROR_INVALID_PARAMETER);
    synchronous.Reset();
    file.value = CreateFileW(scratch.path, GENERIC_READ | GENERIC_WRITE, ShareAll, nullptr, OPEN_EXISTING,
        FILE_FLAG_OVERLAPPED, nullptr);
    REQUIRE(file.value != INVALID_HANDLE_VALUE);
    REQUIRE(DuplicateHandle(GetCurrentProcess(), file.value, GetCurrentProcess(), &fileAlias.value, 0, FALSE,
        DUPLICATE_SAME_ACCESS));
    REQUIRE(CreateIoCompletionPort(file.value, alias.value, 800, 0) == alias.value);
    REQUIRE(!CreateIoCompletionPort(fileAlias.value, alias.value, 801, 0) && GetLastError() == ERROR_INVALID_PARAMETER);
    file.Reset();
    OVERLAPPED write{}, read{};
    DWORD bytes = 0;
    ULONG_PTR key = 0;
    LPOVERLAPPED completed = nullptr;
    const char sample[] = "data";
    const DWORD length = sizeof(sample) - 1;
    const BOOL wroteImmediately = WriteFile(fileAlias.value, sample, length, &bytes, &write);
    REQUIRE(wroteImmediately || GetLastError() == ERROR_IO_PENDING);
    if (!wroteImmediately)
        REQUIRE(!GetOverlappedResultEx(fileAlias.value, &write, &bytes, 5, FALSE) && GetLastError() == WAIT_TIMEOUT);
    REQUIRE(GetQueuedCompletionStatus(alias.value, &bytes, &key, &completed, 5000));
    REQUIRE(bytes == length && key == 800 && completed == &write && write.Internal == 0);

    event.value = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    REQUIRE(event.value && (reinterpret_cast<ULONG_PTR>(event.value) & 1u) == 0);
    read.hEvent = reinterpret_cast<HANDLE>(reinterpret_cast<ULONG_PTR>(event.value) | 1u);
    char copied[sizeof(sample)]{};
    const BOOL readImmediately = ReadFile(fileAlias.value, copied, length, &bytes, &read);
    REQUIRE(readImmediately || GetLastError() == ERROR_IO_PENDING);
    REQUIRE(WaitForSingleObject(event.value, 5000) == WAIT_OBJECT_0);
    REQUIRE(GetOverlappedResultEx(fileAlias.value, &read, &bytes, 0, FALSE) && bytes == length);
    REQUIRE(memcmp(copied, sample, length) == 0);
    REQUIRE(!GetQueuedCompletionStatus(alias.value, &bytes, &key, &completed, 0) &&
        GetLastError() == WAIT_TIMEOUT && !completed);

    REQUIRE(SetFileCompletionNotificationModes(fileAlias.value,
        FILE_SKIP_COMPLETION_PORT_ON_SUCCESS | FILE_SKIP_SET_EVENT_ON_HANDLE));
    OVERLAPPED skipped{};
    const BOOL skippedImmediately = WriteFile(fileAlias.value, sample, length, &bytes, &skipped);
    if (skippedImmediately)
    {
        REQUIRE(!GetQueuedCompletionStatus(alias.value, &bytes, &key, &completed, 0) && GetLastError() == WAIT_TIMEOUT);
    }
    else
    {
        REQUIRE(GetLastError() == ERROR_IO_PENDING);
        REQUIRE(GetQueuedCompletionStatus(alias.value, &bytes, &key, &completed, 5000) &&
            bytes == length && key == 800 && completed == &skipped);
    }

    TemporaryFile slot;
    REQUIRE(GetTempFileNameW(temporaryPath, L"bcn", 0, slot.path) && DeleteFileW(slot.path));
    struct DirectoryCleanup
    {
        std::wstring path;
        ~DirectoryCleanup() { RemoveDirectoryW(path.c_str()); }
    } watched{slot.path};
    slot.path[0] = L'\0';
    REQUIRE(CreateDirectoryW(watched.path.c_str(), nullptr));
    alignas(DWORD) BYTE changes[512]{};
    OVERLAPPED request{};
    Handle directory, duplicateDirectory;
    directory.value = CreateFileW(watched.path.c_str(), FILE_LIST_DIRECTORY, ShareAll, nullptr, OPEN_EXISTING,
        FILE_FLAG_OVERLAPPED | FILE_FLAG_BACKUP_SEMANTICS, nullptr);
    REQUIRE(directory.value != INVALID_HANDLE_VALUE);
    REQUIRE(WaitForSingleObject(directory.value, 0) == WAIT_OBJECT_0);
    REQUIRE(CreateIoCompletionPort(directory.value, alias.value, 900, 0) == alias.value);
    REQUIRE(DuplicateHandle(GetCurrentProcess(), directory.value, GetCurrentProcess(), &duplicateDirectory.value, 0,
        FALSE, DUPLICATE_SAME_ACCESS));
    const HANDLE aliases[] = {directory.value, duplicateDirectory.value};
    REQUIRE(WaitForMultipleObjects(2, aliases, FALSE, 0) == WAIT_OBJECT_0);
    REQUIRE(WaitForMultipleObjects(2, aliases, TRUE, 0) == WAIT_FAILED && GetLastError() == ERROR_INVALID_PARAMETER);
    REQUIRE(ReadDirectoryChangesW(directory.value, changes, sizeof(changes), FALSE, FILE_NOTIFY_CHANGE_FILE_NAME,
        nullptr, &request, nullptr));
    REQUIRE(WaitForSingleObject(duplicateDirectory.value, 0) == WAIT_TIMEOUT);
    directory.Reset();
    using CancelNative = LONG (NTAPI*)(HANDLE, const void*, void*);
    const auto cancelNative = reinterpret_cast<CancelNative>(
        GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "NtCancelIoFileEx"));
    ULONG_PTR cancellation[2]{44, 55};
    REQUIRE(cancelNative && cancelNative(duplicateDirectory.value, &request, cancellation) == 0 &&
        cancellation[0] == 0 && cancellation[1] == 0);
    REQUIRE(WaitForSingleObject(alias.value, 5000) == WAIT_OBJECT_0);
    REQUIRE(WaitForSingleObject(duplicateDirectory.value, 0) == WAIT_TIMEOUT);
    REQUIRE(!GetQueuedCompletionStatus(alias.value, &bytes, &key, &completed, 5000));
    REQUIRE(GetLastError() == ERROR_OPERATION_ABORTED && bytes == 0 && key == 900 && completed == &request);
    REQUIRE(static_cast<DWORD>(request.Internal) == 0xc0000120u);
    REQUIRE(SetEvent(event.value));
    const HANDLE fileAndEvent[] = {duplicateDirectory.value, event.value};
    REQUIRE(WaitForMultipleObjects(2, fileAndEvent, TRUE, 0) == WAIT_OBJECT_0);
    REQUIRE(WaitForSingleObject(duplicateDirectory.value, 0) == WAIT_OBJECT_0);
    REQUIRE(!CancelIoEx(duplicateDirectory.value, &request) && GetLastError() == ERROR_NOT_FOUND);

    // An explicit event makes status visible before dequeue and leaves the
    // file's implicit signal cleared, even when a packet is also requested.
    request = {};
    request.hEvent = event.value;
    REQUIRE(ReadDirectoryChangesW(duplicateDirectory.value, changes, sizeof(changes), FALSE,
        FILE_NOTIFY_CHANGE_FILE_NAME, nullptr, &request, nullptr));
    REQUIRE(WaitForSingleObject(event.value, 0) == WAIT_TIMEOUT);
    REQUIRE(CancelIoEx(duplicateDirectory.value, &request));
    REQUIRE(WaitForSingleObject(event.value, 5000) == WAIT_OBJECT_0);
    REQUIRE(!GetOverlappedResult(duplicateDirectory.value, &request, &bytes, FALSE) &&
        GetLastError() == ERROR_OPERATION_ABORTED);
    REQUIRE(!GetQueuedCompletionStatus(alias.value, &bytes, &key, &completed, 5000) &&
        GetLastError() == ERROR_OPERATION_ABORTED && completed == &request);
    REQUIRE(WaitForSingleObject(duplicateDirectory.value, 0) == WAIT_TIMEOUT);

    REQUIRE(SetFileCompletionNotificationModes(duplicateDirectory.value, FILE_SKIP_SET_EVENT_ON_HANDLE));
    request = {};
    REQUIRE(ReadDirectoryChangesW(duplicateDirectory.value, changes, sizeof(changes), FALSE,
        FILE_NOTIFY_CHANGE_FILE_NAME, nullptr, &request, nullptr));
    REQUIRE(CancelIoEx(duplicateDirectory.value, &request));
    REQUIRE(!GetQueuedCompletionStatus(alias.value, &bytes, &key, &completed, 5000) &&
        GetLastError() == ERROR_OPERATION_ABORTED && completed == &request);
    REQUIRE(WaitForSingleObject(duplicateDirectory.value, 0) == WAIT_TIMEOUT);

    request = {};
    REQUIRE(ReadDirectoryChangesW(duplicateDirectory.value, changes, sizeof(changes), FALSE,
        FILE_NOTIFY_CHANGE_FILE_NAME, nullptr, &request, nullptr));
    const std::wstring childPath = watched.path + L"\\notification.txt";
    Handle child;
    child.value = CreateFileW(childPath.c_str(), GENERIC_WRITE, ShareAll, nullptr, CREATE_NEW, 0, nullptr);
    REQUIRE(child.value != INVALID_HANDLE_VALUE);
    child.Reset();
    const BOOL gotChange = GetQueuedCompletionStatus(alias.value, &bytes, &key, &completed, 5000);
    DeleteFileW(childPath.c_str());
    REQUIRE(gotChange && bytes > 0 && key == 900 && completed == &request && request.Internal == 0);
    REQUIRE(reinterpret_cast<FILE_NOTIFY_INFORMATION*>(changes)->Action == FILE_ACTION_ADDED);
    return 0;
}

struct NativeDiskStatus
{
    ULONG_PTR status = 0;
    ULONG_PTR information = 0;
};

struct NativeDiskApcResult
{
    NativeDiskStatus* received = nullptr;
    DWORD thread = 0;
    ULONG reserved = MAXDWORD;
};

void NTAPI RecordNativeDiskApc(void* context, NativeDiskStatus* status, ULONG reserved)
{
    const auto result = static_cast<NativeDiskApcResult*>(context);
    result->received = status;
    result->reserved = reserved;
    result->thread = GetCurrentThreadId();
}

int CheckNativeDiskIo(const wchar_t* temporaryPath)
{
    using Apc = void (NTAPI*)(void*, NativeDiskStatus*, ULONG);
    using Read = LONG (NTAPI*)(HANDLE, HANDLE, Apc, void*, NativeDiskStatus*,
        void*, ULONG, const LARGE_INTEGER*, const ULONG*);
    using Write = LONG (NTAPI*)(HANDLE, HANDLE, Apc, void*, NativeDiskStatus*,
        const void*, ULONG, const LARGE_INTEGER*, const ULONG*);
    using Cancel = LONG (NTAPI*)(HANDLE, NativeDiskStatus*);
    using CancelEx = LONG (NTAPI*)(HANDLE, const NativeDiskStatus*, NativeDiskStatus*);
    const HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
    const auto read = reinterpret_cast<Read>(GetProcAddress(ntdll, "NtReadFile"));
    const auto write = reinterpret_cast<Write>(GetProcAddress(ntdll, "ZwWriteFile"));
    const auto cancel = reinterpret_cast<Cancel>(GetProcAddress(ntdll, "NtCancelIoFile"));
    const auto cancelEx = reinterpret_cast<CancelEx>(GetProcAddress(ntdll, "ZwCancelIoFileEx"));
    REQUIRE(read && write && cancel && cancelEx);

    constexpr LONG Pending = 0x103;
    constexpr LONG EndOfFile = static_cast<LONG>(0xc0000011u);
    constexpr DWORD ShareAll = FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE;
    TemporaryFile scratch;
    REQUIRE(GetTempFileNameW(temporaryPath, L"bnd", 0, scratch.path));
    Handle file, event, port;
    file.value = CreateFileW(scratch.path, GENERIC_READ | GENERIC_WRITE, ShareAll, nullptr, OPEN_EXISTING, 0, nullptr);
    REQUIRE(file.value != INVALID_HANDLE_VALUE);
    NativeDiskStatus io{};
    SetLastError(ERROR_ALREADY_EXISTS);
    REQUIRE(write(file.value, nullptr, nullptr, nullptr, &io, "abcdef", 6, nullptr, nullptr) == 0);
    REQUIRE(io.status == 0 && io.information == 6 && GetLastError() == ERROR_ALREADY_EXISTS);
    LARGE_INTEGER position{}, offset{};
    offset.QuadPart = 2;
    char data[6]{};
    REQUIRE(read(file.value, nullptr, nullptr, nullptr, &io, data, 2, &offset, nullptr) == 0);
    REQUIRE(io.information == 2 && memcmp(data, "cd", 2) == 0);
    REQUIRE(SetFilePointerEx(file.value, {}, &position, FILE_CURRENT) && position.QuadPart == 4);
    REQUIRE(read(file.value, nullptr, nullptr, nullptr, &io, data, 2, nullptr, nullptr) == 0);
    REQUIRE(io.information == 2 && memcmp(data, "ef", 2) == 0);
    REQUIRE(read(file.value, nullptr, nullptr, nullptr, &io, data, 1, nullptr, nullptr) == EndOfFile);
    REQUIRE(static_cast<LONG>(io.status) == EndOfFile && !io.information);
    file.Reset();

    file.value = CreateFileW(scratch.path, FILE_APPEND_DATA, ShareAll, nullptr, OPEN_EXISTING, 0, nullptr);
    REQUIRE(file.value != INVALID_HANDLE_VALUE);
    offset.QuadPart = 0;
    REQUIRE(write(file.value, nullptr, nullptr, nullptr, &io, "!", 1, &offset, nullptr) == 0 && io.information == 1);
    file.Reset();

    file.value = CreateFileW(scratch.path, GENERIC_READ | GENERIC_WRITE, ShareAll, nullptr, OPEN_EXISTING,
        FILE_FLAG_OVERLAPPED, nullptr);
    event.value = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    REQUIRE(file.value != INVALID_HANDLE_VALUE && event.value);
    REQUIRE(read(file.value, nullptr, nullptr, nullptr, &io, data, 1, nullptr, nullptr) == static_cast<LONG>(0xc000000du));
    NativeDiskApcResult apc;
    offset.QuadPart = 6;
    const LONG apcStatus = read(file.value, event.value, RecordNativeDiskApc, &apc, &io, data, 1, &offset, nullptr);
    REQUIRE(apcStatus == 0 || apcStatus == Pending);
    if (apcStatus == Pending)
    {
        REQUIRE(apc.received == nullptr && SleepEx(5000, TRUE) == WAIT_IO_COMPLETION);
        REQUIRE(apc.received == &io && apc.reserved == 0 && apc.thread == GetCurrentThreadId());
    }
    REQUIRE(WaitForSingleObject(event.value, 5000) == WAIT_OBJECT_0);
    REQUIRE(io.status == 0 && io.information == 1 && data[0] == '!');

    port.value = CreateIoCompletionPort(file.value, nullptr, 444, 1);
    REQUIRE(port.value);
    ULONG_PTR opaqueContext = 0x1122334455667788ull;
    io = {0x12345678u, 0x87654321u};
    offset.QuadPart = 0;
    const LONG queued = read(file.value, nullptr, nullptr, &opaqueContext, &io, data, 2, &offset, nullptr);
    REQUIRE(queued == 0 || queued == Pending);
    REQUIRE(WaitForSingleObject(port.value, 5000) == WAIT_OBJECT_0);
    if (queued == Pending) REQUIRE(io.status == 0x12345678u && io.information == 0x87654321u);
    DWORD bytes = 0;
    ULONG_PTR key = 0;
    LPOVERLAPPED context = nullptr;
    REQUIRE(GetQueuedCompletionStatus(port.value, &bytes, &key, &context, 5000));
    REQUIRE(key == 444 && bytes == 2 && static_cast<void*>(context) == &opaqueContext &&
        opaqueContext == 0x1122334455667788ull && io.status == 0 && io.information == 2);

    const HANDLE tagged = reinterpret_cast<HANDLE>(reinterpret_cast<ULONG_PTR>(event.value) | 1);
    const LONG explicitEvent = read(file.value, tagged, nullptr, &opaqueContext, &io, data, 2, &offset, nullptr);
    REQUIRE(explicitEvent == 0 || explicitEvent == Pending);
    REQUIRE(WaitForSingleObject(event.value, 5000) == WAIT_OBJECT_0);
    REQUIRE(io.status == 0 && io.information == 2);
    REQUIRE(GetQueuedCompletionStatus(port.value, &bytes, &key, &context, 5000) &&
        key == 444 && bytes == 2 && static_cast<void*>(context) == &opaqueContext);

    const LONG withoutContext = read(file.value, event.value, nullptr, nullptr, &io, data, 2, &offset, nullptr);
    REQUIRE(withoutContext == 0 || withoutContext == Pending);
    REQUIRE(WaitForSingleObject(event.value, 5000) == WAIT_OBJECT_0);
    REQUIRE(!GetQueuedCompletionStatus(port.value, &bytes, &key, &context, 0) && GetLastError() == WAIT_TIMEOUT);
    NativeDiskStatus cancelStatus{44, 55};
    REQUIRE(cancelEx(file.value, &io, &cancelStatus) == static_cast<LONG>(0xc0000225u));
    REQUIRE(static_cast<LONG>(cancelStatus.status) == static_cast<LONG>(0xc0000225u) && !cancelStatus.information);
    REQUIRE(cancel(file.value, &cancelStatus) == 0 && !cancelStatus.status && !cancelStatus.information);
    return 0;
}

int CheckByteRangeLocks(const wchar_t* temporaryPath)
{
    constexpr DWORD ShareAll = FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE;
    TemporaryFile scratch;
    REQUIRE(GetTempFileNameW(temporaryPath, L"lck", 0, scratch.path));
    const auto open = [&](DWORD flags)
    { return CreateFileW(scratch.path, GENERIC_READ | GENERIC_WRITE, ShareAll, nullptr, OPEN_EXISTING, flags, nullptr); };
    Handle original, peer, alias;
    original.value = open(0);
    peer.value = open(0);
    REQUIRE(original.value != INVALID_HANDLE_VALUE && peer.value != INVALID_HANDLE_VALUE);
    DWORD count = 0;
    REQUIRE(WriteFile(original.value, "0123456789abcdef", 16, &count, nullptr) && count == 16);
    REQUIRE(LockFile(original.value, 2, 0, 4, 0));
    REQUIRE(DuplicateHandle(GetCurrentProcess(), original.value, GetCurrentProcess(), &alias.value,
        0, FALSE, DUPLICATE_SAME_ACCESS));
    LARGE_INTEGER offset{};
    offset.QuadPart = 2;
    char data[2]{};
    REQUIRE(SetFilePointerEx(alias.value, offset, nullptr, FILE_BEGIN));
    REQUIRE(ReadFile(alias.value, data, 2, &count, nullptr) && count == 2 && memcmp(data, "23", 2) == 0);
    REQUIRE(SetFilePointerEx(alias.value, offset, nullptr, FILE_BEGIN));
    REQUIRE(WriteFile(alias.value, "XY", 2, &count, nullptr) && count == 2);
    REQUIRE(SetFilePointerEx(peer.value, offset, nullptr, FILE_BEGIN));
    REQUIRE(!ReadFile(peer.value, data, 2, &count, nullptr) && GetLastError() == ERROR_LOCK_VIOLATION);
    REQUIRE(ReadFile(peer.value, data, 0, &count, nullptr) && count == 0);
    REQUIRE(!LockFile(original.value, 2, 0, 4, 0) && GetLastError() == ERROR_LOCK_VIOLATION);
    original.Reset();
    REQUIRE(SetFilePointerEx(peer.value, offset, nullptr, FILE_BEGIN));
    REQUIRE(!ReadFile(peer.value, data, 2, &count, nullptr) && GetLastError() == ERROR_LOCK_VIOLATION);
    REQUIRE(UnlockFile(alias.value, 2, 0, 4, 0));
    REQUIRE(ReadFile(peer.value, data, 2, &count, nullptr) && count == 2 && memcmp(data, "XY", 2) == 0);

    OVERLAPPED shared{};
    shared.Offset = 2;
    REQUIRE(LockFileEx(alias.value, LOCKFILE_FAIL_IMMEDIATELY, 0, 4, 0, &shared));
    REQUIRE(LockFileEx(peer.value, LOCKFILE_FAIL_IMMEDIATELY, 0, 4, 0, &shared));
    REQUIRE(SetFilePointerEx(peer.value, offset, nullptr, FILE_BEGIN));
    REQUIRE(ReadFile(peer.value, data, 2, &count, nullptr) && count == 2);
    REQUIRE(SetFilePointerEx(alias.value, offset, nullptr, FILE_BEGIN));
    REQUIRE(!WriteFile(alias.value, "23", 2, &count, nullptr) && GetLastError() == ERROR_LOCK_VIOLATION);
    REQUIRE(UnlockFile(alias.value, 2, 0, 4, 0) && UnlockFile(peer.value, 2, 0, 4, 0));

    using Apc = void (NTAPI*)(void*, NativeDiskStatus*, ULONG);
    using Lock = LONG (NTAPI*)(HANDLE, HANDLE, Apc, void*, NativeDiskStatus*,
        const LARGE_INTEGER*, const LARGE_INTEGER*, ULONG, BOOLEAN, BOOLEAN);
    using Unlock = LONG (NTAPI*)(HANDLE, NativeDiskStatus*, const LARGE_INTEGER*, const LARGE_INTEGER*, ULONG);
    using Read = LONG (NTAPI*)(HANDLE, HANDLE, Apc, void*, NativeDiskStatus*,
        void*, ULONG, const LARGE_INTEGER*, const ULONG*);
    const HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
    const auto lock = reinterpret_cast<Lock>(GetProcAddress(ntdll, "NtLockFile"));
    const auto unlock = reinterpret_cast<Unlock>(GetProcAddress(ntdll, "ZwUnlockFile"));
    const auto read = reinterpret_cast<Read>(GetProcAddress(ntdll, "NtReadFile"));
    REQUIRE(lock && unlock && read);
    NativeDiskStatus status{};
    LARGE_INTEGER length{};
    length.QuadPart = 4;
    constexpr ULONG Key = 17, WrongKey = 0;
    REQUIRE(lock(alias.value, nullptr, nullptr, nullptr, &status, &offset, &length, Key, TRUE, TRUE) == 0);
    REQUIRE(read(alias.value, nullptr, nullptr, nullptr, &status, data, 2, &offset, &WrongKey) ==
        static_cast<LONG>(0xc0000054u));
    REQUIRE(read(alias.value, nullptr, nullptr, nullptr, &status, data, 2, &offset, &Key) == 0);
    REQUIRE(unlock(alias.value, &status, &offset, &length, WrongKey) == static_cast<LONG>(0xc000007eu));
    REQUIRE(unlock(alias.value, &status, &offset, &length, Key) == 0);

    Handle asynchronous, event;
    asynchronous.value = open(FILE_FLAG_OVERLAPPED);
    event.value = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    REQUIRE(asynchronous.value != INVALID_HANDLE_VALUE && event.value);
    OVERLAPPED immediate{};
    immediate.Offset = 8;
    immediate.hEvent = event.value;
    REQUIRE(LockFileEx(asynchronous.value, LOCKFILE_EXCLUSIVE_LOCK, 0, 2, 0, &immediate));
    REQUIRE(WaitForSingleObject(event.value, 0) == WAIT_OBJECT_0);
    REQUIRE(GetOverlappedResult(asynchronous.value, &immediate, &count, FALSE) && count == 0);
    REQUIRE(UnlockFileEx(asynchronous.value, 0, 2, 0, &immediate));
    for (const bool cancel : {false, true})
    {
        REQUIRE(LockFile(alias.value, 8, 0, 2, 0));
        OVERLAPPED pending{};
        pending.Offset = 8;
        pending.hEvent = event.value;
        REQUIRE(!LockFileEx(asynchronous.value, LOCKFILE_EXCLUSIVE_LOCK, 0, 2, 0, &pending) &&
            GetLastError() == ERROR_IO_PENDING);
        REQUIRE(WaitForSingleObject(event.value, 0) == WAIT_TIMEOUT);
        if (cancel) { REQUIRE(CancelIoEx(asynchronous.value, &pending)); }
        else { REQUIRE(UnlockFile(alias.value, 8, 0, 2, 0)); }
        REQUIRE(WaitForSingleObject(event.value, 5000) == WAIT_OBJECT_0);
        const BOOL completed = GetOverlappedResult(asynchronous.value, &pending, &count, FALSE);
        if (cancel)
        {
            REQUIRE(!completed && GetLastError() == ERROR_OPERATION_ABORTED);
            REQUIRE(UnlockFile(alias.value, 8, 0, 2, 0));
        }
        else
        {
            REQUIRE(completed && UnlockFile(asynchronous.value, 8, 0, 2, 0));
        }
    }
    REQUIRE(LockFile(alias.value, 2, 0, 4, 0));
    alias.Reset();
    REQUIRE(SetFilePointerEx(peer.value, offset, nullptr, FILE_BEGIN));
    REQUIRE(ReadFile(peer.value, data, 2, &count, nullptr) && count == 2);

    Handle append;
    append.value = CreateFileW(scratch.path, FILE_APPEND_DATA, ShareAll, nullptr, OPEN_EXISTING, 0, nullptr);
    REQUIRE(append.value != INVALID_HANDLE_VALUE);
    REQUIRE(SetFilePointerEx(append.value, offset, nullptr, FILE_BEGIN));
    REQUIRE(LockFile(peer.value, 16, 0, 1, 0));
    REQUIRE(!WriteFile(append.value, "!", 1, &count, nullptr) && GetLastError() == ERROR_LOCK_VIOLATION);
    REQUIRE(UnlockFile(peer.value, 16, 0, 1, 0));
    REQUIRE(WriteFile(append.value, "!", 1, &count, nullptr) && count == 1);
    LARGE_INTEGER position{};
    REQUIRE(SetFilePointerEx(append.value, {}, &position, FILE_CURRENT) && position.QuadPart == 17);
    position.QuadPart = 16;
    REQUIRE(SetFilePointerEx(peer.value, position, nullptr, FILE_BEGIN));
    REQUIRE(ReadFile(peer.value, data, 1, &count, nullptr) && count == 1 && data[0] == '!');
    return 0;
}

int CheckRenameOpenFile(const wchar_t* temporaryPath)
{
    constexpr DWORD ShareAll = FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE;
    TemporaryFile source, destination;
    REQUIRE(GetTempFileNameW(temporaryPath, L"rns", 0, source.path));
    REQUIRE(GetTempFileNameW(temporaryPath, L"rnd", 0, destination.path));
    Handle original, alias, replaced, reopened, oldName;
    original.value = CreateFileW(source.path, GENERIC_READ | GENERIC_WRITE,
        FILE_SHARE_READ | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, 0, nullptr);
    REQUIRE(original.value != INVALID_HANDLE_VALUE);
    DWORD count = 0;
    REQUIRE(WriteFile(original.value, "source", 6, &count, nullptr) && count == 6);
    REQUIRE(DuplicateHandle(GetCurrentProcess(), original.value, GetCurrentProcess(), &alias.value,
        0, FALSE, DUPLICATE_SAME_ACCESS));
    REQUIRE(LockFile(original.value, 0, 0, 2, 0));

    replaced.value = CreateFileW(destination.path, 0, ShareAll, nullptr, OPEN_EXISTING, 0, nullptr);
    REQUIRE(replaced.value != INVALID_HANDLE_VALUE);
    REQUIRE(!MoveFileExW(source.path, destination.path, MOVEFILE_REPLACE_EXISTING) &&
        GetLastError() == ERROR_ACCESS_DENIED);
    replaced.Reset();
    REQUIRE(MoveFileExW(source.path, destination.path, MOVEFILE_REPLACE_EXISTING));

    wchar_t finalPath[MAX_PATH]{};
    const DWORD pathLength = GetFinalPathNameByHandleW(alias.value, finalPath, MAX_PATH, 0);
    const wchar_t* finalName = wcsrchr(finalPath, L'\\');
    const wchar_t* destinationName = wcsrchr(destination.path, L'\\');
    REQUIRE(pathLength > 0 && pathLength < MAX_PATH && finalName && destinationName &&
        _wcsicmp(finalName, destinationName) == 0);
    reopened.value = CreateFileW(destination.path, GENERIC_WRITE, ShareAll, nullptr, OPEN_EXISTING, 0, nullptr);
    REQUIRE(reopened.value == INVALID_HANDLE_VALUE && GetLastError() == ERROR_SHARING_VIOLATION);
    reopened.value = CreateFileW(destination.path, GENERIC_READ, ShareAll, nullptr, OPEN_EXISTING, 0, nullptr);
    REQUIRE(reopened.value != INVALID_HANDLE_VALUE);
    char data[2]{};
    REQUIRE(!ReadFile(reopened.value, data, sizeof(data), &count, nullptr) && GetLastError() == ERROR_LOCK_VIOLATION);

    oldName.value = CreateFileW(source.path, GENERIC_READ | GENERIC_WRITE, ShareAll, nullptr, CREATE_NEW, 0, nullptr);
    REQUIRE(oldName.value != INVALID_HANDLE_VALUE);
    REQUIRE(WriteFile(oldName.value, "new", 3, &count, nullptr) && count == 3);
    REQUIRE(LockFile(oldName.value, 0, 0, 2, 0));
    REQUIRE(UnlockFile(oldName.value, 0, 0, 2, 0));
    REQUIRE(UnlockFile(alias.value, 0, 0, 2, 0));
    REQUIRE(ReadFile(reopened.value, data, sizeof(data), &count, nullptr) &&
        count == sizeof(data) && memcmp(data, "so", sizeof(data)) == 0);

    // CREATE_ALWAYS truncates the current file, so descriptions opened before
    // it still see the same file contents and locks.
    REQUIRE(LockFile(oldName.value, 0, 0, 2, 0));
    Handle truncate;
    truncate.value = CreateFileW(source.path, GENERIC_WRITE, ShareAll, nullptr, CREATE_ALWAYS, 0, nullptr);
    REQUIRE(truncate.value != INVALID_HANDLE_VALUE && GetLastError() == ERROR_ALREADY_EXISTS);
    LARGE_INTEGER size{};
    REQUIRE(GetFileSizeEx(oldName.value, &size) && size.QuadPart == 0);
    REQUIRE(!WriteFile(truncate.value, "n", 1, &count, nullptr) && GetLastError() == ERROR_LOCK_VIOLATION);
    REQUIRE(UnlockFile(oldName.value, 0, 0, 2, 0));
    REQUIRE(WriteFile(truncate.value, "n", 1, &count, nullptr) && count == 1);
    REQUIRE(GetFileSizeEx(oldName.value, &size) && size.QuadPart == 1);
    return 0;
}

int CheckFileMappings(const wchar_t* temporaryPath)
{
    constexpr DWORD ShareAll = FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE;
    TemporaryFile source, renamed;
    REQUIRE(GetTempFileNameW(temporaryPath, L"mps", 0, source.path));
    REQUIRE(GetTempFileNameW(temporaryPath, L"mpd", 0, renamed.path));
    REQUIRE(DeleteFileW(renamed.path));
    Handle file, section;
    file.value = CreateFileW(source.path, GENERIC_READ | GENERIC_WRITE, ShareAll, nullptr, OPEN_EXISTING, 0, nullptr);
    REQUIRE(file.value != INVALID_HANDLE_VALUE);
    DWORD bytes = 0;
    REQUIRE(WriteFile(file.value, "01234567", 8, &bytes, nullptr) && bytes == 8);
    section.value = CreateFileMappingW(file.value, nullptr, PAGE_READWRITE, 0, 32, nullptr);
    REQUIRE(section.value);
    LARGE_INTEGER position{};
    REQUIRE(SetFilePointerEx(file.value, {}, &position, FILE_CURRENT) && position.QuadPart == 8);
    REQUIRE(GetFileSizeEx(file.value, &position) && position.QuadPart == 32);
    View read, write, privateCopy;
    read.bytes = static_cast<char*>(MapViewOfFile(section.value, FILE_MAP_READ, 0, 0, 0));
    write.bytes = static_cast<char*>(MapViewOfFile(section.value, FILE_MAP_ALL_ACCESS, 0, 0, 0));
    privateCopy.bytes = static_cast<char*>(MapViewOfFile(section.value, FILE_MAP_COPY, 0, 0, 0));
    REQUIRE(read.bytes && write.bytes && privateCopy.bytes);
    REQUIRE(read.bytes != write.bytes && privateCopy.bytes != write.bytes);
    section.Reset();
    file.Reset();
    REQUIRE(MoveFileExW(source.path, renamed.path, 0));
    file.value = CreateFileW(source.path, GENERIC_READ | GENERIC_WRITE, ShareAll, nullptr, CREATE_NEW, 0, nullptr);
    REQUIRE(file.value != INVALID_HANDLE_VALUE);
    REQUIRE(WriteFile(file.value, "new", 3, &bytes, nullptr) && bytes == 3);

    privateCopy.bytes[1] = 'X';
    REQUIRE(FlushViewOfFile(privateCopy.bytes + 1, 1) && read.bytes[1] == '1');
    write.bytes[3] = 'Z';
    REQUIRE(read.bytes[3] == 'Z' && FlushViewOfFile(write.bytes + 3, 1));
    char contents[8]{};
    Handle reader;
    reader.value = CreateFileW(renamed.path, GENERIC_READ, ShareAll, nullptr, OPEN_EXISTING, 0, nullptr);
    REQUIRE(reader.value != INVALID_HANDLE_VALUE);
    REQUIRE(ReadFile(reader.value, contents, sizeof(contents), &bytes, nullptr) && bytes == sizeof(contents));
    REQUIRE(memcmp(contents, "012Z4567", sizeof(contents)) == 0);
    REQUIRE(SetFilePointerEx(file.value, {}, nullptr, FILE_BEGIN));
    REQUIRE(ReadFile(file.value, contents, 3, &bytes, nullptr) && bytes == 3 && memcmp(contents, "new", 3) == 0);

    wchar_t sectionName[64]{};
    swprintf_s(sectionName, L"Win32BridgeFileHandleContracts-%lu", GetCurrentProcessId());
    section.value = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0, 4096, sectionName);
    REQUIRE(section.value && GetLastError() != ERROR_ALREADY_EXISTS);
    Handle readOnly, duplicate;
    readOnly.value = OpenFileMappingW(FILE_MAP_READ, FALSE, sectionName);
    REQUIRE(readOnly.value != nullptr);
    REQUIRE(!MapViewOfFile(readOnly.value, FILE_MAP_WRITE, 0, 0, 0) && GetLastError() == ERROR_ACCESS_DENIED);
    REQUIRE(DuplicateHandle(GetCurrentProcess(), readOnly.value, GetCurrentProcess(), &duplicate.value,
        0, FALSE, DUPLICATE_SAME_ACCESS));
    readOnly.Reset();
    REQUIRE(!MapViewOfFile(duplicate.value, FILE_MAP_WRITE, 0, 0, 0) && GetLastError() == ERROR_ACCESS_DENIED);
    View namedRead, namedWrite;
    namedRead.bytes = static_cast<char*>(MapViewOfFile(duplicate.value, FILE_MAP_READ, 0, 0, 0));
    namedWrite.bytes = static_cast<char*>(MapViewOfFile(section.value, FILE_MAP_WRITE, 0, 0, 0));
    REQUIRE(namedRead.bytes && namedWrite.bytes);
    namedWrite.bytes[9] = 'M';
    REQUIRE(namedRead.bytes[9] == 'M');
    return 0;
}

int CheckNativeIoCompletion(const wchar_t* temporaryPath)
{
    struct UnicodeString { USHORT length, maximum; LPCWSTR buffer; };
    struct ObjectAttributes
    {
        ULONG length;
        HANDLE root;
        UnicodeString* name;
        ULONG flags;
        void* security;
        void* qos;
    };
    struct IoStatus { ULONG_PTR status, information; };
    struct Entry { ULONG_PTR key; void* context; IoStatus io; };
    struct Binding { HANDLE port; ULONG_PTR key; };
    static_assert(sizeof(Entry) == 32 && sizeof(Binding) == 16, "I/O completion x64 ABI");

    const HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
    using Create = LONG (WINAPI*)(HANDLE*, DWORD, const ObjectAttributes*, ULONG);
    using Query = LONG (WINAPI*)(HANDLE, ULONG, void*, ULONG, ULONG*);
    using Post = LONG (WINAPI*)(HANDLE, void*, void*, LONG, ULONG_PTR);
    using Remove = LONG (WINAPI*)(HANDLE, void**, void**, IoStatus*, const LARGE_INTEGER*);
    using RemoveEx = LONG (WINAPI*)(HANDLE, Entry*, ULONG, ULONG*, const LARGE_INTEGER*, BOOLEAN);
    using OpenFile = LONG (WINAPI*)(HANDLE*, DWORD, const ObjectAttributes*, IoStatus*, ULONG, ULONG);
    using FileInformation = LONG (WINAPI*)(HANDLE, IoStatus*, void*, ULONG, ULONG);
    const auto create = reinterpret_cast<Create>(GetProcAddress(ntdll, "NtCreateIoCompletion"));
    const auto query = reinterpret_cast<Query>(GetProcAddress(ntdll, "ZwQueryIoCompletion"));
    const auto post = reinterpret_cast<Post>(GetProcAddress(ntdll, "NtSetIoCompletion"));
    const auto remove = reinterpret_cast<Remove>(GetProcAddress(ntdll, "ZwRemoveIoCompletion"));
    const auto removeEx = reinterpret_cast<RemoveEx>(GetProcAddress(ntdll, "NtRemoveIoCompletionEx"));
    const auto openFile = reinterpret_cast<OpenFile>(GetProcAddress(ntdll, "NtOpenFile"));
    const auto setFile = reinterpret_cast<FileInformation>(GetProcAddress(ntdll, "ZwSetInformationFile"));
    const auto queryFile = reinterpret_cast<FileInformation>(GetProcAddress(ntdll, "NtQueryInformationFile"));
    REQUIRE(create && query && post && remove && removeEx && openFile && setFile && queryFile);

    constexpr DWORD QueryState = 1, ModifyState = 2;
    constexpr ULONG_PTR WideInformation = 0x100000007ull;
    constexpr LONG Cancelled = static_cast<LONG>(0xc0000120u);
    Handle port, queryOnly, wrongType, thread;
    REQUIRE(create(&port.value, QueryState | ModifyState | SYNCHRONIZE, nullptr, 1) == 0 && port.value);
    REQUIRE(DuplicateHandle(GetCurrentProcess(), port.value, GetCurrentProcess(), &queryOnly.value, QueryState,
        FALSE, 0));
    REQUIRE(post(queryOnly.value, nullptr, nullptr, 0, 0) == static_cast<LONG>(0xc0000022u));
    REQUIRE(WaitForSingleObject(queryOnly.value, 0) == WAIT_FAILED && GetLastError() == ERROR_ACCESS_DENIED);
    wrongType.value = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    REQUIRE(wrongType.value && post(wrongType.value, nullptr, nullptr, 0, 0) == static_cast<LONG>(0xc0000024u));

    LONG depth = -1;
    ULONG returned = 99;
    ULONG_PTR contextStorage = 42;
    const auto key = reinterpret_cast<void*>(static_cast<ULONG_PTR>(700));
    const auto context = static_cast<void*>(&contextStorage);
    SetLastError(ERROR_ALREADY_EXISTS);
    REQUIRE(query(port.value, 0, &depth, sizeof(depth) + 1, &returned) == static_cast<LONG>(0xc0000004u));
    REQUIRE(depth == -1 && returned == 99);
    REQUIRE(post(port.value, key, context, Cancelled, WideInformation) == 0);
    REQUIRE(query(queryOnly.value, 0, &depth, sizeof(depth), &returned) == 0 &&
        depth == 1 && returned == sizeof(depth) && GetLastError() == ERROR_ALREADY_EXISTS);

    LARGE_INTEGER poll{};
    void* outKey = nullptr;
    void* outContext = nullptr;
    IoStatus io{};
    REQUIRE(remove(port.value, &outKey, &outContext, &io, &poll) == 0);
    REQUIRE(outKey == key && outContext == context && static_cast<LONG>(io.status) == Cancelled &&
        io.information == WideInformation);
    REQUIRE(remove(port.value, &outKey, &outContext, &io, &poll) == WAIT_TIMEOUT);
    REQUIRE(outKey == key && outContext == context && io.information == WideInformation);

    REQUIRE(post(port.value, key, context, Cancelled, WideInformation) == 0);
    DWORD bytes = 0;
    ULONG_PTR winKey = 0;
    LPOVERLAPPED completed = nullptr;
    REQUIRE(!GetQueuedCompletionStatus(port.value, &bytes, &winKey, &completed, 0));
    REQUIRE(GetLastError() == ERROR_OPERATION_ABORTED && bytes == 7 &&
        winKey == reinterpret_cast<ULONG_PTR>(key) && completed == context);

    constexpr ULONG Count = 70;
    Entry entries[Count]{};
    for (ULONG index = 0; index < Count; ++index)
        REQUIRE(PostQueuedCompletionStatus(port.value, index, index + 1, nullptr));
    REQUIRE(removeEx(port.value, entries, Count, &returned, &poll, FALSE) == 0 && returned == Count);
    for (ULONG index = 0; index < Count; ++index)
        REQUIRE(entries[index].key == index + 1 && !entries[index].context &&
            entries[index].io.status == 0 && entries[index].io.information == index);
    thread.value = OpenThread(THREAD_SET_CONTEXT, FALSE, GetCurrentThreadId());
    bool ran = false;
    REQUIRE(thread.value && QueueUserAPC(MarkHandoffApc, thread.value, reinterpret_cast<ULONG_PTR>(&ran)));
    LARGE_INTEGER callbackDeadline{};
    callbackDeadline.QuadPart = -50'000'000;
    REQUIRE(removeEx(port.value, entries, 1, &returned, &callbackDeadline, TRUE) == WAIT_IO_COMPLETION &&
        returned == 0 && ran);

    TemporaryFile scratch;
    REQUIRE(GetTempFileNameW(temporaryPath, L"bnt", 0, scratch.path));
    const std::wstring path = std::wstring(L"\\??\\") + scratch.path;
    const auto pathLength = static_cast<USHORT>(path.size() * sizeof(wchar_t));
    UnicodeString name{pathLength, pathLength, path.c_str()};
    const ObjectAttributes attributes{sizeof(ObjectAttributes), nullptr, &name, 0x40, nullptr, nullptr};
    Handle file;
    REQUIRE(openFile(&file.value, GENERIC_READ | GENERIC_WRITE | SYNCHRONIZE, &attributes, &io,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, 0x40) == 0 && file.value);
    Binding binding{queryOnly.value, 333};
    REQUIRE(setFile(file.value, &io, &binding, sizeof(binding), 30) == static_cast<LONG>(0xc0000022u));
    binding.port = port.value;
    REQUIRE(setFile(file.value, &io, &binding, sizeof(binding), 30) == 0);
    REQUIRE(setFile(file.value, &io, &binding, sizeof(binding), 30) == static_cast<LONG>(0xc000000du));
    ULONG modes = FILE_SKIP_SET_EVENT_ON_HANDLE | 0x4 | 0x8;
    REQUIRE(setFile(file.value, &io, &modes, sizeof(modes), 41) == 0);
    modes = 0;
    REQUIRE(queryFile(file.value, &io, &modes, sizeof(modes), 41) == 0 &&
        modes == (FILE_SKIP_SET_EVENT_ON_HANDLE | 0x4) && io.information == sizeof(modes));

    OVERLAPPED write{};
    REQUIRE(WriteFile(file.value, "x", 1, &bytes, &write) || GetLastError() == ERROR_IO_PENDING);
    REQUIRE(remove(port.value, &outKey, &outContext, &io, &callbackDeadline) == 0);
    REQUIRE(reinterpret_cast<ULONG_PTR>(outKey) == binding.key && outContext == &write &&
        io.status == 0 && io.information == 1 && write.Internal == 0);
    REQUIRE(WaitForSingleObject(file.value, 0) == WAIT_TIMEOUT);
    return 0;
}

int CheckSignalAndWait()
{
    Handle signal, target, restricted, semaphore, mutex, thread;
    signal.value = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    target.value = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    semaphore.value = CreateSemaphoreW(nullptr, 1, 1, nullptr);
    mutex.value = CreateMutexW(nullptr, FALSE, nullptr);
    REQUIRE(signal.value && target.value && semaphore.value && mutex.value);
    const HANDLE process = GetCurrentProcess();
    REQUIRE(DuplicateHandle(process, target.value, process, &restricted.value, EVENT_MODIFY_STATE, FALSE, 0));

    REQUIRE(SignalObjectAndWait(signal.value, nullptr, 0, FALSE) == WAIT_FAILED &&
        GetLastError() == ERROR_INVALID_HANDLE);
    REQUIRE(SignalObjectAndWait(signal.value, restricted.value, 0, FALSE) == WAIT_FAILED &&
        GetLastError() == ERROR_ACCESS_DENIED);
    REQUIRE(WaitForSingleObject(signal.value, 0) == WAIT_TIMEOUT);
    REQUIRE(SignalObjectAndWait(semaphore.value, signal.value, 0, FALSE) == WAIT_FAILED &&
        GetLastError() == ERROR_TOO_MANY_POSTS);
    REQUIRE(SignalObjectAndWait(mutex.value, signal.value, 0, FALSE) == WAIT_FAILED &&
        GetLastError() == ERROR_NOT_OWNER);
    REQUIRE(SignalObjectAndWait(signal.value, target.value, 0, FALSE) == WAIT_TIMEOUT);
    REQUIRE(WaitForSingleObject(signal.value, 0) == WAIT_OBJECT_0);
    REQUIRE(SignalObjectAndWait(target.value, target.value, 0, FALSE) == WAIT_OBJECT_0);
    REQUIRE(WaitForSingleObject(target.value, 0) == WAIT_TIMEOUT);

    thread.value = OpenThread(THREAD_SET_CONTEXT, FALSE, GetCurrentThreadId());
    REQUIRE(thread.value);
    bool ran = false;
    REQUIRE(QueueUserAPC(MarkHandoffApc, thread.value, reinterpret_cast<ULONG_PTR>(&ran)));
    REQUIRE(SignalObjectAndWait(target.value, target.value, 0, TRUE) == WAIT_IO_COMPLETION);
    REQUIRE(ran && WaitForSingleObject(target.value, 0) == WAIT_OBJECT_0);

    const HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
    using NtWait = LONG (WINAPI*)(HANDLE, BOOLEAN, const LARGE_INTEGER*);
    using NtWaitMultiple = LONG (WINAPI*)(ULONG, const HANDLE*, ULONG, BOOLEAN, const LARGE_INTEGER*);
    using NtSignalWait = LONG (WINAPI*)(HANDLE, HANDLE, BOOLEAN, const LARGE_INTEGER*);
    const auto ntWait = reinterpret_cast<NtWait>(GetProcAddress(ntdll, "NtWaitForSingleObject"));
    const auto ntWaitMultiple = reinterpret_cast<NtWaitMultiple>(GetProcAddress(ntdll, "NtWaitForMultipleObjects"));
    const auto ntSignalWait = reinterpret_cast<NtSignalWait>(GetProcAddress(ntdll, "NtSignalAndWaitForSingleObject"));
    REQUIRE(ntWait && ntWaitMultiple && ntSignalWait);
    LARGE_INTEGER immediate{}, relative{}, expired{};
    relative.QuadPart = -10000; // One millisecond in NT 100 ns units.
    expired.QuadPart = 1;
    SetLastError(ERROR_ALREADY_EXISTS);
    REQUIRE(ntWait(target.value, FALSE, &relative) == WAIT_TIMEOUT && GetLastError() == ERROR_ALREADY_EXISTS);
    REQUIRE(ntWait(target.value, FALSE, &expired) == WAIT_TIMEOUT);
    const HANDLE aliases[] = {target.value, target.value};
    REQUIRE(static_cast<ULONG>(ntWaitMultiple(2, aliases, 0, FALSE, &immediate)) == 0xc0000030u);
    REQUIRE(static_cast<ULONG>(ntSignalWait(semaphore.value, target.value, FALSE, &immediate)) == 0xc0000047u);
    REQUIRE(ntSignalWait(target.value, target.value, FALSE, &immediate) == WAIT_OBJECT_0);
    return 0;
}

int CheckMultipleWaits()
{
    Handle event, semaphore, duplicate, mutex, owner;
    event.value = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    semaphore.value = CreateSemaphoreW(nullptr, 1, 1, nullptr);
    REQUIRE(event.value && semaphore.value);
    const HANDLE objects[] = { semaphore.value, event.value };
    REQUIRE(WaitForMultipleObjects(2, objects, TRUE, 0) == WAIT_TIMEOUT);
    REQUIRE(WaitForSingleObject(semaphore.value, 0) == WAIT_OBJECT_0);
    REQUIRE(ReleaseSemaphore(semaphore.value, 1, nullptr) && SetEvent(event.value));
    REQUIRE(WaitForMultipleObjects(2, objects, FALSE, 0) == WAIT_OBJECT_0);
    REQUIRE(ReleaseSemaphore(semaphore.value, 1, nullptr));
    REQUIRE(WaitForMultipleObjects(2, objects, TRUE, 0) == WAIT_OBJECT_0);
    REQUIRE(WaitForSingleObject(event.value, 0) == WAIT_TIMEOUT && WaitForSingleObject(semaphore.value, 0) == WAIT_TIMEOUT);

    REQUIRE(SetEvent(event.value));
    REQUIRE(DuplicateHandle(GetCurrentProcess(), event.value, GetCurrentProcess(), &duplicate.value, 0, FALSE, DUPLICATE_SAME_ACCESS));
    const HANDLE aliases[] = { event.value, duplicate.value };
    REQUIRE(WaitForMultipleObjects(2, aliases, TRUE, 0) == WAIT_FAILED && GetLastError() == ERROR_INVALID_PARAMETER);
    const HANDLE invalid[] = { event.value, nullptr };
    REQUIRE(WaitForMultipleObjects(2, invalid, FALSE, 0) == WAIT_FAILED && GetLastError() == ERROR_INVALID_HANDLE);
    REQUIRE(WaitForSingleObject(event.value, 0) == WAIT_OBJECT_0);

    mutex.value = CreateMutexW(nullptr, FALSE, nullptr);
    REQUIRE(mutex.value);
    owner.value = CreateThread(nullptr, 0, OwnMutexAndExit, mutex.value, 0, nullptr);
    REQUIRE(owner.value && WaitForSingleObject(owner.value, 5000) == WAIT_OBJECT_0);
    DWORD code = 0;
    REQUIRE(GetExitCodeThread(owner.value, &code) && code == 0 && SetEvent(event.value));
    const HANDLE abandoned[] = { event.value, mutex.value };
    REQUIRE(WaitForMultipleObjects(2, abandoned, TRUE, 0) == WAIT_ABANDONED_0);
    REQUIRE(ReleaseMutex(mutex.value) && WaitForSingleObject(event.value, 0) == WAIT_TIMEOUT);

    Handle ready, waiter;
    ready.value = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    REQUIRE(ready.value && ReleaseSemaphore(semaphore.value, 1, nullptr));
    alertableInputs = {ready.value, semaphore.value, event.value, false};
    waiter.value = CreateThread(nullptr, 0, WaitForApc, &alertableInputs, 0, nullptr);
    REQUIRE(waiter.value && WaitForSingleObject(ready.value, 5000) == WAIT_OBJECT_0);
    const bool apcQueued = QueueUserAPC(MarkWaitApc, waiter.value, reinterpret_cast<ULONG_PTR>(&alertableInputs)) != 0;
    REQUIRE(WaitForSingleObject(waiter.value, 10000) == WAIT_OBJECT_0);
    REQUIRE(apcQueued && GetExitCodeThread(waiter.value, &code) && code == 0);
    REQUIRE(WaitForSingleObject(semaphore.value, 0) == WAIT_OBJECT_0);

    MSG message{};
    PeekMessageW(&message, nullptr, 0, 0, PM_NOREMOVE);
    constexpr DWORD messageFlags = MWMO_WAITALL | MWMO_INPUTAVAILABLE;
    REQUIRE(SetEvent(event.value));
    REQUIRE(MsgWaitForMultipleObjectsEx(1, &event.value, 0, QS_POSTMESSAGE, messageFlags) == WAIT_TIMEOUT);
    REQUIRE(WaitForSingleObject(event.value, 0) == WAIT_OBJECT_0);
    constexpr UINT testMessage = WM_APP + 2;
    REQUIRE(SetEvent(event.value) && PostThreadMessageW(GetCurrentThreadId(), testMessage, 0, 0));
    REQUIRE(MsgWaitForMultipleObjectsEx(1, &event.value, 0, QS_POSTMESSAGE, messageFlags) == WAIT_OBJECT_0);
    REQUIRE(PeekMessageW(&message, nullptr, testMessage, testMessage, PM_REMOVE));
    REQUIRE(WaitForSingleObject(event.value, 0) == WAIT_TIMEOUT);
    return 0;
}
}
int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR command, int)
{
    const wchar_t* argument = command ? std::wcsstr(command, L"--writer ") : nullptr;
    if (argument)
        return ChildWriter(reinterpret_cast<HANDLE>(static_cast<ULONG_PTR>(std::wcstoull(argument + 9, nullptr, 10))));
    argument = command ? std::wcsstr(command, L"--exit-main-thread ") : nullptr;
    if (argument)
    {
        exitInputs.mainThread = OpenThread(SYNCHRONIZE, FALSE, GetCurrentThreadId());
        REQUIRE(exitInputs.mainThread);
        exitInputs.gate = reinterpret_cast<HANDLE>(static_cast<ULONG_PTR>(std::wcstoull(argument + 19, nullptr, 10)));
        const HANDLE worker = CreateThread(nullptr, 0, OutliveMainThread, &exitInputs, 0, nullptr);
        REQUIRE(worker && CloseHandle(worker));
        ExitThread(13);
    }
    if (command && std::wcsstr(command, L"--exit-worker"))
    {
        const HANDLE worker = CreateThread(nullptr, 0, ExitFromWorker, nullptr, 0, nullptr);
        REQUIRE(worker && CloseHandle(worker));
        SleepEx(INFINITE, TRUE);
        return __LINE__;
    }
    wchar_t directory[MAX_PATH]{}, executable[MAX_PATH]{};
    REQUIRE(GetTempPathW(MAX_PATH, directory) > 0);
    TemporaryFile file;
    REQUIRE(GetTempFileNameW(directory, L"bfh", 0, file.path) != 0);
    SECURITY_ATTRIBUTES security{ sizeof(SECURITY_ATTRIBUTES), nullptr, TRUE };
    Handle original, duplicate;
    original.value = CreateFileW(file.path, GENERIC_READ | GENERIC_WRITE,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, &security, CREATE_ALWAYS, 0, nullptr);
    REQUIRE(original.value != INVALID_HANDLE_VALUE);
    DWORD done = 0, flags = 0;
    REQUIRE(GetHandleInformation(original.value, &flags) && (flags & HANDLE_FLAG_INHERIT));
    REQUIRE(WriteFile(original.value, "ab", 2, &done, nullptr) && done == 2);
    REQUIRE(DuplicateHandle(GetCurrentProcess(), original.value, GetCurrentProcess(),
        &duplicate.value, 0, TRUE, DUPLICATE_SAME_ACCESS));
    LARGE_INTEGER distance{}, position{};
    REQUIRE(SetFilePointerEx(duplicate.value, distance, &position, FILE_CURRENT) && position.QuadPart == 2);
    original.Reset();
    REQUIRE(GetModuleFileNameW(nullptr, executable, MAX_PATH) > 0);
    Handle gate, signal;
    gate.value = CreateEventW(&security, TRUE, FALSE, nullptr);
    REQUIRE(gate.value);
    REQUIRE(DuplicateHandle(GetCurrentProcess(), gate.value, GetCurrentProcess(), &signal.value, 0, FALSE, DUPLICATE_SAME_ACCESS));
    REQUIRE(GetHandleInformation(signal.value, &flags) && !(flags & HANDLE_FLAG_INHERIT));
    std::wstring childCommand = L"\"" + std::wstring(executable) + L"\" --writer " +
        std::to_wstring(reinterpret_cast<ULONG_PTR>(gate.value));
    STARTUPINFOW startup{}; startup.cb = sizeof(startup); startup.dwFlags = STARTF_USESTDHANDLES;
    startup.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
    startup.hStdOutput = startup.hStdError = duplicate.value;
    PROCESS_INFORMATION process{};
    REQUIRE(CreateProcessW(executable, &childCommand[0], nullptr, nullptr, TRUE, 0, nullptr, nullptr, &startup, &process));
    Handle processHandle, threadHandle, processCopy, threadCopy, reopenedProcess, reopenedThread;
    processHandle.value = process.hProcess;
    threadHandle.value = process.hThread;
    REQUIRE(DuplicateHandle(GetCurrentProcess(), processHandle.value, GetCurrentProcess(), &processCopy.value, 0, FALSE, DUPLICATE_SAME_ACCESS));
    REQUIRE(DuplicateHandle(GetCurrentProcess(), threadHandle.value, GetCurrentProcess(), &threadCopy.value, 0, FALSE, DUPLICATE_SAME_ACCESS));
    processHandle.Reset();
    threadHandle.Reset();
    REQUIRE(GetThreadId(threadCopy.value) == process.dwThreadId);
    threadCopy.Reset();
    reopenedProcess.value = OpenProcess(SYNCHRONIZE | PROCESS_QUERY_LIMITED_INFORMATION, FALSE, process.dwProcessId);
    reopenedThread.value = OpenThread(SYNCHRONIZE | THREAD_QUERY_LIMITED_INFORMATION, FALSE, process.dwThreadId);
    REQUIRE(reopenedProcess.value && reopenedThread.value);
    REQUIRE(GetProcessId(processCopy.value) == process.dwProcessId);
    DWORD exit = 0;
    REQUIRE(GetExitCodeProcess(processCopy.value, &exit) && exit == STILL_ACTIVE);
    REQUIRE(WaitForSingleObject(processCopy.value, 0) == WAIT_TIMEOUT);
    gate.Reset();
    REQUIRE(SetEvent(signal.value));
    signal.Reset();
    REQUIRE(WaitForSingleObject(reopenedThread.value, 15000) == WAIT_OBJECT_0);
    REQUIRE(GetExitCodeThread(reopenedThread.value, &exit) && exit == 0);
    REQUIRE(WaitForSingleObject(processCopy.value, 15000) == WAIT_OBJECT_0);
    REQUIRE(GetExitCodeProcess(reopenedProcess.value, &exit) && exit == 0);
    REQUIRE(SetFilePointerEx(duplicate.value, distance, &position, FILE_CURRENT) && position.QuadPart == 7);
    REQUIRE(WriteFile(duplicate.value, "tail", 4, &done, nullptr) && done == 4);
    REQUIRE(SetFilePointerEx(duplicate.value, distance, nullptr, FILE_BEGIN));
    char bytes[32]{};
    REQUIRE(ReadFile(duplicate.value, bytes, sizeof(bytes), &done, nullptr) && done == 11 && !std::memcmp(bytes,"abchildtail",11));
    REQUIRE(SetHandleInformation(duplicate.value, HANDLE_FLAG_PROTECT_FROM_CLOSE, HANDLE_FLAG_PROTECT_FROM_CLOSE));
    REQUIRE(!CloseHandle(duplicate.value) && GetLastError() == ERROR_INVALID_HANDLE);
    REQUIRE(SetHandleInformation(duplicate.value, HANDLE_FLAG_PROTECT_FROM_CLOSE, 0));
    duplicate.Reset();
    // A second description tests delete-on-close independently of the first.
    TemporaryFile removed;
    REQUIRE(GetTempFileNameW(directory, L"bfd", 0, removed.path) != 0);
    Handle deleting, last;
    deleting.value = CreateFileW(removed.path, GENERIC_READ | GENERIC_WRITE,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, CREATE_ALWAYS, FILE_FLAG_DELETE_ON_CLOSE, nullptr);
    REQUIRE(deleting.value != INVALID_HANDLE_VALUE);
    REQUIRE(DuplicateHandle(GetCurrentProcess(), deleting.value, GetCurrentProcess(), &last.value, 0, FALSE, DUPLICATE_SAME_ACCESS));
    deleting.Reset();
    REQUIRE(GetFileAttributesW(removed.path) != INVALID_FILE_ATTRIBUTES);
    REQUIRE(WriteFile(last.value, "x", 1, &done, nullptr) && done == 1);
    last.Reset();
    REQUIRE(GetFileAttributesW(removed.path) == INVALID_FILE_ATTRIBUTES);
    const int synchronizationAccess = CheckSynchronizationAccess();
    if (synchronizationAccess) return synchronizationAccess;
    const int waitableTimers = CheckWaitableTimers();
    if (waitableTimers) return waitableTimers;
    const int nativeWaitableTimers = CheckNativeWaitableTimers();
    if (nativeWaitableTimers) return nativeWaitableTimers;
    const int threadSynchronization = CheckThreadSynchronization();
    if (threadSynchronization) return threadSynchronization;
    const int overlappedDiskIo = CheckOverlappedDiskIo(directory);
    if (overlappedDiskIo) return overlappedDiskIo;
    const int ioCompletionPorts = CheckIoCompletionPorts(directory);
    if (ioCompletionPorts) return ioCompletionPorts;
    const int nativeIoCompletion = CheckNativeIoCompletion(directory);
    if (nativeIoCompletion) return nativeIoCompletion;
    const int nativeDiskIo = CheckNativeDiskIo(directory);
    if (nativeDiskIo) return nativeDiskIo;
    const int byteRangeLocks = CheckByteRangeLocks(directory);
    if (byteRangeLocks) return byteRangeLocks;
    const int renameOpenFile = CheckRenameOpenFile(directory);
    if (renameOpenFile) return renameOpenFile;
    const int fileMappings = CheckFileMappings(directory);
    if (fileMappings) return fileMappings;
    const int signalAndWait = CheckSignalAndWait();
    if (signalAndWait) return signalAndWait;
    const int multipleWaits = CheckMultipleWaits();
    if (multipleWaits) return multipleWaits;
    const int workerShutdown = CheckThreadShutdown(executable, false);
    return workerShutdown ? workerShutdown : CheckThreadShutdown(executable, true);
}
