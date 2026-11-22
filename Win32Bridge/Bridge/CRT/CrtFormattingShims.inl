// Included after the virtual stdio adapters. Buffer formatting uses UCRT;
// stream formatting always emits through CrtStdio.
namespace
{
    using Win32Bridge::Bridge::CrtFormatFlavor;
    enum class CrtBufferStyle { Terminated, Legacy, C99, Secure, Positional };
    template<bool Legacy> constexpr unsigned __int64 NamedFormatOptions()
    { return Legacy ? Win32Bridge::Bridge::CrtNamedMsvcrtFormatOptions : Win32Bridge::Bridge::CrtNamedUcrtFormatOptions; }
    int NormalizeFormattedResult(int result) { return result < 0 ? -1 : result; }

    template<class Character> int FormatGuestStream(unsigned __int64 options, FILE* stream, const Character* format,
        _locale_t locale, va_list arguments, CrtFormatFlavor flavor);
    template<> int FormatGuestStream<char>(unsigned __int64 options, FILE* stream, const char* format,
        _locale_t locale, va_list arguments, CrtFormatFlavor flavor)
    {
        auto io = CurrentCrtStdio(); if (!io || io->Descriptor(stream) < 0) return -1;
        std::vector<char> text;
        const int count = Win32Bridge::Bridge::FormatCrtText(options, format, locale, arguments, flavor, &text);
        if (count < 0) return -1;
        return io->Write(text.data(), 1, static_cast<size_t>(count), stream) == static_cast<size_t>(count) ? count : -1;
    }
    template<> int FormatGuestStream<wchar_t>(unsigned __int64 options, FILE* stream, const wchar_t* format,
        _locale_t locale, va_list arguments, CrtFormatFlavor flavor)
    {
        auto io = CurrentCrtStdio(); if (!io || io->Descriptor(stream) < 0) return -1;
        if (!locale) locale = Win32Bridge::Bridge::CurrentCrtLocaleHandle();
        std::vector<wchar_t> text;
        const int count = Win32Bridge::Bridge::FormatCrtText(options, format, locale, arguments, flavor, &text);
        if (count < 0) return -1;
        return io->WriteWideText(text.data(), static_cast<size_t>(count), stream, locale) == 0 ? count : -1;
    }
    template<class Character, CrtFormatFlavor Flavor> int __cdecl CommonFormatBuffer(unsigned __int64 options,
        Character* buffer, size_t count, const Character* format, _locale_t locale, va_list args)
    { return Win32Bridge::Bridge::FormatCrtBuffer(options, buffer, count, 0, format, locale, args, Flavor); }
    template<class Character> int __cdecl CommonFormatTruncate(unsigned __int64 options,
        Character* buffer, size_t count, size_t maximum, const Character* format, _locale_t locale, va_list args)
    { return Win32Bridge::Bridge::FormatCrtBuffer(options, buffer, count, maximum, format, locale, args, CrtFormatFlavor::SecureTruncate); }
    template<class Character, CrtFormatFlavor Flavor> int __cdecl CommonFormatStream(unsigned __int64 options,
        FILE* stream, const Character* format, _locale_t locale, va_list args)
    { return FormatGuestStream(options, stream, format, locale, args, Flavor); }

    template<bool Legacy, class Character> int __cdecl FormatVUnboundedL(Character* buffer, const Character* format, _locale_t locale, va_list args)
    { return NormalizeFormattedResult(Win32Bridge::Bridge::FormatCrtBuffer(NamedFormatOptions<Legacy>(), buffer, (std::numeric_limits<size_t>::max)(), 0, format, locale, args, CrtFormatFlavor::Standard)); }
    template<bool Legacy, class Character> int __cdecl FormatVUnbounded(Character* buffer, const Character* format, va_list args)
    { return FormatVUnboundedL<Legacy>(buffer, format, nullptr, args); }
    template<bool Legacy, class Character> int __cdecl FormatUnbounded(Character* buffer, const Character* format, ...)
    { va_list args; va_start(args, format); const int result = FormatVUnbounded<Legacy>(buffer, format, args); va_end(args); return result; }
    template<bool Legacy, class Character> int __cdecl FormatUnboundedL(Character* buffer, const Character* format, _locale_t locale, ...)
    { va_list args; va_start(args, locale); const int result = FormatVUnboundedL<Legacy>(buffer, format, locale, args); va_end(args); return result; }

