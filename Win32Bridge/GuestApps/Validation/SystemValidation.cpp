#include "Panel.h"
#include <ole2.h>
#include <oleauto.h>
#include <shlwapi.h>
#include <cstring>

namespace
{
    Validation::Log* registryLog = nullptr;

    int Registry(HWND)
    {
        wchar_t path[100]{};
        swprintf_s(path, L"Software\\Win32BridgeValidation-%lu-%llu", GetCurrentProcessId(), GetTickCount64());
        HKEY key = nullptr;
        LONG result = RegCreateKeyExW(HKEY_CURRENT_USER, path, 0, nullptr, 0, KEY_READ | KEY_WRITE, nullptr, &key, nullptr);
        registryLog->Write(L"EVENT", L"RegCreateKeyExW", result);
        if (result) return result;
        const DWORD expected = 0x13579bdf;
        result = RegSetValueExW(key, L"Value", 0, REG_DWORD, reinterpret_cast<const BYTE*>(&expected), sizeof(expected));
        registryLog->Write(L"EVENT", L"RegSetValueExW", result);
        RegCloseKey(key);
        if (!result) result = RegOpenKeyExW(HKEY_CURRENT_USER, path, 0, KEY_READ, &key);
        if (!result)
        {
            DWORD actual = 0, size = sizeof(actual), kind = 0;
            result = RegQueryValueExW(key, L"Value", nullptr, &kind, reinterpret_cast<BYTE*>(&actual), &size);
            if (!result && (actual != expected || size != sizeof(actual) || kind != REG_DWORD)) result = ERROR_INVALID_DATA;
            RegCloseKey(key);
        }
        const LONG cleaned = RegDeleteKeyW(HKEY_CURRENT_USER, path);
        registryLog->Write(L"EVENT", L"RegDeleteKeyW", cleaned);
        return result ? result : cleaned;
    }

    int Automation(HWND)
    {
        VARIANT original{}, copy{};
        VariantInit(&original);
        VariantInit(&copy);
        original.vt = VT_BSTR;
        constexpr wchar_t value[] = {L'A', 0, L'Z'};
        original.bstrVal = SysAllocStringLen(value, ARRAYSIZE(value));
        if (!original.bstrVal) return E_OUTOFMEMORY;
        const HRESULT copied = VariantCopy(&copy, &original);
        const bool equal = SUCCEEDED(copied) && copy.vt == VT_BSTR &&
            SysStringLen(copy.bstrVal) == ARRAYSIZE(value) && copy.bstrVal[2] == L'Z';
        VariantClear(&copy);
        VariantClear(&original);
        return equal ? 0 : E_FAIL;
    }

    int Stream(HWND)
    {
        const HRESULT init = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
        if (FAILED(init)) return init;
        IStream* stream = SHCreateMemStream(nullptr, 0);
        HRESULT result = E_NOINTERFACE;
        if (stream)
        {
            result = S_OK;
            const char expected[] = "Win32 stream";
            char actual[sizeof(expected)]{};
            ULONG count = 0;
            LARGE_INTEGER origin{};
            if (FAILED(stream->Write(expected, sizeof(expected), &count)) || count != sizeof(expected) ||
                FAILED(stream->Seek(origin, STREAM_SEEK_SET, nullptr)) ||
                FAILED(stream->Read(actual, sizeof(actual), &count)) || count != sizeof(actual) ||
                std::memcmp(expected, actual, sizeof(actual)) != 0) result = E_FAIL;
            stream->Release();
        }
        CoUninitialize();
        return FAILED(result) ? result : 0;
    }

    int VirtualMemory(HWND)
    {
        auto bytes = static_cast<BYTE*>(VirtualAlloc(nullptr, 8192, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
        if (!bytes) return GetLastError();
        bytes[4096] = 19;
        DWORD prior = 0;
        MEMORY_BASIC_INFORMATION info{};
        const bool correct = VirtualProtect(bytes + 4096, 4096, PAGE_READONLY, &prior) &&
            VirtualQuery(bytes + 4096, &info, sizeof(info)) == sizeof(info) &&
            prior == PAGE_READWRITE && info.Protect == PAGE_READONLY && bytes[4096] == 19;
        const bool released = VirtualFree(bytes, 0, MEM_RELEASE);
        return correct && released ? 0 : __LINE__;
    }

    int Environment(HWND)
    {
        const wchar_t name[] = L"WIN32BRIDGE_VALIDATION_SESSION";
        wchar_t before[512]{}, expanded[32]{};
        SetLastError(ERROR_SUCCESS);
        const DWORD count = GetEnvironmentVariableW(name, before, ARRAYSIZE(before));
        const bool exists = count || GetLastError() != ERROR_ENVVAR_NOT_FOUND;
        if (count >= ARRAYSIZE(before)) return Validation::Skip;
        if (!SetEnvironmentVariableW(name, L"ok")) return GetLastError();
        const DWORD size = ExpandEnvironmentStringsW(L"test:%WIN32BRIDGE_VALIDATION_SESSION%", expanded, ARRAYSIZE(expanded));
        const BOOL restored = SetEnvironmentVariableW(name, exists ? before : nullptr);
        return restored && size == 8 && std::wcscmp(expanded, L"test:ok") == 0 ? 0 : __LINE__;
    }
}

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int)
{
    Validation::Log log(L"SystemValidation");
    registryLog = &log;
    const Validation::Button choices[] = {{L"Registry create / reopen / remove", Registry}, {L"COM memory stream", Stream},
        {L"OLE Automation and BSTR", Automation}, {L"Virtual memory and protection", VirtualMemory},
        {L"Environment and expansion", Environment}};
    Validation::Panel(instance, log, choices, ARRAYSIZE(choices)).Show(L"System validation");
    return 0;
}
