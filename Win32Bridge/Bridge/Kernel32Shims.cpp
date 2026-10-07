#include "pch.h"
#include "Bridge/ApiSet.h"
#include "Bridge\\Kernel32Shims.h"
#include "Bridge/ActivationContext.h"
#include "Bridge\\GuestKernel.h"
#include "Bridge\\GuestModule.h"
#include "Bridge\\GuestResources.h"
#include "Bridge\\GuestRuntime.h"
#include "Bridge\\GuestStorage.h"
#include "Bridge\\RuntimeDiagnostics.h"
#include "Bridge\\Win32Shims.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cwctype>
#include <limits>
#include <memory>
#include <mutex>
#include <new>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>
#include <winternl.h>

using namespace Win32Bridge::Bridge;

namespace
{
    thread_local DWORD g_guestLastError = ERROR_SUCCESS;
    thread_local DWORD g_guestThreadId = 0;
    thread_local DWORD g_guestProcessId = 1;
    thread_local std::wstring g_guestCommandLine = L"Win32BridgeGuest";
    std::atomic<DWORD> g_nextGuestThreadId{ 1 };
    std::atomic<DWORD> g_guestDllDirectoryFlags{ 0 };
    std::atomic<unsigned> g_storageEnumerationDiagnostics{ 0 };
    std::atomic<ULONG_PTR> g_nextGuestResourceHandle{ 0x73000000 };
    std::mutex g_guestResourcesLock;
    std::unordered_map<ULONG_PTR, GuestResourceData> g_guestResources;
    constexpr ULONG_PTR FirstFileMappingHandle = 0x74000000;
    std::atomic<ULONG_PTR> g_nextFileMappingHandle{ FirstFileMappingHandle };

    struct GuestFileMapping final
    {
        BYTE* memory = nullptr;
        SIZE_T size = 0;
        DWORD protection = PAGE_READONLY;
        std::wstring name;

        ~GuestFileMapping()
        {
            if (memory) ::VirtualFree(memory, 0, MEM_RELEASE);
        }
    };

    struct GuestMappingView final
    {
        std::shared_ptr<GuestFileMapping> mapping;
        std::shared_ptr<void> privateAllocation;
        SIZE_T size = 0;
        ULONG references = 0;
    };

    std::mutex g_fileMappingsLock;
    std::unordered_map<ULONG_PTR, std::shared_ptr<GuestFileMapping>> g_fileMappings;
    std::unordered_map<std::wstring, std::weak_ptr<GuestFileMapping>> g_namedFileMappings;
    std::unordered_map<const void*, GuestMappingView> g_fileMappingViews;

    std::wstring FileMappingNameKey(LPCWSTR name)
    {
        std::wstring key = name ? name : L"";
        std::transform(key.begin(), key.end(), key.begin(), [](wchar_t value)
        {
            return static_cast<wchar_t>(std::towlower(value));
        });
        return key;
    }

    HANDLE RegisterFileMappingLocked(const std::shared_ptr<GuestFileMapping>& mapping)
    {
        if (!mapping) return nullptr;
        for (size_t attempt = 0; attempt < 4096; ++attempt)
        {
            ULONG_PTR token = g_nextFileMappingHandle.fetch_add(1);
            if (token < FirstFileMappingHandle)
            {
                token = FirstFileMappingHandle;
                g_nextFileMappingHandle.store(token + 1);
            }
            if (g_fileMappings.find(token) != g_fileMappings.end()) continue;
            g_fileMappings.emplace(token, mapping);
            return reinterpret_cast<HANDLE>(token);
        }
        return nullptr;
    }

    std::shared_ptr<GuestFileMapping> LookupFileMapping(HANDLE handle)
    {
        std::lock_guard<std::mutex> guard(g_fileMappingsLock);
        const auto found = g_fileMappings.find(reinterpret_cast<ULONG_PTR>(handle));
        return found == g_fileMappings.end() ? nullptr : found->second;
    }

    bool CloseFileMappingHandle(HANDLE handle)
    {
        std::lock_guard<std::mutex> guard(g_fileMappingsLock);
        const auto found = g_fileMappings.find(reinterpret_cast<ULONG_PTR>(handle));
        if (found == g_fileMappings.end()) return false;
        g_fileMappings.erase(found);
        return true;
    }

    void ResetGuestFileMappings()
    {
        std::lock_guard<std::mutex> guard(g_fileMappingsLock);
        g_fileMappingViews.clear();
        g_fileMappings.clear();
        g_namedFileMappings.clear();
        g_nextFileMappingHandle.store(FirstFileMappingHandle);
    }

    void RecordStorageEnumeration(const wchar_t* message)
    {
        // Enumeration can legitimately probe several times while the file
        // manager constructs its view. Keep the persisted runtime report
        // useful without turning it into an unbounded trace.
        if (g_storageEnumerationDiagnostics.fetch_add(1) < 32)
        {
            RuntimeDiagnostics::Record(message);
        }
    }

    // This is an opaque per-process identity for the primary PE, rather than
    // a host module address.  It gives normal Win32 startup code an HMODULE
    // it can carry into RegisterClass/CreateWindow without exposing a UWP
    // module or relying on a desktop loader.
    constexpr ULONG_PTR GuestMainModuleToken = 0x10000;
    constexpr ULONG_PTR GuestProcessHeapToken = 0x10001;
    constexpr ULONG_PTR BridgeSystemModuleBaseToken = 0x20000;

    // These are deliberately opaque guest-side HMODULE values. They identify
    // a bridge implementation, never a module in the UWP host process.
    struct BridgeSystemModule final
    {
        const wchar_t* library;
        ULONG_PTR token;
    };

    constexpr BridgeSystemModule BridgeSystemModules[] =
    {
        { L"kernel32.dll", BridgeSystemModuleBaseToken + 1 },
        { L"kernelbase.dll", BridgeSystemModuleBaseToken + 2 },
        { L"user32.dll", BridgeSystemModuleBaseToken + 3 },
        { L"gdi32.dll", BridgeSystemModuleBaseToken + 4 },
        { L"comctl32.dll", BridgeSystemModuleBaseToken + 5 },
        { L"shell32.dll", BridgeSystemModuleBaseToken + 6 },
        { L"ole32.dll", BridgeSystemModuleBaseToken + 7 },
        { L"oleaut32.dll", BridgeSystemModuleBaseToken + 8 },
        { L"advapi32.dll", BridgeSystemModuleBaseToken + 9 },
        { L"comdlg32.dll", BridgeSystemModuleBaseToken + 10 },
        { L"msvcrt.dll", BridgeSystemModuleBaseToken + 11 },
        { L"version.dll", BridgeSystemModuleBaseToken + 12 },
    };

    HMODULE GuestMainModule()
    {
        return reinterpret_cast<HMODULE>(GuestMainModuleToken);
    }

    bool IsGuestMainModule(HMODULE module)
    {
        return module == nullptr || module == GuestMainModule();
    }

    DWORD ResourceStatusError(GuestResourceStatus status)
    {
        switch (status)
        {
        case GuestResourceStatus::Success: return ERROR_SUCCESS;
        case GuestResourceStatus::InvalidParameter: return ERROR_INVALID_PARAMETER;
        case GuestResourceStatus::ModuleNotFound: return ERROR_MOD_NOT_FOUND;
        case GuestResourceStatus::TypeNotFound: return ERROR_RESOURCE_TYPE_NOT_FOUND;
        case GuestResourceStatus::NameNotFound: return ERROR_RESOURCE_NAME_NOT_FOUND;
        case GuestResourceStatus::LanguageNotFound: return ERROR_RESOURCE_LANG_NOT_FOUND;
        case GuestResourceStatus::InvalidImage:
        case GuestResourceStatus::InvalidData:
        default:
            return ERROR_RESOURCE_DATA_NOT_FOUND;
        }
    }

    HRSRC RegisterGuestResource(const GuestResourceData& resource)
    {
        try
        {
            std::lock_guard<std::mutex> guard(g_guestResourcesLock);
            for (const auto& existing : g_guestResources)
            {
                if (existing.second.module == resource.module &&
                    existing.second.data == resource.data &&
                    existing.second.size == resource.size &&
                    existing.second.language == resource.language)
                {
                    return reinterpret_cast<HRSRC>(existing.first);
                }
            }
            ULONG_PTR token = g_nextGuestResourceHandle.fetch_add(1);
            if (token <= 0xffff)
            {
                token = 0x73000000;
                g_nextGuestResourceHandle.store(token + 1);
            }
            g_guestResources[token] = resource;
            return reinterpret_cast<HRSRC>(token);
        }
        catch (...)
        {
            return nullptr;
        }
    }

    bool LookupGuestResource(HANDLE handle, GuestResourceData* resource)
    {
        if (!handle || !resource)
        {
            return false;
        }
        try
        {
            std::lock_guard<std::mutex> guard(g_guestResourcesLock);
            const auto found = g_guestResources.find(reinterpret_cast<ULONG_PTR>(handle));
            if (found == g_guestResources.end())
            {
                return false;
            }
            *resource = found->second;
            return true;
        }
        catch (...)
        {
            return false;
        }
    }

    bool ResourceModuleMatches(HMODULE requested, HMODULE actual)
    {
        if (IsGuestMainModule(requested) ||
            reinterpret_cast<const BYTE*>(requested) == CurrentGuestImageBase())
        {
            return actual == GuestMainModule();
        }
        return requested == actual;
    }

    HMODULE NormalizeResourceModule(HMODULE module)
    {
        return IsGuestMainModule(module) ||
            reinterpret_cast<const BYTE*>(module) == CurrentGuestImageBase()
            ? GuestMainModule()
            : module;
    }

    bool EnumeratesLocalResources(DWORD flags)
    {
        // MUI satellite probing is intentionally deferred. Zero is the legacy
        // API behavior; otherwise enumerate the language-neutral image only
        // when RESOURCE_ENUM_LN is explicitly requested.
        return flags == 0 || (flags & RESOURCE_ENUM_LN) != 0;
    }

    LPWSTR ResourceIdentifierPointer(const GuestResourceIdentifier& identifier)
    {
        return identifier.ordinal
            ? reinterpret_cast<LPWSTR>(static_cast<ULONG_PTR>(identifier.id))
            : const_cast<LPWSTR>(identifier.text.c_str());
    }

    bool ConvertAnsiResourceIdentifier(
        LPCSTR value,
        std::wstring* storage,
        LPCWSTR* converted)
    {
        if (!value || !storage || !converted)
        {
            return false;
        }
        storage->clear();
        const ULONG_PTR raw = reinterpret_cast<ULONG_PTR>(value);
        if (raw <= 0xffff)
        {
            *converted = reinterpret_cast<LPCWSTR>(raw);
            return true;
        }
        const int required = ::MultiByteToWideChar(CP_ACP, 0, value, -1, nullptr, 0);
        if (required <= 0)
        {
            return false;
        }
        storage->resize(static_cast<size_t>(required));
        if (::MultiByteToWideChar(CP_ACP, 0, value, -1, &(*storage)[0], required) != required)
        {
            storage->clear();
            return false;
        }
        storage->resize(static_cast<size_t>(required - 1));
        *converted = storage->c_str();
        return true;
    }

    LPSTR ConvertResourceIdentifierToAnsi(
        const GuestResourceIdentifier& identifier,
        std::string* storage)
    {
        if (identifier.ordinal)
        {
            return reinterpret_cast<LPSTR>(static_cast<ULONG_PTR>(identifier.id));
        }
        if (!storage)
        {
            return nullptr;
        }
        storage->clear();
        const int required = ::WideCharToMultiByte(
            CP_ACP, 0, identifier.text.c_str(), -1, nullptr, 0, nullptr, nullptr);
        if (required <= 0)
        {
            return nullptr;
        }
        storage->resize(static_cast<size_t>(required));
        if (::WideCharToMultiByte(
            CP_ACP, 0, identifier.text.c_str(), -1, &(*storage)[0], required,
            nullptr, nullptr) != required)
        {
            storage->clear();
            return nullptr;
        }
        return &(*storage)[0];
    }

    BOOL InvokeEnumResourceTypeA(
        ENUMRESTYPEPROCA callback,
        HMODULE module,
        LPSTR type,
        LONG_PTR parameter)
    {
        __try
        {
            return callback(module, type, parameter);
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return FALSE;
        }
    }

    BOOL InvokeEnumResourceNameA(
        ENUMRESNAMEPROCA callback,
        HMODULE module,
        LPCSTR type,
        LPSTR name,
        LONG_PTR parameter)
    {
        __try
        {
            return callback(module, type, name, parameter);
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return FALSE;
        }
    }

    BOOL InvokeEnumResourceLanguageA(
        ENUMRESLANGPROCA callback,
        HMODULE module,
        LPCSTR type,
        LPCSTR name,
        WORD language,
        LONG_PTR parameter)
    {
        __try
        {
            return callback(module, type, name, language, parameter);
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return FALSE;
        }
    }

    BOOL InvokeEnumResourceType(
        ENUMRESTYPEPROCW callback,
        HMODULE module,
        LPWSTR type,
        LONG_PTR parameter)
    {
        __try
        {
            return callback(module, type, parameter);
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return FALSE;
        }
    }

    BOOL InvokeEnumResourceName(
        ENUMRESNAMEPROCW callback,
        HMODULE module,
        LPCWSTR type,
        LPWSTR name,
        LONG_PTR parameter)
    {
        __try
        {
            return callback(module, type, name, parameter);
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return FALSE;
        }
    }

    BOOL InvokeEnumResourceLanguage(
        ENUMRESLANGPROCW callback,
        HMODULE module,
        LPCWSTR type,
        LPCWSTR name,
        WORD language,
        LONG_PTR parameter)
    {
        __try
        {
            return callback(module, type, name, language, parameter);
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return FALSE;
        }
    }

    const wchar_t* FileNameFromPath(LPCWSTR value)
    {
        if (!value)
        {
            return nullptr;
        }
        const wchar_t* slash = wcsrchr(value, L'\\');
        const wchar_t* forwardSlash = wcsrchr(value, L'/');
        const wchar_t* separator = slash;
        if (!separator || (forwardSlash && forwardSlash > separator))
        {
            separator = forwardSlash;
        }
        return separator ? separator + 1 : value;
    }

    const BridgeSystemModule* FindBridgeSystemModule(LPCWSTR requestedName)
    {
        if (!requestedName) return nullptr;
        const std::wstring canonical = ApiSetHostLibrary(requestedName);
        const wchar_t* fileName = FileNameFromPath(canonical.c_str());
        if (!fileName || !*fileName)
        {
            return nullptr;
        }

        for (const auto& candidate : BridgeSystemModules)
        {
            if (_wcsicmp(fileName, candidate.library) == 0)
            {
                return &candidate;
            }
        }

        return nullptr;
    }

    const BridgeSystemModule* FindBridgeSystemModule(HMODULE module)
    {
        for (const auto& candidate : BridgeSystemModules)
        {
            if (module == reinterpret_cast<HMODULE>(candidate.token))
            {
                return &candidate;
            }
        }
        return nullptr;
    }

    bool CopyAsciiImportName(LPCSTR source, std::wstring* destination)
    {
        if (!source || !destination)
        {
            return false;
        }
        destination->clear();
        for (size_t index = 0; source[index] != '\0'; ++index)
        {
            const unsigned char character = static_cast<unsigned char>(source[index]);
            if (character > 0x7f || index >= 255)
            {
                return false;
            }
            destination->push_back(static_cast<wchar_t>(character));
        }
        return !destination->empty();
    }

    FARPROC ResolveBridgeSystemProcedure(
        const BridgeSystemModule& module,
        LPCSTR nameOrOrdinal,
        DWORD* error)
    {
        if (!nameOrOrdinal)
        {
            *error = ERROR_INVALID_PARAMETER;
            RuntimeDiagnostics::Record(L"DYNAMIC IMPORT FAILED: " + std::wstring(module.library) + L" has an empty symbol.");
            return nullptr;
        }

        ImportedSymbol symbol;
        symbol.library = module.library;
        const ULONG_PTR rawSymbol = reinterpret_cast<ULONG_PTR>(nameOrOrdinal);
        if (rawSymbol <= 0xffff)
        {
            symbol.importedByOrdinal = true;
            symbol.ordinal = static_cast<WORD>(rawSymbol);
        }
        else if (!CopyAsciiImportName(nameOrOrdinal, &symbol.name))
        {
            *error = ERROR_NO_UNICODE_TRANSLATION;
            RuntimeDiagnostics::Record(L"DYNAMIC IMPORT FAILED: " + std::wstring(module.library) + L" has an invalid ANSI symbol.");
            return nullptr;
        }

        const ImportResolution resolution = ResolveRuntimeImport(symbol);
        if (resolution.targetAddress == 0)
        {
            *error = ERROR_PROC_NOT_FOUND;
            RuntimeDiagnostics::Record(
                L"DYNAMIC IMPORT MISSING: " + std::wstring(module.library) + L"!" +
                (symbol.importedByOrdinal ? (L"#" + std::to_wstring(symbol.ordinal)) : symbol.name));
            return nullptr;
        }

        *error = ERROR_SUCCESS;
        RuntimeDiagnostics::Record(
            L"DYNAMIC IMPORT OK: " + std::wstring(module.library) + L"!" +
            (symbol.importedByOrdinal ? (L"#" + std::to_wstring(symbol.ordinal)) : symbol.name));
        return reinterpret_cast<FARPROC>(static_cast<ULONG_PTR>(resolution.targetAddress));
    }

    ULONGLONG GuestMonotonicMilliseconds()
    {
        static const auto epoch = std::chrono::steady_clock::now();
        const auto elapsed = std::chrono::steady_clock::now() - epoch;
        return static_cast<ULONGLONG>(
            std::chrono::duration_cast<std::chrono::milliseconds>(elapsed).count());
    }

    void SetGuestLastError(DWORD error)
    {
        g_guestLastError = error;
    }

    GuestStorageContext* CurrentStorageOrFail()
    {
        GuestStorageContext* storage = CurrentGuestStorageContext();
        if (!storage)
        {
            SetGuestLastError(ERROR_INVALID_FUNCTION);
        }
        return storage;
    }

    GuestKernelContext* CurrentKernelOrFail()
    {
        GuestKernelContext* kernel = CurrentGuestKernelContext();
        if (!kernel)
        {
            SetGuestLastError(ERROR_INVALID_FUNCTION);
        }
        return kernel;
    }

    GuestModuleLoader* CurrentModuleLoaderOrFail()
    {
        GuestModuleLoader* loader = CurrentGuestModuleLoader();
        if (!loader)
        {
            SetGuestLastError(ERROR_INVALID_FUNCTION);
        }
        return loader;
    }

    bool CopyGuestString(const std::wstring& value, DWORD capacity, LPWSTR buffer, DWORD* result)
    {
        const DWORD required = static_cast<DWORD>(value.size() + 1);
        if (capacity == 0)
        {
            *result = required;
            return true;
        }
        if (!buffer)
        {
            SetGuestLastError(ERROR_INVALID_PARAMETER);
            *result = 0;
            return false;
        }
        if (capacity < required)
        {
            buffer[0] = L'\0';
            SetGuestLastError(ERROR_INSUFFICIENT_BUFFER);
            *result = required;
            return false;
        }

        memcpy(buffer, value.c_str(), required * sizeof(wchar_t));
        *result = static_cast<DWORD>(value.size());
        SetGuestLastError(ERROR_SUCCESS);
        return true;
    }

    bool IsKernelLibrary(const std::wstring& library)
    {
        return _wcsicmp(library.c_str(), L"kernel32.dll") == 0 ||
            _wcsicmp(library.c_str(), L"kernelbase.dll") == 0 ||
            _wcsicmp(library.c_str(), L"ntdll.dll") == 0 ||
            _wcsnicmp(library.c_str(), L"api-ms-win-core-", 16) == 0;
    }

    bool IsGuestModuleName(const std::wstring& modulePath, LPCWSTR requestedName)
    {
        if (!requestedName)
        {
            return true;
        }

        const size_t slash = modulePath.find_last_of(L"\\\\/");
        const wchar_t* fileName = slash == std::wstring::npos
            ? modulePath.c_str()
            : modulePath.c_str() + slash + 1;
        return _wcsicmp(requestedName, modulePath.c_str()) == 0 ||
            _wcsicmp(requestedName, fileName) == 0;
    }

    bool IsVirtualDriveRoot(LPCWSTR path)
    {
        return !path || !*path || _wcsicmp(path, L"C:") == 0 ||
            _wcsicmp(path, L"C:\\") == 0;
    }
}

void Win32Bridge::Bridge::ResetGuestResourceHandles()
{
    {
        std::lock_guard<std::mutex> guard(g_guestResourcesLock);
        g_guestResources.clear();
        g_nextGuestResourceHandle.store(0x73000000);
    }
    ResetGuestFileMappings();
}

HANDLE WINAPI Win32Bridge::Bridge::BridgeCreateFileW(
    LPCWSTR fileName,
    DWORD desiredAccess,
    DWORD shareMode,
    LPSECURITY_ATTRIBUTES,
    DWORD creationDisposition,
    DWORD flagsAndAttributes,
    HANDLE templateFile)
{
    GuestStorageContext* storage = CurrentStorageOrFail();
    if (!storage)
    {
        return INVALID_HANDLE_VALUE;
    }

    HANDLE handle = INVALID_HANDLE_VALUE;
    DWORD error = ERROR_SUCCESS;
    if (!storage->CreateFile(
        fileName,
        desiredAccess,
        shareMode,
        creationDisposition,
        flagsAndAttributes,
        templateFile,
        &handle,
        &error))
    {
        SetGuestLastError(error);
        RuntimeDiagnostics::Record(
            L"STORAGE: CreateFileW failed; disposition " +
            std::to_wstring(creationDisposition) + L", flags/attributes " +
            std::to_wstring(flagsAndAttributes) + L", error " +
            std::to_wstring(error) + L".");
        return INVALID_HANDLE_VALUE;
    }
    SetGuestLastError(ERROR_SUCCESS);
    return handle;
}

