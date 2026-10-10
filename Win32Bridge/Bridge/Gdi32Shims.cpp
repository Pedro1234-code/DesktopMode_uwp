#include "pch.h"
#include "Bridge\\Gdi32Shims.h"

#include "Bridge\\GuestWindow.h"
#include "Bridge\\GuestWin32Abi.h"
#include "Bridge\\Kernel32Shims.h"
#include "Bridge\\MiniGdi.h"
#include "Bridge\\RuntimeDiagnostics.h"

#include <algorithm>
#include <cstring>
#include <limits>
#include <mutex>
#include <unordered_map>
#include <vector>

using namespace Win32Bridge::Bridge;

namespace
{
    static_assert(
        MiniGdi::FontFaceNameCapacity == static_cast<std::size_t>(GuestAbi::LfFaceSize),
        "MiniGdi and guest LOGFONTW face-name capacities must agree.");

    bool IsGdiLibrary(const std::wstring& library)
    {
        return _wcsicmp(library.c_str(), L"gdi32.dll") == 0 ||
            _wcsicmp(library.c_str(), L"gdi32full.dll") == 0 ||
            _wcsicmp(library.c_str(), L"msimg32.dll") == 0 ||
            _wcsnicmp(library.c_str(), L"api-ms-win-gdi-", 15) == 0;
    }

    GuestWindowManager* CurrentManagerOrFail()
    {
        GuestWindowManager* manager = CurrentGuestWindowManager();
        if (!manager)
        {
            BridgeSetLastError(ERROR_INVALID_FUNCTION);
        }
        return manager;
    }

    HGDIOBJ ToGuestObject(MiniGdi::ObjectHandle object)
    {
        return reinterpret_cast<HGDIOBJ>(static_cast<ULONG_PTR>(object));
    }

    HDC ToGuestDc(MiniGdi::DcHandle dc)
    {
        return reinterpret_cast<HDC>(static_cast<ULONG_PTR>(dc));
    }

    HGDIOBJ InvalidGuestGdiObject()
    {
        return reinterpret_cast<HGDIOBJ>(static_cast<ULONG_PTR>(-1));
    }

    MiniGdi::ObjectHandle FromGuestObject(HGDIOBJ object)
    {
        const ULONG_PTR value = reinterpret_cast<ULONG_PTR>(object);
        if (value == 0 || value > static_cast<ULONG_PTR>((std::numeric_limits<MiniGdi::ObjectHandle>::max)()))
        {
            return MiniGdi::InvalidObject;
        }
        return static_cast<MiniGdi::ObjectHandle>(value);
    }

    MiniGdi::DcHandle FromGuestDc(HDC dc)
    {
        const ULONG_PTR value = reinterpret_cast<ULONG_PTR>(dc);
        if (value == 0 || value > static_cast<ULONG_PTR>((std::numeric_limits<MiniGdi::DcHandle>::max)()))
        {
            return MiniGdi::InvalidDc;
        }
        return static_cast<MiniGdi::DcHandle>(value);
    }

    MiniGdi::Color ColorFromColorRef(COLORREF color)
    {
        return MiniGdi::MakeColor(
            static_cast<std::uint8_t>(color & 0xff),
            static_cast<std::uint8_t>((color >> 8) & 0xff),
            static_cast<std::uint8_t>((color >> 16) & 0xff));
    }

    COLORREF ColorRefFromColor(MiniGdi::Color color)
    {
        return static_cast<COLORREF>(MiniGdi::Red(color) |
            (static_cast<COLORREF>(MiniGdi::Green(color)) << 8) |
            (static_cast<COLORREF>(MiniGdi::Blue(color)) << 16));
    }

    int Win32BackgroundMode(MiniGdi::BackgroundMode mode)
    {
        return mode == MiniGdi::BackgroundMode::Transparent
            ? GuestAbi::BackgroundTransparent
            : GuestAbi::BackgroundOpaque;
    }

    bool MiniGdiBackgroundMode(int mode, MiniGdi::BackgroundMode* result)
    {
        if (!result)
        {
            return false;
        }

        switch (mode)
        {
        case GuestAbi::BackgroundOpaque:
            *result = MiniGdi::BackgroundMode::Opaque;
            return true;
        case GuestAbi::BackgroundTransparent:
            *result = MiniGdi::BackgroundMode::Transparent;
            return true;
        default:
            return false;
        }
    }

    MiniGdi::Font MakeDefaultMiniGdiFont();

    MiniGdi::Font MakeStockFont(bool fixedPitch, bool oem = false)
    {
        MiniGdi::Font font = MakeDefaultMiniGdiFont();
        const wchar_t* family = fixedPitch ? L"Consolas" : L"Segoe UI";
        for (std::size_t index = 0; index < MiniGdi::FontFaceNameCapacity; ++index)
            font.faceName[index] = L'\0';
        for (std::size_t index = 0;
            family[index] && index + 1 < MiniGdi::FontFaceNameCapacity; ++index)
            font.faceName[index] = family[index];
        font.width = fixedPitch ? 8 : 0;
        font.pitchAndFamily = fixedPitch ? 0x01 : 0x02;
        if (oem) font.charSet = 255;
        return font;
    }

    MiniGdi::ObjectHandle CreateStockObject(GuestWindowManager* manager, int object)
    {
        if (!manager)
        {
            return MiniGdi::InvalidObject;
        }

        MiniGdi::GdiContext& gdi = manager->Gdi();
        switch (object)
        {
        case GuestAbi::BrushWhite:
            return gdi.CreateSolidBrush(MiniGdi::OpaqueWhite);
        case GuestAbi::BrushLtGray:
            return gdi.CreateSolidBrush(MiniGdi::MakeColor(192, 192, 192));
        case GuestAbi::BrushGray:
            return gdi.CreateSolidBrush(MiniGdi::MakeColor(128, 128, 128));
        case GuestAbi::BrushDkGray:
            return gdi.CreateSolidBrush(MiniGdi::MakeColor(64, 64, 64));
        case GuestAbi::BrushBlack:
            return gdi.CreateSolidBrush(MiniGdi::OpaqueBlack);
        case GuestAbi::BrushNull:
            return gdi.CreateNullBrush();
        case GuestAbi::PenWhite:
            return gdi.CreatePen(MiniGdi::OpaqueWhite);
        case GuestAbi::PenBlack:
            return gdi.CreatePen(MiniGdi::OpaqueBlack);
        case GuestAbi::PenNull:
            return gdi.CreateNullPen();
        case GuestAbi::FontOemFixed:
            return gdi.CreateFont(MakeStockFont(true, true));
        case GuestAbi::FontAnsiFixed:
        case GuestAbi::FontSystemFixed:
            return gdi.CreateFont(MakeStockFont(true));
        case GuestAbi::FontAnsiVariable:
        case GuestAbi::FontSystem:
        case GuestAbi::FontDeviceDefault:
        case GuestAbi::FontDefaultGui:
            return gdi.CreateFont(MakeStockFont(false));
        default:
            return MiniGdi::InvalidObject;
        }
    }

