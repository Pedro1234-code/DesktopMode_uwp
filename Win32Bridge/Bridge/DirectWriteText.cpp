#include "Bridge/DirectWriteText.h"

#include <windows.h>
#include <dwrite.h>
#include <wrl/client.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cwchar>
#include <limits>
#include <mutex>
#include <new>
#include <string>
#include <unordered_map>
#include <vector>

using Microsoft::WRL::ComPtr;

namespace Win32Bridge
{
namespace Bridge
{
namespace DirectWriteText
{
namespace
{
    constexpr float LogicalDpi = 96.0f;
    constexpr wchar_t DefaultFamily[] = L"Segoe UI";
    constexpr wchar_t DefaultLocale[] = L"en-us";

    struct CachedFontObjects final
    {
        ComPtr<IDWriteTextFormat> format;
        ComPtr<IDWriteFontFace> face;
        float emSize = static_cast<float>(MiniGdi::DefaultTextGlyphHeight);
    };

    struct FactoryState final
    {
        std::once_flag once;
        ComPtr<IDWriteFactory> factory;
        ComPtr<IDWriteFontCollection> fonts;
        std::mutex cacheLock;
        std::unordered_map<std::wstring, CachedFontObjects> fontCache;
    };

    FactoryState& State()
    {
        static FactoryState state;
        return state;
    }

    IDWriteFactory* Factory()
    {
        FactoryState& state = State();
        std::call_once(state.once, [&state]()
        {
            IDWriteFactory* created = nullptr;
            if (SUCCEEDED(DWriteCreateFactory(
                DWRITE_FACTORY_TYPE_SHARED,
                __uuidof(IDWriteFactory),
                reinterpret_cast<IUnknown**>(&created))))
            {
                state.factory.Attach(created);
                state.factory->GetSystemFontCollection(&state.fonts, FALSE);
            }
        });
        return state.factory.Get();
    }

    std::wstring RequestedFamily(const MiniGdi::Font& font)
    {
        std::size_t length = 0;
        while (length < MiniGdi::FontFaceNameCapacity && font.faceName[length]) ++length;
        std::wstring requested(font.faceName, font.faceName + length);
        if (requested.empty() || _wcsicmp(requested.c_str(), L"Win32Bridge Fixed") == 0)
            return DefaultFamily;
        if (_wcsicmp(requested.c_str(), L"MS Shell Dlg") == 0 ||
            _wcsicmp(requested.c_str(), L"MS Shell Dlg 2") == 0 ||
            _wcsicmp(requested.c_str(), L"MS Sans Serif") == 0)
            return DefaultFamily;
        return requested;
    }

    std::wstring ResolveFamily(const MiniGdi::Font& font)
    {
        const std::wstring requested = RequestedFamily(font);
        FactoryState& state = State();
        if (!Factory() || !state.fonts) return requested;
        UINT32 index = 0;
        BOOL exists = FALSE;
        if (SUCCEEDED(state.fonts->FindFamilyName(requested.c_str(), &index, &exists)) && exists)
            return requested;
        if (_wcsicmp(requested.c_str(), L"Courier") == 0)
        {
            exists = FALSE;
            if (SUCCEEDED(state.fonts->FindFamilyName(L"Consolas", &index, &exists)) && exists)
                return L"Consolas";
        }
        return DefaultFamily;
    }

    DWRITE_FONT_WEIGHT FontWeight(const MiniGdi::Font& font)
    {
        return static_cast<DWRITE_FONT_WEIGHT>((std::max)(1, (std::min)(999, font.weight)));
    }

    float RequestedEmSize(const MiniGdi::Font& font)
    {
        const int requested = font.height == 0
            ? MiniGdi::DefaultTextGlyphHeight
            : (font.height == (std::numeric_limits<int>::min)()
                ? (std::numeric_limits<int>::max)()
                : std::abs(font.height));
        return static_cast<float>((std::max)(1, (std::min)(4096, requested)));
    }

    struct TextObjects final
    {
        ComPtr<IDWriteTextFormat> format;
        ComPtr<IDWriteTextLayout> layout;
        ComPtr<IDWriteFontFace> face;
        float emSize = static_cast<float>(MiniGdi::DefaultTextGlyphHeight);
    };