BOOL WINAPI Win32Bridge::Bridge::BridgeReadFile(
    HANDLE file,
    LPVOID buffer,
    DWORD bytesToRead,
    LPDWORD bytesRead,
    LPOVERLAPPED overlapped)
{
    if (overlapped)
    {
        RuntimeDiagnostics::Record(
            L"STORAGE: ReadFile rejected an OVERLAPPED request (" +
            std::to_wstring(bytesToRead) + L" byte(s)).");
        SetGuestLastError(ERROR_NOT_SUPPORTED);
        return FALSE;
    }
    GuestStorageContext* storage = CurrentStorageOrFail();
    if (!storage)
    {
        return FALSE;
    }

    DWORD error = ERROR_SUCCESS;
    if (!storage->ReadFile(file, buffer, bytesToRead, bytesRead, &error))
    {
        SetGuestLastError(error);
        return FALSE;
    }
    SetGuestLastError(ERROR_SUCCESS);
    return TRUE;
}

BOOL WINAPI Win32Bridge::Bridge::BridgeWriteFile(
    HANDLE file,
    LPCVOID buffer,
    DWORD bytesToWrite,
    LPDWORD bytesWritten,
    LPOVERLAPPED overlapped)
{
    if (overlapped)
    {
        RuntimeDiagnostics::Record(
            L"STORAGE: WriteFile rejected an OVERLAPPED request (" +
            std::to_wstring(bytesToWrite) + L" byte(s)).");
        SetGuestLastError(ERROR_NOT_SUPPORTED);
        return FALSE;
    }
    GuestStorageContext* storage = CurrentStorageOrFail();
    if (!storage)
    {
        return FALSE;
    }

    DWORD error = ERROR_SUCCESS;
    if (!storage->WriteFile(file, buffer, bytesToWrite, bytesWritten, &error))
    {
        if (error == ERROR_SUCCESS) error = ERROR_WRITE_FAULT;
        SetGuestLastError(error);
        RuntimeDiagnostics::Record(
            L"STORAGE: WriteFile failed for handle " +
            std::to_wstring(reinterpret_cast<ULONG_PTR>(file)) + L"; requested " +
            std::to_wstring(bytesToWrite) + L" byte(s), error " +
            std::to_wstring(error) + L".");
        return FALSE;
    }
    SetGuestLastError(ERROR_SUCCESS);
    return TRUE;
}

BOOL WINAPI Win32Bridge::Bridge::BridgeCloseHandle(HANDLE object)
{
    if (CloseFileMappingHandle(object))
    {
        SetGuestLastError(ERROR_SUCCESS);
        return TRUE;
    }

    // The ranges are disjoint, but try the synchronization table first so a
    // regular CloseHandle works for either family without changing file-handle
    // or FindClose semantics in the storage adapter.
    GuestKernelContext* kernel = CurrentGuestKernelContext();
    DWORD error = ERROR_INVALID_HANDLE;
    if (kernel && kernel->CloseHandle(object, &error))
    {
        SetGuestLastError(ERROR_SUCCESS);
        return TRUE;
    }

    GuestStorageContext* storage = CurrentGuestStorageContext();
    if (!storage)
    {
        SetGuestLastError(kernel ? ERROR_INVALID_HANDLE : ERROR_INVALID_FUNCTION);
        return FALSE;
    }

    error = ERROR_SUCCESS;
    if (!storage->CloseFile(object, &error))
    {
        SetGuestLastError(error);
        return FALSE;
    }
    SetGuestLastError(ERROR_SUCCESS);
    return TRUE;
}

BOOL WINAPI Win32Bridge::Bridge::BridgeGetFileSizeEx(HANDLE file, PLARGE_INTEGER fileSize)
{
    GuestStorageContext* storage = CurrentStorageOrFail();
    if (!storage)
    {
        return FALSE;
    }

    DWORD error = ERROR_SUCCESS;
    if (!storage->GetFileSize(file, fileSize, &error))
    {
        SetGuestLastError(error);
        return FALSE;
    }
    SetGuestLastError(ERROR_SUCCESS);
    return TRUE;
}

DWORD WINAPI Win32Bridge::Bridge::BridgeGetFileSize(HANDLE file, LPDWORD fileSizeHigh)
{
    LARGE_INTEGER size{};
    if (!BridgeGetFileSizeEx(file, &size))
    {
        return INVALID_FILE_SIZE;
    }
    const ULONGLONG value = static_cast<ULONGLONG>(size.QuadPart);
    if (fileSizeHigh)
    {
        *fileSizeHigh = static_cast<DWORD>(value >> 32);
    }
    return static_cast<DWORD>(value);
}

BOOL WINAPI Win32Bridge::Bridge::BridgeSetFilePointerEx(
    HANDLE file,
    LARGE_INTEGER distance,
    PLARGE_INTEGER newPosition,
    DWORD moveMethod)
{
    GuestStorageContext* storage = CurrentStorageOrFail();
    if (!storage)
    {
        return FALSE;
    }

    DWORD error = ERROR_SUCCESS;
    if (!storage->SetFilePointer(file, distance, newPosition, moveMethod, &error))
    {
        SetGuestLastError(error);
        return FALSE;
    }
    SetGuestLastError(ERROR_SUCCESS);
    return TRUE;
}

DWORD WINAPI Win32Bridge::Bridge::BridgeSetFilePointer(HANDLE file, LONG distance, PLONG distanceHigh, DWORD moveMethod)
{
    LARGE_INTEGER request{};
    request.LowPart = static_cast<DWORD>(distance);
    if (distanceHigh)
    {
        request.HighPart = *distanceHigh;
    }
    LARGE_INTEGER position{};
    if (!BridgeSetFilePointerEx(file, request, &position, moveMethod))
    {
        return INVALID_SET_FILE_POINTER;
    }
    if (distanceHigh)
    {
        *distanceHigh = position.HighPart;
    }
    return position.LowPart;
}

BOOL WINAPI Win32Bridge::Bridge::BridgeFlushFileBuffers(HANDLE file)
{
    GuestStorageContext* storage = CurrentStorageOrFail();
    if (!storage)
    {
        return FALSE;
    }

    DWORD error = ERROR_SUCCESS;
    if (!storage->FlushFile(file, &error))
    {
        SetGuestLastError(error);
        return FALSE;
    }
    SetGuestLastError(ERROR_SUCCESS);
    return TRUE;
}

BOOL WINAPI Win32Bridge::Bridge::BridgeCreateDirectoryW(LPCWSTR path, LPSECURITY_ATTRIBUTES)
{
    GuestStorageContext* storage = CurrentStorageOrFail();
    if (!storage)
    {
        return FALSE;
    }

    DWORD error = ERROR_SUCCESS;
    if (!storage->CreateDirectory(path, &error))
    {
        SetGuestLastError(error);
        return FALSE;
    }
    SetGuestLastError(ERROR_SUCCESS);
    return TRUE;
}

BOOL WINAPI Win32Bridge::Bridge::BridgeDeleteFileW(LPCWSTR path)
{
    GuestStorageContext* storage = CurrentStorageOrFail();
    if (!storage)
    {
        return FALSE;
    }

    DWORD error = ERROR_SUCCESS;
    if (!storage->DeleteGuestFile(path, &error))
    {
        SetGuestLastError(error);
        return FALSE;
    }
    SetGuestLastError(ERROR_SUCCESS);
    return TRUE;
}

BOOL WINAPI Win32Bridge::Bridge::BridgeMoveFileW(LPCWSTR existingFileName, LPCWSTR newFileName)
{
    return BridgeMoveFileExW(existingFileName, newFileName, 0);
}

BOOL WINAPI Win32Bridge::Bridge::BridgeMoveFileExW(
    LPCWSTR existingFileName,
    LPCWSTR newFileName,
    DWORD flags)
{
    GuestStorageContext* storage = CurrentStorageOrFail();
    if (!storage)
    {
        return FALSE;
    }

    DWORD error = ERROR_SUCCESS;
    if (!storage->MoveGuestPath(existingFileName, newFileName, flags, &error))
    {
        SetGuestLastError(error);
        return FALSE;
    }
    SetGuestLastError(ERROR_SUCCESS);
    return TRUE;
}

BOOL WINAPI Win32Bridge::Bridge::BridgeRemoveDirectoryW(LPCWSTR path)
{
    GuestStorageContext* storage = CurrentStorageOrFail();
    if (!storage)
    {
        return FALSE;
    }

    DWORD error = ERROR_SUCCESS;
    if (!storage->RemoveGuestDirectory(path, &error))
    {
        SetGuestLastError(error);
        return FALSE;
    }
    SetGuestLastError(ERROR_SUCCESS);
    return TRUE;
}

DWORD WINAPI Win32Bridge::Bridge::BridgeGetFileAttributesW(LPCWSTR path)
{
    GuestStorageContext* storage = CurrentStorageOrFail();
    if (!storage)
    {
        return INVALID_FILE_ATTRIBUTES;
    }

    DWORD error = ERROR_SUCCESS;
    const DWORD attributes = storage->GetGuestFileAttributes(path, &error);
    if (attributes == INVALID_FILE_ATTRIBUTES)
    {
        SetGuestLastError(error);
        return INVALID_FILE_ATTRIBUTES;
    }
    SetGuestLastError(ERROR_SUCCESS);
    return attributes;
}

BOOL WINAPI Win32Bridge::Bridge::BridgeSetFileAttributesW(LPCWSTR path, DWORD attributes)
{
    GuestStorageContext* storage = CurrentStorageOrFail();
    if (!storage)
    {
        return FALSE;
    }

    DWORD error = ERROR_SUCCESS;
    if (!storage->SetGuestFileAttributes(path, attributes, &error))
    {
        SetGuestLastError(error);
        RuntimeDiagnostics::Record(
            L"STORAGE: SetFileAttributesW failed; attributes " +
            std::to_wstring(attributes) + L", error " +
            std::to_wstring(error) + L".");
        return FALSE;
    }
    RuntimeDiagnostics::Record(
        L"STORAGE: applied virtual file attributes " +
        std::to_wstring(attributes) + L".");
    SetGuestLastError(ERROR_SUCCESS);
    return TRUE;
}

HANDLE WINAPI Win32Bridge::Bridge::BridgeFindFirstFileW(LPCWSTR searchPattern, LPWIN32_FIND_DATAW findData)
{
    GuestStorageContext* storage = CurrentStorageOrFail();
    if (!storage)
    {
        return INVALID_HANDLE_VALUE;
    }

    HANDLE handle = INVALID_HANDLE_VALUE;
    DWORD error = ERROR_SUCCESS;
    if (!storage->FindFirstGuestFile(searchPattern, findData, &handle, &error))
    {
        SetGuestLastError(error);
        const size_t length = searchPattern ? wcsnlen_s(searchPattern, 260) : 0;
        const std::wstring pattern = searchPattern
            ? std::wstring(searchPattern, length)
            : std::wstring(L"<null>");
        RecordStorageEnumeration((std::wstring(L"STORAGE: FindFirstFileW failed for virtual pattern '") +
            pattern + L"'; error " + std::to_wstring(error) + L".").c_str());
        return INVALID_HANDLE_VALUE;
    }
    SetGuestLastError(ERROR_SUCCESS);
    RecordStorageEnumeration(L"STORAGE: FindFirstFileW returned virtual directory data.");
    return handle;
}

BOOL WINAPI Win32Bridge::Bridge::BridgeFindNextFileW(HANDLE findHandle, LPWIN32_FIND_DATAW findData)
{
    GuestStorageContext* storage = CurrentStorageOrFail();
    if (!storage)
    {
        return FALSE;
    }

    DWORD error = ERROR_SUCCESS;
    if (!storage->FindNextGuestFile(findHandle, findData, &error))
    {
        SetGuestLastError(error);
        if (error != ERROR_NO_MORE_FILES)
        {
            RecordStorageEnumeration((std::wstring(L"STORAGE: FindNextFileW failed; error ") +
                std::to_wstring(error) + L".").c_str());
        }
        return FALSE;
    }
    SetGuestLastError(ERROR_SUCCESS);
    return TRUE;
}

BOOL WINAPI Win32Bridge::Bridge::BridgeFindClose(HANDLE findHandle)
{
    GuestStorageContext* storage = CurrentStorageOrFail();
    if (!storage)
    {
        return FALSE;
    }

    DWORD error = ERROR_SUCCESS;
    if (!storage->CloseFindHandle(findHandle, &error))
    {
        SetGuestLastError(error);
        return FALSE;
    }
    SetGuestLastError(ERROR_SUCCESS);
    return TRUE;
}

BOOL WINAPI Win32Bridge::Bridge::BridgeSetCurrentDirectoryW(LPCWSTR path)
{
    GuestStorageContext* storage = CurrentStorageOrFail();
    if (!storage)
    {
        return FALSE;
    }

    DWORD error = ERROR_SUCCESS;
    if (!storage->SetCurrentDirectory(path, &error))
    {
        SetGuestLastError(error);
        return FALSE;
    }
    SetGuestLastError(ERROR_SUCCESS);
    return TRUE;
}

DWORD WINAPI Win32Bridge::Bridge::BridgeGetCurrentDirectoryW(DWORD bufferLength, LPWSTR buffer)
{
    GuestStorageContext* storage = CurrentStorageOrFail();
    if (!storage)
    {
        return 0;
    }

    DWORD result = 0;
    CopyGuestString(storage->CurrentDirectory(), bufferLength, buffer, &result);
    return result;
}

DWORD WINAPI Win32Bridge::Bridge::BridgeGetTempPathW(DWORD bufferLength, LPWSTR buffer)
{
    GuestStorageContext* storage = CurrentStorageOrFail();
    if (!storage)
    {
        return 0;
    }

    DWORD result = 0;
    CopyGuestString(storage->TempPath(), bufferLength, buffer, &result);
    return result;
}

DWORD WINAPI Win32Bridge::Bridge::BridgeGetLogicalDrives()
{
    // Bit zero denotes C:.  The bridge deliberately exposes only the one
    // LocalFolder-backed volume, rather than leaking host drives into a UWP
    // guest.  Some file managers obtain their root list through this compact
    // mask instead of GetLogicalDriveStringsW.
    SetGuestLastError(ERROR_SUCCESS);
    RecordStorageEnumeration(L"STORAGE: GetLogicalDrives returned virtual C: drive.");
    return 1u << (L'C' - L'A');
}

DWORD WINAPI Win32Bridge::Bridge::BridgeGetLogicalDriveStringsW(DWORD bufferLength, LPWSTR buffer)
{
    const std::wstring drives = L"C:\\";
    const DWORD required = static_cast<DWORD>(drives.size() + 2);
    if (bufferLength < required)
    {
        SetGuestLastError(ERROR_INSUFFICIENT_BUFFER);
        return required;
    }
    if (!buffer)
    {
        SetGuestLastError(ERROR_INVALID_PARAMETER);
        return 0;
    }
    memcpy(buffer, drives.c_str(), (drives.size() + 1) * sizeof(wchar_t));
    buffer[drives.size() + 1] = L'\0';
    SetGuestLastError(ERROR_SUCCESS);
    RecordStorageEnumeration(L"STORAGE: GetLogicalDriveStringsW returned virtual C: drive.");
    return static_cast<DWORD>(drives.size() + 1);
}

UINT WINAPI Win32Bridge::Bridge::BridgeGetDriveTypeW(LPCWSTR rootPath)
{
    if (!IsVirtualDriveRoot(rootPath))
    {
        SetGuestLastError(ERROR_PATH_NOT_FOUND);
        return DRIVE_NO_ROOT_DIR;
    }
    SetGuestLastError(ERROR_SUCCESS);
    return DRIVE_FIXED;
}

BOOL WINAPI Win32Bridge::Bridge::BridgeGetDiskFreeSpaceExW(LPCWSTR rootPath, PULARGE_INTEGER available, PULARGE_INTEGER total, PULARGE_INTEGER freeBytes)
{
    if (!IsVirtualDriveRoot(rootPath))
    {
        SetGuestLastError(ERROR_PATH_NOT_FOUND);
        return FALSE;
    }
    constexpr ULONGLONG capacity = 8ull * 1024ull * 1024ull * 1024ull;
    if (available) available->QuadPart = capacity;
    if (total) total->QuadPart = capacity;
    if (freeBytes) freeBytes->QuadPart = capacity;
    SetGuestLastError(ERROR_SUCCESS);
    return TRUE;
}

BOOL WINAPI Win32Bridge::Bridge::BridgeGetDiskFreeSpaceW(LPCWSTR rootPath, LPDWORD sectors, LPDWORD bytes, LPDWORD freeClusters, LPDWORD totalClusters)
{
    if (!IsVirtualDriveRoot(rootPath) || !sectors || !bytes || !freeClusters || !totalClusters)
    {
        SetGuestLastError(!IsVirtualDriveRoot(rootPath) ? ERROR_PATH_NOT_FOUND : ERROR_INVALID_PARAMETER);
        return FALSE;
    }
    *sectors = 8; *bytes = 512; *totalClusters = 2 * 1024 * 1024; *freeClusters = *totalClusters;
    SetGuestLastError(ERROR_SUCCESS);
    return TRUE;
}

BOOL WINAPI Win32Bridge::Bridge::BridgeGetVolumeInformationW(LPCWSTR rootPath, LPWSTR name, DWORD nameSize, LPDWORD serial, LPDWORD componentLength, LPDWORD flags, LPWSTR fsName, DWORD fsNameSize)
{
    if (!IsVirtualDriveRoot(rootPath)) { SetGuestLastError(ERROR_PATH_NOT_FOUND); return FALSE; }
    const wchar_t* volume = L"Win32Bridge"; const wchar_t* fileSystem = L"NTFS";
    if ((name && nameSize <= wcslen(volume)) || (fsName && fsNameSize <= wcslen(fileSystem))) { SetGuestLastError(ERROR_MORE_DATA); return FALSE; }
    if (name) wcscpy_s(name, nameSize, volume);
    if (fsName) wcscpy_s(fsName, fsNameSize, fileSystem);
    if (serial) *serial = 0x57333242; if (componentLength) *componentLength = 255; if (flags) *flags = FILE_CASE_PRESERVED_NAMES | FILE_UNICODE_ON_DISK;
    SetGuestLastError(ERROR_SUCCESS); return TRUE;
}

UINT WINAPI Win32Bridge::Bridge::BridgeGetWindowsDirectoryW(LPWSTR buffer, UINT bufferLength)
{
    const std::wstring path = L"C:\\Windows"; DWORD result = 0;
    if (!CopyGuestString(path, bufferLength, buffer, &result)) return result;
    return result;
}

DWORD WINAPI Win32Bridge::Bridge::BridgeExpandEnvironmentStringsW(LPCWSTR source, LPWSTR destination, DWORD destinationLength)
{
    if (!source) { SetGuestLastError(ERROR_INVALID_PARAMETER); return 0; }
    std::wstring text(source); const std::wstring temp = L"%TEMP%";
    size_t pos = 0; while ((pos = text.find(temp, pos)) != std::wstring::npos) { text.replace(pos, temp.size(), L"C:\\Users\\Default\\AppData\\Local\\Temp"); }
    DWORD result = 0; CopyGuestString(text, destinationLength, destination, &result);
    return result + 1;
}

DWORD WINAPI Win32Bridge::Bridge::BridgeGetModuleFileNameW(HMODULE module, LPWSTR buffer, DWORD bufferLength)
{
    std::wstring path;
    if (const BridgeSystemModule* systemModule = FindBridgeSystemModule(module))
    {
        path = L"C:\\Windows\\System32\\" + std::wstring(systemModule->library);
    }
    else if (IsGuestMainModule(module))
    {
        GuestStorageContext* storage = CurrentStorageOrFail();
        if (!storage) return 0;
        path = storage->ModulePath();
    }
    else
    {
        GuestModuleLoader* loader = CurrentModuleLoaderOrFail();
        if (!loader) return 0;
        DWORD error = ERROR_SUCCESS;
        if (!loader->GetModulePath(module, &path, &error))
        {
            SetGuestLastError(error);
            return 0;
        }
    }

    DWORD result = 0;
    CopyGuestString(path, bufferLength, buffer, &result);
    return result;
}

HMODULE WINAPI Win32Bridge::Bridge::BridgeGetModuleHandleW(LPCWSTR moduleName)
{
    if (const BridgeSystemModule* systemModule = FindBridgeSystemModule(moduleName))
    {
        SetGuestLastError(ERROR_SUCCESS);
        RuntimeDiagnostics::Record(L"MODULE HANDLE: bridge " + std::wstring(systemModule->library) + L".");
        return reinterpret_cast<HMODULE>(systemModule->token);
    }

    GuestStorageContext* storage = CurrentStorageOrFail();
    if (!storage)
    {
        return nullptr;
    }

    if (IsGuestModuleName(storage->ModulePath(), moduleName))
    {
        SetGuestLastError(ERROR_SUCCESS);
        return GuestMainModule();
    }

    GuestModuleLoader* loader = CurrentModuleLoaderOrFail();
    if (!loader) return nullptr;
    DWORD error = ERROR_SUCCESS;
    HMODULE module = nullptr;
    if (!loader->GetModuleHandle(moduleName, &module, &error))
    {
        SetGuestLastError(error);
        return nullptr;
    }
    SetGuestLastError(ERROR_SUCCESS);
    return module;
}

HMODULE WINAPI Win32Bridge::Bridge::BridgeGetModuleHandleA(LPCSTR moduleName)
{
    if (!moduleName)
    {
        return BridgeGetModuleHandleW(nullptr);
    }
    std::wstring wideName;
    for (size_t index = 0; moduleName[index] != '\0'; ++index)
    {
        const unsigned char value = static_cast<unsigned char>(moduleName[index]);
        if (value > 0x7f || index >= MAX_PATH)
        {
            SetGuestLastError(ERROR_NO_UNICODE_TRANSLATION);
            return nullptr;
        }
        wideName.push_back(static_cast<wchar_t>(value));
    }
    return BridgeGetModuleHandleW(wideName.c_str());
}

