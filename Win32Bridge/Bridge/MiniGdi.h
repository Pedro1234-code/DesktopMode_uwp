#pragma once

// A deliberately small, software-only GDI foundation.  This layer has no
// WinRT, XAML, Direct2D, or windowing dependency: a future USER32 bridge owns
// the windows and presents Surface::Pixels() however it chooses.

#include <cstddef>
#include <cstdint>
#include <memory>
#include <unordered_map>
#include <vector>

namespace Win32Bridge
{
namespace Bridge
{
namespace MiniGdi
{
    // Color is stored as 0xAARRGGBB.  On the little-endian platforms targeted
    // by this bridge its in-memory byte order is BGRA, matching B8G8R8A8
    // WriteableBitmap / Direct3D upload buffers.
    using Color = std::uint32_t;

    constexpr Color MakeColor(
        std::uint8_t red,
        std::uint8_t green,
        std::uint8_t blue,
        std::uint8_t alpha = 0xff)
    {
        return (static_cast<Color>(alpha) << 24) |
            (static_cast<Color>(red) << 16) |
            (static_cast<Color>(green) << 8) |
            static_cast<Color>(blue);
    }

    constexpr std::uint8_t Red(Color color)
    {
        return static_cast<std::uint8_t>((color >> 16) & 0xff);
    }

    constexpr std::uint8_t Green(Color color)
    {
        return static_cast<std::uint8_t>((color >> 8) & 0xff);
    }

    constexpr std::uint8_t Blue(Color color)
    {
        return static_cast<std::uint8_t>(color & 0xff);
    }

    constexpr std::uint8_t Alpha(Color color)
    {
        return static_cast<std::uint8_t>((color >> 24) & 0xff);
    }

    constexpr Color OpaqueBlack = MakeColor(0, 0, 0);
    constexpr Color OpaqueWhite = MakeColor(0xff, 0xff, 0xff);
    constexpr Color Transparent = MakeColor(0, 0, 0, 0);

    struct Point
    {
        int x = 0;
        int y = 0;
    };

    struct Size
    {
        int width = 0;
        int height = 0;
    };

    // Like a Win32 RECT, right and bottom are exclusive.  Most public drawing
    // methods normalize their RECT input, so callers may supply reversed sides.
    struct Rect
    {
        int left = 0;
        int top = 0;
        int right = 0;
        int bottom = 0;

        bool Empty() const;
    };

    Rect NormalizeRect(const Rect& rect);
    Rect IntersectRect(const Rect& first, const Rect& second);

    // Source-over blending for straight-alpha 0xAARRGGBB colors.  Classic GDI
    // brushes and pens are normally opaque, but preserving alpha here makes
    // the core usable by later layered-window and bitmap adapters as well.
    Color BlendSourceOver(Color source, Color destination);

    class Surface final
    {
    public:
        Surface() = default;
        Surface(int width, int height, Color clearColor = Transparent);

        // Leaves this Surface untouched when dimensions overflow or allocation
        // fails.  A zero-sized Surface is valid and has no backing pixels.
        bool Resize(int width, int height, Color clearColor = Transparent);
        void Clear(Color color);

        int Width() const { return m_width; }
        int Height() const { return m_height; }
        Rect Bounds() const { return Rect{ 0, 0, m_width, m_height }; }
        bool Empty() const { return m_width <= 0 || m_height <= 0; }

        Color* Data() { return m_pixels.empty() ? nullptr : m_pixels.data(); }
        const Color* Data() const { return m_pixels.empty() ? nullptr : m_pixels.data(); }
        std::size_t PixelCount() const { return m_pixels.size(); }
        std::vector<Color>& Pixels() { return m_pixels; }
        const std::vector<Color>& Pixels() const { return m_pixels; }

        // Returns nullptr rather than allowing an out-of-range access.
        Color* PixelAt(int x, int y);
        const Color* PixelAt(int x, int y) const;

    private:
        int m_width = 0;
        int m_height = 0;
        std::vector<Color> m_pixels;
    };