    template<bool Legacy, class Character, CrtBufferStyle Style> int __cdecl FormatVBoundedL(Character* buffer, size_t count,
        const Character* format, _locale_t locale, va_list args)
    {
        unsigned __int64 options = NamedFormatOptions<Legacy>();
        if (Style == CrtBufferStyle::Legacy) options |= _CRT_INTERNAL_PRINTF_LEGACY_VSPRINTF_NULL_TERMINATION;
        if (Style == CrtBufferStyle::C99) options |= _CRT_INTERNAL_PRINTF_STANDARD_SNPRINTF_BEHAVIOR;
        const auto flavor = Style == CrtBufferStyle::Secure ? CrtFormatFlavor::Secure :
            Style == CrtBufferStyle::Positional ? CrtFormatFlavor::Positional : CrtFormatFlavor::Standard;
        return NormalizeFormattedResult(Win32Bridge::Bridge::FormatCrtBuffer(options, buffer, count, 0, format, locale, args, flavor));
    }
    template<bool Legacy, class Character, CrtBufferStyle Style> int __cdecl FormatVBounded(Character* buffer, size_t count, const Character* format, va_list args)
    { return FormatVBoundedL<Legacy, Character, Style>(buffer, count, format, nullptr, args); }
    template<bool Legacy, class Character, CrtBufferStyle Style> int __cdecl FormatBounded(Character* buffer, size_t count, const Character* format, ...)
    { va_list args; va_start(args, format); const int result = FormatVBounded<Legacy, Character, Style>(buffer, count, format, args); va_end(args); return result; }
    template<bool Legacy, class Character, CrtBufferStyle Style> int __cdecl FormatBoundedL(Character* buffer, size_t count, const Character* format, _locale_t locale, ...)
    { va_list args; va_start(args, locale); const int result = FormatVBoundedL<Legacy, Character, Style>(buffer, count, format, locale, args); va_end(args); return result; }

    template<bool Legacy, class Character> int __cdecl FormatVTruncateL(Character* buffer, size_t count, size_t maximum,
        const Character* format, _locale_t locale, va_list args)
    { return NormalizeFormattedResult(Win32Bridge::Bridge::FormatCrtBuffer(NamedFormatOptions<Legacy>(), buffer, count, maximum, format, locale, args, CrtFormatFlavor::SecureTruncate)); }
    template<bool Legacy, class Character> int __cdecl FormatVTruncate(Character* buffer, size_t count, size_t maximum, const Character* format, va_list args)
    { return FormatVTruncateL<Legacy>(buffer, count, maximum, format, nullptr, args); }
    template<bool Legacy, class Character> int __cdecl FormatTruncate(Character* buffer, size_t count, size_t maximum, const Character* format, ...)
    { va_list args; va_start(args, format); const int result = FormatVTruncate<Legacy>(buffer, count, maximum, format, args); va_end(args); return result; }
    template<bool Legacy, class Character> int __cdecl FormatTruncateL(Character* buffer, size_t count, size_t maximum, const Character* format, _locale_t locale, ...)
    { va_list args; va_start(args, locale); const int result = FormatVTruncateL<Legacy>(buffer, count, maximum, format, locale, args); va_end(args); return result; }

    template<bool Legacy, class Character, CrtFormatFlavor Flavor> int __cdecl FormatVCountL(const Character* format, _locale_t locale, va_list args)
    { return NormalizeFormattedResult(Win32Bridge::Bridge::FormatCrtBuffer(NamedFormatOptions<Legacy>() | _CRT_INTERNAL_PRINTF_STANDARD_SNPRINTF_BEHAVIOR,
        static_cast<Character*>(nullptr), 0, 0, format, locale, args, Flavor)); }
    template<bool Legacy, class Character, CrtFormatFlavor Flavor> int __cdecl FormatVCount(const Character* format, va_list args)
    { return FormatVCountL<Legacy, Character, Flavor>(format, nullptr, args); }
    template<bool Legacy, class Character, CrtFormatFlavor Flavor> int __cdecl FormatCount(const Character* format, ...)
    { va_list args; va_start(args, format); const int result = FormatVCount<Legacy, Character, Flavor>(format, args); va_end(args); return result; }
    template<bool Legacy, class Character, CrtFormatFlavor Flavor> int __cdecl FormatCountL(const Character* format, _locale_t locale, ...)
    { va_list args; va_start(args, locale); const int result = FormatVCountL<Legacy, Character, Flavor>(format, locale, args); va_end(args); return result; }

