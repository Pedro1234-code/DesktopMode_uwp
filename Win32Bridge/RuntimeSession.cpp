#include "pch.h"
#include "RuntimeSession.h"

#include "Bridge\GuestRuntime.h"
#include "Bridge/GuestResources.h"
#include "Bridge/ShellLinkReader.h"
#include "Bridge/VirtualPath.h"
#include "Bridge/User32Shims.h"
#include "Bridge\GuestStorage.h"
#include "Bridge\GuestWindow.h"
#include "Bridge\MouseInput.h"
#include "Bridge\RuntimeDiagnostics.h"
#include "Bridge\Win32Shims.h"

#include <windows.globalization.h>
#include <windows.storage.accesscache.h>
#include <fileapifromapp.h>
#include <robuffer.h>
#include <wrl/client.h>

#include <atomic>
#include <algorithm>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

using namespace concurrency;
using namespace Platform;
using namespace Windows::Foundation;
using namespace Windows::Globalization;
using namespace Windows::Storage;
using namespace Windows::Storage::AccessCache;
using namespace Windows::Storage::Streams;
using namespace Windows::UI::Core;
using namespace Windows::UI::Xaml;
using namespace Windows::UI::Xaml::Controls;
using namespace Win32Bridge::Bridge;

namespace
{
    constexpr wchar_t RuntimeLogFileName[] = L"Win32Bridge-runtime.log";
    constexpr wchar_t ImportLogFileName[] = L"Win32Bridge-imports.log";
    constexpr wchar_t ExternalFolderToken[] = L"CoreShell.Files.Mount.Removable media";
    constexpr unsigned int IconPreviewLimit = 64 * 1024 * 1024;
    std::atomic<bool> g_diagnosticsSnapshotPending{ false };

    std::wstring ToWide(String^ value)
    {
        return value ? std::wstring(value->Data()) : std::wstring();
    }

    task<StorageFolder^> GetExternalMountFolderAsync()
    {
        try
        {
            auto accessList = StorageApplicationPermissions::FutureAccessList;
            auto token = ref new String(ExternalFolderToken);
            if (!accessList->ContainsItem(token))
                return task_from_result<StorageFolder^>(nullptr);

            return create_task(accessList->GetFolderAsync(token)).then(
                [](task<StorageFolder^> pending)
            {
                try { return pending.get(); }
                catch (Exception^ exception)
                {
                    RuntimeDiagnostics::Record(
                        L"STORAGE: authorized removable drive is unavailable; HRESULT " +
                        std::to_wstring(static_cast<unsigned long>(exception->HResult)) + L".");
                    return static_cast<StorageFolder^>(nullptr);
                }
            });
        }
        catch (Exception^ exception)
        {
            RuntimeDiagnostics::Record(
                L"STORAGE: could not inspect the removable-drive authorization; HRESULT " +
                std::to_wstring(static_cast<unsigned long>(exception->HResult)) + L".");
            return task_from_result<StorageFolder^>(nullptr);
        }
    }

    task<StorageFile^> TryGetFileAsync(StorageFolder^ folder, String^ name)
    {
        if (!folder || !name) return task_from_result<StorageFile^>(nullptr);
        return create_task(folder->TryGetItemAsync(name)).then([](task<IStorageItem^> previous)
        {
            try
            {
                return dynamic_cast<StorageFile^>(previous.get());
            }
            catch (...)
            {
                return static_cast<StorageFile^>(nullptr);
            }
        });
    }

    task<StorageFile^> TryGetMuiFileAsync(
        StorageFolder^ sourceFolder,
        String^ language,
        String^ satelliteName)
    {
        if (!sourceFolder || !language || language->Length() == 0)
            return task_from_result<StorageFile^>(nullptr);
        return create_task(sourceFolder->TryGetItemAsync(language)).then(
            [satelliteName](task<IStorageItem^> previous) -> task<StorageFile^>
        {
            try
            {
                StorageFolder^ languageFolder = dynamic_cast<StorageFolder^>(previous.get());
                return languageFolder
                    ? TryGetFileAsync(languageFolder, satelliteName)
                    : task_from_result<StorageFile^>(nullptr);
            }
            catch (...)
            {
                return task_from_result<StorageFile^>(nullptr);
            }
        });
    }

