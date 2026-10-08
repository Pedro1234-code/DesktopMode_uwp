namespace
{
using GuestAbortProc = BOOL (CALLBACK*)(HDC, int);
using GuestFontEnumProcW = int (CALLBACK*)(const LOGFONTW*, const TEXTMETRICW*, DWORD, LPARAM);

struct GuestDcCompatibilityState final
{
    POINT origin{};
    POINT brushOrigin{};
    int stretchMode = COLORONCOLOR;
    int rop2 = R2_COPYPEN;
    UINT textAlign = TA_LEFT | TA_TOP | TA_NOUPDATECP;
};
struct GuestSavedDcState final
{
    GuestDcCompatibilityState state;
    MiniGdi::Rect clip{};
};
struct GuestRegion final { RECT bounds{}; bool empty = true; };
std::mutex g_gdiCompatibilityLock;
std::unordered_map<ULONG_PTR, GuestDcCompatibilityState> g_dcCompatibility;
std::unordered_map<ULONG_PTR, std::vector<GuestSavedDcState>> g_savedDcStates;
std::unordered_map<ULONG_PTR, GuestRegion> g_regions;
ULONG_PTR g_nextRegion = 0x6f000000;

GuestDcCompatibilityState DcState(HDC dc)
{
    std::lock_guard<std::mutex> guard(g_gdiCompatibilityLock);
    return g_dcCompatibility[reinterpret_cast<ULONG_PTR>(dc)];
}

HRGN StoreRegion(const RECT& rect)
{
    std::lock_guard<std::mutex> guard(g_gdiCompatibilityLock);
    const ULONG_PTR token = ++g_nextRegion;
    g_regions[token] = { rect, rect.left >= rect.right || rect.top >= rect.bottom };
    return reinterpret_cast<HRGN>(token);
}

BOOL WINAPI BridgeSetWindowOrgEx(HDC dc, int x, int y, LPPOINT previous)
{
    GuestWindowManager* manager = CurrentManagerOrFail();
    if (!manager || !manager->Gdi().HasDc(FromGuestDc(dc))) return FALSE;
    std::lock_guard<std::mutex> guard(g_gdiCompatibilityLock);
    auto& state = g_dcCompatibility[reinterpret_cast<ULONG_PTR>(dc)];
    if (previous) *previous = state.origin;
    state.origin = { x, y };
    return TRUE;
}

BOOL WINAPI BridgeOffsetWindowOrgEx(HDC dc, int x, int y, LPPOINT previous)
{
    const auto state = DcState(dc);
    return BridgeSetWindowOrgEx(dc, state.origin.x + x, state.origin.y + y, previous);
}

BOOL WINAPI BridgeDPtoLP(HDC dc, LPPOINT points, int count)
{
    if (!points || count < 0) return FALSE;
    const auto state = DcState(dc);
    for (int index = 0; index < count; ++index)
    {
        points[index].x += state.origin.x;
        points[index].y += state.origin.y;
    }
    return TRUE;
}

BOOL WINAPI BridgeSetBrushOrgEx(HDC dc, int x, int y, LPPOINT previous)
{
    std::lock_guard<std::mutex> guard(g_gdiCompatibilityLock);
    auto& state = g_dcCompatibility[reinterpret_cast<ULONG_PTR>(dc)];
    if (previous) *previous = state.brushOrigin;
    state.brushOrigin = { x, y };
    return TRUE;
}

int WINAPI BridgeSetStretchBltMode(HDC dc, int mode)
{
    std::lock_guard<std::mutex> guard(g_gdiCompatibilityLock);
    auto& state = g_dcCompatibility[reinterpret_cast<ULONG_PTR>(dc)];
    const int previous = state.stretchMode;
    state.stretchMode = mode;
    return previous;
}

int WINAPI BridgeSetROP2(HDC dc, int mode)
{
    std::lock_guard<std::mutex> guard(g_gdiCompatibilityLock);
    auto& state = g_dcCompatibility[reinterpret_cast<ULONG_PTR>(dc)];
    const int previous = state.rop2; state.rop2 = mode; return previous;
}
int WINAPI BridgeGetROP2(HDC dc) { return DcState(dc).rop2; }

UINT WINAPI BridgeSetTextAlign(HDC dc, UINT alignment)
{
    std::lock_guard<std::mutex> guard(g_gdiCompatibilityLock);
    auto& state = g_dcCompatibility[reinterpret_cast<ULONG_PTR>(dc)];
    const UINT previous = state.textAlign; state.textAlign = alignment; return previous;
}

HBITMAP WINAPI BridgeCreateBitmap(int width, int height, UINT, UINT, const void*)
{
    GuestWindowManager* manager = CurrentManagerOrFail();
    if (!manager || width <= 0 || height <= 0) return nullptr;
    const auto bitmap = manager->Gdi().CreateBitmap(width, height, MiniGdi::Transparent);
    return bitmap == MiniGdi::InvalidObject ? nullptr : reinterpret_cast<HBITMAP>(static_cast<ULONG_PTR>(bitmap));
}

HBRUSH WINAPI BridgeCreatePatternBrush(HBITMAP bitmap)
{
    GuestWindowManager* manager = CurrentManagerOrFail();
    if (!manager) return nullptr;
    const MiniGdi::Surface* surface = manager->Gdi().GetBitmapSurface(FromGuestObject(bitmap));
    const MiniGdi::Color color = surface && !surface->Empty() ? surface->Pixels().front() : MiniGdi::OpaqueWhite;
    return reinterpret_cast<HBRUSH>(static_cast<ULONG_PTR>(manager->Gdi().CreateSolidBrush(color)));
}

BOOL WINAPI BridgePatBlt(HDC dc, int x, int y, int width, int height, DWORD operation)
{
    GuestWindowManager* manager = CurrentManagerOrFail();
    if (!manager) return FALSE;
    const auto state = DcState(dc);
    if (operation == BLACKNESS || operation == WHITENESS)
    {
        return manager->Gdi().Clear(FromGuestDc(dc), operation == BLACKNESS ? MiniGdi::OpaqueBlack : MiniGdi::OpaqueWhite);
    }
    return manager->Gdi().FillRect(FromGuestDc(dc),
        { x - state.origin.x, y - state.origin.y,
          x - state.origin.x + width, y - state.origin.y + height });
}

HRGN WINAPI BridgeCreateRectRgn(int left, int top, int right, int bottom)
{
    return StoreRegion({ left, top, right, bottom });
}
HRGN WINAPI BridgeCreateRectRgnIndirect(const RECT* rect) { return rect ? StoreRegion(*rect) : nullptr; }

int WINAPI BridgeSelectClipRgn(HDC dc, HRGN region)
{
    GuestWindowManager* manager = CurrentManagerOrFail();
    if (!manager) return ERROR;
    if (!region) return manager->Gdi().ResetClip(FromGuestDc(dc)) ? NULLREGION : ERROR;
    std::lock_guard<std::mutex> guard(g_gdiCompatibilityLock);
    const auto found = g_regions.find(reinterpret_cast<ULONG_PTR>(region));
    if (found == g_regions.end()) return ERROR;
    return manager->Gdi().SetClipRect(FromGuestDc(dc),
        { found->second.bounds.left, found->second.bounds.top,
          found->second.bounds.right, found->second.bounds.bottom })
        ? (found->second.empty ? NULLREGION : SIMPLEREGION) : ERROR;
}

int WINAPI BridgeIntersectClipRect(HDC dc, int left, int top, int right, int bottom)
{
    GuestWindowManager* manager = CurrentManagerOrFail();
    if (!manager) return ERROR;
    MiniGdi::Rect current{};
    if (!manager->Gdi().GetClipRect(FromGuestDc(dc), &current)) return ERROR;
    const MiniGdi::Rect result = MiniGdi::IntersectRect(current, { left, top, right, bottom });
    return manager->Gdi().SetClipRect(FromGuestDc(dc), result)
        ? (result.Empty() ? NULLREGION : SIMPLEREGION) : ERROR;
}

int WINAPI BridgeExcludeClipRect(HDC dc, int left, int top, int right, int bottom)
{
    GuestWindowManager* manager = CurrentManagerOrFail();
    if (!manager) return ERROR;
    MiniGdi::Rect current{};
    if (!manager->Gdi().GetClipRect(FromGuestDc(dc), &current)) return ERROR;
    // MiniGDI has one rectangular clip. Preserve the largest rectangular
    // remainder, which is exact for the edge exclusions used by controls.
    MiniGdi::Rect result = current;
    if (left <= current.left && right >= current.right)
    {
        if (top <= current.top) result.top = (std::min)(current.bottom, bottom);
        else if (bottom >= current.bottom) result.bottom = (std::max)(current.top, top);
    }
    else if (top <= current.top && bottom >= current.bottom)
    {
        if (left <= current.left) result.left = (std::min)(current.right, right);
        else if (right >= current.right) result.right = (std::max)(current.left, left);
    }
    manager->Gdi().SetClipRect(FromGuestDc(dc), result);
    return result.Empty() ? NULLREGION : SIMPLEREGION;
}

int WINAPI BridgeGetClipRgn(HDC dc, HRGN region)
{
    GuestWindowManager* manager = CurrentManagerOrFail();
    if (!manager || !region) return -1;
    MiniGdi::Rect clip{};
    if (!manager->Gdi().GetClipRect(FromGuestDc(dc), &clip)) return -1;
    std::lock_guard<std::mutex> guard(g_gdiCompatibilityLock);
    auto& target = g_regions[reinterpret_cast<ULONG_PTR>(region)];
    target.bounds = { clip.left, clip.top, clip.right, clip.bottom };
    target.empty = clip.Empty();
    return 1;
}

int WINAPI BridgeCombineRgn(HRGN destination, HRGN first, HRGN second, int mode)
{
    std::lock_guard<std::mutex> guard(g_gdiCompatibilityLock);
    const auto a = g_regions.find(reinterpret_cast<ULONG_PTR>(first));
    const auto b = g_regions.find(reinterpret_cast<ULONG_PTR>(second));
    if (a == g_regions.end() || (mode != RGN_COPY && b == g_regions.end())) return ERROR;
    RECT result = a->second.bounds;
    if (mode == RGN_AND)
    {
        result.left = (std::max)(result.left, b->second.bounds.left);
        result.top = (std::max)(result.top, b->second.bounds.top);
        result.right = (std::min)(result.right, b->second.bounds.right);
        result.bottom = (std::min)(result.bottom, b->second.bounds.bottom);
    }
    else if (mode == RGN_OR)
    {
        result.left = (std::min)(result.left, b->second.bounds.left);
        result.top = (std::min)(result.top, b->second.bounds.top);
        result.right = (std::max)(result.right, b->second.bounds.right);
        result.bottom = (std::max)(result.bottom, b->second.bounds.bottom);
    }
    g_regions[reinterpret_cast<ULONG_PTR>(destination)] = { result,
        result.left >= result.right || result.top >= result.bottom };
    return result.left >= result.right || result.top >= result.bottom ? NULLREGION : SIMPLEREGION;
}

int WINAPI BridgeSaveDC(HDC dc)
{
    GuestWindowManager* manager = CurrentManagerOrFail();
    if (!manager) return 0;
    MiniGdi::Rect clip{};
    if (!manager->Gdi().GetClipRect(FromGuestDc(dc), &clip)) return 0;
    std::lock_guard<std::mutex> guard(g_gdiCompatibilityLock);
    auto& saves = g_savedDcStates[reinterpret_cast<ULONG_PTR>(dc)];
    saves.push_back({ g_dcCompatibility[reinterpret_cast<ULONG_PTR>(dc)], clip });
    return static_cast<int>(saves.size());
}

BOOL WINAPI BridgeRestoreDC(HDC dc, int saved)
{
    GuestWindowManager* manager = CurrentManagerOrFail();
    if (!manager) return FALSE;
    GuestSavedDcState state{};
    {
        std::lock_guard<std::mutex> guard(g_gdiCompatibilityLock);
        auto& saves = g_savedDcStates[reinterpret_cast<ULONG_PTR>(dc)];
        if (saves.empty()) return FALSE;
        size_t index = saved < 0 ? saves.size() - 1 : static_cast<size_t>(saved - 1);
        if (index >= saves.size()) return FALSE;
        state = saves[index]; saves.resize(index);
        g_dcCompatibility[reinterpret_cast<ULONG_PTR>(dc)] = state.state;
    }
    return manager->Gdi().SetClipRect(FromGuestDc(dc), state.clip);
}

BOOL WINAPI BridgeRectVisible(HDC dc, const RECT* rect)
{
    if (!rect) return FALSE;
    GuestWindowManager* manager = CurrentManagerOrFail();
    MiniGdi::Rect clip{};
    return manager && manager->Gdi().GetClipRect(FromGuestDc(dc), &clip) &&
        !MiniGdi::IntersectRect(clip, { rect->left, rect->top, rect->right, rect->bottom }).Empty();
}

BOOL WINAPI BridgePolyline(HDC dc, const POINT* points, int count)
{
    if (!points || count < 2) return FALSE;
    BOOL result = BridgeMoveToEx(dc, points[0].x, points[0].y, nullptr);
    for (int index = 1; result && index < count; ++index)
        result = BridgeLineTo(dc, points[index].x, points[index].y);
    return result;
}

BOOL WINAPI BridgePolygon(HDC dc, const POINT* points, int count)
{
    if (!BridgePolyline(dc, points, count)) return FALSE;
    return BridgeLineTo(dc, points[0].x, points[0].y);
}

BOOL WINAPI BridgeRoundRect(HDC dc, int left, int top, int right, int bottom, int, int)
{
    // MiniGDI's antialiased ellipse/rectangle primitives share the same
    // selected pen and brush. Until path geometry is added, a zero-radius
    // compatible rectangle preserves bounds, clipping and object selection.
    return BridgeRectangle(dc, left, top, right, bottom);
}

HBRUSH WINAPI BridgeCreateHatchBrush(int, COLORREF color) { return BridgeCreateSolidBrush(color); }

struct GuestLogBrush { UINT style; COLORREF color; ULONG_PTR hatch; };
HPEN WINAPI BridgeExtCreatePen(DWORD style, DWORD width, const GuestLogBrush* brush,
    DWORD, const DWORD*)
{
    return BridgeCreatePen(static_cast<int>(style & 0xf), static_cast<int>((std::max)(1ul, width)),
        brush ? brush->color : RGB(0, 0, 0));
}

BOOL WINAPI BridgeExtTextOutW(HDC dc, int x, int y, UINT, const RECT*, LPCWSTR text,
    UINT count, const INT*)
{
    const auto state = DcState(dc);
    return BridgeTextOutW(dc, x - state.origin.x, y - state.origin.y, text, static_cast<int>(count));
}

BOOL WINAPI BridgeExtTextOutA(HDC dc, int x, int y, UINT options, const RECT* rect,
    LPCSTR text, UINT count, const INT* spacing)
{
    if (!text) return FALSE;
    const int wideCount = BridgeMultiByteToWideChar(CP_ACP, 0, text, static_cast<int>(count), nullptr, 0);
    if (wideCount <= 0) return FALSE;
    std::vector<wchar_t> wide(static_cast<size_t>(wideCount));
    BridgeMultiByteToWideChar(CP_ACP, 0, text, static_cast<int>(count), wide.data(), wideCount);
    return BridgeExtTextOutW(dc, x, y, options, rect, wide.data(), static_cast<UINT>(wideCount), spacing);
}

BOOL WINAPI BridgeGetTextExtentPointW(HDC dc, LPCWSTR text, int count, LPSIZE size)
{
    return BridgeGetTextExtentPoint32W(dc, text, count, size);
}

BOOL WINAPI BridgeGetTextExtentPoint32A(HDC dc, LPCSTR text, int count, LPSIZE size)
{
    if (!text || count < 0) return FALSE;
    const int wideCount = BridgeMultiByteToWideChar(CP_ACP, 0, text, count, nullptr, 0);
    std::vector<wchar_t> wide(static_cast<size_t>((std::max)(0, wideCount)));
    if (wideCount && BridgeMultiByteToWideChar(CP_ACP, 0, text, count, wide.data(), wideCount) != wideCount) return FALSE;
    return BridgeGetTextExtentPoint32W(dc, wide.data(), wideCount, size);
}

BOOL WINAPI BridgeGetTextExtentExPointW(HDC dc, LPCWSTR text, int count, int maximum,
    LPINT fit, LPINT advances, LPSIZE size)
{
    if (!BridgeGetTextExtentPoint32W(dc, text, count, size)) return FALSE;
    GuestWindowManager* manager = CurrentManagerOrFail();
    MiniGdi::FontMetrics metrics{};
    if (!manager || !manager->Gdi().GetSelectedFontMetrics(FromGuestDc(dc), &metrics)) return FALSE;
    const int cell = (std::max)(1, metrics.averageWidth);
    const int fitted = maximum < 0 ? count : (std::min)(count, maximum / cell);
    if (fit) *fit = fitted;
    if (advances) for (int index = 0; index < count; ++index) advances[index] = (index + 1) * cell;
    return TRUE;
}

BOOL WINAPI BridgeGetTextExtentExPointA(HDC dc, LPCSTR text, int count, int maximum,
    LPINT fit, LPINT advances, LPSIZE size)
{
    if (!text || count < 0) return FALSE;
    const int wideCount = BridgeMultiByteToWideChar(CP_ACP, 0, text, count, nullptr, 0);
    std::vector<wchar_t> wide(static_cast<size_t>((std::max)(0, wideCount)));
    if (wideCount) BridgeMultiByteToWideChar(CP_ACP, 0, text, count, wide.data(), wideCount);
    return BridgeGetTextExtentExPointW(dc, wide.data(), wideCount, maximum, fit, advances, size);
}

BOOL WINAPI BridgeStretchBlt(HDC destination, int x, int y, int width, int height,
    HDC source, int sourceX, int sourceY, int sourceWidth, int sourceHeight, DWORD operation)
{
    if (operation != SRCCOPY || width <= 0 || height <= 0 || sourceWidth <= 0 || sourceHeight <= 0) return FALSE;
    GuestWindowManager* manager = CurrentManagerOrFail();
    if (!manager) return FALSE;
    MiniGdi::Surface* output = manager->Gdi().GetSurface(FromGuestDc(destination));
    const MiniGdi::Surface* input = manager->Gdi().GetSurface(FromGuestDc(source));
    if (!output || !input) return FALSE;
    MiniGdi::Rect clip{};
    manager->Gdi().GetClipRect(FromGuestDc(destination), &clip);
    for (int dy = 0; dy < height; ++dy) for (int dx = 0; dx < width; ++dx)
    {
        const int tx = x + dx, ty = y + dy;
        if (tx < clip.left || tx >= clip.right || ty < clip.top || ty >= clip.bottom) continue;
        const MiniGdi::Color* pixel = input->PixelAt(sourceX + dx * sourceWidth / width,
            sourceY + dy * sourceHeight / height);
        MiniGdi::Color* target = output->PixelAt(tx, ty);
        if (pixel && target) *target = *pixel;
    }
    return TRUE;
}

BOOL WINAPI BridgeGdiAlphaBlend(HDC destination, int x, int y, int width, int height,
    HDC source, int sourceX, int sourceY, int sourceWidth, int sourceHeight, DWORD blend)
{
    UNREFERENCED_PARAMETER(blend);
    return BridgeStretchBlt(destination, x, y, width, height, source,
        sourceX, sourceY, sourceWidth, sourceHeight, SRCCOPY);
}

int WINAPI BridgeEnumFontFamiliesExW(HDC dc, LOGFONTW* font, GuestFontEnumProcW callback,
    LPARAM parameter, DWORD)
{
    return BridgeEnumFontsW(dc, font ? font->lfFaceName : nullptr, callback, parameter);
}

bool CopyDibToSurface(const BITMAPINFO* information, const void* bits,
    MiniGdi::Surface* surface, UINT start, UINT lines)
{
    if (!information || !bits || !surface || information->bmiHeader.biSize < sizeof(BITMAPINFOHEADER)) return false;
    const auto& header = information->bmiHeader;
    const int width = header.biWidth;
    const int height = header.biHeight < 0 ? -header.biHeight : header.biHeight;
    if (width <= 0 || height <= 0 || (header.biBitCount != 32 && header.biBitCount != 24)) return false;
    const size_t stride = ((static_cast<size_t>(width) * header.biBitCount + 31) / 32) * 4;
    const UINT copiedLines = (std::min)(lines, static_cast<UINT>((std::max)(0, height - static_cast<int>(start))));
    const BYTE* source = static_cast<const BYTE*>(bits);
    for (UINT row = 0; row < copiedLines; ++row)
    {
        const int logicalY = static_cast<int>(start + row);
        const int destinationY = header.biHeight < 0 ? logicalY : height - 1 - logicalY;
        for (int x = 0; x < width; ++x)
        {
            const BYTE* pixel = source + static_cast<size_t>(row) * stride +
                static_cast<size_t>(x) * (header.biBitCount / 8);
            MiniGdi::Color* target = surface->PixelAt(x, destinationY);
            if (target) *target = MiniGdi::MakeColor(pixel[2], pixel[1], pixel[0],
                header.biBitCount == 32 && pixel[3] ? pixel[3] : 255);
        }
    }
    return true;
}

HBITMAP WINAPI BridgeCreateDIBSection(HDC, const BITMAPINFO* information, UINT,
    void** bits, HANDLE, DWORD)
{
    if (!information || !bits) return nullptr;
    const int width = information->bmiHeader.biWidth;
    const int height = information->bmiHeader.biHeight < 0
        ? -information->bmiHeader.biHeight : information->bmiHeader.biHeight;
    HBITMAP bitmap = BridgeCreateBitmap(width, height, 1, information->bmiHeader.biBitCount, nullptr);
    GuestWindowManager* manager = CurrentManagerOrFail();
    MiniGdi::Surface* surface = manager && bitmap
        ? manager->Gdi().GetBitmapSurface(FromGuestObject(bitmap)) : nullptr;
    if (!surface) { *bits = nullptr; return nullptr; }
    *bits = surface->Data();
    return bitmap;
}

int WINAPI BridgeSetDIBits(HDC, HBITMAP bitmap, UINT start, UINT lines,
    const void* bits, const BITMAPINFO* information, UINT)
{
    GuestWindowManager* manager = CurrentManagerOrFail();
    MiniGdi::Surface* surface = manager
        ? manager->Gdi().GetBitmapSurface(FromGuestObject(bitmap)) : nullptr;
    return CopyDibToSurface(information, bits, surface, start, lines) ? static_cast<int>(lines) : 0;
}

int WINAPI BridgeGetDIBits(HDC, HBITMAP bitmap, UINT start, UINT lines,
    void* bits, BITMAPINFO* information, UINT)
{
    GuestWindowManager* manager = CurrentManagerOrFail();
    const MiniGdi::Surface* surface = manager
        ? manager->Gdi().GetBitmapSurface(FromGuestObject(bitmap)) : nullptr;
    if (!surface || !information) return 0;
    auto& header = information->bmiHeader;
    if (header.biSize < sizeof(BITMAPINFOHEADER)) return 0;
    if (header.biWidth == 0) header.biWidth = surface->Width();
    if (header.biHeight == 0) header.biHeight = surface->Height();
    if (header.biPlanes == 0) header.biPlanes = 1;
    if (header.biBitCount == 0) header.biBitCount = 32;
    if (!bits) return static_cast<int>(lines);
    if (header.biBitCount != 32 && header.biBitCount != 24) return 0;
    const size_t stride = ((static_cast<size_t>(surface->Width()) * header.biBitCount + 31) / 32) * 4;
    BYTE* destination = static_cast<BYTE*>(bits);
    const UINT copied = (std::min)(lines, static_cast<UINT>((std::max)(0, surface->Height() - static_cast<int>(start))));
    for (UINT row = 0; row < copied; ++row) for (int x = 0; x < surface->Width(); ++x)
    {
        const int sourceY = header.biHeight < 0 ? static_cast<int>(start + row)
            : surface->Height() - 1 - static_cast<int>(start + row);
        const MiniGdi::Color* color = surface->PixelAt(x, sourceY);
        BYTE* pixel = destination + static_cast<size_t>(row) * stride + static_cast<size_t>(x) * (header.biBitCount / 8);
        if (color) { pixel[0] = MiniGdi::Blue(*color); pixel[1] = MiniGdi::Green(*color); pixel[2] = MiniGdi::Red(*color); if (header.biBitCount == 32) pixel[3] = MiniGdi::Alpha(*color); }
    }
    return static_cast<int>(copied);
}

int WINAPI BridgeStretchDIBits(HDC destination, int x, int y, int width, int height,
    int sourceX, int sourceY, int sourceWidth, int sourceHeight, const void* bits,
    const BITMAPINFO* information, UINT, DWORD operation)
{
    if (!information || !bits || operation != SRCCOPY) return GDI_ERROR;
    const int bitmapHeight = information->bmiHeader.biHeight < 0
        ? -information->bmiHeader.biHeight : information->bmiHeader.biHeight;
    MiniGdi::Surface source(information->bmiHeader.biWidth, bitmapHeight, MiniGdi::Transparent);
    if (!CopyDibToSurface(information, bits, &source, 0, static_cast<UINT>(bitmapHeight))) return GDI_ERROR;
    GuestWindowManager* manager = CurrentManagerOrFail();
    MiniGdi::Surface* output = manager ? manager->Gdi().GetSurface(FromGuestDc(destination)) : nullptr;
    if (!output) return GDI_ERROR;
    for (int dy = 0; dy < height; ++dy) for (int dx = 0; dx < width; ++dx)
    {
        const MiniGdi::Color* pixel = source.PixelAt(sourceX + dx * sourceWidth / width,
            sourceY + dy * sourceHeight / height);
        MiniGdi::Color* target = output->PixelAt(x + dx, y + dy);
        if (pixel && target) *target = *pixel;
    }
    return height;
}

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
