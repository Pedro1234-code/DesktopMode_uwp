#include "pch.h"
#include "Bridge\\User32Shims.h"
#include "Bridge\\Win32Shims.h"

#include "Bridge\\GuestWindow.h"
#include "Bridge\\Kernel32Shims.h"
#include "Bridge\\MiniGdi.h"
#include "Bridge/GuestMetrics.h"
#include "Bridge/GuestResources.h"
#include "Bridge/DialogResources.h"
#include "Bridge/RuntimeDiagnostics.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <cwchar>
#include <cwctype>
#include <limits>
#include <mutex>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

using namespace Win32Bridge::Bridge;

namespace
{
    constexpr int MaximumGuestSystemColor = 30;
    constexpr ULONG_PTR GuestCursorToken = 0x7fff1000;
    constexpr ULONG_PTR GuestIconToken = 0x7fff2000;
    ULONG_PTR g_nextGuestIcon = GuestIconToken + 1;
    std::mutex g_iconLock;
    std::unordered_map<ULONG_PTR, MiniGdi::Surface> g_guestIcons;

    bool IsUserLibrary(const std::wstring& library)
    {
        return _wcsicmp(library.c_str(), L"user32.dll") == 0 ||
            _wcsnicmp(library.c_str(), L"api-ms-win-ntuser-", 18) == 0 ||
            _wcsnicmp(library.c_str(), L"ext-ms-win-ntuser-", 18) == 0;
    }

    GuestWindowManager* CurrentManagerOrFail()
    {
        GuestWindowManager* manager = CurrentGuestWindowManager();
        if (!manager)
        {
            BridgeSetLastError(ERROR_INVALID_FUNCTION);
        }
        return manager;
    }

    MiniGdi::ObjectHandle FromGuestObject(HGDIOBJ object)
    {
        const ULONG_PTR raw = reinterpret_cast<ULONG_PTR>(object);
        if (raw == 0 || raw > static_cast<ULONG_PTR>((std::numeric_limits<MiniGdi::ObjectHandle>::max)()))
        {
            return MiniGdi::InvalidObject;
        }
        return static_cast<MiniGdi::ObjectHandle>(raw);
    }

    // CallWindowProcW is also a guest-to-guest callback. Keep the SEH scope
    // free of C++ objects so a broken subclass procedure cannot terminate the
    // UWP host while handling a navigation/control notification.
    LRESULT InvokeGuestSubclassProcedure(
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

    bool ReadGuestMessageValue(
        const GuestAbi::Message* source,
        GuestAbi::Message* destination)
    {
        if (!source || !destination) return false;
        __try
        {
            *destination = *source;
            return true;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return false;
        }
    }

    bool IsGuestSystemColor(int color)
    {
        return color >= 0 && color <= MaximumGuestSystemColor;
    }

    MiniGdi::Color GuestSystemColor(int color)
    {
        // Stable colors make traditional Win32 code deterministic on Xbox and
        // avoid exposing a host theme object through a guest COLORREF.
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
        case 0:  // COLOR_SCROLLBAR
        case 2:  // COLOR_ACTIVECAPTION
        case 3:  // COLOR_INACTIVECAPTION
        case 4:  // COLOR_MENU
        case 10: // COLOR_ACTIVEBORDER
        case 11: // COLOR_INACTIVEBORDER
        case 12: // COLOR_APPWORKSPACE
        case 19: // COLOR_INACTIVECAPTIONTEXT
        case 23: // COLOR_3DHILIGHT
        case 25: // COLOR_INFOTEXT
        case 27: // COLOR_GRADIENTACTIVECAPTION
        case 28: // COLOR_GRADIENTINACTIVECAPTION
        case 29: // COLOR_MENUHILIGHT
        case 30: // COLOR_MENUBAR
        case 5:  // COLOR_WINDOW
        default:
            return MiniGdi::OpaqueWhite;
        }
    }

    COLORREF GuestColorRef(int color)
    {
        const MiniGdi::Color value = GuestSystemColor(color);
        return static_cast<COLORREF>(MiniGdi::Red(value)) |
            (static_cast<COLORREF>(MiniGdi::Green(value)) << 8) |
            (static_cast<COLORREF>(MiniGdi::Blue(value)) << 16);
    }

    bool DecodeDeviceIndependentBitmap(const BYTE* data, size_t size, bool iconBitmap, MiniGdi::Surface* image)
    {
        if (!data || !image || size < sizeof(BITMAPINFOHEADER)) return false;
        BITMAPINFOHEADER header = {};
        memcpy(&header, data, sizeof(header));
        if (header.biSize < sizeof(BITMAPINFOHEADER) || header.biWidth <= 0 || header.biHeight == 0 ||
            (header.biBitCount != 4 && header.biBitCount != 8 && header.biBitCount != 24 && header.biBitCount != 32) ||
            header.biCompression != BI_RGB)
        {
            return false;
        }
        const int width = header.biWidth;
        const int sourceHeight = header.biHeight < 0 ? -header.biHeight : header.biHeight;
        const int height = iconBitmap ? sourceHeight / 2 : sourceHeight;
        if (height <= 0 || width > 512 || height > 512 || !image->Resize(width, height, MiniGdi::Transparent))
        {
            return false;
        }
        const size_t paletteEntries = header.biBitCount <= 8
            ? (header.biClrUsed ? header.biClrUsed : (1u << header.biBitCount)) : 0;
        const size_t paletteOffset = header.biSize;
        const size_t paletteBytes = paletteEntries * sizeof(RGBQUAD);
        if (paletteOffset > size || paletteBytes > size - paletteOffset) return false;
        const size_t bitsOffset = paletteOffset + paletteBytes;
        const size_t bitsPerRow = static_cast<size_t>(((static_cast<unsigned long long>(width) * header.biBitCount + 31) / 32) * 4);
        const size_t xorBytes = bitsPerRow * static_cast<size_t>(height);
        if (bitsOffset > size || xorBytes > size - bitsOffset) return false;
        const BYTE* palette = data + paletteOffset;
        const BYTE* bits = data + bitsOffset;
        const size_t andStride = static_cast<size_t>(((static_cast<unsigned long long>(width) + 31) / 32) * 4);
        const BYTE* andBits = nullptr;
        if (iconBitmap && xorBytes <= size - bitsOffset && andStride <= (size - bitsOffset - xorBytes) / static_cast<size_t>(height))
        {
            andBits = bits + xorBytes;
        }
        const bool topDown = header.biHeight < 0;
        for (int y = 0; y < height; ++y)
        {
            const int sourceY = topDown ? y : height - 1 - y;
            const BYTE* row = bits + static_cast<size_t>(sourceY) * bitsPerRow;
            for (int x = 0; x < width; ++x)
            {
                BYTE red = 0, green = 0, blue = 0, alpha = 255;
                if (header.biBitCount == 32)
                {
                    const BYTE* pixel = row + static_cast<size_t>(x) * 4;
                    blue = pixel[0]; green = pixel[1]; red = pixel[2]; alpha = pixel[3] ? pixel[3] : 255;
                }
                else if (header.biBitCount == 24)
                {
                    const BYTE* pixel = row + static_cast<size_t>(x) * 3;
                    blue = pixel[0]; green = pixel[1]; red = pixel[2];
                }
                else
                {
                    const BYTE index = header.biBitCount == 8
                        ? row[x]
                        : static_cast<BYTE>((row[x / 2] >> ((x & 1) ? 0 : 4)) & 0x0f);
                    if (index >= paletteEntries) return false;
                    const RGBQUAD* color = reinterpret_cast<const RGBQUAD*>(palette) + index;
                    blue = color->rgbBlue; green = color->rgbGreen; red = color->rgbRed;
                }
                if (andBits)
                {
                    const BYTE* maskRow = andBits + static_cast<size_t>(sourceY) * andStride;
                    if ((maskRow[x / 8] & (0x80u >> (x & 7))) != 0)
                    {
                        alpha = 0;
                    }
                }
                MiniGdi::Color* output = image->PixelAt(x, y);
                if (!output) return false;
                *output = MiniGdi::MakeColor(red, green, blue, alpha);
            }
        }
        return true;
    }

    bool LoadBitmapResourcePixels(LPCWSTR resource, MiniGdi::Surface* image)
    {
        const BYTE* data = nullptr;
        size_t size = 0;
        return FindGuestResource(2 /* RT_BITMAP */, resource, &data, &size) &&
            DecodeDeviceIndependentBitmap(data, size, false, image);
    }

    bool LoadIconResourcePixels(LPCWSTR resource, MiniGdi::Surface* image)
    {
        const BYTE* group = nullptr;
        size_t groupSize = 0;
        if (!FindGuestResource(14 /* RT_GROUP_ICON */, resource, &group, &groupSize) || groupSize < 6)
        {
            return false;
        }
        WORD count = 0;
        memcpy(&count, group + 4, sizeof(count));
        if (count == 0 || groupSize < 6 + static_cast<size_t>(count) * 14) return false;
        const BYTE* best = group + 6;
        int bestDistance = (std::numeric_limits<int>::max)();
        for (WORD index = 0; index < count; ++index)
        {
            const BYTE* entry = group + 6 + static_cast<size_t>(index) * 14;
            const int width = entry[0] ? entry[0] : 256;
            const int height = entry[1] ? entry[1] : 256;
            const int distance = (width > 24 ? width - 24 : 24 - width) +
                (height > 24 ? height - 24 : 24 - height);
            if (distance < bestDistance) { best = entry; bestDistance = distance; }
        }
        WORD iconId = 0;
        memcpy(&iconId, best + 12, sizeof(iconId));
        const BYTE* icon = nullptr;
        size_t iconSize = 0;
        return FindGuestResource(3 /* RT_ICON */, reinterpret_cast<LPCWSTR>(static_cast<ULONG_PTR>(iconId)), &icon, &iconSize) &&
            DecodeDeviceIndependentBitmap(icon, iconSize, true, image);
    }

    // Wire layout of MENUITEMINFOW.  It belongs to the guest ABI, rather than
    // the UWP SDK, and is deliberately kept local to the menu translator.
    struct GuestMenuItemInfoW
    {
        UINT cbSize;
        UINT fMask;
        UINT fType;
        UINT fState;
        UINT wID;
        HMENU hSubMenu;
        HBITMAP hbmpChecked;
        HBITMAP hbmpUnchecked;
        ULONG_PTR dwItemData;
        LPWSTR dwTypeData;
        UINT cch;
        HBITMAP hbmpItem;
    };

    constexpr size_t GuestMenuItemInfoLegacySize = offsetof(GuestMenuItemInfoW, hbmpItem);

    bool ReadGuestMenuItemInfo(const void* source, GuestMenuItemInfoW* destination)
    {
        if (!source || !destination || reinterpret_cast<ULONG_PTR>(source) <= 0xffff) return false;
        __try
        {
            const UINT size = *static_cast<const UINT*>(source);
            if (size < GuestMenuItemInfoLegacySize) return false;
            memset(destination, 0, sizeof(*destination));
            memcpy(destination, source, (std::min)(static_cast<size_t>(size), sizeof(*destination)));
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return false;
        }
        return true;
    }

    bool WriteGuestMenuItemInfo(void* destination, const GuestMenuItemInfoW& source)
    {
        if (!destination || reinterpret_cast<ULONG_PTR>(destination) <= 0xffff) return false;
        __try
        {
            const UINT size = *static_cast<const UINT*>(destination);
            if (size < GuestMenuItemInfoLegacySize) return false;
            memcpy(destination, &source,
                (std::min)(static_cast<size_t>(size), sizeof(source)));
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return false;
        }
        return true;
    }

    struct GuestWindowPlacement
    {
        UINT length;
        UINT flags;
        UINT showCmd;
        POINT minPosition;
        POINT maxPosition;
        RECT normalPosition;
    };

    struct VirtualMenuItem
    {
        UINT identifier = 0;
        UINT type = 0;
        UINT state = 0;
        HMENU subMenu = nullptr;
        HBITMAP checkedBitmap = nullptr;
        HBITMAP uncheckedBitmap = nullptr;
        HBITMAP itemBitmap = nullptr;
        ULONG_PTR itemData = 0;
        std::wstring text;
    };

    struct VirtualMenu
    {
        std::vector<VirtualMenuItem> items;
    };

    constexpr UINT kMfByPosition = 0x0400;
    constexpr UINT kMfPopup = 0x0010;
    constexpr UINT kMfGrayed = 0x0001;
    constexpr UINT kMfChecked = 0x0008;
    constexpr UINT kMfDisabled = 0x0002;
    constexpr UINT kMfHighlighted = 0x0080;
    constexpr UINT kMfDefault = 0x1000;
    constexpr UINT kMfSeparator = 0x0800;
    constexpr UINT kMftRadioCheck = 0x0200;
    constexpr UINT kMiimState = 0x0001;
    constexpr UINT kMiimId = 0x0002;
    constexpr UINT kMiimSubmenu = 0x0004;
    constexpr UINT kMiimCheckmarks = 0x0008;
    // Older callers commonly use MIIM_TYPE together with MFT_STRING instead
    // of the newer MIIM_STRING spelling.  Both describe dwTypeData.
    constexpr UINT kMiimType = 0x0010;
    constexpr UINT kMiimFtype = 0x0100;
    constexpr UINT kMiimString = 0x0040;
    constexpr UINT kMiimData = 0x0020;
    constexpr UINT kMiimBitmap = 0x0080;

    bool ProbeGuestMenuText(LPCWSTR source, size_t maximum, size_t* length)
    {
        if (!length)
        {
            return false;
        }
        *length = 0;
        if (!source || reinterpret_cast<ULONG_PTR>(source) <= 0xffff)
        {
            return source == nullptr;
        }
        __try
        {
            while (*length < maximum && source[*length] != L'\0') ++*length;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            *length = 0;
            return false;
        }
        return *length < maximum;
    }

    bool CopyGuestMenuText(LPWSTR destination, size_t capacity, const std::wstring& source)
    {
        if (!destination || capacity == 0 || reinterpret_cast<ULONG_PTR>(destination) <= 0xffff)
        {
            return false;
        }
        const size_t count = (std::min)(source.size(), capacity - 1);
        __try
        {
            if (count) memcpy(destination, source.data(), count * sizeof(wchar_t));
            destination[count] = L'\0';
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return false;
        }
        return true;
    }

    bool CopyGuestMenuCharacters(LPCWSTR source, wchar_t* destination, size_t count)
    {
        if (!source || !destination || reinterpret_cast<ULONG_PTR>(source) <= 0xffff)
        {
            return false;
        }
        __try
        {
            if (count) memcpy(destination, source, count * sizeof(wchar_t));
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return false;
        }
        return true;
    }

    std::wstring ReadGuestMenuText(const GuestMenuItemInfoW& source)
    {
        if (!source.dwTypeData)
        {
            return {};
        }

        // cch is a retrieval-buffer size in several otherwise-valid menu
        // construction paths.  A zero value therefore means that dwTypeData
        // is the usual NUL-terminated menu caption, not an empty caption.
        constexpr size_t MaximumMenuTextLength = 32768;
        const size_t requested = source.cch
            ? (std::min)(static_cast<size_t>(source.cch), MaximumMenuTextLength)
            : MaximumMenuTextLength;
        size_t length = 0;
        if (!ProbeGuestMenuText(source.dwTypeData, requested, &length))
        {
            // MIIM_STRING setters may provide cch without a trailing NUL. In
            // that form the explicit count is authoritative, but the memory
            // still has to be readable before it enters std::wstring.
            if (!source.cch || length != requested)
                return {};
        }
        std::vector<wchar_t> characters(length);
        if (characters.empty()) return {};
        if (length && !CopyGuestMenuCharacters(source.dwTypeData, characters.data(), length)) return {};
        return std::wstring(characters.data(), characters.size());
    }

