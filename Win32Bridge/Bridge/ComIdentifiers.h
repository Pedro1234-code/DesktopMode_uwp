#pragma once
#include <windows.h>
#include <cwchar>

namespace Win32Bridge { namespace Bridge {

inline bool ParseGuestGuid(const wchar_t* text, GUID* result)
{
    if (!text || !result || wcsnlen_s(text, 39) != 38 || text[0] != L'{' || text[37] != L'}' ||
        text[9] != L'-' || text[14] != L'-' || text[19] != L'-' || text[24] != L'-') return false;
    BYTE bytes[16]{};
    unsigned digit = 0;
    for (size_t index = 1; index < 37; ++index)
    {
        if (index == 9 || index == 14 || index == 19 || index == 24) continue;
        const wchar_t character = text[index];
        unsigned value = 0;
        if (character >= L'0' && character <= L'9') value = character - L'0';
        else if (character >= L'a' && character <= L'f') value = character - L'a' + 10;
        else if (character >= L'A' && character <= L'F') value = character - L'A' + 10;
        else return false;
        bytes[digit / 2] = static_cast<BYTE>((bytes[digit / 2] << 4) | value);
        ++digit;
    }
    GUID parsed{};
    parsed.Data1 = (static_cast<ULONG>(bytes[0]) << 24) | (static_cast<ULONG>(bytes[1]) << 16) |
        (static_cast<ULONG>(bytes[2]) << 8) | bytes[3];
    parsed.Data2 = static_cast<USHORT>((bytes[4] << 8) | bytes[5]);
    parsed.Data3 = static_cast<USHORT>((bytes[6] << 8) | bytes[7]);
    for (size_t index = 0; index < 8; ++index) parsed.Data4[index] = bytes[index + 8];
    *result = parsed;
    return true;
}

inline int FormatGuestGuid(REFGUID guid, wchar_t* buffer, int characters)
{
    if (!buffer || characters < 39) return 0;
    const int written = swprintf_s(buffer, static_cast<size_t>(characters),
        L"{%08lX-%04X-%04X-%02X%02X-%02X%02X%02X%02X%02X%02X}",
        guid.Data1, static_cast<unsigned>(guid.Data2), static_cast<unsigned>(guid.Data3),
        static_cast<unsigned>(guid.Data4[0]), static_cast<unsigned>(guid.Data4[1]),
        static_cast<unsigned>(guid.Data4[2]), static_cast<unsigned>(guid.Data4[3]),
        static_cast<unsigned>(guid.Data4[4]), static_cast<unsigned>(guid.Data4[5]),
        static_cast<unsigned>(guid.Data4[6]), static_cast<unsigned>(guid.Data4[7]));
    return written == 38 ? 39 : 0;
}

} }