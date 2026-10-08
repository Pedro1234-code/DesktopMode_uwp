#include "pch.h"
#include "Bridge/DialogResources.h"
#include "Bridge/GuestMetrics.h"

#include "Bridge/GuestResources.h"
#include "Bridge/GuestStorage.h"
#include "Bridge/GuestWindow.h"
#include "Bridge/Gdi32Shims.h"
#include "Bridge/Kernel32Shims.h"
#include "Bridge/RuntimeDiagnostics.h"
#include "Bridge/User32Shims.h"
#include "Bridge/Win32Shims.h"

#include <algorithm>
#include <atomic>
#include <cstring>
#include <cwchar>
#include <cwctype>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

using namespace Win32Bridge::Bridge;

namespace
{
    constexpr WORD DialogResourceType = 5; // RT_DIALOG
    constexpr DWORD DialogSetFont = 0x00000040;
    constexpr DWORD DialogChildStyle = 0x40000000;
    constexpr DWORD DialogVisibleStyle = 0x10000000;
    constexpr DWORD DialogDisabledStyle = 0x08000000;
    constexpr DWORD DialogTabStopStyle = 0x00010000;
    constexpr DWORD DialogCenterStyle = 0x00000800;
    constexpr DWORD DialogNoFailCreateStyle = 0x00000010;
    constexpr DWORD DialogModalFrameStyle = 0x00000080;
    constexpr DWORD DialogPopupStyle = 0x80000000;
    constexpr DWORD DialogCaptionStyle = 0x00c00000;
    constexpr DWORD DialogSystemMenuStyle = 0x00080000;
    constexpr DWORD ButtonTypeMask = 0x0000000f;
    constexpr DWORD ButtonDefaultPush = 0x00000001;
    constexpr DWORD StaticLeft = 0x00000000;
    constexpr DWORD EditAutoHorizontalScroll = 0x00000080;
    constexpr DWORD EditReadOnly = 0x00000800;
    constexpr DWORD WindowBorderStyle = 0x00800000;
    constexpr DWORD ListViewReportStyle = 0x00000001;
    constexpr DWORD ListViewSingleSelectionStyle = 0x00000004;
    constexpr DWORD ListViewShowSelectionAlwaysStyle = 0x00000008;
    constexpr DWORD ComboBoxDropDownListStyle = 0x00000003;
    constexpr DWORD FileDialogOverwritePrompt = 0x00000002;
    constexpr DWORD FileDialogNoValidate = 0x00000100;
    constexpr DWORD FileDialogPathMustExist = 0x00000800;
    constexpr DWORD FileDialogFileMustExist = 0x00001000;
    constexpr UINT FileDialogPathId = 0x5201;
    constexpr UINT FileDialogUpId = 0x5202;
    constexpr UINT FileDialogListId = 0x5203;
    constexpr UINT FileDialogNameId = 0x5204;
    constexpr UINT FileDialogFilterId = 0x5205;
    constexpr UINT ShellAboutIconId = 0x5301;
    constexpr UINT StaticSetIconMessage = 0x0170;
    constexpr UINT ListViewDeleteAllItemsMessage = 0x1009;
    constexpr UINT ListViewInsertItemWMessage = 0x104d;
    constexpr UINT ListViewInsertColumnWMessage = 0x1061;
    constexpr UINT ComboBoxAddStringMessage = 0x0143;
    constexpr UINT ComboBoxGetCurrentSelectionMessage = 0x0147;
    constexpr UINT ComboBoxResetContentMessage = 0x014b;
    constexpr UINT ComboBoxSetCurrentSelectionMessage = 0x014e;
    constexpr UINT ListViewItemChangedNotification = static_cast<UINT>(-101);
    constexpr UINT ListViewItemActivateNotification = static_cast<UINT>(-114);
    constexpr int DialogMessageResultOffset = 0;
    constexpr int DialogProcedureOffset = sizeof(LONG_PTR);
    constexpr int DialogWindowExtra = sizeof(LONG_PTR) * 4;
    constexpr UINT DialogGetDefaultId = WM_USER;
    constexpr UINT DialogSetDefaultId = WM_USER + 1;
    constexpr LRESULT DialogDefaultIdMarker = 0x534b0000;

    struct DialogValue final
    {
        bool ordinal = false;
        WORD id = 0;
        std::wstring text;
    };

    class Reader final
    {
    public:
        Reader(const BYTE* begin, const BYTE* end) : m_current(begin), m_end(end) {}

        bool Word(WORD* value) { return Read(value); }
        bool Dword(DWORD* value) { return Read(value); }
        bool Short(short* value) { return Read(value); }

        bool AlignDword()
        {
            const uintptr_t address = reinterpret_cast<uintptr_t>(m_current);
            const uintptr_t aligned = (address + 3u) & ~uintptr_t(3u);
            if (aligned < address || aligned > reinterpret_cast<uintptr_t>(m_end)) return false;
            m_current = reinterpret_cast<const BYTE*>(aligned);
            return true;
        }

        bool Value(DialogValue* value)
        {
            if (!value) return false;
            *value = DialogValue{};
            WORD first = 0;
            if (!Word(&first)) return false;
            if (first == 0) return true;
            if (first == 0xffff)
            {
                value->ordinal = true;
                return Word(&value->id);
            }
            value->text.push_back(static_cast<wchar_t>(first));
            for (;;)
            {
                WORD character = 0;
                if (!Word(&character)) return false;
                if (character == 0) return true;
                value->text.push_back(static_cast<wchar_t>(character));
            }
        }

        bool Skip(size_t count)
        {
            if (count > static_cast<size_t>(m_end - m_current)) return false;
            m_current += count;
            return true;
        }

        bool Bytes(size_t count, std::vector<BYTE>* bytes)
        {
            if (!bytes || count > static_cast<size_t>(m_end - m_current)) return false;
            bytes->assign(m_current, m_current + count);
            m_current += count;
            return true;
        }

        const BYTE* Current() const { return m_current; }
        size_t Remaining() const { return static_cast<size_t>(m_end - m_current); }

    private:
        template <typename T>
        bool Read(T* value)
        {
            if (!value || sizeof(T) > static_cast<size_t>(m_end - m_current)) return false;
            std::memcpy(value, m_current, sizeof(T));
            m_current += sizeof(T);
            return true;
        }

        const BYTE* m_current;
        const BYTE* m_end;
    };

    struct DialogItem final
    {
        DWORD helpId = 0;
        DWORD style = 0;
        DWORD extendedStyle = 0;
        short x = 0;
        short y = 0;
        short width = 0;
        short height = 0;
        DWORD id = 0;
        DialogValue windowClass;
        DialogValue title;
        std::vector<BYTE> creationData;
    };

    struct DialogFont final
    {
        bool present = false;
        WORD pointSize = 0;
        WORD weight = FW_NORMAL;
        BYTE italic = FALSE;
        BYTE characterSet = DEFAULT_CHARSET;
        DialogValue face;
    };

    struct DialogTemplate final
    {
        bool extended = false;
        DWORD helpId = 0;
        DWORD style = 0;
        DWORD extendedStyle = 0;
        short x = 0;
        short y = 0;
        short width = 0;
        short height = 0;
        DialogValue menu;
        DialogValue windowClass;
        DialogValue title;
        DialogFont font;
        std::vector<DialogItem> items;
    };

    struct FileDialogListViewColumn final
    {
        UINT mask = 0;
        int format = 0;
        int width = 0;
        LPWSTR text = nullptr;
        int textCapacity = 0;
        int subItem = 0;
        int image = 0;
        int order = 0;
    };

    struct FileDialogListViewItem final
    {
        UINT mask = 0;
        int item = 0;
        int subItem = 0;
        UINT state = 0;
        UINT stateMask = 0;
        LPWSTR text = nullptr;
        int textCapacity = 0;
        int image = 0;
        LPARAM itemData = 0;
    };

    struct FileDialogNotifyHeader final
    {
        HWND from = nullptr;
        UINT_PTR identifier = 0;
        UINT code = 0;
    };

    struct FileDialogListViewNotification final
    {
        FileDialogNotifyHeader header;
        int item = -1;
        int subItem = 0;
        UINT newState = 0;
        UINT oldState = 0;
        UINT changed = 0;
        POINT action{};
        LPARAM itemData = 0;
        UINT keyFlags = 0;
    };
    static_assert(sizeof(FileDialogListViewNotification) == 72,
        "Virtual file picker notifications must remain x64-compatible.");

    struct FileDialogFilter final
    {
        std::wstring label;
        std::vector<std::wstring> patterns;
    };

    struct FileDialogEntry final
    {
        std::wstring name;
        bool directory = false;
    };

    struct FileDialogRuntime final
    {
        GuestFileDialogDescriptor descriptor;
        GuestStorageContext* storage = nullptr;
        HWND window = nullptr;
        std::wstring currentDirectory;
        std::wstring selectedPath;
        std::vector<FileDialogFilter> filters;
        std::vector<FileDialogEntry> entries;
        DWORD selectedFilter = 1;
        bool listColumnCreated = false;
    };

    struct ModalDialogState final
    {
        ModalDialogState* previous = nullptr;
        HWND window = nullptr;
        INT_PTR result = -1;
        bool ended = false;
    };

    struct DialogWindowState final
    {
        HWND window = nullptr;
        HWND owner = nullptr;
        std::vector<HWND> controls;
        std::vector<HWND> tabControls;
        UINT defaultButton = IDOK;
        HFONT font = nullptr;
        int baseUnitX = GuestMetrics::TextWidth;
        int baseUnitY = GuestMetrics::TextHeight;
    };

    constexpr UINT PropertyTabId = 0x3020;
    constexpr UINT PropertyApplyId = 0x3021;
    constexpr UINT PropertySheetSetCurrent = WM_USER + 101;
    constexpr UINT PropertySheetChanged = WM_USER + 104;
    constexpr UINT PropertySheetQuerySiblings = WM_USER + 108;
    constexpr UINT PropertySheetUnchanged = WM_USER + 109;
    constexpr UINT PropertySheetApply = WM_USER + 110;
    constexpr UINT PropertySheetPressButton = WM_USER + 113;
    constexpr UINT PropertySheetGetTabControl = WM_USER + 116;
    constexpr UINT PropertySheetGetCurrentPage = WM_USER + 118;
    constexpr UINT TabGetCurrentSelection = 0x130b;
    constexpr UINT TabSetCurrentSelection = 0x130c;
    constexpr UINT TabInsertItemW = 0x133e;
    constexpr UINT TabNotifySelectionChange = static_cast<UINT>(-551);
    constexpr int PropertyNotifySetActive = -200;
    constexpr int PropertyNotifyKillActive = -201;
    constexpr int PropertyNotifyApply = -202;
    constexpr int PropertyNotifyReset = -203;
    constexpr int PropertyNotifyQueryCancel = -209;

    struct PropertyPageRuntime final
    {
        HINSTANCE instance = nullptr;
        LPCWSTR templateName = nullptr;
        DLGPROC procedure = nullptr;
        LPARAM initParameter = 0;
        std::wstring title;
        std::vector<std::pair<std::wstring, std::wstring>> fields;
        HWND window = nullptr;
    };

    struct PropertySheetRuntime final
    {
        HWND window = nullptr;
        HWND parent = nullptr;
        HWND tabControl = nullptr;
        std::vector<PropertyPageRuntime> pages;
        UINT selected = 0;
        bool dirty = false;
    };

    struct PropertySheetNotification final
    {
        struct Header final
        {
            HWND hwndFrom = nullptr;
            UINT_PTR idFrom = 0;
            UINT code = 0;
        } header;
        LPARAM parameter = 0;
    };

