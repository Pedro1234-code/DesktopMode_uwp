#include "pch.h"
#include "Bridge/DialogResources.h"
#include "Bridge/ApiSet.h"
#include "Bridge\\Win32Shims.h"
#include "Bridge\\Gdi32Shims.h"
#include "Bridge/Advapi32Shims.h"
#include "Bridge/CryptoShims.h"
#include "Bridge/CommonControlsShims.h"
#include "Bridge/Comdlg32Shims.h"
#include "Bridge/OleShims.h"
#include "Bridge/MprShims.h"
#include "Bridge/MsvcrtShims.h"
#include "Bridge/Shell32Shims.h"
#include "Bridge\\Kernel32Shims.h"
#include "Bridge\\User32Shims.h"
#include "Bridge/VersionShims.h"

using namespace Win32Bridge::Bridge;

namespace
{
    bool IsUserLibrary(const std::wstring& library)
    {
        return _wcsicmp(library.c_str(), L"user32.dll") == 0 ||
            _wcsnicmp(library.c_str(), L"api-ms-win-ntuser-", 18) == 0 ||
            _wcsnicmp(library.c_str(), L"ext-ms-win-ntuser-", 18) == 0;
    }

    struct GuestUrlComponentsW final
    {
        DWORD size;
        LPWSTR scheme;
        DWORD schemeLength;
        int schemeId;
        LPWSTR host;
        DWORD hostLength;
        USHORT port;
        LPWSTR user;
        DWORD userLength;
        LPWSTR password;
        DWORD passwordLength;
        LPWSTR path;
        DWORD pathLength;
        LPWSTR extra;
        DWORD extraLength;
    };
    static_assert(sizeof(GuestUrlComponentsW) == 104, "URL_COMPONENTSW x64 guest ABI mismatch");

    bool CopyUrlPart(LPWSTR& destination, DWORD& capacity, const wchar_t* source, size_t length)
    {
        if (!destination)
        {
            destination = const_cast<LPWSTR>(source);
            capacity = static_cast<DWORD>(length);
            return true;
        }
        if (capacity <= length)
        {
            capacity = static_cast<DWORD>(length + 1);
            return false;
        }
        if (length) memcpy(destination, source, length * sizeof(wchar_t));
        destination[length] = L'\0';
        capacity = static_cast<DWORD>(length);
        return true;
    }

}

