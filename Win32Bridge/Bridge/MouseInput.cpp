#include "pch.h"
#include "Bridge\\MouseInput.h"
#include "Bridge/RuntimeDiagnostics.h"

#include <cmath>

using namespace Win32Bridge::Bridge;

using namespace Platform;
using namespace Windows::Devices::Input;
using namespace Windows::Foundation;
using namespace Windows::UI::Core;
using namespace Windows::UI::Input;

namespace
{
    // The UWP header partition omits the desktop MAPVK_* macros even though
    // guest applications still pass the documented numeric map types.
    constexpr UINT MapVirtualKeyVkToVsc = 0;
    constexpr UINT MapVirtualKeyVscToVk = 1;
    constexpr UINT MapVirtualKeyVkToChar = 2;
    constexpr UINT MapVirtualKeyVscToVkEx = 3;
    constexpr UINT MapVirtualKeyVkToVscEx = 4;

    UINT StandardScanForVirtualKey(UINT key)
    {
        if (key >= '1' && key <= '9') return key - '1' + 0x02;
        if (key >= VK_F1 && key <= VK_F10) return key - VK_F1 + 0x3b;
        switch (key)
        {
        case VK_ESCAPE: return 0x01;
        case '0': return 0x0b;
        case VK_BACK: return 0x0e;
        case VK_TAB: return 0x0f;
        case 'Q': return 0x10; case 'W': return 0x11; case 'E': return 0x12;
        case 'R': return 0x13; case 'T': return 0x14; case 'Y': return 0x15;
        case 'U': return 0x16; case 'I': return 0x17; case 'O': return 0x18;
        case 'P': return 0x19;
        case VK_RETURN: return 0x1c;
        case VK_CONTROL: case VK_LCONTROL: return 0x1d;
        case VK_RCONTROL: return 0xe01d;
        case 'A': return 0x1e; case 'S': return 0x1f; case 'D': return 0x20;
        case 'F': return 0x21; case 'G': return 0x22; case 'H': return 0x23;
        case 'J': return 0x24; case 'K': return 0x25; case 'L': return 0x26;
        case VK_SHIFT: case VK_LSHIFT: return 0x2a;
        case VK_RSHIFT: return 0x36;
        case 'Z': return 0x2c; case 'X': return 0x2d; case 'C': return 0x2e;
        case 'V': return 0x2f; case 'B': return 0x30; case 'N': return 0x31;
        case 'M': return 0x32;
        case VK_MULTIPLY: return 0x37;
        case VK_MENU: case VK_LMENU: return 0x38;
        case VK_RMENU: return 0xe038;
        case VK_SPACE: return 0x39;
        case VK_CAPITAL: return 0x3a;
        case VK_NUMLOCK: return 0x45;
        case VK_SCROLL: return 0x46;
        case VK_NUMPAD7: return 0x47; case VK_NUMPAD8: return 0x48;
        case VK_NUMPAD9: return 0x49; case VK_SUBTRACT: return 0x4a;
        case VK_NUMPAD4: return 0x4b; case VK_NUMPAD5: return 0x4c;
        case VK_NUMPAD6: return 0x4d; case VK_ADD: return 0x4e;
        case VK_NUMPAD1: return 0x4f; case VK_NUMPAD2: return 0x50;
        case VK_NUMPAD3: return 0x51; case VK_NUMPAD0: return 0x52;
        case VK_DECIMAL: return 0x53;
        case VK_F11: return 0x57; case VK_F12: return 0x58;
        case VK_HOME: return 0xe047; case VK_UP: return 0xe048;
        case VK_PRIOR: return 0xe049; case VK_LEFT: return 0xe04b;
        case VK_RIGHT: return 0xe04d; case VK_END: return 0xe04f;
        case VK_DOWN: return 0xe050; case VK_NEXT: return 0xe051;
        case VK_INSERT: return 0xe052; case VK_DELETE: return 0xe053;
        case VK_LWIN: return 0xe05b; case VK_RWIN: return 0xe05c;
        case VK_APPS: return 0xe05d;
        default: return 0;
        }
    }

    using SehInvocation = void(*)(void*);

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

    struct MouseUpdateCall final
    {
        MouseInputBridge* bridge;
        PointerPoint^ point;
        int wheelDelta;
    };

