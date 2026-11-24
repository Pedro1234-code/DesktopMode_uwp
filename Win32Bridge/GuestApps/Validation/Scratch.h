#pragma once
#include <windows.h>
#include <cwchar>

namespace Validation
{
    inline UINT CreateScratchFile(PCWSTR directory, PCWSTR prefix, UINT, PWSTR path)
    {
        static volatile LONG sequence = 0;
        const size_t length = std::wcslen(directory);
        const wchar_t* separator = length && (directory[length - 1] == L'\\' || directory[length - 1] == L'/') ? L"" : L"\\";
        for (unsigned i = 0; i < 64; ++i)
        {
            const auto id = static_cast<ULONG>(InterlockedIncrement(&sequence));
            if (swprintf_s(path, MAX_PATH, L"%ls%ls%.3ls-%lu-%llu-%lu.tmp", directory, separator, prefix,
                    GetCurrentProcessId(), GetTickCount64(), id) < 0) return 0;
            HANDLE file = CreateFileW(path, GENERIC_WRITE, FILE_SHARE_READ, nullptr, CREATE_NEW,
                FILE_ATTRIBUTE_NORMAL, nullptr);
            if (file != INVALID_HANDLE_VALUE) { CloseHandle(file); return id ? id : 1; }
            const DWORD error = GetLastError();
            if (error != ERROR_FILE_EXISTS && error != ERROR_ALREADY_EXISTS) break;
        }
        path[0] = 0;
        return 0;
    }
}
