#include "Bridge\\PeImage.h"

#include <algorithm>
#include <cstring>

using namespace Win32Bridge::Bridge;

namespace
{
    class ImageReader final
    {
    public:
        ImageReader(const BYTE* bytes, size_t count) : m_bytes(bytes), m_count(count) {}

        template <typename T>
        bool Read(size_t offset, T* value) const
        {
            if (!value || !Contains(offset, sizeof(T)))
            {
                return false;
            }

            memcpy(value, m_bytes + offset, sizeof(T));
            return true;
        }

        bool ReadAsciiString(size_t offset, std::wstring* value) const
        {
            if (!value || !Contains(offset, 1))
            {
                return false;
            }

            size_t end = offset;
            while (end < m_count && m_bytes[end] != 0)
            {
                ++end;
            }

            if (end == m_count)
            {
                return false;
            }

            value->assign(m_bytes + offset, m_bytes + end);
            return true;
        }

        bool Contains(size_t offset, size_t length) const
        {
            return offset <= m_count && length <= m_count - offset;
        }

    private:
        const BYTE* m_bytes;
        size_t m_count;
    };

    bool RvaToOffset(
        const ImageReader& reader,
        const IMAGE_OPTIONAL_HEADER64& optionalHeader,
        const std::vector<IMAGE_SECTION_HEADER>& sections,
        DWORD rva,
        size_t* offset)
    {
        if (!offset)
        {
            return false;
        }

        if (rva < optionalHeader.SizeOfHeaders)
        {
            if (!reader.Contains(rva, 1))
            {
                return false;
            }

            *offset = rva;
            return true;
        }

        for (const auto& section : sections)
        {
            const DWORD sectionSize = (std::max)(section.Misc.VirtualSize, section.SizeOfRawData);
            if (rva < section.VirtualAddress || rva - section.VirtualAddress >= sectionSize)
            {
                continue;
            }

            const DWORD delta = rva - section.VirtualAddress;
            if (delta >= section.SizeOfRawData)
            {
                return false;
            }

            const size_t candidate = static_cast<size_t>(section.PointerToRawData) + delta;
            if (!reader.Contains(candidate, 1))
            {
                return false;
            }

            *offset = candidate;
            return true;
        }

        return false;
    }

    void SetError(PeImageInfo* result, const wchar_t* error)
    {
        result->valid = false;
        result->error = error;
    }

    bool IsRvaInDirectory(const IMAGE_DATA_DIRECTORY& directory, DWORD rva)
    {
        return directory.VirtualAddress != 0 &&
            rva >= directory.VirtualAddress &&
            static_cast<ULONGLONG>(rva) < static_cast<ULONGLONG>(directory.VirtualAddress) + directory.Size;
    }

    struct DelayImportDescriptor final
    {
        DWORD attributes;
        DWORD name;
        DWORD moduleHandle;
        DWORD importAddressTable;
        DWORD importNameTable;
        DWORD boundImportAddressTable;
        DWORD unloadImportAddressTable;
        DWORD timeStamp;
    };

    bool DelayFieldToRva(
        DWORD value,
        bool fieldsAreRvas,
        ULONGLONG imageBase,
        DWORD* rva)
    {
        if (!rva) return false;
        if (value == 0)
        {
            *rva = 0;
            return true;
        }
        if (fieldsAreRvas)
        {
            *rva = value;
            return true;
        }
        if (value < imageBase || static_cast<ULONGLONG>(value) - imageBase > MAXDWORD)
            return false;
        *rva = static_cast<DWORD>(static_cast<ULONGLONG>(value) - imageBase);
        return true;
    }