    void RemoveMouseHandlers(
        CoreWindow^ window,
        Windows::Foundation::EventRegistrationToken moved,
        Windows::Foundation::EventRegistrationToken pressed,
        Windows::Foundation::EventRegistrationToken released,
        Windows::Foundation::EventRegistrationToken wheel)
    {
        if (!window)
        {
            return;
        }

        window->PointerMoved -= moved;
        window->PointerPressed -= pressed;
        window->PointerReleased -= released;
        window->PointerWheelChanged -= wheel;
    }
}

DWORD MouseInputBridge::InvokeUpdate(MouseInputBridge* bridge, PointerPoint^ point, int wheelDelta)
{
    MouseUpdateCall call{ bridge, point, wheelDelta };
    return InvokeSehProtected(&MouseInputBridge::InvokeUpdateThunk, &call);
}

void MouseInputBridge::InvokeUpdateThunk(void* context)
{
    const auto* call = static_cast<MouseUpdateCall*>(context);
    call->bridge->Update(call->point, call->wheelDelta);
}

void MouseInputBridge::Attach(CoreWindow^ window)
{
    std::lock_guard<std::mutex> attachmentLock(m_attachmentMutex);
    CoreWindow^ attached = m_window.Get();
    if (!window || attached == window)
    {
        return;
    }

    DetachLocked();
    m_dispatcher = Platform::Agile<CoreDispatcher^>(window->Dispatcher);
    m_window = Platform::Agile<CoreWindow^>(window);
    attached = m_window.Get();
    if (!attached)
    {
        return;
    }
    m_movedToken = attached->PointerMoved += ref new TypedEventHandler<CoreWindow^, PointerEventArgs^>(
        [this](CoreWindow^, PointerEventArgs^ args)
    {
        try
        {
            const DWORD exceptionCode = InvokeUpdate(this, args ? args->CurrentPoint : nullptr, 0);
            if (exceptionCode != ERROR_SUCCESS) RuntimeDiagnostics::Record(L"MOUSE INPUT SEH: PointerMoved code " + std::to_wstring(static_cast<unsigned long>(exceptionCode)) + L".");
        }
        catch (Exception^ error) { RuntimeDiagnostics::Record(L"MOUSE INPUT EXCEPTION: PointerMoved HRESULT " + std::to_wstring(static_cast<unsigned long>(error->HResult)) + L"."); }
        catch (...) { RuntimeDiagnostics::Record(L"MOUSE INPUT EXCEPTION: PointerMoved raised an unknown exception."); }
    });
    m_pressedToken = attached->PointerPressed += ref new TypedEventHandler<CoreWindow^, PointerEventArgs^>(
        [this](CoreWindow^, PointerEventArgs^ args)
    {
        try
        {
            const DWORD exceptionCode = InvokeUpdate(this, args ? args->CurrentPoint : nullptr, 0);
            if (exceptionCode != ERROR_SUCCESS) RuntimeDiagnostics::Record(L"MOUSE INPUT SEH: PointerPressed code " + std::to_wstring(static_cast<unsigned long>(exceptionCode)) + L".");
        }
        catch (Exception^ error) { RuntimeDiagnostics::Record(L"MOUSE INPUT EXCEPTION: PointerPressed HRESULT " + std::to_wstring(static_cast<unsigned long>(error->HResult)) + L"."); }
        catch (...) { RuntimeDiagnostics::Record(L"MOUSE INPUT EXCEPTION: PointerPressed raised an unknown exception."); }
    });
    m_releasedToken = attached->PointerReleased += ref new TypedEventHandler<CoreWindow^, PointerEventArgs^>(
        [this](CoreWindow^, PointerEventArgs^ args)
    {
        try
        {
            const DWORD exceptionCode = InvokeUpdate(this, args ? args->CurrentPoint : nullptr, 0);
            if (exceptionCode != ERROR_SUCCESS) RuntimeDiagnostics::Record(L"MOUSE INPUT SEH: PointerReleased code " + std::to_wstring(static_cast<unsigned long>(exceptionCode)) + L".");
        }
        catch (Exception^ error) { RuntimeDiagnostics::Record(L"MOUSE INPUT EXCEPTION: PointerReleased HRESULT " + std::to_wstring(static_cast<unsigned long>(error->HResult)) + L"."); }
        catch (...) { RuntimeDiagnostics::Record(L"MOUSE INPUT EXCEPTION: PointerReleased raised an unknown exception."); }
    });
    m_wheelToken = attached->PointerWheelChanged += ref new TypedEventHandler<CoreWindow^, PointerEventArgs^>(
        [this](CoreWindow^, PointerEventArgs^ args)
    {
        try
        {
            auto point = args ? args->CurrentPoint : nullptr;
            const DWORD exceptionCode = InvokeUpdate(this, point, point && point->Properties ? point->Properties->MouseWheelDelta : 0);
            if (exceptionCode != ERROR_SUCCESS) RuntimeDiagnostics::Record(L"MOUSE INPUT SEH: PointerWheelChanged code " + std::to_wstring(static_cast<unsigned long>(exceptionCode)) + L".");
        }
        catch (Exception^ error) { RuntimeDiagnostics::Record(L"MOUSE INPUT EXCEPTION: PointerWheelChanged HRESULT " + std::to_wstring(static_cast<unsigned long>(error->HResult)) + L"."); }
        catch (...) { RuntimeDiagnostics::Record(L"MOUSE INPUT EXCEPTION: PointerWheelChanged raised an unknown exception."); }
    });
}

