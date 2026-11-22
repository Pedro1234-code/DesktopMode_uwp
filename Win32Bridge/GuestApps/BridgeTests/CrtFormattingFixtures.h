#pragma once
#include <locale.h>
#include "Bridge/CRT/CrtFormatting.h"
#include "CrtStdioFixtures.h"

namespace
{
    template<class Character> int FormatBufferFixture(unsigned __int64 options, Win32Bridge::Bridge::CrtFormatFlavor flavor,
        Character* buffer, size_t count, size_t maximum, const Character* format, ...)
    {
        va_list args; va_start(args, format);
        const int result = Win32Bridge::Bridge::FormatCrtBuffer(options, buffer, count, maximum, format, nullptr, args, flavor);
        va_end(args); return result;
    }
    template<class Character> int FormatTextFixture(std::vector<Character>* output, unsigned __int64 options,
        Win32Bridge::Bridge::CrtFormatFlavor flavor, const Character* format, ...)
    {
        va_list args; va_start(args, format);
        const int result = Win32Bridge::Bridge::FormatCrtText(options, format, nullptr, args, flavor, output);
        va_end(args); return result;
    }
    int FormatLocaleFixture(char* buffer, size_t count, void* locale, const char* format, ...)
    {
        va_list args; va_start(args, format);
        const int result = Win32Bridge::Bridge::FormatCrtBuffer(0, buffer, count, 0, format, locale, args, Win32Bridge::Bridge::CrtFormatFlavor::Standard);
        va_end(args); return result;
    }
    int g_hostFormatValidationCalls = 0;
    void __cdecl HostFormatValidationFixture(const wchar_t*, const wchar_t*, const wchar_t*, unsigned, uintptr_t)
    { ++g_hostFormatValidationCalls; }

