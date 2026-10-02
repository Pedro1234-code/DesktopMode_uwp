#pragma once

#include "Bridge/MiniGdi.h"

#include <atomic>

namespace Win32Bridge
{
namespace Bridge
{
namespace GuestMetrics
{
    // Metrics for the bridge-owned virtual desktop at its current logical
    // 96-DPI baseline. Controls derive their extents from these primitives
    // instead of carrying unrelated pixel literals. A future DPI adapter only
    // needs to scale this single layer.
    constexpr int LogicalDpi = 96;
    constexpr int VirtualScreenWidth = 800;
    constexpr int VirtualScreenHeight = 480;

    inline std::atomic<int>& CurrentScreenWidthStorage()
    {
        static std::atomic<int> value{ VirtualScreenWidth };
        return value;
    }

    inline std::atomic<int>& CurrentScreenHeightStorage()
    {
        static std::atomic<int> value{ VirtualScreenHeight };
        return value;
    }

    inline int CurrentScreenWidth()
    {
        return CurrentScreenWidthStorage().load();
    }

    inline int CurrentScreenHeight()
    {
        return CurrentScreenHeightStorage().load();
    }

    inline void SetCurrentScreenSize(int width, int height)
    {
        if (width > 0) CurrentScreenWidthStorage().store(width);
        if (height > 0) CurrentScreenHeightStorage().store(height);
    }

    constexpr int Border = 1;
    constexpr int FixedFrame = 4;
    constexpr int ScrollBarExtent = 17;
    constexpr int CaptionHeight = 23;
    constexpr int IconExtent = 32;
    constexpr int DoubleClickExtent = 4;

    constexpr int TextHeight = MiniGdi::DefaultTextGlyphHeight;
    constexpr int TextWidth = MiniGdi::DefaultTextGlyphWidth;
    constexpr int DialogExternalLeading = 3;
    constexpr int ControlVerticalPadding = 2;
    constexpr int ControlHorizontalPadding = 4;

    constexpr int MenuHeight = TextHeight + 2 * ControlVerticalPadding + 2 * Border;
    constexpr int MenuTextTop = (MenuHeight - TextHeight) / 2;
    constexpr int ComboFieldHeight = MenuHeight;
    constexpr int StatusBarMinimumHeight = TextHeight + 2 * Border;
    constexpr int StatusBarHeight = TextHeight + 2 * ControlVerticalPadding + 2 * Border;
    constexpr int DefaultBitmapExtent = 16;
    constexpr int ToolbarButtonExtent = DefaultBitmapExtent + 2 * ControlHorizontalPadding;
    constexpr int ToolbarHeight = ToolbarButtonExtent + 2 * Border;
    constexpr int ListViewHeaderHeight = TextHeight + 2 * ControlVerticalPadding + 2 * Border;
    constexpr int TabItemHeight = ToolbarButtonExtent;
    constexpr int MinimumColumnWidth = 3 * TextWidth;
    constexpr int MenuCheckColumnWidth = 3 * TextWidth;
    constexpr int MaximumControlStripHeight = LogicalDpi;

    constexpr DWORD WindowStyleChild = 0x40000000u;
    constexpr DWORD WindowStyleCaption = 0x00c00000u;
    constexpr DWORD WindowStyleBorder = 0x00800000u;
    constexpr DWORD WindowStyleDialogFrame = 0x00400000u;
    constexpr DWORD WindowStyleThickFrame = 0x00040000u;
    constexpr DWORD WindowStyleSystemMenu = 0x00080000u;
    constexpr DWORD WindowExtendedStyleModalFrame = 0x00000001u;

    struct NonClientMetrics final
    {
        int left = 0;
        int top = 0;
        int right = 0;
        int bottom = 0;
        bool caption = false;
        bool closeButton = false;
    };

    // Owned popups live inside the bridge's single host surface but retain
    // ordinary Win32 non-client geometry. Plain WS_CHILD controls do not;
    // framed/captioned child windows (such as MDI children) still do.
    inline NonClientMetrics NonClientForEmbeddedWindow(
        DWORD style, DWORD extendedStyle, bool hasParent)
    {
        NonClientMetrics result;
        result.caption = (style & WindowStyleCaption) == WindowStyleCaption;
        const bool framedChild = (style & WindowStyleChild) != 0 &&
            (result.caption || (style & (WindowStyleDialogFrame |
                WindowStyleThickFrame)) != 0 ||
                (extendedStyle & WindowExtendedStyleModalFrame) != 0);
        if (!hasParent || ((style & WindowStyleChild) != 0 && !framedChild))
        {
            result.caption = false;
            return result;
        }

        result.closeButton = result.caption &&
            (style & WindowStyleSystemMenu) != 0;
        int frame = 0;
        if ((style & WindowStyleThickFrame) != 0)
            frame = FixedFrame;
        else if ((style & WindowStyleDialogFrame) != 0 ||
            (extendedStyle & WindowExtendedStyleModalFrame) != 0 || result.caption)
            frame = FixedFrame;
        else if ((style & WindowStyleBorder) != 0)
            frame = Border;
        result.left = result.right = result.bottom = frame;
        result.top = frame + (result.caption ? CaptionHeight : 0);
        return result;
    }

    constexpr int ControlHeightForText(int textHeight)
    {
        const int effective = textHeight > TextHeight ? textHeight : TextHeight;
        return effective + 2 * ControlVerticalPadding + 2 * Border;
    }

    inline bool TryGetSystemMetric(int index, int* value)
    {
        if (!value) return false;
        switch (index)
        {
        case 0:  // SM_CXSCREEN
        case 16: // SM_CXFULLSCREEN
        case 78: // SM_CXVIRTUALSCREEN
            *value = CurrentScreenWidth(); return true;
        case 1:  // SM_CYSCREEN
        case 17: // SM_CYFULLSCREEN
        case 79: // SM_CYVIRTUALSCREEN
            *value = CurrentScreenHeight(); return true;
        case 2:  // SM_CXVSCROLL
        case 3:  // SM_CYHSCROLL
        case 20: // SM_CYVSCROLL
        case 21: // SM_CXHSCROLL
            *value = ScrollBarExtent; return true;
        case 4: // SM_CYCAPTION
            *value = CaptionHeight; return true;
        case 5: // SM_CXBORDER
        case 6: // SM_CYBORDER
            *value = Border; return true;
        case 7:  // SM_CXDLGFRAME / SM_CXFIXEDFRAME
        case 8:  // SM_CYDLGFRAME / SM_CYFIXEDFRAME
        case 32: // SM_CXFRAME / SM_CXSIZEFRAME
        case 33: // SM_CYFRAME / SM_CYSIZEFRAME
            *value = FixedFrame; return true;
        case 11: // SM_CXICON
        case 12: // SM_CYICON
        case 13: // SM_CXCURSOR
        case 14: // SM_CYCURSOR
            *value = IconExtent; return true;
        case 15: // SM_CYMENU
            *value = MenuHeight; return true;
        case 19: // SM_MOUSEPRESENT
        case 80: // SM_CMONITORS
            *value = 1; return true;
        case 36: // SM_CXDOUBLECLK
        case 37: // SM_CYDOUBLECLK
            *value = DoubleClickExtent; return true;
        case 43: // SM_CMOUSEBUTTONS
            *value = 5; return true;
        default:
            return false;
        }
    }
}
}
}
