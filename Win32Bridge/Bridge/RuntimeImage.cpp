#include "Bridge\\RuntimeImage.h"

#include <algorithm>
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

    bool InvokeTlsCallback(
        PIMAGE_TLS_CALLBACK callback,
        BYTE* module,
        DWORD reason)
    {
        __try
        {
            callback(module, reason, nullptr);
            return true;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return false;
        }
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
    return RegisterUnwindMetadata(error);
}

bool RuntimeImage::RegisterUnwindMetadata(std::wstring* error)
{
    if (!m_base || m_size < sizeof(IMAGE_DOS_HEADER))
    {
        SetError(error, L"The runtime image is unavailable for unwind registration.");
        return false;
    }

    const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(m_base);
    if (dos->e_magic != IMAGE_DOS_SIGNATURE || dos->e_lfanew < 0 ||
        !Contains(m_size, static_cast<size_t>(dos->e_lfanew), sizeof(IMAGE_NT_HEADERS64)))
    {
        SetError(error, L"The mapped PE headers are invalid for unwind registration.");
        return false;
    }

    const auto* headers = reinterpret_cast<const IMAGE_NT_HEADERS64*>(m_base + dos->e_lfanew);
    if (headers->Signature != IMAGE_NT_SIGNATURE || headers->OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR64_MAGIC)
    {
        SetError(error, L"The mapped image is not a PE32+ image.");
        return false;
    }

    const IMAGE_DATA_DIRECTORY directory =
        headers->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXCEPTION];
    if (directory.VirtualAddress == 0 || directory.Size == 0)
    {
        return true;
    }
    if (directory.Size % sizeof(RUNTIME_FUNCTION) != 0 ||
        !Contains(m_size, directory.VirtualAddress, directory.Size))
    {
        SetError(error, L"The PE exception directory is malformed.");
        return false;
    }

    auto* table = reinterpret_cast<PRUNTIME_FUNCTION>(m_base + directory.VirtualAddress);
    const DWORD entryCount = directory.Size / sizeof(RUNTIME_FUNCTION);
    if (entryCount == 0 || !RtlAddFunctionTable(table, entryCount, reinterpret_cast<DWORD64>(m_base)))
    {
        SetError(error, L"RtlAddFunctionTable failed for the mapped guest image.");
        return false;
    }

    m_functionTable = table;
    m_functionEntryCount = entryCount;
    return true;
}

