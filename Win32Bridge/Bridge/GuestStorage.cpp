#include "pch.h"
#include "Bridge\\GuestStorage.h"
#include "Bridge/RuntimeDiagnostics.h"

#include <windows.storage.fileproperties.h>

#include <algorithm>
#include <cwctype>
#include <vector>

using namespace Win32Bridge::Bridge;

using namespace Platform;
using namespace Windows::Storage;
using namespace Windows::Storage::FileProperties;
using namespace Windows::Storage::Streams;
using namespace concurrency;

namespace
{
    constexpr DWORD MaxSynchronousIo = 16 * 1024 * 1024;
    constexpr size_t MaxGuestPathCharacters = 32767;
    constexpr DWORD VirtualMutableFileAttributes =
        FILE_ATTRIBUTE_READONLY |
        FILE_ATTRIBUTE_HIDDEN |
        FILE_ATTRIBUTE_SYSTEM |
        FILE_ATTRIBUTE_DIRECTORY |
        FILE_ATTRIBUTE_ARCHIVE |
        FILE_ATTRIBUTE_NORMAL |
        FILE_ATTRIBUTE_TEMPORARY |
        FILE_ATTRIBUTE_OFFLINE |
        FILE_ATTRIBUTE_NOT_CONTENT_INDEXED;
    thread_local GuestStorageContext* g_currentGuestStorage = nullptr;
    thread_local unsigned g_storageEnumerationDiagnostics = 0;

    void SetError(std::wstring* error, const std::wstring& message)
    {
        if (error)
        {
            *error = message;
        }
    }

    void SetWin32Error(DWORD* output, DWORD value)
    {
        if (output)
        {
            *output = value;
        }
    }

    DWORD ErrorFromException(Exception^ exception)
    {
        if (!exception)
        {
            return ERROR_GEN_FAILURE;
        }

        const HRESULT code = exception->HResult;
        if (HRESULT_FACILITY(code) == FACILITY_WIN32)
        {
            return HRESULT_CODE(code);
        }
        if (code == E_ACCESSDENIED)
        {
            return ERROR_ACCESS_DENIED;
        }
        if (code == E_INVALIDARG)
        {
            return ERROR_INVALID_PARAMETER;
        }
        return ERROR_GEN_FAILURE;
    }

    void RecordStorageException(const wchar_t* operation, Exception^ exception)
    {
        RuntimeDiagnostics::Record(
            std::wstring(L"STORAGE EXCEPTION: ") + (operation ? operation : L"unknown") +
            L"; HRESULT " + std::to_wstring(static_cast<unsigned long>(
                exception ? exception->HResult : E_FAIL)) + L".");
    }

    void MoveStorageFolderTree(StorageFolder^ source, StorageFolder^ destinationParent,
        const std::wstring& destinationName, bool replaceExisting, unsigned depth)
    {
        if (!source || !destinationParent || destinationName.empty() || depth > 64)
            throw ref new InvalidArgumentException();
        StorageFolder^ destination = create_task(destinationParent->CreateFolderAsync(
            ref new String(destinationName.c_str()), replaceExisting
                ? CreationCollisionOption::ReplaceExisting
                : CreationCollisionOption::FailIfExists)).get();
        auto children = create_task(source->GetItemsAsync()).get();
        for (unsigned index = 0; index < children->Size; ++index)
        {
            IStorageItem^ child = children->GetAt(index);
            const std::wstring childName(child->Name->Data());
            if (child->IsOfType(StorageItemTypes::File))
            {
                StorageFile^ file = safe_cast<StorageFile^>(child);
                create_task(file->MoveAsync(destination, child->Name,
                    replaceExisting ? NameCollisionOption::ReplaceExisting
                        : NameCollisionOption::FailIfExists)).get();
            }
            else if (child->IsOfType(StorageItemTypes::Folder))
            {
                MoveStorageFolderTree(safe_cast<StorageFolder^>(child), destination,
                    childName, replaceExisting, depth + 1);
            }
        }
        create_task(source->DeleteAsync()).get();
    }

    std::vector<std::wstring> PhysicalComponents(const GuestPath& path)
    {
        std::vector<std::wstring> result;
        result.reserve(path.components.size() + 1);
        result.push_back(L"drive_c");
        result.insert(result.end(), path.components.begin(), path.components.end());
        return result;
    }

    std::wstring MetadataKey(const std::wstring& canonicalPath)
    {
        std::wstring key = canonicalPath;
        std::transform(key.begin(), key.end(), key.begin(), [](wchar_t value)
        {
            return static_cast<wchar_t>(std::towlower(value));
        });
        while (key.size() > 3 && key.back() == L'\\') key.pop_back();
        return key;
    }

    std::wstring ChildCanonicalPath(
        const std::wstring& parent,
        const std::wstring& child)
    {
        if (parent.empty()) return child;
        return parent.back() == L'\\' ? parent + child : parent + L"\\" + child;
    }

    bool HasReadAccess(DWORD access)
    {
        return (access & (GENERIC_READ | FILE_READ_DATA)) != 0;
    }

    bool HasWriteAccess(DWORD access)
    {
        return (access & (GENERIC_WRITE | FILE_WRITE_DATA | FILE_APPEND_DATA)) != 0;
    }

    bool IsValidSearchPattern(const std::wstring& pattern)
    {
        if (pattern.empty() || pattern == L"." || pattern == L"..")
        {
            return false;
        }

        for (const wchar_t character : pattern)
        {
            if (character < 0x20 || character == L'<' || character == L'>' ||
                character == L'"' || character == L'|' || character == L':' ||
                character == L'\\' || character == L'/')
            {
                return false;
            }
        }
        return true;
    }

    bool SplitSearchPattern(
        LPCWSTR searchPattern,
        std::wstring* directory,
        std::wstring* pattern,
        DWORD* win32Error)
    {
        if (!searchPattern || !directory || !pattern)
        {
            SetWin32Error(win32Error, ERROR_INVALID_PARAMETER);
            return false;
        }

        const size_t length = wcsnlen_s(searchPattern, MaxGuestPathCharacters + 1);
        if (length == 0 || length > MaxGuestPathCharacters)
        {
            SetWin32Error(win32Error, ERROR_INVALID_NAME);
            return false;
        }

        std::wstring value(searchPattern, length);
        for (wchar_t& character : value)
        {
            if (character == L'/')
            {
                character = L'\\';
            }
        }

        // Some desktop file managers probe the Win32 device-root spelling
        // (\\.\\) while constructing their drive view. The bridge has no
        // device namespace to expose; its only valid root is the guest C:
        // drive. Treat this probe as an enumeration of that root instead of
        // leaking ERROR_INVALID_NAME into normal shell navigation.
        if (value == L"\\\\.\\" || value == L"\\\\?\\")
        {
            *directory = L"C:\\";
            *pattern = L"*";
            return true;
        }

        const size_t separator = value.find_last_of(L'\\');
        if (separator == std::wstring::npos)
        {
            if (value.size() >= 2 && std::iswalpha(value[0]) && value[1] == L':')
            {
                *directory = value.substr(0, 2);
                *pattern = value.substr(2);
            }
            else
            {
                *directory = L".";
                *pattern = value;
            }
        }
        else
        {
            *directory = value.substr(0, separator + 1);
            *pattern = value.substr(separator + 1);
        }

        if (!IsValidSearchPattern(*pattern))
        {
            SetWin32Error(win32Error, ERROR_INVALID_NAME);
            return false;
        }
        return true;
    }

    bool WildcardMatch(const std::wstring& pattern, const std::wstring& name)
    {
        // Win32 treats *.* as an all-files wildcard, including names without a dot.
        if (_wcsicmp(pattern.c_str(), L"*.*") == 0)
        {
            return true;
        }

        size_t patternIndex = 0;
        size_t nameIndex = 0;
        size_t lastStar = std::wstring::npos;
        size_t starMatch = 0;
        while (nameIndex < name.size())
        {
            if (patternIndex < pattern.size() &&
                (pattern[patternIndex] == L'?' ||
                 std::towlower(pattern[patternIndex]) == std::towlower(name[nameIndex])))
            {
                ++patternIndex;
                ++nameIndex;
            }
            else if (patternIndex < pattern.size() && pattern[patternIndex] == L'*')
            {
                lastStar = patternIndex++;
                starMatch = nameIndex;
            }
            else if (lastStar != std::wstring::npos)
            {
                patternIndex = lastStar + 1;
                nameIndex = ++starMatch;
            }
            else
            {
                return false;
            }
        }

        while (patternIndex < pattern.size() && pattern[patternIndex] == L'*')
        {
            ++patternIndex;
        }
        return patternIndex == pattern.size();
    }

