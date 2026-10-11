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
        // Delay-load IAT entries initially point at linker-generated thunks.
        // Keep those entries intact so __delayLoadHelper2 performs the load on
        // first use through the bridge's LoadLibrary/GetProcAddress shims.
        // Eagerly resolving them here makes optional DLLs mandatory at process
        // startup and does not match the NT loader contract.
        if (symbol.delayLoaded)
        {
            ++report->deferred;
            continue;
        }

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
