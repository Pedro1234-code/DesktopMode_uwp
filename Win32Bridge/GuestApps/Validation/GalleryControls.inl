    constexpr UINT Preview = 230, Slider = 231, Scroll = 232, Meter = 233, Spinner = 234;
    constexpr UINT Status = 235, ToolbarFirst = 240, ToolbarSecond = 241;

    void Label(HWND window, LPCWSTR text, int x, int y, int width)
    {
        Add(window, L"STATIC", text, 0, x, y, width, 23);
    }

    void PopulateInputs(HWND window)
    {
        Label(window, L"Text fields, selection, buttons and scroll messages", 12, 12, 525);
        Add(window, L"BUTTON", L"Text and selection", BS_GROUPBOX, 12, 41, 260, 273);
        Add(window, L"EDIT", L"Editable text\r\nSecond line", WS_BORDER | WS_TABSTOP | WS_VSCROLL |
            ES_MULTILINE | ES_AUTOVSCROLL | ES_WANTRETURN, 25, 65, 232, 73, 260);
        Add(window, L"EDIT", L"password", WS_BORDER | WS_TABSTOP | ES_PASSWORD | ES_AUTOHSCROLL, 25, 149, 232, 26, 261);
        HWND list = Add(window, L"LISTBOX", L"", WS_BORDER | WS_VSCROLL | WS_TABSTOP |
            LBS_NOTIFY | LBS_EXTENDEDSEL, 25, 185, 232, 113, 262);
        for (const auto* text : {L"First option", L"Second option", L"Third option", L"Fourth option", L"Fifth option"})
            SendMessageW(list, LB_ADDSTRING, 0, reinterpret_cast<LPARAM>(text));
        SendMessageW(list, LB_SETSEL, TRUE, 1);

        Add(window, L"BUTTON", L"Buttons and Unicode", BS_GROUPBOX, 282, 41, 260, 273);
        HWND first = Add(window, L"BUTTON", L"Option A", BS_AUTORADIOBUTTON | WS_GROUP | WS_TABSTOP, 295, 67, 114, 24, 263);
        Add(window, L"BUTTON", L"Option B", BS_AUTORADIOBUTTON | WS_TABSTOP, 414, 67, 112, 24, 264);
        SendMessageW(first, BM_SETCHECK, BST_CHECKED, 0);
        HWND check = Add(window, L"BUTTON", L"Cycle three states", BS_AUTO3STATE | WS_GROUP | WS_TABSTOP,
            295, 103, 222, 26, 265);
        SendMessageW(check, BM_SETCHECK, BST_INDETERMINATE, 0);
        HWND combo = Add(window, L"COMBOBOX", L"", CBS_DROPDOWN | WS_BORDER | WS_TABSTOP | WS_VSCROLL, 295, 143, 223, 108, 266);
        for (const auto* text : {L"Editable combo", L"Portugu\u00eas", L"\u65e5\u672c\u8a9e"})
            SendMessageW(combo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(text));
        SendMessageW(combo, CB_SETCURSEL, 0, 0);
        HWND scrollbar = Add(window, L"SCROLLBAR", L"", SBS_HORZ | WS_TABSTOP, 295, 190, 223, 23, Scroll);
        SCROLLINFO info{sizeof(info), SIF_RANGE | SIF_PAGE | SIF_POS, 0, 100, 10, 25, 0};
        SetScrollInfo(scrollbar, SB_CTL, &info, TRUE);
        Add(window, L"STATIC", L"Scroll position: 25", SS_SUNKEN, 295, 229, 223, 27, Preview);
        Label(window, L"Shift/Ctrl select list rows. Tab moves focus.", 24, 323, 400);
    }

    void PopulateRanges(HWND window)
    {
        Label(window, L"Trackbar, progress and up-down buddy", 12, 12, 530);
        HWND slider = Add(window, TRACKBAR_CLASSW, L"", TBS_AUTOTICKS | WS_TABSTOP, 12, 37, 283, 35, Slider);
        SendMessageW(slider, TBM_SETRANGE, TRUE, MAKELPARAM(0, 100));
        SendMessageW(slider, TBM_SETTICFREQ, 10, 0);
        SendMessageW(slider, TBM_SETPOS, TRUE, 35);
        HWND meter = Add(window, PROGRESS_CLASSW, L"", PBS_SMOOTH, 14, 83, 270, 22, Meter);
        SendMessageW(meter, PBM_SETRANGE32, 0, 100);
        SendMessageW(meter, PBM_SETPOS, 35, 0);
        Add(window, L"STATIC", L"Trackbar: 35", SS_SUNKEN, 315, 83, 214, 24, Preview);
        HWND buddy = Add(window, L"EDIT", L"42", WS_BORDER | WS_TABSTOP | ES_NUMBER, 315, 40, 192, 26, 267);
        HWND spin = Add(window, UPDOWN_CLASSW, L"", UDS_ALIGNRIGHT | UDS_SETBUDDYINT | UDS_ARROWKEYS,
            507, 40, 22, 26, Spinner);
        SendMessageW(spin, UDM_SETBUDDY, reinterpret_cast<WPARAM>(buddy), 0);
        SendMessageW(spin, UDM_SETRANGE32, 0, 100);
        SendMessageW(spin, UDM_SETPOS32, 0, 42);
        Label(window, L"Month calendar", 14, 120, 275);
        Add(window, MONTHCAL_CLASSW, L"", WS_TABSTOP | MCS_NOTODAY, 14, 145, 270, 172, 268);
        Label(window, L"Date picker", 315, 120, 214);
        Add(window, DATETIMEPICK_CLASSW, L"", WS_TABSTOP | DTS_SHORTDATEFORMAT, 315, 145, 214, 27, 269);
        Label(window, L"IP address", 315, 189, 214);
        HWND ip = Add(window, WC_IPADDRESSW, L"", WS_TABSTOP, 315, 213, 214, 27, 270);
        SendMessageW(ip, IPM_SETADDRESS, 0, MAKEIPADDRESS(192, 0, 2, 42));
        Label(window, L"Hotkey (Ctrl+K initially)", 315, 258, 214);
        HWND hotkey = Add(window, HOTKEY_CLASSW, L"", WS_TABSTOP | WS_BORDER, 315, 283, 214, 27, 271);
        SendMessageW(hotkey, HKM_SETHOTKEY, MAKEWORD('K', HOTKEYF_CONTROL), 0);
    }

    void AddTooltip(HWND window, HWND target)
    {
        if (!target) return;
        HWND tip = CreateWindowExW(WS_EX_TOPMOST, TOOLTIPS_CLASSW, nullptr, WS_POPUP | TTS_ALWAYSTIP | TTS_NOPREFIX,
            CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT, window, nullptr, instance, nullptr);
        if (!tip) { events->Write(L"FAIL", TOOLTIPS_CLASSW, GetLastError()); return; }
        TTTOOLINFOW info{};
        info.cbSize = sizeof(info);
        info.hwnd = window;
        info.uFlags = TTF_IDISHWND | TTF_SUBCLASS;
        info.uId = reinterpret_cast<UINT_PTR>(target);
        static wchar_t description[] = L"This tooltip is a separate common control.";
        info.lpszText = description;
        if (!SendMessageW(tip, TTM_ADDTOOLW, 0, reinterpret_cast<LPARAM>(&info)))
            events->Write(L"FAIL", L"TTM_ADDTOOLW");
    }

    void PopulateCommands(HWND window)
    {
        Label(window, L"Drag the rebar gripper, resize headers and try the toolbar buttons.", 12, 10, 535);
        HWND rebar = Add(window, REBARCLASSNAMEW, L"", CCS_NORESIZE | CCS_NOPARENTALIGN | RBS_VARHEIGHT | WS_CLIPCHILDREN,
            12, 37, 525, 57, 272);
        HWND toolbar = Add(rebar ? rebar : window, TOOLBARCLASSNAMEW, L"",
            TBSTYLE_FLAT | TBSTYLE_LIST | CCS_NORESIZE | CCS_NOPARENTALIGN, 12, 38, 240, 30, 273);
        SendMessageW(toolbar, TB_BUTTONSTRUCTSIZE, sizeof(TBBUTTON), 0);
        static wchar_t first[] = L"Run", second[] = L"Toggle";
        TBBUTTON buttons[2]{};
        buttons[0].iBitmap = buttons[1].iBitmap = I_IMAGENONE;
        buttons[0].idCommand = ToolbarFirst; buttons[1].idCommand = ToolbarSecond;
        buttons[0].fsState = buttons[1].fsState = TBSTATE_ENABLED;
        buttons[0].fsStyle = BTNS_BUTTON | BTNS_AUTOSIZE | BTNS_SHOWTEXT;
        buttons[1].fsStyle = BTNS_CHECK | BTNS_AUTOSIZE | BTNS_SHOWTEXT;
        buttons[0].iString = reinterpret_cast<INT_PTR>(first);
        buttons[1].iString = reinterpret_cast<INT_PTR>(second);
        SendMessageW(toolbar, TB_ADDBUTTONSW, ARRAYSIZE(buttons), reinterpret_cast<LPARAM>(buttons));
        if (rebar && toolbar)
        {
            REBARBANDINFOW band{};
            band.cbSize = sizeof(band);
            band.fMask = RBBIM_CHILD | RBBIM_CHILDSIZE | RBBIM_SIZE | RBBIM_STYLE;
            band.fStyle = RBBS_GRIPPERALWAYS;
            band.hwndChild = toolbar; band.cxMinChild = 140; band.cyMinChild = 27; band.cx = 255;
            if (!SendMessageW(rebar, RB_INSERTBANDW, static_cast<WPARAM>(-1), reinterpret_cast<LPARAM>(&band)))
                events->Write(L"FAIL", L"RB_INSERTBANDW");
        }
        SendMessageW(toolbar, TB_SETPARENT, reinterpret_cast<WPARAM>(window), 0);
        Label(window, L"Standalone header", 12, 103, 525);
        HWND header = Add(window, WC_HEADERW, L"", HDS_BUTTONS | HDS_DRAGDROP, 12, 129, 525, 29, 274);
        wchar_t name[] = L"Name", state[] = L"State", details[] = L"Details";
        wchar_t* titles[] = {name, state, details};
        for (int i = 0; i != ARRAYSIZE(titles); ++i)
        {
            HDITEMW item{}; item.mask = HDI_TEXT | HDI_WIDTH | HDI_FORMAT;
            item.cxy = 160; item.pszText = titles[i]; item.fmt = HDF_STRING | HDF_LEFT;
            SendMessageW(header, HDM_INSERTITEMW, i, reinterpret_cast<LPARAM>(&item));
        }
        Label(window, L"ComboBoxEx", 12, 176, 245);
        HWND combo = Add(window, WC_COMBOBOXEXW, L"", CBS_DROPDOWNLIST | WS_TABSTOP,
            12, 202, 245, 112, 275);
        for (auto* title : titles)
        {
            COMBOBOXEXITEMW item{};
            item.mask = CBEIF_TEXT; item.iItem = -1; item.pszText = title;
            SendMessageW(combo, CBEM_INSERTITEMW, 0, reinterpret_cast<LPARAM>(&item));
        }
        SendMessageW(combo, CB_SETCURSEL, 0, 0);
        Add(window, WC_LINK, L"<a id=\"demo\">Activate this link</a>", WS_TABSTOP, 287, 201, 245, 26, 276);
        HWND target = Add(window, L"BUTTON", L"Hover for tooltip", WS_TABSTOP, 12, 251, 245, 30, 277);
        AddTooltip(window, target);
        HWND bar = Add(window, STATUSCLASSNAMEW, L"", CCS_NOMOVEY | CCS_NOPARENTALIGN | CCS_NORESIZE | SBARS_SIZEGRIP,
            12, 313, 525, 26, Status);
        const int parts[] = {265, -1};
        SendMessageW(bar, SB_SETPARTS, ARRAYSIZE(parts), reinterpret_cast<LPARAM>(parts));
        SendMessageW(bar, SB_SETTEXTW, 0, reinterpret_cast<LPARAM>(L"Choose a toolbar command"));
        SendMessageW(bar, SB_SETTEXTW, 1, reinterpret_cast<LPARAM>(L"Status-bar second pane"));
    }

    void PopulateVirtualList(HWND window)
    {
        Label(window, L"Scroll 10,000 rows. Cell text is supplied by LVN_GETDISPINFO.", 12, 12, 525);
        HWND list = Add(window, WC_LISTVIEWW, L"", WS_BORDER | WS_TABSTOP | LVS_OWNERDATA | LVS_REPORT | LVS_SHOWSELALWAYS,
            12, 46, 525, 282, 278);
        SendMessageW(list, LVM_SETEXTENDEDLISTVIEWSTYLE, 0, LVS_EX_FULLROWSELECT | LVS_EX_GRIDLINES);
        wchar_t row[] = L"Virtual row", payload[] = L"Callback value";
        LVCOLUMNW column{}; column.mask = LVCF_WIDTH | LVCF_TEXT; column.cx = 230;
        column.pszText = row;
        SendMessageW(list, LVM_INSERTCOLUMNW, 0, reinterpret_cast<LPARAM>(&column));
        column.pszText = payload;
        SendMessageW(list, LVM_INSERTCOLUMNW, 1, reinterpret_cast<LPARAM>(&column));
        SendMessageW(list, LVM_SETITEMCOUNT, 10000, 0);
        Add(window, L"BUTTON", L"Jump to last row", WS_TABSTOP, 12, 337, 185, 26, 279);
    }

    void UpdatePreview(HWND window, HWND control, WPARAM wp)
    {
        const int id = GetDlgCtrlID(control);
        wchar_t caption[72]{};
        if (id == Slider)
        {
            const auto position = SendMessageW(control, TBM_GETPOS, 0, 0);
            SendDlgItemMessageW(window, Meter, PBM_SETPOS, position, 0);
            swprintf_s(caption, L"Trackbar: %ld", static_cast<long>(position));
        }
        else if (id == Scroll)
        {
            SCROLLINFO info{sizeof(info), SIF_ALL};
            if (!GetScrollInfo(control, SB_CTL, &info)) return;
            switch (LOWORD(wp))
            {
            case SB_LINELEFT: --info.nPos; break;
            case SB_LINERIGHT: ++info.nPos; break;
            case SB_PAGELEFT: info.nPos -= static_cast<int>(info.nPage); break;
            case SB_PAGERIGHT: info.nPos += static_cast<int>(info.nPage); break;
            case SB_THUMBTRACK: case SB_THUMBPOSITION: info.nPos = HIWORD(wp); break;
            case SB_LEFT: info.nPos = info.nMin; break;
            case SB_RIGHT: info.nPos = info.nMax; break;
            default: return;
            }
            info.fMask = SIF_POS;
            const int position = SetScrollInfo(control, SB_CTL, &info, TRUE);
            swprintf_s(caption, L"Scroll position: %d", position);
        }
        else return;
        SetWindowTextW(GetDlgItem(window, Preview), caption);
    }
