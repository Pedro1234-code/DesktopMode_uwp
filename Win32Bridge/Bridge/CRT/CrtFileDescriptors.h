#pragma once
#include <windows.h>
#include "Bridge/CRT/CrtStartupFiles.h"
#include "Bridge/CRT/CrtTextEncoding.h"
#include <fcntl.h>
#include <share.h>
#include <cerrno>
#include <cstring>
#include <climits>
#include <cstdint>
#include <cstdlib>
#include <map>
#include <deque>
#include <memory>
#include <mutex>
#include <new>
#include <vector>
#include <algorithm>

namespace Win32Bridge { namespace Bridge {

inline int CrtIoFailure(DWORD error)
{
    int value = EIO;
    switch (error)
    {
    case ERROR_FILE_NOT_FOUND: case ERROR_PATH_NOT_FOUND: case ERROR_INVALID_DRIVE: value = ENOENT; break;
    case ERROR_TOO_MANY_OPEN_FILES: value = EMFILE; break;
    case ERROR_ACCESS_DENIED: case ERROR_SHARING_VIOLATION: case ERROR_LOCK_VIOLATION: case ERROR_WRITE_PROTECT: value = EACCES; break;
    case ERROR_INVALID_HANDLE: value = EBADF; break;
    case ERROR_NOT_ENOUGH_MEMORY: case ERROR_OUTOFMEMORY: value = ENOMEM; break;
    case ERROR_FILE_EXISTS: case ERROR_ALREADY_EXISTS: value = EEXIST; break;
    case ERROR_DISK_FULL: case ERROR_HANDLE_DISK_FULL: value = ENOSPC; break;
    case ERROR_INVALID_PARAMETER: case ERROR_NEGATIVE_SEEK: value = EINVAL; break;
    case ERROR_NO_UNICODE_TRANSLATION: value = EILSEQ; break;
    case ERROR_INSUFFICIENT_BUFFER: value = ERANGE; break;
    case ERROR_NOT_SUPPORTED: case ERROR_CALL_NOT_IMPLEMENTED: value = ENOTSUP; break;
    case ERROR_DIR_NOT_EMPTY: value = ENOTEMPTY; break;
    case ERROR_BROKEN_PIPE: case ERROR_NO_DATA: value = EPIPE; break;
    case ERROR_INVALID_FUNCTION: value = EINVAL; break;
    }
    errno = value; _set_doserrno(error); return -1;
}
inline int CrtDescriptorFailure(int value) { errno = value; _set_doserrno(0); return -1; }

// The backend never returns a host HANDLE. It is also a test seam for exercising
// CRT translation and descriptor ownership without a UWP storage broker.
class CrtFileBackend
{
public:
    virtual ~CrtFileBackend() = default;
    virtual HANDLE Standard(unsigned) { return nullptr; }
    virtual bool SetStandard(unsigned, HANDLE, DWORD* error) { *error = ERROR_SUCCESS; return true; }
    virtual DWORD Type(HANDLE value, DWORD* error)
    { LONGLONG size; return Size(value, &size, error) ? FILE_TYPE_DISK : FILE_TYPE_UNKNOWN; }
    virtual DWORD DataAccess(HANDLE) { return GENERIC_READ | GENERIC_WRITE; }
    virtual bool SetInherit(HANDLE, bool, DWORD* error) { *error = ERROR_SUCCESS; return true; }
    virtual bool Duplicate(HANDLE, bool, HANDLE*, DWORD* error) { *error = ERROR_NOT_SUPPORTED; return false; }
    virtual bool ReadWide(HANDLE, wchar_t*, DWORD, DWORD*, DWORD* error) { *error = ERROR_NOT_SUPPORTED; return false; }
    virtual bool WriteWide(HANDLE, const wchar_t*, DWORD, DWORD*, DWORD* error) { *error = ERROR_NOT_SUPPORTED; return false; }
    // Nonzero for a console whose WriteWide accepts a complete batch atomically.
    virtual UINT ConsoleOutputCodePage() { return 0; }
    virtual UINT ConsoleLocaleCodePage() { return 0; }
    virtual bool Open(const wchar_t*, DWORD access, DWORD share, DWORD create, DWORD attributes, HANDLE*, DWORD*) = 0;
    virtual bool Close(HANDLE, DWORD*) = 0;
    virtual bool Read(HANDLE, void*, DWORD, DWORD*, DWORD*) = 0;
    virtual bool Write(HANDLE, const void*, DWORD, DWORD*, DWORD*) = 0;
    virtual bool Seek(HANDLE, LONGLONG, DWORD, LONGLONG*, DWORD*) = 0;
    virtual bool Size(HANDLE, LONGLONG*, DWORD*) = 0;
    virtual bool Resize(HANDLE, LONGLONG, DWORD*) = 0;
    virtual bool Flush(HANDLE, DWORD*) = 0;
};

class CrtFileDescriptors final
{
    struct OpenFile
    {
        std::shared_ptr<CrtFileBackend> backend;
        HANDLE handle = INVALID_HANDLE_VALUE;
        DWORD type = FILE_TYPE_DISK;
        std::recursive_mutex lock;
        ~OpenFile() { if (handle != INVALID_HANDLE_VALUE) { DWORD ignored; backend->Close(handle, &ignored); } }
    };
    struct PartialTextUnit
    {
        unsigned char input[2 * sizeof(wchar_t)]{};
        unsigned inputBytes = 0;
        unsigned outputBytes = 0;
    };
    struct ConsoleTextPrefix
    {
        BYTE bytes[4]{};
        unsigned size = 0;
        UINT codePage = 0;
    };
    struct Descriptor
    {
        std::shared_ptr<OpenFile> file;
        int flags = 0;
        bool textEof = false;
        std::deque<BYTE> pending;
        PartialTextUnit partialWrite;
        ConsoleTextPrefix consolePrefix;
        CrtTextEncoding encoding = CrtTextEncoding::Ansi;
        std::recursive_mutex lock;
    };
public:
    explicit CrtFileDescriptors(std::shared_ptr<CrtFileBackend> backend) : m_backend(std::move(backend)) { }
    ~CrtFileDescriptors() { CloseAll(); }
    bool InitializeStandards(const std::vector<CrtStartupFile>& inherited = {})
    {
        try
        {
            std::lock_guard<std::mutex> guard(m_lock);
            std::map<HANDLE, std::shared_ptr<OpenFile>> files;
            for (const auto& entry : m_entries)
                files.emplace(entry.second->file->handle, entry.second->file);
            const auto install = [&](int index, HANDLE handle, int flags, bool eof)
            {
                if (m_entries.count(index) || !handle || handle == INVALID_HANDLE_VALUE ||
                    reinterpret_cast<intptr_t>(handle) == -2) return;
                DWORD error = ERROR_SUCCESS; const DWORD type = m_backend->Type(handle, &error);
                if (type == FILE_TYPE_UNKNOWN && error) return;
                auto entry = std::make_shared<Descriptor>();
                entry->flags = flags;
                entry->textEof = eof;
                const auto previous = files.find(handle);
                if (previous != files.end()) entry->file = previous->second;
                if (!entry->file)
                {
                    entry->file = std::make_shared<OpenFile>();
                    entry->file->backend = m_backend;
                    entry->file->type = type;
                    files.emplace(handle, entry->file);
                }
                m_entries.emplace(index, entry);
                entry->file->handle = handle;
            };
            const auto count = (std::min)(inherited.size(), static_cast<size_t>(CrtStartupFile::Limit));
            for (size_t index = 0; index < count; ++index)
            {
                const auto& file = inherited[index];
                if (!file.IsInheritable()) continue;
                const DWORD access = m_backend->DataAccess(file.handle);
                int flags = access == GENERIC_READ ? _O_RDONLY : access == GENERIC_WRITE ? _O_WRONLY : _O_RDWR;
                flags |= (index < 3 || (file.flags & CrtStartupFile::Text)) ? _O_TEXT : _O_BINARY;
                if (file.flags & CrtStartupFile::Append) flags |= _O_APPEND;
                install(static_cast<int>(index), file.handle, flags, (file.flags & CrtStartupFile::Eof) != 0);
            }
            for (unsigned index = 0; index < 3; ++index)
                install(static_cast<int>(index), m_backend->Standard(index),
                    (index ? _O_WRONLY : _O_RDONLY) | _O_TEXT, false);
            return true;
        }
        catch (const std::bad_alloc&) { CrtDescriptorFailure(ENOMEM); return false; }
    }
    int IsTerminal(int descriptor)
    {
        const auto entry = Find(descriptor); if (!entry) return 0;
        return entry->file->type == FILE_TYPE_CHAR ? 1 : 0;
    }

