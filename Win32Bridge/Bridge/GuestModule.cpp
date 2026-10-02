#include "pch.h"
#include "Bridge/GuestModule.h"

#include "Bridge/PeImage.h"
#include "Bridge/PeMapper.h"
#include "Bridge/RuntimeDiagnostics.h"

#include <algorithm>
#include <cwctype>

using namespace Win32Bridge::Bridge;
using namespace Windows::Storage;
using namespace Windows::Storage::Streams;
using namespace concurrency;

namespace
{
    constexpr DWORD LoadLibrarySearchDllLoadDir = 0x00000100u;
    constexpr DWORD LoadLibrarySearchApplicationDir = 0x00000200u;
    constexpr DWORD LoadLibrarySearchUserDirs = 0x00000400u;
    constexpr DWORD LoadLibrarySearchSystem32 = 0x00000800u;
    constexpr DWORD LoadLibrarySearchDefaultDirs = 0x00001000u;
    constexpr DWORD SupportedSearchFlags = LoadLibrarySearchDllLoadDir |
        LoadLibrarySearchApplicationDir | LoadLibrarySearchUserDirs |
        LoadLibrarySearchSystem32 | LoadLibrarySearchDefaultDirs;

    thread_local GuestModuleLoader* g_currentModuleLoader = nullptr;
    thread_local unsigned g_forwarderDepth = 0;

    void SetWin32Error(DWORD* target, DWORD value)
    {
        if (target)
        {
            *target = value;
        }
    }

    std::wstring Lowercase(std::wstring value)
    {
        for (auto& character : value)
        {
            character = static_cast<wchar_t>(towlower(character));
        }
        return value;
    }

    std::wstring FileNameOf(const std::wstring& path)
    {
        const size_t separator = path.find_last_of(L'\\');
        return Lowercase(separator == std::wstring::npos ? path : path.substr(separator + 1));
    }

    std::wstring DirectoryOf(const std::wstring& path)
    {
        const size_t separator = path.find_last_of(L'\\');
        return separator == std::wstring::npos ? std::wstring() : path.substr(0, separator);
    }

    std::wstring JoinPath(const std::wstring& directory, const std::wstring& leaf)
    {
        return directory.empty() || directory.back() == L'\\'
            ? directory + leaf
            : directory + L'\\' + leaf;
    }

    bool IsWithinDirectory(
        const std::wstring& path,
        const std::wstring& directory,
        std::wstring* relative)
    {
        if (path.size() <= directory.size() ||
            _wcsnicmp(path.c_str(), directory.c_str(), directory.size()) != 0 ||
            path[directory.size()] != L'\\')
        {
            return false;
        }
        if (relative) *relative = path.substr(directory.size() + 1);
        return true;
    }

    bool NormalizeRequestedName(LPCWSTR requestedName, std::wstring* normalized)
    {
        if (!requestedName || !normalized) return false;
        const size_t length = wcsnlen_s(requestedName, 32768);
        if (length == 0 || length >= 32768) return false;

        normalized->assign(requestedName, length);
        for (auto& character : *normalized)
        {
            if (character == L'/') character = L'\\';
        }
        const size_t separator = normalized->find_last_of(L'\\');
        const size_t leafStart = separator == std::wstring::npos ? 0 : separator + 1;
        if (leafStart == normalized->size()) return false;
        const size_t dot = normalized->find_last_of(L'.');
        if (dot == std::wstring::npos || dot < leafStart) *normalized += L".dll";
        return true;
    }
}

struct GuestModuleLoader::Module final
{
    std::wstring identity;
    std::wstring canonicalName;
    PeImageInfo metadata;
    MappedPeImage mapped;
    RuntimeImage runtime;
    BindingReport bindings;
    ULONG references = 1;
};

struct GuestModuleLoader::ModuleCandidate final
{
    enum class Source
    {
        AuthorizedApplicationFolder,
        VirtualDrive
    };

    Source source = Source::VirtualDrive;
    std::wstring readPath;
    std::wstring identity;
    std::wstring displayName;
};

GuestModuleLoader::GuestModuleLoader(
    std::shared_ptr<GuestStorageContext> storage,
    ImportResolver resolver,
    StorageFolder^ moduleSourceFolder)
    : m_storage(std::move(storage)),
      m_resolver(std::move(resolver)),
      m_moduleSourceFolder(moduleSourceFolder)
{
}

