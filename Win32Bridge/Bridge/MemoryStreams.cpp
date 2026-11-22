#if defined(__cplusplus_winrt)
#include "pch.h"
#endif
#include "Bridge/MemoryStreams.h"

#include <algorithm>
#include <atomic>
#include <cstring>
#include <cwchar>
#include <limits>
#include <memory>
#include <mutex>
#include <new>
#include <stdexcept>
#include <vector>
#include <utility>

using namespace Win32Bridge::Bridge;

namespace
{
    // Contracts compared with reactos-master/dll/win32/shcore/main.c and
    // shlwapi/istream.c. Windows 8+ also requires thread safety and Clone.
    struct MemoryStreamStorage
    {
        std::mutex mutex;
        std::vector<BYTE> bytes;
    };

    class MemoryStream final : public IStream
    {
    public:
        explicit MemoryStream(std::shared_ptr<MemoryStreamStorage> storage, ULONGLONG position = 0)
            : m_storage(std::move(storage)), m_position(position) {}

        HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void** result) override
        {
            if (!result) return E_POINTER;
            *result = nullptr;
            if (iid != __uuidof(IUnknown) && iid != __uuidof(ISequentialStream) && iid != __uuidof(IStream))
                return E_NOINTERFACE;
            *result = static_cast<IStream*>(this);
            AddRef();
            return S_OK;
        }
        ULONG STDMETHODCALLTYPE AddRef() override { return ++m_references; }
        ULONG STDMETHODCALLTYPE Release() override
        {
            const ULONG remaining = --m_references;
            if (!remaining) delete this;
            return remaining;
        }
        HRESULT STDMETHODCALLTYPE Read(void* destination, ULONG bytes, ULONG* read) override
        {
            if (read) *read = 0;
            if (bytes && !destination) return STG_E_INVALIDPOINTER;
            std::lock_guard<std::mutex> lock(m_storage->mutex);
            const ULONGLONG available = m_position < m_storage->bytes.size()
                ? m_storage->bytes.size() - m_position : 0;
            const ULONG count = static_cast<ULONG>((std::min)(available, static_cast<ULONGLONG>(bytes)));
            if (count) std::memcpy(destination, m_storage->bytes.data() + static_cast<size_t>(m_position), count);
            m_position += count;
            if (read) *read = count;
            return count == bytes ? S_OK : S_FALSE;
        }
        HRESULT STDMETHODCALLTYPE Write(const void* source, ULONG bytes, ULONG* written) override
        {
            if (written) *written = 0;
            if (bytes && !source) return STG_E_INVALIDPOINTER;
            if (!bytes) return S_OK;
            std::lock_guard<std::mutex> lock(m_storage->mutex);
            if (m_position > m_storage->bytes.max_size() ||
                bytes > m_storage->bytes.max_size() - m_position)
                return STG_E_INSUFFICIENTMEMORY;
            const size_t end = static_cast<size_t>(m_position + bytes);
            try
            {
                if (end > m_storage->bytes.size()) m_storage->bytes.resize(end, 0);
            }
            catch (const std::bad_alloc&) { return STG_E_INSUFFICIENTMEMORY; }
            catch (const std::length_error&) { return STG_E_INSUFFICIENTMEMORY; }
            std::memcpy(m_storage->bytes.data() + static_cast<size_t>(m_position), source, bytes);
            m_position = end;
            if (written) *written = bytes;
            return S_OK;
        }
        HRESULT STDMETHODCALLTYPE Seek(LARGE_INTEGER move, DWORD origin, ULARGE_INTEGER* position) override
        {
            std::lock_guard<std::mutex> lock(m_storage->mutex);
            ULONGLONG base = 0;
            switch (origin)
            {
            case STREAM_SEEK_SET: break;
            case STREAM_SEEK_CUR: base = m_position; break;
            case STREAM_SEEK_END: base = m_storage->bytes.size(); break;
            default: return STG_E_INVALIDPARAMETER;
            }
            ULONGLONG next = 0;
            if (move.QuadPart < 0)
            {
                const ULONGLONG distance = static_cast<ULONGLONG>(-(move.QuadPart + 1)) + 1;
                if (distance > base) return STG_E_INVALIDFUNCTION;
                next = base - distance;
            }
            else
            {
                const ULONGLONG distance = static_cast<ULONGLONG>(move.QuadPart);
                if (distance > (std::numeric_limits<ULONGLONG>::max)() - base)
                    return STG_E_INVALIDFUNCTION;
                next = base + distance;
            }
            m_position = next;
            if (position) position->QuadPart = next;
            return S_OK;
        }
        HRESULT STDMETHODCALLTYPE SetSize(ULARGE_INTEGER size) override
        {
            std::lock_guard<std::mutex> lock(m_storage->mutex);
            if (size.QuadPart > m_storage->bytes.max_size()) return STG_E_INSUFFICIENTMEMORY;
            try { m_storage->bytes.resize(static_cast<size_t>(size.QuadPart), 0); }
            catch (const std::bad_alloc&) { return STG_E_INSUFFICIENTMEMORY; }
            catch (const std::length_error&) { return STG_E_INSUFFICIENTMEMORY; }
            return S_OK;
        }
        HRESULT STDMETHODCALLTYPE CopyTo(IStream* destination, ULARGE_INTEGER size,
            ULARGE_INTEGER* read, ULARGE_INTEGER* written) override
        {
            if (read) read->QuadPart = 0;
            if (written) written->QuadPart = 0;
            if (!destination) return STG_E_INVALIDPOINTER;
            ULONGLONG available = 0;
            {
                std::lock_guard<std::mutex> lock(m_storage->mutex);
                available = m_position < m_storage->bytes.size() ? m_storage->bytes.size() - m_position : 0;
            }
            // Bound the initial extent so a copy to this stream or a clone
            // cannot keep extending the source indefinitely.
            const ULONGLONG limit = (std::min)(size.QuadPart, available);
            ULONGLONG totalRead = 0;
            ULONGLONG totalWritten = 0;
            HRESULT result = S_OK;
            BYTE buffer[4096];
            while (totalRead < limit)
            {
                const ULONG chunk = static_cast<ULONG>((std::min)(limit - totalRead,
                    static_cast<ULONGLONG>(sizeof(buffer))));
                ULONG received = 0;
                result = Read(buffer, chunk, &received);
                totalRead += received;
                if (FAILED(result)) break;
                if (received)
                {
                    ULONG sent = 0;
                    // External COM calls run after Read has released the lock.
                    result = destination->Write(buffer, received, &sent);
                    totalWritten += sent;
                    if (FAILED(result)) break;
                    if (sent != received) { result = STG_E_MEDIUMFULL; break; }
                }
                if (received < chunk) { result = S_FALSE; break; }
            }
            if (SUCCEEDED(result) && totalRead < size.QuadPart) result = S_FALSE;
            if (read) read->QuadPart = totalRead;
            if (written) written->QuadPart = totalWritten;
            return result;
        }
        HRESULT STDMETHODCALLTYPE Commit(DWORD) override { return E_NOTIMPL; }
        HRESULT STDMETHODCALLTYPE Revert() override { return E_NOTIMPL; }
        HRESULT STDMETHODCALLTYPE LockRegion(ULARGE_INTEGER, ULARGE_INTEGER, DWORD) override { return E_NOTIMPL; }
        HRESULT STDMETHODCALLTYPE UnlockRegion(ULARGE_INTEGER, ULARGE_INTEGER, DWORD) override { return E_NOTIMPL; }
        HRESULT STDMETHODCALLTYPE Stat(STATSTG* stat, DWORD flags) override
        {
            if (!stat) return STG_E_INVALIDPOINTER;
            if (flags != STATFLAG_DEFAULT && flags != STATFLAG_NONAME && flags != STATFLAG_NOOPEN)
                return STG_E_INVALIDFLAG;
            std::lock_guard<std::mutex> lock(m_storage->mutex);
            *stat = STATSTG{};
            stat->type = STGTY_STREAM;
            stat->cbSize.QuadPart = m_storage->bytes.size();
            stat->grfMode = STGM_READWRITE;
            return S_OK;
        }
        HRESULT STDMETHODCALLTYPE Clone(IStream** result) override
        {
            if (!result) return E_POINTER;
            *result = nullptr;
            std::lock_guard<std::mutex> lock(m_storage->mutex);
            auto clone = new (std::nothrow) MemoryStream(m_storage, m_position);
            if (!clone) return E_OUTOFMEMORY;
            *result = clone;
            return S_OK;
        }

