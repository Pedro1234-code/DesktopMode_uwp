#include "pch.h"
#include "Bridge/GuestResources.h"

#include "Bridge/GuestModule.h"
#include "Bridge/PeMapper.h"

#include <limits>
#include <utility>

namespace
{
    using namespace Win32Bridge::Bridge;

    thread_local const BYTE* g_guestImageBase = nullptr;
    thread_local size_t g_guestImageSize = 0;
    thread_local const BYTE* g_guestResourceSatelliteBase = nullptr;
    thread_local size_t g_guestResourceSatelliteSize = 0;

    constexpr ULONG_PTR GuestMainModuleToken = 0x10000;
    constexpr size_t MaximumResourceEntries = 1024u * 1024u;

    bool IsMainResourceModule(HMODULE module)
    {
        return !module ||
            module == reinterpret_cast<HMODULE>(GuestMainModuleToken) ||
            reinterpret_cast<const BYTE*>(module) == g_guestImageBase;
    }

    class SatelliteResourceImageScope final
    {
    public:
        SatelliteResourceImageScope()
            : m_primaryBase(g_guestImageBase),
              m_primarySize(g_guestImageSize),
              m_satelliteBase(g_guestResourceSatelliteBase),
              m_satelliteSize(g_guestResourceSatelliteSize)
        {
            g_guestImageBase = m_satelliteBase;
            g_guestImageSize = m_satelliteSize;
            g_guestResourceSatelliteBase = nullptr;
            g_guestResourceSatelliteSize = 0;
        }

        ~SatelliteResourceImageScope()
        {
            g_guestImageBase = m_primaryBase;
            g_guestImageSize = m_primarySize;
            g_guestResourceSatelliteBase = m_satelliteBase;
            g_guestResourceSatelliteSize = m_satelliteSize;
        }

    private:
        const BYTE* m_primaryBase;
        size_t m_primarySize;
        const BYTE* m_satelliteBase;
        size_t m_satelliteSize;
    };

    bool CanTryResourceSatellite(HMODULE module)
    {
        return IsMainResourceModule(module) &&
            g_guestResourceSatelliteBase && g_guestResourceSatelliteSize != 0;
    }

    struct ImageView final
    {
        const BYTE* base = nullptr;
        size_t size = 0;
        HMODULE module = nullptr;

        bool Contains(size_t offset, size_t count) const
        {
            return base && offset <= size && count <= size - offset;
        }

        template<typename T>
        const T* At(size_t offset) const
        {
            return Contains(offset, sizeof(T))
                ? reinterpret_cast<const T*>(base + offset)
                : nullptr;
        }
    };

    struct ResourceView final
    {
        ImageView image;
        size_t offset = 0;
        size_t size = 0;

        bool Contains(size_t relativeOffset, size_t count) const
        {
            return relativeOffset <= size && count <= size - relativeOffset &&
                offset <= image.size && relativeOffset <= image.size - offset &&
                image.Contains(offset + relativeOffset, count);
        }

        template<typename T>
        const T* At(size_t relativeOffset) const
        {
            return Contains(relativeOffset, sizeof(T))
                ? reinterpret_cast<const T*>(image.base + offset + relativeOffset)
                : nullptr;
        }
    };

    struct DirectoryView final
    {
        const IMAGE_RESOURCE_DIRECTORY* directory = nullptr;
        const IMAGE_RESOURCE_DIRECTORY_ENTRY* entries = nullptr;
        size_t count = 0;
    };

    bool MultiplyFits(size_t left, size_t right, size_t* result)
    {
        if (!result || (right != 0 && left > (std::numeric_limits<size_t>::max)() / right))
        {
            return false;
        }
        *result = left * right;
        return true;
    }

    bool MeasureResourceString(LPCWSTR value, size_t* length)
    {
        if (!value || !length)
        {
            return false;
        }
        __try
        {
            size_t count = 0;
            while (count <= 0xffff && value[count] != L'\0')
            {
                ++count;
            }
            if (count > 0xffff)
            {
                return false;
            }
            *length = count;
            return true;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return false;
        }
    }

    bool CopyResourceString(LPCWSTR value, WCHAR* destination, size_t length)
    {
        if (!value || (!destination && length != 0))
        {
            return false;
        }
        __try
        {
            for (size_t index = 0; index < length; ++index)
            {
                destination[index] = value[index];
            }
            return true;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return false;
        }
    }

