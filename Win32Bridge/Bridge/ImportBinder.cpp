#include "Bridge\\ImportBinder.h"

#include <cstring>

using namespace Win32Bridge::Bridge;

namespace
{
    bool Contains(size_t size, size_t offset, size_t length)
    {
        return offset <= size && length <= size - offset;
    }

    void SetError(std::wstring* error, const wchar_t* message)
    {
        if (error)
        {
            *error = message;
        }
    }
}

bool ImportBinder::Bind(
    MappedPeImage* image,
    const PeImageInfo& metadata,
    const ImportResolver& resolver,
    BindingReport* report,
    std::wstring* error)
{
    if (!image || image->bytes.empty() || !resolver || !report)
    {
        SetError(error, L"The binder needs a materialized image, resolver, and report.");
        return false;
    }

    *report = BindingReport{};
    for (const auto& symbol : metadata.imports)
    {
        const ImportResolution resolution = resolver(symbol);
        report->resolutions.push_back(resolution);
        if (resolution.targetAddress == 0)
        {
            ++report->unresolved;
            continue;
        }

        if (!Contains(image->bytes.size(), symbol.iatRva, sizeof(resolution.targetAddress)))
        {
            SetError(error, L"An IAT entry is outside the materialized image.");
            return false;
        }

        memcpy(image->bytes.data() + symbol.iatRva, &resolution.targetAddress, sizeof(resolution.targetAddress));
        ++report->bound;
    }

    return true;
}
