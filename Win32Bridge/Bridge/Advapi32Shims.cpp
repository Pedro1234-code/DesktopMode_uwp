#include "pch.h"
#include "Bridge/Advapi32Shims.h"
#include "Bridge/GuestRegistry.h"
#include "Bridge/GuestKernel.h"

#include <string>
#include <atomic>
#include <vector>

using namespace Win32Bridge::Bridge;

namespace
{
    GuestRegistryContext* Registry() { return CurrentGuestRegistryContext(); }
    LSTATUS Status(DWORD error) { return static_cast<LSTATUS>(error); }
    bool IsAdvapiLibrary(const std::wstring& library) { return _wcsicmp(library.c_str(), L"advapi32.dll") == 0; }
    bool IsTextRegistryType(DWORD type) { return type == REG_SZ || type == REG_EXPAND_SZ || type == REG_MULTI_SZ; }

    bool ToWide(LPCSTR value, std::wstring* result)
    {
        if (!result) return false;
        if (!value) { result->clear(); return true; }
        const int length = ::MultiByteToWideChar(CP_ACP, 0, value, -1, nullptr, 0);
        if (length <= 0) return false;
        std::vector<wchar_t> buffer(static_cast<size_t>(length));
        if (::MultiByteToWideChar(CP_ACP, 0, value, -1, buffer.data(), length) != length) return false;
        result->assign(buffer.data());
        return true;
    }
}

LSTATUS WINAPI Win32Bridge::Bridge::BridgeRegOpenKeyExW(HKEY parent, LPCWSTR subKey, DWORD, REGSAM access, PHKEY result)
{
    if (!Registry() || !result) return Status(ERROR_INVALID_PARAMETER);
    if (!subKey || !*subKey) { *result = parent; return ERROR_SUCCESS; }
    DWORD error = 0;
    return Registry()->OpenKey(parent, subKey, access, result, &error) ? ERROR_SUCCESS : Status(error);
}

LSTATUS WINAPI Win32Bridge::Bridge::BridgeRegCreateKeyExW(HKEY parent, LPCWSTR subKey, DWORD, LPWSTR, DWORD, REGSAM access, LPSECURITY_ATTRIBUTES, PHKEY result, LPDWORD disposition)
{
    if (!Registry() || !result) return Status(ERROR_INVALID_PARAMETER);
    DWORD error = 0;
    return Registry()->CreateKey(parent, subKey, access, result, disposition, &error) ? ERROR_SUCCESS : Status(error);
}

LSTATUS WINAPI Win32Bridge::Bridge::BridgeRegCreateKeyW(HKEY parent, LPCWSTR subKey, PHKEY result)
{
    return BridgeRegCreateKeyExW(parent, subKey, 0, nullptr, 0,
        KEY_READ | KEY_WRITE, nullptr, result, nullptr);
}

BOOL WINAPI Win32Bridge::Bridge::BridgeIsTextUnicode(
    const void* buffer, int byteCount, LPINT tests)
{
    if (!buffer || byteCount < 0)
    {
        ::SetLastError(ERROR_INVALID_PARAMETER);
        return FALSE;
    }
    if (byteCount < static_cast<int>(sizeof(wchar_t))) return FALSE;
    const BYTE* bytes = static_cast<const BYTE*>(buffer);
    bool hasZeroHighByte = false;
    bool hasUnicodeBom = byteCount >= 2 && bytes[0] == 0xff && bytes[1] == 0xfe;
    for (int index = 1; index < byteCount; index += 2)
        hasZeroHighByte = hasZeroHighByte || bytes[index] == 0;
    if (tests)
    {
        int result = 0;
        if (hasUnicodeBom) result |= IS_TEXT_UNICODE_SIGNATURE;
        if (hasZeroHighByte) result |= IS_TEXT_UNICODE_STATISTICS;
        *tests &= result;
    }
    return hasUnicodeBom || hasZeroHighByte;
}

HANDLE WINAPI Win32Bridge::Bridge::BridgeOpenSCManagerW(LPCWSTR, LPCWSTR, DWORD)
{
    ::SetLastError(ERROR_SUCCESS);
    return reinterpret_cast<HANDLE>(static_cast<ULONG_PTR>(0x76000001));
}

