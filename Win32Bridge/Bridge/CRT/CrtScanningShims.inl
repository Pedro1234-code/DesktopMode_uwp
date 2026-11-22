// Buffer and virtual-stream scanf entry points.
namespace
{
    template<bool Legacy, bool Secure> constexpr unsigned __int64 NamedScanOptions()
    {
        return (Legacy ? Win32Bridge::Bridge::CrtNamedMsvcrtScanOptions : Win32Bridge::Bridge::CrtNamedUcrtScanOptions) |
            (Secure ? _CRT_INTERNAL_SCANF_SECURECRT : 0);
    }
    template<class Character> int __cdecl CommonScanBuffer(unsigned __int64 options, const Character* input,
        size_t count, const Character* format, _locale_t locale, va_list args)
    { return Win32Bridge::Bridge::ScanCrtBuffer(options, input, count, format, locale, args); }
    template<bool Legacy, class Character, bool Secure> int __cdecl ScanVStringL(const Character* input,
        const Character* format, _locale_t locale, va_list args)
    { return Win32Bridge::Bridge::ScanCrtBuffer(NamedScanOptions<Legacy, Secure>(), input, (std::numeric_limits<size_t>::max)(), format, locale, args); }
    template<bool Legacy, class Character, bool Secure> int __cdecl ScanVString(const Character* input, const Character* format, va_list args)
    { return ScanVStringL<Legacy, Character, Secure>(input, format, nullptr, args); }
    template<bool Legacy, class Character, bool Secure> int __cdecl ScanString(const Character* input, const Character* format, ...)
    { va_list args; va_start(args, format); const int result = ScanVString<Legacy, Character, Secure>(input, format, args); va_end(args); return result; }
    template<bool Legacy, class Character, bool Secure> int __cdecl ScanStringL(const Character* input, const Character* format, _locale_t locale, ...)
    { va_list args; va_start(args, locale); const int result = ScanVStringL<Legacy, Character, Secure>(input, format, locale, args); va_end(args); return result; }
    template<bool Legacy, class Character, bool Secure> int __cdecl ScanVCountedL(const Character* input,
        size_t count, const Character* format, _locale_t locale, va_list args)
    { return Win32Bridge::Bridge::ScanCrtBuffer(NamedScanOptions<Legacy, Secure>(), input, count, format, locale, args); }
    template<bool Legacy, class Character, bool Secure> int __cdecl ScanVCounted(const Character* input, size_t count, const Character* format, va_list args)
    { return ScanVCountedL<Legacy, Character, Secure>(input, count, format, nullptr, args); }
    template<bool Legacy, class Character, bool Secure> int __cdecl ScanCounted(const Character* input, size_t count, const Character* format, ...)
    { va_list args; va_start(args, format); const int result = ScanVCounted<Legacy, Character, Secure>(input, count, format, args); va_end(args); return result; }
    template<bool Legacy, class Character, bool Secure> int __cdecl ScanCountedL(const Character* input, size_t count, const Character* format, _locale_t locale, ...)
    { va_list args; va_start(args, locale); const int result = ScanVCountedL<Legacy, Character, Secure>(input, count, format, locale, args); va_end(args); return result; }

