#pragma once

#include "Bridge\\GuestMessageQueue.h"
#include "Bridge\\MiniGdi.h"
#include "Bridge\\GuestWin32Abi.h"

#include <agile.h>
#include <windows.h>
#include <windows.foundation.h>
#include <windows.ui.core.h>
#include <windows.ui.input.h>
#include <windows.ui.xaml.controls.h>
#include <windows.ui.xaml.media.imaging.h>

#include <atomic>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>

namespace Win32Bridge
{
namespace Bridge
{
    struct GuestPresentationState;
    struct GuestInputCallbackState;
    struct GuestTimerState;

    // Per-guest USER32 environment. Windows and classes are bridge-owned
    // handles; a guest never obtains a host HWND or desktop window.
    class GuestWindowManager final
    {
    public:
        GuestWindowManager(
            Windows::UI::Core::CoreWindow^ coreWindow,
            Windows::UI::Xaml::Controls::Panel^ surfaceHost);
        ~GuestWindowManager();

        GuestWindowManager(const GuestWindowManager&) = delete;
        GuestWindowManager& operator=(const GuestWindowManager&) = delete;

        void Activate();
        void Deactivate();
        void SetInputEnabled(bool enabled) { m_inputEnabled.store(enabled); }

        ATOM RegisterGuestClass(const GuestAbi::WndClassExW* windowClass, DWORD* win32Error);
        ATOM RegisterGuestClass(const GuestAbi::WndClassW* windowClass, DWORD* win32Error);
        HWND CreateGuestWindow(
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
            DWORD* win32Error);
        BOOL DestroyGuestWindow(HWND window, DWORD* win32Error);
        BOOL ShowGuestWindow(HWND window, int command, DWORD* win32Error);
        BOOL SetGuestWindowMenuBar(HWND window, BOOL visible, DWORD* win32Error);
        UINT TrackGuestPopupMenu(HMENU menu, UINT flags, int x, int y, HWND owner, DWORD* win32Error);
        BOOL GetGuestClientRect(HWND window, LPRECT rect, DWORD* win32Error) const;
        BOOL GetGuestWindowRect(HWND window, LPRECT rect, DWORD* win32Error) const;
        BOOL SetGuestWindowText(HWND window, LPCWSTR text, DWORD* win32Error);
        int GetGuestWindowText(HWND window, LPWSTR buffer, int count, DWORD* win32Error) const;
        int GetGuestWindowTextLength(HWND window, DWORD* win32Error) const;
        BOOL IsGuestWindow(HWND window) const;
        BOOL IsGuestWindowVisible(HWND window, DWORD* win32Error) const;
        BOOL IsGuestWindowEnabled(HWND window, DWORD* win32Error) const;
        BOOL EnableGuestWindow(HWND window, BOOL enable, DWORD* win32Error);
        HWND SetGuestFocus(HWND window, DWORD* win32Error);
        HWND GetGuestFocus(DWORD* win32Error);
        HWND SetGuestCapture(HWND window, DWORD* win32Error);
        BOOL ReleaseGuestCapture(DWORD* win32Error);
        HWND GetGuestCapture(DWORD* win32Error);
        HWND GetGuestParent(HWND window, DWORD* win32Error) const;
        HWND GetGuestDlgItem(HWND parent, int identifier, DWORD* win32Error) const;
        LONG GetGuestWindowLong(HWND window, int index, DWORD* win32Error) const;
        LONG SetGuestWindowLong(HWND window, int index, LONG value, DWORD* win32Error);
        LONG_PTR GetGuestWindowLongPtr(HWND window, int index, DWORD* win32Error) const;
        LONG_PTR SetGuestWindowLongPtr(HWND window, int index, LONG_PTR value, DWORD* win32Error);
        BOOL SetGuestWindowPos(
            HWND window,
            HWND insertAfter,
            int x,
            int y,
            int width,
            int height,
            UINT flags,
            DWORD* win32Error);

        // TIMERPROC is accepted for normal SetTimer ABI compatibility but is
        // intentionally never invoked on a host callback thread. Expiry posts
        // one coalesced WM_TIMER (lParam == 0) through the guest queue instead.
        UINT_PTR SetGuestTimer(
            HWND window,
            UINT_PTR timerId,
            UINT elapseMilliseconds,
            GuestAbi::TimerProc timerProcedure,
            DWORD* win32Error);
        BOOL KillGuestTimer(HWND window, UINT_PTR timerId, DWORD* win32Error);

