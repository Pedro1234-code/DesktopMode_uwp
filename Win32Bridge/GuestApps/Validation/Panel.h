#pragma once
#include "Log.h"

namespace Validation
{
    struct Button
    {
        const wchar_t* label;
        int (*run)(HWND);
    };

    class Panel
    {
    public:
        Panel(HINSTANCE instance, Log& log, const Button* buttons, unsigned count, bool select = false)
            : instance(instance), log(log), buttons(buttons), count(count), select(select) {}

        int Show(const wchar_t* title)
        {
            WNDCLASSW cls{};
            cls.hInstance = instance;
            cls.lpfnWndProc = Procedure;
            cls.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_BTNFACE + 1);
            cls.lpszClassName = L"BridgeValidationPanel";
            if (!RegisterClassW(&cls) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) return -1;
            const HWND window = CreateWindowExW(WS_EX_CONTROLPARENT, cls.lpszClassName, title,
                WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT, 670, 440, nullptr, nullptr, instance, this);
            if (!window) { log.Write(L"FAIL", L"Create launcher", GetLastError()); return -1; }
            ShowWindow(window, SW_SHOWNORMAL);
            MSG msg{};
            while (!finished && GetMessageW(&msg, nullptr, 0, 0) > 0)
                if (!IsDialogMessageW(window, &msg)) { TranslateMessage(&msg); DispatchMessageW(&msg); }
            if (IsWindow(window)) DestroyWindow(window);
            return selection;
        }

    private:
        static LRESULT CALLBACK Procedure(HWND window, UINT message, WPARAM wp, LPARAM lp)
        {
            if (message == WM_NCCREATE)
            {
                auto self = static_cast<Panel*>(reinterpret_cast<CREATESTRUCTW*>(lp)->lpCreateParams);
                SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
            }
            auto self = reinterpret_cast<Panel*>(GetWindowLongPtrW(window, GWLP_USERDATA));
            if (!self) return DefWindowProcW(window, message, wp, lp);
            if (message == WM_CREATE)
            {
                for (unsigned i = 0; i < self->count; ++i)
                    CreateWindowExW(0, L"BUTTON", self->buttons[i].label, WS_CHILD | WS_VISIBLE | WS_TABSTOP,
                        14 + (i % 2) * 310, 12 + (i / 2) * 36, 298, 30, window,
                        reinterpret_cast<HMENU>(static_cast<UINT_PTR>(100 + i)), self->instance, nullptr);
                self->output = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"",
                    WS_CHILD | WS_VISIBLE | WS_VSCROLL | ES_MULTILINE | ES_AUTOVSCROLL | ES_READONLY,
                    14, 24 + ((self->count + 1) / 2) * 36, 608, 95, window, nullptr, self->instance, nullptr);
                self->Print(self->log.saved ? self->log.path : L"Log open failed. See the runtime log.");
                return 0;
            }
            if (message == WM_COMMAND && LOWORD(wp) >= 100 && LOWORD(wp) - 100u < self->count && HIWORD(wp) == BN_CLICKED)
            {
                const unsigned index = LOWORD(wp) - 100;
                if (self->select)
                {
                    self->selection = static_cast<int>(index);
                    DestroyWindow(window);
                }
                else if (!self->running)
                {
                    self->running = true;
                    const auto& test = self->buttons[index];
                    self->log.Write(L"BEGIN", test.label);
                    self->Print(test.label);
                    const int result = test.run(window);
                    self->log.Write(Outcome(result), test.label, static_cast<unsigned>(result));
                    wchar_t report[160]{};
                    swprintf_s(report, L"%ls [0x%08X]%ls", Outcome(result), static_cast<unsigned>(result),
                        self->log.saved ? L"" : L"  (log write failed)");
                    self->Print(report);
                    self->running = false;
                }
                return 0;
            }
            if (message == WM_DESTROY) { self->finished = true; return 0; }
            return DefWindowProcW(window, message, wp, lp);
        }

        void Print(const wchar_t* line)
        {
            SendMessageW(output, EM_SETSEL, static_cast<WPARAM>(-1), -1);
            SendMessageW(output, EM_REPLACESEL, FALSE, reinterpret_cast<LPARAM>(line));
            SendMessageW(output, EM_REPLACESEL, FALSE, reinterpret_cast<LPARAM>(L"\r\n"));
            SendMessageW(output, EM_SCROLLCARET, 0, 0);
        }

        HINSTANCE instance;
        Log& log;
        const Button* buttons;
        unsigned count;
        bool select;
        bool finished = false;
        bool running = false;
        int selection = -1;
        HWND output = nullptr;
    };
}

