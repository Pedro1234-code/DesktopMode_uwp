#pragma once
#include "Bridge/CRT/CrtExceptions.h"
#include <cerrno>
#include <climits>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cwchar>
#include <ctype.h>
#include <uchar.h>
#include <algorithm>

namespace Win32Bridge { namespace Bridge {
namespace CrtRestartableDetail {
    constexpr size_t Invalid = static_cast<size_t>(-1);
    constexpr size_t Incomplete = static_cast<size_t>(-2);

    inline mbstate_t* SelectState(mbstate_t* state, mbstate_t& fallback, mbstate_t CrtLocaleThreadState::* implicitState)
    {
        if (state) return state;
        auto* thread = CurrentCrtThreadExceptionState();
        return thread ? &(thread->locale.*implicitState) : &fallback;
    }

    inline size_t InvalidSequence(mbstate_t& state)
    {
        state = {};
        errno = EILSEQ;
        return Invalid;
    }

    // UTF-8 state is the accumulated scalar, original length and remaining
    // byte count, matching the Windows mbstate_t representation.
    inline size_t DecodeUtf8(char32_t& scalar, const char* source, size_t count, mbstate_t& state)
    {
        size_t consumed = 0;
        unsigned length = state._Byte;
        unsigned remaining = state._State;
        char32_t value = state._Wchar;
        if (remaining == 0)
        {
            const auto first = static_cast<unsigned char>(source[consumed++]);
            if (first < 0x80)
            {
                scalar = first;
                state = {};
                return first ? 1 : 0;
            }
            if (first >= 0xc0 && first <= 0xdf) length = 2;
            else if (first >= 0xe0 && first <= 0xef) length = 3;
            else if (first >= 0xf0 && first <= 0xf7) length = 4;
            else return InvalidSequence(state);
            remaining = length - 1;
            value = first & ((1u << (7 - length)) - 1);
        }
        else if (length < 2 || length > 4 || remaining >= length || value > 0x10ffff)
            return InvalidSequence(state);

        while (remaining && consumed < count)
        {
            const auto next = static_cast<unsigned char>(source[consumed++]);
            if ((next & 0xc0) != 0x80) return InvalidSequence(state);
            value = (value << 6) | (next & 0x3f);
            --remaining;
        }
        if (remaining)
        {
            state._Wchar = value;
            state._Byte = static_cast<unsigned short>(length);
            state._State = static_cast<unsigned short>(remaining);
            return Incomplete;
        }
        constexpr char32_t minimum[] = {0x80, 0x800, 0x10000};
        if (value < minimum[length - 2] || value > 0x10ffff || (value >= 0xd800 && value <= 0xdfff))
            return InvalidSequence(state);
        scalar = value;
        state = {};
        return consumed;
    }

    struct DecodedCharacter { size_t result; size_t consumed; char32_t scalar; };

    inline DecodedCharacter Decode(const char* source, size_t count, mbstate_t& state, _locale_t locale, bool utf8)
    {
        if (!source || !count || !*source) return {0, 0, 0};
        if (utf8)
        {
            char32_t scalar = 0;
            const auto result = DecodeUtf8(scalar, source, count, state);
            return {result, result, scalar};
        }
        const bool pending = state._Wchar != 0;
        const bool leadByte = ::_isctype_l(static_cast<unsigned char>(*source), _LEADBYTE, locale) != 0;
        if (!pending && leadByte && count < 2)
        {
            state._Wchar = static_cast<unsigned char>(*source);
            return {Incomplete, 0, 0};
        }
        wchar_t wide = 0;
        if (pending)
        {
            const char pair[] = {static_cast<char>(state._Wchar), *source};
            state = {};
            if (!::_isctype_l(static_cast<unsigned char>(pair[0]), _LEADBYTE, locale) ||
                ::MultiByteToWideChar(CrtLocaleCodePage(locale), MB_PRECOMPOSED | MB_ERR_INVALID_CHARS, pair, 2, &wide, 1) != 1)
                return {InvalidSequence(state), 0, 0};
            // UCRT reports the full DBCS width, including the retained lead
            // byte. Only one new byte was consumed by this call.
            return {2, 1, wide};
        }
        const int result = ::_mbtowc_l(&wide, source, leadByte ? 2 : 1, locale);
        if (result <= 0) return {InvalidSequence(state), 0, 0};
        const auto consumed = static_cast<size_t>(result);
        return {consumed, consumed, wide};
    }

    inline size_t DecodeOne(wchar_t* target, const char* source, size_t count, mbstate_t& state, _locale_t locale)
    {
        if (!source) return 0;
        const auto decoded = Decode(source, count, state, locale, CrtLocaleCodePage(locale) == CP_UTF8);
        if (target) *target = decoded.scalar > 0xffff ? L'\xfffd' : static_cast<wchar_t>(decoded.scalar);
        return decoded.result;
    }

