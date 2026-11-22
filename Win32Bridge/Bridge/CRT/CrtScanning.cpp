#include "Bridge/CRT/CrtScanning.h"
#include "Bridge/CRT/CrtLocaleData.h"
#include <cstdint>
#include <algorithm>
#include <climits>
#include <cstring>
#include <cwchar>
#include <clocale>
#include <limits>
#include <mbctype.h>
#include <string>
#include <type_traits>
#include <vector>

namespace Win32Bridge { namespace Bridge {
namespace {
    void __cdecl ReturnInvalidScanToGuest(const wchar_t*, const wchar_t*, const wchar_t*, unsigned, uintptr_t) { }
    template<class Character> struct NativeScan;
    template<> struct NativeScan<char>
    {
        static int Invoke(unsigned __int64 options, const char* buffer, size_t count, const char* format, void* locale, va_list args)
        { return ::__stdio_common_vsscanf(options, buffer, count, format, static_cast<_locale_t>(locale), args); }
    };
    template<> struct NativeScan<wchar_t>
    {
        static int Invoke(unsigned __int64 options, const wchar_t* buffer, size_t count, const wchar_t* format, void* locale, va_list args)
        { return ::__stdio_common_vswscanf(options, buffer, count, format, static_cast<_locale_t>(locale), args); }
    };
    template<class Character> int GuardedScan(unsigned __int64 options, const Character* buffer, size_t count,
        const Character* format, void* locale, va_list arguments)
    {
        if(!locale)locale=CurrentCrtLocaleHandle();
        const auto previous = ::_set_thread_local_invalid_parameter_handler(&ReturnInvalidScanToGuest);
        va_list copied; va_copy(copied, arguments);
        int result = -1;
        // No C++ objects requiring unwinding in this SEH leaf. Restore the
        // host handler even when a guest input/destination pointer faults.
        __try
        {
            result = NativeScan<Character>::Invoke(options, buffer, count, format, locale, copied);
        }
        __finally
        {
            va_end(copied);
            ::_set_thread_local_invalid_parameter_handler(previous);
        }
        return result;
    }
}
int ScanCrtBuffer(unsigned __int64 options, const char* buffer, size_t count,
    const char* format, void* locale, va_list arguments)
{ return GuardedScan(options, buffer, count, format, locale, arguments); }
int ScanCrtBuffer(unsigned __int64 options, const wchar_t* buffer, size_t count,
    const wchar_t* format, void* locale, va_list arguments)
{ return GuardedScan(options, buffer, count, format, locale, arguments); }
} }
#include "Bridge/CRT/CrtStreamScanning.inl"