    template<class Character> int ScanLockedGuestStream(unsigned __int64 options,
        Win32Bridge::Bridge::CrtStdioScanInput<(sizeof(Character) > 1)>* input,
        const Character* format, _locale_t locale, va_list args)
    {
        int result = -1;
        // Keep the SEH leaf free of objects requiring C++ unwinding (C2712).
        __try { result = Win32Bridge::Bridge::ScanCrtStream(options, *input, format, locale, args); }
        __finally { input->ReleaseLock(); }
        return result;
    }
    template<class Character> int __cdecl CommonScanStream(unsigned __int64 options, FILE* stream,
        const Character* format, _locale_t locale, va_list args)
    {
        auto io = CurrentCrtStdio(); if (!io) { errno = EBADF; return -1; }
        Win32Bridge::Bridge::CrtStdioScanInput<(sizeof(Character) > 1)> input(*io, stream);
        if (io->Descriptor(stream) < 0) return -1;
        try { return ScanLockedGuestStream(options, &input, format, locale, args); }
        catch (const std::bad_alloc&) { errno = ENOMEM; return -1; }
    }
    template<bool Legacy, class Character, bool Secure> int __cdecl ScanVFileL(FILE* stream,
        const Character* format, _locale_t locale, va_list args)
    { return CommonScanStream(NamedScanOptions<Legacy, Secure>(), stream, format, locale, args); }
    template<bool Legacy, class Character, bool Secure> int __cdecl ScanVFile(FILE* stream, const Character* format, va_list args)
    { return ScanVFileL<Legacy, Character, Secure>(stream, format, nullptr, args); }
    template<bool Legacy, class Character, bool Secure> int __cdecl ScanFile(FILE* stream, const Character* format, ...)
    { va_list args; va_start(args, format); const int result = ScanVFile<Legacy, Character, Secure>(stream, format, args); va_end(args); return result; }
    template<bool Legacy, class Character, bool Secure> int __cdecl ScanFileL(FILE* stream, const Character* format, _locale_t locale, ...)
    { va_list args; va_start(args, locale); const int result = ScanVFileL<Legacy, Character, Secure>(stream, format, locale, args); va_end(args); return result; }
    template<bool Legacy, class Character, bool Secure> int __cdecl ScanVInputL(const Character* format, _locale_t locale, va_list args)
    { return ScanVFileL<Legacy, Character, Secure>(BridgeStdioAcrtIob(0), format, locale, args); }
    template<bool Legacy, class Character, bool Secure> int __cdecl ScanVInput(const Character* format, va_list args)
    { return ScanVInputL<Legacy, Character, Secure>(format, nullptr, args); }
    template<bool Legacy, class Character, bool Secure> int __cdecl ScanInput(const Character* format, ...)
    { va_list args; va_start(args, format); const int result = ScanVInput<Legacy, Character, Secure>(format, args); va_end(args); return result; }
    template<bool Legacy, class Character, bool Secure> int __cdecl ScanInputL(const Character* format, _locale_t locale, ...)
    { va_list args; va_start(args, locale); const int result = ScanVInputL<Legacy, Character, Secure>(format, locale, args); va_end(args); return result; }
    template<bool Legacy> Win32Bridge::Bridge::ImportResolution ResolveNamedStringScanning(const Win32Bridge::Bridge::ImportedSymbol& symbol)
    {
        using namespace Win32Bridge::Bridge;
        auto result = CompatibilityCatalog::Resolve(symbol);
#define CRT_SCAN(exportName, ...) if (symbol.name == L##exportName) result.targetAddress = reinterpret_cast<ULONGLONG>(&__VA_ARGS__); else
        CRT_SCAN("fscanf", ScanFile<Legacy, char, false>)
        CRT_SCAN("fscanf_s", ScanFile<Legacy, char, true>)
        CRT_SCAN("vfscanf", ScanVFile<Legacy, char, false>)
        CRT_SCAN("vfscanf_s", ScanVFile<Legacy, char, true>)
        CRT_SCAN("_fscanf_l", ScanFileL<Legacy, char, false>)
        CRT_SCAN("_fscanf_s_l", ScanFileL<Legacy, char, true>)
        CRT_SCAN("_vfscanf_l", ScanVFileL<Legacy, char, false>)
        CRT_SCAN("_vfscanf_s_l", ScanVFileL<Legacy, char, true>)
        CRT_SCAN("fwscanf", ScanFile<Legacy, wchar_t, false>)
        CRT_SCAN("fwscanf_s", ScanFile<Legacy, wchar_t, true>)
        CRT_SCAN("vfwscanf", ScanVFile<Legacy, wchar_t, false>)
        CRT_SCAN("vfwscanf_s", ScanVFile<Legacy, wchar_t, true>)
        CRT_SCAN("_fwscanf_l", ScanFileL<Legacy, wchar_t, false>)
        CRT_SCAN("_fwscanf_s_l", ScanFileL<Legacy, wchar_t, true>)
        CRT_SCAN("_vfwscanf_l", ScanVFileL<Legacy, wchar_t, false>)
        CRT_SCAN("_vfwscanf_s_l", ScanVFileL<Legacy, wchar_t, true>)
        CRT_SCAN("scanf", ScanInput<Legacy, char, false>)
        CRT_SCAN("scanf_s", ScanInput<Legacy, char, true>)
        CRT_SCAN("vscanf", ScanVInput<Legacy, char, false>)
        CRT_SCAN("vscanf_s", ScanVInput<Legacy, char, true>)
        CRT_SCAN("_scanf_l", ScanInputL<Legacy, char, false>)
        CRT_SCAN("_scanf_s_l", ScanInputL<Legacy, char, true>)
        CRT_SCAN("_vscanf_l", ScanVInputL<Legacy, char, false>)
        CRT_SCAN("_vscanf_s_l", ScanVInputL<Legacy, char, true>)
        CRT_SCAN("wscanf", ScanInput<Legacy, wchar_t, false>)
        CRT_SCAN("wscanf_s", ScanInput<Legacy, wchar_t, true>)
        CRT_SCAN("vwscanf", ScanVInput<Legacy, wchar_t, false>)
        CRT_SCAN("vwscanf_s", ScanVInput<Legacy, wchar_t, true>)
        CRT_SCAN("_wscanf_l", ScanInputL<Legacy, wchar_t, false>)
        CRT_SCAN("_wscanf_s_l", ScanInputL<Legacy, wchar_t, true>)
        CRT_SCAN("_vwscanf_l", ScanVInputL<Legacy, wchar_t, false>)
        CRT_SCAN("_vwscanf_s_l", ScanVInputL<Legacy, wchar_t, true>)
        CRT_SCAN("sscanf", ScanString<Legacy, char, false>)
        CRT_SCAN("sscanf_s", ScanString<Legacy, char, true>)
        CRT_SCAN("vsscanf", ScanVString<Legacy, char, false>)
        CRT_SCAN("vsscanf_s", ScanVString<Legacy, char, true>)
        CRT_SCAN("_sscanf_l", ScanStringL<Legacy, char, false>)
        CRT_SCAN("_sscanf_s_l", ScanStringL<Legacy, char, true>)
        CRT_SCAN("_vsscanf_l", ScanVStringL<Legacy, char, false>)
        CRT_SCAN("_vsscanf_s_l", ScanVStringL<Legacy, char, true>)
        CRT_SCAN("swscanf", ScanString<Legacy, wchar_t, false>)
        CRT_SCAN("swscanf_s", ScanString<Legacy, wchar_t, true>)
        CRT_SCAN("vswscanf", ScanVString<Legacy, wchar_t, false>)
        CRT_SCAN("vswscanf_s", ScanVString<Legacy, wchar_t, true>)
        CRT_SCAN("_swscanf_l", ScanStringL<Legacy, wchar_t, false>)
        CRT_SCAN("_swscanf_s_l", ScanStringL<Legacy, wchar_t, true>)
        CRT_SCAN("_vswscanf_l", ScanVStringL<Legacy, wchar_t, false>)
        CRT_SCAN("_vswscanf_s_l", ScanVStringL<Legacy, wchar_t, true>)
        CRT_SCAN("_snscanf", ScanCounted<Legacy, char, false>)
        CRT_SCAN("_snscanf_s", ScanCounted<Legacy, char, true>)
        CRT_SCAN("_vsnscanf", ScanVCounted<Legacy, char, false>)
        CRT_SCAN("_vsnscanf_s", ScanVCounted<Legacy, char, true>)
        CRT_SCAN("_snscanf_l", ScanCountedL<Legacy, char, false>)
        CRT_SCAN("_snscanf_s_l", ScanCountedL<Legacy, char, true>)
        CRT_SCAN("_vsnscanf_l", ScanVCountedL<Legacy, char, false>)
        CRT_SCAN("_vsnscanf_s_l", ScanVCountedL<Legacy, char, true>)
        CRT_SCAN("_snwscanf", ScanCounted<Legacy, wchar_t, false>)
        CRT_SCAN("_snwscanf_s", ScanCounted<Legacy, wchar_t, true>)
        CRT_SCAN("_vsnwscanf", ScanVCounted<Legacy, wchar_t, false>)
        CRT_SCAN("_vsnwscanf_s", ScanVCounted<Legacy, wchar_t, true>)
        CRT_SCAN("_snwscanf_l", ScanCountedL<Legacy, wchar_t, false>)
        CRT_SCAN("_snwscanf_s_l", ScanCountedL<Legacy, wchar_t, true>)
        CRT_SCAN("_vsnwscanf_l", ScanVCountedL<Legacy, wchar_t, false>)
        CRT_SCAN("_vsnwscanf_s_l", ScanVCountedL<Legacy, wchar_t, true>)
        { }
#undef CRT_SCAN
        return result;
    }
    Win32Bridge::Bridge::ImportResolution ResolveCrtScanningImport(const Win32Bridge::Bridge::ImportedSymbol& symbol)
    {
        using namespace Win32Bridge::Bridge;
        auto result = CompatibilityCatalog::Resolve(symbol);
        if (symbol.importedByOrdinal || (!IsMsvcrt(symbol.library) && _wcsicmp(symbol.library.c_str(), L"ucrtbase.dll"))) return result;
        if (symbol.name == L"__stdio_common_vsscanf") result.targetAddress = reinterpret_cast<ULONGLONG>(&CommonScanBuffer<char>);
        else if (symbol.name == L"__stdio_common_vswscanf") result.targetAddress = reinterpret_cast<ULONGLONG>(&CommonScanBuffer<wchar_t>);
        else if (symbol.name == L"__stdio_common_vfscanf") result.targetAddress = reinterpret_cast<ULONGLONG>(&CommonScanStream<char>);
        else if (symbol.name == L"__stdio_common_vfwscanf") result.targetAddress = reinterpret_cast<ULONGLONG>(&CommonScanStream<wchar_t>);
        else result = IsMsvcrt(symbol.library) ? ResolveNamedStringScanning<true>(symbol) : ResolveNamedStringScanning<false>(symbol);
        if (result.targetAddress)
        {
            result.disposition = ImportDisposition::NeedsBridge;
            result.note = L"CRT scanf with guest streams, memory inputs, and native numeric conversion.";
        }
        return result;
    }
}