    struct PropertyTabItemW final
    {
        UINT mask = 0;
        DWORD state = 0;
        DWORD stateMask = 0;
        LPWSTR text = nullptr;
        int textCapacity = 0;
        int image = -1;
        LPARAM parameter = 0;
    };

    thread_local ModalDialogState* g_activeModalDialog = nullptr;
    std::atomic<unsigned long> g_nextDialogClass{ 1 };
    std::mutex g_dialogWindowsLock;
    std::unordered_map<ULONG_PTR, std::shared_ptr<DialogWindowState>> g_dialogWindows;

    bool FindTypedResource(HINSTANCE instance, WORD resourceType, const DialogValue& requested, const BYTE** data, size_t* size)
    {
        if (!data || !size) return false;
        *data = nullptr;
        *size = 0;
        const LPCWSTR type = reinterpret_cast<LPCWSTR>(static_cast<ULONG_PTR>(resourceType));
        const LPCWSTR name = requested.ordinal
            ? reinterpret_cast<LPCWSTR>(static_cast<ULONG_PTR>(requested.id))
            : requested.text.c_str();
        GuestResourceData resource;
        if (FindGuestResource(
            reinterpret_cast<HMODULE>(instance), type, name, 0, false, &resource) !=
            GuestResourceStatus::Success)
        {
            return false;
        }
        *data = resource.data;
        *size = resource.size;
        return true;
    }

    bool ReadDialogFont(Reader* reader, bool extended, DialogFont* font)
    {
        if (!reader || !font) return false;
        *font = DialogFont{};
        font->present = true;
        if (!reader->Word(&font->pointSize)) return false;
        if (extended)
        {
            WORD italicAndCharset = 0;
            if (!reader->Word(&font->weight) || !reader->Word(&italicAndCharset)) return false;
            font->italic = static_cast<BYTE>(italicAndCharset & 0xff);
            font->characterSet = static_cast<BYTE>((italicAndCharset >> 8) & 0xff);
        }
        return reader->Value(&font->face);
    }

    bool ParseDialog(const BYTE* data, size_t size, DialogTemplate* dialog)
    {
        if (!data || !dialog || size < 18) return false;
        *dialog = DialogTemplate{};
        Reader reader(data, data + size);
        WORD first = 0;
        WORD second = 0;
        if (!reader.Word(&first) || !reader.Word(&second)) return false;
        const bool extended = first == 1 && second == 0xffff;
        dialog->extended = extended;
        WORD itemCount = 0;
        if (extended)
        {
            if (!reader.Dword(&dialog->helpId) || !reader.Dword(&dialog->extendedStyle) || !reader.Dword(&dialog->style) ||
                !reader.Word(&itemCount) || !reader.Short(&dialog->x) || !reader.Short(&dialog->y) ||
                !reader.Short(&dialog->width) || !reader.Short(&dialog->height)) return false;
        }
        else
        {
            dialog->style = static_cast<DWORD>(first) | (static_cast<DWORD>(second) << 16);
            if (!reader.Dword(&dialog->extendedStyle) || !reader.Word(&itemCount) || !reader.Short(&dialog->x) ||
                !reader.Short(&dialog->y) || !reader.Short(&dialog->width) || !reader.Short(&dialog->height)) return false;
        }
        if (!reader.Value(&dialog->menu) || !reader.Value(&dialog->windowClass) ||
            !reader.Value(&dialog->title)) return false;
        if ((dialog->style & DialogSetFont) && !ReadDialogFont(&reader, extended, &dialog->font)) return false;

        dialog->items.reserve(itemCount);
        for (WORD index = 0; index < itemCount; ++index)
        {
            if (!reader.AlignDword()) return false;
            DialogItem item;
            if (extended)
            {
                if (!reader.Dword(&item.helpId) || !reader.Dword(&item.extendedStyle) || !reader.Dword(&item.style) ||
                    !reader.Short(&item.x) || !reader.Short(&item.y) || !reader.Short(&item.width) || !reader.Short(&item.height) ||
                    !reader.Dword(&item.id)) return false;
            }
            else
            {
                WORD id = 0;
                if (!reader.Dword(&item.style) || !reader.Dword(&item.extendedStyle) || !reader.Short(&item.x) ||
                    !reader.Short(&item.y) || !reader.Short(&item.width) || !reader.Short(&item.height) || !reader.Word(&id)) return false;
                item.id = id;
            }
            WORD creationDataSize = 0;
            if (!reader.Value(&item.windowClass) || !reader.Value(&item.title) ||
                !reader.Word(&creationDataSize)) return false;
            // The size stored in a dialog template includes the size WORD
            // itself. The old parser skipped the full value after consuming
            // that WORD and consequently started every following item two
            // bytes late whenever creation data was present.
            if (creationDataSize != 0)
            {
                if (creationDataSize < sizeof(WORD) ||
                    !reader.Bytes(creationDataSize - sizeof(WORD), &item.creationData)) return false;
            }
            dialog->items.push_back(std::move(item));
        }
        return true;
    }

    LPCWSTR ClassNameFor(const DialogValue& value)
    {
        if (!value.ordinal) return value.text.empty() ? L"static" : value.text.c_str();
        switch (value.id)
        {
        case 0x0080: return L"button";
        case 0x0081: return L"edit";
        case 0x0082: return L"static";
        case 0x0083: return L"listbox";
        case 0x0084: return L"scrollbar";
        case 0x0085: return L"combobox";
        default: return L"static";
        }
    }

    struct DialogBaseUnits final
    {
        int x = GuestMetrics::TextWidth;
        int y = GuestMetrics::TextHeight;
    };

    int ScaleDialogUnit(int value, int numerator, int denominator)
    {
        const long long scaled = static_cast<long long>(value) * numerator;
        const long long half = denominator / 2;
        return static_cast<int>(scaled >= 0
            ? (scaled + half) / denominator
            : (scaled - half) / denominator);
    }

    DialogBaseUnits BaseUnitsFor(const DialogTemplate& dialog)
    {
        if (!dialog.font.present || dialog.font.pointSize == 0) return {};

        // Windows derives dialog units from the selected template font. The
        // MiniGDI backend is deterministic, so derive the same pair from the
        // requested point height instead of consulting a host XAML font.
        DialogBaseUnits units;
        units.y = (std::max)(GuestMetrics::TextWidth,
            ScaleDialogUnit(static_cast<int>(dialog.font.pointSize),
                GuestMetrics::LogicalDpi, 72) + GuestMetrics::DialogExternalLeading);
        units.x = (std::max)(GuestMetrics::ControlHorizontalPadding,
            (units.y * 7 + GuestMetrics::TextWidth) / GuestMetrics::TextHeight);
        return units;
    }

    int PixelsX(short units, const DialogBaseUnits& base)
    {
        return ScaleDialogUnit(static_cast<int>(units), base.x, 4);
    }

    int PixelsY(short units, const DialogBaseUnits& base)
    {
        return ScaleDialogUnit(static_cast<int>(units), base.y, 8);
    }

    LPCWSTR ResourcePointer(const DialogValue& value)
    {
        return value.ordinal
            ? reinterpret_cast<LPCWSTR>(static_cast<ULONG_PTR>(value.id))
            : (value.text.empty() ? nullptr : value.text.c_str());
    }

    std::shared_ptr<DialogWindowState> FindDialogState(HWND window)
    {
        std::lock_guard<std::mutex> guard(g_dialogWindowsLock);
        const auto found = g_dialogWindows.find(reinterpret_cast<ULONG_PTR>(window));
        return found == g_dialogWindows.end() ? nullptr : found->second;
    }

    bool IsDialogDescendant(
        GuestWindowManager* manager,
        HWND dialog,
        HWND candidate)
    {
        if (!manager || !dialog || !candidate) return false;
        HWND current = candidate;
        for (size_t depth = 0; depth < 128 && current; ++depth)
        {
            if (current == dialog) return true;
            DWORD error = ERROR_SUCCESS;
            current = manager->GetGuestParent(current, &error);
            if (error != ERROR_SUCCESS) return false;
        }
        return false;
    }

    bool FocusNextDialogControl(
        const std::shared_ptr<DialogWindowState>& state,
        GuestWindowManager* manager,
        HWND current,
        bool previous)
    {
        if (!state || !manager || state->tabControls.empty()) return false;
        const auto found = std::find(
            state->tabControls.begin(), state->tabControls.end(), current);
        size_t index = found == state->tabControls.end()
            ? (previous ? state->tabControls.size() - 1 : 0)
            : static_cast<size_t>(found - state->tabControls.begin());
        if (found != state->tabControls.end())
        {
            index = previous
                ? (index + state->tabControls.size() - 1) % state->tabControls.size()
                : (index + 1) % state->tabControls.size();
        }
        for (size_t attempts = 0; attempts < state->tabControls.size(); ++attempts)
        {
            const HWND candidate = state->tabControls[index];
            if (manager->IsGuestWindow(candidate))
            {
                manager->SetGuestFocus(candidate, nullptr);
                return true;
            }
            index = previous
                ? (index + state->tabControls.size() - 1) % state->tabControls.size()
                : (index + 1) % state->tabControls.size();
        }
        return false;
    }

    int CaptureDialogException(
        EXCEPTION_POINTERS* information,
        DWORD* exceptionCode,
        ULONG_PTR* exceptionAddress)
    {
        if (exceptionCode)
            *exceptionCode = information && information->ExceptionRecord
                ? information->ExceptionRecord->ExceptionCode
                : ERROR_GEN_FAILURE;
        if (exceptionAddress)
            *exceptionAddress = information && information->ExceptionRecord
                ? reinterpret_cast<ULONG_PTR>(information->ExceptionRecord->ExceptionAddress)
                : 0;
        return EXCEPTION_EXECUTE_HANDLER;
    }

    INT_PTR InvokeGuestDialogProcedure(
        DLGPROC procedure,
        HWND window,
        UINT message,
        WPARAM wParam,
        LPARAM lParam,
        DWORD* exceptionCode,
        ULONG_PTR* exceptionAddress)
    {
        if (exceptionCode) *exceptionCode = ERROR_SUCCESS;
        if (exceptionAddress) *exceptionAddress = 0;
        if (!procedure) return FALSE;
        __try
        {
            return procedure(window, message, wParam, lParam);
        }
        __except (CaptureDialogException(
            GetExceptionInformation(), exceptionCode, exceptionAddress))
        {
            return FALSE;
        }
    }

    bool DialogProcedureResultIsDirect(UINT message)
    {
        return message == WM_INITDIALOG || message == WM_COMPAREITEM ||
            message == WM_VKEYTOITEM || message == WM_CHARTOITEM ||
            message == WM_QUERYDRAGICON ||
            (message >= WM_CTLCOLORMSGBOX && message <= WM_CTLCOLORSTATIC);
    }

