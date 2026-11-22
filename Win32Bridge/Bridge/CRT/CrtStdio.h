#pragma once
#include "Bridge/CRT/CrtStdioMode.h"
#include "Bridge/CRT/CrtExceptions.h"
#include <ctype.h>
#include <array>
#include <atomic>
#include <cstring>
#include <limits>
#include <cwchar>

namespace Win32Bridge { namespace Bridge {

class CrtStdio final
{
    enum { ReadFlag = 1, WriteFlag = 2, NoBufferFlag = 4, EofFlag = 0x10, ErrorFlag = 0x20, UpdateFlag = 0x80 };
    struct Stream
    {
        CrtLegacyFile ownHeader;
        CrtLegacyFile* header = &ownHeader;
        CrtStdioMode mode;
        std::atomic<bool> closed{ true };
        bool standard = false;
        int buffering = 0; // _IOFBF
        size_t capacity = 4096, pending = 0;
        UINT pendingConsolePage = 0;
        char* userBuffer = nullptr;
        std::vector<unsigned char> output, pushedBytes;
        std::vector<wchar_t> pushedWide;
        int pendingWide = -1;
        std::recursive_mutex lock;
    };
public:
    explicit CrtStdio(CrtFileDescriptors& files) : m_files(files)
    {
        for (size_t index = 0; index < m_iob.size(); ++index)
        {
            auto stream = std::make_shared<Stream>(); stream->header = &m_iob[index];
            if (index < 3)
            {
                stream->standard = true;
                CrtStdioMode mode; mode.flags = index == 0 ? _O_RDONLY : _O_WRONLY;
                Reset(*stream, static_cast<int>(index), mode);
            }
            m_lookup.emplace(stream->header, stream); m_streams.push_back(stream);
        }
    }
    ~CrtStdio() { CloseAll(true); }
    CrtLegacyFile* Iob() { return m_iob.data(); }
    void* Standard(unsigned index)
    {
        if (index >= 3) { CrtDescriptorFailure(EINVAL); return nullptr; }
        return &m_iob[index];
    }

