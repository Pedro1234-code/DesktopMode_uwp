#pragma once
#include "Bridge/CompatibilityCatalog.h"
#include "Bridge/CRT/CrtLocaleData.h"
#include "Bridge/CRT/CrtMultibyteData.h"
#include "Bridge/CRT/CrtRestartableConversion.h"
#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <cwchar>
#include <cmath>
#include <fenv.h>
#include <ctime>
#include <malloc.h>
#include <ctype.h>
#include <wctype.h>
#include <errno.h>

namespace Win32Bridge { namespace Bridge {
inline ImportResolution ResolveCrtDataImport(const ImportedSymbol& symbol)
{
    ImportResolution result{ImportDisposition::Unsupported, L"No CRT data implementation.", 0};
    if (symbol.importedByOrdinal || (_wcsicmp(symbol.library.c_str(), L"msvcrt.dll") != 0 &&
        _wcsicmp(symbol.library.c_str(), L"ucrtbase.dll") != 0)) return result;
    struct Entry { const wchar_t* name; ULONGLONG address; };
    static const Entry entries[] = {
// Legacy CRT exports intentionally retain their ABI (strcpy, strcat, etc.).
// Suppress MSVC's deprecation diagnostic only while taking these addresses.
#pragma warning(push)
#pragma warning(disable: 4996)
#include "Bridge/CRT/CrtDataExports.inl"
#pragma warning(pop)
    };
    const auto end = entries + sizeof(entries) / sizeof(entries[0]);
    const auto found = std::lower_bound(entries, end, symbol.name.c_str(),
        [](const Entry& entry, const wchar_t* name) { return std::wcscmp(entry.name, name) < 0; });
    if (found != end && symbol.name == found->name)
    {
        result.targetAddress = found->address;
        result.disposition = ImportDisposition::NeedsBridge;
        result.note = L"CRT data adapter: " + symbol.name + L".";
    }
    return result;
}
} }