HRSRC WINAPI Win32Bridge::Bridge::BridgeFindResourceA(
    HMODULE module,
    LPCSTR name,
    LPCSTR type)
{
    std::wstring wideNameStorage;
    std::wstring wideTypeStorage;
    LPCWSTR wideName = nullptr;
    LPCWSTR wideType = nullptr;
    if (!ConvertAnsiResourceIdentifier(name, &wideNameStorage, &wideName) ||
        !ConvertAnsiResourceIdentifier(type, &wideTypeStorage, &wideType))
    {
        SetGuestLastError(ERROR_NO_UNICODE_TRANSLATION);
        return nullptr;
    }
    return BridgeFindResourceW(module, wideName, wideType);
}

HRSRC WINAPI Win32Bridge::Bridge::BridgeFindResourceW(
    HMODULE module,
    LPCWSTR name,
    LPCWSTR type)
{
    GuestResourceData resource;
    const GuestResourceStatus status =
        FindGuestResource(module, type, name, 0, false, &resource);
    if (status != GuestResourceStatus::Success)
    {
        SetGuestLastError(ResourceStatusError(status));
        return nullptr;
    }
    const HRSRC handle = RegisterGuestResource(resource);
    SetGuestLastError(handle ? ERROR_SUCCESS : ERROR_NOT_ENOUGH_MEMORY);
    return handle;
}

HRSRC WINAPI Win32Bridge::Bridge::BridgeFindResourceExA(
    HMODULE module,
    LPCSTR type,
    LPCSTR name,
    WORD language)
{
    std::wstring wideNameStorage;
    std::wstring wideTypeStorage;
    LPCWSTR wideName = nullptr;
    LPCWSTR wideType = nullptr;
    if (!ConvertAnsiResourceIdentifier(name, &wideNameStorage, &wideName) ||
        !ConvertAnsiResourceIdentifier(type, &wideTypeStorage, &wideType))
    {
        SetGuestLastError(ERROR_NO_UNICODE_TRANSLATION);
        return nullptr;
    }
    return BridgeFindResourceExW(module, wideType, wideName, language);
}

HRSRC WINAPI Win32Bridge::Bridge::BridgeFindResourceExW(
    HMODULE module,
    LPCWSTR type,
    LPCWSTR name,
    WORD language)
{
    GuestResourceData resource;
    const GuestResourceStatus status =
        FindGuestResource(module, type, name, language, true, &resource);
    if (status != GuestResourceStatus::Success)
    {
        SetGuestLastError(ResourceStatusError(status));
        return nullptr;
    }
    const HRSRC handle = RegisterGuestResource(resource);
    SetGuestLastError(handle ? ERROR_SUCCESS : ERROR_NOT_ENOUGH_MEMORY);
    return handle;
}

HGLOBAL WINAPI Win32Bridge::Bridge::BridgeLoadResource(HMODULE module, HRSRC resourceHandle)
{
    GuestResourceData resource;
    if (!LookupGuestResource(resourceHandle, &resource) ||
        !ResourceModuleMatches(module, resource.module))
    {
        SetGuestLastError(ERROR_INVALID_HANDLE);
        return nullptr;
    }
    SetGuestLastError(ERROR_SUCCESS);
    return reinterpret_cast<HGLOBAL>(resourceHandle);
}

LPVOID WINAPI Win32Bridge::Bridge::BridgeLockResource(HGLOBAL resourceHandle)
{
    GuestResourceData resource;
    if (!LookupGuestResource(resourceHandle, &resource))
    {
        SetGuestLastError(ERROR_INVALID_HANDLE);
        return nullptr;
    }
    SetGuestLastError(ERROR_SUCCESS);
    return const_cast<BYTE*>(resource.data);
}

DWORD WINAPI Win32Bridge::Bridge::BridgeSizeofResource(HMODULE module, HRSRC resourceHandle)
{
    GuestResourceData resource;
    if (!LookupGuestResource(resourceHandle, &resource) ||
        !ResourceModuleMatches(module, resource.module))
    {
        SetGuestLastError(ERROR_INVALID_HANDLE);
        return 0;
    }
    if (resource.size > MAXDWORD)
    {
        SetGuestLastError(ERROR_RESOURCE_DATA_NOT_FOUND);
        return 0;
    }
    SetGuestLastError(ERROR_SUCCESS);
    return static_cast<DWORD>(resource.size);
}

BOOL WINAPI Win32Bridge::Bridge::BridgeFreeResource(HGLOBAL resourceHandle)
{
    GuestResourceData resource;
    if (!LookupGuestResource(resourceHandle, &resource))
    {
        SetGuestLastError(ERROR_INVALID_HANDLE);
        return FALSE;
    }
    // Resource bytes belong to the mapped image. FreeResource is obsolete on
    // Win32 and does not release that storage; preserve the handle registry so
    // a later LockResource remains valid for the image lifetime.
    SetGuestLastError(ERROR_SUCCESS);
    return FALSE;
}

BOOL WINAPI Win32Bridge::Bridge::BridgeEnumResourceTypesW(
    HMODULE module,
    ENUMRESTYPEPROCW callback,
    LONG_PTR parameter)
{
    if (!callback)
    {
        SetGuestLastError(ERROR_INVALID_PARAMETER);
        return FALSE;
    }
    std::vector<GuestResourceIdentifier> types;
    const GuestResourceStatus status = EnumerateGuestResourceTypes(module, &types);
    if (status != GuestResourceStatus::Success)
    {
        SetGuestLastError(ResourceStatusError(status));
        return FALSE;
    }
    SetGuestLastError(ERROR_SUCCESS);
    const HMODULE callbackModule = NormalizeResourceModule(module);
    for (const auto& type : types)
    {
        if (!InvokeEnumResourceType(
            callback, callbackModule, ResourceIdentifierPointer(type), parameter))
        {
            return FALSE;
        }
    }
    return TRUE;
}

BOOL WINAPI Win32Bridge::Bridge::BridgeEnumResourceTypesA(
    HMODULE module,
    ENUMRESTYPEPROCA callback,
    LONG_PTR parameter)
{
    if (!callback)
    {
        SetGuestLastError(ERROR_INVALID_PARAMETER);
        return FALSE;
    }
    std::vector<GuestResourceIdentifier> types;
    const GuestResourceStatus status = EnumerateGuestResourceTypes(module, &types);
    if (status != GuestResourceStatus::Success)
    {
        SetGuestLastError(ResourceStatusError(status));
        return FALSE;
    }
    SetGuestLastError(ERROR_SUCCESS);
    const HMODULE callbackModule = NormalizeResourceModule(module);
    for (const auto& type : types)
    {
        std::string ansiType;
        LPSTR pointer = ConvertResourceIdentifierToAnsi(type, &ansiType);
        if (!pointer)
        {
            SetGuestLastError(ERROR_NO_UNICODE_TRANSLATION);
            return FALSE;
        }
        if (!InvokeEnumResourceTypeA(callback, callbackModule, pointer, parameter))
        {
            return FALSE;
        }
    }
    return TRUE;
}

BOOL WINAPI Win32Bridge::Bridge::BridgeEnumResourceTypesExA(
    HMODULE module,
    ENUMRESTYPEPROCA callback,
    LONG_PTR parameter,
    DWORD flags,
    LANGID)
{
    if (!EnumeratesLocalResources(flags))
    {
        SetGuestLastError(ERROR_SUCCESS);
        return FALSE;
    }
    return BridgeEnumResourceTypesA(module, callback, parameter);
}

BOOL WINAPI Win32Bridge::Bridge::BridgeEnumResourceTypesExW(
    HMODULE module,
    ENUMRESTYPEPROCW callback,
    LONG_PTR parameter,
    DWORD flags,
    LANGID)
{
    if (!EnumeratesLocalResources(flags))
    {
        SetGuestLastError(ERROR_SUCCESS);
        return FALSE;
    }
    return BridgeEnumResourceTypesW(module, callback, parameter);
}

BOOL WINAPI Win32Bridge::Bridge::BridgeEnumResourceNamesW(
    HMODULE module,
    LPCWSTR type,
    ENUMRESNAMEPROCW callback,
    LONG_PTR parameter)
{
    if (!callback || !type)
    {
        SetGuestLastError(ERROR_INVALID_PARAMETER);
        return FALSE;
    }
    std::vector<GuestResourceIdentifier> names;
    const GuestResourceStatus status = EnumerateGuestResourceNames(module, type, &names);
    if (status != GuestResourceStatus::Success)
    {
        SetGuestLastError(ResourceStatusError(status));
        return FALSE;
    }
    SetGuestLastError(ERROR_SUCCESS);
    const HMODULE callbackModule = NormalizeResourceModule(module);
    for (const auto& name : names)
    {
        if (!InvokeEnumResourceName(
            callback, callbackModule, type, ResourceIdentifierPointer(name), parameter))
        {
            return FALSE;
        }
    }
    return TRUE;
}

BOOL WINAPI Win32Bridge::Bridge::BridgeEnumResourceNamesA(
    HMODULE module,
    LPCSTR type,
    ENUMRESNAMEPROCA callback,
    LONG_PTR parameter)
{
    if (!callback || !type)
    {
        SetGuestLastError(ERROR_INVALID_PARAMETER);
        return FALSE;
    }
    std::wstring wideTypeStorage;
    LPCWSTR wideType = nullptr;
    if (!ConvertAnsiResourceIdentifier(type, &wideTypeStorage, &wideType))
    {
        SetGuestLastError(ERROR_NO_UNICODE_TRANSLATION);
        return FALSE;
    }
    std::vector<GuestResourceIdentifier> names;
    const GuestResourceStatus status = EnumerateGuestResourceNames(module, wideType, &names);
    if (status != GuestResourceStatus::Success)
    {
        SetGuestLastError(ResourceStatusError(status));
        return FALSE;
    }
    SetGuestLastError(ERROR_SUCCESS);
    const HMODULE callbackModule = NormalizeResourceModule(module);
    for (const auto& name : names)
    {
        std::string ansiName;
        LPSTR pointer = ConvertResourceIdentifierToAnsi(name, &ansiName);
        if (!pointer)
        {
            SetGuestLastError(ERROR_NO_UNICODE_TRANSLATION);
            return FALSE;
        }
        if (!InvokeEnumResourceNameA(callback, callbackModule, type, pointer, parameter))
        {
            return FALSE;
        }
    }
    return TRUE;
}

BOOL WINAPI Win32Bridge::Bridge::BridgeEnumResourceNamesExA(
    HMODULE module,
    LPCSTR type,
    ENUMRESNAMEPROCA callback,
    LONG_PTR parameter,
    DWORD flags,
    LANGID)
{
    if (!EnumeratesLocalResources(flags))
    {
        SetGuestLastError(ERROR_SUCCESS);
        return FALSE;
    }
    return BridgeEnumResourceNamesA(module, type, callback, parameter);
}

BOOL WINAPI Win32Bridge::Bridge::BridgeEnumResourceNamesExW(
    HMODULE module,
    LPCWSTR type,
    ENUMRESNAMEPROCW callback,
    LONG_PTR parameter,
    DWORD flags,
    LANGID)
{
    if (!EnumeratesLocalResources(flags))
    {
        SetGuestLastError(ERROR_SUCCESS);
        return FALSE;
    }
    return BridgeEnumResourceNamesW(module, type, callback, parameter);
}

BOOL WINAPI Win32Bridge::Bridge::BridgeEnumResourceLanguagesW(
    HMODULE module,
    LPCWSTR type,
    LPCWSTR name,
    ENUMRESLANGPROCW callback,
    LONG_PTR parameter)
{
    if (!callback || !type || !name)
    {
        SetGuestLastError(ERROR_INVALID_PARAMETER);
        return FALSE;
    }
    std::vector<LANGID> languages;
    const GuestResourceStatus status =
        EnumerateGuestResourceLanguages(module, type, name, &languages);
    if (status != GuestResourceStatus::Success)
    {
        SetGuestLastError(ResourceStatusError(status));
        return FALSE;
    }
    SetGuestLastError(ERROR_SUCCESS);
    const HMODULE callbackModule = NormalizeResourceModule(module);
    for (LANGID language : languages)
    {
        if (!InvokeEnumResourceLanguage(
            callback, callbackModule, type, name, language, parameter))
        {
            return FALSE;
        }
    }
    return TRUE;
}

BOOL WINAPI Win32Bridge::Bridge::BridgeEnumResourceLanguagesA(
    HMODULE module,
    LPCSTR type,
    LPCSTR name,
    ENUMRESLANGPROCA callback,
    LONG_PTR parameter)
{
    if (!callback || !type || !name)
    {
        SetGuestLastError(ERROR_INVALID_PARAMETER);
        return FALSE;
    }
    std::wstring wideTypeStorage;
    std::wstring wideNameStorage;
    LPCWSTR wideType = nullptr;
    LPCWSTR wideName = nullptr;
    if (!ConvertAnsiResourceIdentifier(type, &wideTypeStorage, &wideType) ||
        !ConvertAnsiResourceIdentifier(name, &wideNameStorage, &wideName))
    {
        SetGuestLastError(ERROR_NO_UNICODE_TRANSLATION);
        return FALSE;
    }
    std::vector<LANGID> languages;
    const GuestResourceStatus status =
        EnumerateGuestResourceLanguages(module, wideType, wideName, &languages);
    if (status != GuestResourceStatus::Success)
    {
        SetGuestLastError(ResourceStatusError(status));
        return FALSE;
    }
    SetGuestLastError(ERROR_SUCCESS);
    const HMODULE callbackModule = NormalizeResourceModule(module);
    for (LANGID language : languages)
    {
        if (!InvokeEnumResourceLanguageA(
            callback, callbackModule, type, name, language, parameter))
        {
            return FALSE;
        }
    }
    return TRUE;
}

BOOL WINAPI Win32Bridge::Bridge::BridgeEnumResourceLanguagesExA(
    HMODULE module,
    LPCSTR type,
    LPCSTR name,
    ENUMRESLANGPROCA callback,
    LONG_PTR parameter,
    DWORD flags,
    LANGID)
{
    if (!EnumeratesLocalResources(flags))
    {
        SetGuestLastError(ERROR_SUCCESS);
        return FALSE;
    }
    return BridgeEnumResourceLanguagesA(module, type, name, callback, parameter);
}

BOOL WINAPI Win32Bridge::Bridge::BridgeEnumResourceLanguagesExW(
    HMODULE module,
    LPCWSTR type,
    LPCWSTR name,
    ENUMRESLANGPROCW callback,
    LONG_PTR parameter,
    DWORD flags,
    LANGID)
{
    if (!EnumeratesLocalResources(flags))
    {
        SetGuestLastError(ERROR_SUCCESS);
        return FALSE;
    }
    return BridgeEnumResourceLanguagesW(module, type, name, callback, parameter);
}

DWORD WINAPI Win32Bridge::Bridge::BridgeGetTickCount()
{
    return static_cast<DWORD>(GuestMonotonicMilliseconds());
}

ULONGLONG WINAPI Win32Bridge::Bridge::BridgeGetTickCount64()
{
    return GuestMonotonicMilliseconds();
}

BOOL WINAPI Win32Bridge::Bridge::BridgeQueryPerformanceCounter(PLARGE_INTEGER counter)
{
    if (!counter)
    {
        SetGuestLastError(ERROR_INVALID_PARAMETER);
        return FALSE;
    }

    // A millisecond monotonic clock is intentionally used as a consistent
    // clock domain with GetTickCount64.  The paired frequency reports 1000,
    // so elapsed-time calculations retain normal QPC semantics without
    // exposing host timing handles to the guest.
    counter->QuadPart = static_cast<LONGLONG>(GuestMonotonicMilliseconds());
    SetGuestLastError(ERROR_SUCCESS);
    return TRUE;
}

BOOL WINAPI Win32Bridge::Bridge::BridgeQueryPerformanceFrequency(PLARGE_INTEGER frequency)
{
    if (!frequency)
    {
        SetGuestLastError(ERROR_INVALID_PARAMETER);
        return FALSE;
    }

    frequency->QuadPart = 1000;
    SetGuestLastError(ERROR_SUCCESS);
    return TRUE;
}

DWORD WINAPI Win32Bridge::Bridge::BridgeGetCurrentThreadId()
{
    if (g_guestThreadId == 0)
    {
        DWORD assigned = g_nextGuestThreadId.fetch_add(1);
        if (assigned == 0)
        {
            assigned = g_nextGuestThreadId.fetch_add(1);
        }
        g_guestThreadId = assigned;
    }
    return g_guestThreadId;
}

DWORD WINAPI Win32Bridge::Bridge::BridgeGetCurrentProcessId()
{
    return g_guestProcessId;
}

void WINAPI Win32Bridge::Bridge::BridgeGetSystemTimeAsFileTime(LPFILETIME systemTime)
{
    if (!systemTime)
    {
        SetGuestLastError(ERROR_INVALID_PARAMETER);
        return;
    }
    ::GetSystemTimeAsFileTime(systemTime);
    SetGuestLastError(ERROR_SUCCESS);
}

HANDLE WINAPI Win32Bridge::Bridge::BridgeGetCurrentProcess()
{
    return reinterpret_cast<HANDLE>(static_cast<LONG_PTR>(-1));
}

LPVOID WINAPI Win32Bridge::Bridge::BridgeVirtualAlloc(LPVOID address, SIZE_T size, DWORD allocationType, DWORD protection)
{
    const DWORD supportedAllocation = MEM_RESERVE | MEM_COMMIT;
    if (size == 0 || (allocationType & ~supportedAllocation) != 0 ||
        (allocationType & supportedAllocation) == 0 ||
        (protection != PAGE_READWRITE && protection != PAGE_READONLY &&
            protection != PAGE_NOACCESS && protection != PAGE_EXECUTE_READ))
    {
        SetGuestLastError(ERROR_INVALID_PARAMETER);
        return nullptr;
    }

    LPVOID allocation = VirtualAllocFromApp(address, size, allocationType, protection);
    if (!allocation && address)
    {
        allocation = VirtualAllocFromApp(nullptr, size, allocationType, protection);
    }
    SetGuestLastError(allocation ? ERROR_SUCCESS : ::GetLastError());
    return allocation;
}

BOOL WINAPI Win32Bridge::Bridge::BridgeVirtualFree(LPVOID address, SIZE_T size, DWORD freeType)
{
    if (!address || (freeType != MEM_RELEASE && freeType != MEM_DECOMMIT) ||
        (freeType == MEM_RELEASE && size != 0))
    {
        SetGuestLastError(ERROR_INVALID_PARAMETER);
        return FALSE;
    }
    const BOOL result = ::VirtualFree(address, size, freeType);
    SetGuestLastError(result ? ERROR_SUCCESS : ::GetLastError());
    return result;
}

BOOL WINAPI Win32Bridge::Bridge::BridgeVirtualProtect(LPVOID address, SIZE_T size, DWORD protection, PDWORD oldProtection)
{
    if (!address || size == 0 || !oldProtection ||
        (protection != PAGE_READWRITE && protection != PAGE_READONLY &&
            protection != PAGE_NOACCESS && protection != PAGE_EXECUTE_READ))
    {
        SetGuestLastError(ERROR_INVALID_PARAMETER);
        return FALSE;
    }
    ULONG previous = 0;
    const BOOL result = VirtualProtectFromApp(address, size, protection, &previous);
    if (result)
    {
        *oldProtection = previous;
    }
    SetGuestLastError(result ? ERROR_SUCCESS : ::GetLastError());
    return result;
}

SIZE_T WINAPI Win32Bridge::Bridge::BridgeGetLargePageMinimum()
{
    // Large pages require privileges unavailable to an AppContainer guest.
    SetGuestLastError(ERROR_SUCCESS);
    return 0;
}

HGLOBAL WINAPI Win32Bridge::Bridge::BridgeGlobalAlloc(UINT flags, SIZE_T bytes)
{
    const DWORD heapFlags = (flags & GMEM_ZEROINIT) != 0 ? HEAP_ZERO_MEMORY : 0;
    HGLOBAL memory = static_cast<HGLOBAL>(::HeapAlloc(::GetProcessHeap(), heapFlags, bytes));
    SetGuestLastError(memory ? ERROR_SUCCESS : ERROR_NOT_ENOUGH_MEMORY);
    return memory;
}

HGLOBAL WINAPI Win32Bridge::Bridge::BridgeGlobalFree(HGLOBAL memory)
{
    if (!memory)
    {
        SetGuestLastError(ERROR_SUCCESS);
        return nullptr;
    }
    if (::HeapFree(::GetProcessHeap(), 0, memory))
    {
        SetGuestLastError(ERROR_SUCCESS);
        return nullptr;
    }
    SetGuestLastError(ERROR_INVALID_HANDLE);
    return memory;
}

LPVOID WINAPI Win32Bridge::Bridge::BridgeGlobalLock(HGLOBAL memory)
{
    if (!memory)
    {
        SetGuestLastError(ERROR_INVALID_HANDLE);
        return nullptr;
    }
    SetGuestLastError(ERROR_SUCCESS);
    return memory;
}

BOOL WINAPI Win32Bridge::Bridge::BridgeGlobalUnlock(HGLOBAL memory)
{
    if (!memory)
    {
        SetGuestLastError(ERROR_INVALID_HANDLE);
        return FALSE;
    }
    // Fixed global allocations have no movable-lock count.
    SetGuestLastError(ERROR_SUCCESS);
    return FALSE;
}

SIZE_T WINAPI Win32Bridge::Bridge::BridgeGlobalSize(HGLOBAL memory)
{
    if (!memory)
    {
        SetGuestLastError(ERROR_INVALID_HANDLE);
        return 0;
    }
    const SIZE_T size = ::HeapSize(::GetProcessHeap(), 0, memory);
    if (size == static_cast<SIZE_T>(-1))
    {
        SetGuestLastError(ERROR_INVALID_HANDLE);
        return 0;
    }
    SetGuestLastError(ERROR_SUCCESS);
    return size;
}

HANDLE WINAPI Win32Bridge::Bridge::BridgeGetProcessHeap()
{
    return reinterpret_cast<HANDLE>(GuestProcessHeapToken);
}

