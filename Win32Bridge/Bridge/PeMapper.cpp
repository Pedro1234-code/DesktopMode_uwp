#include "Bridge\\PeMapper.h"

#include <algorithm>
#include <cstring>

using namespace Win32Bridge::Bridge;

namespace
{
    bool Contains(size_t size, size_t offset, size_t length)
    {
        return offset <= size && length <= size - offset;
    }

    template <typename T>
    bool Read(const BYTE* bytes, size_t size, size_t offset, T* value)
    {
        if (!bytes || !value || !Contains(size, offset, sizeof(T)))
        {
            return false;
        }

        memcpy(value, bytes + offset, sizeof(T));
        return true;
    }

    void SetError(std::wstring* error, const wchar_t* message)
    {
        if (error)
        {
            *error = message;
        }
    }

    bool ReadOptionalHeader(
        const BYTE* bytes,
        size_t size,
        IMAGE_FILE_HEADER* fileHeader,
        IMAGE_OPTIONAL_HEADER64* optionalHeader,
        size_t* sectionOffset,
        std::wstring* error)
    {
        IMAGE_DOS_HEADER dosHeader{};
        if (!Read(bytes, size, 0, &dosHeader) || dosHeader.e_magic != IMAGE_DOS_SIGNATURE || dosHeader.e_lfanew < 0)
        {
            SetError(error, L"The image does not have a valid DOS header.");
            return false;
        }

        const size_t ntOffset = static_cast<size_t>(dosHeader.e_lfanew);
        DWORD signature = 0;
        if (!Read(bytes, size, ntOffset, &signature) || signature != IMAGE_NT_SIGNATURE ||
            !Read(bytes, size, ntOffset + sizeof(signature), fileHeader))
        {
            SetError(error, L"The image does not have a valid NT header.");
            return false;
        }

        const size_t optionalOffset = ntOffset + sizeof(signature) + sizeof(*fileHeader);
        WORD magic = 0;
        if (!Read(bytes, size, optionalOffset, &magic) || magic != IMAGE_NT_OPTIONAL_HDR64_MAGIC ||
            fileHeader->Machine != IMAGE_FILE_MACHINE_AMD64 ||
            fileHeader->SizeOfOptionalHeader < sizeof(*optionalHeader) ||
            !Read(bytes, size, optionalOffset, optionalHeader))
        {
            SetError(error, L"The image is not a complete x64 PE32+ executable.");
            return false;
        }

        *sectionOffset = optionalOffset + fileHeader->SizeOfOptionalHeader;
        return true;
    }
}

bool PeMapper::Materialize(const BYTE* fileBytes, size_t fileSize, MappedPeImage* image, std::wstring* error)
{
    if (!image)
    {
        SetError(error, L"No output image was provided.");
        return false;
    }

    *image = MappedPeImage{};
    IMAGE_FILE_HEADER fileHeader{};
    IMAGE_OPTIONAL_HEADER64 optionalHeader{};
    size_t sectionOffset = 0;
    if (!ReadOptionalHeader(fileBytes, fileSize, &fileHeader, &optionalHeader, &sectionOffset, error))
    {
        return false;
    }

    if (optionalHeader.SizeOfImage == 0 || optionalHeader.SizeOfHeaders > optionalHeader.SizeOfImage ||
        !Contains(fileSize, 0, optionalHeader.SizeOfHeaders))
    {
        SetError(error, L"The image has invalid virtual or header sizes.");
        return false;
    }

    image->bytes.assign(optionalHeader.SizeOfImage, 0);
    memcpy(image->bytes.data(), fileBytes, optionalHeader.SizeOfHeaders);

    for (WORD index = 0; index < fileHeader.NumberOfSections; ++index)
    {
        IMAGE_SECTION_HEADER section{};
        const size_t offset = sectionOffset + static_cast<size_t>(index) * sizeof(section);
        if (!Read(fileBytes, fileSize, offset, &section))
        {
            SetError(error, L"The PE section table is truncated.");
            return false;
        }

        if (section.SizeOfRawData == 0)
        {
            if (section.Misc.VirtualSize != 0)
            {
                image->sections.push_back({ section.VirtualAddress, section.Misc.VirtualSize, section.Characteristics });
            }
            continue;
        }

        if (!Contains(fileSize, section.PointerToRawData, section.SizeOfRawData) ||
            !Contains(image->bytes.size(), section.VirtualAddress, section.SizeOfRawData))
        {
            SetError(error, L"A PE section exceeds its source or virtual image bounds.");
            return false;
        }

        memcpy(image->bytes.data() + section.VirtualAddress, fileBytes + section.PointerToRawData, section.SizeOfRawData);
        image->sections.push_back({ section.VirtualAddress,
            (std::max)(section.Misc.VirtualSize, section.SizeOfRawData), section.Characteristics });
    }

    image->preferredImageBase = optionalHeader.ImageBase;
    image->entryPointRva = optionalHeader.AddressOfEntryPoint;
    return true;
}

