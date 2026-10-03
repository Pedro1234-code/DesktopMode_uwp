#include "pch.h"
#include "RuntimeHost.h"

#include <windows.applicationmodel.h>

#include <vector>

using namespace concurrency;
using namespace Platform;
using namespace Windows::ApplicationModel;
using namespace Windows::Foundation;
using namespace Windows::Storage;

namespace
{
    constexpr wchar_t DriveRootNameValue[] = L"drive_c";
    constexpr wchar_t DefaultDriveContentFolderName[] = L"GuestRuntimeDefaults";
    constexpr wchar_t RuntimeLogFileNameValue[] = L"Win32Bridge-runtime.log";

    task<void> CopyDefaultFileIfMissingAsync(
        StorageFile^ source,
        StorageFolder^ destination)
    {
        return create_task(destination->TryGetItemAsync(source->Name)).then(
            [source, destination](IStorageItem^ existing) -> task<void>
        {
            if (existing) return task_from_result();

            return create_task(source->CopyAsync(
                destination,
                source->Name,
                NameCollisionOption::FailIfExists)).then([](StorageFile^)
            {
            });
        });
    }

    task<void> CopyDefaultTreeIfMissingAsync(
        StorageFolder^ source,
        StorageFolder^ destination)
    {
        return create_task(source->GetFilesAsync()).then(
            [source, destination](
                Windows::Foundation::Collections::IVectorView<StorageFile^>^ files)
            -> task<void>
        {
            std::vector<task<void>> copies;
            for (unsigned index = 0; files && index < files->Size; ++index)
                copies.push_back(CopyDefaultFileIfMissingAsync(
                    files->GetAt(index), destination));

            task<void> filesCopied = copies.empty()
                ? task_from_result()
                : when_all(copies.begin(), copies.end());
            return filesCopied.then([source, destination]()
            {
                return create_task(source->GetFoldersAsync()).then(
                    [destination](
                        Windows::Foundation::Collections::IVectorView<StorageFolder^>^ folders)
                    -> task<void>
                {
                    std::vector<task<void>> copies;
                    for (unsigned index = 0; folders && index < folders->Size; ++index)
                    {
                        StorageFolder^ sourceChild = folders->GetAt(index);
                        copies.push_back(create_task(destination->CreateFolderAsync(
                            sourceChild->Name,
                            CreationCollisionOption::OpenIfExists)).then(
                                [sourceChild](StorageFolder^ destinationChild)
                        {
                            return CopyDefaultTreeIfMissingAsync(
                                sourceChild, destinationChild);
                        }));
                    }

                    return copies.empty()
                        ? task_from_result()
                        : when_all(copies.begin(), copies.end());
                });
            });
        });
    }
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
    return create_async([]()
    {
        return create_task(ApplicationData::Current->LocalFolder->CreateFolderAsync(
            ref new String(DriveRootNameValue),
            CreationCollisionOption::OpenIfExists)).then([](StorageFolder^ driveRoot)
        {
            return create_task(Package::Current->InstalledLocation->TryGetItemAsync(
                ref new String(DefaultDriveContentFolderName))).then(
                    [driveRoot](IStorageItem^ item) -> task<StorageFolder^>
            {
                StorageFolder^ defaults = dynamic_cast<StorageFolder^>(item);
                if (!defaults) return task_from_result(driveRoot);

                return CopyDefaultTreeIfMissingAsync(defaults, driveRoot).then(
                    [driveRoot]()
                {
                    return driveRoot;
                });
            });
        });
    });
}
