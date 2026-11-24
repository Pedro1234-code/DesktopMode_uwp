#include "Panel.h"
#include <commctrl.h>
#include <commdlg.h>
#include <initializer_list>

#pragma comment(linker, "/manifestdependency:\"type='win32' name='Microsoft.Windows.Common-Controls' version='6.0.0.0' processorArchitecture='*' publicKeyToken='6595b64144ccf1df' language='*'\"")

int RunGalleryPropertySheet(HINSTANCE, HWND);
int RunGalleryPaintAssertions(HINSTANCE);

namespace
{
    enum class Page { Lists, Drawing, Inputs, Ranges, Commands, VirtualList };
    Page page = Page::Lists;
    HWND progress = nullptr;
    HINSTANCE instance = nullptr;
    Validation::Log* events = nullptr;
    unsigned paintCount = 0;
    bool alternateColor = false;
    bool virtualCallbackSeen = false;

    HWND Add(HWND parent, LPCWSTR cls, LPCWSTR label, DWORD style, int x, int y, int w, int h, UINT id = 0)
    {
        HWND child = CreateWindowExW(0, cls, label, WS_CHILD | WS_VISIBLE | style, x, y, w, h, parent,
            reinterpret_cast<HMENU>(static_cast<UINT_PTR>(id)), instance, nullptr);
        if (!child)
        {
            events->Write(L"FAIL", cls, GetLastError());
            wchar_t message[110]{};
            swprintf_s(message, L"Unavailable: %ls", cls);
            CreateWindowExW(0, L"STATIC", message, WS_CHILD | WS_VISIBLE, x, y, w, 22, parent, nullptr, instance, nullptr);
        }
        return child;
    }

#include "GalleryControls.inl"

    void Populate(HWND window)
    {
        Add(window, L"STATIC", L"Edit text, switch tabs, expand the tree and select list rows.", 0, 12, 10, 548, 22);
        Add(window, L"EDIT", L"Portugu\u00eas / \u20ac / \u65e5\u672c\u8a9e", WS_BORDER | WS_TABSTOP | ES_AUTOHSCROLL, 12, 35, 252, 26, 201);
        Add(window, L"BUTTON", L"Checkbox", BS_AUTOCHECKBOX | WS_TABSTOP, 278, 35, 100, 26, 202);
        Add(window, L"BUTTON", L"Progress +", BS_PUSHBUTTON | WS_TABSTOP, 389, 35, 150, 26, 203);
        HWND combo = Add(window, L"COMBOBOX", L"", CBS_DROPDOWNLIST | WS_TABSTOP | WS_VSCROLL, 12, 71, 252, 140, 204);
        for (const auto* item : {L"Alpha", L"Beta", L"Gamma"})
            SendMessageW(combo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(item));
        SendMessageW(combo, CB_SETCURSEL, 0, 0);
        progress = Add(window, PROGRESS_CLASSW, L"", PBS_SMOOTH, 278, 71, 260, 22);
        SendMessageW(progress, PBM_SETRANGE32, 0, 100);
        SendMessageW(progress, PBM_SETPOS, 20, 0);
        HWND tabs = Add(window, WC_TABCONTROLW, L"", WS_TABSTOP, 12, 107, 527, 32, 205);
        TCITEMW tab{}; tab.mask = TCIF_TEXT;
        wchar_t one[] = L"First", two[] = L"Second";
        tab.pszText = one; SendMessageW(tabs, TCM_INSERTITEMW, 0, reinterpret_cast<LPARAM>(&tab));
        tab.pszText = two; SendMessageW(tabs, TCM_INSERTITEMW, 1, reinterpret_cast<LPARAM>(&tab));
        HWND tree = Add(window, WC_TREEVIEWW, L"", WS_BORDER | WS_TABSTOP | TVS_HASBUTTONS | TVS_HASLINES | TVS_LINESATROOT,
            12, 150, 230, 170, 206);
        TVINSERTSTRUCTW branch{};
        branch.hParent = TVI_ROOT; branch.hInsertAfter = TVI_LAST; branch.item.mask = TVIF_TEXT;
        wchar_t root[] = L"Root", child[] = L"Child";
        branch.item.pszText = root;
        branch.hParent = reinterpret_cast<HTREEITEM>(SendMessageW(tree, TVM_INSERTITEMW, 0, reinterpret_cast<LPARAM>(&branch)));
        branch.item.pszText = child;
        SendMessageW(tree, TVM_INSERTITEMW, 0, reinterpret_cast<LPARAM>(&branch));
        HWND list = Add(window, WC_LISTVIEWW, L"", WS_BORDER | WS_TABSTOP | LVS_REPORT | LVS_EDITLABELS | LVS_SHOWSELALWAYS,
            254, 150, 285, 170, 207);
        SendMessageW(list, LVM_SETEXTENDEDLISTVIEWSTYLE, 0, LVS_EX_FULLROWSELECT | LVS_EX_CHECKBOXES | LVS_EX_GRIDLINES);
        LVCOLUMNW column{}; column.mask = LVCF_WIDTH | LVCF_TEXT; column.cx = 138;
        wchar_t name[] = L"Name", value[] = L"Value";
        column.pszText = name; SendMessageW(list, LVM_INSERTCOLUMNW, 0, reinterpret_cast<LPARAM>(&column));
        column.pszText = value; SendMessageW(list, LVM_INSERTCOLUMNW, 1, reinterpret_cast<LPARAM>(&column));
        for (int index = 0; index != 8; ++index)
        {
            wchar_t label[32]{};
            swprintf_s(label, L"Row %d", index + 1);
            LVITEMW item{}; item.mask = LVIF_TEXT; item.iItem = index; item.pszText = label;
            SendMessageW(list, LVM_INSERTITEMW, 0, reinterpret_cast<LPARAM>(&item));
            item.iSubItem = 1; item.pszText = value;
            SendMessageW(list, LVM_SETITEMTEXTW, index, reinterpret_cast<LPARAM>(&item));
        }
        Add(window, L"BUTTON", L"Close", WS_TABSTOP, 430, 330, 108, 25, IDCANCEL);
    }

