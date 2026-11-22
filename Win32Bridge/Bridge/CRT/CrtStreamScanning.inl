// Included by CrtScanning.cpp. The lexical rules follow UCRT's input processor
// and input_adapter_character_source (also present in ReactOS's sdk/lib/ucrt).
// Numeric conversion/rounding stays in the native memory-only UCRT scanner.
namespace Win32Bridge { namespace Bridge { namespace {
    int ScanLower(int c) { return c >= 'A' && c <= 'Z' ? c + ('a' - 'A') : c; }
    template<class Character> unsigned ScanCode(Character c)
    { return static_cast<typename std::make_unsigned<Character>::type>(c); }

    template<class Character> unsigned ScanDigit(int c,_locale_t locale)
    {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'z') return c - 'a' + 10;
        if (c >= 'A' && c <= 'Z') return c - 'A' + 10;
        if (sizeof(Character) > 1 && c >= 0)
        {
            // Use the same Unicode decimal digit table as the active CRT. This
            // avoids maintaining an independent, eventually stale range table.
            const wchar_t probe[2] = { static_cast<wchar_t>(c), 0 }; wchar_t* end = nullptr;
            const int saved = errno; const auto value = ::_wcstoul_l(probe, &end, 10,locale); errno = saved;
            if (end == probe + 1 && value < 10) return static_cast<unsigned>(value);
        }
        return (std::numeric_limits<unsigned>::max)();
    }
    bool ScanStringSeparator(int value)
    {
        return value == ' ' || (value >= '\t' && value <= '\r');
    }

    enum class ScanLength { None, HH, H, L, LL, J, Z, T, CapitalL, I32, I64, CapitalT };
    template<class Character> struct ScanDirective
    {
        const Character* begin = nullptr;
        const Character* end = nullptr;
        uint64_t width = 0;
        bool suppressed = false, wide = false, rejectSet = false;
        ScanLength length = ScanLength::None;
        int conversion = 0;
        std::vector<std::pair<unsigned, unsigned>> ranges;
        bool Text() const { return conversion == 'c' || conversion == 's' || conversion == '['; }
        bool Float() const { return conversion == 'a' || conversion == 'e' || conversion == 'f' || conversion == 'g'; }
        bool Matches(int c) const
        {
            bool found = false;
            for (const auto& range : ranges)
                if (static_cast<unsigned>(c) >= range.first && static_cast<unsigned>(c) <= range.second) { found = true; break; }
            return rejectSet != found;
        }
        size_t IntegerSize() const
        {
            switch (length)
            {
            case ScanLength::HH: return 1;
            case ScanLength::H: return 2;
            case ScanLength::LL: case ScanLength::J: case ScanLength::Z: case ScanLength::T: case ScanLength::I64: return 8;
            default: return 4; // Windows long remains 32 bits on x64.
            }
        }
        bool Parse(const Character*& next, unsigned __int64 options, _locale_t locale)
        {
            begin = next++; // '%'
            if (*next == '*') { suppressed = true; ++next; }
            unsigned digit = ScanDigit<Character>(ScanCode(*next), locale);
            if (digit <= 9)
            {
                constexpr auto maximumWidth = (std::numeric_limits<uint64_t>::max)();
                do
                {
                    if (width > (maximumWidth - digit) / 10)
                    {
                        width = maximumWidth;
                        errno = ERANGE;
                    }
                    else width = width * 10 + digit;
                    digit = ScanDigit<Character>(ScanCode(*++next), locale);
                } while (digit <= 9);
                if (!width) return false;
            }
            switch (*next)
            {
            case 'h': ++next; length = ScanLength::H; if (*next == 'h') { ++next; length = ScanLength::HH; } break;
            case 'l': ++next; length = ScanLength::L; if (*next == 'l') { ++next; length = ScanLength::LL; } break;
            case 'L': ++next; length = ScanLength::CapitalL; break;
            case 'j': ++next; length = ScanLength::J; break;
            case 'z': ++next; length = ScanLength::Z; break;
            case 't': ++next; length = ScanLength::T; break;
            case 'T': ++next; length = ScanLength::CapitalT; break;
            case 'I':
                if (next[1] == '3' && next[2] == '2') { next += 3; length = ScanLength::I32; }
                else if (next[1] == '6' && next[2] == '4') { next += 3; length = ScanLength::I64; }
                else if (next[1] > 0 && next[1] < 128 && std::strchr("diouxX", static_cast<int>(next[1]))) { ++next; length = ScanLength::I64; }
                break;
            }
            const bool explicitWide = *next == 'w';
            if (explicitWide) ++next;
            const int original = ScanCode(*next);
            if (!original || original > 127) return false;
            ++next;
            wide = explicitWide || (sizeof(Character) == 1 ? original == 'C' || original == 'S' :
                original != 'C' && original != 'S' && (length == ScanLength::CapitalT || (options & _CRT_INTERNAL_SCANF_LEGACY_WIDE_SPECIFIERS)));
            if (length == ScanLength::H) wide = false;
            if (length == ScanLength::L || length == ScanLength::LL || length == ScanLength::CapitalL) wide = true;
            conversion = original == 'I' ? 'i' : ScanLower(original);
            // Uppercase conversion aliases are limited to the ones UCRT accepts.
            if (original >= 'A' && original <= 'Z' && !std::strchr("CSIXAEFG", original)) return false;
            if (conversion == 'p') length = ScanLength::I64;
            if (conversion == 'c' && !width) width = 1;
            if (conversion == '[')
            {
                rejectSet = *next == '^'; if (rejectSet) ++next;
                if (*next == ']') { ranges.emplace_back(']', ']'); ++next; }
                const Character* first = next; const Character* lastRangeEnd = nullptr;
                for (; *next && *next != ']'; ++next)
                {
                    if (*next != '-' || next == first || next - 1 == lastRangeEnd || next[1] == ']')
                        ranges.emplace_back(ScanCode(*next), ScanCode(*next));
                    else
                    {
                        const auto a = ScanCode(next[-1]), b = ScanCode(next[1]);
                        ranges.emplace_back((std::min)(a, b), (std::max)(a, b)); lastRangeEnd = next + 1;
                    }
                }
                if (*next != ']') return false;
                ++next;
            }
            end = next;
            bool valid = false;
            if (conversion == '[') valid = length == ScanLength::None || length == ScanLength::L || length == ScanLength::CapitalT;
            else if (Text()) valid = length == ScanLength::None || length == ScanLength::H || length == ScanLength::L ||
                length == ScanLength::LL || length == ScanLength::CapitalL || length == ScanLength::CapitalT;
            else if (Float()) valid = length == ScanLength::None || length == ScanLength::L || length == ScanLength::CapitalL;
            else if (std::strchr("diouxpn", conversion)) valid = length != ScanLength::CapitalL && length != ScanLength::CapitalT;
            return valid;
        }
    };

