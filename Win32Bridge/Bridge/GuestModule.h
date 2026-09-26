#pragma once

#include "Bridge/GuestStorage.h"
#include "Bridge/ImportBinder.h"
#include "Bridge/RuntimeImage.h"

#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace Win32Bridge
{
namespace Bridge
{
    // Maps PE DLLs stored in the guest C: drive. Module handles are guest
    // image bases, never host HMODULEs, and exports therefore remain inside
    // the same W^X-managed runtime as the main executable.
    class GuestModuleLoader final
    {
    public:
        GuestModuleLoader(std::shared_ptr<GuestStorageContext> storage, ImportResolver resolver);
        ~GuestModuleLoader();

        bool LoadLibrary(LPCWSTR requestedName, HMODULE* module, DWORD* win32Error);
        FARPROC GetProcAddress(HMODULE module, LPCSTR nameOrOrdinal, DWORD* win32Error) const;
        bool FreeLibrary(HMODULE module, DWORD* win32Error);
        void ReleaseAll();

    private:
        struct Module;

        static std::wstring CanonicalName(LPCWSTR requestedName);
        std::shared_ptr<Module> FindModuleLocked(HMODULE module) const;

        std::shared_ptr<GuestStorageContext> m_storage;
        ImportResolver m_resolver;
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