HANDLE WINAPI Win32Bridge::Bridge::BridgeOpenServiceW(
    HANDLE manager, LPCWSTR serviceName, DWORD)
{
    if (manager != reinterpret_cast<HANDLE>(static_cast<ULONG_PTR>(0x76000001)) ||
        !serviceName || !*serviceName)
    {
        ::SetLastError(ERROR_INVALID_PARAMETER);
        return nullptr;
    }
    // The sandbox has no host SCM services. Report absence rather than exposing
    // or fabricating control over services outside the guest environment.
    ::SetLastError(ERROR_SERVICE_DOES_NOT_EXIST);
    return nullptr;
}

BOOL WINAPI Win32Bridge::Bridge::BridgeCloseServiceHandle(HANDLE handle)
{
    if (!handle) { ::SetLastError(ERROR_INVALID_HANDLE); return FALSE; }
    ::SetLastError(ERROR_SUCCESS);
    return TRUE;
}

BOOL WINAPI Win32Bridge::Bridge::BridgeQueryServiceConfigW(
    HANDLE, PVOID, DWORD, LPDWORD requiredSize)
{
    if (requiredSize) *requiredSize = 0;
    ::SetLastError(ERROR_SERVICE_DOES_NOT_EXIST);
    return FALSE;
}

LSTATUS WINAPI Win32Bridge::Bridge::BridgeRegQueryValueExW(HKEY key, LPCWSTR name, LPDWORD, LPDWORD type, LPBYTE data, LPDWORD byteCount)
{
    if (!Registry()) return Status(ERROR_INVALID_FUNCTION);
    DWORD error = 0;
    return Registry()->QueryValue(key, name, type, data, byteCount, &error) ? ERROR_SUCCESS : Status(error);
}

LSTATUS WINAPI Win32Bridge::Bridge::BridgeRegSetValueExW(HKEY key, LPCWSTR name, DWORD, DWORD type, const BYTE* data, DWORD byteCount)
{
    if (!Registry()) return Status(ERROR_INVALID_FUNCTION);
    DWORD error = 0;
    return Registry()->SetValue(key, name, type, data, byteCount, &error) ? ERROR_SUCCESS : Status(error);
}

LSTATUS WINAPI Win32Bridge::Bridge::BridgeRegCloseKey(HKEY key)
{
    if (key == HKEY_CURRENT_USER || key == HKEY_LOCAL_MACHINE || key == HKEY_USERS ||
        key == HKEY_CLASSES_ROOT || key == HKEY_CURRENT_CONFIG) return ERROR_SUCCESS;
    if (!Registry()) return Status(ERROR_INVALID_FUNCTION);
    DWORD error = 0;
    return Registry()->CloseKey(key, &error) ? ERROR_SUCCESS : Status(error);
}

LSTATUS WINAPI Win32Bridge::Bridge::BridgeRegDeleteValueW(HKEY key, LPCWSTR name)
{
    if (!Registry()) return Status(ERROR_INVALID_FUNCTION);
    DWORD error = 0;
    return Registry()->DeleteValue(key, name, &error) ? ERROR_SUCCESS : Status(error);
}

LSTATUS WINAPI Win32Bridge::Bridge::BridgeRegDeleteKeyW(HKEY key, LPCWSTR subKey)
{
    if (!Registry()) return Status(ERROR_INVALID_FUNCTION);
    DWORD error = 0;
    return Registry()->DeleteKey(key, subKey, &error) ? ERROR_SUCCESS : Status(error);
}

LSTATUS WINAPI Win32Bridge::Bridge::BridgeRegEnumKeyExW(HKEY key, DWORD index, LPWSTR name, LPDWORD characterCount, LPDWORD, LPWSTR, LPDWORD, PFILETIME)
{
    if (!Registry()) return Status(ERROR_INVALID_FUNCTION);
    DWORD error = 0;
    return Registry()->EnumKey(key, index, name, characterCount, &error) ? ERROR_SUCCESS : Status(error);
}

