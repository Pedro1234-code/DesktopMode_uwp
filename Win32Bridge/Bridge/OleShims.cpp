#include "pch.h"
#include "Bridge/OleShims.h"
#include "Bridge/Gdi32Shims.h"
#include "Bridge/Kernel32Shims.h"
#include "Bridge/RuntimeDiagnostics.h"
#include "Bridge/User32Shims.h"

#include <mutex>
#include <unordered_map>

using namespace Win32Bridge::Bridge;
namespace
{
    thread_local unsigned g_comCount = 0;
    thread_local DWORD g_comApartment = 0xffffffffu;
    std::mutex g_dropTargetsLock;
    std::unordered_map<ULONG_PTR, IUnknown*> g_dropTargets;
    bool Name(const std::wstring& value, const wchar_t* expected)
    {
        return _wcsicmp(value.c_str(), expected) == 0;
    }
    bool GuestComAddRef(IUnknown* value)
    {
        __try { value->AddRef(); return true; }
        __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
    }
    void GuestComRelease(IUnknown* value)
    {
        if (!value) return;
        __try { value->Release(); }
        __except (EXCEPTION_EXECUTE_HANDLER) { }
    }
}

HRESULT WINAPI Win32Bridge::Bridge::BridgeCoInitialize(LPVOID reserved)
{
    return BridgeCoInitializeEx(reserved, COINIT_APARTMENTTHREADED);
}
HRESULT WINAPI Win32Bridge::Bridge::BridgeCoInitializeEx(LPVOID, DWORD flags)
{
    const DWORD apartment = flags & (COINIT_APARTMENTTHREADED | COINIT_MULTITHREADED);
    if (g_comCount && apartment != g_comApartment) return RPC_E_CHANGED_MODE;
    if (!g_comCount) g_comApartment = apartment;
    ++g_comCount;
    return g_comCount == 1 ? S_OK : S_FALSE;
}
HRESULT WINAPI Win32Bridge::Bridge::BridgeOleInitialize(LPVOID reserved)
{
    return BridgeCoInitializeEx(reserved, COINIT_APARTMENTTHREADED);
}
void WINAPI Win32Bridge::Bridge::BridgeCoUninitialize()
{
    if (g_comCount && --g_comCount == 0) g_comApartment = 0xffffffffu;
}
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
void WINAPI Win32Bridge::Bridge::BridgeReleaseStgMedium(PVOID value)
{
    if (!value) return;
    auto* medium = static_cast<STGMEDIUM*>(value);
    if (medium->pUnkForRelease)
    {
        GuestComRelease(medium->pUnkForRelease);
    }
    else
    {
        switch (medium->tymed)
        {
        case TYMED_HGLOBAL:
            if (medium->hGlobal) BridgeGlobalFree(medium->hGlobal);
            break;
        case TYMED_GDI:
            if (medium->hBitmap) BridgeDeleteObject(medium->hBitmap);
            break;
        case TYMED_FILE:
            if (medium->lpszFileName) BridgeCoTaskMemFree(medium->lpszFileName);
            break;
        case TYMED_ISTREAM:
            GuestComRelease(medium->pstm);
            break;
        case TYMED_ISTORAGE:
            GuestComRelease(medium->pstg);
            break;
        default:
            break;
        }
    }
    memset(medium, 0, sizeof(*medium));
    medium->tymed = TYMED_NULL;
}
HRESULT WINAPI Win32Bridge::Bridge::BridgeRegisterDragDrop(HWND window, PVOID target)
{
    if (!BridgeIsWindow(window)) return DRAGDROP_E_INVALIDHWND;
    if (!target) return E_INVALIDARG;
    auto* unknown = static_cast<IUnknown*>(target);
    if (!GuestComAddRef(unknown)) return E_UNEXPECTED;
    {
        std::lock_guard<std::mutex> guard(g_dropTargetsLock);
        const ULONG_PTR key = reinterpret_cast<ULONG_PTR>(window);
        if (g_dropTargets.find(key) != g_dropTargets.end())
        {
            GuestComRelease(unknown);
            return DRAGDROP_E_ALREADYREGISTERED;
        }
        g_dropTargets.emplace(key, unknown);
    }
    return S_OK;
}
HRESULT WINAPI Win32Bridge::Bridge::BridgeRevokeDragDrop(HWND window)
{
    IUnknown* target = nullptr;
    {
        std::lock_guard<std::mutex> guard(g_dropTargetsLock);
        const auto found = g_dropTargets.find(reinterpret_cast<ULONG_PTR>(window));
        if (found == g_dropTargets.end()) return DRAGDROP_E_NOTREGISTERED;
        target = found->second;
        g_dropTargets.erase(found);
    }
    GuestComRelease(target);
    return S_OK;
}
HRESULT WINAPI Win32Bridge::Bridge::BridgeDoDragDrop(PVOID, PVOID, DWORD, DWORD* effect) { if (effect) *effect = 0; return E_NOTIMPL; }
BSTR WINAPI Win32Bridge::Bridge::BridgeSysAllocString(LPCOLESTR value) { return ::SysAllocString(value); }
BSTR WINAPI Win32Bridge::Bridge::BridgeSysAllocStringLen(const OLECHAR* value, UINT length) { return ::SysAllocStringLen(value, length); }
BSTR WINAPI Win32Bridge::Bridge::BridgeSysAllocStringByteLen(LPCSTR value, UINT length) { return ::SysAllocStringByteLen(value, length); }
void WINAPI Win32Bridge::Bridge::BridgeSysFreeString(BSTR value) { ::SysFreeString(value); }
UINT WINAPI Win32Bridge::Bridge::BridgeSysStringLen(BSTR value) { return ::SysStringLen(value); }
UINT WINAPI Win32Bridge::Bridge::BridgeSysStringByteLen(BSTR value) { return ::SysStringByteLen(value); }
HRESULT WINAPI Win32Bridge::Bridge::BridgeVariantClear(VARIANTARG* value) { return value ? ::VariantClear(value) : E_INVALIDARG; }
HRESULT WINAPI Win32Bridge::Bridge::BridgeVariantCopy(VARIANTARG* destination, const VARIANTARG* source) { return destination && source ? ::VariantCopy(destination, source) : E_INVALIDARG; }
HRESULT WINAPI BridgeCLSIDFromProgID(LPCOLESTR progId, LPCLSID clsid)
{
    if (!clsid) return E_POINTER;
    *clsid = CLSID_NULL;
    if (!progId || !*progId) return CO_E_CLASSSTRING;
    // COM registration belongs to the guest registry.  Until that registry is
    // connected to the class factory, fail explicitly instead of consulting or
    // leaking the host machine's COM catalogue.
    RuntimeDiagnostics::Record(L"COM: CLSIDFromProgID has no guest registration for " + std::wstring(progId));
    return REGDB_E_CLASSNOTREG;
}

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
        else if (Name(symbol.name, L"clsidfromprogid")) result.targetAddress = reinterpret_cast<ULONGLONG>(&BridgeCLSIDFromProgID);
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