    FILETIME ToFileTime(Windows::Foundation::DateTime value)
    {
        ULARGE_INTEGER raw = {};
        raw.QuadPart = static_cast<ULONGLONG>(value.UniversalTime);
        FILETIME result = {};
        result.dwLowDateTime = raw.LowPart;
        result.dwHighDateTime = raw.HighPart;
        return result;
    }

    DWORD MapFileAttributes(IStorageItem^ item, bool isDirectory)
    {
        const DWORD source = static_cast<DWORD>(item->Attributes);
        DWORD result = 0;
        if ((source & static_cast<DWORD>(FileAttributes::ReadOnly)) != 0)
        {
            result |= FILE_ATTRIBUTE_READONLY;
        }
        if ((source & static_cast<DWORD>(FileAttributes::Archive)) != 0)
        {
            result |= FILE_ATTRIBUTE_ARCHIVE;
        }
        if ((source & static_cast<DWORD>(FileAttributes::Temporary)) != 0)
        {
            result |= FILE_ATTRIBUTE_TEMPORARY;
        }
        if (isDirectory)
        {
            result |= FILE_ATTRIBUTE_DIRECTORY;
        }
        return result == 0 ? FILE_ATTRIBUTE_NORMAL : result;
    }

    bool FillFindData(IStorageItem^ item, WIN32_FIND_DATAW* findData, DWORD* win32Error)
    {
        if (!item || !findData)
        {
            SetWin32Error(win32Error, ERROR_INVALID_PARAMETER);
            return false;
        }

        const bool isDirectory = item->IsOfType(StorageItemTypes::Folder);
        const std::wstring name(item->Name->Data());
        if (name.empty() || name.size() >= ARRAYSIZE(findData->cFileName))
        {
            SetWin32Error(win32Error, ERROR_FILENAME_EXCED_RANGE);
            return false;
        }

        try
        {
            auto properties = create_task(item->GetBasicPropertiesAsync()).get();
            ZeroMemory(findData, sizeof(*findData));
            findData->dwFileAttributes = MapFileAttributes(item, isDirectory);
            findData->ftCreationTime = ToFileTime(item->DateCreated);
            findData->ftLastWriteTime = ToFileTime(properties->DateModified);
            // UWP exposes no separate access timestamp. Use the modified time,
            // which is the least surprising value for old Win32 callers.
            findData->ftLastAccessTime = findData->ftLastWriteTime;
            if (!isDirectory)
            {
                const ULONGLONG size = properties->Size;
                findData->nFileSizeLow = static_cast<DWORD>(size);
                findData->nFileSizeHigh = static_cast<DWORD>(size >> 32);
            }
            wcsncpy_s(findData->cFileName, name.c_str(), _TRUNCATE);
            SetWin32Error(win32Error, ERROR_SUCCESS);
            return true;
        }
        catch (Exception^ exception)
        {
            RecordStorageException(L"GetBasicPropertiesAsync during directory enumeration", exception);
            SetWin32Error(win32Error, ErrorFromException(exception));
            return false;
        }
    }
}

struct GuestStorageContext::FileRecord final
{
    FileRecord(IRandomAccessStream^ value, bool canRead, bool canWrite,
        std::wstring path, DWORD access, DWORD sharing, bool removeOnClose)
        : stream(value), readable(canRead), writable(canWrite), canonicalPath(std::move(path)),
          desiredAccess(access), shareMode(sharing), deleteOnClose(removeOnClose)
    {
    }

    Platform::Agile<IRandomAccessStream^> stream;
    bool readable;
    bool writable;
    std::wstring canonicalPath;
    DWORD desiredAccess = 0;
    DWORD shareMode = 0;
    bool deleteOnClose = false;
    // Protected by lock.  Closing first removes the handle from the shared
    // table, then marks this record closed before releasing its stream.  An
    // I/O operation that already retained the record therefore cannot race a
    // subsequent stream close.
    bool closed = false;
    std::mutex lock;
};

struct GuestStorageContext::FindRecord final
{
    explicit FindRecord(std::vector<WIN32_FIND_DATAW> values)
        : entries(std::move(values))
    {
    }

    std::vector<WIN32_FIND_DATAW> entries;
    size_t next = 1;
    std::mutex lock;
};

GuestStorageContext::GuestStorageContext(StorageFolder^ localFolder, const std::wstring& modulePath,
    StorageFolder^ moduleSourceFolder)
    : m_localFolder(localFolder), m_moduleSourceFolder(moduleSourceFolder)
{
    GuestPath module;
    std::wstring ignored;
    if (m_paths.Resolve(modulePath.c_str(), &module, &ignored))
    {
        m_modulePath = module.canonical;
        const size_t lastSeparator = m_modulePath.find_last_of(L'\\');
        GuestPath current;
        if (lastSeparator != std::wstring::npos &&
            m_paths.Resolve(m_modulePath.substr(0, lastSeparator).c_str(), &current, &ignored))
        {
            m_moduleDirectory = current.canonical;
            m_moduleDirectoryComponentCount = current.components.size();
            m_paths.SetCurrentDirectoryPath(current, &ignored);
        }
    }
    else
    {
        m_modulePath = L"C:\\Program Files\\Win32Bridge\\Guest.exe";
    }

    // Preserve the staging-time default.  The resolver itself is reused
    // across runs, so its mutable current directory must not become guest
    // process state that leaks into the next invocation.
    m_initialCurrentDirectory = m_paths.CurrentDirectory();
}

GuestStorageContext::~GuestStorageContext()
{
    CloseAll();
}

bool GuestStorageContext::Resolve(LPCWSTR path, GuestPath* resolved, DWORD* win32Error) const
{
    std::wstring error;
    if (!m_paths.Resolve(path, resolved, &error))
    {
        SetWin32Error(win32Error, ERROR_INVALID_NAME);
        return false;
    }
    return true;
}

bool GuestStorageContext::CanonicalPath(
    LPCWSTR path,
    std::wstring* canonical,
    DWORD* win32Error) const
{
    if (!canonical)
    {
        SetWin32Error(win32Error, ERROR_INVALID_PARAMETER);
        return false;
    }

    canonical->clear();
    GuestPath resolved;
    if (!Resolve(path, &resolved, win32Error))
    {
        return false;
    }
    *canonical = std::move(resolved.canonical);
    SetWin32Error(win32Error, ERROR_SUCCESS);
    return true;
}

bool GuestStorageContext::GetFolder(
    const std::vector<std::wstring>& physicalComponents,
    bool createMissing,
    StorageFolder^* folder,
    DWORD* win32Error) const
{
    if (!folder)
    {
        SetWin32Error(win32Error, ERROR_INVALID_PARAMETER);
        return false;
    }

    try
    {
        StorageFolder^ current = m_localFolder.Get();
        if (!current)
        {
            SetWin32Error(win32Error, ERROR_PATH_NOT_FOUND);
            return false;
        }

        for (const auto& component : physicalComponents)
        {
            const auto name = ref new String(component.c_str());
            current = createMissing
                ? create_task(current->CreateFolderAsync(name, CreationCollisionOption::OpenIfExists)).get()
                : create_task(current->GetFolderAsync(name)).get();
        }
        *folder = current;
        return true;
    }
    catch (Exception^ exception)
    {
        RecordStorageException(createMissing ? L"CreateFolderAsync" : L"GetFolderAsync", exception);
        SetWin32Error(win32Error, ErrorFromException(exception));
        return false;
    }
}

bool GuestStorageContext::GetParentFolder(
    const GuestPath& path,
    StorageFolder^* parent,
    std::wstring* leafName,
    DWORD* win32Error) const
{
    if (path.components.empty() || !parent || !leafName)
    {
        SetWin32Error(win32Error, ERROR_INVALID_NAME);
        return false;
    }

    std::vector<std::wstring> physical = PhysicalComponents(path);
    *leafName = physical.back();
    physical.pop_back();
    return GetFolder(physical, false, parent, win32Error);
}

bool GuestStorageContext::GetDirectoryFolder(const GuestPath& path, StorageFolder^* folder, DWORD* win32Error) const
{
    return GetFolder(PhysicalComponents(path), false, folder, win32Error);
}

