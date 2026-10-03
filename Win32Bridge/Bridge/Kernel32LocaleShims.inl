// Included by Kernel32Shims.cpp so these adapters share its guest last-error
// state without exporting host KERNEL32 entry points directly to a guest.
namespace
{
    int CopyKernelLocaleText(const std::wstring& text, LPWSTR buffer, int characterCount)
    {
        const int required = static_cast<int>(text.size() + 1);
        if (!buffer || characterCount == 0) return required;
        if (characterCount < required)
        {
            SetGuestLastError(ERROR_INSUFFICIENT_BUFFER);
            return 0;
        }
        memcpy(buffer, text.c_str(), static_cast<size_t>(required) * sizeof(wchar_t));
        SetGuestLastError(ERROR_SUCCESS);
        return required;
    }

    std::wstring KernelTwoDigits(WORD value)
    {
        wchar_t result[3]{};
        swprintf_s(result, L"%02u", static_cast<unsigned>(value));
        return result;
    }

    void ReplaceKernelFormatToken(
        std::wstring* output,
        const wchar_t* token,
        const std::wstring& value)
    {
        size_t offset = 0;
        const size_t tokenLength = wcslen(token);
        while ((offset = output->find(token, offset)) != std::wstring::npos)
        {
            output->replace(offset, tokenLength, value);
            offset += value.size();
        }
    }
}

void WINAPI Win32Bridge::Bridge::BridgeGetLocalTime(LPSYSTEMTIME systemTime)
{
    if (!systemTime) { SetGuestLastError(ERROR_INVALID_PARAMETER); return; }
    ::GetLocalTime(systemTime);
    SetGuestLastError(ERROR_SUCCESS);
}

int WINAPI Win32Bridge::Bridge::BridgeGetDateFormatW(
    LCID, DWORD, const SYSTEMTIME* date, LPCWSTR format, LPWSTR buffer, int characterCount)
{
    SYSTEMTIME current = {};
    if (!date) { BridgeGetLocalTime(&current); date = &current; }
    std::wstring output = format ? format : L"MM/dd/yyyy";
    ReplaceKernelFormatToken(&output, L"yyyy", std::to_wstring(date->wYear));
    ReplaceKernelFormatToken(&output, L"MM", KernelTwoDigits(date->wMonth));
    ReplaceKernelFormatToken(&output, L"dd", KernelTwoDigits(date->wDay));
    return CopyKernelLocaleText(output, buffer, characterCount);
}

int WINAPI Win32Bridge::Bridge::BridgeGetTimeFormatW(
    LCID, DWORD, const SYSTEMTIME* time, LPCWSTR format, LPWSTR buffer, int characterCount)
{
    SYSTEMTIME current = {};
    if (!time) { BridgeGetLocalTime(&current); time = &current; }
    std::wstring output = format ? format : L"HH:mm:ss";
    ReplaceKernelFormatToken(&output, L"HH", KernelTwoDigits(time->wHour));
    ReplaceKernelFormatToken(&output, L"mm", KernelTwoDigits(time->wMinute));
    ReplaceKernelFormatToken(&output, L"ss", KernelTwoDigits(time->wSecond));
    return CopyKernelLocaleText(output, buffer, characterCount);
}

LANGID WINAPI Win32Bridge::Bridge::BridgeGetUserDefaultUILanguage()
{
    return BridgeGetUserDefaultLangID();
}

int WINAPI Win32Bridge::Bridge::BridgeFindNLSString(
    LCID, DWORD flags, LPCWSTR source, int sourceCount, LPCWSTR value,
    int valueCount, LPINT foundCount)
{
    if (!source || !value) { SetGuestLastError(ERROR_INVALID_PARAMETER); return -1; }
    const size_t sourceLength = sourceCount < 0 ? wcslen(source) : static_cast<size_t>(sourceCount);
    const size_t valueLength = valueCount < 0 ? wcslen(value) : static_cast<size_t>(valueCount);
    if (valueLength == 0 || valueLength > sourceLength)
    {
        if (foundCount) *foundCount = 0;
        return -1;
    }
    const bool ignoreCase = (flags & NORM_IGNORECASE) != 0;
    const auto equalAt = [&](size_t offset)
    {
        for (size_t index = 0; index < valueLength; ++index)
        {
            wchar_t left = source[offset + index];
            wchar_t right = value[index];
            if (ignoreCase)
            {
                left = static_cast<wchar_t>(towlower(left));
                right = static_cast<wchar_t>(towlower(right));
            }
            if (left != right) return false;
        }
        return true;
    };
    if ((flags & FIND_FROMEND) != 0)
    {
        for (size_t offset = sourceLength - valueLength + 1; offset-- > 0;)
            if (equalAt(offset)) { if (foundCount) *foundCount = static_cast<int>(valueLength); return static_cast<int>(offset); }
    }
    else
    {
        for (size_t offset = 0; offset + valueLength <= sourceLength; ++offset)
            if (equalAt(offset)) { if (foundCount) *foundCount = static_cast<int>(valueLength); return static_cast<int>(offset); }
    }
    if (foundCount) *foundCount = 0;
    return -1;
}

