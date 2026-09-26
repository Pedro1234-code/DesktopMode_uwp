#pragma once

// The UWP SDK intentionally omits desktop-only USER32/GDI declarations such
// as WNDCLASSW and PAINTSTRUCT.  A guest PE still passes those well-known ABI
// layouts to its imports, so the bridge models the wire format locally rather
// than depending on desktop headers or host HWNDs.

#include <windows.h>

namespace Win32Bridge
{
namespace Bridge
{
namespace GuestAbi
{
    using WndProc = LRESULT(WINAPI*)(HWND, UINT, WPARAM, LPARAM);
    // USER32 normally allows a TIMERPROC to be supplied to SetTimer.  The
    // bridge keeps the ABI type so an x64 guest can import the normal entry
    // point, but it deliberately routes expiry through the guest message queue
    // instead of calling arbitrary guest code from a host thread-pool callback.
    using TimerProc = void(CALLBACK*)(HWND, UINT, UINT_PTR, DWORD);

    struct WndClassW
    {
        UINT style;
        WndProc lpfnWndProc;
        int cbClsExtra;
        int cbWndExtra;
        HINSTANCE hInstance;
        HICON hIcon;
        HCURSOR hCursor;
        HBRUSH hbrBackground;
        LPCWSTR lpszMenuName;
        LPCWSTR lpszClassName;
    };

    struct WndClassExW
    {
        UINT cbSize;
        UINT style;
        WndProc lpfnWndProc;
        int cbClsExtra;
        int cbWndExtra;
        HINSTANCE hInstance;
        HICON hIcon;
        HCURSOR hCursor;
        HBRUSH hbrBackground;
        LPCWSTR lpszMenuName;
        LPCWSTR lpszClassName;
        HICON hIconSm;
    };

    struct CreateStructW
    {
        LPVOID lpCreateParams;
        HINSTANCE hInstance;
        HMENU hMenu;
        HWND hwndParent;
        int cy;
        int cx;
        int y;
        int x;
        LONG style;
        LPCWSTR lpszName;
        LPCWSTR lpszClass;
        DWORD dwExStyle;
    };

    struct PaintStruct
    {
        HDC hdc;
        BOOL fErase;
        RECT rcPaint;
        BOOL fRestore;
        BOOL fIncUpdate;
        BYTE rgbReserved[32];
    };

    // Layout-compatible guest LOGFONTW.  UWP headers intentionally do not
    // make desktop GDI declarations a bridge dependency, while guest PEs may
    // still pass this wire-format structure to CreateFontIndirectW.
    constexpr int LfFaceSize = 32;
    struct LogFontW
    {
        LONG lfHeight;
        LONG lfWidth;
        LONG lfEscapement;
        LONG lfOrientation;
        LONG lfWeight;
        BYTE lfItalic;
        BYTE lfUnderline;
        BYTE lfStrikeOut;
        BYTE lfCharSet;
        BYTE lfOutPrecision;
        BYTE lfClipPrecision;
        BYTE lfQuality;
        BYTE lfPitchAndFamily;
        WCHAR lfFaceName[LfFaceSize];
    };

    // TEXTMETRICW is another desktop GDI wire structure that the UWP SDK
    // does not consistently expose.  The bridge reports fixed-raster layout
    // values together with the selected HFONT's retained style attributes.
    struct TextMetricW
    {
        LONG tmHeight;
        LONG tmAscent;
        LONG tmDescent;
        LONG tmInternalLeading;
        LONG tmExternalLeading;
        LONG tmAveCharWidth;
        LONG tmMaxCharWidth;
        LONG tmWeight;
        LONG tmOverhang;
        LONG tmDigitizedAspectX;
        LONG tmDigitizedAspectY;
        WCHAR tmFirstChar;
        WCHAR tmLastChar;
        WCHAR tmDefaultChar;
        WCHAR tmBreakChar;
        BYTE tmItalic;
        BYTE tmUnderlined;
        BYTE tmStruckOut;
        BYTE tmPitchAndFamily;
        BYTE tmCharSet;
    };