    std::mutex g_menuLock;
    std::unordered_map<ULONG_PTR, VirtualMenu> g_menus;
    std::unordered_map<ULONG_PTR, HMENU> g_windowMenus;
    ULONG_PTR g_nextMenu = 0x7fff5000;
    UINT g_nextClipboardFormat = 0xc000;

    VirtualMenu* FindMenuLocked(HMENU menu)
    {
        const auto found = g_menus.find(reinterpret_cast<ULONG_PTR>(menu));
        return found == g_menus.end() ? nullptr : &found->second;
    }

    VirtualMenuItem* FindMenuItemLocked(VirtualMenu& menu, UINT item, BOOL byPosition)
    {
        if (byPosition)
        {
            return item < menu.items.size() ? &menu.items[item] : nullptr;
        }
        for (auto& candidate : menu.items)
        {
            if (candidate.identifier == item) return &candidate;
        }
        return nullptr;
    }

    constexpr WORD MenuResourceType = 4; // RT_MENU
    constexpr WORD MenuFlagPopup = 0x0010;
    constexpr WORD MenuFlagEnd = 0x0080;

    bool ReadMenuWord(const BYTE* data, size_t size, size_t* offset, WORD* value)
    {
        if (!data || !offset || !value || *offset > size || size - *offset < sizeof(*value))
        {
            return false;
        }
        memcpy(value, data + *offset, sizeof(*value));
        *offset += sizeof(*value);
        return true;
    }

    bool ReadMenuDword(const BYTE* data, size_t size, size_t* offset, DWORD* value)
    {
        if (!data || !offset || !value || *offset > size || size - *offset < sizeof(*value))
        {
            return false;
        }
        memcpy(value, data + *offset, sizeof(*value));
        *offset += sizeof(*value);
        return true;
    }

    bool AlignMenuOffset(size_t* offset, size_t size, size_t alignment)
    {
        if (!offset || alignment == 0)
        {
            return false;
        }
        const size_t aligned = (*offset + alignment - 1) & ~(alignment - 1);
        if (aligned < *offset || aligned > size)
        {
            return false;
        }
        *offset = aligned;
        return true;
    }

    bool ReadMenuString(const BYTE* data, size_t size, size_t* offset, std::wstring* text)
    {
        if (!data || !offset || !text || *offset > size)
        {
            return false;
        }
        text->clear();
        while (*offset <= size && size - *offset >= sizeof(WORD))
        {
            WORD character = 0;
            if (!ReadMenuWord(data, size, offset, &character))
            {
                return false;
            }
            if (character == 0)
            {
                return true;
            }
            text->push_back(static_cast<wchar_t>(character));
        }
        return false;
    }

    bool ParseStandardMenuItems(const BYTE* data, size_t size, size_t* offset, HMENU destination, unsigned depth)
    {
        if (!destination || depth > 16)
        {
            return false;
        }
        for (;;)
        {
            WORD rawFlags = 0;
            if (!ReadMenuWord(data, size, offset, &rawFlags))
            {
                return false;
            }
            const bool end = (rawFlags & MenuFlagEnd) != 0;
            const UINT flags = rawFlags & ~MenuFlagEnd;
            WORD identifier = 0;
            if ((flags & MenuFlagPopup) == 0 && !ReadMenuWord(data, size, offset, &identifier))
            {
                return false;
            }
            std::wstring caption;
            if (!ReadMenuString(data, size, offset, &caption))
            {
                return false;
            }
            if ((flags & MenuFlagPopup) != 0)
            {
                HMENU child = BridgeCreatePopupMenu();
                if (!child || !ParseStandardMenuItems(data, size, offset, child, depth + 1) ||
                    !BridgeAppendMenuW(destination, flags, reinterpret_cast<UINT_PTR>(child), caption.c_str()))
                {
                    if (child) BridgeDestroyMenu(child);
                    return false;
                }
            }
            else if (!BridgeAppendMenuW(destination, flags, identifier, caption.empty() ? nullptr : caption.c_str()))
            {
                return false;
            }
            if (end)
            {
                return true;
            }
        }
    }

    bool ParseExtendedMenuItems(const BYTE* data, size_t size, size_t* offset, HMENU destination, unsigned depth)
    {
        if (!destination || depth > 16)
        {
            return false;
        }
        for (;;)
        {
            DWORD type = 0;
            DWORD state = 0;
            DWORD identifier = 0;
            WORD information = 0;
            if (!ReadMenuDword(data, size, offset, &type) || !ReadMenuDword(data, size, offset, &state) ||
                !ReadMenuDword(data, size, offset, &identifier) || !ReadMenuWord(data, size, offset, &information) ||
                !AlignMenuOffset(offset, size, sizeof(WORD)))
            {
                return false;
            }
            std::wstring caption;
            if (!ReadMenuString(data, size, offset, &caption) || !AlignMenuOffset(offset, size, sizeof(DWORD)))
            {
                return false;
            }
            const bool popup = (information & 0x0001) != 0;
            if (popup)
            {
                DWORD ignoredHelpId = 0;
                HMENU child = nullptr;
                if (!ReadMenuDword(data, size, offset, &ignoredHelpId) || !(child = BridgeCreatePopupMenu()) ||
                    !ParseExtendedMenuItems(data, size, offset, child, depth + 1) ||
                    !BridgeAppendMenuW(destination, static_cast<UINT>(type | MenuFlagPopup | state),
                        reinterpret_cast<UINT_PTR>(child), caption.c_str()))
                {
                    if (child) BridgeDestroyMenu(child);
                    return false;
                }
            }
            else if (!BridgeAppendMenuW(destination, static_cast<UINT>(type | state), identifier,
                caption.empty() ? nullptr : caption.c_str()))
            {
                return false;
            }
            if ((information & MenuFlagEnd) != 0)
            {
                return true;
            }
        }
    }

    HMENU LoadGuestMenuResource(LPCWSTR resource)
    {
        const BYTE* data = nullptr;
        size_t size = 0;
        if (!FindGuestResource(MenuResourceType, resource, &data, &size) || size < 4)
        {
            return nullptr;
        }
        WORD version = 0;
        WORD offset = 0;
        size_t cursor = 0;
        if (!ReadMenuWord(data, size, &cursor, &version) || !ReadMenuWord(data, size, &cursor, &offset) ||
            offset > size - cursor)
        {
            return nullptr;
        }
        cursor += offset;
        HMENU menu = BridgeCreatePopupMenu();
        const bool parsed = menu && (version == 0
            ? ParseStandardMenuItems(data, size, &cursor, menu, 0)
            : version == 1 && ParseExtendedMenuItems(data, size, &cursor, menu, 0));
        if (!parsed)
        {
            if (menu) BridgeDestroyMenu(menu);
            return nullptr;
        }
        return menu;
    }
}

ATOM WINAPI Win32Bridge::Bridge::BridgeRegisterClassExW(const GuestAbi::WndClassExW* windowClass)
{
    GuestWindowManager* manager = CurrentManagerOrFail();
    if (!manager)
    {
        return 0;
    }

    DWORD error = ERROR_SUCCESS;
    const ATOM result = manager->RegisterGuestClass(windowClass, &error);
    BridgeSetLastError(error);
    return result;
}

ATOM WINAPI Win32Bridge::Bridge::BridgeRegisterClassW(const GuestAbi::WndClassW* windowClass)
{
    GuestWindowManager* manager = CurrentManagerOrFail();
    if (!manager)
    {
        return 0;
    }

    DWORD error = ERROR_SUCCESS;
    const ATOM result = manager->RegisterGuestClass(windowClass, &error);
    BridgeSetLastError(error);
    return result;
}

HWND WINAPI Win32Bridge::Bridge::BridgeCreateWindowExW(
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
    LPVOID parameter)
{
    GuestWindowManager* manager = CurrentManagerOrFail();
    if (!manager)
    {
        return nullptr;
    }

    DWORD error = ERROR_SUCCESS;
    const HWND result = manager->CreateGuestWindow(
        extendedStyle,
        className,
        windowName,
        style,
        x,
        y,
        width,
        height,
        parent,
        menu,
        instance,
        parameter,
        &error);
    BridgeSetLastError(error);
    return result;
}

HCURSOR WINAPI Win32Bridge::Bridge::BridgeLoadCursorW(HINSTANCE, LPCWSTR cursorName)
{
    if (!cursorName)
    {
        BridgeSetLastError(ERROR_INVALID_PARAMETER);
        return nullptr;
    }

    // Cursor resources are guest-visible tokens only. CoreWindow owns the
    // actual host pointer, so never surface a desktop HCURSOR to the PE.
    BridgeSetLastError(ERROR_SUCCESS);
    return reinterpret_cast<HCURSOR>(GuestCursorToken);
}

HCURSOR WINAPI Win32Bridge::Bridge::BridgeSetCursor(HCURSOR cursor)
{
    // The UWP CoreWindow owns the actual cursor.  Preserve the guest ABI and
    // return the previous opaque token rather than exposing a host cursor.
    static thread_local HCURSOR current = nullptr;
    HCURSOR previous = current;
    current = cursor;
    BridgeSetLastError(ERROR_SUCCESS);
    return previous;
}

HICON WINAPI Win32Bridge::Bridge::BridgeLoadIconW(HINSTANCE, LPCWSTR iconName)
{
    if (!iconName)
    {
        BridgeSetLastError(ERROR_INVALID_PARAMETER);
        return nullptr;
    }

    MiniGdi::Surface image;
    if (LoadIconResourcePixels(iconName, &image))
    {
        std::lock_guard<std::mutex> guard(g_iconLock);
        const ULONG_PTR token = g_nextGuestIcon++;
        g_guestIcons.emplace(token, std::move(image));
        RuntimeDiagnostics::Record(L"ICON: decoded a guest RT_GROUP_ICON resource.");
        BridgeSetLastError(ERROR_SUCCESS);
        return reinterpret_cast<HICON>(token);
    }

    // System/class icons may legitimately have no PE resource. Keep their
    // opaque fallback token, but prefer decoded guest pixels whenever present.
    RuntimeDiagnostics::Record(L"ICON: requested resource was unavailable; using the shared fallback token.");
    BridgeSetLastError(ERROR_SUCCESS);
    return reinterpret_cast<HICON>(GuestIconToken);
}

BOOL WINAPI Win32Bridge::Bridge::BridgeDestroyCursor(HCURSOR cursor)
{
    if (reinterpret_cast<ULONG_PTR>(cursor) != GuestCursorToken)
    {
        BridgeSetLastError(ERROR_INVALID_HANDLE);
        return FALSE;
    }
    // Every cursor token is shared/static in this bridge.
    BridgeSetLastError(ERROR_SUCCESS);
    return TRUE;
}

BOOL WINAPI Win32Bridge::Bridge::BridgeDestroyIcon(HICON icon)
{
    const ULONG_PTR token = reinterpret_cast<ULONG_PTR>(icon);
    if (token == GuestIconToken)
    {
        BridgeSetLastError(ERROR_SUCCESS);
        return TRUE;
    }
    std::lock_guard<std::mutex> guard(g_iconLock);
    if (g_guestIcons.erase(token) == 0)
    {
        BridgeSetLastError(ERROR_INVALID_HANDLE);
        return FALSE;
    }
    BridgeSetLastError(ERROR_SUCCESS);
    return TRUE;
}

bool Win32Bridge::Bridge::CopyGuestIconPixels(HICON icon, MiniGdi::Surface* destination)
{
    if (!destination) return false;
    std::lock_guard<std::mutex> guard(g_iconLock);
    const auto found = g_guestIcons.find(reinterpret_cast<ULONG_PTR>(icon));
    if (found == g_guestIcons.end()) return false;
    *destination = found->second;
    return true;
}

int WINAPI Win32Bridge::Bridge::BridgeGetSystemMetrics(int index)
{
    // The values describe the bridge's virtual desktop rather than the UWP
    // host monitor. Keep common Win32 layout calculations deterministic.
    int value = 0;
    if (!GuestMetrics::TryGetSystemMetric(index, &value))
    {
        BridgeSetLastError(ERROR_INVALID_PARAMETER);
        return 0;
    }
    BridgeSetLastError(ERROR_SUCCESS);
    return value;
}

COLORREF WINAPI Win32Bridge::Bridge::BridgeGetSysColor(int color)
{
    if (!IsGuestSystemColor(color))
    {
        BridgeSetLastError(ERROR_INVALID_PARAMETER);
        return 0;
    }
    BridgeSetLastError(ERROR_SUCCESS);
    return GuestColorRef(color);
}

HBRUSH WINAPI Win32Bridge::Bridge::BridgeGetSysColorBrush(int color)
{
    if (!IsGuestSystemColor(color))
    {
        BridgeSetLastError(ERROR_INVALID_PARAMETER);
        return nullptr;
    }

    // Match the documented WNDCLASS encoding: COLOR_* + 1. FillRect and the
    // class-background path recognize this non-owning system-brush form.
    BridgeSetLastError(ERROR_SUCCESS);
    return reinterpret_cast<HBRUSH>(static_cast<ULONG_PTR>(color + 1));
}

BOOL WINAPI Win32Bridge::Bridge::BridgeAdjustWindowRect(LPRECT rect, DWORD, BOOL hasMenu)
{
    if (!rect)
    {
        BridgeSetLastError(ERROR_INVALID_PARAMETER);
        return FALSE;
    }

    // Guest windows are client-only surfaces today. Retaining the exact RECT
    // is equivalent to a zero non-client frame and keeps layout arithmetic
    // portable until a title-bar adapter exists.
    BridgeSetLastError(ERROR_SUCCESS);
    if (hasMenu)
    {
        rect->bottom += GuestMetrics::MenuHeight;
    }
    return TRUE;
}

BOOL WINAPI Win32Bridge::Bridge::BridgeAdjustWindowRectEx(LPRECT rect, DWORD, BOOL hasMenu, DWORD)
{
    if (!rect)
    {
        BridgeSetLastError(ERROR_INVALID_PARAMETER);
        return FALSE;
    }

    BridgeSetLastError(ERROR_SUCCESS);
    if (hasMenu)
    {
        rect->bottom += GuestMetrics::MenuHeight;
    }
    return TRUE;
}

BOOL WINAPI Win32Bridge::Bridge::BridgeDestroyWindow(HWND window)
{
    GuestWindowManager* manager = CurrentManagerOrFail();
    if (!manager)
    {
        return FALSE;
    }

    DWORD error = ERROR_SUCCESS;
    const BOOL result = manager->DestroyGuestWindow(window, &error);
    BridgeSetLastError(error);
    return result;
}

BOOL WINAPI Win32Bridge::Bridge::BridgeShowWindow(HWND window, int command)
{
    GuestWindowManager* manager = CurrentManagerOrFail();
    if (!manager)
    {
        return FALSE;
    }

    DWORD error = ERROR_SUCCESS;
    const BOOL result = manager->ShowGuestWindow(window, command, &error);
    BridgeSetLastError(error);
    return result;
}

BOOL WINAPI Win32Bridge::Bridge::BridgeGetClientRect(HWND window, LPRECT rect)
{
    GuestWindowManager* manager = CurrentManagerOrFail();
    if (!manager)
    {
        return FALSE;
    }

    DWORD error = ERROR_SUCCESS;
    const BOOL result = manager->GetGuestClientRect(window, rect, &error);
    BridgeSetLastError(error);
    return result;
}

BOOL WINAPI Win32Bridge::Bridge::BridgeGetWindowRect(HWND window, LPRECT rect)
{
    GuestWindowManager* manager = CurrentManagerOrFail();
    if (!manager)
    {
        return FALSE;
    }

    DWORD error = ERROR_SUCCESS;
    const BOOL result = manager->GetGuestWindowRect(window, rect, &error);
    BridgeSetLastError(error);
    return result;
}

