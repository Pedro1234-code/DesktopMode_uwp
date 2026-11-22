#pragma once
#include <cstdio>
#include <cstdlib>
#include <cstdarg>
#include <cerrno>
#include <corecrt_stdio_config.h>

namespace Win32Bridge { namespace Bridge {
constexpr unsigned __int64 CrtNamedUcrtScanOptions = _CRT_INTERNAL_SCANF_LEGACY_WIDE_SPECIFIERS;
constexpr unsigned __int64 CrtNamedMsvcrtScanOptions = _CRT_INTERNAL_SCANF_LEGACY_WIDE_SPECIFIERS | _CRT_INTERNAL_SCANF_LEGACY_MSVCRT_COMPATIBILITY;

// The native scanner sees only caller-owned memory, never a guest FILE.
int ScanCrtBuffer(unsigned __int64 options, const char* buffer, size_t count,
    const char* format, void* locale, va_list arguments);
int ScanCrtBuffer(unsigned __int64 options, const wchar_t* buffer, size_t count,
    const wchar_t* format, void* locale, va_list arguments);
// Character transport for fscanf. EOF is -1; an embedded NUL is ordinary
// input. Implementations own the stream lock and never expose a host FILE.
class CrtScanInput
{
public:
    virtual ~CrtScanInput() = default;
    virtual int Get() = 0;
    virtual void Unget(int character) = 0;
};
int ScanCrtStream(unsigned __int64 options, CrtScanInput& input,
    const char* format, void* locale, va_list arguments);
int ScanCrtStream(unsigned __int64 options, CrtScanInput& input,
    const wchar_t* format, void* locale, va_list arguments);
} }