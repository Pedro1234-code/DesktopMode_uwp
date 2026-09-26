// A deliberately small, normal x64 Win32 PE used to exercise the bridge.
// It has no CRT: GuestEntry is the PE entry point and every API below is
// resolved through the bridge's import catalog.

#include <windows.h>

namespace
{
    bool g_storageRoundTripPassed = false;

    int StringLength(const wchar_t* value)
    {
        int length = 0;
        while (value && value[length] != L'\0')
        {
            ++length;
        }
        return length;
    }

    bool RunStorageRoundTrip()
    {
        const wchar_t* const folder = L"C:\\Users\\Default\\Documents\\StorageDemo";
        const wchar_t* const fileName = L"C:\\Users\\Default\\Documents\\StorageDemo\\bridge-test.txt";
        const char payload[] = "Win32Bridge virtual storage is working.";
        const DWORD payloadSize = static_cast<DWORD>(sizeof(payload) - 1);

        // ERROR_ALREADY_EXISTS is an expected result after the first guest run.
        CreateDirectoryW(folder, nullptr);

        HANDLE file = CreateFileW(
            fileName,
            GENERIC_WRITE,
            FILE_SHARE_READ,
            nullptr,
            CREATE_ALWAYS,
            FILE_ATTRIBUTE_NORMAL,
            nullptr);
        if (file == INVALID_HANDLE_VALUE)
        {
            return false;
        }

        DWORD written = 0;
        const BOOL writeSucceeded = WriteFile(file, payload, payloadSize, &written, nullptr);
        CloseHandle(file);
        if (!writeSucceeded || written != payloadSize)
        {
            return false;
        }

        file = CreateFileW(
            fileName,
            GENERIC_READ,
            FILE_SHARE_READ,
            nullptr,
            OPEN_EXISTING,
            FILE_ATTRIBUTE_NORMAL,
            nullptr);
        if (file == INVALID_HANDLE_VALUE)
        {
            return false;
        }

        char readBack[sizeof(payload)] = {};
        DWORD read = 0;
        const BOOL readSucceeded = ReadFile(file, readBack, payloadSize, &read, nullptr);
        CloseHandle(file);
        if (!readSucceeded || read != payloadSize)
        {
            return false;
        }

        for (DWORD index = 0; index < payloadSize; ++index)
        {
            if (readBack[index] != payload[index])
            {
                return false;
            }
        }
        return true;
    }

    void PaintGuestWindow(HWND window)
    {
        PAINTSTRUCT paint = {};
        HDC dc = BeginPaint(window, &paint);
        if (!dc)
        {
            return;
        }

        RECT client = {};
        GetClientRect(window, &client);

        HBRUSH background = CreateSolidBrush(RGB(18, 27, 46));
        FillRect(dc, &client, background);
        DeleteObject(background);

        const RECT card = { 36, 80, client.right - 36, client.bottom - 36 };
        HBRUSH cardBrush = CreateSolidBrush(RGB(34, 52, 80));
        HPEN borderPen = CreatePen(PS_SOLID, 2, RGB(102, 204, 255));
        HGDIOBJ oldBrush = SelectObject(dc, cardBrush);
        HGDIOBJ oldPen = SelectObject(dc, borderPen);
        Rectangle(dc, card.left, card.top, card.right, card.bottom);

        // MiniGDI intentionally makes selected-object deletion safe. Clear the
        // two objects we created and also release the disposable defaults that
        // SelectObject returned on a fresh virtual HDC.
        DeleteObject(cardBrush);
        DeleteObject(borderPen);
        if (oldBrush)
        {
            DeleteObject(oldBrush);
        }
        if (oldPen)
        {
            DeleteObject(oldPen);
        }

        SetBkMode(dc, TRANSPARENT);
        SetTextColor(dc, RGB(230, 242, 255));
        const wchar_t* const heading = L"Win32Bridge MiniGDI guest";
        TextOutW(dc, 58, 30, heading, StringLength(heading));

        SetTextColor(dc, g_storageRoundTripPassed ? RGB(126, 242, 163) : RGB(255, 128, 128));
        const wchar_t* const storageStatus = g_storageRoundTripPassed
            ? L"Storage: C: drive write/read round-trip passed"
            : L"Storage: C: drive write/read round-trip failed";
        TextOutW(dc, 64, 116, storageStatus, StringLength(storageStatus));

        SetTextColor(dc, RGB(220, 230, 244));
        const wchar_t* const detail = L"This rectangle, border, and text are painted by a guest PE.";
        const wchar_t* const instruction = L"Click anywhere in the guest surface to close it.";
        TextOutW(dc, 64, 164, detail, StringLength(detail));
        TextOutW(dc, 64, 210, instruction, StringLength(instruction));

        HPEN accent = CreatePen(PS_SOLID, 3, RGB(255, 190, 92));
        HGDIOBJ oldAccent = SelectObject(dc, accent);
        MoveToEx(dc, 64, 258, nullptr);
        LineTo(dc, client.right - 64, 258);
        DeleteObject(accent);
        if (oldAccent)
        {
            DeleteObject(oldAccent);
        }

        EndPaint(window, &paint);
    }

    LRESULT CALLBACK GuestWindowProcedure(HWND window, UINT message, WPARAM wParam, LPARAM lParam)
    {
        switch (message)
        {
        case WM_PAINT:
            PaintGuestWindow(window);
            return 0;

        case WM_LBUTTONUP:
        case WM_RBUTTONUP:
        case WM_CLOSE:
            DestroyWindow(window);
            return 0;

        case WM_KEYDOWN:
            if (wParam == VK_ESCAPE)
            {
                DestroyWindow(window);
                return 0;
            }
            break;

        case WM_DESTROY:
            PostQuitMessage(0);
            return 0;
        }

        return DefWindowProcW(window, message, wParam, lParam);
    }
}

extern "C" int WINAPI GuestEntry()
{
    g_storageRoundTripPassed = RunStorageRoundTrip();

    WNDCLASSEXW windowClass = {};
    windowClass.cbSize = sizeof(windowClass);
    windowClass.lpfnWndProc = GuestWindowProcedure;
    windowClass.lpszClassName = L"Win32Bridge.MiniGdiDemo";
    if (RegisterClassExW(&windowClass) == 0)
    {
        return 10;
    }

    HWND window = CreateWindowExW(
        0,
        windowClass.lpszClassName,
        L"Win32Bridge - guest USER32 + MiniGDI",
        WS_OVERLAPPEDWINDOW,
        CW_USEDEFAULT,
        CW_USEDEFAULT,
        900,
        520,
        nullptr,
        nullptr,
        nullptr,
        nullptr);
    if (!window)
    {
        return 11;
    }

    ShowWindow(window, SW_SHOW);
    UpdateWindow(window);

    MSG message = {};
    while (GetMessageW(&message, nullptr, 0, 0) > 0)
    {
        TranslateMessage(&message);
        DispatchMessageW(&message);
    }
    return static_cast<int>(message.wParam);
}