    template<bool Legacy, class Character, CrtFormatFlavor Flavor> int __cdecl FormatVFileL(FILE* stream, const Character* format, _locale_t locale, va_list args)
    { return FormatGuestStream(NamedFormatOptions<Legacy>(), stream, format, locale, args, Flavor); }
    template<bool Legacy, class Character, CrtFormatFlavor Flavor> int __cdecl FormatVFile(FILE* stream, const Character* format, va_list args)
    { return FormatVFileL<Legacy, Character, Flavor>(stream, format, nullptr, args); }
    template<bool Legacy, class Character, CrtFormatFlavor Flavor> int __cdecl FormatFile(FILE* stream, const Character* format, ...)
    { va_list args; va_start(args, format); const int result = FormatVFile<Legacy, Character, Flavor>(stream, format, args); va_end(args); return result; }
    template<bool Legacy, class Character, CrtFormatFlavor Flavor> int __cdecl FormatFileL(FILE* stream, const Character* format, _locale_t locale, ...)
    { va_list args; va_start(args, locale); const int result = FormatVFileL<Legacy, Character, Flavor>(stream, format, locale, args); va_end(args); return result; }
    template<bool Legacy, class Character, CrtFormatFlavor Flavor> int __cdecl FormatVConsoleL(const Character* format, _locale_t locale, va_list args)
    { return FormatVFileL<Legacy, Character, Flavor>(BridgeStdioAcrtIob(1), format, locale, args); }
    template<bool Legacy, class Character, CrtFormatFlavor Flavor> int __cdecl FormatVConsole(const Character* format, va_list args)
    { return FormatVConsoleL<Legacy, Character, Flavor>(format, nullptr, args); }
    template<bool Legacy, class Character, CrtFormatFlavor Flavor> int __cdecl FormatConsole(const Character* format, ...)
    { va_list args; va_start(args, format); const int result = FormatVConsole<Legacy, Character, Flavor>(format, args); va_end(args); return result; }
    template<bool Legacy, class Character, CrtFormatFlavor Flavor> int __cdecl FormatConsoleL(const Character* format, _locale_t locale, ...)
    { va_list args; va_start(args, locale); const int result = FormatVConsoleL<Legacy, Character, Flavor>(format, locale, args); va_end(args); return result; }