    std::wstring FontCacheKey(const MiniGdi::Font& font, const std::wstring& family)
    {
        return family + L"\x1f" + std::to_wstring(font.height) + L"\x1f" +
            std::to_wstring(font.width) + L"\x1f" + std::to_wstring(font.weight) +
            L"\x1f" + std::to_wstring(font.italic ? 1 : 0);
    }

    bool CreateObjects(
        const MiniGdi::Font& font,
        const wchar_t* text,
        std::size_t characterCount,
        TextObjects* result)
    {
        if (!result || characterCount > UINT32_MAX ||
            (characterCount != 0 && !text)) return false;
        *result = TextObjects{};
        IDWriteFactory* factory = Factory();
        FactoryState& state = State();
        if (!factory || !state.fonts) return false;

        const std::wstring family = ResolveFamily(font);
        const std::wstring cacheKey = FontCacheKey(font, family);
        CachedFontObjects cached;
        {
            std::lock_guard<std::mutex> guard(state.cacheLock);
            const auto found = state.fontCache.find(cacheKey);
            if (found != state.fontCache.end()) cached = found->second;
        }
        if (!cached.format || !cached.face)
        {
            UINT32 familyIndex = 0;
            BOOL exists = FALSE;
            ComPtr<IDWriteFontFamily> fontFamily;
            ComPtr<IDWriteFont> resolvedFont;
            if (FAILED(state.fonts->FindFamilyName(family.c_str(), &familyIndex, &exists)) ||
                !exists || FAILED(state.fonts->GetFontFamily(familyIndex, &fontFamily)) ||
                FAILED(fontFamily->GetFirstMatchingFont(
                    FontWeight(font), DWRITE_FONT_STRETCH_NORMAL,
                    font.italic ? DWRITE_FONT_STYLE_ITALIC : DWRITE_FONT_STYLE_NORMAL,
                    &resolvedFont)) || FAILED(resolvedFont->CreateFontFace(&cached.face)))
                return false;

            float emSize = RequestedEmSize(font);
            if (font.height > 0)
            {
                DWRITE_FONT_METRICS metrics{};
                cached.face->GetMetrics(&metrics);
                const UINT32 cellUnits = metrics.ascent + metrics.descent;
                if (metrics.designUnitsPerEm && cellUnits)
                    emSize = static_cast<float>(font.height) * metrics.designUnitsPerEm /
                        static_cast<float>(cellUnits);
            }
            cached.emSize = (std::max)(1.0f, emSize);
            if (FAILED(factory->CreateTextFormat(
                family.c_str(), state.fonts.Get(), FontWeight(font),
                font.italic ? DWRITE_FONT_STYLE_ITALIC : DWRITE_FONT_STYLE_NORMAL,
                DWRITE_FONT_STRETCH_NORMAL, cached.emSize, DefaultLocale,
                &cached.format))) return false;
            cached.format->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
            {
                std::lock_guard<std::mutex> guard(state.cacheLock);
                if (state.fontCache.size() >= 256) state.fontCache.clear();
                state.fontCache.emplace(cacheKey, cached);
            }
        }
        result->format = cached.format;
        result->face = cached.face;
        result->emSize = cached.emSize;
        result->format->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);

        const wchar_t empty = L'\0';
        const wchar_t* source = characterCount ? text : &empty;
        if (FAILED(factory->CreateGdiCompatibleTextLayout(
            source, static_cast<UINT32>(characterCount), result->format.Get(),
            1048576.0f, 1048576.0f, LogicalDpi / 96.0f, nullptr, TRUE,
            &result->layout))) return false;
        if (characterCount)
        {
            const DWRITE_TEXT_RANGE range{ 0, static_cast<UINT32>(characterCount) };
            if (font.underline) result->layout->SetUnderline(TRUE, range);
            if (font.strikeOut) result->layout->SetStrikethrough(TRUE, range);
        }
        return true;
    }

