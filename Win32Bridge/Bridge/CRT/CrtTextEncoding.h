#pragma once
#include <windows.h>
#include <cstddef>

namespace Win32Bridge { namespace Bridge {

enum class CrtTextEncoding { Ansi, Utf8, Utf16LittleEndian };

inline unsigned CrtUtf8SequenceSize(unsigned char lead)
{
    if (lead >= 0xc2 && lead <= 0xdf) return 2;
    if (lead >= 0xe0 && lead <= 0xef) return 3;
    if (lead >= 0xf0 && lead <= 0xf4) return 4;
    return 1;
}

// Keep a partial UTF-8 sequence for the next physical read. Invalid complete
// sequences are left to the same Windows converter used by the UCRT.
inline size_t CompleteCrtUtf8Prefix(const unsigned char* bytes, size_t length)
{
    if (!length) return 0;
    size_t lead = length - 1;
    unsigned continuations = 0;
    while (lead && (bytes[lead] & 0xc0) == 0x80 && continuations < 3) { --lead; ++continuations; }
    const size_t expected = CrtUtf8SequenceSize(bytes[lead]);
    return length - lead < expected ? lead : length;
}

inline size_t CompleteCrtDbcsPrefix(const unsigned char* bytes, size_t length, const CPINFO& page)
{
    size_t offset = 0;
    while (offset < length)
    {
        bool lead = false;
        for (size_t range = 0; range + 1 < sizeof(page.LeadByte) && page.LeadByte[range]; range += 2)
        {
            if (bytes[offset] >= page.LeadByte[range] && bytes[offset] <= page.LeadByte[range + 1])
            {
                lead = true;
                break;
            }
        }
        if (lead && offset + 1 == length) break;
        offset += lead ? 2 : 1;
    }
    return offset;
}

} }
