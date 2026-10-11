namespace
{
using GuestAbortProc = BOOL (CALLBACK*)(HDC, int);
using GuestFontEnumProcW = int (CALLBACK*)(const LOGFONTW*, const TEXTMETRICW*, DWORD, LPARAM);

// Desktop GDI declares these in wingdi.h, but the UWP SDK projection omits
// them.  Keep the exact native layout at the guest ABI boundary.
struct GuestEnumLogFontExW final
{
    LOGFONTW logFont;
    wchar_t fullName[64];
    wchar_t style[32];
    wchar_t script[32];
};

struct GuestNewTextMetricW final
{
    TEXTMETRICW textMetric;
    DWORD flags;
    UINT sizeEm;
    UINT cellHeight;
    UINT averageWidth;
};

struct GuestFontSignature final
{
    DWORD unicodeSubsets[4];
    DWORD codePages[2];
};

struct GuestNewTextMetricExW final
{
    GuestNewTextMetricW newTextMetric;
    GuestFontSignature signature;
};

int InvokeGuestFontEnumProcWSeh(GuestFontEnumProcW callback,
    const LOGFONTW* font, const TEXTMETRICW* metrics, DWORD type,
    LPARAM parameter, bool* faulted)
{
    *faulted = false;
    __try
    {
        return callback(font, metrics, type, parameter);
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        *faulted = true;
        return 0;
    }
}

int InvokeGuestFontEnumProcW(GuestFontEnumProcW callback,
    const LOGFONTW* font, const TEXTMETRICW* metrics, DWORD type, LPARAM parameter)
{
    if (!callback) return 0;
    bool faulted = false;
    const int result = InvokeGuestFontEnumProcWSeh(
        callback, font, metrics, type, parameter, &faulted);
    if (faulted)
        RuntimeDiagnostics::Record(L"GDI: guest font-enumeration callback raised an exception.");
    return result;
}

struct GuestDcCompatibilityState final
{
    POINT windowOrigin{};
    POINT viewportOrigin{};
    SIZE windowExtent{ 1, 1 };
    SIZE viewportExtent{ 1, 1 };
    POINT brushOrigin{};
    int mapMode = MM_TEXT;
    int stretchMode = COLORONCOLOR;
    int rop2 = R2_COPYPEN;
    UINT textAlign = TA_LEFT | TA_TOP | TA_NOUPDATECP;
    int graphicsMode = GM_COMPATIBLE;
    XFORM worldTransform{ 1.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f };
    bool pathOpen = false;
    bool pathReady = false;
    std::vector<POINT> pathPoints;
    std::vector<BYTE> pathTypes;
    HPALETTE selectedPalette = nullptr;
    int icmMode = 1; // ICM_OFF
};
struct GuestSavedDcState final
{
    GuestDcCompatibilityState state;
    MiniGdi::Region clip;
};
struct GuestRegion final { MiniGdi::Region geometry; };
std::mutex g_gdiCompatibilityLock;
std::unordered_map<ULONG_PTR, GuestDcCompatibilityState> g_dcCompatibility;
std::unordered_map<ULONG_PTR, std::vector<GuestSavedDcState>> g_savedDcStates;
std::unordered_map<ULONG_PTR, GuestRegion> g_regions;
ULONG_PTR g_nextRegion = 0x6f000000;
struct GuestPalette final { std::vector<PALETTEENTRY> entries; };
std::unordered_map<ULONG_PTR, GuestPalette> g_palettes;
ULONG_PTR g_nextPalette = 0x6e000000;
struct GuestMetafile final { MiniGdi::Surface surface; RECT bounds{}; };
struct GuestMetafileRecording final { HBITMAP bitmap = nullptr; RECT bounds{}; };
std::unordered_map<ULONG_PTR, GuestMetafile> g_metafiles;
std::unordered_map<ULONG_PTR, GuestMetafileRecording> g_metafileRecordings;
ULONG_PTR g_nextMetafile = 0x6d000000;

GuestDcCompatibilityState DcState(HDC dc)
{
    std::lock_guard<std::mutex> guard(g_gdiCompatibilityLock);
    return g_dcCompatibility[reinterpret_cast<ULONG_PTR>(dc)];
}

void RecordPathPoint(HDC dc, POINT point, BYTE type)
{
    std::lock_guard<std::mutex> guard(g_gdiCompatibilityLock);
    auto& state = g_dcCompatibility[reinterpret_cast<ULONG_PTR>(dc)];
    if (!state.pathOpen) return;
    state.pathPoints.push_back(point); state.pathTypes.push_back(type);
}

void EraseDcCompatibilityState(HDC dc)
{
    std::lock_guard<std::mutex> guard(g_gdiCompatibilityLock);
    const ULONG_PTR key = reinterpret_cast<ULONG_PTR>(dc);
    g_dcCompatibility.erase(key);
    g_savedDcStates.erase(key);
}

LONG ScaleCoordinate(LONG value, LONG numerator, LONG denominator)
{
    if (denominator == 0) return 0;
    const long long product = static_cast<long long>(value) * numerator;
    const long long absoluteDenominator = denominator < 0
        ? -static_cast<long long>(denominator)
        : static_cast<long long>(denominator);
    const long long adjustment = absoluteDenominator / 2;
    const bool negative = (product < 0) != (denominator < 0);
    const long long magnitude = product < 0 ? -product : product;
    const long long rounded = (magnitude + adjustment) / absoluteDenominator;
    const long long result = negative ? -rounded : rounded;
    return static_cast<LONG>((std::max)(
        static_cast<long long>((std::numeric_limits<LONG>::min)()),
        (std::min)(static_cast<long long>((std::numeric_limits<LONG>::max)()), result)));
}

LONG SaturateToLong(long long value)
{
    return static_cast<LONG>((std::max)(
        static_cast<long long>((std::numeric_limits<LONG>::min)()),
        (std::min)(static_cast<long long>((std::numeric_limits<LONG>::max)()), value)));
}

POINT LogicalToDevicePoint(const GuestDcCompatibilityState& state, LONG x, LONG y)
{
    double logicalX = x;
    double logicalY = y;
    if (state.graphicsMode == GM_ADVANCED)
    {
        logicalX = static_cast<double>(x) * state.worldTransform.eM11 +
            static_cast<double>(y) * state.worldTransform.eM21 + state.worldTransform.eDx;
        logicalY = static_cast<double>(x) * state.worldTransform.eM12 +
            static_cast<double>(y) * state.worldTransform.eM22 + state.worldTransform.eDy;
    }
    return POINT{
        state.viewportOrigin.x + ScaleCoordinate(
            SaturateToLong(static_cast<long long>(std::llround(logicalX))) - state.windowOrigin.x,
            state.viewportExtent.cx, state.windowExtent.cx),
        state.viewportOrigin.y + ScaleCoordinate(
            SaturateToLong(static_cast<long long>(std::llround(logicalY))) - state.windowOrigin.y,
            state.viewportExtent.cy, state.windowExtent.cy)
    };
}

POINT DeviceToLogicalPoint(const GuestDcCompatibilityState& state, LONG x, LONG y)
{
    double logicalX = state.windowOrigin.x + ScaleCoordinate(
            x - state.viewportOrigin.x, state.windowExtent.cx, state.viewportExtent.cx),
        logicalY = state.windowOrigin.y + ScaleCoordinate(
            y - state.viewportOrigin.y, state.windowExtent.cy, state.viewportExtent.cy);
    if (state.graphicsMode == GM_ADVANCED)
    {
        const double determinant = static_cast<double>(state.worldTransform.eM11) * state.worldTransform.eM22 -
            static_cast<double>(state.worldTransform.eM12) * state.worldTransform.eM21;
        if (std::abs(determinant) > 1.0e-12)
        {
            const double translatedX = logicalX - state.worldTransform.eDx;
            const double translatedY = logicalY - state.worldTransform.eDy;
            const double originalX = (translatedX * state.worldTransform.eM22 -
                translatedY * state.worldTransform.eM21) / determinant;
            const double originalY = (-translatedX * state.worldTransform.eM12 +
                translatedY * state.worldTransform.eM11) / determinant;
            logicalX = originalX;
            logicalY = originalY;
        }
    }
    return POINT{ SaturateToLong(static_cast<long long>(std::llround(logicalX))),
        SaturateToLong(static_cast<long long>(std::llround(logicalY))) };
}

void SetFixedMapModeExtents(GuestDcCompatibilityState* state, int mode)
{
    if (!state) return;
    switch (mode)
    {
    case MM_TEXT:
        state->windowExtent = { 1, 1 };
        state->viewportExtent = { 1, 1 };
        break;
    case MM_LOMETRIC:
        state->windowExtent = { 254, 254 };
        state->viewportExtent = { 96, -96 };
        break;
    case MM_HIMETRIC:
        state->windowExtent = { 2540, 2540 };
        state->viewportExtent = { 96, -96 };
        break;
    case MM_LOENGLISH:
        state->windowExtent = { 100, 100 };
        state->viewportExtent = { 96, -96 };
        break;
    case MM_HIENGLISH:
        state->windowExtent = { 1000, 1000 };
        state->viewportExtent = { 96, -96 };
        break;
    case MM_TWIPS:
        state->windowExtent = { 1440, 1440 };
        state->viewportExtent = { 96, -96 };
        break;
    default:
        break;
    }
}

HRGN StoreRegion(const RECT& rect)
{
    std::lock_guard<std::mutex> guard(g_gdiCompatibilityLock);
    const ULONG_PTR token = ++g_nextRegion;
    g_regions[token].geometry = MiniGdi::RectRegion(
        { rect.left, rect.top, rect.right, rect.bottom });
    return reinterpret_cast<HRGN>(token);
}

HRGN StoreRegion(const MiniGdi::Region& geometry)
{
    std::lock_guard<std::mutex> guard(g_gdiCompatibilityLock);
    const ULONG_PTR token = ++g_nextRegion;
    g_regions[token].geometry = geometry;
    return reinterpret_cast<HRGN>(token);
}

int RegionComplexity(const MiniGdi::Region& region)
{
    if (region.Empty()) return NULLREGION;
    return region.rectangles.size() == 1 ? SIMPLEREGION : COMPLEXREGION;
}

MiniGdi::Region LogicalToDeviceRegion(
    const GuestDcCompatibilityState& state, const MiniGdi::Region& source)
{
    MiniGdi::Region result;
    for (const MiniGdi::Rect& rect : source.rectangles)
    {
        const POINT first = LogicalToDevicePoint(state, rect.left, rect.top);
        const POINT second = LogicalToDevicePoint(state, rect.right, rect.bottom);
        result = MiniGdi::UnionRegion(result, MiniGdi::RectRegion({
            (std::min)(first.x, second.x), (std::min)(first.y, second.y),
            (std::max)(first.x, second.x), (std::max)(first.y, second.y) }));
    }
    return result;
}

MiniGdi::Region DeviceToLogicalRegion(
    const GuestDcCompatibilityState& state, const MiniGdi::Region& source)
{
    MiniGdi::Region result;
    for (const MiniGdi::Rect& rect : source.rectangles)
    {
        const POINT first = DeviceToLogicalPoint(state, rect.left, rect.top);
        const POINT second = DeviceToLogicalPoint(state, rect.right, rect.bottom);
        result = MiniGdi::UnionRegion(result, MiniGdi::RectRegion({
            (std::min)(first.x, second.x), (std::min)(first.y, second.y),
            (std::max)(first.x, second.x), (std::max)(first.y, second.y) }));
    }
    return result;
}

bool CopyRegionHandle(HRGN region, MiniGdi::Region* geometry)
{
    if (!region || !geometry) return false;
    std::lock_guard<std::mutex> guard(g_gdiCompatibilityLock);
    const auto found = g_regions.find(reinterpret_cast<ULONG_PTR>(region));
    if (found == g_regions.end()) return false;
    *geometry = found->second.geometry;
    return true;
}

bool AssignRegionHandle(HRGN region, const MiniGdi::Region& geometry)
{
    if (!region) return false;
    std::lock_guard<std::mutex> guard(g_gdiCompatibilityLock);
    const auto found = g_regions.find(reinterpret_cast<ULONG_PTR>(region));
    if (found == g_regions.end()) return false;
    found->second.geometry = geometry;
    return true;
}

bool DeleteRegionHandle(HRGN region)
{
    if (!region) return false;
    std::lock_guard<std::mutex> guard(g_gdiCompatibilityLock);
    return g_regions.erase(reinterpret_cast<ULONG_PTR>(region)) != 0;
}

BOOL WINAPI BridgeSetWindowOrgEx(HDC dc, int x, int y, LPPOINT previous)
{
    GuestWindowManager* manager = CurrentManagerOrFail();
    if (!manager || !manager->Gdi().HasDc(FromGuestDc(dc))) return FALSE;
    std::lock_guard<std::mutex> guard(g_gdiCompatibilityLock);
    auto& state = g_dcCompatibility[reinterpret_cast<ULONG_PTR>(dc)];
    if (previous) *previous = state.windowOrigin;
    state.windowOrigin = { x, y };
    return TRUE;
}

BOOL WINAPI BridgeOffsetWindowOrgEx(HDC dc, int x, int y, LPPOINT previous)
{
    const auto state = DcState(dc);
    return BridgeSetWindowOrgEx(dc,
        state.windowOrigin.x + x, state.windowOrigin.y + y, previous);
}

BOOL WINAPI BridgeGetWindowOrgEx(HDC dc, LPPOINT origin)
{
    if (!origin) { BridgeSetLastError(ERROR_INVALID_PARAMETER); return FALSE; }
    GuestWindowManager* manager = CurrentManagerOrFail();
    if (!manager || !manager->Gdi().HasDc(FromGuestDc(dc))) return FALSE;
    *origin = DcState(dc).windowOrigin;
    return TRUE;
}

BOOL WINAPI BridgeSetViewportOrgEx(HDC dc, int x, int y, LPPOINT previous)
{
    GuestWindowManager* manager = CurrentManagerOrFail();
    if (!manager || !manager->Gdi().HasDc(FromGuestDc(dc))) return FALSE;
    std::lock_guard<std::mutex> guard(g_gdiCompatibilityLock);
    auto& state = g_dcCompatibility[reinterpret_cast<ULONG_PTR>(dc)];
    if (previous) *previous = state.viewportOrigin;
    state.viewportOrigin = { x, y };
    return TRUE;
}

BOOL WINAPI BridgeOffsetViewportOrgEx(HDC dc, int x, int y, LPPOINT previous)
{
    const auto state = DcState(dc);
    return BridgeSetViewportOrgEx(dc,
        state.viewportOrigin.x + x, state.viewportOrigin.y + y, previous);
}

BOOL WINAPI BridgeGetViewportOrgEx(HDC dc, LPPOINT origin)
{
    if (!origin) { BridgeSetLastError(ERROR_INVALID_PARAMETER); return FALSE; }
    GuestWindowManager* manager = CurrentManagerOrFail();
    if (!manager || !manager->Gdi().HasDc(FromGuestDc(dc))) return FALSE;
    *origin = DcState(dc).viewportOrigin;
    return TRUE;
}

BOOL WINAPI BridgeDPtoLP(HDC dc, LPPOINT points, int count)
{
    if (!points || count < 0) return FALSE;
    const auto state = DcState(dc);
    for (int index = 0; index < count; ++index)
    {
        points[index] = DeviceToLogicalPoint(state, points[index].x, points[index].y);
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

UINT WINAPI BridgeGetTextAlign(HDC dc)
{
    GuestWindowManager* manager = CurrentManagerOrFail();
    if (!manager || !manager->Gdi().HasDc(FromGuestDc(dc))) return GDI_ERROR;
    return DcState(dc).textAlign;
}

BOOL WINAPI BridgeGetCurrentPositionEx(HDC dc, LPPOINT point)
{
    if (!point) { BridgeSetLastError(ERROR_INVALID_PARAMETER); return FALSE; }
    GuestWindowManager* manager = CurrentManagerOrFail();
    MiniGdi::Point current{};
    if (!manager || !manager->Gdi().GetCurrentPosition(FromGuestDc(dc), &current))
        return FALSE;
    *point = DeviceToLogicalPoint(DcState(dc), current.x, current.y);
    return TRUE;
}

BOOL WINAPI BridgeGetBrushOrgEx(HDC dc, LPPOINT origin)
{
    if (!origin) { BridgeSetLastError(ERROR_INVALID_PARAMETER); return FALSE; }
    GuestWindowManager* manager = CurrentManagerOrFail();
    if (!manager || !manager->Gdi().HasDc(FromGuestDc(dc))) return FALSE;
    *origin = DcState(dc).brushOrigin;
    return TRUE;
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

MiniGdi::Color ApplyRasterOperation(DWORD operation, MiniGdi::Color source,
    MiniGdi::Color destination, MiniGdi::Color pattern);

BOOL WINAPI BridgePatBlt(HDC dc, int x, int y, int width, int height, DWORD operation)
{
    GuestWindowManager* manager = CurrentManagerOrFail();
    if (!manager) return FALSE;
    const auto state = DcState(dc);
    const POINT first = LogicalToDevicePoint(state, x, y);
    const POINT second = LogicalToDevicePoint(state, x + width, y + height);
    const MiniGdi::Rect bounds{
        (std::min)(first.x, second.x), (std::min)(first.y, second.y),
        (std::max)(first.x, second.x), (std::max)(first.y, second.y) };
    MiniGdi::Surface* surface = manager->Gdi().GetSurface(FromGuestDc(dc));
    MiniGdi::Brush brush{};
    if (!surface || !manager->Gdi().GetSelectedBrush(FromGuestDc(dc), &brush)) return FALSE;
    MiniGdi::Region clip; manager->Gdi().GetClipRegion(FromGuestDc(dc), &clip);
    for (int row = bounds.top; row < bounds.bottom; ++row)
        for (int column = bounds.left; column < bounds.right; ++column)
        {
            if (!clip.Contains({ column, row })) continue;
            MiniGdi::Color* target = surface->PixelAt(column, row);
            if (target) *target = ApplyRasterOperation(operation, MiniGdi::Transparent,
                *target, brush.isNull ? MiniGdi::OpaqueWhite : brush.color);
        }
    return TRUE;
}

HRGN WINAPI BridgeCreateRectRgn(int left, int top, int right, int bottom)
{
    return StoreRegion({ left, top, right, bottom });
}
HRGN WINAPI BridgeCreateRectRgnIndirect(const RECT* rect) { return rect ? StoreRegion(*rect) : nullptr; }

HRGN WINAPI BridgeCreateEllipticRgn(int left, int top, int right, int bottom)
{
    if (left > right) (std::swap)(left, right);
    if (top > bottom) (std::swap)(top, bottom);
    MiniGdi::Region geometry;
    if (left >= right || top >= bottom) return StoreRegion(geometry);
    const double radiusX = (static_cast<double>(right) - static_cast<double>(left)) / 2.0;
    const double radiusY = (static_cast<double>(bottom) - static_cast<double>(top)) / 2.0;
    const double centerX = static_cast<double>(left) + radiusX;
    const double centerY = static_cast<double>(top) + radiusY;
    for (int y = top; y < bottom; ++y)
    {
        const double normalizedY = (static_cast<double>(y) + 0.5 - centerY) / radiusY;
        const double squared = 1.0 - normalizedY * normalizedY;
        if (squared <= 0.0) continue;
        const double span = radiusX * std::sqrt(squared);
        const int runLeft = static_cast<int>(std::ceil(centerX - span - 0.5));
        const int runRight = static_cast<int>(std::floor(centerX + span - 0.5)) + 1;
        if (runLeft >= runRight) continue;
        if (!geometry.rectangles.empty())
        {
            MiniGdi::Rect& previous = geometry.rectangles.back();
            if (previous.left == runLeft && previous.right == runRight && previous.bottom == y)
            {
                previous.bottom = y + 1;
                continue;
            }
        }
        geometry.rectangles.push_back({ runLeft, y, runRight, y + 1 });
    }
    return StoreRegion(geometry);
}

BOOL WINAPI BridgeSetRectRgn(HRGN region, int left, int top, int right, int bottom)
{
    return AssignRegionHandle(region, MiniGdi::RectRegion({ left, top, right, bottom }))
        ? TRUE : FALSE;
}

int WINAPI BridgeOffsetRgn(HRGN region, int x, int y)
{
    MiniGdi::Region geometry;
    if (!CopyRegionHandle(region, &geometry)) return ERROR;
    for (MiniGdi::Rect& rect : geometry.rectangles)
    {
        rect.left = SaturateToLong(static_cast<long long>(rect.left) + x);
        rect.right = SaturateToLong(static_cast<long long>(rect.right) + x);
        rect.top = SaturateToLong(static_cast<long long>(rect.top) + y);
        rect.bottom = SaturateToLong(static_cast<long long>(rect.bottom) + y);
    }
    if (!AssignRegionHandle(region, geometry)) return ERROR;
    return RegionComplexity(geometry);
}

int WINAPI BridgeGetRgnBox(HRGN region, LPRECT bounds)
{
    if (!bounds) return ERROR;
    MiniGdi::Region geometry;
    if (!CopyRegionHandle(region, &geometry)) return ERROR;
    const MiniGdi::Rect value = geometry.Bounds();
    *bounds = { value.left, value.top, value.right, value.bottom };
    return RegionComplexity(geometry);
}

BOOL WINAPI BridgePtInRegion(HRGN region, int x, int y)
{
    MiniGdi::Region geometry;
    return CopyRegionHandle(region, &geometry) && geometry.Contains({ x, y });
}

BOOL WINAPI BridgeRectInRegion(HRGN region, const RECT* rect)
{
    if (!rect) return FALSE;
    MiniGdi::Region geometry;
    if (!CopyRegionHandle(region, &geometry)) return FALSE;
    return !MiniGdi::IntersectRegion(geometry, MiniGdi::RectRegion(
        { rect->left, rect->top, rect->right, rect->bottom })).Empty();
}

BOOL WINAPI BridgeEqualRgn(HRGN first, HRGN second)
{
    MiniGdi::Region a;
    MiniGdi::Region b;
    if (!CopyRegionHandle(first, &a) || !CopyRegionHandle(second, &b) ||
        a.rectangles.size() != b.rectangles.size()) return FALSE;
    for (std::size_t index = 0; index < a.rectangles.size(); ++index)
    {
        const MiniGdi::Rect& left = a.rectangles[index];
        const MiniGdi::Rect& right = b.rectangles[index];
        if (left.left != right.left || left.top != right.top ||
            left.right != right.right || left.bottom != right.bottom) return FALSE;
    }
    return TRUE;
}

int WINAPI BridgeSelectClipRgn(HDC dc, HRGN region)
{
    GuestWindowManager* manager = CurrentManagerOrFail();
    if (!manager) return ERROR;
    if (!region) return manager->Gdi().ResetClip(FromGuestDc(dc)) ? NULLREGION : ERROR;
    std::lock_guard<std::mutex> guard(g_gdiCompatibilityLock);
    const auto found = g_regions.find(reinterpret_cast<ULONG_PTR>(region));
    if (found == g_regions.end()) return ERROR;
    const MiniGdi::Region mapped = LogicalToDeviceRegion(
        g_dcCompatibility[reinterpret_cast<ULONG_PTR>(dc)], found->second.geometry);
    return manager->Gdi().SetClipRegion(FromGuestDc(dc), mapped)
        ? RegionComplexity(mapped) : ERROR;
}

int WINAPI BridgeExtSelectClipRgn(HDC dc, HRGN region, int mode)
{
    GuestWindowManager* manager = CurrentManagerOrFail();
    if (!manager) return ERROR;
    if (!region && mode == RGN_COPY)
        return manager->Gdi().ResetClip(FromGuestDc(dc)) ? NULLREGION : ERROR;
    MiniGdi::Region source;
    MiniGdi::Region current;
    if (!CopyRegionHandle(region, &source) ||
        !manager->Gdi().GetClipRegion(FromGuestDc(dc), &current)) return ERROR;
    const MiniGdi::Region mapped = LogicalToDeviceRegion(DcState(dc), source);
    MiniGdi::Region result;
    switch (mode)
    {
    case RGN_COPY: result = mapped; break;
    case RGN_AND: result = MiniGdi::IntersectRegion(current, mapped); break;
    case RGN_OR: result = MiniGdi::UnionRegion(current, mapped); break;
    case RGN_XOR: result = MiniGdi::XorRegion(current, mapped); break;
    case RGN_DIFF: result = MiniGdi::SubtractRegion(current, mapped); break;
    default: return ERROR;
    }
    return manager->Gdi().SetClipRegion(FromGuestDc(dc), result)
        ? RegionComplexity(result) : ERROR;
}

int WINAPI BridgeGetClipBox(HDC dc, LPRECT bounds)
{
    if (!bounds) return ERROR;
    GuestWindowManager* manager = CurrentManagerOrFail();
    MiniGdi::Region clip;
    if (!manager || !manager->Gdi().GetClipRegion(FromGuestDc(dc), &clip)) return ERROR;
    const MiniGdi::Region logical = DeviceToLogicalRegion(DcState(dc), clip);
    const MiniGdi::Rect value = logical.Bounds();
    *bounds = { value.left, value.top, value.right, value.bottom };
    return RegionComplexity(logical);
}

int WINAPI BridgeIntersectClipRect(HDC dc, int left, int top, int right, int bottom)
{
    GuestWindowManager* manager = CurrentManagerOrFail();
    if (!manager) return ERROR;
    MiniGdi::Region current;
    if (!manager->Gdi().GetClipRegion(FromGuestDc(dc), &current)) return ERROR;
    const auto state = DcState(dc);
    const POINT first = LogicalToDevicePoint(state, left, top);
    const POINT second = LogicalToDevicePoint(state, right, bottom);
    const MiniGdi::Rect mapped{
        (std::min)(first.x, second.x), (std::min)(first.y, second.y),
        (std::max)(first.x, second.x), (std::max)(first.y, second.y) };
    const MiniGdi::Region result = MiniGdi::IntersectRegion(
        current, MiniGdi::RectRegion(mapped));
    return manager->Gdi().SetClipRegion(FromGuestDc(dc), result)
        ? RegionComplexity(result) : ERROR;
}

int WINAPI BridgeExcludeClipRect(HDC dc, int left, int top, int right, int bottom)
{
    GuestWindowManager* manager = CurrentManagerOrFail();
    if (!manager) return ERROR;
    MiniGdi::Region current;
    if (!manager->Gdi().GetClipRegion(FromGuestDc(dc), &current)) return ERROR;
    const auto state = DcState(dc);
    const POINT first = LogicalToDevicePoint(state, left, top);
    const POINT second = LogicalToDevicePoint(state, right, bottom);
    left = (std::min)(first.x, second.x);
    top = (std::min)(first.y, second.y);
    right = (std::max)(first.x, second.x);
    bottom = (std::max)(first.y, second.y);
    const MiniGdi::Region result = MiniGdi::SubtractRegion(current,
        MiniGdi::RectRegion({ left, top, right, bottom }));
    if (!manager->Gdi().SetClipRegion(FromGuestDc(dc), result)) return ERROR;
    return RegionComplexity(result);
}

int WINAPI BridgeGetClipRgn(HDC dc, HRGN region)
{
    GuestWindowManager* manager = CurrentManagerOrFail();
    if (!manager || !region) return -1;
    MiniGdi::Region clip;
    if (!manager->Gdi().GetClipRegion(FromGuestDc(dc), &clip)) return -1;
    std::lock_guard<std::mutex> guard(g_gdiCompatibilityLock);
    const auto state = g_dcCompatibility[reinterpret_cast<ULONG_PTR>(dc)];
    const auto target = g_regions.find(reinterpret_cast<ULONG_PTR>(region));
    if (target == g_regions.end()) return -1;
    target->second.geometry = DeviceToLogicalRegion(state, clip);
    return 1;
}

int WINAPI BridgeCombineRgn(HRGN destination, HRGN first, HRGN second, int mode)
{
    std::lock_guard<std::mutex> guard(g_gdiCompatibilityLock);
    const auto a = g_regions.find(reinterpret_cast<ULONG_PTR>(first));
    const auto b = g_regions.find(reinterpret_cast<ULONG_PTR>(second));
    const auto target = g_regions.find(reinterpret_cast<ULONG_PTR>(destination));
    if (target == g_regions.end() || a == g_regions.end() ||
        (mode != RGN_COPY && b == g_regions.end())) return ERROR;
    MiniGdi::Region result;
    switch (mode)
    {
    case RGN_COPY: result = a->second.geometry; break;
    case RGN_AND: result = MiniGdi::IntersectRegion(a->second.geometry, b->second.geometry); break;
    case RGN_OR: result = MiniGdi::UnionRegion(a->second.geometry, b->second.geometry); break;
    case RGN_XOR: result = MiniGdi::XorRegion(a->second.geometry, b->second.geometry); break;
    case RGN_DIFF: result = MiniGdi::SubtractRegion(a->second.geometry, b->second.geometry); break;
    default: return ERROR;
    }
    target->second.geometry = result;
    return RegionComplexity(result);
}

int WINAPI BridgeSaveDC(HDC dc)
{
    GuestWindowManager* manager = CurrentManagerOrFail();
    if (!manager) return 0;
    MiniGdi::Region clip;
    if (!manager->Gdi().GetClipRegion(FromGuestDc(dc), &clip)) return 0;
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
    return manager->Gdi().SetClipRegion(FromGuestDc(dc), state.clip);
}

BOOL WINAPI BridgeRectVisible(HDC dc, const RECT* rect)
{
    if (!rect) return FALSE;
    GuestWindowManager* manager = CurrentManagerOrFail();
    MiniGdi::Region clip;
    if (!manager || !manager->Gdi().GetClipRegion(FromGuestDc(dc), &clip)) return FALSE;
    const auto state = DcState(dc);
    const POINT first = LogicalToDevicePoint(state, rect->left, rect->top);
    const POINT second = LogicalToDevicePoint(state, rect->right, rect->bottom);
    return !MiniGdi::IntersectRegion(clip, MiniGdi::RectRegion(
        { (std::min)(first.x, second.x), (std::min)(first.y, second.y),
          (std::max)(first.x, second.x), (std::max)(first.y, second.y) })).Empty();
}

BOOL WINAPI BridgePolyline(HDC dc, const POINT* points, int count)
{
    if (!points || count < 2) return FALSE;
    BOOL result = BridgeMoveToEx(dc, points[0].x, points[0].y, nullptr);
    for (int index = 1; result && index < count; ++index)
        result = BridgeLineTo(dc, points[index].x, points[index].y);
    return result;
}

BOOL WINAPI BridgeBeginPath(HDC dc);
BOOL WINAPI BridgeEndPath(HDC dc);
BOOL WINAPI BridgeAbortPath(HDC dc);
BOOL WINAPI BridgeCloseFigure(HDC dc);
BOOL WINAPI BridgeStrokeAndFillPath(HDC dc);

BOOL WINAPI BridgePolygon(HDC dc, const POINT* points, int count)
{
    if (!points || count < 2) return FALSE;
    const bool existingPath = DcState(dc).pathOpen;
    if (!existingPath && !BridgeBeginPath(dc)) return FALSE;
    if (!BridgePolyline(dc, points, count) || !BridgeCloseFigure(dc)) return FALSE;
    if (existingPath) return TRUE;
    return BridgeEndPath(dc) && BridgeStrokeAndFillPath(dc);
}

BOOL WINAPI BridgePolyPolygon(HDC dc, const POINT* points, const INT* counts, int polygonCount)
{
    if (!points || !counts || polygonCount <= 0 || !BridgeBeginPath(dc)) return FALSE;
    size_t offset = 0;
    for (int polygon = 0; polygon < polygonCount; ++polygon)
    {
        if (counts[polygon] < 2 || !BridgePolyline(dc, points + offset, counts[polygon]) || !BridgeCloseFigure(dc))
        { BridgeAbortPath(dc); return FALSE; }
        offset += static_cast<size_t>(counts[polygon]);
    }
    return BridgeEndPath(dc) && BridgeStrokeAndFillPath(dc);
}

BOOL WINAPI BridgePolyPolyline(HDC dc, const POINT* points, const DWORD* counts, DWORD lineCount)
{
    if (!points || !counts) return FALSE; size_t offset = 0;
    for (DWORD line = 0; line < lineCount; ++line)
    {
        if (counts[line] < 2 || !BridgePolyline(dc, points + offset, static_cast<int>(counts[line]))) return FALSE;
        offset += counts[line];
    }
    return TRUE;
}

BOOL DrawBezierSegments(HDC dc, POINT start, const POINT* points, DWORD count)
{
    if (!points || count == 0 || count % 3 != 0) return FALSE;
    POINT p0 = start;
    for (DWORD segment = 0; segment < count; segment += 3)
    {
        const POINT p1 = points[segment], p2 = points[segment + 1], p3 = points[segment + 2];
        constexpr int Steps = 32;
        for (int step = 1; step <= Steps; ++step)
        {
            const double t = static_cast<double>(step) / Steps, u = 1.0 - t;
            const int x = static_cast<int>(std::llround(u*u*u*p0.x + 3*u*u*t*p1.x + 3*u*t*t*p2.x + t*t*t*p3.x));
            const int y = static_cast<int>(std::llround(u*u*u*p0.y + 3*u*u*t*p1.y + 3*u*t*t*p2.y + t*t*t*p3.y));
            if (!BridgeLineTo(dc, x, y)) return FALSE;
        }
        p0 = p3;
    }
    return TRUE;
}

BOOL WINAPI BridgePolyBezier(HDC dc, const POINT* points, DWORD count)
{
    if (!points || count < 4 || (count - 1) % 3 != 0) return FALSE;
    return BridgeMoveToEx(dc, points[0].x, points[0].y, nullptr) &&
        DrawBezierSegments(dc, points[0], points + 1, count - 1);
}

BOOL WINAPI BridgePolyBezierTo(HDC dc, const POINT* points, DWORD count)
{
    POINT current{}; if (!BridgeGetCurrentPositionEx(dc, &current)) return FALSE;
    return DrawBezierSegments(dc, current, points, count);
}

std::vector<POINT> ArcPoints(int left, int top, int right, int bottom,
    int startX, int startY, int endX, int endY)
{
    std::vector<POINT> result; const double cx = (left + right) / 2.0, cy = (top + bottom) / 2.0;
    const double rx = std::abs(right - left) / 2.0, ry = std::abs(bottom - top) / 2.0;
    if (rx <= 0 || ry <= 0) return result;
    double first = std::atan2((startY - cy) / ry, (startX - cx) / rx);
    double last = std::atan2((endY - cy) / ry, (endX - cx) / rx);
    while (last <= first) last += 6.2831853071795864769;
    const int steps = (std::max)(8, static_cast<int>(std::ceil((last - first) * (std::max)(rx, ry) / 4.0)));
    result.reserve(steps + 1);
    for (int index = 0; index <= steps; ++index)
    {
        const double angle = first + (last - first) * index / steps;
        result.push_back({ static_cast<LONG>(std::llround(cx + std::cos(angle) * rx)),
            static_cast<LONG>(std::llround(cy + std::sin(angle) * ry)) });
    }
    return result;
}

BOOL WINAPI BridgeArc(HDC dc, int left, int top, int right, int bottom,
    int startX, int startY, int endX, int endY)
{
    const auto points = ArcPoints(left, top, right, bottom, startX, startY, endX, endY);
    return points.size() >= 2 ? BridgePolyline(dc, points.data(), static_cast<int>(points.size())) : FALSE;
}

BOOL WINAPI BridgeArcTo(HDC dc, int left, int top, int right, int bottom,
    int startX, int startY, int endX, int endY)
{
    const auto points = ArcPoints(left, top, right, bottom, startX, startY, endX, endY);
    if (points.size() < 2 || !BridgeLineTo(dc, points.front().x, points.front().y)) return FALSE;
    for (size_t index = 1; index < points.size(); ++index)
        if (!BridgeLineTo(dc, points[index].x, points[index].y)) return FALSE;
    return TRUE;
}

BOOL WINAPI BridgePie(HDC dc, int left, int top, int right, int bottom,
    int startX, int startY, int endX, int endY)
{
    auto points = ArcPoints(left, top, right, bottom, startX, startY, endX, endY);
    points.push_back({ (left + right) / 2, (top + bottom) / 2 });
    return points.size() >= 4 ? BridgePolygon(dc, points.data(), static_cast<int>(points.size())) : FALSE;
}

BOOL WINAPI BridgeChord(HDC dc, int left, int top, int right, int bottom,
    int startX, int startY, int endX, int endY)
{
    const auto points = ArcPoints(left, top, right, bottom, startX, startY, endX, endY);
    return points.size() >= 3 ? BridgePolygon(dc, points.data(), static_cast<int>(points.size())) : FALSE;
}

BOOL WINAPI BridgeBeginPath(HDC dc)
{
    GuestWindowManager* manager = CurrentManagerOrFail();
    if (!manager || !manager->Gdi().HasDc(FromGuestDc(dc))) return FALSE;
    std::lock_guard<std::mutex> guard(g_gdiCompatibilityLock);
    auto& state = g_dcCompatibility[reinterpret_cast<ULONG_PTR>(dc)];
    state.pathPoints.clear(); state.pathTypes.clear(); state.pathOpen = true; state.pathReady = false;
    return TRUE;
}

BOOL WINAPI BridgeEndPath(HDC dc)
{
    std::lock_guard<std::mutex> guard(g_gdiCompatibilityLock);
    auto& state = g_dcCompatibility[reinterpret_cast<ULONG_PTR>(dc)];
    if (!state.pathOpen) return FALSE;
    state.pathOpen = false; state.pathReady = true; return TRUE;
}

BOOL WINAPI BridgeAbortPath(HDC dc)
{
    std::lock_guard<std::mutex> guard(g_gdiCompatibilityLock);
    auto& state = g_dcCompatibility[reinterpret_cast<ULONG_PTR>(dc)];
    state.pathPoints.clear(); state.pathTypes.clear(); state.pathOpen = false; state.pathReady = false;
    return TRUE;
}

BOOL WINAPI BridgeCloseFigure(HDC dc)
{
    std::lock_guard<std::mutex> guard(g_gdiCompatibilityLock);
    auto& state = g_dcCompatibility[reinterpret_cast<ULONG_PTR>(dc)];
    if (!state.pathOpen || state.pathTypes.empty()) return FALSE;
    state.pathTypes.back() |= PT_CLOSEFIGURE; return TRUE;
}

MiniGdi::Region PathRegion(const GuestDcCompatibilityState& state)
{
    MiniGdi::Region result;
    size_t begin = 0;
    while (begin < state.pathPoints.size())
    {
        size_t end = begin + 1;
        while (end < state.pathPoints.size() && (state.pathTypes[end] & ~PT_CLOSEFIGURE) != PT_MOVETO) ++end;
        if (end - begin >= 3)
        {
            int top = state.pathPoints[begin].y, bottom = top;
            for (size_t index = begin + 1; index < end; ++index)
            { top = (std::min)(top, state.pathPoints[index].y); bottom = (std::max)(bottom, state.pathPoints[index].y); }
            for (int y = top; y < bottom; ++y)
            {
                std::vector<int> crossings;
                for (size_t index = begin, previous = end - 1; index < end; previous = index++)
                {
                    const POINT& a = state.pathPoints[previous]; const POINT& b = state.pathPoints[index];
                    if ((a.y <= y && b.y > y) || (b.y <= y && a.y > y))
                        crossings.push_back(a.x + static_cast<int>((static_cast<long long>(y - a.y) * (b.x - a.x)) / (b.y - a.y)));
                }
                std::sort(crossings.begin(), crossings.end());
                for (size_t index = 1; index < crossings.size(); index += 2)
                    if (crossings[index - 1] < crossings[index])
                        result.rectangles.push_back({ crossings[index - 1], y, crossings[index], y + 1 });
            }
        }
        begin = end;
    }
    return result;
}

BOOL PaintCurrentPath(HDC dc, bool fill, bool stroke)
{
    GuestWindowManager* manager = CurrentManagerOrFail();
    if (!manager) return FALSE;
    GuestDcCompatibilityState state = DcState(dc);
    if (!state.pathReady) return FALSE;
    MiniGdi::Surface* surface = manager->Gdi().GetSurface(FromGuestDc(dc));
    MiniGdi::Region clip; MiniGdi::Brush brush{}; MiniGdi::Pen pen{};
    if (!surface || !manager->Gdi().GetClipRegion(FromGuestDc(dc), &clip)) return FALSE;
    manager->Gdi().GetSelectedBrush(FromGuestDc(dc), &brush);
    manager->Gdi().GetSelectedPen(FromGuestDc(dc), &pen);
    if (fill && !brush.isNull)
    {
        MiniGdi::Region painted = MiniGdi::IntersectRegion(PathRegion(state), clip);
        for (const auto& rectangle : painted.rectangles) MiniGdi::FillRect(*surface, rectangle, brush.color, nullptr);
    }
    if (stroke && !pen.isNull)
    {
        size_t start = 0;
        for (size_t index = 1; index < state.pathPoints.size(); ++index)
        {
            if ((state.pathTypes[index] & ~PT_CLOSEFIGURE) == PT_MOVETO) { start = index; continue; }
            for (const auto& rectangle : clip.rectangles)
                MiniGdi::DrawLine(*surface, { state.pathPoints[index - 1].x, state.pathPoints[index - 1].y },
                    { state.pathPoints[index].x, state.pathPoints[index].y }, pen.color, pen.width, &rectangle);
            if ((state.pathTypes[index] & PT_CLOSEFIGURE) != 0)
                for (const auto& rectangle : clip.rectangles)
                    MiniGdi::DrawLine(*surface, { state.pathPoints[index].x, state.pathPoints[index].y },
                        { state.pathPoints[start].x, state.pathPoints[start].y }, pen.color, pen.width, &rectangle);
        }
    }
    BridgeAbortPath(dc); return TRUE;
}

BOOL WINAPI BridgeFillPath(HDC dc) { return PaintCurrentPath(dc, true, false); }
BOOL WINAPI BridgeStrokePath(HDC dc) { return PaintCurrentPath(dc, false, true); }
BOOL WINAPI BridgeStrokeAndFillPath(HDC dc) { return PaintCurrentPath(dc, true, true); }
BOOL WINAPI BridgeFlattenPath(HDC dc) { return DcState(dc).pathReady ? TRUE : FALSE; }
BOOL WINAPI BridgeWidenPath(HDC dc) { return DcState(dc).pathReady ? TRUE : FALSE; }

int WINAPI BridgeGetPath(HDC dc, LPPOINT points, LPBYTE types, int count)
{
    const auto state = DcState(dc); if (!state.pathReady) return -1;
    const int required = static_cast<int>((std::min)(state.pathPoints.size(), static_cast<size_t>(INT_MAX)));
    if (!points || !types) return required;
    if (count < required) return -1;
    for (int index = 0; index < required; ++index)
    { points[index] = DeviceToLogicalPoint(state, state.pathPoints[index].x, state.pathPoints[index].y); types[index] = state.pathTypes[index]; }
    return required;
}

BOOL WINAPI BridgeSelectClipPath(HDC dc, int mode)
{
    GuestWindowManager* manager = CurrentManagerOrFail(); const auto state = DcState(dc);
    if (!manager || !state.pathReady) return FALSE;
    MiniGdi::Region existing; manager->Gdi().GetClipRegion(FromGuestDc(dc), &existing);
    MiniGdi::Region path = PathRegion(state), result;
    if (mode == RGN_COPY) result = path;
    else if (mode == RGN_AND) result = MiniGdi::IntersectRegion(existing, path);
    else if (mode == RGN_OR) result = MiniGdi::UnionRegion(existing, path);
    else if (mode == RGN_XOR) result = MiniGdi::XorRegion(existing, path);
    else if (mode == RGN_DIFF) result = MiniGdi::SubtractRegion(existing, path);
    else return FALSE;
    BridgeAbortPath(dc); return manager->Gdi().SetClipRegion(FromGuestDc(dc), result);
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

BOOL WINAPI BridgeExtTextOutW(HDC dc, int x, int y, UINT options, const RECT* rect,
    LPCWSTR text, UINT count, const INT* spacing)
{
    if ((count != 0 && !text) ||
        ((options & (ETO_CLIPPED | ETO_OPAQUE)) != 0 && !rect))
    {
        BridgeSetLastError(ERROR_INVALID_PARAMETER);
        return FALSE;
    }
    if ((options & ETO_GLYPH_INDEX) != 0)
    {
        // Glyph-index output requires a DirectWrite glyph run.  Treating the
        // WORD values as UTF-16 would silently draw unrelated characters.
        BridgeSetLastError(ERROR_NOT_SUPPORTED);
        return FALSE;
    }

    GuestWindowManager* manager = CurrentManagerOrFail();
    const MiniGdi::DcHandle resolvedDc = FromGuestDc(dc);
    if (!manager || !manager->Gdi().HasDc(resolvedDc)) return FALSE;

    const auto state = DcState(dc);
    POINT origin = LogicalToDevicePoint(state, x, y);
    MiniGdi::Point current{};
    const bool updateCurrentPosition = (state.textAlign & TA_UPDATECP) != 0;
    if (updateCurrentPosition)
    {
        if (manager->Gdi().GetCurrentPosition(resolvedDc, &current))
            origin = { current.x, current.y };
        else
            current = { origin.x, origin.y };
    }

    MiniGdi::Size extent{};
    if (!manager->Gdi().GetTextExtentW(resolvedDc, text, count, &extent))
    {
        BridgeSetLastError(ERROR_INVALID_PARAMETER);
        return FALSE;
    }
    MiniGdi::FontMetrics metrics{};
    manager->Gdi().GetSelectedFontMetrics(resolvedDc, &metrics);

    switch (state.textAlign & (TA_LEFT | TA_RIGHT | TA_CENTER))
    {
    case TA_RIGHT: origin.x -= extent.width; break;
    case TA_CENTER: origin.x -= extent.width / 2; break;
    default: break;
    }
    if ((state.textAlign & TA_BASELINE) == TA_BASELINE)
        origin.y -= metrics.ascent;
    else if ((state.textAlign & TA_BOTTOM) == TA_BOTTOM)
        origin.y -= extent.height;

    MiniGdi::Region oldClip;
    bool clipChanged = false;
    MiniGdi::Rect outputRect{};
    if (rect)
    {
        const POINT first = LogicalToDevicePoint(state, rect->left, rect->top);
        const POINT second = LogicalToDevicePoint(state, rect->right, rect->bottom);
        outputRect = {
            (std::min)(first.x, second.x), (std::min)(first.y, second.y),
            (std::max)(first.x, second.x), (std::max)(first.y, second.y) };
    }
    if ((options & ETO_CLIPPED) != 0 &&
        manager->Gdi().GetClipRegion(resolvedDc, &oldClip))
    {
        manager->Gdi().SetClipRegion(resolvedDc, MiniGdi::IntersectRegion(
            oldClip, MiniGdi::RectRegion(outputRect)));
        clipChanged = true;
    }

    if ((options & ETO_OPAQUE) != 0)
    {
        MiniGdi::Color background = MiniGdi::OpaqueWhite;
        manager->Gdi().GetBackgroundColor(resolvedDc, &background);
        const MiniGdi::ObjectHandle brush = manager->Gdi().CreateSolidBrush(background);
        if (brush != MiniGdi::InvalidObject)
        {
            MiniGdi::ObjectHandle previous = MiniGdi::InvalidObject;
            if (manager->Gdi().SelectBrush(resolvedDc, brush, &previous))
            {
                manager->Gdi().FillRect(resolvedDc, outputRect);
                if (previous != MiniGdi::InvalidObject)
                    manager->Gdi().SelectBrush(resolvedDc, previous, nullptr);
            }
            manager->Gdi().DeleteObject(brush);
        }
    }

    bool drawn = true;
    LONG advanceX = extent.width;
    LONG advanceY = 0;
    if (!spacing || count == 0)
    {
        drawn = manager->Gdi().TextOutW(
            resolvedDc, { origin.x, origin.y }, text, count, nullptr);
    }
    else
    {
        POINT cursor = origin;
        advanceX = 0;
        for (UINT index = 0; index < count && drawn; ++index)
        {
            drawn = manager->Gdi().TextOutW(
                resolvedDc, { cursor.x, cursor.y }, text + index, 1, nullptr);
            const LONG logicalX = spacing[(options & ETO_PDY) ? index * 2 : index];
            const LONG logicalY = (options & ETO_PDY) ? spacing[index * 2 + 1] : 0;
            const LONG deviceX = ScaleCoordinate(
                logicalX, state.viewportExtent.cx, state.windowExtent.cx);
            const LONG deviceY = ScaleCoordinate(
                logicalY, state.viewportExtent.cy, state.windowExtent.cy);
            cursor.x += deviceX;
            cursor.y += deviceY;
            advanceX += deviceX;
            advanceY += deviceY;
        }
    }

    if (clipChanged)
        manager->Gdi().SetClipRegion(resolvedDc, oldClip);
    if (drawn && updateCurrentPosition)
        manager->Gdi().MoveTo(resolvedDc,
            { current.x + advanceX, current.y + advanceY }, nullptr);

    BridgeSetLastError(drawn ? ERROR_SUCCESS : ERROR_INVALID_PARAMETER);
    return drawn ? TRUE : FALSE;
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
    int fitted = 0;
    for (int index = 0; index < count; ++index)
    {
        SIZE prefix{};
        if (!BridgeGetTextExtentPoint32W(dc, text, index + 1, &prefix)) return FALSE;
        if (advances) advances[index] = prefix.cx;
        if (maximum < 0 || prefix.cx <= maximum) fitted = index + 1;
    }
    if (fit) *fit = fitted;
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

MiniGdi::Color ApplyRasterOperation(
    DWORD operation, MiniGdi::Color source, MiniGdi::Color destination,
    MiniGdi::Color pattern)
{
    const BYTE rop = static_cast<BYTE>((operation >> 16) & 0xffu);
    const DWORD s = source & 0x00ffffffu;
    const DWORD d = destination & 0x00ffffffu;
    const DWORD p = pattern & 0x00ffffffu;
    DWORD result = 0;
    for (unsigned combination = 0; combination < 8; ++combination)
    {
        if ((rop & (1u << combination)) == 0) continue;
        const DWORD pm = (combination & 4u) ? p : ~p;
        const DWORD sm = (combination & 2u) ? s : ~s;
        const DWORD dm = (combination & 1u) ? d : ~d;
        result |= pm & sm & dm;
    }
    return (result & 0x00ffffffu) | 0xff000000u;
}

MiniGdi::Color SelectedPatternColor(GuestWindowManager* manager, HDC dc)
{
    MiniGdi::Brush brush{};
    return manager && manager->Gdi().GetSelectedBrush(FromGuestDc(dc), &brush) && !brush.isNull
        ? brush.color : MiniGdi::OpaqueWhite;
}

BOOL WINAPI BridgeStretchBlt(HDC destination, int x, int y, int width, int height,
    HDC source, int sourceX, int sourceY, int sourceWidth, int sourceHeight, DWORD operation)
{
    if (width == 0 || height == 0 || sourceWidth == 0 || sourceHeight == 0) return TRUE;
    GuestWindowManager* manager = CurrentManagerOrFail();
    if (!manager) return FALSE;
    MiniGdi::Surface* output = manager->Gdi().GetSurface(FromGuestDc(destination));
    const MiniGdi::Surface* input = manager->Gdi().GetSurface(FromGuestDc(source));
    if (!output || !input) return FALSE;
    const auto destinationState = DcState(destination);
    const auto sourceState = DcState(source);
    const POINT destinationFirst = LogicalToDevicePoint(destinationState, x, y);
    const POINT destinationSecond = LogicalToDevicePoint(
        destinationState, x + width, y + height);
    const POINT sourceFirst = LogicalToDevicePoint(sourceState, sourceX, sourceY);
    const POINT sourceSecond = LogicalToDevicePoint(
        sourceState, sourceX + sourceWidth, sourceY + sourceHeight);
    const bool mirrorX = (destinationSecond.x < destinationFirst.x) !=
        (sourceSecond.x < sourceFirst.x);
    const bool mirrorY = (destinationSecond.y < destinationFirst.y) !=
        (sourceSecond.y < sourceFirst.y);
    x = (std::min)(destinationFirst.x, destinationSecond.x);
    y = (std::min)(destinationFirst.y, destinationSecond.y);
    width = (std::abs)(destinationSecond.x - destinationFirst.x);
    height = (std::abs)(destinationSecond.y - destinationFirst.y);
    sourceX = (std::min)(sourceFirst.x, sourceSecond.x);
    sourceY = (std::min)(sourceFirst.y, sourceSecond.y);
    sourceWidth = (std::abs)(sourceSecond.x - sourceFirst.x);
    sourceHeight = (std::abs)(sourceSecond.y - sourceFirst.y);
    if (width == 0 || height == 0 || sourceWidth == 0 || sourceHeight == 0) return TRUE;
    MiniGdi::Region clip;
    manager->Gdi().GetClipRegion(FromGuestDc(destination), &clip);
    const MiniGdi::Color pattern = SelectedPatternColor(manager, destination);
    // Sampling first is required when source and destination refer to the same
    // surface and their rectangles overlap.  GDI blits behave as if the source
    // rectangle were immutable for the duration of the operation.
    std::vector<MiniGdi::Color> samples(static_cast<size_t>(width) * height);
    for (int dy = 0; dy < height; ++dy) for (int dx = 0; dx < width; ++dx)
    {
        const int sampleX = mirrorX ? width - 1 - dx : dx;
        const int sampleY = mirrorY ? height - 1 - dy : dy;
        const MiniGdi::Color* pixel = input->PixelAt(sourceX + sampleX * sourceWidth / width,
            sourceY + sampleY * sourceHeight / height);
        samples[static_cast<size_t>(dy) * width + dx] = pixel ? *pixel : MiniGdi::Transparent;
    }
    for (int dy = 0; dy < height; ++dy) for (int dx = 0; dx < width; ++dx)
    {
        const int tx = x + dx, ty = y + dy;
        if (!clip.Contains({ tx, ty })) continue;
        MiniGdi::Color* target = output->PixelAt(tx, ty);
        if (!target) continue;
        const MiniGdi::Color sourcePixel = samples[static_cast<size_t>(dy) * width + dx];
        *target = ApplyRasterOperation(operation, sourcePixel, *target, pattern);
    }
    BridgeSetLastError(ERROR_SUCCESS);
    return TRUE;
}

BOOL WINAPI BridgeMaskBlt(HDC destination, int x, int y, int width, int height,
    HDC source, int sourceX, int sourceY, HBITMAP maskBitmap,
    int maskX, int maskY, DWORD rasterOperation)
{
    if (width <= 0 || height <= 0) return width == 0 || height == 0;
    GuestWindowManager* manager = CurrentManagerOrFail();
    if (!manager) return FALSE;
    MiniGdi::Surface* output = manager->Gdi().GetSurface(FromGuestDc(destination));
    const MiniGdi::Surface* input = manager->Gdi().GetSurface(FromGuestDc(source));
    const MiniGdi::Surface* mask = maskBitmap
        ? manager->Gdi().GetBitmapSurface(FromGuestObject(maskBitmap)) : nullptr;
    if (!output || !input || !mask)
    { BridgeSetLastError(ERROR_INVALID_HANDLE); return FALSE; }
    const auto destinationState = DcState(destination);
    const auto sourceState = DcState(source);
    const POINT destinationFirst = LogicalToDevicePoint(destinationState, x, y);
    const POINT destinationSecond = LogicalToDevicePoint(destinationState, x + width, y + height);
    const POINT sourceFirst = LogicalToDevicePoint(sourceState, sourceX, sourceY);
    const POINT sourceSecond = LogicalToDevicePoint(sourceState, sourceX + width, sourceY + height);
    const int destinationX = (std::min)(destinationFirst.x, destinationSecond.x);
    const int destinationY = (std::min)(destinationFirst.y, destinationSecond.y);
    const int destinationWidth = (std::abs)(destinationSecond.x - destinationFirst.x);
    const int destinationHeight = (std::abs)(destinationSecond.y - destinationFirst.y);
    const int sourceBaseX = (std::min)(sourceFirst.x, sourceSecond.x);
    const int sourceBaseY = (std::min)(sourceFirst.y, sourceSecond.y);
    const int sourceDeviceWidth = (std::abs)(sourceSecond.x - sourceFirst.x);
    const int sourceDeviceHeight = (std::abs)(sourceSecond.y - sourceFirst.y);
    if (!destinationWidth || !destinationHeight || !sourceDeviceWidth || !sourceDeviceHeight) return TRUE;
    MiniGdi::Region clip;
    manager->Gdi().GetClipRegion(FromGuestDc(destination), &clip);
    std::vector<MiniGdi::Color> samples(static_cast<size_t>(destinationWidth) * destinationHeight);
    for (int row = 0; row < destinationHeight; ++row)
        for (int column = 0; column < destinationWidth; ++column)
        {
            const MiniGdi::Color* pixel = input->PixelAt(
                sourceBaseX + column * sourceDeviceWidth / destinationWidth,
                sourceBaseY + row * sourceDeviceHeight / destinationHeight);
            samples[static_cast<size_t>(row) * destinationWidth + column] =
                pixel ? *pixel : MiniGdi::Transparent;
        }
    const DWORD foreground = rasterOperation & 0x00ffffffu;
    const DWORD background = (rasterOperation & 0xff000000u) >> 8;
    const MiniGdi::Color pattern = SelectedPatternColor(manager, destination);
    for (int row = 0; row < destinationHeight; ++row)
        for (int column = 0; column < destinationWidth; ++column)
        {
            const int targetX = destinationX + column;
            const int targetY = destinationY + row;
            if (!clip.Contains({ targetX, targetY })) continue;
            MiniGdi::Color* target = output->PixelAt(targetX, targetY);
            const MiniGdi::Color* maskPixel = mask->PixelAt(
                maskX + column * width / destinationWidth,
                maskY + row * height / destinationHeight);
            if (!target || !maskPixel) continue;
            const bool set = (MiniGdi::Red(*maskPixel) + MiniGdi::Green(*maskPixel) +
                MiniGdi::Blue(*maskPixel)) >= 384;
            *target = ApplyRasterOperation(set ? foreground : background,
                samples[static_cast<size_t>(row) * destinationWidth + column], *target, pattern);
        }
    return TRUE;
}

BOOL WINAPI BridgePlgBlt(HDC destination, const POINT* points, HDC source,
    int sourceX, int sourceY, int sourceWidth, int sourceHeight,
    HBITMAP maskBitmap, int maskX, int maskY)
{
    if (!points || sourceWidth == 0 || sourceHeight == 0)
    { BridgeSetLastError(ERROR_INVALID_PARAMETER); return FALSE; }
    GuestWindowManager* manager = CurrentManagerOrFail();
    if (!manager) return FALSE;
    MiniGdi::Surface* output = manager->Gdi().GetSurface(FromGuestDc(destination));
    const MiniGdi::Surface* input = manager->Gdi().GetSurface(FromGuestDc(source));
    const MiniGdi::Surface* mask = maskBitmap
        ? manager->Gdi().GetBitmapSurface(FromGuestObject(maskBitmap)) : nullptr;
    if (!output || !input || (maskBitmap && !mask))
    { BridgeSetLastError(ERROR_INVALID_HANDLE); return FALSE; }
    const auto destinationState = DcState(destination);
    const auto sourceState = DcState(source);
    const POINT p0 = LogicalToDevicePoint(destinationState, points[0].x, points[0].y);
    const POINT p1 = LogicalToDevicePoint(destinationState, points[1].x, points[1].y);
    const POINT p2 = LogicalToDevicePoint(destinationState, points[2].x, points[2].y);
    const POINT s0 = LogicalToDevicePoint(sourceState, sourceX, sourceY);
    const POINT s1 = LogicalToDevicePoint(sourceState,
        sourceX + sourceWidth, sourceY + sourceHeight);
    const double ux = static_cast<double>(p1.x - p0.x);
    const double uy = static_cast<double>(p1.y - p0.y);
    const double vx = static_cast<double>(p2.x - p0.x);
    const double vy = static_cast<double>(p2.y - p0.y);
    const double determinant = ux * vy - uy * vx;
    if (std::abs(determinant) <= 1.0e-12)
    { BridgeSetLastError(ERROR_INVALID_PARAMETER); return FALSE; }
    const int fourthX = p1.x + p2.x - p0.x;
    const int fourthY = p1.y + p2.y - p0.y;
    const int left = (std::min)({ p0.x, p1.x, p2.x, fourthX });
    const int right = (std::max)({ p0.x, p1.x, p2.x, fourthX });
    const int top = (std::min)({ p0.y, p1.y, p2.y, fourthY });
    const int bottom = (std::max)({ p0.y, p1.y, p2.y, fourthY });
    MiniGdi::Region clip;
    manager->Gdi().GetClipRegion(FromGuestDc(destination), &clip);
    const int sw = (std::abs)(s1.x - s0.x), sh = (std::abs)(s1.y - s0.y);
    if (!sw || !sh) return TRUE;
    std::vector<MiniGdi::Color> samples(static_cast<size_t>(sw) * sh);
    for (int row = 0; row < sh; ++row) for (int column = 0; column < sw; ++column)
    {
        const int sampleX = s0.x + (s1.x >= s0.x ? column : -column - 1);
        const int sampleY = s0.y + (s1.y >= s0.y ? row : -row - 1);
        const MiniGdi::Color* pixel = input->PixelAt(sampleX, sampleY);
        samples[static_cast<size_t>(row) * sw + column] = pixel ? *pixel : MiniGdi::Transparent;
    }
    for (int targetY = top; targetY < bottom; ++targetY)
        for (int targetX = left; targetX < right; ++targetX)
        {
            if (!clip.Contains({ targetX, targetY })) continue;
            const double dx = targetX + 0.5 - p0.x;
            const double dy = targetY + 0.5 - p0.y;
            const double a = (dx * vy - dy * vx) / determinant;
            const double b = (dy * ux - dx * uy) / determinant;
            if (a < 0.0 || b < 0.0 || a >= 1.0 || b >= 1.0) continue;
            const int sampleX = (std::min)(sw - 1, static_cast<int>(a * sw));
            const int sampleY = (std::min)(sh - 1, static_cast<int>(b * sh));
            if (mask)
            {
                const MiniGdi::Color* maskPixel = mask->PixelAt(maskX + sampleX, maskY + sampleY);
                if (!maskPixel || MiniGdi::Red(*maskPixel) + MiniGdi::Green(*maskPixel) +
                    MiniGdi::Blue(*maskPixel) < 384) continue;
            }
            MiniGdi::Color* target = output->PixelAt(targetX, targetY);
            if (target) *target = samples[static_cast<size_t>(sampleY) * sw + sampleX];
        }
    return TRUE;
}

BOOL WINAPI BridgeGdiAlphaBlend(HDC destination, int x, int y, int width, int height,
    HDC source, int sourceX, int sourceY, int sourceWidth, int sourceHeight, DWORD blend)
{
    const BYTE blendOperation = static_cast<BYTE>(blend);
    const BYTE blendFlags = static_cast<BYTE>(blend >> 8);
    const BYTE constantAlpha = static_cast<BYTE>(blend >> 16);
    const BYTE alphaFormat = static_cast<BYTE>(blend >> 24);
    constexpr BYTE SourceOver = 0;
    constexpr BYTE SourceAlpha = 1;
    if (blendOperation != SourceOver || blendFlags != 0 ||
        (alphaFormat != 0 && alphaFormat != SourceAlpha) || width <= 0 || height <= 0 ||
        sourceWidth <= 0 || sourceHeight <= 0)
    {
        BridgeSetLastError(ERROR_INVALID_PARAMETER);
        return FALSE;
    }
    GuestWindowManager* manager = CurrentManagerOrFail();
    if (!manager) return FALSE;
    MiniGdi::Surface* output = manager->Gdi().GetSurface(FromGuestDc(destination));
    const MiniGdi::Surface* input = manager->Gdi().GetSurface(FromGuestDc(source));
    if (!output || !input) { BridgeSetLastError(ERROR_INVALID_HANDLE); return FALSE; }
    const auto destinationState = DcState(destination);
    const auto sourceState = DcState(source);
    const POINT d0 = LogicalToDevicePoint(destinationState, x, y);
    const POINT d1 = LogicalToDevicePoint(destinationState, x + width, y + height);
    const POINT s0 = LogicalToDevicePoint(sourceState, sourceX, sourceY);
    const POINT s1 = LogicalToDevicePoint(sourceState, sourceX + sourceWidth, sourceY + sourceHeight);
    x = (std::min)(d0.x, d1.x); y = (std::min)(d0.y, d1.y);
    width = (std::abs)(d1.x - d0.x); height = (std::abs)(d1.y - d0.y);
    sourceX = (std::min)(s0.x, s1.x); sourceY = (std::min)(s0.y, s1.y);
    sourceWidth = (std::abs)(s1.x - s0.x); sourceHeight = (std::abs)(s1.y - s0.y);
    if (!width || !height || !sourceWidth || !sourceHeight) return TRUE;
    MiniGdi::Region clip;
    manager->Gdi().GetClipRegion(FromGuestDc(destination), &clip);
    std::vector<MiniGdi::Color> samples(static_cast<size_t>(width) * height);
    for (int dy = 0; dy < height; ++dy) for (int dx = 0; dx < width; ++dx)
    {
        const MiniGdi::Color* pixel = input->PixelAt(
            sourceX + dx * sourceWidth / width, sourceY + dy * sourceHeight / height);
        samples[static_cast<size_t>(dy) * width + dx] = pixel ? *pixel : MiniGdi::Transparent;
    }
    const unsigned constant = constantAlpha;
    for (int dy = 0; dy < height; ++dy) for (int dx = 0; dx < width; ++dx)
    {
        const int tx = x + dx, ty = y + dy;
        if (!clip.Contains({ tx, ty })) continue;
        MiniGdi::Color* destinationPixel = output->PixelAt(tx, ty);
        if (!destinationPixel) continue;
        const MiniGdi::Color sourcePixel = samples[static_cast<size_t>(dy) * width + dx];
        const unsigned pixelAlpha = alphaFormat == SourceAlpha ? MiniGdi::Alpha(sourcePixel) : 255u;
        const unsigned sourceAlpha = (pixelAlpha * constant + 127u) / 255u;
        const unsigned inverse = 255u - sourceAlpha;
        auto channel = [&](BYTE sourceChannel, BYTE destinationChannel) -> BYTE
        {
            // AC_SRC_ALPHA input is premultiplied by its per-pixel alpha.
            const unsigned sourceContribution = alphaFormat == SourceAlpha
                ? (static_cast<unsigned>(sourceChannel) * constant + 127u) / 255u
                : (static_cast<unsigned>(sourceChannel) * sourceAlpha + 127u) / 255u;
            return static_cast<BYTE>((std::min)(255u, sourceContribution +
                (static_cast<unsigned>(destinationChannel) * inverse + 127u) / 255u));
        };
        const BYTE outputAlpha = static_cast<BYTE>((std::min)(255u, sourceAlpha +
            (static_cast<unsigned>(MiniGdi::Alpha(*destinationPixel)) * inverse + 127u) / 255u));
        *destinationPixel = MiniGdi::MakeColor(
            channel(MiniGdi::Red(sourcePixel), MiniGdi::Red(*destinationPixel)),
            channel(MiniGdi::Green(sourcePixel), MiniGdi::Green(*destinationPixel)),
            channel(MiniGdi::Blue(sourcePixel), MiniGdi::Blue(*destinationPixel)), outputAlpha);
    }
    BridgeSetLastError(ERROR_SUCCESS);
    return TRUE;
}

BOOL WINAPI BridgeTransparentBlt(HDC destination, int x, int y, int width, int height,
    HDC source, int sourceX, int sourceY, int sourceWidth, int sourceHeight, UINT transparent)
{
    if (width <= 0 || height <= 0 || sourceWidth <= 0 || sourceHeight <= 0)
        return FALSE;
    GuestWindowManager* manager = CurrentManagerOrFail();
    if (!manager) return FALSE;
    MiniGdi::Surface* output = manager->Gdi().GetSurface(FromGuestDc(destination));
    const MiniGdi::Surface* input = manager->Gdi().GetSurface(FromGuestDc(source));
    if (!output || !input) { BridgeSetLastError(ERROR_INVALID_HANDLE); return FALSE; }
    const auto destinationState = DcState(destination);
    const auto sourceState = DcState(source);
    const POINT d0 = LogicalToDevicePoint(destinationState, x, y);
    const POINT d1 = LogicalToDevicePoint(destinationState, x + width, y + height);
    const POINT s0 = LogicalToDevicePoint(sourceState, sourceX, sourceY);
    const POINT s1 = LogicalToDevicePoint(sourceState, sourceX + sourceWidth, sourceY + sourceHeight);
    x = (std::min)(d0.x, d1.x); y = (std::min)(d0.y, d1.y);
    width = (std::abs)(d1.x - d0.x); height = (std::abs)(d1.y - d0.y);
    sourceX = (std::min)(s0.x, s1.x); sourceY = (std::min)(s0.y, s1.y);
    sourceWidth = (std::abs)(s1.x - s0.x); sourceHeight = (std::abs)(s1.y - s0.y);
    MiniGdi::Region clip;
    manager->Gdi().GetClipRegion(FromGuestDc(destination), &clip);
    std::vector<MiniGdi::Color> samples(static_cast<size_t>(width) * height);
    for (int dy = 0; dy < height; ++dy) for (int dx = 0; dx < width; ++dx)
    {
        const MiniGdi::Color* pixel = input->PixelAt(
            sourceX + dx * sourceWidth / width, sourceY + dy * sourceHeight / height);
        samples[static_cast<size_t>(dy) * width + dx] = pixel ? *pixel : MiniGdi::Transparent;
    }
    const BYTE keyRed = GetRValue(transparent), keyGreen = GetGValue(transparent),
        keyBlue = GetBValue(transparent);
    for (int dy = 0; dy < height; ++dy) for (int dx = 0; dx < width; ++dx)
    {
        const int tx = x + dx, ty = y + dy;
        if (!clip.Contains({ tx, ty })) continue;
        const MiniGdi::Color pixel = samples[static_cast<size_t>(dy) * width + dx];
        if (MiniGdi::Red(pixel) == keyRed && MiniGdi::Green(pixel) == keyGreen &&
            MiniGdi::Blue(pixel) == keyBlue) continue;
        MiniGdi::Color* target = output->PixelAt(tx, ty);
        if (target) *target = pixel;
    }
    BridgeSetLastError(ERROR_SUCCESS);
    return TRUE;
}

int WINAPI BridgeEnumFontsW(
    HDC dc, LPCWSTR faceName, GuestFontEnumProcW callback, LPARAM parameter);

int WINAPI BridgeEnumFontFamiliesExW(HDC dc, LOGFONTW* font, GuestFontEnumProcW callback,
    LPARAM parameter, DWORD)
{
    UNREFERENCED_PARAMETER(dc);
    if (!callback) return 0;
    GuestEnumLogFontExW enumerated{};
    enumerated.logFont = font ? *font : LOGFONTW{};
    if (!enumerated.logFont.lfFaceName[0])
        wcscpy_s(enumerated.logFont.lfFaceName,
            _countof(enumerated.logFont.lfFaceName), L"Segoe UI");
    wcscpy_s(enumerated.fullName, _countof(enumerated.fullName),
        enumerated.logFont.lfFaceName);
    wcscpy_s(enumerated.style, _countof(enumerated.style),
        enumerated.logFont.lfItalic ? L"Italic" : L"Regular");
    wcscpy_s(enumerated.script, _countof(enumerated.script), L"Western");
    GuestNewTextMetricExW metrics{};
    metrics.newTextMetric.textMetric.tmHeight = 16;
    metrics.newTextMetric.textMetric.tmAscent = 13;
    metrics.newTextMetric.textMetric.tmDescent = 3;
    metrics.newTextMetric.textMetric.tmAveCharWidth = 8;
    metrics.newTextMetric.textMetric.tmMaxCharWidth = 16;
    metrics.newTextMetric.textMetric.tmWeight = enumerated.logFont.lfWeight
        ? enumerated.logFont.lfWeight : FW_NORMAL;
    metrics.newTextMetric.textMetric.tmCharSet = enumerated.logFont.lfCharSet == DEFAULT_CHARSET
        ? ANSI_CHARSET : enumerated.logFont.lfCharSet;
    metrics.newTextMetric.flags = 0x00000040u; // NTM_REGULAR
    return InvokeGuestFontEnumProcW(callback,
        reinterpret_cast<const LOGFONTW*>(&enumerated),
        reinterpret_cast<const TEXTMETRICW*>(&metrics), TRUETYPE_FONTTYPE, parameter);
}

BYTE ExpandDibChannel(DWORD value, DWORD mask)
{
    if (!mask) return 0;
    unsigned shift = 0;
    while (((mask >> shift) & 1u) == 0u && shift < 31) ++shift;
    const DWORD maximum = mask >> shift;
    return maximum ? static_cast<BYTE>(((value & mask) >> shift) * 255ull / maximum) : 0;
}

bool DibMasks(const BITMAPINFO* information, DWORD* red, DWORD* green, DWORD* blue, DWORD* alpha)
{
    const auto& header = information->bmiHeader;
    if (header.biCompression == BI_RGB)
    {
        if (header.biBitCount == 16) { *red = 0x7c00; *green = 0x03e0; *blue = 0x001f; }
        else { *red = 0x00ff0000; *green = 0x0000ff00; *blue = 0x000000ff; }
        *alpha = header.biBitCount == 32 ? 0xff000000 : 0;
        return true;
    }
    if (header.biCompression != BI_BITFIELDS || (header.biBitCount != 16 && header.biBitCount != 32)) return false;
    const BYTE* base = reinterpret_cast<const BYTE*>(information);
    const DWORD* masks = reinterpret_cast<const DWORD*>(base + sizeof(BITMAPINFOHEADER));
    *red = masks[0]; *green = masks[1]; *blue = masks[2];
    *alpha = header.biSize >= 56 ? *reinterpret_cast<const DWORD*>(base + 52) : 0;
    return *red && *green && *blue;
}

const RGBQUAD* DibPalette(const BITMAPINFO* information)
{
    const auto& header = information->bmiHeader;
    size_t offset = header.biSize;
    if (header.biSize == sizeof(BITMAPINFOHEADER) && header.biCompression == BI_BITFIELDS)
        offset += 3 * sizeof(DWORD);
    return reinterpret_cast<const RGBQUAD*>(reinterpret_cast<const BYTE*>(information) + offset);
}

bool ReadDibPixel(const BITMAPINFO* information, const BYTE* row, int x, MiniGdi::Color* color)
{
    const auto& header = information->bmiHeader;
    if (!color || x < 0) return false;
    if (header.biBitCount <= 8)
    {
        unsigned index = header.biBitCount == 8 ? row[x] : header.biBitCount == 4
            ? ((row[x / 2] >> ((x & 1) ? 0 : 4)) & 0xfu)
            : ((row[x / 8] >> (7 - (x & 7))) & 1u);
        const unsigned count = header.biClrUsed ? header.biClrUsed : (1u << header.biBitCount);
        if (index >= count) return false;
        const RGBQUAD& entry = DibPalette(information)[index];
        *color = MiniGdi::MakeColor(entry.rgbRed, entry.rgbGreen, entry.rgbBlue, 255);
        return true;
    }
    if (header.biBitCount == 24)
    {
        const BYTE* pixel = row + static_cast<size_t>(x) * 3;
        *color = MiniGdi::MakeColor(pixel[2], pixel[1], pixel[0], 255); return true;
    }
    if (header.biBitCount == 16 || header.biBitCount == 32)
    {
        DWORD value = header.biBitCount == 16
            ? reinterpret_cast<const WORD*>(row)[x] : reinterpret_cast<const DWORD*>(row)[x];
        DWORD red = 0, green = 0, blue = 0, alpha = 0;
        if (!DibMasks(information, &red, &green, &blue, &alpha)) return false;
        const BYTE a = alpha ? ExpandDibChannel(value, alpha) : 255;
        *color = MiniGdi::MakeColor(ExpandDibChannel(value, red), ExpandDibChannel(value, green),
            ExpandDibChannel(value, blue), a);
        return true;
    }
    return false;
}

DWORD PackDibChannel(BYTE channel, DWORD mask)
{
    if (!mask) return 0;
    unsigned shift = 0;
    while (((mask >> shift) & 1u) == 0u && shift < 31) ++shift;
    const DWORD maximum = mask >> shift;
    return static_cast<DWORD>((static_cast<unsigned long long>(channel) * maximum + 127) / 255) << shift;
}

unsigned ClosestPaletteIndex(const BITMAPINFO* information, MiniGdi::Color color)
{
    const auto& header = information->bmiHeader;
    const unsigned count = header.biClrUsed ? header.biClrUsed : (1u << header.biBitCount);
    const RGBQUAD* palette = DibPalette(information);
    unsigned best = 0; unsigned long bestDistance = (std::numeric_limits<unsigned long>::max)();
    for (unsigned index = 0; index < count; ++index)
    {
        const long dr = static_cast<long>(MiniGdi::Red(color)) - palette[index].rgbRed;
        const long dg = static_cast<long>(MiniGdi::Green(color)) - palette[index].rgbGreen;
        const long db = static_cast<long>(MiniGdi::Blue(color)) - palette[index].rgbBlue;
        const unsigned long distance = static_cast<unsigned long>(dr * dr + dg * dg + db * db);
        if (distance < bestDistance) { bestDistance = distance; best = index; }
    }
    return best;
}

bool WriteDibPixel(BITMAPINFO* information, BYTE* row, int x, MiniGdi::Color color)
{
    const auto& header = information->bmiHeader;
    if (header.biBitCount <= 8)
    {
        const unsigned index = ClosestPaletteIndex(information, color);
        if (header.biBitCount == 8) row[x] = static_cast<BYTE>(index);
        else if (header.biBitCount == 4)
        {
            BYTE& value = row[x / 2];
            value = static_cast<BYTE>((x & 1) ? ((value & 0xf0) | index) : ((value & 0x0f) | (index << 4)));
        }
        else
        {
            BYTE& value = row[x / 8]; const BYTE bit = static_cast<BYTE>(1u << (7 - (x & 7)));
            value = index ? static_cast<BYTE>(value | bit) : static_cast<BYTE>(value & ~bit);
        }
        return true;
    }
    if (header.biBitCount == 24)
    {
        BYTE* pixel = row + static_cast<size_t>(x) * 3;
        pixel[0] = MiniGdi::Blue(color); pixel[1] = MiniGdi::Green(color); pixel[2] = MiniGdi::Red(color); return true;
    }
    if (header.biBitCount == 16 || header.biBitCount == 32)
    {
        DWORD red = 0, green = 0, blue = 0, alpha = 0;
        if (!DibMasks(information, &red, &green, &blue, &alpha)) return false;
        const DWORD value = PackDibChannel(MiniGdi::Red(color), red) |
            PackDibChannel(MiniGdi::Green(color), green) | PackDibChannel(MiniGdi::Blue(color), blue) |
            PackDibChannel(MiniGdi::Alpha(color), alpha);
        if (header.biBitCount == 16) reinterpret_cast<WORD*>(row)[x] = static_cast<WORD>(value);
        else reinterpret_cast<DWORD*>(row)[x] = value;
        return true;
    }
    return false;
}

bool CopyDibToSurface(const BITMAPINFO* information, const void* bits,
    MiniGdi::Surface* surface, UINT start, UINT lines)
{
    if (!information || !bits || !surface || information->bmiHeader.biSize < sizeof(BITMAPINFOHEADER)) return false;
    const auto& header = information->bmiHeader;
    const int width = header.biWidth;
    const int height = header.biHeight < 0 ? -header.biHeight : header.biHeight;
    if (width <= 0 || height <= 0 || header.biPlanes != 1 ||
        (header.biBitCount != 1 && header.biBitCount != 4 && header.biBitCount != 8 &&
         header.biBitCount != 16 && header.biBitCount != 24 && header.biBitCount != 32) ||
        (header.biCompression != BI_RGB && header.biCompression != BI_BITFIELDS)) return false;
    const size_t stride = ((static_cast<size_t>(width) * header.biBitCount + 31) / 32) * 4;
    const UINT copiedLines = (std::min)(lines, static_cast<UINT>((std::max)(0, height - static_cast<int>(start))));
    const BYTE* source = static_cast<const BYTE*>(bits);
    for (UINT row = 0; row < copiedLines; ++row)
    {
        const int logicalY = static_cast<int>(start + row);
        const int destinationY = header.biHeight < 0 ? logicalY : height - 1 - logicalY;
        for (int x = 0; x < width; ++x)
        {
            MiniGdi::Color* target = surface->PixelAt(x, destinationY);
            MiniGdi::Color color{};
            if (target && ReadDibPixel(information, source + static_cast<size_t>(row) * stride, x, &color))
                *target = color;
        }
    }
    return true;
}

HBITMAP WINAPI BridgeCreateDIBSection(HDC, const BITMAPINFO* information, UINT usage,
    void** bits, HANDLE section, DWORD offset)
{
    if (bits) *bits = nullptr;
    if (!information || !bits)
    {
        BridgeSetLastError(ERROR_INVALID_PARAMETER);
        return nullptr;
    }

    const BITMAPINFOHEADER& header = information->bmiHeader;
    if (header.biSize < sizeof(BITMAPINFOHEADER) || header.biWidth <= 0 ||
        header.biHeight == 0 || header.biHeight == (std::numeric_limits<LONG>::min)() ||
        header.biPlanes != 1 || usage != DIB_RGB_COLORS)
    {
        BridgeSetLastError(ERROR_INVALID_PARAMETER);
        return nullptr;
    }

    // MiniGdi surfaces deliberately use the native 32-bit BI_RGB byte layout
    // (BGRA in little-endian memory).  That lets a guest write through ppvBits
    // and lets the renderer consume the same stable allocation without a
    // shadow-buffer race.  Other formats require conversion on every guest
    // write, which cannot be observed through a raw DIB pointer, so do not
    // pretend they are compatible.
    if (header.biBitCount != 32 ||
        (header.biCompression != BI_RGB && header.biCompression != BI_BITFIELDS))
    {
        BridgeSetLastError(ERROR_NOT_SUPPORTED);
        return nullptr;
    }

    // Section-backed DIBs need mapped-section lifetime/offset semantics.  A
    // private DIB section is fully supported; rejecting the other form avoids
    // returning a pointer unrelated to the caller's mapping.
    if (section != nullptr || offset != 0)
    {
        BridgeSetLastError(ERROR_NOT_SUPPORTED);
        return nullptr;
    }

    const int width = header.biWidth;
    const int height = header.biHeight < 0 ? -header.biHeight : header.biHeight;
    const std::uint64_t pixelCount = static_cast<std::uint64_t>(width) *
        static_cast<std::uint64_t>(height);
    constexpr std::uint64_t MaximumDibBytes = 256ull * 1024ull * 1024ull;
    if (pixelCount > MaximumDibBytes / sizeof(MiniGdi::Color))
    {
        BridgeSetLastError(ERROR_NOT_ENOUGH_MEMORY);
        return nullptr;
    }

    HBITMAP bitmap = BridgeCreateBitmap(width, height, 1, 32, nullptr);
    GuestWindowManager* manager = CurrentManagerOrFail();
    MiniGdi::Surface* surface = manager && bitmap
        ? manager->Gdi().GetBitmapSurface(FromGuestObject(bitmap)) : nullptr;
    if (!surface)
    {
        if (bitmap) BridgeDeleteObject(bitmap);
        BridgeSetLastError(ERROR_NOT_ENOUGH_MEMORY);
        return nullptr;
    }
    *bits = surface->Data();
    BridgeSetLastError(ERROR_SUCCESS);
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
    if (header.biCompression != BI_RGB && header.biCompression != BI_BITFIELDS) return 0;
    if (header.biBitCount != 1 && header.biBitCount != 4 && header.biBitCount != 8 &&
        header.biBitCount != 16 && header.biBitCount != 24 && header.biBitCount != 32) return 0;
    if (header.biBitCount <= 8 && header.biClrUsed == 0)
    {
        const unsigned count = 1u << header.biBitCount;
        RGBQUAD* palette = const_cast<RGBQUAD*>(DibPalette(information));
        for (unsigned index = 0; index < count; ++index)
        {
            const BYTE value = static_cast<BYTE>(index * 255u / (count - 1u));
            palette[index] = RGBQUAD{ value, value, value, 0 };
        }
        header.biClrUsed = count;
    }
    header.biSizeImage = static_cast<DWORD>(((static_cast<size_t>(surface->Width()) *
        header.biBitCount + 31) / 32) * 4 * surface->Height());
    if (!bits) return static_cast<int>(lines);
    const size_t stride = ((static_cast<size_t>(surface->Width()) * header.biBitCount + 31) / 32) * 4;
    BYTE* destination = static_cast<BYTE*>(bits);
    const UINT copied = (std::min)(lines, static_cast<UINT>((std::max)(0, surface->Height() - static_cast<int>(start))));
    for (UINT row = 0; row < copied; ++row) for (int x = 0; x < surface->Width(); ++x)
    {
        const int sourceY = header.biHeight < 0 ? static_cast<int>(start + row)
            : surface->Height() - 1 - static_cast<int>(start + row);
        const MiniGdi::Color* color = surface->PixelAt(x, sourceY);
        BYTE* outputRow = destination + static_cast<size_t>(row) * stride;
        if (color && !WriteDibPixel(information, outputRow, x, *color)) return 0;
    }
    return static_cast<int>(copied);
}

int WINAPI BridgeStretchDIBits(HDC destination, int x, int y, int width, int height,
    int sourceX, int sourceY, int sourceWidth, int sourceHeight, const void* bits,
    const BITMAPINFO* information, UINT, DWORD operation)
{
    if (!information || !bits) return GDI_ERROR;
    const int bitmapHeight = information->bmiHeader.biHeight < 0
        ? -information->bmiHeader.biHeight : information->bmiHeader.biHeight;
    MiniGdi::Surface source(information->bmiHeader.biWidth, bitmapHeight, MiniGdi::Transparent);
    if (!CopyDibToSurface(information, bits, &source, 0, static_cast<UINT>(bitmapHeight))) return GDI_ERROR;
    GuestWindowManager* manager = CurrentManagerOrFail();
    MiniGdi::Surface* output = manager ? manager->Gdi().GetSurface(FromGuestDc(destination)) : nullptr;
    if (!output) return GDI_ERROR;
    const auto destinationState = DcState(destination);
    const POINT destinationFirst = LogicalToDevicePoint(destinationState, x, y);
    const POINT destinationSecond = LogicalToDevicePoint(
        destinationState, x + width, y + height);
    const bool mirrorX = (destinationSecond.x < destinationFirst.x) != (sourceWidth < 0);
    const bool mirrorY = (destinationSecond.y < destinationFirst.y) != (sourceHeight < 0);
    x = (std::min)(destinationFirst.x, destinationSecond.x);
    y = (std::min)(destinationFirst.y, destinationSecond.y);
    width = (std::abs)(destinationSecond.x - destinationFirst.x);
    height = (std::abs)(destinationSecond.y - destinationFirst.y);
    if (width == 0 || height == 0) return 0;
    MiniGdi::Region clip;
    manager->Gdi().GetClipRegion(FromGuestDc(destination), &clip);
    const MiniGdi::Color pattern = SelectedPatternColor(manager, destination);
    const int sourceBaseX = sourceWidth < 0 ? sourceX + sourceWidth : sourceX;
    const int sourceBaseY = sourceHeight < 0 ? sourceY + sourceHeight : sourceY;
    for (int dy = 0; dy < height; ++dy) for (int dx = 0; dx < width; ++dx)
    {
        const int tx = x + dx, ty = y + dy;
        if (!clip.Contains({ tx, ty })) continue;
        const int sampleColumn = mirrorX ? width - 1 - dx : dx;
        const int sampleRow = mirrorY ? height - 1 - dy : dy;
        const MiniGdi::Color* pixel = source.PixelAt(sourceBaseX + sampleColumn * (std::abs)(sourceWidth) / width,
            sourceBaseY + sampleRow * (std::abs)(sourceHeight) / height);
        MiniGdi::Color* target = output->PixelAt(tx, ty);
        if (!target) continue;
        const MiniGdi::Color sourcePixel = pixel ? *pixel : MiniGdi::Transparent;
        *target = ApplyRasterOperation(operation, sourcePixel, *target, pattern);
    }
    BridgeSetLastError(ERROR_SUCCESS);
    return height;
}

int WINAPI BridgeSetDIBitsToDevice(HDC destination, int x, int y, DWORD width,
    DWORD height, int sourceX, int sourceY, UINT startScan, UINT scanLines,
    const void* bits, const BITMAPINFO* information, UINT colorUse)
{
    if (!information || !bits || colorUse != DIB_RGB_COLORS ||
        width > static_cast<DWORD>((std::numeric_limits<int>::max)()) ||
        height > static_cast<DWORD>((std::numeric_limits<int>::max)()))
    {
        BridgeSetLastError(ERROR_INVALID_PARAMETER);
        return 0;
    }
    const int bitmapHeight = information->bmiHeader.biHeight < 0
        ? -information->bmiHeader.biHeight : information->bmiHeader.biHeight;
    MiniGdi::Surface source(information->bmiHeader.biWidth, bitmapHeight,
        MiniGdi::Transparent);
    if (!CopyDibToSurface(information, bits, &source, startScan, scanLines))
        return 0;
    GuestWindowManager* manager = CurrentManagerOrFail();
    MiniGdi::Surface* output = manager
        ? manager->Gdi().GetSurface(FromGuestDc(destination)) : nullptr;
    if (!output) { BridgeSetLastError(ERROR_INVALID_HANDLE); return 0; }
    const auto state = DcState(destination);
    const POINT first = LogicalToDevicePoint(state, x, y);
    const POINT second = LogicalToDevicePoint(state,
        SaturateToLong(static_cast<long long>(x) + width),
        SaturateToLong(static_cast<long long>(y) + height));
    const int destinationX = (std::min)(first.x, second.x);
    const int destinationY = (std::min)(first.y, second.y);
    const int destinationWidth = (std::abs)(second.x - first.x);
    const int destinationHeight = (std::abs)(second.y - first.y);
    MiniGdi::Region clip;
    manager->Gdi().GetClipRegion(FromGuestDc(destination), &clip);
    const int availableLines = (std::min)(destinationHeight,
        static_cast<int>(scanLines));
    for (int row = 0; row < availableLines; ++row)
    {
        for (int column = 0; column < destinationWidth; ++column)
        {
            const int targetX = destinationX + column;
            const int targetY = destinationY + row;
            if (!clip.Contains({ targetX, targetY })) continue;
            const MiniGdi::Color* sourcePixel = source.PixelAt(
                sourceX + column, sourceY + static_cast<int>(startScan) + row);
            MiniGdi::Color* target = output->PixelAt(targetX, targetY);
            if (sourcePixel && target) *target = *sourcePixel;
        }
    }
    BridgeSetLastError(ERROR_SUCCESS);
    return availableLines;
}

int WINAPI BridgeStartPage(HDC) { BridgeSetLastError(ERROR_CALL_NOT_IMPLEMENTED); return SP_ERROR; }
int WINAPI BridgeStartDocW(HDC, const void*) { BridgeSetLastError(ERROR_CALL_NOT_IMPLEMENTED); return SP_ERROR; }
int WINAPI BridgeSetAbortProc(HDC, GuestAbortProc) { BridgeSetLastError(ERROR_CALL_NOT_IMPLEMENTED); return SP_ERROR; }
int WINAPI BridgeEndDoc(HDC) { BridgeSetLastError(ERROR_CALL_NOT_IMPLEMENTED); return SP_ERROR; }
int WINAPI BridgeAbortDoc(HDC) { BridgeSetLastError(ERROR_CALL_NOT_IMPLEMENTED); return SP_ERROR; }
int WINAPI BridgeEndPage(HDC) { BridgeSetLastError(ERROR_CALL_NOT_IMPLEMENTED); return SP_ERROR; }

BOOL WINAPI BridgeLPtoDP(HDC dc, LPPOINT points, int count)
{
    if (!points || count < 0) { BridgeSetLastError(ERROR_INVALID_PARAMETER); return FALSE; }
    GuestWindowManager* manager = CurrentManagerOrFail();
    if (!manager || !manager->Gdi().HasDc(FromGuestDc(dc))) return FALSE;
    const auto state = DcState(dc);
    for (int index = 0; index < count; ++index)
        points[index] = LogicalToDevicePoint(state, points[index].x, points[index].y);
    return TRUE;
}

BOOL WINAPI BridgeSetWindowExtEx(HDC dc, int x, int y, LPSIZE previous)
{
    GuestWindowManager* manager = CurrentManagerOrFail();
    if (!manager || !manager->Gdi().HasDc(FromGuestDc(dc))) return FALSE;
    std::lock_guard<std::mutex> guard(g_gdiCompatibilityLock);
    auto& state = g_dcCompatibility[reinterpret_cast<ULONG_PTR>(dc)];
    if (previous) *previous = state.windowExtent;
    if (x == 0 || y == 0) { BridgeSetLastError(ERROR_INVALID_PARAMETER); return FALSE; }
    if (state.mapMode == MM_ISOTROPIC || state.mapMode == MM_ANISOTROPIC)
        state.windowExtent = { x, y };
    return TRUE;
}

BOOL WINAPI BridgeGetWindowExtEx(HDC dc, LPSIZE extent)
{
    if (!extent) { BridgeSetLastError(ERROR_INVALID_PARAMETER); return FALSE; }
    GuestWindowManager* manager = CurrentManagerOrFail();
    if (!manager || !manager->Gdi().HasDc(FromGuestDc(dc))) return FALSE;
    *extent = DcState(dc).windowExtent;
    return TRUE;
}

BOOL WINAPI BridgeSetViewportExtEx(HDC dc, int x, int y, LPSIZE previous)
{
    GuestWindowManager* manager = CurrentManagerOrFail();
    if (!manager || !manager->Gdi().HasDc(FromGuestDc(dc))) return FALSE;
    std::lock_guard<std::mutex> guard(g_gdiCompatibilityLock);
    auto& state = g_dcCompatibility[reinterpret_cast<ULONG_PTR>(dc)];
    if (previous) *previous = state.viewportExtent;
    if (x == 0 || y == 0) { BridgeSetLastError(ERROR_INVALID_PARAMETER); return FALSE; }
    if (state.mapMode == MM_ISOTROPIC || state.mapMode == MM_ANISOTROPIC)
        state.viewportExtent = { x, y };
    return TRUE;
}

BOOL WINAPI BridgeGetViewportExtEx(HDC dc, LPSIZE extent)
{
    if (!extent) { BridgeSetLastError(ERROR_INVALID_PARAMETER); return FALSE; }
    GuestWindowManager* manager = CurrentManagerOrFail();
    if (!manager || !manager->Gdi().HasDc(FromGuestDc(dc))) return FALSE;
    *extent = DcState(dc).viewportExtent;
    return TRUE;
}

int WINAPI BridgeSetMapMode(HDC dc, int mode)
{
    if (mode < MM_TEXT || mode > MM_ANISOTROPIC)
    {
        BridgeSetLastError(ERROR_INVALID_PARAMETER);
        return 0;
    }
    GuestWindowManager* manager = CurrentManagerOrFail();
    if (!manager || !manager->Gdi().HasDc(FromGuestDc(dc))) return 0;
    std::lock_guard<std::mutex> guard(g_gdiCompatibilityLock);
    auto& state = g_dcCompatibility[reinterpret_cast<ULONG_PTR>(dc)];
    const int previous = state.mapMode;
    state.mapMode = mode;
    if (mode <= MM_TWIPS)
        SetFixedMapModeExtents(&state, mode);
    return previous;
}

int WINAPI BridgeGetMapMode(HDC dc)
{
    GuestWindowManager* manager = CurrentManagerOrFail();
    if (!manager || !manager->Gdi().HasDc(FromGuestDc(dc))) return 0;
    return DcState(dc).mapMode;
}

int WINAPI BridgeSetGraphicsMode(HDC dc, int mode)
{
    if (mode != GM_COMPATIBLE && mode != GM_ADVANCED)
    { BridgeSetLastError(ERROR_INVALID_PARAMETER); return 0; }
    GuestWindowManager* manager = CurrentManagerOrFail();
    if (!manager || !manager->Gdi().HasDc(FromGuestDc(dc))) return 0;
    std::lock_guard<std::mutex> guard(g_gdiCompatibilityLock);
    auto& state = g_dcCompatibility[reinterpret_cast<ULONG_PTR>(dc)];
    if (mode == GM_COMPATIBLE && (state.worldTransform.eM11 != 1.0f ||
        state.worldTransform.eM12 != 0.0f || state.worldTransform.eM21 != 0.0f ||
        state.worldTransform.eM22 != 1.0f || state.worldTransform.eDx != 0.0f ||
        state.worldTransform.eDy != 0.0f))
    { BridgeSetLastError(ERROR_INVALID_PARAMETER); return 0; }
    const int previous = state.graphicsMode;
    state.graphicsMode = mode;
    return previous;
}

int WINAPI BridgeGetGraphicsMode(HDC dc)
{
    GuestWindowManager* manager = CurrentManagerOrFail();
    if (!manager || !manager->Gdi().HasDc(FromGuestDc(dc))) return 0;
    return DcState(dc).graphicsMode;
}

BOOL WINAPI BridgeGetWorldTransform(HDC dc, XFORM* transform)
{
    if (!transform) { BridgeSetLastError(ERROR_INVALID_PARAMETER); return FALSE; }
    GuestWindowManager* manager = CurrentManagerOrFail();
    if (!manager || !manager->Gdi().HasDc(FromGuestDc(dc))) return FALSE;
    *transform = DcState(dc).worldTransform;
    return TRUE;
}

BOOL WINAPI BridgeSetWorldTransform(HDC dc, const XFORM* transform)
{
    if (!transform) { BridgeSetLastError(ERROR_INVALID_PARAMETER); return FALSE; }
    GuestWindowManager* manager = CurrentManagerOrFail();
    if (!manager || !manager->Gdi().HasDc(FromGuestDc(dc))) return FALSE;
    const double determinant = static_cast<double>(transform->eM11) * transform->eM22 -
        static_cast<double>(transform->eM12) * transform->eM21;
    if (!std::isfinite(determinant) || !std::isfinite(transform->eDx) ||
        !std::isfinite(transform->eDy) || std::abs(determinant) <= 1.0e-12)
    { BridgeSetLastError(ERROR_INVALID_PARAMETER); return FALSE; }
    std::lock_guard<std::mutex> guard(g_gdiCompatibilityLock);
    auto& state = g_dcCompatibility[reinterpret_cast<ULONG_PTR>(dc)];
    if (state.graphicsMode != GM_ADVANCED)
    { BridgeSetLastError(ERROR_INVALID_PARAMETER); return FALSE; }
    state.worldTransform = *transform;
    return TRUE;
}

XFORM MultiplyTransforms(const XFORM& left, const XFORM& right)
{
    XFORM result{};
    result.eM11 = left.eM11 * right.eM11 + left.eM12 * right.eM21;
    result.eM12 = left.eM11 * right.eM12 + left.eM12 * right.eM22;
    result.eM21 = left.eM21 * right.eM11 + left.eM22 * right.eM21;
    result.eM22 = left.eM21 * right.eM12 + left.eM22 * right.eM22;
    result.eDx = left.eDx * right.eM11 + left.eDy * right.eM21 + right.eDx;
    result.eDy = left.eDx * right.eM12 + left.eDy * right.eM22 + right.eDy;
    return result;
}

BOOL WINAPI BridgeCombineTransform(XFORM* result, const XFORM* first, const XFORM* second)
{
    if (!result || !first || !second)
    { BridgeSetLastError(ERROR_INVALID_PARAMETER); return FALSE; }
    const XFORM combined = MultiplyTransforms(*first, *second);
    const double determinant = static_cast<double>(combined.eM11) * combined.eM22 -
        static_cast<double>(combined.eM12) * combined.eM21;
    if (!std::isfinite(determinant) || !std::isfinite(combined.eDx) ||
        !std::isfinite(combined.eDy) || std::abs(determinant) <= 1.0e-12)
    { BridgeSetLastError(ERROR_INVALID_PARAMETER); return FALSE; }
    *result = combined;
    return TRUE;
}

BOOL WINAPI BridgeModifyWorldTransform(HDC dc, const XFORM* transform, DWORD mode)
{
    constexpr DWORD Identity = 1, LeftMultiply = 2, RightMultiply = 3;
    GuestWindowManager* manager = CurrentManagerOrFail();
    if (!manager || !manager->Gdi().HasDc(FromGuestDc(dc))) return FALSE;
    std::lock_guard<std::mutex> guard(g_gdiCompatibilityLock);
    auto& state = g_dcCompatibility[reinterpret_cast<ULONG_PTR>(dc)];
    if (state.graphicsMode != GM_ADVANCED)
    { BridgeSetLastError(ERROR_INVALID_PARAMETER); return FALSE; }
    const XFORM identity{ 1.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f };
    XFORM candidate = state.worldTransform;
    if (mode == Identity) candidate = identity;
    else if (!transform) { BridgeSetLastError(ERROR_INVALID_PARAMETER); return FALSE; }
    else if (mode == LeftMultiply)
        candidate = MultiplyTransforms(*transform, state.worldTransform);
    else if (mode == RightMultiply)
        candidate = MultiplyTransforms(state.worldTransform, *transform);
    else { BridgeSetLastError(ERROR_INVALID_PARAMETER); return FALSE; }
    const double determinant = static_cast<double>(candidate.eM11) * candidate.eM22 -
        static_cast<double>(candidate.eM12) * candidate.eM21;
    if (!std::isfinite(determinant) || !std::isfinite(candidate.eDx) ||
        !std::isfinite(candidate.eDy) || std::abs(determinant) <= 1.0e-12)
    { BridgeSetLastError(ERROR_INVALID_PARAMETER); return FALSE; }
    state.worldTransform = candidate;
    return TRUE;
}

int WINAPI BridgeEnumFontsW(
    HDC dc, LPCWSTR faceName, GuestFontEnumProcW callback, LPARAM parameter)
{
    LOGFONTW font = {};
    wcscpy_s(font.lfFaceName, faceName && *faceName ? faceName : L"Segoe UI");
    font.lfCharSet = DEFAULT_CHARSET;
    return BridgeEnumFontFamiliesExW(dc, &font, callback, parameter, 0);
}

DWORD WINAPI BridgeGetGlyphIndicesW(HDC dc, LPCWSTR text, int count, LPWORD glyphs, DWORD)
{
    if (count < 0 || (!text && count) || (!glyphs && count)) return GDI_ERROR;
    GuestWindowManager* manager = CurrentManagerOrFail(); MiniGdi::Font font{};
    if (!manager || !manager->Gdi().GetSelectedFont(FromGuestDc(dc), &font) ||
        !DirectWriteText::MapGlyphs(font, text, static_cast<size_t>(count), glyphs)) return GDI_ERROR;
    return static_cast<DWORD>(count);
}

BOOL WINAPI BridgeGetCharABCWidthsW(HDC dc, UINT first, UINT last, LPABC widths)
{
    if (!widths || last < first || static_cast<unsigned long long>(last) - first + 1 > 65536) return FALSE;
    GuestWindowManager* manager = CurrentManagerOrFail(); MiniGdi::Font font{};
    if (!manager || !manager->Gdi().GetSelectedFont(FromGuestDc(dc), &font)) return FALSE;
    const size_t count = static_cast<size_t>(last - first) + 1;
    std::vector<wchar_t> characters(count); std::vector<WORD> glyphs(count);
    std::vector<DirectWriteText::GlyphMetrics> metrics(count);
    for (size_t index = 0; index < count; ++index) characters[index] = static_cast<wchar_t>(first + index);
    if (!DirectWriteText::MapGlyphs(font, characters.data(), count, glyphs.data()) ||
        !DirectWriteText::GetGlyphMetrics(font, glyphs.data(), count, metrics.data())) return FALSE;
    for (size_t index = 0; index < count; ++index)
    {
        widths[index].abcA = metrics[index].leftSideBearing;
        widths[index].abcB = static_cast<UINT>((std::max)(0, metrics[index].advanceWidth -
            metrics[index].leftSideBearing - metrics[index].rightSideBearing));
        widths[index].abcC = metrics[index].rightSideBearing;
    }
    return TRUE;
}

BOOL WINAPI BridgeGetCharWidth32W(HDC dc, UINT first, UINT last, LPINT widths)
{
    if (!widths || last < first) return FALSE;
    const size_t count = static_cast<size_t>(last - first) + 1; std::vector<ABC> abc(count);
    if (!BridgeGetCharABCWidthsW(dc, first, last, abc.data())) return FALSE;
    for (size_t index = 0; index < count; ++index)
        widths[index] = abc[index].abcA + static_cast<int>(abc[index].abcB) + abc[index].abcC;
    return TRUE;
}

DWORD WINAPI BridgeGetGlyphOutlineW(HDC dc, UINT character, UINT format,
    LPGLYPHMETRICS output, DWORD bufferSize, LPVOID buffer, const MAT2*)
{
    constexpr UINT GlyphIndexFlag = 0x0080u;
    if (!output) return GDI_ERROR;
    GuestWindowManager* manager = CurrentManagerOrFail(); MiniGdi::Font font{};
    if (!manager || !manager->Gdi().GetSelectedFont(FromGuestDc(dc), &font)) return GDI_ERROR;
    WORD glyph = static_cast<WORD>(character);
    if ((format & GlyphIndexFlag) == 0)
    {
        const wchar_t value = static_cast<wchar_t>(character);
        if (!DirectWriteText::MapGlyphs(font, &value, 1, &glyph)) return GDI_ERROR;
    }
    DirectWriteText::GlyphMetrics metric{};
    if (!DirectWriteText::GetGlyphMetrics(font, &glyph, 1, &metric)) return GDI_ERROR;
    output->gmBlackBoxX = static_cast<UINT>((std::max)(0, metric.advanceWidth - metric.leftSideBearing - metric.rightSideBearing));
    output->gmBlackBoxY = static_cast<UINT>((std::max)(0, metric.advanceHeight - metric.topSideBearing - metric.bottomSideBearing));
    output->gmptGlyphOrigin = { metric.leftSideBearing, metric.verticalOriginY - metric.topSideBearing };
    output->gmCellIncX = static_cast<short>(metric.advanceWidth); output->gmCellIncY = 0;
    if ((format & 0x7fu) == GGO_METRICS) return 0;
    UNREFERENCED_PARAMETER(bufferSize); UNREFERENCED_PARAMETER(buffer);
    BridgeSetLastError(ERROR_NOT_SUPPORTED); return GDI_ERROR;
}

DWORD WINAPI BridgeGetCharacterPlacementW(HDC dc, LPCWSTR text, int count,
    int maximumExtent, LPGCP_RESULTSW results, DWORD)
{
    if (!results || results->lStructSize < sizeof(GCP_RESULTSW) || count < 0 || (!text && count))
        return 0;
    GuestWindowManager* manager = CurrentManagerOrFail(); MiniGdi::Font font{};
    if (!manager || !manager->Gdi().GetSelectedFont(FromGuestDc(dc), &font)) return 0;
    const size_t capacity = (std::min)(static_cast<size_t>(count), static_cast<size_t>(results->nGlyphs));
    std::vector<WORD> glyphs(static_cast<size_t>(count));
    std::vector<DirectWriteText::GlyphMetrics> metrics(static_cast<size_t>(count));
    if (!DirectWriteText::MapGlyphs(font, text, static_cast<size_t>(count), glyphs.data()) ||
        !DirectWriteText::GetGlyphMetrics(font, glyphs.data(), static_cast<size_t>(count), metrics.data())) return 0;
    int x = 0; size_t fit = 0;
    for (size_t index = 0; index < capacity; ++index)
    {
        if (results->lpOutString) results->lpOutString[index] = text[index];
        if (results->lpOrder) results->lpOrder[index] = static_cast<UINT>(index);
        if (results->lpDx) results->lpDx[index] = metrics[index].advanceWidth;
        if (results->lpCaretPos) results->lpCaretPos[index] = x;
        if (results->lpClass) results->lpClass[index] = 0;
        if (results->lpGlyphs) results->lpGlyphs[index] = glyphs[index];
        x += metrics[index].advanceWidth;
        if (maximumExtent <= 0 || x <= maximumExtent) fit = index + 1;
    }
    results->nGlyphs = static_cast<UINT>(capacity); results->nMaxFit = static_cast<int>(fit);
    MiniGdi::FontMetrics fontMetrics{}; manager->Gdi().GetSelectedFontMetrics(FromGuestDc(dc), &fontMetrics);
    return MAKELONG((std::min)(x, 0xffff), (std::min)(fontMetrics.height, 0xffff));
}

HPALETTE WINAPI BridgeCreatePalette(const LOGPALETTE* palette)
{
    if (!palette || !palette->palNumEntries) return nullptr;
    GuestPalette value; value.entries.assign(palette->palPalEntry,
        palette->palPalEntry + palette->palNumEntries);
    std::lock_guard<std::mutex> guard(g_gdiCompatibilityLock);
    ULONG_PTR token = ++g_nextPalette; if (!token) token = ++g_nextPalette;
    g_palettes.emplace(token, std::move(value));
    return reinterpret_cast<HPALETTE>(token);
}

bool BridgeDeletePalette(HGDIOBJ palette)
{
    std::lock_guard<std::mutex> guard(g_gdiCompatibilityLock);
    return g_palettes.erase(reinterpret_cast<ULONG_PTR>(palette)) != 0;
}

HPALETTE WINAPI BridgeSelectPalette(HDC dc, HPALETTE palette, BOOL)
{
    std::lock_guard<std::mutex> guard(g_gdiCompatibilityLock);
    if (palette && g_palettes.find(reinterpret_cast<ULONG_PTR>(palette)) == g_palettes.end()) return nullptr;
    auto& state = g_dcCompatibility[reinterpret_cast<ULONG_PTR>(dc)];
    const HPALETTE previous = state.selectedPalette; state.selectedPalette = palette; return previous;
}

UINT WINAPI BridgeRealizePalette(HDC dc)
{
    std::lock_guard<std::mutex> guard(g_gdiCompatibilityLock);
    const auto selected = g_dcCompatibility[reinterpret_cast<ULONG_PTR>(dc)].selectedPalette;
    const auto found = g_palettes.find(reinterpret_cast<ULONG_PTR>(selected));
    return found == g_palettes.end() ? 0 : static_cast<UINT>(found->second.entries.size());
}

UINT WINAPI BridgeGetPaletteEntries(HPALETTE palette, UINT start, UINT count, LPPALETTEENTRY entries)
{
    std::lock_guard<std::mutex> guard(g_gdiCompatibilityLock);
    const auto found = g_palettes.find(reinterpret_cast<ULONG_PTR>(palette));
    if (found == g_palettes.end() || start >= found->second.entries.size()) return 0;
    const UINT copied = (std::min)(count, static_cast<UINT>(found->second.entries.size() - start));
    if (entries && copied) memcpy(entries, found->second.entries.data() + start, copied * sizeof(PALETTEENTRY));
    return copied;
}

UINT WINAPI BridgeSetPaletteEntries(HPALETTE palette, UINT start, UINT count, const PALETTEENTRY* entries)
{
    if (!entries && count) return 0; std::lock_guard<std::mutex> guard(g_gdiCompatibilityLock);
    auto found = g_palettes.find(reinterpret_cast<ULONG_PTR>(palette));
    if (found == g_palettes.end() || start >= found->second.entries.size()) return 0;
    const UINT copied = (std::min)(count, static_cast<UINT>(found->second.entries.size() - start));
    if (copied) memcpy(found->second.entries.data() + start, entries, copied * sizeof(PALETTEENTRY));
    return copied;
}

BOOL WINAPI BridgeResizePalette(HPALETTE palette, UINT count)
{
    std::lock_guard<std::mutex> guard(g_gdiCompatibilityLock);
    auto found = g_palettes.find(reinterpret_cast<ULONG_PTR>(palette)); if (found == g_palettes.end()) return FALSE;
    try { found->second.entries.resize(count); } catch (...) { return FALSE; } return TRUE;
}

UINT WINAPI BridgeGetNearestPaletteIndex(HPALETTE palette, COLORREF color)
{
    std::lock_guard<std::mutex> guard(g_gdiCompatibilityLock);
    const auto found = g_palettes.find(reinterpret_cast<ULONG_PTR>(palette));
    if (found == g_palettes.end() || found->second.entries.empty()) return CLR_INVALID;
    UINT best = 0; unsigned long bestDistance = (std::numeric_limits<unsigned long>::max)();
    for (UINT index = 0; index < found->second.entries.size(); ++index)
    {
        const auto& entry = found->second.entries[index];
        const long dr = GetRValue(color) - entry.peRed, dg = GetGValue(color) - entry.peGreen, db = GetBValue(color) - entry.peBlue;
        const unsigned long distance = static_cast<unsigned long>(dr*dr + dg*dg + db*db);
        if (distance < bestDistance) { bestDistance = distance; best = index; }
    }
    return best;
}

BOOL WINAPI BridgeUpdateColors(HDC dc) { return BridgeRealizePalette(dc) || !DcState(dc).selectedPalette; }
int WINAPI BridgeSetICMMode(HDC dc, int mode)
{
    if (mode != 1 && mode != 2 && mode != 3) return 0;
    std::lock_guard<std::mutex> guard(g_gdiCompatibilityLock);
    auto& state = g_dcCompatibility[reinterpret_cast<ULONG_PTR>(dc)]; const int previous = state.icmMode;
    if (mode != 3) state.icmMode = mode; return mode == 3 ? previous : previous;
}
BOOL WINAPI BridgeSetICMProfileW(HDC, LPWSTR) { BridgeSetLastError(ERROR_NOT_SUPPORTED); return FALSE; }
BOOL WINAPI BridgeGetICMProfileW(HDC, LPDWORD size, LPWSTR name)
{
    constexpr wchar_t Profile[] = L"sRGB Color Space Profile.icm";
    const DWORD required = static_cast<DWORD>(_countof(Profile));
    if (!size) return FALSE; if (!name || *size < required) { *size = required; BridgeSetLastError(ERROR_INSUFFICIENT_BUFFER); return FALSE; }
    memcpy(name, Profile, sizeof(Profile)); *size = required; return TRUE;
}

HDC WINAPI BridgeCreateEnhMetaFileW(HDC reference, LPCWSTR, const RECT* bounds, LPCWSTR)
{
    RECT frame = bounds ? *bounds : RECT{ 0, 0, 1024, 768 };
    const int width = (std::max)(1L, std::abs(frame.right - frame.left));
    const int height = (std::max)(1L, std::abs(frame.bottom - frame.top));
    HDC dc = BridgeCreateCompatibleDC(reference); if (!dc) return nullptr;
    HBITMAP bitmap = BridgeCreateBitmap(width, height, 1, 32, nullptr);
    if (!bitmap) { BridgeDeleteDC(dc); return nullptr; }
    BridgeSelectObject(dc, bitmap);
    std::lock_guard<std::mutex> guard(g_gdiCompatibilityLock);
    g_metafileRecordings[reinterpret_cast<ULONG_PTR>(dc)] = { bitmap, frame }; return dc;
}

HENHMETAFILE WINAPI BridgeCloseEnhMetaFile(HDC dc)
{
    GuestMetafileRecording recording{};
    {
        std::lock_guard<std::mutex> guard(g_gdiCompatibilityLock);
        const auto found = g_metafileRecordings.find(reinterpret_cast<ULONG_PTR>(dc));
        if (found == g_metafileRecordings.end()) return nullptr;
        recording = found->second; g_metafileRecordings.erase(found);
    }
    GuestWindowManager* manager = CurrentManagerOrFail();
    const MiniGdi::Surface* surface = manager ? manager->Gdi().GetBitmapSurface(FromGuestObject(recording.bitmap)) : nullptr;
    if (!surface) return nullptr;
    GuestMetafile metafile{ *surface, recording.bounds };
    BridgeDeleteDC(dc); BridgeDeleteObject(recording.bitmap);
    std::lock_guard<std::mutex> guard(g_gdiCompatibilityLock);
    ULONG_PTR token = ++g_nextMetafile; if (!token) token = ++g_nextMetafile;
    g_metafiles.emplace(token, std::move(metafile)); return reinterpret_cast<HENHMETAFILE>(token);
}

BOOL WINAPI BridgeDeleteEnhMetaFile(HENHMETAFILE metafile)
{
    std::lock_guard<std::mutex> guard(g_gdiCompatibilityLock);
    return g_metafiles.erase(reinterpret_cast<ULONG_PTR>(metafile)) ? TRUE : FALSE;
}

BOOL WINAPI BridgePlayEnhMetaFile(HDC dc, HENHMETAFILE metafile, const RECT* bounds)
{
    if (!bounds) return FALSE; GuestWindowManager* manager = CurrentManagerOrFail(); if (!manager) return FALSE;
    GuestMetafile copy;
    { std::lock_guard<std::mutex> guard(g_gdiCompatibilityLock); const auto found = g_metafiles.find(reinterpret_cast<ULONG_PTR>(metafile)); if (found == g_metafiles.end()) return FALSE; copy = found->second; }
    MiniGdi::Surface* target = manager->Gdi().GetSurface(FromGuestDc(dc)); if (!target) return FALSE;
    MiniGdi::Region clip; manager->Gdi().GetClipRegion(FromGuestDc(dc), &clip);
    const int width = (std::max)(0L, bounds->right - bounds->left), height = (std::max)(0L, bounds->bottom - bounds->top);
    for (int y = 0; y < height; ++y) for (int x = 0; x < width; ++x)
    {
        const int tx = bounds->left + x, ty = bounds->top + y; if (!clip.Contains({ tx, ty })) continue;
        const auto* source = copy.surface.PixelAt(x * copy.surface.Width() / (std::max)(1, width), y * copy.surface.Height() / (std::max)(1, height));
        auto* destination = target->PixelAt(tx, ty); if (source && destination) *destination = *source;
    }
    return TRUE;
}

UINT WINAPI BridgeGetEnhMetaFileHeader(HENHMETAFILE metafile, UINT size, LPENHMETAHEADER header)
{
    std::lock_guard<std::mutex> guard(g_gdiCompatibilityLock); const auto found = g_metafiles.find(reinterpret_cast<ULONG_PTR>(metafile));
    if (found == g_metafiles.end()) return 0; if (!header || size < sizeof(ENHMETAHEADER)) return sizeof(ENHMETAHEADER);
    ZeroMemory(header, sizeof(*header)); header->iType = EMR_HEADER; header->nSize = sizeof(*header);
    header->rclBounds = found->second.bounds; header->dSignature = ENHMETA_SIGNATURE; header->nVersion = 0x10000;
    return sizeof(ENHMETAHEADER);
}

HDC WINAPI BridgeCreateDCW(LPCWSTR, LPCWSTR, LPCWSTR, const void*)
{
    BridgeSetLastError(ERROR_CALL_NOT_IMPLEMENTED);
    return nullptr;
}
}