LSTATUS WINAPI Win32Bridge::Bridge::BridgeRegEnumValueW(HKEY key, DWORD index, LPWSTR name,
    LPDWORD characterCount, LPDWORD, LPDWORD type, LPBYTE data, LPDWORD byteCount)
{
    if (!Registry()) return Status(ERROR_INVALID_FUNCTION);
    DWORD error = 0;
    return Registry()->EnumValue(key, index, name, characterCount, type, data, byteCount, &error)
        ? ERROR_SUCCESS : Status(error);
}

LSTATUS WINAPI Win32Bridge::Bridge::BridgeRegQueryInfoKeyW(HKEY key, LPWSTR, LPDWORD, LPDWORD,
    LPDWORD subKeys, LPDWORD maximumSubKeyLength, LPDWORD, LPDWORD values,
    LPDWORD maximumValueNameLength, LPDWORD maximumValueDataLength, LPDWORD,
    PFILETIME lastWriteTime)
{
    if (!Registry()) return Status(ERROR_INVALID_FUNCTION);
    DWORD error = 0;
    return Registry()->QueryInfoKey(key, subKeys, maximumSubKeyLength, values,
        maximumValueNameLength, maximumValueDataLength, lastWriteTime, &error)
        ? ERROR_SUCCESS : Status(error);
}

LSTATUS WINAPI Win32Bridge::Bridge::BridgeRegFlushKey(HKEY)
{
    if (!Registry()) return Status(ERROR_INVALID_FUNCTION);
    DWORD error = 0;
    return Registry()->Flush(&error) ? ERROR_SUCCESS : Status(error);
}

LSTATUS WINAPI Win32Bridge::Bridge::BridgeRegOpenKeyExA(HKEY parent, LPCSTR subKey, DWORD options, REGSAM access, PHKEY result)
{
    std::wstring wide;
    if (subKey && !ToWide(subKey, &wide)) return Status(ERROR_INVALID_PARAMETER);
    return BridgeRegOpenKeyExW(parent, subKey ? wide.c_str() : nullptr, options, access, result);
}

LSTATUS WINAPI Win32Bridge::Bridge::BridgeRegCreateKeyExA(HKEY parent, LPCSTR subKey, DWORD reserved, LPSTR, DWORD options, REGSAM access, LPSECURITY_ATTRIBUTES attributes, PHKEY result, LPDWORD disposition)
{
    std::wstring wide;
    if (!ToWide(subKey, &wide)) return Status(ERROR_INVALID_PARAMETER);
    return BridgeRegCreateKeyExW(parent, wide.c_str(), reserved, nullptr, options, access, attributes, result, disposition);
}

LSTATUS WINAPI Win32Bridge::Bridge::BridgeRegQueryValueExA(HKEY key, LPCSTR name, LPDWORD reserved, LPDWORD type, LPBYTE data, LPDWORD byteCount)
{
    if (!byteCount) return Status(ERROR_INVALID_PARAMETER);
    std::wstring wideName;
    if (name && !ToWide(name, &wideName)) return Status(ERROR_INVALID_PARAMETER);

    DWORD registryType = 0;
    DWORD wideByteCount = 0;
    const LSTATUS probe = BridgeRegQueryValueExW(key, name ? wideName.c_str() : nullptr, reserved, &registryType, nullptr, &wideByteCount);
    if (probe != ERROR_SUCCESS) return probe;
    if (type) *type = registryType;
    if (!IsTextRegistryType(registryType))
        return BridgeRegQueryValueExW(key, name ? wideName.c_str() : nullptr, reserved, type, data, byteCount);

    std::vector<BYTE> wideBytes(wideByteCount);
    DWORD readBytes = wideByteCount;
    const LSTATUS read = BridgeRegQueryValueExW(key, name ? wideName.c_str() : nullptr, reserved, type, wideBytes.data(), &readBytes);
    if (read != ERROR_SUCCESS) return read;
    const int characterCount = static_cast<int>(readBytes / sizeof(wchar_t));
    const int required = ::WideCharToMultiByte(CP_ACP, 0, reinterpret_cast<LPCWCH>(wideBytes.data()), characterCount, nullptr, 0, nullptr, nullptr);
    if (required < 0) return Status(ERROR_INVALID_DATA);
    if (!data) { *byteCount = static_cast<DWORD>(required); return ERROR_SUCCESS; }
    if (*byteCount < static_cast<DWORD>(required)) { *byteCount = static_cast<DWORD>(required); return Status(ERROR_MORE_DATA); }
    if (::WideCharToMultiByte(CP_ACP, 0, reinterpret_cast<LPCWCH>(wideBytes.data()), characterCount, reinterpret_cast<LPSTR>(data), required, nullptr, nullptr) != required)
        return Status(ERROR_INVALID_DATA);
    *byteCount = static_cast<DWORD>(required);
    return ERROR_SUCCESS;
}

