#pragma once

#include "Bridge/CompatibilityCatalog.h"
#include "Bridge\\MiniGdi.h"

namespace Win32Bridge
{
namespace Bridge
{
    // comctl32 is not part of the UWP SDK.  These declarations mirror only
    // the pointer-shaped values that cross the guest ABI.
    struct GuestInitCommonControlsEx
    {
        DWORD dwSize;
        DWORD dwICC;
    };

    struct GuestDllVersionInfo
    {
        DWORD cbSize;
        DWORD dwMajorVersion;
        DWORD dwMinorVersion;
        DWORD dwBuildNumber;
        DWORD dwPlatformID;
    };

    using GuestImageList = HANDLE;

    void WINAPI BridgeInitCommonControls();
    BOOL WINAPI BridgeInitCommonControlsEx(const GuestInitCommonControlsEx* controls);
    GuestImageList WINAPI BridgeImageListCreate(int width, int height, UINT flags, int initial, int grow);
    BOOL WINAPI BridgeImageListDestroy(GuestImageList imageList);
    int WINAPI BridgeImageListAddMasked(GuestImageList imageList, HBITMAP bitmap, COLORREF mask);
    int WINAPI BridgeImageListGetImageCount(GuestImageList imageList);
    int WINAPI BridgeImageListReplaceIcon(GuestImageList imageList, int index, HICON icon);
    bool CopyGuestImageListImage(GuestImageList imageList, int index, MiniGdi::Surface* destination);
    HWND WINAPI BridgeCreateToolbarEx(HWND parent, DWORD style, UINT identifier, int bitmapCount, HINSTANCE instance, UINT_PTR bitmapId, const void* buttons, int buttonCount, int buttonWidth, int buttonHeight, int bitmapWidth, int bitmapHeight, UINT structureSize);
    HWND WINAPI BridgeCreateStatusWindowW(LONG style, LPCWSTR text, HWND parent, UINT identifier);
    INT_PTR WINAPI BridgePropertySheetW(const void* header);
    HRESULT WINAPI BridgeDllGetVersion(GuestDllVersionInfo* versionInfo);
    HANDLE WINAPI BridgeCreatePropertySheetPageW(const void* page);
    ULONG_PTR WINAPI BridgeCommonControlOrdinal345();
    ImportResolution ResolveCommonControlsImport(const ImportedSymbol& symbol);
}
}