bool GuestStorageContext::GetAuthorizedFolder(const GuestPath& path, StorageFolder^* folder) const
{
    if (!folder) return false;
    *folder = nullptr;
    StorageFolder^ current = m_moduleSourceFolder.Get();
    const bool sameDirectory = !m_moduleDirectory.empty() &&
        _wcsicmp(path.canonical.c_str(), m_moduleDirectory.c_str()) == 0;
    const bool childDirectory = !m_moduleDirectory.empty() &&
        path.canonical.size() > m_moduleDirectory.size() &&
        path.canonical[m_moduleDirectory.size()] == L'\\' &&
        _wcsnicmp(path.canonical.c_str(), m_moduleDirectory.c_str(),
            m_moduleDirectory.size()) == 0;
    if (!current || (!sameDirectory && !childDirectory) ||
        path.components.size() < m_moduleDirectoryComponentCount)
        return false;
    try
    {
        for (size_t index = m_moduleDirectoryComponentCount; index < path.components.size(); ++index)
            current = create_task(current->GetFolderAsync(
                ref new String(path.components[index].c_str()))).get();
        *folder = current;
        return true;
    }
    catch (...) { return false; }
}

bool GuestStorageContext::OpenAuthorizedFile(const GuestPath& path, DWORD desiredAccess,
    DWORD shareMode, HANDLE* guestHandle, DWORD* win32Error)
{
    if (HasWriteAccess(desiredAccess) || path.components.size() <= m_moduleDirectoryComponentCount)
        return false;
    GuestPath parentPath = path;
    const std::wstring leaf = parentPath.components.back();
    parentPath.components.pop_back();
    parentPath.canonical = parentPath.canonical.substr(0, parentPath.canonical.find_last_of(L'\\'));
    StorageFolder^ parent = nullptr;
    if (!GetAuthorizedFolder(parentPath, &parent)) return false;
    try
    {
        StorageFile^ file = create_task(parent->GetFileAsync(ref new String(leaf.c_str()))).get();
        IRandomAccessStream^ stream = create_task(file->OpenAsync(FileAccessMode::Read)).get();
        return AddFile(stream, true, false, path.canonical, desiredAccess, shareMode,
            false, guestHandle, win32Error);
    }
    catch (...) { return false; }
}

bool GuestStorageContext::EnsureLayout(std::wstring* error)
{
    const std::vector<std::vector<std::wstring>> requiredDirectories =
    {
        { L"drive_c" },
        { L"drive_c", L"Program Files" },
        { L"drive_c", L"Program Files", L"Win32Bridge" },
        { L"drive_c", L"Users" },
        { L"drive_c", L"Users", L"Default" },
        { L"drive_c", L"Users", L"Default", L"Documents" },
        { L"drive_c", L"Users", L"Default", L"AppData" },
        { L"drive_c", L"Users", L"Default", L"AppData", L"Local" },
        { L"drive_c", L"Users", L"Default", L"AppData", L"Roaming" },
        { L"drive_c", L"Users", L"Default", L"AppData", L"Local", L"Temp" },
        { L"drive_c", L"Windows" },
        { L"drive_c", L"Windows", L"System32" },
        { L"drive_c", L"Windows", L"System32", L"config" }
    };

    DWORD win32Error = ERROR_SUCCESS;
    for (const auto& directory : requiredDirectories)
    {
        StorageFolder^ ignored = nullptr;
        if (!GetFolder(directory, true, &ignored, &win32Error))
        {
            SetError(error, L"Could not initialize the virtual C: drive (" + std::to_wstring(win32Error) + L").");
            return false;
        }
    }
    return true;
}

bool GuestStorageContext::AddFile(
    IRandomAccessStream^ stream,
    bool readable,
    bool writable,
    const std::wstring& canonicalPath,
    DWORD desiredAccess,
    DWORD shareMode,
    bool deleteOnClose,
    HANDLE* guestHandle,
    DWORD* win32Error)
{
    if (!stream || !guestHandle)
    {
        SetWin32Error(win32Error, ERROR_INVALID_PARAMETER);
        return false;
    }

    auto record = std::make_shared<FileRecord>(stream, readable, writable,
        canonicalPath, desiredAccess, shareMode, deleteOnClose);
    std::lock_guard<std::mutex> guard(m_handlesLock);
    ULONG_PTR token = 0;
    if (!AllocateHandleLocked(&token))
    {
        SetWin32Error(win32Error, ERROR_TOO_MANY_OPEN_FILES);
        return false;
    }

    m_files.emplace(token, record);
    *guestHandle = reinterpret_cast<HANDLE>(token);
    SetWin32Error(win32Error, ERROR_SUCCESS);
    return true;
}

bool GuestStorageContext::AddFind(
    const std::shared_ptr<FindRecord>& record,
    HANDLE* guestHandle,
    DWORD* win32Error)
{
    if (!record || !guestHandle)
    {
        SetWin32Error(win32Error, ERROR_INVALID_PARAMETER);
        return false;
    }

    std::lock_guard<std::mutex> guard(m_handlesLock);
    ULONG_PTR token = 0;
    if (!AllocateHandleLocked(&token))
    {
        SetWin32Error(win32Error, ERROR_TOO_MANY_OPEN_FILES);
        return false;
    }

    m_finds.emplace(token, record);
    *guestHandle = reinterpret_cast<HANDLE>(token);
    SetWin32Error(win32Error, ERROR_SUCCESS);
    return true;
}

bool GuestStorageContext::AllocateHandleLocked(ULONG_PTR* token)
{
    if (!token || m_files.size() + m_finds.size() >= MaximumHandles)
    {
        return false;
    }

    // With the bounded table we will find a free token within this many
    // probes.  The range check also makes a reused context recover cleanly if
    // a previous run advanced the cursor to the synchronization namespace.
    for (size_t attempt = 0; attempt <= MaximumHandles; ++attempt)
    {
        if (m_nextHandle < FirstHandleToken || m_nextHandle >= FirstReservedHandleToken)
        {
            m_nextHandle = FirstHandleToken;
        }

        const ULONG_PTR candidate = m_nextHandle++;
        if (m_nextHandle >= FirstReservedHandleToken)
        {
            m_nextHandle = FirstHandleToken;
        }

        if (m_files.find(candidate) != m_files.end() ||
            m_finds.find(candidate) != m_finds.end())
        {
            continue;
        }

        *token = candidate;
        return true;
    }

    return false;
}

std::shared_ptr<GuestStorageContext::FileRecord> GuestStorageContext::LookupFile(HANDLE guestHandle) const
{
    const ULONG_PTR token = reinterpret_cast<ULONG_PTR>(guestHandle);
    if (token < FirstHandleToken || token >= FirstReservedHandleToken)
    {
        return nullptr;
    }

    std::lock_guard<std::mutex> guard(m_handlesLock);
    const auto found = m_files.find(token);
    return found == m_files.end() ? nullptr : found->second;
}

std::shared_ptr<GuestStorageContext::FindRecord> GuestStorageContext::LookupFind(HANDLE guestHandle) const
{
    const ULONG_PTR token = reinterpret_cast<ULONG_PTR>(guestHandle);
    if (token < FirstHandleToken || token >= FirstReservedHandleToken)
    {
        return nullptr;
    }

    std::lock_guard<std::mutex> guard(m_handlesLock);
    const auto found = m_finds.find(token);
    return found == m_finds.end() ? nullptr : found->second;
}

bool GuestStorageContext::GetFilePath(
    HANDLE guestHandle, std::wstring* path, DWORD* win32Error) const
{
    if (!path)
    {
        SetWin32Error(win32Error, ERROR_INVALID_PARAMETER);
        return false;
    }
    const auto record = LookupFile(guestHandle);
    if (!record)
    {
        SetWin32Error(win32Error, ERROR_INVALID_HANDLE);
        return false;
    }
    std::lock_guard<std::mutex> guard(record->lock);
    if (record->closed)
    {
        SetWin32Error(win32Error, ERROR_INVALID_HANDLE);
        return false;
    }
    *path = record->canonicalPath;
    SetWin32Error(win32Error, ERROR_SUCCESS);
    return true;
}

DWORD GuestStorageContext::ApplyAttributeOverride(
    const std::wstring& canonicalPath,
    DWORD attributes) const
{
    const std::wstring key = MetadataKey(canonicalPath);
    std::lock_guard<std::mutex> guard(m_metadataLock);
    const auto found = m_attributeOverrides.find(key);
    if (found == m_attributeOverrides.end()) return attributes;

    // DIRECTORY describes the actual storage item and cannot be changed by
    // SetFileAttributes. NORMAL is represented by the absence of all other
    // mutable bits, as on Win32.
    DWORD result = found->second & ~(FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_NORMAL);
    result |= attributes & FILE_ATTRIBUTE_DIRECTORY;
    return result == 0 ? FILE_ATTRIBUTE_NORMAL : result;
}

