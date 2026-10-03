#include "pch.h"
#include "Bridge/OleShims.h"
#include "Bridge/RuntimeDiagnostics.h"

using namespace Win32Bridge::Bridge;
namespace { thread_local unsigned g_comCount = 0; bool Name(const std::wstring& value, const wchar_t* expected) { return _wcsicmp(value.c_str(), expected) == 0; } }

HRESULT WINAPI Win32Bridge::Bridge::BridgeCoInitialize(LPVOID) { ++g_comCount; return g_comCount == 1 ? S_OK : S_FALSE; }
HRESULT WINAPI Win32Bridge::Bridge::BridgeCoInitializeEx(LPVOID reserved, DWORD) { return BridgeCoInitialize(reserved); }
HRESULT WINAPI Win32Bridge::Bridge::BridgeOleInitialize(LPVOID reserved) { return BridgeCoInitialize(reserved); }
void WINAPI Win32Bridge::Bridge::BridgeCoUninitialize() { if (g_comCount) --g_comCount; }
void WINAPI Win32Bridge::Bridge::BridgeOleUninitialize() { BridgeCoUninitialize(); }
LPVOID WINAPI Win32Bridge::Bridge::BridgeCoTaskMemAlloc(SIZE_T size) { return ::CoTaskMemAlloc(size); }
void WINAPI Win32Bridge::Bridge::BridgeCoTaskMemFree(LPVOID memory) { ::CoTaskMemFree(memory); }
HRESULT WINAPI Win32Bridge::Bridge::BridgeCoCreateInstance(REFCLSID clsid, LPUNKNOWN, DWORD, REFIID iid, LPVOID* result)
{
    if (result) *result = nullptr;
    RuntimeDiagnostics::Record(L"COM: class " + std::to_wstring(clsid.Data1) +
        L" / interface " + std::to_wstring(iid.Data1) + L" is not registered.");
    return REGDB_E_CLASSNOTREG;
}
void WINAPI Win32Bridge::Bridge::BridgeReleaseStgMedium(PVOID) { }
HRESULT WINAPI Win32Bridge::Bridge::BridgeRegisterDragDrop(HWND, PVOID) { return E_NOTIMPL; }
HRESULT WINAPI Win32Bridge::Bridge::BridgeRevokeDragDrop(HWND) { return E_NOTIMPL; }
HRESULT WINAPI Win32Bridge::Bridge::BridgeDoDragDrop(PVOID, PVOID, DWORD, DWORD* effect) { if (effect) *effect = 0; return E_NOTIMPL; }
BSTR WINAPI Win32Bridge::Bridge::BridgeSysAllocString(LPCOLESTR value) { return ::SysAllocString(value); }
BSTR WINAPI Win32Bridge::Bridge::BridgeSysAllocStringLen(const OLECHAR* value, UINT length) { return ::SysAllocStringLen(value, length); }
BSTR WINAPI Win32Bridge::Bridge::BridgeSysAllocStringByteLen(LPCSTR value, UINT length) { return ::SysAllocStringByteLen(value, length); }
void WINAPI Win32Bridge::Bridge::BridgeSysFreeString(BSTR value) { ::SysFreeString(value); }
UINT WINAPI Win32Bridge::Bridge::BridgeSysStringLen(BSTR value) { return ::SysStringLen(value); }
UINT WINAPI Win32Bridge::Bridge::BridgeSysStringByteLen(BSTR value) { return ::SysStringByteLen(value); }
HRESULT WINAPI Win32Bridge::Bridge::BridgeVariantClear(VARIANTARG* value) { return value ? ::VariantClear(value) : E_INVALIDARG; }
HRESULT WINAPI Win32Bridge::Bridge::BridgeVariantCopy(VARIANTARG* destination, const VARIANTARG* source) { return destination && source ? ::VariantCopy(destination, source) : E_INVALIDARG; }

ImportResolution Win32Bridge::Bridge::ResolveOleImport(const ImportedSymbol& symbol)
{
    auto result = CompatibilityCatalog::Resolve(symbol);
    if (Name(symbol.library, L"ole32.dll"))
    {
        if (Name(symbol.name, L"coinitialize")) result.targetAddress = reinterpret_cast<ULONGLONG>(&BridgeCoInitialize);
        else if (Name(symbol.name, L"coinitializeex")) result.targetAddress = reinterpret_cast<ULONGLONG>(&BridgeCoInitializeEx);
        else if (Name(symbol.name, L"oleinitialize")) result.targetAddress = reinterpret_cast<ULONGLONG>(&BridgeOleInitialize);
        else if (Name(symbol.name, L"couninitialize")) result.targetAddress = reinterpret_cast<ULONGLONG>(&BridgeCoUninitialize);
        else if (Name(symbol.name, L"oleuninitialize")) result.targetAddress = reinterpret_cast<ULONGLONG>(&BridgeOleUninitialize);
        else if (Name(symbol.name, L"cotaskmemalloc")) result.targetAddress = reinterpret_cast<ULONGLONG>(&BridgeCoTaskMemAlloc);
        else if (Name(symbol.name, L"cotaskmemfree")) result.targetAddress = reinterpret_cast<ULONGLONG>(&BridgeCoTaskMemFree);
        else if (Name(symbol.name, L"cocreateinstance")) result.targetAddress = reinterpret_cast<ULONGLONG>(&BridgeCoCreateInstance);
        else if (Name(symbol.name, L"releasestgmedium")) result.targetAddress = reinterpret_cast<ULONGLONG>(&BridgeReleaseStgMedium);
        else if (Name(symbol.name, L"registerdragdrop")) result.targetAddress = reinterpret_cast<ULONGLONG>(&BridgeRegisterDragDrop);
        else if (Name(symbol.name, L"revokedragdrop")) result.targetAddress = reinterpret_cast<ULONGLONG>(&BridgeRevokeDragDrop);
        else if (Name(symbol.name, L"dodragdrop")) result.targetAddress = reinterpret_cast<ULONGLONG>(&BridgeDoDragDrop);
    }
    else if (Name(symbol.library, L"oleaut32.dll") && symbol.importedByOrdinal)
    {
        switch (symbol.ordinal)
        {
        case 2: result.targetAddress = reinterpret_cast<ULONGLONG>(&BridgeSysAllocString); break;
        case 4: result.targetAddress = reinterpret_cast<ULONGLONG>(&BridgeSysAllocStringLen); break;
        case 6: result.targetAddress = reinterpret_cast<ULONGLONG>(&BridgeSysFreeString); break;
        case 7: result.targetAddress = reinterpret_cast<ULONGLONG>(&BridgeSysStringLen); break;
        case 9: result.targetAddress = reinterpret_cast<ULONGLONG>(&BridgeVariantClear); break;
        case 10: result.targetAddress = reinterpret_cast<ULONGLONG>(&BridgeVariantCopy); break;
        case 149: result.targetAddress = reinterpret_cast<ULONGLONG>(&BridgeSysStringByteLen); break;
        case 150: result.targetAddress = reinterpret_cast<ULONGLONG>(&BridgeSysAllocStringByteLen); break;
        }
    }
    if (result.targetAddress) result.disposition = ImportDisposition::NeedsBridge;
    return result;
}