GuestModuleLoader::~GuestModuleLoader()
{
    ReleaseAll();
}

bool GuestModuleLoader::BuildCandidates(
    LPCWSTR requestedName,
    DWORD searchFlags,
    std::vector<ModuleCandidate>* candidates,
    std::wstring* requestedBaseName,
    bool* searchByBaseName,
    DWORD* win32Error) const
{
    if (!candidates || !requestedBaseName || !searchByBaseName || !m_storage ||
        (searchFlags & ~SupportedSearchFlags) != 0)
    {
        SetWin32Error(win32Error, ERROR_INVALID_PARAMETER);
        return false;
    }
    candidates->clear();
    requestedBaseName->clear();
    *searchByBaseName = false;

    std::wstring normalized;
    if (!NormalizeRequestedName(requestedName, &normalized))
    {
        SetWin32Error(win32Error, ERROR_MOD_NOT_FOUND);
        return false;
    }
    *requestedBaseName = FileNameOf(normalized);

    const std::wstring applicationDirectory = DirectoryOf(m_storage->ModulePath());
    if (applicationDirectory.empty())
    {
        SetWin32Error(win32Error, ERROR_BAD_PATHNAME);
        return false;
    }

    auto addVirtual = [&](const std::wstring& path) -> bool
    {
        std::wstring canonical;
        DWORD error = ERROR_SUCCESS;
        if (!m_storage->CanonicalPath(path.c_str(), &canonical, &error))
        {
            SetWin32Error(win32Error, error);
            return false;
        }
        const std::wstring identity = L"guest:" + Lowercase(canonical);
        for (const auto& existing : *candidates)
        {
            if (existing.source == ModuleCandidate::Source::VirtualDrive &&
                existing.identity == identity)
            {
                return true;
            }
        }
        ModuleCandidate candidate;
        candidate.source = ModuleCandidate::Source::VirtualDrive;
        candidate.readPath = canonical;
        candidate.identity = identity;
        candidate.displayName = canonical;
        candidates->push_back(std::move(candidate));
        return true;
    };

    auto addApplicationSource = [&](const std::wstring& relative, const std::wstring& logicalPath)
    {
        if (relative.empty()) return;
        ModuleCandidate candidate;
        candidate.source = ModuleCandidate::Source::AuthorizedApplicationFolder;
        candidate.readPath = relative;
        candidate.identity = L"guest:" + Lowercase(logicalPath);
        candidate.displayName = logicalPath;
        candidates->push_back(std::move(candidate));
    };

    const bool containsPath = normalized.find(L'\\') != std::wstring::npos ||
        (normalized.size() >= 2 && normalized[1] == L':');
    *searchByBaseName = !containsPath;
    if (containsPath)
    {
        std::wstring canonical;
        DWORD error = ERROR_SUCCESS;
        if (!m_storage->CanonicalPath(normalized.c_str(), &canonical, &error))
        {
            SetWin32Error(win32Error, error);
            return false;
        }
        std::wstring sourceRelative;
        if (IsWithinDirectory(canonical, applicationDirectory, &sourceRelative))
        {
            addApplicationSource(sourceRelative, canonical);
        }
        if (!addVirtual(canonical)) return false;
    }
    else
    {
        const DWORD directoryFlags = searchFlags &
            (LoadLibrarySearchApplicationDir | LoadLibrarySearchUserDirs |
             LoadLibrarySearchSystem32 | LoadLibrarySearchDefaultDirs);
        const bool classicSearch = directoryFlags == 0;
        const bool searchApplication = classicSearch ||
            (searchFlags & (LoadLibrarySearchApplicationDir | LoadLibrarySearchDefaultDirs)) != 0;
        const bool searchSystem32 = classicSearch ||
            (searchFlags & (LoadLibrarySearchSystem32 | LoadLibrarySearchDefaultDirs)) != 0;

        if ((searchFlags & LoadLibrarySearchDllLoadDir) != 0)
        {
            SetWin32Error(win32Error, ERROR_INVALID_PARAMETER);
            return false;
        }
        if (searchApplication)
        {
            const std::wstring logicalPath = JoinPath(applicationDirectory, normalized);
            addApplicationSource(normalized, logicalPath);
            if (!addVirtual(logicalPath)) return false;
        }
        if (searchSystem32 && !addVirtual(JoinPath(L"C:\\Windows\\System32", normalized)))
        {
            return false;
        }
        if (classicSearch &&
            (!addVirtual(JoinPath(L"C:\\Windows", normalized)) ||
             !addVirtual(JoinPath(m_storage->CurrentDirectory(), normalized))))
        {
            return false;
        }
        // USER_DIRS is retained as policy even though no user-directory API
        // has populated that list yet.
    }

    SetWin32Error(win32Error, ERROR_SUCCESS);
    return true;
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

bool GuestModuleLoader::ReadAuthorizedModuleBytes(
    const std::wstring& relativeName,
    std::vector<BYTE>* bytes) const
{
    StorageFolder^ current = m_moduleSourceFolder.Get();
    if (!current || !bytes || relativeName.empty() ||
        relativeName.front() == L'\\' || relativeName.find(L':') != std::wstring::npos)
    {
        return false;
    }

    std::vector<std::wstring> components;
    size_t cursor = 0;
    while (cursor <= relativeName.size())
    {
        const size_t separator = relativeName.find(L'\\', cursor);
        const size_t end = separator == std::wstring::npos ? relativeName.size() : separator;
        const std::wstring component = relativeName.substr(cursor, end - cursor);
        if (component.empty() || component == L"." || component == L"..")
        {
            return false;
        }
        components.push_back(component);
        if (separator == std::wstring::npos)
        {
            break;
        }
        cursor = separator + 1;
    }
    if (components.empty())
    {
        return false;
    }

    try
    {
        for (size_t index = 0; index + 1 < components.size(); ++index)
        {
            current = create_task(current->GetFolderAsync(
                ref new Platform::String(components[index].c_str()))).get();
        }

        StorageFile^ file = create_task(current->GetFileAsync(
            ref new Platform::String(components.back().c_str()))).get();
        IBuffer^ buffer = create_task(FileIO::ReadBufferAsync(file)).get();
        if (!buffer || buffer->Length > 128u * 1024u * 1024u)
        {
            return false;
        }

        auto managedBytes = ref new Platform::Array<unsigned char>(buffer->Length);
        DataReader^ reader = DataReader::FromBuffer(buffer);
        reader->ReadBytes(managedBytes);
        bytes->assign(managedBytes->Data, managedBytes->Data + managedBytes->Length);
        delete reader;
        return true;
    }
    catch (Platform::Exception^)
    {
        bytes->clear();
        return false;
    }
}

bool GuestModuleLoader::ReadModuleBytes(
    const ModuleCandidate& candidate,
    std::vector<BYTE>* bytes) const
{
    if (candidate.source == ModuleCandidate::Source::AuthorizedApplicationFolder)
    {
        if (ReadAuthorizedModuleBytes(candidate.readPath, bytes))
        {
            RuntimeDiagnostics::Record(
                L"DLL SOURCE: loaded " + candidate.displayName +
                L" from the authorized application directory.");
            return true;
        }
        return false;
    }

    DWORD ignored = ERROR_SUCCESS;
    if (m_storage && m_storage->ReadAllBytes(candidate.readPath.c_str(), bytes, &ignored))
    {
        RuntimeDiagnostics::Record(
            L"DLL SOURCE: loaded " + candidate.displayName + L" from LocalStorage\\drive_c.");
        return true;
    }
    return false;
}

bool GuestModuleLoader::LoadLibrary(
    LPCWSTR requestedName,
    HMODULE* module,
    DWORD* win32Error,
    DWORD searchFlags)
{
    RuntimeDiagnostics::Record(L"DLL: LoadLibrary request for " + (requestedName ? std::wstring(requestedName) : L"<null>") + L".");
    if (!module || !m_resolver)
    {
        SetWin32Error(win32Error, ERROR_INVALID_PARAMETER);
        RuntimeDiagnostics::Record(L"DLL FAILED: invalid LoadLibrary context.");
        return false;
    }
    *module = nullptr;
    std::vector<ModuleCandidate> candidates;
    std::wstring requestedBaseName;
    bool searchByBaseName = false;
    if (!BuildCandidates(
        requestedName, searchFlags, &candidates, &requestedBaseName,
        &searchByBaseName, win32Error))
    {
        RuntimeDiagnostics::Record(L"DLL FAILED: invalid module name or search policy.");
        return false;
    }

    if (searchByBaseName)
    {
        std::lock_guard<std::mutex> guard(m_lock);
        const auto loaded = FindModuleByBaseNameLocked(requestedBaseName);
        if (loaded)
        {
            ++loaded->references;
            *module = reinterpret_cast<HMODULE>(loaded->runtime.Base());
            SetWin32Error(win32Error, ERROR_SUCCESS);
            RuntimeDiagnostics::Record(
                L"DLL: reused loaded module " + loaded->canonicalName + L" by base name.");
            return true;
        }
    }

    std::vector<BYTE> fileBytes;
    const ModuleCandidate* resolved = nullptr;
    for (const auto& current : candidates)
    {
        {
            std::lock_guard<std::mutex> guard(m_lock);
            const auto loaded = FindModuleByIdentityLocked(current.identity);
            if (loaded)
            {
                ++loaded->references;
                *module = reinterpret_cast<HMODULE>(loaded->runtime.Base());
                SetWin32Error(win32Error, ERROR_SUCCESS);
                RuntimeDiagnostics::Record(L"DLL: reused loaded module " + loaded->canonicalName + L".");
                return true;
            }
        }
        if (ReadModuleBytes(current, &fileBytes))
        {
            resolved = &current;
            break;
        }
    }
    if (!resolved)
    {
        SetWin32Error(win32Error, ERROR_MOD_NOT_FOUND);
        std::wstring searched;
        for (const auto& current : candidates)
        {
            if (!searched.empty()) searched += L"; ";
            searched += current.displayName;
            searched += current.source == ModuleCandidate::Source::AuthorizedApplicationFolder
                ? L" [application source]"
                : L" [virtual drive]";
        }
        RuntimeDiagnostics::Record(
            L"DLL FAILED: no candidate matched " +
            (requestedName ? std::wstring(requestedName) : L"<null>") +
            L". Searched: " + searched + L".");
        return false;
    }

    auto candidate = std::make_shared<Module>();
    candidate->identity = resolved->identity;
    candidate->canonicalName = resolved->displayName;
    const std::wstring& canonicalName = candidate->canonicalName;
    std::wstring preparationError;
    if (!PeImage::Inspect(fileBytes.data(), fileBytes.size(), &candidate->metadata) ||
        !PeMapper::Materialize(fileBytes.data(), fileBytes.size(), &candidate->mapped, &preparationError) ||
        !RuntimeImage::Reserve(candidate->mapped.bytes.size(), candidate->mapped.preferredImageBase, &candidate->runtime, &preparationError) ||
        !PeMapper::ApplyBaseRelocations(
            &candidate->mapped,
            reinterpret_cast<ULONGLONG>(candidate->runtime.Base()),
            &preparationError))
    {
        candidate->runtime.Release();
        SetWin32Error(win32Error, ERROR_PROC_NOT_FOUND);
        RuntimeDiagnostics::Record(
            L"DLL FAILED: image preparation for " + canonicalName + L" (" + preparationError + L").");
        return false;
    }

    // Publish the reserved image before binding imports. Dependency graphs can
    // contain cycles; an import back to this module must resolve to this same
    // image rather than recursively mapping a duplicate.
    {
        std::lock_guard<std::mutex> guard(m_lock);
        m_modules.push_back(candidate);
    }

    const ImportResolver dependencyResolver = [this, candidate, searchFlags](const ImportedSymbol& symbol)
    {
        ImportResolution resolution = m_resolver(symbol);
        if (resolution.targetAddress != 0) return resolution;

        HMODULE dependency = nullptr;
        DWORD error = ERROR_SUCCESS;
        if (!GetModuleHandle(symbol.library.c_str(), &dependency, &error))
        {
            if ((searchFlags & LoadLibrarySearchDllLoadDir) != 0)
            {
                const std::wstring dependencyPath =
                    JoinPath(DirectoryOf(candidate->canonicalName), symbol.library);
                LoadLibrary(dependencyPath.c_str(), &dependency, &error);
            }
            const DWORD inheritedFlags = searchFlags & ~LoadLibrarySearchDllLoadDir;
            if (!dependency &&
                ((searchFlags & LoadLibrarySearchDllLoadDir) == 0 || inheritedFlags != 0))
            {
                LoadLibrary(symbol.library.c_str(), &dependency, &error, inheritedFlags);
            }
            if (!dependency) return resolution;
        }

        LPCSTR requested = symbol.importedByOrdinal
            ? reinterpret_cast<LPCSTR>(static_cast<ULONG_PTR>(symbol.ordinal))
            : nullptr;
        std::string asciiName;
        if (!symbol.importedByOrdinal)
        {
            asciiName.reserve(symbol.name.size());
            for (const wchar_t character : symbol.name)
            {
                if (character > 0x7f) return resolution;
                asciiName.push_back(static_cast<char>(character));
            }
            requested = asciiName.c_str();
        }

        const FARPROC procedure = GetProcAddress(dependency, requested, &error);
        if (procedure)
        {
            resolution.targetAddress = reinterpret_cast<ULONGLONG>(procedure);
            resolution.note = L"Resolved from guest dependency " + symbol.library + L".";
        }
        return resolution;
    };

    if (!ImportBinder::Bind(
            &candidate->mapped,
            candidate->metadata,
            dependencyResolver,
            &candidate->bindings,
            &preparationError) ||
        candidate->bindings.unresolved != 0 ||
        !candidate->runtime.CopyFrom(candidate->mapped, &preparationError) ||
        !candidate->runtime.FinalizeProtections(candidate->mapped, &preparationError))
    {
        {
            std::lock_guard<std::mutex> guard(m_lock);
            m_modules.erase(std::remove(m_modules.begin(), m_modules.end(), candidate), m_modules.end());
        }
        candidate->runtime.Release();
        SetWin32Error(win32Error, ERROR_PROC_NOT_FOUND);
        RuntimeDiagnostics::Record(
            L"DLL FAILED: import binding for " + canonicalName + L" (" + preparationError +
            L"; " + std::to_wstring(candidate->bindings.unresolved) + L" unresolved imports).");
        return false;
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

bool GuestModuleLoader::GetModuleHandle(
    LPCWSTR requestedName,
    HMODULE* module,
    DWORD* win32Error) const
{
    if (!module)
    {
        SetWin32Error(win32Error, ERROR_INVALID_PARAMETER);
        return false;
    }
    *module = nullptr;

    std::vector<ModuleCandidate> candidates;
    std::wstring requestedBaseName;
    bool searchByBaseName = false;
    if (!BuildCandidates(
        requestedName, 0, &candidates, &requestedBaseName,
        &searchByBaseName, win32Error))
    {
        return false;
    }

    std::lock_guard<std::mutex> guard(m_lock);
    std::shared_ptr<Module> found;
    if (searchByBaseName)
    {
        found = FindModuleByBaseNameLocked(requestedBaseName);
    }
    else
    {
        for (const auto& candidate : candidates)
        {
            found = FindModuleByIdentityLocked(candidate.identity);
            if (found) break;
        }
    }
    if (!found)
    {
        SetWin32Error(win32Error, ERROR_MOD_NOT_FOUND);
        return false;
    }

    *module = reinterpret_cast<HMODULE>(found->runtime.Base());
    SetWin32Error(win32Error, ERROR_SUCCESS);
    return true;
}

bool GuestModuleLoader::GetModulePath(
    HMODULE module,
    std::wstring* path,
    DWORD* win32Error) const
{
    if (!path)
    {
        SetWin32Error(win32Error, ERROR_INVALID_PARAMETER);
        return false;
    }
    path->clear();
    std::lock_guard<std::mutex> guard(m_lock);
    const auto found = FindModuleLocked(module);
    if (!found)
    {
        SetWin32Error(win32Error, ERROR_MOD_NOT_FOUND);
        return false;
    }
    *path = found->canonicalName;
    SetWin32Error(win32Error, ERROR_SUCCESS);
    return true;
}

FARPROC GuestModuleLoader::GetProcAddress(HMODULE module, LPCSTR nameOrOrdinal, DWORD* win32Error)
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

    std::shared_ptr<Module> candidate;
    {
        std::lock_guard<std::mutex> guard(m_lock);
        candidate = FindModuleLocked(module);
    }
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
    const ExportedSymbol* found = nullptr;
    for (const auto& symbol : candidate->metadata.exports)
    {
        const bool matches = byOrdinal
            ? symbol.ordinal == raw
            : !symbol.name.empty() && _wcsicmp(symbol.name.c_str(), requested.c_str()) == 0;
        if (matches)
        {
            found = &symbol;
            break;
        }
    }
    if (found && found->rva != 0 && found->forwarder.empty())
    {
        SetWin32Error(win32Error, ERROR_SUCCESS);
        RuntimeDiagnostics::Record(L"GetProcAddress OK: " + candidate->canonicalName + L"!" + requested + L".");
        return reinterpret_cast<FARPROC>(candidate->runtime.Base() + found->rva);
    }

    if (found && !found->forwarder.empty() && g_forwarderDepth < 32)
    {
        const size_t separator = found->forwarder.find_last_of(L'.');
        if (separator != std::wstring::npos && separator != 0 &&
            separator + 1 < found->forwarder.size())
        {
            const std::wstring library = found->forwarder.substr(0, separator) + L".dll";
            const std::wstring forwardedName = found->forwarder.substr(separator + 1);
            ImportedSymbol forwarded;
            forwarded.library = library;
            if (forwardedName.front() == L'#')
            {
                wchar_t* end = nullptr;
                const unsigned long ordinal = wcstoul(forwardedName.c_str() + 1, &end, 10);
                if (end && *end == L'\0' && ordinal <= 0xffff)
                {
                    forwarded.importedByOrdinal = true;
                    forwarded.ordinal = static_cast<WORD>(ordinal);
                }
            }
            else
            {
                forwarded.name = forwardedName;
            }

            const ImportResolution bridgeResolution = m_resolver(forwarded);
            if (bridgeResolution.targetAddress != 0)
            {
                SetWin32Error(win32Error, ERROR_SUCCESS);
                return reinterpret_cast<FARPROC>(bridgeResolution.targetAddress);
            }

            HMODULE forwardedModule = nullptr;
            DWORD error = ERROR_SUCCESS;
            if ((GetModuleHandle(library.c_str(), &forwardedModule, &error) ||
                 LoadLibrary(library.c_str(), &forwardedModule, &error)) &&
                forwardedModule)
            {
                std::string asciiName;
                LPCSTR forwardedProcedure = nullptr;
                if (forwarded.importedByOrdinal)
                {
                    forwardedProcedure = reinterpret_cast<LPCSTR>(
                        static_cast<ULONG_PTR>(forwarded.ordinal));
                }
                else
                {
                    bool ascii = true;
                    for (const wchar_t character : forwarded.name)
                    {
                        if (character > 0x7f) { ascii = false; break; }
                        asciiName.push_back(static_cast<char>(character));
                    }
                    if (ascii) forwardedProcedure = asciiName.c_str();
                }
                if (forwardedProcedure)
                {
                    ++g_forwarderDepth;
                    FARPROC result = GetProcAddress(forwardedModule, forwardedProcedure, &error);
                    --g_forwarderDepth;
                    if (result)
                    {
                        SetWin32Error(win32Error, ERROR_SUCCESS);
                        RuntimeDiagnostics::Record(
                            L"GetProcAddress FORWARDED: " + candidate->canonicalName + L"!" +
                            requested + L" -> " + found->forwarder + L".");
                        return result;
                    }
                }
            }
        }
    }

    SetWin32Error(win32Error, ERROR_PROC_NOT_FOUND);
    RuntimeDiagnostics::Record(L"GetProcAddress FAILED: " + candidate->canonicalName + L"!" + requested + L" was not found.");
    return nullptr;
}

std::shared_ptr<GuestModuleLoader::Module> GuestModuleLoader::FindModuleByIdentityLocked(
    const std::wstring& identity) const
{
    for (const auto& candidate : m_modules)
    {
        if (candidate && candidate->identity == identity) return candidate;
    }
    return nullptr;
}

std::shared_ptr<GuestModuleLoader::Module> GuestModuleLoader::FindModuleByBaseNameLocked(
    const std::wstring& baseName) const
{
    for (const auto& candidate : m_modules)
    {
        if (candidate && FileNameOf(candidate->canonicalName) == baseName) return candidate;
    }
    return nullptr;
}

bool GuestModuleLoader::GetMappedImage(HMODULE module, const BYTE** imageBase, size_t* imageSize) const
{
    if (!imageBase || !imageSize)
    {
        return false;
    }
    *imageBase = nullptr;
    *imageSize = 0;

    std::lock_guard<std::mutex> guard(m_lock);
    const auto candidate = FindModuleLocked(module);
    if (!candidate || !candidate->runtime.Base() || candidate->runtime.Size() == 0)
    {
        return false;
    }
    *imageBase = candidate->runtime.Base();
    *imageSize = candidate->runtime.Size();
    return true;
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
