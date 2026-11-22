#pragma once

#include "Bridge/PeImage.h"

#include <algorithm>
#include <cwchar>

namespace Win32Bridge { namespace Bridge {

// Export metadata only. Addresses always come from an existing implementation
// in the runtime resolver. This catalog never creates a successful stub.
class ExportAliases final
{
public:
    static bool Redirect(const ImportedSymbol& source, ImportedSymbol* target)
    {
        if (!target) return false;
        // UCRT OS-mode wrappers retain the public export's calling convention.
        // Redirect only inside UCRT and only one wrapper level. The resolver
        // still requires a real implementation; unknown targets remain errors.
        if (_wcsicmp(source.library.c_str(), L"ucrtbase.dll") == 0 &&
            !source.importedByOrdinal && source.name.size() > 3 &&
            source.name.compare(0, 3, L"_o_") == 0 &&
            source.name.compare(3, 3, L"_o_") != 0)
        {
            *target = source; target->name.erase(0, 3); return true;
        }
        struct Entry
        {
            const wchar_t* library;
            const wchar_t* name;
            WORD ordinal;
            const wchar_t* targetLibrary;
            const wchar_t* targetName;
            WORD targetOrdinal;
        };
        static const Entry entries[] = {
#include "Bridge/GeneratedExportAliases.inl"
        };
        const auto end = entries + sizeof(entries) / sizeof(entries[0]);
        const auto first = std::lower_bound(entries, end, source.library.c_str(),
            [](const Entry& entry, const wchar_t* library)
            { return _wcsicmp(entry.library, library) < 0; });
        for (auto entry = first; entry != end && _wcsicmp(entry->library, source.library.c_str()) == 0; ++entry)
        {
            const bool matches = source.importedByOrdinal
                ? entry->ordinal != 0 && entry->ordinal == source.ordinal
                : entry->ordinal == 0 && source.name == entry->name;
            if (!matches) continue;
            *target = source;
            target->library = entry->targetLibrary;
            target->name = entry->targetName;
            target->ordinal = entry->targetOrdinal;
            target->importedByOrdinal = entry->targetOrdinal != 0;
            return true;
        }
        return false;
    }
};

} }