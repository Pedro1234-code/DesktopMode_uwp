#pragma once
#include <windows.h>
#include <cstdint>
#include <cstring>
#include <vector>

namespace Win32Bridge { namespace Bridge {

// The x64 CRT spawn packet is an int32 count, a byte per descriptor,
// then a packed array of HANDLEs. Array indexes are descriptor numbers.
struct CrtStartupFile
{
    enum : BYTE { Open = 0x01, Eof = 0x02, Pipe = 0x08, NoInherit = 0x10,
        Append = 0x20, Device = 0x40, Text = 0x80 };
    static constexpr int Limit = 8192;
    static constexpr size_t PacketLimit = (UINT16_MAX - sizeof(int32_t)) / (1 + sizeof(HANDLE));
    BYTE flags = 0;
    HANDLE handle = INVALID_HANDLE_VALUE;

    bool IsInheritable() const
    {
        return (flags & (Open | NoInherit)) == Open && handle &&
            handle != INVALID_HANDLE_VALUE && reinterpret_cast<intptr_t>(handle) != -2;
    }
};

inline std::vector<CrtStartupFile> DecodeCrtStartupFiles(const std::vector<BYTE>& bytes)
{
    if (bytes.size() < sizeof(int32_t) || bytes.size() > UINT16_MAX) return {};
    int32_t count;
    std::memcpy(&count, bytes.data(), sizeof(count));
    if (count <= 0 || count > CrtStartupFile::Limit ||
        bytes.size() != sizeof(count) + static_cast<size_t>(count) * (1 + sizeof(HANDLE))) return {};

    std::vector<CrtStartupFile> files(static_cast<size_t>(count));
    const BYTE* flags = bytes.data() + sizeof(count);
    const BYTE* handles = flags + count;
    for (size_t index = 0; index < files.size(); ++index)
    {
        files[index].flags = flags[index];
        std::memcpy(&files[index].handle, handles + index * sizeof(HANDLE), sizeof(HANDLE));
        if (!files[index].IsInheritable()) files[index] = {};
    }
    return files;
}

inline std::vector<BYTE> EncodeCrtStartupFiles(const std::vector<CrtStartupFile>& files)
{
    if (files.empty() || files.size() > CrtStartupFile::PacketLimit ||
        files.size() > CrtStartupFile::Limit) return {};
    const auto count = static_cast<int32_t>(files.size());
    std::vector<BYTE> bytes(sizeof(count) + files.size() * (1 + sizeof(HANDLE)));
    std::memcpy(bytes.data(), &count, sizeof(count));
    BYTE* flags = bytes.data() + sizeof(count);
    BYTE* handles = flags + files.size();
    for (size_t index = 0; index < files.size(); ++index)
    {
        flags[index] = files[index].flags;
        std::memcpy(handles + index * sizeof(HANDLE), &files[index].handle, sizeof(HANDLE));
    }
    return bytes;
}

} }
