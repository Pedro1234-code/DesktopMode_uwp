#include "pch.h"
#include "Bridge/CommonControlsShims.h"
#include "Bridge/GuestMetrics.h"
#include "Bridge/GuestWindow.h"
#include "Bridge/User32Shims.h"
#include "Bridge/RuntimeDiagnostics.h"
#include "Bridge\\MiniGdi.h"

#include <algorithm>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

// Keep the private image-list backing store consistent with the other bridge
// shims: MiniGdi is nested under this namespace, while the backing store below
// itself remains translation-unit private.
using namespace Win32Bridge::Bridge;

void WINAPI Win32Bridge::Bridge::BridgeInitCommonControls()
{
    // GuestWindowManager owns the controls implemented by the bridge; no host
    // DLL registration is necessary.  The call still succeeds as on Win32.
}

namespace
{
    constexpr COLORREF NoMaskColor = 0xffffffffu; // CLR_NONE without desktop commctrl headers.
    struct ImageListRecord
    {
        int width = 16;
        int height = 16;
        std::vector<MiniGdi::Surface> images;
    };
    std::mutex g_imageListsLock;
    std::unordered_map<ULONG_PTR, ImageListRecord> g_imageLists;
    ULONG_PTR g_nextImageList = 0x60000000;

    MiniGdi::Surface ScaleImage(const MiniGdi::Surface& source, int width, int height)
    {
        MiniGdi::Surface result(width, height, MiniGdi::Transparent);
        if (source.Empty() || result.Empty()) return result;
        for (int y = 0; y < height; ++y)
        {
            const int sourceY = (std::min)(source.Height() - 1, y * source.Height() / height);
            for (int x = 0; x < width; ++x)
            {
                const int sourceX = (std::min)(source.Width() - 1, x * source.Width() / width);
                MiniGdi::Color* target = result.PixelAt(x, y);
                const MiniGdi::Color* pixel = source.PixelAt(sourceX, sourceY);
                if (target && pixel) *target = *pixel;
            }
        }
        return result;
    }
}

BOOL WINAPI Win32Bridge::Bridge::BridgeInitCommonControlsEx(const GuestInitCommonControlsEx*) { return TRUE; }
Win32Bridge::Bridge::GuestImageList WINAPI Win32Bridge::Bridge::BridgeImageListCreate(int width, int height, UINT, int, int)
{
    if (width <= 0 || height <= 0 || width > 256 || height > 256) return nullptr;
    std::lock_guard<std::mutex> guard(g_imageListsLock);
    const ULONG_PTR token = g_nextImageList++;
    ImageListRecord record;
    record.width = width;
    record.height = height;
    g_imageLists.emplace(token, std::move(record));
    return reinterpret_cast<GuestImageList>(token);
}
BOOL WINAPI Win32Bridge::Bridge::BridgeImageListDestroy(GuestImageList imageList)
{
    std::lock_guard<std::mutex> guard(g_imageListsLock);
    return g_imageLists.erase(reinterpret_cast<ULONG_PTR>(imageList)) ? TRUE : FALSE;
}
int WINAPI Win32Bridge::Bridge::BridgeImageListAddMasked(GuestImageList imageList, HBITMAP bitmap, COLORREF mask)
{
    std::lock_guard<std::mutex> guard(g_imageListsLock);
    const auto found = g_imageLists.find(reinterpret_cast<ULONG_PTR>(imageList));
    if (found == g_imageLists.end()) return -1;
    GuestWindowManager* manager = CurrentGuestWindowManager();
    const MiniGdi::ObjectHandle handle = static_cast<MiniGdi::ObjectHandle>(reinterpret_cast<ULONG_PTR>(bitmap));
    const MiniGdi::Surface* source = manager ? manager->Gdi().GetBitmapSurface(handle) : nullptr;
    if (!source || source->Empty()) return -1;
    ImageListRecord& list = found->second;
    const int imageCount = source->Width() / list.width;
    if (imageCount <= 0 || source->Height() < list.height) return -1;
    const int first = static_cast<int>(list.images.size());
    const MiniGdi::Color maskColor = MiniGdi::MakeColor(
        static_cast<std::uint8_t>(mask & 0xff),
        static_cast<std::uint8_t>((mask >> 8) & 0xff),
        static_cast<std::uint8_t>((mask >> 16) & 0xff));
    for (int imageIndex = 0; imageIndex < imageCount; ++imageIndex)
    {
        MiniGdi::Surface tile(list.width, list.height, MiniGdi::Transparent);
        MiniGdi::CopyRect(tile, MiniGdi::Point{ 0, 0 }, *source,
            MiniGdi::Rect{ imageIndex * list.width, 0, (imageIndex + 1) * list.width, list.height });
        if (mask != NoMaskColor)
        {
            for (MiniGdi::Color& pixel : tile.Pixels())
            {
                if ((pixel & 0x00ffffffu) == (maskColor & 0x00ffffffu)) pixel = MiniGdi::Transparent;
            }
        }
        list.images.push_back(std::move(tile));
    }
    RuntimeDiagnostics::Record(L"IMAGELIST: imported " + std::to_wstring(imageCount) + L" bitmap image(s).");
    return first;
}
int WINAPI Win32Bridge::Bridge::BridgeImageListGetImageCount(GuestImageList imageList)
{
    std::lock_guard<std::mutex> guard(g_imageListsLock);
    const auto found = g_imageLists.find(reinterpret_cast<ULONG_PTR>(imageList));
    return found == g_imageLists.end() ? 0 : static_cast<int>(found->second.images.size());
}
int WINAPI Win32Bridge::Bridge::BridgeImageListReplaceIcon(GuestImageList imageList, int index, HICON icon)
{
    std::lock_guard<std::mutex> guard(g_imageListsLock);
    const auto found = g_imageLists.find(reinterpret_cast<ULONG_PTR>(imageList));
    if (found == g_imageLists.end()) return -1;
    MiniGdi::Surface source;
    if (!CopyGuestIconPixels(icon, &source) || source.Empty()) return -1;
    ImageListRecord& list = found->second;
    MiniGdi::Surface tile = ScaleImage(source, list.width, list.height);
    if (index < 0)
    {
        list.images.push_back(std::move(tile));
        RuntimeDiagnostics::Record(L"IMAGELIST: appended a decoded icon resource.");
        return static_cast<int>(list.images.size()) - 1;
    }
    if (static_cast<size_t>(index) >= list.images.size()) return -1;
    list.images[static_cast<size_t>(index)] = std::move(tile);
    RuntimeDiagnostics::Record(L"IMAGELIST: replaced an icon resource.");
    return index;
}

