#include "pch.h"
#include "Bridge/VersionShims.h"

#include "Bridge/GuestResources.h"
#include "Bridge/GuestStorage.h"
#include "Bridge/Kernel32Shims.h"
#include "Bridge/RuntimeDiagnostics.h"

#include <algorithm>
#include <cstring>
#include <limits>
#include <string>
#include <vector>

using namespace Win32Bridge::Bridge;

namespace
{
    constexpr DWORD SupportedVersionFlags = 0x00000007; // FILE_VER_GET_*.

    size_t Align4(size_t value)
    {
        return (value + 3u) & ~size_t(3u);
    }

    bool ConvertAnsi(LPCSTR source, std::wstring* destination)
    {
        if (!source || !destination) return false;
        const int required = MultiByteToWideChar(CP_ACP, MB_ERR_INVALID_CHARS, source, -1, nullptr, 0);
        if (required <= 0) return false;
        destination->resize(static_cast<size_t>(required));
        if (MultiByteToWideChar(CP_ACP, MB_ERR_INVALID_CHARS, source, -1,
            &(*destination)[0], required) != required)
        {
            destination->clear();
            return false;
        }
        destination->resize(static_cast<size_t>(required - 1));
        return true;
    }

    DWORD ResourceError(GuestResourceStatus status)
    {
        switch (status)
        {
        case GuestResourceStatus::InvalidParameter: return ERROR_INVALID_PARAMETER;
        case GuestResourceStatus::InvalidImage: return ERROR_BAD_EXE_FORMAT;
        case GuestResourceStatus::TypeNotFound: return ERROR_RESOURCE_TYPE_NOT_FOUND;
        case GuestResourceStatus::NameNotFound: return ERROR_RESOURCE_NAME_NOT_FOUND;
        case GuestResourceStatus::LanguageNotFound: return ERROR_RESOURCE_LANG_NOT_FOUND;
        case GuestResourceStatus::InvalidData: return ERROR_INVALID_DATA;
        default: return ERROR_RESOURCE_DATA_NOT_FOUND;
        }
    }

    bool ReadVersionResource(LPCWSTR fileName, std::vector<BYTE>* bytes, DWORD* error)
    {
        if (!fileName || !bytes || !error)
        {
            if (error) *error = ERROR_INVALID_PARAMETER;
            return false;
        }
        GuestStorageContext* storage = CurrentGuestStorageContext();
        std::vector<BYTE> file;
        if (!storage || !storage->ReadAllBytes(fileName, &file, error)) return false;
        const GuestResourceStatus status = CopyGuestFileResource(
            file.data(), file.size(), MAKEINTRESOURCEW(16), MAKEINTRESOURCEW(1),
            0, false, bytes);
        if (status != GuestResourceStatus::Success)
        {
            *error = ResourceError(status);
            return false;
        }
        if (bytes->size() < 6 || bytes->size() > (std::numeric_limits<DWORD>::max)())
        {
            *error = ERROR_INVALID_DATA;
            return false;
        }
        *error = ERROR_SUCCESS;
        return true;
    }

    struct VersionBlock final
    {
        const BYTE* base = nullptr;
        size_t available = 0;
        WORD totalLength = 0;
        WORD valueLength = 0;
        WORD type = 0;
        std::wstring key;
        size_t valueOffset = 0;
        size_t valueBytes = 0;
        size_t childrenOffset = 0;
    };

    bool ReadWord(const BYTE* bytes, size_t size, size_t offset, WORD* value)
    {
        if (!bytes || !value || offset > size || sizeof(WORD) > size - offset) return false;
        memcpy(value, bytes + offset, sizeof(*value));
        return true;
    }

    bool ParseBlock(const BYTE* bytes, size_t available, VersionBlock* result)
    {
        if (!bytes || !result || available < 6) return false;
        VersionBlock block;
        block.base = bytes;
        block.available = available;
        if (!ReadWord(bytes, available, 0, &block.totalLength) ||
            !ReadWord(bytes, available, 2, &block.valueLength) ||
            !ReadWord(bytes, available, 4, &block.type) ||
            block.totalLength < 6 || block.totalLength > available)
        {
            return false;
        }
        size_t cursor = 6;
        while (true)
        {
            WORD character = 0;
            if (!ReadWord(bytes, block.totalLength, cursor, &character)) return false;
            cursor += sizeof(WORD);
            if (character == 0) break;
            block.key.push_back(static_cast<wchar_t>(character));
        }
        block.valueOffset = Align4(cursor);
        block.valueBytes = block.type == 1
            ? static_cast<size_t>(block.valueLength) * sizeof(wchar_t)
            : static_cast<size_t>(block.valueLength);
        if (block.valueOffset > block.totalLength ||
            block.valueBytes > block.totalLength - block.valueOffset)
        {
            return false;
        }
        block.childrenOffset = Align4(block.valueOffset + block.valueBytes);
        if (block.childrenOffset > block.totalLength) block.childrenOffset = block.totalLength;
        *result = std::move(block);
        return true;
    }