LSTATUS WINAPI Win32Bridge::Bridge::BridgeRegSetValueExA(HKEY key, LPCSTR name, DWORD reserved, DWORD type, const BYTE* data, DWORD byteCount)
{
    std::wstring wideName;
    if (name && !ToWide(name, &wideName)) return Status(ERROR_INVALID_PARAMETER);
    if (!IsTextRegistryType(type) || byteCount == 0)
        return BridgeRegSetValueExW(key, name ? wideName.c_str() : nullptr, reserved, type, data, byteCount);
    if (!data) return Status(ERROR_INVALID_PARAMETER);

    const int characters = ::MultiByteToWideChar(CP_ACP, 0, reinterpret_cast<LPCCH>(data), static_cast<int>(byteCount), nullptr, 0);
    if (characters <= 0) return Status(ERROR_INVALID_DATA);
    std::vector<wchar_t> wide(static_cast<size_t>(characters));
    if (::MultiByteToWideChar(CP_ACP, 0, reinterpret_cast<LPCCH>(data), static_cast<int>(byteCount), wide.data(), characters) != characters)
        return Status(ERROR_INVALID_DATA);
    return BridgeRegSetValueExW(key, name ? wideName.c_str() : nullptr, reserved, type,
        reinterpret_cast<const BYTE*>(wide.data()), static_cast<DWORD>(wide.size() * sizeof(wchar_t)));
}

LSTATUS WINAPI Win32Bridge::Bridge::BridgeRegDeleteValueA(HKEY key, LPCSTR name)
{
    std::wstring wide;
    return ToWide(name, &wide) ? BridgeRegDeleteValueW(key, wide.c_str()) : Status(ERROR_INVALID_PARAMETER);
}

LSTATUS WINAPI Win32Bridge::Bridge::BridgeRegDeleteKeyA(HKEY key, LPCSTR subKey)
{
    std::wstring wide;
    return ToWide(subKey, &wide) ? BridgeRegDeleteKeyW(key, wide.c_str()) : Status(ERROR_INVALID_PARAMETER);
}

