#pragma once

#include <cstddef>
#include <windows.h>

namespace Win32Bridge
{
namespace Bridge
{
    // Resource APIs read only from the already-mapped PE image.  They never
    // expose a host module or use the desktop resource loader.
    const BYTE* CurrentGuestImageBase();
    size_t CurrentGuestImageSize();

    // Looks up one resource directly in the mapped guest PE.  `name` accepts
    // either an ordinal resource token (MAKEINTRESOURCE-style) or a Unicode
    // resource name.  The returned bytes remain owned by the mapped image.
    bool FindGuestResource(WORD resourceType, LPCWSTR name, const BYTE** data, size_t* size);

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
