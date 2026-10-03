#pragma once

#include <agile.h>
#include <windows.h>
#include <windows.foundation.h>
#include <windows.ui.core.h>
#include <windows.ui.input.h>

#include <cstdint>
#include <array>
#include <deque>
#include <mutex>

namespace Win32Bridge
{
namespace Bridge
{
    enum MouseButtonMask : unsigned int
    {
        MouseLeftButton = 1 << 0,
        MouseMiddleButton = 1 << 1,
        MouseRightButton = 1 << 2,
        MouseXButton1 = 1 << 3,
        MouseXButton2 = 1 << 4
    };

    struct MouseSnapshot
    {
        double x = 0;
        double y = 0;
        unsigned int buttons = 0;
        uint64_t sequence = 0;
    };

    struct MouseEvent
    {
        MouseSnapshot snapshot;
        int wheelDelta = 0;
    };

    // Converts the UWP CoreWindow stream (including USB mice on Xbox) into a
    // thread-safe input source for Win32 API adapters and future message queues.
    class MouseInputBridge final
    {
    public:
        void Attach(Windows::UI::Core::CoreWindow^ window);
        void Detach();

        bool IsMouseDetected() const;
        MouseSnapshot Snapshot() const;
        bool TryDequeue(MouseEvent* event);
        SHORT GetAsyncKeyState(int virtualKey);
        SHORT GetKeyState(int virtualKey) const;
        void UpdateKeyState(int virtualKey, bool down, UINT scanCode = 0,
            bool extended = false);
        void ResetKeyState();
        UINT MapVirtualKey(UINT code, UINT mapType) const;

    private:
        // Must be called with m_attachmentMutex held.  CoreWindow event
        // removal has UI-thread affinity, so this snapshots the old
        // attachment and dispatches removal when the caller is a worker.
        void DetachLocked();
        static DWORD InvokeUpdate(MouseInputBridge* bridge, Windows::UI::Input::PointerPoint^ point, int wheelDelta);
        static void InvokeUpdateThunk(void* context);
        void Update(Windows::UI::Input::PointerPoint^ point, int wheelDelta);
        static unsigned int ButtonMaskForVirtualKey(int virtualKey);

        // Attach/Detach can be requested while a guest worker is unwinding.
        // Keep both UI references agile so a worker can safely ask the
        // original dispatcher to perform event removal.
        Platform::Agile<Windows::UI::Core::CoreWindow^> m_window;
        Platform::Agile<Windows::UI::Core::CoreDispatcher^> m_dispatcher;
        Windows::Foundation::EventRegistrationToken m_movedToken{};
        Windows::Foundation::EventRegistrationToken m_pressedToken{};
        Windows::Foundation::EventRegistrationToken m_releasedToken{};
        Windows::Foundation::EventRegistrationToken m_wheelToken{};

        mutable std::mutex m_attachmentMutex;
        mutable std::mutex m_mutex;
        MouseSnapshot m_snapshot;
        unsigned int m_pressedSinceRead = 0;
        std::array<BYTE, 256> m_keyboardState{};
        std::array<bool, 256> m_keyboardPressedSinceRead{};
        std::array<UINT, 256> m_virtualKeyToScan{};
        std::array<UINT, 512> m_scanToVirtualKey{};
        bool m_detected = false;
        std::deque<MouseEvent> m_events;
    };

    MouseInputBridge& MouseInput();
}
}
