#pragma once

#include <windows.h>

#include <string>
#include <vector>
#include <memory>

namespace Win32Bridge
{
namespace Bridge
{
    void ResetGuestDialogState();
    HWND FindNextGuestDialogTabItem(HWND dialog, HWND current, BOOL previous);
    HWND FindNextGuestDialogGroupItem(HWND dialog, HWND current, BOOL previous);

    constexpr UINT GuestFileDialogNameId = 0x5204;
    constexpr UINT GuestFileDialogFilterId = 0x5205;
    constexpr UINT GuestFileDialogNameLabelId = 0x5206;
    constexpr UINT GuestFileDialogSetFilterMessage = WM_APP + 0x205;
    constexpr UINT GuestFileDialogSetFolderMessage = WM_APP + 0x206;

    struct GuestFileDialogCheckBox
    {
        DWORD id = 0;
        std::wstring label;
        BOOL checked = FALSE;
        DWORD state = 3; // CDCS_ENABLED | CDCS_VISIBLE
        HWND window = nullptr;
    };

    constexpr UINT MaximumGuestFileDialogFilters = 128;

    enum class GuestFileDialogEvent
    {
        Initialized, FolderChanging, FolderChanged, SelectionChanged,
        TypeChanged, FileOk, CheckButtonToggled, Destroyed
    };

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
        BOOL allowMultiple = FALSE;
        LPCWSTR okButtonLabel = nullptr;
        LPCWSTR fileNameLabel = nullptr;
        std::vector<GuestFileDialogCheckBox>* checkBoxes = nullptr;
        HRESULT (*onEvent)(void*, GuestFileDialogEvent, HWND,
            const std::wstring&, const std::vector<std::wstring>&, DWORD) = nullptr;
        void* eventContext = nullptr;
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
        const void* templateData = nullptr;
        HANDLE pageHandle = nullptr;
        DWORD pageFlags = 0;
        HANDLE activationContext = nullptr;
        std::shared_ptr<void> lifetime;
        bool (*prepareCreate)(void*) = nullptr;
        void (*release)(void*) = nullptr;
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
        int (CALLBACK* callback)(HWND, UINT, LPARAM) = nullptr;
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
    void ResetGuestPropertySheetsForCurrentProcess();
    bool ShowGuestFileDialog(const GuestFileDialogDescriptor& descriptor,
        std::wstring* selectedPath, DWORD* selectedFilterIndex,
        std::vector<std::wstring>* selectedPaths = nullptr);
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
