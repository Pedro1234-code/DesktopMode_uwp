#pragma once

#include "Bridge/CompatibilityCatalog.h"

namespace Win32Bridge
{
namespace Bridge
{
    // The bridge exposes a small LocalFolder-backed shell namespace.  Its
    // PIDLs and IShellFolder objects are guest-only values and never reveal
    // desktop host folders or host shell interfaces.
    PVOID WINAPI BridgeSHBrowseForFolderW(PVOID browseInfo);
    DWORD_PTR WINAPI BridgeSHGetFileInfoW(LPCWSTR path, DWORD attributes, PVOID info, UINT size, UINT flags);
    BOOL WINAPI BridgeSHGetPathFromIDListW(PVOID itemIdList, LPWSTR path);
    UINT WINAPI BridgeExtractIconExW(LPCWSTR fileName, int iconIndex, HICON* largeIcons, HICON* smallIcons, UINT iconCount);
    UINT WINAPI BridgeExtractIconExA(LPCSTR fileName, int iconIndex, HICON* largeIcons, HICON* smallIcons, UINT iconCount);
    HRESULT WINAPI BridgeSHGetDesktopFolder(PVOID* desktopFolder);
    HRESULT WINAPI BridgeSHGetSpecialFolderLocation(HWND owner, int folder, PVOID* itemIdList);
    BOOL WINAPI BridgeSHGetSpecialFolderPathW(HWND owner, LPWSTR path, int folder, BOOL create);
    int WINAPI BridgeSHFileOperationW(PVOID operation);
    void WINAPI BridgeSHChangeNotify(LONG eventId, UINT flags, LPCVOID first, LPCVOID second);
    BOOL WINAPI BridgeShellExecuteExW(PVOID executeInfo);
    HINSTANCE WINAPI BridgeShellExecuteW(HWND owner, LPCWSTR operation, LPCWSTR file, LPCWSTR parameters, LPCWSTR directory, INT showCommand);
    HRESULT WINAPI BridgeSHGetFolderPathW(HWND owner, int folder, HANDLE token, DWORD flags, LPWSTR path);
    void WINAPI BridgeDragAcceptFiles(HWND window, BOOL accept);
    void WINAPI BridgeDragFinish(HANDLE drop);
    void WINAPI BridgeSHAddToRecentDocs(UINT flags, LPCVOID data);
    HRESULT WINAPI BridgeSHCreateItemFromParsingName(PCWSTR path, PVOID bindContext, REFIID interfaceId, void** result);
    int WINAPI BridgeShellAboutW(HWND owner, LPCWSTR title, LPCWSTR text, HICON icon);
    UINT WINAPI BridgeDragQueryFileW(HANDLE drop, UINT file, LPWSTR path, UINT characterCount);
    ImportResolution ResolveShell32Import(const ImportedSymbol& symbol);
}
}