    MiniGdi::ObjectHandle CreateDefaultObjectForKind(
        GuestWindowManager* manager,
        MiniGdi::ObjectKind kind)
    {
        switch (kind)
        {
        case MiniGdi::ObjectKind::Pen:
            return manager->Gdi().CreatePen(MiniGdi::OpaqueBlack);
        case MiniGdi::ObjectKind::Brush:
            return manager->Gdi().CreateSolidBrush(MiniGdi::OpaqueWhite);
        case MiniGdi::ObjectKind::Font:
            return manager->Gdi().CreateFont(MakeDefaultMiniGdiFont());
        case MiniGdi::ObjectKind::Bitmap:
            // Windows memory DCs have a stock monochrome bitmap selected by
            // default.  A tiny transparent surface preserves the opaque-handle
            // contract without exposing a host bitmap.
            return manager->Gdi().CreateBitmap(1, 1, MiniGdi::Transparent);
        default:
            return MiniGdi::InvalidObject;
        }
    }

    bool HasValidDc(GuestWindowManager* manager, HDC dc, MiniGdi::DcHandle* resolvedDc)
    {
        if (!manager)
        {
            return false;
        }

        const MiniGdi::DcHandle value = FromGuestDc(dc);
        if (value == MiniGdi::InvalidDc || !manager->Gdi().HasDc(value))
        {
            BridgeSetLastError(ERROR_INVALID_HANDLE);
            return false;
        }

        if (resolvedDc)
        {
            *resolvedDc = value;
        }
        return true;
    }

    bool IsSupportedCreatePenStyle(int style, bool* nullPen)
    {
        if (!nullPen)
        {
            return false;
        }

        *nullPen = false;
        switch (style & GuestAbi::PenStyleMask)
        {
        case GuestAbi::PenSolid:
        case GuestAbi::PenInsideFrame:
            return true;
        case GuestAbi::PenNullStyle:
            *nullPen = true;
            return true;
        default:
            // Dashed and alternate pens need a dash rasterizer; failing here
            // is preferable to silently drawing an incorrect solid line.
            return false;
        }
    }

    MiniGdi::Font MakeMiniGdiFont(
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
        LPCWSTR faceName)
    {
        MiniGdi::Font font;
        font.height = height;
        font.width = width;
        font.escapement = escapement;
        font.orientation = orientation;
        font.weight = weight;
        font.italic = italic != 0;
        font.underline = underline != 0;
        font.strikeOut = strikeOut != 0;
        font.charSet = static_cast<std::uint8_t>(charSet);
        font.outPrecision = static_cast<std::uint8_t>(outPrecision);
        font.clipPrecision = static_cast<std::uint8_t>(clipPrecision);
        font.quality = static_cast<std::uint8_t>(quality);
        font.pitchAndFamily = static_cast<std::uint8_t>(pitchAndFamily);

        // CreateFontW permits a null face name.  When a name is supplied,
        // read at most LF_FACESIZE - 1 characters and always retain a final
        // terminator.  LOGFONTW has exactly LF_FACESIZE slots, so accepting a
        // non-terminated guest array must not turn later query APIs into an
        // unbounded string scan.
        if (faceName != nullptr)
        {
            for (std::size_t index = 0; index < MiniGdi::FontFaceNameCapacity; ++index)
            {
                font.faceName[index] = L'\0';
            }
            for (std::size_t index = 0; index + 1 < MiniGdi::FontFaceNameCapacity; ++index)
            {
                const wchar_t character = faceName[index];
                font.faceName[index] = character;
                if (character == L'\0')
                {
                    break;
                }
            }
        }
        return font;
    }

    std::size_t BoundedFaceNameLength(const MiniGdi::Font& font)
    {
        std::size_t length = 0;
        while (length < MiniGdi::FontFaceNameCapacity && font.faceName[length] != L'\0')
        {
            ++length;
        }
        return length;
    }

    GuestAbi::LogFontW ToGuestLogFont(const MiniGdi::Font& font)
    {
        GuestAbi::LogFontW result = {};
        result.lfHeight = font.height;
        result.lfWidth = font.width;
        result.lfEscapement = font.escapement;
        result.lfOrientation = font.orientation;
        result.lfWeight = font.weight;
        result.lfItalic = font.italic ? 1 : 0;
        result.lfUnderline = font.underline ? 1 : 0;
        result.lfStrikeOut = font.strikeOut ? 1 : 0;
        result.lfCharSet = font.charSet;
        result.lfOutPrecision = font.outPrecision;
        result.lfClipPrecision = font.clipPrecision;
        result.lfQuality = font.quality;
        result.lfPitchAndFamily = font.pitchAndFamily;

        // The stored face is already terminated by the creation path, but
        // bound this copy independently so a future Font producer cannot
        // cause GetObjectW to read or expose bytes beyond LF_FACESIZE.
        const std::size_t length = BoundedFaceNameLength(font);
        for (std::size_t index = 0; index < length; ++index)
        {
            result.lfFaceName[index] = font.faceName[index];
        }
        result.lfFaceName[GuestAbi::LfFaceSize - 1] = L'\0';
        return result;
    }

    MiniGdi::Font MakeDefaultMiniGdiFont()
    {
        MiniGdi::Font font;
        font.height = MiniGdi::DefaultTextGlyphHeight;
        font.width = MiniGdi::DefaultTextGlyphWidth;
        font.weight = 400;
        return font;
    }

    HFONT CreateGuestFont(GuestWindowManager* manager, const MiniGdi::Font& font)
    {
        const MiniGdi::ObjectHandle object = manager->Gdi().CreateFont(font);
        if (object == MiniGdi::InvalidObject)
        {
            BridgeSetLastError(ERROR_NOT_ENOUGH_MEMORY);
            return nullptr;
        }

        BridgeSetLastError(ERROR_SUCCESS);
        return reinterpret_cast<HFONT>(ToGuestObject(object));
    }
}

HBRUSH WINAPI Win32Bridge::Bridge::BridgeCreateSolidBrush(COLORREF color)
{
    GuestWindowManager* manager = CurrentManagerOrFail();
    if (!manager)
    {
        return nullptr;
    }

    const MiniGdi::ObjectHandle brush = manager->Gdi().CreateSolidBrush(ColorFromColorRef(color));
    if (brush == MiniGdi::InvalidObject)
    {
        BridgeSetLastError(ERROR_NOT_ENOUGH_MEMORY);
        return nullptr;
    }

    BridgeSetLastError(ERROR_SUCCESS);
    return reinterpret_cast<HBRUSH>(ToGuestObject(brush));
}

HPEN WINAPI Win32Bridge::Bridge::BridgeCreatePen(int style, int width, COLORREF color)
{
    GuestWindowManager* manager = CurrentManagerOrFail();
    if (!manager)
    {
        return nullptr;
    }

    bool nullPen = false;
    if (!IsSupportedCreatePenStyle(style, &nullPen) || width < 0)
    {
        BridgeSetLastError(ERROR_NOT_SUPPORTED);
        return nullptr;
    }

    // A cosmetic pen with width zero is one pixel wide.  Cap a guest supplied
    // geometric width to the maximum surface dimension accepted by USER32.
    const int effectiveWidth = width == 0 ? 1 : (width > 2048 ? 2048 : width);
    const MiniGdi::ObjectHandle pen = nullPen
        ? manager->Gdi().CreateNullPen()
        : manager->Gdi().CreatePen(ColorFromColorRef(color), effectiveWidth);
    if (pen == MiniGdi::InvalidObject)
    {
        BridgeSetLastError(ERROR_NOT_ENOUGH_MEMORY);
        return nullptr;
    }

    BridgeSetLastError(ERROR_SUCCESS);
    return reinterpret_cast<HPEN>(ToGuestObject(pen));
}

