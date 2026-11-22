#pragma once
#include "Bridge/CRT/CrtScanning.h"
#include "Bridge/CRT/CrtStdio.h"

namespace Win32Bridge { namespace Bridge {
// Own the recursive FILE lock for the entire conversion, not one character at
// a time. Recursive acquisition by Get/Unget also supports _lock_file callers.
template<bool Wide> class CrtStdioScanInput final : public CrtScanInput
{
    CrtStdio& io;
    void* stream;
    bool locked = true;
public:
    CrtStdioScanInput(CrtStdio& value, void* handle) : io(value), stream(handle) { io.Lock(stream); }
    ~CrtStdioScanInput() override { ReleaseLock(); }
    // The ABI leaf also calls this from __finally when a guest pointer faults;
    // /EHsc alone does not unwind C++ destructors for access violations.
    void ReleaseLock() { if (locked) { locked = false; io.Unlock(stream); } }
    CrtStdioScanInput(const CrtStdioScanInput&) = delete;
    CrtStdioScanInput& operator=(const CrtStdioScanInput&) = delete;
    int Get() override
    {
        if (!Wide) return io.GetByte(stream);
        const auto c = io.GetWide(stream); return c == 0xffff ? -1 : c;
    }
    void Unget(int c) override
    {
        if (c < 0) return;
        if (Wide) io.UngetWide(static_cast<unsigned short>(c), stream);
        else io.UngetByte(c, stream);
    }
};
} }