void GuestStorageContext::StoreAttributeOverride(
    const std::wstring& canonicalPath,
    DWORD attributes)
{
    std::lock_guard<std::mutex> guard(m_metadataLock);
    m_attributeOverrides[MetadataKey(canonicalPath)] = attributes;
}

void GuestStorageContext::RemoveAttributeOverrides(
    const std::wstring& canonicalPath,
    bool includeChildren)
{
    const std::wstring key = MetadataKey(canonicalPath);
    const std::wstring prefix = key.empty() || key.back() == L'\\'
        ? key
        : key + L"\\";
    std::lock_guard<std::mutex> guard(m_metadataLock);
    for (auto iterator = m_attributeOverrides.begin(); iterator != m_attributeOverrides.end();)
    {
        const bool child = includeChildren && iterator->first.size() > prefix.size() &&
            iterator->first.compare(0, prefix.size(), prefix) == 0;
        if (iterator->first == key || child) iterator = m_attributeOverrides.erase(iterator);
        else ++iterator;
    }
}

void GuestStorageContext::MoveAttributeOverrides(
    const std::wstring& sourceCanonicalPath,
    const std::wstring& destinationCanonicalPath)
{
    const std::wstring source = MetadataKey(sourceCanonicalPath);
    const std::wstring destination = MetadataKey(destinationCanonicalPath);
    if (source == destination) return;
    const std::wstring prefix = source.empty() || source.back() == L'\\'
        ? source
        : source + L"\\";
    const std::wstring destinationPrefix = destination.empty() || destination.back() == L'\\'
        ? destination
        : destination + L"\\";
    std::vector<std::pair<std::wstring, DWORD>> moved;
    std::lock_guard<std::mutex> guard(m_metadataLock);
    for (auto iterator = m_attributeOverrides.begin(); iterator != m_attributeOverrides.end();)
    {
        if (iterator->first == destination ||
            (iterator->first.size() > destinationPrefix.size() &&
             iterator->first.compare(0, destinationPrefix.size(), destinationPrefix) == 0))
        {
            iterator = m_attributeOverrides.erase(iterator);
        }
        else
        {
            ++iterator;
        }
    }
    for (auto iterator = m_attributeOverrides.begin(); iterator != m_attributeOverrides.end();)
    {
        if (iterator->first == source)
        {
            moved.emplace_back(destination, iterator->second);
            iterator = m_attributeOverrides.erase(iterator);
        }
        else if (iterator->first.size() > prefix.size() &&
            iterator->first.compare(0, prefix.size(), prefix) == 0)
        {
            moved.emplace_back(destination + L"\\" + iterator->first.substr(prefix.size()),
                iterator->second);
            iterator = m_attributeOverrides.erase(iterator);
        }
        else
        {
            ++iterator;
        }
    }
    for (const auto& entry : moved) m_attributeOverrides[entry.first] = entry.second;
}

bool GuestStorageContext::CreateFile(
    LPCWSTR fileName,
    DWORD desiredAccess,
    DWORD shareMode,
    DWORD creationDisposition,
    DWORD flagsAndAttributes,
    HANDLE,
    HANDLE* guestHandle,
    DWORD* win32Error)
{
    if (!guestHandle)
    {
        SetWin32Error(win32Error, ERROR_INVALID_PARAMETER);
        return false;
    }
    *guestHandle = INVALID_HANDLE_VALUE;

    if ((flagsAndAttributes & FILE_FLAG_OVERLAPPED) != 0)
    {
        SetWin32Error(win32Error, ERROR_NOT_SUPPORTED);
        return false;
    }

    const bool readable = HasReadAccess(desiredAccess);
    const bool writable = HasWriteAccess(desiredAccess);
    if ((creationDisposition == CREATE_NEW || creationDisposition == CREATE_ALWAYS ||
        creationDisposition == TRUNCATE_EXISTING) && !writable)
    {
        SetWin32Error(win32Error, ERROR_ACCESS_DENIED);
        return false;
    }

    GuestPath path;
    if (!Resolve(fileName, &path, win32Error) || path.components.empty())
    {
        if (win32Error && *win32Error == ERROR_SUCCESS)
        {
            *win32Error = ERROR_INVALID_NAME;
        }
        return false;
    }

    const DWORD supportedSharing = FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE;
    if ((shareMode & ~supportedSharing) != 0)
    {
        SetWin32Error(win32Error, ERROR_INVALID_PARAMETER);
        return false;
    }
    const bool deleteAccess = (desiredAccess & DELETE) != 0 ||
        (flagsAndAttributes & FILE_FLAG_DELETE_ON_CLOSE) != 0;
    {
        std::lock_guard<std::mutex> guard(m_handlesLock);
        if (m_deletePending.find(MetadataKey(path.canonical)) != m_deletePending.end())
        {
            SetWin32Error(win32Error, ERROR_ACCESS_DENIED);
            return false;
        }
        for (const auto& entry : m_files)
        {
            const auto& existing = entry.second;
            if (!existing || _wcsicmp(existing->canonicalPath.c_str(), path.canonical.c_str()) != 0)
                continue;
            const bool existingReads = HasReadAccess(existing->desiredAccess);
            const bool existingWrites = HasWriteAccess(existing->desiredAccess);
            const bool existingDeletes = (existing->desiredAccess & DELETE) != 0 || existing->deleteOnClose;
            if ((readable && (existing->shareMode & FILE_SHARE_READ) == 0) ||
                (writable && (existing->shareMode & FILE_SHARE_WRITE) == 0) ||
                (deleteAccess && (existing->shareMode & FILE_SHARE_DELETE) == 0) ||
                (existingReads && (shareMode & FILE_SHARE_READ) == 0) ||
                (existingWrites && (shareMode & FILE_SHARE_WRITE) == 0) ||
                (existingDeletes && (shareMode & FILE_SHARE_DELETE) == 0))
            {
                SetWin32Error(win32Error, ERROR_SHARING_VIOLATION);
                return false;
            }
        }
    }

    StorageFolder^ parent = nullptr;
    std::wstring leaf;
    if (!GetParentFolder(path, &parent, &leaf, win32Error))
    {
        if (creationDisposition == OPEN_EXISTING &&
            OpenAuthorizedFile(path, desiredAccess, shareMode, guestHandle, win32Error))
            return true;
        return false;
    }

    try
    {
        StorageFile^ file = nullptr;
        const auto name = ref new String(leaf.c_str());
        switch (creationDisposition)
        {
        case CREATE_NEW:
            file = create_task(parent->CreateFileAsync(name, CreationCollisionOption::FailIfExists)).get();
            break;
        case CREATE_ALWAYS:
            file = create_task(parent->CreateFileAsync(name, CreationCollisionOption::ReplaceExisting)).get();
            break;
        case OPEN_ALWAYS:
            file = create_task(parent->CreateFileAsync(name, CreationCollisionOption::OpenIfExists)).get();
            break;
        case OPEN_EXISTING:
        case TRUNCATE_EXISTING:
            file = create_task(parent->GetFileAsync(name)).get();
            break;
        default:
            SetWin32Error(win32Error, ERROR_INVALID_PARAMETER);
            return false;
        }

        const FileAccessMode mode = writable ? FileAccessMode::ReadWrite : FileAccessMode::Read;
        IRandomAccessStream^ stream = create_task(file->OpenAsync(mode)).get();
        if (creationDisposition == TRUNCATE_EXISTING)
        {
            stream->Size = 0;
            stream->Seek(0);
        }
        if (!AddFile(stream, readable || !writable, writable, path.canonical,
            desiredAccess, shareMode,
            (flagsAndAttributes & FILE_FLAG_DELETE_ON_CLOSE) != 0,
            guestHandle, win32Error))
        {
            return false;
        }
        if (creationDisposition == CREATE_NEW || creationDisposition == CREATE_ALWAYS)
        {
            DWORD requestedAttributes = flagsAndAttributes & VirtualMutableFileAttributes;
            requestedAttributes &= ~FILE_ATTRIBUTE_DIRECTORY;
            if ((requestedAttributes & ~FILE_ATTRIBUTE_NORMAL) != 0)
                requestedAttributes &= ~FILE_ATTRIBUTE_NORMAL;
            if (requestedAttributes == 0) requestedAttributes = FILE_ATTRIBUTE_NORMAL;
            StoreAttributeOverride(path.canonical, requestedAttributes);
        }
        return true;
    }
    catch (Exception^ exception)
    {
        if (creationDisposition == OPEN_EXISTING &&
            OpenAuthorizedFile(path, desiredAccess, shareMode, guestHandle, win32Error))
            return true;
        SetWin32Error(win32Error, ErrorFromException(exception));
        return false;
    }
}