LSTATUS WINAPI Win32Bridge::Bridge::BridgeRegEnumValueA(HKEY key, DWORD index, LPSTR name,
    LPDWORD characterCount, LPDWORD reserved, LPDWORD type, LPBYTE data, LPDWORD byteCount)
{
    if (!characterCount) return Status(ERROR_INVALID_PARAMETER);
    const DWORD suppliedNameCharacters = *characterCount;
    const DWORD suppliedDataBytes = byteCount ? *byteCount : 0;
    DWORD wideCharacters = 0;
    DWORD registryType = 0;
    DWORD wideDataBytes = 0;
    LSTATUS status = BridgeRegEnumValueW(key, index, nullptr, &wideCharacters, reserved,
        &registryType, nullptr, &wideDataBytes);
    if (status != ERROR_MORE_DATA && status != ERROR_SUCCESS) return status;
    std::vector<wchar_t> wideName(static_cast<size_t>(wideCharacters) + 1);
    DWORD capacity = wideCharacters + 1;
    std::vector<BYTE> wideData(IsTextRegistryType(registryType) ? wideDataBytes : 0);
    DWORD readDataBytes = wideDataBytes;
    status = BridgeRegEnumValueW(key, index, wideName.data(), &capacity, reserved,
        &registryType, IsTextRegistryType(registryType) ? wideData.data() : data,
        IsTextRegistryType(registryType) ? &readDataBytes : byteCount);
    if (status != ERROR_SUCCESS) return status;
    if (type) *type = registryType;
    const int required = ::WideCharToMultiByte(CP_ACP, 0, wideName.data(), static_cast<int>(capacity),
        nullptr, 0, nullptr, nullptr);
    if (required < 0) return Status(ERROR_INVALID_DATA);
    DWORD requiredDataBytes = wideDataBytes;
    if (IsTextRegistryType(registryType))
    {
        requiredDataBytes = static_cast<DWORD>(::WideCharToMultiByte(CP_ACP, 0,
            reinterpret_cast<LPCWCH>(wideData.data()), static_cast<int>(readDataBytes / sizeof(wchar_t)),
            nullptr, 0, nullptr, nullptr));
    }
    if (byteCount) *byteCount = requiredDataBytes;
    if (!name || suppliedNameCharacters <= static_cast<DWORD>(required) ||
        (data && (!byteCount || suppliedDataBytes < requiredDataBytes)))
    {
        *characterCount = static_cast<DWORD>(required);
        return Status(ERROR_MORE_DATA);
    }
    ::WideCharToMultiByte(CP_ACP, 0, wideName.data(), static_cast<int>(capacity),
        name, static_cast<int>(suppliedNameCharacters), nullptr, nullptr);
    name[required] = '\0';
    if (data && IsTextRegistryType(registryType) && requiredDataBytes)
        ::WideCharToMultiByte(CP_ACP, 0, reinterpret_cast<LPCWCH>(wideData.data()),
            static_cast<int>(readDataBytes / sizeof(wchar_t)), reinterpret_cast<LPSTR>(data),
            static_cast<int>(suppliedDataBytes), nullptr, nullptr);
    *characterCount = static_cast<DWORD>(required);
    return ERROR_SUCCESS;
}

LSTATUS WINAPI Win32Bridge::Bridge::BridgeRegQueryInfoKeyA(HKEY key, LPSTR, LPDWORD,
    LPDWORD reserved, LPDWORD subKeys, LPDWORD maximumSubKeyLength, LPDWORD maximumClassLength,
    LPDWORD values, LPDWORD maximumValueNameLength, LPDWORD maximumValueDataLength,
    LPDWORD securityDescriptorLength, PFILETIME lastWriteTime)
{
    return BridgeRegQueryInfoKeyW(key, nullptr, nullptr, reserved, subKeys, maximumSubKeyLength,
        maximumClassLength, values, maximumValueNameLength, maximumValueDataLength,
        securityDescriptorLength, lastWriteTime);
}

BOOL WINAPI Win32Bridge::Bridge::BridgeLookupPrivilegeValueW(LPCWSTR, LPCWSTR name, PLUID value)
{
    if (!name || !value)
    {
        ::SetLastError(ERROR_INVALID_PARAMETER);
        return FALSE;
    }
    // Privileges are scoped to the guest only.  The deterministic LUID lets
    // callers build TOKEN_PRIVILEGES structures without obtaining a host token.
    value->LowPart = 1;
    value->HighPart = 0;
    ::SetLastError(ERROR_SUCCESS);
    return TRUE;
}

BOOL WINAPI Win32Bridge::Bridge::BridgeOpenProcessToken(HANDLE, DWORD, PHANDLE token)
{
    if (!token || !CurrentGuestKernelContext())
    {
        ::SetLastError(ERROR_INVALID_PARAMETER);
        return FALSE;
    }
    DWORD error = ERROR_SUCCESS;
    HANDLE guestToken = CurrentGuestKernelContext()->CreateEvent(false, false, nullptr, &error);
    if (!guestToken)
    {
        ::SetLastError(error);
        return FALSE;
    }
    *token = guestToken;
    ::SetLastError(ERROR_SUCCESS);
    return TRUE;
}