    bool CreateBoundedObjects(
        const MiniGdi::Font& font,
        const wchar_t* text,
        std::size_t characterCount,
        const MiniGdi::Size& bounds,
        const MiniGdi::TextLayoutOptions& options,
        TextObjects* result)
    {
        if (!CreateObjects(font, text, characterCount, result)) return false;
        const float width = static_cast<float>((std::max)(0, bounds.width));
        const float height = static_cast<float>((std::max)(0, bounds.height));
        if (FAILED(result->layout->SetMaxWidth(width)) ||
            FAILED(result->layout->SetMaxHeight(height))) return false;

        DWRITE_TEXT_ALIGNMENT horizontal = DWRITE_TEXT_ALIGNMENT_LEADING;
        if (options.horizontal == MiniGdi::TextHorizontalAlignment::Center)
            horizontal = DWRITE_TEXT_ALIGNMENT_CENTER;
        else if (options.horizontal == MiniGdi::TextHorizontalAlignment::Right)
            horizontal = DWRITE_TEXT_ALIGNMENT_TRAILING;
        if (FAILED(result->layout->SetTextAlignment(horizontal))) return false;

        DWRITE_PARAGRAPH_ALIGNMENT vertical = DWRITE_PARAGRAPH_ALIGNMENT_NEAR;
        if (options.vertical == MiniGdi::TextVerticalAlignment::Center)
            vertical = DWRITE_PARAGRAPH_ALIGNMENT_CENTER;
        else if (options.vertical == MiniGdi::TextVerticalAlignment::Bottom)
            vertical = DWRITE_PARAGRAPH_ALIGNMENT_FAR;
        if (FAILED(result->layout->SetParagraphAlignment(vertical)) ||
            FAILED(result->layout->SetWordWrapping(options.wordWrap
                ? DWRITE_WORD_WRAPPING_WRAP : DWRITE_WORD_WRAPPING_NO_WRAP)))
            return false;

        DWRITE_FONT_METRICS fontMetrics{};
        result->face->GetMetrics(&fontMetrics);
        if (fontMetrics.designUnitsPerEm)
        {
            const float scale = result->emSize / fontMetrics.designUnitsPerEm;
            const float baseline = fontMetrics.ascent * scale;
            float lineHeight = (fontMetrics.ascent + fontMetrics.descent) * scale;
            if (options.includeExternalLeading)
                lineHeight += fontMetrics.lineGap * scale;
            if (FAILED(result->layout->SetLineSpacing(
                DWRITE_LINE_SPACING_METHOD_UNIFORM,
                (std::max)((std::max)(1.0f, baseline), lineHeight),
                (std::max)(0.0f, baseline))))
                return false;
        }

        if (options.rightToLeft &&
            FAILED(result->layout->SetReadingDirection(DWRITE_READING_DIRECTION_RIGHT_TO_LEFT)))
            return false;
        if (options.tabStop > 0.0f &&
            FAILED(result->layout->SetIncrementalTabStop(options.tabStop)))
            return false;
        if (options.mnemonicStart < characterCount)
        {
            const DWRITE_TEXT_RANGE mnemonic{
                static_cast<UINT32>(options.mnemonicStart), 1 };
            if (FAILED(result->layout->SetUnderline(TRUE, mnemonic))) return false;
        }

        if (options.trimming != MiniGdi::TextTrimming::None)
        {
            DWRITE_TRIMMING trimming{};
            trimming.granularity = options.trimming == MiniGdi::TextTrimming::Word
                ? DWRITE_TRIMMING_GRANULARITY_WORD
                : DWRITE_TRIMMING_GRANULARITY_CHARACTER;
            if (options.trimming == MiniGdi::TextTrimming::Path)
            {
                trimming.delimiter = L'\\';
                trimming.delimiterCount = 1;
            }
            ComPtr<IDWriteInlineObject> sign;
            if (!Factory() || FAILED(Factory()->CreateEllipsisTrimmingSign(
                result->format.Get(), &sign)) ||
                FAILED(result->layout->SetTrimming(&trimming, sign.Get()))) return false;
        }
        return true;
    }

    int CeilToInt(float value)
    {
        if (!(value > 0.0f)) return 0;
        if (value >= static_cast<float>((std::numeric_limits<int>::max)()))
            return (std::numeric_limits<int>::max)();
        return static_cast<int>(std::ceil(value));
    }

