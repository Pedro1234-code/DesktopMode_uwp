#pragma once

#include "Bridge/CompatibilityCatalog.h"

#include <windows.h>

namespace Win32Bridge
{
namespace Bridge
{
    DWORD WINAPI BridgeGetFileVersionInfoSizeW(LPCWSTR fileName, LPDWORD handle);
    DWORD WINAPI BridgeGetFileVersionInfoSizeA(LPCSTR fileName, LPDWORD handle);
    DWORD WINAPI BridgeGetFileVersionInfoSizeExW(DWORD flags, LPCWSTR fileName, LPDWORD handle);
    DWORD WINAPI BridgeGetFileVersionInfoSizeExA(DWORD flags, LPCSTR fileName, LPDWORD handle);
    BOOL WINAPI BridgeGetFileVersionInfoW(LPCWSTR fileName, DWORD handle, DWORD length, LPVOID data);
    BOOL WINAPI BridgeGetFileVersionInfoA(LPCSTR fileName, DWORD handle, DWORD length, LPVOID data);
    BOOL WINAPI BridgeGetFileVersionInfoExW(DWORD flags, LPCWSTR fileName, DWORD handle, DWORD length, LPVOID data);
    BOOL WINAPI BridgeGetFileVersionInfoExA(DWORD flags, LPCSTR fileName, DWORD handle, DWORD length, LPVOID data);
    BOOL WINAPI BridgeVerQueryValueW(LPCVOID block, LPCWSTR subBlock, LPVOID* value, PUINT length);
    BOOL WINAPI BridgeVerQueryValueA(LPCVOID block, LPCSTR subBlock, LPVOID* value, PUINT length);
    DWORD WINAPI BridgeVerLanguageNameW(DWORD language, LPWSTR buffer, DWORD count);
    DWORD WINAPI BridgeVerLanguageNameA(DWORD language, LPSTR buffer, DWORD count);

    ImportResolution ResolveVersionImport(const ImportedSymbol& symbol);
}
}
