#include "pch.h"
#include "Bridge\\User32Shims.h"
#include "Bridge\\Win32Shims.h"

#include "Bridge\\GuestWindow.h"
#include "Bridge\\Kernel32Shims.h"
#include "Bridge\\MiniGdi.h"
#include "Bridge/GuestMetrics.h"
#include "Bridge/GuestResources.h"
#include "Bridge/GuestStorage.h"
#include "Bridge/DialogResources.h"
#include "Bridge/RuntimeDiagnostics.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <cwchar>
#include <cwctype>
#include <limits>
#include <mutex>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>
#include <wincodec.h>
#include <wrl/client.h>

using namespace Win32Bridge::Bridge;

namespace
{
    constexpr DWORD BitmapCompressionAlphaBitFields = 6;
    constexpr DWORD GuestBitmapV4HeaderSize = 108;
    constexpr size_t GuestBitmapV4RedMaskOffset = 40;
    constexpr UINT DrawIconMask = 0x0001;
    constexpr UINT DrawIconImage = 0x0002;
    constexpr UINT DrawIconNormal = DrawIconMask | DrawIconImage;
    constexpr int MaximumGuestSystemColor = 30;
    constexpr ULONG_PTR GuestCursorToken = 0x7fff1000;
    constexpr ULONG_PTR GuestIconToken = 0x7fff2000;
    ULONG_PTR g_nextGuestCursor = GuestCursorToken + 1;
    ULONG_PTR g_nextGuestIcon = GuestIconToken + 1;
    std::mutex g_iconLock;
    std::unordered_map<ULONG_PTR, MiniGdi::Surface> g_guestIcons;
    struct GuestCursorRecord final
    {
        MiniGdi::Surface image;
        POINT hotspot{};
        DWORD systemIdentifier = 0;
    };
    std::mutex g_cursorLock;
    std::unordered_map<ULONG_PTR, GuestCursorRecord> g_guestCursors;

#pragma pack(push, 2)
    struct GuestBitmapFileHeader final
    {
        WORD type;
        DWORD size;
        WORD reserved1;
        WORD reserved2;
        DWORD pixelOffset;
    };
#pragma pack(pop)

    struct GuestBitmapCoreHeader final
    {
        DWORD size;
        WORD width;
        WORD height;
        WORD planes;
        WORD bitCount;
    };

    static_assert(sizeof(GuestBitmapFileHeader) == 14, "BITMAPFILEHEADER ABI mismatch");
    static_assert(sizeof(GuestBitmapCoreHeader) == 12, "BITMAPCOREHEADER ABI mismatch");