BOOL WINAPI Win32Bridge::Bridge::BridgeSetWindowTextW(HWND window, LPCWSTR text)
{
    GuestWindowManager* manager = CurrentManagerOrFail();
    if (!manager)
    {
        return FALSE;
    }

    DWORD error = ERROR_SUCCESS;
    const BOOL result = manager->SetGuestWindowText(window, text, &error);
    BridgeSetLastError(error);
    return result;
}

int WINAPI Win32Bridge::Bridge::BridgeGetWindowTextW(HWND window, LPWSTR buffer, int count)
{
    GuestWindowManager* manager = CurrentManagerOrFail();
    if (!manager)
    {
        return 0;
    }

    DWORD error = ERROR_SUCCESS;
    const int result = manager->GetGuestWindowText(window, buffer, count, &error);
    BridgeSetLastError(error);
    return result;
}

int WINAPI Win32Bridge::Bridge::BridgeGetWindowTextLengthW(HWND window)
{
    GuestWindowManager* manager = CurrentManagerOrFail();
    if (!manager)
    {
        return 0;
    }

    DWORD error = ERROR_SUCCESS;
    const int result = manager->GetGuestWindowTextLength(window, &error);
    BridgeSetLastError(error);
    return result;
}

HWND WINAPI Win32Bridge::Bridge::BridgeGetDlgItem(HWND parent, int identifier)
{
    GuestWindowManager* manager = CurrentManagerOrFail();
    if (!manager)
    {
        return nullptr;
    }

    DWORD error = ERROR_SUCCESS;
    const HWND result = manager->GetGuestDlgItem(parent, identifier, &error);
    BridgeSetLastError(error);
    return result;
}

int WINAPI Win32Bridge::Bridge::BridgeGetDlgCtrlID(HWND window)
{
    GuestWindowManager* manager = CurrentManagerOrFail();
    if (!manager)
    {
        return 0;
    }

    DWORD error = ERROR_SUCCESS;
    const LONG_PTR identifier = manager->GetGuestWindowLongPtr(window, GuestAbi::GwlpId, &error);
    BridgeSetLastError(error);
    return error == ERROR_SUCCESS ? static_cast<int>(identifier) : 0;
}

BOOL WINAPI Win32Bridge::Bridge::BridgeSetDlgItemTextW(HWND parent, int identifier, LPCWSTR text)
{
    GuestWindowManager* manager = CurrentManagerOrFail();
    if (!manager)
    {
        return FALSE;
    }

    DWORD error = ERROR_SUCCESS;
    const HWND child = manager->GetGuestDlgItem(parent, identifier, &error);
    if (!child)
    {
        BridgeSetLastError(error == ERROR_SUCCESS ? ERROR_NOT_FOUND : error);
        return FALSE;
    }

    const BOOL result = manager->SetGuestWindowText(child, text, &error);
    BridgeSetLastError(error);
    return result;
}

UINT WINAPI Win32Bridge::Bridge::BridgeGetDlgItemTextW(HWND parent, int identifier, LPWSTR buffer, int count)
{
    GuestWindowManager* manager = CurrentManagerOrFail();
    if (!manager)
    {
        return 0;
    }

    DWORD error = ERROR_SUCCESS;
    const HWND child = manager->GetGuestDlgItem(parent, identifier, &error);
    if (!child)
    {
        BridgeSetLastError(error == ERROR_SUCCESS ? ERROR_NOT_FOUND : error);
        return 0;
    }

    const int result = manager->GetGuestWindowText(child, buffer, count, &error);
    BridgeSetLastError(error);
    return result > 0 ? static_cast<UINT>(result) : 0;
}

LRESULT WINAPI Win32Bridge::Bridge::BridgeSendDlgItemMessageW(
    HWND parent,
    int identifier,
    UINT message,
    WPARAM wParam,
    LPARAM lParam)
{
    GuestWindowManager* manager = CurrentManagerOrFail();
    if (!manager)
    {
        return 0;
    }

    DWORD error = ERROR_SUCCESS;
    const HWND child = manager->GetGuestDlgItem(parent, identifier, &error);
    if (!child)
    {
        BridgeSetLastError(error == ERROR_SUCCESS ? ERROR_NOT_FOUND : error);
        return 0;
    }

    const LRESULT result = manager->SendGuestMessage(child, message, wParam, lParam, &error);
    BridgeSetLastError(error);
    return result;
}

BOOL WINAPI Win32Bridge::Bridge::BridgeIsWindow(HWND window)
{
    GuestWindowManager* manager = CurrentManagerOrFail();
    if (!manager)
    {
        return FALSE;
    }

    const BOOL result = manager->IsGuestWindow(window);
    BridgeSetLastError(result ? ERROR_SUCCESS : ERROR_INVALID_WINDOW_HANDLE);
    return result;
}

BOOL WINAPI Win32Bridge::Bridge::BridgeIsWindowVisible(HWND window)
{
    GuestWindowManager* manager = CurrentManagerOrFail();
    if (!manager)
    {
        return FALSE;
    }

    DWORD error = ERROR_SUCCESS;
    const BOOL result = manager->IsGuestWindowVisible(window, &error);
    BridgeSetLastError(error);
    return result;
}

BOOL WINAPI Win32Bridge::Bridge::BridgeIsWindowEnabled(HWND window)
{
    GuestWindowManager* manager = CurrentManagerOrFail();
    if (!manager)
    {
        return FALSE;
    }

    DWORD error = ERROR_SUCCESS;
    const BOOL result = manager->IsGuestWindowEnabled(window, &error);
    BridgeSetLastError(error);
    return result;
}

BOOL WINAPI Win32Bridge::Bridge::BridgeIsZoomed(HWND window)
{
    if (!BridgeIsWindow(window))
    {
        BridgeSetLastError(ERROR_INVALID_WINDOW_HANDLE);
        return FALSE;
    }
    // The bridge currently exposes one non-maximized guest presentation
    // surface. Returning FALSE is the normal state, not an unsupported call.
    BridgeSetLastError(ERROR_SUCCESS);
    return FALSE;
}

BOOL WINAPI Win32Bridge::Bridge::BridgeEnableWindow(HWND window, BOOL enable)
{
    GuestWindowManager* manager = CurrentManagerOrFail();
    if (!manager)
    {
        return FALSE;
    }

    DWORD error = ERROR_SUCCESS;
    const BOOL result = manager->EnableGuestWindow(window, enable, &error);
    BridgeSetLastError(error);
    return result;
}

HWND WINAPI Win32Bridge::Bridge::BridgeSetFocus(HWND window)
{
    GuestWindowManager* manager = CurrentManagerOrFail();
    if (!manager)
    {
        return nullptr;
    }

    DWORD error = ERROR_SUCCESS;
    const HWND result = manager->SetGuestFocus(window, &error);
    BridgeSetLastError(error);
    return result;
}

HWND WINAPI Win32Bridge::Bridge::BridgeGetFocus()
{
    GuestWindowManager* manager = CurrentManagerOrFail();
    if (!manager)
    {
        return nullptr;
    }

    DWORD error = ERROR_SUCCESS;
    const HWND result = manager->GetGuestFocus(&error);
    BridgeSetLastError(error);
    return result;
}

HWND WINAPI Win32Bridge::Bridge::BridgeSetCapture(HWND window)
{
    GuestWindowManager* manager = CurrentManagerOrFail();
    if (!manager)
    {
        return nullptr;
    }

    DWORD error = ERROR_SUCCESS;
    const HWND result = manager->SetGuestCapture(window, &error);
    BridgeSetLastError(error);
    return result;
}

BOOL WINAPI Win32Bridge::Bridge::BridgeReleaseCapture()
{
    GuestWindowManager* manager = CurrentManagerOrFail();
    if (!manager)
    {
        return FALSE;
    }

    DWORD error = ERROR_SUCCESS;
    const BOOL result = manager->ReleaseGuestCapture(&error);
    BridgeSetLastError(error);
    return result;
}

HWND WINAPI Win32Bridge::Bridge::BridgeGetCapture()
{
    GuestWindowManager* manager = CurrentManagerOrFail();
    if (!manager)
    {
        return nullptr;
    }

    DWORD error = ERROR_SUCCESS;
    const HWND result = manager->GetGuestCapture(&error);
    BridgeSetLastError(error);
    return result;
}

HWND WINAPI Win32Bridge::Bridge::BridgeGetParent(HWND window)
{
    GuestWindowManager* manager = CurrentManagerOrFail();
    if (!manager)
    {
        return nullptr;
    }

    DWORD error = ERROR_SUCCESS;
    const HWND result = manager->GetGuestParent(window, &error);
    BridgeSetLastError(error);
    return result;
}

LONG WINAPI Win32Bridge::Bridge::BridgeGetWindowLongW(HWND window, int index)
{
    GuestWindowManager* manager = CurrentManagerOrFail();
    if (!manager)
    {
        return 0;
    }

    DWORD error = ERROR_SUCCESS;
    const LONG result = manager->GetGuestWindowLong(window, index, &error);
    BridgeSetLastError(error);
    return result;
}

LONG WINAPI Win32Bridge::Bridge::BridgeSetWindowLongW(HWND window, int index, LONG value)
{
    GuestWindowManager* manager = CurrentManagerOrFail();
    if (!manager)
    {
        return 0;
    }

    DWORD error = ERROR_SUCCESS;
    const LONG result = manager->SetGuestWindowLong(window, index, value, &error);
    BridgeSetLastError(error);
    return result;
}

LONG_PTR WINAPI Win32Bridge::Bridge::BridgeGetWindowLongPtrW(HWND window, int index)
{
    GuestWindowManager* manager = CurrentManagerOrFail();
    if (!manager)
    {
        return 0;
    }

    DWORD error = ERROR_SUCCESS;
    const LONG_PTR result = manager->GetGuestWindowLongPtr(window, index, &error);
    BridgeSetLastError(error);
    return result;
}

LONG_PTR WINAPI Win32Bridge::Bridge::BridgeSetWindowLongPtrW(HWND window, int index, LONG_PTR value)
{
    GuestWindowManager* manager = CurrentManagerOrFail();
    if (!manager)
    {
        return 0;
    }

    DWORD error = ERROR_SUCCESS;
    const LONG_PTR result = manager->SetGuestWindowLongPtr(window, index, value, &error);
    BridgeSetLastError(error);
    return result;
}

BOOL WINAPI Win32Bridge::Bridge::BridgeSetWindowPos(
    HWND window,
    HWND insertAfter,
    int x,
    int y,
    int width,
    int height,
    UINT flags)
{
    GuestWindowManager* manager = CurrentManagerOrFail();
    if (!manager)
    {
        return FALSE;
    }

    DWORD error = ERROR_SUCCESS;
    const BOOL result = manager->SetGuestWindowPos(
        window,
        insertAfter,
        x,
        y,
        width,
        height,
        flags,
        &error);
    BridgeSetLastError(error);
    return result;
}

UINT_PTR WINAPI Win32Bridge::Bridge::BridgeSetTimer(
    HWND window,
    UINT_PTR timerId,
    UINT elapseMilliseconds,
    GuestAbi::TimerProc timerProcedure)
{
    GuestWindowManager* manager = CurrentManagerOrFail();
    if (!manager)
    {
        return 0;
    }

    DWORD error = ERROR_SUCCESS;
    const UINT_PTR result = manager->SetGuestTimer(
        window,
        timerId,
        elapseMilliseconds,
        timerProcedure,
        &error);
    BridgeSetLastError(error);
    return result;
}

BOOL WINAPI Win32Bridge::Bridge::BridgeKillTimer(HWND window, UINT_PTR timerId)
{
    GuestWindowManager* manager = CurrentManagerOrFail();
    if (!manager)
    {
        return FALSE;
    }

    DWORD error = ERROR_SUCCESS;
    const BOOL result = manager->KillGuestTimer(window, timerId, &error);
    BridgeSetLastError(error);
    return result;
}

LRESULT WINAPI Win32Bridge::Bridge::BridgeDefWindowProcW(HWND window, UINT message, WPARAM wParam, LPARAM lParam)
{
    GuestWindowManager* manager = CurrentManagerOrFail();
    if (!manager)
    {
        return 0;
    }

    BridgeSetLastError(ERROR_SUCCESS);
    return manager->DefaultGuestWindowProcedure(window, message, wParam, lParam);
}

BOOL WINAPI Win32Bridge::Bridge::BridgePostMessageW(HWND window, UINT message, WPARAM wParam, LPARAM lParam)
{
    GuestWindowManager* manager = CurrentManagerOrFail();
    if (!manager)
    {
        return FALSE;
    }

    DWORD error = ERROR_SUCCESS;
    const BOOL result = manager->PostGuestMessage(window, message, wParam, lParam, &error);
    BridgeSetLastError(error);
    return result;
}

LRESULT WINAPI Win32Bridge::Bridge::BridgeSendMessageW(HWND window, UINT message, WPARAM wParam, LPARAM lParam)
{
    GuestWindowManager* manager = CurrentManagerOrFail();
    if (!manager)
    {
        return 0;
    }

    DWORD error = ERROR_SUCCESS;
    const LRESULT result = manager->SendGuestMessage(window, message, wParam, lParam, &error);
    BridgeSetLastError(error);
    return result;
}

void WINAPI Win32Bridge::Bridge::BridgePostQuitMessage(int exitCode)
{
    GuestWindowManager* manager = CurrentManagerOrFail();
    if (manager)
    {
        manager->PostGuestQuitMessage(exitCode);
        BridgeSetLastError(ERROR_SUCCESS);
    }
}

BOOL WINAPI Win32Bridge::Bridge::BridgeGetMessageW(
    GuestAbi::Message* message,
    HWND window,
    UINT minimumMessage,
    UINT maximumMessage)
{
    GuestWindowManager* manager = CurrentManagerOrFail();
    if (!manager)
    {
        return static_cast<BOOL>(-1);
    }
    if (!message)
    {
        BridgeSetLastError(ERROR_INVALID_PARAMETER);
        return static_cast<BOOL>(-1);
    }

    switch (manager->GetGuestMessage(message, window, minimumMessage, maximumMessage))
    {
    case GuestGetMessageResult::Message:
        BridgeSetLastError(ERROR_SUCCESS);
        return TRUE;
    case GuestGetMessageResult::Quit:
        BridgeSetLastError(ERROR_SUCCESS);
        return FALSE;
    default:
        BridgeSetLastError(ERROR_INVALID_PARAMETER);
        return static_cast<BOOL>(-1);
    }
}

BOOL WINAPI Win32Bridge::Bridge::BridgePeekMessageW(
    GuestAbi::Message* message,
    HWND window,
    UINT minimumMessage,
    UINT maximumMessage,
    UINT removeMessage)
{
    GuestWindowManager* manager = CurrentManagerOrFail();
    if (!manager)
    {
        return FALSE;
    }
    if (!message || (removeMessage & ~(GuestAbi::PeekRemove | GuestAbi::PeekNoYield)) != 0)
    {
        BridgeSetLastError(ERROR_INVALID_PARAMETER);
        return FALSE;
    }

    const BOOL result = manager->PeekGuestMessage(message, window, minimumMessage, maximumMessage, removeMessage);
    if (result)
    {
        BridgeSetLastError(ERROR_SUCCESS);
    }
    return result;
}

BOOL WINAPI Win32Bridge::Bridge::BridgeTranslateMessage(const GuestAbi::Message* message)
{
    GuestWindowManager* manager = CurrentManagerOrFail();
    if (!manager)
    {
        return FALSE;
    }
    if (!message)
    {
        BridgeSetLastError(ERROR_INVALID_PARAMETER);
        return FALSE;
    }

    const BOOL result = manager->TranslateGuestMessage(message);
    BridgeSetLastError(ERROR_SUCCESS);
    return result;
}