    struct Message
    {
        HWND hwnd;
        UINT message;
        WPARAM wParam;
        LPARAM lParam;
        DWORD time;
        POINT pt;
    };

#if defined(_WIN64)
    static_assert(sizeof(WndClassW) == 72, "Guest WNDCLASSW ABI must stay x64-compatible.");
    static_assert(sizeof(WndClassExW) == 80, "Guest WNDCLASSEXW ABI must stay x64-compatible.");
    static_assert(sizeof(CreateStructW) == 80, "Guest CREATESTRUCTW ABI must stay x64-compatible.");
    static_assert(sizeof(PaintStruct) == 72, "Guest PAINTSTRUCT ABI must stay x64-compatible.");
    static_assert(sizeof(Message) == 48, "Guest MSG ABI must stay x64-compatible.");
    static_assert(sizeof(LogFontW) == 92, "Guest LOGFONTW ABI must stay x64-compatible.");
    static_assert(sizeof(TextMetricW) == 60, "Guest TEXTMETRICW ABI must stay x64-compatible.");
#endif

    constexpr int CwUseDefault = static_cast<int>(0x80000000u);
    constexpr int SwHide = 0;
    constexpr int SwShow = 5;
    constexpr UINT SizeRestored = 0;
    constexpr LRESULT HtClient = 1;

    constexpr UINT WmCreate = 0x0001;
    constexpr UINT WmDestroy = 0x0002;
    constexpr UINT WmSize = 0x0005;
    constexpr UINT WmSetFocus = 0x0007;
    constexpr UINT WmKillFocus = 0x0008;
    constexpr UINT WmEnable = 0x000A;
    constexpr UINT WmSetText = 0x000C;
    constexpr UINT WmGetText = 0x000D;
    constexpr UINT WmGetTextLength = 0x000E;
    constexpr UINT WmPaint = 0x000F;
    constexpr UINT WmClose = 0x0010;
    constexpr UINT WmQuit = 0x0012;
    constexpr UINT WmEraseBkgnd = 0x0014;
    constexpr UINT WmShowWindow = 0x0018;
    constexpr UINT WmSetFont = 0x0030;
    constexpr UINT WmGetFont = 0x0031;
    constexpr UINT WmNotify = 0x004e;
    constexpr UINT WmContextMenu = 0x007b;
    constexpr UINT WmNcCreate = 0x0081;
    constexpr UINT WmNcDestroy = 0x0082;
    constexpr UINT WmNcHitTest = 0x0084;
    constexpr UINT WmKeyDown = 0x0100;
    constexpr UINT WmKeyUp = 0x0101;
    constexpr UINT WmChar = 0x0102;
    constexpr UINT WmCommand = 0x0111;
    constexpr UINT WmTimer = 0x0113;
    constexpr UINT WmInitMenu = 0x0116;
    constexpr UINT WmInitMenuPopup = 0x0117;
    constexpr UINT WmMouseMove = 0x0200;
    constexpr UINT WmLButtonDown = 0x0201;
    constexpr UINT WmLButtonUp = 0x0202;
    constexpr UINT WmRButtonDown = 0x0204;
    constexpr UINT WmRButtonUp = 0x0205;
    constexpr UINT WmMButtonDown = 0x0207;
    constexpr UINT WmMButtonUp = 0x0208;
    constexpr UINT WmMouseWheel = 0x020A;
    constexpr UINT WmXButtonDown = 0x020B;
    constexpr UINT WmXButtonUp = 0x020C;
    constexpr UINT WmCaptureChanged = 0x0215;

    constexpr UINT PeekNoRemove = 0x0000;
    constexpr UINT PeekRemove = 0x0001;
    constexpr UINT PeekNoYield = 0x0002;

    // Mirrors USER_TIMER_MINIMUM / USER_TIMER_MAXIMUM.  The bridge clamps a
    // requested period to this range before giving it to a UWP ThreadPoolTimer.
    constexpr UINT UserTimerMinimum = 10;
    constexpr UINT UserTimerMaximum = 0x7fffffff;

    // Window fields use the guest Win32 ABI offsets, not host USER32
    // declarations.  Keeping these local lets x64 PEs use the usual window
    // state APIs without exposing any host HWND state.
    constexpr int GwlpWndProc = -4;
    constexpr int GwlpHInstance = -6;
    constexpr int GwlpHwndParent = -8;
    constexpr int GwlpId = -12;
    constexpr int GwlStyle = -16;
    constexpr int GwlExStyle = -20;
    constexpr int GwlpUserData = -21;

