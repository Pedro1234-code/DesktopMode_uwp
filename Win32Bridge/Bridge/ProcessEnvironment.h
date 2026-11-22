#pragma once
#include <windows.h>
#include <map>
#include <cwchar>
#include <mutex>
#include <string>
#include <vector>
#include <utility>

namespace Win32Bridge { namespace Bridge {

class ProcessEnvironment final
{
    struct NameLess
    {
        bool operator()(const std::wstring& a, const std::wstring& b) const
        {
            return ::CompareStringOrdinal(a.c_str(), -1, b.c_str(), -1, TRUE) == CSTR_LESS_THAN;
        }
    };
    using Variables = std::map<std::wstring, std::wstring, NameLess>;
public:
    void Initialize(const std::vector<std::wstring>& entries)
    {
        Variables variables;
        for (const auto& entry : entries)
        {
            // Internal drive-current-directory entries have names like '=C:'.
            const size_t equals = entry.find(L'=', entry.empty() || entry[0] != L'=' ? 0 : 1);
            if (equals == std::wstring::npos || !equals) continue;
            variables.emplace(entry.substr(0, equals), entry.substr(equals + 1));
        }
        std::lock_guard<std::mutex> guard(m_lock);
        m_variables.swap(variables);
    }
    bool Get(const std::wstring& name, std::wstring* value) const
    {
        std::lock_guard<std::mutex> guard(m_lock);
        const auto found = m_variables.find(name);
        if (found == m_variables.end()) return false;
        *value = found->second; return true;
    }
    DWORD Set(const wchar_t* name, const wchar_t* value)
    {
        if (!name || !*name || wcschr(name, L'=')) return ERROR_INVALID_PARAMETER;
        if (wcslen(name) > 32767 || (value && wcslen(value) > 32767)) return ERROR_INVALID_PARAMETER;
        const std::wstring variableName(name);
        std::wstring replacement = value ? value : L"";
        std::lock_guard<std::mutex> guard(m_lock);
        const auto found = m_variables.find(variableName);
        if (!value) { if (found != m_variables.end()) m_variables.erase(found); }
        else if (found == m_variables.end()) m_variables.emplace(variableName, std::move(replacement));
        else found->second.swap(replacement);
        return ERROR_SUCCESS;
    }
    std::vector<std::wstring> Snapshot() const
    {
        std::lock_guard<std::mutex> guard(m_lock);
        std::vector<std::wstring> result;
        for (const auto& entry : m_variables) result.push_back(entry.first + L"=" + entry.second);
        return result;
    }
    std::wstring Expand(const std::wstring& source) const
    {
        std::lock_guard<std::mutex> guard(m_lock);
        std::wstring result;
        size_t position = 0;
        while (position < source.size())
        {
            const size_t begin = source.find(L'%', position);
            if (begin == std::wstring::npos) { result.append(source, position, std::wstring::npos); break; }
            result.append(source, position, begin - position);
            const size_t end = source.find(L'%', begin + 1);
            if (end == std::wstring::npos) { result.append(source, begin, std::wstring::npos); break; }
            const auto found = m_variables.find(source.substr(begin + 1, end - begin - 1));
            if (found != m_variables.end()) result += found->second;
            else result.append(source, begin, end - begin + 1);
            position = end + 1;
        }
        return result;
    }
    std::vector<std::wstring> SearchDirectories() const
    {
        std::wstring path;
        if (!Get(L"PATH", &path)) return {};
        std::vector<std::wstring> directories;
        std::wstring directory;
        bool quoted = false;
        for (wchar_t character : path)
        {
            if (character == L'"') { quoted = !quoted; continue; }
            if (character == L';' && !quoted)
            {
                if (!directory.empty()) directories.push_back(std::move(directory));
                directory.clear();
            }
            else directory.push_back(character);
        }
        if (!directory.empty()) directories.push_back(std::move(directory));
        return directories;
    }
    std::vector<wchar_t> Block() const
    {
        const auto snapshot = Snapshot();
        std::vector<wchar_t> block;
        for (const auto& entry : snapshot)
        {
            block.insert(block.end(), entry.begin(), entry.end()); block.push_back(0);
        }
        if (block.empty()) block.push_back(0);
        block.push_back(0); return block;
    }
private:
    mutable std::mutex m_lock;
    Variables m_variables;
};

} }