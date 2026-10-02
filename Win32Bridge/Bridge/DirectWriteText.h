#pragma once

#include "Bridge/MiniGdi.h"

namespace Win32Bridge
{
namespace Bridge
{
namespace DirectWriteText
{
    // Returns false when DirectWrite is unavailable or rejects the requested
    // font. Callers retain MiniGDI's compact raster font as their fallback.
    bool Measure(
        const MiniGdi::Font& font,
        const wchar_t* text,
        std::size_t characterCount,
        MiniGdi::Size* extent,
        MiniGdi::FontMetrics* metrics = nullptr);

    bool Draw(
        MiniGdi::Surface& surface,
        const MiniGdi::Rect& clip,
        MiniGdi::Point origin,
        const MiniGdi::Font& font,
        const wchar_t* text,
        std::size_t characterCount,
        MiniGdi::Color color,
        bool opaqueBackground);

    // DrawText-style bounded layout. The same path performs measurement and
    // rendering, so DT_CALCRECT and the pixels on screen agree.
    bool Layout(
        MiniGdi::Surface* surface,
        const MiniGdi::Rect* clip,
        const MiniGdi::Rect& layoutRect,
        const MiniGdi::Font& font,
        const wchar_t* text,
        std::size_t characterCount,
        const MiniGdi::TextLayoutOptions& options,
        MiniGdi::Color color,
        MiniGdi::Color backgroundColor,
        bool opaqueBackground,
        MiniGdi::Size* extent);
}
}
}