    bool FindChild(const VersionBlock& parent, const std::wstring& key, VersionBlock* child)
    {
        size_t cursor = parent.childrenOffset;
        while (cursor + 6 <= parent.totalLength)
        {
            VersionBlock candidate;
            if (!ParseBlock(parent.base + cursor, parent.totalLength - cursor, &candidate) ||
                candidate.totalLength == 0)
            {
                return false;
            }
            if (_wcsicmp(candidate.key.c_str(), key.c_str()) == 0)
            {
                *child = std::move(candidate);
                return true;
            }
            cursor = Align4(cursor + candidate.totalLength);
        }
        return false;
    }

    std::vector<std::wstring> SplitPath(LPCWSTR path)
    {
        std::vector<std::wstring> parts;
        if (!path) return parts;
        const wchar_t* cursor = path;
        while (*cursor)
        {
            while (*cursor == L'\\' || *cursor == L'/') ++cursor;
            const wchar_t* start = cursor;
            while (*cursor && *cursor != L'\\' && *cursor != L'/') ++cursor;
            if (cursor != start) parts.emplace_back(start, cursor);
        }
        return parts;
    }

    bool QueryVersionValue(LPCVOID data, LPCWSTR path, LPVOID* value, PUINT length)
    {
        if (!data || !path || !value || !length) return false;
        *value = nullptr;
        *length = 0;
        const BYTE* bytes = static_cast<const BYTE*>(data);
        WORD rootLength = 0;
        memcpy(&rootLength, bytes, sizeof(rootLength));
        VersionBlock current;
        if (!ParseBlock(bytes, rootLength, &current) ||
            _wcsicmp(current.key.c_str(), L"VS_VERSION_INFO") != 0)
        {
            return false;
        }
        for (const auto& part : SplitPath(path))
        {
            VersionBlock next;
            if (!FindChild(current, part, &next)) return false;
            current = std::move(next);
        }
        *value = const_cast<BYTE*>(current.base + current.valueOffset);
        *length = current.valueLength;
        return true;
    }

    BOOL GetVersionInfo(LPCWSTR fileName, DWORD length, LPVOID data)
    {
        if (!data)
        {
            BridgeSetLastError(ERROR_INVALID_PARAMETER);
            return FALSE;
        }
        std::vector<BYTE> resource;
        DWORD error = ERROR_SUCCESS;
        if (!ReadVersionResource(fileName, &resource, &error))
        {
            BridgeSetLastError(error);
            return FALSE;
        }
        if (length < resource.size())
        {
            BridgeSetLastError(ERROR_INSUFFICIENT_BUFFER);
            return FALSE;
        }
        memcpy(data, resource.data(), resource.size());
        BridgeSetLastError(ERROR_SUCCESS);
        return TRUE;
    }
}

DWORD WINAPI Win32Bridge::Bridge::BridgeGetFileVersionInfoSizeW(LPCWSTR fileName, LPDWORD handle)
{
    if (handle) *handle = 0;
    std::vector<BYTE> resource;
    DWORD error = ERROR_SUCCESS;
    if (!ReadVersionResource(fileName, &resource, &error))
    {
        BridgeSetLastError(error);
        return 0;
    }
    BridgeSetLastError(ERROR_SUCCESS);
    return static_cast<DWORD>(resource.size());
}

DWORD WINAPI Win32Bridge::Bridge::BridgeGetFileVersionInfoSizeA(LPCSTR fileName, LPDWORD handle)
{
    std::wstring wide;
    if (!ConvertAnsi(fileName, &wide))
    {
        BridgeSetLastError(ERROR_NO_UNICODE_TRANSLATION);
        return 0;
    }
    return BridgeGetFileVersionInfoSizeW(wide.c_str(), handle);
}

DWORD WINAPI Win32Bridge::Bridge::BridgeGetFileVersionInfoSizeExW(DWORD flags, LPCWSTR fileName, LPDWORD handle)
{
    if ((flags & ~SupportedVersionFlags) != 0)
    {
        BridgeSetLastError(ERROR_INVALID_FLAGS);
        return 0;
    }
    return BridgeGetFileVersionInfoSizeW(fileName, handle);
}

