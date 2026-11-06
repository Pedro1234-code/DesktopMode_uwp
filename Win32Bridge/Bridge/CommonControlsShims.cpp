#include "pch.h"
#include "Bridge/CommonControlsShims.h"
#include "Bridge/ActivationContext.h"
#include "Bridge/GuestMetrics.h"
#include "Bridge/GuestWindow.h"
#include "Bridge/Gdi32Shims.h"
#include "Bridge/Kernel32Shims.h"
#include "Bridge/User32Shims.h"
#include "Bridge/DialogResources.h"
#include "Bridge/RuntimeDiagnostics.h"
#include "Bridge\\MiniGdi.h"

#include <algorithm>
#include <memory>
#include <mutex>
#include <new>
#include <string>
#include <unordered_map>
#include <vector>

// Keep the private image-list backing store consistent with the other bridge
// shims: MiniGdi is nested under this namespace, while the backing store below
// itself remains translation-unit private.
using namespace Win32Bridge::Bridge;

namespace
{
    constexpr DWORD GuestExitException = 0xE0424242u;

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

    HRESULT WINAPI BridgeDrawThemeBackground(
        HANDLE theme, HDC dc, int part, int state, const RECT* bounds, const RECT* clip)
    {
        if (!theme || !CurrentGuestUsesVisualStyles()) return E_HANDLE;
        if (!dc || !bounds) return E_INVALIDARG;
        // Custom/subclassed controls also call this API on memory DCs. Their
        // buffer is later blitted into the HWND, so the retained control
        // renderer cannot supply this background on their behalf. Use the
        // solid background exposed by GetThemeColor until theme-part artwork
        // is available; never claim success with an untouched black bitmap.
        RECT output = *bounds;
        if (clip)
        {
            output.left = (std::max)(output.left, clip->left);
            output.top = (std::max)(output.top, clip->top);
            output.right = (std::min)(output.right, clip->right);
            output.bottom = (std::min)(output.bottom, clip->bottom);
        }
        if (output.right <= output.left || output.bottom <= output.top) return S_OK;
        const HRESULT result = BridgeFillRect(dc, &output, BridgeGetSysColorBrush(15))
            ? S_OK : E_FAIL;
        RuntimeDiagnostics::RecordChanged(
            L"theme-background:" + std::to_wstring(reinterpret_cast<ULONG_PTR>(dc)),
            L"THEME BACKGROUND: theme=" + std::to_wstring(reinterpret_cast<ULONG_PTR>(theme)) +
            L" dc=" + std::to_wstring(reinterpret_cast<ULONG_PTR>(dc)) +
            L" part=" + std::to_wstring(part) + L" state=" + std::to_wstring(state) +
            L" rect=" + std::to_wstring(output.left) + L"," + std::to_wstring(output.top) +
            L"," + std::to_wstring(output.right) + L"," + std::to_wstring(output.bottom) +
            L" HRESULT=" + std::to_wstring(static_cast<unsigned long>(result)));
        return result;
    }

    HRESULT WINAPI BridgeGetThemeColor(HANDLE theme, int, int, int property, COLORREF* color)
    {
        if (!theme || !color || !CurrentGuestUsesVisualStyles()) return E_HANDLE;
        *color = property == 3803 /* TMT_TEXTCOLOR */ ? RGB(0, 0, 0) : RGB(240, 240, 240);
        return S_OK;
    }

    HANDLE WINAPI BridgeBeginBufferedAnimation(
        HWND, HDC target, const RECT*, int, const void*, const void*, HDC* from, HDC* to)
    {
        if (from) *from = target;
        if (to) *to = target;
        return target ? reinterpret_cast<HANDLE>(target) : nullptr;
    }

    HRESULT WINAPI BridgeEndBufferedAnimation(HANDLE buffer, BOOL)
    {
        return buffer ? S_OK : E_INVALIDARG;
    }

    BOOL WINAPI BridgeBufferedPaintRenderAnimation(HWND, HDC) { return FALSE; }
    HRESULT WINAPI BridgeBufferedPaintStopAllAnimations(HWND) { return S_OK; }
    HRESULT WINAPI BridgeDrawThemeParentBackground(HWND window, HDC, RECT*)
    {
        return window ? S_OK : E_INVALIDARG;
    }
    HRESULT WINAPI BridgeDrawThemeTextEx(
        HANDLE theme, HDC dc, int, int, LPCWSTR text, int count,
        DWORD flags, LPRECT rect, const void*)
    {
        if (!theme || !dc || !text || !rect) return E_INVALIDARG;
        return BridgeDrawTextW(dc, const_cast<LPWSTR>(text), count, rect, flags) ? S_OK : E_FAIL;
    }
    HRESULT WINAPI BridgeEnableThemeDialogTexture(HWND window, DWORD)
    {
        return window ? S_OK : E_INVALIDARG;
    }
    HRESULT WINAPI BridgeGetThemeBackgroundContentRect(
        HANDLE theme, HDC, int, int, const RECT* bounding, RECT* content)
    {
        if (!theme || !bounding || !content) return E_INVALIDARG;
        *content = *bounding;
        return S_OK;
    }
    HRESULT WINAPI BridgeGetThemeFont(
        HANDLE theme, HDC, int, int, int, LOGFONTW* font)
    {
        if (!theme || !font) return E_INVALIDARG;
        ZeroMemory(font, sizeof(*font));
        font->lfHeight = -12;
        font->lfWeight = FW_NORMAL;
        wcscpy_s(font->lfFaceName, L"Segoe UI");
        return S_OK;
    }
    HRESULT WINAPI BridgeGetThemePartSize(
        HANDLE theme, HDC, int, int, const RECT* bounds, int, SIZE* size)
    {
        if (!theme || !size) return E_INVALIDARG;
        size->cx = bounds ? (std::max)(0L, bounds->right - bounds->left) : 16;
        size->cy = bounds ? (std::max)(0L, bounds->bottom - bounds->top) : 16;
        return S_OK;
    }
    HRESULT WINAPI BridgeGetThemeTransitionDuration(
        HANDLE theme, int, int, int, int, DWORD* duration)
    {
        if (!theme || !duration) return E_INVALIDARG;
        *duration = 0;
        return S_OK;
    }

    BOOL WINAPI BridgeMakeDragList(HWND list)
    {
        GuestWindowManager* manager = CurrentGuestWindowManager();
        return manager && manager->IsGuestWindow(list) ? TRUE : FALSE;
    }

    void WINAPI BridgeDrawInsert(HWND, HWND list, int)
    {
        if (GuestWindowManager* manager = CurrentGuestWindowManager())
            manager->InvalidateGuestRect(list, nullptr, FALSE, nullptr);
    }

