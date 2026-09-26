#include "pch.h"
#include "Bridge/GuestModule.h"

#include "Bridge/PeImage.h"
#include "Bridge/PeMapper.h"
#include "Bridge/RuntimeDiagnostics.h"

#include <algorithm>
#include <cwctype>

using namespace Win32Bridge::Bridge;

namespace
{
    thread_local GuestModuleLoader* g_currentModuleLoader = nullptr;

    void SetWin32Error(DWORD* target, DWORD value)
    {
        if (target)
        {
            *target = value;
        }
    }

    void SetMessage(std::wstring* target, const std::wstring& value)
    {
        if (target)
        {
            *target = value;
        }
    }
}

struct GuestModuleLoader::Module final
{
    std::wstring canonicalName;
    PeImageInfo metadata;
    MappedPeImage mapped;
    RuntimeImage runtime;
    BindingReport bindings;
    ULONG references = 1;
};

GuestModuleLoader::GuestModuleLoader(std::shared_ptr<GuestStorageContext> storage, ImportResolver resolver)
    : m_storage(std::move(storage)), m_resolver(std::move(resolver))
{
}

GuestModuleLoader::~GuestModuleLoader()
{
    ReleaseAll();
}

std::wstring GuestModuleLoader::CanonicalName(LPCWSTR requestedName)
{
    if (!requestedName || !*requestedName)
    {
        return std::wstring();
    }

    std::wstring value(requestedName);
    for (auto& character : value)
    {
        if (character == L'/') character = L'\\';
        character = static_cast<wchar_t>(towlower(character));
    }
    const size_t slash = value.find_last_of(L'\\');
    const size_t dot = value.find_last_of(L'.');
    if (dot == std::wstring::npos || (slash != std::wstring::npos && dot < slash))
    {
        value += L".dll";
    }
    return value;
}

std::shared_ptr<GuestModuleLoader::Module> GuestModuleLoader::FindModuleLocked(HMODULE module) const
{
    for (const auto& candidate : m_modules)
    {
        if (candidate && reinterpret_cast<HMODULE>(candidate->runtime.Base()) == module)
        {
            return candidate;
        }
    }
    return nullptr;
}

bool GuestModuleLoader::LoadLibrary(LPCWSTR requestedName, HMODULE* module, DWORD* win32Error)
{
    RuntimeDiagnostics::Record(L"DLL: LoadLibrary request for " + (requestedName ? std::wstring(requestedName) : L"<null>") + L".");
    if (!module || !m_storage || !m_resolver)
    {
        SetWin32Error(win32Error, ERROR_INVALID_PARAMETER);
        RuntimeDiagnostics::Record(L"DLL FAILED: invalid LoadLibrary context.");
        return false;
    }
    *module = nullptr;
    const std::wstring canonicalName = CanonicalName(requestedName);
    if (canonicalName.empty())
    {
        SetWin32Error(win32Error, ERROR_MOD_NOT_FOUND);
        RuntimeDiagnostics::Record(L"DLL FAILED: empty module name.");
        return false;
    }

    {
        std::lock_guard<std::mutex> guard(m_lock);
        for (const auto& candidate : m_modules)
        {
            if (candidate && candidate->canonicalName == canonicalName)
            {
                ++candidate->references;
                *module = reinterpret_cast<HMODULE>(candidate->runtime.Base());
                SetWin32Error(win32Error, ERROR_SUCCESS);
                RuntimeDiagnostics::Record(L"DLL: reused loaded module " + canonicalName + L".");
                return true;
            }
        }
    }

    std::vector<BYTE> fileBytes;
    DWORD storageError = ERROR_SUCCESS;
    if (!m_storage->ReadAllBytes(canonicalName.c_str(), &fileBytes, &storageError))
    {
        SetWin32Error(win32Error, storageError);
        RuntimeDiagnostics::Record(L"DLL FAILED: could not read " + canonicalName + L" (Win32 error " + std::to_wstring(storageError) + L").");
        return false;
    }

    auto candidate = std::make_shared<Module>();
    candidate->canonicalName = canonicalName;
    std::wstring preparationError;
    if (!PeImage::Inspect(fileBytes.data(), fileBytes.size(), &candidate->metadata) ||
        !PeMapper::Materialize(fileBytes.data(), fileBytes.size(), &candidate->mapped, &preparationError) ||
        !RuntimeImage::Reserve(candidate->mapped.bytes.size(), candidate->mapped.preferredImageBase, &candidate->runtime, &preparationError) ||
        !PeMapper::ApplyBaseRelocations(&candidate->mapped, reinterpret_cast<ULONGLONG>(candidate->runtime.Base()), &preparationError) ||
        !ImportBinder::Bind(&candidate->mapped, candidate->metadata, m_resolver, &candidate->bindings, &preparationError) ||
        candidate->bindings.unresolved != 0 ||
        !candidate->runtime.CopyFrom(candidate->mapped, &preparationError) ||
        !candidate->runtime.FinalizeProtections(candidate->mapped, &preparationError))
    {
        candidate->runtime.Release();
        SetWin32Error(win32Error, ERROR_PROC_NOT_FOUND);
        RuntimeDiagnostics::Record(
            L"DLL FAILED: preparation for " + canonicalName + L" (" + preparationError +
            L"; " + std::to_wstring(candidate->bindings.unresolved) + L" unresolved imports).");
        return false;
    }

    // Keep the module reachable before DllMain. This permits a DLL to query
    // itself or load a dependency during DLL_PROCESS_ATTACH.
    {
        std::lock_guard<std::mutex> guard(m_lock);
        m_modules.push_back(candidate);
    }

    if (candidate->mapped.entryPointRva != 0)
    {
        using DllEntry = BOOL(WINAPI*)(HINSTANCE, DWORD, LPVOID);
        const auto entry = reinterpret_cast<DllEntry>(candidate->runtime.Base() + candidate->mapped.entryPointRva);
        bool attached = false;
        try
        {
            attached = entry(reinterpret_cast<HINSTANCE>(candidate->runtime.Base()), DLL_PROCESS_ATTACH, nullptr) != FALSE;
        }
        catch (...)
        {
            attached = false;
        }
        if (!attached)
        {
            std::lock_guard<std::mutex> guard(m_lock);
            m_modules.erase(std::remove(m_modules.begin(), m_modules.end(), candidate), m_modules.end());
            candidate->runtime.Release();
            SetWin32Error(win32Error, ERROR_DLL_INIT_FAILED);
            RuntimeDiagnostics::Record(L"DLL FAILED: " + canonicalName + L" rejected DLL_PROCESS_ATTACH.");
            return false;
        }
    }

    *module = reinterpret_cast<HMODULE>(candidate->runtime.Base());
    SetWin32Error(win32Error, ERROR_SUCCESS);
    RuntimeDiagnostics::Record(
        L"DLL OK: " + canonicalName + L" loaded; " +
        std::to_wstring(candidate->bindings.bound) + L" imports bound.");
    return true;
}