    bool MakeIdentifier(LPCWSTR value, GuestResourceIdentifier* identifier)
    {
        if (!value || !identifier)
        {
            return false;
        }
        *identifier = GuestResourceIdentifier{};
        const ULONG_PTR raw = reinterpret_cast<ULONG_PTR>(value);
        if (raw <= 0xffff)
        {
            identifier->ordinal = true;
            identifier->id = static_cast<WORD>(raw);
        }
        else
        {
            size_t length = 0;
            if (!MeasureResourceString(value, &length))
            {
                return false;
            }
            identifier->text.resize(length);
            if (!CopyResourceString(
                value, length ? &identifier->text[0] : nullptr, length))
            {
                *identifier = GuestResourceIdentifier{};
                return false;
            }
            const wchar_t* text = identifier->text.c_str();
            if (length != 0 && text[0] == L'#')
            {
                if (length == 1)
                {
                    return false;
                }
                unsigned long parsed = 0;
                size_t index = 1;
                for (; index < length; ++index)
                {
                    if (text[index] < L'0' || text[index] > L'9')
                    {
                        break;
                    }
                    parsed = parsed * 10 + static_cast<unsigned long>(text[index] - L'0');
                    if (parsed > 0xffff)
                    {
                        break;
                    }
                }
                if (index == length && parsed <= 0xffff)
                {
                    identifier->ordinal = true;
                    identifier->id = static_cast<WORD>(parsed);
                    identifier->text.clear();
                    return true;
                }
                return false;
            }
        }
        return true;
    }

    bool ResolveImage(HMODULE requestedModule, ImageView* image)
    {
        if (!image)
        {
            return false;
        }
        *image = ImageView{};

        const HMODULE mainModule = reinterpret_cast<HMODULE>(GuestMainModuleToken);
        const BYTE* currentBase = g_guestImageBase;
        if (!requestedModule || requestedModule == mainModule ||
            reinterpret_cast<const BYTE*>(requestedModule) == currentBase)
        {
            if (!currentBase || g_guestImageSize == 0)
            {
                return false;
            }
            image->base = currentBase;
            image->size = g_guestImageSize;
            image->module = mainModule;
            return true;
        }

        GuestModuleLoader* loader = CurrentGuestModuleLoader();
        if (!loader || !loader->GetMappedImage(requestedModule, &image->base, &image->size))
        {
            return false;
        }
        image->module = requestedModule;
        return true;
    }