namespace Win32Bridge
{
namespace Bridge
{
int WINAPI BridgeMessageBoxW(HWND owner, LPCWSTR text, LPCWSTR caption, UINT type)
{
    return ShowGuestMessageBox(owner, text, caption, type);
}

BOOL WINAPI BridgeGetCursorPos(LPPOINT point)
{
    if (!point || !MouseInput().IsMouseDetected())
    {
        return FALSE;
    }

    const auto snapshot = MouseInput().Snapshot();
    point->x = static_cast<LONG>(snapshot.x);
    point->y = static_cast<LONG>(snapshot.y);
    return TRUE;
}

SHORT WINAPI BridgeGetAsyncKeyState(int virtualKey)
{
    return MouseInput().GetAsyncKeyState(virtualKey);
}

SHORT WINAPI BridgeGetKeyState(int virtualKey)
{
    return MouseInput().GetKeyState(virtualKey);
}

ULONGLONG MessageBoxWAdapterAddress()
{
    return reinterpret_cast<ULONGLONG>(&BridgeMessageBoxW);
}

BOOL WINAPI BridgeOpenPrinterW(LPWSTR, PHANDLE printer, LPVOID)
{
    if (printer) *printer = nullptr;
    BridgeSetLastError(1801u); // ERROR_INVALID_PRINTER_NAME
    return FALSE;
}

BOOL WINAPI BridgeGetPrinterDriverW(HANDLE, LPWSTR, DWORD, LPBYTE, DWORD, LPDWORD required)
{
    if (required) *required = 0;
    BridgeSetLastError(1797u); // ERROR_UNKNOWN_PRINTER_DRIVER
    return FALSE;
}

BOOL WINAPI BridgeClosePrinter(HANDLE) { return TRUE; }

BOOL WINAPI BridgePathIsFileSpecW(LPCWSTR path)
{
    return path && !wcschr(path, L'\\') && !wcschr(path, L'/') && !wcschr(path, L':');
}

LPWSTR WINAPI BridgePathFindExtensionW(LPCWSTR path)
{
    if (!path) return nullptr;
    const wchar_t* file = path;
    const wchar_t* extension = path + wcslen(path);
    for (const wchar_t* cursor = path; *cursor; ++cursor)
    {
        if (*cursor == L'\\' || *cursor == L'/' || *cursor == L':')
        {
            file = cursor + 1;
            extension = path + wcslen(path);
        }
        else if (*cursor == L'.' && cursor >= file)
        {
            extension = cursor;
        }
    }
    return const_cast<LPWSTR>(extension);
}

LPWSTR WINAPI BridgePathFindFileNameW(LPCWSTR path)
{
    if (!path) return nullptr;
    const wchar_t* result = path;
    for (const wchar_t* cursor = path; *cursor; ++cursor)
        if (*cursor == L'\\' || *cursor == L'/' || *cursor == L':') result = cursor + 1;
    return const_cast<LPWSTR>(result);
}

BOOL WINAPI BridgePathRemoveExtensionW(LPWSTR path)
{
    LPWSTR extension = BridgePathFindExtensionW(path);
    if (!extension || !*extension) return FALSE;
    *extension = L'\0';
    return TRUE;
}

BOOL WINAPI BridgePathRemoveFileSpecW(LPWSTR path)
{
    if (!path || !*path) return FALSE;
    size_t length = wcslen(path);
    while (length > 0 && (path[length - 1] == L'\\' || path[length - 1] == L'/')) --length;
    while (length > 0 && path[length - 1] != L'\\' && path[length - 1] != L'/') --length;
    if (length == 0) return FALSE;
    if (length > 3 || path[length - 1] != L'\\') --length;
    path[length] = L'\0';
    return TRUE;
}

void WINAPI BridgePathStripPathW(LPWSTR path)
{
    if (!path) return;
    LPWSTR file = BridgePathFindFileNameW(path);
    if (file && file != path) memmove(path, file, (wcslen(file) + 1) * sizeof(wchar_t));
}

BOOL WINAPI BridgePathAppendW(LPWSTR path, LPCWSTR more)
{
    if (!path || !more) return FALSE;
    std::wstring result(path);
    while (*more == L'\\' || *more == L'/') ++more;
    if (!result.empty() && result.back() != L'\\' && result.back() != L'/') result.push_back(L'\\');
    result.append(more);
    if (result.size() >= MAX_PATH) return FALSE;
    wcscpy_s(path, MAX_PATH, result.c_str());
    return TRUE;
}

LPWSTR WINAPI BridgePathCombineW(LPWSTR destination, LPCWSTR directory, LPCWSTR file)
{
    if (!destination || (!directory && !file)) return nullptr;
    std::wstring result = directory ? directory : L"";
    if (file && *file)
    {
        const bool absolute = (iswalpha(file[0]) && file[1] == L':') ||
            (file[0] == L'\\' && file[1] == L'\\');
        if (absolute) result = file;
        else
        {
            if (!result.empty() && result.back() != L'\\' && result.back() != L'/') result.push_back(L'\\');
            while (*file == L'\\' || *file == L'/') ++file;
            result.append(file);
        }
    }
    if (result.size() >= MAX_PATH) return nullptr;
    wcscpy_s(destination, MAX_PATH, result.c_str());
    return destination;
}

BOOL WINAPI BridgePathCanonicalizeW(LPWSTR destination, LPCWSTR source)
{
    if (!destination || !source) return FALSE;
    std::wstring input(source);
    std::replace(input.begin(), input.end(), L'/', L'\\');
    std::wstring prefix;
    size_t offset = 0;
    if (input.size() >= 2 && input[1] == L':')
    {
        prefix = input.substr(0, (input.size() >= 3 && input[2] == L'\\') ? 3 : 2);
        offset = prefix.size();
    }
    else if (input.rfind(L"\\\\", 0) == 0)
    {
        prefix = L"\\\\";
        offset = 2;
    }
    std::vector<std::wstring> components;
    while (offset <= input.size())
    {
        const size_t next = input.find(L'\\', offset);
        const std::wstring part = input.substr(offset,
            next == std::wstring::npos ? std::wstring::npos : next - offset);
        if (!part.empty() && part != L".")
        {
            if (part == L".." && !components.empty() && components.back() != L"..") components.pop_back();
            else if (part != L".." || prefix.empty()) components.push_back(part);
        }
        if (next == std::wstring::npos) break;
        offset = next + 1;
    }
    std::wstring result = prefix;
    for (const auto& part : components)
    {
        if (!result.empty() && result.back() != L'\\') result.push_back(L'\\');
        result += part;
    }
    if (result.empty()) result = L".";
    if (result.size() >= MAX_PATH) return FALSE;
    wcscpy_s(destination, MAX_PATH, result.c_str());
    return TRUE;
}

BOOL WINAPI BridgePathCompactPathExW(
    LPWSTR destination, LPCWSTR source, UINT maximumCharacters, DWORD)
{
    if (!destination || !source || maximumCharacters == 0) return FALSE;
    const size_t length = wcslen(source);
    if (length + 1 <= maximumCharacters)
    {
        wcscpy_s(destination, maximumCharacters, source);
        return TRUE;
    }
    if (maximumCharacters < 5)
    {
        destination[0] = L'\0';
        return FALSE;
    }
    const size_t available = maximumCharacters - 4;
    const size_t left = available / 2;
    const size_t right = available - left;
    std::wstring result(source, source + left);
    result += L"...";
    result.append(source + length - right, source + length);
    wcscpy_s(destination, maximumCharacters, result.c_str());
    return TRUE;
}

int WINAPI BridgePathGetDriveNumberW(LPCWSTR path)
{
    return path && iswalpha(path[0]) && path[1] == L':' ? towupper(path[0]) - L'A' : -1;
}

BOOL WINAPI BridgePathIsNetworkPathW(LPCWSTR path)
{
    return path && path[0] == L'\\' && path[1] == L'\\';
}

BOOL WINAPI BridgePathIsRelativeW(LPCWSTR path)
{
    if (!path || !*path) return TRUE;
    return !((iswalpha(path[0]) && path[1] == L':') || BridgePathIsNetworkPathW(path));
}

BOOL WINAPI BridgePathMatchSpecW(LPCWSTR path, LPCWSTR specification)
{
    if (!path || !specification) return FALSE;
    const auto matches = [](const wchar_t* text, const wchar_t* pattern, const auto& self) -> bool
    {
        while (*pattern)
        {
            if (*pattern == L'*')
            {
                while (*pattern == L'*') ++pattern;
                if (!*pattern) return true;
                for (; *text; ++text) if (self(text, pattern, self)) return true;
                return false;
            }
            if (!*text || (*pattern != L'?' && towlower(*pattern) != towlower(*text))) return false;
            ++text; ++pattern;
        }
        return *text == L'\0';
    };
    const wchar_t* current = specification;
    while (*current)
    {
        const wchar_t* separator = wcschr(current, L';');
        const std::wstring pattern(current,
            separator ? separator : current + wcslen(current));
        if (matches(path, pattern.c_str(), matches)) return TRUE;
        if (!separator) break;
        current = separator + 1;
    }
    return FALSE;
}

HRESULT WINAPI BridgeAssocQueryStringW(
    DWORD, DWORD, LPCWSTR, LPCWSTR, LPWSTR output, DWORD* characters)
{
    if (output && characters && *characters) output[0] = L'\0';
    return HRESULT_FROM_WIN32(ERROR_NO_ASSOCIATION);
}

COLORREF WINAPI BridgeColorHLSToRGB(WORD hue, WORD luminance, WORD saturation)
{
    const double h = (hue % 240) / 240.0;
    const double l = (std::min<WORD>)(luminance, 240) / 240.0;
    const double s = (std::min<WORD>)(saturation, 240) / 240.0;
    if (s == 0.0)
    {
        const BYTE value = static_cast<BYTE>(l * 255.0 + 0.5);
        return RGB(value, value, value);
    }
    const double q = l < 0.5 ? l * (1.0 + s) : l + s - l * s;
    const double p = 2.0 * l - q;
    const auto channel = [p, q](double t)
    {
        if (t < 0.0) t += 1.0;
        if (t > 1.0) t -= 1.0;
        const double value = t < 1.0 / 6.0 ? p + (q - p) * 6.0 * t :
            t < 0.5 ? q : t < 2.0 / 3.0 ? p + (q - p) * (2.0 / 3.0 - t) * 6.0 : p;
        return static_cast<BYTE>(value * 255.0 + 0.5);
    };
    return RGB(channel(h + 1.0 / 3.0), channel(h), channel(h - 1.0 / 3.0));
}

void WINAPI BridgeColorRGBToHLS(COLORREF color, WORD* hue, WORD* luminance, WORD* saturation)
{
    const double r = GetRValue(color) / 255.0, g = GetGValue(color) / 255.0, b = GetBValue(color) / 255.0;
    const double maximum = (std::max)(r, (std::max)(g, b));
    const double minimum = (std::min)(r, (std::min)(g, b));
    const double delta = maximum - minimum;
    const double l = (maximum + minimum) / 2.0;
    double h = 0.0, s = 0.0;
    if (delta != 0.0)
    {
        s = l < 0.5 ? delta / (maximum + minimum) : delta / (2.0 - maximum - minimum);
        h = maximum == r ? (g - b) / delta : maximum == g ? 2.0 + (b - r) / delta : 4.0 + (r - g) / delta;
        h /= 6.0;
        if (h < 0.0) h += 1.0;
    }
    if (hue) *hue = static_cast<WORD>(h * 240.0 + 0.5);
    if (luminance) *luminance = static_cast<WORD>(l * 240.0 + 0.5);
    if (saturation) *saturation = static_cast<WORD>(s * 240.0 + 0.5);
}

COLORREF WINAPI BridgeColorAdjustLuma(COLORREF color, int amount, BOOL scale)
{
    WORD h = 0, l = 0, s = 0;
    BridgeColorRGBToHLS(color, &h, &l, &s);
    const int adjusted = scale ? l + (240 - l) * amount / 1000 : l + amount * 240 / 1000;
    return BridgeColorHLSToRGB(h, static_cast<WORD>((std::max)(0, (std::min)(240, adjusted))), s);
}

HRESULT WINAPI BridgeSHStrDupW(LPCWSTR source, LPWSTR* result)
{
    if (!result) return E_POINTER;
    *result = nullptr;
    if (!source) return E_INVALIDARG;
    const size_t bytes = (wcslen(source) + 1) * sizeof(wchar_t);
    LPWSTR copy = static_cast<LPWSTR>(BridgeCoTaskMemAlloc(bytes));
    if (!copy) return E_OUTOFMEMORY;
    memcpy(copy, source, bytes);
    *result = copy;
    return S_OK;
}

HRESULT WINAPI BridgeDwmSetWindowAttribute(HWND, DWORD, LPCVOID, DWORD) { return S_OK; }
HRESULT WINAPI BridgeDwmGetColorizationColor(DWORD* color, BOOL* opaque)
{
    if (!color || !opaque) return E_INVALIDARG;
    *color = 0xff0078d4u;
    *opaque = TRUE;
    return S_OK;
}

BOOL WINAPI BridgeIsNetworkAlive(LPDWORD flags)
{
    if (flags) *flags = 0;
    BridgeSetLastError(ERROR_NETWORK_UNREACHABLE);
    return FALSE;
}

BOOL WINAPI BridgeIsDestinationReachableW(LPCWSTR, PVOID)
{
    BridgeSetLastError(ERROR_NETWORK_UNREACHABLE);
    return FALSE;
}

PIMAGE_NT_HEADERS WINAPI BridgeImageNtHeader(PVOID base)
{
    if (!base) return nullptr;
    __try
    {
        auto* dos = static_cast<PIMAGE_DOS_HEADER>(base);
        if (dos->e_magic != IMAGE_DOS_SIGNATURE || dos->e_lfanew <= 0) return nullptr;
        auto* nt = reinterpret_cast<PIMAGE_NT_HEADERS>(
            static_cast<BYTE*>(base) + static_cast<size_t>(dos->e_lfanew));
        return nt->Signature == IMAGE_NT_SIGNATURE ? nt : nullptr;
    }
    __except (EXCEPTION_EXECUTE_HANDLER) { return nullptr; }
}

LONG WINAPI BridgeWinVerifyTrust(HWND, GUID*, PVOID)
{
    // Trust must never be fabricated.  The guest currently has no certificate
    // chain engine, so unsigned/unverifiable is the only safe answer.
    return static_cast<LONG>(0x800B0100u); // TRUST_E_NOSIGNATURE
}

BOOL WINAPI BridgeInternetCrackUrlW(
    LPCWSTR url, DWORD urlLength, DWORD, GuestUrlComponentsW* parts)
{
    if (!url || !parts || parts->size < sizeof(GuestUrlComponentsW))
    {
        BridgeSetLastError(ERROR_INVALID_PARAMETER);
        return FALSE;
    }
    const size_t length = urlLength ? urlLength : wcslen(url);
    const std::wstring value(url, length);
    size_t schemeEnd = value.find(L':');
    if (schemeEnd == std::wstring::npos)
    {
        BridgeSetLastError(ERROR_INVALID_PARAMETER);
        return FALSE;
    }
    const std::wstring scheme = value.substr(0, schemeEnd);
    parts->schemeId = _wcsicmp(scheme.c_str(), L"http") == 0 ? 3 :
        _wcsicmp(scheme.c_str(), L"https") == 0 ? 4 :
        _wcsicmp(scheme.c_str(), L"ftp") == 0 ? 1 :
        _wcsicmp(scheme.c_str(), L"file") == 0 ? 5 : 0;
    size_t authority = schemeEnd + 1;
    if (authority + 1 < length && value[authority] == L'/' && value[authority + 1] == L'/') authority += 2;
    size_t pathAt = value.find_first_of(L"/?#", authority);
    const size_t authorityEnd = pathAt == std::wstring::npos ? length : pathAt;
    size_t hostAt = authority;
    size_t at = value.find(L'@', authority);
    size_t colon = value.find(L':', authority);
    size_t userLength = 0, passwordAt = 0, passwordLength = 0;
    if (at != std::wstring::npos && at < authorityEnd)
    {
        if (colon != std::wstring::npos && colon < at)
        {
            userLength = colon - authority;
            passwordAt = colon + 1;
            passwordLength = at - passwordAt;
        }
        else userLength = at - authority;
        hostAt = at + 1;
    }
    size_t portColon = value.rfind(L':', authorityEnd);
    size_t hostEnd = authorityEnd;
    parts->port = parts->schemeId == 4 ? 443 : parts->schemeId == 3 ? 80 : parts->schemeId == 1 ? 21 : 0;
    if (portColon != std::wstring::npos && portColon >= hostAt && portColon < authorityEnd)
    {
        hostEnd = portColon;
        wchar_t* end = nullptr;
        const unsigned long parsed = wcstoul(value.c_str() + portColon + 1, &end, 10);
        if (end == value.c_str() + authorityEnd && parsed <= 65535) parts->port = static_cast<USHORT>(parsed);
    }
    size_t extraAt = pathAt == std::wstring::npos ? length : value.find_first_of(L"?#", pathAt);
    if (extraAt == std::wstring::npos) extraAt = length;
    const size_t actualPathAt = pathAt == std::wstring::npos ? length : pathAt;
    bool ok = true;
    ok = CopyUrlPart(parts->scheme, parts->schemeLength, url, schemeEnd) && ok;
    ok = CopyUrlPart(parts->host, parts->hostLength, url + hostAt, hostEnd - hostAt) && ok;
    ok = CopyUrlPart(parts->user, parts->userLength, url + authority, userLength) && ok;
    ok = CopyUrlPart(parts->password, parts->passwordLength, url + passwordAt, passwordLength) && ok;
    ok = CopyUrlPart(parts->path, parts->pathLength, url + actualPathAt, extraAt - actualPathAt) && ok;
    ok = CopyUrlPart(parts->extra, parts->extraLength, url + extraAt, length - extraAt) && ok;
    if (!ok) BridgeSetLastError(ERROR_INSUFFICIENT_BUFFER);
    else BridgeSetLastError(ERROR_SUCCESS);
    return ok ? TRUE : FALSE;
}

ImportResolution ResolveAuxiliaryImport(const ImportedSymbol& symbol)
{
    ImportResolution resolution = CompatibilityCatalog::Resolve(symbol);
    if (_wcsicmp(symbol.library.c_str(), L"winspool.drv") == 0)
    {
        if (_wcsicmp(symbol.name.c_str(), L"openprinterw") == 0) resolution.targetAddress = reinterpret_cast<ULONGLONG>(&BridgeOpenPrinterW);
        else if (_wcsicmp(symbol.name.c_str(), L"getprinterdriverw") == 0) resolution.targetAddress = reinterpret_cast<ULONGLONG>(&BridgeGetPrinterDriverW);
        else if (_wcsicmp(symbol.name.c_str(), L"closeprinter") == 0) resolution.targetAddress = reinterpret_cast<ULONGLONG>(&BridgeClosePrinter);
    }
    else if (_wcsicmp(symbol.library.c_str(), L"shlwapi.dll") == 0)
    {
        if (_wcsicmp(symbol.name.c_str(), L"pathisfilespecw") == 0) resolution.targetAddress = reinterpret_cast<ULONGLONG>(&BridgePathIsFileSpecW);
        else if (_wcsicmp(symbol.name.c_str(), L"shstrdupw") == 0) resolution.targetAddress = reinterpret_cast<ULONGLONG>(&BridgeSHStrDupW);
        else if (_wcsicmp(symbol.name.c_str(), L"pathappendw") == 0) resolution.targetAddress = reinterpret_cast<ULONGLONG>(&BridgePathAppendW);
        else if (_wcsicmp(symbol.name.c_str(), L"pathcanonicalizew") == 0) resolution.targetAddress = reinterpret_cast<ULONGLONG>(&BridgePathCanonicalizeW);
        else if (_wcsicmp(symbol.name.c_str(), L"pathcombinew") == 0) resolution.targetAddress = reinterpret_cast<ULONGLONG>(&BridgePathCombineW);
        else if (_wcsicmp(symbol.name.c_str(), L"pathcompactpathexw") == 0) resolution.targetAddress = reinterpret_cast<ULONGLONG>(&BridgePathCompactPathExW);
        else if (_wcsicmp(symbol.name.c_str(), L"pathfindextensionw") == 0) resolution.targetAddress = reinterpret_cast<ULONGLONG>(&BridgePathFindExtensionW);
        else if (_wcsicmp(symbol.name.c_str(), L"pathfindfilenamew") == 0) resolution.targetAddress = reinterpret_cast<ULONGLONG>(&BridgePathFindFileNameW);
        else if (_wcsicmp(symbol.name.c_str(), L"pathgetdrivenumberw") == 0) resolution.targetAddress = reinterpret_cast<ULONGLONG>(&BridgePathGetDriveNumberW);
        else if (_wcsicmp(symbol.name.c_str(), L"pathisnetworkpathw") == 0) resolution.targetAddress = reinterpret_cast<ULONGLONG>(&BridgePathIsNetworkPathW);
        else if (_wcsicmp(symbol.name.c_str(), L"pathisrelativew") == 0) resolution.targetAddress = reinterpret_cast<ULONGLONG>(&BridgePathIsRelativeW);
        else if (_wcsicmp(symbol.name.c_str(), L"pathmatchspecw") == 0) resolution.targetAddress = reinterpret_cast<ULONGLONG>(&BridgePathMatchSpecW);
        else if (_wcsicmp(symbol.name.c_str(), L"pathremoveextensionw") == 0) resolution.targetAddress = reinterpret_cast<ULONGLONG>(&BridgePathRemoveExtensionW);
        else if (_wcsicmp(symbol.name.c_str(), L"pathremovefilespecw") == 0) resolution.targetAddress = reinterpret_cast<ULONGLONG>(&BridgePathRemoveFileSpecW);
        else if (_wcsicmp(symbol.name.c_str(), L"pathstrippathw") == 0) resolution.targetAddress = reinterpret_cast<ULONGLONG>(&BridgePathStripPathW);
        else if (_wcsicmp(symbol.name.c_str(), L"assocquerystringw") == 0) resolution.targetAddress = reinterpret_cast<ULONGLONG>(&BridgeAssocQueryStringW);
        else if (_wcsicmp(symbol.name.c_str(), L"coloradjustluma") == 0) resolution.targetAddress = reinterpret_cast<ULONGLONG>(&BridgeColorAdjustLuma);
        else if (_wcsicmp(symbol.name.c_str(), L"colorhlstorgb") == 0) resolution.targetAddress = reinterpret_cast<ULONGLONG>(&BridgeColorHLSToRGB);
        else if (_wcsicmp(symbol.name.c_str(), L"colorrgbtohls") == 0) resolution.targetAddress = reinterpret_cast<ULONGLONG>(&BridgeColorRGBToHLS);
    }
    else if (_wcsicmp(symbol.library.c_str(), L"dwmapi.dll") == 0)
    {
        if (_wcsicmp(symbol.name.c_str(), L"dwmsetwindowattribute") == 0) resolution.targetAddress = reinterpret_cast<ULONGLONG>(&BridgeDwmSetWindowAttribute);
        else if (_wcsicmp(symbol.name.c_str(), L"dwmgetcolorizationcolor") == 0) resolution.targetAddress = reinterpret_cast<ULONGLONG>(&BridgeDwmGetColorizationColor);
    }
    else if (_wcsicmp(symbol.library.c_str(), L"sensapi.dll") == 0)
    {
        if (_wcsicmp(symbol.name.c_str(), L"isnetworkalive") == 0) resolution.targetAddress = reinterpret_cast<ULONGLONG>(&BridgeIsNetworkAlive);
        else if (_wcsicmp(symbol.name.c_str(), L"isdestinationreachablew") == 0) resolution.targetAddress = reinterpret_cast<ULONGLONG>(&BridgeIsDestinationReachableW);
    }
    else if (_wcsicmp(symbol.library.c_str(), L"dbghelp.dll") == 0 && _wcsicmp(symbol.name.c_str(), L"imagentheader") == 0)
        resolution.targetAddress = reinterpret_cast<ULONGLONG>(&BridgeImageNtHeader);
    else if (_wcsicmp(symbol.library.c_str(), L"wintrust.dll") == 0 && _wcsicmp(symbol.name.c_str(), L"winverifytrust") == 0)
        resolution.targetAddress = reinterpret_cast<ULONGLONG>(&BridgeWinVerifyTrust);
    else if (_wcsicmp(symbol.library.c_str(), L"wininet.dll") == 0 && _wcsicmp(symbol.name.c_str(), L"internetcrackurlw") == 0)
        resolution.targetAddress = reinterpret_cast<ULONGLONG>(&BridgeInternetCrackUrlW);
    if (resolution.targetAddress) resolution.disposition = ImportDisposition::NeedsBridge;
    return resolution;
}

ImportResolution ResolveRuntimeImport(const ImportedSymbol& symbol)
{
    ImportedSymbol canonical = symbol;
    canonical.library = ApiSetHostLibrary(symbol.library);
    auto resolution = CompatibilityCatalog::Resolve(canonical);
    const auto versionResolution = ResolveVersionImport(canonical);
    if (versionResolution.targetAddress != 0) return versionResolution;
    if (_wcsicmp(canonical.library.c_str(), L"kernel32.dll") == 0 ||
        _wcsicmp(canonical.library.c_str(), L"kernelbase.dll") == 0 ||
        _wcsicmp(canonical.library.c_str(), L"ntdll.dll") == 0)
    {
        return ResolveKernel32Import(canonical);
    }
    const auto registryResolution = ResolveAdvapi32Import(canonical);
    if (registryResolution.targetAddress != 0) return registryResolution;
    const auto cryptoResolution = ResolveCryptoImport(canonical);
    if (cryptoResolution.targetAddress != 0) return cryptoResolution;
    const auto commonControlsResolution = ResolveCommonControlsImport(canonical);
    if (commonControlsResolution.targetAddress != 0) return commonControlsResolution;
    const auto commonDialogResolution = ResolveComdlg32Import(canonical);
    if (commonDialogResolution.targetAddress != 0) return commonDialogResolution;
    const auto oleResolution = ResolveOleImport(canonical);
    if (oleResolution.targetAddress != 0) return oleResolution;
    const auto mprResolution = ResolveMprImport(canonical);
    if (mprResolution.targetAddress != 0) return mprResolution;
    const auto shellResolution = ResolveShell32Import(canonical);
    if (shellResolution.targetAddress != 0) return shellResolution;
    const auto msvcrtResolution = ResolveMsvcrtImport(canonical);
    if (msvcrtResolution.targetAddress != 0) return msvcrtResolution;
    const auto auxiliaryResolution = ResolveAuxiliaryImport(canonical);
    if (auxiliaryResolution.targetAddress != 0) return auxiliaryResolution;

    const auto gdiResolution = ResolveGdi32Import(canonical);
    if (gdiResolution.targetAddress != 0)
    {
        return gdiResolution;
    }

    const auto userResolution = ResolveUser32Import(canonical);
    if (userResolution.targetAddress != 0)
    {
        return userResolution;
    }

    if (!IsUserLibrary(canonical.library))
    {
        return resolution;
    }

    if (_wcsicmp(canonical.name.c_str(), L"messageboxw") == 0)
    {
        resolution.targetAddress = MessageBoxWAdapterAddress();
    }
    else if (_wcsicmp(canonical.name.c_str(), L"getcursorpos") == 0)
    {
        resolution.targetAddress = reinterpret_cast<ULONGLONG>(&BridgeGetCursorPos);
    }
    else if (_wcsicmp(canonical.name.c_str(), L"getasynckeystate") == 0)
    {
        resolution.targetAddress = reinterpret_cast<ULONGLONG>(&BridgeGetAsyncKeyState);
    }
    else if (_wcsicmp(canonical.name.c_str(), L"getkeystate") == 0)
    {
        resolution.targetAddress = reinterpret_cast<ULONGLONG>(&BridgeGetKeyState);
    }
    return resolution;
}
}
}