int WINAPI Win32Bridge::Bridge::BridgeLstrcmpW(LPCWSTR left, LPCWSTR right)
{
    const int result = wcscmp(left ? left : L"", right ? right : L"");
    return result < 0 ? -1 : result > 0 ? 1 : 0;
}

int WINAPI Win32Bridge::Bridge::BridgeLstrcmpiW(LPCWSTR left, LPCWSTR right)
{
    const int result = _wcsicmp(left ? left : L"", right ? right : L"");
    return result < 0 ? -1 : result > 0 ? 1 : 0;
}

BOOL WINAPI Win32Bridge::Bridge::BridgeGetVersionExW(LPOSVERSIONINFOW information)
{
    if (!information || information->dwOSVersionInfoSize < sizeof(OSVERSIONINFOW))
    {
        SetGuestLastError(ERROR_INSUFFICIENT_BUFFER);
        return FALSE;
    }
    const DWORD size = information->dwOSVersionInfoSize;
    ZeroMemory(information, size);
    information->dwOSVersionInfoSize = size;
    information->dwMajorVersion = 10;
    information->dwMinorVersion = 0;
    information->dwBuildNumber = 19045;
    information->dwPlatformId = VER_PLATFORM_WIN32_NT;
    return TRUE;
}

BOOL WINAPI Win32Bridge::Bridge::BridgeWow64DisableWow64FsRedirection(PVOID* oldValue)
{
    if (!oldValue) { SetGuestLastError(ERROR_INVALID_PARAMETER); return FALSE; }
    *oldValue = nullptr;
    SetGuestLastError(ERROR_SUCCESS);
    return TRUE;
}

BOOL WINAPI Win32Bridge::Bridge::BridgeWow64RevertWow64FsRedirection(PVOID)
{
    SetGuestLastError(ERROR_SUCCESS);
    return TRUE;
}

void WINAPI Win32Bridge::Bridge::BridgeFreeLibraryAndExitThread(HMODULE module, DWORD exitCode)
{
    BridgeFreeLibrary(module);
    ::ExitThread(exitCode);
}

BOOL WINAPI Win32Bridge::Bridge::BridgeIsWow64Process(HANDLE, PBOOL wow64Process)
{
    if (!wow64Process) { SetGuestLastError(ERROR_INVALID_PARAMETER); return FALSE; }
    *wow64Process = FALSE;
    SetGuestLastError(ERROR_SUCCESS);
    return TRUE;
}

HLOCAL WINAPI Win32Bridge::Bridge::BridgeLocalReAlloc(HLOCAL memory, SIZE_T bytes, UINT flags)
{
    if (!memory) return BridgeLocalAlloc(flags, bytes);
    const DWORD heapFlags = (flags & LMEM_ZEROINIT) != 0 ? HEAP_ZERO_MEMORY : 0;
    HLOCAL result = static_cast<HLOCAL>(::HeapReAlloc(::GetProcessHeap(), heapFlags, memory, bytes));
    SetGuestLastError(result ? ERROR_SUCCESS : ERROR_NOT_ENOUGH_MEMORY);
    return result;
}

SIZE_T WINAPI Win32Bridge::Bridge::BridgeLocalSize(HLOCAL memory)
{
    return BridgeGlobalSize(reinterpret_cast<HGLOBAL>(memory));
}

LPVOID WINAPI Win32Bridge::Bridge::BridgeLocalLock(HLOCAL memory)
{
    return BridgeGlobalLock(reinterpret_cast<HGLOBAL>(memory));
}

BOOL WINAPI Win32Bridge::Bridge::BridgeLocalUnlock(HLOCAL memory)
{
    return BridgeGlobalUnlock(reinterpret_cast<HGLOBAL>(memory));
}

UINT WINAPI Win32Bridge::Bridge::BridgeGetACP() { return 1252; }

void WINAPI Win32Bridge::Bridge::BridgeGetStartupInfoW(LPSTARTUPINFOW startupInfo)
{
    if (!startupInfo) { SetGuestLastError(ERROR_INVALID_PARAMETER); return; }
    ZeroMemory(startupInfo, sizeof(*startupInfo));
    startupInfo->cb = sizeof(*startupInfo);
    startupInfo->dwFlags = 0x00000001u; // STARTF_USESHOWWINDOW
    startupInfo->wShowWindow = SW_SHOWNORMAL;
    SetGuestLastError(ERROR_SUCCESS);
}