    LRESULT CALLBACK DialogWindowProcedure(HWND window, UINT message, WPARAM wParam, LPARAM lParam)
    {
        if (message == WM_NCCREATE) return TRUE;
        GuestWindowManager* manager = CurrentGuestWindowManager();
        if (!manager) return 0;

        DWORD error = ERROR_SUCCESS;
        const auto procedure = reinterpret_cast<DLGPROC>(manager->GetGuestWindowLongPtr(
            window, DialogProcedureOffset, &error));
        if (procedure && error == ERROR_SUCCESS)
        {
            manager->SetGuestWindowLongPtr(window, DialogMessageResultOffset, 0, &error);
            DWORD exceptionCode = ERROR_SUCCESS;
            ULONG_PTR exceptionAddress = 0;
            const INT_PTR handled = InvokeGuestDialogProcedure(
                procedure, window, message, wParam, lParam,
                &exceptionCode, &exceptionAddress);
            if (exceptionCode != ERROR_SUCCESS)
            {
                RuntimeDiagnostics::Record(L"DIALOG CALLBACK EXCEPTION: code " +
                    std::to_wstring(static_cast<unsigned long>(exceptionCode)) +
                    L", message " + std::to_wstring(message) +
                    L", address " + std::to_wstring(exceptionAddress) + L".");
            }
            else if (handled != FALSE)
            {
                if (DialogProcedureResultIsDirect(message)) return handled;
                return manager->GetGuestWindowLongPtr(
                    window, DialogMessageResultOffset, &error);
            }
        }

        const auto state = FindDialogState(window);
        switch (message)
        {
        case WM_CLOSE:
            if (EndGuestResourceDialog(window, IDCANCEL)) return 0;
            break;
        case WM_NEXTDLGCTL:
            if (state && !state->tabControls.empty())
            {
                if (lParam)
                {
                    manager->SetGuestFocus(reinterpret_cast<HWND>(wParam), nullptr);
                }
                else
                {
                    DWORD ignored = ERROR_SUCCESS;
                    FocusNextDialogControl(
                        state, manager, manager->GetGuestFocus(&ignored), wParam != FALSE);
                }
                return 0;
            }
            break;
        case DialogGetDefaultId:
            return state ? DialogDefaultIdMarker | (state->defaultButton & 0xffffu) : 0;
        case DialogSetDefaultId:
            if (state)
            {
                state->defaultButton = static_cast<UINT>(wParam & 0xffffu);
                return TRUE;
            }
            break;
        default:
            break;
        }

        const LRESULT result = manager->DefaultGuestWindowProcedure(window, message, wParam, lParam);
        if (message == WM_NCDESTROY)
        {
            std::shared_ptr<DialogWindowState> removed;
            {
                std::lock_guard<std::mutex> guard(g_dialogWindowsLock);
                const auto found = g_dialogWindows.find(reinterpret_cast<ULONG_PTR>(window));
                if (found != g_dialogWindows.end())
                {
                    removed = found->second;
                    g_dialogWindows.erase(found);
                }
            }
            if (removed && removed->font) BridgeDeleteObject(removed->font);
        }
        return result;
    }

    bool IsDialogControlMessage(
        const std::shared_ptr<DialogWindowState>& state,
        const GuestAbi::Message& message)
    {
        if (!state || message.message != WM_KEYDOWN) return false;
        GuestWindowManager* manager = CurrentGuestWindowManager();
        if (!manager) return false;

        if (message.wParam == VK_TAB && !state->tabControls.empty())
        {
            return FocusNextDialogControl(
                state, manager, message.hwnd, BridgeGetKeyState(VK_SHIFT) < 0);
        }
        if (message.wParam == VK_ESCAPE)
        {
            manager->SendGuestMessage(state->window, WM_COMMAND,
                MAKEWPARAM(IDCANCEL, BN_CLICKED), 0, nullptr);
            return true;
        }
        if (message.wParam == VK_RETURN)
        {
            manager->SendGuestMessage(state->window, WM_COMMAND,
                MAKEWPARAM(state->defaultButton, BN_CLICKED), 0, nullptr);
            return true;
        }
        return false;
    }

    INT_PTR RunGuestDialog(
        HINSTANCE instance,
        const DialogTemplate& dialog,
        HWND parent,
        DLGPROC procedure,
        LPARAM initParameter,
        bool runModal = true,
        HWND* createdWindow = nullptr)
    {
        GuestWindowManager* manager = CurrentGuestWindowManager();
        if (!manager)
        {
            BridgeSetLastError(ERROR_INVALID_FUNCTION);
            return -1;
        }

        const std::wstring className = L"Win32Bridge.Dialog." +
            std::to_wstring(g_nextDialogClass.fetch_add(1));
        GuestAbi::WndClassW windowClass{};
        windowClass.lpfnWndProc = &DialogWindowProcedure;
        windowClass.cbWndExtra = DialogWindowExtra;
        windowClass.hInstance = instance;
        windowClass.hbrBackground = reinterpret_cast<HBRUSH>(
            static_cast<ULONG_PTR>(COLOR_BTNFACE + 1));
        windowClass.lpszClassName = className.c_str();
        DWORD error = ERROR_SUCCESS;
        if (manager->RegisterGuestClass(&windowClass, &error) == 0)
        {
            BridgeSetLastError(error);
            return -1;
        }

        const DialogBaseUnits baseUnits = BaseUnitsFor(dialog);
        const int clientWidth = (std::max)(1, PixelsX(dialog.width, baseUnits));
        const int clientHeight = (std::max)(1, PixelsY(dialog.height, baseUnits));
        const DWORD rootStyle = (runModal ? dialog.style | DialogPopupStyle : dialog.style) &
            ~DialogVisibleStyle;
        const GuestMetrics::NonClientMetrics nonClient =
            GuestMetrics::NonClientForEmbeddedWindow(
                rootStyle, dialog.extendedStyle, parent != nullptr);
        // Dialog-template dimensions describe the client area. CreateWindowEx
        // dimensions describe the complete window, including its frame.
        const int dialogWidth = clientWidth + nonClient.left + nonClient.right;
        const int dialogHeight = clientHeight + nonClient.top + nonClient.bottom;
        int dialogX = PixelsX(dialog.x, baseUnits);
        int dialogY = PixelsY(dialog.y, baseUnits);
        if ((dialog.style & DialogCenterStyle) != 0 && parent)
        {
            RECT parentClient{};
            if (manager->GetGuestClientRect(parent, &parentClient, nullptr))
            {
                dialogX = (std::max)(0,
                    (static_cast<int>(parentClient.right - parentClient.left) - dialogWidth) / 2);
                dialogY = (std::max)(0,
                    (static_cast<int>(parentClient.bottom - parentClient.top) - dialogHeight) / 2);
            }
        }

        const std::wstring title = dialog.title.ordinal ? L"" : dialog.title.text;
        HWND root = manager->CreateGuestWindow(
            dialog.extendedStyle,
            className.c_str(),
            title.c_str(),
            rootStyle,
            dialogX,
            dialogY,
            dialogWidth,
            dialogHeight,
            parent,
            nullptr,
            instance,
            nullptr,
            &error);
        if (!root)
        {
            BridgeSetLastError(error);
            return -1;
        }

        auto runtime = std::make_shared<DialogWindowState>();
        runtime->window = root;
        runtime->owner = parent;
        runtime->baseUnitX = baseUnits.x;
        runtime->baseUnitY = baseUnits.y;
        if (dialog.font.present)
        {
            const int fontHeight = -ScaleDialogUnit(
                (std::max)(1, static_cast<int>(dialog.font.pointSize)), 96, 72);
            runtime->font = BridgeCreateFontW(
                fontHeight, 0, 0, 0, dialog.font.weight,
                dialog.font.italic, FALSE, FALSE, dialog.font.characterSet,
                OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, DEFAULT_QUALITY,
                DEFAULT_PITCH | FF_DONTCARE,
                dialog.font.face.ordinal ? nullptr : dialog.font.face.text.c_str());
        }
        {
            std::lock_guard<std::mutex> guard(g_dialogWindowsLock);
            g_dialogWindows.emplace(reinterpret_cast<ULONG_PTR>(root), runtime);
        }

        manager->SetGuestWindowLongPtr(root, DialogProcedureOffset,
            reinterpret_cast<LONG_PTR>(procedure), &error);
        if (error != ERROR_SUCCESS)
        {
            manager->DestroyGuestWindow(root, nullptr);
            BridgeSetLastError(error);
            return -1;
        }

        if (runtime->font)
        {
            manager->SendGuestMessage(root, WM_SETFONT,
                reinterpret_cast<WPARAM>(runtime->font), FALSE, nullptr);
        }
        if (ResourcePointer(dialog.menu))
        {
            const HMENU menu = BridgeLoadMenuW(instance, ResourcePointer(dialog.menu));
            if (menu) BridgeSetMenu(root, menu);
        }

        HWND initialFocus = nullptr;
        for (const auto& item : dialog.items)
        {
            const std::wstring itemTitle = item.title.ordinal ? L"" : item.title.text;
            const DWORD itemStyle = item.style | DialogChildStyle;
            HWND child = manager->CreateGuestWindow(
                item.extendedStyle,
                ClassNameFor(item.windowClass),
                itemTitle.c_str(),
                itemStyle,
                PixelsX(item.x, baseUnits),
                PixelsY(item.y, baseUnits),
                (std::max)(1, PixelsX(item.width, baseUnits)),
                (std::max)(1, PixelsY(item.height, baseUnits)),
                root,
                reinterpret_cast<HMENU>(static_cast<ULONG_PTR>(item.id)),
                instance,
                item.creationData.empty() ? nullptr : const_cast<BYTE*>(item.creationData.data()),
                &error);
            if (!child)
            {
                RuntimeDiagnostics::Record(L"DIALOG: failed to create control " +
                    std::to_wstring(item.id) + L" (error " + std::to_wstring(error) + L").");
                if ((dialog.style & DialogNoFailCreateStyle) != 0) continue;
                manager->DestroyGuestWindow(root, nullptr);
                BridgeSetLastError(error == ERROR_SUCCESS ? ERROR_CANNOT_MAKE : error);
                return -1;
            }
            runtime->controls.push_back(child);
            if (runtime->font)
            {
                manager->SendGuestMessage(child, WM_SETFONT,
                    reinterpret_cast<WPARAM>(runtime->font), FALSE, nullptr);
            }
            if (!initialFocus && (itemStyle & DialogVisibleStyle) != 0 &&
                (itemStyle & DialogDisabledStyle) == 0 &&
                (itemStyle & DialogTabStopStyle) != 0)
            {
                initialFocus = child;
            }
            if ((itemStyle & DialogVisibleStyle) != 0 &&
                (itemStyle & DialogDisabledStyle) == 0 &&
                (itemStyle & DialogTabStopStyle) != 0)
            {
                runtime->tabControls.push_back(child);
            }
            if (_wcsicmp(ClassNameFor(item.windowClass), L"button") == 0 &&
                (itemStyle & ButtonTypeMask) == ButtonDefaultPush)
            {
                runtime->defaultButton = item.id;
            }
        }

        if (!runModal)
        {
            const LRESULT initializeFocus = manager->SendGuestMessage(root, WM_INITDIALOG,
                reinterpret_cast<WPARAM>(initialFocus), initParameter, &error);
            if (manager->IsGuestWindow(root))
            {
                manager->UpdateGuestWindow(root, nullptr);
                for (const HWND control : runtime->controls)
                {
                    if (manager->IsGuestWindow(control)) manager->UpdateGuestWindow(control, nullptr);
                }
                manager->ShowGuestWindow(root, SW_SHOW, &error);
                if (initializeFocus != FALSE && initialFocus)
                    manager->SetGuestFocus(initialFocus, nullptr);
            }
            if (createdWindow) *createdWindow = root;
            RuntimeDiagnostics::Record(L"DIALOG: instantiated modeless child with " +
                std::to_wstring(dialog.items.size()) + L" control(s).");
            BridgeSetLastError(ERROR_SUCCESS);
            return TRUE;
        }

        ModalDialogState modal;
        modal.previous = g_activeModalDialog;
        modal.window = root;
        g_activeModalDialog = &modal;

        // DialogBox disables an enabled owner for the lifetime of the modal
        // loop and restores exactly that state afterwards. Nested modal
        // dialogs therefore disable their immediate owner without
        // accidentally re-enabling an owner disabled by an outer loop.
        const bool ownerWasEnabled = parent && manager->IsGuestWindow(parent) &&
            manager->IsGuestWindowEnabled(parent, nullptr) != FALSE;
        if (ownerWasEnabled) manager->EnableGuestWindow(parent, FALSE, nullptr);

        const LRESULT initializeFocus = manager->SendGuestMessage(root, WM_INITDIALOG,
            reinterpret_cast<WPARAM>(initialFocus), initParameter, &error);
        if (!modal.ended && manager->IsGuestWindow(root))
        {
            // Resource dialogs are assembled hidden. Paint their complete
            // visible subtree while it is still off-screen, then expose the
            // already-populated surfaces in one composed frame. The queued
            // WM_PAINT notifications become harmless stale notifications once
            // UpdateWindow consumes each invalid region.
            manager->UpdateGuestWindow(root, nullptr);
            for (const HWND control : runtime->controls)
            {
                if (manager->IsGuestWindow(control))
                {
                    manager->UpdateGuestWindow(control, nullptr);
                }
            }
            manager->ShowGuestWindow(root, SW_SHOW, &error);
            // SetFocus rejects effectively hidden descendants, as Win32 does.
            // Apply the dialog manager's requested initial control only after
            // the dialog itself has become visible.
            if (initializeFocus != FALSE && initialFocus)
                manager->SetGuestFocus(initialFocus, nullptr);
        }

        RuntimeDiagnostics::Record(L"DIALOG: instantiated " +
            std::to_wstring(dialog.items.size()) + L" control(s) from a native template.");
        while (!modal.ended && manager->IsGuestWindow(root))
        {
            GuestAbi::Message message{};
            const GuestGetMessageResult read =
                manager->GetGuestMessage(&message, nullptr, 0, 0);
            if (read == GuestGetMessageResult::Quit)
            {
                manager->PostGuestQuitMessage(static_cast<int>(message.wParam));
                modal.result = -1;
                break;
            }
            if (read != GuestGetMessageResult::Message)
            {
                modal.result = -1;
                break;
            }
            const bool belongsToDialog =
                IsDialogDescendant(manager, root, message.hwnd);
            const bool inputMessage =
                (message.message >= WM_KEYFIRST && message.message <= WM_KEYLAST) ||
                (message.message >= WM_MOUSEFIRST && message.message <= WM_MOUSELAST);
            if (!belongsToDialog && inputMessage) continue;
            if (IsDialogControlMessage(runtime, message)) continue;
            manager->TranslateGuestMessage(&message);
            manager->DispatchGuestMessage(&message, &error);
        }

        if (manager->IsGuestWindow(root)) manager->DestroyGuestWindow(root, &error);
        if (ownerWasEnabled && parent && manager->IsGuestWindow(parent))
            manager->EnableGuestWindow(parent, TRUE, nullptr);
        if (parent && manager->IsGuestWindow(parent) &&
            manager->IsGuestWindowEnabled(parent, nullptr))
            manager->SetGuestFocus(parent, nullptr);
        g_activeModalDialog = modal.previous;
        BridgeSetLastError(ERROR_SUCCESS);
        return modal.result;
    }

