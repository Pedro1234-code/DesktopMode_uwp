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
    if (mask == 0)
    {
        return 0;
    }

    std::lock_guard<std::mutex> lock(m_mutex);
    const bool isDown = (m_snapshot.buttons & mask) != 0;
    const bool wasPressed = (m_pressedSinceRead & mask) != 0;
    m_pressedSinceRead &= ~mask;
    return static_cast<SHORT>((isDown ? 0x8000 : 0) | (wasPressed ? 0x0001 : 0));
}

SHORT MouseInputBridge::GetKeyState(int virtualKey) const
{
    const unsigned int mask = ButtonMaskForVirtualKey(virtualKey);
    if (mask == 0)
    {
        return 0;
    }

    std::lock_guard<std::mutex> lock(m_mutex);
    return (m_snapshot.buttons & mask) != 0 ? static_cast<SHORT>(0x8000) : 0;
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
