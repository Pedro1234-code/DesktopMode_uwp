#pragma once

#include "Bridge\\VirtualPath.h"

#include <agile.h>
#include <windows.h>
#include <windows.storage.h>
#include <windows.storage.streams.h>

#include <cstddef>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace Win32Bridge
{
namespace Bridge
{
    // One storage environment belongs to one running guest.  It owns opaque
    // guest HANDLE values and maps virtual C: paths onto LocalFolder\drive_c.
    class GuestStorageContext final
    {
    public:
        GuestStorageContext(
            Windows::Storage::StorageFolder^ localFolder,
            const std::wstring& modulePath);
        ~GuestStorageContext();

        bool EnsureLayout(std::wstring* error);

        bool CreateFile(
            LPCWSTR fileName,
            DWORD desiredAccess,
            DWORD shareMode,
            DWORD creationDisposition,
            DWORD flagsAndAttributes,
            HANDLE templateFile,
            HANDLE* guestHandle,
            DWORD* win32Error);
        bool ReadFile(HANDLE guestHandle, void* buffer, DWORD bytesToRead, DWORD* bytesRead, DWORD* win32Error);
        bool WriteFile(HANDLE guestHandle, const void* buffer, DWORD bytesToWrite, DWORD* bytesWritten, DWORD* win32Error);
        bool CloseFile(HANDLE guestHandle, DWORD* win32Error);
        bool GetFileSize(HANDLE guestHandle, LARGE_INTEGER* fileSize, DWORD* win32Error);
        bool SetFilePointer(HANDLE guestHandle, LARGE_INTEGER distance, LARGE_INTEGER* newPosition, DWORD moveMethod, DWORD* win32Error);
        bool SetEndOfFile(HANDLE guestHandle, DWORD* win32Error);
        bool FlushFile(HANDLE guestHandle, DWORD* win32Error);

        // Internal module loading uses the same virtual filesystem as guest
        // CreateFile calls. It never exposes a host path to a PE.
        bool ReadAllBytes(LPCWSTR path, std::vector<BYTE>* bytes, DWORD* win32Error);
        bool CanonicalPath(LPCWSTR path, std::wstring* canonical, DWORD* win32Error) const;

        bool CreateDirectory(LPCWSTR path, DWORD* win32Error);
        bool DeleteGuestFile(LPCWSTR path, DWORD* win32Error);
        bool MoveGuestPath(LPCWSTR existingPath, LPCWSTR newPath, DWORD flags, DWORD* win32Error);
        bool RemoveGuestDirectory(LPCWSTR path, DWORD* win32Error);
        DWORD GetGuestFileAttributes(LPCWSTR path, DWORD* win32Error);
        bool SetGuestFileAttributes(LPCWSTR path, DWORD attributes, DWORD* win32Error);
        bool FindFirstGuestFile(
            LPCWSTR searchPattern,
            WIN32_FIND_DATAW* findData,
            HANDLE* guestHandle,
            DWORD* win32Error);
        bool FindNextGuestFile(HANDLE guestHandle, WIN32_FIND_DATAW* findData, DWORD* win32Error);
        bool CloseFindHandle(HANDLE guestHandle, DWORD* win32Error);
        bool SetCurrentDirectory(LPCWSTR path, DWORD* win32Error);
        // Each guest invocation starts from the same virtual working
        // directory chosen when its module was staged.  Files under drive_c
        // remain persistent; only the per-invocation path state is reset.
        bool ResetCurrentDirectory(DWORD* win32Error);
        std::wstring CurrentDirectory() const;
        const std::wstring& ModulePath() const { return m_modulePath; }
        Windows::Storage::StorageFolder^ LocalFolder() const { return m_localFolder.Get(); }
        std::wstring TempPath() const;

        void CloseAll();

    private:
        struct FileRecord;
        struct FindRecord;

        bool Resolve(LPCWSTR path, GuestPath* resolved, DWORD* win32Error) const;
        bool GetFolder(
            const std::vector<std::wstring>& physicalComponents,
            bool createMissing,
            Windows::Storage::StorageFolder^* folder,
            DWORD* win32Error) const;
        bool GetParentFolder(
            const GuestPath& path,
            Windows::Storage::StorageFolder^* parent,
            std::wstring* leafName,
            DWORD* win32Error) const;
        bool GetDirectoryFolder(
            const GuestPath& path,
            Windows::Storage::StorageFolder^* folder,
            DWORD* win32Error) const;
        bool AddFile(
            Windows::Storage::Streams::IRandomAccessStream^ stream,
            bool readable,
            bool writable,
            HANDLE* guestHandle,
            DWORD* win32Error);
        bool AddFind(
            const std::shared_ptr<FindRecord>& record,
            HANDLE* guestHandle,
            DWORD* win32Error);
        // m_handlesLock must be held.  File and directory-search handles share
        // one token namespace so a guest can never close one as the other.
        bool AllocateHandleLocked(ULONG_PTR* token);
        std::shared_ptr<FileRecord> LookupFile(HANDLE guestHandle) const;
        std::shared_ptr<FindRecord> LookupFind(HANDLE guestHandle) const;
        DWORD ApplyAttributeOverride(const std::wstring& canonicalPath, DWORD attributes) const;
        void StoreAttributeOverride(const std::wstring& canonicalPath, DWORD attributes);
        void RemoveAttributeOverrides(const std::wstring& canonicalPath, bool includeChildren);
        void MoveAttributeOverrides(
            const std::wstring& sourceCanonicalPath,
            const std::wstring& destinationCanonicalPath);

        // Keep storage tokens separate from GuestKernelContext, whose
        // synchronization-object namespace begins at 0x40000000.
        static constexpr size_t MaximumHandles = 1024;
        static constexpr ULONG_PTR FirstHandleToken = 0x10000;
        static constexpr ULONG_PTR FirstReservedHandleToken = 0x40000000;

        Platform::Agile<Windows::Storage::StorageFolder^> m_localFolder;
        VirtualPathResolver m_paths;
        std::wstring m_modulePath;
        std::wstring m_initialCurrentDirectory;
        mutable std::mutex m_handlesLock;
        std::unordered_map<ULONG_PTR, std::shared_ptr<FileRecord>> m_files;
        std::unordered_map<ULONG_PTR, std::shared_ptr<FindRecord>> m_finds;
        ULONG_PTR m_nextHandle = FirstHandleToken;
        mutable std::mutex m_metadataLock;
        std::unordered_map<std::wstring, DWORD> m_attributeOverrides;
    };

    // File shims resolve their environment through TLS, so a later CreateThread
    // adapter can explicitly propagate the appropriate guest context.
    GuestStorageContext* CurrentGuestStorageContext();

    class GuestStorageScope final
    {
    public:
        explicit GuestStorageScope(GuestStorageContext* context);
        ~GuestStorageScope();

        GuestStorageScope(const GuestStorageScope&) = delete;
        GuestStorageScope& operator=(const GuestStorageScope&) = delete;

    private:
        GuestStorageContext* m_previous;
    };
}
}
