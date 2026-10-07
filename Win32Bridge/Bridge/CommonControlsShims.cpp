#include "pch.h"
#include "Bridge/CommonControlsShims.h"
#include "Bridge/ActivationContext.h"
#include "Bridge/GuestMetrics.h"
#include "Bridge/GuestWindow.h"
#include "Bridge/User32Shims.h"
#include "Bridge/DialogResources.h"
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

namespace
{
    HANDLE WINAPI BridgeOpenThemeData(HWND window, LPCWSTR)
    {
        if (!CurrentGuestUsesVisualStyles()) return nullptr;
        const ULONG_PTR value = reinterpret_cast<ULONG_PTR>(window);
        return reinterpret_cast<HANDLE>((value ? value : 1) | static_cast<ULONG_PTR>(1));
    }

    HRESULT WINAPI BridgeCloseThemeData(HANDLE theme) { return theme ? S_OK : E_HANDLE; }
    BOOL WINAPI BridgeIsThemeActive() { return CurrentGuestUsesVisualStyles() ? TRUE : FALSE; }
    BOOL WINAPI BridgeIsAppThemed() { return BridgeIsThemeActive(); }
    HRESULT WINAPI BridgeSetWindowTheme(HWND, LPCWSTR, LPCWSTR) { return S_OK; }

    HRESULT WINAPI BridgeDrawThemeBackground(HANDLE theme, HDC, int, int, const RECT*, const RECT*)
    {
        // GuestWindow already paints built-in controls according to the
        // active context. Do not let callers overlay a classic fallback.
        return theme && CurrentGuestUsesVisualStyles() ? S_OK : E_HANDLE;
    }

    HRESULT WINAPI BridgeGetThemeColor(HANDLE theme, int, int, int property, COLORREF* color)
    {
        if (!theme || !color || !CurrentGuestUsesVisualStyles()) return E_HANDLE;
        *color = property == 3803 /* TMT_TEXTCOLOR */ ? RGB(0, 0, 0) : RGB(240, 240, 240);
        return S_OK;
    }
}

void WINAPI Win32Bridge::Bridge::BridgeInitCommonControls()
{
    // GuestWindowManager owns the controls implemented by the bridge; no host
    // DLL registration is necessary.  The call still succeeds as on Win32.
}

namespace
{
    struct GuestPropertySheetPageW final
    {
        DWORD size;
        DWORD flags;
        HINSTANCE instance;
        LPCWSTR templateName;
        HICON icon;
        LPCWSTR title;
        DLGPROC dialogProcedure;
        LPARAM parameter;
        PVOID callback;
        UINT* referenceCount;
    };

    struct GuestPropertySheetHeaderW final
    {
        DWORD size;
        DWORD flags;
        HWND parent;
        HINSTANCE instance;
        HICON icon;
        LPCWSTR caption;
        UINT pageCount;
        // PROPSHEETHEADERW declares this field as a union of UINT and
        // LPCWSTR.  Store the whole pointer-sized union so the following
        // fields retain their native x64 offsets.
        ULONG_PTR startPage;
        const GuestPropertySheetPageW* pages;
        PVOID callback;
    };

    constexpr DWORD PropertySheetHeaderPagesAreStructures = 0x00000008;
    constexpr DWORD PropertySheetHeaderUsesStartPageName = 0x00000040;

    bool ReadBoundedGuestString(LPCWSTR source, std::wstring* result)
    {
        if (!result) return false;
        result->clear();
        if (!source || reinterpret_cast<ULONG_PTR>(source) <= 0xffff) return false;
        __try
        {
            constexpr std::size_t MaximumCaptionCharacters = 4096;
            std::size_t length = 0;
            while (length < MaximumCaptionCharacters && source[length]) ++length;
            if (length == MaximumCaptionCharacters) return false;
            result->assign(source, source + length);
            return true;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            result->clear();
            return false;
        }
    }