    task<StorageFile^> FindMuiSatelliteAsync(
        StorageFolder^ sourceFolder,
        const std::wstring& executableName)
    {
        const auto satelliteName = ref new String((executableName + L".mui").c_str());
        std::vector<task<StorageFile^>> probes;
        probes.push_back(TryGetFileAsync(sourceFolder, satelliteName));

        std::vector<std::wstring> seen;
        const auto languages = ApplicationLanguages::Languages;
        for (unsigned index = 0; languages && index < languages->Size; ++index)
        {
            String^ language = languages->GetAt(index);
            const std::wstring key = ToWide(language);
            if (key.empty() || std::find(seen.begin(), seen.end(), key) != seen.end()) continue;
            seen.push_back(key);
            probes.push_back(TryGetMuiFileAsync(sourceFolder, language, satelliteName));
        }
        if (std::find(seen.begin(), seen.end(), L"en-US") == seen.end())
            probes.push_back(TryGetMuiFileAsync(
                sourceFolder, ref new String(L"en-US"), satelliteName));

        return when_all(probes.begin(), probes.end()).then(
            [sourceFolder, satelliteName](const std::vector<StorageFile^>& files)
            -> task<StorageFile^>
        {
            for (StorageFile^ file : files)
                if (file) return task_from_result(file);

            // The host's preferred languages do not necessarily match the
            // language folders of a guest Windows installation. If none of
            // the preferred probes matched, inspect every immediate language
            // directory for the conventional <module>.mui satellite. This is
            // a deterministic resource fallback, not an application-specific
            // path or locale assumption.
            return create_task(sourceFolder->GetFoldersAsync()).then(
                [satelliteName](task<Windows::Foundation::Collections::IVectorView<StorageFolder^>^> previous)
                -> task<StorageFile^>
            {
                std::vector<task<StorageFile^>> fallbacks;
                try
                {
                    const auto folders = previous.get();
                    for (unsigned index = 0; folders && index < folders->Size; ++index)
                        fallbacks.push_back(TryGetFileAsync(folders->GetAt(index), satelliteName));
                }
                catch (...)
                {
                    return task_from_result<StorageFile^>(nullptr);
                }
                if (fallbacks.empty()) return task_from_result<StorageFile^>(nullptr);
                return when_all(fallbacks.begin(), fallbacks.end()).then(
                    [](const std::vector<StorageFile^>& candidates)
                {
                    for (StorageFile^ candidate : candidates) if (candidate) return candidate;
                    return static_cast<StorageFile^>(nullptr);
                });
            });
        });
    }

    task<std::vector<BYTE>> ReadOptionalFileBytesAsync(StorageFile^ file,
        unsigned int maximumSize = MAXDWORD)
    {
        if (!file) return task_from_result(std::vector<BYTE>{});
        auto read = maximumSize == MAXDWORD ? create_task(FileIO::ReadBufferAsync(file))
            : create_task(file->OpenAsync(FileAccessMode::Read)).then(
            [maximumSize](IRandomAccessStream^ stream) -> task<IBuffer^>
        {
            if (!stream || stream->Size == 0 || stream->Size > maximumSize)
                return task_from_result<IBuffer^>(nullptr);
            const auto size = static_cast<unsigned int>(stream->Size);
            return create_task(stream->ReadAsync(
                ref new Windows::Storage::Streams::Buffer(size), size, InputStreamOptions::None))
                .then([stream](IBuffer^ buffer) { return buffer; });
        });
        return read.then([](task<IBuffer^> previous)
        {
            std::vector<BYTE> bytes;
            try
            {
                IBuffer^ buffer = previous.get();
                if (!buffer || buffer->Length == 0) return bytes;
                bytes.resize(buffer->Length);
                DataReader::FromBuffer(buffer)->ReadBytes(
                    ArrayReference<BYTE>(bytes.data(), static_cast<unsigned int>(bytes.size())));
            }
            catch (...)
            {
                bytes.clear();
            }
            return bytes;
        });
    }