    using LegacyWidePrintSignature = int(__cdecl*)(wchar_t*, const wchar_t*, ...);
    using BoundedWidePrintSignature = int(__cdecl*)(wchar_t*, size_t, const wchar_t*, ...);
    using BoundedWideLocaleVSignature = int(__cdecl*)(wchar_t*, size_t, const wchar_t*, _locale_t, va_list);
    static_assert(std::is_same<decltype(&FormatUnbounded<true, wchar_t>), LegacyWidePrintSignature>::value, "Legacy swprintf ABI mismatch");
    static_assert(std::is_same<decltype(&FormatBounded<false, wchar_t, CrtBufferStyle::Terminated>), BoundedWidePrintSignature>::value, "UCRT swprintf ABI mismatch");
    static_assert(std::is_same<decltype(&FormatVBoundedL<false, wchar_t, CrtBufferStyle::Terminated>), BoundedWideLocaleVSignature>::value, "UCRT locale wide ABI mismatch");
    template<bool Legacy> Win32Bridge::Bridge::ImportResolution ResolveNamedFormatting(const Win32Bridge::Bridge::ImportedSymbol& symbol)
    {
        using namespace Win32Bridge::Bridge;
        auto result = CompatibilityCatalog::Resolve(symbol);
#define CRT_FMT(exportName, ...) if (symbol.name == L##exportName) result.targetAddress = reinterpret_cast<ULONGLONG>(&__VA_ARGS__); else
        CRT_FMT("sprintf", FormatUnbounded<Legacy, char>)
        CRT_FMT("vsprintf", FormatVUnbounded<Legacy, char>)
        CRT_FMT("_sprintf_l", FormatUnboundedL<Legacy, char>)
        CRT_FMT("_vsprintf_l", FormatVUnboundedL<Legacy, char>)
        CRT_FMT("_swprintf", FormatUnbounded<Legacy, wchar_t>)
        CRT_FMT("_vswprintf", FormatVUnbounded<Legacy, wchar_t>)
        CRT_FMT("__swprintf_l", FormatUnboundedL<Legacy, wchar_t>)
        CRT_FMT("__vswprintf_l", FormatVUnboundedL<Legacy, wchar_t>)
        CRT_FMT("snprintf", FormatBounded<Legacy, char, CrtBufferStyle::C99>)
        CRT_FMT("vsnprintf", FormatVBounded<Legacy, char, Legacy ? CrtBufferStyle::Legacy : CrtBufferStyle::C99>)
        CRT_FMT("_snprintf", FormatBounded<Legacy, char, CrtBufferStyle::Legacy>)
        CRT_FMT("_vsnprintf", FormatVBounded<Legacy, char, CrtBufferStyle::Legacy>)
        CRT_FMT("_snprintf_l", FormatBoundedL<Legacy, char, CrtBufferStyle::Legacy>)
        CRT_FMT("_vsnprintf_l", FormatVBoundedL<Legacy, char, CrtBufferStyle::Legacy>)
        CRT_FMT("_snprintf_c", FormatBounded<Legacy, char, CrtBufferStyle::Terminated>)
        CRT_FMT("_vsnprintf_c", FormatVBounded<Legacy, char, CrtBufferStyle::Terminated>)
        CRT_FMT("_snprintf_c_l", FormatBoundedL<Legacy, char, CrtBufferStyle::Terminated>)
        CRT_FMT("_vsnprintf_c_l", FormatVBoundedL<Legacy, char, CrtBufferStyle::Terminated>)
        CRT_FMT("_snwprintf", FormatBounded<Legacy, wchar_t, CrtBufferStyle::Legacy>)
        CRT_FMT("_vsnwprintf", FormatVBounded<Legacy, wchar_t, CrtBufferStyle::Legacy>)
        CRT_FMT("_snwprintf_l", FormatBoundedL<Legacy, wchar_t, CrtBufferStyle::Legacy>)
        CRT_FMT("_vsnwprintf_l", FormatVBoundedL<Legacy, wchar_t, CrtBufferStyle::Legacy>)
        CRT_FMT("_swprintf_c", FormatBounded<Legacy, wchar_t, CrtBufferStyle::Terminated>)
        CRT_FMT("_vswprintf_c", FormatVBounded<Legacy, wchar_t, CrtBufferStyle::Terminated>)
        CRT_FMT("_swprintf_c_l", FormatBoundedL<Legacy, wchar_t, CrtBufferStyle::Terminated>)
        CRT_FMT("_vswprintf_c_l", FormatVBoundedL<Legacy, wchar_t, CrtBufferStyle::Terminated>)
        CRT_FMT("sprintf_s", FormatBounded<Legacy, char, CrtBufferStyle::Secure>)
        CRT_FMT("vsprintf_s", FormatVBounded<Legacy, char, CrtBufferStyle::Secure>)
        CRT_FMT("_sprintf_s_l", FormatBoundedL<Legacy, char, CrtBufferStyle::Secure>)
        CRT_FMT("_vsprintf_s_l", FormatVBoundedL<Legacy, char, CrtBufferStyle::Secure>)
        CRT_FMT("swprintf_s", FormatBounded<Legacy, wchar_t, CrtBufferStyle::Secure>)
        CRT_FMT("vswprintf_s", FormatVBounded<Legacy, wchar_t, CrtBufferStyle::Secure>)
        CRT_FMT("_swprintf_s_l", FormatBoundedL<Legacy, wchar_t, CrtBufferStyle::Secure>)
        CRT_FMT("_vswprintf_s_l", FormatVBoundedL<Legacy, wchar_t, CrtBufferStyle::Secure>)
        CRT_FMT("_snprintf_s", FormatTruncate<Legacy, char>)
        CRT_FMT("_vsnprintf_s", FormatVTruncate<Legacy, char>)
        CRT_FMT("_snprintf_s_l", FormatTruncateL<Legacy, char>)
        CRT_FMT("_vsnprintf_s_l", FormatVTruncateL<Legacy, char>)
        CRT_FMT("_snwprintf_s", FormatTruncate<Legacy, wchar_t>)
        CRT_FMT("_vsnwprintf_s", FormatVTruncate<Legacy, wchar_t>)
        CRT_FMT("_snwprintf_s_l", FormatTruncateL<Legacy, wchar_t>)
        CRT_FMT("_vsnwprintf_s_l", FormatVTruncateL<Legacy, wchar_t>)
        CRT_FMT("_sprintf_p", FormatBounded<Legacy, char, CrtBufferStyle::Positional>)
        CRT_FMT("_vsprintf_p", FormatVBounded<Legacy, char, CrtBufferStyle::Positional>)
        CRT_FMT("_sprintf_p_l", FormatBoundedL<Legacy, char, CrtBufferStyle::Positional>)
        CRT_FMT("_vsprintf_p_l", FormatVBoundedL<Legacy, char, CrtBufferStyle::Positional>)
        CRT_FMT("_swprintf_p", FormatBounded<Legacy, wchar_t, CrtBufferStyle::Positional>)
        CRT_FMT("_vswprintf_p", FormatVBounded<Legacy, wchar_t, CrtBufferStyle::Positional>)
        CRT_FMT("_swprintf_p_l", FormatBoundedL<Legacy, wchar_t, CrtBufferStyle::Positional>)
        CRT_FMT("_vswprintf_p_l", FormatVBoundedL<Legacy, wchar_t, CrtBufferStyle::Positional>)
        CRT_FMT("_scprintf", FormatCount<Legacy, char, CrtFormatFlavor::Standard>)
        CRT_FMT("_vscprintf", FormatVCount<Legacy, char, CrtFormatFlavor::Standard>)
        CRT_FMT("_scprintf_l", FormatCountL<Legacy, char, CrtFormatFlavor::Standard>)
        CRT_FMT("_vscprintf_l", FormatVCountL<Legacy, char, CrtFormatFlavor::Standard>)
        CRT_FMT("_scwprintf", FormatCount<Legacy, wchar_t, CrtFormatFlavor::Standard>)
        CRT_FMT("_vscwprintf", FormatVCount<Legacy, wchar_t, CrtFormatFlavor::Standard>)
        CRT_FMT("_scwprintf_l", FormatCountL<Legacy, wchar_t, CrtFormatFlavor::Standard>)
        CRT_FMT("_vscwprintf_l", FormatVCountL<Legacy, wchar_t, CrtFormatFlavor::Standard>)
        CRT_FMT("_scprintf_p", FormatCount<Legacy, char, CrtFormatFlavor::Positional>)
        CRT_FMT("_vscprintf_p", FormatVCount<Legacy, char, CrtFormatFlavor::Positional>)
        CRT_FMT("_scprintf_p_l", FormatCountL<Legacy, char, CrtFormatFlavor::Positional>)
        CRT_FMT("_vscprintf_p_l", FormatVCountL<Legacy, char, CrtFormatFlavor::Positional>)
        CRT_FMT("_scwprintf_p", FormatCount<Legacy, wchar_t, CrtFormatFlavor::Positional>)
        CRT_FMT("_vscwprintf_p", FormatVCount<Legacy, wchar_t, CrtFormatFlavor::Positional>)
        CRT_FMT("_scwprintf_p_l", FormatCountL<Legacy, wchar_t, CrtFormatFlavor::Positional>)
        CRT_FMT("_vscwprintf_p_l", FormatVCountL<Legacy, wchar_t, CrtFormatFlavor::Positional>)
#define CRT_FMT_STREAM_SET(narrow, wide, flavor) \
        CRT_FMT(narrow, FormatFile<Legacy, char, flavor>) \
        CRT_FMT(wide, FormatFile<Legacy, wchar_t, flavor>)
        CRT_FMT_STREAM_SET("fprintf", "fwprintf", CrtFormatFlavor::Standard)
        CRT_FMT_STREAM_SET("fprintf_s", "fwprintf_s", CrtFormatFlavor::Secure)
        CRT_FMT_STREAM_SET("_fprintf_p", "_fwprintf_p", CrtFormatFlavor::Positional)
#undef CRT_FMT_STREAM_SET
#define CRT_FMT_VSTREAM_SET(narrow, wide, flavor) \
        CRT_FMT(narrow, FormatVFile<Legacy, char, flavor>) \
        CRT_FMT(wide, FormatVFile<Legacy, wchar_t, flavor>)
        CRT_FMT_VSTREAM_SET("vfprintf", "vfwprintf", CrtFormatFlavor::Standard)
        CRT_FMT_VSTREAM_SET("vfprintf_s", "vfwprintf_s", CrtFormatFlavor::Secure)
        CRT_FMT_VSTREAM_SET("_vfprintf_p", "_vfwprintf_p", CrtFormatFlavor::Positional)
#undef CRT_FMT_VSTREAM_SET
#define CRT_FMT_STREAM_L_SET(narrow, wide, flavor) \
        CRT_FMT(narrow, FormatFileL<Legacy, char, flavor>) \
        CRT_FMT(wide, FormatFileL<Legacy, wchar_t, flavor>)
        CRT_FMT_STREAM_L_SET("_fprintf_l", "_fwprintf_l", CrtFormatFlavor::Standard)
        CRT_FMT_STREAM_L_SET("_fprintf_s_l", "_fwprintf_s_l", CrtFormatFlavor::Secure)
        CRT_FMT_STREAM_L_SET("_fprintf_p_l", "_fwprintf_p_l", CrtFormatFlavor::Positional)
#undef CRT_FMT_STREAM_L_SET
#define CRT_FMT_VSTREAM_L_SET(narrow, wide, flavor) \
        CRT_FMT(narrow, FormatVFileL<Legacy, char, flavor>) \
        CRT_FMT(wide, FormatVFileL<Legacy, wchar_t, flavor>)
        CRT_FMT_VSTREAM_L_SET("_vfprintf_l", "_vfwprintf_l", CrtFormatFlavor::Standard)
        CRT_FMT_VSTREAM_L_SET("_vfprintf_s_l", "_vfwprintf_s_l", CrtFormatFlavor::Secure)
        CRT_FMT_VSTREAM_L_SET("_vfprintf_p_l", "_vfwprintf_p_l", CrtFormatFlavor::Positional)
#undef CRT_FMT_VSTREAM_L_SET
#define CRT_FMT_CONSOLE_SET(narrow, wide, flavor) \
        CRT_FMT(narrow, FormatConsole<Legacy, char, flavor>) \
        CRT_FMT(wide, FormatConsole<Legacy, wchar_t, flavor>)
        CRT_FMT_CONSOLE_SET("printf", "wprintf", CrtFormatFlavor::Standard)
        CRT_FMT_CONSOLE_SET("printf_s", "wprintf_s", CrtFormatFlavor::Secure)
        CRT_FMT_CONSOLE_SET("_printf_p", "_wprintf_p", CrtFormatFlavor::Positional)
#undef CRT_FMT_CONSOLE_SET
#define CRT_FMT_VCONSOLE_SET(narrow, wide, flavor) \
        CRT_FMT(narrow, FormatVConsole<Legacy, char, flavor>) \
        CRT_FMT(wide, FormatVConsole<Legacy, wchar_t, flavor>)
        CRT_FMT_VCONSOLE_SET("vprintf", "vwprintf", CrtFormatFlavor::Standard)
        CRT_FMT_VCONSOLE_SET("vprintf_s", "vwprintf_s", CrtFormatFlavor::Secure)
        CRT_FMT_VCONSOLE_SET("_vprintf_p", "_vwprintf_p", CrtFormatFlavor::Positional)
#undef CRT_FMT_VCONSOLE_SET
#define CRT_FMT_CONSOLE_L_SET(narrow, wide, flavor) \
        CRT_FMT(narrow, FormatConsoleL<Legacy, char, flavor>) \
        CRT_FMT(wide, FormatConsoleL<Legacy, wchar_t, flavor>)
        CRT_FMT_CONSOLE_L_SET("_printf_l", "_wprintf_l", CrtFormatFlavor::Standard)
        CRT_FMT_CONSOLE_L_SET("_printf_s_l", "_wprintf_s_l", CrtFormatFlavor::Secure)
        CRT_FMT_CONSOLE_L_SET("_printf_p_l", "_wprintf_p_l", CrtFormatFlavor::Positional)
#undef CRT_FMT_CONSOLE_L_SET
#define CRT_FMT_VCONSOLE_L_SET(narrow, wide, flavor) \
        CRT_FMT(narrow, FormatVConsoleL<Legacy, char, flavor>) \
        CRT_FMT(wide, FormatVConsoleL<Legacy, wchar_t, flavor>)
        CRT_FMT_VCONSOLE_L_SET("_vprintf_l", "_vwprintf_l", CrtFormatFlavor::Standard)
        CRT_FMT_VCONSOLE_L_SET("_vprintf_s_l", "_vwprintf_s_l", CrtFormatFlavor::Secure)
        CRT_FMT_VCONSOLE_L_SET("_vprintf_p_l", "_vwprintf_p_l", CrtFormatFlavor::Positional)
#undef CRT_FMT_VCONSOLE_L_SET
        { }
#undef CRT_FMT
        if (symbol.name == L"swprintf") result.targetAddress = Legacy ? reinterpret_cast<ULONGLONG>(&FormatUnbounded<true, wchar_t>) : reinterpret_cast<ULONGLONG>(&FormatBounded<false, wchar_t, CrtBufferStyle::Terminated>);
        if (symbol.name == L"vswprintf") result.targetAddress = Legacy ? reinterpret_cast<ULONGLONG>(&FormatVUnbounded<true, wchar_t>) : reinterpret_cast<ULONGLONG>(&FormatVBounded<false, wchar_t, CrtBufferStyle::Terminated>);
        if (symbol.name == L"_swprintf_l") result.targetAddress = Legacy ? reinterpret_cast<ULONGLONG>(&FormatUnboundedL<true, wchar_t>) : reinterpret_cast<ULONGLONG>(&FormatBoundedL<false, wchar_t, CrtBufferStyle::Terminated>);
        if (symbol.name == L"_vswprintf_l") result.targetAddress = Legacy ? reinterpret_cast<ULONGLONG>(&FormatVUnboundedL<true, wchar_t>) : reinterpret_cast<ULONGLONG>(&FormatVBoundedL<false, wchar_t, CrtBufferStyle::Terminated>);
        return result;
    }

