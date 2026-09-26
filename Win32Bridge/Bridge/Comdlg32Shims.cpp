#include "pch.h"
#include "Bridge/Comdlg32Shims.h"

#include <windows.applicationmodel.core.h>
#include <windows.storage.h>
#include <windows.storage.pickers.h>
#include <windows.ui.core.h>

using namespace Platform;
using namespace Windows::ApplicationModel::Core;
using namespace Windows::Storage;
using namespace Windows::Storage::Pickers;
using namespace Windows::UI::Core;
using namespace concurrency;

namespace
{
    thread_local DWORD g_commonDialogError = 0;
    constexpr DWORD kCdErrInitialization = 0x0002;
    constexpr DWORD kFnErrBufferTooSmall = 0x3003;

    bool IsComdlgLibrary(const std::wstring& library)
    {
        return _wcsicmp(library.c_str(), L"comdlg32.dll") == 0;
    }

    bool IsName(const std::wstring& value, const wchar_t* expected)
    {
        return _wcsicmp(value.c_str(), expected) == 0;
    }

    void SetCommonDialogError(DWORD error)
    {
        g_commonDialogError = error;
    }

    bool CopyGuestPathToCaller(Win32Bridge::Bridge::GuestOpenFileNameW* openFileName, const std::wstring& guestPath)
    {
        if (!openFileName || !openFileName->lpstrFile || openFileName->nMaxFile == 0)
        {
            SetCommonDialogError(kCdErrInitialization);
            return false;
        }

        if (guestPath.size() + 1 > openFileName->nMaxFile)
        {
            // This is the documented failure for an OPENFILENAME buffer that
            // cannot hold the returned path.  Keep the path virtual: no host
            // filesystem path may escape into the PE.
            openFileName->lpstrFile[0] = L'\0';
            SetCommonDialogError(kFnErrBufferTooSmall);
            return false;
        }

        wcscpy_s(openFileName->lpstrFile, openFileName->nMaxFile, guestPath.c_str());
        const size_t separator = guestPath.find_last_of(L'\\');
        openFileName->nFileOffset = static_cast<WORD>(separator == std::wstring::npos ? 0 : separator + 1);
        const size_t dot = guestPath.find_last_of(L'.');
        openFileName->nFileExtension = static_cast<WORD>(dot == std::wstring::npos ? 0 : dot + 1);
        SetCommonDialogError(0);
        return true;
    }

    StorageFolder^ GuestDocumentsFolder()
    {
        StorageFolder^ local = ApplicationData::Current->LocalFolder;
        StorageFolder^ drive = create_task(local->CreateFolderAsync(L"drive_c", CreationCollisionOption::OpenIfExists)).get();
        StorageFolder^ users = create_task(drive->CreateFolderAsync(L"Users", CreationCollisionOption::OpenIfExists)).get();
        StorageFolder^ guest = create_task(users->CreateFolderAsync(L"Default", CreationCollisionOption::OpenIfExists)).get();
        return create_task(guest->CreateFolderAsync(L"Documents", CreationCollisionOption::OpenIfExists)).get();
    }

    bool StageSelectedFile(Win32Bridge::Bridge::GuestOpenFileNameW* openFileName, StorageFile^ selected)
    {
        if (!selected)
        {
            SetCommonDialogError(0);
            return false;
        }

        try
        {
            StorageFolder^ documents = GuestDocumentsFolder();
            StorageFile^ staged = create_task(selected->CopyAsync(
                documents,
                selected->Name,
                NameCollisionOption::GenerateUniqueName)).get();
            return CopyGuestPathToCaller(openFileName, L"C:\\Users\\Default\\Documents\\" + std::wstring(staged->Name->Data()));
        }
        catch (Exception^ exception)
        {
            SetCommonDialogError(HRESULT_FACILITY(exception->HResult) == FACILITY_WIN32
                ? HRESULT_CODE(exception->HResult)
                : kCdErrInitialization);
            return false;
        }
    }

