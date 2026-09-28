#pragma once

#include <windows.foundation.h>
#include <windows.storage.h>
#include <windows.ui.xaml.controls.h>

#include <memory>

namespace Win32Bridge
{
    struct RuntimeSessionState;

    // One executable invocation hosted by a CoreShell window.  The runtime
    // owns the guest image and virtual Win32 environment; CoreShell owns the
    // surrounding window chrome, focus and z-order.
    public ref class RuntimeSession sealed
    {
    public:
        RuntimeSession(Windows::UI::Xaml::Controls::Panel^ surfaceHost);
        virtual ~RuntimeSession();

        Windows::Foundation::IAsyncOperation<bool>^ PrepareAsync(
            Windows::Storage::StorageFile^ executable,
            Windows::Storage::StorageFolder^ moduleSourceFolder);
        Windows::Foundation::IAsyncOperation<int>^ RunAsync();

        void SetInputEnabled(bool enabled);
        void FlushDiagnostics();
        void Close();

        property Platform::String^ ExecutableName
        {
            Platform::String^ get();
        }

        property Platform::String^ LastError
        {
            Platform::String^ get();
        }

        property Platform::String^ ImportSummary
        {
            Platform::String^ get();
        }

        property bool IsPrepared
        {
            bool get();
        }

        property bool IsRunning
        {
            bool get();
        }

    private:
        // Async prepare/run continuations retain this shared state directly.
        // The C++/CX wrapper may be released as soon as CoreShell closes a
        // window, while the guest worker is still unwinding its message loop.
        std::shared_ptr<RuntimeSessionState> m_state;
        void PersistDiagnostics();
    };
}