    bool ParseDialogResource(
        HINSTANCE instance,
        LPCWSTR templateName,
        DialogTemplate* dialog)
    {
        if (!templateName || !dialog) return false;
        DialogValue requested;
        if (reinterpret_cast<ULONG_PTR>(templateName) <= 0xffff)
        {
            requested.ordinal = true;
            requested.id = static_cast<WORD>(reinterpret_cast<ULONG_PTR>(templateName));
        }
        else
        {
            requested.text = templateName;
        }
        const BYTE* bytes = nullptr;
        size_t byteCount = 0;
        return FindTypedResource(instance, DialogResourceType, requested, &bytes, &byteCount) &&
            ParseDialog(bytes, byteCount, dialog);
    }

    INT_PTR CALLBACK SyntheticPropertyPageProcedure(HWND, UINT message, WPARAM, LPARAM)
    {
        return message == WM_INITDIALOG ? TRUE : FALSE;
    }

    bool BuildPropertyFieldsDialog(
        const PropertyPageRuntime& page,
        DialogTemplate* dialog)
    {
        if (!dialog || page.fields.empty()) return false;

        size_t longestName = 0;
        size_t longestValue = 0;
        for (const auto& field : page.fields)
        {
            longestName = (std::max)(longestName, field.first.size());
            longestValue = (std::max)(longestValue, field.second.size());
        }

        // The template is expressed in dialog units. Its dimensions follow
        // the supplied data, with generic usability bounds rather than any
        // application-specific resource geometry.
        const int labelWidth = (std::max)(44, (std::min)(112,
            static_cast<int>(longestName * 4 + 8)));
        const int valueWidth = (std::max)(132, (std::min)(344,
            static_cast<int>(longestValue * 4 + 12)));
        const int rowHeight = 18;

        *dialog = DialogTemplate{};
        dialog->style = DialogChildStyle | DialogVisibleStyle | DialogSetFont;
        dialog->width = static_cast<short>(labelWidth + valueWidth + 24);
        dialog->height = static_cast<short>((std::min)(32767,
            12 + static_cast<int>(page.fields.size()) * rowHeight));
        dialog->font.present = true;
        dialog->font.pointSize = 9;
        dialog->font.weight = FW_NORMAL;
        dialog->font.face.text = L"Segoe UI";

        for (size_t index = 0; index < page.fields.size(); ++index)
        {
            const short y = static_cast<short>(7 + index * rowHeight);

            DialogItem label;
            label.style = DialogChildStyle | DialogVisibleStyle | StaticLeft;
            label.x = 8;
            label.y = y + 2;
            label.width = static_cast<short>(labelWidth);
            label.height = 12;
            label.id = static_cast<DWORD>(0x4100 + index * 2);
            label.windowClass.ordinal = true;
            label.windowClass.id = 0x0082; // Static.
            label.title.text = page.fields[index].first;
            dialog->items.push_back(std::move(label));

            DialogItem value;
            value.style = DialogChildStyle | DialogVisibleStyle |
                DialogTabStopStyle | WindowBorderStyle |
                EditAutoHorizontalScroll | EditReadOnly;
            value.x = static_cast<short>(12 + labelWidth);
            value.y = y;
            value.width = static_cast<short>(valueWidth);
            value.height = 14;
            value.id = static_cast<DWORD>(0x4101 + index * 2);
            value.windowClass.ordinal = true;
            value.windowClass.id = 0x0081; // Edit.
            value.title.text = page.fields[index].second;
            dialog->items.push_back(std::move(value));
        }
        return true;
    }

    HWND CreatePropertyPageWindow(PropertySheetRuntime* sheet, UINT index)
    {
        if (!sheet || index >= sheet->pages.size()) return nullptr;
        PropertyPageRuntime& page = sheet->pages[index];
        if (page.window) return page.window;

        DialogTemplate dialog;
        if (!page.fields.empty())
        {
            if (!BuildPropertyFieldsDialog(page, &dialog)) return nullptr;
        }
        else if (!ParseDialogResource(page.instance, page.templateName, &dialog))
        {
            return nullptr;
        }
        dialog.style |= DialogChildStyle;
        dialog.style &= ~(DialogPopupStyle | DialogCaptionStyle |
            DialogSystemMenuStyle | DialogCenterStyle);
        dialog.x = 0;
        dialog.y = 0;
        dialog.title = DialogValue{};

        HWND window = nullptr;
        if (RunGuestDialog(
                page.instance,
                dialog,
                sheet->window,
                page.procedure ? page.procedure : &SyntheticPropertyPageProcedure,
                page.initParameter,
                false,
                &window) == -1 || !window)
        {
            return nullptr;
        }
        page.window = window;

        GuestWindowManager* manager = CurrentGuestWindowManager();
        RECT pageRect{};
        if (manager && manager->GetGuestWindowRect(window, &pageRect, nullptr))
        {
            manager->SetGuestWindowPos(
                window, nullptr, 12, 42,
                (std::max)(1L, pageRect.right - pageRect.left),
                (std::max)(1L, pageRect.bottom - pageRect.top),
                SWP_NOZORDER | SWP_NOACTIVATE,
                nullptr);
        }
        return window;
    }

    LRESULT NotifyPropertyPage(
        PropertySheetRuntime* sheet,
        UINT index,
        int code,
        LPARAM parameter = 0)
    {
        if (!sheet || index >= sheet->pages.size()) return 0;
        GuestWindowManager* manager = CurrentGuestWindowManager();
        const HWND page = sheet->pages[index].window;
        if (!manager || !page || !manager->IsGuestWindow(page)) return 0;

        PropertySheetNotification notification;
        notification.header.hwndFrom = sheet->window;
        notification.header.idFrom = 0;
        notification.header.code = static_cast<UINT>(code);
        notification.parameter = parameter;
        return manager->SendGuestMessage(
            page, WM_NOTIFY, 0,
            reinterpret_cast<LPARAM>(&notification), nullptr);
    }

    void SetPropertySheetDirty(PropertySheetRuntime* sheet, bool dirty)
    {
        if (!sheet) return;
        sheet->dirty = dirty;
        GuestWindowManager* manager = CurrentGuestWindowManager();
        if (!manager) return;
        const HWND apply = manager->GetGuestDlgItem(
            sheet->window, PropertyApplyId, nullptr);
        if (apply) manager->EnableGuestWindow(apply, dirty ? TRUE : FALSE, nullptr);
    }

    bool SelectPropertyPage(PropertySheetRuntime* sheet, UINT index, bool initial)
    {
        if (!sheet || index >= sheet->pages.size()) return false;
        GuestWindowManager* manager = CurrentGuestWindowManager();
        if (!manager) return false;

        if (!initial && sheet->selected < sheet->pages.size())
        {
            if (NotifyPropertyPage(sheet, sheet->selected, PropertyNotifyKillActive) != 0)
            {
                if (sheet->tabControl)
                    manager->SendGuestMessage(
                        sheet->tabControl, TabSetCurrentSelection,
                        sheet->selected, 0, nullptr);
                return false;
            }
            const HWND previous = sheet->pages[sheet->selected].window;
            if (previous) manager->ShowGuestWindow(previous, SW_HIDE, nullptr);
        }

        const HWND page = CreatePropertyPageWindow(sheet, index);
        if (!page) return false;
        sheet->selected = index;
        if (sheet->tabControl)
            manager->SendGuestMessage(
                sheet->tabControl, TabSetCurrentSelection, index, 0, nullptr);
        manager->ShowGuestWindow(page, SW_SHOW, nullptr);
        NotifyPropertyPage(sheet, index, PropertyNotifySetActive);
        return true;
    }

    bool ApplyPropertyPages(PropertySheetRuntime* sheet, bool closeAfterApply)
    {
        if (!sheet || sheet->selected >= sheet->pages.size()) return false;
        if (NotifyPropertyPage(sheet, sheet->selected, PropertyNotifyKillActive) != 0)
            return false;
        for (UINT index = 0; index < sheet->pages.size(); ++index)
        {
            if (sheet->pages[index].window)
            {
                const LRESULT result =
                    NotifyPropertyPage(sheet, index, PropertyNotifyApply);
                if (result != 0)
                {
                    if (result == 1) // PSNRET_INVALID
                        SelectPropertyPage(sheet, index, index == sheet->selected);
                    return false;
                }
            }
        }
        SetPropertySheetDirty(sheet, false);
        if (closeAfterApply) EndGuestResourceDialog(sheet->window, IDOK);
        return true;
    }