    void* Open(const wchar_t* path, const wchar_t* mode, int sharing, int defaultMode, int creationMask, bool commitDefault)
    {
        CrtStdioMode parsed;
        if (!path || !ParseCrtStdioMode(mode, commitDefault, &parsed)) { CrtDescriptorFailure(EINVAL); return nullptr; }
        int descriptor = -1;
        std::shared_ptr<Stream> stream;
        try
        {
            stream = Allocate(); if (!stream) return nullptr;
            descriptor = m_files.Open(path, parsed.flags, sharing, 0600 & ~creationMask, defaultMode);
            if (descriptor < 0) { stream->closed = true; return nullptr; }
            std::lock_guard<std::recursive_mutex> guard(stream->lock);
            Reset(*stream, descriptor, parsed); return stream->header;
        }
        catch (const std::bad_alloc&)
        {
            if (descriptor >= 0) m_files.Close(descriptor);
            if (stream) stream->closed = true;
            CrtDescriptorFailure(ENOMEM); return nullptr;
        }
    }
    void* Attach(int descriptor, const wchar_t* mode, bool commitDefault)
    {
        CrtStdioMode parsed;
        if (!ParseCrtStdioMode(mode, commitDefault, &parsed)) { CrtDescriptorFailure(EINVAL); return nullptr; }
        if (m_files.Handle(descriptor) == -1) return nullptr;
        try
        {
            auto stream = Allocate(); if (!stream) return nullptr;
            std::lock_guard<std::recursive_mutex> guard(stream->lock);
            Reset(*stream, descriptor, parsed); return stream->header;
        }
        catch (const std::bad_alloc&) { CrtDescriptorFailure(ENOMEM); return nullptr; }
    }
    void* Reopen(const wchar_t* path, const wchar_t* mode, void* handle, int defaultMode, int creationMask, bool commitDefault)
    {
        CrtStdioMode parsed;
        if (!path || !ParseCrtStdioMode(mode, commitDefault, &parsed)) { CrtDescriptorFailure(EINVAL); return nullptr; }
        auto stream = Lookup(handle); if (!stream) return nullptr;
        std::lock_guard<std::recursive_mutex> guard(stream->lock);
        if (!stream->closed) CloseLocked(*stream);
        try
        {
            const int descriptor = m_files.Open(path, parsed.flags, _SH_DENYNO, 0600 & ~creationMask, defaultMode);
            if (descriptor < 0) return nullptr;
            Reset(*stream, descriptor, parsed); return stream->header;
        }
        catch (const std::bad_alloc&) { CrtDescriptorFailure(ENOMEM); return nullptr; }
    }
    int Close(void* handle)
    {
        auto stream = Lookup(handle); if (!stream) return -1;
        std::lock_guard<std::recursive_mutex> guard(stream->lock);
        if (!Valid(*stream)) return -1;
        return CloseLocked(*stream);
    }
    int CloseAll(bool includeStandard = false, bool flush = true)
    {
        int closed = 0; bool failed = false;
        for (size_t index = 0;; ++index)
        {
            std::shared_ptr<Stream> stream;
            { std::lock_guard<std::mutex> guard(m_registryLock); if (index == m_streams.size()) break; stream = m_streams[index]; }
            std::lock_guard<std::recursive_mutex> guard(stream->lock);
            if (stream->closed || (stream->standard && !includeStandard)) continue;
            if (CloseLocked(*stream, flush) < 0) failed = true; else ++closed;
        }
        return failed ? -1 : closed;
    }
    int Flush(void* handle)
    {
        if (!handle) return FlushAll(false);
        auto stream = Lookup(handle); if (!stream) return -1;
        std::lock_guard<std::recursive_mutex> guard(stream->lock);
        return Valid(*stream) ? FlushLocked(*stream) : -1;
    }
    int FlushAll(bool returnCount)
    {
        int count = 0; bool failed = false;
        for (size_t index = 0;; ++index)
        {
            std::shared_ptr<Stream> stream;
            { std::lock_guard<std::mutex> guard(m_registryLock); if (index == m_streams.size()) break; stream = m_streams[index]; }
            std::lock_guard<std::recursive_mutex> guard(stream->lock);
            if (stream->closed) continue;
            if ((stream->header->flags & WriteFlag) || stream->mode.commit)
                if (FlushLocked(*stream) < 0) { failed = true; continue; }
            ++count;
        }
        return returnCount ? count : failed ? -1 : 0;
    }
    size_t Read(void* buffer, size_t elementSize, size_t count, void* handle)
    {
        if (!elementSize || !count) return 0;
        if (!buffer || count > (std::numeric_limits<size_t>::max)() / elementSize) { CrtDescriptorFailure(EINVAL); return 0; }
        auto stream = Lookup(handle); if (!stream) return 0;
        std::lock_guard<std::recursive_mutex> guard(stream->lock);
        return Valid(*stream) ? ReadLocked(*stream, buffer, elementSize * count) / elementSize : 0;
    }
    size_t Write(const void* buffer, size_t elementSize, size_t count, void* handle)
    {
        if (!elementSize || !count) return 0;
        if (!buffer || count > (std::numeric_limits<size_t>::max)() / elementSize) { CrtDescriptorFailure(EINVAL); return 0; }
        auto stream = Lookup(handle); if (!stream) return 0;
        std::lock_guard<std::recursive_mutex> guard(stream->lock);
        return Valid(*stream) ? WriteLocked(*stream, buffer, elementSize * count) / elementSize : 0;
    }
    int GetByte(void* handle)
    {
        auto stream = Lookup(handle); if (!stream) return -1;
        std::lock_guard<std::recursive_mutex> guard(stream->lock);
        unsigned char value;
        return Valid(*stream) && ReadLocked(*stream, &value, 1) == 1 ? value : -1;
    }
    int PutByte(int character, void* handle)
    {
        const auto value = static_cast<unsigned char>(character);
        return Write(&value, 1, 1, handle) == 1 ? value : -1;
    }
    int UngetByte(int character, void* handle)
    {
        if (character == -1) return -1;
        auto stream = Lookup(handle); if (!stream) return -1;
        std::lock_guard<std::recursive_mutex> guard(stream->lock);
        if (!Valid(*stream) || (stream->mode.flags & 3) == _O_WRONLY) return -1;
        const auto value = static_cast<unsigned char>(character);
        return PushByteLocked(*stream, value) ? value : -1;
    }
    char* GetLine(char* buffer, int count, void* handle)
    {
        if (!buffer || count <= 0) { CrtDescriptorFailure(EINVAL); return nullptr; }
        auto stream = Lookup(handle); if (!stream) return nullptr;
        std::lock_guard<std::recursive_mutex> guard(stream->lock);
        if (!Valid(*stream)) return nullptr;
        int position = 0;
        while (position + 1 < count)
        {
            unsigned char value;
            if (ReadLocked(*stream, &value, 1) != 1)
            {
                if ((stream->header->flags & ErrorFlag) && !(stream->header->flags & EofFlag)) return nullptr;
                break;
            }
            buffer[position++] = static_cast<char>(value);
            if (value == '\n') break;
        }
        if (!position && count != 1) return nullptr;
        buffer[position] = 0; return buffer;
    }
    int PutString(const char* text, void* handle)
    {
        if (!text) return CrtDescriptorFailure(EINVAL);
        auto stream = Lookup(handle); if (!stream) return -1;
        std::lock_guard<std::recursive_mutex> guard(stream->lock);
        if (!Valid(*stream)) return -1;
        const size_t length = std::strlen(text);
        return WriteLocked(*stream, text, length) == length ? 0 : -1;
    }
    unsigned short GetWide(void* handle)
    {
        auto stream = Lookup(handle); if (!stream) return 0xffff;
        std::lock_guard<std::recursive_mutex> guard(stream->lock);
        return Valid(*stream) ? GetWideLocked(*stream) : 0xffff;
    }
    unsigned short PutWide(unsigned short character, void* handle)
    {
        const wchar_t value = character;
        return WriteWideText(&value, 1, handle) == 0 ? character : 0xffff;
    }
    unsigned short UngetWide(unsigned short character, void* handle)
    {
        if (character == 0xffff) return 0xffff;
        auto stream = Lookup(handle); if (!stream) return 0xffff;
        std::lock_guard<std::recursive_mutex> guard(stream->lock);
        if (!Valid(*stream) || (stream->mode.flags & 3) == _O_WRONLY) return 0xffff;
        try { stream->pushedWide.push_back(character); }
        catch (const std::bad_alloc&) { CrtDescriptorFailure(ENOMEM); return 0xffff; }
        stream->header->flags &= ~EofFlag; stream->header->count = 0;
        return character;
    }
    wchar_t* GetWideLine(wchar_t* buffer, int count, void* handle)
    {
        if (!buffer || count <= 0) { CrtDescriptorFailure(EINVAL); return nullptr; }
        auto stream = Lookup(handle); if (!stream) return nullptr;
        std::lock_guard<std::recursive_mutex> guard(stream->lock);
        if (!Valid(*stream)) return nullptr;
        int position = 0;
        while (position + 1 < count)
        {
            const auto character = GetWideLocked(*stream);
            if (character == 0xffff)
            {
                if ((stream->header->flags & ErrorFlag) && !(stream->header->flags & EofFlag)) return nullptr;
                break;
            }
            buffer[position++] = character;
            if (character == '\n') break;
        }
        if (!position && count != 1) return nullptr;
        buffer[position] = 0; return buffer;
    }
    int WriteWideText(const wchar_t* text, size_t length, void* handle, _locale_t locale = nullptr)
    {
        if (!text) return CrtDescriptorFailure(EINVAL);
        auto stream = Lookup(handle); if (!stream) return -1;
        std::lock_guard<std::recursive_mutex> guard(stream->lock);
        if (!Valid(*stream)) return -1;
        if (length > (std::numeric_limits<size_t>::max)() / sizeof(wchar_t)) return CrtDescriptorFailure(EINVAL);
        if (!length) return 0;
        const int mode = m_files.Mode(stream->header->descriptor);
        if (mode < 0) { stream->header->flags |= ErrorFlag; return -1; }
        if (mode != _O_TEXT) return WriteLocked(*stream, text, length * sizeof(wchar_t)) == length * sizeof(wchar_t) ? 0 : -1;
        const UINT explicitPage = locale ? CrtLocaleCodePage(locale) : 0;
        if (!locale) locale = CurrentCrtLocaleHandle();
        const bool utf8 = CrtLocaleCodePage(locale) == CP_UTF8;
        for (size_t index = 0; index < length; ++index)
        {
            char bytes[MB_LEN_MAX];
            int encoded = 0;
            const wchar_t value = text[index];
            if (utf8 && value >= 0xd800 && value <= 0xdfff)
            {
                if (value > 0xdbff || index + 1 == length || text[index + 1] < 0xdc00 || text[index + 1] > 0xdfff)
                    return CrtDescriptorFailure(EILSEQ);
                encoded = ::WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, text + index, 2, bytes, sizeof(bytes), nullptr, nullptr);
                if (!encoded) return CrtIoFailure(::GetLastError());
                ++index;
            }
            else if (::_wctomb_s_l(&encoded, bytes, sizeof(bytes), value, locale) != 0) return -1;
            if (encoded <= 0) return CrtDescriptorFailure(EILSEQ);
            if (WriteLocked(*stream, bytes, encoded, explicitPage) != static_cast<size_t>(encoded)) return -1;
        }
        return 0;
    }
    int PutWideString(const wchar_t* text, void* handle)
    {
        if (!text) return CrtDescriptorFailure(EINVAL);
        return WriteWideText(text, wcslen(text), handle);
    }
    int PutLine(const char* text, void* handle)
    {
        if (!text) return CrtDescriptorFailure(EINVAL);
        auto stream = Lookup(handle); if (!stream) return -1;
        std::lock_guard<std::recursive_mutex> guard(stream->lock);
        if (!Valid(*stream)) return -1;
        const size_t length = std::strlen(text);
        const unsigned char newline = '\n';
        if (WriteLocked(*stream, text, length) != length || WriteLocked(*stream, &newline, 1) != 1) return -1;
        return 0;
    }
    int PutWideLine(const wchar_t* text, void* handle)
    {
        auto stream = Lookup(handle); if (!stream) return -1;
        std::lock_guard<std::recursive_mutex> guard(stream->lock);
        if (!Valid(*stream)) return -1;
        return PutWideString(text, handle) < 0 || PutWide(L'\n', handle) == 0xffff ? -1 : 0;
    }
    int Descriptor(void* handle)
    {
        auto stream = Lookup(handle); if (!stream) return -1;
        std::lock_guard<std::recursive_mutex> guard(stream->lock);
        return Valid(*stream) ? stream->header->descriptor : -1;
    }
    int Eof(void* handle) { return Flag(handle, EofFlag); }
    int Error(void* handle) { return Flag(handle, ErrorFlag); }
    void ClearError(void* handle)
    {
        auto stream = Lookup(handle); if (!stream) return;
        std::lock_guard<std::recursive_mutex> guard(stream->lock);
        if (!Valid(*stream)) return;
        stream->header->flags &= ~(ErrorFlag | EofFlag);
        m_files.ClearTextEof(stream->header->descriptor);
    }
    LONGLONG Tell(void* handle)
    {
        auto stream = Lookup(handle); if (!stream) return -1;
        std::lock_guard<std::recursive_mutex> guard(stream->lock);
        try { return Valid(*stream) ? TellLocked(*stream) : -1; }
        catch (const std::bad_alloc&) { return CrtDescriptorFailure(ENOMEM); }
    }
    int Seek(void* handle, LONGLONG offset, int origin)
    {
        if (origin < 0 || origin > 2) return CrtDescriptorFailure(EINVAL);
        auto stream = Lookup(handle); if (!stream) return -1;
        std::lock_guard<std::recursive_mutex> guard(stream->lock);
        if (!Valid(*stream)) return -1;
        if (origin == FILE_CURRENT)
        {
            LONGLONG position;
            try { position = TellLocked(*stream); }
            catch (const std::bad_alloc&) { return CrtDescriptorFailure(ENOMEM); }
            if (position < 0) return -1;
            if ((offset > 0 && position > LLONG_MAX - offset) || (offset < 0 && position < LLONG_MIN - offset)) return CrtDescriptorFailure(EINVAL);
            offset += position; origin = FILE_BEGIN;
        }
        if (FlushLocked(*stream) < 0 || m_files.Seek(stream->header->descriptor, offset, origin) < 0) return -1;
        stream->pushedBytes.clear(); stream->pushedWide.clear(); stream->pendingWide = -1;
        stream->header->count = 0; stream->header->flags &= ~EofFlag;
        return 0;
    }
    void Rewind(void* handle) { if (Seek(handle, 0, FILE_BEGIN) == 0) ClearError(handle); }
    int SetBuffer(void* handle, char* buffer, int mode, size_t size)
    {
        if ((mode != 0 && mode != 4 && mode != 0x40) || (mode != 4 && (size < 2 || size > INT_MAX))) return CrtDescriptorFailure(EINVAL);
        auto stream = Lookup(handle); if (!stream) return -1;
        std::lock_guard<std::recursive_mutex> guard(stream->lock);
        if (!Valid(*stream)) return -1;
        try
        {
            std::vector<unsigned char> storage;
            if (!buffer && mode != 4) storage.resize(size);
            if (FlushLocked(*stream) < 0) return -1;
            stream->output.swap(storage); stream->userBuffer = buffer;
            stream->capacity = mode == 4 ? 0 : size; stream->buffering = mode;
            return 0;
        }
        catch (const std::bad_alloc&) { return CrtDescriptorFailure(ENOMEM); }
    }
    void Lock(void* handle) { auto stream = Lookup(handle); if (stream) stream->lock.lock(); }
    void Unlock(void* handle) { auto stream = Lookup(handle); if (stream) stream->lock.unlock(); }
    int MaximumStreams() { std::lock_guard<std::mutex> guard(m_registryLock); return m_maximumStreams; }
    int SetMaximumStreams(int count)
    {
        if (count < 3 || count > 8192) return CrtDescriptorFailure(EINVAL);
        std::lock_guard<std::mutex> guard(m_registryLock);
        for (size_t index = static_cast<size_t>(count); index < m_streams.size(); ++index)
            if (!m_streams[index]->closed) return CrtDescriptorFailure(EINVAL);
        m_maximumStreams = count; return count;
    }

private:
    // Definitions below keep stdio bookkeeping separate from descriptor state.
    std::shared_ptr<Stream> Lookup(void* handle)
    {
        std::lock_guard<std::mutex> guard(m_registryLock);
        const auto found = m_lookup.find(static_cast<CrtLegacyFile*>(handle));
        if (found == m_lookup.end()) { CrtDescriptorFailure(EINVAL); return {}; }
        return found->second;
    }
    std::shared_ptr<Stream> Allocate()
    {
        std::lock_guard<std::mutex> guard(m_registryLock);
        for (size_t index = 3; index < m_streams.size() && index < static_cast<size_t>(m_maximumStreams); ++index)
        {
            const auto& stream = m_streams[index];
            std::unique_lock<std::recursive_mutex> lock(stream->lock, std::try_to_lock);
            if (lock.owns_lock() && stream->closed) { stream->closed = false; return stream; }
        }
        if (m_streams.size() >= static_cast<size_t>(m_maximumStreams)) { CrtDescriptorFailure(EMFILE); return {}; }
        auto stream = std::make_shared<Stream>(); stream->closed = false;
        m_streams.push_back(stream);
        try { m_lookup.emplace(stream->header, stream); }
        catch (...) { m_streams.pop_back(); throw; }
        return stream;
    }    static bool Valid(Stream& stream)
    { if (!stream.closed) return true; CrtDescriptorFailure(EINVAL); return false; }
    static void Reset(Stream& stream, int descriptor, const CrtStdioMode& mode)
    {
        *stream.header = {}; stream.header->descriptor = descriptor;
        stream.header->flags = NoBufferFlag | ((mode.flags & 3) == _O_RDWR ? UpdateFlag : (mode.flags & 3) == _O_WRONLY ? WriteFlag : ReadFlag);
        stream.mode = mode; stream.closed = false; stream.pending = 0; stream.pendingConsolePage = 0;
        stream.buffering = stream.standard ? 4 : 0; stream.capacity = stream.standard ? 0 : 4096;
        stream.userBuffer = nullptr; stream.output.clear(); stream.pushedBytes.clear(); stream.pushedWide.clear(); stream.pendingWide = -1;
    }
    int CloseLocked(Stream& stream, bool flush = true)
    {
        const int flushed = flush ? FlushLocked(stream) : 0;
        const int closed = m_files.Close(stream.header->descriptor);
        stream.closed = true; *stream.header = {}; stream.header->descriptor = -1;
        stream.output.clear(); stream.pushedBytes.clear(); stream.pushedWide.clear(); stream.pendingWide = -1; stream.pending = 0;
        return flushed < 0 || closed < 0 ? -1 : 0;
    }
    int Flag(void* handle, int flag)
    {
        auto stream = Lookup(handle); if (!stream) return 0;
        std::lock_guard<std::recursive_mutex> guard(stream->lock);
        return Valid(*stream) && (stream->header->flags & flag) ? 1 : 0;
    }
    unsigned char* OutputBuffer(Stream& stream)
    {
        if (stream.userBuffer) return reinterpret_cast<unsigned char*>(stream.userBuffer);
        if (stream.output.size() != stream.capacity) stream.output.resize(stream.capacity);
        return stream.output.data();
    }
    int FlushLocked(Stream& stream, bool commit = true)
    {
        size_t delivered = 0;
        while (delivered < stream.pending)
        {
            const unsigned request = static_cast<unsigned>((std::min)(stream.pending - delivered, static_cast<size_t>(INT_MAX)));
            const int written = m_files.Write(stream.header->descriptor, OutputBuffer(stream) + delivered, request,
                stream.pendingConsolePage);
            if (written <= 0)
            {
                if (!written) CrtIoFailure(ERROR_DISK_FULL);
                if (delivered) memmove(OutputBuffer(stream), OutputBuffer(stream) + delivered, stream.pending - delivered);
                stream.pending -= delivered; stream.header->flags |= ErrorFlag; return -1;
            }
            delivered += written;
        }
        stream.pending = 0;
        stream.pendingConsolePage = 0;
        if (commit && stream.mode.commit && m_files.Commit(stream.header->descriptor) < 0) { stream.header->flags |= ErrorFlag; return -1; }
        return 0;
    }
    static bool PushByteLocked(Stream& stream, unsigned char value)
    {
        try { stream.pushedBytes.push_back(value); }
        catch (const std::bad_alloc&) { CrtDescriptorFailure(ENOMEM); return false; }
        stream.header->flags &= ~EofFlag;
        stream.header->count = 0;
        return true;
    }
    size_t ReadLocked(Stream& stream, void* buffer, size_t count, bool singleWide = false)
    {
        stream.header->count = 0;
        if ((stream.mode.flags & 3) == _O_WRONLY) { CrtDescriptorFailure(EBADF); stream.header->flags |= ErrorFlag; return 0; }
        if (stream.pending && FlushLocked(stream) < 0) return 0;
        if (stream.header->flags & UpdateFlag) stream.header->flags &= ~WriteFlag;
        stream.header->flags |= ReadFlag;
        auto destination = static_cast<unsigned char*>(buffer);
        size_t consumed = 0;
        if (singleWide && !stream.pushedBytes.empty())
        {
            count = sizeof(wchar_t);
            singleWide = false;
        }
        while (consumed < count && !stream.pushedBytes.empty())
        { destination[consumed++] = stream.pushedBytes.back(); stream.pushedBytes.pop_back(); }
        if (stream.header->flags & EofFlag) return consumed;
        while (consumed < count)
        {
            const unsigned request = static_cast<unsigned>((std::min)(count - consumed, static_cast<size_t>(INT_MAX)));
            const int read = singleWide ? m_files.ReadWideCharacter(stream.header->descriptor, destination)
                                        : m_files.Read(stream.header->descriptor, destination + consumed, request);
            if (read < 0) { stream.header->flags |= ErrorFlag; break; }
            if (!read) { stream.header->flags |= EofFlag; break; }
            consumed += read;
            if (singleWide) break;
        }
        return consumed;
    }
    static wchar_t OutputUnit(const unsigned char* bytes)
    {
        wchar_t value;
        std::memcpy(&value, bytes, sizeof(value));
        return value;
    }
    static bool IsOutputSurrogatePair(const unsigned char* bytes)
    {
        const wchar_t high = OutputUnit(bytes);
        const wchar_t low = OutputUnit(bytes + sizeof(wchar_t));
        return high >= 0xd800 && high <= 0xdbff && low >= 0xdc00 && low <= 0xdfff;
    }
    static size_t FitOutput(const unsigned char* bytes, size_t remaining, size_t limit, size_t unit, bool utf8)
    {
        size_t fit = (std::min)(remaining, limit);
        fit -= fit % unit;
        if (utf8 && fit >= unit && fit < remaining && IsOutputSurrogatePair(bytes + fit - unit)) fit -= unit;
        return fit;
    }
    static size_t OutputLineSize(const unsigned char* bytes, size_t count, bool unicode, bool& newline)
    {
        const size_t unit = unicode ? sizeof(wchar_t) : 1;
        for (size_t offset = 0; offset < count; offset += unit)
        {
            const wchar_t value = unicode ? OutputUnit(bytes + offset) : bytes[offset];
            if (value == L'\n')
            {
                newline = true;
                return offset + unit;
            }
        }
        return count;
    }
    size_t WriteLocked(Stream& stream, const void* buffer, size_t count, UINT explicitPage = 0)
    {
        stream.header->count = 0;
        if ((stream.mode.flags & 3) == _O_RDONLY) { CrtDescriptorFailure(EBADF); stream.header->flags |= ErrorFlag; return 0; }
        if (stream.header->flags & UpdateFlag) stream.header->flags &= ~ReadFlag;
        stream.header->flags |= WriteFlag;
        const int mode = m_files.Mode(stream.header->descriptor);
        if (mode < 0) { stream.header->flags |= ErrorFlag; return 0; }
        const bool unicode = mode != _O_BINARY && mode != _O_TEXT;
        const bool utf8 = mode == _O_U8TEXT;
        const size_t unit = unicode ? sizeof(wchar_t) : 1;
        if (count % unit) { CrtDescriptorFailure(EINVAL); stream.header->flags |= ErrorFlag; return 0; }
        const UINT consolePage = mode == _O_TEXT ? m_files.ConsoleAnsiInputPage(stream.header->descriptor, explicitPage) : 0;
        if (stream.pending && stream.pendingConsolePage != consolePage && FlushLocked(stream, false) < 0) return 0;
        const auto bytes = static_cast<const unsigned char*>(buffer);
        const bool lineBuffered = stream.buffering == 0x40;
        size_t consumed = 0;
        try
        {
            while (consumed < count)
            {
                const auto source = bytes + consumed;
                const auto remaining = count - consumed;
                size_t copied = FitOutput(source, remaining, stream.capacity - stream.pending, unit, utf8);
                if (stream.pending && !copied)
                {
                    if (FlushLocked(stream, false) < 0) break;
                    continue;
                }
                if (!stream.pending && (!copied || (!lineBuffered && remaining >= stream.capacity)))
                {
                    // A pair may be larger than the configured buffer. In that
                    // case send both units together even on a line stream.
                    const size_t limit = lineBuffered ? 2 * unit : static_cast<size_t>(INT_MAX);
                    const auto request = static_cast<unsigned>(FitOutput(source, remaining, limit, unit, utf8));
                    const int written = m_files.Write(stream.header->descriptor, source, request, consolePage);
                    if (written > 0) consumed += written;
                    if (written != static_cast<int>(request)) { stream.header->flags |= ErrorFlag; break; }
                    continue;
                }

                bool newline = false;
                if (lineBuffered) copied = OutputLineSize(source, copied, unicode, newline);
                std::memcpy(OutputBuffer(stream) + stream.pending, source, copied);
                stream.pendingConsolePage = consolePage;
                stream.pending += copied;
                if (newline && FlushLocked(stream, false) < 0)
                {
                    // Keep earlier accepted data for fflush retry. The caller
                    // owns the unreported suffix of this write and may retry it.
                    const auto retryByCaller = (std::min)(stream.pending, copied);
                    stream.pending -= retryByCaller;
                    consumed += copied - retryByCaller;
                    break;
                }
                consumed += copied;
            }
        }
        catch (const std::bad_alloc&) { CrtDescriptorFailure(ENOMEM); stream.header->flags |= ErrorFlag; }
        return consumed;
    }
    unsigned short GetWideLocked(Stream& stream)
    {
        if (!stream.pushedWide.empty()) { const auto value = stream.pushedWide.back(); stream.pushedWide.pop_back(); return value; }
        if (stream.pendingWide >= 0) { const auto value = static_cast<unsigned short>(stream.pendingWide); stream.pendingWide = -1; return value; }
        const int mode = m_files.Mode(stream.header->descriptor);
        if (mode < 0) { stream.header->flags |= ErrorFlag; return 0xffff; }
        if (mode != _O_TEXT)
        {
            wchar_t units[2]{};
            const bool singleWide = mode == _O_U8TEXT;
            const size_t read = ReadLocked(stream, units, singleWide ? sizeof(units) : sizeof(wchar_t), singleWide);
            if (read < sizeof(wchar_t)) return 0xffff;
            if (read == sizeof(units)) stream.pendingWide = units[1];
            return units[0];
        }

        const auto locale = CurrentCrtLocaleHandle();
        const bool utf8 = CrtLocaleCodePage(locale) == CP_UTF8;
        unsigned char first;
        if (ReadLocked(stream, &first, 1) != 1) return 0xffff;
        const unsigned required = utf8 ? CrtUtf8SequenceSize(first) : ::_isctype_l(first, _LEADBYTE, locale) ? 2u : 1u;
        char bytes[4] = {static_cast<char>(first)};
        for (unsigned index = 1; index < required; ++index)
        {
            if (ReadLocked(stream, &bytes[index], 1) != 1)
            {
                // Native ANSI fgetwc makes a truncated DBCS lead available
                // to a later byte read and clears EOF if pushback succeeds.
                if (!utf8 && (stream.header->flags & EofFlag)) PushByteLocked(stream, first);
                else if (utf8 && (stream.header->flags & EofFlag)) CrtDescriptorFailure(EILSEQ);
                return 0xffff;
            }
            if (utf8 && (static_cast<unsigned char>(bytes[index]) & 0xc0) != 0x80)
            {
                if (PushByteLocked(stream, static_cast<unsigned char>(bytes[index]))) CrtDescriptorFailure(EILSEQ);
                return 0xffff;
            }
        }
        if (!utf8)
        {
            wchar_t wide = 0;
            return ::_mbtowc_l(&wide, bytes, required, locale) < 0 ? 0xffff : wide;
        }
        wchar_t units[2];
        const int length = ::MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, bytes, required, units, 2);
        if (!length) { CrtDescriptorFailure(EILSEQ); return 0xffff; }
        if (length == 2) stream.pendingWide = units[1];
        return units[0];
    }
    LONGLONG TellLocked(Stream& stream)
    {
        LONGLONG position = m_files.Position(stream.header->descriptor); if (position < 0) return -1;
        const int mode = m_files.Mode(stream.header->descriptor); if (mode < 0) return -1;
        if (stream.pending)
        {
            const auto bytes = OutputBuffer(stream);
            LONGLONG external = static_cast<LONGLONG>(stream.pending);
            if (mode == _O_TEXT)
            { for (size_t index = 0; index < stream.pending; ++index) if (bytes[index] == '\n') ++external; }
            else if (mode != _O_BINARY)
            {
                if (stream.pending % sizeof(wchar_t)) return CrtDescriptorFailure(EINVAL);
                std::wstring translated;
                for (size_t index = 0; index < stream.pending; index += sizeof(wchar_t))
                {
                    wchar_t character; memcpy(&character, bytes + index, sizeof(character));
                    if (character == L'\n') translated.push_back(L'\r'); translated.push_back(character);
                }
                if (mode == _O_U8TEXT)
                {
                    const int required = ::WideCharToMultiByte(CP_UTF8, 0, translated.data(), static_cast<int>(translated.size()), nullptr, 0, nullptr, nullptr);
                    if (!required) return CrtIoFailure(::GetLastError()); external = required;
                }
                else external = static_cast<LONGLONG>(translated.size() * sizeof(wchar_t));
            }
            if (position > LLONG_MAX - external) return CrtDescriptorFailure(EOVERFLOW);
            position += external;
        }
        // ungetc is only guaranteed to restore one character. Text-mode
        // positions after arbitrary pushed characters are implementation-defined.
        position -= static_cast<LONGLONG>(stream.pushedBytes.size());
        position -= static_cast<LONGLONG>(stream.pushedWide.size() * sizeof(wchar_t));
        return position < 0 ? 0 : position;
    }

    CrtFileDescriptors& m_files;
    std::array<CrtLegacyFile, 20> m_iob;
    std::mutex m_registryLock;
    std::map<CrtLegacyFile*, std::shared_ptr<Stream>> m_lookup;
    std::vector<std::shared_ptr<Stream>> m_streams;
    int m_maximumStreams = 512;
};

} }