    int Open(const wchar_t* path, int flags, int sharing, int permissions, int defaultMode)
    {
        if (!path || !*path || (flags & 3) == 3) return CrtDescriptorFailure(EINVAL);
        constexpr int unicodeFlags = _O_WTEXT | _O_U16TEXT | _O_U8TEXT;
        if (flags & _O_OBTAIN_DIR) return CrtDescriptorFailure(ENOTSUP);
        constexpr int translationFlags = _O_TEXT | _O_BINARY | unicodeFlags;
        if (!(flags & translationFlags)) flags |= defaultMode & translationFlags;
        if (!(flags & translationFlags)) flags |= _O_TEXT;
        const int unicodeMode = flags & unicodeFlags;
        if (unicodeMode && (unicodeMode & (unicodeMode - 1))) return CrtDescriptorFailure(EINVAL);
        DWORD access = (flags & 3) == _O_RDONLY ? GENERIC_READ : (flags & 3) == _O_WRONLY ? GENERIC_WRITE : GENERIC_READ | GENERIC_WRITE;
        DWORD share;
        switch (sharing)
        {
        case _SH_DENYRW: share = 0; break;
        case _SH_DENYWR: share = FILE_SHARE_READ; break;
        case _SH_DENYRD: share = FILE_SHARE_WRITE; break;
        case _SH_DENYNO: share = FILE_SHARE_READ | FILE_SHARE_WRITE; break;
        case _SH_SECURE: share = (flags & 3) == _O_RDONLY ? FILE_SHARE_READ : 0; break;
        default: return CrtDescriptorFailure(EINVAL);
        }
        DWORD create = OPEN_EXISTING;
        if (flags & _O_CREAT) create = flags & _O_EXCL ? CREATE_NEW : flags & _O_TRUNC ? CREATE_ALWAYS : OPEN_ALWAYS;
        else if (flags & _O_TRUNC) create = TRUNCATE_EXISTING;
        DWORD attributes = FILE_ATTRIBUTE_NORMAL;
        if ((flags & _O_CREAT) && !(permissions & 0200)) attributes = FILE_ATTRIBUTE_READONLY;
        if (flags & (_O_TEMPORARY | _O_SHORT_LIVED)) attributes |= FILE_ATTRIBUTE_TEMPORARY;
        if (flags & _O_TEMPORARY) { attributes |= FILE_FLAG_DELETE_ON_CLOSE; share |= FILE_SHARE_DELETE; access |= DELETE; }
        if (flags & _O_RANDOM) attributes |= FILE_FLAG_RANDOM_ACCESS;
        else if (flags & _O_SEQUENTIAL) attributes |= FILE_FLAG_SEQUENTIAL_SCAN;
        HANDLE handle = INVALID_HANDLE_VALUE; DWORD error = ERROR_SUCCESS;
        if ((flags & 3) == _O_WRONLY && (flags & _O_APPEND) && unicodeMode && !(flags & _O_BINARY)) access |= GENERIC_READ;
        if (!m_backend->Open(path, access, share, create, attributes, &handle, &error))
        {
            if ((flags & 3) != _O_WRONLY || !(access & GENERIC_READ)) return CrtIoFailure(error);
            access &= ~GENERIC_READ;
            if (!m_backend->Open(path, access, share, create, attributes, &handle, &error)) return CrtIoFailure(error);
        }
        if (!m_backend->SetInherit(handle, !(flags & _O_NOINHERIT), &error))
        { DWORD ignored; m_backend->Close(handle, &ignored); return CrtIoFailure(error); }
        CrtTextEncoding encoding = CrtTextEncoding::Ansi;
        if (ConfigureOpenedEncoding(handle, flags, access, create, &encoding) < 0)
        {
            const int savedError = errno; unsigned long savedDosError; _get_doserrno(&savedDosError);
            DWORD ignored; m_backend->Close(handle, &ignored);
            errno = savedError; _set_doserrno(savedDosError); return -1;
        }
        return Adopt(handle, flags, true, encoding);
    }