    void PopulateFontMetrics(const TextObjects& objects, MiniGdi::FontMetrics* output)
    {
        if (!output || !objects.face) return;
        DWRITE_FONT_METRICS metrics{};
        objects.face->GetMetrics(&metrics);
        if (!metrics.designUnitsPerEm) return;
        const float scale = objects.emSize / metrics.designUnitsPerEm;
        output->ascent = CeilToInt(metrics.ascent * scale);
        output->descent = CeilToInt(metrics.descent * scale);
        output->internalLeading = (std::max)(0,
            output->ascent + output->descent - CeilToInt(objects.emSize));
        output->externalLeading = CeilToInt(metrics.lineGap * scale);
        output->height = output->ascent + output->descent;
        output->averageWidth = (std::max)(1, CeilToInt(objects.emSize * 0.5f));
        output->maximumWidth = (std::max)(output->averageWidth, CeilToInt(objects.emSize));
        static constexpr wchar_t WidthSample[] =
            L"abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789";
        ComPtr<IDWriteTextLayout> sample;
        IDWriteFactory* factory = Factory();
        if (factory && SUCCEEDED(factory->CreateGdiCompatibleTextLayout(
            WidthSample, static_cast<UINT32>(_countof(WidthSample) - 1),
            objects.format.Get(), 1048576.0f, 1048576.0f, 1.0f,
            nullptr, TRUE, &sample)))
        {
            DWRITE_TEXT_METRICS sampleMetrics{};
            if (SUCCEEDED(sample->GetMetrics(&sampleMetrics)))
            {
                output->averageWidth = (std::max)(1, CeilToInt(
                    sampleMetrics.widthIncludingTrailingWhitespace /
                    static_cast<float>(_countof(WidthSample) - 1)));
                output->maximumWidth = (std::max)(output->averageWidth,
                    CeilToInt(objects.emSize * 1.5f));
            }
        }
    }

    float HorizontalScale(const MiniGdi::Font& font, const TextObjects& objects)
    {
        if (font.width <= 0) return 1.0f;
        MiniGdi::FontMetrics metrics{};
        PopulateFontMetrics(objects, &metrics);
        if (metrics.averageWidth <= 0) return 1.0f;
        return (std::max)(0.125f, (std::min)(8.0f,
            static_cast<float>(font.width) / metrics.averageWidth));
    }

    class GlyphRenderer final : public IDWriteTextRenderer
    {
    public:
        GlyphRenderer(IDWriteFactory* factory, MiniGdi::Surface* surface,
            const MiniGdi::Rect& clip, MiniGdi::Color color,
            bool opaqueBackground, std::uint8_t quality, float horizontalScale,
            int escapement, MiniGdi::Point transformOrigin,
            bool renderGlyphs = true)
            : m_factory(factory), m_surface(surface), m_clip(clip),
              m_color(color), m_renderGlyphs(renderGlyphs)
        {
            if (m_factory) m_factory->AddRef();
            constexpr std::uint8_t NonAntialiasedQuality = 3;
            constexpr std::uint8_t AntialiasedQuality = 4;
            m_renderingMode = quality == NonAntialiasedQuality
                ? DWRITE_RENDERING_MODE_ALIASED
                : quality == 6
                    ? DWRITE_RENDERING_MODE_NATURAL
                    : DWRITE_RENDERING_MODE_GDI_NATURAL;
            m_texture = quality == NonAntialiasedQuality
                ? DWRITE_TEXTURE_ALIASED_1x1
                : DWRITE_TEXTURE_CLEARTYPE_3x1;
            m_subpixel = opaqueBackground && quality != AntialiasedQuality &&
                quality != NonAntialiasedQuality;
            constexpr float Pi = 3.14159265358979323846f;
            const float radians = -static_cast<float>(escapement) * Pi / 1800.0f;
            const float cosine = std::cos(radians);
            const float sine = std::sin(radians);
            m_transform.m11 = cosine * horizontalScale;
            m_transform.m12 = sine * horizontalScale;
            m_transform.m21 = -sine;
            m_transform.m22 = cosine;
            m_transform.dx = transformOrigin.x -
                (transformOrigin.x * m_transform.m11 +
                    transformOrigin.y * m_transform.m21);
            m_transform.dy = transformOrigin.y -
                (transformOrigin.x * m_transform.m12 +
                    transformOrigin.y * m_transform.m22);
            m_transformed = std::abs(horizontalScale - 1.0f) > 0.001f ||
                escapement != 0;
            if (m_transformed) m_subpixel = false;
        }

        ~GlyphRenderer()
        {
            if (m_factory) m_factory->Release();
        }

