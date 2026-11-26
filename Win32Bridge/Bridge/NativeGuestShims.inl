// NT ABI adapters. All handles/paths stay in the guest namespace.
namespace Win32Bridge { namespace Bridge {
namespace {
// NT create options (ReactOS sdk/include/ndk/iotypes.h); distinct from Win32 FILE_FLAG_* flags.
constexpr ULONG GuestNtFileDirectoryFile=0x00000001u;
constexpr ULONG GuestNtFileSynchronousIoAlert=0x00000010u;
constexpr ULONG GuestNtFileSynchronousIoNonalert=0x00000020u;
constexpr ULONG GuestNtFileNonDirectoryFile=0x00000040u;
constexpr ULONG GuestNtFileDeleteOnClose=0x00001000u;
constexpr ULONG GuestNtFileOpenForBackupIntent=0x00004000u;
constexpr ULONG GuestNtFileOpenReparsePoint=0x00200000u;
struct GuestUnicodeString { USHORT length, maximumLength; wchar_t* buffer; };
struct GuestRelativeName { GuestUnicodeString name; HANDLE directory; void* reference; };
struct GuestObjectAttributes { ULONG length; HANDLE root; GuestUnicodeString* name; ULONG attributes; void* security; void* qos; };
struct GuestIoStatus { LONG_PTR status; ULONG_PTR information; };
static_assert(sizeof(GuestUnicodeString)==16 && sizeof(GuestObjectAttributes)==48 && sizeof(GuestIoStatus)==16,"NT x64 ABI");
constexpr LONG NtInvalidHandle=static_cast<LONG>(0xc0000008u),NtInvalidParameter=static_cast<LONG>(0xc000000du),NtNotSupported=static_cast<LONG>(0xc00000bbu),NtLengthMismatch=static_cast<LONG>(0xc0000004u);
LONG NativeStatus(DWORD error) {
    switch(error){case 0:return 0;case ERROR_FILE_NOT_FOUND:return static_cast<LONG>(0xc0000034u);case ERROR_MOD_NOT_FOUND:return static_cast<LONG>(0xc0000135u);case ERROR_PROC_NOT_FOUND:return static_cast<LONG>(0xc0000139u);case ERROR_PATH_NOT_FOUND:return static_cast<LONG>(0xc000003au);case ERROR_ACCESS_DENIED:return static_cast<LONG>(0xc0000022u);case ERROR_INVALID_HANDLE:return NtInvalidHandle;case ERROR_INVALID_PARAMETER:return NtInvalidParameter;case ERROR_SHARING_VIOLATION:return static_cast<LONG>(0xc0000043u);case ERROR_NOT_ENOUGH_MEMORY:return static_cast<LONG>(0xc0000017u);case ERROR_BAD_LENGTH:return NtLengthMismatch;case ERROR_INSUFFICIENT_BUFFER:return static_cast<LONG>(0xc0000023u);default:return NtNotSupported;}
}

bool CopyNativeWaitDeadline(const LARGE_INTEGER* timeout, GuestKernelContext::WaitDeadline& deadline)
{
    deadline = (GuestKernelContext::WaitDeadline::max)();
    if (!timeout) return true;

    LARGE_INTEGER value{};
    if (!CopyConsoleBytes(&value, timeout, sizeof(value))) return false;
    deadline = GuestKernelContext::DeadlineFromNtTime(value.QuadPart);
    return true;
}

LONG FinishNativeWait(DWORD result, DWORD error)
{
    FinishGuestWait(result, error, false);
    if (result != WAIT_FAILED) return static_cast<LONG>(result);
    switch (error)
    {
    case ERROR_INVALID_FUNCTION: return static_cast<LONG>(0xc0000024u); // STATUS_OBJECT_TYPE_MISMATCH
    case ERROR_NOT_OWNER: return static_cast<LONG>(0xc0000046u);
    case ERROR_TOO_MANY_POSTS: return static_cast<LONG>(0xc0000047u);
    case ERROR_OPERATION_ABORTED: return static_cast<LONG>(0xc0000120u);
    default: return NativeStatus(error);
    }
}

LONG WaitForNativeGuestObjects(ULONG count, const HANDLE* handles, bool all, BOOLEAN alertable,
    const LARGE_INTEGER* timeout)
{
    ExitGuestThreadOnShutdown();
    const auto kernel = CurrentGuestKernelContext();
    if (!kernel) return NtInvalidHandle;
    GuestKernelContext::WaitDeadline deadline;
    if (!CopyNativeWaitDeadline(timeout, deadline)) return static_cast<LONG>(0xc0000005u);
    DWORD error = ERROR_SUCCESS;
    const DWORD result = kernel->WaitForMultipleObjectsUntil(count, handles, all, deadline, &error,
        alertable ? &HasPendingGuestApc : nullptr);
    // All arguments have been captured and validated. INVALID_PARAMETER here
    // identifies duplicate dispatcher objects in a wait-all set.
    if (result == WAIT_FAILED && error == ERROR_INVALID_PARAMETER) return static_cast<LONG>(0xc0000030u);
    return FinishNativeWait(result, error);
}

LONG WINAPI GuestNtWaitForSingleObject(HANDLE handle, BOOLEAN alertable, const LARGE_INTEGER* timeout)
{
    return WaitForNativeGuestObjects(1, &handle, false, alertable, timeout);
}

LONG WINAPI GuestNtWaitForMultipleObjects(ULONG count, const HANDLE* handles, ULONG waitType, BOOLEAN alertable,
    const LARGE_INTEGER* timeout)
{
    if (!count || count > MAXIMUM_WAIT_OBJECTS) return static_cast<LONG>(0xc00000efu);
    if (waitType > 1) return static_cast<LONG>(0xc00000f1u);
    std::array<HANDLE, MAXIMUM_WAIT_OBJECTS> captured;
    if (!handles || !CopyConsoleBytes(captured.data(), handles, count * sizeof(HANDLE)))
        return static_cast<LONG>(0xc0000005u);
    return WaitForNativeGuestObjects(count, captured.data(), waitType == 0, alertable, timeout);
}

LONG WINAPI GuestNtSignalAndWaitForSingleObject(HANDLE signal, HANDLE wait, BOOLEAN alertable,
    const LARGE_INTEGER* timeout)
{
    ExitGuestThreadOnShutdown();
    const auto kernel = CurrentGuestKernelContext();
    if (!kernel) return NtInvalidHandle;
    GuestKernelContext::WaitDeadline deadline;
    if (!CopyNativeWaitDeadline(timeout, deadline)) return static_cast<LONG>(0xc0000005u);
    DWORD error = ERROR_SUCCESS;
    const DWORD result = kernel->SignalObjectAndWaitUntil(signal, wait, deadline, &error,
        alertable ? &HasPendingGuestApc : nullptr);
    return FinishNativeWait(result, error);
}

constexpr LONG NtAccessViolation = static_cast<LONG>(0xc0000005u);
constexpr LONG NtUnsuccessful = static_cast<LONG>(0xc0000001u);
constexpr LONG NtInvalidParameter2 = static_cast<LONG>(0xc00000f0u);
constexpr LONG NtInvalidParameter3 = static_cast<LONG>(0xc00000f1u);
constexpr LONG NtInvalidInfoClass = static_cast<LONG>(0xc0000003u);
constexpr LONG NtObjectTypeMismatch = static_cast<LONG>(0xc0000024u);
constexpr LONG NtObjectNameInvalid = static_cast<LONG>(0xc0000033u);
constexpr LONG NtObjectNameCollision = static_cast<LONG>(0xc0000035u);
constexpr LONG NtObjectPathSyntaxBad = static_cast<LONG>(0xc000003bu);
constexpr LONG NtDatatypeMisalignment = static_cast<LONG>(0x80000002u);
constexpr LONG NtTimerResumeIgnored = 0x40000025L;
constexpr LONG NtObjectNameExists = 0x40000000L;

struct GuestClientId { HANDLE process; HANDLE thread; };
static_assert(sizeof(GuestClientId) == 16, "CLIENT_ID x64 ABI");

LONG OpenNativeGuestCompletion(bool process, HANDLE* output, DWORD access,
    const GuestObjectAttributes* attributes, const GuestClientId* clientId)
{
    constexpr ULONG ObjectInherit = 0x00000002u;
    constexpr LONG InvalidClientId = static_cast<LONG>(0xc000000bu);
    GuestObjectAttributes capturedAttributes{};
    GuestClientId capturedId{};
    if (!output || !attributes || !CopyConsoleBytes(&capturedAttributes, attributes, sizeof(capturedAttributes)))
        return NtAccessViolation;
    if (capturedAttributes.length != sizeof(capturedAttributes)) return NtInvalidParameter;
    if (capturedAttributes.name)
    {
        GuestUnicodeString name{};
        if (!CopyConsoleBytes(&name, capturedAttributes.name, sizeof(name))) return NtAccessViolation;
        if (name.length) return NtObjectNameInvalid;
    }
    if (!clientId) return NtInvalidParameter;
    if (!CopyConsoleBytes(&capturedId, clientId, sizeof(capturedId))) return NtAccessViolation;
    const auto pid = reinterpret_cast<ULONG_PTR>(capturedId.process);
    const auto tid = reinterpret_cast<ULONG_PTR>(capturedId.thread);
    if (pid > MAXDWORD || tid > MAXDWORD || !(process ? pid : tid)) return InvalidClientId;

    const auto kernel = CurrentGuestKernelContext();
    if (!kernel) return NtInvalidHandle;
    DWORD error = ERROR_SUCCESS;
    const HANDLE handle = kernel->OpenCompletion(static_cast<DWORD>(process ? pid : tid), process,
        (capturedAttributes.attributes & ObjectInherit) != 0, &error, access, process ? 0 : static_cast<DWORD>(pid));
    if (!handle) return error == ERROR_INVALID_PARAMETER ? InvalidClientId : NativeStatus(error);
    if (CopyConsoleBytes(output, &handle, sizeof(handle))) return 0;
    kernel->CloseHandle(handle, nullptr);
    return NtAccessViolation;
}

LONG WINAPI GuestNtOpenProcess(HANDLE* output, DWORD access,
    const GuestObjectAttributes* attributes, const GuestClientId* clientId)
{
    return OpenNativeGuestCompletion(true, output, access, attributes, clientId);
}

LONG WINAPI GuestNtOpenThread(HANDLE* output, DWORD access,
    const GuestObjectAttributes* attributes, const GuestClientId* clientId)
{
    return OpenNativeGuestCompletion(false, output, access, attributes, clientId);
}

LONG WINAPI GuestNtResumeThread(HANDLE thread, ULONG* previousCount)
{
    DWORD count = 0;
    if (previousCount)
    {
        if (reinterpret_cast<ULONG_PTR>(previousCount) % alignof(ULONG)) return NtDatatypeMisalignment;
        if (!CopyConsoleBytes(&count, previousCount, sizeof(count)) ||
            !CopyConsoleBytes(previousCount, &count, sizeof(count))) return NtAccessViolation;
    }
    const auto kernel = CurrentGuestKernelContext();
    DWORD error = ERROR_INVALID_HANDLE;
    if (!kernel || !kernel->ResumeThread(thread, &count, &error)) return NativeStatus(error);
    return !previousCount || CopyConsoleBytes(previousCount, &count, sizeof(count)) ? 0 : NtAccessViolation;
}

bool CaptureLocalSyncTimeout(const LARGE_INTEGER* timeout, DWORD& milliseconds)
{
    milliseconds = INFINITE;
    if (!timeout) return true;
    GuestKernelContext::WaitDeadline deadline;
    if (!CopyNativeWaitDeadline(timeout, deadline)) return false;
    const auto remaining = deadline - std::chrono::steady_clock::now();
    if (remaining <= std::chrono::steady_clock::duration::zero())
    {
        milliseconds = 0;
        return true;
    }
    auto rounded = std::chrono::duration_cast<std::chrono::milliseconds>(remaining);
    if (rounded < remaining) ++rounded;
    constexpr auto MaxFiniteWait = static_cast<std::chrono::milliseconds::rep>(INFINITE - 1);
    milliseconds = static_cast<DWORD>((std::min)(rounded.count(), MaxFiniteWait));
    return true;
}

LONG LocalSyncStatus(BOOL succeeded)
{
    if (succeeded) return 0;
    const DWORD error = ::GetLastError();
    switch (error)
    {
    case ERROR_TIMEOUT: return static_cast<LONG>(WAIT_TIMEOUT);
    case ERROR_NOACCESS: return NtAccessViolation;
    case ERROR_GEN_FAILURE: return NtUnsuccessful;
    default: return error == ERROR_SUCCESS ? NtUnsuccessful : NativeStatus(error);
    }
}

LONG WINAPI GuestRtlWaitOnAddress(volatile void* address, const void* comparison, SIZE_T size,
    const LARGE_INTEGER* timeout)
{
    DWORD milliseconds;
    if (!CaptureLocalSyncTimeout(timeout, milliseconds)) return NtAccessViolation;
    ExitGuestThreadOnShutdown();
    const LONG status = LocalSyncStatus(::WaitOnAddress(address, const_cast<void*>(comparison), size, milliseconds));
    ExitGuestThreadOnShutdown();
    return status;
}

LONG WINAPI GuestRtlSleepConditionVariableCS(PCONDITION_VARIABLE condition, PCRITICAL_SECTION section,
    const LARGE_INTEGER* timeout)
{
    if (!condition || !section) return NtAccessViolation;
    DWORD milliseconds;
    if (!CaptureLocalSyncTimeout(timeout, milliseconds)) return NtAccessViolation;
    return LocalSyncStatus(::SleepConditionVariableCS(condition, section, milliseconds));
}

LONG WINAPI GuestRtlSleepConditionVariableSRW(PCONDITION_VARIABLE condition, PSRWLOCK lock,
    const LARGE_INTEGER* timeout, ULONG flags)
{
    if (!condition || !lock) return NtAccessViolation;
    if (flags & ~CONDITION_VARIABLE_LOCKMODE_SHARED) return NtInvalidParameter;
    DWORD milliseconds;
    if (!CaptureLocalSyncTimeout(timeout, milliseconds)) return NtAccessViolation;
    return LocalSyncStatus(::SleepConditionVariableSRW(condition, lock, milliseconds, flags));
}

LONG WINAPI GuestRtlInitializeCriticalSection(LPCRITICAL_SECTION section)
{
    if (!section) return NtAccessViolation;
    ::InitializeCriticalSection(section);
    return 0;
}

LONG WINAPI GuestRtlInitializeCriticalSectionAndSpinCount(LPCRITICAL_SECTION section, DWORD spinCount)
{
    if (!section) return NtAccessViolation;
    return LocalSyncStatus(::InitializeCriticalSectionAndSpinCount(section, spinCount));
}

LONG WINAPI GuestRtlInitializeCriticalSectionEx(LPCRITICAL_SECTION section, DWORD spinCount, DWORD flags)
{
    if (!section) return NtAccessViolation;
    return LocalSyncStatus(::InitializeCriticalSectionEx(section, spinCount, flags));
}

LONG WINAPI GuestRtlEnterCriticalSection(LPCRITICAL_SECTION section)
{
    if (!section) return NtAccessViolation;
    ::EnterCriticalSection(section);
    return 0;
}

LONG WINAPI GuestRtlLeaveCriticalSection(LPCRITICAL_SECTION section)
{
    if (!section) return NtAccessViolation;
    ::LeaveCriticalSection(section);
    return 0;
}

LONG WINAPI GuestRtlDeleteCriticalSection(LPCRITICAL_SECTION section)
{
    if (!section) return NtAccessViolation;
    ::DeleteCriticalSection(section);
    return 0;
}

BOOLEAN WINAPI GuestRtlTryEnterCriticalSection(LPCRITICAL_SECTION section)
{
    return BridgeTryEnterCriticalSection(section) != FALSE;
}

LONG WINAPI GuestRtlRunOnceBeginInitialize(PINIT_ONCE once, ULONG flags, void** context)
{
    if (!once) return NtAccessViolation;
    BOOL pending = FALSE;
    if (::InitOnceBeginInitialize(once, flags, &pending, context))
        return pending ? 0x103L : 0; // STATUS_PENDING
    const LONG status = LocalSyncStatus(FALSE);
    return status == NtInvalidParameter ? NtInvalidParameter2 : status;
}

LONG WINAPI GuestRtlRunOnceComplete(PINIT_ONCE once, ULONG flags, void* context)
{
    if (!once) return NtAccessViolation;
    if (reinterpret_cast<ULONG_PTR>(context) & ((1u << INIT_ONCE_CTX_RESERVED_BITS) - 1))
        return NtInvalidParameter3;
    const LONG status = LocalSyncStatus(::InitOnceComplete(once, flags, context));
    return status == NtInvalidParameter ? NtInvalidParameter2 : status;
}

LONG WINAPI GuestRtlRunOnceExecuteOnce(PINIT_ONCE once, PINIT_ONCE_FN callback, void* parameter, void** context)
{
    if (!callback) return NtAccessViolation;
    const LONG begin = GuestRtlRunOnceBeginInitialize(once, 0, context);
    if (begin != 0x103L) return begin;
    if (!callback(once, parameter, context))
    {
        GuestRtlRunOnceComplete(once, INIT_ONCE_INIT_FAILED, nullptr);
        return NtUnsuccessful;
    }
    return GuestRtlRunOnceComplete(once, 0, context ? *context : nullptr);
}

struct GuestTimerBasicInformation
{
    LONGLONG remaining;
    BOOLEAN signaled;
    BYTE reserved[7];
};

struct GuestTimerCoalescingInformation
{
    LONGLONG due;
    GuestKernelContext::WaitableTimerApc::Procedure callback;
    void* argument;
    const void* wakeContext;
    ULONG period;
    ULONG tolerableDelay;
    BOOLEAN* previousState;
};
static_assert(sizeof(GuestTimerBasicInformation) == 16, "TimerBasicInformation x64 ABI");
static_assert(sizeof(GuestTimerCoalescingInformation) == 48, "TimerSetCoalescableTimer x64 ABI");

bool ProbeNativeOutput(void* address, size_t size)
{
    const auto start = reinterpret_cast<ULONG_PTR>(address);
    if (!start || !size || size - 1 > (std::numeric_limits<ULONG_PTR>::max)() - start) return false;
    constexpr size_t PageSize = 4096;
    __try
    {
        auto bytes = static_cast<volatile BYTE*>(address);
        bytes[0] = bytes[0];
        for (size_t offset = PageSize - (start % PageSize); offset < size; offset += PageSize)
            bytes[offset] = bytes[offset];
        bytes[size - 1] = bytes[size - 1];
        return true;
    }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

LONG NativeObjectStatus(DWORD error)
{
    switch (error)
    {
    case ERROR_INVALID_FUNCTION: return NtObjectTypeMismatch;
    case ERROR_ALREADY_EXISTS: return NtObjectNameCollision;
    case ERROR_INVALID_NAME:
    case ERROR_FILENAME_EXCED_RANGE: return NtObjectNameInvalid;
    case ERROR_TOO_MANY_OPEN_FILES: return static_cast<LONG>(0xc000011fu);
    default: return NativeStatus(error);
    }
}

LONG CaptureNativeObjectName(const GuestObjectAttributes* attributes, bool opening, std::wstring& name,
    bool& inherit, GuestKernelContext::NativeObjectNameOptions& options)
{
    if (!attributes) return opening ? NtInvalidParameter : 0;
    GuestObjectAttributes captured{};
    if (!CopyConsoleBytes(&captured, attributes, sizeof(captured))) return NtAccessViolation;
    if (captured.length != sizeof(captured)) return NtInvalidParameter;

    constexpr ULONG Inherit = 0x02;
    constexpr ULONG CaseInsensitive = 0x40;
    constexpr ULONG OpenIf = 0x80;
    if (captured.attributes & ~(Inherit | CaseInsensitive | OpenIf)) return NtInvalidParameter;
    if (captured.root || captured.security || captured.qos) return NtNotSupported;
    inherit = (captured.attributes & Inherit) != 0;
    options.ignoreCase = (captured.attributes & CaseInsensitive) != 0;
    options.openIf = (captured.attributes & OpenIf) != 0;
    if (!captured.name) return opening ? NtInvalidParameter : 0;

    GuestUnicodeString unicode{};
    if (!CopyConsoleBytes(&unicode, captured.name, sizeof(unicode))) return NtAccessViolation;
    if (unicode.length > unicode.maximumLength || unicode.length % sizeof(wchar_t)) return NtObjectNameInvalid;
    if (!unicode.length) return opening ? NtObjectNameInvalid : 0;
    if (!unicode.buffer) return NtAccessViolation;

    try
    {
        std::wstring path(unicode.length / sizeof(wchar_t), L'\0');
        if (!CopyConsoleBytes(&path[0], unicode.buffer, unicode.length)) return NtAccessViolation;
        if (path.find(L'\0') != std::wstring::npos) return NtObjectNameInvalid;
        if (path.front() != L'\\') return NtObjectPathSyntaxBad;

        // The guest has a single virtual session (TokenSessionId == 0). Its
        // BaseNamedObjects directory is shared with Win32 named handles.
        for (const wchar_t* prefix : {L"\\BaseNamedObjects\\", L"\\Sessions\\0\\BaseNamedObjects\\"})
        {
            const size_t length = wcslen(prefix);
            if (path.size() < length) continue;
            if (::CompareStringOrdinal(path.c_str(), static_cast<int>(length), prefix,
                static_cast<int>(length), options.ignoreCase) != CSTR_EQUAL) continue;
            name = path.substr(length);
            if (name.empty()) return NtObjectNameInvalid;
            // Let the common registry parse its exact Global and Local prefixes.
            if (options.ignoreCase)
            {
                if (name.size() >= 6 && ::CompareStringOrdinal(name.c_str(), 6, L"Local\\", 6, TRUE) == CSTR_EQUAL)
                    name.replace(0, 6, L"Local\\");
                else if (name.size() >= 7 && ::CompareStringOrdinal(name.c_str(), 7, L"Global\\", 7, TRUE) == CSTR_EQUAL)
                    name.replace(0, 7, L"Global\\");
            }
            return 0;
        }
        return NativeStatus(ERROR_PATH_NOT_FOUND);
    }
    catch (const std::bad_alloc&)
    {
        return NativeStatus(ERROR_NOT_ENOUGH_MEMORY);
    }
}

template<class Operation>
LONG WithNativeObjectName(HANDLE* output, const GuestObjectAttributes* attributes, bool opening,
    const Operation& operation)
{
    if (!ProbeNativeOutput(output, sizeof(*output))) return NtAccessViolation;
    std::wstring name;
    bool inherit = false;
    GuestKernelContext::NativeObjectNameOptions options;
    const LONG capture = CaptureNativeObjectName(attributes, opening, name, inherit, options);
    if (capture != 0) return capture;
    const auto kernel = CurrentGuestKernelContext();
    if (!kernel) return NtInvalidHandle;
    DWORD error = ERROR_SUCCESS;
    const HANDLE handle = operation(*kernel, name.empty() ? nullptr : name.c_str(), inherit, options, error);
    if (!handle) return NativeObjectStatus(error);
    if (!CopyConsoleBytes(output, &handle, sizeof(handle)))
    {
        kernel->CloseHandle(handle, nullptr);
        return NtAccessViolation;
    }
    return error == ERROR_ALREADY_EXISTS ? NtObjectNameExists : 0;
}

LONG WINAPI GuestNtCreateTimer(HANDLE* output, DWORD access, const GuestObjectAttributes* attributes, ULONG type)
{
    if (type > 1) return static_cast<LONG>(0xc00000f2u); // STATUS_INVALID_PARAMETER_4
    return WithNativeObjectName(output, attributes, false,
        [access, type](GuestKernelContext& kernel, LPCWSTR name, bool inherit,
            const GuestKernelContext::NativeObjectNameOptions& options, DWORD& error)
        { return kernel.CreateTimerObject(type == 0, name, &error, inherit, access, &options); });
}

LONG WINAPI GuestNtOpenTimer(HANDLE* output, DWORD access, const GuestObjectAttributes* attributes)
{
    return WithNativeObjectName(output, attributes, true,
        [access](GuestKernelContext& kernel, LPCWSTR name, bool inherit,
            const GuestKernelContext::NativeObjectNameOptions& options, DWORD& error)
        { return kernel.OpenTimerObject(access, inherit, name, &error, &options); });
}

LONG SetNativeTimer(HANDLE handle, LONGLONG due, LONG period, ULONG tolerableDelay,
    GuestKernelContext::WaitableTimerApc::Procedure callback, void* parameter, bool resume, BOOLEAN* previousState)
{
    const auto kernel = CurrentGuestKernelContext();
    if (!kernel) return NtInvalidHandle;
    bool signaled = false;
    DWORD error = ERROR_SUCCESS;
    if (!ArmGuestWaitableTimer(*kernel, handle, due, period, callback, parameter, tolerableDelay,
        error, previousState ? &signaled : nullptr)) return NativeObjectStatus(error);
    // Windows commits the new schedule even if the optional state write faults.
    const BOOLEAN state = signaled;
    if (previousState) CopyConsoleBytes(previousState, &state, sizeof(state));
    return resume ? NtTimerResumeIgnored : 0;
}

LONG WINAPI GuestNtSetTimer(HANDLE handle, const LARGE_INTEGER* dueTime,
    GuestKernelContext::WaitableTimerApc::Procedure callback, void* parameter, BOOLEAN resume, LONG period,
    BOOLEAN* previousState)
{
    if (period < 0) return static_cast<LONG>(0xc00000f4u); // STATUS_INVALID_PARAMETER_6
    LARGE_INTEGER due{};
    if (!dueTime || !CopyConsoleBytes(&due, dueTime, sizeof(due))) return NtAccessViolation;
    return SetNativeTimer(handle, due.QuadPart, period, 0, callback, parameter, resume != FALSE, previousState);
}

LONG WINAPI GuestNtSetTimerEx(HANDLE handle, ULONG informationClass, const void* information, ULONG length)
{
    if (informationClass != 0) return NtInvalidInfoClass;
    if (length != sizeof(GuestTimerCoalescingInformation)) return NtLengthMismatch;
    if (reinterpret_cast<ULONG_PTR>(information) % alignof(ULONG)) return NtDatatypeMisalignment;
    GuestTimerCoalescingInformation captured{};
    if (!information || !CopyConsoleBytes(&captured, information, sizeof(captured))) return NtAccessViolation;
    if (captured.period > static_cast<ULONG>(LONG_MAX)) return static_cast<LONG>(0xc00000f1u); // STATUS_INVALID_PARAMETER_3
    return SetNativeTimer(handle, captured.due, static_cast<LONG>(captured.period), captured.tolerableDelay,
        captured.callback, captured.argument, captured.wakeContext != nullptr, captured.previousState);
}

LONG WINAPI GuestNtCancelTimer(HANDLE handle, BOOLEAN* currentState)
{
    if (currentState && !ProbeNativeOutput(currentState, sizeof(*currentState))) return NtAccessViolation;
    const auto kernel = CurrentGuestKernelContext();
    if (!kernel) return NtInvalidHandle;
    bool signaled = false;
    DWORD error = ERROR_SUCCESS;
    if (!kernel->CancelTimerObject(handle, &error, currentState ? &signaled : nullptr))
        return NativeObjectStatus(error);
    const BOOLEAN state = signaled;
    return !currentState || CopyConsoleBytes(currentState, &state, sizeof(state)) ? 0 : NtAccessViolation;
}

LONG WINAPI GuestNtQueryTimer(HANDLE handle, ULONG informationClass, void* information, ULONG length, ULONG* returned)
{
    if (informationClass != 0) return NtInvalidInfoClass;
    constexpr ULONG Required = sizeof(GuestTimerBasicInformation);
    if (length != Required) return NtLengthMismatch;
    if (reinterpret_cast<ULONG_PTR>(information) % alignof(ULONG)) return NtDatatypeMisalignment;
    if (!ProbeNativeOutput(information, Required) ||
        (returned && !ProbeNativeOutput(returned, sizeof(*returned)))) return NtAccessViolation;
    const auto kernel = CurrentGuestKernelContext();
    if (!kernel) return NtInvalidHandle;
    GuestTimerBasicInformation captured{};
    bool signaled = false;
    DWORD error = ERROR_SUCCESS;
    if (!kernel->QueryTimerObject(handle, captured.remaining, signaled, &error)) return NativeObjectStatus(error);
    captured.signaled = signaled;
    if (!CopyConsoleBytes(information, &captured, sizeof(captured)) ||
        (returned && !CopyConsoleBytes(returned, &Required, sizeof(Required)))) return NtAccessViolation;
    return 0;
}

LONG WINAPI GuestNtCreateIoCompletion(HANDLE* output, DWORD access, const GuestObjectAttributes* attributes,
    ULONG concurrency)
{
    return WithNativeObjectName(output, attributes, false,
        [access, concurrency](GuestKernelContext& kernel, LPCWSTR name, bool inherit,
            const GuestKernelContext::NativeObjectNameOptions& options, DWORD& error)
        { return kernel.CreateIoPort(concurrency, &error, name, inherit, access, &options); });
}

LONG WINAPI GuestNtOpenIoCompletion(HANDLE* output, DWORD access, const GuestObjectAttributes* attributes)
{
    return WithNativeObjectName(output, attributes, true,
        [access](GuestKernelContext& kernel, LPCWSTR name, bool inherit,
            const GuestKernelContext::NativeObjectNameOptions& options, DWORD& error)
        { return kernel.OpenIoPort(access, inherit, name, &error, options); });
}

LONG WINAPI GuestNtQueryIoCompletion(HANDLE handle, ULONG informationClass, void* information, ULONG length,
    ULONG* returned)
{
    if (informationClass != 0) return NtInvalidInfoClass;
    constexpr ULONG Required = sizeof(LONG);
    if (length != Required) return NtLengthMismatch;
    if (reinterpret_cast<ULONG_PTR>(information) % alignof(LONG)) return NtDatatypeMisalignment;
    if (!ProbeNativeOutput(information, Required) ||
        (returned && !ProbeNativeOutput(returned, sizeof(*returned)))) return NtAccessViolation;

    const auto kernel = CurrentGuestKernelContext();
    if (!kernel) return NtInvalidHandle;
    DWORD error = ERROR_SUCCESS;
    const auto port = kernel->LookupIoPort(handle, &error, GuestKernelContext::IoCompletionQuery, true);
    if (!port) return NativeObjectStatus(error);
    const LONG depth = port->QueryDepth();
    if (!CopyConsoleBytes(information, &depth, sizeof(depth)) ||
        (returned && !CopyConsoleBytes(returned, &Required, sizeof(Required)))) return NtAccessViolation;
    return 0;
}

LONG WINAPI GuestNtSetIoCompletion(HANDLE handle, void* key, void* context, LONG status, ULONG_PTR information)
{
    const auto kernel = CurrentGuestKernelContext();
    if (!kernel) return NtInvalidHandle;
    DWORD error = ERROR_SUCCESS;
    const auto port = kernel->LookupIoPort(handle, &error, GuestKernelContext::IoCompletionModify, true);
    if (port)
        error = port->Post(GuestIoPacket{information, reinterpret_cast<ULONG_PTR>(key),
            static_cast<LPOVERLAPPED>(context), static_cast<ULONG>(status)});
    return NativeObjectStatus(error);
}

LONG FinishNativeIoCompletionWait(DWORD error)
{
    switch (error)
    {
    case ERROR_SUCCESS: return 0;
    case WAIT_TIMEOUT: return WAIT_TIMEOUT;
    case WAIT_IO_COMPLETION:
        DispatchGuestApcs(BridgeGetCurrentThreadId());
        return WAIT_IO_COMPLETION;
    case ERROR_ABANDONED_WAIT_0: return WAIT_ABANDONED_0;
    case ERROR_OPERATION_ABORTED:
        ExitGuestThreadOnShutdown();
        return static_cast<LONG>(0xc0000120u);
    default: return NativeObjectStatus(error);
    }
}

LONG WINAPI GuestNtRemoveIoCompletion(HANDLE port, void** key, void** context, GuestIoStatus* io,
    const LARGE_INTEGER* timeout)
{
    ExitGuestThreadOnShutdown();
    GuestKernelContext::WaitDeadline deadline;
    if (!CopyNativeWaitDeadline(timeout, deadline) || !ProbeNativeOutput(key, sizeof(*key)) ||
        !ProbeNativeOutput(context, sizeof(*context)) || !ProbeNativeOutput(io, sizeof(*io))) return NtAccessViolation;

    GuestIoPacket packet{};
    ULONG removed = 0;
    const DWORD error = DequeueGuestIo(port, &packet, 1, removed, deadline, false, true);
    if (!removed) return FinishNativeIoCompletionWait(error);
    void* const completionKey = reinterpret_cast<void*>(packet.key);
    void* const completionContext = packet.overlapped;
    const GuestIoStatus result{static_cast<LONG_PTR>(PublishGuestIoPacket(packet)), packet.information};
    if (!CopyConsoleBytes(key, &completionKey, sizeof(completionKey)) ||
        !CopyConsoleBytes(context, &completionContext, sizeof(completionContext)) ||
        !CopyConsoleBytes(io, &result, sizeof(result))) return NtAccessViolation;
    return 0;
}

struct GuestIoCompletionEntry
{
    ULONG_PTR key;
    void* context;
    GuestIoStatus io;
};
static_assert(sizeof(GuestIoCompletionEntry) == 32, "FILE_IO_COMPLETION_INFORMATION x64 ABI");

LONG WINAPI GuestNtRemoveIoCompletionEx(HANDLE port, GuestIoCompletionEntry* output, ULONG capacity,
    ULONG* returned, const LARGE_INTEGER* timeout, BOOLEAN alertable)
{
    ExitGuestThreadOnShutdown();
    if (!capacity) return NtInvalidParameter;
    if (reinterpret_cast<ULONG_PTR>(output) % alignof(GuestIoCompletionEntry)) return NtDatatypeMisalignment;
    GuestKernelContext::WaitDeadline deadline;
    if (!CopyNativeWaitDeadline(timeout, deadline) ||
        !ProbeNativeOutput(output, static_cast<size_t>(capacity) * sizeof(*output)) ||
        !ProbeNativeOutput(returned, sizeof(*returned))) return NtAccessViolation;

    constexpr ULONG Batch = GuestIoCompletionPort::MaximumBatch;
    ULONG total = 0;
    DWORD error = ERROR_SUCCESS;
    while (total < capacity)
    {
        GuestIoPacket packets[Batch]{};
        ULONG removed = 0;
        error = DequeueGuestIo(port, packets, (std::min)(capacity - total, Batch), removed,
            total ? IoCompletionDeadline(0) : deadline, !total && alertable, true);
        if (!removed) break;
        GuestIoCompletionEntry entries[Batch]{};
        for (ULONG index = 0; index < removed; ++index)
        {
            const auto& packet = packets[index];
            entries[index] = {packet.key, packet.overlapped,
                {static_cast<LONG_PTR>(PublishGuestIoPacket(packet)), packet.information}};
        }
        if (!CopyConsoleBytes(output + total, entries, removed * sizeof(*entries))) return NtAccessViolation;
        total += removed;
    }
    if (!CopyConsoleBytes(returned, &total, sizeof(total))) return NtAccessViolation;
    return total ? 0 : FinishNativeIoCompletionWait(error);
}

void NativeIoResult(GuestIoStatus* output,LONG status,ULONG_PTR information=0){if(output){GuestIoStatus result{status,information};CopyConsoleBytes(output,&result,sizeof(result));}}
ULONG WINAPI GuestEventRegister(const GUID* id,void*,void*,ULONGLONG* handle){
    if(!id||!handle)return ERROR_INVALID_PARAMETER;*handle=0;GUID copy{};if(!CopyConsoleBytes(&copy,id,sizeof(copy)))return ERROR_NOACCESS;
    auto kernel=CurrentGuestKernelContext();return kernel?kernel->RegisterTraceProvider(copy,handle):ERROR_INVALID_HANDLE;
}
ULONG WINAPI GuestEventUnregister(ULONGLONG handle){auto kernel=CurrentGuestKernelContext();return kernel?kernel->UnregisterTraceProvider(handle):ERROR_INVALID_HANDLE;}
ULONG WINAPI GuestEventWriteTransfer(ULONGLONG handle,const void* descriptor,const GUID*,const GUID*,ULONG count,const void* data){
    if(!descriptor||(count&&!data))return ERROR_INVALID_PARAMETER;auto kernel=CurrentGuestKernelContext();return kernel?kernel->WriteTraceEvent(handle):ERROR_INVALID_HANDLE;
}
ULONG WINAPI GuestEventSetInformation(ULONGLONG handle,ULONG kind,void* information,ULONG length){
    if(length>65535||(!information&&length))return ERROR_INVALID_PARAMETER;std::vector<BYTE> copy(length);
    if(length&&!CopyConsoleBytes(copy.data(),information,length))return ERROR_NOACCESS;
    auto kernel=CurrentGuestKernelContext();return kernel?kernel->SetTraceInformation(handle,kind,copy.data(),length):ERROR_INVALID_HANDLE;
}
BOOL WINAPI GuestRevertToSelf(){SetGuestLastError(0);return TRUE;} // Guest identity is never impersonated into the host.
BOOL WINAPI GuestGetSecurityDescriptorOwner(PSECURITY_DESCRIPTOR descriptor,PSID* owner,LPBOOL defaulted){
    if(!descriptor||!owner||!defaulted){SetGuestLastError(ERROR_INVALID_PARAMETER);return FALSE;}
    struct Header{BYTE revision,reserved;WORD control;} header{};
    if(!CopyConsoleBytes(&header,descriptor,sizeof(header))||header.revision!=1){SetGuestLastError(ERROR_INVALID_SECURITY_DESCR);return FALSE;}
    PSID sid=nullptr;
    if(header.control&0x8000u){DWORD offset=0;if(!CopyConsoleBytes(&offset,static_cast<BYTE*>(descriptor)+4,4)){SetGuestLastError(ERROR_INVALID_SECURITY_DESCR);return FALSE;}if(offset)sid=static_cast<BYTE*>(descriptor)+offset;}
    else {SECURITY_DESCRIPTOR absolute{};if(!CopyConsoleBytes(&absolute,descriptor,sizeof(absolute))){SetGuestLastError(ERROR_INVALID_SECURITY_DESCR);return FALSE;}sid=absolute.Owner;}
    *owner=sid;*defaulted=(header.control&1)!=0;SetGuestLastError(0);return TRUE;
}
struct PreserveNativeLastError
{
    const DWORD saved = BridgeGetLastError();
    ~PreserveNativeLastError() { SetGuestLastError(saved); }
};

LONG NativeLoaderStatus(DWORD error, bool ordinal = false)
{
    switch (error)
    {
    case ERROR_PROC_NOT_FOUND: return static_cast<LONG>(ordinal ? 0xc0000138u : 0xc0000139u);
    case ERROR_BAD_EXE_FORMAT: return static_cast<LONG>(0xc000007bu);
    case ERROR_DLL_INIT_FAILED: return static_cast<LONG>(0xc0000142u);
    default: return NativeStatus(error);
    }
}

LONG CaptureNativeModuleName(const GuestUnicodeString* input, std::wstring& name)
{
    if (!input) return NtInvalidParameter;
    GuestUnicodeString string{};
    if (!CopyConsoleBytes(&string, input, sizeof(string))) return NtAccessViolation;
    if (string.length > string.maximumLength || string.length % sizeof(wchar_t)) return NtInvalidParameter;
    name.resize(string.length / sizeof(wchar_t));
    if (!name.empty() &&
        (!string.buffer || !CopyConsoleBytes(&name[0], string.buffer, string.length))) return NtAccessViolation;
    if (name.find(L'\0') != std::wstring::npos) return NtInvalidParameter;
    if (name.compare(0, 4, L"\\??\\") == 0) name.erase(0, 4);
    return 0;
}

LONG WINAPI GuestLdrLoadDll(LPCWSTR searchPath, const ULONG* dllCharacteristics,
    const GuestUnicodeString* name, HMODULE* output)
{
    const PreserveNativeLastError preserve;
    if (!output) return NtInvalidParameter;
    HMODULE module = nullptr;
    if (!ProbeNativeOutput(output, sizeof(module))) return NtAccessViolation;
    try
    {
        std::wstring library, directories;
        const LONG status = CaptureNativeModuleName(name, library);
        if (status) return status;
        if (searchPath)
        {
            for (size_t index = 0; index < 32768; ++index)
            {
                wchar_t ch = 0;
                if (!CopyConsoleBytes(&ch, searchPath + index, sizeof(ch))) return NtAccessViolation;
                if (!ch) break;
                directories.push_back(ch);
            }
            if (directories.size() == 32768) return NtInvalidParameter;
        }
        ULONG characteristics = 0;
        if (dllCharacteristics && !CopyConsoleBytes(&characteristics, dllCharacteristics, sizeof(characteristics)))
            return NtAccessViolation;
        if (characteristics & ~IMAGE_FILE_EXECUTABLE_IMAGE) return NtNotSupported;
        const DWORD flags = characteristics ? DONT_RESOLVE_DLL_REFERENCES : 0;
        module = LoadGuestLibrary(library.c_str(), nullptr, flags, searchPath ? directories.c_str() : nullptr);
        if (!module) return NativeLoaderStatus(BridgeGetLastError());
        if (CopyConsoleBytes(output, &module, sizeof(module))) return 0;
        BridgeFreeLibrary(module);
        return NtAccessViolation;
    }
    catch (const std::bad_alloc&) { return NativeStatus(ERROR_NOT_ENOUGH_MEMORY); }
}

LONG WINAPI GuestLdrGetDllHandleEx(ULONG flags, LPCWSTR, const ULONG*,
    const GuestUnicodeString* name, HMODULE* output)
{
    const PreserveNativeLastError preserve;
    constexpr ULONG Unchanged = 1, Pin = 2;
    HMODULE module = nullptr;
    if (output && !ProbeNativeOutput(output, sizeof(module))) return NtAccessViolation;
    if ((flags & ~7u) || (flags & (Unchanged | Pin)) == (Unchanged | Pin) ||
        (!output && !(flags & Pin))) return NtInvalidParameter;
    try
    {
        std::wstring library;
        const LONG status = CaptureNativeModuleName(name, library);
        if (status) return status;
        module = LookupGuestModuleByName(library.c_str(), !(flags & Unchanged), (flags & Pin) != 0);
        if (!module) return NativeLoaderStatus(BridgeGetLastError());
        if (!output || CopyConsoleBytes(output, &module, sizeof(module))) return 0;
        if (!(flags & (Unchanged | Pin))) BridgeFreeLibrary(module);
        return NtAccessViolation;
    }
    catch (const std::bad_alloc&) { return NativeStatus(ERROR_NOT_ENOUGH_MEMORY); }
}

LONG WINAPI GuestLdrGetDllHandle(LPCWSTR searchPath, const ULONG* characteristics,
    const GuestUnicodeString* name, HMODULE* output)
{
    return GuestLdrGetDllHandleEx(1, searchPath, characteristics, name, output);
}

LONG WINAPI GuestLdrAddRefDll(ULONG flags, HMODULE module)
{
    if (flags & ~1u) return NtInvalidParameter;
    if (FindBridgeSystemModule(module) || IsGuestMainModule(module)) return 0;
    const auto loader = CurrentGuestModuleLoader();
    DWORD error = ERROR_MOD_NOT_FOUND;
    return loader && loader->AddReference(module, (flags & 1u) != 0, &error) ? 0 : NativeLoaderStatus(error);
}

LONG WINAPI GuestLdrUnloadDll(HMODULE module)
{
    const PreserveNativeLastError preserve;
    return BridgeFreeLibrary(module) ? 0 : NativeLoaderStatus(BridgeGetLastError());
}

LONG WINAPI GuestLdrDisableThreadCalloutsForDll(HMODULE module)
{
    const PreserveNativeLastError preserve;
    if (FindBridgeSystemModule(module)) return 0;
    return BridgeDisableThreadLibraryCalls(module) ? 0 : NativeLoaderStatus(BridgeGetLastError());
}

struct GuestAnsiString { USHORT length, maximumLength; const char* buffer; };
static_assert(sizeof(GuestAnsiString) == 16, "ANSI_STRING x64 ABI");

LONG WINAPI GuestLdrGetProcedureAddress(HMODULE module, const GuestAnsiString* name,
    ULONG ordinal, FARPROC* output)
{
    const PreserveNativeLastError preserve;
    if (!output) return NtInvalidParameter;
    FARPROC procedure = nullptr;
    if (!CopyConsoleBytes(output, &procedure, sizeof(procedure))) return NtAccessViolation;
    if (!name && !ordinal) return NtInvalidParameter;
    try
    {
        std::string symbol;
        LPCSTR requested = nullptr;
        if (name)
        {
            GuestAnsiString string{};
            if (!CopyConsoleBytes(&string, name, sizeof(string))) return NtAccessViolation;
            if (!string.length || string.length > string.maximumLength) return NtInvalidParameter;
            symbol.resize(string.length);
            if (!string.buffer || !CopyConsoleBytes(&symbol[0], string.buffer, string.length)) return NtAccessViolation;
            if (symbol.find('\0') != std::string::npos) return NtInvalidParameter;
            requested = symbol.c_str();
        }
        else
        {
            if (ordinal > 0xffffu) return NativeLoaderStatus(ERROR_PROC_NOT_FOUND, true);
            requested = reinterpret_cast<LPCSTR>(static_cast<ULONG_PTR>(ordinal));
        }
        procedure = BridgeGetProcAddress(module, requested);
        if (!procedure) return NativeLoaderStatus(BridgeGetLastError(), !name);
        return CopyConsoleBytes(output, &procedure, sizeof(procedure)) ? 0 : NtAccessViolation;
    }
    catch (const std::bad_alloc&) { return NativeStatus(ERROR_NOT_ENOUGH_MEMORY); }
}

LONG WINAPI GuestNtClose(HANDLE handle)
{
    const PreserveNativeLastError preserve;
    return BridgeCloseHandle(handle) ? 0 : NativeStatus(BridgeGetLastError());
}
LONG WINAPI GuestNtOpenProcessToken(HANDLE process,DWORD access,HANDLE* output){
    if(output)*output=nullptr;if(process!=BridgeGetCurrentProcess())return NtInvalidHandle;
    return BridgeOpenProcessToken(process,access,output)?0:NativeStatus(BridgeGetLastError());
}
LONG WINAPI GuestNtOpenThreadToken(HANDLE,DWORD,BOOLEAN,HANDLE* output){if(!output)return NtInvalidParameter;*output=nullptr;return static_cast<LONG>(0xc000007cu);} // Guest has no impersonation token.
LONG WINAPI GuestNtQueryInformationToken(HANDLE token,DWORD kind,void* buffer,ULONG length,ULONG* returned){return BridgeGetTokenInformation(token,static_cast<TOKEN_INFORMATION_CLASS>(kind),buffer,length,returned)?0:NativeStatus(BridgeGetLastError());}
ULONG WINAPI GuestRtlNtStatusToDosError(LONG status){
    if(!status)return 0;
    if((static_cast<ULONG>(status)&0xffff0000u)==0xc0070000u)return static_cast<ULONG>(status)&0xffffu;
    switch(static_cast<ULONG>(status)){
    case 0x00000102u:return ERROR_TIMEOUT;case 0x00000103u:return ERROR_IO_PENDING;
    case 0x40000000u:return ERROR_OBJECT_NAME_EXISTS;case 0x40000025u:return ERROR_TIMER_RESUME_IGNORED;
    case 0x80000005u:return ERROR_MORE_DATA;
    case 0x80000006u:return ERROR_NO_MORE_FILES;case 0x8000001au:return ERROR_NO_MORE_ITEMS;
    case 0xc0000010u:return ERROR_INVALID_FUNCTION;case 0xc0000011u:return ERROR_HANDLE_EOF;
    case 0xc0000033u:return ERROR_INVALID_NAME;
    case 0xc00000bau:return ERROR_ACCESS_DENIED;case 0xc0000103u:return ERROR_DIRECTORY;
    case 0xc00000d4u:return ERROR_NOT_SAME_DEVICE;case 0xc0000101u:return ERROR_DIR_NOT_EMPTY;
    case 0xc0000106u:return ERROR_FILENAME_EXCED_RANGE;
    case 0xc0000135u:return ERROR_MOD_NOT_FOUND;case 0xc0000139u:return ERROR_PROC_NOT_FOUND;
    case 0xc000007bu:return ERROR_BAD_EXE_FORMAT;case 0xc0000138u:return ERROR_INVALID_ORDINAL;
    case 0xc0000142u:return ERROR_DLL_INIT_FAILED;
    case 0xc0000275u:return ERROR_NOT_A_REPARSE_POINT;
    case 0xc0000002u:return ERROR_INVALID_FUNCTION;case 0xc0000003u:return ERROR_INVALID_PARAMETER;
    case 0xc0000004u:return ERROR_BAD_LENGTH;case 0xc0000023u:return ERROR_INSUFFICIENT_BUFFER;case 0xc0000008u:return ERROR_INVALID_HANDLE;
    case 0xc0000005u:case 0x80000002u:return ERROR_NOACCESS;
    case 0xc000000du:return ERROR_INVALID_PARAMETER;case 0xc0000017u:return ERROR_NOT_ENOUGH_MEMORY;
    case 0xc0000022u:return ERROR_ACCESS_DENIED;case 0xc0000034u:return ERROR_FILE_NOT_FOUND;
    case 0xc0000024u:return ERROR_INVALID_HANDLE;case 0xc0000030u:return ERROR_INVALID_PARAMETER;
    case 0xc0000046u:return ERROR_NOT_OWNER;case 0xc0000047u:return ERROR_TOO_MANY_POSTS;
    case 0xc0000054u:case 0xc0000055u:return ERROR_LOCK_VIOLATION;
    case 0xc000007eu:return ERROR_NOT_LOCKED;
    case 0xc0000001u:return ERROR_GEN_FAILURE;
    case 0xc00000efu:case 0xc00000f0u:case 0xc00000f1u:case 0xc00000f2u:case 0xc00000f4u:return ERROR_INVALID_PARAMETER;
    case 0xc0000035u:return ERROR_ALREADY_EXISTS;case 0xc000003bu:return ERROR_BAD_PATHNAME;
    case 0xc000011fu:return ERROR_TOO_MANY_OPEN_FILES;
    case 0xc000003au:return ERROR_PATH_NOT_FOUND;case 0xc0000043u:return ERROR_SHARING_VIOLATION;
    case 0xc000007cu:return ERROR_NO_TOKEN;case 0xc00000bbu:return ERROR_NOT_SUPPORTED;
    case 0xc0000120u:return ERROR_OPERATION_ABORTED;case 0xc0000225u:return ERROR_NOT_FOUND;
    default:return ERROR_MR_MID_NOT_FOUND;}
}
BOOLEAN WINAPI GuestRtlCreateUnicodeStringFromAsciiz(GuestUnicodeString* output,const char* text){
    if(!output||!text)return FALSE;*output={};const int length=BridgeMultiByteToWideChar(CP_ACP,0,text,-1,nullptr,0);
    if(length<=0||length>32767)return FALSE;
    auto buffer=static_cast<wchar_t*>(BridgeHeapAlloc(BridgeGetProcessHeap(),0,length*sizeof(wchar_t)));if(!buffer)return FALSE;
    if(!BridgeMultiByteToWideChar(CP_ACP,0,text,-1,buffer,length)){BridgeHeapFree(BridgeGetProcessHeap(),0,buffer);return FALSE;}
    *output={static_cast<USHORT>((length-1)*2),static_cast<USHORT>(length*2),buffer};return TRUE;
}
void WINAPI GuestRtlFreeUnicodeString(GuestUnicodeString* text){if(text){if(text->buffer)BridgeHeapFree(BridgeGetProcessHeap(),0,text->buffer);*text={};}}
BOOLEAN WINAPI GuestRtlDosPathNameToNtPathName_U(const wchar_t* path,GuestUnicodeString* output,wchar_t** filePart,GuestRelativeName* relative){
    if(!path||!output)return FALSE;*output={};if(filePart)*filePart=nullptr;if(relative)*relative={};
    auto storage=CurrentGuestStorageContext();std::wstring resolved;DWORD error=0;
    if(!storage||!storage->CanonicalPath(path,&resolved,&error))return FALSE;
    const std::wstring nt=L"\\??\\"+resolved;
    if(nt.size()>32766)return FALSE;auto buffer=static_cast<wchar_t*>(BridgeHeapAlloc(BridgeGetProcessHeap(),0,(nt.size()+1)*sizeof(wchar_t)));
    if(!buffer)return FALSE;memcpy(buffer,nt.c_str(),(nt.size()+1)*sizeof(wchar_t));
    *output={static_cast<USHORT>(nt.size()*2),static_cast<USHORT>((nt.size()+1)*2),buffer};
    if(filePart){const auto slash=nt.find_last_of(L'\\');if(slash!=std::wstring::npos&&slash+1<nt.size())*filePart=buffer+slash+1;}
    return TRUE;
}
LONG WINAPI GuestRtlDosPathNameToRelativeNtPathName_U_WithStatus(const wchar_t* path,GuestUnicodeString* output,wchar_t** part,GuestRelativeName* relative){return GuestRtlDosPathNameToNtPathName_U(path,output,part,relative)?0:static_cast<LONG>(0xc0000033u);}
void WINAPI GuestRtlReleaseRelativeName(GuestRelativeName* relative){if(relative)*relative={};} // Absolute conversion does not acquire a directory reference.
CHAR WINAPI GuestRtlFindLeastSignificantBit(ULONGLONG value){if(!value)return -1;CHAR bit=0;while(!(value&1)){value>>=1;++bit;}return bit;}
BOOLEAN WINAPI GuestRtlFreeHeap(HANDLE heap,ULONG flags,void* memory){return BridgeHeapFree(heap,flags,memory)?TRUE:FALSE;}
LONG WINAPI GuestNtOpenFile(HANDLE* output,DWORD access,const GuestObjectAttributes* attributes,GuestIoStatus* io,ULONG share,ULONG options){
    const PreserveNativeLastError preserve;
    if(!output||!attributes||!io)return NtInvalidParameter;*output=nullptr;GuestObjectAttributes object{};
    if(!CopyConsoleBytes(&object,attributes,sizeof(object))||object.length!=sizeof(object)||!object.name)return NtInvalidParameter;
    GuestUnicodeString name{};if(!CopyConsoleBytes(&name,object.name,sizeof(name))||!name.length||name.length%2||!name.buffer)return NtInvalidParameter;
    std::wstring path(name.length/2,L'\0');if(!CopyConsoleBytes(&path[0],name.buffer,name.length))return NtInvalidParameter;
    if(path.compare(0,4,L"\\??\\")==0)path.erase(0,4);
    if (object.root)
    {
        auto storage = CurrentGuestStorageContext(); DWORD error=0; std::wstring directory;
        if (!storage || !storage->IsDirectoryHandle(object.root,&error) || !storage->GetFilePath(object.root,&directory,&error))
        { const LONG status=NativeStatus(error?error:ERROR_INVALID_HANDLE); NativeIoResult(io,status); return status; }
        if (path.empty() || path.front()==L'\\' || path.find(L':')!=std::wstring::npos) { NativeIoResult(io,NtInvalidParameter); return NtInvalidParameter; }
        path = directory + L"\\" + path;
    }
    if (name.maximumLength < name.length || path.find(L'\0') != std::wstring::npos || (options & (GuestNtFileSynchronousIoAlert|GuestNtFileSynchronousIoNonalert)) == (GuestNtFileSynchronousIoAlert|GuestNtFileSynchronousIoNonalert)) { NativeIoResult(io,NtInvalidParameter); return NtInvalidParameter; }
    const DWORD existing = BridgeGetFileAttributesW(path.c_str());
    if (existing != INVALID_FILE_ATTRIBUTES)
    {
        const bool directory = (existing & FILE_ATTRIBUTE_DIRECTORY) != 0;
        if (directory && (options & GuestNtFileNonDirectoryFile)) { const LONG status=static_cast<LONG>(0xc00000bau); NativeIoResult(io,status); return status; }
        if (!directory && (options & GuestNtFileDirectoryFile)) { const LONG status=static_cast<LONG>(0xc0000103u); NativeIoResult(io,status); return status; }
    }
    if(options&~(GuestNtFileDirectoryFile|GuestNtFileSynchronousIoNonalert|GuestNtFileNonDirectoryFile|GuestNtFileSynchronousIoAlert|GuestNtFileOpenReparsePoint|GuestNtFileOpenForBackupIntent|GuestNtFileDeleteOnClose)){NativeIoResult(io,NtNotSupported);return NtNotSupported;}
    SECURITY_ATTRIBUTES security{};security.nLength=sizeof(security);security.bInheritHandle=(object.attributes&2)!=0;
    DWORD flags=FILE_ATTRIBUTE_NORMAL;
    if (!(options & (GuestNtFileSynchronousIoAlert | GuestNtFileSynchronousIoNonalert))) flags |= FILE_FLAG_OVERLAPPED;
    if((options&GuestNtFileOpenForBackupIntent)||(existing!=INVALID_FILE_ATTRIBUTES&&(existing&FILE_ATTRIBUTE_DIRECTORY)))flags|=FILE_FLAG_BACKUP_SEMANTICS;
    if(options&GuestNtFileOpenReparsePoint)flags|=FILE_FLAG_OPEN_REPARSE_POINT;
    if(options&GuestNtFileDeleteOnClose)
    {
        if(!(access&DELETE)){const LONG status=static_cast<LONG>(0xc0000022u);NativeIoResult(io,status);return status;}
        flags|=FILE_FLAG_DELETE_ON_CLOSE;
    }
    const HANDLE handle=BridgeCreateFileW(path.c_str(),access,share,&security,OPEN_EXISTING,flags,nullptr);
    const LONG status=handle==INVALID_HANDLE_VALUE?NativeStatus(BridgeGetLastError()):0;
    if(!status)*output=handle;NativeIoResult(io,status,status?0:1);return status;
}

constexpr ULONG GuestFileCompletionInformation = 30;
constexpr ULONG GuestFileIoCompletionNotificationInformation = 41;
constexpr ULONG GuestFileStandardInformation = 5;
constexpr ULONG GuestFileRenameInformation = 10;
constexpr ULONG GuestFileDispositionInformation = 13;
constexpr ULONG GuestFilePositionInformation = 14;
constexpr ULONG GuestFileEndOfFileInformation = 20;
constexpr ULONG GuestFileDispositionInformationEx = 64;
constexpr ULONG GuestFileRenameInformationEx = 65;
static_assert(sizeof(FILE_STANDARD_INFO) == 24, "FILE_STANDARD_INFORMATION x64 ABI");
struct GuestFileCompletion
{
    HANDLE port;
    ULONG_PTR key;
};
static_assert(sizeof(GuestFileCompletion) == 16, "FILE_COMPLETION_INFORMATION x64 ABI");

LONG TransferNativeFile(HANDLE handle, HANDLE event, GuestNativeIoApc apc, void* context, GuestIoStatus* io,
    void* buffer, ULONG length, const LARGE_INTEGER* byteOffset, const ULONG* key, bool writing)
{
    const auto storage = CurrentGuestStorageContext();
    const auto kernel = CurrentGuestKernelContext();
    if (!storage || !kernel) return NtInvalidHandle;
    const auto file = storage->CaptureDiskFile(handle);
    if (!file.Identity()) return NtInvalidHandle;
    if (!ProbeNativeOutput(io, sizeof(*io))) return NtAccessViolation;

    constexpr LONGLONG UseFilePointer = -2;
    constexpr LONGLONG WriteToEnd = -1;
    LARGE_INTEGER start{};
    start.QuadPart = UseFilePointer;
    ULONG lockKey = 0;
    if ((byteOffset && !CopyConsoleBytes(&start, byteOffset, sizeof(start))) ||
        (key && !CopyConsoleBytes(&lockKey, key, sizeof(lockKey)))) return NtAccessViolation;
    const bool allowsFilePointer = !file.IsOverlapped() || (byteOffset && writing && file.IsAppendOnly());
    if (start.QuadPart < 0 &&
        !(start.QuadPart == UseFilePointer && allowsFilePointer) &&
        !(start.QuadPart == WriteToEnd && writing)) return NtInvalidParameter;
    if (length > GuestStorageContext::MaximumTransferSize()) return NativeStatus(ERROR_NOT_ENOUGH_MEMORY);
    if (length && (!buffer || (!writing && !ProbeNativeOutput(buffer, length)))) return NtAccessViolation;
    const DWORD access = file.ValidateAccess(writing);
    if (access == ERROR_INVALID_FUNCTION) return static_cast<LONG>(0xc0000010u);
    if (access) return NativeObjectStatus(access);

    // Native event handles can carry tag bits. ApcContext alone determines
    // whether an IOCP receives this request's completion.
    const HANDLE waitEvent = reinterpret_cast<HANDLE>(reinterpret_cast<ULONG_PTR>(event) & ~ULONG_PTR{3});
    const ULONGLONG offset = static_cast<ULONGLONG>(start.QuadPart);
    if (file.IsOverlapped())
    {
        const GuestDiskCompletionTarget target{io, context, waitEvent, nullptr, apc, false};
        const DWORD result = QueueGuestDiskRequest(*storage, handle, file, writing, buffer, length, offset, target, lockKey);
        if (result == ERROR_IO_PENDING) return 0x103L;
        if (result == ERROR_NOACCESS) return NtAccessViolation;
        return NativeObjectStatus(result);
    }

    try
    {
        GuestKernelContext::IoEvent completionEvent;
        DWORD error = ERROR_SUCCESS;
        if (waitEvent && !kernel->CaptureIoEvent(waitEvent, completionEvent, &error)) return NativeObjectStatus(error);
        std::vector<BYTE> staging(length);
        if (writing && length && !CopyConsoleBytes(staging.data(), buffer, length)) return NtAccessViolation;
        const auto signal = storage->LookupFileSignal(handle);
        signal.Begin();
        completionEvent.Set(false);
        DWORD transferred = 0;
        error = GuestStorageContext::TransferDiskFile(file, writing, offset, staging.data(), length,
            transferred, Concurrency::cancellation_token::none(), true, lockKey);
        if (!writing && !error && transferred && !CopyConsoleBytes(buffer, staging.data(), transferred))
        {
            error = ERROR_NOACCESS;
            transferred = 0;
        }
        const ULONG status = static_cast<ULONG>(GuestNtStatusForIoError(error));
        const bool published = WriteGuestIoStatus(io, status, transferred);
        completionEvent.Set(true);
        signal.Complete();
        return published ? static_cast<LONG>(status) : NtAccessViolation;
    }
    catch (const std::bad_alloc&) { return NativeStatus(ERROR_NOT_ENOUGH_MEMORY); }
}

LONG WINAPI GuestNtReadFile(HANDLE handle, HANDLE event, GuestNativeIoApc apc, void* context, GuestIoStatus* io,
    void* buffer, ULONG length, const LARGE_INTEGER* byteOffset, const ULONG* key)
{
    return TransferNativeFile(handle, event, apc, context, io, buffer, length, byteOffset, key, false);
}

LONG WINAPI GuestNtWriteFile(HANDLE handle, HANDLE event, GuestNativeIoApc apc, void* context, GuestIoStatus* io,
    const void* buffer, ULONG length, const LARGE_INTEGER* byteOffset, const ULONG* key)
{
    return TransferNativeFile(handle, event, apc, context, io, const_cast<void*>(buffer), length, byteOffset, key, true);
}

LONG WINAPI GuestNtLockFile(HANDLE handle, HANDLE event, GuestNativeIoApc apc, void* context,
    GuestIoStatus* io, const LARGE_INTEGER* byteOffset, const LARGE_INTEGER* length,
    ULONG key, BOOLEAN failImmediately, BOOLEAN exclusive)
{
    const auto storage = CurrentGuestStorageContext();
    if (!storage) return NtInvalidHandle;
    LARGE_INTEGER start{}, extent{};
    if (!ProbeNativeOutput(io, sizeof(*io)) || !byteOffset || !length ||
        !CopyConsoleBytes(&start, byteOffset, sizeof(start)) ||
        !CopyConsoleBytes(&extent, length, sizeof(extent))) return NtAccessViolation;
    const HANDLE waitEvent = reinterpret_cast<HANDLE>(reinterpret_cast<ULONG_PTR>(event) & ~ULONG_PTR{3});
    const GuestDiskCompletionTarget target{io, context, waitEvent, nullptr, apc, false};
    const DWORD error = RequestGuestRangeLock(*storage, handle, static_cast<ULONGLONG>(start.QuadPart),
        static_cast<ULONGLONG>(extent.QuadPart), exclusive != FALSE, failImmediately != FALSE, key, target);
    if (error == ERROR_OPERATION_ABORTED) ExitGuestThreadOnShutdown();
    if (error == ERROR_LOCK_VIOLATION) return static_cast<LONG>(0xc0000055u);
    return static_cast<LONG>(GuestNtStatusForIoError(error));
}

LONG WINAPI GuestNtUnlockFile(HANDLE handle, GuestIoStatus* io, const LARGE_INTEGER* byteOffset,
    const LARGE_INTEGER* length, ULONG key)
{
    const auto storage = CurrentGuestStorageContext();
    if (!storage) return NtInvalidHandle;
    LARGE_INTEGER start{}, extent{};
    if (!ProbeNativeOutput(io, sizeof(*io)) || !byteOffset || !length ||
        !CopyConsoleBytes(&start, byteOffset, sizeof(start)) ||
        !CopyConsoleBytes(&extent, length, sizeof(extent))) return NtAccessViolation;
    DWORD error = ERROR_SUCCESS;
    storage->UnlockFileRange(handle, static_cast<ULONGLONG>(start.QuadPart),
        static_cast<ULONGLONG>(extent.QuadPart), &error, key);
    const auto status = GuestNtStatusForIoError(error);
    return WriteGuestIoStatus(io, status, 0) ? static_cast<LONG>(status) : NtAccessViolation;
}

LONG CancelNativeFileIo(HANDLE file, const GuestIoStatus* request, GuestIoStatus* output, bool allThreads)
{
    if (!ProbeNativeOutput(output, sizeof(*output))) return NtAccessViolation;
    const auto storage = CurrentGuestStorageContext();
    const auto kernel = CurrentGuestKernelContext();
    const auto description = storage ? storage->RetainFileDescription(file) : nullptr;
    if (!kernel || !description) return NtInvalidHandle;
    const auto filter = reinterpret_cast<LPOVERLAPPED>(const_cast<GuestIoStatus*>(request));
    const bool matched = CancelGuestFileRequests(description.get(), kernel,
        allThreads ? 0 : BridgeGetCurrentThreadId(), filter);
    const ULONG status = allThreads && !matched ? 0xc0000225u : 0; // STATUS_NOT_FOUND
    return WriteGuestIoStatus(output, status, 0) ? static_cast<LONG>(status) : NtAccessViolation;
}

LONG WINAPI GuestNtCancelIoFile(HANDLE file, GuestIoStatus* output)
{
    return CancelNativeFileIo(file, nullptr, output, false);
}

LONG WINAPI GuestNtCancelIoFileEx(HANDLE file, const GuestIoStatus* request, GuestIoStatus* output)
{
    return CancelNativeFileIo(file, request, output, true);
}

LONG SetNativeFileCompletion(HANDLE handle, GuestIoStatus* io, const void* information, ULONG length, ULONG kind)
{
    const bool association = kind == GuestFileCompletionInformation;
    const size_t required = association ? sizeof(GuestFileCompletion) : sizeof(ULONG);
    if (length < required) return NtLengthMismatch;
    if (!information || !ProbeNativeOutput(io, sizeof(*io))) return NtAccessViolation;
    const auto storage = CurrentGuestStorageContext();
    const auto kernel = CurrentGuestKernelContext();
    if (!storage || !kernel) return NtInvalidHandle;
    const auto file = storage->CaptureDiskFile(handle);
    if (!file.Identity()) return NtInvalidHandle;
    if (!file.IsOverlapped()) return NtInvalidParameter;

    DWORD error = ERROR_SUCCESS;
    if (association)
    {
        GuestFileCompletion binding{};
        if (!CopyConsoleBytes(&binding, information, sizeof(binding))) return NtAccessViolation;
        const auto port = kernel->LookupIoPort(binding.port, &error, GuestKernelContext::IoCompletionModify, true);
        if (!port || !storage->BindIoCompletionPort(handle, port, binding.key, &error))
            return NativeObjectStatus(error);
    }
    else
    {
        ULONG flags = 0;
        if (!CopyConsoleBytes(&flags, information, sizeof(flags))) return NtAccessViolation;
        // Native FileIoCompletionNotificationInformation also records the
        // fast-I/O user-event bit. Guest files use the regular completion path.
        constexpr ULONG SkipFastIoEvent = 0x4;
        constexpr ULONG Available = FILE_SKIP_COMPLETION_PORT_ON_SUCCESS | FILE_SKIP_SET_EVENT_ON_HANDLE | SkipFastIoEvent;
        if (!storage->SetIoCompletionModes(handle, static_cast<UCHAR>(flags & Available), &error))
            return NativeObjectStatus(error);
    }
    const GuestIoStatus result{};
    return CopyConsoleBytes(io, &result, sizeof(result)) ? 0 : NtAccessViolation;
}

LONG WINAPI GuestNtQueryInformationFile(HANDLE handle, GuestIoStatus* io, void* information, ULONG length, ULONG kind)
{
    size_t required = 0;
    switch (kind)
    {
    case GuestFileStandardInformation: required = sizeof(FILE_STANDARD_INFO); break;
    case GuestFilePositionInformation: required = sizeof(LARGE_INTEGER); break;
    case GuestFileIoCompletionNotificationInformation: required = sizeof(ULONG); break;
    default: return NtInvalidInfoClass;
    }
    if (length < required) return NtLengthMismatch;
    if (!ProbeNativeOutput(information, required) || !ProbeNativeOutput(io, sizeof(*io))) return NtAccessViolation;
    const auto storage = CurrentGuestStorageContext();
    if (!storage || !storage->OwnsFile(handle)) return NtInvalidHandle;
    DWORD error = ERROR_SUCCESS;
    if (kind == GuestFileStandardInformation)
    {
        FILE_STANDARD_INFO standard{};
        if (!storage->GetFileStandardInfo(handle, &standard, &error)) return NativeStatus(error);
        if (!CopyConsoleBytes(information, &standard, required)) return NtAccessViolation;
    }
    else if (kind == GuestFilePositionInformation)
    {
        LARGE_INTEGER position{};
        if (!storage->SetFilePointer(handle, {}, &position, FILE_CURRENT, &error)) return NativeStatus(error);
        if (!CopyConsoleBytes(information, &position, required)) return NtAccessViolation;
    }
    else
    {
        const ULONG modes = storage->IoCompletionBinding(handle).modes;
        if (!CopyConsoleBytes(information, &modes, required)) return NtAccessViolation;
    }
    const GuestIoStatus result{0, required};
    return CopyConsoleBytes(io, &result, sizeof(result)) ? 0 : NtAccessViolation;
}

LONG NativeFileUpdateStatus(DWORD error)
{
    switch (error)
    {
    case ERROR_NOACCESS: return NtAccessViolation;
    case ERROR_ALREADY_EXISTS:
    case ERROR_FILE_EXISTS: return static_cast<LONG>(0xc0000035u);
    case ERROR_INVALID_NAME: return static_cast<LONG>(0xc0000033u);
    case ERROR_NOT_SAME_DEVICE: return static_cast<LONG>(0xc00000d4u);
    case ERROR_DIR_NOT_EMPTY: return static_cast<LONG>(0xc0000101u);
    case ERROR_FILENAME_EXCED_RANGE: return static_cast<LONG>(0xc0000106u);
    default: return NativeStatus(error);
    }
}

LONG WINAPI GuestNtSetInformationFile(HANDLE handle, GuestIoStatus* io, void* buffer, ULONG length, ULONG kind)
{
    if (kind == GuestFileCompletionInformation || kind == GuestFileIoCompletionNotificationInformation)
        return SetNativeFileCompletion(handle, io, buffer, length, kind);
    if (!ProbeNativeOutput(io, sizeof(*io))) return NtAccessViolation;
    DWORD error = ERROR_SUCCESS;
    if (kind == GuestFilePositionInformation)
    {
        LARGE_INTEGER position{};
        if (length < sizeof(position)) return NtLengthMismatch;
        if (!buffer || !CopyConsoleBytes(&position, buffer, sizeof(position))) return NtAccessViolation;
        if (position.QuadPart < 0) return NtInvalidParameter;
        const auto storage = CurrentGuestStorageContext();
        if (!storage) return NtInvalidHandle;
        storage->SetFilePointer(handle, position, nullptr, FILE_BEGIN, &error);
    }
    else
    {
        FILE_INFO_BY_HANDLE_CLASS update;
        switch (kind)
        {
        case GuestFileRenameInformation: update = FileRenameInfo; break;
        case GuestFileDispositionInformation: update = FileDispositionInfo; break;
        case GuestFileEndOfFileInformation: update = FileEndOfFileInfo; break;
        case GuestFileDispositionInformationEx: update = FileDispositionInfoEx; break;
        case GuestFileRenameInformationEx: update = FileRenameInfoEx; break;
        default: return NtNotSupported;
        }
        error = SetGuestFileInformation(handle, update, buffer, length, true);
    }
    const LONG status = NativeFileUpdateStatus(error);
    const GuestIoStatus result{status, 0};
    return CopyConsoleBytes(io, &result, sizeof(result)) ? status : NtAccessViolation;
}
LONG WINAPI GuestNtFsControlFile(HANDLE,HANDLE,void*,void*,GuestIoStatus* io,ULONG code,void*,ULONG,void*,ULONG){
    // The virtual drive has no reparse points; other FSCTLs are explicitly unsupported.
    LONG status=code==0x000900a8u?static_cast<LONG>(0xc0000275u):static_cast<LONG>(0xc0000010u);NativeIoResult(io,status);return status;
}
LONG WINAPI GuestNtCancelSynchronousIoFile(HANDLE,GuestIoStatus*,GuestIoStatus* result){NativeIoResult(result,NtNotSupported);return NtNotSupported;}
LONG WINAPI GuestNtQueryVolumeInformationFile(HANDLE handle,GuestIoStatus* io,void* output,ULONG length,ULONG kind){
    DWORD type=BridgeGetFileType(handle);if(type==FILE_TYPE_UNKNOWN){NativeIoResult(io,NtInvalidHandle);return NtInvalidHandle;}
    if(kind==4){ULONG device[2]={type==FILE_TYPE_PIPE?0x11u:7u,0};LONG status=length<sizeof(device)?NtLengthMismatch:0;if(!status&&!CopyConsoleBytes(output,device,sizeof(device)))status=NtInvalidParameter;NativeIoResult(io,status,status?0:sizeof(device));return status;}
    NativeIoResult(io,NtNotSupported);return NtNotSupported;
}
LONG QueryNativeGuestProcessImageName(HANDLE process, bool nativePath, void* output,
    ULONG length, ULONG* returned)
{
    const PreserveNativeLastError preserve;
    try
    {
        std::wstring path;
        if (!GetGuestProcessImagePath(process, nativePath, path)) return NativeStatus(BridgeGetLastError());
        if (path.size() > (0xffffu / sizeof(wchar_t)) - 1) return NtLengthMismatch;
        const auto textBytes = static_cast<USHORT>(path.size() * sizeof(wchar_t));
        const auto capacity = static_cast<USHORT>(textBytes + sizeof(wchar_t));
        const ULONG required = sizeof(GuestUnicodeString) + capacity;
        if (returned && !CopyConsoleBytes(returned, &required, sizeof(required))) return NtAccessViolation;
        if (length < required) return NtLengthMismatch;
        if (!output) return NtAccessViolation;

        auto buffer = reinterpret_cast<wchar_t*>(static_cast<BYTE*>(output) + sizeof(GuestUnicodeString));
        GuestUnicodeString result{};
        result.length = textBytes;
        result.maximumLength = capacity;
        result.buffer = buffer;
        if (!CopyConsoleBytes(buffer, path.c_str(), capacity) ||
            !CopyConsoleBytes(output, &result, sizeof(result))) return NtAccessViolation;
        return 0;
    }
    catch (const std::bad_alloc&)
    {
        return NativeStatus(ERROR_NOT_ENOUGH_MEMORY);
    }
}

LONG WINAPI GuestNtQueryInformationProcess(HANDLE process,ULONG kind,void* output,ULONG length,ULONG* returned){
    constexpr ULONG processImageFileName = 27;
    constexpr ULONG processImageFileNameWin32 = 43;
    if (kind == processImageFileName || kind == processImageFileNameWin32)
        return QueryNativeGuestProcessImageName(process, kind == processImageFileName, output, length, returned);
    if(returned)*returned=0;if(process!=BridgeGetCurrentProcess())return NtInvalidHandle;
    // Guest processes are native x64, not debugged, and cannot terminate the host.
    ULONG_PTR value=0;ULONG required=sizeof(value);
    if(kind==29)required=sizeof(ULONG);else if(kind==61)required=1;else if(kind!=7&&kind!=26)return static_cast<LONG>(0xc0000003u);
    if(returned)*returned=required;if(length<required)return NtLengthMismatch;
    return CopyConsoleBytes(output,&value,required)?0:NtInvalidParameter;
}
LONG WINAPI GuestNtSetInformationProcess(HANDLE process,ULONG kind,const void* value,ULONG length){
    if(process!=BridgeGetCurrentProcess())return NtInvalidHandle;
    if(kind==29&&value&&length==sizeof(ULONG)){ULONG enabled=1;CopyConsoleBytes(&enabled,value,sizeof(enabled));return enabled?static_cast<LONG>(0xc0000022u):0;}
    return static_cast<LONG>(0xc0000003u);
}
}
ImportResolution ResolveNativeGuestImport(const ImportedSymbol& symbol){
    ImportResolution result{ImportDisposition::Unsupported,L"No NT guest implementation.",0};
    if(symbol.importedByOrdinal)return result;
    if(_wcsicmp(symbol.library.c_str(),L"advapi32.dll")==0) {
        if(symbol.name==L"EventRegister")result.targetAddress=reinterpret_cast<ULONGLONG>(&GuestEventRegister);
        else if(symbol.name==L"EventUnregister")result.targetAddress=reinterpret_cast<ULONGLONG>(&GuestEventUnregister);
        else if(symbol.name==L"EventWriteTransfer")result.targetAddress=reinterpret_cast<ULONGLONG>(&GuestEventWriteTransfer);
        else if(symbol.name==L"EventSetInformation")result.targetAddress=reinterpret_cast<ULONGLONG>(&GuestEventSetInformation);
        else if(symbol.name==L"RevertToSelf")result.targetAddress=reinterpret_cast<ULONGLONG>(&GuestRevertToSelf);
        else if(symbol.name==L"GetSecurityDescriptorOwner")result.targetAddress=reinterpret_cast<ULONGLONG>(&GuestGetSecurityDescriptorOwner);
        if(result.targetAddress){result.disposition=ImportDisposition::NeedsBridge;result.note=L"Isolated guest identity and ETW providers with no enabled tracing sessions.";}
        return result;
    }
    if(_wcsicmp(symbol.library.c_str(),L"ntdll.dll"))return result;
#define NATIVE_EXPORT(exportName,function) if(symbol.name==L##exportName)result.targetAddress=reinterpret_cast<ULONGLONG>(&function);else
    NATIVE_EXPORT("NtClose",GuestNtClose)
    NATIVE_EXPORT("ZwClose",GuestNtClose)
    NATIVE_EXPORT("NtWaitForSingleObject",GuestNtWaitForSingleObject)
    NATIVE_EXPORT("ZwWaitForSingleObject",GuestNtWaitForSingleObject)
    NATIVE_EXPORT("NtWaitForMultipleObjects",GuestNtWaitForMultipleObjects)
    NATIVE_EXPORT("ZwWaitForMultipleObjects",GuestNtWaitForMultipleObjects)
    NATIVE_EXPORT("NtSignalAndWaitForSingleObject",GuestNtSignalAndWaitForSingleObject)
    NATIVE_EXPORT("ZwSignalAndWaitForSingleObject",GuestNtSignalAndWaitForSingleObject)
    NATIVE_EXPORT("NtResumeThread",GuestNtResumeThread)
    NATIVE_EXPORT("ZwResumeThread",GuestNtResumeThread)
    NATIVE_EXPORT("NtOpenProcess",GuestNtOpenProcess)
    NATIVE_EXPORT("ZwOpenProcess",GuestNtOpenProcess)
    NATIVE_EXPORT("NtOpenThread",GuestNtOpenThread)
    NATIVE_EXPORT("ZwOpenThread",GuestNtOpenThread)
    NATIVE_EXPORT("NtCreateTimer",GuestNtCreateTimer)
    NATIVE_EXPORT("ZwCreateTimer",GuestNtCreateTimer)
    NATIVE_EXPORT("NtOpenTimer",GuestNtOpenTimer)
    NATIVE_EXPORT("ZwOpenTimer",GuestNtOpenTimer)
    NATIVE_EXPORT("NtSetTimer",GuestNtSetTimer)
    NATIVE_EXPORT("ZwSetTimer",GuestNtSetTimer)
    NATIVE_EXPORT("NtSetTimerEx",GuestNtSetTimerEx)
    NATIVE_EXPORT("ZwSetTimerEx",GuestNtSetTimerEx)
    NATIVE_EXPORT("NtCancelTimer",GuestNtCancelTimer)
    NATIVE_EXPORT("ZwCancelTimer",GuestNtCancelTimer)
    NATIVE_EXPORT("NtQueryTimer",GuestNtQueryTimer)
    NATIVE_EXPORT("ZwQueryTimer",GuestNtQueryTimer)
    NATIVE_EXPORT("NtCreateIoCompletion",GuestNtCreateIoCompletion)
    NATIVE_EXPORT("ZwCreateIoCompletion",GuestNtCreateIoCompletion)
    NATIVE_EXPORT("NtOpenIoCompletion",GuestNtOpenIoCompletion)
    NATIVE_EXPORT("ZwOpenIoCompletion",GuestNtOpenIoCompletion)
    NATIVE_EXPORT("NtQueryIoCompletion",GuestNtQueryIoCompletion)
    NATIVE_EXPORT("ZwQueryIoCompletion",GuestNtQueryIoCompletion)
    NATIVE_EXPORT("NtSetIoCompletion",GuestNtSetIoCompletion)
    NATIVE_EXPORT("ZwSetIoCompletion",GuestNtSetIoCompletion)
    NATIVE_EXPORT("NtRemoveIoCompletion",GuestNtRemoveIoCompletion)
    NATIVE_EXPORT("ZwRemoveIoCompletion",GuestNtRemoveIoCompletion)
    NATIVE_EXPORT("NtRemoveIoCompletionEx",GuestNtRemoveIoCompletionEx)
    NATIVE_EXPORT("ZwRemoveIoCompletionEx",GuestNtRemoveIoCompletionEx)
    NATIVE_EXPORT("RtlWaitOnAddress",GuestRtlWaitOnAddress)
    NATIVE_EXPORT("RtlWakeAddressSingle",BridgeWakeByAddressSingle)
    NATIVE_EXPORT("RtlWakeAddressAll",BridgeWakeByAddressAll)
    NATIVE_EXPORT("RtlInitializeSRWLock",BridgeInitializeSRWLock)
    NATIVE_EXPORT("RtlAcquireSRWLockExclusive",BridgeAcquireSRWLockExclusive)
    NATIVE_EXPORT("RtlReleaseSRWLockExclusive",BridgeReleaseSRWLockExclusive)
    NATIVE_EXPORT("RtlTryAcquireSRWLockExclusive",BridgeTryAcquireSRWLockExclusive)
    NATIVE_EXPORT("RtlAcquireSRWLockShared",BridgeAcquireSRWLockShared)
    NATIVE_EXPORT("RtlReleaseSRWLockShared",BridgeReleaseSRWLockShared)
    NATIVE_EXPORT("RtlTryAcquireSRWLockShared",BridgeTryAcquireSRWLockShared)
    NATIVE_EXPORT("RtlInitializeConditionVariable",BridgeInitializeConditionVariable)
    NATIVE_EXPORT("RtlWakeConditionVariable",BridgeWakeConditionVariable)
    NATIVE_EXPORT("RtlWakeAllConditionVariable",BridgeWakeAllConditionVariable)
    NATIVE_EXPORT("RtlSleepConditionVariableCS",GuestRtlSleepConditionVariableCS)
    NATIVE_EXPORT("RtlSleepConditionVariableSRW",GuestRtlSleepConditionVariableSRW)
    NATIVE_EXPORT("RtlInitializeCriticalSection",GuestRtlInitializeCriticalSection)
    NATIVE_EXPORT("RtlInitializeCriticalSectionAndSpinCount",GuestRtlInitializeCriticalSectionAndSpinCount)
    NATIVE_EXPORT("RtlInitializeCriticalSectionEx",GuestRtlInitializeCriticalSectionEx)
    NATIVE_EXPORT("RtlEnterCriticalSection",GuestRtlEnterCriticalSection)
    NATIVE_EXPORT("RtlLeaveCriticalSection",GuestRtlLeaveCriticalSection)
    NATIVE_EXPORT("RtlDeleteCriticalSection",GuestRtlDeleteCriticalSection)
    NATIVE_EXPORT("RtlTryEnterCriticalSection",GuestRtlTryEnterCriticalSection)
    NATIVE_EXPORT("RtlSetCriticalSectionSpinCount",BridgeSetCriticalSectionSpinCount)
    NATIVE_EXPORT("RtlRunOnceInitialize",BridgeInitOnceInitialize)
    NATIVE_EXPORT("RtlRunOnceBeginInitialize",GuestRtlRunOnceBeginInitialize)
    NATIVE_EXPORT("RtlRunOnceComplete",GuestRtlRunOnceComplete)
    NATIVE_EXPORT("RtlRunOnceExecuteOnce",GuestRtlRunOnceExecuteOnce)
    NATIVE_EXPORT("NtOpenProcessToken",GuestNtOpenProcessToken)
    NATIVE_EXPORT("NtOpenThreadToken",GuestNtOpenThreadToken)
    NATIVE_EXPORT("NtQueryInformationToken",GuestNtQueryInformationToken)
    NATIVE_EXPORT("RtlNtStatusToDosError",GuestRtlNtStatusToDosError)
    NATIVE_EXPORT("LdrLoadDll",GuestLdrLoadDll)
    NATIVE_EXPORT("LdrGetDllHandle",GuestLdrGetDllHandle)
    NATIVE_EXPORT("LdrGetDllHandleEx",GuestLdrGetDllHandleEx)
    NATIVE_EXPORT("LdrAddRefDll",GuestLdrAddRefDll)
    NATIVE_EXPORT("LdrUnloadDll",GuestLdrUnloadDll)
    NATIVE_EXPORT("LdrDisableThreadCalloutsForDll",GuestLdrDisableThreadCalloutsForDll)
    NATIVE_EXPORT("LdrGetProcedureAddress",GuestLdrGetProcedureAddress)
    NATIVE_EXPORT("RtlCreateUnicodeStringFromAsciiz",GuestRtlCreateUnicodeStringFromAsciiz)
    NATIVE_EXPORT("RtlFreeUnicodeString",GuestRtlFreeUnicodeString)
    NATIVE_EXPORT("RtlDosPathNameToNtPathName_U",GuestRtlDosPathNameToNtPathName_U)
    NATIVE_EXPORT("RtlDosPathNameToRelativeNtPathName_U_WithStatus",GuestRtlDosPathNameToRelativeNtPathName_U_WithStatus)
    NATIVE_EXPORT("RtlReleaseRelativeName",GuestRtlReleaseRelativeName)
    NATIVE_EXPORT("RtlFindLeastSignificantBit",GuestRtlFindLeastSignificantBit)
    NATIVE_EXPORT("RtlFreeHeap",GuestRtlFreeHeap)
    NATIVE_EXPORT("NtOpenFile",GuestNtOpenFile)
    NATIVE_EXPORT("ZwOpenFile",GuestNtOpenFile)
    NATIVE_EXPORT("NtReadFile",GuestNtReadFile)
    NATIVE_EXPORT("ZwReadFile",GuestNtReadFile)
    NATIVE_EXPORT("NtWriteFile",GuestNtWriteFile)
    NATIVE_EXPORT("ZwWriteFile",GuestNtWriteFile)
    NATIVE_EXPORT("NtLockFile",GuestNtLockFile)
    NATIVE_EXPORT("ZwLockFile",GuestNtLockFile)
    NATIVE_EXPORT("NtUnlockFile",GuestNtUnlockFile)
    NATIVE_EXPORT("ZwUnlockFile",GuestNtUnlockFile)
    NATIVE_EXPORT("NtCancelIoFile",GuestNtCancelIoFile)
    NATIVE_EXPORT("ZwCancelIoFile",GuestNtCancelIoFile)
    NATIVE_EXPORT("NtCancelIoFileEx",GuestNtCancelIoFileEx)
    NATIVE_EXPORT("ZwCancelIoFileEx",GuestNtCancelIoFileEx)
    NATIVE_EXPORT("NtSetInformationFile",GuestNtSetInformationFile)
    NATIVE_EXPORT("ZwSetInformationFile",GuestNtSetInformationFile)
    NATIVE_EXPORT("NtQueryInformationFile",GuestNtQueryInformationFile)
    NATIVE_EXPORT("ZwQueryInformationFile",GuestNtQueryInformationFile)
    NATIVE_EXPORT("NtFsControlFile",GuestNtFsControlFile)
    NATIVE_EXPORT("NtCancelSynchronousIoFile",GuestNtCancelSynchronousIoFile)
    NATIVE_EXPORT("NtQueryVolumeInformationFile",GuestNtQueryVolumeInformationFile)
    NATIVE_EXPORT("NtQueryInformationProcess",GuestNtQueryInformationProcess)
    NATIVE_EXPORT("NtSetInformationProcess",GuestNtSetInformationProcess)
    {}
#undef NATIVE_EXPORT
    if(result.targetAddress){result.disposition=ImportDisposition::NeedsBridge;result.note=L"Guest NT adapter; unsupported classes/operations return explicit NTSTATUS failures.";}return result;
}
} }