bool GuestStorageContext::ReadFile(HANDLE guestHandle, void* buffer, DWORD bytesToRead, DWORD* bytesRead, DWORD* win32Error)
{
    if (bytesRead)
    {
        *bytesRead = 0;
    }
    if ((bytesToRead != 0 && !buffer) || bytesToRead > MaxSynchronousIo)
    {
        SetWin32Error(win32Error, bytesToRead > MaxSynchronousIo ? ERROR_NOT_ENOUGH_MEMORY : ERROR_INVALID_PARAMETER);
        return false;
    }

    auto record = LookupFile(guestHandle);
    if (!record)
    {
        SetWin32Error(win32Error, ERROR_INVALID_HANDLE);
        return false;
    }
    try
    {
        std::lock_guard<std::mutex> guard(record->lock);
        IRandomAccessStream^ stream = record->stream.Get();
        if (record->closed || !stream)
        {
            SetWin32Error(win32Error, ERROR_INVALID_HANDLE);
            return false;
        }
        if (!record->readable)
        {
            SetWin32Error(win32Error, ERROR_ACCESS_DENIED);
            return false;
        }
        if (bytesToRead == 0)
        {
            SetWin32Error(win32Error, ERROR_SUCCESS);
            return true;
        }
        auto requested = ref new Buffer(bytesToRead);
        IBuffer^ data = create_task(stream->ReadAsync(requested, bytesToRead, InputStreamOptions::None)).get();
        auto copy = ref new Array<byte>(data->Length);
        DataReader::FromBuffer(data)->ReadBytes(copy);
        memcpy(buffer, copy->Data, copy->Length);
        if (bytesRead)
        {
            *bytesRead = copy->Length;
        }
        SetWin32Error(win32Error, ERROR_SUCCESS);
        return true;
    }
    catch (Exception^ exception)
    {
        SetWin32Error(win32Error, ErrorFromException(exception));
        return false;
    }
}

bool GuestStorageContext::WriteFile(HANDLE guestHandle, const void* buffer, DWORD bytesToWrite, DWORD* bytesWritten, DWORD* win32Error)
{
    if (bytesWritten)
    {
        *bytesWritten = 0;
    }
    if ((bytesToWrite != 0 && !buffer) || bytesToWrite > MaxSynchronousIo)
    {
        SetWin32Error(win32Error, bytesToWrite > MaxSynchronousIo ? ERROR_NOT_ENOUGH_MEMORY : ERROR_INVALID_PARAMETER);
        return false;
    }

    auto record = LookupFile(guestHandle);
    if (!record)
    {
        SetWin32Error(win32Error, ERROR_INVALID_HANDLE);
        return false;
    }
    try
    {
        Array<byte>^ copy = nullptr;
        if (bytesToWrite != 0)
        {
            copy = ref new Array<byte>(bytesToWrite);
            memcpy(copy->Data, buffer, bytesToWrite);
        }

        std::lock_guard<std::mutex> guard(record->lock);
        IRandomAccessStream^ stream = record->stream.Get();
        if (record->closed || !stream)
        {
            SetWin32Error(win32Error, ERROR_INVALID_HANDLE);
            return false;
        }
        if (!record->writable)
        {
            SetWin32Error(win32Error, ERROR_ACCESS_DENIED);
            return false;
        }
        if (bytesToWrite == 0)
        {
            SetWin32Error(win32Error, ERROR_SUCCESS);
            return true;
        }
        // Bind the writer directly to the random-access stream.  This is the
        // UWP storage contract equivalent of synchronous Win32 WriteFile and
        // avoids relying on a detached in-memory IBuffer being accepted by
        // every IRandomAccessStream implementation.  DetachStream is required
        // because disposing a DataWriter would otherwise close the file handle
        // after the first write.
        auto writer = ref new DataWriter(stream);
        writer->WriteBytes(copy);
        const unsigned int written = create_task(writer->StoreAsync()).get();
        writer->DetachStream();
        delete writer;
        if (bytesWritten)
        {
            *bytesWritten = written;
        }
        if (written != bytesToWrite)
        {
            SetWin32Error(win32Error, ERROR_WRITE_FAULT);
            return false;
        }
        SetWin32Error(win32Error, ERROR_SUCCESS);
        return true;
    }
    catch (Exception^ exception)
    {
        RecordStorageException(L"WriteFile", exception);
        const DWORD error = ErrorFromException(exception);
        SetWin32Error(win32Error,
            error == ERROR_SUCCESS ? ERROR_WRITE_FAULT : error);
        return false;
    }

}

bool GuestStorageContext::CloseFile(HANDLE guestHandle, DWORD* win32Error)
{
    const ULONG_PTR token = reinterpret_cast<ULONG_PTR>(guestHandle);
    std::shared_ptr<FileRecord> record;
    bool removeFile = false;
    {
        std::lock_guard<std::mutex> guard(m_handlesLock);
        const auto found = m_files.find(token);
        if (found == m_files.end())
        {
            SetWin32Error(win32Error, ERROR_INVALID_HANDLE);
            return false;
        }
        record = found->second;
        const std::wstring key = MetadataKey(record->canonicalPath);
        if (record->deleteOnClose) m_deletePending.insert(key);
        m_files.erase(found);
        bool stillOpen = false;
        for (const auto& entry : m_files)
        {
            if (entry.second && _wcsicmp(entry.second->canonicalPath.c_str(),
                record->canonicalPath.c_str()) == 0)
            {
                stillOpen = true;
                break;
            }
        }
        if (!stillOpen && m_deletePending.erase(key) != 0) removeFile = true;
    }

    try
    {
        std::lock_guard<std::mutex> guard(record->lock);
        if (record->closed)
        {
            SetWin32Error(win32Error, ERROR_INVALID_HANDLE);
            return false;
        }

        IRandomAccessStream^ stream = record->stream.Get();
        record->closed = true;
        record->stream = nullptr;
        if (!stream)
        {
            SetWin32Error(win32Error, ERROR_INVALID_HANDLE);
            return false;
        }
        delete stream;
        if (removeFile)
        {
            DWORD deleteError = ERROR_SUCCESS;
            if (!DeleteGuestFile(record->canonicalPath.c_str(), &deleteError))
            {
                SetWin32Error(win32Error, deleteError);
                return false;
            }
        }
        SetWin32Error(win32Error, ERROR_SUCCESS);
        return true;
    }
    catch (Exception^ exception)
    {
        SetWin32Error(win32Error, ErrorFromException(exception));
        return false;
    }
}

bool GuestStorageContext::GetFileSize(HANDLE guestHandle, LARGE_INTEGER* fileSize, DWORD* win32Error)
{
    if (!fileSize)
    {
        SetWin32Error(win32Error, ERROR_INVALID_PARAMETER);
        return false;
    }
    auto record = LookupFile(guestHandle);
    if (!record)
    {
        SetWin32Error(win32Error, ERROR_INVALID_HANDLE);
        return false;
    }

    try
    {
        std::lock_guard<std::mutex> guard(record->lock);
        IRandomAccessStream^ stream = record->stream.Get();
        if (record->closed || !stream)
        {
            SetWin32Error(win32Error, ERROR_INVALID_HANDLE);
            return false;
        }
        fileSize->QuadPart = static_cast<LONGLONG>(stream->Size);
        SetWin32Error(win32Error, ERROR_SUCCESS);
        return true;
    }
    catch (Exception^ exception)
    {
        SetWin32Error(win32Error, ErrorFromException(exception));
        return false;
    }
}