    bool AppendImportThunks(
        const ImageReader& reader,
        const IMAGE_OPTIONAL_HEADER64& optionalHeader,
        const std::vector<IMAGE_SECTION_HEADER>& sections,
        const std::wstring& library,
        DWORD lookupTableRva,
        DWORD addressTableRva,
        bool delayLoaded,
        bool thunkValuesAreVas,
        std::vector<ImportedSymbol>* imports,
        PeImageInfo* result)
    {
        size_t thunkOffset = 0;
        if (!imports || lookupTableRva == 0 || addressTableRva == 0 ||
            !RvaToOffset(reader, optionalHeader, sections, lookupTableRva, &thunkOffset))
        {
            SetError(result, delayLoaded
                ? L"A delay-import thunk points outside the image."
                : L"An import thunk points outside the image.");
            return false;
        }

        for (size_t thunkIndex = 0; thunkIndex < 65536; ++thunkIndex)
        {
            IMAGE_THUNK_DATA64 thunk{};
            const size_t currentThunkOffset = thunkOffset + thunkIndex * sizeof(thunk);
            if (!reader.Read(currentThunkOffset, &thunk))
            {
                SetError(result, delayLoaded
                    ? L"A delay-import thunk table is truncated."
                    : L"An import thunk table is truncated.");
                return false;
            }
            if (thunk.u1.AddressOfData == 0) return true;

            ImportedSymbol symbol{};
            symbol.library = library;
            symbol.delayLoaded = delayLoaded;
            const ULONGLONG iatRva = static_cast<ULONGLONG>(addressTableRva) +
                thunkIndex * sizeof(ULONGLONG);
            if (iatRva > MAXDWORD)
            {
                SetError(result, L"An import address table entry exceeds the PE address space.");
                return false;
            }
            symbol.iatRva = static_cast<DWORD>(iatRva);
            symbol.importedByOrdinal = (thunk.u1.Ordinal & IMAGE_ORDINAL_FLAG64) != 0;
            if (symbol.importedByOrdinal)
            {
                symbol.ordinal = static_cast<WORD>(thunk.u1.Ordinal & 0xffff);
            }
            else
            {
                ULONGLONG nameValue = thunk.u1.AddressOfData;
                if (thunkValuesAreVas)
                {
                    if (nameValue < optionalHeader.ImageBase ||
                        nameValue - optionalHeader.ImageBase > MAXDWORD)
                    {
                        SetError(result, L"A delay-import name has an invalid address.");
                        return false;
                    }
                    nameValue -= optionalHeader.ImageBase;
                }
                size_t nameOffset = 0;
                if (nameValue > MAXDWORD ||
                    !RvaToOffset(reader, optionalHeader, sections,
                        static_cast<DWORD>(nameValue), &nameOffset) ||
                    !reader.Contains(nameOffset, sizeof(WORD)) ||
                    !reader.ReadAsciiString(nameOffset + sizeof(WORD), &symbol.name))
                {
                    SetError(result, delayLoaded
                        ? L"A delay-import function name is malformed."
                        : L"An imported function name is malformed.");
                    return false;
                }
            }
            imports->push_back(std::move(symbol));
        }

        SetError(result, delayLoaded
            ? L"A delay-import thunk table has no terminator."
            : L"An import thunk table has no terminator.");
        return false;
    }
}

