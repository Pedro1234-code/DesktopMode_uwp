#include "pch.h"
#include "Bridge/MprShims.h"

using namespace Win32Bridge::Bridge;

namespace
{
    constexpr DWORD ErrorInvalidHandle = 6;
    constexpr DWORD ErrorBadNetName = 67;
    constexpr DWORD ErrorInvalidParameter = 87;
    constexpr DWORD ErrorNoMoreItems = 259;
    constexpr DWORD ErrorNoNetwork = 1222;

    BYTE g_emptyNetworkEnumeration = 0;

    HANDLE EmptyNetworkEnumeration()
    {
        return reinterpret_cast<HANDLE>(&g_emptyNetworkEnumeration);
    }

    bool Name(const std::wstring& value, const wchar_t* expected)
    {
        return _wcsicmp(value.c_str(), expected) == 0;
    }
}

DWORD WINAPI Win32Bridge::Bridge::BridgeWNetOpenEnumW(
    DWORD, DWORD, DWORD, LPVOID, LPHANDLE enumeration)
{
    if (!enumeration) return ErrorInvalidParameter;

    // The sandbox has no Windows network-provider namespace.  Represent it
    // as a valid, empty enumeration so callers can continue normally.
    *enumeration = EmptyNetworkEnumeration();
    return NO_ERROR;
}

DWORD WINAPI Win32Bridge::Bridge::BridgeWNetEnumResourceW(
    HANDLE enumeration, LPDWORD count, LPVOID, LPDWORD bufferSize)
{
    if (enumeration != EmptyNetworkEnumeration()) return ErrorInvalidHandle;
    if (!count || !bufferSize) return ErrorInvalidParameter;

    *count = 0;
    return ErrorNoMoreItems;
}

DWORD WINAPI Win32Bridge::Bridge::BridgeWNetCloseEnum(HANDLE enumeration)
{
    return enumeration == EmptyNetworkEnumeration() ? NO_ERROR : ErrorInvalidHandle;
}

DWORD WINAPI Win32Bridge::Bridge::BridgeWNetAddConnection2W(
    LPVOID, LPCWSTR, LPCWSTR, DWORD)
{
    return ErrorNoNetwork;
}

DWORD WINAPI Win32Bridge::Bridge::BridgeWNetGetResourceParentW(
    LPVOID, LPVOID, LPDWORD bufferSize)
{
    if (!bufferSize) return ErrorInvalidParameter;
    return ErrorBadNetName;
}

DWORD WINAPI Win32Bridge::Bridge::BridgeWNetGetResourceInformationW(
    LPVOID, LPVOID, LPDWORD bufferSize, LPWSTR* systemPart)
{
    if (systemPart) *systemPart = nullptr;
    if (!bufferSize) return ErrorInvalidParameter;
    return ErrorBadNetName;
}

ImportResolution Win32Bridge::Bridge::ResolveMprImport(const ImportedSymbol& symbol)
{
    ImportResolution resolution = CompatibilityCatalog::Resolve(symbol);
    if (!Name(symbol.library, L"mpr.dll") || symbol.importedByOrdinal)
        return resolution;

    if (Name(symbol.name, L"wnetopenenumw"))
        resolution.targetAddress = reinterpret_cast<ULONGLONG>(&BridgeWNetOpenEnumW);
    else if (Name(symbol.name, L"wnetenumresourcew"))
        resolution.targetAddress = reinterpret_cast<ULONGLONG>(&BridgeWNetEnumResourceW);
    else if (Name(symbol.name, L"wnetcloseenum"))
        resolution.targetAddress = reinterpret_cast<ULONGLONG>(&BridgeWNetCloseEnum);
    else if (Name(symbol.name, L"wnetaddconnection2w"))
        resolution.targetAddress = reinterpret_cast<ULONGLONG>(&BridgeWNetAddConnection2W);
    else if (Name(symbol.name, L"wnetgetresourceparentw"))
        resolution.targetAddress = reinterpret_cast<ULONGLONG>(&BridgeWNetGetResourceParentW);
    else if (Name(symbol.name, L"wnetgetresourceinformationw"))
        resolution.targetAddress = reinterpret_cast<ULONGLONG>(&BridgeWNetGetResourceInformationW);

    if (resolution.targetAddress)
    {
        resolution.disposition = ImportDisposition::NeedsBridge;
        resolution.note = L"MPR adapter: expose an empty network-resource namespace without MPR.dll.";
    }
    return resolution;
}
