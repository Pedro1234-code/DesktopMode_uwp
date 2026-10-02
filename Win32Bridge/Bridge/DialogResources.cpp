#include "pch.h"
#include "Bridge/DialogResources.h"
#include "Bridge/GuestMetrics.h"

#include "Bridge/GuestResources.h"
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

    INT_PTR InvokeGuestDialogProcedure(
        DLGPROC procedure,
        HWND window,
        UINT message,
        WPARAM wParam,
        LPARAM lParam,
        DWORD* exceptionCode)
    {
        if (exceptionCode) *exceptionCode = ERROR_SUCCESS;
        if (!procedure) return FALSE;
        __try
        {
            return procedure(window, message, wParam, lParam);
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            if (exceptionCode) *exceptionCode = GetExceptionCode();
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
            const INT_PTR handled = InvokeGuestDialogProcedure(
                procedure, window, message, wParam, lParam, &exceptionCode);
            if (exceptionCode != ERROR_SUCCESS)
            {
                RuntimeDiagnostics::Record(L"DIALOG CALLBACK EXCEPTION: code " +
                    std::to_wstring(static_cast<unsigned long>(exceptionCode)) +
                    L", message " + std::to_wstring(message) + L".");
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
        LPARAM initParameter)
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
        const DWORD rootStyle = (dialog.style | DialogPopupStyle) & ~DialogVisibleStyle;
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

        ModalDialogState modal;
        modal.previous = g_activeModalDialog;
        modal.window = root;
        g_activeModalDialog = &modal;

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
        if (parent && manager->IsGuestWindow(parent)) manager->SetGuestFocus(parent, nullptr);
        g_activeModalDialog = modal.previous;
        BridgeSetLastError(ERROR_SUCCESS);
        return modal.result;
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

int Win32Bridge::Bridge::ShowGuestMessageBox(
    HWND owner,
    LPCWSTR text,
    LPCWSTR caption,
    UINT type)
{
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
