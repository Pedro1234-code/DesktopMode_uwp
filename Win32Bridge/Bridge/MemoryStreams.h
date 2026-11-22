#pragma once

#include "Bridge/CompatibilityCatalog.h"
#include <objidl.h>

namespace Win32Bridge { namespace Bridge {

IStream* WINAPI BridgeSHCreateMemStream(const BYTE* initialData, UINT initialBytes);
HRESULT WINAPI BridgeIStreamRead(IStream* stream, void* destination, ULONG bytes);
HRESULT WINAPI BridgeIStreamWrite(IStream* stream, const void* source, ULONG bytes);
HRESULT WINAPI BridgeIStreamReset(IStream* stream);
HRESULT WINAPI BridgeIStreamSize(IStream* stream, ULARGE_INTEGER* size);
ImportResolution ResolveMemoryStreamImport(const ImportedSymbol& symbol);

} }