    void PersistReport(const std::wstring& report)
    {
        try
        {
            auto text = ref new String(report.c_str());
            create_task(ApplicationData::Current->LocalFolder->CreateFileAsync(
                ref new String(RuntimeLogFileName),
                CreationCollisionOption::ReplaceExisting))
                .then([text](StorageFile^ file)
            {
                return FileIO::WriteTextAsync(file, text);
            }).then([](task<void> completed)
            {
                try { completed.get(); }
                catch (...) { }
            });
        }
        catch (...)
        {
        }
    }

    const wchar_t* DispositionLabel(ImportDisposition disposition)
    {
        switch (disposition)
        {
        case ImportDisposition::NeedsBridge: return L"ADAPTER";
        case ImportDisposition::Deferred: return L"PENDING";
        default: return L"UNSUPPORTED";
        }
    }

    std::wstring BuildImportReport(const std::wstring& executableName, const GuestRuntime& runtime)
    {
        const auto& image = runtime.Metadata();
        const auto& binding = runtime.Bindings();
        std::wstring report = L"Win32Bridge import report: " + executableName + L"\n";

        if (!image.valid)
        {
            report += L"PE metadata is unavailable.";
            if (!image.error.empty())
            {
                report += L"\nError: " + image.error;
            }
            report += L"\n";
            return report;
        }

        unsigned int adapters = 0;
        unsigned int deferred = 0;
        for (const auto& resolution : binding.resolutions)
        {
            if (resolution.disposition == ImportDisposition::NeedsBridge) ++adapters;
            if (resolution.disposition == ImportDisposition::Deferred) ++deferred;
        }

        report += L"x64 PE accepted. " + std::to_wstring(image.imports.size()) +
            L" imports; IAT: " + std::to_wstring(binding.bound) + L" bound, " +
            std::to_wstring(binding.unresolved) + L" unresolved. Catalog: " +
            std::to_wstring(adapters) + L" adapter(s), " +
            std::to_wstring(deferred) + L" pending.\n\n";
        report += L"PE32+ x64\nEntry point RVA (decimal): " +
            std::to_wstring(image.entryPointRva) + L"\n\n";

        for (size_t index = 0; index < image.imports.size(); ++index)
        {
            const auto& symbol = image.imports[index];
            report += L"[";
            if (index < binding.resolutions.size())
            {
                const auto& resolution = binding.resolutions[index];
                report += DispositionLabel(resolution.disposition);
                report += L"] " + symbol.library + L"!";
                report += symbol.importedByOrdinal
                    ? L"#" + std::to_wstring(symbol.ordinal)
                    : symbol.name;
                report += L"\n    " + resolution.note + L"\n";
            }
            else
            {
                report += L"UNRESOLVED] " + symbol.library + L"!";
                report += symbol.importedByOrdinal
                    ? L"#" + std::to_wstring(symbol.ordinal)
                    : symbol.name;
                report += L"\n    Binding did not produce a resolution entry.\n";
            }
        }
        return report;
    }

    void PersistImportReport(const std::wstring& report)
    {
        try
        {
            auto text = ref new String(report.c_str());
            create_task(ApplicationData::Current->LocalFolder->CreateFileAsync(
                ref new String(ImportLogFileName),
                CreationCollisionOption::ReplaceExisting))
                .then([text](StorageFile^ file)
            {
                return FileIO::WriteTextAsync(file, text);
            }).then([](task<void> completed)
            {
                try { completed.get(); }
                catch (...) { }
            });
        }
        catch (...)
        {
            // Import diagnostics must never prevent the guest from starting.
        }
    }
}

struct Win32Bridge::RuntimeSessionState final
{
    std::mutex lock;
    std::shared_ptr<GuestWindowManager> windows;
    std::shared_ptr<GuestStorageContext> storage;
    std::unique_ptr<GuestRuntime> runtime;
    Platform::Agile<StorageFile^> executable;
    Platform::Agile<StorageFolder^> sourceFolder;
    std::wstring executableName;
    std::wstring lastError;
    std::wstring importSummary;
    std::atomic<bool> prepared{ false };
    std::atomic<bool> running{ false };
    std::atomic<bool> closeRequested{ false };
};

