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

GuestDcCompatibilityState DcState(HDC dc)
{
    std::lock_guard<std::mutex> guard(g_gdiCompatibilityLock);
    return g_dcCompatibility[reinterpret_cast<ULONG_PTR>(dc)];
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
    return POINT{
        state.viewportOrigin.x + ScaleCoordinate(
            x - state.windowOrigin.x, state.viewportExtent.cx, state.windowExtent.cx),
        state.viewportOrigin.y + ScaleCoordinate(
            y - state.windowOrigin.y, state.viewportExtent.cy, state.windowExtent.cy)
    };
}

POINT DeviceToLogicalPoint(const GuestDcCompatibilityState& state, LONG x, LONG y)
{
    return POINT{
        state.windowOrigin.x + ScaleCoordinate(
            x - state.viewportOrigin.x, state.windowExtent.cx, state.viewportExtent.cx),
        state.windowOrigin.y + ScaleCoordinate(
            y - state.viewportOrigin.y, state.windowExtent.cy, state.viewportExtent.cy)
    };
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
    if (operation == BLACKNESS || operation == WHITENESS)
    {
        const MiniGdi::ObjectHandle brush = manager->Gdi().CreateSolidBrush(
            operation == BLACKNESS ? MiniGdi::OpaqueBlack : MiniGdi::OpaqueWhite);
        if (brush == MiniGdi::InvalidObject) return FALSE;
        MiniGdi::ObjectHandle previous = MiniGdi::InvalidObject;
        const bool selected = manager->Gdi().SelectBrush(FromGuestDc(dc), brush, &previous);
        const bool painted = selected && manager->Gdi().FillRect(FromGuestDc(dc), bounds);
        if (selected && previous != MiniGdi::InvalidObject)
            manager->Gdi().SelectBrush(FromGuestDc(dc), previous, nullptr);
        manager->Gdi().DeleteObject(brush);
        return painted ? TRUE : FALSE;
    }
    return manager->Gdi().FillRect(FromGuestDc(dc), bounds);
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

BOOL WINAPI BridgeStretchBlt(HDC destination, int x, int y, int width, int height,
    HDC source, int sourceX, int sourceY, int sourceWidth, int sourceHeight, DWORD operation)
{
    if (width <= 0 || height <= 0 || sourceWidth <= 0 || sourceHeight <= 0) return FALSE;
    switch (operation)
    {
    case SRCCOPY:
    case SRCAND:
    case SRCPAINT:
    case SRCINVERT:
    case NOTSRCCOPY:
    case BLACKNESS:
    case WHITENESS:
        break;
    default:
        BridgeSetLastError(ERROR_NOT_SUPPORTED);
        return FALSE;
    }
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
    // Sampling first is required when source and destination refer to the same
    // surface and their rectangles overlap.  GDI blits behave as if the source
    // rectangle were immutable for the duration of the operation.
    std::vector<MiniGdi::Color> samples(static_cast<size_t>(width) * height);
    for (int dy = 0; dy < height; ++dy) for (int dx = 0; dx < width; ++dx)
    {
        const MiniGdi::Color* pixel = input->PixelAt(sourceX + dx * sourceWidth / width,
            sourceY + dy * sourceHeight / height);
        samples[static_cast<size_t>(dy) * width + dx] = pixel ? *pixel : MiniGdi::Transparent;
    }
    for (int dy = 0; dy < height; ++dy) for (int dx = 0; dx < width; ++dx)
    {
        const int tx = x + dx, ty = y + dy;
        if (!clip.Contains({ tx, ty })) continue;
        MiniGdi::Color* target = output->PixelAt(tx, ty);
        if (!target) continue;
        const MiniGdi::Color sourcePixel = samples[static_cast<size_t>(dy) * width + dx];
        switch (operation)
        {
        case SRCCOPY: *target = sourcePixel; break;
        case SRCAND: *target = (sourcePixel & *target) | 0xff000000u; break;
        case SRCPAINT: *target = (sourcePixel | *target) | 0xff000000u; break;
        case SRCINVERT: *target = (sourcePixel ^ *target) | 0xff000000u; break;
        case NOTSRCCOPY: *target = (~sourcePixel) | 0xff000000u; break;
        case BLACKNESS: *target = MiniGdi::OpaqueBlack; break;
        case WHITENESS: *target = MiniGdi::OpaqueWhite; break;
        }
    }
    BridgeSetLastError(ERROR_SUCCESS);
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
    if (!information || !bits) return GDI_ERROR;
    switch (operation)
    {
    case SRCCOPY:
    case SRCAND:
    case SRCPAINT:
    case SRCINVERT:
    case NOTSRCCOPY:
    case BLACKNESS:
    case WHITENESS:
        break;
    default:
        BridgeSetLastError(ERROR_NOT_SUPPORTED);
        return GDI_ERROR;
    }
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
    x = (std::min)(destinationFirst.x, destinationSecond.x);
    y = (std::min)(destinationFirst.y, destinationSecond.y);
    width = (std::abs)(destinationSecond.x - destinationFirst.x);
    height = (std::abs)(destinationSecond.y - destinationFirst.y);
    if (width == 0 || height == 0) return 0;
    MiniGdi::Region clip;
    manager->Gdi().GetClipRegion(FromGuestDc(destination), &clip);
    for (int dy = 0; dy < height; ++dy) for (int dx = 0; dx < width; ++dx)
    {
        const int tx = x + dx, ty = y + dy;
        if (!clip.Contains({ tx, ty })) continue;
        const MiniGdi::Color* pixel = source.PixelAt(sourceX + dx * sourceWidth / width,
            sourceY + dy * sourceHeight / height);
        MiniGdi::Color* target = output->PixelAt(tx, ty);
        if (!target) continue;
        const MiniGdi::Color sourcePixel = pixel ? *pixel : MiniGdi::Transparent;
        switch (operation)
        {
        case SRCCOPY: *target = sourcePixel; break;
        case SRCAND: *target = (sourcePixel & *target) | 0xff000000u; break;
        case SRCPAINT: *target = (sourcePixel | *target) | 0xff000000u; break;
        case SRCINVERT: *target = (sourcePixel ^ *target) | 0xff000000u; break;
        case NOTSRCCOPY: *target = (~sourcePixel) | 0xff000000u; break;
        case BLACKNESS: *target = MiniGdi::OpaqueBlack; break;
        case WHITENESS: *target = MiniGdi::OpaqueWhite; break;
        }
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

int WINAPI BridgeEnumFontsW(
    HDC dc, LPCWSTR faceName, GuestFontEnumProcW callback, LPARAM parameter)
{
    LOGFONTW font = {};
    wcscpy_s(font.lfFaceName, faceName && *faceName ? faceName : L"Segoe UI");
    font.lfCharSet = DEFAULT_CHARSET;
    return BridgeEnumFontFamiliesExW(dc, &font, callback, parameter, 0);
}

HDC WINAPI BridgeCreateDCW(LPCWSTR, LPCWSTR, LPCWSTR, const void*)
{
    BridgeSetLastError(ERROR_CALL_NOT_IMPLEMENTED);
    return nullptr;
}
}
