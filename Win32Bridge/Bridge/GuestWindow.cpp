#include "pch.h"
#include "Bridge\\CommonControlsShims.h"
#include "Bridge\\GuestWindow.h"
#include "Bridge\\User32Shims.h"
#include "Bridge/RuntimeDiagnostics.h"

#include <wrl.h>
#include <windows.system.threading.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <cwctype>
#include <functional>
#include <limits>
#include <new>
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

    thread_local GuestWindowManager* g_currentGuestWindowManager = nullptr;
    thread_local unsigned g_ownerDataDisplayInfoDiagnostics = 0;
    thread_local unsigned g_listViewPopulationDiagnostics = 0;

    void SetWin32Error(DWORD* output, DWORD value)
    {
        if (output)
        {
            *output = value;
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
        // desktop Windows.  7-Zip creates these directly (rather than only
        // through the legacy CreateToolbarEx helper), so rejecting them here
        // causes its panel creation to collapse into the generic E_FAIL
        // reported by the application.  They stay guest-owned virtual
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
            kind == BuiltinControlKind::Tab
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

    int ToolbarItemWidth(const std::vector<BYTE>& styles, const std::vector<int>& bitmaps, size_t index, int buttonWidth)
    {
        const BYTE style = index < styles.size() ? styles[index] : 0;
        if ((style & ToolbarStyleSeparator) != 0)
        {
            // For a separator iBitmap is its requested width; Windows uses a
            // small fixed gap when the caller supplies zero or a sentinel.
            const int requested = index < bitmaps.size() ? bitmaps[index] : 0;
            return requested > 0 ? (std::min)(requested, 128) : 8;
        }
        return (std::max)(16, buttonWidth);
    }

    int ToolbarItemLeft(const std::vector<BYTE>& styles, const std::vector<int>& bitmaps, size_t index, int buttonWidth)
    {
        int left = 2;
        for (size_t previous = 0; previous < index; ++previous)
        {
            left += ToolbarItemWidth(styles, bitmaps, previous, buttonWidth) + 1;
        }
        return left;
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
    constexpr UINT ComboBoxGetCount = 0x0146;
    constexpr UINT ComboBoxExSetImageList = 0x0402;
    constexpr UINT ComboBoxExGetImageList = 0x0403;
    constexpr UINT ComboBoxExGetComboControl = 0x0406;
    constexpr UINT ComboBoxExGetEditControl = 0x0407;
    constexpr UINT ComboBoxExInsertItemW = 0x040b;
    constexpr UINT ComboBoxExSetItemW = 0x040c;
    constexpr UINT ComboBoxExGetItemW = 0x040d;
    constexpr UINT ScrollBarSetPosition = 0x00e0;
    constexpr UINT ScrollBarGetPosition = 0x00e1;
    constexpr UINT ListViewGetItemCount = 0x1004;
    constexpr UINT ListViewSetBackgroundColor = 0x1001;
    constexpr UINT ListViewSetImageList = 0x1003;
    constexpr UINT ListViewDeleteItem = 0x1008;
    constexpr UINT ListViewDeleteAllItems = 0x1009;
    constexpr UINT ListViewGetNextItem = 0x100c;
    constexpr UINT ListViewSetCallbackMask = 0x100b;
    constexpr UINT ListViewSetColumnWidth = 0x101e;
    constexpr UINT ListViewSetTextColor = 0x1024;
    constexpr UINT ListViewSetTextBackgroundColor = 0x1026;
    constexpr UINT ListViewSetItemState = 0x102b;
    constexpr UINT ListViewSetItemCount = 0x102f;
    constexpr UINT ListViewEnsureVisible = 0x1013;
    constexpr UINT ListViewScroll = 0x1014;
    constexpr UINT ListViewGetTopIndex = 0x1027;
    constexpr UINT ListViewGetCountPerPage = 0x1028;
    constexpr UINT ListViewSetExtendedStyle = 0x1036;
    constexpr UINT ListViewSetColumnOrderArray = 0x103a;
    constexpr UINT ListViewGetSelectedCount = 0x1032;
    constexpr UINT ListViewDeleteColumn = 0x101c;
    constexpr UINT ListViewInsertItemW = 0x104d;
    constexpr UINT ListViewSetItemW = 0x104c;
    constexpr UINT ListViewSetItemTextW = 0x1074;
    constexpr UINT ListViewGetItemTextW = 0x1073;
    constexpr UINT ListViewGetItemW = 0x104b;
    constexpr UINT ListViewInsertColumnW = 0x1061;
    constexpr UINT ListViewSetColumnW = 0x1060;
    constexpr UINT ToolbarButtonStructSize = 0x041e;
    constexpr UINT ToolbarSetButtonSize = 0x041f;
    constexpr UINT ToolbarSetBitmapSize = 0x0420;
    constexpr UINT ToolbarAutoSize = 0x0421;
    constexpr UINT ToolbarAddButtonsW = 0x0444;
    constexpr UINT ToolbarGetButtonCount = 0x0418;
    constexpr UINT ToolbarGetRows = 0x0428;
    constexpr UINT ToolbarGetButtonSize = 0x043a;
    constexpr UINT ToolbarSetImageList = 0x0430;
    constexpr UINT ToolbarGetImageList = 0x0431;
    constexpr UINT ToolbarGetItemRect = 0x041d;
    constexpr UINT ToolbarSetButtonWidth = 0x043b;
    constexpr UINT RebarSetBarInfo = 0x0404;
    constexpr UINT RebarInsertBandW = 0x040a;
    constexpr UINT RebarSetBandInfoW = 0x040b;
    constexpr UINT RebarSizeToRect = 0x0417;
    constexpr UINT RebarGetBandCount = 0x040c;
    constexpr UINT RebarGetBarHeight = 0x041b;
    constexpr UINT RebarGetRowHeight = 0x041c;
    constexpr UINT ListViewColumnText = 0x0004;
    constexpr UINT ListViewItemText = 0x0001;
    constexpr UINT ListViewItemParam = 0x0004;
    constexpr UINT ListViewItemState = 0x0008;
    constexpr UINT ListViewStateSelected = 0x0002;
    // LVS_OWNERDATA. LVM_SETITEMCOUNT is defined only for this virtual
    // ListView style; applying it to a retained-item view invents blank rows.
    constexpr DWORD ListViewStyleOwnerData = 0x00001000u;
    constexpr UINT ListViewExtendedGridLines = 0x00000001;
    constexpr UINT ListViewNextItemSelected = 0x0002;
    constexpr UINT ListViewNotifyGetDisplayInfoW = static_cast<UINT>(-177); // LVN_GETDISPINFOW
    constexpr UINT ListViewNotifyItemChanged = static_cast<UINT>(-101); // LVN_ITEMCHANGED
    constexpr UINT ListViewNotifyItemActivate = static_cast<UINT>(-114); // LVN_ITEMACTIVATE
    constexpr UINT NotifyClick = static_cast<UINT>(-2); // NM_CLICK
    constexpr UINT NotifyDoubleClick = static_cast<UINT>(-3); // NM_DBLCLK
    constexpr BYTE ToolbarStateEnabled = 0x04;
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

    // COMBOBOXEXITEMW through lParam.  The address control in 7-Zip uses the
    // text field to publish its current shell location.
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
        int minimumWidth = 24;
        int minimumHeight = 24;
        int width = 0;
    };

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
        return (std::max)(0, (std::min)(value, 2048));
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
        Image^ image)
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

        const int pixelX = (std::max)(0, (std::min)(width - 1, static_cast<int>(x) - targetLeft));
        const int pixelY = (std::max)(0, (std::min)(height - 1, static_cast<int>(y) - targetTop));
        return GuestAbi::MakeMouseLParam(static_cast<WORD>(pixelX), static_cast<WORD>(pixelY));
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
                // Recheck immediately before touching XAML so a frame queued
                // before Deactivate cannot resurrect an old guest surface.
                std::lock_guard<std::mutex> guard(presentation->lock);
                if (presentation->active && presentation->presentQueued &&
                    presentation->queuedTicket == ticket)
                {
                    Image^ image = presentation->image.Get();
                    WriteableBitmap^ bitmap = presentation->bitmap.Get();
                    if (image)
                    {
                        if (!bitmap || bitmap->PixelWidth != width || bitmap->PixelHeight != height)
                        {
                            bitmap = ref new WriteableBitmap(width, height);
                            presentation->bitmap = Platform::Agile<WriteableBitmap^>(bitmap);
                            image->Source = bitmap;
                        }

                        IBuffer^ pixelBuffer = bitmap->PixelBuffer;
                        const size_t byteCount = pixels->size() * sizeof(MiniGdi::Color);
                        ComPtr<IBufferByteAccess> access;
                        const HRESULT result = pixelBuffer
                            ? reinterpret_cast<IInspectable*>(pixelBuffer)->QueryInterface(IID_PPV_ARGS(&access))
                            : E_POINTER;
                        if (SUCCEEDED(result) && pixelBuffer->Length >= byteCount)
                        {
                            unsigned char* destination = nullptr;
                            if (SUCCEEDED(access->Buffer(&destination)) &&
                                reinterpret_cast<ULONG_PTR>(destination) >= 0x10000)
                            {
                                memcpy(destination, pixels->data(), byteCount);
                                bitmap->Invalidate();
                                frameUploaded = true;
                            }
                            else
                            {
                                uploadFailure = L"the bitmap pixel buffer was unavailable";
                            }
                        }
                        else
                        {
                            uploadFailure = L"the bitmap pixel-buffer query failed";
                        }
                    }
                    else
                    {
                        uploadFailure = L"the XAML image was unavailable";
                    }
                }
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
                    ProcessPresentation(presentation, ticket);
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
    size_t editCaret = 0;
    bool buttonPressed = false;
    bool buttonKeyboardPressed = false;
    std::vector<std::wstring> choiceItems;
    HANDLE comboBoxExImageList = nullptr;
    int selectedChoice = -1;
    int scrollPosition = 0;
    std::vector<std::wstring> listViewColumns;
    std::vector<int> listViewColumnWidths;
    // Each inner element is one report-view row.  Keeping subitems here is
    // important: file managers populate the name first and then fill size,
    // type and timestamp through LVM_SETITEMTEXTW.
    std::vector<std::vector<std::wstring>> listViewItems;
    std::vector<LPARAM> listViewItemData;
    COLORREF listViewBackgroundColor = 0x00ffffff;
    COLORREF listViewTextColor = 0x00000000;
    COLORREF listViewTextBackgroundColor = 0x00ffffff;
    UINT listViewExtendedStyle = 0;
    UINT listViewCallbackMask = 0;
    int listViewSelectedItem = -1;
    int listViewTopItem = 0;
    int listViewPressedItem = -1;
    std::chrono::steady_clock::time_point listViewLastClick = {};
    int listViewLastClickItem = -1;
    std::vector<int> toolbarCommands;
    std::vector<int> toolbarBitmaps;
    std::vector<BYTE> toolbarButtonStates;
    std::vector<BYTE> toolbarButtonStyles;
    int toolbarButtonWidth = 24;
    int toolbarButtonHeight = 24;
    int toolbarBitmapWidth = 16;
    int toolbarBitmapHeight = 16;
    int toolbarPressedIndex = -1;
    HANDLE toolbarImageList = nullptr;
    std::vector<RebarBand> rebarBands;
    bool addressBackButton = false;
    bool menuBar = false;
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

    auto image = ref new Image();
    image->Stretch = Stretch::Uniform;
    image->HorizontalAlignment = HorizontalAlignment::Stretch;
    image->VerticalAlignment = VerticalAlignment::Stretch;
    surfaceHost->Children->Append(image);
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
        std::lock_guard<std::mutex> guard(callbacks->lock);
        if (callbacks->owner)
        {
            callbacks->owner->HandlePointer(args, GuestAbi::WmMouseMove);
        }
    });
    m_pointerPressedToken = coreWindow->PointerPressed += ref new TypedEventHandler<CoreWindow^, PointerEventArgs^>(
        [callbacks](CoreWindow^, PointerEventArgs^ args)
    {
        std::lock_guard<std::mutex> guard(callbacks->lock);
        if (callbacks->owner)
        {
            callbacks->owner->HandlePointer(args, 0);
        }
    });
    m_pointerReleasedToken = coreWindow->PointerReleased += ref new TypedEventHandler<CoreWindow^, PointerEventArgs^>(
        [callbacks](CoreWindow^, PointerEventArgs^ args)
    {
        std::lock_guard<std::mutex> guard(callbacks->lock);
        if (callbacks->owner)
        {
            callbacks->owner->HandlePointer(args, 0);
        }
    });
    m_pointerWheelToken = coreWindow->PointerWheelChanged += ref new TypedEventHandler<CoreWindow^, PointerEventArgs^>(
        [callbacks](CoreWindow^, PointerEventArgs^ args)
    {
        std::lock_guard<std::mutex> guard(callbacks->lock);
        if (callbacks->owner)
        {
            callbacks->owner->HandleWheel(args);
        }
    });
    m_keyDownToken = coreWindow->KeyDown += ref new TypedEventHandler<CoreWindow^, KeyEventArgs^>(
        [callbacks](CoreWindow^, KeyEventArgs^ args)
    {
        std::lock_guard<std::mutex> guard(callbacks->lock);
        if (callbacks->owner)
        {
            callbacks->owner->HandleKey(args, GuestAbi::WmKeyDown);
        }
    });
    m_keyUpToken = coreWindow->KeyUp += ref new TypedEventHandler<CoreWindow^, KeyEventArgs^>(
        [callbacks](CoreWindow^, KeyEventArgs^ args)
    {
        std::lock_guard<std::mutex> guard(callbacks->lock);
        if (callbacks->owner)
        {
            callbacks->owner->HandleKey(args, GuestAbi::WmKeyUp);
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

    const std::wstring name = Lowercase(windowClass->lpszClassName);
    if (name.empty())
    {
        SetWin32Error(win32Error, ERROR_INVALID_NAME);
        return 0;
    }

    std::lock_guard<std::mutex> guard(m_classesLock);
    if (m_classes.find(name) != m_classes.end())
    {
        SetWin32Error(win32Error, ERROR_CLASS_ALREADY_EXISTS);
        return 0;
    }

    auto registered = std::make_shared<WindowClass>();
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
    m_classes.emplace(name, registered);
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

std::shared_ptr<GuestWindowManager::WindowClass> GuestWindowManager::FindClass(LPCWSTR className) const
{
    if (!className)
    {
        return nullptr;
    }

    std::lock_guard<std::mutex> guard(m_classesLock);
    if (IsAtomPointer(className))
    {
        const auto found = m_classesByAtom.find(static_cast<ATOM>(reinterpret_cast<ULONG_PTR>(className)));
        return found == m_classesByAtom.end() ? nullptr : found->second;
    }

    const auto found = m_classes.find(Lowercase(className));
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
    window->editCaret = window->title.size();
    window->enabled = (style & GuestWsDisabled) == 0;
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
    // top-level desktop. Giving a toolbar/ReBar a 480-pixel default height
    // makes well-behaved clients (including 7-Zip) conclude that no client
    // area remains for their panel. Keep the generic top-level fallback, but
    // use normal control-strip dimensions for controls that participate in
    // vertical layout before their parent performs its first resize.
    const BuiltinControlKind builtinKind = registered->builtinKind;
    const int defaultWidth = 800;
    const int defaultHeight = builtinKind == BuiltinControlKind::Toolbar ||
        builtinKind == BuiltinControlKind::Rebar ||
        builtinKind == BuiltinControlKind::StatusBar
        ? 24
        : 480;
    window->bounds.left = DefaultCoordinate(x);
    window->bounds.top = DefaultCoordinate(y);
    window->bounds.right = window->bounds.left + DefaultExtent(width, defaultWidth);
    window->bounds.bottom = window->bounds.top + DefaultExtent(height, defaultHeight);
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
    // bridge retained neither relationship, so 7-Zip could create and query
    // its menus but never receive a visible, interactive menu bar.
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
        {
            std::lock_guard<std::mutex> guard(record->lock);
            if (record->destroyed || !record->enabled)
            {
                return false;
            }
            parent = record->parent;
        }
        current = parent;
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
        const LRESULT result = procedure(handle, message, wParam, lParam);
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
        width = record->surface.Width();
        height = record->surface.Height();
    }

    if (show && !wasVisible)
    {
        // Showing a child must not make it a new top-level foreground window.
        // Its parent is composed into the same guest desktop and hit-testing
        // selects the child under the pointer.
        if (!parent)
        {
            m_foregroundWindow.store(reinterpret_cast<ULONG_PTR>(window));
        }
        CallWindowProcedure(record, GuestAbi::WmShowWindow, TRUE, 0);
        if (enabled)
        {
            DWORD ignored = ERROR_SUCCESS;
            SetGuestFocus(window, &ignored);
        }
        PostGuestMessage(window, GuestAbi::WmSize, GuestAbi::SizeRestored,
            GuestAbi::MakeMouseLParam(static_cast<WORD>(width), static_cast<WORD>(height)), nullptr);
        InvalidateGuestRect(window, nullptr, TRUE, nullptr);
        Present(record);
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
    rect->left = 0;
    rect->top = 0;
    rect->right = record->surface.Width();
    rect->bottom = (std::max)(0, record->surface.Height() -
        ((!record->parent && record->menuBar) ? 22 : 0));
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
        width = record->surface.Width();
        height = (std::max)(0, record->surface.Height() - (record->menuBar ? 22 : 0));
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

UINT GuestWindowManager::TrackGuestPopupMenu(HMENU menu, UINT flags, int x, int y, HWND owner, DWORD* win32Error)
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

    // TPM_RETURNCMD is the only result-affecting flag. For the ordinary form
    // the selected command is posted to the owner after the modal menu closes.
    constexpr UINT TpmReturnCommand = 0x0100;
    const std::vector<GuestMenuVisualItem> popupItems = GetGuestMenuItems(menu);
    {
        std::lock_guard<std::mutex> guard(m_popupMenuLock);
        m_popupMenu.menu = menu;
        m_popupMenu.owner = owner;
        m_popupMenu.root = root;
        m_popupMenu.left = (std::max)(0, (std::min)(x, (std::max)(0, rootWidth - 40)));
        m_popupMenu.top = (std::max)(0, (std::min)(y, (std::max)(0, rootHeight - 20)));
        m_popupMenu.returnCommand = (flags & TpmReturnCommand) != 0;
        m_popupMenu.selectedCommand = 0;
        m_popupMenu.open = true;
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
    m_popupMenu = PopupMenuSession{};
    lock.unlock();
    if (command && !returnCommand)
    {
        PostGuestMessage(owner, GuestAbi::WmCommand,
            GuestAbi::MakeCommandWParam(static_cast<WORD>(command), 0), 0, nullptr);
    }
    SetWin32Error(win32Error, ERROR_SUCCESS);
    return returnCommand ? command : (command ? TRUE : FALSE);
}

BOOL GuestWindowManager::GetGuestWindowRect(HWND window, LPRECT rect, DWORD* win32Error) const
{
    const auto record = FindWindow(window);
    if (!record || !rect)
    {
        SetWin32Error(win32Error, !rect ? ERROR_INVALID_PARAMETER : ERROR_INVALID_WINDOW_HANDLE);
        return FALSE;
    }

    std::lock_guard<std::mutex> guard(record->lock);
    *rect = record->bounds;
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
            *targetLeft = accumulatedLeft;
            *targetTop = SaturatingAdd(accumulatedTop,
                rootMenuBar && window != current ? 22 : 0);
            return width > 0 && height > 0 && *targetWidth > 0 && *targetHeight > 0;
        }

        accumulatedLeft = SaturatingAdd(accumulatedLeft, bounds.left);
        accumulatedTop = SaturatingAdd(accumulatedTop, bounds.top);
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
            snapshot.visible = record->visible;
            snapshot.menuBar = record->menuBar;
        }
        windows.push_back(snapshot);
    }

    std::sort(
        windows.begin(),
        windows.end(),
        [](const HitTestSnapshot& left, const HitTestSnapshot& right)
        {
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

        HWND hit = nullptr;
        for (auto current = windows.rbegin(); current != windows.rend(); ++current)
        {
            if (!current->visible || current->parent != parent ||
                parentX < current->bounds.left || parentY < current->bounds.top ||
                parentX >= current->bounds.right || parentY >= current->bounds.bottom)
            {
                continue;
            }

            const int childX = parentX - current->bounds.left;
            const int childY = parentY - current->bounds.top;
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
        if (rootY < 22)
        {
            // Menu handling owns this non-client strip. Returning the root
            // prevents its toolbar from receiving clicks shifted by the menu.
            return rootWindow;
        }
        rootY -= 22;
    }

    const HWND child = hitTestChildren(rootWindow, rootX, rootY);
    return child ? child : rootWindow;
}

bool GuestWindowManager::HandleGuestMenuPointer(HWND rootWindow, int rootX, int rootY, UINT message)
{
    if (message != GuestAbi::WmLButtonDown && message != GuestAbi::WmLButtonUp)
    {
        return false;
    }
    const auto root = FindWindow(rootWindow);
    if (!root)
    {
        return false;
    }

    PopupMenuSession popup;
    {
        std::lock_guard<std::mutex> guard(m_popupMenuLock);
        popup = m_popupMenu;
    }
    if (popup.open && popup.root == rootWindow)
    {
        const std::vector<GuestMenuVisualItem> items = GetGuestMenuItems(popup.menu);
        constexpr int popupWidth = 220;
        constexpr int popupRowHeight = 20;
        const int popupHeight = static_cast<int>((std::min)(items.size(), static_cast<size_t>(32))) * popupRowHeight;
        const bool inside = rootX >= popup.left && rootX < SaturatingAdd(popup.left, popupWidth) &&
            rootY >= popup.top && rootY < SaturatingAdd(popup.top, popupHeight);
        if (message == GuestAbi::WmLButtonUp)
        {
            UINT command = 0;
            HMENU childMenu = nullptr;
            size_t childIndex = 0;
            if (inside)
            {
                const size_t index = static_cast<size_t>((rootY - popup.top) / popupRowHeight);
                if (index < items.size() && (items[index].state & 0x0002u) == 0)
                {
                    if (items[index].subMenu)
                    {
                        childMenu = items[index].subMenu;
                        childIndex = index;
                    }
                    else if (items[index].identifier != 0)
                    {
                        command = items[index].identifier;
                    }
                }
            }
            if (childMenu)
            {
                {
                    std::lock_guard<std::mutex> guard(m_popupMenuLock);
                    if (m_popupMenu.open && m_popupMenu.root == rootWindow)
                    {
                        m_popupMenu.menu = childMenu;
                        m_popupMenu.left = SaturatingAdd(popup.left, popupWidth - 2);
                        m_popupMenu.top = SaturatingAdd(popup.top,
                            static_cast<int>(childIndex) * popupRowHeight);
                    }
                }
                SendGuestMessage(popup.owner, GuestAbi::WmInitMenuPopup,
                    reinterpret_cast<WPARAM>(childMenu),
                    GuestAbi::MakeCommandWParam(static_cast<WORD>(childIndex), 0), nullptr);
                RuntimeDiagnostics::Record(L"MENU: opened a nested popup submenu.");
                InvalidateGuestRect(rootWindow, nullptr, FALSE, nullptr);
                return true;
            }
            {
                std::lock_guard<std::mutex> guard(m_popupMenuLock);
                if (m_popupMenu.open && m_popupMenu.root == rootWindow)
                {
                    m_popupMenu.selectedCommand = command;
                    m_popupMenu.open = false;
                }
            }
            RuntimeDiagnostics::Record(command ?
                L"MENU: popup selected command " + std::to_wstring(command) + L"." :
                L"MENU: popup dismissed without a command.");
            m_popupMenuChanged.notify_all();
            InvalidateGuestRect(rootWindow, nullptr, FALSE, nullptr);
        }
        return true;
    }
    const std::vector<GuestMenuVisualItem> menuItems = GetGuestMenuBarItems(rootWindow);
    if (menuItems.empty())
    {
        return false;
    }

    int openIndex = -1;
    {
        std::lock_guard<std::mutex> guard(root->lock);
        if (root->destroyed || !root->menuBar)
        {
            return false;
        }
        openIndex = root->openMenuIndex;
    }

    int menuLeft = 8;
    if (rootY >= 0 && rootY < 22)
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
        if (message == GuestAbi::WmLButtonDown)
        {
            HMENU openedSubMenu = nullptr;
            {
                std::lock_guard<std::mutex> guard(root->lock);
                root->openMenuIndex = selected >= 0 && menuItems[static_cast<size_t>(selected)].subMenu
                    ? selected : -1;
                if (root->openMenuIndex >= 0)
                {
                    openedSubMenu = menuItems[static_cast<size_t>(root->openMenuIndex)].subMenu;
                }
            }
            if (openedSubMenu)
            {
                // Let the guest update enabled state and populate dynamic
                // items before the virtual popup is painted, just as USER32
                // does for every top-level menu.
                const HMENU rootMenu = BridgeGetMenu(rootWindow);
                if (rootMenu)
                {
                    SendGuestMessage(rootWindow, GuestAbi::WmInitMenu,
                        reinterpret_cast<WPARAM>(rootMenu), 0, nullptr);
                }
                SendGuestMessage(rootWindow, GuestAbi::WmInitMenuPopup,
                    reinterpret_cast<WPARAM>(openedSubMenu),
                    GuestAbi::MakeCommandWParam(static_cast<WORD>(selected), 0), nullptr);
                RuntimeDiagnostics::Record(L"MENU: initialized top-level submenu " +
                    std::to_wstring(selected) + L".");
            }
            InvalidateGuestRect(rootWindow, nullptr, TRUE, nullptr);
        }
        return true;
    }

    if (openIndex < 0 || static_cast<size_t>(openIndex) >= menuItems.size())
    {
        return false;
    }
    for (int index = 0; index < openIndex; ++index)
    {
        menuLeft = SaturatingAdd(menuLeft, MenuBarItemWidth(menuItems[static_cast<size_t>(index)]));
    }
    const std::vector<GuestMenuVisualItem> popupItems =
        GetGuestMenuItems(menuItems[static_cast<size_t>(openIndex)].subMenu);
    const int popupWidth = 220;
    const int popupRowHeight = 20;
    const int popupHeight = static_cast<int>((std::min)(popupItems.size(), static_cast<size_t>(32))) * popupRowHeight;
    if (rootX >= menuLeft && rootX < SaturatingAdd(menuLeft, popupWidth) &&
        rootY >= 22 && rootY < SaturatingAdd(22, popupHeight))
    {
        if (message == GuestAbi::WmLButtonUp)
        {
            const size_t itemIndex = static_cast<size_t>((rootY - 22) / popupRowHeight);
            if (itemIndex < popupItems.size())
            {
                const GuestMenuVisualItem& item = popupItems[itemIndex];
                if (!item.subMenu && item.identifier != 0 && (item.state & 0x0002u) == 0)
                {
                    // USER32 delivers a normal menu selection to its owner
                    // synchronously. Posting it left 7-Zip's modal command
                    // paths waiting for a message that was never observed.
                    RuntimeDiagnostics::Record(
                        L"MENU: invoking command " + std::to_wstring(item.identifier) +
                        L" from the top-level menu.");
                    SendGuestMessage(rootWindow, GuestAbi::WmCommand,
                        GuestAbi::MakeCommandWParam(static_cast<WORD>(item.identifier), 0), 0, nullptr);
                }
            }
            {
                std::lock_guard<std::mutex> guard(root->lock);
                root->openMenuIndex = -1;
            }
            InvalidateGuestRect(rootWindow, nullptr, TRUE, nullptr);
        }
        return true;
    }

    if (message == GuestAbi::WmLButtonDown)
    {
        {
            std::lock_guard<std::mutex> guard(root->lock);
            root->openMenuIndex = -1;
        }
        InvalidateGuestRect(rootWindow, nullptr, TRUE, nullptr);
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
            Present(record);
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
    bool resizedRebar = false;
    std::vector<RebarBand> rebarBandsToLayout;

    {
        std::lock_guard<std::mutex> guard(record->lock);
        if (record->destroyed)
        {
            SetWin32Error(win32Error, ERROR_INVALID_WINDOW_HANDLE);
            return FALSE;
        }

        const int oldWidth = record->surface.Width();
        const int oldHeight = record->surface.Height();
        const int newWidth = resize ? SetWindowPosExtent(width) : oldWidth;
        int newHeight = resize ? SetWindowPosExtent(height) : oldHeight;
        if (resize && record->windowClass &&
            record->windowClass->builtinKind == BuiltinControlKind::Rebar &&
            !record->rebarBands.empty())
        {
            int bandHeight = 18;
            for (const auto& band : record->rebarBands)
            {
                bandHeight = (std::max)(bandHeight, band.minimumHeight);
            }
            // A rebar is a control strip, not the file-panel client area.
            // Native clients can briefly pass an obsolete, full-panel height
            // during negotiation; retain the normalized band height rather
            // than covering the ListView or accepting zero.
            newHeight = (std::min)(64, (std::max)(18, bandHeight));
        }
        const int newLeft = move ? x : record->bounds.left;
        const int newTop = move ? y : record->bounds.top;

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
        if (resizedRebar)
        {
            rebarBandsToLayout = record->rebarBands;
        }
    }

    if (resizedRebar && !rebarBandsToLayout.empty())
    {
        size_t comboBand = rebarBandsToLayout.size();
        bool addressOnlyBands = true;
        for (size_t index = 0; index < rebarBandsToLayout.size(); ++index)
        {
            const auto child = FindWindow(rebarBandsToLayout[index].child);
            BuiltinControlKind childKind = BuiltinControlKind::None;
            if (child)
            {
                std::lock_guard<std::mutex> childGuard(child->lock);
                childKind = child->windowClass ? child->windowClass->builtinKind : BuiltinControlKind::None;
            }
            if (childKind == BuiltinControlKind::ComboBox)
            {
                comboBand = index;
            }
            else if (childKind != BuiltinControlKind::Static)
            {
                addressOnlyBands = false;
            }
        }
        addressOnlyBands = addressOnlyBands && comboBand < rebarBandsToLayout.size();
        for (size_t index = 0; index < rebarBandsToLayout.size(); ++index)
        {
            const auto child = FindWindow(rebarBandsToLayout[index].child);
            if (child)
            {
                std::lock_guard<std::mutex> childGuard(child->lock);
                const bool staticChild = child->windowClass &&
                    child->windowClass->builtinKind == BuiltinControlKind::Static;
                child->addressBackButton = addressOnlyBands && staticChild && index != comboBand;
            }
        }
        int cursor = 0;
        for (size_t index = 0; index < rebarBandsToLayout.size(); ++index)
        {
            const RebarBand& band = rebarBandsToLayout[index];
            const int remaining = (std::max)(0, currentWidth - cursor);
            const size_t remainingBands = rebarBandsToLayout.size() - index;
            const int rowShare = remainingBands == 0 ? remaining : remaining / static_cast<int>(remainingBands);
            const int preferredWidth = band.width == 0 ? band.minimumWidth : band.width;
            const int childWidth = addressOnlyBands
                ? (index == comboBand ? remaining : (std::min)(26, remaining))
                : (index + 1 == rebarBandsToLayout.size()
                    ? remaining
                    : (std::min)(preferredWidth, (std::max)(24, rowShare)));
            DWORD ignored = ERROR_SUCCESS;
            SetGuestWindowPos(band.child, nullptr, cursor, 0, childWidth,
                currentHeight, GuestSwpNoZOrder, &ignored);
            cursor += (std::max)(0, childWidth);
        }
        RuntimeDiagnostics::Record(L"REBAR: relaid out " +
            std::to_wstring(rebarBandsToLayout.size()) + L" band(s) at " +
            std::to_wstring(currentWidth) + L"x" + std::to_wstring(currentHeight) + L".");
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
            L" -> " + std::to_wstring(currentWidth) + L"x" + std::to_wstring(currentHeight) +
            L" at " + std::to_wstring(currentLeft) + L"," + std::to_wstring(currentTop) + L".");
    }

    if (changedSize)
    {
        PostGuestMessage(
            window,
            GuestAbi::WmSize,
            GuestAbi::SizeRestored,
            GuestAbi::MakeMouseLParam(static_cast<WORD>(currentWidth), static_cast<WORD>(currentHeight)),
            nullptr);
    }

    if ((changedSize || becameVisible || (flags & GuestSwpFrameChanged) != 0) &&
        (flags & GuestSwpNoRedraw) == 0)
    {
        InvalidateGuestRect(window, nullptr, TRUE, nullptr);
        Present(record);
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
                window->listViewSelectedItem = -1;
                window->listViewTopItem = 0;
                window->listViewPressedItem = -1;
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
                if (static_cast<size_t>(item) < window->listViewItemData.size())
                {
                    window->listViewItemData.erase(window->listViewItemData.begin() + item);
                }
                if (window->listViewSelectedItem == item)
                {
                    window->listViewSelectedItem = -1;
                }
                else if (window->listViewSelectedItem > item)
                {
                    --window->listViewSelectedItem;
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
            return window->listViewSelectedItem >= 0 ? 1 : 0;
        }
        if (message == ListViewGetNextItem)
        {
            if ((static_cast<UINT>(lParam) & ListViewNextItemSelected) == 0)
            {
                return -1;
            }
            std::lock_guard<std::mutex> guard(window->lock);
            return window->listViewSelectedItem > static_cast<int>(wParam)
                ? window->listViewSelectedItem
                : -1;
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
            bool selected = false;
            LPARAM itemData = 0;
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
                selected = window->listViewSelectedItem == item;
                if (static_cast<size_t>(item) < window->listViewItemData.size())
                {
                    itemData = window->listViewItemData[static_cast<size_t>(item)];
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
                    request.state = selected ? ListViewStateSelected : 0;
                }
                if ((request.mask & ListViewItemParam) != 0)
                {
                    request.itemData = itemData;
                }
                if (!TryWriteGuestValue(guestItem, request))
                {
                    return FALSE;
                }
                return TRUE;
            }
            return static_cast<LRESULT>(copied);
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
        if (message == ListViewSetImageList || message == ListViewSetColumnOrderArray)
        {
            // Image lists and visual column order are retained by the guest.
            // The first MiniGDI report-view renderer has no icon surface yet,
            // but returning the documented success keeps the standard Wine
            // setup sequence progressing into item population.
            return 0;
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
            return (std::max)(0, (height - 24) / MiniGdi::DefaultTextGlyphHeight);
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
                const int rowsPerPage = (std::max)(1, (height - 24) / MiniGdi::DefaultTextGlyphHeight);
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
        if (message == ListViewScroll)
        {
            // Report view scrolls in pixels in the desktop API.  This
            // retained renderer is row-based, so translate a vertical pixel
            // delta into the smallest whole-row movement that preserves the
            // visible ordering.
            const int verticalPixels = static_cast<int>(lParam);
            int delta = verticalPixels / MiniGdi::DefaultTextGlyphHeight;
            if (verticalPixels != 0 && verticalPixels % MiniGdi::DefaultTextGlyphHeight != 0)
            {
                delta += verticalPixels > 0 ? 1 : -1;
            }
            bool changed = false;
            {
                std::lock_guard<std::mutex> guard(window->lock);
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
        if (message == ListViewSetItemState)
        {
            GuestListViewItemW source = {};
            if (!TryReadGuestValue(reinterpret_cast<const GuestListViewItemW*>(lParam), &source))
            {
                return FALSE;
            }
            if ((source.mask & ListViewItemState) != 0 &&
                (source.stateMask & ListViewStateSelected) != 0)
            {
                {
                    std::lock_guard<std::mutex> guard(window->lock);
                    const bool selected = (source.state & ListViewStateSelected) != 0;
                    const int item = static_cast<int>(wParam);
                    if (item == -1)
                    {
                        window->listViewSelectedItem = selected && !window->listViewItems.empty() ? 0 : -1;
                    }
                    else if (item >= 0 && static_cast<size_t>(item) < window->listViewItems.size())
                    {
                        window->listViewSelectedItem = selected ? item : -1;
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
                if (source.width > 0 && static_cast<size_t>(column) < window->listViewColumnWidths.size())
                {
                    window->listViewColumnWidths[static_cast<size_t>(column)] = source.width;
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
                    std::vector<std::wstring> row(columnCount);
                    row[0] = std::move(itemText);
                    window->listViewItems.insert(window->listViewItems.begin() + position, std::move(row));
                    window->listViewItemData.insert(window->listViewItemData.begin() + position,
                        (source.mask & ListViewItemParam) != 0 ? source.itemData : 0);
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
        if (message == ToolbarSetButtonSize)
        {
            {
                std::lock_guard<std::mutex> guard(window->lock);
                window->toolbarButtonWidth = (std::max)(16, static_cast<int>(LOWORD(lParam)));
                window->toolbarButtonHeight = (std::max)(16, static_cast<int>(HIWORD(lParam)));
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
                int requested = (std::max)(window->toolbarButtonWidth, (std::max)(16, minimum));
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
            copiedButtons.reserve(count);
            for (size_t index = 0; index < count; ++index)
            {
                GuestToolbarButton button = {};
                if (!TryReadGuestValue(buttons + index, &button))
                {
                    return FALSE;
                }
                copiedButtons.push_back(button);
            }
            {
                std::lock_guard<std::mutex> guard(window->lock);
                for (const auto& button : copiedButtons)
                {
                    window->toolbarCommands.push_back(button.command);
                    window->toolbarBitmaps.push_back(button.bitmap);
                    window->toolbarButtonStates.push_back(button.state);
                    window->toolbarButtonStyles.push_back(button.style);
                }
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
                desiredHeight = (std::min)(96, (std::max)(18, window->toolbarButtonHeight + 2));
            }
            const auto parentWindow = FindWindow(parent);
            if (parentWindow)
            {
                int parentWidth = 0;
                bool rebarParent = false;
                {
                    std::lock_guard<std::mutex> parentGuard(parentWindow->lock);
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
                        parentWidth = (std::max)(1, parentWindow->surface.Width());
                    }
                }
                if (rebarParent)
                {
                    DWORD ignored = ERROR_SUCCESS;
                    SetGuestWindowPos(parent, nullptr, 0, 0, parentWidth, desiredHeight,
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
        if (message == ToolbarGetItemRect)
        {
            RECT result = {};
            {
                std::lock_guard<std::mutex> guard(window->lock);
                const int index = static_cast<int>(wParam);
                if (index < 0 || static_cast<size_t>(index) >= window->toolbarCommands.size())
                {
                    return FALSE;
                }
                const int left = ToolbarItemLeft(window->toolbarButtonStyles, window->toolbarBitmaps,
                    static_cast<size_t>(index), window->toolbarButtonWidth);
                result.left = left;
                result.top = 1;
                result.right = left + ToolbarItemWidth(window->toolbarButtonStyles, window->toolbarBitmaps,
                    static_cast<size_t>(index), window->toolbarButtonWidth);
                result.bottom = result.top + (std::max)(16, window->toolbarButtonHeight);
            }
            return TryWriteGuestValue(reinterpret_cast<RECT*>(lParam), result) ? TRUE : FALSE;
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
        const auto band = reinterpret_cast<const GuestRebarBandInfoW*>(lParam);
        if (!band || band->size < offsetof(GuestRebarBandInfoW, width) + sizeof(band->width))
        {
            return FALSE;
        }

        RebarBand updated;
        updated.child = band->child;
        updated.style = band->style;
        // 7-Zip's rebar setup can carry stale pre-layout dimensions from a
        // native HWND (for example 2048x398) while the guest root is only
        // 800x480. They are preferences, not permission to consume the panel.
        // Keep a conservative, control-strip-sized minimum and resolve widths
        // against the actual composed rebar below.
        updated.minimumWidth = (std::min)(1024, (std::max)(24, static_cast<int>(band->minimumChildWidth)));
        // cyMinChild can be a stale full-panel value while the control is
        // being constructed. This bridge renders rebars as a single address
        // strip, so normalize the height instead of trusting that transient
        // native layout value.
        updated.minimumHeight = 26;
        updated.width = band->width == 0 ? 0 : (std::min)(2048,
            (std::max)(updated.minimumWidth, static_cast<int>(band->width)));
        std::vector<RebarBand> bands;
        int rebarWidth = 800;
        int rebarHeight = updated.minimumHeight;
        HWND rebar = nullptr;
        {
            std::lock_guard<std::mutex> guard(window->lock);
            const size_t requestedIndex = static_cast<size_t>(wParam);
            if (message == RebarInsertBandW)
            {
                const size_t insertion = (std::min)(requestedIndex, window->rebarBands.size());
                window->rebarBands.insert(window->rebarBands.begin() + insertion, updated);
            }
            else
            {
                if (requestedIndex >= window->rebarBands.size())
                {
                    return FALSE;
                }
                // RB_SETBANDINFOW can update only a subset of the members.
                // Keep the already hosted child when the caller only changes
                // width or style metadata.
                RebarBand& existing = window->rebarBands[requestedIndex];
                if (updated.child)
                {
                    existing.child = updated.child;
                }
                existing.style = updated.style;
                existing.minimumWidth = updated.minimumWidth;
                existing.minimumHeight = updated.minimumHeight;
                existing.width = updated.width;
            }
            bands = window->rebarBands;
            rebarWidth = (std::max)(24, window->surface.Width());
            for (const auto& current : bands)
            {
                rebarHeight = (std::max)(rebarHeight, current.minimumHeight);
            }
            rebarHeight = (std::min)(64, (std::max)(18, rebarHeight));
            rebar = window->handle;
        }
        size_t comboBand = bands.size();
        bool addressOnlyBands = !bands.empty();
        for (size_t index = 0; index < bands.size(); ++index)
        {
            const auto child = FindWindow(bands[index].child);
            BuiltinControlKind childKind = BuiltinControlKind::None;
            if (child)
            {
                std::lock_guard<std::mutex> childGuard(child->lock);
                childKind = child->windowClass ? child->windowClass->builtinKind : BuiltinControlKind::None;
            }
            if (childKind == BuiltinControlKind::ComboBox)
            {
                comboBand = index;
            }
            else if (childKind != BuiltinControlKind::Static)
            {
                addressOnlyBands = false;
            }
        }
        addressOnlyBands = addressOnlyBands && comboBand < bands.size();
        for (size_t index = 0; index < bands.size(); ++index)
        {
            const auto child = FindWindow(bands[index].child);
            if (child)
            {
                std::lock_guard<std::mutex> childGuard(child->lock);
                const bool staticChild = child->windowClass &&
                    child->windowClass->builtinKind == BuiltinControlKind::Static;
                child->addressBackButton = addressOnlyBands && staticChild && index != comboBand;
            }
        }
        int cursor = 0;
        for (size_t index = 0; index < bands.size(); ++index)
        {
            const RebarBand& current = bands[index];
            const int remaining = (std::max)(0, rebarWidth - cursor);
            const size_t remainingBands = bands.size() - index;
            const int rowShare = remainingBands == 0 ? remaining : remaining / static_cast<int>(remainingBands);
            const int preferredWidth = current.width == 0 ? current.minimumWidth : current.width;
            // Keep every band on the one supported row. In particular, this
            // reserves the trailing space for the ComboBoxEx address field
            // instead of allowing an earlier helper/static band to push it
            // thousands of pixels beyond the visible rebar.
            const int childWidth = addressOnlyBands
                ? (index == comboBand ? remaining : (std::min)(26, remaining))
                : (index + 1 == bands.size()
                    ? remaining
                    : (std::min)(preferredWidth, (std::max)(24, rowShare)));
            const auto child = FindWindow(current.child);
            if (child)
            {
                // A rebar owns the hosted child. Reparent it in the virtual
                // tree so software composition composites every band above
                // the rebar background in its documented order.
                std::lock_guard<std::mutex> childGuard(child->lock);
                child->parent = rebar;
            }
            DWORD ignored = ERROR_SUCCESS;
            if (current.child)
            {
                SetGuestWindowPos(current.child, nullptr, cursor, 0, childWidth,
                    rebarHeight, GuestSwpNoZOrder, &ignored);
            }
            cursor += (std::max)(0, childWidth);
        }
        DWORD ignored = ERROR_SUCCESS;
        SetGuestWindowPos(rebar, nullptr, 0, 0, rebarWidth, rebarHeight, GuestSwpNoZOrder, &ignored);
        RuntimeDiagnostics::Record(L"REBAR: inserted or updated " + std::to_wstring(bands.size()) + L" band(s).");
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
        if (message == RebarGetBarHeight || message == RebarGetRowHeight)
        {
            std::lock_guard<std::mutex> guard(window->lock);
            int height = 24;
            for (const auto& band : window->rebarBands)
            {
                height = (std::max)(height, band.minimumHeight);
            }
            return height;
        }
        if (message == RebarSizeToRect)
        {
            RECT requested = {};
            if (!TryReadGuestValue(reinterpret_cast<const RECT*>(lParam), &requested))
            {
                return FALSE;
            }
            int height = 24;
            {
                std::lock_guard<std::mutex> guard(window->lock);
                for (const auto& band : window->rebarBands)
                {
                    height = (std::max)(height, band.minimumHeight);
                }
            }
            requested.top += height;
            return TryWriteGuestValue(reinterpret_cast<RECT*>(lParam), requested) ? TRUE : FALSE;
        }
    }

    if ((controlKind == BuiltinControlKind::Toolbar ||
        controlKind == BuiltinControlKind::Rebar ||
        controlKind == BuiltinControlKind::ListView) && message >= 0x0400)
    {
        RuntimeDiagnostics::Record(
            L"COMMON CONTROL MESSAGE: handle " +
            std::to_wstring(reinterpret_cast<ULONG_PTR>(window->handle)) + L", code " +
            std::to_wstring(message) + L".");
    }

    // Parent notifications intentionally snapshot their values before calling
    // into guest code. WM_COMMAND can destroy or reparent this control, so no
    // WindowRecord field may be touched after SendGuestMessage returns.
    const auto notifyParent = [this, window](WORD notification)
    {
        HWND parent = nullptr;
        HWND handle = nullptr;
        UINT_PTR identifier = 0;
        {
            std::lock_guard<std::mutex> guard(window->lock);
            if (window->destroyed)
            {
                return;
            }
            parent = window->parent;
            handle = window->handle;
            identifier = window->controlId;
        }
        if (parent)
        {
            SendGuestMessage(
                parent,
                GuestAbi::WmCommand,
                GuestAbi::MakeCommandWParam(
                    static_cast<WORD>(identifier),
                    notification),
                reinterpret_cast<LPARAM>(handle),
                nullptr);
        }
    };

    const auto invalidate = [this, window]()
    {
        HWND handle = nullptr;
        {
            std::lock_guard<std::mutex> guard(window->lock);
            if (window->destroyed)
            {
                return;
            }
            handle = window->handle;
        }
        InvalidateGuestRect(handle, nullptr, TRUE, nullptr);
    };

    switch (message)
    {
    case GuestAbi::WmNcCreate:
        return TRUE;
    case GuestAbi::WmNcHitTest:
        return GuestAbi::HtClient;
    case GuestAbi::WmEraseBkgnd:
        // BeginPaint owns the class-brush fallback. Returning zero gives it
        // the same negotiation as DefWindowProc without double-erasing.
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
        std::lock_guard<std::mutex> guard(window->lock);
        return static_cast<LRESULT>((std::min)(
            window->title.size(),
            static_cast<size_t>((std::numeric_limits<LRESULT>::max)())));
    }
    case GuestAbi::WmGetText:
    {
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
            window->title = std::move(text);
            window->editCaret = window->title.size();
        }
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
        {
            std::lock_guard<std::mutex> guard(window->lock);
            if (window->destroyed)
            {
                return 0;
            }
            window->controlFont = requested;
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
        std::wstring text;
        DWORD style = 0;
        bool enabled = false;
        bool pressed = false;
        size_t caret = 0;
        MiniGdi::ObjectHandle font = MiniGdi::InvalidObject;
        std::vector<std::wstring> listColumns;
        std::vector<int> listColumnWidths;
        std::vector<std::vector<std::wstring>> listItems;
        std::vector<LPARAM> listViewItemData;
        COLORREF listViewBackgroundColor = 0x00ffffff;
        COLORREF listViewTextColor = 0x00000000;
        COLORREF listViewTextBackgroundColor = 0x00ffffff;
        UINT listViewExtendedStyle = 0;
        int listViewSelectedItem = -1;
        int listViewTopItem = 0;
        std::vector<int> toolbarCommands;
        std::vector<int> toolbarBitmaps;
        std::vector<BYTE> toolbarButtonStates;
        std::vector<BYTE> toolbarButtonStyles;
        HANDLE toolbarImageList = nullptr;
        int toolbarButtonWidth = 24;
        int toolbarButtonHeight = 24;
        int toolbarBitmapWidth = 16;
        int toolbarBitmapHeight = 16;
        int toolbarPressedIndex = -1;
        bool addressBackButton = false;
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
            pressed = window->buttonPressed || window->buttonKeyboardPressed;
            caret = window->editCaret;
            font = window->controlFont;
            listColumns = window->listViewColumns;
            listColumnWidths = window->listViewColumnWidths;
            listItems = window->listViewItems;
            listViewItemData = window->listViewItemData;
            listViewBackgroundColor = window->listViewBackgroundColor;
            listViewTextColor = window->listViewTextColor;
            listViewTextBackgroundColor = window->listViewTextBackgroundColor;
            listViewExtendedStyle = window->listViewExtendedStyle;
            listViewSelectedItem = window->listViewSelectedItem;
            listViewTopItem = window->listViewTopItem;
            toolbarCommands = window->toolbarCommands;
            toolbarBitmaps = window->toolbarBitmaps;
            toolbarButtonStates = window->toolbarButtonStates;
            toolbarButtonStyles = window->toolbarButtonStyles;
            toolbarImageList = window->toolbarImageList;
            toolbarButtonWidth = window->toolbarButtonWidth;
            toolbarButtonHeight = window->toolbarButtonHeight;
            toolbarBitmapWidth = window->toolbarBitmapWidth;
            toolbarBitmapHeight = window->toolbarBitmapHeight;
            toolbarPressedIndex = window->toolbarPressedIndex;
            addressBackButton = window->addressBackButton;
            handle = window->handle;
            parent = window->parent;
            controlId = window->controlId;
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
                    ? (pressed ? MiniGdi::MakeColor(214, 214, 214) : MiniGdi::MakeColor(240, 240, 240))
                    : MiniGdi::MakeColor(235, 235, 235);
                MiniGdi::DrawRectangle(
                    *surface,
                    fullRect,
                    fill,
                    focused ? MiniGdi::MakeColor(0, 120, 215) : MiniGdi::MakeColor(96, 96, 96));
            }
            else if (controlKind == BuiltinControlKind::Static && addressBackButton)
            {
                MiniGdi::DrawRectangle(
                    *surface,
                    fullRect,
                    focused ? MiniGdi::MakeColor(225, 235, 245) : MiniGdi::MakeColor(240, 240, 240),
                    MiniGdi::MakeColor(128, 128, 128));
            }
            else if (controlKind == BuiltinControlKind::Edit)
            {
                MiniGdi::DrawRectangle(
                    *surface,
                    fullRect,
                    MiniGdi::OpaqueWhite,
                    focused ? MiniGdi::MakeColor(0, 120, 215) : MiniGdi::MakeColor(128, 128, 128));
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
                        MiniGdi::Rect{ 1, 1, (std::max)(1, width - 1), (std::min)(22, (std::max)(1, height - 1)) },
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
            else if (controlKind == BuiltinControlKind::ComboBox ||
                controlKind == BuiltinControlKind::ScrollBar ||
                controlKind == BuiltinControlKind::UpDown)
            {
                MiniGdi::DrawRectangle(
                    *surface,
                    fullRect,
                    MiniGdi::OpaqueWhite,
                    MiniGdi::MakeColor(128, 128, 128));
                const int buttonWidth = (std::min)(20, (std::max)(1, width / 3));
                MiniGdi::FillRect(
                    *surface,
                    MiniGdi::Rect{ (std::max)(0, width - buttonWidth), 1, (std::max)(0, width - 1), (std::max)(1, height - 1) },
                    MiniGdi::MakeColor(240, 240, 240));
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

            const size_t visibleCharacters = static_cast<size_t>((std::max)(0, width / MiniGdi::DefaultTextGlyphWidth));
            const int visibleTextWidth = static_cast<int>(
                (std::min)(text.size(), visibleCharacters) * MiniGdi::DefaultTextGlyphWidth);
            int textX = 2;
            int textY = (std::max)(0, (height - MiniGdi::DefaultTextGlyphHeight) / 2);
            if (controlKind == BuiltinControlKind::Static)
            {
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
                if (alignment == GuestAbi::EsCenter)
                {
                    textX = (std::max)(2, (width - visibleTextWidth) / 2);
                }
                else if (alignment == GuestAbi::EsRight)
                {
                    textX = (std::max)(2, width - visibleTextWidth - 2);
                }
            }

            m_gdi.TextOutW(guestDc, MiniGdi::Point{ textX, textY }, text.data(), text.size(), nullptr);

            if (controlKind == BuiltinControlKind::Static && addressBackButton)
            {
                DrawToolbarGlyph(*surface, MiniGdi::Rect{ 3, 3, (std::max)(4, width - 3),
                    (std::max)(4, height - 3) }, 0);
            }

            if (controlKind == BuiltinControlKind::Toolbar && !toolbarCommands.empty())
            {
                const int buttonWidth = (std::max)(16, toolbarButtonWidth);
                const int buttonHeight = (std::min)(height - 2, (std::max)(16, toolbarButtonHeight));
                for (size_t index = 0; index < toolbarCommands.size(); ++index)
                {
                    const BYTE buttonStyle = index < toolbarButtonStyles.size() ? toolbarButtonStyles[index] : 0;
                    const BYTE buttonState = index < toolbarButtonStates.size() ? toolbarButtonStates[index] : ToolbarStateEnabled;
                    const int itemWidth = ToolbarItemWidth(toolbarButtonStyles, toolbarBitmaps, index, buttonWidth);
                    const int left = ToolbarItemLeft(toolbarButtonStyles, toolbarBitmaps, index, buttonWidth);
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
                    MiniGdi::DrawRectangle(
                        *surface,
                        MiniGdi::Rect{ left, 1, (std::min)(width - 2, left + itemWidth), 1 + buttonHeight },
                        static_cast<int>(index) == toolbarPressedIndex
                            ? MiniGdi::MakeColor(214, 214, 214)
                            : (buttonEnabled ? MiniGdi::MakeColor(248, 248, 248) : MiniGdi::MakeColor(232, 232, 232)),
                        buttonEnabled ? MiniGdi::MakeColor(128, 128, 128) : MiniGdi::MakeColor(184, 184, 184));
                    MiniGdi::Surface image;
                    const int bitmapIndex = index < toolbarBitmaps.size()
                        ? toolbarBitmaps[index] : static_cast<int>(index);
                    const int iconWidth = (std::min)(itemWidth - 4, (std::max)(1, toolbarBitmapWidth));
                    const int iconHeight = (std::min)(buttonHeight - 4, (std::max)(1, toolbarBitmapHeight));
                    const MiniGdi::Rect iconRect{ left + (itemWidth - iconWidth) / 2, 1 + (buttonHeight - iconHeight) / 2,
                        left + (itemWidth - iconWidth) / 2 + iconWidth, 1 + (buttonHeight - iconHeight) / 2 + iconHeight };
                    if (toolbarImageList && bitmapIndex >= 0 &&
                        CopyGuestImageListImage(toolbarImageList, bitmapIndex, &image) && !image.Empty())
                    {
                        DrawToolbarImage(*surface, iconRect, image);
                    }
                    else
                    {
                        DrawToolbarGlyph(*surface, iconRect, bitmapIndex);
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
                for (size_t index = 0; index < listColumns.size(); ++index)
                {
                    const int columnWidth = index < listColumnWidths.size()
                        ? (std::max)(24, listColumnWidths[index])
                        : 120;
                    const size_t visibleColumnCharacters = static_cast<size_t>((std::max)(0,
                        (columnWidth - 4) / MiniGdi::DefaultTextGlyphWidth));
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
                const size_t visibleRows = static_cast<size_t>((std::max)(0, (height - 24) / MiniGdi::DefaultTextGlyphHeight));
                const size_t firstVisible = static_cast<size_t>((std::max)(0, listViewTopItem));
                for (size_t displayIndex = 0;
                    firstVisible + displayIndex < listItems.size() && displayIndex < visibleRows;
                    ++displayIndex)
                {
                    const size_t index = firstVisible + displayIndex;
                    const int rowTop = 24 + static_cast<int>(displayIndex) * MiniGdi::DefaultTextGlyphHeight;
                    const bool selected = static_cast<int>(index) == listViewSelectedItem;
                    if (selected)
                    {
                        MiniGdi::FillRect(
                            *surface,
                            MiniGdi::Rect{ 1, rowTop, (std::max)(1, width - 1), rowTop + MiniGdi::DefaultTextGlyphHeight },
                            MiniGdi::MakeColor(0, 120, 215));
                        m_gdi.SetTextColor(guestDc, MiniGdi::OpaqueWhite, nullptr);
                    }
                    else
                    {
                        // Separate text-background color is a real ListView
                        // setting; respecting it avoids white text artefacts
                        // when 7-Zip switches views or high-contrast colors.
                        MiniGdi::FillRect(
                            *surface,
                            MiniGdi::Rect{ 1, rowTop, (std::max)(1, width - 1), rowTop + MiniGdi::DefaultTextGlyphHeight },
                            ColorFromGuestColorRef(listViewTextBackgroundColor));
                    }
                    int cellLeft = 2;
                    const size_t cellCount = (std::max)(
                        static_cast<size_t>(1),
                        (std::max)(listItems[index].size(), listColumns.size()));
                    for (size_t subItem = 0; subItem < cellCount; ++subItem)
                    {
                        if (cellLeft >= width - 2)
                        {
                            break;
                        }
                        std::wstring callbackText;
                        const std::wstring emptyCell;
                        const std::wstring* cellText = subItem < listItems[index].size()
                            ? &listItems[index][subItem]
                            : &emptyCell;
                        if (cellText->empty() && parent)
                        {
                            // Wine's list-view asks its owner for virtual
                            // text through LVN_GETDISPINFOW.  7-Zip uses this
                            // owner-data path instead of inserting a string
                            // for every file, so without this request the
                            // report view has rows but no captions.
                            wchar_t scratch[260] = {};
                            GuestListViewDisplayInfoW notification = {};
                            notification.header.from = handle;
                            notification.header.identifier = controlId;
                            notification.header.code = ListViewNotifyGetDisplayInfoW;
                            notification.item.mask = ListViewItemText;
                            notification.item.item = static_cast<int>(index);
                            notification.item.subItem = static_cast<int>(subItem);
                            notification.item.text = scratch;
                            notification.item.textCapacity = static_cast<int>(_countof(scratch));
                            notification.item.itemData = index < listViewItemData.size()
                                ? listViewItemData[index]
                                : 0;
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
                        const size_t visibleCellCharacters = static_cast<size_t>((std::max)(0,
                            (cellWidth - 4) / MiniGdi::DefaultTextGlyphWidth));
                        const size_t cellCharacters = (std::min)(cellText->size(), visibleCellCharacters);
                        m_gdi.TextOutW(
                            guestDc,
                            MiniGdi::Point{ cellLeft + 2, rowTop },
                            cellText->data(),
                            cellCharacters,
                            nullptr);
                        cellLeft += cellWidth;
                    }
                    if (selected)
                    {
                        m_gdi.SetTextColor(guestDc, textColor, nullptr);
                    }
                    if ((listViewExtendedStyle & ListViewExtendedGridLines) != 0)
                    {
                        MiniGdi::DrawLine(
                            *surface,
                            MiniGdi::Point{ 1, rowTop + MiniGdi::DefaultTextGlyphHeight - 1 },
                            MiniGdi::Point{ (std::max)(1, width - 2), rowTop + MiniGdi::DefaultTextGlyphHeight - 1 },
                            MiniGdi::MakeColor(208, 208, 208));
                    }
                }
            }

            if ((controlKind == BuiltinControlKind::Edit || controlKind == BuiltinControlKind::ComboBox) && focused && enabled)
            {
                const size_t clampedCaret = (std::min)(caret, text.size());
                const int caretX = (std::min)(
                    (std::max)(1, width - 2),
                    textX + static_cast<int>((std::min)(
                        clampedCaret,
                        static_cast<size_t>((std::numeric_limits<int>::max)() / MiniGdi::DefaultTextGlyphWidth)) *
                        MiniGdi::DefaultTextGlyphWidth));
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
            const int x = static_cast<int>(static_cast<WORD>(lParam & 0xffff));
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
                        index, window->toolbarButtonWidth);
                    const int right = left + ToolbarItemWidth(window->toolbarButtonStyles, window->toolbarBitmaps,
                        index, window->toolbarButtonWidth);
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
                // not the toolbar's child identifier. This is how 7-Zip maps
                // its navigation and file-operation buttons.
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

    case BuiltinControlKind::ComboBox:
        if (message == GuestAbi::WmLButtonDown)
        {
            const int x = static_cast<int>(static_cast<WORD>(lParam & 0xffff));
            HWND handle = nullptr;
            {
                std::lock_guard<std::mutex> guard(window->lock);
                if (window->destroyed || !window->enabled)
                {
                    return 0;
                }
                const size_t requested = x <= 3
                    ? 0
                    : static_cast<size_t>((x - 2 + MiniGdi::DefaultTextGlyphWidth / 2) /
                        MiniGdi::DefaultTextGlyphWidth);
                window->editCaret = (std::min)(requested, window->title.size());
                handle = window->handle;
            }
            SetGuestFocus(handle, nullptr);
            invalidate();
            return 0;
        }
        if ((message == GuestAbi::WmLButtonUp &&
                [&]()
                {
                    std::lock_guard<std::mutex> guard(window->lock);
                    const int x = static_cast<int>(static_cast<WORD>(lParam & 0xffff));
                    return x >= (std::max)(0, window->surface.Width() - 20);
                }()) ||
            (message == GuestAbi::WmKeyDown &&
                (wParam == GuestAbi::VkUp || wParam == GuestAbi::VkDown)))
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
            }
            if (changed)
            {
                invalidate();
                // CBN_SELCHANGE. Returning the ComboBoxEx itself from
                // CBEM_GETCOMBOCONTROL makes this equally useful to clients
                // that wire the address bar through the inner combo handle.
                notifyParent(1);
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
                if (wParam == GuestAbi::VkBack)
                {
                    if (window->editCaret != 0)
                    {
                        window->title.erase(window->editCaret - 1, 1);
                        --window->editCaret;
                        changed = true;
                    }
                }
                else if (wParam >= 0x20 && wParam <= 0xfffd && wParam != 0x7f &&
                    window->title.size() < MaximumBuiltinControlTextLength)
                {
                    window->title.insert(window->editCaret, 1, static_cast<wchar_t>(wParam));
                    ++window->editCaret;
                    changed = true;
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
            {
                std::lock_guard<std::mutex> guard(window->lock);
                if (window->destroyed || !window->enabled)
                {
                    return 0;
                }
                window->editCaret = (std::min)(window->editCaret, window->title.size());
                switch (wParam)
                {
                case GuestAbi::VkLeft:
                    if (window->editCaret != 0) { --window->editCaret; redraw = true; }
                    break;
                case GuestAbi::VkRight:
                    if (window->editCaret < window->title.size()) { ++window->editCaret; redraw = true; }
                    break;
                case GuestAbi::VkHome:
                    if (window->editCaret != 0) { window->editCaret = 0; redraw = true; }
                    break;
                case GuestAbi::VkEnd:
                    if (window->editCaret != window->title.size()) { window->editCaret = window->title.size(); redraw = true; }
                    break;
                case GuestAbi::VkBack:
                    if (window->editCaret != 0)
                    {
                        window->title.erase(window->editCaret - 1, 1);
                        --window->editCaret;
                        redraw = changed = true;
                    }
                    break;
                case GuestAbi::VkDelete:
                    if (window->editCaret < window->title.size())
                    {
                        window->title.erase(window->editCaret, 1);
                        redraw = changed = true;
                    }
                    break;
                default:
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
        switch (message)
        {
        case GuestAbi::WmLButtonDown:
        {
            const int x = static_cast<int>(static_cast<WORD>(lParam & 0xffff));
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
                item = y < 24 ? -1 : window->listViewTopItem +
                    (y - 24) / MiniGdi::DefaultTextGlyphHeight;
                if (item >= 0 && static_cast<size_t>(item) < window->listViewItems.size())
                {
                    previousItem = window->listViewSelectedItem;
                    changed = previousItem != item;
                    window->listViewSelectedItem = item;
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
                item = y < 24 ? -1 : window->listViewTopItem +
                    (y - 24) / MiniGdi::DefaultTextGlyphHeight;
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
            // 7-Zip builds its file/folder menu from WM_CONTEXTMENU; merely
            // forwarding WM_RBUTTONUP leaves that entire path dormant.
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
                RuntimeDiagnostics::Record(L"LISTVIEW: forwarding WM_CONTEXTMENU to its owner.");
                SendGuestMessage(parent, GuestAbi::WmContextMenu,
                    reinterpret_cast<WPARAM>(handle), lParam, nullptr);
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
                    window->listViewSelectedItem = item;
                    const int height = (std::max)(0, static_cast<int>(window->bounds.bottom - window->bounds.top));
                    const int rowsPerPage = (std::max)(1, (height - 24) / MiniGdi::DefaultTextGlyphHeight);
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
        case GuestAbi::WmKillFocus:
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
            const int x = static_cast<int>(static_cast<WORD>(lParam & 0xffff));
            HWND handle = nullptr;
            {
                std::lock_guard<std::mutex> guard(window->lock);
                if (window->destroyed || !window->enabled)
                {
                    return 0;
                }
                const int firstCharacterCenter = 2 + MiniGdi::DefaultTextGlyphWidth / 2;
                const size_t requested = x <= firstCharacterCenter
                    ? 0
                    : static_cast<size_t>((x - 2 + MiniGdi::DefaultTextGlyphWidth / 2) /
                        MiniGdi::DefaultTextGlyphWidth);
                window->editCaret = (std::min)(requested, window->title.size());
                handle = window->handle;
            }
            SetGuestFocus(handle, nullptr);
            invalidate();
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
                if (wParam == GuestAbi::VkBack)
                {
                    if (window->editCaret != 0)
                    {
                        window->title.erase(window->editCaret - 1, 1);
                        --window->editCaret;
                        changed = true;
                    }
                }
                else if (wParam >= 0x20 && wParam <= 0xfffd && wParam != 0x7f &&
                    window->title.size() < MaximumBuiltinControlTextLength)
                {
                    window->title.insert(window->editCaret, 1, static_cast<wchar_t>(wParam));
                    ++window->editCaret;
                    changed = true;
                }
            }
            if (changed)
            {
                invalidate();
                notifyParent(GuestAbi::EnChange);
            }
            return 0;
        }
        case GuestAbi::WmKeyDown:
        {
            bool redraw = false;
            bool changed = false;
            {
                std::lock_guard<std::mutex> guard(window->lock);
                if (window->destroyed || !window->enabled)
                {
                    return 0;
                }
                window->editCaret = (std::min)(window->editCaret, window->title.size());
                switch (wParam)
                {
                case GuestAbi::VkLeft:
                    if (window->editCaret != 0)
                    {
                        --window->editCaret;
                        redraw = true;
                    }
                    break;
                case GuestAbi::VkRight:
                    if (window->editCaret < window->title.size())
                    {
                        ++window->editCaret;
                        redraw = true;
                    }
                    break;
                case GuestAbi::VkHome:
                    if (window->editCaret != 0)
                    {
                        window->editCaret = 0;
                        redraw = true;
                    }
                    break;
                case GuestAbi::VkEnd:
                    if (window->editCaret != window->title.size())
                    {
                        window->editCaret = window->title.size();
                        redraw = true;
                    }
                    break;
                case GuestAbi::VkBack:
                    if ((window->style & GuestAbi::EsReadOnly) == 0 && window->editCaret != 0)
                    {
                        window->title.erase(window->editCaret - 1, 1);
                        --window->editCaret;
                        redraw = true;
                        changed = true;
                    }
                    break;
                case GuestAbi::VkDelete:
                    if ((window->style & GuestAbi::EsReadOnly) == 0 && window->editCaret < window->title.size())
                    {
                        window->title.erase(window->editCaret, 1);
                        redraw = true;
                        changed = true;
                    }
                    break;
                default:
                    break;
                }
            }
            if (redraw)
            {
                invalidate();
            }
            if (changed)
            {
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
        if (message == GuestAbi::WmLButtonDown)
        {
            HWND handle = nullptr;
            bool backButton = false;
            {
                std::lock_guard<std::mutex> guard(window->lock);
                backButton = !window->destroyed && window->enabled && window->addressBackButton;
                if (backButton)
                {
                    window->buttonPressed = true;
                    handle = window->handle;
                }
            }
            if (backButton)
            {
                // Preserve focus in the report view: 7-Zip's keyboard Back
                // is processed by the active view rather than by this static
                // ReBar child.
                SetGuestCapture(handle, nullptr);
                invalidate();
            }
            return 0;
        }
        if (message == GuestAbi::WmLButtonUp)
        {
            HWND handle = nullptr;
            bool clicked = false;
            {
                std::lock_guard<std::mutex> guard(window->lock);
                if (!window->destroyed && window->addressBackButton)
                {
                    clicked = window->buttonPressed;
                    window->buttonPressed = false;
                    handle = window->handle;
                }
            }
            if (handle)
            {
                DWORD ignored = ERROR_SUCCESS;
                if (GetGuestCapture(&ignored) == handle)
                {
                    ReleaseGuestCapture(&ignored);
                }
                invalidate();
            }
            if (clicked)
            {
                // A static has no intrinsic action. Preserve the standard
                // BUTTON-like notification and let the owning guest decide
                // what this affordance means; no application command IDs are
                // embedded in the generic window layer.
                notifyParent(GuestAbi::BnClicked);
            }
            return 0;
        }
        if (message == GuestAbi::WmCaptureChanged)
        {
            bool changed = false;
            {
                std::lock_guard<std::mutex> guard(window->lock);
                changed = !window->destroyed && window->buttonPressed;
                window->buttonPressed = false;
            }
            if (changed)
            {
                invalidate();
            }
            return 0;
        }
        if (message == GuestAbi::WmSetFocus || message == GuestAbi::WmKillFocus || message == GuestAbi::WmEnable)
        {
            invalidate();
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
    case GuestAbi::WmEraseBkgnd:
        // BeginPaint performs the class-brush fallback after the guest gets a
        // chance to handle WM_ERASEBKGND. Returning zero here preserves that
        // normal DefWindowProc negotiation without pretending a background
        // was painted when it was not.
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

    if (message->message == GuestAbi::WmKeyDown && message->hwnd && message->wParam >= 0x20 && message->wParam <= 0x7e)
    {
        return PostGuestMessage(message->hwnd, GuestAbi::WmChar, message->wParam, message->lParam, nullptr);
    }
    return FALSE;
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
    Present(record);
    RuntimeDiagnostics::Record(
        L"PAINT PRESENTED: handle " +
        std::to_wstring(reinterpret_cast<ULONG_PTR>(window)) + L".");
    SetWin32Error(win32Error, ERROR_SUCCESS);
    return TRUE;
}

HDC GuestWindowManager::GetGuestDC(HWND window, DWORD* win32Error)
{
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
    const auto record = FindWindow(window);
    if (!record || FromGuestDc(dc) != record->dc)
    {
        SetWin32Error(win32Error, ERROR_INVALID_HANDLE);
        return 0;
    }
    Present(record);
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
        const int menuHeight = menuItems.empty() ? 0 : (std::min)(22, composite.Height());
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
                    if (static_cast<int>(&item - menuItems.data()) == openMenuIndex)
                    {
                        MiniGdi::FillRect(composite,
                            MiniGdi::Rect{ left, 1, (std::min)(composite.Width(), SaturatingAdd(left, itemWidth)), menuHeight - 1 },
                            MiniGdi::MakeColor(214, 226, 242));
                    }
                    if ((item.state & 0x0002u) == 0)
                    {
                        m_gdi.TextOutW(menuDc, MiniGdi::Point{ left + 6, 3 }, caption.data(), caption.size(), nullptr);
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

                const int childX = SaturatingAdd(parentX, child.bounds.left);
                const int childY = SaturatingAdd(
                    SaturatingAdd(parentY, child.bounds.top),
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
                            child.record->surface.Bounds());
                    }
                }
                if (copied)
                {
                    composeChildren(childIndex, childX, childY);
                }
            }
        };
        composeChildren(root, 0, 0);

        if (openMenuIndex >= 0 && static_cast<size_t>(openMenuIndex) < menuItems.size() &&
            menuItems[static_cast<size_t>(openMenuIndex)].subMenu)
        {
            int popupLeft = 8;
            for (int index = 0; index < openMenuIndex; ++index)
            {
                popupLeft = SaturatingAdd(popupLeft, MenuBarItemWidth(menuItems[static_cast<size_t>(index)]));
            }
            const std::vector<GuestMenuVisualItem> popupItems =
                GetGuestMenuItems(menuItems[static_cast<size_t>(openMenuIndex)].subMenu);
            const int rowHeight = 20;
            const int popupHeight = (std::min)(static_cast<int>(popupItems.size()) * rowHeight,
                (std::max)(0, composite.Height() - menuHeight));
            const int popupRight = (std::min)(composite.Width(), SaturatingAdd(popupLeft, 220));
            if (popupHeight > 0 && popupRight > popupLeft)
            {
                MiniGdi::DrawRectangle(composite,
                    MiniGdi::Rect{ popupLeft, menuHeight, popupRight, SaturatingAdd(menuHeight, popupHeight) },
                    MiniGdi::MakeColor(250, 250, 250), MiniGdi::MakeColor(96, 96, 96));
                const MiniGdi::DcHandle popupDc = m_gdi.CreateDc(&composite);
                if (popupDc != MiniGdi::InvalidDc)
                {
                    MiniGdi::Color ignored = MiniGdi::OpaqueBlack;
                    m_gdi.SetTextColor(popupDc, MiniGdi::OpaqueBlack, &ignored);
                    MiniGdi::BackgroundMode ignoredMode = MiniGdi::BackgroundMode::Opaque;
                    m_gdi.SetBackgroundMode(popupDc, MiniGdi::BackgroundMode::Transparent, &ignoredMode);
                    for (size_t index = 0; index < popupItems.size() &&
                        static_cast<int>(index) * rowHeight < popupHeight; ++index)
                    {
                        const int top = SaturatingAdd(menuHeight, static_cast<int>(index) * rowHeight);
                        const GuestMenuVisualItem& item = popupItems[index];
                        if (item.identifier == 0 && item.text.empty())
                        {
                            MiniGdi::DrawLine(composite, MiniGdi::Point{ popupLeft + 4, top + rowHeight / 2 },
                                MiniGdi::Point{ popupRight - 4, top + rowHeight / 2 }, MiniGdi::MakeColor(192, 192, 192));
                        }
                        else if ((item.state & 0x0002u) == 0)
                        {
                            const std::wstring caption = MenuCaptionForDisplay(item.text);
                            m_gdi.TextOutW(popupDc, MiniGdi::Point{ popupLeft + 8, top + 2 },
                                caption.data(), caption.size(), nullptr);
                        }
                    }
                    m_gdi.DestroyDc(popupDc);
                }
            }
        }

        PopupMenuSession contextPopup;
        {
            std::lock_guard<std::mutex> guard(m_popupMenuLock);
            contextPopup = m_popupMenu;
        }
        if (contextPopup.open && contextPopup.root == snapshots[root].handle)
        {
            const std::vector<GuestMenuVisualItem> popupItems = GetGuestMenuItems(contextPopup.menu);
            const int rowHeight = 20;
            const int popupHeight = (std::min)(static_cast<int>(popupItems.size()) * rowHeight,
                (std::max)(0, composite.Height() - contextPopup.top));
            const int popupRight = (std::min)(composite.Width(), SaturatingAdd(contextPopup.left, 220));
            if (popupHeight > 0 && popupRight > contextPopup.left)
            {
                MiniGdi::DrawRectangle(composite,
                    MiniGdi::Rect{ contextPopup.left, contextPopup.top, popupRight,
                        SaturatingAdd(contextPopup.top, popupHeight) },
                    MiniGdi::MakeColor(250, 250, 250), MiniGdi::MakeColor(72, 72, 72));
                const MiniGdi::DcHandle popupDc = m_gdi.CreateDc(&composite);
                if (popupDc != MiniGdi::InvalidDc)
                {
                    MiniGdi::Color ignored = MiniGdi::OpaqueBlack;
                    m_gdi.SetTextColor(popupDc, MiniGdi::OpaqueBlack, &ignored);
                    MiniGdi::BackgroundMode ignoredMode = MiniGdi::BackgroundMode::Opaque;
                    m_gdi.SetBackgroundMode(popupDc, MiniGdi::BackgroundMode::Transparent, &ignoredMode);
                    for (size_t index = 0; index < popupItems.size() &&
                        static_cast<int>(index) * rowHeight < popupHeight; ++index)
                    {
                        const GuestMenuVisualItem& item = popupItems[index];
                        const int top = SaturatingAdd(contextPopup.top, static_cast<int>(index) * rowHeight);
                        if (item.identifier == 0 && item.text.empty())
                        {
                            MiniGdi::DrawLine(composite, MiniGdi::Point{ contextPopup.left + 4, top + rowHeight / 2 },
                                MiniGdi::Point{ popupRight - 4, top + rowHeight / 2 }, MiniGdi::MakeColor(192, 192, 192));
                        }
                        else if ((item.state & 0x0002u) == 0)
                        {
                            const std::wstring caption = MenuCaptionForDisplay(item.text);
                            m_gdi.TextOutW(popupDc, MiniGdi::Point{ contextPopup.left + 8, top + 2 },
                                caption.data(), caption.size(), nullptr);
                        }
                    }
                    m_gdi.DestroyDc(popupDc);
                }
            }
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
        default: menuMessage = GuestAbi::WmMouseMove; break;
        }
    }
    const HWND menuForeground = reinterpret_cast<HWND>(m_foregroundWindow.load());
    HWND menuRoot = nullptr;
    int menuRootWidth = 0;
    int menuRootHeight = 0;
    int ignoredDimension = 0;
    if (GetGuestSurfaceGeometry(menuForeground, &menuRoot, &menuRootWidth, &menuRootHeight,
        &ignoredDimension, &ignoredDimension, &ignoredDimension, &ignoredDimension))
    {
        const LPARAM menuPosition = MousePosition(menuRootWidth, menuRootHeight, menuRootWidth,
            menuRootHeight, 0, 0, point->Position, m_surfaceImage.Get());
        if (HandleGuestMenuPointer(menuRoot,
            static_cast<int>(static_cast<WORD>(menuPosition & 0xffff)),
            static_cast<int>(static_cast<WORD>((menuPosition >> 16) & 0xffff)), menuMessage))
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
            default: menuMessage = GuestAbi::WmMouseMove; break;
            }
        }
        const HWND foreground = reinterpret_cast<HWND>(m_foregroundWindow.load());
        HWND root = nullptr;
        int rootWidth = 0;
        int rootHeight = 0;
        int ignoredWidth = 0;
        int ignoredHeight = 0;
        int ignoredLeft = 0;
        int ignoredTop = 0;
        if (!GetGuestSurfaceGeometry(
            foreground,
            &root,
            &rootWidth,
            &rootHeight,
            &ignoredWidth,
            &ignoredHeight,
            &ignoredLeft,
            &ignoredTop))
        {
            return;
        }

        const LPARAM rootPosition = MousePosition(
            rootWidth,
            rootHeight,
            rootWidth,
            rootHeight,
            0,
            0,
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
        rootWidth,
        rootHeight,
        targetWidth,
        targetHeight,
        targetLeft,
        targetTop,
        point->Position,
        m_surfaceImage.Get());
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
        int ignoredLeft = 0;
        int ignoredTop = 0;
        if (!GetGuestSurfaceGeometry(
            foreground,
            &root,
            &rootWidth,
            &rootHeight,
            &ignoredWidth,
            &ignoredHeight,
            &ignoredLeft,
            &ignoredTop))
        {
            return;
        }

        const LPARAM rootPosition = MousePosition(
            rootWidth,
            rootHeight,
            rootWidth,
            rootHeight,
            0,
            0,
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
        rootWidth,
        rootHeight,
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
    HWND target = reinterpret_cast<HWND>(m_focusWindow.load());
    if (!target)
    {
        target = reinterpret_cast<HWND>(m_foregroundWindow.load());
    }
    if (target && IsGuestWindowVisibleInternal(target) && IsGuestWindowEnabledInternal(target))
    {
        PostGuestMessage(target, message, static_cast<WPARAM>(args->VirtualKey), 0, nullptr);
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
        CoreDispatcher^ dispatcher = m_dispatcher.Get();
        if (dispatcher && dispatcher->HasThreadAccess)
        {
            coreWindow->PointerMoved -= pointerMoved;
            coreWindow->PointerPressed -= pointerPressed;
            coreWindow->PointerReleased -= pointerReleased;
            coreWindow->PointerWheelChanged -= pointerWheel;
            coreWindow->KeyDown -= keyDown;
            coreWindow->KeyUp -= keyUp;
            return;
        }

        if (dispatcher)
        {
            Platform::Agile<CoreWindow^> agileCoreWindow(coreWindow);
            dispatcher->RunAsync(CoreDispatcherPriority::Normal,
                ref new DispatchedHandler([
                    agileCoreWindow,
                    pointerMoved,
                    pointerPressed,
                    pointerReleased,
                    pointerWheel,
                    keyDown,
                    keyUp]()
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

GuestWindowScope::GuestWindowScope(GuestWindowManager* manager)
    : m_previous(g_currentGuestWindowManager)
{
    g_currentGuestWindowManager = manager;
    if (manager)
    {
        manager->Activate();
    }
}

GuestWindowScope::~GuestWindowScope()
{
    if (g_currentGuestWindowManager)
    {
        g_currentGuestWindowManager->Deactivate();
    }
    g_currentGuestWindowManager = m_previous;
    if (m_previous)
    {
        m_previous->Activate();
    }
}