    int Adopt(HANDLE handle, int flags, bool closeOnFailure = false, CrtTextEncoding encoding = CrtTextEncoding::Ansi)
    {
        if (!handle || handle == INVALID_HANDLE_VALUE) return CrtDescriptorFailure(EBADF);

        DWORD validationError = ERROR_SUCCESS;
        const DWORD type = m_backend->Type(handle, &validationError);
        if (type == FILE_TYPE_UNKNOWN && validationError)
        {
            if (closeOnFailure) { DWORD ignored; m_backend->Close(handle, &ignored); }
            return CrtIoFailure(validationError);
        }
        if (!(flags & (_O_BINARY | _O_TEXT | _O_WTEXT | _O_U16TEXT | _O_U8TEXT))) flags |= _O_BINARY;
        try
        {
            auto entry = std::make_shared<Descriptor>();
            auto file = std::make_shared<OpenFile>(); file->backend = m_backend; file->type = type;
            entry->file = file; entry->flags = flags; entry->encoding = encoding;
            if (encoding == CrtTextEncoding::Ansi && !(flags & _O_BINARY))
                entry->encoding = flags & _O_U8TEXT ? CrtTextEncoding::Utf8 :
                    flags & (_O_U16TEXT | _O_WTEXT) ? CrtTextEncoding::Utf16LittleEndian : CrtTextEncoding::Ansi;
            std::lock_guard<std::mutex> guard(m_lock);
            const int descriptor = FreeDescriptorLocked();
            if (descriptor < 0)
            {
                if (closeOnFailure) { DWORD ignored; m_backend->Close(handle, &ignored); }
                return CrtDescriptorFailure(EMFILE);
            }
            m_entries.emplace(descriptor, entry); file->handle = handle;
            return descriptor;
        }
        catch (const std::bad_alloc&)
        {
            if (closeOnFailure) { DWORD ignored; m_backend->Close(handle, &ignored); }
            return CrtDescriptorFailure(ENOMEM);
        }
    }

    int Close(int descriptor)
    {
        std::shared_ptr<Descriptor> entry;
        {
            std::lock_guard<std::mutex> guard(m_lock);
            const auto found = m_entries.find(descriptor);
            if (found == m_entries.end()) return CrtDescriptorFailure(EBADF);
            entry = std::move(found->second); m_entries.erase(found);
        }
        std::lock_guard<std::recursive_mutex> descriptorGuard(entry->lock);
        std::lock_guard<std::recursive_mutex> fileGuard(entry->file->lock);
        if (descriptor >= 0 && descriptor < 3 && m_backend->Standard(static_cast<unsigned>(descriptor)) == entry->file->handle)
        { DWORD ignored = ERROR_SUCCESS; m_backend->SetStandard(static_cast<unsigned>(descriptor), nullptr, &ignored); }
        if (entry->file.use_count() == 1)
        {
            DWORD error = ERROR_SUCCESS;
            const bool closed = entry->file->backend->Close(entry->file->handle, &error);
            entry->file->handle = INVALID_HANDLE_VALUE;
            if (!closed) return CrtIoFailure(error);
        }
        return 0;
    }
    void CloseAll()
    {
        std::map<int, std::shared_ptr<Descriptor>> entries;
        { std::lock_guard<std::mutex> guard(m_lock); entries.swap(m_entries); }
        entries.clear();
    }
    intptr_t Handle(int descriptor)
    {
        auto entry = Find(descriptor); if (!entry) return -1;
        std::lock_guard<std::recursive_mutex> guard(entry->file->lock);
        return reinterpret_cast<intptr_t>(entry->file->handle);
    }
    bool BuildSpawnData(bool includeStandards, std::vector<BYTE>* bytes)
    {
        try
        {
            // I/O can pump guest messages, so release the table lock before
            // acquiring an individual descriptor or file lock.
            std::map<int, std::shared_ptr<Descriptor>> entries;
            {
                std::lock_guard<std::mutex> guard(m_lock);
                entries = m_entries;
            }
            std::vector<CrtStartupFile> files;
            for (const auto& item : entries)
            {
                if (!includeStandards && item.first < 3) continue;
                const auto& entry = item.second;
                std::lock_guard<std::recursive_mutex> guard(entry->lock);
                std::lock_guard<std::recursive_mutex> fileGuard(entry->file->lock);
                if (entry->flags & _O_NOINHERIT) continue;

                BYTE flags = CrtStartupFile::Open;
                if (!(entry->flags & _O_BINARY)) flags |= CrtStartupFile::Text;
                if (entry->flags & _O_APPEND) flags |= CrtStartupFile::Append;
                if (entry->textEof) flags |= CrtStartupFile::Eof;
                if (entry->file->type == FILE_TYPE_PIPE) flags |= CrtStartupFile::Pipe;
                if (entry->file->type == FILE_TYPE_CHAR) flags |= CrtStartupFile::Device;
                const CrtStartupFile file{ flags, entry->file->handle };
                if (!file.IsInheritable()) continue;

                const auto index = static_cast<size_t>(item.first);
                if (index >= CrtStartupFile::PacketLimit)
                { CrtDescriptorFailure(ENOMEM); return false; }
                if (index >= files.size()) files.resize(index + 1);
                files[index] = file;
            }
            *bytes = EncodeCrtStartupFiles(files);
            return true;
        }
        catch (const std::bad_alloc&) { CrtDescriptorFailure(ENOMEM); return false; }
    }
    int Duplicate(int descriptor, int target = -1)
    {
        if (target < -1 || target >= MaximumDescriptors) return CrtDescriptorFailure(EBADF);
        auto source = Find(descriptor); if (!source) return -1;
        if (descriptor == target) return 0;
        try
        {
            auto copy = std::make_shared<Descriptor>();
            {
                std::lock_guard<std::recursive_mutex> guard(source->lock);
                std::lock_guard<std::recursive_mutex> fileGuard(source->file->lock);
                HANDLE duplicate = INVALID_HANDLE_VALUE; DWORD error = ERROR_SUCCESS;
                auto file = std::make_shared<OpenFile>(); file->backend = m_backend; file->type = source->file->type;
                if (m_backend->Duplicate(source->file->handle, true, &duplicate, &error))
                { file->handle = duplicate; copy->file = file; }
                else if (error == ERROR_NOT_SUPPORTED) copy->file = source->file; // Portable test backends may share descriptions directly.
                else return CrtIoFailure(error);
                copy->flags = source->flags & ~_O_NOINHERIT; copy->textEof = source->textEof;
                // Lookahead is private to the source descriptor, not the shared
                // OS description. Duplicating it would deliver bytes twice.
                copy->encoding = source->encoding;
            }
            std::shared_ptr<Descriptor> previous;
            int installed;
            {
                std::lock_guard<std::mutex> guard(m_lock);
                installed = target == -1 ? FreeDescriptorLocked() : target;
                if (installed < 0) return CrtDescriptorFailure(EMFILE);
                auto found = m_entries.find(installed);
                if (found == m_entries.end()) m_entries.emplace(installed, copy);
                else { previous = std::move(found->second); found->second = copy; }
            }
            // Dispose the replaced fd before publishing its new std handle;
            // otherwise an old descriptor's cleanup could clear the new slot.
            previous.reset();
            if (installed < 3)
            {
                DWORD error = ERROR_SUCCESS;
                if (!m_backend->SetStandard(static_cast<unsigned>(installed), copy->file->handle, &error)) return CrtIoFailure(error);
            }
            return target == -1 ? installed : 0;
        }
        catch (const std::bad_alloc&) { return CrtDescriptorFailure(ENOMEM); }
    }
    int Read(int descriptor, void* buffer, unsigned count)
    {
        return ReadInternal(descriptor, buffer, count, false);
    }
    // Stdio reads one UTF-8 scalar but supplies space for the possible UTF-16 pair.
    int ReadWideCharacter(int descriptor, void* buffer)
    {
        return ReadInternal(descriptor, buffer, 2 * sizeof(wchar_t), true);
    }
    UINT ConsoleAnsiInputPage(int descriptor, UINT explicitPage = 0)
    {
        const auto entry = Find(descriptor);
        if (!entry) return 0;
        std::lock_guard<std::recursive_mutex> guard(entry->lock);
        if (entry->file->type != FILE_TYPE_CHAR || (entry->flags & _O_BINARY) ||
            entry->encoding != CrtTextEncoding::Ansi) return 0;
        return SelectConsoleInputPage(m_backend->ConsoleOutputCodePage(), explicitPage);
    }

