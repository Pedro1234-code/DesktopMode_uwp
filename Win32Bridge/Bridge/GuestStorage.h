#pragma once

#include "Bridge\\VirtualPath.h"
#include "Bridge/GuestKernel.h"

#include <agile.h>
#include <windows.h>
#include <windows.storage.h>
#include <windows.storage.streams.h>

#include <cstddef>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace Concurrency { class cancellation_token; }

namespace Win32Bridge
{
namespace Bridge
{
    class GuestIoCompletionPort;
    struct GuestIoCompletionBinding;

    // One storage environment belongs to one running guest.  It owns opaque
    // guest HANDLE values and maps virtual C: paths onto LocalFolder\drive_c.
    class GuestStorageContext final
    {
        struct FileRecord;
        struct FileRangeState;
        struct SharedFiles;
        struct MappedFileState;

    public:
        class MappedFile final
        {
        public:
            explicit operator bool() const { return m_state != nullptr; }
            DWORD Populate(BYTE* buffer, SIZE_T size) const;
            DWORD Flush(ULONGLONG offset, const BYTE* buffer, SIZE_T size) const;

        private:
            friend class GuestStorageContext;
            std::shared_ptr<MappedFileState> m_state;
        };

        class DiskFile final
        {
        public:
            bool IsOverlapped() const;
            bool IsAppendOnly() const;
            DWORD ValidateAccess(bool writing) const;
            DWORD ValidateLockAccess() const;
            const void* Identity() const { return m_record.get(); }

        private:
            friend class GuestStorageContext;
            std::shared_ptr<FileRecord> m_record;
            std::shared_ptr<SharedFiles> m_namespace;
            const GuestStorageContext* m_owner = nullptr;
        };

        GuestStorageContext(
            Windows::Storage::StorageFolder^ localFolder,
            const std::wstring& modulePath,
            Windows::Storage::StorageFolder^ moduleSourceFolder = nullptr,
            Windows::Storage::StorageFolder^ externalFolder = nullptr);
        ~GuestStorageContext();

        // Bound individual WinRT buffers; callers can stream larger requests.
        static constexpr DWORD MaximumTransferSize() { return 16u * 1024u * 1024u; }
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
        bool ReadFileAt(HANDLE guestHandle, void* buffer, DWORD bytesToRead,
            ULONGLONG offset, DWORD* bytesRead, DWORD* win32Error);
        bool WriteFile(HANDLE guestHandle, const void* buffer, DWORD bytesToWrite, DWORD* bytesWritten, DWORD* win32Error);
        bool WriteFileAt(HANDLE guestHandle, const void* buffer, DWORD bytesToWrite,
            ULONGLONG offset, DWORD* bytesWritten, DWORD* win32Error);
        DWORD CaptureMappedFile(HANDLE file, bool writable, bool executable,
            MappedFile& backing, ULONGLONG& size) const;
        bool CloseFile(HANDLE guestHandle, DWORD* win32Error);
        bool OwnsFile(HANDLE guestHandle) const;
        DWORD FileDataAccess(HANDLE guestHandle) const;
        std::shared_ptr<const void> RetainFileDescription(HANDLE file) const;
        DiskFile CaptureDiskFile(HANDLE file) const;
        GuestKernelContext::FileSignal LookupFileSignal(HANDLE file) const;
        static constexpr ULONGLONG UseCurrentFilePosition = ~1ull;
        static DWORD TransferDiskFile(const DiskFile& file, bool writing, ULONGLONG offset, BYTE* buffer,
            DWORD length, DWORD& transferred, const Concurrency::cancellation_token& cancellation,
            bool updateFilePosition = false, ULONG lockKey = 0);
        bool BindIoCompletionPort(HANDLE file, const std::shared_ptr<GuestIoCompletionPort>& port,
            ULONG_PTR key, DWORD* win32Error);
        bool SetIoCompletionModes(HANDLE file, UCHAR modes, DWORD* win32Error);
        GuestIoCompletionBinding IoCompletionBinding(HANDLE file) const;
        bool FileHandleFlags(HANDLE guestHandle, DWORD* flags, DWORD mask, bool set, DWORD* win32Error);
        bool DuplicateFile(HANDLE source, bool inherit, HANDLE* duplicate, DWORD* win32Error);
        bool InheritFiles(const GuestStorageContext& parent, DWORD* win32Error);
        bool GetFileSize(HANDLE guestHandle, LARGE_INTEGER* fileSize, DWORD* win32Error);
        bool SetFilePointer(HANDLE guestHandle, LARGE_INTEGER distance, LARGE_INTEGER* newPosition, DWORD moveMethod, DWORD* win32Error);
        bool SetEndOfFile(HANDLE guestHandle, DWORD* win32Error);
        bool SetFileSize(HANDLE guestHandle, LARGE_INTEGER size, DWORD* win32Error);
        bool SetFileDisposition(HANDLE guestHandle, bool remove, DWORD* win32Error);
        bool GetFileStandardInfo(HANDLE guestHandle, FILE_STANDARD_INFO* info, DWORD* win32Error) const;
        bool RenameFile(HANDLE guestHandle, HANDLE root, LPCWSTR name, bool replace,
            bool nativeName, DWORD* win32Error);
        bool FlushFile(HANDLE guestHandle, DWORD* win32Error);
        bool GetFilePath(HANDLE guestHandle, std::wstring* path, DWORD* win32Error) const;
        bool IsDirectoryHandle(HANDLE guestHandle, DWORD* win32Error) const;
        bool LockFileRange(HANDLE guestHandle, ULONGLONG offset, ULONGLONG length,
            bool exclusive, bool failImmediately, DWORD* win32Error, ULONG key = 0);
        static DWORD LockDiskRange(const DiskFile& file, ULONGLONG offset, ULONGLONG length,
            bool exclusive, bool failImmediately, ULONG key, const Concurrency::cancellation_token& cancellation,
            HANDLE callerHandle = nullptr);
        bool UnlockFileRange(HANDLE guestHandle, ULONGLONG offset, ULONGLONG length,
            DWORD* win32Error, ULONG key = 0);

