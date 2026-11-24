#pragma once
#include <windows.h>
#include <cstdio>
#include <cwchar>

namespace Validation
{
    class Log
    {
    public:
        explicit Log(const wchar_t* app)
        {
            constexpr wchar_t root[] = L"C:\\Windows\\test_apps";
            wchar_t directory[MAX_PATH]{};
            if (GetFileAttributesW(root) != INVALID_FILE_ATTRIBUTES)
            {
                swprintf_s(directory, L"%ls\\logs", root);
                CreateDirectoryW(directory, nullptr);
                swprintf_s(path, L"%ls\\%ls.log", directory, app);
            }
            else if (GetTempPathW(MAX_PATH, directory) < MAX_PATH && *directory)
                swprintf_s(path, L"%ls%ls.log", directory, app);
            Write(L"SESSION", app);
        }

        void Write(const wchar_t* status, const wchar_t* name, unsigned code = 0)
        {
            SYSTEMTIME time{};
            GetLocalTime(&time);
            wchar_t line[512]{};
            swprintf_s(line, L"%04u-%02u-%02u %02u:%02u:%02u pid=%lu %ls %ls [0x%08X]\r\n",
                time.wYear, time.wMonth, time.wDay, time.wHour, time.wMinute, time.wSecond,
                GetCurrentProcessId(), status, name, code);
            char utf8[2048]{};
            const int length = WideCharToMultiByte(CP_UTF8, 0, line, -1, utf8, sizeof(utf8), nullptr, nullptr);
            HANDLE file = CreateFileW(path, FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE,
                nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
            saved = false;
            if (file != INVALID_HANDLE_VALUE)
            {
                DWORD written = 0;
                saved = length > 0 && WriteFile(file, utf8, length - 1, &written, nullptr) &&
                    written == static_cast<DWORD>(length - 1);
                CloseHandle(file);
            }
        }

        wchar_t path[MAX_PATH]{};
        bool saved = false;
    };

    constexpr int Manual = -1;
    constexpr int Skip = -2;
    inline const wchar_t* Outcome(int result)
    { return result == Manual ? L"MANUAL" : result == Skip ? L"SKIP" : result ? L"FAIL" : L"PASS"; }

}


