#pragma once

#include "Bridge/CompatibilityCatalog.h"

namespace Win32Bridge
{
namespace Bridge
{
    // OPENFILENAMEW is a desktop-SDK declaration.  Keep the guest ABI here so
    // the UWP build neither includes nor links against comdlg32.
    struct GuestOpenFileNameW
    {
        DWORD lStructSize;
        HWND hwndOwner;
        HINSTANCE hInstance;
        LPCWSTR lpstrFilter;
        LPWSTR lpstrCustomFilter;
        DWORD nMaxCustFilter;
        DWORD nFilterIndex;
        LPWSTR lpstrFile;
        DWORD nMaxFile;
        LPWSTR lpstrFileTitle;
        DWORD nMaxFileTitle;
        LPCWSTR lpstrInitialDir;
        LPCWSTR lpstrTitle;
        DWORD Flags;
        WORD nFileOffset;
        WORD nFileExtension;
        LPCWSTR lpstrDefExt;
        LPARAM lCustData;
        PVOID lpfnHook;
        LPCWSTR lpTemplateName;
        PVOID pvReserved;
        DWORD dwReserved;
        DWORD FlagsEx;
    };

    BOOL WINAPI BridgeGetOpenFileNameW(GuestOpenFileNameW* openFileName);
    BOOL WINAPI BridgeGetSaveFileNameW(GuestOpenFileNameW* openFileName);
    DWORD WINAPI BridgeCommDlgExtendedError();

    ImportResolution ResolveComdlg32Import(const ImportedSymbol& symbol);
}
}
