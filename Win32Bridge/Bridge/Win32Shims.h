#pragma once

#include "Bridge\\CompatibilityCatalog.h"
#include "Bridge\\MouseInput.h"

#include <windows.h>

namespace Win32Bridge
{
namespace Bridge
{
    // A synchronous Win32 ABI adapter. Guest execution must occur on a worker
    // thread, while the adapter marshals presentation onto the UWP UI thread.
    int WINAPI BridgeMessageBoxW(HWND owner, LPCWSTR text, LPCWSTR caption, UINT type);
    BOOL WINAPI BridgeGetCursorPos(LPPOINT point);
    SHORT WINAPI BridgeGetAsyncKeyState(int virtualKey);
    SHORT WINAPI BridgeGetKeyState(int virtualKey);
    ULONGLONG MessageBoxWAdapterAddress();
    ImportResolution ResolveRuntimeImport(const ImportedSymbol& symbol);
}
}