    constexpr WPARAM MkLButton = 0x0001;
    constexpr WPARAM MkRButton = 0x0002;
    constexpr WPARAM MkShift = 0x0004;
    constexpr WPARAM MkControl = 0x0008;
    constexpr WPARAM MkMButton = 0x0010;
    constexpr WPARAM MkXButton1 = 0x0020;
    constexpr WPARAM MkXButton2 = 0x0040;
    constexpr WORD XButton1 = 0x0001;
    constexpr WORD XButton2 = 0x0002;

    // The stock control styles are kept in the guest ABI instead of taking a
    // dependency on desktop-only USER32 headers.  The first control slice
    // intentionally supports only the common text/button variants below;
    // later controls can add their own style bits alongside these values.
    constexpr DWORD WsChild = 0x40000000u;
    constexpr DWORD BsTypeMask = 0x0000000fu;
    constexpr DWORD BsPushButton = 0x00000000u;
    constexpr DWORD BsDefPushButton = 0x00000001u;
    constexpr DWORD SsLeft = 0x00000000u;
    constexpr DWORD SsCenter = 0x00000001u;
    constexpr DWORD SsRight = 0x00000002u;
    constexpr DWORD EsLeft = 0x00000000u;
    constexpr DWORD EsCenter = 0x00000001u;
    constexpr DWORD EsRight = 0x00000002u;
    constexpr DWORD EsReadOnly = 0x00000800u;

    constexpr WORD BnClicked = 0;
    constexpr WORD EnChange = 0x0300;

    constexpr WPARAM VkBack = 0x08;
    constexpr WPARAM VkReturn = 0x0d;
    constexpr WPARAM VkSpace = 0x20;
    constexpr WPARAM VkEnd = 0x23;
    constexpr WPARAM VkHome = 0x24;
    constexpr WPARAM VkLeft = 0x25;
    constexpr WPARAM VkUp = 0x26;
    constexpr WPARAM VkRight = 0x27;
    constexpr WPARAM VkDown = 0x28;
    constexpr WPARAM VkDelete = 0x2e;

    constexpr UINT DrawTextCenter = 0x00000001;
    constexpr UINT DrawTextRight = 0x00000002;
    constexpr UINT DrawTextVCenter = 0x00000004;
    constexpr UINT DrawTextBottom = 0x00000008;
    constexpr UINT DrawTextCalcRect = 0x00000400;

    constexpr int BrushWhite = 0;
    constexpr int BrushLtGray = 1;
    constexpr int BrushGray = 2;
    constexpr int BrushDkGray = 3;
    constexpr int BrushBlack = 4;
    constexpr int BrushNull = 5;
    constexpr int PenWhite = 6;
    constexpr int PenBlack = 7;
    constexpr int PenNull = 8;
    constexpr int FontOemFixed = 10;
    constexpr int FontAnsiFixed = 11;
    constexpr int FontAnsiVariable = 12;
    constexpr int FontSystem = 13;
    constexpr int FontDeviceDefault = 14;
    constexpr int FontSystemFixed = 16;
    constexpr int FontDefaultGui = 17;

    constexpr int PenSolid = 0;
    constexpr int PenNullStyle = 5;
    constexpr int PenInsideFrame = 6;
    constexpr int PenStyleMask = 0x0000000f;
    constexpr int BackgroundTransparent = 1;
    constexpr int BackgroundOpaque = 2;
    constexpr DWORD RasterOpSrcCopy = 0x00CC0020;
    constexpr COLORREF ColorInvalid = static_cast<COLORREF>(0xffffffffu);

    inline LPARAM MakeMouseLParam(WORD x, WORD y)
    {
        return static_cast<LPARAM>(static_cast<DWORD>(x) |
            (static_cast<DWORD>(y) << 16));
    }

    inline WPARAM MakeCommandWParam(WORD identifier, WORD notification)
    {
        return static_cast<WPARAM>(identifier) |
            (static_cast<WPARAM>(notification) << 16);
    }
}
}
}
