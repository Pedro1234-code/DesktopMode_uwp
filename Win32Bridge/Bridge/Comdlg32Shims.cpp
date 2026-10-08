#include "pch.h"
#include "Bridge/Comdlg32Shims.h"
#include "Bridge/DialogResources.h"

namespace
{
    thread_local DWORD g_commonDialogError = 0;
    constexpr DWORD kCdErrInitialization = 0x0002;
    constexpr DWORD kFnErrBufferTooSmall = 0x3003;
    constexpr DWORD kOfnExtensionDifferent = 0x00000400;

    bool IsComdlgLibrary(const std::wstring& library)
    {
        return _wcsicmp(library.c_str(), L"comdlg32.dll") == 0;
    }

    bool IsName(const std::wstring& value, const wchar_t* expected)
    {
        return _wcsicmp(value.c_str(), expected) == 0;
    }

    void SetCommonDialogError(DWORD error)
    {
        g_commonDialogError = error;
    }

    bool CopyGuestPathToCaller(Win32Bridge::Bridge::GuestOpenFileNameW* openFileName, const std::wstring& guestPath)
    {
        if (!openFileName || !openFileName->lpstrFile || openFileName->nMaxFile == 0)
        {
            SetCommonDialogError(kCdErrInitialization);
            return false;
        }

        if (guestPath.size() + 1 > openFileName->nMaxFile)
        {
            // This is the documented failure for an OPENFILENAME buffer that
            // cannot hold the returned path.  Keep the path virtual: no host
            // filesystem path may escape into the PE.
            openFileName->lpstrFile[0] = L'\0';
            SetCommonDialogError(kFnErrBufferTooSmall);
            return false;
        }

        wcscpy_s(openFileName->lpstrFile, openFileName->nMaxFile, guestPath.c_str());
        const size_t separator = guestPath.find_last_of(L'\\');
        openFileName->nFileOffset = static_cast<WORD>(separator == std::wstring::npos ? 0 : separator + 1);
        const size_t dot = guestPath.find_last_of(L'.');
        openFileName->nFileExtension = static_cast<WORD>(
            dot == std::wstring::npos ||
            (separator != std::wstring::npos && dot < separator)
                ? 0 : dot + 1);
        if (openFileName->lpstrFileTitle && openFileName->nMaxFileTitle != 0)
        {
            const std::wstring title = separator == std::wstring::npos
                ? guestPath : guestPath.substr(separator + 1);
            wcsncpy_s(openFileName->lpstrFileTitle,
                openFileName->nMaxFileTitle, title.c_str(), _TRUNCATE);
        }
        SetCommonDialogError(0);
        return true;
    }

    bool RunVirtualFileDialog(
        Win32Bridge::Bridge::GuestOpenFileNameW* openFileName,
        bool saveDialog)
    {
        if (!openFileName || !openFileName->lpstrFile ||
            openFileName->nMaxFile == 0)
        {
            SetCommonDialogError(kCdErrInitialization);
            return false;
        }

        std::wstring initialFile(openFileName->lpstrFile);
        std::wstring embeddedDirectory;
        const size_t separator = initialFile.find_last_of(L"\\/");
        if (separator != std::wstring::npos)
        {
            embeddedDirectory = initialFile.substr(0, separator);
            initialFile.erase(0, separator + 1);
        }

        Win32Bridge::Bridge::GuestFileDialogDescriptor descriptor;
        descriptor.owner = openFileName->hwndOwner;
        descriptor.instance = openFileName->hInstance;
        descriptor.title = openFileName->lpstrTitle;
        descriptor.initialDirectory = openFileName->lpstrInitialDir &&
            *openFileName->lpstrInitialDir
            ? openFileName->lpstrInitialDir
            : (embeddedDirectory.empty() ? nullptr : embeddedDirectory.c_str());
        descriptor.initialFileName = initialFile.c_str();
        descriptor.filter = openFileName->lpstrFilter;
        descriptor.filterIndex = openFileName->nFilterIndex;
        descriptor.defaultExtension = openFileName->lpstrDefExt;
        descriptor.flags = openFileName->Flags;
        descriptor.saveDialog = saveDialog ? TRUE : FALSE;

        std::wstring selected;
        DWORD selectedFilter = descriptor.filterIndex;
        if (!Win32Bridge::Bridge::ShowGuestFileDialog(
            descriptor, &selected, &selectedFilter))
        {
            SetCommonDialogError(0);
            return false;
        }
        openFileName->nFilterIndex = selectedFilter;
        if (openFileName->lpstrDefExt && *openFileName->lpstrDefExt)
        {
            const size_t dot = selected.find_last_of(L'.');
            const size_t slash = selected.find_last_of(L'\\');
            if (dot != std::wstring::npos &&
                (slash == std::wstring::npos || dot > slash) &&
                _wcsicmp(selected.c_str() + dot + 1,
                    openFileName->lpstrDefExt[0] == L'.'
                        ? openFileName->lpstrDefExt + 1
                        : openFileName->lpstrDefExt) != 0)
            {
                openFileName->Flags |= kOfnExtensionDifferent;
            }
            else
            {
                openFileName->Flags &= ~kOfnExtensionDifferent;
            }
        }
        return CopyGuestPathToCaller(openFileName, selected);
    }
}

