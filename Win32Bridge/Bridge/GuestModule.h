#pragma once

#include "Bridge/GuestStorage.h"
#include "Bridge/ImportBinder.h"
#include "Bridge/CRT/CrtProcessState.h"
#include "Bridge/CRT/CrtFileIo.h"
#include "Bridge/CRT/CrtStdio.h"
#include "Bridge/CRT/CrtExceptions.h"
#include "Bridge/ProcessEnvironment.h"
#include "Bridge/RuntimeImage.h"

#include <memory>
#include <mutex>
#include <cstdint>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>
#include <agile.h>
#include <windows.storage.h>

namespace Win32Bridge
{
namespace Bridge
{
    // Maps PE DLLs from the executable's authorized source folder, falling
    // back to the guest C: drive for modules intentionally placed there.
    // Module handles are guest image bases, never host HMODULEs.
    class GuestModuleLoader final
    {
    public:
        GuestModuleLoader(
            std::shared_ptr<GuestStorageContext> storage,
            ImportResolver resolver,
            Windows::Storage::StorageFolder^ moduleSourceFolder,
            std::shared_ptr<GuestKernelContext> kernel = {},
            const std::vector<CrtStartupFile>& inherited = {});
        ~GuestModuleLoader();

        bool LoadLibrary(
            LPCWSTR requestedName,
            HMODULE* module,
            DWORD* win32Error,
            DWORD searchFlags = 0,
            DWORD loadBehaviorFlags = 0,
            LPCWSTR searchPath = nullptr,
            LPCWSTR dependencyDirectory = nullptr);
        ImportResolution ResolveImport(const ImportedSymbol& symbol, DWORD searchFlags = 0,
            DWORD* win32Error = nullptr);
        bool GetModuleHandle(LPCWSTR requestedName, HMODULE* module, DWORD* win32Error,
            bool addReference = false, bool pin = false) const;
        bool GetModuleHandleFromAddress(
            ULONG_PTR address, HMODULE* module, DWORD* win32Error,
            bool addReference = false, bool pin = false) const;
        bool AddReference(HMODULE module, bool pin, DWORD* win32Error);
        void SetDefaultDllDirectories(DWORD flags);
        bool AddDllDirectory(LPCWSTR path, PVOID* cookie, DWORD* win32Error);
        bool RemoveDllDirectory(PVOID cookie, DWORD* win32Error);
        bool SetDllDirectory(LPCWSTR path, DWORD* win32Error);
        DWORD GetDllDirectory(DWORD bufferLength, LPWSTR buffer, DWORD* win32Error) const;
        bool GetModulePath(HMODULE module, std::wstring* path, DWORD* win32Error) const;
        FARPROC GetProcAddress(HMODULE module, LPCSTR nameOrOrdinal, DWORD* win32Error);
        bool GetMappedImage(HMODULE module, const BYTE** imageBase, size_t* imageSize) const;
        bool DescribeAddress(ULONG_PTR address, std::wstring* description) const;
        bool FreeLibrary(HMODULE module, DWORD* win32Error);
        bool DisableThreadNotifications(HMODULE module, DWORD* win32Error);
        bool NotifyThread(DWORD reason, std::wstring* error);
        void ReleaseThreadTls();
        void ReleaseAll();
        bool ReadExecutableBytes(
            LPCWSTR requestedName,
            std::vector<BYTE>* bytes,
            std::wstring* logicalPath,
            DWORD* win32Error) const;
        ProcessEnvironment& Environment() { return m_environment; }
        CrtStdio& Stdio() { return m_stdio; }
        CrtFileDescriptors& CrtFiles() { return m_crtFiles; }
        CrtProcessState& CrtState() { return m_crtState; }
        CrtThreadExceptionState* ExceptionThreadState(bool freshThread);
        std::shared_ptr<GuestStorageContext> StorageContext() const { return m_storage; }
        Windows::Storage::StorageFolder^ ModuleSourceFolder() const
        {
            return m_moduleSourceFolder.Get();
        }

    private:
        struct Module;
        struct ModuleCandidate;

        ImportResolution ResolveModuleImport(const ImportedSymbol& symbol,
            const std::shared_ptr<Module>& importer, DWORD searchFlags, DWORD* win32Error,
            LPCWSTR searchPath = nullptr, LPCWSTR dependencyDirectory = nullptr);

        bool BuildCandidates(
            LPCWSTR requestedName,
            DWORD searchFlags,
            std::vector<ModuleCandidate>* candidates,
            std::wstring* requestedBaseName,
            bool* searchByBaseName,
            DWORD* win32Error,
            LPCWSTR searchPath = nullptr, LPCWSTR dependencyDirectory = nullptr) const;
        static void ReferenceModuleLocked(Module& module, bool pin);
        std::shared_ptr<Module> FindModuleLocked(HMODULE module) const;
        std::shared_ptr<Module> FindModuleByIdentityLocked(const std::wstring& identity) const;
        std::shared_ptr<Module> FindModuleByBaseNameLocked(const std::wstring& baseName) const;
        bool ReadModuleBytes(const ModuleCandidate& candidate, std::vector<BYTE>* bytes) const;
        bool ReadAuthorizedModuleBytes(
            const std::wstring& relativeName,
            std::vector<BYTE>* bytes) const;
        bool NotifyModule(const std::shared_ptr<Module>& module, DWORD reason,
            std::wstring* error) const;
        void ReleaseModuleDependencies(const std::shared_ptr<Module>& module);

        ProcessEnvironment m_environment;
        CrtProcessState m_crtState;
        std::shared_ptr<GuestStorageContext> m_storage;
        CrtFileDescriptors m_crtFiles;
        CrtStdio m_stdio;
        std::mutex m_exceptionThreadLock;
        std::unordered_map<DWORD, std::unique_ptr<CrtThreadExceptionState>> m_exceptionThreads;
        ImportResolver m_resolver;
        Platform::Agile<Windows::Storage::StorageFolder^> m_moduleSourceFolder;
        // The native loader serializes mapping, import snapping and entry-point
        // notification under a re-entrant loader lock.  Dependency loading is
        // recursive on the owning thread, hence std::recursive_mutex rather
        // than exposing half-initialized modules to a competing guest thread.
        mutable std::recursive_mutex m_loaderLock;
        mutable std::mutex m_lock;
        std::vector<std::shared_ptr<Module>> m_modules;
        std::unordered_map<std::wstring, HMODULE> m_importModules;
        std::vector<ImportedSymbol> m_resolvingImports;
        std::vector<std::pair<ULONG_PTR, std::wstring>> m_userDllDirectories;
        std::wstring m_legacyDllDirectory;
        bool m_legacyDllDirectorySpecified = false;
        DWORD m_defaultDllDirectoryFlags = 0;
        ULONG_PTR m_nextDllDirectoryCookie = 1;
        std::uint64_t m_nextAttachSequence = 1;
    };

    GuestModuleLoader* CurrentGuestModuleLoader();

    class GuestModuleScope final
    {
    public:
        explicit GuestModuleScope(GuestModuleLoader* loader, bool freshThread = false);
        ~GuestModuleScope();
        GuestModuleScope(const GuestModuleScope&) = delete;
        GuestModuleScope& operator=(const GuestModuleScope&) = delete;

    private:
        GuestModuleLoader* m_previous;
        CrtThreadExceptionState* m_previousExceptions = nullptr;
    };
}
}