    GuestResourceStatus OpenResources(HMODULE module, ResourceView* resources)
    {
        if (!resources)
        {
            return GuestResourceStatus::InvalidParameter;
        }
        *resources = ResourceView{};

        ImageView image;
        if (!ResolveImage(module, &image))
        {
            return GuestResourceStatus::ModuleNotFound;
        }
        const IMAGE_DOS_HEADER* dos = image.At<IMAGE_DOS_HEADER>(0);
        if (!dos || dos->e_magic != IMAGE_DOS_SIGNATURE || dos->e_lfanew < 0)
        {
            return GuestResourceStatus::InvalidImage;
        }
        const size_t ntOffset = static_cast<size_t>(dos->e_lfanew);
        const IMAGE_NT_HEADERS64* nt = image.At<IMAGE_NT_HEADERS64>(ntOffset);
        if (!nt || nt->Signature != IMAGE_NT_SIGNATURE ||
            nt->FileHeader.Machine != IMAGE_FILE_MACHINE_AMD64 ||
            nt->OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR64_MAGIC ||
            nt->FileHeader.SizeOfOptionalHeader < sizeof(IMAGE_OPTIONAL_HEADER64) ||
            nt->OptionalHeader.NumberOfRvaAndSizes <= IMAGE_DIRECTORY_ENTRY_RESOURCE)
        {
            return GuestResourceStatus::InvalidImage;
        }

        const IMAGE_DATA_DIRECTORY& entry =
            nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_RESOURCE];
        if (entry.VirtualAddress == 0 || entry.Size < sizeof(IMAGE_RESOURCE_DIRECTORY) ||
            !image.Contains(entry.VirtualAddress, entry.Size))
        {
            return GuestResourceStatus::InvalidData;
        }
        resources->image = image;
        resources->offset = entry.VirtualAddress;
        resources->size = entry.Size;
        return GuestResourceStatus::Success;
    }

    bool OpenDirectory(const ResourceView& resources, size_t offset, DirectoryView* view)
    {
        if (!view)
        {
            return false;
        }
        *view = DirectoryView{};
        const IMAGE_RESOURCE_DIRECTORY* directory = resources.At<IMAGE_RESOURCE_DIRECTORY>(offset);
        if (!directory)
        {
            return false;
        }
        const size_t count = static_cast<size_t>(directory->NumberOfNamedEntries) +
            static_cast<size_t>(directory->NumberOfIdEntries);
        if (count > MaximumResourceEntries)
        {
            return false;
        }
        size_t byteCount = 0;
        if (!MultiplyFits(count, sizeof(IMAGE_RESOURCE_DIRECTORY_ENTRY), &byteCount))
        {
            return false;
        }
        const size_t entriesOffset = offset + sizeof(IMAGE_RESOURCE_DIRECTORY);
        if (entriesOffset < offset || !resources.Contains(entriesOffset, byteCount))
        {
            return false;
        }
        view->directory = directory;
        view->entries = reinterpret_cast<const IMAGE_RESOURCE_DIRECTORY_ENTRY*>(
            resources.image.base + resources.offset + entriesOffset);
        view->count = count;
        return true;
    }

    bool ReadEntryIdentifier(
        const ResourceView& resources,
        const IMAGE_RESOURCE_DIRECTORY_ENTRY& entry,
        GuestResourceIdentifier* identifier)
    {
        if (!identifier)
        {
            return false;
        }
        *identifier = GuestResourceIdentifier{};
        if (!entry.NameIsString)
        {
            identifier->ordinal = true;
            identifier->id = entry.Id;
            return true;
        }

        const size_t stringOffset = entry.NameOffset;
        const WORD* length = resources.At<WORD>(stringOffset);
        if (!length)
        {
            return false;
        }
        size_t characterBytes = 0;
        const size_t charactersOffset = stringOffset + sizeof(WORD);
        if (charactersOffset < stringOffset ||
            !MultiplyFits(*length, sizeof(WCHAR), &characterBytes) ||
            !resources.Contains(charactersOffset, characterBytes))
        {
            return false;
        }
        const WCHAR* characters = reinterpret_cast<const WCHAR*>(
            resources.image.base + resources.offset + charactersOffset);
        identifier->text.assign(characters, characters + *length);
        return true;
    }

    bool IdentifierEquals(
        const GuestResourceIdentifier& left,
        const GuestResourceIdentifier& right)
    {
        if (left.ordinal != right.ordinal)
        {
            return false;
        }
        if (left.ordinal)
        {
            return left.id == right.id;
        }
        if (left.text.size() != right.text.size())
        {
            return false;
        }
        if (left.text.empty())
        {
            return true;
        }
        return ::CompareStringOrdinal(
            left.text.data(), static_cast<int>(left.text.size()),
            right.text.data(), static_cast<int>(right.text.size()), TRUE) == CSTR_EQUAL;
    }

    void AppendUniqueIdentifiers(
        const std::vector<GuestResourceIdentifier>& source,
        std::vector<GuestResourceIdentifier>* destination)
    {
        if (!destination) return;
        for (const auto& candidate : source)
        {
            bool present = false;
            for (const auto& existing : *destination)
            {
                if (IdentifierEquals(existing, candidate))
                {
                    present = true;
                    break;
                }
            }
            if (!present) destination->push_back(candidate);
        }
    }

    void AppendUniqueLanguages(
        const std::vector<LANGID>& source,
        std::vector<LANGID>* destination)
    {
        if (!destination) return;
        for (LANGID candidate : source)
        {
            bool present = false;
            for (LANGID existing : *destination)
            {
                if (existing == candidate)
                {
                    present = true;
                    break;
                }
            }
            if (!present) destination->push_back(candidate);
        }
    }

    const IMAGE_RESOURCE_DIRECTORY_ENTRY* FindEntry(
        const ResourceView& resources,
        const DirectoryView& directory,
        const GuestResourceIdentifier& requested,
        GuestResourceIdentifier* actual)
    {
        for (size_t index = 0; index < directory.count; ++index)
        {
            GuestResourceIdentifier candidate;
            if (!ReadEntryIdentifier(resources, directory.entries[index], &candidate))
            {
                continue;
            }
            if (IdentifierEquals(candidate, requested))
            {
                if (actual)
                {
                    *actual = std::move(candidate);
                }
                return &directory.entries[index];
            }
        }
        return nullptr;
    }

    bool OpenChildDirectory(
        const ResourceView& resources,
        const IMAGE_RESOURCE_DIRECTORY_ENTRY* entry,
        DirectoryView* child)
    {
        return entry && entry->DataIsDirectory &&
            OpenDirectory(resources, entry->OffsetToDirectory, child);
    }

    GuestResourceStatus OpenNamedResource(
        HMODULE module,
        LPCWSTR typePointer,
        LPCWSTR namePointer,
        ResourceView* resources,
        GuestResourceIdentifier* type,
        GuestResourceIdentifier* name,
        DirectoryView* languages)
    {
        if (!resources || !type || !name || !languages ||
            !MakeIdentifier(typePointer, type) || !MakeIdentifier(namePointer, name))
        {
            return GuestResourceStatus::InvalidParameter;
        }
        GuestResourceStatus status = OpenResources(module, resources);
        if (status != GuestResourceStatus::Success)
        {
            return status;
        }
        DirectoryView root;
        if (!OpenDirectory(*resources, 0, &root))
        {
            return GuestResourceStatus::InvalidData;
        }
        GuestResourceIdentifier actualType;
        const IMAGE_RESOURCE_DIRECTORY_ENTRY* typeEntry =
            FindEntry(*resources, root, *type, &actualType);
        DirectoryView names;
        if (!typeEntry)
        {
            return GuestResourceStatus::TypeNotFound;
        }
        if (!OpenChildDirectory(*resources, typeEntry, &names))
        {
            return GuestResourceStatus::InvalidData;
        }
        GuestResourceIdentifier actualName;
        const IMAGE_RESOURCE_DIRECTORY_ENTRY* nameEntry =
            FindEntry(*resources, names, *name, &actualName);
        if (!nameEntry)
        {
            return GuestResourceStatus::NameNotFound;
        }
        if (!OpenChildDirectory(*resources, nameEntry, languages))
        {
            return GuestResourceStatus::InvalidData;
        }
        *type = std::move(actualType);
        *name = std::move(actualName);
        return GuestResourceStatus::Success;
    }

    GuestResourceStatus EnumerateDirectoryIdentifiers(
        const ResourceView& resources,
        const DirectoryView& directory,
        std::vector<GuestResourceIdentifier>* identifiers)
    {
        if (!identifiers)
        {
            return GuestResourceStatus::InvalidParameter;
        }
        identifiers->clear();
        identifiers->reserve(directory.count);
        for (size_t index = 0; index < directory.count; ++index)
        {
            GuestResourceIdentifier identifier;
            if (!ReadEntryIdentifier(resources, directory.entries[index], &identifier))
            {
                identifiers->clear();
                return GuestResourceStatus::InvalidData;
            }
            identifiers->push_back(std::move(identifier));
        }
        return GuestResourceStatus::Success;
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

GuestResourceStatus Win32Bridge::Bridge::FindGuestResource(
    HMODULE module,
    LPCWSTR typePointer,
    LPCWSTR namePointer,
    LANGID requestedLanguage,
    bool requireExactLanguage,
    GuestResourceData* resource)
{
    if (!resource)
    {
        return GuestResourceStatus::InvalidParameter;
    }
    const auto findInCurrentImage = [&](HMODULE currentModule) -> GuestResourceStatus
    {
        *resource = GuestResourceData{};
        ResourceView resources;
        GuestResourceIdentifier type;
        GuestResourceIdentifier name;
        DirectoryView languages;
        GuestResourceStatus status = OpenNamedResource(
            currentModule, typePointer, namePointer, &resources, &type, &name, &languages);
        if (status != GuestResourceStatus::Success) return status;
        if (languages.count == 0) return GuestResourceStatus::LanguageNotFound;

        const IMAGE_RESOURCE_DIRECTORY_ENTRY* selected = nullptr;
        LANGID selectedLanguage = 0;
        for (size_t index = 0; index < languages.count; ++index)
        {
            const IMAGE_RESOURCE_DIRECTORY_ENTRY& candidate = languages.entries[index];
            if (candidate.NameIsString) continue;
            if (!requireExactLanguage || candidate.Id == requestedLanguage)
            {
                selected = &candidate;
                selectedLanguage = candidate.Id;
                break;
            }
        }
        if (!selected) return GuestResourceStatus::LanguageNotFound;
        if (selected->DataIsDirectory) return GuestResourceStatus::InvalidData;

        const IMAGE_RESOURCE_DATA_ENTRY* dataEntry =
            resources.At<IMAGE_RESOURCE_DATA_ENTRY>(selected->OffsetToData);
        if (!dataEntry || !resources.image.Contains(dataEntry->OffsetToData, dataEntry->Size))
            return GuestResourceStatus::InvalidData;

        resource->module = resources.image.module;
        resource->type = std::move(type);
        resource->name = std::move(name);
        resource->language = selectedLanguage;
        resource->codePage = dataEntry->CodePage;
        resource->data = resources.image.base + dataEntry->OffsetToData;
        resource->size = dataEntry->Size;
        return GuestResourceStatus::Success;
    };

    GuestResourceStatus status = findInCurrentImage(module);
    if (status == GuestResourceStatus::Success || !CanTryResourceSatellite(module))
        return status;

    SatelliteResourceImageScope satellite;
    return findInCurrentImage(nullptr);
}

GuestResourceStatus Win32Bridge::Bridge::CopyGuestFileResource(
    const BYTE* fileBytes,
    size_t fileSize,
    LPCWSTR type,
    LPCWSTR name,
    LANGID language,
    bool requireExactLanguage,
    std::vector<BYTE>* payload,
    LANGID* selectedLanguage,
    DWORD* codePage)
{
    if (!fileBytes || fileSize == 0 || !payload)
    {
        return GuestResourceStatus::InvalidParameter;
    }
    payload->clear();
    if (selectedLanguage) *selectedLanguage = 0;
    if (codePage) *codePage = 0;

    MappedPeImage image;
    std::wstring error;
    if (!PeMapper::Materialize(fileBytes, fileSize, &image, &error))
    {
        return GuestResourceStatus::InvalidImage;
    }

    GuestResourceScope scope(image.bytes.data(), image.bytes.size());
    GuestResourceData resource;
    const GuestResourceStatus status = FindGuestResource(
        nullptr, type, name, language, requireExactLanguage, &resource);
    if (status != GuestResourceStatus::Success)
    {
        return status;
    }
    if (resource.size != 0 && !resource.data)
    {
        return GuestResourceStatus::InvalidData;
    }
    payload->assign(resource.data, resource.data + resource.size);
    if (selectedLanguage) *selectedLanguage = resource.language;
    if (codePage) *codePage = resource.codePage;
    return GuestResourceStatus::Success;
}

GuestResourceStatus Win32Bridge::Bridge::EnumerateGuestResourceTypes(
    HMODULE module,
    std::vector<GuestResourceIdentifier>* types)
{
    if (!types)
    {
        return GuestResourceStatus::InvalidParameter;
    }
    const auto enumerateCurrentImage = [types](HMODULE currentModule)
        -> GuestResourceStatus
    {
        types->clear();
        ResourceView resources;
        GuestResourceStatus status = OpenResources(currentModule, &resources);
        if (status != GuestResourceStatus::Success) return status;
        DirectoryView root;
        if (!OpenDirectory(resources, 0, &root)) return GuestResourceStatus::InvalidData;
        return EnumerateDirectoryIdentifiers(resources, root, types);
    };

    GuestResourceStatus primaryStatus = enumerateCurrentImage(module);
    if (!CanTryResourceSatellite(module)) return primaryStatus;

    std::vector<GuestResourceIdentifier> primary;
    if (primaryStatus == GuestResourceStatus::Success) primary = *types;
    GuestResourceStatus satelliteStatus = GuestResourceStatus::ModuleNotFound;
    {
        SatelliteResourceImageScope satellite;
        satelliteStatus = enumerateCurrentImage(nullptr);
    }
    if (satelliteStatus == GuestResourceStatus::Success)
    {
        std::vector<GuestResourceIdentifier> satellite = std::move(*types);
        *types = std::move(primary);
        AppendUniqueIdentifiers(satellite, types);
        return GuestResourceStatus::Success;
    }
    *types = std::move(primary);
    return primaryStatus;
}

GuestResourceStatus Win32Bridge::Bridge::EnumerateGuestResourceNames(
    HMODULE module,
    LPCWSTR typePointer,
    std::vector<GuestResourceIdentifier>* names)
{
    if (!names)
    {
        return GuestResourceStatus::InvalidParameter;
    }
    const auto enumerateCurrentImage = [typePointer, names](HMODULE currentModule)
        -> GuestResourceStatus
    {
        names->clear();
        GuestResourceIdentifier type;
        if (!MakeIdentifier(typePointer, &type)) return GuestResourceStatus::InvalidParameter;
        ResourceView resources;
        GuestResourceStatus status = OpenResources(currentModule, &resources);
        if (status != GuestResourceStatus::Success) return status;
        DirectoryView root;
        if (!OpenDirectory(resources, 0, &root)) return GuestResourceStatus::InvalidData;
        const IMAGE_RESOURCE_DIRECTORY_ENTRY* typeEntry =
            FindEntry(resources, root, type, nullptr);
        if (!typeEntry) return GuestResourceStatus::TypeNotFound;
        DirectoryView nameDirectory;
        if (!OpenChildDirectory(resources, typeEntry, &nameDirectory))
            return GuestResourceStatus::InvalidData;
        return EnumerateDirectoryIdentifiers(resources, nameDirectory, names);
    };

    GuestResourceStatus primaryStatus = enumerateCurrentImage(module);
    if (!CanTryResourceSatellite(module)) return primaryStatus;

    std::vector<GuestResourceIdentifier> primary;
    if (primaryStatus == GuestResourceStatus::Success) primary = *names;
    GuestResourceStatus satelliteStatus = GuestResourceStatus::ModuleNotFound;
    {
        SatelliteResourceImageScope satellite;
        satelliteStatus = enumerateCurrentImage(nullptr);
    }
    if (satelliteStatus == GuestResourceStatus::Success)
    {
        std::vector<GuestResourceIdentifier> satellite = std::move(*names);
        *names = std::move(primary);
        AppendUniqueIdentifiers(satellite, names);
        return GuestResourceStatus::Success;
    }
    *names = std::move(primary);
    return primaryStatus;
}

GuestResourceStatus Win32Bridge::Bridge::EnumerateGuestResourceLanguages(
    HMODULE module,
    LPCWSTR typePointer,
    LPCWSTR namePointer,
    std::vector<LANGID>* languagesOutput)
{
    if (!languagesOutput)
    {
        return GuestResourceStatus::InvalidParameter;
    }
    const auto enumerateCurrentImage = [typePointer, namePointer, languagesOutput](
        HMODULE currentModule) -> GuestResourceStatus
    {
        languagesOutput->clear();
        ResourceView resources;
        GuestResourceIdentifier type;
        GuestResourceIdentifier name;
        DirectoryView languages;
        GuestResourceStatus status = OpenNamedResource(
            currentModule, typePointer, namePointer, &resources, &type, &name, &languages);
        if (status != GuestResourceStatus::Success) return status;
        languagesOutput->reserve(languages.count);
        for (size_t index = 0; index < languages.count; ++index)
        {
            const IMAGE_RESOURCE_DIRECTORY_ENTRY& entry = languages.entries[index];
            if (entry.NameIsString || entry.DataIsDirectory)
            {
                languagesOutput->clear();
                return GuestResourceStatus::InvalidData;
            }
            languagesOutput->push_back(entry.Id);
        }
        return GuestResourceStatus::Success;
    };

    GuestResourceStatus primaryStatus = enumerateCurrentImage(module);
    if (!CanTryResourceSatellite(module)) return primaryStatus;

    std::vector<LANGID> primary;
    if (primaryStatus == GuestResourceStatus::Success) primary = *languagesOutput;
    GuestResourceStatus satelliteStatus = GuestResourceStatus::ModuleNotFound;
    {
        SatelliteResourceImageScope satellite;
        satelliteStatus = enumerateCurrentImage(nullptr);
    }
    if (satelliteStatus == GuestResourceStatus::Success)
    {
        std::vector<LANGID> satellite = std::move(*languagesOutput);
        *languagesOutput = std::move(primary);
        AppendUniqueLanguages(satellite, languagesOutput);
        return GuestResourceStatus::Success;
    }
    *languagesOutput = std::move(primary);
    return primaryStatus;
}

Win32Bridge::Bridge::GuestResourceScope::GuestResourceScope(
    const BYTE* imageBase,
    size_t imageSize,
    const BYTE* satelliteBase,
    size_t satelliteSize)
    : m_previousBase(g_guestImageBase),
      m_previousSize(g_guestImageSize),
      m_previousSatelliteBase(g_guestResourceSatelliteBase),
      m_previousSatelliteSize(g_guestResourceSatelliteSize)
{
    g_guestImageBase = imageBase;
    g_guestImageSize = imageSize;
    g_guestResourceSatelliteBase = satelliteBase;
    g_guestResourceSatelliteSize = satelliteSize;
}

Win32Bridge::Bridge::GuestResourceScope::~GuestResourceScope()
{
    g_guestImageBase = m_previousBase;
    g_guestImageSize = m_previousSize;
    g_guestResourceSatelliteBase = m_previousSatelliteBase;
    g_guestResourceSatelliteSize = m_previousSatelliteSize;
}