    void Paint(HWND window)
    {
        PAINTSTRUCT ps{};
        HDC dc = BeginPaint(window, &ps);
        if (!dc) { events->Write(L"FAIL", L"BeginPaint (GDI scene)", GetLastError()); return; }
        RECT area{}; GetClientRect(window, &area);
        ++paintCount;
        FillRect(dc, &area, reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1));
        HBRUSH brush = CreateSolidBrush(alternateColor ? RGB(225, 155, 65) : RGB(75, 140, 210));
        HPEN pen = CreatePen(PS_SOLID, 3, RGB(190, 40, 72));
        HGDIOBJ previousBrush = SelectObject(dc, brush);
        HGDIOBJ previousPen = SelectObject(dc, pen);
        Rectangle(dc, 26, 80, 155, 174);
        Ellipse(dc, 180, 80, 314, 174);
        POINT triangle[] = {{362, 170}, {402, 80}, {492, 166}};
        Polygon(dc, triangle, ARRAYSIZE(triangle));
        SelectObject(dc, previousBrush); SelectObject(dc, previousPen);
        DeleteObject(brush); DeleteObject(pen);
        SetBkMode(dc, TRANSPARENT);
        RECT text{20, 44, area.right - 12, 78};
        wchar_t description[160]{};
        swprintf_s(description, L"Client: %ld x %ld  |  Paint #%u\r\nPortugu\u00eas \u20ac  \u65e5\u672c\u8a9e",
            area.right, area.bottom, paintCount);
        DrawTextW(dc, description, -1, &text, DT_LEFT);
        DWORD pixels[] = {0x00FF0000, 0x0000FF00, 0x000000FF, 0x00FFFFFF};
        BITMAPINFO bitmap{};
        bitmap.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
        bitmap.bmiHeader.biWidth = 2; bitmap.bmiHeader.biHeight = -2;
        bitmap.bmiHeader.biPlanes = 1; bitmap.bmiHeader.biBitCount = 32;
        StretchDIBits(dc, 28, 203, 180, 100, 0, 0, 2, 2, pixels, &bitmap, DIB_RGB_COLORS, SRCCOPY);
        EndPaint(window, &ps);
        if (paintCount == 1) events->Write(L"EVENT", L"First GDI paint");
    }

    void Repaint(HWND window)
    {
        alternateColor = !alternateColor;
        RECT region{};
        GetClientRect(window, &region);
        region.top = 43;
        InvalidateRect(window, &region, FALSE);
        UpdateWindow(window);
        events->Write(L"EVENT", L"Manual GDI repaint", paintCount);
    }

    LRESULT CALLBACK Scene(HWND window, UINT msg, WPARAM wp, LPARAM lp)
    {
        const bool drawing = page == Page::Drawing;
        switch (msg)
        {
        case WM_CREATE:
            switch (page)
            {
            case Page::Lists: Populate(window); break;
            case Page::Inputs: PopulateInputs(window); break;
            case Page::Ranges: PopulateRanges(window); break;
            case Page::Commands: PopulateCommands(window); break;
            case Page::VirtualList: PopulateVirtualList(window); break;
            case Page::Drawing:
                Add(window, L"BUTTON", L"Repaint / change color", WS_TABSTOP, 16, 8, 180, 27, 220);
                Add(window, L"BUTTON", L"Close", WS_TABSTOP, 204, 8, 90, 27, IDCANCEL);
                return 0;
            }
            if (page != Page::Lists) Add(window, L"BUTTON", L"Close", WS_TABSTOP, 430, 337, 108, 26, IDCANCEL);
            return 0;
        case WM_PAINT: if (drawing) { Paint(window); return 0; } break;
        case WM_SIZE:
            if (drawing && wp != SIZE_MINIMIZED)
            {
                InvalidateRect(window, nullptr, FALSE);
                events->Write(L"EVENT", L"GDI resize (height:width)", static_cast<unsigned>(lp));
            }
            break;
        case WM_LBUTTONDOWN: if (drawing) { Repaint(window); return 0; } break;
        case WM_HSCROLL:
            if (lp) UpdatePreview(window, reinterpret_cast<HWND>(lp), wp);
            return 0;
        case WM_COMMAND:
            if (LOWORD(wp) == IDCANCEL) { DestroyWindow(window); return 0; }
            if (LOWORD(wp) == 220) { Repaint(window); return 0; }
            if (LOWORD(wp) == ToolbarFirst || LOWORD(wp) == ToolbarSecond)
                SendDlgItemMessageW(window, Status, SB_SETTEXTW, 0,
                    reinterpret_cast<LPARAM>(LOWORD(wp) == ToolbarFirst ? L"Run clicked" : L"Toggle clicked"));
            if (LOWORD(wp) == 279) SendDlgItemMessageW(window, 278, LVM_ENSUREVISIBLE, 9999, FALSE);
            if (LOWORD(wp) == 203)
            {
                auto value = SendMessageW(progress, PBM_GETPOS, 0, 0);
                SendMessageW(progress, PBM_SETPOS, (value + 10) % 110, 0);
            }
            if (HIWORD(wp) != EN_UPDATE && HIWORD(wp) != EN_CHANGE)
                events->Write(L"EVENT", L"WM_COMMAND", static_cast<unsigned>(wp));
            return 0;
        case WM_NOTIFY:
            if (lp)
            {
                const auto notice = reinterpret_cast<NMHDR*>(lp);
                if (notice->code == LVN_GETDISPINFOW && notice->idFrom == 278)
                {
                    auto& item = reinterpret_cast<NMLVDISPINFOW*>(lp)->item;
                    if ((item.mask & LVIF_TEXT) && item.pszText && item.cchTextMax > 0)
                    {
                        swprintf_s(item.pszText, item.cchTextMax, item.iSubItem ? L"Value %d" : L"Row %d", item.iItem + 1);
                        if (!virtualCallbackSeen)
                        {
                            virtualCallbackSeen = true;
                            events->Write(L"EVENT", L"Virtual ListView requested cell text", static_cast<unsigned>(item.iItem));
                        }
                    }
                    return 0;
                }
                if (notice->idFrom == 276 && (notice->code == NM_CLICK || notice->code == NM_RETURN))
                    SendDlgItemMessageW(window, Status, SB_SETTEXTW, 0, reinterpret_cast<LPARAM>(L"Link activated"));
                if (notice->code != NM_CUSTOMDRAW)
                    events->Write(L"EVENT", L"WM_NOTIFY", notice->code);
                if (notice->code == LVN_ENDLABELEDITW)
                    return reinterpret_cast<NMLVDISPINFOW*>(lp)->item.pszText != nullptr;
            }
            return 0;
        case WM_CONTEXTMENU:
            {
                HMENU popup = CreatePopupMenu();
                AppendMenuW(popup, MF_STRING, 1, L"First item");
                AppendMenuW(popup, MF_STRING | MF_CHECKED, 2, L"Checked item");
                AppendMenuW(popup, MF_STRING | MF_GRAYED, 3, L"Disabled item");
                POINT point{}; GetCursorPos(&point);
                const UINT chosen = TrackPopupMenu(popup, TPM_RETURNCMD | TPM_RIGHTBUTTON, point.x, point.y, 0, window, nullptr);
                DestroyMenu(popup);
                events->Write(L"EVENT", L"Context menu command (0 = cancel)", chosen);
                return 0;
            }
        }
        return DefWindowProcW(window, msg, wp, lp);
    }

    int OpenScene(HWND owner, Page selected, LPCWSTR title)
    {
        page = selected;
        const bool gdi = page == Page::Drawing;
        paintCount = 0;
        alternateColor = false;
        virtualCallbackSeen = false;
        WNDCLASSW cls{};
        cls.hInstance = instance; cls.lpfnWndProc = Scene; cls.lpszClassName = L"ValidationGalleryScene";
        cls.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
        if (!RegisterClassW(&cls) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) return GetLastError();
        RECT anchor{};
        const bool position = GetWindowRect(owner, &anchor) != FALSE;
        HWND window = CreateWindowExW(WS_EX_CONTROLPARENT, cls.lpszClassName, title,
            WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN, position ? anchor.left + 24 : CW_USEDEFAULT,
            position ? anchor.top + 24 : CW_USEDEFAULT, 581, 422, owner, nullptr, instance, nullptr);
        if (!window) return GetLastError();
        const bool disableOwner = IsWindowEnabled(owner) != FALSE;
        if (disableOwner) EnableWindow(owner, FALSE);
        ShowWindow(window, SW_SHOWNORMAL);
        SetWindowPos(window, HWND_TOP, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_SHOWWINDOW);
        UpdateWindow(window);
        events->Write(L"EVENT", gdi ? L"GDI scene opened" : L"Control scene opened");
        MSG msg{};
        int result = Validation::Manual;
        while (IsWindow(window))
        {
            const int received = GetMessageW(&msg, nullptr, 0, 0);
            if (received <= 0)
            {
                if (received < 0) result = static_cast<int>(GetLastError());
                DestroyWindow(window);
                if (!received) PostQuitMessage(static_cast<int>(msg.wParam));
                break;
            }
            if (!IsDialogMessageW(window, &msg)) { TranslateMessage(&msg); DispatchMessageW(&msg); }
        }
        if (disableOwner && IsWindow(owner))
        {
            EnableWindow(owner, TRUE);
            SetActiveWindow(owner);
        }
        return gdi && !paintCount && result == Validation::Manual ? __LINE__ : result;
    }

    INT_PTR CALLBACK Dialog(HWND window, UINT msg, WPARAM wp, LPARAM)
    {
        if (msg == WM_INITDIALOG) return TRUE;
        if (msg == WM_CLOSE || (msg == WM_COMMAND && (LOWORD(wp) == IDOK || LOWORD(wp) == IDCANCEL)))
        {
            EndDialog(window, msg == WM_CLOSE ? IDCANCEL : LOWORD(wp));
            return TRUE;
        }
        return FALSE;
    }

    int Controls(HWND owner) { return OpenScene(owner, Page::Lists, L"Lists, tree and tabs"); }
    int Gdi(HWND owner) { return OpenScene(owner, Page::Drawing, L"GDI drawing and resize"); }
    int Inputs(HWND owner) { return OpenScene(owner, Page::Inputs, L"Inputs and buttons"); }
    int Ranges(HWND owner) { return OpenScene(owner, Page::Ranges, L"Sliders, dates and specialized input"); }
    int Commands(HWND owner) { return OpenScene(owner, Page::Commands, L"Toolbar, header and status bar"); }
    int VirtualList(HWND owner) { return OpenScene(owner, Page::VirtualList, L"Virtual ListView"); }
    int DialogResource(HWND owner)
    {
        const auto result = DialogBoxParamW(instance, MAKEINTRESOURCEW(101), owner, Dialog, 0);
        events->Write(L"EVENT", L"Resource dialog result", static_cast<unsigned>(result));
        return result == IDOK || result == IDCANCEL ? Validation::Manual : __LINE__;
    }
    int Sheets(HWND owner) { return RunGalleryPropertySheet(instance, owner); }
    int PaintAssertions(HWND) { return RunGalleryPaintAssertions(instance); }
    int Picker(HWND owner)
    {
        wchar_t path[MAX_PATH]{};
        OPENFILENAMEW picker{};
        picker.lStructSize = sizeof(picker); picker.hwndOwner = owner;
        picker.lpstrFilter = L"All files\0*.*\0\0"; picker.lpstrFile = path; picker.nMaxFile = ARRAYSIZE(path);
        picker.lpstrInitialDir = L"C:\\Windows\\test_apps"; picker.Flags = OFN_FILEMUSTEXIST | OFN_NOCHANGEDIR;
        if (GetOpenFileNameW(&picker)) return Validation::Manual;
        const DWORD error = CommDlgExtendedError();
        return error ? static_cast<int>(error) : Validation::Manual;
    }
}