BOOL WINAPI Win32Bridge::Bridge::BridgeGetOpenFileNameW(GuestOpenFileNameW* openFileName)
{
    return RunVirtualFileDialog(openFileName, false) ? TRUE : FALSE;
}

BOOL WINAPI Win32Bridge::Bridge::BridgeGetSaveFileNameW(GuestOpenFileNameW* openFileName)
{
    return RunVirtualFileDialog(openFileName, true) ? TRUE : FALSE;
}

DWORD WINAPI Win32Bridge::Bridge::BridgeCommDlgExtendedError()
{
    return g_commonDialogError;
}

namespace Win32Bridge { namespace Bridge {
HWND WINAPI BridgeFindTextW(PVOID) { SetCommonDialogError(0); return nullptr; }
HWND WINAPI BridgeReplaceTextW(PVOID) { SetCommonDialogError(0); return nullptr; }
BOOL WINAPI BridgePageSetupDlgW(PVOID) { SetCommonDialogError(0); return FALSE; }
BOOL WINAPI BridgeChooseFontW(PVOID) { SetCommonDialogError(0); return FALSE; }
BOOL WINAPI BridgeChooseColorW(PVOID) { SetCommonDialogError(0); return FALSE; }
// Printing is intentionally unavailable in the sandbox.  Report the same
// observable result as a user-cancelled common dialog, rather than inventing a
// printer or allowing the guest to reach the host print subsystem.
BOOL WINAPI BridgePrintDlgW(PVOID) { SetCommonDialogError(0); return FALSE; }
HRESULT WINAPI BridgePrintDlgExW(PVOID) { SetCommonDialogError(0); return E_NOTIMPL; }
short WINAPI BridgeGetFileTitleW(LPCWSTR path, LPWSTR title, WORD capacity)
{
    if (!path || !title || capacity == 0) return -1;
    const wchar_t* slash = wcsrchr(path, L'\\');
    const wchar_t* forward = wcsrchr(path, L'/');
    const wchar_t* name = slash && (!forward || slash > forward) ? slash + 1 :
        forward ? forward + 1 : path;
    const size_t length = wcslen(name);
    if (length + 1 > capacity) return -1;
    wcscpy_s(title, capacity, name);
    return 0;
}
} }

Win32Bridge::Bridge::ImportResolution Win32Bridge::Bridge::ResolveComdlg32Import(const ImportedSymbol& symbol)
{
    auto resolution = CompatibilityCatalog::Resolve(symbol);
    if (!IsComdlgLibrary(symbol.library))
    {
        return resolution;
    }

    if (IsName(symbol.name, L"getopenfilenamew"))
    {
        resolution.targetAddress = reinterpret_cast<ULONGLONG>(&BridgeGetOpenFileNameW);
    }
    else if (IsName(symbol.name, L"getsavefilenamew"))
    {
        resolution.targetAddress = reinterpret_cast<ULONGLONG>(&BridgeGetSaveFileNameW);
    }
    else if (IsName(symbol.name, L"commdlgextendederror"))
    {
        resolution.targetAddress = reinterpret_cast<ULONGLONG>(&BridgeCommDlgExtendedError);
    }
    else if (IsName(symbol.name, L"findtextw")) resolution.targetAddress = reinterpret_cast<ULONGLONG>(&BridgeFindTextW);
    else if (IsName(symbol.name, L"replacetextw")) resolution.targetAddress = reinterpret_cast<ULONGLONG>(&BridgeReplaceTextW);
    else if (IsName(symbol.name, L"pagesetupdlgw")) resolution.targetAddress = reinterpret_cast<ULONGLONG>(&BridgePageSetupDlgW);
    else if (IsName(symbol.name, L"choosefontw")) resolution.targetAddress = reinterpret_cast<ULONGLONG>(&BridgeChooseFontW);
    else if (IsName(symbol.name, L"choosecolorw")) resolution.targetAddress = reinterpret_cast<ULONGLONG>(&BridgeChooseColorW);
    else if (IsName(symbol.name, L"getfiletitlew")) resolution.targetAddress = reinterpret_cast<ULONGLONG>(&BridgeGetFileTitleW);
    else if (IsName(symbol.name, L"printdlgw")) resolution.targetAddress = reinterpret_cast<ULONGLONG>(&BridgePrintDlgW);
    else if (IsName(symbol.name, L"printdlgexw")) resolution.targetAddress = reinterpret_cast<ULONGLONG>(&BridgePrintDlgExW);

    if (resolution.targetAddress)
    {
        resolution.disposition = ImportDisposition::NeedsBridge;
    }
    return resolution;
}