    int Write(int descriptor, const void* buffer, unsigned count, UINT consoleSourcePage = 0)
    {
        auto entry = Find(descriptor); if (!entry) return -1;
        if (count > INT_MAX || (!buffer && count)) return CrtDescriptorFailure(EINVAL);
        std::lock_guard<std::recursive_mutex> descriptorGuard(entry->lock);
        std::lock_guard<std::recursive_mutex> fileGuard(entry->file->lock);
        if ((entry->flags & 3) == _O_RDONLY) return CrtDescriptorFailure(EBADF);
        if (!count) return 0;
        if (!(entry->flags & _O_BINARY) && entry->encoding != CrtTextEncoding::Ansi && count % sizeof(wchar_t)) return CrtDescriptorFailure(EINVAL);
        // The caller still owns an incomplete logical unit. It must supply
        // that same unit before we can append more data at the current position.
        if (entry->partialWrite.inputBytes && (count < entry->partialWrite.inputBytes ||
            std::memcmp(buffer, entry->partialWrite.input, entry->partialWrite.inputBytes) != 0))
            return CrtDescriptorFailure(EINVAL);
        DWORD error = ERROR_SUCCESS;
        if ((entry->flags & _O_APPEND) && entry->file->type == FILE_TYPE_DISK)
        {
            LONGLONG ignored;
            if (!m_backend->Seek(entry->file->handle, 0, FILE_END, &ignored, &error)) return CrtIoFailure(error);
        }
        if (entry->flags & _O_BINARY)
        {
            DWORD written = 0;
            const bool complete = m_backend->Write(entry->file->handle, buffer, count, &written, &error);
            if (!complete || error) CrtIoFailure(error ? error : ERROR_WRITE_FAULT);
            if (written) return static_cast<int>(written);
            return complete && !error ? CrtIoFailure(ERROR_DISK_FULL) : -1;
        }
        if (entry->encoding != CrtTextEncoding::Ansi) return WriteUnicode(*entry, buffer, count);
        const auto source = static_cast<const unsigned char*>(buffer);
        if (entry->file->type == FILE_TYPE_CHAR)
        {
            const UINT output = m_backend->ConsoleOutputCodePage();
            const UINT page = SelectConsoleInputPage(output, consoleSourcePage);
            auto& prefix = entry->consolePrefix;
            if (output && prefix.size && prefix.codePage != page)
            {
                if (!EmitAnsiConsoleText(*entry, prefix.codePage, output, prefix.bytes, prefix.size)) return -1;
                prefix = {};
            }
            CPINFO info{};
            if (output && page && (page == CP_UTF8 || ::GetCPInfo(page, &info)))
                return WriteAnsiConsole(*entry, source, count, page, output, info);
        }
        unsigned consumed = 0;
        while (consumed < count)
        {
            unsigned char translated[4096];
            unsigned physical = 0, logical = 0;
            while (consumed + logical < count && physical + 2 <= sizeof(translated))
            {
                const auto character = source[consumed + logical++];
                if (character == '\n') translated[physical++] = '\r';
                translated[physical++] = character;
            }
            unsigned delivered = entry->partialWrite.outputBytes;
            while (delivered < physical)
            {
                DWORD written = 0;
                error = ERROR_SUCCESS;
                const bool success = m_backend->Write(entry->file->handle, translated + delivered, physical - delivered, &written, &error);
                delivered += written;
                if (!success || error || !written)
                {
                    CrtIoFailure(error ? error : success ? ERROR_DISK_FULL : ERROR_WRITE_FAULT);
                    unsigned complete = 0, used = 0;
                    while (complete < logical)
                    {
                        const unsigned width = source[consumed + complete] == '\n' ? 2u : 1u;
                        if (used + width > delivered) break;
                        used += width; ++complete;
                    }
                    entry->partialWrite = {};
                    if (complete < logical && used < delivered)
                        SavePartialTextUnit(*entry, source + consumed + complete, 1, delivered - used);
                    return consumed + complete ? static_cast<int>(consumed + complete) : -1;
                }
            }
            entry->partialWrite = {};
            consumed += logical;
        }
        return static_cast<int>(consumed);
    }
    LONGLONG Seek(int descriptor, LONGLONG distance, int origin)
    {
        auto entry = Find(descriptor); if (!entry) return -1;
        if (origin < 0 || origin > 2) return CrtDescriptorFailure(EINVAL);
        std::lock_guard<std::recursive_mutex> descriptorGuard(entry->lock);
        std::lock_guard<std::recursive_mutex> fileGuard(entry->file->lock);
        DWORD error = ERROR_SUCCESS; LONGLONG result;
        if (!m_backend->Seek(entry->file->handle, distance, static_cast<DWORD>(origin), &result, &error)) return CrtIoFailure(error);
        entry->textEof = false; entry->pending.clear(); entry->partialWrite = {}; return result;
    }
    LONGLONG Size(int descriptor)
    {
        auto entry = Find(descriptor); if (!entry) return -1;
        std::lock_guard<std::recursive_mutex> guard(entry->file->lock);
        LONGLONG result; DWORD error = ERROR_SUCCESS;
        return m_backend->Size(entry->file->handle, &result, &error) ? result : CrtIoFailure(error);
    }
    int Eof(int descriptor)
    {
        auto entry = Find(descriptor); if (!entry) return -1;
        std::lock_guard<std::recursive_mutex> guard(entry->file->lock);
        LONGLONG position, size; DWORD error = ERROR_SUCCESS;
        if (!m_backend->Seek(entry->file->handle, 0, FILE_CURRENT, &position, &error) ||
            !m_backend->Size(entry->file->handle, &size, &error)) return CrtIoFailure(error);
        return position == size ? 1 : 0;
    }
    int Resize(int descriptor, LONGLONG size)
    {
        if (size < 0) return CrtDescriptorFailure(EINVAL);
        auto entry = Find(descriptor); if (!entry) return -1;
        std::lock_guard<std::recursive_mutex> descriptorGuard(entry->lock);
        std::lock_guard<std::recursive_mutex> fileGuard(entry->file->lock);
        if ((entry->flags & 3) == _O_RDONLY) return CrtDescriptorFailure(EACCES);
        DWORD error = ERROR_SUCCESS;
        if (!m_backend->Resize(entry->file->handle, size, &error)) return CrtIoFailure(error);
        entry->partialWrite = {};
        return 0;
    }
    int Commit(int descriptor)
    {
        auto entry = Find(descriptor); if (!entry) return -1;
        std::lock_guard<std::recursive_mutex> guard(entry->file->lock);
        DWORD error = ERROR_SUCCESS;
        return m_backend->Flush(entry->file->handle, &error) ? 0 : CrtIoFailure(error);
    }
    int Mode(int descriptor)
    {
        auto entry = Find(descriptor); if (!entry) return -1;
        std::lock_guard<std::recursive_mutex> guard(entry->lock);
        if (entry->flags & _O_BINARY) return _O_BINARY;
        return entry->encoding == CrtTextEncoding::Utf8 ? _O_U8TEXT :
            entry->encoding == CrtTextEncoding::Utf16LittleEndian ? _O_WTEXT : _O_TEXT;
    }
    LONGLONG Position(int descriptor)
    {
        auto entry = Find(descriptor); if (!entry) return -1;
        std::lock_guard<std::recursive_mutex> guard(entry->file->lock);
        LONGLONG result; DWORD error = ERROR_SUCCESS;
        return m_backend->Seek(entry->file->handle, 0, FILE_CURRENT, &result, &error) ? result : CrtIoFailure(error);
    }
    int ClearTextEof(int descriptor)
    {
        auto entry = Find(descriptor); if (!entry) return -1;
        std::lock_guard<std::recursive_mutex> guard(entry->lock);
        entry->textEof = false; return 0;
    }
    int SetMode(int descriptor, int mode)
    {
        if (mode != _O_TEXT && mode != _O_BINARY && mode != _O_WTEXT && mode != _O_U16TEXT && mode != _O_U8TEXT) return CrtDescriptorFailure(EINVAL);
        auto entry = Find(descriptor); if (!entry) return -1;
        std::lock_guard<std::recursive_mutex> guard(entry->lock);
        const int previous = entry->flags & _O_BINARY ? _O_BINARY : entry->encoding == CrtTextEncoding::Utf8 ? _O_U8TEXT :
            entry->encoding == CrtTextEncoding::Utf16LittleEndian ? _O_WTEXT : _O_TEXT;
        const int normalizedMode = mode == _O_U16TEXT ? _O_WTEXT : mode;
        if (normalizedMode != previous)
        {
            entry->partialWrite = {};
            entry->consolePrefix = {};
        }
        entry->flags = (entry->flags & ~(_O_TEXT | _O_BINARY | _O_WTEXT | _O_U16TEXT | _O_U8TEXT)) | mode;
        if (mode != _O_BINARY) entry->encoding = mode == _O_U8TEXT ? CrtTextEncoding::Utf8 :
            mode == _O_TEXT ? CrtTextEncoding::Ansi : CrtTextEncoding::Utf16LittleEndian;
        return previous;
    }
private:
    static constexpr unsigned ConsoleTextBatch = 1024;