LRESULT WINAPI Win32Bridge::Bridge::BridgeDispatchMessageW(const GuestAbi::Message* message)
{
    GuestWindowManager* manager = CurrentManagerOrFail();
    if (!manager)
    {
        return 0;
    }

    DWORD error = ERROR_SUCCESS;
    const LRESULT result = manager->DispatchGuestMessage(message, &error);
    BridgeSetLastError(error);
    return result;
}

BOOL WINAPI Win32Bridge::Bridge::BridgeInvalidateRect(HWND window, const RECT* rect, BOOL erase)
{
    GuestWindowManager* manager = CurrentManagerOrFail();
    if (!manager)
    {
        return FALSE;
    }

    DWORD error = ERROR_SUCCESS;
    const BOOL result = manager->InvalidateGuestRect(window, rect, erase, &error);
    BridgeSetLastError(error);
    return result;
}

BOOL WINAPI Win32Bridge::Bridge::BridgeUpdateWindow(HWND window)
{
    GuestWindowManager* manager = CurrentManagerOrFail();
    if (!manager)
    {
        return FALSE;
    }

    DWORD error = ERROR_SUCCESS;
    const BOOL result = manager->UpdateGuestWindow(window, &error);
    BridgeSetLastError(error);
    return result;
}

HDC WINAPI Win32Bridge::Bridge::BridgeBeginPaint(HWND window, GuestAbi::PaintStruct* paint)
{
    GuestWindowManager* manager = CurrentManagerOrFail();
    if (!manager)
    {
        return nullptr;
    }

    DWORD error = ERROR_SUCCESS;
    const HDC result = manager->BeginGuestPaint(window, paint, &error);
    BridgeSetLastError(error);
    return result;
}

BOOL WINAPI Win32Bridge::Bridge::BridgeEndPaint(HWND window, const GuestAbi::PaintStruct* paint)
{
    GuestWindowManager* manager = CurrentManagerOrFail();
    if (!manager)
    {
        return FALSE;
    }

    DWORD error = ERROR_SUCCESS;
    const BOOL result = manager->EndGuestPaint(window, paint, &error);
    BridgeSetLastError(error);
    return result;
}

HDC WINAPI Win32Bridge::Bridge::BridgeGetDC(HWND window)
{
    GuestWindowManager* manager = CurrentManagerOrFail();
    if (!manager)
    {
        return nullptr;
    }

    DWORD error = ERROR_SUCCESS;
    const HDC result = manager->GetGuestDC(window, &error);
    BridgeSetLastError(error);
    return result;
}

int WINAPI Win32Bridge::Bridge::BridgeReleaseDC(HWND window, HDC dc)
{
    GuestWindowManager* manager = CurrentManagerOrFail();
    if (!manager)
    {
        return 0;
    }

    DWORD error = ERROR_SUCCESS;
    const int result = manager->ReleaseGuestDC(window, dc, &error);
    BridgeSetLastError(error);
    return result;
}

int WINAPI Win32Bridge::Bridge::BridgeFillRect(HDC dc, const RECT* rect, HBRUSH brush)
{
    GuestWindowManager* manager = CurrentManagerOrFail();
    if (!manager)
    {
        return 0;
    }
    if (!rect)
    {
        BridgeSetLastError(ERROR_INVALID_PARAMETER);
        return 0;
    }

    const MiniGdi::DcHandle guestDc = manager->GuestDcHandle(dc);
    const ULONG_PTR rawBrush = reinterpret_cast<ULONG_PTR>(brush);
    MiniGdi::ObjectHandle guestBrush = MiniGdi::InvalidObject;
    bool temporarySystemBrush = false;
    if (rawBrush >= 1 && rawBrush <= static_cast<ULONG_PTR>(MaximumGuestSystemColor + 1))
    {
        guestBrush = manager->Gdi().CreateSolidBrush(GuestSystemColor(static_cast<int>(rawBrush - 1)));
        temporarySystemBrush = true;
    }
    else
    {
        guestBrush = FromGuestObject(brush);
    }

    MiniGdi::ObjectKind kind;
    if (guestDc == MiniGdi::InvalidDc || guestBrush == MiniGdi::InvalidObject ||
        !manager->Gdi().ObjectType(guestBrush, &kind) || kind != MiniGdi::ObjectKind::Brush)
    {
        if (temporarySystemBrush && guestBrush != MiniGdi::InvalidObject)
        {
            manager->Gdi().DeleteObject(guestBrush);
        }
        BridgeSetLastError(ERROR_INVALID_HANDLE);
        return 0;
    }

    MiniGdi::ObjectHandle previous = MiniGdi::InvalidObject;
    if (!manager->Gdi().SelectBrush(guestDc, guestBrush, &previous))
    {
        if (temporarySystemBrush)
        {
            manager->Gdi().DeleteObject(guestBrush);
        }
        BridgeSetLastError(ERROR_INVALID_HANDLE);
        return 0;
    }

    manager->Gdi().FillRect(guestDc, MiniGdi::Rect{ rect->left, rect->top, rect->right, rect->bottom });
    manager->Gdi().SelectBrush(guestDc, previous, nullptr);
    if (temporarySystemBrush)
    {
        manager->Gdi().DeleteObject(guestBrush);
    }
    BridgeSetLastError(ERROR_SUCCESS);
    return 1;
}

int WINAPI Win32Bridge::Bridge::BridgeDrawTextW(HDC dc, LPWSTR text, int characterCount, LPRECT rect, UINT format)
{
    GuestWindowManager* manager = CurrentManagerOrFail();
    if (!manager)
    {
        return 0;
    }
    if (!rect || !text || characterCount < -1)
    {
        BridgeSetLastError(ERROR_INVALID_PARAMETER);
        return 0;
    }

    constexpr size_t MaximumDrawTextCharacters = 1024 * 1024;
    size_t count = static_cast<size_t>(characterCount);
    if (characterCount == -1)
    {
        count = 0;
        while (count < MaximumDrawTextCharacters && text[count] != L'\0')
        {
            ++count;
        }
        if (count == MaximumDrawTextCharacters)
        {
            BridgeSetLastError(ERROR_INVALID_PARAMETER);
            return 0;
        }
    }

    const MiniGdi::DcHandle guestDc = manager->GuestDcHandle(dc);
    if (guestDc == MiniGdi::InvalidDc)
    {
        BridgeSetLastError(ERROR_INVALID_HANDLE);
        return 0;
    }

    const bool processPrefix = (format & GuestAbi::DrawTextNoPrefix) == 0;
    const bool singleLine = (format & GuestAbi::DrawTextSingleLine) != 0;
    const auto prepareDisplayText = [processPrefix, singleLine](
        const std::wstring& source, std::size_t* mnemonicPosition)
    {
        std::wstring prepared;
        prepared.reserve(source.size());
        if (mnemonicPosition)
            *mnemonicPosition = static_cast<std::size_t>(-1);
        for (std::size_t index = 0; index < source.size(); ++index)
        {
            wchar_t character = source[index];
            if (singleLine && (character == L'\r' || character == L'\n'))
            {
                if (character == L'\r' && index + 1 < source.size() &&
                    source[index + 1] == L'\n') ++index;
                prepared.push_back(L' ');
                continue;
            }
            if (processPrefix && character == L'&')
            {
                if (index + 1 < source.size() && source[index + 1] == L'&')
                {
                    prepared.push_back(L'&');
                    ++index;
                }
                else if (index + 1 < source.size() && mnemonicPosition &&
                    *mnemonicPosition == static_cast<std::size_t>(-1))
                {
                    *mnemonicPosition = prepared.size();
                }
                continue;
            }
            prepared.push_back(character);
        }
        return prepared;
    };

    std::wstring sourceText(text, text + count);
    std::wstring displayText = prepareDisplayText(sourceText, nullptr);
    bool modifiedByEllipsis = false;
    const bool calculate = (format & GuestAbi::DrawTextCalcRect) != 0;
    const bool mayModify = (format & GuestAbi::DrawTextModifyString) != 0 &&
        (format & (GuestAbi::DrawTextEndEllipsis |
            GuestAbi::DrawTextPathEllipsis |
            GuestAbi::DrawTextWordEllipsis)) != 0;
    const auto boundedRectExtent = [](LONG end, LONG start)
    {
        const std::int64_t extent = static_cast<std::int64_t>(end) - start;
        if (extent <= 0) return 0;
        if (extent > (std::numeric_limits<int>::max)())
            return (std::numeric_limits<int>::max)();
        return static_cast<int>(extent);
    };
    const int availableWidth = boundedRectExtent(rect->right, rect->left);
    const int availableHeight = calculate
        ? 1024 * 1024
        : boundedRectExtent(rect->bottom, rect->top);
    const auto measuredWidth = [manager, guestDc](const std::wstring& value)
    {
        MiniGdi::Size measured{};
        if (!manager->Gdi().GetTextExtentW(
            guestDc, value.data(), value.size(), &measured))
            return (std::numeric_limits<int>::max)();
        return measured.width;
    };
    MiniGdi::TextLayoutOptions fitOptions;
    fitOptions.wordWrap = !singleLine &&
        (format & GuestAbi::DrawTextWordBreak) != 0;
    fitOptions.includeExternalLeading =
        (format & GuestAbi::DrawTextExternalLeading) != 0;
    fitOptions.clipToLayout = false;
    if ((format & GuestAbi::DrawTextExpandTabs) != 0)
    {
        unsigned tabCharacters = 8;
        if ((format & GuestAbi::DrawTextTabStop) != 0)
        {
            const unsigned requested = (format >> 8) & 0xffu;
            if (requested != 0) tabCharacters = requested;
        }
        MiniGdi::FontMetrics fontMetrics{};
        if (manager->Gdi().GetSelectedFontMetrics(guestDc, &fontMetrics))
            fitOptions.tabStop = static_cast<float>((std::max)(1,
                fontMetrics.averageWidth) * tabCharacters);
    }
    const auto fitsInLayout = [manager, guestDc, availableWidth, availableHeight,
        &fitOptions](const std::wstring& value)
    {
        MiniGdi::Size measured{};
        if (!manager->Gdi().DrawTextW(guestDc,
            MiniGdi::Rect{ 0, 0, availableWidth, availableHeight },
            value.data(), value.size(), fitOptions, false, &measured)) return false;
        return measured.width <= availableWidth && measured.height <= availableHeight;
    };
    const auto endEllipsify = [&fitsInLayout](
        const std::wstring& value, bool atWordBoundary)
    {
        static const std::wstring ellipsis = L"...";
        if (fitsInLayout(value)) return value;
        std::size_t low = 0;
        std::size_t high = value.size();
        while (low < high)
        {
            const std::size_t middle = low + (high - low + 1) / 2;
            if (fitsInLayout(value.substr(0, middle) + ellipsis))
                low = middle;
            else
                high = middle - 1;
        }
        std::size_t prefix = low;
        if (atWordBoundary && prefix < value.size())
        {
            while (prefix > 0 && !std::iswspace(value[prefix - 1])) --prefix;
            while (prefix > 0 && std::iswspace(value[prefix - 1])) --prefix;
        }
        return value.substr(0, prefix) + ellipsis;
    };
    const auto pathEllipsify = [&measuredWidth, &endEllipsify, availableWidth](
        const std::wstring& value)
    {
        static const std::wstring ellipsis = L"...";
        if (measuredWidth(value) <= availableWidth) return value;
        const std::size_t slash = value.find_last_of(L"\\/");
        if (slash == std::wstring::npos) return endEllipsify(value, false);
        const std::wstring suffix = value.substr(slash);
        std::size_t low = 0;
        std::size_t high = slash;
        while (low < high)
        {
            const std::size_t middle = low + (high - low + 1) / 2;
            if (measuredWidth(value.substr(0, middle) + ellipsis + suffix) <=
                availableWidth)
                low = middle;
            else
                high = middle - 1;
        }
        return value.substr(0, low) + ellipsis + suffix;
    };

    if (mayModify && !fitsInLayout(displayText))
    {
        std::wstring modified = displayText;
        if ((format & GuestAbi::DrawTextPathEllipsis) != 0)
            modified = pathEllipsify(modified);
        if ((format & GuestAbi::DrawTextEndEllipsis) != 0 &&
            !fitsInLayout(modified))
            modified = endEllipsify(modified, false);
        else if ((format & GuestAbi::DrawTextWordEllipsis) != 0 &&
            !fitsInLayout(modified))
            modified = endEllipsify(modified, true);
        if (modified != displayText)
        {
            // Win32 documents DT_MODIFYSTRING as requiring room for four
            // additional WCHARs: at most three dots plus the terminator.
            // Never exceed that contract even when the input count names a
            // buffer that was not originally null-terminated.
            const std::size_t maximumResult = count + 3;
            if (modified.size() > maximumResult) modified.resize(maximumResult);
            for (std::size_t index = 0; index < modified.size(); ++index)
                text[index] = modified[index];
            text[modified.size()] = L'\0';
            displayText = std::move(modified);
            modifiedByEllipsis = true;
        }
    }

    std::size_t mnemonic = static_cast<std::size_t>(-1);
    std::wstring layoutText = modifiedByEllipsis
        ? displayText
        : prepareDisplayText(sourceText, &mnemonic);
    if ((format & GuestAbi::DrawTextHidePrefix) != 0)
        mnemonic = static_cast<std::size_t>(-1);

    MiniGdi::TextLayoutOptions options;
    if ((format & GuestAbi::DrawTextCenter) != 0)
        options.horizontal = MiniGdi::TextHorizontalAlignment::Center;
    else if ((format & GuestAbi::DrawTextRight) != 0)
        options.horizontal = MiniGdi::TextHorizontalAlignment::Right;
    if (singleLine && (format & GuestAbi::DrawTextVCenter) != 0)
        options.vertical = MiniGdi::TextVerticalAlignment::Center;
    else if (singleLine && (format & GuestAbi::DrawTextBottom) != 0)
        options.vertical = MiniGdi::TextVerticalAlignment::Bottom;
    options.wordWrap = !singleLine &&
        (format & GuestAbi::DrawTextWordBreak) != 0;
    options.rightToLeft = (format & GuestAbi::DrawTextRtlReading) != 0;
    options.clipToLayout = (format & GuestAbi::DrawTextNoClip) == 0;
    options.includeExternalLeading =
        (format & GuestAbi::DrawTextExternalLeading) != 0;
    options.renderGlyphs = (format & GuestAbi::DrawTextPrefixOnly) == 0;
    options.mnemonicStart = mnemonic;
    if (!modifiedByEllipsis)
    {
        if ((format & GuestAbi::DrawTextPathEllipsis) != 0)
            options.trimming = MiniGdi::TextTrimming::Path;
        else if ((format & GuestAbi::DrawTextWordEllipsis) != 0)
            options.trimming = MiniGdi::TextTrimming::Word;
        else if ((format & GuestAbi::DrawTextEndEllipsis) != 0)
            options.trimming = MiniGdi::TextTrimming::Character;
    }
    options.tabStop = fitOptions.tabStop;

    constexpr int MaximumLayoutDimension = 1024 * 1024;
    MiniGdi::Rect layoutRect{ rect->left, rect->top, rect->right, rect->bottom };
    if (calculate)
    {
        if (!options.wordWrap)
            layoutRect.right = rect->left + MaximumLayoutDimension;
        layoutRect.bottom = rect->top + MaximumLayoutDimension;
    }
    MiniGdi::Size extent{};
    if (!manager->Gdi().DrawTextW(guestDc, layoutRect,
        layoutText.data(), layoutText.size(), options, !calculate, &extent))
    {
        BridgeSetLastError(ERROR_INVALID_PARAMETER);
        return 0;
    }
    if (calculate)
    {
        rect->right = rect->left + extent.width;
        rect->bottom = rect->top + extent.height;
    }

    BridgeSetLastError(ERROR_SUCCESS);
    return extent.height;
}

