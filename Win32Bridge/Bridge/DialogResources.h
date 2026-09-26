#pragma once

#include <windows.h>

namespace Win32Bridge
{
namespace Bridge
{
    INT_PTR ShowGuestDialogFromResource(HINSTANCE instance, LPCWSTR templateName, HWND parent, DLGPROC procedure, LPARAM initParameter);
    BOOL EndGuestResourceDialog(HWND dialog, INT_PTR result);
    int LoadGuestStringResource(UINT identifier, LPWSTR buffer, int bufferCount);
}
}