    bool PickOpenFile(Win32Bridge::Bridge::GuestOpenFileNameW* openFileName)
    {
        try
        {
            CoreDispatcher^ dispatcher = CoreApplication::MainView->CoreWindow->Dispatcher;
            if (!dispatcher || dispatcher->HasThreadAccess)
            {
                SetCommonDialogError(kCdErrInitialization);
                return false;
            }

            task_completion_event<StorageFile^> completion;
            auto showPicker = ref new DispatchedHandler([completion]() mutable
            {
                try
                {
                    FileOpenPicker^ picker = ref new FileOpenPicker();
                    picker->SuggestedStartLocation = PickerLocationId::DocumentsLibrary;
                    picker->FileTypeFilter->Append(L"*");
                    create_task(picker->PickSingleFileAsync()).then([completion](StorageFile^ selected) mutable
                    {
                        completion.set(selected);
                    });
                }
                catch (...)
                {
                    completion.set(nullptr);
                }
            });
            create_task(dispatcher->RunAsync(CoreDispatcherPriority::Normal, showPicker)).then([completion](task<void> dispatched) mutable
            {
                try { dispatched.get(); }
                catch (...) { completion.set(nullptr); }
            });

            return StageSelectedFile(openFileName, create_task(completion).get());
        }
        catch (...)
        {
            SetCommonDialogError(kCdErrInitialization);
            return false;
        }
    }

    bool PickSaveFile(Win32Bridge::Bridge::GuestOpenFileNameW* openFileName)
    {
        try
        {
            CoreDispatcher^ dispatcher = CoreApplication::MainView->CoreWindow->Dispatcher;
            if (!dispatcher || dispatcher->HasThreadAccess)
            {
                SetCommonDialogError(kCdErrInitialization);
                return false;
            }

            std::wstring suggested = openFileName && openFileName->lpstrFile ? openFileName->lpstrFile : L"Untitled";
            const size_t separator = suggested.find_last_of(L"\\/");
            if (separator != std::wstring::npos)
            {
                suggested.erase(0, separator + 1);
            }
            if (suggested.empty())
            {
                suggested = L"Untitled";
            }

            task_completion_event<StorageFile^> completion;
            String^ suggestedName = ref new String(suggested.c_str());
            auto showPicker = ref new DispatchedHandler([completion, suggestedName]() mutable
            {
                try
                {
                    FileSavePicker^ picker = ref new FileSavePicker();
                    picker->SuggestedStartLocation = PickerLocationId::DocumentsLibrary;
                    picker->SuggestedFileName = suggestedName;
                    auto types = ref new Platform::Collections::Vector<String^>();
                    types->Append(L".dat");
                    picker->FileTypeChoices->Insert(L"All files", types);
                    create_task(picker->PickSaveFileAsync()).then([completion](StorageFile^ selected) mutable
                    {
                        completion.set(selected);
                    });
                }
                catch (...)
                {
                    completion.set(nullptr);
                }
            });
            create_task(dispatcher->RunAsync(CoreDispatcherPriority::Normal, showPicker)).then([completion](task<void> dispatched) mutable
            {
                try { dispatched.get(); }
                catch (...) { completion.set(nullptr); }
            });

            // The picked file is immediately represented in the virtual C:
            // drive. Later guest writes therefore remain inside LocalFolder,
            // with no dependency on the external picker token.
            return StageSelectedFile(openFileName, create_task(completion).get());
        }
        catch (...)
        {
            SetCommonDialogError(kCdErrInitialization);
            return false;
        }
    }
}

BOOL WINAPI Win32Bridge::Bridge::BridgeGetOpenFileNameW(GuestOpenFileNameW* openFileName)
{
    return PickOpenFile(openFileName) ? TRUE : FALSE;
}

BOOL WINAPI Win32Bridge::Bridge::BridgeGetSaveFileNameW(GuestOpenFileNameW* openFileName)
{
    return PickSaveFile(openFileName) ? TRUE : FALSE;
}

DWORD WINAPI Win32Bridge::Bridge::BridgeCommDlgExtendedError()
{
    return g_commonDialogError;
}

Win32Bridge::Bridge::ImportResolution Win32Bridge::Bridge::ResolveComdlg32Import(const ImportedSymbol& symbol)
{
    auto resolution = CompatibilityCatalog::Resolve(symbol);
    if (!IsComdlgLibrary(symbol.library))
    {
        return resolution;
    }

    if (IsName(symbol.name, L"getopenfilenamew"))
    {
        resolution.targetAddress = reinterpret_cast<ULONGLONG>(&BridgeGetOpenFileNameW);
    }
    else if (IsName(symbol.name, L"getsavefilenamew"))
    {
        resolution.targetAddress = reinterpret_cast<ULONGLONG>(&BridgeGetSaveFileNameW);
    }
    else if (IsName(symbol.name, L"commdlgextendederror"))
    {
        resolution.targetAddress = reinterpret_cast<ULONGLONG>(&BridgeCommDlgExtendedError);
    }

    if (resolution.targetAddress)
    {
        resolution.disposition = ImportDisposition::NeedsBridge;
    }
    return resolution;
}