bool Win32Bridge::Bridge::CopyGuestImageListImage(GuestImageList imageList, int index, MiniGdi::Surface* destination)
{
    if (!destination || index < 0) return false;
    std::lock_guard<std::mutex> guard(g_imageListsLock);
    const auto found = g_imageLists.find(reinterpret_cast<ULONG_PTR>(imageList));
    if (found == g_imageLists.end() || static_cast<size_t>(index) >= found->second.images.size()) return false;
    *destination = found->second.images[static_cast<size_t>(index)];
    return true;
}
HWND WINAPI Win32Bridge::Bridge::BridgeCreateToolbarEx(
    HWND parent,
    DWORD style,
    UINT identifier,
    int bitmapCount,
    HINSTANCE instance,
    UINT_PTR bitmapId,
    const void* buttons,
    int buttonCount,
    int buttonWidth,
    int buttonHeight,
    int bitmapWidth,
    int bitmapHeight,
    UINT structureSize)
{
    GuestWindowManager* manager = CurrentGuestWindowManager();
    DWORD error = ERROR_SUCCESS;
    if (!manager)
    {
        return nullptr;
    }
    const HWND toolbar = manager->CreateGuestWindow(0, L"ToolbarWindow32", L"",
        style | WS_CHILD, 0, 0, 100, GuestMetrics::ToolbarHeight, parent,
        reinterpret_cast<HMENU>(static_cast<ULONG_PTR>(identifier)), instance, nullptr, &error);
    if (!toolbar)
    {
        return nullptr;
    }
    manager->SendGuestMessage(toolbar, 0x041e, structureSize, 0, nullptr); // TB_BUTTONSTRUCTSIZE
    if (buttonWidth > 0 && buttonHeight > 0)
    {
        manager->SendGuestMessage(toolbar, 0x041f, 0,
            MAKELPARAM(buttonWidth, buttonHeight), nullptr); // TB_SETBUTTONSIZE
    }
    if (bitmapWidth > 0 && bitmapHeight > 0)
    {
        manager->SendGuestMessage(toolbar, 0x0420, 0,
            MAKELPARAM(bitmapWidth, bitmapHeight), nullptr); // TB_SETBITMAPSIZE
    }
    // CreateToolbarEx is also responsible for loading the caller's bitmap
    // strip.  Keeping only the button records leaves a correctly-sized but
    // visually empty toolbar, even though every command is present.  Model
    // the native TB_ADDBITMAP path with a bridge-owned image list so this
    // remains useful for any guest that uses the legacy helper.
    if (bitmapCount > 0 && bitmapId != 0)
    {
        const int imageWidth = bitmapWidth > 0
            ? bitmapWidth : GuestMetrics::DefaultBitmapExtent;
        const int imageHeight = bitmapHeight > 0
            ? bitmapHeight : GuestMetrics::DefaultBitmapExtent;
        const HBITMAP bitmap = BridgeLoadBitmapW(
            instance, reinterpret_cast<LPCWSTR>(bitmapId));
        if (bitmap)
        {
            const GuestImageList imageList = BridgeImageListCreate(
                imageWidth, imageHeight, 0, bitmapCount, 1);
            const int firstImage = imageList
                ? BridgeImageListAddMasked(imageList, bitmap, RGB(192, 192, 192))
                : -1;
            manager->Gdi().DeleteObject(
                static_cast<MiniGdi::ObjectHandle>(reinterpret_cast<ULONG_PTR>(bitmap)));
            if (firstImage >= 0)
            {
                manager->SendGuestMessage(toolbar, 0x0430, 0,
                    reinterpret_cast<LPARAM>(imageList), nullptr); // TB_SETIMAGELIST
            }
            else if (imageList)
            {
                BridgeImageListDestroy(imageList);
            }
        }
    }
    if (buttons && buttonCount > 0)
    {
        manager->SendGuestMessage(toolbar, 0x0444,
            static_cast<WPARAM>(buttonCount), reinterpret_cast<LPARAM>(buttons), nullptr); // TB_ADDBUTTONSW
    }
    manager->SendGuestMessage(toolbar, 0x0421, 0, 0, nullptr); // TB_AUTOSIZE
    return toolbar;
}
HWND WINAPI Win32Bridge::Bridge::BridgeCreateStatusWindowW(LONG style, LPCWSTR text, HWND parent, UINT identifier)
{
    GuestWindowManager* manager = CurrentGuestWindowManager();
    DWORD error = ERROR_SUCCESS;
    return manager ? manager->CreateGuestWindow(0, L"msctls_statusbar32", text ? text : L"",
        static_cast<DWORD>(style) | WS_CHILD | WS_VISIBLE,
        0, 0, 100, GuestMetrics::StatusBarHeight, parent,
        reinterpret_cast<HMENU>(static_cast<ULONG_PTR>(identifier)), nullptr, nullptr, &error) : nullptr;
}
INT_PTR WINAPI Win32Bridge::Bridge::BridgePropertySheetW(const void*) { return -1; }
HRESULT WINAPI Win32Bridge::Bridge::BridgeDllGetVersion(GuestDllVersionInfo* versionInfo)
{
    if (!versionInfo || versionInfo->cbSize < sizeof(GuestDllVersionInfo))
    {
        return E_INVALIDARG;
    }
    versionInfo->dwMajorVersion = 6;
    versionInfo->dwMinorVersion = 0;
    versionInfo->dwBuildNumber = 0;
    versionInfo->dwPlatformID = VER_PLATFORM_WIN32_NT;
    return S_OK;
}

