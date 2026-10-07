#pragma once

#include <cwctype>
#include <cwchar>
#include <string>

namespace Win32Bridge
{
namespace Bridge
{
    inline std::wstring ApiSetHostLibrary(const std::wstring& requested)
    {
        std::wstring name = requested;
        for (auto& character : name)
        {
            if (character == L'/') character = L'\\';
            character = static_cast<wchar_t>(towlower(character));
        }
        const size_t separator = name.find_last_of(L'\\');
        const std::wstring leaf = separator == std::wstring::npos
            ? name : name.substr(separator + 1);
        const bool apiSet = leaf.rfind(L"api-ms-win-", 0) == 0 ||
            leaf.rfind(L"ext-ms-win-", 0) == 0;
        if (!apiSet) return requested;

        if (leaf.rfind(L"api-ms-win-crt-", 0) == 0) return L"ucrtbase.dll";
        if (leaf.find(L"-ntuser-") != std::wstring::npos) return L"user32.dll";
        if (leaf.find(L"-gdi-") != std::wstring::npos) return L"gdi32.dll";
        if (leaf.find(L"-shell-") != std::wstring::npos) return L"shell32.dll";
        if (leaf.find(L"-shcore-") != std::wstring::npos) return L"shcore.dll";
        if (leaf.find(L"-com-") != std::wstring::npos ||
            leaf.find(L"-ole-") != std::wstring::npos) return L"ole32.dll";
        if (leaf.find(L"-security-") != std::wstring::npos ||
            leaf.find(L"-service-") != std::wstring::npos ||
            leaf.find(L"-eventing-") != std::wstring::npos ||
            leaf.find(L"-registry-") != std::wstring::npos ||
            leaf.find(L"-advapi32-") != std::wstring::npos) return L"advapi32.dll";
        if (leaf.find(L"-version-") != std::wstring::npos) return L"version.dll";
        if (leaf.find(L"-commctrl-") != std::wstring::npos) return L"comctl32.dll";
        if (leaf.find(L"-comdlg-") != std::wstring::npos) return L"comdlg32.dll";

        if (leaf.rfind(L"api-ms-win-core-", 0) == 0 ||
            leaf.rfind(L"api-ms-win-downlevel-kernel", 0) == 0 ||
            leaf.rfind(L"api-ms-win-appmodel-", 0) == 0)
        {
            // Core contracts are implemented by the kernel bridge. Kernel32
            // is the stable guest-visible host even when desktop Windows
            // internally forwards a contract to KernelBase.
            return L"kernel32.dll";
        }
        if (leaf.rfind(L"api-ms-win-ro-", 0) == 0 ||
            leaf.rfind(L"api-ms-win-rtcore-", 0) == 0)
            return L"combase.dll";

        // Unknown contracts must remain loadable from the virtual System32;
        // guessing kernel32 would silently bind an unrelated export.
        return requested;
    }

    inline bool IsApiSetLibrary(const std::wstring& requested)
    {
        const std::wstring host = ApiSetHostLibrary(requested);
        return _wcsicmp(host.c_str(), requested.c_str()) != 0;
    }
}
}