    bool ValidateCrtFormattingFixtures()
    {
        using namespace Win32Bridge::Bridge;
        constexpr auto standard = CrtFormatFlavor::Standard;
        constexpr unsigned __int64 c99 = _CRT_INTERNAL_PRINTF_STANDARD_SNPRINTF_BEHAVIOR;
        constexpr unsigned __int64 legacyTermination = _CRT_INTERNAL_PRINTF_LEGACY_VSPRINTF_NULL_TERMINATION;
        char buffer[128]{}; wchar_t wide[128]{};
        if (FormatBufferFixture(c99, standard, buffer, sizeof(buffer), 0, "%04d:%.*s", 7, 3, "abcdef") != 8 || std::strcmp(buffer, "0007:abc")) return false;
        if (FormatBufferFixture(CrtNamedMsvcrtFormatOptions, standard, buffer, sizeof(buffer), 0, "%I64d", -1234567890123456ll) != 17 || std::strcmp(buffer, "-1234567890123456")) return false;
        memset(buffer, 'X', sizeof(buffer));
        if (FormatBufferFixture(c99, standard, buffer, 3, 0, "%s", "abcd") != 4 || memcmp(buffer, "ab\0X", 4)) return false;
        memset(buffer, 'X', sizeof(buffer));
        if (FormatBufferFixture(legacyTermination, standard, buffer, 3, 0, "%s", "abc") != 3 || memcmp(buffer, "abcX", 4)) return false;
        if (FormatBufferFixture(0, standard, buffer, 3, 0, "%s", "abc") != -2 || memcmp(buffer, "ab\0", 3)) return false;
        if (FormatBufferFixture(c99, standard, static_cast<char*>(nullptr), 0, 0, "%d:%s", 10, "value") != 8) return false;
        if (FormatBufferFixture(0, CrtFormatFlavor::Positional, buffer, sizeof(buffer), 0, "%2$d:%1$s", "name", 7) != 6 || std::strcmp(buffer, "7:name")) return false;
        if (FormatBufferFixture(0, standard, wide, 128, 0, L"%s/%ls", "n", L"w") != 3 || std::wcscmp(wide, L"n/w")) return false;
        if (FormatBufferFixture(CrtNamedUcrtFormatOptions, standard, wide, 128, 0, L"%s/%hs", L"w", "n") != 3 || std::wcscmp(wide, L"w/n")) return false;
        if (FormatBufferFixture(CrtNamedMsvcrtFormatOptions, standard, buffer, sizeof(buffer), 0, "%.1e", 1.0) < 0 || !std::strstr(buffer, "e+000")) return false;

        std::vector<char> text;
        if (FormatTextFixture(&text, 0, standard, "%d:%d", 10, 20) != 5 || std::strcmp(text.data(), "10:20")) return false;
        if (FormatTextFixture(&text, 0, CrtFormatFlavor::Positional, "%2$d:%1$s", "name", 7) != 6 || std::strcmp(text.data(), "7:name")) return false;
        if (FormatTextFixture(&text, 0, standard, "A%cB", 0) != 3 || text.size() != 4 || memcmp(text.data(), "A\0B\0", 4)) return false;
        std::vector<wchar_t> wideText;
        if (FormatTextFixture(&wideText, 0, standard, L"A%cB", 0) != 3 || wideText.size() != 4 ||
            wideText[0] != L'A' || wideText[1] || wideText[2] != L'B' || wideText[3]) return false;

        if (CrtFormatContainsCountWrite("%%n %lsn") || !CrtFormatContainsCountWrite("%I64n") ||
            !CrtFormatContainsCountWrite(L"%1$hn") || CrtFormatContainsCountWrite(L"%Zn")) return false;
        int countWrite = 123;
        if (FormatBufferFixture(c99, standard, buffer, sizeof(buffer), 0, "%n", &countWrite) != -1 || errno != EINVAL || countWrite != 123) return false;
        const auto explicitLocale=::_create_locale(LC_ALL,"C");if(!explicitLocale)return false;
        const int localeResult=FormatLocaleFixture(buffer,sizeof(buffer),explicitLocale,"%d",7);::_free_locale(explicitLocale);
        if(localeResult!=1||std::strcmp(buffer,"7"))return false;

        const auto previousHandler = ::_set_thread_local_invalid_parameter_handler(&HostFormatValidationFixture);
        g_hostFormatValidationCalls = 0;
        const int secureResult = FormatBufferFixture(0, CrtFormatFlavor::Secure, buffer, 3, 0, "%s", "abcd");
        const bool secureCorrect = secureResult == -1 && !buffer[0] && errno == ERANGE &&
            ::_get_thread_local_invalid_parameter_handler() == &HostFormatValidationFixture;
        const int invalidResult = FormatBufferFixture(0, CrtFormatFlavor::Secure, buffer, sizeof(buffer), 0, static_cast<const char*>(nullptr));
        const bool invalidCorrect = invalidResult == -1 && errno == EINVAL &&
            ::_get_thread_local_invalid_parameter_handler() == &HostFormatValidationFixture && g_hostFormatValidationCalls == 0;
        ::_set_thread_local_invalid_parameter_handler(previousHandler);
        if (!secureCorrect || !invalidCorrect) return false;
        if (FormatBufferFixture(0, CrtFormatFlavor::SecureTruncate, buffer, 3, (std::numeric_limits<size_t>::max)(), "%s", "abcd") != -1 || std::strcmp(buffer, "ab")) return false;
        return true;
    }
    bool ValidateCrtFormattedStreamFixtures()
    {
        using namespace Win32Bridge::Bridge;
        auto backend = std::make_shared<CrtMemoryFileBackend>(); CrtFileDescriptors files(backend); CrtStdio io(files);
        std::vector<char> narrow; std::vector<wchar_t> wide;
        if (FormatTextFixture(&narrow, 0, CrtFormatFlavor::Standard, "%d:%s%cX\n", 7, "data", 0) != 9) return false;
        void* stream = io.Open(L"formatted", L"wb", _SH_DENYNO, _O_TEXT, 0, false);
        if (!stream || io.Write(narrow.data(), 1, 9, stream) != 9 || io.Close(stream) || backend->Bytes(L"formatted") != std::string("7:data\0X\n", 9)) return false;
        if (FormatTextFixture(&wide, 0, CrtFormatFlavor::Standard, L"A%cB", 0) != 3) return false;
        stream = io.Open(L"formatted-wide", L"wb", _SH_DENYNO, _O_TEXT, 0, false);
        if (!stream || io.WriteWideText(wide.data(), 3, stream) || io.Close(stream) ||
            backend->Bytes(L"formatted-wide") != CrtUtf16Bytes(std::wstring(wide.data(), 3))) return false;
        stream = io.Open(L"formatted-ansi-wide", L"wt", _SH_DENYNO, _O_BINARY, 0, false);
        if (!stream || io.WriteWideText(wide.data(), 3, stream) || io.Close(stream) ||
            backend->Bytes(L"formatted-ansi-wide") != std::string("A\0B", 3)) return false;
        return true;
    }
}