        // Internal module loading uses the same virtual filesystem as guest
        // CreateFile calls. It never exposes a host path to a PE.
        bool ReadAllBytes(LPCWSTR path, std::vector<BYTE>* bytes, DWORD* win32Error);
        bool CanonicalPath(LPCWSTR path, std::wstring* canonical, DWORD* win32Error) const;

        bool CreateDirectory(LPCWSTR path, DWORD* win32Error);
        bool DeleteGuestFile(LPCWSTR path, DWORD* win32Error);
        bool MoveGuestPath(LPCWSTR existingPath, LPCWSTR newPath, DWORD flags, DWORD* win32Error);
        bool ReplaceGuestFile(LPCWSTR replaced, LPCWSTR replacement, LPCWSTR backup, DWORD* win32Error);
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
        Windows::Storage::StorageFolder^ ExternalFolder() const { return m_externalFolder.Get(); }
        std::wstring TempPath() const;

        void CloseAll();

    private:
        void ReleaseFileRanges(const std::shared_ptr<FileRecord>& record);
        static std::shared_ptr<SharedFiles> AcquireFileNamespace(Windows::Storage::StorageFolder^ root);
        bool RegisterFileLocked(const std::shared_ptr<FileRecord>& record, DWORD flags, HANDLE* value, DWORD* error);
        bool CloseDescription(const std::shared_ptr<FileRecord>& record, DWORD* error);
        static std::wstring FilePathSnapshot(SharedFiles& files, const FileRecord& record);
        bool DeleteGuestFileLocked(const GuestPath& path, DWORD* win32Error);
        bool RemoveGuestDirectoryLocked(const GuestPath& path, bool validateOnly, DWORD* win32Error);
        bool IsDeletePendingLocked(const std::wstring& path, bool includeParents = false) const;
        bool SetFileEnd(HANDLE guestHandle, const LARGE_INTEGER* size, DWORD* win32Error);
        bool MoveGuestPathLocked(const GuestPath& source, const GuestPath& destination, DWORD flags,
            bool sourceWasLowerLayerFile, const FileRecord* initiator, DWORD* win32Error);
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
        bool GetAuthorizedFolder(
            const GuestPath& path,
            Windows::Storage::StorageFolder^* folder) const;
        bool OpenAuthorizedFile(
            const GuestPath& path,
            DWORD desiredAccess,
            DWORD shareMode,
            DWORD flags,
            HANDLE* guestHandle,
            DWORD* win32Error);
        bool AddFile(
            Windows::Storage::Streams::IRandomAccessStream^ stream,
            bool readable,
            bool writable,
            const std::wstring& canonicalPath,
            DWORD desiredAccess,
            DWORD shareMode,
            DWORD flags,
            HANDLE* guestHandle,
            DWORD* win32Error);
        bool AddDirectory(
            Windows::Storage::StorageFolder^ folder,
            const std::wstring& canonicalPath,
            DWORD desiredAccess,
            DWORD shareMode,
            DWORD flags,
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
        bool IsLowerLayerTombstoned(const std::wstring& canonicalPath) const;
        void SetLowerLayerTombstone(const std::wstring& canonicalPath, bool present);

        // Keep storage tokens separate from GuestKernelContext, whose
        // synchronization-object namespace begins at 0x40000000.
        static constexpr size_t MaximumHandles = 1024;
        static constexpr ULONG_PTR FirstHandleToken = 0x10000;
        static constexpr ULONG_PTR FirstReservedHandleToken = 0x40000000;

        std::shared_ptr<SharedFiles> m_sharedFiles;
        Platform::Agile<Windows::Storage::StorageFolder^> m_localFolder;
        Platform::Agile<Windows::Storage::StorageFolder^> m_moduleSourceFolder;
        Platform::Agile<Windows::Storage::StorageFolder^> m_externalFolder;
        VirtualPathResolver m_paths;
        std::wstring m_modulePath;
        std::wstring m_moduleDirectory;
        size_t m_moduleDirectoryComponentCount = 0;
        std::wstring m_initialCurrentDirectory;
        std::mutex& m_handlesLock;
        std::unordered_map<ULONG_PTR, std::shared_ptr<FileRecord>> m_files;
        std::unordered_map<ULONG_PTR, DWORD> m_fileFlags;
        std::unordered_set<std::wstring>& m_deletePending;
        std::unordered_map<ULONG_PTR, std::shared_ptr<FindRecord>> m_finds;
        mutable std::mutex m_metadataLock;
        std::unordered_map<std::wstring, DWORD> m_attributeOverrides;
        std::unordered_set<std::wstring> m_lowerLayerTombstones;
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
