#pragma once
#include "CrtFileFixtures.h"
#include "Bridge/CRT/CrtStdio.h"

namespace
{
    bool ValidateCrtStdioModeFixtures()
    {
        using namespace Win32Bridge::Bridge;
        CrtStdioMode mode;
        if (!ParseCrtStdioMode(L" rb+ ", false, &mode) || mode.flags != (_O_RDWR | _O_BINARY)) return false;
        if (!ParseCrtStdioMode(L"w+x, ccs = utf-8", false, &mode) ||
            mode.flags != (_O_RDWR | _O_CREAT | _O_TRUNC | _O_EXCL | _O_U8TEXT)) return false;
        if (!ParseCrtStdioMode(L"aTNSc", false, &mode) || !mode.commit ||
            mode.flags != (_O_WRONLY | _O_CREAT | _O_APPEND | _O_SHORT_LIVED | _O_NOINHERIT | _O_SEQUENTIAL)) return false;
        if (!ParseCrtStdioMode(L"rn,ccs=UNICODE", true, &mode) || mode.commit || !(mode.flags & _O_WTEXT)) return false;
        const wchar_t* invalid[] = { L"", L"q", L"r++", L"rbt", L"rx", L"wxx", L"rSR", L"rTT", L"rcn", L"r,ccs=BAD", L"r,CCS=UTF-8", L"r,ccs=UTF-8junk" };
        for (auto text : invalid) if (ParseCrtStdioMode(text, false, &mode)) return false;
        return !ParseCrtStdioMode(nullptr, false, &mode);
    }
    bool ValidateCrtStdioFixtures()
    {
        using namespace Win32Bridge::Bridge;
        auto backend = std::make_shared<CrtMemoryFileBackend>();
        CrtFileDescriptors files(backend); CrtStdio io(files);
        if (io.Standard(0) != io.Iob() || io.Iob()[0].descriptor != 0 || io.Iob()[1].descriptor != 1 ||
            io.Iob()[2].descriptor != 2 || io.Standard(3) || io.Iob()[3].flags) return false;
        void* stream = io.Open(L"stdio-binary", L"w+b", _SH_DENYNO, _O_TEXT, 0, false);
        if (!stream || stream != &io.Iob()[3]) return false;
        const unsigned char payload[] = { 0, 'a', '\n', 0xff };
        if (io.Write(payload, 2, 2, stream) != 2 || !backend->Bytes(L"stdio-binary").empty() || io.Tell(stream) != 4) return false;
        if (io.Flush(stream) || backend->Bytes(L"stdio-binary") != std::string(reinterpret_cast<const char*>(payload), sizeof(payload))) return false;
        if (io.Seek(stream, 0, FILE_BEGIN)) return false;
        unsigned char bytes[16]{};
        if (io.Read(bytes, 3, 2, stream) != 1 || memcmp(bytes, payload, sizeof(payload)) || !io.Eof(stream) || io.Error(stream)) return false;
        if (io.GetByte(stream) != -1 || io.UngetByte('z', stream) != 'z' || io.Eof(stream) || io.Tell(stream) != 3 ||
            io.GetByte(stream) != 'z' || io.GetByte(stream) != -1) return false;
        io.ClearError(stream);
        if (io.Eof(stream) || io.Error(stream) || io.Seek(stream, 0, FILE_BEGIN)) return false;
        io.Lock(stream);
        const int first = io.GetByte(stream); io.Unlock(stream);
        if (first != 0 || io.Close(stream)) return false;
        if (io.Close(stream) != -1 || errno != EINVAL) return false;
        if (io.Read(bytes, 0, 8, nullptr) || io.Write(bytes, 2, (std::numeric_limits<size_t>::max)(), stream) != 0) return false;

        backend->Put(L"lines", "one\r\ntwo\nend");
        stream = io.Open(L"lines", L"rt", _SH_DENYNO, _O_BINARY, 0, false);
        char line[16]{};
        if (!stream || !io.GetLine(line, sizeof(line), stream) || std::strcmp(line, "one\n") || io.Tell(stream) != 5 ||
            io.GetByte(stream) != 't' || io.UngetByte('T', stream) != 'T' || !io.GetLine(line, sizeof(line), stream) || std::strcmp(line, "Two\n")) return false;
        if (!io.GetLine(line, sizeof(line), stream) || std::strcmp(line, "end") || !io.Eof(stream) || io.GetLine(line, sizeof(line), stream)) return false;
        io.Rewind(stream);
        if (io.Eof(stream) || io.Error(stream) || !io.GetLine(line, 1, stream) || line[0] || io.Tell(stream)) return false;
        if (io.Write("x", 1, 1, stream) != 0 || !io.Error(stream) || errno != EBADF || !(static_cast<CrtLegacyFile*>(stream)->flags & 0x20)) return false;
        io.ClearError(stream); if (io.Error(stream) || io.Close(stream)) return false;

        stream = io.Open(L"buffer", L"wb", _SH_DENYNO, _O_TEXT, 0, false);
        char userBuffer[8]{};
        if (!stream || io.SetBuffer(stream, userBuffer, 0, sizeof(userBuffer)) || io.Write("ab", 1, 2, stream) != 2 ||
            memcmp(userBuffer, "ab", 2) || !backend->Bytes(L"buffer").empty() || io.Tell(stream) != 2) return false;
        if (io.SetBuffer(stream, nullptr, 4, 0) || io.Write("c", 1, 1, stream) != 1 || backend->Bytes(L"buffer") != "abc" || io.Close(stream)) return false;
        stream = io.Open(L"line-buffer", L"wt", _SH_DENYNO, _O_BINARY, 0, false);
        if (!stream || io.SetBuffer(stream, nullptr, 0x40, 8) || io.Write("x\n", 1, 2, stream) != 2 || backend->Bytes(L"line-buffer") != "x\r\n" || io.Close(stream)) return false;

        stream = io.Open(L"retry", L"wb", _SH_DENYNO, _O_TEXT, 0, false);
        if (!stream || io.Write("ab", 1, 2, stream) != 2) return false;
        backend->maxWrite = 1; backend->writesUntilFailure = 1;
        if (io.Flush(stream) != -1 || !io.Error(stream) || errno != ENOSPC || backend->Bytes(L"retry") != "a") return false;
        backend->writesUntilFailure = -1; io.ClearError(stream);
        if (io.Flush(stream) || backend->Bytes(L"retry") != "ab" || io.Error(stream) || io.Close(stream)) return false;
        backend->maxWrite = 0;

        stream = io.Open(L"full-buffer", L"wb", _SH_DENYNO, _O_TEXT, 0, false);
        if (!stream || io.SetBuffer(stream, userBuffer, 0, sizeof(userBuffer)) || io.PutString("ab", stream)) return false;
        backend->writesUntilFailure = 0;
        if (io.Write("123456", 1, 6, stream) != 6 || io.Error(stream) || !backend->Bytes(L"full-buffer").empty() ||
            io.PutByte('Z', stream) != -1 || !io.Error(stream) || errno != ENOSPC) return false;
        backend->writesUntilFailure = -1;
        if (io.Flush(stream) || io.PutByte('Z', stream) != 'Z' || io.Close(stream) ||
            backend->Bytes(L"full-buffer") != "ab123456Z") return false;

        stream = io.Open(L"partial-direct", L"wb", _SH_DENYNO, _O_TEXT, 0, false);
        if (!stream || io.SetBuffer(stream, nullptr, 4, 0)) return false;
        backend->writesUntilFailure = 0;
        backend->bytesWrittenOnFailure = 3;
        if (io.Write("abcdef", 2, 3, stream) != 1 || !io.Error(stream) || errno != ENOSPC ||
            backend->Bytes(L"partial-direct") != "abc") return false;
        backend->writesUntilFailure = -1;
        backend->bytesWrittenOnFailure = 0;
        if (io.Close(stream)) return false;

        stream = io.Open(L"line-retry", L"wb", _SH_DENYNO, _O_TEXT, 0, false);
        if (!stream || io.SetBuffer(stream, nullptr, 0x40, 8) || io.PutString("old", stream)) return false;
        backend->writesUntilFailure = 0;
        backend->bytesWrittenOnFailure = 4;
        if (io.Write("new\nZ", 1, 5, stream) != 1 || !io.Error(stream) || errno != ENOSPC ||
            backend->Bytes(L"line-retry") != "oldn") return false;
        backend->writesUntilFailure = -1;
        backend->bytesWrittenOnFailure = 0;
        if (io.Write("ew\nZ", 1, 4, stream) != 4 || backend->Bytes(L"line-retry") != "oldnew\n" ||
            io.Close(stream) || backend->Bytes(L"line-retry") != "oldnew\nZ") return false;

        stream = io.Open(L"line-crlf-retry", L"wt", _SH_DENYNO, _O_TEXT, 0, false);
        if (!stream || io.SetBuffer(stream, nullptr, 0x40, 8) || io.PutString("A", stream)) return false;
        backend->maxWrite = 2;
        backend->writesUntilFailure = 1;
        if (io.PutByte('\n', stream) != -1 || !io.Error(stream) || errno != ENOSPC ||
            backend->Bytes(L"line-crlf-retry") != "A\r") return false;
        backend->maxWrite = 0;
        backend->writesUntilFailure = -1;
        if (io.PutByte('\n', stream) != '\n' || io.Close(stream) || backend->Bytes(L"line-crlf-retry") != "A\r\n") return false;

        const auto beforeCommit = backend->flushed;
        stream = io.Open(L"commit", L"wc", _SH_DENYNO, _O_TEXT, 0, false);
        if (!stream || io.MaximumStreams() != 512 || io.PutString("saved", stream) || io.Flush(stream) ||
            backend->flushed != beforeCommit + 1 || io.FlushAll(true) != 4 || io.CloseAll() != 1 || backend->Bytes(L"commit") != "saved") return false;
        backend->Put(L"attached", "abc");
        int descriptor = files.Open(L"attached", _O_RDWR | _O_BINARY, _SH_DENYNO, 0600, _O_TEXT);
        stream = io.Attach(descriptor, L"w", false);
        if (!stream || io.Write("Z", 1, 1, stream) != 1 || io.Close(stream) || backend->Bytes(L"attached") != "Zbc" || files.Handle(descriptor) != -1) return false;
        stream = io.Reopen(L"stdout", L"wb", io.Standard(1), _O_TEXT, 0, false);
        if (stream != io.Standard(1) || io.PutString("redirected", stream) || backend->Bytes(L"stdout") != "redirected" || io.Close(stream)) return false;

        if (io.SetMaximumStreams(3) != 3 || io.Open(L"limit", L"wb", _SH_DENYNO, _O_TEXT, 0, false) || errno != EMFILE) return false;
        if (io.SetMaximumStreams(4) != 4) return false;
        stream = io.Open(L"limit", L"wb", _SH_DENYNO, _O_TEXT, 0, false);
        if (!stream || io.SetMaximumStreams(3) != -1 || errno != EINVAL || io.Close(stream) || io.SetMaximumStreams(3) != 3 ||
            io.SetMaximumStreams(8193) != -1 || io.SetMaximumStreams(512) != 512) return false;
        stream = io.Open(L"reopen-from", L"wb", _SH_DENYNO, _O_TEXT, 0, false);
        if (!stream || io.Write("first", 1, 5, stream) != 5 || io.Reopen(L"reopen-to", L"wb", stream, _O_TEXT, 0, false) != stream ||
            backend->Bytes(L"reopen-from") != "first" || io.PutString("second", stream) || io.Close(stream) || backend->Bytes(L"reopen-to") != "second") return false;

        stream = io.Open(L"discarded", L"wb", _SH_DENYNO, _O_TEXT, 0, false);
        if (!stream || io.PutString("pending", stream)) return false;
        io.CloseAll(true, false);
        return backend->Bytes(L"discarded").empty();
    }
    void __cdecl CrtAnsiWideLocaleFixture(void* context)
    {
        using namespace Win32Bridge::Bridge;
        auto& valid = *static_cast<bool*>(context);
        auto backend = std::make_shared<CrtMemoryFileBackend>();
        CrtFileDescriptors files(backend);
        CrtStdio io(files);
        if (!SetCrtLocale(LC_CTYPE, "C")) return;
        void* stream = io.Open(L"ansi-C", L"wt", _SH_DENYNO, _O_TEXT, 0, false);
        if (!stream || io.PutWide(0xe9, stream) != 0xe9) return;
        errno = 0;
        if (io.PutWideString(L"A\u3042Z", stream) != -1 || errno != EILSEQ || io.Error(stream) ||
            io.Close(stream) || backend->Bytes(L"ansi-C") != "\xe9" "A") return;
        stream = io.Open(L"ansi-C", L"rt", _SH_DENYNO, _O_TEXT, 0, false);
        if (!stream || io.GetWide(stream) != 0xe9 || io.GetWide(stream) != L'A' || io.Close(stream)) return;

        if (!SetCrtLocale(LC_CTYPE, "Japanese_Japan.932") || SetCrtMultibyteCodePage(0) != 0) return;
        stream = io.Open(L"ansi-JP", L"wt", _SH_DENYNO, _O_TEXT, 0, false);
        if (!stream || io.PutWideString(L"\u3042\n", stream) || io.Close(stream) ||
            backend->Bytes(L"ansi-JP") != "\x82\xa0\r\n") return;
        stream = io.Open(L"ansi-JP", L"rt", _SH_DENYNO, _O_TEXT, 0, false);
        if (!stream || io.GetWide(stream) != 0x3042 || io.GetWide(stream) != L'\n' || io.Close(stream)) return;
        backend->Put(L"truncated-JP", "\x82");
        stream = io.Open(L"truncated-JP", L"rt", _SH_DENYNO, _O_TEXT, 0, false);
        if (!stream || io.GetWide(stream) != 0xffff || io.Eof(stream) || io.Error(stream) ||
            io.GetByte(stream) != 0x82 || io.Close(stream)) return;

        if (!SetCrtLocale(LC_CTYPE, ".UTF-8")) return;
        stream = io.Open(L"ansi-u8", L"wt", _SH_DENYNO, _O_TEXT, 0, false);
        if (!stream || io.PutWideString(L"\u3042\xd83d\xde42\n", stream) || io.Close(stream) ||
            backend->Bytes(L"ansi-u8") != "\xe3\x81\x82\xf0\x9f\x99\x82\r\n") return;
        stream = io.Open(L"ansi-u8", L"rt", _SH_DENYNO, _O_TEXT, 0, false);
        if (!stream || io.GetWide(stream) != 0x3042 || io.GetWide(stream) != 0xd83d ||
            io.GetWide(stream) != 0xde42 || io.GetWide(stream) != L'\n' || io.Close(stream)) return;
        valid = true;
    }