int WINAPI Win32Bridge::Bridge::BridgeLoadStringW(HINSTANCE instance, UINT identifier, LPWSTR buffer, int bufferCount)
{
    const int result = LoadGuestStringResource(instance, identifier, buffer, bufferCount);
    BridgeSetLastError(result >= 0 ? ERROR_SUCCESS : ERROR_RESOURCE_NAME_NOT_FOUND);
    return result;
}

BOOL WINAPI Win32Bridge::Bridge::BridgeScreenToClient(HWND window, LPPOINT point)
{
    GuestWindowManager* manager = CurrentManagerOrFail();
    if (!manager || !point)
    {
        BridgeSetLastError(ERROR_INVALID_PARAMETER);
        return FALSE;
    }

    POINT origin{};
    DWORD error = ERROR_SUCCESS;
    if (!manager->GetGuestClientOrigin(window, &origin, &error))
    {
        BridgeSetLastError(error);
        return FALSE;
    }
    point->x -= origin.x;
    point->y -= origin.y;
    BridgeSetLastError(error);
    return TRUE;
}

BOOL WINAPI Win32Bridge::Bridge::BridgeMoveWindow(HWND window, int x, int y, int width, int height, BOOL repaint)
{
    return BridgeSetWindowPos(window, nullptr, x, y, width, height,
        repaint ? SWP_NOZORDER : (SWP_NOZORDER | SWP_NOREDRAW));
}

LPWSTR WINAPI Win32Bridge::Bridge::BridgeCharUpperW(LPWSTR text)
{
    if (!text) return nullptr;
    if (reinterpret_cast<ULONG_PTR>(text) <= 0xffff)
    {
        const wchar_t character = static_cast<wchar_t>(reinterpret_cast<ULONG_PTR>(text));
        return reinterpret_cast<LPWSTR>(static_cast<ULONG_PTR>(towupper(character)));
    }
    for (wchar_t* current = text; *current; ++current) *current = towupper(*current);
    return text;
}

LPSTR WINAPI Win32Bridge::Bridge::BridgeCharPrevExA(UINT, LPCSTR textStart, LPCSTR current, DWORD)
{
    if (!textStart || !current || current <= textStart) return const_cast<LPSTR>(textStart);
    return const_cast<LPSTR>(current - 1);
}

BOOL WINAPI Win32Bridge::Bridge::BridgeSetDlgItemTextA(HWND parent, int identifier, LPCSTR text)
{
    if (!text) return BridgeSetDlgItemTextW(parent, identifier, L"");
    const int count = BridgeMultiByteToWideChar(CP_ACP, 0, text, -1, nullptr, 0);
    if (count <= 0) return FALSE;
    std::vector<wchar_t> wide(static_cast<size_t>(count));
    if (BridgeMultiByteToWideChar(CP_ACP, 0, text, -1, wide.data(), count) != count) return FALSE;
    return BridgeSetDlgItemTextW(parent, identifier, wide.data());
}

BOOL WINAPI Win32Bridge::Bridge::BridgeMapDialogRect(HWND dialog, LPRECT rect)
{
    if (!rect || !BridgeIsWindow(dialog))
    {
        BridgeSetLastError(ERROR_INVALID_PARAMETER);
        return FALSE;
    }
    if (!MapGuestDialogRect(dialog, rect))
    {
        BridgeSetLastError(ERROR_INVALID_WINDOW_HANDLE);
        return FALSE;
    }
    BridgeSetLastError(ERROR_SUCCESS);
    return TRUE;
}

BOOL WINAPI Win32Bridge::Bridge::BridgeCheckDlgButton(HWND dialog, int identifier, UINT check)
{
    return BridgeSendDlgItemMessageW(dialog, identifier, BM_SETCHECK, check, 0) != 0 || BridgeGetLastError() == ERROR_SUCCESS;
}

UINT WINAPI Win32Bridge::Bridge::BridgeIsDlgButtonChecked(HWND dialog, int identifier)
{
    return static_cast<UINT>(BridgeSendDlgItemMessageW(dialog, identifier, BM_GETCHECK, 0, 0));
}

BOOL WINAPI Win32Bridge::Bridge::BridgeCheckRadioButton(HWND dialog, int first, int last, int selected)
{
    if (first > last || selected < first || selected > last)
    {
        BridgeSetLastError(ERROR_INVALID_PARAMETER);
        return FALSE;
    }
    for (int identifier = first; identifier <= last; ++identifier)
        BridgeSendDlgItemMessageW(dialog, identifier, BM_SETCHECK, identifier == selected ? BST_CHECKED : BST_UNCHECKED, 0);
    BridgeSetLastError(ERROR_SUCCESS);
    return TRUE;
}

BOOL WINAPI Win32Bridge::Bridge::BridgeEndDialog(HWND dialog, INT_PTR result)
{
    return EndGuestResourceDialog(dialog, result);
}

BOOL WINAPI Win32Bridge::Bridge::BridgeSystemParametersInfoW(UINT action, UINT, PVOID value, UINT)
{
    if (action == SPI_GETWORKAREA && value)
    {
        *static_cast<RECT*>(value) = { 0, 0, 1280, 720 };
        BridgeSetLastError(ERROR_SUCCESS);
        return TRUE;
    }
    BridgeSetLastError(ERROR_CALL_NOT_IMPLEMENTED);
    return FALSE;
}

HMONITOR WINAPI Win32Bridge::Bridge::BridgeMonitorFromWindow(HWND, DWORD)
{
    return reinterpret_cast<HMONITOR>(static_cast<ULONG_PTR>(1));
}

namespace
{
    // MONITORINFO is a desktop-SDK type.  This layout is the guest ABI used by
    // GetMonitorInfoA and keeps the UWP translation unit desktop-header free.
    struct GuestMonitorInfo
    {
        DWORD cbSize;
        RECT rcMonitor;
        RECT rcWork;
        DWORD dwFlags;
    };
}

BOOL WINAPI Win32Bridge::Bridge::BridgeGetMonitorInfoA(HMONITOR monitor, PVOID monitorInfo)
{
    auto info = static_cast<GuestMonitorInfo*>(monitorInfo);
    if (monitor != reinterpret_cast<HMONITOR>(static_cast<ULONG_PTR>(1)) || !info || info->cbSize < sizeof(GuestMonitorInfo))
    {
        BridgeSetLastError(ERROR_INVALID_PARAMETER);
        return FALSE;
    }
    info->rcMonitor = { 0, 0, 1280, 720 };
    info->rcWork = info->rcMonitor;
    info->dwFlags = 1; // MONITORINFOF_PRIMARY
    BridgeSetLastError(ERROR_SUCCESS);
    return TRUE;
}

BOOL WINAPI Win32Bridge::Bridge::BridgeOpenClipboard(HWND)
{
    BridgeSetLastError(ERROR_SUCCESS);
    return TRUE;
}

BOOL WINAPI Win32Bridge::Bridge::BridgeCloseClipboard()
{
    BridgeSetLastError(ERROR_SUCCESS);
    return TRUE;
}

BOOL WINAPI Win32Bridge::Bridge::BridgeEmptyClipboard()
{
    BridgeSetLastError(ERROR_SUCCESS);
    return TRUE;
}

HANDLE WINAPI Win32Bridge::Bridge::BridgeSetClipboardData(UINT, HANDLE memory)
{
    BridgeSetLastError(ERROR_SUCCESS);
    return memory;
}

UINT WINAPI Win32Bridge::Bridge::BridgeRegisterClipboardFormatW(LPCWSTR)
{
    std::lock_guard<std::mutex> guard(g_menuLock);
    return g_nextClipboardFormat++;
}

HMENU WINAPI Win32Bridge::Bridge::BridgeCreateMenu()
{
    std::lock_guard<std::mutex> guard(g_menuLock);
    const ULONG_PTR handle = g_nextMenu++;
    g_menus.emplace(handle, VirtualMenu{});
    BridgeSetLastError(ERROR_SUCCESS);
    return reinterpret_cast<HMENU>(handle);
}

HMENU WINAPI Win32Bridge::Bridge::BridgeCreatePopupMenu()
{
    return BridgeCreateMenu();
}

BOOL WINAPI Win32Bridge::Bridge::BridgeDestroyMenu(HMENU menu)
{
    std::lock_guard<std::mutex> guard(g_menuLock);
    VirtualMenu* root = FindMenuLocked(menu);
    if (!root)
    {
        BridgeSetLastError(ERROR_INVALID_HANDLE);
        return FALSE;
    }
    std::vector<HMENU> pending{ menu };
    for (size_t index = 0; index < pending.size(); ++index)
    {
        VirtualMenu* current = FindMenuLocked(pending[index]);
        if (!current) continue;
        for (const auto& item : current->items)
            if (item.subMenu && std::find(pending.begin(), pending.end(), item.subMenu) == pending.end())
                pending.push_back(item.subMenu);
    }
    for (HMENU current : pending) g_menus.erase(reinterpret_cast<ULONG_PTR>(current));
    for (auto iterator = g_windowMenus.begin(); iterator != g_windowMenus.end();)
    {
        if (std::find(pending.begin(), pending.end(), iterator->second) != pending.end())
            iterator = g_windowMenus.erase(iterator);
        else ++iterator;
    }
    BridgeSetLastError(ERROR_SUCCESS);
    return TRUE;
}

BOOL WINAPI Win32Bridge::Bridge::BridgeAppendMenuW(HMENU menu, UINT flags, UINT_PTR identifier, LPCWSTR text)
{
    std::lock_guard<std::mutex> guard(g_menuLock);
    VirtualMenu* destination = FindMenuLocked(menu);
    if (!destination)
    {
        BridgeSetLastError(ERROR_INVALID_HANDLE);
        return FALSE;
    }
    VirtualMenuItem item;
    item.identifier = (flags & kMfPopup) ? 0 : static_cast<UINT>(identifier);
    item.type = flags;
    item.state = flags & (kMfGrayed | kMfDisabled | kMfChecked | kMfHighlighted | kMfDefault);
    item.subMenu = (flags & kMfPopup) ? reinterpret_cast<HMENU>(identifier) : nullptr;
    if (text)
    {
        GuestMenuItemInfoW source = {};
        source.dwTypeData = const_cast<LPWSTR>(text);
        item.text = ReadGuestMenuText(source);
    }
    destination->items.push_back(std::move(item));
    BridgeSetLastError(ERROR_SUCCESS);
    return TRUE;
}

BOOL WINAPI Win32Bridge::Bridge::BridgeInsertMenuItemW(HMENU menu, UINT item, BOOL byPosition, const void* itemInfo)
{
    GuestMenuItemInfoW sourceValue = {};
    if (!ReadGuestMenuItemInfo(itemInfo, &sourceValue) ||
        sourceValue.cbSize < GuestMenuItemInfoLegacySize)
    {
        BridgeSetLastError(ERROR_INVALID_PARAMETER);
        return FALSE;
    }
    const auto* source = &sourceValue;
    std::lock_guard<std::mutex> guard(g_menuLock);
    VirtualMenu* destination = FindMenuLocked(menu);
    if (!destination)
    {
        BridgeSetLastError(ERROR_INVALID_HANDLE);
        return FALSE;
    }
    VirtualMenuItem entry;
    if (source->fMask & kMiimId) entry.identifier = source->wID;
    if (source->fMask & (kMiimFtype | kMiimType)) entry.type = source->fType;
    if (source->fMask & kMiimState) entry.state = source->fState;
    if (source->fMask & kMiimSubmenu) entry.subMenu = source->hSubMenu;
    if (source->fMask & kMiimCheckmarks)
    {
        entry.checkedBitmap = source->hbmpChecked;
        entry.uncheckedBitmap = source->hbmpUnchecked;
    }
    if (source->fMask & kMiimBitmap) entry.itemBitmap = source->hbmpItem;
    if (source->fMask & kMiimData) entry.itemData = source->dwItemData;
    if (source->fMask & (kMiimString | kMiimType)) entry.text = ReadGuestMenuText(*source);
    size_t insertion = destination->items.size();
    if (byPosition)
    {
        insertion = (std::min)(static_cast<size_t>(item), destination->items.size());
    }
    else if (item != static_cast<UINT>(-1))
    {
        const auto before = std::find_if(destination->items.begin(), destination->items.end(),
            [item](const VirtualMenuItem& candidate) { return candidate.identifier == item; });
        if (before == destination->items.end()) return FALSE;
        insertion = static_cast<size_t>(before - destination->items.begin());
    }
    destination->items.insert(destination->items.begin() + insertion, std::move(entry));
    BridgeSetLastError(ERROR_SUCCESS);
    return TRUE;
}

int WINAPI Win32Bridge::Bridge::BridgeGetMenuItemCount(HMENU menu)
{
    std::lock_guard<std::mutex> guard(g_menuLock);
    VirtualMenu* found = FindMenuLocked(menu);
    if (!found) return -1;
    return static_cast<int>(found->items.size());
}

BOOL WINAPI Win32Bridge::Bridge::BridgeGetMenuItemInfoW(HMENU menu, UINT item, BOOL byPosition, void* itemInfo)
{
    GuestMenuItemInfoW resultValue = {};
    if (!ReadGuestMenuItemInfo(itemInfo, &resultValue) ||
        resultValue.cbSize < GuestMenuItemInfoLegacySize) return FALSE;
    auto* result = &resultValue;
    std::lock_guard<std::mutex> guard(g_menuLock);
    VirtualMenu* source = FindMenuLocked(menu);
    VirtualMenuItem* entry = source ? FindMenuItemLocked(*source, item, byPosition) : nullptr;
    if (!entry) return FALSE;
    if (result->fMask & kMiimState) result->fState = entry->state;
    if (result->fMask & kMiimId) result->wID = entry->identifier;
    if (result->fMask & kMiimSubmenu) result->hSubMenu = entry->subMenu;
    if (result->fMask & kMiimCheckmarks)
    {
        result->hbmpChecked = entry->checkedBitmap;
        result->hbmpUnchecked = entry->uncheckedBitmap;
    }
    if (result->fMask & kMiimBitmap) result->hbmpItem = entry->itemBitmap;
    if (result->fMask & kMiimData) result->dwItemData = entry->itemData;
    if (result->fMask & (kMiimFtype | kMiimType)) result->fType = entry->type;
    if (result->fMask & (kMiimString | kMiimType))
    {
        if (result->dwTypeData && result->cch)
        {
            if (!CopyGuestMenuText(result->dwTypeData, result->cch, entry->text)) return FALSE;
        }
        result->cch = static_cast<UINT>(entry->text.size());
    }
    return WriteGuestMenuItemInfo(itemInfo, resultValue) ? TRUE : FALSE;
}

BOOL WINAPI Win32Bridge::Bridge::BridgeSetMenuItemInfoW(HMENU menu, UINT item, BOOL byPosition, const void* itemInfo)
{
    GuestMenuItemInfoW updateValue = {};
    if (!ReadGuestMenuItemInfo(itemInfo, &updateValue) ||
        updateValue.cbSize < GuestMenuItemInfoLegacySize) return FALSE;
    const auto* update = &updateValue;
    std::lock_guard<std::mutex> guard(g_menuLock);
    VirtualMenu* destination = FindMenuLocked(menu);
    VirtualMenuItem* entry = destination ? FindMenuItemLocked(*destination, item, byPosition) : nullptr;
    if (!entry) return FALSE;
    if (update->fMask & kMiimState) entry->state = update->fState;
    if (update->fMask & kMiimId) entry->identifier = update->wID;
    if (update->fMask & kMiimSubmenu) entry->subMenu = update->hSubMenu;
    if (update->fMask & kMiimCheckmarks)
    {
        entry->checkedBitmap = update->hbmpChecked;
        entry->uncheckedBitmap = update->hbmpUnchecked;
    }
    if (update->fMask & kMiimBitmap) entry->itemBitmap = update->hbmpItem;
    if (update->fMask & kMiimData) entry->itemData = update->dwItemData;
    if (update->fMask & (kMiimFtype | kMiimType)) entry->type = update->fType;
    if (update->fMask & (kMiimString | kMiimType)) entry->text = ReadGuestMenuText(*update);
    return TRUE;
}