Win32Bridge::RuntimeSession::RuntimeSession(Panel^ surfaceHost)
    : RuntimeSession(surfaceHost, nullptr)
{
}

Win32Bridge::RuntimeSession::RuntimeSession(Panel^ surfaceHost,
    InitialGuestWindowSizeHandler^ initialSizeHandler)
    : m_state()
{
    if (!surfaceHost)
    {
        throw ref new InvalidArgumentException(L"A XAML surface host is required.");
    }

    auto state = std::make_shared<RuntimeSessionState>();
    state->windows = std::make_shared<GuestWindowManager>(Window::Current->CoreWindow, surfaceHost);
    if (initialSizeHandler)
    {
        Platform::Agile<InitialGuestWindowSizeHandler^> handler(initialSizeHandler);
        state->windows->SetInitialWindowSizeHandler([handler](int width, int height)
        {
            handler.Get()->Invoke(width, height);
        });
    }
    state->windows->Activate();
    state->windows->SetInputEnabled(false);

    static std::once_flag mouseInputOnce;
    std::call_once(mouseInputOnce, []
    {
        MouseInput().Attach(Window::Current->CoreWindow);
    });

    m_state = std::move(state);
}

Win32Bridge::RuntimeSession::~RuntimeSession()
{
    Close();
    // Outstanding async operations retain their own shared_ptr copy.  This
    // deliberately releases only the CoreShell-facing reference here.
    m_state.reset();
}

IAsyncOperation<bool>^ Win32Bridge::RuntimeSession::PrepareAsync(
    StorageFile^ executable,
    StorageFolder^ moduleSourceFolder)
{
    return PrepareShortcutAsync(executable, moduleSourceFolder, nullptr, nullptr, nullptr);
}