Win32Bridge::Bridge::ImportResolution Win32Bridge::Bridge::ResolveCommonControlsImport(const ImportedSymbol& symbol)
{
    auto resolution = CompatibilityCatalog::Resolve(symbol);
    if (_wcsicmp(symbol.library.c_str(), L"comctl32.dll") == 0 && symbol.importedByOrdinal && symbol.ordinal == 17)
    {
        resolution.targetAddress = reinterpret_cast<ULONGLONG>(&BridgeInitCommonControls);
        resolution.disposition = ImportDisposition::NeedsBridge;
        resolution.note = L"Common-controls bootstrap: uses bridge-owned controls rather than a desktop DLL.";
    }
    else if (_wcsicmp(symbol.library.c_str(), L"comctl32.dll") == 0 && !symbol.importedByOrdinal)
    {
        if (_wcsicmp(symbol.name.c_str(), L"initcommoncontrolsex") == 0) resolution.targetAddress = reinterpret_cast<ULONGLONG>(&BridgeInitCommonControlsEx);
        else if (_wcsicmp(symbol.name.c_str(), L"imagelist_create") == 0) resolution.targetAddress = reinterpret_cast<ULONGLONG>(&BridgeImageListCreate);
        else if (_wcsicmp(symbol.name.c_str(), L"imagelist_destroy") == 0) resolution.targetAddress = reinterpret_cast<ULONGLONG>(&BridgeImageListDestroy);
        else if (_wcsicmp(symbol.name.c_str(), L"imagelist_addmasked") == 0) resolution.targetAddress = reinterpret_cast<ULONGLONG>(&BridgeImageListAddMasked);
        else if (_wcsicmp(symbol.name.c_str(), L"imagelist_getimagecount") == 0) resolution.targetAddress = reinterpret_cast<ULONGLONG>(&BridgeImageListGetImageCount);
        else if (_wcsicmp(symbol.name.c_str(), L"imagelist_replaceicon") == 0) resolution.targetAddress = reinterpret_cast<ULONGLONG>(&BridgeImageListReplaceIcon);
        else if (_wcsicmp(symbol.name.c_str(), L"createtoolbarex") == 0) resolution.targetAddress = reinterpret_cast<ULONGLONG>(&BridgeCreateToolbarEx);
        else if (_wcsicmp(symbol.name.c_str(), L"createstatuswindoww") == 0) resolution.targetAddress = reinterpret_cast<ULONGLONG>(&BridgeCreateStatusWindowW);
        else if (_wcsicmp(symbol.name.c_str(), L"propertysheetw") == 0) resolution.targetAddress = reinterpret_cast<ULONGLONG>(&BridgePropertySheetW);
        else if (_wcsicmp(symbol.name.c_str(), L"dllgetversion") == 0) resolution.targetAddress = reinterpret_cast<ULONGLONG>(&BridgeDllGetVersion);
        if (resolution.targetAddress)
        {
            resolution.disposition = ImportDisposition::NeedsBridge;
            resolution.note = L"Common-controls adapter: guest-owned image lists and virtual toolbar/status controls.";
        }
    }
    return resolution;
}
