#pragma once
#include "Bridge/CRT/CrtFileDescriptors.h"
#include <string>

namespace Win32Bridge { namespace Bridge {

struct CrtStdioMode
{
    int flags = 0;
    bool commit = false;
};
inline bool ParseCrtStdioMode(const wchar_t* mode, bool commitDefault, CrtStdioMode* result)
{
    if (!mode || !result) return false;
    CrtStdioMode parsed; parsed.commit = commitDefault;
    while (*mode == L' ') ++mode;
    switch (*mode++)
    {
    case L'r': parsed.flags = _O_RDONLY; break;
    case L'w': parsed.flags = _O_WRONLY | _O_CREAT | _O_TRUNC; break;
    case L'a': parsed.flags = _O_WRONLY | _O_CREAT | _O_APPEND; break;
    default: return false;
    }
    bool plus = false, translation = false, commit = false, scan = false, temporary = false, shortLived = false, exclusive = false;
    while (*mode && *mode != L',')
    {
        switch (*mode++)
        {
        case L' ': break;
        case L'+': if (plus) return false; plus = true; parsed.flags = (parsed.flags & ~3) | _O_RDWR; break;
        case L'b': if (translation) return false; translation = true; parsed.flags |= _O_BINARY; break;
        case L't': if (translation) return false; translation = true; parsed.flags |= _O_TEXT; break;
        case L'c': if (commit) return false; commit = true; parsed.commit = true; break;
        case L'n': if (commit) return false; commit = true; parsed.commit = false; break;
        case L'S': if (scan) return false; scan = true; parsed.flags |= _O_SEQUENTIAL; break;
        case L'R': if (scan) return false; scan = true; parsed.flags |= _O_RANDOM; break;
        case L'T': if (shortLived) return false; shortLived = true; parsed.flags |= _O_SHORT_LIVED; break;
        case L'D': if (temporary) return false; temporary = true; parsed.flags |= _O_TEMPORARY; break;
        case L'N': parsed.flags |= _O_NOINHERIT; break;
        case L'x': if (exclusive || !(parsed.flags & _O_TRUNC)) return false; exclusive = true; parsed.flags |= _O_EXCL; break;
        default: return false;
        }
    }
    if (*mode == L',')
    {
        ++mode; while (*mode == L' ') ++mode;
        if (wcsncmp(mode, L"ccs", 3)) return false;
        mode += 3; while (*mode == L' ') ++mode;
        if (*mode++ != L'=') return false;
        while (*mode == L' ') ++mode;
        if (!_wcsnicmp(mode, L"UTF-8", 5)) { parsed.flags |= _O_U8TEXT; mode += 5; }
        else if (!_wcsnicmp(mode, L"UTF-16LE", 8)) { parsed.flags |= _O_U16TEXT; mode += 8; }
        else if (!_wcsnicmp(mode, L"UNICODE", 7)) { parsed.flags |= _O_WTEXT; mode += 7; }
        else return false;
        while (*mode == L' ') ++mode;
        if (*mode) return false;
    }
    *result = parsed; return true;
}

// MSVCRT's public _iobuf layout, also usable as an opaque UCRT FILE pointer.
// Private buffering stays in the bridge: cnt=0 always routes legacy getc/putc
// macros through the exported _filbuf/_flsbuf adapters.
struct CrtLegacyFile
{
    char* pointer = nullptr;
    int count = 0;
    char* base = nullptr;
    int flags = 0;
    int descriptor = -1;
    int character = 0;
    int bufferSize = 0;
    char* temporaryName = nullptr;
};
static_assert(sizeof(CrtLegacyFile) == (sizeof(void*) == 8 ? 48 : 32), "MSVCRT FILE ABI mismatch");
static_assert(offsetof(CrtLegacyFile, flags) == (sizeof(void*) == 8 ? 24 : 12), "MSVCRT FILE flag offset mismatch");

} }