    inline errno_t EncodeOne(int& written, char* target, wchar_t character, _locale_t locale)
    {
        if (character >= 0xd800 && character <= 0xdfff && CrtLocaleCodePage(locale) == CP_UTF8)
        {
            written = -1;
            errno = EILSEQ;
            return EILSEQ;
        }
        return ::_wctomb_s_l(&written, target, MB_LEN_MAX, character, locale);
    }

    inline errno_t InvalidParameter()
    {
        ReportCrtInvalidParameter(nullptr, nullptr, nullptr, 0, 0);
        errno = EINVAL;
        return EINVAL;
    }

    inline size_t DecodeString(wchar_t* target, const char** source, size_t count, mbstate_t& state, _locale_t locale)
    {
        if (!source || !*source) { InvalidParameter(); return Invalid; }
        const bool utf8 = CrtLocaleCodePage(locale) == CP_UTF8;
        const char* cursor = *source;
        auto nextState = state;
        size_t written = 0;
        while (!target || written < count)
        {
            const auto decoded = Decode(cursor, 4, nextState, locale, utf8);
            if (decoded.result == Invalid || decoded.result == Incomplete)
            {
                if (target) *source = cursor;
                state = {};
                errno = EILSEQ;
                return Invalid;
            }
            if (decoded.result == 0)
            {
                if (target)
                {
                    target[written] = L'\0';
                    *source = nullptr;
                    state = {};
                }
                return written;
            }
            const size_t units = decoded.scalar > 0xffff ? 2 : 1;
            if (target && count - written < units) break;
            if (target)
            {
                if (units == 1) target[written] = static_cast<wchar_t>(decoded.scalar);
                else
                {
                    const auto supplementary = decoded.scalar - 0x10000;
                    target[written] = static_cast<wchar_t>(0xd800 + (supplementary >> 10));
                    target[written + 1] = static_cast<wchar_t>(0xdc00 + (supplementary & 0x3ff));
                }
                state = nextState;
            }
            written += units;
            cursor += decoded.consumed;
        }
        *source = cursor;
        return written;
    }

    inline size_t EncodeString(char* target, const wchar_t** source, size_t count, mbstate_t* state, _locale_t locale)
    {
        if (!source || !*source) { InvalidParameter(); return Invalid; }
        const bool utf8 = CrtLocaleCodePage(locale) == CP_UTF8;
        const wchar_t* cursor = *source;
        size_t written = 0;
        while (!target || written < count)
        {
            if (!*cursor)
            {
                if (target)
                {
                    target[written] = '\0';
                    *source = nullptr;
                    if (state) *state = {};
                }
                return written;
            }
            char bytes[MB_LEN_MAX];
            int byteCount = 0;
            size_t units = 1;
            if (utf8 && *cursor >= 0xd800 && *cursor <= 0xdbff)
            {
                if (cursor[1] >= 0xdc00 && cursor[1] <= 0xdfff)
                {
                    units = 2;
                    byteCount = ::WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, cursor, 2, bytes, sizeof(bytes), nullptr, nullptr);
                }
            }
            else if (EncodeOne(byteCount, bytes, *cursor, locale) != 0) byteCount = 0;
            if (byteCount <= 0)
            {
                if (target) *source = cursor;
                if (state) *state = {};
                errno = EILSEQ;
                return Invalid;
            }
            const auto size = static_cast<size_t>(byteCount);
            if (target && count - written < size) break;
            if (target)
            {
                std::memcpy(target + written, bytes, size);
                if (state) *state = {};
            }
            written += size;
            cursor += units;
        }
        *source = cursor;
        return written;
    }

