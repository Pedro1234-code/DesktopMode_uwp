#pragma once

#include <windows.h>

#include <string>

namespace Win32Bridge
{
namespace Bridge
{
    struct GuestFileDialogDescriptor
    {
        HWND owner = nullptr;
        HINSTANCE instance = nullptr;
        LPCWSTR title = nullptr;
        LPCWSTR initialDirectory = nullptr;
        LPCWSTR initialFileName = nullptr;
        LPCWSTR filter = nullptr;
        DWORD filterIndex = 1;
        LPCWSTR defaultExtension = nullptr;
        DWORD flags = 0;
        BOOL saveDialog = FALSE;
    };

    struct GuestPropertyFieldDescriptor
    {
        LPCWSTR name = nullptr;
        LPCWSTR value = nullptr;
    };

    struct GuestPropertyPageDescriptor
    {
        HINSTANCE instance = nullptr;
        LPCWSTR templateName = nullptr;
        DLGPROC dialogProcedure = nullptr;
        LPARAM initParameter = 0;
        LPCWSTR title = nullptr;
        const GuestPropertyFieldDescriptor* fields = nullptr;
        UINT fieldCount = 0;
    };

    struct GuestPropertySheetDescriptor
    {
        HWND parent = nullptr;
        HINSTANCE instance = nullptr;
        LPCWSTR caption = nullptr;
        const GuestPropertyPageDescriptor* pages = nullptr;
        UINT pageCount = 0;
        UINT startPage = 0;
        DWORD flags = 0;
    };

    INT_PTR ShowGuestDialogFromResource(HINSTANCE instance, LPCWSTR templateName, HWND parent, DLGPROC procedure, LPARAM initParameter);
    INT_PTR ShowGuestDialogFromResourceWithStyles(HINSTANCE instance, LPCWSTR templateName,
        HWND parent, DLGPROC procedure, LPARAM initParameter,
        DWORD stylesToAdd, DWORD stylesToRemove, LPCWSTR titleOverride);
    INT_PTR ShowGuestPropertySheet(const GuestPropertySheetDescriptor& descriptor);
    bool ShowGuestFileDialog(const GuestFileDialogDescriptor& descriptor,
        std::wstring* selectedPath, DWORD* selectedFilterIndex);
    int ShowGuestShellAbout(HWND owner, LPCWSTR title, LPCWSTR text, HICON icon);
    int ShowGuestMessageBox(HWND owner, LPCWSTR text, LPCWSTR caption, UINT type);
    BOOL EndGuestResourceDialog(HWND dialog, INT_PTR result);
    BOOL HandleGuestDialogMessage(HWND dialog, const MSG* message);
    BOOL MapGuestDialogRect(HWND dialog, LPRECT rect);
    int LoadGuestStringResource(HINSTANCE instance, UINT identifier, LPWSTR buffer, int bufferCount);
}
}