LPVOID WINAPI Win32Bridge::Bridge::BridgeHeapAlloc(HANDLE heap, DWORD flags, SIZE_T bytes)
{
    if (heap != reinterpret_cast<HANDLE>(GuestProcessHeapToken) || (flags & ~HEAP_ZERO_MEMORY) != 0)
    {
        SetGuestLastError(ERROR_INVALID_HANDLE);
        return nullptr;
    }
    LPVOID allocation = ::HeapAlloc(::GetProcessHeap(), flags, bytes);
    SetGuestLastError(allocation ? ERROR_SUCCESS : ERROR_NOT_ENOUGH_MEMORY);
    return allocation;
}

BOOL WINAPI Win32Bridge::Bridge::BridgeHeapFree(HANDLE heap, DWORD flags, LPVOID memory)
{
    if (heap != reinterpret_cast<HANDLE>(GuestProcessHeapToken) || flags != 0)
    {
        SetGuestLastError(ERROR_INVALID_HANDLE);
        return FALSE;
    }
    const BOOL result = ::HeapFree(::GetProcessHeap(), 0, memory);
    SetGuestLastError(result ? ERROR_SUCCESS : ERROR_INVALID_HANDLE);
    return result;
}

void WINAPI Win32Bridge::Bridge::BridgeInitializeCriticalSection(LPCRITICAL_SECTION criticalSection)
{
    if (!criticalSection)
    {
        SetGuestLastError(ERROR_INVALID_PARAMETER);
        return;
    }
    ::InitializeCriticalSection(criticalSection);
    SetGuestLastError(ERROR_SUCCESS);
}

void WINAPI Win32Bridge::Bridge::BridgeEnterCriticalSection(LPCRITICAL_SECTION criticalSection)
{
    if (!criticalSection)
    {
        SetGuestLastError(ERROR_INVALID_PARAMETER);
        return;
    }
    ::EnterCriticalSection(criticalSection);
    SetGuestLastError(ERROR_SUCCESS);
}

void WINAPI Win32Bridge::Bridge::BridgeLeaveCriticalSection(LPCRITICAL_SECTION criticalSection)
{
    if (!criticalSection)
    {
        SetGuestLastError(ERROR_INVALID_PARAMETER);
        return;
    }
    ::LeaveCriticalSection(criticalSection);
    SetGuestLastError(ERROR_SUCCESS);
}

void WINAPI Win32Bridge::Bridge::BridgeDeleteCriticalSection(LPCRITICAL_SECTION criticalSection)
{
    if (!criticalSection)
    {
        SetGuestLastError(ERROR_INVALID_PARAMETER);
        return;
    }
    ::DeleteCriticalSection(criticalSection);
    SetGuestLastError(ERROR_SUCCESS);
}

void WINAPI Win32Bridge::Bridge::BridgeGetSystemInfo(LPSYSTEM_INFO systemInfo)
{
    if (!systemInfo)
    {
        SetGuestLastError(ERROR_INVALID_PARAMETER);
        return;
    }
    ZeroMemory(systemInfo, sizeof(*systemInfo));
    systemInfo->wProcessorArchitecture = PROCESSOR_ARCHITECTURE_AMD64;
    systemInfo->dwPageSize = 4096;
    systemInfo->lpMinimumApplicationAddress = reinterpret_cast<LPVOID>(0x10000);
    systemInfo->lpMaximumApplicationAddress = reinterpret_cast<LPVOID>(static_cast<ULONG_PTR>(0x00007ffffffeffffull));
    systemInfo->dwActiveProcessorMask = 1;
    systemInfo->dwNumberOfProcessors = 1;
    systemInfo->dwProcessorType = PROCESSOR_AMD_X8664;
    systemInfo->dwAllocationGranularity = 65536;
    systemInfo->wProcessorLevel = 6;
    SetGuestLastError(ERROR_SUCCESS);
}

BOOL WINAPI Win32Bridge::Bridge::BridgeGlobalMemoryStatusEx(LPMEMORYSTATUSEX memoryStatus)
{
    if (!memoryStatus || memoryStatus->dwLength != sizeof(MEMORYSTATUSEX))
    {
        SetGuestLastError(ERROR_INVALID_PARAMETER);
        return FALSE;
    }
    memoryStatus->dwMemoryLoad = 0;
    memoryStatus->ullTotalPhys = 2ull * 1024ull * 1024ull * 1024ull;
    memoryStatus->ullAvailPhys = memoryStatus->ullTotalPhys;
    memoryStatus->ullTotalPageFile = memoryStatus->ullTotalPhys;
    memoryStatus->ullAvailPageFile = memoryStatus->ullTotalPageFile;
    memoryStatus->ullTotalVirtual = 128ull * 1024ull * 1024ull * 1024ull;
    memoryStatus->ullAvailVirtual = memoryStatus->ullTotalVirtual;
    memoryStatus->ullAvailExtendedVirtual = 0;
    SetGuestLastError(ERROR_SUCCESS);
    return TRUE;
}

BOOL WINAPI Win32Bridge::Bridge::BridgeIsProcessorFeaturePresent(DWORD)
{
    SetGuestLastError(ERROR_SUCCESS);
    return FALSE;
}

int WINAPI Win32Bridge::Bridge::BridgeLstrlenW(LPCWSTR text)
{
    if (!text)
    {
        SetGuestLastError(ERROR_SUCCESS);
        return 0;
    }
    const size_t length = wcslen(text);
    if (length > static_cast<size_t>((std::numeric_limits<int>::max)()))
    {
        SetGuestLastError(ERROR_BUFFER_OVERFLOW);
        return 0;
    }
    SetGuestLastError(ERROR_SUCCESS);
    return static_cast<int>(length);
}

LPWSTR WINAPI Win32Bridge::Bridge::BridgeLstrcatW(LPWSTR destination, LPCWSTR source)
{
    if (!destination || !source)
    {
        SetGuestLastError(ERROR_INVALID_PARAMETER);
        return nullptr;
    }
    // Win32 lstrcat has no length argument. Preserve that ABI for compatible
    // callers; the bridge cannot invent a buffer size not supplied by guest.
    // lstrcatW has no destination capacity parameter.  Retain its documented
    // ABI rather than inventing one for the guest-facing shim.
#pragma warning(suppress : 4996)
    wcscat(destination, source);
    SetGuestLastError(ERROR_SUCCESS);
    return destination;
}

int WINAPI Win32Bridge::Bridge::BridgeWideCharToMultiByte(
    UINT codePage,
    DWORD flags,
    LPCWCH wideCharacters,
    int wideCharacterCount,
    LPSTR multiByte,
    int multiByteCount,
    LPCCH defaultCharacter,
    LPBOOL usedDefaultCharacter)
{
    const int result = ::WideCharToMultiByte(
        codePage, flags, wideCharacters, wideCharacterCount,
        multiByte, multiByteCount, defaultCharacter, usedDefaultCharacter);
    SetGuestLastError(result != 0 ? ERROR_SUCCESS : ::GetLastError());
    return result;
}

int WINAPI Win32Bridge::Bridge::BridgeMultiByteToWideChar(
    UINT codePage,
    DWORD flags,
    LPCCH multiByte,
    int multiByteCount,
    LPWSTR wideCharacters,
    int wideCharacterCount)
{
    const int result = ::MultiByteToWideChar(
        codePage, flags, multiByte, multiByteCount, wideCharacters, wideCharacterCount);
    SetGuestLastError(result != 0 ? ERROR_SUCCESS : ::GetLastError());
    return result;
}

LANGID WINAPI Win32Bridge::Bridge::BridgeGetSystemDefaultLangID()
{
    // The virtual system language is stable and intentionally independent of
    // host profile data. Applications may still load their packaged locale.
    return MAKELANGID(LANG_ENGLISH, SUBLANG_ENGLISH_US);
}

LANGID WINAPI Win32Bridge::Bridge::BridgeGetUserDefaultLangID()
{
    return BridgeGetSystemDefaultLangID();
}

DWORD WINAPI Win32Bridge::Bridge::BridgeGetVersion()
{
    // Windows 10-compatible virtual version: major 10, minor 0.
    return 0x0000000a;
}

#include "Bridge/Kernel32LocaleShims.inl"

void WINAPI Win32Bridge::Bridge::BridgeGetStartupInfoA(LPSTARTUPINFOA startupInfo)
{
    if (!startupInfo) { SetGuestLastError(ERROR_INVALID_PARAMETER); return; }
    ZeroMemory(startupInfo, sizeof(*startupInfo)); startupInfo->cb = sizeof(*startupInfo);
    startupInfo->wShowWindow = SW_SHOWNORMAL; SetGuestLastError(ERROR_SUCCESS);
}

HANDLE WINAPI Win32Bridge::Bridge::BridgeGetStdHandle(DWORD)
{
    // GUI guests have no inherited console. NULL is the documented absence.
    SetGuestLastError(ERROR_SUCCESS); return nullptr;
}

BOOL WINAPI Win32Bridge::Bridge::BridgeGetConsoleMode(HANDLE, LPDWORD)
{
    SetGuestLastError(ERROR_INVALID_HANDLE); return FALSE;
}

BOOL WINAPI Win32Bridge::Bridge::BridgeSetPriorityClass(HANDLE process, DWORD)
{
    if (process != BridgeGetCurrentProcess()) { SetGuestLastError(ERROR_INVALID_HANDLE); return FALSE; }
    SetGuestLastError(ERROR_SUCCESS); return TRUE;
}

BOOL WINAPI Win32Bridge::Bridge::BridgeSetProcessAffinityMask(HANDLE process, DWORD_PTR processMask)
{
    if (process != BridgeGetCurrentProcess() || processMask == 0) { SetGuestLastError(ERROR_INVALID_PARAMETER); return FALSE; }
    SetGuestLastError(ERROR_SUCCESS); return TRUE;
}

DWORD_PTR WINAPI Win32Bridge::Bridge::BridgeSetThreadAffinityMask(HANDLE, DWORD_PTR threadMask)
{
    if (threadMask == 0) { SetGuestLastError(ERROR_INVALID_PARAMETER); return 0; }
    SetGuestLastError(ERROR_SUCCESS); return threadMask;
}

BOOL WINAPI Win32Bridge::Bridge::BridgeSetDefaultDllDirectories(DWORD directoryFlags)
{
    // The guest loader has no ambient desktop DLL search path. Retain this
    // setting so a later guest-DLL lookup can observe the caller's intended
    // policy, while always confining resolution to staged guest files and
    // bridge-provided system-module tokens.
    constexpr DWORD permittedFlags = 0x00000200u | // LOAD_LIBRARY_SEARCH_APPLICATION_DIR
        0x00000400u | // LOAD_LIBRARY_SEARCH_USER_DIRS
        0x00000800u | // LOAD_LIBRARY_SEARCH_SYSTEM32
        0x00001000u;  // LOAD_LIBRARY_SEARCH_DEFAULT_DIRS
    if ((directoryFlags & ~permittedFlags) != 0)
    {
        SetGuestLastError(ERROR_INVALID_PARAMETER);
        RuntimeDiagnostics::Record(L"DLL SEARCH FAILED: unsupported SetDefaultDllDirectories flags.");
        return FALSE;
    }
    g_guestDllDirectoryFlags.store(directoryFlags);
    SetGuestLastError(ERROR_SUCCESS);
    RuntimeDiagnostics::Record(L"DLL SEARCH: SetDefaultDllDirectories accepted.");
    return TRUE;
}

HMODULE WINAPI Win32Bridge::Bridge::BridgeLoadLibraryW(LPCWSTR fileName)
{
    if (const BridgeSystemModule* systemModule = FindBridgeSystemModule(fileName))
    {
        SetGuestLastError(ERROR_SUCCESS);
        RuntimeDiagnostics::Record(L"LOAD LIBRARY: bridge " + std::wstring(systemModule->library) + L".");
        return reinterpret_cast<HMODULE>(systemModule->token);
    }

    GuestModuleLoader* loader = CurrentModuleLoaderOrFail();
    if (!loader)
    {
        return nullptr;
    }
    DWORD error = ERROR_SUCCESS;
    HMODULE module = nullptr;
    if (!loader->LoadLibrary(fileName, &module, &error, g_guestDllDirectoryFlags.load()))
    {
        SetGuestLastError(error);
        return nullptr;
    }
    SetGuestLastError(ERROR_SUCCESS);
    return module;
}

HMODULE WINAPI Win32Bridge::Bridge::BridgeLoadLibraryA(LPCSTR fileName)
{
    if (!fileName)
    {
        SetGuestLastError(ERROR_INVALID_PARAMETER);
        return nullptr;
    }
    // Guest module names are ASCII DLL filenames in the initial loader
    // subset. Reject non-ASCII rather than silently applying a host codepage.
    std::wstring wideName;
    for (size_t index = 0; fileName[index] != '\0'; ++index)
    {
        const unsigned char value = static_cast<unsigned char>(fileName[index]);
        if (value > 0x7f || index >= MAX_PATH)
        {
            SetGuestLastError(ERROR_NO_UNICODE_TRANSLATION);
            return nullptr;
        }
        wideName.push_back(static_cast<wchar_t>(value));
    }
    return BridgeLoadLibraryW(wideName.c_str());
}

HMODULE WINAPI Win32Bridge::Bridge::BridgeLoadLibraryExW(LPCWSTR fileName, HANDLE file, DWORD flags)
{
    constexpr DWORD supportedSearchFlags = 0x00000100u | // LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR
        0x00000200u | // LOAD_LIBRARY_SEARCH_APPLICATION_DIR
        0x00000400u | // LOAD_LIBRARY_SEARCH_USER_DIRS
        0x00000800u | // LOAD_LIBRARY_SEARCH_SYSTEM32
        0x00001000u;  // LOAD_LIBRARY_SEARCH_DEFAULT_DIRS
    if (file != nullptr || (flags & ~supportedSearchFlags) != 0)
    {
        SetGuestLastError(ERROR_NOT_SUPPORTED);
        return nullptr;
    }
    if (const BridgeSystemModule* systemModule = FindBridgeSystemModule(fileName))
    {
        SetGuestLastError(ERROR_SUCCESS);
        RuntimeDiagnostics::Record(L"LOAD LIBRARY EX: bridge " + std::wstring(systemModule->library) + L".");
        return reinterpret_cast<HMODULE>(systemModule->token);
    }

    GuestModuleLoader* loader = CurrentModuleLoaderOrFail();
    if (!loader) return nullptr;

    DWORD error = ERROR_SUCCESS;
    HMODULE module = nullptr;
    const DWORD effectiveFlags = flags == 0 ? g_guestDllDirectoryFlags.load() : flags;
    if (!loader->LoadLibrary(fileName, &module, &error, effectiveFlags))
    {
        SetGuestLastError(error);
        return nullptr;
    }
    SetGuestLastError(ERROR_SUCCESS);
    return module;
}

FARPROC WINAPI Win32Bridge::Bridge::BridgeGetProcAddress(HMODULE module, LPCSTR nameOrOrdinal)
{
    if (const BridgeSystemModule* systemModule = FindBridgeSystemModule(module))
    {
        DWORD error = ERROR_SUCCESS;
        FARPROC procedure = ResolveBridgeSystemProcedure(*systemModule, nameOrOrdinal, &error);
        SetGuestLastError(error);
        return procedure;
    }

    GuestModuleLoader* loader = CurrentModuleLoaderOrFail();
    if (!loader)
    {
        return nullptr;
    }
    DWORD error = ERROR_SUCCESS;
    FARPROC procedure = loader->GetProcAddress(module, nameOrOrdinal, &error);
    SetGuestLastError(error);
    return procedure;
}

BOOL WINAPI Win32Bridge::Bridge::BridgeFreeLibrary(HMODULE module)
{
    if (FindBridgeSystemModule(module))
    {
        // Bridge module tokens describe services owned by the runtime. They
        // have process lifetime and therefore have no unload side effects.
        SetGuestLastError(ERROR_SUCCESS);
        return TRUE;
    }

    GuestModuleLoader* loader = CurrentModuleLoaderOrFail();
    if (!loader)
    {
        return FALSE;
    }
    DWORD error = ERROR_SUCCESS;
    if (!loader->FreeLibrary(module, &error))
    {
        SetGuestLastError(error);
        return FALSE;
    }
    SetGuestLastError(ERROR_SUCCESS);
    return TRUE;
}

GuestCommandLineScope::GuestCommandLineScope(
    const std::wstring& commandLine,
    DWORD processId,
    DWORD threadId)
    : m_previous(std::move(g_guestCommandLine)),
      m_previousProcessId(g_guestProcessId),
      m_previousThreadId(g_guestThreadId)
{
    g_guestCommandLine = commandLine.empty() ? L"Win32BridgeGuest" : commandLine;
    g_guestProcessId = processId ? processId : 1;
    if (threadId) g_guestThreadId = threadId;
}

BOOL WINAPI Win32Bridge::Bridge::BridgeDisableThreadLibraryCalls(HMODULE module)
{
    GuestModuleLoader* loader = CurrentGuestModuleLoader();
    if (!loader)
    {
        SetGuestLastError(ERROR_INVALID_HANDLE);
        return FALSE;
    }
    DWORD error = ERROR_SUCCESS;
    if (!loader->DisableThreadNotifications(module, &error))
    {
        SetGuestLastError(error);
        return FALSE;
    }
    SetGuestLastError(ERROR_SUCCESS);
    return TRUE;
}

GuestCommandLineScope::~GuestCommandLineScope()
{
    g_guestCommandLine = std::move(m_previous);
    g_guestProcessId = m_previousProcessId;
    g_guestThreadId = m_previousThreadId;
}

LPWSTR WINAPI Win32Bridge::Bridge::BridgeGetCommandLineW()
{
    // The backing string is per guest thread and intentionally writable, as
    // required by the historical GetCommandLineW contract.
    return g_guestCommandLine.empty() ? const_cast<LPWSTR>(L"") : &g_guestCommandLine[0];
}

void WINAPI Win32Bridge::Bridge::BridgeOutputDebugStringW(LPCWSTR)
{
    // Debug output is deliberately isolated from the host process. Binding
    // the common diagnostic import keeps normal release binaries runnable.
}

BOOL WINAPI Win32Bridge::Bridge::BridgeIsDebuggerPresent()
{
    return FALSE;
}

DWORD WINAPI Win32Bridge::Bridge::BridgeGetLastError()
{
    return g_guestLastError;
}

void WINAPI Win32Bridge::Bridge::BridgeSetLastError(DWORD error)
{
    SetGuestLastError(error);
}

HANDLE WINAPI Win32Bridge::Bridge::BridgeCreateEventW(
    LPSECURITY_ATTRIBUTES,
    BOOL manualReset,
    BOOL initialState,
    LPCWSTR name)
{
    GuestKernelContext* kernel = CurrentKernelOrFail();
    if (!kernel)
    {
        return nullptr;
    }

    DWORD error = ERROR_SUCCESS;
    HANDLE handle = kernel->CreateEvent(manualReset != FALSE, initialState != FALSE, name, &error);
    if (name && *name)
    {
        RuntimeDiagnostics::Record(
            handle
                ? L"KERNEL: opened or created named event '" + std::wstring(name) + L"'."
                : L"KERNEL: failed to create named event '" + std::wstring(name) +
                    L"'; error " + std::to_wstring(error) + L".");
    }
    SetGuestLastError(error);
    return handle;
}

BOOL WINAPI Win32Bridge::Bridge::BridgeSetEvent(HANDLE eventHandle)
{
    GuestKernelContext* kernel = CurrentKernelOrFail();
    if (!kernel)
    {
        return FALSE;
    }

    DWORD error = ERROR_SUCCESS;
    if (!kernel->SetEvent(eventHandle, &error))
    {
        SetGuestLastError(error);
        return FALSE;
    }
    SetGuestLastError(ERROR_SUCCESS);
    return TRUE;
}

BOOL WINAPI Win32Bridge::Bridge::BridgeResetEvent(HANDLE eventHandle)
{
    GuestKernelContext* kernel = CurrentKernelOrFail();
    if (!kernel)
    {
        return FALSE;
    }

    DWORD error = ERROR_SUCCESS;
    if (!kernel->ResetEvent(eventHandle, &error))
    {
        SetGuestLastError(error);
        return FALSE;
    }
    SetGuestLastError(ERROR_SUCCESS);
    return TRUE;
}

HANDLE WINAPI Win32Bridge::Bridge::BridgeCreateMutexW(
    LPSECURITY_ATTRIBUTES,
    BOOL initialOwner,
    LPCWSTR name)
{
    GuestKernelContext* kernel = CurrentKernelOrFail();
    if (!kernel)
    {
        return nullptr;
    }

    DWORD error = ERROR_SUCCESS;
    HANDLE handle = kernel->CreateMutex(initialOwner != FALSE, name, &error);
    SetGuestLastError(error);
    return handle;
}

BOOL WINAPI Win32Bridge::Bridge::BridgeReleaseMutex(HANDLE mutexHandle)
{
    GuestKernelContext* kernel = CurrentKernelOrFail();
    if (!kernel)
    {
        return FALSE;
    }

    DWORD error = ERROR_SUCCESS;
    if (!kernel->ReleaseMutex(mutexHandle, &error))
    {
        SetGuestLastError(error);
        return FALSE;
    }
    SetGuestLastError(ERROR_SUCCESS);
    return TRUE;
}

HANDLE WINAPI Win32Bridge::Bridge::BridgeCreateSemaphoreW(
    LPSECURITY_ATTRIBUTES,
    LONG initialCount,
    LONG maximumCount,
    LPCWSTR name)
{
    GuestKernelContext* kernel = CurrentKernelOrFail();
    if (!kernel)
    {
        return nullptr;
    }
    DWORD error = ERROR_SUCCESS;
    HANDLE handle = kernel->CreateSemaphore(initialCount, maximumCount, name, &error);
    SetGuestLastError(error);
    return handle;
}