    UINT SelectConsoleInputPage(UINT output, UINT requested) const
    {
        if (!output) return 0;
        const UINT locale = requested ? requested : m_backend->ConsoleLocaleCodePage();
        return locale ? locale : output;
    }

    bool EmitAnsiConsoleText(Descriptor& entry, UINT page, UINT outputPage, const BYTE* bytes, unsigned count)
    {
        if (!count) return true;
        wchar_t decoded[ConsoleTextBatch];
        int units = ::MultiByteToWideChar(page, 0, reinterpret_cast<const char*>(bytes),
            static_cast<int>(count), decoded, ConsoleTextBatch);
        if (!units)
        {
            CrtIoFailure(::GetLastError());
            return false;
        }
        if (page != outputPage)
        {
            // The virtual console stores UTF-16. Round-trip through the console
            // page so it exhibits the same substitutions as a native console.
            char consoleBytes[ConsoleTextBatch * 5];
            const int encoded = ::WideCharToMultiByte(outputPage, 0, decoded, units, consoleBytes,
                static_cast<int>(sizeof(consoleBytes)), nullptr, nullptr);
            if (!encoded || !(units = ::MultiByteToWideChar(outputPage, 0, consoleBytes, encoded, decoded, ConsoleTextBatch)))
            {
                CrtIoFailure(::GetLastError());
                return false;
            }
        }
        wchar_t translated[ConsoleTextBatch * 2];
        DWORD length = 0;
        for (int i = 0; i < units; ++i)
        {
            if (decoded[i] == L'\n') translated[length++] = L'\r';
            translated[length++] = decoded[i];
        }
        DWORD written = 0, error = ERROR_SUCCESS;
        if (m_backend->WriteWide(entry.file->handle, translated, length, &written, &error) && written == length)
            return true;
        CrtIoFailure(error ? error : ERROR_WRITE_FAULT);
        return false;
    }

