#include "pch.h"
#include "Bridge/CRT/CrtFileIo.h"
#include "Bridge/CRT/CrtExceptions.h"
#include "Bridge/GuestStorage.h"
#include "Bridge/GuestKernel.h"
#include "Bridge/Kernel32Shims.h"

namespace Win32Bridge { namespace Bridge {
namespace {
class GuestCrtFileBackend final : public CrtFileBackend
{
public:
    explicit GuestCrtFileBackend(std::shared_ptr<GuestStorageContext> storage, std::shared_ptr<GuestKernelContext> kernel)
        : m_storage(std::move(storage)), m_kernel(std::move(kernel)) { }
    HANDLE Standard(unsigned index) override
    { DWORD error = ERROR_SUCCESS; return m_kernel && index < 3 ? m_kernel->Console().GetStandard(static_cast<DWORD>(-10 - static_cast<int>(index)), &error) : nullptr; }
    bool SetStandard(unsigned index, HANDLE value, DWORD* error) override
    { if (!m_kernel) { *error = ERROR_INVALID_FUNCTION; return false; }
      return m_kernel->Console().SetStandard(static_cast<DWORD>(-10 - static_cast<int>(index)), value, error); }
    DWORD Type(HANDLE value, DWORD* error) override
    {
        if (m_kernel && m_kernel->Console().Owns(value)) { *error = ERROR_SUCCESS; return FILE_TYPE_CHAR; }
        if (m_kernel && m_kernel->Pipes().Owns(value)) { *error = ERROR_SUCCESS; return FILE_TYPE_PIPE; }
        if (m_storage && m_storage->OwnsFile(value)) { *error = ERROR_SUCCESS; return FILE_TYPE_DISK; }
        *error = ERROR_INVALID_HANDLE; return FILE_TYPE_UNKNOWN;
    }
    DWORD DataAccess(HANDLE value) override
    {
        if (m_kernel)
        {
            const DWORD consoleAccess = m_kernel->Console().DataAccess(value);
            if (consoleAccess) return consoleAccess;
            const DWORD pipeAccess = m_kernel->Pipes().DataAccess(value);
            if (pipeAccess) return pipeAccess;
        }
        return m_storage ? m_storage->FileDataAccess(value) : 0;
    }
    bool SetInherit(HANDLE value, bool inherit, DWORD* error) override
    {
        DWORD flags = inherit ? 1u : 0u;
        if (m_kernel && m_kernel->Console().Owns(value)) return m_kernel->Console().Flags(value, &flags, 1, true, error);
        if (m_kernel && m_kernel->Pipes().Owns(value)) return m_kernel->Pipes().Flags(value, &flags, 1, true, error);
        return Ready(error) && m_storage->FileHandleFlags(value, &flags, 1, true, error);
    }
    bool Duplicate(HANDLE value, bool inherit, HANDLE* result, DWORD* error) override
    {
        if (m_kernel && m_kernel->Console().Owns(value)) return m_kernel->Console().Duplicate(value, inherit, result, error);
        if (m_kernel && m_kernel->Pipes().Owns(value)) return m_kernel->Pipes().Duplicate(value, inherit, result, error);
        return Ready(error) && m_storage->DuplicateFile(value, inherit, result, error);
    }
    bool ReadWide(HANDLE value, wchar_t* buffer, DWORD count, DWORD* read, DWORD* error) override
    {
        if (!Current(error)) return false;
        const bool result = BridgeReadConsoleW(value, buffer, count, read, nullptr) != FALSE;
        *error = result ? ERROR_SUCCESS : BridgeGetLastError(); return result;
    }
    bool WriteWide(HANDLE value, const wchar_t* buffer, DWORD count, DWORD* written, DWORD* error) override
    {
        if (!Current(error)) return false;
        const bool result = BridgeWriteConsoleW(value, buffer, count, written, nullptr) != FALSE;
        *error = result ? ERROR_SUCCESS : BridgeGetLastError(); return result;
    }
    UINT ConsoleOutputCodePage() override { return m_kernel ? m_kernel->Console().CodePage(true) : 0; }
    UINT ConsoleLocaleCodePage() override { return CurrentCrtConsoleSourcePage(); }
    bool Open(const wchar_t* path, DWORD access, DWORD share, DWORD create, DWORD attributes, HANDLE* handle, DWORD* error) override
    {
        if (!Ready(error)) return false;
        return m_storage->CreateFile(path, access, share, create, attributes, nullptr, handle, error);
    }
    bool Close(HANDLE handle, DWORD* error) override
    {
        if (m_kernel && m_kernel->Pipes().Owns(handle)) return m_kernel->Pipes().Close(handle, error);
        if (m_kernel && m_kernel->Console().Owns(handle)) return m_kernel->Console().Close(handle, error);
        return Ready(error) && m_storage->CloseFile(handle, error);
    }
    bool Read(HANDLE handle, void* buffer, DWORD count, DWORD* read, DWORD* error) override
    {
        if (m_kernel && (m_kernel->Pipes().Owns(handle) || m_kernel->Console().Owns(handle)))
        {
            if (!Current(error)) return false;
            const bool result = BridgeReadFile(handle, buffer, count, read, nullptr) != FALSE;
            if (!result && BridgeGetLastError() == ERROR_BROKEN_PIPE) { *read = 0; *error = ERROR_SUCCESS; return true; }
            *error = result ? ERROR_SUCCESS : BridgeGetLastError(); return result;
        }
        if (!Ready(error)) return false;
        *read = 0;
        auto bytes = static_cast<BYTE*>(buffer);
        while (*read < count)
        {
            const DWORD chunk = (std::min)(count - *read, GuestStorageContext::MaximumTransferSize());
            DWORD transferred = 0;
            if (!m_storage->ReadFile(handle, bytes + *read, chunk, &transferred, error)) return *read != 0;
            *read += transferred;
            if (transferred < chunk) break;
        }
        return true;
    }
    bool Write(HANDLE handle, const void* buffer, DWORD count, DWORD* written, DWORD* error) override
    {
        if (m_kernel && (m_kernel->Pipes().Owns(handle) || m_kernel->Console().Owns(handle)))
        {
            if (!Current(error)) return false;
            const bool result = BridgeWriteFile(handle, buffer, count, written, nullptr) != FALSE;
            *error = result ? ERROR_SUCCESS : BridgeGetLastError(); return result;
        }
        if (!Ready(error)) return false;
        *written = 0;
        auto bytes = static_cast<const BYTE*>(buffer);
        while (*written < count)
        {
            const DWORD chunk = (std::min)(count - *written, GuestStorageContext::MaximumTransferSize());
            DWORD transferred = 0;
            if (!m_storage->WriteFile(handle, bytes + *written, chunk, &transferred, error)) return *written != 0;
            *written += transferred;
            if (transferred < chunk) break;
        }
        return true;
    }
    bool Seek(HANDLE handle, LONGLONG offset, DWORD origin, LONGLONG* result, DWORD* error) override
    {
        if (!Ready(error)) return false;
        if (Type(handle, error) != FILE_TYPE_DISK) { *error = ERROR_INVALID_FUNCTION; return false; }
        LARGE_INTEGER distance{}, position{}; distance.QuadPart = offset;
        if (!m_storage->SetFilePointer(handle, distance, &position, origin, error)) return false;
        *result = position.QuadPart; return true;
    }
    bool Size(HANDLE handle, LONGLONG* size, DWORD* error) override
    {
        if (!Ready(error)) return false;
        if (Type(handle, error) != FILE_TYPE_DISK) { *error = ERROR_INVALID_FUNCTION; return false; }
        LARGE_INTEGER result{};
        if (!m_storage->GetFileSize(handle, &result, error)) return false;
        *size = result.QuadPart; return true;
    }
    bool Resize(HANDLE handle, LONGLONG size, DWORD* error) override
    {
        LONGLONG previous, ignored;
        if (!Seek(handle, 0, FILE_CURRENT, &previous, error) || !Seek(handle, size, FILE_BEGIN, &ignored, error)) return false;
        const bool resized = m_storage->SetEndOfFile(handle, error);
        DWORD restoreError = ERROR_SUCCESS;
        const bool restored = Seek(handle, previous, FILE_BEGIN, &ignored, &restoreError);
        if (resized && !restored) *error = restoreError;
        return resized && restored;
    }
    bool Flush(HANDLE handle, DWORD* error) override { return Ready(error) && m_storage->FlushFile(handle, error); }
private:
    bool Current(DWORD* error) const
    {
        if (m_kernel && CurrentGuestKernelContext() == m_kernel.get()) return true;
        *error = ERROR_INVALID_FUNCTION; return false;
    }
    bool Ready(DWORD* error) const
    {
        if (m_storage) return true;
        *error = ERROR_INVALID_FUNCTION; return false;
    }
    std::shared_ptr<GuestStorageContext> m_storage;
    std::shared_ptr<GuestKernelContext> m_kernel;
};
}
std::shared_ptr<CrtFileBackend> CreateGuestCrtFileBackend(std::shared_ptr<GuestStorageContext> storage, std::shared_ptr<GuestKernelContext> kernel)
{
    return std::make_shared<GuestCrtFileBackend>(std::move(storage), std::move(kernel));
}
} }