bool GuestStorageContext::SetFilePointer(
    HANDLE guestHandle,
    LARGE_INTEGER distance,
    LARGE_INTEGER* newPosition,
    DWORD moveMethod,
    DWORD* win32Error)
{
    auto record = LookupFile(guestHandle);
    if (!record)
    {
        SetWin32Error(win32Error, ERROR_INVALID_HANDLE);
        return false;
    }

    try
    {
        std::lock_guard<std::mutex> guard(record->lock);
        IRandomAccessStream^ stream = record->stream.Get();
        if (record->closed || !stream)
        {
            SetWin32Error(win32Error, ERROR_INVALID_HANDLE);
            return false;
        }
        ULONGLONG origin = 0;
        switch (moveMethod)
        {
        case FILE_BEGIN: origin = 0; break;
        case FILE_CURRENT: origin = stream->Position; break;
        case FILE_END: origin = stream->Size; break;
        default:
            SetWin32Error(win32Error, ERROR_INVALID_PARAMETER);
            return false;
        }

        const LONGLONG offset = distance.QuadPart;
        ULONGLONG position = 0;
        if (offset < 0)
        {
            const ULONGLONG magnitude = static_cast<ULONGLONG>(-(offset + 1)) + 1;
            if (magnitude > origin)
            {
                SetWin32Error(win32Error, ERROR_NEGATIVE_SEEK);
                return false;
            }
            position = origin - magnitude;
        }
        else
        {
            const ULONGLONG magnitude = static_cast<ULONGLONG>(offset);
            if (magnitude > ULLONG_MAX - origin)
            {
                SetWin32Error(win32Error, ERROR_NEGATIVE_SEEK);
                return false;
            }
            position = origin + magnitude;
        }

        stream->Seek(position);
        if (newPosition)
        {
            newPosition->QuadPart = static_cast<LONGLONG>(position);
        }
        SetWin32Error(win32Error, ERROR_SUCCESS);
        return true;
    }
    catch (Exception^ exception)
    {
        SetWin32Error(win32Error, ErrorFromException(exception));
        return false;
    }
}

bool GuestStorageContext::SetEndOfFile(HANDLE guestHandle, DWORD* win32Error)
{
    auto record = LookupFile(guestHandle);
    if (!record)
    {
        SetWin32Error(win32Error, ERROR_INVALID_HANDLE);
        return false;
    }

    try
    {
        std::lock_guard<std::mutex> guard(record->lock);
        IRandomAccessStream^ stream = record->stream.Get();
        if (record->closed || !stream)
        {
            SetWin32Error(win32Error, ERROR_INVALID_HANDLE);
            return false;
        }
        if (!record->writable)
        {
            SetWin32Error(win32Error, ERROR_ACCESS_DENIED);
            return false;
        }
        stream->Size = stream->Position;
        SetWin32Error(win32Error, ERROR_SUCCESS);
        return true;
    }
    catch (Exception^ exception)
    {
        SetWin32Error(win32Error, ErrorFromException(exception));
        return false;
    }
}

bool GuestStorageContext::FlushFile(HANDLE guestHandle, DWORD* win32Error)
{
    auto record = LookupFile(guestHandle);
    if (!record)
    {
        SetWin32Error(win32Error, ERROR_INVALID_HANDLE);
        return false;
    }

    try
    {
        std::lock_guard<std::mutex> guard(record->lock);
        IRandomAccessStream^ stream = record->stream.Get();
        if (record->closed || !stream)
        {
            SetWin32Error(win32Error, ERROR_INVALID_HANDLE);
            return false;
        }
        if (!create_task(stream->FlushAsync()).get())
        {
            SetWin32Error(win32Error, ERROR_WRITE_FAULT);
            return false;
        }
        SetWin32Error(win32Error, ERROR_SUCCESS);
        return true;
    }
    catch (Exception^ exception)
    {
        SetWin32Error(win32Error, ErrorFromException(exception));
        return false;
    }
}

bool GuestStorageContext::ReadAllBytes(LPCWSTR path, std::vector<BYTE>* bytes, DWORD* win32Error)
{
    if (!path || !bytes)
    {
        SetWin32Error(win32Error, ERROR_INVALID_PARAMETER);
        return false;
    }
    bytes->clear();

    HANDLE file = INVALID_HANDLE_VALUE;
    if (!CreateFile(path, GENERIC_READ, FILE_SHARE_READ, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr, &file, win32Error))
    {
        return false;
    }

    bool success = false;
    LARGE_INTEGER size{};
    if (GetFileSize(file, &size, win32Error) && size.QuadPart >= 0 &&
        static_cast<ULONGLONG>(size.QuadPart) <= 128ull * 1024ull * 1024ull)
    {
        try
        {
            bytes->resize(static_cast<size_t>(size.QuadPart));
            size_t offset = 0;
            while (offset < bytes->size())
            {
                const DWORD request = static_cast<DWORD>((std::min)(
                    bytes->size() - offset,
                    static_cast<size_t>(MaxSynchronousIo)));
                DWORD received = 0;
                if (!ReadFile(file, bytes->data() + offset, request, &received, win32Error) || received == 0)
                {
                    bytes->clear();
                    break;
                }
                offset += received;
            }
            success = offset == bytes->size();
            if (!success && bytes->empty() && size.QuadPart == 0)
            {
                success = true;
            }
            if (!success && win32Error && *win32Error == ERROR_SUCCESS)
            {
                *win32Error = ERROR_READ_FAULT;
            }
        }
        catch (const std::bad_alloc&)
        {
            SetWin32Error(win32Error, ERROR_NOT_ENOUGH_MEMORY);
        }
    }
    else if (win32Error && *win32Error == ERROR_SUCCESS)
    {
        *win32Error = ERROR_FILE_TOO_LARGE;
    }

    DWORD closeError = ERROR_SUCCESS;
    CloseFile(file, &closeError);
    return success;
}

bool GuestStorageContext::CreateDirectory(LPCWSTR path, DWORD* win32Error)
{
    GuestPath resolved;
    if (!Resolve(path, &resolved, win32Error) || resolved.components.empty())
    {
        if (win32Error && *win32Error == ERROR_SUCCESS)
        {
            *win32Error = ERROR_ALREADY_EXISTS;
        }
        return false;
    }

    StorageFolder^ parent = nullptr;
    std::wstring leaf;
    if (!GetParentFolder(resolved, &parent, &leaf, win32Error))
    {
        return false;
    }

    try
    {
        create_task(parent->CreateFolderAsync(ref new String(leaf.c_str()), CreationCollisionOption::FailIfExists)).get();
        StoreAttributeOverride(resolved.canonical, FILE_ATTRIBUTE_DIRECTORY);
        SetWin32Error(win32Error, ERROR_SUCCESS);
        return true;
    }
    catch (Exception^ exception)
    {
        SetWin32Error(win32Error, ErrorFromException(exception));
        return false;
    }
}

bool GuestStorageContext::DeleteGuestFile(LPCWSTR path, DWORD* win32Error)
{
    GuestPath resolved;
    if (!Resolve(path, &resolved, win32Error) || resolved.components.empty())
    {
        if (win32Error && *win32Error == ERROR_SUCCESS)
        {
            *win32Error = ERROR_ACCESS_DENIED;
        }
        return false;
    }

    {
        std::lock_guard<std::mutex> guard(m_handlesLock);
        bool open = false;
        for (const auto& entry : m_files)
        {
            const auto& record = entry.second;
            if (!record || _wcsicmp(record->canonicalPath.c_str(), resolved.canonical.c_str()) != 0)
                continue;
            if ((record->shareMode & FILE_SHARE_DELETE) == 0)
            {
                SetWin32Error(win32Error, ERROR_SHARING_VIOLATION);
                return false;
            }
            open = true;
        }
        if (open)
        {
            m_deletePending.insert(MetadataKey(resolved.canonical));
            SetWin32Error(win32Error, ERROR_SUCCESS);
            return true;
        }
        m_deletePending.erase(MetadataKey(resolved.canonical));
    }

    StorageFolder^ parent = nullptr;
    std::wstring leaf;
    if (!GetParentFolder(resolved, &parent, &leaf, win32Error))
    {
        return false;
    }

    try
    {
        IStorageItem^ item = create_task(parent->GetItemAsync(ref new String(leaf.c_str()))).get();
        if (!item->IsOfType(StorageItemTypes::File))
        {
            SetWin32Error(win32Error, ERROR_ACCESS_DENIED);
            return false;
        }
        create_task(item->DeleteAsync()).get();
        RemoveAttributeOverrides(resolved.canonical, false);
        SetWin32Error(win32Error, ERROR_SUCCESS);
        return true;
    }
    catch (Exception^ exception)
    {
        SetWin32Error(win32Error, ErrorFromException(exception));
        return false;
    }
}

