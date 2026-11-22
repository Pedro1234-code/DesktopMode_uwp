#pragma once

#include "Bridge/CompatibilityCatalog.h"
#include "Bridge/ExportAliases.h"
#include <oleauto.h>
#include <algorithm>
#include <cwchar>

namespace Win32Bridge { namespace Bridge {

// SAFEARRAY, VARIANT and BSTR have the Windows x64 ABI and native allocator
// representation throughout the bridge. Their native implementations may also
// call the guest's IUnknown, IDispatch or IRecordInfo through those COM vtables.
inline ImportResolution ResolveAutomationDataImport(const ImportedSymbol& symbol)
{
    ImportResolution result{ ImportDisposition::Unsupported, L"No Automation data export.", 0 };
    if (_wcsicmp(symbol.library.c_str(), L"oleaut32.dll") != 0) return result;
    ImportedSymbol named = symbol;
    if (named.importedByOrdinal && !ExportAliases::Redirect(symbol, &named)) return result;
    if (named.importedByOrdinal || _wcsicmp(named.library.c_str(), L"oleaut32.dll") != 0) return result;
    struct Entry { const wchar_t* name; ULONGLONG address; };
#define BRIDGE_AUTOMATION_WIDE(text) L##text
#define BRIDGE_AUTOMATION_EXPORT(name) { BRIDGE_AUTOMATION_WIDE(#name), reinterpret_cast<ULONGLONG>(&::name) },
    static const Entry entries[] = {
#include "Bridge/AutomationExports.inl"
    };
#undef BRIDGE_AUTOMATION_EXPORT
#undef BRIDGE_AUTOMATION_WIDE
    const auto end = entries + sizeof(entries) / sizeof(entries[0]);
    const auto found = std::lower_bound(entries, end, named.name.c_str(),
        [](const Entry& entry, const wchar_t* name) { return std::wcscmp(entry.name, name) < 0; });
    if (found != end && named.name == found->name)
    {
        result.disposition = ImportDisposition::NeedsBridge;
        result.targetAddress = found->address;
        result.note = L"UWP-native OLE Automation data implementation: " + named.name + L".";
    }
    return result;
}

} }