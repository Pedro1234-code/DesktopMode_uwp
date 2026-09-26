#pragma once

#include "Bridge\\CompatibilityCatalog.h"
#include "Bridge\\PeMapper.h"

#include <functional>

namespace Win32Bridge
{
namespace Bridge
{
    struct BindingReport
    {
        unsigned int bound = 0;
        unsigned int unresolved = 0;
        std::vector<ImportResolution> resolutions;
    };

    using ImportResolver = std::function<ImportResolution(const ImportedSymbol&)>;

    // Writes supplied host adapter addresses to the image's IAT. This operates
    // only on a materialized byte buffer; it does not make memory executable.
    class ImportBinder final
    {
    public:
        static bool Bind(
            MappedPeImage* image,
            const PeImageInfo& metadata,
            const ImportResolver& resolver,
            BindingReport* report,
            std::wstring* error);
    };
}
}
