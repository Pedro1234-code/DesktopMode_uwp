#pragma once

#include "Bridge\\CompatibilityCatalog.h"
#include "Bridge\\GuestWin32Abi.h"
#include "Bridge\\MiniGdi.h"

#include <windows.h>

#include <string>
#include <vector>

namespace Win32Bridge
{
namespace Bridge
{
    // A read-only snapshot consumed by the guest compositor.  Menu handles
    // remain private to USER32; the window layer receives only presentation
    // metadata and never exposes a host menu object to the PE.
    struct GuestMenuVisualItem final
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

    std::vector<GuestMenuVisualItem> GetGuestMenuItems(HMENU menu);
    std::vector<GuestMenuVisualItem> GetGuestMenuBarItems(HWND window);
    bool CopyGuestIconPixels(HICON icon, MiniGdi::Surface* destination);
    HICON CreateGuestShellIcon(bool directory, int size);
    ATOM WINAPI BridgeRegisterClassExW(const GuestAbi::WndClassExW* windowClass);
    ATOM WINAPI BridgeRegisterClassW(const GuestAbi::WndClassW* windowClass);
    HWND WINAPI BridgeCreateWindowExW(
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
        LPVOID parameter);
    HCURSOR WINAPI BridgeLoadCursorW(HINSTANCE instance, LPCWSTR cursorName);
    HCURSOR WINAPI BridgeLoadCursorA(HINSTANCE instance, LPCSTR cursorName);
    HCURSOR WINAPI BridgeLoadCursorFromFileW(LPCWSTR fileName);
    HCURSOR WINAPI BridgeLoadCursorFromFileA(LPCSTR fileName);
    HCURSOR WINAPI BridgeSetCursor(HCURSOR cursor);
    HICON WINAPI BridgeLoadIconW(HINSTANCE instance, LPCWSTR iconName);
    HICON WINAPI BridgeLoadIconA(HINSTANCE instance, LPCSTR iconName);
    HANDLE WINAPI BridgeLoadImageW(HINSTANCE instance, LPCWSTR name, UINT type, int width, int height, UINT flags);
    HANDLE WINAPI BridgeLoadImageA(HINSTANCE instance, LPCSTR name, UINT type, int width, int height, UINT flags);
    HICON WINAPI BridgeCreateIconFromResourceEx(PBYTE bits, DWORD size, BOOL icon, DWORD version, int width, int height, UINT flags);
    HICON WINAPI BridgeCreateIconFromResource(PBYTE bits, DWORD size, BOOL icon, DWORD version);
    HANDLE WINAPI BridgeCopyImage(HANDLE image, UINT type, int width, int height, UINT flags);
    HICON WINAPI BridgeCopyIcon(HICON icon);
    BOOL WINAPI BridgeDrawIcon(HDC dc, int x, int y, HICON icon);
    BOOL WINAPI BridgeDrawIconEx(HDC dc, int x, int y, HICON icon, int width, int height, UINT step, HBRUSH brush, UINT flags);
    BOOL WINAPI BridgeDestroyCursor(HCURSOR cursor);
    BOOL WINAPI BridgeDestroyIcon(HICON icon);
    int WINAPI BridgeGetSystemMetrics(int index);
    COLORREF WINAPI BridgeGetSysColor(int color);
    HBRUSH WINAPI BridgeGetSysColorBrush(int color);
    BOOL WINAPI BridgeAdjustWindowRect(LPRECT rect, DWORD style, BOOL hasMenu);
    BOOL WINAPI BridgeAdjustWindowRectEx(LPRECT rect, DWORD style, BOOL hasMenu, DWORD extendedStyle);
    BOOL WINAPI BridgeDestroyWindow(HWND window);
    BOOL WINAPI BridgeShowWindow(HWND window, int command);
    BOOL WINAPI BridgeGetClientRect(HWND window, LPRECT rect);
    BOOL WINAPI BridgeGetWindowRect(HWND window, LPRECT rect);
    BOOL WINAPI BridgeSetWindowTextW(HWND window, LPCWSTR text);
    int WINAPI BridgeGetWindowTextW(HWND window, LPWSTR buffer, int count);
    int WINAPI BridgeGetWindowTextLengthW(HWND window);
    HWND WINAPI BridgeGetDlgItem(HWND parent, int identifier);
    int WINAPI BridgeGetDlgCtrlID(HWND window);
    BOOL WINAPI BridgeSetDlgItemTextW(HWND parent, int identifier, LPCWSTR text);
    UINT WINAPI BridgeGetDlgItemTextW(HWND parent, int identifier, LPWSTR buffer, int count);
    LRESULT WINAPI BridgeSendDlgItemMessageW(HWND parent, int identifier, UINT message, WPARAM wParam, LPARAM lParam);
    BOOL WINAPI BridgeIsWindow(HWND window);
    BOOL WINAPI BridgeIsWindowVisible(HWND window);
    BOOL WINAPI BridgeIsWindowEnabled(HWND window);
    BOOL WINAPI BridgeIsZoomed(HWND window);
    BOOL WINAPI BridgeEnableWindow(HWND window, BOOL enable);
    HWND WINAPI BridgeSetFocus(HWND window);
    HWND WINAPI BridgeGetFocus();
    HWND WINAPI BridgeSetCapture(HWND window);
    BOOL WINAPI BridgeReleaseCapture();
    HWND WINAPI BridgeGetCapture();
    HWND WINAPI BridgeGetParent(HWND window);
    LONG WINAPI BridgeGetWindowLongW(HWND window, int index);
    LONG WINAPI BridgeSetWindowLongW(HWND window, int index, LONG value);
    LONG_PTR WINAPI BridgeGetWindowLongPtrW(HWND window, int index);
    LONG_PTR WINAPI BridgeSetWindowLongPtrW(HWND window, int index, LONG_PTR value);
    BOOL WINAPI BridgeSetWindowPos(HWND window, HWND insertAfter, int x, int y, int width, int height, UINT flags);
    UINT_PTR WINAPI BridgeSetTimer(HWND window, UINT_PTR timerId, UINT elapseMilliseconds, GuestAbi::TimerProc timerProcedure);
    BOOL WINAPI BridgeKillTimer(HWND window, UINT_PTR timerId);
    LRESULT WINAPI BridgeDefWindowProcW(HWND window, UINT message, WPARAM wParam, LPARAM lParam);
    BOOL WINAPI BridgePostMessageW(HWND window, UINT message, WPARAM wParam, LPARAM lParam);
    LRESULT WINAPI BridgeSendMessageW(HWND window, UINT message, WPARAM wParam, LPARAM lParam);
    void WINAPI BridgePostQuitMessage(int exitCode);
    BOOL WINAPI BridgeGetMessageW(GuestAbi::Message* message, HWND window, UINT minimumMessage, UINT maximumMessage);
    BOOL WINAPI BridgePeekMessageW(GuestAbi::Message* message, HWND window, UINT minimumMessage, UINT maximumMessage, UINT removeMessage);
    BOOL WINAPI BridgeTranslateMessage(const GuestAbi::Message* message);
    LRESULT WINAPI BridgeDispatchMessageW(const GuestAbi::Message* message);
    BOOL WINAPI BridgeInvalidateRect(HWND window, const RECT* rect, BOOL erase);
    BOOL WINAPI BridgeUpdateWindow(HWND window);
    HDC WINAPI BridgeBeginPaint(HWND window, GuestAbi::PaintStruct* paint);
    BOOL WINAPI BridgeEndPaint(HWND window, const GuestAbi::PaintStruct* paint);
    HDC WINAPI BridgeGetDC(HWND window);
    int WINAPI BridgeReleaseDC(HWND window, HDC dc);
    int WINAPI BridgeFillRect(HDC dc, const RECT* rect, HBRUSH brush);
    int WINAPI BridgeDrawTextW(HDC dc, LPWSTR text, int characterCount, LPRECT rect, UINT format);
    int WINAPI BridgeLoadStringW(HINSTANCE instance, UINT identifier, LPWSTR buffer, int bufferCount);
    int WINAPI BridgeMapWindowPoints(HWND from, HWND to, LPPOINT points, UINT count);
    BOOL WINAPI BridgeScreenToClient(HWND window, LPPOINT point);
    BOOL WINAPI BridgeMoveWindow(HWND window, int x, int y, int width, int height, BOOL repaint);
    LPWSTR WINAPI BridgeCharUpperW(LPWSTR text);
    LPSTR WINAPI BridgeCharPrevExA(UINT codePage, LPCSTR textStart, LPCSTR current, DWORD flags);
    BOOL WINAPI BridgeSetDlgItemTextA(HWND parent, int identifier, LPCSTR text);
    BOOL WINAPI BridgeMapDialogRect(HWND dialog, LPRECT rect);
    BOOL WINAPI BridgeCheckDlgButton(HWND dialog, int identifier, UINT check);
    UINT WINAPI BridgeIsDlgButtonChecked(HWND dialog, int identifier);
    BOOL WINAPI BridgeCheckRadioButton(HWND dialog, int first, int last, int selected);
    BOOL WINAPI BridgeEndDialog(HWND dialog, INT_PTR result);
    BOOL WINAPI BridgeSystemParametersInfoW(UINT action, UINT parameter, PVOID value, UINT flags);
    HMONITOR WINAPI BridgeMonitorFromWindow(HWND window, DWORD flags);
    BOOL WINAPI BridgeGetMonitorInfoA(HMONITOR monitor, PVOID monitorInfo);
    BOOL WINAPI BridgeOpenClipboard(HWND owner);
    BOOL WINAPI BridgeCloseClipboard();
    BOOL WINAPI BridgeEmptyClipboard();
    HANDLE WINAPI BridgeSetClipboardData(UINT format, HANDLE memory);
    INT_PTR WINAPI BridgeDialogBoxParamW(HINSTANCE instance, LPCWSTR templateName, HWND parent, DLGPROC dialogProcedure, LPARAM initParameter);
    BOOL WINAPI BridgeIsDialogMessageW(HWND dialog, const GuestAbi::Message* message);
    UINT WINAPI BridgeRegisterClipboardFormatW(LPCWSTR formatName);
    HMENU WINAPI BridgeCreateMenu();
    HMENU WINAPI BridgeCreatePopupMenu();
    BOOL WINAPI BridgeDestroyMenu(HMENU menu);
    BOOL WINAPI BridgeAppendMenuW(HMENU menu, UINT flags, UINT_PTR identifier, LPCWSTR text);
    BOOL WINAPI BridgeInsertMenuItemW(HMENU menu, UINT item, BOOL byPosition, const void* itemInfo);
    int WINAPI BridgeGetMenuItemCount(HMENU menu);
    BOOL WINAPI BridgeGetMenuItemInfoW(HMENU menu, UINT item, BOOL byPosition, void* itemInfo);
    BOOL WINAPI BridgeSetMenuItemInfoW(HMENU menu, UINT item, BOOL byPosition, const void* itemInfo);
    UINT WINAPI BridgeEnableMenuItem(HMENU menu, UINT item, UINT flags);
    DWORD WINAPI BridgeCheckMenuItem(HMENU menu, UINT item, UINT flags);
    BOOL WINAPI BridgeCheckMenuRadioItem(HMENU menu, UINT first, UINT last, UINT selected, UINT flags);
    BOOL WINAPI BridgeRemoveMenu(HMENU menu, UINT item, UINT flags);
    BOOL WINAPI BridgeDeleteMenu(HMENU menu, UINT item, UINT flags);
    UINT WINAPI BridgeGetMenuItemID(HMENU menu, int position);
    UINT WINAPI BridgeGetMenuState(HMENU menu, UINT item, UINT flags);
    int WINAPI BridgeGetMenuStringW(HMENU menu, UINT item, LPWSTR text, int count, UINT flags);
    BOOL WINAPI BridgeSetMenuDefaultItem(HMENU menu, UINT item, UINT byPosition);
    UINT WINAPI BridgeGetMenuDefaultItem(HMENU menu, UINT byPosition, UINT flags);
    BOOL WINAPI BridgeIsMenu(HMENU menu);
    BOOL WINAPI BridgeHiliteMenuItem(HWND window, HMENU menu, UINT item, UINT flags);
    HMENU WINAPI BridgeGetSubMenu(HMENU menu, int position);
    HMENU WINAPI BridgeGetMenu(HWND window);
    BOOL WINAPI BridgeSetMenu(HWND window, HMENU menu);
    BOOL WINAPI BridgeDrawMenuBar(HWND window);
    HMENU WINAPI BridgeLoadMenuW(HINSTANCE instance, LPCWSTR resource);
    UINT WINAPI BridgeTrackPopupMenuEx(HMENU menu, UINT flags, int x, int y, HWND owner, const RECT* excludeRect);
    BOOL WINAPI BridgeTrackPopupMenu(HMENU menu, UINT flags, int x, int y, int reserved, HWND owner, const RECT* rect);
    BOOL WINAPI BridgeEndMenu();
    HANDLE WINAPI BridgeLoadAcceleratorsW(HINSTANCE instance, LPCWSTR resource);
    int WINAPI BridgeTranslateAcceleratorW(HWND window, HANDLE accelerators, const GuestAbi::Message* message);
    UINT WINAPI BridgeGetDialogBaseUnits();
    HWND WINAPI BridgeChildWindowFromPointEx(HWND parent, POINT point, UINT flags);
    HWND WINAPI BridgeWindowFromPoint(POINT point);
    UINT WINAPI BridgeMapVirtualKeyW(UINT code, UINT mapType);
    BOOL WINAPI BridgeClientToScreen(HWND window, LPPOINT point);
    BOOL WINAPI BridgeGetWindowPlacement(HWND window, void* placement);
    BOOL WINAPI BridgeSetWindowPlacement(HWND window, const void* placement);
    HBITMAP WINAPI BridgeLoadBitmapW(HINSTANCE instance, LPCWSTR bitmapName);
    HBITMAP WINAPI BridgeLoadBitmapA(HINSTANCE instance, LPCSTR bitmapName);
    BOOL WINAPI BridgeGetClassInfoW(HINSTANCE instance, LPCWSTR className, GuestAbi::WndClassW* windowClass);
    LRESULT WINAPI BridgeCallWindowProcW(GuestAbi::WndProc procedure, HWND window, UINT message, WPARAM wParam, LPARAM lParam);

    ImportResolution ResolveUser32Import(const ImportedSymbol& symbol);
}
}
