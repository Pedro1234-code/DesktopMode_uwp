#pragma once

#include "Bridge\\ImportBinder.h"
#include "Bridge\\RuntimeImage.h"
#include "Bridge\\GuestStorage.h"

#include <memory>
#include <agile.h>
#include <windows.storage.h>

namespace Win32Bridge
{
namespace Bridge
{
    class GuestWindowManager;
    class GuestKernelContext;
    class GuestModuleLoader;
    class GuestRegistryContext;

    // Owns one fully prepared guest image. A guest becomes runnable only after
    // every import has an adapter address and its pages are sealed W^X.
    class GuestRuntime final
    {
    public:
        void SetStorageContext(std::shared_ptr<GuestStorageContext> storage) { m_storage = std::move(storage); }
        void SetWindowManager(std::shared_ptr<GuestWindowManager> windows) { m_windows = std::move(windows); }
        void SetModuleSourceFolder(Windows::Storage::StorageFolder^ folder) { m_moduleSourceFolder = folder; }
        void SetCommandLine(std::wstring commandLine) { m_commandLine = std::move(commandLine); }
        void SetInitialCurrentDirectory(std::wstring directory) { m_initialCurrentDirectory = std::move(directory); }
        void SetSharedWindowManager(bool shared) { m_sharedWindowManager = shared; }
        void SetProcessId(DWORD processId) { m_processId = processId ? processId : 1; }
        void SetMainThreadId(DWORD threadId) { m_mainThreadId = threadId; }
        bool Prepare(const BYTE* fileBytes, size_t fileSize, const ImportResolver& resolver, std::wstring* error);
        bool Run(int* exitCode, std::wstring* error);
        bool LaunchChildProcess(
            LPCWSTR executableName,
            LPCWSTR commandLine,
            LPCWSTR currentDirectory,
            LPPROCESS_INFORMATION processInformation,
            DWORD* win32Error);
        HANDLE LaunchThread(
            SIZE_T stackSize,
            LPTHREAD_START_ROUTINE startAddress,
            LPVOID parameter,
            DWORD creationFlags,
            LPDWORD threadId,
            DWORD* win32Error);

        const PeImageInfo& Metadata() const { return m_metadata; }
        const BindingReport& Bindings() const { return m_bindings; }

    private:
        PeImageInfo m_metadata;
        MappedPeImage m_mapped;
        RuntimeImage m_runtime;
        BindingReport m_bindings;
        std::shared_ptr<GuestStorageContext> m_storage;
        std::shared_ptr<GuestWindowManager> m_windows;
        std::shared_ptr<GuestKernelContext> m_kernel;
        std::shared_ptr<GuestModuleLoader> m_modules;
        std::shared_ptr<GuestRegistryContext> m_registry;
        Platform::Agile<Windows::Storage::StorageFolder^> m_moduleSourceFolder;
        ImportResolver m_resolver;
        std::wstring m_commandLine;
        std::wstring m_initialCurrentDirectory;
        bool m_sharedWindowManager = false;
        DWORD m_processId = 1;
        DWORD m_mainThreadId = 0;
        bool m_ready = false;
    };

    GuestRuntime* CurrentGuestRuntime();
}
}
