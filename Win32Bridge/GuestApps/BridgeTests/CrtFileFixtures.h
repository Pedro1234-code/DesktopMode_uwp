#pragma once
#include "Bridge/CRT/CrtFileDescriptors.h"
#include <cstring>
#include <string>

namespace
{
    class CrtMemoryFileBackend final : public Win32Bridge::Bridge::CrtFileBackend
    {
        struct File { std::vector<unsigned char> bytes; };
        struct HandleState { std::shared_ptr<File> file; size_t position = 0; DWORD access = 0; };
        std::map<std::wstring, std::shared_ptr<File>> m_files;
        std::map<uintptr_t, HandleState> m_handles;
        uintptr_t m_next = 0x123000;
        HandleState* Find(HANDLE handle, DWORD* error)
        {
            auto found = m_handles.find(reinterpret_cast<uintptr_t>(handle));
            if (found == m_handles.end()) { *error = ERROR_INVALID_HANDLE; return nullptr; }
            return &found->second;
        }
    public:
        unsigned closed = 0, flushed = 0, maxRead = 0, maxWrite = 0;
        HANDLE standard[3]{};
        int writesUntilFailure = -1;
        DWORD bytesWrittenOnFailure = 0;
        DWORD lastAccess = 0, lastShare = 0, lastCreate = 0, lastAttributes = 0;
        HANDLE Standard(unsigned index) override { return standard[index]; }
        bool SetStandard(unsigned index, HANDLE handle, DWORD* error) override
        { standard[index] = handle; *error = ERROR_SUCCESS; return true; }
        DWORD DataAccess(HANDLE handle) override
        { DWORD error = ERROR_SUCCESS; const auto state = Find(handle, &error); return state ? state->access : 0; }
        void Put(const wchar_t* path, const std::string& bytes)
        {
            auto file = std::make_shared<File>(); file->bytes.assign(bytes.begin(), bytes.end()); m_files[path] = file;
        }
        std::string Bytes(const wchar_t* path) { const auto& bytes = m_files.at(path)->bytes; return std::string(bytes.begin(), bytes.end()); }
        bool Open(const wchar_t* path, DWORD access, DWORD share, DWORD create, DWORD attributes, HANDLE* handle, DWORD* error) override
        {
            lastAccess = access; lastShare = share; lastCreate = create; lastAttributes = attributes;
            auto found = m_files.find(path);
            if (found != m_files.end() && create == CREATE_NEW) { *error = ERROR_FILE_EXISTS; return false; }
            if (found == m_files.end())
            {
                if (create == OPEN_EXISTING || create == TRUNCATE_EXISTING) { *error = ERROR_FILE_NOT_FOUND; return false; }
                found = m_files.emplace(path, std::make_shared<File>()).first;
            }
            if (create == CREATE_ALWAYS || create == TRUNCATE_EXISTING) found->second->bytes.clear();
            HandleState state; state.file = found->second; state.access = access;
            const auto token = m_next++; m_handles.emplace(token, state); *handle = reinterpret_cast<HANDLE>(token); return true;
        }
        bool Close(HANDLE handle, DWORD* error) override
        {
            if (!Find(handle, error)) return false;
            m_handles.erase(reinterpret_cast<uintptr_t>(handle)); ++closed; return true;
        }
        bool Read(HANDLE handle, void* buffer, DWORD count, DWORD* read, DWORD* error) override
        {
            auto state = Find(handle, error); if (!state) return false;
            if (!(state->access & GENERIC_READ)) { *error = ERROR_ACCESS_DENIED; return false; }
            const size_t remaining = state->position < state->file->bytes.size() ? state->file->bytes.size() - state->position : 0;
            *read = static_cast<DWORD>((std::min)(remaining, static_cast<size_t>(maxRead ? (std::min<DWORD>)(maxRead, count) : count)));
            if (*read) memcpy(buffer, state->file->bytes.data() + state->position, *read);
            state->position += *read; return true;
        }
        bool Write(HANDLE handle, const void* buffer, DWORD count, DWORD* written, DWORD* error) override
        {
            auto state = Find(handle, error); if (!state) return false;
            if (!(state->access & GENERIC_WRITE)) { *error = ERROR_ACCESS_DENIED; return false; }
            const bool fail = writesUntilFailure == 0;
            if (writesUntilFailure > 0) --writesUntilFailure;
            *written = fail ? (std::min)(bytesWrittenOnFailure, count)
                            : maxWrite ? (std::min<DWORD>)(maxWrite, count) : count;
            if (fail) *error = ERROR_DISK_FULL;
            if (state->position + *written > state->file->bytes.size()) state->file->bytes.resize(state->position + *written);
            if (*written) memcpy(state->file->bytes.data() + state->position, buffer, *written);
            state->position += *written; return !fail;
        }
        bool Seek(HANDLE handle, LONGLONG offset, DWORD origin, LONGLONG* result, DWORD* error) override
        {
            auto state = Find(handle, error); if (!state) return false;
            const LONGLONG base = origin == FILE_BEGIN ? 0 : origin == FILE_CURRENT ? state->position : state->file->bytes.size();
            if (base + offset < 0) { *error = ERROR_NEGATIVE_SEEK; return false; }
            state->position = static_cast<size_t>(base + offset); *result = static_cast<LONGLONG>(state->position); return true;
        }
        bool Size(HANDLE handle, LONGLONG* size, DWORD* error) override
        { auto state = Find(handle, error); if (!state) return false; *size = static_cast<LONGLONG>(state->file->bytes.size()); return true; }
        bool Resize(HANDLE handle, LONGLONG size, DWORD* error) override
        { auto state = Find(handle, error); if (!state) return false; state->file->bytes.resize(static_cast<size_t>(size)); return true; }
        bool Flush(HANDLE handle, DWORD* error) override { if (!Find(handle, error)) return false; ++flushed; return true; }
    };