    void CancelPropertySheet(PropertySheetRuntime* sheet)
    {
        if (!sheet || sheet->selected >= sheet->pages.size()) return;
        if (NotifyPropertyPage(sheet, sheet->selected, PropertyNotifyQueryCancel) != 0)
            return;
        for (UINT index = 0; index < sheet->pages.size(); ++index)
        {
            if (sheet->pages[index].window)
                NotifyPropertyPage(sheet, index, PropertyNotifyReset, FALSE);
        }
        EndGuestResourceDialog(sheet->window, IDCANCEL);
    }

    INT_PTR CALLBACK PropertySheetDialogProcedure(
        HWND dialog,
        UINT message,
        WPARAM wParam,
        LPARAM lParam)
    {
        PropertySheetRuntime* sheet = reinterpret_cast<PropertySheetRuntime*>(
            BridgeGetWindowLongPtrW(dialog, GWLP_USERDATA));
        if (message == WM_INITDIALOG)
        {
            sheet = reinterpret_cast<PropertySheetRuntime*>(lParam);
            if (!sheet) return FALSE;
            sheet->window = dialog;
            BridgeSetWindowLongPtrW(dialog, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(sheet));
            GuestWindowManager* manager = CurrentGuestWindowManager();
            sheet->tabControl = manager
                ? manager->GetGuestDlgItem(dialog, PropertyTabId, nullptr)
                : nullptr;
            if (manager && sheet->tabControl)
            {
                for (UINT index = 0; index < sheet->pages.size(); ++index)
                {
                    PropertyTabItemW item;
                    item.mask = 0x00000001u; // TCIF_TEXT
                    item.text = const_cast<LPWSTR>(sheet->pages[index].title.c_str());
                    manager->SendGuestMessage(
                        sheet->tabControl, TabInsertItemW, index,
                        reinterpret_cast<LPARAM>(&item), nullptr);
                }
            }
            SetPropertySheetDirty(sheet, false);
            return SelectPropertyPage(sheet, sheet->selected, true) ? TRUE : FALSE;
        }
        if (!sheet) return FALSE;

        if (message == WM_COMMAND)
        {
            const UINT command = LOWORD(wParam);
            if (command == IDOK)
            {
                ApplyPropertyPages(sheet, true);
                return TRUE;
            }
            if (command == IDCANCEL)
            {
                CancelPropertySheet(sheet);
                return TRUE;
            }
            if (command == PropertyApplyId)
            {
                ApplyPropertyPages(sheet, false);
                return TRUE;
            }
        }
        if (message == WM_CLOSE)
        {
            CancelPropertySheet(sheet);
            return TRUE;
        }
        if (message == WM_NOTIFY && lParam)
        {
            const auto header = reinterpret_cast<const PropertySheetNotification::Header*>(lParam);
            if (header->hwndFrom == sheet->tabControl &&
                header->code == TabNotifySelectionChange)
            {
                GuestWindowManager* manager = CurrentGuestWindowManager();
                const LRESULT selected = manager
                    ? manager->SendGuestMessage(
                        sheet->tabControl, TabGetCurrentSelection, 0, 0, nullptr)
                    : -1;
                if (selected >= 0) SelectPropertyPage(sheet, static_cast<UINT>(selected), false);
                return TRUE;
            }
        }
        if (message == PropertySheetChanged)
        {
            SetPropertySheetDirty(sheet, true);
            return TRUE;
        }
        if (message == PropertySheetUnchanged)
        {
            SetPropertySheetDirty(sheet, false);
            return TRUE;
        }
        if (message == PropertySheetSetCurrent)
        {
            return SelectPropertyPage(sheet, static_cast<UINT>(wParam), false) ? TRUE : FALSE;
        }
        if (message == PropertySheetApply)
        {
            return ApplyPropertyPages(sheet, false) ? TRUE : FALSE;
        }
        if (message == PropertySheetGetCurrentPage)
        {
            return reinterpret_cast<LRESULT>(sheet->pages[sheet->selected].window);
        }
        if (message == PropertySheetGetTabControl)
        {
            return reinterpret_cast<LRESULT>(sheet->tabControl);
        }
        if (message == PropertySheetQuerySiblings)
        {
            GuestWindowManager* manager = CurrentGuestWindowManager();
            if (!manager) return 0;
            for (const auto& page : sheet->pages)
            {
                if (!page.window) continue;
                const LRESULT result = manager->SendGuestMessage(
                    page.window, PropertySheetQuerySiblings, wParam, lParam, nullptr);
                if (result) return result;
            }
            return 0;
        }
        if (message == PropertySheetPressButton)
        {
            // PSBTN_OK=3, PSBTN_APPLYNOW=4, PSBTN_CANCEL=5.
            const UINT button = static_cast<UINT>(wParam);
            if (button == 3) ApplyPropertyPages(sheet, true);
            else if (button == 4) ApplyPropertyPages(sheet, false);
            else if (button == 5) CancelPropertySheet(sheet);
            return TRUE;
        }
        if (message == WM_NCDESTROY)
        {
            BridgeSetWindowLongPtrW(dialog, GWLP_USERDATA, 0);
        }
        return FALSE;
    }

    DialogItem PropertySheetButton(
        DWORD id,
        short x,
        short y,
        short width,
        const std::wstring& title,
        bool defaultButton = false)
    {
        DialogItem item;
        item.style = DialogVisibleStyle | DialogTabStopStyle |
            (defaultButton ? ButtonDefaultPush : 0);
        item.x = x;
        item.y = y;
        item.width = width;
        item.height = 14;
        item.id = id;
        item.windowClass.ordinal = true;
        item.windowClass.id = 0x0080;
        item.title.text = title;
        return item;
    }

    bool FileDialogWildcardMatch(const wchar_t* pattern, const wchar_t* text)
    {
        if (!pattern || !text) return false;
        const wchar_t* star = nullptr;
        const wchar_t* retry = nullptr;
        while (*text)
        {
            if (*pattern == L'?' || std::towlower(*pattern) == std::towlower(*text))
            {
                ++pattern;
                ++text;
                continue;
            }
            if (*pattern == L'*')
            {
                star = pattern++;
                retry = text;
                continue;
            }
            if (star)
            {
                pattern = star + 1;
                text = ++retry;
                continue;
            }
            return false;
        }
        while (*pattern == L'*') ++pattern;
        return *pattern == L'\0';
    }

    std::vector<std::wstring> SplitFileDialogPatterns(const std::wstring& value)
    {
        std::vector<std::wstring> patterns;
        size_t start = 0;
        while (start <= value.size())
        {
            const size_t separator = value.find(L';', start);
            const size_t end = separator == std::wstring::npos ? value.size() : separator;
            if (end > start) patterns.push_back(value.substr(start, end - start));
            if (separator == std::wstring::npos) break;
            start = separator + 1;
        }
        if (patterns.empty()) patterns.push_back(L"*.*");
        return patterns;
    }

    std::vector<FileDialogFilter> ParseFileDialogFilters(LPCWSTR filter)
    {
        std::vector<FileDialogFilter> result;
        const wchar_t* current = filter;
        for (size_t count = 0; current && *current && count < 128; ++count)
        {
            FileDialogFilter entry;
            entry.label = current;
            current += entry.label.size() + 1;
            if (!*current) break;
            const std::wstring patterns = current;
            current += patterns.size() + 1;
            entry.patterns = SplitFileDialogPatterns(patterns);
            result.push_back(std::move(entry));
        }
        if (result.empty())
        {
            FileDialogFilter entry;
            entry.label = L"All files (*.*)";
            entry.patterns.push_back(L"*.*");
            result.push_back(std::move(entry));
        }
        return result;
    }

    bool FileDialogNameMatches(const FileDialogRuntime& runtime, const std::wstring& name)
    {
        if (runtime.filters.empty()) return true;
        const size_t index = (std::min)(runtime.filters.size() - 1,
            runtime.selectedFilter > 0 ? static_cast<size_t>(runtime.selectedFilter - 1) : 0);
        for (const auto& pattern : runtime.filters[index].patterns)
        {
            if (pattern == L"*" || pattern == L"*.*" ||
                FileDialogWildcardMatch(pattern.c_str(), name.c_str())) return true;
        }
        return false;
    }

    std::wstring FileDialogJoinPath(const std::wstring& directory, const std::wstring& name)
    {
        if (directory.empty()) return name;
        return directory.back() == L'\\' ? directory + name : directory + L"\\" + name;
    }

    std::wstring FileDialogParentPath(const std::wstring& path)
    {
        if (path.size() <= 3) return L"C:\\";
        const size_t separator = path.find_last_of(L'\\');
        if (separator == std::wstring::npos || separator <= 2) return L"C:\\";
        return path.substr(0, separator);
    }

    bool FileDialogDirectoryExists(
        GuestStorageContext* storage,
        const std::wstring& requested,
        std::wstring* canonical)
    {
        if (!storage || requested.empty()) return false;
        DWORD error = ERROR_SUCCESS;
        std::wstring normalized;
        if (!storage->CanonicalPath(requested.c_str(), &normalized, &error)) return false;
        const DWORD attributes = storage->GetGuestFileAttributes(normalized.c_str(), &error);
        if (attributes == INVALID_FILE_ATTRIBUTES ||
            (attributes & FILE_ATTRIBUTE_DIRECTORY) == 0) return false;
        if (canonical) *canonical = std::move(normalized);
        return true;
    }