FARPROC GuestModuleLoader::GetProcAddress(HMODULE module, LPCSTR nameOrOrdinal, DWORD* win32Error) const
{
    if (!module || !nameOrOrdinal)
    {
        SetWin32Error(win32Error, ERROR_INVALID_PARAMETER);
        RuntimeDiagnostics::Record(
            L"GetProcAddress FAILED: invalid module or symbol (module=" +
            std::to_wstring(reinterpret_cast<ULONG_PTR>(module)) + L", symbol=" +
            std::to_wstring(reinterpret_cast<ULONG_PTR>(nameOrOrdinal)) + L").");
        return nullptr;
    }

    std::lock_guard<std::mutex> guard(m_lock);
    const auto candidate = FindModuleLocked(module);
    if (!candidate)
    {
        SetWin32Error(win32Error, ERROR_MOD_NOT_FOUND);
        RuntimeDiagnostics::Record(
            L"GetProcAddress FAILED: module handle " +
            std::to_wstring(reinterpret_cast<ULONG_PTR>(module)) + L" is not a loaded guest DLL.");
        return nullptr;
    }

    const ULONG_PTR raw = reinterpret_cast<ULONG_PTR>(nameOrOrdinal);
    const bool byOrdinal = raw <= 0xffff;
    std::wstring requested;
    if (byOrdinal)
    {
        requested = L"#" + std::to_wstring(raw);
    }
    else
    {
        for (const unsigned char* current = reinterpret_cast<const unsigned char*>(nameOrOrdinal); *current; ++current)
        {
            requested.push_back(static_cast<wchar_t>(*current));
        }
    }
    for (const auto& symbol : candidate->metadata.exports)
    {
        const bool matches = byOrdinal
            ? symbol.ordinal == raw
            : !symbol.name.empty() && _wcsicmp(symbol.name.c_str(), requested.c_str()) == 0;
        if (matches && symbol.rva != 0 && symbol.forwarder.empty())
        {
            SetWin32Error(win32Error, ERROR_SUCCESS);
            RuntimeDiagnostics::Record(L"GetProcAddress OK: " + candidate->canonicalName + L"!" + requested + L".");
            return reinterpret_cast<FARPROC>(candidate->runtime.Base() + symbol.rva);
        }
    }

    SetWin32Error(win32Error, ERROR_PROC_NOT_FOUND);
    RuntimeDiagnostics::Record(L"GetProcAddress FAILED: " + candidate->canonicalName + L"!" + requested + L" was not found.");
    return nullptr;
}

bool GuestModuleLoader::FreeLibrary(HMODULE module, DWORD* win32Error)
{
    std::lock_guard<std::mutex> guard(m_lock);
    const auto candidate = FindModuleLocked(module);
    if (!candidate)
    {
        SetWin32Error(win32Error, ERROR_MOD_NOT_FOUND);
        return false;
    }
    if (candidate->references > 1)
    {
        --candidate->references;
    }
    // Keep the mapping valid through guest shutdown. Windows permits callers
    // to retain function pointers only while loaded; keeping it mapped is a
    // deliberately safer initial subset until unload notifications are added.
    SetWin32Error(win32Error, ERROR_SUCCESS);
    return true;
}

void GuestModuleLoader::ReleaseAll()
{
    std::vector<std::shared_ptr<Module>> modules;
    {
        std::lock_guard<std::mutex> guard(m_lock);
        modules.swap(m_modules);
    }
    for (const auto& module : modules)
    {
        if (module)
        {
            module->runtime.Release();
        }
    }
}

GuestModuleLoader* Win32Bridge::Bridge::CurrentGuestModuleLoader()
{
    return g_currentModuleLoader;
}

GuestModuleScope::GuestModuleScope(GuestModuleLoader* loader)
    : m_previous(g_currentModuleLoader)
{
    g_currentModuleLoader = loader;
}

GuestModuleScope::~GuestModuleScope()
{
    g_currentModuleLoader = m_previous;
}
