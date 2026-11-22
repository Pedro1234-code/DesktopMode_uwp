#pragma once

#include "Bridge/ImportBinder.h"

namespace Win32Bridge { namespace Bridge {
    bool HasHostShaderCompiler();
    ImportResolution ResolveGraphicsImport(const ImportedSymbol& symbol);
} }