void MouseInputBridge::Detach()
{
    std::lock_guard<std::mutex> attachmentLock(m_attachmentMutex);
    DetachLocked();
}

void MouseInputBridge::DetachLocked()
{
    CoreWindow^ attached = nullptr;
    CoreDispatcher^ dispatcher = nullptr;
    const Windows::Foundation::EventRegistrationToken moved = m_movedToken;
    const Windows::Foundation::EventRegistrationToken pressed = m_pressedToken;
    const Windows::Foundation::EventRegistrationToken released = m_releasedToken;
    const Windows::Foundation::EventRegistrationToken wheel = m_wheelToken;

    try
    {
        attached = m_window.Get();
        dispatcher = m_dispatcher.Get();
    }
    catch (...)
    {
        // Shutdown can invalidate the originating apartment before the
        // bridge is asked to detach.  Clear local ownership; any remaining
        // CoreWindow delegate only references this process-lifetime bridge.
    }

    // Clear member state before removing or scheduling removal.  A later
    // Attach can therefore use fresh tokens without a delayed callback ever
    // touching the new CoreWindow registration.
    m_window = Platform::Agile<CoreWindow^>();
    m_dispatcher = Platform::Agile<CoreDispatcher^>();
    m_movedToken = Windows::Foundation::EventRegistrationToken{};
    m_pressedToken = Windows::Foundation::EventRegistrationToken{};
    m_releasedToken = Windows::Foundation::EventRegistrationToken{};
    m_wheelToken = Windows::Foundation::EventRegistrationToken{};

    if (!attached)
    {
        return;
    }

    try
    {
        if (dispatcher && dispatcher->HasThreadAccess)
        {
            RemoveMouseHandlers(attached, moved, pressed, released, wheel);
            return;
        }

        if (dispatcher)
        {
            // Agile keeps the old CoreWindow valid across the worker/UI
            // boundary.  The actual event subtraction still occurs only on
            // its owning dispatcher.
            Platform::Agile<CoreWindow^> agileWindow(attached);
            dispatcher->RunAsync(CoreDispatcherPriority::Normal,
                ref new DispatchedHandler([
                    agileWindow,
                    moved,
                    pressed,
                    released,
                    wheel]()
            {
                try
                {
                    RemoveMouseHandlers(
                        agileWindow.Get(),
                        moved,
                        pressed,
                        released,
                        wheel);
                }
                catch (...)
                {
                }
            }));
        }
    }
    catch (...)
    {
        // A dispatcher that is already shutting down cannot service the
        // deferred removal.  Local state is gone, and the static bridge has
        // no guest-owned lifetime to expose to that stale delegate.
    }
}

bool MouseInputBridge::IsMouseDetected() const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_detected;
}

MouseSnapshot MouseInputBridge::Snapshot() const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_snapshot;
}

bool MouseInputBridge::TryDequeue(MouseEvent* event)
{
    if (!event)
    {
        return false;
    }

    std::lock_guard<std::mutex> lock(m_mutex);
    if (m_events.empty())
    {
        return false;
    }

    *event = m_events.front();
    m_events.pop_front();
    return true;
}

SHORT MouseInputBridge::GetAsyncKeyState(int virtualKey)
{
    const unsigned int mask = ButtonMaskForVirtualKey(virtualKey);
    if (mask == 0 && (virtualKey < 0 || virtualKey >= 256))
    {
        return 0;
    }

    std::lock_guard<std::mutex> lock(m_mutex);
    if (mask == 0)
    {
        const size_t index = static_cast<size_t>(virtualKey);
        const bool isDown = (m_keyboardState[index] & 0x80u) != 0;
        const bool wasPressed = m_keyboardPressedSinceRead[index];
        m_keyboardPressedSinceRead[index] = false;
        return static_cast<SHORT>((isDown ? 0x8000 : 0) | (wasPressed ? 0x0001 : 0));
    }
    const bool isDown = (m_snapshot.buttons & mask) != 0;
    const bool wasPressed = (m_pressedSinceRead & mask) != 0;
    m_pressedSinceRead &= ~mask;
    return static_cast<SHORT>((isDown ? 0x8000 : 0) | (wasPressed ? 0x0001 : 0));
}