DWORD WINAPI Win32Bridge::Bridge::BridgeGetFileVersionInfoSizeExA(DWORD flags, LPCSTR fileName, LPDWORD handle)
{
    if ((flags & ~SupportedVersionFlags) != 0)
    {
        BridgeSetLastError(ERROR_INVALID_FLAGS);
        return 0;
    }
    return BridgeGetFileVersionInfoSizeA(fileName, handle);
}

BOOL WINAPI Win32Bridge::Bridge::BridgeGetFileVersionInfoW(LPCWSTR fileName, DWORD, DWORD length, LPVOID data)
{
    return GetVersionInfo(fileName, length, data);
}

BOOL WINAPI Win32Bridge::Bridge::BridgeGetFileVersionInfoA(LPCSTR fileName, DWORD handle, DWORD length, LPVOID data)
{
    std::wstring wide;
    if (!ConvertAnsi(fileName, &wide))
    {
        BridgeSetLastError(ERROR_NO_UNICODE_TRANSLATION);
        return FALSE;
    }
    return BridgeGetFileVersionInfoW(wide.c_str(), handle, length, data);
}

BOOL WINAPI Win32Bridge::Bridge::BridgeGetFileVersionInfoExW(
    DWORD flags, LPCWSTR fileName, DWORD handle, DWORD length, LPVOID data)
{
    if ((flags & ~SupportedVersionFlags) != 0)
    {
        BridgeSetLastError(ERROR_INVALID_FLAGS);
        return FALSE;
    }
    return BridgeGetFileVersionInfoW(fileName, handle, length, data);
}

BOOL WINAPI Win32Bridge::Bridge::BridgeGetFileVersionInfoExA(
    DWORD flags, LPCSTR fileName, DWORD handle, DWORD length, LPVOID data)
{
    if ((flags & ~SupportedVersionFlags) != 0)
    {
        BridgeSetLastError(ERROR_INVALID_FLAGS);
        return FALSE;
    }
    return BridgeGetFileVersionInfoA(fileName, handle, length, data);
}

BOOL WINAPI Win32Bridge::Bridge::BridgeVerQueryValueW(
    LPCVOID block, LPCWSTR subBlock, LPVOID* value, PUINT length)
{
    const BOOL result = QueryVersionValue(block, subBlock, value, length) ? TRUE : FALSE;
    BridgeSetLastError(result ? ERROR_SUCCESS : ERROR_RESOURCE_DATA_NOT_FOUND);
    return result;
}

BOOL WINAPI Win32Bridge::Bridge::BridgeVerQueryValueA(
    LPCVOID block, LPCSTR subBlock, LPVOID* value, PUINT length)
{
    std::wstring wide;
    if (!ConvertAnsi(subBlock, &wide))
    {
        BridgeSetLastError(ERROR_NO_UNICODE_TRANSLATION);
        return FALSE;
    }
    LPVOID wideValue = nullptr;
    UINT wideLength = 0;
    if (!QueryVersionValue(block, wide.c_str(), &wideValue, &wideLength))
    {
        BridgeSetLastError(ERROR_RESOURCE_DATA_NOT_FOUND);
        return FALSE;
    }
    const auto parts = SplitPath(wide.c_str());
    const bool stringValue = parts.size() >= 3 && _wcsicmp(parts[0].c_str(), L"StringFileInfo") == 0;
    if (!stringValue)
    {
        *value = wideValue;
        *length = wideLength;
        BridgeSetLastError(ERROR_SUCCESS);
        return TRUE;
    }
    static thread_local std::vector<char> converted;
    const wchar_t* source = static_cast<const wchar_t*>(wideValue);
    const int required = WideCharToMultiByte(CP_ACP, 0, source, static_cast<int>(wideLength),
        nullptr, 0, nullptr, nullptr);
    if (required <= 0)
    {
        BridgeSetLastError(ERROR_NO_UNICODE_TRANSLATION);
        return FALSE;
    }
    converted.resize(static_cast<size_t>(required));
    WideCharToMultiByte(CP_ACP, 0, source, static_cast<int>(wideLength),
        converted.data(), required, nullptr, nullptr);
    *value = converted.data();
    *length = static_cast<UINT>(converted.size());
    BridgeSetLastError(ERROR_SUCCESS);
    return TRUE;
}