BOOL WINAPI Win32Bridge::Bridge::BridgeAdjustTokenPrivileges(HANDLE token, BOOL, PTOKEN_PRIVILEGES, DWORD, PTOKEN_PRIVILEGES, PDWORD)
{
    if (!CurrentGuestKernelContext() || !token)
    {
        ::SetLastError(ERROR_INVALID_HANDLE);
        return FALSE;
    }
    ::SetLastError(ERROR_SUCCESS);
    return TRUE;
}

BOOL WINAPI Win32Bridge::Bridge::BridgeGetFileSecurityW(LPCWSTR fileName, SECURITY_INFORMATION, PSECURITY_DESCRIPTOR descriptor, DWORD descriptorLength, LPDWORD requiredLength)
{
    if (!fileName || !requiredLength)
    {
        ::SetLastError(ERROR_INVALID_PARAMETER);
        return FALSE;
    }
    const DWORD required = sizeof(SECURITY_DESCRIPTOR);
    *requiredLength = required;
    if (!descriptor || descriptorLength < required)
    {
        ::SetLastError(ERROR_INSUFFICIENT_BUFFER);
        return FALSE;
    }
    ZeroMemory(descriptor, required);
    ::SetLastError(ERROR_SUCCESS);
    return TRUE;
}

BOOL WINAPI Win32Bridge::Bridge::BridgeSetFileSecurityW(LPCWSTR fileName, SECURITY_INFORMATION, PSECURITY_DESCRIPTOR)
{
    if (!fileName)
    {
        ::SetLastError(ERROR_INVALID_PARAMETER);
        return FALSE;
    }
    // ACLs cannot escape the LocalFolder sandbox.  A successful no-op matches
    // the effective guest model: every guest file is owned by this app.
    ::SetLastError(ERROR_SUCCESS);
    return TRUE;
}

LSTATUS WINAPI Win32Bridge::Bridge::BridgeRegDeleteKeyExW(HKEY key, LPCWSTR subKey, REGSAM, DWORD)
{
    return BridgeRegDeleteKeyW(key, subKey);
}

BOOL WINAPI Win32Bridge::Bridge::BridgeGetUserNameW(LPWSTR buffer, LPDWORD characters)
{
    static const wchar_t user[] = L"Win32Bridge";
    const DWORD required = static_cast<DWORD>(_countof(user));
    if (!characters) { ::SetLastError(ERROR_INVALID_PARAMETER); return FALSE; }
    if (!buffer || *characters < required) { *characters = required; ::SetLastError(ERROR_INSUFFICIENT_BUFFER); return FALSE; }
    memcpy(buffer, user, required * sizeof(wchar_t));
    *characters = required - 1;
    ::SetLastError(ERROR_SUCCESS);
    return TRUE;
}

BOOL WINAPI Win32Bridge::Bridge::BridgeLookupAccountNameW(LPCWSTR, LPCWSTR, PSID, LPDWORD sidSize, LPWSTR, LPDWORD domainSize, PSID_NAME_USE)
{
    if (sidSize) *sidSize = 0;
    if (domainSize) *domainSize = 0;
    ::SetLastError(ERROR_NONE_MAPPED);
    return FALSE;
}

LONG WINAPI Win32Bridge::Bridge::BridgeLsaOpenPolicy(PVOID, PVOID, ACCESS_MASK, PVOID policy)
{
    if (policy) *reinterpret_cast<ULONG_PTR*>(policy) = 1;
    return 0;
}
LONG WINAPI Win32Bridge::Bridge::BridgeLsaAddAccountRights(ULONG_PTR, PSID, PVOID, ULONG) { return 0; }
LONG WINAPI Win32Bridge::Bridge::BridgeLsaClose(ULONG_PTR) { return 0; }

BOOLEAN WINAPI Win32Bridge::Bridge::BridgeSystemFunction036(PVOID buffer, ULONG length)
{
    if (!buffer && length != 0) return FALSE;
    // Guest-local entropy is used only for compatibility identifiers; it is
    // never represented as host credentials or security material.
    static std::atomic<ULONGLONG> state{ 0x9e3779b97f4a7c15ull };
    ULONGLONG value = state.fetch_add(0x9e3779b97f4a7c15ull);
    BYTE* output = static_cast<BYTE*>(buffer);
    for (ULONG index = 0; index < length; ++index)
    {
        value ^= value << 13;
        value ^= value >> 7;
        value ^= value << 17;
        output[index] = static_cast<BYTE>(value >> 24);
    }
    return TRUE;
}