HFONT WINAPI Win32Bridge::Bridge::BridgeCreateFontW(
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
    LPCWSTR faceName)
{
    GuestWindowManager* manager = CurrentManagerOrFail();
    if (!manager)
    {
        return nullptr;
    }

    return CreateGuestFont(
        manager,
        MakeMiniGdiFont(
            height,
            width,
            escapement,
            orientation,
            weight,
            italic,
            underline,
            strikeOut,
            charSet,
            outPrecision,
            clipPrecision,
            quality,
            pitchAndFamily,
            faceName));
}

HFONT WINAPI Win32Bridge::Bridge::BridgeCreateFontIndirectW(const GuestAbi::LogFontW* logFont)
{
    if (logFont == nullptr)
    {
        BridgeSetLastError(ERROR_INVALID_PARAMETER);
        return nullptr;
    }

    GuestWindowManager* manager = CurrentManagerOrFail();
    if (!manager)
    {
        return nullptr;
    }

    return CreateGuestFont(
        manager,
        MakeMiniGdiFont(
            logFont->lfHeight,
            logFont->lfWidth,
            logFont->lfEscapement,
            logFont->lfOrientation,
            logFont->lfWeight,
            logFont->lfItalic,
            logFont->lfUnderline,
            logFont->lfStrikeOut,
            logFont->lfCharSet,
            logFont->lfOutPrecision,
            logFont->lfClipPrecision,
            logFont->lfQuality,
            logFont->lfPitchAndFamily,
            logFont->lfFaceName));
}

BOOL WINAPI Win32Bridge::Bridge::BridgeGetTextMetricsW(HDC dc, GuestAbi::TextMetricW* metrics)
{
    if (metrics == nullptr)
    {
        BridgeSetLastError(ERROR_INVALID_PARAMETER);
        return FALSE;
    }

    GuestWindowManager* manager = CurrentManagerOrFail();
    MiniGdi::DcHandle resolvedDc = MiniGdi::InvalidDc;
    if (!manager || !HasValidDc(manager, dc, &resolvedDc))
    {
        return FALSE;
    }

    MiniGdi::Font font;
    if (!manager->Gdi().GetSelectedFont(resolvedDc, &font))
    {
        BridgeSetLastError(ERROR_INVALID_HANDLE);
        return FALSE;
    }

    MiniGdi::FontMetrics measured{};
    if (!manager->Gdi().GetSelectedFontMetrics(resolvedDc, &measured))
    {
        BridgeSetLastError(ERROR_INVALID_HANDLE);
        return FALSE;
    }
    GuestAbi::TextMetricW result = {};
    result.tmHeight = measured.height;
    result.tmAscent = measured.ascent;
    result.tmDescent = measured.descent;
    result.tmInternalLeading = measured.internalLeading;
    result.tmExternalLeading = measured.externalLeading;
    result.tmAveCharWidth = measured.averageWidth;
    result.tmMaxCharWidth = measured.maximumWidth;
    result.tmWeight = font.weight;
    result.tmOverhang = 0;
    result.tmDigitizedAspectX = 1;
    result.tmDigitizedAspectY = 1;
    result.tmFirstChar = L' ';
    result.tmLastChar = L'~';
    result.tmDefaultChar = L'?';
    result.tmBreakChar = L' ';
    result.tmItalic = font.italic ? 1 : 0;
    result.tmUnderlined = font.underline ? 1 : 0;
    result.tmStruckOut = font.strikeOut ? 1 : 0;
    result.tmPitchAndFamily = font.pitchAndFamily;
    result.tmCharSet = font.charSet;
    *metrics = result;

    BridgeSetLastError(ERROR_SUCCESS);
    return TRUE;
}

int WINAPI Win32Bridge::Bridge::BridgeGetTextFaceW(
    HDC dc,
    int characterCapacity,
    LPWSTR faceName)
{
    if (characterCapacity < 0 || (characterCapacity != 0 && faceName == nullptr))
    {
        BridgeSetLastError(ERROR_INVALID_PARAMETER);
        return 0;
    }

    GuestWindowManager* manager = CurrentManagerOrFail();
    MiniGdi::DcHandle resolvedDc = MiniGdi::InvalidDc;
    if (!manager || !HasValidDc(manager, dc, &resolvedDc))
    {
        return 0;
    }

    MiniGdi::Font font;
    if (!manager->Gdi().GetSelectedFont(resolvedDc, &font))
    {
        BridgeSetLastError(ERROR_INVALID_HANDLE);
        return 0;
    }

    const std::size_t nameLength = BoundedFaceNameLength(font);
    if (characterCapacity == 0)
    {
        BridgeSetLastError(ERROR_SUCCESS);
        return static_cast<int>(nameLength);
    }

    const std::size_t copyCount = (std::min)(
        nameLength,
        static_cast<std::size_t>(characterCapacity - 1));
    for (std::size_t index = 0; index < copyCount; ++index)
    {
        faceName[index] = font.faceName[index];
    }
    faceName[copyCount] = L'\0';

    BridgeSetLastError(ERROR_SUCCESS);
    return static_cast<int>(copyCount);
}

int WINAPI Win32Bridge::Bridge::BridgeGetObjectW(
    HGDIOBJ object,
    int bufferBytes,
    LPVOID buffer)
{
    if (bufferBytes < 0)
    {
        BridgeSetLastError(ERROR_INVALID_PARAMETER);
        return 0;
    }

    GuestWindowManager* manager = CurrentManagerOrFail();
    const MiniGdi::ObjectHandle resolvedObject = FromGuestObject(object);
    if (!manager || resolvedObject == MiniGdi::InvalidObject)
    {
        if (manager)
        {
            BridgeSetLastError(ERROR_INVALID_HANDLE);
        }
        return 0;
    }

    MiniGdi::ObjectKind kind;
    if (!manager->Gdi().ObjectType(resolvedObject, &kind))
    {
        BridgeSetLastError(ERROR_INVALID_HANDLE);
        return 0;
    }
    if (kind != MiniGdi::ObjectKind::Font)
    {
        // Keep this entry deliberately narrow.  Returning misleading LOGFONT
        // bytes for a pen, brush, or bitmap would be worse than letting the
        // guest see a clear unsupported-object result.
        BridgeSetLastError(ERROR_NOT_SUPPORTED);
        return 0;
    }

    MiniGdi::Font font;
    if (!manager->Gdi().GetFont(resolvedObject, &font))
    {
        BridgeSetLastError(ERROR_INVALID_HANDLE);
        return 0;
    }

    constexpr int RequiredBytes = static_cast<int>(sizeof(GuestAbi::LogFontW));
    if (bufferBytes == 0)
    {
        BridgeSetLastError(ERROR_SUCCESS);
        return RequiredBytes;
    }
    if (buffer == nullptr)
    {
        BridgeSetLastError(ERROR_INVALID_PARAMETER);
        return 0;
    }
    if (bufferBytes < RequiredBytes)
    {
        BridgeSetLastError(ERROR_INSUFFICIENT_BUFFER);
        return 0;
    }

    const GuestAbi::LogFontW result = ToGuestLogFont(font);
    std::memcpy(buffer, &result, sizeof(result));
    BridgeSetLastError(ERROR_SUCCESS);
    return RequiredBytes;
}