bool PeMapper::ApplyBaseRelocations(MappedPeImage* image, ULONGLONG actualImageBase, std::wstring* error)
{
    if (!image || image->bytes.empty())
    {
        SetError(error, L"No materialized image was provided.");
        return false;
    }

    IMAGE_FILE_HEADER fileHeader{};
    IMAGE_OPTIONAL_HEADER64 optionalHeader{};
    size_t sectionOffset = 0;
    if (!ReadOptionalHeader(image->bytes.data(), image->bytes.size(), &fileHeader, &optionalHeader, &sectionOffset, error))
    {
        return false;
    }

    const IMAGE_DATA_DIRECTORY directory = optionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_BASERELOC];
    if (directory.VirtualAddress == 0 || directory.Size == 0)
    {
        return actualImageBase == image->preferredImageBase;
    }

    if (!Contains(image->bytes.size(), directory.VirtualAddress, directory.Size))
    {
        SetError(error, L"The relocation directory exceeds the materialized image.");
        return false;
    }

    const ULONGLONG delta = actualImageBase - image->preferredImageBase;
    size_t cursor = directory.VirtualAddress;
    const size_t end = cursor + directory.Size;
    while (cursor < end)
    {
        IMAGE_BASE_RELOCATION block{};
        if (!Read(image->bytes.data(), image->bytes.size(), cursor, &block) ||
            block.SizeOfBlock < sizeof(block) || !Contains(end, cursor, block.SizeOfBlock))
        {
            SetError(error, L"The relocation table is malformed.");
            return false;
        }

        const size_t entriesOffset = cursor + sizeof(block);
        const size_t entriesSize = block.SizeOfBlock - sizeof(block);
        if (entriesSize % sizeof(WORD) != 0)
        {
            SetError(error, L"The relocation table has an invalid entry size.");
            return false;
        }

        for (size_t index = 0; index < entriesSize / sizeof(WORD); ++index)
        {
            WORD entry = 0;
            Read(image->bytes.data(), image->bytes.size(), entriesOffset + index * sizeof(entry), &entry);
            const WORD type = entry >> 12;
            const WORD offset = entry & 0x0fff;
            if (type == IMAGE_REL_BASED_ABSOLUTE)
            {
                continue;
            }

            const size_t target = static_cast<size_t>(block.VirtualAddress) + offset;
            if (type != IMAGE_REL_BASED_DIR64 || !Contains(image->bytes.size(), target, sizeof(ULONGLONG)))
            {
                SetError(error, L"The image contains an unsupported or out-of-bounds relocation.");
                return false;
            }

            ULONGLONG value = 0;
            Read(image->bytes.data(), image->bytes.size(), target, &value);
            value += delta;
            memcpy(image->bytes.data() + target, &value, sizeof(value));
        }

        cursor += block.SizeOfBlock;
    }

    return true;
}