SHORT MouseInputBridge::GetKeyState(int virtualKey) const
{
    const unsigned int mask = ButtonMaskForVirtualKey(virtualKey);
    if (mask == 0 && (virtualKey < 0 || virtualKey >= 256))
    {
        return 0;
    }

    std::lock_guard<std::mutex> lock(m_mutex);
    if (mask == 0)
    {
        const BYTE state = m_keyboardState[static_cast<size_t>(virtualKey)];
        return static_cast<SHORT>(((state & 0x80u) != 0 ? 0x8000 : 0) |
            ((state & 0x01u) != 0 ? 0x0001 : 0));
    }
    return (m_snapshot.buttons & mask) != 0 ? static_cast<SHORT>(0x8000) : 0;
}

void MouseInputBridge::UpdateKeyState(int virtualKey, bool down, UINT scanCode,
    bool extended)
{
    if (virtualKey < 0 || virtualKey >= 256) return;
    std::lock_guard<std::mutex> lock(m_mutex);
    const size_t index = static_cast<size_t>(virtualKey);
    const bool wasDown = (m_keyboardState[index] & 0x80u) != 0;
    BYTE toggled = static_cast<BYTE>(m_keyboardState[index] & 0x01u);
    if (down && !wasDown &&
        (virtualKey == VK_CAPITAL || virtualKey == VK_NUMLOCK || virtualKey == VK_SCROLL))
    {
        toggled ^= 0x01u;
    }
    m_keyboardState[index] = static_cast<BYTE>(toggled | (down ? 0x80u : 0));
    if (down && !wasDown) m_keyboardPressedSinceRead[index] = true;
    if (scanCode != 0)
    {
        const UINT encodedScan = (scanCode & 0xffu) | (extended ? 0xe000u : 0u);
        m_virtualKeyToScan[index] = encodedScan;
        const size_t scanIndex = static_cast<size_t>(scanCode & 0xffu) |
            (extended ? static_cast<size_t>(0x100u) : 0u);
        m_scanToVirtualKey[scanIndex] = static_cast<UINT>(virtualKey);
    }

    int aggregateKey = 0;
    int leftKey = 0;
    int rightKey = 0;
    if (virtualKey == VK_LSHIFT || virtualKey == VK_RSHIFT)
    {
        aggregateKey = VK_SHIFT;
        leftKey = VK_LSHIFT;
        rightKey = VK_RSHIFT;
    }
    else if (virtualKey == VK_LCONTROL || virtualKey == VK_RCONTROL)
    {
        aggregateKey = VK_CONTROL;
        leftKey = VK_LCONTROL;
        rightKey = VK_RCONTROL;
    }
    else if (virtualKey == VK_LMENU || virtualKey == VK_RMENU)
    {
        aggregateKey = VK_MENU;
        leftKey = VK_LMENU;
        rightKey = VK_RMENU;
    }
    if (aggregateKey != 0)
    {
        const bool aggregateWasDown =
            (m_keyboardState[static_cast<size_t>(aggregateKey)] & 0x80u) != 0;
        const bool aggregateDown =
            (m_keyboardState[static_cast<size_t>(leftKey)] & 0x80u) != 0 ||
            (m_keyboardState[static_cast<size_t>(rightKey)] & 0x80u) != 0;
        BYTE aggregateState = static_cast<BYTE>(
            m_keyboardState[static_cast<size_t>(aggregateKey)] & 0x01u);
        if (aggregateDown) aggregateState |= 0x80u;
        m_keyboardState[static_cast<size_t>(aggregateKey)] = aggregateState;
        if (aggregateDown && !aggregateWasDown)
            m_keyboardPressedSinceRead[static_cast<size_t>(aggregateKey)] = true;
    }
}

void MouseInputBridge::ResetKeyState()
{
    std::lock_guard<std::mutex> lock(m_mutex);
    for (BYTE& state : m_keyboardState)
        state = static_cast<BYTE>(state & 0x01u);
    m_keyboardPressedSinceRead.fill(false);
}

