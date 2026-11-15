#pragma once

#include <windows.h>
#include <cstring>
#include <string>
#include <utility>
#include <vector>

namespace Win32Bridge { namespace Bridge {

constexpr size_t MaximumShellLinkSize = 1024 * 1024;

struct ShellLinkData
{
    std::wstring target, relativePath, description, workingDirectory, arguments, iconPath;
    LONG iconIndex = 0;
    DWORD showCommand = SW_SHOWNORMAL;
    WORD hotKey = 0;
};

inline bool ParseShellLink(const std::vector<BYTE>& bytes, ShellLinkData& result)
{
    constexpr size_t HeaderSize = 0x4c;
    constexpr BYTE ClassId[] = { 1, 0x14, 2, 0, 0, 0, 0, 0, 0xc0, 0, 0, 0, 0, 0, 0, 0x46 };
    if (bytes.size() < HeaderSize || bytes.size() > MaximumShellLinkSize ||
        std::memcmp(bytes.data() + 4, ClassId, sizeof(ClassId)) != 0)
        return false;

    const auto read = [&bytes](size_t offset, auto& value)
    {
        if (offset > bytes.size() || sizeof(value) > bytes.size() - offset) return false;
        std::memcpy(&value, bytes.data() + offset, sizeof(value));
        return true;
    };
    const auto decode = [&bytes](size_t offset, size_t count, bool unicode, std::wstring& value)
    {
        value.clear();
        if (!count) return true;
        if (unicode)
        {
            value.resize(count);
            std::memcpy(&value[0], bytes.data() + offset, count * sizeof(wchar_t));
        }
        else
        {
            const auto* input = reinterpret_cast<const char*>(bytes.data() + offset);
            const int length = MultiByteToWideChar(CP_ACP, 0, input, static_cast<int>(count), nullptr, 0);
            if (!length) return false;
            value.resize(length);
            if (!MultiByteToWideChar(CP_ACP, 0, input, static_cast<int>(count), &value[0], length))
                return false;
        }
        return value.find(L'\0') == std::wstring::npos;
    };

    DWORD headerSize = 0, flags = 0;
    read(0, headerSize);
    if (headerSize != HeaderSize) return false;
    read(20, flags);
    ShellLinkData link;
    read(56, link.iconIndex);
    read(60, link.showCommand);
    read(64, link.hotKey);
    size_t cursor = HeaderSize;
    if (flags & 1) // LinkTargetIDList
    {
        WORD size = 0;
        if (!read(cursor, size) || size > bytes.size() - cursor - sizeof(size)) return false;
        cursor += sizeof(size) + size;
    }
    if (flags & 2) // LinkInfo
    {
        DWORD size = 0, header = 0, infoFlags = 0;
        if (!read(cursor, size) || size < 0x1c || size > bytes.size() - cursor ||
            !read(cursor + 4, header) || header < 0x1c || header > size ||
            (header > 0x1c && header < 0x24))
            return false;
        read(cursor + 8, infoFlags);
        if ((infoFlags & 1) && !(flags & 0x100)) // Local path, unless ForceNoLinkInfo
        {
            const auto pathString = [&](size_t ansiField, size_t unicodeField, std::wstring& value)
            {
                DWORD offset = 0;
                if (header >= 0x24) read(cursor + unicodeField, offset);
                const bool unicode = offset != 0;
                if (!unicode) read(cursor + ansiField, offset);
                if (offset < header || offset >= size) return false;
                const size_t unit = unicode ? 2 : 1;
                for (size_t end = offset; unit <= size - end; end += unit)
                {
                    const size_t index = cursor + end;
                    if (!bytes[index] && (!unicode || !bytes[index + 1]))
                        return decode(cursor + offset, (end - offset) / unit, unicode, value);
                }
                return false;
            };
            std::wstring suffix;
            if (!pathString(16, 28, link.target) || !pathString(24, 32, suffix)) return false;
            link.target += suffix;
        }
        cursor += size;
    }

    std::wstring* fields[] = { &link.description, &link.relativePath, &link.workingDirectory,
        &link.arguments, &link.iconPath };
    const bool unicode = (flags & 0x80) != 0;
    for (size_t index = 0; index < ARRAYSIZE(fields); ++index)
    {
        if (!(flags & (4u << index))) continue;
        WORD count = 0;
        if (!read(cursor, count)) return false;
        cursor += sizeof(count);
        const size_t length = count * (unicode ? 2u : 1u);
        if (length > bytes.size() - cursor || !decode(cursor, count, unicode, *fields[index]))
            return false;
        cursor += length;
    }
    while (cursor < bytes.size())
    {
        DWORD blockSize = 0, signature = 0;
        if (!read(cursor, blockSize)) return false;
        if (blockSize < sizeof(DWORD)) break; // TerminalBlock
        if (blockSize < 8 || blockSize > bytes.size() - cursor ||
            !read(cursor + 4, signature))
            return false;
        if (signature == 0xa0000001 && blockSize == 0x314 &&
            (link.target.empty() || (flags & 0x02000000))) // EnvironmentVariableDataBlock
        {
            constexpr size_t UnicodeOffset = 8 + 260;
            const auto terminated = [&](size_t start, size_t unit, std::wstring& path)
            {
                for (size_t end = start; unit <= blockSize - end; end += unit)
                    if (!bytes[cursor + end] && (unit == 1 || !bytes[cursor + end + 1]))
                        return end != start && decode(cursor + start, (end - start) / unit, unit == 2, path);
                return false;
            };
            std::wstring path;
            if (terminated(UnicodeOffset, 2, path))
                link.target = std::move(path);
            else
            {
                size_t length = 0;
                while (length < 260 && bytes[cursor + 8 + length]) ++length;
                if (length > 0 && length < 260 && decode(cursor + 8, length, false, path))
                    link.target = std::move(path);
            }
        }
        cursor += blockSize;
    }
    result = std::move(link);
    return true;
}

} }