int WINAPI wWinMain(HINSTANCE module, HINSTANCE, PWSTR, int)
{
    instance = module;
    Validation::Log log(L"Win32Gallery");
    events = &log;
    constexpr DWORD groups[] = {ICC_LISTVIEW_CLASSES | ICC_TREEVIEW_CLASSES | ICC_TAB_CLASSES,
        ICC_BAR_CLASSES | ICC_COOL_CLASSES | ICC_PROGRESS_CLASS | ICC_UPDOWN_CLASS,
        ICC_DATE_CLASSES | ICC_INTERNET_CLASSES | ICC_HOTKEY_CLASS | ICC_USEREX_CLASSES, ICC_LINK_CLASS};
    for (DWORD group : groups)
    {
        INITCOMMONCONTROLSEX controls{sizeof(controls), group};
        log.Write(InitCommonControlsEx(&controls) ? L"PASS" : L"FAIL", L"InitCommonControlsEx mask", group);
    }
    const Validation::Button choices[] = {{L"Lists, tree, tabs and context menu", Controls}, {L"GDI drawing and resize", Gdi},
        {L"Modal dialog from PE resources", DialogResource}, {L"Property sheet assertions", Sheets},
        {L"Open-file picker", Picker}, {L"Paint assertions", PaintAssertions},
        {L"Inputs and buttons", Inputs}, {L"Sliders, dates and specialized input", Ranges},
        {L"Toolbar, header and status bar", Commands}, {L"Virtual ListView (10,000 rows)", VirtualList}};
    Validation::Panel(instance, log, choices, ARRAYSIZE(choices)).Show(L"Win32 control and GDI gallery");
    return 0;
}
