#include "pch.h"
#include "Bridge/FileStreams.h"
#include "Bridge/Kernel32Shims.h"
#include "Bridge/GuestModule.h"
#include <algorithm>
#include <atomic>
#include <memory>
#include <mutex>
#include <new>
#include <limits>

using namespace Win32Bridge::Bridge;
namespace
{
    struct FileStorage
    {
        std::shared_ptr<GuestStorageContext> context;
        HANDLE file = INVALID_HANDLE_VALUE;
        bool owned = false;
        DWORD access = GENERIC_READ;
        std::mutex mutex;
        ~FileStorage() { GuestStorageScope scope(context.get()); if (owned) BridgeCloseHandle(file); }
    };
    HRESULT FileError() { return HRESULT_FROM_WIN32(BridgeGetLastError()); }
    class FileStream final : public IStream
    {
    public:
        explicit FileStream(std::shared_ptr<FileStorage> storage, ULONGLONG position = 0)
            : m_storage(std::move(storage)), m_position(position) {}
        HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void** result) override
        {
            if (!result) return E_POINTER;
            *result = nullptr;
            if (iid != __uuidof(IUnknown) && iid != __uuidof(IStream) && iid != __uuidof(ISequentialStream)) return E_NOINTERFACE;
            *result = static_cast<IStream*>(this); AddRef(); return S_OK;
        }
        ULONG STDMETHODCALLTYPE AddRef() override { return ++m_refs; }
        ULONG STDMETHODCALLTYPE Release() override { const ULONG refs = --m_refs; if (!refs) delete this; return refs; }
        HRESULT STDMETHODCALLTYPE Read(void* buffer, ULONG bytes, ULONG* count) override
        {
            if (count) *count = 0;
            if (bytes && !buffer) return STG_E_INVALIDPOINTER;
            GuestStorageScope scope(m_storage->context.get());
            std::lock_guard<std::mutex> lock(m_storage->mutex);
            LARGE_INTEGER position{}; position.QuadPart = static_cast<LONGLONG>(m_position);
            if (!BridgeSetFilePointerEx(m_storage->file, position, nullptr, FILE_BEGIN)) return FileError();
            DWORD read = 0;
            if (!BridgeReadFile(m_storage->file, buffer, bytes, &read, nullptr)) return FileError();
            m_position += read;
            if (count) *count = read;
            return read == bytes ? S_OK : S_FALSE;
        }
        HRESULT STDMETHODCALLTYPE Write(const void* buffer, ULONG bytes, ULONG* count) override
        {
            if (count) *count = 0;
            if (bytes && !buffer) return STG_E_INVALIDPOINTER;
            GuestStorageScope scope(m_storage->context.get());
            std::lock_guard<std::mutex> lock(m_storage->mutex);
            LARGE_INTEGER position{}; position.QuadPart = static_cast<LONGLONG>(m_position);
            if (!BridgeSetFilePointerEx(m_storage->file, position, nullptr, FILE_BEGIN)) return FileError();
            DWORD written = 0;
            if (!BridgeWriteFile(m_storage->file, buffer, bytes, &written, nullptr)) return FileError();
            m_position += written;
            if (count) *count = written;
            return written == bytes ? S_OK : STG_E_MEDIUMFULL;
        }
        HRESULT STDMETHODCALLTYPE Seek(LARGE_INTEGER move, DWORD origin, ULARGE_INTEGER* result) override
        {
            GuestStorageScope scope(m_storage->context.get());
            std::lock_guard<std::mutex> lock(m_storage->mutex);
            LONGLONG base = 0;
            if (origin == STREAM_SEEK_CUR) base = static_cast<LONGLONG>(m_position);
            else if (origin == STREAM_SEEK_END)
            {
                LARGE_INTEGER size{};
                if (!BridgeGetFileSizeEx(m_storage->file, &size)) return FileError();
                base = size.QuadPart;
            }
            else if (origin != STREAM_SEEK_SET) return STG_E_INVALIDFUNCTION;
            if ((move.QuadPart < 0 && move.QuadPart < -base) ||
                (move.QuadPart > 0 && base > (std::numeric_limits<LONGLONG>::max)() - move.QuadPart)) return STG_E_INVALIDFUNCTION;
            m_position = static_cast<ULONGLONG>(base + move.QuadPart);
            if (result) result->QuadPart = m_position;
            return S_OK;
        }
        HRESULT STDMETHODCALLTYPE SetSize(ULARGE_INTEGER size) override
        {
            if (size.QuadPart > static_cast<ULONGLONG>((std::numeric_limits<LONGLONG>::max)())) return STG_E_INVALIDFUNCTION;
            GuestStorageScope scope(m_storage->context.get());
            std::lock_guard<std::mutex> lock(m_storage->mutex);
            LARGE_INTEGER position{}; position.QuadPart = static_cast<LONGLONG>(size.QuadPart);
            if (!BridgeSetFilePointerEx(m_storage->file, position, nullptr, FILE_BEGIN) ||
                !BridgeSetEndOfFile(m_storage->file)) return FileError();
            return S_OK;
        }
        HRESULT STDMETHODCALLTYPE CopyTo(IStream* target, ULARGE_INTEGER requested, ULARGE_INTEGER* read, ULARGE_INTEGER* written) override
        {
            if (read) read->QuadPart = 0;
            if (written) written->QuadPart = 0;
            if (!target) return STG_E_INVALIDPOINTER;
            ULONGLONG limit = 0;
            {
                GuestStorageScope scope(m_storage->context.get());
            std::lock_guard<std::mutex> lock(m_storage->mutex);
                LARGE_INTEGER size{};
                if (!BridgeGetFileSizeEx(m_storage->file, &size)) return FileError();
                limit = m_position < static_cast<ULONGLONG>(size.QuadPart) ? size.QuadPart - m_position : 0;
            }
            limit = (std::min)(limit, requested.QuadPart);
            ULONGLONG totalRead = 0, totalWritten = 0;
            HRESULT status = S_OK;
            BYTE buffer[4096];
            while (totalRead < limit)
            {
                ULONG received = 0, sent = 0;
                const ULONG chunk = static_cast<ULONG>((std::min)(limit - totalRead, static_cast<ULONGLONG>(sizeof(buffer))));
                status = Read(buffer, chunk, &received); totalRead += received;
                if (FAILED(status) || !received) break;
                status = target->Write(buffer, received, &sent); totalWritten += sent;
                if (FAILED(status)) break;
                if (sent != received) { status = STG_E_MEDIUMFULL; break; }
            }
            if (read) read->QuadPart = totalRead;
            if (written) written->QuadPart = totalWritten;
            return SUCCEEDED(status) && totalRead < requested.QuadPart ? S_FALSE : status;
        }
        HRESULT STDMETHODCALLTYPE Commit(DWORD) override { GuestStorageScope scope(m_storage->context.get()); return BridgeFlushFileBuffers(m_storage->file) ? S_OK : FileError(); }
        HRESULT STDMETHODCALLTYPE Revert() override { return STG_E_INVALIDFUNCTION; }
        HRESULT STDMETHODCALLTYPE LockRegion(ULARGE_INTEGER, ULARGE_INTEGER, DWORD) override { return STG_E_INVALIDFUNCTION; }
        HRESULT STDMETHODCALLTYPE UnlockRegion(ULARGE_INTEGER, ULARGE_INTEGER, DWORD) override { return STG_E_INVALIDFUNCTION; }
        HRESULT STDMETHODCALLTYPE Stat(STATSTG* result, DWORD flags) override
        {
            if (!result) return STG_E_INVALIDPOINTER;
            *result = STATSTG{};
            if (flags > STATFLAG_NOOPEN) return STG_E_INVALIDFLAG;
            GuestStorageScope scope(m_storage->context.get());
            std::lock_guard<std::mutex> lock(m_storage->mutex);
            LARGE_INTEGER size{};
            if (!BridgeGetFileSizeEx(m_storage->file, &size)) return FileError();
            result->type = STGTY_STREAM; result->cbSize.QuadPart = size.QuadPart;
            result->grfMode = (m_storage->access & GENERIC_WRITE) ?
                ((m_storage->access & GENERIC_READ) ? STGM_READWRITE : STGM_WRITE) : STGM_READ;
            return S_OK;
        }
        HRESULT STDMETHODCALLTYPE Clone(IStream** result) override
        {
            if (!result) return E_POINTER;
            *result = nullptr;
            GuestStorageScope scope(m_storage->context.get());
            std::lock_guard<std::mutex> lock(m_storage->mutex);
            *result = new (std::nothrow) FileStream(m_storage, m_position);
            return *result ? S_OK : E_OUTOFMEMORY;
        }
    private:
        std::atomic<ULONG> m_refs{1};
        std::shared_ptr<FileStorage> m_storage;
        ULONGLONG m_position;
    };
    HRESULT MakeFileStream(HANDLE file, bool owned, DWORD access, IStream** result, const std::shared_ptr<GuestStorageContext>& context)
    {
        if (!result) { if (owned) BridgeCloseHandle(file); return E_POINTER; }
        *result = nullptr;
        std::shared_ptr<FileStorage> storage;
        try { storage = std::make_shared<FileStorage>(); }
        catch (const std::bad_alloc&) { if (owned) BridgeCloseHandle(file); return E_OUTOFMEMORY; }
        storage->context = context; storage->file = file; storage->owned = owned; storage->access = access;
        *result = new (std::nothrow) FileStream(std::move(storage));
        return *result ? S_OK : E_OUTOFMEMORY;
    }
}
HRESULT Win32Bridge::Bridge::OpenGuestFileStream(LPCWSTR path, DWORD access, IStream** result, const std::shared_ptr<GuestStorageContext>& storage)
{
    if (!result) return E_POINTER;
    *result = nullptr;
    auto context = storage ? storage : (CurrentGuestModuleLoader() ? CurrentGuestModuleLoader()->StorageContext() : nullptr);
    if (!context) return E_UNEXPECTED;
    GuestStorageScope scope(context.get());
    if (!path || !*path || !access || (access & ~(GENERIC_READ | GENERIC_WRITE))) return E_INVALIDARG;
    HANDLE file = BridgeCreateFileW(path, access, FILE_SHARE_READ, nullptr,
        (access & GENERIC_WRITE) ? CREATE_ALWAYS : OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return FileError();
    return MakeFileStream(file, true, access, result, context);
}
HRESULT Win32Bridge::Bridge::BorrowGuestFileStream(HANDLE file, IStream** result, const std::shared_ptr<GuestStorageContext>& storage)
{
    if (!result) return E_POINTER;
    *result = nullptr;
    auto context = storage ? storage : (CurrentGuestModuleLoader() ? CurrentGuestModuleLoader()->StorageContext() : nullptr);
    if (!context) return E_UNEXPECTED;
    GuestStorageScope scope(context.get());
    LARGE_INTEGER size{};
    if (!BridgeGetFileSizeEx(file, &size)) return FileError();
    return MakeFileStream(file, false, GENERIC_READ, result, context);
}