        LRESULT DefaultGuestWindowProcedure(HWND window, UINT message, WPARAM wParam, LPARAM lParam);
        BOOL PostGuestMessage(HWND window, UINT message, WPARAM wParam, LPARAM lParam, DWORD* win32Error);
        LRESULT SendGuestMessage(HWND window, UINT message, WPARAM wParam, LPARAM lParam, DWORD* win32Error);
        void PostGuestQuitMessage(int exitCode);
        GuestGetMessageResult GetGuestMessage(GuestAbi::Message* message, HWND filter, UINT minimum, UINT maximum);
        BOOL PeekGuestMessage(GuestAbi::Message* message, HWND filter, UINT minimum, UINT maximum, UINT removeFlags);
        BOOL TranslateGuestMessage(const GuestAbi::Message* message);
        LRESULT DispatchGuestMessage(const GuestAbi::Message* message, DWORD* win32Error);

        BOOL InvalidateGuestRect(HWND window, const RECT* rect, BOOL erase, DWORD* win32Error);
        BOOL UpdateGuestWindow(HWND window, DWORD* win32Error);
        HDC BeginGuestPaint(HWND window, GuestAbi::PaintStruct* paint, DWORD* win32Error);
        BOOL EndGuestPaint(HWND window, const GuestAbi::PaintStruct* paint, DWORD* win32Error);
        HDC GetGuestDC(HWND window, DWORD* win32Error);
        int ReleaseGuestDC(HWND window, HDC dc, DWORD* win32Error);

        // The virtual HDC values belong to MiniGdi.  These helpers intentionally
        // expose only opaque integer handles, never host UI resources.
        bool IsGuestDc(HDC dc) const;
        MiniGdi::DcHandle GuestDcHandle(HDC dc) const;

        MiniGdi::GdiContext& Gdi() { return m_gdi; }
        const MiniGdi::GdiContext& Gdi() const { return m_gdi; }

    private:
        friend struct GuestTimerState;

        struct WindowClass;
        struct WindowRecord;
        struct PopupMenuLevel final
        {
            HMENU menu = nullptr;
            int left = 0;
            int top = 0;
            int parentItem = -1;
            int hotItem = -1;
        };

        struct PopupMenuSession final
        {
            HMENU menu = nullptr;
            HWND owner = nullptr;
            HWND root = nullptr;
            int left = 0;
            int top = 0;
            bool open = false;
            bool menuBar = false;
            bool returnCommand = false;
            bool notifyOwner = true;
            bool allowRightButton = false;
            UINT selectedCommand = 0;
            int topMenuIndex = -1;
            std::vector<PopupMenuLevel> levels;
        };

