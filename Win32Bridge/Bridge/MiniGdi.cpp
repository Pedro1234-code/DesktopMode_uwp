#include "Bridge\\MiniGdi.h"
#include "Bridge/DirectWriteText.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <cstdlib>
#include <limits>
#include <new>

namespace Win32Bridge
{
namespace Bridge
{
namespace MiniGdi
{
namespace
{
    // Hand-authored 5x7 source glyphs.  Each low five bits is one row, from
    // left to right (bit 4 through bit 0). RasterGlyph expands the rows into
    // the selected font cell while retaining a small inter-character margin.
    struct GlyphRows
    {
        std::uint8_t rows[7];
    };

    int SaturateToInt(std::int64_t value);

    // Printable ASCII from U+0020 (space) through U+005F (underscore).
    const GlyphRows kAsciiGlyphs[] =
    {
        { { 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00 } }, // space
        { { 0x04, 0x04, 0x04, 0x04, 0x04, 0x00, 0x04 } }, // !
        { { 0x0a, 0x0a, 0x0a, 0x00, 0x00, 0x00, 0x00 } }, // "
        { { 0x0a, 0x1f, 0x0a, 0x0a, 0x1f, 0x0a, 0x00 } }, // #
        { { 0x04, 0x0f, 0x14, 0x0e, 0x05, 0x1e, 0x04 } }, // $
        { { 0x19, 0x1a, 0x04, 0x08, 0x16, 0x13, 0x00 } }, // %
        { { 0x0c, 0x12, 0x14, 0x08, 0x15, 0x12, 0x0d } }, // &
        { { 0x04, 0x04, 0x08, 0x00, 0x00, 0x00, 0x00 } }, // '
        { { 0x02, 0x04, 0x08, 0x08, 0x08, 0x04, 0x02 } }, // (
        { { 0x08, 0x04, 0x02, 0x02, 0x02, 0x04, 0x08 } }, // )
        { { 0x00, 0x15, 0x0e, 0x1f, 0x0e, 0x15, 0x00 } }, // *
        { { 0x00, 0x04, 0x04, 0x1f, 0x04, 0x04, 0x00 } }, // +
        { { 0x00, 0x00, 0x00, 0x00, 0x04, 0x04, 0x08 } }, // ,
        { { 0x00, 0x00, 0x00, 0x1f, 0x00, 0x00, 0x00 } }, // -
        { { 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x04 } }, // .
        { { 0x01, 0x02, 0x04, 0x08, 0x10, 0x00, 0x00 } }, // /
        { { 0x0e, 0x11, 0x13, 0x15, 0x19, 0x11, 0x0e } }, // 0
        { { 0x04, 0x0c, 0x04, 0x04, 0x04, 0x04, 0x0e } }, // 1
        { { 0x0e, 0x11, 0x01, 0x02, 0x04, 0x08, 0x1f } }, // 2
        { { 0x1e, 0x01, 0x01, 0x0e, 0x01, 0x01, 0x1e } }, // 3
        { { 0x02, 0x06, 0x0a, 0x12, 0x1f, 0x02, 0x02 } }, // 4
        { { 0x1f, 0x10, 0x10, 0x1e, 0x01, 0x01, 0x1e } }, // 5
        { { 0x0e, 0x10, 0x10, 0x1e, 0x11, 0x11, 0x0e } }, // 6
        { { 0x1f, 0x01, 0x02, 0x04, 0x08, 0x08, 0x08 } }, // 7
        { { 0x0e, 0x11, 0x11, 0x0e, 0x11, 0x11, 0x0e } }, // 8
        { { 0x0e, 0x11, 0x11, 0x0f, 0x01, 0x01, 0x0e } }, // 9
        { { 0x00, 0x04, 0x00, 0x00, 0x04, 0x00, 0x00 } }, // :
        { { 0x00, 0x04, 0x00, 0x00, 0x04, 0x04, 0x08 } }, // ;
        { { 0x02, 0x04, 0x08, 0x10, 0x08, 0x04, 0x02 } }, // <
        { { 0x00, 0x00, 0x1f, 0x00, 0x1f, 0x00, 0x00 } }, // =
        { { 0x08, 0x04, 0x02, 0x01, 0x02, 0x04, 0x08 } }, // >
        { { 0x0e, 0x11, 0x01, 0x02, 0x04, 0x00, 0x04 } }, // ?
        { { 0x0e, 0x11, 0x17, 0x15, 0x17, 0x10, 0x0f } }, // @
        { { 0x0e, 0x11, 0x11, 0x1f, 0x11, 0x11, 0x11 } }, // A
        { { 0x1e, 0x11, 0x11, 0x1e, 0x11, 0x11, 0x1e } }, // B
        { { 0x0e, 0x11, 0x10, 0x10, 0x10, 0x11, 0x0e } }, // C
        { { 0x1e, 0x11, 0x11, 0x11, 0x11, 0x11, 0x1e } }, // D
        { { 0x1f, 0x10, 0x10, 0x1e, 0x10, 0x10, 0x1f } }, // E
        { { 0x1f, 0x10, 0x10, 0x1e, 0x10, 0x10, 0x10 } }, // F
        { { 0x0e, 0x11, 0x10, 0x17, 0x11, 0x11, 0x0f } }, // G
        { { 0x11, 0x11, 0x11, 0x1f, 0x11, 0x11, 0x11 } }, // H
        { { 0x0e, 0x04, 0x04, 0x04, 0x04, 0x04, 0x0e } }, // I
        { { 0x01, 0x01, 0x01, 0x01, 0x11, 0x11, 0x0e } }, // J
        { { 0x11, 0x12, 0x14, 0x18, 0x14, 0x12, 0x11 } }, // K
        { { 0x10, 0x10, 0x10, 0x10, 0x10, 0x10, 0x1f } }, // L
        { { 0x11, 0x1b, 0x15, 0x15, 0x11, 0x11, 0x11 } }, // M
        { { 0x11, 0x19, 0x15, 0x13, 0x11, 0x11, 0x11 } }, // N
        { { 0x0e, 0x11, 0x11, 0x11, 0x11, 0x11, 0x0e } }, // O
        { { 0x1e, 0x11, 0x11, 0x1e, 0x10, 0x10, 0x10 } }, // P
        { { 0x0e, 0x11, 0x11, 0x11, 0x15, 0x12, 0x0d } }, // Q
        { { 0x1e, 0x11, 0x11, 0x1e, 0x14, 0x12, 0x11 } }, // R
        { { 0x0f, 0x10, 0x10, 0x0e, 0x01, 0x01, 0x1e } }, // S
        { { 0x1f, 0x04, 0x04, 0x04, 0x04, 0x04, 0x04 } }, // T
        { { 0x11, 0x11, 0x11, 0x11, 0x11, 0x11, 0x0e } }, // U
        { { 0x11, 0x11, 0x11, 0x11, 0x11, 0x0a, 0x04 } }, // V
        { { 0x11, 0x11, 0x11, 0x15, 0x15, 0x1b, 0x11 } }, // W
        { { 0x11, 0x11, 0x0a, 0x04, 0x0a, 0x11, 0x11 } }, // X
        { { 0x11, 0x11, 0x0a, 0x04, 0x04, 0x04, 0x04 } }, // Y
        { { 0x1f, 0x01, 0x02, 0x04, 0x08, 0x10, 0x1f } }, // Z
        { { 0x0e, 0x08, 0x08, 0x08, 0x08, 0x08, 0x0e } }, // [
        { { 0x10, 0x08, 0x04, 0x02, 0x01, 0x00, 0x00 } }, // reverse solidus
        { { 0x0e, 0x02, 0x02, 0x02, 0x02, 0x02, 0x0e } }, // ]
        { { 0x04, 0x0a, 0x11, 0x00, 0x00, 0x00, 0x00 } }, // ^
        { { 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x1f } }  // _
    };
    static_assert(
        sizeof(kAsciiGlyphs) / sizeof(kAsciiGlyphs[0]) == L'_' - L' ' + 1,
        "ASCII fallback glyph table must cover U+0020 through U+005F");

