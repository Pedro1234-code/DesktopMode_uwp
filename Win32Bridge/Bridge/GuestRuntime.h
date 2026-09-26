#pragma once

#include "Bridge\\ImportBinder.h"
#include "Bridge\\RuntimeImage.h"
#include "Bridge\\GuestStorage.h"

#include <memory>

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
        bool Prepare(const BYTE* fileBytes, size_t fileSize, const ImportResolver& resolver, std::wstring* error);
        bool Run(int* exitCode, std::wstring* error);

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
        ImportResolver m_resolver;
        bool m_ready = false;
    };
}
}
