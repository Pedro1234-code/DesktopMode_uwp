#pragma once
#include "Bridge/GuestPipes.h"
#include <cstring>
namespace {
inline bool ValidatePipeFixtures()
{
    using namespace Win32Bridge::Bridge;
    GuestPipeContext parent, child, adopted;
    HANDLE read = nullptr, write = nullptr, duplicate = nullptr;
    DWORD error = 0, done = 0, available = 0, flags = 0;
    BYTE text[32]{};
    const BYTE source[] = {'a','b','c','d','e','f'};
    if (!parent.Create(true, 4, &read, &write, &error) || !read || !write || read == write) return false;
    if (!parent.Flags(read, &flags, 0, false, &error) || flags != 1) return false;
    if (parent.Read(write, text, 1, &done, &error) || error != ERROR_ACCESS_DENIED) return false;
    if (parent.Write(read, source, 1, &done, &error) || error != ERROR_ACCESS_DENIED) return false;
    if (parent.Read(read, text, 1, &done, &error) || error != ERROR_IO_PENDING) return false;
    if (!parent.Write(write, source, 6, &done, &error) || done != 4) return false;
    if (parent.Write(write, source, 1, &done, &error) || error != ERROR_IO_PENDING) return false;
    if (!parent.Peek(read, text, 2, &done, &available, &error) || done != 2 || available != 4 || std::memcmp(text,source,2)) return false;
    if (!parent.Read(read, text, 2, &done, &error) || done != 2 || std::memcmp(text,source,2)) return false;
    if (!parent.Write(write, source+4, 2, &done, &error) || done != 2) return false;
    flags = 0;
    if (!parent.Flags(read, &flags, 1, true, &error)) return false;
    child.Inherit(parent);
    if (child.Owns(read) || !child.Owns(write)) return false;
    if (!parent.Close(write, &error)) return false;
    if (!parent.Read(read, text, 32, &done, &error) || done != 4 || std::memcmp(text,source+2,4)) return false;
    if (parent.Read(read, text, 1, &done, &error) || error != ERROR_IO_PENDING) return false;
    adopted.Adopt(child);
    if (child.Owns(write) || !adopted.Owns(write)) return false;
    if (!adopted.Duplicate(write, false, &duplicate, &error)) return false;
    if (!adopted.Flags(duplicate, &flags, 0, false, &error) || flags != 0) return false;
    flags = 2;
    if (!adopted.Flags(write, &flags, 2, true, &error) || adopted.Close(write, &error) || error != ERROR_INVALID_HANDLE) return false;
    adopted.CloseAll(); // Teardown ignores close protection and removes every writer.
    if (parent.Read(read, text, 1, &done, &error) || error != ERROR_BROKEN_PIPE) return false;
    if (parent.Read(read, text, 0, &done, &error) || error != ERROR_BROKEN_PIPE) return false;
    if (parent.Peek(read, text, 32, &done, &available, &error) || error != ERROR_BROKEN_PIPE) return false;
    if (!parent.Close(read, &error)) return false;
    if (!parent.Create(false, 8, &read, &write, &error) || !parent.Close(read, &error)) return false;
    if (parent.Write(write, source, 1, &done, &error) || error != ERROR_NO_DATA) return false;
    if (parent.Write(write, nullptr, 0, &done, &error) || error != ERROR_NO_DATA) return false;
    return true;
}
}
