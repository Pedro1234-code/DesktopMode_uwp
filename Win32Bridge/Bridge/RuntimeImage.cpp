#include "Bridge\\RuntimeImage.h"

#include <cstring>
#include <vector>

using namespace Win32Bridge::Bridge;

namespace
{
    constexpr size_t PageSize = 4096;

    bool Contains(size_t size, size_t offset, size_t length)
    {
        return offset <= size && length <= size - offset;
    }

    void SetError(std::wstring* error, const std::wstring& message)
    {
        if (error)
        {
            *error = message;
        }
    }

    DWORD PageProtection(DWORD characteristics)
    {
        if ((characteristics & IMAGE_SCN_MEM_EXECUTE) != 0)
        {
            // UWP prohibits a writable-and-executable page. IAT binding and
            // relocation happen before this phase, while pages are writable.
            return PAGE_EXECUTE_READ;
        }
        if ((characteristics & IMAGE_SCN_MEM_WRITE) != 0)
        {
            return PAGE_READWRITE;
        }
        return PAGE_READONLY;
    }
}

RuntimeImage::~RuntimeImage()
{
    Release();
}

bool RuntimeImage::Reserve(size_t size, ULONGLONG preferredBase, RuntimeImage* image, std::wstring* error)
{
    if (!image || size == 0)
    {
        SetError(error, L"A non-empty runtime image is required.");
        return false;
    }

    image->Release();
    void* requestedBase = reinterpret_cast<void*>(static_cast<ULONG_PTR>(preferredBase));
    void* memory = VirtualAllocFromApp(requestedBase, size, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    if (!memory && requestedBase)
    {
        memory = VirtualAllocFromApp(nullptr, size, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    }
    if (!memory)
    {
        SetError(error, L"VirtualAllocFromApp failed (" + std::to_wstring(GetLastError()) + L").");
        return false;
    }

    image->m_base = static_cast<BYTE*>(memory);
    image->m_size = size;
    return true;
}

bool RuntimeImage::CopyFrom(const MappedPeImage& image, std::wstring* error)
{
    if (!m_base || image.bytes.size() != m_size)
    {
        SetError(error, L"The runtime allocation does not match the materialized PE image.");
        return false;
    }

    memcpy(m_base, image.bytes.data(), m_size);
    return true;
}

bool RuntimeImage::FinalizeProtections(const MappedPeImage& image, std::wstring* error)
{
    if (!m_base || image.bytes.size() != m_size || m_size % PageSize != 0)
    {
        SetError(error, L"The runtime image must be page-aligned before protection is finalized.");
        return false;
    }

    std::vector<DWORD> pageCharacteristics(m_size / PageSize, IMAGE_SCN_MEM_READ);
    for (const auto& section : image.sections)
    {
        if (section.size == 0 || !Contains(m_size, section.rva, section.size))
        {
            SetError(error, L"A mapped section exceeds the runtime allocation.");
            return false;
        }

        const size_t firstPage = section.rva / PageSize;
        const size_t lastPage = (static_cast<size_t>(section.rva) + section.size - 1) / PageSize;
        for (size_t page = firstPage; page <= lastPage; ++page)
        {
            pageCharacteristics[page] |= section.characteristics;
        }
    }

    size_t rangeStart = 0;
    while (rangeStart < pageCharacteristics.size())
    {
        const DWORD protection = PageProtection(pageCharacteristics[rangeStart]);
        size_t rangeEnd = rangeStart + 1;
        while (rangeEnd < pageCharacteristics.size() && PageProtection(pageCharacteristics[rangeEnd]) == protection)
        {
            ++rangeEnd;
        }

        ULONG oldProtection = 0;
        const size_t offset = rangeStart * PageSize;
        const size_t size = (rangeEnd - rangeStart) * PageSize;
        if (!VirtualProtectFromApp(m_base + offset, size, protection, &oldProtection))
        {
            SetError(error, L"VirtualProtectFromApp failed (" + std::to_wstring(GetLastError()) +
                L"). Executable pages require the UWP codeGeneration capability.");
            return false;
        }

        rangeStart = rangeEnd;
    }

    FlushInstructionCache(GetCurrentProcess(), m_base, m_size);
    return true;
}

void RuntimeImage::Release()
{
    if (m_base)
    {
        VirtualFree(m_base, 0, MEM_RELEASE);
        m_base = nullptr;
        m_size = 0;
    }
}
