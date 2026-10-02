#pragma once

#include <windows.h>

namespace Win32Bridge
{
namespace Bridge
{
    INT_PTR ShowGuestDialogFromResource(HINSTANCE instance, LPCWSTR templateName, HWND parent, DLGPROC procedure, LPARAM initParameter);
    INT_PTR ShowGuestDialogFromResourceWithStyles(HINSTANCE instance, LPCWSTR templateName,
        HWND parent, DLGPROC procedure, LPARAM initParameter,
        DWORD stylesToAdd, DWORD stylesToRemove, LPCWSTR titleOverride);
    int ShowGuestMessageBox(HWND owner, LPCWSTR text, LPCWSTR caption, UINT type);
    BOOL EndGuestResourceDialog(HWND dialog, INT_PTR result);
    BOOL HandleGuestDialogMessage(HWND dialog, const MSG* message);
    BOOL MapGuestDialogRect(HWND dialog, LPRECT rect);
    int LoadGuestStringResource(HINSTANCE instance, UINT identifier, LPWSTR buffer, int bufferCount);
}
}
