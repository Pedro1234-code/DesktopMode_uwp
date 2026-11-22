#pragma once

#include <windows.foundation.h>
#include <windows.storage.h>
#include <windows.storage.streams.h>
#include <windows.ui.xaml.controls.h>
#include <windows.ui.xaml.media.h>

#include <memory>

namespace Win32Bridge
{
    struct RuntimeSessionState;
    public delegate void InitialGuestWindowSizeHandler(int width, int height);

    public ref class ShellShortcut sealed
    {
    internal:
        ShellShortcut(Platform::String^ target, Platform::String^ relativePath,
            Platform::String^ arguments, Platform::String^ workingDirectory);
    public:
        property Platform::String^ Target { Platform::String^ get() { return m_target; } }
        property Platform::String^ RelativePath { Platform::String^ get() { return m_relativePath; } }
        property Platform::String^ Arguments { Platform::String^ get() { return m_arguments; } }
        property Platform::String^ WorkingDirectory { Platform::String^ get() { return m_workingDirectory; } }
    private:
        Platform::String^ m_target;
        Platform::String^ m_relativePath;
        Platform::String^ m_arguments;
        Platform::String^ m_workingDirectory;
    };

    public ref class ShellShortcutDirectory sealed
    {
    internal:
        ShellShortcutDirectory(Windows::Foundation::Collections::IVectorView<Platform::String^>^ folders,
            Windows::Foundation::Collections::IVectorView<Platform::String^>^ shortcuts);
    public:
        property Windows::Foundation::Collections::IVectorView<Platform::String^>^ Folders
        { Windows::Foundation::Collections::IVectorView<Platform::String^>^ get() { return m_folders; } }
        property Windows::Foundation::Collections::IVectorView<Platform::String^>^ Shortcuts
        { Windows::Foundation::Collections::IVectorView<Platform::String^>^ get() { return m_shortcuts; } }
    private:
        Windows::Foundation::Collections::IVectorView<Platform::String^>^ m_folders;
        Windows::Foundation::Collections::IVectorView<Platform::String^>^ m_shortcuts;
    };

    // One executable invocation hosted by a CoreShell window.  The runtime
    // owns the guest image and virtual Win32 environment; CoreShell owns the
    // surrounding window chrome, focus and z-order.
    public ref class RuntimeSession sealed
    {
    public:
        RuntimeSession(Windows::UI::Xaml::Controls::Panel^ surfaceHost);
        RuntimeSession(Windows::UI::Xaml::Controls::Panel^ surfaceHost,
            InitialGuestWindowSizeHandler^ initialSizeHandler);
        virtual ~RuntimeSession();

        Windows::Foundation::IAsyncOperation<bool>^ PrepareAsync(
            Windows::Storage::StorageFile^ executable,
            Windows::Storage::StorageFolder^ moduleSourceFolder);
        Windows::Foundation::IAsyncOperation<bool>^ PrepareShortcutAsync(
            Windows::Storage::StorageFile^ executable,
            Windows::Storage::StorageFolder^ moduleSourceFolder,
            Platform::String^ virtualPath, Platform::String^ arguments,
            Platform::String^ workingDirectory);
        Windows::Foundation::IAsyncOperation<int>^ RunAsync();

        property bool IsConsoleApplication { bool get(); }
        unsigned int ConsoleInputMode();
        void SetConsoleSize(unsigned int columns, unsigned int rows);
        bool FeedConsoleInput(Platform::String^ text);
        Platform::String^ DrainConsoleOutput();
        void SetInputEnabled(bool enabled);
        void FlushDiagnostics();
        Windows::UI::Xaml::Media::ImageSource^ LoadExecutableIcon();
        static ShellShortcut^ ReadShellShortcut(Windows::Storage::Streams::IBuffer^ buffer);
        static ShellShortcutDirectory^ ListShellShortcutDirectory(Platform::String^ folderPath);
        static ShellShortcut^ ReadShellShortcutFile(Platform::String^ folderPath,
            Platform::String^ name);
        static Platform::String^ ResolveShortcutPath(Platform::String^ path, Platform::String^ directory);
        static Windows::Foundation::IAsyncOperation<Windows::UI::Xaml::Media::ImageSource^>^
            ReadExecutableIconAsync(Windows::Storage::StorageFile^ executable);
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
