#include "pch.h"
#include "RuntimeSession.h"

#include "Bridge\GuestRuntime.h"
#include "Bridge\GuestStorage.h"
#include "Bridge\GuestWindow.h"
#include "Bridge\MouseInput.h"
#include "Bridge\RuntimeDiagnostics.h"
#include "Bridge\Win32Shims.h"

#include <windows.globalization.h>

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
using namespace Windows::Storage::Streams;
using namespace Windows::UI::Core;
using namespace Windows::UI::Xaml;
using namespace Windows::UI::Xaml::Controls;
using namespace Win32Bridge::Bridge;

namespace
{
    constexpr wchar_t RuntimeLogFileName[] = L"Win32Bridge-runtime.log";
    constexpr wchar_t ImportLogFileName[] = L"Win32Bridge-imports.log";

    std::wstring ToWide(String^ value)
    {
        return value ? std::wstring(value->Data()) : std::wstring();
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

    task<std::vector<BYTE>> ReadOptionalFileBytesAsync(StorageFile^ file)
    {
        if (!file) return task_from_result(std::vector<BYTE>{});
        return create_task(FileIO::ReadBufferAsync(file)).then([](task<IBuffer^> previous)
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
    : m_state()
{
    if (!surfaceHost)
    {
        throw ref new InvalidArgumentException(L"A XAML surface host is required.");
    }

    auto state = std::make_shared<RuntimeSessionState>();
    state->windows = std::make_shared<GuestWindowManager>(Window::Current->CoreWindow, surfaceHost);
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

    return create_async([state, executable, moduleSourceFolder]()
    {
        return create_task(FileIO::ReadBufferAsync(executable)).then(
            [state, moduleSourceFolder](IBuffer^ buffer) -> task<bool>
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
            }).then([state, moduleSourceFolder, bytes](std::vector<BYTE> satelliteBytes)
            {
                const std::wstring virtualModulePath =
                    L"C:\\Program Files\\Win32Bridge\\" + state->executableName;
                state->storage = std::make_shared<GuestStorageContext>(
                    ApplicationData::Current->LocalFolder,
                    virtualModulePath);
                state->runtime = std::unique_ptr<GuestRuntime>(new GuestRuntime());
                state->runtime->SetStorageContext(state->storage);
                state->runtime->SetWindowManager(state->windows);
                state->runtime->SetModuleSourceFolder(moduleSourceFolder);

                std::wstring error;
                if (!satelliteBytes.empty() && !state->runtime->SetResourceSatellite(
                    satelliteBytes.data(), satelliteBytes.size(), &error))
                {
                    RuntimeDiagnostics::Record(
                        L"MUI: ignoring an invalid companion resource image: " + error);
                    error.clear();
                }
                else if (satelliteBytes.empty())
                {
                    RuntimeDiagnostics::Record(
                        L"MUI: no companion resource file was found beside the executable.");
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

void Win32Bridge::RuntimeSession::FlushDiagnostics()
{
    PersistDiagnostics();
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