IAsyncOperation<bool>^ Win32Bridge::RuntimeSession::PrepareShortcutAsync(
    StorageFile^ executable, StorageFolder^ moduleSourceFolder,
    String^ virtualPath, String^ arguments, String^ workingDirectory)
{
    if (!m_state || !executable || !moduleSourceFolder)
    {
        throw ref new InvalidArgumentException(L"The executable and its authorized source folder are required.");
    }

    const std::shared_ptr<RuntimeSessionState> state = m_state;
    state->prepared.store(false);
    state->closeRequested.store(false);
    state->executable = executable;
    state->sourceFolder = moduleSourceFolder;
    state->executableName = ToWide(executable->Name);
    state->lastError.clear();
    state->importSummary.clear();
    RuntimeDiagnostics::Reset(L"CoreShell: " + state->executableName);
    RuntimeDiagnostics::Record(L"HOST: preparing an executable selected through CoreShell Files.");

    const auto shortcutPath = ToWide(virtualPath);
    const auto shortcutArguments = ToWide(arguments);
    const auto shortcutDirectory = ToWide(workingDirectory);
    return create_async([state, executable, moduleSourceFolder, shortcutPath, shortcutArguments, shortcutDirectory]()
    {
        auto preparation = create_task(FileIO::ReadBufferAsync(executable)).then(
            [state, moduleSourceFolder, shortcutPath, shortcutArguments, shortcutDirectory](IBuffer^ buffer) -> task<bool>
        {
            if (!buffer || buffer->Length == 0)
            {
                state->lastError = L"The selected executable is empty.";
                RuntimeDiagnostics::Record(L"PREPARE FAILED: empty executable file.");
                PersistReport(RuntimeDiagnostics::Snapshot());
                return task_from_result(false);
            }

            DataReader^ reader = DataReader::FromBuffer(buffer);
            auto bytes = std::make_shared<std::vector<BYTE>>(buffer->Length);
            reader->ReadBytes(ArrayReference<BYTE>(
                bytes->data(), static_cast<unsigned int>(bytes->size())));

            return FindMuiSatelliteAsync(moduleSourceFolder, state->executableName).then(
                [](StorageFile^ satellite)
            {
                if (satellite)
                {
                    RuntimeDiagnostics::Record(L"MUI: found companion resource file '" +
                        ToWide(satellite->Name) + L"'.");
                }
                return ReadOptionalFileBytesAsync(satellite);
            }).then([state, moduleSourceFolder, bytes, shortcutPath, shortcutArguments, shortcutDirectory](std::vector<BYTE> satelliteBytes) -> task<bool>
            {
                if (satelliteBytes.empty())
                    RuntimeDiagnostics::Record(
                        L"MUI: no companion resource file was found beside the executable.");
                return GetExternalMountFolderAsync().then(
                    [state, moduleSourceFolder, bytes, satelliteBytes, shortcutPath, shortcutArguments, shortcutDirectory](StorageFolder^ externalFolder)
                {
                    // Give a picked executable a stable guest-visible directory that
                    // represents the folder the user actually selected.  Treating every
                    // loose executable as if it were installed below Program Files changes
                    // observable application behaviour (portable applications commonly
                    // use their module directory to select their configuration mode).
                    // GuestStorageContext overlays writes in LocalStorage while continuing
                    // to expose the authorised source folder as the read-only lower layer.
                    std::wstring sourceDirectoryName = moduleSourceFolder && moduleSourceFolder->Name
                        ? ToWide(moduleSourceFolder->Name)
                        : std::wstring(L"Application");
                    if (sourceDirectoryName.empty()) sourceDirectoryName = L"Application";
                    const std::wstring virtualModulePath = shortcutPath.empty() ?
                        L"C:\\PortableApps\\" + sourceDirectoryName + L"\\" +
                        state->executableName : shortcutPath;
                    state->storage = std::make_shared<GuestStorageContext>(
                        ApplicationData::Current->LocalFolder,
                        virtualModulePath,
                        moduleSourceFolder,
                        externalFolder);
                    state->runtime = std::unique_ptr<GuestRuntime>(new GuestRuntime());
                    state->runtime->SetStorageContext(state->storage);
                    state->runtime->SetWindowManager(state->windows);
                    state->runtime->SetModuleSourceFolder(moduleSourceFolder);
                    if (!shortcutArguments.empty())
                        state->runtime->SetCommandLine(L"\"" + virtualModulePath + L"\" " + shortcutArguments);
                    state->runtime->SetInitialCurrentDirectory(shortcutDirectory);

                    std::wstring error;
                    if (!satelliteBytes.empty() && !state->runtime->SetResourceSatellite(
                        satelliteBytes.data(), satelliteBytes.size(), &error))
                    {
                        RuntimeDiagnostics::Record(
                            L"MUI: ignoring an invalid companion resource image: " + error);
                        error.clear();
                    }
                    const bool prepared = state->runtime->Prepare(
                        bytes->data(), bytes->size(), ResolveRuntimeImport, &error);
                    state->prepared.store(prepared);
                    const auto& bindings = state->runtime->Bindings();
                    state->importSummary = std::to_wstring(bindings.bound) + L" imports bound; " +
                        std::to_wstring(bindings.unresolved) + L" unresolved.";
                    PersistImportReport(BuildImportReport(state->executableName, *state->runtime));
                    if (!prepared)
                    {
                        state->lastError = error.empty()
                            ? L"The runtime could not prepare this executable." : error;
                    }
                    PersistReport(RuntimeDiagnostics::Snapshot());
                    return prepared;
                });
            });
        });
        return preparation.then([state](task<bool> completed)
        {
            try
            {
                return completed.get();
            }
            catch (Exception^ exception)
            {
                const unsigned long hresult = static_cast<unsigned long>(
                    exception ? exception->HResult : E_FAIL);
                const std::wstring message = exception && exception->Message
                    ? ToWide(exception->Message) : std::wstring(L"Unspecified error");
                state->lastError = L"Runtime preparation failed (HRESULT " +
                    std::to_wstring(hresult) + L"): " + message;
                state->prepared.store(false);
                RuntimeDiagnostics::Record(L"PREPARE EXCEPTION: " + state->lastError);
                PersistReport(RuntimeDiagnostics::Snapshot());
                return false;
            }
            catch (const std::exception& exception)
            {
                std::wstring message;
                const char* source = exception.what();
                while (source && *source)
                    message.push_back(static_cast<unsigned char>(*source++));
                state->lastError = L"Runtime preparation failed: " + message;
                state->prepared.store(false);
                RuntimeDiagnostics::Record(L"PREPARE EXCEPTION: " + state->lastError);
                PersistReport(RuntimeDiagnostics::Snapshot());
                return false;
            }
            catch (...)
            {
                state->lastError = L"Runtime preparation failed with an unknown exception.";
                state->prepared.store(false);
                RuntimeDiagnostics::Record(L"PREPARE EXCEPTION: unknown exception.");
                PersistReport(RuntimeDiagnostics::Snapshot());
                return false;
            }
        });
    });
}