    bool ValidateCrtStartupFileFixtures()
    {
        using namespace Win32Bridge::Bridge;
        auto backend = std::make_shared<CrtMemoryFileBackend>();
        backend->Put(L"inherited-input", "R");
        HANDLE output, input, file;
        DWORD error = ERROR_SUCCESS;
        if (!backend->Open(L"inherited-output", GENERIC_WRITE, 0, CREATE_NEW, 0, &output, &error) ||
            !backend->Open(L"inherited-input", GENERIC_READ, 0, OPEN_EXISTING, 0, &input, &error) ||
            !backend->Open(L"inherited-file", GENERIC_WRITE, 0, CREATE_NEW, 0, &file, &error)) return false;
        backend->standard[0] = input;
        backend->standard[1] = output;

        std::vector<CrtStartupFile> original(39);
        original[1] = {CrtStartupFile::Open, file};
        original[35] = {CrtStartupFile::Open | CrtStartupFile::Pipe, reinterpret_cast<HANDLE>(0x123fff)};
        original[36] = {CrtStartupFile::Open | CrtStartupFile::NoInherit, output};
        original[37] = {CrtStartupFile::Open | CrtStartupFile::Append, file};
        original[38] = {CrtStartupFile::Open | CrtStartupFile::Text | CrtStartupFile::Eof, input};
        auto packet = EncodeCrtStartupFiles(original);
        constexpr size_t count = 39;
        if (packet.size() != sizeof(int32_t) + count * (1 + sizeof(HANDLE)) ||
            packet[sizeof(int32_t) + 37] != (CrtStartupFile::Open | CrtStartupFile::Append)) return false;
        HANDLE packedHandle = nullptr;
        std::memcpy(&packedHandle, packet.data() + sizeof(int32_t) + count + 37 * sizeof(HANDLE), sizeof(HANDLE));
        if (packedHandle != file) return false;
        const auto inherited = DecodeCrtStartupFiles(packet);
        if (inherited.size() != count || inherited[36].IsInheritable()) return false;
        packet.pop_back();
        if (!DecodeCrtStartupFiles(packet).empty()) return false;
        const int32_t invalidCount = -1;
        std::memcpy(packet.data(), &invalidCount, sizeof(invalidCount));
        if (!DecodeCrtStartupFiles(packet).empty()) return false;

        CrtFileDescriptors files(backend);
        if (!files.InitializeStandards(inherited) || files.Handle(35) != -1 || files.Handle(36) != -1 ||
            files.Handle(0) != reinterpret_cast<intptr_t>(input) ||
            files.Handle(1) != reinterpret_cast<intptr_t>(file) ||
            files.Handle(37) != reinterpret_cast<intptr_t>(file) || files.Mode(1) != _O_TEXT ||
            files.Mode(37) != _O_BINARY) return false;
        const int privateFd = files.Open(L"private", _O_CREAT | _O_RDWR | _O_BINARY | _O_NOINHERIT,
            _SH_DENYNO, 0600, _O_BINARY);
        if (privateFd != 3 || !files.BuildSpawnData(true, &packet)) return false;
        const auto spawned = DecodeCrtStartupFiles(packet);
        if (spawned.size() != count || spawned[privateFd].IsInheritable() || spawned[1].handle != file ||
            spawned[1].flags != (CrtStartupFile::Open | CrtStartupFile::Text) ||
            spawned[37].flags != (CrtStartupFile::Open | CrtStartupFile::Append) ||
            spawned[38].flags != (CrtStartupFile::Open | CrtStartupFile::Text | CrtStartupFile::Eof) ||
            !files.BuildSpawnData(false, &packet)) return false;
        const auto detached = DecodeCrtStartupFiles(packet);
        if (detached.size() != count || detached[0].IsInheritable() || detached[1].IsInheritable() ||
            detached[37].handle != file || detached[38].handle != input) return false;
        char byte;
        if (files.Read(37, &byte, 1) != -1 || errno != EBADF ||
            files.Write(38, "!", 1) != -1 || errno != EBADF || files.Read(38, &byte, 1) != 0 ||
            files.ClearTextEof(38) || files.Read(38, &byte, 1) != 1 || byte != 'R') return false;
        if (files.Write(1, "\n", 1) != 1 || files.Close(1) != 0 || backend->closed ||
            backend->standard[1] != output || files.Seek(37, 0, FILE_BEGIN) != 0 ||
            files.Write(37, "X", 1) != 1 || backend->Bytes(L"inherited-file") != "\r\nX" ||
            files.Close(37) != 0 || backend->closed != 1) return false;
        return backend->Close(output, &error);
    }

