// Bridge USER32/comctl32 integration fixture. In addition to ordinary Win32
// contracts, it asserts the bridge's resize and notification-reentry guards.
// These extra guards are not a claim about undocumented desktop behavior.
#include <windows.h>
#include <commctrl.h>
#include <prsht.h>

namespace
{
    struct alignas(DWORD) EmptyTemplate
    {
        DLGTEMPLATE dialog;
        WORD menu, windowClass, title;
    };
    struct PageState
    {
        HWND window, first, preferred;
        bool vetoLeave, vetoApply, skip, tryRecursiveApply;
        bool recursiveRejected;
        unsigned applies, releases;
    };
    int failure = 0;
    void Check(bool result, int line) { if (!result && !failure) failure = line; }
#define CHECK(expression) Check(!!(expression), __LINE__)

    const RECT PaintArea{ 6, 8, 26, 30 };
    unsigned paints = 0, erases = 0;
    bool measuringPaint = false;
    HRGN appRegion = nullptr, copiedRegion = nullptr;

    LRESULT CALLBACK PaintProcedure(HWND window, UINT message, WPARAM wParam, LPARAM lParam)
    {
        if (message == WM_ERASEBKGND && measuringPaint)
        {
            ++erases;
            return TRUE;
        }
        if (message == WM_PAINT)
        {
            PAINTSTRUCT paint{};
            const HDC dc = BeginPaint(window, &paint);
            if (dc && measuringPaint)
            {
                ++paints;
                CHECK(!paint.fErase && EqualRect(&paint.rcPaint, &PaintArea));
                CHECK(GetClipRgn(dc, copiedRegion) == 0);
                CHECK(SelectClipRgn(dc, appRegion) == SIMPLEREGION);
                RECT bounds{};
                CHECK(GetClipBox(dc, &bounds) == SIMPLEREGION && EqualRect(&bounds, &PaintArea));
                CHECK(SelectClipRgn(dc, nullptr) == SIMPLEREGION);
                CHECK(GetClipBox(dc, &bounds) == SIMPLEREGION && EqualRect(&bounds, &PaintArea));
            }
            if (dc) EndPaint(window, &paint);
            return 0;
        }
        return DefWindowProcW(window, message, wParam, lParam);
    }

    void CheckPaintContracts(HINSTANCE instance)
    {
        WNDCLASSW windowClass{};
        windowClass.hInstance = instance;
        windowClass.lpfnWndProc = PaintProcedure;
        windowClass.lpszClassName = L"Win32BridgePaintContracts";
        CHECK(RegisterClassW(&windowClass));
        const HWND window = CreateWindowExW(0, windowClass.lpszClassName, L"", WS_POPUP | WS_VISIBLE,
            0, 0, 120, 100, nullptr, nullptr, instance, nullptr);
        CHECK(window != nullptr);
        if (!window) return;
        appRegion = CreateRectRgn(0, 0, 120, 100);
        copiedRegion = CreateRectRgn(1, 1, 2, 2);
        CHECK(appRegion && copiedRegion);
        const HDC dc = GetDC(window);
        CHECK(dc != nullptr);
        if (dc)
        {
            RECT bounds{};
            const RECT deviceRegion{ 0, 0, 120, 100 };
            POINT origin{}, screen{};
            CHECK(GetDCOrgEx(dc, &origin) && ClientToScreen(window, &screen));
            CHECK(origin.x == screen.x && origin.y == screen.y);
            CHECK(GetClipRgn(dc, copiedRegion) == 0);
            CHECK(SetViewportOrgEx(dc, 10, 12, nullptr));
            CHECK(SelectClipRgn(dc, appRegion) == SIMPLEREGION && GetClipRgn(dc, copiedRegion) == 1);
            CHECK(GetRgnBox(copiedRegion, &bounds) == SIMPLEREGION && EqualRect(&bounds, &deviceRegion));
            CHECK(SelectClipRgn(dc, nullptr) == SIMPLEREGION && GetClipRgn(dc, copiedRegion) == 0);
            CHECK(SetViewportOrgEx(dc, 0, 0, nullptr));
            CHECK(ReleaseDC(window, dc));
        }
        CHECK(ValidateRect(window, nullptr));
        measuringPaint = true;
        CHECK(RedrawWindow(window, &PaintArea, nullptr, RDW_INVALIDATE | RDW_ERASE | RDW_ERASENOW));
        CHECK(erases == 1 && paints == 0);
        RECT dirty{};
        CHECK(GetUpdateRect(window, &dirty, FALSE) && EqualRect(&dirty, &PaintArea));
        CHECK(RedrawWindow(window, nullptr, nullptr, RDW_UPDATENOW));
        CHECK(erases == 1 && paints == 1 && !GetUpdateRect(window, nullptr, FALSE));
        CHECK(RedrawWindow(window, &PaintArea, nullptr, RDW_INVALIDATE | RDW_ERASE | RDW_UPDATENOW));
        CHECK(erases == 2 && paints == 2 && !GetUpdateRect(window, nullptr, FALSE));
        measuringPaint = false;
        DestroyWindow(window);
        if (appRegion) DeleteObject(appRegion);
        if (copiedRegion) DeleteObject(copiedRegion);
    }