IAsyncOperation<int>^ Win32Bridge::RuntimeSession::RunAsync()
{
    if (!m_state || !m_state->prepared.load() || !m_state->runtime)
    {
        throw ref new FailureException(L"PrepareAsync must complete successfully before RunAsync.");
    }
    if (m_state->closeRequested.load())
    {
        throw ref new OperationCanceledException(L"The runtime session was closed before execution began.");
    }

    const std::shared_ptr<RuntimeSessionState> state = m_state;
    return create_async([state]() -> int
    {
        state->running.store(true);
        state->windows->SetInputEnabled(true);
        int exitCode = -1;
        std::wstring error;
        const bool succeeded = state->runtime->Run(&exitCode, &error);
        state->running.store(false);
        state->windows->SetInputEnabled(false);
        if (!succeeded)
        {
            state->lastError = error.empty() ? L"The guest executable stopped unexpectedly." : error;
            exitCode = -1;
        }
        PersistReport(RuntimeDiagnostics::Snapshot());
        return exitCode;
    });
}

void Win32Bridge::RuntimeSession::SetInputEnabled(bool enabled)
{
    if (m_state && m_state->windows)
    {
        m_state->windows->SetInputEnabled(enabled && !m_state->closeRequested.load());
    }
}

Win32Bridge::ShellShortcut::ShellShortcut(String^ target, String^ relativePath,
    String^ arguments, String^ workingDirectory)
    : m_target(target), m_relativePath(relativePath), m_arguments(arguments),
      m_workingDirectory(workingDirectory)
{
}

String^ Win32Bridge::RuntimeSession::ResolveShortcutPath(String^ path, String^ directory)
{
    if (!path || !path->Length() || !directory) return nullptr;
    VirtualPathResolver resolver;
    GuestPath base;
    GuestPath target;
    if (!resolver.Resolve(directory->Data(), &base, nullptr) ||
        !resolver.SetCurrentDirectoryPath(base, nullptr) ||
        !resolver.Resolve(path->Data(), &target, nullptr))
        return nullptr;
    return ref new String(target.canonical.c_str());
}

Win32Bridge::ShellShortcutDirectory::ShellShortcutDirectory(
    Windows::Foundation::Collections::IVectorView<String^>^ folders,
    Windows::Foundation::Collections::IVectorView<String^>^ shortcuts)
    : m_folders(folders), m_shortcuts(shortcuts)
{
}