    const GlyphRows kGrave = { { 0x08, 0x04, 0x02, 0x00, 0x00, 0x00, 0x00 } };
    const GlyphRows kLeftBrace = { { 0x03, 0x04, 0x04, 0x08, 0x04, 0x04, 0x03 } };
    const GlyphRows kVerticalBar = { { 0x04, 0x04, 0x04, 0x04, 0x04, 0x04, 0x04 } };
    const GlyphRows kRightBrace = { { 0x18, 0x04, 0x04, 0x02, 0x04, 0x04, 0x18 } };
    const GlyphRows kTilde = { { 0x00, 0x00, 0x09, 0x16, 0x00, 0x00, 0x00 } };

    const GlyphRows kLowercaseGlyphs[] =
    {
        { { 0x00, 0x00, 0x0e, 0x01, 0x0f, 0x11, 0x0f } }, // a
        { { 0x10, 0x10, 0x1e, 0x11, 0x11, 0x11, 0x1e } }, // b
        { { 0x00, 0x00, 0x0e, 0x11, 0x10, 0x11, 0x0e } }, // c
        { { 0x01, 0x01, 0x0f, 0x11, 0x11, 0x11, 0x0f } }, // d
        { { 0x00, 0x00, 0x0e, 0x11, 0x1f, 0x10, 0x0e } }, // e
        { { 0x06, 0x09, 0x08, 0x1c, 0x08, 0x08, 0x08 } }, // f
        { { 0x00, 0x00, 0x0f, 0x11, 0x0f, 0x01, 0x0e } }, // g
        { { 0x10, 0x10, 0x1e, 0x11, 0x11, 0x11, 0x11 } }, // h
        { { 0x04, 0x00, 0x0c, 0x04, 0x04, 0x04, 0x0e } }, // i
        { { 0x02, 0x00, 0x06, 0x02, 0x02, 0x12, 0x0c } }, // j
        { { 0x10, 0x10, 0x12, 0x14, 0x18, 0x14, 0x12 } }, // k
        { { 0x0c, 0x04, 0x04, 0x04, 0x04, 0x04, 0x0e } }, // l
        { { 0x00, 0x00, 0x1a, 0x15, 0x15, 0x11, 0x11 } }, // m
        { { 0x00, 0x00, 0x1e, 0x11, 0x11, 0x11, 0x11 } }, // n
        { { 0x00, 0x00, 0x0e, 0x11, 0x11, 0x11, 0x0e } }, // o
        { { 0x00, 0x00, 0x1e, 0x11, 0x11, 0x1e, 0x10 } }, // p
        { { 0x00, 0x00, 0x0f, 0x11, 0x11, 0x0f, 0x01 } }, // q
        { { 0x00, 0x00, 0x16, 0x19, 0x10, 0x10, 0x10 } }, // r
        { { 0x00, 0x00, 0x0f, 0x10, 0x0e, 0x01, 0x1e } }, // s
        { { 0x08, 0x08, 0x1c, 0x08, 0x08, 0x09, 0x06 } }, // t
        { { 0x00, 0x00, 0x11, 0x11, 0x11, 0x13, 0x0d } }, // u
        { { 0x00, 0x00, 0x11, 0x11, 0x11, 0x0a, 0x04 } }, // v
        { { 0x00, 0x00, 0x11, 0x11, 0x15, 0x15, 0x0a } }, // w
        { { 0x00, 0x00, 0x11, 0x0a, 0x04, 0x0a, 0x11 } }, // x
        { { 0x00, 0x00, 0x11, 0x11, 0x0f, 0x01, 0x0e } }, // y
        { { 0x00, 0x00, 0x1f, 0x02, 0x04, 0x08, 0x1f } }  // z
    };
    static_assert(
        sizeof(kLowercaseGlyphs) / sizeof(kLowercaseGlyphs[0]) == L'z' - L'a' + 1,
        "lowercase fallback glyph table must cover ASCII a-z");

    const GlyphRows& GlyphForCharacter(wchar_t character)
    {
        // The compact fallback has no combining-mark rasterizer. Preserve
        // legibility for the Latin scripts commonly found in localized Win32
        // resources by mapping precomposed letters to their base glyph.
        switch (character)
        {
        case 0x00c0: case 0x00c1: case 0x00c2: case 0x00c3: case 0x00c4: case 0x00c5: character = L'A'; break;
        case 0x00c6: character = L'A'; break;
        case 0x00c7: character = L'C'; break;
        case 0x00c8: case 0x00c9: case 0x00ca: case 0x00cb: character = L'E'; break;
        case 0x00cc: case 0x00cd: case 0x00ce: case 0x00cf: character = L'I'; break;
        case 0x00d1: character = L'N'; break;
        case 0x00d2: case 0x00d3: case 0x00d4: case 0x00d5: case 0x00d6: case 0x00d8: character = L'O'; break;
        case 0x00d9: case 0x00da: case 0x00db: case 0x00dc: character = L'U'; break;
        case 0x00dd: character = L'Y'; break;
        case 0x00e0: case 0x00e1: case 0x00e2: case 0x00e3: case 0x00e4: case 0x00e5: character = L'a'; break;
        case 0x00e6: character = L'a'; break;
        case 0x00e7: character = L'c'; break;
        case 0x00e8: case 0x00e9: case 0x00ea: case 0x00eb: character = L'e'; break;
        case 0x00ec: case 0x00ed: case 0x00ee: case 0x00ef: character = L'i'; break;
        case 0x00f1: character = L'n'; break;
        case 0x00f2: case 0x00f3: case 0x00f4: case 0x00f5: case 0x00f6: case 0x00f8: character = L'o'; break;
        case 0x00f9: case 0x00fa: case 0x00fb: case 0x00fc: character = L'u'; break;
        case 0x00fd: case 0x00ff: character = L'y'; break;
        case 0x2013: case 0x2014: character = L'-'; break;
        case 0x201c: case 0x201d: character = L'"'; break;
        case 0x2018: case 0x2019: character = L'\''; break;
        default: break;
        }

        if (character >= L'a' && character <= L'z')
        {
            return kLowercaseGlyphs[character - L'a'];
        }

        if (character >= L' ' && character <= L'_')
        {
            return kAsciiGlyphs[character - L' '];
        }

        switch (character)
        {
        case L'`': return kGrave;
        case L'{': return kLeftBrace;
        case L'|': return kVerticalBar;
        case L'}': return kRightBrace;
        case L'~': return kTilde;
        default: return kAsciiGlyphs[L'?' - L' '];
        }
    }

    Size TextExtentForCount(std::size_t characterCount, const Size& cell)
    {
        const std::size_t maximumWidth = static_cast<std::size_t>(std::numeric_limits<int>::max());
        const int width = characterCount > maximumWidth / static_cast<std::size_t>(cell.width)
            ? std::numeric_limits<int>::max()
            : static_cast<int>(characterCount * static_cast<std::size_t>(cell.width));
        return Size{ width, characterCount == 0 ? 0 : cell.height };
    }

    bool RasterGlyph(
        Surface& surface,
        int cellLeft,
        int cellTop,
        const GlyphRows& glyph,
        Color color,
        const Rect& clip,
        const Size& cell)
    {
        if (Alpha(color) == 0)
        {
            return false;
        }

        bool drawn = false;
        for (int row = 0; row != 7; ++row)
        {
            const std::uint8_t bits = glyph.rows[row];
            for (int column = 0; column != 5; ++column)
            {
                if ((bits & (1u << (4 - column))) == 0)
                {
                    continue;
                }

                const int inkLeft = (std::max)(1, cell.width / 8);
                const int inkTop = (std::max)(1, cell.height / 16);
                const int inkWidth = (std::max)(1, cell.width - 2 * inkLeft);
                const int inkHeight = (std::max)(1, cell.height - 2 * inkTop);
                const int left = SaturateToInt(static_cast<std::int64_t>(cellLeft) + inkLeft +
                    static_cast<std::int64_t>(column) * inkWidth / 5);
                const int top = SaturateToInt(static_cast<std::int64_t>(cellTop) + inkTop +
                    static_cast<std::int64_t>(row) * inkHeight / 7);
                const int right = SaturateToInt(static_cast<std::int64_t>(cellLeft) + inkLeft +
                    static_cast<std::int64_t>(column + 1) * inkWidth / 5);
                const int bottom = SaturateToInt(static_cast<std::int64_t>(cellTop) + inkTop +
                    static_cast<std::int64_t>(row + 1) * inkHeight / 7);
                drawn = FillRect(surface, Rect{ left, top, right, bottom }, color, &clip) || drawn;
            }
        }
        return drawn;
    }

