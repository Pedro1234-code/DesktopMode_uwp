#pragma once

#include "Bridge/CompatibilityCatalog.h"

namespace Win32Bridge
{
namespace Bridge
{
    DWORD WINAPI BridgeWNetOpenEnumW(DWORD scope, DWORD type, DWORD usage,
        LPVOID networkResource, LPHANDLE enumeration);
    DWORD WINAPI BridgeWNetEnumResourceW(HANDLE enumeration, LPDWORD count,
        LPVOID buffer, LPDWORD bufferSize);
    DWORD WINAPI BridgeWNetCloseEnum(HANDLE enumeration);
    DWORD WINAPI BridgeWNetAddConnection2W(LPVOID networkResource,
        LPCWSTR password, LPCWSTR userName, DWORD flags);
    DWORD WINAPI BridgeWNetGetResourceParentW(LPVOID networkResource,
        LPVOID buffer, LPDWORD bufferSize);
    DWORD WINAPI BridgeWNetGetResourceInformationW(LPVOID networkResource,
        LPVOID buffer, LPDWORD bufferSize, LPWSTR* systemPart);

    ImportResolution ResolveMprImport(const ImportedSymbol& symbol);
}
}