    bool IsUserLibrary(const std::wstring& library)
    {
        return _wcsicmp(library.c_str(), L"user32.dll") == 0 ||
            _wcsnicmp(library.c_str(), L"api-ms-win-ntuser-", 18) == 0 ||
            _wcsnicmp(library.c_str(), L"ext-ms-win-ntuser-", 18) == 0;
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

    MiniGdi::ObjectHandle FromGuestObject(HGDIOBJ object)
    {
        const ULONG_PTR raw = reinterpret_cast<ULONG_PTR>(object);
        if (raw == 0 || raw > static_cast<ULONG_PTR>((std::numeric_limits<MiniGdi::ObjectHandle>::max)()))
        {
            return MiniGdi::InvalidObject;
        }
        return static_cast<MiniGdi::ObjectHandle>(raw);
    }

    // CallWindowProcW is also a guest-to-guest callback. Keep the SEH scope
    // free of C++ objects so a broken subclass procedure cannot terminate the
    // UWP host while handling a navigation/control notification.
    LRESULT InvokeGuestSubclassProcedure(
        GuestAbi::WndProc procedure,
        HWND window,
        UINT message,
        WPARAM wParam,
        LPARAM lParam,
        DWORD* exceptionCode)
    {
        if (exceptionCode)
        {
            *exceptionCode = ERROR_SUCCESS;
        }
        __try
        {
            return procedure(window, message, wParam, lParam);
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            if (exceptionCode)
            {
                *exceptionCode = GetExceptionCode();
            }
            return 0;
        }
    }

    bool ReadGuestMessageValue(
        const GuestAbi::Message* source,
        GuestAbi::Message* destination)
    {
        if (!source || !destination) return false;
        __try
        {
            *destination = *source;
            return true;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return false;
        }
    }

    bool IsGuestSystemColor(int color)
    {
        return color >= 0 && color <= MaximumGuestSystemColor;
    }

    MiniGdi::Color GuestSystemColor(int color)
    {
        // Stable colors make traditional Win32 code deterministic on Xbox and
        // avoid exposing a host theme object through a guest COLORREF.
        switch (color)
        {
        case 1:  // COLOR_BACKGROUND
        case 6:  // COLOR_WINDOWFRAME
        case 7:  // COLOR_MENUTEXT
        case 8:  // COLOR_WINDOWTEXT
        case 18: // COLOR_BTNTEXT
            return MiniGdi::OpaqueBlack;
        case 9:  // COLOR_CAPTIONTEXT
        case 14: // COLOR_HIGHLIGHTTEXT
        case 20: // COLOR_BTNHIGHLIGHT
            return MiniGdi::OpaqueWhite;
        case 13: // COLOR_HIGHLIGHT
        case 26: // COLOR_HOTLIGHT
            return MiniGdi::MakeColor(0, 120, 215);
        case 16: // COLOR_BTNSHADOW
        case 17: // COLOR_GRAYTEXT
        case 21: // COLOR_3DDKSHADOW
            return MiniGdi::MakeColor(128, 128, 128);
        case 15: // COLOR_BTNFACE / COLOR_3DFACE
        case 22: // COLOR_3DLIGHT
        case 24: // COLOR_INFOBK
            return MiniGdi::MakeColor(240, 240, 240);
        case 0:  // COLOR_SCROLLBAR
        case 2:  // COLOR_ACTIVECAPTION
        case 3:  // COLOR_INACTIVECAPTION
        case 4:  // COLOR_MENU
        case 10: // COLOR_ACTIVEBORDER
        case 11: // COLOR_INACTIVEBORDER
        case 12: // COLOR_APPWORKSPACE
        case 19: // COLOR_INACTIVECAPTIONTEXT
        case 23: // COLOR_3DHILIGHT
        case 25: // COLOR_INFOTEXT
        case 27: // COLOR_GRADIENTACTIVECAPTION
        case 28: // COLOR_GRADIENTINACTIVECAPTION
        case 29: // COLOR_MENUHILIGHT
        case 30: // COLOR_MENUBAR
        case 5:  // COLOR_WINDOW
        default:
            return MiniGdi::OpaqueWhite;
        }
    }

    COLORREF GuestColorRef(int color)
    {
        const MiniGdi::Color value = GuestSystemColor(color);
        return static_cast<COLORREF>(MiniGdi::Red(value)) |
            (static_cast<COLORREF>(MiniGdi::Green(value)) << 8) |
            (static_cast<COLORREF>(MiniGdi::Blue(value)) << 16);
    }

    bool DecodeEncodedImage(
        const BYTE* data,
        size_t size,
        MiniGdi::Surface* image,
        int requestedWidth = 0,
        int requestedHeight = 0)
    {
        using Microsoft::WRL::ComPtr;
        if (!data || !image || size == 0 || size > MAXDWORD)
        {
            return false;
        }
        ComPtr<IWICImagingFactory> factory;
        HRESULT result = ::CoCreateInstance(
            CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
            IID_PPV_ARGS(&factory));
        if (FAILED(result)) return false;
        ComPtr<IWICStream> stream;
        if (FAILED(factory->CreateStream(&stream)) ||
            FAILED(stream->InitializeFromMemory(const_cast<BYTE*>(data), static_cast<DWORD>(size))))
        {
            return false;
        }
        ComPtr<IWICBitmapDecoder> decoder;
        if (FAILED(factory->CreateDecoderFromStream(
            stream.Get(), nullptr, WICDecodeMetadataCacheOnLoad, &decoder))) return false;
        UINT frameCount = 0;
        if (FAILED(decoder->GetFrameCount(&frameCount)) || frameCount == 0) return false;
        ComPtr<IWICBitmapFrameDecode> frame;
        int bestDistance = (std::numeric_limits<int>::max)();
        for (UINT index = 0; index < frameCount; ++index)
        {
            ComPtr<IWICBitmapFrameDecode> candidate;
            UINT candidateWidth = 0;
            UINT candidateHeight = 0;
            if (FAILED(decoder->GetFrame(index, &candidate)) ||
                FAILED(candidate->GetSize(&candidateWidth, &candidateHeight)) ||
                candidateWidth == 0 || candidateHeight == 0) continue;
            const int targetWidth = requestedWidth > 0 ? requestedWidth : static_cast<int>(candidateWidth);
            const int targetHeight = requestedHeight > 0 ? requestedHeight : static_cast<int>(candidateHeight);
            const int distance = abs(static_cast<int>(candidateWidth) - targetWidth) +
                abs(static_cast<int>(candidateHeight) - targetHeight);
            if (!frame || distance < bestDistance)
            {
                frame = std::move(candidate);
                bestDistance = distance;
            }
        }
        if (!frame) return false;
        UINT width = 0;
        UINT height = 0;
        if (FAILED(frame->GetSize(&width, &height)) || width == 0 || height == 0 ||
            width > 4096 || height > 4096) return false;
        ComPtr<IWICFormatConverter> converter;
        if (FAILED(factory->CreateFormatConverter(&converter)) ||
            FAILED(converter->Initialize(frame.Get(), GUID_WICPixelFormat32bppBGRA,
                WICBitmapDitherTypeNone, nullptr, 0.0, WICBitmapPaletteTypeCustom)) ||
            !image->Resize(static_cast<int>(width), static_cast<int>(height), MiniGdi::Transparent))
        {
            return false;
        }
        const size_t stride = static_cast<size_t>(width) * sizeof(MiniGdi::Color);
        const size_t bytes = stride * height;
        return bytes <= MAXDWORD && SUCCEEDED(converter->CopyPixels(
            nullptr, static_cast<UINT>(stride), static_cast<UINT>(bytes),
            reinterpret_cast<BYTE*>(image->Data())));
    }

    BYTE ExpandMaskedChannel(DWORD value, DWORD mask)
    {
        if (mask == 0) return 0;
        unsigned shift = 0;
        while (((mask >> shift) & 1u) == 0u && shift < 31) ++shift;
        const DWORD normalizedMask = mask >> shift;
        const DWORD component = (value & mask) >> shift;
        return static_cast<BYTE>((component * 255u + normalizedMask / 2u) / normalizedMask);
    }

    bool DecodeBitmapResourceWithWic(
        const BYTE* dib,
        size_t dibSize,
        size_t pixelOffset,
        MiniGdi::Surface* image)
    {
        if (!dib || !image || pixelOffset > dibSize ||
            dibSize > MAXDWORD - sizeof(GuestBitmapFileHeader)) return false;
        std::vector<BYTE> file(sizeof(GuestBitmapFileHeader) + dibSize);
        GuestBitmapFileHeader header{};
        header.type = 0x4d42;
        header.size = static_cast<DWORD>(file.size());
        header.pixelOffset = static_cast<DWORD>(sizeof(header) + pixelOffset);
        memcpy(file.data(), &header, sizeof(header));
        memcpy(file.data() + sizeof(header), dib, dibSize);
        return DecodeEncodedImage(file.data(), file.size(), image);
    }

    bool DecodeDeviceIndependentBitmap(const BYTE* data, size_t size, bool iconBitmap, MiniGdi::Surface* image)
    {
        if (!data || !image || size < sizeof(DWORD)) return false;
        static const BYTE PngSignature[] = { 0x89, 'P', 'N', 'G', 0x0d, 0x0a, 0x1a, 0x0a };
        if (size >= sizeof(PngSignature) && memcmp(data, PngSignature, sizeof(PngSignature)) == 0)
        {
            return DecodeEncodedImage(data, size, image);
        }

        DWORD headerSize = 0;
        memcpy(&headerSize, data, sizeof(headerSize));
        bool coreHeader = headerSize == sizeof(GuestBitmapCoreHeader);
        LONG rawWidth = 0;
        LONG rawHeight = 0;
        WORD bitCount = 0;
        DWORD compression = BI_RGB;
        DWORD colorsUsed = 0;
        bool topDown = false;
        if (coreHeader)
        {
            if (size < sizeof(GuestBitmapCoreHeader)) return false;
            GuestBitmapCoreHeader core{};
            memcpy(&core, data, sizeof(core));
            rawWidth = core.width;
            rawHeight = core.height;
            bitCount = core.bitCount;
        }
        else
        {
            if (headerSize < sizeof(BITMAPINFOHEADER) || headerSize > size) return false;
            BITMAPINFOHEADER header{};
            memcpy(&header, data, sizeof(header));
            rawWidth = header.biWidth;
            rawHeight = header.biHeight;
            bitCount = header.biBitCount;
            compression = header.biCompression;
            colorsUsed = header.biClrUsed;
            topDown = rawHeight < 0;
        }
        if (rawWidth <= 0 || rawHeight == 0 || rawHeight == LONG_MIN ||
            (bitCount != 1 && bitCount != 4 && bitCount != 8 && bitCount != 16 &&
             bitCount != 24 && bitCount != 32) ||
            (compression != BI_RGB && compression != BI_BITFIELDS && compression != BitmapCompressionAlphaBitFields &&
             compression != BI_RLE4 && compression != BI_RLE8 &&
             compression != BI_JPEG && compression != BI_PNG) ||
            (compression == BI_RLE4 && bitCount != 4) ||
            (compression == BI_RLE8 && bitCount != 8))
        {
            return false;
        }

        const int width = rawWidth;
        const int totalHeight = rawHeight < 0 ? -rawHeight : rawHeight;
        const int height = iconBitmap ? totalHeight / 2 : totalHeight;
        if (height <= 0 || width > 4096 || height > 4096 ||
            !image->Resize(width, height, MiniGdi::Transparent)) return false;

        size_t cursor = headerSize;
        DWORD redMask = bitCount == 16 ? 0x7c00u : 0x00ff0000u;
        DWORD greenMask = bitCount == 16 ? 0x03e0u : 0x0000ff00u;
        DWORD blueMask = bitCount == 16 ? 0x001fu : 0x000000ffu;
        DWORD alphaMask = 0;
        if (!coreHeader && (compression == BI_BITFIELDS || compression == BitmapCompressionAlphaBitFields))
        {
            if (headerSize >= GuestBitmapV4HeaderSize)
            {
                memcpy(&redMask, data + GuestBitmapV4RedMaskOffset, sizeof(redMask));
                memcpy(&greenMask, data + GuestBitmapV4RedMaskOffset + sizeof(DWORD), sizeof(greenMask));
                memcpy(&blueMask, data + GuestBitmapV4RedMaskOffset + 2 * sizeof(DWORD), sizeof(blueMask));
                memcpy(&alphaMask, data + GuestBitmapV4RedMaskOffset + 3 * sizeof(DWORD), sizeof(alphaMask));
            }
            else
            {
                const size_t maskCount = compression == BitmapCompressionAlphaBitFields ? 4 : 3;
                if (cursor > size || maskCount * sizeof(DWORD) > size - cursor) return false;
                const DWORD* masks = reinterpret_cast<const DWORD*>(data + cursor);
                redMask = masks[0]; greenMask = masks[1]; blueMask = masks[2];
                if (maskCount == 4) alphaMask = masks[3];
                cursor += maskCount * sizeof(DWORD);
            }
        }

        const size_t paletteEntries = bitCount <= 8
            ? (colorsUsed ? colorsUsed : (static_cast<size_t>(1) << bitCount)) : 0;
        const size_t paletteEntrySize = coreHeader ? sizeof(RGBTRIPLE) : sizeof(RGBQUAD);
        if (paletteEntries > 256 || cursor > size ||
            paletteEntries * paletteEntrySize > size - cursor) return false;
        const BYTE* palette = data + cursor;
        cursor += paletteEntries * paletteEntrySize;

        if (!iconBitmap && (compression == BI_RLE4 || compression == BI_RLE8))
            return DecodeBitmapResourceWithWic(data, size, cursor, image);
        if (!iconBitmap && (compression == BI_JPEG || compression == BI_PNG))
            return cursor < size && DecodeEncodedImage(data + cursor, size - cursor, image);

        const unsigned long long rowBits = static_cast<unsigned long long>(width) * bitCount;
        const size_t xorStride = static_cast<size_t>(((rowBits + 31ull) / 32ull) * 4ull);
        if (xorStride == 0 || static_cast<size_t>(height) > (std::numeric_limits<size_t>::max)() / xorStride) return false;
        const size_t xorBytes = xorStride * static_cast<size_t>(height);
        if (cursor > size || xorBytes > size - cursor) return false;
        const BYTE* bits = data + cursor;
        const size_t andStride = static_cast<size_t>(((static_cast<unsigned long long>(width) + 31ull) / 32ull) * 4ull);
        const size_t andBytes = andStride * static_cast<size_t>(height);
        const BYTE* andBits = iconBitmap && andBytes <= size - cursor - xorBytes
            ? bits + xorBytes : nullptr;

        bool hasMeaningfulAlpha = false;
        if (bitCount == 32 && alphaMask == 0 && iconBitmap)
        {
            for (int y = 0; y < height && !hasMeaningfulAlpha; ++y)
            {
                const BYTE* row = bits + static_cast<size_t>(y) * xorStride;
                for (int x = 0; x < width; ++x)
                {
                    if (row[static_cast<size_t>(x) * 4 + 3] != 0)
                    {
                        hasMeaningfulAlpha = true;
                        break;
                    }
                }
            }
        }

        for (int y = 0; y < height; ++y)
        {
            const int sourceY = topDown ? y : height - 1 - y;
            const BYTE* row = bits + static_cast<size_t>(sourceY) * xorStride;
            for (int x = 0; x < width; ++x)
            {
                BYTE red = 0, green = 0, blue = 0, alpha = 255;
                if (bitCount == 32)
                {
                    DWORD pixel = 0;
                    memcpy(&pixel, row + static_cast<size_t>(x) * 4, sizeof(pixel));
                    red = ExpandMaskedChannel(pixel, redMask);
                    green = ExpandMaskedChannel(pixel, greenMask);
                    blue = ExpandMaskedChannel(pixel, blueMask);
                    alpha = alphaMask ? ExpandMaskedChannel(pixel, alphaMask)
                        : (hasMeaningfulAlpha ? static_cast<BYTE>(pixel >> 24) : 255);
                }
                else if (bitCount == 24)
                {
                    const BYTE* pixel = row + static_cast<size_t>(x) * 3;
                    blue = pixel[0]; green = pixel[1]; red = pixel[2];
                }
                else if (bitCount == 16)
                {
                    WORD packed = 0;
                    memcpy(&packed, row + static_cast<size_t>(x) * 2, sizeof(packed));
                    red = ExpandMaskedChannel(packed, redMask);
                    green = ExpandMaskedChannel(packed, greenMask);
                    blue = ExpandMaskedChannel(packed, blueMask);
                    if (alphaMask) alpha = ExpandMaskedChannel(packed, alphaMask);
                }
                else
                {
                    BYTE index = 0;
                    if (bitCount == 8) index = row[x];
                    else if (bitCount == 4)
                        index = static_cast<BYTE>((row[x / 2] >> ((x & 1) ? 0 : 4)) & 0x0f);
                    else
                        index = static_cast<BYTE>((row[x / 8] >> (7 - (x & 7))) & 1);
                    if (index >= paletteEntries) return false;
                    if (coreHeader)
                    {
                        const RGBTRIPLE* color = reinterpret_cast<const RGBTRIPLE*>(palette) + index;
                        blue = color->rgbtBlue; green = color->rgbtGreen; red = color->rgbtRed;
                    }
                    else
                    {
                        const RGBQUAD* color = reinterpret_cast<const RGBQUAD*>(palette) + index;
                        blue = color->rgbBlue; green = color->rgbGreen; red = color->rgbRed;
                    }
                }
                if (andBits)
                {
                    const BYTE* maskRow = andBits + static_cast<size_t>(sourceY) * andStride;
                    if ((maskRow[x / 8] & (0x80u >> (x & 7))) != 0)
                    {
                        alpha = 0;
                    }
                }
                MiniGdi::Color* output = image->PixelAt(x, y);
                if (!output) return false;
                *output = MiniGdi::MakeColor(red, green, blue, alpha);
            }
        }
        return true;
    }

    bool FindModuleResource(
        HINSTANCE instance,
        WORD typeId,
        LPCWSTR name,
        const BYTE** data,
        size_t* size)
    {
        if (!data || !size)
        {
            return false;
        }
        *data = nullptr;
        *size = 0;
        GuestResourceData resource;
        const LPCWSTR type = reinterpret_cast<LPCWSTR>(static_cast<ULONG_PTR>(typeId));
        if (FindGuestResource(
            reinterpret_cast<HMODULE>(instance), type, name, 0, false, &resource) !=
            GuestResourceStatus::Success)
        {
            return false;
        }
        *data = resource.data;
        *size = resource.size;
        return true;
    }

    bool LoadBitmapResourcePixels(HINSTANCE instance, LPCWSTR resource, MiniGdi::Surface* image)
    {
        const BYTE* data = nullptr;
        size_t size = 0;
        return FindModuleResource(instance, 2 /* RT_BITMAP */, resource, &data, &size) &&
            DecodeDeviceIndependentBitmap(data, size, false, image);
    }

    MiniGdi::Surface ScaleSurface(const MiniGdi::Surface& source, int width, int height)
    {
        if (source.Empty() || width <= 0 || height <= 0 ||
            (width == source.Width() && height == source.Height())) return source;
        MiniGdi::Surface scaled(width, height, MiniGdi::Transparent);
        if (scaled.Empty()) return MiniGdi::Surface{};
        for (int y = 0; y < height; ++y)
        {
            const int sourceY = (std::min)(source.Height() - 1,
                static_cast<int>((static_cast<long long>(y) * source.Height()) / height));
            for (int x = 0; x < width; ++x)
            {
                const int sourceX = (std::min)(source.Width() - 1,
                    static_cast<int>((static_cast<long long>(x) * source.Width()) / width));
                MiniGdi::Color* target = scaled.PixelAt(x, y);
                const MiniGdi::Color* pixel = source.PixelAt(sourceX, sourceY);
                if (target && pixel) *target = *pixel;
            }
        }
        return scaled;
    }

    bool LoadIconResourcePixels(
        HINSTANCE instance,
        LPCWSTR resource,
        int requestedWidth,
        int requestedHeight,
        MiniGdi::Surface* image)
    {
        const BYTE* group = nullptr;
        size_t groupSize = 0;
        if (!FindModuleResource(
            instance, 14 /* RT_GROUP_ICON */, resource, &group, &groupSize) || groupSize < 6)
        {
            return false;
        }
        WORD reserved = 0;
        WORD type = 0;
        WORD count = 0;
        memcpy(&reserved, group, sizeof(reserved));
        memcpy(&type, group + 2, sizeof(type));
        memcpy(&count, group + 4, sizeof(count));
        // GRPICONDIR uses type 1 for icons (type 2 belongs to cursors).
        if (reserved != 0 || type != 1 || count == 0 ||
            groupSize < 6 + static_cast<size_t>(count) * 14) return false;
        const BYTE* best = nullptr;
        int bestDistance = (std::numeric_limits<int>::max)();
        WORD bestDepth = 0;
        const int targetWidth = requestedWidth > 0 ? requestedWidth : 32;
        const int targetHeight = requestedHeight > 0 ? requestedHeight : 32;
        for (WORD index = 0; index < count; ++index)
        {
            const BYTE* entry = group + 6 + static_cast<size_t>(index) * 14;
            const int width = entry[0] ? entry[0] : 256;
            const int height = entry[1] ? entry[1] : 256;
            WORD depth = 0;
            memcpy(&depth, entry + 6, sizeof(depth));
            const int distance = abs(width - targetWidth) + abs(height - targetHeight);
            if (distance < bestDistance || (distance == bestDistance && depth > bestDepth))
            {
                best = entry;
                bestDistance = distance;
                bestDepth = depth;
            }
        }
        if (!best) return false;
        WORD iconId = 0;
        memcpy(&iconId, best + 12, sizeof(iconId));
        const BYTE* icon = nullptr;
        size_t iconSize = 0;
        if (!FindModuleResource(instance, 3 /* RT_ICON */,
            reinterpret_cast<LPCWSTR>(static_cast<ULONG_PTR>(iconId)), &icon, &iconSize)) return false;
        MiniGdi::Surface decoded;
        if (!DecodeDeviceIndependentBitmap(icon, iconSize, true, &decoded)) return false;
        *image = ScaleSurface(decoded, requestedWidth > 0 ? requestedWidth : decoded.Width(),
            requestedHeight > 0 ? requestedHeight : decoded.Height());
        return !image->Empty();
    }

    bool LoadCursorResourcePixels(
        HINSTANCE instance,
        LPCWSTR resource,
        int requestedWidth,
        int requestedHeight,
        MiniGdi::Surface* image,
        POINT* hotspot)
    {
        if (!image || !hotspot) return false;
        const BYTE* group = nullptr;
        size_t groupSize = 0;
        if (!FindModuleResource(
            instance, 12 /* RT_GROUP_CURSOR */, resource, &group, &groupSize) || groupSize < 6)
        {
            return false;
        }
        WORD reserved = 0;
        WORD type = 0;
        WORD count = 0;
        memcpy(&reserved, group, sizeof(reserved));
        memcpy(&type, group + 2, sizeof(type));
        memcpy(&count, group + 4, sizeof(count));
        // GRPCURSORDIR keeps the CUR format type value (2).
        if (reserved != 0 || type != 2 || count == 0 ||
            groupSize < 6 + static_cast<size_t>(count) * 14) return false;
        const int targetWidth = requestedWidth > 0 ? requestedWidth : 32;
        const int targetHeight = requestedHeight > 0 ? requestedHeight : 32;
        const BYTE* best = nullptr;
        int bestDistance = (std::numeric_limits<int>::max)();
        WORD bestDepth = 0;
        for (WORD index = 0; index < count; ++index)
        {
            const BYTE* entry = group + 6 + static_cast<size_t>(index) * 14;
            WORD width = 0;
            WORD height = 0;
            memcpy(&width, entry, sizeof(width));
            memcpy(&height, entry + 2, sizeof(height));
            WORD depth = 0;
            memcpy(&depth, entry + 6, sizeof(depth));
            if (height == width * 2) height /= 2;
            const int distance = abs(static_cast<int>(width) - targetWidth) +
                abs(static_cast<int>(height) - targetHeight);
            if (distance < bestDistance || (distance == bestDistance && depth > bestDepth))
            {
                best = entry;
                bestDistance = distance;
                bestDepth = depth;
            }
        }
        if (!best) return false;
        WORD cursorId = 0;
        memcpy(&cursorId, best + 12, sizeof(cursorId));
        const BYTE* cursor = nullptr;
        size_t cursorSize = 0;
        if (!FindModuleResource(instance, 1 /* RT_CURSOR */,
            reinterpret_cast<LPCWSTR>(static_cast<ULONG_PTR>(cursorId)),
            &cursor, &cursorSize) || cursorSize < 4) return false;
        SHORT x = 0;
        SHORT y = 0;
        memcpy(&x, cursor, sizeof(x));
        memcpy(&y, cursor + sizeof(x), sizeof(y));
        MiniGdi::Surface decoded;
        if (!DecodeDeviceIndependentBitmap(cursor + 4, cursorSize - 4, true, &decoded)) return false;
        const int outputWidth = requestedWidth > 0 ? requestedWidth : decoded.Width();
        const int outputHeight = requestedHeight > 0 ? requestedHeight : decoded.Height();
        *image = ScaleSurface(decoded, outputWidth, outputHeight);
        if (image->Empty()) return false;
        hotspot->x = decoded.Width() > 0 ? x * outputWidth / decoded.Width() : x;
        hotspot->y = decoded.Height() > 0 ? y * outputHeight / decoded.Height() : y;
        return true;
    }

    HICON StoreGuestIcon(MiniGdi::Surface image)
    {
        if (image.Empty()) return nullptr;
        std::lock_guard<std::mutex> guard(g_iconLock);
        const ULONG_PTR token = g_nextGuestIcon++;
        g_guestIcons.emplace(token, std::move(image));
        return reinterpret_cast<HICON>(token);
    }

    HCURSOR StoreGuestCursor(MiniGdi::Surface image, POINT hotspot)
    {
        if (image.Empty()) return nullptr;
        GuestCursorRecord record;
        record.image = std::move(image);
        record.hotspot = hotspot;
        std::lock_guard<std::mutex> guard(g_cursorLock);
        const ULONG_PTR token = g_nextGuestCursor++;
        g_guestCursors.emplace(token, std::move(record));
        return reinterpret_cast<HCURSOR>(token);
    }

    HBITMAP CreateGuestBitmapFromSurface(const MiniGdi::Surface& image)
    {
        GuestWindowManager* manager = CurrentGuestWindowManager();
        if (!manager || image.Empty()) return nullptr;
        const MiniGdi::ObjectHandle bitmap = manager->Gdi().CreateBitmap(
            image.Width(), image.Height(), MiniGdi::Transparent);
        MiniGdi::Surface* target = manager->Gdi().GetBitmapSurface(bitmap);
        if (bitmap == MiniGdi::InvalidObject || !target) return nullptr;
        target->Pixels() = image.Pixels();
        return reinterpret_cast<HBITMAP>(static_cast<ULONG_PTR>(bitmap));
    }

    bool CopyGuestByteRange(const BYTE* source, size_t size, std::vector<BYTE>* copy)
    {
        if (!source || !copy || size == 0 || size > 128u * 1024u * 1024u) return false;
        copy->resize(size);
        __try
        {
            memcpy(copy->data(), source, size);
            return true;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            copy->clear();
            return false;
        }
    }

    bool ConvertAnsiResourceName(LPCSTR source, std::wstring* storage, LPCWSTR* converted)
    {
        if (!source || !storage || !converted) return false;
        const ULONG_PTR raw = reinterpret_cast<ULONG_PTR>(source);
        if (raw <= 0xffff)
        {
            *converted = reinterpret_cast<LPCWSTR>(raw);
            return true;
        }
        const int count = ::MultiByteToWideChar(CP_ACP, 0, source, -1, nullptr, 0);
        if (count <= 0) return false;
        storage->resize(static_cast<size_t>(count));
        if (::MultiByteToWideChar(CP_ACP, 0, source, -1, &(*storage)[0], count) != count)
        {
            storage->clear();
            return false;
        }
        storage->resize(static_cast<size_t>(count - 1));
        *converted = storage->c_str();
        return true;
    }

    // Wire layout of MENUITEMINFOW.  It belongs to the guest ABI, rather than
    // the UWP SDK, and is deliberately kept local to the menu translator.
    struct GuestMenuItemInfoW
    {
        UINT cbSize;
        UINT fMask;
        UINT fType;
        UINT fState;
        UINT wID;
        HMENU hSubMenu;
        HBITMAP hbmpChecked;
        HBITMAP hbmpUnchecked;
        ULONG_PTR dwItemData;
        LPWSTR dwTypeData;
        UINT cch;
        HBITMAP hbmpItem;
    };

    constexpr size_t GuestMenuItemInfoLegacySize = offsetof(GuestMenuItemInfoW, hbmpItem);

    bool ReadGuestMenuItemInfo(const void* source, GuestMenuItemInfoW* destination)
    {
        if (!source || !destination || reinterpret_cast<ULONG_PTR>(source) <= 0xffff) return false;
        __try
        {
            const UINT size = *static_cast<const UINT*>(source);
            if (size < GuestMenuItemInfoLegacySize) return false;
            memset(destination, 0, sizeof(*destination));
            memcpy(destination, source, (std::min)(static_cast<size_t>(size), sizeof(*destination)));
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return false;
        }
        return true;
    }

    bool WriteGuestMenuItemInfo(void* destination, const GuestMenuItemInfoW& source)
    {
        if (!destination || reinterpret_cast<ULONG_PTR>(destination) <= 0xffff) return false;
        __try
        {
            const UINT size = *static_cast<const UINT*>(destination);
            if (size < GuestMenuItemInfoLegacySize) return false;
            memcpy(destination, &source,
                (std::min)(static_cast<size_t>(size), sizeof(source)));
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return false;
        }
        return true;
    }

    struct GuestWindowPlacement
    {
        UINT length;
        UINT flags;
        UINT showCmd;
        POINT minPosition;
        POINT maxPosition;
        RECT normalPosition;
    };

    struct VirtualMenuItem
    {
        UINT identifier = 0;
        UINT type = 0;
        UINT state = 0;
        HMENU subMenu = nullptr;
        HBITMAP checkedBitmap = nullptr;
        HBITMAP uncheckedBitmap = nullptr;
        HBITMAP itemBitmap = nullptr;
        ULONG_PTR itemData = 0;
        std::wstring text;
    };

    struct VirtualMenu
    {
        std::vector<VirtualMenuItem> items;
    };

    constexpr UINT kMfByPosition = 0x0400;
    constexpr UINT kMfPopup = 0x0010;
    constexpr UINT kMfGrayed = 0x0001;
    constexpr UINT kMfChecked = 0x0008;
    constexpr UINT kMfDisabled = 0x0002;
    constexpr UINT kMfHighlighted = 0x0080;
    constexpr UINT kMfDefault = 0x1000;
    constexpr UINT kMfSeparator = 0x0800;
    constexpr UINT kMftRadioCheck = 0x0200;
    constexpr UINT kMiimState = 0x0001;
    constexpr UINT kMiimId = 0x0002;
    constexpr UINT kMiimSubmenu = 0x0004;
    constexpr UINT kMiimCheckmarks = 0x0008;
    // Older callers commonly use MIIM_TYPE together with MFT_STRING instead
    // of the newer MIIM_STRING spelling.  Both describe dwTypeData.
    constexpr UINT kMiimType = 0x0010;
    constexpr UINT kMiimFtype = 0x0100;
    constexpr UINT kMiimString = 0x0040;
    constexpr UINT kMiimData = 0x0020;
    constexpr UINT kMiimBitmap = 0x0080;

    bool ProbeGuestMenuText(LPCWSTR source, size_t maximum, size_t* length)
    {
        if (!length)
        {
            return false;
        }
        *length = 0;
        if (!source || reinterpret_cast<ULONG_PTR>(source) <= 0xffff)
        {
            return source == nullptr;
        }
        __try
        {
            while (*length < maximum && source[*length] != L'\0') ++*length;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            *length = 0;
            return false;
        }
        return *length < maximum;
    }

    bool CopyGuestMenuText(LPWSTR destination, size_t capacity, const std::wstring& source)
    {
        if (!destination || capacity == 0 || reinterpret_cast<ULONG_PTR>(destination) <= 0xffff)
        {
            return false;
        }
        const size_t count = (std::min)(source.size(), capacity - 1);
        __try
        {
            if (count) memcpy(destination, source.data(), count * sizeof(wchar_t));
            destination[count] = L'\0';
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return false;
        }
        return true;
    }

    bool CopyGuestMenuCharacters(LPCWSTR source, wchar_t* destination, size_t count)
    {
        if (!source || !destination || reinterpret_cast<ULONG_PTR>(source) <= 0xffff)
        {
            return false;
        }
        __try
        {
            if (count) memcpy(destination, source, count * sizeof(wchar_t));
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return false;
        }
        return true;
    }

    std::wstring ReadGuestMenuText(const GuestMenuItemInfoW& source)
    {
        if (!source.dwTypeData)
        {
            return {};
        }

        // cch is a retrieval-buffer size in several otherwise-valid menu
        // construction paths.  A zero value therefore means that dwTypeData
        // is the usual NUL-terminated menu caption, not an empty caption.
        constexpr size_t MaximumMenuTextLength = 32768;
        const size_t requested = source.cch
            ? (std::min)(static_cast<size_t>(source.cch), MaximumMenuTextLength)
            : MaximumMenuTextLength;
        size_t length = 0;
        if (!ProbeGuestMenuText(source.dwTypeData, requested, &length))
        {
            // MIIM_STRING setters may provide cch without a trailing NUL. In
            // that form the explicit count is authoritative, but the memory
            // still has to be readable before it enters std::wstring.
            if (!source.cch || length != requested)
                return {};
        }
        std::vector<wchar_t> characters(length);
        if (characters.empty()) return {};
        if (length && !CopyGuestMenuCharacters(source.dwTypeData, characters.data(), length)) return {};
        return std::wstring(characters.data(), characters.size());
    }

    std::mutex g_menuLock;
    std::unordered_map<ULONG_PTR, VirtualMenu> g_menus;
    std::unordered_map<ULONG_PTR, HMENU> g_windowMenus;
    ULONG_PTR g_nextMenu = 0x7fff5000;
    UINT g_nextClipboardFormat = 0xc000;

    VirtualMenu* FindMenuLocked(HMENU menu)
    {
        const auto found = g_menus.find(reinterpret_cast<ULONG_PTR>(menu));
        return found == g_menus.end() ? nullptr : &found->second;
    }

    VirtualMenuItem* FindMenuItemLocked(VirtualMenu& menu, UINT item, BOOL byPosition)
    {
        if (byPosition)
        {
            return item < menu.items.size() ? &menu.items[item] : nullptr;
        }
        for (auto& candidate : menu.items)
        {
            if (candidate.identifier == item) return &candidate;
        }
        return nullptr;
    }

    constexpr WORD MenuResourceType = 4; // RT_MENU
    constexpr WORD MenuFlagPopup = 0x0010;
    constexpr WORD MenuFlagEnd = 0x0080;

    bool ReadMenuWord(const BYTE* data, size_t size, size_t* offset, WORD* value)
    {
        if (!data || !offset || !value || *offset > size || size - *offset < sizeof(*value))
        {
            return false;
        }
        memcpy(value, data + *offset, sizeof(*value));
        *offset += sizeof(*value);
        return true;
    }

    bool ReadMenuDword(const BYTE* data, size_t size, size_t* offset, DWORD* value)
    {
        if (!data || !offset || !value || *offset > size || size - *offset < sizeof(*value))
        {
            return false;
        }
        memcpy(value, data + *offset, sizeof(*value));
        *offset += sizeof(*value);
        return true;
    }

    bool AlignMenuOffset(size_t* offset, size_t size, size_t alignment)
    {
        if (!offset || alignment == 0)
        {
            return false;
        }
        const size_t aligned = (*offset + alignment - 1) & ~(alignment - 1);
        if (aligned < *offset || aligned > size)
        {
            return false;
        }
        *offset = aligned;
        return true;
    }

    bool ReadMenuString(const BYTE* data, size_t size, size_t* offset, std::wstring* text)
    {
        if (!data || !offset || !text || *offset > size)
        {
            return false;
        }
        text->clear();
        while (*offset <= size && size - *offset >= sizeof(WORD))
        {
            WORD character = 0;
            if (!ReadMenuWord(data, size, offset, &character))
            {
                return false;
            }
            if (character == 0)
            {
                return true;
            }
            text->push_back(static_cast<wchar_t>(character));
        }
        return false;
    }

    bool ParseStandardMenuItems(const BYTE* data, size_t size, size_t* offset, HMENU destination, unsigned depth)
    {
        if (!destination || depth > 16)
        {
            return false;
        }
        for (;;)
        {
            WORD rawFlags = 0;
            if (!ReadMenuWord(data, size, offset, &rawFlags))
            {
                return false;
            }
            const bool end = (rawFlags & MenuFlagEnd) != 0;
            const UINT flags = rawFlags & ~MenuFlagEnd;
            WORD identifier = 0;
            if ((flags & MenuFlagPopup) == 0 && !ReadMenuWord(data, size, offset, &identifier))
            {
                return false;
            }
            std::wstring caption;
            if (!ReadMenuString(data, size, offset, &caption))
            {
                return false;
            }
            if ((flags & MenuFlagPopup) != 0)
            {
                HMENU child = BridgeCreatePopupMenu();
                if (!child || !ParseStandardMenuItems(data, size, offset, child, depth + 1) ||
                    !BridgeAppendMenuW(destination, flags, reinterpret_cast<UINT_PTR>(child), caption.c_str()))
                {
                    if (child) BridgeDestroyMenu(child);
                    return false;
                }
            }
            else if (!BridgeAppendMenuW(destination, flags, identifier, caption.empty() ? nullptr : caption.c_str()))
            {
                return false;
            }
            if (end)
            {
                return true;
            }
        }
    }

    bool ParseExtendedMenuItems(const BYTE* data, size_t size, size_t* offset, HMENU destination, unsigned depth)
    {
        if (!destination || depth > 16)
        {
            return false;
        }
        for (;;)
        {
            DWORD type = 0;
            DWORD state = 0;
            DWORD identifier = 0;
            WORD information = 0;
            if (!ReadMenuDword(data, size, offset, &type) || !ReadMenuDword(data, size, offset, &state) ||
                !ReadMenuDword(data, size, offset, &identifier) || !ReadMenuWord(data, size, offset, &information) ||
                !AlignMenuOffset(offset, size, sizeof(WORD)))
            {
                return false;
            }
            std::wstring caption;
            if (!ReadMenuString(data, size, offset, &caption) || !AlignMenuOffset(offset, size, sizeof(DWORD)))
            {
                return false;
            }
            const bool popup = (information & 0x0001) != 0;
            if (popup)
            {
                DWORD ignoredHelpId = 0;
                HMENU child = nullptr;
                if (!ReadMenuDword(data, size, offset, &ignoredHelpId) || !(child = BridgeCreatePopupMenu()) ||
                    !ParseExtendedMenuItems(data, size, offset, child, depth + 1) ||
                    !BridgeAppendMenuW(destination, static_cast<UINT>(type | MenuFlagPopup | state),
                        reinterpret_cast<UINT_PTR>(child), caption.c_str()))
                {
                    if (child) BridgeDestroyMenu(child);
                    return false;
                }
            }
            else if (!BridgeAppendMenuW(destination, static_cast<UINT>(type | state), identifier,
                caption.empty() ? nullptr : caption.c_str()))
            {
                return false;
            }
            if ((information & MenuFlagEnd) != 0)
            {
                return true;
            }
        }
    }

    HMENU LoadGuestMenuResource(HINSTANCE instance, LPCWSTR resource)
    {
        const BYTE* data = nullptr;
        size_t size = 0;
        if (!FindModuleResource(instance, MenuResourceType, resource, &data, &size) || size < 4)
        {
            return nullptr;
        }
        WORD version = 0;
        WORD offset = 0;
        size_t cursor = 0;
        if (!ReadMenuWord(data, size, &cursor, &version) || !ReadMenuWord(data, size, &cursor, &offset) ||
            offset > size - cursor)
        {
            return nullptr;
        }
        cursor += offset;
        HMENU menu = BridgeCreatePopupMenu();
        const bool parsed = menu && (version == 0
            ? ParseStandardMenuItems(data, size, &cursor, menu, 0)
            : version == 1 && ParseExtendedMenuItems(data, size, &cursor, menu, 0));
        if (!parsed)
        {
            if (menu) BridgeDestroyMenu(menu);
            return nullptr;
        }
        return menu;
    }
}

ATOM WINAPI Win32Bridge::Bridge::BridgeRegisterClassExW(const GuestAbi::WndClassExW* windowClass)
{
    GuestWindowManager* manager = CurrentManagerOrFail();
    if (!manager)
    {
        return 0;
    }

    DWORD error = ERROR_SUCCESS;
    const ATOM result = manager->RegisterGuestClass(windowClass, &error);
    BridgeSetLastError(error);
    return result;
}

ATOM WINAPI Win32Bridge::Bridge::BridgeRegisterClassW(const GuestAbi::WndClassW* windowClass)
{
    GuestWindowManager* manager = CurrentManagerOrFail();
    if (!manager)
    {
        return 0;
    }

    DWORD error = ERROR_SUCCESS;
    const ATOM result = manager->RegisterGuestClass(windowClass, &error);
    BridgeSetLastError(error);
    return result;
}

HWND WINAPI Win32Bridge::Bridge::BridgeCreateWindowExW(
    DWORD extendedStyle,
    LPCWSTR className,
    LPCWSTR windowName,
    DWORD style,
    int x,
    int y,
    int width,
    int height,
    HWND parent,
    HMENU menu,
    HINSTANCE instance,
    LPVOID parameter)
{
    GuestWindowManager* manager = CurrentManagerOrFail();
    if (!manager)
    {
        return nullptr;
    }

    DWORD error = ERROR_SUCCESS;
    const HWND result = manager->CreateGuestWindow(
        extendedStyle,
        className,
        windowName,
        style,
        x,
        y,
        width,
        height,
        parent,
        menu,
        instance,
        parameter,
        &error);
    BridgeSetLastError(error);
    return result;
}

HCURSOR WINAPI Win32Bridge::Bridge::BridgeLoadCursorW(HINSTANCE instance, LPCWSTR cursorName)
{
    if (!cursorName)
    {
        BridgeSetLastError(ERROR_INVALID_PARAMETER);
        return nullptr;
    }

    GuestCursorRecord record;
    const ULONG_PTR rawName = reinterpret_cast<ULONG_PTR>(cursorName);
    if (!instance && rawName <= 0xffff)
    {
        record.systemIdentifier = static_cast<DWORD>(rawName);
        std::lock_guard<std::mutex> guard(g_cursorLock);
        for (const auto& existing : g_guestCursors)
        {
            if (existing.second.systemIdentifier == record.systemIdentifier)
            {
                BridgeSetLastError(ERROR_SUCCESS);
                return reinterpret_cast<HCURSOR>(existing.first);
            }
        }
        const ULONG_PTR token = g_nextGuestCursor++;
        g_guestCursors.emplace(token, std::move(record));
        BridgeSetLastError(ERROR_SUCCESS);
        return reinterpret_cast<HCURSOR>(token);
    }

    if (!LoadCursorResourcePixels(instance, cursorName, 0, 0, &record.image, &record.hotspot))
    {
        BridgeSetLastError(ERROR_RESOURCE_NAME_NOT_FOUND);
        return nullptr;
    }
    std::lock_guard<std::mutex> guard(g_cursorLock);
    const ULONG_PTR token = g_nextGuestCursor++;
    g_guestCursors.emplace(token, std::move(record));
    RuntimeDiagnostics::Record(L"CURSOR: decoded RT_GROUP_CURSOR/RT_CURSOR resources.");
    BridgeSetLastError(ERROR_SUCCESS);
    return reinterpret_cast<HCURSOR>(token);
}

HCURSOR WINAPI Win32Bridge::Bridge::BridgeLoadCursorA(HINSTANCE instance, LPCSTR cursorName)
{
    std::wstring storage;
    LPCWSTR converted = nullptr;
    if (!ConvertAnsiResourceName(cursorName, &storage, &converted))
    {
        BridgeSetLastError(ERROR_NO_UNICODE_TRANSLATION);
        return nullptr;
    }
    return BridgeLoadCursorW(instance, converted);
}

HCURSOR WINAPI Win32Bridge::Bridge::BridgeLoadCursorFromFileW(LPCWSTR fileName)
{
    return reinterpret_cast<HCURSOR>(BridgeLoadImageW(
        nullptr, fileName, IMAGE_CURSOR, 0, 0, LR_LOADFROMFILE | LR_DEFAULTSIZE));
}

HCURSOR WINAPI Win32Bridge::Bridge::BridgeLoadCursorFromFileA(LPCSTR fileName)
{
    return reinterpret_cast<HCURSOR>(BridgeLoadImageA(
        nullptr, fileName, IMAGE_CURSOR, 0, 0, LR_LOADFROMFILE | LR_DEFAULTSIZE));
}

HCURSOR WINAPI Win32Bridge::Bridge::BridgeSetCursor(HCURSOR cursor)
{
    static thread_local HCURSOR current = nullptr;
    HCURSOR previous = current;
    DWORD systemIdentifier = 0;
    if (cursor)
    {
        std::lock_guard<std::mutex> guard(g_cursorLock);
        const auto found = g_guestCursors.find(reinterpret_cast<ULONG_PTR>(cursor));
        if (found == g_guestCursors.end())
        {
            BridgeSetLastError(ERROR_INVALID_CURSOR_HANDLE);
            return nullptr;
        }
        systemIdentifier = found->second.systemIdentifier;
    }
    current = cursor;
    try
    {
        using Windows::UI::Core::CoreCursor;
        using Windows::UI::Core::CoreCursorType;
        CoreCursorType type = CoreCursorType::Arrow;
        switch (systemIdentifier)
        {
        case 32513: type = CoreCursorType::IBeam; break;
        case 32514: type = CoreCursorType::Wait; break;
        case 32515: type = CoreCursorType::Cross; break;
        case 32516: type = CoreCursorType::UpArrow; break;
        case 32642: type = CoreCursorType::SizeNorthwestSoutheast; break;
        case 32643: type = CoreCursorType::SizeNortheastSouthwest; break;
        case 32644: type = CoreCursorType::SizeWestEast; break;
        case 32645: type = CoreCursorType::SizeNorthSouth; break;
        case 32646: type = CoreCursorType::SizeAll; break;
        case 32648: type = CoreCursorType::UniversalNo; break;
        case 32649: type = CoreCursorType::Hand; break;
        case 32650: type = CoreCursorType::Wait; break;
        case 32651: type = CoreCursorType::Help; break;
        default: break;
        }
        Windows::UI::Core::CoreWindow^ window = Windows::UI::Core::CoreWindow::GetForCurrentThread();
        if (window) window->PointerCursor = ref new CoreCursor(type, 0);
    }
    catch (Platform::Exception^)
    {
        // Keep the guest cursor state even when the current UWP surface does
        // not expose a mutable hardware cursor (notably controller-only Xbox).
    }
    BridgeSetLastError(ERROR_SUCCESS);
    return previous;
}

HICON WINAPI Win32Bridge::Bridge::BridgeLoadIconW(HINSTANCE instance, LPCWSTR iconName)
{
    if (!iconName)
    {
        BridgeSetLastError(ERROR_INVALID_PARAMETER);
        return nullptr;
    }

    MiniGdi::Surface image;
    if (LoadIconResourcePixels(instance, iconName, 0, 0, &image))
    {
        std::lock_guard<std::mutex> guard(g_iconLock);
        const ULONG_PTR token = g_nextGuestIcon++;
        g_guestIcons.emplace(token, std::move(image));
        RuntimeDiagnostics::Record(L"ICON: decoded a guest RT_GROUP_ICON resource.");
        BridgeSetLastError(ERROR_SUCCESS);
        return reinterpret_cast<HICON>(token);
    }

    // Stock USER icons are not PE resources in the guest. UWP cannot expose
    // desktop HICONs, so retain a real bridge-owned pixel surface instead of
    // an unusable sentinel. The fallback remains independent of any app.
    if (!instance && reinterpret_cast<ULONG_PTR>(iconName) <= 0xffff)
    {
        HICON fallback = CreateGuestShellIcon(false, 32);
        BridgeSetLastError(fallback ? ERROR_SUCCESS : ERROR_NOT_ENOUGH_MEMORY);
        return fallback;
    }
    RuntimeDiagnostics::Record(L"ICON: requested RT_GROUP_ICON resource was unavailable.");
    BridgeSetLastError(ERROR_RESOURCE_NAME_NOT_FOUND);
    return nullptr;
}

HICON WINAPI Win32Bridge::Bridge::BridgeLoadIconA(HINSTANCE instance, LPCSTR iconName)
{
    std::wstring storage;
    LPCWSTR converted = nullptr;
    if (!ConvertAnsiResourceName(iconName, &storage, &converted))
    {
        BridgeSetLastError(ERROR_NO_UNICODE_TRANSLATION);
        return nullptr;
    }
    return BridgeLoadIconW(instance, converted);
}

HICON WINAPI Win32Bridge::Bridge::BridgeCreateIconFromResourceEx(
    PBYTE bits,
    DWORD size,
    BOOL icon,
    DWORD version,
    int width,
    int height,
    UINT)
{
    if (!bits || size == 0 || (version != 0 && version != 0x00030000))
    {
        BridgeSetLastError(ERROR_INVALID_PARAMETER);
        return nullptr;
    }
    std::vector<BYTE> copy;
    if (!CopyGuestByteRange(bits, size, &copy))
    {
        BridgeSetLastError(ERROR_INVALID_DATA);
        return nullptr;
    }
    POINT hotspot{};
    const BYTE* imageBytes = copy.data();
    size_t imageSize = copy.size();
    if (!icon)
    {
        if (imageSize < 4)
        {
            BridgeSetLastError(ERROR_INVALID_DATA);
            return nullptr;
        }
        SHORT x = 0;
        SHORT y = 0;
        memcpy(&x, imageBytes, sizeof(x));
        memcpy(&y, imageBytes + sizeof(x), sizeof(y));
        hotspot.x = x;
        hotspot.y = y;
        imageBytes += 4;
        imageSize -= 4;
    }
    MiniGdi::Surface decoded;
    if (!DecodeDeviceIndependentBitmap(imageBytes, imageSize, true, &decoded))
    {
        BridgeSetLastError(ERROR_INVALID_DATA);
        return nullptr;
    }
    const int outputWidth = width > 0 ? width : decoded.Width();
    const int outputHeight = height > 0 ? height : decoded.Height();
    MiniGdi::Surface scaled = ScaleSurface(decoded, outputWidth, outputHeight);
    if (scaled.Empty())
    {
        BridgeSetLastError(ERROR_NOT_ENOUGH_MEMORY);
        return nullptr;
    }
    if (icon)
    {
        HICON handle = StoreGuestIcon(std::move(scaled));
        BridgeSetLastError(handle ? ERROR_SUCCESS : ERROR_NOT_ENOUGH_MEMORY);
        return handle;
    }
    if (decoded.Width() > 0 && decoded.Height() > 0)
    {
        hotspot.x = hotspot.x * outputWidth / decoded.Width();
        hotspot.y = hotspot.y * outputHeight / decoded.Height();
    }
    HCURSOR handle = StoreGuestCursor(std::move(scaled), hotspot);
    BridgeSetLastError(handle ? ERROR_SUCCESS : ERROR_NOT_ENOUGH_MEMORY);
    return reinterpret_cast<HICON>(handle);
}

HICON WINAPI Win32Bridge::Bridge::BridgeCreateIconFromResource(
    PBYTE bits,
    DWORD size,
    BOOL icon,
    DWORD version)
{
    return BridgeCreateIconFromResourceEx(bits, size, icon, version, 0, 0, LR_DEFAULTSIZE | LR_SHARED);
}

HANDLE WINAPI Win32Bridge::Bridge::BridgeLoadImageW(
    HINSTANCE instance,
    LPCWSTR name,
    UINT type,
    int width,
    int height,
    UINT flags)
{
    if (!name || (type != IMAGE_BITMAP && type != IMAGE_ICON && type != IMAGE_CURSOR))
    {
        BridgeSetLastError(ERROR_INVALID_PARAMETER);
        return nullptr;
    }
    if ((flags & LR_DEFAULTSIZE) != 0)
    {
        if (width <= 0) width = type == IMAGE_BITMAP ? 0 : 32;
        if (height <= 0) height = type == IMAGE_BITMAP ? 0 : 32;
    }

    MiniGdi::Surface image;
    POINT hotspot{};
    if ((flags & LR_LOADFROMFILE) != 0)
    {
        GuestStorageContext* storage = CurrentGuestStorageContext();
        std::vector<BYTE> bytes;
        DWORD error = ERROR_SUCCESS;
        if (!storage || !storage->ReadAllBytes(name, &bytes, &error) ||
            !DecodeEncodedImage(bytes.data(), bytes.size(), &image, width, height))
        {
            BridgeSetLastError(error == ERROR_SUCCESS ? ERROR_INVALID_DATA : error);
            return nullptr;
        }
        if (width > 0 || height > 0)
        {
            image = ScaleSurface(image, width > 0 ? width : image.Width(),
                height > 0 ? height : image.Height());
        }
        hotspot.x = image.Width() / 2;
        hotspot.y = image.Height() / 2;
    }
    else if (type == IMAGE_BITMAP)
    {
        if (!LoadBitmapResourcePixels(instance, name, &image))
        {
            BridgeSetLastError(ERROR_RESOURCE_NAME_NOT_FOUND);
            return nullptr;
        }
        if (width > 0 || height > 0)
        {
            image = ScaleSurface(image, width > 0 ? width : image.Width(),
                height > 0 ? height : image.Height());
        }
    }
    else if (type == IMAGE_ICON)
    {
        if (!LoadIconResourcePixels(instance, name, width, height, &image))
        {
            BridgeSetLastError(ERROR_RESOURCE_NAME_NOT_FOUND);
            return nullptr;
        }
    }
    else if (!LoadCursorResourcePixels(instance, name, width, height, &image, &hotspot))
    {
        BridgeSetLastError(ERROR_RESOURCE_NAME_NOT_FOUND);
        return nullptr;
    }

    HANDLE result = nullptr;
    if (type == IMAGE_BITMAP) result = CreateGuestBitmapFromSurface(image);
    else if (type == IMAGE_ICON) result = StoreGuestIcon(std::move(image));
    else result = StoreGuestCursor(std::move(image), hotspot);
    BridgeSetLastError(result ? ERROR_SUCCESS : ERROR_NOT_ENOUGH_MEMORY);
    return result;
}

HANDLE WINAPI Win32Bridge::Bridge::BridgeLoadImageA(
    HINSTANCE instance,
    LPCSTR name,
    UINT type,
    int width,
    int height,
    UINT flags)
{
    std::wstring storage;
    LPCWSTR converted = nullptr;
    if (!ConvertAnsiResourceName(name, &storage, &converted))
    {
        BridgeSetLastError(ERROR_NO_UNICODE_TRANSLATION);
        return nullptr;
    }
    return BridgeLoadImageW(instance, converted, type, width, height, flags);
}

HANDLE WINAPI Win32Bridge::Bridge::BridgeCopyImage(
    HANDLE source,
    UINT type,
    int width,
    int height,
    UINT flags)
{
    if (!source)
    {
        BridgeSetLastError(ERROR_INVALID_HANDLE);
        return nullptr;
    }
    MiniGdi::Surface image;
    POINT hotspot{};
    if (type == IMAGE_ICON)
    {
        if (!CopyGuestIconPixels(reinterpret_cast<HICON>(source), &image))
        {
            BridgeSetLastError(ERROR_INVALID_HANDLE);
            return nullptr;
        }
    }
    else if (type == IMAGE_CURSOR)
    {
        std::lock_guard<std::mutex> guard(g_cursorLock);
        const auto found = g_guestCursors.find(reinterpret_cast<ULONG_PTR>(source));
        if (found == g_guestCursors.end() || found->second.image.Empty())
        {
            BridgeSetLastError(ERROR_INVALID_HANDLE);
            return nullptr;
        }
        image = found->second.image;
        hotspot = found->second.hotspot;
    }
    else if (type == IMAGE_BITMAP)
    {
        GuestWindowManager* manager = CurrentGuestWindowManager();
        const MiniGdi::Surface* bitmap = manager ? manager->Gdi().GetBitmapSurface(
            static_cast<MiniGdi::ObjectHandle>(reinterpret_cast<ULONG_PTR>(source))) : nullptr;
        if (!bitmap || bitmap->Empty())
        {
            BridgeSetLastError(ERROR_INVALID_HANDLE);
            return nullptr;
        }
        image = *bitmap;
    }
    else
    {
        BridgeSetLastError(ERROR_INVALID_PARAMETER);
        return nullptr;
    }
    const int outputWidth = width > 0 ? width : image.Width();
    const int outputHeight = height > 0 ? height : image.Height();
    if (type == IMAGE_CURSOR && image.Width() > 0 && image.Height() > 0)
    {
        hotspot.x = hotspot.x * outputWidth / image.Width();
        hotspot.y = hotspot.y * outputHeight / image.Height();
    }
    image = ScaleSurface(image, outputWidth, outputHeight);
    HANDLE result = type == IMAGE_ICON
        ? reinterpret_cast<HANDLE>(StoreGuestIcon(std::move(image)))
        : type == IMAGE_CURSOR
            ? reinterpret_cast<HANDLE>(StoreGuestCursor(std::move(image), hotspot))
            : reinterpret_cast<HANDLE>(CreateGuestBitmapFromSurface(image));
    if (result && (flags & LR_COPYDELETEORG) != 0)
    {
        if (type == IMAGE_ICON) BridgeDestroyIcon(reinterpret_cast<HICON>(source));
        else if (type == IMAGE_CURSOR) BridgeDestroyCursor(reinterpret_cast<HCURSOR>(source));
        else if (GuestWindowManager* manager = CurrentGuestWindowManager())
            manager->Gdi().DeleteObject(static_cast<MiniGdi::ObjectHandle>(reinterpret_cast<ULONG_PTR>(source)));
    }
    BridgeSetLastError(result ? ERROR_SUCCESS : ERROR_NOT_ENOUGH_MEMORY);
    return result;
}

HICON WINAPI Win32Bridge::Bridge::BridgeCopyIcon(HICON icon)
{
    return reinterpret_cast<HICON>(BridgeCopyImage(icon, IMAGE_ICON, 0, 0, 0));
}

BOOL WINAPI Win32Bridge::Bridge::BridgeDrawIconEx(
    HDC dc, int x, int y, HICON icon, int width, int height, UINT step, HBRUSH, UINT flags)
{
    if (!dc || !icon || step != 0 || (flags & (DrawIconMask | DrawIconImage)) == 0)
    {
        BridgeSetLastError(ERROR_INVALID_PARAMETER);
        return FALSE;
    }
    MiniGdi::Surface source;
    if (!CopyGuestIconPixels(icon, &source))
    {
        std::lock_guard<std::mutex> guard(g_cursorLock);
        const auto found = g_guestCursors.find(reinterpret_cast<ULONG_PTR>(icon));
        if (found == g_guestCursors.end())
        {
            BridgeSetLastError(ERROR_INVALID_HANDLE);
            return FALSE;
        }
        source = found->second.image;
    }
    const int outputWidth = width > 0 ? width : source.Width();
    const int outputHeight = height > 0 ? height : source.Height();
    source = ScaleSurface(source, outputWidth, outputHeight);
    GuestWindowManager* manager = CurrentGuestWindowManager();
    MiniGdi::Surface* destination = manager
        ? manager->Gdi().GetSurface(manager->GuestDcHandle(dc)) : nullptr;
    if (!destination || source.Empty())
    {
        BridgeSetLastError(ERROR_INVALID_HANDLE);
        return FALSE;
    }
    const bool imagePass = (flags & DrawIconImage) != 0;
    for (int sourceY = 0; sourceY < source.Height(); ++sourceY)
    {
        const int destinationY = y + sourceY;
        if (destinationY < 0 || destinationY >= destination->Height()) continue;
        for (int sourceX = 0; sourceX < source.Width(); ++sourceX)
        {
            const int destinationX = x + sourceX;
            if (destinationX < 0 || destinationX >= destination->Width()) continue;
            const MiniGdi::Color* input = source.PixelAt(sourceX, sourceY);
            MiniGdi::Color* output = destination->PixelAt(destinationX, destinationY);
            if (!input || !output) continue;
            if (imagePass)
                *output = MiniGdi::BlendSourceOver(*input, *output);
            else
                *output = MiniGdi::Alpha(*input) == 0 ? MiniGdi::OpaqueWhite : MiniGdi::OpaqueBlack;
        }
    }
    BridgeSetLastError(ERROR_SUCCESS);
    return TRUE;
}

BOOL WINAPI Win32Bridge::Bridge::BridgeDrawIcon(HDC dc, int x, int y, HICON icon)
{
    return BridgeDrawIconEx(dc, x, y, icon, 0, 0, 0, nullptr, DrawIconNormal);
}

BOOL WINAPI Win32Bridge::Bridge::BridgeDestroyCursor(HCURSOR cursor)
{
    if (!cursor)
    {
        BridgeSetLastError(ERROR_INVALID_HANDLE);
        return FALSE;
    }
    std::lock_guard<std::mutex> guard(g_cursorLock);
    if (g_guestCursors.erase(reinterpret_cast<ULONG_PTR>(cursor)) == 0)
    {
        BridgeSetLastError(ERROR_INVALID_HANDLE);
        return FALSE;
    }
    BridgeSetLastError(ERROR_SUCCESS);
    return TRUE;
}

BOOL WINAPI Win32Bridge::Bridge::BridgeDestroyIcon(HICON icon)
{
    const ULONG_PTR token = reinterpret_cast<ULONG_PTR>(icon);
    if (token == GuestIconToken)
    {
        BridgeSetLastError(ERROR_SUCCESS);
        return TRUE;
    }
    std::lock_guard<std::mutex> guard(g_iconLock);
    if (g_guestIcons.erase(token) == 0)
    {
        BridgeSetLastError(ERROR_INVALID_HANDLE);
        return FALSE;
    }
    BridgeSetLastError(ERROR_SUCCESS);
    return TRUE;
}

bool Win32Bridge::Bridge::CopyGuestIconPixels(HICON icon, MiniGdi::Surface* destination)
{
    if (!destination) return false;
    std::lock_guard<std::mutex> guard(g_iconLock);
    const auto found = g_guestIcons.find(reinterpret_cast<ULONG_PTR>(icon));
    if (found == g_guestIcons.end()) return false;
    *destination = found->second;
    return true;
}

HICON Win32Bridge::Bridge::CreateGuestShellIcon(bool directory, int size)
{
    size = (std::max)(8, (std::min)(size, 256));
    MiniGdi::Surface image(size, size, MiniGdi::Transparent);
    const int margin = (std::max)(1, size / 8);
    if (directory)
    {
        const int tabWidth = (std::max)(3, size * 2 / 5);
        const int tabHeight = (std::max)(2, size / 5);
        MiniGdi::DrawRectangle(image,
            MiniGdi::Rect{ margin, margin + tabHeight / 2, margin + tabWidth, margin + tabHeight + 1 },
            MiniGdi::MakeColor(255, 194, 45), MiniGdi::MakeColor(183, 119, 0));
        MiniGdi::DrawRectangle(image,
            MiniGdi::Rect{ margin, margin + tabHeight, size - margin, size - margin },
            MiniGdi::MakeColor(255, 202, 62), MiniGdi::MakeColor(183, 119, 0));
    }
    else
    {
        const int fold = (std::max)(2, size / 4);
        const MiniGdi::Color border = MiniGdi::MakeColor(105, 114, 125);
        MiniGdi::DrawRectangle(image,
            MiniGdi::Rect{ margin, margin, size - margin, size - margin },
            MiniGdi::OpaqueWhite, border);
        for (int y = margin + fold + 2; y < size - margin - 1; y += (std::max)(2, size / 5))
            MiniGdi::DrawLine(image, { margin + 2, y }, { size - margin - 3, y },
                MiniGdi::MakeColor(145, 154, 165));
        MiniGdi::DrawLine(image, { size - margin - fold, margin },
            { size - margin, margin + fold }, border);
    }
    return StoreGuestIcon(std::move(image));
}

int WINAPI Win32Bridge::Bridge::BridgeGetSystemMetrics(int index)
{
    // The values describe the bridge's virtual desktop rather than the UWP
    // host monitor. Keep common Win32 layout calculations deterministic.
    int value = 0;
    if (!GuestMetrics::TryGetSystemMetric(index, &value))
    {
        BridgeSetLastError(ERROR_INVALID_PARAMETER);
        return 0;
    }
    BridgeSetLastError(ERROR_SUCCESS);
    return value;
}

COLORREF WINAPI Win32Bridge::Bridge::BridgeGetSysColor(int color)
{
    if (!IsGuestSystemColor(color))
    {
        BridgeSetLastError(ERROR_INVALID_PARAMETER);
        return 0;
    }
    BridgeSetLastError(ERROR_SUCCESS);
    return GuestColorRef(color);
}

HBRUSH WINAPI Win32Bridge::Bridge::BridgeGetSysColorBrush(int color)
{
    if (!IsGuestSystemColor(color))
    {
        BridgeSetLastError(ERROR_INVALID_PARAMETER);
        return nullptr;
    }

    // Match the documented WNDCLASS encoding: COLOR_* + 1. FillRect and the
    // class-background path recognize this non-owning system-brush form.
    BridgeSetLastError(ERROR_SUCCESS);
    return reinterpret_cast<HBRUSH>(static_cast<ULONG_PTR>(color + 1));
}

BOOL WINAPI Win32Bridge::Bridge::BridgeAdjustWindowRect(LPRECT rect, DWORD, BOOL hasMenu)
{
    if (!rect)
    {
        BridgeSetLastError(ERROR_INVALID_PARAMETER);
        return FALSE;
    }

    // Guest windows are client-only surfaces today. Retaining the exact RECT
    // is equivalent to a zero non-client frame and keeps layout arithmetic
    // portable until a title-bar adapter exists.
    BridgeSetLastError(ERROR_SUCCESS);
    if (hasMenu)
    {
        rect->bottom += GuestMetrics::MenuHeight;
    }
    return TRUE;
}

BOOL WINAPI Win32Bridge::Bridge::BridgeAdjustWindowRectEx(LPRECT rect, DWORD, BOOL hasMenu, DWORD)
{
    if (!rect)
    {
        BridgeSetLastError(ERROR_INVALID_PARAMETER);
        return FALSE;
    }

    BridgeSetLastError(ERROR_SUCCESS);
    if (hasMenu)
    {
        rect->bottom += GuestMetrics::MenuHeight;
    }
    return TRUE;
}

BOOL WINAPI Win32Bridge::Bridge::BridgeDestroyWindow(HWND window)
{
    GuestWindowManager* manager = CurrentManagerOrFail();
    if (!manager)
    {
        return FALSE;
    }

    DWORD error = ERROR_SUCCESS;
    const BOOL result = manager->DestroyGuestWindow(window, &error);
    BridgeSetLastError(error);
    return result;
}

BOOL WINAPI Win32Bridge::Bridge::BridgeShowWindow(HWND window, int command)
{
    GuestWindowManager* manager = CurrentManagerOrFail();
    if (!manager)
    {
        return FALSE;
    }

    DWORD error = ERROR_SUCCESS;
    const BOOL result = manager->ShowGuestWindow(window, command, &error);
    BridgeSetLastError(error);
    return result;
}

BOOL WINAPI Win32Bridge::Bridge::BridgeGetClientRect(HWND window, LPRECT rect)
{
    GuestWindowManager* manager = CurrentManagerOrFail();
    if (!manager)
    {
        return FALSE;
    }

    DWORD error = ERROR_SUCCESS;
    const BOOL result = manager->GetGuestClientRect(window, rect, &error);
    BridgeSetLastError(error);
    return result;
}

BOOL WINAPI Win32Bridge::Bridge::BridgeGetWindowRect(HWND window, LPRECT rect)
{
    GuestWindowManager* manager = CurrentManagerOrFail();
    if (!manager)
    {
        return FALSE;
    }

    DWORD error = ERROR_SUCCESS;
    const BOOL result = manager->GetGuestWindowRect(window, rect, &error);
    BridgeSetLastError(error);
    return result;
}

BOOL WINAPI Win32Bridge::Bridge::BridgeSetWindowTextW(HWND window, LPCWSTR text)
{
    GuestWindowManager* manager = CurrentManagerOrFail();
    if (!manager)
    {
        return FALSE;
    }

    DWORD error = ERROR_SUCCESS;
    const BOOL result = manager->SetGuestWindowText(window, text, &error);
    BridgeSetLastError(error);
    return result;
}

int WINAPI Win32Bridge::Bridge::BridgeGetWindowTextW(HWND window, LPWSTR buffer, int count)
{
    GuestWindowManager* manager = CurrentManagerOrFail();
    if (!manager)
    {
        return 0;
    }

    DWORD error = ERROR_SUCCESS;
    const int result = manager->GetGuestWindowText(window, buffer, count, &error);
    BridgeSetLastError(error);
    return result;
}

int WINAPI Win32Bridge::Bridge::BridgeGetWindowTextLengthW(HWND window)
{
    GuestWindowManager* manager = CurrentManagerOrFail();
    if (!manager)
    {
        return 0;
    }

    DWORD error = ERROR_SUCCESS;
    const int result = manager->GetGuestWindowTextLength(window, &error);
    BridgeSetLastError(error);
    return result;
}

HWND WINAPI Win32Bridge::Bridge::BridgeGetDlgItem(HWND parent, int identifier)
{
    GuestWindowManager* manager = CurrentManagerOrFail();
    if (!manager)
    {
        return nullptr;
    }

    DWORD error = ERROR_SUCCESS;
    const HWND result = manager->GetGuestDlgItem(parent, identifier, &error);
    BridgeSetLastError(error);
    return result;
}

int WINAPI Win32Bridge::Bridge::BridgeGetDlgCtrlID(HWND window)
{
    GuestWindowManager* manager = CurrentManagerOrFail();
    if (!manager)
    {
        return 0;
    }

    DWORD error = ERROR_SUCCESS;
    const LONG_PTR identifier = manager->GetGuestWindowLongPtr(window, GuestAbi::GwlpId, &error);
    BridgeSetLastError(error);
    return error == ERROR_SUCCESS ? static_cast<int>(identifier) : 0;
}

BOOL WINAPI Win32Bridge::Bridge::BridgeSetDlgItemTextW(HWND parent, int identifier, LPCWSTR text)
{
    GuestWindowManager* manager = CurrentManagerOrFail();
    if (!manager)
    {
        return FALSE;
    }

    DWORD error = ERROR_SUCCESS;
    const HWND child = manager->GetGuestDlgItem(parent, identifier, &error);
    if (!child)
    {
        BridgeSetLastError(error == ERROR_SUCCESS ? ERROR_NOT_FOUND : error);
        return FALSE;
    }

    const BOOL result = manager->SetGuestWindowText(child, text, &error);
    BridgeSetLastError(error);
    return result;
}

UINT WINAPI Win32Bridge::Bridge::BridgeGetDlgItemTextW(HWND parent, int identifier, LPWSTR buffer, int count)
{
    GuestWindowManager* manager = CurrentManagerOrFail();
    if (!manager)
    {
        return 0;
    }

    DWORD error = ERROR_SUCCESS;
    const HWND child = manager->GetGuestDlgItem(parent, identifier, &error);
    if (!child)
    {
        BridgeSetLastError(error == ERROR_SUCCESS ? ERROR_NOT_FOUND : error);
        return 0;
    }

    const int result = manager->GetGuestWindowText(child, buffer, count, &error);
    BridgeSetLastError(error);
    return result > 0 ? static_cast<UINT>(result) : 0;
}

LRESULT WINAPI Win32Bridge::Bridge::BridgeSendDlgItemMessageW(
    HWND parent,
    int identifier,
    UINT message,
    WPARAM wParam,
    LPARAM lParam)
{
    GuestWindowManager* manager = CurrentManagerOrFail();
    if (!manager)
    {
        return 0;
    }

    DWORD error = ERROR_SUCCESS;
    const HWND child = manager->GetGuestDlgItem(parent, identifier, &error);
    if (!child)
    {
        BridgeSetLastError(error == ERROR_SUCCESS ? ERROR_NOT_FOUND : error);
        return 0;
    }

    const LRESULT result = manager->SendGuestMessage(child, message, wParam, lParam, &error);
    BridgeSetLastError(error);
    return result;
}

BOOL WINAPI Win32Bridge::Bridge::BridgeIsWindow(HWND window)
{
    GuestWindowManager* manager = CurrentManagerOrFail();
    if (!manager)
    {
        return FALSE;
    }

    const BOOL result = manager->IsGuestWindow(window);
    BridgeSetLastError(result ? ERROR_SUCCESS : ERROR_INVALID_WINDOW_HANDLE);
    return result;
}

BOOL WINAPI Win32Bridge::Bridge::BridgeIsWindowVisible(HWND window)
{
    GuestWindowManager* manager = CurrentManagerOrFail();
    if (!manager)
    {
        return FALSE;
    }

    DWORD error = ERROR_SUCCESS;
    const BOOL result = manager->IsGuestWindowVisible(window, &error);
    BridgeSetLastError(error);
    return result;
}

BOOL WINAPI Win32Bridge::Bridge::BridgeIsWindowEnabled(HWND window)
{
    GuestWindowManager* manager = CurrentManagerOrFail();
    if (!manager)
    {
        return FALSE;
    }

    DWORD error = ERROR_SUCCESS;
    const BOOL result = manager->IsGuestWindowEnabled(window, &error);
    BridgeSetLastError(error);
    return result;
}

BOOL WINAPI Win32Bridge::Bridge::BridgeIsZoomed(HWND window)
{
    if (!BridgeIsWindow(window))
    {
        BridgeSetLastError(ERROR_INVALID_WINDOW_HANDLE);
        return FALSE;
    }
    // The bridge currently exposes one non-maximized guest presentation
    // surface. Returning FALSE is the normal state, not an unsupported call.
    BridgeSetLastError(ERROR_SUCCESS);
    return FALSE;
}

BOOL WINAPI Win32Bridge::Bridge::BridgeEnableWindow(HWND window, BOOL enable)
{
    GuestWindowManager* manager = CurrentManagerOrFail();
    if (!manager)
    {
        return FALSE;
    }

    DWORD error = ERROR_SUCCESS;
    const BOOL result = manager->EnableGuestWindow(window, enable, &error);
    BridgeSetLastError(error);
    return result;
}

HWND WINAPI Win32Bridge::Bridge::BridgeSetFocus(HWND window)
{
    GuestWindowManager* manager = CurrentManagerOrFail();
    if (!manager)
    {
        return nullptr;
    }

    DWORD error = ERROR_SUCCESS;
    const HWND result = manager->SetGuestFocus(window, &error);
    BridgeSetLastError(error);
    return result;
}

HWND WINAPI Win32Bridge::Bridge::BridgeGetFocus()
{
    GuestWindowManager* manager = CurrentManagerOrFail();
    if (!manager)
    {
        return nullptr;
    }

    DWORD error = ERROR_SUCCESS;
    const HWND result = manager->GetGuestFocus(&error);
    BridgeSetLastError(error);
    return result;
}

HWND WINAPI Win32Bridge::Bridge::BridgeSetCapture(HWND window)
{
    GuestWindowManager* manager = CurrentManagerOrFail();
    if (!manager)
    {
        return nullptr;
    }

    DWORD error = ERROR_SUCCESS;
    const HWND result = manager->SetGuestCapture(window, &error);
    BridgeSetLastError(error);
    return result;
}

BOOL WINAPI Win32Bridge::Bridge::BridgeReleaseCapture()
{
    GuestWindowManager* manager = CurrentManagerOrFail();
    if (!manager)
    {
        return FALSE;
    }

    DWORD error = ERROR_SUCCESS;
    const BOOL result = manager->ReleaseGuestCapture(&error);
    BridgeSetLastError(error);
    return result;
}

HWND WINAPI Win32Bridge::Bridge::BridgeGetCapture()
{
    GuestWindowManager* manager = CurrentManagerOrFail();
    if (!manager)
    {
        return nullptr;
    }

    DWORD error = ERROR_SUCCESS;
    const HWND result = manager->GetGuestCapture(&error);
    BridgeSetLastError(error);
    return result;
}

HWND WINAPI Win32Bridge::Bridge::BridgeGetParent(HWND window)
{
    GuestWindowManager* manager = CurrentManagerOrFail();
    if (!manager)
    {
        return nullptr;
    }

    DWORD error = ERROR_SUCCESS;
    const HWND result = manager->GetGuestParent(window, &error);
    BridgeSetLastError(error);
    return result;
}

LONG WINAPI Win32Bridge::Bridge::BridgeGetWindowLongW(HWND window, int index)
{
    GuestWindowManager* manager = CurrentManagerOrFail();
    if (!manager)
    {
        return 0;
    }

    DWORD error = ERROR_SUCCESS;
    const LONG result = manager->GetGuestWindowLong(window, index, &error);
    BridgeSetLastError(error);
    return result;
}

LONG WINAPI Win32Bridge::Bridge::BridgeSetWindowLongW(HWND window, int index, LONG value)
{
    GuestWindowManager* manager = CurrentManagerOrFail();
    if (!manager)
    {
        return 0;
    }

    DWORD error = ERROR_SUCCESS;
    const LONG result = manager->SetGuestWindowLong(window, index, value, &error);
    BridgeSetLastError(error);
    return result;
}

LONG_PTR WINAPI Win32Bridge::Bridge::BridgeGetWindowLongPtrW(HWND window, int index)
{
    GuestWindowManager* manager = CurrentManagerOrFail();
    if (!manager)
    {
        return 0;
    }

    DWORD error = ERROR_SUCCESS;
    const LONG_PTR result = manager->GetGuestWindowLongPtr(window, index, &error);
    BridgeSetLastError(error);
    return result;
}

LONG_PTR WINAPI Win32Bridge::Bridge::BridgeSetWindowLongPtrW(HWND window, int index, LONG_PTR value)
{
    GuestWindowManager* manager = CurrentManagerOrFail();
    if (!manager)
    {
        return 0;
    }

    DWORD error = ERROR_SUCCESS;
    const LONG_PTR result = manager->SetGuestWindowLongPtr(window, index, value, &error);
    BridgeSetLastError(error);
    return result;
}

BOOL WINAPI Win32Bridge::Bridge::BridgeSetWindowPos(
    HWND window,
    HWND insertAfter,
    int x,
    int y,
    int width,
    int height,
    UINT flags)
{
    GuestWindowManager* manager = CurrentManagerOrFail();
    if (!manager)
    {
        return FALSE;
    }

    DWORD error = ERROR_SUCCESS;
    const BOOL result = manager->SetGuestWindowPos(
        window,
        insertAfter,
        x,
        y,
        width,
        height,
        flags,
        &error);
    BridgeSetLastError(error);
    return result;
}

UINT_PTR WINAPI Win32Bridge::Bridge::BridgeSetTimer(
    HWND window,
    UINT_PTR timerId,
    UINT elapseMilliseconds,
    GuestAbi::TimerProc timerProcedure)
{
    GuestWindowManager* manager = CurrentManagerOrFail();
    if (!manager)
    {
        return 0;
    }

    DWORD error = ERROR_SUCCESS;
    const UINT_PTR result = manager->SetGuestTimer(
        window,
        timerId,
        elapseMilliseconds,
        timerProcedure,
        &error);
    BridgeSetLastError(error);
    return result;
}

BOOL WINAPI Win32Bridge::Bridge::BridgeKillTimer(HWND window, UINT_PTR timerId)
{
    GuestWindowManager* manager = CurrentManagerOrFail();
    if (!manager)
    {
        return FALSE;
    }

    DWORD error = ERROR_SUCCESS;
    const BOOL result = manager->KillGuestTimer(window, timerId, &error);
    BridgeSetLastError(error);
    return result;
}

LRESULT WINAPI Win32Bridge::Bridge::BridgeDefWindowProcW(HWND window, UINT message, WPARAM wParam, LPARAM lParam)
{
    GuestWindowManager* manager = CurrentManagerOrFail();
    if (!manager)
    {
        return 0;
    }

    BridgeSetLastError(ERROR_SUCCESS);
    return manager->DefaultGuestWindowProcedure(window, message, wParam, lParam);
}

BOOL WINAPI Win32Bridge::Bridge::BridgePostMessageW(HWND window, UINT message, WPARAM wParam, LPARAM lParam)
{
    GuestWindowManager* manager = CurrentManagerOrFail();
    if (!manager)
    {
        return FALSE;
    }

    DWORD error = ERROR_SUCCESS;
    const BOOL result = manager->PostGuestMessage(window, message, wParam, lParam, &error);
    BridgeSetLastError(error);
    return result;
}

LRESULT WINAPI Win32Bridge::Bridge::BridgeSendMessageW(HWND window, UINT message, WPARAM wParam, LPARAM lParam)
{
    GuestWindowManager* manager = CurrentManagerOrFail();
    if (!manager)
    {
        return 0;
    }

    DWORD error = ERROR_SUCCESS;
    const LRESULT result = manager->SendGuestMessage(window, message, wParam, lParam, &error);
    BridgeSetLastError(error);
    return result;
}

void WINAPI Win32Bridge::Bridge::BridgePostQuitMessage(int exitCode)
{
    GuestWindowManager* manager = CurrentManagerOrFail();
    if (manager)
    {
        manager->PostGuestQuitMessage(exitCode);
        BridgeSetLastError(ERROR_SUCCESS);
    }
}

BOOL WINAPI Win32Bridge::Bridge::BridgeGetMessageW(
    GuestAbi::Message* message,
    HWND window,
    UINT minimumMessage,
    UINT maximumMessage)
{
    GuestWindowManager* manager = CurrentManagerOrFail();
    if (!manager)
    {
        return static_cast<BOOL>(-1);
    }
    if (!message)
    {
        BridgeSetLastError(ERROR_INVALID_PARAMETER);
        return static_cast<BOOL>(-1);
    }

    switch (manager->GetGuestMessage(message, window, minimumMessage, maximumMessage))
    {
    case GuestGetMessageResult::Message:
        BridgeSetLastError(ERROR_SUCCESS);
        return TRUE;
    case GuestGetMessageResult::Quit:
        BridgeSetLastError(ERROR_SUCCESS);
        return FALSE;
    default:
        BridgeSetLastError(ERROR_INVALID_PARAMETER);
        return static_cast<BOOL>(-1);
    }
}

BOOL WINAPI Win32Bridge::Bridge::BridgePeekMessageW(
    GuestAbi::Message* message,
    HWND window,
    UINT minimumMessage,
    UINT maximumMessage,
    UINT removeMessage)
{
    GuestWindowManager* manager = CurrentManagerOrFail();
    if (!manager)
    {
        return FALSE;
    }
    if (!message || (removeMessage & ~(GuestAbi::PeekRemove | GuestAbi::PeekNoYield)) != 0)
    {
        BridgeSetLastError(ERROR_INVALID_PARAMETER);
        return FALSE;
    }

    const BOOL result = manager->PeekGuestMessage(message, window, minimumMessage, maximumMessage, removeMessage);
    if (result)
    {
        BridgeSetLastError(ERROR_SUCCESS);
    }
    return result;
}

BOOL WINAPI Win32Bridge::Bridge::BridgeTranslateMessage(const GuestAbi::Message* message)
{
    GuestWindowManager* manager = CurrentManagerOrFail();
    if (!manager)
    {
        return FALSE;
    }
    if (!message)
    {
        BridgeSetLastError(ERROR_INVALID_PARAMETER);
        return FALSE;
    }

    const BOOL result = manager->TranslateGuestMessage(message);
    BridgeSetLastError(ERROR_SUCCESS);
    return result;
}

LRESULT WINAPI Win32Bridge::Bridge::BridgeDispatchMessageW(const GuestAbi::Message* message)
{
    GuestWindowManager* manager = CurrentManagerOrFail();
    if (!manager)
    {
        return 0;
    }

    DWORD error = ERROR_SUCCESS;
    const LRESULT result = manager->DispatchGuestMessage(message, &error);
    BridgeSetLastError(error);
    return result;
}

BOOL WINAPI Win32Bridge::Bridge::BridgeInvalidateRect(HWND window, const RECT* rect, BOOL erase)
{
    GuestWindowManager* manager = CurrentManagerOrFail();
    if (!manager)
    {
        return FALSE;
    }

    DWORD error = ERROR_SUCCESS;
    const BOOL result = manager->InvalidateGuestRect(window, rect, erase, &error);
    BridgeSetLastError(error);
    return result;
}

BOOL WINAPI Win32Bridge::Bridge::BridgeUpdateWindow(HWND window)
{
    GuestWindowManager* manager = CurrentManagerOrFail();
    if (!manager)
    {
        return FALSE;
    }

    DWORD error = ERROR_SUCCESS;
    const BOOL result = manager->UpdateGuestWindow(window, &error);
    BridgeSetLastError(error);
    return result;
}

HDC WINAPI Win32Bridge::Bridge::BridgeBeginPaint(HWND window, GuestAbi::PaintStruct* paint)
{
    GuestWindowManager* manager = CurrentManagerOrFail();
    if (!manager)
    {
        return nullptr;
    }

    DWORD error = ERROR_SUCCESS;
    const HDC result = manager->BeginGuestPaint(window, paint, &error);
    BridgeSetLastError(error);
    return result;
}

BOOL WINAPI Win32Bridge::Bridge::BridgeEndPaint(HWND window, const GuestAbi::PaintStruct* paint)
{
    GuestWindowManager* manager = CurrentManagerOrFail();
    if (!manager)
    {
        return FALSE;
    }

    DWORD error = ERROR_SUCCESS;
    const BOOL result = manager->EndGuestPaint(window, paint, &error);
    BridgeSetLastError(error);
    return result;
}

HDC WINAPI Win32Bridge::Bridge::BridgeGetDC(HWND window)
{
    GuestWindowManager* manager = CurrentManagerOrFail();
    if (!manager)
    {
        return nullptr;
    }

    DWORD error = ERROR_SUCCESS;
    const HDC result = manager->GetGuestDC(window, &error);
    BridgeSetLastError(error);
    return result;
}

int WINAPI Win32Bridge::Bridge::BridgeReleaseDC(HWND window, HDC dc)
{
    GuestWindowManager* manager = CurrentManagerOrFail();
    if (!manager)
    {
        return 0;
    }

    DWORD error = ERROR_SUCCESS;
    const int result = manager->ReleaseGuestDC(window, dc, &error);
    BridgeSetLastError(error);
    return result;
}

int WINAPI Win32Bridge::Bridge::BridgeFillRect(HDC dc, const RECT* rect, HBRUSH brush)
{
    GuestWindowManager* manager = CurrentManagerOrFail();
    if (!manager)
    {
        return 0;
    }
    if (!rect)
    {
        BridgeSetLastError(ERROR_INVALID_PARAMETER);
        return 0;
    }

    const MiniGdi::DcHandle guestDc = manager->GuestDcHandle(dc);
    const ULONG_PTR rawBrush = reinterpret_cast<ULONG_PTR>(brush);
    MiniGdi::ObjectHandle guestBrush = MiniGdi::InvalidObject;
    bool temporarySystemBrush = false;
    if (rawBrush >= 1 && rawBrush <= static_cast<ULONG_PTR>(MaximumGuestSystemColor + 1))
    {
        guestBrush = manager->Gdi().CreateSolidBrush(GuestSystemColor(static_cast<int>(rawBrush - 1)));
        temporarySystemBrush = true;
    }
    else
    {
        guestBrush = FromGuestObject(brush);
    }

    MiniGdi::ObjectKind kind;
    if (guestDc == MiniGdi::InvalidDc || guestBrush == MiniGdi::InvalidObject ||
        !manager->Gdi().ObjectType(guestBrush, &kind) || kind != MiniGdi::ObjectKind::Brush)
    {
        if (temporarySystemBrush && guestBrush != MiniGdi::InvalidObject)
        {
            manager->Gdi().DeleteObject(guestBrush);
        }
        BridgeSetLastError(ERROR_INVALID_HANDLE);
        return 0;
    }

    MiniGdi::ObjectHandle previous = MiniGdi::InvalidObject;
    if (!manager->Gdi().SelectBrush(guestDc, guestBrush, &previous))
    {
        if (temporarySystemBrush)
        {
            manager->Gdi().DeleteObject(guestBrush);
        }
        BridgeSetLastError(ERROR_INVALID_HANDLE);
        return 0;
    }

    manager->Gdi().FillRect(guestDc, MiniGdi::Rect{ rect->left, rect->top, rect->right, rect->bottom });
    manager->Gdi().SelectBrush(guestDc, previous, nullptr);
    if (temporarySystemBrush)
    {
        manager->Gdi().DeleteObject(guestBrush);
    }
    BridgeSetLastError(ERROR_SUCCESS);
    return 1;
}

int WINAPI Win32Bridge::Bridge::BridgeDrawTextW(HDC dc, LPWSTR text, int characterCount, LPRECT rect, UINT format)
{
    GuestWindowManager* manager = CurrentManagerOrFail();
    if (!manager)
    {
        return 0;
    }
    if (!rect || !text || characterCount < -1)
    {
        BridgeSetLastError(ERROR_INVALID_PARAMETER);
        return 0;
    }

    constexpr size_t MaximumDrawTextCharacters = 1024 * 1024;
    size_t count = static_cast<size_t>(characterCount);
    if (characterCount == -1)
    {
        count = 0;
        while (count < MaximumDrawTextCharacters && text[count] != L'\0')
        {
            ++count;
        }
        if (count == MaximumDrawTextCharacters)
        {
            BridgeSetLastError(ERROR_INVALID_PARAMETER);
            return 0;
        }
    }

    const MiniGdi::DcHandle guestDc = manager->GuestDcHandle(dc);
    if (guestDc == MiniGdi::InvalidDc)
    {
        BridgeSetLastError(ERROR_INVALID_HANDLE);
        return 0;
    }

    const bool processPrefix = (format & GuestAbi::DrawTextNoPrefix) == 0;
    const bool singleLine = (format & GuestAbi::DrawTextSingleLine) != 0;
    const auto prepareDisplayText = [processPrefix, singleLine](
        const std::wstring& source, std::size_t* mnemonicPosition)
    {
        std::wstring prepared;
        prepared.reserve(source.size());
        if (mnemonicPosition)
            *mnemonicPosition = static_cast<std::size_t>(-1);
        for (std::size_t index = 0; index < source.size(); ++index)
        {
            wchar_t character = source[index];
            if (singleLine && (character == L'\r' || character == L'\n'))
            {
                if (character == L'\r' && index + 1 < source.size() &&
                    source[index + 1] == L'\n') ++index;
                prepared.push_back(L' ');
                continue;
            }
            if (processPrefix && character == L'&')
            {
                if (index + 1 < source.size() && source[index + 1] == L'&')
                {
                    prepared.push_back(L'&');
                    ++index;
                }
                else if (index + 1 < source.size() && mnemonicPosition &&
                    *mnemonicPosition == static_cast<std::size_t>(-1))
                {
                    *mnemonicPosition = prepared.size();
                }
                continue;
            }
            prepared.push_back(character);
        }
        return prepared;
    };

    std::wstring sourceText(text, text + count);
    std::wstring displayText = prepareDisplayText(sourceText, nullptr);
    bool modifiedByEllipsis = false;
    const bool calculate = (format & GuestAbi::DrawTextCalcRect) != 0;
    const bool mayModify = (format & GuestAbi::DrawTextModifyString) != 0 &&
        (format & (GuestAbi::DrawTextEndEllipsis |
            GuestAbi::DrawTextPathEllipsis |
            GuestAbi::DrawTextWordEllipsis)) != 0;
    const auto boundedRectExtent = [](LONG end, LONG start)
    {
        const std::int64_t extent = static_cast<std::int64_t>(end) - start;
        if (extent <= 0) return 0;
        if (extent > (std::numeric_limits<int>::max)())
            return (std::numeric_limits<int>::max)();
        return static_cast<int>(extent);
    };
    const int availableWidth = boundedRectExtent(rect->right, rect->left);
    const int availableHeight = calculate
        ? 1024 * 1024
        : boundedRectExtent(rect->bottom, rect->top);
    const auto measuredWidth = [manager, guestDc](const std::wstring& value)
    {
        MiniGdi::Size measured{};
        if (!manager->Gdi().GetTextExtentW(
            guestDc, value.data(), value.size(), &measured))
            return (std::numeric_limits<int>::max)();
        return measured.width;
    };
    MiniGdi::TextLayoutOptions fitOptions;
    fitOptions.wordWrap = !singleLine &&
        (format & GuestAbi::DrawTextWordBreak) != 0;
    fitOptions.includeExternalLeading =
        (format & GuestAbi::DrawTextExternalLeading) != 0;
    fitOptions.clipToLayout = false;
    if ((format & GuestAbi::DrawTextExpandTabs) != 0)
    {
        unsigned tabCharacters = 8;
        if ((format & GuestAbi::DrawTextTabStop) != 0)
        {
            const unsigned requested = (format >> 8) & 0xffu;
            if (requested != 0) tabCharacters = requested;
        }
        MiniGdi::FontMetrics fontMetrics{};
        if (manager->Gdi().GetSelectedFontMetrics(guestDc, &fontMetrics))
            fitOptions.tabStop = static_cast<float>((std::max)(1,
                fontMetrics.averageWidth) * tabCharacters);
    }
    const auto fitsInLayout = [manager, guestDc, availableWidth, availableHeight,
        &fitOptions](const std::wstring& value)
    {
        MiniGdi::Size measured{};
        if (!manager->Gdi().DrawTextW(guestDc,
            MiniGdi::Rect{ 0, 0, availableWidth, availableHeight },
            value.data(), value.size(), fitOptions, false, &measured)) return false;
        return measured.width <= availableWidth && measured.height <= availableHeight;
    };
    const auto endEllipsify = [&fitsInLayout](
        const std::wstring& value, bool atWordBoundary)
    {
        static const std::wstring ellipsis = L"...";
        if (fitsInLayout(value)) return value;
        std::size_t low = 0;
        std::size_t high = value.size();
        while (low < high)
        {
            const std::size_t middle = low + (high - low + 1) / 2;
            if (fitsInLayout(value.substr(0, middle) + ellipsis))
                low = middle;
            else
                high = middle - 1;
        }
        std::size_t prefix = low;
        if (atWordBoundary && prefix < value.size())
        {
            while (prefix > 0 && !std::iswspace(value[prefix - 1])) --prefix;
            while (prefix > 0 && std::iswspace(value[prefix - 1])) --prefix;
        }
        return value.substr(0, prefix) + ellipsis;
    };
    const auto pathEllipsify = [&measuredWidth, &endEllipsify, availableWidth](
        const std::wstring& value)
    {
        static const std::wstring ellipsis = L"...";
        if (measuredWidth(value) <= availableWidth) return value;
        const std::size_t slash = value.find_last_of(L"\\/");
        if (slash == std::wstring::npos) return endEllipsify(value, false);
        const std::wstring suffix = value.substr(slash);
        std::size_t low = 0;
        std::size_t high = slash;
        while (low < high)
        {
            const std::size_t middle = low + (high - low + 1) / 2;
            if (measuredWidth(value.substr(0, middle) + ellipsis + suffix) <=
                availableWidth)
                low = middle;
            else
                high = middle - 1;
        }
        return value.substr(0, low) + ellipsis + suffix;
    };

    if (mayModify && !fitsInLayout(displayText))
    {
        std::wstring modified = displayText;
        if ((format & GuestAbi::DrawTextPathEllipsis) != 0)
            modified = pathEllipsify(modified);
        if ((format & GuestAbi::DrawTextEndEllipsis) != 0 &&
            !fitsInLayout(modified))
            modified = endEllipsify(modified, false);
        else if ((format & GuestAbi::DrawTextWordEllipsis) != 0 &&
            !fitsInLayout(modified))
            modified = endEllipsify(modified, true);
        if (modified != displayText)
        {
            // Win32 documents DT_MODIFYSTRING as requiring room for four
            // additional WCHARs: at most three dots plus the terminator.
            // Never exceed that contract even when the input count names a
            // buffer that was not originally null-terminated.
            const std::size_t maximumResult = count + 3;
            if (modified.size() > maximumResult) modified.resize(maximumResult);
            for (std::size_t index = 0; index < modified.size(); ++index)
                text[index] = modified[index];
            text[modified.size()] = L'\0';
            displayText = std::move(modified);
            modifiedByEllipsis = true;
        }
    }

    std::size_t mnemonic = static_cast<std::size_t>(-1);
    std::wstring layoutText = modifiedByEllipsis
        ? displayText
        : prepareDisplayText(sourceText, &mnemonic);
    if ((format & GuestAbi::DrawTextHidePrefix) != 0)
        mnemonic = static_cast<std::size_t>(-1);

    MiniGdi::TextLayoutOptions options;
    if ((format & GuestAbi::DrawTextCenter) != 0)
        options.horizontal = MiniGdi::TextHorizontalAlignment::Center;
    else if ((format & GuestAbi::DrawTextRight) != 0)
        options.horizontal = MiniGdi::TextHorizontalAlignment::Right;
    if (singleLine && (format & GuestAbi::DrawTextVCenter) != 0)
        options.vertical = MiniGdi::TextVerticalAlignment::Center;
    else if (singleLine && (format & GuestAbi::DrawTextBottom) != 0)
        options.vertical = MiniGdi::TextVerticalAlignment::Bottom;
    options.wordWrap = !singleLine &&
        (format & GuestAbi::DrawTextWordBreak) != 0;
    options.rightToLeft = (format & GuestAbi::DrawTextRtlReading) != 0;
    options.clipToLayout = (format & GuestAbi::DrawTextNoClip) == 0;
    options.includeExternalLeading =
        (format & GuestAbi::DrawTextExternalLeading) != 0;
    options.renderGlyphs = (format & GuestAbi::DrawTextPrefixOnly) == 0;
    options.mnemonicStart = mnemonic;
    if (!modifiedByEllipsis)
    {
        if ((format & GuestAbi::DrawTextPathEllipsis) != 0)
            options.trimming = MiniGdi::TextTrimming::Path;
        else if ((format & GuestAbi::DrawTextWordEllipsis) != 0)
            options.trimming = MiniGdi::TextTrimming::Word;
        else if ((format & GuestAbi::DrawTextEndEllipsis) != 0)
            options.trimming = MiniGdi::TextTrimming::Character;
    }
    options.tabStop = fitOptions.tabStop;

    constexpr int MaximumLayoutDimension = 1024 * 1024;
    MiniGdi::Rect layoutRect{ rect->left, rect->top, rect->right, rect->bottom };
    if (calculate)
    {
        if (!options.wordWrap)
            layoutRect.right = rect->left + MaximumLayoutDimension;
        layoutRect.bottom = rect->top + MaximumLayoutDimension;
    }
    MiniGdi::Size extent{};
    if (!manager->Gdi().DrawTextW(guestDc, layoutRect,
        layoutText.data(), layoutText.size(), options, !calculate, &extent))
    {
        BridgeSetLastError(ERROR_INVALID_PARAMETER);
        return 0;
    }
    if (calculate)
    {
        rect->right = rect->left + extent.width;
        rect->bottom = rect->top + extent.height;
    }

    BridgeSetLastError(ERROR_SUCCESS);
    return extent.height;
}

int WINAPI Win32Bridge::Bridge::BridgeLoadStringW(HINSTANCE instance, UINT identifier, LPWSTR buffer, int bufferCount)
{
    const int result = LoadGuestStringResource(instance, identifier, buffer, bufferCount);
    BridgeSetLastError(result >= 0 ? ERROR_SUCCESS : ERROR_RESOURCE_NAME_NOT_FOUND);
    return result;
}

BOOL WINAPI Win32Bridge::Bridge::BridgeScreenToClient(HWND window, LPPOINT point)
{
    GuestWindowManager* manager = CurrentManagerOrFail();
    if (!manager || !point)
    {
        BridgeSetLastError(ERROR_INVALID_PARAMETER);
        return FALSE;
    }

    POINT origin{};
    DWORD error = ERROR_SUCCESS;
    if (!manager->GetGuestClientOrigin(window, &origin, &error))
    {
        BridgeSetLastError(error);
        return FALSE;
    }
    point->x -= origin.x;
    point->y -= origin.y;
    BridgeSetLastError(error);
    return TRUE;
}

BOOL WINAPI Win32Bridge::Bridge::BridgeMoveWindow(HWND window, int x, int y, int width, int height, BOOL repaint)
{
    return BridgeSetWindowPos(window, nullptr, x, y, width, height,
        repaint ? SWP_NOZORDER : (SWP_NOZORDER | SWP_NOREDRAW));
}

LPWSTR WINAPI Win32Bridge::Bridge::BridgeCharUpperW(LPWSTR text)
{
    if (!text) return nullptr;
    if (reinterpret_cast<ULONG_PTR>(text) <= 0xffff)
    {
        const wchar_t character = static_cast<wchar_t>(reinterpret_cast<ULONG_PTR>(text));
        return reinterpret_cast<LPWSTR>(static_cast<ULONG_PTR>(towupper(character)));
    }
    for (wchar_t* current = text; *current; ++current) *current = towupper(*current);
    return text;
}

LPSTR WINAPI Win32Bridge::Bridge::BridgeCharPrevExA(UINT, LPCSTR textStart, LPCSTR current, DWORD)
{
    if (!textStart || !current || current <= textStart) return const_cast<LPSTR>(textStart);
    return const_cast<LPSTR>(current - 1);
}

BOOL WINAPI Win32Bridge::Bridge::BridgeSetDlgItemTextA(HWND parent, int identifier, LPCSTR text)
{
    if (!text) return BridgeSetDlgItemTextW(parent, identifier, L"");
    const int count = BridgeMultiByteToWideChar(CP_ACP, 0, text, -1, nullptr, 0);
    if (count <= 0) return FALSE;
    std::vector<wchar_t> wide(static_cast<size_t>(count));
    if (BridgeMultiByteToWideChar(CP_ACP, 0, text, -1, wide.data(), count) != count) return FALSE;
    return BridgeSetDlgItemTextW(parent, identifier, wide.data());
}

BOOL WINAPI Win32Bridge::Bridge::BridgeMapDialogRect(HWND dialog, LPRECT rect)
{
    if (!rect || !BridgeIsWindow(dialog))
    {
        BridgeSetLastError(ERROR_INVALID_PARAMETER);
        return FALSE;
    }
    if (!MapGuestDialogRect(dialog, rect))
    {
        BridgeSetLastError(ERROR_INVALID_WINDOW_HANDLE);
        return FALSE;
    }
    BridgeSetLastError(ERROR_SUCCESS);
    return TRUE;
}

BOOL WINAPI Win32Bridge::Bridge::BridgeCheckDlgButton(HWND dialog, int identifier, UINT check)
{
    return BridgeSendDlgItemMessageW(dialog, identifier, BM_SETCHECK, check, 0) != 0 || BridgeGetLastError() == ERROR_SUCCESS;
}

UINT WINAPI Win32Bridge::Bridge::BridgeIsDlgButtonChecked(HWND dialog, int identifier)
{
    return static_cast<UINT>(BridgeSendDlgItemMessageW(dialog, identifier, BM_GETCHECK, 0, 0));
}

BOOL WINAPI Win32Bridge::Bridge::BridgeCheckRadioButton(HWND dialog, int first, int last, int selected)
{
    if (first > last || selected < first || selected > last)
    {
        BridgeSetLastError(ERROR_INVALID_PARAMETER);
        return FALSE;
    }
    for (int identifier = first; identifier <= last; ++identifier)
        BridgeSendDlgItemMessageW(dialog, identifier, BM_SETCHECK, identifier == selected ? BST_CHECKED : BST_UNCHECKED, 0);
    BridgeSetLastError(ERROR_SUCCESS);
    return TRUE;
}

BOOL WINAPI Win32Bridge::Bridge::BridgeEndDialog(HWND dialog, INT_PTR result)
{
    return EndGuestResourceDialog(dialog, result);
}

BOOL WINAPI Win32Bridge::Bridge::BridgeSystemParametersInfoW(UINT action, UINT, PVOID value, UINT)
{
    if (action == SPI_GETWORKAREA && value)
    {
        *static_cast<RECT*>(value) = { 0, 0, 1280, 720 };
        BridgeSetLastError(ERROR_SUCCESS);
        return TRUE;
    }
    BridgeSetLastError(ERROR_CALL_NOT_IMPLEMENTED);
    return FALSE;
}

HMONITOR WINAPI Win32Bridge::Bridge::BridgeMonitorFromWindow(HWND, DWORD)
{
    return reinterpret_cast<HMONITOR>(static_cast<ULONG_PTR>(1));
}

namespace
{
    // MONITORINFO is a desktop-SDK type.  This layout is the guest ABI used by
    // GetMonitorInfoA and keeps the UWP translation unit desktop-header free.
    struct GuestMonitorInfo
    {
        DWORD cbSize;
        RECT rcMonitor;
        RECT rcWork;
        DWORD dwFlags;
    };
}

BOOL WINAPI Win32Bridge::Bridge::BridgeGetMonitorInfoA(HMONITOR monitor, PVOID monitorInfo)
{
    auto info = static_cast<GuestMonitorInfo*>(monitorInfo);
    if (monitor != reinterpret_cast<HMONITOR>(static_cast<ULONG_PTR>(1)) || !info || info->cbSize < sizeof(GuestMonitorInfo))
    {
        BridgeSetLastError(ERROR_INVALID_PARAMETER);
        return FALSE;
    }
    info->rcMonitor = { 0, 0, 1280, 720 };
    info->rcWork = info->rcMonitor;
    info->dwFlags = 1; // MONITORINFOF_PRIMARY
    BridgeSetLastError(ERROR_SUCCESS);
    return TRUE;
}

BOOL WINAPI Win32Bridge::Bridge::BridgeOpenClipboard(HWND)
{
    BridgeSetLastError(ERROR_SUCCESS);
    return TRUE;
}

BOOL WINAPI Win32Bridge::Bridge::BridgeCloseClipboard()
{
    BridgeSetLastError(ERROR_SUCCESS);
    return TRUE;
}

BOOL WINAPI Win32Bridge::Bridge::BridgeEmptyClipboard()
{
    BridgeSetLastError(ERROR_SUCCESS);
    return TRUE;
}

HANDLE WINAPI Win32Bridge::Bridge::BridgeSetClipboardData(UINT, HANDLE memory)
{
    BridgeSetLastError(ERROR_SUCCESS);
    return memory;
}

UINT WINAPI Win32Bridge::Bridge::BridgeRegisterClipboardFormatW(LPCWSTR)
{
    std::lock_guard<std::mutex> guard(g_menuLock);
    return g_nextClipboardFormat++;
}

HMENU WINAPI Win32Bridge::Bridge::BridgeCreateMenu()
{
    std::lock_guard<std::mutex> guard(g_menuLock);
    const ULONG_PTR handle = g_nextMenu++;
    g_menus.emplace(handle, VirtualMenu{});
    BridgeSetLastError(ERROR_SUCCESS);
    return reinterpret_cast<HMENU>(handle);
}

HMENU WINAPI Win32Bridge::Bridge::BridgeCreatePopupMenu()
{
    return BridgeCreateMenu();
}

BOOL WINAPI Win32Bridge::Bridge::BridgeDestroyMenu(HMENU menu)
{
    std::lock_guard<std::mutex> guard(g_menuLock);
    VirtualMenu* root = FindMenuLocked(menu);
    if (!root)
    {
        BridgeSetLastError(ERROR_INVALID_HANDLE);
        return FALSE;
    }
    std::vector<HMENU> pending{ menu };
    for (size_t index = 0; index < pending.size(); ++index)
    {
        VirtualMenu* current = FindMenuLocked(pending[index]);
        if (!current) continue;
        for (const auto& item : current->items)
            if (item.subMenu && std::find(pending.begin(), pending.end(), item.subMenu) == pending.end())
                pending.push_back(item.subMenu);
    }
    for (HMENU current : pending) g_menus.erase(reinterpret_cast<ULONG_PTR>(current));
    for (auto iterator = g_windowMenus.begin(); iterator != g_windowMenus.end();)
    {
        if (std::find(pending.begin(), pending.end(), iterator->second) != pending.end())
            iterator = g_windowMenus.erase(iterator);
        else ++iterator;
    }
    BridgeSetLastError(ERROR_SUCCESS);
    return TRUE;
}

BOOL WINAPI Win32Bridge::Bridge::BridgeAppendMenuW(HMENU menu, UINT flags, UINT_PTR identifier, LPCWSTR text)
{
    std::lock_guard<std::mutex> guard(g_menuLock);
    VirtualMenu* destination = FindMenuLocked(menu);
    if (!destination)
    {
        BridgeSetLastError(ERROR_INVALID_HANDLE);
        return FALSE;
    }
    VirtualMenuItem item;
    item.identifier = (flags & kMfPopup) ? 0 : static_cast<UINT>(identifier);
    item.type = flags;
    item.state = flags & (kMfGrayed | kMfDisabled | kMfChecked | kMfHighlighted | kMfDefault);
    item.subMenu = (flags & kMfPopup) ? reinterpret_cast<HMENU>(identifier) : nullptr;
    if (text)
    {
        GuestMenuItemInfoW source = {};
        source.dwTypeData = const_cast<LPWSTR>(text);
        item.text = ReadGuestMenuText(source);
    }
    destination->items.push_back(std::move(item));
    BridgeSetLastError(ERROR_SUCCESS);
    return TRUE;
}

BOOL WINAPI Win32Bridge::Bridge::BridgeInsertMenuItemW(HMENU menu, UINT item, BOOL byPosition, const void* itemInfo)
{
    GuestMenuItemInfoW sourceValue = {};
    if (!ReadGuestMenuItemInfo(itemInfo, &sourceValue) ||
        sourceValue.cbSize < GuestMenuItemInfoLegacySize)
    {
        BridgeSetLastError(ERROR_INVALID_PARAMETER);
        return FALSE;
    }
    const auto* source = &sourceValue;
    std::lock_guard<std::mutex> guard(g_menuLock);
    VirtualMenu* destination = FindMenuLocked(menu);
    if (!destination)
    {
        BridgeSetLastError(ERROR_INVALID_HANDLE);
        return FALSE;
    }
    VirtualMenuItem entry;
    if (source->fMask & kMiimId) entry.identifier = source->wID;
    if (source->fMask & (kMiimFtype | kMiimType)) entry.type = source->fType;
    if (source->fMask & kMiimState) entry.state = source->fState;
    if (source->fMask & kMiimSubmenu) entry.subMenu = source->hSubMenu;
    if (source->fMask & kMiimCheckmarks)
    {
        entry.checkedBitmap = source->hbmpChecked;
        entry.uncheckedBitmap = source->hbmpUnchecked;
    }
    if (source->fMask & kMiimBitmap) entry.itemBitmap = source->hbmpItem;
    if (source->fMask & kMiimData) entry.itemData = source->dwItemData;
    if (source->fMask & (kMiimString | kMiimType)) entry.text = ReadGuestMenuText(*source);
    size_t insertion = destination->items.size();
    if (byPosition)
    {
        insertion = (std::min)(static_cast<size_t>(item), destination->items.size());
    }
    else if (item != static_cast<UINT>(-1))
    {
        const auto before = std::find_if(destination->items.begin(), destination->items.end(),
            [item](const VirtualMenuItem& candidate) { return candidate.identifier == item; });
        if (before == destination->items.end()) return FALSE;
        insertion = static_cast<size_t>(before - destination->items.begin());
    }
    destination->items.insert(destination->items.begin() + insertion, std::move(entry));
    BridgeSetLastError(ERROR_SUCCESS);
    return TRUE;
}

int WINAPI Win32Bridge::Bridge::BridgeGetMenuItemCount(HMENU menu)
{
    std::lock_guard<std::mutex> guard(g_menuLock);
    VirtualMenu* found = FindMenuLocked(menu);
    if (!found) return -1;
    return static_cast<int>(found->items.size());
}

BOOL WINAPI Win32Bridge::Bridge::BridgeGetMenuItemInfoW(HMENU menu, UINT item, BOOL byPosition, void* itemInfo)
{
    GuestMenuItemInfoW resultValue = {};
    if (!ReadGuestMenuItemInfo(itemInfo, &resultValue) ||
        resultValue.cbSize < GuestMenuItemInfoLegacySize) return FALSE;
    auto* result = &resultValue;
    std::lock_guard<std::mutex> guard(g_menuLock);
    VirtualMenu* source = FindMenuLocked(menu);
    VirtualMenuItem* entry = source ? FindMenuItemLocked(*source, item, byPosition) : nullptr;
    if (!entry) return FALSE;
    if (result->fMask & kMiimState) result->fState = entry->state;
    if (result->fMask & kMiimId) result->wID = entry->identifier;
    if (result->fMask & kMiimSubmenu) result->hSubMenu = entry->subMenu;
    if (result->fMask & kMiimCheckmarks)
    {
        result->hbmpChecked = entry->checkedBitmap;
        result->hbmpUnchecked = entry->uncheckedBitmap;
    }
    if (result->fMask & kMiimBitmap) result->hbmpItem = entry->itemBitmap;
    if (result->fMask & kMiimData) result->dwItemData = entry->itemData;
    if (result->fMask & (kMiimFtype | kMiimType)) result->fType = entry->type;
    if (result->fMask & (kMiimString | kMiimType))
    {
        if (result->dwTypeData && result->cch)
        {
            if (!CopyGuestMenuText(result->dwTypeData, result->cch, entry->text)) return FALSE;
        }
        result->cch = static_cast<UINT>(entry->text.size());
    }
    return WriteGuestMenuItemInfo(itemInfo, resultValue) ? TRUE : FALSE;
}

BOOL WINAPI Win32Bridge::Bridge::BridgeSetMenuItemInfoW(HMENU menu, UINT item, BOOL byPosition, const void* itemInfo)
{
    GuestMenuItemInfoW updateValue = {};
    if (!ReadGuestMenuItemInfo(itemInfo, &updateValue) ||
        updateValue.cbSize < GuestMenuItemInfoLegacySize) return FALSE;
    const auto* update = &updateValue;
    std::lock_guard<std::mutex> guard(g_menuLock);
    VirtualMenu* destination = FindMenuLocked(menu);
    VirtualMenuItem* entry = destination ? FindMenuItemLocked(*destination, item, byPosition) : nullptr;
    if (!entry) return FALSE;
    if (update->fMask & kMiimState) entry->state = update->fState;
    if (update->fMask & kMiimId) entry->identifier = update->wID;
    if (update->fMask & kMiimSubmenu) entry->subMenu = update->hSubMenu;
    if (update->fMask & kMiimCheckmarks)
    {
        entry->checkedBitmap = update->hbmpChecked;
        entry->uncheckedBitmap = update->hbmpUnchecked;
    }
    if (update->fMask & kMiimBitmap) entry->itemBitmap = update->hbmpItem;
    if (update->fMask & kMiimData) entry->itemData = update->dwItemData;
    if (update->fMask & (kMiimFtype | kMiimType)) entry->type = update->fType;
    if (update->fMask & (kMiimString | kMiimType)) entry->text = ReadGuestMenuText(*update);
    return TRUE;
}

UINT WINAPI Win32Bridge::Bridge::BridgeEnableMenuItem(HMENU menu, UINT item, UINT flags)
{
    std::lock_guard<std::mutex> guard(g_menuLock);
    VirtualMenu* destination = FindMenuLocked(menu);
    VirtualMenuItem* entry = destination ? FindMenuItemLocked(*destination, item, (flags & kMfByPosition) != 0) : nullptr;
    if (!entry) return static_cast<UINT>(-1);
    const UINT previous = entry->state;
    entry->state = (entry->state & ~(kMfGrayed | kMfDisabled)) |
        (flags & (kMfGrayed | kMfDisabled));
    return previous;
}

DWORD WINAPI Win32Bridge::Bridge::BridgeCheckMenuItem(HMENU menu, UINT item, UINT flags)
{
    std::lock_guard<std::mutex> guard(g_menuLock);
    VirtualMenu* destination = FindMenuLocked(menu);
    VirtualMenuItem* entry = destination ? FindMenuItemLocked(*destination, item, (flags & kMfByPosition) != 0) : nullptr;
    if (!entry) return static_cast<DWORD>(-1);
    const DWORD previous = entry->state;
    entry->state = (entry->state & ~kMfChecked) | (flags & kMfChecked);
    return previous;
}

BOOL WINAPI Win32Bridge::Bridge::BridgeCheckMenuRadioItem(HMENU menu, UINT first, UINT last, UINT selected, UINT flags)
{
    std::lock_guard<std::mutex> guard(g_menuLock);
    VirtualMenu* destination = FindMenuLocked(menu);
    if (!destination) return FALSE;
    for (UINT index = first; index <= last; ++index)
    {
        VirtualMenuItem* entry = FindMenuItemLocked(*destination, index, (flags & kMfByPosition) != 0);
        if (entry)
        {
            entry->type |= kMftRadioCheck;
            entry->state = (entry->state & ~kMfChecked) | (index == selected ? kMfChecked : 0);
        }
    }
    return TRUE;
}

BOOL WINAPI Win32Bridge::Bridge::BridgeRemoveMenu(HMENU menu, UINT item, UINT flags)
{
    std::lock_guard<std::mutex> guard(g_menuLock);
    VirtualMenu* destination = FindMenuLocked(menu);
    if (!destination) return FALSE;
    for (auto iterator = destination->items.begin(); iterator != destination->items.end(); ++iterator)
    {
        const bool match = (flags & kMfByPosition) ? static_cast<UINT>(iterator - destination->items.begin()) == item : iterator->identifier == item;
        if (match) { destination->items.erase(iterator); return TRUE; }
    }
    return FALSE;
}

BOOL WINAPI Win32Bridge::Bridge::BridgeDeleteMenu(HMENU menu, UINT item, UINT flags)
{
    HMENU child = nullptr;
    {
        std::lock_guard<std::mutex> guard(g_menuLock);
        VirtualMenu* destination = FindMenuLocked(menu);
        VirtualMenuItem* entry = destination ? FindMenuItemLocked(
            *destination, item, (flags & kMfByPosition) != 0) : nullptr;
        if (!entry) return FALSE;
        child = entry->subMenu;
    }
    if (!BridgeRemoveMenu(menu, item, flags)) return FALSE;
    return !child || BridgeDestroyMenu(child);
}

UINT WINAPI Win32Bridge::Bridge::BridgeGetMenuItemID(HMENU menu, int position)
{
    std::lock_guard<std::mutex> guard(g_menuLock);
    VirtualMenu* source = FindMenuLocked(menu);
    if (!source || position < 0 || static_cast<size_t>(position) >= source->items.size())
        return static_cast<UINT>(-1);
    const VirtualMenuItem& entry = source->items[static_cast<size_t>(position)];
    return entry.subMenu ? static_cast<UINT>(-1) : entry.identifier;
}

UINT WINAPI Win32Bridge::Bridge::BridgeGetMenuState(HMENU menu, UINT item, UINT flags)
{
    std::lock_guard<std::mutex> guard(g_menuLock);
    VirtualMenu* source = FindMenuLocked(menu);
    VirtualMenuItem* entry = source ? FindMenuItemLocked(
        *source, item, (flags & kMfByPosition) != 0) : nullptr;
    if (!entry) return static_cast<UINT>(-1);
    UINT result = entry->type | entry->state | (entry->subMenu ? kMfPopup : 0);
    if (entry->subMenu)
    {
        const VirtualMenu* child = FindMenuLocked(entry->subMenu);
        if (child) result |= (static_cast<UINT>((std::min)(child->items.size(),
            static_cast<size_t>(0xff))) << 8);
    }
    return result;
}

BOOL WINAPI Win32Bridge::Bridge::BridgeWaitMessage()
{
    GuestWindowManager* manager = CurrentManagerOrFail();
    if (!manager) return FALSE;
    GuestAbi::Message message{};
    while (!manager->PeekGuestMessage(&message, nullptr, 0, 0, 0))
        BridgeSleep(1);
    BridgeSetLastError(ERROR_SUCCESS);
    return TRUE;
}

DWORD WINAPI Win32Bridge::Bridge::BridgeMsgWaitForMultipleObjectsEx(
    DWORD count, const HANDLE* handles, DWORD milliseconds, DWORD wakeMask, DWORD flags)
{
    constexpr DWORD supportedFlags = MWMO_WAITALL | MWMO_ALERTABLE | MWMO_INPUTAVAILABLE;
    if (count > MAXIMUM_WAIT_OBJECTS - 1 || (count != 0 && !handles) ||
        (flags & ~supportedFlags) != 0)
    {
        BridgeSetLastError(ERROR_INVALID_PARAMETER);
        return WAIT_FAILED;
    }
    GuestWindowManager* manager = CurrentManagerOrFail();
    if (!manager) return WAIT_FAILED;
    const ULONGLONG started = GetTickCount64();
    for (;;)
    {
        GuestAbi::Message message{};
        const bool inputReady = wakeMask != 0 &&
            manager->PeekGuestMessage(&message, nullptr, 0, 0, 0) != FALSE;
        const bool waitAll = (flags & MWMO_WAITALL) != 0;
        DWORD objectResult = WAIT_TIMEOUT;
        if (count != 0)
            objectResult = BridgeWaitForMultipleObjects(count, handles, waitAll ? TRUE : FALSE, 0);
        const bool objectsReady = count == 0 || objectResult != WAIT_TIMEOUT;
        if (waitAll)
        {
            if (inputReady && objectsReady) return WAIT_OBJECT_0;
        }
        else
        {
            if (objectResult != WAIT_TIMEOUT) return objectResult;
            if (inputReady) return WAIT_OBJECT_0 + count;
        }
        if (milliseconds == 0) return WAIT_TIMEOUT;
        if (milliseconds != INFINITE && GetTickCount64() - started >= milliseconds)
            return WAIT_TIMEOUT;
        BridgeSleep(1);
    }
}

DWORD WINAPI Win32Bridge::Bridge::BridgeMsgWaitForMultipleObjects(
    DWORD count, const HANDLE* handles, BOOL waitAll, DWORD milliseconds, DWORD wakeMask)
{
    return BridgeMsgWaitForMultipleObjectsEx(count, handles, milliseconds, wakeMask,
        waitAll ? MWMO_WAITALL : 0);
}

int WINAPI Win32Bridge::Bridge::BridgeGetMenuStringW(
    HMENU menu, UINT item, LPWSTR text, int count, UINT flags)
{
    std::lock_guard<std::mutex> guard(g_menuLock);
    VirtualMenu* source = FindMenuLocked(menu);
    VirtualMenuItem* entry = source ? FindMenuItemLocked(
        *source, item, (flags & kMfByPosition) != 0) : nullptr;
    if (!entry) return 0;
    if (!text || count <= 0) return static_cast<int>(entry->text.size());
    if (!CopyGuestMenuText(text, static_cast<size_t>(count), entry->text)) return 0;
    return static_cast<int>((std::min)(entry->text.size(), static_cast<size_t>(count - 1)));
}

BOOL WINAPI Win32Bridge::Bridge::BridgeSetMenuDefaultItem(HMENU menu, UINT item, UINT byPosition)
{
    std::lock_guard<std::mutex> guard(g_menuLock);
    VirtualMenu* destination = FindMenuLocked(menu);
    if (!destination) return FALSE;
    for (auto& entry : destination->items) entry.state &= ~kMfDefault;
    if (item == static_cast<UINT>(-1)) return TRUE;
    VirtualMenuItem* selected = FindMenuItemLocked(*destination, item, byPosition != FALSE);
    if (!selected) return FALSE;
    selected->state |= kMfDefault;
    return TRUE;
}

UINT WINAPI Win32Bridge::Bridge::BridgeGetMenuDefaultItem(HMENU menu, UINT byPosition, UINT)
{
    std::lock_guard<std::mutex> guard(g_menuLock);
    VirtualMenu* source = FindMenuLocked(menu);
    if (!source) return static_cast<UINT>(-1);
    for (size_t index = 0; index < source->items.size(); ++index)
        if (source->items[index].state & kMfDefault)
            return byPosition ? static_cast<UINT>(index) : source->items[index].identifier;
    return static_cast<UINT>(-1);
}

BOOL WINAPI Win32Bridge::Bridge::BridgeIsMenu(HMENU menu)
{
    std::lock_guard<std::mutex> guard(g_menuLock);
    return FindMenuLocked(menu) ? TRUE : FALSE;
}

BOOL WINAPI Win32Bridge::Bridge::BridgeHiliteMenuItem(HWND window, HMENU menu, UINT item, UINT flags)
{
    {
        std::lock_guard<std::mutex> guard(g_menuLock);
        VirtualMenu* destination = FindMenuLocked(menu);
        VirtualMenuItem* entry = destination ? FindMenuItemLocked(
            *destination, item, (flags & kMfByPosition) != 0) : nullptr;
        if (!entry) return FALSE;
        entry->state = (entry->state & ~kMfHighlighted) | (flags & kMfHighlighted);
    }
    if (GuestWindowManager* manager = CurrentGuestWindowManager())
        manager->InvalidateGuestRect(window, nullptr, FALSE, nullptr);
    return TRUE;
}

HMENU WINAPI Win32Bridge::Bridge::BridgeGetSubMenu(HMENU menu, int position)
{
    std::lock_guard<std::mutex> guard(g_menuLock);
    VirtualMenu* source = FindMenuLocked(menu);
    return source && position >= 0 && static_cast<size_t>(position) < source->items.size() ? source->items[position].subMenu : nullptr;
}

HMENU WINAPI Win32Bridge::Bridge::BridgeGetMenu(HWND window)
{
    std::lock_guard<std::mutex> guard(g_menuLock);
    const auto found = g_windowMenus.find(reinterpret_cast<ULONG_PTR>(window));
    return found == g_windowMenus.end() ? nullptr : found->second;
}

BOOL WINAPI Win32Bridge::Bridge::BridgeSetMenu(HWND window, HMENU menu)
{
    if (window && !BridgeIsWindow(window)) return FALSE;
    {
        std::lock_guard<std::mutex> guard(g_menuLock);
        if (menu && !FindMenuLocked(menu)) return FALSE;
        if (menu) g_windowMenus[reinterpret_cast<ULONG_PTR>(window)] = menu;
        else g_windowMenus.erase(reinterpret_cast<ULONG_PTR>(window));
    }
    if (GuestWindowManager* manager = CurrentGuestWindowManager())
    {
        DWORD ignored = ERROR_SUCCESS;
        manager->SetGuestWindowMenuBar(window, menu ? TRUE : FALSE, &ignored);
    }
    return TRUE;
}

std::vector<Win32Bridge::Bridge::GuestMenuVisualItem>
Win32Bridge::Bridge::GetGuestMenuItems(HMENU menuHandle)
{
    std::vector<GuestMenuVisualItem> result;
    std::lock_guard<std::mutex> guard(g_menuLock);
    const VirtualMenu* menu = FindMenuLocked(menuHandle);
    if (!menu)
    {
        return result;
    }
    result.reserve(menu->items.size());
    for (const auto& item : menu->items)
    {
        GuestMenuVisualItem visual;
        visual.identifier = item.identifier;
        visual.type = item.type;
        visual.state = item.state;
        visual.subMenu = item.subMenu;
        visual.checkedBitmap = item.checkedBitmap;
        visual.uncheckedBitmap = item.uncheckedBitmap;
        visual.itemBitmap = item.itemBitmap;
        visual.itemData = item.itemData;
        visual.text = item.text;
        result.push_back(std::move(visual));
    }
    return result;
}

std::vector<Win32Bridge::Bridge::GuestMenuVisualItem>
Win32Bridge::Bridge::GetGuestMenuBarItems(HWND window)
{
    HMENU menu = nullptr;
    {
        std::lock_guard<std::mutex> guard(g_menuLock);
        const auto found = g_windowMenus.find(reinterpret_cast<ULONG_PTR>(window));
        if (found == g_windowMenus.end())
        {
            return {};
        }
        menu = found->second;
    }
    return GetGuestMenuItems(menu);
}

BOOL WINAPI Win32Bridge::Bridge::BridgeDrawMenuBar(HWND window)
{
    if (!BridgeIsWindow(window)) return FALSE;
    if (GuestWindowManager* manager = CurrentGuestWindowManager())
    {
        DWORD ignored = ERROR_SUCCESS;
        manager->SetGuestWindowMenuBar(window, BridgeGetMenu(window) ? TRUE : FALSE, &ignored);
        manager->InvalidateGuestRect(window, nullptr, TRUE, &ignored);
    }
    return TRUE;
}
HMENU WINAPI Win32Bridge::Bridge::BridgeLoadMenuW(HINSTANCE instance, LPCWSTR resource)
{
    const HMENU menu = LoadGuestMenuResource(instance, resource);
    BridgeSetLastError(menu ? ERROR_SUCCESS : ERROR_RESOURCE_NAME_NOT_FOUND);
    if (menu)
    {
        RuntimeDiagnostics::Record(L"MENU: loaded MENU resource into virtual menu model.");
    }
    else
    {
        RuntimeDiagnostics::Record(L"MENU: requested resource was unavailable or malformed.");
    }
    return menu;
}
UINT WINAPI Win32Bridge::Bridge::BridgeTrackPopupMenuEx(HMENU menu, UINT flags, int x, int y, HWND owner, const RECT* excludeRect)
{
    {
        std::lock_guard<std::mutex> guard(g_menuLock);
        if (!FindMenuLocked(menu))
        {
            BridgeSetLastError(ERROR_INVALID_MENU_HANDLE);
            return 0;
        }
    }
    GuestWindowManager* manager = CurrentGuestWindowManager();
    if (!manager)
    {
        BridgeSetLastError(ERROR_INVALID_WINDOW_HANDLE);
        return 0;
    }
    DWORD error = ERROR_SUCCESS;
    const UINT result = manager->TrackGuestPopupMenu(menu, flags, x, y, owner, excludeRect, &error);
    BridgeSetLastError(error);
    return result;
}
HANDLE WINAPI Win32Bridge::Bridge::BridgeLoadAcceleratorsW(HINSTANCE, LPCWSTR) { return reinterpret_cast<HANDLE>(static_cast<ULONG_PTR>(1)); }
int WINAPI Win32Bridge::Bridge::BridgeTranslateAcceleratorW(HWND, HANDLE, const GuestAbi::Message*) { return 0; }
UINT WINAPI Win32Bridge::Bridge::BridgeGetDialogBaseUnits()
{
    return static_cast<UINT>(GuestMetrics::TextWidth) |
        (static_cast<UINT>(GuestMetrics::TextHeight) << 16);
}

HWND WINAPI Win32Bridge::Bridge::BridgeChildWindowFromPointEx(HWND parent, POINT point, UINT flags)
{
    GuestWindowManager* manager = CurrentManagerOrFail();
    if (!manager) return nullptr;
    DWORD error = ERROR_SUCCESS;
    const HWND result = manager->ChildGuestWindowFromPoint(parent, point, flags, &error);
    BridgeSetLastError(error);
    return result;
}
BOOL WINAPI Win32Bridge::Bridge::BridgeTrackPopupMenu(
    HMENU menu, UINT flags, int x, int y, int, HWND owner, const RECT*)
{
    return static_cast<BOOL>(BridgeTrackPopupMenuEx(menu, flags, x, y, owner, nullptr));
}
BOOL WINAPI Win32Bridge::Bridge::BridgeEndMenu()
{
    GuestWindowManager* manager = CurrentGuestWindowManager();
    if (!manager) return FALSE;
    DWORD error = ERROR_SUCCESS;
    const BOOL result = manager->EndGuestMenu(&error);
    BridgeSetLastError(error);
    return result;
}

HWND WINAPI Win32Bridge::Bridge::BridgeWindowFromPoint(POINT point)
{
    GuestWindowManager* manager = CurrentManagerOrFail();
    if (!manager) return nullptr;
    DWORD error = ERROR_SUCCESS;
    const HWND result = manager->GuestWindowFromPoint(point, &error);
    BridgeSetLastError(error);
    return result;
}
UINT WINAPI Win32Bridge::Bridge::BridgeMapVirtualKeyW(UINT code, UINT mapType)
{
    return MouseInput().MapVirtualKey(code, mapType);
}
int WINAPI Win32Bridge::Bridge::BridgeMapWindowPoints(
    HWND from,
    HWND to,
    LPPOINT points,
    UINT count)
{
    GuestWindowManager* manager = CurrentManagerOrFail();
    if (!manager || (count != 0 && !points))
    {
        BridgeSetLastError(ERROR_INVALID_PARAMETER);
        return 0;
    }

    POINT fromOrigin{};
    POINT toOrigin{};
    DWORD error = ERROR_SUCCESS;
    if (from && !manager->GetGuestClientOrigin(from, &fromOrigin, &error))
    {
        BridgeSetLastError(error);
        return 0;
    }
    if (to && !manager->GetGuestClientOrigin(to, &toOrigin, &error))
    {
        BridgeSetLastError(error);
        return 0;
    }

    const int deltaX = fromOrigin.x - toOrigin.x;
    const int deltaY = fromOrigin.y - toOrigin.y;
    for (UINT index = 0; index < count; ++index)
    {
        points[index].x += deltaX;
        points[index].y += deltaY;
    }

    BridgeSetLastError(ERROR_SUCCESS);
    return static_cast<int>(MAKELONG(LOWORD(deltaX), LOWORD(deltaY)));
}

BOOL WINAPI Win32Bridge::Bridge::BridgeClientToScreen(HWND window, LPPOINT point)
{
    GuestWindowManager* manager = CurrentManagerOrFail();
    if (!manager || !point)
    {
        BridgeSetLastError(ERROR_INVALID_PARAMETER);
        return FALSE;
    }

    POINT origin{};
    DWORD error = ERROR_SUCCESS;
    if (!manager->GetGuestClientOrigin(window, &origin, &error))
    {
        BridgeSetLastError(error);
        return FALSE;
    }
    point->x += origin.x;
    point->y += origin.y;
    BridgeSetLastError(error);
    return TRUE;
}

BOOL WINAPI Win32Bridge::Bridge::BridgeGetWindowPlacement(HWND window, void* placement)
{
    auto* result = static_cast<GuestWindowPlacement*>(placement);
    if (!result || result->length < sizeof(GuestWindowPlacement) || !BridgeGetWindowRect(window, &result->normalPosition)) return FALSE;
    result->flags = 0;
    // A number of conventional desktop applications use
    // GetWindowPlacement/SetWindowPlacement instead of ShowWindow during
    // their first startup. Preserve the current visibility so their later
    // showCmd update can make a newly-created top-level guest visible.
    result->showCmd = BridgeIsWindowVisible(window) ? 1u : 0u;
    result->minPosition = { 0, 0 };
    result->maxPosition = { 0, 0 };
    return TRUE;
}

BOOL WINAPI Win32Bridge::Bridge::BridgeSetWindowPlacement(HWND window, const void* placement)
{
    const auto* source = static_cast<const GuestWindowPlacement*>(placement);
    if (!source || source->length < sizeof(GuestWindowPlacement)) return FALSE;
    const RECT& rect = source->normalPosition;
    if (!BridgeSetWindowPos(window, nullptr, rect.left, rect.top, rect.right - rect.left, rect.bottom - rect.top, 0))
    {
        return FALSE;
    }

    // SW_HIDE is the only hiding command. Every other documented showCmd
    // value (normal, minimized, maximized, restore, and default) is presented
    // as an active guest surface in this initial client-only window model.
    // Do not return ShowWindow's "previously visible" result: successful
    // SetWindowPlacement reports TRUE even when it changed hidden -> shown.
    BridgeShowWindow(window, source->showCmd == 0 ? 0 : 1);
    RuntimeDiagnostics::Record(
        L"WINDOW PLACEMENT: showCmd " + std::to_wstring(source->showCmd) + L" applied.");
    return TRUE;
}

HBITMAP WINAPI Win32Bridge::Bridge::BridgeLoadBitmapW(HINSTANCE instance, LPCWSTR resource)
{
    GuestWindowManager* manager = CurrentManagerOrFail();
    MiniGdi::Surface image;
    if (!manager || !resource || !LoadBitmapResourcePixels(instance, resource, &image))
    {
        BridgeSetLastError(ERROR_RESOURCE_NAME_NOT_FOUND);
        return nullptr;
    }
    const MiniGdi::ObjectHandle bitmap = manager->Gdi().CreateBitmap(
        image.Width(), image.Height(), MiniGdi::Transparent);
    MiniGdi::Surface* target = manager->Gdi().GetBitmapSurface(bitmap);
    if (bitmap == MiniGdi::InvalidObject || !target)
    {
        BridgeSetLastError(ERROR_NOT_ENOUGH_MEMORY);
        return nullptr;
    }
    target->Pixels() = image.Pixels();
    RuntimeDiagnostics::Record(L"BITMAP: decoded a guest RT_BITMAP resource.");
    BridgeSetLastError(ERROR_SUCCESS);
    return reinterpret_cast<HBITMAP>(static_cast<ULONG_PTR>(bitmap));
}
HBITMAP WINAPI Win32Bridge::Bridge::BridgeLoadBitmapA(HINSTANCE instance, LPCSTR resource)
{
    std::wstring storage;
    LPCWSTR converted = nullptr;
    if (!ConvertAnsiResourceName(resource, &storage, &converted))
    {
        BridgeSetLastError(ERROR_NO_UNICODE_TRANSLATION);
        return nullptr;
    }
    return BridgeLoadBitmapW(instance, converted);
}

BOOL WINAPI Win32Bridge::Bridge::BridgeGetClassInfoW(HINSTANCE, LPCWSTR, GuestAbi::WndClassW*) { BridgeSetLastError(ERROR_INVALID_PARAMETER); return FALSE; }
LRESULT WINAPI Win32Bridge::Bridge::BridgeCallWindowProcW(GuestAbi::WndProc procedure, HWND window, UINT message, WPARAM wParam, LPARAM lParam)
{
    if (!procedure)
    {
        BridgeSetLastError(ERROR_INVALID_PARAMETER);
        return 0;
    }

    DWORD guestException = ERROR_SUCCESS;
    const LRESULT result = InvokeGuestSubclassProcedure(
        procedure,
        window,
        message,
        wParam,
        lParam,
        &guestException);
    if (guestException != ERROR_SUCCESS)
    {
        RuntimeDiagnostics::Record(
            L"GUEST SUBCLASS EXCEPTION: code " +
            std::to_wstring(static_cast<unsigned long>(guestException)) +
            L", message " + std::to_wstring(message) +
            L", window " + std::to_wstring(reinterpret_cast<ULONG_PTR>(window)) + L".");
        BridgeSetLastError(ERROR_EXCEPTION_IN_SERVICE);
        return 0;
    }
    BridgeSetLastError(ERROR_SUCCESS);
    return result;
}

INT_PTR WINAPI Win32Bridge::Bridge::BridgeDialogBoxParamW(HINSTANCE instance, LPCWSTR templateName, HWND parent, DLGPROC dialogProcedure, LPARAM initParameter)
{
    return ShowGuestDialogFromResource(instance, templateName, parent, dialogProcedure, initParameter);
}

#include "Bridge/User32CompatibilityShims.inl"

BOOL WINAPI Win32Bridge::Bridge::BridgeIsDialogMessageW(
    HWND dialog,
    const GuestAbi::Message* message)
{
    if (!message) return FALSE;
    GuestAbi::Message copy{};
    if (!ReadGuestMessageValue(message, &copy)) return FALSE;
    MSG native{};
    native.hwnd = copy.hwnd;
    native.message = copy.message;
    native.wParam = copy.wParam;
    native.lParam = copy.lParam;
    return HandleGuestDialogMessage(dialog, &native);
}

ImportResolution Win32Bridge::Bridge::ResolveUser32Import(const ImportedSymbol& symbol)
{
    ImportResolution resolution = CompatibilityCatalog::Resolve(symbol);
    if (symbol.importedByOrdinal || !IsUserLibrary(symbol.library))
    {
        return resolution;
    }

    if (_wcsicmp(symbol.name.c_str(), L"registerclassexw") == 0)
        resolution.targetAddress = reinterpret_cast<ULONGLONG>(&BridgeRegisterClassExW);
    else if (_wcsicmp(symbol.name.c_str(), L"registerclassw") == 0)
        resolution.targetAddress = reinterpret_cast<ULONGLONG>(&BridgeRegisterClassW);
    else if (_wcsicmp(symbol.name.c_str(), L"createwindowexw") == 0)
        resolution.targetAddress = reinterpret_cast<ULONGLONG>(&BridgeCreateWindowExW);
    else if (_wcsicmp(symbol.name.c_str(), L"loadcursorw") == 0)
        resolution.targetAddress = reinterpret_cast<ULONGLONG>(&BridgeLoadCursorW);
    else if (_wcsicmp(symbol.name.c_str(), L"loadcursora") == 0)
        resolution.targetAddress = reinterpret_cast<ULONGLONG>(&BridgeLoadCursorA);
    else if (_wcsicmp(symbol.name.c_str(), L"loadcursorfromfilew") == 0)
        resolution.targetAddress = reinterpret_cast<ULONGLONG>(&BridgeLoadCursorFromFileW);
    else if (_wcsicmp(symbol.name.c_str(), L"loadcursorfromfilea") == 0)
        resolution.targetAddress = reinterpret_cast<ULONGLONG>(&BridgeLoadCursorFromFileA);
    else if (_wcsicmp(symbol.name.c_str(), L"loadiconw") == 0)
        resolution.targetAddress = reinterpret_cast<ULONGLONG>(&BridgeLoadIconW);
    else if (_wcsicmp(symbol.name.c_str(), L"loadicona") == 0)
        resolution.targetAddress = reinterpret_cast<ULONGLONG>(&BridgeLoadIconA);
    else if (_wcsicmp(symbol.name.c_str(), L"loadimagew") == 0)
        resolution.targetAddress = reinterpret_cast<ULONGLONG>(&BridgeLoadImageW);
    else if (_wcsicmp(symbol.name.c_str(), L"loadimagea") == 0)
        resolution.targetAddress = reinterpret_cast<ULONGLONG>(&BridgeLoadImageA);
    else if (_wcsicmp(symbol.name.c_str(), L"createiconfromresourceex") == 0)
        resolution.targetAddress = reinterpret_cast<ULONGLONG>(&BridgeCreateIconFromResourceEx);
    else if (_wcsicmp(symbol.name.c_str(), L"createiconfromresource") == 0)
        resolution.targetAddress = reinterpret_cast<ULONGLONG>(&BridgeCreateIconFromResource);
    else if (_wcsicmp(symbol.name.c_str(), L"copyimage") == 0)
        resolution.targetAddress = reinterpret_cast<ULONGLONG>(&BridgeCopyImage);
    else if (_wcsicmp(symbol.name.c_str(), L"copyicon") == 0)
        resolution.targetAddress = reinterpret_cast<ULONGLONG>(&BridgeCopyIcon);
    else if (_wcsicmp(symbol.name.c_str(), L"drawicon") == 0)
        resolution.targetAddress = reinterpret_cast<ULONGLONG>(&BridgeDrawIcon);
    else if (_wcsicmp(symbol.name.c_str(), L"drawiconex") == 0)
        resolution.targetAddress = reinterpret_cast<ULONGLONG>(&BridgeDrawIconEx);
    else if (_wcsicmp(symbol.name.c_str(), L"setcursor") == 0)
        resolution.targetAddress = reinterpret_cast<ULONGLONG>(&BridgeSetCursor);
    else if (_wcsicmp(symbol.name.c_str(), L"destroycursor") == 0)
        resolution.targetAddress = reinterpret_cast<ULONGLONG>(&BridgeDestroyCursor);
    else if (_wcsicmp(symbol.name.c_str(), L"destroyicon") == 0)
        resolution.targetAddress = reinterpret_cast<ULONGLONG>(&BridgeDestroyIcon);
    else if (_wcsicmp(symbol.name.c_str(), L"getsystemmetrics") == 0)
        resolution.targetAddress = reinterpret_cast<ULONGLONG>(&BridgeGetSystemMetrics);
    else if (_wcsicmp(symbol.name.c_str(), L"getsyscolor") == 0)
        resolution.targetAddress = reinterpret_cast<ULONGLONG>(&BridgeGetSysColor);
    else if (_wcsicmp(symbol.name.c_str(), L"getsyscolorbrush") == 0)
        resolution.targetAddress = reinterpret_cast<ULONGLONG>(&BridgeGetSysColorBrush);
    else if (_wcsicmp(symbol.name.c_str(), L"adjustwindowrect") == 0)
        resolution.targetAddress = reinterpret_cast<ULONGLONG>(&BridgeAdjustWindowRect);
    else if (_wcsicmp(symbol.name.c_str(), L"adjustwindowrectex") == 0)
        resolution.targetAddress = reinterpret_cast<ULONGLONG>(&BridgeAdjustWindowRectEx);
    if (_wcsicmp(symbol.name.c_str(), L"destroywindow") == 0)
        resolution.targetAddress = reinterpret_cast<ULONGLONG>(&BridgeDestroyWindow);
    else if (_wcsicmp(symbol.name.c_str(), L"showwindow") == 0)
        resolution.targetAddress = reinterpret_cast<ULONGLONG>(&BridgeShowWindow);
    else if (_wcsicmp(symbol.name.c_str(), L"getclientrect") == 0)
        resolution.targetAddress = reinterpret_cast<ULONGLONG>(&BridgeGetClientRect);
    else if (_wcsicmp(symbol.name.c_str(), L"getwindowrect") == 0)
        resolution.targetAddress = reinterpret_cast<ULONGLONG>(&BridgeGetWindowRect);
    else if (_wcsicmp(symbol.name.c_str(), L"setwindowtextw") == 0)
        resolution.targetAddress = reinterpret_cast<ULONGLONG>(&BridgeSetWindowTextW);
    else if (_wcsicmp(symbol.name.c_str(), L"getwindowtextw") == 0)
        resolution.targetAddress = reinterpret_cast<ULONGLONG>(&BridgeGetWindowTextW);
    else if (_wcsicmp(symbol.name.c_str(), L"getwindowtextlengthw") == 0)
        resolution.targetAddress = reinterpret_cast<ULONGLONG>(&BridgeGetWindowTextLengthW);
    else if (_wcsicmp(symbol.name.c_str(), L"getdlgitem") == 0)
        resolution.targetAddress = reinterpret_cast<ULONGLONG>(&BridgeGetDlgItem);
    else if (_wcsicmp(symbol.name.c_str(), L"getdlgctrlid") == 0)
        resolution.targetAddress = reinterpret_cast<ULONGLONG>(&BridgeGetDlgCtrlID);
    else if (_wcsicmp(symbol.name.c_str(), L"setdlgitemtextw") == 0)
        resolution.targetAddress = reinterpret_cast<ULONGLONG>(&BridgeSetDlgItemTextW);
    else if (_wcsicmp(symbol.name.c_str(), L"getdlgitemtextw") == 0)
        resolution.targetAddress = reinterpret_cast<ULONGLONG>(&BridgeGetDlgItemTextW);
    else if (_wcsicmp(symbol.name.c_str(), L"senddlgitemmessagew") == 0)
        resolution.targetAddress = reinterpret_cast<ULONGLONG>(&BridgeSendDlgItemMessageW);
    else if (_wcsicmp(symbol.name.c_str(), L"iswindow") == 0)
        resolution.targetAddress = reinterpret_cast<ULONGLONG>(&BridgeIsWindow);
    else if (_wcsicmp(symbol.name.c_str(), L"iswindowvisible") == 0)
        resolution.targetAddress = reinterpret_cast<ULONGLONG>(&BridgeIsWindowVisible);
    else if (_wcsicmp(symbol.name.c_str(), L"iswindowenabled") == 0)
        resolution.targetAddress = reinterpret_cast<ULONGLONG>(&BridgeIsWindowEnabled);
    else if (_wcsicmp(symbol.name.c_str(), L"iszoomed") == 0)
        resolution.targetAddress = reinterpret_cast<ULONGLONG>(&BridgeIsZoomed);
    else if (_wcsicmp(symbol.name.c_str(), L"enablewindow") == 0)
        resolution.targetAddress = reinterpret_cast<ULONGLONG>(&BridgeEnableWindow);
    else if (_wcsicmp(symbol.name.c_str(), L"setfocus") == 0)
        resolution.targetAddress = reinterpret_cast<ULONGLONG>(&BridgeSetFocus);
    else if (_wcsicmp(symbol.name.c_str(), L"getfocus") == 0)
        resolution.targetAddress = reinterpret_cast<ULONGLONG>(&BridgeGetFocus);
    else if (_wcsicmp(symbol.name.c_str(), L"setcapture") == 0)
        resolution.targetAddress = reinterpret_cast<ULONGLONG>(&BridgeSetCapture);
    else if (_wcsicmp(symbol.name.c_str(), L"releasecapture") == 0)
        resolution.targetAddress = reinterpret_cast<ULONGLONG>(&BridgeReleaseCapture);
    else if (_wcsicmp(symbol.name.c_str(), L"getcapture") == 0)
        resolution.targetAddress = reinterpret_cast<ULONGLONG>(&BridgeGetCapture);
    if (_wcsicmp(symbol.name.c_str(), L"getparent") == 0)
        resolution.targetAddress = reinterpret_cast<ULONGLONG>(&BridgeGetParent);
    else if (_wcsicmp(symbol.name.c_str(), L"getwindowlongw") == 0)
        resolution.targetAddress = reinterpret_cast<ULONGLONG>(&BridgeGetWindowLongW);
    else if (_wcsicmp(symbol.name.c_str(), L"setwindowlongw") == 0)
        resolution.targetAddress = reinterpret_cast<ULONGLONG>(&BridgeSetWindowLongW);
    else if (_wcsicmp(symbol.name.c_str(), L"getwindowlongptrw") == 0)
        resolution.targetAddress = reinterpret_cast<ULONGLONG>(&BridgeGetWindowLongPtrW);
    else if (_wcsicmp(symbol.name.c_str(), L"setwindowlongptrw") == 0)
        resolution.targetAddress = reinterpret_cast<ULONGLONG>(&BridgeSetWindowLongPtrW);
    else if (_wcsicmp(symbol.name.c_str(), L"setwindowpos") == 0)
        resolution.targetAddress = reinterpret_cast<ULONGLONG>(&BridgeSetWindowPos);
    else if (_wcsicmp(symbol.name.c_str(), L"settimer") == 0)
        resolution.targetAddress = reinterpret_cast<ULONGLONG>(&BridgeSetTimer);
    else if (_wcsicmp(symbol.name.c_str(), L"killtimer") == 0)
        resolution.targetAddress = reinterpret_cast<ULONGLONG>(&BridgeKillTimer);
    else if (_wcsicmp(symbol.name.c_str(), L"defwindowprocw") == 0)
        resolution.targetAddress = reinterpret_cast<ULONGLONG>(&BridgeDefWindowProcW);
    else if (_wcsicmp(symbol.name.c_str(), L"postmessagew") == 0)
        resolution.targetAddress = reinterpret_cast<ULONGLONG>(&BridgePostMessageW);
    else if (_wcsicmp(symbol.name.c_str(), L"sendmessagew") == 0)
        resolution.targetAddress = reinterpret_cast<ULONGLONG>(&BridgeSendMessageW);
    else if (_wcsicmp(symbol.name.c_str(), L"postquitmessage") == 0)
        resolution.targetAddress = reinterpret_cast<ULONGLONG>(&BridgePostQuitMessage);
    else if (_wcsicmp(symbol.name.c_str(), L"getmessagew") == 0)
        resolution.targetAddress = reinterpret_cast<ULONGLONG>(&BridgeGetMessageW);
    else if (_wcsicmp(symbol.name.c_str(), L"peekmessagew") == 0)
        resolution.targetAddress = reinterpret_cast<ULONGLONG>(&BridgePeekMessageW);
    else if (_wcsicmp(symbol.name.c_str(), L"waitmessage") == 0)
        resolution.targetAddress = reinterpret_cast<ULONGLONG>(&BridgeWaitMessage);
    else if (_wcsicmp(symbol.name.c_str(), L"msgwaitformultipleobjectsex") == 0)
        resolution.targetAddress = reinterpret_cast<ULONGLONG>(&BridgeMsgWaitForMultipleObjectsEx);
    else if (_wcsicmp(symbol.name.c_str(), L"msgwaitformultipleobjects") == 0)
        resolution.targetAddress = reinterpret_cast<ULONGLONG>(&BridgeMsgWaitForMultipleObjects);
    else if (_wcsicmp(symbol.name.c_str(), L"translatemessage") == 0)
        resolution.targetAddress = reinterpret_cast<ULONGLONG>(&BridgeTranslateMessage);
    else if (_wcsicmp(symbol.name.c_str(), L"dispatchmessagew") == 0)
        resolution.targetAddress = reinterpret_cast<ULONGLONG>(&BridgeDispatchMessageW);
    else if (_wcsicmp(symbol.name.c_str(), L"invalidaterect") == 0)
        resolution.targetAddress = reinterpret_cast<ULONGLONG>(&BridgeInvalidateRect);
    else if (_wcsicmp(symbol.name.c_str(), L"updatewindow") == 0)
        resolution.targetAddress = reinterpret_cast<ULONGLONG>(&BridgeUpdateWindow);
    else if (_wcsicmp(symbol.name.c_str(), L"beginpaint") == 0)
        resolution.targetAddress = reinterpret_cast<ULONGLONG>(&BridgeBeginPaint);
    else if (_wcsicmp(symbol.name.c_str(), L"endpaint") == 0)
        resolution.targetAddress = reinterpret_cast<ULONGLONG>(&BridgeEndPaint);
    if (_wcsicmp(symbol.name.c_str(), L"getdc") == 0)
        resolution.targetAddress = reinterpret_cast<ULONGLONG>(&BridgeGetDC);
    else if (_wcsicmp(symbol.name.c_str(), L"releasedc") == 0)
        resolution.targetAddress = reinterpret_cast<ULONGLONG>(&BridgeReleaseDC);
    else if (_wcsicmp(symbol.name.c_str(), L"fillrect") == 0)
        resolution.targetAddress = reinterpret_cast<ULONGLONG>(&BridgeFillRect);
    else if (_wcsicmp(symbol.name.c_str(), L"drawtextw") == 0)
        resolution.targetAddress = reinterpret_cast<ULONGLONG>(&BridgeDrawTextW);
    else if (_wcsicmp(symbol.name.c_str(), L"loadstringw") == 0)
        resolution.targetAddress = reinterpret_cast<ULONGLONG>(&BridgeLoadStringW);
    else if (_wcsicmp(symbol.name.c_str(), L"screentoclient") == 0)
        resolution.targetAddress = reinterpret_cast<ULONGLONG>(&BridgeScreenToClient);
    else if (_wcsicmp(symbol.name.c_str(), L"mapwindowpoints") == 0)
        resolution.targetAddress = reinterpret_cast<ULONGLONG>(&BridgeMapWindowPoints);
    else if (_wcsicmp(symbol.name.c_str(), L"movewindow") == 0)
        resolution.targetAddress = reinterpret_cast<ULONGLONG>(&BridgeMoveWindow);
    else if (_wcsicmp(symbol.name.c_str(), L"charupperw") == 0)
        resolution.targetAddress = reinterpret_cast<ULONGLONG>(&BridgeCharUpperW);
    else if (_wcsicmp(symbol.name.c_str(), L"charprevexa") == 0)
        resolution.targetAddress = reinterpret_cast<ULONGLONG>(&BridgeCharPrevExA);
    else if (_wcsicmp(symbol.name.c_str(), L"getkeystate") == 0)
        resolution.targetAddress = reinterpret_cast<ULONGLONG>(&BridgeGetKeyState);
    else if (_wcsicmp(symbol.name.c_str(), L"setdlgitemtexta") == 0)
        resolution.targetAddress = reinterpret_cast<ULONGLONG>(&BridgeSetDlgItemTextA);
    if (_wcsicmp(symbol.name.c_str(), L"mapdialogrect") == 0)
        resolution.targetAddress = reinterpret_cast<ULONGLONG>(&BridgeMapDialogRect);
    else if (_wcsicmp(symbol.name.c_str(), L"checkdlgbutton") == 0)
        resolution.targetAddress = reinterpret_cast<ULONGLONG>(&BridgeCheckDlgButton);
    else if (_wcsicmp(symbol.name.c_str(), L"isdlgbuttonchecked") == 0)
        resolution.targetAddress = reinterpret_cast<ULONGLONG>(&BridgeIsDlgButtonChecked);
    else if (_wcsicmp(symbol.name.c_str(), L"checkradiobutton") == 0)
        resolution.targetAddress = reinterpret_cast<ULONGLONG>(&BridgeCheckRadioButton);
    else if (_wcsicmp(symbol.name.c_str(), L"enddialog") == 0)
        resolution.targetAddress = reinterpret_cast<ULONGLONG>(&BridgeEndDialog);
    else if (_wcsicmp(symbol.name.c_str(), L"systemparametersinfow") == 0)
        resolution.targetAddress = reinterpret_cast<ULONGLONG>(&BridgeSystemParametersInfoW);
    else if (_wcsicmp(symbol.name.c_str(), L"monitorfromwindow") == 0)
        resolution.targetAddress = reinterpret_cast<ULONGLONG>(&BridgeMonitorFromWindow);
    else if (_wcsicmp(symbol.name.c_str(), L"getmonitorinfoa") == 0 || _wcsicmp(symbol.name.c_str(), L"getmonitorinfow") == 0)
        resolution.targetAddress = reinterpret_cast<ULONGLONG>(&BridgeGetMonitorInfoA);
    if (_wcsicmp(symbol.name.c_str(), L"openclipboard") == 0)
        resolution.targetAddress = reinterpret_cast<ULONGLONG>(&BridgeOpenClipboard);
    else if (_wcsicmp(symbol.name.c_str(), L"closeclipboard") == 0)
        resolution.targetAddress = reinterpret_cast<ULONGLONG>(&BridgeCloseClipboard);
    else if (_wcsicmp(symbol.name.c_str(), L"emptyclipboard") == 0)
        resolution.targetAddress = reinterpret_cast<ULONGLONG>(&BridgeEmptyClipboard);
    else if (_wcsicmp(symbol.name.c_str(), L"setclipboarddata") == 0)
        resolution.targetAddress = reinterpret_cast<ULONGLONG>(&BridgeSetClipboardData);
    else if (_wcsicmp(symbol.name.c_str(), L"registerclipboardformatw") == 0)
        resolution.targetAddress = reinterpret_cast<ULONGLONG>(&BridgeRegisterClipboardFormatW);
    else if (_wcsicmp(symbol.name.c_str(), L"createmenu") == 0)
        resolution.targetAddress = reinterpret_cast<ULONGLONG>(&BridgeCreateMenu);
    else if (_wcsicmp(symbol.name.c_str(), L"createpopupmenu") == 0)
        resolution.targetAddress = reinterpret_cast<ULONGLONG>(&BridgeCreatePopupMenu);
    else if (_wcsicmp(symbol.name.c_str(), L"destroymenu") == 0)
        resolution.targetAddress = reinterpret_cast<ULONGLONG>(&BridgeDestroyMenu);
    else if (_wcsicmp(symbol.name.c_str(), L"appendmenuw") == 0)
        resolution.targetAddress = reinterpret_cast<ULONGLONG>(&BridgeAppendMenuW);
    if (_wcsicmp(symbol.name.c_str(), L"insertmenuitemw") == 0)
        resolution.targetAddress = reinterpret_cast<ULONGLONG>(&BridgeInsertMenuItemW);
    else if (_wcsicmp(symbol.name.c_str(), L"getmenuitemcount") == 0)
        resolution.targetAddress = reinterpret_cast<ULONGLONG>(&BridgeGetMenuItemCount);
    else if (_wcsicmp(symbol.name.c_str(), L"getmenuiteminfow") == 0)
        resolution.targetAddress = reinterpret_cast<ULONGLONG>(&BridgeGetMenuItemInfoW);
    else if (_wcsicmp(symbol.name.c_str(), L"setmenuiteminfow") == 0)
        resolution.targetAddress = reinterpret_cast<ULONGLONG>(&BridgeSetMenuItemInfoW);
    else if (_wcsicmp(symbol.name.c_str(), L"enablemenuitem") == 0)
        resolution.targetAddress = reinterpret_cast<ULONGLONG>(&BridgeEnableMenuItem);
    else if (_wcsicmp(symbol.name.c_str(), L"checkmenuitem") == 0)
        resolution.targetAddress = reinterpret_cast<ULONGLONG>(&BridgeCheckMenuItem);
    else if (_wcsicmp(symbol.name.c_str(), L"checkmenuradioitem") == 0)
        resolution.targetAddress = reinterpret_cast<ULONGLONG>(&BridgeCheckMenuRadioItem);
    else if (_wcsicmp(symbol.name.c_str(), L"removemenu") == 0)
        resolution.targetAddress = reinterpret_cast<ULONGLONG>(&BridgeRemoveMenu);
    else if (_wcsicmp(symbol.name.c_str(), L"deletemenu") == 0)
        resolution.targetAddress = reinterpret_cast<ULONGLONG>(&BridgeDeleteMenu);
    else if (_wcsicmp(symbol.name.c_str(), L"getmenuitemid") == 0)
        resolution.targetAddress = reinterpret_cast<ULONGLONG>(&BridgeGetMenuItemID);
    else if (_wcsicmp(symbol.name.c_str(), L"getmenustate") == 0)
        resolution.targetAddress = reinterpret_cast<ULONGLONG>(&BridgeGetMenuState);
    else if (_wcsicmp(symbol.name.c_str(), L"getmenustringw") == 0)
        resolution.targetAddress = reinterpret_cast<ULONGLONG>(&BridgeGetMenuStringW);
    else if (_wcsicmp(symbol.name.c_str(), L"setmenudefaultitem") == 0)
        resolution.targetAddress = reinterpret_cast<ULONGLONG>(&BridgeSetMenuDefaultItem);
    else if (_wcsicmp(symbol.name.c_str(), L"getmenudefaultitem") == 0)
        resolution.targetAddress = reinterpret_cast<ULONGLONG>(&BridgeGetMenuDefaultItem);
    else if (_wcsicmp(symbol.name.c_str(), L"ismenu") == 0)
        resolution.targetAddress = reinterpret_cast<ULONGLONG>(&BridgeIsMenu);
    else if (_wcsicmp(symbol.name.c_str(), L"hilitemenuitem") == 0)
        resolution.targetAddress = reinterpret_cast<ULONGLONG>(&BridgeHiliteMenuItem);
    else if (_wcsicmp(symbol.name.c_str(), L"getsubmenu") == 0)
        resolution.targetAddress = reinterpret_cast<ULONGLONG>(&BridgeGetSubMenu);
    else if (_wcsicmp(symbol.name.c_str(), L"getmenu") == 0)
        resolution.targetAddress = reinterpret_cast<ULONGLONG>(&BridgeGetMenu);
    else if (_wcsicmp(symbol.name.c_str(), L"setmenu") == 0)
        resolution.targetAddress = reinterpret_cast<ULONGLONG>(&BridgeSetMenu);
    else if (_wcsicmp(symbol.name.c_str(), L"drawmenubar") == 0)
        resolution.targetAddress = reinterpret_cast<ULONGLONG>(&BridgeDrawMenuBar);
    else if (_wcsicmp(symbol.name.c_str(), L"loadmenuw") == 0)
        resolution.targetAddress = reinterpret_cast<ULONGLONG>(&BridgeLoadMenuW);
    else if (_wcsicmp(symbol.name.c_str(), L"trackpopupmenuex") == 0)
        resolution.targetAddress = reinterpret_cast<ULONGLONG>(&BridgeTrackPopupMenuEx);
    else if (_wcsicmp(symbol.name.c_str(), L"trackpopupmenu") == 0)
        resolution.targetAddress = reinterpret_cast<ULONGLONG>(&BridgeTrackPopupMenu);
    else if (_wcsicmp(symbol.name.c_str(), L"endmenu") == 0)
        resolution.targetAddress = reinterpret_cast<ULONGLONG>(&BridgeEndMenu);
    if (_wcsicmp(symbol.name.c_str(), L"loadacceleratorsw") == 0)
        resolution.targetAddress = reinterpret_cast<ULONGLONG>(&BridgeLoadAcceleratorsW);
    else if (_wcsicmp(symbol.name.c_str(), L"translateacceleratorw") == 0)
        resolution.targetAddress = reinterpret_cast<ULONGLONG>(&BridgeTranslateAcceleratorW);
    else if (_wcsicmp(symbol.name.c_str(), L"getdialogbaseunits") == 0)
        resolution.targetAddress = reinterpret_cast<ULONGLONG>(&BridgeGetDialogBaseUnits);
    else if (_wcsicmp(symbol.name.c_str(), L"childwindowfrompointex") == 0)
        resolution.targetAddress = reinterpret_cast<ULONGLONG>(&BridgeChildWindowFromPointEx);
    else if (_wcsicmp(symbol.name.c_str(), L"windowfrompoint") == 0)
        resolution.targetAddress = reinterpret_cast<ULONGLONG>(&BridgeWindowFromPoint);
    else if (_wcsicmp(symbol.name.c_str(), L"mapvirtualkeyw") == 0)
        resolution.targetAddress = reinterpret_cast<ULONGLONG>(&BridgeMapVirtualKeyW);
    else if (_wcsicmp(symbol.name.c_str(), L"clienttoscreen") == 0)
        resolution.targetAddress = reinterpret_cast<ULONGLONG>(&BridgeClientToScreen);
    else if (_wcsicmp(symbol.name.c_str(), L"getwindowplacement") == 0)
        resolution.targetAddress = reinterpret_cast<ULONGLONG>(&BridgeGetWindowPlacement);
    else if (_wcsicmp(symbol.name.c_str(), L"setwindowplacement") == 0)
        resolution.targetAddress = reinterpret_cast<ULONGLONG>(&BridgeSetWindowPlacement);
    else if (_wcsicmp(symbol.name.c_str(), L"loadbitmapw") == 0)
        resolution.targetAddress = reinterpret_cast<ULONGLONG>(&BridgeLoadBitmapW);
    else if (_wcsicmp(symbol.name.c_str(), L"loadbitmapa") == 0)
        resolution.targetAddress = reinterpret_cast<ULONGLONG>(&BridgeLoadBitmapA);
    else if (_wcsicmp(symbol.name.c_str(), L"getclassinfow") == 0)
        resolution.targetAddress = reinterpret_cast<ULONGLONG>(&BridgeGetClassInfoW);
    else if (_wcsicmp(symbol.name.c_str(), L"callwindowprocw") == 0)
        resolution.targetAddress = reinterpret_cast<ULONGLONG>(&BridgeCallWindowProcW);
    else if (_wcsicmp(symbol.name.c_str(), L"dialogboxparamw") == 0)
        resolution.targetAddress = reinterpret_cast<ULONGLONG>(&BridgeDialogBoxParamW);
    else if (_wcsicmp(symbol.name.c_str(), L"isdialogmessagew") == 0)
        resolution.targetAddress = reinterpret_cast<ULONGLONG>(&BridgeIsDialogMessageW);

    if (_wcsicmp(symbol.name.c_str(), L"winhelpw") == 0) resolution.targetAddress = reinterpret_cast<ULONGLONG>(&BridgeWinHelpW);
    else if (_wcsicmp(symbol.name.c_str(), L"childwindowfrompoint") == 0) resolution.targetAddress = reinterpret_cast<ULONGLONG>(&BridgeChildWindowFromPoint);
    else if (_wcsicmp(symbol.name.c_str(), L"getsystemmenu") == 0) resolution.targetAddress = reinterpret_cast<ULONGLONG>(&BridgeGetSystemMenu);
    else if (_wcsicmp(symbol.name.c_str(), L"registerwindowmessagew") == 0) resolution.targetAddress = reinterpret_cast<ULONGLONG>(&BridgeRegisterWindowMessageW);
    else if (_wcsicmp(symbol.name.c_str(), L"setscrollpos") == 0) resolution.targetAddress = reinterpret_cast<ULONGLONG>(&BridgeSetScrollPos);
    else if (_wcsicmp(symbol.name.c_str(), L"createdialogparamw") == 0) resolution.targetAddress = reinterpret_cast<ULONGLONG>(&BridgeCreateDialogParamW);
    else if (_wcsicmp(symbol.name.c_str(), L"drawtextexw") == 0) resolution.targetAddress = reinterpret_cast<ULONGLONG>(&BridgeDrawTextExW);
    else if (_wcsicmp(symbol.name.c_str(), L"getancestor") == 0) resolution.targetAddress = reinterpret_cast<ULONGLONG>(&BridgeGetAncestor);
    else if (_wcsicmp(symbol.name.c_str(), L"findwindoww") == 0) resolution.targetAddress = reinterpret_cast<ULONGLONG>(&BridgeFindWindowW);
    else if (_wcsicmp(symbol.name.c_str(), L"setforegroundwindow") == 0) resolution.targetAddress = reinterpret_cast<ULONGLONG>(&BridgeSetForegroundWindow);
    else if (_wcsicmp(symbol.name.c_str(), L"setwineventhook") == 0) resolution.targetAddress = reinterpret_cast<ULONGLONG>(&BridgeSetWinEventHook);
    else if (_wcsicmp(symbol.name.c_str(), L"unhookwinevent") == 0) resolution.targetAddress = reinterpret_cast<ULONGLONG>(&BridgeUnhookWinEvent);
    else if (_wcsicmp(symbol.name.c_str(), L"charnextw") == 0) resolution.targetAddress = reinterpret_cast<ULONGLONG>(&BridgeCharNextW);
    else if (_wcsicmp(symbol.name.c_str(), L"getkeyboardlayout") == 0) resolution.targetAddress = reinterpret_cast<ULONGLONG>(&BridgeGetKeyboardLayout);
    else if (_wcsicmp(symbol.name.c_str(), L"getforegroundwindow") == 0) resolution.targetAddress = reinterpret_cast<ULONGLONG>(&BridgeGetForegroundWindow);
    else if (_wcsicmp(symbol.name.c_str(), L"messagebeep") == 0) resolution.targetAddress = reinterpret_cast<ULONGLONG>(&BridgeMessageBeep);
    else if (_wcsicmp(symbol.name.c_str(), L"isiconic") == 0) resolution.targetAddress = reinterpret_cast<ULONGLONG>(&BridgeIsIconic);
    else if (_wcsicmp(symbol.name.c_str(), L"isclipboardformatavailable") == 0) resolution.targetAddress = reinterpret_cast<ULONGLONG>(&BridgeIsClipboardFormatAvailable);
    else if (_wcsicmp(symbol.name.c_str(), L"setactivewindow") == 0) resolution.targetAddress = reinterpret_cast<ULONGLONG>(&BridgeSetActiveWindow);

    if (resolution.targetAddress != 0)
    {
        resolution.disposition = ImportDisposition::NeedsBridge;
        if (_wcsicmp(symbol.name.c_str(), L"settimer") == 0 ||
            _wcsicmp(symbol.name.c_str(), L"killtimer") == 0)
        {
            resolution.note = L"USER32 timer adapter: bounded UWP timers post coalesced WM_TIMER; TIMERPROC callbacks are not invoked.";
        }
        else
        {
            resolution.note = L"USER32 adapter: guest windows, queued messages, painting, and virtual device contexts.";
        }
    }
    return resolution;
}