UINT MouseInputBridge::MapVirtualKey(UINT code, UINT mapType) const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    if (mapType == MapVirtualKeyVkToVsc || mapType == MapVirtualKeyVkToVscEx)
    {
        if (code >= m_virtualKeyToScan.size()) return 0;
        UINT scan = m_virtualKeyToScan[code];
        if (scan == 0) scan = StandardScanForVirtualKey(code);
        return mapType == MapVirtualKeyVkToVsc ? (scan & 0xffu) : scan;
    }
    if (mapType == MapVirtualKeyVscToVk || mapType == MapVirtualKeyVscToVkEx)
    {
        const bool extended = (code & 0xff00u) == 0xe000u;
        const size_t scanIndex = static_cast<size_t>(code & 0xffu) |
            (extended ? static_cast<size_t>(0x100u) : 0u);
        UINT key = m_scanToVirtualKey[scanIndex];
        if (key == 0)
        {
            for (UINT candidate = 1; candidate < 256; ++candidate)
            {
                const UINT scan = StandardScanForVirtualKey(candidate);
                if ((scan & 0xffu) == (code & 0xffu) &&
                    ((scan & 0xe000u) != 0) == extended)
                {
                    key = candidate;
                    break;
                }
            }
        }
        if (mapType == MapVirtualKeyVscToVkEx)
        {
            if (key == VK_SHIFT)
                key = (code & 0xffu) == 0x36u ? VK_RSHIFT : VK_LSHIFT;
            else if (key == VK_CONTROL)
                key = extended ? VK_RCONTROL : VK_LCONTROL;
            else if (key == VK_MENU)
                key = extended ? VK_RMENU : VK_LMENU;
        }
        else
        {
            if (key == VK_LSHIFT || key == VK_RSHIFT) key = VK_SHIFT;
            else if (key == VK_LCONTROL || key == VK_RCONTROL) key = VK_CONTROL;
            else if (key == VK_LMENU || key == VK_RMENU) key = VK_MENU;
        }
        return key;
    }
    if (mapType == MapVirtualKeyVkToChar)
    {
        if ((code >= 'A' && code <= 'Z') || (code >= '0' && code <= '9'))
            return code;
        if (code == VK_SPACE) return L' ';
        if (code >= VK_NUMPAD0 && code <= VK_NUMPAD9)
            return L'0' + code - VK_NUMPAD0;
        if (code == VK_MULTIPLY) return L'*';
        if (code == VK_ADD) return L'+';
        if (code == VK_SUBTRACT) return L'-';
        if (code == VK_DECIMAL) return L'.';
        if (code == VK_DIVIDE) return L'/';
    }
    return 0;
}

void MouseInputBridge::Update(PointerPoint^ point, int wheelDelta)
{
    if (!point || !point->PointerDevice || point->PointerDevice->PointerDeviceType != PointerDeviceType::Mouse)
    {
        return;
    }

    const auto properties = point->Properties;
    unsigned int buttons = 0;
    if (properties->IsLeftButtonPressed) buttons |= MouseLeftButton;
    if (properties->IsMiddleButtonPressed) buttons |= MouseMiddleButton;
    if (properties->IsRightButtonPressed) buttons |= MouseRightButton;
    if (properties->IsXButton1Pressed) buttons |= MouseXButton1;
    if (properties->IsXButton2Pressed) buttons |= MouseXButton2;

    std::lock_guard<std::mutex> lock(m_mutex);
    m_pressedSinceRead |= buttons & ~m_snapshot.buttons;
    m_snapshot.x = point->Position.X;
    m_snapshot.y = point->Position.Y;
    m_snapshot.buttons = buttons;
    ++m_snapshot.sequence;
    m_detected = true;
    m_events.push_back({ m_snapshot, wheelDelta });
    if (m_events.size() > 256)
    {
        m_events.pop_front();
    }
}

unsigned int MouseInputBridge::ButtonMaskForVirtualKey(int virtualKey)
{
    switch (virtualKey)
    {
    case VK_LBUTTON: return MouseLeftButton;
    case VK_MBUTTON: return MouseMiddleButton;
    case VK_RBUTTON: return MouseRightButton;
    case VK_XBUTTON1: return MouseXButton1;
    case VK_XBUTTON2: return MouseXButton2;
    default: return 0;
    }
}

MouseInputBridge& Win32Bridge::Bridge::MouseInput()
{
    static MouseInputBridge input;
    return input;
}
