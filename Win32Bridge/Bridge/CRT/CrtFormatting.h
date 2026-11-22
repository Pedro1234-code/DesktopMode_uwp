#pragma once
#include <cstdio>
#include <cstdlib>
#include <cstdarg>
#include <cerrno>
#include <vector>
#include <new>
#include <type_traits>
#include <corecrt_stdio_config.h>

namespace Win32Bridge { namespace Bridge {

enum class CrtFormatFlavor { Standard, Secure, SecureTruncate, Positional };
constexpr unsigned __int64 CrtNamedUcrtFormatOptions = _CRT_INTERNAL_PRINTF_LEGACY_WIDE_SPECIFIERS;
constexpr unsigned __int64 CrtNamedMsvcrtFormatOptions = _CRT_INTERNAL_PRINTF_LEGACY_WIDE_SPECIFIERS |
    _CRT_INTERNAL_PRINTF_LEGACY_MSVCRT_COMPATIBILITY | _CRT_INTERNAL_PRINTF_LEGACY_THREE_DIGIT_EXPONENTS;

template<class Character> bool CrtFormatContainsCountWrite(const Character* format)
{
    if (!format) return false;
    for (size_t index = 0; format[index]; ++index)
    {
        if (format[index] != '%') continue;
        ++index;
        if (format[index] == '%') continue;
        for (; format[index]; ++index)
        {
            const auto value = format[index];
            if (value == 'n') return true;
            const char* conversions = "diouxXfFeEgGaAcCsSpZ%";
            bool converted = false;
            for (const char* it = conversions; *it; ++it) if (value == *it) { converted = true; break; }
            if (converted) break;
        }
        if (!format[index]) break;
    }
    return false;
}

// Only in-memory CRT APIs are invoked. A thread-local validation guard returns
// CRT errors to the guest and restores the host handler on every SEH path.
int FormatCrtBuffer(unsigned __int64 options, char* buffer, size_t capacity, size_t maximum,
    const char* format, void* locale, va_list arguments, CrtFormatFlavor flavor);
int FormatCrtBuffer(unsigned __int64 options, wchar_t* buffer, size_t capacity, size_t maximum,
    const wchar_t* format, void* locale, va_list arguments, CrtFormatFlavor flavor);

template<class Character> int FormatCrtText(unsigned __int64 options, const Character* format, void* locale,
    va_list arguments, CrtFormatFlavor flavor, std::vector<Character>* output)
{
    output->clear();
    const auto measurementFlavor = flavor == CrtFormatFlavor::Positional ? flavor : CrtFormatFlavor::Standard;
    const int required = FormatCrtBuffer(options | _CRT_INTERNAL_PRINTF_STANDARD_SNPRINTF_BEHAVIOR,
        static_cast<Character*>(nullptr), 0, 0, format, locale, arguments, measurementFlavor);
    if (required < 0) return -1;
    try
    {
        output->resize(static_cast<size_t>(required) + 1);
        const int written = FormatCrtBuffer(options | _CRT_INTERNAL_PRINTF_STANDARD_SNPRINTF_BEHAVIOR,
            output->data(), output->size(), output->size(), format, locale, arguments, flavor);
        if (written < 0 || written > required)
        {
            output->clear(); if (written > required) errno = ERANGE;
            return -1;
        }
        output->resize(static_cast<size_t>(written) + 1); return written;
    }
    catch (const std::bad_alloc&) { output->clear(); errno = ENOMEM; return -1; }
}

} }