UINT WINAPI Win32Bridge::Bridge::BridgeEnableMenuItem(HMENU menu, UINT item, UINT flags)
{
    std::lock_guard<std::mutex> guard(g_menuLock);
    VirtualMenu* destination = FindMenuLocked(menu);
    VirtualMenuItem* entry = destination ? FindMenuItemLocked(*destination, item, (flags & kMfByPosition) != 0) : nullptr;
    if (!entry) return static_cast<UINT>(-1);
    const UINT previous = entry->state;
    entry->state = (entry->state & ~(kMfGrayed | kMfDisabled)) |
        (flags & (kMfGrayed | kMfDisabled));
    return previous;
}

DWORD WINAPI Win32Bridge::Bridge::BridgeCheckMenuItem(HMENU menu, UINT item, UINT flags)
{
    std::lock_guard<std::mutex> guard(g_menuLock);
    VirtualMenu* destination = FindMenuLocked(menu);
    VirtualMenuItem* entry = destination ? FindMenuItemLocked(*destination, item, (flags & kMfByPosition) != 0) : nullptr;
    if (!entry) return static_cast<DWORD>(-1);
    const DWORD previous = entry->state;
    entry->state = (entry->state & ~kMfChecked) | (flags & kMfChecked);
    return previous;
}

BOOL WINAPI Win32Bridge::Bridge::BridgeCheckMenuRadioItem(HMENU menu, UINT first, UINT last, UINT selected, UINT flags)
{
    std::lock_guard<std::mutex> guard(g_menuLock);
    VirtualMenu* destination = FindMenuLocked(menu);
    if (!destination) return FALSE;
    for (UINT index = first; index <= last; ++index)
    {
        VirtualMenuItem* entry = FindMenuItemLocked(*destination, index, (flags & kMfByPosition) != 0);
        if (entry)
        {
            entry->type |= kMftRadioCheck;
            entry->state = (entry->state & ~kMfChecked) | (index == selected ? kMfChecked : 0);
        }
    }
    return TRUE;
}

BOOL WINAPI Win32Bridge::Bridge::BridgeRemoveMenu(HMENU menu, UINT item, UINT flags)
{
    std::lock_guard<std::mutex> guard(g_menuLock);
    VirtualMenu* destination = FindMenuLocked(menu);
    if (!destination) return FALSE;
    for (auto iterator = destination->items.begin(); iterator != destination->items.end(); ++iterator)
    {
        const bool match = (flags & kMfByPosition) ? static_cast<UINT>(iterator - destination->items.begin()) == item : iterator->identifier == item;
        if (match) { destination->items.erase(iterator); return TRUE; }
    }
    return FALSE;
}

BOOL WINAPI Win32Bridge::Bridge::BridgeDeleteMenu(HMENU menu, UINT item, UINT flags)
{
    HMENU child = nullptr;
    {
        std::lock_guard<std::mutex> guard(g_menuLock);
        VirtualMenu* destination = FindMenuLocked(menu);
        VirtualMenuItem* entry = destination ? FindMenuItemLocked(
            *destination, item, (flags & kMfByPosition) != 0) : nullptr;
        if (!entry) return FALSE;
        child = entry->subMenu;
    }
    if (!BridgeRemoveMenu(menu, item, flags)) return FALSE;
    return !child || BridgeDestroyMenu(child);
}

UINT WINAPI Win32Bridge::Bridge::BridgeGetMenuItemID(HMENU menu, int position)
{
    std::lock_guard<std::mutex> guard(g_menuLock);
    VirtualMenu* source = FindMenuLocked(menu);
    if (!source || position < 0 || static_cast<size_t>(position) >= source->items.size())
        return static_cast<UINT>(-1);
    const VirtualMenuItem& entry = source->items[static_cast<size_t>(position)];
    return entry.subMenu ? static_cast<UINT>(-1) : entry.identifier;
}

UINT WINAPI Win32Bridge::Bridge::BridgeGetMenuState(HMENU menu, UINT item, UINT flags)
{
    std::lock_guard<std::mutex> guard(g_menuLock);
    VirtualMenu* source = FindMenuLocked(menu);
    VirtualMenuItem* entry = source ? FindMenuItemLocked(
        *source, item, (flags & kMfByPosition) != 0) : nullptr;
    if (!entry) return static_cast<UINT>(-1);
    UINT result = entry->type | entry->state | (entry->subMenu ? kMfPopup : 0);
    if (entry->subMenu)
    {
        const VirtualMenu* child = FindMenuLocked(entry->subMenu);
        if (child) result |= (static_cast<UINT>((std::min)(child->items.size(),
            static_cast<size_t>(0xff))) << 8);
    }
    return result;
}

int WINAPI Win32Bridge::Bridge::BridgeGetMenuStringW(
    HMENU menu, UINT item, LPWSTR text, int count, UINT flags)
{
    std::lock_guard<std::mutex> guard(g_menuLock);
    VirtualMenu* source = FindMenuLocked(menu);
    VirtualMenuItem* entry = source ? FindMenuItemLocked(
        *source, item, (flags & kMfByPosition) != 0) : nullptr;
    if (!entry) return 0;
    if (!text || count <= 0) return static_cast<int>(entry->text.size());
    if (!CopyGuestMenuText(text, static_cast<size_t>(count), entry->text)) return 0;
    return static_cast<int>((std::min)(entry->text.size(), static_cast<size_t>(count - 1)));
}

BOOL WINAPI Win32Bridge::Bridge::BridgeSetMenuDefaultItem(HMENU menu, UINT item, UINT byPosition)
{
    std::lock_guard<std::mutex> guard(g_menuLock);
    VirtualMenu* destination = FindMenuLocked(menu);
    if (!destination) return FALSE;
    for (auto& entry : destination->items) entry.state &= ~kMfDefault;
    if (item == static_cast<UINT>(-1)) return TRUE;
    VirtualMenuItem* selected = FindMenuItemLocked(*destination, item, byPosition != FALSE);
    if (!selected) return FALSE;
    selected->state |= kMfDefault;
    return TRUE;
}

UINT WINAPI Win32Bridge::Bridge::BridgeGetMenuDefaultItem(HMENU menu, UINT byPosition, UINT)
{
    std::lock_guard<std::mutex> guard(g_menuLock);
    VirtualMenu* source = FindMenuLocked(menu);
    if (!source) return static_cast<UINT>(-1);
    for (size_t index = 0; index < source->items.size(); ++index)
        if (source->items[index].state & kMfDefault)
            return byPosition ? static_cast<UINT>(index) : source->items[index].identifier;
    return static_cast<UINT>(-1);
}

BOOL WINAPI Win32Bridge::Bridge::BridgeIsMenu(HMENU menu)
{
    std::lock_guard<std::mutex> guard(g_menuLock);
    return FindMenuLocked(menu) ? TRUE : FALSE;
}

BOOL WINAPI Win32Bridge::Bridge::BridgeHiliteMenuItem(HWND window, HMENU menu, UINT item, UINT flags)
{
    {
        std::lock_guard<std::mutex> guard(g_menuLock);
        VirtualMenu* destination = FindMenuLocked(menu);
        VirtualMenuItem* entry = destination ? FindMenuItemLocked(
            *destination, item, (flags & kMfByPosition) != 0) : nullptr;
        if (!entry) return FALSE;
        entry->state = (entry->state & ~kMfHighlighted) | (flags & kMfHighlighted);
    }
    if (GuestWindowManager* manager = CurrentGuestWindowManager())
        manager->InvalidateGuestRect(window, nullptr, FALSE, nullptr);
    return TRUE;
}

HMENU WINAPI Win32Bridge::Bridge::BridgeGetSubMenu(HMENU menu, int position)
{
    std::lock_guard<std::mutex> guard(g_menuLock);
    VirtualMenu* source = FindMenuLocked(menu);
    return source && position >= 0 && static_cast<size_t>(position) < source->items.size() ? source->items[position].subMenu : nullptr;
}

HMENU WINAPI Win32Bridge::Bridge::BridgeGetMenu(HWND window)
{
    std::lock_guard<std::mutex> guard(g_menuLock);
    const auto found = g_windowMenus.find(reinterpret_cast<ULONG_PTR>(window));
    return found == g_windowMenus.end() ? nullptr : found->second;
}

BOOL WINAPI Win32Bridge::Bridge::BridgeSetMenu(HWND window, HMENU menu)
{
    if (window && !BridgeIsWindow(window)) return FALSE;
    {
        std::lock_guard<std::mutex> guard(g_menuLock);
        if (menu && !FindMenuLocked(menu)) return FALSE;
        if (menu) g_windowMenus[reinterpret_cast<ULONG_PTR>(window)] = menu;
        else g_windowMenus.erase(reinterpret_cast<ULONG_PTR>(window));
    }
    if (GuestWindowManager* manager = CurrentGuestWindowManager())
    {
        DWORD ignored = ERROR_SUCCESS;
        manager->SetGuestWindowMenuBar(window, menu ? TRUE : FALSE, &ignored);
    }
    return TRUE;
}

std::vector<Win32Bridge::Bridge::GuestMenuVisualItem>
Win32Bridge::Bridge::GetGuestMenuItems(HMENU menuHandle)
{
    std::vector<GuestMenuVisualItem> result;
    std::lock_guard<std::mutex> guard(g_menuLock);
    const VirtualMenu* menu = FindMenuLocked(menuHandle);
    if (!menu)
    {
        return result;
    }
    result.reserve(menu->items.size());
    for (const auto& item : menu->items)
    {
        GuestMenuVisualItem visual;
        visual.identifier = item.identifier;
        visual.type = item.type;
        visual.state = item.state;
        visual.subMenu = item.subMenu;
        visual.checkedBitmap = item.checkedBitmap;
        visual.uncheckedBitmap = item.uncheckedBitmap;
        visual.itemBitmap = item.itemBitmap;
        visual.itemData = item.itemData;
        visual.text = item.text;
        result.push_back(std::move(visual));
    }
    return result;
}

std::vector<Win32Bridge::Bridge::GuestMenuVisualItem>
Win32Bridge::Bridge::GetGuestMenuBarItems(HWND window)
{
    HMENU menu = nullptr;
    {
        std::lock_guard<std::mutex> guard(g_menuLock);
        const auto found = g_windowMenus.find(reinterpret_cast<ULONG_PTR>(window));
        if (found == g_windowMenus.end())
        {
            return {};
        }
        menu = found->second;
    }
    return GetGuestMenuItems(menu);
}

BOOL WINAPI Win32Bridge::Bridge::BridgeDrawMenuBar(HWND window)
{
    if (!BridgeIsWindow(window)) return FALSE;
    if (GuestWindowManager* manager = CurrentGuestWindowManager())
    {
        DWORD ignored = ERROR_SUCCESS;
        manager->SetGuestWindowMenuBar(window, BridgeGetMenu(window) ? TRUE : FALSE, &ignored);
        manager->InvalidateGuestRect(window, nullptr, TRUE, &ignored);
    }
    return TRUE;
}
HMENU WINAPI Win32Bridge::Bridge::BridgeLoadMenuW(HINSTANCE, LPCWSTR resource)
{
    const HMENU menu = LoadGuestMenuResource(resource);
    BridgeSetLastError(menu ? ERROR_SUCCESS : ERROR_RESOURCE_NAME_NOT_FOUND);
    if (menu)
    {
        RuntimeDiagnostics::Record(L"MENU: loaded MENU resource into virtual menu model.");
    }
    else
    {
        RuntimeDiagnostics::Record(L"MENU: requested resource was unavailable or malformed.");
    }
    return menu;
}
UINT WINAPI Win32Bridge::Bridge::BridgeTrackPopupMenuEx(HMENU menu, UINT flags, int x, int y, HWND owner, const RECT* excludeRect)
{
    {
        std::lock_guard<std::mutex> guard(g_menuLock);
        if (!FindMenuLocked(menu))
        {
            BridgeSetLastError(ERROR_INVALID_MENU_HANDLE);
            return 0;
        }
    }
    GuestWindowManager* manager = CurrentGuestWindowManager();
    if (!manager)
    {
        BridgeSetLastError(ERROR_INVALID_WINDOW_HANDLE);
        return 0;
    }
    DWORD error = ERROR_SUCCESS;
    const UINT result = manager->TrackGuestPopupMenu(menu, flags, x, y, owner, excludeRect, &error);
    BridgeSetLastError(error);
    return result;
}
HANDLE WINAPI Win32Bridge::Bridge::BridgeLoadAcceleratorsW(HINSTANCE, LPCWSTR) { return reinterpret_cast<HANDLE>(static_cast<ULONG_PTR>(1)); }
int WINAPI Win32Bridge::Bridge::BridgeTranslateAcceleratorW(HWND, HANDLE, const GuestAbi::Message*) { return 0; }
UINT WINAPI Win32Bridge::Bridge::BridgeGetDialogBaseUnits()
{
    return static_cast<UINT>(GuestMetrics::TextWidth) |
        (static_cast<UINT>(GuestMetrics::TextHeight) << 16);
}

HWND WINAPI Win32Bridge::Bridge::BridgeChildWindowFromPointEx(HWND parent, POINT point, UINT flags)
{
    GuestWindowManager* manager = CurrentManagerOrFail();
    if (!manager) return nullptr;
    DWORD error = ERROR_SUCCESS;
    const HWND result = manager->ChildGuestWindowFromPoint(parent, point, flags, &error);
    BridgeSetLastError(error);
    return result;
}
BOOL WINAPI Win32Bridge::Bridge::BridgeTrackPopupMenu(
    HMENU menu, UINT flags, int x, int y, int, HWND owner, const RECT*)
{
    return static_cast<BOOL>(BridgeTrackPopupMenuEx(menu, flags, x, y, owner, nullptr));
}
BOOL WINAPI Win32Bridge::Bridge::BridgeEndMenu()
{
    GuestWindowManager* manager = CurrentGuestWindowManager();
    if (!manager) return FALSE;
    DWORD error = ERROR_SUCCESS;
    const BOOL result = manager->EndGuestMenu(&error);
    BridgeSetLastError(error);
    return result;
}

HWND WINAPI Win32Bridge::Bridge::BridgeWindowFromPoint(POINT point)
{
    GuestWindowManager* manager = CurrentManagerOrFail();
    if (!manager) return nullptr;
    DWORD error = ERROR_SUCCESS;
    const HWND result = manager->GuestWindowFromPoint(point, &error);
    BridgeSetLastError(error);
    return result;
}
UINT WINAPI Win32Bridge::Bridge::BridgeMapVirtualKeyW(UINT code, UINT) { return code; }
int WINAPI Win32Bridge::Bridge::BridgeMapWindowPoints(
    HWND from,
    HWND to,
    LPPOINT points,
    UINT count)
{
    GuestWindowManager* manager = CurrentManagerOrFail();
    if (!manager || (count != 0 && !points))
    {
        BridgeSetLastError(ERROR_INVALID_PARAMETER);
        return 0;
    }

    POINT fromOrigin{};
    POINT toOrigin{};
    DWORD error = ERROR_SUCCESS;
    if (from && !manager->GetGuestClientOrigin(from, &fromOrigin, &error))
    {
        BridgeSetLastError(error);
        return 0;
    }
    if (to && !manager->GetGuestClientOrigin(to, &toOrigin, &error))
    {
        BridgeSetLastError(error);
        return 0;
    }

    const int deltaX = fromOrigin.x - toOrigin.x;
    const int deltaY = fromOrigin.y - toOrigin.y;
    for (UINT index = 0; index < count; ++index)
    {
        points[index].x += deltaX;
        points[index].y += deltaY;
    }

    BridgeSetLastError(ERROR_SUCCESS);
    return static_cast<int>(MAKELONG(LOWORD(deltaX), LOWORD(deltaY)));
}