    // All primitives use half-open RECTs and apply the optional clip after
    // clipping it to the Surface bounds.  They return true only if at least
    // one output pixel was changed or blended.
    bool FillRect(Surface& surface, const Rect& rect, Color color, const Rect* clip = nullptr);
    bool StrokeRect(
        Surface& surface,
        const Rect& rect,
        Color color,
        int thickness = 1,
        const Rect* clip = nullptr);
    bool DrawLine(
        Surface& surface,
        Point from,
        Point to,
        Color color,
        int thickness = 1,
        const Rect* clip = nullptr);
    bool DrawRectangle(
        Surface& surface,
        const Rect& rect,
        Color fillColor,
        Color strokeColor,
        int strokeThickness = 1,
        const Rect* clip = nullptr);
    // Draws a complete ellipse bounded by rect.  Like DrawRectangle, the
    // brush fill is laid down first and the pen is then rasterized inside the
    // bounding rectangle.  The implementation only iterates the clipped
    // output area, so an untrusted, huge guest rectangle cannot create a
    // proportional CPU walk outside the target Surface.
    bool DrawEllipse(
        Surface& surface,
        const Rect& rect,
        Color fillColor,
        Color strokeColor,
        int strokeThickness = 1,
        const Rect* clip = nullptr);

    // Exact BGRA SRCCOPY for a source rectangle.  Both clips are optional and
    // are bounded to their respective Surfaces.  The copy is overlap-safe
    // when source and destination are the same Surface.
    bool CopyRect(
        Surface& destination,
        Point destinationOrigin,
        const Surface& source,
        const Rect& sourceRect,
        const Rect* sourceClip = nullptr,
        const Rect* destinationClip = nullptr);

    // The fallback raster font is fixed-width within each selected HFONT cell.
    // HFONT objects are tracked so CreateFont/SelectObject and geometric text
    // queries share the same dimensions without depending on a host font API.
    constexpr int DefaultTextGlyphWidth = 8;
    constexpr int DefaultTextGlyphHeight = 16;
    // Baseline proportions used when scaling the compact fallback glyphs.
    constexpr int DefaultTextGlyphAscent = 12;
    constexpr int DefaultTextGlyphDescent =
        DefaultTextGlyphHeight - DefaultTextGlyphAscent;

    enum class BackgroundMode : std::uint8_t
    {
        Opaque,
        Transparent
    };

    using ObjectHandle = std::uint32_t;
    using DcHandle = std::uint32_t;
    using BitmapHandle = ObjectHandle;
    constexpr ObjectHandle InvalidObject = 0;
    constexpr DcHandle InvalidDc = 0;

    enum class ObjectKind : std::uint8_t
    {
        Pen,
        Brush,
        Bitmap,
        Font
    };

    struct Pen
    {
        Color color = OpaqueBlack;
        int width = 1;
        bool isNull = false;
    };

    struct Brush
    {
        Color color = OpaqueWhite;
        bool isNull = false;
    };

    // Keep the portable subset of LOGFONTW attributes in a fixed-size object.
    // Face names are deliberately bounded to LF_FACESIZE, so accepting guest
    // font requests cannot allocate unbounded host strings.  The current
    // rasterizer still uses its fixed fallback glyphs; these values preserve
    // the selected HFONT state for future scalable text work.
    constexpr std::size_t FontFaceNameCapacity = 32;
    struct Font
    {
        int height = 0;
        int width = 0;
        int escapement = 0;
        int orientation = 0;
        int weight = 400;
        bool italic = false;
        bool underline = false;
        bool strikeOut = false;
        std::uint8_t charSet = 1;
        std::uint8_t outPrecision = 0;
        std::uint8_t clipPrecision = 0;
        std::uint8_t quality = 0;
        std::uint8_t pitchAndFamily = 0;
        // A real host face is intentionally not selected yet.  Giving the
        // retained fallback a stable name lets GetTextFaceW distinguish it
        // from an invalid/empty result while remaining within LF_FACESIZE.
        wchar_t faceName[FontFaceNameCapacity] = L"Win32Bridge Fixed";
    };