bool GuestStorageContext::MoveGuestPath(
    LPCWSTR existingPath,
    LPCWSTR newPath,
    DWORD flags,
    DWORD* win32Error)
{
    // Every move remains in LocalFolder\drive_c. MOVEFILE_COPY_ALLOWED is a
    // harmless permission on this single virtual volume and must still be
    // accepted because file managers routinely pass it unconditionally.
    const DWORD supportedFlags = MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH |
        MOVEFILE_COPY_ALLOWED;
    if ((flags & ~supportedFlags) != 0)
    {
        SetWin32Error(win32Error, ERROR_NOT_SUPPORTED);
        return false;
    }

    GuestPath source;
    GuestPath destination;
    if (!Resolve(existingPath, &source, win32Error) ||
        !Resolve(newPath, &destination, win32Error) ||
        source.components.empty() || destination.components.empty())
    {
        if (win32Error && *win32Error == ERROR_SUCCESS)
        {
            *win32Error = ERROR_ACCESS_DENIED;
        }
        return false;
    }

    {
        std::lock_guard<std::mutex> guard(m_handlesLock);
        for (const auto& entry : m_files)
        {
            const auto& record = entry.second;
            if (!record) continue;
            const bool sourceOpen = _wcsicmp(record->canonicalPath.c_str(), source.canonical.c_str()) == 0;
            const bool destinationOpen = _wcsicmp(record->canonicalPath.c_str(), destination.canonical.c_str()) == 0;
            if ((sourceOpen || destinationOpen) && (record->shareMode & FILE_SHARE_DELETE) == 0)
            {
                SetWin32Error(win32Error, ERROR_SHARING_VIOLATION);
                return false;
            }
        }
        if (m_deletePending.count(MetadataKey(source.canonical)) != 0 ||
            m_deletePending.count(MetadataKey(destination.canonical)) != 0)
        {
            SetWin32Error(win32Error, ERROR_ACCESS_DENIED);
            return false;
        }
    }

    StorageFolder^ sourceParent = nullptr;
    StorageFolder^ destinationParent = nullptr;
    std::wstring sourceName;
    std::wstring destinationName;
    if (!GetParentFolder(source, &sourceParent, &sourceName, win32Error) ||
        !GetParentFolder(destination, &destinationParent, &destinationName, win32Error))
    {
        return false;
    }

    try
    {
        IStorageItem^ item = create_task(sourceParent->GetItemAsync(ref new String(sourceName.c_str()))).get();
        const NameCollisionOption collision = (flags & MOVEFILE_REPLACE_EXISTING) != 0
            ? NameCollisionOption::ReplaceExisting
            : NameCollisionOption::FailIfExists;

        if (item->IsOfType(StorageItemTypes::File))
        {
            StorageFile^ file = safe_cast<StorageFile^>(item);
            create_task(file->MoveAsync(destinationParent, ref new String(destinationName.c_str()), collision)).get();
            MoveAttributeOverrides(source.canonical, destination.canonical);
            SetWin32Error(win32Error, ERROR_SUCCESS);
            return true;
        }

        if (item->IsOfType(StorageItemTypes::Folder))
        {
            bool sameParent = source.components.size() == destination.components.size();
            if (sameParent)
            {
                for (size_t index = 0; index + 1 < source.components.size(); ++index)
                {
                    if (_wcsicmp(source.components[index].c_str(), destination.components[index].c_str()) != 0)
                    {
                        sameParent = false;
                        break;
                    }
                }
            }
            if (!sameParent)
            {
                if (destination.components.size() >= source.components.size() &&
                    std::equal(source.components.begin(), source.components.end(),
                        destination.components.begin(), [](const std::wstring& left,
                            const std::wstring& right)
                        {
                            return _wcsicmp(left.c_str(), right.c_str()) == 0;
                        }))
                {
                    SetWin32Error(win32Error, ERROR_ACCESS_DENIED);
                    return false;
                }
                MoveStorageFolderTree(safe_cast<StorageFolder^>(item), destinationParent,
                    destinationName, (flags & MOVEFILE_REPLACE_EXISTING) != 0, 0);
                MoveAttributeOverrides(source.canonical, destination.canonical);
                SetWin32Error(win32Error, ERROR_SUCCESS);
                RuntimeDiagnostics::Record(L"STORAGE: moved a virtual directory tree.");
                return true;
            }

            StorageFolder^ folder = safe_cast<StorageFolder^>(item);
            create_task(folder->RenameAsync(ref new String(destinationName.c_str()), collision)).get();
            MoveAttributeOverrides(source.canonical, destination.canonical);
            SetWin32Error(win32Error, ERROR_SUCCESS);
            return true;
        }

        SetWin32Error(win32Error, ERROR_NOT_SUPPORTED);
        return false;
    }
    catch (Exception^ exception)
    {
        SetWin32Error(win32Error, ErrorFromException(exception));
        return false;
    }
}

bool GuestStorageContext::RemoveGuestDirectory(LPCWSTR path, DWORD* win32Error)
{
    GuestPath resolved;
    if (!Resolve(path, &resolved, win32Error) || resolved.components.empty())
    {
        if (win32Error && *win32Error == ERROR_SUCCESS)
        {
            *win32Error = ERROR_ACCESS_DENIED;
        }
        return false;
    }

    StorageFolder^ folder = nullptr;
    if (!GetDirectoryFolder(resolved, &folder, win32Error))
    {
        return false;
    }

    try
    {
        auto items = create_task(folder->GetItemsAsync()).get();
        if (items->Size != 0)
        {
            SetWin32Error(win32Error, ERROR_DIR_NOT_EMPTY);
            return false;
        }
        create_task(folder->DeleteAsync()).get();
        RemoveAttributeOverrides(resolved.canonical, true);
        SetWin32Error(win32Error, ERROR_SUCCESS);
        return true;
    }
    catch (Exception^ exception)
    {
        SetWin32Error(win32Error, ErrorFromException(exception));
        return false;
    }
}

DWORD GuestStorageContext::GetGuestFileAttributes(LPCWSTR path, DWORD* win32Error)
{
    GuestPath resolved;
    if (!Resolve(path, &resolved, win32Error))
    {
        return INVALID_FILE_ATTRIBUTES;
    }

    if (resolved.components.empty())
    {
        StorageFolder^ root = nullptr;
        if (!GetDirectoryFolder(resolved, &root, win32Error))
        {
            return INVALID_FILE_ATTRIBUTES;
        }
        SetWin32Error(win32Error, ERROR_SUCCESS);
        return ApplyAttributeOverride(resolved.canonical, FILE_ATTRIBUTE_DIRECTORY);
    }

    StorageFolder^ parent = nullptr;
    std::wstring leaf;
    if (!GetParentFolder(resolved, &parent, &leaf, win32Error))
    {
        StorageFolder^ authorizedParent = nullptr;
        GuestPath parentPath = resolved;
        parentPath.components.pop_back();
        parentPath.canonical = parentPath.canonical.substr(0, parentPath.canonical.find_last_of(L'\\'));
        if (!GetAuthorizedFolder(parentPath, &authorizedParent))
            return INVALID_FILE_ATTRIBUTES;
        parent = authorizedParent;
    }

    try
    {
        IStorageItem^ item = create_task(parent->GetItemAsync(ref new String(leaf.c_str()))).get();
        const DWORD attributes = ApplyAttributeOverride(
            resolved.canonical,
            MapFileAttributes(item, item->IsOfType(StorageItemTypes::Folder)));
        SetWin32Error(win32Error, ERROR_SUCCESS);
        return attributes;
    }
    catch (Exception^ exception)
    {
        StorageFolder^ authorizedParent = nullptr;
        GuestPath parentPath = resolved;
        parentPath.components.pop_back();
        parentPath.canonical = parentPath.canonical.substr(0, parentPath.canonical.find_last_of(L'\\'));
        if (GetAuthorizedFolder(parentPath, &authorizedParent))
        {
            try
            {
                IStorageItem^ item = create_task(authorizedParent->GetItemAsync(
                    ref new String(leaf.c_str()))).get();
                SetWin32Error(win32Error, ERROR_SUCCESS);
                return MapFileAttributes(item, item->IsOfType(StorageItemTypes::Folder));
            }
            catch (...) { }
        }
        SetWin32Error(win32Error, ErrorFromException(exception));
        return INVALID_FILE_ATTRIBUTES;
    }
}

bool GuestStorageContext::SetGuestFileAttributes(
    LPCWSTR path,
    DWORD attributes,
    DWORD* win32Error)
{
    if (!path || attributes == 0 ||
        (attributes & ~VirtualMutableFileAttributes) != 0 ||
        ((attributes & FILE_ATTRIBUTE_NORMAL) != 0 && attributes != FILE_ATTRIBUTE_NORMAL))
    {
        SetWin32Error(win32Error, ERROR_INVALID_PARAMETER);
        return false;
    }

    GuestPath resolved;
    if (!Resolve(path, &resolved, win32Error)) return false;
    try
    {
        bool isDirectory = resolved.components.empty();
        if (isDirectory)
        {
            StorageFolder^ root = nullptr;
            if (!GetDirectoryFolder(resolved, &root, win32Error)) return false;
        }
        else
        {
            StorageFolder^ parent = nullptr;
            std::wstring leaf;
            if (!GetParentFolder(resolved, &parent, &leaf, win32Error)) return false;
            IStorageItem^ item = create_task(
                parent->GetItemAsync(ref new String(leaf.c_str()))).get();
            isDirectory = item->IsOfType(StorageItemTypes::Folder);
        }

        DWORD normalized = attributes & ~(FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_NORMAL);
        if (isDirectory) normalized |= FILE_ATTRIBUTE_DIRECTORY;
        if (normalized == 0) normalized = FILE_ATTRIBUTE_NORMAL;
        StoreAttributeOverride(resolved.canonical, normalized);
        SetWin32Error(win32Error, ERROR_SUCCESS);
        return true;
    }
    catch (Exception^ exception)
    {
        RecordStorageException(L"SetFileAttributesW item lookup", exception);
        SetWin32Error(win32Error, ErrorFromException(exception));
        return false;
    }
}

