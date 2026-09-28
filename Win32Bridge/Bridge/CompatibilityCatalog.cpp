#include "Bridge\\CompatibilityCatalog.h"

#include <cwctype>

using namespace Win32Bridge::Bridge;

namespace
{
    std::wstring Lowercase(std::wstring value)
    {
        for (auto& character : value)
        {
            character = static_cast<wchar_t>(towlower(character));
        }
        return value;
    }

    bool IsKernelFamily(const std::wstring& library)
    {
        return library == L"kernel32.dll" ||
            library == L"kernelbase.dll" ||
            library.rfind(L"api-ms-win-core-", 0) == 0;
    }

    bool IsUserFamily(const std::wstring& library)
    {
        return library == L"user32.dll" ||
            library.rfind(L"api-ms-win-ntuser-", 0) == 0 ||
            library.rfind(L"ext-ms-win-ntuser-", 0) == 0;
    }

    bool IsGdiFamily(const std::wstring& library)
    {
        return library == L"gdi32.dll" ||
            library == L"gdi32full.dll" ||
            library.rfind(L"api-ms-win-gdi-", 0) == 0;
    }

    bool IsVirtualStorageImport(const std::wstring& name)
    {
        return name == L"createfilew" ||
            name == L"readfile" ||
            name == L"writefile" ||
            name == L"closehandle" ||
            name == L"getfilesizeex" ||
            name == L"setfilepointerex" ||
            name == L"flushfilebuffers" ||
            name == L"createdirectoryw" ||
            name == L"deletefilew" ||
            name == L"movefilew" ||
            name == L"movefileexw" ||
            name == L"removedirectoryw" ||
            name == L"getfileattributesw" ||
            name == L"findfirstfilew" ||
            name == L"findnextfilew" ||
            name == L"findclose" ||
            name == L"setcurrentdirectoryw" ||
            name == L"getcurrentdirectoryw" ||
            name == L"gettemppathw" ||
            name == L"getmodulefilenamew" ||
            name == L"getlasterror" ||
            name == L"setlasterror";
    }

    bool IsGuestSynchronizationImport(const std::wstring& name)
    {
        return name == L"createeventw" ||
            name == L"setevent" ||
            name == L"resetevent" ||
            name == L"createmutexw" ||
            name == L"releasemutex" ||
            name == L"waitforsingleobject" ||
            name == L"sleep";
    }

    bool IsGuestRuntimeBasicsImport(const std::wstring& name)
    {
        return name == L"getmodulehandlew" ||
            name == L"gettickcount" ||
            name == L"gettickcount64" ||
            name == L"queryperformancecounter" ||
            name == L"queryperformancefrequency" ||
            name == L"getcurrentthreadid" ||
            name == L"getcurrentprocessid" ||
            name == L"getcommandlinew" ||
            name == L"outputdebugstringw" ||
            name == L"isdebuggerpresent";
    }

    bool IsGuestWindowImport(const std::wstring& name)
    {
        return name == L"registerclassexw" ||
            name == L"registerclassw" ||
            name == L"createwindowexw" ||
            name == L"loadcursorw" ||
            name == L"loadiconw" ||
            name == L"destroycursor" ||
            name == L"destroyicon" ||
            name == L"getsystemmetrics" ||
            name == L"getsyscolor" ||
            name == L"getsyscolorbrush" ||
            name == L"adjustwindowrect" ||
            name == L"adjustwindowrectex" ||
            name == L"destroywindow" ||
            name == L"showwindow" ||
            name == L"getclientrect" ||
            name == L"getwindowrect" ||
            name == L"setwindowtextw" ||
            name == L"getwindowtextw" ||
            name == L"getwindowtextlengthw" ||
            name == L"getdlgitem" ||
            name == L"getdlgctrlid" ||
            name == L"setdlgitemtextw" ||
            name == L"getdlgitemtextw" ||
            name == L"senddlgitemmessagew" ||
            name == L"iswindow" ||
            name == L"iswindowvisible" ||
            name == L"iswindowenabled" ||
            name == L"enablewindow" ||
            name == L"setfocus" ||
            name == L"getfocus" ||
            name == L"setcapture" ||
            name == L"releasecapture" ||
            name == L"getcapture" ||
            name == L"getparent" ||
            name == L"getwindowlongw" ||
            name == L"setwindowlongw" ||
            name == L"getwindowlongptrw" ||
            name == L"setwindowlongptrw" ||
            name == L"setwindowpos" ||
            name == L"settimer" ||
            name == L"killtimer" ||
            name == L"defwindowprocw" ||
            name == L"postmessagew" ||
            name == L"sendmessagew" ||
            name == L"postquitmessage" ||
            name == L"getmessagew" ||
            name == L"peekmessagew" ||
            name == L"translatemessage" ||
            name == L"dispatchmessagew" ||
            name == L"invalidaterect" ||
            name == L"updatewindow" ||
            name == L"beginpaint" ||
            name == L"endpaint" ||
            name == L"getdc" ||
            name == L"releasedc" ||
            name == L"fillrect" ||
            name == L"drawtextw";
    }