    template<class Character, class Source, class Convert>
    inline errno_t ConvertStringSecure(size_t* result, Character* target, size_t capacity,
        Source source, size_t count, Convert convert)
    {
        if (result) *result = Invalid;
        if ((target == nullptr) != (capacity == 0)) return InvalidParameter();
        if (target) target[0] = 0;
        const size_t written = convert(target, source, (std::min)(count, capacity));
        if (written == Invalid)
        {
            if (target) target[0] = 0;
            return errno;
        }
        if (target && written >= capacity)
        {
            target[0] = 0;
            errno = ERANGE;
            return ERANGE;
        }
        if (target) target[written] = 0;
        if (result) *result = written + 1;
        return 0;
    }
}

inline size_t __cdecl CrtRestartable_mbrtowc(wchar_t* target, const char* source, size_t count, mbstate_t* state)
{
    mbstate_t local{};
    auto& selected = *CrtRestartableDetail::SelectState(state, local, &CrtLocaleThreadState::restartableCharacter);
    return CrtRestartableDetail::DecodeOne(target, source, count, selected, CurrentCrtLocaleHandle());
}

inline size_t __cdecl CrtRestartable_mbrlen(const char* source, size_t count, mbstate_t* state)
{
    mbstate_t local{};
    auto& selected = *CrtRestartableDetail::SelectState(state, local, &CrtLocaleThreadState::restartableLength);
    return CrtRestartableDetail::DecodeOne(nullptr, source, count, selected, CurrentCrtLocaleHandle());
}

inline int __cdecl CrtRestartable_mbsinit(const mbstate_t* state)
{
    return !state || state->_Wchar == 0;
}

inline wint_t __cdecl CrtRestartable_btowc(int value)
{
    if (value == EOF) return WEOF;
    const auto byte = static_cast<char>(value);
    mbstate_t state{};
    wchar_t wide = 0;
    const auto result = CrtRestartableDetail::DecodeOne(&wide, &byte, 1, state, CurrentCrtLocaleHandle());
    return result <= 1 ? wide : WEOF;
}

inline int __cdecl CrtRestartable_wctob(wint_t value)
{
    if (value == WEOF) return EOF;
    char bytes[MB_LEN_MAX];
    int written = 0;
    return CrtRestartableDetail::EncodeOne(written, bytes, value, CurrentCrtLocaleHandle()) == 0 && written == 1
        ? static_cast<signed char>(bytes[0]) : EOF;
}

inline errno_t __cdecl CrtRestartable_wcrtomb_s(size_t* result, char* target, size_t capacity, wchar_t value, mbstate_t* state)
{
    using namespace CrtRestartableDetail;
    if (result) *result = Invalid;
    if (!target && capacity) return InvalidParameter();
    if (state) *state = {};
    char bytes[MB_LEN_MAX];
    int written = 0;
    const auto error = EncodeOne(written, bytes, value, CurrentCrtLocaleHandle());
    if (error != 0) return error;
    if (target)
    {
        if (static_cast<size_t>(written) > capacity) { errno = ERANGE; return ERANGE; }
        std::memcpy(target, bytes, written);
    }
    if (result) *result = static_cast<size_t>(written);
    return 0;
}

inline size_t __cdecl CrtRestartable_wcrtomb(char* target, wchar_t value, mbstate_t* state)
{
    size_t result = CrtRestartableDetail::Invalid;
    CrtRestartable_wcrtomb_s(&result, target, target ? MB_LEN_MAX : 0, value, state);
    return result;
}

inline size_t __cdecl CrtRestartable_mbsrtowcs(wchar_t* target, const char** source, size_t count, mbstate_t* state)
{
    mbstate_t local{};
    auto& selected = *CrtRestartableDetail::SelectState(state, local, &CrtLocaleThreadState::restartableString);
    return CrtRestartableDetail::DecodeString(target, source, count, selected, CurrentCrtLocaleHandle());
}

inline size_t __cdecl CrtRestartable_wcsrtombs(char* target, const wchar_t** source, size_t count, mbstate_t* state)
{
    return CrtRestartableDetail::EncodeString(target, source, count, state, CurrentCrtLocaleHandle());
}

inline errno_t __cdecl CrtRestartable_mbsrtowcs_s(size_t* result, wchar_t* target, size_t capacity,
    const char** source, size_t count, mbstate_t* state)
{
    return CrtRestartableDetail::ConvertStringSecure(result, target, capacity, source, count,
        [state](wchar_t* output, const char** input, size_t limit) { return CrtRestartable_mbsrtowcs(output, input, limit, state); });
}

inline errno_t __cdecl CrtRestartable_wcsrtombs_s(size_t* result, char* target, size_t capacity,
    const wchar_t** source, size_t count, mbstate_t* state)
{
    return CrtRestartableDetail::ConvertStringSecure(result, target, capacity, source, count,
        [state](char* output, const wchar_t** input, size_t limit) { return CrtRestartable_wcsrtombs(output, input, limit, state); });
}

// UCRT's char16_t/char32_t entry points use UTF-8 independently of LC_CTYPE.
// Pass an explicit state so native fallback state is never shared with guests.
inline size_t __cdecl CrtRestartable_mbrtoc16(char16_t* target, const char* source, size_t count, mbstate_t* state)
{
    mbstate_t fallback{};
    return ::mbrtoc16(target, source, count,
        CrtRestartableDetail::SelectState(state, fallback, &CrtLocaleThreadState::utf8ToUtf16));
}

inline size_t __cdecl CrtRestartable_mbrtoc32(char32_t* target, const char* source, size_t count, mbstate_t* state)
{
    mbstate_t fallback{};
    return ::mbrtoc32(target, source, count,
        CrtRestartableDetail::SelectState(state, fallback, &CrtLocaleThreadState::utf8ToUtf32));
}

inline size_t __cdecl CrtRestartable_c16rtomb(char* target, char16_t value, mbstate_t* state)
{
    mbstate_t fallback{};
    return ::c16rtomb(target, value,
        CrtRestartableDetail::SelectState(state, fallback, &CrtLocaleThreadState::utf16ToUtf8));
}

inline size_t __cdecl CrtRestartable_c32rtomb(char* target, char32_t value, mbstate_t* state)
{
    mbstate_t fallback{};
    return ::c32rtomb(target, value,
        CrtRestartableDetail::SelectState(state, fallback, &CrtLocaleThreadState::utf32ToUtf8));
}
} }