HGDIOBJ WINAPI Win32Bridge::Bridge::BridgeGetStockObject(int object)
{
    GuestWindowManager* manager = CurrentManagerOrFail();
    if (!manager)
    {
        return nullptr;
    }

    // MiniGdi has no host GDI handles.  Return a fresh, ordinary object with
    // stock-equivalent attributes so it can be selected and safely deleted by
    // the guest without sharing state across guest processes.
    const MiniGdi::ObjectHandle stock = CreateStockObject(manager, object);
    if (stock == MiniGdi::InvalidObject)
    {
        BridgeSetLastError(ERROR_NOT_SUPPORTED);
        return nullptr;
    }

    BridgeSetLastError(ERROR_SUCCESS);
    return ToGuestObject(stock);
}

BOOL WINAPI Win32Bridge::Bridge::BridgeDeleteObject(HGDIOBJ object)
{
    GuestWindowManager* manager = CurrentManagerOrFail();
    const MiniGdi::ObjectHandle resolved = FromGuestObject(object);
    if (!manager || resolved == MiniGdi::InvalidObject)
    {
        if (manager)
        {
            BridgeSetLastError(ERROR_INVALID_HANDLE);
        }
        return FALSE;
    }

    if (!manager->Gdi().DeleteObject(resolved))
    {
        BridgeSetLastError(ERROR_INVALID_HANDLE);
        return FALSE;
    }

    BridgeSetLastError(ERROR_SUCCESS);
    return TRUE;
}

HGDIOBJ WINAPI Win32Bridge::Bridge::BridgeSelectObject(HDC dc, HGDIOBJ object)
{
    GuestWindowManager* manager = CurrentManagerOrFail();
    MiniGdi::DcHandle resolvedDc = MiniGdi::InvalidDc;
    const MiniGdi::ObjectHandle resolvedObject = FromGuestObject(object);
    if (!manager || resolvedObject == MiniGdi::InvalidObject || !HasValidDc(manager, dc, &resolvedDc))
    {
        if (manager && resolvedObject == MiniGdi::InvalidObject)
        {
            BridgeSetLastError(ERROR_INVALID_HANDLE);
        }
        return InvalidGuestGdiObject();
    }

    MiniGdi::ObjectKind kind;
    if (!manager->Gdi().ObjectType(resolvedObject, &kind))
    {
        BridgeSetLastError(ERROR_INVALID_HANDLE);
        return InvalidGuestGdiObject();
    }

    MiniGdi::ObjectHandle previous = MiniGdi::InvalidObject;
    bool selected = false;
    if (kind == MiniGdi::ObjectKind::Pen)
    {
        selected = manager->Gdi().SelectPen(resolvedDc, resolvedObject, &previous);
    }
    else if (kind == MiniGdi::ObjectKind::Brush)
    {
        selected = manager->Gdi().SelectBrush(resolvedDc, resolvedObject, &previous);
    }
    else if (kind == MiniGdi::ObjectKind::Font)
    {
        selected = manager->Gdi().SelectFont(resolvedDc, resolvedObject, &previous);
    }
    else if (kind == MiniGdi::ObjectKind::Bitmap)
    {
        selected = manager->Gdi().SelectBitmap(resolvedDc, resolvedObject, &previous);
    }
    if (!selected)
    {
        BridgeSetLastError(ERROR_INVALID_HANDLE);
        return InvalidGuestGdiObject();
    }

    if (previous == MiniGdi::InvalidObject)
    {
        // Selection initially uses an internal default.  Materialize a
        // disposable equivalent so SelectObject keeps the Win32 contract of
        // returning a usable object instead of a null failure value.
        previous = CreateDefaultObjectForKind(manager, kind);
        if (previous == MiniGdi::InvalidObject)
        {
            BridgeSetLastError(ERROR_NOT_ENOUGH_MEMORY);
            return InvalidGuestGdiObject();
        }
    }

    BridgeSetLastError(ERROR_SUCCESS);
    return ToGuestObject(previous);
}

COLORREF WINAPI Win32Bridge::Bridge::BridgeSetTextColor(HDC dc, COLORREF color)
{
    GuestWindowManager* manager = CurrentManagerOrFail();
    MiniGdi::DcHandle resolvedDc = MiniGdi::InvalidDc;
    if (!manager || !HasValidDc(manager, dc, &resolvedDc))
    {
        return GuestAbi::ColorInvalid;
    }

    MiniGdi::Color previous;
    if (!manager->Gdi().SetTextColor(resolvedDc, ColorFromColorRef(color), &previous))
    {
        BridgeSetLastError(ERROR_INVALID_HANDLE);
        return GuestAbi::ColorInvalid;
    }

    BridgeSetLastError(ERROR_SUCCESS);
    return ColorRefFromColor(previous);
}

COLORREF WINAPI Win32Bridge::Bridge::BridgeSetBkColor(HDC dc, COLORREF color)
{
    GuestWindowManager* manager = CurrentManagerOrFail();
    MiniGdi::DcHandle resolvedDc = MiniGdi::InvalidDc;
    if (!manager || !HasValidDc(manager, dc, &resolvedDc))
    {
        return GuestAbi::ColorInvalid;
    }

    MiniGdi::Color previous;
    if (!manager->Gdi().SetBackgroundColor(resolvedDc, ColorFromColorRef(color), &previous))
    {
        BridgeSetLastError(ERROR_INVALID_HANDLE);
        return GuestAbi::ColorInvalid;
    }

    BridgeSetLastError(ERROR_SUCCESS);
    return ColorRefFromColor(previous);
}

int WINAPI Win32Bridge::Bridge::BridgeSetBkMode(HDC dc, int mode)
{
    MiniGdi::BackgroundMode requested;
    if (!MiniGdiBackgroundMode(mode, &requested))
    {
        BridgeSetLastError(ERROR_INVALID_PARAMETER);
        return 0;
    }

    GuestWindowManager* manager = CurrentManagerOrFail();
    MiniGdi::DcHandle resolvedDc = MiniGdi::InvalidDc;
    if (!manager || !HasValidDc(manager, dc, &resolvedDc))
    {
        return 0;
    }

    MiniGdi::BackgroundMode previous;
    if (!manager->Gdi().SetBackgroundMode(resolvedDc, requested, &previous))
    {
        BridgeSetLastError(ERROR_INVALID_HANDLE);
        return 0;
    }

    BridgeSetLastError(ERROR_SUCCESS);
    return Win32BackgroundMode(previous);
}

BOOL WINAPI Win32Bridge::Bridge::BridgeRectangle(HDC dc, int left, int top, int right, int bottom)
{
    GuestWindowManager* manager = CurrentManagerOrFail();
    MiniGdi::DcHandle resolvedDc = MiniGdi::InvalidDc;
    if (!manager || !HasValidDc(manager, dc, &resolvedDc))
    {
        return FALSE;
    }

    manager->Gdi().Rectangle(resolvedDc, MiniGdi::Rect{ left, top, right, bottom });
    BridgeSetLastError(ERROR_SUCCESS);
    return TRUE;
}