        std::shared_ptr<WindowRecord> FindWindow(HWND window) const;
        std::shared_ptr<WindowClass> FindClass(LPCWSTR className) const;
        bool IsGuestWindowVisibleInternal(HWND window) const;
        bool IsGuestWindowEnabledInternal(HWND window) const;
        bool IsGuestWindowDescendantOrSelf(HWND candidate, HWND ancestor) const;
        bool WouldCreateParentCycle(HWND window, HWND proposedParent) const;
        void CancelGuestTimersForWindow(HWND window);
        void StopGuestTimers();
        bool PostGuestTimerMessage(HWND window, UINT_PTR timerId);
        void ClearGuestForegroundForSubtree(HWND window);
        void ClearGuestFocusForSubtree(HWND window);
        void ClearGuestCapture(HWND window, HWND replacement = nullptr);
        void ClearGuestCaptureForSubtree(HWND window, HWND replacement = nullptr);
        bool GetGuestSurfaceGeometry(
            HWND window,
            HWND* rootWindow,
            int* rootWidth,
            int* rootHeight,
            int* targetWidth,
            int* targetHeight,
            int* targetLeft,
            int* targetTop) const;
        HWND HitTestGuestWindow(HWND rootWindow, int rootX, int rootY) const;
        bool HandleGuestMenuPointer(HWND rootWindow, int rootX, int rootY, UINT message);
        LRESULT CallWindowProcedure(const std::shared_ptr<WindowRecord>& window, UINT message, WPARAM wParam, LPARAM lParam);
        LRESULT BuiltinControlProcedure(const std::shared_ptr<WindowRecord>& window, UINT message, WPARAM wParam, LPARAM lParam);
        bool EraseGuestBackground(const std::shared_ptr<WindowRecord>& window, HDC dc, const RECT& rect);
        void PostPaint(const std::shared_ptr<WindowRecord>& window, const RECT* rect = nullptr, BOOL erase = TRUE);
        void Present(const std::shared_ptr<WindowRecord>& window);
        // Leaf SEH gateways for CoreWindow delegates. The UI dispatcher must
        // never receive a guest/bridge structured exception directly.
        static DWORD InvokePointerInput(GuestWindowManager* manager, Windows::UI::Core::PointerEventArgs^ args, UINT message);
        static DWORD InvokeWheelInput(GuestWindowManager* manager, Windows::UI::Core::PointerEventArgs^ args);
        static DWORD InvokeKeyInput(GuestWindowManager* manager, Windows::UI::Core::KeyEventArgs^ args, UINT message);
        static void InvokePointerInputThunk(void* context);
        static void InvokeWheelInputThunk(void* context);
        static void InvokeKeyInputThunk(void* context);
        void HandlePointer(Windows::UI::Core::PointerEventArgs^ args, UINT message);
        void HandleWheel(Windows::UI::Core::PointerEventArgs^ args);
        void HandleKey(Windows::UI::Core::KeyEventArgs^ args, UINT message);
        void DetachHostEvents();

        Platform::Agile<Windows::UI::Core::CoreWindow^> m_coreWindow;
        Platform::Agile<Windows::UI::Core::CoreDispatcher^> m_dispatcher;
        Platform::Agile<Windows::UI::Xaml::Controls::Image^> m_surfaceImage;
        std::shared_ptr<GuestPresentationState> m_presentation;
        std::shared_ptr<GuestInputCallbackState> m_inputCallbacks;
        std::shared_ptr<GuestTimerState> m_timers;
        Windows::Foundation::EventRegistrationToken m_pointerMovedToken{};
        Windows::Foundation::EventRegistrationToken m_pointerPressedToken{};
        Windows::Foundation::EventRegistrationToken m_pointerReleasedToken{};
        Windows::Foundation::EventRegistrationToken m_pointerWheelToken{};
        Windows::Foundation::EventRegistrationToken m_keyDownToken{};
        Windows::Foundation::EventRegistrationToken m_keyUpToken{};

        mutable std::mutex m_classesLock;
        std::unordered_map<std::wstring, std::shared_ptr<WindowClass>> m_classes;
        std::unordered_map<ATOM, std::shared_ptr<WindowClass>> m_classesByAtom;
        ATOM m_nextAtom = 0xC000;

        mutable std::mutex m_windowsLock;
        std::unordered_map<ULONG_PTR, std::shared_ptr<WindowRecord>> m_windows;
        ULONG_PTR m_nextWindow = 0x20000;
        std::atomic<ULONG_PTR> m_foregroundWindow{ 0 };
        std::atomic<ULONG_PTR> m_focusWindow{ 0 };
        std::atomic<ULONG_PTR> m_captureWindow{ 0 };
        std::mutex m_popupMenuLock;
        std::condition_variable m_popupMenuChanged;
        PopupMenuSession m_popupMenu;
        std::atomic<bool> m_active{ false };
        std::atomic<bool> m_inputEnabled{ true };
        std::atomic<bool> m_eventsAttached{ false };

        GuestMessageQueue m_messages;
        MiniGdi::GdiContext m_gdi;
    };

    GuestWindowManager* CurrentGuestWindowManager();

    // Returned as the previous GWLP_WNDPROC when guest code subclasses one of
    // the bridge's stock controls. Calling it through CallWindowProcW routes
    // back to the control's default virtual behavior.
    LRESULT WINAPI BridgeBuiltinControlWindowProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam);

    class GuestWindowScope final
    {
    public:
        explicit GuestWindowScope(GuestWindowManager* manager);
        ~GuestWindowScope();

        GuestWindowScope(const GuestWindowScope&) = delete;
        GuestWindowScope& operator=(const GuestWindowScope&) = delete;

    private:
        GuestWindowManager* m_previous;
    };
}
}
