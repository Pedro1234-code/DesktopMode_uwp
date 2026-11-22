#pragma once
#include "Bridge/CompatibilityCatalog.h"
namespace Win32Bridge { namespace Bridge {
bool IsGuestWicFactoryClass(REFCLSID clsid);
HRESULT CreateGuestWicFactory(REFIID iid, void** result);
ImportResolution ResolveWindowsCodecsImport(const ImportedSymbol& symbol);
} }