    int WriteAnsiConsole(Descriptor& entry, const BYTE* source, unsigned count, UINT page, UINT output, const CPINFO& info)
    {
        auto& prefix = entry.consolePrefix;
        unsigned consumed = 0;
        while (consumed < count)
        {
            BYTE batch[ConsoleTextBatch];
            const unsigned take = (std::min)(count - consumed, ConsoleTextBatch - prefix.size);
            std::memcpy(batch, prefix.bytes, prefix.size);
            std::memcpy(batch + prefix.size, source + consumed, take);
            const unsigned total = prefix.size + take;
            unsigned complete = total;
            if (page == CP_UTF8) complete = static_cast<unsigned>(CompleteCrtUtf8Prefix(batch, total));
            else if (info.MaxCharSize == 2) complete = static_cast<unsigned>(CompleteCrtDbcsPrefix(batch, total, info));
            if (!EmitAnsiConsoleText(entry, page, output, batch, complete))
                return consumed ? static_cast<int>(consumed) : -1;

            // Credit incomplete bytes to this write. Only the descriptor that
            // accepted them can complete that character on a subsequent write.
            prefix.size = total - complete;
            prefix.codePage = page;
            std::memcpy(prefix.bytes, batch + complete, prefix.size);
            consumed += take;
        }
        return static_cast<int>(consumed);
    }

    int ReadInternal(int descriptor, void* buffer, unsigned count, bool singleWide)
    {
        auto entry = Find(descriptor); if (!entry) return -1;
        if (count > INT_MAX || (!buffer && count)) return CrtDescriptorFailure(EINVAL);
        std::lock_guard<std::recursive_mutex> descriptorGuard(entry->lock);
        std::lock_guard<std::recursive_mutex> fileGuard(entry->file->lock);
        if ((entry->flags & 3) == _O_WRONLY) return CrtDescriptorFailure(EBADF);
        if (!count || entry->textEof) return 0;
        entry->partialWrite = {};
        if (!(entry->flags & _O_BINARY) && entry->encoding != CrtTextEncoding::Ansi) return ReadUnicode(*entry, buffer, count, singleWide);
        if (singleWide) count = sizeof(wchar_t);
        DWORD read = 0, error = ERROR_SUCCESS;
        if (!ReadPhysical(*entry, buffer, count, &read, &error)) return CrtIoFailure(error);
        if (entry->flags & _O_BINARY) return static_cast<int>(read);
        return TranslateRead(*entry, static_cast<unsigned char*>(buffer), read);
    }
    bool ReadPhysical(Descriptor& entry, void* buffer, DWORD count, DWORD* done, DWORD* error, bool wideConsole = false)
    {
        *done = 0; auto bytes = static_cast<BYTE*>(buffer);
        while (*done < count && !entry.pending.empty())
        { bytes[(*done)++] = entry.pending.front(); entry.pending.pop_front(); }
        if (*done == count || (*done && entry.file->type != FILE_TYPE_DISK)) { *error = ERROR_SUCCESS; return true; }
        DWORD received = 0;
        const bool result = wideConsole && entry.file->type == FILE_TYPE_CHAR
            ? m_backend->ReadWide(entry.file->handle, reinterpret_cast<wchar_t*>(bytes + *done), (count - *done) / sizeof(wchar_t), &received, error)
            : m_backend->Read(entry.file->handle, bytes + *done, count - *done, &received, error);
        if (wideConsole && entry.file->type == FILE_TYPE_CHAR) received *= sizeof(wchar_t);
        *done += received;
        return result || *done != 0;
    }
    bool ReturnPhysical(Descriptor& entry, const void* bytes, size_t count, DWORD* error)
    {
        if (entry.file->type == FILE_TYPE_DISK)
        { LONGLONG ignored; return m_backend->Seek(entry.file->handle, -static_cast<LONGLONG>(count), FILE_CURRENT, &ignored, error); }
        try
        {
            const auto source = static_cast<const BYTE*>(bytes);
            auto pending = entry.pending;
            pending.insert(pending.begin(), source, source + count);
            entry.pending.swap(pending);
            *error = ERROR_SUCCESS; return true;
        }
        catch (const std::bad_alloc&) { *error = ERROR_NOT_ENOUGH_MEMORY; return false; }
    }
    int ConfigureOpenedEncoding(HANDLE handle, int flags, DWORD access, DWORD, CrtTextEncoding* encoding)
    {
        if (flags & _O_BINARY) return 0;
        if (flags & _O_U8TEXT) *encoding = CrtTextEncoding::Utf8;
        else if (flags & _O_U16TEXT) *encoding = CrtTextEncoding::Utf16LittleEndian;
        else if ((flags & (_O_WTEXT | _O_WRONLY | _O_CREAT | _O_TRUNC)) == (_O_WTEXT | _O_WRONLY | _O_CREAT | _O_TRUNC))
            *encoding = CrtTextEncoding::Utf16LittleEndian;
        if (!(flags & (_O_WTEXT | _O_U8TEXT | _O_U16TEXT))) return 0;
        DWORD error = ERROR_SUCCESS; LONGLONG size, ignored;
        if (m_backend->Type(handle, &error) != FILE_TYPE_DISK) return 0;
        if (!m_backend->Size(handle, &size, &error)) return CrtIoFailure(error);
        const bool writeBom = (access & GENERIC_WRITE) && size == 0;
        if (writeBom)
        {
            const unsigned char utf8Bom[] = { 0xef, 0xbb, 0xbf };
            const unsigned char utf16Bom[] = { 0xff, 0xfe };
            const auto bytes = *encoding == CrtTextEncoding::Utf8 ? utf8Bom : utf16Bom;
            const DWORD length = *encoding == CrtTextEncoding::Ansi ? 0 : *encoding == CrtTextEncoding::Utf8 ? 3 : 2;
            for (DWORD position = 0; position < length; )
            {
                DWORD written = 0;
                if (!m_backend->Write(handle, bytes + position, length - position, &written, &error)) return CrtIoFailure(error);
                if (!written) return CrtIoFailure(ERROR_DISK_FULL);
                position += written;
            }
        }
        else if (access & GENERIC_READ)
        {
            unsigned char bytes[3]{}; DWORD read = 0;
            while (read < sizeof(bytes))
            {
                DWORD transferred = 0;
                if (!m_backend->Read(handle, bytes + read, static_cast<DWORD>(sizeof(bytes)) - read, &transferred, &error)) return CrtIoFailure(error);
                read += transferred; if (!transferred) break;
            }
            LONGLONG position = 0;
            if (read >= 2 && bytes[0] == 0xfe && bytes[1] == 0xff) return CrtDescriptorFailure(EINVAL);
            if (read >= 2 && bytes[0] == 0xff && bytes[1] == 0xfe)
            { *encoding = CrtTextEncoding::Utf16LittleEndian; position = 2; }
            else if (read == 3 && bytes[0] == 0xef && bytes[1] == 0xbb && bytes[2] == 0xbf)
            { *encoding = CrtTextEncoding::Utf8; position = 3; }
            if (!m_backend->Seek(handle, position, FILE_BEGIN, &ignored, &error)) return CrtIoFailure(error);
        }
        return 0;
    }