        IFACEMETHOD(QueryInterface)(REFIID iid, void** object) override
        {
            if (!object) return E_POINTER;
            *object = nullptr;
            if (iid == __uuidof(IUnknown) || iid == __uuidof(IDWritePixelSnapping) ||
                iid == __uuidof(IDWriteTextRenderer))
            {
                *object = static_cast<IDWriteTextRenderer*>(this);
                AddRef();
                return S_OK;
            }
            return E_NOINTERFACE;
        }

        IFACEMETHOD_(ULONG, AddRef)() override { return ++m_references; }
        IFACEMETHOD_(ULONG, Release)() override
        {
            const ULONG remaining = --m_references;
            if (!remaining) delete this;
            return remaining;
        }

        IFACEMETHOD(IsPixelSnappingDisabled)(void*, BOOL* disabled) override
        {
            if (!disabled) return E_POINTER;
            *disabled = FALSE;
            return S_OK;
        }

        IFACEMETHOD(GetCurrentTransform)(void*, DWRITE_MATRIX* transform) override
        {
            if (!transform) return E_POINTER;
            *transform = m_transform;
            return S_OK;
        }

        IFACEMETHOD(GetPixelsPerDip)(void*, FLOAT* pixelsPerDip) override
        {
            if (!pixelsPerDip) return E_POINTER;
            *pixelsPerDip = 1.0f;
            return S_OK;
        }

        IFACEMETHOD(DrawGlyphRun)(void*, FLOAT baselineOriginX, FLOAT baselineOriginY,
            DWRITE_MEASURING_MODE measuringMode, const DWRITE_GLYPH_RUN* glyphRun,
            const DWRITE_GLYPH_RUN_DESCRIPTION*, IUnknown*) override
        {
            if (!m_factory || !m_surface || !glyphRun) return E_INVALIDARG;
            if (!m_renderGlyphs) return S_OK;
            ComPtr<IDWriteGlyphRunAnalysis> analysis;
            HRESULT result = m_factory->CreateGlyphRunAnalysis(
                glyphRun, 1.0f, m_transformed ? &m_transform : nullptr, m_renderingMode,
                measuringMode, baselineOriginX, baselineOriginY, &analysis);
            if (FAILED(result)) return result;
            RECT bounds{};
            if (FAILED(analysis->GetAlphaTextureBounds(m_texture, &bounds))) return E_FAIL;
            const int width = bounds.right - bounds.left;
            const int height = bounds.bottom - bounds.top;
            if (width <= 0 || height <= 0) return S_OK;
            const std::size_t channels = m_texture == DWRITE_TEXTURE_CLEARTYPE_3x1 ? 3 : 1;
            const std::size_t bytes = static_cast<std::size_t>(width) * height * channels;
            if (bytes > 128u * 1024u * 1024u) return E_OUTOFMEMORY;
            std::vector<BYTE> alpha;
            try { alpha.resize(bytes); }
            catch (const std::bad_alloc&) { return E_OUTOFMEMORY; }
            result = analysis->CreateAlphaTexture(m_texture, &bounds,
                alpha.data(), static_cast<UINT32>(alpha.size()));
            if (FAILED(result)) return result;

            for (int y = 0; y < height; ++y)
            {
                const int destinationY = bounds.top + y;
                if (destinationY < m_clip.top || destinationY >= m_clip.bottom ||
                    destinationY < 0 || destinationY >= m_surface->Height()) continue;
                for (int x = 0; x < width; ++x)
                {
                    const int destinationX = bounds.left + x;
                    if (destinationX < m_clip.left || destinationX >= m_clip.right ||
                        destinationX < 0 || destinationX >= m_surface->Width()) continue;
                    const std::size_t offset =
                        (static_cast<std::size_t>(y) * width + x) * channels;
                    const BYTE redCoverage = alpha[offset];
                    const BYTE greenCoverage = channels == 3 ? alpha[offset + 1] : redCoverage;
                    const BYTE blueCoverage = channels == 3 ? alpha[offset + 2] : redCoverage;
                    MiniGdi::Color* destination = m_surface->PixelAt(destinationX, destinationY);
                    if (!destination) continue;
                    if (!m_subpixel)
                    {
                        const BYTE coverage = static_cast<BYTE>((
                            static_cast<unsigned>(redCoverage) + greenCoverage +
                            blueCoverage + 1) / 3);
                        *destination = MiniGdi::BlendSourceOver(
                            MiniGdi::MakeColor(MiniGdi::Red(m_color),
                                MiniGdi::Green(m_color), MiniGdi::Blue(m_color), coverage),
                            *destination);
                        continue;
                    }
                    const auto blend = [](BYTE foreground, BYTE background, BYTE coverage)
                    {
                        return static_cast<BYTE>((foreground * coverage +
                            background * (255 - coverage) + 127) / 255);
                    };
                    *destination = MiniGdi::MakeColor(
                        blend(MiniGdi::Red(m_color), MiniGdi::Red(*destination), redCoverage),
                        blend(MiniGdi::Green(m_color), MiniGdi::Green(*destination), greenCoverage),
                        blend(MiniGdi::Blue(m_color), MiniGdi::Blue(*destination), blueCoverage),
                        (std::max)(MiniGdi::Alpha(*destination),
                            (std::max)(redCoverage, (std::max)(greenCoverage, blueCoverage))));
                }
            }
            return S_OK;
        }

