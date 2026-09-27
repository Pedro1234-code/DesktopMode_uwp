#include "pch.h"
#include "RuntimeSession.h"

#include "Bridge\GuestRuntime.h"
#include "Bridge\GuestStorage.h"
#include "Bridge\GuestWindow.h"
#include "Bridge\MouseInput.h"
#include "Bridge\RuntimeDiagnostics.h"
#include "Bridge\Win32Shims.h"

#include <atomic>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

using namespace concurrency;
using namespace Platform;
using namespace Windows::Foundation;
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
    : m_state(nullptr)
{
    if (!surfaceHost)
    {
        throw ref new InvalidArgumentException(L"A XAML surface host is required.");
    }

    auto state = std::unique_ptr<RuntimeSessionState>(new RuntimeSessionState());
    state->windows = std::make_shared<GuestWindowManager>(Window::Current->CoreWindow, surfaceHost);
    state->windows->Activate();
    state->windows->SetInputEnabled(false);

    static std::once_flag mouseInputOnce;
    std::call_once(mouseInputOnce, []
    {
        MouseInput().Attach(Window::Current->CoreWindow);
    });

    m_state = state.release();
}

Win32Bridge::RuntimeSession::~RuntimeSession()
{
    Close();
    if (m_state && !m_state->running.load())
    {
        delete m_state;
        m_state = nullptr;
    }
}

IAsyncOperation<bool>^ Win32Bridge::RuntimeSession::PrepareAsync(
    StorageFile^ executable,
    StorageFolder^ moduleSourceFolder)
{
    if (!m_state || !executable || !moduleSourceFolder)
    {
        throw ref new InvalidArgumentException(L"The executable and its authorized source folder are required.");
    }

    RuntimeSessionState* state = m_state;
    state->prepared.store(false);
    state->closeRequested.store(false);
    state->executable = executable;
    state->sourceFolder = moduleSourceFolder;
    state->executableName = ToWide(executable->Name);
    state->lastError.clear();
    state->importSummary.clear();
    RuntimeDiagnostics::Reset(L"CoreShell: " + state->executableName);
    RuntimeDiagnostics::Record(L"HOST: preparing an executable selected through CoreShell Files.");

    return create_async([this, state, executable, moduleSourceFolder]()
    {
        return create_task(FileIO::ReadBufferAsync(executable)).then(
            [this, state, moduleSourceFolder](IBuffer^ buffer) -> bool
        {
            if (!buffer || buffer->Length == 0)
            {
                state->lastError = L"The selected executable is empty.";
                RuntimeDiagnostics::Record(L"PREPARE FAILED: empty executable file.");
                PersistDiagnostics();
                return false;
            }

            DataReader^ reader = DataReader::FromBuffer(buffer);
            std::vector<BYTE> bytes(buffer->Length);
            reader->ReadBytes(ArrayReference<BYTE>(bytes.data(), static_cast<unsigned int>(bytes.size())));

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
            const bool prepared = state->runtime->Prepare(
                bytes.data(),
                bytes.size(),
                ResolveRuntimeImport,
                &error);
            state->prepared.store(prepared);
            const auto& bindings = state->runtime->Bindings();
            state->importSummary = std::to_wstring(bindings.bound) + L" imports bound; " +
                std::to_wstring(bindings.unresolved) + L" unresolved.";
            PersistImportReport(BuildImportReport(state->executableName, *state->runtime));
            if (!prepared)
            {
                state->lastError = error.empty() ? L"The runtime could not prepare this executable." : error;
            }
            PersistDiagnostics();
            return prepared;
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

    RuntimeSessionState* state = m_state;
    return create_async([this, state]() -> int
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
        PersistDiagnostics();
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