    template<class Character> int TranslateRead(Descriptor& entry, Character* bytes, DWORD count)
    {
        size_t output = 0;
        for (DWORD position = 0; position < count; ++position)
        {
            const auto character = bytes[position];
            if (character == 0x1a)
            {
                if (entry.file->type == FILE_TYPE_CHAR) bytes[output++] = character;
                else entry.textEof = true;
                break;
            }
            if (character == '\r')
            {
                if (position + 1 < count)
                {
                    if (bytes[position + 1] == '\n') { ++position; bytes[output++] = '\n'; continue; }
                }
                else
                {
                    Character next{}; DWORD looked = 0, error = ERROR_SUCCESS;
                    const bool peeked = ReadPhysical(entry, &next, sizeof(next), &looked, &error, sizeof(Character) == sizeof(wchar_t));
                    while (peeked && looked && looked < sizeof(next))
                    {
                        DWORD extra = 0;
                        const bool more = ReadPhysical(entry, reinterpret_cast<BYTE*>(&next) + looked,
                            static_cast<DWORD>(sizeof(next)) - looked, &extra, &error);
                        looked += extra;
                        if (!more || !extra) break;
                    }
                    if (looked == sizeof(next) && next == '\n') { bytes[output++] = '\n'; continue; }
                    if (looked && !ReturnPhysical(entry, &next, looked, &error)) return CrtIoFailure(error);
                }
            }
            bytes[output++] = character;
        }
        return static_cast<int>(output * sizeof(Character));
    }

    int ReadUnicode(Descriptor& entry, void* buffer, unsigned count, bool singleWide)
    {
        if (count % sizeof(wchar_t)) return CrtDescriptorFailure(EINVAL);
        DWORD error = ERROR_SUCCESS, read = 0;
        if (entry.encoding == CrtTextEncoding::Utf16LittleEndian || entry.file->type == FILE_TYPE_CHAR)
        {
            if (!ReadPhysical(entry, buffer, singleWide ? sizeof(wchar_t) : count, &read, &error, true)) return CrtIoFailure(error);
            if (read % sizeof(wchar_t))
            {
                DWORD extra = 0;
                if (!ReadPhysical(entry, static_cast<BYTE*>(buffer) + read, 1, &extra, &error)) return CrtIoFailure(error);
                if (!extra) return CrtDescriptorFailure(EILSEQ);
                read += extra;
            }
            return TranslateRead(entry, static_cast<wchar_t*>(buffer), static_cast<DWORD>(read / sizeof(wchar_t)));
        }
        try
        {
            const DWORD request = singleWide ? 1 : count / static_cast<unsigned>(sizeof(wchar_t));
            std::vector<unsigned char> raw(static_cast<size_t>(request) + 3);
            if (!ReadPhysical(entry, raw.data(), request, &read, &error)) return CrtIoFailure(error);
            if (!read) return 0;
            size_t complete = CompleteCrtUtf8Prefix(raw.data(), read);
            // Small requests must still be able to read one full multibyte
            // sequence. Otherwise a non-ASCII character could stall forever.
            while (!complete)
            {
                DWORD extra = 0;
                if (!ReadPhysical(entry, raw.data() + read, 1, &extra, &error)) return CrtIoFailure(error);
                if (!extra) return CrtDescriptorFailure(EILSEQ);
                read += extra; complete = CompleteCrtUtf8Prefix(raw.data(), read);
            }
            if (complete < read)
            {
                if (!ReturnPhysical(entry, raw.data() + complete, read - complete, &error)) return CrtIoFailure(error);
            }
            const int translated = TranslateRead(entry, raw.data(), static_cast<DWORD>(complete));
            if (translated <= 0) return translated;
            const int converted = ::MultiByteToWideChar(CP_UTF8, 0, reinterpret_cast<const char*>(raw.data()), translated,
                static_cast<wchar_t*>(buffer), static_cast<int>(count / sizeof(wchar_t)));
            if (!converted) return CrtIoFailure(::GetLastError());
            return static_cast<int>(converted * sizeof(wchar_t));
        }
        catch (const std::bad_alloc&) { return CrtDescriptorFailure(ENOMEM); }
    }

