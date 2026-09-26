#pragma once

#include <windows.h>

#include <string>
#include <vector>

namespace Win32Bridge
{
namespace Bridge
{
    struct ImportedSymbol
    {
        std::wstring library;
        std::wstring name;
        WORD ordinal = 0;
        bool importedByOrdinal = false;
        DWORD iatRva = 0;
    };

    struct ExportedSymbol
    {
        std::wstring name;
        DWORD ordinal = 0;
        DWORD rva = 0;
        // Forwarded exports are represented explicitly rather than treated as
        // executable RVAs. The module loader can resolve them later.
        std::wstring forwarder;
    };

    struct PeImageInfo
    {
        bool valid = false;
        std::wstring error;
        WORD machine = 0;
        DWORD entryPointRva = 0;
        std::vector<ImportedSymbol> imports;
        std::vector<ExportedSymbol> exports;
    };

    // Reads metadata only. It never maps the image or executes guest code.
    class PeImage final
    {
    public:
        static bool Inspect(const BYTE* bytes, size_t byteCount, PeImageInfo* result);
    };
}
}