    // Resolves the logical cell used by the software fallback rasterizer.
    // Negative LOGFONT heights describe character height on Win32; the
    // rasterizer only needs the resulting positive pixel extent.
    Size FontCellSize(const Font& font);

    // This is an in-process model of the small part of GDI object selection we
    // need.  A USER32 bridge can map guest HDC/HBRUSH/HPEN/HFONT values to
    // these integer handles without exposing host pointers to guest code.
    //
    // CreateDc uses a non-owning external Surface, whose owner must outlive its
    // DC.  A memory DC instead receives its target from a selected, owned
    // bitmap.  GdiContext is intentionally single-threaded; its guest-window
    // manager should serialize rendering and presentation on its chosen thread.
    class GdiContext final
    {
    public:
        GdiContext();
        ~GdiContext() = default;

        GdiContext(const GdiContext&) = delete;
        GdiContext& operator=(const GdiContext&) = delete;

        DcHandle CreateDc(Surface* surface);
        // A memory DC has no target until a bitmap is selected into it.
        DcHandle CreateMemoryDc();
        bool DestroyDc(DcHandle dc);

        // Discards every guest-owned DC, GDI object, and owned bitmap surface.
        // It is intended for a guest-lifetime teardown only: existing handles
        // become invalid and the monotonically allocated handle values are not
        // reused by a later guest.
        void Reset();

        bool HasDc(DcHandle dc) const;
        bool IsMemoryDc(DcHandle dc) const;
        Surface* GetSurface(DcHandle dc);
        const Surface* GetSurface(DcHandle dc) const;

        ObjectHandle CreatePen(Color color, int width = 1);
        ObjectHandle CreateNullPen();
        ObjectHandle CreateSolidBrush(Color color);
        ObjectHandle CreateNullBrush();
        ObjectHandle CreateFont(const Font& font);
        BitmapHandle CreateBitmap(int width, int height, Color clearColor = Transparent);
        bool GetBitmapSize(BitmapHandle bitmap, Size* size) const;
        Surface* GetBitmapSurface(BitmapHandle bitmap);
        const Surface* GetBitmapSurface(BitmapHandle bitmap) const;
        bool DeleteObject(ObjectHandle object);
        bool ObjectType(ObjectHandle object, ObjectKind* kind) const;
        // Returns an owned HFONT's retained LOGFONT-like attributes.  The
        // implicit default is intentionally queried through GetSelectedFont;
        // it does not have a guest-visible object handle of its own.
        bool GetFont(ObjectHandle object, Font* font) const;
        // Reports the selected HFONT, or the fixed fallback when the DC has
        // not selected an explicit font.  It is valid for a targetless memory
        // DC because Win32 permits font queries before a bitmap is selected.
        bool GetSelectedFont(DcHandle dc, Font* font) const;

        // InvalidObject selects the built-in default (black pen / white brush
        // / fixed fallback font).
        bool SelectPen(DcHandle dc, ObjectHandle pen, ObjectHandle* previous = nullptr);
        bool SelectBrush(DcHandle dc, ObjectHandle brush, ObjectHandle* previous = nullptr);
        bool SelectFont(DcHandle dc, ObjectHandle font, ObjectHandle* previous = nullptr);
        // Only memory DCs may select bitmaps.  InvalidObject detaches the
        // current bitmap and leaves the memory DC without a drawing target.
        bool SelectBitmap(DcHandle dc, BitmapHandle bitmap, BitmapHandle* previous = nullptr);

        bool SetClipRect(DcHandle dc, const Rect& clip);
        bool ResetClip(DcHandle dc);
        bool GetClipRect(DcHandle dc, Rect* clip) const;