    INT_PTR CALLBACK PageProcedure(HWND page, UINT message, WPARAM, LPARAM parameter)
    {
        if (message == WM_INITDIALOG)
        {
            auto state = reinterpret_cast<PageState*>(reinterpret_cast<PROPSHEETPAGEW*>(parameter)->lParam);
            SetWindowLongPtrW(page, DWLP_USER, reinterpret_cast<LONG_PTR>(state));
            state->window = page;
            state->first = CreateWindowExW(0, L"EDIT", L"First", WS_CHILD | WS_VISIBLE | WS_TABSTOP,
                8, 8, 100, 22, page, reinterpret_cast<HMENU>(101), nullptr, nullptr);
            state->preferred = CreateWindowExW(0, L"EDIT", L"Preferred", WS_CHILD | WS_VISIBLE | WS_TABSTOP,
                8, 38, 100, 22, page, reinterpret_cast<HMENU>(102), nullptr, nullptr);
            return TRUE;
        }
        auto state = reinterpret_cast<PageState*>(GetWindowLongPtrW(page, DWLP_USER));
        if (message != WM_NOTIFY || !state || !parameter) return FALSE;
        const auto notification = reinterpret_cast<NMHDR*>(parameter);
        LRESULT result = 0;
        switch (notification->code)
        {
        case PSN_QUERYINITIALFOCUS: result = reinterpret_cast<LRESULT>(state->preferred); break;
        case PSN_KILLACTIVE: result = state->vetoLeave; break;
        case PSN_SETACTIVE: result = state->skip ? -1 : 0; break;
        case PSN_APPLY:
            ++state->applies;
            if (state->tryRecursiveApply)
            {
                state->tryRecursiveApply = false;
                state->recursiveRejected = SendMessageW(GetParent(page), PSM_APPLY, 0, 0) == FALSE;
            }
            result = state->vetoApply ? PSNRET_INVALID : PSNRET_NOERROR;
            break;
        default: return FALSE;
        }
        SetWindowLongPtrW(page, DWLP_MSGRESULT, result);
        return TRUE;
    }
    UINT CALLBACK PageCallback(HWND, UINT message, LPPROPSHEETPAGEW page)
    {
        if (message == PSPCB_RELEASE) ++reinterpret_cast<PageState*>(page->lParam)->releases;
        return TRUE;
    }
}

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int)
{
    CheckPaintContracts(instance);
    INITCOMMONCONTROLSEX controls{ sizeof(controls), ICC_TAB_CLASSES };
    CHECK(InitCommonControlsEx(&controls));
    EmptyTemplate content{};
    content.dialog.style = WS_CHILD | DS_CONTROL;
    content.dialog.cx = 160; content.dialog.cy = 85;
    PageState states[3]{};
    HPROPSHEETPAGE pages[3]{};
    for (unsigned index = 0; index != 3; ++index)
    {
        PROPSHEETPAGEW page{};
        page.dwSize = sizeof(page); page.dwFlags = PSP_DLGINDIRECT | PSP_USETITLE | PSP_USECALLBACK;
        page.hInstance = instance; page.pResource = &content.dialog;
        page.pszTitle = index == 0 ? L"First" : index == 1 ? L"Second" : L"Third";
        page.pfnDlgProc = PageProcedure; page.lParam = reinterpret_cast<LPARAM>(&states[index]);
        page.pfnCallback = PageCallback;
        pages[index] = CreatePropertySheetPageW(&page);
        CHECK(pages[index] != nullptr);
    }
    if (failure)
    {
        for (auto page : pages) if (page) DestroyPropertySheetPage(page);
        return failure;
    }
    PROPSHEETHEADERW header{};
    header.dwSize = sizeof(header); header.dwFlags = PSH_MODELESS;
    header.hInstance = instance; header.pszCaption = L"Win32Bridge dialog contracts";
    header.nPages = 3; header.phpage = pages;
    const HWND sheet = reinterpret_cast<HWND>(PropertySheetW(&header));
    CHECK(sheet && sheet != reinterpret_cast<HWND>(-1));
    if (failure) return failure;
    const HWND tab = reinterpret_cast<HWND>(SendMessageW(sheet, PSM_GETTABCONTROL, 0, 0));
    CHECK(tab && IsWindow(tab));
    CHECK(SendMessageW(sheet, PSM_GETCURRENTPAGEHWND, 0, 0) == reinterpret_cast<LRESULT>(states[0].window));
    CHECK(GetFocus() == states[0].preferred);
    CHECK(SendMessageW(sheet, PSM_HWNDTOINDEX, 0, 0) == -1);
    CHECK(SendMessageW(sheet, PSM_HWNDTOINDEX, reinterpret_cast<WPARAM>(states[0].window), 0) == 0);
    CHECK(SendMessageW(sheet, PSM_INDEXTOHWND, 1, 0) == 0); // lazy page
    CHECK(SendMessageW(sheet, PSM_INDEXTOPAGE, 1, 0) == reinterpret_cast<LRESULT>(pages[1]));
    CHECK(SendMessageW(sheet, PSM_PAGETOINDEX, 0, reinterpret_cast<LPARAM>(pages[2])) == 2);
    CHECK(SendMessageW(sheet, PSM_PAGETOINDEX, 0, 0) == -1);
    CHECK(SendMessageW(sheet, PSM_SETCURSEL, 1, 0) != FALSE);
    CHECK(GetFocus() == states[1].preferred && !IsWindowVisible(states[0].window));
    CHECK(SendMessageW(sheet, PSM_SETCURSEL, 0, 0) != FALSE);
    states[0].vetoLeave = true;
    CHECK(SendMessageW(sheet, PSM_SETCURSEL, 1, 0) == FALSE);
    CHECK(SendMessageW(sheet, PSM_GETCURRENTPAGEHWND, 0, 0) == reinterpret_cast<LRESULT>(states[0].window));
    states[0].vetoLeave = false;
    states[1].skip = true;
    CHECK(SendMessageW(sheet, PSM_SETCURSEL, 1, 0) != FALSE);
    CHECK(SendMessageW(sheet, PSM_GETCURRENTPAGEHWND, 0, 0) == reinterpret_cast<LRESULT>(states[2].window));
    CHECK(GetFocus() == states[2].preferred);
    states[1].skip = false;
    CHECK(SendMessageW(sheet, PSM_SETCURSEL, 0, 0) != FALSE);
    CHECK(GetNextDlgTabItem(states[0].window, nullptr, FALSE) == states[0].first);
    EnableWindow(states[0].first, FALSE);
    CHECK(GetNextDlgTabItem(states[0].window, nullptr, FALSE) == states[0].preferred);
    EnableWindow(states[0].first, TRUE);
    const HWND spare = CreateWindowExW(0, L"BUTTON", L"Extra", WS_CHILD | WS_TABSTOP,
        120, 8, 40, 22, states[0].window, reinterpret_cast<HMENU>(103), instance, nullptr);
    CHECK(spare != nullptr);
    const HWND focused = GetFocus();
    ShowWindow(spare, SW_SHOW);
    CHECK(GetFocus() == focused);
    ShowWindow(spare, SW_HIDE);
    SetWindowPos(spare, nullptr, 0, 0, 0, 0, SWP_SHOWWINDOW | SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER);
    CHECK(GetFocus() == focused);
    if (SetForegroundWindow(sheet)) CHECK(GetFocus() == focused);
    RECT before{}, after{}, frame{}, tabBefore{}, tabAfter{};
    GetClientRect(states[0].window, &before); GetWindowRect(sheet, &frame); GetWindowRect(tab, &tabBefore);
    SetWindowPos(sheet, nullptr, 0, 0, frame.right - frame.left + 80, frame.bottom - frame.top + 60,
        SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
    GetClientRect(states[0].window, &after); GetWindowRect(tab, &tabAfter);
    CHECK(after.right - before.right == 80 && after.bottom - before.bottom == 60);
    CHECK(tabAfter.right - tabBefore.right == 80 && tabAfter.bottom - tabBefore.bottom == 60);
    states[0].vetoApply = true;
    CHECK(SendMessageW(sheet, PSM_APPLY, 0, 0) == FALSE);
    states[0].vetoApply = false; states[0].tryRecursiveApply = true;
    CHECK(SendMessageW(sheet, PSM_APPLY, 0, 0) != FALSE);
    CHECK(states[0].recursiveRejected);
    CHECK(states[0].applies == 2 && states[1].applies == 1 && states[2].applies == 1);
    SendMessageW(sheet, PSM_PRESSBUTTON, PSBTN_CANCEL, 0);
    CHECK(IsWindow(sheet)); // application owns modeless destruction
    CHECK(SendMessageW(sheet, PSM_GETCURRENTPAGEHWND, 0, 0) == 0);
    CHECK(SendMessageW(sheet, PSM_GETRESULT, 0, 0) == 0);
    DestroyWindow(sheet);
    for (auto& state : states) CHECK(state.releases == 1);
    return failure; // zero = success, otherwise first failing source line
}
