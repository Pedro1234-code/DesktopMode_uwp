#include "pch.h"
#include "Bridge/GuestResources.h"

#include <cstring>
#include <cwchar>
#include <string>

namespace
{
    thread_local const BYTE* g_guestImageBase = nullptr;
    thread_local size_t g_guestImageSize = 0;

    struct ResourceKey final
    {
        bool ordinal = false;
        WORD id = 0;
        std::wstring text;
    };

    bool IsImageRange(const BYTE* address, size_t count)
    {
        const BYTE* image = g_guestImageBase;
        const size_t imageSize = g_guestImageSize;
        return image && address >= image && count <= imageSize &&
            static_cast<size_t>(address - image) <= imageSize - count;
    }

    bool MakeResourceKey(LPCWSTR name, ResourceKey* key)
    {
        if (!name || !key)
        {
            return false;
        }
        *key = ResourceKey{};
        const ULONG_PTR raw = reinterpret_cast<ULONG_PTR>(name);
        if (raw <= 0xffff)
        {
            key->ordinal = true;
            key->id = static_cast<WORD>(raw);
            return true;
        }
        key->text = name;
        return true;
    }

    const IMAGE_RESOURCE_DIRECTORY_ENTRY* FindEntry(
        const BYTE* resourceBase,
        const IMAGE_RESOURCE_DIRECTORY* directory,
        const ResourceKey& key)
    {
        if (!resourceBase || !directory || !IsImageRange(reinterpret_cast<const BYTE*>(directory), sizeof(*directory)))
        {
            return nullptr;
        }
        const size_t count = static_cast<size_t>(directory->NumberOfNamedEntries) + directory->NumberOfIdEntries;
        const auto entries = reinterpret_cast<const IMAGE_RESOURCE_DIRECTORY_ENTRY*>(directory + 1);
        if (!IsImageRange(reinterpret_cast<const BYTE*>(entries), count * sizeof(*entries)))
        {
            return nullptr;
        }
        for (size_t index = 0; index < count; ++index)
        {
            const auto& entry = entries[index];
            if (key.ordinal)
            {
                if (!entry.NameIsString && entry.Id == key.id)
                {
                    return &entry;
                }
                continue;
            }
            if (!entry.NameIsString)
            {
                continue;
            }
            const auto string = reinterpret_cast<const IMAGE_RESOURCE_DIR_STRING_U*>(resourceBase + entry.NameOffset);
            if (!IsImageRange(reinterpret_cast<const BYTE*>(string), sizeof(WORD)))
            {
                continue;
            }
            const size_t stringBytes = sizeof(WORD) + static_cast<size_t>(string->Length) * sizeof(WCHAR);
            if (IsImageRange(reinterpret_cast<const BYTE*>(string), stringBytes) &&
                key.text.size() == string->Length &&
                std::wmemcmp(key.text.data(), string->NameString, string->Length) == 0)
            {
                return &entry;
            }
        }
        return nullptr;
    }

    const IMAGE_RESOURCE_DIRECTORY* FindChildDirectory(
        const BYTE* resourceBase,
        const IMAGE_RESOURCE_DIRECTORY_ENTRY* entry)
    {
        if (!entry || !entry->DataIsDirectory)
        {
            return nullptr;
        }
        const auto directory = reinterpret_cast<const IMAGE_RESOURCE_DIRECTORY*>(resourceBase + entry->OffsetToDirectory);
        return IsImageRange(reinterpret_cast<const BYTE*>(directory), sizeof(*directory)) ? directory : nullptr;
    }
}

const BYTE* Win32Bridge::Bridge::CurrentGuestImageBase()
{
    return g_guestImageBase;
}

size_t Win32Bridge::Bridge::CurrentGuestImageSize()
{
    return g_guestImageSize;
}

bool Win32Bridge::Bridge::FindGuestResource(WORD resourceType, LPCWSTR name, const BYTE** data, size_t* size)
{
    if (!data || !size)
    {
        return false;
    }
    *data = nullptr;
    *size = 0;

    ResourceKey requested;
    if (!MakeResourceKey(name, &requested) || !g_guestImageBase ||
        g_guestImageSize < sizeof(IMAGE_DOS_HEADER))
    {
        return false;
    }
    const auto dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(g_guestImageBase);
    if (dos->e_magic != IMAGE_DOS_SIGNATURE || dos->e_lfanew < 0 ||
        !IsImageRange(g_guestImageBase + dos->e_lfanew, sizeof(IMAGE_NT_HEADERS64)))
    {
        return false;
    }
    const auto nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(g_guestImageBase + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE || nt->OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR64_MAGIC)
    {
        return false;
    }
    const auto& resourceDirectory = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_RESOURCE];
    if (resourceDirectory.VirtualAddress == 0 || resourceDirectory.Size < sizeof(IMAGE_RESOURCE_DIRECTORY) ||
        !IsImageRange(g_guestImageBase + resourceDirectory.VirtualAddress, resourceDirectory.Size))
    {
        return false;
    }

    const BYTE* resourceBase = g_guestImageBase + resourceDirectory.VirtualAddress;
    const auto root = reinterpret_cast<const IMAGE_RESOURCE_DIRECTORY*>(resourceBase);
    ResourceKey type;
    type.ordinal = true;
    type.id = resourceType;
    const auto typeEntry = FindEntry(resourceBase, root, type);
    const auto nameDirectory = FindChildDirectory(resourceBase, typeEntry);
    const auto nameEntry = FindEntry(resourceBase, nameDirectory, requested);
    const auto languageDirectory = FindChildDirectory(resourceBase, nameEntry);
    if (!languageDirectory)
    {
        return false;
    }
    const size_t languageCount = static_cast<size_t>(languageDirectory->NumberOfNamedEntries) + languageDirectory->NumberOfIdEntries;
    const auto languageEntries = reinterpret_cast<const IMAGE_RESOURCE_DIRECTORY_ENTRY*>(languageDirectory + 1);
    if (languageCount == 0 || !IsImageRange(reinterpret_cast<const BYTE*>(languageEntries),
        languageCount * sizeof(*languageEntries)) || languageEntries[0].DataIsDirectory)
    {
        return false;
    }
    const auto entry = reinterpret_cast<const IMAGE_RESOURCE_DATA_ENTRY*>(
        resourceBase + languageEntries[0].OffsetToData);
    if (!IsImageRange(reinterpret_cast<const BYTE*>(entry), sizeof(*entry)) ||
        !IsImageRange(g_guestImageBase + entry->OffsetToData, entry->Size))
    {
        return false;
    }
    *data = g_guestImageBase + entry->OffsetToData;
    *size = entry->Size;
    return true;
}

Win32Bridge::Bridge::GuestResourceScope::GuestResourceScope(const BYTE* imageBase, size_t imageSize)
    : m_previousBase(g_guestImageBase), m_previousSize(g_guestImageSize)
{
    g_guestImageBase = imageBase;
    g_guestImageSize = imageSize;
}

Win32Bridge::Bridge::GuestResourceScope::~GuestResourceScope()
{
    g_guestImageBase = m_previousBase;
    g_guestImageSize = m_previousSize;
}
