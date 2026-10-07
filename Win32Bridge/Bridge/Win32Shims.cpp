#include "pch.h"
#include "Bridge/DialogResources.h"
#include "Bridge/ApiSet.h"
#include "Bridge\\Win32Shims.h"
#include "Bridge\\Gdi32Shims.h"
#include "Bridge/Advapi32Shims.h"
#include "Bridge/CommonControlsShims.h"
#include "Bridge/Comdlg32Shims.h"
#include "Bridge/OleShims.h"
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
    }
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
    const auto commonControlsResolution = ResolveCommonControlsImport(canonical);
    if (commonControlsResolution.targetAddress != 0) return commonControlsResolution;
    const auto commonDialogResolution = ResolveComdlg32Import(canonical);
    if (commonDialogResolution.targetAddress != 0) return commonDialogResolution;
    const auto oleResolution = ResolveOleImport(canonical);
    if (oleResolution.targetAddress != 0) return oleResolution;
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