DWORD WINAPI Win32Bridge::Bridge::BridgeVerLanguageNameW(DWORD language, LPWSTR buffer, DWORD count)
{
    if (!buffer || count == 0) return 0;
    wchar_t localeName[LOCALE_NAME_MAX_LENGTH]{};
    if (!LCIDToLocaleName(language, localeName, ARRAYSIZE(localeName), 0)) return 0;
    const int length = GetLocaleInfoEx(localeName, LOCALE_SLOCALIZEDDISPLAYNAME,
        buffer, static_cast<int>(count));
    return length > 0 ? static_cast<DWORD>(length - 1) : 0;
}

DWORD WINAPI Win32Bridge::Bridge::BridgeVerLanguageNameA(DWORD language, LPSTR buffer, DWORD count)
{
    if (!buffer || count == 0) return 0;
    wchar_t wide[128]{};
    const DWORD wideLength = BridgeVerLanguageNameW(language, wide, ARRAYSIZE(wide));
    if (wideLength == 0) return 0;
    const int result = WideCharToMultiByte(CP_ACP, 0, wide, -1, buffer,
        static_cast<int>(count), nullptr, nullptr);
    return result > 0 ? static_cast<DWORD>(result - 1) : 0;
}

ImportResolution Win32Bridge::Bridge::ResolveVersionImport(const ImportedSymbol& symbol)
{
    auto resolution = CompatibilityCatalog::Resolve(symbol);
    const bool versionLibrary = _wcsicmp(symbol.library.c_str(), L"version.dll") == 0 ||
        _wcsnicmp(symbol.library.c_str(), L"api-ms-win-core-version-", 24) == 0;
    if (symbol.importedByOrdinal || !versionLibrary)
        return resolution;

    if (_wcsicmp(symbol.name.c_str(), L"getfileversioninfosizew") == 0)
        resolution.targetAddress = reinterpret_cast<ULONGLONG>(&BridgeGetFileVersionInfoSizeW);
    else if (_wcsicmp(symbol.name.c_str(), L"getfileversioninfosizea") == 0)
        resolution.targetAddress = reinterpret_cast<ULONGLONG>(&BridgeGetFileVersionInfoSizeA);
    else if (_wcsicmp(symbol.name.c_str(), L"getfileversioninfosizeexw") == 0)
        resolution.targetAddress = reinterpret_cast<ULONGLONG>(&BridgeGetFileVersionInfoSizeExW);
    else if (_wcsicmp(symbol.name.c_str(), L"getfileversioninfosizeexa") == 0)
        resolution.targetAddress = reinterpret_cast<ULONGLONG>(&BridgeGetFileVersionInfoSizeExA);
    else if (_wcsicmp(symbol.name.c_str(), L"getfileversioninfow") == 0)
        resolution.targetAddress = reinterpret_cast<ULONGLONG>(&BridgeGetFileVersionInfoW);
    else if (_wcsicmp(symbol.name.c_str(), L"getfileversioninfoa") == 0)
        resolution.targetAddress = reinterpret_cast<ULONGLONG>(&BridgeGetFileVersionInfoA);
    else if (_wcsicmp(symbol.name.c_str(), L"getfileversioninfoexw") == 0)
        resolution.targetAddress = reinterpret_cast<ULONGLONG>(&BridgeGetFileVersionInfoExW);
    else if (_wcsicmp(symbol.name.c_str(), L"getfileversioninfoexa") == 0)
        resolution.targetAddress = reinterpret_cast<ULONGLONG>(&BridgeGetFileVersionInfoExA);
    else if (_wcsicmp(symbol.name.c_str(), L"verqueryvaluew") == 0)
        resolution.targetAddress = reinterpret_cast<ULONGLONG>(&BridgeVerQueryValueW);
    else if (_wcsicmp(symbol.name.c_str(), L"verqueryvaluea") == 0)
        resolution.targetAddress = reinterpret_cast<ULONGLONG>(&BridgeVerQueryValueA);
    else if (_wcsicmp(symbol.name.c_str(), L"verlanguageNamew") == 0)
        resolution.targetAddress = reinterpret_cast<ULONGLONG>(&BridgeVerLanguageNameW);
    else if (_wcsicmp(symbol.name.c_str(), L"verlanguageNamea") == 0)
        resolution.targetAddress = reinterpret_cast<ULONGLONG>(&BridgeVerLanguageNameA);
    if (resolution.targetAddress != 0)
    {
        resolution.disposition = ImportDisposition::NeedsBridge;
        resolution.note = L"VERSION adapter: reads and queries native VS_VERSION_INFO data from the guest virtual filesystem.";
    }
    return resolution;
}