    bool ReadPropertySheetHeader(const void* source, GuestPropertySheetHeaderW* result)
    {
        if (!source || !result || reinterpret_cast<ULONG_PTR>(source) <= 0xffff) return false;
        __try
        {
            const DWORD size = *static_cast<const DWORD*>(source);
            if (size < offsetof(GuestPropertySheetHeaderW, callback)) return false;
            ZeroMemory(result, sizeof(*result));
            memcpy(result, source, (std::min)(static_cast<size_t>(size), sizeof(*result)));
            return true;
        }
        __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
    }

    bool ReadPropertySheetPage(const BYTE* source, GuestPropertySheetPageW* result, DWORD* size)
    {
        if (!source || !result || !size) return false;
        __try
        {
            *size = *reinterpret_cast<const DWORD*>(source);
            if (*size < offsetof(GuestPropertySheetPageW, callback) || *size > 4096) return false;
            ZeroMemory(result, sizeof(*result));
            memcpy(result, source, (std::min)(static_cast<size_t>(*size), sizeof(*result)));
            return true;
        }
        __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
    }
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
INT_PTR WINAPI Win32Bridge::Bridge::BridgePropertySheetW(const void* headerPointer)
{
    if (!headerPointer || reinterpret_cast<ULONG_PTR>(headerPointer) <= 0xffff)
        return -1;
    GuestPropertySheetHeaderW header = {};
    if (!ReadPropertySheetHeader(headerPointer, &header)) return -1;
    if (header.pageCount == 0 || header.pageCount > 256 || !header.pages ||
        (header.flags & PropertySheetHeaderPagesAreStructures) == 0)
    {
        RuntimeDiagnostics::Record(L"PROPERTYSHEET: unsupported or empty page collection.");
        return -1;
    }

    UINT selected = (header.flags & PropertySheetHeaderUsesStartPageName) != 0
        ? 0
        : (std::min)(static_cast<UINT>(header.startPage), header.pageCount - 1);
    const BYTE* cursor = reinterpret_cast<const BYTE*>(header.pages);
    std::vector<GuestPropertyPageDescriptor> pages;
    std::vector<std::wstring> pageTitles;
    pages.reserve(header.pageCount);
    pageTitles.reserve(header.pageCount);
    for (UINT index = 0; index < header.pageCount; ++index)
    {
        DWORD pageSize = 0;
        GuestPropertySheetPageW page = {};
        if (!ReadPropertySheetPage(cursor, &page, &pageSize)) return -1;
        RuntimeDiagnostics::Record(
            L"PROPERTYSHEET: page " + std::to_wstring(index) +
            L" size " + std::to_wstring(pageSize) +
            L", flags " + std::to_wstring(page.flags) + L".");

        GuestPropertyPageDescriptor descriptor;
        descriptor.instance = page.instance ? page.instance : header.instance;
        descriptor.templateName = page.templateName;
        descriptor.dialogProcedure = page.dialogProcedure;
        descriptor.initParameter = reinterpret_cast<LPARAM>(cursor);
        std::wstring pageTitle;
        if ((page.flags & 0x00000008u) != 0) // PSP_USETITLE
            ReadBoundedGuestString(page.title, &pageTitle);
        pageTitles.push_back(std::move(pageTitle));
        descriptor.title = nullptr;
        pages.push_back(descriptor);

        if ((header.flags & PropertySheetHeaderUsesStartPageName) != 0 &&
            header.startPage > 0xffff && !pageTitles.back().empty())
        {
            std::wstring requestedTitle;
            if (ReadBoundedGuestString(
                    reinterpret_cast<LPCWSTR>(header.startPage), &requestedTitle) &&
                _wcsicmp(requestedTitle.c_str(), pageTitles.back().c_str()) == 0)
            {
                selected = index;
            }
        }
        cursor += pageSize;
    }
    for (UINT index = 0; index < header.pageCount; ++index)
    {
        if (!pageTitles[index].empty()) pages[index].title = pageTitles[index].c_str();
    }
    RuntimeDiagnostics::Record(L"PROPERTYSHEET: presenting page " +
        std::to_wstring(selected) + L" of " + std::to_wstring(header.pageCount) + L".");

    std::wstring caption;
    ReadBoundedGuestString(header.caption, &caption);
    GuestPropertySheetDescriptor sheet;
    sheet.parent = header.parent;
    sheet.instance = header.instance;
    sheet.caption = caption.empty() ? nullptr : caption.c_str();
    sheet.pages = pages.data();
    sheet.pageCount = static_cast<UINT>(pages.size());
    sheet.startPage = selected;
    sheet.flags = header.flags;
    return ShowGuestPropertySheet(sheet);
}
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

HANDLE WINAPI Win32Bridge::Bridge::BridgeCreatePropertySheetPageW(const void* page)
{
    return page ? const_cast<void*>(page) : nullptr;
}

ULONG_PTR WINAPI Win32Bridge::Bridge::BridgeCommonControlOrdinal345()
{
    return TRUE;
}

Win32Bridge::Bridge::ImportResolution Win32Bridge::Bridge::ResolveCommonControlsImport(const ImportedSymbol& symbol)
{
    auto resolution = CompatibilityCatalog::Resolve(symbol);
    if (_wcsicmp(symbol.library.c_str(), L"comctl32.dll") == 0 &&
        symbol.importedByOrdinal && (symbol.ordinal == 17 || symbol.ordinal == 345))
    {
        resolution.targetAddress = symbol.ordinal == 17
            ? reinterpret_cast<ULONGLONG>(&BridgeInitCommonControls)
            : reinterpret_cast<ULONGLONG>(&BridgeCommonControlOrdinal345);
        resolution.disposition = ImportDisposition::NeedsBridge;
        resolution.note = L"Common-controls bootstrap: uses bridge-owned controls rather than a desktop DLL.";
    }
    else if (_wcsicmp(symbol.library.c_str(), L"uxtheme.dll") == 0 && !symbol.importedByOrdinal)
    {
        if (_wcsicmp(symbol.name.c_str(), L"openthemedata") == 0) resolution.targetAddress = reinterpret_cast<ULONGLONG>(&BridgeOpenThemeData);
        else if (_wcsicmp(symbol.name.c_str(), L"closethemedata") == 0) resolution.targetAddress = reinterpret_cast<ULONGLONG>(&BridgeCloseThemeData);
        else if (_wcsicmp(symbol.name.c_str(), L"isthemeactive") == 0) resolution.targetAddress = reinterpret_cast<ULONGLONG>(&BridgeIsThemeActive);
        else if (_wcsicmp(symbol.name.c_str(), L"isappthemed") == 0) resolution.targetAddress = reinterpret_cast<ULONGLONG>(&BridgeIsAppThemed);
        else if (_wcsicmp(symbol.name.c_str(), L"setwindowtheme") == 0) resolution.targetAddress = reinterpret_cast<ULONGLONG>(&BridgeSetWindowTheme);
        else if (_wcsicmp(symbol.name.c_str(), L"drawthemebackground") == 0) resolution.targetAddress = reinterpret_cast<ULONGLONG>(&BridgeDrawThemeBackground);
        else if (_wcsicmp(symbol.name.c_str(), L"getthemecolor") == 0) resolution.targetAddress = reinterpret_cast<ULONGLONG>(&BridgeGetThemeColor);
        if (resolution.targetAddress)
        {
            resolution.disposition = ImportDisposition::NeedsBridge;
            resolution.note = L"Activation-context-aware virtual theme API.";
        }
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
        else if (_wcsicmp(symbol.name.c_str(), L"createpropertysheetpagew") == 0) resolution.targetAddress = reinterpret_cast<ULONGLONG>(&BridgeCreatePropertySheetPageW);
        else if (_wcsicmp(symbol.name.c_str(), L"dllgetversion") == 0) resolution.targetAddress = reinterpret_cast<ULONGLONG>(&BridgeDllGetVersion);
        if (resolution.targetAddress)
        {
            resolution.disposition = ImportDisposition::NeedsBridge;
            resolution.note = L"Common-controls adapter: guest-owned image lists and virtual toolbar/status controls.";
        }
    }
    return resolution;
}
