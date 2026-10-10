#include "pch.h"
#include "Bridge\\CommonControlsShims.h"
#include "Bridge\\ActivationContext.h"
#include "Bridge\\DialogResources.h"
#include "Bridge\\GuestWindow.h"
#include "Bridge\\Kernel32Shims.h"
#include "Bridge\\MouseInput.h"
#include "Bridge\\User32Shims.h"
#include "Bridge/GuestMetrics.h"
#include "Bridge/RuntimeDiagnostics.h"

#include <wrl.h>
#include <windows.system.threading.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <cwctype>
#include <functional>
#include <limits>
#include <new>
#include <numeric>
#include <unordered_set>
#include <vector>

using namespace Win32Bridge::Bridge;

using namespace Microsoft::WRL;
using namespace Platform;
using namespace Windows::Devices::Input;
using namespace Windows::Foundation;
using namespace Windows::Storage::Streams;
using namespace Windows::System;
using namespace Windows::System::Threading;
using namespace Windows::UI::Core;
using namespace Windows::UI::Input;
using namespace Windows::UI::Xaml;
using namespace Windows::UI::Xaml::Controls;
using namespace Windows::UI::Xaml::Media;
using namespace Windows::UI::Xaml::Media::Imaging;

namespace
{
    // WriteableBitmap exposes its storage through this documented internal
    // interface. The surface data never leaves the UWP app container.
    // IBufferByteAccess is the SDK COM interface behind WriteableBitmap's
    // PixelBuffer. Keep this UUID byte-for-byte aligned with robuffer.h:
    // querying a different interface can appear to succeed but returns an
    // unrelated vtable and, consequently, an invalid pixel address.
    struct __declspec(uuid("905a0fef-bc53-11df-8c49-001e4fc686da")) IBufferByteAccess : IUnknown
    {
        virtual HRESULT STDMETHODCALLTYPE Buffer(unsigned char** value) = 0;
    };

    HRESULT CopyPixelsToBuffer(
        IBufferByteAccess* access,
        const void* source,
        size_t byteCount,
        DWORD* exceptionCode)
    {
        if (exceptionCode) *exceptionCode = ERROR_SUCCESS;
        if (!access || (!source && byteCount != 0)) return E_POINTER;
        __try
        {
            unsigned char* destination = nullptr;
            const HRESULT result = access->Buffer(&destination);
            if (FAILED(result)) return result;
            if (!destination || reinterpret_cast<ULONG_PTR>(destination) < 0x10000)
                return E_POINTER;
            if (byteCount != 0) memcpy(destination, source, byteCount);
            return S_OK;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            if (exceptionCode) *exceptionCode = GetExceptionCode();
            return E_FAIL;
        }
    }

    thread_local GuestWindowManager* g_currentGuestWindowManager = nullptr;
    thread_local unsigned g_ownerDataDisplayInfoDiagnostics = 0;
    thread_local unsigned g_listViewPopulationDiagnostics = 0;

    using SehInvocation = void(*)(void*);

    // The function containing __try must stay entirely native. C++/CX
    // tracking handles (^) need unwinding cleanup and are carried by the
    // call-context structs below, outside this SEH leaf.
    DWORD InvokeSehProtected(SehInvocation invocation, void* context)
    {
        __try
        {
            invocation(context);
            return ERROR_SUCCESS;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return GetExceptionCode();
        }
    }

    struct PointerInputCall final
    {
        GuestWindowManager* manager;
        PointerEventArgs^ args;
        UINT message;
    };

    struct WheelInputCall final
    {
        GuestWindowManager* manager;
        PointerEventArgs^ args;
    };

    struct KeyInputCall final
    {
        GuestWindowManager* manager;
        KeyEventArgs^ args;
        UINT message;
    };

    struct CharacterInputCall final
    {
        GuestWindowManager* manager;
        CharacterReceivedEventArgs^ args;
    };

    void SetWin32Error(DWORD* output, DWORD value)
    {
        if (output)
        {
            *output = value;
        }
    }

    // Guest window procedures are native code from the mapped PE. They run
    // in the same process as the UWP host, so an access violation must never
    // be allowed to tear down CoreShell without leaving diagnostics. Keep the
    // SEH boundary in this tiny leaf function: MSVC forbids __try in functions
    // which need C++ object unwinding.
    LRESULT InvokeGuestWindowProcedure(
        GuestAbi::WndProc procedure,
        HWND window,
        UINT message,
        WPARAM wParam,
        LPARAM lParam,
        DWORD* exceptionCode)
    {
        if (exceptionCode)
        {
            *exceptionCode = ERROR_SUCCESS;
        }

        __try
        {
            return procedure(window, message, wParam, lParam);
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            if (exceptionCode)
            {
                *exceptionCode = GetExceptionCode();
            }
            return 0;
        }
    }

    std::wstring Lowercase(std::wstring value)
    {
        for (auto& character : value)
        {
            character = static_cast<wchar_t>(towlower(character));
        }
        return value;
    }

    bool IsAtomPointer(LPCWSTR value)
    {
        return reinterpret_cast<ULONG_PTR>(value) != 0 && reinterpret_cast<ULONG_PTR>(value) <= 0xffff;
    }

    // These are bridge-owned equivalents of the small set of predefined
    // USER32 control classes needed by ordinary dialog-style PEs.  They are
    // deliberately not host XAML controls: the resulting pixels, focus and
    // message dispatch all remain in the guest window model.
    enum class BuiltinControlKind : std::uint8_t
    {
        None,
        Static,
        Button,
        Edit,
        ListBox,
        ComboBox,
        ScrollBar,
        ListView,
        Header,
        Toolbar,
        StatusBar,
        Rebar,
        Tab,
        Progress,
        TreeView,
        UpDown,
        ToolTip
    };

    BuiltinControlKind BuiltinControlKindForClassName(LPCWSTR className)
    {
        if (!className || IsAtomPointer(className))
        {
            return BuiltinControlKind::None;
        }
        if (_wcsicmp(className, L"static") == 0)
        {
            return BuiltinControlKind::Static;
        }
        if (_wcsicmp(className, L"button") == 0)
        {
            return BuiltinControlKind::Button;
        }
        if (_wcsicmp(className, L"edit") == 0)
        {
            return BuiltinControlKind::Edit;
        }
        if (_wcsicmp(className, L"listbox") == 0)
        {
            return BuiltinControlKind::ListBox;
        }
        if (_wcsicmp(className, L"combobox") == 0)
        {
            return BuiltinControlKind::ComboBox;
        }
        if (_wcsicmp(className, L"comboboxex32") == 0)
        {
            return BuiltinControlKind::ComboBox;
        }
        if (_wcsicmp(className, L"scrollbar") == 0)
        {
            return BuiltinControlKind::ScrollBar;
        }
        // Common-controls window classes are pre-registered by comctl32 on
        // desktop Windows. Applications may create these directly rather
        // than through legacy helper functions, so they stay guest-owned virtual
        // controls; no desktop HWND or host common-control DLL is exposed.
        if (_wcsicmp(className, L"syslistview32") == 0)
        {
            return BuiltinControlKind::ListView;
        }
        if (_wcsicmp(className, L"sysheader32") == 0)
        {
            return BuiltinControlKind::Header;
        }
        if (_wcsicmp(className, L"toolbarwindow32") == 0)
        {
            return BuiltinControlKind::Toolbar;
        }
        if (_wcsicmp(className, L"msctls_statusbar32") == 0)
        {
            return BuiltinControlKind::StatusBar;
        }
        if (_wcsicmp(className, L"rebarwindow32") == 0)
        {
            return BuiltinControlKind::Rebar;
        }
        if (_wcsicmp(className, L"systabcontrol32") == 0)
        {
            return BuiltinControlKind::Tab;
        }
        if (_wcsicmp(className, L"msctls_progress32") == 0)
        {
            return BuiltinControlKind::Progress;
        }
        if (_wcsicmp(className, L"systreeview32") == 0)
        {
            return BuiltinControlKind::TreeView;
        }
        if (_wcsicmp(className, L"msctls_updown32") == 0 ||
            _wcsicmp(className, L"updown") == 0)
        {
            return BuiltinControlKind::UpDown;
        }
        if (_wcsicmp(className, L"tooltips_class32") == 0)
        {
            return BuiltinControlKind::ToolTip;
        }
        return BuiltinControlKind::None;
    }

    HBRUSH BuiltinControlBackground(BuiltinControlKind kind)
    {
        // WNDCLASS's COLOR_* + 1 convention is already interpreted by the
        // class-brush fallback in EraseGuestBackground.
        const ULONG_PTR colorPlusOne = kind == BuiltinControlKind::Button ||
            kind == BuiltinControlKind::Toolbar ||
            kind == BuiltinControlKind::StatusBar ||
            kind == BuiltinControlKind::Rebar ||
            kind == BuiltinControlKind::Tab ||
            kind == BuiltinControlKind::Progress
            ? 16 // COLOR_BTNFACE + 1
            : 6; // COLOR_WINDOW + 1
        return reinterpret_cast<HBRUSH>(colorPlusOne);
    }

    int DefaultExtent(int value, int fallback)
    {
        if (value == GuestAbi::CwUseDefault || value <= 0)
        {
            return fallback;
        }
        return (std::max)(1, (std::min)(value, 2048));
    }

    int DefaultCoordinate(int value)
    {
        return value == GuestAbi::CwUseDefault ? 0 : value;
    }

    struct EditVisualLine final
    {
        EditVisualLine() = default;
        EditVisualLine(size_t first, size_t last, size_t following)
            : start(first), end(last), next(following) {}

        size_t start = 0;
        size_t end = 0;
        size_t next = 0;
    };

    std::vector<EditVisualLine> BuildEditVisualLines(
        const std::wstring& text,
        bool wrap,
        int availableWidth,
        const std::function<int(const wchar_t*, size_t)>& measure)
    {
        std::vector<EditVisualLine> lines;
        size_t paragraphStart = 0;
        for (;;)
        {
            size_t paragraphEnd = paragraphStart;
            while (paragraphEnd < text.size() && text[paragraphEnd] != L'\r' &&
                text[paragraphEnd] != L'\n')
                ++paragraphEnd;

            if (paragraphStart == paragraphEnd)
            {
                lines.push_back(EditVisualLine{ paragraphStart, paragraphEnd, paragraphEnd });
            }
            else if (!wrap)
            {
                lines.push_back(EditVisualLine{ paragraphStart, paragraphEnd, paragraphEnd });
            }
            else
            {
                size_t lineStart = paragraphStart;
                while (lineStart < paragraphEnd)
                {
                    size_t low = 1;
                    size_t high = paragraphEnd - lineStart;
                    size_t fitting = 0;
                    while (low <= high)
                    {
                        const size_t middle = low + (high - low) / 2;
                        if (measure(text.data() + lineStart, middle) <= availableWidth)
                        {
                            fitting = middle;
                            low = middle + 1;
                        }
                        else
                        {
                            high = middle - 1;
                        }
                    }
                    if (fitting == 0) fitting = 1;
                    const size_t lineEnd = (std::min)(paragraphEnd, lineStart + fitting);
                    lines.push_back(EditVisualLine{ lineStart, lineEnd, lineEnd });
                    lineStart = lineEnd;
                }
            }

            if (paragraphEnd == text.size()) break;
            size_t nextParagraph = paragraphEnd + 1;
            if (text[paragraphEnd] == L'\r' && nextParagraph < text.size() &&
                text[nextParagraph] == L'\n')
                ++nextParagraph;
            lines.back().next = nextParagraph;
            paragraphStart = nextParagraph;
            if (paragraphStart == text.size())
            {
                lines.push_back(EditVisualLine{ paragraphStart, paragraphStart, paragraphStart });
                break;
            }
        }
        if (lines.empty()) lines.push_back(EditVisualLine{});
        return lines;
    }

    size_t EditLineForPosition(
        const std::vector<EditVisualLine>& lines,
        size_t position)
    {
        if (lines.empty()) return 0;
        for (size_t index = 0; index < lines.size(); ++index)
        {
            if (position < lines[index].next || index + 1 == lines.size()) return index;
        }
        return lines.size() - 1;
    }

    int SaturatingAdd(int value, int delta);

    std::wstring MenuCaptionForDisplay(const std::wstring& source)
    {
        std::wstring result;
        result.reserve(source.size());
        for (size_t index = 0; index < source.size(); ++index)
        {
            if (source[index] != L'&')
            {
                result.push_back(source[index]);
            }
            else if (index + 1 < source.size() && source[index + 1] == L'&')
            {
                result.push_back(L'&');
                ++index;
            }
        }
        return result;
    }

    int MenuBarItemWidth(const GuestMenuVisualItem& item)
    {
        const std::wstring caption = MenuCaptionForDisplay(item.text);
        const size_t cappedCharacters = (std::min)(caption.size(),
            static_cast<size_t>(((std::numeric_limits<int>::max)() - 16) / MiniGdi::DefaultTextGlyphWidth));
        return (std::max)(36, static_cast<int>(cappedCharacters) * MiniGdi::DefaultTextGlyphWidth + 16);
    }

    constexpr UINT MenuFlagGrayed = 0x0001;
    constexpr UINT MenuFlagDisabled = 0x0002;
    constexpr UINT MenuFlagChecked = 0x0008;
    constexpr UINT MenuFlagPopup = 0x0010;
    constexpr UINT MenuFlagHighlighted = 0x0080;
    constexpr UINT MenuFlagRadioCheck = 0x0200;
    constexpr UINT MenuFlagSeparator = 0x0800;
    constexpr UINT MenuFlagDefault = 0x1000;
    constexpr UINT MenuFlagMouseSelect = 0x8000;

    bool IsMenuItemDisabled(const GuestMenuVisualItem& item)
    {
        return (item.state & (MenuFlagGrayed | MenuFlagDisabled)) != 0;
    }

    bool IsMenuItemSeparator(const GuestMenuVisualItem& item)
    {
        return (item.type & MenuFlagSeparator) != 0 ||
            (item.identifier == 0 && !item.subMenu && item.text.empty());
    }

    int PopupMenuWidth(const std::vector<GuestMenuVisualItem>& items)
    {
        size_t captionCharacters = 0;
        size_t shortcutCharacters = 0;
        for (const auto& item : items)
        {
            const std::wstring display = MenuCaptionForDisplay(item.text);
            const size_t tab = display.find(L'\t');
            captionCharacters = (std::max)(captionCharacters,
                tab == std::wstring::npos ? display.size() : tab);
            if (tab != std::wstring::npos)
                shortcutCharacters = (std::max)(shortcutCharacters, display.size() - tab - 1);
        }
        const size_t maximumCharacters = static_cast<size_t>(
            ((std::numeric_limits<int>::max)() - 72) / MiniGdi::DefaultTextGlyphWidth);
        const size_t characters = (std::min)(maximumCharacters,
            captionCharacters + (shortcutCharacters ? shortcutCharacters + 3 : 0));
        return (std::max)(96, static_cast<int>(characters) *
            MiniGdi::DefaultTextGlyphWidth + 56);
    }

    int PopupMenuItemHeight(const GuestMenuVisualItem& item)
    {
        return IsMenuItemSeparator(item) ? (std::max)(5, GuestMetrics::MenuHeight / 3) :
            GuestMetrics::MenuHeight;
    }

    int PopupMenuHeight(const std::vector<GuestMenuVisualItem>& items)
    {
        int result = 0;
        for (const auto& item : items)
            result = SaturatingAdd(result, PopupMenuItemHeight(item));
        return result;
    }

    int PopupMenuItemTop(const std::vector<GuestMenuVisualItem>& items, size_t index)
    {
        int result = 0;
        for (size_t current = 0; current < index && current < items.size(); ++current)
            result = SaturatingAdd(result, PopupMenuItemHeight(items[current]));
        return result;
    }

    int PopupMenuItemAt(const std::vector<GuestMenuVisualItem>& items, int y)
    {
        if (y < 0) return -1;
        int top = 0;
        for (size_t index = 0; index < items.size(); ++index)
        {
            const int bottom = SaturatingAdd(top, PopupMenuItemHeight(items[index]));
            if (y < bottom) return static_cast<int>(index);
            top = bottom;
        }
        return -1;
    }

    wchar_t MenuMnemonic(const std::wstring& source)
    {
        for (size_t index = 0; index + 1 < source.size(); ++index)
        {
            if (source[index] != L'&') continue;
            if (source[index + 1] == L'&') { ++index; continue; }
            return static_cast<wchar_t>(towupper(source[index + 1]));
        }
        return 0;
    }

    int MenuMnemonicDisplayIndex(const std::wstring& source)
    {
        int displayIndex = 0;
        for (size_t index = 0; index < source.size(); ++index)
        {
            if (source[index] == L'\t') break;
            if (source[index] != L'&') { ++displayIndex; continue; }
            if (index + 1 < source.size() && source[index + 1] == L'&')
            {
                ++displayIndex;
                ++index;
                continue;
            }
            return index + 1 < source.size() ? displayIndex : -1;
        }
        return -1;
    }

    UINT MenuSelectFlags(const GuestMenuVisualItem& item, bool mouseSelection)
    {
        UINT flags = item.type | item.state;
        if (item.subMenu) flags |= MenuFlagPopup;
        if (mouseSelection) flags |= MenuFlagMouseSelect;
        return flags & 0xffffu;
    }

    constexpr BYTE ToolbarStyleSeparator = 0x01;

    void DrawToolbarGlyph(MiniGdi::Surface& surface, const MiniGdi::Rect& button, int glyph)
    {
        const MiniGdi::Color ink = MiniGdi::MakeColor(48, 72, 96);
        const int centerX = (button.left + button.right) / 2;
        const int centerY = (button.top + button.bottom) / 2;
        const int radius = (std::max)(3, (std::min)(button.right - button.left, button.bottom - button.top) / 4);
        switch (glyph < 0 ? 0 : glyph % 7)
        {
        case 0: // Back / navigation
            MiniGdi::DrawLine(surface, MiniGdi::Point{ centerX + radius, centerY }, MiniGdi::Point{ centerX - radius, centerY }, ink);
            MiniGdi::DrawLine(surface, MiniGdi::Point{ centerX - radius, centerY }, MiniGdi::Point{ centerX - 1, centerY - radius }, ink);
            MiniGdi::DrawLine(surface, MiniGdi::Point{ centerX - radius, centerY }, MiniGdi::Point{ centerX - 1, centerY + radius }, ink);
            break;
        case 1: // Up
            MiniGdi::DrawLine(surface, MiniGdi::Point{ centerX, centerY + radius }, MiniGdi::Point{ centerX, centerY - radius }, ink);
            MiniGdi::DrawLine(surface, MiniGdi::Point{ centerX, centerY - radius }, MiniGdi::Point{ centerX - radius, centerY - 1 }, ink);
            MiniGdi::DrawLine(surface, MiniGdi::Point{ centerX, centerY - radius }, MiniGdi::Point{ centerX + radius, centerY - 1 }, ink);
            break;
        case 2: // Add
            MiniGdi::DrawLine(surface, MiniGdi::Point{ centerX - radius, centerY }, MiniGdi::Point{ centerX + radius, centerY }, ink);
            MiniGdi::DrawLine(surface, MiniGdi::Point{ centerX, centerY - radius }, MiniGdi::Point{ centerX, centerY + radius }, ink);
            break;
        case 3: // Extract / save
            MiniGdi::DrawLine(surface, MiniGdi::Point{ centerX, centerY - radius }, MiniGdi::Point{ centerX, centerY + radius - 2 }, ink);
            MiniGdi::DrawLine(surface, MiniGdi::Point{ centerX, centerY + radius - 2 }, MiniGdi::Point{ centerX - radius, centerY + 1 }, ink);
            MiniGdi::DrawLine(surface, MiniGdi::Point{ centerX, centerY + radius - 2 }, MiniGdi::Point{ centerX + radius, centerY + 1 }, ink);
            MiniGdi::DrawLine(surface, MiniGdi::Point{ centerX - radius, centerY + radius }, MiniGdi::Point{ centerX + radius, centerY + radius }, ink);
            break;
        case 4: // Delete
            MiniGdi::DrawLine(surface, MiniGdi::Point{ centerX - radius, centerY - radius }, MiniGdi::Point{ centerX + radius, centerY + radius }, ink);
            MiniGdi::DrawLine(surface, MiniGdi::Point{ centerX + radius, centerY - radius }, MiniGdi::Point{ centerX - radius, centerY + radius }, ink);
            break;
        case 5: // Test / verify
            MiniGdi::DrawLine(surface, MiniGdi::Point{ centerX - radius, centerY }, MiniGdi::Point{ centerX - 1, centerY + radius }, ink);
            MiniGdi::DrawLine(surface, MiniGdi::Point{ centerX - 1, centerY + radius }, MiniGdi::Point{ centerX + radius, centerY - radius }, ink);
            break;
        default: // Information
            MiniGdi::DrawEllipse(surface, MiniGdi::Rect{ centerX - radius, centerY - radius, centerX + radius, centerY + radius }, MiniGdi::MakeColor(240, 248, 255), ink);
            MiniGdi::FillRect(surface, MiniGdi::Rect{ centerX, centerY - 1, centerX + 1, centerY + radius - 1 }, ink);
            MiniGdi::FillRect(surface, MiniGdi::Rect{ centerX, centerY - radius + 1, centerX + 1, centerY - radius + 2 }, ink);
            break;
        }
    }

    int ToolbarItemWidth(
        const std::vector<BYTE>& styles,
        const std::vector<int>& bitmaps,
        const std::vector<BYTE>& states,
        size_t index,
        int buttonWidth,
        const std::vector<std::wstring>* texts = nullptr)
    {
        if (index < states.size() && (states[index] & 0x08) != 0) // TBSTATE_HIDDEN
        {
            return 0;
        }
        const BYTE style = index < styles.size() ? styles[index] : 0;
        if ((style & ToolbarStyleSeparator) != 0)
        {
            const int requested = index < bitmaps.size() ? bitmaps[index] : 0;
            return requested > 0 ? (std::min)(requested, 128) : 8;
        }
        int width = (std::max)(GuestMetrics::DefaultBitmapExtent, buttonWidth);
        if (texts && index < texts->size() && !(*texts)[index].empty())
        {
            const size_t boundedLength = (std::min)((*texts)[index].size(),
                static_cast<size_t>((std::numeric_limits<int>::max)() /
                    GuestMetrics::TextWidth));
            const int textWidth = static_cast<int>(boundedLength) * GuestMetrics::TextWidth;
            width = (std::max)(width,
                textWidth + 2 * GuestMetrics::ControlHorizontalPadding);
        }
        return width;
    }

    int ToolbarItemLeft(
        const std::vector<BYTE>& styles,
        const std::vector<int>& bitmaps,
        const std::vector<BYTE>& states,
        size_t index,
        int buttonWidth,
        const std::vector<std::wstring>* texts = nullptr)
    {
        int left = 2;
        for (size_t previous = 0; previous < index; ++previous)
        {
            const int width = ToolbarItemWidth(styles, bitmaps, states,
                previous, buttonWidth, texts);
            if (width != 0)
            {
                left += width + 1;
            }
        }
        return left;
    }

    int ToolbarContentWidth(
        const std::vector<BYTE>& styles,
        const std::vector<int>& bitmaps,
        const std::vector<BYTE>& states,
        size_t itemCount,
        int buttonWidth,
        const std::vector<std::wstring>* texts = nullptr)
    {
        int right = 0;
        for (size_t index = 0; index < itemCount; ++index)
        {
            const int width = ToolbarItemWidth(styles, bitmaps, states,
                index, buttonWidth, texts);
            if (width > 0)
            {
                right = ToolbarItemLeft(styles, bitmaps, states,
                    index, buttonWidth, texts) + width;
            }
        }
        return right > 0 ? right + 2 : 0;
    }

    void DrawToolbarImage(MiniGdi::Surface& destination, const MiniGdi::Rect& destinationRect, const MiniGdi::Surface& source)
    {
        const int width = destinationRect.right - destinationRect.left;
        const int height = destinationRect.bottom - destinationRect.top;
        if (source.Empty() || width <= 0 || height <= 0)
        {
            return;
        }
        for (int y = 0; y < height; ++y)
        {
            const int sourceY = (std::min)(source.Height() - 1, y * source.Height() / height);
            for (int x = 0; x < width; ++x)
            {
                const int sourceX = (std::min)(source.Width() - 1, x * source.Width() / width);
                MiniGdi::Color* target = destination.PixelAt(destinationRect.left + x, destinationRect.top + y);
                const MiniGdi::Color* pixel = source.PixelAt(sourceX, sourceY);
                if (target && pixel && MiniGdi::Alpha(*pixel) != 0)
                {
                    *target = *pixel;
                }
            }
        }
    }

    constexpr UINT GuestSwpNoSize = 0x0001;
    constexpr UINT GuestSwpNoMove = 0x0002;
    constexpr UINT GuestSwpNoZOrder = 0x0004;
    constexpr UINT GuestSwpNoRedraw = 0x0008;
    constexpr UINT GuestSwpNoActivate = 0x0010;
    constexpr UINT GuestSwpFrameChanged = 0x0020;
    constexpr UINT GuestSwpShowWindow = 0x0040;
    constexpr UINT GuestSwpHideWindow = 0x0080;
    constexpr UINT GuestSwpNoSendChanging = 0x0400;
    constexpr int MaximumWindowExtraBytes = 64 * 1024;
    constexpr DWORD GuestWsVisible = 0x10000000u;
    constexpr DWORD GuestWsDisabled = 0x08000000u;
    constexpr ULONG_PTR MaximumSystemColorBrush = 31;
    constexpr size_t MaximumBuiltinControlTextLength = 32767;
    constexpr UINT ListBoxAddString = 0x0180;
    constexpr UINT ListBoxInsertString = 0x0181;
    constexpr UINT ListBoxDeleteString = 0x0182;
    constexpr UINT ListBoxResetContent = 0x0184;
    constexpr UINT ListBoxSetCurrentSelection = 0x0186;
    constexpr UINT ListBoxGetCurrentSelection = 0x0188;
    constexpr UINT ListBoxGetText = 0x0189;
    constexpr UINT ListBoxGetCount = 0x018b;
    constexpr UINT ComboBoxAddString = 0x0143;
    constexpr UINT ComboBoxGetCurrentSelection = 0x0147;
    constexpr UINT ComboBoxGetText = 0x0148;
    constexpr UINT ComboBoxResetContent = 0x014b;
    constexpr UINT ComboBoxSetCurrentSelection = 0x014e;
    constexpr UINT ComboBoxShowDropDown = 0x014f;
    constexpr UINT ComboBoxGetDroppedControlRect = 0x0152;
    constexpr UINT ComboBoxGetDroppedState = 0x0157;
    constexpr UINT ComboBoxSetDroppedWidth = 0x0160;
    constexpr UINT ComboBoxGetCount = 0x0146;
    constexpr UINT ComboNotificationSelectionChange = 1;
    constexpr UINT ComboNotificationDropDown = 7;
    constexpr UINT ComboNotificationCloseUp = 8;
    constexpr DWORD ComboBoxStyleSimple = 0x0001;
    constexpr UINT ComboBoxExSetImageList = 0x0402;
    constexpr UINT ComboBoxExGetImageList = 0x0403;
    constexpr UINT ComboBoxExGetComboControl = 0x0406;
    constexpr UINT ComboBoxExGetEditControl = 0x0407;
    constexpr UINT ComboBoxExInsertItemW = 0x040b;
    constexpr UINT ComboBoxExSetItemW = 0x040c;
    constexpr UINT ComboBoxExGetItemW = 0x040d;
    constexpr UINT ScrollBarSetPosition = 0x00e0;
    constexpr UINT ScrollBarGetPosition = 0x00e1;
    constexpr UINT EditGetSelection = 0x00b0;
    constexpr UINT EditSetSelection = 0x00b1;
    constexpr UINT EditSetHandle = 0x00bc;
    constexpr UINT EditGetHandle = 0x00bd;
    constexpr UINT EditGetLineCount = 0x00ba;
    constexpr UINT EditLineIndex = 0x00bb;
    constexpr UINT EditLineLength = 0x00c1;
    constexpr UINT EditReplaceSelection = 0x00c2;
    constexpr UINT EditGetLine = 0x00c4;
    constexpr UINT EditLimitText = 0x00c5;
    constexpr UINT EditLineFromCharacter = 0x00c9;
    constexpr UINT EditGetFirstVisibleLine = 0x00ce;
    constexpr UINT EditSetPasswordCharacter = 0x00cc;
    constexpr UINT EditSetReadOnly = 0x00cf;
    constexpr UINT EditGetPasswordCharacter = 0x00d2;
    constexpr UINT EditGetLimitText = 0x00d5;
    constexpr UINT CommonControlSetUnicodeFormat = 0x2005;
    constexpr UINT CommonControlGetUnicodeFormat = 0x2006;
    constexpr UINT ListViewGetBackgroundColor = 0x1000;
    constexpr UINT ListViewGetImageList = 0x1002;
    constexpr UINT ListViewGetItemCount = 0x1004;
    constexpr UINT ListViewGetCallbackMask = 0x100a;
    constexpr UINT ListViewSetBackgroundColor = 0x1001;
    constexpr UINT ListViewSetImageList = 0x1003;
    constexpr UINT ListViewDeleteItem = 0x1008;
    constexpr UINT ListViewDeleteAllItems = 0x1009;
    constexpr UINT ListViewGetNextItem = 0x100c;
    constexpr UINT ListViewSetCallbackMask = 0x100b;
    constexpr UINT ListViewGetItemRect = 0x100e;
    constexpr UINT ListViewGetItemPosition = 0x1010;
    constexpr UINT ListViewHitTest = 0x1012;
    constexpr UINT ListViewRedrawItems = 0x1015;
    constexpr UINT ListViewSetColumnWidth = 0x101e;
    constexpr UINT ListViewGetColumnWidth = 0x101d;
    constexpr UINT ListViewGetTextColor = 0x1023;
    constexpr UINT ListViewSetTextColor = 0x1024;
    constexpr UINT ListViewGetTextBackgroundColor = 0x1025;
    constexpr UINT ListViewSetTextBackgroundColor = 0x1026;
    constexpr UINT ListViewGetItemState = 0x102c;
    constexpr UINT ListViewSetItemState = 0x102b;
    constexpr UINT ListViewSetItemCount = 0x102f;
    constexpr UINT ListViewEnsureVisible = 0x1013;
    constexpr UINT ListViewScroll = 0x1014;
    constexpr UINT ListViewGetTopIndex = 0x1027;
    constexpr UINT ListViewGetCountPerPage = 0x1028;
    constexpr UINT ListViewSetExtendedStyle = 0x1036;
    constexpr UINT ListViewGetExtendedStyle = 0x1037;
    constexpr UINT ListViewGetSubItemRect = 0x1038;
    constexpr UINT ListViewSubItemHitTest = 0x1039;
    constexpr UINT ListViewSetColumnOrderArray = 0x103a;
    constexpr UINT ListViewGetColumnOrderArray = 0x103b;
    constexpr UINT ListViewGetSelectionMark = 0x1042;
    constexpr UINT ListViewSetSelectionMark = 0x1043;
    constexpr UINT ListViewSortItems = 0x1030;
    constexpr UINT ListViewGetSelectedCount = 0x1032;
    constexpr UINT ListViewDeleteColumn = 0x101c;
    constexpr UINT ListViewInsertItemW = 0x104d;
    constexpr UINT ListViewSetItemW = 0x104c;
    constexpr UINT ListViewSetItemTextW = 0x1074;
    constexpr UINT ListViewEditLabelW = 0x1076;
    constexpr UINT ListViewGetEditControl = 0x1018;
    constexpr UINT ListViewGetItemTextW = 0x1073;
    constexpr UINT ListViewGetItemW = 0x104b;
    constexpr UINT ListViewGetColumnW = 0x105f;
    constexpr UINT ListViewGetStringWidthW = 0x1057;
    constexpr UINT ListViewInsertColumnW = 0x1061;
    constexpr UINT ListViewSetColumnW = 0x1060;
    constexpr UINT ToolbarButtonStructSize = 0x041e;
    constexpr UINT ToolbarEnableButton = 0x0401;
    constexpr UINT ToolbarCheckButton = 0x0402;
    constexpr UINT ToolbarPressButton = 0x0403;
    constexpr UINT ToolbarHideButton = 0x0404;
    constexpr UINT ToolbarIndeterminateButton = 0x0405;
    constexpr UINT ToolbarMarkButton = 0x0406;
    constexpr UINT ToolbarIsButtonEnabled = 0x0409;
    constexpr UINT ToolbarIsButtonChecked = 0x040a;
    constexpr UINT ToolbarIsButtonPressed = 0x040b;
    constexpr UINT ToolbarIsButtonHidden = 0x040c;
    constexpr UINT ToolbarIsButtonIndeterminate = 0x040d;
    constexpr UINT ToolbarIsButtonHighlighted = 0x040e;
    constexpr UINT ToolbarSetState = 0x0411;
    constexpr UINT ToolbarGetState = 0x0412;
    constexpr UINT ToolbarDeleteButton = 0x0416;
    constexpr UINT ToolbarGetButton = 0x0417;
    constexpr UINT ToolbarCommandToIndex = 0x0419;
    constexpr UINT ToolbarSetButtonSize = 0x041f;
    constexpr UINT ToolbarSetBitmapSize = 0x0420;
    constexpr UINT ToolbarAutoSize = 0x0421;
    constexpr UINT ToolbarAddButtonsW = 0x0444;
    constexpr UINT ToolbarGetButtonTextW = 0x044b;
    constexpr UINT ToolbarAddStringW = 0x044d;
    constexpr UINT ToolbarGetButtonCount = 0x0418;
    constexpr UINT ToolbarGetRows = 0x0428;
    constexpr UINT ToolbarGetButtonSize = 0x043a;
    constexpr UINT ToolbarSetImageList = 0x0430;
    constexpr UINT ToolbarGetImageList = 0x0431;
    constexpr UINT ToolbarGetItemRect = 0x041d;
    constexpr UINT ToolbarSetCommandId = 0x042a;
    constexpr UINT ToolbarChangeBitmap = 0x042b;
    constexpr UINT ToolbarGetBitmap = 0x042c;
    constexpr UINT ToolbarGetRect = 0x0433;
    constexpr UINT ToolbarSetButtonWidth = 0x043b;
    constexpr UINT ToolbarInsertButtonW = 0x0443;
    constexpr UINT ToolbarHitTest = 0x0445;
    constexpr UINT ToolbarGetMaxSize = 0x0453;
    constexpr UINT RebarSetBarInfo = 0x0404;
    constexpr UINT RebarInsertBandW = 0x040a;
    constexpr UINT RebarSetBandInfoW = 0x040b;
    constexpr UINT RebarSizeToRect = 0x0417;
    constexpr UINT RebarGetBandCount = 0x040c;
    constexpr UINT RebarGetBarHeight = 0x041b;
    constexpr UINT RebarGetRowHeight = 0x041c;
    constexpr UINT RebarBandMaskStyle = 0x00000001;
    constexpr UINT RebarBandMaskChild = 0x00000010;
    constexpr UINT RebarBandMaskChildSize = 0x00000020;
    constexpr UINT RebarBandMaskSize = 0x00000040;
    constexpr UINT RebarBandStyleBreak = 0x00000001;
    constexpr UINT RebarBandStyleFixedSize = 0x00000002;
    constexpr UINT RebarBandStyleHidden = 0x00000008;
    constexpr UINT StatusBarSetTextW = 0x040b;
    constexpr UINT StatusBarGetTextLengthW = 0x040c;
    constexpr UINT StatusBarGetTextW = 0x040d;
    constexpr UINT StatusBarSetParts = 0x0404;
    constexpr UINT StatusBarGetParts = 0x0406;
    constexpr UINT StatusBarGetRect = 0x040a;
    constexpr UINT StatusBarSetMinimumHeight = 0x0408;
    constexpr UINT StatusBarSetSimple = 0x0409;
    constexpr UINT StatusBarIsSimple = 0x040e;
    constexpr UINT ProgressSetRange = 0x0401;
    constexpr UINT ProgressSetPosition = 0x0402;
    constexpr UINT ProgressDeltaPosition = 0x0403;
    constexpr UINT ProgressSetStep = 0x0404;
    constexpr UINT ProgressStep = 0x0405;
    constexpr UINT ProgressSetRange32 = 0x0406;
    constexpr UINT ProgressGetRange = 0x0407;
    constexpr UINT ProgressGetPosition = 0x0408;
    constexpr UINT ProgressSetBarColor = 0x0409;
    constexpr UINT ProgressSetBackgroundColor = 0x2001;
    constexpr UINT ProgressSetMarquee = 0x040a;
    constexpr UINT ProgressGetStep = 0x040d;
    constexpr UINT ProgressGetBackgroundColor = 0x040e;
    constexpr UINT ProgressGetBarColor = 0x040f;
    constexpr UINT ProgressSetState = 0x0410;
    constexpr UINT ProgressGetState = 0x0411;
    constexpr UINT TabGetImageList = 0x1302;
    constexpr UINT TabSetImageList = 0x1303;
    constexpr UINT TabGetItemCount = 0x1304;
    constexpr UINT TabDeleteItem = 0x1308;
    constexpr UINT TabDeleteAllItems = 0x1309;
    constexpr UINT TabGetItemRect = 0x130a;
    constexpr UINT TabGetCurrentSelection = 0x130b;
    constexpr UINT TabSetCurrentSelection = 0x130c;
    constexpr UINT TabHitTest = 0x130d;
    constexpr UINT TabAdjustRect = 0x1328;
    constexpr UINT TabSetItemSize = 0x1329;
    constexpr UINT TabSetPadding = 0x132b;
    constexpr UINT TabGetRowCount = 0x132c;
    constexpr UINT TabGetCurrentFocus = 0x132f;
    constexpr UINT TabSetCurrentFocus = 0x1330;
    constexpr UINT TabGetItemW = 0x133c;
    constexpr UINT TabSetItemW = 0x133d;
    constexpr UINT TabInsertItemW = 0x133e;
    constexpr UINT TabItemText = 0x0001;
    constexpr UINT TabItemImage = 0x0002;
    constexpr UINT TabItemParam = 0x0008;
    constexpr UINT TabItemState = 0x0010;
    constexpr UINT TabNotifySelectionChange = static_cast<UINT>(-551);
    constexpr UINT TabNotifySelectionChanging = static_cast<UINT>(-552);
    constexpr UINT HeaderGetItemCount = 0x1200;
    constexpr UINT HeaderDeleteItem = 0x1202;
    constexpr UINT HeaderLayout = 0x1205;
    constexpr UINT HeaderHitTest = 0x1206;
    constexpr UINT HeaderGetItemRect = 0x1207;
    constexpr UINT HeaderSetImageList = 0x1208;
    constexpr UINT HeaderGetImageList = 0x1209;
    constexpr UINT HeaderInsertItemW = 0x120a;
    constexpr UINT HeaderGetItemW = 0x120b;
    constexpr UINT HeaderSetItemW = 0x120c;
    constexpr UINT HeaderOrderToIndex = 0x120f;
    constexpr UINT HeaderGetOrderArray = 0x1211;
    constexpr UINT HeaderSetOrderArray = 0x1212;
    constexpr UINT HeaderItemWidth = 0x0001;
    constexpr UINT HeaderItemText = 0x0002;
    constexpr UINT HeaderItemFormat = 0x0004;
    constexpr UINT HeaderItemParam = 0x0008;
    constexpr UINT HeaderItemImage = 0x0020;
    constexpr UINT HeaderItemOrder = 0x0080;
    constexpr UINT HeaderNotifyItemClickW = static_cast<UINT>(-322);
    constexpr UINT TreeDeleteItem = 0x1101;
    constexpr UINT TreeExpand = 0x1102;
    constexpr UINT TreeGetItemRect = 0x1104;
    constexpr UINT TreeGetCount = 0x1105;
    constexpr UINT TreeGetIndent = 0x1106;
    constexpr UINT TreeSetIndent = 0x1107;
    constexpr UINT TreeGetImageList = 0x1108;
    constexpr UINT TreeSetImageList = 0x1109;
    constexpr UINT TreeGetNextItem = 0x110a;
    constexpr UINT TreeSelectItem = 0x110b;
    constexpr UINT TreeGetVisibleCount = 0x1110;
    constexpr UINT TreeHitTest = 0x1111;
    constexpr UINT TreeEnsureVisible = 0x1114;
    constexpr UINT TreeSetItemHeight = 0x111b;
    constexpr UINT TreeGetItemHeight = 0x111c;
    constexpr UINT TreeSetBackgroundColor = 0x111d;
    constexpr UINT TreeSetTextColor = 0x111e;
    constexpr UINT TreeGetBackgroundColor = 0x111f;
    constexpr UINT TreeGetTextColor = 0x1120;
    constexpr UINT TreeGetItemState = 0x1127;
    constexpr UINT TreeInsertItemW = 0x1132;
    constexpr UINT TreeGetItemW = 0x113e;
    constexpr UINT TreeSetItemW = 0x113f;
    constexpr UINT TreeItemText = 0x0001;
    constexpr UINT TreeItemImage = 0x0002;
    constexpr UINT TreeItemParam = 0x0004;
    constexpr UINT TreeItemState = 0x0008;
    constexpr UINT TreeItemHandle = 0x0010;
    constexpr UINT TreeItemSelectedImage = 0x0020;
    constexpr UINT TreeItemChildren = 0x0040;
    constexpr UINT TreeStateSelected = 0x0002;
    constexpr UINT TreeStateExpanded = 0x0020;
    constexpr UINT TreeExpandCollapse = 0x0001;
    constexpr UINT TreeExpandExpand = 0x0002;
    constexpr UINT TreeExpandToggle = 0x0003;
    constexpr UINT TreeNextRoot = 0x0000;
    constexpr UINT TreeNextSibling = 0x0001;
    constexpr UINT TreePreviousSibling = 0x0002;
    constexpr UINT TreeParent = 0x0003;
    constexpr UINT TreeChild = 0x0004;
    constexpr UINT TreeFirstVisible = 0x0005;
    constexpr UINT TreeNextVisible = 0x0006;
    constexpr UINT TreePreviousVisible = 0x0007;
    constexpr UINT TreeCaret = 0x0009;
    constexpr UINT TreeLastVisible = 0x000a;
    constexpr UINT TreeNotifySelectionChangingW = static_cast<UINT>(-450);
    constexpr UINT TreeNotifySelectionChangedW = static_cast<UINT>(-451);
    constexpr UINT UpDownSetRange = 0x0465;
    constexpr UINT UpDownGetRange = 0x0466;
    constexpr UINT UpDownSetPosition = 0x0467;
    constexpr UINT UpDownGetPosition = 0x0468;
    constexpr UINT UpDownSetBuddy = 0x0469;
    constexpr UINT UpDownGetBuddy = 0x046a;
    constexpr UINT UpDownSetBase = 0x046d;
    constexpr UINT UpDownGetBase = 0x046e;
    constexpr UINT UpDownSetRange32 = 0x046f;
    constexpr UINT UpDownGetRange32 = 0x0470;
    constexpr UINT UpDownSetPosition32 = 0x0471;
    constexpr UINT UpDownGetPosition32 = 0x0472;
    constexpr UINT UpDownNotifyDeltaPosition = static_cast<UINT>(-722);
    constexpr UINT ListViewColumnFormat = 0x0001;
    constexpr UINT ListViewColumnWidth = 0x0002;
    constexpr UINT ListViewColumnText = 0x0004;
    constexpr UINT ListViewColumnSubItem = 0x0008;
    constexpr UINT ListViewColumnImage = 0x0010;
    constexpr UINT ListViewColumnOrder = 0x0020;
    constexpr UINT ListViewItemText = 0x0001;
    constexpr UINT ListViewItemImage = 0x0002;
    constexpr UINT ListViewItemParam = 0x0004;
    constexpr UINT ListViewItemState = 0x0008;
    constexpr UINT ListViewStateSelected = 0x0002;
    constexpr UINT ListViewStateFocused = 0x0001;
    constexpr UINT ListViewHitNowhere = 0x0001;
    constexpr UINT ListViewHitOnItemIcon = 0x0002;
    constexpr UINT ListViewHitOnItemLabel = 0x0004;
    // LVS_OWNERDATA. LVM_SETITEMCOUNT is defined only for this virtual
    // ListView style; applying it to a retained-item view invents blank rows.
    constexpr DWORD ListViewStyleOwnerData = 0x00001000u;
    constexpr UINT ListViewExtendedGridLines = 0x00000001;
    constexpr UINT ListViewNextItemSelected = 0x0002;
    constexpr UINT ListViewNotifyGetDisplayInfoW = static_cast<UINT>(-177); // LVN_GETDISPINFOW
    constexpr UINT ListViewNotifyItemChanged = static_cast<UINT>(-101); // LVN_ITEMCHANGED
    constexpr UINT ListViewNotifyItemActivate = static_cast<UINT>(-114); // LVN_ITEMACTIVATE
    constexpr UINT ListViewNotifyBeginLabelEditW = static_cast<UINT>(-175); // LVN_BEGINLABELEDITW
    constexpr UINT ListViewNotifyEndLabelEditW = static_cast<UINT>(-176); // LVN_ENDLABELEDITW
    constexpr UINT NotifyClick = static_cast<UINT>(-2); // NM_CLICK
    constexpr UINT NotifyDoubleClick = static_cast<UINT>(-3); // NM_DBLCLK
    constexpr BYTE ToolbarStateEnabled = 0x04;
    constexpr BYTE ToolbarStateChecked = 0x01;
    constexpr BYTE ToolbarStatePressed = 0x02;
    constexpr BYTE ToolbarStateHidden = 0x08;
    constexpr BYTE ToolbarStateIndeterminate = 0x10;
    constexpr BYTE ToolbarStateMarked = 0x80;
    constexpr BYTE ToolbarStyleDropDown = 0x08;

    MiniGdi::Color ColorFromGuestColorRef(COLORREF color)
    {
        return MiniGdi::MakeColor(
            static_cast<std::uint8_t>(color & 0xff),
            static_cast<std::uint8_t>((color >> 8) & 0xff),
            static_cast<std::uint8_t>((color >> 16) & 0xff));
    }

    struct GuestListViewColumnW final
    {
        UINT mask;
        int format;
        int width;
        LPWSTR text;
        int textCapacity;
        int subItem;
        int image;
        int order;
    };

    struct GuestListViewItemW final
    {
        UINT mask;
        int item;
        int subItem;
        UINT state;
        UINT stateMask;
        LPWSTR text;
        int textCapacity;
        int image;
        LPARAM itemData;
    };

    // COMBOBOXEXITEMW through lParam. ComboBoxEx clients use the text field
    // to publish each item's current caption.
    struct GuestComboBoxExItemW final
    {
        UINT mask;
        int item;
        LPWSTR text;
        int textCapacity;
        int image;
        int selectedImage;
        int overlay;
        int indent;
        LPARAM itemData;
    };

    struct GuestNotifyHeader final
    {
        HWND from;
        UINT_PTR identifier;
        UINT code;
    };

    struct GuestListViewDisplayInfoW final
    {
        GuestNotifyHeader header;
        GuestListViewItemW item;
    };

    struct GuestListViewHitTestInfo final
    {
        POINT point;
        UINT flags;
        int item;
        int subItem;
        int group;
    };

    // NMLISTVIEW/NMITEMACTIVATE share this prefix.  The compact bridge model
    // deliberately sends the common fields for selection and activation, so
    // owners can keep their usual WM_NOTIFY dispatch rather than special-case
    // UWP input.
    struct GuestListViewNotification final
    {
        GuestNotifyHeader header;
        int item;
        int subItem;
        UINT newState;
        UINT oldState;
        UINT changed;
        POINT action;
        LPARAM itemData;
        UINT keyFlags;
    };
    static_assert(sizeof(GuestListViewNotification) == 72,
        "Guest NMITEMACTIVATE layout must remain x64-compatible.");

    using GuestListViewCompare = int(CALLBACK*)(LPARAM, LPARAM, LPARAM);

    int InvokeGuestListViewCompare(
        GuestListViewCompare compare,
        LPARAM left,
        LPARAM right,
        LPARAM parameter,
        DWORD* exceptionCode)
    {
        if (exceptionCode)
        {
            *exceptionCode = ERROR_SUCCESS;
        }
        __try
        {
            return compare(left, right, parameter);
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            if (exceptionCode)
            {
                *exceptionCode = GetExceptionCode();
            }
            return 0;
        }
    }

    struct GuestToolbarButton final
    {
        int bitmap;
        int command;
        BYTE state;
        BYTE style;
        BYTE reserved[6];
        UINT_PTR data;
        INT_PTR text;
    };

    struct GuestRebarBandInfoW final
    {
        UINT size;
        UINT mask;
        UINT style;
        COLORREF foreground;
        COLORREF background;
        LPWSTR text;
        UINT textCapacity;
        int image;
        HWND child;
        UINT minimumChildWidth;
        UINT minimumChildHeight;
        UINT width;
    };

    struct RebarBand final
    {
        HWND child = nullptr;
        UINT style = 0;
        int minimumWidth = GuestMetrics::ToolbarButtonExtent;
        int minimumHeight = GuestMetrics::ToolbarHeight;
        int width = 0;
        int naturalWidth = 0;
        int naturalHeight = 0;
    };

    int SetWindowPosExtent(int value);

    bool IsRebarBandHidden(const RebarBand& band)
    {
        return (band.style & RebarBandStyleHidden) != 0;
    }

    int RebarBandPreferredWidth(const RebarBand& band)
    {
        return (std::max)(band.minimumWidth,
            band.width > 0 ? band.width : band.naturalWidth);
    }

    int RebarBandHeight(const RebarBand& band)
    {
        // REBARBANDINFO permits a zero child height. In that case the common
        // control uses its normal control-strip height rather than collapsing
        // the row to a single pixel.
        if (band.minimumHeight > 0) return band.minimumHeight;
        if (band.naturalHeight > 0) return band.naturalHeight;
        return GuestMetrics::ToolbarHeight;
    }

    int ValidatedRebarDimension(UINT value)
    {
        // Match comctl32's defensive validation. Applications commonly leave
        // unused trailing REBARBANDINFO members uninitialized; Wine and native
        // comctl32 reject implausible values instead of turning them into a
        // gigantic band. Validate before applying the bridge surface limit.
        if (value > 65535u)
        {
            return 0;
        }
        return SetWindowPosExtent(static_cast<int>(value));
    }

    int ValidatedRebarChildHeight(UINT value)
    {
        const int height = ValidatedRebarDimension(value);
        // A horizontal rebar row cannot have a useful minimum taller than the
        // guest desktop. Values such as a ComboBox's requested drop-list
        // height belong to its popup, not to the closed band.
        return height > GuestMetrics::CurrentScreenHeight() ? 0 : height;
    }

    std::vector<int> RebarRowHeights(const std::vector<RebarBand>& bands, int availableWidth)
    {
        const int width = (std::max)(1, availableWidth);
        std::vector<int> heights;
        int rowWidth = 0;
        int rowHeight = 0;
        bool rowHasBand = false;
        for (const auto& band : bands)
        {
            if (IsRebarBandHidden(band) || !band.child) continue;
            const int desiredWidth = (std::min)(width,
                (std::max)(0, band.minimumWidth));
            const bool startsRow = rowHasBand &&
                (((band.style & RebarBandStyleBreak) != 0) || rowWidth + desiredWidth > width);
            if (startsRow)
            {
                heights.push_back(rowHeight);
                rowWidth = 0;
                rowHeight = 0;
                rowHasBand = false;
            }
            rowWidth = SaturatingAdd(rowWidth, desiredWidth);
            rowHeight = (std::max)(rowHeight, RebarBandHeight(band));
            rowHasBand = true;
        }
        if (rowHasBand) heights.push_back(rowHeight);
        return heights;
    }

    int RequiredRebarHeight(const std::vector<RebarBand>& bands, int availableWidth)
    {
        int totalHeight = 0;
        for (const int rowHeight : RebarRowHeights(bands, availableWidth))
        {
            totalHeight = SaturatingAdd(totalHeight, rowHeight);
        }
        return totalHeight > 0 ? totalHeight : GuestMetrics::ToolbarHeight;
    }

    struct GuestProgressRange final
    {
        int low;
        int high;
    };

    struct GuestTabItemW final
    {
        UINT mask;
        DWORD state;
        DWORD stateMask;
        LPWSTR text;
        int textCapacity;
        int image;
        LPARAM itemData;
    };

    struct GuestTabHitTestInfo final
    {
        POINT point;
        UINT flags;
    };

    struct GuestHeaderItemW final
    {
        UINT mask;
        int width;
        LPWSTR text;
        HBITMAP bitmap;
        int textCapacity;
        int format;
        LPARAM itemData;
        int image;
        int order;
    };

    struct GuestHeaderHitTestInfo final
    {
        POINT point;
        UINT flags;
        int item;
    };

    // WINDOWPOS is hidden by some UWP SDK partitions even though HDLAYOUT
    // still carries a pointer to its ABI. Keep the Win32 layout locally so the
    // guest structure remains binary-compatible without depending on that
    // desktop-only declaration.
    struct GuestWindowPosition final
    {
        HWND window;
        HWND insertAfter;
        int x;
        int y;
        int cx;
        int cy;
        UINT flags;
    };
    static_assert(sizeof(GuestWindowPosition) == 40,
        "Guest WINDOWPOS layout must remain x64-compatible.");

    struct GuestHeaderLayout final
    {
        RECT* rect;
        GuestWindowPosition* windowPosition;
    };

    struct GuestHeaderNotification final
    {
        GuestNotifyHeader header;
        int item;
        int button;
        GuestHeaderItemW* headerItem;
    };

    struct GuestTreeItemW final
    {
        UINT mask;
        HANDLE item;
        UINT state;
        UINT stateMask;
        LPWSTR text;
        int textCapacity;
        int image;
        int selectedImage;
        int children;
        LPARAM itemData;
    };

    struct GuestTreeInsertW final
    {
        HANDLE parent;
        HANDLE insertAfter;
        GuestTreeItemW item;
    };

    struct GuestTreeHitTestInfo final
    {
        POINT point;
        UINT flags;
        HANDLE item;
    };

    struct GuestTreeNotification final
    {
        GuestNotifyHeader header;
        UINT action;
        GuestTreeItemW oldItem;
        GuestTreeItemW newItem;
        POINT dragPoint;
    };

    struct GuestUpDownNotification final
    {
        GuestNotifyHeader header;
        int position;
        int delta;
    };

    struct TreeNode final
    {
        ULONG_PTR token = 0;
        ULONG_PTR parent = 0;
        std::vector<ULONG_PTR> children;
        std::wstring text;
        LPARAM itemData = 0;
        UINT state = 0;
        int image = -1;
        int selectedImage = -1;
        int declaredChildren = 0;
    };

    struct VisibleTreeNode final
    {
        ULONG_PTR token = 0;
        int depth = 0;
    };

    const TreeNode* FindTreeNode(const std::vector<TreeNode>& nodes, ULONG_PTR token)
    {
        const auto found = std::find_if(nodes.begin(), nodes.end(), [token](const TreeNode& node)
        {
            return node.token == token;
        });
        return found == nodes.end() ? nullptr : &*found;
    }

    TreeNode* FindTreeNode(std::vector<TreeNode>& nodes, ULONG_PTR token)
    {
        const auto found = std::find_if(nodes.begin(), nodes.end(), [token](const TreeNode& node)
        {
            return node.token == token;
        });
        return found == nodes.end() ? nullptr : &*found;
    }

    void AppendVisibleTreeNodes(
        const std::vector<TreeNode>& nodes,
        ULONG_PTR parent,
        int depth,
        std::vector<VisibleTreeNode>* visible)
    {
        if (!visible || visible->size() >= 4096 || depth > 128)
        {
            return;
        }
        for (const auto& node : nodes)
        {
            if (node.parent != parent)
            {
                continue;
            }
            visible->push_back(VisibleTreeNode{ node.token, depth });
            if (visible->size() >= 4096)
            {
                return;
            }
            if ((node.state & TreeStateExpanded) != 0)
            {
                AppendVisibleTreeNodes(nodes, node.token, depth + 1, visible);
            }
        }
    }

    std::vector<VisibleTreeNode> VisibleTreeNodes(const std::vector<TreeNode>& nodes)
    {
        std::vector<VisibleTreeNode> visible;
        visible.reserve(nodes.size());
        AppendVisibleTreeNodes(nodes, 0, 0, &visible);
        return visible;
    }

    // LVITEM/LVCOLUMN text is a pointer into guest-owned mapped memory.  It
    // is optional even when the structure itself is present, and real-world
    // callers use the (-1) callback sentinel too.  Do not let either an
    // uninitialised optional member or a bad guest pointer take down the host
    // UI thread.  The bounded probe happens before constructing a std::wstring
    // so C++ unwinding never crosses the SEH boundary.
    bool TryReadGuestWideString(LPCWSTR source, std::wstring* destination)
    {
        if (!destination)
        {
            return false;
        }
        destination->clear();
        const ULONG_PTR address = reinterpret_cast<ULONG_PTR>(source);
        if (address <= 0xffff || address == (std::numeric_limits<ULONG_PTR>::max)())
        {
            return false;
        }

        size_t length = 0;
        __try
        {
            while (length < MaximumBuiltinControlTextLength && source[length] != L'\0')
            {
                ++length;
            }
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return false;
        }
        if (length == MaximumBuiltinControlTextLength)
        {
            return false;
        }

        destination->assign(source, length);
        return true;
    }

    template <typename TValue>
    bool TryReadGuestValue(const TValue* source, TValue* destination)
    {
        if (!source || !destination || reinterpret_cast<ULONG_PTR>(source) <= 0xffff)
        {
            return false;
        }
        __try
        {
            *destination = *source;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return false;
        }
        return true;
    }

    bool TryWriteGuestWideString(LPWSTR destination, size_t capacity, const std::wstring& source, size_t* written)
    {
        if (written)
        {
            *written = 0;
        }
        if (!destination || capacity == 0 || reinterpret_cast<ULONG_PTR>(destination) <= 0xffff)
        {
            return false;
        }
        const size_t copyCount = (std::min)(source.size(), capacity - 1);
        __try
        {
            if (copyCount != 0)
            {
                memcpy(destination, source.data(), copyCount * sizeof(wchar_t));
            }
            destination[copyCount] = L'\0';
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return false;
        }
        if (written)
        {
            *written = copyCount;
        }
        return true;
    }

    template <typename TValue>
    bool TryWriteGuestValue(TValue* destination, const TValue& source)
    {
        if (!destination || reinterpret_cast<ULONG_PTR>(destination) <= 0xffff)
        {
            return false;
        }
        __try
        {
            *destination = source;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return false;
        }
        return true;
    }

    bool IsValidWindowExtraOffset(int index, size_t available, size_t valueSize)
    {
        if (index < 0 || valueSize == 0 || (index % static_cast<int>(valueSize)) != 0)
        {
            return false;
        }
        const size_t offset = static_cast<size_t>(index);
        return offset <= available && valueSize <= available - offset;
    }

    int SetWindowPosExtent(int value)
    {
        // The first surface is deliberately bounded to keep an untrusted PE
        // from turning one SetWindowPos call into a huge allocation.  Zero is
        // valid (unlike the CreateWindow default sizing rule).
        const int viewportLimit = (std::max)(GuestMetrics::CurrentScreenWidth(),
            GuestMetrics::CurrentScreenHeight());
        const int maximum = (std::min)(8192, (std::max)(2048, viewportLimit));
        return (std::max)(0, (std::min)(value, maximum));
    }

    bool HasArea(const RECT& rect)
    {
        return rect.left < rect.right && rect.top < rect.bottom;
    }

    RECT ClipToSurface(const RECT* requested, const MiniGdi::Surface& surface)
    {
        const LONG width = static_cast<LONG>((std::max)(0, surface.Width()));
        const LONG height = static_cast<LONG>((std::max)(0, surface.Height()));
        RECT clipped = requested
            ? *requested
            : RECT{ 0, 0, width, height };
        clipped.left = (std::max)(0L, (std::min)(clipped.left, width));
        clipped.top = (std::max)(0L, (std::min)(clipped.top, height));
        clipped.right = (std::max)(0L, (std::min)(clipped.right, width));
        clipped.bottom = (std::max)(0L, (std::min)(clipped.bottom, height));
        return clipped;
    }

    void UnionDamage(RECT* target, const RECT& addition)
    {
        if (!target || !HasArea(addition))
        {
            return;
        }
        if (!HasArea(*target))
        {
            *target = addition;
            return;
        }
        target->left = (std::min)(target->left, addition.left);
        target->top = (std::min)(target->top, addition.top);
        target->right = (std::max)(target->right, addition.right);
        target->bottom = (std::max)(target->bottom, addition.bottom);
    }

    MiniGdi::Color SystemColorBrush(ULONG_PTR encodedBrush)
    {
        // WNDCLASS permits (HBRUSH)(COLOR_* + 1). The guest is deliberately
        // given a small, stable palette rather than host theme handles.
        const ULONG_PTR color = encodedBrush - 1;
        switch (color)
        {
        case 1:  // COLOR_BACKGROUND
        case 6:  // COLOR_WINDOWFRAME
        case 7:  // COLOR_MENUTEXT
        case 8:  // COLOR_WINDOWTEXT
        case 18: // COLOR_BTNTEXT
            return MiniGdi::OpaqueBlack;
        case 9:  // COLOR_CAPTIONTEXT
        case 14: // COLOR_HIGHLIGHTTEXT
        case 20: // COLOR_BTNHIGHLIGHT
            return MiniGdi::OpaqueWhite;
        case 13: // COLOR_HIGHLIGHT
        case 26: // COLOR_HOTLIGHT
            return MiniGdi::MakeColor(0, 120, 215);
        case 16: // COLOR_BTNSHADOW
        case 17: // COLOR_GRAYTEXT
        case 21: // COLOR_3DDKSHADOW
            return MiniGdi::MakeColor(128, 128, 128);
        case 15: // COLOR_BTNFACE / COLOR_3DFACE
        case 22: // COLOR_3DLIGHT
        case 24: // COLOR_INFOBK
            return MiniGdi::MakeColor(240, 240, 240);
        case 5:  // COLOR_WINDOW
        default:
            return MiniGdi::OpaqueWhite;
        }
    }

    bool ToMiniGdiObject(HGDIOBJ object, MiniGdi::ObjectHandle* result)
    {
        if (!result)
        {
            return false;
        }
        const ULONG_PTR raw = reinterpret_cast<ULONG_PTR>(object);
        if (raw == 0 || raw > static_cast<ULONG_PTR>((std::numeric_limits<MiniGdi::ObjectHandle>::max)()))
        {
            return false;
        }
        *result = static_cast<MiniGdi::ObjectHandle>(raw);
        return true;
    }

    int SaturatingAdd(int value, int delta)
    {
        const std::int64_t result = static_cast<std::int64_t>(value) + delta;
        if (result > (std::numeric_limits<int>::max)())
        {
            return (std::numeric_limits<int>::max)();
        }
        if (result < (std::numeric_limits<int>::min)())
        {
            return (std::numeric_limits<int>::min)();
        }
        return static_cast<int>(result);
    }

    WORD SignedCoordinateWord(int value)
    {
        const int bounded = (std::max)(static_cast<int>((std::numeric_limits<short>::min)()),
            (std::min)(static_cast<int>((std::numeric_limits<short>::max)()), value));
        return static_cast<WORD>(static_cast<short>(bounded));
    }

    WPARAM MouseKeyState(PointerPointProperties^ properties)
    {
        if (!properties)
        {
            return 0;
        }

        WPARAM result = 0;
        if (properties->IsLeftButtonPressed) result |= GuestAbi::MkLButton;
        if (properties->IsMiddleButtonPressed) result |= GuestAbi::MkMButton;
        if (properties->IsRightButtonPressed) result |= GuestAbi::MkRButton;
        if (properties->IsXButton1Pressed) result |= GuestAbi::MkXButton1;
        if (properties->IsXButton2Pressed) result |= GuestAbi::MkXButton2;
        return result;
    }

    LPARAM MousePosition(
        int rootWidth,
        int rootHeight,
        int targetWidth,
        int targetHeight,
        int targetLeft,
        int targetTop,
        const Point& hostPosition,
        Image^ image,
        bool clampToTarget = true)
    {
        const int hostWidth = (std::max)(1, rootWidth);
        const int hostHeight = (std::max)(1, rootHeight);
        const int width = (std::max)(1, targetWidth);
        const int height = (std::max)(1, targetHeight);
        double x = hostPosition.X;
        double y = hostPosition.Y;

        // CoreWindow reports positions in root-view coordinates whereas the
        // guest surface is letterboxed inside its XAML Image. Convert to
        // client pixels so a resized/scaled guest sees normal WM_MOUSE*
        // coordinates rather than the app's global coordinates.
        if (image && image->ActualWidth > 0.0 && image->ActualHeight > 0.0)
        {
            try
            {
                GeneralTransform^ transform = image->TransformToVisual(nullptr);
                GeneralTransform^ inverse = transform ? transform->Inverse : nullptr;
                const Point localPosition = inverse
                    ? inverse->TransformPoint(hostPosition)
                    : hostPosition;
                const double scale = (std::min)(
                    image->ActualWidth / static_cast<double>(hostWidth),
                    image->ActualHeight / static_cast<double>(hostHeight));
                if (scale > 0.0)
                {
                    const double renderedWidth = static_cast<double>(hostWidth) * scale;
                    const double renderedHeight = static_cast<double>(hostHeight) * scale;
                    // Convert the CoreWindow point all the way back into the
                    // Image's local coordinate space first.  Subtracting only
                    // the transformed origin mixes physical view coordinates
                    // with pre-transform ActualWidth/ActualHeight when the
                    // CoreShell desktop has an outer RenderTransform (Xbox).
                    x = (localPosition.X - (image->ActualWidth - renderedWidth) / 2.0) / scale;
                    y = (localPosition.Y - (image->ActualHeight - renderedHeight) / 2.0) / scale;
                }
            }
            catch (...)
            {
                // Fall back to the root coordinates if the visual is being
                // torn down while an input callback is in flight.
            }
        }

        int pixelX = static_cast<int>(x) - targetLeft;
        int pixelY = static_cast<int>(y) - targetTop;
        if (clampToTarget)
        {
            pixelX = (std::max)(0, (std::min)(width - 1, pixelX));
            pixelY = (std::max)(0, (std::min)(height - 1, pixelY));
        }
        else
        {
            pixelX = static_cast<int>(static_cast<short>(SignedCoordinateWord(pixelX)));
            pixelY = static_cast<int>(static_cast<short>(SignedCoordinateWord(pixelY)));
        }
        return GuestAbi::MakeMouseLParam(
            SignedCoordinateWord(pixelX), SignedCoordinateWord(pixelY));
    }

    WORD XButtonFromUpdate(PointerUpdateKind update)
    {
        switch (update)
        {
        case PointerUpdateKind::XButton2Pressed:
        case PointerUpdateKind::XButton2Released:
            return GuestAbi::XButton2;
        default:
            return GuestAbi::XButton1;
        }
    }

    HDC ToGuestDc(MiniGdi::DcHandle dc)
    {
        return reinterpret_cast<HDC>(static_cast<ULONG_PTR>(dc));
    }

    MiniGdi::DcHandle FromGuestDc(HDC dc)
    {
        const ULONG_PTR value = reinterpret_cast<ULONG_PTR>(dc);
        return value > static_cast<ULONG_PTR>((std::numeric_limits<MiniGdi::DcHandle>::max)())
            ? MiniGdi::InvalidDc
            : static_cast<MiniGdi::DcHandle>(value);
    }
}

namespace Win32Bridge
{
namespace Bridge
{
struct GuestPresentationState final
{
    std::mutex lock;
    Platform::Agile<CoreDispatcher^> dispatcher;
    Platform::Agile<Image^> image;
    Platform::Agile<WriteableBitmap^> bitmap;
    std::shared_ptr<std::vector<MiniGdi::Color>> pixels;
    int width = 0;
    int height = 0;
    std::uint64_t epoch = 0;
    std::uint64_t frameSerial = 0;
    std::uint64_t queuedTicket = 0;
    std::uint64_t nextTicket = 0;
    bool active = false;
    bool presentQueued = false;
};

struct GuestInputCallbackState final
{
    // Event delegates can outlive GuestWindowManager when CoreWindow
    // unsubscription must be deferred to the UI dispatcher. Holding this
    // lock through the small input-routing operation makes detachment and
    // destruction mutually exclusive with a callback using the raw owner.
    std::mutex lock;
    GuestWindowManager* owner = nullptr;
};

struct GuestTimerKey final
{
    ULONG_PTR window = 0;
    UINT_PTR timerId = 0;

    bool operator==(const GuestTimerKey& other) const
    {
        return window == other.window && timerId == other.timerId;
    }
};

struct GuestTimerKeyHash final
{
    size_t operator()(const GuestTimerKey& key) const
    {
        const size_t windowHash = std::hash<ULONG_PTR>{}(key.window);
        const size_t timerHash = std::hash<UINT_PTR>{}(key.timerId);
        return windowHash ^ (timerHash + static_cast<size_t>(0x9e3779b9u) +
            (windowHash << 6) + (windowHash >> 2));
    }
};

struct GuestTimerEntry final
{
    HWND window = nullptr;
    UINT_PTR timerId = 0;
    std::uint64_t generation = 0;
    ThreadPoolTimer^ timer = nullptr;
};

struct GuestTimerState final
{
    mutable std::mutex lock;
    GuestWindowManager* owner = nullptr;
    std::unordered_map<GuestTimerKey, GuestTimerEntry, GuestTimerKeyHash> entries;
    UINT_PTR nextGeneratedId = 1;
    std::uint64_t nextGeneration = 0;
    bool active = false;

    // The UWP ThreadPoolTimer delegate captures a weak state object rather
    // than GuestWindowManager. Holding this lock across the short queue post
    // makes teardown mutually exclusive with the last in-flight callback.
    void Deliver(const GuestTimerKey& key, std::uint64_t generation)
    {
        std::lock_guard<std::mutex> guard(lock);
        if (!active || !owner)
        {
            return;
        }

        const auto found = entries.find(key);
        if (found == entries.end() || found->second.generation != generation)
        {
            return;
        }

        owner->PostGuestTimerMessage(found->second.window, found->second.timerId);
    }
};
}
}

namespace
{
    constexpr size_t MaximumGuestTimers = 64;

    GuestTimerKey MakeGuestTimerKey(HWND window, UINT_PTR timerId)
    {
        GuestTimerKey key = {};
        key.window = reinterpret_cast<ULONG_PTR>(window);
        key.timerId = timerId;
        return key;
    }

    UINT ClampGuestTimerPeriod(UINT milliseconds)
    {
        if (milliseconds < GuestAbi::UserTimerMinimum)
        {
            return GuestAbi::UserTimerMinimum;
        }
        return (std::min)(milliseconds, GuestAbi::UserTimerMaximum);
    }

    UINT_PTR AllocateGuestTimerIdLocked(GuestTimerState& state, HWND window)
    {
        // The table is bounded, so at most MaximumGuestTimers + 1 probes are
        // needed before a free, nonzero timer ID is found for this HWND.
        for (size_t attempt = 0; attempt <= MaximumGuestTimers; ++attempt)
        {
            UINT_PTR candidate = state.nextGeneratedId++;
            if (candidate == 0)
            {
                candidate = state.nextGeneratedId++;
            }
            if (candidate == 0)
            {
                continue;
            }

            if (state.entries.find(MakeGuestTimerKey(window, candidate)) == state.entries.end())
            {
                return candidate;
            }
        }
        return 0;
    }

    void CancelThreadPoolTimer(ThreadPoolTimer^ timer)
    {
        if (!timer)
        {
            return;
        }
        try
        {
            timer->Cancel();
        }
        catch (Exception^ error)
        {
            RuntimeDiagnostics::Record(
                L"TIMER EXCEPTION: cancellation failed; HRESULT " +
                std::to_wstring(static_cast<unsigned long>(error->HResult)) + L".");
        }
        catch (...)
        {
            RuntimeDiagnostics::Record(L"TIMER EXCEPTION: cancellation raised an unknown exception.");
            // Cancellation is best-effort. The state entry was already
            // removed, so a delayed UWP callback can no longer post work.
        }
    }

    using CancelledTimerList = std::array<ThreadPoolTimer^, MaximumGuestTimers>;

    void CancelThreadPoolTimers(const CancelledTimerList& timers, size_t count)
    {
        for (size_t index = 0; index < count; ++index)
        {
            CancelThreadPoolTimer(timers[index]);
        }
    }

    void QueuePresentation(
        const std::shared_ptr<GuestPresentationState>& presentation,
        std::uint64_t ticket);

    void ProcessPresentation(
        const std::shared_ptr<GuestPresentationState>& presentation,
        std::uint64_t ticket)
    {
        std::shared_ptr<std::vector<MiniGdi::Color>> pixels;
        int width = 0;
        int height = 0;
        std::uint64_t serial = 0;
        {
            std::lock_guard<std::mutex> guard(presentation->lock);
            if (!presentation->active || !presentation->presentQueued ||
                presentation->queuedTicket != ticket)
            {
                return;
            }

            pixels = presentation->pixels;
            width = presentation->width;
            height = presentation->height;
            serial = presentation->frameSerial;
        }

        bool frameUploaded = false;
        std::wstring uploadFailure;
        if (pixels && width > 0 && height > 0 && !pixels->empty())
        {
            try
            {
                Image^ image = nullptr;
                WriteableBitmap^ bitmap = nullptr;
                bool currentFrame = false;
                {
                    // Snapshot the agile XAML references, but never hold this
                    // mutex while invoking COM or copying pixels. A native
                    // fault at that boundary must not abandon a locked mutex.
                    std::lock_guard<std::mutex> guard(presentation->lock);
                    currentFrame = presentation->active && presentation->presentQueued &&
                        presentation->queuedTicket == ticket;
                }
                if (currentFrame)
                {
                    image = presentation->image.Get();
                    bitmap = presentation->bitmap.Get();
                }
                if (image)
                {
                    if (!bitmap || bitmap->PixelWidth != width || bitmap->PixelHeight != height)
                    {
                        bitmap = ref new WriteableBitmap(width, height);
                        image->Source = bitmap;
                        Platform::Agile<WriteableBitmap^> agileBitmap(bitmap);
                        std::lock_guard<std::mutex> guard(presentation->lock);
                        if (presentation->active && presentation->presentQueued &&
                            presentation->queuedTicket == ticket)
                        {
                            presentation->bitmap = agileBitmap;
                        }
                    }

                    IBuffer^ pixelBuffer = bitmap->PixelBuffer;
                    const size_t byteCount = pixels->size() * sizeof(MiniGdi::Color);
                    ComPtr<IBufferByteAccess> access;
                    const HRESULT result = pixelBuffer
                        ? reinterpret_cast<IInspectable*>(pixelBuffer)->QueryInterface(IID_PPV_ARGS(&access))
                        : E_POINTER;
                    if (SUCCEEDED(result) && pixelBuffer->Length >= byteCount)
                    {
                        DWORD nativeException = ERROR_SUCCESS;
                        if (SUCCEEDED(CopyPixelsToBuffer(
                            access.Get(), pixels->data(), byteCount, &nativeException)))
                        {
                            bitmap->Invalidate();
                            frameUploaded = true;
                        }
                        else
                        {
                            uploadFailure = nativeException == ERROR_SUCCESS
                                ? L"the bitmap pixel buffer was unavailable"
                                : L"the bitmap pixel copy raised native exception code " +
                                    std::to_wstring(static_cast<unsigned long>(nativeException));
                        }
                    }
                    else
                    {
                        uploadFailure = L"the bitmap pixel-buffer query failed";
                    }
                }
                else uploadFailure = L"the XAML image was unavailable";
            }
            catch (Exception^ error)
            {
                // A failed frame must not unwind into the guest. A later paint
                // gets a fresh opportunity to present.
                uploadFailure = L"the XAML bitmap update raised HRESULT " +
                    std::to_wstring(static_cast<unsigned long>(error->HResult));
                RuntimeDiagnostics::Record(L"FRAME EXCEPTION: " + uploadFailure + L".");
            }
            catch (...)
            {
                // A failed frame must not unwind into the guest. A later paint
                // gets a fresh opportunity to present.
                uploadFailure = L"an unknown exception occurred while updating the XAML bitmap";
                RuntimeDiagnostics::Record(L"FRAME EXCEPTION: " + uploadFailure + L".");
            }
        }
        else
        {
            uploadFailure = L"the frame had no pixels";
        }
        if (frameUploaded)
        {
            RuntimeDiagnostics::Record(L"FRAME UPLOAD: WriteableBitmap updated.");
        }
        else
        {
            RuntimeDiagnostics::Record(L"FRAME UPLOAD FAILED: " + uploadFailure + L".");
        }

        bool queueAnother = false;
        std::uint64_t nextTicket = 0;
        {
            std::lock_guard<std::mutex> guard(presentation->lock);
            if (!presentation->active || !presentation->presentQueued ||
                presentation->queuedTicket != ticket)
            {
                return;
            }

            if (presentation->frameSerial != serial)
            {
                nextTicket = ++presentation->nextTicket;
                presentation->queuedTicket = nextTicket;
                queueAnother = true;
            }
            else
            {
                presentation->presentQueued = false;
            }
        }
        if (queueAnother)
        {
            QueuePresentation(presentation, nextTicket);
        }
    }

    struct PresentationCall final
    {
        const std::shared_ptr<GuestPresentationState>* presentation = nullptr;
        std::uint64_t ticket = 0;
    };

    void ProcessPresentationThunk(void* raw)
    {
        const auto* call = static_cast<const PresentationCall*>(raw);
        if (call && call->presentation)
            ProcessPresentation(*call->presentation, call->ticket);
    }

    DWORD ProcessPresentationProtected(
        const std::shared_ptr<GuestPresentationState>& presentation,
        std::uint64_t ticket)
    {
        PresentationCall call{ &presentation, ticket };
        return InvokeSehProtected(&ProcessPresentationThunk, &call);
    }

    void QueuePresentation(
        const std::shared_ptr<GuestPresentationState>& presentation,
        std::uint64_t ticket)
    {
        bool queued = false;
        try
        {
            CoreDispatcher^ dispatcher = presentation->dispatcher.Get();
            if (dispatcher)
            {
                dispatcher->RunAsync(CoreDispatcherPriority::Normal,
                    ref new DispatchedHandler([presentation, ticket]()
                {
                    const DWORD exception = ProcessPresentationProtected(presentation, ticket);
                    if (exception != ERROR_SUCCESS)
                    {
                        RuntimeDiagnostics::Record(
                            L"FRAME SEH: presentation raised native exception code " +
                            std::to_wstring(static_cast<unsigned long>(exception)) + L".");
                        // Never wait here: if the fault happened in an
                        // unforeseen locked section, blocking would turn a
                        // contained rendering failure into a UI deadlock.
                        if (presentation->lock.try_lock())
                        {
                            if (presentation->presentQueued &&
                                presentation->queuedTicket == ticket)
                                presentation->presentQueued = false;
                            presentation->lock.unlock();
                        }
                    }
                }));
                queued = true;
            }
        }
        catch (Exception^ error)
        {
            RuntimeDiagnostics::Record(
                L"FRAME EXCEPTION: dispatcher queue failed; HRESULT " +
                std::to_wstring(static_cast<unsigned long>(error->HResult)) + L".");
        }
        catch (...)
        {
            RuntimeDiagnostics::Record(L"FRAME EXCEPTION: dispatcher queue raised an unknown exception.");
        }

        if (!queued)
        {
            std::lock_guard<std::mutex> guard(presentation->lock);
            if (presentation->presentQueued && presentation->queuedTicket == ticket)
            {
                presentation->presentQueued = false;
            }
        }
    }

    void ClearPresentation(
        const std::shared_ptr<GuestPresentationState>& presentation,
        std::uint64_t epoch)
    {
        if (!presentation)
        {
            return;
        }

        try
        {
            CoreDispatcher^ dispatcher = presentation->dispatcher.Get();
            if (!dispatcher)
            {
                return;
            }
            dispatcher->RunAsync(CoreDispatcherPriority::Normal,
                ref new DispatchedHandler([presentation, epoch]()
            {
                try
                {
                    std::lock_guard<std::mutex> guard(presentation->lock);
                    if (presentation->epoch != epoch)
                    {
                        return;
                    }
                    Image^ image = presentation->image.Get();
                    if (image)
                    {
                        image->Source = nullptr;
                    }
                    presentation->bitmap = Platform::Agile<WriteableBitmap^>();
                }
                catch (...)
                {
                }
            }));
        }
        catch (...)
        {
        }
    }
}

struct GuestWindowManager::WindowClass final
{
    DWORD processId = 0;
    std::wstring name;
    // WNDCLASS owns the menu-resource association for a top-level window.
    // Keep either the resource name or its MAKEINTRESOURCE value so creating
    // the window can materialize the virtual USER32 menu later.
    std::wstring menuResourceName;
    ULONG_PTR menuResourceAtom = 0;
    GuestAbi::WndProc procedure = nullptr;
    HBRUSH background = nullptr;
    UINT style = 0;
    int windowExtraBytes = 0;
    ATOM atom = 0;
    BuiltinControlKind builtinKind = BuiltinControlKind::None;
};

struct GuestWindowManager::WindowRecord final
{
    HWND handle = nullptr;
    DWORD processId = 0;
    std::shared_ptr<WindowClass> windowClass;
    GuestAbi::WndProc procedure = nullptr;
    std::wstring title;
    RECT bounds = {};
    DWORD style = 0;
    DWORD extendedStyle = 0;
    LONG_PTR userData = 0;
    HINSTANCE instance = nullptr;
    HWND parent = nullptr;
    UINT_PTR controlId = 0;
    std::vector<BYTE> extraBytes;
    MiniGdi::ObjectHandle controlFont = MiniGdi::InvalidObject;
    bool visualStyles = false;
    HICON staticIcon = nullptr;
    MiniGdi::Surface staticImage;
    int controlTextWidth = GuestMetrics::TextWidth;
    int controlTextHeight = GuestMetrics::TextHeight;
    int listViewHeaderHeight = GuestMetrics::ListViewHeaderHeight;
    int listViewRowHeight = GuestMetrics::TextHeight;
    size_t editCaret = 0;
    size_t editSelectionAnchor = 0;
    size_t editSelectionEnd = 0;
    size_t editTextLimit = MaximumBuiltinControlTextLength;
    size_t editFirstVisibleCharacter = 0;
    size_t editFirstVisibleLine = 0;
    int editHorizontalOffset = 0;
    wchar_t editPasswordCharacter = L'\0';
    HLOCAL editTextHandle = nullptr;
    bool buttonPressed = false;
    bool buttonKeyboardPressed = false;
    std::vector<std::wstring> choiceItems;
    HANDLE comboBoxExImageList = nullptr;
    int selectedChoice = -1;
    bool comboDropped = false;
    int comboDroppedWidth = 0;
    int scrollPosition = 0;
    std::vector<std::wstring> listViewColumns;
    std::vector<int> listViewColumnWidths;
    std::vector<int> listViewColumnFormats;
    std::vector<int> listViewColumnSubItems;
    std::vector<int> listViewColumnImages;
    std::vector<int> listViewColumnOrders;
    // Each inner element is one report-view row.  Keeping subitems here is
    // important: file managers populate the name first and then fill size,
    // type and timestamp through LVM_SETITEMTEXTW.
    std::vector<std::vector<std::wstring>> listViewItems;
    std::vector<LPARAM> listViewItemData;
    std::vector<UINT> listViewItemStates;
    std::vector<int> listViewItemImages;
    HANDLE listViewImageLists[3] = {};
    COLORREF listViewBackgroundColor = 0x00ffffff;
    COLORREF listViewTextColor = 0x00000000;
    COLORREF listViewTextBackgroundColor = 0x00ffffff;
    UINT listViewExtendedStyle = 0;
    UINT listViewCallbackMask = 0;
    int listViewSelectedItem = -1;
    int listViewSelectionMark = -1;
    int listViewTopItem = 0;
    int listViewPressedItem = -1;
    int listViewEditItem = -1;
    std::wstring listViewEditText;
    size_t listViewEditCaret = 0;
    std::chrono::steady_clock::time_point listViewLastClick = {};
    int listViewLastClickItem = -1;
    std::vector<int> toolbarCommands;
    std::vector<int> toolbarBitmaps;
    std::vector<BYTE> toolbarButtonStates;
    std::vector<BYTE> toolbarButtonStyles;
    std::vector<UINT_PTR> toolbarButtonData;
    std::vector<std::wstring> toolbarStrings;
    std::vector<std::wstring> toolbarButtonTexts;
    std::vector<INT_PTR> toolbarButtonStringRefs;
    int toolbarButtonWidth = GuestMetrics::ToolbarButtonExtent;
    int toolbarButtonHeight = GuestMetrics::ToolbarButtonExtent;
    int toolbarBitmapWidth = GuestMetrics::DefaultBitmapExtent;
    int toolbarBitmapHeight = GuestMetrics::DefaultBitmapExtent;
    int toolbarPressedIndex = -1;
    HANDLE toolbarImageList = nullptr;
    bool commonControlUnicode = true;
    std::vector<RebarBand> rebarBands;
    std::vector<int> statusBarParts;
    std::vector<std::wstring> statusBarTexts;
    std::vector<UINT> statusBarTextStyles;
    std::wstring statusBarSimpleText;
    UINT statusBarSimpleStyle = 0;
    int statusBarMinimumHeight = GuestMetrics::StatusBarMinimumHeight;
    bool statusBarSimple = false;
    int progressMinimum = 0;
    int progressMaximum = 100;
    int progressPosition = 0;
    int progressStep = 10;
    UINT progressState = 1;
    COLORREF progressBarColor = 0xffffffffu;
    COLORREF progressBackgroundColor = 0xffffffffu;
    bool progressMarquee = false;
    std::vector<std::wstring> tabItems;
    std::vector<LPARAM> tabItemData;
    std::vector<int> tabItemImages;
    std::vector<UINT> tabItemStates;
    HANDLE tabImageList = nullptr;
    int tabSelectedItem = -1;
    int tabFocusedItem = -1;
    int tabItemWidth = 0;
    int tabItemHeight = GuestMetrics::TabItemHeight;
    int tabHorizontalPadding = 6;
    int tabVerticalPadding = 3;
    std::vector<std::wstring> headerItems;
    std::vector<int> headerItemWidths;
    std::vector<int> headerItemFormats;
    std::vector<LPARAM> headerItemData;
    std::vector<int> headerItemImages;
    std::vector<int> headerItemOrders;
    HANDLE headerImageList = nullptr;
    int headerPressedItem = -1;
    std::vector<TreeNode> treeNodes;
    ULONG_PTR nextTreeToken = 0x100000;
    ULONG_PTR treeSelectedItem = 0;
    ULONG_PTR treeTopItem = 0;
    HANDLE treeImageLists[2] = {};
    int treeIndent = 16;
    int treeItemHeight = 18;
    COLORREF treeBackgroundColor = 0x00ffffff;
    COLORREF treeTextColor = 0x00000000;
    int upDownMinimum = 0;
    int upDownMaximum = 100;
    int upDownPosition = 0;
    UINT upDownBase = 10;
    HWND upDownBuddy = nullptr;
    bool menuBar = false;
    bool viewportWidthBound = false;
    bool viewportHeightBound = false;
    int openMenuIndex = -1;
    bool visible = false;
    bool enabled = true;
    bool invalidated = false;
    bool paintPosted = false;
    bool erasePending = false;
    bool paintActive = false;
    RECT updateRect = {};
    bool destroyed = false;
    MiniGdi::Surface surface;
    MiniGdi::DcHandle dc = MiniGdi::InvalidDc;
    mutable std::mutex lock;
};

GuestWindowManager::GuestWindowManager(CoreWindow^ coreWindow, Panel^ surfaceHost)
    : m_coreWindow(coreWindow),
      m_dispatcher(coreWindow ? coreWindow->Dispatcher : nullptr),
      m_timers(std::make_shared<GuestTimerState>())
{
    if (!coreWindow || !surfaceHost)
    {
        return;
    }

    const auto hostBounds = coreWindow->Bounds;
    const double availableWidth = surfaceHost->ActualWidth > 0.0
        ? surfaceHost->ActualWidth : hostBounds.Width;
    const double availableHeight = surfaceHost->ActualHeight > 0.0
        ? surfaceHost->ActualHeight : hostBounds.Height;
    const int initialWidth = (std::max)(1, static_cast<int>(std::lround(availableWidth)));
    const int initialHeight = (std::max)(1, static_cast<int>(std::lround(availableHeight)));
    m_viewportWidth.store(initialWidth);
    m_viewportHeight.store(initialHeight);
    GuestMetrics::SetCurrentScreenSize(initialWidth, initialHeight);

    auto image = ref new Image();
    image->Stretch = Stretch::Uniform;
    image->HorizontalAlignment = HorizontalAlignment::Stretch;
    image->VerticalAlignment = VerticalAlignment::Stretch;
    surfaceHost->Children->Append(image);
    m_surfaceHost = Platform::Agile<Panel^>(surfaceHost);
    m_surfaceImage = Platform::Agile<Image^>(image);
    m_presentation = std::make_shared<GuestPresentationState>();
    m_presentation->dispatcher = Platform::Agile<CoreDispatcher^>(coreWindow->Dispatcher);
    m_presentation->image = Platform::Agile<Image^>(image);
    m_inputCallbacks = std::make_shared<GuestInputCallbackState>();
    m_inputCallbacks->owner = this;
    const auto callbacks = m_inputCallbacks;

    m_pointerMovedToken = coreWindow->PointerMoved += ref new TypedEventHandler<CoreWindow^, PointerEventArgs^>(
        [callbacks](CoreWindow^, PointerEventArgs^ args)
    {
        try
        {
            std::lock_guard<std::mutex> guard(callbacks->lock);
            if (callbacks->owner)
            {
                const DWORD exceptionCode = GuestWindowManager::InvokePointerInput(callbacks->owner, args, GuestAbi::WmMouseMove);
                if (exceptionCode != ERROR_SUCCESS)
                    RuntimeDiagnostics::Record(L"HOST INPUT SEH: PointerMoved code " + std::to_wstring(static_cast<unsigned long>(exceptionCode)) + L".");
            }
        }
        catch (Exception^ error) { RuntimeDiagnostics::Record(L"HOST INPUT EXCEPTION: PointerMoved HRESULT " + std::to_wstring(static_cast<unsigned long>(error->HResult)) + L"."); }
        catch (...) { RuntimeDiagnostics::Record(L"HOST INPUT EXCEPTION: PointerMoved raised an unknown exception."); }
    });
    m_pointerPressedToken = coreWindow->PointerPressed += ref new TypedEventHandler<CoreWindow^, PointerEventArgs^>(
        [callbacks](CoreWindow^, PointerEventArgs^ args)
    {
        try
        {
            std::lock_guard<std::mutex> guard(callbacks->lock);
            if (callbacks->owner)
            {
                const DWORD exceptionCode = GuestWindowManager::InvokePointerInput(callbacks->owner, args, 0);
                if (exceptionCode != ERROR_SUCCESS)
                    RuntimeDiagnostics::Record(L"HOST INPUT SEH: PointerPressed code " + std::to_wstring(static_cast<unsigned long>(exceptionCode)) + L".");
            }
        }
        catch (Exception^ error) { RuntimeDiagnostics::Record(L"HOST INPUT EXCEPTION: PointerPressed HRESULT " + std::to_wstring(static_cast<unsigned long>(error->HResult)) + L"."); }
        catch (...) { RuntimeDiagnostics::Record(L"HOST INPUT EXCEPTION: PointerPressed raised an unknown exception."); }
    });
    m_pointerReleasedToken = coreWindow->PointerReleased += ref new TypedEventHandler<CoreWindow^, PointerEventArgs^>(
        [callbacks](CoreWindow^, PointerEventArgs^ args)
    {
        try
        {
            std::lock_guard<std::mutex> guard(callbacks->lock);
            if (callbacks->owner)
            {
                const DWORD exceptionCode = GuestWindowManager::InvokePointerInput(callbacks->owner, args, 0);
                if (exceptionCode != ERROR_SUCCESS)
                    RuntimeDiagnostics::Record(L"HOST INPUT SEH: PointerReleased code " + std::to_wstring(static_cast<unsigned long>(exceptionCode)) + L".");
            }
        }
        catch (Exception^ error) { RuntimeDiagnostics::Record(L"HOST INPUT EXCEPTION: PointerReleased HRESULT " + std::to_wstring(static_cast<unsigned long>(error->HResult)) + L"."); }
        catch (...) { RuntimeDiagnostics::Record(L"HOST INPUT EXCEPTION: PointerReleased raised an unknown exception."); }
    });
    m_pointerWheelToken = coreWindow->PointerWheelChanged += ref new TypedEventHandler<CoreWindow^, PointerEventArgs^>(
        [callbacks](CoreWindow^, PointerEventArgs^ args)
    {
        try
        {
            std::lock_guard<std::mutex> guard(callbacks->lock);
            if (callbacks->owner)
            {
                const DWORD exceptionCode = GuestWindowManager::InvokeWheelInput(callbacks->owner, args);
                if (exceptionCode != ERROR_SUCCESS)
                    RuntimeDiagnostics::Record(L"HOST INPUT SEH: PointerWheelChanged code " + std::to_wstring(static_cast<unsigned long>(exceptionCode)) + L".");
            }
        }
        catch (Exception^ error) { RuntimeDiagnostics::Record(L"HOST INPUT EXCEPTION: PointerWheelChanged HRESULT " + std::to_wstring(static_cast<unsigned long>(error->HResult)) + L"."); }
        catch (...) { RuntimeDiagnostics::Record(L"HOST INPUT EXCEPTION: PointerWheelChanged raised an unknown exception."); }
    });
    m_keyDownToken = coreWindow->KeyDown += ref new TypedEventHandler<CoreWindow^, KeyEventArgs^>(
        [callbacks](CoreWindow^, KeyEventArgs^ args)
    {
        try
        {
            std::lock_guard<std::mutex> guard(callbacks->lock);
            if (callbacks->owner)
            {
                const DWORD exceptionCode = GuestWindowManager::InvokeKeyInput(callbacks->owner, args, GuestAbi::WmKeyDown);
                if (exceptionCode != ERROR_SUCCESS)
                    RuntimeDiagnostics::Record(L"HOST INPUT SEH: KeyDown code " + std::to_wstring(static_cast<unsigned long>(exceptionCode)) + L".");
            }
        }
        catch (Exception^ error) { RuntimeDiagnostics::Record(L"HOST INPUT EXCEPTION: KeyDown HRESULT " + std::to_wstring(static_cast<unsigned long>(error->HResult)) + L"."); }
        catch (...) { RuntimeDiagnostics::Record(L"HOST INPUT EXCEPTION: KeyDown raised an unknown exception."); }
    });
    m_keyUpToken = coreWindow->KeyUp += ref new TypedEventHandler<CoreWindow^, KeyEventArgs^>(
        [callbacks](CoreWindow^, KeyEventArgs^ args)
    {
        try
        {
            std::lock_guard<std::mutex> guard(callbacks->lock);
            if (callbacks->owner)
            {
                const DWORD exceptionCode = GuestWindowManager::InvokeKeyInput(callbacks->owner, args, GuestAbi::WmKeyUp);
                if (exceptionCode != ERROR_SUCCESS)
                    RuntimeDiagnostics::Record(L"HOST INPUT SEH: KeyUp code " + std::to_wstring(static_cast<unsigned long>(exceptionCode)) + L".");
            }
        }
        catch (Exception^ error) { RuntimeDiagnostics::Record(L"HOST INPUT EXCEPTION: KeyUp HRESULT " + std::to_wstring(static_cast<unsigned long>(error->HResult)) + L"."); }
        catch (...) { RuntimeDiagnostics::Record(L"HOST INPUT EXCEPTION: KeyUp raised an unknown exception."); }
    });
    m_characterReceivedToken = coreWindow->CharacterReceived +=
        ref new TypedEventHandler<CoreWindow^, CharacterReceivedEventArgs^>(
        [callbacks](CoreWindow^, CharacterReceivedEventArgs^ args)
    {
        try
        {
            std::lock_guard<std::mutex> guard(callbacks->lock);
            if (callbacks->owner)
            {
                const DWORD exceptionCode =
                    GuestWindowManager::InvokeCharacterInput(callbacks->owner, args);
                if (exceptionCode != ERROR_SUCCESS)
                    RuntimeDiagnostics::Record(L"HOST INPUT SEH: CharacterReceived code " +
                        std::to_wstring(static_cast<unsigned long>(exceptionCode)) + L".");
            }
        }
        catch (Exception^ error)
        {
            RuntimeDiagnostics::Record(L"HOST INPUT EXCEPTION: CharacterReceived HRESULT " +
                std::to_wstring(static_cast<unsigned long>(error->HResult)) + L".");
        }
        catch (...)
        {
            RuntimeDiagnostics::Record(L"HOST INPUT EXCEPTION: CharacterReceived raised an unknown exception.");
        }
    });
    m_sizeChangedToken = coreWindow->SizeChanged +=
        ref new TypedEventHandler<CoreWindow^, WindowSizeChangedEventArgs^>(
        [callbacks](CoreWindow^, WindowSizeChangedEventArgs^ args)
    {
        try
        {
            std::lock_guard<std::mutex> guard(callbacks->lock);
            if (callbacks->owner && args)
            {
                callbacks->owner->HandleHostSizeChanged(
                    (std::max)(1, static_cast<int>(std::lround(args->Size.Width))),
                    (std::max)(1, static_cast<int>(std::lround(args->Size.Height))));
            }
        }
        catch (Exception^ error)
        {
            RuntimeDiagnostics::Record(L"HOST SIZE EXCEPTION: HRESULT " +
                std::to_wstring(static_cast<unsigned long>(error->HResult)) + L".");
        }
        catch (...)
        {
            RuntimeDiagnostics::Record(L"HOST SIZE EXCEPTION: unknown exception.");
        }
    });
    m_surfaceSizeChangedToken = surfaceHost->SizeChanged += ref new SizeChangedEventHandler(
        [callbacks](Platform::Object^, SizeChangedEventArgs^ args)
    {
        try
        {
            std::lock_guard<std::mutex> guard(callbacks->lock);
            if (callbacks->owner && args)
            {
                callbacks->owner->HandleHostSizeChanged(
                    (std::max)(1, static_cast<int>(std::lround(args->NewSize.Width))),
                    (std::max)(1, static_cast<int>(std::lround(args->NewSize.Height))));
            }
        }
        catch (...)
        {
            RuntimeDiagnostics::Record(L"HOST SURFACE SIZE EXCEPTION.");
        }
    });
    m_eventsAttached.store(true);
}

GuestWindowManager::~GuestWindowManager()
{
    Deactivate();
    DetachHostEvents();
}

void GuestWindowManager::Activate()
{
    if (m_active.exchange(true))
    {
        return;
    }

    m_messages.Reset();
    m_focusWindow.store(0);
    const auto timers = m_timers;
    if (timers)
    {
        std::lock_guard<std::mutex> guard(timers->lock);
        timers->owner = this;
        timers->active = true;
    }
    const auto presentation = m_presentation;
    if (presentation)
    {
        std::lock_guard<std::mutex> guard(presentation->lock);
        presentation->active = true;
        ++presentation->epoch;
        presentation->pixels.reset();
        presentation->width = 0;
        presentation->height = 0;
        presentation->presentQueued = false;
        presentation->queuedTicket = ++presentation->nextTicket;
    }
}

void GuestWindowManager::Deactivate()
{
    m_active.store(false);
    MouseInput().ResetKeyState();
    {
        std::lock_guard<std::mutex> guard(m_popupMenuLock);
        m_popupMenu.open = false;
    }
    m_popupMenuChanged.notify_all();
    m_foregroundWindow.store(0);
    m_focusWindow.store(0);
    m_captureWindow.store(0);
    StopGuestTimers();
    m_messages.Close();
    m_messages.Clear();

    // A guest may return without explicitly destroying every window.  Those
    // HWNDs and HDCs are guest-lifetime resources, so tear them down before a
    // later PE run can reuse this manager.  Do not invoke a guest WNDPROC here:
    // cleanup must remain safe even after a malformed guest exits abruptly.
    std::vector<std::shared_ptr<WindowRecord>> abandoned;
    {
        std::lock_guard<std::mutex> guard(m_windowsLock);
        for (const auto& item : m_windows)
        {
            abandoned.push_back(item.second);
        }
        m_windows.clear();
    }
    for (const auto& window : abandoned)
    {
        MiniGdi::DcHandle dc = MiniGdi::InvalidDc;
        {
            std::lock_guard<std::mutex> guard(window->lock);
            window->destroyed = true;
            dc = window->dc;
            window->dc = MiniGdi::InvalidDc;
        }
        if (dc != MiniGdi::InvalidDc)
        {
            m_gdi.DestroyDc(dc);
        }
    }

    // The explicit window-DC destruction above preserves the established
    // teardown order.  A guest can also leave memory DCs, brushes, pens, and
    // bitmaps behind, however; they are scoped to the same guest lifetime.
    // Reset only after every WindowRecord has been made unreachable, so no
    // active guest callback can observe invalidated handles mid-execution.
    m_gdi.Reset();
    {
        std::lock_guard<std::mutex> guard(m_screenDcLock);
        m_screenDc = MiniGdi::InvalidDc;
        m_screenSurface = MiniGdi::Surface();
    }

    {
        std::lock_guard<std::mutex> guard(m_classesLock);
        m_classes.clear();
        m_classesByAtom.clear();
        m_nextAtom = 0xC000;
    }

    const auto presentation = m_presentation;
    if (presentation)
    {
        std::uint64_t presentationEpoch = 0;
        {
            std::lock_guard<std::mutex> guard(presentation->lock);
            presentation->active = false;
            presentationEpoch = ++presentation->epoch;
            presentation->pixels.reset();
            presentation->width = 0;
            presentation->height = 0;
            presentation->presentQueued = false;
            presentation->queuedTicket = ++presentation->nextTicket;
        }
        ClearPresentation(presentation, presentationEpoch);
    }
}

ATOM GuestWindowManager::RegisterGuestClass(const GuestAbi::WndClassExW* windowClass, DWORD* win32Error)
{
    if (!windowClass || windowClass->cbSize < sizeof(GuestAbi::WndClassExW) || !windowClass->lpszClassName ||
        !windowClass->lpfnWndProc || windowClass->cbClsExtra < 0 || windowClass->cbWndExtra < 0 ||
        windowClass->cbWndExtra > MaximumWindowExtraBytes)
    {
        SetWin32Error(win32Error, ERROR_INVALID_PARAMETER);
        return 0;
    }

    const DWORD processId = BridgeGetCurrentProcessId();
    const std::wstring name = Lowercase(windowClass->lpszClassName);
    if (name.empty())
    {
        SetWin32Error(win32Error, ERROR_INVALID_NAME);
        return 0;
    }

    std::lock_guard<std::mutex> guard(m_classesLock);
    const std::wstring processClassName = std::to_wstring(processId) + L":" + name;
    if (m_classes.find(processClassName) != m_classes.end())
    {
        SetWin32Error(win32Error, ERROR_CLASS_ALREADY_EXISTS);
        return 0;
    }

    auto registered = std::make_shared<WindowClass>();
    registered->processId = processId;
    registered->name = name;
    if (windowClass->lpszMenuName)
    {
        if (IsAtomPointer(windowClass->lpszMenuName))
        {
            registered->menuResourceAtom = reinterpret_cast<ULONG_PTR>(windowClass->lpszMenuName);
        }
        else
        {
            registered->menuResourceName = windowClass->lpszMenuName;
        }
    }
    registered->procedure = windowClass->lpfnWndProc;
    registered->background = windowClass->hbrBackground;
    registered->style = windowClass->style;
    registered->windowExtraBytes = windowClass->cbWndExtra;
    registered->atom = m_nextAtom++;
    if (registered->atom == 0)
    {
        registered->atom = m_nextAtom++;
    }
    m_classes.emplace(processClassName, registered);
    m_classesByAtom.emplace(registered->atom, registered);
    SetWin32Error(win32Error, ERROR_SUCCESS);
    return registered->atom;
}

ATOM GuestWindowManager::RegisterGuestClass(const GuestAbi::WndClassW* windowClass, DWORD* win32Error)
{
    if (!windowClass)
    {
        SetWin32Error(win32Error, ERROR_INVALID_PARAMETER);
        return 0;
    }

    GuestAbi::WndClassExW expanded = {};
    expanded.cbSize = sizeof(expanded);
    expanded.style = windowClass->style;
    expanded.lpfnWndProc = windowClass->lpfnWndProc;
    expanded.cbClsExtra = windowClass->cbClsExtra;
    expanded.cbWndExtra = windowClass->cbWndExtra;
    expanded.hInstance = windowClass->hInstance;
    expanded.hIcon = windowClass->hIcon;
    expanded.hCursor = windowClass->hCursor;
    expanded.hbrBackground = windowClass->hbrBackground;
    expanded.lpszMenuName = windowClass->lpszMenuName;
    expanded.lpszClassName = windowClass->lpszClassName;
    return RegisterGuestClass(&expanded, win32Error);
}

BOOL GuestWindowManager::UnregisterGuestClass(LPCWSTR className, HINSTANCE, DWORD* win32Error)
{
    if (!className)
    {
        SetWin32Error(win32Error, ERROR_INVALID_PARAMETER);
        return FALSE;
    }
    std::shared_ptr<WindowClass> target;
    {
        std::lock_guard<std::mutex> guard(m_classesLock);
        if (IsAtomPointer(className))
        {
            const auto found = m_classesByAtom.find(static_cast<ATOM>(reinterpret_cast<ULONG_PTR>(className)));
            if (found != m_classesByAtom.end()) target = found->second;
        }
        else
        {
            const std::wstring key = std::to_wstring(BridgeGetCurrentProcessId()) + L":" + Lowercase(className);
            const auto found = m_classes.find(key);
            if (found != m_classes.end()) target = found->second;
        }
    }
    if (!target)
    {
        SetWin32Error(win32Error, ERROR_CLASS_DOES_NOT_EXIST);
        return FALSE;
    }
    {
        std::lock_guard<std::mutex> guard(m_windowsLock);
        for (const auto& item : m_windows)
        {
            std::lock_guard<std::mutex> windowGuard(item.second->lock);
            if (!item.second->destroyed && item.second->windowClass == target)
            {
                SetWin32Error(win32Error, ERROR_CLASS_HAS_WINDOWS);
                return FALSE;
            }
        }
    }
    {
        std::lock_guard<std::mutex> guard(m_classesLock);
        m_classesByAtom.erase(target->atom);
        m_classes.erase(std::to_wstring(target->processId) + L":" + target->name);
    }
    SetWin32Error(win32Error, ERROR_SUCCESS);
    return TRUE;
}

std::shared_ptr<GuestWindowManager::WindowClass> GuestWindowManager::FindClass(LPCWSTR className) const
{
    if (!className)
    {
        return nullptr;
    }

    std::lock_guard<std::mutex> guard(m_classesLock);
    const DWORD processId = BridgeGetCurrentProcessId();
    if (IsAtomPointer(className))
    {
        const auto found = m_classesByAtom.find(static_cast<ATOM>(reinterpret_cast<ULONG_PTR>(className)));
        return found == m_classesByAtom.end() ? nullptr : found->second;
    }

    const auto found = m_classes.find(
        std::to_wstring(processId) + L":" + Lowercase(className));
    return found == m_classes.end() ? nullptr : found->second;
}

HWND GuestWindowManager::CreateGuestWindow(
    DWORD extendedStyle,
    LPCWSTR className,
    LPCWSTR windowName,
    DWORD style,
    int x,
    int y,
    int width,
    int height,
    HWND parent,
    HMENU menu,
    HINSTANCE instance,
    LPVOID parameter,
    DWORD* win32Error)
{
    auto registered = FindClass(className);
    if (!registered)
    {
        const BuiltinControlKind builtinKind = BuiltinControlKindForClassName(className);
        if (builtinKind == BuiltinControlKind::None)
        {
            SetWin32Error(win32Error, ERROR_CANNOT_FIND_WND_CLASS);
            RuntimeDiagnostics::Record(
                L"WINDOW FAILED: unregistered class " +
                (className ? std::wstring(className) : L"<null>") + L".");
            return nullptr;
        }

        // Predefined classes are deliberately ephemeral rather than inserted
        // into the guest's RegisterClass map. A PE can therefore still
        // register an ordinary class of the same name for its own process,
        // while unregistered STATIC/BUTTON/EDIT windows retain predictable
        // bridge behavior.
        registered = std::make_shared<WindowClass>();
        registered->processId = BridgeGetCurrentProcessId();
        registered->name = Lowercase(className);
        registered->background = BuiltinControlBackground(builtinKind);
        registered->builtinKind = builtinKind;
    }
    if (parent && !IsGuestWindow(parent))
    {
        SetWin32Error(win32Error, ERROR_INVALID_WINDOW_HANDLE);
        return nullptr;
    }

    auto window = std::make_shared<WindowRecord>();
    window->processId = BridgeGetCurrentProcessId();
    window->windowClass = registered;
    window->procedure = registered->procedure;
    window->title = windowName ? windowName : L"";
    if (registered->builtinKind != BuiltinControlKind::None &&
        window->title.size() > MaximumBuiltinControlTextLength)
    {
        window->title.resize(MaximumBuiltinControlTextLength);
    }
    window->style = style;
    window->extendedStyle = extendedStyle;
    window->instance = instance;
    window->parent = parent;
    window->controlId = parent ? reinterpret_cast<UINT_PTR>(menu) : 0;
    // Comctl32 binds a control to the activation context in effect when the
    // window is created. Preserve that choice even if a nested context is
    // activated later while the control is being painted.
    window->visualStyles = CurrentGuestUsesVisualStyles();
    window->editCaret = window->title.size();
    window->editSelectionAnchor = window->editCaret;
    window->editSelectionEnd = window->editCaret;
    if (registered->builtinKind == BuiltinControlKind::Edit &&
        (style & GuestAbi::EsPassword) != 0)
    {
        window->editPasswordCharacter = L'\x25cf';
    }
    window->enabled = (style & GuestWsDisabled) == 0;
    if (registered->builtinKind == BuiltinControlKind::StatusBar)
    {
        window->statusBarParts.push_back(-1);
        window->statusBarTexts.push_back(window->title);
        window->statusBarTextStyles.push_back(0);
    }
    try
    {
        window->extraBytes.resize(static_cast<size_t>(registered->windowExtraBytes));
    }
    catch (const std::bad_alloc&)
    {
        SetWin32Error(win32Error, ERROR_NOT_ENOUGH_MEMORY);
        return nullptr;
    }
    // CW_USEDEFAULT has a different meaning for a stock control than for a
    // top-level desktop. Use the virtual desktop for top-level defaults and
    // control metrics for child strips before their parent performs layout.
    const BuiltinControlKind builtinKind = registered->builtinKind;
    const int defaultWidth = GuestMetrics::CurrentScreenWidth();
    int defaultHeight = GuestMetrics::CurrentScreenHeight();
    if (builtinKind == BuiltinControlKind::Toolbar ||
        builtinKind == BuiltinControlKind::Rebar)
        defaultHeight = GuestMetrics::ToolbarHeight;
    else if (builtinKind == BuiltinControlKind::StatusBar)
        defaultHeight = GuestMetrics::StatusBarHeight;
    else if (builtinKind == BuiltinControlKind::Header)
        defaultHeight = GuestMetrics::ListViewHeaderHeight;
    else if (builtinKind == BuiltinControlKind::Progress ||
        builtinKind == BuiltinControlKind::UpDown ||
        builtinKind == BuiltinControlKind::ToolTip)
        defaultHeight = GuestMetrics::ControlHeightForText(GuestMetrics::TextHeight);
    const bool defaultTopLevelX = !parent && x == GuestAbi::CwUseDefault;
    const bool defaultTopLevelY = !parent && y == GuestAbi::CwUseDefault;
    window->viewportWidthBound = !parent && width == GuestAbi::CwUseDefault;
    window->viewportHeightBound = !parent && height == GuestAbi::CwUseDefault;
    const int initialWindowWidth = DefaultExtent(width,
        parent ? defaultWidth : (std::max)(1, m_viewportWidth.load()));
    const int initialWindowHeight = DefaultExtent(height,
        parent ? defaultHeight : (std::max)(1, m_viewportHeight.load()));
    window->bounds.left = defaultTopLevelX && initialWindowWidth < m_viewportWidth.load()
        ? (m_viewportWidth.load() - initialWindowWidth) / 2
        : DefaultCoordinate(x);
    window->bounds.top = defaultTopLevelY && initialWindowHeight < m_viewportHeight.load()
        ? (m_viewportHeight.load() - initialWindowHeight) / 2
        : DefaultCoordinate(y);
    window->bounds.right = window->bounds.left + initialWindowWidth;
    window->bounds.bottom = window->bounds.top + initialWindowHeight;
    if (!window->surface.Resize(window->bounds.right - window->bounds.left, window->bounds.bottom - window->bounds.top, MiniGdi::OpaqueWhite))
    {
        SetWin32Error(win32Error, ERROR_NOT_ENOUGH_MEMORY);
        return nullptr;
    }
    window->dc = m_gdi.CreateDc(&window->surface);
    if (window->dc == MiniGdi::InvalidDc)
    {
        SetWin32Error(win32Error, ERROR_NOT_ENOUGH_MEMORY);
        return nullptr;
    }

    {
        std::lock_guard<std::mutex> guard(m_windowsLock);
        const ULONG_PTR token = m_nextWindow++;
        window->handle = reinterpret_cast<HWND>(token);
        m_windows.emplace(token, window);
    }

    // CreateWindowEx associates a top-level window with either the supplied
    // menu handle or the menu resource declared in its WNDCLASS.  The old
    // bridge retained neither relationship, so applications could create and
    // query menus but never receive a visible, interactive menu bar.
    if (!parent)
    {
        HMENU effectiveMenu = menu;
        if (!effectiveMenu)
        {
            LPCWSTR menuResource = nullptr;
            if (registered->menuResourceAtom)
            {
                menuResource = reinterpret_cast<LPCWSTR>(registered->menuResourceAtom);
            }
            else if (!registered->menuResourceName.empty())
            {
                menuResource = registered->menuResourceName.c_str();
            }
            if (menuResource)
            {
                effectiveMenu = BridgeLoadMenuW(instance, menuResource);
            }
        }
        if (effectiveMenu && BridgeSetMenu(window->handle, effectiveMenu))
        {
            RuntimeDiagnostics::Record(L"MENU: attached the class/top-level menu to guest window " +
                std::to_wstring(reinterpret_cast<ULONG_PTR>(window->handle)) + L".");
        }
    }

    GuestAbi::CreateStructW create = {};
    create.lpCreateParams = parameter;
    create.hInstance = instance;
    create.hMenu = menu;
    create.hwndParent = parent;
    create.cy = window->bounds.bottom - window->bounds.top;
    create.cx = window->bounds.right - window->bounds.left;
    create.y = window->bounds.top;
    create.x = window->bounds.left;
    create.style = static_cast<LONG>(style);
    create.lpszName = windowName;
    create.lpszClass = className;
    create.dwExStyle = extendedStyle;

    if (CallWindowProcedure(window, GuestAbi::WmNcCreate, 0, reinterpret_cast<LPARAM>(&create)) == FALSE ||
        CallWindowProcedure(window, GuestAbi::WmCreate, 0, reinterpret_cast<LPARAM>(&create)) == -1)
    {
        DWORD ignored = ERROR_SUCCESS;
        DestroyGuestWindow(window->handle, &ignored);
        SetWin32Error(win32Error, ERROR_NOT_ENOUGH_MEMORY);
        RuntimeDiagnostics::Record(
            L"WINDOW FAILED: creation callback rejected " +
            (className ? std::wstring(className) : L"<null>") + L".");
        return nullptr;
    }

    // WS_VISIBLE is state, not merely a style bit, in the virtual window
    // manager. Route it through the ordinary show path so creation gets the
    // same focus, invalidation, WM_SHOWWINDOW and initial WM_SIZE behavior as
    // a later ShowWindow call.
    bool initiallyVisible = false;
    {
        std::lock_guard<std::mutex> guard(window->lock);
        initiallyVisible = (window->style & GuestWsVisible) != 0;
    }
    if (initiallyVisible)
    {
        DWORD ignored = ERROR_SUCCESS;
        ShowGuestWindow(window->handle, GuestAbi::SwShow, &ignored);
    }

    SetWin32Error(win32Error, ERROR_SUCCESS);
    RECT createdBounds = {};
    {
        std::lock_guard<std::mutex> guard(window->lock);
        createdBounds = window->bounds;
    }
    RuntimeDiagnostics::Record(
        L"WINDOW OK: " + (className ? std::wstring(className) : L"<null>") +
        (parent ? L" (child, " : L" (top-level, ") +
        std::to_wstring(createdBounds.right - createdBounds.left) + L"x" +
        std::to_wstring(createdBounds.bottom - createdBounds.top) + L" at " +
        std::to_wstring(createdBounds.left) + L"," + std::to_wstring(createdBounds.top) + L").");
    return window->handle;
}

std::shared_ptr<GuestWindowManager::WindowRecord> GuestWindowManager::FindWindow(HWND window) const
{
    const ULONG_PTR token = reinterpret_cast<ULONG_PTR>(window);
    if (token == 0)
    {
        return nullptr;
    }

    std::lock_guard<std::mutex> guard(m_windowsLock);
    const auto found = m_windows.find(token);
    return found == m_windows.end() ? nullptr : found->second;
}

void GuestWindowManager::LayoutGuestRebar(HWND rebar)
{
    const auto record = FindWindow(rebar);
    if (!record)
    {
        return;
    }

    std::vector<RebarBand> bands;
    int availableWidth = 0;
    int availableHeight = 0;
    {
        std::lock_guard<std::mutex> guard(record->lock);
        if (record->destroyed || !record->windowClass ||
            record->windowClass->builtinKind != BuiltinControlKind::Rebar)
        {
            return;
        }
        bands = record->rebarBands;
        availableWidth = record->surface.Width();
        availableHeight = record->surface.Height();
    }

    availableWidth = (std::max)(1, availableWidth);
    availableHeight = (std::max)(1, availableHeight);

    for (auto& band : bands)
    {
        if (!band.child)
        {
            continue;
        }
        const auto child = FindWindow(band.child);
        if (child)
        {
            std::lock_guard<std::mutex> childGuard(child->lock);
            child->parent = rebar;
            band.naturalWidth = child->surface.Width();
            band.naturalHeight = child->surface.Height();
            if (child->windowClass &&
                child->windowClass->builtinKind == BuiltinControlKind::Toolbar)
            {
                const int contentWidth = ToolbarContentWidth(
                    child->toolbarButtonStyles,
                    child->toolbarBitmaps,
                    child->toolbarButtonStates,
                    child->toolbarCommands.size(),
                    child->toolbarButtonWidth,
                    &child->toolbarButtonTexts);
                if (contentWidth > 0) band.naturalWidth = contentWidth;
            }
            else if (child->windowClass &&
                child->windowClass->builtinKind == BuiltinControlKind::ComboBox &&
                (child->style & 0x0003u) != ComboBoxStyleSimple)
            {
                band.naturalHeight = (std::min)(band.naturalHeight,
                    GuestMetrics::ControlHeightForText(GuestMetrics::TextHeight));
            }
        }
        if (IsRebarBandHidden(band))
        {
            DWORD ignored = ERROR_SUCCESS;
            SetGuestWindowPos(band.child, nullptr, 0, 0, 0, 0,
                GuestSwpNoMove | GuestSwpNoSize | GuestSwpNoZOrder | GuestSwpHideWindow,
                &ignored);
        }
    }

    int rowTop = 0;
    size_t first = 0;
    std::vector<HWND> laidOutChildren;
    while (first < bands.size() && rowTop < availableHeight)
    {
        while (first < bands.size() &&
            (IsRebarBandHidden(bands[first]) || !bands[first].child))
        {
            ++first;
        }
        if (first >= bands.size())
        {
            break;
        }

        std::vector<size_t> row;
        int preferredTotal = 0;
        int minimumTotal = 0;
        int rowHeight = 1;
        size_t next = first;
        for (; next < bands.size(); ++next)
        {
            const auto& band = bands[next];
            if (IsRebarBandHidden(band) || !band.child)
            {
                continue;
            }
            const int preferred = (std::min)(availableWidth, RebarBandPreferredWidth(band));
            const int minimum = (std::min)(availableWidth,
                (std::max)(0, band.minimumWidth));
            if (!row.empty() && (((band.style & RebarBandStyleBreak) != 0) ||
                SaturatingAdd(minimumTotal, minimum) > availableWidth))
            {
                break;
            }
            row.push_back(next);
            preferredTotal = SaturatingAdd(preferredTotal, preferred);
            minimumTotal = SaturatingAdd(minimumTotal, minimum);
            rowHeight = (std::max)(rowHeight, RebarBandHeight(band));
        }

        rowHeight = (std::min)(rowHeight, availableHeight - rowTop);
        int flexible = -1;
        for (size_t position = 0; position < row.size(); ++position)
        {
            if ((bands[row[position]].style & RebarBandStyleFixedSize) == 0)
            {
                flexible = static_cast<int>(position);
            }
        }

        int cursor = 0;
        for (size_t position = 0; position < row.size(); ++position)
        {
            const auto& band = bands[row[position]];
            const int remaining = (std::max)(0, availableWidth - cursor);
            int laterMinimum = 0;
            for (size_t later = position + 1; later < row.size(); ++later)
            {
                laterMinimum = SaturatingAdd(laterMinimum,
                    (std::max)(0, bands[row[later]].minimumWidth));
            }
            const int maximumHere = (std::max)(0, remaining - laterMinimum);
            int childWidth = (std::min)(maximumHere, RebarBandPreferredWidth(band));
            childWidth = (std::max)(childWidth,
                (std::min)(remaining, (std::max)(0, band.minimumWidth)));
            if (static_cast<int>(position) == flexible && preferredTotal < availableWidth)
            {
                childWidth = SaturatingAdd(childWidth, availableWidth - preferredTotal);
            }
            DWORD ignored = ERROR_SUCCESS;
            SetGuestWindowPos(band.child, nullptr, cursor, rowTop, childWidth, rowHeight,
                GuestSwpNoZOrder | GuestSwpShowWindow, &ignored);
            laidOutChildren.push_back(band.child);
            cursor = SaturatingAdd(cursor, childWidth);
        }

        rowTop = SaturatingAdd(rowTop, rowHeight);
        first = next;
    }

    // SetWindowPos invalidates resized children, but its immediate retained
    // composition can briefly expose the freshly cleared white surface before
    // the queued WM_PAINT runs. Common controls normally redraw as part of the
    // completed rebar layout, so consume those invalid regions now.
    for (const HWND child : laidOutChildren)
    {
        DWORD ignored = ERROR_SUCCESS;
        UpdateGuestWindow(child, &ignored);
    }
    DWORD ignored = ERROR_SUCCESS;
    InvalidateGuestRect(rebar, nullptr, TRUE, &ignored);
    UpdateGuestWindow(rebar, &ignored);

    RuntimeDiagnostics::Record(L"REBAR: laid out " + std::to_wstring(bands.size()) +
        L" band(s) in " + std::to_wstring(availableWidth) + L"x" +
        std::to_wstring(availableHeight) + L".");
}

bool GuestWindowManager::IsGuestWindowVisibleInternal(HWND window) const
{
    std::unordered_set<ULONG_PTR> visited;
    HWND current = window;
    while (current)
    {
        const ULONG_PTR token = reinterpret_cast<ULONG_PTR>(current);
        if (token == 0 || !visited.emplace(token).second)
        {
            return false;
        }

        const auto record = FindWindow(current);
        if (!record)
        {
            return false;
        }

        HWND parent = nullptr;
        {
            std::lock_guard<std::mutex> guard(record->lock);
            if (record->destroyed || !record->visible)
            {
                return false;
            }
            parent = record->parent;
        }
        current = parent;
    }
    return true;
}

bool GuestWindowManager::IsGuestWindowEnabledInternal(HWND window) const
{
    std::unordered_set<ULONG_PTR> visited;
    HWND current = window;
    while (current)
    {
        const ULONG_PTR token = reinterpret_cast<ULONG_PTR>(current);
        if (token == 0 || !visited.emplace(token).second)
        {
            return false;
        }

        const auto record = FindWindow(current);
        if (!record)
        {
            return false;
        }

        HWND parent = nullptr;
        bool isChild = false;
        {
            std::lock_guard<std::mutex> guard(record->lock);
            if (record->destroyed || !record->enabled)
            {
                return false;
            }
            parent = record->parent;
            isChild = (record->style & GuestAbi::WsChild) != 0;
        }

        // For WS_CHILD windows, enabled state is inherited from the real
        // parent hierarchy.  A WS_POPUP uses the same retained edge only as
        // its owner/composition anchor: disabling that owner for a modal
        // dialog must not disable the popup dialog or its controls as well.
        current = isChild ? parent : nullptr;
    }
    return true;
}

bool GuestWindowManager::IsGuestWindowDescendantOrSelf(HWND candidate, HWND ancestor) const
{
    if (!candidate || !ancestor)
    {
        return false;
    }

    std::unordered_set<ULONG_PTR> visited;
    HWND current = candidate;
    while (current)
    {
        if (current == ancestor)
        {
            return true;
        }

        const ULONG_PTR token = reinterpret_cast<ULONG_PTR>(current);
        if (token == 0 || !visited.emplace(token).second)
        {
            return false;
        }

        const auto record = FindWindow(current);
        if (!record)
        {
            return false;
        }

        std::lock_guard<std::mutex> guard(record->lock);
        // Deliberately follow the retained parent edge even while a parent is
        // being dismantled. That lets hide/disable/destroy clean up capture
        // and focus owned by a descendant before its record leaves the map.
        current = record->parent;
    }
    return false;
}

void GuestWindowManager::ClearGuestForegroundForSubtree(HWND window)
{
    if (!window)
    {
        return;
    }

    const HWND foreground = reinterpret_cast<HWND>(m_foregroundWindow.load());
    if (!foreground || !IsGuestWindowDescendantOrSelf(foreground, window))
    {
        return;
    }

    ULONG_PTR expected = reinterpret_cast<ULONG_PTR>(foreground);
    m_foregroundWindow.compare_exchange_strong(expected, 0);
}

void GuestWindowManager::ClearGuestFocusForSubtree(HWND window)
{
    if (!window)
    {
        return;
    }

    const HWND focus = reinterpret_cast<HWND>(m_focusWindow.load());
    if (!focus || !IsGuestWindowDescendantOrSelf(focus, window))
    {
        return;
    }

    ULONG_PTR expected = reinterpret_cast<ULONG_PTR>(focus);
    if (!m_focusWindow.compare_exchange_strong(expected, 0))
    {
        return;
    }

    const auto record = FindWindow(focus);
    if (!record)
    {
        return;
    }

    bool notify = false;
    {
        std::lock_guard<std::mutex> guard(record->lock);
        // Do not re-enter guest code after DestroyWindow has committed to
        // tearing this HWND down. Hide/disable transitions call this helper
        // before that point and retain the normal WM_KILLFOCUS notification.
        notify = !record->destroyed;
    }
    if (notify)
    {
        CallWindowProcedure(record, GuestAbi::WmKillFocus, 0, 0);
    }
}

bool GuestWindowManager::WouldCreateParentCycle(HWND window, HWND proposedParent) const
{
    std::unordered_set<ULONG_PTR> visited;
    HWND current = proposedParent;
    while (current)
    {
        if (current == window)
        {
            return true;
        }

        const ULONG_PTR token = reinterpret_cast<ULONG_PTR>(current);
        if (token == 0 || !visited.emplace(token).second)
        {
            return true;
        }

        const auto record = FindWindow(current);
        if (!record)
        {
            return true;
        }

        std::lock_guard<std::mutex> guard(record->lock);
        if (record->destroyed)
        {
            return true;
        }
        current = record->parent;
    }
    return false;
}

LRESULT GuestWindowManager::CallWindowProcedure(const std::shared_ptr<WindowRecord>& window, UINT message, WPARAM wParam, LPARAM lParam)
{
    if (!window)
    {
        return 0;
    }

    GuestAbi::WndProc procedure = nullptr;
    HWND handle = nullptr;
    BuiltinControlKind builtinKind = BuiltinControlKind::None;
    {
        std::lock_guard<std::mutex> guard(window->lock);
        procedure = window->procedure;
        handle = window->handle;
        if (window->windowClass)
        {
            builtinKind = window->windowClass->builtinKind;
        }
    }
    if (procedure)
    {
        DWORD guestException = ERROR_SUCCESS;
        const LRESULT result = InvokeGuestWindowProcedure(
            procedure,
            handle,
            message,
            wParam,
            lParam,
            &guestException);
        if (guestException != ERROR_SUCCESS)
        {
            RuntimeDiagnostics::Record(
                L"GUEST CALLBACK EXCEPTION: code " +
                std::to_wstring(static_cast<unsigned long>(guestException)) +
                L", message " + std::to_wstring(message) +
                L", window " + std::to_wstring(reinterpret_cast<ULONG_PTR>(handle)) + L".");
            return 0;
        }
        if (message == GuestAbi::WmPaint)
        {
            bool paintActive = false;
            {
                std::lock_guard<std::mutex> guard(window->lock);
                paintActive = window->paintActive;
            }
            RuntimeDiagnostics::Record(
                L"PAINT CALLBACK: handle " + std::to_wstring(reinterpret_cast<ULONG_PTR>(handle)) +
                L" used a custom procedure (result=" + std::to_wstring(result) +
                L", BeginPaint active=" + (paintActive ? L"yes" : L"no") + L").");
        }
        return result;
    }
    const LRESULT result = builtinKind != BuiltinControlKind::None
        ? BuiltinControlProcedure(window, message, wParam, lParam)
        : DefaultGuestWindowProcedure(handle, message, wParam, lParam);
    if (message == GuestAbi::WmPaint)
    {
        RuntimeDiagnostics::Record(
            L"PAINT CALLBACK: handle " + std::to_wstring(reinterpret_cast<ULONG_PTR>(handle)) +
            (builtinKind != BuiltinControlKind::None ? L" used a bridge control." : L" used the default procedure."));
    }
    return result;
}

void GuestWindowManager::CancelGuestTimersForWindow(HWND window)
{
    if (!window)
    {
        return;
    }

    const auto timers = m_timers;
    if (!timers)
    {
        return;
    }

    CancelledTimerList cancelled = {};
    size_t cancelledCount = 0;
    {
        std::lock_guard<std::mutex> guard(timers->lock);
        for (auto entry = timers->entries.begin(); entry != timers->entries.end();)
        {
            if (entry->second.window == window)
            {
                if (entry->second.timer)
                {
                    cancelled[cancelledCount++] = entry->second.timer;
                }
                entry = timers->entries.erase(entry);
            }
            else
            {
                ++entry;
            }
        }
    }
    CancelThreadPoolTimers(cancelled, cancelledCount);
}

void GuestWindowManager::StopGuestTimers()
{
    const auto timers = m_timers;
    if (!timers)
    {
        return;
    }

    CancelledTimerList cancelled = {};
    size_t cancelledCount = 0;
    {
        std::lock_guard<std::mutex> guard(timers->lock);
        // After this point, any delayed ThreadPoolTimer delegate observes no
        // owner and no entry. This barrier is established before the manager
        // closes its guest message queue or releases window records.
        timers->active = false;
        timers->owner = nullptr;
        for (const auto& entry : timers->entries)
        {
            if (entry.second.timer)
            {
                cancelled[cancelledCount++] = entry.second.timer;
            }
        }
        timers->entries.clear();
    }
    CancelThreadPoolTimers(cancelled, cancelledCount);
}

BOOL GuestWindowManager::DestroyGuestWindow(HWND window, DWORD* win32Error)
{
    const auto record = FindWindow(window);
    if (!record)
    {
        SetWin32Error(win32Error, ERROR_INVALID_WINDOW_HANDLE);
        return FALSE;
    }

    HWND parent = nullptr;
    {
        std::lock_guard<std::mutex> guard(record->lock);
        if (record->destroyed)
        {
            SetWin32Error(win32Error, ERROR_INVALID_WINDOW_HANDLE);
            return FALSE;
        }
        parent = record->parent;
        record->destroyed = true;
    }

    // USER32 tears down child HWNDs before their parent. Snapshot first so
    // re-entrant guest destruction cannot invalidate the iteration, then use
    // the ordinary path for each child (including its timer/DC cleanup).
    std::vector<std::shared_ptr<WindowRecord>> candidates;
    {
        std::lock_guard<std::mutex> guard(m_windowsLock);
        candidates.reserve(m_windows.size());
        for (const auto& item : m_windows)
        {
            if (item.second != record)
            {
                candidates.push_back(item.second);
            }
        }
    }
    for (const auto& candidate : candidates)
    {
        HWND child = nullptr;
        {
            std::lock_guard<std::mutex> guard(candidate->lock);
            if (!candidate->destroyed && candidate->parent == window)
            {
                child = candidate->handle;
            }
        }
        if (child)
        {
            DWORD ignored = ERROR_SUCCESS;
            DestroyGuestWindow(child, &ignored);
        }
    }

    // A callback that was already in flight is serialized by the timer-state
    // lock. Removing its entries before ClearForWindow below ensures it cannot
    // append a WM_TIMER after this HWND has been torn down.
    CancelGuestTimersForWindow(window);
    ClearGuestCaptureForSubtree(window);
    ClearGuestFocusForSubtree(window);
    ClearGuestForegroundForSubtree(window);

    CallWindowProcedure(record, GuestAbi::WmDestroy, 0, 0);
    CallWindowProcedure(record, GuestAbi::WmNcDestroy, 0, 0);
    m_messages.ClearForWindow(window);
    m_gdi.DestroyDc(record->dc);
    {
        std::lock_guard<std::mutex> guard(m_windowsLock);
        m_windows.erase(reinterpret_cast<ULONG_PTR>(window));
    }
    if (parent)
    {
        const auto parentRecord = FindWindow(parent);
        if (parentRecord)
        {
            // Once a child leaves the map, rebuilding its parent is the only
            // way to remove the stale child pixels from the composed desktop.
            Present(parentRecord);
        }
    }
    SetWin32Error(win32Error, ERROR_SUCCESS);
    return TRUE;
}

BOOL GuestWindowManager::ShowGuestWindow(HWND window, int command, DWORD* win32Error)
{
    const auto record = FindWindow(window);
    if (!record)
    {
        SetWin32Error(win32Error, ERROR_INVALID_WINDOW_HANDLE);
        return FALSE;
    }

    const bool show = command != GuestAbi::SwHide;
    bool wasVisible = false;
    bool enabled = false;
    HWND parent = nullptr;
    int width = 0;
    int height = 0;
    {
        std::lock_guard<std::mutex> guard(record->lock);
        if (record->destroyed)
        {
            SetWin32Error(win32Error, ERROR_INVALID_WINDOW_HANDLE);
            return FALSE;
        }
        wasVisible = record->visible;
        record->visible = show;
        if (show)
        {
            record->style |= GuestWsVisible;
        }
        else
        {
            record->style &= ~GuestWsVisible;
        }
        enabled = record->enabled;
        parent = record->parent;
        const GuestMetrics::NonClientMetrics nonClient =
            GuestMetrics::NonClientForEmbeddedWindow(
                record->style, record->extendedStyle, parent != nullptr);
        width = (std::max)(0, record->surface.Width() -
            nonClient.left - nonClient.right);
        height = (std::max)(0, record->surface.Height() -
            nonClient.top - nonClient.bottom -
            ((!parent && record->menuBar) ? GuestMetrics::MenuHeight : 0));
    }

    if (show && !wasVisible)
    {
        const bool effectivelyVisible = IsGuestWindowVisibleInternal(window);
        // Showing a child must not make it a new top-level foreground window.
        // Its parent is composed into the same guest desktop and hit-testing
        // selects the child under the pointer.
        if (!parent)
        {
            m_foregroundWindow.store(reinterpret_cast<ULONG_PTR>(window));
        }
        CallWindowProcedure(record, GuestAbi::WmShowWindow, TRUE, 0);
        if (enabled && effectivelyVisible)
        {
            DWORD ignored = ERROR_SUCCESS;
            SetGuestFocus(window, &ignored);
        }
        PostGuestMessage(window, GuestAbi::WmSize, GuestAbi::SizeRestored,
            GuestAbi::MakeMouseLParam(static_cast<WORD>(width), static_cast<WORD>(height)), nullptr);
        InvalidateGuestRect(window, nullptr, TRUE, nullptr);
        // WS_VISIBLE on a child does not make it screen-visible while one of
        // its ancestors is hidden.  Dialog templates create all visible
        // controls before showing the dialog itself; presenting each child at
        // that point uploaded the unchanged owner frame repeatedly and later
        // exposed the dialog one control at a time as queued paints ran.
        if (effectivelyVisible)
        {
            Present(record);
        }
        RuntimeDiagnostics::Record(
            L"WINDOW SHOW: handle " + std::to_wstring(reinterpret_cast<ULONG_PTR>(window)) +
            (parent ? L" (child)." : L" (top-level)."));
    }
    else if (!show && wasVisible)
    {
        ClearGuestCaptureForSubtree(window);
        ClearGuestFocusForSubtree(window);
        ClearGuestForegroundForSubtree(window);
        CallWindowProcedure(record, GuestAbi::WmShowWindow, FALSE, 0);
        // The record is still mapped, so Present can rebuild its visible
        // top-level ancestor without this newly-hidden child layer.
        Present(record);
    }
    SetWin32Error(win32Error, ERROR_SUCCESS);
    return wasVisible ? TRUE : FALSE;
}

BOOL GuestWindowManager::GetGuestClientRect(HWND window, LPRECT rect, DWORD* win32Error) const
{
    const auto record = FindWindow(window);
    if (!record || !rect)
    {
        SetWin32Error(win32Error, !rect ? ERROR_INVALID_PARAMETER : ERROR_INVALID_WINDOW_HANDLE);
        return FALSE;
    }

    std::lock_guard<std::mutex> guard(record->lock);
    const GuestMetrics::NonClientMetrics nonClient =
        GuestMetrics::NonClientForEmbeddedWindow(
            record->style, record->extendedStyle, record->parent != nullptr);
    rect->left = 0;
    rect->top = 0;
    rect->right = (std::max)(0, record->surface.Width() -
        nonClient.left - nonClient.right);
    rect->bottom = (std::max)(0, record->surface.Height() -
        nonClient.top - nonClient.bottom -
        ((!record->parent && record->menuBar) ? GuestMetrics::MenuHeight : 0));
    SetWin32Error(win32Error, ERROR_SUCCESS);
    return TRUE;
}

BOOL GuestWindowManager::SetGuestWindowMenuBar(HWND window, BOOL visible, DWORD* win32Error)
{
    const auto record = FindWindow(window);
    if (!record)
    {
        SetWin32Error(win32Error, ERROR_INVALID_WINDOW_HANDLE);
        return FALSE;
    }

    bool changed = false;
    bool topLevel = false;
    int width = 0;
    int height = 0;
    {
        std::lock_guard<std::mutex> guard(record->lock);
        topLevel = !record->parent;
        const bool requested = visible != FALSE;
        changed = topLevel && record->menuBar != requested;
        record->menuBar = topLevel && requested;
        const GuestMetrics::NonClientMetrics nonClient =
            GuestMetrics::NonClientForEmbeddedWindow(
                record->style, record->extendedStyle, false);
        width = (std::max)(0, record->surface.Width() -
            nonClient.left - nonClient.right);
        height = (std::max)(0, record->surface.Height() -
            nonClient.top - nonClient.bottom -
            (record->menuBar ? GuestMetrics::MenuHeight : 0));
    }
    if (changed)
    {
        // A menu is non-client chrome. The guest therefore continues to use
        // client coordinates beginning at zero, while composition applies the
        // visual offset below the bar.
        PostGuestMessage(window, GuestAbi::WmSize, GuestAbi::SizeRestored,
            GuestAbi::MakeMouseLParam(static_cast<WORD>(width), static_cast<WORD>(height)), nullptr);
    }
    InvalidateGuestRect(window, nullptr, TRUE, nullptr);
    Present(record);
    SetWin32Error(win32Error, ERROR_SUCCESS);
    return TRUE;
}

UINT GuestWindowManager::TrackGuestPopupMenu(HMENU menu, UINT flags, int x, int y,
    HWND owner, const RECT* excludeRect, DWORD* win32Error)
{
    HWND root = nullptr;
    int rootWidth = 0;
    int rootHeight = 0;
    int ignored = 0;
    if (!menu || !owner || !GetGuestSurfaceGeometry(owner, &root, &rootWidth, &rootHeight,
        &ignored, &ignored, &ignored, &ignored))
    {
        SetWin32Error(win32Error, ERROR_INVALID_PARAMETER);
        return 0;
    }

    // TrackPopupMenuEx initializes the popup before taking its visual snapshot;
    // applications commonly change captions and enabled state from this
    // callback. The bridge never keeps a menu-model lock while invoking guest
    // code, so dynamic InsertMenuItem/SetMenuItemInfo calls are safe here.
    constexpr UINT TpmReturnCommand = 0x0100;
    constexpr UINT TpmNoNotify = 0x0080;
    constexpr UINT TpmRightButton = 0x0002;
    constexpr UINT TpmCenterAlign = 0x0004;
    constexpr UINT TpmRightAlign = 0x0008;
    constexpr UINT TpmVCenterAlign = 0x0010;
    constexpr UINT TpmBottomAlign = 0x0020;
    SendGuestMessage(owner, GuestAbi::WmEnterMenuLoop, TRUE, 0, nullptr);
    SendGuestMessage(owner, GuestAbi::WmInitMenu,
        reinterpret_cast<WPARAM>(menu), 0, nullptr);
    SendGuestMessage(owner, GuestAbi::WmInitMenuPopup,
        reinterpret_cast<WPARAM>(menu), 0, nullptr);
    const std::vector<GuestMenuVisualItem> popupItems = GetGuestMenuItems(menu);
    if (popupItems.empty())
    {
        SendGuestMessage(owner, GuestAbi::WmExitMenuLoop, TRUE, 0, nullptr);
        SetWin32Error(win32Error, ERROR_SUCCESS);
        return 0;
    }
    // TrackPopupMenuEx receives screen coordinates.  The retained popup is
    // composed in the complete root surface (including its menu strip), so
    // translate against the root window origin rather than its client origin.
    RECT rootRect{};
    if (!GetGuestWindowRect(root, &rootRect, win32Error))
    {
        return 0;
    }
    int rootX = x - rootRect.left;
    int rootY = y - rootRect.top;
    const int popupWidth = PopupMenuWidth(popupItems);
    const int popupHeight = PopupMenuHeight(popupItems);
    if (flags & TpmRightAlign) rootX -= popupWidth;
    else if (flags & TpmCenterAlign) rootX -= popupWidth / 2;
    if (flags & TpmBottomAlign) rootY -= popupHeight;
    else if (flags & TpmVCenterAlign) rootY -= popupHeight / 2;
    if (excludeRect)
    {
        const RECT local{ excludeRect->left - rootRect.left, excludeRect->top - rootRect.top,
            excludeRect->right - rootRect.left, excludeRect->bottom - rootRect.top };
        const bool overlaps = rootX < local.right && rootX + popupWidth > local.left &&
            rootY < local.bottom && rootY + popupHeight > local.top;
        if (overlaps)
        {
            if (local.bottom + popupHeight <= rootHeight) rootY = local.bottom;
            else if (local.top >= popupHeight) rootY = local.top - popupHeight;
            else if (local.right + popupWidth <= rootWidth) rootX = local.right;
            else rootX = local.left - popupWidth;
        }
    }
    const int popupLeft = (std::max)(0, (std::min)(rootX,
        (std::max)(0, rootWidth - popupWidth)));
    const int popupTop = (std::max)(0, (std::min)(rootY,
        (std::max)(0, rootHeight - popupHeight)));
    {
        std::lock_guard<std::mutex> guard(m_popupMenuLock);
        m_popupMenu = PopupMenuSession{};
        m_popupMenu.menu = menu;
        m_popupMenu.owner = owner;
        m_popupMenu.root = root;
        m_popupMenu.left = popupLeft;
        m_popupMenu.top = popupTop;
        m_popupMenu.returnCommand = (flags & TpmReturnCommand) != 0;
        m_popupMenu.notifyOwner = (flags & TpmNoNotify) == 0;
        m_popupMenu.allowRightButton = (flags & TpmRightButton) != 0;
        m_popupMenu.selectedCommand = 0;
        m_popupMenu.open = true;
        m_popupMenu.levels.push_back(PopupMenuLevel{ menu, popupLeft, popupTop, -1, -1 });
    }
    size_t captionCount = 0;
    for (const auto& item : popupItems)
    {
        if (!item.text.empty()) ++captionCount;
    }
    RuntimeDiagnostics::Record(L"MENU: opened a guest popup menu with " +
        std::to_wstring(popupItems.size()) + L" item(s), " +
        std::to_wstring(captionCount) + L" caption(s).");
    InvalidateGuestRect(root, nullptr, FALSE, nullptr);
    Present(FindWindow(root));

    std::unique_lock<std::mutex> lock(m_popupMenuLock);
    m_popupMenuChanged.wait(lock, [this] { return !m_popupMenu.open || !m_active.load(); });
    const UINT command = m_popupMenu.selectedCommand;
    const bool returnCommand = m_popupMenu.returnCommand;
    const bool notifyOwner = m_popupMenu.notifyOwner;
    const std::vector<PopupMenuLevel> closedLevels = m_popupMenu.levels;
    m_popupMenu = PopupMenuSession{};
    lock.unlock();
    if (command && !returnCommand && notifyOwner)
    {
        SendGuestMessage(owner, GuestAbi::WmCommand,
            GuestAbi::MakeCommandWParam(static_cast<WORD>(command), 0), 0, nullptr);
    }
    for (auto iterator = closedLevels.rbegin(); iterator != closedLevels.rend(); ++iterator)
        SendGuestMessage(owner, GuestAbi::WmUninitMenuPopup,
            reinterpret_cast<WPARAM>(iterator->menu), 0, nullptr);
    SendGuestMessage(owner, GuestAbi::WmExitMenuLoop, TRUE, 0, nullptr);
    SetWin32Error(win32Error, ERROR_SUCCESS);
    return returnCommand ? command : (command ? TRUE : FALSE);
}

HWND GuestWindowManager::GetGuestForegroundWindow() const
{
    const HWND foreground = reinterpret_cast<HWND>(m_foregroundWindow.load());
    return foreground && IsGuestWindow(foreground) ? foreground : nullptr;
}

void GuestWindowManager::DestroyGuestWindowsForProcess(
    DWORD processId,
    HWND restoreForeground,
    HWND restoreFocus)
{
    if (!processId)
    {
        return;
    }

    // Run has already restored the caller's runtime context. Therefore this is
    // process-exit teardown, not DestroyWindow: release USER/GDI state without
    // calling a departed process's WNDPROC under the parent's shim context.
    std::vector<std::shared_ptr<WindowRecord>> victims;
    std::unordered_set<ULONG_PTR> victimHandles;
    {
        std::lock_guard<std::mutex> guard(m_windowsLock);
        for (const auto& item : m_windows)
        {
            const auto& candidate = item.second;
            std::lock_guard<std::mutex> windowGuard(candidate->lock);
            if (!candidate->destroyed && candidate->processId == processId)
            {
                candidate->destroyed = true;
                victims.push_back(candidate);
                victimHandles.emplace(item.first);
            }
        }
        for (ULONG_PTR handle : victimHandles)
        {
            m_windows.erase(handle);
        }
    }

    for (const auto& victim : victims)
    {
        CancelGuestTimersForWindow(victim->handle);
        m_messages.ClearForWindow(victim->handle);
        m_gdi.DestroyDc(victim->dc);
    }

    const auto clearOwnedAtomic = [&victimHandles](std::atomic<ULONG_PTR>& value)
    {
        ULONG_PTR current = value.load();
        while (victimHandles.find(current) != victimHandles.end() &&
            !value.compare_exchange_weak(current, 0))
        {
        }
    };
    clearOwnedAtomic(m_captureWindow);
    clearOwnedAtomic(m_focusWindow);
    clearOwnedAtomic(m_foregroundWindow);
    {
        std::lock_guard<std::mutex> guard(m_popupMenuLock);
        if (victimHandles.find(reinterpret_cast<ULONG_PTR>(m_popupMenu.owner)) != victimHandles.end() ||
            victimHandles.find(reinterpret_cast<ULONG_PTR>(m_popupMenu.root)) != victimHandles.end())
        {
            m_popupMenu = PopupMenuSession{};
            m_popupMenuChanged.notify_all();
        }
    }
    {
        std::lock_guard<std::mutex> guard(m_classesLock);
        for (auto iterator = m_classes.begin(); iterator != m_classes.end();)
        {
            if (iterator->second && iterator->second->processId == processId)
                iterator = m_classes.erase(iterator);
            else
                ++iterator;
        }
        for (auto iterator = m_classesByAtom.begin(); iterator != m_classesByAtom.end();)
        {
            if (iterator->second && iterator->second->processId == processId)
                iterator = m_classesByAtom.erase(iterator);
            else
                ++iterator;
        }
    }

    HWND foreground = IsGuestWindow(restoreForeground) ? restoreForeground : nullptr;
    if (!foreground)
    {
        ULONG_PTR newest = 0;
        std::lock_guard<std::mutex> guard(m_windowsLock);
        for (const auto& item : m_windows)
        {
            const auto& candidate = item.second;
            std::lock_guard<std::mutex> windowGuard(candidate->lock);
            if (!candidate->destroyed && candidate->visible && !candidate->parent &&
                item.first > newest)
            {
                newest = item.first;
                foreground = candidate->handle;
            }
        }
    }
    m_foregroundWindow.store(reinterpret_cast<ULONG_PTR>(foreground));

    if (IsGuestWindow(restoreFocus))
    {
        DWORD ignored = ERROR_SUCCESS;
        SetGuestFocus(restoreFocus, &ignored);
    }
    else
    {
        m_focusWindow.store(reinterpret_cast<ULONG_PTR>(foreground));
    }

    if (foreground)
    {
        const auto record = FindWindow(foreground);
        if (record)
        {
            InvalidateGuestRect(foreground, nullptr, TRUE, nullptr);
            Present(record);
        }
    }
    RuntimeDiagnostics::Record(L"PROCESS WINDOWS: removed windows for process " +
        std::to_wstring(processId) + L" and restored the previous desktop activation.");
}

BOOL GuestWindowManager::EndGuestMenu(DWORD* win32Error)
{
    PopupMenuSession closing;
    {
        std::lock_guard<std::mutex> guard(m_popupMenuLock);
        if (!m_popupMenu.open)
        {
            SetWin32Error(win32Error, ERROR_INVALID_MENU_HANDLE);
            return FALSE;
        }
        m_popupMenu.selectedCommand = 0;
        m_popupMenu.open = false;
        closing = m_popupMenu;
    }
    if (const auto root = FindWindow(closing.root))
    {
        std::lock_guard<std::mutex> guard(root->lock);
        if (!root->destroyed) root->openMenuIndex = -1;
    }
    if (closing.owner)
    {
        SendGuestMessage(closing.owner, GuestAbi::WmMenuSelect,
            GuestAbi::MakeCommandWParam(0, 0xffff), 0, nullptr);
        if (closing.menuBar)
        {
            for (auto iterator = closing.levels.rbegin(); iterator != closing.levels.rend(); ++iterator)
                SendGuestMessage(closing.owner, GuestAbi::WmUninitMenuPopup,
                    reinterpret_cast<WPARAM>(iterator->menu), 0, nullptr);
            SendGuestMessage(closing.owner, GuestAbi::WmExitMenuLoop, FALSE, 0, nullptr);
        }
    }
    m_popupMenuChanged.notify_all();
    InvalidateGuestRect(closing.root, nullptr, FALSE, nullptr);
    Present(FindWindow(closing.root));
    SetWin32Error(win32Error, ERROR_SUCCESS);
    return TRUE;
}

BOOL GuestWindowManager::GetGuestWindowRect(HWND window, LPRECT rect, DWORD* win32Error) const
{
    if (!rect)
    {
        SetWin32Error(win32Error, ERROR_INVALID_PARAMETER);
        return FALSE;
    }
    // Win32 GetWindowRect always returns screen coordinates, including for
    // child windows.  WindowRecord::bounds is deliberately parent-client
    // relative because the compositor consumes it that way, so accumulate
    // the complete parent chain here instead of exposing the retained-model
    // representation to guest applications.
    std::unordered_set<ULONG_PTR> visited;
    HWND current = window;
    bool first = true;
    int left = 0;
    int top = 0;
    int width = 0;
    int height = 0;
    while (current)
    {
        const ULONG_PTR token = reinterpret_cast<ULONG_PTR>(current);
        if (token == 0 || !visited.emplace(token).second)
        {
            SetWin32Error(win32Error, ERROR_INVALID_WINDOW_HANDLE);
            return FALSE;
        }

        const auto record = FindWindow(current);
        if (!record)
        {
            SetWin32Error(win32Error, ERROR_INVALID_WINDOW_HANDLE);
            return FALSE;
        }

        HWND parent = nullptr;
        RECT bounds{};
        bool menuBar = false;
        {
            std::lock_guard<std::mutex> guard(record->lock);
            if (record->destroyed)
            {
                SetWin32Error(win32Error, ERROR_INVALID_WINDOW_HANDLE);
                return FALSE;
            }
            parent = record->parent;
            bounds = record->bounds;
            menuBar = record->menuBar;
        }

        if (first)
        {
            width = bounds.right - bounds.left;
            height = bounds.bottom - bounds.top;
            first = false;
        }
        left = SaturatingAdd(left, bounds.left);
        top = SaturatingAdd(top, bounds.top);

        // Child coordinates are relative to the parent's client origin.  The
        // bridge models the root menu as non-client chrome, so descendants of
        // that root begin below it on the composed guest screen.
        if (!parent && menuBar && current != window)
        {
            top = SaturatingAdd(top, GuestMetrics::MenuHeight);
        }
        current = parent;
    }

    rect->left = left;
    rect->top = top;
    rect->right = SaturatingAdd(left, width);
    rect->bottom = SaturatingAdd(top, height);
    SetWin32Error(win32Error, ERROR_SUCCESS);
    return TRUE;
}

BOOL GuestWindowManager::GetGuestClientOrigin(HWND window, LPPOINT point, DWORD* win32Error) const
{
    if (!point)
    {
        SetWin32Error(win32Error, ERROR_INVALID_PARAMETER);
        return FALSE;
    }

    RECT windowRect{};
    if (!GetGuestWindowRect(window, &windowRect, win32Error))
    {
        return FALSE;
    }

    const auto record = FindWindow(window);
    if (!record)
    {
        SetWin32Error(win32Error, ERROR_INVALID_WINDOW_HANDLE);
        return FALSE;
    }

    bool rootMenuBar = false;
    {
        std::lock_guard<std::mutex> guard(record->lock);
        if (record->destroyed)
        {
            SetWin32Error(win32Error, ERROR_INVALID_WINDOW_HANDLE);
            return FALSE;
        }
        // Descendant window rectangles already include the root's client
        // offset while walking their parent chain.  Only the root itself
        // still needs its non-client menu height applied here.
        rootMenuBar = !record->parent && record->menuBar;
    }

    point->x = windowRect.left;
    point->y = SaturatingAdd(windowRect.top,
        rootMenuBar ? GuestMetrics::MenuHeight : 0);
    SetWin32Error(win32Error, ERROR_SUCCESS);
    return TRUE;
}

BOOL GuestWindowManager::SetGuestWindowText(HWND window, LPCWSTR text, DWORD* win32Error)
{
    const auto record = FindWindow(window);
    if (!record || !text)
    {
        SetWin32Error(win32Error, !text ? ERROR_INVALID_PARAMETER : ERROR_INVALID_WINDOW_HANDLE);
        return FALSE;
    }

    {
        std::lock_guard<std::mutex> guard(record->lock);
        record->title = text;
    }
    CallWindowProcedure(record, GuestAbi::WmSetText, 0, reinterpret_cast<LPARAM>(text));
    SetWin32Error(win32Error, ERROR_SUCCESS);
    return TRUE;
}

int GuestWindowManager::GetGuestWindowText(HWND window, LPWSTR buffer, int count, DWORD* win32Error) const
{
    const auto record = FindWindow(window);
    if (!record || !buffer || count <= 0)
    {
        SetWin32Error(win32Error, (!buffer || count <= 0) ? ERROR_INVALID_PARAMETER : ERROR_INVALID_WINDOW_HANDLE);
        return 0;
    }

    std::lock_guard<std::mutex> guard(record->lock);
    const size_t copied = (std::min)(record->title.size(), static_cast<size_t>(count - 1));
    memcpy(buffer, record->title.data(), copied * sizeof(wchar_t));
    buffer[copied] = L'\0';
    SetWin32Error(win32Error, ERROR_SUCCESS);
    return static_cast<int>(copied);
}

int GuestWindowManager::GetGuestWindowTextLength(HWND window, DWORD* win32Error) const
{
    const auto record = FindWindow(window);
    if (!record)
    {
        SetWin32Error(win32Error, ERROR_INVALID_WINDOW_HANDLE);
        return 0;
    }

    std::lock_guard<std::mutex> guard(record->lock);
    if (record->destroyed)
    {
        SetWin32Error(win32Error, ERROR_INVALID_WINDOW_HANDLE);
        return 0;
    }
    if (record->title.size() > static_cast<size_t>((std::numeric_limits<int>::max)()))
    {
        SetWin32Error(win32Error, ERROR_ARITHMETIC_OVERFLOW);
        return 0;
    }

    SetWin32Error(win32Error, ERROR_SUCCESS);
    return static_cast<int>(record->title.size());
}

BOOL GuestWindowManager::IsGuestWindow(HWND window) const
{
    return FindWindow(window) ? TRUE : FALSE;
}

std::vector<HWND> GuestWindowManager::SnapshotGuestWindows(HWND parent, bool descendants) const
{
    std::vector<std::shared_ptr<WindowRecord>> records;
    {
        std::lock_guard<std::mutex> guard(m_windowsLock);
        records.reserve(m_windows.size());
        for (const auto& item : m_windows) records.push_back(item.second);
    }
    std::vector<HWND> result;
    for (const auto& record : records)
    {
        HWND candidateParent = nullptr;
        bool alive = false;
        {
            std::lock_guard<std::mutex> guard(record->lock);
            alive = !record->destroyed;
            candidateParent = record->parent;
        }
        if (!alive) continue;
        const bool matches = parent
            ? (descendants ? IsGuestWindowDescendantOrSelf(candidateParent, parent)
                           : candidateParent == parent)
            : candidateParent == nullptr;
        if (matches) result.push_back(record->handle);
    }
    std::sort(result.begin(), result.end(), [](HWND left, HWND right)
    {
        return reinterpret_cast<ULONG_PTR>(left) < reinterpret_cast<ULONG_PTR>(right);
    });
    return result;
}

BOOL GuestWindowManager::IsGuestChild(HWND parent, HWND window) const
{
    return parent && window && window != parent && IsGuestWindowDescendantOrSelf(window, parent);
}

int GuestWindowManager::GetGuestClassName(HWND window, LPWSTR buffer, int count, DWORD* win32Error) const
{
    const auto record = FindWindow(window);
    if (!record || !buffer || count <= 0)
    {
        SetWin32Error(win32Error, record ? ERROR_INVALID_PARAMETER : ERROR_INVALID_WINDOW_HANDLE);
        return 0;
    }
    std::wstring name;
    {
        std::lock_guard<std::mutex> guard(record->lock);
        if (record->destroyed || !record->windowClass)
        {
            SetWin32Error(win32Error, ERROR_INVALID_WINDOW_HANDLE);
            return 0;
        }
        name = record->windowClass->name;
    }
    const size_t copied = (std::min)(name.size(), static_cast<size_t>(count - 1));
    std::memcpy(buffer, name.data(), copied * sizeof(wchar_t));
    buffer[copied] = L'\0';
    SetWin32Error(win32Error, ERROR_SUCCESS);
    return static_cast<int>(copied);
}

HWND GuestWindowManager::FindGuestWindowEx(HWND parent, HWND after, LPCWSTR className,
    LPCWSTR title, DWORD* win32Error) const
{
    const auto candidates = SnapshotGuestWindows(parent, false);
    bool eligible = after == nullptr;
    for (const HWND candidate : candidates)
    {
        if (!eligible)
        {
            eligible = candidate == after;
            continue;
        }
        const auto record = FindWindow(candidate);
        if (!record) continue;
        std::wstring candidateClass;
        std::wstring candidateTitle;
        {
            std::lock_guard<std::mutex> guard(record->lock);
            if (record->destroyed) continue;
            candidateClass = record->windowClass ? record->windowClass->name : L"";
            candidateTitle = record->title;
        }
        const bool classMatches = !className ||
            (reinterpret_cast<ULONG_PTR>(className) <= 0xffff
                ? record->windowClass && record->windowClass->atom == LOWORD(reinterpret_cast<ULONG_PTR>(className))
                : _wcsicmp(candidateClass.c_str(), className) == 0);
        if (classMatches && (!title || candidateTitle == title))
        {
            SetWin32Error(win32Error, ERROR_SUCCESS);
            return candidate;
        }
    }
    SetWin32Error(win32Error, ERROR_SUCCESS);
    return nullptr;
}

HWND GuestWindowManager::GetGuestWindowRelationship(HWND window, UINT command, DWORD* win32Error) const
{
    constexpr UINT GuestGwHwndFirst = 0;
    constexpr UINT GuestGwHwndLast = 1;
    constexpr UINT GuestGwHwndNext = 2;
    constexpr UINT GuestGwHwndPrev = 3;
    constexpr UINT GuestGwOwner = 4;
    constexpr UINT GuestGwChild = 5;
    const auto record = FindWindow(window);
    if (!record)
    {
        SetWin32Error(win32Error, ERROR_INVALID_WINDOW_HANDLE);
        return nullptr;
    }
    HWND parent = nullptr;
    {
        std::lock_guard<std::mutex> guard(record->lock);
        parent = record->parent;
    }
    if (command == GuestGwOwner)
    {
        SetWin32Error(win32Error, ERROR_SUCCESS);
        return parent;
    }
    if (command == GuestGwChild)
    {
        const auto children = SnapshotGuestWindows(window, false);
        SetWin32Error(win32Error, ERROR_SUCCESS);
        return children.empty() ? nullptr : children.front();
    }
    if (command == GuestGwHwndFirst || command == GuestGwHwndLast || command == GuestGwHwndNext || command == GuestGwHwndPrev)
    {
        const auto siblings = SnapshotGuestWindows(parent, false);
        const auto found = std::find(siblings.begin(), siblings.end(), window);
        if (found == siblings.end()) return nullptr;
        HWND result = nullptr;
        if (command == GuestGwHwndFirst && !siblings.empty()) result = siblings.front();
        else if (command == GuestGwHwndLast && !siblings.empty()) result = siblings.back();
        else if (command == GuestGwHwndNext && std::next(found) != siblings.end()) result = *std::next(found);
        else if (command == GuestGwHwndPrev && found != siblings.begin()) result = *std::prev(found);
        SetWin32Error(win32Error, ERROR_SUCCESS);
        return result;
    }
    SetWin32Error(win32Error, ERROR_INVALID_PARAMETER);
    return nullptr;
}

HWND GuestWindowManager::SetGuestParent(HWND window, HWND parent, DWORD* win32Error)
{
    const auto record = FindWindow(window);
    if (!record || (parent && !FindWindow(parent)) || WouldCreateParentCycle(window, parent))
    {
        SetWin32Error(win32Error, !record ? ERROR_INVALID_WINDOW_HANDLE : ERROR_INVALID_PARAMETER);
        return nullptr;
    }
    HWND previous = nullptr;
    {
        std::lock_guard<std::mutex> guard(record->lock);
        previous = record->parent;
        record->parent = parent;
        if (parent) record->style |= WS_CHILD;
        else record->style &= ~WS_CHILD;
    }
    RECT bounds{};
    GetGuestWindowRect(window, &bounds, nullptr);
    InvalidateGuestRect(window, nullptr, TRUE, nullptr);
    SetWin32Error(win32Error, ERROR_SUCCESS);
    return previous;
}

BOOL GuestWindowManager::IsGuestWindowVisible(HWND window, DWORD* win32Error) const
{
    const auto record = FindWindow(window);
    if (!record)
    {
        SetWin32Error(win32Error, ERROR_INVALID_WINDOW_HANDLE);
        return FALSE;
    }
    {
        std::lock_guard<std::mutex> guard(record->lock);
        if (record->destroyed)
        {
            SetWin32Error(win32Error, ERROR_INVALID_WINDOW_HANDLE);
            return FALSE;
        }
    }

    SetWin32Error(win32Error, ERROR_SUCCESS);
    return IsGuestWindowVisibleInternal(window) ? TRUE : FALSE;
}

BOOL GuestWindowManager::IsGuestWindowEnabled(HWND window, DWORD* win32Error) const
{
    const auto record = FindWindow(window);
    if (!record)
    {
        SetWin32Error(win32Error, ERROR_INVALID_WINDOW_HANDLE);
        return FALSE;
    }
    {
        std::lock_guard<std::mutex> guard(record->lock);
        if (record->destroyed)
        {
            SetWin32Error(win32Error, ERROR_INVALID_WINDOW_HANDLE);
            return FALSE;
        }
    }

    SetWin32Error(win32Error, ERROR_SUCCESS);
    return IsGuestWindowEnabledInternal(window) ? TRUE : FALSE;
}

BOOL GuestWindowManager::EnableGuestWindow(HWND window, BOOL enable, DWORD* win32Error)
{
    const auto record = FindWindow(window);
    if (!record)
    {
        SetWin32Error(win32Error, ERROR_INVALID_WINDOW_HANDLE);
        return FALSE;
    }

    const bool requested = enable != FALSE;
    bool previouslyEnabled = false;
    {
        std::lock_guard<std::mutex> guard(record->lock);
        if (record->destroyed)
        {
            SetWin32Error(win32Error, ERROR_INVALID_WINDOW_HANDLE);
            return FALSE;
        }
        previouslyEnabled = record->enabled;
        record->enabled = requested;
        if (requested)
        {
            record->style &= ~GuestWsDisabled;
        }
        else
        {
            record->style |= GuestWsDisabled;
        }
    }

    if (previouslyEnabled != requested)
    {
        CallWindowProcedure(record, GuestAbi::WmEnable, requested ? TRUE : FALSE, 0);
        if (!requested)
        {
            ClearGuestCaptureForSubtree(window);
            ClearGuestFocusForSubtree(window);
        }
    }

    SetWin32Error(win32Error, ERROR_SUCCESS);
    // EnableWindow reports whether the target was previously disabled, which
    // lets a guest distinguish a state change from a no-op without exposing
    // host state.
    return previouslyEnabled ? FALSE : TRUE;
}

HWND GuestWindowManager::SetGuestFocus(HWND window, DWORD* win32Error)
{
    if (window)
    {
        const auto requested = FindWindow(window);
        if (!requested)
        {
            SetWin32Error(win32Error, ERROR_INVALID_WINDOW_HANDLE);
            return nullptr;
        }
        {
            std::lock_guard<std::mutex> guard(requested->lock);
            if (requested->destroyed)
            {
                SetWin32Error(win32Error, ERROR_INVALID_WINDOW_HANDLE);
                return nullptr;
            }
        }
        if (!IsGuestWindowVisibleInternal(window) || !IsGuestWindowEnabledInternal(window))
        {
            SetWin32Error(win32Error, ERROR_INVALID_PARAMETER);
            return nullptr;
        }
    }

    const ULONG_PTR requestedValue = reinterpret_cast<ULONG_PTR>(window);
    const HWND previous = reinterpret_cast<HWND>(m_focusWindow.exchange(requestedValue));
    if (previous != window)
    {
        const auto previousRecord = FindWindow(previous);
        if (previousRecord)
        {
            CallWindowProcedure(previousRecord, GuestAbi::WmKillFocus, reinterpret_cast<WPARAM>(window), 0);
        }

        const auto requestedRecord = FindWindow(window);
        if (requestedRecord)
        {
            CallWindowProcedure(requestedRecord, GuestAbi::WmSetFocus, reinterpret_cast<WPARAM>(previous), 0);
        }
    }

    SetWin32Error(win32Error, ERROR_SUCCESS);
    return previous;
}

HWND GuestWindowManager::GetGuestFocus(DWORD* win32Error)
{
    const HWND focus = reinterpret_cast<HWND>(m_focusWindow.load());
    if (!focus)
    {
        SetWin32Error(win32Error, ERROR_SUCCESS);
        return nullptr;
    }

    const auto record = FindWindow(focus);
    bool usable = false;
    if (record)
    {
        std::lock_guard<std::mutex> guard(record->lock);
        usable = !record->destroyed;
    }
    if (!usable || !IsGuestWindowVisibleInternal(focus) || !IsGuestWindowEnabledInternal(focus))
    {
        ULONG_PTR expected = reinterpret_cast<ULONG_PTR>(focus);
        m_focusWindow.compare_exchange_strong(expected, 0);
        SetWin32Error(win32Error, ERROR_SUCCESS);
        return nullptr;
    }

    SetWin32Error(win32Error, ERROR_SUCCESS);
    return focus;
}

HWND GuestWindowManager::SetGuestCapture(HWND window, DWORD* win32Error)
{
    if (!m_active.load())
    {
        SetWin32Error(win32Error, ERROR_INVALID_FUNCTION);
        return nullptr;
    }
    if (!window)
    {
        const HWND previous = reinterpret_cast<HWND>(m_captureWindow.exchange(0));
        if (previous)
        {
            const auto previousRecord = FindWindow(previous);
            if (previousRecord)
            {
                CallWindowProcedure(previousRecord, GuestAbi::WmCaptureChanged, 0, 0);
            }
        }
        SetWin32Error(win32Error, ERROR_SUCCESS);
        return previous;
    }

    const auto requested = FindWindow(window);
    if (!requested)
    {
        SetWin32Error(win32Error, ERROR_INVALID_WINDOW_HANDLE);
        return nullptr;
    }
    {
        std::lock_guard<std::mutex> guard(requested->lock);
        if (requested->destroyed)
        {
            SetWin32Error(win32Error, ERROR_INVALID_WINDOW_HANDLE);
            return nullptr;
        }
    }
    if (!IsGuestWindowVisibleInternal(window) || !IsGuestWindowEnabledInternal(window))
    {
        SetWin32Error(win32Error, ERROR_INVALID_PARAMETER);
        return nullptr;
    }

    const HWND previous = reinterpret_cast<HWND>(
        m_captureWindow.exchange(reinterpret_cast<ULONG_PTR>(window)));
    if (previous && previous != window)
    {
        const auto previousRecord = FindWindow(previous);
        if (previousRecord)
        {
            CallWindowProcedure(
                previousRecord,
                GuestAbi::WmCaptureChanged,
                0,
                reinterpret_cast<LPARAM>(window));
        }
    }
    SetWin32Error(win32Error, ERROR_SUCCESS);
    return previous;
}

BOOL GuestWindowManager::ReleaseGuestCapture(DWORD* win32Error)
{
    const HWND previous = reinterpret_cast<HWND>(m_captureWindow.exchange(0));
    if (previous)
    {
        const auto previousRecord = FindWindow(previous);
        if (previousRecord)
        {
            CallWindowProcedure(previousRecord, GuestAbi::WmCaptureChanged, 0, 0);
        }
    }
    SetWin32Error(win32Error, ERROR_SUCCESS);
    return TRUE;
}

HWND GuestWindowManager::GetGuestCapture(DWORD* win32Error)
{
    const HWND capture = reinterpret_cast<HWND>(m_captureWindow.load());
    if (!capture)
    {
        SetWin32Error(win32Error, ERROR_SUCCESS);
        return nullptr;
    }

    const auto record = FindWindow(capture);
    bool valid = false;
    if (record)
    {
        std::lock_guard<std::mutex> guard(record->lock);
        valid = !record->destroyed;
    }
    if (!valid || !IsGuestWindowVisibleInternal(capture) || !IsGuestWindowEnabledInternal(capture))
    {
        ULONG_PTR expected = reinterpret_cast<ULONG_PTR>(capture);
        m_captureWindow.compare_exchange_strong(expected, 0);
        SetWin32Error(win32Error, ERROR_SUCCESS);
        return nullptr;
    }

    SetWin32Error(win32Error, ERROR_SUCCESS);
    return capture;
}

void GuestWindowManager::ClearGuestCapture(HWND window, HWND replacement)
{
    if (!window)
    {
        return;
    }

    ULONG_PTR expected = reinterpret_cast<ULONG_PTR>(window);
    if (!m_captureWindow.compare_exchange_strong(expected, reinterpret_cast<ULONG_PTR>(replacement)))
    {
        return;
    }

    const auto record = FindWindow(window);
    if (record)
    {
        bool notify = false;
        {
            std::lock_guard<std::mutex> guard(record->lock);
            // DestroyGuestWindow intentionally marks a record before tearing
            // down children and queues. Suppress a re-entrant WNDPROC only in
            // that committed teardown state; ordinary hide/disable/replacement
            // paths still observe the normal synchronous notification.
            notify = !record->destroyed;
        }
        if (notify)
        {
            CallWindowProcedure(
                record,
                GuestAbi::WmCaptureChanged,
                0,
                reinterpret_cast<LPARAM>(replacement));
        }
    }
}

void GuestWindowManager::ClearGuestCaptureForSubtree(HWND window, HWND replacement)
{
    if (!window)
    {
        return;
    }

    const HWND capture = reinterpret_cast<HWND>(m_captureWindow.load());
    if (capture && IsGuestWindowDescendantOrSelf(capture, window))
    {
        ClearGuestCapture(capture, replacement);
    }
}

bool GuestWindowManager::GetGuestSurfaceGeometry(
    HWND window,
    HWND* rootWindow,
    int* rootWidth,
    int* rootHeight,
    int* targetWidth,
    int* targetHeight,
    int* targetLeft,
    int* targetTop) const
{
    if (!window || !rootWindow || !rootWidth || !rootHeight || !targetWidth || !targetHeight ||
        !targetLeft || !targetTop)
    {
        return false;
    }

    std::unordered_set<ULONG_PTR> visited;
    HWND current = window;
    bool first = true;
    int accumulatedLeft = 0;
    int accumulatedTop = 0;
    bool rootMenuBar = false;
    while (current)
    {
        const ULONG_PTR token = reinterpret_cast<ULONG_PTR>(current);
        if (token == 0 || !visited.emplace(token).second)
        {
            return false;
        }

        const auto record = FindWindow(current);
        if (!record)
        {
            return false;
        }

        HWND parent = nullptr;
        int width = 0;
        int height = 0;
        RECT bounds = {};
        {
            std::lock_guard<std::mutex> guard(record->lock);
            if (record->destroyed)
            {
                return false;
            }
            parent = record->parent;
            width = record->surface.Width();
            height = record->surface.Height();
            bounds = record->bounds;
            if (!parent)
            {
                rootMenuBar = record->menuBar;
            }
        }

        if (first)
        {
            *targetWidth = width;
            *targetHeight = height;
            first = false;
        }
        if (!parent)
        {
            *rootWindow = current;
            *rootWidth = width;
            *rootHeight = height;
            *targetLeft = SaturatingAdd(bounds.left, accumulatedLeft);
            *targetTop = SaturatingAdd(SaturatingAdd(bounds.top, accumulatedTop),
                rootMenuBar && window != current ? GuestMetrics::MenuHeight : 0);
            return width > 0 && height > 0 && *targetWidth > 0 && *targetHeight > 0;
        }

        accumulatedLeft = SaturatingAdd(accumulatedLeft, bounds.left);
        accumulatedTop = SaturatingAdd(accumulatedTop, bounds.top);
        const auto parentRecord = FindWindow(parent);
        if (!parentRecord) return false;
        {
            std::lock_guard<std::mutex> guard(parentRecord->lock);
            if (parentRecord->destroyed) return false;
            const GuestMetrics::NonClientMetrics parentNonClient =
                GuestMetrics::NonClientForEmbeddedWindow(
                    parentRecord->style, parentRecord->extendedStyle,
                    parentRecord->parent != nullptr);
            accumulatedLeft = SaturatingAdd(accumulatedLeft, parentNonClient.left);
            accumulatedTop = SaturatingAdd(accumulatedTop, parentNonClient.top);
        }
        current = parent;
    }
    return false;
}

HWND GuestWindowManager::HitTestGuestWindow(HWND rootWindow, int rootX, int rootY) const
{
    if (!rootWindow)
    {
        return nullptr;
    }

    struct HitTestSnapshot final
    {
        HWND handle = nullptr;
        HWND parent = nullptr;
        RECT bounds = {};
        bool visible = false;
        bool menuBar = false;
        bool topMost = false;
        int clientLeft = 0;
        int clientTop = 0;
    };

    std::vector<std::shared_ptr<WindowRecord>> records;
    {
        std::lock_guard<std::mutex> guard(m_windowsLock);
        records.reserve(m_windows.size());
        for (const auto& item : m_windows)
        {
            records.push_back(item.second);
        }
    }

    std::vector<HitTestSnapshot> windows;
    windows.reserve(records.size());
    for (const auto& record : records)
    {
        HitTestSnapshot snapshot = {};
        {
            std::lock_guard<std::mutex> guard(record->lock);
            if (record->destroyed)
            {
                continue;
            }
            snapshot.handle = record->handle;
            snapshot.parent = record->parent;
            snapshot.bounds = record->bounds;
            if (record->windowClass &&
                record->windowClass->builtinKind == BuiltinControlKind::ComboBox &&
                !record->comboDropped &&
                (record->style & 0x0003u) != ComboBoxStyleSimple)
            {
                snapshot.bounds.bottom = (std::min<LONG>)(snapshot.bounds.bottom,
                    static_cast<LONG>(SaturatingAdd(snapshot.bounds.top,
                        GuestMetrics::ControlHeightForText(GuestMetrics::TextHeight))));
            }
            snapshot.visible = record->visible;
            snapshot.menuBar = record->menuBar;
            const GuestMetrics::NonClientMetrics nonClient =
                GuestMetrics::NonClientForEmbeddedWindow(
                    record->style, record->extendedStyle,
                    record->parent != nullptr);
            snapshot.clientLeft = nonClient.left;
            snapshot.clientTop = nonClient.top;
            snapshot.topMost = record->windowClass &&
                record->windowClass->builtinKind == BuiltinControlKind::ComboBox &&
                record->comboDropped;
        }
        windows.push_back(snapshot);
    }

    std::sort(
        windows.begin(),
        windows.end(),
        [](const HitTestSnapshot& left, const HitTestSnapshot& right)
        {
            if (left.topMost != right.topMost) return !left.topMost;
            return reinterpret_cast<ULONG_PTR>(left.handle) <
                reinterpret_cast<ULONG_PTR>(right.handle);
        });

    std::unordered_set<ULONG_PTR> path;
    std::function<HWND(HWND, int, int)> hitTestChildren;
    hitTestChildren = [&windows, &path, &hitTestChildren](HWND parent, int parentX, int parentY) -> HWND
    {
        const ULONG_PTR parentToken = reinterpret_cast<ULONG_PTR>(parent);
        if (!path.emplace(parentToken).second)
        {
            return nullptr;
        }

        int clientX = parentX;
        int clientY = parentY;
        const auto parentWindow = std::find_if(windows.begin(), windows.end(),
            [parent](const HitTestSnapshot& candidate)
            {
                return candidate.handle == parent;
            });
        if (parentWindow != windows.end())
        {
            clientX -= parentWindow->clientLeft;
            clientY -= parentWindow->clientTop;
        }

        HWND hit = nullptr;
        for (auto current = windows.rbegin(); current != windows.rend(); ++current)
        {
            if (!current->visible || current->parent != parent ||
                clientX < current->bounds.left || clientY < current->bounds.top ||
                clientX >= current->bounds.right || clientY >= current->bounds.bottom)
            {
                continue;
            }

            const int childX = clientX - current->bounds.left;
            const int childY = clientY - current->bounds.top;
            hit = hitTestChildren(current->handle, childX, childY);
            if (!hit)
            {
                hit = current->handle;
            }
            break;
        }

        path.erase(parentToken);
        return hit;
    };

    const auto root = std::find_if(
        windows.begin(),
        windows.end(),
        [rootWindow](const HitTestSnapshot& window)
        {
            return window.handle == rootWindow && window.parent == nullptr && window.visible;
        });
    if (root == windows.end() || rootX < 0 || rootY < 0 ||
        rootX >= root->bounds.right - root->bounds.left ||
        rootY >= root->bounds.bottom - root->bounds.top)
    {
        return nullptr;
    }

    if (root->menuBar)
    {
        if (rootY < GuestMetrics::MenuHeight)
        {
            // Menu handling owns this non-client strip. Returning the root
            // prevents its toolbar from receiving clicks shifted by the menu.
            return rootWindow;
        }
        rootY -= GuestMetrics::MenuHeight;
    }

    const HWND child = hitTestChildren(rootWindow, rootX, rootY);
    return child ? child : rootWindow;
}

bool GuestWindowManager::HandleGuestMenuPointer(HWND rootWindow, int rootX, int rootY, UINT message)
{
    if (message != GuestAbi::WmMouseMove &&
        message != GuestAbi::WmLButtonDown && message != GuestAbi::WmLButtonUp &&
        message != GuestAbi::WmRButtonDown && message != GuestAbi::WmRButtonUp &&
        message != GuestAbi::WmMButtonDown && message != GuestAbi::WmMButtonUp)
    {
        return false;
    }
    const auto root = FindWindow(rootWindow);
    if (!root)
    {
        return false;
    }

    const auto notifyMenuClosed = [this](HWND owner)
    {
        if (owner)
            SendGuestMessage(owner, GuestAbi::WmMenuSelect,
                GuestAbi::MakeCommandWParam(0, 0xffff), 0, nullptr);
    };
    const auto notifyMenuSelection = [this](HWND owner, HMENU containingMenu,
        const GuestMenuVisualItem& item, size_t index)
    {
        if (!owner) return;
        const UINT selected = item.subMenu ? static_cast<UINT>(index) : item.identifier;
        SendGuestMessage(owner, GuestAbi::WmMenuSelect,
            GuestAbi::MakeCommandWParam(static_cast<WORD>(selected),
                static_cast<WORD>(MenuSelectFlags(item, true))),
            reinterpret_cast<LPARAM>(containingMenu), nullptr);
    };
    const auto dismissPopup = [this, root, rootWindow, &notifyMenuClosed](UINT command)
    {
        PopupMenuSession closing;
        {
            std::lock_guard<std::mutex> guard(m_popupMenuLock);
            if (!m_popupMenu.open || m_popupMenu.root != rootWindow) return;
            m_popupMenu.selectedCommand = command;
            m_popupMenu.open = false;
            closing = m_popupMenu;
        }
        {
            std::lock_guard<std::mutex> guard(root->lock);
            if (!root->destroyed) root->openMenuIndex = -1;
        }
        notifyMenuClosed(closing.owner);
        if (closing.menuBar && closing.owner)
        {
            for (auto iterator = closing.levels.rbegin(); iterator != closing.levels.rend(); ++iterator)
                SendGuestMessage(closing.owner, GuestAbi::WmUninitMenuPopup,
                    reinterpret_cast<WPARAM>(iterator->menu), 0, nullptr);
            SendGuestMessage(closing.owner, GuestAbi::WmExitMenuLoop, FALSE, 0, nullptr);
        }
        RuntimeDiagnostics::Record(command
            ? L"MENU: selected command " + std::to_wstring(command) + L"."
            : L"MENU: dismissed without a command.");
        m_popupMenuChanged.notify_all();
        InvalidateGuestRect(rootWindow, nullptr, FALSE, nullptr);
        Present(root);
        if (command && closing.menuBar && closing.notifyOwner)
        {
            PostGuestMessage(closing.owner, GuestAbi::WmCommand,
                GuestAbi::MakeCommandWParam(static_cast<WORD>(command), 0), 0, nullptr);
        }
    };

    PopupMenuSession popup;
    {
        std::lock_guard<std::mutex> guard(m_popupMenuLock);
        popup = m_popupMenu;
    }
    if (popup.open && popup.root == rootWindow)
    {
        // While a menu-bar popup is active, clicking another top-level caption
        // switches menus without first delivering the click to the guest
        // client area.
        if (popup.menuBar && rootY >= 0 && rootY < GuestMetrics::MenuHeight &&
            (message == GuestAbi::WmLButtonDown || message == GuestAbi::WmMouseMove))
        {
            std::vector<GuestMenuVisualItem> barItems = GetGuestMenuBarItems(rootWindow);
            int selected = -1;
            int left = 8;
            for (size_t index = 0; index < barItems.size(); ++index)
            {
                const int width = MenuBarItemWidth(barItems[index]);
                if (rootX >= left && rootX < SaturatingAdd(left, width))
                {
                    selected = static_cast<int>(index);
                    break;
                }
                left = SaturatingAdd(left, width);
            }
            if (selected == popup.topMenuIndex) return true;
            if (selected < 0 || !barItems[static_cast<size_t>(selected)].subMenu ||
                IsMenuItemDisabled(barItems[static_cast<size_t>(selected)]))
            {
                if (message == GuestAbi::WmLButtonDown) dismissPopup(0);
                return true;
            }
            const HMENU child = barItems[static_cast<size_t>(selected)].subMenu;
            SendGuestMessage(rootWindow, GuestAbi::WmInitMenuPopup,
                reinterpret_cast<WPARAM>(child),
                GuestAbi::MakeCommandWParam(static_cast<WORD>(selected), 0), nullptr);
            barItems = GetGuestMenuBarItems(rootWindow);
            if (static_cast<size_t>(selected) >= barItems.size() ||
                !barItems[static_cast<size_t>(selected)].subMenu)
            {
                dismissPopup(0);
                return true;
            }
            left = 8;
            for (int index = 0; index < selected; ++index)
                left = SaturatingAdd(left, MenuBarItemWidth(barItems[static_cast<size_t>(index)]));
            const int switchedWidth = PopupMenuWidth(GetGuestMenuItems(
                barItems[static_cast<size_t>(selected)].subMenu));
            {
                std::lock_guard<std::mutex> guard(root->lock);
                left = (std::max)(0, (std::min)(left,
                    (std::max)(0, root->surface.Width() - switchedWidth)));
            }
            {
                std::lock_guard<std::mutex> guard(m_popupMenuLock);
                if (m_popupMenu.open && m_popupMenu.root == rootWindow)
                {
                    m_popupMenu.menu = barItems[static_cast<size_t>(selected)].subMenu;
                    m_popupMenu.left = left;
                    m_popupMenu.top = GuestMetrics::MenuHeight;
                    m_popupMenu.topMenuIndex = selected;
                    m_popupMenu.levels.clear();
                    m_popupMenu.levels.push_back(PopupMenuLevel{
                        m_popupMenu.menu, left, GuestMetrics::MenuHeight, selected, -1 });
                }
            }
            {
                std::lock_guard<std::mutex> guard(root->lock);
                if (!root->destroyed) root->openMenuIndex = selected;
            }
            notifyMenuSelection(rootWindow, BridgeGetMenu(rootWindow),
                barItems[static_cast<size_t>(selected)], static_cast<size_t>(selected));
            InvalidateGuestRect(rootWindow, nullptr, FALSE, nullptr);
            Present(root);
            return true;
        }
        if (popup.menuBar && rootY >= 0 && rootY < GuestMetrics::MenuHeight)
        {
            // Releasing the click that opened the menu must not immediately
            // close it merely because the pointer is still over its caption.
            return true;
        }

        int hitLevel = -1;
        int hitItem = -1;
        std::vector<GuestMenuVisualItem> hitItems;
        for (size_t reverse = popup.levels.size(); reverse > 0; --reverse)
        {
            const size_t levelIndex = reverse - 1;
            const PopupMenuLevel& level = popup.levels[levelIndex];
            std::vector<GuestMenuVisualItem> items = GetGuestMenuItems(level.menu);
            const int width = PopupMenuWidth(items);
            const int height = PopupMenuHeight(items);
            if (rootX >= level.left && rootX < SaturatingAdd(level.left, width) &&
                rootY >= level.top && rootY < SaturatingAdd(level.top, height))
            {
                hitLevel = static_cast<int>(levelIndex);
                hitItem = PopupMenuItemAt(items, rootY - level.top);
                hitItems = std::move(items);
                break;
            }
        }

        if (hitLevel < 0)
        {
            if (message != GuestAbi::WmMouseMove)
                dismissPopup(0);
            return true;
        }
        if (hitItem < 0 || static_cast<size_t>(hitItem) >= hitItems.size()) return true;
        const GuestMenuVisualItem hit = hitItems[static_cast<size_t>(hitItem)];
        const PopupMenuLevel hitGeometry = popup.levels[static_cast<size_t>(hitLevel)];

        bool hotChanged = false;
        {
            std::lock_guard<std::mutex> guard(m_popupMenuLock);
            if (m_popupMenu.open && m_popupMenu.root == rootWindow &&
                static_cast<size_t>(hitLevel) < m_popupMenu.levels.size())
            {
                PopupMenuLevel& level = m_popupMenu.levels[static_cast<size_t>(hitLevel)];
                hotChanged = level.hotItem != hitItem;
                level.hotItem = hitItem;
                if ((!hit.subMenu || IsMenuItemDisabled(hit) || IsMenuItemSeparator(hit)) &&
                    m_popupMenu.levels.size() > static_cast<size_t>(hitLevel + 1))
                    m_popupMenu.levels.resize(static_cast<size_t>(hitLevel + 1));
            }
        }
        if (hotChanged)
        {
            notifyMenuSelection(popup.owner, hitGeometry.menu, hit,
                static_cast<size_t>(hitItem));
            InvalidateGuestRect(rootWindow, nullptr, FALSE, nullptr);
            Present(root);
        }

        if (IsMenuItemDisabled(hit) || IsMenuItemSeparator(hit)) return true;
        if (hit.subMenu && (message == GuestAbi::WmMouseMove || message == GuestAbi::WmLButtonUp))
        {
            bool alreadyOpen = false;
            {
                std::lock_guard<std::mutex> guard(m_popupMenuLock);
                alreadyOpen = m_popupMenu.open &&
                    m_popupMenu.levels.size() > static_cast<size_t>(hitLevel + 1) &&
                    m_popupMenu.levels[static_cast<size_t>(hitLevel + 1)].menu == hit.subMenu;
            }
            if (!alreadyOpen)
            {
                SendGuestMessage(popup.owner, GuestAbi::WmInitMenuPopup,
                    reinterpret_cast<WPARAM>(hit.subMenu),
                    GuestAbi::MakeCommandWParam(static_cast<WORD>(hitItem), 0), nullptr);
                const std::vector<GuestMenuVisualItem> refreshed = GetGuestMenuItems(hitGeometry.menu);
                if (static_cast<size_t>(hitItem) < refreshed.size() &&
                    refreshed[static_cast<size_t>(hitItem)].subMenu)
                {
                    const HMENU child = refreshed[static_cast<size_t>(hitItem)].subMenu;
                    const std::vector<GuestMenuVisualItem> childItems = GetGuestMenuItems(child);
                    if (childItems.empty()) return true;
                    int rootWidth = 0;
                    int rootHeight = 0;
                    {
                        std::lock_guard<std::mutex> rootGuard(root->lock);
                        rootWidth = root->surface.Width();
                        rootHeight = root->surface.Height();
                    }
                    const int childWidth = PopupMenuWidth(childItems);
                    int childLeft = SaturatingAdd(hitGeometry.left, PopupMenuWidth(refreshed) - 2);
                    if (childLeft + childWidth > rootWidth)
                        childLeft = (std::max)(0, hitGeometry.left - childWidth + 2);
                    const int childHeight = PopupMenuHeight(childItems);
                    const int childTop = (std::max)(0, (std::min)(
                        SaturatingAdd(hitGeometry.top, PopupMenuItemTop(
                            refreshed, static_cast<size_t>(hitItem))),
                        (std::max)(0, rootHeight - childHeight)));
                    std::lock_guard<std::mutex> guard(m_popupMenuLock);
                    if (m_popupMenu.open && m_popupMenu.root == rootWindow)
                    {
                        m_popupMenu.levels.resize(static_cast<size_t>(hitLevel + 1));
                        m_popupMenu.levels.push_back(PopupMenuLevel{
                            child, childLeft, childTop, hitItem, -1 });
                    }
                    RuntimeDiagnostics::Record(L"MENU: opened nested submenu at level " +
                        std::to_wstring(hitLevel + 1) + L".");
                    InvalidateGuestRect(rootWindow, nullptr, FALSE, nullptr);
                    Present(root);
                }
            }
            return true;
        }
        const bool activate = message == GuestAbi::WmLButtonUp ||
            (message == GuestAbi::WmRButtonUp && popup.allowRightButton);
        if (activate && hit.identifier)
        {
            dismissPopup(hit.identifier);
        }
        return true;
    }
    const std::vector<GuestMenuVisualItem> menuItems = GetGuestMenuBarItems(rootWindow);
    if (menuItems.empty())
    {
        return false;
    }

    {
        std::lock_guard<std::mutex> guard(root->lock);
        if (root->destroyed || !root->menuBar)
        {
            return false;
        }
    }

    int menuLeft = 8;
    if (rootY >= 0 && rootY < GuestMetrics::MenuHeight)
    {
        int selected = -1;
        for (size_t index = 0; index < menuItems.size(); ++index)
        {
            const int width = MenuBarItemWidth(menuItems[index]);
            if (rootX >= menuLeft && rootX < SaturatingAdd(menuLeft, width))
            {
                selected = static_cast<int>(index);
                break;
            }
            menuLeft = SaturatingAdd(menuLeft, width);
        }
        if (message == GuestAbi::WmLButtonUp && selected >= 0)
        {
            const GuestMenuVisualItem& item = menuItems[static_cast<size_t>(selected)];
            if (!item.subMenu && !IsMenuItemDisabled(item) && !IsMenuItemSeparator(item) &&
                item.identifier)
            {
                PostGuestMessage(rootWindow, GuestAbi::WmCommand,
                    GuestAbi::MakeCommandWParam(static_cast<WORD>(item.identifier), 0), 0, nullptr);
            }
            return true;
        }
        if (message == GuestAbi::WmLButtonDown)
        {
            if (selected < 0 || !menuItems[static_cast<size_t>(selected)].subMenu ||
                IsMenuItemDisabled(menuItems[static_cast<size_t>(selected)])) return true;
            HMENU openedSubMenu = menuItems[static_cast<size_t>(selected)].subMenu;
            if (openedSubMenu)
            {
                // Let the guest update enabled state and populate dynamic
                // items before the virtual popup is painted, just as USER32
                // does for every top-level menu.
                const HMENU rootMenu = BridgeGetMenu(rootWindow);
                if (rootMenu)
                {
                    SendGuestMessage(rootWindow, GuestAbi::WmEnterMenuLoop, FALSE, 0, nullptr);
                    SendGuestMessage(rootWindow, GuestAbi::WmInitMenu,
                        reinterpret_cast<WPARAM>(rootMenu), 0, nullptr);
                }
                SendGuestMessage(rootWindow, GuestAbi::WmInitMenuPopup,
                    reinterpret_cast<WPARAM>(openedSubMenu),
                    GuestAbi::MakeCommandWParam(static_cast<WORD>(selected), 0), nullptr);
                const std::vector<GuestMenuVisualItem> refreshed = GetGuestMenuBarItems(rootWindow);
                if (static_cast<size_t>(selected) >= refreshed.size() ||
                    !refreshed[static_cast<size_t>(selected)].subMenu)
                    return true;
                openedSubMenu = refreshed[static_cast<size_t>(selected)].subMenu;
                menuLeft = 8;
                for (int index = 0; index < selected; ++index)
                    menuLeft = SaturatingAdd(menuLeft,
                        MenuBarItemWidth(refreshed[static_cast<size_t>(index)]));
                const int openedWidth = PopupMenuWidth(GetGuestMenuItems(openedSubMenu));
                {
                    std::lock_guard<std::mutex> guard(root->lock);
                    menuLeft = (std::max)(0, (std::min)(menuLeft,
                        (std::max)(0, root->surface.Width() -
                            openedWidth)));
                }
                {
                    std::lock_guard<std::mutex> guard(m_popupMenuLock);
                    m_popupMenu = PopupMenuSession{};
                    m_popupMenu.menu = openedSubMenu;
                    m_popupMenu.owner = rootWindow;
                    m_popupMenu.root = rootWindow;
                    m_popupMenu.left = menuLeft;
                    m_popupMenu.top = GuestMetrics::MenuHeight;
                    m_popupMenu.open = true;
                    m_popupMenu.menuBar = true;
                    m_popupMenu.notifyOwner = true;
                    m_popupMenu.topMenuIndex = selected;
                    m_popupMenu.levels.push_back(PopupMenuLevel{
                        openedSubMenu, menuLeft, GuestMetrics::MenuHeight, selected, -1 });
                }
                {
                    std::lock_guard<std::mutex> guard(root->lock);
                    if (!root->destroyed) root->openMenuIndex = selected;
                }
                notifyMenuSelection(rootWindow, BridgeGetMenu(rootWindow),
                    refreshed[static_cast<size_t>(selected)], static_cast<size_t>(selected));
                RuntimeDiagnostics::Record(L"MENU: initialized top-level submenu " +
                    std::to_wstring(selected) + L".");
            }
            InvalidateGuestRect(rootWindow, nullptr, TRUE, nullptr);
            Present(root);
        }
        return true;
    }

    return false;
}

HWND GuestWindowManager::GetGuestParent(HWND window, DWORD* win32Error) const
{
    const auto record = FindWindow(window);
    if (!record)
    {
        SetWin32Error(win32Error, ERROR_INVALID_WINDOW_HANDLE);
        return nullptr;
    }

    std::lock_guard<std::mutex> guard(record->lock);
    if (record->destroyed)
    {
        SetWin32Error(win32Error, ERROR_INVALID_WINDOW_HANDLE);
        return nullptr;
    }
    SetWin32Error(win32Error, ERROR_SUCCESS);
    return record->parent;
}

HWND GuestWindowManager::ChildGuestWindowFromPoint(
    HWND parent,
    POINT point,
    UINT flags,
    DWORD* win32Error) const
{
    const auto parentRecord = FindWindow(parent);
    if (!parentRecord)
    {
        SetWin32Error(win32Error, ERROR_INVALID_WINDOW_HANDLE);
        return nullptr;
    }

    constexpr UINT SkipInvisible = 0x0001;
    constexpr UINT SkipDisabled = 0x0002;
    constexpr UINT SkipTransparent = 0x0004;
    constexpr DWORD ExTransparent = 0x00000020;

    struct Candidate final
    {
        HWND handle = nullptr;
        RECT bounds{};
        bool visible = false;
        bool enabled = false;
        bool transparent = false;
    };
    std::vector<Candidate> candidates;
    {
        std::lock_guard<std::mutex> guard(m_windowsLock);
        candidates.reserve(m_windows.size());
        for (const auto& item : m_windows)
        {
            const auto& record = item.second;
            std::lock_guard<std::mutex> recordGuard(record->lock);
            if (record->destroyed || record->parent != parent) continue;
            RECT bounds = record->bounds;
            if (record->windowClass &&
                record->windowClass->builtinKind == BuiltinControlKind::ComboBox &&
                !record->comboDropped &&
                (record->style & 0x0003u) != ComboBoxStyleSimple)
            {
                bounds.bottom = (std::min<LONG>)(bounds.bottom,
                    static_cast<LONG>(SaturatingAdd(bounds.top,
                        GuestMetrics::ControlHeightForText(GuestMetrics::TextHeight))));
            }
            candidates.push_back(Candidate{
                record->handle,
                bounds,
                record->visible,
                record->enabled,
                (record->extendedStyle & ExTransparent) != 0 });
        }
    }
    std::sort(candidates.begin(), candidates.end(),
        [](const Candidate& left, const Candidate& right)
        {
            return reinterpret_cast<ULONG_PTR>(left.handle) >
                reinterpret_cast<ULONG_PTR>(right.handle);
        });

    for (const auto& candidate : candidates)
    {
        if ((flags & SkipInvisible) != 0 && !candidate.visible) continue;
        if ((flags & SkipDisabled) != 0 && !candidate.enabled) continue;
        if ((flags & SkipTransparent) != 0 && candidate.transparent) continue;
        if (point.x >= candidate.bounds.left && point.y >= candidate.bounds.top &&
            point.x < candidate.bounds.right && point.y < candidate.bounds.bottom)
        {
            SetWin32Error(win32Error, ERROR_SUCCESS);
            return candidate.handle;
        }
    }

    SetWin32Error(win32Error, ERROR_SUCCESS);
    return parent;
}

HWND GuestWindowManager::GuestWindowFromPoint(POINT point, DWORD* win32Error) const
{
    struct RootCandidate final
    {
        HWND handle = nullptr;
        RECT bounds{};
        bool visible = false;
        bool enabled = false;
    };
    std::vector<RootCandidate> roots;
    {
        std::lock_guard<std::mutex> guard(m_windowsLock);
        roots.reserve(m_windows.size());
        for (const auto& item : m_windows)
        {
            const auto& record = item.second;
            std::lock_guard<std::mutex> recordGuard(record->lock);
            if (record->destroyed || record->parent) continue;
            roots.push_back(RootCandidate{
                record->handle, record->bounds, record->visible, record->enabled });
        }
    }
    std::sort(roots.begin(), roots.end(),
        [](const RootCandidate& left, const RootCandidate& right)
        {
            return reinterpret_cast<ULONG_PTR>(left.handle) >
                reinterpret_cast<ULONG_PTR>(right.handle);
        });

    for (const auto& root : roots)
    {
        if (!root.visible || !root.enabled ||
            point.x < root.bounds.left || point.y < root.bounds.top ||
            point.x >= root.bounds.right || point.y >= root.bounds.bottom)
        {
            continue;
        }
        const HWND hit = HitTestGuestWindow(
            root.handle, point.x - root.bounds.left, point.y - root.bounds.top);
        if (hit && IsGuestWindowVisibleInternal(hit) && IsGuestWindowEnabledInternal(hit))
        {
            SetWin32Error(win32Error, ERROR_SUCCESS);
            return hit;
        }
    }

    SetWin32Error(win32Error, ERROR_SUCCESS);
    return nullptr;
}

HWND GuestWindowManager::GetGuestDlgItem(HWND parent, int identifier, DWORD* win32Error) const
{
    const auto parentRecord = FindWindow(parent);
    if (!parentRecord)
    {
        SetWin32Error(win32Error, ERROR_INVALID_WINDOW_HANDLE);
        return nullptr;
    }
    {
        std::lock_guard<std::mutex> guard(parentRecord->lock);
        if (parentRecord->destroyed)
        {
            SetWin32Error(win32Error, ERROR_INVALID_WINDOW_HANDLE);
            return nullptr;
        }
    }

    // Snapshot the map before taking individual record locks. A guest
    // WM_COMMAND handler can destroy a sibling while a caller is looking it
    // up, and this avoids holding the global map lock across that re-entrant
    // window lifecycle work.
    std::vector<std::shared_ptr<WindowRecord>> records;
    {
        std::lock_guard<std::mutex> guard(m_windowsLock);
        records.reserve(m_windows.size());
        for (const auto& item : m_windows)
        {
            records.push_back(item.second);
        }
    }

    const UINT_PTR requestedId = static_cast<UINT_PTR>(identifier);
    HWND result = nullptr;
    for (const auto& record : records)
    {
        std::lock_guard<std::mutex> guard(record->lock);
        if (!record->destroyed && record->parent == parent && record->controlId == requestedId)
        {
            // Duplicate child IDs are legal even though they are usually a
            // bug. Pick the oldest bridge HWND deterministically, mirroring
            // the first matching child in the normal creation/z-order path.
            if (!result || reinterpret_cast<ULONG_PTR>(record->handle) < reinterpret_cast<ULONG_PTR>(result))
            {
                result = record->handle;
            }
        }
    }

    // GetDlgItem conventionally returns null for a missing ID without making
    // that absence look like an invalid parent window.
    SetWin32Error(win32Error, ERROR_SUCCESS);
    return result;
}

LONG GuestWindowManager::GetGuestWindowLong(HWND window, int index, DWORD* win32Error) const
{
    // The legacy *LongW APIs address four-byte entries in cbWndExtra. On
    // x64, forwarding them to the pointer-sized path would incorrectly reject
    // a perfectly valid four-byte class slot.
    if (index < 0)
    {
        return static_cast<LONG>(GetGuestWindowLongPtr(window, index, win32Error));
    }

    const auto record = FindWindow(window);
    if (!record)
    {
        SetWin32Error(win32Error, ERROR_INVALID_WINDOW_HANDLE);
        return 0;
    }

    std::lock_guard<std::mutex> guard(record->lock);
    if (!IsValidWindowExtraOffset(index, record->extraBytes.size(), sizeof(LONG)))
    {
        SetWin32Error(win32Error, ERROR_INVALID_PARAMETER);
        return 0;
    }

    LONG value = 0;
    memcpy(&value, record->extraBytes.data() + index, sizeof(value));
    SetWin32Error(win32Error, ERROR_SUCCESS);
    return value;
}

LONG GuestWindowManager::SetGuestWindowLong(HWND window, int index, LONG value, DWORD* win32Error)
{
    if (index < 0)
    {
        return static_cast<LONG>(SetGuestWindowLongPtr(
            window,
            index,
            static_cast<LONG_PTR>(value),
            win32Error));
    }

    const auto record = FindWindow(window);
    if (!record)
    {
        SetWin32Error(win32Error, ERROR_INVALID_WINDOW_HANDLE);
        return 0;
    }

    std::lock_guard<std::mutex> guard(record->lock);
    if (!IsValidWindowExtraOffset(index, record->extraBytes.size(), sizeof(LONG)))
    {
        SetWin32Error(win32Error, ERROR_INVALID_PARAMETER);
        return 0;
    }

    LONG previous = 0;
    memcpy(&previous, record->extraBytes.data() + index, sizeof(previous));
    memcpy(record->extraBytes.data() + index, &value, sizeof(value));
    SetWin32Error(win32Error, ERROR_SUCCESS);
    return previous;
}

LONG_PTR GuestWindowManager::GetGuestWindowLongPtr(HWND window, int index, DWORD* win32Error) const
{
    const auto record = FindWindow(window);
    if (!record)
    {
        SetWin32Error(win32Error, ERROR_INVALID_WINDOW_HANDLE);
        return 0;
    }

    std::lock_guard<std::mutex> guard(record->lock);
    if (index >= 0)
    {
        if (!IsValidWindowExtraOffset(index, record->extraBytes.size(), sizeof(LONG_PTR)))
        {
            SetWin32Error(win32Error, ERROR_INVALID_PARAMETER);
            return 0;
        }

        LONG_PTR value = 0;
        memcpy(&value, record->extraBytes.data() + index, sizeof(value));
        SetWin32Error(win32Error, ERROR_SUCCESS);
        return value;
    }
    switch (index)
    {
    case GuestAbi::GwlStyle:
        SetWin32Error(win32Error, ERROR_SUCCESS);
        return static_cast<LONG_PTR>(record->style);
    case GuestAbi::GwlExStyle:
        SetWin32Error(win32Error, ERROR_SUCCESS);
        return static_cast<LONG_PTR>(record->extendedStyle);
    case GuestAbi::GwlpUserData:
        SetWin32Error(win32Error, ERROR_SUCCESS);
        return record->userData;
    case GuestAbi::GwlpHInstance:
        SetWin32Error(win32Error, ERROR_SUCCESS);
        return reinterpret_cast<LONG_PTR>(record->instance);
    case GuestAbi::GwlpHwndParent:
        SetWin32Error(win32Error, ERROR_SUCCESS);
        return reinterpret_cast<LONG_PTR>(record->parent);
    case GuestAbi::GwlpId:
        SetWin32Error(win32Error, ERROR_SUCCESS);
        return static_cast<LONG_PTR>(record->controlId);
    case GuestAbi::GwlpWndProc:
        SetWin32Error(win32Error, ERROR_SUCCESS);
        return record->procedure
            ? reinterpret_cast<LONG_PTR>(record->procedure)
            : (record->windowClass && record->windowClass->builtinKind != BuiltinControlKind::None
                ? reinterpret_cast<LONG_PTR>(&BridgeBuiltinControlWindowProc)
                : 0);
    default:
        SetWin32Error(win32Error, ERROR_INVALID_PARAMETER);
        return 0;
    }
}

LONG_PTR GuestWindowManager::SetGuestWindowLongPtr(HWND window, int index, LONG_PTR value, DWORD* win32Error)
{
    const auto record = FindWindow(window);
    if (!record)
    {
        SetWin32Error(win32Error, ERROR_INVALID_WINDOW_HANDLE);
        return 0;
    }

    HWND requestedParent = nullptr;
    if (index == GuestAbi::GwlpHwndParent)
    {
        requestedParent = reinterpret_cast<HWND>(value);
        if (requestedParent && (!IsGuestWindow(requestedParent) || WouldCreateParentCycle(window, requestedParent)))
        {
            SetWin32Error(win32Error, ERROR_INVALID_WINDOW_HANDLE);
            return 0;
        }
    }
    else if (index == GuestAbi::GwlpWndProc && value == 0)
    {
        SetWin32Error(win32Error, ERROR_INVALID_PARAMETER);
        return 0;
    }

    LONG_PTR previous = 0;
    bool becameVisible = false;
    bool becameHidden = false;
    bool becameDisabled = false;
    bool styleChanged = false;
    bool subclassedBuiltin = false;
    {
        std::lock_guard<std::mutex> guard(record->lock);
        if (record->destroyed)
        {
            SetWin32Error(win32Error, ERROR_INVALID_WINDOW_HANDLE);
            return 0;
        }

        if (index >= 0)
        {
            if (!IsValidWindowExtraOffset(index, record->extraBytes.size(), sizeof(LONG_PTR)))
            {
                SetWin32Error(win32Error, ERROR_INVALID_PARAMETER);
                return 0;
            }

            memcpy(&previous, record->extraBytes.data() + index, sizeof(previous));
            memcpy(record->extraBytes.data() + index, &value, sizeof(value));
        }
        else
        {
            switch (index)
            {
            case GuestAbi::GwlStyle:
            {
                previous = static_cast<LONG_PTR>(record->style);
                const bool wasVisible = record->visible;
                const bool wasEnabled = record->enabled;
                record->style = static_cast<DWORD>(value);
                // The bridge keeps explicit state for safe input and paint
                // routing. Keep it in sync when a normal Win32 client changes
                // WS_VISIBLE/WS_DISABLED through SetWindowLongPtr.
                record->visible = (record->style & GuestWsVisible) != 0;
                record->enabled = (record->style & GuestWsDisabled) == 0;
                becameVisible = !wasVisible && record->visible;
                becameHidden = wasVisible && !record->visible;
                becameDisabled = wasEnabled && !record->enabled;
                styleChanged = true;
                break;
            }
            case GuestAbi::GwlExStyle:
                previous = static_cast<LONG_PTR>(record->extendedStyle);
                record->extendedStyle = static_cast<DWORD>(value);
                break;
            case GuestAbi::GwlpUserData:
                previous = record->userData;
                record->userData = value;
                break;
            case GuestAbi::GwlpHInstance:
                previous = reinterpret_cast<LONG_PTR>(record->instance);
                record->instance = reinterpret_cast<HINSTANCE>(value);
                break;
            case GuestAbi::GwlpHwndParent:
                previous = reinterpret_cast<LONG_PTR>(record->parent);
                record->parent = requestedParent;
                break;
            case GuestAbi::GwlpId:
                previous = static_cast<LONG_PTR>(record->controlId);
                record->controlId = static_cast<UINT_PTR>(value);
                break;
            case GuestAbi::GwlpWndProc:
                previous = record->procedure
                    ? reinterpret_cast<LONG_PTR>(record->procedure)
                    : (record->windowClass && record->windowClass->builtinKind != BuiltinControlKind::None
                        ? reinterpret_cast<LONG_PTR>(&BridgeBuiltinControlWindowProc)
                        : 0);
                record->procedure = reinterpret_cast<GuestAbi::WndProc>(value);
                subclassedBuiltin = record->windowClass &&
                    record->windowClass->builtinKind != BuiltinControlKind::None;
                break;
            default:
                SetWin32Error(win32Error, ERROR_INVALID_PARAMETER);
                return 0;
            }
        }
    }

    if (styleChanged)
    {
        // SetWindowLongPtr does not synthesize WM_SHOWWINDOW or WM_ENABLE on
        // desktop Windows. The bridge likewise keeps this transition silent,
        // but must retire stale host-facing focus/capture state immediately.
        if (becameHidden || becameDisabled)
        {
            ClearGuestCaptureForSubtree(window);
            ClearGuestFocusForSubtree(window);
        }
        if (becameHidden)
        {
            ClearGuestForegroundForSubtree(window);
            Present(record);
        }
        if (becameVisible)
        {
            InvalidateGuestRect(window, nullptr, TRUE, nullptr);
            if (IsGuestWindowVisibleInternal(window))
            {
                Present(record);
            }
        }
    }

    if (subclassedBuiltin)
    {
        RuntimeDiagnostics::Record(
            L"CONTROL SUBCLASS: handle " + std::to_wstring(reinterpret_cast<ULONG_PTR>(window)) +
            L" retained its bridge default procedure.");
    }

    SetWin32Error(win32Error, ERROR_SUCCESS);
    return previous;
}

BOOL GuestWindowManager::SetGuestWindowPos(
    HWND window,
    HWND insertAfter,
    int x,
    int y,
    int width,
    int height,
    UINT flags,
    DWORD* win32Error)
{
    const auto record = FindWindow(window);
    if (!record)
    {
        SetWin32Error(win32Error, ERROR_INVALID_WINDOW_HANDLE);
        return FALSE;
    }

    GuestWindowPosition requested{
        window, insertAfter, x, y, width, height, flags };
    if ((flags & GuestSwpNoSendChanging) == 0)
    {
        CallWindowProcedure(record, GuestAbi::WmWindowPosChanging, 0,
            reinterpret_cast<LPARAM>(&requested));
    }

    insertAfter = requested.insertAfter;
    x = requested.x;
    y = requested.y;
    width = requested.cx;
    height = requested.cy;
    flags = requested.flags;
    const bool resize = (flags & GuestSwpNoSize) == 0;
    const bool move = (flags & GuestSwpNoMove) == 0;
    if (resize && (width < 0 || height < 0))
    {
        SetWin32Error(win32Error, ERROR_INVALID_PARAMETER);
        return FALSE;
    }

    // The composed guest desktop has deterministic creation-order layering.
    // insertAfter needs a dedicated sibling z-order table before it can alter
    // that layer order; position and size are retained now for layout and
    // child-surface composition.
    (void)insertAfter;
    const bool show = (flags & GuestSwpShowWindow) != 0;
    const bool hide = !show && (flags & GuestSwpHideWindow) != 0;
    bool changedSize = false;
    bool becameVisible = false;
    bool becameHidden = false;
    bool enabled = false;
    HWND parent = nullptr;
    int currentWidth = 0;
    int currentHeight = 0;
    int currentLeft = 0;
    int currentTop = 0;
    int previousWidth = 0;
    int previousHeight = 0;
    int previousLeft = 0;
    int previousTop = 0;
    std::wstring className;
    bool resizedRebar = false;

    {
        std::lock_guard<std::mutex> guard(record->lock);
        if (record->destroyed)
        {
            SetWin32Error(win32Error, ERROR_INVALID_WINDOW_HANDLE);
            return FALSE;
        }

        const int oldWidth = record->surface.Width();
        const int oldHeight = record->surface.Height();
        previousWidth = oldWidth;
        previousHeight = oldHeight;
        previousLeft = record->bounds.left;
        previousTop = record->bounds.top;
        className = record->windowClass ? record->windowClass->name : L"<unknown>";
        const bool hostedRoot = record->parent == nullptr;
        const int newWidth = resize ? SetWindowPosExtent(width) : oldWidth;
        int newHeight = resize ? SetWindowPosExtent(height) : oldHeight;
        if (resize && record->windowClass &&
            record->windowClass->builtinKind == BuiltinControlKind::Rebar &&
            !record->rebarBands.empty())
        {
            newHeight = RequiredRebarHeight(record->rebarBands, newWidth);
        }
        const int newLeft = move ? x : record->bounds.left;
        const int newTop = move ? y : record->bounds.top;

        // An explicit SetWindowPos/MoveWindow detaches that dimension from the
        // host viewport. Only CW_USEDEFAULT-created roots continue following a
        // later host resize automatically.
        if (hostedRoot && resize)
        {
            record->viewportWidthBound = false;
            record->viewportHeightBound = false;
        }

        if (resize && (newWidth != oldWidth || newHeight != oldHeight))
        {
            if (!record->surface.Resize(newWidth, newHeight, MiniGdi::OpaqueWhite))
            {
                SetWin32Error(win32Error, ERROR_NOT_ENOUGH_MEMORY);
                return FALSE;
            }
            m_gdi.ResetClip(record->dc);
            // A damage rectangle expressed against the old surface cannot be
            // reused after resize. The normal full invalidation below creates
            // a fresh client-sized update region and leaves any old queued
            // WM_PAINT harmlessly coalesced.
            record->invalidated = false;
            record->erasePending = false;
            record->updateRect = RECT{};
            changedSize = true;
        }

        if (move || changedSize)
        {
            record->bounds.left = newLeft;
            record->bounds.top = newTop;
            record->bounds.right = SaturatingAdd(newLeft, newWidth);
            record->bounds.bottom = SaturatingAdd(newTop, newHeight);
        }

        const bool wasVisible = record->visible;
        if (show)
        {
            record->visible = true;
            record->style |= GuestWsVisible;
        }
        else if (hide)
        {
            record->visible = false;
            record->style &= ~GuestWsVisible;
        }
        becameVisible = !wasVisible && record->visible;
        becameHidden = wasVisible && !record->visible;
        enabled = record->enabled;
        parent = record->parent;
        currentWidth = record->surface.Width();
        currentHeight = record->surface.Height();
        currentLeft = record->bounds.left;
        currentTop = record->bounds.top;
        resizedRebar = changedSize && record->windowClass &&
            record->windowClass->builtinKind == BuiltinControlKind::Rebar;
    }

    if (resizedRebar)
    {
        LayoutGuestRebar(window);
    }

    if (becameVisible)
    {
        if (!parent && (flags & GuestSwpNoActivate) == 0)
        {
            m_foregroundWindow.store(reinterpret_cast<ULONG_PTR>(window));
        }
        CallWindowProcedure(record, GuestAbi::WmShowWindow, TRUE, 0);
        if ((flags & GuestSwpNoActivate) == 0 && enabled)
        {
            DWORD ignored = ERROR_SUCCESS;
            SetGuestFocus(window, &ignored);
        }
    }
    else if (becameHidden)
    {
        ClearGuestCaptureForSubtree(window);
        ClearGuestFocusForSubtree(window);
        ClearGuestForegroundForSubtree(window);
        CallWindowProcedure(record, GuestAbi::WmShowWindow, FALSE, 0);
    }

    if (move || changedSize)
    {
        RuntimeDiagnostics::Record(
            L"WINDOW LAYOUT: handle " + std::to_wstring(reinterpret_cast<ULONG_PTR>(window)) +
            L" (" + className + L", parent " +
            std::to_wstring(reinterpret_cast<ULONG_PTR>(parent)) + L", flags 0x" +
            [&flags]()
            {
                wchar_t buffer[16]{};
                swprintf_s(buffer, L"%08X", flags);
                return std::wstring(buffer);
            }() + L") " +
            std::to_wstring(previousWidth) + L"x" + std::to_wstring(previousHeight) +
            L" at " + std::to_wstring(previousLeft) + L"," + std::to_wstring(previousTop) +
            L" -> " + std::to_wstring(currentWidth) + L"x" + std::to_wstring(currentHeight) +
            L" at " + std::to_wstring(currentLeft) + L"," + std::to_wstring(currentTop) + L".");
    }

    const bool changedPosition = move &&
        (currentLeft != previousLeft || currentTop != previousTop);
    if (changedSize || changedPosition || becameVisible || becameHidden ||
        (flags & GuestSwpFrameChanged) != 0)
    {
        GuestWindowPosition completed{
            window,
            insertAfter,
            currentLeft,
            currentTop,
            currentWidth,
            currentHeight,
            flags };
        CallWindowProcedure(record, GuestAbi::WmWindowPosChanged, 0,
            reinterpret_cast<LPARAM>(&completed));
    }

    if (becameVisible && !parent)
    {
        // A top-level window can be shown through SetWindowPos or
        // SetWindowPlacement without passing through ShowWindow.  Do not
        // depend on the guest procedure forwarding WM_WINDOWPOSCHANGED to
        // DefWindowProc before its first frame: finish the initial client
        // layout synchronously, with menu/non-client chrome excluded.
        RECT client{};
        if (GetGuestClientRect(window, &client, nullptr))
        {
            const int clientWidth = (std::max)(0L, client.right - client.left);
            const int clientHeight = (std::max)(0L, client.bottom - client.top);
            CallWindowProcedure(record, GuestAbi::WmSize, GuestAbi::SizeRestored,
                GuestAbi::MakeMouseLParam(
                    static_cast<WORD>((std::min)(clientWidth, 0xffff)),
                    static_cast<WORD>((std::min)(clientHeight, 0xffff))));
        }
    }

    if ((changedSize || becameVisible || (flags & GuestSwpFrameChanged) != 0) &&
        (flags & GuestSwpNoRedraw) == 0)
    {
        InvalidateGuestRect(window, nullptr, TRUE, nullptr);
        if (IsGuestWindowVisibleInternal(window))
        {
            Present(record);
        }
    }
    else if (becameHidden && (flags & GuestSwpNoRedraw) == 0)
    {
        // A hidden child has no paint of its own, but removing its composed
        // layer still changes the visible root desktop.
        Present(record);
    }

    SetWin32Error(win32Error, ERROR_SUCCESS);
    return TRUE;
}

UINT_PTR GuestWindowManager::SetGuestTimer(
    HWND window,
    UINT_PTR timerId,
    UINT elapseMilliseconds,
    GuestAbi::TimerProc timerProcedure,
    DWORD* win32Error)
{
    // A guest TIMERPROC would otherwise run from a UWP thread-pool callback,
    // outside the guest's runtime scope and without its normal message-loop
    // serialization. Preserve safe USER32-style WM_TIMER delivery instead.
    (void)timerProcedure;

    if (window)
    {
        const auto record = FindWindow(window);
        if (!record)
        {
            SetWin32Error(win32Error, ERROR_INVALID_WINDOW_HANDLE);
            return 0;
        }
        std::lock_guard<std::mutex> guard(record->lock);
        if (record->destroyed)
        {
            SetWin32Error(win32Error, ERROR_INVALID_WINDOW_HANDLE);
            return 0;
        }
    }

    const auto timers = m_timers;
    if (!timers || !m_active.load())
    {
        SetWin32Error(win32Error, ERROR_INVALID_FUNCTION);
        return 0;
    }

    ThreadPoolTimer^ replaced = nullptr;
    UINT_PTR resolvedId = timerId;
    ThreadPoolTimer^ scheduled = nullptr;
    try
    {
        std::lock_guard<std::mutex> guard(timers->lock);
        if (!timers->active || timers->owner != this || !m_active.load())
        {
            SetWin32Error(win32Error, ERROR_INVALID_FUNCTION);
            return 0;
        }

        if (resolvedId == 0)
        {
            // Returning zero conventionally means failure, so allocate a
            // stable nonzero ID for either a window or a thread timer.
            resolvedId = AllocateGuestTimerIdLocked(*timers, window);
            if (resolvedId == 0)
            {
                SetWin32Error(win32Error, ERROR_NOT_ENOUGH_QUOTA);
                return 0;
            }
        }

        const GuestTimerKey key = MakeGuestTimerKey(window, resolvedId);
        const auto existing = timers->entries.find(key);
        if (existing == timers->entries.end() && timers->entries.size() >= MaximumGuestTimers)
        {
            SetWin32Error(win32Error, ERROR_NOT_ENOUGH_QUOTA);
            return 0;
        }

        std::uint64_t generation = ++timers->nextGeneration;
        if (generation == 0)
        {
            generation = ++timers->nextGeneration;
        }

        TimeSpan period = {};
        period.Duration = static_cast<INT64>(ClampGuestTimerPeriod(elapseMilliseconds)) * 10000;
        const std::weak_ptr<GuestTimerState> weakTimers(timers);
        scheduled = ThreadPoolTimer::CreatePeriodicTimer(
            ref new TimerElapsedHandler([weakTimers, key, generation](ThreadPoolTimer^)
        {
            const auto state = weakTimers.lock();
            if (state)
            {
                state->Deliver(key, generation);
            }
        }), period);
        if (!scheduled)
        {
            SetWin32Error(win32Error, ERROR_NOT_ENOUGH_MEMORY);
            return 0;
        }

        GuestTimerEntry replacement = {};
        replacement.window = window;
        replacement.timerId = resolvedId;
        replacement.generation = generation;
        replacement.timer = scheduled;
        if (existing != timers->entries.end())
        {
            replaced = existing->second.timer;
            existing->second = replacement;
        }
        else
        {
            timers->entries.emplace(key, replacement);
        }
    }
    catch (...)
    {
        if (scheduled)
        {
            try
            {
                scheduled->Cancel();
            }
            catch (...)
            {
            }
        }
        SetWin32Error(win32Error, ERROR_NOT_ENOUGH_MEMORY);
        return 0;
    }

    if (replaced)
    {
        CancelThreadPoolTimer(replaced);
    }
    SetWin32Error(win32Error, ERROR_SUCCESS);
    return resolvedId;
}

BOOL GuestWindowManager::KillGuestTimer(HWND window, UINT_PTR timerId, DWORD* win32Error)
{
    if (timerId == 0)
    {
        SetWin32Error(win32Error, ERROR_NOT_FOUND);
        return FALSE;
    }

    if (window)
    {
        const auto record = FindWindow(window);
        if (!record)
        {
            SetWin32Error(win32Error, ERROR_INVALID_WINDOW_HANDLE);
            return FALSE;
        }
        std::lock_guard<std::mutex> guard(record->lock);
        if (record->destroyed)
        {
            SetWin32Error(win32Error, ERROR_INVALID_WINDOW_HANDLE);
            return FALSE;
        }
    }

    const auto timers = m_timers;
    if (!timers || !m_active.load())
    {
        SetWin32Error(win32Error, ERROR_INVALID_FUNCTION);
        return FALSE;
    }

    ThreadPoolTimer^ cancelledTimer = nullptr;
    {
        std::lock_guard<std::mutex> guard(timers->lock);
        if (!timers->active || timers->owner != this || !m_active.load())
        {
            SetWin32Error(win32Error, ERROR_INVALID_FUNCTION);
            return FALSE;
        }

        const auto found = timers->entries.find(MakeGuestTimerKey(window, timerId));
        if (found == timers->entries.end())
        {
            SetWin32Error(win32Error, ERROR_NOT_FOUND);
            return FALSE;
        }

        cancelledTimer = found->second.timer;
        timers->entries.erase(found);
    }

    if (cancelledTimer)
    {
        CancelThreadPoolTimer(cancelledTimer);
    }
    SetWin32Error(win32Error, ERROR_SUCCESS);
    return TRUE;
}

LRESULT GuestWindowManager::BuiltinControlProcedure(
    const std::shared_ptr<WindowRecord>& window,
    UINT message,
    WPARAM wParam,
    LPARAM lParam)
{
    if (!window)
    {
        return 0;
    }

    BuiltinControlKind controlKind = BuiltinControlKind::None;
    {
        std::lock_guard<std::mutex> guard(window->lock);
        if (window->destroyed || !window->windowClass)
        {
            return 0;
        }
        controlKind = window->windowClass->builtinKind;
    }
    if (controlKind == BuiltinControlKind::None)
    {
        return 0;
    }

    // Snapshot parent-notification data before calling guest code because a
    // WM_COMMAND handler may destroy or reparent this control.
    const auto notifyParent = [this, window](WORD notification)
    {
        HWND parent = nullptr;
        HWND handle = nullptr;
        UINT_PTR identifier = 0;
        {
            std::lock_guard<std::mutex> guard(window->lock);
            if (window->destroyed) return;
            parent = window->parent;
            handle = window->handle;
            identifier = window->controlId;
        }
        if (parent)
        {
            SendGuestMessage(parent, GuestAbi::WmCommand,
                GuestAbi::MakeCommandWParam(static_cast<WORD>(identifier), notification),
                reinterpret_cast<LPARAM>(handle), nullptr);
        }
    };

    const auto invalidate = [this, window]()
    {
        HWND handle = nullptr;
        {
            std::lock_guard<std::mutex> guard(window->lock);
            if (window->destroyed) return;
            handle = window->handle;
        }
        InvalidateGuestRect(handle, nullptr, TRUE, nullptr);
    };

    // Multiline EDIT controls expose their backing local-memory buffer through
    // EM_GETHANDLE.  Keep the ordinary std::wstring representation used by the
    // renderer synchronized with that Win32-compatible buffer.
    const auto syncEditHandleFromTitle = [window]()
    {
        HLOCAL handle = nullptr;
        std::wstring text;
        {
            std::lock_guard<std::mutex> guard(window->lock);
            if (window->destroyed || !window->editTextHandle) return;
            handle = window->editTextHandle;
            text = window->title;
        }
        const SIZE_T bytes = BridgeLocalSize(handle);
        auto* destination = static_cast<wchar_t*>(BridgeLocalLock(handle));
        if (!destination || bytes < sizeof(wchar_t)) return;
        const size_t capacity = bytes / sizeof(wchar_t);
        const size_t copied = (std::min)(text.size(), capacity - 1);
        if (copied) memcpy(destination, text.data(), copied * sizeof(wchar_t));
        destination[copied] = L'\0';
        BridgeLocalUnlock(handle);
    };

    const auto syncEditTitleFromHandle = [window]()
    {
        HLOCAL handle = nullptr;
        {
            std::lock_guard<std::mutex> guard(window->lock);
            if (window->destroyed || !window->editTextHandle) return;
            handle = window->editTextHandle;
        }
        const SIZE_T bytes = BridgeLocalSize(handle);
        auto* source = static_cast<const wchar_t*>(BridgeLocalLock(handle));
        if (!source || bytes < sizeof(wchar_t)) return;
        const size_t capacity = bytes / sizeof(wchar_t);
        size_t length = 0;
        while (length < capacity && source[length]) ++length;
        std::wstring text(source, length);
        BridgeLocalUnlock(handle);
        {
            std::lock_guard<std::mutex> guard(window->lock);
            if (window->destroyed || window->editTextHandle != handle) return;
            window->title = std::move(text);
            window->editCaret = (std::min)(window->editCaret, window->title.size());
            window->editSelectionAnchor = (std::min)(
                window->editSelectionAnchor, window->title.size());
            window->editSelectionEnd = (std::min)(
                window->editSelectionEnd, window->title.size());
        }
    };

    if (message == CommonControlSetUnicodeFormat)
    {
        std::lock_guard<std::mutex> guard(window->lock);
        const bool previous = window->commonControlUnicode;
        window->commonControlUnicode = wParam != FALSE;
        return previous ? TRUE : FALSE;
    }
    if (message == CommonControlGetUnicodeFormat)
    {
        std::lock_guard<std::mutex> guard(window->lock);
        return window->commonControlUnicode ? TRUE : FALSE;
    }

    const bool textEditor = controlKind == BuiltinControlKind::Edit ||
        controlKind == BuiltinControlKind::ComboBox;
    if (controlKind == BuiltinControlKind::Edit && message == EditGetHandle)
    {
        std::lock_guard<std::mutex> guard(window->lock);
        if (window->destroyed ||
            (window->style & GuestAbi::EsMultiline) == 0)
            return 0;
        if (!window->editTextHandle)
        {
            const size_t characters = MaximumBuiltinControlTextLength + 1;
            HLOCAL handle = BridgeLocalAlloc(
                LMEM_MOVEABLE | LMEM_ZEROINIT,
                characters * sizeof(wchar_t));
            if (!handle) return 0;
            auto* destination = static_cast<wchar_t*>(BridgeLocalLock(handle));
            if (!destination)
            {
                BridgeLocalFree(handle);
                return 0;
            }
            const size_t copied = (std::min)(window->title.size(), characters - 1);
            if (copied)
                memcpy(destination, window->title.data(), copied * sizeof(wchar_t));
            destination[copied] = L'\0';
            BridgeLocalUnlock(handle);
            window->editTextHandle = handle;
        }
        RuntimeDiagnostics::Record(L"EDIT: EM_GETHANDLE returned a virtual local buffer.");
        return reinterpret_cast<LRESULT>(window->editTextHandle);
    }
    if (controlKind == BuiltinControlKind::Edit && message == EditSetHandle)
    {
        HLOCAL handle = reinterpret_cast<HLOCAL>(wParam);
        if (!handle) return 0;
        const SIZE_T bytes = BridgeLocalSize(handle);
        auto* source = static_cast<const wchar_t*>(BridgeLocalLock(handle));
        if (!source || bytes < sizeof(wchar_t)) return 0;
        const size_t capacity = bytes / sizeof(wchar_t);
        size_t length = 0;
        while (length < capacity && source[length]) ++length;
        std::wstring text(source, length);
        BridgeLocalUnlock(handle);
        {
            std::lock_guard<std::mutex> guard(window->lock);
            if (window->destroyed ||
                (window->style & GuestAbi::EsMultiline) == 0)
                return 0;
            window->editTextHandle = handle;
            window->title = std::move(text);
            window->editCaret = 0;
            window->editSelectionAnchor = 0;
            window->editSelectionEnd = 0;
            window->editFirstVisibleCharacter = 0;
            window->editFirstVisibleLine = 0;
            window->editHorizontalOffset = 0;
        }
        invalidate();
        return 0;
    }
    if (textEditor && message == EditGetSelection)
    {
        size_t start = 0;
        size_t end = 0;
        {
            std::lock_guard<std::mutex> guard(window->lock);
            start = (std::min)(window->editSelectionAnchor, window->editSelectionEnd);
            end = (std::max)(window->editSelectionAnchor, window->editSelectionEnd);
        }
        const DWORD start32 = static_cast<DWORD>((std::min)(start,
            static_cast<size_t>((std::numeric_limits<DWORD>::max)())));
        const DWORD end32 = static_cast<DWORD>((std::min)(end,
            static_cast<size_t>((std::numeric_limits<DWORD>::max)())));
        if (wParam) TryWriteGuestValue(reinterpret_cast<DWORD*>(wParam), start32);
        if (lParam) TryWriteGuestValue(reinterpret_cast<DWORD*>(lParam), end32);
        return MAKELONG(static_cast<WORD>((std::min)(start32,
            static_cast<DWORD>(0xffff))),
            static_cast<WORD>((std::min)(end32, static_cast<DWORD>(0xffff))));
    }
    if (textEditor && message == EditSetSelection)
    {
        {
            std::lock_guard<std::mutex> guard(window->lock);
            const size_t length = window->title.size();
            const LONG requestedStart = static_cast<LONG>(wParam);
            const LONG requestedEnd = static_cast<LONG>(lParam);
            if (requestedStart == -1)
            {
                window->editSelectionAnchor = window->editCaret;
                window->editSelectionEnd = window->editCaret;
            }
            else
            {
                const size_t start = (std::min)(static_cast<size_t>((std::max)(0L, requestedStart)), length);
                const size_t end = requestedEnd == -1
                    ? length
                    : (std::min)(static_cast<size_t>((std::max)(0L, requestedEnd)), length);
                window->editSelectionAnchor = start;
                window->editSelectionEnd = end;
                window->editCaret = end;
            }
        }
        invalidate();
        return 0;
    }
    if (textEditor && (message == EditGetLineCount || message == EditLineIndex ||
        message == EditLineLength || message == EditGetLine ||
        message == EditLineFromCharacter || message == EditGetFirstVisibleLine))
    {
        std::wstring text;
        size_t caret = 0;
        size_t firstVisibleLine = 0;
        int characterWidth = GuestMetrics::TextWidth;
        int availableWidth = 1;
        bool wrap = false;
        {
            std::lock_guard<std::mutex> guard(window->lock);
            if (window->destroyed) return 0;
            text = window->title;
            caret = (std::min)(window->editCaret, text.size());
            firstVisibleLine = window->editFirstVisibleLine;
            characterWidth = (std::max)(1, window->controlTextWidth);
            availableWidth = (std::max)(1, window->surface.Width() - 4 -
                ((window->style & 0x00200000u /* WS_VSCROLL */) != 0
                    ? GuestMetrics::ScrollBarExtent : 0));
            wrap = controlKind == BuiltinControlKind::Edit &&
                (window->style & GuestAbi::EsMultiline) != 0 &&
                (window->style & GuestAbi::EsAutoHScroll) == 0 &&
                (window->style & 0x00100000u /* WS_HSCROLL */) == 0;
        }
        const auto lines = BuildEditVisualLines(text, wrap, availableWidth,
            [characterWidth](const wchar_t*, size_t count)
        {
            return static_cast<int>((std::min)(count, static_cast<size_t>(
                (std::numeric_limits<int>::max)() / characterWidth))) * characterWidth;
        });

        if (message == EditGetLineCount)
            return static_cast<LRESULT>((std::min)(lines.size(), static_cast<size_t>(
                (std::numeric_limits<LRESULT>::max)())));
        if (message == EditGetFirstVisibleLine)
            return static_cast<LRESULT>((std::min)(firstVisibleLine, lines.size() - 1));

        if (message == EditLineIndex)
        {
            const LRESULT requested = static_cast<LRESULT>(wParam);
            const size_t line = requested < 0
                ? EditLineForPosition(lines, caret)
                : static_cast<size_t>(requested);
            return line < lines.size() ? static_cast<LRESULT>(lines[line].start) : -1;
        }

        const LRESULT requestedCharacter = static_cast<LRESULT>(wParam);
        const size_t character = requestedCharacter < 0
            ? caret
            : (std::min)(static_cast<size_t>(requestedCharacter), text.size());
        const size_t line = EditLineForPosition(lines, character);
        if (message == EditLineFromCharacter)
            return static_cast<LRESULT>(line);
        if (message == EditLineLength)
            return static_cast<LRESULT>(lines[line].end - lines[line].start);

        if (message == EditGetLine)
        {
            WCHAR* destination = reinterpret_cast<WCHAR*>(lParam);
            WCHAR capacityValue = 0;
            if (!destination || !TryReadGuestValue(destination, &capacityValue) ||
                capacityValue == 0)
                return 0;
            const WORD capacity = static_cast<WORD>(capacityValue);
            const size_t requestedLine = static_cast<size_t>(wParam);
            if (requestedLine >= lines.size()) return 0;
            const EditVisualLine& source = lines[requestedLine];
            const size_t copied = (std::min)(source.end - source.start,
                static_cast<size_t>(capacity));
            for (size_t index = 0; index < copied; ++index)
            {
                const WCHAR characterValue = text[source.start + index];
                if (!TryWriteGuestValue(destination + index, characterValue)) return 0;
            }
            return static_cast<LRESULT>(copied);
        }
    }
    if (textEditor && message == EditReplaceSelection)
    {
        std::wstring replacement;
        if (lParam && !TryReadGuestWideString(reinterpret_cast<LPCWSTR>(lParam), &replacement))
            return FALSE;
        bool changed = false;
        {
            std::lock_guard<std::mutex> guard(window->lock);
            if (window->destroyed || (window->style & GuestAbi::EsReadOnly) != 0)
                return FALSE;
            size_t start = (std::min)(window->editSelectionAnchor, window->editSelectionEnd);
            size_t end = (std::max)(window->editSelectionAnchor, window->editSelectionEnd);
            start = (std::min)(start, window->title.size());
            end = (std::min)(end, window->title.size());
            const size_t capacity = window->editTextLimit > window->title.size() - (end - start)
                ? window->editTextLimit - (window->title.size() - (end - start))
                : 0;
            if (replacement.size() > capacity) replacement.resize(capacity);
            window->title.replace(start, end - start, replacement);
            window->editCaret = start + replacement.size();
            window->editSelectionAnchor = window->editCaret;
            window->editSelectionEnd = window->editCaret;
            changed = end != start || !replacement.empty();
        }
        if (changed)
        {
            if (controlKind == BuiltinControlKind::Edit) syncEditHandleFromTitle();
            invalidate();
            notifyParent(GuestAbi::EnChange);
        }
        return TRUE;
    }
    if (textEditor && message == EditLimitText)
    {
        std::lock_guard<std::mutex> guard(window->lock);
        window->editTextLimit = wParam == 0
            ? MaximumBuiltinControlTextLength
            : (std::min)(static_cast<size_t>(wParam), MaximumBuiltinControlTextLength);
        return TRUE;
    }
    if (textEditor && message == EditGetLimitText)
    {
        std::lock_guard<std::mutex> guard(window->lock);
        return static_cast<LRESULT>(window->editTextLimit);
    }
    if (controlKind == BuiltinControlKind::Edit &&
        message == EditSetPasswordCharacter)
    {
        {
            std::lock_guard<std::mutex> guard(window->lock);
            window->editPasswordCharacter = static_cast<wchar_t>(wParam);
        }
        invalidate();
        return 0;
    }
    if (controlKind == BuiltinControlKind::Edit &&
        message == EditGetPasswordCharacter)
    {
        std::lock_guard<std::mutex> guard(window->lock);
        return static_cast<LRESULT>(window->editPasswordCharacter);
    }
    if (textEditor && message == EditSetReadOnly)
    {
        {
            std::lock_guard<std::mutex> guard(window->lock);
            if (wParam) window->style |= GuestAbi::EsReadOnly;
            else window->style &= ~GuestAbi::EsReadOnly;
        }
        invalidate();
        return TRUE;
    }

    if (controlKind == BuiltinControlKind::Rebar &&
        (message == GuestAbi::WmCommand || message == GuestAbi::WmNotify))
    {
        HWND parent = nullptr;
        {
            std::lock_guard<std::mutex> guard(window->lock);
            parent = window->parent;
        }
        if (parent)
        {
            // ReBar is a container. Forward child control notifications to
            // the panel that owns the address bar, as native comctl32 does.
            return SendGuestMessage(parent, message, wParam, lParam, nullptr);
        }
        return 0;
    }

    const bool choiceControl = controlKind == BuiltinControlKind::ListBox ||
        controlKind == BuiltinControlKind::ComboBox;
    const bool listBox = controlKind == BuiltinControlKind::ListBox;
    const bool comboBox = controlKind == BuiltinControlKind::ComboBox;
    if (choiceControl)
    {
        if (comboBox && message == ComboBoxShowDropDown)
        {
            const bool requested = wParam != 0;
            bool changed = false;
            {
                std::lock_guard<std::mutex> guard(window->lock);
                if (window->destroyed) return FALSE;
                changed = window->comboDropped != requested;
                window->comboDropped = requested;
            }
            if (changed)
            {
                invalidate();
                notifyParent(requested ? ComboNotificationDropDown : ComboNotificationCloseUp);
            }
            return TRUE;
        }
        if (comboBox && message == ComboBoxGetDroppedState)
        {
            std::lock_guard<std::mutex> guard(window->lock);
            return window->comboDropped ? TRUE : FALSE;
        }
        if (comboBox && message == ComboBoxSetDroppedWidth)
        {
            std::lock_guard<std::mutex> guard(window->lock);
            const int previous = window->comboDroppedWidth;
            window->comboDroppedWidth = SetWindowPosExtent(static_cast<int>((std::min)(wParam,
                static_cast<WPARAM>((std::numeric_limits<int>::max)()))));
            return previous;
        }
        if (comboBox && message == ComboBoxGetDroppedControlRect)
        {
            RECT rect = {};
            if (!GetGuestWindowRect(window->handle, &rect, nullptr)) return FALSE;
            {
                std::lock_guard<std::mutex> guard(window->lock);
                if (window->comboDroppedWidth > rect.right - rect.left)
                {
                    rect.right = SaturatingAdd(rect.left, window->comboDroppedWidth);
                }
            }
            return TryWriteGuestValue(reinterpret_cast<RECT*>(lParam), rect) ? TRUE : FALSE;
        }
        if (comboBox && (message == ComboBoxExInsertItemW || message == ComboBoxExSetItemW ||
            message == ComboBoxExGetItemW || message == ComboBoxExSetImageList ||
            message == ComboBoxExGetImageList || message == ComboBoxExGetComboControl ||
            message == ComboBoxExGetEditControl))
        {
            if (message == ComboBoxExSetImageList)
            {
                std::lock_guard<std::mutex> guard(window->lock);
                const HANDLE previous = window->comboBoxExImageList;
                window->comboBoxExImageList = reinterpret_cast<HANDLE>(lParam);
                return reinterpret_cast<LRESULT>(previous);
            }
            if (message == ComboBoxExGetImageList)
            {
                std::lock_guard<std::mutex> guard(window->lock);
                return reinterpret_cast<LRESULT>(window->comboBoxExImageList);
            }
            if (message == ComboBoxExGetComboControl || message == ComboBoxExGetEditControl)
            {
                // The bridge renders the editable portion in the ComboBoxEx
                // surface itself. Returning that guest HWND lets callers use
                // their usual CB_* / WM_SETTEXT path without exposing a host
                // child window that could escape UWP composition.
                return reinterpret_cast<LRESULT>(window->handle);
            }

            GuestComboBoxExItemW item = {};
            if (!TryReadGuestValue(reinterpret_cast<const GuestComboBoxExItemW*>(lParam), &item) || item.item < 0)
            {
                return message == ComboBoxExInsertItemW ? -1 : FALSE;
            }

            if (message == ComboBoxExGetItemW)
            {
                std::lock_guard<std::mutex> guard(window->lock);
                if (static_cast<size_t>(item.item) >= window->choiceItems.size())
                {
                    return FALSE;
                }
                if ((item.mask & 0x00000001u) != 0)
                {
                    TryWriteGuestWideString(item.text, static_cast<size_t>((std::max)(0, item.textCapacity)),
                        window->choiceItems[static_cast<size_t>(item.item)], nullptr);
                }
                return TRUE;
            }

            std::wstring caption;
            if ((item.mask & 0x00000001u) != 0 && !TryReadGuestWideString(item.text, &caption))
            {
                return message == ComboBoxExInsertItemW ? -1 : FALSE;
            }
            int result = -1;
            {
                std::lock_guard<std::mutex> guard(window->lock);
                if (window->destroyed)
                {
                    return message == ComboBoxExInsertItemW ? -1 : FALSE;
                }
                if (message == ComboBoxExInsertItemW)
                {
                    const size_t insertion = (std::min)(static_cast<size_t>(item.item), window->choiceItems.size());
                    window->choiceItems.insert(window->choiceItems.begin() + insertion, std::move(caption));
                    result = static_cast<int>(insertion);
                    if (window->selectedChoice < 0)
                    {
                        window->selectedChoice = result;
                        window->title = window->choiceItems[insertion];
                    }
                }
                else if (static_cast<size_t>(item.item) < window->choiceItems.size())
                {
                    if ((item.mask & 0x00000001u) != 0)
                    {
                        window->choiceItems[static_cast<size_t>(item.item)] = std::move(caption);
                        if (window->selectedChoice == item.item)
                        {
                            window->title = window->choiceItems[static_cast<size_t>(item.item)];
                        }
                    }
                    result = TRUE;
                }
            }
            if (result >= 0)
            {
                InvalidateGuestRect(window->handle, nullptr, TRUE, nullptr);
            }
            return result;
        }
        const bool add = (listBox && message == ListBoxAddString) || (comboBox && message == ComboBoxAddString);
        const bool insert = listBox && message == ListBoxInsertString;
        const bool reset = (listBox && message == ListBoxResetContent) || (comboBox && message == ComboBoxResetContent);
        const bool setSelection = (listBox && message == ListBoxSetCurrentSelection) || (comboBox && message == ComboBoxSetCurrentSelection);
        const bool getSelection = (listBox && message == ListBoxGetCurrentSelection) || (comboBox && message == ComboBoxGetCurrentSelection);
        const bool getCount = (listBox && message == ListBoxGetCount) || (comboBox && message == ComboBoxGetCount);
        const bool getText = (listBox && message == ListBoxGetText) || (comboBox && message == ComboBoxGetText);
        const bool remove = listBox && message == ListBoxDeleteString;

        if (add || insert)
        {
            const LPCWSTR source = reinterpret_cast<LPCWSTR>(lParam);
            if (!source) return -1;
            size_t length = 0;
            while (length < MaximumBuiltinControlTextLength && source[length] != L'\0') ++length;
            std::lock_guard<std::mutex> guard(window->lock);
            if (window->destroyed) return -1;
            const size_t requested = insert && static_cast<INT_PTR>(wParam) >= 0 ? static_cast<size_t>(wParam) : window->choiceItems.size();
            const size_t position = (std::min)(requested, window->choiceItems.size());
            window->choiceItems.insert(window->choiceItems.begin() + position, std::wstring(source, length));
            if (window->selectedChoice >= static_cast<int>(position)) ++window->selectedChoice;
            if (window->selectedChoice < 0) window->title = window->choiceItems.front();
            return static_cast<LRESULT>(position);
        }
        if (reset)
        {
            std::lock_guard<std::mutex> guard(window->lock);
            window->choiceItems.clear();
            window->selectedChoice = -1;
            window->title.clear();
            return 0;
        }
        if (remove)
        {
            std::lock_guard<std::mutex> guard(window->lock);
            const INT_PTR index = static_cast<INT_PTR>(wParam);
            if (index < 0 || static_cast<size_t>(index) >= window->choiceItems.size()) return -1;
            window->choiceItems.erase(window->choiceItems.begin() + index);
            if (window->selectedChoice >= static_cast<int>(window->choiceItems.size())) window->selectedChoice = -1;
            return static_cast<LRESULT>(window->choiceItems.size());
        }
        if (setSelection)
        {
            std::lock_guard<std::mutex> guard(window->lock);
            const INT_PTR index = static_cast<INT_PTR>(wParam);
            if (index < -1 || static_cast<size_t>(index) >= window->choiceItems.size()) return -1;
            window->selectedChoice = static_cast<int>(index);
            window->title = index >= 0 ? window->choiceItems[static_cast<size_t>(index)] : std::wstring();
            return static_cast<LRESULT>(index);
        }
        if (getSelection)
        {
            std::lock_guard<std::mutex> guard(window->lock);
            return window->selectedChoice;
        }
        if (getCount)
        {
            std::lock_guard<std::mutex> guard(window->lock);
            return static_cast<LRESULT>(window->choiceItems.size());
        }
        if (getText)
        {
            LPWSTR destination = reinterpret_cast<LPWSTR>(lParam);
            std::lock_guard<std::mutex> guard(window->lock);
            const INT_PTR index = static_cast<INT_PTR>(wParam);
            if (!destination || index < 0 || static_cast<size_t>(index) >= window->choiceItems.size()) return -1;
            const auto& value = window->choiceItems[static_cast<size_t>(index)];
            memcpy(destination, value.c_str(), (value.size() + 1) * sizeof(wchar_t));
            return static_cast<LRESULT>(value.size());
        }
    }
    if (controlKind == BuiltinControlKind::ScrollBar)
    {
        if (message == ScrollBarSetPosition)
        {
            std::lock_guard<std::mutex> guard(window->lock);
            const int previous = window->scrollPosition;
            window->scrollPosition = static_cast<int>(wParam);
            return previous;
        }
        if (message == ScrollBarGetPosition)
        {
            std::lock_guard<std::mutex> guard(window->lock);
            return window->scrollPosition;
        }
    }

    if (controlKind == BuiltinControlKind::ListView)
    {
        if (message == ListViewGetEditControl)
        {
            std::lock_guard<std::mutex> guard(window->lock);
            return window->listViewEditItem >= 0
                ? reinterpret_cast<LRESULT>(window->handle) : 0;
        }
        if (message == ListViewEditLabelW)
        {
            const int item = static_cast<int>(wParam);
            HWND parent = nullptr;
            HWND handle = nullptr;
            UINT_PTR controlId = 0;
            LPARAM itemData = 0;
            std::wstring text;
            {
                std::lock_guard<std::mutex> guard(window->lock);
                if (window->destroyed || item < 0 ||
                    static_cast<size_t>(item) >= window->listViewItems.size())
                    return 0;
                parent = window->parent;
                handle = window->handle;
                controlId = window->controlId;
                if (!window->listViewItems[static_cast<size_t>(item)].empty())
                    text = window->listViewItems[static_cast<size_t>(item)][0];
                if (static_cast<size_t>(item) < window->listViewItemData.size())
                    itemData = window->listViewItemData[static_cast<size_t>(item)];
            }
            if (text.empty() && parent)
            {
                // Owner-data ListViews do not retain item captions.  Obtain
                // the current label through the same LVN_GETDISPINFOW contract
                // used for painting before creating the in-place editor.
                wchar_t scratch[260] = {};
                GuestListViewDisplayInfoW displayInfo = {};
                displayInfo.header.from = handle;
                displayInfo.header.identifier = controlId;
                displayInfo.header.code = ListViewNotifyGetDisplayInfoW;
                displayInfo.item.mask = ListViewItemText;
                displayInfo.item.item = item;
                displayInfo.item.subItem = 0;
                displayInfo.item.text = scratch;
                displayInfo.item.textCapacity = static_cast<int>(_countof(scratch));
                displayInfo.item.itemData = itemData;
                SendGuestMessage(parent, GuestAbi::WmNotify,
                    static_cast<WPARAM>(controlId),
                    reinterpret_cast<LPARAM>(&displayInfo), nullptr);
                TryReadGuestWideString(displayInfo.item.text, &text);
            }
            GuestListViewDisplayInfoW notification = {};
            notification.header.from = handle;
            notification.header.identifier = controlId;
            notification.header.code = ListViewNotifyBeginLabelEditW;
            notification.item.mask = ListViewItemText | ListViewItemParam;
            notification.item.item = item;
            notification.item.text = text.empty() ? nullptr : const_cast<LPWSTR>(text.c_str());
            notification.item.textCapacity = static_cast<int>(text.size() + 1);
            notification.item.itemData = itemData;
            if (parent && SendGuestMessage(parent, GuestAbi::WmNotify,
                static_cast<WPARAM>(controlId), reinterpret_cast<LPARAM>(&notification), nullptr))
                return 0;
            {
                std::lock_guard<std::mutex> guard(window->lock);
                if (window->destroyed) return 0;
                window->listViewEditItem = item;
                window->listViewEditText = text;
                window->listViewEditCaret = text.size();
            }
            SetGuestFocus(handle, nullptr);
            InvalidateGuestRect(handle, nullptr, TRUE, nullptr);
            RuntimeDiagnostics::Record(L"LISTVIEW: began in-place label editing for row " +
                std::to_wstring(item) + L".");
            return reinterpret_cast<LRESULT>(handle);
        }
        if (message == ListViewGetItemCount)
        {
            std::lock_guard<std::mutex> guard(window->lock);
            return static_cast<LRESULT>(window->listViewItems.size());
        }
        if (message == ListViewDeleteAllItems)
        {
            {
                std::lock_guard<std::mutex> guard(window->lock);
                window->listViewItems.clear();
                window->listViewItemData.clear();
                window->listViewItemStates.clear();
                window->listViewItemImages.clear();
                window->listViewSelectedItem = -1;
                window->listViewSelectionMark = -1;
                window->listViewTopItem = 0;
                window->listViewPressedItem = -1;
                window->listViewEditItem = -1;
                window->listViewEditText.clear();
            }
            InvalidateGuestRect(window->handle, nullptr, TRUE, nullptr);
            return TRUE;
        }
        if (message == ListViewDeleteItem)
        {
            const int item = static_cast<int>(wParam);
            {
                std::lock_guard<std::mutex> guard(window->lock);
                if (item < 0 || static_cast<size_t>(item) >= window->listViewItems.size())
                {
                    return FALSE;
                }
                window->listViewItems.erase(window->listViewItems.begin() + item);
                if (window->listViewEditItem == item)
                {
                    window->listViewEditItem = -1;
                    window->listViewEditText.clear();
                }
                else if (window->listViewEditItem > item)
                    --window->listViewEditItem;
                if (static_cast<size_t>(item) < window->listViewItemData.size())
                {
                    window->listViewItemData.erase(window->listViewItemData.begin() + item);
                }
                if (static_cast<size_t>(item) < window->listViewItemStates.size())
                {
                    window->listViewItemStates.erase(window->listViewItemStates.begin() + item);
                }
                if (static_cast<size_t>(item) < window->listViewItemImages.size())
                {
                    window->listViewItemImages.erase(window->listViewItemImages.begin() + item);
                }
                if (window->listViewSelectedItem == item)
                {
                    window->listViewSelectedItem = -1;
                }
                else if (window->listViewSelectedItem > item)
                {
                    --window->listViewSelectedItem;
                }
                if (window->listViewSelectionMark == item)
                {
                    window->listViewSelectionMark = -1;
                }
                else if (window->listViewSelectionMark > item)
                {
                    --window->listViewSelectionMark;
                }
                window->listViewTopItem = (std::min)(window->listViewTopItem,
                    (std::max)(0, static_cast<int>(window->listViewItems.size()) - 1));
            }
            InvalidateGuestRect(window->handle, nullptr, TRUE, nullptr);
            return TRUE;
        }
        if (message == ListViewDeleteColumn)
        {
            const int column = static_cast<int>(wParam);
            {
                std::lock_guard<std::mutex> guard(window->lock);
                if (column < 0 || static_cast<size_t>(column) >= window->listViewColumns.size())
                {
                    return FALSE;
                }
                window->listViewColumns.erase(window->listViewColumns.begin() + column);
                window->listViewColumnWidths.erase(window->listViewColumnWidths.begin() + column);
                window->listViewColumnFormats.erase(window->listViewColumnFormats.begin() + column);
                window->listViewColumnSubItems.erase(window->listViewColumnSubItems.begin() + column);
                window->listViewColumnImages.erase(window->listViewColumnImages.begin() + column);
                window->listViewColumnOrders.erase(window->listViewColumnOrders.begin() + column);
                for (size_t index = 0; index < window->listViewColumnOrders.size(); ++index)
                {
                    window->listViewColumnOrders[index] = static_cast<int>(index);
                }
                for (auto& row : window->listViewItems)
                {
                    if (static_cast<size_t>(column) < row.size())
                    {
                        row.erase(row.begin() + column);
                    }
                }
            }
            InvalidateGuestRect(window->handle, nullptr, TRUE, nullptr);
            return TRUE;
        }
        if (message == ListViewGetSelectedCount)
        {
            std::lock_guard<std::mutex> guard(window->lock);
            return static_cast<LRESULT>(std::count_if(
                window->listViewItemStates.begin(),
                window->listViewItemStates.end(),
                [](UINT state) { return (state & ListViewStateSelected) != 0; }));
        }
        if (message == ListViewGetSelectionMark)
        {
            std::lock_guard<std::mutex> guard(window->lock);
            return window->listViewSelectionMark;
        }
        if (message == ListViewSetSelectionMark)
        {
            const int requested = static_cast<int>(lParam);
            std::lock_guard<std::mutex> guard(window->lock);
            const int previous = window->listViewSelectionMark;
            window->listViewSelectionMark = requested >= 0 &&
                static_cast<size_t>(requested) < window->listViewItems.size()
                ? requested
                : -1;
            return previous;
        }
        if (message == ListViewGetNextItem)
        {
            std::lock_guard<std::mutex> guard(window->lock);
            const UINT requestedState = static_cast<UINT>(lParam) &
                (ListViewStateFocused | ListViewStateSelected);
            const int first = static_cast<int>(wParam) + 1;
            for (int item = (std::max)(0, first);
                static_cast<size_t>(item) < window->listViewItemStates.size(); ++item)
            {
                const UINT state = window->listViewItemStates[static_cast<size_t>(item)];
                if (requestedState == 0 || (state & requestedState) == requestedState)
                {
                    return item;
                }
            }
            return -1;
        }
        if (message == ListViewGetItemTextW || message == ListViewGetItemW)
        {
            const auto guestItem = reinterpret_cast<GuestListViewItemW*>(lParam);
            GuestListViewItemW request = {};
            if (!TryReadGuestValue(guestItem, &request))
            {
                return message == ListViewGetItemTextW ? 0 : FALSE;
            }
            const int item = message == ListViewGetItemTextW ? static_cast<int>(wParam) : request.item;
            const int subItem = request.subItem;
            std::wstring text;
            UINT itemState = 0;
            LPARAM itemData = 0;
            int itemImage = -1;
            {
                std::lock_guard<std::mutex> guard(window->lock);
                if (item < 0 || subItem < 0 || static_cast<size_t>(item) >= window->listViewItems.size())
                {
                    return message == ListViewGetItemTextW ? 0 : FALSE;
                }
                const auto& row = window->listViewItems[static_cast<size_t>(item)];
                if (static_cast<size_t>(subItem) < row.size())
                {
                    text = row[static_cast<size_t>(subItem)];
                }
                if (static_cast<size_t>(item) < window->listViewItemStates.size())
                {
                    itemState = window->listViewItemStates[static_cast<size_t>(item)];
                }
                if (static_cast<size_t>(item) < window->listViewItemData.size())
                {
                    itemData = window->listViewItemData[static_cast<size_t>(item)];
                }
                if (static_cast<size_t>(item) < window->listViewItemImages.size())
                {
                    itemImage = window->listViewItemImages[static_cast<size_t>(item)];
                }
            }
            size_t copied = 0;
            if ((request.mask & ListViewItemText) != 0 || message == ListViewGetItemTextW)
            {
                if (!TryWriteGuestWideString(request.text, (std::max)(0, request.textCapacity), text, &copied))
                {
                    return message == ListViewGetItemTextW ? 0 : FALSE;
                }
            }
            if (message == ListViewGetItemW)
            {
                if ((request.mask & ListViewItemState) != 0)
                {
                    request.state = itemState & request.stateMask;
                }
                if ((request.mask & ListViewItemParam) != 0)
                {
                    request.itemData = itemData;
                }
                if ((request.mask & ListViewItemImage) != 0)
                {
                    request.image = itemImage;
                }
                if (!TryWriteGuestValue(guestItem, request))
                {
                    return FALSE;
                }
                return TRUE;
            }
            return static_cast<LRESULT>(copied);
        }
        if (message == ListViewGetBackgroundColor)
        {
            std::lock_guard<std::mutex> guard(window->lock);
            return static_cast<LRESULT>(window->listViewBackgroundColor);
        }
        if (message == ListViewGetTextColor)
        {
            std::lock_guard<std::mutex> guard(window->lock);
            return static_cast<LRESULT>(window->listViewTextColor);
        }
        if (message == ListViewGetTextBackgroundColor)
        {
            std::lock_guard<std::mutex> guard(window->lock);
            return static_cast<LRESULT>(window->listViewTextBackgroundColor);
        }
        if (message == ListViewSetBackgroundColor ||
            message == ListViewSetTextColor ||
            message == ListViewSetTextBackgroundColor)
        {
            {
                std::lock_guard<std::mutex> guard(window->lock);
                const COLORREF color = static_cast<COLORREF>(lParam);
                if (message == ListViewSetBackgroundColor)
                {
                    window->listViewBackgroundColor = color;
                }
                else if (message == ListViewSetTextColor)
                {
                    window->listViewTextColor = color;
                }
                else
                {
                    window->listViewTextBackgroundColor = color;
                }
            }
            InvalidateGuestRect(window->handle, nullptr, TRUE, nullptr);
            return TRUE;
        }
        if (message == ListViewSetImageList)
        {
            const size_t imageList = static_cast<size_t>(wParam);
            if (imageList >= _countof(window->listViewImageLists))
            {
                return 0;
            }
            HANDLE previous = nullptr;
            {
                std::lock_guard<std::mutex> guard(window->lock);
                previous = window->listViewImageLists[imageList];
                window->listViewImageLists[imageList] = reinterpret_cast<HANDLE>(lParam);
            }
            InvalidateGuestRect(window->handle, nullptr, TRUE, nullptr);
            return reinterpret_cast<LRESULT>(previous);
        }
        if (message == ListViewGetImageList)
        {
            const size_t imageList = static_cast<size_t>(wParam);
            std::lock_guard<std::mutex> guard(window->lock);
            return imageList < _countof(window->listViewImageLists)
                ? reinterpret_cast<LRESULT>(window->listViewImageLists[imageList])
                : 0;
        }
        if (message == ListViewSetColumnOrderArray || message == ListViewGetColumnOrderArray)
        {
            const size_t count = static_cast<size_t>(wParam);
            auto values = reinterpret_cast<int*>(lParam);
            if (!values || count > 256)
            {
                return FALSE;
            }
            std::vector<int> order(count);
            if (message == ListViewSetColumnOrderArray)
            {
                for (size_t position = 0; position < count; ++position)
                {
                    if (!TryReadGuestValue(values + position, &order[position]) ||
                        order[position] < 0 || static_cast<size_t>(order[position]) >= count)
                    {
                        return FALSE;
                    }
                }
                std::vector<bool> seen(count, false);
                for (const int column : order)
                {
                    if (seen[static_cast<size_t>(column)])
                    {
                        return FALSE;
                    }
                    seen[static_cast<size_t>(column)] = true;
                }
                {
                    std::lock_guard<std::mutex> guard(window->lock);
                    if (count != window->listViewColumns.size())
                    {
                        return FALSE;
                    }
                    for (size_t position = 0; position < count; ++position)
                    {
                        window->listViewColumnOrders[static_cast<size_t>(order[position])] =
                            static_cast<int>(position);
                    }
                }
                InvalidateGuestRect(window->handle, nullptr, TRUE, nullptr);
                return TRUE;
            }

            {
                std::lock_guard<std::mutex> guard(window->lock);
                if (count != window->listViewColumns.size())
                {
                    return FALSE;
                }
                for (size_t column = 0; column < count; ++column)
                {
                    const int position = window->listViewColumnOrders[column];
                    if (position < 0 || static_cast<size_t>(position) >= count)
                    {
                        return FALSE;
                    }
                    order[static_cast<size_t>(position)] = static_cast<int>(column);
                }
            }
            for (size_t position = 0; position < count; ++position)
            {
                if (!TryWriteGuestValue(values + position, order[position]))
                {
                    return FALSE;
                }
            }
            return TRUE;
        }
        if (message == ListViewGetCallbackMask)
        {
            std::lock_guard<std::mutex> guard(window->lock);
            return window->listViewCallbackMask;
        }
        if (message == ListViewSetCallbackMask)
        {
            std::lock_guard<std::mutex> guard(window->lock);
            window->listViewCallbackMask = static_cast<UINT>(wParam);
            return TRUE;
        }
        if (message == ListViewSetExtendedStyle)
        {
            UINT previous = 0;
            {
                std::lock_guard<std::mutex> guard(window->lock);
                previous = window->listViewExtendedStyle;
                const UINT mask = static_cast<UINT>(wParam);
                const UINT requested = static_cast<UINT>(lParam);
                window->listViewExtendedStyle = mask == 0
                    ? requested
                    : ((window->listViewExtendedStyle & ~mask) | (requested & mask));
            }
            InvalidateGuestRect(window->handle, nullptr, TRUE, nullptr);
            return previous;
        }
        if (message == ListViewGetTopIndex)
        {
            std::lock_guard<std::mutex> guard(window->lock);
            return window->listViewTopItem;
        }
        if (message == ListViewGetCountPerPage)
        {
            std::lock_guard<std::mutex> guard(window->lock);
            const int height = (std::max)(0, static_cast<int>(window->bounds.bottom - window->bounds.top));
            return (std::max)(0,
                (height - window->listViewHeaderHeight) /
                    window->listViewRowHeight);
        }
        if (message == ListViewGetStringWidthW)
        {
            std::wstring text;
            if (!TryReadGuestWideString(reinterpret_cast<LPCWSTR>(lParam), &text))
            {
                return 0;
            }
            int textWidth = GuestMetrics::TextWidth;
            MiniGdi::DcHandle dc = MiniGdi::InvalidDc;
            MiniGdi::ObjectHandle font = MiniGdi::InvalidObject;
            {
                std::lock_guard<std::mutex> guard(window->lock);
                textWidth = window->controlTextWidth;
                dc = window->dc;
                font = window->controlFont;
            }
            MiniGdi::ObjectHandle previous = MiniGdi::InvalidObject;
            MiniGdi::Size measured{};
            if (dc != MiniGdi::InvalidDc && m_gdi.SelectFont(dc, font, &previous))
            {
                const bool succeeded = m_gdi.GetTextExtentW(
                    dc, text.data(), text.size(), &measured);
                m_gdi.SelectFont(dc, previous, nullptr);
                if (succeeded) return measured.width;
            }
            const size_t maximumCharacters = static_cast<size_t>(
                (std::numeric_limits<int>::max)() / textWidth);
            return static_cast<LRESULT>((std::min)(text.size(), maximumCharacters) *
                textWidth);
        }
        if (message == ListViewGetItemRect || message == ListViewGetSubItemRect)
        {
            RECT result = {};
            if (!TryReadGuestValue(reinterpret_cast<const RECT*>(lParam), &result))
            {
                return FALSE;
            }
            const int item = static_cast<int>(wParam);
            int topItem = 0;
            int clientWidth = 0;
            int headerHeight = GuestMetrics::ListViewHeaderHeight;
            int rowHeight = GuestMetrics::TextHeight;
            size_t itemCount = 0;
            std::vector<int> widths;
            std::vector<int> orders;
            {
                std::lock_guard<std::mutex> guard(window->lock);
                topItem = window->listViewTopItem;
                clientWidth = window->surface.Width();
                headerHeight = window->listViewHeaderHeight;
                rowHeight = window->listViewRowHeight;
                itemCount = window->listViewItems.size();
                widths = window->listViewColumnWidths;
                orders = window->listViewColumnOrders;
            }
            if (item < 0 || static_cast<size_t>(item) >= itemCount)
            {
                return FALSE;
            }

            const int rowTop = headerHeight + (item - topItem) * rowHeight;
            int column = 0;
            const int portion = result.left;
            if (message == ListViewGetSubItemRect)
            {
                column = result.top;
                if (column < 0 || static_cast<size_t>(column) >= widths.size())
                {
                    return FALSE;
                }
            }
            int columnLeft = 1;
            std::vector<size_t> displayColumns(widths.size());
            std::iota(displayColumns.begin(), displayColumns.end(), static_cast<size_t>(0));
            if (orders.size() == displayColumns.size())
            {
                std::stable_sort(displayColumns.begin(), displayColumns.end(), [&orders](size_t left, size_t right)
                {
                    return orders[left] < orders[right];
                });
            }
            for (const size_t displayedColumn : displayColumns)
            {
                if (displayedColumn == static_cast<size_t>(column))
                {
                    break;
                }
                columnLeft += (std::max)(24, widths[displayedColumn]);
            }
            const int columnWidth = static_cast<size_t>(column) < widths.size()
                ? (std::max)(24, widths[static_cast<size_t>(column)])
                : (std::max)(1, clientWidth - columnLeft - 1);

            result.top = rowTop;
            result.bottom = rowTop + rowHeight;
            if (portion == 0) // LVIR_BOUNDS
            {
                result.left = message == ListViewGetSubItemRect ? columnLeft : 1;
                result.right = message == ListViewGetSubItemRect
                    ? columnLeft + columnWidth
                    : (std::max)(1, clientWidth - 1);
            }
            else if (portion == 1) // LVIR_ICON
            {
                result.left = columnLeft + 2;
                result.right = (std::min)(columnLeft + columnWidth, columnLeft + 20);
            }
            else // LVIR_LABEL / LVIR_SELECTBOUNDS
            {
                result.left = columnLeft + 2;
                result.right = columnLeft + columnWidth;
            }
            return TryWriteGuestValue(reinterpret_cast<RECT*>(lParam), result) ? TRUE : FALSE;
        }
        if (message == ListViewGetItemPosition)
        {
            const int item = static_cast<int>(wParam);
            POINT result = {};
            {
                std::lock_guard<std::mutex> guard(window->lock);
                if (item < 0 || static_cast<size_t>(item) >= window->listViewItems.size())
                {
                    return FALSE;
                }
                result.x = 1;
                result.y = window->listViewHeaderHeight +
                    (item - window->listViewTopItem) * window->listViewRowHeight;
            }
            return TryWriteGuestValue(reinterpret_cast<POINT*>(lParam), result) ? TRUE : FALSE;
        }
        if (message == ListViewHitTest || message == ListViewSubItemHitTest)
        {
            auto destination = reinterpret_cast<GuestListViewHitTestInfo*>(lParam);
            GuestListViewHitTestInfo result = {};
            if (!TryReadGuestValue(destination, &result))
            {
                return -1;
            }
            int topItem = 0;
            int clientWidth = 0;
            int clientHeight = 0;
            int headerHeight = GuestMetrics::ListViewHeaderHeight;
            int rowHeight = GuestMetrics::TextHeight;
            size_t itemCount = 0;
            std::vector<int> widths;
            std::vector<int> orders;
            {
                std::lock_guard<std::mutex> guard(window->lock);
                topItem = window->listViewTopItem;
                clientWidth = window->surface.Width();
                clientHeight = window->surface.Height();
                headerHeight = window->listViewHeaderHeight;
                rowHeight = window->listViewRowHeight;
                itemCount = window->listViewItems.size();
                widths = window->listViewColumnWidths;
                orders = window->listViewColumnOrders;
            }
            result.flags = ListViewHitNowhere;
            result.item = -1;
            result.subItem = 0;
            result.group = -1;
            if (result.point.x >= 0 && result.point.x < clientWidth &&
                result.point.y >= headerHeight &&
                result.point.y < clientHeight)
            {
                const int item = topItem +
                    (result.point.y - headerHeight) / rowHeight;
                if (item >= 0 && static_cast<size_t>(item) < itemCount)
                {
                    result.item = item;
                    result.flags = result.point.x < 20
                        ? ListViewHitOnItemIcon
                        : ListViewHitOnItemLabel;
                    int right = 1;
                    std::vector<size_t> displayColumns(widths.size());
                    std::iota(displayColumns.begin(), displayColumns.end(), static_cast<size_t>(0));
                    if (orders.size() == displayColumns.size())
                    {
                        std::stable_sort(displayColumns.begin(), displayColumns.end(),
                            [&orders](size_t left, size_t right)
                        {
                            return orders[left] < orders[right];
                        });
                    }
                    for (const size_t column : displayColumns)
                    {
                        right += (std::max)(24, widths[column]);
                        if (result.point.x < right)
                        {
                            result.subItem = static_cast<int>(column);
                            break;
                        }
                    }
                }
            }
            if (!TryWriteGuestValue(destination, result))
            {
                return -1;
            }
            return result.item;
        }
        if (message == ListViewRedrawItems)
        {
            const int first = static_cast<int>(wParam);
            const int last = static_cast<int>(lParam);
            size_t itemCount = 0;
            {
                std::lock_guard<std::mutex> guard(window->lock);
                itemCount = window->listViewItems.size();
            }
            if (first < 0 || last < first || static_cast<size_t>(last) >= itemCount)
            {
                return FALSE;
            }
            InvalidateGuestRect(window->handle, nullptr, TRUE, nullptr);
            return TRUE;
        }
        if (message == ListViewEnsureVisible)
        {
            const int requested = static_cast<int>(wParam);
            bool changed = false;
            {
                std::lock_guard<std::mutex> guard(window->lock);
                if (requested < 0 || static_cast<size_t>(requested) >= window->listViewItems.size())
                {
                    return FALSE;
                }
                const int height = (std::max)(0, static_cast<int>(window->bounds.bottom - window->bounds.top));
                const int rowsPerPage = (std::max)(1,
                    (height - window->listViewHeaderHeight) /
                        window->listViewRowHeight);
                const int previous = window->listViewTopItem;
                if (requested < window->listViewTopItem)
                {
                    window->listViewTopItem = requested;
                }
                else if (requested >= window->listViewTopItem + rowsPerPage)
                {
                    window->listViewTopItem = requested - rowsPerPage + 1;
                }
                changed = previous != window->listViewTopItem;
            }
            if (changed)
            {
                InvalidateGuestRect(window->handle, nullptr, TRUE, nullptr);
            }
            return TRUE;
        }
        if (message == ListViewGetExtendedStyle)
        {
            std::lock_guard<std::mutex> guard(window->lock);
            return window->listViewExtendedStyle;
        }
        if (message == ListViewScroll)
        {
            // Report view scrolls in pixels in the desktop API.  This
            // retained renderer is row-based, so translate a vertical pixel
            // delta into the smallest whole-row movement that preserves the
            // visible ordering.
            const int verticalPixels = static_cast<int>(lParam);
            bool changed = false;
            {
                std::lock_guard<std::mutex> guard(window->lock);
                int delta = verticalPixels / window->listViewRowHeight;
                if (verticalPixels != 0 && verticalPixels % window->listViewRowHeight != 0)
                {
                    delta += verticalPixels > 0 ? 1 : -1;
                }
                const int maximumTop = (std::max)(0, static_cast<int>(window->listViewItems.size()) - 1);
                const int next = (std::max)(0, (std::min)(maximumTop, window->listViewTopItem + delta));
                changed = next != window->listViewTopItem;
                window->listViewTopItem = next;
            }
            if (changed)
            {
                InvalidateGuestRect(window->handle, nullptr, TRUE, nullptr);
            }
            return TRUE;
        }
        if (message == ListViewSetItemCount)
        {
            const size_t requested = (std::min)(static_cast<size_t>(wParam), static_cast<size_t>(65536));
            bool ownerData = false;
            {
                std::lock_guard<std::mutex> guard(window->lock);
                ownerData = (window->style & ListViewStyleOwnerData) != 0;
                if (!ownerData)
                {
                    // Windows ignores this request for a normal retained-item
                    // ListView. Do not turn a stale folder count into synthetic
                    // duplicate rows.
                    if (g_listViewPopulationDiagnostics < 64)
                    {
                        ++g_listViewPopulationDiagnostics;
                        RuntimeDiagnostics::Record(
                            L"LISTVIEW: ignored LVM_SETITEMCOUNT for a retained-item view; requested " +
                            std::to_wstring(requested) + L" rows.");
                    }
                    return FALSE;
                }
                // Owner-data list views establish their row count before asking
                // for text.  Materialise empty rows so later LVM_SETITEMTEXTW
                // updates have a stable target without unbounded allocation.
                window->listViewItems.resize(requested);
                window->listViewItemData.resize(requested);
                window->listViewItemStates.resize(requested);
                window->listViewItemImages.resize(requested, -1);
                if (window->listViewSelectedItem >= static_cast<int>(requested))
                {
                    window->listViewSelectedItem = -1;
                }
                window->listViewTopItem = (std::min)(window->listViewTopItem,
                    (std::max)(0, static_cast<int>(requested) - 1));
            }
            if (g_listViewPopulationDiagnostics < 64)
            {
                ++g_listViewPopulationDiagnostics;
                RuntimeDiagnostics::Record(
                    L"LISTVIEW: accepted owner-data row count " + std::to_wstring(requested) + L".");
            }
            InvalidateGuestRect(window->handle, nullptr, TRUE, nullptr);
            return TRUE;
        }
        if (message == ListViewSortItems)
        {
            const auto compare = reinterpret_cast<GuestListViewCompare>(lParam);
            if (!compare)
            {
                return FALSE;
            }

            std::vector<std::vector<std::wstring>> rows;
            std::vector<LPARAM> itemData;
            std::vector<UINT> itemStates;
            std::vector<int> itemImages;
            int selectedItem = -1;
            int selectionMark = -1;
            {
                std::lock_guard<std::mutex> guard(window->lock);
                rows = window->listViewItems;
                itemData = window->listViewItemData;
                itemStates = window->listViewItemStates;
                itemImages = window->listViewItemImages;
                selectedItem = window->listViewSelectedItem;
                selectionMark = window->listViewSelectionMark;
            }
            if (rows.size() != itemData.size() || rows.size() != itemStates.size() ||
                rows.size() != itemImages.size())
            {
                return FALSE;
            }

            std::vector<size_t> order(rows.size());
            std::iota(order.begin(), order.end(), static_cast<size_t>(0));
            DWORD callbackException = ERROR_SUCCESS;
            std::stable_sort(order.begin(), order.end(), [&](size_t left, size_t right)
            {
                if (callbackException != ERROR_SUCCESS)
                {
                    return false;
                }
                DWORD currentException = ERROR_SUCCESS;
                const int comparison = InvokeGuestListViewCompare(
                    compare,
                    itemData[left],
                    itemData[right],
                    static_cast<LPARAM>(wParam),
                    &currentException);
                if (currentException != ERROR_SUCCESS)
                {
                    callbackException = currentException;
                    return false;
                }
                return comparison < 0;
            });
            if (callbackException != ERROR_SUCCESS)
            {
                RuntimeDiagnostics::Record(
                    L"LISTVIEW: comparison callback raised exception " +
                    std::to_wstring(static_cast<unsigned long>(callbackException)) + L".");
                return FALSE;
            }

            std::vector<std::vector<std::wstring>> sortedRows;
            std::vector<LPARAM> sortedData;
            std::vector<UINT> sortedStates;
            std::vector<int> sortedImages;
            sortedRows.reserve(order.size());
            sortedData.reserve(order.size());
            sortedStates.reserve(order.size());
            sortedImages.reserve(order.size());
            int sortedSelectedItem = -1;
            int sortedSelectionMark = -1;
            for (size_t position = 0; position < order.size(); ++position)
            {
                const size_t original = order[position];
                sortedRows.push_back(std::move(rows[original]));
                sortedData.push_back(itemData[original]);
                sortedStates.push_back(itemStates[original]);
                sortedImages.push_back(itemImages[original]);
                if (static_cast<int>(original) == selectedItem)
                {
                    sortedSelectedItem = static_cast<int>(position);
                }
                if (static_cast<int>(original) == selectionMark)
                {
                    sortedSelectionMark = static_cast<int>(position);
                }
            }
            {
                std::lock_guard<std::mutex> guard(window->lock);
                if (window->listViewItems.size() != order.size() ||
                    window->listViewItemData != itemData)
                {
                    return FALSE;
                }
                window->listViewItems = std::move(sortedRows);
                window->listViewItemData = std::move(sortedData);
                window->listViewItemStates = std::move(sortedStates);
                window->listViewItemImages = std::move(sortedImages);
                window->listViewSelectedItem = sortedSelectedItem;
                window->listViewSelectionMark = sortedSelectionMark;
            }
            InvalidateGuestRect(window->handle, nullptr, TRUE, nullptr);
            return TRUE;
        }
        if (message == ListViewSetItemState)
        {
            GuestListViewItemW source = {};
            if (!TryReadGuestValue(reinterpret_cast<const GuestListViewItemW*>(lParam), &source))
            {
                return FALSE;
            }
            const UINT stateMask = source.stateMask;
            if (stateMask != 0)
            {
                {
                    std::lock_guard<std::mutex> guard(window->lock);
                    const int item = static_cast<int>(wParam);
                    if (item == -1)
                    {
                        for (auto& state : window->listViewItemStates)
                        {
                            state = (state & ~stateMask) | (source.state & stateMask);
                        }
                        if ((stateMask & ListViewStateSelected) != 0 &&
                            (source.state & ListViewStateSelected) == 0)
                        {
                            window->listViewSelectedItem = -1;
                        }
                    }
                    else if (item >= 0 && static_cast<size_t>(item) < window->listViewItemStates.size())
                    {
                        auto& state = window->listViewItemStates[static_cast<size_t>(item)];
                        state = (state & ~stateMask) | (source.state & stateMask);
                        if ((stateMask & (ListViewStateSelected | ListViewStateFocused)) != 0 &&
                            (state & (ListViewStateSelected | ListViewStateFocused)) != 0)
                        {
                            window->listViewSelectedItem = item;
                            window->listViewSelectionMark = item;
                        }
                        else if (window->listViewSelectedItem == item)
                        {
                            window->listViewSelectedItem = -1;
                        }
                    }
                    else
                    {
                        return FALSE;
                    }
                }
                InvalidateGuestRect(window->handle, nullptr, TRUE, nullptr);
            }
            return TRUE;
        }
        if (message == ListViewGetItemState)
        {
            const int item = static_cast<int>(wParam);
            const UINT mask = static_cast<UINT>(lParam);
            std::lock_guard<std::mutex> guard(window->lock);
            return item >= 0 && static_cast<size_t>(item) < window->listViewItemStates.size()
                ? static_cast<LRESULT>(window->listViewItemStates[static_cast<size_t>(item)] & mask)
                : 0;
        }
        if (message == ListViewSetColumnWidth)
        {
            const int column = static_cast<int>(wParam);
            const int width = (std::max)(24, static_cast<int>(lParam));
            {
                std::lock_guard<std::mutex> guard(window->lock);
                if (column < 0 || static_cast<size_t>(column) >= window->listViewColumnWidths.size())
                {
                    return FALSE;
                }
                window->listViewColumnWidths[static_cast<size_t>(column)] = width;
            }
            InvalidateGuestRect(window->handle, nullptr, TRUE, nullptr);
            return TRUE;
        }
        if (message == ListViewGetColumnWidth)
        {
            const int column = static_cast<int>(wParam);
            std::lock_guard<std::mutex> guard(window->lock);
            return column >= 0 && static_cast<size_t>(column) < window->listViewColumnWidths.size()
                ? window->listViewColumnWidths[static_cast<size_t>(column)]
                : 0;
        }
        if (message == ListViewGetColumnW)
        {
            auto guestColumn = reinterpret_cast<GuestListViewColumnW*>(lParam);
            GuestListViewColumnW result = {};
            const int column = static_cast<int>(wParam);
            if (column < 0 || !TryReadGuestValue(guestColumn, &result))
            {
                return FALSE;
            }

            std::wstring caption;
            {
                std::lock_guard<std::mutex> guard(window->lock);
                const size_t index = static_cast<size_t>(column);
                if (index >= window->listViewColumns.size())
                {
                    return FALSE;
                }
                caption = window->listViewColumns[index];
                if ((result.mask & ListViewColumnFormat) != 0)
                {
                    result.format = window->listViewColumnFormats[index];
                }
                if ((result.mask & ListViewColumnWidth) != 0)
                {
                    result.width = window->listViewColumnWidths[index];
                }
                if ((result.mask & ListViewColumnSubItem) != 0)
                {
                    result.subItem = window->listViewColumnSubItems[index];
                }
                if ((result.mask & ListViewColumnImage) != 0)
                {
                    result.image = window->listViewColumnImages[index];
                }
                if ((result.mask & ListViewColumnOrder) != 0)
                {
                    result.order = window->listViewColumnOrders[index];
                }
            }
            if ((result.mask & ListViewColumnText) != 0 &&
                !TryWriteGuestWideString(result.text,
                    static_cast<size_t>((std::max)(0, result.textCapacity)), caption, nullptr))
            {
                return FALSE;
            }
            return TryWriteGuestValue(guestColumn, result) ? TRUE : FALSE;
        }
        if (message == ListViewSetColumnW)
        {
            GuestListViewColumnW source = {};
            const int column = static_cast<int>(wParam);
            if (column < 0 || !TryReadGuestValue(reinterpret_cast<const GuestListViewColumnW*>(lParam), &source))
            {
                return FALSE;
            }
            {
                std::lock_guard<std::mutex> guard(window->lock);
                if (static_cast<size_t>(column) >= window->listViewColumns.size())
                {
                    return FALSE;
                }
                if ((source.mask & ListViewColumnText) != 0)
                {
                    std::wstring caption;
                    if (TryReadGuestWideString(source.text, &caption))
                    {
                        window->listViewColumns[static_cast<size_t>(column)] = std::move(caption);
                    }
                }
                const size_t index = static_cast<size_t>(column);
                if ((source.mask & ListViewColumnFormat) != 0)
                {
                    window->listViewColumnFormats[index] = source.format;
                }
                if ((source.mask & ListViewColumnWidth) != 0)
                {
                    window->listViewColumnWidths[index] = (std::max)(0, source.width);
                }
                if ((source.mask & ListViewColumnSubItem) != 0)
                {
                    window->listViewColumnSubItems[index] = source.subItem;
                }
                if ((source.mask & ListViewColumnImage) != 0)
                {
                    window->listViewColumnImages[index] = source.image;
                }
                if ((source.mask & ListViewColumnOrder) != 0)
                {
                    window->listViewColumnOrders[index] = source.order;
                }
            }
            InvalidateGuestRect(window->handle, nullptr, TRUE, nullptr);
            return TRUE;
        }
        if (message == ListViewInsertColumnW)
        {
            GuestListViewColumnW source = {};
            if (!TryReadGuestValue(reinterpret_cast<const GuestListViewColumnW*>(lParam), &source))
            {
                return -1;
            }
            const int requested = static_cast<int>(wParam);
            std::wstring columnText;
            if ((source.mask & ListViewColumnText) == 0 ||
                !TryReadGuestWideString(source.text, &columnText))
            {
                columnText = L"Column " + std::to_wstring(
                    static_cast<unsigned int>((std::max)(0, requested) + 1));
            }
            const int columnWidth = (std::max)(24, source.width);
            {
                std::lock_guard<std::mutex> guard(window->lock);
                const size_t position = requested < 0
                    ? window->listViewColumns.size()
                    : (std::min)(static_cast<size_t>(requested), window->listViewColumns.size());
                window->listViewColumns.insert(window->listViewColumns.begin() + position, columnText);
                window->listViewColumnWidths.insert(window->listViewColumnWidths.begin() + position, columnWidth);
                window->listViewColumnFormats.insert(window->listViewColumnFormats.begin() + position,
                    (source.mask & ListViewColumnFormat) != 0 ? source.format : 0);
                window->listViewColumnSubItems.insert(window->listViewColumnSubItems.begin() + position,
                    (source.mask & ListViewColumnSubItem) != 0 ? source.subItem : static_cast<int>(position));
                window->listViewColumnImages.insert(window->listViewColumnImages.begin() + position,
                    (source.mask & ListViewColumnImage) != 0 ? source.image : -1);
                window->listViewColumnOrders.insert(window->listViewColumnOrders.begin() + position,
                    (source.mask & ListViewColumnOrder) != 0 ? source.order : static_cast<int>(position));
                for (auto& row : window->listViewItems)
                {
                    row.insert(row.begin() + (std::min)(position, row.size()), std::wstring());
                }
            }
            InvalidateGuestRect(window->handle, nullptr, TRUE, nullptr);
            return requested < 0 ? 0 : requested;
        }
        if (message == ListViewInsertItemW || message == ListViewSetItemW || message == ListViewSetItemTextW)
        {
            GuestListViewItemW source = {};
            if (!TryReadGuestValue(reinterpret_cast<const GuestListViewItemW*>(lParam), &source) || source.subItem < 0)
            {
                return message == ListViewInsertItemW ? -1 : FALSE;
            }
            // Per the comctl32 contract, SETITEMTEXT uses wParam for the row;
            // INSERTITEM and SETITEM use LVITEM::iItem.
            int itemIndex = message == ListViewSetItemTextW
                ? static_cast<int>(wParam)
                : source.item;
            const int subItem = source.subItem;
            std::wstring itemText;
            if (((source.mask & ListViewItemText) == 0 && message != ListViewSetItemTextW) ||
                !TryReadGuestWideString(source.text, &itemText))
            {
                // LPSTR_TEXTCALLBACKW is valid ListView input. Keep this
                // cell empty until paint requests LVN_GETDISPINFOW instead of
                // substituting a bridge-invented caption such as "Item 1".
                itemText.clear();
            }
            {
                std::lock_guard<std::mutex> guard(window->lock);
                if (message == ListViewInsertItemW)
                {
                    // Windows requires iSubItem == 0 on insertion.  Refusing
                    // a malformed insert prevents a sparse phantom row.
                    if (subItem != 0)
                    {
                        return -1;
                    }
                    const size_t position = itemIndex < 0
                        ? window->listViewItems.size()
                        : (std::min)(static_cast<size_t>(itemIndex), window->listViewItems.size());
                    const size_t columnCount = (std::max)(
                        static_cast<size_t>(1), window->listViewColumns.size());
                    if (window->listViewSelectedItem >= static_cast<int>(position))
                    {
                        ++window->listViewSelectedItem;
                    }
                    if (window->listViewSelectionMark >= static_cast<int>(position))
                    {
                        ++window->listViewSelectionMark;
                    }
                    std::vector<std::wstring> row(columnCount);
                    row[0] = std::move(itemText);
                    window->listViewItems.insert(window->listViewItems.begin() + position, std::move(row));
                    window->listViewItemData.insert(window->listViewItemData.begin() + position,
                        (source.mask & ListViewItemParam) != 0 ? source.itemData : 0);
                    window->listViewItemStates.insert(window->listViewItemStates.begin() + position,
                        (source.mask & ListViewItemState) != 0
                            ? source.state & source.stateMask
                            : 0);
                    window->listViewItemImages.insert(window->listViewItemImages.begin() + position,
                        (source.mask & ListViewItemImage) != 0 ? source.image : -1);
                    if ((window->listViewItemStates[position] &
                        (ListViewStateSelected | ListViewStateFocused)) != 0)
                    {
                        window->listViewSelectedItem = static_cast<int>(position);
                        window->listViewSelectionMark = static_cast<int>(position);
                    }
                    itemIndex = static_cast<int>(position);
                }
                else if (itemIndex >= 0 && static_cast<size_t>(itemIndex) < window->listViewItems.size())
                {
                    auto& row = window->listViewItems[static_cast<size_t>(itemIndex)];
                    const size_t requiredColumns = (std::max)(
                        static_cast<size_t>(subItem + 1), window->listViewColumns.size());
                    if (row.size() < requiredColumns)
                    {
                        row.resize(requiredColumns);
                    }
                    row[static_cast<size_t>(subItem)] = std::move(itemText);
                    if ((source.mask & ListViewItemParam) != 0 &&
                        static_cast<size_t>(itemIndex) < window->listViewItemData.size())
                    {
                        window->listViewItemData[static_cast<size_t>(itemIndex)] = source.itemData;
                    }
                    if ((source.mask & ListViewItemState) != 0 &&
                        static_cast<size_t>(itemIndex) < window->listViewItemStates.size())
                    {
                        auto& state = window->listViewItemStates[static_cast<size_t>(itemIndex)];
                        state = (state & ~source.stateMask) | (source.state & source.stateMask);
                    }
                    if ((source.mask & ListViewItemImage) != 0 &&
                        static_cast<size_t>(itemIndex) < window->listViewItemImages.size())
                    {
                        window->listViewItemImages[static_cast<size_t>(itemIndex)] = source.image;
                    }
                }
                else
                {
                    return FALSE;
                }
            }
            if (g_listViewPopulationDiagnostics < 64)
            {
                ++g_listViewPopulationDiagnostics;
                const wchar_t* operation = message == ListViewInsertItemW
                    ? L"inserted" : (message == ListViewSetItemW ? L"updated" : L"set text for");
                RuntimeDiagnostics::Record(
                    std::wstring(L"LISTVIEW: ") + operation + L" row " +
                    std::to_wstring(itemIndex) + L", column " + std::to_wstring(subItem) +
                    L", item-data " + std::to_wstring(static_cast<unsigned long long>(source.itemData)) + L".");
            }
            InvalidateGuestRect(window->handle, nullptr, TRUE, nullptr);
            return message == ListViewInsertItemW ? itemIndex : TRUE;
        }
    }

    if (controlKind == BuiltinControlKind::Toolbar)
    {
        if (message == ToolbarButtonStructSize)
        {
            return wParam == sizeof(GuestToolbarButton) ? TRUE : FALSE;
        }
        if (message == ToolbarAddStringW)
        {
            std::vector<std::wstring> strings;
            if ((static_cast<ULONG_PTR>(lParam) >> 16) == 0)
            {
                wchar_t resource[512] = {};
                const int length = LoadGuestStringResource(
                    reinterpret_cast<HINSTANCE>(wParam), static_cast<UINT>(lParam),
                    resource, static_cast<int>(_countof(resource)));
                if (length > 1)
                {
                    const wchar_t delimiter = resource[0];
                    const wchar_t* begin = resource + 1;
                    const wchar_t* end = resource + length;
                    while (begin < end)
                    {
                        const wchar_t* next = std::find(begin, end, delimiter);
                        if (next == end) break;
                        strings.emplace_back(begin, next);
                        begin = next + 1;
                    }
                }
                else if (length == 0)
                {
                    return -1;
                }
            }
            else
            {
                const wchar_t* source = reinterpret_cast<const wchar_t*>(lParam);
                if (!source) return -1;
                size_t offset = 0;
                for (size_t count = 0; count < 256; ++count)
                {
                    std::wstring value;
                    if (!TryReadGuestWideString(source + offset, &value)) return -1;
                    if (value.empty()) break;
                    offset += value.size() + 1;
                    strings.push_back(std::move(value));
                }
            }
            std::lock_guard<std::mutex> guard(window->lock);
            const LRESULT first = static_cast<LRESULT>(window->toolbarStrings.size());
            window->toolbarStrings.insert(window->toolbarStrings.end(),
                strings.begin(), strings.end());
            for (size_t index = 0; index < window->toolbarButtonStringRefs.size() &&
                index < window->toolbarButtonTexts.size(); ++index)
            {
                const INT_PTR reference = window->toolbarButtonStringRefs[index];
                if (reference >= 0 && static_cast<size_t>(reference) < window->toolbarStrings.size())
                {
                    window->toolbarButtonTexts[index] =
                        window->toolbarStrings[static_cast<size_t>(reference)];
                }
            }
            if (std::any_of(window->toolbarButtonTexts.begin(), window->toolbarButtonTexts.end(),
                [](const std::wstring& value) { return !value.empty(); }))
            {
                window->toolbarButtonHeight = (std::max)(window->toolbarButtonHeight,
                    window->toolbarBitmapHeight + GuestMetrics::TextHeight +
                    3 * GuestMetrics::ControlVerticalPadding);
            }
            return first;
        }
        if (message == ToolbarGetButtonTextW)
        {
            std::wstring text;
            {
                std::lock_guard<std::mutex> guard(window->lock);
                const auto found = std::find(window->toolbarCommands.begin(),
                    window->toolbarCommands.end(), static_cast<int>(wParam));
                if (found == window->toolbarCommands.end()) return -1;
                const size_t index = static_cast<size_t>(found - window->toolbarCommands.begin());
                if (index < window->toolbarButtonTexts.size())
                    text = window->toolbarButtonTexts[index];
            }
            if (lParam && !TryWriteGuestWideString(reinterpret_cast<wchar_t*>(lParam),
                text.size() + 1, text, nullptr)) return -1;
            return static_cast<LRESULT>(text.size());
        }
        if (message == ToolbarEnableButton || message == ToolbarCheckButton ||
            message == ToolbarPressButton || message == ToolbarHideButton ||
            message == ToolbarIndeterminateButton || message == ToolbarMarkButton ||
            message == ToolbarSetState)
        {
            bool updated = false;
            {
                std::lock_guard<std::mutex> guard(window->lock);
                const auto found = std::find(
                    window->toolbarCommands.begin(), window->toolbarCommands.end(),
                    static_cast<int>(wParam));
                if (found == window->toolbarCommands.end())
                {
                    return FALSE;
                }
                const size_t index = static_cast<size_t>(found - window->toolbarCommands.begin());
                BYTE& state = window->toolbarButtonStates[index];
                if (message == ToolbarSetState)
                {
                    state = static_cast<BYTE>(LOWORD(lParam));
                }
                else
                {
                    BYTE bit = ToolbarStateEnabled;
                    if (message == ToolbarCheckButton) bit = ToolbarStateChecked;
                    else if (message == ToolbarPressButton) bit = ToolbarStatePressed;
                    else if (message == ToolbarHideButton) bit = ToolbarStateHidden;
                    else if (message == ToolbarIndeterminateButton) bit = ToolbarStateIndeterminate;
                    else if (message == ToolbarMarkButton) bit = ToolbarStateMarked;
                    state = LOWORD(lParam) != FALSE
                        ? static_cast<BYTE>(state | bit)
                        : static_cast<BYTE>(state & ~bit);
                }
                updated = true;
            }
            if (updated)
            {
                InvalidateGuestRect(window->handle, nullptr, TRUE, nullptr);
            }
            return TRUE;
        }
        if (message == ToolbarGetState || message == ToolbarIsButtonEnabled ||
            message == ToolbarIsButtonChecked || message == ToolbarIsButtonPressed ||
            message == ToolbarIsButtonHidden || message == ToolbarIsButtonIndeterminate ||
            message == ToolbarIsButtonHighlighted)
        {
            std::lock_guard<std::mutex> guard(window->lock);
            const auto found = std::find(
                window->toolbarCommands.begin(), window->toolbarCommands.end(),
                static_cast<int>(wParam));
            if (found == window->toolbarCommands.end())
            {
                return message == ToolbarGetState ? -1 : FALSE;
            }
            const size_t index = static_cast<size_t>(found - window->toolbarCommands.begin());
            const BYTE state = window->toolbarButtonStates[index];
            if (message == ToolbarGetState) return state;
            if (message == ToolbarIsButtonEnabled) return (state & ToolbarStateEnabled) != 0;
            if (message == ToolbarIsButtonChecked) return (state & ToolbarStateChecked) != 0;
            if (message == ToolbarIsButtonPressed) return (state & ToolbarStatePressed) != 0;
            if (message == ToolbarIsButtonHidden) return (state & ToolbarStateHidden) != 0;
            if (message == ToolbarIsButtonIndeterminate) return (state & ToolbarStateIndeterminate) != 0;
            return (state & ToolbarStateMarked) != 0;
        }
        if (message == ToolbarSetButtonSize)
        {
            {
                std::lock_guard<std::mutex> guard(window->lock);
                window->toolbarButtonWidth = (std::max)(
                    GuestMetrics::DefaultBitmapExtent, static_cast<int>(LOWORD(lParam)));
                window->toolbarButtonHeight = (std::max)(
                    GuestMetrics::DefaultBitmapExtent, static_cast<int>(HIWORD(lParam)));
            }
            InvalidateGuestRect(window->handle, nullptr, TRUE, nullptr);
            return TRUE;
        }
        if (message == ToolbarSetBitmapSize)
        {
            {
                std::lock_guard<std::mutex> guard(window->lock);
                window->toolbarBitmapWidth = (std::max)(1, static_cast<int>(LOWORD(lParam)));
                window->toolbarBitmapHeight = (std::max)(1, static_cast<int>(HIWORD(lParam)));
            }
            InvalidateGuestRect(window->handle, nullptr, TRUE, nullptr);
            return TRUE;
        }
        if (message == ToolbarSetButtonWidth)
        {
            const int minimum = static_cast<int>(LOWORD(lParam));
            const int maximum = static_cast<int>(HIWORD(lParam));
            {
                std::lock_guard<std::mutex> guard(window->lock);
                int requested = (std::max)(window->toolbarButtonWidth,
                    (std::max)(GuestMetrics::DefaultBitmapExtent, minimum));
                if (maximum > 0)
                {
                    requested = (std::min)(requested, maximum);
                }
                window->toolbarButtonWidth = requested;
            }
            InvalidateGuestRect(window->handle, nullptr, TRUE, nullptr);
            return TRUE;
        }
        if (message == ToolbarAddButtonsW)
        {
            const auto buttons = reinterpret_cast<const GuestToolbarButton*>(lParam);
            const size_t count = static_cast<size_t>(wParam);
            if (!buttons || count > 256)
            {
                return FALSE;
            }
            std::vector<GuestToolbarButton> copiedButtons;
            std::vector<std::wstring> knownStrings;
            {
                std::lock_guard<std::mutex> guard(window->lock);
                knownStrings = window->toolbarStrings;
            }
            std::vector<std::wstring> copiedTexts;
            copiedButtons.reserve(count);
            copiedTexts.reserve(count);
            for (size_t index = 0; index < count; ++index)
            {
                GuestToolbarButton button = {};
                if (!TryReadGuestValue(buttons + index, &button))
                {
                    return FALSE;
                }
                copiedButtons.push_back(button);
                std::wstring buttonText;
                if (button.text >= 0 && static_cast<size_t>(button.text) < knownStrings.size())
                {
                    buttonText = knownStrings[static_cast<size_t>(button.text)];
                }
                else if (button.text > 0xffff)
                {
                    TryReadGuestWideString(reinterpret_cast<LPCWSTR>(button.text), &buttonText);
                }
                copiedTexts.push_back(std::move(buttonText));
            }
            {
                std::lock_guard<std::mutex> guard(window->lock);
                for (size_t index = 0; index < copiedButtons.size(); ++index)
                {
                    const auto& button = copiedButtons[index];
                    window->toolbarCommands.push_back(button.command);
                    window->toolbarBitmaps.push_back(button.bitmap);
                    window->toolbarButtonStates.push_back(button.state);
                    window->toolbarButtonStyles.push_back(button.style);
                    window->toolbarButtonData.push_back(button.data);
                    window->toolbarButtonTexts.push_back(copiedTexts[index]);
                    window->toolbarButtonStringRefs.push_back(button.text);
                }
                if (std::any_of(copiedTexts.begin(), copiedTexts.end(),
                    [](const std::wstring& value) { return !value.empty(); }))
                {
                    window->toolbarButtonHeight = (std::max)(window->toolbarButtonHeight,
                        window->toolbarBitmapHeight + GuestMetrics::TextHeight +
                        3 * GuestMetrics::ControlVerticalPadding);
                }
            }
            InvalidateGuestRect(window->handle, nullptr, TRUE, nullptr);
            return TRUE;
        }
        if (message == ToolbarInsertButtonW)
        {
            GuestToolbarButton button = {};
            if (!TryReadGuestValue(reinterpret_cast<const GuestToolbarButton*>(lParam), &button))
            {
                return FALSE;
            }
            std::wstring buttonText;
            {
                std::lock_guard<std::mutex> guard(window->lock);
                if (button.text >= 0 && static_cast<size_t>(button.text) < window->toolbarStrings.size())
                    buttonText = window->toolbarStrings[static_cast<size_t>(button.text)];
            }
            if (buttonText.empty() && button.text > 0xffff)
                TryReadGuestWideString(reinterpret_cast<LPCWSTR>(button.text), &buttonText);
            {
                std::lock_guard<std::mutex> guard(window->lock);
                const size_t position = (std::min)(
                    static_cast<size_t>(wParam), window->toolbarCommands.size());
                window->toolbarCommands.insert(window->toolbarCommands.begin() + position, button.command);
                window->toolbarBitmaps.insert(window->toolbarBitmaps.begin() + position, button.bitmap);
                window->toolbarButtonStates.insert(window->toolbarButtonStates.begin() + position, button.state);
                window->toolbarButtonStyles.insert(window->toolbarButtonStyles.begin() + position, button.style);
                window->toolbarButtonData.insert(window->toolbarButtonData.begin() + position, button.data);
                window->toolbarButtonTexts.insert(window->toolbarButtonTexts.begin() + position,
                    std::move(buttonText));
                window->toolbarButtonStringRefs.insert(
                    window->toolbarButtonStringRefs.begin() + position, button.text);
                if (!window->toolbarButtonTexts[position].empty())
                {
                    window->toolbarButtonHeight = (std::max)(window->toolbarButtonHeight,
                        window->toolbarBitmapHeight + GuestMetrics::TextHeight +
                        3 * GuestMetrics::ControlVerticalPadding);
                }
            }
            InvalidateGuestRect(window->handle, nullptr, TRUE, nullptr);
            return TRUE;
        }
        if (message == ToolbarDeleteButton)
        {
            const size_t index = static_cast<size_t>(wParam);
            {
                std::lock_guard<std::mutex> guard(window->lock);
                if (index >= window->toolbarCommands.size())
                {
                    return FALSE;
                }
                window->toolbarCommands.erase(window->toolbarCommands.begin() + index);
                window->toolbarBitmaps.erase(window->toolbarBitmaps.begin() + index);
                window->toolbarButtonStates.erase(window->toolbarButtonStates.begin() + index);
                window->toolbarButtonStyles.erase(window->toolbarButtonStyles.begin() + index);
                window->toolbarButtonData.erase(window->toolbarButtonData.begin() + index);
                if (index < window->toolbarButtonTexts.size())
                    window->toolbarButtonTexts.erase(window->toolbarButtonTexts.begin() + index);
                if (index < window->toolbarButtonStringRefs.size())
                    window->toolbarButtonStringRefs.erase(window->toolbarButtonStringRefs.begin() + index);
                window->toolbarPressedIndex = -1;
            }
            InvalidateGuestRect(window->handle, nullptr, TRUE, nullptr);
            return TRUE;
        }
        if (message == ToolbarGetButton)
        {
            const size_t index = static_cast<size_t>(wParam);
            GuestToolbarButton result = {};
            {
                std::lock_guard<std::mutex> guard(window->lock);
                if (index >= window->toolbarCommands.size())
                {
                    return FALSE;
                }
                result.bitmap = window->toolbarBitmaps[index];
                result.command = window->toolbarCommands[index];
                result.state = window->toolbarButtonStates[index];
                result.style = window->toolbarButtonStyles[index];
                result.data = window->toolbarButtonData[index];
                result.text = index < window->toolbarButtonStringRefs.size()
                    ? window->toolbarButtonStringRefs[index] : -1;
            }
            return TryWriteGuestValue(reinterpret_cast<GuestToolbarButton*>(lParam), result)
                ? TRUE : FALSE;
        }
        if (message == ToolbarCommandToIndex)
        {
            std::lock_guard<std::mutex> guard(window->lock);
            const auto found = std::find(
                window->toolbarCommands.begin(), window->toolbarCommands.end(),
                static_cast<int>(wParam));
            return found == window->toolbarCommands.end()
                ? -1
                : static_cast<LRESULT>(found - window->toolbarCommands.begin());
        }
        if (message == ToolbarSetCommandId)
        {
            const size_t index = static_cast<size_t>(wParam);
            std::lock_guard<std::mutex> guard(window->lock);
            if (index >= window->toolbarCommands.size())
            {
                return FALSE;
            }
            window->toolbarCommands[index] = static_cast<int>(lParam);
            return TRUE;
        }
        if (message == ToolbarChangeBitmap || message == ToolbarGetBitmap)
        {
            {
                std::lock_guard<std::mutex> guard(window->lock);
                const auto found = std::find(
                    window->toolbarCommands.begin(), window->toolbarCommands.end(),
                    static_cast<int>(wParam));
                if (found == window->toolbarCommands.end())
                {
                    return message == ToolbarGetBitmap ? -1 : FALSE;
                }
                const size_t index = static_cast<size_t>(found - window->toolbarCommands.begin());
                if (message == ToolbarGetBitmap)
                {
                    return window->toolbarBitmaps[index];
                }
                window->toolbarBitmaps[index] = static_cast<int>(lParam);
            }
            InvalidateGuestRect(window->handle, nullptr, TRUE, nullptr);
            return TRUE;
        }
        if (message == ToolbarAutoSize)
        {
            HWND toolbar = nullptr;
            HWND parent = nullptr;
            int toolbarWidth = 0;
            int desiredHeight = 0;
            {
                std::lock_guard<std::mutex> guard(window->lock);
                toolbar = window->handle;
                parent = window->parent;
                toolbarWidth = (std::max)(1, window->surface.Width());
                desiredHeight = (std::min)(GuestMetrics::MaximumControlStripHeight,
                    (std::max)(GuestMetrics::StatusBarMinimumHeight,
                        window->toolbarButtonHeight + 2 * GuestMetrics::Border));
            }
            const auto parentWindow = FindWindow(parent);
            if (parentWindow)
            {
                int parentWidth = 0;
                bool rebarParent = false;
                {
                    std::lock_guard<std::mutex> parentGuard(parentWindow->lock);
                    parentWidth = (std::max)(1, parentWindow->surface.Width());
                    rebarParent = parentWindow->windowClass &&
                        parentWindow->windowClass->builtinKind == BuiltinControlKind::Rebar;
                    if (rebarParent)
                    {
                        for (auto& band : parentWindow->rebarBands)
                        {
                            if (band.child == toolbar)
                            {
                                band.minimumHeight = desiredHeight;
                            }
                        }
                    }
                }
                DWORD ignored = ERROR_SUCCESS;
                if (rebarParent)
                {
                    SetGuestWindowPos(parent, nullptr, 0, 0, parentWidth, desiredHeight,
                        GuestSwpNoMove | GuestSwpNoZOrder, &ignored);
                }
                else
                {
                    // A standalone toolbar is a horizontal control strip.
                    // TB_AUTOSIZE uses its parent's current client width and
                    // the button-derived height; this also keeps it aligned
                    // when the host viewport changes.
                    SetGuestWindowPos(toolbar, nullptr, 0, 0, parentWidth, desiredHeight,
                        GuestSwpNoMove | GuestSwpNoZOrder, &ignored);
                }
            }
            else
            {
                DWORD ignored = ERROR_SUCCESS;
                SetGuestWindowPos(toolbar, nullptr, 0, 0, toolbarWidth, desiredHeight,
                    GuestSwpNoMove | GuestSwpNoZOrder, &ignored);
            }
            InvalidateGuestRect(window->handle, nullptr, TRUE, nullptr);
            RuntimeDiagnostics::Record(L"TOOLBAR: autosized to " + std::to_wstring(desiredHeight) + L"px high.");
            return 0;
        }
        if (message == ToolbarGetButtonCount)
        {
            std::lock_guard<std::mutex> guard(window->lock);
            return static_cast<LRESULT>(window->toolbarCommands.size());
        }
        if (message == ToolbarGetRows)
        {
            return 1;
        }
        if (message == ToolbarGetButtonSize)
        {
            std::lock_guard<std::mutex> guard(window->lock);
            return MAKELONG(window->toolbarButtonWidth, window->toolbarButtonHeight);
        }
        if (message == ToolbarGetMaxSize)
        {
            SIZE result{};
            {
                std::lock_guard<std::mutex> guard(window->lock);
                result.cx = ToolbarContentWidth(
                    window->toolbarButtonStyles,
                    window->toolbarBitmaps,
                    window->toolbarButtonStates,
                    window->toolbarCommands.size(),
                    window->toolbarButtonWidth,
                    &window->toolbarButtonTexts);
                result.cy = (std::min)(GuestMetrics::MaximumControlStripHeight,
                    (std::max)(GuestMetrics::StatusBarMinimumHeight,
                        window->toolbarButtonHeight + 2 * GuestMetrics::Border));
            }
            return TryWriteGuestValue(reinterpret_cast<SIZE*>(lParam), result) ? TRUE : FALSE;
        }
        if (message == ToolbarGetItemRect || message == ToolbarGetRect)
        {
            RECT result = {};
            {
                std::lock_guard<std::mutex> guard(window->lock);
                int index = static_cast<int>(wParam);
                if (message == ToolbarGetRect)
                {
                    const auto found = std::find(
                        window->toolbarCommands.begin(), window->toolbarCommands.end(), index);
                    index = found == window->toolbarCommands.end()
                        ? -1
                        : static_cast<int>(found - window->toolbarCommands.begin());
                }
                if (index < 0 || static_cast<size_t>(index) >= window->toolbarCommands.size())
                {
                    return FALSE;
                }
                const int left = ToolbarItemLeft(window->toolbarButtonStyles, window->toolbarBitmaps,
                    window->toolbarButtonStates, static_cast<size_t>(index), window->toolbarButtonWidth,
                    &window->toolbarButtonTexts);
                result.left = left;
                result.top = 1;
                result.right = left + ToolbarItemWidth(window->toolbarButtonStyles, window->toolbarBitmaps,
                    window->toolbarButtonStates, static_cast<size_t>(index), window->toolbarButtonWidth,
                    &window->toolbarButtonTexts);
                result.bottom = result.top + (std::max)(
                    GuestMetrics::DefaultBitmapExtent, window->toolbarButtonHeight);
            }
            return TryWriteGuestValue(reinterpret_cast<RECT*>(lParam), result) ? TRUE : FALSE;
        }
        if (message == ToolbarHitTest)
        {
            POINT point = {};
            if (!TryReadGuestValue(reinterpret_cast<const POINT*>(lParam), &point))
            {
                return -1;
            }
            std::lock_guard<std::mutex> guard(window->lock);
            for (size_t index = 0; index < window->toolbarCommands.size(); ++index)
            {
                const BYTE state = window->toolbarButtonStates[index];
                if ((state & ToolbarStateHidden) != 0)
                {
                    continue;
                }
                const int left = ToolbarItemLeft(window->toolbarButtonStyles, window->toolbarBitmaps,
                    window->toolbarButtonStates, index, window->toolbarButtonWidth,
                    &window->toolbarButtonTexts);
                const int right = left + ToolbarItemWidth(window->toolbarButtonStyles,
                    window->toolbarBitmaps, window->toolbarButtonStates, index, window->toolbarButtonWidth,
                    &window->toolbarButtonTexts);
                if (point.x >= left && point.x < right && point.y >= 0 &&
                    point.y < window->toolbarButtonHeight + 2 * GuestMetrics::Border)
                {
                    return static_cast<LRESULT>(index);
                }
            }
            return -1;
        }
        if (message == ToolbarSetImageList)
        {
            HANDLE previous = nullptr;
            {
                std::lock_guard<std::mutex> guard(window->lock);
                previous = window->toolbarImageList;
                window->toolbarImageList = reinterpret_cast<HANDLE>(lParam);
            }
            InvalidateGuestRect(window->handle, nullptr, TRUE, nullptr);
            return reinterpret_cast<LRESULT>(previous);
        }
        if (message == ToolbarGetImageList)
        {
            std::lock_guard<std::mutex> guard(window->lock);
            return reinterpret_cast<LRESULT>(window->toolbarImageList);
        }
    }

    if (controlKind == BuiltinControlKind::Rebar &&
        (message == RebarInsertBandW || message == RebarSetBandInfoW))
    {
        GuestRebarBandInfoW source = {};
        if (!TryReadGuestValue(reinterpret_cast<const GuestRebarBandInfoW*>(lParam), &source) ||
            source.size < offsetof(GuestRebarBandInfoW, width) + sizeof(source.width))
        {
            return FALSE;
        }

        RuntimeDiagnostics::Record(L"REBAR BAND INFO: mask " +
            std::to_wstring(source.mask) + L", style " +
            std::to_wstring(source.style) + L", minimum " +
            std::to_wstring(source.minimumChildWidth) + L"x" +
            std::to_wstring(source.minimumChildHeight) + L", width " +
            std::to_wstring(source.width) + L".");
        int rebarWidth = GuestMetrics::CurrentScreenWidth();
        int rebarHeight = GuestMetrics::ToolbarHeight;
        HWND rebar = nullptr;
        HWND hostedChild = nullptr;
        size_t affectedIndex = 0;
        {
            std::lock_guard<std::mutex> guard(window->lock);
            const size_t requestedIndex = static_cast<size_t>(wParam);
            if (message == RebarInsertBandW)
            {
                RebarBand inserted;
                if ((source.mask & RebarBandMaskChild) != 0) inserted.child = source.child;
                if ((source.mask & RebarBandMaskStyle) != 0) inserted.style = source.style;
                if ((source.mask & RebarBandMaskChildSize) != 0)
                {
                    inserted.minimumWidth = ValidatedRebarDimension(source.minimumChildWidth);
                    inserted.minimumHeight = ValidatedRebarChildHeight(source.minimumChildHeight);
                }
                if ((source.mask & RebarBandMaskSize) != 0)
                {
                    inserted.width = ValidatedRebarDimension(source.width);
                }
                const size_t insertion = (std::min)(requestedIndex, window->rebarBands.size());
                window->rebarBands.insert(window->rebarBands.begin() + insertion, inserted);
                hostedChild = inserted.child;
                affectedIndex = insertion;
            }
            else
            {
                if (requestedIndex >= window->rebarBands.size())
                {
                    return FALSE;
                }
                RebarBand& existing = window->rebarBands[requestedIndex];
                if ((source.mask & RebarBandMaskChild) != 0)
                {
                    existing.child = source.child;
                }
                if ((source.mask & RebarBandMaskStyle) != 0)
                {
                    existing.style = source.style;
                }
                if ((source.mask & RebarBandMaskChildSize) != 0)
                {
                    existing.minimumWidth = ValidatedRebarDimension(source.minimumChildWidth);
                    existing.minimumHeight = ValidatedRebarChildHeight(source.minimumChildHeight);
                }
                if ((source.mask & RebarBandMaskSize) != 0)
                {
                    existing.width = ValidatedRebarDimension(source.width);
                }
                hostedChild = existing.child;
                affectedIndex = requestedIndex;
            }
            rebarWidth = (std::max)(1, window->surface.Width());
            rebar = window->handle;
        }

        int naturalWidth = 0;
        int naturalHeight = 0;
        if (const auto child = FindWindow(hostedChild))
        {
            std::lock_guard<std::mutex> childGuard(child->lock);
            if (!child->destroyed)
            {
                naturalWidth = child->surface.Width();
                naturalHeight = child->surface.Height();
                if (child->windowClass &&
                    child->windowClass->builtinKind == BuiltinControlKind::Toolbar)
                {
                    const int contentWidth = ToolbarContentWidth(
                        child->toolbarButtonStyles,
                        child->toolbarBitmaps,
                        child->toolbarButtonStates,
                        child->toolbarCommands.size(),
                        child->toolbarButtonWidth,
                        &child->toolbarButtonTexts);
                    if (contentWidth > 0) naturalWidth = contentWidth;
                }
                else if (child->windowClass &&
                    child->windowClass->builtinKind == BuiltinControlKind::ComboBox &&
                    (child->style & 0x0003u) != ComboBoxStyleSimple)
                {
                    naturalHeight = (std::min)(naturalHeight,
                        GuestMetrics::ControlHeightForText(GuestMetrics::TextHeight));
                }
            }
        }
        {
            std::lock_guard<std::mutex> guard(window->lock);
            if (affectedIndex < window->rebarBands.size() &&
                window->rebarBands[affectedIndex].child == hostedChild)
            {
                window->rebarBands[affectedIndex].naturalWidth = naturalWidth;
                window->rebarBands[affectedIndex].naturalHeight = naturalHeight;
            }
            rebarHeight = RequiredRebarHeight(window->rebarBands, rebarWidth);
        }
        DWORD ignored = ERROR_SUCCESS;
        SetGuestWindowPos(rebar, nullptr, 0, 0, rebarWidth, rebarHeight, GuestSwpNoZOrder, &ignored);
        LayoutGuestRebar(rebar);
        RuntimeDiagnostics::Record(L"REBAR: inserted or updated a band" +
            std::wstring(hostedChild ? L" with a hosted child." : L"."));
        InvalidateGuestRect(window->handle, nullptr, TRUE, nullptr);
        return TRUE;
    }

    if (controlKind == BuiltinControlKind::Rebar)
    {
        if (message == RebarSetBarInfo)
        {
            // RB_SETBARINFO only establishes optional image-list metadata.
            // The bridge keeps image data guest-local, but success is needed
            // for callers to continue into their normal band/layout setup.
            return TRUE;
        }
        if (message == RebarGetBandCount)
        {
            std::lock_guard<std::mutex> guard(window->lock);
            return static_cast<LRESULT>(window->rebarBands.size());
        }
        if (message == RebarGetBarHeight)
        {
            std::lock_guard<std::mutex> guard(window->lock);
            return RequiredRebarHeight(window->rebarBands, window->surface.Width());
        }
        if (message == RebarGetRowHeight)
        {
            std::lock_guard<std::mutex> guard(window->lock);
            const auto heights = RebarRowHeights(window->rebarBands, window->surface.Width());
            const size_t row = static_cast<size_t>(wParam);
            return row < heights.size() ? heights[row] : 0;
        }
        if (message == RebarSizeToRect)
        {
            RECT requested = {};
            if (!TryReadGuestValue(reinterpret_cast<const RECT*>(lParam), &requested))
            {
                return FALSE;
            }
            int height = GuestMetrics::ToolbarHeight;
            {
                std::lock_guard<std::mutex> guard(window->lock);
                height = RequiredRebarHeight(window->rebarBands,
                    (std::max)(1L, requested.right - requested.left));
            }
            requested.top += height;
            return TryWriteGuestValue(reinterpret_cast<RECT*>(lParam), requested) ? TRUE : FALSE;
        }
    }

    if (controlKind == BuiltinControlKind::StatusBar)
    {
        if (message == GuestAbi::WmSize)
        {
            HWND status = nullptr;
            HWND parent = nullptr;
            int currentLeft = 0;
            int currentTop = 0;
            int currentWidth = 0;
            int currentHeight = 0;
            int desiredHeight = GuestMetrics::StatusBarHeight;
            {
                std::lock_guard<std::mutex> guard(window->lock);
                status = window->handle;
                parent = window->parent;
                currentLeft = window->bounds.left;
                currentTop = window->bounds.top;
                currentWidth = window->surface.Width();
                currentHeight = window->surface.Height();
                desiredHeight = (std::max)(window->statusBarMinimumHeight,
                    GuestMetrics::StatusBarHeight);
            }
            RECT parentClient = {};
            DWORD ignored = ERROR_SUCCESS;
            if (parent && GetGuestClientRect(parent, &parentClient, &ignored))
            {
                const int desiredWidth = (std::max)(0,
                    static_cast<int>(parentClient.right - parentClient.left));
                const int desiredTop = (std::max)(0,
                    static_cast<int>(parentClient.bottom - parentClient.top) - desiredHeight);
                if (currentLeft != 0 || currentTop != desiredTop ||
                    currentWidth != desiredWidth || currentHeight != desiredHeight)
                {
                    SetGuestWindowPos(status, nullptr, 0, desiredTop,
                        desiredWidth, desiredHeight,
                        GuestSwpNoZOrder | GuestSwpNoActivate, &ignored);
                }
            }
            InvalidateGuestRect(status, nullptr, TRUE, nullptr);
            return 0;
        }
        if (message == StatusBarSetParts)
        {
            const size_t count = static_cast<size_t>(wParam);
            const auto source = reinterpret_cast<const int*>(lParam);
            if (count == 0 || count > 256 || !source)
            {
                return FALSE;
            }
            std::vector<int> parts(count);
            for (size_t index = 0; index < count; ++index)
            {
                if (!TryReadGuestValue(source + index, &parts[index]))
                {
                    return FALSE;
                }
            }
            {
                std::lock_guard<std::mutex> guard(window->lock);
                window->statusBarParts = std::move(parts);
                window->statusBarTexts.resize(count);
                window->statusBarTextStyles.resize(count);
            }
            InvalidateGuestRect(window->handle, nullptr, TRUE, nullptr);
            return TRUE;
        }
        if (message == StatusBarGetParts)
        {
            std::vector<int> parts;
            {
                std::lock_guard<std::mutex> guard(window->lock);
                parts = window->statusBarParts;
            }
            const size_t requested = static_cast<size_t>(wParam);
            auto destination = reinterpret_cast<int*>(lParam);
            if (destination)
            {
                const size_t count = (std::min)(requested, parts.size());
                for (size_t index = 0; index < count; ++index)
                {
                    if (!TryWriteGuestValue(destination + index, parts[index]))
                    {
                        return 0;
                    }
                }
            }
            return static_cast<LRESULT>(parts.size());
        }
        if (message == StatusBarSetTextW)
        {
            const UINT part = LOWORD(wParam);
            const UINT style = static_cast<UINT>(wParam) & 0xff00u;
            std::wstring text;
            if (lParam && !TryReadGuestWideString(reinterpret_cast<LPCWSTR>(lParam), &text))
            {
                return FALSE;
            }
            {
                std::lock_guard<std::mutex> guard(window->lock);
                if (part == 0xffu)
                {
                    window->statusBarSimpleText = std::move(text);
                    window->statusBarSimpleStyle = style;
                }
                else
                {
                    if (part >= window->statusBarTexts.size())
                    {
                        return FALSE;
                    }
                    window->statusBarTexts[part] = std::move(text);
                    window->statusBarTextStyles[part] = style;
                }
            }
            InvalidateGuestRect(window->handle, nullptr, TRUE, nullptr);
            return TRUE;
        }
        if (message == StatusBarGetTextW || message == StatusBarGetTextLengthW)
        {
            const UINT part = LOWORD(wParam);
            std::wstring text;
            UINT style = 0;
            {
                std::lock_guard<std::mutex> guard(window->lock);
                if (part == 0xffu)
                {
                    text = window->statusBarSimpleText;
                    style = window->statusBarSimpleStyle;
                }
                else
                {
                    if (part >= window->statusBarTexts.size())
                    {
                        return 0;
                    }
                    text = window->statusBarTexts[part];
                    style = window->statusBarTextStyles[part];
                }
            }
            if (message == StatusBarGetTextW &&
                !TryWriteGuestWideString(reinterpret_cast<LPWSTR>(lParam), text.size() + 1, text, nullptr))
            {
                return 0;
            }
            return MAKELONG(static_cast<WORD>((std::min)(text.size(), static_cast<size_t>(0xffff))),
                static_cast<WORD>(style));
        }
        if (message == StatusBarGetRect)
        {
            const size_t part = static_cast<size_t>(wParam);
            RECT result = {};
            {
                std::lock_guard<std::mutex> guard(window->lock);
                if (part >= window->statusBarParts.size())
                {
                    return FALSE;
                }
                result.left = part == 0 ? 0 : window->statusBarParts[part - 1];
                result.right = window->statusBarParts[part] < 0
                    ? window->surface.Width()
                    : window->statusBarParts[part];
                result.top = 0;
                result.bottom = window->surface.Height();
            }
            return TryWriteGuestValue(reinterpret_cast<RECT*>(lParam), result) ? TRUE : FALSE;
        }
        if (message == StatusBarSetMinimumHeight)
        {
            std::lock_guard<std::mutex> guard(window->lock);
            window->statusBarMinimumHeight = (std::max)(1, static_cast<int>(wParam));
            return 0;
        }
        if (message == StatusBarSetSimple)
        {
            {
                std::lock_guard<std::mutex> guard(window->lock);
                window->statusBarSimple = wParam != FALSE;
            }
            InvalidateGuestRect(window->handle, nullptr, TRUE, nullptr);
            return TRUE;
        }
        if (message == StatusBarIsSimple)
        {
            std::lock_guard<std::mutex> guard(window->lock);
            return window->statusBarSimple ? TRUE : FALSE;
        }
    }

    if (controlKind == BuiltinControlKind::Header)
    {
        if (message == HeaderGetItemCount)
        {
            std::lock_guard<std::mutex> guard(window->lock);
            return static_cast<LRESULT>(window->headerItems.size());
        }
        if (message == HeaderSetImageList || message == HeaderGetImageList)
        {
            std::lock_guard<std::mutex> guard(window->lock);
            if (message == HeaderGetImageList) return reinterpret_cast<LRESULT>(window->headerImageList);
            const HANDLE previous = window->headerImageList;
            window->headerImageList = reinterpret_cast<HANDLE>(lParam);
            return reinterpret_cast<LRESULT>(previous);
        }
        if (message == HeaderInsertItemW || message == HeaderSetItemW || message == HeaderGetItemW)
        {
            auto guestItem = reinterpret_cast<GuestHeaderItemW*>(lParam);
            GuestHeaderItemW item = {};
            if (!TryReadGuestValue(guestItem, &item))
                return message == HeaderInsertItemW ? -1 : FALSE;
            const int requested = static_cast<int>(wParam);
            if (message == HeaderGetItemW)
            {
                std::wstring text;
                {
                    std::lock_guard<std::mutex> guard(window->lock);
                    if (requested < 0 || static_cast<size_t>(requested) >= window->headerItems.size()) return FALSE;
                    const size_t index = static_cast<size_t>(requested);
                    text = window->headerItems[index];
                    if ((item.mask & HeaderItemWidth) != 0) item.width = window->headerItemWidths[index];
                    if ((item.mask & HeaderItemFormat) != 0) item.format = window->headerItemFormats[index];
                    if ((item.mask & HeaderItemParam) != 0) item.itemData = window->headerItemData[index];
                    if ((item.mask & HeaderItemImage) != 0) item.image = window->headerItemImages[index];
                    if ((item.mask & HeaderItemOrder) != 0) item.order = window->headerItemOrders[index];
                }
                if ((item.mask & HeaderItemText) != 0 &&
                    !TryWriteGuestWideString(item.text,
                        static_cast<size_t>((std::max)(0, item.textCapacity)), text, nullptr)) return FALSE;
                return TryWriteGuestValue(guestItem, item) ? TRUE : FALSE;
            }
            std::wstring text;
            if ((item.mask & HeaderItemText) != 0 && !TryReadGuestWideString(item.text, &text))
                return message == HeaderInsertItemW ? -1 : FALSE;
            int result = FALSE;
            {
                std::lock_guard<std::mutex> guard(window->lock);
                if (message == HeaderInsertItemW)
                {
                    const size_t position = requested < 0 ? window->headerItems.size() :
                        (std::min)(static_cast<size_t>(requested), window->headerItems.size());
                    window->headerItems.insert(window->headerItems.begin() + position, std::move(text));
                    window->headerItemWidths.insert(window->headerItemWidths.begin() + position,
                        (item.mask & HeaderItemWidth) != 0 ? (std::max)(0, item.width) : 120);
                    window->headerItemFormats.insert(window->headerItemFormats.begin() + position,
                        (item.mask & HeaderItemFormat) != 0 ? item.format : 0);
                    window->headerItemData.insert(window->headerItemData.begin() + position,
                        (item.mask & HeaderItemParam) != 0 ? item.itemData : 0);
                    window->headerItemImages.insert(window->headerItemImages.begin() + position,
                        (item.mask & HeaderItemImage) != 0 ? item.image : -1);
                    window->headerItemOrders.insert(window->headerItemOrders.begin() + position,
                        (item.mask & HeaderItemOrder) != 0 ? item.order : static_cast<int>(position));
                    result = static_cast<int>(position);
                }
                else
                {
                    if (requested < 0 || static_cast<size_t>(requested) >= window->headerItems.size()) return FALSE;
                    const size_t index = static_cast<size_t>(requested);
                    if ((item.mask & HeaderItemText) != 0) window->headerItems[index] = std::move(text);
                    if ((item.mask & HeaderItemWidth) != 0) window->headerItemWidths[index] = (std::max)(0, item.width);
                    if ((item.mask & HeaderItemFormat) != 0) window->headerItemFormats[index] = item.format;
                    if ((item.mask & HeaderItemParam) != 0) window->headerItemData[index] = item.itemData;
                    if ((item.mask & HeaderItemImage) != 0) window->headerItemImages[index] = item.image;
                    if ((item.mask & HeaderItemOrder) != 0) window->headerItemOrders[index] = item.order;
                    result = TRUE;
                }
            }
            InvalidateGuestRect(window->handle, nullptr, TRUE, nullptr);
            return result;
        }
        if (message == HeaderDeleteItem)
        {
            const size_t index = static_cast<size_t>(wParam);
            {
                std::lock_guard<std::mutex> guard(window->lock);
                if (index >= window->headerItems.size()) return FALSE;
                window->headerItems.erase(window->headerItems.begin() + index);
                window->headerItemWidths.erase(window->headerItemWidths.begin() + index);
                window->headerItemFormats.erase(window->headerItemFormats.begin() + index);
                window->headerItemData.erase(window->headerItemData.begin() + index);
                window->headerItemImages.erase(window->headerItemImages.begin() + index);
                window->headerItemOrders.erase(window->headerItemOrders.begin() + index);
                for (size_t item = 0; item < window->headerItemOrders.size(); ++item)
                    window->headerItemOrders[item] = static_cast<int>(item);
            }
            InvalidateGuestRect(window->handle, nullptr, TRUE, nullptr);
            return TRUE;
        }
        if (message == HeaderGetItemRect || message == HeaderHitTest)
        {
            std::vector<int> widths;
            std::vector<int> orders;
            int headerHeight = GuestMetrics::ListViewHeaderHeight;
            {
                std::lock_guard<std::mutex> guard(window->lock);
                widths = window->headerItemWidths;
                orders = window->headerItemOrders;
                headerHeight = window->listViewHeaderHeight;
            }
            std::vector<size_t> display(widths.size());
            std::iota(display.begin(), display.end(), static_cast<size_t>(0));
            if (orders.size() == display.size())
                std::stable_sort(display.begin(), display.end(), [&orders](size_t left, size_t right)
                { return orders[left] < orders[right]; });
            int left = 0;
            if (message == HeaderGetItemRect)
            {
                const size_t requested = static_cast<size_t>(wParam);
                if (requested >= widths.size()) return FALSE;
                for (const size_t item : display)
                {
                    if (item == requested) break;
                    left += widths[item];
                }
                RECT result{ left, 0, left + widths[requested], headerHeight };
                return TryWriteGuestValue(reinterpret_cast<RECT*>(lParam), result) ? TRUE : FALSE;
            }
            auto destination = reinterpret_cast<GuestHeaderHitTestInfo*>(lParam);
            GuestHeaderHitTestInfo hit = {};
            if (!TryReadGuestValue(destination, &hit)) return -1;
            hit.item = -1;
            hit.flags = 1;
            for (const size_t item : display)
            {
                const int right = left + widths[item];
                if (hit.point.x >= left && hit.point.x < right && hit.point.y >= 0 &&
                    hit.point.y < headerHeight)
                {
                    hit.item = static_cast<int>(item);
                    hit.flags = 2;
                    break;
                }
                left = right;
            }
            TryWriteGuestValue(destination, hit);
            return hit.item;
        }
        if (message == HeaderLayout)
        {
            GuestHeaderLayout layout = {};
            if (!TryReadGuestValue(reinterpret_cast<const GuestHeaderLayout*>(lParam), &layout)) return FALSE;
            RECT available = {};
            GuestWindowPosition position = {};
            if (!TryReadGuestValue(layout.rect, &available) ||
                !TryReadGuestValue(layout.windowPosition, &position)) return FALSE;
            int headerHeight = GuestMetrics::ListViewHeaderHeight;
            {
                std::lock_guard<std::mutex> guard(window->lock);
                headerHeight = window->listViewHeaderHeight;
            }
            position.x = available.left;
            position.y = available.top;
            position.cx = (std::max)(0L, available.right - available.left);
            position.cy = headerHeight;
            position.flags = 0;
            available.top = (std::min)(available.bottom, available.top + headerHeight);
            return TryWriteGuestValue(layout.rect, available) &&
                TryWriteGuestValue(layout.windowPosition, position) ? TRUE : FALSE;
        }
        if (message == HeaderOrderToIndex)
        {
            const int requested = static_cast<int>(wParam);
            std::lock_guard<std::mutex> guard(window->lock);
            const auto found = std::find(window->headerItemOrders.begin(),
                window->headerItemOrders.end(), requested);
            return found == window->headerItemOrders.end()
                ? -1 : static_cast<LRESULT>(found - window->headerItemOrders.begin());
        }
        if (message == HeaderGetOrderArray || message == HeaderSetOrderArray)
        {
            const size_t count = static_cast<size_t>(wParam);
            auto values = reinterpret_cast<int*>(lParam);
            if (!values || count > 256) return FALSE;
            if (message == HeaderSetOrderArray)
            {
                std::vector<int> display(count);
                for (size_t position = 0; position < count; ++position)
                    if (!TryReadGuestValue(values + position, &display[position])) return FALSE;
                std::lock_guard<std::mutex> guard(window->lock);
                if (count != window->headerItems.size()) return FALSE;
                for (size_t position = 0; position < count; ++position)
                {
                    const int item = display[position];
                    if (item < 0 || static_cast<size_t>(item) >= count) return FALSE;
                    window->headerItemOrders[static_cast<size_t>(item)] = static_cast<int>(position);
                }
                return TRUE;
            }
            std::vector<int> display(count);
            {
                std::lock_guard<std::mutex> guard(window->lock);
                if (count != window->headerItems.size()) return FALSE;
                for (size_t item = 0; item < count; ++item)
                    display[static_cast<size_t>(window->headerItemOrders[item])] = static_cast<int>(item);
            }
            for (size_t position = 0; position < count; ++position)
                if (!TryWriteGuestValue(values + position, display[position])) return FALSE;
            return TRUE;
        }
    }

    if (controlKind == BuiltinControlKind::TreeView)
    {
        const auto treeToken = [](HANDLE item)
        {
            return reinterpret_cast<ULONG_PTR>(item);
        };
        const auto isTreeRoot = [&treeToken](HANDLE item)
        {
            return item == nullptr || static_cast<INT_PTR>(treeToken(item)) == -0x10000;
        };

        if (message == TreeGetCount)
        {
            std::lock_guard<std::mutex> guard(window->lock);
            return static_cast<LRESULT>(window->treeNodes.size());
        }
        if (message == TreeGetIndent || message == TreeSetIndent)
        {
            if (message == TreeGetIndent)
            {
                std::lock_guard<std::mutex> guard(window->lock);
                return window->treeIndent;
            }
            {
                std::lock_guard<std::mutex> guard(window->lock);
                window->treeIndent = (std::max)(0, static_cast<int>(wParam));
            }
            InvalidateGuestRect(window->handle, nullptr, TRUE, nullptr);
            return 0;
        }
        if (message == TreeGetItemHeight || message == TreeSetItemHeight)
        {
            int previous = 0;
            {
                std::lock_guard<std::mutex> guard(window->lock);
                previous = window->treeItemHeight;
                if (message == TreeGetItemHeight) return previous;
                window->treeItemHeight = wParam == static_cast<WPARAM>(-1)
                    ? 18 : (std::max)(1, static_cast<int>(wParam));
            }
            InvalidateGuestRect(window->handle, nullptr, TRUE, nullptr);
            return previous;
        }
        if (message == TreeGetImageList || message == TreeSetImageList)
        {
            const size_t imageList = static_cast<size_t>(wParam);
            if (imageList >= 2) return 0;
            if (message == TreeGetImageList)
            {
                std::lock_guard<std::mutex> guard(window->lock);
                return reinterpret_cast<LRESULT>(window->treeImageLists[imageList]);
            }
            HANDLE previous = nullptr;
            {
                std::lock_guard<std::mutex> guard(window->lock);
                previous = window->treeImageLists[imageList];
                window->treeImageLists[imageList] = reinterpret_cast<HANDLE>(lParam);
            }
            InvalidateGuestRect(window->handle, nullptr, TRUE, nullptr);
            return reinterpret_cast<LRESULT>(previous);
        }
        if (message == TreeSetBackgroundColor || message == TreeSetTextColor ||
            message == TreeGetBackgroundColor || message == TreeGetTextColor)
        {
            if (message == TreeGetBackgroundColor || message == TreeGetTextColor)
            {
                std::lock_guard<std::mutex> guard(window->lock);
                return message == TreeGetBackgroundColor
                    ? window->treeBackgroundColor : window->treeTextColor;
            }
            COLORREF previous = 0;
            {
                std::lock_guard<std::mutex> guard(window->lock);
                COLORREF* target = message == TreeSetBackgroundColor
                    ? &window->treeBackgroundColor : &window->treeTextColor;
                previous = *target;
                *target = static_cast<COLORREF>(lParam);
            }
            InvalidateGuestRect(window->handle, nullptr, TRUE, nullptr);
            return previous;
        }
        if (message == TreeInsertItemW)
        {
            GuestTreeInsertW insertion = {};
            if (!TryReadGuestValue(reinterpret_cast<const GuestTreeInsertW*>(lParam), &insertion))
                return 0;
            std::wstring text;
            if ((insertion.item.mask & TreeItemText) != 0 &&
                reinterpret_cast<INT_PTR>(insertion.item.text) != -1 &&
                !TryReadGuestWideString(insertion.item.text, &text))
                return 0;

            ULONG_PTR result = 0;
            {
                std::lock_guard<std::mutex> guard(window->lock);
                const ULONG_PTR parent = isTreeRoot(insertion.parent) ? 0 : treeToken(insertion.parent);
                if (parent != 0 && !FindTreeNode(window->treeNodes, parent)) return 0;
                do
                {
                    result = window->nextTreeToken++;
                } while (result == 0 || FindTreeNode(window->treeNodes, result));
                TreeNode node;
                node.token = result;
                node.parent = parent;
                node.text = std::move(text);
                node.itemData = (insertion.item.mask & TreeItemParam) != 0
                    ? insertion.item.itemData : 0;
                node.state = (insertion.item.mask & TreeItemState) != 0
                    ? insertion.item.state & insertion.item.stateMask : 0;
                node.image = (insertion.item.mask & TreeItemImage) != 0
                    ? insertion.item.image : -1;
                node.selectedImage = (insertion.item.mask & TreeItemSelectedImage) != 0
                    ? insertion.item.selectedImage : -1;
                node.declaredChildren = (insertion.item.mask & TreeItemChildren) != 0
                    ? insertion.item.children : 0;

                const INT_PTR insertAfter = reinterpret_cast<INT_PTR>(insertion.insertAfter);
                auto position = window->treeNodes.end();
                if (insertAfter == -0xffff)
                {
                    position = std::find_if(window->treeNodes.begin(), window->treeNodes.end(),
                        [parent](const TreeNode& candidate) { return candidate.parent == parent; });
                }
                else if (insertAfter == -0xfffd)
                {
                    position = std::find_if(window->treeNodes.begin(), window->treeNodes.end(),
                        [&node, parent](const TreeNode& candidate)
                        {
                            return candidate.parent == parent && _wcsicmp(candidate.text.c_str(), node.text.c_str()) > 0;
                        });
                }
                else if (insertAfter > 0)
                {
                    const ULONG_PTR afterToken = treeToken(insertion.insertAfter);
                    const auto found = std::find_if(window->treeNodes.begin(), window->treeNodes.end(),
                        [afterToken, parent](const TreeNode& candidate)
                        {
                            return candidate.token == afterToken && candidate.parent == parent;
                        });
                    if (found != window->treeNodes.end()) position = found + 1;
                }
                window->treeNodes.insert(position, std::move(node));
            }
            InvalidateGuestRect(window->handle, nullptr, TRUE, nullptr);
            return reinterpret_cast<LRESULT>(reinterpret_cast<HANDLE>(result));
        }
        if (message == TreeGetItemW || message == TreeSetItemW)
        {
            auto destination = reinterpret_cast<GuestTreeItemW*>(lParam);
            GuestTreeItemW item = {};
            if (!TryReadGuestValue(destination, &item)) return FALSE;
            const ULONG_PTR token = treeToken(item.item);
            if (message == TreeGetItemW)
            {
                std::wstring text;
                {
                    std::lock_guard<std::mutex> guard(window->lock);
                    const TreeNode* node = FindTreeNode(window->treeNodes, token);
                    if (!node) return FALSE;
                    text = node->text;
                    if ((item.mask & TreeItemHandle) != 0) item.item = reinterpret_cast<HANDLE>(node->token);
                    if ((item.mask & TreeItemState) != 0) item.state = node->state & item.stateMask;
                    if ((item.mask & TreeItemImage) != 0) item.image = node->image;
                    if ((item.mask & TreeItemSelectedImage) != 0) item.selectedImage = node->selectedImage;
                    if ((item.mask & TreeItemChildren) != 0)
                    {
                        const bool actualChildren = std::any_of(window->treeNodes.begin(), window->treeNodes.end(),
                            [token](const TreeNode& candidate) { return candidate.parent == token; });
                        item.children = actualChildren ? 1 : node->declaredChildren;
                    }
                    if ((item.mask & TreeItemParam) != 0) item.itemData = node->itemData;
                }
                if ((item.mask & TreeItemText) != 0 &&
                    !TryWriteGuestWideString(item.text,
                        static_cast<size_t>((std::max)(0, item.textCapacity)), text, nullptr)) return FALSE;
                return TryWriteGuestValue(destination, item) ? TRUE : FALSE;
            }

            std::wstring text;
            if ((item.mask & TreeItemText) != 0 &&
                reinterpret_cast<INT_PTR>(item.text) != -1 &&
                !TryReadGuestWideString(item.text, &text)) return FALSE;
            {
                std::lock_guard<std::mutex> guard(window->lock);
                TreeNode* node = FindTreeNode(window->treeNodes, token);
                if (!node) return FALSE;
                if ((item.mask & TreeItemText) != 0) node->text = std::move(text);
                if ((item.mask & TreeItemState) != 0)
                    node->state = (node->state & ~item.stateMask) | (item.state & item.stateMask);
                if ((item.mask & TreeItemImage) != 0) node->image = item.image;
                if ((item.mask & TreeItemSelectedImage) != 0) node->selectedImage = item.selectedImage;
                if ((item.mask & TreeItemChildren) != 0) node->declaredChildren = item.children;
                if ((item.mask & TreeItemParam) != 0) node->itemData = item.itemData;
                if ((node->state & TreeStateSelected) != 0) window->treeSelectedItem = token;
            }
            InvalidateGuestRect(window->handle, nullptr, TRUE, nullptr);
            return TRUE;
        }
        if (message == TreeDeleteItem)
        {
            const HANDLE requested = reinterpret_cast<HANDLE>(lParam);
            bool changed = false;
            {
                std::lock_guard<std::mutex> guard(window->lock);
                if (isTreeRoot(requested))
                {
                    changed = !window->treeNodes.empty();
                    window->treeNodes.clear();
                    window->treeSelectedItem = 0;
                    window->treeTopItem = 0;
                }
                else
                {
                    const ULONG_PTR token = treeToken(requested);
                    if (!FindTreeNode(window->treeNodes, token)) return FALSE;
                    std::vector<ULONG_PTR> removal{ token };
                    for (size_t index = 0; index < removal.size(); ++index)
                    {
                        const ULONG_PTR parent = removal[index];
                        for (const auto& node : window->treeNodes)
                            if (node.parent == parent) removal.push_back(node.token);
                    }
                    window->treeNodes.erase(std::remove_if(window->treeNodes.begin(), window->treeNodes.end(),
                        [&removal](const TreeNode& node)
                        {
                            return std::find(removal.begin(), removal.end(), node.token) != removal.end();
                        }), window->treeNodes.end());
                    if (std::find(removal.begin(), removal.end(), window->treeSelectedItem) != removal.end())
                        window->treeSelectedItem = 0;
                    if (std::find(removal.begin(), removal.end(), window->treeTopItem) != removal.end())
                        window->treeTopItem = 0;
                    changed = true;
                }
            }
            if (changed) InvalidateGuestRect(window->handle, nullptr, TRUE, nullptr);
            return TRUE;
        }
        if (message == TreeExpand)
        {
            bool changed = false;
            {
                std::lock_guard<std::mutex> guard(window->lock);
                TreeNode* node = FindTreeNode(window->treeNodes, treeToken(reinterpret_cast<HANDLE>(lParam)));
                if (!node) return FALSE;
                const UINT operation = static_cast<UINT>(wParam) & 0x000f;
                const UINT previous = node->state;
                if (operation == TreeExpandCollapse) node->state &= ~TreeStateExpanded;
                else if (operation == TreeExpandExpand) node->state |= TreeStateExpanded;
                else if (operation == TreeExpandToggle) node->state ^= TreeStateExpanded;
                changed = previous != node->state;
            }
            if (changed) InvalidateGuestRect(window->handle, nullptr, TRUE, nullptr);
            return TRUE;
        }
        if (message == TreeGetItemState)
        {
            std::lock_guard<std::mutex> guard(window->lock);
            const TreeNode* node = FindTreeNode(window->treeNodes, treeToken(reinterpret_cast<HANDLE>(wParam)));
            return node ? node->state & static_cast<UINT>(lParam) : 0;
        }
        if (message == TreeGetNextItem)
        {
            std::lock_guard<std::mutex> guard(window->lock);
            const UINT relation = static_cast<UINT>(wParam);
            const ULONG_PTR token = treeToken(reinterpret_cast<HANDLE>(lParam));
            const auto visible = VisibleTreeNodes(window->treeNodes);
            ULONG_PTR result = 0;
            if (relation == TreeCaret) result = window->treeSelectedItem;
            else if (relation == TreeFirstVisible || relation == TreeLastVisible)
            {
                size_t first = 0;
                if (window->treeTopItem)
                {
                    const auto found = std::find_if(visible.begin(), visible.end(), [window](const VisibleTreeNode& item)
                    { return item.token == window->treeTopItem; });
                    if (found != visible.end()) first = static_cast<size_t>(found - visible.begin());
                }
                if (!visible.empty())
                {
                    const size_t rows = static_cast<size_t>((std::max)(1,
                        window->surface.Height() / (std::max)(1, window->treeItemHeight)));
                    const size_t last = (std::min)(visible.size() - 1, first + rows - 1);
                    result = relation == TreeFirstVisible ? visible[first].token : visible[last].token;
                }
            }
            else if (relation == TreeNextRoot)
            {
                const auto found = std::find_if(window->treeNodes.begin(), window->treeNodes.end(),
                    [](const TreeNode& node) { return node.parent == 0; });
                if (found != window->treeNodes.end()) result = found->token;
            }
            else if (relation == TreeParent)
            {
                const TreeNode* node = FindTreeNode(window->treeNodes, token);
                result = node ? node->parent : 0;
            }
            else if (relation == TreeChild)
            {
                const auto found = std::find_if(window->treeNodes.begin(), window->treeNodes.end(),
                    [token](const TreeNode& node) { return node.parent == token; });
                if (found != window->treeNodes.end()) result = found->token;
            }
            else if (relation == TreeNextVisible || relation == TreePreviousVisible)
            {
                const auto found = std::find_if(visible.begin(), visible.end(),
                    [token](const VisibleTreeNode& item) { return item.token == token; });
                if (found != visible.end())
                {
                    if (relation == TreeNextVisible && found + 1 != visible.end()) result = (found + 1)->token;
                    if (relation == TreePreviousVisible && found != visible.begin()) result = (found - 1)->token;
                }
            }
            else if (relation == TreeNextSibling || relation == TreePreviousSibling)
            {
                const TreeNode* node = FindTreeNode(window->treeNodes, token);
                if (node)
                {
                    std::vector<ULONG_PTR> siblings;
                    for (const auto& candidate : window->treeNodes)
                        if (candidate.parent == node->parent) siblings.push_back(candidate.token);
                    const auto found = std::find(siblings.begin(), siblings.end(), token);
                    if (found != siblings.end())
                    {
                        if (relation == TreeNextSibling && found + 1 != siblings.end()) result = *(found + 1);
                        if (relation == TreePreviousSibling && found != siblings.begin()) result = *(found - 1);
                    }
                }
            }
            return reinterpret_cast<LRESULT>(reinterpret_cast<HANDLE>(result));
        }
        if (message == TreeSelectItem)
        {
            const ULONG_PTR requested = treeToken(reinterpret_cast<HANDLE>(lParam));
            {
                std::lock_guard<std::mutex> guard(window->lock);
                if (requested && !FindTreeNode(window->treeNodes, requested)) return FALSE;
                if (wParam == TreeCaret)
                {
                    for (auto& node : window->treeNodes) node.state &= ~TreeStateSelected;
                    TreeNode* node = FindTreeNode(window->treeNodes, requested);
                    if (node) node->state |= TreeStateSelected;
                    window->treeSelectedItem = requested;
                }
                else if (wParam == TreeFirstVisible)
                {
                    window->treeTopItem = requested;
                }
            }
            InvalidateGuestRect(window->handle, nullptr, TRUE, nullptr);
            return TRUE;
        }
        if (message == TreeGetVisibleCount)
        {
            std::lock_guard<std::mutex> guard(window->lock);
            return window->treeItemHeight > 0
                ? (std::max)(1, window->surface.Height() / window->treeItemHeight) : 1;
        }
        if (message == TreeGetItemRect)
        {
            HANDLE requested = nullptr;
            if (!TryReadGuestValue(reinterpret_cast<const HANDLE*>(lParam), &requested)) return FALSE;
            RECT result = {};
            {
                std::lock_guard<std::mutex> guard(window->lock);
                const auto visible = VisibleTreeNodes(window->treeNodes);
                const ULONG_PTR token = treeToken(requested);
                const auto found = std::find_if(visible.begin(), visible.end(),
                    [token](const VisibleTreeNode& item) { return item.token == token; });
                if (found == visible.end()) return FALSE;
                size_t top = 0;
                if (window->treeTopItem)
                {
                    const auto topFound = std::find_if(visible.begin(), visible.end(), [window](const VisibleTreeNode& item)
                    { return item.token == window->treeTopItem; });
                    if (topFound != visible.end()) top = static_cast<size_t>(topFound - visible.begin());
                }
                const size_t itemIndex = static_cast<size_t>(found - visible.begin());
                const size_t visibleRows = static_cast<size_t>((std::max)(1,
                    window->surface.Height() / (std::max)(1, window->treeItemHeight)));
                if (itemIndex < top || itemIndex >= top + visibleRows) return FALSE;
                const int row = static_cast<int>(itemIndex - top);
                result.left = wParam ? found->depth * window->treeIndent + window->treeIndent + 2 : 0;
                result.top = row * window->treeItemHeight;
                result.right = window->surface.Width();
                result.bottom = result.top + window->treeItemHeight;
            }
            return TryWriteGuestValue(reinterpret_cast<RECT*>(lParam), result) ? TRUE : FALSE;
        }
        if (message == TreeHitTest)
        {
            auto destination = reinterpret_cast<GuestTreeHitTestInfo*>(lParam);
            GuestTreeHitTestInfo hit = {};
            if (!TryReadGuestValue(destination, &hit)) return 0;
            hit.flags = 1;
            hit.item = nullptr;
            {
                std::lock_guard<std::mutex> guard(window->lock);
                const auto visible = VisibleTreeNodes(window->treeNodes);
                size_t top = 0;
                if (window->treeTopItem)
                {
                    const auto topFound = std::find_if(visible.begin(), visible.end(), [window](const VisibleTreeNode& item)
                    { return item.token == window->treeTopItem; });
                    if (topFound != visible.end()) top = static_cast<size_t>(topFound - visible.begin());
                }
                const int row = hit.point.y >= 0 && window->treeItemHeight > 0
                    ? hit.point.y / window->treeItemHeight : -1;
                const size_t index = row < 0 ? visible.size() : top + static_cast<size_t>(row);
                if (index < visible.size())
                {
                    hit.item = reinterpret_cast<HANDLE>(visible[index].token);
                    hit.flags = 0x0046;
                }
            }
            TryWriteGuestValue(destination, hit);
            return reinterpret_cast<LRESULT>(hit.item);
        }
        if (message == TreeEnsureVisible)
        {
            const ULONG_PTR requested = treeToken(reinterpret_cast<HANDLE>(lParam));
            {
                std::lock_guard<std::mutex> guard(window->lock);
                TreeNode* node = FindTreeNode(window->treeNodes, requested);
                if (!node) return FALSE;
                ULONG_PTR parent = node->parent;
                while (parent)
                {
                    TreeNode* ancestor = FindTreeNode(window->treeNodes, parent);
                    if (!ancestor) break;
                    ancestor->state |= TreeStateExpanded;
                    parent = ancestor->parent;
                }
                window->treeTopItem = requested;
            }
            InvalidateGuestRect(window->handle, nullptr, TRUE, nullptr);
            return TRUE;
        }
    }

    if (controlKind == BuiltinControlKind::Progress)
    {
        if (message == ProgressSetRange || message == ProgressSetRange32)
        {
            int low = message == ProgressSetRange
                ? static_cast<int>(LOWORD(lParam))
                : static_cast<int>(wParam);
            int high = message == ProgressSetRange
                ? static_cast<int>(HIWORD(lParam))
                : static_cast<int>(lParam);
            LRESULT previous = 0;
            {
                std::lock_guard<std::mutex> guard(window->lock);
                previous = message == ProgressSetRange
                    ? MAKELONG(LOWORD(window->progressMinimum), LOWORD(window->progressMaximum))
                    : 0;
                window->progressMinimum = low;
                window->progressMaximum = high;
                if (window->progressMinimum > window->progressMaximum)
                {
                    std::swap(window->progressMinimum, window->progressMaximum);
                }
                window->progressPosition = (std::max)(window->progressMinimum,
                    (std::min)(window->progressMaximum, window->progressPosition));
            }
            InvalidateGuestRect(window->handle, nullptr, TRUE, nullptr);
            return previous;
        }
        if (message == ProgressSetPosition || message == ProgressDeltaPosition ||
            message == ProgressStep)
        {
            int previous = 0;
            {
                std::lock_guard<std::mutex> guard(window->lock);
                previous = window->progressPosition;
                if (message == ProgressSetPosition)
                {
                    window->progressPosition = static_cast<int>(wParam);
                }
                else
                {
                    const int delta = message == ProgressDeltaPosition
                        ? static_cast<int>(wParam)
                        : window->progressStep;
                    window->progressPosition = SaturatingAdd(window->progressPosition, delta);
                }
                if (message == ProgressStep && window->progressMaximum > window->progressMinimum &&
                    window->progressPosition > window->progressMaximum)
                {
                    const int span = window->progressMaximum - window->progressMinimum;
                    window->progressPosition = window->progressMinimum +
                        (window->progressPosition - window->progressMinimum) % span;
                }
                window->progressPosition = (std::max)(window->progressMinimum,
                    (std::min)(window->progressMaximum, window->progressPosition));
            }
            InvalidateGuestRect(window->handle, nullptr, TRUE, nullptr);
            return previous;
        }
        if (message == ProgressSetStep)
        {
            std::lock_guard<std::mutex> guard(window->lock);
            const int previous = window->progressStep;
            window->progressStep = static_cast<int>(wParam);
            return previous;
        }
        if (message == ProgressGetStep)
        {
            std::lock_guard<std::mutex> guard(window->lock);
            return window->progressStep;
        }
        if (message == ProgressGetPosition)
        {
            std::lock_guard<std::mutex> guard(window->lock);
            return window->progressPosition;
        }
        if (message == ProgressGetRange)
        {
            GuestProgressRange range = {};
            {
                std::lock_guard<std::mutex> guard(window->lock);
                range.low = window->progressMinimum;
                range.high = window->progressMaximum;
            }
            if (lParam && !TryWriteGuestValue(reinterpret_cast<GuestProgressRange*>(lParam), range))
            {
                return 0;
            }
            return wParam ? range.low : range.high;
        }
        if (message == ProgressSetBarColor || message == ProgressSetBackgroundColor)
        {
            COLORREF previous = 0;
            {
                std::lock_guard<std::mutex> guard(window->lock);
                COLORREF& target = message == ProgressSetBarColor
                    ? window->progressBarColor
                    : window->progressBackgroundColor;
                previous = target;
                target = static_cast<COLORREF>(lParam);
            }
            InvalidateGuestRect(window->handle, nullptr, TRUE, nullptr);
            return previous;
        }
        if (message == ProgressGetBarColor || message == ProgressGetBackgroundColor)
        {
            std::lock_guard<std::mutex> guard(window->lock);
            return message == ProgressGetBarColor
                ? window->progressBarColor
                : window->progressBackgroundColor;
        }
        if (message == ProgressSetState)
        {
            const UINT requested = static_cast<UINT>(wParam);
            if (requested < 1 || requested > 3)
            {
                return 0;
            }
            UINT previous = 0;
            {
                std::lock_guard<std::mutex> guard(window->lock);
                previous = window->progressState;
                window->progressState = requested;
            }
            InvalidateGuestRect(window->handle, nullptr, TRUE, nullptr);
            return previous;
        }
        if (message == ProgressGetState)
        {
            std::lock_guard<std::mutex> guard(window->lock);
            return window->progressState;
        }
        if (message == ProgressSetMarquee)
        {
            {
                std::lock_guard<std::mutex> guard(window->lock);
                window->progressMarquee = wParam != FALSE;
            }
            InvalidateGuestRect(window->handle, nullptr, TRUE, nullptr);
            return TRUE;
        }
    }

    if (controlKind == BuiltinControlKind::Tab)
    {
        if (message == TabSetImageList || message == TabGetImageList)
        {
            std::lock_guard<std::mutex> guard(window->lock);
            if (message == TabGetImageList)
            {
                return reinterpret_cast<LRESULT>(window->tabImageList);
            }
            const HANDLE previous = window->tabImageList;
            window->tabImageList = reinterpret_cast<HANDLE>(lParam);
            return reinterpret_cast<LRESULT>(previous);
        }
        if (message == TabGetItemCount)
        {
            std::lock_guard<std::mutex> guard(window->lock);
            return static_cast<LRESULT>(window->tabItems.size());
        }
        if (message == TabInsertItemW || message == TabSetItemW || message == TabGetItemW)
        {
            auto guestItem = reinterpret_cast<GuestTabItemW*>(lParam);
            GuestTabItemW item = {};
            if (!TryReadGuestValue(guestItem, &item))
            {
                return message == TabInsertItemW ? -1 : FALSE;
            }
            const int requested = static_cast<int>(wParam);
            if (message == TabGetItemW)
            {
                std::wstring text;
                {
                    std::lock_guard<std::mutex> guard(window->lock);
                    if (requested < 0 || static_cast<size_t>(requested) >= window->tabItems.size())
                    {
                        return FALSE;
                    }
                    const size_t index = static_cast<size_t>(requested);
                    text = window->tabItems[index];
                    if ((item.mask & TabItemImage) != 0) item.image = window->tabItemImages[index];
                    if ((item.mask & TabItemParam) != 0) item.itemData = window->tabItemData[index];
                    if ((item.mask & TabItemState) != 0)
                        item.state = window->tabItemStates[index] & item.stateMask;
                }
                if ((item.mask & TabItemText) != 0 &&
                    !TryWriteGuestWideString(item.text,
                        static_cast<size_t>((std::max)(0, item.textCapacity)), text, nullptr))
                {
                    return FALSE;
                }
                return TryWriteGuestValue(guestItem, item) ? TRUE : FALSE;
            }

            std::wstring text;
            if ((item.mask & TabItemText) != 0 && !TryReadGuestWideString(item.text, &text))
            {
                return message == TabInsertItemW ? -1 : FALSE;
            }
            int result = FALSE;
            {
                std::lock_guard<std::mutex> guard(window->lock);
                if (message == TabInsertItemW)
                {
                    const size_t position = requested < 0
                        ? window->tabItems.size()
                        : (std::min)(static_cast<size_t>(requested), window->tabItems.size());
                    window->tabItems.insert(window->tabItems.begin() + position, std::move(text));
                    window->tabItemData.insert(window->tabItemData.begin() + position,
                        (item.mask & TabItemParam) != 0 ? item.itemData : 0);
                    window->tabItemImages.insert(window->tabItemImages.begin() + position,
                        (item.mask & TabItemImage) != 0 ? item.image : -1);
                    window->tabItemStates.insert(window->tabItemStates.begin() + position,
                        (item.mask & TabItemState) != 0 ? item.state & item.stateMask : 0);
                    if (window->tabSelectedItem < 0)
                    {
                        window->tabSelectedItem = 0;
                        window->tabFocusedItem = 0;
                    }
                    else
                    {
                        if (window->tabSelectedItem >= static_cast<int>(position)) ++window->tabSelectedItem;
                        if (window->tabFocusedItem >= static_cast<int>(position)) ++window->tabFocusedItem;
                    }
                    result = static_cast<int>(position);
                }
                else
                {
                    if (requested < 0 || static_cast<size_t>(requested) >= window->tabItems.size())
                    {
                        return FALSE;
                    }
                    const size_t index = static_cast<size_t>(requested);
                    if ((item.mask & TabItemText) != 0) window->tabItems[index] = std::move(text);
                    if ((item.mask & TabItemParam) != 0) window->tabItemData[index] = item.itemData;
                    if ((item.mask & TabItemImage) != 0) window->tabItemImages[index] = item.image;
                    if ((item.mask & TabItemState) != 0)
                        window->tabItemStates[index] =
                            (window->tabItemStates[index] & ~item.stateMask) | (item.state & item.stateMask);
                    result = TRUE;
                }
            }
            InvalidateGuestRect(window->handle, nullptr, TRUE, nullptr);
            return result;
        }
        if (message == TabDeleteItem || message == TabDeleteAllItems)
        {
            {
                std::lock_guard<std::mutex> guard(window->lock);
                if (message == TabDeleteAllItems)
                {
                    window->tabItems.clear();
                    window->tabItemData.clear();
                    window->tabItemImages.clear();
                    window->tabItemStates.clear();
                    window->tabSelectedItem = -1;
                    window->tabFocusedItem = -1;
                }
                else
                {
                    const size_t index = static_cast<size_t>(wParam);
                    if (index >= window->tabItems.size()) return FALSE;
                    window->tabItems.erase(window->tabItems.begin() + index);
                    window->tabItemData.erase(window->tabItemData.begin() + index);
                    window->tabItemImages.erase(window->tabItemImages.begin() + index);
                    window->tabItemStates.erase(window->tabItemStates.begin() + index);
                    if (window->tabItems.empty())
                    {
                        window->tabSelectedItem = -1;
                        window->tabFocusedItem = -1;
                    }
                    else
                    {
                        if (window->tabSelectedItem >= static_cast<int>(window->tabItems.size()))
                            window->tabSelectedItem = static_cast<int>(window->tabItems.size()) - 1;
                        if (window->tabFocusedItem >= static_cast<int>(window->tabItems.size()))
                            window->tabFocusedItem = window->tabSelectedItem;
                    }
                }
            }
            InvalidateGuestRect(window->handle, nullptr, TRUE, nullptr);
            return TRUE;
        }
        if (message == TabGetCurrentSelection || message == TabGetCurrentFocus)
        {
            std::lock_guard<std::mutex> guard(window->lock);
            return message == TabGetCurrentSelection
                ? window->tabSelectedItem
                : window->tabFocusedItem;
        }
        if (message == TabSetCurrentSelection || message == TabSetCurrentFocus)
        {
            const int requested = static_cast<int>(wParam);
            int previous = -1;
            {
                std::lock_guard<std::mutex> guard(window->lock);
                if (requested < 0 || static_cast<size_t>(requested) >= window->tabItems.size())
                {
                    return -1;
                }
                if (message == TabSetCurrentSelection)
                {
                    previous = window->tabSelectedItem;
                    window->tabSelectedItem = requested;
                }
                else
                {
                    previous = window->tabFocusedItem;
                    window->tabFocusedItem = requested;
                }
            }
            InvalidateGuestRect(window->handle, nullptr, TRUE, nullptr);
            return previous;
        }
        if (message == TabSetItemSize)
        {
            DWORD previous = 0;
            {
                std::lock_guard<std::mutex> guard(window->lock);
                previous = MAKELONG(window->tabItemWidth, window->tabItemHeight);
                window->tabItemWidth = (std::max)(1, static_cast<int>(LOWORD(lParam)));
                window->tabItemHeight = (std::max)(1, static_cast<int>(HIWORD(lParam)));
            }
            InvalidateGuestRect(window->handle, nullptr, TRUE, nullptr);
            return previous;
        }
        if (message == TabSetPadding)
        {
            std::lock_guard<std::mutex> guard(window->lock);
            window->tabHorizontalPadding = static_cast<int>(LOWORD(lParam));
            window->tabVerticalPadding = static_cast<int>(HIWORD(lParam));
            return 0;
        }
        if (message == TabGetRowCount)
        {
            return 1;
        }
        if (message == TabGetItemRect || message == TabHitTest)
        {
            std::vector<std::wstring> items;
            int fixedWidth = 0;
            int itemHeight = GuestMetrics::TabItemHeight;
            {
                std::lock_guard<std::mutex> guard(window->lock);
                items = window->tabItems;
                fixedWidth = window->tabItemWidth;
                itemHeight = window->tabItemHeight;
            }
            const auto widthFor = [fixedWidth](const std::wstring& text)
            {
                return fixedWidth > 0 ? fixedWidth : (std::max)(32,
                    static_cast<int>((std::min)(text.size(), static_cast<size_t>(128))) *
                        MiniGdi::DefaultTextGlyphWidth + 16);
            };
            if (message == TabGetItemRect)
            {
                const int requested = static_cast<int>(wParam);
                if (requested < 0 || static_cast<size_t>(requested) >= items.size()) return FALSE;
                RECT result{ 1, 1, 1, 1 + itemHeight };
                for (int index = 0; index < requested; ++index)
                    result.left += widthFor(items[static_cast<size_t>(index)]);
                result.right = result.left + widthFor(items[static_cast<size_t>(requested)]);
                return TryWriteGuestValue(reinterpret_cast<RECT*>(lParam), result) ? TRUE : FALSE;
            }
            auto destination = reinterpret_cast<GuestTabHitTestInfo*>(lParam);
            GuestTabHitTestInfo hit = {};
            if (!TryReadGuestValue(destination, &hit)) return -1;
            int left = 1;
            int found = -1;
            for (size_t index = 0; index < items.size(); ++index)
            {
                const int right = left + widthFor(items[index]);
                if (hit.point.x >= left && hit.point.x < right &&
                    hit.point.y >= 1 && hit.point.y < 1 + itemHeight)
                {
                    found = static_cast<int>(index);
                    hit.flags = 0;
                    break;
                }
                left = right;
            }
            if (found < 0) hit.flags = 1;
            TryWriteGuestValue(destination, hit);
            return found;
        }
        if (message == TabAdjustRect)
        {
            RECT result = {};
            if (!TryReadGuestValue(reinterpret_cast<const RECT*>(lParam), &result)) return FALSE;
            int itemHeight = GuestMetrics::TabItemHeight;
            {
                std::lock_guard<std::mutex> guard(window->lock);
                itemHeight = window->tabItemHeight;
            }
            if (wParam)
            {
                result.left -= 2; result.right += 2; result.top -= itemHeight + 2; result.bottom += 2;
            }
            else
            {
                result.left += 2; result.right -= 2; result.top += itemHeight + 2; result.bottom -= 2;
            }
            return TryWriteGuestValue(reinterpret_cast<RECT*>(lParam), result) ? TRUE : FALSE;
        }
    }

    if (controlKind == BuiltinControlKind::UpDown)
    {
        if (message == UpDownSetRange || message == UpDownSetRange32)
        {
            {
                std::lock_guard<std::mutex> guard(window->lock);
                if (message == UpDownSetRange)
                {
                    window->upDownMaximum = static_cast<short>(LOWORD(lParam));
                    window->upDownMinimum = static_cast<short>(HIWORD(lParam));
                }
                else
                {
                    window->upDownMinimum = static_cast<int>(wParam);
                    window->upDownMaximum = static_cast<int>(lParam);
                }
                const int low = (std::min)(window->upDownMinimum, window->upDownMaximum);
                const int high = (std::max)(window->upDownMinimum, window->upDownMaximum);
                window->upDownPosition = (std::max)(low, (std::min)(high, window->upDownPosition));
            }
            InvalidateGuestRect(window->handle, nullptr, TRUE, nullptr);
            return 0;
        }
        if (message == UpDownGetRange)
        {
            std::lock_guard<std::mutex> guard(window->lock);
            return MAKELONG(static_cast<short>(window->upDownMaximum),
                static_cast<short>(window->upDownMinimum));
        }
        if (message == UpDownGetRange32)
        {
            int minimum = 0;
            int maximum = 0;
            {
                std::lock_guard<std::mutex> guard(window->lock);
                minimum = window->upDownMinimum;
                maximum = window->upDownMaximum;
            }
            if (wParam && !TryWriteGuestValue(reinterpret_cast<int*>(wParam), minimum)) return 0;
            if (lParam && !TryWriteGuestValue(reinterpret_cast<int*>(lParam), maximum)) return 0;
            return 0;
        }
        if (message == UpDownSetPosition || message == UpDownSetPosition32)
        {
            int previous = 0;
            HWND buddy = nullptr;
            int current = 0;
            UINT numberBase = 10;
            {
                std::lock_guard<std::mutex> guard(window->lock);
                previous = window->upDownPosition;
                const int requested = message == UpDownSetPosition
                    ? static_cast<short>(LOWORD(lParam)) : static_cast<int>(lParam);
                const int low = (std::min)(window->upDownMinimum, window->upDownMaximum);
                const int high = (std::max)(window->upDownMinimum, window->upDownMaximum);
                window->upDownPosition = (std::max)(low, (std::min)(high, requested));
                current = window->upDownPosition;
                buddy = window->upDownBuddy;
                numberBase = window->upDownBase;
            }
            if (buddy)
            {
                wchar_t buffer[40] = {};
                if (numberBase == 16) swprintf_s(buffer, L"%X", static_cast<unsigned int>(current));
                else swprintf_s(buffer, L"%d", current);
                SendGuestMessage(buddy, GuestAbi::WmSetText, 0,
                    reinterpret_cast<LPARAM>(buffer), nullptr);
            }
            InvalidateGuestRect(window->handle, nullptr, TRUE, nullptr);
            return message == UpDownSetPosition ? MAKELONG(static_cast<short>(previous), 0) : previous;
        }
        if (message == UpDownGetPosition || message == UpDownGetPosition32)
        {
            std::lock_guard<std::mutex> guard(window->lock);
            if (message == UpDownGetPosition32 && lParam)
            {
                const BOOL success = TRUE;
                TryWriteGuestValue(reinterpret_cast<BOOL*>(lParam), success);
            }
            return message == UpDownGetPosition
                ? MAKELONG(static_cast<short>(window->upDownPosition), 0)
                : window->upDownPosition;
        }
        if (message == UpDownSetBuddy || message == UpDownGetBuddy)
        {
            std::lock_guard<std::mutex> guard(window->lock);
            if (message == UpDownGetBuddy) return reinterpret_cast<LRESULT>(window->upDownBuddy);
            const HWND previous = window->upDownBuddy;
            window->upDownBuddy = reinterpret_cast<HWND>(wParam);
            return reinterpret_cast<LRESULT>(previous);
        }
        if (message == UpDownSetBase || message == UpDownGetBase)
        {
            std::lock_guard<std::mutex> guard(window->lock);
            if (message == UpDownGetBase) return window->upDownBase;
            if (wParam != 10 && wParam != 16) return 0;
            const UINT previous = window->upDownBase;
            window->upDownBase = static_cast<UINT>(wParam);
            return previous;
        }
    }

    if ((controlKind == BuiltinControlKind::Toolbar ||
        controlKind == BuiltinControlKind::Rebar ||
        controlKind == BuiltinControlKind::ListView ||
        controlKind == BuiltinControlKind::Header ||
        controlKind == BuiltinControlKind::Tab ||
        controlKind == BuiltinControlKind::Progress ||
        controlKind == BuiltinControlKind::TreeView ||
        controlKind == BuiltinControlKind::UpDown) && message >= 0x0400)
    {
        RuntimeDiagnostics::Record(
            L"COMMON CONTROL MESSAGE: handle " +
            std::to_wstring(reinterpret_cast<ULONG_PTR>(window->handle)) + L", code " +
            std::to_wstring(message) + L".");
    }

    switch (message)
    {
    case GuestAbi::WmNcCreate:
        return TRUE;
    case GuestAbi::WmNcHitTest:
        return GuestAbi::HtClient;
    case GuestAbi::WmWindowPosChanged:
    {
        GuestWindowPosition position{};
        if (!TryReadGuestValue(
            reinterpret_cast<const GuestWindowPosition*>(lParam), &position))
        {
            return 0;
        }
        if ((position.flags & GuestSwpNoMove) == 0)
        {
            CallWindowProcedure(window, GuestAbi::WmMove, 0,
                GuestAbi::MakeMouseLParam(
                    SignedCoordinateWord(position.x),
                    SignedCoordinateWord(position.y)));
        }
        if ((position.flags & GuestSwpNoSize) == 0)
        {
            int clientWidth = position.cx;
            int clientHeight = position.cy;
            {
                std::lock_guard<std::mutex> guard(window->lock);
                const GuestMetrics::NonClientMetrics nonClient =
                    GuestMetrics::NonClientForEmbeddedWindow(
                        window->style, window->extendedStyle,
                        window->parent != nullptr);
                clientWidth = (std::max)(0, clientWidth -
                    nonClient.left - nonClient.right);
                clientHeight = (std::max)(0, clientHeight -
                    nonClient.top - nonClient.bottom -
                    ((!window->parent && window->menuBar) ?
                        GuestMetrics::MenuHeight : 0));
            }
            CallWindowProcedure(window, GuestAbi::WmSize, GuestAbi::SizeRestored,
                GuestAbi::MakeMouseLParam(
                    static_cast<WORD>((std::min)(clientWidth, 0xffff)),
                    static_cast<WORD>((std::min)(clientHeight, 0xffff))));
        }
        return 0;
    }
    case GuestAbi::WmEraseBkgnd:
        // BeginPaint owns the class-brush fallback. Returning zero gives it
        // the same negotiation as DefWindowProc without double-erasing.
        return 0;
    case GuestAbi::WmSysCommand:
        if ((wParam & GuestAbi::ScMask) == GuestAbi::ScClose)
            return CallWindowProcedure(window, GuestAbi::WmClose, 0, 0);
        return 0;
    case GuestAbi::WmClose:
    {
        HWND handle = nullptr;
        {
            std::lock_guard<std::mutex> guard(window->lock);
            handle = window->handle;
        }
        DestroyGuestWindow(handle, nullptr);
        return 0;
    }
    case GuestAbi::WmGetTextLength:
    {
        if (controlKind == BuiltinControlKind::Edit) syncEditTitleFromHandle();
        std::lock_guard<std::mutex> guard(window->lock);
        return static_cast<LRESULT>((std::min)(
            window->title.size(),
            static_cast<size_t>((std::numeric_limits<LRESULT>::max)())));
    }
    case GuestAbi::WmGetText:
    {
        if (controlKind == BuiltinControlKind::Edit) syncEditTitleFromHandle();
        LPWSTR destination = reinterpret_cast<LPWSTR>(lParam);
        const size_t capacity = static_cast<size_t>((std::min)(
            wParam,
            static_cast<WPARAM>(MaximumBuiltinControlTextLength + 1)));
        if (!destination || capacity == 0)
        {
            return 0;
        }

        std::wstring text;
        {
            std::lock_guard<std::mutex> guard(window->lock);
            text = window->title;
        }
        const size_t copied = (std::min)(text.size(), capacity - 1);
        if (copied != 0)
        {
            memcpy(destination, text.data(), copied * sizeof(wchar_t));
        }
        destination[copied] = L'\0';
        return static_cast<LRESULT>(copied);
    }
    case GuestAbi::WmSetText:
    {
        const LPCWSTR source = reinterpret_cast<LPCWSTR>(lParam);
        size_t length = 0;
        if (source)
        {
            while (length < MaximumBuiltinControlTextLength && source[length] != L'\0')
            {
                ++length;
            }
        }
        std::wstring text = source ? std::wstring(source, length) : std::wstring();
        {
            std::lock_guard<std::mutex> guard(window->lock);
            if (window->destroyed)
            {
                return FALSE;
            }
            if ((controlKind == BuiltinControlKind::Edit ||
                controlKind == BuiltinControlKind::ComboBox) &&
                text.size() > window->editTextLimit)
            {
                text.resize(window->editTextLimit);
            }
            window->title = std::move(text);
            window->editCaret = controlKind == BuiltinControlKind::Edit &&
                (window->style & GuestAbi::EsMultiline) != 0
                    ? 0 : window->title.size();
            window->editSelectionAnchor = window->editCaret;
            window->editSelectionEnd = window->editCaret;
            window->editFirstVisibleCharacter = 0;
            window->editFirstVisibleLine = 0;
            window->editHorizontalOffset = 0;
            if (controlKind == BuiltinControlKind::StatusBar && !window->statusBarTexts.empty())
            {
                window->statusBarTexts[0] = window->title;
            }
        }
        if (controlKind == BuiltinControlKind::Edit) syncEditHandleFromTitle();
        invalidate();
        return TRUE;
    }
    case GuestAbi::WmSetFont:
    {
        MiniGdi::ObjectHandle requested = MiniGdi::InvalidObject;
        if (wParam != 0)
        {
            MiniGdi::ObjectKind objectKind;
            if (!ToMiniGdiObject(reinterpret_cast<HGDIOBJ>(wParam), &requested) ||
                !m_gdi.ObjectType(requested, &objectKind) ||
                objectKind != MiniGdi::ObjectKind::Font)
            {
                return 0;
            }
        }
        MiniGdi::FontMetrics fontMetrics{};
        if (!m_gdi.GetFontMetrics(requested, &fontMetrics))
        {
            fontMetrics.height = GuestMetrics::TextHeight;
            fontMetrics.averageWidth = GuestMetrics::TextWidth;
        }
        {
            std::lock_guard<std::mutex> guard(window->lock);
            if (window->destroyed)
            {
                return 0;
            }
            window->controlFont = requested;
            window->controlTextWidth = (std::max)(1, fontMetrics.averageWidth);
            window->controlTextHeight = (std::max)(1, fontMetrics.height);
            window->listViewHeaderHeight = GuestMetrics::ControlHeightForText(
                window->controlTextHeight);
            window->listViewRowHeight = (std::max)(GuestMetrics::TextHeight,
                window->controlTextHeight);
        }
        if (lParam != 0)
        {
            invalidate();
        }
        return 0;
    }
    case GuestAbi::WmGetFont:
    {
        std::lock_guard<std::mutex> guard(window->lock);
        return static_cast<LRESULT>(window->controlFont);
    }
    case GuestAbi::WmPaint:
    {
        if (controlKind == BuiltinControlKind::Edit) syncEditTitleFromHandle();
        std::wstring text;
        DWORD style = 0;
        bool enabled = false;
        bool visualStyles = false;
        bool pressed = false;
        size_t caret = 0;
        size_t selectionAnchor = 0;
        size_t selectionEnd = 0;
        wchar_t passwordCharacter = L'\0';
        MiniGdi::ObjectHandle font = MiniGdi::InvalidObject;
        MiniGdi::Surface staticImage;
        std::vector<std::wstring> choiceItems;
        int selectedChoice = -1;
        bool comboDropped = false;
        std::vector<std::wstring> listColumns;
        std::vector<int> listColumnWidths;
        std::vector<int> listColumnOrders;
        std::vector<std::vector<std::wstring>> listItems;
        std::vector<LPARAM> listViewItemData;
        std::vector<UINT> listViewItemStates;
        std::vector<int> listViewItemImages;
        HANDLE listViewImageList = nullptr;
        COLORREF listViewBackgroundColor = 0x00ffffff;
        COLORREF listViewTextColor = 0x00000000;
        COLORREF listViewTextBackgroundColor = 0x00ffffff;
        UINT listViewExtendedStyle = 0;
        int listViewSelectedItem = -1;
        int listViewTopItem = 0;
        int listViewEditItem = -1;
        std::wstring listViewEditText;
        size_t listViewEditCaret = 0;
        int controlTextWidth = GuestMetrics::TextWidth;
        int controlTextHeight = GuestMetrics::TextHeight;
        size_t editFirstVisibleLine = 0;
        int editHorizontalOffset = 0;
        int listViewHeaderHeight = GuestMetrics::ListViewHeaderHeight;
        int listViewRowHeight = GuestMetrics::TextHeight;
        std::vector<int> toolbarCommands;
        std::vector<int> toolbarBitmaps;
        std::vector<BYTE> toolbarButtonStates;
        std::vector<BYTE> toolbarButtonStyles;
        std::vector<std::wstring> toolbarButtonTexts;
        HANDLE toolbarImageList = nullptr;
        int toolbarButtonWidth = GuestMetrics::ToolbarButtonExtent;
        int toolbarButtonHeight = GuestMetrics::ToolbarButtonExtent;
        int toolbarBitmapWidth = GuestMetrics::DefaultBitmapExtent;
        int toolbarBitmapHeight = GuestMetrics::DefaultBitmapExtent;
        int toolbarPressedIndex = -1;
        std::vector<int> statusBarParts;
        std::vector<std::wstring> statusBarTexts;
        std::wstring statusBarSimpleText;
        bool statusBarSimple = false;
        int progressMinimum = 0;
        int progressMaximum = 100;
        int progressPosition = 0;
        UINT progressState = 1;
        COLORREF progressBarColor = 0xffffffffu;
        COLORREF progressBackgroundColor = 0xffffffffu;
        bool progressMarquee = false;
        std::vector<std::wstring> tabItems;
        std::vector<int> tabItemImages;
        HANDLE tabImageList = nullptr;
        int tabSelectedItem = -1;
        int tabItemWidth = 0;
        int tabItemHeight = GuestMetrics::TabItemHeight;
        std::vector<std::wstring> headerItems;
        std::vector<int> headerItemWidths;
        std::vector<int> headerItemOrders;
        std::vector<int> headerItemImages;
        HANDLE headerImageList = nullptr;
        int headerPressedItem = -1;
        std::vector<TreeNode> treeNodes;
        ULONG_PTR treeSelectedItem = 0;
        ULONG_PTR treeTopItem = 0;
        HANDLE treeImageList = nullptr;
        int treeIndent = 16;
        int treeItemHeight = 18;
        COLORREF treeBackgroundColor = 0x00ffffff;
        COLORREF treeTextColor = 0x00000000;
        HWND handle = nullptr;
        HWND parent = nullptr;
        UINT_PTR controlId = 0;
        {
            std::lock_guard<std::mutex> guard(window->lock);
            if (window->destroyed)
            {
                return 0;
            }
            text = window->title;
            style = window->style;
            enabled = window->enabled;
            visualStyles = window->visualStyles;
            pressed = window->buttonPressed || window->buttonKeyboardPressed;
            caret = window->editCaret;
            selectionAnchor = window->editSelectionAnchor;
            selectionEnd = window->editSelectionEnd;
            passwordCharacter = window->editPasswordCharacter;
            font = window->controlFont;
            staticImage = window->staticImage;
            choiceItems = window->choiceItems;
            selectedChoice = window->selectedChoice;
            comboDropped = window->comboDropped ||
                ((window->style & 0x0003u) == ComboBoxStyleSimple);
            listColumns = window->listViewColumns;
            listColumnWidths = window->listViewColumnWidths;
            listColumnOrders = window->listViewColumnOrders;
            listItems = window->listViewItems;
            listViewItemData = window->listViewItemData;
            listViewItemStates = window->listViewItemStates;
            listViewItemImages = window->listViewItemImages;
            listViewImageList = window->listViewImageLists[1]
                ? window->listViewImageLists[1]
                : window->listViewImageLists[0];
            listViewBackgroundColor = window->listViewBackgroundColor;
            listViewTextColor = window->listViewTextColor;
            listViewTextBackgroundColor = window->listViewTextBackgroundColor;
            listViewExtendedStyle = window->listViewExtendedStyle;
            listViewSelectedItem = window->listViewSelectedItem;
            listViewTopItem = window->listViewTopItem;
            listViewEditItem = window->listViewEditItem;
            listViewEditText = window->listViewEditText;
            listViewEditCaret = window->listViewEditCaret;
            controlTextWidth = window->controlTextWidth;
            controlTextHeight = window->controlTextHeight;
            editFirstVisibleLine = window->editFirstVisibleLine;
            editHorizontalOffset = window->editHorizontalOffset;
            listViewHeaderHeight = window->listViewHeaderHeight;
            listViewRowHeight = window->listViewRowHeight;
            toolbarCommands = window->toolbarCommands;
            toolbarBitmaps = window->toolbarBitmaps;
            toolbarButtonStates = window->toolbarButtonStates;
            toolbarButtonStyles = window->toolbarButtonStyles;
            toolbarButtonTexts = window->toolbarButtonTexts;
            toolbarImageList = window->toolbarImageList;
            toolbarButtonWidth = window->toolbarButtonWidth;
            toolbarButtonHeight = window->toolbarButtonHeight;
            toolbarBitmapWidth = window->toolbarBitmapWidth;
            toolbarBitmapHeight = window->toolbarBitmapHeight;
            toolbarPressedIndex = window->toolbarPressedIndex;
            statusBarParts = window->statusBarParts;
            statusBarTexts = window->statusBarTexts;
            statusBarSimpleText = window->statusBarSimpleText;
            statusBarSimple = window->statusBarSimple;
            progressMinimum = window->progressMinimum;
            progressMaximum = window->progressMaximum;
            progressPosition = window->progressPosition;
            progressState = window->progressState;
            progressBarColor = window->progressBarColor;
            progressBackgroundColor = window->progressBackgroundColor;
            progressMarquee = window->progressMarquee;
            tabItems = window->tabItems;
            tabItemImages = window->tabItemImages;
            tabImageList = window->tabImageList;
            tabSelectedItem = window->tabSelectedItem;
            tabItemWidth = window->tabItemWidth;
            tabItemHeight = window->tabItemHeight;
            headerItems = window->headerItems;
            headerItemWidths = window->headerItemWidths;
            headerItemOrders = window->headerItemOrders;
            headerItemImages = window->headerItemImages;
            headerImageList = window->headerImageList;
            headerPressedItem = window->headerPressedItem;
            treeNodes = window->treeNodes;
            treeSelectedItem = window->treeSelectedItem;
            treeTopItem = window->treeTopItem;
            treeImageList = window->treeImageLists[0];
            treeIndent = window->treeIndent;
            treeItemHeight = window->treeItemHeight;
            treeBackgroundColor = window->treeBackgroundColor;
            treeTextColor = window->treeTextColor;
            handle = window->handle;
            parent = window->parent;
            controlId = window->controlId;
        }

        if (controlKind == BuiltinControlKind::Edit && passwordCharacter != L'\0')
            text.assign(text.size(), passwordCharacter);

        std::vector<size_t> listDisplayColumns(listColumns.size());
        std::iota(listDisplayColumns.begin(), listDisplayColumns.end(), static_cast<size_t>(0));
        if (listColumnOrders.size() == listDisplayColumns.size())
        {
            std::stable_sort(listDisplayColumns.begin(), listDisplayColumns.end(),
                [&listColumnOrders](size_t left, size_t right)
            {
                return listColumnOrders[left] < listColumnOrders[right];
            });
        }

        GuestAbi::PaintStruct paint = {};
        const HDC dc = BeginGuestPaint(handle, &paint, nullptr);
        if (!dc)
        {
            return 0;
        }

        const MiniGdi::DcHandle guestDc = FromGuestDc(dc);
        MiniGdi::Surface* surface = m_gdi.GetSurface(guestDc);
        if (surface)
        {
            const int width = surface->Width();
            const int height = surface->Height();
            const MiniGdi::Rect fullRect{ 0, 0, width, height };
            const bool focused = m_focusWindow.load() == reinterpret_cast<ULONG_PTR>(handle);
            const MiniGdi::Color textColor = enabled
                ? (controlKind == BuiltinControlKind::ListView
                    ? ColorFromGuestColorRef(listViewTextColor)
                    : MiniGdi::OpaqueBlack)
                : MiniGdi::MakeColor(128, 128, 128);

            if (controlKind == BuiltinControlKind::Button)
            {
                const MiniGdi::Color fill = enabled
                    ? (pressed ? MiniGdi::MakeColor(214, 214, 214) :
                        (visualStyles ? MiniGdi::MakeColor(250, 250, 250) : MiniGdi::MakeColor(240, 240, 240)))
                    : MiniGdi::MakeColor(235, 235, 235);
                MiniGdi::DrawRectangle(
                    *surface,
                    fullRect,
                    fill,
                    focused && visualStyles ? MiniGdi::MakeColor(0, 120, 215) : MiniGdi::MakeColor(96, 96, 96));
            }
            else if (controlKind == BuiltinControlKind::Edit)
            {
                MiniGdi::DrawRectangle(
                    *surface,
                    fullRect,
                    MiniGdi::OpaqueWhite,
                    focused && visualStyles ? MiniGdi::MakeColor(0, 120, 215) : MiniGdi::MakeColor(128, 128, 128));
            }
            else if (controlKind == BuiltinControlKind::ListView)
            {
                // A list-view starts empty, but it must still establish a
                // visible client area and edge before its owner inserts the
                // first column/item. This also prevents an unpainted child
                // surface from being mistaken for a failed presentation.
                MiniGdi::DrawRectangle(
                    *surface,
                    fullRect,
                    ColorFromGuestColorRef(listViewBackgroundColor),
                    MiniGdi::MakeColor(128, 128, 128));
                if (!listColumns.empty())
                {
                    MiniGdi::FillRect(
                        *surface,
                        MiniGdi::Rect{ 1, 1, (std::max)(1, width - 1),
                            (std::min)(listViewHeaderHeight,
                                (std::max)(1, height - 1)) },
                        MiniGdi::MakeColor(240, 240, 240));
                }
            }
            else if (controlKind == BuiltinControlKind::Toolbar ||
                controlKind == BuiltinControlKind::Rebar ||
                controlKind == BuiltinControlKind::StatusBar ||
                controlKind == BuiltinControlKind::Tab)
            {
                MiniGdi::DrawRectangle(
                    *surface,
                    fullRect,
                    MiniGdi::MakeColor(240, 240, 240),
                    MiniGdi::MakeColor(160, 160, 160));
                if (height > 2)
                {
                    MiniGdi::DrawLine(
                        *surface,
                        MiniGdi::Point{ 1, height - 1 },
                        MiniGdi::Point{ (std::max)(1, width - 2), height - 1 },
                        MiniGdi::MakeColor(208, 208, 208));
                }
            }
            else if (controlKind == BuiltinControlKind::Progress)
            {
                const MiniGdi::Color background = progressBackgroundColor == 0xffffffffu
                    ? MiniGdi::MakeColor(232, 232, 232)
                    : ColorFromGuestColorRef(progressBackgroundColor);
                MiniGdi::DrawRectangle(*surface, fullRect, background,
                    MiniGdi::MakeColor(128, 128, 128));
                int fillLeft = 2;
                int fillRight = 2;
                if (progressMarquee)
                {
                    fillRight = (std::max)(fillLeft, 2 + (std::max)(0, width - 4) / 3);
                }
                else if (progressMaximum > progressMinimum)
                {
                    const std::int64_t range = static_cast<std::int64_t>(progressMaximum) - progressMinimum;
                    const std::int64_t value = (std::max)(progressMinimum,
                        (std::min)(progressMaximum, progressPosition)) - progressMinimum;
                    fillRight = 2 + static_cast<int>(value * (std::max)(0, width - 4) / range);
                }
                MiniGdi::Color bar = progressState == 2
                    ? MiniGdi::MakeColor(210, 45, 45)
                    : (progressState == 3
                        ? MiniGdi::MakeColor(225, 170, 25)
                        : MiniGdi::MakeColor(35, 165, 70));
                if (progressBarColor != 0xffffffffu)
                {
                    bar = ColorFromGuestColorRef(progressBarColor);
                }
                if (fillRight > fillLeft && height > 4)
                {
                    MiniGdi::FillRect(*surface,
                        MiniGdi::Rect{ fillLeft, 2, (std::min)(width - 2, fillRight), height - 2 }, bar);
                }
            }
            else if (controlKind == BuiltinControlKind::ComboBox ||
                controlKind == BuiltinControlKind::ScrollBar)
            {
                // For CBS_DROPDOWN/CBS_DROPDOWNLIST the CreateWindow height
                // includes the future drop-list.  Only the selection field is
                // visible while the list is closed.  Centering the caption in
                // the complete requested height can put it underneath later
                // dialog controls (a common resource-dialog layout).
                const int fieldHeight = controlKind == BuiltinControlKind::ComboBox
                    ? (std::min)(height,
                        GuestMetrics::ControlHeightForText(controlTextHeight))
                    : height;
                MiniGdi::DrawRectangle(
                    *surface,
                    MiniGdi::Rect{ 0, 0, width, fieldHeight },
                    MiniGdi::OpaqueWhite,
                    MiniGdi::MakeColor(128, 128, 128));
                const int buttonWidth = (std::min)(GuestMetrics::ScrollBarExtent,
                    (std::max)(1, width / 3));
                MiniGdi::FillRect(
                    *surface,
                    MiniGdi::Rect{ (std::max)(0, width - buttonWidth), 1,
                        (std::max)(0, width - 1), (std::max)(1, fieldHeight - 1) },
                    MiniGdi::MakeColor(240, 240, 240));
                if (controlKind == BuiltinControlKind::ComboBox && comboDropped &&
                    fieldHeight < height)
                {
                    MiniGdi::DrawRectangle(*surface,
                        MiniGdi::Rect{ 0, fieldHeight, width, height },
                        MiniGdi::OpaqueWhite, MiniGdi::MakeColor(128, 128, 128));
                }
            }
            else if (controlKind == BuiltinControlKind::UpDown)
            {
                MiniGdi::DrawRectangle(*surface, fullRect,
                    MiniGdi::MakeColor(240, 240, 240), MiniGdi::MakeColor(128, 128, 128));
                const bool horizontal = (style & 0x0040u) != 0;
                if (horizontal)
                {
                    const int middle = width / 2;
                    MiniGdi::DrawLine(*surface, MiniGdi::Point{ middle, 1 },
                        MiniGdi::Point{ middle, (std::max)(1, height - 2) }, MiniGdi::MakeColor(160, 160, 160));
                    DrawToolbarGlyph(*surface, MiniGdi::Rect{ 1, 1, (std::max)(2, middle - 1), height - 1 }, 0);
                    DrawToolbarGlyph(*surface, MiniGdi::Rect{ middle + 1, 1, width - 1, height - 1 }, 4);
                }
                else
                {
                    const int middle = height / 2;
                    MiniGdi::DrawLine(*surface, MiniGdi::Point{ 1, middle },
                        MiniGdi::Point{ (std::max)(1, width - 2), middle }, MiniGdi::MakeColor(160, 160, 160));
                    const MiniGdi::Color ink = MiniGdi::MakeColor(48, 48, 48);
                    const int centerX = width / 2;
                    const int upperY = (std::max)(2, middle / 2);
                    const int lowerY = middle + (std::max)(2, (height - middle) / 2);
                    MiniGdi::DrawLine(*surface, MiniGdi::Point{ centerX - 3, upperY + 2 },
                        MiniGdi::Point{ centerX, upperY - 1 }, ink);
                    MiniGdi::DrawLine(*surface, MiniGdi::Point{ centerX, upperY - 1 },
                        MiniGdi::Point{ centerX + 3, upperY + 2 }, ink);
                    MiniGdi::DrawLine(*surface, MiniGdi::Point{ centerX - 3, lowerY - 2 },
                        MiniGdi::Point{ centerX, lowerY + 1 }, ink);
                    MiniGdi::DrawLine(*surface, MiniGdi::Point{ centerX, lowerY + 1 },
                        MiniGdi::Point{ centerX + 3, lowerY - 2 }, ink);
                }
            }
            else if (controlKind == BuiltinControlKind::Header)
            {
                MiniGdi::DrawRectangle(
                    *surface,
                    fullRect,
                    MiniGdi::MakeColor(240, 240, 240),
                    MiniGdi::MakeColor(160, 160, 160));
            }
            else if (controlKind == BuiltinControlKind::ToolTip)
            {
                MiniGdi::DrawRectangle(
                    *surface,
                    fullRect,
                    MiniGdi::MakeColor(255, 255, 225),
                    MiniGdi::MakeColor(96, 96, 96));
            }

            MiniGdi::Color previousText = MiniGdi::OpaqueBlack;
            MiniGdi::BackgroundMode previousBackground = MiniGdi::BackgroundMode::Opaque;
            MiniGdi::ObjectHandle previousFont = MiniGdi::InvalidObject;
            const bool textColorSelected = m_gdi.SetTextColor(guestDc, textColor, &previousText);
            const bool backgroundModeSelected = m_gdi.SetBackgroundMode(
                guestDc,
                MiniGdi::BackgroundMode::Transparent,
                &previousBackground);
            const bool fontSelected = font != MiniGdi::InvalidObject &&
                m_gdi.SelectFont(guestDc, font, &previousFont);

            const auto measuredTextWidth = [this, guestDc, controlTextWidth](
                const wchar_t* characters, size_t count)
            {
                MiniGdi::Size measured{};
                if (m_gdi.GetTextExtentW(guestDc, characters, count, &measured))
                    return measured.width;
                const size_t bounded = (std::min)(count, static_cast<size_t>(
                    (std::numeric_limits<int>::max)() / controlTextWidth));
                return static_cast<int>(bounded * static_cast<size_t>(controlTextWidth));
            };

            const bool multilineEdit = controlKind == BuiltinControlKind::Edit &&
                (style & GuestAbi::EsMultiline) != 0;
            if (multilineEdit)
            {
                const int lineHeight = (std::max)(1, controlTextHeight);
                const bool hasVerticalScroll = (style & 0x00200000u /* WS_VSCROLL */) != 0;
                const bool hasHorizontalScroll = (style & 0x00100000u /* WS_HSCROLL */) != 0;
                const int verticalScrollWidth = hasVerticalScroll
                    ? (std::min)(GuestMetrics::ScrollBarExtent, (std::max)(0, width - 2)) : 0;
                const int horizontalScrollHeight = hasHorizontalScroll
                    ? (std::min)(GuestMetrics::ScrollBarExtent, (std::max)(0, height - 2)) : 0;
                const int contentRight = (std::max)(2, width - 1 - verticalScrollWidth);
                const int contentBottom = (std::max)(2, height - 1 - horizontalScrollHeight);
                const int availableWidth = (std::max)(1, contentRight - 3);
                const bool wrap = (style & GuestAbi::EsAutoHScroll) == 0 &&
                    !hasHorizontalScroll;
                const auto lines = BuildEditVisualLines(
                    text, wrap, availableWidth, measuredTextWidth);
                const size_t clampedCaret = (std::min)(caret, text.size());
                const size_t caretLine = EditLineForPosition(lines, clampedCaret);
                const size_t visibleRows = static_cast<size_t>((std::max)(1,
                    (std::max)(1, contentBottom - 2) / lineHeight));
                size_t firstLine = (std::min)(editFirstVisibleLine, lines.size() - 1);
                if (focused)
                {
                    if (caretLine < firstLine) firstLine = caretLine;
                    else if (caretLine >= firstLine + visibleRows)
                        firstLine = caretLine - visibleRows + 1;
                }

                int horizontalOffset = wrap ? 0 : (std::max)(0, editHorizontalOffset);
                const EditVisualLine& caretVisualLine = lines[caretLine];
                const size_t caretInLine = (std::min)(clampedCaret, caretVisualLine.end);
                const int caretNaturalX = measuredTextWidth(
                    text.data() + caretVisualLine.start,
                    caretInLine - caretVisualLine.start);
                if (!wrap && focused)
                {
                    if (caretNaturalX < horizontalOffset)
                        horizontalOffset = caretNaturalX;
                    else if (caretNaturalX - horizontalOffset > availableWidth)
                        horizontalOffset = (std::max)(0,
                            caretNaturalX - availableWidth + controlTextWidth);
                }
                {
                    std::lock_guard<std::mutex> guard(window->lock);
                    if (!window->destroyed)
                    {
                        window->editFirstVisibleLine = firstLine;
                        window->editHorizontalOffset = horizontalOffset;
                    }
                }

                const bool showSelection = focused ||
                    (style & GuestAbi::EsNoHideSelection) != 0;
                const size_t selectionStart = showSelection ? (std::min)(
                    (std::min)(selectionAnchor, selectionEnd), text.size()) : 0;
                const size_t selectionFinish = showSelection ? (std::min)(
                    (std::max)(selectionAnchor, selectionEnd), text.size()) : 0;
                for (size_t row = 0; row < visibleRows && firstLine + row < lines.size(); ++row)
                {
                    const EditVisualLine& line = lines[firstLine + row];
                    const int lineY = 2 + static_cast<int>(row) * lineHeight;
                    if (lineY >= contentBottom) break;
                    const int lineX = 2 - horizontalOffset;
                    const size_t lineLength = line.end - line.start;
                    m_gdi.TextOutW(guestDc, MiniGdi::Point{ lineX, lineY },
                        text.data() + line.start, lineLength, nullptr);

                    const size_t selectedStart = (std::max)(selectionStart, line.start);
                    const size_t selectedEnd = (std::min)(selectionFinish, line.end);
                    if (selectedEnd > selectedStart)
                    {
                        const int selectedLeft = lineX + measuredTextWidth(
                            text.data() + line.start, selectedStart - line.start);
                        const int selectedRight = lineX + measuredTextWidth(
                            text.data() + line.start, selectedEnd - line.start);
                        const int clippedLeft = (std::max)(1, selectedLeft);
                        const int clippedRight = (std::min)(contentRight, selectedRight);
                        if (clippedRight > clippedLeft)
                        {
                            MiniGdi::FillRect(*surface,
                                MiniGdi::Rect{ clippedLeft, lineY, clippedRight,
                                    (std::min)(contentBottom, lineY + lineHeight) },
                                MiniGdi::MakeColor(0, 120, 215));
                            m_gdi.SetTextColor(guestDc, MiniGdi::OpaqueWhite, nullptr);
                            m_gdi.TextOutW(guestDc,
                                MiniGdi::Point{ selectedLeft, lineY },
                                text.data() + selectedStart,
                                selectedEnd - selectedStart, nullptr);
                            m_gdi.SetTextColor(guestDc, textColor, nullptr);
                        }
                    }
                }

                if (focused && enabled && caretLine >= firstLine &&
                    caretLine < firstLine + visibleRows)
                {
                    const int caretX = (std::max)(1, (std::min)(contentRight - 1,
                        2 - horizontalOffset + caretNaturalX));
                    const int caretY = 2 + static_cast<int>(caretLine - firstLine) * lineHeight;
                    MiniGdi::FillRect(*surface,
                        MiniGdi::Rect{ caretX, caretY, caretX + 1,
                            (std::min)(contentBottom, caretY + lineHeight) },
                        MiniGdi::MakeColor(0, 120, 215));
                }

                if (hasVerticalScroll && verticalScrollWidth > 0)
                {
                    const int left = width - verticalScrollWidth;
                    MiniGdi::DrawRectangle(*surface,
                        MiniGdi::Rect{ left, 0, width, contentBottom + 1 },
                        MiniGdi::MakeColor(240, 240, 240), MiniGdi::MakeColor(160, 160, 160));
                    const int button = (std::min)(verticalScrollWidth,
                        (std::max)(1, contentBottom / 2));
                    const int trackTop = button;
                    const int trackBottom = (std::max)(trackTop, contentBottom - button);
                    if (trackBottom > trackTop)
                    {
                        const int trackHeight = trackBottom - trackTop;
                        const int proportionalThumbHeight = static_cast<int>(
                            static_cast<unsigned long long>(trackHeight) * visibleRows /
                            lines.size());
                        const int thumbHeight = lines.size() <= visibleRows
                            ? trackHeight
                            : (std::min)(trackHeight,
                                (std::max)(8, proportionalThumbHeight));
                        const size_t maximumFirst = lines.size() > visibleRows
                            ? lines.size() - visibleRows : 0;
                        const int thumbTop = maximumFirst == 0 ? trackTop :
                            trackTop + static_cast<int>(
                                static_cast<unsigned long long>(trackHeight - thumbHeight) *
                                (std::min)(firstLine, maximumFirst) / maximumFirst);
                        MiniGdi::DrawRectangle(*surface,
                            MiniGdi::Rect{ left + 2, thumbTop, width - 2,
                                thumbTop + thumbHeight },
                            MiniGdi::MakeColor(205, 205, 205), MiniGdi::MakeColor(128, 128, 128));
                    }
                }
                if (hasHorizontalScroll && horizontalScrollHeight > 0)
                {
                    const int top = height - horizontalScrollHeight;
                    MiniGdi::DrawRectangle(*surface,
                        MiniGdi::Rect{ 0, top, contentRight + 1, height },
                        MiniGdi::MakeColor(240, 240, 240), MiniGdi::MakeColor(160, 160, 160));
                    int maximumLineWidth = 0;
                    for (const auto& line : lines)
                        maximumLineWidth = (std::max)(maximumLineWidth,
                            measuredTextWidth(text.data() + line.start,
                                line.end - line.start));
                    const int trackLeft = horizontalScrollHeight;
                    const int trackRight = (std::max)(trackLeft,
                        contentRight - horizontalScrollHeight);
                    if (trackRight > trackLeft)
                    {
                        const int trackWidth = trackRight - trackLeft;
                        const int proportionalThumbWidth = static_cast<int>(
                            static_cast<long long>(trackWidth) * availableWidth /
                            (std::max)(1, maximumLineWidth));
                        const int thumbWidth = maximumLineWidth <= availableWidth
                            ? trackWidth
                            : (std::min)(trackWidth,
                                (std::max)(8, proportionalThumbWidth));
                        const int maximumOffset = (std::max)(0,
                            maximumLineWidth - availableWidth);
                        const int thumbLeft = maximumOffset == 0 ? trackLeft :
                            trackLeft + static_cast<int>(
                                static_cast<long long>(trackWidth - thumbWidth) *
                                (std::min)(horizontalOffset, maximumOffset) / maximumOffset);
                        MiniGdi::DrawRectangle(*surface,
                            MiniGdi::Rect{ thumbLeft, top + 2,
                                thumbLeft + thumbWidth, height - 2 },
                            MiniGdi::MakeColor(205, 205, 205), MiniGdi::MakeColor(128, 128, 128));
                    }
                }
            }

            size_t displayTextStart = 0;
            if ((controlKind == BuiltinControlKind::Edit ||
                controlKind == BuiltinControlKind::ComboBox) && focused && !multilineEdit)
            {
                const size_t clampedCaret = (std::min)(caret, text.size());
                const int reservedRight = controlKind == BuiltinControlKind::ComboBox
                    ? GuestMetrics::ScrollBarExtent : 2;
                const int availableWidth = (std::max)(0, width - 2 - reservedRight);
                if (measuredTextWidth(text.data(), clampedCaret) > availableWidth)
                {
                    size_t low = 0;
                    size_t high = clampedCaret;
                    while (low < high)
                    {
                        const size_t middle = low + (high - low) / 2;
                        if (measuredTextWidth(text.data() + middle,
                            clampedCaret - middle) <= availableWidth)
                            high = middle;
                        else
                            low = middle + 1;
                    }
                    displayTextStart = low;
                }
            }
            if (controlKind == BuiltinControlKind::Edit ||
                controlKind == BuiltinControlKind::ComboBox)
            {
                if (!multilineEdit)
                {
                    std::lock_guard<std::mutex> guard(window->lock);
                    if (!window->destroyed)
                        window->editFirstVisibleCharacter = displayTextStart;
                }
            }

            const size_t visibleCharacters = static_cast<size_t>((std::max)(0, width / controlTextWidth));
            const size_t displayTextLength = text.size() - displayTextStart;
            const size_t visibleTextCharacters = (std::min)(displayTextLength, visibleCharacters);
            const int visibleTextWidth = measuredTextWidth(
                text.data() + displayTextStart, visibleTextCharacters);
            int textX = 2;
            int textY = (std::max)(0,
                (height - controlTextHeight) / 2);
            if (controlKind == BuiltinControlKind::Static)
            {
                if (!staticImage.Empty())
                {
                    const int imageWidth = (std::min)(width, staticImage.Width());
                    const int imageHeight = (std::min)(height, staticImage.Height());
                    DrawToolbarImage(*surface,
                        MiniGdi::Rect{
                            (width - imageWidth) / 2,
                            (height - imageHeight) / 2,
                            (width - imageWidth) / 2 + imageWidth,
                            (height - imageHeight) / 2 + imageHeight },
                        staticImage);
                }
                const DWORD alignment = style & 0x00000003u;
                if (alignment == GuestAbi::SsCenter)
                {
                    textX = (std::max)(0, (width - visibleTextWidth) / 2);
                }
                else if (alignment == GuestAbi::SsRight)
                {
                    textX = (std::max)(0, width - visibleTextWidth - 2);
                }
            }
            else if (controlKind == BuiltinControlKind::Button)
            {
                textX = (std::max)(2, (width - visibleTextWidth) / 2);
                if (pressed)
                {
                    ++textX;
                    ++textY;
                }
            }
            else if (controlKind == BuiltinControlKind::Edit)
            {
                const DWORD alignment = style & 0x00000003u;
                if (displayTextStart != 0)
                {
                    textX = 2;
                }
                else if (alignment == GuestAbi::EsCenter)
                {
                    textX = (std::max)(2, (width - visibleTextWidth) / 2);
                }
                else if (alignment == GuestAbi::EsRight)
                {
                    textX = (std::max)(2, width - visibleTextWidth - 2);
                }
            }
            else if (controlKind == BuiltinControlKind::ComboBox)
            {
                const int fieldHeight = (std::min)(height,
                    GuestMetrics::ControlHeightForText(controlTextHeight));
                textY = (std::max)(0,
                    (fieldHeight - GuestMetrics::TextHeight) / 2);
            }

            if ((controlKind != BuiltinControlKind::StatusBar ||
                (!statusBarSimple && statusBarParts.empty())) &&
                !multilineEdit &&
                controlKind != BuiltinControlKind::Tab &&
                controlKind != BuiltinControlKind::Header &&
                controlKind != BuiltinControlKind::Progress &&
                controlKind != BuiltinControlKind::TreeView &&
                controlKind != BuiltinControlKind::UpDown &&
                staticImage.Empty())
            {
                m_gdi.TextOutW(guestDc, MiniGdi::Point{ textX, textY },
                    text.data() + displayTextStart, displayTextLength, nullptr);
            }

            if ((controlKind == BuiltinControlKind::Edit ||
                controlKind == BuiltinControlKind::ComboBox) && focused && enabled &&
                !multilineEdit)
            {
                const size_t start = (std::max)(displayTextStart,
                    (std::min)((std::min)(selectionAnchor, selectionEnd), text.size()));
                const size_t end = (std::min)((std::max)(selectionAnchor, selectionEnd), text.size());
                if (end > start)
                {
                    const int selectionLeft = (std::min)(width - 1,
                        textX + measuredTextWidth(text.data() + displayTextStart,
                            start - displayTextStart));
                    int selectionRight = (std::min)(width - 1,
                        textX + measuredTextWidth(text.data() + displayTextStart,
                            end - displayTextStart));
                    if (controlKind == BuiltinControlKind::ComboBox)
                    {
                        selectionRight = (std::min)(selectionRight,
                            (std::max)(selectionLeft, width - GuestMetrics::ScrollBarExtent));
                    }
                    if (selectionRight > selectionLeft)
                    {
                        MiniGdi::FillRect(*surface,
                            MiniGdi::Rect{ selectionLeft, 2, selectionRight,
                                (std::max)(2, textY + controlTextHeight) },
                            MiniGdi::MakeColor(0, 120, 215));
                        m_gdi.SetTextColor(guestDc, MiniGdi::OpaqueWhite, nullptr);
                        m_gdi.TextOutW(guestDc,
                            MiniGdi::Point{ selectionLeft, textY },
                            text.data() + start, end - start, nullptr);
                        m_gdi.SetTextColor(guestDc, textColor, nullptr);
                    }
                }
            }

            if (controlKind == BuiltinControlKind::ComboBox && comboDropped)
            {
                const int fieldHeight = (std::min)(height,
                    GuestMetrics::ControlHeightForText(controlTextHeight));
                const int rowHeight = (std::max)(GuestMetrics::TextHeight + 4,
                    GuestMetrics::ControlHeightForText(controlTextHeight));
                int rowTop = fieldHeight;
                for (size_t index = 0; index < choiceItems.size() && rowTop < height; ++index)
                {
                    const int rowBottom = (std::min)(height, SaturatingAdd(rowTop, rowHeight));
                    const bool selected = static_cast<int>(index) == selectedChoice;
                    if (selected)
                    {
                        MiniGdi::FillRect(*surface,
                            MiniGdi::Rect{ 1, rowTop, (std::max)(1, width - 1), rowBottom },
                            MiniGdi::MakeColor(0, 120, 215));
                    }
                    m_gdi.SetTextColor(guestDc,
                        selected ? MiniGdi::OpaqueWhite : MiniGdi::OpaqueBlack, nullptr);
                    const size_t visible = (std::min)(choiceItems[index].size(),
                        static_cast<size_t>((std::max)(0, width - 6) /
                            controlTextWidth));
                    m_gdi.TextOutW(guestDc, MiniGdi::Point{ 3, rowTop + 2 },
                        choiceItems[index].data(), visible, nullptr);
                    rowTop = rowBottom;
                }
            }

            if (controlKind == BuiltinControlKind::StatusBar)
            {
                if (statusBarSimple)
                {
                    const size_t visible = (std::min)(statusBarSimpleText.size(),
                        static_cast<size_t>((std::max)(0, width - 6) / controlTextWidth));
                    m_gdi.TextOutW(guestDc, MiniGdi::Point{ 3, textY },
                        statusBarSimpleText.data(), visible, nullptr);
                }
                else
                {
                    int left = 0;
                    for (size_t part = 0; part < statusBarParts.size(); ++part)
                    {
                        const int right = statusBarParts[part] < 0
                            ? width
                            : (std::min)(width, (std::max)(left, statusBarParts[part]));
                        if (part != 0)
                        {
                            MiniGdi::DrawLine(*surface,
                                MiniGdi::Point{ left, 1 },
                                MiniGdi::Point{ left, (std::max)(1, height - 2) },
                                MiniGdi::MakeColor(184, 184, 184));
                        }
                        if (part < statusBarTexts.size())
                        {
                            const size_t visible = (std::min)(statusBarTexts[part].size(),
                                static_cast<size_t>((std::max)(0, right - left - 6) /
                                    controlTextWidth));
                            m_gdi.TextOutW(guestDc, MiniGdi::Point{ left + 3, textY },
                                statusBarTexts[part].data(), visible, nullptr);
                        }
                        left = right;
                        if (left >= width)
                        {
                            break;
                        }
                    }
                }
            }

            if (controlKind == BuiltinControlKind::Tab)
            {
                int left = 1;
                const int itemHeight = (std::min)((std::max)(1, height - 2),
                    (std::max)(18, tabItemHeight));
                for (size_t index = 0; index < tabItems.size(); ++index)
                {
                    const int itemWidth = tabItemWidth > 0
                        ? tabItemWidth
                        : (std::max)(32, static_cast<int>((std::min)(
                            tabItems[index].size(), static_cast<size_t>(128))) *
                            controlTextWidth + 16);
                    const int right = (std::min)(width - 1, left + itemWidth);
                    if (right <= left) break;
                    const bool selected = static_cast<int>(index) == tabSelectedItem;
                    MiniGdi::DrawRectangle(*surface,
                        MiniGdi::Rect{ left, selected ? 1 : 3, right, itemHeight + 1 },
                        selected ? MiniGdi::OpaqueWhite : MiniGdi::MakeColor(232, 232, 232),
                        MiniGdi::MakeColor(144, 144, 144));
                    int textLeft = left + 6;
                    if (tabImageList && index < tabItemImages.size() && tabItemImages[index] >= 0)
                    {
                        MiniGdi::Surface image;
                        if (CopyGuestImageListImage(tabImageList, tabItemImages[index], &image) && !image.Empty())
                        {
                            const int iconSize = (std::min)(14, itemHeight - 4);
                            DrawToolbarImage(*surface,
                                MiniGdi::Rect{ textLeft, 4, textLeft + iconSize, 4 + iconSize }, image);
                            textLeft += iconSize + 3;
                        }
                    }
                    const size_t visible = (std::min)(tabItems[index].size(),
                        static_cast<size_t>((std::max)(0, right - textLeft - 4) /
                            controlTextWidth));
                    m_gdi.TextOutW(guestDc,
                        MiniGdi::Point{ textLeft, (std::max)(2,
                            (itemHeight - controlTextHeight) / 2) },
                        tabItems[index].data(), visible, nullptr);
                    left = right;
                    if (left >= width - 1) break;
                }
            }

            if (controlKind == BuiltinControlKind::Header)
            {
                std::vector<size_t> display(headerItems.size());
                std::iota(display.begin(), display.end(), static_cast<size_t>(0));
                if (headerItemOrders.size() == display.size())
                    std::stable_sort(display.begin(), display.end(), [&headerItemOrders](size_t left, size_t right)
                    { return headerItemOrders[left] < headerItemOrders[right]; });
                int left = 0;
                for (const size_t item : display)
                {
                    const int itemWidth = item < headerItemWidths.size()
                        ? (std::max)(0, headerItemWidths[item]) : 120;
                    const int right = (std::min)(width, left + itemWidth);
                    if (right <= left) continue;
                    MiniGdi::DrawRectangle(*surface, MiniGdi::Rect{ left, 0, right, height },
                        static_cast<int>(item) == headerPressedItem
                            ? MiniGdi::MakeColor(216, 216, 216)
                            : MiniGdi::MakeColor(240, 240, 240),
                        MiniGdi::MakeColor(160, 160, 160));
                    int textLeft = left + 4;
                    if (headerImageList && item < headerItemImages.size() && headerItemImages[item] >= 0)
                    {
                        MiniGdi::Surface image;
                        if (CopyGuestImageListImage(headerImageList, headerItemImages[item], &image) && !image.Empty())
                        {
                            const int iconSize = (std::min)(14, (std::max)(1, height - 4));
                            DrawToolbarImage(*surface,
                                MiniGdi::Rect{ textLeft, 2, textLeft + iconSize, 2 + iconSize }, image);
                            textLeft += iconSize + 3;
                        }
                    }
                    const size_t visible = (std::min)(headerItems[item].size(),
                        static_cast<size_t>((std::max)(0, right - textLeft - 3) /
                            controlTextWidth));
                    m_gdi.TextOutW(guestDc, MiniGdi::Point{ textLeft,
                        (std::max)(0, (height - controlTextHeight) / 2) },
                        headerItems[item].data(), visible, nullptr);
                    left = right;
                    if (left >= width) break;
                }
            }

            if (controlKind == BuiltinControlKind::TreeView)
            {
                const MiniGdi::Color background = treeBackgroundColor == 0xffffffffu
                    ? MiniGdi::OpaqueWhite : ColorFromGuestColorRef(treeBackgroundColor);
                MiniGdi::DrawRectangle(*surface, MiniGdi::Rect{ 0, 0, width, height },
                    background, background);
                const auto visible = VisibleTreeNodes(treeNodes);
                size_t first = 0;
                if (treeTopItem)
                {
                    const auto found = std::find_if(visible.begin(), visible.end(), [treeTopItem](const VisibleTreeNode& item)
                    { return item.token == treeTopItem; });
                    if (found != visible.end()) first = static_cast<size_t>(found - visible.begin());
                }
                const int rowHeight = (std::max)(1, treeItemHeight);
                for (size_t position = first; position < visible.size(); ++position)
                {
                    const int row = static_cast<int>(position - first);
                    const int top = row * rowHeight;
                    if (top >= height) break;
                    const TreeNode* node = FindTreeNode(treeNodes, visible[position].token);
                    if (!node) continue;
                    const bool selected = node->token == treeSelectedItem ||
                        (node->state & TreeStateSelected) != 0;
                    if (selected)
                    {
                        MiniGdi::DrawRectangle(*surface,
                            MiniGdi::Rect{ 0, top, width, (std::min)(height, top + rowHeight) },
                            MiniGdi::MakeColor(0, 120, 215), MiniGdi::MakeColor(0, 120, 215));
                    }
                    const int branchLeft = visible[position].depth * treeIndent + 2;
                    const bool hasChildren = node->declaredChildren != 0 ||
                        std::any_of(treeNodes.begin(), treeNodes.end(), [node](const TreeNode& candidate)
                        { return candidate.parent == node->token; });
                    if (hasChildren)
                    {
                        const int boxSize = (std::max)(7, (std::min)(11, rowHeight - 4));
                        const int boxTop = top + (rowHeight - boxSize) / 2;
                        MiniGdi::DrawRectangle(*surface,
                            MiniGdi::Rect{ branchLeft, boxTop, branchLeft + boxSize, boxTop + boxSize },
                            MiniGdi::OpaqueWhite, MiniGdi::MakeColor(96, 96, 96));
                        MiniGdi::DrawLine(*surface,
                            MiniGdi::Point{ branchLeft + 2, boxTop + boxSize / 2 },
                            MiniGdi::Point{ branchLeft + boxSize - 2, boxTop + boxSize / 2 },
                            MiniGdi::MakeColor(64, 64, 64));
                        if ((node->state & TreeStateExpanded) == 0)
                        {
                            MiniGdi::DrawLine(*surface,
                                MiniGdi::Point{ branchLeft + boxSize / 2, boxTop + 2 },
                                MiniGdi::Point{ branchLeft + boxSize / 2, boxTop + boxSize - 2 },
                                MiniGdi::MakeColor(64, 64, 64));
                        }
                    }
                    int textLeft = branchLeft + treeIndent;
                    const int image = selected && node->selectedImage >= 0
                        ? node->selectedImage : node->image;
                    if (treeImageList && image >= 0)
                    {
                        MiniGdi::Surface icon;
                        if (CopyGuestImageListImage(treeImageList, image, &icon) && !icon.Empty())
                        {
                            const int iconSize = (std::max)(1, (std::min)(16, rowHeight - 2));
                            DrawToolbarImage(*surface,
                                MiniGdi::Rect{ textLeft, top + (rowHeight - iconSize) / 2,
                                    textLeft + iconSize, top + (rowHeight + iconSize) / 2 }, icon);
                            textLeft += iconSize + 3;
                        }
                    }
                    const size_t characters = (std::min)(node->text.size(),
                        static_cast<size_t>((std::max)(0, width - textLeft - 2) /
                            controlTextWidth));
                    const MiniGdi::Color ink = selected
                        ? MiniGdi::OpaqueWhite : ColorFromGuestColorRef(treeTextColor);
                    m_gdi.SetTextColor(guestDc, ink, nullptr);
                    m_gdi.TextOutW(guestDc, MiniGdi::Point{ textLeft,
                        top + (std::max)(0, (rowHeight - controlTextHeight) / 2) },
                        node->text.data(), characters, nullptr);
                }
            }

            if (controlKind == BuiltinControlKind::Toolbar && !toolbarCommands.empty())
            {
                const int buttonWidth = (std::max)(
                    GuestMetrics::DefaultBitmapExtent, toolbarButtonWidth);
                const int buttonHeight = (std::min)(height - 2 * GuestMetrics::Border,
                    (std::max)(GuestMetrics::DefaultBitmapExtent, toolbarButtonHeight));
                for (size_t index = 0; index < toolbarCommands.size(); ++index)
                {
                    const BYTE buttonStyle = index < toolbarButtonStyles.size() ? toolbarButtonStyles[index] : 0;
                    const BYTE buttonState = index < toolbarButtonStates.size() ? toolbarButtonStates[index] : ToolbarStateEnabled;
                    const int itemWidth = ToolbarItemWidth(
                        toolbarButtonStyles, toolbarBitmaps, toolbarButtonStates, index,
                        buttonWidth, &toolbarButtonTexts);
                    const int left = ToolbarItemLeft(
                        toolbarButtonStyles, toolbarBitmaps, toolbarButtonStates, index,
                        buttonWidth, &toolbarButtonTexts);
                    if ((buttonState & ToolbarStateHidden) != 0)
                    {
                        continue;
                    }
                    if (left >= width - 2)
                    {
                        break;
                    }
                    if ((buttonStyle & ToolbarStyleSeparator) != 0)
                    {
                        MiniGdi::DrawLine(
                            *surface,
                            MiniGdi::Point{ left + itemWidth / 2, 3 },
                            MiniGdi::Point{ left + itemWidth / 2, (std::max)(3, height - 4) },
                            MiniGdi::MakeColor(184, 184, 184));
                        continue;
                    }
                    const bool buttonEnabled = (buttonState & ToolbarStateEnabled) != 0;
                    const bool buttonPressed = static_cast<int>(index) == toolbarPressedIndex ||
                        (buttonState & (ToolbarStatePressed | ToolbarStateChecked)) != 0;
                    MiniGdi::DrawRectangle(
                        *surface,
                        MiniGdi::Rect{ left, 1, (std::min)(width - 2, left + itemWidth), 1 + buttonHeight },
                        buttonPressed
                            ? MiniGdi::MakeColor(214, 214, 214)
                            : (buttonEnabled ? MiniGdi::MakeColor(248, 248, 248) : MiniGdi::MakeColor(232, 232, 232)),
                        buttonEnabled ? MiniGdi::MakeColor(128, 128, 128) : MiniGdi::MakeColor(184, 184, 184));
                    MiniGdi::Surface image;
                    const int bitmapIndex = index < toolbarBitmaps.size()
                        ? toolbarBitmaps[index] : static_cast<int>(index);
                    const int iconWidth = (std::min)(itemWidth - 4, (std::max)(1, toolbarBitmapWidth));
                    const int iconHeight = (std::min)(buttonHeight - 4, (std::max)(1, toolbarBitmapHeight));
                    const bool hasText = index < toolbarButtonTexts.size() &&
                        !toolbarButtonTexts[index].empty();
                    const int iconTop = hasText
                        ? 1 + GuestMetrics::ControlVerticalPadding
                        : 1 + (buttonHeight - iconHeight) / 2;
                    const MiniGdi::Rect iconRect{ left + (itemWidth - iconWidth) / 2, iconTop,
                        left + (itemWidth - iconWidth) / 2 + iconWidth, iconTop + iconHeight };
                    if (toolbarImageList && bitmapIndex >= 0 &&
                        CopyGuestImageListImage(toolbarImageList, bitmapIndex, &image) && !image.Empty())
                    {
                        DrawToolbarImage(*surface, iconRect, image);
                    }
                    else
                    {
                        DrawToolbarGlyph(*surface, iconRect, bitmapIndex);
                    }
                    if (hasText)
                    {
                        const auto& label = toolbarButtonTexts[index];
                        const size_t visible = (std::min)(label.size(),
                            static_cast<size_t>((std::max)(0, itemWidth - 4) /
                                GuestMetrics::TextWidth));
                        const int labelWidth = static_cast<int>(visible) * GuestMetrics::TextWidth;
                        m_gdi.SetTextColor(guestDc, buttonEnabled
                            ? MiniGdi::OpaqueBlack : MiniGdi::MakeColor(144, 144, 144), nullptr);
                        m_gdi.TextOutW(guestDc,
                            MiniGdi::Point{ left + (itemWidth - labelWidth) / 2,
                                (std::max)(iconTop + iconHeight + 1,
                                    1 + buttonHeight - GuestMetrics::TextHeight - 1) },
                            label.data(), visible, nullptr);
                    }
                    if ((buttonStyle & ToolbarStyleDropDown) != 0 && itemWidth >= 14)
                    {
                        const MiniGdi::Color arrow = buttonEnabled
                            ? MiniGdi::MakeColor(48, 48, 48)
                            : MiniGdi::MakeColor(144, 144, 144);
                        const int arrowX = left + itemWidth - 7;
                        const int arrowY = 1 + buttonHeight / 2;
                        MiniGdi::DrawLine(*surface, MiniGdi::Point{ arrowX - 2, arrowY - 1 },
                            MiniGdi::Point{ arrowX, arrowY + 1 }, arrow);
                        MiniGdi::DrawLine(*surface, MiniGdi::Point{ arrowX, arrowY + 1 },
                            MiniGdi::Point{ arrowX + 2, arrowY - 1 }, arrow);
                    }
                }
            }
            else if (controlKind == BuiltinControlKind::ListView)
            {
                int columnLeft = 2;
                for (size_t displayIndex = 0; displayIndex < listDisplayColumns.size(); ++displayIndex)
                {
                    const size_t index = listDisplayColumns[displayIndex];
                    const int columnWidth = index < listColumnWidths.size()
                        ? (std::max)(24, listColumnWidths[index])
                        : 120;
                    const size_t visibleColumnCharacters = static_cast<size_t>((std::max)(0,
                        (columnWidth - 4) / controlTextWidth));
                    const size_t columnCharacters = (std::min)(
                        listColumns[index].size(), visibleColumnCharacters);
                    m_gdi.TextOutW(guestDc, MiniGdi::Point{ columnLeft + 2, 3 }, listColumns[index].data(), columnCharacters, nullptr);
                    MiniGdi::DrawLine(
                        *surface,
                        MiniGdi::Point{ (std::min)(width - 1, columnLeft + columnWidth), 1 },
                        MiniGdi::Point{ (std::min)(width - 1, columnLeft + columnWidth), (std::max)(1, height - 2) },
                        MiniGdi::MakeColor(208, 208, 208));
                    columnLeft += columnWidth;
                    if (columnLeft >= width - 2)
                    {
                        break;
                    }
                }
                const size_t visibleRows = static_cast<size_t>((std::max)(0,
                    (height - listViewHeaderHeight) / listViewRowHeight));
                const size_t firstVisible = static_cast<size_t>((std::max)(0, listViewTopItem));
                for (size_t displayIndex = 0;
                    firstVisible + displayIndex < listItems.size() && displayIndex < visibleRows;
                    ++displayIndex)
                {
                    const size_t index = firstVisible + displayIndex;
                    const int rowTop = listViewHeaderHeight +
                        static_cast<int>(displayIndex) * listViewRowHeight;
                    const bool selected = index < listViewItemStates.size() &&
                        (listViewItemStates[index] & ListViewStateSelected) != 0;
                    const bool editingLabel = static_cast<int>(index) == listViewEditItem;
                    if (selected)
                    {
                        MiniGdi::FillRect(
                            *surface,
                            MiniGdi::Rect{ 1, rowTop, (std::max)(1, width - 1), rowTop + listViewRowHeight },
                            MiniGdi::MakeColor(0, 120, 215));
                        m_gdi.SetTextColor(guestDc, MiniGdi::OpaqueWhite, nullptr);
                    }
                    else
                    {
                        // Separate text-background color is a real ListView
                        // setting; respecting it avoids white text artefacts
                        // when an application switches views or high-contrast colors.
                        MiniGdi::FillRect(
                            *surface,
                            MiniGdi::Rect{ 1, rowTop, (std::max)(1, width - 1), rowTop + listViewRowHeight },
                            ColorFromGuestColorRef(listViewTextBackgroundColor));
                    }
                    if (editingLabel)
                    {
                        const int firstColumnWidth = !listDisplayColumns.empty() &&
                            listDisplayColumns[0] < listColumnWidths.size()
                            ? (std::max)(24, listColumnWidths[listDisplayColumns[0]]) : 120;
                        MiniGdi::DrawRectangle(*surface,
                            MiniGdi::Rect{ 2, rowTop, (std::min)(width - 1, firstColumnWidth + 2),
                                rowTop + listViewRowHeight },
                            MiniGdi::OpaqueWhite, MiniGdi::MakeColor(0, 120, 215));
                        m_gdi.SetTextColor(guestDc, MiniGdi::OpaqueBlack, nullptr);
                    }
                    int cellLeft = 2;
                    const size_t cellCount = (std::max)(
                        static_cast<size_t>(1),
                        (std::max)(listItems[index].size(), listColumns.size()));
                    for (size_t displayIndex = 0; displayIndex < cellCount; ++displayIndex)
                    {
                        const size_t subItem = displayIndex < listDisplayColumns.size()
                            ? listDisplayColumns[displayIndex]
                            : displayIndex;
                        if (cellLeft >= width - 2)
                        {
                            break;
                        }
                        std::wstring callbackText;
                        const std::wstring emptyCell;
                        const std::wstring* cellText = subItem < listItems[index].size()
                            ? &listItems[index][subItem]
                            : &emptyCell;
                        if (editingLabel && displayIndex == 0)
                            cellText = &listViewEditText;
                        const bool needsImage = subItem == 0 && listViewImageList &&
                            (index >= listViewItemImages.size() || listViewItemImages[index] < 0);
                        if ((!editingLabel || displayIndex != 0) &&
                            (cellText->empty() || needsImage) && parent)
                        {
                            // Wine's list-view asks its owner for virtual
                            // text through LVN_GETDISPINFOW. Virtual list views use
                            // this owner-data path instead of inserting a string
                            // for every file, so without this request the
                            // report view has rows but no captions.
                            wchar_t scratch[260] = {};
                            GuestListViewDisplayInfoW notification = {};
                            notification.header.from = handle;
                            notification.header.identifier = controlId;
                            notification.header.code = ListViewNotifyGetDisplayInfoW;
                            notification.item.mask = (cellText->empty() ? ListViewItemText : 0) |
                                (needsImage ? ListViewItemImage : 0);
                            notification.item.item = static_cast<int>(index);
                            notification.item.subItem = static_cast<int>(subItem);
                            notification.item.text = scratch;
                            notification.item.textCapacity = static_cast<int>(_countof(scratch));
                            notification.item.itemData = index < listViewItemData.size()
                                ? listViewItemData[index]
                                : 0;
                            notification.item.image = index < listViewItemImages.size()
                                ? listViewItemImages[index]
                                : -1;
                            SendGuestMessage(
                                parent,
                                GuestAbi::WmNotify,
                                static_cast<WPARAM>(controlId),
                                reinterpret_cast<LPARAM>(&notification),
                                nullptr);
                            // A handler can deliberately leave pszText unchanged
                            // when it has no label for this cell.  Treat an empty
                            // scratch buffer as that outcome, rather than claiming
                            // that it supplied text and drawing a phantom blank.
                            const bool suppliedText =
                                TryReadGuestWideString(notification.item.text, &callbackText) &&
                                !callbackText.empty();
                            if (suppliedText)
                            {
                                cellText = &callbackText;
                                // A non-owner-data ListView may still ask for
                                // text lazily. Keep the owner's answer with
                                // the exact row and subitem so later paints do
                                // not call back with stale/default item data.
                                // This mirrors comctl32's retained-item path.
                                {
                                    std::lock_guard<std::mutex> guard(window->lock);
                                    if (!window->destroyed && index < window->listViewItems.size())
                                    {
                                        auto& liveRow = window->listViewItems[index];
                                        if (liveRow.size() <= subItem)
                                        {
                                            liveRow.resize(subItem + 1);
                                        }
                                        liveRow[subItem] = callbackText;
                                    }
                                }
                                if (index < listItems.size())
                                {
                                    if (listItems[index].size() <= subItem)
                                    {
                                        listItems[index].resize(subItem + 1);
                                    }
                                    listItems[index][subItem] = callbackText;
                                    cellText = &listItems[index][subItem];
                                }
                            }
                            if (subItem == 0 && notification.item.image >= 0)
                            {
                                if (index >= listViewItemImages.size())
                                {
                                    listViewItemImages.resize(index + 1, -1);
                                }
                                listViewItemImages[index] = notification.item.image;
                                std::lock_guard<std::mutex> guard(window->lock);
                                if (!window->destroyed && index < window->listViewItemImages.size())
                                {
                                    window->listViewItemImages[index] = notification.item.image;
                                }
                            }
                            if (g_ownerDataDisplayInfoDiagnostics < 32)
                            {
                                ++g_ownerDataDisplayInfoDiagnostics;
                                RuntimeDiagnostics::Record(
                                    std::wstring(L"LISTVIEW: display callback for row ") +
                                    std::to_wstring(index) + L", column " + std::to_wstring(subItem) +
                                    L", item-data " + std::to_wstring(static_cast<unsigned long long>(notification.item.itemData)) +
                                    (suppliedText ? L" returned text." : L" returned no usable text."));
                            }
                        }
                        const int cellWidth = subItem < listColumnWidths.size()
                            ? (std::max)(24, listColumnWidths[subItem])
                            : 120;
                        int textInset = 2;
                        if (subItem == 0 && listViewImageList && index < listViewItemImages.size() &&
                            listViewItemImages[index] >= 0)
                        {
                            MiniGdi::Surface image;
                            if (CopyGuestImageListImage(
                                listViewImageList, listViewItemImages[index], &image) && !image.Empty())
                            {
                                const int iconSize = (std::min)(14, listViewRowHeight - 2);
                                DrawToolbarImage(*surface,
                                    MiniGdi::Rect{ cellLeft + 2, rowTop + 1,
                                        cellLeft + 2 + iconSize, rowTop + 1 + iconSize }, image);
                                textInset += iconSize + 2;
                            }
                        }
                        const size_t visibleCellCharacters = static_cast<size_t>((std::max)(0,
                            (cellWidth - textInset - 2) / controlTextWidth));
                        const size_t cellCharacters = (std::min)(cellText->size(), visibleCellCharacters);
                        m_gdi.TextOutW(
                            guestDc,
                            MiniGdi::Point{ cellLeft + textInset, rowTop },
                            cellText->data(),
                            cellCharacters,
                            nullptr);
                        cellLeft += cellWidth;
                    }
                    if (selected)
                    {
                        m_gdi.SetTextColor(guestDc, textColor, nullptr);
                    }
                    if (editingLabel)
                    {
                        const int caretX = 4 + static_cast<int>((std::min)(
                            listViewEditCaret, listViewEditText.size())) * controlTextWidth;
                        MiniGdi::FillRect(*surface,
                            MiniGdi::Rect{ (std::min)(width - 2, caretX), rowTop + 1,
                                (std::min)(width - 1, caretX + 1), rowTop + listViewRowHeight - 1 },
                            MiniGdi::MakeColor(0, 120, 215));
                    }
                    if ((listViewExtendedStyle & ListViewExtendedGridLines) != 0)
                    {
                        MiniGdi::DrawLine(
                            *surface,
                            MiniGdi::Point{ 1, rowTop + listViewRowHeight - 1 },
                            MiniGdi::Point{ (std::max)(1, width - 2), rowTop + listViewRowHeight - 1 },
                            MiniGdi::MakeColor(208, 208, 208));
                    }
                }
            }

            if ((controlKind == BuiltinControlKind::Edit ||
                controlKind == BuiltinControlKind::ComboBox) && focused && enabled &&
                !multilineEdit)
            {
                const size_t clampedCaret = (std::min)(caret, text.size());
                const int caretX = (std::min)(
                    (std::max)(1, width - 2),
                    textX + measuredTextWidth(text.data() + displayTextStart,
                        clampedCaret - displayTextStart));
                MiniGdi::FillRect(
                    *surface,
                    MiniGdi::Rect{ caretX, 2, caretX + 1, (std::max)(2, height - 2) },
                    MiniGdi::MakeColor(0, 120, 215));
            }

            if (fontSelected)
            {
                m_gdi.SelectFont(guestDc, previousFont, nullptr);
            }
            if (backgroundModeSelected)
            {
                m_gdi.SetBackgroundMode(guestDc, previousBackground, nullptr);
            }
            if (textColorSelected)
            {
                m_gdi.SetTextColor(guestDc, previousText, nullptr);
            }
        }
        EndGuestPaint(handle, &paint, nullptr);
        return 0;
    }
    default:
        break;
    }

    switch (controlKind)
    {
    case BuiltinControlKind::Button:
        switch (message)
        {
        case GuestAbi::WmLButtonDown:
        {
            HWND handle = nullptr;
            bool enabled = false;
            {
                std::lock_guard<std::mutex> guard(window->lock);
                if (window->destroyed)
                {
                    return 0;
                }
                handle = window->handle;
                enabled = window->enabled;
            }
            if (!enabled)
            {
                return 0;
            }
            SetGuestFocus(handle, nullptr);
            SetGuestCapture(handle, nullptr);
            bool changed = false;
            {
                std::lock_guard<std::mutex> guard(window->lock);
                if (!window->destroyed && !window->buttonPressed)
                {
                    window->buttonPressed = true;
                    changed = true;
                }
            }
            if (changed)
            {
                invalidate();
            }
            return 0;
        }
        case GuestAbi::WmLButtonUp:
        {
            HWND handle = nullptr;
            bool clicked = false;
            {
                std::lock_guard<std::mutex> guard(window->lock);
                if (window->destroyed)
                {
                    return 0;
                }
                handle = window->handle;
                clicked = window->buttonPressed;
                window->buttonPressed = false;
            }
            DWORD ignored = ERROR_SUCCESS;
            if (GetGuestCapture(&ignored) == handle)
            {
                ReleaseGuestCapture(&ignored);
            }
            invalidate();
            if (clicked)
            {
                notifyParent(GuestAbi::BnClicked);
            }
            return 0;
        }
        case GuestAbi::WmKeyDown:
            if (wParam == GuestAbi::VkSpace)
            {
                bool changed = false;
                {
                    std::lock_guard<std::mutex> guard(window->lock);
                    if (!window->destroyed && window->enabled && !window->buttonKeyboardPressed)
                    {
                        window->buttonKeyboardPressed = true;
                        changed = true;
                    }
                }
                if (changed)
                {
                    invalidate();
                }
            }
            return 0;
        case GuestAbi::WmKeyUp:
            if (wParam == GuestAbi::VkSpace)
            {
                bool clicked = false;
                {
                    std::lock_guard<std::mutex> guard(window->lock);
                    if (!window->destroyed)
                    {
                        clicked = window->buttonKeyboardPressed;
                        window->buttonKeyboardPressed = false;
                    }
                }
                invalidate();
                if (clicked)
                {
                    notifyParent(GuestAbi::BnClicked);
                }
            }
            return 0;
        case GuestAbi::WmCaptureChanged:
        {
            bool changed = false;
            {
                std::lock_guard<std::mutex> guard(window->lock);
                if (!window->destroyed && window->buttonPressed)
                {
                    window->buttonPressed = false;
                    changed = true;
                }
            }
            if (changed)
            {
                invalidate();
            }
            return 0;
        }
        case GuestAbi::WmKillFocus:
        {
            bool changed = false;
            {
                std::lock_guard<std::mutex> guard(window->lock);
                if (!window->destroyed && window->buttonKeyboardPressed)
                {
                    window->buttonKeyboardPressed = false;
                    changed = true;
                }
            }
            if (changed)
            {
                invalidate();
            }
            return 0;
        }
        case GuestAbi::WmSetFocus:
        case GuestAbi::WmEnable:
            invalidate();
            return 0;
        default:
            return 0;
        }

    case BuiltinControlKind::Toolbar:
        switch (message)
        {
        case GuestAbi::WmLButtonDown:
        {
            const int x = static_cast<int>(static_cast<SHORT>(lParam & 0xffff));
            HWND handle = nullptr;
            int button = -1;
            {
                std::lock_guard<std::mutex> guard(window->lock);
                if (window->destroyed || !window->enabled)
                {
                    return 0;
                }
                for (size_t index = 0; index < window->toolbarCommands.size(); ++index)
                {
                    const int left = ToolbarItemLeft(window->toolbarButtonStyles, window->toolbarBitmaps,
                        window->toolbarButtonStates, index, window->toolbarButtonWidth,
                        &window->toolbarButtonTexts);
                    const int right = left + ToolbarItemWidth(window->toolbarButtonStyles, window->toolbarBitmaps,
                        window->toolbarButtonStates, index, window->toolbarButtonWidth,
                        &window->toolbarButtonTexts);
                    const BYTE style = index < window->toolbarButtonStyles.size() ? window->toolbarButtonStyles[index] : 0;
                    const BYTE state = index < window->toolbarButtonStates.size() ? window->toolbarButtonStates[index] : ToolbarStateEnabled;
                    if (x >= left && x < right && (style & ToolbarStyleSeparator) == 0 &&
                        (state & ToolbarStateEnabled) != 0)
                    {
                        window->toolbarPressedIndex = static_cast<int>(index);
                        button = static_cast<int>(index);
                        handle = window->handle;
                        break;
                    }
                }
            }
            if (button >= 0)
            {
                SetGuestFocus(handle, nullptr);
                SetGuestCapture(handle, nullptr);
                invalidate();
            }
            return 0;
        }
        case GuestAbi::WmLButtonUp:
        {
            HWND handle = nullptr;
            HWND parent = nullptr;
            int command = -1;
            {
                std::lock_guard<std::mutex> guard(window->lock);
                if (window->destroyed)
                {
                    return 0;
                }
                const int index = window->toolbarPressedIndex;
                window->toolbarPressedIndex = -1;
                if (index >= 0 && static_cast<size_t>(index) < window->toolbarCommands.size())
                {
                    command = window->toolbarCommands[static_cast<size_t>(index)];
                    handle = window->handle;
                    parent = window->parent;
                }
            }
            DWORD ignored = ERROR_SUCCESS;
            if (handle && GetGuestCapture(&ignored) == handle)
            {
                ReleaseGuestCapture(&ignored);
            }
            invalidate();
            if (command >= 0 && parent)
            {
                // Toolbar buttons notify their owner with the button command,
                // not the toolbar's child identifier, so owners can map each
                // button to its corresponding command.
                SendGuestMessage(
                    parent,
                    GuestAbi::WmCommand,
                    GuestAbi::MakeCommandWParam(static_cast<WORD>(command), 0),
                    reinterpret_cast<LPARAM>(handle),
                    nullptr);
            }
            return 0;
        }
        case GuestAbi::WmCaptureChanged:
        {
            bool changed = false;
            {
                std::lock_guard<std::mutex> guard(window->lock);
                changed = !window->destroyed && window->toolbarPressedIndex != -1;
                window->toolbarPressedIndex = -1;
            }
            if (changed)
            {
                invalidate();
            }
            return 0;
        }
        default:
            return 0;
        }

    case BuiltinControlKind::Header:
        switch (message)
        {
        case GuestAbi::WmLButtonDown:
        {
            const int x = static_cast<int>(static_cast<SHORT>(LOWORD(lParam)));
            HWND handle = nullptr;
            int pressed = -1;
            {
                std::lock_guard<std::mutex> guard(window->lock);
                if (window->destroyed || !window->enabled) return 0;
                std::vector<size_t> display(window->headerItems.size());
                std::iota(display.begin(), display.end(), static_cast<size_t>(0));
                if (window->headerItemOrders.size() == display.size())
                    std::stable_sort(display.begin(), display.end(), [window](size_t left, size_t right)
                    { return window->headerItemOrders[left] < window->headerItemOrders[right]; });
                int left = 0;
                for (const size_t item : display)
                {
                    const int right = left + (item < window->headerItemWidths.size()
                        ? (std::max)(0, window->headerItemWidths[item]) : 120);
                    if (x >= left && x < right)
                    {
                        pressed = static_cast<int>(item);
                        break;
                    }
                    left = right;
                }
                window->headerPressedItem = pressed;
                handle = window->handle;
            }
            if (pressed >= 0)
            {
                SetGuestFocus(handle, nullptr);
                SetGuestCapture(handle, nullptr);
                invalidate();
            }
            return 0;
        }
        case GuestAbi::WmLButtonUp:
        {
            const int x = static_cast<int>(static_cast<SHORT>(LOWORD(lParam)));
            HWND handle = nullptr;
            HWND parent = nullptr;
            UINT_PTR controlId = 0;
            int clicked = -1;
            {
                std::lock_guard<std::mutex> guard(window->lock);
                if (window->destroyed) return 0;
                const int pressed = window->headerPressedItem;
                window->headerPressedItem = -1;
                std::vector<size_t> display(window->headerItems.size());
                std::iota(display.begin(), display.end(), static_cast<size_t>(0));
                if (window->headerItemOrders.size() == display.size())
                    std::stable_sort(display.begin(), display.end(), [window](size_t left, size_t right)
                    { return window->headerItemOrders[left] < window->headerItemOrders[right]; });
                int left = 0;
                for (const size_t item : display)
                {
                    const int right = left + (item < window->headerItemWidths.size()
                        ? (std::max)(0, window->headerItemWidths[item]) : 120);
                    if (x >= left && x < right)
                    {
                        if (pressed == static_cast<int>(item)) clicked = pressed;
                        break;
                    }
                    left = right;
                }
                handle = window->handle;
                parent = window->parent;
                controlId = window->controlId;
            }
            DWORD ignored = ERROR_SUCCESS;
            if (handle && GetGuestCapture(&ignored) == handle) ReleaseGuestCapture(&ignored);
            invalidate();
            if (clicked >= 0 && parent)
            {
                GuestHeaderNotification notification = {};
                notification.header = GuestNotifyHeader{ handle, controlId, HeaderNotifyItemClickW };
                notification.item = clicked;
                notification.button = 0;
                SendGuestMessage(parent, GuestAbi::WmNotify, static_cast<WPARAM>(controlId),
                    reinterpret_cast<LPARAM>(&notification), nullptr);
            }
            return 0;
        }
        case GuestAbi::WmCaptureChanged:
        {
            bool changed = false;
            {
                std::lock_guard<std::mutex> guard(window->lock);
                changed = window->headerPressedItem >= 0;
                window->headerPressedItem = -1;
            }
            if (changed) invalidate();
            return 0;
        }
        default:
            return 0;
        }

    case BuiltinControlKind::TreeView:
    {
        const auto notifyTreeSelection = [this, window](UINT code, ULONG_PTR oldToken, ULONG_PTR newToken, UINT action)
        {
            HWND parent = nullptr;
            HWND handle = nullptr;
            UINT_PTR controlId = 0;
            TreeNode oldNode;
            TreeNode newNode;
            bool hasOld = false;
            bool hasNew = false;
            {
                std::lock_guard<std::mutex> guard(window->lock);
                if (window->destroyed) return static_cast<LRESULT>(0);
                parent = window->parent;
                handle = window->handle;
                controlId = window->controlId;
                const TreeNode* oldValue = FindTreeNode(window->treeNodes, oldToken);
                const TreeNode* newValue = FindTreeNode(window->treeNodes, newToken);
                if (oldValue) { oldNode = *oldValue; hasOld = true; }
                if (newValue) { newNode = *newValue; hasNew = true; }
            }
            if (!parent) return static_cast<LRESULT>(0);
            GuestTreeNotification notification = {};
            notification.header = GuestNotifyHeader{ handle, controlId, code };
            notification.action = action;
            if (hasOld)
            {
                notification.oldItem.mask = TreeItemHandle | TreeItemState | TreeItemParam;
                notification.oldItem.item = reinterpret_cast<HANDLE>(oldNode.token);
                notification.oldItem.state = oldNode.state;
                notification.oldItem.stateMask = 0xffffffffu;
                notification.oldItem.itemData = oldNode.itemData;
            }
            if (hasNew)
            {
                notification.newItem.mask = TreeItemHandle | TreeItemState | TreeItemParam;
                notification.newItem.item = reinterpret_cast<HANDLE>(newNode.token);
                notification.newItem.state = newNode.state;
                notification.newItem.stateMask = 0xffffffffu;
                notification.newItem.itemData = newNode.itemData;
            }
            return SendGuestMessage(parent, GuestAbi::WmNotify, static_cast<WPARAM>(controlId),
                reinterpret_cast<LPARAM>(&notification), nullptr);
        };

        if (message == GuestAbi::WmMouseWheel)
        {
            const short wheelDelta = static_cast<short>(HIWORD(wParam));
            bool changed = false;
            {
                std::lock_guard<std::mutex> guard(window->lock);
                const auto visible = VisibleTreeNodes(window->treeNodes);
                if (visible.empty()) return 0;
                size_t current = 0;
                if (window->treeTopItem)
                {
                    const auto found = std::find_if(visible.begin(), visible.end(), [window](const VisibleTreeNode& item)
                    { return item.token == window->treeTopItem; });
                    if (found != visible.end()) current = static_cast<size_t>(found - visible.begin());
                }
                const int requested = static_cast<int>(current) +
                    (wheelDelta > 0 ? -3 : wheelDelta < 0 ? 3 : 0);
                const size_t next = static_cast<size_t>((std::max)(0,
                    (std::min)(static_cast<int>(visible.size()) - 1, requested)));
                changed = visible[next].token != window->treeTopItem;
                window->treeTopItem = visible[next].token;
            }
            if (changed) invalidate();
            return 0;
        }

        if (message == GuestAbi::WmLButtonDown || message == GuestAbi::WmKeyDown)
        {
            ULONG_PTR requested = 0;
            ULONG_PTR previous = 0;
            UINT action = message == GuestAbi::WmLButtonDown ? 1u : 2u;
            bool toggled = false;
            HWND handle = nullptr;
            {
                std::lock_guard<std::mutex> guard(window->lock);
                if (window->destroyed || !window->enabled) return 0;
                auto visible = VisibleTreeNodes(window->treeNodes);
                size_t first = 0;
                if (window->treeTopItem)
                {
                    const auto topFound = std::find_if(visible.begin(), visible.end(), [window](const VisibleTreeNode& item)
                    { return item.token == window->treeTopItem; });
                    if (topFound != visible.end()) first = static_cast<size_t>(topFound - visible.begin());
                }
                previous = window->treeSelectedItem;
                if (message == GuestAbi::WmLButtonDown)
                {
                    const int x = static_cast<int>(static_cast<SHORT>(LOWORD(lParam)));
                    const int y = static_cast<int>(static_cast<SHORT>(HIWORD(lParam)));
                    const int row = y >= 0 && window->treeItemHeight > 0
                        ? y / window->treeItemHeight : -1;
                    const size_t index = row < 0 ? visible.size() : first + static_cast<size_t>(row);
                    if (index >= visible.size()) return 0;
                    requested = visible[index].token;
                    TreeNode* node = FindTreeNode(window->treeNodes, requested);
                    if (!node) return 0;
                    const int branchLeft = visible[index].depth * window->treeIndent + 2;
                    const bool hasChildren = node->declaredChildren != 0 ||
                        std::any_of(window->treeNodes.begin(), window->treeNodes.end(), [requested](const TreeNode& candidate)
                        { return candidate.parent == requested; });
                    if (hasChildren && x >= branchLeft && x < branchLeft + window->treeIndent)
                    {
                        node->state ^= TreeStateExpanded;
                        toggled = true;
                    }
                }
                else
                {
                    const auto selected = std::find_if(visible.begin(), visible.end(), [previous](const VisibleTreeNode& item)
                    { return item.token == previous; });
                    size_t index = selected == visible.end() ? 0 : static_cast<size_t>(selected - visible.begin());
                    if (visible.empty()) return 0;
                    TreeNode* node = selected == visible.end() ? nullptr : FindTreeNode(window->treeNodes, previous);
                    if (wParam == GuestAbi::VkUp)
                        requested = visible[index == 0 ? 0 : index - 1].token;
                    else if (wParam == GuestAbi::VkDown)
                        requested = visible[(std::min)(visible.size() - 1, index + 1)].token;
                    else if (wParam == GuestAbi::VkRight && node)
                    {
                        const auto child = std::find_if(window->treeNodes.begin(), window->treeNodes.end(),
                            [previous](const TreeNode& candidate) { return candidate.parent == previous; });
                        if (child != window->treeNodes.end())
                        {
                            if ((node->state & TreeStateExpanded) == 0)
                            {
                                node->state |= TreeStateExpanded;
                                toggled = true;
                            }
                            else requested = child->token;
                        }
                    }
                    else if (wParam == GuestAbi::VkLeft && node)
                    {
                        if ((node->state & TreeStateExpanded) != 0)
                        {
                            node->state &= ~TreeStateExpanded;
                            toggled = true;
                        }
                        else requested = node->parent;
                    }
                    else if (wParam != GuestAbi::VkUp && wParam != GuestAbi::VkDown)
                    {
                        return 0;
                    }
                }
                handle = window->handle;
            }
            if (toggled)
            {
                invalidate();
                return 0;
            }
            if (!requested || requested == previous) return 0;
            if (notifyTreeSelection(TreeNotifySelectionChangingW, previous, requested, action) != 0) return 0;
            {
                std::lock_guard<std::mutex> guard(window->lock);
                if (window->destroyed || !FindTreeNode(window->treeNodes, requested)) return 0;
                for (auto& node : window->treeNodes) node.state &= ~TreeStateSelected;
                TreeNode* node = FindTreeNode(window->treeNodes, requested);
                node->state |= TreeStateSelected;
                window->treeSelectedItem = requested;
            }
            SetGuestFocus(handle, nullptr);
            invalidate();
            notifyTreeSelection(TreeNotifySelectionChangedW, previous, requested, action);
            return 0;
        }
        return 0;
    }

    case BuiltinControlKind::Tab:
        if (message == GuestAbi::WmLButtonUp ||
            (message == GuestAbi::WmKeyDown &&
                (wParam == GuestAbi::VkLeft || wParam == GuestAbi::VkRight)))
        {
            HWND parent = nullptr;
            HWND handle = nullptr;
            UINT_PTR controlId = 0;
            int requested = -1;
            int previous = -1;
            {
                std::lock_guard<std::mutex> guard(window->lock);
                if (window->destroyed || !window->enabled || window->tabItems.empty()) return 0;
                previous = window->tabSelectedItem;
                if (message == GuestAbi::WmKeyDown)
                {
                    requested = previous < 0 ? 0 : previous +
                        (wParam == GuestAbi::VkLeft ? -1 : 1);
                    requested = (std::max)(0,
                        (std::min)(static_cast<int>(window->tabItems.size()) - 1, requested));
                }
                else
                {
                    const int x = static_cast<int>(static_cast<WORD>(lParam & 0xffff));
                    const int y = static_cast<int>(static_cast<WORD>(
                        (static_cast<ULONG_PTR>(lParam) >> 16) & 0xffff));
                    if (y < 1 || y >= 1 + window->tabItemHeight) return 0;
                    int left = 1;
                    for (size_t index = 0; index < window->tabItems.size(); ++index)
                    {
                        const int itemWidth = window->tabItemWidth > 0
                            ? window->tabItemWidth
                            : (std::max)(32, static_cast<int>((std::min)(
                                window->tabItems[index].size(), static_cast<size_t>(128))) *
                                window->controlTextWidth + 16);
                        if (x >= left && x < left + itemWidth)
                        {
                            requested = static_cast<int>(index);
                            break;
                        }
                        left += itemWidth;
                    }
                }
                parent = window->parent;
                handle = window->handle;
                controlId = window->controlId;
            }
            if (requested < 0 || requested == previous) return 0;
            GuestNotifyHeader notification{ handle, controlId, TabNotifySelectionChanging };
            if (parent && SendGuestMessage(parent, GuestAbi::WmNotify,
                static_cast<WPARAM>(controlId), reinterpret_cast<LPARAM>(&notification), nullptr) != 0)
            {
                return 0;
            }
            {
                std::lock_guard<std::mutex> guard(window->lock);
                if (window->destroyed || static_cast<size_t>(requested) >= window->tabItems.size()) return 0;
                window->tabSelectedItem = requested;
                window->tabFocusedItem = requested;
            }
            SetGuestFocus(handle, nullptr);
            invalidate();
            if (parent)
            {
                notification.code = TabNotifySelectionChange;
                SendGuestMessage(parent, GuestAbi::WmNotify,
                    static_cast<WPARAM>(controlId), reinterpret_cast<LPARAM>(&notification), nullptr);
            }
            return 0;
        }
        return 0;

    case BuiltinControlKind::ComboBox:
        if (message == GuestAbi::WmKillFocus)
        {
            bool closed = false;
            {
                std::lock_guard<std::mutex> guard(window->lock);
                closed = !window->destroyed && window->comboDropped;
                window->comboDropped = false;
            }
            invalidate();
            if (closed) notifyParent(ComboNotificationCloseUp);
            return 0;
        }
        if (message == GuestAbi::WmSetFocus || message == GuestAbi::WmEnable)
        {
            invalidate();
            return 0;
        }
        if (message == GuestAbi::WmLButtonDown)
        {
            const int x = static_cast<int>(static_cast<SHORT>(lParam & 0xffff));
            HWND handle = nullptr;
            {
                std::lock_guard<std::mutex> guard(window->lock);
                if (window->destroyed || !window->enabled)
                {
                    return 0;
                }
                const size_t requested = x <= 3
                    ? window->editFirstVisibleCharacter
                    : window->editFirstVisibleCharacter +
                        static_cast<size_t>((x - 2 + window->controlTextWidth / 2) /
                            window->controlTextWidth);
                window->editCaret = (std::min)(requested, window->title.size());
                window->editSelectionAnchor = window->editCaret;
                window->editSelectionEnd = window->editCaret;
                handle = window->handle;
            }
            SetGuestFocus(handle, nullptr);
            invalidate();
            return 0;
        }
        if (message == GuestAbi::WmLButtonUp)
        {
            const int x = static_cast<int>(static_cast<SHORT>(lParam & 0xffff));
            const int y = static_cast<int>(static_cast<SHORT>((lParam >> 16) & 0xffff));
            bool changed = false;
            bool dropChanged = false;
            bool dropped = false;
            {
                std::lock_guard<std::mutex> guard(window->lock);
                if (window->destroyed || !window->enabled) return 0;
                const int fieldHeight = (std::min)(window->surface.Height(),
                    GuestMetrics::ControlHeightForText(GuestMetrics::TextHeight));
                if (window->comboDropped && y >= fieldHeight)
                {
                    const int rowHeight = GuestMetrics::ControlHeightForText(GuestMetrics::TextHeight);
                    const int requested = (y - fieldHeight) / (std::max)(1, rowHeight);
                    if (requested >= 0 && static_cast<size_t>(requested) < window->choiceItems.size())
                    {
                        changed = requested != window->selectedChoice;
                        window->selectedChoice = requested;
                        window->title = window->choiceItems[static_cast<size_t>(requested)];
                        window->editCaret = window->title.size();
                        window->editSelectionAnchor = window->editCaret;
                        window->editSelectionEnd = window->editCaret;
                    }
                    window->comboDropped = false;
                    dropChanged = true;
                }
                else if (x >= (std::max)(0,
                    window->surface.Width() - GuestMetrics::ScrollBarExtent))
                {
                    window->comboDropped = !window->comboDropped;
                    dropChanged = true;
                }
                dropped = window->comboDropped;
            }
            if (changed || dropChanged) invalidate();
            if (changed) notifyParent(ComboNotificationSelectionChange);
            if (dropChanged) notifyParent(dropped
                ? ComboNotificationDropDown : ComboNotificationCloseUp);
            return 0;
        }
        if (message == GuestAbi::WmKeyDown && wParam == 0x73)
        {
            bool dropped = false;
            {
                std::lock_guard<std::mutex> guard(window->lock);
                if (window->destroyed || !window->enabled) return 0;
                window->comboDropped = !window->comboDropped;
                dropped = window->comboDropped;
            }
            invalidate();
            notifyParent(dropped ? ComboNotificationDropDown : ComboNotificationCloseUp);
            return 0;
        }
        if (message == GuestAbi::WmKeyDown &&
            (wParam == GuestAbi::VkUp || wParam == GuestAbi::VkDown))
        {
            bool changed = false;
            {
                std::lock_guard<std::mutex> guard(window->lock);
                if (window->destroyed || !window->enabled || window->choiceItems.empty())
                {
                    return 0;
                }
                int next = window->selectedChoice;
                if (message == GuestAbi::WmKeyDown && wParam == GuestAbi::VkUp)
                {
                    next = next <= 0 ? static_cast<int>(window->choiceItems.size()) - 1 : next - 1;
                }
                else
                {
                    next = next + 1;
                    if (next < 0 || static_cast<size_t>(next) >= window->choiceItems.size())
                    {
                        next = 0;
                    }
                }
                changed = next != window->selectedChoice;
                window->selectedChoice = next;
                window->title = window->choiceItems[static_cast<size_t>(next)];
                window->editCaret = window->title.size();
                window->editSelectionAnchor = window->editCaret;
                window->editSelectionEnd = window->editCaret;
            }
            if (changed)
            {
                invalidate();
                // CBN_SELCHANGE. Returning the ComboBoxEx itself from
                // CBEM_GETCOMBOCONTROL makes this equally useful to clients
                // that wire the address bar through the inner combo handle.
                notifyParent(ComboNotificationSelectionChange);
            }
            return 0;
        }
        if (message == GuestAbi::WmChar)
        {
            bool changed = false;
            {
                std::lock_guard<std::mutex> guard(window->lock);
                if (window->destroyed || !window->enabled)
                {
                    return 0;
                }
                window->editCaret = (std::min)(window->editCaret, window->title.size());
                size_t selectionStart = (std::min)(window->editSelectionAnchor,
                    window->editSelectionEnd);
                size_t selectionEnd = (std::max)(window->editSelectionAnchor,
                    window->editSelectionEnd);
                selectionStart = (std::min)(selectionStart, window->title.size());
                selectionEnd = (std::min)(selectionEnd, window->title.size());
                if (wParam == GuestAbi::VkBack)
                {
                    if (selectionEnd > selectionStart)
                    {
                        window->title.erase(selectionStart, selectionEnd - selectionStart);
                        window->editCaret = selectionStart;
                        changed = true;
                    }
                    else if (window->editCaret != 0)
                    {
                        window->title.erase(window->editCaret - 1, 1);
                        --window->editCaret;
                        changed = true;
                    }
                }
                else if (wParam >= 0x20 && wParam <= 0xfffd && wParam != 0x7f &&
                    window->title.size() - (selectionEnd - selectionStart) < window->editTextLimit)
                {
                    if (selectionEnd > selectionStart)
                        window->title.erase(selectionStart, selectionEnd - selectionStart);
                    window->editCaret = selectionStart;
                    window->title.insert(window->editCaret++, 1, static_cast<wchar_t>(wParam));
                    changed = true;
                }
                if (changed)
                {
                    window->editSelectionAnchor = window->editCaret;
                    window->editSelectionEnd = window->editCaret;
                }
            }
            if (changed)
            {
                invalidate();
                notifyParent(5); // CBN_EDITCHANGE
            }
            return 0;
        }
        if (message == GuestAbi::WmKeyDown)
        {
            bool redraw = false;
            bool changed = false;
            bool accept = wParam == GuestAbi::VkReturn;
            const bool shift = (MouseInput().GetKeyState(
                static_cast<int>(GuestAbi::VkShift)) & 0x8000) != 0;
            const bool control = (MouseInput().GetKeyState(
                static_cast<int>(GuestAbi::VkControl)) & 0x8000) != 0;
            {
                std::lock_guard<std::mutex> guard(window->lock);
                if (window->destroyed || !window->enabled)
                {
                    return 0;
                }
                window->editCaret = (std::min)(window->editCaret, window->title.size());
                window->editSelectionAnchor = (std::min)(
                    window->editSelectionAnchor, window->title.size());
                window->editSelectionEnd = (std::min)(
                    window->editSelectionEnd, window->title.size());
                const size_t selectionStart = (std::min)(
                    window->editSelectionAnchor, window->editSelectionEnd);
                const size_t selectionEnd = (std::max)(
                    window->editSelectionAnchor, window->editSelectionEnd);
                const auto moveCaret = [window, shift, &redraw](size_t requested)
                {
                    requested = (std::min)(requested, window->title.size());
                    if (!shift)
                        window->editSelectionAnchor = requested;
                    window->editCaret = requested;
                    window->editSelectionEnd = requested;
                    redraw = true;
                };
                switch (wParam)
                {
                case GuestAbi::VkLeft:
                    if (!shift && selectionEnd > selectionStart)
                        moveCaret(selectionStart);
                    else if (window->editCaret != 0)
                        moveCaret(window->editCaret >= 2 &&
                            window->title[window->editCaret - 2] == L'\r' &&
                            window->title[window->editCaret - 1] == L'\n'
                                ? window->editCaret - 2 : window->editCaret - 1);
                    break;
                case GuestAbi::VkRight:
                    if (!shift && selectionEnd > selectionStart)
                        moveCaret(selectionEnd);
                    else if (window->editCaret < window->title.size())
                        moveCaret(window->editCaret + 1 < window->title.size() &&
                            window->title[window->editCaret] == L'\r' &&
                            window->title[window->editCaret + 1] == L'\n'
                                ? window->editCaret + 2 : window->editCaret + 1);
                    break;
                case GuestAbi::VkHome:
                    moveCaret(0);
                    break;
                case GuestAbi::VkEnd:
                    moveCaret(window->title.size());
                    break;
                case GuestAbi::VkDelete:
                    if (selectionEnd > selectionStart)
                    {
                        window->title.erase(selectionStart, selectionEnd - selectionStart);
                        window->editCaret = selectionStart;
                        window->editSelectionAnchor = selectionStart;
                        window->editSelectionEnd = selectionStart;
                        redraw = changed = true;
                    }
                    else if (window->editCaret < window->title.size())
                    {
                        window->title.erase(window->editCaret, 1);
                        redraw = changed = true;
                    }
                    break;
                default:
                    if (control && (wParam == 'A' || wParam == 'a'))
                    {
                        window->editSelectionAnchor = 0;
                        window->editSelectionEnd = window->title.size();
                        window->editCaret = window->title.size();
                        redraw = true;
                    }
                    break;
                }
            }
            if (redraw)
            {
                invalidate();
            }
            if (changed)
            {
                notifyParent(5); // CBN_EDITCHANGE
            }
            if (accept)
            {
                notifyParent(9); // CBN_SELENDOK
            }
            return 0;
        }
        return 0;

    case BuiltinControlKind::ListView:
    {
        const auto notifyOwner = [this, window](UINT code, int item, UINT oldState, UINT newState, UINT changed, POINT point)
        {
            HWND parent = nullptr;
            HWND handle = nullptr;
            UINT_PTR controlId = 0;
            LPARAM itemData = 0;
            {
                std::lock_guard<std::mutex> guard(window->lock);
                if (window->destroyed)
                {
                    return;
                }
                parent = window->parent;
                handle = window->handle;
                controlId = window->controlId;
                if (item >= 0 && static_cast<size_t>(item) < window->listViewItemData.size())
                {
                    itemData = window->listViewItemData[static_cast<size_t>(item)];
                }
            }
            if (!parent)
            {
                return;
            }
            GuestListViewNotification notification = {};
            notification.header.from = handle;
            notification.header.identifier = controlId;
            notification.header.code = code;
            notification.item = item;
            notification.newState = newState;
            notification.oldState = oldState;
            notification.changed = changed;
            notification.action = point;
            notification.itemData = itemData;
            SendGuestMessage(
                parent,
                GuestAbi::WmNotify,
                static_cast<WPARAM>(controlId),
                reinterpret_cast<LPARAM>(&notification),
                nullptr);
        };
        const auto finishLabelEdit = [this, window, &invalidate](bool accept)
        {
            HWND parent = nullptr;
            HWND handle = nullptr;
            UINT_PTR controlId = 0;
            int item = -1;
            LPARAM itemData = 0;
            std::wstring text;
            {
                std::lock_guard<std::mutex> guard(window->lock);
                if (window->destroyed || window->listViewEditItem < 0) return false;
                parent = window->parent;
                handle = window->handle;
                controlId = window->controlId;
                item = window->listViewEditItem;
                text = window->listViewEditText;
                if (static_cast<size_t>(item) < window->listViewItemData.size())
                    itemData = window->listViewItemData[static_cast<size_t>(item)];
                window->listViewEditItem = -1;
                window->listViewEditText.clear();
                window->listViewEditCaret = 0;
            }
            GuestListViewDisplayInfoW notification = {};
            notification.header.from = handle;
            notification.header.identifier = controlId;
            notification.header.code = ListViewNotifyEndLabelEditW;
            notification.item.mask = ListViewItemText | ListViewItemParam;
            notification.item.item = item;
            notification.item.text = accept ? const_cast<LPWSTR>(text.c_str()) : nullptr;
            notification.item.textCapacity = accept ? static_cast<int>(text.size() + 1) : 0;
            notification.item.itemData = itemData;
            const bool accepted = accept && parent && SendGuestMessage(parent,
                GuestAbi::WmNotify, static_cast<WPARAM>(controlId),
                reinterpret_cast<LPARAM>(&notification), nullptr) != FALSE;
            if (accepted)
            {
                std::lock_guard<std::mutex> guard(window->lock);
                if (!window->destroyed && item >= 0 &&
                    static_cast<size_t>(item) < window->listViewItems.size())
                {
                    auto& row = window->listViewItems[static_cast<size_t>(item)];
                    if (row.empty()) row.resize(1);
                    row[0] = text;
                }
            }
            invalidate();
            RuntimeDiagnostics::Record(std::wstring(L"LISTVIEW: label edit ") +
                (accepted ? L"accepted." : accept ? L"rejected by owner." : L"cancelled."));
            return accepted;
        };
        switch (message)
        {
        case GuestAbi::WmChar:
        {
            bool changed = false;
            {
                std::lock_guard<std::mutex> guard(window->lock);
                if (window->listViewEditItem < 0) return 0;
                window->listViewEditCaret = (std::min)(window->listViewEditCaret,
                    window->listViewEditText.size());
                if (wParam == GuestAbi::VkBack)
                {
                    if (window->listViewEditCaret > 0)
                    {
                        window->listViewEditText.erase(window->listViewEditCaret - 1, 1);
                        --window->listViewEditCaret;
                        changed = true;
                    }
                }
                else if (wParam >= 0x20 && wParam <= 0xfffd && wParam != 0x7f &&
                    window->listViewEditText.size() < MaximumBuiltinControlTextLength)
                {
                    window->listViewEditText.insert(window->listViewEditCaret, 1,
                        static_cast<wchar_t>(wParam));
                    ++window->listViewEditCaret;
                    changed = true;
                }
            }
            if (changed) invalidate();
            return 0;
        }
        case GuestAbi::WmLButtonDown:
        {
            bool wasEditing = false;
            {
                std::lock_guard<std::mutex> guard(window->lock);
                wasEditing = window->listViewEditItem >= 0;
            }
            if (wasEditing) finishLabelEdit(true);
            const int x = static_cast<int>(static_cast<SHORT>(lParam & 0xffff));
            const int y = static_cast<int>(static_cast<WORD>((static_cast<ULONG_PTR>(lParam) >> 16) & 0xffff));
            HWND handle = nullptr;
            bool changed = false;
            bool activate = false;
            int item = -1;
            int previousItem = -1;
            {
                std::lock_guard<std::mutex> guard(window->lock);
                if (window->destroyed || !window->enabled)
                {
                    return 0;
                }
                item = y < window->listViewHeaderHeight ? -1 :
                    window->listViewTopItem +
                    (y - window->listViewHeaderHeight) / window->listViewRowHeight;
                if (item >= 0 && static_cast<size_t>(item) < window->listViewItems.size())
                {
                    previousItem = window->listViewSelectedItem;
                    changed = previousItem != item;
                    if (previousItem >= 0 &&
                        static_cast<size_t>(previousItem) < window->listViewItemStates.size())
                    {
                        window->listViewItemStates[static_cast<size_t>(previousItem)] &=
                            ~(ListViewStateSelected | ListViewStateFocused);
                    }
                    window->listViewItemStates[static_cast<size_t>(item)] |=
                        ListViewStateSelected | ListViewStateFocused;
                    window->listViewSelectedItem = item;
                    window->listViewSelectionMark = item;
                    window->listViewPressedItem = item;
                    const auto now = std::chrono::steady_clock::now();
                    activate = window->listViewLastClickItem == item &&
                        now - window->listViewLastClick <= std::chrono::milliseconds(500);
                    window->listViewLastClick = now;
                    window->listViewLastClickItem = item;
                    handle = window->handle;
                }
            }
            if (handle)
            {
                SetGuestFocus(handle, nullptr);
                if (changed)
                {
                    invalidate();
                }
            }
            if (changed)
            {
                if (previousItem >= 0)
                {
                    notifyOwner(ListViewNotifyItemChanged, previousItem,
                        ListViewStateSelected, 0, ListViewStateSelected, POINT{ x, y });
                }
                notifyOwner(
                    ListViewNotifyItemChanged,
                    item,
                    0,
                    ListViewStateSelected,
                    ListViewStateSelected,
                    POINT{ x, y });
            }
            if (activate)
            {
                notifyOwner(NotifyDoubleClick, item, 0, 0, 0, POINT{ x, y });
                notifyOwner(ListViewNotifyItemActivate, item, 0, ListViewStateSelected,
                    ListViewStateSelected, POINT{ x, y });
            }
            return 0;
        }
        case GuestAbi::WmLButtonUp:
        {
            const int x = static_cast<int>(static_cast<WORD>(lParam & 0xffff));
            const int y = static_cast<int>(static_cast<WORD>((static_cast<ULONG_PTR>(lParam) >> 16) & 0xffff));
            int item = -1;
            bool clicked = false;
            {
                std::lock_guard<std::mutex> guard(window->lock);
                item = y < window->listViewHeaderHeight ? -1 :
                    window->listViewTopItem +
                    (y - window->listViewHeaderHeight) / window->listViewRowHeight;
                clicked = item >= 0 && static_cast<size_t>(item) < window->listViewItems.size() &&
                    item == window->listViewPressedItem;
                window->listViewPressedItem = -1;
            }
            if (clicked)
            {
                notifyOwner(NotifyClick, item, 0, 0, 0, POINT{ x, y });
            }
            return 0;
        }
        case GuestAbi::WmRButtonUp:
        {
            // Native list-view forwards a context-menu request to its owner.
            // Owners commonly build their item menu from WM_CONTEXTMENU;
            // merely forwarding WM_RBUTTONUP leaves that path dormant.
            HWND parent = nullptr;
            HWND handle = nullptr;
            {
                std::lock_guard<std::mutex> guard(window->lock);
                if (window->destroyed || !window->enabled)
                {
                    return 0;
                }
                parent = window->parent;
                handle = window->handle;
            }
            if (parent)
            {
                LPARAM contextPosition = lParam;
                HWND rootWindow = nullptr;
                int rootWidth = 0;
                int rootHeight = 0;
                int targetWidth = 0;
                int targetHeight = 0;
                int targetLeft = 0;
                int targetTop = 0;
                if (GetGuestSurfaceGeometry(handle, &rootWindow, &rootWidth, &rootHeight,
                    &targetWidth, &targetHeight, &targetLeft, &targetTop))
                {
                    const int clientX = static_cast<int>(static_cast<short>(lParam & 0xffff));
                    const int clientY = static_cast<int>(static_cast<short>((lParam >> 16) & 0xffff));
                    const int screenX = SaturatingAdd(targetLeft, clientX);
                    const int screenY = SaturatingAdd(targetTop, clientY);
                    contextPosition = GuestAbi::MakeMouseLParam(
                        SignedCoordinateWord(screenX), SignedCoordinateWord(screenY));
                }
                RuntimeDiagnostics::Record(L"LISTVIEW: forwarding WM_CONTEXTMENU to its owner.");
                SendGuestMessage(parent, GuestAbi::WmContextMenu,
                    reinterpret_cast<WPARAM>(handle), contextPosition, nullptr);
            }
            return 0;
        }
        case GuestAbi::WmMouseWheel:
        {
            const short wheelDelta = static_cast<short>((static_cast<ULONG_PTR>(wParam) >> 16) & 0xffff);
            bool changed = false;
            {
                std::lock_guard<std::mutex> guard(window->lock);
                const int maximumTop = (std::max)(0, static_cast<int>(window->listViewItems.size()) - 1);
                const int next = (std::max)(0, (std::min)(maximumTop,
                    window->listViewTopItem + (wheelDelta > 0 ? -3 : wheelDelta < 0 ? 3 : 0)));
                changed = next != window->listViewTopItem;
                window->listViewTopItem = next;
            }
            if (changed)
            {
                invalidate();
            }
            return 0;
        }
        case GuestAbi::WmKeyDown:
        {
            const WPARAM key = wParam;
            bool editing = false;
            {
                std::lock_guard<std::mutex> guard(window->lock);
                editing = window->listViewEditItem >= 0;
                if (editing && key == GuestAbi::VkLeft && window->listViewEditCaret > 0)
                    --window->listViewEditCaret;
                else if (editing && key == GuestAbi::VkRight &&
                    window->listViewEditCaret < window->listViewEditText.size())
                    ++window->listViewEditCaret;
                else if (editing && key == GuestAbi::VkHome) window->listViewEditCaret = 0;
                else if (editing && key == GuestAbi::VkEnd)
                    window->listViewEditCaret = window->listViewEditText.size();
                else if (editing && key == GuestAbi::VkDelete &&
                    window->listViewEditCaret < window->listViewEditText.size())
                    window->listViewEditText.erase(window->listViewEditCaret, 1);
            }
            if (editing)
            {
                if (key == GuestAbi::VkReturn) finishLabelEdit(true);
                else if (key == GuestAbi::VkEscape) finishLabelEdit(false);
                else invalidate();
                return 0;
            }
            int item = -1;
            int previousItem = -1;
            bool changed = false;
            bool activate = false;
            {
                std::lock_guard<std::mutex> guard(window->lock);
                const int count = static_cast<int>(window->listViewItems.size());
                if (count == 0)
                {
                    return 0;
                }
                previousItem = window->listViewSelectedItem;
                item = previousItem < 0 ? 0 : previousItem;
                if (key == GuestAbi::VkUp) item = (std::max)(0, item - 1);
                else if (key == GuestAbi::VkDown) item = (std::min)(count - 1, item + 1);
                else if (key == GuestAbi::VkHome) item = 0;
                else if (key == GuestAbi::VkEnd) item = count - 1;
                else if (key == GuestAbi::VkReturn) activate = previousItem >= 0;
                else return 0;

                if (!activate)
                {
                    changed = previousItem != item;
                    if (previousItem >= 0 &&
                        static_cast<size_t>(previousItem) < window->listViewItemStates.size())
                    {
                        window->listViewItemStates[static_cast<size_t>(previousItem)] &=
                            ~(ListViewStateSelected | ListViewStateFocused);
                    }
                    window->listViewItemStates[static_cast<size_t>(item)] |=
                        ListViewStateSelected | ListViewStateFocused;
                    window->listViewSelectedItem = item;
                    window->listViewSelectionMark = item;
                    const int height = (std::max)(0, static_cast<int>(window->bounds.bottom - window->bounds.top));
                    const int rowsPerPage = (std::max)(1,
                        (height - window->listViewHeaderHeight) /
                            window->listViewRowHeight);
                    if (item < window->listViewTopItem)
                        window->listViewTopItem = item;
                    else if (item >= window->listViewTopItem + rowsPerPage)
                        window->listViewTopItem = item - rowsPerPage + 1;
                }
            }
            if (changed)
            {
                SetGuestFocus(window->handle, nullptr);
                invalidate();
                if (previousItem >= 0)
                {
                    notifyOwner(ListViewNotifyItemChanged, previousItem,
                        ListViewStateSelected, 0, ListViewStateSelected, POINT{});
                }
                notifyOwner(ListViewNotifyItemChanged, item,
                    0,
                    ListViewStateSelected, ListViewStateSelected, POINT{});
            }
            if (activate)
            {
                notifyOwner(ListViewNotifyItemActivate, item, 0,
                    ListViewStateSelected, ListViewStateSelected, POINT{});
            }
            return 0;
        }
        case GuestAbi::WmSetFocus:
            invalidate();
            return 0;
        case GuestAbi::WmKillFocus:
            finishLabelEdit(true);
            invalidate();
            return 0;
        default:
            return 0;
        }
    }

    case BuiltinControlKind::Edit:
        switch (message)
        {
        case GuestAbi::WmLButtonDown:
        {
            const int x = static_cast<int>(static_cast<SHORT>(lParam & 0xffff));
            const int y = static_cast<int>(static_cast<SHORT>(
                (static_cast<ULONG_PTR>(lParam) >> 16) & 0xffff));
            HWND handle = nullptr;
            {
                std::lock_guard<std::mutex> guard(window->lock);
                if (window->destroyed || !window->enabled)
                {
                    return 0;
                }
                size_t requested = 0;
                if ((window->style & GuestAbi::EsMultiline) != 0)
                {
                    const int characterWidth = (std::max)(1, window->controlTextWidth);
                    const int lineHeight = (std::max)(1, window->controlTextHeight);
                    const int availableWidth = (std::max)(1, window->surface.Width() - 4 -
                        ((window->style & 0x00200000u /* WS_VSCROLL */) != 0
                            ? GuestMetrics::ScrollBarExtent : 0));
                    const bool wrap = (window->style & GuestAbi::EsAutoHScroll) == 0 &&
                        (window->style & 0x00100000u /* WS_HSCROLL */) == 0;
                    const auto lines = BuildEditVisualLines(window->title, wrap,
                        availableWidth, [characterWidth](const wchar_t*, size_t count)
                    {
                        return static_cast<int>((std::min)(count, static_cast<size_t>(
                            (std::numeric_limits<int>::max)() / characterWidth))) * characterWidth;
                    });
                    const size_t row = window->editFirstVisibleLine +
                        static_cast<size_t>((std::max)(0, y - 2) / lineHeight);
                    const EditVisualLine& line = lines[(std::min)(row, lines.size() - 1)];
                    const int contentX = (std::max)(0,
                        x - 2 + window->editHorizontalOffset);
                    const size_t column = static_cast<size_t>((contentX + characterWidth / 2) /
                        characterWidth);
                    requested = line.start + (std::min)(column, line.end - line.start);
                }
                else
                {
                    const int firstCharacterCenter = 2 + window->controlTextWidth / 2;
                    requested = x <= firstCharacterCenter
                        ? window->editFirstVisibleCharacter
                        : window->editFirstVisibleCharacter +
                            static_cast<size_t>((x - 2 + window->controlTextWidth / 2) /
                                window->controlTextWidth);
                }
                window->editCaret = (std::min)(requested, window->title.size());
                window->editSelectionAnchor = window->editCaret;
                window->editSelectionEnd = window->editCaret;
                handle = window->handle;
            }
            SetGuestFocus(handle, nullptr);
            SetGuestCapture(handle, nullptr);
            invalidate();
            return 0;
        }
        case GuestAbi::WmMouseMove:
        case GuestAbi::WmLButtonUp:
        {
            if (message == GuestAbi::WmMouseMove && (wParam & 0x0001u) == 0)
                return 0;
            const int x = static_cast<int>(static_cast<SHORT>(lParam & 0xffff));
            const int y = static_cast<int>(static_cast<SHORT>(
                (static_cast<ULONG_PTR>(lParam) >> 16) & 0xffff));
            bool redraw = false;
            HWND handle = nullptr;
            {
                std::lock_guard<std::mutex> guard(window->lock);
                if (window->destroyed || !window->enabled) return 0;
                size_t requested = 0;
                if ((window->style & GuestAbi::EsMultiline) != 0)
                {
                    const int characterWidth = (std::max)(1, window->controlTextWidth);
                    const int lineHeight = (std::max)(1, window->controlTextHeight);
                    const int availableWidth = (std::max)(1, window->surface.Width() - 4 -
                        ((window->style & 0x00200000u /* WS_VSCROLL */) != 0
                            ? GuestMetrics::ScrollBarExtent : 0));
                    const bool wrap = (window->style & GuestAbi::EsAutoHScroll) == 0 &&
                        (window->style & 0x00100000u /* WS_HSCROLL */) == 0;
                    const auto lines = BuildEditVisualLines(window->title, wrap,
                        availableWidth, [characterWidth](const wchar_t*, size_t count)
                    {
                        return static_cast<int>((std::min)(count, static_cast<size_t>(
                            (std::numeric_limits<int>::max)() / characterWidth))) * characterWidth;
                    });
                    const size_t row = window->editFirstVisibleLine +
                        static_cast<size_t>((std::max)(0, y - 2) / lineHeight);
                    const EditVisualLine& line = lines[(std::min)(row, lines.size() - 1)];
                    const int contentX = (std::max)(0,
                        x - 2 + window->editHorizontalOffset);
                    const size_t column = static_cast<size_t>((contentX + characterWidth / 2) /
                        characterWidth);
                    requested = line.start + (std::min)(column, line.end - line.start);
                }
                else
                {
                    requested = x <= 2
                        ? window->editFirstVisibleCharacter
                        : window->editFirstVisibleCharacter +
                            static_cast<size_t>((x - 2 + window->controlTextWidth / 2) /
                                window->controlTextWidth);
                }
                const size_t clamped = (std::min)(requested, window->title.size());
                redraw = clamped != window->editSelectionEnd;
                window->editCaret = clamped;
                window->editSelectionEnd = clamped;
                handle = window->handle;
            }
            if (message == GuestAbi::WmLButtonUp)
            {
                DWORD ignored = ERROR_SUCCESS;
                if (GetGuestCapture(&ignored) == handle) ReleaseGuestCapture(&ignored);
            }
            if (redraw) invalidate();
            return 0;
        }
        case GuestAbi::WmMouseWheel:
        {
            const short delta = static_cast<short>(
                (static_cast<ULONG_PTR>(wParam) >> 16) & 0xffff);
            bool changed = false;
            {
                std::lock_guard<std::mutex> guard(window->lock);
                if ((window->style & GuestAbi::EsMultiline) == 0) return 0;
                const size_t previous = window->editFirstVisibleLine;
                if (delta > 0)
                    window->editFirstVisibleLine = previous > 3 ? previous - 3 : 0;
                else if (delta < 0)
                    window->editFirstVisibleLine = previous >
                        (std::numeric_limits<size_t>::max)() - 3
                            ? (std::numeric_limits<size_t>::max)() : previous + 3;
                changed = previous != window->editFirstVisibleLine;
            }
            if (changed) invalidate();
            return 0;
        }
        case GuestAbi::WmChar:
        {
            bool changed = false;
            {
                std::lock_guard<std::mutex> guard(window->lock);
                if (window->destroyed || !window->enabled || (window->style & GuestAbi::EsReadOnly) != 0)
                {
                    return 0;
                }
                window->editCaret = (std::min)(window->editCaret, window->title.size());
                size_t selectionStart = (std::min)(window->editSelectionAnchor,
                    window->editSelectionEnd);
                size_t selectionEnd = (std::max)(window->editSelectionAnchor,
                    window->editSelectionEnd);
                selectionStart = (std::min)(selectionStart, window->title.size());
                selectionEnd = (std::min)(selectionEnd, window->title.size());
                if (wParam == GuestAbi::VkBack)
                {
                    if (selectionEnd > selectionStart)
                    {
                        window->title.erase(selectionStart, selectionEnd - selectionStart);
                        window->editCaret = selectionStart;
                        changed = true;
                    }
                    else if (window->editCaret != 0)
                    {
                        if (window->editCaret >= 2 &&
                            window->title[window->editCaret - 2] == L'\r' &&
                            window->title[window->editCaret - 1] == L'\n')
                        {
                            window->title.erase(window->editCaret - 2, 2);
                            window->editCaret -= 2;
                        }
                        else
                        {
                            window->title.erase(window->editCaret - 1, 1);
                            --window->editCaret;
                        }
                        changed = true;
                    }
                }
                else if (wParam == L'\r' &&
                    (window->style & GuestAbi::EsMultiline) != 0 &&
                    window->title.size() - (selectionEnd - selectionStart) + 2 <=
                        window->editTextLimit)
                {
                    if (selectionEnd > selectionStart)
                        window->title.erase(selectionStart, selectionEnd - selectionStart);
                    window->editCaret = selectionStart;
                    window->title.insert(window->editCaret, L"\r\n");
                    window->editCaret += 2;
                    changed = true;
                }
                else if (wParam == L'\t' &&
                    (window->style & GuestAbi::EsMultiline) != 0 &&
                    window->title.size() - (selectionEnd - selectionStart) <
                        window->editTextLimit)
                {
                    if (selectionEnd > selectionStart)
                        window->title.erase(selectionStart, selectionEnd - selectionStart);
                    window->editCaret = selectionStart;
                    window->title.insert(window->editCaret++, 1, L'\t');
                    changed = true;
                }
                else if (wParam >= 0x20 && wParam <= 0xfffd && wParam != 0x7f &&
                    window->title.size() - (selectionEnd - selectionStart) < window->editTextLimit)
                {
                    if (selectionEnd > selectionStart)
                        window->title.erase(selectionStart, selectionEnd - selectionStart);
                    window->editCaret = selectionStart;
                    window->title.insert(window->editCaret++, 1, static_cast<wchar_t>(wParam));
                    changed = true;
                }
                if (changed)
                {
                    window->editSelectionAnchor = window->editCaret;
                    window->editSelectionEnd = window->editCaret;
                }
            }
            if (changed)
            {
                syncEditHandleFromTitle();
                invalidate();
                notifyParent(GuestAbi::EnChange);
            }
            return 0;
        }
        case GuestAbi::WmKeyDown:
        {
            bool redraw = false;
            bool changed = false;
            const bool shift = (MouseInput().GetKeyState(
                static_cast<int>(GuestAbi::VkShift)) & 0x8000) != 0;
            const bool control = (MouseInput().GetKeyState(
                static_cast<int>(GuestAbi::VkControl)) & 0x8000) != 0;
            {
                std::lock_guard<std::mutex> guard(window->lock);
                if (window->destroyed || !window->enabled)
                {
                    return 0;
                }
                window->editCaret = (std::min)(window->editCaret, window->title.size());
                window->editSelectionAnchor = (std::min)(
                    window->editSelectionAnchor, window->title.size());
                window->editSelectionEnd = (std::min)(
                    window->editSelectionEnd, window->title.size());
                const size_t selectionStart = (std::min)(
                    window->editSelectionAnchor, window->editSelectionEnd);
                const size_t selectionEnd = (std::max)(
                    window->editSelectionAnchor, window->editSelectionEnd);
                const auto moveCaret = [window, shift, &redraw](size_t requested)
                {
                    requested = (std::min)(requested, window->title.size());
                    if (!shift)
                        window->editSelectionAnchor = requested;
                    window->editCaret = requested;
                    window->editSelectionEnd = requested;
                    redraw = true;
                };
                const bool multiline = (window->style & GuestAbi::EsMultiline) != 0;
                std::vector<EditVisualLine> editLines;
                size_t currentLine = 0;
                if (multiline)
                {
                    const int characterWidth = (std::max)(1, window->controlTextWidth);
                    const int availableWidth = (std::max)(1, window->surface.Width() - 4 -
                        ((window->style & 0x00200000u /* WS_VSCROLL */) != 0
                            ? GuestMetrics::ScrollBarExtent : 0));
                    const bool wrap = (window->style & GuestAbi::EsAutoHScroll) == 0 &&
                        (window->style & 0x00100000u /* WS_HSCROLL */) == 0;
                    editLines = BuildEditVisualLines(window->title, wrap,
                        availableWidth, [characterWidth](const wchar_t*, size_t count)
                    {
                        return static_cast<int>((std::min)(count, static_cast<size_t>(
                            (std::numeric_limits<int>::max)() / characterWidth))) * characterWidth;
                    });
                    currentLine = EditLineForPosition(editLines, window->editCaret);
                }
                switch (wParam)
                {
                case GuestAbi::VkLeft:
                    if (!shift && selectionEnd > selectionStart)
                        moveCaret(selectionStart);
                    else if (window->editCaret != 0)
                        moveCaret(window->editCaret - 1);
                    break;
                case GuestAbi::VkRight:
                    if (!shift && selectionEnd > selectionStart)
                        moveCaret(selectionEnd);
                    else if (window->editCaret < window->title.size())
                        moveCaret(window->editCaret + 1);
                    break;
                case GuestAbi::VkHome:
                    moveCaret(multiline && !control ? editLines[currentLine].start : 0);
                    break;
                case GuestAbi::VkEnd:
                    moveCaret(multiline && !control
                        ? editLines[currentLine].end : window->title.size());
                    break;
                case GuestAbi::VkUp:
                    if (multiline && currentLine > 0)
                    {
                        const size_t column = (std::min)(window->editCaret,
                            editLines[currentLine].end) - editLines[currentLine].start;
                        const EditVisualLine& target = editLines[currentLine - 1];
                        moveCaret(target.start + (std::min)(column,
                            target.end - target.start));
                    }
                    break;
                case GuestAbi::VkDown:
                    if (multiline && currentLine + 1 < editLines.size())
                    {
                        const size_t column = (std::min)(window->editCaret,
                            editLines[currentLine].end) - editLines[currentLine].start;
                        const EditVisualLine& target = editLines[currentLine + 1];
                        moveCaret(target.start + (std::min)(column,
                            target.end - target.start));
                    }
                    break;
                case GuestAbi::VkDelete:
                    if ((window->style & GuestAbi::EsReadOnly) == 0 &&
                        selectionEnd > selectionStart)
                    {
                        window->title.erase(selectionStart, selectionEnd - selectionStart);
                        window->editCaret = selectionStart;
                        window->editSelectionAnchor = selectionStart;
                        window->editSelectionEnd = selectionStart;
                        redraw = true;
                        changed = true;
                    }
                    else if ((window->style & GuestAbi::EsReadOnly) == 0 &&
                        window->editCaret < window->title.size())
                    {
                        const size_t eraseCount = window->editCaret + 1 < window->title.size() &&
                            window->title[window->editCaret] == L'\r' &&
                            window->title[window->editCaret + 1] == L'\n' ? 2 : 1;
                        window->title.erase(window->editCaret, eraseCount);
                        redraw = true;
                        changed = true;
                    }
                    break;
                default:
                    if (control && (wParam == 'A' || wParam == 'a'))
                    {
                        window->editSelectionAnchor = 0;
                        window->editSelectionEnd = window->title.size();
                        window->editCaret = window->title.size();
                        redraw = true;
                    }
                    break;
                }
            }
            if (redraw)
            {
                invalidate();
            }
            if (changed)
            {
                syncEditHandleFromTitle();
                notifyParent(GuestAbi::EnChange);
            }
            return 0;
        }
        case GuestAbi::WmSetFocus:
        case GuestAbi::WmKillFocus:
        case GuestAbi::WmEnable:
            invalidate();
            return 0;
        default:
            return 0;
        }

    case BuiltinControlKind::Static:
        if (message == GuestAbi::StmSetIcon ||
            (message == GuestAbi::StmSetImage && wParam == GuestAbi::ImageIcon))
        {
            HICON requested = message == GuestAbi::StmSetIcon
                ? reinterpret_cast<HICON>(wParam)
                : reinterpret_cast<HICON>(lParam);
            MiniGdi::Surface decoded;
            if (requested && !CopyGuestIconPixels(requested, &decoded))
            {
                return 0;
            }
            HICON previous = nullptr;
            {
                std::lock_guard<std::mutex> guard(window->lock);
                if (window->destroyed) return 0;
                previous = window->staticIcon;
                window->staticIcon = requested;
                window->staticImage = std::move(decoded);
            }
            invalidate();
            return reinterpret_cast<LRESULT>(previous);
        }
        if (message == GuestAbi::StmGetIcon ||
            (message == GuestAbi::StmGetImage && wParam == GuestAbi::ImageIcon))
        {
            std::lock_guard<std::mutex> guard(window->lock);
            return reinterpret_cast<LRESULT>(window->staticIcon);
        }
        if (message == GuestAbi::WmSetFocus || message == GuestAbi::WmKillFocus || message == GuestAbi::WmEnable)
        {
            invalidate();
        }
        return 0;

    case BuiltinControlKind::UpDown:
        if (message == GuestAbi::WmLButtonUp ||
            (message == GuestAbi::WmKeyDown &&
                (wParam == GuestAbi::VkUp || wParam == GuestAbi::VkDown ||
                    wParam == GuestAbi::VkLeft || wParam == GuestAbi::VkRight)))
        {
            HWND parent = nullptr;
            HWND handle = nullptr;
            HWND buddy = nullptr;
            UINT_PTR controlId = 0;
            int position = 0;
            int delta = 0;
            UINT numberBase = 10;
            {
                std::lock_guard<std::mutex> guard(window->lock);
                if (window->destroyed || !window->enabled) return 0;
                const bool horizontal = (window->style & 0x0040u) != 0;
                if (message == GuestAbi::WmKeyDown)
                {
                    delta = wParam == GuestAbi::VkUp || wParam == GuestAbi::VkRight ? 1 : -1;
                }
                else if (horizontal)
                {
                    const int x = static_cast<int>(static_cast<SHORT>(LOWORD(lParam)));
                    delta = x >= window->surface.Width() / 2 ? 1 : -1;
                }
                else
                {
                    const int y = static_cast<int>(static_cast<SHORT>(HIWORD(lParam)));
                    delta = y < window->surface.Height() / 2 ? 1 : -1;
                }
                parent = window->parent;
                handle = window->handle;
                buddy = window->upDownBuddy;
                controlId = window->controlId;
                position = window->upDownPosition;
                numberBase = window->upDownBase;
            }
            GuestUpDownNotification notification = {};
            notification.header = GuestNotifyHeader{ handle, controlId, UpDownNotifyDeltaPosition };
            notification.position = position;
            notification.delta = delta;
            if (parent && SendGuestMessage(parent, GuestAbi::WmNotify,
                static_cast<WPARAM>(controlId), reinterpret_cast<LPARAM>(&notification), nullptr) != 0)
                return 0;
            int current = position;
            {
                std::lock_guard<std::mutex> guard(window->lock);
                if (window->destroyed) return 0;
                const int low = (std::min)(window->upDownMinimum, window->upDownMaximum);
                const int high = (std::max)(window->upDownMinimum, window->upDownMaximum);
                const std::int64_t requested = static_cast<std::int64_t>(window->upDownPosition) + delta;
                current = static_cast<int>((std::max)(static_cast<std::int64_t>(low),
                    (std::min)(static_cast<std::int64_t>(high), requested)));
                window->upDownPosition = current;
            }
            if (buddy)
            {
                wchar_t buffer[40] = {};
                if (numberBase == 16) swprintf_s(buffer, L"%X", static_cast<unsigned int>(current));
                else swprintf_s(buffer, L"%d", current);
                SendGuestMessage(buddy, GuestAbi::WmSetText, 0,
                    reinterpret_cast<LPARAM>(buffer), nullptr);
            }
            SetGuestFocus(handle, nullptr);
            invalidate();
            return 0;
        }
        return 0;

    case BuiltinControlKind::None:
    default:
        return 0;
    }
}

LRESULT GuestWindowManager::DefaultGuestWindowProcedure(HWND window, UINT message, WPARAM wParam, LPARAM lParam)
{
    const auto record = FindWindow(window);
    if (record)
    {
        BuiltinControlKind builtinKind = BuiltinControlKind::None;
        {
            std::lock_guard<std::mutex> guard(record->lock);
            if (record->windowClass)
            {
                builtinKind = record->windowClass->builtinKind;
            }
        }
        if (builtinKind != BuiltinControlKind::None)
        {
            // This path matters when a PE subclasses a stock control through
            // GWLP_WNDPROC and then calls DefWindowProcW for the behavior it
            // did not override.
            return BuiltinControlProcedure(record, message, wParam, lParam);
        }
    }

    switch (message)
    {
    case GuestAbi::WmNcCreate:
        return TRUE;
    case GuestAbi::WmNcHitTest:
        return GuestAbi::HtClient;
    case GuestAbi::WmWindowPosChanged:
    {
        GuestWindowPosition position{};
        if (!TryReadGuestValue(
            reinterpret_cast<const GuestWindowPosition*>(lParam), &position))
        {
            return 0;
        }
        if ((position.flags & GuestSwpNoMove) == 0)
        {
            CallWindowProcedure(record, GuestAbi::WmMove, 0,
                GuestAbi::MakeMouseLParam(
                    SignedCoordinateWord(position.x),
                    SignedCoordinateWord(position.y)));
        }
        if ((position.flags & GuestSwpNoSize) == 0)
        {
            int clientWidth = position.cx;
            int clientHeight = position.cy;
            {
                std::lock_guard<std::mutex> guard(record->lock);
                const GuestMetrics::NonClientMetrics nonClient =
                    GuestMetrics::NonClientForEmbeddedWindow(
                        record->style, record->extendedStyle,
                        record->parent != nullptr);
                clientWidth = (std::max)(0, clientWidth -
                    nonClient.left - nonClient.right);
                clientHeight = (std::max)(0, clientHeight -
                    nonClient.top - nonClient.bottom -
                    ((!record->parent && record->menuBar) ?
                        GuestMetrics::MenuHeight : 0));
            }
            CallWindowProcedure(record, GuestAbi::WmSize, GuestAbi::SizeRestored,
                GuestAbi::MakeMouseLParam(
                    static_cast<WORD>((std::min)(clientWidth, 0xffff)),
                    static_cast<WORD>((std::min)(clientHeight, 0xffff))));
        }
        return 0;
    }
    case GuestAbi::WmEraseBkgnd:
        // BeginPaint performs the class-brush fallback after the guest gets a
        // chance to handle WM_ERASEBKGND. Returning zero here preserves that
        // normal DefWindowProc negotiation without pretending a background
        // was painted when it was not.
        return 0;
    case GuestAbi::WmSysCommand:
        if ((wParam & GuestAbi::ScMask) == GuestAbi::ScClose)
            return CallWindowProcedure(record, GuestAbi::WmClose, 0, 0);
        return 0;
    case GuestAbi::WmClose:
        DestroyGuestWindow(window, nullptr);
        return 0;
    default:
        return 0;
    }
}

BOOL GuestWindowManager::PostGuestMessage(HWND window, UINT message, WPARAM wParam, LPARAM lParam, DWORD* win32Error)
{
    if (window && !IsGuestWindow(window))
    {
        SetWin32Error(win32Error, ERROR_INVALID_WINDOW_HANDLE);
        return FALSE;
    }
    if (!m_messages.Post(window, message, wParam, lParam))
    {
        SetWin32Error(win32Error, ERROR_NOT_ENOUGH_QUOTA);
        return FALSE;
    }
    SetWin32Error(win32Error, ERROR_SUCCESS);
    return TRUE;
}

bool GuestWindowManager::PostGuestTimerMessage(HWND window, UINT_PTR timerId)
{
    if (!m_active.load())
    {
        return false;
    }

    if (window)
    {
        const auto record = FindWindow(window);
        if (!record)
        {
            return false;
        }
        std::lock_guard<std::mutex> guard(record->lock);
        if (record->destroyed)
        {
            return false;
        }
    }

    return m_messages.PostTimer(window, timerId);
}

LRESULT GuestWindowManager::SendGuestMessage(HWND window, UINT message, WPARAM wParam, LPARAM lParam, DWORD* win32Error)
{
    const auto record = FindWindow(window);
    if (!record)
    {
        SetWin32Error(win32Error, ERROR_INVALID_WINDOW_HANDLE);
        return 0;
    }
    SetWin32Error(win32Error, ERROR_SUCCESS);
    return CallWindowProcedure(record, message, wParam, lParam);
}

void GuestWindowManager::PostGuestQuitMessage(int exitCode)
{
    m_messages.PostQuit(exitCode);
}

GuestGetMessageResult GuestWindowManager::GetGuestMessage(GuestAbi::Message* message, HWND filter, UINT minimum, UINT maximum)
{
    return m_messages.Get(message, filter, minimum, maximum);
}

BOOL GuestWindowManager::PeekGuestMessage(GuestAbi::Message* message, HWND filter, UINT minimum, UINT maximum, UINT removeFlags)
{
    return m_messages.Peek(message, filter, minimum, maximum, removeFlags) ? TRUE : FALSE;
}

BOOL GuestWindowManager::TranslateGuestMessage(const GuestAbi::Message* message)
{
    if (!message)
    {
        return FALSE;
    }

    // CoreWindow::CharacterReceived already applies the active keyboard
    // layout, Shift/CapsLock state, dead keys and Unicode composition. A
    // second VK-to-ASCII conversion here would lose that information and
    // enqueue duplicate WM_CHAR messages.
    return message->message == GuestAbi::WmKeyDown && message->hwnd ? TRUE : FALSE;
}

LRESULT GuestWindowManager::DispatchGuestMessage(const GuestAbi::Message* message, DWORD* win32Error)
{
    if (!message)
    {
        SetWin32Error(win32Error, ERROR_INVALID_PARAMETER);
        return 0;
    }
    if (message->message == GuestAbi::WmQuit || !message->hwnd)
    {
        SetWin32Error(win32Error, ERROR_SUCCESS);
        return 0;
    }

    // UpdateWindow can synchronously consume the invalid region while the
    // coalesced WM_PAINT remains in the posted queue. Do not turn that stale
    // notification into an extra guest paint.
    if (message->message == GuestAbi::WmPaint)
    {
        const auto record = FindWindow(message->hwnd);
        if (!record)
        {
            SetWin32Error(win32Error, ERROR_INVALID_WINDOW_HANDLE);
            return 0;
        }
        std::lock_guard<std::mutex> guard(record->lock);
        record->paintPosted = false;
        if (!record->invalidated)
        {
            SetWin32Error(win32Error, ERROR_SUCCESS);
            return 0;
        }
        RuntimeDiagnostics::Record(
            L"PAINT DISPATCH: handle " +
            std::to_wstring(reinterpret_cast<ULONG_PTR>(message->hwnd)) + L".");
    }
    try
    {
        return SendGuestMessage(message->hwnd, message->message, message->wParam, message->lParam, win32Error);
    }
    catch (Exception^ error)
    {
        SetWin32Error(win32Error, ERROR_GEN_FAILURE);
        RuntimeDiagnostics::Record(
            L"WINDOW EXCEPTION: dispatch of message " + std::to_wstring(message->message) +
            L" raised HRESULT " + std::to_wstring(static_cast<unsigned long>(error->HResult)) + L".");
        return 0;
    }
    catch (...)
    {
        SetWin32Error(win32Error, ERROR_GEN_FAILURE);
        RuntimeDiagnostics::Record(
            L"WINDOW EXCEPTION: dispatch of message " + std::to_wstring(message->message) +
            L" raised an unknown exception.");
        return 0;
    }
}

bool GuestWindowManager::EraseGuestBackground(
    const std::shared_ptr<WindowRecord>& window,
    HDC dc,
    const RECT& rect)
{
    if (!window || !HasArea(rect))
    {
        return false;
    }

    HBRUSH background = nullptr;
    {
        std::lock_guard<std::mutex> guard(window->lock);
        if (window->destroyed || !window->windowClass)
        {
            return false;
        }
        background = window->windowClass->background;
    }
    if (!background)
    {
        return false;
    }

    const MiniGdi::DcHandle guestDc = FromGuestDc(dc);
    if (guestDc == MiniGdi::InvalidDc || !m_gdi.HasDc(guestDc))
    {
        return false;
    }

    const ULONG_PTR rawBackground = reinterpret_cast<ULONG_PTR>(background);
    MiniGdi::ObjectHandle brush = MiniGdi::InvalidObject;
    bool temporaryBrush = false;
    if (rawBackground <= MaximumSystemColorBrush)
    {
        brush = m_gdi.CreateSolidBrush(SystemColorBrush(rawBackground));
        temporaryBrush = true;
    }
    else
    {
        MiniGdi::ObjectHandle candidate = MiniGdi::InvalidObject;
        MiniGdi::ObjectKind kind;
        if (!ToMiniGdiObject(reinterpret_cast<HGDIOBJ>(background), &candidate) ||
            !m_gdi.ObjectType(candidate, &kind) || kind != MiniGdi::ObjectKind::Brush)
        {
            return false;
        }
        brush = candidate;
    }
    if (brush == MiniGdi::InvalidObject)
    {
        return false;
    }

    MiniGdi::ObjectHandle previous = MiniGdi::InvalidObject;
    if (!m_gdi.SelectBrush(guestDc, brush, &previous))
    {
        if (temporaryBrush)
        {
            m_gdi.DeleteObject(brush);
        }
        return false;
    }

    const bool filled = m_gdi.FillRect(
        guestDc,
        MiniGdi::Rect{ rect.left, rect.top, rect.right, rect.bottom });
    m_gdi.SelectBrush(guestDc, previous, nullptr);
    if (temporaryBrush)
    {
        m_gdi.DeleteObject(brush);
    }
    return filled;
}

void GuestWindowManager::PostPaint(const std::shared_ptr<WindowRecord>& window, const RECT* rect, BOOL erase)
{
    if (!window)
    {
        return;
    }

    bool post = false;
    {
        std::lock_guard<std::mutex> guard(window->lock);
        if (window->destroyed)
        {
            return;
        }

        const RECT clipped = ClipToSurface(rect, window->surface);
        if (!HasArea(clipped))
        {
            return;
        }

        if (!window->invalidated)
        {
            window->invalidated = true;
            window->updateRect = clipped;
        }
        else
        {
            UnionDamage(&window->updateRect, clipped);
        }
        window->erasePending = window->erasePending || erase != FALSE;
        if (!window->paintPosted)
        {
            window->paintPosted = true;
            post = true;
        }
    }
    if (post && !PostGuestMessage(window->handle, GuestAbi::WmPaint, 0, 0, nullptr))
    {
        // A bounded queue can reject a paint while the region remains dirty.
        // Do not strand that region behind a phantom "posted" bit: a later
        // invalidation or UpdateWindow gets another chance to schedule it.
        std::lock_guard<std::mutex> guard(window->lock);
        window->paintPosted = false;
    }
}

BOOL GuestWindowManager::InvalidateGuestRect(HWND window, const RECT* rect, BOOL erase, DWORD* win32Error)
{
    if (window)
    {
        const auto record = FindWindow(window);
        if (!record)
        {
            SetWin32Error(win32Error, ERROR_INVALID_WINDOW_HANDLE);
            return FALSE;
        }
        PostPaint(record, rect, erase);
        SetWin32Error(win32Error, ERROR_SUCCESS);
        return TRUE;
    }

    // InvalidateRect(nullptr, ...) means all windows owned by this guest UI
    // thread, not just whichever top-level surface last held the foreground.
    std::vector<std::shared_ptr<WindowRecord>> windows;
    {
        std::lock_guard<std::mutex> guard(m_windowsLock);
        windows.reserve(m_windows.size());
        for (const auto& item : m_windows)
        {
            windows.push_back(item.second);
        }
    }
    for (const auto& record : windows)
    {
        PostPaint(record, rect, erase);
    }

    SetWin32Error(win32Error, ERROR_SUCCESS);
    return TRUE;
}

BOOL GuestWindowManager::UpdateGuestWindow(HWND window, DWORD* win32Error)
{
    const auto record = FindWindow(window);
    if (!record)
    {
        SetWin32Error(win32Error, ERROR_INVALID_WINDOW_HANDLE);
        return FALSE;
    }

    bool needsPaint = false;
    {
        std::lock_guard<std::mutex> guard(record->lock);
        needsPaint = record->invalidated;
    }
    if (needsPaint)
    {
        CallWindowProcedure(record, GuestAbi::WmPaint, 0, 0);
    }
    SetWin32Error(win32Error, ERROR_SUCCESS);
    return TRUE;
}

HDC GuestWindowManager::BeginGuestPaint(HWND window, GuestAbi::PaintStruct* paint, DWORD* win32Error)
{
    const auto record = FindWindow(window);
    if (!record || !paint)
    {
        SetWin32Error(win32Error, !paint ? ERROR_INVALID_PARAMETER : ERROR_INVALID_WINDOW_HANDLE);
        return nullptr;
    }

    HDC dc = nullptr;
    RECT update = {};
    bool erase = false;
    {
        std::lock_guard<std::mutex> guard(record->lock);
        if (record->destroyed)
        {
            SetWin32Error(win32Error, ERROR_INVALID_WINDOW_HANDLE);
            return nullptr;
        }
        if (record->paintActive)
        {
            SetWin32Error(win32Error, ERROR_BUSY);
            return nullptr;
        }

        dc = ToGuestDc(record->dc);
        if (record->invalidated)
        {
            update = record->updateRect;
            erase = record->erasePending;
        }
        record->invalidated = false;
        record->erasePending = false;
        record->updateRect = RECT{};
        record->paintActive = true;
    }

    memset(paint, 0, sizeof(*paint));
    paint->hdc = dc;
    paint->fErase = erase ? TRUE : FALSE;
    paint->rcPaint = update;

    if (erase && HasArea(update))
    {
        const LRESULT handled = CallWindowProcedure(
            record,
            GuestAbi::WmEraseBkgnd,
            reinterpret_cast<WPARAM>(dc),
            0);

        // WM_ERASEBKGND is guest code and can re-enter the bridge. It may end
        // the paint, resize the target, or destroy the HWND altogether. Never
        // return a stale HDC after such a transition.
        bool stillPainting = false;
        {
            std::lock_guard<std::mutex> guard(record->lock);
            stillPainting = !record->destroyed && record->paintActive &&
                record->dc == FromGuestDc(dc) && m_gdi.HasDc(record->dc);
        }
        if (!stillPainting)
        {
            SetWin32Error(win32Error, ERROR_INVALID_HANDLE);
            return nullptr;
        }
        if (handled == 0)
        {
            EraseGuestBackground(record, dc, update);
        }
    }

    SetWin32Error(win32Error, ERROR_SUCCESS);
    return dc;
}

BOOL GuestWindowManager::EndGuestPaint(HWND window, const GuestAbi::PaintStruct* paint, DWORD* win32Error)
{
    const auto record = FindWindow(window);
    if (!record || !paint)
    {
        SetWin32Error(win32Error, !paint ? ERROR_INVALID_PARAMETER : ERROR_INVALID_WINDOW_HANDLE);
        return FALSE;
    }

    {
        std::lock_guard<std::mutex> guard(record->lock);
        if (record->destroyed || FromGuestDc(paint->hdc) != record->dc || !record->paintActive)
        {
            SetWin32Error(win32Error, ERROR_INVALID_HANDLE);
            return FALSE;
        }
        record->paintActive = false;
    }
    if (IsGuestWindowVisibleInternal(window))
    {
        Present(record);
        RuntimeDiagnostics::Record(
            L"PAINT PRESENTED: handle " +
            std::to_wstring(reinterpret_cast<ULONG_PTR>(window)) + L".");
    }
    SetWin32Error(win32Error, ERROR_SUCCESS);
    return TRUE;
}

HDC GuestWindowManager::GetGuestDC(HWND window, DWORD* win32Error)
{
    if (!window)
    {
        std::lock_guard<std::mutex> guard(m_screenDcLock);
        if (m_screenDc == MiniGdi::InvalidDc || !m_gdi.HasDc(m_screenDc))
        {
            const int width = (std::max)(1, m_viewportWidth.load());
            const int height = (std::max)(1, m_viewportHeight.load());
            if (!m_screenSurface.Resize(width, height, MiniGdi::OpaqueWhite))
            {
                SetWin32Error(win32Error, ERROR_NOT_ENOUGH_MEMORY);
                return nullptr;
            }
            m_screenDc = m_gdi.CreateDc(&m_screenSurface);
            if (m_screenDc == MiniGdi::InvalidDc)
            {
                SetWin32Error(win32Error, ERROR_NOT_ENOUGH_MEMORY);
                return nullptr;
            }
            RuntimeDiagnostics::Record(L"GDI: created virtual screen DC " +
                std::to_wstring(static_cast<ULONG_PTR>(m_screenDc)) + L".");
        }
        SetWin32Error(win32Error, ERROR_SUCCESS);
        return ToGuestDc(m_screenDc);
    }

    const auto record = FindWindow(window);
    if (!record)
    {
        SetWin32Error(win32Error, ERROR_INVALID_WINDOW_HANDLE);
        return nullptr;
    }
    SetWin32Error(win32Error, ERROR_SUCCESS);
    return ToGuestDc(record->dc);
}

int GuestWindowManager::ReleaseGuestDC(HWND window, HDC dc, DWORD* win32Error)
{
    if (!window)
    {
        std::lock_guard<std::mutex> guard(m_screenDcLock);
        if (m_screenDc == MiniGdi::InvalidDc || FromGuestDc(dc) != m_screenDc ||
            !m_gdi.HasDc(m_screenDc))
        {
            SetWin32Error(win32Error, ERROR_INVALID_HANDLE);
            return 0;
        }
        // Win32 ReleaseDC does not destroy the common screen DC. It merely
        // releases the caller's use of it, so keep the virtual handle stable.
        SetWin32Error(win32Error, ERROR_SUCCESS);
        return 1;
    }

    const auto record = FindWindow(window);
    if (!record || FromGuestDc(dc) != record->dc)
    {
        SetWin32Error(win32Error, ERROR_INVALID_HANDLE);
        return 0;
    }
    if (IsGuestWindowVisibleInternal(window))
    {
        Present(record);
    }
    SetWin32Error(win32Error, ERROR_SUCCESS);
    return 1;
}

bool GuestWindowManager::IsGuestDc(HDC dc) const
{
    const MiniGdi::DcHandle handle = FromGuestDc(dc);
    return handle != MiniGdi::InvalidDc && m_gdi.HasDc(handle);
}

MiniGdi::DcHandle GuestWindowManager::GuestDcHandle(HDC dc) const
{
    const MiniGdi::DcHandle handle = FromGuestDc(dc);
    return m_gdi.HasDc(handle) ? handle : MiniGdi::InvalidDc;
}

void GuestWindowManager::Present(const std::shared_ptr<WindowRecord>& window)
{
    const auto presentation = m_presentation;
    if (!window || !presentation)
    {
        return;
    }

    try
    {
        {
            std::lock_guard<std::mutex> guard(presentation->lock);
            if (!presentation->active)
            {
                return;
            }
        }

        HWND requested = nullptr;
        {
            std::lock_guard<std::mutex> guard(window->lock);
            if (window->destroyed)
            {
                return;
            }
            requested = window->handle;
        }
        if (!requested)
        {
            return;
        }

        // One XAML Image represents the visible desktop of this guest. Build
        // that frame from the requested top-level window and its visible child
        // tree instead of replacing the whole image with a child HDC's surface.
        // This is intentionally software composition: all source pixels remain
        // bridge-owned and the UWP host only receives the final bitmap.
        struct WindowSnapshot final
        {
            HWND handle = nullptr;
            HWND parent = nullptr;
            RECT bounds = {};
            bool visible = false;
            int compositionHeight = 0;
            bool topMost = false;
            std::wstring title;
            DWORD style = 0;
            DWORD extendedStyle = 0;
            GuestMetrics::NonClientMetrics nonClient;
            std::shared_ptr<WindowRecord> record;
        };

        std::vector<std::shared_ptr<WindowRecord>> records;
        {
            std::lock_guard<std::mutex> guard(m_windowsLock);
            records.reserve(m_windows.size());
            for (const auto& item : m_windows)
            {
                records.push_back(item.second);
            }
        }

        std::vector<WindowSnapshot> snapshots;
        snapshots.reserve(records.size());
        for (const auto& candidate : records)
        {
            WindowSnapshot snapshot = {};
            {
                std::lock_guard<std::mutex> guard(candidate->lock);
                if (candidate->destroyed)
                {
                    continue;
                }
                snapshot.handle = candidate->handle;
                snapshot.parent = candidate->parent;
                snapshot.bounds = candidate->bounds;
                snapshot.visible = candidate->visible;
                snapshot.compositionHeight = candidate->surface.Height();
                snapshot.title = candidate->title;
                snapshot.style = candidate->style;
                snapshot.extendedStyle = candidate->extendedStyle;
                snapshot.nonClient = GuestMetrics::NonClientForEmbeddedWindow(
                    candidate->style, candidate->extendedStyle,
                    candidate->parent != nullptr);
                snapshot.topMost = candidate->windowClass &&
                    candidate->windowClass->builtinKind == BuiltinControlKind::ComboBox &&
                    candidate->comboDropped;
                if (candidate->windowClass &&
                    candidate->windowClass->builtinKind == BuiltinControlKind::ComboBox &&
                    !candidate->comboDropped &&
                    (candidate->style & 0x0003u) != ComboBoxStyleSimple)
                {
                    snapshot.compositionHeight = (std::min)(snapshot.compositionHeight,
                        GuestMetrics::ControlHeightForText(GuestMetrics::TextHeight));
                }
                snapshot.record = candidate;
            }
            snapshots.push_back(std::move(snapshot));
        }
        if (snapshots.empty())
        {
            return;
        }

        // Handles are monotonic. Sorting gives newly-created sibling windows a
        // deterministic front-to-back order (newer children are drawn later),
        // until a dedicated SetWindowPos z-order table is introduced.
        std::sort(
            snapshots.begin(),
            snapshots.end(),
            [](const WindowSnapshot& left, const WindowSnapshot& right)
            {
                if (left.topMost != right.topMost) return !left.topMost;
                return reinterpret_cast<ULONG_PTR>(left.handle) <
                    reinterpret_cast<ULONG_PTR>(right.handle);
            });

        std::unordered_map<ULONG_PTR, size_t> indexByHandle;
        indexByHandle.reserve(snapshots.size());
        for (size_t index = 0; index < snapshots.size(); ++index)
        {
            indexByHandle.emplace(reinterpret_cast<ULONG_PTR>(snapshots[index].handle), index);
        }

        const size_t noWindow = (std::numeric_limits<size_t>::max)();
        const auto rootFor = [&snapshots, &indexByHandle, noWindow](HWND start) -> size_t
        {
            const auto initial = indexByHandle.find(reinterpret_cast<ULONG_PTR>(start));
            if (initial == indexByHandle.end())
            {
                return noWindow;
            }

            std::unordered_set<ULONG_PTR> visited;
            size_t current = initial->second;
            for (;;)
            {
                const HWND handle = snapshots[current].handle;
                if (!visited.emplace(reinterpret_cast<ULONG_PTR>(handle)).second)
                {
                    return noWindow;
                }

                const HWND parent = snapshots[current].parent;
                if (!parent)
                {
                    return current;
                }
                const auto next = indexByHandle.find(reinterpret_cast<ULONG_PTR>(parent));
                if (next == indexByHandle.end())
                {
                    return noWindow;
                }
                current = next->second;
            }
        };

        const size_t root = rootFor(requested);
        if (root == noWindow || !snapshots[root].visible)
        {
            return;
        }

        const HWND foreground = reinterpret_cast<HWND>(m_foregroundWindow.load());
        if (foreground)
        {
            const size_t foregroundRoot = rootFor(foreground);
            if (foregroundRoot != noWindow && foregroundRoot != root)
            {
                // A paint in a background top-level window updates its own
                // virtual surface, but must not steal the host presentation.
                return;
            }
        }

        MiniGdi::Surface composite;
        {
            const auto rootRecord = snapshots[root].record;
            std::lock_guard<std::mutex> guard(rootRecord->lock);
            if (rootRecord->destroyed || !rootRecord->visible ||
                !composite.Resize(rootRecord->surface.Width(), rootRecord->surface.Height(), MiniGdi::Transparent))
            {
                return;
            }
            composite.Pixels() = rootRecord->surface.Pixels();
        }
        if (composite.Empty() || composite.Pixels().empty())
        {
            return;
        }

        // USER32 owns the virtual HMENU hierarchy.  Render a compact
        // non-client menu strip here, after the guest's own top-level surface
        // and before any child controls. This keeps menu pixels and hit input
        // in the guest compositor rather than introducing a host XAML menu.
        const std::vector<GuestMenuVisualItem> menuItems =
            GetGuestMenuBarItems(snapshots[root].handle);
        const int menuHeight = menuItems.empty() ? 0 :
            (std::min)(GuestMetrics::MenuHeight, composite.Height());
        int openMenuIndex = -1;
        if (menuHeight > 0)
        {
            std::lock_guard<std::mutex> guard(snapshots[root].record->lock);
            openMenuIndex = snapshots[root].record->openMenuIndex;
        }
        if (menuHeight > 0)
        {
            MiniGdi::FillRect(
                composite,
                MiniGdi::Rect{ 0, 0, composite.Width(), menuHeight },
                MiniGdi::MakeColor(240, 240, 240));
            MiniGdi::DrawLine(
                composite,
                MiniGdi::Point{ 0, menuHeight - 1 },
                MiniGdi::Point{ (std::max)(0, composite.Width() - 1), menuHeight - 1 },
                MiniGdi::MakeColor(160, 160, 160));

            const MiniGdi::DcHandle menuDc = m_gdi.CreateDc(&composite);
            if (menuDc != MiniGdi::InvalidDc)
            {
                MiniGdi::Color ignored = MiniGdi::OpaqueBlack;
                m_gdi.SetTextColor(menuDc, MiniGdi::OpaqueBlack, &ignored);
                MiniGdi::BackgroundMode ignoredMode = MiniGdi::BackgroundMode::Opaque;
                m_gdi.SetBackgroundMode(menuDc, MiniGdi::BackgroundMode::Transparent, &ignoredMode);
                int left = 8;
                for (const auto& item : menuItems)
                {
                    const std::wstring caption = MenuCaptionForDisplay(item.text);
                    const int itemWidth = MenuBarItemWidth(item);
                    if (left >= composite.Width())
                    {
                        break;
                    }
                    if (static_cast<int>(&item - menuItems.data()) == openMenuIndex ||
                        (item.state & MenuFlagHighlighted) != 0)
                    {
                        MiniGdi::FillRect(composite,
                            MiniGdi::Rect{ left, 1, (std::min)(composite.Width(), SaturatingAdd(left, itemWidth)), menuHeight - 1 },
                            MiniGdi::MakeColor(214, 226, 242));
                    }
                    m_gdi.SetTextColor(menuDc, IsMenuItemDisabled(item)
                        ? MiniGdi::MakeColor(144, 144, 144) : MiniGdi::OpaqueBlack, nullptr);
                    m_gdi.TextOutW(menuDc,
                        MiniGdi::Point{ left + 6, GuestMetrics::MenuTextTop },
                        caption.data(), caption.size(), nullptr);
                    const int mnemonic = MenuMnemonicDisplayIndex(item.text);
                    if (mnemonic >= 0)
                    {
                        const int underlineLeft = left + 6 + mnemonic * MiniGdi::DefaultTextGlyphWidth;
                        MiniGdi::DrawLine(composite,
                            MiniGdi::Point{ underlineLeft,
                                GuestMetrics::MenuTextTop + MiniGdi::DefaultTextGlyphHeight - 2 },
                            MiniGdi::Point{ underlineLeft + MiniGdi::DefaultTextGlyphWidth - 2,
                                GuestMetrics::MenuTextTop + MiniGdi::DefaultTextGlyphHeight - 2 },
                            IsMenuItemDisabled(item) ? MiniGdi::MakeColor(144, 144, 144) : MiniGdi::OpaqueBlack);
                    }
                    left = SaturatingAdd(left, itemWidth);
                }
                m_gdi.DestroyDc(menuDc);
            }
        }

        std::unordered_set<ULONG_PTR> composed;
        composed.emplace(reinterpret_cast<ULONG_PTR>(snapshots[root].handle));
        std::function<void(size_t, int, int)> composeChildren;
        composeChildren = [
            &snapshots,
            &composite,
            &composed,
            &composeChildren,
            this,
            root,
            menuHeight](size_t parentIndex, int parentX, int parentY)
        {
            const HWND parent = snapshots[parentIndex].handle;
            for (size_t childIndex = 0; childIndex < snapshots.size(); ++childIndex)
            {
                const WindowSnapshot& child = snapshots[childIndex];
                if (!child.visible || child.parent != parent ||
                    !composed.emplace(reinterpret_cast<ULONG_PTR>(child.handle)).second)
                {
                    continue;
                }

                const WindowSnapshot& parentSnapshot = snapshots[parentIndex];
                const int childX = SaturatingAdd(
                    SaturatingAdd(parentX, parentSnapshot.nonClient.left),
                    child.bounds.left);
                const int childY = SaturatingAdd(
                    SaturatingAdd(
                        SaturatingAdd(parentY, parentSnapshot.nonClient.top),
                        child.bounds.top),
                    parentIndex == root ? menuHeight : 0);
                bool copied = false;
                {
                    std::lock_guard<std::mutex> guard(child.record->lock);
                    // Parentage and visibility can change re-entrantly between
                    // the snapshot and this copy. Skip that stale layer rather
                    // than compositing it into an unrelated root.
                    if (!child.record->destroyed && child.record->visible &&
                        child.record->parent == parent)
                    {
                        copied = MiniGdi::CopyRect(
                            composite,
                            MiniGdi::Point{ childX, childY },
                            child.record->surface,
                            MiniGdi::Rect{ 0, 0, child.record->surface.Width(),
                                (std::min)(child.compositionHeight,
                                    child.record->surface.Height()) });
                    }
                }
                if (copied)
                {
                    if (child.nonClient.left > 0 || child.nonClient.top > 0)
                    {
                        const int childWidth = child.bounds.right - child.bounds.left;
                        const int childHeight = child.bounds.bottom - child.bounds.top;
                        const MiniGdi::Rect frameRect{ childX, childY,
                            SaturatingAdd(childX, childWidth),
                            SaturatingAdd(childY, childHeight) };
                        MiniGdi::DrawRectangle(composite, frameRect,
                            MiniGdi::Transparent, MiniGdi::MakeColor(92, 92, 92));
                        if (child.nonClient.caption)
                        {
                            const int captionLeft = SaturatingAdd(childX, child.nonClient.left);
                            const int captionTop = SaturatingAdd(childY,
                                child.nonClient.top - GuestMetrics::CaptionHeight);
                            const int captionRight = SaturatingAdd(childX,
                                childWidth - child.nonClient.right);
                            const int captionBottom = SaturatingAdd(captionTop,
                                GuestMetrics::CaptionHeight);
                            MiniGdi::FillRect(composite,
                                MiniGdi::Rect{ captionLeft, captionTop,
                                    captionRight, captionBottom },
                                MiniGdi::MakeColor(48, 96, 160));
                            const MiniGdi::DcHandle captionDc = m_gdi.CreateDc(&composite);
                            if (captionDc != MiniGdi::InvalidDc)
                            {
                                m_gdi.SetTextColor(captionDc, MiniGdi::OpaqueWhite, nullptr);
                                m_gdi.SetBackgroundMode(captionDc,
                                    MiniGdi::BackgroundMode::Transparent, nullptr);
                                const int closeExtent = child.nonClient.closeButton
                                    ? GuestMetrics::CaptionHeight : 0;
                                const size_t availableCharacters = static_cast<size_t>((std::max)(
                                    0, captionRight - captionLeft - closeExtent - 10) /
                                    MiniGdi::DefaultTextGlyphWidth);
                                const size_t titleLength = (std::min)(
                                    child.title.size(), availableCharacters);
                                m_gdi.TextOutW(captionDc,
                                    MiniGdi::Point{ captionLeft + 6,
                                        captionTop + (GuestMetrics::CaptionHeight -
                                            MiniGdi::DefaultTextGlyphHeight) / 2 },
                                    child.title.data(), titleLength, nullptr);
                                m_gdi.DestroyDc(captionDc);
                            }
                            if (child.nonClient.closeButton)
                            {
                                const int buttonLeft = captionRight -
                                    GuestMetrics::CaptionHeight;
                                MiniGdi::DrawLine(composite,
                                    MiniGdi::Point{ buttonLeft + 7, captionTop + 7 },
                                    MiniGdi::Point{ captionRight - 7, captionBottom - 7 },
                                    MiniGdi::OpaqueWhite);
                                MiniGdi::DrawLine(composite,
                                    MiniGdi::Point{ captionRight - 7, captionTop + 7 },
                                    MiniGdi::Point{ buttonLeft + 7, captionBottom - 7 },
                                    MiniGdi::OpaqueWhite);
                            }
                        }
                    }
                    composeChildren(childIndex, childX, childY);
                }
            }
        };
        composeChildren(root, 0, 0);

        PopupMenuSession activePopup;
        {
            std::lock_guard<std::mutex> guard(m_popupMenuLock);
            activePopup = m_popupMenu;
        }
        if (activePopup.open && activePopup.root == snapshots[root].handle)
        {
            for (const PopupMenuLevel& level : activePopup.levels)
            {
                const std::vector<GuestMenuVisualItem> popupItems = GetGuestMenuItems(level.menu);
                const int popupWidth = PopupMenuWidth(popupItems);
                const int popupHeight = (std::min)(PopupMenuHeight(popupItems),
                    (std::max)(0, composite.Height() - level.top));
                const int popupRight = (std::min)(composite.Width(), SaturatingAdd(level.left, popupWidth));
                if (popupHeight <= 0 || popupRight <= level.left) continue;
                MiniGdi::DrawRectangle(composite,
                    MiniGdi::Rect{ level.left, level.top, popupRight,
                        SaturatingAdd(level.top, popupHeight) },
                    MiniGdi::MakeColor(250, 250, 250), MiniGdi::MakeColor(72, 72, 72));
                const MiniGdi::DcHandle popupDc = m_gdi.CreateDc(&composite);
                if (popupDc != MiniGdi::InvalidDc)
                {
                    MiniGdi::Color ignored = MiniGdi::OpaqueBlack;
                    m_gdi.SetTextColor(popupDc, MiniGdi::OpaqueBlack, &ignored);
                    MiniGdi::BackgroundMode ignoredMode = MiniGdi::BackgroundMode::Opaque;
                    m_gdi.SetBackgroundMode(popupDc, MiniGdi::BackgroundMode::Transparent, &ignoredMode);
                    int itemOffset = 0;
                    for (size_t index = 0; index < popupItems.size() && itemOffset < popupHeight; ++index)
                    {
                        const GuestMenuVisualItem& item = popupItems[index];
                        const int rowHeight = PopupMenuItemHeight(item);
                        const int top = SaturatingAdd(level.top, itemOffset);
                        if ((static_cast<int>(index) == level.hotItem ||
                            (item.state & MenuFlagHighlighted) != 0) &&
                            !IsMenuItemSeparator(item) && !IsMenuItemDisabled(item))
                        {
                            MiniGdi::FillRect(composite,
                                MiniGdi::Rect{ level.left + 1, top + 1, popupRight - 1, top + rowHeight - 1 },
                                MiniGdi::MakeColor(214, 226, 242));
                        }
                        if (IsMenuItemSeparator(item))
                        {
                            MiniGdi::DrawLine(composite, MiniGdi::Point{ level.left + 4, top + rowHeight / 2 },
                                MiniGdi::Point{ popupRight - 4, top + rowHeight / 2 }, MiniGdi::MakeColor(192, 192, 192));
                        }
                        else
                        {
                            m_gdi.SetTextColor(popupDc, IsMenuItemDisabled(item)
                                ? MiniGdi::MakeColor(144, 144, 144) : MiniGdi::OpaqueBlack, nullptr);
                            HBITMAP displayedBitmap = item.itemBitmap;
                            if (!displayedBitmap)
                                displayedBitmap = (item.state & MenuFlagChecked) != 0
                                    ? item.checkedBitmap : item.uncheckedBitmap;
                            const MiniGdi::Surface* bitmap = displayedBitmap
                                ? m_gdi.GetBitmapSurface(static_cast<MiniGdi::BitmapHandle>(
                                    reinterpret_cast<ULONG_PTR>(displayedBitmap))) : nullptr;
                            if (bitmap)
                            {
                                const int copyWidth = (std::min)(bitmap->Width(),
                                    GuestMetrics::MenuCheckColumnWidth - 4);
                                const int copyHeight = (std::min)(bitmap->Height(), rowHeight - 4);
                                MiniGdi::CopyRect(composite,
                                    MiniGdi::Point{ level.left + 2,
                                        top + (rowHeight - copyHeight) / 2 }, *bitmap,
                                    MiniGdi::Rect{ 0, 0, copyWidth, copyHeight });
                            }
                            else if ((item.state & MenuFlagChecked) != 0)
                            {
                                const MiniGdi::Color mark = IsMenuItemDisabled(item)
                                    ? MiniGdi::MakeColor(144, 144, 144) : MiniGdi::MakeColor(32, 32, 32);
                                if (item.type & MenuFlagRadioCheck)
                                {
                                    MiniGdi::FillRect(composite,
                                        MiniGdi::Rect{ level.left + 8, top + rowHeight / 2 - 2,
                                            level.left + 13, top + rowHeight / 2 + 3 }, mark);
                                }
                                else
                                {
                                    MiniGdi::DrawLine(composite,
                                        MiniGdi::Point{ level.left + 7, top + rowHeight / 2 },
                                        MiniGdi::Point{ level.left + 10, top + rowHeight / 2 + 3 }, mark);
                                    MiniGdi::DrawLine(composite,
                                        MiniGdi::Point{ level.left + 10, top + rowHeight / 2 + 3 },
                                        MiniGdi::Point{ level.left + 15, top + rowHeight / 2 - 4 }, mark);
                                }
                            }
                            std::wstring caption = MenuCaptionForDisplay(item.text);
                            std::wstring accelerator;
                            const size_t tab = caption.find(L'\t');
                            if (tab != std::wstring::npos)
                            {
                                accelerator = caption.substr(tab + 1);
                                caption.erase(tab);
                            }
                            m_gdi.TextOutW(popupDc,
                                MiniGdi::Point{ level.left + GuestMetrics::MenuCheckColumnWidth,
                                    top + GuestMetrics::MenuTextTop },
                                caption.data(), caption.size(), nullptr);
                            if (item.state & MenuFlagDefault)
                            {
                                m_gdi.TextOutW(popupDc,
                                    MiniGdi::Point{ level.left + GuestMetrics::MenuCheckColumnWidth + 1,
                                        top + GuestMetrics::MenuTextTop },
                                    caption.data(), caption.size(), nullptr);
                            }
                            const int mnemonic = MenuMnemonicDisplayIndex(item.text);
                            if (mnemonic >= 0)
                            {
                                const int underlineLeft = level.left + GuestMetrics::MenuCheckColumnWidth +
                                    mnemonic * MiniGdi::DefaultTextGlyphWidth;
                                const MiniGdi::Color underline = IsMenuItemDisabled(item)
                                    ? MiniGdi::MakeColor(144, 144, 144) : MiniGdi::OpaqueBlack;
                                MiniGdi::DrawLine(composite,
                                    MiniGdi::Point{ underlineLeft,
                                        top + GuestMetrics::MenuTextTop + MiniGdi::DefaultTextGlyphHeight - 2 },
                                    MiniGdi::Point{ underlineLeft + MiniGdi::DefaultTextGlyphWidth - 2,
                                        top + GuestMetrics::MenuTextTop + MiniGdi::DefaultTextGlyphHeight - 2 },
                                    underline);
                            }
                            if (!accelerator.empty())
                            {
                                const int acceleratorWidth = static_cast<int>((std::min)(
                                    accelerator.size(), static_cast<size_t>(40))) * MiniGdi::DefaultTextGlyphWidth;
                                m_gdi.TextOutW(popupDc,
                                    MiniGdi::Point{ (std::max)(
                                        level.left + GuestMetrics::MenuCheckColumnWidth,
                                        popupRight - acceleratorWidth -
                                            (item.subMenu ? GuestMetrics::MenuHeight : 8)),
                                        top + GuestMetrics::MenuTextTop },
                                    accelerator.data(), accelerator.size(), nullptr);
                            }
                            if (item.subMenu)
                            {
                                const MiniGdi::Color arrow = IsMenuItemDisabled(item)
                                    ? MiniGdi::MakeColor(144, 144, 144) : MiniGdi::MakeColor(48, 48, 48);
                                const int arrowX = popupRight - 10;
                                MiniGdi::DrawLine(composite, MiniGdi::Point{ arrowX - 2, top + 6 },
                                    MiniGdi::Point{ arrowX + 2, top + 10 }, arrow);
                                MiniGdi::DrawLine(composite, MiniGdi::Point{ arrowX + 2, top + 10 },
                                    MiniGdi::Point{ arrowX - 2, top + 14 }, arrow);
                            }
                        }
                        itemOffset = SaturatingAdd(itemOffset, rowHeight);
                    }
                    m_gdi.DestroyDc(popupDc);
                }
            }
        }

        // The host Image is the virtual desktop, not the currently active
        // top-level HWND. Keep it viewport-sized and place a smaller root at
        // its real desktop coordinates. Otherwise XAML stretches modal and
        // process-owned progress windows to fill the entire application.
        const int viewportWidth = (std::max)(1, m_viewportWidth.load());
        const int viewportHeight = (std::max)(1, m_viewportHeight.load());
        if (composite.Width() != viewportWidth || composite.Height() != viewportHeight)
        {
            MiniGdi::Surface desktop;
            if (!desktop.Resize(viewportWidth, viewportHeight, MiniGdi::OpaqueWhite))
            {
                return;
            }
            {
                std::lock_guard<std::mutex> guard(presentation->lock);
                if (presentation->pixels && presentation->width == viewportWidth &&
                    presentation->height == viewportHeight &&
                    presentation->pixels->size() == desktop.Pixels().size())
                {
                    desktop.Pixels() = *presentation->pixels;
                }
            }

            // Keep virtual-screen coordinates intact, including partially
            // off-screen windows. CopyRect clips pixels at the viewport edge;
            // input uses the same origin and therefore remains aligned.
            const int rootLeft = snapshots[root].bounds.left;
            const int rootTop = snapshots[root].bounds.top;
            MiniGdi::CopyRect(desktop, MiniGdi::Point{ rootLeft, rootTop }, composite,
                MiniGdi::Rect{ 0, 0, composite.Width(), composite.Height() });

            const GuestMetrics::NonClientMetrics& nonClient = snapshots[root].nonClient;
            if (nonClient.left > 0 || nonClient.top > 0 || nonClient.right > 0 ||
                nonClient.bottom > 0)
            {
                const int rootRight = SaturatingAdd(rootLeft, composite.Width());
                const int rootBottom = SaturatingAdd(rootTop, composite.Height());
                MiniGdi::DrawRectangle(desktop,
                    MiniGdi::Rect{ rootLeft, rootTop, rootRight, rootBottom },
                    MiniGdi::Transparent, MiniGdi::MakeColor(92, 92, 92));
                if (nonClient.caption)
                {
                    const int captionLeft = SaturatingAdd(rootLeft, nonClient.left);
                    const int captionTop = SaturatingAdd(rootTop,
                        nonClient.top - GuestMetrics::CaptionHeight);
                    const int captionRight = rootRight - nonClient.right;
                    const int captionBottom = SaturatingAdd(captionTop,
                        GuestMetrics::CaptionHeight);
                    MiniGdi::FillRect(desktop,
                        MiniGdi::Rect{ captionLeft, captionTop, captionRight, captionBottom },
                        MiniGdi::MakeColor(48, 96, 160));
                    const MiniGdi::DcHandle captionDc = m_gdi.CreateDc(&desktop);
                    if (captionDc != MiniGdi::InvalidDc)
                    {
                        m_gdi.SetTextColor(captionDc, MiniGdi::OpaqueWhite, nullptr);
                        m_gdi.SetBackgroundMode(captionDc,
                            MiniGdi::BackgroundMode::Transparent, nullptr);
                        const int closeExtent = nonClient.closeButton
                            ? GuestMetrics::CaptionHeight : 0;
                        const size_t availableCharacters = static_cast<size_t>((std::max)(
                            0, captionRight - captionLeft - closeExtent - 10) /
                            MiniGdi::DefaultTextGlyphWidth);
                        const size_t titleLength = (std::min)(
                            snapshots[root].title.size(), availableCharacters);
                        m_gdi.TextOutW(captionDc,
                            MiniGdi::Point{ captionLeft + 6,
                                captionTop + (GuestMetrics::CaptionHeight -
                                    MiniGdi::DefaultTextGlyphHeight) / 2 },
                            snapshots[root].title.data(), titleLength, nullptr);
                        m_gdi.DestroyDc(captionDc);
                    }
                    if (nonClient.closeButton)
                    {
                        const int buttonLeft = captionRight - GuestMetrics::CaptionHeight;
                        MiniGdi::DrawLine(desktop,
                            MiniGdi::Point{ buttonLeft + 7, captionTop + 7 },
                            MiniGdi::Point{ captionRight - 7, captionBottom - 7 },
                            MiniGdi::OpaqueWhite);
                        MiniGdi::DrawLine(desktop,
                            MiniGdi::Point{ captionRight - 7, captionTop + 7 },
                            MiniGdi::Point{ buttonLeft + 7, captionBottom - 7 },
                            MiniGdi::OpaqueWhite);
                    }
                }
            }
            composite = std::move(desktop);
        }

        const size_t changedPixels = static_cast<size_t>(std::count_if(
            composite.Pixels().cbegin(),
            composite.Pixels().cend(),
            [](MiniGdi::Color color)
            {
                return color != MiniGdi::OpaqueWhite;
            }));
        RuntimeDiagnostics::Record(
            L"FRAME COMPOSED: root " + std::to_wstring(reinterpret_cast<ULONG_PTR>(snapshots[root].handle)) +
            L", " + std::to_wstring(composite.Width()) + L"x" + std::to_wstring(composite.Height()) +
            L", non-white pixels " + std::to_wstring(changedPixels) + L".");

        auto pixels = std::make_shared<std::vector<MiniGdi::Color>>(composite.Pixels());
        const int width = composite.Width();
        const int height = composite.Height();

        bool queue = false;
        std::uint64_t ticket = 0;
        {
            std::lock_guard<std::mutex> guard(presentation->lock);
            if (!presentation->active)
            {
                return;
            }
            presentation->pixels = std::move(pixels);
            presentation->width = width;
            presentation->height = height;
            ++presentation->frameSerial;
            if (!presentation->presentQueued)
            {
                presentation->presentQueued = true;
                ticket = ++presentation->nextTicket;
                presentation->queuedTicket = ticket;
                queue = true;
            }
        }
        if (queue)
        {
            QueuePresentation(presentation, ticket);
        }
    }
    catch (Exception^ error)
    {
        // Snapshot allocation and dispatcher work are both fallible. Neither
        // may be allowed to unwind into guest code.
        RuntimeDiagnostics::Record(
            L"FRAME EXCEPTION: composition failed; HRESULT " +
            std::to_wstring(static_cast<unsigned long>(error->HResult)) + L".");
    }
    catch (...)
    {
        RuntimeDiagnostics::Record(L"FRAME EXCEPTION: composition raised an unknown exception.");
    }
}

DWORD GuestWindowManager::InvokePointerInput(GuestWindowManager* manager, PointerEventArgs^ args, UINT message)
{
    PointerInputCall call{ manager, args, message };
    return InvokeSehProtected(&GuestWindowManager::InvokePointerInputThunk, &call);
}

DWORD GuestWindowManager::InvokeWheelInput(GuestWindowManager* manager, PointerEventArgs^ args)
{
    WheelInputCall call{ manager, args };
    return InvokeSehProtected(&GuestWindowManager::InvokeWheelInputThunk, &call);
}

DWORD GuestWindowManager::InvokeKeyInput(GuestWindowManager* manager, KeyEventArgs^ args, UINT message)
{
    KeyInputCall call{ manager, args, message };
    return InvokeSehProtected(&GuestWindowManager::InvokeKeyInputThunk, &call);
}

DWORD GuestWindowManager::InvokeCharacterInput(
    GuestWindowManager* manager,
    CharacterReceivedEventArgs^ args)
{
    CharacterInputCall call{ manager, args };
    return InvokeSehProtected(&GuestWindowManager::InvokeCharacterInputThunk, &call);
}

void GuestWindowManager::InvokePointerInputThunk(void* context)
{
    const auto* call = static_cast<PointerInputCall*>(context);
    call->manager->HandlePointer(call->args, call->message);
}

void GuestWindowManager::InvokeWheelInputThunk(void* context)
{
    const auto* call = static_cast<WheelInputCall*>(context);
    call->manager->HandleWheel(call->args);
}

void GuestWindowManager::InvokeKeyInputThunk(void* context)
{
    const auto* call = static_cast<KeyInputCall*>(context);
    call->manager->HandleKey(call->args, call->message);
}

void GuestWindowManager::InvokeCharacterInputThunk(void* context)
{
    const auto* call = static_cast<CharacterInputCall*>(context);
    call->manager->HandleCharacter(call->args);
}

void GuestWindowManager::HandlePointer(PointerEventArgs^ args, UINT requestedMessage)
{
    try
    {
    if (!m_active.load() || !m_inputEnabled.load() || !args || !args->CurrentPoint || !args->CurrentPoint->PointerDevice ||
        args->CurrentPoint->PointerDevice->PointerDeviceType != PointerDeviceType::Mouse)
    {
        return;
    }

    const auto point = args->CurrentPoint;
    // A tracked popup is modal: it owns input even if a guest control still
    // has capture from the right-click that opened it. Route pointer input to
    // the menu before consulting capture/hit-testing, otherwise its only way
    // to disappear is an unrelated ListView click.
    const auto menuProperties = point->Properties;
    UINT menuMessage = requestedMessage;
    if (menuMessage == 0 && menuProperties)
    {
        switch (menuProperties->PointerUpdateKind)
        {
        case PointerUpdateKind::LeftButtonPressed: menuMessage = GuestAbi::WmLButtonDown; break;
        case PointerUpdateKind::LeftButtonReleased: menuMessage = GuestAbi::WmLButtonUp; break;
        case PointerUpdateKind::RightButtonPressed: menuMessage = GuestAbi::WmRButtonDown; break;
        case PointerUpdateKind::RightButtonReleased: menuMessage = GuestAbi::WmRButtonUp; break;
        case PointerUpdateKind::MiddleButtonPressed: menuMessage = GuestAbi::WmMButtonDown; break;
        case PointerUpdateKind::MiddleButtonReleased: menuMessage = GuestAbi::WmMButtonUp; break;
        default: menuMessage = GuestAbi::WmMouseMove; break;
        }
    }
    HWND menuForeground = reinterpret_cast<HWND>(m_foregroundWindow.load());
    {
        std::lock_guard<std::mutex> guard(m_popupMenuLock);
        if (m_popupMenu.open && m_popupMenu.root)
            menuForeground = m_popupMenu.root;
    }
    HWND menuRoot = nullptr;
    int menuRootWidth = 0;
    int menuRootHeight = 0;
    int ignoredWidth = 0;
    int ignoredHeight = 0;
    int menuRootLeft = 0;
    int menuRootTop = 0;
    if (GetGuestSurfaceGeometry(menuForeground, &menuRoot, &menuRootWidth, &menuRootHeight,
        &ignoredWidth, &ignoredHeight, &menuRootLeft, &menuRootTop))
    {
        const LPARAM menuPosition = MousePosition(
            (std::max)(1, m_viewportWidth.load()),
            (std::max)(1, m_viewportHeight.load()),
            menuRootWidth, menuRootHeight, menuRootLeft, menuRootTop,
            point->Position, m_surfaceImage.Get(), false);
        if (HandleGuestMenuPointer(menuRoot,
            static_cast<int>(static_cast<short>(menuPosition & 0xffff)),
            static_cast<int>(static_cast<short>((menuPosition >> 16) & 0xffff)), menuMessage))
        {
            return;
        }
    }
    HWND target = reinterpret_cast<HWND>(m_captureWindow.load());
    if (target && (!IsGuestWindowVisibleInternal(target) || !IsGuestWindowEnabledInternal(target)))
    {
        // CoreWindow input arrives on the host UI thread. Do not invoke a
        // guest WNDPROC from here merely to announce a stale capture; normal
        // guest-side destroy/hide/disable paths already send the notification.
        ULONG_PTR expected = reinterpret_cast<ULONG_PTR>(target);
        m_captureWindow.compare_exchange_strong(expected, 0);
        target = nullptr;
    }
    if (!target)
    {
        const auto menuProperties = point->Properties;
        UINT menuMessage = requestedMessage;
        if (menuMessage == 0 && menuProperties)
        {
            switch (menuProperties->PointerUpdateKind)
            {
            case PointerUpdateKind::LeftButtonPressed: menuMessage = GuestAbi::WmLButtonDown; break;
            case PointerUpdateKind::LeftButtonReleased: menuMessage = GuestAbi::WmLButtonUp; break;
            case PointerUpdateKind::RightButtonPressed: menuMessage = GuestAbi::WmRButtonDown; break;
            case PointerUpdateKind::RightButtonReleased: menuMessage = GuestAbi::WmRButtonUp; break;
            case PointerUpdateKind::MiddleButtonPressed: menuMessage = GuestAbi::WmMButtonDown; break;
            case PointerUpdateKind::MiddleButtonReleased: menuMessage = GuestAbi::WmMButtonUp; break;
            default: menuMessage = GuestAbi::WmMouseMove; break;
            }
        }
        const HWND foreground = reinterpret_cast<HWND>(m_foregroundWindow.load());
        HWND root = nullptr;
        int rootWidth = 0;
        int rootHeight = 0;
        int ignoredWidth = 0;
        int ignoredHeight = 0;
        int rootLeft = 0;
        int rootTop = 0;
        if (!GetGuestSurfaceGeometry(
            foreground,
            &root,
            &rootWidth,
            &rootHeight,
            &ignoredWidth,
            &ignoredHeight,
            &rootLeft,
            &rootTop))
        {
            return;
        }

        const LPARAM rootPosition = MousePosition(
            (std::max)(1, m_viewportWidth.load()),
            (std::max)(1, m_viewportHeight.load()),
            rootWidth,
            rootHeight,
            rootLeft,
            rootTop,
            point->Position,
            m_surfaceImage.Get());
        const int rootX = static_cast<int>(static_cast<WORD>(rootPosition & 0xffff));
        const int rootY = static_cast<int>(static_cast<WORD>((rootPosition >> 16) & 0xffff));
        if (HandleGuestMenuPointer(root, rootX, rootY, menuMessage))
        {
            return;
        }
        target = HitTestGuestWindow(root, rootX, rootY);
    }

    if (!target || !IsGuestWindowVisibleInternal(target) || !IsGuestWindowEnabledInternal(target))
    {
        return;
    }

    const auto properties = point->Properties;
    UINT message = requestedMessage;
    if (message == 0 && properties)
    {
        switch (properties->PointerUpdateKind)
        {
        case PointerUpdateKind::LeftButtonPressed: message = GuestAbi::WmLButtonDown; break;
        case PointerUpdateKind::LeftButtonReleased: message = GuestAbi::WmLButtonUp; break;
        case PointerUpdateKind::RightButtonPressed: message = GuestAbi::WmRButtonDown; break;
        case PointerUpdateKind::RightButtonReleased: message = GuestAbi::WmRButtonUp; break;
        case PointerUpdateKind::MiddleButtonPressed: message = GuestAbi::WmMButtonDown; break;
        case PointerUpdateKind::MiddleButtonReleased: message = GuestAbi::WmMButtonUp; break;
        case PointerUpdateKind::XButton1Pressed: message = GuestAbi::WmXButtonDown; break;
        case PointerUpdateKind::XButton1Released: message = GuestAbi::WmXButtonUp; break;
        case PointerUpdateKind::XButton2Pressed: message = GuestAbi::WmXButtonDown; break;
        case PointerUpdateKind::XButton2Released: message = GuestAbi::WmXButtonUp; break;
        default: message = GuestAbi::WmMouseMove; break;
        }
    }

    HWND root = nullptr;
    int rootWidth = 0;
    int rootHeight = 0;
    int targetWidth = 0;
    int targetHeight = 0;
    int targetLeft = 0;
    int targetTop = 0;
    if (!GetGuestSurfaceGeometry(
        target,
        &root,
        &rootWidth,
        &rootHeight,
        &targetWidth,
        &targetHeight,
        &targetLeft,
        &targetTop))
    {
        return;
    }
    const LPARAM position = MousePosition(
        (std::max)(1, m_viewportWidth.load()),
        (std::max)(1, m_viewportHeight.load()),
        targetWidth,
        targetHeight,
        targetLeft,
        targetTop,
        point->Position,
        m_surfaceImage.Get());
    const int localX = static_cast<int>(static_cast<short>(position & 0xffff));
    const int localY = static_cast<int>(static_cast<short>((position >> 16) & 0xffff));
    if (message == GuestAbi::WmLButtonDown || message == GuestAbi::WmLButtonUp)
    {
        const auto targetRecord = FindWindow(target);
        bool closeHit = false;
        if (targetRecord)
        {
            std::lock_guard<std::mutex> guard(targetRecord->lock);
            const GuestMetrics::NonClientMetrics nonClient =
                GuestMetrics::NonClientForEmbeddedWindow(
                    targetRecord->style, targetRecord->extendedStyle,
                    targetRecord->parent != nullptr);
            const int captionTop = nonClient.top - GuestMetrics::CaptionHeight;
            const int captionRight = targetWidth - nonClient.right;
            closeHit = nonClient.closeButton &&
                localX >= captionRight - GuestMetrics::CaptionHeight &&
                localX < captionRight && localY >= captionTop &&
                localY < nonClient.top;
        }
        if (closeHit)
        {
            if (message == GuestAbi::WmLButtonUp)
                PostGuestMessage(target, GuestAbi::WmSysCommand,
                    GuestAbi::ScClose, 0, nullptr);
            return;
        }
    }
    WPARAM state = MouseKeyState(properties);
    if (message == GuestAbi::WmXButtonDown || message == GuestAbi::WmXButtonUp)
    {
        const WORD xButton = properties
            ? XButtonFromUpdate(properties->PointerUpdateKind)
            : GuestAbi::XButton1;
        state |= static_cast<WPARAM>(xButton) << 16;
    }
        PostGuestMessage(target, message, state, position, nullptr);
    }
    catch (Exception^ error)
    {
        RuntimeDiagnostics::Record(L"INPUT: CoreWindow pointer event was rejected (HRESULT " +
            std::to_wstring(static_cast<unsigned long>(error->HResult)) + L").");
    }
    catch (...)
    {
        RuntimeDiagnostics::Record(L"INPUT: CoreWindow pointer event raised an unknown exception.");
    }
}

void GuestWindowManager::HandleWheel(PointerEventArgs^ args)
{
    try
    {
    if (!m_active.load() || !m_inputEnabled.load() || !args || !args->CurrentPoint || !args->CurrentPoint->Properties)
    {
        return;
    }

    const auto point = args->CurrentPoint;
    HWND target = reinterpret_cast<HWND>(m_captureWindow.load());
    if (target && (!IsGuestWindowVisibleInternal(target) || !IsGuestWindowEnabledInternal(target)))
    {
        ULONG_PTR expected = reinterpret_cast<ULONG_PTR>(target);
        m_captureWindow.compare_exchange_strong(expected, 0);
        target = nullptr;
    }
    if (!target)
    {
        const HWND foreground = reinterpret_cast<HWND>(m_foregroundWindow.load());
        HWND root = nullptr;
        int rootWidth = 0;
        int rootHeight = 0;
        int ignoredWidth = 0;
        int ignoredHeight = 0;
        int rootLeft = 0;
        int rootTop = 0;
        if (!GetGuestSurfaceGeometry(
            foreground,
            &root,
            &rootWidth,
            &rootHeight,
            &ignoredWidth,
            &ignoredHeight,
            &rootLeft,
            &rootTop))
        {
            return;
        }

        const LPARAM rootPosition = MousePosition(
            (std::max)(1, m_viewportWidth.load()),
            (std::max)(1, m_viewportHeight.load()),
            rootWidth,
            rootHeight,
            rootLeft,
            rootTop,
            point->Position,
            m_surfaceImage.Get());
        const int rootX = static_cast<int>(static_cast<WORD>(rootPosition & 0xffff));
        const int rootY = static_cast<int>(static_cast<WORD>((rootPosition >> 16) & 0xffff));
        target = HitTestGuestWindow(root, rootX, rootY);
    }

    if (!target || !IsGuestWindowVisibleInternal(target) || !IsGuestWindowEnabledInternal(target))
    {
        return;
    }
    HWND root = nullptr;
    int rootWidth = 0;
    int rootHeight = 0;
    int targetWidth = 0;
    int targetHeight = 0;
    int targetLeft = 0;
    int targetTop = 0;
    if (!GetGuestSurfaceGeometry(
        target,
        &root,
        &rootWidth,
        &rootHeight,
        &targetWidth,
        &targetHeight,
        &targetLeft,
        &targetTop))
    {
        return;
    }
    const LPARAM position = MousePosition(
        (std::max)(1, m_viewportWidth.load()),
        (std::max)(1, m_viewportHeight.load()),
        targetWidth,
        targetHeight,
        targetLeft,
        targetTop,
        point->Position,
        m_surfaceImage.Get());
    const WPARAM wParam = MouseKeyState(point->Properties) |
        (static_cast<WPARAM>(static_cast<WORD>(point->Properties->MouseWheelDelta)) << 16);
        PostGuestMessage(target, GuestAbi::WmMouseWheel, wParam, position, nullptr);
    }
    catch (Exception^ error)
    {
        RuntimeDiagnostics::Record(L"INPUT: CoreWindow wheel event was rejected (HRESULT " +
            std::to_wstring(static_cast<unsigned long>(error->HResult)) + L").");
    }
    catch (...)
    {
        RuntimeDiagnostics::Record(L"INPUT: CoreWindow wheel event raised an unknown exception.");
    }
}

void GuestWindowManager::HandleKey(KeyEventArgs^ args, UINT message)
{
    if (!m_active.load() || !m_inputEnabled.load() || !args)
    {
        return;
    }
    const WPARAM key = static_cast<WPARAM>(args->VirtualKey);
    const auto keyStatus = args->KeyStatus;
    const UINT repeatCount = (std::max)(1u,
        (std::min)(static_cast<UINT>(keyStatus.RepeatCount), 0xffffu));
    ULONG_PTR keyData = repeatCount |
        ((static_cast<ULONG_PTR>(keyStatus.ScanCode) & 0xffu) << 16);
    if (keyStatus.IsExtendedKey) keyData |= 1ull << 24;
    if (keyStatus.IsMenuKeyDown) keyData |= 1ull << 29;
    if (keyStatus.WasKeyDown) keyData |= 1ull << 30;
    if (message == GuestAbi::WmKeyUp || keyStatus.IsKeyReleased)
        keyData |= 1ull << 31;
    MouseInput().UpdateKeyState(static_cast<int>(key),
        message == GuestAbi::WmKeyDown, keyStatus.ScanCode,
        keyStatus.IsExtendedKey);
    if (message == GuestAbi::WmKeyDown)
    {
        PopupMenuSession popup;
        {
            std::lock_guard<std::mutex> guard(m_popupMenuLock);
            popup = m_popupMenu;
        }
        if (popup.open)
        {
            if (popup.menuBar && args->KeyStatus.IsMenuKeyDown && key >= L'0' && key <= L'Z')
            {
                const wchar_t wanted = static_cast<wchar_t>(towupper(static_cast<wchar_t>(key)));
                const std::vector<GuestMenuVisualItem> bar = GetGuestMenuBarItems(popup.root);
                for (size_t index = 0; index < bar.size(); ++index)
                {
                    if (MenuMnemonic(bar[index].text) != wanted || !bar[index].subMenu ||
                        IsMenuItemDisabled(bar[index])) continue;
                    if (static_cast<int>(index) != popup.topMenuIndex)
                    {
                        int x = 8;
                        for (size_t before = 0; before < index; ++before)
                            x = SaturatingAdd(x, MenuBarItemWidth(bar[before]));
                        HandleGuestMenuPointer(popup.root, x + 1, 1, GuestAbi::WmLButtonDown);
                    }
                    return;
                }
            }
            bool repaint = false;
            bool close = false;
            bool activate = false;
            bool switchTop = false;
            int switchDirection = 0;
            UINT command = 0;
            HMENU openedChild = nullptr;
            HMENU closedChild = nullptr;
            int openedFrom = -1;
            int openedLevel = -1;
            {
                std::lock_guard<std::mutex> guard(m_popupMenuLock);
                if (!m_popupMenu.open || m_popupMenu.levels.empty()) return;
                PopupMenuLevel& level = m_popupMenu.levels.back();
                std::vector<GuestMenuVisualItem> items = GetGuestMenuItems(level.menu);
                const auto selectable = [&items](int index)
                {
                    return index >= 0 && static_cast<size_t>(index) < items.size() &&
                        !IsMenuItemSeparator(items[static_cast<size_t>(index)]) &&
                        !IsMenuItemDisabled(items[static_cast<size_t>(index)]);
                };
                const auto nextSelectable = [&items, &selectable](int current, int direction)
                {
                    if (items.empty()) return -1;
                    for (size_t count = 0; count < items.size(); ++count)
                    {
                        current = (current + direction + static_cast<int>(items.size())) %
                            static_cast<int>(items.size());
                        if (selectable(current)) return current;
                    }
                    return -1;
                };
                if (key == GuestAbi::VkEscape)
                {
                    if (m_popupMenu.levels.size() > 1)
                    {
                        closedChild = m_popupMenu.levels.back().menu;
                        m_popupMenu.levels.pop_back();
                        repaint = true;
                    }
                    else close = true;
                }
                else if (key == GuestAbi::VkDown || key == GuestAbi::VkUp)
                {
                    level.hotItem = nextSelectable(level.hotItem,
                        key == GuestAbi::VkDown ? 1 : -1);
                    if (level.hotItem >= 0)
                        m_popupMenu.levels.resize(m_popupMenu.levels.size());
                    repaint = true;
                }
                else if (key == GuestAbi::VkLeft)
                {
                    if (m_popupMenu.levels.size() > 1)
                    {
                        closedChild = m_popupMenu.levels.back().menu;
                        m_popupMenu.levels.pop_back();
                        repaint = true;
                    }
                    else if (m_popupMenu.menuBar)
                    {
                        switchTop = true;
                        switchDirection = -1;
                    }
                }
                else if (key == GuestAbi::VkRight || key == GuestAbi::VkReturn)
                {
                    if (!selectable(level.hotItem))
                        level.hotItem = nextSelectable(-1, 1);
                    if (selectable(level.hotItem))
                    {
                        const GuestMenuVisualItem& item = items[static_cast<size_t>(level.hotItem)];
                        if (item.subMenu)
                        {
                            openedChild = item.subMenu;
                            openedFrom = level.hotItem;
                            openedLevel = static_cast<int>(m_popupMenu.levels.size() - 1);
                        }
                        else if (key == GuestAbi::VkReturn && item.identifier)
                        {
                            command = item.identifier;
                            activate = true;
                        }
                        else if (key == GuestAbi::VkRight && m_popupMenu.menuBar &&
                            m_popupMenu.levels.size() == 1)
                        {
                            switchTop = true;
                            switchDirection = 1;
                        }
                    }
                    else if (key == GuestAbi::VkRight && m_popupMenu.menuBar)
                    {
                        switchTop = true;
                        switchDirection = 1;
                    }
                }
                else if (key >= L'0' && key <= L'Z')
                {
                    const wchar_t wanted = static_cast<wchar_t>(towupper(static_cast<wchar_t>(key)));
                    for (size_t index = 0; index < items.size(); ++index)
                    {
                        if (MenuMnemonic(items[index].text) != wanted || !selectable(static_cast<int>(index)))
                            continue;
                        level.hotItem = static_cast<int>(index);
                        if (items[index].subMenu)
                        {
                            openedChild = items[index].subMenu;
                            openedFrom = static_cast<int>(index);
                            openedLevel = static_cast<int>(m_popupMenu.levels.size() - 1);
                        }
                        else
                        {
                            command = items[index].identifier;
                            activate = command != 0;
                        }
                        repaint = true;
                        break;
                    }
                }
                if (close || activate)
                {
                    m_popupMenu.selectedCommand = activate ? command : 0;
                    m_popupMenu.open = false;
                    popup = m_popupMenu;
                }
            }

            if (closedChild)
                SendGuestMessage(popup.owner, GuestAbi::WmUninitMenuPopup,
                    reinterpret_cast<WPARAM>(closedChild), 0, nullptr);

            if (repaint && !close && !activate)
            {
                HMENU selectedMenu = nullptr;
                int selectedIndex = -1;
                {
                    std::lock_guard<std::mutex> guard(m_popupMenuLock);
                    if (m_popupMenu.open && !m_popupMenu.levels.empty())
                    {
                        selectedMenu = m_popupMenu.levels.back().menu;
                        selectedIndex = m_popupMenu.levels.back().hotItem;
                    }
                }
                const std::vector<GuestMenuVisualItem> selectedItems =
                    GetGuestMenuItems(selectedMenu);
                if (selectedIndex >= 0 && static_cast<size_t>(selectedIndex) < selectedItems.size())
                {
                    const GuestMenuVisualItem& selectedItem =
                        selectedItems[static_cast<size_t>(selectedIndex)];
                    const UINT selectedValue = selectedItem.subMenu
                        ? static_cast<UINT>(selectedIndex) : selectedItem.identifier;
                    SendGuestMessage(popup.owner, GuestAbi::WmMenuSelect,
                        GuestAbi::MakeCommandWParam(static_cast<WORD>(selectedValue),
                            static_cast<WORD>(MenuSelectFlags(selectedItem, false))),
                        reinterpret_cast<LPARAM>(selectedMenu), nullptr);
                }
            }

            if (switchTop)
            {
                const std::vector<GuestMenuVisualItem> bar = GetGuestMenuBarItems(popup.root);
                if (!bar.empty())
                {
                    int candidate = popup.topMenuIndex;
                    for (size_t count = 0; count < bar.size(); ++count)
                    {
                        candidate = (candidate + switchDirection + static_cast<int>(bar.size())) %
                            static_cast<int>(bar.size());
                        if (bar[static_cast<size_t>(candidate)].subMenu &&
                            !IsMenuItemDisabled(bar[static_cast<size_t>(candidate)])) break;
                    }
                    int x = 8;
                    for (int index = 0; index < candidate; ++index)
                        x = SaturatingAdd(x, MenuBarItemWidth(bar[static_cast<size_t>(index)]));
                    HandleGuestMenuPointer(popup.root, x + 1, 1, GuestAbi::WmLButtonDown);
                }
                return;
            }
            if (openedChild)
            {
                SendGuestMessage(popup.owner, GuestAbi::WmInitMenuPopup,
                    reinterpret_cast<WPARAM>(openedChild),
                    GuestAbi::MakeCommandWParam(static_cast<WORD>(openedFrom), 0), nullptr);
                const std::vector<GuestMenuVisualItem> childItems = GetGuestMenuItems(openedChild);
                std::lock_guard<std::mutex> guard(m_popupMenuLock);
                if (m_popupMenu.open && openedLevel >= 0 &&
                    static_cast<size_t>(openedLevel) < m_popupMenu.levels.size() && !childItems.empty())
                {
                    const PopupMenuLevel parent = m_popupMenu.levels[static_cast<size_t>(openedLevel)];
                    const std::vector<GuestMenuVisualItem> parentItems = GetGuestMenuItems(parent.menu);
                    const int childWidth = PopupMenuWidth(childItems);
                    int childLeft = parent.left + PopupMenuWidth(parentItems) - 2;
                    int width = 0, height = 0, ignored = 0;
                    HWND ignoredRoot = nullptr;
                    GetGuestSurfaceGeometry(popup.root, &ignoredRoot, &width, &height,
                        &ignored, &ignored, &ignored, &ignored);
                    if (childLeft + childWidth > width) childLeft = (std::max)(0, parent.left - childWidth + 2);
                    const int childTop = (std::max)(0, (std::min)(parent.top +
                        PopupMenuItemTop(parentItems, static_cast<size_t>(openedFrom)),
                        (std::max)(0, height - PopupMenuHeight(childItems))));
                    m_popupMenu.levels.resize(static_cast<size_t>(openedLevel + 1));
                    m_popupMenu.levels.push_back(PopupMenuLevel{
                        openedChild, childLeft, childTop, openedFrom, -1 });
                    repaint = true;
                }
            }
            if (close || activate)
            {
                const auto root = FindWindow(popup.root);
                if (root)
                {
                    std::lock_guard<std::mutex> guard(root->lock);
                    if (!root->destroyed) root->openMenuIndex = -1;
                }
                if (popup.owner)
                {
                    SendGuestMessage(popup.owner, GuestAbi::WmMenuSelect,
                        GuestAbi::MakeCommandWParam(0, 0xffff), 0, nullptr);
                    if (popup.menuBar)
                    {
                        for (auto iterator = popup.levels.rbegin(); iterator != popup.levels.rend(); ++iterator)
                            SendGuestMessage(popup.owner, GuestAbi::WmUninitMenuPopup,
                                reinterpret_cast<WPARAM>(iterator->menu), 0, nullptr);
                        SendGuestMessage(popup.owner, GuestAbi::WmExitMenuLoop, FALSE, 0, nullptr);
                        if (activate && popup.notifyOwner)
                            PostGuestMessage(popup.owner, GuestAbi::WmCommand,
                                GuestAbi::MakeCommandWParam(static_cast<WORD>(command), 0), 0, nullptr);
                    }
                }
                m_popupMenuChanged.notify_all();
            }
            if (repaint || close || activate)
            {
                InvalidateGuestRect(popup.root, nullptr, FALSE, nullptr);
                Present(FindWindow(popup.root));
            }
            return;
        }

        if (key == GuestAbi::VkMenu || key == GuestAbi::VkF10)
        {
            HWND target = reinterpret_cast<HWND>(m_focusWindow.load());
            if (!target) target = reinterpret_cast<HWND>(m_foregroundWindow.load());
            HWND root = nullptr;
            int ignored = 0;
            if (target && GetGuestSurfaceGeometry(target, &root, &ignored, &ignored,
                &ignored, &ignored, &ignored, &ignored))
            {
                const auto bar = GetGuestMenuBarItems(root);
                int x = 8;
                for (const auto& item : bar)
                {
                    if (item.subMenu && !IsMenuItemDisabled(item))
                    {
                        HandleGuestMenuPointer(root, x + 1, 1, GuestAbi::WmLButtonDown);
                        break;
                    }
                    x = SaturatingAdd(x, MenuBarItemWidth(item));
                }
            }
            return;
        }
    }
    HWND target = reinterpret_cast<HWND>(m_focusWindow.load());
    if (!target)
    {
        target = reinterpret_cast<HWND>(m_foregroundWindow.load());
    }
    if (target && IsGuestWindowVisibleInternal(target) && IsGuestWindowEnabledInternal(target))
    {
        PostGuestMessage(target, message, static_cast<WPARAM>(args->VirtualKey),
            static_cast<LPARAM>(keyData), nullptr);
    }
}

void GuestWindowManager::HandleCharacter(CharacterReceivedEventArgs^ args)
{
    if (!m_active.load() || !m_inputEnabled.load() || !args)
        return;

    {
        std::lock_guard<std::mutex> guard(m_popupMenuLock);
        if (m_popupMenu.open) return;
    }

    HWND target = reinterpret_cast<HWND>(m_focusWindow.load());
    if (!target) target = reinterpret_cast<HWND>(m_foregroundWindow.load());
    if (!target || !IsGuestWindowVisibleInternal(target) ||
        !IsGuestWindowEnabledInternal(target))
        return;

    const UINT32 codePoint = args->KeyCode;
    if (codePoint == 0 || codePoint > 0x10ffff ||
        (codePoint >= 0xd800 && codePoint <= 0xdfff))
        return;

    if (codePoint <= 0xffff)
    {
        PostGuestMessage(target, GuestAbi::WmChar,
            static_cast<WPARAM>(codePoint), 0, nullptr);
    }
    else
    {
        const UINT32 scalar = codePoint - 0x10000;
        const wchar_t high = static_cast<wchar_t>(0xd800 + (scalar >> 10));
        const wchar_t low = static_cast<wchar_t>(0xdc00 + (scalar & 0x3ff));
        PostGuestMessage(target, GuestAbi::WmChar, static_cast<WPARAM>(high), 0, nullptr);
        PostGuestMessage(target, GuestAbi::WmChar, static_cast<WPARAM>(low), 0, nullptr);
    }
    args->Handled = true;
}

void GuestWindowManager::HandleHostSizeChanged(int width, int height)
{
    width = (std::max)(0, (std::min)(width, 8192));
    height = (std::max)(0, (std::min)(height, 8192));
    if (width <= 0 || height <= 0)
    {
        return;
    }
    if (m_viewportWidth.exchange(width) == width &&
        m_viewportHeight.exchange(height) == height)
    {
        return;
    }
    m_viewportHeight.store(height);
    GuestMetrics::SetCurrentScreenSize(width, height);

    {
        std::lock_guard<std::mutex> guard(m_screenDcLock);
        if (m_screenDc != MiniGdi::InvalidDc && m_gdi.HasDc(m_screenDc))
        {
            m_screenSurface.Resize(width, height, MiniGdi::OpaqueWhite);
            m_gdi.ResetClip(m_screenDc);
        }
    }

    std::vector<std::shared_ptr<WindowRecord>> roots;
    {
        std::lock_guard<std::mutex> guard(m_windowsLock);
        for (const auto& item : m_windows)
        {
            roots.push_back(item.second);
        }
    }
    for (const auto& root : roots)
    {
        HWND handle = nullptr;
        MiniGdi::DcHandle dc = MiniGdi::InvalidDc;
        bool resized = false;
        int resizedWidth = 0;
        int resizedHeight = 0;
        int resizedClientWidth = 0;
        int resizedClientHeight = 0;
        {
            std::lock_guard<std::mutex> guard(root->lock);
            if (root->destroyed || root->parent)
            {
                continue;
            }
            handle = root->handle;
            dc = root->dc;
            const int oldWidth = root->bounds.right - root->bounds.left;
            const int oldHeight = root->bounds.bottom - root->bounds.top;
            const int newWidth = root->viewportWidthBound ? width : oldWidth;
            const int newHeight = root->viewportHeightBound ? height : oldHeight;
            if (oldWidth == newWidth && oldHeight == newHeight)
            {
                continue;
            }
            if (!root->surface.Resize(newWidth, newHeight, MiniGdi::OpaqueWhite))
            {
                continue;
            }
            root->bounds.right = SaturatingAdd(root->bounds.left, newWidth);
            root->bounds.bottom = SaturatingAdd(root->bounds.top, newHeight);
            root->invalidated = false;
            root->erasePending = false;
            root->updateRect = RECT{};
            resizedWidth = newWidth;
            resizedHeight = newHeight;
            const GuestMetrics::NonClientMetrics nonClient =
                GuestMetrics::NonClientForEmbeddedWindow(
                    root->style, root->extendedStyle, false);
            resizedClientWidth = (std::max)(0, newWidth -
                nonClient.left - nonClient.right);
            resizedClientHeight = (std::max)(0, newHeight -
                nonClient.top - nonClient.bottom -
                (root->menuBar ? GuestMetrics::MenuHeight : 0));
            resized = true;
        }
        if (resized)
        {
            m_gdi.ResetClip(dc);
            PostGuestMessage(handle, GuestAbi::WmSize, GuestAbi::SizeRestored,
                GuestAbi::MakeMouseLParam(
                    static_cast<WORD>((std::min)(resizedClientWidth, 0xffff)),
                    static_cast<WORD>((std::min)(resizedClientHeight, 0xffff))), nullptr);
            InvalidateGuestRect(handle, nullptr, TRUE, nullptr);
            RuntimeDiagnostics::Record(L"VIEWPORT: resized default-bound root to " +
                std::to_wstring(resizedWidth) + L"x" +
                std::to_wstring(resizedHeight) + L".");
        }
    }
}

void GuestWindowManager::DetachHostEvents()
{
    try
    {
        const auto callbacks = m_inputCallbacks;
        if (callbacks)
        {
            std::lock_guard<std::mutex> guard(callbacks->lock);
            callbacks->owner = nullptr;
        }

        if (!m_eventsAttached.exchange(false))
        {
            return;
        }

        CoreWindow^ coreWindow = m_coreWindow.Get();
        if (!coreWindow)
        {
            return;
        }

        const auto pointerMoved = m_pointerMovedToken;
        const auto pointerPressed = m_pointerPressedToken;
        const auto pointerReleased = m_pointerReleasedToken;
        const auto pointerWheel = m_pointerWheelToken;
        const auto keyDown = m_keyDownToken;
        const auto keyUp = m_keyUpToken;
        const auto characterReceived = m_characterReceivedToken;
        const auto sizeChanged = m_sizeChangedToken;
        const auto surfaceSizeChanged = m_surfaceSizeChangedToken;
        Panel^ surfaceHost = m_surfaceHost.Get();
        CoreDispatcher^ dispatcher = m_dispatcher.Get();
        if (dispatcher && dispatcher->HasThreadAccess)
        {
            coreWindow->PointerMoved -= pointerMoved;
            coreWindow->PointerPressed -= pointerPressed;
            coreWindow->PointerReleased -= pointerReleased;
            coreWindow->PointerWheelChanged -= pointerWheel;
            coreWindow->KeyDown -= keyDown;
            coreWindow->KeyUp -= keyUp;
            coreWindow->CharacterReceived -= characterReceived;
            coreWindow->SizeChanged -= sizeChanged;
            if (surfaceHost) surfaceHost->SizeChanged -= surfaceSizeChanged;
            return;
        }

        if (dispatcher)
        {
            Platform::Agile<CoreWindow^> agileCoreWindow(coreWindow);
            Platform::Agile<Panel^> agileSurfaceHost(surfaceHost);
            dispatcher->RunAsync(CoreDispatcherPriority::Normal,
                ref new DispatchedHandler([
                    agileCoreWindow,
                    agileSurfaceHost,
                    pointerMoved,
                    pointerPressed,
                    pointerReleased,
                    pointerWheel,
                    keyDown,
                    keyUp,
                    characterReceived,
                    sizeChanged,
                    surfaceSizeChanged]()
            {
                try
                {
                    CoreWindow^ target = agileCoreWindow.Get();
                    if (!target)
                    {
                        return;
                    }
                    target->PointerMoved -= pointerMoved;
                    target->PointerPressed -= pointerPressed;
                    target->PointerReleased -= pointerReleased;
                    target->PointerWheelChanged -= pointerWheel;
                    target->KeyDown -= keyDown;
                    target->KeyUp -= keyUp;
                    target->CharacterReceived -= characterReceived;
                    target->SizeChanged -= sizeChanged;
                    Panel^ panel = agileSurfaceHost.Get();
                    if (panel) panel->SizeChanged -= surfaceSizeChanged;
                }
                catch (...)
                {
                }
            }));
        }
    }
    catch (...)
    {
    }
}

GuestWindowManager* Win32Bridge::Bridge::CurrentGuestWindowManager()
{
    return g_currentGuestWindowManager;
}

LRESULT WINAPI Win32Bridge::Bridge::BridgeBuiltinControlWindowProc(
    HWND window,
    UINT message,
    WPARAM wParam,
    LPARAM lParam)
{
    GuestWindowManager* manager = CurrentGuestWindowManager();
    return manager ? manager->DefaultGuestWindowProcedure(window, message, wParam, lParam) : 0;
}

GuestWindowScope::GuestWindowScope(GuestWindowManager* manager, bool manageActivation)
    : m_previous(g_currentGuestWindowManager), m_manageActivation(manageActivation)
{
    g_currentGuestWindowManager = manager;
    if (manager && m_manageActivation)
    {
        manager->Activate();
    }
}

GuestWindowScope::~GuestWindowScope()
{
    if (g_currentGuestWindowManager && m_manageActivation)
    {
        g_currentGuestWindowManager->Deactivate();
    }
    g_currentGuestWindowManager = m_previous;
    if (m_previous && m_manageActivation)
    {
        m_previous->Activate();
    }
}
