#pragma once

#include <windows.foundation.h>
#include <windows.storage.h>

namespace Win32Bridge
{
    // Public, UI-independent entry point consumed by CoreShell.  Guest
    // sessions and their presentation surface will be layered on this host.
    public ref class RuntimeHost sealed
    {
    public:
        RuntimeHost();

        property Platform::String^ DriveRootName
        {
            Platform::String^ get();
        }

        property Platform::String^ RuntimeLogFileName
        {
            Platform::String^ get();
        }

        Windows::Foundation::IAsyncOperation<Windows::Storage::StorageFolder^>^
            EnsureDriveRootAsync();
    };
}