    bool ValidateCrtWideStdioFixtures()
    {
        using namespace Win32Bridge::Bridge;
        auto backend = std::make_shared<CrtMemoryFileBackend>();
        CrtFileDescriptors files(backend); CrtStdio io(files);
        const std::string bom("\xef\xbb\xbf", 3);
        const std::wstring message = L"A\u00e9\xd83d\xde42\n";
        void* stream = io.Open(L"stdio-unicode", L"w, ccs=UTF-8", _SH_DENYNO, _O_TEXT, 0, false);
        if (!stream || io.PutWideString(message.c_str(), stream) || io.Tell(stream) != 12 ||
            backend->Bytes(L"stdio-unicode") != bom || io.Close(stream) || backend->Bytes(L"stdio-unicode") != bom + "A\xc3\xa9\xf0\x9f\x99\x82\r\n") return false;
        stream = io.Open(L"stdio-unicode", L"r,ccs=UNICODE", _SH_DENYNO, _O_TEXT, 0, false);
        wchar_t wide[16]{};
        if (!stream || !io.GetWideLine(wide, 16, stream) || std::wstring(wide) != message || io.Error(stream) ||
            io.GetWide(stream) != 0xffff || !io.Eof(stream)) return false;
        if (io.UngetWide(L'Z', stream) != L'Z' || io.Eof(stream) || io.GetWide(stream) != L'Z' || io.Close(stream)) return false;
        stream = io.Open(L"wide-buffer-boundary", L"w, ccs=UTF-8", _SH_DENYNO, _O_TEXT, 0, false);
        if (!stream || io.SetBuffer(stream, nullptr, 0, 6) || io.PutWideString(L"AB", stream) ||
            io.PutWideString(L"\xd83d\xde42!", stream) || io.Close(stream) ||
            backend->Bytes(L"wide-buffer-boundary") != bom + "AB\xf0\x9f\x99\x82!") return false;
        stream = io.Open(L"wide-line-boundary", L"w, ccs=UTF-8", _SH_DENYNO, _O_TEXT, 0, false);
        if (!stream || io.SetBuffer(stream, nullptr, 0x40, 8) || io.PutWideString(L"\u010a", stream) ||
            backend->Bytes(L"wide-line-boundary") != bom || io.PutWideString(L"\nX", stream) ||
            backend->Bytes(L"wide-line-boundary") != bom + "\xc4\x8a\r\n" || io.Close(stream) ||
            backend->Bytes(L"wide-line-boundary") != bom + "\xc4\x8a\r\nX") return false;
        stream = io.Open(L"wide-partial", L"w, ccs=UTF-8", _SH_DENYNO, _O_TEXT, 0, false);
        if (!stream || io.SetBuffer(stream, nullptr, 4, 0)) return false;
        backend->writesUntilFailure = 0;
        backend->bytesWrittenOnFailure = 5;
        const wchar_t unicodeText[] = L"A\xd83d\xde42Z";
        if (io.Write(unicodeText, sizeof(wchar_t), 4, stream) != 3 || !io.Error(stream) || errno != ENOSPC ||
            backend->Bytes(L"wide-partial") != bom + "A\xf0\x9f\x99\x82") return false;
        backend->writesUntilFailure = -1;
        backend->bytesWrittenOnFailure = 0;
        if (io.PutWide(L'Z', stream) != L'Z' || io.Close(stream) ||
            backend->Bytes(L"wide-partial") != bom + "A\xf0\x9f\x99\x82Z") return false;
        stream = io.Open(L"wide-binary", L"wb", _SH_DENYNO, _O_TEXT, 0, false);
        if (!stream || io.PutWideString(message.c_str(), stream) || io.Close(stream) || backend->Bytes(L"wide-binary") != CrtUtf16Bytes(message)) return false;
        stream = io.Open(L"wide-ansi", L"wt", _SH_DENYNO, _O_BINARY, 0, false);
        if (!stream || io.PutWideString(L"a\nb", stream) || io.Close(stream) || backend->Bytes(L"wide-ansi") != "a\r\nb") return false;
        stream = io.Open(L"text-position", L"w+t", _SH_DENYNO, _O_TEXT, 0, false);
        if (!stream || io.Write("x\n", 1, 2, stream) != 2 || io.Tell(stream) != 3 || io.Seek(stream, -1, FILE_CURRENT) ||
            io.GetByte(stream) != '\n' || io.Close(stream)) return false;
        CrtExceptionProcessState process;
        CrtThreadExceptionState thread;
        thread.process = &process;
        const auto previous = SetCrtThreadExceptionState(&thread);
        bool valid = false;
        const bool completed = InvokeCrtExecution(&CrtAnsiWideLocaleFixture, &valid);
        SetCrtThreadExceptionState(previous);
        return completed && valid;
    }
}