bool PeImage::Inspect(const BYTE* bytes, size_t byteCount, PeImageInfo* result)
{
    if (!result)
    {
        return false;
    }

    *result = PeImageInfo{};
    if (!bytes || byteCount == 0)
    {
        SetError(result, L"The guest file is empty.");
        return false;
    }

    const ImageReader reader(bytes, byteCount);
    IMAGE_DOS_HEADER dosHeader{};
    if (!reader.Read(0, &dosHeader) || dosHeader.e_magic != IMAGE_DOS_SIGNATURE || dosHeader.e_lfanew < 0)
    {
        SetError(result, L"The guest is not a valid DOS/PE image.");
        return false;
    }

    const size_t ntOffset = static_cast<size_t>(dosHeader.e_lfanew);
    DWORD signature = 0;
    IMAGE_FILE_HEADER fileHeader{};
    if (!reader.Read(ntOffset, &signature) || signature != IMAGE_NT_SIGNATURE ||
        !reader.Read(ntOffset + sizeof(signature), &fileHeader))
    {
        SetError(result, L"The PE header is truncated or invalid.");
        return false;
    }

    const size_t optionalOffset = ntOffset + sizeof(signature) + sizeof(fileHeader);
    WORD optionalMagic = 0;
    IMAGE_OPTIONAL_HEADER64 optionalHeader{};
    if (!reader.Read(optionalOffset, &optionalMagic) || optionalMagic != IMAGE_NT_OPTIONAL_HDR64_MAGIC ||
        fileHeader.SizeOfOptionalHeader < sizeof(optionalHeader) ||
        !reader.Read(optionalOffset, &optionalHeader))
    {
        SetError(result, L"Only complete PE32+ (64-bit) images are supported by this bootstrap.");
        return false;
    }

    const size_t sectionsOffset = optionalOffset + fileHeader.SizeOfOptionalHeader;
    std::vector<IMAGE_SECTION_HEADER> sections;
    sections.reserve(fileHeader.NumberOfSections);
    for (WORD index = 0; index < fileHeader.NumberOfSections; ++index)
    {
        IMAGE_SECTION_HEADER section{};
        const size_t offset = sectionsOffset + static_cast<size_t>(index) * sizeof(section);
        if (!reader.Read(offset, &section))
        {
            SetError(result, L"The PE section table is truncated.");
            return false;
        }

        sections.push_back(section);
    }

    result->machine = fileHeader.Machine;
    result->entryPointRva = optionalHeader.AddressOfEntryPoint;
    if (fileHeader.Machine != IMAGE_FILE_MACHINE_AMD64)
    {
        SetError(result, L"This guest is not x64. Xbox UWP guests must match the x64 host architecture.");
        return false;
    }

    const IMAGE_DATA_DIRECTORY exportDirectory = optionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXPORT];
    if (exportDirectory.VirtualAddress != 0 && exportDirectory.Size != 0)
    {
        size_t exportOffset = 0;
        IMAGE_EXPORT_DIRECTORY exports{};
        if (!RvaToOffset(reader, optionalHeader, sections, exportDirectory.VirtualAddress, &exportOffset) ||
            !reader.Read(exportOffset, &exports))
        {
            SetError(result, L"The PE export directory points outside the image.");
            return false;
        }
        if (exports.NumberOfFunctions > 65536 || exports.NumberOfNames > exports.NumberOfFunctions)
        {
            SetError(result, L"The PE export directory has invalid entry counts.");
            return false;
        }

        result->exports.resize(exports.NumberOfFunctions);
        for (DWORD index = 0; index < exports.NumberOfFunctions; ++index)
        {
            size_t functionOffset = 0;
            DWORD functionRva = 0;
            const DWORD functionTableRva = exports.AddressOfFunctions + index * sizeof(DWORD);
            if (functionTableRva < exports.AddressOfFunctions ||
                !RvaToOffset(reader, optionalHeader, sections, functionTableRva, &functionOffset) ||
                !reader.Read(functionOffset, &functionRva))
            {
                SetError(result, L"The PE export address table is malformed.");
                return false;
            }

            ExportedSymbol& symbol = result->exports[index];
            symbol.ordinal = exports.Base + index;
            symbol.rva = functionRva;
            if (functionRva != 0 && IsRvaInDirectory(exportDirectory, functionRva))
            {
                size_t forwarderOffset = 0;
                if (!RvaToOffset(reader, optionalHeader, sections, functionRva, &forwarderOffset) ||
                    !reader.ReadAsciiString(forwarderOffset, &symbol.forwarder))
                {
                    SetError(result, L"A PE export forwarder is malformed.");
                    return false;
                }
            }
        }

        for (DWORD index = 0; index < exports.NumberOfNames; ++index)
        {
            size_t nameRvaOffset = 0;
            size_t ordinalOffset = 0;
            DWORD nameRva = 0;
            WORD ordinalIndex = 0;
            const DWORD nameTableRva = exports.AddressOfNames + index * sizeof(DWORD);
            const DWORD ordinalTableRva = exports.AddressOfNameOrdinals + index * sizeof(WORD);
            if (nameTableRva < exports.AddressOfNames || ordinalTableRva < exports.AddressOfNameOrdinals ||
                !RvaToOffset(reader, optionalHeader, sections, nameTableRva, &nameRvaOffset) ||
                !reader.Read(nameRvaOffset, &nameRva) ||
                !RvaToOffset(reader, optionalHeader, sections, ordinalTableRva, &ordinalOffset) ||
                !reader.Read(ordinalOffset, &ordinalIndex) || ordinalIndex >= result->exports.size())
            {
                SetError(result, L"The PE export name table is malformed.");
                return false;
            }

            size_t nameOffset = 0;
            if (!RvaToOffset(reader, optionalHeader, sections, nameRva, &nameOffset) ||
                !reader.ReadAsciiString(nameOffset, &result->exports[ordinalIndex].name))
            {
                SetError(result, L"A PE export name is malformed.");
                return false;
            }
        }
    }

    const IMAGE_DATA_DIRECTORY importDirectory = optionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    if (importDirectory.VirtualAddress != 0 && importDirectory.Size != 0)
    {
        size_t importOffset = 0;
        if (!RvaToOffset(reader, optionalHeader, sections, importDirectory.VirtualAddress, &importOffset))
        {
            SetError(result, L"The PE import directory points outside the image.");
            return false;
        }

        bool terminated = false;
        const size_t maximumDescriptors = importDirectory.Size / sizeof(IMAGE_IMPORT_DESCRIPTOR) + 1;
        for (size_t descriptorIndex = 0; descriptorIndex < maximumDescriptors; ++descriptorIndex)
        {
            IMAGE_IMPORT_DESCRIPTOR descriptor{};
            const size_t descriptorOffset = importOffset + descriptorIndex * sizeof(descriptor);
            if (!reader.Read(descriptorOffset, &descriptor))
            {
                SetError(result, L"The PE import descriptor table is truncated.");
                return false;
            }
            if (descriptor.Name == 0 && descriptor.OriginalFirstThunk == 0 && descriptor.FirstThunk == 0)
            {
                terminated = true;
                break;
            }

            size_t libraryOffset = 0;
            std::wstring library;
            if (!RvaToOffset(reader, optionalHeader, sections, descriptor.Name, &libraryOffset) ||
                !reader.ReadAsciiString(libraryOffset, &library))
            {
                SetError(result, L"An imported DLL name is malformed.");
                return false;
            }
            const DWORD lookup = descriptor.OriginalFirstThunk
                ? descriptor.OriginalFirstThunk : descriptor.FirstThunk;
            if (!AppendImportThunks(reader, optionalHeader, sections, library,
                lookup, descriptor.FirstThunk, false, false, &result->imports, result))
                return false;
        }
        if (!terminated)
        {
            SetError(result, L"The import descriptor table has no terminator.");
            return false;
        }
    }

    const IMAGE_DATA_DIRECTORY delayDirectory =
        optionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_DELAY_IMPORT];
    if (delayDirectory.VirtualAddress != 0 && delayDirectory.Size != 0)
    {
        size_t delayOffset = 0;
        if (!RvaToOffset(reader, optionalHeader, sections,
            delayDirectory.VirtualAddress, &delayOffset))
        {
            SetError(result, L"The PE delay-import directory points outside the image.");
            return false;
        }

        bool terminated = false;
        const size_t maximumDescriptors = delayDirectory.Size / sizeof(DelayImportDescriptor) + 1;
        for (size_t descriptorIndex = 0; descriptorIndex < maximumDescriptors; ++descriptorIndex)
        {
            DelayImportDescriptor descriptor{};
            const size_t descriptorOffset = delayOffset + descriptorIndex * sizeof(descriptor);
            if (!reader.Read(descriptorOffset, &descriptor))
            {
                SetError(result, L"The PE delay-import descriptor table is truncated.");
                return false;
            }
            if (descriptor.attributes == 0 && descriptor.name == 0 &&
                descriptor.importAddressTable == 0 && descriptor.importNameTable == 0)
            {
                terminated = true;
                break;
            }
            if ((descriptor.attributes & ~1u) != 0)
            {
                SetError(result, L"The PE delay-import descriptor uses unsupported attributes.");
                return false;
            }

            const bool fieldsAreRvas = (descriptor.attributes & 1u) != 0;
            DWORD nameRva = 0;
            DWORD iatRva = 0;
            DWORD intRva = 0;
            if (!DelayFieldToRva(descriptor.name, fieldsAreRvas,
                    optionalHeader.ImageBase, &nameRva) ||
                !DelayFieldToRva(descriptor.importAddressTable, fieldsAreRvas,
                    optionalHeader.ImageBase, &iatRva) ||
                !DelayFieldToRva(descriptor.importNameTable, fieldsAreRvas,
                    optionalHeader.ImageBase, &intRva))
            {
                SetError(result, L"The PE delay-import descriptor contains an invalid VA.");
                return false;
            }
            if (intRva == 0) intRva = iatRva;

            size_t libraryOffset = 0;
            std::wstring library;
            if (!RvaToOffset(reader, optionalHeader, sections, nameRva, &libraryOffset) ||
                !reader.ReadAsciiString(libraryOffset, &library))
            {
                SetError(result, L"A delay-imported DLL name is malformed.");
                return false;
            }
            if (!AppendImportThunks(reader, optionalHeader, sections, library,
                intRva, iatRva, true, !fieldsAreRvas, &result->imports, result))
                return false;
        }
        if (!terminated)
        {
            SetError(result, L"The delay-import descriptor table has no terminator.");
            return false;
        }
    }

    result->valid = true;
    return true;
}