bool RuntimeImage::NotifyTls(DWORD reason, std::wstring* error) const
{
    if (!m_base || m_size < sizeof(IMAGE_DOS_HEADER))
    {
        SetError(error, L"The runtime image is unavailable for TLS notification.");
        return false;
    }
    const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(m_base);
    if (dos->e_magic != IMAGE_DOS_SIGNATURE || dos->e_lfanew < 0 ||
        !Contains(m_size, static_cast<size_t>(dos->e_lfanew), sizeof(IMAGE_NT_HEADERS64)))
    {
        SetError(error, L"The mapped PE headers are invalid for TLS notification.");
        return false;
    }
    const auto* headers = reinterpret_cast<const IMAGE_NT_HEADERS64*>(m_base + dos->e_lfanew);
    if (headers->Signature != IMAGE_NT_SIGNATURE ||
        headers->OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR64_MAGIC ||
        headers->OptionalHeader.NumberOfRvaAndSizes <= IMAGE_DIRECTORY_ENTRY_TLS)
        return true;

    const IMAGE_DATA_DIRECTORY directory =
        headers->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_TLS];
    if (directory.VirtualAddress == 0 || directory.Size == 0) return true;
    if (directory.Size < sizeof(IMAGE_TLS_DIRECTORY64) ||
        !Contains(m_size, directory.VirtualAddress, sizeof(IMAGE_TLS_DIRECTORY64)))
    {
        SetError(error, L"The PE TLS directory is malformed.");
        return false;
    }

    const auto* tls = reinterpret_cast<const IMAGE_TLS_DIRECTORY64*>(
        m_base + directory.VirtualAddress);
    const ULONG_PTR base = reinterpret_cast<ULONG_PTR>(m_base);
    const auto inImageAddress = [base, this](ULONGLONG address, size_t bytes, size_t* offset)
    {
        if (address < base || address - base > m_size) return false;
        const size_t value = static_cast<size_t>(address - base);
        if (!Contains(m_size, value, bytes)) return false;
        if (offset) *offset = value;
        return true;
    };

    if (reason == DLL_PROCESS_ATTACH || reason == DLL_THREAD_ATTACH)
    {
        if (m_tlsIndex == TLS_OUT_OF_INDEXES)
        {
            m_tlsIndex = TlsAlloc();
            if (m_tlsIndex == TLS_OUT_OF_INDEXES)
            {
                SetError(error, L"TlsAlloc failed for the mapped PE image.");
                return false;
            }
            size_t indexOffset = 0;
            if (!inImageAddress(tls->AddressOfIndex, sizeof(DWORD), &indexOffset))
            {
                TlsFree(m_tlsIndex);
                m_tlsIndex = TLS_OUT_OF_INDEXES;
                SetError(error, L"The PE TLS index slot lies outside the mapped image.");
                return false;
            }
            ULONG oldProtection = 0;
            if (!VirtualProtectFromApp(
                    m_base + indexOffset, sizeof(m_tlsIndex), PAGE_READWRITE, &oldProtection))
            {
                TlsFree(m_tlsIndex);
                m_tlsIndex = TLS_OUT_OF_INDEXES;
                SetError(error, L"The PE TLS index slot could not be made writable.");
                return false;
            }
            memcpy(m_base + indexOffset, &m_tlsIndex, sizeof(m_tlsIndex));
            ULONG ignoredProtection = 0;
            if (!VirtualProtectFromApp(
                    m_base + indexOffset, sizeof(m_tlsIndex), oldProtection, &ignoredProtection))
            {
                TlsFree(m_tlsIndex);
                m_tlsIndex = TLS_OUT_OF_INDEXES;
                SetError(error, L"The PE TLS index-slot protection could not be restored.");
                return false;
            }
        }

        size_t rawOffset = 0;
        size_t rawBytes = 0;
        if (tls->EndAddressOfRawData < tls->StartAddressOfRawData)
        {
            SetError(error, L"The PE TLS template range is malformed.");
            return false;
        }
        const ULONGLONG rawLength = tls->EndAddressOfRawData - tls->StartAddressOfRawData;
        if (rawLength > static_cast<ULONGLONG>(SIZE_MAX) ||
            (rawLength != 0 && !inImageAddress(
                tls->StartAddressOfRawData, static_cast<size_t>(rawLength), &rawOffset)))
        {
            SetError(error, L"The PE TLS template lies outside the mapped image.");
            return false;
        }
        rawBytes = static_cast<size_t>(rawLength);
        const ULONGLONG allocation64 = rawLength + tls->SizeOfZeroFill;
        if (allocation64 < rawLength || allocation64 > 64ull * 1024ull * 1024ull)
        {
            SetError(error, L"The PE TLS allocation exceeds the safety limit.");
            return false;
        }
        const SIZE_T allocation = static_cast<SIZE_T>(allocation64 == 0 ? 1 : allocation64);
        void* block = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, allocation);
        if (!block)
        {
            SetError(error, L"The PE TLS data block could not be allocated.");
            return false;
        }
        if (rawBytes != 0) memcpy(block, m_base + rawOffset, rawBytes);
        if (!TlsSetValue(m_tlsIndex, block))
        {
            HeapFree(GetProcessHeap(), 0, block);
            SetError(error, L"TlsSetValue failed for the mapped PE image.");
            return false;
        }
    }

    const auto releaseThreadStorage = [this, reason]()
    {
        if (m_tlsIndex == TLS_OUT_OF_INDEXES) return;
        void* block = TlsGetValue(m_tlsIndex);
        if (block) HeapFree(GetProcessHeap(), 0, block);
        TlsSetValue(m_tlsIndex, nullptr);
        if (reason == DLL_PROCESS_DETACH)
        {
            TlsFree(m_tlsIndex);
            m_tlsIndex = TLS_OUT_OF_INDEXES;
        }
    };

    if (tls->AddressOfCallBacks == 0)
    {
        if (reason == DLL_PROCESS_DETACH || reason == DLL_THREAD_DETACH)
            releaseThreadStorage();
        return true;
    }
    const ULONG_PTR callbacksAddress = static_cast<ULONG_PTR>(tls->AddressOfCallBacks);
    if (callbacksAddress < base || callbacksAddress - base >= m_size)
    {
        SetError(error, L"The PE TLS callback array lies outside the mapped image.");
        return false;
    }

    const size_t callbacksOffset = callbacksAddress - base;
    constexpr size_t MaximumTlsCallbacks = 4096;
    for (size_t index = 0; index < MaximumTlsCallbacks; ++index)
    {
        const size_t entryOffset = callbacksOffset + index * sizeof(ULONGLONG);
        if (entryOffset < callbacksOffset || !Contains(m_size, entryOffset, sizeof(ULONGLONG)))
        {
            SetError(error, L"The PE TLS callback array has no in-image terminator.");
            return false;
        }
        ULONGLONG callbackValue = 0;
        memcpy(&callbackValue, m_base + entryOffset, sizeof(callbackValue));
        if (callbackValue == 0)
        {
            if (reason == DLL_PROCESS_DETACH || reason == DLL_THREAD_DETACH)
                releaseThreadStorage();
            return true;
        }
        const ULONG_PTR callbackAddress = static_cast<ULONG_PTR>(callbackValue);
        if (callbackAddress < base || callbackAddress - base >= m_size)
        {
            SetError(error, L"A PE TLS callback lies outside the mapped image.");
            return false;
        }
        if (!InvokeTlsCallback(
            reinterpret_cast<PIMAGE_TLS_CALLBACK>(callbackAddress), m_base, reason))
        {
            SetError(error, L"A PE TLS callback raised a structured exception.");
            return false;
        }
    }

    SetError(error, L"The PE TLS callback array exceeds the safety limit.");
    return false;
}

void RuntimeImage::Release()
{
    if (m_tlsIndex != TLS_OUT_OF_INDEXES)
    {
        void* block = TlsGetValue(m_tlsIndex);
        if (block) HeapFree(GetProcessHeap(), 0, block);
        TlsSetValue(m_tlsIndex, nullptr);
        TlsFree(m_tlsIndex);
        m_tlsIndex = TLS_OUT_OF_INDEXES;
    }
    if (m_functionTable)
    {
        RtlDeleteFunctionTable(m_functionTable);
        m_functionTable = nullptr;
        m_functionEntryCount = 0;
    }
    if (m_base)
    {
        VirtualFree(m_base, 0, MEM_RELEASE);
        m_base = nullptr;
        m_size = 0;
    }
}