    int WINAPI BridgeListBoxItemFromPoint(HWND list, POINT point, BOOL)
    {
        GuestWindowManager* manager = CurrentGuestWindowManager();
        if (!manager || !manager->IsGuestWindow(list)) return -1;
        RECT bounds{};
        if (!manager->GetGuestWindowRect(list, &bounds, nullptr)) return -1;
        const int x = point.x - bounds.left;
        const int y = point.y - bounds.top;
        const LRESULT hit = manager->SendGuestMessage(list, 0x01a9, 0,
            MAKELPARAM(static_cast<SHORT>(x), static_cast<SHORT>(y)), nullptr);
        return HIWORD(hit) ? -1 : static_cast<int>(LOWORD(hit));
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

    struct GuestTaskDialogButton final
    {
        int identifier;
        LPCWSTR text;
    };

    struct GuestTaskDialogConfig final
    {
        UINT size;
        HWND parent;
        HINSTANCE instance;
        DWORD flags;
        DWORD commonButtons;
        LPCWSTR windowTitle;
        ULONG_PTR mainIcon;
        LPCWSTR mainInstruction;
        LPCWSTR content;
        UINT buttonCount;
        const GuestTaskDialogButton* buttons;
        int defaultButton;
        UINT radioButtonCount;
        const GuestTaskDialogButton* radioButtons;
        int defaultRadioButton;
        LPCWSTR verificationText;
        LPCWSTR expandedInformation;
        LPCWSTR expandedControlText;
        LPCWSTR collapsedControlText;
        ULONG_PTR footerIcon;
        LPCWSTR footer;
        GuestTaskDialogCallback callback;
        LONG_PTR callbackData;
        UINT width;
    };
    static_assert(sizeof(GuestTaskDialogButton) == 16,
        "TASKDIALOG_BUTTON x64 ABI drifted.");
    static_assert(sizeof(GuestTaskDialogConfig) == 176,
        "TASKDIALOGCONFIG x64 ABI drifted.");

    constexpr DWORD TaskDialogCommonOk = 0x0001;
    constexpr DWORD TaskDialogCommonYes = 0x0002;
    constexpr DWORD TaskDialogCommonNo = 0x0004;
    constexpr DWORD TaskDialogCommonCancel = 0x0008;
    constexpr DWORD TaskDialogCommonRetry = 0x0010;
    constexpr DWORD TaskDialogCommonClose = 0x0020;
    constexpr DWORD TaskDialogVerificationChecked = 0x0100;
    constexpr DWORD TaskDialogNoDefaultRadioButton = 0x4000;

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

    bool ResolveTaskDialogText(HINSTANCE instance, LPCWSTR source,
        std::wstring* result)
    {
        if (!result) return false;
        result->clear();
        if (!source) return true;
        if (reinterpret_cast<ULONG_PTR>(source) <= 0xffff)
        {
            wchar_t buffer[4096] = {};
            const int length = LoadGuestStringResource(instance,
                static_cast<UINT>(reinterpret_cast<ULONG_PTR>(source)),
                buffer, ARRAYSIZE(buffer));
            if (length <= 0) return false;
            result->assign(buffer, buffer + length);
            return true;
        }
        return ReadBoundedGuestString(source, result);
    }

    bool ReadTaskDialogConfig(const GuestTaskDialogConfig* source,
        GuestTaskDialogConfig* result)
    {
        if (!source || !result || reinterpret_cast<ULONG_PTR>(source) <= 0xffff)
            return false;
        __try
        {
            if (source->size != sizeof(*result)) return false;
            *result = *source;
            return true;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return false;
        }
    }

    bool ReadTaskDialogButtonEntry(const GuestTaskDialogButton* source,
        UINT index, GuestTaskDialogButton* result) noexcept
    {
        if (!source || !result) return false;
        __try
        {
            *result = source[index];
            return true;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return false;
        }
    }

    bool ReadTaskDialogButtons(const GuestTaskDialogButton* source, UINT count,
        HINSTANCE instance, std::vector<GuestTaskDialogButtonDescriptor>* buttons,
        std::vector<std::wstring>* labels)
    {
        if (!buttons || !labels || count > 64 || (count && !source)) return false;
        buttons->clear();
        labels->clear();
        buttons->reserve(count);
        labels->reserve(count);
        for (UINT index = 0; index < count; ++index)
        {
            GuestTaskDialogButton entry{};
            if (!ReadTaskDialogButtonEntry(source, index, &entry)) return false;
            std::wstring label;
            if (!ResolveTaskDialogText(instance, entry.text, &label)) return false;
            labels->push_back(std::move(label));
            GuestTaskDialogButtonDescriptor descriptor;
            descriptor.identifier = entry.identifier;
            buttons->push_back(descriptor);
        }
        for (UINT index = 0; index < count; ++index)
            (*buttons)[index].text = (*labels)[index].c_str();
        return true;
    }

    bool WriteTaskDialogIntResult(int* destination, int value) noexcept
    {
        if (!destination) return true;
        __try { *destination = value; return true; }
        __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
    }

    bool WriteTaskDialogBooleanResult(BOOL* destination, BOOL value) noexcept
    {
        if (!destination) return true;
        __try { *destination = value; return true; }
        __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
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

    struct GuestImageInfo final
    {
        HBITMAP image;
        HBITMAP mask;
        int unused1;
        int unused2;
        RECT imageRect;
    };

    struct ImageListDragState final
    {
        GuestImageList imageList = nullptr;
        int image = -1;
        POINT hotspot{};
        POINT position{};
        HWND owner = nullptr;
        bool visible = false;
    };
    ImageListDragState g_dragState;

    using GuestSubclassProc = LRESULT(CALLBACK*)(
        HWND, UINT, WPARAM, LPARAM, UINT_PTR, DWORD_PTR);
    struct SubclassEntry final
    {
        GuestSubclassProc procedure = nullptr;
        UINT_PTR identifier = 0;
        DWORD_PTR referenceData = 0;
        bool removed = false;
        SubclassEntry* next = nullptr;
    };
    struct SubclassWindow final
    {
        HWND window = nullptr;
        GuestAbi::WndProc original = nullptr;
        SubclassEntry* first = nullptr;
        unsigned int running = 0;
        SubclassWindow* next = nullptr;
    };
    // Keep the subclass registry independent from the C++ runtime's heap-backed
    // mutex implementation.  Guest callbacks execute arbitrary PE code and may
    // terminate/re-enter the runtime; an SRW lock has process-lifetime storage
    // and no separately allocated _Mtx_t state that can become stale across a
    // callback boundary.
    SRWLOCK g_subclassLock = SRWLOCK_INIT;
    struct SubclassRegistryLock final
    {
        SubclassRegistryLock() noexcept { ::AcquireSRWLockExclusive(&g_subclassLock); }
        ~SubclassRegistryLock() noexcept { ::ReleaseSRWLockExclusive(&g_subclassLock); }
        SubclassRegistryLock(const SubclassRegistryLock&) = delete;
        SubclassRegistryLock& operator=(const SubclassRegistryLock&) = delete;
    };
    // Keep the subclass stack in the same deliberately simple form used by
    // comctl32: one window record and a linked list of procedures.  This path
    // is entered from arbitrary guest callbacks and must not depend on a C++
    // hash container remaining internally consistent after SEH/re-entry.
    SubclassWindow* g_subclasses = nullptr;

    SubclassWindow* FindSubclassWindowUnlocked(HWND window) noexcept
    {
        for (SubclassWindow* state = g_subclasses; state; state = state->next)
        {
            if (state->window == window) return state;
        }
        return nullptr;
    }

    SubclassWindow** FindSubclassWindowLinkUnlocked(HWND window) noexcept
    {
        SubclassWindow** link = &g_subclasses;
        while (*link && (*link)->window != window) link = &(*link)->next;
        return link;
    }

    void PurgeRemovedSubclassEntriesUnlocked(SubclassWindow* state) noexcept
    {
        if (!state || state->running != 0) return;
        SubclassEntry** link = &state->first;
        while (*link)
        {
            SubclassEntry* entry = *link;
            if (!entry->removed)
            {
                link = &entry->next;
                continue;
            }
            *link = entry->next;
            delete entry;
        }
    }

    void DeleteSubclassWindowUnlocked(SubclassWindow* state) noexcept
    {
        if (!state) return;
        SubclassEntry* entry = state->first;
        while (entry)
        {
            SubclassEntry* next = entry->next;
            delete entry;
            entry = next;
        }
        delete state;
    }

    bool TryWriteSubclassReference(DWORD_PTR* destination, DWORD_PTR value) noexcept
    {
        if (!destination) return true;
        __try
        {
            *destination = value;
            return true;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return false;
        }
    }
    struct SubclassFrame final
    {
        HWND window = nullptr;
        UINT message = 0;
        WPARAM wParam = 0;
        LPARAM lParam = 0;
        // A subclass callback may synchronously destroy its HWND, remove the
        // final subclass, or dispatch WM_NCDESTROY before returning.  The
        // original procedure is therefore part of the active call frame.
        // Snapshot it before entering guest code so DefSubclassProc never has
        // to follow mutable registry storage after a re-entrant callback.
        GuestAbi::WndProc original = nullptr;
        SubclassEntry* next = nullptr;
        SubclassFrame* previous = nullptr;
    };
    thread_local SubclassFrame* g_subclassFrame = nullptr;
    thread_local unsigned int g_subclassDispatchDepth = 0;
    // This is a last-resort ceiling.  GuestWindow and CallWindowProc also
    // detect repeated procedure/message edges, while this limit protects
    // chains made entirely from distinct subclass callbacks.  Keep enough
    // stack in reserve to report the fault and unwind safely.
    constexpr unsigned int MaximumSubclassDispatchDepth = 32;

    LRESULT InvokeSubclassEntry(
        SubclassFrame* frame,
        GuestSubclassProc procedure,
        UINT_PTR identifier,
        DWORD_PTR referenceData)
    {
        __try
        {
            return procedure(frame->window, frame->message, frame->wParam,
                frame->lParam, identifier, referenceData);
        }
        __except (GetExceptionCode() == GuestExitException
            ? EXCEPTION_CONTINUE_SEARCH : EXCEPTION_EXECUTE_HANDLER)
        {
            return 0;
        }
    }

    LRESULT WINAPI BridgeDefSubclassProc(
        HWND window, UINT message, WPARAM wParam, LPARAM lParam);

    LRESULT CALLBACK SubclassDispatcher(
        HWND window, UINT message, WPARAM wParam, LPARAM lParam)
    {
        // A malformed subclass chain must not be allowed to consume the host
        // thread's stack.  Windows keeps the subclass chain separately from
        // GWLP_WNDPROC and ultimately calls the saved original procedure.  A
        // guest that accidentally installs the dispatcher as that original
        // procedure would otherwise recurse until ntdll faults while unwinding.
        if (g_subclassDispatchDepth >= MaximumSubclassDispatchDepth)
        {
            RuntimeDiagnostics::Record(
                L"CONTROL SUBCLASS: recursive dispatch was stopped for handle " +
                std::to_wstring(reinterpret_cast<ULONG_PTR>(window)) + L".");
            return BridgeDefWindowProcW(window, message, wParam, lParam);
        }

        SubclassFrame frame;
        frame.window = window;
        frame.message = message;
        frame.wParam = wParam;
        frame.lParam = lParam;
        {
            SubclassRegistryLock guard;
            SubclassWindow* state = FindSubclassWindowUnlocked(window);
            if (!state)
                return BridgeDefWindowProcW(window, message, wParam, lParam);
            frame.original = state->original;
            frame.next = state->first;
            ++state->running;
        }
        frame.previous = g_subclassFrame;
        g_subclassFrame = &frame;
        ++g_subclassDispatchDepth;
        const LRESULT result = BridgeDefSubclassProc(window, message, wParam, lParam);
        --g_subclassDispatchDepth;
        g_subclassFrame = frame.previous;

        GuestAbi::WndProc restore = nullptr;
        bool restoreProcedure = false;
        {
            SubclassRegistryLock guard;
            const bool destroying = message == 0x0082; // WM_NCDESTROY
            SubclassWindow** stateLink = FindSubclassWindowLinkUnlocked(window);
            SubclassWindow* state = *stateLink;
            if (state)
            {
                if (state->running != 0)
                    --state->running;
                if (destroying)
                {
                    for (SubclassEntry* entry = state->first; entry; entry = entry->next)
                        entry->removed = true;
                }

                PurgeRemovedSubclassEntriesUnlocked(state);
                if (!state->first && state->running == 0)
                {
                    restore = state->original;
                    restoreProcedure = !destroying;
                    *stateLink = state->next;
                    DeleteSubclassWindowUnlocked(state);
                }
            }
        }
        if (restoreProcedure &&
            reinterpret_cast<GuestAbi::WndProc>(BridgeGetWindowLongPtrW(window, -4)) ==
                &SubclassDispatcher)
        {
            BridgeSetWindowLongPtrW(window, -4, reinterpret_cast<LONG_PTR>(restore));
        }
        return result;
    }

    BOOL WINAPI BridgeSetWindowSubclass(
        HWND window, GuestSubclassProc procedure, UINT_PTR identifier, DWORD_PTR referenceData)
    {
        if (!window || !procedure) return FALSE;
        GuestAbi::WndProc rollback = nullptr;
        bool createdWindowState = false;
        bool updated = false;
        bool registered = false;
        {
            SubclassRegistryLock guard;
            SubclassWindow* state = FindSubclassWindowUnlocked(window);
            if (!state)
            {
                state = new (std::nothrow) SubclassWindow();
                if (!state) return FALSE;
                state->window = window;
                state->original = reinterpret_cast<GuestAbi::WndProc>(
                    BridgeSetWindowLongPtrW(window, -4,
                        reinterpret_cast<LONG_PTR>(&SubclassDispatcher)));
                if (!state->original)
                {
                    delete state;
                    RuntimeDiagnostics::Record(
                        L"CONTROL SUBCLASS FAILED: the original procedure for handle " +
                        std::to_wstring(reinterpret_cast<ULONG_PTR>(window)) +
                        L" could not be obtained.");
                    return FALSE;
                }
                rollback = state->original;
                state->next = g_subclasses;
                g_subclasses = state;
                createdWindowState = true;
            }

            for (SubclassEntry* entry = state->first; entry; entry = entry->next)
            {
                if (!entry->removed && entry->procedure == procedure &&
                    entry->identifier == identifier)
                {
                    entry->referenceData = referenceData;
                    updated = true;
                    break;
                }
            }

            if (!updated)
            {
                // COMCTL32 inserts new procedures at the head of the chain.
                SubclassEntry* entry = new (std::nothrow) SubclassEntry();
                if (entry)
                {
                    entry->procedure = procedure;
                    entry->identifier = identifier;
                    entry->referenceData = referenceData;
                    entry->next = state->first;
                    state->first = entry;
                    registered = true;
                }
                else if (createdWindowState)
                {
                    g_subclasses = state->next;
                    DeleteSubclassWindowUnlocked(state);
                }
            }
        }

        if (!updated && !registered)
        {
            if (createdWindowState && rollback)
                BridgeSetWindowLongPtrW(window, -4, reinterpret_cast<LONG_PTR>(rollback));
            RuntimeDiagnostics::Record(
                L"CONTROL SUBCLASS FAILED: allocation failed for handle " +
                std::to_wstring(reinterpret_cast<ULONG_PTR>(window)) + L".");
            return FALSE;
        }

        RuntimeDiagnostics::Record(
            L"CONTROL SUBCLASS: " + std::wstring(updated ? L"updated" : L"registered") +
            L" handle " + std::to_wstring(reinterpret_cast<ULONG_PTR>(window)) +
            L", identifier " + std::to_wstring(identifier) + L", procedure " +
            std::to_wstring(reinterpret_cast<ULONG_PTR>(procedure)) + L".");
        return TRUE;
    }

    BOOL WINAPI BridgeGetWindowSubclass(
        HWND window, GuestSubclassProc procedure, UINT_PTR identifier, DWORD_PTR* referenceData)
    {
        if (!TryWriteSubclassReference(referenceData, 0)) return FALSE;
        if (!window || !procedure) return FALSE;
        SubclassRegistryLock guard;
        const SubclassWindow* state = FindSubclassWindowUnlocked(window);
        if (!state) return FALSE;
        for (const SubclassEntry* entry = state->first; entry; entry = entry->next)
        {
            if (!entry->removed && entry->procedure == procedure &&
                entry->identifier == identifier)
            {
                return TryWriteSubclassReference(referenceData, entry->referenceData)
                    ? TRUE : FALSE;
            }
        }
        return FALSE;
    }

    BOOL WINAPI BridgeRemoveWindowSubclass(
        HWND window, GuestSubclassProc procedure, UINT_PTR identifier)
    {
        GuestAbi::WndProc restore = nullptr;
        bool removed = false;
        {
            SubclassRegistryLock guard;
            SubclassWindow** stateLink = FindSubclassWindowLinkUnlocked(window);
            SubclassWindow* state = *stateLink;
            if (!state) return FALSE;
            SubclassEntry** link = &state->first;
            while (*link)
            {
                SubclassEntry* entry = *link;
                if (!entry->removed && entry->procedure == procedure &&
                    entry->identifier == identifier)
                {
                    if (state->running != 0)
                        entry->removed = true;
                    else
                    {
                        *link = entry->next;
                        delete entry;
                    }
                    removed = true;
                    break;
                }
                link = &entry->next;
            }
            PurgeRemovedSubclassEntriesUnlocked(state);
            if (!state->first && state->running == 0)
            {
                restore = state->original;
                *stateLink = state->next;
                DeleteSubclassWindowUnlocked(state);
            }
        }

        if (!removed)
            return FALSE;
        if (restore &&
            reinterpret_cast<GuestAbi::WndProc>(BridgeGetWindowLongPtrW(window, -4)) ==
                &SubclassDispatcher)
        {
            BridgeSetWindowLongPtrW(window, -4, reinterpret_cast<LONG_PTR>(restore));
        }
        return TRUE;
    }

    LRESULT WINAPI BridgeDefSubclassProc(
        HWND window, UINT message, WPARAM wParam, LPARAM lParam)
    {
        SubclassFrame* frame = g_subclassFrame;
        if (!frame || frame->window != window)
            return BridgeDefWindowProcW(window, message, wParam, lParam);
        frame->message = message;
        frame->wParam = wParam;
        frame->lParam = lParam;

        GuestSubclassProc procedure = nullptr;
        UINT_PTR identifier = 0;
        DWORD_PTR referenceData = 0;
        {
            SubclassRegistryLock guard;
            while (frame->next && frame->next->removed)
                frame->next = frame->next->next;
            if (frame->next)
            {
                const SubclassEntry* entry = frame->next;
                frame->next = entry->next;
                procedure = entry->procedure;
                identifier = entry->identifier;
                referenceData = entry->referenceData;
            }
        }
        if (procedure)
            return InvokeSubclassEntry(frame, procedure, identifier, referenceData);

        const GuestAbi::WndProc original = frame->original;
        return original && original != &SubclassDispatcher
            ? BridgeCallWindowProcW(original, window, message, wParam, lParam)
            : BridgeDefWindowProcW(window, message, wParam, lParam);
    }

    int WINAPI BridgeDrawShadowText(HDC dc, LPCWSTR text, UINT count, RECT* rect,
        DWORD format, COLORREF textColor, COLORREF shadowColor, int offsetX, int offsetY)
    {
        if (!dc || !text || !rect) return 0;
        RECT shadow = *rect;
        shadow.left += offsetX;
        shadow.right += offsetX;
        shadow.top += offsetY;
        shadow.bottom += offsetY;
        const COLORREF previous = BridgeSetTextColor(dc, shadowColor);
        BridgeDrawTextW(dc, const_cast<LPWSTR>(text), static_cast<int>(count), &shadow, format);
        BridgeSetTextColor(dc, textColor);
        const int result = BridgeDrawTextW(
            dc, const_cast<LPWSTR>(text), static_cast<int>(count), rect, format);
        BridgeSetTextColor(dc, previous);
        return result;
    }

    bool TryWriteGuestIcon(HICON* destination, HICON value) noexcept
    {
        if (!destination || reinterpret_cast<ULONG_PTR>(destination) <= 0xffff)
            return false;
        __try
        {
            *destination = value;
            return true;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return false;
        }
    }

    HRESULT WINAPI BridgeLoadIconWithScaleDown(
        HINSTANCE instance, LPCWSTR name, int width, int height, HICON* icon)
    {
        // COMCTL32 v6 ordinal 381.  The output pointer belongs to the guest,
        // so clear it defensively before validating the remaining arguments.
        if (!TryWriteGuestIcon(icon, nullptr))
            return E_INVALIDARG;
        if (!name)
            return E_INVALIDARG;

        const bool integerResource = reinterpret_cast<ULONG_PTR>(name) <= 0xffff;
        const UINT flags = (instance || integerResource) ? 0u : LR_LOADFROMFILE;
        const HICON loaded = reinterpret_cast<HICON>(BridgeLoadImageW(
            instance, name, IMAGE_ICON, width, height, flags));
        if (!loaded)
        {
            DWORD error = BridgeGetLastError();
            if (error == ERROR_SUCCESS) error = ERROR_RESOURCE_NAME_NOT_FOUND;
            return HRESULT_FROM_WIN32(error);
        }

        if (!TryWriteGuestIcon(icon, loaded))
        {
            BridgeDestroyIcon(loaded);
            return E_INVALIDARG;
        }
        return S_OK;
    }

    HRESULT WINAPI BridgeLoadIconMetric(
        HINSTANCE instance, LPCWSTR name, int size, HICON* icon)
    {
        // LIM_SMALL and LIM_LARGE are part of the comctl32 ABI.  Keep their
        // numeric values local so this UWP target does not depend on desktop
        // commctrl headers exposing them.
        constexpr int LoadIconMetricSmall = 0;
        constexpr int LoadIconMetricLarge = 1;

        int width = 0;
        int height = 0;
        if (size == LoadIconMetricSmall)
        {
            width = BridgeGetSystemMetrics(SM_CXSMICON);
            height = BridgeGetSystemMetrics(SM_CYSMICON);
        }
        else if (size == LoadIconMetricLarge)
        {
            width = BridgeGetSystemMetrics(SM_CXICON);
            height = BridgeGetSystemMetrics(SM_CYICON);
        }
        else
        {
            if (!TryWriteGuestIcon(icon, nullptr)) return E_INVALIDARG;
            return E_INVALIDARG;
        }

        return BridgeLoadIconWithScaleDown(instance, name, width, height, icon);
    }

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

void Win32Bridge::Bridge::ResetGuestCommonControlState()
{
    // No guest callback is made here.  A failed PE may have already unloaded
    // the module that owned a subclass procedure, so cleanup must only discard
    // bridge-owned bookkeeping.
    {
        SubclassRegistryLock guard;
        SubclassWindow* state = g_subclasses;
        g_subclasses = nullptr;
        while (state)
        {
            SubclassWindow* next = state->next;
            DeleteSubclassWindowUnlocked(state);
            state = next;
        }
    }
    {
        std::lock_guard<std::mutex> guard(g_imageListsLock);
        g_imageLists.clear();
        g_nextImageList = 0x60000000;
        g_dragState = ImageListDragState{};
    }
    g_subclassFrame = nullptr;
    g_subclassDispatchDepth = 0;
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
    RuntimeDiagnostics::Record(
        L"IMAGELIST: created " + std::to_wstring(width) + L"x" +
        std::to_wstring(height) + L" list " + std::to_wstring(token) + L".");
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
    if (!CopyGuestIconPixels(icon, &source) || source.Empty())
    {
        RuntimeDiagnostics::Record(
            L"IMAGELIST: rejected an invalid icon for list " +
            std::to_wstring(reinterpret_cast<ULONG_PTR>(imageList)) + L".");
        return -1;
    }
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

BOOL WINAPI Win32Bridge::Bridge::BridgeImageListRemove(GuestImageList imageList, int index)
{
    std::lock_guard<std::mutex> guard(g_imageListsLock);
    const auto found = g_imageLists.find(reinterpret_cast<ULONG_PTR>(imageList));
    if (found == g_imageLists.end()) return FALSE;
    auto& images = found->second.images;
    if (index == -1)
    {
        images.clear();
        return TRUE;
    }
    if (index < 0 || static_cast<size_t>(index) >= images.size()) return FALSE;
    images.erase(images.begin() + index);
    return TRUE;
}

BOOL WINAPI Win32Bridge::Bridge::BridgeImageListSetIconSize(
    GuestImageList imageList, int width, int height)
{
    if (width <= 0 || height <= 0 || width > 256 || height > 256) return FALSE;
    std::lock_guard<std::mutex> guard(g_imageListsLock);
    const auto found = g_imageLists.find(reinterpret_cast<ULONG_PTR>(imageList));
    if (found == g_imageLists.end()) return FALSE;
    found->second.width = width;
    found->second.height = height;
    // Native ImageList_SetIconSize removes existing images.
    found->second.images.clear();
    return TRUE;
}

BOOL WINAPI Win32Bridge::Bridge::BridgeImageListGetIconSize(
    GuestImageList imageList, int* width, int* height)
{
    if (!width || !height) return FALSE;
    std::lock_guard<std::mutex> guard(g_imageListsLock);
    const auto found = g_imageLists.find(reinterpret_cast<ULONG_PTR>(imageList));
    if (found == g_imageLists.end()) return FALSE;
    *width = found->second.width;
    *height = found->second.height;
    return TRUE;
}

HICON WINAPI Win32Bridge::Bridge::BridgeImageListGetIcon(
    GuestImageList imageList, int index, UINT)
{
    MiniGdi::Surface image;
    return CopyGuestImageListImage(imageList, index, &image)
        ? StoreGuestIconPixels(image) : nullptr;
}

BOOL WINAPI Win32Bridge::Bridge::BridgeImageListDraw(
    GuestImageList imageList, int index, HDC dc, int x, int y, UINT)
{
    MiniGdi::Surface image;
    if (!CopyGuestImageListImage(imageList, index, &image)) return FALSE;
    GuestWindowManager* manager = CurrentGuestWindowManager();
    MiniGdi::Surface* destination = manager ? manager->Gdi().GetSurface(
        static_cast<MiniGdi::DcHandle>(reinterpret_cast<ULONG_PTR>(dc))) : nullptr;
    if (!destination) return FALSE;
    return MiniGdi::CopyRect(*destination, MiniGdi::Point{ x, y }, image,
        MiniGdi::Rect{ 0, 0, image.Width(), image.Height() }) ? TRUE : FALSE;
}

BOOL WINAPI Win32Bridge::Bridge::BridgeImageListGetImageInfo(
    GuestImageList imageList, int index, void* information)
{
    if (!information) return FALSE;
    MiniGdi::Surface image;
    if (!CopyGuestImageListImage(imageList, index, &image)) return FALSE;
    GuestWindowManager* manager = CurrentGuestWindowManager();
    if (!manager) return FALSE;
    const MiniGdi::BitmapHandle bitmap = manager->Gdi().CreateBitmap(
        image.Width(), image.Height(), MiniGdi::Transparent);
    MiniGdi::Surface* copy = manager->Gdi().GetBitmapSurface(bitmap);
    if (!bitmap || !copy) return FALSE;
    *copy = image;
    GuestImageInfo result{};
    result.image = reinterpret_cast<HBITMAP>(static_cast<ULONG_PTR>(bitmap));
    result.imageRect = RECT{ 0, 0, image.Width(), image.Height() };
    *static_cast<GuestImageInfo*>(information) = result;
    return TRUE;
}

BOOL WINAPI BridgeImageListBeginDrag(GuestImageList imageList, int index, int x, int y)
{
    MiniGdi::Surface ignored;
    if (!CopyGuestImageListImage(imageList, index, &ignored)) return FALSE;
    std::lock_guard<std::mutex> guard(g_imageListsLock);
    g_dragState = ImageListDragState{};
    g_dragState.imageList = imageList;
    g_dragState.image = index;
    g_dragState.hotspot = POINT{ x, y };
    g_dragState.visible = true;
    return TRUE;
}

void WINAPI BridgeImageListEndDrag()
{
    std::lock_guard<std::mutex> guard(g_imageListsLock);
    g_dragState = ImageListDragState{};
}

BOOL WINAPI BridgeImageListDragEnter(HWND owner, int x, int y)
{
    std::lock_guard<std::mutex> guard(g_imageListsLock);
    if (!g_dragState.imageList) return FALSE;
    g_dragState.owner = owner;
    g_dragState.position = POINT{ x, y };
    return TRUE;
}

BOOL WINAPI BridgeImageListDragMove(int x, int y)
{
    std::lock_guard<std::mutex> guard(g_imageListsLock);
    if (!g_dragState.imageList) return FALSE;
    g_dragState.position = POINT{ x, y };
    return TRUE;
}

BOOL WINAPI BridgeImageListDragShowNolock(BOOL visible)
{
    std::lock_guard<std::mutex> guard(g_imageListsLock);
    if (!g_dragState.imageList) return FALSE;
    g_dragState.visible = visible != FALSE;
    return TRUE;
}

struct GuestTrackMouseEvent final
{
    DWORD cbSize;
    DWORD dwFlags;
    HWND hwndTrack;
    DWORD dwHoverTime;
};

BOOL WINAPI BridgeTrackMouseEvent(GuestTrackMouseEvent* event)
{
    if (!event || event->cbSize != sizeof(*event) || !event->hwndTrack) return FALSE;
    // Mouse leave/hover generation is owned by GuestWindow's pointer routing.
    return TRUE;
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

HRESULT WINAPI BridgeTaskDialogIndirect(const GuestTaskDialogConfig* source,
    int* selectedButton, int* selectedRadioButton, BOOL* verificationChecked)
{
    GuestTaskDialogConfig config{};
    if (!ReadTaskDialogConfig(source, &config)) return E_INVALIDARG;

    std::wstring title;
    std::wstring instruction;
    std::wstring content;
    std::wstring verification;
    std::wstring expanded;
    std::wstring footer;
    if (!ResolveTaskDialogText(config.instance, config.windowTitle, &title) ||
        !ResolveTaskDialogText(config.instance, config.mainInstruction, &instruction) ||
        !ResolveTaskDialogText(config.instance, config.content, &content) ||
        !ResolveTaskDialogText(config.instance, config.verificationText, &verification) ||
        !ResolveTaskDialogText(config.instance, config.expandedInformation, &expanded) ||
        !ResolveTaskDialogText(config.instance, config.footer, &footer))
    {
        return HRESULT_FROM_WIN32(ERROR_RESOURCE_NAME_NOT_FOUND);
    }
    if (!expanded.empty())
    {
        if (!content.empty()) content.append(L"\n\n");
        content.append(expanded);
    }
    if (!footer.empty())
    {
        if (!content.empty()) content.append(L"\n\n");
        content.append(footer);
    }

    std::vector<GuestTaskDialogButtonDescriptor> buttons;
    std::vector<std::wstring> buttonLabels;
    if (!ReadTaskDialogButtons(config.buttons, config.buttonCount,
        config.instance, &buttons, &buttonLabels)) return E_INVALIDARG;
    buttons.reserve(static_cast<size_t>(config.buttonCount) + 6);
    buttonLabels.reserve(static_cast<size_t>(config.buttonCount) + 6);
    auto containsButton = [&buttons](int identifier)
    {
        return std::any_of(buttons.begin(), buttons.end(),
            [identifier](const GuestTaskDialogButtonDescriptor& candidate)
            { return candidate.identifier == identifier; });
    };
    auto addCommonButton = [&](DWORD flag, int identifier, LPCWSTR label)
    {
        if ((config.commonButtons & flag) == 0 || containsButton(identifier)) return;
        buttonLabels.emplace_back(label);
        GuestTaskDialogButtonDescriptor button;
        button.identifier = identifier;
        buttons.push_back(button);
    };
    addCommonButton(TaskDialogCommonOk, IDOK, L"OK");
    addCommonButton(TaskDialogCommonYes, IDYES, L"Yes");
    addCommonButton(TaskDialogCommonNo, IDNO, L"No");
    addCommonButton(TaskDialogCommonCancel, IDCANCEL, L"Cancel");
    addCommonButton(TaskDialogCommonRetry, IDRETRY, L"Retry");
    addCommonButton(TaskDialogCommonClose, 8 /* IDCLOSE */, L"Close");
    if (buttons.empty())
    {
        buttonLabels.emplace_back(L"OK");
        GuestTaskDialogButtonDescriptor button;
        button.identifier = IDOK;
        buttons.push_back(button);
    }
    for (size_t index = 0; index < buttons.size(); ++index)
        buttons[index].text = buttonLabels[index].c_str();

    std::vector<GuestTaskDialogButtonDescriptor> radioButtons;
    std::vector<std::wstring> radioLabels;
    if (!ReadTaskDialogButtons(config.radioButtons, config.radioButtonCount,
        config.instance, &radioButtons, &radioLabels)) return E_INVALIDARG;

    GuestTaskDialogDescriptor descriptor;
    descriptor.owner = config.parent;
    descriptor.instance = config.instance;
    descriptor.title = title.empty() ? nullptr : title.c_str();
    descriptor.mainInstruction = instruction.empty() ? nullptr : instruction.c_str();
    descriptor.content = content.empty() ? nullptr : content.c_str();
    descriptor.buttons = buttons.data();
    descriptor.buttonCount = static_cast<UINT>(buttons.size());
    descriptor.defaultButton = config.defaultButton;
    descriptor.radioButtons = radioButtons.empty() ? nullptr : radioButtons.data();
    descriptor.radioButtonCount = static_cast<UINT>(radioButtons.size());
    descriptor.defaultRadioButton = config.defaultRadioButton;
    if ((config.flags & TaskDialogNoDefaultRadioButton) != 0)
    {
        descriptor.defaultRadioButton = 0;
    }
    else if (!radioButtons.empty() &&
        std::none_of(radioButtons.begin(), radioButtons.end(),
            [&descriptor](const GuestTaskDialogButtonDescriptor& candidate)
            { return candidate.identifier == descriptor.defaultRadioButton; }))
    {
        descriptor.defaultRadioButton = radioButtons[0].identifier;
    }
    descriptor.verificationText = verification.empty() ? nullptr : verification.c_str();
    descriptor.verificationChecked =
        (config.flags & TaskDialogVerificationChecked) != 0;
    descriptor.callback = config.callback;
    descriptor.callbackData = config.callbackData;

    int button = 0;
    int radio = 0;
    BOOL checked = FALSE;
    const HRESULT result = ShowGuestTaskDialog(descriptor, &button, &radio, &checked);
    if (FAILED(result)) return result;
    if (!WriteTaskDialogIntResult(selectedButton, button) ||
        !WriteTaskDialogIntResult(selectedRadioButton, radio) ||
        !WriteTaskDialogBooleanResult(verificationChecked, checked))
        return E_INVALIDARG;
    return S_OK;
}

HRESULT WINAPI BridgeTaskDialog(HWND parent, HINSTANCE instance,
    LPCWSTR windowTitle, LPCWSTR mainInstruction, LPCWSTR content,
    DWORD commonButtons, LPCWSTR, int* selectedButton)
{
    GuestTaskDialogConfig config{};
    config.size = sizeof(config);
    config.parent = parent;
    config.instance = instance;
    config.windowTitle = windowTitle;
    config.mainInstruction = mainInstruction;
    config.content = content;
    config.commonButtons = commonButtons;
    return BridgeTaskDialogIndirect(&config, selectedButton, nullptr, nullptr);
}

Win32Bridge::Bridge::ImportResolution Win32Bridge::Bridge::ResolveCommonControlsImport(const ImportedSymbol& symbol)
{
    auto resolution = CompatibilityCatalog::Resolve(symbol);
    if (_wcsicmp(symbol.library.c_str(), L"comctl32.dll") == 0 &&
        symbol.importedByOrdinal && (symbol.ordinal == 17 || symbol.ordinal == 345 ||
            (symbol.ordinal >= 380 && symbol.ordinal <= 381) ||
            (symbol.ordinal >= 410 && symbol.ordinal <= 413)))
    {
        switch (symbol.ordinal)
        {
        case 17: resolution.targetAddress = reinterpret_cast<ULONGLONG>(&BridgeInitCommonControls); break;
        case 380: resolution.targetAddress = reinterpret_cast<ULONGLONG>(&BridgeLoadIconMetric); break;
        case 381: resolution.targetAddress = reinterpret_cast<ULONGLONG>(&BridgeLoadIconWithScaleDown); break;
        case 410: resolution.targetAddress = reinterpret_cast<ULONGLONG>(&BridgeSetWindowSubclass); break;
        case 411: resolution.targetAddress = reinterpret_cast<ULONGLONG>(&BridgeGetWindowSubclass); break;
        case 412: resolution.targetAddress = reinterpret_cast<ULONGLONG>(&BridgeRemoveWindowSubclass); break;
        case 413: resolution.targetAddress = reinterpret_cast<ULONGLONG>(&BridgeDefSubclassProc); break;
        default: resolution.targetAddress = reinterpret_cast<ULONGLONG>(&BridgeTaskDialogIndirect); break;
        }
        resolution.disposition = ImportDisposition::NeedsBridge;
        if (symbol.ordinal == 345)
            resolution.note = L"COMCTL32 v6 TaskDialogIndirect: virtual modal dialog with custom/common buttons and callbacks.";
        else if (symbol.ordinal == 380 || symbol.ordinal == 381)
            resolution.note = L"COMCTL32 v6 icon loader: resolves resource/file icons through the guest image pipeline.";
        else if (symbol.ordinal >= 410 && symbol.ordinal <= 413)
            resolution.note = L"COMCTL32 subclass chain with re-entrant DefSubclassProc semantics.";
        else
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
        else if (_wcsicmp(symbol.name.c_str(), L"beginbufferedanimation") == 0) resolution.targetAddress = reinterpret_cast<ULONGLONG>(&BridgeBeginBufferedAnimation);
        else if (_wcsicmp(symbol.name.c_str(), L"endbufferedanimation") == 0) resolution.targetAddress = reinterpret_cast<ULONGLONG>(&BridgeEndBufferedAnimation);
        else if (_wcsicmp(symbol.name.c_str(), L"bufferedpaintrenderanimation") == 0) resolution.targetAddress = reinterpret_cast<ULONGLONG>(&BridgeBufferedPaintRenderAnimation);
        else if (_wcsicmp(symbol.name.c_str(), L"bufferedpaintstopallanimations") == 0) resolution.targetAddress = reinterpret_cast<ULONGLONG>(&BridgeBufferedPaintStopAllAnimations);
        else if (_wcsicmp(symbol.name.c_str(), L"drawthemeparentbackground") == 0) resolution.targetAddress = reinterpret_cast<ULONGLONG>(&BridgeDrawThemeParentBackground);
        else if (_wcsicmp(symbol.name.c_str(), L"drawthemetextex") == 0) resolution.targetAddress = reinterpret_cast<ULONGLONG>(&BridgeDrawThemeTextEx);
        else if (_wcsicmp(symbol.name.c_str(), L"enablethemedialogtexture") == 0) resolution.targetAddress = reinterpret_cast<ULONGLONG>(&BridgeEnableThemeDialogTexture);
        else if (_wcsicmp(symbol.name.c_str(), L"getthemebackgroundcontentrect") == 0) resolution.targetAddress = reinterpret_cast<ULONGLONG>(&BridgeGetThemeBackgroundContentRect);
        else if (_wcsicmp(symbol.name.c_str(), L"getthemefont") == 0) resolution.targetAddress = reinterpret_cast<ULONGLONG>(&BridgeGetThemeFont);
        else if (_wcsicmp(symbol.name.c_str(), L"getthemepartsize") == 0) resolution.targetAddress = reinterpret_cast<ULONGLONG>(&BridgeGetThemePartSize);
        else if (_wcsicmp(symbol.name.c_str(), L"getthemetransitionduration") == 0) resolution.targetAddress = reinterpret_cast<ULONGLONG>(&BridgeGetThemeTransitionDuration);
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
        else if (_wcsicmp(symbol.name.c_str(), L"imagelist_remove") == 0) resolution.targetAddress = reinterpret_cast<ULONGLONG>(&BridgeImageListRemove);
        else if (_wcsicmp(symbol.name.c_str(), L"imagelist_seticonsize") == 0) resolution.targetAddress = reinterpret_cast<ULONGLONG>(&BridgeImageListSetIconSize);
        else if (_wcsicmp(symbol.name.c_str(), L"imagelist_geticonsize") == 0) resolution.targetAddress = reinterpret_cast<ULONGLONG>(&BridgeImageListGetIconSize);
        else if (_wcsicmp(symbol.name.c_str(), L"imagelist_geticon") == 0) resolution.targetAddress = reinterpret_cast<ULONGLONG>(&BridgeImageListGetIcon);
        else if (_wcsicmp(symbol.name.c_str(), L"imagelist_draw") == 0) resolution.targetAddress = reinterpret_cast<ULONGLONG>(&BridgeImageListDraw);
        else if (_wcsicmp(symbol.name.c_str(), L"imagelist_getimageinfo") == 0) resolution.targetAddress = reinterpret_cast<ULONGLONG>(&BridgeImageListGetImageInfo);
        else if (_wcsicmp(symbol.name.c_str(), L"imagelist_begindrag") == 0) resolution.targetAddress = reinterpret_cast<ULONGLONG>(&BridgeImageListBeginDrag);
        else if (_wcsicmp(symbol.name.c_str(), L"imagelist_enddrag") == 0) resolution.targetAddress = reinterpret_cast<ULONGLONG>(&BridgeImageListEndDrag);
        else if (_wcsicmp(symbol.name.c_str(), L"imagelist_dragenter") == 0) resolution.targetAddress = reinterpret_cast<ULONGLONG>(&BridgeImageListDragEnter);
        else if (_wcsicmp(symbol.name.c_str(), L"imagelist_dragmove") == 0) resolution.targetAddress = reinterpret_cast<ULONGLONG>(&BridgeImageListDragMove);
        else if (_wcsicmp(symbol.name.c_str(), L"imagelist_dragshownolock") == 0) resolution.targetAddress = reinterpret_cast<ULONGLONG>(&BridgeImageListDragShowNolock);
        else if (_wcsicmp(symbol.name.c_str(), L"_trackmouseevent") == 0) resolution.targetAddress = reinterpret_cast<ULONGLONG>(&BridgeTrackMouseEvent);
        else if (_wcsicmp(symbol.name.c_str(), L"createtoolbarex") == 0) resolution.targetAddress = reinterpret_cast<ULONGLONG>(&BridgeCreateToolbarEx);
        else if (_wcsicmp(symbol.name.c_str(), L"createstatuswindoww") == 0) resolution.targetAddress = reinterpret_cast<ULONGLONG>(&BridgeCreateStatusWindowW);
        else if (_wcsicmp(symbol.name.c_str(), L"makedraglist") == 0) resolution.targetAddress = reinterpret_cast<ULONGLONG>(&BridgeMakeDragList);
        else if (_wcsicmp(symbol.name.c_str(), L"drawinsert") == 0) resolution.targetAddress = reinterpret_cast<ULONGLONG>(&BridgeDrawInsert);
        else if (_wcsicmp(symbol.name.c_str(), L"lbitemfrompt") == 0) resolution.targetAddress = reinterpret_cast<ULONGLONG>(&BridgeListBoxItemFromPoint);
        else if (_wcsicmp(symbol.name.c_str(), L"propertysheetw") == 0) resolution.targetAddress = reinterpret_cast<ULONGLONG>(&BridgePropertySheetW);
        else if (_wcsicmp(symbol.name.c_str(), L"createpropertysheetpagew") == 0) resolution.targetAddress = reinterpret_cast<ULONGLONG>(&BridgeCreatePropertySheetPageW);
        else if (_wcsicmp(symbol.name.c_str(), L"dllgetversion") == 0) resolution.targetAddress = reinterpret_cast<ULONGLONG>(&BridgeDllGetVersion);
        else if (_wcsicmp(symbol.name.c_str(), L"drawshadowtext") == 0) resolution.targetAddress = reinterpret_cast<ULONGLONG>(&BridgeDrawShadowText);
        else if (_wcsicmp(symbol.name.c_str(), L"loadiconmetric") == 0) resolution.targetAddress = reinterpret_cast<ULONGLONG>(&BridgeLoadIconMetric);
        else if (_wcsicmp(symbol.name.c_str(), L"loadiconwithscaledown") == 0) resolution.targetAddress = reinterpret_cast<ULONGLONG>(&BridgeLoadIconWithScaleDown);
        else if (_wcsicmp(symbol.name.c_str(), L"taskdialog") == 0) resolution.targetAddress = reinterpret_cast<ULONGLONG>(&BridgeTaskDialog);
        else if (_wcsicmp(symbol.name.c_str(), L"taskdialogindirect") == 0) resolution.targetAddress = reinterpret_cast<ULONGLONG>(&BridgeTaskDialogIndirect);
        if (resolution.targetAddress)
        {
            resolution.disposition = ImportDisposition::NeedsBridge;
            resolution.note = L"Common-controls adapter: guest-owned image lists and virtual toolbar/status controls.";
        }
    }
    return resolution;
}