    int SaturateToInt(std::int64_t value)
    {
        if (value > static_cast<std::int64_t>(std::numeric_limits<int>::max()))
        {
            return std::numeric_limits<int>::max();
        }

        if (value < static_cast<std::int64_t>(std::numeric_limits<int>::min()))
        {
            return std::numeric_limits<int>::min();
        }

        return static_cast<int>(value);
    }

    Rect SurfaceClip(const Surface& surface, const Rect* requestedClip)
    {
        Rect result = surface.Bounds();
        if (requestedClip != nullptr)
        {
            result = IntersectRect(result, *requestedClip);
        }

        return result;
    }

    Rect EffectiveRect(const Surface& surface, const Rect& rect, const Rect* clip)
    {
        return IntersectRect(NormalizeRect(rect), SurfaceClip(surface, clip));
    }

    void BlendPixel(Color* destination, Color source)
    {
        if (destination != nullptr)
        {
            *destination = BlendSourceOver(source, *destination);
        }
    }

    enum LineOutCode : unsigned int
    {
        LineInside = 0,
        LineLeft = 1,
        LineRight = 2,
        LineTop = 4,
        LineBottom = 8
    };

    unsigned int OutCode(double x, double y, const Rect& bounds)
    {
        unsigned int code = LineInside;
        const double right = static_cast<double>(bounds.right - 1);
        const double bottom = static_cast<double>(bounds.bottom - 1);

        if (x < bounds.left)
        {
            code |= LineLeft;
        }
        else if (x > right)
        {
            code |= LineRight;
        }

        if (y < bounds.top)
        {
            code |= LineTop;
        }
        else if (y > bottom)
        {
            code |= LineBottom;
        }

        return code;
    }

