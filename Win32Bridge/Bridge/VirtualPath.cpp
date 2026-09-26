#include "Bridge\\VirtualPath.h"

#include <cwctype>

using namespace Win32Bridge::Bridge;

namespace
{
    constexpr size_t MaxGuestPathCharacters = 32767;

    void SetError(std::wstring* error, const std::wstring& message)
    {
        if (error)
        {
            *error = message;
        }
    }

    std::wstring NormalizeSeparators(std::wstring value)
    {
        for (auto& character : value)
        {
            if (character == L'/')
            {
                character = L'\\';
            }
        }
        return value;
    }

    bool IsDrivePrefix(const std::wstring& value)
    {
        return value.size() >= 2 && std::iswalpha(value[0]) && value[1] == L':';
    }

    std::vector<std::wstring> ComponentsOfCanonicalPath(const std::wstring& value)
    {
        std::vector<std::wstring> components;
        size_t cursor = 3; // Skip the C: root prefix in a canonical path.
        while (cursor < value.size())
        {
            const size_t separator = value.find(L'\\', cursor);
            const size_t length = (separator == std::wstring::npos ? value.size() : separator) - cursor;
            if (length != 0)
            {
                components.push_back(value.substr(cursor, length));
            }
            if (separator == std::wstring::npos)
            {
                break;
            }
            cursor = separator + 1;
        }
        return components;
    }

    std::wstring CanonicalPath(const std::vector<std::wstring>& components)
    {
        std::wstring result = L"C:\\";
        for (size_t index = 0; index < components.size(); ++index)
        {
            if (index != 0)
            {
                result += L'\\';
            }
            result += components[index];
        }
        return result;
    }

    bool IsForbiddenComponent(const std::wstring& component)
    {
        if (component.empty())
        {
            return true;
        }

        for (const wchar_t character : component)
        {
            if (character < 0x20 || character == L'<' || character == L'>' ||
                character == L'"' || character == L'|' || character == L'?' ||
                character == L'*' || character == L':')
            {
                return true;
            }
        }
        return false;
    }
}

bool VirtualPathResolver::Resolve(const wchar_t* guestPath, GuestPath* resolved, std::wstring* error) const
{
    if (!guestPath || !resolved)
    {
        SetError(error, L"A non-null guest path and output object are required.");
        return false;
    }

    const size_t length = wcsnlen_s(guestPath, MaxGuestPathCharacters + 1);
    if (length == 0 || length > MaxGuestPathCharacters)
    {
        SetError(error, L"The guest path is empty or exceeds the virtual path limit.");
        return false;
    }

    std::lock_guard<std::mutex> guard(m_lock);
    return Parse(std::wstring(guestPath, length), m_currentDirectory, resolved, error);
}

bool VirtualPathResolver::SetCurrentDirectoryPath(const GuestPath& directory, std::wstring* error)
{
    if (directory.canonical.empty() || directory.components.empty() && directory.canonical != L"C:\\")
    {
        SetError(error, L"The current directory must be a canonical virtual C: path.");
        return false;
    }

    std::lock_guard<std::mutex> guard(m_lock);
    m_currentDirectory = directory.canonical;
    return true;
}

std::wstring VirtualPathResolver::CurrentDirectory() const
{
    std::lock_guard<std::mutex> guard(m_lock);
    return m_currentDirectory;
}

bool VirtualPathResolver::Parse(
    const std::wstring& rawInput,
    const std::wstring& currentDirectory,
    GuestPath* resolved,
    std::wstring* error)
{
    std::wstring input = NormalizeSeparators(rawInput);
    // File managers commonly use the Win32 extended-path spelling even for
    // ordinary local paths (for example \\?\\C:\\folder). It is not a host
    // device path in the guest: after removing that spelling it must still
    // resolve within the bridge's sole virtual C: drive. Keep UNC forms
    // rejected below, including \\?\\UNC\\..., so this never broadens the
    // guest's storage authority.
    if (input.rfind(L"\\\\?\\", 0) == 0 || input.rfind(L"\\\\.\\", 0) == 0)
    {
        input.erase(0, 4);
    }
    if (input.rfind(L"\\\\", 0) == 0)
    {
        SetError(error, L"UNC and device paths are not available in the virtual drive.");
        return false;
    }

    std::vector<std::wstring> components;
    size_t cursor = 0;
    if (IsDrivePrefix(input))
    {
        if (std::towlower(input[0]) != L'c')
        {
            SetError(error, L"Only the virtual C: drive is available.");
            return false;
        }

        cursor = 2;
        if (cursor < input.size() && input[cursor] == L'\\')
        {
            ++cursor;
        }
        else
        {
            // C:relative paths use the guest's current directory, as Win32 does.
            components = ComponentsOfCanonicalPath(currentDirectory);
        }
    }
    else if (!input.empty() && input.front() == L'\\')
    {
        cursor = 1;
    }
    else
    {
        components = ComponentsOfCanonicalPath(currentDirectory);
    }

    while (cursor <= input.size())
    {
        const size_t separator = input.find(L'\\', cursor);
        const size_t end = separator == std::wstring::npos ? input.size() : separator;
        const std::wstring component = input.substr(cursor, end - cursor);
        if (!component.empty() && component != L".")
        {
            if (component == L"..")
            {
                if (components.empty())
                {
                    SetError(error, L"The guest path attempts to escape C:\\.");
                    return false;
                }
                components.pop_back();
            }
            else if (IsForbiddenComponent(component))
            {
                SetError(error, L"The guest path contains a forbidden path component.");
                return false;
            }
            else
            {
                components.push_back(component);
            }
        }

        if (separator == std::wstring::npos)
        {
            break;
        }
        cursor = separator + 1;
    }

    resolved->components = std::move(components);
    resolved->canonical = CanonicalPath(resolved->components);
    return true;
}