DWORD WINAPI Win32Bridge::Bridge::BridgeGetFullPathNameW(
    LPCWSTR fileName, DWORD bufferLength, LPWSTR buffer, LPWSTR* filePart)
{
    if (!fileName || !*fileName) { SetGuestLastError(ERROR_INVALID_NAME); return 0; }
    std::wstring full = fileName;
    const bool absolute = full.size() >= 3 && iswalpha(full[0]) && full[1] == L':' &&
        (full[2] == L'\\' || full[2] == L'/');
    if (!absolute)
    {
        const DWORD required = BridgeGetCurrentDirectoryW(0, nullptr);
        std::vector<wchar_t> current(static_cast<size_t>(required) + 1);
        if (!required || !BridgeGetCurrentDirectoryW(static_cast<DWORD>(current.size()), current.data())) return 0;
        full.assign(current.data());
        if (!full.empty() && full.back() != L'\\') full.push_back(L'\\');
        full.append(fileName);
    }
    std::replace(full.begin(), full.end(), L'/', L'\\');
    const DWORD length = static_cast<DWORD>(full.size());
    if (!buffer || bufferLength <= length) return length + 1;
    memcpy(buffer, full.c_str(), static_cast<size_t>(length + 1) * sizeof(wchar_t));
    if (filePart)
    {
        const size_t slash = full.find_last_of(L'\\');
        *filePart = buffer + (slash == std::wstring::npos ? 0 : slash + 1);
    }
    SetGuestLastError(ERROR_SUCCESS);
    return length;
}

int WINAPI Win32Bridge::Bridge::BridgeFoldStringW(
    DWORD, LPCWSTR source, int sourceCount, LPWSTR destination, int destinationCount)
{
    if (!source || sourceCount == 0) { SetGuestLastError(ERROR_INVALID_PARAMETER); return 0; }
    const int count = sourceCount < 0 ? static_cast<int>(wcslen(source) + 1) : sourceCount;
    if (!destination || destinationCount == 0) return count;
    if (destinationCount < count) { SetGuestLastError(ERROR_INSUFFICIENT_BUFFER); return 0; }
    memcpy(destination, source, static_cast<size_t>(count) * sizeof(wchar_t));
    SetGuestLastError(ERROR_SUCCESS);
    return count;
}

BOOL WINAPI Win32Bridge::Bridge::BridgeHeapSetInformation(
    HANDLE, HEAP_INFORMATION_CLASS, PVOID, SIZE_T)
{
    SetGuestLastError(ERROR_SUCCESS);
    return TRUE;
}

int WINAPI Win32Bridge::Bridge::BridgeMulDiv(int number, int numerator, int denominator)
{
    if (denominator == 0) return -1;
    const LONGLONG product = static_cast<LONGLONG>(number) * numerator;
    const LONGLONG half = denominator > 0 ? denominator / 2 : -denominator / 2;
    const LONGLONG result = (product >= 0 ? product + half : product - half) / denominator;
    return result < INT_MIN || result > INT_MAX ? -1 : static_cast<int>(result);
}

int WINAPI Win32Bridge::Bridge::BridgeGetLocaleInfoW(
    LCID, LCTYPE type, LPWSTR data, int characterCount)
{
    std::wstring value;
    switch (type & 0xffff)
    {
    case LOCALE_ILANGUAGE: value = L"0409"; break;
    case LOCALE_SLANGUAGE: value = L"English (United States)"; break;
    case LOCALE_SENGLANGUAGE: value = L"English"; break;
    case LOCALE_SCOUNTRY: value = L"United States"; break;
    case LOCALE_SDECIMAL: value = L"."; break;
    case LOCALE_STHOUSAND: value = L","; break;
    case LOCALE_SLIST: value = L","; break;
    case LOCALE_SDATE: value = L"/"; break;
    case LOCALE_STIME: value = L":"; break;
    case LOCALE_SSHORTDATE: value = L"M/d/yyyy"; break;
    case LOCALE_SLONGDATE: value = L"dddd, MMMM d, yyyy"; break;
    case LOCALE_STIMEFORMAT: value = L"h:mm:ss tt"; break;
    case LOCALE_IDEFAULTANSICODEPAGE: value = L"1252"; break;
    default: value.clear(); break;
    }
    return CopyKernelLocaleText(value, data, characterCount);
}

UINT WINAPI Win32Bridge::Bridge::BridgeSetErrorMode(UINT mode)
{
    static std::atomic<UINT> current{ 0 };
    return current.exchange(mode);
}

namespace
{
struct GuestUnicodeString
{
    USHORT Length;
    USHORT MaximumLength;
    PWSTR Buffer;
};

void NTAPI BridgeRtlInitUnicodeString(
    GuestUnicodeString* destination, PCWSTR source)
{
    if (!destination) return;
    const size_t characters = source ? wcslen(source) : 0;
    const size_t bytes = (std::min)(characters * sizeof(wchar_t),
        static_cast<size_t>((std::numeric_limits<USHORT>::max)() - sizeof(wchar_t)));
    destination->Length = static_cast<USHORT>(bytes);
    destination->MaximumLength = static_cast<USHORT>(bytes + sizeof(wchar_t));
    destination->Buffer = const_cast<PWSTR>(source);
}

LONG NTAPI BridgeNtQueryLicenseValue(
    GuestUnicodeString*, PULONG type, PVOID, ULONG, PULONG resultLength)
{
    if (type) *type = REG_NONE;
    if (resultLength) *resultLength = 0;
    return static_cast<LONG>(0xC0000034L);
}

ULONG WINAPI BridgeWinSqmIncrementDWORD(PVOID, ULONG, DWORD)
{
    return ERROR_SUCCESS;
}

ULONG WINAPI BridgeWinSqmAddToStream(PVOID, ULONG, ULONG, PVOID)
{
    return ERROR_SUCCESS;
}
}