    private:
        std::atomic<ULONG> m_references{ 1 };
        std::shared_ptr<MemoryStreamStorage> m_storage;
        ULONGLONG m_position;
    };
}

IStream* WINAPI Win32Bridge::Bridge::BridgeSHCreateMemStream(const BYTE* initialData, UINT initialBytes)
{
    if (!initialData && initialBytes) return nullptr;
    try
    {
        auto storage = std::make_shared<MemoryStreamStorage>();
        if (initialBytes) storage->bytes.assign(initialData, initialData + initialBytes);
        return new (std::nothrow) MemoryStream(std::move(storage));
    }
    catch (const std::bad_alloc&) { return nullptr; }
    catch (const std::length_error&) { return nullptr; }
}

HRESULT WINAPI Win32Bridge::Bridge::BridgeIStreamRead(IStream* stream, void* destination, ULONG bytes)
{
    if (!stream) return E_INVALIDARG;
    ULONG read = 0;
    const HRESULT result = stream->Read(destination, bytes, &read);
    return SUCCEEDED(result) && read != bytes ? E_FAIL : result;
}
HRESULT WINAPI Win32Bridge::Bridge::BridgeIStreamWrite(IStream* stream, const void* source, ULONG bytes)
{
    if (!stream) return E_INVALIDARG;
    ULONG written = 0;
    const HRESULT result = stream->Write(source, bytes, &written);
    return SUCCEEDED(result) && written != bytes ? E_FAIL : result;
}
HRESULT WINAPI Win32Bridge::Bridge::BridgeIStreamReset(IStream* stream)
{
    if (!stream) return E_INVALIDARG;
    LARGE_INTEGER zero{};
    return stream->Seek(zero, STREAM_SEEK_SET, nullptr);
}
HRESULT WINAPI Win32Bridge::Bridge::BridgeIStreamSize(IStream* stream, ULARGE_INTEGER* size)
{
    if (!stream) return E_INVALIDARG;
    STATSTG stat{};
    const HRESULT result = stream->Stat(&stat, STATFLAG_NONAME);
    if (SUCCEEDED(result) && size) *size = stat.cbSize;
    return result;
}