BOOL WINAPI Win32Bridge::Bridge::BridgeEllipse(HDC dc, int left, int top, int right, int bottom)
{
    GuestWindowManager* manager = CurrentManagerOrFail();
    MiniGdi::DcHandle resolvedDc = MiniGdi::InvalidDc;
    if (!manager || !HasValidDc(manager, dc, &resolvedDc))
    {
        return FALSE;
    }
    if (!manager->Gdi().GetSurface(resolvedDc))
    {
        BridgeSetLastError(ERROR_INVALID_HANDLE);
        return FALSE;
    }

    // A NULL_PEN/NULL_BRUSH, a reversed rectangle, or a fully clipped ellipse
    // is a valid GDI no-op.  The only bridge-level failure here is an invalid
    // or targetless guest DC.
    manager->Gdi().Ellipse(resolvedDc, MiniGdi::Rect{ left, top, right, bottom });
    BridgeSetLastError(ERROR_SUCCESS);
    return TRUE;
}

BOOL WINAPI Win32Bridge::Bridge::BridgeMoveToEx(HDC dc, int x, int y, LPPOINT previous)
{
    GuestWindowManager* manager = CurrentManagerOrFail();
    MiniGdi::DcHandle resolvedDc = MiniGdi::InvalidDc;
    if (!manager || !HasValidDc(manager, dc, &resolvedDc))
    {
        return FALSE;
    }

    MiniGdi::Point oldPosition;
    if (!manager->Gdi().MoveTo(resolvedDc, MiniGdi::Point{ x, y }, previous ? &oldPosition : nullptr))
    {
        BridgeSetLastError(ERROR_INVALID_HANDLE);
        return FALSE;
    }
    if (previous)
    {
        previous->x = oldPosition.x;
        previous->y = oldPosition.y;
    }

    BridgeSetLastError(ERROR_SUCCESS);
    return TRUE;
}

BOOL WINAPI Win32Bridge::Bridge::BridgeLineTo(HDC dc, int x, int y)
{
    GuestWindowManager* manager = CurrentManagerOrFail();
    MiniGdi::DcHandle resolvedDc = MiniGdi::InvalidDc;
    if (!manager || !HasValidDc(manager, dc, &resolvedDc))
    {
        return FALSE;
    }

    // A NULL_PEN or a completely clipped line performs no raster write, but
    // it is still a successful GDI state transition (the current point moves).
    manager->Gdi().LineTo(resolvedDc, MiniGdi::Point{ x, y });
    BridgeSetLastError(ERROR_SUCCESS);
    return TRUE;
}

BOOL WINAPI Win32Bridge::Bridge::BridgeSetPixelV(HDC dc, int x, int y, COLORREF color)
{
    GuestWindowManager* manager = CurrentManagerOrFail();
    MiniGdi::DcHandle resolvedDc = MiniGdi::InvalidDc;
    if (!manager || !HasValidDc(manager, dc, &resolvedDc))
    {
        return FALSE;
    }
    if (!manager->Gdi().SetPixelV(resolvedDc, MiniGdi::Point{ x, y }, ColorFromColorRef(color)))
    {
        BridgeSetLastError(ERROR_INVALID_HANDLE);
        return FALSE;
    }

    BridgeSetLastError(ERROR_SUCCESS);
    return TRUE;
}

COLORREF WINAPI Win32Bridge::Bridge::BridgeGetPixel(HDC dc, int x, int y)
{
    GuestWindowManager* manager = CurrentManagerOrFail();
    MiniGdi::DcHandle resolvedDc = MiniGdi::InvalidDc;
    if (!manager || !HasValidDc(manager, dc, &resolvedDc))
    {
        return GuestAbi::ColorInvalid;
    }

    MiniGdi::Color color;
    if (!manager->Gdi().GetPixel(resolvedDc, MiniGdi::Point{ x, y }, &color))
    {
        // GetPixel's only supported readback is a visible point in a guest
        // surface.  CLR_INVALID makes clip/bounds failure unambiguous to the
        // PE without exposing a host buffer address.
        BridgeSetLastError(manager->Gdi().GetSurface(resolvedDc)
            ? ERROR_INVALID_PARAMETER
            : ERROR_INVALID_HANDLE);
        return GuestAbi::ColorInvalid;
    }

    BridgeSetLastError(ERROR_SUCCESS);
    return ColorRefFromColor(color);
}

int WINAPI Win32Bridge::Bridge::BridgeGetDeviceCaps(HDC dc, int index)
{
    GuestWindowManager* manager = CurrentManagerOrFail();
    MiniGdi::DcHandle resolvedDc = MiniGdi::InvalidDc;
    if (!manager || !HasValidDc(manager, dc, &resolvedDc))
    {
        return 0;
    }

    const MiniGdi::Surface* surface = manager->Gdi().GetSurface(resolvedDc);
    const int width = surface ? surface->Width() : 0;
    const int height = surface ? surface->Height() : 0;

    // Report a stable 32-bpp software raster. These are deliberately device
    // caps for the virtual HDC, not the Xbox display hardware.
    int result = 0;
    switch (index)
    {
    case 2:   // TECHNOLOGY (DT_RASDISPLAY)
        result = 1;
        break;
    case 8:   // HORZRES
    case 110: // PHYSICALWIDTH
        result = width;
        break;
    case 10:  // VERTRES
    case 111: // PHYSICALHEIGHT
        result = height;
        break;
    case 12:  // BITSPIXEL
        result = 32;
        break;
    case 14:  // PLANES
        result = 1;
        break;
    case 38:  // RASTERCAPS (RC_BITBLT | RC_DI_BITMAP)
        result = 0x0001 | 0x0080;
        break;
    case 88:  // LOGPIXELSX
    case 90:  // LOGPIXELSY
        result = 96;
        break;
    case 116: // VREFRESH
        result = 60;
        break;
    default:
        // Unknown capabilities follow the usual GDI convention of returning
        // zero; the known HDC itself remains valid.
        break;
    }

    BridgeSetLastError(ERROR_SUCCESS);
    return result;
}

BOOL WINAPI Win32Bridge::Bridge::BridgeTextOutW(HDC dc, int x, int y, LPCWSTR text, int characterCount)
{
    if (characterCount < 0 || (characterCount != 0 && text == nullptr))
    {
        BridgeSetLastError(ERROR_INVALID_PARAMETER);
        return FALSE;
    }

    GuestWindowManager* manager = CurrentManagerOrFail();
    MiniGdi::DcHandle resolvedDc = MiniGdi::InvalidDc;
    if (!manager || !HasValidDc(manager, dc, &resolvedDc))
    {
        return FALSE;
    }

    if (!manager->Gdi().TextOutW(
        resolvedDc,
        MiniGdi::Point{ x, y },
        text,
        static_cast<std::size_t>(characterCount)))
    {
        BridgeSetLastError(ERROR_INVALID_PARAMETER);
        return FALSE;
    }

    BridgeSetLastError(ERROR_SUCCESS);
    return TRUE;
}

