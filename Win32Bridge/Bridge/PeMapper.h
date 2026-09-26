#pragma once

#include <windows.h>

#include <string>
#include <vector>

namespace Win32Bridge
{
namespace Bridge
{
    struct MappedSection
    {
        DWORD rva = 0;
        DWORD size = 0;
        DWORD characteristics = 0;
    };

    struct MappedPeImage
    {
        ULONGLONG preferredImageBase = 0;
        DWORD entryPointRva = 0;
        std::vector<BYTE> bytes;
        std::vector<MappedSection> sections;
    };

    // Builds the PE's in-memory section layout without granting execute access.
    // A later runtime layer will own allocation protection and entry-point calls.
    class PeMapper final
    {
    public:
        static bool Materialize(const BYTE* fileBytes, size_t fileSize, MappedPeImage* image, std::wstring* error);
        static bool ApplyBaseRelocations(MappedPeImage* image, ULONGLONG actualImageBase, std::wstring* error);
    };
}
}
