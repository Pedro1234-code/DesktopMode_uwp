#pragma once

#include "Bridge\\ImportBinder.h"
#include "Bridge\\RuntimeImage.h"
#include "Bridge\\GuestStorage.h"

#include <memory>
#include <functional>
#include <array>
#include <atomic>
#include <condition_variable>
#include <mutex>
#include <unordered_set>
#include <agile.h>
#include <windows.storage.h>

namespace Win32Bridge
{
namespace Bridge
{
    namespace MiniGdi { class Surface; }
    class GuestWindowManager;
    class GuestKernelContext;
    class GuestConsoleContext;
    class GuestPipeContext;
    class GuestModuleLoader;
    class GuestRegistryContext;

    // Owns one fully prepared guest image. A guest becomes runnable only after
    // every import has an adapter address and its pages are sealed W^X.
    class GuestRuntime final
    {
    public:
        void SetStorageContext(std::shared_ptr<GuestStorageContext> storage) { m_storage = std::move(storage); }
        void SetRegistryContext(std::shared_ptr<GuestRegistryContext> registry) { m_registry = std::move(registry); }
        void SetWindowManager(std::shared_ptr<GuestWindowManager> windows) { m_windows = std::move(windows); }
        void SetModuleSourceFolder(Windows::Storage::StorageFolder^ folder) { m_moduleSourceFolder = folder; }
        void SetCommandLine(std::wstring commandLine) { m_commandLine = std::move(commandLine); }
        void SetInitialCurrentDirectory(std::wstring directory) { m_initialCurrentDirectory = std::move(directory); }
        void SetInitialEnvironment(std::vector<std::wstring> entries) { m_initialEnvironment = std::move(entries); m_hasInitialEnvironment = true; }
        void SetSharedWindowManager(bool shared) { m_sharedWindowManager = shared; }
        void SetProcessId(DWORD processId) { m_processId = processId ? processId : 1; }
        void SetMainThreadId(DWORD threadId) { m_mainThreadId = threadId; }
        bool SetResourceSatellite(
            const BYTE* fileBytes,
            size_t fileSize,
            std::wstring* error);
        bool Prepare(const BYTE* fileBytes, size_t fileSize, const ImportResolver& resolver, std::wstring* error);
        bool CopyExecutableIcon(int size, MiniGdi::Surface* destination) const;
        void RequestStop();
        void RequestProcessExit(DWORD exitCode);
        bool FeedConsoleInput(const std::wstring& text, bool eof = false);
        std::wstring DrainConsoleOutput();
        unsigned ConsoleInputMode();
        void SetConsoleSize(unsigned columns, unsigned rows);
        bool Run(int* exitCode, std::wstring* error, const std::function<void()>& bindingObserver = {});
        bool LaunchChildProcess(
            LPCWSTR executableName,
            LPCWSTR commandLine,
            LPCWSTR currentDirectory,
            LPPROCESS_INFORMATION processInformation,
            DWORD* win32Error,
            const std::vector<std::wstring>* environment = nullptr,
            DWORD creationFlags = 0, bool inheritHandles = false,
            const HANDLE* standardHandles = nullptr,
            const std::vector<BYTE>& startupCrtData = {},
            bool inheritProcessHandle = false, bool inheritThreadHandle = false);
        void FillStartupCrtData(STARTUPINFOW& startupInfo)
        {
            startupInfo.cbReserved2 = static_cast<WORD>(m_startupCrtData.size());
            startupInfo.lpReserved2 = m_startupCrtData.empty() ? nullptr : m_startupCrtData.data();
        }
        HANDLE LaunchThread(
            SIZE_T stackSize,
            LPTHREAD_START_ROUTINE startAddress,
            LPVOID parameter,
            DWORD creationFlags,
            LPDWORD threadId,
            DWORD* win32Error, bool inheritHandle = false);

        const BYTE* ImageBase() const { return m_runtime.Base(); }
        size_t ImageSize() const { return m_runtime.Size(); }
        const PeImageInfo& Metadata() const { return m_metadata; }
        const BindingReport& Bindings() const { return m_bindings; }

    private:
        struct WorkerLease;
        void WaitForWorkers();

        PeImageInfo m_metadata;
        MappedPeImage m_mapped;
        MappedPeImage m_resourceSatellite;
        RuntimeImage m_runtime;
        BindingReport m_bindings;
        std::shared_ptr<GuestStorageContext> m_storage;
        std::shared_ptr<GuestWindowManager> m_windows;
        std::shared_ptr<GuestKernelContext> m_kernel;
        std::shared_ptr<GuestKernelContext> m_consoleTransport;
        std::atomic<unsigned> m_consoleColumns{80}, m_consoleRows{25};
        std::shared_ptr<GuestModuleLoader> m_modules;
        std::shared_ptr<GuestRegistryContext> m_registry;
        Platform::Agile<Windows::Storage::StorageFolder^> m_moduleSourceFolder;
        ImportResolver m_resolver;
        std::wstring m_commandLine;
        std::wstring m_initialCurrentDirectory;
        std::vector<std::wstring> m_initialEnvironment;
        bool m_hasInitialEnvironment = false;
        bool m_sharedWindowManager = false;
        DWORD m_processId = 1;
        DWORD m_mainThreadId = 0;
        std::function<void(DWORD)> m_mainThreadCompleted;
        std::shared_ptr<GuestKernelContext> m_initialKernel;
        std::shared_ptr<GuestConsoleContext> m_initialConsole;
        std::shared_ptr<GuestPipeContext> m_initialPipes;
        std::array<HANDLE,3> m_initialStandardHandles{};
        std::vector<BYTE> m_startupCrtData;
        bool m_hasInitialStandardHandles = false;
        bool m_detachedConsole = false, m_createConsole = false;
        std::atomic<bool> m_stopRequested{false};
        std::mutex m_threadLock;
        std::condition_variable m_threadsChanged;
        std::unordered_set<DWORD> m_workerThreads;
        DWORD m_runningMainThreadId = 0;
        DWORD m_lastWorkerExitCode = 0;
        DWORD m_processExitCode = 0;
        bool m_threadsMayRun = false;
        bool m_processExitRequested = false;
        bool m_ready = false;
    };

    GuestRuntime* CurrentGuestRuntime();

    // Implements ExitProcess/ExitThread as bridge control flow without exposing
    // a private exception code to the guest's VEH/SEH chain.  Returns false when
    // the calling thread is not currently executing a guest entry point.
    bool RequestGuestExit(DWORD exitCode, bool threadExit);
    void ExitGuestThreadOnShutdown();
}
}
