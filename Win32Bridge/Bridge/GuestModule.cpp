#include "pch.h"
#include "Bridge/GuestModule.h"

#include "Bridge/ApiSet.h"
#include "Bridge/ActivationContext.h"
#include "Bridge/PeImage.h"
#include "Bridge/PeMapper.h"
#include "Bridge/GuestResources.h"
#include "Bridge/RuntimeDiagnostics.h"

#include <algorithm>
#include <cwctype>
#include <cwchar>
#include <initializer_list>
#include <limits>
#include <unordered_map>

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
    constexpr DWORD ActCtxResourceNameValid = 0x00000008u;
    constexpr DWORD ActCtxModuleValid = 0x00000080u;

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

    bool InvokeDllEntry(
        BYTE* base,
        DWORD entryPointRva,
        DWORD reason,
        LPVOID reserved,
        BOOL* result)
    {
        if (result) *result = TRUE;
        if (!base || entryPointRva == 0) return true;
        using DllEntry = BOOL(WINAPI*)(HINSTANCE, DWORD, LPVOID);
        const auto entry = reinterpret_cast<DllEntry>(base + entryPointRva);
        __try
        {
            const BOOL value = entry(reinterpret_cast<HINSTANCE>(base), reason, reserved);
            if (result) *result = value;
            return true;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return false;
        }
    }

    class ScopedActivationContext final
    {
    public:
        explicit ScopedActivationContext(HANDLE context)
        {
            if (context && context != INVALID_HANDLE_VALUE)
                m_active = BridgeActivateActCtx(context, &m_cookie) != FALSE;
        }

        ~ScopedActivationContext()
        {
            if (m_active) BridgeDeactivateActCtx(0, m_cookie);
        }

        ScopedActivationContext(const ScopedActivationContext&) = delete;
        ScopedActivationContext& operator=(const ScopedActivationContext&) = delete;

    private:
        ULONG_PTR m_cookie = 0;
        bool m_active = false;
    };

    bool IsAbsoluteGuestPath(LPCWSTR path)
    {
        if (!path || !*path) return false;
        if (path[0] == L'\\' || path[0] == L'/') return true;
        return path[0] != L'\0' && path[1] == L':' &&
            (path[2] == L'\\' || path[2] == L'/');
    }

    const GuestResourceIdentifier* PreferredManifest(
        const std::vector<GuestResourceIdentifier>& names,
        std::initializer_list<WORD> preferredIds)
    {
        for (const WORD preferred : preferredIds)
            for (const auto& name : names)
                if (name.ordinal && name.id == preferred) return &name;
        return names.empty() ? nullptr : &names.front();
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
    std::uint64_t attachSequence = 0;
    bool loading = true;
    bool unloading = false;
    bool pinned = false;
    bool resourceOnly = false;
    bool processAttached = false;
    bool threadNotifications = true;
    HANDLE activationContext = nullptr;
    std::unordered_map<std::wstring, HMODULE> dependencyModules;
    std::vector<HMODULE> dependencyReferences;
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
    const std::wstring apiSetHost = ApiSetHostLibrary(normalized);
    if (IsApiSetLibrary(normalized))
    {
        RuntimeDiagnostics::Record(
            L"APISET: " + normalized + L" -> " + apiSetHost + L".");
        normalized = apiSetHost;
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
        std::wstring activationPath;
        if (ResolveGuestActivationContextModule(normalized.c_str(), &activationPath) &&
            !addVirtual(activationPath))
        {
            return false;
        }
        if (searchApplication)
        {
            const std::wstring logicalPath = JoinPath(applicationDirectory, normalized);
            addApplicationSource(normalized, logicalPath);
            if (!addVirtual(logicalPath)) return false;
        }
        const bool searchUserDirectories =
            (searchFlags & (LoadLibrarySearchUserDirs | LoadLibrarySearchDefaultDirs)) != 0;
        if (classicSearch &&
            m_legacyDllDirectorySpecified && !m_legacyDllDirectory.empty() &&
            !addVirtual(JoinPath(m_legacyDllDirectory, normalized)))
        {
            return false;
        }
        if (searchUserDirectories)
        {
            for (const auto& directory : m_userDllDirectories)
            {
                if (!addVirtual(JoinPath(directory.second, normalized))) return false;
            }
        }
        if (searchSystem32 && !addVirtual(JoinPath(L"C:\\Windows\\System32", normalized)))
        {
            return false;
        }
        if (classicSearch && !addVirtual(JoinPath(L"C:\\Windows", normalized)))
        {
            return false;
        }
        if (classicSearch && !m_legacyDllDirectorySpecified &&
            !addVirtual(JoinPath(m_storage->CurrentDirectory(), normalized)))
        {
            return false;
        }
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

bool GuestModuleLoader::ReadExecutableBytes(
    LPCWSTR requestedName,
    std::vector<BYTE>* bytes,
    std::wstring* logicalPath,
    DWORD* win32Error) const
{
    std::lock_guard<std::recursive_mutex> loaderGuard(m_loaderLock);
    if (!bytes || !logicalPath)
    {
        SetWin32Error(win32Error, ERROR_INVALID_PARAMETER);
        return false;
    }
    bytes->clear();
    logicalPath->clear();

    std::vector<ModuleCandidate> candidates;
    std::wstring requestedBaseName;
    bool searchByBaseName = false;
    if (!BuildCandidates(
        requestedName, 0, &candidates, &requestedBaseName,
        &searchByBaseName, win32Error))
    {
        return false;
    }
    for (const auto& candidate : candidates)
    {
        if (ReadModuleBytes(candidate, bytes))
        {
            *logicalPath = candidate.displayName;
            SetWin32Error(win32Error, ERROR_SUCCESS);
            RuntimeDiagnostics::Record(
                L"PROCESS SOURCE: loaded " + candidate.displayName + L".");
            return true;
        }
    }
    SetWin32Error(win32Error, ERROR_FILE_NOT_FOUND);
    return false;
}

bool GuestModuleLoader::AddDllDirectory(
    LPCWSTR path, PVOID* cookie, DWORD* win32Error)
{
    std::lock_guard<std::recursive_mutex> loaderGuard(m_loaderLock);
    if (!path || !*path || !cookie || !m_storage || !IsAbsoluteGuestPath(path))
    {
        SetWin32Error(win32Error, ERROR_INVALID_PARAMETER);
        return false;
    }
    *cookie = nullptr;
    std::wstring canonical;
    DWORD error = ERROR_SUCCESS;
    if (!m_storage->CanonicalPath(path, &canonical, &error) ||
        canonical.size() < 3 || canonical[1] != L':')
    {
        SetWin32Error(win32Error, error == ERROR_SUCCESS ? ERROR_INVALID_PARAMETER : error);
        return false;
    }
    const DWORD attributes = m_storage->GetGuestFileAttributes(canonical.c_str(), &error);
    if (attributes == INVALID_FILE_ATTRIBUTES || (attributes & FILE_ATTRIBUTE_DIRECTORY) == 0)
    {
        SetWin32Error(win32Error, error == ERROR_SUCCESS ? ERROR_PATH_NOT_FOUND : error);
        return false;
    }
    ULONG_PTR token = m_nextDllDirectoryCookie++;
    while (token == 0 || token == reinterpret_cast<ULONG_PTR>(INVALID_HANDLE_VALUE) ||
        std::find_if(m_userDllDirectories.begin(), m_userDllDirectories.end(),
            [token](const std::pair<ULONG_PTR, std::wstring>& item)
            { return item.first == token; }) != m_userDllDirectories.end())
    {
        token = m_nextDllDirectoryCookie++;
    }
    m_userDllDirectories.emplace_back(token, std::move(canonical));
    *cookie = reinterpret_cast<PVOID>(token);
    SetWin32Error(win32Error, ERROR_SUCCESS);
    return true;
}

bool GuestModuleLoader::RemoveDllDirectory(PVOID cookie, DWORD* win32Error)
{
    std::lock_guard<std::recursive_mutex> loaderGuard(m_loaderLock);
    const ULONG_PTR token = reinterpret_cast<ULONG_PTR>(cookie);
    const auto found = std::find_if(m_userDllDirectories.begin(), m_userDllDirectories.end(),
        [token](const std::pair<ULONG_PTR, std::wstring>& item)
        { return token != 0 && item.first == token; });
    if (found == m_userDllDirectories.end())
    {
        SetWin32Error(win32Error, ERROR_INVALID_PARAMETER);
        return false;
    }
    m_userDllDirectories.erase(found);
    SetWin32Error(win32Error, ERROR_SUCCESS);
    return true;
}

bool GuestModuleLoader::SetDllDirectory(LPCWSTR path, DWORD* win32Error)
{
    std::lock_guard<std::recursive_mutex> loaderGuard(m_loaderLock);
    if (!path)
    {
        m_legacyDllDirectory.clear();
        m_legacyDllDirectorySpecified = false;
        SetWin32Error(win32Error, ERROR_SUCCESS);
        return true;
    }
    if (!*path)
    {
        m_legacyDllDirectory.clear();
        m_legacyDllDirectorySpecified = true;
        SetWin32Error(win32Error, ERROR_SUCCESS);
        return true;
    }
    if (wcschr(path, L';'))
    {
        SetWin32Error(win32Error, ERROR_INVALID_PARAMETER);
        return false;
    }
    std::wstring canonical;
    DWORD error = ERROR_SUCCESS;
    if (!m_storage || !m_storage->CanonicalPath(path, &canonical, &error))
    {
        SetWin32Error(win32Error, error == ERROR_SUCCESS ? ERROR_INVALID_PARAMETER : error);
        return false;
    }
    m_legacyDllDirectory = std::move(canonical);
    m_legacyDllDirectorySpecified = true;
    SetWin32Error(win32Error, ERROR_SUCCESS);
    return true;
}

DWORD GuestModuleLoader::GetDllDirectory(
    DWORD bufferLength, LPWSTR buffer, DWORD* win32Error) const
{
    std::lock_guard<std::recursive_mutex> loaderGuard(m_loaderLock);
    const std::wstring value = m_legacyDllDirectorySpecified
        ? m_legacyDllDirectory : std::wstring{};
    const size_t required = value.size() + 1;
    if (!buffer || bufferLength < required)
    {
        SetWin32Error(win32Error, buffer || bufferLength ? ERROR_INSUFFICIENT_BUFFER : ERROR_SUCCESS);
        return static_cast<DWORD>((std::min)(required, static_cast<size_t>((std::numeric_limits<DWORD>::max)())));
    }
    memcpy(buffer, value.c_str(), required * sizeof(wchar_t));
    SetWin32Error(win32Error, ERROR_SUCCESS);
    return static_cast<DWORD>(value.size());
}

bool GuestModuleLoader::LoadLibrary(
    LPCWSTR requestedName,
    HMODULE* module,
    DWORD* win32Error,
    DWORD searchFlags,
    DWORD loadBehaviorFlags)
{
    std::lock_guard<std::recursive_mutex> loaderGuard(m_loaderLock);
    constexpr DWORD DontResolveDllReferences = 0x00000001u;
    constexpr DWORD LoadAsDataFile = 0x00000002u;
    constexpr DWORD LoadAsImageResource = 0x00000020u;
    constexpr DWORD LoadAsDataFileExclusive = 0x00000040u;
    constexpr DWORD ResourceOnlyFlags =
        LoadAsDataFile | LoadAsImageResource | LoadAsDataFileExclusive;
    constexpr DWORD SupportedBehaviorFlags = ResourceOnlyFlags | DontResolveDllReferences;
    if ((loadBehaviorFlags & ~SupportedBehaviorFlags) != 0)
    {
        SetWin32Error(win32Error, ERROR_INVALID_PARAMETER);
        return false;
    }
    const bool resourceOnly = (loadBehaviorFlags & ResourceOnlyFlags) != 0;
    const bool skipInitialization = resourceOnly ||
        (loadBehaviorFlags & DontResolveDllReferences) != 0;
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

    if (resourceOnly)
    {
        // Data-file mappings are not members of the executable loader list on
        // Windows. Keep a separate identity so a later normal LoadLibrary can
        // still map, bind and initialize the same physical image.
        for (auto& candidate : candidates) candidate.identity += L"|resource";
    }

    if (searchByBaseName && !resourceOnly)
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
            L"; Win32 error " + std::to_wstring(static_cast<unsigned long>(ERROR_MOD_NOT_FOUND)) +
            L". Searched: " + searched + L".");
        return false;
    }

    auto candidate = std::make_shared<Module>();
    candidate->identity = resolved->identity;
    candidate->canonicalName = resolved->displayName;
    candidate->resourceOnly = resourceOnly;
    const std::wstring& canonicalName = candidate->canonicalName;
    std::wstring preparationError;
    if (!PeImage::Inspect(fileBytes.data(), fileBytes.size(), &candidate->metadata))
    {
        preparationError = candidate->metadata.error;
        SetWin32Error(win32Error, ERROR_BAD_EXE_FORMAT);
        RuntimeDiagnostics::Record(
            L"DLL FAILED: invalid PE image for " + canonicalName + L" (" +
            preparationError + L"; machine " +
            std::to_wstring(static_cast<unsigned int>(candidate->metadata.machine)) +
            L"; Win32 error " +
            std::to_wstring(static_cast<unsigned long>(ERROR_BAD_EXE_FORMAT)) + L").");
        return false;
    }
    if (!PeMapper::Materialize(fileBytes.data(), fileBytes.size(), &candidate->mapped, &preparationError) ||
        !RuntimeImage::Reserve(candidate->mapped.bytes.size(), candidate->mapped.preferredImageBase,
            &candidate->runtime, &preparationError) ||
        (!resourceOnly && !PeMapper::ApplyBaseRelocations(
            &candidate->mapped,
            reinterpret_cast<ULONGLONG>(candidate->runtime.Base()),
            &preparationError)))
    {
        candidate->runtime.Release();
        SetWin32Error(win32Error, ERROR_BAD_EXE_FORMAT);
        RuntimeDiagnostics::Record(
            L"DLL FAILED: image preparation for " + canonicalName + L" (" + preparationError + L").");
        return false;
    }

    // Publish the reserved image before binding imports. Dependency graphs can
    // contain cycles; an import back to this module must resolve to this same
    // image rather than recursively mapping a duplicate.
    {
        std::lock_guard<std::mutex> guard(m_lock);
        const auto raced = FindModuleByIdentityLocked(candidate->identity);
        if (raced)
        {
            ++raced->references;
            *module = reinterpret_cast<HMODULE>(raced->runtime.Base());
            candidate->runtime.Release();
            SetWin32Error(win32Error, ERROR_SUCCESS);
            RuntimeDiagnostics::Record(
                L"DLL: concurrent load reused " + raced->canonicalName + L".");
            return true;
        }
        m_modules.push_back(candidate);
    }

    if (skipInitialization)
    {
        const bool copied = candidate->runtime.CopyFrom(candidate->mapped, &preparationError);
        const bool protectedImage = copied && (resourceOnly ||
            candidate->runtime.FinalizeProtections(candidate->mapped, &preparationError));
        if (!copied || !protectedImage)
        {
            {
                std::lock_guard<std::mutex> guard(m_lock);
                m_modules.erase(std::remove(
                    m_modules.begin(), m_modules.end(), candidate), m_modules.end());
            }
            candidate->runtime.Release();
            SetWin32Error(win32Error, ERROR_BAD_EXE_FORMAT);
            RuntimeDiagnostics::Record(
                L"DLL FAILED: non-initializing mapping failed for " + canonicalName +
                L" (" + preparationError + L").");
            return false;
        }
        {
            std::lock_guard<std::mutex> guard(m_lock);
            candidate->loading = false;
        }
        *module = reinterpret_cast<HMODULE>(candidate->runtime.Base());
        SetWin32Error(win32Error, ERROR_SUCCESS);
        RuntimeDiagnostics::Record(resourceOnly
            ? L"DLL RESOURCE: mapped without IAT/TLS/DllMain: " + canonicalName + L"."
            : L"DLL NORESOLVE: mapped without IAT/TLS/DllMain: " + canonicalName + L".");
        return true;
    }

    const ImportResolver dependencyResolver = [this, candidate, searchFlags](const ImportedSymbol& symbol)
    {
        ImportResolution resolution = m_resolver(symbol);
        if (resolution.targetAddress != 0) return resolution;

        const std::wstring dependencyName = ApiSetHostLibrary(symbol.library);
        const std::wstring dependencyKey = Lowercase(FileNameOf(dependencyName));
        HMODULE dependency = nullptr;
        DWORD error = ERROR_SUCCESS;

        // Snap a dependency once per importing module, not once per symbol.
        // A dependency that is already being mapped is a cycle edge: its base
        // is valid for IAT addresses, but taking another load-count reference
        // would create a permanent reference cycle.
        {
            std::lock_guard<std::mutex> guard(m_lock);
            const auto cached = candidate->dependencyModules.find(dependencyKey);
            if (cached != candidate->dependencyModules.end())
            {
                dependency = cached->second;
            }
            else
            {
                const auto existing = FindModuleByBaseNameLocked(dependencyKey);
                if (existing)
                {
                    dependency = reinterpret_cast<HMODULE>(existing->runtime.Base());
                    candidate->dependencyModules.emplace(dependencyKey, dependency);
                    if (!existing->loading && existing != candidate)
                    {
                        ++existing->references;
                        candidate->dependencyReferences.push_back(dependency);
                    }
                }
            }
        }
        if (!GetModuleHandle(dependencyName.c_str(), &dependency, &error))
        {
            if (!dependency && (searchFlags & LoadLibrarySearchDllLoadDir) != 0)
            {
                const std::wstring dependencyPath =
                    JoinPath(DirectoryOf(candidate->canonicalName), dependencyName);
                LoadLibrary(dependencyPath.c_str(), &dependency, &error);
            }
            const DWORD inheritedFlags = searchFlags & ~LoadLibrarySearchDllLoadDir;
            if (!dependency &&
                ((searchFlags & LoadLibrarySearchDllLoadDir) == 0 || inheritedFlags != 0))
            {
                LoadLibrary(dependencyName.c_str(), &dependency, &error, inheritedFlags);
            }
            if (!dependency) return resolution;

            std::lock_guard<std::mutex> guard(m_lock);
            const auto inserted = candidate->dependencyModules.emplace(
                dependencyKey, dependency);
            if (inserted.second)
                candidate->dependencyReferences.push_back(dependency);
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
            resolution.note = L"Resolved from guest dependency " + dependencyName + L".";
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
        ReleaseModuleDependencies(candidate);
        SetWin32Error(win32Error, ERROR_PROC_NOT_FOUND);
        RuntimeDiagnostics::Record(
            L"DLL FAILED: import binding for " + canonicalName + L" (" + preparationError +
            L"; " + std::to_wstring(candidate->bindings.unresolved) +
            L" unresolved imports; Win32 error " +
            std::to_wstring(static_cast<unsigned long>(ERROR_PROC_NOT_FOUND)) + L").");
        return false;
    }

    // ReactOS/NT associates an entry-point activation context with each
    // loader entry and activates it while TLS callbacks and DllMain run.  The
    // process-default context remains the fallback for DLLs without a
    // manifest; an embedded RT_MANIFEST gets its own independently ref-counted
    // context.
    std::vector<GuestResourceIdentifier> manifestNames;
    const HMODULE candidateHandle = reinterpret_cast<HMODULE>(candidate->runtime.Base());
    const GuestResourceStatus manifestStatus = EnumerateGuestResourceNames(
        candidateHandle, MAKEINTRESOURCEW(24), &manifestNames);
    if (manifestStatus == GuestResourceStatus::Success && !manifestNames.empty())
    {
        // ID 2 is the standard DLL manifest, ID 3 is its no-static-import
        // variant. ID 1 remains a fallback for nonconforming images.
        const GuestResourceIdentifier* manifestName =
            PreferredManifest(manifestNames, { 2, 3, 1 });
        const LPCWSTR resourceName = manifestName->ordinal
            ? MAKEINTRESOURCEW(manifestName->id) : manifestName->text.c_str();
        GuestActCtxW actctx{};
        actctx.cbSize = sizeof(actctx);
        actctx.dwFlags = ActCtxResourceNameValid | ActCtxModuleValid;
        actctx.lpSource = candidate->canonicalName.c_str();
        actctx.lpResourceName = resourceName;
        actctx.hModule = candidateHandle;
        candidate->activationContext = BridgeCreateActCtxW(&actctx);
        if (candidate->activationContext == INVALID_HANDLE_VALUE)
        {
            candidate->activationContext = nullptr;
            {
                std::lock_guard<std::mutex> guard(m_lock);
                m_modules.erase(std::remove(m_modules.begin(), m_modules.end(), candidate), m_modules.end());
            }
            candidate->runtime.Release();
            ReleaseModuleDependencies(candidate);
            SetWin32Error(win32Error, ERROR_SXS_CANT_GEN_ACTCTX);
            RuntimeDiagnostics::Record(
                L"DLL FAILED: activation context creation failed for " + canonicalName + L".");
            return false;
        }
        RuntimeDiagnostics::Record(
            L"DLL ACTCTX: associated embedded manifest with " + canonicalName + L".");
    }

    ScopedActivationContext activation(candidate->activationContext);
    if (!candidate->runtime.NotifyTls(DLL_PROCESS_ATTACH, &preparationError))
    {
        {
            std::lock_guard<std::mutex> guard(m_lock);
            m_modules.erase(std::remove(m_modules.begin(), m_modules.end(), candidate), m_modules.end());
        }
        candidate->runtime.Release();
        if (candidate->activationContext)
        {
            BridgeReleaseActCtx(candidate->activationContext);
            candidate->activationContext = nullptr;
        }
        ReleaseModuleDependencies(candidate);
        SetWin32Error(win32Error, ERROR_DLL_INIT_FAILED);
        RuntimeDiagnostics::Record(
            L"DLL FAILED: TLS process attach failed for " + canonicalName + L" (" +
            preparationError + L").");
        return false;
    }

    BOOL attachResult = TRUE;
    if (!InvokeDllEntry(candidate->runtime.Base(), candidate->mapped.entryPointRva,
            DLL_PROCESS_ATTACH, nullptr, &attachResult) || !attachResult)
    {
        std::wstring ignored;
        candidate->runtime.NotifyTls(DLL_PROCESS_DETACH, &ignored);
        {
            std::lock_guard<std::mutex> guard(m_lock);
            m_modules.erase(std::remove(m_modules.begin(), m_modules.end(), candidate), m_modules.end());
        }
        candidate->runtime.Release();
        if (candidate->activationContext)
        {
            BridgeReleaseActCtx(candidate->activationContext);
            candidate->activationContext = nullptr;
        }
        ReleaseModuleDependencies(candidate);
        SetWin32Error(win32Error, ERROR_DLL_INIT_FAILED);
        RuntimeDiagnostics::Record(L"DLL FAILED: " + canonicalName + L" rejected DLL_PROCESS_ATTACH.");
        return false;
    }
    {
        std::lock_guard<std::mutex> guard(m_lock);
        candidate->processAttached = true;
        candidate->loading = false;
        candidate->attachSequence = m_nextAttachSequence++;
    }

    *module = reinterpret_cast<HMODULE>(candidate->runtime.Base());
    SetWin32Error(win32Error, ERROR_SUCCESS);
    RuntimeDiagnostics::Record(
        L"DLL OK: " + canonicalName + L" loaded; " +
        std::to_wstring(candidate->bindings.bound) + L" imports bound, " +
        std::to_wstring(candidate->bindings.deferred) + L" delay imports deferred.");
    return true;
}

ImportResolution GuestModuleLoader::ResolveImport(
    const ImportedSymbol& symbol,
    DWORD searchFlags)
{
    std::lock_guard<std::recursive_mutex> loaderGuard(m_loaderLock);
    ImportResolution resolution = m_resolver(symbol);
    if (resolution.targetAddress != 0) return resolution;

    const std::wstring requestedLibrary = ApiSetHostLibrary(symbol.library);
    const std::wstring key = Lowercase(requestedLibrary);
    HMODULE dependency = nullptr;
    {
        std::lock_guard<std::mutex> guard(m_lock);
        const auto found = m_importModules.find(key);
        if (found != m_importModules.end()) dependency = found->second;
    }
    DWORD error = ERROR_SUCCESS;
    if (!dependency)
    {
        if (!LoadLibrary(requestedLibrary.c_str(), &dependency, &error, searchFlags))
            return resolution;
        std::lock_guard<std::mutex> guard(m_lock);
        m_importModules.emplace(key, dependency);
    }

    LPCSTR requested = nullptr;
    std::string asciiName;
    if (symbol.importedByOrdinal)
    {
        requested = reinterpret_cast<LPCSTR>(static_cast<ULONG_PTR>(symbol.ordinal));
    }
    else
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
        resolution.disposition = ImportDisposition::NeedsBridge;
        resolution.note = symbol.importedByOrdinal
            ? L"Resolved ordinal #" + std::to_wstring(symbol.ordinal) +
                L" from guest dependency " + requestedLibrary + L"."
            : L"Resolved from guest dependency " + requestedLibrary + L".";
    }
    return resolution;
}

