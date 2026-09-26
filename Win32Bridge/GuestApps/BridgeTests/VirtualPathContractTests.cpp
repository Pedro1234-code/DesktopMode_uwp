#include "Bridge\\VirtualPath.h"

#include <iostream>

namespace
{
    std::wstring StoragePath(const Win32Bridge::Bridge::GuestPath& path)
    {
        std::wstring result = L"drive_c";
        for (const auto& component : path.components)
        {
            result += L'\\';
            result += component;
        }
        return result;
    }

    struct Case
    {
        const wchar_t* name;
        const wchar_t* requestedPath;
        const wchar_t* currentDirectory;
        bool shouldSucceed;
        const wchar_t* expectedGuestPath;
        const wchar_t* expectedStoragePath;
    };

    bool RunCase(const Case& test)
    {
        Win32Bridge::Bridge::VirtualPathResolver resolver;
        Win32Bridge::Bridge::GuestPath cwd;
        std::wstring error;
        if (!resolver.Resolve(test.currentDirectory, &cwd, &error) || !resolver.SetCurrentDirectory(cwd, &error))
        {
            std::wcerr << L"FAIL  test setup: " << test.name << L"\n";
            return false;
        }

        Win32Bridge::Bridge::GuestPath result;
        const bool succeeded = resolver.Resolve(test.requestedPath, &result, &error);
        const bool passed = succeeded == test.shouldSucceed &&
            (!succeeded || (result.canonical == test.expectedGuestPath && StoragePath(result) == test.expectedStoragePath));

        if (passed)
        {
            std::wcout << L"PASS  " << test.name << L"\n";
        }
        else
        {
            std::wcerr << L"FAIL  " << test.name << L"\n"
                << L"      request: " << test.requestedPath << L"\n"
                << L"      cwd:     " << test.currentDirectory << L"\n"
                << L"      result:  " << (succeeded ? result.canonical : error) << L"\n";
        }
        return passed;
    }
}

int wmain()
{
    const Case cases[] =
    {
        { L"absolute path", L"C:\\Users\\Default\\Documents\\save.dat", L"C:\\", true,
            L"C:\\Users\\Default\\Documents\\save.dat", L"drive_c\\Users\\Default\\Documents\\save.dat" },
        { L"lowercase drive and forward slashes", L"c:/Program Files/Test/app.exe", L"C:\\", true,
            L"C:\\Program Files\\Test\\app.exe", L"drive_c\\Program Files\\Test\\app.exe" },
        { L"relative path", L"Logs\\session.log", L"C:\\Users\\Default", true,
            L"C:\\Users\\Default\\Logs\\session.log", L"drive_c\\Users\\Default\\Logs\\session.log" },
        { L"relative dot and parent", L".\\Documents\\..\\Temp\\cache.bin", L"C:\\Users\\Default", true,
            L"C:\\Users\\Default\\Temp\\cache.bin", L"drive_c\\Users\\Default\\Temp\\cache.bin" },
        { L"legal parent traversal", L"..\\Shared\\game.ini", L"C:\\Users\\Default", true,
            L"C:\\Users\\Shared\\game.ini", L"drive_c\\Users\\Shared\\game.ini" },
        { L"repeated separators", L"C:\\Games\\\\Demo\\.\\data", L"C:\\", true,
            L"C:\\Games\\Demo\\data", L"drive_c\\Games\\Demo\\data" },
        { L"virtual drive root", L"C:\\", L"C:\\Users\\Default", true, L"C:\\", L"drive_c" },
        { L"drive-relative form", L"C:save.dat", L"C:\\Users\\Default", true,
            L"C:\\Users\\Default\\save.dat", L"drive_c\\Users\\Default\\save.dat" },
        { L"drive-rooted form", L"\\Windows\\file", L"C:\\Users\\Default", true,
            L"C:\\Windows\\file", L"drive_c\\Windows\\file" },
        { L"absolute escape is rejected", L"C:\\..\\outside", L"C:\\Users\\Default", false, L"", L"" },
        { L"relative escape is rejected", L"..\\escape", L"C:\\", false, L"", L"" },
        { L"deep relative escape is rejected", L"..\\..\\..\\outside", L"C:\\Users", false, L"", L"" },
        { L"other drive is rejected", L"D:\\save.dat", L"C:\\Users\\Default", false, L"", L"" },
        { L"UNC path is rejected", L"\\\\server\\share\\save.dat", L"C:\\Users\\Default", false, L"", L"" },
        { L"device path is rejected", L"\\\\?\\C:\\Windows\\file", L"C:\\Users\\Default", false, L"", L"" },
        { L"alternate stream is rejected", L"C:\\save.dat:stream", L"C:\\Users\\Default", false, L"", L"" },
        { L"wildcard is rejected outside enumeration", L"C:\\*.txt", L"C:\\Users\\Default", false, L"", L"" },
    };

    int failures = 0;
    for (const auto& test : cases)
    {
        if (!RunCase(test))
        {
            ++failures;
        }
    }

    if (failures != 0)
    {
        std::wcerr << failures << L" virtual path test(s) failed.\n";
        return 1;
    }

    std::wcout << L"All virtual path tests passed.\n";
    return 0;
}
