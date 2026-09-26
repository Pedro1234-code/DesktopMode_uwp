#include "Bridge\\GuestRuntime.h"
#include "Bridge\\GuestKernel.h"
#include "Bridge\\GuestModule.h"
#include "Bridge/GuestRegistry.h"
#include "Bridge/GuestResources.h"
#include "Bridge\\GuestWindow.h"
#include "Bridge/RuntimeDiagnostics.h"

using namespace Win32Bridge::Bridge;

namespace
{
    void SetError(std::wstring* error, const std::wstring& message)
    {
        if (error)
        {
            *error = message;
        }
    }
}

bool GuestRuntime::Prepare(const BYTE* fileBytes, size_t fileSize, const ImportResolver& resolver, std::wstring* error)
{
    RuntimeDiagnostics::Record(L"PREPARE: inspecting main PE (" + std::to_wstring(fileSize) + L" bytes).");
    m_ready = false;
    m_runtime.Release();
    m_metadata = PeImageInfo{};
    m_mapped = MappedPeImage{};
    m_bindings = BindingReport{};
    m_kernel.reset();
    m_modules.reset();
    m_registry.reset();
    m_resolver = resolver;

    if (!PeImage::Inspect(fileBytes, fileSize, &m_metadata))
    {
        SetError(error, m_metadata.error);
        RuntimeDiagnostics::Record(L"PREPARE FAILED: PE inspection: " + m_metadata.error);
        return false;
    }
    if (!PeMapper::Materialize(fileBytes, fileSize, &m_mapped, error))
    {
        RuntimeDiagnostics::Record(L"PREPARE FAILED: PE materialization.");
        return false;
    }
    if (!RuntimeImage::Reserve(m_mapped.bytes.size(), m_mapped.preferredImageBase, &m_runtime, error))
    {
        RuntimeDiagnostics::Record(L"PREPARE FAILED: executable image reservation.");
        return false;
    }
    if (!PeMapper::ApplyBaseRelocations(&m_mapped, reinterpret_cast<ULONGLONG>(m_runtime.Base()), error))
    {
        RuntimeDiagnostics::Record(L"PREPARE FAILED: base relocations.");
        return false;
    }
    if (!ImportBinder::Bind(&m_mapped, m_metadata, resolver, &m_bindings, error))
    {
        RuntimeDiagnostics::Record(L"PREPARE FAILED: import binding.");
        return false;
    }
    if (m_bindings.unresolved != 0)
    {
        SetError(error, std::to_wstring(m_bindings.unresolved) +
            L" guest imports are still unresolved; execution was not attempted.");
        RuntimeDiagnostics::Record(L"PREPARE FAILED: " + std::to_wstring(m_bindings.unresolved) + L" unresolved main imports.");
        return false;
    }
    if (!m_runtime.CopyFrom(m_mapped, error) || !m_runtime.FinalizeProtections(m_mapped, error))
    {
        RuntimeDiagnostics::Record(L"PREPARE FAILED: image copy or final page protections.");
        return false;
    }

    m_ready = true;
    RuntimeDiagnostics::Record(
        L"PREPARE OK: main image reserved; " + std::to_wstring(m_bindings.bound) +
        L" imports bound; entry RVA " + std::to_wstring(m_mapped.entryPointRva) + L".");
    return true;
}

bool GuestRuntime::Run(int* exitCode, std::wstring* error)
{
    if (!m_ready || !exitCode)
    {
        SetError(error, L"The guest has not been fully prepared for execution.");
        RuntimeDiagnostics::Record(L"RUN FAILED: called before a successful prepare.");
        return false;
    }

    // Kernel objects are per invocation, just like the current guest storage
    // and window scopes.  They must not survive a later run of the same image.
    m_kernel = std::make_shared<GuestKernelContext>();
    m_modules = std::make_shared<GuestModuleLoader>(m_storage, m_resolver);
    m_registry = std::make_shared<GuestRegistryContext>();
    GuestKernelScope kernelScope(m_kernel.get());
    GuestModuleScope moduleScope(m_modules.get());
    GuestRegistryScope registryScope(m_registry.get());
    GuestResourceScope resourceScope(m_runtime.Base(), m_runtime.Size());
    GuestStorageScope storageScope(m_storage.get());
    GuestWindowScope windowScope(m_windows.get());
    if (m_storage && !m_storage->EnsureLayout(error))
    {
        m_kernel->CloseAll();
        m_modules->ReleaseAll();
        RuntimeDiagnostics::Record(L"RUN FAILED: virtual drive layout could not be ensured.");
        return false;
    }
    if (m_storage)
    {
        DWORD storageError = ERROR_SUCCESS;
        if (!m_storage->ResetCurrentDirectory(&storageError))
        {
            m_kernel->CloseAll();
            m_modules->ReleaseAll();
            SetError(error, L"Could not reset the guest current directory (" + std::to_wstring(storageError) + L").");
            RuntimeDiagnostics::Record(L"RUN FAILED: guest current-directory reset error " + std::to_wstring(storageError) + L".");
            return false;
        }
    }

    using GuestEntryPoint = int(WINAPI*)();
    const auto entryPoint = reinterpret_cast<GuestEntryPoint>(m_runtime.Base() + m_mapped.entryPointRva);
    try
    {
        RuntimeDiagnostics::Record(L"RUN: entering guest entry point.");
        *exitCode = entryPoint();
        if (m_storage)
        {
            m_storage->CloseAll();
        }
        m_kernel->CloseAll();
        m_modules->ReleaseAll();
        RuntimeDiagnostics::Record(L"RUN: guest returned exit code " + std::to_wstring(*exitCode) + L".");
        return true;
    }
    catch (Platform::Exception^ exception)
    {
        if (m_storage)
        {
            m_storage->CloseAll();
        }
        m_kernel->CloseAll();
        m_modules->ReleaseAll();
        SetError(error, L"The guest raised an unhandled WinRT exception.");
        RuntimeDiagnostics::Record(L"RUN FAILED: guest raised a WinRT exception; HRESULT " +
            std::to_wstring(static_cast<unsigned long>(exception->HResult)) + L".");
        return false;
    }
    catch (...)
    {
        if (m_storage)
        {
            m_storage->CloseAll();
        }
        m_kernel->CloseAll();
        m_modules->ReleaseAll();
        SetError(error, L"The guest raised an unhandled C++ exception.");
        RuntimeDiagnostics::Record(L"RUN FAILED: guest raised an unhandled C++ exception.");
        return false;
    }
}