bool GuestModuleLoader::GetModuleHandle(
    LPCWSTR requestedName,
    HMODULE* module,
    DWORD* win32Error) const
{
    std::lock_guard<std::recursive_mutex> loaderGuard(m_loaderLock);
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

bool GuestModuleLoader::GetModuleHandleFromAddress(
    ULONG_PTR address, HMODULE* module, DWORD* win32Error) const
{
    std::lock_guard<std::recursive_mutex> loaderGuard(m_loaderLock);
    if (!address || !module)
    {
        SetWin32Error(win32Error, ERROR_INVALID_PARAMETER);
        return false;
    }
    *module = nullptr;
    std::lock_guard<std::mutex> guard(m_lock);
    for (const auto& candidate : m_modules)
    {
        if (!candidate || candidate->resourceOnly || !candidate->runtime.Base() ||
            candidate->runtime.Size() == 0)
            continue;
        const ULONG_PTR base = reinterpret_cast<ULONG_PTR>(candidate->runtime.Base());
        if (address >= base && address - base < candidate->runtime.Size())
        {
            *module = reinterpret_cast<HMODULE>(candidate->runtime.Base());
            SetWin32Error(win32Error, ERROR_SUCCESS);
            return true;
        }
    }
    SetWin32Error(win32Error, ERROR_MOD_NOT_FOUND);
    return false;
}

bool GuestModuleLoader::AddReference(HMODULE module, bool pin, DWORD* win32Error)
{
    std::lock_guard<std::recursive_mutex> loaderGuard(m_loaderLock);
    std::lock_guard<std::mutex> guard(m_lock);
    const auto candidate = FindModuleLocked(module);
    if (!candidate || candidate->unloading)
    {
        SetWin32Error(win32Error, ERROR_MOD_NOT_FOUND);
        return false;
    }
    if (pin)
    {
        candidate->pinned = true;
    }
    else if (!candidate->pinned && candidate->references != (std::numeric_limits<ULONG>::max)())
    {
        ++candidate->references;
    }
    SetWin32Error(win32Error, ERROR_SUCCESS);
    return true;
}

bool GuestModuleLoader::GetModulePath(
    HMODULE module,
    std::wstring* path,
    DWORD* win32Error) const
{
    std::lock_guard<std::recursive_mutex> loaderGuard(m_loaderLock);
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
    std::lock_guard<std::recursive_mutex> loaderGuard(m_loaderLock);
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
    if (!candidate || candidate->resourceOnly)
    {
        SetWin32Error(win32Error,
            candidate ? ERROR_PROC_NOT_FOUND : ERROR_MOD_NOT_FOUND);
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
            : !symbol.name.empty() && symbol.name == requested;
        if (matches)
        {
            found = &symbol;
            break;
        }
    }
    if (found && found->rva != 0 && found->forwarder.empty() &&
        found->rva < candidate->runtime.Size())
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
                else
                {
                    SetWin32Error(win32Error, ERROR_PROC_NOT_FOUND);
                    return nullptr;
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
        if (candidate && !candidate->resourceOnly &&
            FileNameOf(candidate->canonicalName) == baseName) return candidate;
    }
    return nullptr;
}

bool GuestModuleLoader::GetMappedImage(HMODULE module, const BYTE** imageBase, size_t* imageSize) const
{
    std::lock_guard<std::recursive_mutex> loaderGuard(m_loaderLock);
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

bool GuestModuleLoader::DescribeAddress(ULONG_PTR address, std::wstring* description) const
{
    if (!description) return false;
    description->clear();

    // Diagnostics may run while unwinding an exception raised inside loader
    // code. Never block forever trying to describe that same failure.
    std::unique_lock<std::mutex> guard(m_lock, std::try_to_lock);
    if (!guard.owns_lock()) return false;
    for (const auto& candidate : m_modules)
    {
        if (!candidate || !candidate->runtime.Base() || candidate->runtime.Size() == 0)
            continue;

        const ULONG_PTR base = reinterpret_cast<ULONG_PTR>(candidate->runtime.Base());
        if (address < base || address - base >= candidate->runtime.Size())
            continue;

        wchar_t offset[32]{};
        swprintf_s(offset, L"0x%llX", static_cast<unsigned long long>(address - base));
        *description = candidate->canonicalName + L"+" + offset;
        return true;
    }
    return false;
}

bool GuestModuleLoader::FreeLibrary(HMODULE module, DWORD* win32Error)
{
    std::lock_guard<std::recursive_mutex> loaderGuard(m_loaderLock);
    std::shared_ptr<Module> candidate;
    {
        std::lock_guard<std::mutex> guard(m_lock);
        candidate = FindModuleLocked(module);
        if (!candidate)
        {
            SetWin32Error(win32Error, ERROR_MOD_NOT_FOUND);
            return false;
        }
        if (candidate->unloading)
        {
            // A recursive FreeLibrary from this DLL's detach path must not
            // execute callbacks twice.  The outer unload owns the transition.
            SetWin32Error(win32Error, ERROR_SUCCESS);
            return true;
        }
        if (candidate->pinned)
        {
            SetWin32Error(win32Error, ERROR_SUCCESS);
            return true;
        }
        if (candidate->references > 1)
        {
            --candidate->references;
            SetWin32Error(win32Error, ERROR_SUCCESS);
            return true;
        }
        candidate->unloading = true;
    }

    // Keep the loader entry discoverable until all detach notifications have
    // returned, matching LdrUnloadDll/ReactOS.  DllMain is allowed to query its
    // own HMODULE and exported addresses during teardown.
    std::wstring ignored;
    if (candidate->processAttached)
    {
        ScopedActivationContext activation(candidate->activationContext);
        candidate->runtime.NotifyTls(DLL_PROCESS_DETACH, &ignored);
        BOOL unused = TRUE;
        InvokeDllEntry(candidate->runtime.Base(), candidate->mapped.entryPointRva,
            DLL_PROCESS_DETACH, nullptr, &unused);
        candidate->processAttached = false;
    }
    {
        std::lock_guard<std::mutex> guard(m_lock);
        m_modules.erase(std::remove(m_modules.begin(), m_modules.end(), candidate), m_modules.end());
        for (auto iterator = m_importModules.begin(); iterator != m_importModules.end();)
        {
            if (iterator->second == module) iterator = m_importModules.erase(iterator);
            else ++iterator;
        }
    }
    RuntimeDiagnostics::Record(L"DLL: unloaded " + candidate->canonicalName + L".");
    candidate->runtime.Release();
    if (candidate->activationContext)
    {
        BridgeReleaseActCtx(candidate->activationContext);
        candidate->activationContext = nullptr;
    }
    ReleaseModuleDependencies(candidate);
    SetWin32Error(win32Error, ERROR_SUCCESS);
    return true;
}

void GuestModuleLoader::ReleaseModuleDependencies(
    const std::shared_ptr<Module>& module)
{
    if (!module) return;
    std::vector<HMODULE> dependencies;
    {
        std::lock_guard<std::mutex> guard(m_lock);
        dependencies.swap(module->dependencyReferences);
        module->dependencyModules.clear();
    }
    for (auto iterator = dependencies.rbegin(); iterator != dependencies.rend(); ++iterator)
    {
        DWORD ignored = ERROR_SUCCESS;
        FreeLibrary(*iterator, &ignored);
    }
}

bool GuestModuleLoader::NotifyModule(
    const std::shared_ptr<Module>& module,
    DWORD reason,
    std::wstring* error) const
{
    if (!module || !module->processAttached) return true;
    if ((reason == DLL_THREAD_ATTACH || reason == DLL_THREAD_DETACH) &&
        !module->threadNotifications) return true;
    ScopedActivationContext activation(module->activationContext);
    if (!module->runtime.NotifyTls(reason, error)) return false;
    BOOL entryResult = TRUE;
    if (!InvokeDllEntry(module->runtime.Base(), module->mapped.entryPointRva,
            reason, nullptr, &entryResult))
    {
        if (error) *error = L"A DLL entry point raised a structured exception.";
        return false;
    }
    // DllMain's return value is meaningful only for DLL_PROCESS_ATTACH.
    return reason != DLL_PROCESS_ATTACH || entryResult != FALSE;
}

bool GuestModuleLoader::DisableThreadNotifications(HMODULE module, DWORD* win32Error)
{
    std::lock_guard<std::recursive_mutex> loaderGuard(m_loaderLock);
    std::lock_guard<std::mutex> guard(m_lock);
    const auto candidate = FindModuleLocked(module);
    if (!candidate || candidate->resourceOnly)
    {
        SetWin32Error(win32Error, ERROR_MOD_NOT_FOUND);
        return false;
    }
    if (candidate->runtime.HasStaticTls())
    {
        SetWin32Error(win32Error, ERROR_INVALID_PARAMETER);
        return false;
    }
    candidate->threadNotifications = false;
    SetWin32Error(win32Error, ERROR_SUCCESS);
    return true;
}

bool GuestModuleLoader::NotifyThread(DWORD reason, std::wstring* error)
{
    std::lock_guard<std::recursive_mutex> loaderGuard(m_loaderLock);
    std::vector<std::shared_ptr<Module>> modules;
    {
        std::lock_guard<std::mutex> guard(m_lock);
        for (const auto& module : m_modules)
        {
            if (module && module->processAttached && module->threadNotifications)
                modules.push_back(module);
        }
    }
    std::sort(modules.begin(), modules.end(),
        [](const std::shared_ptr<Module>& left,
            const std::shared_ptr<Module>& right)
        { return left->attachSequence < right->attachSequence; });
    if (reason == DLL_THREAD_DETACH)
        std::reverse(modules.begin(), modules.end());
    size_t notified = 0;
    for (const auto& module : modules)
    {
        if (!NotifyModule(module, reason, error))
        {
            if (reason == DLL_THREAD_ATTACH)
            {
                while (notified != 0)
                {
                    --notified;
                    std::wstring ignored;
                    NotifyModule(modules[notified], DLL_THREAD_DETACH, &ignored);
                }
            }
            return false;
        }
        ++notified;
    }
    return true;
}

void GuestModuleLoader::ReleaseAll()
{
    std::lock_guard<std::recursive_mutex> loaderGuard(m_loaderLock);
    std::vector<std::shared_ptr<Module>> modules;
    {
        std::lock_guard<std::mutex> guard(m_lock);
        modules.swap(m_modules);
        m_importModules.clear();
        m_nextAttachSequence = 1;
    }
    std::sort(modules.begin(), modules.end(),
        [](const std::shared_ptr<Module>& left,
            const std::shared_ptr<Module>& right)
        {
            const std::uint64_t leftSequence = left ? left->attachSequence : 0;
            const std::uint64_t rightSequence = right ? right->attachSequence : 0;
            return leftSequence > rightSequence;
        });
    for (const auto& module : modules)
    {
        if (module)
        {
            if (module->processAttached)
            {
                ScopedActivationContext activation(module->activationContext);
                std::wstring ignored;
                module->runtime.NotifyTls(DLL_PROCESS_DETACH, &ignored);
                BOOL unused = TRUE;
                InvokeDllEntry(module->runtime.Base(), module->mapped.entryPointRva,
                    DLL_PROCESS_DETACH, reinterpret_cast<LPVOID>(1), &unused);
                module->processAttached = false;
            }
            module->runtime.Release();
            if (module->activationContext)
            {
                BridgeReleaseActCtx(module->activationContext);
                module->activationContext = nullptr;
            }
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