    void PopulateFileDialog(FileDialogRuntime* runtime)
    {
        if (!runtime || !runtime->window || !runtime->storage) return;
        HWND list = BridgeGetDlgItem(runtime->window, FileDialogListId);
        if (!list) return;

        BridgeSetDlgItemTextW(runtime->window, FileDialogPathId,
            runtime->currentDirectory.c_str());
        BridgeSendMessageW(list, ListViewDeleteAllItemsMessage, 0, 0);
        if (!runtime->listColumnCreated)
        {
            std::wstring heading = L"Name";
            FileDialogListViewColumn column{};
            column.mask = 0x0002 | 0x0004; // LVCF_WIDTH | LVCF_TEXT.
            column.width = 500;
            column.text = &heading[0];
            BridgeSendMessageW(list, ListViewInsertColumnWMessage, 0,
                reinterpret_cast<LPARAM>(&column));
            runtime->listColumnCreated = true;
        }

        runtime->entries.clear();
        WIN32_FIND_DATAW data{};
        HANDLE search = INVALID_HANDLE_VALUE;
        DWORD error = ERROR_SUCCESS;
        const std::wstring pattern = FileDialogJoinPath(runtime->currentDirectory, L"*");
        if (runtime->storage->FindFirstGuestFile(
            pattern.c_str(), &data, &search, &error))
        {
            do
            {
                const std::wstring name(data.cFileName);
                if (name == L"." || name == L"..") continue;
                const bool directory = (data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
                if (!directory && !FileDialogNameMatches(*runtime, name)) continue;
                FileDialogEntry entry;
                entry.name = name;
                entry.directory = directory;
                runtime->entries.push_back(std::move(entry));
            } while (runtime->storage->FindNextGuestFile(search, &data, &error));
            runtime->storage->CloseFindHandle(search, &error);
        }

        std::sort(runtime->entries.begin(), runtime->entries.end(),
            [](const FileDialogEntry& left, const FileDialogEntry& right)
        {
            if (left.directory != right.directory) return left.directory > right.directory;
            return _wcsicmp(left.name.c_str(), right.name.c_str()) < 0;
        });

        for (size_t index = 0; index < runtime->entries.size(); ++index)
        {
            std::wstring caption = runtime->entries[index].directory
                ? runtime->entries[index].name + L"\\"
                : runtime->entries[index].name;
            FileDialogListViewItem item{};
            item.mask = 0x0001 | 0x0004; // LVIF_TEXT | LVIF_PARAM.
            item.item = static_cast<int>(index);
            item.text = &caption[0];
            item.itemData = static_cast<LPARAM>(index);
            BridgeSendMessageW(list, ListViewInsertItemWMessage, 0,
                reinterpret_cast<LPARAM>(&item));
        }
    }

    bool NavigateFileDialog(FileDialogRuntime* runtime, const std::wstring& requested)
    {
        std::wstring canonical;
        if (!runtime || !FileDialogDirectoryExists(
            runtime->storage, requested, &canonical)) return false;
        runtime->currentDirectory = std::move(canonical);
        BridgeSetDlgItemTextW(runtime->window, FileDialogNameId, L"");
        PopulateFileDialog(runtime);
        return true;
    }

    void SelectFileDialogEntry(FileDialogRuntime* runtime, int index)
    {
        if (!runtime || index < 0 ||
            static_cast<size_t>(index) >= runtime->entries.size()) return;
        const auto& entry = runtime->entries[static_cast<size_t>(index)];
        if (!entry.directory)
            BridgeSetDlgItemTextW(runtime->window, FileDialogNameId, entry.name.c_str());
    }

    bool AcceptFileDialog(FileDialogRuntime* runtime)
    {
        if (!runtime || !runtime->storage) return false;
        std::vector<wchar_t> buffer(32768, L'\0');
        BridgeGetDlgItemTextW(runtime->window, FileDialogNameId,
            buffer.data(), static_cast<int>(buffer.size()));
        std::wstring name(buffer.data());
        while (!name.empty() && std::iswspace(name.front())) name.erase(name.begin());
        while (!name.empty() && std::iswspace(name.back())) name.pop_back();
        if (name.empty()) return false;

        if (runtime->descriptor.saveDialog && runtime->descriptor.defaultExtension &&
            *runtime->descriptor.defaultExtension)
        {
            const size_t slash = name.find_last_of(L"\\/");
            const size_t dot = name.find_last_of(L'.');
            if (dot == std::wstring::npos ||
                (slash != std::wstring::npos && dot < slash))
            {
                if (runtime->descriptor.defaultExtension[0] != L'.') name += L'.';
                name += runtime->descriptor.defaultExtension;
            }
        }

        const bool absolute = name.size() >= 3 && std::iswalpha(name[0]) &&
            name[1] == L':' && (name[2] == L'\\' || name[2] == L'/');
        const std::wstring requested = absolute
            ? name : FileDialogJoinPath(runtime->currentDirectory, name);
        DWORD error = ERROR_SUCCESS;
        std::wstring canonical;
        if (!runtime->storage->CanonicalPath(requested.c_str(), &canonical, &error))
        {
            ShowGuestMessageBox(runtime->window, L"The file name is not valid.",
                runtime->descriptor.title, MB_OK | MB_ICONERROR);
            return false;
        }

        const DWORD attributes = runtime->storage->GetGuestFileAttributes(
            canonical.c_str(), &error);
        if (attributes != INVALID_FILE_ATTRIBUTES &&
            (attributes & FILE_ATTRIBUTE_DIRECTORY) != 0)
        {
            return NavigateFileDialog(runtime, canonical);
        }
        if (!runtime->descriptor.saveDialog &&
            (runtime->descriptor.flags & FileDialogFileMustExist) != 0 &&
            attributes == INVALID_FILE_ATTRIBUTES)
        {
            ShowGuestMessageBox(runtime->window, L"The file does not exist.",
                runtime->descriptor.title, MB_OK | MB_ICONERROR);
            return false;
        }
        if ((runtime->descriptor.flags & FileDialogPathMustExist) != 0 &&
            (runtime->descriptor.flags & FileDialogNoValidate) == 0)
        {
            const std::wstring parent = FileDialogParentPath(canonical);
            if (!FileDialogDirectoryExists(runtime->storage, parent, nullptr))
            {
                ShowGuestMessageBox(runtime->window, L"The specified path does not exist.",
                    runtime->descriptor.title, MB_OK | MB_ICONERROR);
                return false;
            }
        }
        if (runtime->descriptor.saveDialog &&
            (runtime->descriptor.flags & FileDialogOverwritePrompt) != 0 &&
            attributes != INVALID_FILE_ATTRIBUTES)
        {
            const int answer = ShowGuestMessageBox(runtime->window,
                L"This file already exists. Do you want to replace it?",
                runtime->descriptor.title, MB_YESNO | MB_ICONWARNING);
            if (answer != IDYES) return false;
        }

        runtime->selectedPath = std::move(canonical);
        EndGuestResourceDialog(runtime->window, IDOK);
        return true;
    }

    INT_PTR CALLBACK FileDialogProcedure(HWND dialog, UINT message, WPARAM wParam, LPARAM lParam)
    {
        FileDialogRuntime* runtime = reinterpret_cast<FileDialogRuntime*>(
            BridgeGetWindowLongPtrW(dialog, GWLP_USERDATA));
        if (message == WM_INITDIALOG)
        {
            runtime = reinterpret_cast<FileDialogRuntime*>(lParam);
            if (!runtime) return FALSE;
            runtime->window = dialog;
            BridgeSetWindowLongPtrW(dialog, GWLP_USERDATA,
                reinterpret_cast<LONG_PTR>(runtime));
            BridgeSetDlgItemTextW(dialog, FileDialogNameId,
                runtime->descriptor.initialFileName
                    ? runtime->descriptor.initialFileName : L"");
            HWND filter = BridgeGetDlgItem(dialog, FileDialogFilterId);
            BridgeSendMessageW(filter, ComboBoxResetContentMessage, 0, 0);
            for (const auto& entry : runtime->filters)
                BridgeSendMessageW(filter, ComboBoxAddStringMessage, 0,
                    reinterpret_cast<LPARAM>(entry.label.c_str()));
            BridgeSendMessageW(filter, ComboBoxSetCurrentSelectionMessage,
                runtime->selectedFilter - 1, 0);
            PopulateFileDialog(runtime);
            return TRUE;
        }
        if (!runtime) return FALSE;

        if (message == WM_NOTIFY)
        {
            const auto* notification = reinterpret_cast<const FileDialogListViewNotification*>(lParam);
            if (!notification || notification->header.identifier != FileDialogListId) return FALSE;
            if (notification->header.code == ListViewItemChangedNotification)
            {
                if ((notification->newState & 0x0002) != 0)
                    SelectFileDialogEntry(runtime, notification->item);
                return TRUE;
            }
            if (notification->header.code == ListViewItemActivateNotification)
            {
                if (notification->item >= 0 &&
                    static_cast<size_t>(notification->item) < runtime->entries.size())
                {
                    const auto& entry = runtime->entries[static_cast<size_t>(notification->item)];
                    if (entry.directory)
                        NavigateFileDialog(runtime,
                            FileDialogJoinPath(runtime->currentDirectory, entry.name));
                    else
                    {
                        SelectFileDialogEntry(runtime, notification->item);
                        AcceptFileDialog(runtime);
                    }
                }
                return TRUE;
            }
        }
        if (message == WM_COMMAND)
        {
            const UINT command = LOWORD(wParam);
            const UINT notification = HIWORD(wParam);
            if (command == IDOK)
            {
                AcceptFileDialog(runtime);
                return TRUE;
            }
            if (command == IDCANCEL)
            {
                EndGuestResourceDialog(dialog, IDCANCEL);
                return TRUE;
            }
            if (command == FileDialogUpId)
            {
                NavigateFileDialog(runtime,
                    FileDialogParentPath(runtime->currentDirectory));
                return TRUE;
            }
            if (command == FileDialogFilterId && notification == 1) // CBN_SELCHANGE.
            {
                HWND filter = BridgeGetDlgItem(dialog, FileDialogFilterId);
                const LRESULT selection = BridgeSendMessageW(
                    filter, ComboBoxGetCurrentSelectionMessage, 0, 0);
                if (selection >= 0 &&
                    static_cast<size_t>(selection) < runtime->filters.size())
                {
                    runtime->selectedFilter = static_cast<DWORD>(selection + 1);
                    PopulateFileDialog(runtime);
                }
                return TRUE;
            }
        }
        if (message == WM_CLOSE)
        {
            EndGuestResourceDialog(dialog, IDCANCEL);
            return TRUE;
        }
        return FALSE;
    }

    DialogItem FileDialogItem(
        DWORD id, LPCWSTR windowClass, LPCWSTR text,
        short x, short y, short width, short height, DWORD style)
    {
        DialogItem item;
        item.id = id;
        item.windowClass.text = windowClass ? windowClass : L"static";
        item.title.text = text ? text : L"";
        item.x = x;
        item.y = y;
        item.width = width;
        item.height = height;
        item.style = style | DialogChildStyle | DialogVisibleStyle;
        return item;
    }

    INT_PTR CALLBACK MessageBoxDialogProcedure(HWND dialog, UINT message, WPARAM wParam, LPARAM)
    {
        if (message == WM_INITDIALOG) return TRUE;
        if (message == WM_COMMAND)
        {
            const UINT command = LOWORD(wParam);
            switch (command)
            {
            case IDOK:
            case IDCANCEL:
            case IDABORT:
            case IDRETRY:
            case IDIGNORE:
            case IDYES:
            case IDNO:
            case IDTRYAGAIN:
            case IDCONTINUE:
                EndGuestResourceDialog(dialog, command);
                return TRUE;
            default:
                break;
            }
        }
        if (message == WM_CLOSE)
        {
            EndGuestResourceDialog(dialog, IDCANCEL);
            return TRUE;
        }
        return FALSE;
    }

    struct ShellAboutRuntime final
    {
        HICON icon = nullptr;
    };

    INT_PTR CALLBACK ShellAboutDialogProcedure(
        HWND dialog, UINT message, WPARAM wParam, LPARAM lParam)
    {
        if (message == WM_INITDIALOG)
        {
            const auto* runtime = reinterpret_cast<const ShellAboutRuntime*>(lParam);
            if (runtime && runtime->icon)
            {
                BridgeSendDlgItemMessageW(dialog, ShellAboutIconId,
                    StaticSetIconMessage,
                    reinterpret_cast<WPARAM>(runtime->icon), 0);
            }
            return TRUE;
        }
        if (message == WM_COMMAND &&
            (LOWORD(wParam) == IDOK || LOWORD(wParam) == IDCANCEL))
        {
            EndGuestResourceDialog(dialog, LOWORD(wParam));
            return TRUE;
        }
        if (message == WM_CLOSE)
        {
            EndGuestResourceDialog(dialog, IDCANCEL);
            return TRUE;
        }
        return FALSE;
    }
}

INT_PTR Win32Bridge::Bridge::ShowGuestDialogFromResourceWithStyles(
    HINSTANCE instance, LPCWSTR templateName, HWND parent, DLGPROC procedure,
    LPARAM initParameter, DWORD stylesToAdd, DWORD stylesToRemove,
    LPCWSTR titleOverride)
{
    if (!CurrentGuestWindowManager() || !templateName)
    {
        BridgeSetLastError(ERROR_INVALID_PARAMETER);
        return -1;
    }
    DialogValue requested;
    if (reinterpret_cast<ULONG_PTR>(templateName) <= 0xffff)
    {
        requested.ordinal = true;
        requested.id = static_cast<WORD>(reinterpret_cast<ULONG_PTR>(templateName));
    }
    else
    {
        requested.text = templateName;
    }

    const BYTE* bytes = nullptr;
    size_t byteCount = 0;
    DialogTemplate dialog;
    if (!FindTypedResource(instance, DialogResourceType, requested, &bytes, &byteCount))
    {
        RuntimeDiagnostics::Record(L"DIALOG RESOURCE FAILED: RT_DIALOG was not found in module " +
            std::to_wstring(reinterpret_cast<ULONG_PTR>(instance)) + L".");
        BridgeSetLastError(ERROR_RESOURCE_DATA_NOT_FOUND);
        return -1;
    }
    if (!ParseDialog(bytes, byteCount, &dialog))
    {
        RuntimeDiagnostics::Record(L"DIALOG RESOURCE FAILED: malformed " +
            std::to_wstring(byteCount) + L"-byte template.");
        BridgeSetLastError(ERROR_RESOURCE_DATA_NOT_FOUND);
        return -1;
    }

    RuntimeDiagnostics::Record(L"DIALOG RESOURCE: parsed " +
        std::wstring(dialog.extended ? L"DLGTEMPLATEEX" : L"DLGTEMPLATE") +
        L" with " + std::to_wstring(dialog.items.size()) + L" control(s).");

    dialog.style = (dialog.style | stylesToAdd) & ~stylesToRemove;
    if (titleOverride) dialog.title.text = titleOverride;

    return RunGuestDialog(instance, dialog, parent, procedure, initParameter);
}

INT_PTR Win32Bridge::Bridge::ShowGuestDialogFromResource(
    HINSTANCE instance, LPCWSTR templateName, HWND parent, DLGPROC procedure,
    LPARAM initParameter)
{
    return ShowGuestDialogFromResourceWithStyles(instance, templateName, parent,
        procedure, initParameter, 0, 0, nullptr);
}

INT_PTR Win32Bridge::Bridge::ShowGuestDialogFromTemplate(
    HINSTANCE instance, const void* templateData, HWND parent, DLGPROC procedure,
    LPARAM initParameter, bool modal, HWND* createdWindow)
{
    if (!templateData || !procedure)
    {
        BridgeSetLastError(ERROR_INVALID_PARAMETER);
        return -1;
    }
    MEMORY_BASIC_INFORMATION memory{};
    if (::VirtualQuery(templateData, &memory, sizeof(memory)) != sizeof(memory) ||
        memory.State != MEM_COMMIT)
    {
        BridgeSetLastError(ERROR_INVALID_ADDRESS);
        return -1;
    }
    const auto begin = reinterpret_cast<ULONG_PTR>(templateData);
    const auto regionEnd = reinterpret_cast<ULONG_PTR>(memory.BaseAddress) + memory.RegionSize;
    if (begin >= regionEnd)
    {
        BridgeSetLastError(ERROR_INVALID_ADDRESS);
        return -1;
    }
    constexpr size_t MaximumDialogTemplateSize = 1024 * 1024;
    const size_t available = (std::min)(static_cast<size_t>(regionEnd - begin),
        MaximumDialogTemplateSize);
    DialogTemplate dialog;
    if (!ParseDialog(static_cast<const BYTE*>(templateData), available, &dialog))
    {
        BridgeSetLastError(ERROR_INVALID_DATA);
        return -1;
    }
    RuntimeDiagnostics::Record(L"DIALOG INDIRECT: parsed template with " +
        std::to_wstring(dialog.items.size()) + L" control(s).");
    return RunGuestDialog(instance, dialog, parent, procedure, initParameter,
        modal, createdWindow);
}

INT_PTR Win32Bridge::Bridge::ShowGuestPropertySheet(
    const GuestPropertySheetDescriptor& descriptor)
{
    if (!descriptor.pages || descriptor.pageCount == 0 || descriptor.pageCount > 256)
    {
        BridgeSetLastError(ERROR_INVALID_PARAMETER);
        return -1;
    }

    PropertySheetRuntime runtime;
    runtime.parent = descriptor.parent;
    runtime.selected = (std::min)(descriptor.startPage, descriptor.pageCount - 1);

    short maximumWidth = 1;
    short maximumHeight = 1;
    for (UINT index = 0; index < descriptor.pageCount; ++index)
    {
        const GuestPropertyPageDescriptor& source = descriptor.pages[index];
        const bool resourcePage = source.templateName && source.dialogProcedure;
        const bool fieldPage = source.fields && source.fieldCount != 0;
        if (!resourcePage && !fieldPage)
        {
            BridgeSetLastError(ERROR_INVALID_DATA);
            return -1;
        }

        DialogTemplate pageTemplate;
        PropertyPageRuntime page;
        page.instance = source.instance;
        page.templateName = source.templateName;
        page.procedure = source.dialogProcedure;
        page.initParameter = source.initParameter;
        if (fieldPage)
        {
            page.fields.reserve(source.fieldCount);
            for (UINT fieldIndex = 0; fieldIndex < source.fieldCount; ++fieldIndex)
            {
                page.fields.emplace_back(
                    source.fields[fieldIndex].name ? source.fields[fieldIndex].name : L"",
                    source.fields[fieldIndex].value ? source.fields[fieldIndex].value : L"");
            }
            if (!BuildPropertyFieldsDialog(page, &pageTemplate))
            {
                BridgeSetLastError(ERROR_INVALID_DATA);
                return -1;
            }
        }
        else if (!ParseDialogResource(source.instance, source.templateName, &pageTemplate))
        {
            BridgeSetLastError(ERROR_RESOURCE_DATA_NOT_FOUND);
            return -1;
        }

        if (source.title && *source.title)
            page.title = source.title;
        else if (!pageTemplate.title.ordinal && !pageTemplate.title.text.empty())
            page.title = pageTemplate.title.text;
        else
            page.title = L"Page " + std::to_wstring(index + 1);
        runtime.pages.push_back(std::move(page));
        maximumWidth = (std::max)(maximumWidth, pageTemplate.width);
        maximumHeight = (std::max)(maximumHeight, pageTemplate.height);
    }

    DialogTemplate host;
    host.style = DialogPopupStyle | DialogCaptionStyle | DialogSystemMenuStyle |
        DialogModalFrameStyle | DialogCenterStyle | DialogSetFont;
    const int footerWidth = (descriptor.flags & 0x00000080u) == 0
        ? 6 + 3 * 46 + 2 * 4 + 6
        : 6 + 2 * 46 + 4 + 6;
    host.width = static_cast<short>((std::min)(32767,
        (std::max)(footerWidth, static_cast<int>(maximumWidth) + 12)));
    host.height = static_cast<short>((std::min)(32767,
        static_cast<int>(maximumHeight) + 48));
    host.title.text = descriptor.caption && *descriptor.caption
        ? descriptor.caption
        : L"Properties";
    host.font.present = true;
    host.font.pointSize = 9;
    host.font.weight = FW_NORMAL;
    host.font.face.text = L"Segoe UI";

    DialogItem tab;
    tab.style = DialogChildStyle | DialogVisibleStyle | DialogTabStopStyle;
    tab.x = 4;
    tab.y = 4;
    tab.width = static_cast<short>(host.width - 8);
    tab.height = static_cast<short>(host.height - 32);
    tab.id = PropertyTabId;
    tab.windowClass.text = L"SysTabControl32";
    host.items.push_back(std::move(tab));

    const short buttonY = static_cast<short>(host.height - 22);
    const short buttonWidth = 46;
    short buttonX = static_cast<short>(host.width - 6 - buttonWidth);
    host.items.push_back(PropertySheetButton(IDCANCEL, buttonX, buttonY, buttonWidth, L"Cancel"));
    buttonX = static_cast<short>(buttonX - buttonWidth - 4);
    if ((descriptor.flags & 0x00000080u) == 0) // PSH_NOAPPLYNOW
    {
        host.items.push_back(PropertySheetButton(
            PropertyApplyId, buttonX, buttonY, buttonWidth, L"Apply"));
        buttonX = static_cast<short>(buttonX - buttonWidth - 4);
    }
    host.items.push_back(PropertySheetButton(IDOK, buttonX, buttonY, buttonWidth, L"OK", true));

    RuntimeDiagnostics::Record(
        L"PROPERTYSHEET HOST: creating " + std::to_wstring(descriptor.pageCount) +
        L" page(s), starting at " + std::to_wstring(runtime.selected) + L".");
    return RunGuestDialog(
        descriptor.instance,
        host,
        descriptor.parent,
        &PropertySheetDialogProcedure,
        reinterpret_cast<LPARAM>(&runtime));
}

bool Win32Bridge::Bridge::ShowGuestFileDialog(
    const GuestFileDialogDescriptor& descriptor,
    std::wstring* selectedPath,
    DWORD* selectedFilterIndex)
{
    if (!selectedPath)
    {
        BridgeSetLastError(ERROR_INVALID_PARAMETER);
        return false;
    }
    selectedPath->clear();

    FileDialogRuntime runtime;
    runtime.descriptor = descriptor;
    runtime.storage = CurrentGuestStorageContext();
    if (!runtime.storage)
    {
        BridgeSetLastError(ERROR_INVALID_HANDLE);
        return false;
    }
    runtime.filters = ParseFileDialogFilters(descriptor.filter);
    runtime.selectedFilter = descriptor.filterIndex == 0
        ? 1
        : (std::min)(descriptor.filterIndex,
            static_cast<DWORD>(runtime.filters.size()));

    std::wstring initial;
    if (descriptor.initialDirectory)
        FileDialogDirectoryExists(runtime.storage,
            descriptor.initialDirectory, &initial);
    if (initial.empty())
        FileDialogDirectoryExists(runtime.storage,
            runtime.storage->CurrentDirectory(), &initial);
    if (initial.empty())
        FileDialogDirectoryExists(runtime.storage,
            L"C:\\Users\\Default\\Documents", &initial);
    if (initial.empty()) initial = L"C:\\";
    runtime.currentDirectory = std::move(initial);

    DialogTemplate dialog;
    dialog.style = DialogPopupStyle | DialogCaptionStyle | DialogSystemMenuStyle |
        DialogModalFrameStyle | DialogCenterStyle | DialogSetFont;
    dialog.width = 330;
    dialog.height = 210;
    dialog.title.text = descriptor.title && *descriptor.title
        ? descriptor.title
        : (descriptor.saveDialog ? L"Save As" : L"Open");
    dialog.font.present = true;
    dialog.font.pointSize = 9;
    dialog.font.weight = FW_NORMAL;
    dialog.font.face.text = L"Segoe UI";

    const DWORD labelStyle = StaticLeft;
    const DWORD editStyle = DialogTabStopStyle | WindowBorderStyle |
        EditAutoHorizontalScroll;
    const DWORD buttonStyle = DialogTabStopStyle;
    dialog.items.push_back(FileDialogItem(
        static_cast<DWORD>(-1), L"static", L"Look in:",
        6, 8, 42, 10, labelStyle));
    dialog.items.push_back(FileDialogItem(
        FileDialogPathId, L"edit", L"",
        48, 5, 230, 14, editStyle | EditReadOnly));
    dialog.items.push_back(FileDialogItem(
        FileDialogUpId, L"button", L"Up",
        282, 5, 40, 14, buttonStyle));
    dialog.items.push_back(FileDialogItem(
        FileDialogListId, L"SysListView32", L"",
        6, 24, 316, 116,
        DialogTabStopStyle | WindowBorderStyle | ListViewReportStyle |
        ListViewSingleSelectionStyle | ListViewShowSelectionAlwaysStyle));
    dialog.items.push_back(FileDialogItem(
        static_cast<DWORD>(-1), L"static", L"File name:",
        6, 149, 48, 10, labelStyle));
    dialog.items.push_back(FileDialogItem(
        FileDialogNameId, L"edit", L"",
        55, 146, 147, 14, editStyle));
    dialog.items.push_back(FileDialogItem(
        static_cast<DWORD>(-1), L"static", L"Files of type:",
        6, 169, 48, 10, labelStyle));
    dialog.items.push_back(FileDialogItem(
        FileDialogFilterId, L"combobox", L"",
        55, 166, 147, 38,
        DialogTabStopStyle | WindowBorderStyle | ComboBoxDropDownListStyle));
    dialog.items.push_back(FileDialogItem(
        IDOK, L"button", descriptor.saveDialog ? L"Save" : L"Open",
        218, 166, 50, 14, buttonStyle | ButtonDefaultPush));
    dialog.items.push_back(FileDialogItem(
        IDCANCEL, L"button", L"Cancel",
        272, 166, 50, 14, buttonStyle));

    RuntimeDiagnostics::Record(std::wstring(L"COMMON DIALOG: opening virtual ") +
        (descriptor.saveDialog ? L"save" : L"open") +
        L" picker in '" + runtime.currentDirectory + L"'.");
    const HINSTANCE instance = descriptor.instance
        ? descriptor.instance
        : reinterpret_cast<HINSTANCE>(const_cast<BYTE*>(CurrentGuestImageBase()));
    const INT_PTR result = RunGuestDialog(
        instance,
        dialog,
        descriptor.owner,
        &FileDialogProcedure,
        reinterpret_cast<LPARAM>(&runtime));
    if (selectedFilterIndex) *selectedFilterIndex = runtime.selectedFilter;
    if (result != IDOK || runtime.selectedPath.empty()) return false;

    *selectedPath = std::move(runtime.selectedPath);
    RuntimeDiagnostics::Record(
        L"COMMON DIALOG: selected virtual path '" + *selectedPath + L"'.");
    BridgeSetLastError(ERROR_SUCCESS);
    return true;
}

int Win32Bridge::Bridge::ShowGuestShellAbout(
    HWND owner,
    LPCWSTR title,
    LPCWSTR text,
    HICON icon)
{
    const std::wstring applicationName = title && *title ? title : L"About";
    RuntimeDiagnostics::Record(L"SHELL ABOUT: creating virtual About dialog for '" +
        applicationName + L"'.");

    DialogTemplate dialog;
    dialog.style = DialogPopupStyle | DialogCaptionStyle | DialogSystemMenuStyle |
        DialogModalFrameStyle | DialogCenterStyle | DialogSetFont;
    dialog.width = 230;
    dialog.height = text && *text ? 116 : 100;
    dialog.title.text = applicationName;
    dialog.font.present = true;
    dialog.font.pointSize = 9;
    dialog.font.weight = FW_NORMAL;
    dialog.font.face.text = L"Segoe UI";

    DialogItem iconItem;
    iconItem.style = DialogChildStyle | DialogVisibleStyle | 0x00000003u; // SS_ICON.
    iconItem.x = 12;
    iconItem.y = 13;
    iconItem.width = 32;
    iconItem.height = 32;
    iconItem.id = ShellAboutIconId;
    iconItem.windowClass.ordinal = true;
    iconItem.windowClass.id = 0x0082;
    dialog.items.push_back(std::move(iconItem));

    dialog.items.push_back(FileDialogItem(static_cast<DWORD>(-1), L"static",
        applicationName.c_str(), 54, 13, 164, 14, StaticLeft));
    dialog.items.push_back(FileDialogItem(static_cast<DWORD>(-1), L"static",
        L"Microsoft Windows", 54, 30, 164, 12, StaticLeft));
    if (text && *text)
    {
        dialog.items.push_back(FileDialogItem(static_cast<DWORD>(-1), L"static",
            text, 12, 54, 206, 26, StaticLeft));
    }

    const short buttonY = static_cast<short>(dialog.height - 23);
    dialog.items.push_back(PropertySheetButton(
        IDOK, static_cast<short>((dialog.width - 50) / 2), buttonY, 50, L"OK", true));

    ShellAboutRuntime runtime;
    runtime.icon = icon ? icon : BridgeLoadIconW(nullptr, MAKEINTRESOURCEW(32512));
    HINSTANCE instance = reinterpret_cast<HINSTANCE>(
        const_cast<BYTE*>(CurrentGuestImageBase()));
    const INT_PTR result = RunGuestDialog(instance, dialog, owner,
        &ShellAboutDialogProcedure, reinterpret_cast<LPARAM>(&runtime));
    return result < 0 ? FALSE : TRUE;
}

int Win32Bridge::Bridge::ShowGuestMessageBox(
    HWND owner,
    LPCWSTR text,
    LPCWSTR caption,
    UINT type)
{
    RuntimeDiagnostics::Record(
        L"MESSAGE BOX: [" + std::wstring(caption ? caption : L"") + L"] " +
        std::wstring(text ? text : L""));
    DialogTemplate dialog;
    dialog.style = DialogPopupStyle | DialogCaptionStyle | DialogSystemMenuStyle |
        DialogModalFrameStyle | DialogCenterStyle | DialogSetFont;
    dialog.width = 220;
    dialog.height = 92;
    dialog.title.text = caption ? caption : L"Win32 application";
    dialog.font.present = true;
    dialog.font.pointSize = 9;
    dialog.font.weight = FW_NORMAL;
    dialog.font.face.text = L"Segoe UI";

    DialogItem message;
    message.style = DialogChildStyle | DialogVisibleStyle | StaticLeft;
    message.x = 10;
    message.y = 10;
    message.width = 200;
    message.height = 46;
    message.id = static_cast<DWORD>(-1);
    message.windowClass.ordinal = true;
    message.windowClass.id = 0x0082;
    message.title.text = text ? text : L"";
    dialog.items.push_back(std::move(message));

    std::vector<std::pair<UINT, const wchar_t*>> buttons;
    switch (type & MB_TYPEMASK)
    {
    case MB_OKCANCEL:
        buttons = { { IDOK, L"OK" }, { IDCANCEL, L"Cancel" } };
        break;
    case MB_ABORTRETRYIGNORE:
        buttons = { { IDABORT, L"Abort" }, { IDRETRY, L"Retry" }, { IDIGNORE, L"Ignore" } };
        break;
    case MB_YESNOCANCEL:
        buttons = { { IDYES, L"Yes" }, { IDNO, L"No" }, { IDCANCEL, L"Cancel" } };
        break;
    case MB_YESNO:
        buttons = { { IDYES, L"Yes" }, { IDNO, L"No" } };
        break;
    case MB_RETRYCANCEL:
        buttons = { { IDRETRY, L"Retry" }, { IDCANCEL, L"Cancel" } };
        break;
    case MB_CANCELTRYCONTINUE:
        buttons = { { IDCANCEL, L"Cancel" }, { IDTRYAGAIN, L"Try Again" }, { IDCONTINUE, L"Continue" } };
        break;
    case MB_OK:
    default:
        buttons = { { IDOK, L"OK" } };
        break;
    }

    const size_t requestedDefault = (type & MB_DEFMASK) >> 8;
    const size_t defaultIndex = (std::min)(requestedDefault, buttons.size() - 1);
    constexpr short buttonWidth = 50;
    constexpr short buttonGap = 4;
    const short totalWidth = static_cast<short>(
        buttons.size() * buttonWidth + (buttons.size() - 1) * buttonGap);
    short x = static_cast<short>((dialog.width - totalWidth) / 2);
    for (size_t index = 0; index < buttons.size(); ++index)
    {
        DialogItem button;
        button.style = DialogChildStyle | DialogVisibleStyle | DialogTabStopStyle |
            (index == defaultIndex ? ButtonDefaultPush : 0);
        button.x = x;
        button.y = 66;
        button.width = buttonWidth;
        button.height = 14;
        button.id = buttons[index].first;
        button.windowClass.ordinal = true;
        button.windowClass.id = 0x0080;
        button.title.text = buttons[index].second;
        dialog.items.push_back(std::move(button));
        x = static_cast<short>(x + buttonWidth + buttonGap);
    }

    HINSTANCE instance = reinterpret_cast<HINSTANCE>(
        const_cast<BYTE*>(CurrentGuestImageBase()));
    const INT_PTR result = RunGuestDialog(
        instance, dialog, owner, &MessageBoxDialogProcedure, 0);
    return result < 0 ? 0 : static_cast<int>(result);
}

BOOL Win32Bridge::Bridge::HandleGuestDialogMessage(HWND dialog, const MSG* message)
{
    if (!dialog || !message) return FALSE;
    const auto state = FindDialogState(dialog);
    if (!state) return FALSE;
    GuestWindowManager* manager = CurrentGuestWindowManager();
    if (!IsDialogDescendant(manager, dialog, message->hwnd)) return FALSE;
    GuestAbi::Message translated{};
    translated.hwnd = message->hwnd;
    translated.message = message->message;
    translated.wParam = message->wParam;
    translated.lParam = message->lParam;
    return IsDialogControlMessage(state, translated) ? TRUE : FALSE;
}

BOOL Win32Bridge::Bridge::MapGuestDialogRect(HWND dialog, LPRECT rect)
{
    if (!dialog || !rect) return FALSE;
    const auto state = FindDialogState(dialog);
    if (!state) return FALSE;

    rect->left = ScaleDialogUnit(rect->left, state->baseUnitX, 4);
    rect->right = ScaleDialogUnit(rect->right, state->baseUnitX, 4);
    rect->top = ScaleDialogUnit(rect->top, state->baseUnitY, 8);
    rect->bottom = ScaleDialogUnit(rect->bottom, state->baseUnitY, 8);
    return TRUE;
}

BOOL Win32Bridge::Bridge::EndGuestResourceDialog(HWND dialog, INT_PTR result)
{
    for (ModalDialogState* state = g_activeModalDialog; state; state = state->previous)
    {
        if (state->window != dialog) continue;
        state->result = result;
        state->ended = true;
        GuestWindowManager* manager = CurrentGuestWindowManager();
        DWORD error = ERROR_SUCCESS;
        if (!manager || !manager->DestroyGuestWindow(dialog, &error))
        {
            BridgeSetLastError(error);
            return FALSE;
        }
        BridgeSetLastError(ERROR_SUCCESS);
        return TRUE;
    }
    BridgeSetLastError(ERROR_INVALID_WINDOW_HANDLE);
    return FALSE;
}

int Win32Bridge::Bridge::LoadGuestStringResource(HINSTANCE instance, UINT identifier, LPWSTR buffer, int bufferCount)
{
    if (buffer && bufferCount > 0) buffer[0] = L'\0';
    DialogValue group;
    group.ordinal = true;
    group.id = static_cast<WORD>((identifier >> 4) + 1);
    const BYTE* bytes = nullptr;
    size_t byteCount = 0;
    if (!FindTypedResource(instance, 6, group, &bytes, &byteCount)) return 0; // RT_STRING

    Reader reader(bytes, bytes + byteCount);
    for (UINT index = 0; index < 16; ++index)
    {
        WORD length = 0;
        if (!reader.Word(&length) || length > byteCount / sizeof(wchar_t)) return 0;
        if (index != (identifier & 0x0f))
        {
            if (!reader.Skip(static_cast<size_t>(length) * sizeof(wchar_t))) return 0;
            continue;
        }
        std::vector<wchar_t> text(static_cast<size_t>(length) + 1, L'\0');
        for (WORD character = 0; character < length; ++character)
        {
            WORD value = 0;
            if (!reader.Word(&value)) return 0;
            text[character] = static_cast<wchar_t>(value);
        }
        if (!buffer || bufferCount <= 0) return length;
        const int copied = (std::min)(static_cast<int>(length), bufferCount - 1);
        if (copied > 0) std::memcpy(buffer, text.data(), static_cast<size_t>(copied) * sizeof(wchar_t));
        buffer[copied] = L'\0';
        return copied;
    }
    return 0;
}
