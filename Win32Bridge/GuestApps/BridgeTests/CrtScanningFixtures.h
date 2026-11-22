#pragma once
#include "Bridge/CRT/CrtScanning.h"
#include "Bridge/CRT/CrtStdioScanInput.h"
#include "CrtStdioFixtures.h"
#include <type_traits>

namespace
{
    template<class Character> int ScanBufferFixture(unsigned __int64 options, const Character* text, size_t count, const Character* format, ...)
    {
        va_list args; va_start(args, format);
        const int result = Win32Bridge::Bridge::ScanCrtBuffer(options, text, count, format, nullptr, args);
        va_end(args); return result;
    }
    template<class Character> int ScanInputFixture(unsigned __int64 options, Win32Bridge::Bridge::CrtScanInput& input, const Character* format, ...)
    {
        va_list args; va_start(args, format);
        const int result = Win32Bridge::Bridge::ScanCrtStream(options, input, format, nullptr, args);
        va_end(args); return result;
    }
    template<class Character> int ScanInputLocaleFixture(unsigned __int64 options, Win32Bridge::Bridge::CrtScanInput& input,
        _locale_t locale, const Character* format, ...)
    {
        va_list args; va_start(args, format);
        const int result = Win32Bridge::Bridge::ScanCrtStream(options, input, format, locale, args);
        va_end(args); return result;
    }
    template<class Character> class ScanMemoryFixture final : public Win32Bridge::Bridge::CrtScanInput
    {
        std::basic_string<Character> text;
    public:
        size_t cursor = 0;
        bool validPushback = true;
        explicit ScanMemoryFixture(std::basic_string<Character> value) : text(std::move(value)) { }
        int Get() override
        { return cursor < text.size() ? static_cast<typename std::make_unsigned<Character>::type>(text[cursor++]) : -1; }
        void Unget(int c) override
        {
            if (!cursor || static_cast<unsigned>(static_cast<typename std::make_unsigned<Character>::type>(text[cursor - 1])) != static_cast<unsigned>(c)) validPushback = false;
            else --cursor;
        }
    };
    int g_hostScanValidationCalls = 0;
    void __cdecl HostScanValidationFixture(const wchar_t*, const wchar_t*, const wchar_t*, unsigned, uintptr_t)
    { ++g_hostScanValidationCalls; }

