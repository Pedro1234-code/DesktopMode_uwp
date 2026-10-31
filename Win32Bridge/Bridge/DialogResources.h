#pragma once

#include <windows.h>

#include <string>

namespace Win32Bridge
{
namespace Bridge
{
    void ResetGuestDialogState();

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
        BOOL folderDialog = FALSE;
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

    struct GuestTaskDialogButtonDescriptor
    {
        int identifier = 0;
        LPCWSTR text = nullptr;
    };

    using GuestTaskDialogCallback = HRESULT(CALLBACK*)(
        HWND window, UINT notification, WPARAM wParam, LPARAM lParam,
        LONG_PTR callbackData);

    struct GuestTaskDialogDescriptor
    {
        HWND owner = nullptr;
        HINSTANCE instance = nullptr;
        LPCWSTR title = nullptr;
        LPCWSTR mainInstruction = nullptr;
        LPCWSTR content = nullptr;
        const GuestTaskDialogButtonDescriptor* buttons = nullptr;
        UINT buttonCount = 0;
        int defaultButton = 0;
        const GuestTaskDialogButtonDescriptor* radioButtons = nullptr;
        UINT radioButtonCount = 0;
        int defaultRadioButton = 0;
        LPCWSTR verificationText = nullptr;
        BOOL verificationChecked = FALSE;
        GuestTaskDialogCallback callback = nullptr;
        LONG_PTR callbackData = 0;
    };

    INT_PTR ShowGuestDialogFromResource(HINSTANCE instance, LPCWSTR templateName, HWND parent, DLGPROC procedure, LPARAM initParameter);
    INT_PTR ShowGuestDialogFromTemplate(HINSTANCE instance, const void* templateData,
        HWND parent, DLGPROC procedure, LPARAM initParameter, bool modal, HWND* createdWindow);
    INT_PTR ShowGuestDialogFromResourceWithStyles(HINSTANCE instance, LPCWSTR templateName,
        HWND parent, DLGPROC procedure, LPARAM initParameter,
        DWORD stylesToAdd, DWORD stylesToRemove, LPCWSTR titleOverride,
        bool modal = true, HWND* createdWindow = nullptr);
    INT_PTR ShowGuestPropertySheet(const GuestPropertySheetDescriptor& descriptor);
    bool ShowGuestFileDialog(const GuestFileDialogDescriptor& descriptor,
        std::wstring* selectedPath, DWORD* selectedFilterIndex);
    bool ShowGuestColorDialog(HWND owner, HINSTANCE instance,
        COLORREF initialColor, COLORREF* selectedColor,
        DLGPROC hookProcedure = nullptr, LPARAM hookParameter = 0);
    int ShowGuestShellAbout(HWND owner, LPCWSTR title, LPCWSTR text, HICON icon);
    int ShowGuestMessageBox(HWND owner, LPCWSTR text, LPCWSTR caption, UINT type);
    HRESULT ShowGuestTaskDialog(const GuestTaskDialogDescriptor& descriptor,
        int* selectedButton, int* selectedRadioButton,
        BOOL* verificationChecked);
    BOOL EndGuestResourceDialog(HWND dialog, INT_PTR result);
    BOOL HandleGuestDialogMessage(HWND dialog, const MSG* message);
    BOOL MapGuestDialogRect(HWND dialog, LPRECT rect);
    int LoadGuestStringResource(HINSTANCE instance, UINT identifier, LPWSTR buffer, int bufferCount);
}
}