BOOL WINAPI Win32Bridge::Bridge::BridgeGetTextExtentPoint32W(
    HDC dc,
    LPCWSTR text,
    int characterCount,
    LPSIZE extent)
{
    if (!extent || characterCount < 0 || (characterCount != 0 && text == nullptr))
    {
        BridgeSetLastError(ERROR_INVALID_PARAMETER);
        return FALSE;
    }

    GuestWindowManager* manager = CurrentManagerOrFail();
    MiniGdi::DcHandle resolvedDc = MiniGdi::InvalidDc;
    if (!manager || !HasValidDc(manager, dc, &resolvedDc))
    {
        return FALSE;
    }

    MiniGdi::Size measured;
    if (!manager->Gdi().GetTextExtentW(resolvedDc, text,
        static_cast<std::size_t>(characterCount), &measured))
    {
        BridgeSetLastError(ERROR_INVALID_PARAMETER);
        return FALSE;
    }

    extent->cx = measured.width;
    extent->cy = measured.height;
    BridgeSetLastError(ERROR_SUCCESS);
    return TRUE;
}

HDC WINAPI Win32Bridge::Bridge::BridgeCreateCompatibleDC(HDC dc)
{
    GuestWindowManager* manager = CurrentManagerOrFail();
    if (!manager)
    {
        return nullptr;
    }

    if (dc != nullptr && !manager->IsGuestDc(dc))
    {
        BridgeSetLastError(ERROR_INVALID_HANDLE);
        return nullptr;
    }

    const MiniGdi::DcHandle memoryDc = manager->Gdi().CreateMemoryDc();
    if (memoryDc == MiniGdi::InvalidDc)
    {
        BridgeSetLastError(ERROR_NOT_ENOUGH_MEMORY);
        return nullptr;
    }

    BridgeSetLastError(ERROR_SUCCESS);
    return ToGuestDc(memoryDc);
}

BOOL WINAPI Win32Bridge::Bridge::BridgeDeleteDC(HDC dc)
{
    GuestWindowManager* manager = CurrentManagerOrFail();
    MiniGdi::DcHandle resolvedDc = MiniGdi::InvalidDc;
    if (!manager || !HasValidDc(manager, dc, &resolvedDc))
    {
        return FALSE;
    }
    if (!manager->Gdi().IsMemoryDc(resolvedDc))
    {
        BridgeSetLastError(ERROR_INVALID_HANDLE);
        return FALSE;
    }
    if (!manager->Gdi().DestroyDc(resolvedDc))
    {
        BridgeSetLastError(ERROR_INVALID_HANDLE);
        return FALSE;
    }

    BridgeSetLastError(ERROR_SUCCESS);
    return TRUE;
}

HBITMAP WINAPI Win32Bridge::Bridge::BridgeCreateCompatibleBitmap(HDC dc, int width, int height)
{
    GuestWindowManager* manager = CurrentManagerOrFail();
    MiniGdi::DcHandle resolvedDc = MiniGdi::InvalidDc;
    if (!manager || !HasValidDc(manager, dc, &resolvedDc))
    {
        return nullptr;
    }
    // Bound a single guest bitmap to the same dimensions used for virtual
    // top-level windows, keeping a hostile guest from exhausting app memory.
    if (width <= 0 || height <= 0 || width > 2048 || height > 2048)
    {
        BridgeSetLastError(width <= 0 || height <= 0 ? ERROR_INVALID_PARAMETER : ERROR_NOT_ENOUGH_MEMORY);
        return nullptr;
    }

    const MiniGdi::BitmapHandle bitmap = manager->Gdi().CreateBitmap(width, height, MiniGdi::Transparent);
    if (bitmap == MiniGdi::InvalidObject)
    {
        BridgeSetLastError(ERROR_NOT_ENOUGH_MEMORY);
        return nullptr;
    }

    BridgeSetLastError(ERROR_SUCCESS);
    return reinterpret_cast<HBITMAP>(ToGuestObject(bitmap));
}

BOOL WINAPI Win32Bridge::Bridge::BridgeBitBlt(
    HDC destination,
    int x,
    int y,
    int width,
    int height,
    HDC source,
    int sourceX,
    int sourceY,
    DWORD rasterOperation)
{
    GuestWindowManager* manager = CurrentManagerOrFail();
    MiniGdi::DcHandle destinationDc = MiniGdi::InvalidDc;
    MiniGdi::DcHandle sourceDc = MiniGdi::InvalidDc;
    if (!manager || !HasValidDc(manager, destination, &destinationDc) ||
        !HasValidDc(manager, source, &sourceDc))
    {
        return FALSE;
    }
    if (rasterOperation != GuestAbi::RasterOpSrcCopy)
    {
        BridgeSetLastError(ERROR_NOT_SUPPORTED);
        return FALSE;
    }
    if (width < 0 || height < 0)
    {
        BridgeSetLastError(ERROR_INVALID_PARAMETER);
        return FALSE;
    }
    if (width == 0 || height == 0)
    {
        BridgeSetLastError(ERROR_SUCCESS);
        return TRUE;
    }
    if (!manager->Gdi().GetSurface(destinationDc) || !manager->Gdi().GetSurface(sourceDc))
    {
        BridgeSetLastError(ERROR_INVALID_HANDLE);
        return FALSE;
    }

    // Fully clipped copies are still valid BitBlt calls.  With valid DCs and
    // SRCCOPY, MiniGdi's false result therefore remains a benign no-op.
    manager->Gdi().BitBlt(
        destinationDc,
        MiniGdi::Point{ x, y },
        sourceDc,
        MiniGdi::Point{ sourceX, sourceY },
        MiniGdi::Size{ width, height });
    BridgeSetLastError(ERROR_SUCCESS);
    return TRUE;
}

#include "Bridge/Gdi32CompatibilityShims.inl"

