#include "Log.h"
#include <cwchar>

namespace
{
    void Print(const wchar_t* text)
    {
        DWORD written = 0;
        WriteConsoleW(GetStdHandle(STD_OUTPUT_HANDLE), text, static_cast<DWORD>(std::wcslen(text)), &written, nullptr);
    }

    int Buffer()
    {
        HANDLE output = GetStdHandle(STD_OUTPUT_HANDLE);
        CONSOLE_SCREEN_BUFFER_INFO info{};
        if (!GetConsoleScreenBufferInfo(output, &info)) return GetLastError();
        using Read = decltype(&ReadConsoleOutputCharacterW);
        using Write = decltype(&WriteConsoleOutputCharacterW);
        const HMODULE kernel = GetModuleHandleW(L"kernel32.dll");
        const auto readOutput = reinterpret_cast<Read>(GetProcAddress(kernel, "ReadConsoleOutputCharacterW"));
        const auto writeOutput = reinterpret_cast<Write>(GetProcAddress(kernel, "WriteConsoleOutputCharacterW"));
        if (!readOutput || !writeOutput) return Validation::Skip;
        COORD cell = info.dwCursorPosition;
        DWORD written = 0, read = 0;
        wchar_t previous = 0, actual = 0;
        if (!readOutput(output, &previous, 1, cell, &read) || read != 1) return __LINE__;
        const bool right = writeOutput(output, L"Q", 1, cell, &written) && written == 1 &&
            readOutput(output, &actual, 1, cell, &read) && read == 1 && actual == L'Q';
        const BOOL restored = writeOutput(output, &previous, 1, cell, &written);
        return right && restored ? 0 : __LINE__;
    }

    int Cursor()
    {
        HANDLE output = GetStdHandle(STD_OUTPUT_HANDLE);
        CONSOLE_SCREEN_BUFFER_INFO original{}, actual{};
        if (!GetConsoleScreenBufferInfo(output, &original)) return GetLastError();
        const COORD test{static_cast<SHORT>(original.dwCursorPosition.X ? 0 : 1), original.dwCursorPosition.Y};
        const bool moved = SetConsoleCursorPosition(output, test) && GetConsoleScreenBufferInfo(output, &actual) &&
            actual.dwCursorPosition.X == test.X && actual.dwCursorPosition.Y == test.Y;
        const BOOL restored = SetConsoleCursorPosition(output, original.dwCursorPosition);
        return moved && restored ? 0 : __LINE__;
    }

    int Colors()
    {
        HANDLE output = GetStdHandle(STD_OUTPUT_HANDLE);
        CONSOLE_SCREEN_BUFFER_INFO info{};
        if (!GetConsoleScreenBufferInfo(output, &info) || !SetConsoleTextAttribute(output, FOREGROUND_GREEN | FOREGROUND_INTENSITY))
            return __LINE__;
        Print(L"Green text, followed by normal colors.\r\n");
        return SetConsoleTextAttribute(output, info.wAttributes) ? Validation::Manual : __LINE__;
    }

    int Vt()
    {
        HANDLE output = GetStdHandle(STD_OUTPUT_HANDLE);
        DWORD mode = 0;
        if (!GetConsoleMode(output, &mode) || !SetConsoleMode(output, mode | ENABLE_VIRTUAL_TERMINAL_PROCESSING))
            return Validation::Skip;
        Print(L"\x1b[35mMagenta VT text\x1b[0m\r\n");
        return SetConsoleMode(output, mode) ? Validation::Manual : __LINE__;
    }

    int Unicode()
    {
        Print(L"Expected: Portugu\u00eas | \u20ac | \u65e5\u672c\u8a9e | \u0416\r\n");
        return Validation::Manual;
    }

    int Mode()
    {
        HANDLE input = GetStdHandle(STD_INPUT_HANDLE);
        DWORD original = 0, actual = 0;
        if (!GetConsoleMode(input, &original)) return GetLastError();
        const bool changed = SetConsoleMode(input, original ^ ENABLE_PROCESSED_INPUT) &&
            GetConsoleMode(input, &actual) && actual == (original ^ ENABLE_PROCESSED_INPUT);
        const BOOL restored = SetConsoleMode(input, original);
        return changed && restored ? 0 : __LINE__;
    }
}

int wmain()
{
    Validation::Log log(L"ConsoleValidation");
    Print(log.saved ? log.path : L"Log open failed. See the runtime log.");
    struct Choice { const wchar_t* title; int (*run)(); };
    const Choice choices[] = {{L"Screen buffer", Buffer}, {L"Console colors", Colors}, {L"VT output", Vt},
        {L"Unicode glyphs", Unicode}, {L"Input mode", Mode}, {L"Cursor positioning", Cursor}};
    for (;;)
    {
        Print(L"\r\n1 Buffer  2 Colors  3 VT  4 Unicode  5 Input mode  6 Cursor  0 Quit\r\n> ");
        wchar_t answer[32]{};
        DWORD read = 0;
        if (!ReadConsoleW(GetStdHandle(STD_INPUT_HANDLE), answer, ARRAYSIZE(answer) - 1, &read, nullptr) || !read)
        { log.Write(L"FAIL", L"ReadConsoleW menu", GetLastError()); return 1; }
        const unsigned index = answer[0] - L'1';
        if (answer[0] == L'0') return 0;
        if (index >= ARRAYSIZE(choices)) continue;
        log.Write(L"BEGIN", choices[index].title);
        const int result = choices[index].run();
        log.Write(Validation::Outcome(result), choices[index].title, static_cast<unsigned>(result));
        Print(Validation::Outcome(result));
        Print(log.saved ? L"\r\n" : L" (log write failed)\r\n");
    }
}
