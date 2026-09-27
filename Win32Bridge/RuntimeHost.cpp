#include "pch.h"
#include "RuntimeHost.h"

using namespace Platform;
using namespace Windows::Foundation;
using namespace Windows::Storage;

namespace
{
    constexpr wchar_t DriveRootNameValue[] = L"drive_c";
    constexpr wchar_t RuntimeLogFileNameValue[] = L"Win32Bridge-runtime.log";
}

Win32Bridge::RuntimeHost::RuntimeHost()
{
}

String^ Win32Bridge::RuntimeHost::DriveRootName::get()
{
    return ref new String(DriveRootNameValue);
}

String^ Win32Bridge::RuntimeHost::RuntimeLogFileName::get()
{
    return ref new String(RuntimeLogFileNameValue);
}

IAsyncOperation<StorageFolder^>^ Win32Bridge::RuntimeHost::EnsureDriveRootAsync()
{
    return ApplicationData::Current->LocalFolder->CreateFolderAsync(
        ref new String(DriveRootNameValue),
        CreationCollisionOption::OpenIfExists);
}