BOOL WINAPI Win32Bridge::Bridge::BridgeClientToScreen(HWND window, LPPOINT point)
{
    GuestWindowManager* manager = CurrentManagerOrFail();
    if (!manager || !point)
    {
        BridgeSetLastError(ERROR_INVALID_PARAMETER);
        return FALSE;
    }

    POINT origin{};
    DWORD error = ERROR_SUCCESS;
    if (!manager->GetGuestClientOrigin(window, &origin, &error))
    {
        BridgeSetLastError(error);
        return FALSE;
    }
    point->x += origin.x;
    point->y += origin.y;
    BridgeSetLastError(error);
    return TRUE;
}

BOOL WINAPI Win32Bridge::Bridge::BridgeGetWindowPlacement(HWND window, void* placement)
{
    auto* result = static_cast<GuestWindowPlacement*>(placement);
    if (!result || result->length < sizeof(GuestWindowPlacement) || !BridgeGetWindowRect(window, &result->normalPosition)) return FALSE;
    result->flags = 0;
    // A number of conventional desktop applications use
    // GetWindowPlacement/SetWindowPlacement instead of ShowWindow during
    // their first startup. Preserve the current visibility so their later
    // showCmd update can make a newly-created top-level guest visible.
    result->showCmd = BridgeIsWindowVisible(window) ? 1u : 0u;
    result->minPosition = { 0, 0 };
    result->maxPosition = { 0, 0 };
    return TRUE;
}

BOOL WINAPI Win32Bridge::Bridge::BridgeSetWindowPlacement(HWND window, const void* placement)
{
    const auto* source = static_cast<const GuestWindowPlacement*>(placement);
    if (!source || source->length < sizeof(GuestWindowPlacement)) return FALSE;
    const RECT& rect = source->normalPosition;
    if (!BridgeSetWindowPos(window, nullptr, rect.left, rect.top, rect.right - rect.left, rect.bottom - rect.top, 0))
    {
        return FALSE;
    }

    // SW_HIDE is the only hiding command. Every other documented showCmd
    // value (normal, minimized, maximized, restore, and default) is presented
    // as an active guest surface in this initial client-only window model.
    // Do not return ShowWindow's "previously visible" result: successful
    // SetWindowPlacement reports TRUE even when it changed hidden -> shown.
    BridgeShowWindow(window, source->showCmd == 0 ? 0 : 1);
    RuntimeDiagnostics::Record(
        L"WINDOW PLACEMENT: showCmd " + std::to_wstring(source->showCmd) + L" applied.");
    return TRUE;
}

HBITMAP WINAPI Win32Bridge::Bridge::BridgeLoadBitmapW(HINSTANCE, LPCWSTR resource)
{
    GuestWindowManager* manager = CurrentManagerOrFail();
    MiniGdi::Surface image;
    if (!manager || !resource || !LoadBitmapResourcePixels(resource, &image))
    {
        BridgeSetLastError(ERROR_RESOURCE_NAME_NOT_FOUND);
        return nullptr;
    }
    const MiniGdi::ObjectHandle bitmap = manager->Gdi().CreateBitmap(
        image.Width(), image.Height(), MiniGdi::Transparent);
    MiniGdi::Surface* target = manager->Gdi().GetBitmapSurface(bitmap);
    if (bitmap == MiniGdi::InvalidObject || !target)
    {
        BridgeSetLastError(ERROR_NOT_ENOUGH_MEMORY);
        return nullptr;
    }
    target->Pixels() = image.Pixels();
    RuntimeDiagnostics::Record(L"BITMAP: decoded a guest RT_BITMAP resource.");
    BridgeSetLastError(ERROR_SUCCESS);
    return reinterpret_cast<HBITMAP>(static_cast<ULONG_PTR>(bitmap));
}
BOOL WINAPI Win32Bridge::Bridge::BridgeGetClassInfoW(HINSTANCE, LPCWSTR, GuestAbi::WndClassW*) { BridgeSetLastError(ERROR_INVALID_PARAMETER); return FALSE; }
LRESULT WINAPI Win32Bridge::Bridge::BridgeCallWindowProcW(GuestAbi::WndProc procedure, HWND window, UINT message, WPARAM wParam, LPARAM lParam)
{
    if (!procedure)
    {
        BridgeSetLastError(ERROR_INVALID_PARAMETER);
        return 0;
    }

    DWORD guestException = ERROR_SUCCESS;
    const LRESULT result = InvokeGuestSubclassProcedure(
        procedure,
        window,
        message,
        wParam,
        lParam,
        &guestException);
    if (guestException != ERROR_SUCCESS)
    {
        RuntimeDiagnostics::Record(
            L"GUEST SUBCLASS EXCEPTION: code " +
            std::to_wstring(static_cast<unsigned long>(guestException)) +
            L", message " + std::to_wstring(message) +
            L", window " + std::to_wstring(reinterpret_cast<ULONG_PTR>(window)) + L".");
        BridgeSetLastError(ERROR_EXCEPTION_IN_SERVICE);
        return 0;
    }
    BridgeSetLastError(ERROR_SUCCESS);
    return result;
}

INT_PTR WINAPI Win32Bridge::Bridge::BridgeDialogBoxParamW(HINSTANCE instance, LPCWSTR templateName, HWND parent, DLGPROC dialogProcedure, LPARAM initParameter)
{
    return ShowGuestDialogFromResource(instance, templateName, parent, dialogProcedure, initParameter);
}

BOOL WINAPI Win32Bridge::Bridge::BridgeIsDialogMessageW(
    HWND dialog,
    const GuestAbi::Message* message)
{
    if (!message) return FALSE;
    GuestAbi::Message copy{};
    if (!ReadGuestMessageValue(message, &copy)) return FALSE;
    MSG native{};
    native.hwnd = copy.hwnd;
    native.message = copy.message;
    native.wParam = copy.wParam;
    native.lParam = copy.lParam;
    return HandleGuestDialogMessage(dialog, &native);
}

