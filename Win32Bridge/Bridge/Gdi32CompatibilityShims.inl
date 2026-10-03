namespace
{
using GuestAbortProc = BOOL (CALLBACK*)(HDC, int);
using GuestFontEnumProcW = int (CALLBACK*)(const LOGFONTW*, const TEXTMETRICW*, DWORD, LPARAM);

int WINAPI BridgeStartPage(HDC) { BridgeSetLastError(ERROR_CALL_NOT_IMPLEMENTED); return SP_ERROR; }
int WINAPI BridgeStartDocW(HDC, const void*) { BridgeSetLastError(ERROR_CALL_NOT_IMPLEMENTED); return SP_ERROR; }
int WINAPI BridgeSetAbortProc(HDC, GuestAbortProc) { BridgeSetLastError(ERROR_CALL_NOT_IMPLEMENTED); return SP_ERROR; }
int WINAPI BridgeEndDoc(HDC) { BridgeSetLastError(ERROR_CALL_NOT_IMPLEMENTED); return SP_ERROR; }
int WINAPI BridgeAbortDoc(HDC) { BridgeSetLastError(ERROR_CALL_NOT_IMPLEMENTED); return SP_ERROR; }
int WINAPI BridgeEndPage(HDC) { BridgeSetLastError(ERROR_CALL_NOT_IMPLEMENTED); return SP_ERROR; }

BOOL WINAPI BridgeLPtoDP(HDC, LPPOINT points, int count)
{
    if (!points || count < 0) { BridgeSetLastError(ERROR_INVALID_PARAMETER); return FALSE; }
    return TRUE;
}

BOOL WINAPI BridgeSetWindowExtEx(HDC, int x, int y, LPSIZE previous)
{
    if (previous) *previous = SIZE{ 1, 1 };
    return x != 0 && y != 0;
}

BOOL WINAPI BridgeSetViewportExtEx(HDC, int x, int y, LPSIZE previous)
{
    if (previous) *previous = SIZE{ 1, 1 };
    return x != 0 && y != 0;
}

int WINAPI BridgeSetMapMode(HDC, int mode)
{
    return mode >= MM_TEXT && mode <= MM_ANISOTROPIC ? MM_TEXT : 0;
}

int WINAPI BridgeEnumFontsW(
    HDC, LPCWSTR faceName, GuestFontEnumProcW callback, LPARAM parameter)
{
    if (!callback) return 0;
    LOGFONTW font = {};
    wcscpy_s(font.lfFaceName, faceName && *faceName ? faceName : L"Segoe UI");
    TEXTMETRICW metrics = {};
    metrics.tmHeight = 16;
    metrics.tmAscent = 13;
    metrics.tmDescent = 3;
    metrics.tmAveCharWidth = 8;
    metrics.tmMaxCharWidth = 16;
    metrics.tmCharSet = DEFAULT_CHARSET;
    return callback(&font, &metrics, 0, parameter);
}

HDC WINAPI BridgeCreateDCW(LPCWSTR, LPCWSTR, LPCWSTR, const void*)
{
    BridgeSetLastError(ERROR_CALL_NOT_IMPLEMENTED);
    return nullptr;
}
}