ImportResolution Win32Bridge::Bridge::ResolveAdvapi32Import(const ImportedSymbol& symbol)
{
    auto resolution = CompatibilityCatalog::Resolve(symbol);
    if (symbol.importedByOrdinal || !IsAdvapiLibrary(symbol.library)) return resolution;

    if (_wcsicmp(symbol.name.c_str(), L"regopenkeyexw") == 0) resolution.targetAddress = reinterpret_cast<ULONGLONG>(&BridgeRegOpenKeyExW);
    else if (_wcsicmp(symbol.name.c_str(), L"regcreatekeyw") == 0) resolution.targetAddress = reinterpret_cast<ULONGLONG>(&BridgeRegCreateKeyW);
    else if (_wcsicmp(symbol.name.c_str(), L"regcreatekeyexw") == 0) resolution.targetAddress = reinterpret_cast<ULONGLONG>(&BridgeRegCreateKeyExW);
    else if (_wcsicmp(symbol.name.c_str(), L"regqueryvalueexw") == 0) resolution.targetAddress = reinterpret_cast<ULONGLONG>(&BridgeRegQueryValueExW);
    else if (_wcsicmp(symbol.name.c_str(), L"regsetvalueexw") == 0) resolution.targetAddress = reinterpret_cast<ULONGLONG>(&BridgeRegSetValueExW);
    else if (_wcsicmp(symbol.name.c_str(), L"regdeletevaluew") == 0) resolution.targetAddress = reinterpret_cast<ULONGLONG>(&BridgeRegDeleteValueW);
    else if (_wcsicmp(symbol.name.c_str(), L"regdeletekeyw") == 0) resolution.targetAddress = reinterpret_cast<ULONGLONG>(&BridgeRegDeleteKeyW);
    else if (_wcsicmp(symbol.name.c_str(), L"regenumkeyexw") == 0) resolution.targetAddress = reinterpret_cast<ULONGLONG>(&BridgeRegEnumKeyExW);
    else if (_wcsicmp(symbol.name.c_str(), L"regenumvaluew") == 0) resolution.targetAddress = reinterpret_cast<ULONGLONG>(&BridgeRegEnumValueW);
    else if (_wcsicmp(symbol.name.c_str(), L"regqueryinfokeyw") == 0) resolution.targetAddress = reinterpret_cast<ULONGLONG>(&BridgeRegQueryInfoKeyW);
    else if (_wcsicmp(symbol.name.c_str(), L"regflushkey") == 0) resolution.targetAddress = reinterpret_cast<ULONGLONG>(&BridgeRegFlushKey);
    else if (_wcsicmp(symbol.name.c_str(), L"regopenkeyexa") == 0) resolution.targetAddress = reinterpret_cast<ULONGLONG>(&BridgeRegOpenKeyExA);
    else if (_wcsicmp(symbol.name.c_str(), L"regcreatekeyexa") == 0) resolution.targetAddress = reinterpret_cast<ULONGLONG>(&BridgeRegCreateKeyExA);
    else if (_wcsicmp(symbol.name.c_str(), L"regqueryvalueexa") == 0) resolution.targetAddress = reinterpret_cast<ULONGLONG>(&BridgeRegQueryValueExA);
    else if (_wcsicmp(symbol.name.c_str(), L"regsetvalueexa") == 0) resolution.targetAddress = reinterpret_cast<ULONGLONG>(&BridgeRegSetValueExA);
    else if (_wcsicmp(symbol.name.c_str(), L"regdeletevaluea") == 0) resolution.targetAddress = reinterpret_cast<ULONGLONG>(&BridgeRegDeleteValueA);
    else if (_wcsicmp(symbol.name.c_str(), L"regdeletekeya") == 0) resolution.targetAddress = reinterpret_cast<ULONGLONG>(&BridgeRegDeleteKeyA);
    else if (_wcsicmp(symbol.name.c_str(), L"regenumvaluea") == 0) resolution.targetAddress = reinterpret_cast<ULONGLONG>(&BridgeRegEnumValueA);
    else if (_wcsicmp(symbol.name.c_str(), L"regqueryinfokeya") == 0) resolution.targetAddress = reinterpret_cast<ULONGLONG>(&BridgeRegQueryInfoKeyA);
    else if (_wcsicmp(symbol.name.c_str(), L"regclosekey") == 0) resolution.targetAddress = reinterpret_cast<ULONGLONG>(&BridgeRegCloseKey);
    else if (_wcsicmp(symbol.name.c_str(), L"lookupprivilegevaluew") == 0) resolution.targetAddress = reinterpret_cast<ULONGLONG>(&BridgeLookupPrivilegeValueW);
    else if (_wcsicmp(symbol.name.c_str(), L"openprocesstoken") == 0) resolution.targetAddress = reinterpret_cast<ULONGLONG>(&BridgeOpenProcessToken);
    else if (_wcsicmp(symbol.name.c_str(), L"adjusttokenprivileges") == 0) resolution.targetAddress = reinterpret_cast<ULONGLONG>(&BridgeAdjustTokenPrivileges);
    else if (_wcsicmp(symbol.name.c_str(), L"getfilesecurityw") == 0) resolution.targetAddress = reinterpret_cast<ULONGLONG>(&BridgeGetFileSecurityW);
    else if (_wcsicmp(symbol.name.c_str(), L"setfilesecurityw") == 0) resolution.targetAddress = reinterpret_cast<ULONGLONG>(&BridgeSetFileSecurityW);
    else if (_wcsicmp(symbol.name.c_str(), L"regdeletekeyexw") == 0) resolution.targetAddress = reinterpret_cast<ULONGLONG>(&BridgeRegDeleteKeyExW);
    else if (_wcsicmp(symbol.name.c_str(), L"getusernamew") == 0) resolution.targetAddress = reinterpret_cast<ULONGLONG>(&BridgeGetUserNameW);
    else if (_wcsicmp(symbol.name.c_str(), L"lookupaccountnamew") == 0) resolution.targetAddress = reinterpret_cast<ULONGLONG>(&BridgeLookupAccountNameW);
    else if (_wcsicmp(symbol.name.c_str(), L"lsaopenpolicy") == 0) resolution.targetAddress = reinterpret_cast<ULONGLONG>(&BridgeLsaOpenPolicy);
    else if (_wcsicmp(symbol.name.c_str(), L"lsaaddaccountrights") == 0) resolution.targetAddress = reinterpret_cast<ULONGLONG>(&BridgeLsaAddAccountRights);
    else if (_wcsicmp(symbol.name.c_str(), L"lsaclose") == 0) resolution.targetAddress = reinterpret_cast<ULONGLONG>(&BridgeLsaClose);
    else if (_wcsicmp(symbol.name.c_str(), L"systemfunction036") == 0) resolution.targetAddress = reinterpret_cast<ULONGLONG>(&BridgeSystemFunction036);
    else if (_wcsicmp(symbol.name.c_str(), L"istextunicode") == 0) resolution.targetAddress = reinterpret_cast<ULONGLONG>(&BridgeIsTextUnicode);
    else if (_wcsicmp(symbol.name.c_str(), L"openscmanagerw") == 0) resolution.targetAddress = reinterpret_cast<ULONGLONG>(&BridgeOpenSCManagerW);
    else if (_wcsicmp(symbol.name.c_str(), L"openservicew") == 0) resolution.targetAddress = reinterpret_cast<ULONGLONG>(&BridgeOpenServiceW);
    else if (_wcsicmp(symbol.name.c_str(), L"closeservicehandle") == 0) resolution.targetAddress = reinterpret_cast<ULONGLONG>(&BridgeCloseServiceHandle);
    else if (_wcsicmp(symbol.name.c_str(), L"queryserviceconfigw") == 0) resolution.targetAddress = reinterpret_cast<ULONGLONG>(&BridgeQueryServiceConfigW);

    if (resolution.targetAddress) resolution.disposition = ImportDisposition::NeedsBridge;
    return resolution;
}
