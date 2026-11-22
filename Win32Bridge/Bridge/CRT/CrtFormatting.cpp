#include "Bridge/CRT/CrtFormatting.h"
#include "Bridge/CRT/CrtLocaleData.h"
#include <cstdint>

namespace Win32Bridge { namespace Bridge {
namespace {
    void __cdecl ReturnInvalidFormatToGuest(const wchar_t*, const wchar_t*, const wchar_t*, unsigned, uintptr_t) { }

    template<class Character> struct NativeFormat;
    template<> struct NativeFormat<char>
    {
        static int Invoke(unsigned __int64 options, char* buffer, size_t count, size_t maximum,
            const char* format, void* locale, va_list args, CrtFormatFlavor flavor)
        {
            switch (flavor)
            {
            case CrtFormatFlavor::Secure: return ::__stdio_common_vsprintf_s(options, buffer, count, format, static_cast<_locale_t>(locale), args);
            case CrtFormatFlavor::SecureTruncate: return ::__stdio_common_vsnprintf_s(options, buffer, count, maximum, format, static_cast<_locale_t>(locale), args);
            case CrtFormatFlavor::Positional: return ::__stdio_common_vsprintf_p(options, buffer, count, format, static_cast<_locale_t>(locale), args);
            default: return ::__stdio_common_vsprintf(options, buffer, count, format, static_cast<_locale_t>(locale), args);
            }
        }
    };
    template<> struct NativeFormat<wchar_t>
    {
        static int Invoke(unsigned __int64 options, wchar_t* buffer, size_t count, size_t maximum,
            const wchar_t* format, void* locale, va_list args, CrtFormatFlavor flavor)
        {
            switch (flavor)
            {
            case CrtFormatFlavor::Secure: return ::__stdio_common_vswprintf_s(options, buffer, count, format, static_cast<_locale_t>(locale), args);
            case CrtFormatFlavor::SecureTruncate: return ::__stdio_common_vsnwprintf_s(options, buffer, count, maximum, format, static_cast<_locale_t>(locale), args);
            case CrtFormatFlavor::Positional: return ::__stdio_common_vswprintf_p(options, buffer, count, format, static_cast<_locale_t>(locale), args);
            default: return ::__stdio_common_vswprintf(options, buffer, count, format, static_cast<_locale_t>(locale), args);
            }
        }
    };

    template<class Character> int GuardedFormat(unsigned __int64 options, Character* buffer, size_t count, size_t maximum,
        const Character* format, void* locale, va_list arguments, CrtFormatFlavor flavor)
    {
        if(!locale)locale=CurrentCrtLocaleHandle();
        const auto previous = ::_set_thread_local_invalid_parameter_handler(&ReturnInvalidFormatToGuest);
        va_list copied; va_copy(copied, arguments);
        int result = -1;
        // Keep this leaf free of C++ objects requiring unwinding. __finally
        // also restores the host handler when a guest argument faults.
        __try
        {
            if (CrtFormatContainsCountWrite(format))
            {
                if (buffer && count && (flavor == CrtFormatFlavor::Secure || flavor == CrtFormatFlavor::SecureTruncate)) buffer[0] = 0;
                errno = EINVAL;
            }
            else result = NativeFormat<Character>::Invoke(options, buffer, count, maximum, format, locale, copied, flavor);
        }
        __finally
        {
            va_end(copied);
            ::_set_thread_local_invalid_parameter_handler(previous);
        }
        return result;
    }
}
int FormatCrtBuffer(unsigned __int64 options, char* buffer, size_t count, size_t maximum,
    const char* format, void* locale, va_list arguments, CrtFormatFlavor flavor)
{ return GuardedFormat(options, buffer, count, maximum, format, locale, arguments, flavor); }
int FormatCrtBuffer(unsigned __int64 options, wchar_t* buffer, size_t count, size_t maximum,
    const wchar_t* format, void* locale, va_list arguments, CrtFormatFlavor flavor)
{ return GuardedFormat(options, buffer, count, maximum, format, locale, arguments, flavor); }
} }