BOOL WINAPI Win32Bridge::Bridge::BridgeReleaseSemaphore(HANDLE semaphoreHandle, LONG releaseCount, LPLONG previousCount)
{
    GuestKernelContext* kernel = CurrentKernelOrFail();
    if (!kernel)
    {
        return FALSE;
    }
    DWORD error = ERROR_SUCCESS;
    if (!kernel->ReleaseSemaphore(semaphoreHandle, releaseCount, previousCount, &error))
    {
        SetGuestLastError(error);
        return FALSE;
    }
    SetGuestLastError(ERROR_SUCCESS);
    return TRUE;
}

DWORD WINAPI Win32Bridge::Bridge::BridgeWaitForSingleObject(HANDLE object, DWORD milliseconds)
{
    GuestKernelContext* kernel = CurrentKernelOrFail();
    if (!kernel)
    {
        return WAIT_FAILED;
    }

    DWORD error = ERROR_SUCCESS;
    const DWORD result = kernel->WaitForSingleObject(object, milliseconds, &error);
    if (result == WAIT_FAILED)
    {
        SetGuestLastError(error);
    }
    return result;
}

DWORD WINAPI Win32Bridge::Bridge::BridgeWaitForMultipleObjects(DWORD count, const HANDLE* handles, BOOL waitAll, DWORD milliseconds)
{
    GuestKernelContext* kernel = CurrentKernelOrFail();
    if (!kernel)
    {
        return WAIT_FAILED;
    }
    DWORD error = ERROR_SUCCESS;
    const DWORD result = kernel->WaitForMultipleObjects(count, handles, waitAll != FALSE, milliseconds, &error);
    if (result == WAIT_FAILED)
    {
        SetGuestLastError(error);
    }
    return result;
}

void WINAPI Win32Bridge::Bridge::BridgeSleep(DWORD milliseconds)
{
    if (milliseconds == 0)
    {
        std::this_thread::yield();
        return;
    }

    if (milliseconds == INFINITE)
    {
        // This deliberately has the same terminal behavior as Sleep(INFINITE)
        // until a future guest-thread/termination subsystem can interrupt it.
        std::mutex blockerMutex;
        std::condition_variable blocker;
        std::unique_lock<std::mutex> lock(blockerMutex);
        blocker.wait(lock);
        return;
    }

    std::this_thread::sleep_for(std::chrono::milliseconds(milliseconds));
}

BOOL WINAPI Win32Bridge::Bridge::BridgeSetEndOfFile(HANDLE file)
{
    GuestStorageContext* storage = CurrentGuestStorageContext();
    if (!storage)
    {
        SetGuestLastError(ERROR_INVALID_HANDLE);
        return FALSE;
    }
    DWORD error = ERROR_SUCCESS;
    const BOOL result = storage->SetEndOfFile(file, &error) ? TRUE : FALSE;
    SetGuestLastError(error);
    return result;
}

HANDLE WINAPI Win32Bridge::Bridge::BridgeOpenEventW(
    DWORD desiredAccess,
    BOOL inheritHandle,
    LPCWSTR name)
{
    GuestKernelContext* kernel = CurrentGuestKernelContext();
    if (!kernel)
    {
        SetGuestLastError(ERROR_INVALID_HANDLE);
        return nullptr;
    }
    DWORD error = ERROR_SUCCESS;
    HANDLE handle = kernel->OpenEvent(desiredAccess, inheritHandle, name, &error);
    RuntimeDiagnostics::Record(
        handle
            ? L"KERNEL: opened named event '" + std::wstring(name ? name : L"") + L"'."
            : L"KERNEL: failed to open named event '" + std::wstring(name ? name : L"") +
                L"'; error " + std::to_wstring(error) + L".");
    SetGuestLastError(error);
    return handle;
}

HANDLE WINAPI Win32Bridge::Bridge::BridgeOpenFileMappingW(DWORD, BOOL, LPCWSTR name)
{
    if (!name || !*name)
    {
        SetGuestLastError(ERROR_INVALID_PARAMETER);
        return nullptr;
    }
    std::lock_guard<std::mutex> guard(g_fileMappingsLock);
    const auto found = g_namedFileMappings.find(FileMappingNameKey(name));
    if (found == g_namedFileMappings.end())
    {
        SetGuestLastError(ERROR_FILE_NOT_FOUND);
        return nullptr;
    }
    const std::shared_ptr<GuestFileMapping> mapping = found->second.lock();
    if (!mapping)
    {
        g_namedFileMappings.erase(found);
        SetGuestLastError(ERROR_FILE_NOT_FOUND);
        return nullptr;
    }
    const HANDLE handle = RegisterFileMappingLocked(mapping);
    SetGuestLastError(handle ? ERROR_SUCCESS : ERROR_TOO_MANY_OPEN_FILES);
    return handle;
}

