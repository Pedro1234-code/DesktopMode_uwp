#pragma once

#include "Bridge\\PeImage.h"

namespace Win32Bridge
{
namespace Bridge
{
    enum class ImportDisposition
    {
        NeedsBridge,
        Deferred,
        Unsupported
    };

    struct ImportResolution
    {
        ImportDisposition disposition;
        std::wstring note;
        ULONGLONG targetAddress = 0;
    };

    // Central inventory for every PE guest. The future loader will use the same
    // catalog to choose an IAT target after an import has been resolved.
    class CompatibilityCatalog final
    {
    public:
        static ImportResolution Resolve(const ImportedSymbol& symbol);
    };
}
}