    static void SavePartialTextUnit(Descriptor& entry, const void* source, unsigned inputBytes, unsigned emitted)
    {
        std::memcpy(entry.partialWrite.input, source, inputBytes);
        entry.partialWrite.inputBytes = inputBytes;
        entry.partialWrite.outputBytes = emitted;
    }

    // Count only logical units fully represented in the bytes already sent.
    // Retain the physical progress within an interrupted unit for a retry.
    // utf8 points to the full translated block, or is null for UTF-16 output.
    static unsigned RecordWideWriteProgress(Descriptor& entry, const wchar_t* source, unsigned units, unsigned delivered, const char* utf8)
    {
        entry.partialWrite = {};
        unsigned logical = 0, physical = 0;
        while (logical < units && physical < delivered)
        {
            const wchar_t value = source[logical];
            unsigned inputUnits = 1;
            unsigned outputBytes = (value == L'\n' ? 2u : 1u) * sizeof(wchar_t);
            if (utf8)
            {
                outputBytes = value == L'\n' ? 2u : CrtUtf8SequenceSize(static_cast<unsigned char>(utf8[physical]));
                if (value >= 0xd800 && value <= 0xdbff && logical + 1 < units &&
                    source[logical + 1] >= 0xdc00 && source[logical + 1] <= 0xdfff) inputUnits = 2;
            }
            if (outputBytes > delivered - physical)
            {
                SavePartialTextUnit(entry, source + logical, inputUnits * sizeof(wchar_t), delivered - physical);
                break;
            }
            physical += outputBytes;
            logical += inputUnits;
        }
        return logical;
    }

    int WriteUnicode(Descriptor& entry, const void* buffer, unsigned count)
    {
        if (count % sizeof(wchar_t)) return CrtDescriptorFailure(EINVAL);
        constexpr unsigned TranslationUnits = 1024;
        const auto source = static_cast<const wchar_t*>(buffer);
        const unsigned units = count / static_cast<unsigned>(sizeof(wchar_t));
        unsigned consumed = 0;
        while (consumed < units)
        {
            wchar_t translated[TranslationUnits]; unsigned used = 0, logical = 0;
            while (consumed + logical < units)
            {
                const wchar_t character = source[consumed + logical];
                if (character >= 0xd800 && character <= 0xdbff && consumed + logical + 1 < units &&
                    source[consumed + logical + 1] >= 0xdc00 && source[consumed + logical + 1] <= 0xdfff)
                {
                    if (used + 2 > TranslationUnits) break;
                    translated[used++] = character; translated[used++] = source[consumed + logical + 1]; logical += 2;
                }
                else
                {
                    if (used + (character == '\n' ? 2u : 1u) > TranslationUnits) break;
                    if (character == '\n') translated[used++] = '\r';
                    translated[used++] = character; ++logical;
                }
            }
            if (entry.file->type == FILE_TYPE_CHAR)
            {
                DWORD written = 0, error = ERROR_SUCCESS;
                const unsigned skip = entry.partialWrite.outputBytes / sizeof(wchar_t);
                const unsigned remaining = used - skip;
                if (!m_backend->WriteWide(entry.file->handle, translated + skip, remaining, &written, &error) || written != remaining)
                {
                    CrtIoFailure(error ? error : ERROR_WRITE_FAULT);
                    consumed += RecordWideWriteProgress(entry, source + consumed, logical, (skip + written) * sizeof(wchar_t), nullptr);
                    return consumed ? static_cast<int>(consumed * sizeof(wchar_t)) : -1;
                }
                entry.partialWrite = {};
                consumed += logical;
                continue;
            }
            char utf8[TranslationUnits * 3];
            const char* bytes = reinterpret_cast<const char*>(translated);
            unsigned physical = used * static_cast<unsigned>(sizeof(wchar_t));
            if (entry.encoding == CrtTextEncoding::Utf8)
            {
                const int encoded = ::WideCharToMultiByte(CP_UTF8, 0, translated, static_cast<int>(used), utf8, static_cast<int>(sizeof(utf8)), nullptr, nullptr);
                if (!encoded)
                {
                    CrtIoFailure(::GetLastError());
                    return consumed ? static_cast<int>(consumed * sizeof(wchar_t)) : -1;
                }
                bytes = utf8; physical = static_cast<unsigned>(encoded);
            }
            unsigned delivered = entry.partialWrite.outputBytes;
            while (delivered < physical)
            {
                DWORD written = 0, error = ERROR_SUCCESS;
                const bool success = m_backend->Write(entry.file->handle, bytes + delivered, physical - delivered, &written, &error);
                delivered += written;
                if (!success || error || !written)
                {
                    CrtIoFailure(error ? error : success ? ERROR_DISK_FULL : ERROR_WRITE_FAULT);
                    consumed += RecordWideWriteProgress(entry, source + consumed, logical, delivered,
                        entry.encoding == CrtTextEncoding::Utf8 ? utf8 : nullptr);
                    return consumed ? static_cast<int>(consumed * sizeof(wchar_t)) : -1;
                }
            }
            entry.partialWrite = {};
            consumed += logical;
        }
        return static_cast<int>(count);
    }
    std::shared_ptr<Descriptor> Find(int descriptor)
    {
        std::lock_guard<std::mutex> guard(m_lock);
        auto found = m_entries.find(descriptor);
        if (found == m_entries.end()) { CrtDescriptorFailure(EBADF); return {}; }
        return found->second;
    }
    int FreeDescriptorLocked() const
    {
        // Preserve the standard slots even when no startup handle exists;
        // applications can populate them later with _dup2.
        for (int descriptor = 3; descriptor < MaximumDescriptors; ++descriptor)
            if (m_entries.find(descriptor) == m_entries.end()) return descriptor;
        return -1;
    }
    static constexpr int MaximumDescriptors = CrtStartupFile::Limit;
    std::shared_ptr<CrtFileBackend> m_backend;
    std::mutex m_lock;
    std::map<int, std::shared_ptr<Descriptor>> m_entries;
};

} }
