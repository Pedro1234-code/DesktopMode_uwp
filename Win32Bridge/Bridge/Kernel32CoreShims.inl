namespace Win32Bridge
{
namespace Bridge
{
// Included by Kernel32Shims.cpp. These adapters cover process-neutral
// Kernel32 primitives used by modern desktop applications. Filesystem and
// process APIs remain routed through the guest-specific subsystems.

void WINAPI BridgeAcquireSRWLockExclusive(PSRWLOCK lock)
{
    if (lock) ::AcquireSRWLockExclusive(lock);
}

void WINAPI BridgeReleaseSRWLockExclusive(PSRWLOCK lock)
{
    if (lock) ::ReleaseSRWLockExclusive(lock);
}

BOOLEAN WINAPI BridgeTryAcquireSRWLockExclusive(PSRWLOCK lock)
{
    return lock ? ::TryAcquireSRWLockExclusive(lock) : FALSE;
}

BOOL WINAPI BridgeSleepConditionVariableSRW(
    PCONDITION_VARIABLE condition, PSRWLOCK lock, DWORD milliseconds, ULONG flags)
{
    if (!condition || !lock || (flags & ~CONDITION_VARIABLE_LOCKMODE_SHARED) != 0)
    {
        SetGuestLastError(ERROR_INVALID_PARAMETER);
        return FALSE;
    }
    const BOOL result = ::SleepConditionVariableSRW(condition, lock, milliseconds, flags);
    SetGuestLastError(result ? ERROR_SUCCESS : ::GetLastError());
    return result;
}

void WINAPI BridgeWakeAllConditionVariable(PCONDITION_VARIABLE condition)
{
    if (condition) ::WakeAllConditionVariable(condition);
}

BOOL WINAPI BridgeInitializeCriticalSectionAndSpinCount(
    LPCRITICAL_SECTION section, DWORD spinCount)
{
    if (!section)
    {
        SetGuestLastError(ERROR_INVALID_PARAMETER);
        return FALSE;
    }
    const BOOL result = ::InitializeCriticalSectionAndSpinCount(section, spinCount);
    SetGuestLastError(result ? ERROR_SUCCESS : ERROR_NOT_ENOUGH_MEMORY);
    return result;
}

BOOL WINAPI BridgeInitializeCriticalSectionEx(
    LPCRITICAL_SECTION section, DWORD spinCount, DWORD flags)
{
    if (!section)
    {
        SetGuestLastError(ERROR_INVALID_PARAMETER);
        return FALSE;
    }
    const BOOL result = ::InitializeCriticalSectionEx(section, spinCount, flags);
    SetGuestLastError(result ? ERROR_SUCCESS : ERROR_NOT_ENOUGH_MEMORY);
    return result;
}

BOOL WINAPI BridgeInitOnceBeginInitialize(
    LPINIT_ONCE once, DWORD flags, PBOOL pending, LPVOID* context)
{
    if (!once || !pending)
    {
        SetGuestLastError(ERROR_INVALID_PARAMETER);
        return FALSE;
    }
    const BOOL result = ::InitOnceBeginInitialize(once, flags, pending, context);
    SetGuestLastError(result ? ERROR_SUCCESS : ::GetLastError());
    return result;
}

BOOL WINAPI BridgeInitOnceComplete(
    LPINIT_ONCE once, DWORD flags, LPVOID context)
{
    if (!once)
    {
        SetGuestLastError(ERROR_INVALID_PARAMETER);
        return FALSE;
    }
    const BOOL result = ::InitOnceComplete(once, flags, context);
    SetGuestLastError(result ? ERROR_SUCCESS : ::GetLastError());
    return result;
}

void WINAPI BridgeInitializeSListHead(PSLIST_HEADER head)
{
    if (head) ::InitializeSListHead(head);
}

DWORD WINAPI BridgeTlsAlloc()
{
    const DWORD index = ::TlsAlloc();
    SetGuestLastError(index == TLS_OUT_OF_INDEXES ? ERROR_NOT_ENOUGH_MEMORY : ERROR_SUCCESS);
    return index;
}

BOOL WINAPI BridgeTlsFree(DWORD index)
{
    const BOOL result = ::TlsFree(index);
    SetGuestLastError(result ? ERROR_SUCCESS : ::GetLastError());
    return result;
}

LPVOID WINAPI BridgeTlsGetValue(DWORD index)
{
    ::SetLastError(ERROR_SUCCESS);
    LPVOID value = ::TlsGetValue(index);
    SetGuestLastError(::GetLastError());
    return value;
}

BOOL WINAPI BridgeTlsSetValue(DWORD index, LPVOID value)
{
    const BOOL result = ::TlsSetValue(index, value);
    SetGuestLastError(result ? ERROR_SUCCESS : ::GetLastError());
    return result;
}

DWORD WINAPI BridgeFlsAlloc(PFLS_CALLBACK_FUNCTION callback)
{
    const DWORD index = ::FlsAlloc(callback);
    SetGuestLastError(index == FLS_OUT_OF_INDEXES ? ERROR_NOT_ENOUGH_MEMORY : ERROR_SUCCESS);
    return index;
}

BOOL WINAPI BridgeFlsFree(DWORD index)
{
    const BOOL result = ::FlsFree(index);
    SetGuestLastError(result ? ERROR_SUCCESS : ::GetLastError());
    return result;
}

PVOID WINAPI BridgeFlsGetValue(DWORD index)
{
    ::SetLastError(ERROR_SUCCESS);
    PVOID value = ::FlsGetValue(index);
    SetGuestLastError(::GetLastError());
    return value;
}

BOOL WINAPI BridgeFlsSetValue(DWORD index, PVOID value)
{
    const BOOL result = ::FlsSetValue(index, value);
    SetGuestLastError(result ? ERROR_SUCCESS : ::GetLastError());
    return result;
}

LPVOID WINAPI BridgeHeapReAlloc(
    HANDLE heap, DWORD flags, LPVOID memory, SIZE_T bytes)
{
    if (heap != BridgeGetProcessHeap() || !memory || (flags & ~HEAP_ZERO_MEMORY) != 0)
    {
        SetGuestLastError(ERROR_INVALID_PARAMETER);
        return nullptr;
    }
    LPVOID result = ::HeapReAlloc(::GetProcessHeap(), flags, memory, bytes);
    SetGuestLastError(result ? ERROR_SUCCESS : ERROR_NOT_ENOUGH_MEMORY);
    return result;
}

SIZE_T WINAPI BridgeHeapSize(
    HANDLE heap, DWORD flags, LPCVOID memory)
{
    if (heap != BridgeGetProcessHeap() || flags != 0 || !memory)
    {
        SetGuestLastError(ERROR_INVALID_PARAMETER);
        return static_cast<SIZE_T>(-1);
    }
    const SIZE_T result = ::HeapSize(::GetProcessHeap(), 0, memory);
    SetGuestLastError(result == static_cast<SIZE_T>(-1) ? ERROR_INVALID_PARAMETER : ERROR_SUCCESS);
    return result;
}

PVOID WINAPI BridgeEncodePointer(PVOID pointer)
{
    return ::EncodePointer(pointer);
}

PVOID WINAPI BridgeDecodePointer(PVOID pointer)
{
    return ::DecodePointer(pointer);
}

DWORD WINAPI BridgeWaitForSingleObjectEx(
    HANDLE object, DWORD milliseconds, BOOL alertable)
{
    // APC delivery is not yet modeled, but timeout and object acquisition use
    // the same guest handle table as WaitForSingleObject.
    UNREFERENCED_PARAMETER(alertable);
    return BridgeWaitForSingleObject(object, milliseconds);
}

DWORD WINAPI BridgeSleepEx(DWORD milliseconds, BOOL alertable)
{
    UNREFERENCED_PARAMETER(alertable);
    BridgeSleep(milliseconds);
    return 0;
}

BOOL WINAPI BridgeAreFileApisANSI()
{
    return TRUE;
}

void WINAPI BridgeGetNativeSystemInfo(LPSYSTEM_INFO information)
{
    BridgeGetSystemInfo(information);
}

BOOL WINAPI BridgeGetProductInfo(
    DWORD, DWORD, DWORD, DWORD, PDWORD productType)
{
    if (!productType)
    {
        SetGuestLastError(ERROR_INVALID_PARAMETER);
        return FALSE;
    }
    *productType = PRODUCT_PROFESSIONAL;
    SetGuestLastError(ERROR_SUCCESS);
    return TRUE;
}

LCID WINAPI BridgeGetUserDefaultLCID()
{
    return MAKELCID(BridgeGetUserDefaultLangID(), SORT_DEFAULT);
}

BOOL WINAPI BridgeIsValidCodePage(UINT codePage)
{
    return ::IsValidCodePage(codePage);
}

BOOL WINAPI BridgeIsValidLocale(LCID locale, DWORD flags)
{
    if (flags != LCID_INSTALLED && flags != LCID_SUPPORTED) return FALSE;
    const LANGID language = LANGIDFROMLCID(locale);
    return language == 0x0409 || language == 0x0416 || language == LANG_NEUTRAL;
}

int WINAPI BridgeCompareStringOrdinal(
    LPCWCH first, int firstCount, LPCWCH second, int secondCount, BOOL ignoreCase)
{
    return ::CompareStringOrdinal(first, firstCount, second, secondCount, ignoreCase);
}

int WINAPI BridgeCompareStringW(
    LCID locale, DWORD flags, PCNZWCH first, int firstCount,
    PCNZWCH second, int secondCount)
{
    return ::CompareStringW(locale, flags, first, firstCount, second, secondCount);
}

int WINAPI BridgeCompareStringEx(
    LPCWSTR localeName, DWORD flags, LPCWCH first, int firstCount,
    LPCWCH second, int secondCount, LPNLSVERSIONINFO version,
    LPVOID reserved, LPARAM parameter)
{
    return ::CompareStringEx(localeName, flags, first, firstCount,
        second, secondCount, version, reserved, parameter);
}

int WINAPI BridgeLCMapStringW(
    LCID locale, DWORD flags, LPCWCH source, int sourceCount,
    LPWSTR destination, int destinationCount)
{
    UNREFERENCED_PARAMETER(locale);
    if (!source || sourceCount == 0) { SetGuestLastError(ERROR_INVALID_PARAMETER); return 0; }
    const int count = sourceCount < 0 ? static_cast<int>(wcslen(source)) + 1 : sourceCount;
    if (!destination || destinationCount == 0) return count;
    if (destinationCount < count) { SetGuestLastError(ERROR_INSUFFICIENT_BUFFER); return 0; }
    for (int index = 0; index < count; ++index)
    {
        wchar_t value = source[index];
        if ((flags & LCMAP_UPPERCASE) != 0) value = towupper(value);
        else if ((flags & LCMAP_LOWERCASE) != 0) value = towlower(value);
        destination[index] = value;
    }
    SetGuestLastError(ERROR_SUCCESS);
    return count;
}

int WINAPI BridgeLCMapStringA(
    LCID locale, DWORD flags, LPCSTR source, int sourceCount,
    LPSTR destination, int destinationCount)
{
    UNREFERENCED_PARAMETER(locale);
    if (!source || sourceCount == 0) { SetGuestLastError(ERROR_INVALID_PARAMETER); return 0; }
    const int count = sourceCount < 0 ? static_cast<int>(strlen(source)) + 1 : sourceCount;
    if (!destination || destinationCount == 0) return count;
    if (destinationCount < count) { SetGuestLastError(ERROR_INSUFFICIENT_BUFFER); return 0; }
    for (int index = 0; index < count; ++index)
    {
        unsigned char value = static_cast<unsigned char>(source[index]);
        if ((flags & LCMAP_UPPERCASE) != 0) value = static_cast<unsigned char>(toupper(value));
        else if ((flags & LCMAP_LOWERCASE) != 0) value = static_cast<unsigned char>(tolower(value));
        destination[index] = static_cast<char>(value);
    }
    SetGuestLastError(ERROR_SUCCESS);
    return count;
}

int WINAPI BridgeLCMapStringEx(
    LPCWSTR localeName, DWORD flags, LPCWCH source, int sourceCount,
    LPWSTR destination, int destinationCount, LPNLSVERSIONINFO version,
    LPVOID reserved, LPARAM parameter)
{
    return ::LCMapStringEx(localeName, flags, source, sourceCount,
        destination, destinationCount, version, reserved, parameter);
}

int WINAPI BridgeLstrcmpiA(LPCSTR first, LPCSTR second)
{
    const int result = _stricmp(first ? first : "", second ? second : "");
    return result < 0 ? -1 : result > 0 ? 1 : 0;
}

LPWSTR WINAPI BridgeLstrcpyW(LPWSTR destination, LPCWSTR source)
{
    if (!destination || !source) return nullptr;
#pragma warning(suppress : 4996)
    return wcscpy(destination, source);
}

LPWSTR WINAPI BridgeLstrcpynW(
    LPWSTR destination, LPCWSTR source, int maximumLength)
{
    if (!destination || !source || maximumLength <= 0) return nullptr;
    wcsncpy_s(destination, static_cast<size_t>(maximumLength), source,
        static_cast<size_t>(maximumLength - 1));
    return destination;
}

LPSTR WINAPI BridgeLstrcpynA(
    LPSTR destination, LPCSTR source, int maximumLength)
{
    if (!destination || !source || maximumLength <= 0) return nullptr;
    strncpy_s(destination, static_cast<size_t>(maximumLength), source,
        static_cast<size_t>(maximumLength - 1));
    return destination;
}

UINT WINAPI BridgeGetConsoleOutputCP()
{
    return BridgeGetOEMCP();
}

BOOL WINAPI BridgeGetCPInfo(UINT codePage, LPCPINFO information)
{
    if (!information)
    {
        SetGuestLastError(ERROR_INVALID_PARAMETER);
        return FALSE;
    }
    const BOOL result = ::GetCPInfo(codePage, information);
    SetGuestLastError(result ? ERROR_SUCCESS : ::GetLastError());
    return result;
}

namespace
{
    struct GuestThreadpoolWork final
    {
        PTP_WORK_CALLBACK callback = nullptr;
        PVOID context = nullptr;
    };

    struct GuestThreadpoolInvocation final
    {
        std::shared_ptr<GuestThreadpoolWork> work;
        PTP_WORK handle = nullptr;
    };

    DWORD WINAPI InvokeGuestThreadpoolWork(PVOID parameter)
    {
        std::unique_ptr<GuestThreadpoolInvocation> invocation(
            static_cast<GuestThreadpoolInvocation*>(parameter));
        invocation->work->callback(
            nullptr, invocation->work->context, invocation->handle);
        return ERROR_SUCCESS;
    }

    std::mutex g_guestThreadpoolLock;
    std::unordered_map<GuestThreadpoolWork*, std::shared_ptr<GuestThreadpoolWork>>
        g_guestThreadpoolWork;

    thread_local std::vector<char> g_guestCommandLineA;
    thread_local std::vector<wchar_t> g_guestEnvironmentBlock;

    void BuildGuestEnvironmentBlock()
    {
        static const wchar_t values[] =
            L"ALLUSERSPROFILE=C:\\ProgramData\0"
            L"APPDATA=C:\\Users\\Default\\AppData\\Roaming\0"
            L"HOMEDRIVE=C:\0"
            L"HOMEPATH=\\Users\\Default\0"
            L"LOCALAPPDATA=C:\\Users\\Default\\AppData\\Local\0"
            L"OS=Windows_NT\0"
            L"PROGRAMDATA=C:\\ProgramData\0"
            L"PROGRAMFILES=C:\\Program Files\0"
            L"SYSTEMDRIVE=C:\0"
            L"SYSTEMROOT=C:\\Windows\0"
            L"TEMP=C:\\Users\\Default\\AppData\\Local\\Temp\0"
            L"TMP=C:\\Users\\Default\\AppData\\Local\\Temp\0"
            L"USERPROFILE=C:\\Users\\Default\0"
            L"WINDIR=C:\\Windows\0\0";
        g_guestEnvironmentBlock.assign(values, values + ARRAYSIZE(values));
    }
}

LPSTR WINAPI BridgeGetCommandLineA()
{
    LPCWSTR wide = BridgeGetCommandLineW();
    const int required = ::WideCharToMultiByte(
        CP_ACP, 0, wide ? wide : L"", -1, nullptr, 0, nullptr, nullptr);
    if (required <= 0) return nullptr;
    g_guestCommandLineA.resize(static_cast<size_t>(required));
    if (::WideCharToMultiByte(CP_ACP, 0, wide ? wide : L"", -1,
        g_guestCommandLineA.data(), required, nullptr, nullptr) != required)
        return nullptr;
    return g_guestCommandLineA.data();
}

LPWCH WINAPI BridgeGetEnvironmentStringsW()
{
    BuildGuestEnvironmentBlock();
    const size_t bytes = g_guestEnvironmentBlock.size() * sizeof(wchar_t);
    LPWCH result = static_cast<LPWCH>(::HeapAlloc(::GetProcessHeap(), 0, bytes));
    if (!result)
    {
        SetGuestLastError(ERROR_NOT_ENOUGH_MEMORY);
        return nullptr;
    }
    memcpy(result, g_guestEnvironmentBlock.data(), bytes);
    SetGuestLastError(ERROR_SUCCESS);
    return result;
}

BOOL WINAPI BridgeFreeEnvironmentStringsW(LPWCH environment)
{
    if (!environment)
    {
        SetGuestLastError(ERROR_INVALID_PARAMETER);
        return FALSE;
    }
    const BOOL result = ::HeapFree(::GetProcessHeap(), 0, environment);
    SetGuestLastError(result ? ERROR_SUCCESS : ERROR_INVALID_PARAMETER);
    return result;
}

BOOL WINAPI BridgeSetEnvironmentVariableW(LPCWSTR name, LPCWSTR)
{
    if (!name || !*name || wcschr(name, L'='))
    {
        SetGuestLastError(ERROR_INVALID_PARAMETER);
        return FALSE;
    }
    // Environment mutation is process-local. The base block is rebuilt for
    // each query; retaining arbitrary overrides belongs in GuestRuntime.
    SetGuestLastError(ERROR_NOT_SUPPORTED);
    return FALSE;
}

PTP_WORK WINAPI BridgeCreateThreadpoolWork(
    PTP_WORK_CALLBACK callback, PVOID context, PTP_CALLBACK_ENVIRON)
{
    if (!callback)
    {
        SetGuestLastError(ERROR_INVALID_PARAMETER);
        return nullptr;
    }
    std::shared_ptr<GuestThreadpoolWork> work;
    try
    {
        work = std::make_shared<GuestThreadpoolWork>();
        work->callback = callback;
        work->context = context;
        std::lock_guard<std::mutex> guard(g_guestThreadpoolLock);
        g_guestThreadpoolWork.emplace(work.get(), work);
    }
    catch (const std::bad_alloc&)
    {
        SetGuestLastError(ERROR_NOT_ENOUGH_MEMORY);
        return nullptr;
    }
    SetGuestLastError(ERROR_SUCCESS);
    return reinterpret_cast<PTP_WORK>(work.get());
}

void WINAPI BridgeSubmitThreadpoolWork(PTP_WORK handle)
{
    std::shared_ptr<GuestThreadpoolWork> work;
    {
        std::lock_guard<std::mutex> guard(g_guestThreadpoolLock);
        const auto found = g_guestThreadpoolWork.find(
            reinterpret_cast<GuestThreadpoolWork*>(handle));
        if (found == g_guestThreadpoolWork.end()) return;
        work = found->second;
    }
    std::unique_ptr<GuestThreadpoolInvocation> invocation;
    try
    {
        invocation = std::make_unique<GuestThreadpoolInvocation>();
        invocation->work = work;
        invocation->handle = handle;
    }
    catch (const std::bad_alloc&)
    {
        SetGuestLastError(ERROR_NOT_ENOUGH_MEMORY);
        return;
    }
    HANDLE thread = BridgeCreateThread(
        nullptr, 0, &InvokeGuestThreadpoolWork, invocation.get(), 0, nullptr);
    if (!thread) return;
    invocation.release();
    BridgeCloseHandle(thread);
}

void WINAPI BridgeCloseThreadpoolWork(PTP_WORK handle)
{
    std::lock_guard<std::mutex> guard(g_guestThreadpoolLock);
    g_guestThreadpoolWork.erase(reinterpret_cast<GuestThreadpoolWork*>(handle));
}

void WINAPI BridgeFreeLibraryWhenCallbackReturns(
    PTP_CALLBACK_INSTANCE, HMODULE)
{
    // Callback cleanup groups are not exposed yet. Keeping the module loaded
    // is safer than releasing code while its callback is still executing.
}

HRESULT WINAPI BridgeRegisterApplicationRestart(
    PCWSTR, DWORD)
{
    return S_OK;
}

HRESULT WINAPI BridgeUnregisterApplicationRestart()
{
    return S_OK;
}

HRESULT WINAPI BridgeGetApplicationRestartSettings(
    HANDLE, PWSTR commandLine, PDWORD characterCount, PDWORD flags)
{
    if (flags) *flags = 0;
    if (characterCount && commandLine && *characterCount) commandLine[0] = L'\0';
    return HRESULT_FROM_WIN32(ERROR_NOT_FOUND);
}

DWORD WINAPI BridgeQueueUserAPC(
    PAPCFUNC, HANDLE, ULONG_PTR)
{
    SetGuestLastError(ERROR_NOT_SUPPORTED);
    return 0;
}

BOOL WINAPI BridgeTerminateThread(HANDLE, DWORD)
{
    SetGuestLastError(ERROR_NOT_SUPPORTED);
    return FALSE;
}

BOOL WINAPI BridgeSetStdHandle(DWORD, HANDLE)
{
    // GUI guests intentionally have no inherited console handles.
    SetGuestLastError(ERROR_SUCCESS);
    return TRUE;
}

BOOL WINAPI BridgeCancelIo(HANDLE)
{
    SetGuestLastError(ERROR_NOT_FOUND);
    return FALSE;
}

BOOL WINAPI BridgeReadDirectoryChangesW(
    HANDLE, LPVOID, DWORD, BOOL, DWORD, LPDWORD bytesReturned,
    LPOVERLAPPED, LPOVERLAPPED_COMPLETION_ROUTINE)
{
    if (bytesReturned) *bytesReturned = 0;
    SetGuestLastError(ERROR_NOT_SUPPORTED);
    return FALSE;
}

BOOL WINAPI BridgeReadConsoleW(
    HANDLE, LPVOID, DWORD, LPDWORD read, LPVOID)
{
    if (read) *read = 0;
    SetGuestLastError(ERROR_INVALID_HANDLE);
    return FALSE;
}

BOOL WINAPI BridgeWriteConsoleW(
    HANDLE, const VOID*, DWORD, LPDWORD written, LPVOID)
{
    if (written) *written = 0;
    SetGuestLastError(ERROR_INVALID_HANDLE);
    return FALSE;
}

BOOL WINAPI BridgeCopyFileW(
    LPCWSTR existingFileName, LPCWSTR newFileName, BOOL failIfExists)
{
    constexpr DWORD CopyFileFailIfExists = 0x00000001;
    return BridgeCopyFileExW(existingFileName, newFileName, nullptr, nullptr,
        nullptr, failIfExists ? CopyFileFailIfExists : 0);
}

HANDLE WINAPI BridgeFindFirstFileExW(
    LPCWSTR pattern, FINDEX_INFO_LEVELS informationLevel, LPVOID findData,
    FINDEX_SEARCH_OPS searchOperation, LPVOID searchFilter, DWORD flags)
{
    if (!findData || informationLevel > FindExInfoBasic ||
        searchOperation != FindExSearchNameMatch || searchFilter ||
        (flags & ~static_cast<DWORD>(FIND_FIRST_EX_LARGE_FETCH)) != 0)
    {
        SetGuestLastError(ERROR_NOT_SUPPORTED);
        return INVALID_HANDLE_VALUE;
    }
    return BridgeFindFirstFileW(pattern, static_cast<LPWIN32_FIND_DATAW>(findData));
}

BOOL WINAPI BridgeGetFileAttributesExW(
    LPCWSTR fileName, GET_FILEEX_INFO_LEVELS informationLevel, LPVOID information)
{
    if (!fileName || !information || informationLevel != GetFileExInfoStandard)
    {
        SetGuestLastError(ERROR_INVALID_PARAMETER);
        return FALSE;
    }
    WIN32_FIND_DATAW data{};
    HANDLE find = BridgeFindFirstFileW(fileName, &data);
    if (find == INVALID_HANDLE_VALUE) return FALSE;
    BridgeFindClose(find);
    WIN32_FILE_ATTRIBUTE_DATA result{};
    result.dwFileAttributes = data.dwFileAttributes;
    result.ftCreationTime = data.ftCreationTime;
    result.ftLastAccessTime = data.ftLastAccessTime;
    result.ftLastWriteTime = data.ftLastWriteTime;
    result.nFileSizeHigh = data.nFileSizeHigh;
    result.nFileSizeLow = data.nFileSizeLow;
    *static_cast<WIN32_FILE_ATTRIBUTE_DATA*>(information) = result;
    SetGuestLastError(ERROR_SUCCESS);
    return TRUE;
}

DWORD WINAPI BridgeGetFileType(HANDLE file)
{
    LARGE_INTEGER size{};
    if (BridgeGetFileSizeEx(file, &size))
    {
        SetGuestLastError(ERROR_SUCCESS);
        return FILE_TYPE_DISK;
    }
    SetGuestLastError(ERROR_INVALID_HANDLE);
    return FILE_TYPE_UNKNOWN;
}

DWORD WINAPI BridgeGetFinalPathNameByHandleW(
    HANDLE file, LPWSTR path, DWORD characterCount, DWORD flags)
{
    constexpr DWORD SupportedFlags = FILE_NAME_NORMALIZED | VOLUME_NAME_DOS;
    if ((flags & ~SupportedFlags) != 0)
    {
        SetGuestLastError(ERROR_NOT_SUPPORTED);
        return 0;
    }
    GuestStorageContext* storage = CurrentGuestStorageContext();
    std::wstring canonical;
    DWORD error = ERROR_SUCCESS;
    if (!storage || !storage->GetFilePath(file, &canonical, &error))
    {
        SetGuestLastError(storage ? error : ERROR_INVALID_FUNCTION);
        return 0;
    }
    const DWORD required = static_cast<DWORD>(canonical.size());
    if (!path || characterCount <= required)
    {
        SetGuestLastError(ERROR_INSUFFICIENT_BUFFER);
        return required + 1;
    }
    memcpy(path, canonical.c_str(), (canonical.size() + 1) * sizeof(wchar_t));
    SetGuestLastError(ERROR_SUCCESS);
    return required;
}

BOOL WINAPI BridgeGetFileInformationByHandleEx(
    HANDLE file, FILE_INFO_BY_HANDLE_CLASS informationClass,
    LPVOID information, DWORD informationSize)
{
    if (!information)
    {
        SetGuestLastError(ERROR_INVALID_PARAMETER);
        return FALSE;
    }
    BY_HANDLE_FILE_INFORMATION source{};
    if (!BridgeGetFileInformationByHandle(file, &source)) return FALSE;
    if (informationClass == FileBasicInfo && informationSize >= sizeof(FILE_BASIC_INFO))
    {
        FILE_BASIC_INFO result{};
        result.CreationTime.LowPart = source.ftCreationTime.dwLowDateTime;
        result.CreationTime.HighPart = source.ftCreationTime.dwHighDateTime;
        result.LastAccessTime.LowPart = source.ftLastAccessTime.dwLowDateTime;
        result.LastAccessTime.HighPart = source.ftLastAccessTime.dwHighDateTime;
        result.LastWriteTime.LowPart = source.ftLastWriteTime.dwLowDateTime;
        result.LastWriteTime.HighPart = source.ftLastWriteTime.dwHighDateTime;
        result.ChangeTime = result.LastWriteTime;
        result.FileAttributes = source.dwFileAttributes;
        *static_cast<FILE_BASIC_INFO*>(information) = result;
        return TRUE;
    }
    if (informationClass == FileStandardInfo && informationSize >= sizeof(FILE_STANDARD_INFO))
    {
        FILE_STANDARD_INFO result{};
        result.EndOfFile.HighPart = source.nFileSizeHigh;
        result.EndOfFile.LowPart = source.nFileSizeLow;
        result.AllocationSize = result.EndOfFile;
        result.NumberOfLinks = source.nNumberOfLinks;
        result.DeletePending = FALSE;
        result.Directory = (source.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
        *static_cast<FILE_STANDARD_INFO*>(information) = result;
        return TRUE;
    }
    SetGuestLastError(ERROR_NOT_SUPPORTED);
    return FALSE;
}

DWORD WINAPI BridgeGetLongPathNameW(
    LPCWSTR shortPath, LPWSTR longPath, DWORD characterCount)
{
    return BridgeGetFullPathNameW(shortPath, characterCount, longPath, nullptr);
}

BOOL WINAPI BridgeReplaceFileW(
    LPCWSTR replaced, LPCWSTR replacement, LPCWSTR backup,
    DWORD, LPVOID, LPVOID)
{
    if (!replaced || !replacement)
    {
        SetGuestLastError(ERROR_INVALID_PARAMETER);
        return FALSE;
    }
    if (backup && *backup && !BridgeCopyFileW(replaced, backup, FALSE)) return FALSE;
    return BridgeMoveFileExW(replacement, replaced, MOVEFILE_REPLACE_EXISTING);
}

BOOL WINAPI BridgeQueryFullProcessImageNameW(
    HANDLE process, DWORD flags, LPWSTR path, PDWORD characterCount)
{
    if (process != BridgeGetCurrentProcess() || flags > 1 || !path || !characterCount)
    {
        SetGuestLastError(ERROR_INVALID_PARAMETER);
        return FALSE;
    }
    GuestStorageContext* storage = CurrentGuestStorageContext();
    if (!storage)
    {
        SetGuestLastError(ERROR_INVALID_FUNCTION);
        return FALSE;
    }
    const std::wstring& modulePath = storage->ModulePath();
    if (*characterCount <= modulePath.size())
    {
        *characterCount = static_cast<DWORD>(modulePath.size() + 1);
        SetGuestLastError(ERROR_INSUFFICIENT_BUFFER);
        return FALSE;
    }
    memcpy(path, modulePath.c_str(), (modulePath.size() + 1) * sizeof(wchar_t));
    *characterCount = static_cast<DWORD>(modulePath.size());
    SetGuestLastError(ERROR_SUCCESS);
    return TRUE;
}

BOOL WINAPI BridgeGetModuleHandleExW(
    DWORD flags, LPCWSTR moduleName, HMODULE* module)
{
    constexpr DWORD FromAddress = 0x00000004;
    constexpr DWORD UnchangedRefcount = 0x00000002;
    constexpr DWORD Pin = 0x00000001;
    if (!module || (flags & ~(FromAddress | UnchangedRefcount | Pin)) != 0)
    {
        SetGuestLastError(ERROR_INVALID_PARAMETER);
        return FALSE;
    }
    if ((flags & FromAddress) != 0)
    {
        MEMORY_BASIC_INFORMATION memory{};
        if (!moduleName || !::VirtualQuery(moduleName, &memory, sizeof(memory)))
        {
            SetGuestLastError(ERROR_MOD_NOT_FOUND);
            return FALSE;
        }
        *module = reinterpret_cast<HMODULE>(memory.AllocationBase);
        SetGuestLastError(ERROR_SUCCESS);
        return TRUE;
    }
    *module = BridgeGetModuleHandleW(moduleName);
    return *module != nullptr;
}

int WINAPI BridgeGetLocaleInfoA(
    LCID locale, LCTYPE type, LPSTR data, int characterCount)
{
    return ::GetLocaleInfoA(locale, type, data, characterCount);
}

int WINAPI BridgeGetLocaleInfoEx(
    LPCWSTR localeName, LCTYPE type, LPWSTR data, int characterCount)
{
    return ::GetLocaleInfoEx(localeName, type, data, characterCount);
}

BOOL WINAPI BridgeGetStringTypeW(
    DWORD type, LPCWCH source, int count, LPWORD result)
{
    return ::GetStringTypeW(type, source, count, result);
}

BOOL WINAPI BridgeGetStringTypeExW(
    LCID locale, DWORD type, LPCWCH source, int count, LPWORD result)
{
    return ::GetStringTypeExW(locale, type, source, count, result);
}

BOOL WINAPI BridgeGetStringTypeExA(
    LCID locale, DWORD type, LPCSTR source, int count, LPWORD result)
{
    return ::GetStringTypeExA(locale, type, source, count, result);
}

int WINAPI BridgeGetDateFormatEx(
    LPCWSTR localeName, DWORD flags, const SYSTEMTIME* date, LPCWSTR format,
    LPWSTR buffer, int characterCount, LPCWSTR calendar)
{
    return ::GetDateFormatEx(localeName, flags, date, format,
        buffer, characterCount, calendar);
}

int WINAPI BridgeGetTimeFormatEx(
    LPCWSTR localeName, DWORD flags, const SYSTEMTIME* time, LPCWSTR format,
    LPWSTR buffer, int characterCount)
{
    return ::GetTimeFormatEx(localeName, flags, time, format, buffer, characterCount);
}

DWORD WINAPI BridgeGetTimeZoneInformation(
    LPTIME_ZONE_INFORMATION information)
{
    if (!information)
    {
        SetGuestLastError(ERROR_INVALID_PARAMETER);
        return TIME_ZONE_ID_INVALID;
    }
    return ::GetTimeZoneInformation(information);
}

BOOL WINAPI BridgeSystemTimeToTzSpecificLocalTime(
    const TIME_ZONE_INFORMATION* zone, const SYSTEMTIME* universal,
    LPSYSTEMTIME local)
{
    return ::SystemTimeToTzSpecificLocalTime(zone, universal, local);
}

BOOL WINAPI BridgeEnumSystemLocalesW(
    LOCALE_ENUMPROCW callback, DWORD flags)
{
    UNREFERENCED_PARAMETER(flags);
    if (!callback) { SetGuestLastError(ERROR_INVALID_PARAMETER); return FALSE; }
    wchar_t english[] = L"00000409";
    wchar_t portuguese[] = L"00000416";
    if (!callback(english)) return TRUE;
    callback(portuguese);
    SetGuestLastError(ERROR_SUCCESS);
    return TRUE;
}

DWORD WINAPI BridgeFormatMessageA(
    DWORD flags, LPCVOID source, DWORD messageId, DWORD languageId,
    LPSTR buffer, DWORD size, va_list* arguments)
{
    return ::FormatMessageA(flags, source, messageId, languageId,
        buffer, size, arguments);
}

PVOID WINAPI BridgeRtlPcToFileHeader(PVOID pc, PVOID* base)
{
    MEMORY_BASIC_INFORMATION memory{};
    if (!pc || !base || !::VirtualQuery(pc, &memory, sizeof(memory))) return nullptr;
    *base = memory.AllocationBase;
    return memory.AllocationBase;
}

void WINAPI BridgeRtlUnwind(
    PVOID, PVOID, PEXCEPTION_RECORD record, PVOID)
{
    if (record) ::RaiseException(record->ExceptionCode,
        record->ExceptionFlags, record->NumberParameters,
        record->ExceptionInformation);
}

void WINAPI BridgeRtlUnwindEx(
    PVOID, PVOID, PEXCEPTION_RECORD record, PVOID,
    PCONTEXT, PUNWIND_HISTORY_TABLE)
{
    if (record) ::RaiseException(record->ExceptionCode,
        record->ExceptionFlags, record->NumberParameters,
        record->ExceptionInformation);
}

void WINAPI BridgeExitProcess(UINT exitCode)
{
    const ULONG_PTR argument = exitCode;
    ::RaiseException(0xE0424242u, EXCEPTION_NONCONTINUABLE, 1, &argument);
}

void WINAPI BridgeExitThread(DWORD exitCode)
{
    const ULONG_PTR arguments[2] = { exitCode, 1 };
    ::RaiseException(0xE0424242u, EXCEPTION_NONCONTINUABLE, 2, arguments);
}

}
}