    bool IsMiniGdiImport(const std::wstring& name)
    {
        return name == L"createsolidbrush" ||
            name == L"createpen" ||
            name == L"createfontw" ||
            name == L"createfontindirectw" ||
            name == L"gettextmetricsw" ||
            name == L"gettextfacew" ||
            name == L"getobjectw" ||
            name == L"getstockobject" ||
            name == L"deleteobject" ||
            name == L"selectobject" ||
            name == L"settextcolor" ||
            name == L"setbkcolor" ||
            name == L"setbkmode" ||
            name == L"rectangle" ||
            name == L"ellipse" ||
            name == L"movetoex" ||
            name == L"lineto" ||
            name == L"setpixelv" ||
            name == L"getpixel" ||
            name == L"getdevicecaps" ||
            name == L"textoutw" ||
            name == L"gettextextentpoint32w" ||
            name == L"createcompatibledc" ||
            name == L"deletedc" ||
            name == L"createcompatiblebitmap" ||
            name == L"bitblt";
    }
}

ImportResolution CompatibilityCatalog::Resolve(const ImportedSymbol& symbol)
{
    if (symbol.importedByOrdinal)
    {
        return { ImportDisposition::Unsupported, L"Ordinal imports need an explicit catalog entry." };
    }

    const std::wstring library = Lowercase(symbol.library);
    const std::wstring name = Lowercase(symbol.name);
    if (IsUserFamily(library) && name == L"messageboxw")
    {
        return { ImportDisposition::NeedsBridge, L"UI adapter: create a native guest dialog on the MiniGDI surface." };
    }

    if (IsUserFamily(library) && name == L"getcursorpos")
    {
        return { ImportDisposition::NeedsBridge, L"Mouse adapter: expose the latest USB mouse coordinates from CoreWindow." };
    }

    if (IsUserFamily(library) && (name == L"getasynckeystate" || name == L"getkeystate"))
    {
        return { ImportDisposition::NeedsBridge, L"Mouse adapter: expose USB mouse button state using Win32 virtual keys." };
    }

    if (IsUserFamily(library) && (name == L"settimer" || name == L"killtimer"))
    {
        return { ImportDisposition::NeedsBridge, L"USER32 timer adapter: bounded UWP timers post coalesced WM_TIMER; TIMERPROC callbacks are not invoked." };
    }

    if (IsUserFamily(library) && IsGuestWindowImport(name))
    {
        return { ImportDisposition::NeedsBridge, L"USER32 adapter: guest windows, message queue, paint cycle, and virtual DCs." };
    }

    if (IsGdiFamily(library) && IsMiniGdiImport(name))
    {
        return { ImportDisposition::NeedsBridge, L"MiniGDI adapter: software pens, brushes, retained font handles and queries (GetObjectW for HFONT), selection, and primitive raster drawing." };
    }

    if (IsKernelFamily(library) && name == L"closehandle")
    {
        return { ImportDisposition::NeedsBridge, L"Guest-handle adapter: closes virtual files or guest-local synchronization objects." };
    }

    if (IsKernelFamily(library) && IsGuestRuntimeBasicsImport(name))
    {
        return { ImportDisposition::NeedsBridge, L"Core runtime adapter: guest module identity, monotonic time, local process/thread IDs, and diagnostics." };
    }

    if (IsKernelFamily(library) && IsVirtualStorageImport(name))
    {
        return { ImportDisposition::NeedsBridge, L"Virtual storage adapter: maps the guest C: drive to the app LocalFolder sandbox." };
    }

    if (IsKernelFamily(library) && IsGuestSynchronizationImport(name))
    {
        return { ImportDisposition::NeedsBridge, L"Kernel synchronization adapter: guest-local events, mutexes, waits, and cooperative sleep." };
    }

    if (IsKernelFamily(library))
    {
        return { ImportDisposition::Deferred, L"Kernel32 adapter pending: resolve through the bridge API catalog, not the desktop DLL." };
    }

    return { ImportDisposition::Unsupported, L"No adapter is registered for this import yet." };
}