LPVOID WINAPI Win32Bridge::Bridge::BridgeMapViewOfFile(
    HANDLE handle,
    DWORD desiredAccess,
    DWORD offsetHigh,
    DWORD offsetLow,
    SIZE_T bytesToMap)
{
    constexpr DWORD FileMapCopy = 0x00000001u;
    constexpr DWORD FileMapWrite = 0x00000002u;
    constexpr DWORD FileMapRead = 0x00000004u;
    constexpr DWORD FileMapAllAccess = 0x000f001fu;
    constexpr DWORD FileMapExecute = 0x00000020u;
    const auto fail = [=](DWORD error, const wchar_t* reason) -> LPVOID
    {
        RuntimeDiagnostics::Record(
            L"STORAGE: MapViewOfFile failed (" + std::wstring(reason) +
            L"); access " + std::to_wstring(desiredAccess) +
            L", offset-high " + std::to_wstring(offsetHigh) +
            L", offset-low " + std::to_wstring(offsetLow) +
            L", bytes " + std::to_wstring(bytesToMap) +
            L", error " + std::to_wstring(error) + L".");
        SetGuestLastError(error);
        return nullptr;
    };
    const std::shared_ptr<GuestFileMapping> mapping = LookupFileMapping(handle);
    if (!mapping)
    {
        return fail(ERROR_INVALID_HANDLE, L"unknown mapping handle");
    }

    const DWORD pageProtection = mapping->protection & 0xffu;
    const bool writableMapping = pageProtection == PAGE_READWRITE ||
        pageProtection == PAGE_WRITECOPY || pageProtection == PAGE_EXECUTE_READWRITE ||
        pageProtection == PAGE_EXECUTE_WRITECOPY;
    const bool executableMapping = pageProtection == PAGE_EXECUTE_READ ||
        pageProtection == PAGE_EXECUTE_READWRITE || pageProtection == PAGE_EXECUTE_WRITECOPY;
    // FILE_MAP_ALL_ACCESS contains the numeric FILE_MAP_COPY bit.  Treating
    // access as a simple bit mask therefore rejects valid read/write views.
    // COPY is a distinct view mode; ALL_ACCESS has normal write semantics.
    const bool allAccess = (desiredAccess & FileMapAllAccess) == FileMapAllAccess;
    const bool copyRequested = !allAccess && (desiredAccess & FileMapCopy) != 0;
    const bool writeRequested = allAccess || (desiredAccess & FileMapWrite) != 0;
    const bool readRequested = allAccess || (desiredAccess & FileMapRead) != 0 ||
        desiredAccess == 0;
    const bool executeRequested = (desiredAccess & FileMapExecute) != 0;
    const bool readableMapping = pageProtection == PAGE_READONLY || writableMapping ||
        executableMapping;
    if (writeRequested && !writableMapping)
    {
        return fail(ERROR_ACCESS_DENIED, L"write access is incompatible with protection");
    }
    if ((readRequested || copyRequested) && !readableMapping)
    {
        return fail(ERROR_ACCESS_DENIED, L"read access is incompatible with protection");
    }
    if (executeRequested && !executableMapping)
    {
        return fail(ERROR_ACCESS_DENIED, L"execute access is incompatible with protection");
    }

    ULARGE_INTEGER offset{};
    offset.HighPart = offsetHigh;
    offset.LowPart = offsetLow;
    SYSTEM_INFO systemInfo{};
    BridgeGetSystemInfo(&systemInfo);
    const DWORD granularity = systemInfo.dwAllocationGranularity
        ? systemInfo.dwAllocationGranularity
        : 65536;
    if ((offset.QuadPart % granularity) != 0)
    {
        return fail(ERROR_MAPPED_ALIGNMENT, L"unaligned file offset");
    }
    if (offset.QuadPart >= mapping->size)
    {
        return fail(ERROR_INVALID_PARAMETER, L"file offset exceeds mapping size");
    }
    const ULONGLONG remaining = mapping->size - offset.QuadPart;
    const SIZE_T viewSize = bytesToMap == 0
        ? static_cast<SIZE_T>(remaining)
        : bytesToMap;
    if (viewSize == 0 || viewSize > remaining)
    {
        return fail(ERROR_INVALID_PARAMETER, L"view size exceeds mapping size");
    }

    BYTE* address = mapping->memory + static_cast<SIZE_T>(offset.QuadPart);
    std::shared_ptr<void> privateAllocation;
    if (copyRequested)
    {
        void* copy = VirtualAllocFromApp(
            nullptr, viewSize, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
        if (!copy)
        {
            return fail(ERROR_NOT_ENOUGH_MEMORY, L"private view allocation failed");
        }
        memcpy(copy, address, viewSize);
        try
        {
            privateAllocation = std::shared_ptr<void>(copy, [](void* allocation)
            {
                if (allocation) ::VirtualFree(allocation, 0, MEM_RELEASE);
            });
        }
        catch (const std::bad_alloc&)
        {
            ::VirtualFree(copy, 0, MEM_RELEASE);
            return fail(ERROR_NOT_ENOUGH_MEMORY, L"private view registration failed");
        }
        address = static_cast<BYTE*>(copy);
    }
    {
        std::lock_guard<std::mutex> guard(g_fileMappingsLock);
        GuestMappingView& view = g_fileMappingViews[address];
        view.mapping = mapping;
        view.privateAllocation = std::move(privateAllocation);
        view.size = viewSize;
        ++view.references;
    }
    RuntimeDiagnostics::Record(
        L"STORAGE: mapped a " + std::to_wstring(viewSize) +
        L"-byte guest file view with access " + std::to_wstring(desiredAccess) + L".");
    SetGuestLastError(ERROR_SUCCESS);
    return address;
}

BOOL WINAPI Win32Bridge::Bridge::BridgeUnmapViewOfFile(LPCVOID baseAddress)
{
    if (!baseAddress)
    {
        SetGuestLastError(ERROR_INVALID_ADDRESS);
        return FALSE;
    }
    std::lock_guard<std::mutex> guard(g_fileMappingsLock);
    const auto found = g_fileMappingViews.find(baseAddress);
    if (found == g_fileMappingViews.end())
    {
        SetGuestLastError(ERROR_INVALID_ADDRESS);
        return FALSE;
    }
    if (found->second.references > 1) --found->second.references;
    else g_fileMappingViews.erase(found);
    SetGuestLastError(ERROR_SUCCESS);
    return TRUE;
}

BOOL WINAPI Win32Bridge::Bridge::BridgeGetFileInformationByHandle(HANDLE file, LPBY_HANDLE_FILE_INFORMATION information)
{
    if (!information)
    {
        SetGuestLastError(ERROR_INVALID_PARAMETER);
        return FALSE;
    }
    LARGE_INTEGER size{};
    if (!BridgeGetFileSizeEx(file, &size))
    {
        return FALSE;
    }
    ZeroMemory(information, sizeof(*information));
    information->dwFileAttributes = FILE_ATTRIBUTE_NORMAL;
    information->nFileSizeHigh = size.HighPart;
    information->nFileSizeLow = size.LowPart;
    information->nNumberOfLinks = 1;
    SetGuestLastError(ERROR_SUCCESS);
    return TRUE;
}

BOOL WINAPI Win32Bridge::Bridge::BridgeSetFileTime(HANDLE file, const FILETIME*, const FILETIME*, const FILETIME*)
{
    LARGE_INTEGER unused{};
    if (!BridgeGetFileSizeEx(file, &unused))
    {
        return FALSE;
    }
    // LocalFolder deliberately has no host timestamp exposure.  Accepting the
    // request preserves common archive-extraction behavior without leaking a
    // host path or timestamp implementation into the guest.
    SetGuestLastError(ERROR_SUCCESS);
    return TRUE;
}

BOOL WINAPI Win32Bridge::Bridge::BridgeFileTimeToDosDateTime(const FILETIME* fileTime, LPWORD date, LPWORD time)
{
    if (!fileTime || !date || !time)
    {
        SetGuestLastError(ERROR_INVALID_PARAMETER);
        return FALSE;
    }
    // DOS timestamps are metadata only in the virtual drive.  A stable zero
    // timestamp is preferable to calling the desktop-only API from UWP.
    *date = 0;
    *time = 0;
    SetGuestLastError(ERROR_SUCCESS);
    return TRUE;
}

BOOL WINAPI Win32Bridge::Bridge::BridgeFileTimeToLocalFileTime(const FILETIME* fileTime, LPFILETIME localFileTime)
{
    const BOOL result = ::FileTimeToLocalFileTime(fileTime, localFileTime);
    SetGuestLastError(result ? ERROR_SUCCESS : ::GetLastError());
    return result;
}

BOOL WINAPI Win32Bridge::Bridge::BridgeLocalFileTimeToFileTime(const FILETIME* localFileTime, LPFILETIME fileTime)
{
    if (!localFileTime || !fileTime)
    {
        SetGuestLastError(ERROR_INVALID_PARAMETER);
        return FALSE;
    }
    *fileTime = *localFileTime;
    SetGuestLastError(ERROR_SUCCESS);
    return TRUE;
}

BOOL WINAPI Win32Bridge::Bridge::BridgeDosDateTimeToFileTime(WORD date, WORD time, LPFILETIME fileTime)
{
    if (!fileTime)
    {
        SetGuestLastError(ERROR_INVALID_PARAMETER);
        return FALSE;
    }
    fileTime->dwLowDateTime = static_cast<DWORD>(time) | (static_cast<DWORD>(date) << 16);
    fileTime->dwHighDateTime = 0;
    SetGuestLastError(ERROR_SUCCESS);
    return TRUE;
}

UINT WINAPI Win32Bridge::Bridge::BridgeGetOEMCP()
{
    return 437;
}

BOOL WINAPI Win32Bridge::Bridge::BridgeFileTimeToSystemTime(const FILETIME* fileTime, LPSYSTEMTIME systemTime)
{
    const BOOL result = ::FileTimeToSystemTime(fileTime, systemTime);
    SetGuestLastError(result ? ERROR_SUCCESS : ::GetLastError());
    return result;
}

LONG WINAPI Win32Bridge::Bridge::BridgeCompareFileTime(const FILETIME* first, const FILETIME* second)
{
    if (!first || !second)
    {
        SetGuestLastError(ERROR_INVALID_PARAMETER);
        return 0;
    }
    ULARGE_INTEGER a{};
    ULARGE_INTEGER b{};
    a.LowPart = first->dwLowDateTime;
    a.HighPart = first->dwHighDateTime;
    b.LowPart = second->dwLowDateTime;
    b.HighPart = second->dwHighDateTime;
    return a.QuadPart < b.QuadPart ? -1 : (a.QuadPart > b.QuadPart ? 1 : 0);
}

BOOL WINAPI Win32Bridge::Bridge::BridgeGetProcessTimes(HANDLE, LPFILETIME creation, LPFILETIME exit, LPFILETIME kernel, LPFILETIME user)
{
    FILETIME now{};
    ::GetSystemTimeAsFileTime(&now);
    if (creation) *creation = now;
    if (exit) ZeroMemory(exit, sizeof(*exit));
    if (kernel) ZeroMemory(kernel, sizeof(*kernel));
    if (user) ZeroMemory(user, sizeof(*user));
    SetGuestLastError(ERROR_SUCCESS);
    return TRUE;
}

BOOL WINAPI Win32Bridge::Bridge::BridgeGetProcessAffinityMask(HANDLE, PDWORD_PTR processMask, PDWORD_PTR systemMask)
{
    if (!processMask || !systemMask)
    {
        SetGuestLastError(ERROR_INVALID_PARAMETER);
        return FALSE;
    }
    *processMask = 1;
    *systemMask = 1;
    SetGuestLastError(ERROR_SUCCESS);
    return TRUE;
}

DWORD WINAPI Win32Bridge::Bridge::BridgeResumeThread(HANDLE)
{
    SetGuestLastError(ERROR_INVALID_HANDLE);
    return static_cast<DWORD>(-1);
}

BOOL WINAPI Win32Bridge::Bridge::BridgeTerminateProcess(HANDLE process, UINT)
{
    if (process != BridgeGetCurrentProcess())
    {
        SetGuestLastError(ERROR_INVALID_HANDLE);
        return FALSE;
    }
    // The host owns process lifetime.  Guest code can still return from its
    // entry point; terminating the UWP host here would be unsafe.
    SetGuestLastError(ERROR_SUCCESS);
    return TRUE;
}

BOOL WINAPI Win32Bridge::Bridge::BridgeDeviceIoControl(HANDLE, DWORD, LPVOID, DWORD, LPVOID, DWORD, LPDWORD bytesReturned, LPOVERLAPPED)
{
    if (bytesReturned) *bytesReturned = 0;
    SetGuestLastError(ERROR_INVALID_FUNCTION);
    return FALSE;
}

HANDLE WINAPI Win32Bridge::Bridge::BridgeFindFirstStreamW(LPCWSTR, DWORD, LPVOID, DWORD)
{
    SetGuestLastError(ERROR_HANDLE_EOF);
    return INVALID_HANDLE_VALUE;
}

BOOL WINAPI Win32Bridge::Bridge::BridgeFindNextStreamW(HANDLE, LPVOID)
{
    SetGuestLastError(ERROR_NO_MORE_FILES);
    return FALSE;
}

BOOL WINAPI Win32Bridge::Bridge::BridgeCreateHardLinkW(
    LPCWSTR newFileName,
    LPCWSTR existingFileName,
    LPSECURITY_ATTRIBUTES)
{
    RuntimeDiagnostics::Record(
        L"STORAGE: CreateHardLinkW is not implemented (existing '" +
        std::wstring(existingFileName ? existingFileName : L"") + L"', new '" +
        std::wstring(newFileName ? newFileName : L"") + L"').");
    SetGuestLastError(ERROR_NOT_SUPPORTED);
    return FALSE;
}

BOOL WINAPI Win32Bridge::Bridge::BridgeMoveFileWithProgressW(LPCWSTR existingFileName, LPCWSTR newFileName, LPPROGRESS_ROUTINE, LPVOID, DWORD flags)
{
    return BridgeMoveFileExW(existingFileName, newFileName, flags);
}

DWORD WINAPI Win32Bridge::Bridge::BridgeFormatMessageW(DWORD flags, LPCVOID, DWORD messageId, DWORD, LPWSTR buffer, DWORD size, va_list*)
{
    wchar_t message[64]{};
    const int length = _snwprintf_s(message, _countof(message), _TRUNCATE, L"Win32Bridge error %lu", messageId);
    if (length < 0) return 0;
    const size_t needed = static_cast<size_t>(length) + 1;
    if (flags & FORMAT_MESSAGE_ALLOCATE_BUFFER)
    {
        if (!buffer) return 0;
        LPWSTR allocated = static_cast<LPWSTR>(BridgeLocalAlloc(LMEM_FIXED, needed * sizeof(wchar_t)));
        if (!allocated) return 0;
        memcpy(allocated, message, needed * sizeof(wchar_t));
        *reinterpret_cast<LPWSTR*>(buffer) = allocated;
        return static_cast<DWORD>(length);
    }
    if (!buffer || size < needed)
    {
        SetGuestLastError(ERROR_INSUFFICIENT_BUFFER);
        return 0;
    }
    memcpy(buffer, message, needed * sizeof(wchar_t));
    return static_cast<DWORD>(length);
}

HLOCAL WINAPI Win32Bridge::Bridge::BridgeLocalFree(HLOCAL memory)
{
    return reinterpret_cast<HLOCAL>(BridgeGlobalFree(reinterpret_cast<HGLOBAL>(memory)));
}

HLOCAL WINAPI Win32Bridge::Bridge::BridgeLocalAlloc(UINT flags, SIZE_T bytes)
{
    return reinterpret_cast<HLOCAL>(BridgeGlobalAlloc(flags, bytes));
}

LONG WINAPI Win32Bridge::Bridge::BridgeUnhandledExceptionFilter(EXCEPTION_POINTERS*)
{
    return EXCEPTION_EXECUTE_HANDLER;
}

LPTOP_LEVEL_EXCEPTION_FILTER WINAPI Win32Bridge::Bridge::BridgeSetUnhandledExceptionFilter(LPTOP_LEVEL_EXCEPTION_FILTER filter)
{
    static std::atomic<LPTOP_LEVEL_EXCEPTION_FILTER> current{ nullptr };
    return current.exchange(filter);
}

PRUNTIME_FUNCTION WINAPI Win32Bridge::Bridge::BridgeRtlLookupFunctionEntry(
    DWORD64 controlPc,
    PDWORD64 imageBase,
    PUNWIND_HISTORY_TABLE historyTable)
{
    return ::RtlLookupFunctionEntry(controlPc, imageBase, historyTable);
}

PEXCEPTION_ROUTINE WINAPI Win32Bridge::Bridge::BridgeRtlVirtualUnwind(
    DWORD handlerType,
    DWORD64 imageBase,
    DWORD64 controlPc,
    PRUNTIME_FUNCTION functionEntry,
    PCONTEXT contextRecord,
    PVOID* handlerData,
    PDWORD64 establisherFrame,
    PKNONVOLATILE_CONTEXT_POINTERS contextPointers)
{
    return ::RtlVirtualUnwind(
        handlerType,
        imageBase,
        controlPc,
        functionEntry,
        contextRecord,
        handlerData,
        establisherFrame,
        contextPointers);
}

void WINAPI Win32Bridge::Bridge::BridgeRtlCaptureContext(PCONTEXT contextRecord)
{
    if (contextRecord) ::RtlCaptureContext(contextRecord);
}

HANDLE WINAPI Win32Bridge::Bridge::BridgeOpenProcess(DWORD, BOOL, DWORD processId)
{
    if (processId == BridgeGetCurrentProcessId()) return BridgeGetCurrentProcess();
    SetGuestLastError(ERROR_INVALID_PARAMETER);
    return nullptr;
}
HANDLE WINAPI Win32Bridge::Bridge::BridgeCreateToolhelp32Snapshot(DWORD, DWORD)
{
    SetGuestLastError(ERROR_NOT_SUPPORTED);
    return INVALID_HANDLE_VALUE;
}
BOOL WINAPI Win32Bridge::Bridge::BridgeProcess32FirstW(HANDLE, LPVOID)
{
    SetGuestLastError(ERROR_NO_MORE_FILES);
    return FALSE;
}
BOOL WINAPI Win32Bridge::Bridge::BridgeProcess32NextW(HANDLE, LPVOID)
{
    SetGuestLastError(ERROR_NO_MORE_FILES);
    return FALSE;
}
BOOL WINAPI Win32Bridge::Bridge::BridgeCopyFileExW(
    LPCWSTR existingFileName,
    LPCWSTR newFileName,
    LPPROGRESS_ROUTINE,
    LPVOID,
    LPBOOL cancel,
    DWORD flags)
{
    if (cancel)
    {
        *cancel = FALSE;
    }
    if (!existingFileName || !newFileName)
    {
        SetGuestLastError(ERROR_INVALID_PARAMETER);
        return FALSE;
    }

    // Copy through the bridge's file handles rather than a host path.  This
    // keeps the operation inside LocalFolder\drive_c and gives applications the
    // normal CopyFileExW behavior it uses for basic file management.
    constexpr DWORD CopyFileFailIfExists = 0x00000001;
    const HANDLE source = BridgeCreateFileW(
        existingFileName,
        GENERIC_READ,
        FILE_SHARE_READ,
        nullptr,
        OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL,
        nullptr);
    if (source == INVALID_HANDLE_VALUE)
    {
        return FALSE;
    }
    const HANDLE destination = BridgeCreateFileW(
        newFileName,
        GENERIC_WRITE,
        0,
        nullptr,
        (flags & CopyFileFailIfExists) != 0 ? CREATE_NEW : CREATE_ALWAYS,
        FILE_ATTRIBUTE_NORMAL,
        nullptr);
    if (destination == INVALID_HANDLE_VALUE)
    {
        const DWORD error = BridgeGetLastError();
        BridgeCloseHandle(source);
        SetGuestLastError(error);
        return FALSE;
    }

    RuntimeDiagnostics::Record(L"STORAGE: CopyFileExW copying inside the virtual C: drive.");
    std::vector<BYTE> buffer(64 * 1024);
    DWORD failure = ERROR_SUCCESS;
    bool copied = true;
    for (;;)
    {
        if (cancel && *cancel)
        {
            copied = false;
            failure = ERROR_REQUEST_ABORTED;
            break;
        }
        DWORD read = 0;
        if (!BridgeReadFile(source, buffer.data(), static_cast<DWORD>(buffer.size()), &read, nullptr))
        {
            copied = false;
            failure = BridgeGetLastError();
            break;
        }
        if (read == 0)
        {
            break;
        }
        DWORD writtenTotal = 0;
        while (writtenTotal < read)
        {
            DWORD written = 0;
            if (!BridgeWriteFile(
                destination,
                buffer.data() + writtenTotal,
                read - writtenTotal,
                &written,
                nullptr) || written == 0)
            {
                copied = false;
                failure = written == 0 ? ERROR_WRITE_FAULT : BridgeGetLastError();
                break;
            }
            writtenTotal += written;
        }
        if (!copied)
        {
            break;
        }
    }

    if (copied && !BridgeFlushFileBuffers(destination))
    {
        copied = false;
        failure = BridgeGetLastError();
    }
    BridgeCloseHandle(destination);
    BridgeCloseHandle(source);
    if (!copied)
    {
        SetGuestLastError(failure == ERROR_SUCCESS ? ERROR_GEN_FAILURE : failure);
        RuntimeDiagnostics::Record(L"STORAGE: CopyFileExW failed in the virtual C: drive.");
        return FALSE;
    }
    SetGuestLastError(ERROR_SUCCESS);
    RuntimeDiagnostics::Record(L"STORAGE: CopyFileExW completed in the virtual C: drive.");
    return TRUE;
}
DWORD WINAPI Win32Bridge::Bridge::BridgeGetCompressedFileSizeW(LPCWSTR fileName, LPDWORD fileSizeHigh)
{
    HANDLE file = BridgeCreateFileW(fileName, GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return INVALID_FILE_SIZE;
    const DWORD size = BridgeGetFileSize(file, fileSizeHigh);
    BridgeCloseHandle(file);
    return size;
}
HANDLE WINAPI Win32Bridge::Bridge::BridgeFindFirstChangeNotificationW(LPCWSTR path, BOOL, DWORD)
{
    GuestStorageContext* storage = CurrentStorageOrFail();
    GuestKernelContext* kernel = CurrentKernelOrFail();
    if (!storage || !kernel || !path)
    {
        return INVALID_HANDLE_VALUE;
    }

    // File managers commonly register a directory watcher for each panel. UWP's
    // LocalFolder does not expose the desktop notification primitive, but an
    // unsignalled guest-local event has the same non-error, waitable contract.
    // File operations performed through the bridge explicitly refresh their
    // views, so this is sufficient until an asynchronous LocalFolder watcher
    // is added.
    DWORD error = ERROR_SUCCESS;
    const DWORD attributes = storage->GetGuestFileAttributes(path, &error);
    if (attributes == INVALID_FILE_ATTRIBUTES || (attributes & FILE_ATTRIBUTE_DIRECTORY) == 0)
    {
        SetGuestLastError(error == ERROR_SUCCESS ? ERROR_PATH_NOT_FOUND : error);
        return INVALID_HANDLE_VALUE;
    }
    HANDLE notification = kernel->CreateEvent(false, false, nullptr, &error);
    SetGuestLastError(error);
    if (notification)
    {
        RuntimeDiagnostics::Record(L"STORAGE: created a guest-local directory change notification.");
    }
    return notification ? notification : INVALID_HANDLE_VALUE;
}
BOOL WINAPI Win32Bridge::Bridge::BridgeFindNextChangeNotification(HANDLE changeHandle)
{
    GuestKernelContext* kernel = CurrentKernelOrFail();
    if (!kernel)
    {
        return FALSE;
    }
    DWORD error = ERROR_SUCCESS;
    const bool reset = kernel->ResetEvent(changeHandle, &error);
    SetGuestLastError(error);
    return reset ? TRUE : FALSE;
}
BOOL WINAPI Win32Bridge::Bridge::BridgeFindCloseChangeNotification(HANDLE changeHandle)
{
    GuestKernelContext* kernel = CurrentKernelOrFail();
    if (!kernel)
    {
        return FALSE;
    }
    DWORD error = ERROR_SUCCESS;
    const bool closed = kernel->CloseHandle(changeHandle, &error);
    SetGuestLastError(error);
    return closed ? TRUE : FALSE;
}
HANDLE WINAPI Win32Bridge::Bridge::BridgeCreateFileMappingW(
    HANDLE file,
    LPSECURITY_ATTRIBUTES,
    DWORD protection,
    DWORD maximumSizeHigh,
    DWORD maximumSizeLow,
    LPCWSTR name)
{
    const DWORD pageProtection = protection & 0xffu;
    if (pageProtection != PAGE_READONLY && pageProtection != PAGE_READWRITE &&
        pageProtection != PAGE_WRITECOPY && pageProtection != PAGE_EXECUTE_READ &&
        pageProtection != PAGE_EXECUTE_READWRITE &&
        pageProtection != PAGE_EXECUTE_WRITECOPY)
    {
        SetGuestLastError(ERROR_INVALID_PARAMETER);
        return nullptr;
    }

    const std::wstring nameKey = FileMappingNameKey(name);
    if (!nameKey.empty())
    {
        std::lock_guard<std::mutex> guard(g_fileMappingsLock);
        const auto named = g_namedFileMappings.find(nameKey);
        if (named != g_namedFileMappings.end())
        {
            const std::shared_ptr<GuestFileMapping> existing = named->second.lock();
            if (existing)
            {
                const HANDLE handle = RegisterFileMappingLocked(existing);
                SetGuestLastError(handle ? ERROR_ALREADY_EXISTS : ERROR_TOO_MANY_OPEN_FILES);
                return handle;
            }
            g_namedFileMappings.erase(named);
        }
    }

    ULARGE_INTEGER requestedSize{};
    requestedSize.HighPart = maximumSizeHigh;
    requestedSize.LowPart = maximumSizeLow;
    ULONGLONG mappingSize = requestedSize.QuadPart;
    ULONGLONG sourceSize = 0;
    GuestStorageContext* storage = nullptr;
    DWORD error = ERROR_SUCCESS;
    LARGE_INTEGER originalPosition{};
    bool restorePosition = false;

    if (file == INVALID_HANDLE_VALUE)
    {
        if (mappingSize == 0)
        {
            SetGuestLastError(ERROR_INVALID_PARAMETER);
            return nullptr;
        }
    }
    else
    {
        storage = CurrentGuestStorageContext();
        LARGE_INTEGER size{};
        if (!storage || !storage->GetFileSize(file, &size, &error) || size.QuadPart < 0)
        {
            SetGuestLastError(storage ? error : ERROR_INVALID_HANDLE);
            return nullptr;
        }
        sourceSize = static_cast<ULONGLONG>(size.QuadPart);
        if (mappingSize == 0) mappingSize = sourceSize;
        if (mappingSize == 0)
        {
            SetGuestLastError(ERROR_FILE_INVALID);
            return nullptr;
        }
    }

    if (mappingSize > static_cast<ULONGLONG>((std::numeric_limits<SIZE_T>::max)()))
    {
        SetGuestLastError(ERROR_NOT_ENOUGH_MEMORY);
        return nullptr;
    }

    std::shared_ptr<GuestFileMapping> mapping;
    try
    {
        mapping = std::make_shared<GuestFileMapping>();
        mapping->size = static_cast<SIZE_T>(mappingSize);
        mapping->protection = protection;
        mapping->name = name ? name : L"";
    }
    catch (const std::bad_alloc&)
    {
        SetGuestLastError(ERROR_NOT_ENOUGH_MEMORY);
        return nullptr;
    }
    mapping->memory = static_cast<BYTE*>(VirtualAllocFromApp(
        nullptr, mapping->size, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
    if (!mapping->memory)
    {
        SetGuestLastError(ERROR_NOT_ENOUGH_MEMORY);
        return nullptr;
    }

    if (storage && sourceSize != 0)
    {
        LARGE_INTEGER zero{};
        if (!storage->SetFilePointer(file, zero, &originalPosition, FILE_CURRENT, &error) ||
            !storage->SetFilePointer(file, zero, nullptr, FILE_BEGIN, &error))
        {
            SetGuestLastError(error);
            return nullptr;
        }
        restorePosition = true;
        const ULONGLONG bytesToRead = (std::min)(sourceSize, mappingSize);
        ULONGLONG offset = 0;
        while (offset < bytesToRead)
        {
            const DWORD request = static_cast<DWORD>((std::min)(
                bytesToRead - offset, static_cast<ULONGLONG>(8 * 1024 * 1024)));
            DWORD received = 0;
            if (!storage->ReadFile(file, mapping->memory + static_cast<SIZE_T>(offset),
                    request, &received, &error) || received == 0)
            {
                if (received == 0 && error == ERROR_SUCCESS) error = ERROR_READ_FAULT;
                LARGE_INTEGER ignored{};
                storage->SetFilePointer(file, originalPosition, &ignored, FILE_BEGIN, nullptr);
                SetGuestLastError(error);
                return nullptr;
            }
            offset += received;
        }
    }
    if (restorePosition)
    {
        LARGE_INTEGER ignored{};
        if (!storage->SetFilePointer(file, originalPosition, &ignored, FILE_BEGIN, &error))
        {
            SetGuestLastError(error);
            return nullptr;
        }
    }

    HANDLE handle = nullptr;
    {
        std::lock_guard<std::mutex> guard(g_fileMappingsLock);
        handle = RegisterFileMappingLocked(mapping);
        if (handle && !nameKey.empty()) g_namedFileMappings[nameKey] = mapping;
    }
    if (!handle)
    {
        SetGuestLastError(ERROR_TOO_MANY_OPEN_FILES);
        return nullptr;
    }
    RuntimeDiagnostics::Record(
        L"STORAGE: created a " + std::to_wstring(mapping->size) +
        L"-byte guest file mapping.");
    SetGuestLastError(ERROR_SUCCESS);
    return handle;
}
BOOL WINAPI Win32Bridge::Bridge::BridgeCreateProcessW(
    LPCWSTR applicationName,
    LPWSTR commandLine,
    LPSECURITY_ATTRIBUTES processAttributes,
    LPSECURITY_ATTRIBUTES threadAttributes,
    BOOL inheritHandles,
    DWORD creationFlags,
    LPVOID environment,
    LPCWSTR currentDirectory,
    LPSTARTUPINFOW startupInfo,
    LPPROCESS_INFORMATION processInformation)
{
    if (processInformation) ZeroMemory(processInformation, sizeof(*processInformation));
    if (!processInformation || !startupInfo || startupInfo->cb < sizeof(STARTUPINFOW))
    {
        SetGuestLastError(ERROR_INVALID_PARAMETER);
        return FALSE;
    }
    if ((processAttributes && processAttributes->nLength < sizeof(SECURITY_ATTRIBUTES)) ||
        (threadAttributes && threadAttributes->nLength < sizeof(SECURITY_ATTRIBUTES)))
    {
        SetGuestLastError(ERROR_INVALID_PARAMETER);
        return FALSE;
    }
    if (environment)
    {
        SetGuestLastError(ERROR_NOT_SUPPORTED);
        return FALSE;
    }
    constexpr DWORD StartfUseStdHandles = 0x00000100;
    if (inheritHandles && (startupInfo->dwFlags & StartfUseStdHandles) != 0 &&
        (startupInfo->hStdInput || startupInfo->hStdOutput || startupInfo->hStdError))
    {
        SetGuestLastError(ERROR_NOT_SUPPORTED);
        return FALSE;
    }

    constexpr DWORD unsupportedFlags = DEBUG_PROCESS | DEBUG_ONLY_THIS_PROCESS |
        CREATE_SUSPENDED | CREATE_BREAKAWAY_FROM_JOB | EXTENDED_STARTUPINFO_PRESENT;
    if ((creationFlags & unsupportedFlags) != 0)
    {
        RuntimeDiagnostics::Record(
            L"PROCESS: unsupported CreateProcessW flags " +
            std::to_wstring(creationFlags) + L".");
        SetGuestLastError(ERROR_NOT_SUPPORTED);
        return FALSE;
    }

    const std::wstring fullCommandLine = commandLine ? commandLine : L"";
    std::vector<std::wstring> executableCandidates;
    if (applicationName && *applicationName)
    {
        executableCandidates.emplace_back(applicationName);
    }
    else
    {
        size_t cursor = fullCommandLine.find_first_not_of(L" \t");
        if (cursor == std::wstring::npos)
        {
            SetGuestLastError(ERROR_INVALID_PARAMETER);
            return FALSE;
        }
        if (fullCommandLine[cursor] == L'\"')
        {
            const size_t end = fullCommandLine.find(L'\"', cursor + 1);
            if (end == std::wstring::npos)
            {
                SetGuestLastError(ERROR_INVALID_PARAMETER);
                return FALSE;
            }
            executableCandidates.emplace_back(
                fullCommandLine.substr(cursor + 1, end - cursor - 1));
        }
        else
        {
            // CreateProcess probes each whitespace-delimited prefix when the
            // executable is not quoted.  This preserves the Win32 ambiguity
            // rules for paths such as "C:\\Program Files\\tool arg" rather
            // than assuming that the first token is always the image name.
            size_t end = cursor;
            while (end < fullCommandLine.size())
            {
                end = fullCommandLine.find_first_of(L" \t", end);
                const size_t candidateEnd = end == std::wstring::npos
                    ? fullCommandLine.size() : end;
                if (candidateEnd > cursor)
                    executableCandidates.emplace_back(
                        fullCommandLine.substr(cursor, candidateEnd - cursor));
                if (end == std::wstring::npos) break;
                end = fullCommandLine.find_first_not_of(L" \t", end);
                if (end == std::wstring::npos) break;
            }
        }
    }
    for (std::wstring& candidate : executableCandidates)
    {
        const size_t separator = candidate.find_last_of(L"\\/");
        const size_t dot = candidate.find_last_of(L'.');
        if (dot == std::wstring::npos ||
            (separator != std::wstring::npos && dot < separator))
        {
            candidate += L".exe";
        }
    }

    GuestRuntime* runtime = CurrentGuestRuntime();
    if (!runtime)
    {
        SetGuestLastError(ERROR_INVALID_FUNCTION);
        return FALSE;
    }
    if (currentDirectory)
    {
        GuestStorageContext* storage = CurrentGuestStorageContext();
        DWORD directoryError = ERROR_SUCCESS;
        const DWORD attributes = storage
            ? storage->GetGuestFileAttributes(currentDirectory, &directoryError)
            : INVALID_FILE_ATTRIBUTES;
        if (attributes == INVALID_FILE_ATTRIBUTES || (attributes & FILE_ATTRIBUTE_DIRECTORY) == 0)
        {
            SetGuestLastError(directoryError == ERROR_SUCCESS ? ERROR_DIRECTORY : directoryError);
            return FALSE;
        }
    }
    DWORD error = ERROR_FILE_NOT_FOUND;
    for (const std::wstring& executable : executableCandidates)
    {
        const std::wstring effectiveCommandLine = fullCommandLine.empty()
            ? L"\"" + executable + L"\""
            : fullCommandLine;
        if (runtime->LaunchChildProcess(
            executable.c_str(), effectiveCommandLine.c_str(), currentDirectory,
            processInformation, &error))
        {
            SetGuestLastError(ERROR_SUCCESS);
            return TRUE;
        }
        if (applicationName || (error != ERROR_FILE_NOT_FOUND &&
            error != ERROR_PATH_NOT_FOUND && error != ERROR_MOD_NOT_FOUND))
        {
            break;
        }
    }
    SetGuestLastError(error);
    return FALSE;
}

BOOL WINAPI Win32Bridge::Bridge::BridgeCreateProcessA(
    LPCSTR applicationName, LPSTR commandLine, LPSECURITY_ATTRIBUTES processAttributes,
    LPSECURITY_ATTRIBUTES threadAttributes, BOOL inheritHandles, DWORD creationFlags,
    LPVOID environment, LPCSTR currentDirectory, LPSTARTUPINFOA startupInfo,
    LPPROCESS_INFORMATION processInformation)
{
    if (processInformation) ZeroMemory(processInformation, sizeof(*processInformation));
    if (!startupInfo || startupInfo->cb < sizeof(STARTUPINFOA))
    {
        SetGuestLastError(ERROR_INVALID_PARAMETER);
        return FALSE;
    }
    const auto widen = [](LPCSTR value, std::wstring* output) -> bool
    {
        output->clear();
        if (!value) return true;
        const int required = MultiByteToWideChar(CP_ACP, MB_ERR_INVALID_CHARS, value, -1, nullptr, 0);
        if (required <= 0) return false;
        output->resize(static_cast<size_t>(required));
        if (MultiByteToWideChar(CP_ACP, MB_ERR_INVALID_CHARS,
            value, -1, &(*output)[0], required) != required) return false;
        output->resize(static_cast<size_t>(required - 1));
        return true;
    };
    std::wstring application, command, directory;
    if (!widen(applicationName, &application) || !widen(commandLine, &command) ||
        !widen(currentDirectory, &directory))
    {
        SetGuestLastError(ERROR_NO_UNICODE_TRANSLATION);
        return FALSE;
    }
    STARTUPINFOW wide{};
    wide.cb = sizeof(wide);
    wide.dwFlags = startupInfo->dwFlags;
    wide.wShowWindow = startupInfo->wShowWindow;
    wide.hStdInput = startupInfo->hStdInput;
    wide.hStdOutput = startupInfo->hStdOutput;
    wide.hStdError = startupInfo->hStdError;
    std::vector<wchar_t> mutableCommand(command.begin(), command.end());
    mutableCommand.push_back(L'\0');
    return BridgeCreateProcessW(
        applicationName ? application.c_str() : nullptr,
        commandLine ? mutableCommand.data() : nullptr,
        processAttributes, threadAttributes, inheritHandles, creationFlags,
        environment, currentDirectory ? directory.c_str() : nullptr,
        &wide, processInformation);
}

BOOL WINAPI Win32Bridge::Bridge::BridgeGetExitCodeProcess(HANDLE process, LPDWORD exitCode)
{
    GuestKernelContext* kernel = CurrentGuestKernelContext();
    DWORD error = ERROR_INVALID_HANDLE;
    const BOOL result = kernel && kernel->GetExitCode(process, true, exitCode, &error) ? TRUE : FALSE;
    SetGuestLastError(error);
    return result;
}

BOOL WINAPI Win32Bridge::Bridge::BridgeGetExitCodeThread(HANDLE thread, LPDWORD exitCode)
{
    GuestKernelContext* kernel = CurrentGuestKernelContext();
    DWORD error = ERROR_INVALID_HANDLE;
    const BOOL result = kernel && kernel->GetExitCode(thread, false, exitCode, &error) ? TRUE : FALSE;
    SetGuestLastError(error);
    return result;
}

HANDLE WINAPI Win32Bridge::Bridge::BridgeCreateThread(
    LPSECURITY_ATTRIBUTES,
    SIZE_T stackSize,
    LPTHREAD_START_ROUTINE startAddress,
    LPVOID parameter,
    DWORD creationFlags,
    LPDWORD threadId)
{
    GuestRuntime* runtime = CurrentGuestRuntime();
    if (!runtime)
    {
        if (threadId) *threadId = 0;
        SetGuestLastError(ERROR_INVALID_FUNCTION);
        return nullptr;
    }
    DWORD error = ERROR_SUCCESS;
    HANDLE handle = runtime->LaunchThread(
        stackSize, startAddress, parameter, creationFlags, threadId, &error);
    SetGuestLastError(error);
    return handle;
}
void WINAPI Win32Bridge::Bridge::BridgeRaiseException(DWORD, DWORD, DWORD, const ULONG_PTR*)
{
    // Guest exceptions require PE unwind metadata registration. Until that
    // runtime slice is present, avoid unwinding through host frames.
    SetGuestLastError(ERROR_NOT_SUPPORTED);
}

ImportResolution Win32Bridge::Bridge::ResolveKernel32Import(const ImportedSymbol& symbol)
{
    auto resolution = CompatibilityCatalog::Resolve(symbol);
    if (symbol.importedByOrdinal || !IsKernelLibrary(symbol.library))
    {
        return resolution;
    }

    if (_wcsicmp(symbol.name.c_str(), L"createfilew") == 0)
        resolution.targetAddress = reinterpret_cast<ULONGLONG>(&BridgeCreateFileW);
    else if (_wcsicmp(symbol.name.c_str(), L"readfile") == 0)
        resolution.targetAddress = reinterpret_cast<ULONGLONG>(&BridgeReadFile);
    else if (_wcsicmp(symbol.name.c_str(), L"writefile") == 0)
        resolution.targetAddress = reinterpret_cast<ULONGLONG>(&BridgeWriteFile);
    else if (_wcsicmp(symbol.name.c_str(), L"closehandle") == 0)
        resolution.targetAddress = reinterpret_cast<ULONGLONG>(&BridgeCloseHandle);
    else if (_wcsicmp(symbol.name.c_str(), L"getfilesizeex") == 0)
        resolution.targetAddress = reinterpret_cast<ULONGLONG>(&BridgeGetFileSizeEx);
    else if (_wcsicmp(symbol.name.c_str(), L"getfilesize") == 0)
        resolution.targetAddress = reinterpret_cast<ULONGLONG>(&BridgeGetFileSize);
    else if (_wcsicmp(symbol.name.c_str(), L"setfilepointerex") == 0)
        resolution.targetAddress = reinterpret_cast<ULONGLONG>(&BridgeSetFilePointerEx);
    else if (_wcsicmp(symbol.name.c_str(), L"setfilepointer") == 0)
        resolution.targetAddress = reinterpret_cast<ULONGLONG>(&BridgeSetFilePointer);
    else if (_wcsicmp(symbol.name.c_str(), L"flushfilebuffers") == 0)
        resolution.targetAddress = reinterpret_cast<ULONGLONG>(&BridgeFlushFileBuffers);
    else if (_wcsicmp(symbol.name.c_str(), L"setendoffile") == 0)
        resolution.targetAddress = reinterpret_cast<ULONGLONG>(&BridgeSetEndOfFile);
    if (_wcsicmp(symbol.name.c_str(), L"openeventw") == 0)
        resolution.targetAddress = reinterpret_cast<ULONGLONG>(&BridgeOpenEventW);
    else if (_wcsicmp(symbol.name.c_str(), L"openfilemappingw") == 0)
        resolution.targetAddress = reinterpret_cast<ULONGLONG>(&BridgeOpenFileMappingW);
    else if (_wcsicmp(symbol.name.c_str(), L"mapviewoffile") == 0)
        resolution.targetAddress = reinterpret_cast<ULONGLONG>(&BridgeMapViewOfFile);
    else if (_wcsicmp(symbol.name.c_str(), L"unmapviewoffile") == 0)
        resolution.targetAddress = reinterpret_cast<ULONGLONG>(&BridgeUnmapViewOfFile);
    else if (_wcsicmp(symbol.name.c_str(), L"getfileinformationbyhandle") == 0)
        resolution.targetAddress = reinterpret_cast<ULONGLONG>(&BridgeGetFileInformationByHandle);
    else if (_wcsicmp(symbol.name.c_str(), L"setfiletime") == 0)
        resolution.targetAddress = reinterpret_cast<ULONGLONG>(&BridgeSetFileTime);
    else if (_wcsicmp(symbol.name.c_str(), L"filetimetodosdatetime") == 0)
        resolution.targetAddress = reinterpret_cast<ULONGLONG>(&BridgeFileTimeToDosDateTime);
    else if (_wcsicmp(symbol.name.c_str(), L"filetimetolocalfiletime") == 0)
        resolution.targetAddress = reinterpret_cast<ULONGLONG>(&BridgeFileTimeToLocalFileTime);
    else if (_wcsicmp(symbol.name.c_str(), L"localfiletimetofiletime") == 0)
        resolution.targetAddress = reinterpret_cast<ULONGLONG>(&BridgeLocalFileTimeToFileTime);
    else if (_wcsicmp(symbol.name.c_str(), L"dosdatetimetofiletime") == 0)
        resolution.targetAddress = reinterpret_cast<ULONGLONG>(&BridgeDosDateTimeToFileTime);
    else if (_wcsicmp(symbol.name.c_str(), L"getoemcp") == 0)
        resolution.targetAddress = reinterpret_cast<ULONGLONG>(&BridgeGetOEMCP);
    else if (_wcsicmp(symbol.name.c_str(), L"filetimetosystemtime") == 0)
        resolution.targetAddress = reinterpret_cast<ULONGLONG>(&BridgeFileTimeToSystemTime);
    else if (_wcsicmp(symbol.name.c_str(), L"comparefiletime") == 0)
        resolution.targetAddress = reinterpret_cast<ULONGLONG>(&BridgeCompareFileTime);
    if (_wcsicmp(symbol.name.c_str(), L"getprocesstimes") == 0)
        resolution.targetAddress = reinterpret_cast<ULONGLONG>(&BridgeGetProcessTimes);
    else if (_wcsicmp(symbol.name.c_str(), L"getprocessaffinitymask") == 0)
        resolution.targetAddress = reinterpret_cast<ULONGLONG>(&BridgeGetProcessAffinityMask);
    else if (_wcsicmp(symbol.name.c_str(), L"resumethread") == 0)
        resolution.targetAddress = reinterpret_cast<ULONGLONG>(&BridgeResumeThread);
    else if (_wcsicmp(symbol.name.c_str(), L"terminateprocess") == 0)
        resolution.targetAddress = reinterpret_cast<ULONGLONG>(&BridgeTerminateProcess);
    else if (_wcsicmp(symbol.name.c_str(), L"deviceiocontrol") == 0)
        resolution.targetAddress = reinterpret_cast<ULONGLONG>(&BridgeDeviceIoControl);
    else if (_wcsicmp(symbol.name.c_str(), L"findfirststreamw") == 0)
        resolution.targetAddress = reinterpret_cast<ULONGLONG>(&BridgeFindFirstStreamW);
    else if (_wcsicmp(symbol.name.c_str(), L"findnextstreamw") == 0)
        resolution.targetAddress = reinterpret_cast<ULONGLONG>(&BridgeFindNextStreamW);
    else if (_wcsicmp(symbol.name.c_str(), L"createhardlinkw") == 0)
        resolution.targetAddress = reinterpret_cast<ULONGLONG>(&BridgeCreateHardLinkW);
    else if (_wcsicmp(symbol.name.c_str(), L"movefilewithprogressw") == 0)
        resolution.targetAddress = reinterpret_cast<ULONGLONG>(&BridgeMoveFileWithProgressW);
    else if (_wcsicmp(symbol.name.c_str(), L"formatmessagew") == 0)
        resolution.targetAddress = reinterpret_cast<ULONGLONG>(&BridgeFormatMessageW);
    if (_wcsicmp(symbol.name.c_str(), L"localfree") == 0)
        resolution.targetAddress = reinterpret_cast<ULONGLONG>(&BridgeLocalFree);
    else if (_wcsicmp(symbol.name.c_str(), L"localalloc") == 0)
        resolution.targetAddress = reinterpret_cast<ULONGLONG>(&BridgeLocalAlloc);
    else if (_wcsicmp(symbol.name.c_str(), L"unhandledexceptionfilter") == 0)
        resolution.targetAddress = reinterpret_cast<ULONGLONG>(&BridgeUnhandledExceptionFilter);
    else if (_wcsicmp(symbol.name.c_str(), L"setunhandledexceptionfilter") == 0)
        resolution.targetAddress = reinterpret_cast<ULONGLONG>(&BridgeSetUnhandledExceptionFilter);
    else if (_wcsicmp(symbol.name.c_str(), L"rtllookupfunctionentry") == 0)
        resolution.targetAddress = reinterpret_cast<ULONGLONG>(&BridgeRtlLookupFunctionEntry);
    else if (_wcsicmp(symbol.name.c_str(), L"rtlvirtualunwind") == 0)
        resolution.targetAddress = reinterpret_cast<ULONGLONG>(&BridgeRtlVirtualUnwind);
    else if (_wcsicmp(symbol.name.c_str(), L"rtlcapturecontext") == 0)
        resolution.targetAddress = reinterpret_cast<ULONGLONG>(&BridgeRtlCaptureContext);
    else if (_wcsicmp(symbol.name.c_str(), L"openprocess") == 0)
        resolution.targetAddress = reinterpret_cast<ULONGLONG>(&BridgeOpenProcess);
    else if (_wcsicmp(symbol.name.c_str(), L"createtoolhelp32snapshot") == 0)
        resolution.targetAddress = reinterpret_cast<ULONGLONG>(&BridgeCreateToolhelp32Snapshot);
    if (_wcsicmp(symbol.name.c_str(), L"process32firstw") == 0)
        resolution.targetAddress = reinterpret_cast<ULONGLONG>(&BridgeProcess32FirstW);
    else if (_wcsicmp(symbol.name.c_str(), L"process32nextw") == 0)
        resolution.targetAddress = reinterpret_cast<ULONGLONG>(&BridgeProcess32NextW);
    else if (_wcsicmp(symbol.name.c_str(), L"copyfileexw") == 0)
        resolution.targetAddress = reinterpret_cast<ULONGLONG>(&BridgeCopyFileExW);
    else if (_wcsicmp(symbol.name.c_str(), L"getcompressedfilesizew") == 0)
        resolution.targetAddress = reinterpret_cast<ULONGLONG>(&BridgeGetCompressedFileSizeW);
    else if (_wcsicmp(symbol.name.c_str(), L"findfirstchangenotificationw") == 0)
        resolution.targetAddress = reinterpret_cast<ULONGLONG>(&BridgeFindFirstChangeNotificationW);
    else if (_wcsicmp(symbol.name.c_str(), L"findnextchangenotification") == 0)
        resolution.targetAddress = reinterpret_cast<ULONGLONG>(&BridgeFindNextChangeNotification);
    else if (_wcsicmp(symbol.name.c_str(), L"findclosechangenotification") == 0)
        resolution.targetAddress = reinterpret_cast<ULONGLONG>(&BridgeFindCloseChangeNotification);
    if (_wcsicmp(symbol.name.c_str(), L"createfilemappingw") == 0)
        resolution.targetAddress = reinterpret_cast<ULONGLONG>(&BridgeCreateFileMappingW);
    else if (_wcsicmp(symbol.name.c_str(), L"createprocessw") == 0)
        resolution.targetAddress = reinterpret_cast<ULONGLONG>(&BridgeCreateProcessW);
    else if (_wcsicmp(symbol.name.c_str(), L"createprocessa") == 0)
        resolution.targetAddress = reinterpret_cast<ULONGLONG>(&BridgeCreateProcessA);
    else if (_wcsicmp(symbol.name.c_str(), L"getexitcodeprocess") == 0)
        resolution.targetAddress = reinterpret_cast<ULONGLONG>(&BridgeGetExitCodeProcess);
    else if (_wcsicmp(symbol.name.c_str(), L"getexitcodethread") == 0)
        resolution.targetAddress = reinterpret_cast<ULONGLONG>(&BridgeGetExitCodeThread);
    else if (_wcsicmp(symbol.name.c_str(), L"createthread") == 0)
        resolution.targetAddress = reinterpret_cast<ULONGLONG>(&BridgeCreateThread);
    else if (_wcsicmp(symbol.name.c_str(), L"raiseexception") == 0)
        resolution.targetAddress = reinterpret_cast<ULONGLONG>(&BridgeRaiseException);
    else if (_wcsicmp(symbol.name.c_str(), L"createdirectoryw") == 0)
        resolution.targetAddress = reinterpret_cast<ULONGLONG>(&BridgeCreateDirectoryW);
    else if (_wcsicmp(symbol.name.c_str(), L"deletefilew") == 0)
        resolution.targetAddress = reinterpret_cast<ULONGLONG>(&BridgeDeleteFileW);
    else if (_wcsicmp(symbol.name.c_str(), L"movefilew") == 0)
        resolution.targetAddress = reinterpret_cast<ULONGLONG>(&BridgeMoveFileW);
    else if (_wcsicmp(symbol.name.c_str(), L"movefileexw") == 0)
        resolution.targetAddress = reinterpret_cast<ULONGLONG>(&BridgeMoveFileExW);
    else if (_wcsicmp(symbol.name.c_str(), L"removedirectoryw") == 0)
        resolution.targetAddress = reinterpret_cast<ULONGLONG>(&BridgeRemoveDirectoryW);
    if (_wcsicmp(symbol.name.c_str(), L"getfileattributesw") == 0)
        resolution.targetAddress = reinterpret_cast<ULONGLONG>(&BridgeGetFileAttributesW);
    else if (_wcsicmp(symbol.name.c_str(), L"setfileattributesw") == 0)
        resolution.targetAddress = reinterpret_cast<ULONGLONG>(&BridgeSetFileAttributesW);
    else if (_wcsicmp(symbol.name.c_str(), L"findfirstfilew") == 0)
        resolution.targetAddress = reinterpret_cast<ULONGLONG>(&BridgeFindFirstFileW);
    else if (_wcsicmp(symbol.name.c_str(), L"findnextfilew") == 0)
        resolution.targetAddress = reinterpret_cast<ULONGLONG>(&BridgeFindNextFileW);
    else if (_wcsicmp(symbol.name.c_str(), L"findclose") == 0)
        resolution.targetAddress = reinterpret_cast<ULONGLONG>(&BridgeFindClose);
    else if (_wcsicmp(symbol.name.c_str(), L"setcurrentdirectoryw") == 0)
        resolution.targetAddress = reinterpret_cast<ULONGLONG>(&BridgeSetCurrentDirectoryW);
    else if (_wcsicmp(symbol.name.c_str(), L"getcurrentdirectoryw") == 0)
        resolution.targetAddress = reinterpret_cast<ULONGLONG>(&BridgeGetCurrentDirectoryW);
    else if (_wcsicmp(symbol.name.c_str(), L"gettemppathw") == 0)
        resolution.targetAddress = reinterpret_cast<ULONGLONG>(&BridgeGetTempPathW);
    if (_wcsicmp(symbol.name.c_str(), L"getlogicaldrives") == 0)
        resolution.targetAddress = reinterpret_cast<ULONGLONG>(&BridgeGetLogicalDrives);
    else if (_wcsicmp(symbol.name.c_str(), L"getlogicaldrivestringsw") == 0)
        resolution.targetAddress = reinterpret_cast<ULONGLONG>(&BridgeGetLogicalDriveStringsW);
    else if (_wcsicmp(symbol.name.c_str(), L"getdrivetypew") == 0)
        resolution.targetAddress = reinterpret_cast<ULONGLONG>(&BridgeGetDriveTypeW);
    else if (_wcsicmp(symbol.name.c_str(), L"getdiskfreespaceexw") == 0)
        resolution.targetAddress = reinterpret_cast<ULONGLONG>(&BridgeGetDiskFreeSpaceExW);
    else if (_wcsicmp(symbol.name.c_str(), L"getdiskfreespacew") == 0)
        resolution.targetAddress = reinterpret_cast<ULONGLONG>(&BridgeGetDiskFreeSpaceW);
    else if (_wcsicmp(symbol.name.c_str(), L"getvolumeinformationw") == 0)
        resolution.targetAddress = reinterpret_cast<ULONGLONG>(&BridgeGetVolumeInformationW);
    else if (_wcsicmp(symbol.name.c_str(), L"getwindowsdirectoryw") == 0)
        resolution.targetAddress = reinterpret_cast<ULONGLONG>(&BridgeGetWindowsDirectoryW);
    else if (_wcsicmp(symbol.name.c_str(), L"expandenvironmentstringsw") == 0)
        resolution.targetAddress = reinterpret_cast<ULONGLONG>(&BridgeExpandEnvironmentStringsW);
    if (_wcsicmp(symbol.name.c_str(), L"getmodulefilenamew") == 0)
        resolution.targetAddress = reinterpret_cast<ULONGLONG>(&BridgeGetModuleFileNameW);
    else if (_wcsicmp(symbol.name.c_str(), L"getmodulehandlew") == 0)
        resolution.targetAddress = reinterpret_cast<ULONGLONG>(&BridgeGetModuleHandleW);
    else if (_wcsicmp(symbol.name.c_str(), L"getmodulehandlea") == 0)
        resolution.targetAddress = reinterpret_cast<ULONGLONG>(&BridgeGetModuleHandleA);
    else if (_wcsicmp(symbol.name.c_str(), L"findresourcea") == 0)
        resolution.targetAddress = reinterpret_cast<ULONGLONG>(&BridgeFindResourceA);
    else if (_wcsicmp(symbol.name.c_str(), L"findresourcew") == 0)
        resolution.targetAddress = reinterpret_cast<ULONGLONG>(&BridgeFindResourceW);
    else if (_wcsicmp(symbol.name.c_str(), L"findresourceexa") == 0)
        resolution.targetAddress = reinterpret_cast<ULONGLONG>(&BridgeFindResourceExA);
    else if (_wcsicmp(symbol.name.c_str(), L"findresourceexw") == 0)
        resolution.targetAddress = reinterpret_cast<ULONGLONG>(&BridgeFindResourceExW);
    else if (_wcsicmp(symbol.name.c_str(), L"loadresource") == 0)
        resolution.targetAddress = reinterpret_cast<ULONGLONG>(&BridgeLoadResource);
    else if (_wcsicmp(symbol.name.c_str(), L"lockresource") == 0)
        resolution.targetAddress = reinterpret_cast<ULONGLONG>(&BridgeLockResource);
    else if (_wcsicmp(symbol.name.c_str(), L"sizeofresource") == 0)
        resolution.targetAddress = reinterpret_cast<ULONGLONG>(&BridgeSizeofResource);
    else if (_wcsicmp(symbol.name.c_str(), L"freeresource") == 0)
        resolution.targetAddress = reinterpret_cast<ULONGLONG>(&BridgeFreeResource);
    else if (_wcsicmp(symbol.name.c_str(), L"enumresourcetypesa") == 0)
        resolution.targetAddress = reinterpret_cast<ULONGLONG>(&BridgeEnumResourceTypesA);
    else if (_wcsicmp(symbol.name.c_str(), L"enumresourcetypesw") == 0)
        resolution.targetAddress = reinterpret_cast<ULONGLONG>(&BridgeEnumResourceTypesW);
    else if (_wcsicmp(symbol.name.c_str(), L"enumresourcetypesexa") == 0)
        resolution.targetAddress = reinterpret_cast<ULONGLONG>(&BridgeEnumResourceTypesExA);
    else if (_wcsicmp(symbol.name.c_str(), L"enumresourcetypesexw") == 0)
        resolution.targetAddress = reinterpret_cast<ULONGLONG>(&BridgeEnumResourceTypesExW);
    else if (_wcsicmp(symbol.name.c_str(), L"enumresourcenamesa") == 0)
        resolution.targetAddress = reinterpret_cast<ULONGLONG>(&BridgeEnumResourceNamesA);
    else if (_wcsicmp(symbol.name.c_str(), L"enumresourcenamesw") == 0)
        resolution.targetAddress = reinterpret_cast<ULONGLONG>(&BridgeEnumResourceNamesW);
    else if (_wcsicmp(symbol.name.c_str(), L"enumresourcenamesexa") == 0)
        resolution.targetAddress = reinterpret_cast<ULONGLONG>(&BridgeEnumResourceNamesExA);
    else if (_wcsicmp(symbol.name.c_str(), L"enumresourcenamesexw") == 0)
        resolution.targetAddress = reinterpret_cast<ULONGLONG>(&BridgeEnumResourceNamesExW);
    else if (_wcsicmp(symbol.name.c_str(), L"enumresourcelanguagesa") == 0)
        resolution.targetAddress = reinterpret_cast<ULONGLONG>(&BridgeEnumResourceLanguagesA);
    else if (_wcsicmp(symbol.name.c_str(), L"enumresourcelanguagesw") == 0)
        resolution.targetAddress = reinterpret_cast<ULONGLONG>(&BridgeEnumResourceLanguagesW);
    else if (_wcsicmp(symbol.name.c_str(), L"enumresourcelanguagesexa") == 0)
        resolution.targetAddress = reinterpret_cast<ULONGLONG>(&BridgeEnumResourceLanguagesExA);
    else if (_wcsicmp(symbol.name.c_str(), L"enumresourcelanguagesexw") == 0)
        resolution.targetAddress = reinterpret_cast<ULONGLONG>(&BridgeEnumResourceLanguagesExW);
    else if (_wcsicmp(symbol.name.c_str(), L"createactctxw") == 0)
        resolution.targetAddress = reinterpret_cast<ULONGLONG>(&BridgeCreateActCtxW);
    else if (_wcsicmp(symbol.name.c_str(), L"createactctxa") == 0)
        resolution.targetAddress = reinterpret_cast<ULONGLONG>(&BridgeCreateActCtxA);
    else if (_wcsicmp(symbol.name.c_str(), L"addrefactctx") == 0)
        resolution.targetAddress = reinterpret_cast<ULONGLONG>(&BridgeAddRefActCtx);
    else if (_wcsicmp(symbol.name.c_str(), L"releaseactctx") == 0)
        resolution.targetAddress = reinterpret_cast<ULONGLONG>(&BridgeReleaseActCtx);
    else if (_wcsicmp(symbol.name.c_str(), L"activateactctx") == 0)
        resolution.targetAddress = reinterpret_cast<ULONGLONG>(&BridgeActivateActCtx);
    else if (_wcsicmp(symbol.name.c_str(), L"deactivateactctx") == 0)
        resolution.targetAddress = reinterpret_cast<ULONGLONG>(&BridgeDeactivateActCtx);
    else if (_wcsicmp(symbol.name.c_str(), L"getcurrentactctx") == 0)
        resolution.targetAddress = reinterpret_cast<ULONGLONG>(&BridgeGetCurrentActCtx);
    else if (_wcsicmp(symbol.name.c_str(), L"queryactctxw") == 0)
        resolution.targetAddress = reinterpret_cast<ULONGLONG>(&BridgeQueryActCtxW);
    else if (_wcsicmp(symbol.name.c_str(), L"queryactctxsettingsw") == 0)
        resolution.targetAddress = reinterpret_cast<ULONGLONG>(&BridgeQueryActCtxSettingsW);
    else if (_wcsicmp(symbol.name.c_str(), L"gettickcount") == 0)
        resolution.targetAddress = reinterpret_cast<ULONGLONG>(&BridgeGetTickCount);
    else if (_wcsicmp(symbol.name.c_str(), L"gettickcount64") == 0)
        resolution.targetAddress = reinterpret_cast<ULONGLONG>(&BridgeGetTickCount64);
    else if (_wcsicmp(symbol.name.c_str(), L"queryperformancecounter") == 0)
        resolution.targetAddress = reinterpret_cast<ULONGLONG>(&BridgeQueryPerformanceCounter);
    else if (_wcsicmp(symbol.name.c_str(), L"queryperformancefrequency") == 0)
        resolution.targetAddress = reinterpret_cast<ULONGLONG>(&BridgeQueryPerformanceFrequency);
    else if (_wcsicmp(symbol.name.c_str(), L"getcurrentthreadid") == 0)
        resolution.targetAddress = reinterpret_cast<ULONGLONG>(&BridgeGetCurrentThreadId);
    else if (_wcsicmp(symbol.name.c_str(), L"getcurrentprocessid") == 0)
        resolution.targetAddress = reinterpret_cast<ULONGLONG>(&BridgeGetCurrentProcessId);
    else if (_wcsicmp(symbol.name.c_str(), L"getcurrentprocess") == 0)
        resolution.targetAddress = reinterpret_cast<ULONGLONG>(&BridgeGetCurrentProcess);
    else if (_wcsicmp(symbol.name.c_str(), L"getsystemtimeasfiletime") == 0)
        resolution.targetAddress = reinterpret_cast<ULONGLONG>(&BridgeGetSystemTimeAsFileTime);
    else if (_wcsicmp(symbol.name.c_str(), L"virtualalloc") == 0)
        resolution.targetAddress = reinterpret_cast<ULONGLONG>(&BridgeVirtualAlloc);
    if (_wcsicmp(symbol.name.c_str(), L"virtualfree") == 0)
        resolution.targetAddress = reinterpret_cast<ULONGLONG>(&BridgeVirtualFree);
    else if (_wcsicmp(symbol.name.c_str(), L"virtualprotect") == 0)
        resolution.targetAddress = reinterpret_cast<ULONGLONG>(&BridgeVirtualProtect);
    else if (_wcsicmp(symbol.name.c_str(), L"getlargepageminimum") == 0)
        resolution.targetAddress = reinterpret_cast<ULONGLONG>(&BridgeGetLargePageMinimum);
    else if (_wcsicmp(symbol.name.c_str(), L"globalalloc") == 0)
        resolution.targetAddress = reinterpret_cast<ULONGLONG>(&BridgeGlobalAlloc);
    else if (_wcsicmp(symbol.name.c_str(), L"globalfree") == 0)
        resolution.targetAddress = reinterpret_cast<ULONGLONG>(&BridgeGlobalFree);
    else if (_wcsicmp(symbol.name.c_str(), L"globallock") == 0)
        resolution.targetAddress = reinterpret_cast<ULONGLONG>(&BridgeGlobalLock);
    else if (_wcsicmp(symbol.name.c_str(), L"globalunlock") == 0)
        resolution.targetAddress = reinterpret_cast<ULONGLONG>(&BridgeGlobalUnlock);
    else if (_wcsicmp(symbol.name.c_str(), L"globalsize") == 0)
        resolution.targetAddress = reinterpret_cast<ULONGLONG>(&BridgeGlobalSize);
    else if (_wcsicmp(symbol.name.c_str(), L"getprocessheap") == 0)
        resolution.targetAddress = reinterpret_cast<ULONGLONG>(&BridgeGetProcessHeap);
    if (_wcsicmp(symbol.name.c_str(), L"heapalloc") == 0)
        resolution.targetAddress = reinterpret_cast<ULONGLONG>(&BridgeHeapAlloc);
    else if (_wcsicmp(symbol.name.c_str(), L"heapfree") == 0)
        resolution.targetAddress = reinterpret_cast<ULONGLONG>(&BridgeHeapFree);
    else if (_wcsicmp(symbol.name.c_str(), L"initializecriticalsection") == 0)
        resolution.targetAddress = reinterpret_cast<ULONGLONG>(&BridgeInitializeCriticalSection);
    else if (_wcsicmp(symbol.name.c_str(), L"entercriticalsection") == 0)
        resolution.targetAddress = reinterpret_cast<ULONGLONG>(&BridgeEnterCriticalSection);
    else if (_wcsicmp(symbol.name.c_str(), L"leavecriticalsection") == 0)
        resolution.targetAddress = reinterpret_cast<ULONGLONG>(&BridgeLeaveCriticalSection);
    else if (_wcsicmp(symbol.name.c_str(), L"deletecriticalsection") == 0)
        resolution.targetAddress = reinterpret_cast<ULONGLONG>(&BridgeDeleteCriticalSection);
    else if (_wcsicmp(symbol.name.c_str(), L"getsysteminfo") == 0)
        resolution.targetAddress = reinterpret_cast<ULONGLONG>(&BridgeGetSystemInfo);
    else if (_wcsicmp(symbol.name.c_str(), L"globalmemorystatusex") == 0)
        resolution.targetAddress = reinterpret_cast<ULONGLONG>(&BridgeGlobalMemoryStatusEx);
    else if (_wcsicmp(symbol.name.c_str(), L"isprocessorfeaturepresent") == 0)
        resolution.targetAddress = reinterpret_cast<ULONGLONG>(&BridgeIsProcessorFeaturePresent);
    else if (_wcsicmp(symbol.name.c_str(), L"lstrlenw") == 0)
        resolution.targetAddress = reinterpret_cast<ULONGLONG>(&BridgeLstrlenW);
    else if (_wcsicmp(symbol.name.c_str(), L"lstrcatw") == 0)
        resolution.targetAddress = reinterpret_cast<ULONGLONG>(&BridgeLstrcatW);
    else if (_wcsicmp(symbol.name.c_str(), L"widechartomultibyte") == 0)
        resolution.targetAddress = reinterpret_cast<ULONGLONG>(&BridgeWideCharToMultiByte);
    else if (_wcsicmp(symbol.name.c_str(), L"multibytetowidechar") == 0)
        resolution.targetAddress = reinterpret_cast<ULONGLONG>(&BridgeMultiByteToWideChar);
    if (_wcsicmp(symbol.name.c_str(), L"getsystemdefaultlangid") == 0)
        resolution.targetAddress = reinterpret_cast<ULONGLONG>(&BridgeGetSystemDefaultLangID);
    else if (_wcsicmp(symbol.name.c_str(), L"getuserdefaultlangid") == 0)
        resolution.targetAddress = reinterpret_cast<ULONGLONG>(&BridgeGetUserDefaultLangID);
    else if (_wcsicmp(symbol.name.c_str(), L"getversion") == 0)
        resolution.targetAddress = reinterpret_cast<ULONGLONG>(&BridgeGetVersion);
    else if (_wcsicmp(symbol.name.c_str(), L"getstartupinfoa") == 0)
        resolution.targetAddress = reinterpret_cast<ULONGLONG>(&BridgeGetStartupInfoA);
    else if (_wcsicmp(symbol.name.c_str(), L"getstdhandle") == 0)
        resolution.targetAddress = reinterpret_cast<ULONGLONG>(&BridgeGetStdHandle);
    else if (_wcsicmp(symbol.name.c_str(), L"getconsolemode") == 0)
        resolution.targetAddress = reinterpret_cast<ULONGLONG>(&BridgeGetConsoleMode);
    else if (_wcsicmp(symbol.name.c_str(), L"setpriorityclass") == 0)
        resolution.targetAddress = reinterpret_cast<ULONGLONG>(&BridgeSetPriorityClass);
    else if (_wcsicmp(symbol.name.c_str(), L"setprocessaffinitymask") == 0)
        resolution.targetAddress = reinterpret_cast<ULONGLONG>(&BridgeSetProcessAffinityMask);
    else if (_wcsicmp(symbol.name.c_str(), L"setthreadaffinitymask") == 0)
        resolution.targetAddress = reinterpret_cast<ULONGLONG>(&BridgeSetThreadAffinityMask);
    if (_wcsicmp(symbol.name.c_str(), L"loadlibraryw") == 0)
        resolution.targetAddress = reinterpret_cast<ULONGLONG>(&BridgeLoadLibraryW);
    else if (_wcsicmp(symbol.name.c_str(), L"loadlibrarya") == 0)
        resolution.targetAddress = reinterpret_cast<ULONGLONG>(&BridgeLoadLibraryA);
    else if (_wcsicmp(symbol.name.c_str(), L"loadlibraryexw") == 0)
        resolution.targetAddress = reinterpret_cast<ULONGLONG>(&BridgeLoadLibraryExW);
    else if (_wcsicmp(symbol.name.c_str(), L"setdefaultdlldirectories") == 0)
        resolution.targetAddress = reinterpret_cast<ULONGLONG>(&BridgeSetDefaultDllDirectories);
    else if (_wcsicmp(symbol.name.c_str(), L"getprocaddress") == 0)
        resolution.targetAddress = reinterpret_cast<ULONGLONG>(&BridgeGetProcAddress);
    else if (_wcsicmp(symbol.name.c_str(), L"freelibrary") == 0)
        resolution.targetAddress = reinterpret_cast<ULONGLONG>(&BridgeFreeLibrary);
    else if (_wcsicmp(symbol.name.c_str(), L"disablethreadlibrarycalls") == 0)
        resolution.targetAddress = reinterpret_cast<ULONGLONG>(&BridgeDisableThreadLibraryCalls);
    else if (_wcsicmp(symbol.name.c_str(), L"getcommandlinew") == 0)
        resolution.targetAddress = reinterpret_cast<ULONGLONG>(&BridgeGetCommandLineW);
    else if (_wcsicmp(symbol.name.c_str(), L"outputdebugstringw") == 0)
        resolution.targetAddress = reinterpret_cast<ULONGLONG>(&BridgeOutputDebugStringW);
    else if (_wcsicmp(symbol.name.c_str(), L"isdebuggerpresent") == 0)
        resolution.targetAddress = reinterpret_cast<ULONGLONG>(&BridgeIsDebuggerPresent);
    else if (_wcsicmp(symbol.name.c_str(), L"getlasterror") == 0)
        resolution.targetAddress = reinterpret_cast<ULONGLONG>(&BridgeGetLastError);
    else if (_wcsicmp(symbol.name.c_str(), L"setlasterror") == 0)
        resolution.targetAddress = reinterpret_cast<ULONGLONG>(&BridgeSetLastError);
    if (_wcsicmp(symbol.name.c_str(), L"createeventw") == 0)
        resolution.targetAddress = reinterpret_cast<ULONGLONG>(&BridgeCreateEventW);
    else if (_wcsicmp(symbol.name.c_str(), L"setevent") == 0)
        resolution.targetAddress = reinterpret_cast<ULONGLONG>(&BridgeSetEvent);
    else if (_wcsicmp(symbol.name.c_str(), L"resetevent") == 0)
        resolution.targetAddress = reinterpret_cast<ULONGLONG>(&BridgeResetEvent);
    else if (_wcsicmp(symbol.name.c_str(), L"createmutexw") == 0)
        resolution.targetAddress = reinterpret_cast<ULONGLONG>(&BridgeCreateMutexW);
    else if (_wcsicmp(symbol.name.c_str(), L"releasemutex") == 0)
        resolution.targetAddress = reinterpret_cast<ULONGLONG>(&BridgeReleaseMutex);
    else if (_wcsicmp(symbol.name.c_str(), L"createsemaphorew") == 0)
        resolution.targetAddress = reinterpret_cast<ULONGLONG>(&BridgeCreateSemaphoreW);
    else if (_wcsicmp(symbol.name.c_str(), L"releasesemaphore") == 0)
        resolution.targetAddress = reinterpret_cast<ULONGLONG>(&BridgeReleaseSemaphore);
    else if (_wcsicmp(symbol.name.c_str(), L"waitforsingleobject") == 0)
        resolution.targetAddress = reinterpret_cast<ULONGLONG>(&BridgeWaitForSingleObject);
    else if (_wcsicmp(symbol.name.c_str(), L"waitformultipleobjects") == 0)
        resolution.targetAddress = reinterpret_cast<ULONGLONG>(&BridgeWaitForMultipleObjects);
    else if (_wcsicmp(symbol.name.c_str(), L"sleep") == 0)
        resolution.targetAddress = reinterpret_cast<ULONGLONG>(&BridgeSleep);

    if (_wcsicmp(symbol.name.c_str(), L"getlocaltime") == 0) resolution.targetAddress = reinterpret_cast<ULONGLONG>(&BridgeGetLocalTime);
    else if (_wcsicmp(symbol.name.c_str(), L"getdateformatw") == 0) resolution.targetAddress = reinterpret_cast<ULONGLONG>(&BridgeGetDateFormatW);
    else if (_wcsicmp(symbol.name.c_str(), L"gettimeformatw") == 0) resolution.targetAddress = reinterpret_cast<ULONGLONG>(&BridgeGetTimeFormatW);
    else if (_wcsicmp(symbol.name.c_str(), L"getuserdefaultuilanguage") == 0) resolution.targetAddress = reinterpret_cast<ULONGLONG>(&BridgeGetUserDefaultUILanguage);
    else if (_wcsicmp(symbol.name.c_str(), L"findnlsstring") == 0) resolution.targetAddress = reinterpret_cast<ULONGLONG>(&BridgeFindNLSString);
    else if (_wcsicmp(symbol.name.c_str(), L"lstrcmpw") == 0) resolution.targetAddress = reinterpret_cast<ULONGLONG>(&BridgeLstrcmpW);
    else if (_wcsicmp(symbol.name.c_str(), L"lstrcmpiw") == 0) resolution.targetAddress = reinterpret_cast<ULONGLONG>(&BridgeLstrcmpiW);
    else if (_wcsicmp(symbol.name.c_str(), L"getversionexw") == 0) resolution.targetAddress = reinterpret_cast<ULONGLONG>(&BridgeGetVersionExW);
    else if (_wcsicmp(symbol.name.c_str(), L"wow64disablewow64fsredirection") == 0) resolution.targetAddress = reinterpret_cast<ULONGLONG>(&BridgeWow64DisableWow64FsRedirection);
    else if (_wcsicmp(symbol.name.c_str(), L"wow64revertwow64fsredirection") == 0) resolution.targetAddress = reinterpret_cast<ULONGLONG>(&BridgeWow64RevertWow64FsRedirection);
    else if (_wcsicmp(symbol.name.c_str(), L"freelibraryandexitthread") == 0) resolution.targetAddress = reinterpret_cast<ULONGLONG>(&BridgeFreeLibraryAndExitThread);
    else if (_wcsicmp(symbol.name.c_str(), L"iswow64process") == 0) resolution.targetAddress = reinterpret_cast<ULONGLONG>(&BridgeIsWow64Process);
    else if (_wcsicmp(symbol.name.c_str(), L"localrealloc") == 0) resolution.targetAddress = reinterpret_cast<ULONGLONG>(&BridgeLocalReAlloc);
    else if (_wcsicmp(symbol.name.c_str(), L"localsize") == 0) resolution.targetAddress = reinterpret_cast<ULONGLONG>(&BridgeLocalSize);
    else if (_wcsicmp(symbol.name.c_str(), L"locallock") == 0) resolution.targetAddress = reinterpret_cast<ULONGLONG>(&BridgeLocalLock);
    else if (_wcsicmp(symbol.name.c_str(), L"localunlock") == 0) resolution.targetAddress = reinterpret_cast<ULONGLONG>(&BridgeLocalUnlock);
    else if (_wcsicmp(symbol.name.c_str(), L"getacp") == 0) resolution.targetAddress = reinterpret_cast<ULONGLONG>(&BridgeGetACP);
    else if (_wcsicmp(symbol.name.c_str(), L"getstartupinfow") == 0) resolution.targetAddress = reinterpret_cast<ULONGLONG>(&BridgeGetStartupInfoW);
    else if (_wcsicmp(symbol.name.c_str(), L"getfullpathnamew") == 0) resolution.targetAddress = reinterpret_cast<ULONGLONG>(&BridgeGetFullPathNameW);
    else if (_wcsicmp(symbol.name.c_str(), L"foldstringw") == 0) resolution.targetAddress = reinterpret_cast<ULONGLONG>(&BridgeFoldStringW);
    else if (_wcsicmp(symbol.name.c_str(), L"heapsetinformation") == 0) resolution.targetAddress = reinterpret_cast<ULONGLONG>(&BridgeHeapSetInformation);
    else if (_wcsicmp(symbol.name.c_str(), L"muldiv") == 0) resolution.targetAddress = reinterpret_cast<ULONGLONG>(&BridgeMulDiv);
    else if (_wcsicmp(symbol.name.c_str(), L"getlocaleinfow") == 0) resolution.targetAddress = reinterpret_cast<ULONGLONG>(&BridgeGetLocaleInfoW);
    else if (_wcsicmp(symbol.name.c_str(), L"seterrormode") == 0) resolution.targetAddress = reinterpret_cast<ULONGLONG>(&BridgeSetErrorMode);
    else if (_wcsicmp(symbol.name.c_str(), L"rtlinitunicodestring") == 0) resolution.targetAddress = reinterpret_cast<ULONGLONG>(&BridgeRtlInitUnicodeString);
    else if (_wcsicmp(symbol.name.c_str(), L"ntquerylicensevalue") == 0) resolution.targetAddress = reinterpret_cast<ULONGLONG>(&BridgeNtQueryLicenseValue);
    else if (_wcsicmp(symbol.name.c_str(), L"winsqmincrementdword") == 0) resolution.targetAddress = reinterpret_cast<ULONGLONG>(&BridgeWinSqmIncrementDWORD);
    else if (_wcsicmp(symbol.name.c_str(), L"winsqmaddtostream") == 0) resolution.targetAddress = reinterpret_cast<ULONGLONG>(&BridgeWinSqmAddToStream);

    if (resolution.targetAddress != 0)
    {
        resolution.disposition = ImportDisposition::NeedsBridge;
        const bool synchronizationImport =
            _wcsicmp(symbol.name.c_str(), L"createeventw") == 0 ||
            _wcsicmp(symbol.name.c_str(), L"setevent") == 0 ||
            _wcsicmp(symbol.name.c_str(), L"resetevent") == 0 ||
            _wcsicmp(symbol.name.c_str(), L"createmutexw") == 0 ||
            _wcsicmp(symbol.name.c_str(), L"releasemutex") == 0 ||
            _wcsicmp(symbol.name.c_str(), L"createsemaphorew") == 0 ||
            _wcsicmp(symbol.name.c_str(), L"releasesemaphore") == 0 ||
            _wcsicmp(symbol.name.c_str(), L"waitforsingleobject") == 0 ||
            _wcsicmp(symbol.name.c_str(), L"waitformultipleobjects") == 0 ||
            _wcsicmp(symbol.name.c_str(), L"sleep") == 0;
        const bool runtimeBasicsImport =
            _wcsicmp(symbol.name.c_str(), L"getmodulehandlew") == 0 ||
            _wcsicmp(symbol.name.c_str(), L"getmodulehandlea") == 0 ||
            _wcsicmp(symbol.name.c_str(), L"gettickcount") == 0 ||
            _wcsicmp(symbol.name.c_str(), L"gettickcount64") == 0 ||
            _wcsicmp(symbol.name.c_str(), L"queryperformancecounter") == 0 ||
            _wcsicmp(symbol.name.c_str(), L"queryperformancefrequency") == 0 ||
            _wcsicmp(symbol.name.c_str(), L"getcurrentthreadid") == 0 ||
            _wcsicmp(symbol.name.c_str(), L"getcurrentprocessid") == 0 ||
            _wcsicmp(symbol.name.c_str(), L"loadlibraryw") == 0 ||
            _wcsicmp(symbol.name.c_str(), L"loadlibrarya") == 0 ||
            _wcsicmp(symbol.name.c_str(), L"loadlibraryexw") == 0 ||
            _wcsicmp(symbol.name.c_str(), L"getprocaddress") == 0 ||
            _wcsicmp(symbol.name.c_str(), L"freelibrary") == 0 ||
            _wcsicmp(symbol.name.c_str(), L"getcommandlinew") == 0 ||
            _wcsicmp(symbol.name.c_str(), L"outputdebugstringw") == 0 ||
            _wcsicmp(symbol.name.c_str(), L"isdebuggerpresent") == 0;
        if (_wcsicmp(symbol.name.c_str(), L"closehandle") == 0)
        {
            resolution.note = L"Guest-handle adapter: closes virtual files or guest-local synchronization objects.";
        }
        else if (runtimeBasicsImport)
        {
            resolution.note = L"Core runtime adapter: guest module identity, monotonic time, local process/thread IDs, and diagnostics.";
        }
        else
        {
            resolution.note = synchronizationImport
                ? L"Kernel synchronization adapter: guest-local events, mutexes, waits, and cooperative sleep."
                : L"Virtual storage adapter: maps the guest C: drive to the app LocalFolder sandbox.";
        }
    }
    return resolution;
}