ImportResolution Win32Bridge::Bridge::ResolveMemoryStreamImport(const ImportedSymbol& symbol)
{
    auto result = CompatibilityCatalog::Resolve(symbol);
    if (symbol.importedByOrdinal || (_wcsicmp(symbol.library.c_str(), L"shcore.dll") != 0 &&
        _wcsicmp(symbol.library.c_str(), L"shlwapi.dll") != 0)) return result;
    if (symbol.name == L"SHCreateMemStream") result.targetAddress = reinterpret_cast<ULONGLONG>(&BridgeSHCreateMemStream);
    else if (symbol.name == L"IStream_Read") result.targetAddress = reinterpret_cast<ULONGLONG>(&BridgeIStreamRead);
    else if (symbol.name == L"IStream_Write") result.targetAddress = reinterpret_cast<ULONGLONG>(&BridgeIStreamWrite);
    else if (symbol.name == L"IStream_Reset") result.targetAddress = reinterpret_cast<ULONGLONG>(&BridgeIStreamReset);
    else if (symbol.name == L"IStream_Size") result.targetAddress = reinterpret_cast<ULONGLONG>(&BridgeIStreamSize);
    if (result.targetAddress)
    {
        result.disposition = ImportDisposition::NeedsBridge;
        result.note = L"Guest memory stream with COM IStream semantics.";
    }
    return result;
}