    // Cohen-Sutherland clipping keeps Bresenham bounded to the destination
    // surface even when a guest sends a line with deliberately huge points.
    bool ClipLineToRect(Point* from, Point* to, const Rect& bounds)
    {
        if (from == nullptr || to == nullptr || bounds.Empty())
        {
            return false;
        }

        double x0 = static_cast<double>(from->x);
        double y0 = static_cast<double>(from->y);
        double x1 = static_cast<double>(to->x);
        double y1 = static_cast<double>(to->y);

        for (int attempt = 0; attempt != 12; ++attempt)
        {
            const unsigned int code0 = OutCode(x0, y0, bounds);
            const unsigned int code1 = OutCode(x1, y1, bounds);

            if ((code0 | code1) == LineInside)
            {
                const int minimumX = bounds.left;
                const int maximumX = bounds.right - 1;
                const int minimumY = bounds.top;
                const int maximumY = bounds.bottom - 1;
                from->x = std::max(minimumX, std::min(maximumX, static_cast<int>(std::lround(x0))));
                from->y = std::max(minimumY, std::min(maximumY, static_cast<int>(std::lround(y0))));
                to->x = std::max(minimumX, std::min(maximumX, static_cast<int>(std::lround(x1))));
                to->y = std::max(minimumY, std::min(maximumY, static_cast<int>(std::lround(y1))));
                return true;
            }

            if ((code0 & code1) != LineInside)
            {
                return false;
            }

            const unsigned int outside = code0 != LineInside ? code0 : code1;
            double x = 0;
            double y = 0;

            if ((outside & LineTop) != 0)
            {
                if (y1 == y0)
                {
                    return false;
                }
                y = bounds.top;
                x = x0 + (x1 - x0) * (y - y0) / (y1 - y0);
            }
            else if ((outside & LineBottom) != 0)
            {
                if (y1 == y0)
                {
                    return false;
                }
                y = bounds.bottom - 1;
                x = x0 + (x1 - x0) * (y - y0) / (y1 - y0);
            }
            else if ((outside & LineRight) != 0)
            {
                if (x1 == x0)
                {
                    return false;
                }
                x = bounds.right - 1;
                y = y0 + (y1 - y0) * (x - x0) / (x1 - x0);
            }
            else
            {
                if (x1 == x0)
                {
                    return false;
                }
                x = bounds.left;
                y = y0 + (y1 - y0) * (x - x0) / (x1 - x0);
            }

            if (outside == code0)
            {
                x0 = x;
                y0 = y;
            }
            else
            {
                x1 = x;
                y1 = y;
            }
        }

        // A finite line against a finite axis-aligned rectangle should finish
        // in at most four intersections.  Treat anything else as malformed
        // rather than risk a non-terminating guest-triggered draw operation.
        return false;
    }
}

Size FontCellSize(const Font& font)
{
    constexpr std::int64_t minimumCell = 1;
    constexpr std::int64_t maximumCell = 512;
    const std::int64_t requestedHeight = font.height < 0
        ? -static_cast<std::int64_t>(font.height)
        : static_cast<std::int64_t>(font.height);
    const int height = static_cast<int>((std::max)(minimumCell,
        (std::min)(requestedHeight == 0 ? static_cast<std::int64_t>(DefaultTextGlyphHeight) : requestedHeight,
            maximumCell)));
    const std::int64_t requestedWidth = font.width < 0
        ? -static_cast<std::int64_t>(font.width)
        : static_cast<std::int64_t>(font.width);
    const int width = requestedWidth == 0
        ? (std::max)(1, (height * DefaultTextGlyphWidth + DefaultTextGlyphHeight / 2) /
            DefaultTextGlyphHeight)
        : static_cast<int>((std::max)(minimumCell, (std::min)(requestedWidth, maximumCell)));
    return Size{ width, height };
}

bool Rect::Empty() const
{
    return right <= left || bottom <= top;
}

Rect NormalizeRect(const Rect& rect)
{
    return Rect
    {
        std::min(rect.left, rect.right),
        std::min(rect.top, rect.bottom),
        std::max(rect.left, rect.right),
        std::max(rect.top, rect.bottom)
    };
}

Rect IntersectRect(const Rect& first, const Rect& second)
{
    const Rect normalizedFirst = NormalizeRect(first);
    const Rect normalizedSecond = NormalizeRect(second);
    const int left = std::max(normalizedFirst.left, normalizedSecond.left);
    const int top = std::max(normalizedFirst.top, normalizedSecond.top);
    const int right = std::min(normalizedFirst.right, normalizedSecond.right);
    const int bottom = std::min(normalizedFirst.bottom, normalizedSecond.bottom);
    if (right <= left || bottom <= top)
    {
        // Keep an empty intersection canonical.  A later NormalizeRect() must
        // never accidentally turn a disjoint clip into a drawable region.
        return Rect{ left, top, left, top };
    }

    return Rect{ left, top, right, bottom };
}

Color BlendSourceOver(Color source, Color destination)
{
    const std::uint32_t sourceAlpha = Alpha(source);
    if (sourceAlpha == 0)
    {
        return destination;
    }

    if (sourceAlpha == 0xff)
    {
        return source;
    }

    const std::uint32_t destinationAlpha = Alpha(destination);
    const std::uint32_t inverseSourceAlpha = 0xff - sourceAlpha;
    const std::uint32_t outputAlpha = sourceAlpha +
        (destinationAlpha * inverseSourceAlpha + 127) / 255;

    if (outputAlpha == 0)
    {
        return Transparent;
    }

    const auto blendChannel = [sourceAlpha, destinationAlpha, inverseSourceAlpha, outputAlpha](
        std::uint8_t sourceChannel,
        std::uint8_t destinationChannel) -> std::uint8_t
    {
        const std::uint32_t numerator =
            static_cast<std::uint32_t>(sourceChannel) * sourceAlpha * 255 +
            static_cast<std::uint32_t>(destinationChannel) * destinationAlpha * inverseSourceAlpha;
        const std::uint32_t denominator = outputAlpha * 255;
        return static_cast<std::uint8_t>((numerator + denominator / 2) / denominator);
    };

    return MakeColor(
        blendChannel(Red(source), Red(destination)),
        blendChannel(Green(source), Green(destination)),
        blendChannel(Blue(source), Blue(destination)),
        static_cast<std::uint8_t>(outputAlpha));
}

Surface::Surface(int width, int height, Color clearColor)
{
    Resize(width, height, clearColor);
}

bool Surface::Resize(int width, int height, Color clearColor)
{
    if (width < 0 || height < 0)
    {
        return false;
    }

    if (width == 0 || height == 0)
    {
        m_width = width;
        m_height = height;
        m_pixels.clear();
        return true;
    }

    const std::size_t pixelWidth = static_cast<std::size_t>(width);
    const std::size_t pixelHeight = static_cast<std::size_t>(height);
    if (pixelHeight > std::numeric_limits<std::size_t>::max() / pixelWidth)
    {
        return false;
    }

    try
    {
        std::vector<Color> pixels(pixelWidth * pixelHeight, clearColor);
        m_pixels.swap(pixels);
        m_width = width;
        m_height = height;
        return true;
    }
    catch (const std::bad_alloc&)
    {
        return false;
    }
}

void Surface::Clear(Color color)
{
    std::fill(m_pixels.begin(), m_pixels.end(), color);
}

Color* Surface::PixelAt(int x, int y)
{
    if (x < 0 || y < 0 || x >= m_width || y >= m_height)
    {
        return nullptr;
    }

    return &m_pixels[static_cast<std::size_t>(y) * m_width + x];
}

const Color* Surface::PixelAt(int x, int y) const
{
    if (x < 0 || y < 0 || x >= m_width || y >= m_height)
    {
        return nullptr;
    }

    return &m_pixels[static_cast<std::size_t>(y) * m_width + x];
}

bool FillRect(Surface& surface, const Rect& rect, Color color, const Rect* clip)
{
    const Rect output = EffectiveRect(surface, rect, clip);
    if (output.Empty() || Alpha(color) == 0)
    {
        return false;
    }

    Color* pixels = surface.Data();
    const std::size_t stride = static_cast<std::size_t>(surface.Width());
    if (pixels == nullptr || stride == 0)
    {
        return false;
    }

    for (int y = output.top; y < output.bottom; ++y)
    {
        Color* pixel = pixels + static_cast<std::size_t>(y) * stride + output.left;
        for (int x = output.left; x < output.right; ++x, ++pixel)
        {
            BlendPixel(pixel, color);
        }
    }

    return true;
}

bool StrokeRect(Surface& surface, const Rect& rect, Color color, int thickness, const Rect* clip)
{
    const Rect normalized = NormalizeRect(rect);
    if (normalized.Empty() || thickness <= 0 || Alpha(color) == 0)
    {
        return false;
    }

    const std::int64_t width = static_cast<std::int64_t>(normalized.right) - normalized.left;
    const std::int64_t height = static_cast<std::int64_t>(normalized.bottom) - normalized.top;
    const int horizontalThickness = static_cast<int>(std::min<std::int64_t>(thickness, height));
    const int verticalThickness = static_cast<int>(std::min<std::int64_t>(thickness, width));
    const int topInside = SaturateToInt(static_cast<std::int64_t>(normalized.top) + horizontalThickness);
    const int bottomInside = SaturateToInt(static_cast<std::int64_t>(normalized.bottom) - horizontalThickness);
    const int leftInside = SaturateToInt(static_cast<std::int64_t>(normalized.left) + verticalThickness);
    const int rightInside = SaturateToInt(static_cast<std::int64_t>(normalized.right) - verticalThickness);

    bool drawn = false;
    drawn = FillRect(surface, Rect{ normalized.left, normalized.top, normalized.right, topInside }, color, clip) || drawn;
    drawn = FillRect(surface, Rect{ normalized.left, bottomInside, normalized.right, normalized.bottom }, color, clip) || drawn;
    if (topInside < bottomInside)
    {
        drawn = FillRect(surface, Rect{ normalized.left, topInside, leftInside, bottomInside }, color, clip) || drawn;
        drawn = FillRect(surface, Rect{ rightInside, topInside, normalized.right, bottomInside }, color, clip) || drawn;
    }
    return drawn;
}

bool DrawLine(Surface& surface, Point from, Point to, Color color, int thickness, const Rect* clip)
{
    if (surface.Empty() || Alpha(color) == 0 || thickness <= 0)
    {
        return false;
    }

    const Rect outputClip = SurfaceClip(surface, clip);
    if (outputClip.Empty() || !ClipLineToRect(&from, &to, outputClip))
    {
        return false;
    }

    const int before = (thickness - 1) / 2;
    const int after = thickness / 2;
    const std::int64_t deltaX = std::llabs(static_cast<std::int64_t>(to.x) - from.x);
    const std::int64_t deltaY = -std::llabs(static_cast<std::int64_t>(to.y) - from.y);
    const int stepX = from.x < to.x ? 1 : -1;
    const int stepY = from.y < to.y ? 1 : -1;
    std::int64_t error = deltaX + deltaY;
    bool drawn = false;

    for (;;)
    {
        const Rect pointRect
        {
            SaturateToInt(static_cast<std::int64_t>(from.x) - before),
            SaturateToInt(static_cast<std::int64_t>(from.y) - before),
            SaturateToInt(static_cast<std::int64_t>(from.x) + after + 1),
            SaturateToInt(static_cast<std::int64_t>(from.y) + after + 1)
        };
        drawn = FillRect(surface, pointRect, color, &outputClip) || drawn;

        if (from.x == to.x && from.y == to.y)
        {
            break;
        }

        const std::int64_t doubledError = error * 2;
        if (doubledError >= deltaY)
        {
            error += deltaY;
            from.x += stepX;
        }
        if (doubledError <= deltaX)
        {
            error += deltaX;
            from.y += stepY;
        }
    }

    return drawn;
}

bool DrawRectangle(
    Surface& surface,
    const Rect& rect,
    Color fillColor,
    Color strokeColor,
    int strokeThickness,
    const Rect* clip)
{
    bool drawn = false;
    if (Alpha(fillColor) != 0)
    {
        drawn = FillRect(surface, rect, fillColor, clip) || drawn;
    }
    if (Alpha(strokeColor) != 0 && strokeThickness > 0)
    {
        drawn = StrokeRect(surface, rect, strokeColor, strokeThickness, clip) || drawn;
    }
    return drawn;
}

bool DrawEllipse(
    Surface& surface,
    const Rect& rect,
    Color fillColor,
    Color strokeColor,
    int strokeThickness,
    const Rect* clip)
{
    const Rect normalized = NormalizeRect(rect);
    const Rect output = EffectiveRect(surface, normalized, clip);
    if (normalized.Empty() || output.Empty())
    {
        return false;
    }

    const std::int64_t width = static_cast<std::int64_t>(normalized.right) - normalized.left;
    const std::int64_t height = static_cast<std::int64_t>(normalized.bottom) - normalized.top;
    if (width <= 0 || height <= 0)
    {
        return false;
    }

    // Use pixel centers and a half-open bounding RECT.  Computing the center
    // and radii in double after widening to int64_t keeps INT_MIN/INT_MAX
    // guest coordinates finite and avoids signed-overflow paths.
    const double centerX =
        (static_cast<double>(normalized.left) + static_cast<double>(normalized.right)) * 0.5;
    const double centerY =
        (static_cast<double>(normalized.top) + static_cast<double>(normalized.bottom)) * 0.5;
    const double radiusX = static_cast<double>(width) * 0.5;
    const double radiusY = static_cast<double>(height) * 0.5;
    if (radiusX <= 0.0 || radiusY <= 0.0)
    {
        return false;
    }

    const bool drawFill = Alpha(fillColor) != 0;
    const bool drawStroke = Alpha(strokeColor) != 0 && strokeThickness > 0;
    if (!drawFill && !drawStroke)
    {
        return false;
    }

    const std::int64_t inset = drawStroke ? static_cast<std::int64_t>(strokeThickness) : 0;
    const std::int64_t innerWidth = width - inset * 2;
    const std::int64_t innerHeight = height - inset * 2;
    const bool hasInnerEllipse = innerWidth > 0 && innerHeight > 0;
    const double innerRadiusX = hasInnerEllipse ? static_cast<double>(innerWidth) * 0.5 : 0.0;
    const double innerRadiusY = hasInnerEllipse ? static_cast<double>(innerHeight) * 0.5 : 0.0;

    Color* pixels = surface.Data();
    const std::size_t stride = static_cast<std::size_t>(surface.Width());
    if (pixels == nullptr || stride == 0)
    {
        return false;
    }

    bool drawn = false;
    for (int y = output.top; y < output.bottom; ++y)
    {
        const double dy = (static_cast<double>(y) + 0.5) - centerY;
        const double outerY = (dy * dy) / (radiusY * radiusY);
        Color* pixel = pixels + static_cast<std::size_t>(y) * stride + output.left;
        for (int x = output.left; x < output.right; ++x, ++pixel)
        {
            const double dx = (static_cast<double>(x) + 0.5) - centerX;
            const bool insideOuter = (dx * dx) / (radiusX * radiusX) + outerY <= 1.0;
            if (!insideOuter)
            {
                continue;
            }

            if (drawFill)
            {
                BlendPixel(pixel, fillColor);
                drawn = true;
            }

            bool insideInner = false;
            if (drawStroke && hasInnerEllipse)
            {
                const double innerDx = (static_cast<double>(x) + 0.5) - centerX;
                const double innerDy = (static_cast<double>(y) + 0.5) - centerY;
                insideInner =
                    (innerDx * innerDx) / (innerRadiusX * innerRadiusX) +
                    (innerDy * innerDy) / (innerRadiusY * innerRadiusY) <= 1.0;
            }

            if (drawStroke && !insideInner)
            {
                BlendPixel(pixel, strokeColor);
                drawn = true;
            }
        }
    }

    return drawn;
}

bool CopyRect(
    Surface& destination,
    Point destinationOrigin,
    const Surface& source,
    const Rect& sourceRect,
    const Rect* sourceClip,
    const Rect* destinationClip)
{
    if (destination.Empty() || source.Empty())
    {
        return false;
    }

    const Rect requestedSource = NormalizeRect(sourceRect);
    const Rect allowedSource = SurfaceClip(source, sourceClip);
    const Rect allowedDestination = SurfaceClip(destination, destinationClip);
    if (requestedSource.Empty() || allowedSource.Empty() || allowedDestination.Empty())
    {
        return false;
    }

    std::int64_t sourceLeft = requestedSource.left;
    std::int64_t sourceTop = requestedSource.top;
    std::int64_t sourceRight = requestedSource.right;
    std::int64_t sourceBottom = requestedSource.bottom;
    std::int64_t destinationLeft = destinationOrigin.x;
    std::int64_t destinationTop = destinationOrigin.y;

    // Clip source first and move the destination origin by the same delta so
    // source pixel (sourceRect.left, sourceRect.top) always maps to destOrigin.
    if (sourceLeft < allowedSource.left)
    {
        const std::int64_t delta = static_cast<std::int64_t>(allowedSource.left) - sourceLeft;
        sourceLeft += delta;
        destinationLeft += delta;
    }
    if (sourceTop < allowedSource.top)
    {
        const std::int64_t delta = static_cast<std::int64_t>(allowedSource.top) - sourceTop;
        sourceTop += delta;
        destinationTop += delta;
    }
    sourceRight = std::min<std::int64_t>(sourceRight, allowedSource.right);
    sourceBottom = std::min<std::int64_t>(sourceBottom, allowedSource.bottom);
    if (sourceRight <= sourceLeft || sourceBottom <= sourceTop)
    {
        return false;
    }

    std::int64_t destinationRight = destinationLeft + (sourceRight - sourceLeft);
    std::int64_t destinationBottom = destinationTop + (sourceBottom - sourceTop);

    // Then clip the destination and advance the paired source coordinates.
    if (destinationLeft < allowedDestination.left)
    {
        const std::int64_t delta = static_cast<std::int64_t>(allowedDestination.left) - destinationLeft;
        destinationLeft += delta;
        sourceLeft += delta;
    }
    if (destinationTop < allowedDestination.top)
    {
        const std::int64_t delta = static_cast<std::int64_t>(allowedDestination.top) - destinationTop;
        destinationTop += delta;
        sourceTop += delta;
    }
    if (destinationRight > allowedDestination.right)
    {
        const std::int64_t delta = destinationRight - allowedDestination.right;
        destinationRight -= delta;
        sourceRight -= delta;
    }
    if (destinationBottom > allowedDestination.bottom)
    {
        const std::int64_t delta = destinationBottom - allowedDestination.bottom;
        destinationBottom -= delta;
        sourceBottom -= delta;
    }
    if (destinationRight <= destinationLeft || destinationBottom <= destinationTop ||
        sourceRight <= sourceLeft || sourceBottom <= sourceTop)
    {
        return false;
    }

    const int sourceX = static_cast<int>(sourceLeft);
    const int sourceY = static_cast<int>(sourceTop);
    const int destinationX = static_cast<int>(destinationLeft);
    const int destinationY = static_cast<int>(destinationTop);
    const int width = static_cast<int>(sourceRight - sourceLeft);
    const int height = static_cast<int>(sourceBottom - sourceTop);

    const Color* sourcePixels = source.Data();
    Color* destinationPixels = destination.Data();
    if (sourcePixels == nullptr || destinationPixels == nullptr || width <= 0 || height <= 0)
    {
        return false;
    }

    const std::size_t sourceStride = static_cast<std::size_t>(source.Width());
    const std::size_t destinationStride = static_cast<std::size_t>(destination.Width());
    const std::size_t rowBytes = static_cast<std::size_t>(width) * sizeof(Color);

    if (&source == &destination && destinationY > sourceY)
    {
        for (int row = height - 1; row >= 0; --row)
        {
            Color* destinationRow = destinationPixels +
                static_cast<std::size_t>(destinationY + row) * destinationStride + destinationX;
            const Color* sourceRow = sourcePixels +
                static_cast<std::size_t>(sourceY + row) * sourceStride + sourceX;
            std::memmove(destinationRow, sourceRow, rowBytes);
        }
    }
    else
    {
        for (int row = 0; row < height; ++row)
        {
            Color* destinationRow = destinationPixels +
                static_cast<std::size_t>(destinationY + row) * destinationStride + destinationX;
            const Color* sourceRow = sourcePixels +
                static_cast<std::size_t>(sourceY + row) * sourceStride + sourceX;
            std::memmove(destinationRow, sourceRow, rowBytes);
        }
    }

    return true;
}

GdiContext::GdiContext()
{
    m_defaultPen.color = OpaqueBlack;
    m_defaultPen.width = 1;
    m_defaultPen.isNull = false;
    m_defaultBrush.color = OpaqueWhite;
    m_defaultBrush.isNull = false;
    m_defaultFont.height = DefaultTextGlyphHeight;
    m_defaultFont.width = DefaultTextGlyphWidth;
    m_defaultFont.weight = 400;
}

DcHandle GdiContext::CreateDc(Surface* surface)
{
    if (surface == nullptr)
    {
        return InvalidDc;
    }

    DeviceContext dc;
    dc.surface = surface;
    dc.clip = surface->Bounds();
    dc.isMemoryDc = false;
    dc.clipIsDefault = true;
    return AllocateDc(dc);
}

DcHandle GdiContext::CreateMemoryDc()
{
    DeviceContext dc;
    dc.surface = nullptr;
    dc.clip = Rect{};
    dc.bitmap = InvalidObject;
    dc.isMemoryDc = true;
    dc.clipIsDefault = true;
    return AllocateDc(dc);
}

bool GdiContext::DestroyDc(DcHandle dc)
{
    return m_dcs.erase(dc) != 0;
}

void GdiContext::Reset()
{
    // Device contexts can hold non-owning pointers to a window Surface, while
    // bitmap objects own their own Surface.  Drop both collections together so
    // no later bridge call can observe either kind after a guest has exited.
    // Do not reset the allocators: a stale handle from that guest must never
    // resolve to a fresh resource in a subsequent run.
    m_dcs.clear();
    m_objects.clear();
}

bool GdiContext::HasDc(DcHandle dc) const
{
    return FindDc(dc) != nullptr;
}

bool GdiContext::IsMemoryDc(DcHandle dc) const
{
    const DeviceContext* context = FindDc(dc);
    return context != nullptr && context->isMemoryDc;
}

Surface* GdiContext::GetSurface(DcHandle dc)
{
    DeviceContext* context = FindDc(dc);
    return context == nullptr ? nullptr : context->surface;
}

const Surface* GdiContext::GetSurface(DcHandle dc) const
{
    const DeviceContext* context = FindDc(dc);
    return context == nullptr ? nullptr : context->surface;
}

ObjectHandle GdiContext::CreatePen(Color color, int width)
{
    if (width <= 0)
    {
        return InvalidObject;
    }

    GdiObject object;
    object.kind = ObjectKind::Pen;
    object.pen.color = color;
    object.pen.width = width;
    object.pen.isNull = false;
    return AllocateObject(object);
}

ObjectHandle GdiContext::CreateNullPen()
{
    GdiObject object;
    object.kind = ObjectKind::Pen;
    object.pen.isNull = true;
    return AllocateObject(object);
}

ObjectHandle GdiContext::CreateSolidBrush(Color color)
{
    GdiObject object;
    object.kind = ObjectKind::Brush;
    object.brush.color = color;
    object.brush.isNull = false;
    return AllocateObject(object);
}

ObjectHandle GdiContext::CreateNullBrush()
{
    GdiObject object;
    object.kind = ObjectKind::Brush;
    object.brush.isNull = true;
    return AllocateObject(object);
}

ObjectHandle GdiContext::CreateFont(const Font& font)
{
    GdiObject object;
    object.kind = ObjectKind::Font;
    object.font = font;
    return AllocateObject(object);
}

BitmapHandle GdiContext::CreateBitmap(int width, int height, Color clearColor)
{
    if (width <= 0 || height <= 0)
    {
        return InvalidObject;
    }

    std::shared_ptr<Surface> bitmap;
    try
    {
        bitmap = std::make_shared<Surface>();
    }
    catch (const std::bad_alloc&)
    {
        return InvalidObject;
    }

    if (!bitmap->Resize(width, height, clearColor))
    {
        return InvalidObject;
    }

    GdiObject object;
    object.kind = ObjectKind::Bitmap;
    object.bitmap = bitmap;
    return AllocateObject(object);
}

bool GdiContext::GetBitmapSize(BitmapHandle bitmap, Size* size) const
{
    const Surface* surface = ResolveBitmap(bitmap);
    if (surface == nullptr || size == nullptr)
    {
        return false;
    }

    *size = Size{ surface->Width(), surface->Height() };
    return true;
}

Surface* GdiContext::GetBitmapSurface(BitmapHandle bitmap)
{
    return ResolveBitmap(bitmap);
}

const Surface* GdiContext::GetBitmapSurface(BitmapHandle bitmap) const
{
    return ResolveBitmap(bitmap);
}

bool GdiContext::DeleteObject(ObjectHandle object)
{
    if (object == InvalidObject || m_objects.erase(object) == 0)
    {
        return false;
    }

    // Deleting a selected pen/brush/font restores its built-in default.
    // Deleting a selected bitmap detaches the memory DC.  Both make dangling
    // guest GDI handles harmless instead of dereferencing stale host pointers.
    for (auto& pair : m_dcs)
    {
        if (pair.second.pen == object)
        {
            pair.second.pen = InvalidObject;
        }
        if (pair.second.brush == object)
        {
            pair.second.brush = InvalidObject;
        }
        if (pair.second.font == object)
        {
            pair.second.font = InvalidObject;
        }
        if (pair.second.isMemoryDc && pair.second.bitmap == object)
        {
            pair.second.bitmap = InvalidObject;
            pair.second.surface = nullptr;
            pair.second.clip = Rect{};
        }
    }

    return true;
}

bool GdiContext::ObjectType(ObjectHandle object, ObjectKind* kind) const
{
    const auto found = m_objects.find(object);
    if (found == m_objects.end())
    {
        return false;
    }

    if (kind != nullptr)
    {
        *kind = found->second.kind;
    }
    return true;
}

bool GdiContext::GetFont(ObjectHandle object, Font* font) const
{
    const auto found = m_objects.find(object);
    if (font == nullptr || found == m_objects.end() || found->second.kind != ObjectKind::Font)
    {
        return false;
    }

    *font = found->second.font;
    return true;
}

bool GdiContext::GetFontMetrics(ObjectHandle object, FontMetrics* metrics) const
{
    if (!metrics || (object != InvalidObject &&
        !IsObjectKind(object, ObjectKind::Font))) return false;
    const Font font = ResolveFont(object);
    Size ignored{};
    if (DirectWriteText::Measure(font, nullptr, 0, &ignored, metrics)) return true;
    const Size cell = FontCellSize(font);
    metrics->height = cell.height;
    metrics->ascent = (std::max)(1, cell.height * DefaultTextGlyphAscent /
        DefaultTextGlyphHeight);
    metrics->descent = cell.height - metrics->ascent;
    metrics->internalLeading = 0;
    metrics->externalLeading = 0;
    metrics->averageWidth = cell.width;
    metrics->maximumWidth = cell.width;
    return true;
}

bool GdiContext::GetSelectedFont(DcHandle dc, Font* font) const
{
    const DeviceContext* context = FindDc(dc);
    if (context == nullptr || font == nullptr)
    {
        return false;
    }

    *font = ResolveFont(context->font);
    return true;
}

bool GdiContext::GetSelectedFontMetrics(DcHandle dc, FontMetrics* metrics) const
{
    const DeviceContext* context = FindDc(dc);
    if (!context || !metrics) return false;
    return GetFontMetrics(context->font, metrics);
}

bool GdiContext::SelectPen(DcHandle dc, ObjectHandle pen, ObjectHandle* previous)
{
    DeviceContext* context = FindDc(dc);
    if (context == nullptr || (pen != InvalidObject && !IsObjectKind(pen, ObjectKind::Pen)))
    {
        return false;
    }

    if (previous != nullptr)
    {
        *previous = context->pen;
    }
    context->pen = pen;
    return true;
}

bool GdiContext::SelectBrush(DcHandle dc, ObjectHandle brush, ObjectHandle* previous)
{
    DeviceContext* context = FindDc(dc);
    if (context == nullptr || (brush != InvalidObject && !IsObjectKind(brush, ObjectKind::Brush)))
    {
        return false;
    }

    if (previous != nullptr)
    {
        *previous = context->brush;
    }
    context->brush = brush;
    return true;
}

bool GdiContext::SelectFont(DcHandle dc, ObjectHandle font, ObjectHandle* previous)
{
    DeviceContext* context = FindDc(dc);
    if (context == nullptr || (font != InvalidObject && !IsObjectKind(font, ObjectKind::Font)))
    {
        return false;
    }

    if (previous != nullptr)
    {
        *previous = context->font;
    }
    context->font = font;
    return true;
}

bool GdiContext::SelectBitmap(DcHandle dc, BitmapHandle bitmap, BitmapHandle* previous)
{
    DeviceContext* context = FindDc(dc);
    if (context == nullptr || !context->isMemoryDc ||
        (bitmap != InvalidObject && !IsObjectKind(bitmap, ObjectKind::Bitmap)))
    {
        return false;
    }

    if (previous != nullptr)
    {
        *previous = context->bitmap;
    }

    context->bitmap = bitmap;
    context->surface = ResolveBitmap(bitmap);
    if (context->surface == nullptr)
    {
        context->clip = Rect{};
    }
    else if (context->clipIsDefault)
    {
        context->clip = context->surface->Bounds();
    }
    else
    {
        context->clip = IntersectRect(context->surface->Bounds(), context->clip);
    }

    return true;
}

bool GdiContext::SetClipRect(DcHandle dc, const Rect& clip)
{
    DeviceContext* context = FindDc(dc);
    if (context == nullptr || context->surface == nullptr)
    {
        return false;
    }

    context->clip = IntersectRect(context->surface->Bounds(), clip);
    context->clipIsDefault = false;
    return true;
}

bool GdiContext::ResetClip(DcHandle dc)
{
    DeviceContext* context = FindDc(dc);
    if (context == nullptr || context->surface == nullptr)
    {
        return false;
    }

    context->clip = context->surface->Bounds();
    context->clipIsDefault = true;
    return true;
}

bool GdiContext::GetClipRect(DcHandle dc, Rect* clip) const
{
    const DeviceContext* context = FindDc(dc);
    if (context == nullptr || clip == nullptr)
    {
        return false;
    }

    *clip = context->clip;
    return true;
}

bool GdiContext::MoveTo(DcHandle dc, Point point, Point* previous)
{
    DeviceContext* context = FindDc(dc);
    if (context == nullptr)
    {
        return false;
    }

    if (previous != nullptr)
    {
        *previous = context->current;
    }
    context->current = point;
    return true;
}

bool GdiContext::GetCurrentPosition(DcHandle dc, Point* point) const
{
    const DeviceContext* context = FindDc(dc);
    if (context == nullptr || point == nullptr)
    {
        return false;
    }

    *point = context->current;
    return true;
}

bool GdiContext::LineTo(DcHandle dc, Point point)
{
    DeviceContext* context = FindDc(dc);
    if (context == nullptr || context->surface == nullptr)
    {
        return false;
    }

    const Point start = context->current;
    context->current = point;
    const Pen& pen = ResolvePen(context->pen);
    if (pen.isNull)
    {
        return false;
    }

    return DrawLine(*context->surface, start, point, pen.color, pen.width, &context->clip);
}

bool GdiContext::FillRect(DcHandle dc, const Rect& rect)
{
    DeviceContext* context = FindDc(dc);
    if (context == nullptr || context->surface == nullptr)
    {
        return false;
    }

    const Brush& brush = ResolveBrush(context->brush);
    if (brush.isNull)
    {
        return false;
    }

    return MiniGdi::FillRect(*context->surface, rect, brush.color, &context->clip);
}

bool GdiContext::Rectangle(DcHandle dc, const Rect& rect)
{
    DeviceContext* context = FindDc(dc);
    if (context == nullptr || context->surface == nullptr)
    {
        return false;
    }

    const Brush& brush = ResolveBrush(context->brush);
    const Pen& pen = ResolvePen(context->pen);
    const Color fill = brush.isNull ? Transparent : brush.color;
    const Color stroke = pen.isNull ? Transparent : pen.color;
    return DrawRectangle(*context->surface, rect, fill, stroke, pen.width, &context->clip);
}

bool GdiContext::Ellipse(DcHandle dc, const Rect& rect)
{
    DeviceContext* context = FindDc(dc);
    if (context == nullptr || context->surface == nullptr)
    {
        return false;
    }

    const Brush& brush = ResolveBrush(context->brush);
    const Pen& pen = ResolvePen(context->pen);
    const Color fill = brush.isNull ? Transparent : brush.color;
    const Color stroke = pen.isNull ? Transparent : pen.color;
    return DrawEllipse(*context->surface, rect, fill, stroke, pen.width, &context->clip);
}

bool GdiContext::SetPixelV(DcHandle dc, Point point, Color color)
{
    DeviceContext* context = FindDc(dc);
    if (context == nullptr || context->surface == nullptr)
    {
        return false;
    }

    const Rect outputClip = SurfaceClip(*context->surface, &context->clip);
    if (outputClip.Empty() || point.x < outputClip.left || point.x >= outputClip.right ||
        point.y < outputClip.top || point.y >= outputClip.bottom)
    {
        // GDI drawing outside the current clip is valid and simply produces
        // no raster output.  Preserve that distinction from an invalid HDC.
        return true;
    }

    Color* pixel = context->surface->PixelAt(point.x, point.y);
    if (pixel == nullptr)
    {
        return false;
    }
    if (Alpha(color) != 0)
    {
        BlendPixel(pixel, color);
    }
    return true;
}

bool GdiContext::GetPixel(DcHandle dc, Point point, Color* color) const
{
    const DeviceContext* context = FindDc(dc);
    if (context == nullptr || context->surface == nullptr || color == nullptr)
    {
        return false;
    }

    const Rect outputClip = SurfaceClip(*context->surface, &context->clip);
    if (outputClip.Empty() || point.x < outputClip.left || point.x >= outputClip.right ||
        point.y < outputClip.top || point.y >= outputClip.bottom)
    {
        return false;
    }

    const Color* pixel = context->surface->PixelAt(point.x, point.y);
    if (pixel == nullptr)
    {
        return false;
    }

    *color = *pixel;
    return true;
}

bool GdiContext::Clear(DcHandle dc, Color color)
{
    DeviceContext* context = FindDc(dc);
    if (context == nullptr || context->surface == nullptr)
    {
        return false;
    }

    context->surface->Clear(color);
    return true;
}

bool GdiContext::BitBlt(
    DcHandle destinationDc,
    Point destinationOrigin,
    DcHandle sourceDc,
    Point sourceOrigin,
    Size extent)
{
    DeviceContext* destination = FindDc(destinationDc);
    DeviceContext* source = FindDc(sourceDc);
    if (destination == nullptr || source == nullptr ||
        destination->surface == nullptr || source->surface == nullptr ||
        extent.width <= 0 || extent.height <= 0)
    {
        return false;
    }

    const Rect sourceRect
    {
        sourceOrigin.x,
        sourceOrigin.y,
        SaturateToInt(static_cast<std::int64_t>(sourceOrigin.x) + extent.width),
        SaturateToInt(static_cast<std::int64_t>(sourceOrigin.y) + extent.height)
    };
    return CopyRect(
        *destination->surface,
        destinationOrigin,
        *source->surface,
        sourceRect,
        &source->clip,
        &destination->clip);
}

bool GdiContext::SetTextColor(DcHandle dc, Color color, Color* previous)
{
    DeviceContext* context = FindDc(dc);
    if (context == nullptr)
    {
        return false;
    }

    if (previous != nullptr)
    {
        *previous = context->textColor;
    }
    context->textColor = color;
    return true;
}

bool GdiContext::GetTextColor(DcHandle dc, Color* color) const
{
    const DeviceContext* context = FindDc(dc);
    if (context == nullptr || color == nullptr)
    {
        return false;
    }

    *color = context->textColor;
    return true;
}

bool GdiContext::SetBackgroundColor(DcHandle dc, Color color, Color* previous)
{
    DeviceContext* context = FindDc(dc);
    if (context == nullptr)
    {
        return false;
    }

    if (previous != nullptr)
    {
        *previous = context->backgroundColor;
    }
    context->backgroundColor = color;
    return true;
}

bool GdiContext::GetBackgroundColor(DcHandle dc, Color* color) const
{
    const DeviceContext* context = FindDc(dc);
    if (context == nullptr || color == nullptr)
    {
        return false;
    }

    *color = context->backgroundColor;
    return true;
}

bool GdiContext::SetBackgroundMode(
    DcHandle dc,
    BackgroundMode mode,
    BackgroundMode* previous)
{
    if (mode != BackgroundMode::Opaque && mode != BackgroundMode::Transparent)
    {
        return false;
    }

    DeviceContext* context = FindDc(dc);
    if (context == nullptr)
    {
        return false;
    }

    if (previous != nullptr)
    {
        *previous = context->backgroundMode;
    }
    context->backgroundMode = mode;
    return true;
}

bool GdiContext::GetBackgroundMode(DcHandle dc, BackgroundMode* mode) const
{
    const DeviceContext* context = FindDc(dc);
    if (context == nullptr || mode == nullptr)
    {
        return false;
    }

    *mode = context->backgroundMode;
    return true;
}

bool GdiContext::TextOutW(
    DcHandle dc,
    Point origin,
    const wchar_t* text,
    std::size_t characterCount,
    Size* extent)
{
    DeviceContext* context = FindDc(dc);
    if (context == nullptr || context->surface == nullptr ||
        (characterCount != 0 && text == nullptr))
    {
        return false;
    }

    const Font selectedFont = ResolveFont(context->font);
    const Size cell = FontCellSize(selectedFont);
    Size directWriteExtent{};
    if (DirectWriteText::Measure(selectedFont, text, characterCount,
        &directWriteExtent, nullptr))
    {
        if (extent) *extent = directWriteExtent;
        if (characterCount == 0) return true;
        const Rect outputClip = SurfaceClip(*context->surface, &context->clip);
        if (outputClip.Empty()) return true;
        if (context->backgroundMode == BackgroundMode::Opaque)
        {
            MiniGdi::FillRect(*context->surface,
                Rect{ origin.x, origin.y,
                    SaturateToInt(static_cast<std::int64_t>(origin.x) +
                        directWriteExtent.width),
                    SaturateToInt(static_cast<std::int64_t>(origin.y) +
                        directWriteExtent.height) },
                context->backgroundColor, &outputClip);
        }
        if (DirectWriteText::Draw(*context->surface, outputClip, origin,
            selectedFont, text, characterCount, context->textColor,
            context->backgroundMode == BackgroundMode::Opaque))
            return true;
        // A transient DirectWrite failure still falls through to the compact
        // rasterizer, preserving GDI's best-effort drawing behavior.
    }
    if (extent != nullptr)
    {
        *extent = TextExtentForCount(characterCount, cell);
    }

    if (characterCount == 0)
    {
        return true;
    }

    const Rect outputClip = SurfaceClip(*context->surface, &context->clip);
    if (outputClip.Empty())
    {
        return true;
    }

    const std::int64_t glyphTop = origin.y;
    const std::int64_t glyphBottom = glyphTop + cell.height;
    if (glyphBottom <= outputClip.top || glyphTop >= outputClip.bottom)
    {
        return true;
    }

    std::size_t firstCharacter = 0;
    const std::int64_t originX = origin.x;
    if (originX < outputClip.left)
    {
        const std::int64_t hiddenPixels = static_cast<std::int64_t>(outputClip.left) - originX;
        const std::int64_t hiddenCharacters =
            (hiddenPixels + cell.width - 1) / cell.width;
        if (static_cast<std::size_t>(hiddenCharacters) >= characterCount)
        {
            return true;
        }
        firstCharacter = static_cast<std::size_t>(hiddenCharacters);
    }

    std::size_t endCharacter = 0;
    if (originX < outputClip.right)
    {
        const std::int64_t visiblePixels = static_cast<std::int64_t>(outputClip.right) - originX;
        const std::int64_t visibleCharacters =
            (visiblePixels + cell.width - 1) / cell.width;
        if (visibleCharacters > 0)
        {
            endCharacter = std::min(
                characterCount,
                static_cast<std::size_t>(visibleCharacters));
        }
    }

    if (firstCharacter >= endCharacter)
    {
        return true;
    }

    const int top = SaturateToInt(glyphTop);
    const int bottom = SaturateToInt(glyphBottom);
    const int firstLeft = SaturateToInt(
        originX + static_cast<std::int64_t>(firstCharacter) * cell.width);
    const int lastRight = SaturateToInt(
        originX + static_cast<std::int64_t>(endCharacter) * cell.width);

    if (context->backgroundMode == BackgroundMode::Opaque)
    {
        MiniGdi::FillRect(
            *context->surface,
            Rect{ firstLeft, top, lastRight, bottom },
            context->backgroundColor,
            &outputClip);
    }

    for (std::size_t index = firstCharacter; index < endCharacter; ++index)
    {
        const int cellLeft = SaturateToInt(
            originX + static_cast<std::int64_t>(index) * cell.width);
        RasterGlyph(
            *context->surface,
            cellLeft,
            top,
            GlyphForCharacter(text[index]),
            context->textColor,
            outputClip,
            cell);
    }

    if (selectedFont.underline)
    {
        MiniGdi::FillRect(*context->surface,
            Rect{ firstLeft, (std::max)(top, bottom - 2), lastRight, bottom - 1 },
            context->textColor, &outputClip);
    }
    if (selectedFont.strikeOut)
    {
        const int strikeY = top + cell.height / 2;
        MiniGdi::FillRect(*context->surface,
            Rect{ firstLeft, strikeY, lastRight, strikeY + 1 },
            context->textColor, &outputClip);
    }

    return true;
}

bool GdiContext::GetTextExtentW(DcHandle dc, const wchar_t* text,
    std::size_t characterCount, Size* extent) const
{
    const DeviceContext* context = FindDc(dc);
    if (context == nullptr || extent == nullptr ||
        (characterCount != 0 && text == nullptr))
    {
        return false;
    }

    if (DirectWriteText::Measure(ResolveFont(context->font), text,
        characterCount, extent, nullptr)) return true;
    *extent = TextExtentForCount(characterCount, FontCellSize(ResolveFont(context->font)));
    return true;
}

bool GdiContext::DrawTextW(DcHandle dc, const Rect& layoutRect,
    const wchar_t* text, std::size_t characterCount,
    const TextLayoutOptions& options, bool draw, Size* extent)
{
    DeviceContext* context = FindDc(dc);
    if (!context || !extent || (characterCount != 0 && !text)) return false;
    Surface* surface = draw ? context->surface : nullptr;
    if (draw && !surface) return false;

    Rect effectiveClip{};
    const Rect* clip = nullptr;
    if (surface)
    {
        effectiveClip = SurfaceClip(*surface, &context->clip);
        if (options.clipToLayout)
            effectiveClip = IntersectRect(effectiveClip, NormalizeRect(layoutRect));
        clip = &effectiveClip;
    }
    if (DirectWriteText::Layout(surface, clip, layoutRect,
        ResolveFont(context->font), text, characterCount, options,
        context->textColor, context->backgroundColor,
        context->backgroundMode == BackgroundMode::Opaque, extent)) return true;

    // Keep the deterministic compact rasterizer available on systems where
    // DirectWrite initialization fails. It cannot reproduce wrapping, but it
    // preserves the basic DrawText contract and alignment.
    *extent = TextExtentForCount(characterCount,
        FontCellSize(ResolveFont(context->font)));
    if (!draw || characterCount == 0) return true;
    const Rect normalized = NormalizeRect(layoutRect);
    Point origin{ normalized.left, normalized.top };
    if (options.horizontal == TextHorizontalAlignment::Center)
        origin.x += ((normalized.right - normalized.left) - extent->width) / 2;
    else if (options.horizontal == TextHorizontalAlignment::Right)
        origin.x = normalized.right - extent->width;
    if (options.vertical == TextVerticalAlignment::Center)
        origin.y += ((normalized.bottom - normalized.top) - extent->height) / 2;
    else if (options.vertical == TextVerticalAlignment::Bottom)
        origin.y = normalized.bottom - extent->height;
    return TextOutW(dc, origin, text, characterCount, nullptr);
}

GdiContext::DeviceContext* GdiContext::FindDc(DcHandle dc)
{
    const auto found = m_dcs.find(dc);
    return found == m_dcs.end() ? nullptr : &found->second;
}

const GdiContext::DeviceContext* GdiContext::FindDc(DcHandle dc) const
{
    const auto found = m_dcs.find(dc);
    return found == m_dcs.end() ? nullptr : &found->second;
}

const Pen& GdiContext::ResolvePen(ObjectHandle object) const
{
    const auto found = m_objects.find(object);
    if (found == m_objects.end() || found->second.kind != ObjectKind::Pen)
    {
        return m_defaultPen;
    }

    return found->second.pen;
}

const Brush& GdiContext::ResolveBrush(ObjectHandle object) const
{
    const auto found = m_objects.find(object);
    if (found == m_objects.end() || found->second.kind != ObjectKind::Brush)
    {
        return m_defaultBrush;
    }

    return found->second.brush;
}

const Font& GdiContext::ResolveFont(ObjectHandle object) const
{
    const auto found = m_objects.find(object);
    if (found == m_objects.end() || found->second.kind != ObjectKind::Font)
    {
        return m_defaultFont;
    }

    return found->second.font;
}

Surface* GdiContext::ResolveBitmap(BitmapHandle bitmap)
{
    const auto found = m_objects.find(bitmap);
    if (found == m_objects.end() || found->second.kind != ObjectKind::Bitmap ||
        found->second.bitmap == nullptr)
    {
        return nullptr;
    }

    return found->second.bitmap.get();
}

const Surface* GdiContext::ResolveBitmap(BitmapHandle bitmap) const
{
    const auto found = m_objects.find(bitmap);
    if (found == m_objects.end() || found->second.kind != ObjectKind::Bitmap ||
        found->second.bitmap == nullptr)
    {
        return nullptr;
    }

    return found->second.bitmap.get();
}

bool GdiContext::IsObjectKind(ObjectHandle object, ObjectKind kind) const
{
    const auto found = m_objects.find(object);
    return found != m_objects.end() && found->second.kind == kind;
}

ObjectHandle GdiContext::AllocateObject(const GdiObject& object)
{
    // Handle zero is permanently reserved as the stock-object sentinel.
    for (std::uint64_t attempts = 0; attempts <= std::numeric_limits<ObjectHandle>::max(); ++attempts)
    {
        const ObjectHandle candidate = m_nextObject++;
        if (candidate != InvalidObject && m_objects.find(candidate) == m_objects.end())
        {
            m_objects.emplace(candidate, object);
            return candidate;
        }
    }

    return InvalidObject;
}

DcHandle GdiContext::AllocateDc(const DeviceContext& dc)
{
    for (std::uint64_t attempts = 0; attempts <= std::numeric_limits<DcHandle>::max(); ++attempts)
    {
        const DcHandle candidate = m_nextDc++;
        if (candidate != InvalidDc && m_dcs.find(candidate) == m_dcs.end())
        {
            m_dcs.emplace(candidate, dc);
            return candidate;
        }
    }

    return InvalidDc;
}
}
}
}
