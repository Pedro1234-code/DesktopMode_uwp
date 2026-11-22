#pragma once
#include "Bridge/CRT/CrtFileDescriptors.h"
#include "Bridge/CRT/CrtStdio.h"
#include "Bridge/GuestConsole.h"
#include "Bridge/GuestPipes.h"

namespace
{
    // Exercise the real transport contexts and CRT classes without UWP storage.
    // No message-pump retry is needed: fixture input is queued before each read.
    class CrtStreamFixtureBackend final : public Win32Bridge::Bridge::CrtFileBackend
    {
    public:
        Win32Bridge::Bridge::GuestConsoleContext console;
        Win32Bridge::Bridge::GuestPipeContext pipes;
        DWORD maxRead = MAXDWORD;
        bool failNextWideWrite = false;
        UINT localePage = 0;
        UINT ConsoleOutputCodePage() override { return console.CodePage(true); }
        UINT ConsoleLocaleCodePage() override { return localePage; }
        HANDLE Standard(unsigned index) override
        { DWORD error; return console.GetStandard(static_cast<DWORD>(-10 - static_cast<int>(index)), &error); }
        bool SetStandard(unsigned index, HANDLE handle, DWORD* error) override
        { return console.SetStandard(static_cast<DWORD>(-10 - static_cast<int>(index)), handle, error); }
        DWORD Type(HANDLE handle, DWORD* error) override
        {
            *error = ERROR_SUCCESS;
            if (console.Owns(handle)) return FILE_TYPE_CHAR;
            if (pipes.Owns(handle)) return FILE_TYPE_PIPE;
            *error = ERROR_INVALID_HANDLE; return FILE_TYPE_UNKNOWN;
        }
        bool Duplicate(HANDLE handle, bool inherit, HANDLE* result, DWORD* error) override
        { return console.Owns(handle) ? console.Duplicate(handle, inherit, result, error) : pipes.Duplicate(handle, inherit, result, error); }
        bool Open(const wchar_t*, DWORD, DWORD, DWORD, DWORD, HANDLE*, DWORD* error) override
        { return Unsupported(error); }
        bool Close(HANDLE handle, DWORD* error) override
        { return console.Owns(handle) ? console.Close(handle, error) : pipes.Close(handle, error); }
        bool Read(HANDLE handle, void* buffer, DWORD count, DWORD* done, DWORD* error) override
        {
            count = (std::min)(count, maxRead);
            if (console.Owns(handle)) return console.ReadBytes(handle, static_cast<char*>(buffer), count, done, error);
            if (pipes.Read(handle, static_cast<BYTE*>(buffer), count, done, error)) return true;
            if (*error == ERROR_BROKEN_PIPE) { *done = 0; *error = ERROR_SUCCESS; return true; }
            return false;
        }
        bool Write(HANDLE handle, const void* buffer, DWORD count, DWORD* done, DWORD* error) override
        {
            if (!console.Owns(handle)) return pipes.Write(handle, static_cast<const BYTE*>(buffer), count, done, error);
            *done = 0;
            const int size = ::MultiByteToWideChar(CP_UTF8, 0, static_cast<const char*>(buffer), count, nullptr, 0);
            std::wstring wide(size, L'\0');
            if (size && !::MultiByteToWideChar(CP_UTF8, 0, static_cast<const char*>(buffer), count, &wide[0], size))
            { *error = ERROR_NO_UNICODE_TRANSLATION; return false; }
            if (!console.Write(handle, wide.data(), wide.size(), error)) return false;
            *done = count; return true;
        }
        bool ReadWide(HANDLE handle, wchar_t* buffer, DWORD count, DWORD* done, DWORD* error) override
        { return console.Read(handle, buffer, count, done, error); }
        bool WriteWide(HANDLE handle, const wchar_t* buffer, DWORD count, DWORD* done, DWORD* error) override
        {
            *done = 0;
            if (failNextWideWrite)
            {
                failNextWideWrite = false; *error = ERROR_WRITE_FAULT; return false;
            }
            if (!console.Write(handle, buffer, count, error)) return false;
            *done = count;
            return true;
        }
        bool Seek(HANDLE, LONGLONG, DWORD, LONGLONG*, DWORD* error) override { return Unsupported(error); }
        bool Size(HANDLE, LONGLONG*, DWORD* error) override { return Unsupported(error); }
        bool Resize(HANDLE, LONGLONG, DWORD* error) override { return Unsupported(error); }
        bool Flush(HANDLE, DWORD* error) override { return Unsupported(error); }
    private:
        static bool Unsupported(DWORD* error) { *error = ERROR_INVALID_FUNCTION; return false; }
    };