ImportResolution Win32Bridge::Bridge::ResolveGdi32Import(const ImportedSymbol& symbol)
{
    ImportResolution resolution = CompatibilityCatalog::Resolve(symbol);
    if (symbol.importedByOrdinal || !IsGdiLibrary(symbol.library))
    {
        return resolution;
    }

    if (_wcsicmp(symbol.name.c_str(), L"createsolidbrush") == 0)
        resolution.targetAddress = reinterpret_cast<ULONGLONG>(&BridgeCreateSolidBrush);
    else if (_wcsicmp(symbol.name.c_str(), L"createpen") == 0)
        resolution.targetAddress = reinterpret_cast<ULONGLONG>(&BridgeCreatePen);
    else if (_wcsicmp(symbol.name.c_str(), L"createfontw") == 0)
        resolution.targetAddress = reinterpret_cast<ULONGLONG>(&BridgeCreateFontW);
    else if (_wcsicmp(symbol.name.c_str(), L"createfontindirectw") == 0)
        resolution.targetAddress = reinterpret_cast<ULONGLONG>(&BridgeCreateFontIndirectW);
    else if (_wcsicmp(symbol.name.c_str(), L"gettextmetricsw") == 0)
        resolution.targetAddress = reinterpret_cast<ULONGLONG>(&BridgeGetTextMetricsW);
    else if (_wcsicmp(symbol.name.c_str(), L"gettextfacew") == 0)
        resolution.targetAddress = reinterpret_cast<ULONGLONG>(&BridgeGetTextFaceW);
    else if (_wcsicmp(symbol.name.c_str(), L"getobjectw") == 0)
        resolution.targetAddress = reinterpret_cast<ULONGLONG>(&BridgeGetObjectW);
    else if (_wcsicmp(symbol.name.c_str(), L"getstockobject") == 0)
        resolution.targetAddress = reinterpret_cast<ULONGLONG>(&BridgeGetStockObject);
    else if (_wcsicmp(symbol.name.c_str(), L"deleteobject") == 0)
        resolution.targetAddress = reinterpret_cast<ULONGLONG>(&BridgeDeleteObject);
    else if (_wcsicmp(symbol.name.c_str(), L"selectobject") == 0)
        resolution.targetAddress = reinterpret_cast<ULONGLONG>(&BridgeSelectObject);
    else if (_wcsicmp(symbol.name.c_str(), L"rectangle") == 0)
        resolution.targetAddress = reinterpret_cast<ULONGLONG>(&BridgeRectangle);
    else if (_wcsicmp(symbol.name.c_str(), L"ellipse") == 0)
        resolution.targetAddress = reinterpret_cast<ULONGLONG>(&BridgeEllipse);
    else if (_wcsicmp(symbol.name.c_str(), L"movetoex") == 0)
        resolution.targetAddress = reinterpret_cast<ULONGLONG>(&BridgeMoveToEx);
    else if (_wcsicmp(symbol.name.c_str(), L"lineto") == 0)
        resolution.targetAddress = reinterpret_cast<ULONGLONG>(&BridgeLineTo);
    else if (_wcsicmp(symbol.name.c_str(), L"setpixelv") == 0)
        resolution.targetAddress = reinterpret_cast<ULONGLONG>(&BridgeSetPixelV);
    else if (_wcsicmp(symbol.name.c_str(), L"getpixel") == 0)
        resolution.targetAddress = reinterpret_cast<ULONGLONG>(&BridgeGetPixel);
    else if (_wcsicmp(symbol.name.c_str(), L"getdevicecaps") == 0)
        resolution.targetAddress = reinterpret_cast<ULONGLONG>(&BridgeGetDeviceCaps);
    else if (_wcsicmp(symbol.name.c_str(), L"settextcolor") == 0)
        resolution.targetAddress = reinterpret_cast<ULONGLONG>(&BridgeSetTextColor);
    else if (_wcsicmp(symbol.name.c_str(), L"setbkcolor") == 0)
        resolution.targetAddress = reinterpret_cast<ULONGLONG>(&BridgeSetBkColor);
    else if (_wcsicmp(symbol.name.c_str(), L"setbkmode") == 0)
        resolution.targetAddress = reinterpret_cast<ULONGLONG>(&BridgeSetBkMode);
    else if (_wcsicmp(symbol.name.c_str(), L"textoutw") == 0)
        resolution.targetAddress = reinterpret_cast<ULONGLONG>(&BridgeTextOutW);
    else if (_wcsicmp(symbol.name.c_str(), L"gettextextentpoint32w") == 0)
        resolution.targetAddress = reinterpret_cast<ULONGLONG>(&BridgeGetTextExtentPoint32W);
    else if (_wcsicmp(symbol.name.c_str(), L"createcompatibledc") == 0)
        resolution.targetAddress = reinterpret_cast<ULONGLONG>(&BridgeCreateCompatibleDC);
    else if (_wcsicmp(symbol.name.c_str(), L"deletedc") == 0)
        resolution.targetAddress = reinterpret_cast<ULONGLONG>(&BridgeDeleteDC);
    else if (_wcsicmp(symbol.name.c_str(), L"createcompatiblebitmap") == 0)
        resolution.targetAddress = reinterpret_cast<ULONGLONG>(&BridgeCreateCompatibleBitmap);
    else if (_wcsicmp(symbol.name.c_str(), L"bitblt") == 0)
        resolution.targetAddress = reinterpret_cast<ULONGLONG>(&BridgeBitBlt);
    else if (_wcsicmp(symbol.name.c_str(), L"startpage") == 0) resolution.targetAddress = reinterpret_cast<ULONGLONG>(&BridgeStartPage);
    else if (_wcsicmp(symbol.name.c_str(), L"startdocw") == 0) resolution.targetAddress = reinterpret_cast<ULONGLONG>(&BridgeStartDocW);
    else if (_wcsicmp(symbol.name.c_str(), L"setabortproc") == 0) resolution.targetAddress = reinterpret_cast<ULONGLONG>(&BridgeSetAbortProc);
    else if (_wcsicmp(symbol.name.c_str(), L"enddoc") == 0) resolution.targetAddress = reinterpret_cast<ULONGLONG>(&BridgeEndDoc);
    else if (_wcsicmp(symbol.name.c_str(), L"abortdoc") == 0) resolution.targetAddress = reinterpret_cast<ULONGLONG>(&BridgeAbortDoc);
    else if (_wcsicmp(symbol.name.c_str(), L"endpage") == 0) resolution.targetAddress = reinterpret_cast<ULONGLONG>(&BridgeEndPage);
    else if (_wcsicmp(symbol.name.c_str(), L"lptodp") == 0) resolution.targetAddress = reinterpret_cast<ULONGLONG>(&BridgeLPtoDP);
    else if (_wcsicmp(symbol.name.c_str(), L"setwindowextex") == 0) resolution.targetAddress = reinterpret_cast<ULONGLONG>(&BridgeSetWindowExtEx);
    else if (_wcsicmp(symbol.name.c_str(), L"setviewportextex") == 0) resolution.targetAddress = reinterpret_cast<ULONGLONG>(&BridgeSetViewportExtEx);
    else if (_wcsicmp(symbol.name.c_str(), L"setmapmode") == 0) resolution.targetAddress = reinterpret_cast<ULONGLONG>(&BridgeSetMapMode);
    else if (_wcsicmp(symbol.name.c_str(), L"enumfontsw") == 0) resolution.targetAddress = reinterpret_cast<ULONGLONG>(&BridgeEnumFontsW);
    else if (_wcsicmp(symbol.name.c_str(), L"createdcw") == 0) resolution.targetAddress = reinterpret_cast<ULONGLONG>(&BridgeCreateDCW);
    else if (_wcsicmp(symbol.name.c_str(), L"setwindoworgex") == 0) resolution.targetAddress = reinterpret_cast<ULONGLONG>(&BridgeSetWindowOrgEx);
    else if (_wcsicmp(symbol.name.c_str(), L"offsetwindoworgex") == 0) resolution.targetAddress = reinterpret_cast<ULONGLONG>(&BridgeOffsetWindowOrgEx);
    else if (_wcsicmp(symbol.name.c_str(), L"dptolp") == 0) resolution.targetAddress = reinterpret_cast<ULONGLONG>(&BridgeDPtoLP);
    else if (_wcsicmp(symbol.name.c_str(), L"setbrushorgex") == 0) resolution.targetAddress = reinterpret_cast<ULONGLONG>(&BridgeSetBrushOrgEx);
    else if (_wcsicmp(symbol.name.c_str(), L"setstretchbltmode") == 0) resolution.targetAddress = reinterpret_cast<ULONGLONG>(&BridgeSetStretchBltMode);
    else if (_wcsicmp(symbol.name.c_str(), L"setrop2") == 0) resolution.targetAddress = reinterpret_cast<ULONGLONG>(&BridgeSetROP2);
    else if (_wcsicmp(symbol.name.c_str(), L"getrop2") == 0) resolution.targetAddress = reinterpret_cast<ULONGLONG>(&BridgeGetROP2);
    else if (_wcsicmp(symbol.name.c_str(), L"settextalign") == 0) resolution.targetAddress = reinterpret_cast<ULONGLONG>(&BridgeSetTextAlign);
    else if (_wcsicmp(symbol.name.c_str(), L"createbitmap") == 0) resolution.targetAddress = reinterpret_cast<ULONGLONG>(&BridgeCreateBitmap);
    else if (_wcsicmp(symbol.name.c_str(), L"createpatternbrush") == 0) resolution.targetAddress = reinterpret_cast<ULONGLONG>(&BridgeCreatePatternBrush);
    else if (_wcsicmp(symbol.name.c_str(), L"patblt") == 0) resolution.targetAddress = reinterpret_cast<ULONGLONG>(&BridgePatBlt);
    else if (_wcsicmp(symbol.name.c_str(), L"createrectrgn") == 0) resolution.targetAddress = reinterpret_cast<ULONGLONG>(&BridgeCreateRectRgn);
    else if (_wcsicmp(symbol.name.c_str(), L"createrectrgnindirect") == 0) resolution.targetAddress = reinterpret_cast<ULONGLONG>(&BridgeCreateRectRgnIndirect);
    else if (_wcsicmp(symbol.name.c_str(), L"selectcliprgn") == 0) resolution.targetAddress = reinterpret_cast<ULONGLONG>(&BridgeSelectClipRgn);
    else if (_wcsicmp(symbol.name.c_str(), L"intersectcliprect") == 0) resolution.targetAddress = reinterpret_cast<ULONGLONG>(&BridgeIntersectClipRect);
    else if (_wcsicmp(symbol.name.c_str(), L"excludecliprect") == 0) resolution.targetAddress = reinterpret_cast<ULONGLONG>(&BridgeExcludeClipRect);
    else if (_wcsicmp(symbol.name.c_str(), L"getcliprgn") == 0) resolution.targetAddress = reinterpret_cast<ULONGLONG>(&BridgeGetClipRgn);
    else if (_wcsicmp(symbol.name.c_str(), L"combinergn") == 0) resolution.targetAddress = reinterpret_cast<ULONGLONG>(&BridgeCombineRgn);
    else if (_wcsicmp(symbol.name.c_str(), L"savedc") == 0) resolution.targetAddress = reinterpret_cast<ULONGLONG>(&BridgeSaveDC);
    else if (_wcsicmp(symbol.name.c_str(), L"restoredc") == 0) resolution.targetAddress = reinterpret_cast<ULONGLONG>(&BridgeRestoreDC);
    else if (_wcsicmp(symbol.name.c_str(), L"rectvisible") == 0) resolution.targetAddress = reinterpret_cast<ULONGLONG>(&BridgeRectVisible);
    else if (_wcsicmp(symbol.name.c_str(), L"polyline") == 0) resolution.targetAddress = reinterpret_cast<ULONGLONG>(&BridgePolyline);
    else if (_wcsicmp(symbol.name.c_str(), L"polygon") == 0) resolution.targetAddress = reinterpret_cast<ULONGLONG>(&BridgePolygon);
    else if (_wcsicmp(symbol.name.c_str(), L"roundrect") == 0) resolution.targetAddress = reinterpret_cast<ULONGLONG>(&BridgeRoundRect);
    else if (_wcsicmp(symbol.name.c_str(), L"createhatchbrush") == 0) resolution.targetAddress = reinterpret_cast<ULONGLONG>(&BridgeCreateHatchBrush);
    else if (_wcsicmp(symbol.name.c_str(), L"extcreatepen") == 0) resolution.targetAddress = reinterpret_cast<ULONGLONG>(&BridgeExtCreatePen);
    else if (_wcsicmp(symbol.name.c_str(), L"exttextoutw") == 0) resolution.targetAddress = reinterpret_cast<ULONGLONG>(&BridgeExtTextOutW);
    else if (_wcsicmp(symbol.name.c_str(), L"exttextouta") == 0) resolution.targetAddress = reinterpret_cast<ULONGLONG>(&BridgeExtTextOutA);
    else if (_wcsicmp(symbol.name.c_str(), L"gettextextentpointw") == 0) resolution.targetAddress = reinterpret_cast<ULONGLONG>(&BridgeGetTextExtentPointW);
    else if (_wcsicmp(symbol.name.c_str(), L"gettextextentpoint32a") == 0) resolution.targetAddress = reinterpret_cast<ULONGLONG>(&BridgeGetTextExtentPoint32A);
    else if (_wcsicmp(symbol.name.c_str(), L"gettextextentexpointw") == 0) resolution.targetAddress = reinterpret_cast<ULONGLONG>(&BridgeGetTextExtentExPointW);
    else if (_wcsicmp(symbol.name.c_str(), L"gettextextentexpointa") == 0) resolution.targetAddress = reinterpret_cast<ULONGLONG>(&BridgeGetTextExtentExPointA);
    else if (_wcsicmp(symbol.name.c_str(), L"stretchblt") == 0) resolution.targetAddress = reinterpret_cast<ULONGLONG>(&BridgeStretchBlt);
    else if (_wcsicmp(symbol.name.c_str(), L"gdialphablend") == 0) resolution.targetAddress = reinterpret_cast<ULONGLONG>(&BridgeGdiAlphaBlend);
    else if (_wcsicmp(symbol.name.c_str(), L"alphablend") == 0) resolution.targetAddress = reinterpret_cast<ULONGLONG>(&BridgeGdiAlphaBlend);
    else if (_wcsicmp(symbol.name.c_str(), L"enumfontfamiliesexw") == 0) resolution.targetAddress = reinterpret_cast<ULONGLONG>(&BridgeEnumFontFamiliesExW);
    else if (_wcsicmp(symbol.name.c_str(), L"createdibsection") == 0) resolution.targetAddress = reinterpret_cast<ULONGLONG>(&BridgeCreateDIBSection);
    else if (_wcsicmp(symbol.name.c_str(), L"setdibits") == 0) resolution.targetAddress = reinterpret_cast<ULONGLONG>(&BridgeSetDIBits);
    else if (_wcsicmp(symbol.name.c_str(), L"getdibits") == 0) resolution.targetAddress = reinterpret_cast<ULONGLONG>(&BridgeGetDIBits);
    else if (_wcsicmp(symbol.name.c_str(), L"stretchdibits") == 0) resolution.targetAddress = reinterpret_cast<ULONGLONG>(&BridgeStretchDIBits);

    if (resolution.targetAddress != 0)
    {
        resolution.disposition = ImportDisposition::NeedsBridge;
        resolution.note = L"MiniGDI adapter: software pens, brushes, retained font handles and queries (GetObjectW for HFONT), primitive raster drawing, text, memory DCs, bitmaps, and SRCCOPY blits.";
    }
    return resolution;
}
