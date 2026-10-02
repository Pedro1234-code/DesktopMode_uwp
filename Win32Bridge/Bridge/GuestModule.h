#pragma once

#include "Bridge/GuestStorage.h"
#include "Bridge/ImportBinder.h"
#include "Bridge/RuntimeImage.h"

#include <memory>
#include <mutex>
#include <string>
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
            Windows::Storage::StorageFolder^ moduleSourceFolder);
        ~GuestModuleLoader();

        bool LoadLibrary(LPCWSTR requestedName, HMODULE* module, DWORD* win32Error);
        FARPROC GetProcAddress(HMODULE module, LPCSTR nameOrOrdinal, DWORD* win32Error) const;
        bool GetMappedImage(HMODULE module, const BYTE** imageBase, size_t* imageSize) const;
        bool FreeLibrary(HMODULE module, DWORD* win32Error);
        void ReleaseAll();

    private:
        struct Module;

        static std::wstring CanonicalName(LPCWSTR requestedName);
        std::shared_ptr<Module> FindModuleLocked(HMODULE module) const;
        bool ReadModuleBytes(
            const std::wstring& canonicalName,
            std::vector<BYTE>* bytes,
            DWORD* win32Error) const;
        bool ReadAuthorizedModuleBytes(
            const std::wstring& relativeName,
            std::vector<BYTE>* bytes) const;

        std::shared_ptr<GuestStorageContext> m_storage;
        ImportResolver m_resolver;
        Platform::Agile<Windows::Storage::StorageFolder^> m_moduleSourceFolder;
        mutable std::mutex m_lock;
        std::vector<std::shared_ptr<Module>> m_modules;
    };

    GuestModuleLoader* CurrentGuestModuleLoader();

    class GuestModuleScope final
    {
    public:
        explicit GuestModuleScope(GuestModuleLoader* loader);
        ~GuestModuleScope();
        GuestModuleScope(const GuestModuleScope&) = delete;
        GuestModuleScope& operator=(const GuestModuleScope&) = delete;

    private:
        GuestModuleLoader* m_previous;
    };
}
}
