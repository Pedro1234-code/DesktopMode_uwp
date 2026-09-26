#pragma once
#include "Bridge/CompatibilityCatalog.h"
namespace Win32Bridge { namespace Bridge {
HRESULT WINAPI BridgeCoInitialize(LPVOID); HRESULT WINAPI BridgeOleInitialize(LPVOID); void WINAPI BridgeCoUninitialize(); void WINAPI BridgeOleUninitialize();
LPVOID WINAPI BridgeCoTaskMemAlloc(SIZE_T); void WINAPI BridgeCoTaskMemFree(LPVOID); HRESULT WINAPI BridgeCoCreateInstance(REFCLSID,LPUNKNOWN,DWORD,REFIID,LPVOID*);
void WINAPI BridgeReleaseStgMedium(PVOID); HRESULT WINAPI BridgeRegisterDragDrop(HWND,PVOID); HRESULT WINAPI BridgeRevokeDragDrop(HWND);
HRESULT WINAPI BridgeDoDragDrop(PVOID, PVOID, DWORD, DWORD*);
BSTR WINAPI BridgeSysAllocString(LPCOLESTR); BSTR WINAPI BridgeSysAllocStringLen(const OLECHAR*,UINT); void WINAPI BridgeSysFreeString(BSTR); UINT WINAPI BridgeSysStringLen(BSTR); UINT WINAPI BridgeSysStringByteLen(BSTR); HRESULT WINAPI BridgeVariantClear(VARIANTARG*); HRESULT WINAPI BridgeVariantCopy(VARIANTARG*,const VARIANTARG*);
ImportResolution ResolveOleImport(const ImportedSymbol&);
} }