        bool MoveTo(DcHandle dc, Point point, Point* previous = nullptr);
        bool GetCurrentPosition(DcHandle dc, Point* point) const;
        bool LineTo(DcHandle dc, Point point);
        bool FillRect(DcHandle dc, const Rect& rect);
        bool Rectangle(DcHandle dc, const Rect& rect);
        bool Ellipse(DcHandle dc, const Rect& rect);
        // SetPixelV follows GDI's no-readback contract.  A valid DC with a
        // point outside its effective clip is a successful no-op; invalid DCs
        // and memory DCs with no selected bitmap fail.
        bool SetPixelV(DcHandle dc, Point point, Color color);
        // GetPixel returns false for an invalid DC, missing target Surface,
        // or a point outside the effective clip/bounds.
        bool GetPixel(DcHandle dc, Point point, Color* color) const;
        bool Clear(DcHandle dc, Color color);

        // Minimal BitBlt equivalent: SRCCOPY only.  The source and
        // destination coordinates are paired one-for-one over extent.
        bool BitBlt(
            DcHandle destinationDc,
            Point destinationOrigin,
            DcHandle sourceDc,
            Point sourceOrigin,
            Size extent);

        bool SetTextColor(DcHandle dc, Color color, Color* previous = nullptr);
        bool GetTextColor(DcHandle dc, Color* color) const;
        bool SetBackgroundColor(DcHandle dc, Color color, Color* previous = nullptr);
        bool GetBackgroundColor(DcHandle dc, Color* color) const;
        bool SetBackgroundMode(
            DcHandle dc,
            BackgroundMode mode,
            BackgroundMode* previous = nullptr);
        bool GetBackgroundMode(DcHandle dc, BackgroundMode* mode) const;

        // Renders a scalable-cell ASCII fallback font using the selected
        // HFONT dimensions. Printable ASCII is supported and all non-ASCII
        // code points become '?'. The result
        // is true for a valid, fully clipped draw just like TextOutW; false
        // denotes invalid input or an invalid DC.
        bool TextOutW(
            DcHandle dc,
            Point origin,
            const wchar_t* text,
            std::size_t characterCount,
            Size* extent = nullptr);
        bool GetTextExtentW(DcHandle dc, std::size_t characterCount, Size* extent) const;

    private:
        struct GdiObject
        {
            ObjectKind kind = ObjectKind::Pen;
            Pen pen;
            Brush brush;
            Font font;
            std::shared_ptr<Surface> bitmap;
        };

        struct DeviceContext
        {
            Surface* surface = nullptr;
            Rect clip;
            Point current;
            ObjectHandle pen = InvalidObject;
            ObjectHandle brush = InvalidObject;
            ObjectHandle font = InvalidObject;
            BitmapHandle bitmap = InvalidObject;
            bool isMemoryDc = false;
            bool clipIsDefault = true;
            Color textColor = OpaqueBlack;
            Color backgroundColor = OpaqueWhite;
            BackgroundMode backgroundMode = BackgroundMode::Opaque;
        };

        DeviceContext* FindDc(DcHandle dc);
        const DeviceContext* FindDc(DcHandle dc) const;
        const Pen& ResolvePen(ObjectHandle object) const;
        const Brush& ResolveBrush(ObjectHandle object) const;
        const Font& ResolveFont(ObjectHandle object) const;
        Surface* ResolveBitmap(BitmapHandle bitmap);
        const Surface* ResolveBitmap(BitmapHandle bitmap) const;
        bool IsObjectKind(ObjectHandle object, ObjectKind kind) const;
        ObjectHandle AllocateObject(const GdiObject& object);
        DcHandle AllocateDc(const DeviceContext& dc);

        Pen m_defaultPen;
        Brush m_defaultBrush;
        Font m_defaultFont;
        std::unordered_map<ObjectHandle, GdiObject> m_objects;
        std::unordered_map<DcHandle, DeviceContext> m_dcs;
        // Low integer HBRUSH values are reserved by WNDCLASS for the
        // (COLOR_* + 1) shorthand. Keep bridge-owned GDI tokens outside that
        // range so a class background can unambiguously be either a system
        // color or a real guest brush.
        ObjectHandle m_nextObject = 0x1000;
        DcHandle m_nextDc = 1;
    };
}
}
}