ImportResolution Win32Bridge::Bridge::ResolveUser32Import(const ImportedSymbol& symbol)
{
    ImportResolution resolution = CompatibilityCatalog::Resolve(symbol);
    if (symbol.importedByOrdinal || !IsUserLibrary(symbol.library))
    {
        return resolution;
    }

    if (_wcsicmp(symbol.name.c_str(), L"registerclassexw") == 0)
        resolution.targetAddress = reinterpret_cast<ULONGLONG>(&BridgeRegisterClassExW);
    else if (_wcsicmp(symbol.name.c_str(), L"registerclassw") == 0)
        resolution.targetAddress = reinterpret_cast<ULONGLONG>(&BridgeRegisterClassW);
    else if (_wcsicmp(symbol.name.c_str(), L"createwindowexw") == 0)
        resolution.targetAddress = reinterpret_cast<ULONGLONG>(&BridgeCreateWindowExW);
    else if (_wcsicmp(symbol.name.c_str(), L"loadcursorw") == 0)
        resolution.targetAddress = reinterpret_cast<ULONGLONG>(&BridgeLoadCursorW);
    else if (_wcsicmp(symbol.name.c_str(), L"loadiconw") == 0)
        resolution.targetAddress = reinterpret_cast<ULONGLONG>(&BridgeLoadIconW);
    else if (_wcsicmp(symbol.name.c_str(), L"setcursor") == 0)
        resolution.targetAddress = reinterpret_cast<ULONGLONG>(&BridgeSetCursor);
    else if (_wcsicmp(symbol.name.c_str(), L"destroycursor") == 0)
        resolution.targetAddress = reinterpret_cast<ULONGLONG>(&BridgeDestroyCursor);
    else if (_wcsicmp(symbol.name.c_str(), L"destroyicon") == 0)
        resolution.targetAddress = reinterpret_cast<ULONGLONG>(&BridgeDestroyIcon);
    else if (_wcsicmp(symbol.name.c_str(), L"getsystemmetrics") == 0)
        resolution.targetAddress = reinterpret_cast<ULONGLONG>(&BridgeGetSystemMetrics);
    else if (_wcsicmp(symbol.name.c_str(), L"getsyscolor") == 0)
        resolution.targetAddress = reinterpret_cast<ULONGLONG>(&BridgeGetSysColor);
    else if (_wcsicmp(symbol.name.c_str(), L"getsyscolorbrush") == 0)
        resolution.targetAddress = reinterpret_cast<ULONGLONG>(&BridgeGetSysColorBrush);
    else if (_wcsicmp(symbol.name.c_str(), L"adjustwindowrect") == 0)
        resolution.targetAddress = reinterpret_cast<ULONGLONG>(&BridgeAdjustWindowRect);
    else if (_wcsicmp(symbol.name.c_str(), L"adjustwindowrectex") == 0)
        resolution.targetAddress = reinterpret_cast<ULONGLONG>(&BridgeAdjustWindowRectEx);
    if (_wcsicmp(symbol.name.c_str(), L"destroywindow") == 0)
        resolution.targetAddress = reinterpret_cast<ULONGLONG>(&BridgeDestroyWindow);
    else if (_wcsicmp(symbol.name.c_str(), L"showwindow") == 0)
        resolution.targetAddress = reinterpret_cast<ULONGLONG>(&BridgeShowWindow);
    else if (_wcsicmp(symbol.name.c_str(), L"getclientrect") == 0)
        resolution.targetAddress = reinterpret_cast<ULONGLONG>(&BridgeGetClientRect);
    else if (_wcsicmp(symbol.name.c_str(), L"getwindowrect") == 0)
        resolution.targetAddress = reinterpret_cast<ULONGLONG>(&BridgeGetWindowRect);
    else if (_wcsicmp(symbol.name.c_str(), L"setwindowtextw") == 0)
        resolution.targetAddress = reinterpret_cast<ULONGLONG>(&BridgeSetWindowTextW);
    else if (_wcsicmp(symbol.name.c_str(), L"getwindowtextw") == 0)
        resolution.targetAddress = reinterpret_cast<ULONGLONG>(&BridgeGetWindowTextW);
    else if (_wcsicmp(symbol.name.c_str(), L"getwindowtextlengthw") == 0)
        resolution.targetAddress = reinterpret_cast<ULONGLONG>(&BridgeGetWindowTextLengthW);
    else if (_wcsicmp(symbol.name.c_str(), L"getdlgitem") == 0)
        resolution.targetAddress = reinterpret_cast<ULONGLONG>(&BridgeGetDlgItem);
    else if (_wcsicmp(symbol.name.c_str(), L"getdlgctrlid") == 0)
        resolution.targetAddress = reinterpret_cast<ULONGLONG>(&BridgeGetDlgCtrlID);
    else if (_wcsicmp(symbol.name.c_str(), L"setdlgitemtextw") == 0)
        resolution.targetAddress = reinterpret_cast<ULONGLONG>(&BridgeSetDlgItemTextW);
    else if (_wcsicmp(symbol.name.c_str(), L"getdlgitemtextw") == 0)
        resolution.targetAddress = reinterpret_cast<ULONGLONG>(&BridgeGetDlgItemTextW);
    else if (_wcsicmp(symbol.name.c_str(), L"senddlgitemmessagew") == 0)
        resolution.targetAddress = reinterpret_cast<ULONGLONG>(&BridgeSendDlgItemMessageW);
    else if (_wcsicmp(symbol.name.c_str(), L"iswindow") == 0)
        resolution.targetAddress = reinterpret_cast<ULONGLONG>(&BridgeIsWindow);
    else if (_wcsicmp(symbol.name.c_str(), L"iswindowvisible") == 0)
        resolution.targetAddress = reinterpret_cast<ULONGLONG>(&BridgeIsWindowVisible);
    else if (_wcsicmp(symbol.name.c_str(), L"iswindowenabled") == 0)
        resolution.targetAddress = reinterpret_cast<ULONGLONG>(&BridgeIsWindowEnabled);
    else if (_wcsicmp(symbol.name.c_str(), L"iszoomed") == 0)
        resolution.targetAddress = reinterpret_cast<ULONGLONG>(&BridgeIsZoomed);
    else if (_wcsicmp(symbol.name.c_str(), L"enablewindow") == 0)
        resolution.targetAddress = reinterpret_cast<ULONGLONG>(&BridgeEnableWindow);
    else if (_wcsicmp(symbol.name.c_str(), L"setfocus") == 0)
        resolution.targetAddress = reinterpret_cast<ULONGLONG>(&BridgeSetFocus);
    else if (_wcsicmp(symbol.name.c_str(), L"getfocus") == 0)
        resolution.targetAddress = reinterpret_cast<ULONGLONG>(&BridgeGetFocus);
    else if (_wcsicmp(symbol.name.c_str(), L"setcapture") == 0)
        resolution.targetAddress = reinterpret_cast<ULONGLONG>(&BridgeSetCapture);
    else if (_wcsicmp(symbol.name.c_str(), L"releasecapture") == 0)
        resolution.targetAddress = reinterpret_cast<ULONGLONG>(&BridgeReleaseCapture);
    else if (_wcsicmp(symbol.name.c_str(), L"getcapture") == 0)
        resolution.targetAddress = reinterpret_cast<ULONGLONG>(&BridgeGetCapture);
    if (_wcsicmp(symbol.name.c_str(), L"getparent") == 0)
        resolution.targetAddress = reinterpret_cast<ULONGLONG>(&BridgeGetParent);
    else if (_wcsicmp(symbol.name.c_str(), L"getwindowlongw") == 0)
        resolution.targetAddress = reinterpret_cast<ULONGLONG>(&BridgeGetWindowLongW);
    else if (_wcsicmp(symbol.name.c_str(), L"setwindowlongw") == 0)
        resolution.targetAddress = reinterpret_cast<ULONGLONG>(&BridgeSetWindowLongW);
    else if (_wcsicmp(symbol.name.c_str(), L"getwindowlongptrw") == 0)
        resolution.targetAddress = reinterpret_cast<ULONGLONG>(&BridgeGetWindowLongPtrW);
    else if (_wcsicmp(symbol.name.c_str(), L"setwindowlongptrw") == 0)
        resolution.targetAddress = reinterpret_cast<ULONGLONG>(&BridgeSetWindowLongPtrW);
    else if (_wcsicmp(symbol.name.c_str(), L"setwindowpos") == 0)
        resolution.targetAddress = reinterpret_cast<ULONGLONG>(&BridgeSetWindowPos);
    else if (_wcsicmp(symbol.name.c_str(), L"settimer") == 0)
        resolution.targetAddress = reinterpret_cast<ULONGLONG>(&BridgeSetTimer);
    else if (_wcsicmp(symbol.name.c_str(), L"killtimer") == 0)
        resolution.targetAddress = reinterpret_cast<ULONGLONG>(&BridgeKillTimer);
    else if (_wcsicmp(symbol.name.c_str(), L"defwindowprocw") == 0)
        resolution.targetAddress = reinterpret_cast<ULONGLONG>(&BridgeDefWindowProcW);
    else if (_wcsicmp(symbol.name.c_str(), L"postmessagew") == 0)
        resolution.targetAddress = reinterpret_cast<ULONGLONG>(&BridgePostMessageW);
    else if (_wcsicmp(symbol.name.c_str(), L"sendmessagew") == 0)
        resolution.targetAddress = reinterpret_cast<ULONGLONG>(&BridgeSendMessageW);
    else if (_wcsicmp(symbol.name.c_str(), L"postquitmessage") == 0)
        resolution.targetAddress = reinterpret_cast<ULONGLONG>(&BridgePostQuitMessage);
    else if (_wcsicmp(symbol.name.c_str(), L"getmessagew") == 0)
        resolution.targetAddress = reinterpret_cast<ULONGLONG>(&BridgeGetMessageW);
    else if (_wcsicmp(symbol.name.c_str(), L"peekmessagew") == 0)
        resolution.targetAddress = reinterpret_cast<ULONGLONG>(&BridgePeekMessageW);
    else if (_wcsicmp(symbol.name.c_str(), L"translatemessage") == 0)
        resolution.targetAddress = reinterpret_cast<ULONGLONG>(&BridgeTranslateMessage);
    else if (_wcsicmp(symbol.name.c_str(), L"dispatchmessagew") == 0)
        resolution.targetAddress = reinterpret_cast<ULONGLONG>(&BridgeDispatchMessageW);
    else if (_wcsicmp(symbol.name.c_str(), L"invalidaterect") == 0)
        resolution.targetAddress = reinterpret_cast<ULONGLONG>(&BridgeInvalidateRect);
    else if (_wcsicmp(symbol.name.c_str(), L"updatewindow") == 0)
        resolution.targetAddress = reinterpret_cast<ULONGLONG>(&BridgeUpdateWindow);
    else if (_wcsicmp(symbol.name.c_str(), L"beginpaint") == 0)
        resolution.targetAddress = reinterpret_cast<ULONGLONG>(&BridgeBeginPaint);
    else if (_wcsicmp(symbol.name.c_str(), L"endpaint") == 0)
        resolution.targetAddress = reinterpret_cast<ULONGLONG>(&BridgeEndPaint);
    if (_wcsicmp(symbol.name.c_str(), L"getdc") == 0)
        resolution.targetAddress = reinterpret_cast<ULONGLONG>(&BridgeGetDC);
    else if (_wcsicmp(symbol.name.c_str(), L"releasedc") == 0)
        resolution.targetAddress = reinterpret_cast<ULONGLONG>(&BridgeReleaseDC);
    else if (_wcsicmp(symbol.name.c_str(), L"fillrect") == 0)
        resolution.targetAddress = reinterpret_cast<ULONGLONG>(&BridgeFillRect);
    else if (_wcsicmp(symbol.name.c_str(), L"drawtextw") == 0)
        resolution.targetAddress = reinterpret_cast<ULONGLONG>(&BridgeDrawTextW);
    else if (_wcsicmp(symbol.name.c_str(), L"loadstringw") == 0)
        resolution.targetAddress = reinterpret_cast<ULONGLONG>(&BridgeLoadStringW);
    else if (_wcsicmp(symbol.name.c_str(), L"screentoclient") == 0)
        resolution.targetAddress = reinterpret_cast<ULONGLONG>(&BridgeScreenToClient);
    else if (_wcsicmp(symbol.name.c_str(), L"mapwindowpoints") == 0)
        resolution.targetAddress = reinterpret_cast<ULONGLONG>(&BridgeMapWindowPoints);
    else if (_wcsicmp(symbol.name.c_str(), L"movewindow") == 0)
        resolution.targetAddress = reinterpret_cast<ULONGLONG>(&BridgeMoveWindow);
    else if (_wcsicmp(symbol.name.c_str(), L"charupperw") == 0)
        resolution.targetAddress = reinterpret_cast<ULONGLONG>(&BridgeCharUpperW);
    else if (_wcsicmp(symbol.name.c_str(), L"charprevexa") == 0)
        resolution.targetAddress = reinterpret_cast<ULONGLONG>(&BridgeCharPrevExA);
    else if (_wcsicmp(symbol.name.c_str(), L"getkeystate") == 0)
        resolution.targetAddress = reinterpret_cast<ULONGLONG>(&BridgeGetKeyState);
    else if (_wcsicmp(symbol.name.c_str(), L"setdlgitemtexta") == 0)
        resolution.targetAddress = reinterpret_cast<ULONGLONG>(&BridgeSetDlgItemTextA);
    if (_wcsicmp(symbol.name.c_str(), L"mapdialogrect") == 0)
        resolution.targetAddress = reinterpret_cast<ULONGLONG>(&BridgeMapDialogRect);
    else if (_wcsicmp(symbol.name.c_str(), L"checkdlgbutton") == 0)
        resolution.targetAddress = reinterpret_cast<ULONGLONG>(&BridgeCheckDlgButton);
    else if (_wcsicmp(symbol.name.c_str(), L"isdlgbuttonchecked") == 0)
        resolution.targetAddress = reinterpret_cast<ULONGLONG>(&BridgeIsDlgButtonChecked);
    else if (_wcsicmp(symbol.name.c_str(), L"checkradiobutton") == 0)
        resolution.targetAddress = reinterpret_cast<ULONGLONG>(&BridgeCheckRadioButton);
    else if (_wcsicmp(symbol.name.c_str(), L"enddialog") == 0)
        resolution.targetAddress = reinterpret_cast<ULONGLONG>(&BridgeEndDialog);
    else if (_wcsicmp(symbol.name.c_str(), L"systemparametersinfow") == 0)
        resolution.targetAddress = reinterpret_cast<ULONGLONG>(&BridgeSystemParametersInfoW);
    else if (_wcsicmp(symbol.name.c_str(), L"monitorfromwindow") == 0)
        resolution.targetAddress = reinterpret_cast<ULONGLONG>(&BridgeMonitorFromWindow);
    else if (_wcsicmp(symbol.name.c_str(), L"getmonitorinfoa") == 0 || _wcsicmp(symbol.name.c_str(), L"getmonitorinfow") == 0)
        resolution.targetAddress = reinterpret_cast<ULONGLONG>(&BridgeGetMonitorInfoA);
    if (_wcsicmp(symbol.name.c_str(), L"openclipboard") == 0)
        resolution.targetAddress = reinterpret_cast<ULONGLONG>(&BridgeOpenClipboard);
    else if (_wcsicmp(symbol.name.c_str(), L"closeclipboard") == 0)
        resolution.targetAddress = reinterpret_cast<ULONGLONG>(&BridgeCloseClipboard);
    else if (_wcsicmp(symbol.name.c_str(), L"emptyclipboard") == 0)
        resolution.targetAddress = reinterpret_cast<ULONGLONG>(&BridgeEmptyClipboard);
    else if (_wcsicmp(symbol.name.c_str(), L"setclipboarddata") == 0)
        resolution.targetAddress = reinterpret_cast<ULONGLONG>(&BridgeSetClipboardData);
    else if (_wcsicmp(symbol.name.c_str(), L"registerclipboardformatw") == 0)
        resolution.targetAddress = reinterpret_cast<ULONGLONG>(&BridgeRegisterClipboardFormatW);
    else if (_wcsicmp(symbol.name.c_str(), L"createmenu") == 0)
        resolution.targetAddress = reinterpret_cast<ULONGLONG>(&BridgeCreateMenu);
    else if (_wcsicmp(symbol.name.c_str(), L"createpopupmenu") == 0)
        resolution.targetAddress = reinterpret_cast<ULONGLONG>(&BridgeCreatePopupMenu);
    else if (_wcsicmp(symbol.name.c_str(), L"destroymenu") == 0)
        resolution.targetAddress = reinterpret_cast<ULONGLONG>(&BridgeDestroyMenu);
    else if (_wcsicmp(symbol.name.c_str(), L"appendmenuw") == 0)
        resolution.targetAddress = reinterpret_cast<ULONGLONG>(&BridgeAppendMenuW);
    if (_wcsicmp(symbol.name.c_str(), L"insertmenuitemw") == 0)
        resolution.targetAddress = reinterpret_cast<ULONGLONG>(&BridgeInsertMenuItemW);
    else if (_wcsicmp(symbol.name.c_str(), L"getmenuitemcount") == 0)
        resolution.targetAddress = reinterpret_cast<ULONGLONG>(&BridgeGetMenuItemCount);
    else if (_wcsicmp(symbol.name.c_str(), L"getmenuiteminfow") == 0)
        resolution.targetAddress = reinterpret_cast<ULONGLONG>(&BridgeGetMenuItemInfoW);
    else if (_wcsicmp(symbol.name.c_str(), L"setmenuiteminfow") == 0)
        resolution.targetAddress = reinterpret_cast<ULONGLONG>(&BridgeSetMenuItemInfoW);
    else if (_wcsicmp(symbol.name.c_str(), L"enablemenuitem") == 0)
        resolution.targetAddress = reinterpret_cast<ULONGLONG>(&BridgeEnableMenuItem);
    else if (_wcsicmp(symbol.name.c_str(), L"checkmenuitem") == 0)
        resolution.targetAddress = reinterpret_cast<ULONGLONG>(&BridgeCheckMenuItem);
    else if (_wcsicmp(symbol.name.c_str(), L"checkmenuradioitem") == 0)
        resolution.targetAddress = reinterpret_cast<ULONGLONG>(&BridgeCheckMenuRadioItem);
    else if (_wcsicmp(symbol.name.c_str(), L"removemenu") == 0)
        resolution.targetAddress = reinterpret_cast<ULONGLONG>(&BridgeRemoveMenu);
    else if (_wcsicmp(symbol.name.c_str(), L"deletemenu") == 0)
        resolution.targetAddress = reinterpret_cast<ULONGLONG>(&BridgeDeleteMenu);
    else if (_wcsicmp(symbol.name.c_str(), L"getmenuitemid") == 0)
        resolution.targetAddress = reinterpret_cast<ULONGLONG>(&BridgeGetMenuItemID);
    else if (_wcsicmp(symbol.name.c_str(), L"getmenustate") == 0)
        resolution.targetAddress = reinterpret_cast<ULONGLONG>(&BridgeGetMenuState);
    else if (_wcsicmp(symbol.name.c_str(), L"getmenustringw") == 0)
        resolution.targetAddress = reinterpret_cast<ULONGLONG>(&BridgeGetMenuStringW);
    else if (_wcsicmp(symbol.name.c_str(), L"setmenudefaultitem") == 0)
        resolution.targetAddress = reinterpret_cast<ULONGLONG>(&BridgeSetMenuDefaultItem);
    else if (_wcsicmp(symbol.name.c_str(), L"getmenudefaultitem") == 0)
        resolution.targetAddress = reinterpret_cast<ULONGLONG>(&BridgeGetMenuDefaultItem);
    else if (_wcsicmp(symbol.name.c_str(), L"ismenu") == 0)
        resolution.targetAddress = reinterpret_cast<ULONGLONG>(&BridgeIsMenu);
    else if (_wcsicmp(symbol.name.c_str(), L"hilitemenuitem") == 0)
        resolution.targetAddress = reinterpret_cast<ULONGLONG>(&BridgeHiliteMenuItem);
    else if (_wcsicmp(symbol.name.c_str(), L"getsubmenu") == 0)
        resolution.targetAddress = reinterpret_cast<ULONGLONG>(&BridgeGetSubMenu);
    else if (_wcsicmp(symbol.name.c_str(), L"getmenu") == 0)
        resolution.targetAddress = reinterpret_cast<ULONGLONG>(&BridgeGetMenu);
    else if (_wcsicmp(symbol.name.c_str(), L"setmenu") == 0)
        resolution.targetAddress = reinterpret_cast<ULONGLONG>(&BridgeSetMenu);
    else if (_wcsicmp(symbol.name.c_str(), L"drawmenubar") == 0)
        resolution.targetAddress = reinterpret_cast<ULONGLONG>(&BridgeDrawMenuBar);
    else if (_wcsicmp(symbol.name.c_str(), L"loadmenuw") == 0)
        resolution.targetAddress = reinterpret_cast<ULONGLONG>(&BridgeLoadMenuW);
    else if (_wcsicmp(symbol.name.c_str(), L"trackpopupmenuex") == 0)
        resolution.targetAddress = reinterpret_cast<ULONGLONG>(&BridgeTrackPopupMenuEx);
    else if (_wcsicmp(symbol.name.c_str(), L"trackpopupmenu") == 0)
        resolution.targetAddress = reinterpret_cast<ULONGLONG>(&BridgeTrackPopupMenu);
    else if (_wcsicmp(symbol.name.c_str(), L"endmenu") == 0)
        resolution.targetAddress = reinterpret_cast<ULONGLONG>(&BridgeEndMenu);
    if (_wcsicmp(symbol.name.c_str(), L"loadacceleratorsw") == 0)
        resolution.targetAddress = reinterpret_cast<ULONGLONG>(&BridgeLoadAcceleratorsW);
    else if (_wcsicmp(symbol.name.c_str(), L"translateacceleratorw") == 0)
        resolution.targetAddress = reinterpret_cast<ULONGLONG>(&BridgeTranslateAcceleratorW);
    else if (_wcsicmp(symbol.name.c_str(), L"getdialogbaseunits") == 0)
        resolution.targetAddress = reinterpret_cast<ULONGLONG>(&BridgeGetDialogBaseUnits);
    else if (_wcsicmp(symbol.name.c_str(), L"childwindowfrompointex") == 0)
        resolution.targetAddress = reinterpret_cast<ULONGLONG>(&BridgeChildWindowFromPointEx);
    else if (_wcsicmp(symbol.name.c_str(), L"windowfrompoint") == 0)
        resolution.targetAddress = reinterpret_cast<ULONGLONG>(&BridgeWindowFromPoint);
    else if (_wcsicmp(symbol.name.c_str(), L"mapvirtualkeyw") == 0)
        resolution.targetAddress = reinterpret_cast<ULONGLONG>(&BridgeMapVirtualKeyW);
    else if (_wcsicmp(symbol.name.c_str(), L"clienttoscreen") == 0)
        resolution.targetAddress = reinterpret_cast<ULONGLONG>(&BridgeClientToScreen);
    else if (_wcsicmp(symbol.name.c_str(), L"getwindowplacement") == 0)
        resolution.targetAddress = reinterpret_cast<ULONGLONG>(&BridgeGetWindowPlacement);
    else if (_wcsicmp(symbol.name.c_str(), L"setwindowplacement") == 0)
        resolution.targetAddress = reinterpret_cast<ULONGLONG>(&BridgeSetWindowPlacement);
    else if (_wcsicmp(symbol.name.c_str(), L"loadbitmapw") == 0)
        resolution.targetAddress = reinterpret_cast<ULONGLONG>(&BridgeLoadBitmapW);
    else if (_wcsicmp(symbol.name.c_str(), L"getclassinfow") == 0)
        resolution.targetAddress = reinterpret_cast<ULONGLONG>(&BridgeGetClassInfoW);
    else if (_wcsicmp(symbol.name.c_str(), L"callwindowprocw") == 0)
        resolution.targetAddress = reinterpret_cast<ULONGLONG>(&BridgeCallWindowProcW);
    else if (_wcsicmp(symbol.name.c_str(), L"dialogboxparamw") == 0)
        resolution.targetAddress = reinterpret_cast<ULONGLONG>(&BridgeDialogBoxParamW);
    else if (_wcsicmp(symbol.name.c_str(), L"isdialogmessagew") == 0)
        resolution.targetAddress = reinterpret_cast<ULONGLONG>(&BridgeIsDialogMessageW);

    if (resolution.targetAddress != 0)
    {
        resolution.disposition = ImportDisposition::NeedsBridge;
        if (_wcsicmp(symbol.name.c_str(), L"settimer") == 0 ||
            _wcsicmp(symbol.name.c_str(), L"killtimer") == 0)
        {
            resolution.note = L"USER32 timer adapter: bounded UWP timers post coalesced WM_TIMER; TIMERPROC callbacks are not invoked.";
        }
        else
        {
            resolution.note = L"USER32 adapter: guest windows, queued messages, painting, and virtual device contexts.";
        }
    }
    return resolution;
}