    bool ValidateCrtScanningFixtures()
    {
        using namespace Win32Bridge::Bridge;
        const size_t unlimited = (std::numeric_limits<size_t>::max)();
        int number = 0, count = -1; double real = 0; char text[32]{}; wchar_t wide[32]{};
        if (ScanBufferFixture(0, "0x2a 3.5", unlimited, "%i %lf%n", &number, &real, &count) != 2 || number != 42 || real != 3.5 || count != 8) return false;
        if (ScanBufferFixture(0, "1234", 2, "%d%n", &number, &count) != 1 || number != 12 || count != 2) return false;
        if (ScanBufferFixture(0, "12 abc", unlimited, "%*d %2s%n", text, &count) != 1 || strcmp(text, "ab") || count != 5) return false;
        if (ScanBufferFixture(0, L"texto", unlimited, L"%ls%n", wide, &count) != 1 || wcscmp(wide, L"texto") || count != 5) return false;
        if (ScanBufferFixture(CrtNamedUcrtScanOptions, L"wide", unlimited, L"%s", wide) != 1 || wcscmp(wide, L"wide")) return false;
        if (ScanBufferFixture(0, L"narrow", unlimited, L"%s", text) != 1 || strcmp(text, "narrow")) return false;
        if (ScanBufferFixture(0, "", unlimited, "%n", &count) != 0 || count) return false;
        number = 777;
        if (ScanBufferFixture(0, "x", unlimited, "%d", &number) != 0 || number != 777 || ScanBufferFixture(0, "", unlimited, "%d", &number) != -1) return false;
        if (ScanBufferFixture(_CRT_INTERNAL_SCANF_SECURECRT, "abc", unlimited, "%3s%n", text, 4u, &count) != 1 || strcmp(text, "abc") || count != 3) return false;
        const auto previousHandler = ::_set_thread_local_invalid_parameter_handler(&HostScanValidationFixture);
        g_hostScanValidationCalls = 0;
        const int invalid = ScanBufferFixture(_CRT_INTERNAL_SCANF_SECURECRT, "abc", unlimited, "%s", static_cast<char*>(nullptr), 4u);
        const bool handlerSafe = invalid <= 0 && errno == EINVAL && !g_hostScanValidationCalls &&
            ::_get_thread_local_invalid_parameter_handler() == &HostScanValidationFixture;
        ::_set_thread_local_invalid_parameter_handler(previousHandler);
        return handlerSafe;
    }
    bool ValidateCrtStreamScanningFixtures()
    {
        using namespace Win32Bridge::Bridge;
        int number = 0, count = -1; double real = 0; char text[32]{}; wchar_t wide[32]{};
        ScanMemoryFixture<char> basic("42 tail");
        if (ScanInputFixture(0, basic, "%d%n", &number, &count) != 1 || number != 42 || count != 2 || basic.cursor != 2 || !basic.validPushback) return false;
        for (const char* invalid : { "+x", "0xZ" })
        {
            ScanMemoryFixture<char> input(invalid);
            if (ScanInputFixture(0, input, "%i", &number) != 0 || input.cursor != strlen(invalid) - 1 || !input.validPushback) return false;
        }
        for (const char* invalid : { "1e+X", ".e2", "infiZ" })
        {
            ScanMemoryFixture<char> input(invalid);
            const size_t expected = invalid[0] == '.' ? 1 : strlen(invalid) - 1;
            if (ScanInputFixture(0, input, "%lf", &real) != 0 || input.cursor != expected || !input.validPushback) return false;
        }
        ScanMemoryFixture<char> embedded(std::string("ab\0cZ", 5));
        if (ScanInputFixture(0, embedded, "%4c%n", text, &count) != 1 || memcmp(text, "ab\0c", 4) || count != 4 || embedded.cursor != 4) return false;
        ScanMemoryFixture<char> nulNumber(std::string("12\0X", 4));
        if (ScanInputFixture(0, nulNumber, "%d%n", &number, &count) != 1 || number != 12 || count != 3 || nulNumber.cursor != 3) return false;
        ScanMemoryFixture<char> secure("abcd!");
        if (ScanInputFixture(_CRT_INTERNAL_SCANF_SECURECRT, secure, "%s", text, 3u) != 0 || errno != ENOMEM || *text || secure.cursor != 3) return false;
        ScanMemoryFixture<char> zero("abc");
        if (ScanInputFixture(_CRT_INTERNAL_SCANF_SECURECRT, zero, "%s", text, 0u) != 0 || errno != ENOMEM || zero.cursor) return false;
        ScanMemoryFixture<char> legacyZero("abc");
        if (ScanInputFixture(_CRT_INTERNAL_SCANF_SECURECRT | CrtNamedMsvcrtScanOptions, legacyZero, "%s", text, 0u) != 0 || errno != ENOMEM || *text || legacyZero.cursor != 1) return false;
        ScanMemoryFixture<char> overflowAtEof("abc");
        text[3] = 'Q';
        if (ScanInputFixture(_CRT_INTERNAL_SCANF_SECURECRT, overflowAtEof, "%s", text, 3u) != -1 ||
            errno != ENOMEM || *text || text[3] != 'Q' || overflowAtEof.cursor != 3) return false;
        ScanMemoryFixture<char> normalBadFormat("12 X"), secureBadFormat("12 X");
        errno = 0;
        if (ScanInputFixture(0, normalBadFormat, "%d%q", &number) != 1 || number != 12 || errno || normalBadFormat.cursor != 2) return false;
        if (ScanInputFixture(_CRT_INTERNAL_SCANF_SECURECRT, secureBadFormat, "%d%q", &number) != 1 || errno != EINVAL || secureBadFormat.cursor != 2) return false;
        ScanMemoryFixture<char> wideLimit("abc X");
        errno = 0;
        if (ScanInputFixture(0, wideLimit, "%18446744073709551616s%n", text, &count) != 1 || strcmp(text, "abc") ||
            errno != ERANGE || count != 3 || wideLimit.cursor != 3) return false;
        ScanMemoryFixture<wchar_t> unicodeWidth(L"123 X");
        if (ScanInputFixture(0, unicodeWidth, L"%\u0662d%n", &number, &count) != 1 || number != 12 || count != 2 || unicodeWidth.cursor != 2) return false;
        ScanMemoryFixture<wchar_t> unicodeSpacing(L"\u3000A\u3000B X");
        if (ScanInputFixture(0, unicodeSpacing, L"%ls%n", wide, &count) != 1 || wcscmp(wide, L"A\u3000B") || count != 4 || unicodeSpacing.cursor != 4) return false;
        const auto dbcs = ::_wcreate_locale(LC_CTYPE, L"Japanese_Japan.932");
        if (!dbcs) return false;
        ScanMemoryFixture<char> malformedDbcs("\x82\xa0X"), mismatchedDbcs("\x82" "BX");
        errno = 0;
        const bool correctFormatFailure = ScanInputLocaleFixture(_CRT_INTERNAL_SCANF_SECURECRT, malformedDbcs, dbcs, "\x82") == 0 &&
            errno == EILSEQ && !malformedDbcs.cursor && malformedDbcs.validPushback;
        errno = 0;
        const bool correctLiteralMismatch = ScanInputLocaleFixture(0, mismatchedDbcs, dbcs, "\x82\xa0") == 0 &&
            !errno && !mismatchedDbcs.cursor && mismatchedDbcs.validPushback;
        ::_free_locale(dbcs);
        if (!correctFormatFailure || !correctLiteralMismatch) return false;
        const auto utf8 = ::_wcreate_locale(LC_CTYPE, L".UTF-8");
        if (!utf8) return false;
        ScanMemoryFixture<wchar_t> utf8Overflow(L"\u3042 X");
        char bounded[3] = {'X', 'Y', 'Z'};
        const bool correctBounds = ScanInputLocaleFixture(_CRT_INTERNAL_SCANF_SECURECRT, utf8Overflow, utf8, L"%hs", bounded, 2u) == 0 &&
            errno == ERANGE && bounded[0] == 0 && bounded[1] == 'Y' && bounded[2] == 'Z' && utf8Overflow.cursor == 1;
        ::_free_locale(utf8);
        if (!correctBounds) return false;
        ScanMemoryFixture<char> ranges("abcd!");
        if (ScanInputFixture(0, ranges, "%[a-c-e]", text) != 1 || strcmp(text, "abc") || ranges.cursor != 3) return false;
        ScanMemoryFixture<char> descending("abc!");
        if (ScanInputFixture(0, descending, "%[c-a]", text) != 1 || strcmp(text, "abc") || descending.cursor != 3) return false;
        ScanMemoryFixture<char> strict("ab"), legacy("ab");
        if (ScanInputFixture(0, strict, "%3c", text) != -1 || memcmp(text, "ab", 2) ||
            ScanInputFixture(CrtNamedMsvcrtScanOptions, legacy, "%3c", text) != 1 || memcmp(text, "ab", 2)) return false;
        ScanMemoryFixture<char> empty("");
        if (ScanInputFixture(0, empty, "%n", &count) != 0 || count) return false;
        ScanMemoryFixture<char> suppressed("12 abc");
        if (ScanInputFixture(0, suppressed, "%*d %2s%n", text, &count) != 1 || strcmp(text, "ab") || count != 5 || suppressed.cursor != 5) return false;
        ScanMemoryFixture<wchar_t> wideInput(L"12 wide!");
        if (ScanInputFixture(CrtNamedUcrtScanOptions, wideInput, L"%d %4s%n", &number, wide, &count) != 2 || number != 12 || wcscmp(wide, L"wide") || count != 7) return false;
        ScanMemoryFixture<char> lengths("abc"); signed char tiny = -1; short small = -1; long long large = -1;
        if (ScanInputFixture(0, lengths, "%*2c%hhn%hn%lln", &tiny, &small, &large) != 0 || tiny != 2 || small != 2 || large != 2) return false;
        ScanMemoryFixture<char> percent("%7!");
        return ScanInputFixture(0, percent, "%%%d", &number) == 1 && number == 7 && percent.cursor == 2;
    }
    bool ValidateCrtScannedFileFixtures()
    {
        using namespace Win32Bridge::Bridge;
        auto backend = std::make_shared<CrtMemoryFileBackend>(); CrtFileDescriptors files(backend); CrtStdio io(files);
        backend->Put(L"scan-text", "12\r\n34 X");
        void* stream = io.Open(L"scan-text", L"rt", _SH_DENYNO, _O_BINARY, 0, false);
        if (!stream) return false;
        {
            CrtStdioScanInput<false> input(io, stream); int first = 0, second = 0, count = -1;
            if (ScanInputFixture(0, input, "%d %d%n", &first, &second, &count) != 2 || first != 12 || second != 34 || count != 5 || io.Tell(stream) != 6 || io.GetByte(stream) != ' ') return false;
        }
        if (io.Close(stream)) return false;
        backend->Put(L"scan-wide", CrtUtf16Bytes(L"17 word!"));
        stream = io.Open(L"scan-wide", L"rb", _SH_DENYNO, _O_TEXT, 0, false);
        if (!stream) return false;
        {
            CrtStdioScanInput<true> input(io, stream); int number = 0; wchar_t word[8]{};
            if (ScanInputFixture(CrtNamedUcrtScanOptions, input, L"%d %4s", &number, word) != 2 || number != 17 || wcscmp(word, L"word") || io.GetWide(stream) != L'!') return false;
        }
        return io.Close(stream) == 0;
    }
}
