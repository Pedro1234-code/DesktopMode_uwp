#pragma once

#include "Bridge\\CompatibilityCatalog.h"
#include "Bridge\\GuestWin32Abi.h"

#include <windows.h>

namespace Win32Bridge
{
namespace Bridge
{
    HBRUSH WINAPI BridgeCreateSolidBrush(COLORREF color);
    HPEN WINAPI BridgeCreatePen(int style, int width, COLORREF color);
    HFONT WINAPI BridgeCreateFontW(
        int height,
        int width,
        int escapement,
        int orientation,
        int weight,
        DWORD italic,
        DWORD underline,
        DWORD strikeOut,
        DWORD charSet,
        DWORD outPrecision,
        DWORD clipPrecision,
        DWORD quality,
        DWORD pitchAndFamily,
        LPCWSTR faceName);
    HFONT WINAPI BridgeCreateFontIndirectW(const GuestAbi::LogFontW* logFont);
    BOOL WINAPI BridgeGetTextMetricsW(HDC dc, GuestAbi::TextMetricW* metrics);
    int WINAPI BridgeGetTextFaceW(HDC dc, int characterCapacity, LPWSTR faceName);
    int WINAPI BridgeGetObjectW(HGDIOBJ object, int bufferBytes, LPVOID buffer);
    HGDIOBJ WINAPI BridgeGetStockObject(int object);
    BOOL WINAPI BridgeDeleteObject(HGDIOBJ object);
    HGDIOBJ WINAPI BridgeSelectObject(HDC dc, HGDIOBJ object);
    COLORREF WINAPI BridgeSetTextColor(HDC dc, COLORREF color);
    COLORREF WINAPI BridgeSetBkColor(HDC dc, COLORREF color);
    int WINAPI BridgeSetBkMode(HDC dc, int mode);
    BOOL WINAPI BridgeRectangle(HDC dc, int left, int top, int right, int bottom);
    BOOL WINAPI BridgeEllipse(HDC dc, int left, int top, int right, int bottom);
    BOOL WINAPI BridgeMoveToEx(HDC dc, int x, int y, LPPOINT previous);
    BOOL WINAPI BridgeLineTo(HDC dc, int x, int y);
    BOOL WINAPI BridgeSetPixelV(HDC dc, int x, int y, COLORREF color);
    COLORREF WINAPI BridgeGetPixel(HDC dc, int x, int y);
    int WINAPI BridgeGetDeviceCaps(HDC dc, int index);
    BOOL WINAPI BridgeTextOutW(HDC dc, int x, int y, LPCWSTR text, int characterCount);
    BOOL WINAPI BridgeGetTextExtentPoint32W(HDC dc, LPCWSTR text, int characterCount, LPSIZE extent);
    BOOL WINAPI BridgeBitBlt(HDC destination, int x, int y, int width, int height, HDC source, int sourceX, int sourceY, DWORD rasterOperation);
    HDC WINAPI BridgeCreateCompatibleDC(HDC dc);
    BOOL WINAPI BridgeDeleteDC(HDC dc);
    HBITMAP WINAPI BridgeCreateCompatibleBitmap(HDC dc, int width, int height);

    ImportResolution ResolveGdi32Import(const ImportedSymbol& symbol);
}
}
