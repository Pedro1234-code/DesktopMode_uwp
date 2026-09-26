#pragma once

#include "Bridge/CompatibilityCatalog.h"

namespace Win32Bridge
{
namespace Bridge
{
    ImportResolution ResolveMsvcrtImport(const ImportedSymbol& symbol);
}
}