    Win32Bridge::Bridge::ImportResolution ResolveCrtFormattingImport(const Win32Bridge::Bridge::ImportedSymbol& symbol)
    {
        using namespace Win32Bridge::Bridge;
        auto result = CompatibilityCatalog::Resolve(symbol);
        if (symbol.importedByOrdinal || (!IsMsvcrt(symbol.library) && _wcsicmp(symbol.library.c_str(), L"ucrtbase.dll"))) return result;
#define CRT_COMMON(exportName, ...) if (symbol.name == L##exportName) result.targetAddress = reinterpret_cast<ULONGLONG>(&__VA_ARGS__); else
        CRT_COMMON("__stdio_common_vsprintf", CommonFormatBuffer<char, CrtFormatFlavor::Standard>)
        CRT_COMMON("__stdio_common_vswprintf", CommonFormatBuffer<wchar_t, CrtFormatFlavor::Standard>)
        CRT_COMMON("__stdio_common_vsprintf_s", CommonFormatBuffer<char, CrtFormatFlavor::Secure>)
        CRT_COMMON("__stdio_common_vswprintf_s", CommonFormatBuffer<wchar_t, CrtFormatFlavor::Secure>)
        CRT_COMMON("__stdio_common_vsnprintf_s", CommonFormatTruncate<char>)
        CRT_COMMON("__stdio_common_vsnwprintf_s", CommonFormatTruncate<wchar_t>)
        CRT_COMMON("__stdio_common_vsprintf_p", CommonFormatBuffer<char, CrtFormatFlavor::Positional>)
        CRT_COMMON("__stdio_common_vswprintf_p", CommonFormatBuffer<wchar_t, CrtFormatFlavor::Positional>)
        CRT_COMMON("__stdio_common_vfprintf", CommonFormatStream<char, CrtFormatFlavor::Standard>)
        CRT_COMMON("__stdio_common_vfwprintf", CommonFormatStream<wchar_t, CrtFormatFlavor::Standard>)
        CRT_COMMON("__stdio_common_vfprintf_s", CommonFormatStream<char, CrtFormatFlavor::Secure>)
        CRT_COMMON("__stdio_common_vfwprintf_s", CommonFormatStream<wchar_t, CrtFormatFlavor::Secure>)
        CRT_COMMON("__stdio_common_vfprintf_p", CommonFormatStream<char, CrtFormatFlavor::Positional>)
        CRT_COMMON("__stdio_common_vfwprintf_p", CommonFormatStream<wchar_t, CrtFormatFlavor::Positional>)
        { }
#undef CRT_COMMON
        if (!result.targetAddress) result = IsMsvcrt(symbol.library) ? ResolveNamedFormatting<true>(symbol) : ResolveNamedFormatting<false>(symbol);
        if (result.targetAddress)
        {
            result.disposition = ImportDisposition::NeedsBridge;
            result.note = L"Native in-memory formatting with guest-only FILE output.";
        }
        return result;
    }
}