    bool ValidateCrtFileFixtures()
    {
        using namespace Win32Bridge::Bridge;
        if (!ValidateCrtStartupFileFixtures()) return false;
        auto backend = std::make_shared<CrtMemoryFileBackend>();
        CrtFileDescriptors files(backend);
        if (files.Open(L"missing", _O_RDONLY, _SH_DENYNO, 0600, _O_TEXT) != -1 || errno != ENOENT) return false;
        int binary = files.Open(L"binary", _O_CREAT | _O_TRUNC | _O_RDWR | _O_BINARY, _SH_DENYWR, 0600, _O_TEXT);
        if (binary < 3 || backend->lastCreate != CREATE_ALWAYS || backend->lastShare != FILE_SHARE_READ ||
            backend->lastAccess != (GENERIC_READ | GENERIC_WRITE)) return false;
        const char raw[] = { 'a', '\n', 0x1a, 'b' };
        if (files.Write(binary, raw, sizeof(raw)) != sizeof(raw) || files.Seek(binary, 0, FILE_BEGIN) != 0) return false;
        char buffer[32]{};
        if (files.Read(binary, buffer, sizeof(buffer)) != sizeof(raw) || memcmp(raw, buffer, sizeof(raw)) || files.Eof(binary) != 1) return false;
        const int duplicate = files.Duplicate(binary);
        if (duplicate < 0 || files.Seek(duplicate, 1, FILE_BEGIN) != 1 || files.Seek(binary, 0, FILE_CURRENT) != 1) return false;
        const auto beforeClose = backend->closed;
        if (files.Close(binary) || backend->closed != beforeClose || files.Read(duplicate, buffer, 1) != 1 || buffer[0] != '\n') return false;
        if (files.Commit(duplicate) || backend->flushed != 1 || files.Resize(duplicate, 8) || files.Size(duplicate) != 8 ||
            files.Seek(duplicate, 0, FILE_CURRENT) != 2 || files.Close(duplicate) || backend->closed != beforeClose + 1) return false;
        if (files.Read(duplicate, buffer, 1) != -1 || errno != EBADF) return false;

        int text = files.Open(L"text", _O_CREAT | _O_RDWR, _SH_DENYNO, 0600, _O_TEXT);
        backend->maxWrite = 1;
        if (files.Write(text, "a\nb\r\n", 5) != 5 || backend->Bytes(L"text") != "a\r\nb\r\r\n") return false;
        backend->maxWrite = 0;
        if (files.Seek(text, 0, FILE_BEGIN) || files.Read(text, buffer, sizeof(buffer)) != 5 || memcmp(buffer, "a\nb\r\n", 5)) return false;
        if (files.SetMode(text, _O_BINARY) != _O_TEXT || files.Seek(text, 0, FILE_BEGIN) || files.Read(text, buffer, 3) != 3 || memcmp(buffer, "a\r\n", 3)) return false;
        if (files.SetMode(text, _O_TEXT) != _O_BINARY || files.Seek(text, 0, FILE_BEGIN) || files.Read(text, buffer, 2) != 2 || memcmp(buffer, "a\n", 2)) return false;
        if (files.Read(text, buffer, 2) != 2 || memcmp(buffer, "b\r", 2) || files.Read(text, buffer, 2) != 1 || buffer[0] != '\n') return false;
        if (files.Close(text)) return false;

        const int interrupted = files.Open(L"interrupted-crlf", _O_CREAT | _O_TRUNC | _O_RDWR | _O_TEXT, _SH_DENYNO, 0600, _O_BINARY);
        if (interrupted < 0) return false;
        backend->writesUntilFailure = 0;
        backend->bytesWrittenOnFailure = 1;
        if (files.Write(interrupted, "\nZ", 2) != -1 || errno != ENOSPC || backend->Bytes(L"interrupted-crlf") != "\r") return false;
        backend->writesUntilFailure = -1;
        backend->bytesWrittenOnFailure = 0;
        if (files.Write(interrupted, "Z", 1) != -1 || errno != EINVAL || backend->Bytes(L"interrupted-crlf") != "\r" ||
            files.Write(interrupted, "\nZ", 2) != 2 || backend->Bytes(L"interrupted-crlf") != "\r\nZ" || files.Close(interrupted)) return false;

        backend->Put(L"ctrlz", std::string("a\x1a" "b", 3));
        int ctrlz = files.Open(L"ctrlz", _O_RDONLY | _O_TEXT, _SH_DENYNO, 0600, _O_BINARY);
        if (files.Read(ctrlz, buffer, sizeof(buffer)) != 1 || buffer[0] != 'a' || files.Read(ctrlz, buffer, sizeof(buffer)) != 0 ||
            files.Seek(ctrlz, 2, FILE_BEGIN) != 2 || files.Read(ctrlz, buffer, 1) != 1 || buffer[0] != 'b') return false;
        if (files.Write(ctrlz, "x", 1) != -1 || errno != EBADF || files.Resize(ctrlz, 0) != -1 || errno != EACCES) return false;
        if (files.Close(ctrlz)) return false;

        backend->Put(L"append", "old");
        int append = files.Open(L"append", _O_WRONLY | _O_APPEND | _O_BINARY, _SH_DENYNO, 0600, _O_TEXT);
        if (files.Seek(append, 0, FILE_BEGIN) || files.Write(append, "+", 1) != 1 || backend->Bytes(L"append") != "old+") return false;
        int replaced = files.Open(L"replaced", _O_CREAT | _O_RDWR | _O_BINARY, _SH_DENYNO, 0600, _O_TEXT);
        const auto beforeReplace = backend->closed;
        if (files.Duplicate(append, replaced) || backend->closed != beforeReplace + 1 || files.Duplicate(append, append) ||
            files.Close(append) || files.Write(replaced, "!", 1) != 1 || backend->Bytes(L"append") != "old+!") return false;
        if (files.Duplicate(replaced, 0) || files.Close(replaced) || files.Write(0, "?", 1) != 1) return false;
        files.CloseAll();
        if (files.Handle(0) != -1 || errno != EBADF) return false;

        if (files.Open(L"binary", _O_CREAT | _O_EXCL, _SH_DENYNO, 0600, _O_TEXT) != -1 || errno != EEXIST) return false;
        if (files.Open(L"binary", _O_RDONLY, 0x1234, 0600, _O_TEXT) != -1 || errno != EINVAL) return false;
        return true;
    }
}
namespace {
    std::string CrtUtf16Bytes(const std::wstring& text)
    {
        static_assert(sizeof(wchar_t) == 2, "Windows UTF-16 ABI required");
        return std::string(reinterpret_cast<const char*>(text.data()), text.size() * sizeof(wchar_t));
    }
    bool ValidateCrtUnicodeFileFixtures()
    {
        using namespace Win32Bridge::Bridge;
        auto backend = std::make_shared<CrtMemoryFileBackend>(); CrtFileDescriptors files(backend);
        const std::string utf8Bom("\xef\xbb\xbf", 3), utf16Bom("\xff\xfe", 2);
        const std::wstring message = L"A\u00e9\xd83d\xde42\n";
        const auto messageBytes = static_cast<unsigned>(message.size() * sizeof(wchar_t));
        backend->maxWrite = 1;
        int file = files.Open(L"utf8", _O_CREAT | _O_TRUNC | _O_RDWR | _O_U8TEXT, _SH_DENYNO, 0600, _O_TEXT);
        if (file < 0 || files.Seek(file, 0, FILE_CURRENT) != 3 ||
            files.Write(file, message.data(), messageBytes) != messageBytes ||
            backend->Bytes(L"utf8") != utf8Bom + "A\xc3\xa9\xf0\x9f\x99\x82\r\n") return false;
        if (files.Write(file, message.data(), 3) != -1 || errno != EINVAL || files.Close(file)) return false;
        backend->maxWrite = 0;
        file = files.Open(L"interrupted-utf8", _O_CREAT | _O_TRUNC | _O_RDWR | _O_U8TEXT, _SH_DENYNO, 0600, _O_TEXT);
        if (file < 0) return false;
        const wchar_t utf8Retry[] = L"A\xd83d\xde42Z";
        backend->writesUntilFailure = 0;
        backend->bytesWrittenOnFailure = 2;
        if (files.Write(file, utf8Retry, 8) != 2 || errno != ENOSPC || backend->Bytes(L"interrupted-utf8") != utf8Bom + "A\xf0") return false;
        backend->bytesWrittenOnFailure = 1;
        if (files.Write(file, utf8Retry + 1, 6) != -1 || errno != ENOSPC || backend->Bytes(L"interrupted-utf8") != utf8Bom + "A\xf0\x9f") return false;
        backend->writesUntilFailure = -1;
        backend->bytesWrittenOnFailure = 0;
        if (files.Write(file, utf8Retry + 1, 6) != 6 || backend->Bytes(L"interrupted-utf8") != utf8Bom + "A\xf0\x9f\x99\x82Z" || files.Close(file)) return false;
        file = files.Open(L"interrupted-utf16", _O_CREAT | _O_TRUNC | _O_RDWR | _O_U16TEXT, _SH_DENYNO, 0600, _O_TEXT);
        if (file < 0) return false;
        backend->writesUntilFailure = 0;
        backend->bytesWrittenOnFailure = 1;
        if (files.Write(file, L"\u00e9!", 4) != -1 || errno != ENOSPC || backend->Bytes(L"interrupted-utf16") != utf16Bom + "\xe9") return false;
        backend->writesUntilFailure = -1;
        backend->bytesWrittenOnFailure = 0;
        if (files.Write(file, L"\u00e9!", 4) != 4 || backend->Bytes(L"interrupted-utf16") != utf16Bom + CrtUtf16Bytes(L"\u00e9!") || files.Close(file)) return false;
        file = files.Open(L"utf8", _O_RDONLY | _O_WTEXT, _SH_DENYNO, 0600, _O_TEXT);
        wchar_t wide[32]{};
        if (file < 0 || files.Seek(file, 0, FILE_CURRENT) != 3 || files.Read(file, wide, sizeof(wide)) != messageBytes ||
            std::wstring(wide, message.size()) != message || files.Read(file, wide, 3) != -1 || errno != EINVAL) return false;
        if (files.SetMode(file, _O_U16TEXT) != _O_U8TEXT || files.SetMode(file, _O_BINARY) != _O_WTEXT ||
            files.SetMode(file, _O_TEXT) != _O_BINARY || files.Close(file)) return false;

        file = files.Open(L"utf16", _O_CREAT | _O_TRUNC | _O_WRONLY | _O_WTEXT, _SH_DENYNO, 0600, _O_TEXT);
        if (file < 0 || files.Write(file, L"x\ny", 6) != 6 || backend->Bytes(L"utf16") != utf16Bom + CrtUtf16Bytes(L"x\r\ny") || files.Close(file)) return false;
        file = files.Open(L"utf16", _O_RDONLY | _O_U8TEXT, _SH_DENYNO, 0600, _O_TEXT);
        if (file < 0 || files.Read(file, wide, sizeof(wide)) != 6 || std::wstring(wide, 3) != L"x\ny" || files.Close(file)) return false;
        file = files.Open(L"utf16", _O_WRONLY | _O_APPEND | _O_U8TEXT, _SH_DENYNO, 0600, _O_TEXT);
        if (file < 0 || backend->lastAccess != (GENERIC_READ | GENERIC_WRITE) || files.Write(file, L"!", 2) != 2 ||
            backend->Bytes(L"utf16") != utf16Bom + CrtUtf16Bytes(L"x\r\ny!") || files.Close(file)) return false;

        backend->Put(L"ansi", std::string("\xe9\n", 2));
        file = files.Open(L"ansi", _O_RDONLY | _O_WTEXT, _SH_DENYNO, 0600, _O_TEXT);
        char bytes[8]{};
        if (file < 0 || files.Read(file, bytes, 3) != 2 || memcmp(bytes, "\xe9\n", 2) || files.Close(file)) return false;
        backend->Put(L"raw16", CrtUtf16Bytes(L"a\r\nb"));
        file = files.Open(L"raw16", _O_RDONLY | _O_U16TEXT, _SH_DENYNO, 0600, _O_TEXT);
        if (file < 0 || files.Read(file, wide, 4) != 4 || std::wstring(wide, 2) != L"a\n" || files.Read(file, wide, 4) != 2 || wide[0] != L'b' || files.Close(file)) return false;

        backend->Put(L"split", utf8Bom + "abc\xc3\xa9\xf0\x9f\x99\x82");
        file = files.Open(L"split", _O_RDONLY | _O_U8TEXT, _SH_DENYNO, 0600, _O_TEXT);
        if (file < 0 || files.Read(file, wide, 6) != 6 || std::wstring(wide, 3) != L"abc" ||
            files.Read(file, wide, 6) != 2 || wide[0] != L'\u00e9' || files.Seek(file, 0, FILE_CURRENT) != 8 ||
            files.Read(file, wide, 8) != 4 || wide[0] != 0xd83d || wide[1] != 0xde42 || files.Close(file)) return false;
        backend->Put(L"minimal", utf8Bom + "\xc3\xa9!");
        file = files.Open(L"minimal", _O_RDONLY | _O_U8TEXT, _SH_DENYNO, 0600, _O_TEXT);
        if (file < 0 || files.Read(file, wide, 2) != 2 || wide[0] != L'\u00e9' ||
            files.Read(file, wide, 2) != 2 || wide[0] != L'!' || files.Close(file)) return false;
        backend->Put(L"truncated", utf8Bom + std::string("\xc3", 1));
        file = files.Open(L"truncated", _O_RDONLY | _O_U8TEXT, _SH_DENYNO, 0600, _O_TEXT);
        if (file < 0 || files.Read(file, wide, sizeof(wide)) != -1 || errno != EILSEQ || files.Close(file)) return false;
        backend->Put(L"big-endian", std::string("\xfe\xff", 2));
        const auto before = backend->closed;
        if (files.Open(L"big-endian", _O_RDONLY | _O_U16TEXT, _SH_DENYNO, 0600, _O_TEXT) != -1 ||
            errno != EINVAL || backend->closed != before + 1) return false;

        const std::wstring boundary = std::wstring(1023, L'a') + L"\xd83d\xde42\n";
        file = files.Open(L"boundary", _O_CREAT | _O_TRUNC | _O_WRONLY | _O_U8TEXT, _SH_DENYNO, 0600, _O_TEXT);
        if (file < 0 || files.Write(file, boundary.data(), static_cast<unsigned>(boundary.size() * 2)) != boundary.size() * 2 ||
            backend->Bytes(L"boundary") != utf8Bom + std::string(1023, 'a') + "\xf0\x9f\x99\x82\r\n" || files.Close(file)) return false;
        file = files.Open(L"changed", _O_CREAT | _O_TRUNC | _O_RDWR | _O_BINARY, _SH_DENYNO, 0600, _O_TEXT);
        if (file < 0 || files.SetMode(file, _O_U8TEXT) != _O_BINARY || files.Write(file, L"\u00e9", 2) != 2 ||
            backend->Bytes(L"changed") != "\xc3\xa9" || files.Close(file)) return false;
        return true;
    }
}
