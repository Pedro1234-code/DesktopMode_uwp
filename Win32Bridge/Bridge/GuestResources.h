#pragma once

#include <cstddef>
#include <string>
#include <vector>
#include <windows.h>

namespace Win32Bridge
{
namespace Bridge
{
    // Resource APIs read only from the already-mapped PE image.  They never
    // expose a host module or use the desktop resource loader.
    const BYTE* CurrentGuestImageBase();
    size_t CurrentGuestImageSize();

    enum class GuestResourceStatus
    {
        Success,
        InvalidParameter,
        ModuleNotFound,
        InvalidImage,
        TypeNotFound,
        NameNotFound,
        LanguageNotFound,
        InvalidData,
    };

    struct GuestResourceIdentifier final
    {
        bool ordinal = false;
        WORD id = 0;
        std::wstring text;
    };

    struct GuestResourceData final
    {
        // Normalized module identity: 0x10000 for the primary executable or
        // the mapped image base for a guest DLL.
        HMODULE module = nullptr;
        GuestResourceIdentifier type;
        GuestResourceIdentifier name;
        LANGID language = 0;
        DWORD codePage = 0;
        const BYTE* data = nullptr;
        size_t size = 0;
    };

    // Reads from a mapped AMD64 image. With requireExactLanguage == false the
    // first numeric language entry is selected deterministically; locale/MUI
    // fallback is intentionally outside the current parser.
    GuestResourceStatus FindGuestResource(
        HMODULE module,
        LPCWSTR type,
        LPCWSTR name,
        LANGID language,
        bool requireExactLanguage,
        GuestResourceData* resource);

    // Materializes an x64 PE file and copies one resource out of its virtual
    // image. This keeps pointers from temporary mapped storage from escaping.
    GuestResourceStatus CopyGuestFileResource(
        const BYTE* fileBytes,
        size_t fileSize,
        LPCWSTR type,
        LPCWSTR name,
        LANGID language,
        bool requireExactLanguage,
        std::vector<BYTE>* payload,
        LANGID* selectedLanguage = nullptr,
        DWORD* codePage = nullptr);

    GuestResourceStatus EnumerateGuestResourceTypes(
        HMODULE module,
        std::vector<GuestResourceIdentifier>* types);
    GuestResourceStatus EnumerateGuestResourceNames(
        HMODULE module,
        LPCWSTR type,
        std::vector<GuestResourceIdentifier>* names);
    GuestResourceStatus EnumerateGuestResourceLanguages(
        HMODULE module,
        LPCWSTR type,
        LPCWSTR name,
        std::vector<LANGID>* languages);

    class GuestResourceScope final
    {
    public:
        GuestResourceScope(const BYTE* imageBase, size_t imageSize);
        ~GuestResourceScope();

        GuestResourceScope(const GuestResourceScope&) = delete;
        GuestResourceScope& operator=(const GuestResourceScope&) = delete;

    private:
        const BYTE* m_previousBase;
        size_t m_previousSize;
    };
}
}