bool GuestStorageContext::FindFirstGuestFile(
    LPCWSTR searchPattern,
    WIN32_FIND_DATAW* findData,
    HANDLE* guestHandle,
    DWORD* win32Error)
{
    if (!findData || !guestHandle)
    {
        SetWin32Error(win32Error, ERROR_INVALID_PARAMETER);
        return false;
    }
    *guestHandle = INVALID_HANDLE_VALUE;

    std::wstring directory;
    std::wstring pattern;
    if (!SplitSearchPattern(searchPattern, &directory, &pattern, win32Error))
    {
        return false;
    }

    GuestPath resolvedDirectory;
    if (!Resolve(directory.c_str(), &resolvedDirectory, win32Error))
    {
        return false;
    }

    StorageFolder^ folder = nullptr;
    const bool haveLocalFolder = GetDirectoryFolder(resolvedDirectory, &folder, win32Error);
    StorageFolder^ authorizedFolder = nullptr;
    const bool haveAuthorizedFolder = GetAuthorizedFolder(resolvedDirectory, &authorizedFolder);
    if (!haveLocalFolder && !haveAuthorizedFolder) return false;

    try
    {
        std::vector<WIN32_FIND_DATAW> matches;
        const auto appendMatches = [&](StorageFolder^ source)
        {
            if (!source) return true;
            auto items = create_task(source->GetItemsAsync()).get();
            for (unsigned int index = 0; index < items->Size; ++index)
            {
                IStorageItem^ item = items->GetAt(index);
                const std::wstring name(item->Name->Data());
                if (!WildcardMatch(pattern, name)) continue;
                const bool duplicate = std::any_of(matches.begin(), matches.end(),
                    [&name](const WIN32_FIND_DATAW& entry)
                    { return _wcsicmp(entry.cFileName, name.c_str()) == 0; });
                if (duplicate) continue;
                WIN32_FIND_DATAW entry = {};
                if (!FillFindData(item, &entry, win32Error)) return false;
                entry.dwFileAttributes = ApplyAttributeOverride(
                    ChildCanonicalPath(resolvedDirectory.canonical, name),
                    entry.dwFileAttributes);
                matches.push_back(entry);
            }
            return true;
        };
        if ((haveLocalFolder && !appendMatches(folder)) ||
            (haveAuthorizedFolder && !appendMatches(authorizedFolder))) return false;

        if (matches.empty())
        {
            SetWin32Error(win32Error, ERROR_FILE_NOT_FOUND);
            return false;
        }

        if (g_storageEnumerationDiagnostics < 64)
        {
            ++g_storageEnumerationDiagnostics;
            RuntimeDiagnostics::Record(
                L"STORAGE: directory query produced " + std::to_wstring(matches.size()) + L" matching item(s).");
        }
        *findData = matches.front();
        return AddFind(std::make_shared<FindRecord>(std::move(matches)), guestHandle, win32Error);
    }
    catch (Exception^ exception)
    {
        RecordStorageException(L"GetItemsAsync during directory enumeration", exception);
        SetWin32Error(win32Error, ErrorFromException(exception));
        return false;
    }
}

bool GuestStorageContext::FindNextGuestFile(HANDLE guestHandle, WIN32_FIND_DATAW* findData, DWORD* win32Error)
{
    if (!findData)
    {
        SetWin32Error(win32Error, ERROR_INVALID_PARAMETER);
        return false;
    }

    auto record = LookupFind(guestHandle);
    if (!record)
    {
        SetWin32Error(win32Error, ERROR_INVALID_HANDLE);
        return false;
    }

    std::lock_guard<std::mutex> guard(record->lock);
    if (record->next >= record->entries.size())
    {
        SetWin32Error(win32Error, ERROR_NO_MORE_FILES);
        return false;
    }

    *findData = record->entries[record->next++];
    SetWin32Error(win32Error, ERROR_SUCCESS);
    return true;
}

bool GuestStorageContext::CloseFindHandle(HANDLE guestHandle, DWORD* win32Error)
{
    const ULONG_PTR token = reinterpret_cast<ULONG_PTR>(guestHandle);
    std::lock_guard<std::mutex> guard(m_handlesLock);
    const auto found = m_finds.find(token);
    if (found == m_finds.end())
    {
        SetWin32Error(win32Error, ERROR_INVALID_HANDLE);
        return false;
    }

    m_finds.erase(found);
    SetWin32Error(win32Error, ERROR_SUCCESS);
    return true;
}

bool GuestStorageContext::SetCurrentDirectory(LPCWSTR path, DWORD* win32Error)
{
    GuestPath resolved;
    if (!Resolve(path, &resolved, win32Error))
    {
        return false;
    }

    StorageFolder^ folder = nullptr;
    if (!GetDirectoryFolder(resolved, &folder, win32Error))
    {
        return false;
    }

    std::wstring ignored;
    if (!m_paths.SetCurrentDirectoryPath(resolved, &ignored))
    {
        SetWin32Error(win32Error, ERROR_INVALID_NAME);
        return false;
    }
    SetWin32Error(win32Error, ERROR_SUCCESS);
    return true;
}

bool GuestStorageContext::ResetCurrentDirectory(DWORD* win32Error)
{
    GuestPath initial;
    std::wstring ignored;
    if (!m_paths.Resolve(m_initialCurrentDirectory.c_str(), &initial, &ignored) ||
        !m_paths.SetCurrentDirectoryPath(initial, &ignored))
    {
        SetWin32Error(win32Error, ERROR_INVALID_NAME);
        return false;
    }

    SetWin32Error(win32Error, ERROR_SUCCESS);
    return true;
}

std::wstring GuestStorageContext::CurrentDirectory() const
{
    return m_paths.CurrentDirectory();
}

std::wstring GuestStorageContext::TempPath() const
{
    return L"C:\\Users\\Default\\AppData\\Local\\Temp\\";
}

void GuestStorageContext::CloseAll()
{
    std::unordered_map<ULONG_PTR, std::shared_ptr<FileRecord>> files;
    std::unordered_set<std::wstring> removePaths;
    {
        std::lock_guard<std::mutex> guard(m_handlesLock);
        files.swap(m_files);
        for (const auto& entry : files)
        {
            if (entry.second && entry.second->deleteOnClose)
                m_deletePending.insert(MetadataKey(entry.second->canonicalPath));
        }
        for (const auto& pending : m_deletePending)
        {
            for (const auto& entry : files)
            {
                if (entry.second && MetadataKey(entry.second->canonicalPath) == pending)
                {
                    removePaths.insert(entry.second->canonicalPath);
                    break;
                }
            }
        }
        m_deletePending.clear();
        m_finds.clear();
        m_nextHandle = FirstHandleToken;
    }

    for (const auto& pair : files)
    {
        try
        {
            std::lock_guard<std::mutex> guard(pair.second->lock);
            if (pair.second->closed)
            {
                continue;
            }

            IRandomAccessStream^ stream = pair.second->stream.Get();
            pair.second->closed = true;
            pair.second->stream = nullptr;
            if (stream)
            {
                delete stream;
            }
        }
        catch (...)
        {
            // Process teardown semantics: a failed close must not keep a guest alive.
        }
    }
    for (const auto& path : removePaths)
    {
        DWORD ignored = ERROR_SUCCESS;
        DeleteGuestFile(path.c_str(), &ignored);
    }
}

GuestStorageContext* Win32Bridge::Bridge::CurrentGuestStorageContext()
{
    return g_currentGuestStorage;
}

GuestStorageScope::GuestStorageScope(GuestStorageContext* context)
    : m_previous(g_currentGuestStorage)
{
    g_currentGuestStorage = context;
}

GuestStorageScope::~GuestStorageScope()
{
    g_currentGuestStorage = m_previous;
}