    bool ValidateCrtStreamFixtures()
    {
        using namespace Win32Bridge::Bridge;
        auto backend = std::make_shared<CrtStreamFixtureBackend>(); DWORD error = 0, done = 0;
        if (!backend->console.Allocate(CP_UTF8, &error)) return false;
        CrtFileDescriptors files(backend); CrtStdio io(files);
        if (!files.InitializeStandards() || !files.InitializeStandards() || !files.IsTerminal(0) ||
            !files.IsTerminal(1) || !files.IsTerminal(2)) return false;
        if (io.Write("out\n", 1, 4, io.Standard(1)) != 4 || backend->console.DrainOutput() != L"out\r\n") return false;
        const BYTE utf8Lead[] = {0xf0, 0x9f};
        const BYTE utf8Tail[] = {0x99, 0x82, '\n'};
        if (files.Write(1, utf8Lead, sizeof(utf8Lead)) != sizeof(utf8Lead) ||
            !backend->console.DrainOutput().empty()) return false;
        const int alias = files.Duplicate(1);
        if (alias < 3 || files.Write(alias, "X", 1) != 1 || files.Close(alias) ||
            backend->console.DrainOutput() != L"X") return false;
        backend->failNextWideWrite = true;
        if (files.Write(1, utf8Tail, sizeof(utf8Tail)) != -1 || !backend->console.DrainOutput().empty() ||
            files.Write(1, utf8Tail, sizeof(utf8Tail)) != sizeof(utf8Tail) ||
            backend->console.DrainOutput() != L"\U0001f642\r\n") return false;

        // Complete a character across an internal batch boundary in one write.
        std::string longOutput(1023, 'x');
        longOutput += "\xc3\xa9";
        if (files.Write(1, longOutput.data(), static_cast<unsigned>(longOutput.size())) != static_cast<int>(longOutput.size()) ||
            backend->console.DrainOutput() != std::wstring(1023, L'x') + L"\u00e9") return false;

        if (!backend->console.SetCodePage(true, 932)) return false;
        void* stdoutStream = io.Standard(1);
        if (io.SetBuffer(stdoutStream, nullptr, 0, 3) || io.Write("\x82", 1, 1, stdoutStream) != 1 ||
            io.Flush(stdoutStream) || !backend->console.DrainOutput().empty() ||
            io.Write("\xa0\n", 1, 2, stdoutStream) != 2 || io.Flush(stdoutStream) ||
            backend->console.DrainOutput() != L"\u3042\r\n" || io.SetBuffer(stdoutStream, nullptr, 4, 0) ||
            !backend->console.SetCodePage(true, CP_UTF8)) return false;

        backend->localePage = 932;
        if (files.Write(1, "\x82", 1) != 1 || !backend->console.DrainOutput().empty() ||
            files.Write(1, "\xa0", 1) != 1 || backend->console.DrainOutput() != L"\u3042") return false;
        backend->localePage = 1252;
        if (io.SetBuffer(stdoutStream, nullptr, 0, 8) || io.Write("\xe9", 1, 1, stdoutStream) != 1) return false;
        backend->localePage = CP_UTF8;
        if (io.Flush(stdoutStream) || backend->console.DrainOutput() != L"\u00e9" ||
            io.SetBuffer(stdoutStream, nullptr, 4, 0)) return false;
        if (!backend->console.SetCodePage(true, 437) || files.Write(1, "\xe3\x81\x82", 3) != 3 ||
            backend->console.DrainOutput() != L"?" || !backend->console.SetCodePage(true, CP_UTF8)) return false;
        backend->localePage = 0;

        if (!backend->console.FeedInput(L"in\r\n")) return false;
        char line[32]{};
        if (!io.GetLine(line, sizeof(line), io.Standard(0)) || std::string(line) != "in\n") return false;
        if (files.SetMode(1, _O_U16TEXT) != _O_TEXT) return false;
        const wchar_t wide[] = L"\u00e9\n";
        if (files.Write(1, wide, sizeof(wide)-sizeof(wchar_t)) != sizeof(wide)-sizeof(wchar_t) ||
            backend->console.DrainOutput() != L"\u00e9\r\n") return false;
        if (files.SetMode(0, _O_U8TEXT) != _O_TEXT || !backend->console.FeedInput(L"\u00e9\r\n")) return false;
        wchar_t received[8]{};
        if (files.Read(0, received, sizeof(received)) != 2*sizeof(wchar_t) || std::wstring(received, 2) != L"\u00e9\n") return false;

        HANDLE reader, writer;
        if (!backend->pipes.Create(true, 64, &reader, &writer, &error)) return false;
        int readFd = files.Adopt(reader, _O_RDONLY | _O_TEXT);
        int writeFd = files.Adopt(writer, _O_WRONLY | _O_TEXT);
        if (readFd < 3 || writeFd < 3 || files.IsTerminal(readFd) || files.Duplicate(writeFd, 1)) return false;
        const HANDLE redirected = backend->Standard(1);
        if (redirected == writer || redirected != reinterpret_cast<HANDLE>(files.Handle(1)) || files.Close(writeFd)) return false;
        // A live duplicate must keep the writer alive after closing the source fd.
        if (files.SetMode(1, _O_TEXT) != _O_TEXT || io.Write("pipe\n", 1, 5, io.Standard(1)) != 5 ||
            files.Write(1, utf8Lead, sizeof(utf8Lead)) != sizeof(utf8Lead) ||
            files.Write(1, utf8Tail, sizeof(utf8Tail)) != sizeof(utf8Tail) ||
            io.Close(io.Standard(1))) return false;
        if (backend->Standard(1) != nullptr) return false;
        void* stream = io.Attach(readFd, L"rt", false);
        if (!stream || !io.GetLine(line, sizeof(line), stream) || std::string(line) != "pipe\n" ||
            !io.GetLine(line, sizeof(line), stream) || std::string(line) != "\xf0\x9f\x99\x82\n" ||
            io.GetByte(stream) != -1 || !io.Eof(stream) || io.Error(stream) || io.Close(stream)) return false;

        if (!backend->pipes.Create(false, 64, &reader, &writer, &error)) return false;
        const BYTE physical[] = { 'a', '\r', 'X', '\r', '\n', 'b' };
        if (!backend->pipes.Write(writer, physical, sizeof(physical), &done, &error) || done != sizeof(physical) ||
            !backend->pipes.Close(writer, &error)) return false;
        readFd = files.Adopt(reader, _O_RDONLY | _O_TEXT);
        std::string actual;
        for (int i = 0; i < 8; ++i)
        {
            char ch; const int count = files.Read(readFd, &ch, 1);
            if (count < 0) return false;
            if (!count) break;
            actual += ch;
        }
        if (actual != "a\rX\nb" || files.Close(readFd)) return false;

        // A UTF-8 character split across physical reads needs no seek support.
        if (!backend->pipes.Create(false, 64, &reader, &writer, &error)) return false;
        const BYTE utf8[] = { 0xc3, 0xa9, '\r', '\n' };
        if (!backend->pipes.Write(writer, utf8, sizeof(utf8), &done, &error) || !backend->pipes.Close(writer, &error)) return false;
        backend->maxRead = 1;
        readFd = files.Adopt(reader, _O_RDONLY | _O_U8TEXT);
        wchar_t ch;
        if (files.Read(readFd, &ch, sizeof(ch)) != sizeof(ch) || ch != L'\u00e9' ||
            files.Read(readFd, &ch, sizeof(ch)) != sizeof(ch) || ch != L'\n' ||
            files.Read(readFd, &ch, sizeof(ch)) || files.Close(readFd)) return false;

        // fgetwc exposes the pair a unit at a time even when the source cannot seek.
        if (!backend->pipes.Create(false, 64, &reader, &writer, &error)) return false;
        const BYTE supplementary[] = {0xf0, 0x9f, 0x99, 0x82, 'A', '\r', '\n'};
        if (!backend->pipes.Write(writer, supplementary, sizeof(supplementary), &done, &error) ||
            done != sizeof(supplementary) || !backend->pipes.Close(writer, &error)) return false;
        readFd = files.Adopt(reader, _O_RDONLY | _O_U8TEXT);
        stream = io.Attach(readFd, L"rt", false);
        errno = 0;
        if (!stream || io.GetWide(stream) != 0xd83d || io.GetWide(stream) != 0xde42 || errno ||
            io.GetWide(stream) != L'A' || io.GetWide(stream) != L'\n' || io.GetWide(stream) != 0xffff ||
            !io.Eof(stream) || io.Error(stream) || io.Close(stream)) return false;

        // UTF-16 short physical reads must preserve both halves of a code unit.
        if (!backend->pipes.Create(false, 64, &reader, &writer, &error)) return false;
        const BYTE utf16[] = { 0xe9, 0, '\r', 0, '\n', 0 };
        if (!backend->pipes.Write(writer, utf16, sizeof(utf16), &done, &error) || !backend->pipes.Close(writer, &error)) return false;
        readFd = files.Adopt(reader, _O_RDONLY | _O_U16TEXT);
        if (files.Read(readFd, &ch, sizeof(ch)) != sizeof(ch) || ch != L'\u00e9' ||
            files.Read(readFd, &ch, sizeof(ch)) != sizeof(ch) || ch != L'\n' ||
            files.Read(readFd, &ch, sizeof(ch)) || files.Close(readFd)) return false;
        // Private lookahead must not be cloned by _dup.
        backend->maxRead = MAXDWORD;
        if (!backend->pipes.Create(false, 64, &reader, &writer, &error)) return false;
        const BYTE lookahead[] = { '\r', 'X', 'Y' };
        if (!backend->pipes.Write(writer, lookahead, sizeof(lookahead), &done, &error) || !backend->pipes.Close(writer, &error)) return false;
        readFd = files.Adopt(reader, _O_RDONLY | _O_TEXT);
        char byte;
        if (files.Read(readFd, &byte, 1) != 1 || byte != '\r') return false;
        const int duplicate = files.Duplicate(readFd);
        if (duplicate < 3 || files.Read(duplicate, &byte, 1) != 1 || byte != 'Y' ||
            files.Read(readFd, &byte, 1) != 1 || byte != 'X' ||
            files.Read(duplicate, &byte, 1) || files.Close(duplicate) || files.Close(readFd)) return false;

        // A writer whose final reader closed reports EPIPE through stdio.
        if (!backend->pipes.Create(false, 64, &reader, &writer, &error) || !backend->pipes.Close(reader, &error)) return false;
        writeFd = files.Adopt(writer, _O_WRONLY | _O_TEXT);
        stream = io.Attach(writeFd, L"wt", false);
        if (!stream || io.SetBuffer(stream, nullptr, 4, 0) || io.Write("x", 1, 1, stream) ||
            !io.Error(stream) || io.Eof(stream) || errno != EPIPE) return false;
        io.ClearError(stream);
        return io.Close(stream) == 0;
    }
}