Win32Bridge::ShellShortcutDirectory^ Win32Bridge::RuntimeSession::ListShellShortcutDirectory(String^ folderPath)
{
    if (!folderPath || !folderPath->Length())
        throw ref new InvalidArgumentException();
    auto folders = ref new Platform::Collections::Vector<String^>();
    auto shortcuts = ref new Platform::Collections::Vector<String^>();
    const auto pattern = ToWide(folderPath) + L"\\*";
    WIN32_FIND_DATAW data{};
    HANDLE search = ::FindFirstFileExFromAppW(pattern.c_str(), FindExInfoBasic,
        &data, FindExSearchNameMatch, nullptr, 0);
    if (search != INVALID_HANDLE_VALUE)
    {
        const auto closeSearch = [](void* handle) { ::FindClose(handle); };
        std::unique_ptr<void, decltype(closeSearch)> guard(search, closeSearch);
        do
        {
            const std::wstring name(data.cFileName);
            if (data.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) continue;
            if (data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
            {
                if (name != L"." && name != L"..")
                    folders->Append(ref new String(name.c_str()));
            }
            else if (name.size() >= 4 && _wcsicmp(name.c_str() + name.size() - 4, L".lnk") == 0)
                shortcuts->Append(ref new String(name.c_str()));
        } while (::FindNextFileW(search, &data));
        const DWORD error = ::GetLastError();
        if (error != ERROR_NO_MORE_FILES)
            throw Exception::CreateException(HRESULT_FROM_WIN32(error));
    }
    else
    {
        const DWORD error = ::GetLastError();
        if (error != ERROR_FILE_NOT_FOUND && error != ERROR_NO_MORE_FILES)
            throw Exception::CreateException(HRESULT_FROM_WIN32(error));
    }
    return ref new ShellShortcutDirectory(folders->GetView(), shortcuts->GetView());
}

namespace
{
    Win32Bridge::ShellShortcut^ ReadShellShortcutBytes(const std::vector<BYTE>& bytes)
    {
        ShellLinkData link;
        if (!ParseShellLink(bytes, link)) return nullptr;
        return ref new Win32Bridge::ShellShortcut(ref new String(link.target.c_str()),
            ref new String(link.relativePath.c_str()), ref new String(link.arguments.c_str()),
            ref new String(link.workingDirectory.c_str()));
    }
}

Win32Bridge::ShellShortcut^ Win32Bridge::RuntimeSession::ReadShellShortcutFile(
    String^ folderPath, String^ fileName)
{
    const auto name = ToWide(fileName);
    if (!folderPath || !folderPath->Length() || name.size() < 4 ||
        name.find_first_of(L"\\/:*?") != std::wstring::npos ||
        _wcsicmp(name.c_str() + name.size() - 4, L".lnk") != 0)
        throw ref new InvalidArgumentException();

    const auto path = ToWide(folderPath) + L"\\" + name;
    HANDLE file = ::CreateFile2FromAppW(path.c_str(), GENERIC_READ,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, OPEN_EXISTING, nullptr);
    if (file == INVALID_HANDLE_VALUE)
        throw Exception::CreateException(HRESULT_FROM_WIN32(::GetLastError()));
    const auto closeFile = [](void* handle) { ::CloseHandle(handle); };
    std::unique_ptr<void, decltype(closeFile)> guard(file, closeFile);
    LARGE_INTEGER size{};
    if (!::GetFileSizeEx(file, &size))
        throw Exception::CreateException(HRESULT_FROM_WIN32(::GetLastError()));
    if (size.QuadPart <= 0 || size.QuadPart > static_cast<LONGLONG>(MaximumShellLinkSize)) return nullptr;
    std::vector<BYTE> bytes(static_cast<size_t>(size.QuadPart));
    DWORD read = 0;
    if (!::ReadFile(file, bytes.data(), static_cast<DWORD>(bytes.size()), &read, nullptr))
        throw Exception::CreateException(HRESULT_FROM_WIN32(::GetLastError()));
    return read == bytes.size() ? ReadShellShortcutBytes(bytes) : nullptr;
}

Win32Bridge::ShellShortcut^ Win32Bridge::RuntimeSession::ReadShellShortcut(IBuffer^ buffer)
{
    if (!buffer || !buffer->Length || buffer->Length > MaximumShellLinkSize) return nullptr;
    std::vector<BYTE> bytes(buffer->Length);
    DataReader::FromBuffer(buffer)->ReadBytes(ArrayReference<BYTE>(bytes.data(), buffer->Length));
    return ReadShellShortcutBytes(bytes);
}

namespace
{
    Windows::UI::Xaml::Media::ImageSource^ CreateExecutableIconBitmap(const MiniGdi::Surface& icon)
    {
        if (icon.Empty()) return nullptr;
        auto bitmap = ref new Windows::UI::Xaml::Media::Imaging::WriteableBitmap(
            icon.Width(), icon.Height());
        auto buffer = bitmap->PixelBuffer;
        Microsoft::WRL::ComPtr<Windows::Storage::Streams::IBufferByteAccess> access;
        BYTE* pixels = nullptr;
        if (!buffer || FAILED(reinterpret_cast<IInspectable*>(buffer)->QueryInterface(
            IID_PPV_ARGS(&access))) || FAILED(access->Buffer(&pixels)) || !pixels ||
            buffer->Length < icon.Pixels().size() * sizeof(MiniGdi::Color))
            return nullptr;

        // WriteableBitmap expects premultiplied BGRA.
        for (const auto color : icon.Pixels())
        {
            const unsigned alpha = MiniGdi::Alpha(color);
            *pixels++ = static_cast<BYTE>((MiniGdi::Blue(color) * alpha + 127) / 255);
            *pixels++ = static_cast<BYTE>((MiniGdi::Green(color) * alpha + 127) / 255);
            *pixels++ = static_cast<BYTE>((MiniGdi::Red(color) * alpha + 127) / 255);
            *pixels++ = static_cast<BYTE>(alpha);
        }
        bitmap->Invalidate();
        return bitmap;
    }
}

Windows::UI::Xaml::Media::ImageSource^ Win32Bridge::RuntimeSession::LoadExecutableIcon()
{
    if (!m_state || !m_state->prepared.load() || !m_state->runtime) return nullptr;
    try
    {
        MiniGdi::Surface icon;
        return m_state->runtime->CopyExecutableIcon(32, &icon) ? CreateExecutableIconBitmap(icon) : nullptr;
    }
    catch (...)
    {
        return nullptr;
    }
}

IAsyncOperation<Windows::UI::Xaml::Media::ImageSource^>^
Win32Bridge::RuntimeSession::ReadExecutableIconAsync(StorageFile^ executable)
{
    using Windows::UI::Xaml::Media::ImageSource;
    const auto ui = task_continuation_context::use_current();
    return create_async([executable, ui]() -> task<ImageSource^>
    {
        return ReadOptionalFileBytesAsync(executable, IconPreviewLimit).then([](std::vector<BYTE> bytes)
        {
            MiniGdi::Surface icon;
            MappedPeImage mapped;
            std::wstring error;
            if (!bytes.empty() && PeMapper::Materialize(
                bytes.data(), bytes.size(), &mapped, &error, IconPreviewLimit))
            {
                GuestResourceScope resources(mapped.bytes.data(), mapped.bytes.size());
                CopyGuestExecutableIconPixels(48, &icon);
            }
            return icon;
        }, task_continuation_context::use_arbitrary()).then([](MiniGdi::Surface icon) -> ImageSource^
        {
            return CreateExecutableIconBitmap(icon);
        }, ui);
    });
}

void Win32Bridge::RuntimeSession::FlushDiagnostics()
{
    // This method is called by a DispatcherTimer. Building the report copies
    // and concatenates up to 1024 diagnostic lines under a mutex, so doing it
    // inline periodically pauses the Shell UI and its custom mouse pointer.
    if (g_diagnosticsSnapshotPending.exchange(true)) return;
    try
    {
        create_task([]
        {
            try
            {
                PersistReport(RuntimeDiagnostics::Snapshot());
            }
            catch (...)
            {
            }
            g_diagnosticsSnapshotPending.store(false);
        });
    }
    catch (...)
    {
        g_diagnosticsSnapshotPending.store(false);
    }
}

void Win32Bridge::RuntimeSession::Close()
{
    if (!m_state || m_state->closeRequested.exchange(true))
    {
        return;
    }
    if (m_state->windows)
    {
        m_state->windows->SetInputEnabled(false);
        m_state->windows->PostGuestQuitMessage(0);
    }
    PersistDiagnostics();
}

String^ Win32Bridge::RuntimeSession::ExecutableName::get()
{
    return ref new String(m_state ? m_state->executableName.c_str() : L"");
}

String^ Win32Bridge::RuntimeSession::LastError::get()
{
    return ref new String(m_state ? m_state->lastError.c_str() : L"");
}

String^ Win32Bridge::RuntimeSession::ImportSummary::get()
{
    return ref new String(m_state ? m_state->importSummary.c_str() : L"");
}

bool Win32Bridge::RuntimeSession::IsPrepared::get()
{
    return m_state && m_state->prepared.load();
}

bool Win32Bridge::RuntimeSession::IsRunning::get()
{
    return m_state && m_state->running.load();
}

void Win32Bridge::RuntimeSession::PersistDiagnostics()
{
    PersistReport(RuntimeDiagnostics::Snapshot());
}
