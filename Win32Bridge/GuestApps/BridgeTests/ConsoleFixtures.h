#pragma once
#include "Bridge/GuestConsole.h"
#include <string>
namespace {
inline bool ValidateConsoleFixtures()
{
    using namespace Win32Bridge::Bridge;
    GuestConsoleContext parent, child, separate;
    DWORD error = 0, read = 0, mode = 0;
    const DWORD inputId = static_cast<DWORD>(-10), outputId = static_cast<DWORD>(-11);
    if (parent.GetStandard(outputId, &error) || error != ERROR_SUCCESS) return false;
    if (parent.GetStandard(123, &error) != INVALID_HANDLE_VALUE || error != ERROR_INVALID_PARAMETER) return false;
    if (!parent.Allocate(65001, &error) || parent.Allocate(65001, &error) || error != ERROR_ACCESS_DENIED) return false;
    const HANDLE input = parent.GetStandard(inputId, &error), output = parent.GetStandard(outputId, &error);
    if (!input || !output || input == output || !parent.Owns(input) || parent.CodePage(true) != 65001) return false;
    if (!parent.Mode(input, &mode, false, &error) || mode != 7) return false;
    if (!parent.Write(output, L"echo test\r\n", 11, &error) || parent.DrainOutput() != L"echo test\r\n") return false;
    if (parent.Write(input, L"x", 1, &error) || error != ERROR_INVALID_HANDLE) return false;
    wchar_t text[16]{};
    if (parent.Read(input, text, 16, &read, &error) || error != ERROR_NO_DATA || read) return false;
    if (!parent.FeedInput(L"ab") || parent.Read(input, text, 16, &read, &error) || error != ERROR_NO_DATA) return false;
    if (!parent.FeedInput(L"c\r\nnext\r\n") || !parent.Read(input, text, 16, &read, &error) || std::wstring(text, read) != L"abc\r\n") return false;
    if (!parent.Read(input, text, 2, &read, &error) || std::wstring(text, read) != L"ne") return false;
    if (!parent.Read(input, text, 16, &read, &error) || std::wstring(text, read) != L"xt\r\n") return false;
    child.Inherit(parent);
    if (child.GetStandard(outputId, &error) != output || !child.Write(output, L"child", 5, &error) || parent.DrainOutput() != L"child") return false;
    if (!child.Close(output) || parent.DrainOutput() != L"" || !parent.Owns(output) || child.Owns(output)) return false;
    if (!separate.Allocate(65001, &error) || separate.Owns(output)) return false;
    if (!parent.FeedInput(L"\u00e9\n")) return false;
    char bytes[8]{};
    if (!parent.ReadBytes(input, bytes, 1, &read, &error) || read != 1 || static_cast<unsigned char>(bytes[0]) != 0xc3) return false;
    if (!parent.ReadBytes(input, bytes, 8, &read, &error) || read != 2 || static_cast<unsigned char>(bytes[0]) != 0xa9 || bytes[1] != '\n') return false;
    if (!parent.FeedInput(L"", true) || !parent.Read(input, text, 16, &read, &error) || read != 0) return false;
    if (!child.Detach(&error) || !parent.Owns(input)) return false;
    parent.CloseHandles();
    if (parent.Owns(input) || parent.Read(input, text, 16, &read, &error) || error != ERROR_INVALID_HANDLE) return false;
    const HANDLE external = reinterpret_cast<HANDLE>(0x12345);
    if (!parent.SetStandard(outputId, external, &error) || parent.GetStandard(outputId, &error) != external || !parent.HasExternalStandardHandles()) return false;
    return true;
}
}