        IFACEMETHOD(DrawUnderline)(void*, FLOAT originX, FLOAT originY,
            const DWRITE_UNDERLINE* underline, IUnknown*) override
        {
            return DrawDecoration(originX, originY, underline ? underline->offset : 0,
                underline ? underline->width : 0,
                underline ? underline->thickness : 0);
        }

        IFACEMETHOD(DrawStrikethrough)(void*, FLOAT originX, FLOAT originY,
            const DWRITE_STRIKETHROUGH* strike, IUnknown*) override
        {
            return DrawDecoration(originX, originY, strike ? strike->offset : 0,
                strike ? strike->width : 0, strike ? strike->thickness : 0);
        }

        IFACEMETHOD(DrawInlineObject)(void*, FLOAT, FLOAT, IDWriteInlineObject*,
            BOOL, BOOL, IUnknown*) override { return E_NOTIMPL; }

    private:
        HRESULT DrawDecoration(float originX, float originY, float offset,
            float width, float thickness)
        {
            if (!m_surface) return E_FAIL;
            if (m_transformed)
            {
                const auto transformedPoint = [this](float x, float y)
                {
                    return MiniGdi::Point{
                        static_cast<int>(std::lround(
                            x * m_transform.m11 + y * m_transform.m21 + m_transform.dx)),
                        static_cast<int>(std::lround(
                            x * m_transform.m12 + y * m_transform.m22 + m_transform.dy)) };
                };
                MiniGdi::DrawLine(*m_surface,
                    transformedPoint(originX, originY + offset),
                    transformedPoint(originX + width, originY + offset),
                    m_color, (std::max)(1, CeilToInt(thickness)), &m_clip);
                return S_OK;
            }
            const int left = static_cast<int>(std::floor(originX));
            const int top = static_cast<int>(std::floor(originY + offset));
            const int right = left + CeilToInt(width);
            const int bottom = top + (std::max)(1, CeilToInt(thickness));
            MiniGdi::FillRect(*m_surface,
                MiniGdi::Rect{ left, top, right, bottom }, m_color, &m_clip);
            return S_OK;
        }