    class CountedScanInput
    {
        CrtScanInput& input;
        _locale_t locale;
        bool wide;
    public:
        uint64_t count = 0;
        CountedScanInput(CrtScanInput& value,_locale_t selected,bool wideInput) : input(value),locale(selected),wide(wideInput) { }
        bool Space(int c) const {return c>=0&&(wide?::_iswspace_l(static_cast<wint_t>(c),locale) : ::_isspace_l(c,locale));}
        int Get() { const int c = input.Get(); if (c >= 0) ++count; return c; }
        void Unget(int c) { if (c >= 0) { input.Unget(c); --count; } }
        int Peek() { const int c = Get(); Unget(c); return c; }
        void Whitespace() { int c; do { c = Get(); } while (Space(c)); Unget(c); }
    };

    template<class Character> std::basic_string<Character> ScanNumberToken(CountedScanInput& input, const ScanDirective<Character>& directive,_locale_t locale)
    {
        std::basic_string<Character> token; uint64_t remaining = directive.width ? directive.width : (std::numeric_limits<uint64_t>::max)();
        auto read = [&]() { if (!remaining) return -1; --remaining; return input.Get(); };
        int c = read();
        auto take = [&]() { token.push_back(static_cast<Character>(c)); c = read(); };
        if (c == '+' || c == '-') take();
        if (directive.Float())
        {
            const int first = ScanLower(c);
            if (first == 'i' || first == 'n')
            {
                const char* word = first == 'i' ? "inf" : "nan";
                bool matched = true;
                for (; *word; ++word) { if (ScanLower(c) != *word) { matched = false; break; } take(); }
                if (matched && first == 'i' && ScanLower(c) == 'i')
                    for (word = "inity"; *word && ScanLower(c) == *word; ++word) take();
                if (matched && first == 'n' && c == '(')
                {
                    take();
                    while (c >= 0 && (ScanDigit<Character>(c,locale) < 36 || c == '_')) take();
                    if (c == ')') take();
                }
            }
            else
            {
                bool hex = false, digits = false;
                if (c == '0') { digits = true; take(); if (c == 'x' || c == 'X') { hex = true; digits = false; take(); } }
                const unsigned base = hex ? 16 : 10;
                while (ScanDigit<Character>(c,locale) < base) { digits = true; take(); }
                // There is no public localeconv_l. A memory-only native format
                // obtains the same first radix byte used by UCRT strtox without
                // reading private locale structures or changing the caller PTD.
                char formatted[16]{};const int savedError=errno;
                const int length=::_snprintf_s_l(formatted,sizeof(formatted),_TRUNCATE,"%.1f",locale,1.0);
                const int radix=length>=3?static_cast<unsigned char>(formatted[1]):'.';errno=savedError;
                if (c == radix) { take(); while (ScanDigit<Character>(c,locale) < base) { digits = true; take(); } }
                if (digits && ScanLower(c) == (hex ? 'p' : 'e'))
                {
                    take(); if (c == '+' || c == '-') take();
                    while (ScanDigit<Character>(c,locale) < 10) take();
                }
            }
        }
        else
        {
            unsigned base = directive.conversion == 'o' ? 8 : (directive.conversion == 'x' || directive.conversion == 'p') ? 16 :
                directive.conversion == 'i' ? 0 : 10;
            if ((base == 0 || base == 16) && ScanDigit<Character>(c,locale) == 0)
            {
                take();
                if (c == 'x' || c == 'X') { base = 16; take(); }
                else if (!base) base = 8;
            }
            if (!base) base = 10;
            while (ScanDigit<Character>(c,locale) < base) take();
        }
        // UCRT's strtox adapter maps EOF to NUL and cannot unget an actual NUL.
        if (c != 0) input.Unget(c);
        return token;
    }
    template<class Character> int ScanOneNumber(unsigned __int64 options,_locale_t locale, const Character* text, size_t count, const Character* format, ...)
    {
        va_list args; va_start(args, format);
        const int result = ScanCrtBuffer(options, text, count, format, locale, args);
        va_end(args); return result;
    }
    template<class Character> bool ScanTextConversion(unsigned __int64 options, CountedScanInput& input,
        const ScanDirective<Character>& directive, va_list& args,_locale_t locale)
    {
        if (directive.conversion == 's') input.Whitespace();
        char* output = directive.suppressed ? nullptr : static_cast<char*>(va_arg(args, void*));
        if (!directive.suppressed && !output) { errno = EINVAL; return false; }
        const bool secure = (options & _CRT_INTERNAL_SCANF_SECURECRT) != 0;
        const size_t capacity = output && secure ? va_arg(args, unsigned) : (std::numeric_limits<size_t>::max)();
        const size_t elementSize = directive.wide ? sizeof(wchar_t) : 1;
        if (!capacity)
        {
            if (options & _CRT_INTERNAL_SCANF_LEGACY_MSVCRT_COMPATIBILITY) { input.Get(); memset(output, 0, elementSize); }
            errno = ENOMEM; return false;
        }
        size_t available = capacity;
        if (directive.conversion != 'c' && available != (std::numeric_limits<size_t>::max)()) --available;
        uint64_t matched = 0; size_t written = 0;
        for (; !directive.width || matched < directive.width; ++matched)
        {
            const int c = input.Get();
            if (c < 0 || (directive.conversion == 's' && ScanStringSeparator(c)) || (directive.conversion == '[' && !directive.Matches(c)))
            { input.Unget(c); break; }
            if (directive.suppressed) continue;
            if (!available) { memset(output, 0, elementSize); errno = ENOMEM; return false; }
            char narrow[MB_LEN_MAX]{}; wchar_t wide = static_cast<wchar_t>(c); size_t elements = 1;
            const void* converted = directive.wide ? static_cast<const void*>(&wide) : static_cast<const void*>(narrow);
            if (sizeof(Character) == 1 && directive.wide)
            {
                narrow[0] = static_cast<char>(c);
                size_t bytes = 1;
                if (::_isctype_l(static_cast<unsigned char>(c),_LEADBYTE,locale)) { narrow[1] = static_cast<char>(input.Get()); bytes = 2; }
                wide = L'?'; ::_mbtowc_l(&wide, narrow, bytes,locale);
            }
            else if (sizeof(Character) > 1 && !directive.wide)
            {
                int convertedSize = 0; ::_wctomb_s_l(&convertedSize, narrow, sizeof(narrow), static_cast<wchar_t>(c),locale);
                elements = convertedSize > 0 ? static_cast<size_t>(convertedSize) : 0;
            }
            else if (!directive.wide) narrow[0] = static_cast<char>(c);
            if (elements > available) { memset(output, 0, elementSize); errno = ERANGE; break; }
            memcpy(output + written * elementSize, converted, elements * elementSize);
            written += elements; available -= elements;
        }
        if (!matched || (directive.conversion == 'c' && matched != directive.width && !(options & _CRT_INTERNAL_SCANF_LEGACY_MSVCRT_COMPATIBILITY))) return false;
        if (output && directive.conversion != 'c') memset(output + written * elementSize, 0, elementSize);
        return true;
    }
    template<class Character> int StreamScan(unsigned __int64 options, CrtScanInput& transport, const Character* format, _locale_t locale, va_list arguments)
    {
        if (!format) { errno = EINVAL; return -1; }
        CountedScanInput input(transport, locale, sizeof(Character) > 1);
        int assignments = 0;
        bool failed = false;
        int formatError = 0;
        va_list args; va_copy(args, arguments);
        struct EndArguments { va_list& args; ~EndArguments() { va_end(args); } } endArguments{ args };
        const Character* next = format;
        while (*next)
        {
            if (input.Space(ScanCode(*next))) { ++next; input.Whitespace(); continue; }
            if (*next != '%' || next[1] == '%')
            {
                const int expected = ScanCode(*next++);
                if (expected == '%') ++next;
                const bool needsTrail = sizeof(Character) == 1 &&
                    ::_isctype_l(static_cast<unsigned char>(expected), _LEADBYTE, locale) != 0;
                if (needsTrail && !*next)
                {
                    formatError = EILSEQ;
                    failed = true;
                    break;
                }
                const int c = input.Get();
                if (c != expected) { input.Unget(c); failed = true; break; }
                if (needsTrail)
                {
                    const int trail = input.Get();
                    if (trail != static_cast<int>(ScanCode(*next))) { input.Unget(trail); input.Unget(c); failed = true; break; }
                    ++next;
                }
                continue;
            }
            ScanDirective<Character> directive;
            if (!directive.Parse(next, options, locale))
            {
                formatError = EINVAL;
                failed = true;
                break;
            }
            if (directive.conversion == 'n')
            {
                if (!directive.suppressed)
                {
                    void* output = va_arg(args, void*);
                    if (!output) { errno = EINVAL; failed = true; break; }
                    const uint64_t count = input.count; memcpy(output, &count, directive.IntegerSize());
                }
                continue;
            }
            bool converted = false;
            if (directive.Text()) converted = ScanTextConversion(options, input, directive, args,locale);
            else
            {
                input.Whitespace();
                const auto token = ScanNumberToken(input, directive,locale);
                std::basic_string<Character> numericFormat(directive.begin, directive.end);
                numericFormat.push_back('%'); numericFormat.push_back('n');
                int used = -1;
                if (directive.suppressed) ScanOneNumber(options,locale, token.c_str(), token.size(), numericFormat.c_str(), &used);
                else
                {
                    // The UCRT obtains a destination only after recognizing the
                    // subject sequence; retrieving its slot here has no guest effects.
                    void* destination = va_arg(args, void*);
                    ScanOneNumber(options,locale, token.c_str(), token.size(), numericFormat.c_str(), destination, &used);
                }
                converted = used >= 0;
                if (converted && static_cast<size_t>(used) < token.size())
                    for (size_t index = token.size(); index > static_cast<size_t>(used); --index) input.Unget(ScanCode(token[index - 1]));
            }
            if (!converted) { failed = true; break; }
            if (!directive.suppressed) ++assignments;
        }
        const int result = !assignments && failed && input.Peek() < 0 ? -1 : assignments;
        // The native parser exposes format errors through errno only for the
        // secure family. Width overflow is an independent conversion error.
        if (formatError && (options & _CRT_INTERNAL_SCANF_SECURECRT)) errno = formatError;
        return result;
    }
} // anonymous namespace
int ScanCrtStream(unsigned __int64 options, CrtScanInput& input, const char* format, void* locale, va_list arguments)
{ if(!locale)locale=CurrentCrtLocaleHandle(); return StreamScan(options, input, format, static_cast<_locale_t>(locale), arguments); }
int ScanCrtStream(unsigned __int64 options, CrtScanInput& input, const wchar_t* format, void* locale, va_list arguments)
{ if(!locale)locale=CurrentCrtLocaleHandle(); return StreamScan(options, input, format, static_cast<_locale_t>(locale), arguments); }
} }
