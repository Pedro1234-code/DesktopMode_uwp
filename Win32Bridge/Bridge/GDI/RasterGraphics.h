#pragma once

#include "Bridge/GDI/MiniGdi.h"
#include <algorithm>
#include <cmath>
#include <cstddef>

namespace Win32Bridge { namespace Bridge { namespace RasterGraphics {

struct Vertex
{
    double x, y;
    double red, green, blue; // Win32 COLOR16 channels, with 0xff00 representing 255.
};

inline MiniGdi::Color GradientColor(double red, double green, double blue)
{
    const auto channel = [](double value)
    { return static_cast<std::uint8_t>((std::max)(0.0, (std::min)(255.0, std::floor(value / 256.0)))); };
    return MiniGdi::MakeColor(channel(red), channel(green), channel(blue));
}

// Clip before integer conversion and iteration. Work is bounded by the actual
// surface even if guest coordinates describe an enormous off-screen shape.
template<typename Clip, typename Shader>
inline void Rasterize(MiniGdi::Color* pixels, int width, int height,
    double left, double top, double right, double bottom,
    const MiniGdi::Rect& bounds, Clip visible, Shader shade)
{
    if (!pixels || width <= 0 || height <= 0) return;
    left = (std::max)(left, static_cast<double>((std::max)(0, bounds.left)));
    top = (std::max)(top, static_cast<double>((std::max)(0, bounds.top)));
    right = (std::min)(right, static_cast<double>((std::min)(width, bounds.right)));
    bottom = (std::min)(bottom, static_cast<double>((std::min)(height, bounds.bottom)));
    if (!(left < right && top < bottom)) return;
    const int startX = static_cast<int>(std::ceil(left));
    const int startY = static_cast<int>(std::ceil(top));
    const int endX = static_cast<int>(std::ceil(right));
    const int endY = static_cast<int>(std::ceil(bottom));
    for (int y = startY; y < endY; ++y)
        for (int x = startX; x < endX; ++x)
            if (visible(x, y)) shade(x, y, pixels[static_cast<size_t>(y) * width + x]);
}

template<typename Clip, typename Shader>
inline bool Affine(MiniGdi::Color* pixels, int width, int height,
    const Vertex& origin, const Vertex& horizontal, const Vertex& vertical,
    const MiniGdi::Rect& bounds, Clip visible, Shader shade, double sampleOffset = 0.0)
{
    const double ux = horizontal.x - origin.x, uy = horizontal.y - origin.y;
    const double vx = vertical.x - origin.x, vy = vertical.y - origin.y;
    const double determinant = ux * vy - uy * vx;
    if (!std::isfinite(determinant) || std::abs(determinant) < 1.0e-12) return false;
    const double fourthX = horizontal.x + vertical.x - origin.x;
    const double fourthY = horizontal.y + vertical.y - origin.y;
    Rasterize(pixels, width, height,
        (std::min)({origin.x, horizontal.x, vertical.x, fourthX}) - 0.5,
        (std::min)({origin.y, horizontal.y, vertical.y, fourthY}) - 0.5,
        (std::max)({origin.x, horizontal.x, vertical.x, fourthX}) - 0.5,
        (std::max)({origin.y, horizontal.y, vertical.y, fourthY}) - 0.5, bounds, visible,
        [&](int x, int y, MiniGdi::Color& destination)
        {
            const double dx = x + sampleOffset - origin.x, dy = y + sampleOffset - origin.y;
            const double a = (dx * vy - dy * vx) / determinant;
            const double b = (dy * ux - dx * uy) / determinant;
            const double coverageX = x + 0.5 - origin.x, coverageY = y + 0.5 - origin.y;
            const double coverageA = (coverageX * vy - coverageY * vx) / determinant;
            const double coverageB = (coverageY * ux - coverageX * uy) / determinant;
            if (coverageA >= 0.0 && coverageB >= 0.0 && coverageA < 1.0 && coverageB < 1.0)
                shade((std::max)(0.0, (std::min)(1.0, a)), (std::max)(0.0, (std::min)(1.0, b)), destination);
        });
    return true;
}

template<typename Clip>
inline void GradientRectangle(MiniGdi::Color* pixels, int width, int height,
    const Vertex& origin, const Vertex& horizontal, const Vertex& vertical,
    const Vertex& endColor, bool horizontalGradient, const MiniGdi::Rect& bounds, Clip visible)
{
    Affine(pixels, width, height, origin, horizontal, vertical, bounds, visible,
        [&](double a, double b, MiniGdi::Color& destination)
        {
            const double position = horizontalGradient ? a : b;
            destination = GradientColor(origin.red + (endColor.red - origin.red) * position,
                origin.green + (endColor.green - origin.green) * position,
                origin.blue + (endColor.blue - origin.blue) * position);
        });
}

template<typename Clip>
inline void GradientTriangle(MiniGdi::Color* pixels, int width, int height,
    const Vertex& first, const Vertex& second, const Vertex& third,
    const MiniGdi::Rect& bounds, Clip visible)
{
    const double ux = second.x - first.x, uy = second.y - first.y;
    const double vx = third.x - first.x, vy = third.y - first.y;
    const double determinant = ux * vy - uy * vx;
    if (!std::isfinite(determinant) || std::abs(determinant) < 1.0e-12) return;
    Rasterize(pixels, width, height,
        (std::min)({first.x, second.x, third.x}), (std::min)({first.y, second.y, third.y}),
        (std::max)({first.x, second.x, third.x}), (std::max)({first.y, second.y, third.y}), bounds, visible,
        [&](int x, int y, MiniGdi::Color& destination)
        {
            const double dx = x - first.x, dy = y - first.y;
            const double a = (dx * vy - dy * vx) / determinant;
            const double b = (dy * ux - dx * uy) / determinant;
            if (a < -1.0e-10 || b < -1.0e-10 || a + b > 1.0 + 1.0e-10) return;
            destination = GradientColor(first.red + a * (second.red - first.red) + b * (third.red - first.red),
                first.green + a * (second.green - first.green) + b * (third.green - first.green),
                first.blue + a * (second.blue - first.blue) + b * (third.blue - first.blue));
        });
}

inline bool OverlapsRectangle(const Vertex& origin, const Vertex& horizontal, const Vertex& vertical,
    double left, double top, double right, double bottom)
{
    const Vertex polygon[] = { origin, horizontal,
        {horizontal.x + vertical.x - origin.x, horizontal.y + vertical.y - origin.y, 0, 0, 0}, vertical };
    const Vertex rectangle[] = { {left, top, 0, 0, 0}, {right, top, 0, 0, 0},
        {right, bottom, 0, 0, 0}, {left, bottom, 0, 0, 0} };
    const double axes[][2] = { {1, 0}, {0, 1},
        {horizontal.y - origin.y, origin.x - horizontal.x},
        {vertical.y - origin.y, origin.x - vertical.x} };
    for (const auto& axis : axes)
    {
        if (axis[0] == 0.0 && axis[1] == 0.0) continue;
        double firstMin = polygon[0].x * axis[0] + polygon[0].y * axis[1];
        double firstMax = firstMin;
        double secondMin = rectangle[0].x * axis[0] + rectangle[0].y * axis[1];
        double secondMax = secondMin;
        for (size_t index = 1; index < 4; ++index)
        {
            const double a = polygon[index].x * axis[0] + polygon[index].y * axis[1];
            const double b = rectangle[index].x * axis[0] + rectangle[index].y * axis[1];
            firstMin = (std::min)(firstMin, a); firstMax = (std::max)(firstMax, a);
            secondMin = (std::min)(secondMin, b); secondMax = (std::max)(secondMax, b);
        }
        if (firstMax <= secondMin || secondMax <= firstMin) return false;
    }
    return true;
}
inline MiniGdi::Color AlphaOver(MiniGdi::Color source, MiniGdi::Color destination,
    unsigned constantAlpha, bool perPixelAlpha)
{
    const unsigned alpha = perPixelAlpha ? MiniGdi::Alpha(source) : 255u;
    const unsigned effective = (alpha * constantAlpha + 127u) / 255u;
    const unsigned inverse = 255u - effective;
    const auto channel = [&](unsigned sourceChannel, unsigned destinationChannel)
    {
        const unsigned contribution = (sourceChannel * (perPixelAlpha ? constantAlpha : effective) + 127u) / 255u;
        return static_cast<std::uint8_t>((std::min)(255u, contribution + (destinationChannel * inverse + 127u) / 255u));
    };
    return MiniGdi::MakeColor(channel(MiniGdi::Red(source), MiniGdi::Red(destination)),
        channel(MiniGdi::Green(source), MiniGdi::Green(destination)),
        channel(MiniGdi::Blue(source), MiniGdi::Blue(destination)),
        static_cast<std::uint8_t>((std::min)(255u, effective + (MiniGdi::Alpha(destination) * inverse + 127u) / 255u)));
}

} } }