        std::atomic<ULONG> m_references{ 1 };
        IDWriteFactory* m_factory = nullptr;
        MiniGdi::Surface* m_surface = nullptr;
        MiniGdi::Rect m_clip;
        MiniGdi::Color m_color = MiniGdi::OpaqueBlack;
        bool m_subpixel = false;
        DWRITE_RENDERING_MODE m_renderingMode = DWRITE_RENDERING_MODE_GDI_NATURAL;
        DWRITE_TEXTURE_TYPE m_texture = DWRITE_TEXTURE_CLEARTYPE_3x1;
        DWRITE_MATRIX m_transform{ 1, 0, 0, 1, 0, 0 };
        bool m_transformed = false;
        bool m_renderGlyphs = true;
    };
}

bool Measure(const MiniGdi::Font& font, const wchar_t* text,
    std::size_t characterCount, MiniGdi::Size* extent,
    MiniGdi::FontMetrics* fontMetrics)
{
    if (!extent || (characterCount != 0 && !text)) return false;
    TextObjects objects;
    if (!CreateObjects(font, text, characterCount, &objects)) return false;
    DWRITE_TEXT_METRICS metrics{};
    if (FAILED(objects.layout->GetMetrics(&metrics))) return false;
    extent->width = CeilToInt(metrics.widthIncludingTrailingWhitespace);
    MiniGdi::FontMetrics measuredFont{};
    PopulateFontMetrics(objects, &measuredFont);
    const float horizontalScale = HorizontalScale(font, objects);
    if (font.width > 0)
    {
        measuredFont.averageWidth = font.width;
        measuredFont.maximumWidth = CeilToInt(
            measuredFont.maximumWidth * horizontalScale);
    }
    if (fontMetrics) *fontMetrics = measuredFont;
    extent->height = measuredFont.height > 0
        ? measuredFont.height : CeilToInt(metrics.height);
    if (extent->height <= 0) extent->height = CeilToInt(objects.emSize);
    extent->width = CeilToInt(extent->width * horizontalScale);
    return true;
}

bool Draw(MiniGdi::Surface& surface, const MiniGdi::Rect& clip,
    MiniGdi::Point origin, const MiniGdi::Font& font, const wchar_t* text,
    std::size_t characterCount, MiniGdi::Color color, bool opaqueBackground)
{
    if (characterCount == 0) return true;
    TextObjects objects;
    if (!CreateObjects(font, text, characterCount, &objects)) return false;
    GlyphRenderer* renderer = new (std::nothrow) GlyphRenderer(
        Factory(), &surface, clip, color, opaqueBackground, font.quality,
        HorizontalScale(font, objects),
        font.escapement != 0 ? font.escapement : font.orientation, origin);
    if (!renderer) return false;
    const HRESULT result = objects.layout->Draw(nullptr, renderer,
        static_cast<FLOAT>(origin.x), static_cast<FLOAT>(origin.y));
    renderer->Release();
    return SUCCEEDED(result);
}

bool Layout(MiniGdi::Surface* surface, const MiniGdi::Rect* clip,
    const MiniGdi::Rect& layoutRect, const MiniGdi::Font& font,
    const wchar_t* text, std::size_t characterCount,
    const MiniGdi::TextLayoutOptions& options, MiniGdi::Color color,
    MiniGdi::Color backgroundColor, bool opaqueBackground,
    MiniGdi::Size* extent)
{
    if (!extent || (characterCount != 0 && !text)) return false;
    const MiniGdi::Rect normalized = MiniGdi::NormalizeRect(layoutRect);
    const MiniGdi::Size bounds{
        normalized.right - normalized.left,
        normalized.bottom - normalized.top };
    TextObjects objects;
    if (!CreateBoundedObjects(font, text, characterCount, bounds, options, &objects))
        return false;

    const float horizontalScale = HorizontalScale(font, objects);
    if (horizontalScale != 1.0f && FAILED(objects.layout->SetMaxWidth(
        static_cast<float>(bounds.width) / horizontalScale))) return false;

    DWRITE_TEXT_METRICS metrics{};
    if (FAILED(objects.layout->GetMetrics(&metrics))) return false;
    extent->width = CeilToInt(
        metrics.widthIncludingTrailingWhitespace * horizontalScale);
    extent->height = CeilToInt(metrics.height);
    if (!surface) return true;

    MiniGdi::Rect effectiveClip = clip
        ? MiniGdi::IntersectRect(*clip, surface->Bounds())
        : surface->Bounds();
    if (effectiveClip.Empty() || characterCount == 0) return true;
    if (opaqueBackground && options.renderGlyphs)
    {
        const int backgroundLeft = normalized.left +
            static_cast<int>(std::floor(metrics.left));
        const int backgroundTop = normalized.top +
            static_cast<int>(std::floor(metrics.top));
        MiniGdi::FillRect(*surface,
            MiniGdi::Rect{ backgroundLeft, backgroundTop,
                backgroundLeft + extent->width, backgroundTop + extent->height },
            backgroundColor, &effectiveClip);
    }

    GlyphRenderer* renderer = new (std::nothrow) GlyphRenderer(
        Factory(), surface, effectiveClip, color, opaqueBackground, font.quality,
        horizontalScale,
        font.escapement != 0 ? font.escapement : font.orientation,
        MiniGdi::Point{ normalized.left, normalized.top }, options.renderGlyphs);
    if (!renderer) return false;
    const HRESULT result = objects.layout->Draw(nullptr, renderer,
        static_cast<FLOAT>(normalized.left), static_cast<FLOAT>(normalized.top));
    renderer->Release();
    return SUCCEEDED(result);
}
}
}
}
