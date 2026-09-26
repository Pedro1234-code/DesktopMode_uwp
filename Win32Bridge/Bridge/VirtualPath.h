#pragma once

#include <string>
#include <vector>
#include <mutex>

namespace Win32Bridge
{
namespace Bridge
{
    // A canonical path is deliberately kept separate from a host path.  Guest
    // code never receives a path outside the virtual C: drive.
    struct GuestPath final
    {
        std::wstring canonical;
        std::vector<std::wstring> components;
    };

    class VirtualPathResolver final
    {
    public:
        bool Resolve(const wchar_t* guestPath, GuestPath* resolved, std::wstring* error) const;
        // Avoid the Windows SDK SetCurrentDirectory macro: this is an
        // internal virtual-path operation, not the Win32 API export.
        bool SetCurrentDirectoryPath(const GuestPath& directory, std::wstring* error);
        std::wstring CurrentDirectory() const;

    private:
        static bool Parse(
            const std::wstring& input,
            const std::wstring& currentDirectory,
            GuestPath* resolved,
            std::wstring* error);

        mutable std::mutex m_lock;
        std::wstring m_currentDirectory = L"C:\\";
    };
}
}
