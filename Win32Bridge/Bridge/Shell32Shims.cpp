#include "pch.h"
#include "Bridge/Shell32Shims.h"
#include "Bridge/Kernel32Shims.h"
#include "Bridge/CommonControlsShims.h"
#include "Bridge/DialogResources.h"
#include "Bridge/GuestResources.h"
#include "Bridge/GuestStorage.h"
#include "Bridge/OleShims.h"
#include "Bridge/PeMapper.h"
#include "Bridge/RuntimeDiagnostics.h"
#include "Bridge/User32Shims.h"
#include "Bridge/Win32Shims.h"

#include <algorithm>
#include <atomic>
#include <climits>
#include <cstddef>
#include <new>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

namespace
{
    // Guest ABI layout of SHFILEOPSTRUCTW. It deliberately avoids the
    // desktop-only shell header while retaining the x64 field ordering.
    struct GuestShellFileOperationW final
    {
        HWND owner;
        UINT operation;
        LPCWSTR from;
        LPCWSTR to;
        WORD flags;
        BOOL anyOperationsAborted;
        PVOID nameMappings;
        LPCWSTR progressTitle;
    };
    static_assert(sizeof(GuestShellFileOperationW) == 56, "SHFILEOPSTRUCTW x64 guest ABI mismatch");

    struct GuestShellFileInfoW final
    {
        HICON icon;
        int iconIndex;
        DWORD attributes;
        wchar_t displayName[MAX_PATH];
        wchar_t typeName[80];
    };
    static_assert(sizeof(GuestShellFileInfoW) == 696, "SHFILEINFOW x64 guest ABI mismatch");

    constexpr UINT ShellFileMove = 0x0001;
    constexpr UINT ShellFileCopy = 0x0002;
    constexpr UINT ShellFileDelete = 0x0003;
    constexpr UINT ShellFileRename = 0x0004;
    constexpr WORD VirtualPidlMagic = 0x4257; // "WB", bridge-owned PIDL.
    constexpr int CsidlDesktop = 0x0000;
    constexpr int CsidlPersonal = 0x0005;
    constexpr int CsidlDrives = 0x0011;
    constexpr int CsidlAppData = 0x001a;
    constexpr int CsidlLocalAppData = 0x001c;
    constexpr UINT ShgfiIcon = 0x00000100;
    constexpr UINT ShgfiSysIconIndex = 0x00004000;
    constexpr UINT ShgfiSmallIcon = 0x00000001;

    struct ShellImageList final
    {
        Win32Bridge::Bridge::GuestImageList handle = nullptr;
        HICON file = nullptr;
        HICON folder = nullptr;
    };
    std::mutex g_shellImagesLock;
    ShellImageList g_smallShellImages;
    ShellImageList g_largeShellImages;

    ShellImageList* EnsureShellImages(bool useSmallIcons)
    {
        std::lock_guard<std::mutex> guard(g_shellImagesLock);
        ShellImageList& images = useSmallIcons ? g_smallShellImages : g_largeShellImages;
        if (images.handle) return &images;
        const int extent = useSmallIcons ? 16 : 32;
        images.handle = Win32Bridge::Bridge::BridgeImageListCreate(extent, extent, 0, 2, 1);
        images.file = Win32Bridge::Bridge::CreateGuestShellIcon(false, extent);
        images.folder = Win32Bridge::Bridge::CreateGuestShellIcon(true, extent);
        if (!images.handle || !images.file || !images.folder ||
            Win32Bridge::Bridge::BridgeImageListReplaceIcon(images.handle, -1, images.file) != 0 ||
            Win32Bridge::Bridge::BridgeImageListReplaceIcon(images.handle, -1, images.folder) != 1)
        {
            if (images.handle) Win32Bridge::Bridge::BridgeImageListDestroy(images.handle);
            images = ShellImageList{};
            return nullptr;
        }
        return &images;
    }

    // PIDLs normally identify shell-namespace objects. The UWP bridge has no
    // host desktop namespace to expose, so its PIDLs describe nodes in one
    // private, LocalFolder-backed hierarchy instead:
    // Desktop -> Computer -> C: -> drive_c contents.
    //
    // Keeping the node type in the PIDL is essential. A path alone cannot
    // distinguish Desktop, Computer and C:\\, and was the reason the old
    // adapter returned a repeated "Computer" item for every enumeration.
    enum class VirtualShellNode : USHORT
    {
        Desktop = 1,
        Computer = 2,
        Drive = 3,
        Directory = 4,
        File = 5,
    };

    struct VirtualPidl final
    {
        USHORT size;
        USHORT magic;
        VirtualShellNode node;
        USHORT reserved;
        wchar_t path[MAX_PATH];
        USHORT terminator;
    };

    struct VirtualPidlData final
    {
        VirtualShellNode node;
        wchar_t path[MAX_PATH];
    };

    const wchar_t* VirtualSpecialFolderPath(int folder)
    {
        switch (folder & 0xff)
        {
        case CsidlDesktop:
        case CsidlDrives:
            return L"C:\\";
        case CsidlAppData:
            return L"C:\\Users\\Default\\AppData\\Roaming";
        case CsidlLocalAppData:
            return L"C:\\Users\\Default\\AppData\\Local";
        case CsidlPersonal:
        default:
            return L"C:\\Users\\Default\\Documents";
        }
    }

    PVOID CreateVirtualPidl(VirtualShellNode node, const wchar_t* path)
    {
        auto* pidl = static_cast<VirtualPidl*>(Win32Bridge::Bridge::BridgeCoTaskMemAlloc(sizeof(VirtualPidl)));
        if (!pidl)
        {
            return nullptr;
        }
        ZeroMemory(pidl, sizeof(*pidl));
        pidl->size = static_cast<USHORT>(offsetof(VirtualPidl, terminator));
        pidl->magic = VirtualPidlMagic;
        pidl->node = node;
        wcsncpy_s(pidl->path, _countof(pidl->path), path ? path : L"", _TRUNCATE);
        return pidl;
    }

    bool ReadVirtualPidl(PVOID value, VirtualPidlData* data)
    {
        if (!value || !data)
        {
            return false;
        }
        __try
        {
            const auto* pidl = static_cast<const VirtualPidl*>(value);
            if (pidl->size != offsetof(VirtualPidl, terminator) ||
                pidl->magic != VirtualPidlMagic ||
                pidl->terminator != 0)
            {
                return false;
            }
            size_t length = 0;
            while (length < MAX_PATH && pidl->path[length] != L'\0')
            {
                ++length;
            }
            if (length == MAX_PATH)
            {
                return false;
            }
            if (pidl->node < VirtualShellNode::Desktop || pidl->node > VirtualShellNode::File)
            {
                return false;
            }
            data->node = pidl->node;
            memcpy(data->path, pidl->path, sizeof(data->path));
            return true;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return false;
        }
    }

    // ABI-only IShellFolder/IEnumIDList/IContextMenu implementation. This is the
    // Wine-style shell boundary translated to the bridge's LocalFolder-backed
    // namespace: it exposes one virtual drive, not the host desktop.
    struct VirtualEnumIdList;
    struct VirtualShellFolder;
    struct VirtualContextMenu;
    using QueryInterfaceProc = HRESULT(STDMETHODCALLTYPE*)(void*, REFIID, void**);
    using AddRefProc = ULONG(STDMETHODCALLTYPE*)(void*);
    using ReleaseProc = ULONG(STDMETHODCALLTYPE*)(void*);

    struct EnumIdListVTable final
    {
        QueryInterfaceProc queryInterface;
        AddRefProc addRef;
        ReleaseProc release;
        HRESULT(STDMETHODCALLTYPE* next)(void*, ULONG, PVOID*, ULONG*);
        HRESULT(STDMETHODCALLTYPE* skip)(void*, ULONG);
        HRESULT(STDMETHODCALLTYPE* reset)(void*);
        HRESULT(STDMETHODCALLTYPE* clone)(void*, void**);
    };

    struct ShellFolderVTable final
    {
        QueryInterfaceProc queryInterface;
        AddRefProc addRef;
        ReleaseProc release;
        HRESULT(STDMETHODCALLTYPE* parseDisplayName)(void*, HWND, PVOID, LPWSTR, ULONG*, PVOID*, ULONG*);
        HRESULT(STDMETHODCALLTYPE* enumObjects)(void*, HWND, DWORD, void**);
        HRESULT(STDMETHODCALLTYPE* bindToObject)(void*, PVOID, PVOID, REFIID, void**);
        HRESULT(STDMETHODCALLTYPE* bindToStorage)(void*, PVOID, PVOID, REFIID, void**);
        HRESULT(STDMETHODCALLTYPE* compareIds)(void*, LPARAM, PVOID, PVOID);
        HRESULT(STDMETHODCALLTYPE* createViewObject)(void*, HWND, REFIID, void**);
        HRESULT(STDMETHODCALLTYPE* getAttributesOf)(void*, UINT, PVOID const*, DWORD*);
        HRESULT(STDMETHODCALLTYPE* getUiObjectOf)(void*, HWND, UINT, PVOID const*, REFIID, UINT*, void**);
        HRESULT(STDMETHODCALLTYPE* getDisplayNameOf)(void*, PVOID, DWORD, PVOID);
        HRESULT(STDMETHODCALLTYPE* setNameOf)(void*, HWND, PVOID, LPCWSTR, DWORD, PVOID*);
    };

    struct ContextMenuVTable final
    {
        QueryInterfaceProc queryInterface;
        AddRefProc addRef;
        ReleaseProc release;
        HRESULT(STDMETHODCALLTYPE* queryContextMenu)(void*, HMENU, UINT, UINT, UINT, UINT);
        HRESULT(STDMETHODCALLTYPE* invokeCommand)(void*, const void*);
        HRESULT(STDMETHODCALLTYPE* getCommandString)(void*, UINT_PTR, UINT, UINT*, LPSTR, UINT);
    };

    // CMINVOKECOMMANDINFO/CMINVOKECOMMANDINFOEX guest ABI layouts.  Keeping
    // these declarations local avoids a dependency on desktop shell headers.
    struct GuestInvokeCommandInfo final
    {
        DWORD size;
        DWORD mask;
        HWND owner;
        LPCSTR verb;
        LPCSTR parameters;
        LPCSTR directory;
        int show;
        DWORD hotKey;
        HANDLE icon;
    };
    static_assert(sizeof(GuestInvokeCommandInfo) == 56,
        "CMINVOKECOMMANDINFO x64 guest ABI mismatch");

    struct GuestInvokeCommandInfoEx final
    {
        DWORD size;
        DWORD mask;
        HWND owner;
        LPCSTR verb;
        LPCSTR parameters;
        LPCSTR directory;
        int show;
        DWORD hotKey;
        HANDLE icon;
        LPCSTR title;
        LPCWSTR verbWide;
        LPCWSTR parametersWide;
        LPCWSTR directoryWide;
        LPCWSTR titleWide;
        POINT invokePoint;
    };
    static_assert(sizeof(GuestInvokeCommandInfoEx) == 104,
        "CMINVOKECOMMANDINFOEX x64 guest ABI mismatch");

    struct GuestMenuItemInfoW final
    {
        UINT size;
        UINT mask;
        UINT type;
        UINT state;
        UINT identifier;
        HMENU subMenu;
        HBITMAP checkedBitmap;
        HBITMAP uncheckedBitmap;
        ULONG_PTR itemData;
        LPWSTR text;
        UINT textLength;
        HBITMAP itemBitmap;
    };

    std::wstring FileNameFromGuestPath(const std::wstring& path);
    std::wstring JoinGuestPath(const std::wstring& folder, const std::wstring& name);

    std::wstring ShellDisplayName(VirtualShellNode node, const std::wstring& path)
    {
        switch (node)
        {
        case VirtualShellNode::Desktop: return L"Desktop";
        case VirtualShellNode::Computer: return L"Computer";
        case VirtualShellNode::Drive: return L"Local Disk (C:)";
        default:
            {
                std::wstring value = FileNameFromGuestPath(path);
                return value.empty() ? path : value;
            }
        }
    }

    VirtualShellNode NodeForGuestPath(const std::wstring& path)
    {
        if (_wcsicmp(path.c_str(), L"C:\\") == 0 || _wcsicmp(path.c_str(), L"C:") == 0)
        {
            return VirtualShellNode::Drive;
        }
        const DWORD attributes = Win32Bridge::Bridge::BridgeGetFileAttributesW(path.c_str());
        return attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_DIRECTORY)
            ? VirtualShellNode::Directory
            : VirtualShellNode::File;
    }

    struct VirtualEnumIdList final
    {
        const EnumIdListVTable* vtable;
        std::atomic<ULONG> references{ 1 };
        std::vector<VirtualPidlData> items;
        size_t cursor = 0;
    };

    struct VirtualShellFolder final
    {
        const ShellFolderVTable* vtable;
        std::atomic<ULONG> references{ 1 };
        VirtualShellNode node = VirtualShellNode::Desktop;
        std::wstring path;
    };

    struct VirtualContextMenu final
    {
        const ContextMenuVTable* vtable;
        std::atomic<ULONG> references{ 1 };
        HWND owner = nullptr;
        std::vector<VirtualPidlData> items;
    };

    VirtualEnumIdList* CreateVirtualEnumIdList(std::vector<VirtualPidlData> items = {});
    VirtualShellFolder* CreateVirtualShellFolder(VirtualShellNode node = VirtualShellNode::Desktop,
        const std::wstring& path = std::wstring());
    VirtualContextMenu* CreateVirtualContextMenu(
        HWND owner, std::vector<VirtualPidlData> items);

    constexpr GUID VirtualIidEnumIdList =
        { 0x000214f2, 0x0000, 0x0000, { 0xc0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x46 } };
    constexpr GUID VirtualIidShellFolder =
        { 0x000214e6, 0x0000, 0x0000, { 0xc0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x46 } };
    constexpr GUID VirtualIidContextMenu =
        { 0x000214e4, 0x0000, 0x0000, { 0xc0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x46 } };

    HRESULT STDMETHODCALLTYPE EnumQueryInterface(void* self, REFIID iid, void** object)
    {
        if (!object) return E_POINTER;
        *object = nullptr;
        if (!IsEqualGUID(iid, IID_IUnknown) && !IsEqualGUID(iid, VirtualIidEnumIdList)) return E_NOINTERFACE;
        auto* value = static_cast<VirtualEnumIdList*>(self);
        ++value->references;
        *object = value;
        return S_OK;
    }

    ULONG STDMETHODCALLTYPE EnumAddRef(void* self)
    {
        return ++static_cast<VirtualEnumIdList*>(self)->references;
    }

    ULONG STDMETHODCALLTYPE EnumRelease(void* self)
    {
        auto* value = static_cast<VirtualEnumIdList*>(self);
        const ULONG remaining = --value->references;
        if (!remaining) delete value;
        return remaining;
    }

    HRESULT STDMETHODCALLTYPE EnumNext(void* self, ULONG count, PVOID* items, ULONG* fetched)
    {
        if (!items || count == 0) return E_INVALIDARG;
        if (fetched) *fetched = 0;
        auto* value = static_cast<VirtualEnumIdList*>(self);
        ULONG returned = 0;
        while (returned < count && value->cursor < value->items.size())
        {
            const VirtualPidlData& source = value->items[value->cursor++];
            items[returned] = CreateVirtualPidl(source.node, source.path);
            if (!items[returned])
            {
                while (returned > 0) Win32Bridge::Bridge::BridgeCoTaskMemFree(items[--returned]);
                return E_OUTOFMEMORY;
            }
            ++returned;
        }
        if (fetched) *fetched = returned;
        return returned == count ? S_OK : S_FALSE;
    }

    HRESULT STDMETHODCALLTYPE EnumSkip(void* self, ULONG count)
    {
        auto* value = static_cast<VirtualEnumIdList*>(self);
        const size_t remaining = value->items.size() - (std::min)(value->cursor, value->items.size());
        const size_t skipped = (std::min)(remaining, static_cast<size_t>(count));
        value->cursor += skipped;
        return skipped == count ? S_OK : S_FALSE;
    }

    HRESULT STDMETHODCALLTYPE EnumReset(void* self)
    {
        static_cast<VirtualEnumIdList*>(self)->cursor = 0;
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE EnumClone(void* self, void** clone)
    {
        if (!clone) return E_POINTER;
        *clone = nullptr;
        auto* source = static_cast<VirtualEnumIdList*>(self);
        auto* value = CreateVirtualEnumIdList(source->items);
        if (!value) return E_OUTOFMEMORY;
        value->cursor = source->cursor;
        *clone = value;
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE FolderQueryInterface(void* self, REFIID iid, void** object)
    {
        if (!object) return E_POINTER;
        *object = nullptr;
        if (!IsEqualGUID(iid, IID_IUnknown) && !IsEqualGUID(iid, VirtualIidShellFolder)) return E_NOINTERFACE;
        auto* value = static_cast<VirtualShellFolder*>(self);
        ++value->references;
        *object = value;
        return S_OK;
    }

    ULONG STDMETHODCALLTYPE FolderAddRef(void* self)
    {
        return ++static_cast<VirtualShellFolder*>(self)->references;
    }

    ULONG STDMETHODCALLTYPE FolderRelease(void* self)
    {
        auto* value = static_cast<VirtualShellFolder*>(self);
        const ULONG remaining = --value->references;
        if (!remaining) delete value;
        return remaining;
    }

    HRESULT STDMETHODCALLTYPE FolderParseDisplayName(void* self, HWND, PVOID, LPWSTR name, ULONG*, PVOID* item, ULONG*)
    {
        if (!item) return E_POINTER;
        const auto* folder = static_cast<VirtualShellFolder*>(self);
        const std::wstring requested = name ? name : L"";
        VirtualShellNode node = VirtualShellNode::File;
        std::wstring path;
        if (folder->node == VirtualShellNode::Desktop && _wcsicmp(requested.c_str(), L"Computer") == 0)
        {
            node = VirtualShellNode::Computer;
        }
        else if (folder->node == VirtualShellNode::Computer &&
            (_wcsicmp(requested.c_str(), L"C:") == 0 || _wcsicmp(requested.c_str(), L"C:\\") == 0))
        {
            node = VirtualShellNode::Drive;
            path = L"C:\\";
        }
        else
        {
            path = requested.size() >= 2 && requested[1] == L':'
                ? requested
                : JoinGuestPath(folder->path, requested);
            node = NodeForGuestPath(path);
        }
        *item = CreateVirtualPidl(node, path.c_str());
        return *item ? S_OK : E_OUTOFMEMORY;
    }

    std::vector<VirtualPidlData> EnumerateFolderChildren(const VirtualShellFolder& folder)
    {
        std::vector<VirtualPidlData> items;
        if (folder.node == VirtualShellNode::Desktop)
        {
            VirtualPidlData computer = {};
            computer.node = VirtualShellNode::Computer;
            items.push_back(computer);
            return items;
        }
        if (folder.node == VirtualShellNode::Computer)
        {
            VirtualPidlData drive = {};
            drive.node = VirtualShellNode::Drive;
            wcscpy_s(drive.path, L"C:\\");
            items.push_back(drive);
            return items;
        }
        if (folder.node != VirtualShellNode::Drive && folder.node != VirtualShellNode::Directory)
        {
            return items;
        }

        const std::wstring pattern = JoinGuestPath(folder.path, L"*");
        WIN32_FIND_DATAW entry = {};
        HANDLE search = Win32Bridge::Bridge::BridgeFindFirstFileW(pattern.c_str(), &entry);
        if (search == INVALID_HANDLE_VALUE)
        {
            Win32Bridge::Bridge::RuntimeDiagnostics::Record(L"SHELL: namespace folder enumeration found no guest items.");
            return items;
        }
        do
        {
            if (wcscmp(entry.cFileName, L".") == 0 || wcscmp(entry.cFileName, L"..") == 0)
            {
                continue;
            }
            VirtualPidlData child = {};
            const std::wstring childPath = JoinGuestPath(folder.path, entry.cFileName);
            child.node = (entry.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
                ? VirtualShellNode::Directory
                : VirtualShellNode::File;
            wcsncpy_s(child.path, childPath.c_str(), _TRUNCATE);
            items.push_back(child);
        } while (Win32Bridge::Bridge::BridgeFindNextFileW(search, &entry));
        Win32Bridge::Bridge::BridgeFindClose(search);
        Win32Bridge::Bridge::RuntimeDiagnostics::Record(
            L"SHELL: namespace enumerated LocalStorage drive_c folder.");
        return items;
    }

    HRESULT STDMETHODCALLTYPE FolderEnumObjects(void* self, HWND, DWORD, void** enumeration)
    {
        if (!enumeration) return E_POINTER;
        *enumeration = nullptr;
        auto* folder = static_cast<VirtualShellFolder*>(self);
        auto* value = CreateVirtualEnumIdList(EnumerateFolderChildren(*folder));
        if (!value) return E_OUTOFMEMORY;
        *enumeration = value;
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE FolderBindToObject(void*, PVOID item, PVOID, REFIID iid, void** object)
    {
        if (!object) return E_POINTER;
        *object = nullptr;
        if (!IsEqualGUID(iid, IID_IUnknown) && !IsEqualGUID(iid, VirtualIidShellFolder)) return E_NOINTERFACE;
        VirtualPidlData child = {};
        if (!ReadVirtualPidl(item, &child)) return E_INVALIDARG;
        auto* value = CreateVirtualShellFolder(child.node, child.path);
        if (!value) return E_OUTOFMEMORY;
        *object = value;
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE FolderBindToStorage(void*, PVOID, PVOID, REFIID, void**) { return E_NOINTERFACE; }
    HRESULT STDMETHODCALLTYPE FolderCompareIds(void*, LPARAM, PVOID, PVOID) { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE FolderCreateViewObject(void*, HWND, REFIID, void**) { return E_NOINTERFACE; }
    HRESULT STDMETHODCALLTYPE FolderGetAttributesOf(void*, UINT count, PVOID const* items, DWORD* attributes)
    {
        if (!attributes) return E_POINTER;
        constexpr DWORD SfgaoFolder = 0x20000000u;
        constexpr DWORD SfgaoFileSystem = 0x40000000u;
        DWORD supported = SfgaoFolder | SfgaoFileSystem;
        if (count && items)
        {
            VirtualPidlData item = {};
            if (!ReadVirtualPidl(items[0], &item)) return E_INVALIDARG;
            if (item.node == VirtualShellNode::File) supported &= ~SfgaoFolder;
            if (item.node == VirtualShellNode::Desktop || item.node == VirtualShellNode::Computer)
                supported &= ~SfgaoFileSystem;
        }
        *attributes &= supported;
        return S_OK;
    }

    std::wstring ParentGuestPath(const std::wstring& path)
    {
        if (path.empty()) return std::wstring();
        std::wstring normalized = path;
        while (normalized.size() > 3 &&
            (normalized.back() == L'\\' || normalized.back() == L'/'))
        {
            normalized.pop_back();
        }
        const size_t separator = normalized.find_last_of(L"\\/");
        if (separator == std::wstring::npos) return std::wstring();
        if (separator == 2 && normalized.size() >= 3) return normalized.substr(0, 3);
        return normalized.substr(0, separator);
    }

    std::wstring FormatGuestFileTime(const FILETIME& value)
    {
        if (value.dwLowDateTime == 0 && value.dwHighDateTime == 0)
            return L"Not available";
        FILETIME local{};
        SYSTEMTIME time{};
        if (!Win32Bridge::Bridge::BridgeFileTimeToLocalFileTime(&value, &local) ||
            !Win32Bridge::Bridge::BridgeFileTimeToSystemTime(&local, &time))
        {
            return L"Not available";
        }
        wchar_t buffer[64]{};
        swprintf_s(buffer, L"%04u-%02u-%02u %02u:%02u:%02u",
            time.wYear, time.wMonth, time.wDay,
            time.wHour, time.wMinute, time.wSecond);
        return buffer;
    }

    std::wstring FormatGuestAttributes(DWORD attributes)
    {
        if (attributes == INVALID_FILE_ATTRIBUTES) return L"Not available";
        struct AttributeName final { DWORD flag; const wchar_t* name; };
        constexpr AttributeName names[] =
        {
            { FILE_ATTRIBUTE_READONLY, L"Read-only" },
            { FILE_ATTRIBUTE_HIDDEN, L"Hidden" },
            { FILE_ATTRIBUTE_SYSTEM, L"System" },
            { FILE_ATTRIBUTE_DIRECTORY, L"Directory" },
            { FILE_ATTRIBUTE_ARCHIVE, L"Archive" },
            { FILE_ATTRIBUTE_TEMPORARY, L"Temporary" },
            { FILE_ATTRIBUTE_COMPRESSED, L"Compressed" },
            { FILE_ATTRIBUTE_ENCRYPTED, L"Encrypted" },
        };
        std::wstring result;
        for (const auto& entry : names)
        {
            if ((attributes & entry.flag) == 0) continue;
            if (!result.empty()) result += L", ";
            result += entry.name;
        }
        return result.empty() ? L"Normal" : result;
    }

    struct VirtualItemMetadata final
    {
        DWORD attributes = INVALID_FILE_ATTRIBUTES;
        ULONGLONG size = 0;
        FILETIME creation{};
        FILETIME access{};
        FILETIME write{};
        bool hasFindData = false;
    };

    VirtualItemMetadata ReadVirtualItemMetadata(const VirtualPidlData& item)
    {
        VirtualItemMetadata metadata;
        if (item.path[0] == L'\0') return metadata;
        metadata.attributes = Win32Bridge::Bridge::BridgeGetFileAttributesW(item.path);
        if (item.node == VirtualShellNode::Drive) return metadata;

        WIN32_FIND_DATAW data{};
        HANDLE search = Win32Bridge::Bridge::BridgeFindFirstFileW(item.path, &data);
        if (search == INVALID_HANDLE_VALUE) return metadata;
        Win32Bridge::Bridge::BridgeFindClose(search);
        metadata.attributes = data.dwFileAttributes;
        metadata.size = (static_cast<ULONGLONG>(data.nFileSizeHigh) << 32) |
            data.nFileSizeLow;
        metadata.creation = data.ftCreationTime;
        metadata.access = data.ftLastAccessTime;
        metadata.write = data.ftLastWriteTime;
        metadata.hasFindData = true;
        return metadata;
    }

    std::wstring VirtualItemType(const VirtualPidlData& item)
    {
        switch (item.node)
        {
        case VirtualShellNode::Desktop: return L"Virtual desktop";
        case VirtualShellNode::Computer: return L"Computer";
        case VirtualShellNode::Drive: return L"Local disk";
        case VirtualShellNode::Directory: return L"File folder";
        default: return L"File";
        }
    }

    HRESULT ShowVirtualItemProperties(
        HWND owner,
        const std::vector<VirtualPidlData>& items)
    {
        if (items.empty()) return E_INVALIDARG;

        std::vector<std::pair<std::wstring, std::wstring>> values;
        if (items.size() == 1)
        {
            const VirtualPidlData& item = items.front();
            const VirtualItemMetadata metadata = ReadVirtualItemMetadata(item);
            values.emplace_back(L"Name", ShellDisplayName(item.node, item.path));
            values.emplace_back(L"Type", VirtualItemType(item));
            if (item.path[0] != L'\0')
            {
                values.emplace_back(L"Location", item.node == VirtualShellNode::Drive
                    ? std::wstring(L"Computer")
                    : ParentGuestPath(item.path));
                values.emplace_back(L"Path", item.path);
            }
            if (item.node == VirtualShellNode::File)
                values.emplace_back(L"Size", metadata.hasFindData
                    ? std::to_wstring(metadata.size) + L" bytes"
                    : std::wstring(L"Not available"));
            if (metadata.hasFindData)
            {
                values.emplace_back(L"Created", FormatGuestFileTime(metadata.creation));
                values.emplace_back(L"Modified", FormatGuestFileTime(metadata.write));
                values.emplace_back(L"Accessed", FormatGuestFileTime(metadata.access));
            }
            if (item.path[0] != L'\0')
                values.emplace_back(L"Attributes", FormatGuestAttributes(metadata.attributes));
        }
        else
        {
            ULONGLONG totalSize = 0;
            UINT fileCount = 0;
            UINT folderCount = 0;
            UINT unavailableSizeCount = 0;
            std::wstring commonLocation;
            bool sameLocation = true;
            for (const VirtualPidlData& item : items)
            {
                const VirtualItemMetadata metadata = ReadVirtualItemMetadata(item);
                if (item.node == VirtualShellNode::File)
                {
                    ++fileCount;
                    if (metadata.hasFindData)
                    {
                        if (ULLONG_MAX - totalSize >= metadata.size) totalSize += metadata.size;
                        else totalSize = ULLONG_MAX;
                    }
                    else
                    {
                        ++unavailableSizeCount;
                    }
                }
                else
                {
                    ++folderCount;
                }
                const std::wstring location = ParentGuestPath(item.path);
                if (commonLocation.empty()) commonLocation = location;
                else if (_wcsicmp(commonLocation.c_str(), location.c_str()) != 0) sameLocation = false;
            }
            values.emplace_back(L"Selected items", std::to_wstring(items.size()));
            values.emplace_back(L"Files", std::to_wstring(fileCount));
            values.emplace_back(L"Folders", std::to_wstring(folderCount));
            if (sameLocation && !commonLocation.empty())
                values.emplace_back(L"Location", commonLocation);
            values.emplace_back(L"Combined file size", unavailableSizeCount == 0
                ? std::to_wstring(totalSize) + L" bytes"
                : std::wstring(L"Not available"));
        }

        std::vector<Win32Bridge::Bridge::GuestPropertyFieldDescriptor> fields;
        fields.reserve(values.size());
        for (const auto& value : values)
        {
            Win32Bridge::Bridge::GuestPropertyFieldDescriptor field;
            field.name = value.first.c_str();
            field.value = value.second.c_str();
            fields.push_back(field);
        }

        const std::wstring caption = items.size() == 1
            ? ShellDisplayName(items.front().node, items.front().path) + L" Properties"
            : std::to_wstring(items.size()) + L" Items Properties";
        Win32Bridge::Bridge::GuestPropertyPageDescriptor page;
        page.title = L"General";
        page.fields = fields.data();
        page.fieldCount = static_cast<UINT>(fields.size());
        Win32Bridge::Bridge::GuestPropertySheetDescriptor sheet;
        sheet.parent = owner;
        sheet.caption = caption.c_str();
        sheet.pages = &page;
        sheet.pageCount = 1;
        sheet.flags = 0x00000080u; // PSH_NOAPPLYNOW: metadata is read-only.

        Win32Bridge::Bridge::RuntimeDiagnostics::Record(
            L"SHELL: invoking virtual properties for " +
            std::to_wstring(items.size()) + L" item(s).");
        return Win32Bridge::Bridge::ShowGuestPropertySheet(sheet) == -1 ? E_FAIL : S_OK;
    }

    HRESULT STDMETHODCALLTYPE ContextQueryInterface(void* self, REFIID iid, void** object)
    {
        if (!object) return E_POINTER;
        *object = nullptr;
        if (!IsEqualGUID(iid, IID_IUnknown) && !IsEqualGUID(iid, VirtualIidContextMenu))
            return E_NOINTERFACE;
        auto* value = static_cast<VirtualContextMenu*>(self);
        ++value->references;
        *object = value;
        return S_OK;
    }

    ULONG STDMETHODCALLTYPE ContextAddRef(void* self)
    {
        return ++static_cast<VirtualContextMenu*>(self)->references;
    }

    ULONG STDMETHODCALLTYPE ContextRelease(void* self)
    {
        auto* value = static_cast<VirtualContextMenu*>(self);
        const ULONG remaining = --value->references;
        if (!remaining) delete value;
        return remaining;
    }

    HRESULT STDMETHODCALLTYPE ContextQueryContextMenu(
        void*, HMENU menu, UINT insertionIndex,
        UINT firstCommand, UINT lastCommand, UINT flags)
    {
        constexpr UINT CmfDefaultOnly = 0x00000001u;
        constexpr UINT CmfNoVerbs = 0x00000008u;
        constexpr UINT MiimId = 0x00000002u;
        constexpr UINT MiimString = 0x00000040u;
        constexpr UINT MiimFtype = 0x00000100u;
        if (!menu) return E_INVALIDARG;
        if ((flags & (CmfDefaultOnly | CmfNoVerbs)) != 0) return S_OK;
        if (firstCommand > lastCommand) return HRESULT_FROM_WIN32(ERROR_INSUFFICIENT_BUFFER);

        const int existing = Win32Bridge::Bridge::BridgeGetMenuItemCount(menu);
        if (existing < 0) return E_INVALIDARG;
        GuestMenuItemInfoW item{};
        item.size = sizeof(item);
        item.mask = MiimId | MiimString | MiimFtype;
        item.identifier = firstCommand;
        item.text = const_cast<LPWSTR>(L"Properties");
        item.textLength = 10;
        if (!Win32Bridge::Bridge::BridgeInsertMenuItemW(
                menu, insertionIndex, TRUE, &item))
        {
            return E_FAIL;
        }
        Win32Bridge::Bridge::RuntimeDiagnostics::Record(
            L"SHELL: IContextMenu added the properties command.");
        return static_cast<HRESULT>(1); // One command identifier consumed.
    }

    HRESULT STDMETHODCALLTYPE ContextInvokeCommand(void* self, const void* commandInfo)
    {
        constexpr DWORD CmicMaskUnicode = 0x00004000u;
        if (!commandInfo) return E_POINTER;
        const auto* command = static_cast<const GuestInvokeCommandInfo*>(commandInfo);
        if (command->size < sizeof(GuestInvokeCommandInfo)) return E_INVALIDARG;

        bool properties = false;
        const ULONG_PTR verbValue = reinterpret_cast<ULONG_PTR>(command->verb);
        if ((verbValue >> 16) == 0)
        {
            properties = static_cast<UINT>(verbValue & 0xffffu) == 0;
        }
        else if (command->verb)
        {
            properties = _stricmp(command->verb, "properties") == 0;
        }
        if (!properties && (command->mask & CmicMaskUnicode) != 0 &&
            command->size >= sizeof(GuestInvokeCommandInfoEx))
        {
            const auto* extended = static_cast<const GuestInvokeCommandInfoEx*>(commandInfo);
            const ULONG_PTR wideVerbValue = reinterpret_cast<ULONG_PTR>(extended->verbWide);
            properties = wideVerbValue > 0xffff && extended->verbWide &&
                _wcsicmp(extended->verbWide, L"properties") == 0;
        }
        if (!properties) return E_INVALIDARG;

        auto* context = static_cast<VirtualContextMenu*>(self);
        return ShowVirtualItemProperties(command->owner ? command->owner : context->owner,
            context->items);
    }

    HRESULT STDMETHODCALLTYPE ContextGetCommandString(
        void*, UINT_PTR command, UINT flags, UINT*, LPSTR buffer, UINT characterCount)
    {
        if (command != 0) return E_INVALIDARG;
        const UINT request = flags & 0x00000007u;
        if (request == 2 || request == 6) return S_OK; // GCS_VALIDATEA/W.
        if (!buffer || characterCount == 0) return E_POINTER;

        if (request == 4 || request == 5) // GCS_VERBW / GCS_HELPTEXTW.
        {
            const wchar_t* text = request == 4
                ? L"properties"
                : L"Displays properties for the selected virtual item.";
            wcsncpy_s(reinterpret_cast<wchar_t*>(buffer), characterCount, text, _TRUNCATE);
            return S_OK;
        }
        if (request == 0 || request == 1) // GCS_VERBA / GCS_HELPTEXTA.
        {
            const char* text = request == 0
                ? "properties"
                : "Displays properties for the selected virtual item.";
            strncpy_s(buffer, characterCount, text, _TRUNCATE);
            return S_OK;
        }
        return E_INVALIDARG;
    }

    HRESULT STDMETHODCALLTYPE FolderGetUiObjectOf(
        void*, HWND owner, UINT count, PVOID const* items,
        REFIID iid, UINT*, void** object)
    {
        if (!object) return E_POINTER;
        *object = nullptr;
        if (!IsEqualGUID(iid, IID_IUnknown) && !IsEqualGUID(iid, VirtualIidContextMenu))
            return E_NOINTERFACE;
        if (count == 0 || !items) return E_INVALIDARG;

        std::vector<VirtualPidlData> selection;
        selection.reserve(count);
        for (UINT index = 0; index < count; ++index)
        {
            VirtualPidlData item{};
            if (!ReadVirtualPidl(items[index], &item)) return E_INVALIDARG;
            selection.push_back(item);
        }
        auto* context = CreateVirtualContextMenu(owner, std::move(selection));
        if (!context) return E_OUTOFMEMORY;
        *object = context;
        Win32Bridge::Bridge::RuntimeDiagnostics::Record(
            L"SHELL: IShellFolder::GetUIObjectOf returned a virtual IContextMenu.");
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE FolderGetDisplayNameOf(void*, PVOID item, DWORD, PVOID result)
    {
        if (!result) return E_POINTER;
        VirtualPidlData itemData = {};
        if (!ReadVirtualPidl(item, &itemData)) return E_INVALIDARG;
        // STRRET_WSTR is a UINT followed by a CoTaskMem-owned wide pointer on
        // x64.  Keep the layout local because UWP does not expose shell types.
        struct GuestStrRet { UINT type; UINT reserved; LPWSTR value; };
        auto* output = static_cast<GuestStrRet*>(result);
        const std::wstring display = ShellDisplayName(itemData.node, itemData.path);
        const size_t bytes = (display.size() + 1) * sizeof(wchar_t);
        auto* copy = static_cast<LPWSTR>(Win32Bridge::Bridge::BridgeCoTaskMemAlloc(bytes));
        if (!copy) return E_OUTOFMEMORY;
        memcpy(copy, display.c_str(), bytes);
        output->type = 0; // STRRET_WSTR
        output->reserved = 0;
        output->value = copy;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE FolderSetNameOf(void*, HWND, PVOID, LPCWSTR, DWORD, PVOID*) { return E_NOTIMPL; }

    const EnumIdListVTable g_virtualEnumIdListVTable =
    {
        EnumQueryInterface, EnumAddRef, EnumRelease, EnumNext, EnumSkip, EnumReset, EnumClone
    };
    const ShellFolderVTable g_virtualShellFolderVTable =
    {
        FolderQueryInterface, FolderAddRef, FolderRelease, FolderParseDisplayName, FolderEnumObjects,
        FolderBindToObject, FolderBindToStorage, FolderCompareIds, FolderCreateViewObject,
        FolderGetAttributesOf, FolderGetUiObjectOf, FolderGetDisplayNameOf, FolderSetNameOf
    };
    const ContextMenuVTable g_virtualContextMenuVTable =
    {
        ContextQueryInterface, ContextAddRef, ContextRelease,
        ContextQueryContextMenu, ContextInvokeCommand, ContextGetCommandString
    };

    VirtualEnumIdList* CreateVirtualEnumIdList(std::vector<VirtualPidlData> items)
    {
        auto* value = new (std::nothrow) VirtualEnumIdList{};
        if (value)
        {
            value->vtable = &g_virtualEnumIdListVTable;
            value->items = std::move(items);
        }
        return value;
    }

    VirtualShellFolder* CreateVirtualShellFolder(VirtualShellNode node, const std::wstring& path)
    {
        auto* value = new (std::nothrow) VirtualShellFolder{};
        if (value)
        {
            value->vtable = &g_virtualShellFolderVTable;
            value->node = node;
            value->path = path;
        }
        return value;
    }

    VirtualContextMenu* CreateVirtualContextMenu(
        HWND owner, std::vector<VirtualPidlData> items)
    {
        auto* value = new (std::nothrow) VirtualContextMenu{};
        if (value)
        {
            value->vtable = &g_virtualContextMenuVTable;
            value->owner = owner;
            value->items = std::move(items);
        }
        return value;
    }

    std::vector<std::wstring> ReadGuestPathList(LPCWSTR paths)
    {
        std::vector<std::wstring> values;
        if (!paths)
        {
            return values;
        }
        for (const wchar_t* current = paths; *current != L'\0'; current += wcslen(current) + 1)
        {
            values.emplace_back(current);
            if (values.size() >= 256)
            {
                break;
            }
        }
        return values;
    }

    std::wstring FileNameFromGuestPath(const std::wstring& path)
    {
        const size_t separator = path.find_last_of(L"\\/");
        return separator == std::wstring::npos ? path : path.substr(separator + 1);
    }

    std::wstring JoinGuestPath(const std::wstring& folder, const std::wstring& name)
    {
        if (folder.empty())
        {
            return name;
        }
        return (folder.back() == L'\\' || folder.back() == L'/')
            ? folder + name
            : folder + L"\\" + name;
    }

    std::wstring FileOperationTarget(const std::wstring& source, const std::wstring& requested, bool multipleSources)
    {
        const DWORD attributes = requested.empty()
            ? INVALID_FILE_ATTRIBUTES
            : Win32Bridge::Bridge::BridgeGetFileAttributesW(requested.c_str());
        const bool directoryTarget = multipleSources ||
            (attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_DIRECTORY) != 0) ||
            (!requested.empty() && (requested.back() == L'\\' || requested.back() == L'/'));
        return directoryTarget ? JoinGuestPath(requested, FileNameFromGuestPath(source)) : requested;
    }

    bool CopyGuestTree(const std::wstring& source, const std::wstring& destination, unsigned depth)
    {
        if (depth > 64)
        {
            Win32Bridge::Bridge::BridgeSetLastError(ERROR_DIRECTORY);
            return false;
        }
        const DWORD attributes = Win32Bridge::Bridge::BridgeGetFileAttributesW(source.c_str());
        if (attributes == INVALID_FILE_ATTRIBUTES)
        {
            return false;
        }
        if ((attributes & FILE_ATTRIBUTE_DIRECTORY) == 0)
        {
            return Win32Bridge::Bridge::BridgeCopyFileExW(
                source.c_str(), destination.c_str(), nullptr, nullptr, nullptr, 0) != FALSE;
        }

        if (!Win32Bridge::Bridge::BridgeCreateDirectoryW(destination.c_str(), nullptr) &&
            Win32Bridge::Bridge::BridgeGetLastError() != ERROR_ALREADY_EXISTS)
        {
            return false;
        }
        WIN32_FIND_DATAW entry = {};
        const std::wstring pattern = JoinGuestPath(source, L"*");
        HANDLE enumeration = Win32Bridge::Bridge::BridgeFindFirstFileW(pattern.c_str(), &entry);
        if (enumeration == INVALID_HANDLE_VALUE)
        {
            // A directory with no children is still a successful copy.
            return Win32Bridge::Bridge::BridgeGetLastError() == ERROR_FILE_NOT_FOUND;
        }
        bool copied = true;
        do
        {
            if (wcscmp(entry.cFileName, L".") != 0 && wcscmp(entry.cFileName, L"..") != 0)
            {
                copied = CopyGuestTree(
                    JoinGuestPath(source, entry.cFileName),
                    JoinGuestPath(destination, entry.cFileName),
                    depth + 1);
            }
        } while (copied && Win32Bridge::Bridge::BridgeFindNextFileW(enumeration, &entry));
        const DWORD enumerationError = Win32Bridge::Bridge::BridgeGetLastError();
        Win32Bridge::Bridge::BridgeFindClose(enumeration);
        return copied && (enumerationError == ERROR_NO_MORE_FILES || enumerationError == ERROR_SUCCESS);
    }

    bool DeleteGuestTree(const std::wstring& path, unsigned depth)
    {
        if (depth > 64)
        {
            Win32Bridge::Bridge::BridgeSetLastError(ERROR_DIRECTORY);
            return false;
        }
        const DWORD attributes = Win32Bridge::Bridge::BridgeGetFileAttributesW(path.c_str());
        if (attributes == INVALID_FILE_ATTRIBUTES)
        {
            return false;
        }
        if ((attributes & FILE_ATTRIBUTE_DIRECTORY) == 0)
        {
            return Win32Bridge::Bridge::BridgeDeleteFileW(path.c_str()) != FALSE;
        }

        WIN32_FIND_DATAW entry = {};
        const std::wstring pattern = JoinGuestPath(path, L"*");
        HANDLE enumeration = Win32Bridge::Bridge::BridgeFindFirstFileW(pattern.c_str(), &entry);
        if (enumeration != INVALID_HANDLE_VALUE)
        {
            bool removed = true;
            do
            {
                if (wcscmp(entry.cFileName, L".") != 0 && wcscmp(entry.cFileName, L"..") != 0)
                {
                    removed = DeleteGuestTree(JoinGuestPath(path, entry.cFileName), depth + 1);
                }
            } while (removed && Win32Bridge::Bridge::BridgeFindNextFileW(enumeration, &entry));
            const DWORD enumerationError = Win32Bridge::Bridge::BridgeGetLastError();
            Win32Bridge::Bridge::BridgeFindClose(enumeration);
            if (!removed || (enumerationError != ERROR_NO_MORE_FILES && enumerationError != ERROR_SUCCESS))
            {
                return false;
            }
        }
        else if (Win32Bridge::Bridge::BridgeGetLastError() != ERROR_FILE_NOT_FOUND)
        {
            return false;
        }
        return Win32Bridge::Bridge::BridgeRemoveDirectoryW(path.c_str()) != FALSE;
    }
}

PVOID WINAPI Win32Bridge::Bridge::BridgeSHBrowseForFolderW(PVOID)
{
    // Folder selection is supplied by the UWP picker path, not desktop shell
    // PIDLs.  Returning cancellation keeps callers on their normal cancel path.
    return nullptr;
}

DWORD_PTR WINAPI Win32Bridge::Bridge::BridgeSHGetFileInfoW(LPCWSTR path, DWORD attributes, PVOID info, UINT size, UINT flags)
{
    if (!info || size < offsetof(GuestShellFileInfoW, displayName))
    {
        return 0;
    }
    constexpr UINT ShgfiPidl = 0x000000008;
    auto* result = static_cast<GuestShellFileInfoW*>(info);
    ZeroMemory(result, (std::min)(static_cast<size_t>(size), sizeof(*result)));
    std::wstring display;
    bool directory = false;
    if (flags & ShgfiPidl)
    {
        VirtualPidlData item = {};
        if (!ReadVirtualPidl(const_cast<LPWSTR>(path), &item))
        {
            RuntimeDiagnostics::Record(L"SHELL: SHGetFileInfoW rejected a non-bridge PIDL.");
            return 0;
        }
        display = ShellDisplayName(item.node, item.path);
        directory = item.node != VirtualShellNode::File;
        result->attributes = attributes != 0 ? attributes :
            (directory ? FILE_ATTRIBUTE_DIRECTORY : BridgeGetFileAttributesW(item.path));
    }
    else
    {
        result->attributes = attributes != 0 ? attributes :
            (path ? BridgeGetFileAttributesW(path) : FILE_ATTRIBUTE_NORMAL);
        display = path && *path ? FileNameFromGuestPath(path) : std::wstring(L"Computer");
        if (display.empty() && path) display = path;
        directory = result->attributes != INVALID_FILE_ATTRIBUTES &&
            (result->attributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
    }
    if (size >= offsetof(GuestShellFileInfoW, displayName) + sizeof(result->displayName))
        wcsncpy_s(result->displayName, _countof(result->displayName), display.c_str(), _TRUNCATE);
    if (size >= offsetof(GuestShellFileInfoW, typeName) + sizeof(result->typeName))
        wcsncpy_s(result->typeName, _countof(result->typeName),
            directory ? L"File folder" : L"File", _TRUNCATE);
    DWORD_PTR returnValue = 1;
    if ((flags & (ShgfiIcon | ShgfiSysIconIndex)) != 0)
    {
        ShellImageList* images = EnsureShellImages((flags & ShgfiSmallIcon) != 0);
        if (!images) return 0;
        result->iconIndex = directory ? 1 : 0;
        if ((flags & ShgfiIcon) != 0)
        {
            result->icon = reinterpret_cast<HICON>(BridgeCopyImage(
                directory ? images->folder : images->file, IMAGE_ICON, 0, 0, 0));
            if (!result->icon) return 0;
        }
        if ((flags & ShgfiSysIconIndex) != 0)
            returnValue = reinterpret_cast<DWORD_PTR>(images->handle);
    }
    RuntimeDiagnostics::Record(flags & ShgfiPidl
        ? L"SHELL: SHGetFileInfoW resolved bridge PIDL metadata."
        : L"SHELL: SHGetFileInfoW resolved virtual path metadata.");
    return returnValue;
}

BOOL WINAPI Win32Bridge::Bridge::BridgeSHGetPathFromIDListW(PVOID itemIdList, LPWSTR path)
{
    VirtualPidlData item = {};
    if (!path || !ReadVirtualPidl(itemIdList, &item) ||
        (item.node != VirtualShellNode::Drive && item.node != VirtualShellNode::Directory &&
            item.node != VirtualShellNode::File))
    {
        if (path) path[0] = L'\0';
        RuntimeDiagnostics::Record(L"SHELL: SHGetPathFromIDListW rejected a non-bridge PIDL.");
        return FALSE;
    }
    wcscpy_s(path, MAX_PATH, item.path);
    RuntimeDiagnostics::Record(L"SHELL: SHGetPathFromIDListW returned a virtual path.");
    return TRUE;
}

UINT WINAPI Win32Bridge::Bridge::BridgeExtractIconExW(
    LPCWSTR fileName, int iconIndex, HICON* largeIcons, HICON* smallIcons, UINT iconCount)
{
    if (largeIcons) for (UINT index = 0; index < iconCount; ++index) largeIcons[index] = nullptr;
    if (smallIcons) for (UINT index = 0; index < iconCount; ++index) smallIcons[index] = nullptr;
    GuestStorageContext* storage = CurrentGuestStorageContext();
    std::vector<BYTE> file;
    DWORD error = ERROR_SUCCESS;
    if (!fileName || !storage || !storage->ReadAllBytes(fileName, &file, &error)) return 0;
    MappedPeImage mapped;
    std::wstring mapError;
    if (!PeMapper::Materialize(file.data(), file.size(), &mapped, &mapError)) return 0;
    GuestResourceScope scope(mapped.bytes.data(), mapped.bytes.size());
    std::vector<GuestResourceIdentifier> names;
    if (EnumerateGuestResourceNames(nullptr, MAKEINTRESOURCEW(14), &names) != GuestResourceStatus::Success)
        return 0;
    if (iconIndex == -1 && !largeIcons && !smallIcons) return static_cast<UINT>(names.size());
    size_t first = iconIndex >= 0 ? static_cast<size_t>(iconIndex) : 0;
    if (iconIndex < -1)
    {
        const WORD requested = static_cast<WORD>(-iconIndex);
        const auto found = std::find_if(names.begin(), names.end(), [requested](const GuestResourceIdentifier& name)
            { return name.ordinal && name.id == requested; });
        if (found == names.end()) return 0;
        first = static_cast<size_t>(std::distance(names.begin(), found));
    }
    if (first >= names.size() || iconCount == 0) return 0;
    const UINT available = static_cast<UINT>((std::min)(static_cast<size_t>(iconCount), names.size() - first));
    UINT extracted = 0;
    for (UINT output = 0; output < available; ++output)
    {
        const GuestResourceIdentifier& name = names[first + output];
        const LPCWSTR resource = name.ordinal ? MAKEINTRESOURCEW(name.id) : name.text.c_str();
        HICON large = largeIcons ? reinterpret_cast<HICON>(BridgeLoadImageW(
            nullptr, resource, IMAGE_ICON, 32, 32, LR_DEFAULTCOLOR)) : nullptr;
        HICON smallIcon = smallIcons ? reinterpret_cast<HICON>(BridgeLoadImageW(
            nullptr, resource, IMAGE_ICON, 16, 16, LR_DEFAULTCOLOR)) : nullptr;
        if ((largeIcons && !large) || (smallIcons && !smallIcon))
        {
            if (large) BridgeDestroyIcon(large);
            if (smallIcon) BridgeDestroyIcon(smallIcon);
            break;
        }
        if (largeIcons) largeIcons[output] = large;
        if (smallIcons) smallIcons[output] = smallIcon;
        ++extracted;
    }
    return extracted;
}

UINT WINAPI Win32Bridge::Bridge::BridgeExtractIconExA(
    LPCSTR fileName, int iconIndex, HICON* largeIcons, HICON* smallIcons, UINT iconCount)
{
    if (!fileName) return 0;
    const int required = MultiByteToWideChar(CP_ACP, 0, fileName, -1, nullptr, 0);
    if (required <= 0) return 0;
    std::wstring wide(static_cast<size_t>(required), L'\0');
    if (MultiByteToWideChar(CP_ACP, 0, fileName, -1, &wide[0], required) != required) return 0;
    wide.resize(static_cast<size_t>(required - 1));
    return BridgeExtractIconExW(wide.c_str(), iconIndex, largeIcons, smallIcons, iconCount);
}
HRESULT WINAPI Win32Bridge::Bridge::BridgeSHGetDesktopFolder(PVOID* desktopFolder)
{
    if (!desktopFolder)
    {
        return E_POINTER;
    }
    *desktopFolder = CreateVirtualShellFolder();
    if (!*desktopFolder)
    {
        return E_OUTOFMEMORY;
    }
    RuntimeDiagnostics::Record(L"SHELL: SHGetDesktopFolder returned the Desktop namespace root.");
    return S_OK;
}
HRESULT WINAPI Win32Bridge::Bridge::BridgeSHGetSpecialFolderLocation(HWND, int folder, PVOID* itemIdList)
{
    if (!itemIdList)
    {
        return E_INVALIDARG;
    }
    VirtualShellNode node = VirtualShellNode::Directory;
    const wchar_t* path = VirtualSpecialFolderPath(folder);
    switch (folder & 0xff)
    {
    case CsidlDesktop:
        node = VirtualShellNode::Desktop;
        path = L"";
        break;
    case CsidlDrives:
        node = VirtualShellNode::Computer;
        path = L"";
        break;
    default:
        // Known special folders are namespace directories even when an app
        // has not created their backing LocalStorage folder yet.
        node = VirtualShellNode::Directory;
        break;
    }
    *itemIdList = CreateVirtualPidl(node, path);
    if (!*itemIdList)
    {
        return E_OUTOFMEMORY;
    }
    RuntimeDiagnostics::Record(L"SHELL: SHGetSpecialFolderLocation returned a typed bridge PIDL.");
    return S_OK;
}
BOOL WINAPI Win32Bridge::Bridge::BridgeSHGetSpecialFolderPathW(HWND, LPWSTR path, int folder, BOOL)
{
    if (!path) return FALSE;
    wcscpy_s(path, MAX_PATH, VirtualSpecialFolderPath(folder));
    RuntimeDiagnostics::Record(L"SHELL: SHGetSpecialFolderPathW returned a virtual path.");
    return TRUE;
}
int WINAPI Win32Bridge::Bridge::BridgeSHFileOperationW(PVOID operation)
{
    auto* request = static_cast<GuestShellFileOperationW*>(operation);
    if (!request || !request->from)
    {
        return ERROR_INVALID_PARAMETER;
    }
    request->anyOperationsAborted = FALSE;
    request->nameMappings = nullptr;
    const std::vector<std::wstring> sources = ReadGuestPathList(request->from);
    const std::vector<std::wstring> destinations = ReadGuestPathList(request->to);
    if (sources.empty())
    {
        return ERROR_INVALID_PARAMETER;
    }
    if ((request->operation == ShellFileCopy || request->operation == ShellFileMove ||
        request->operation == ShellFileRename) && destinations.empty())
    {
        return ERROR_INVALID_PARAMETER;
    }

    RuntimeDiagnostics::Record(L"SHELL: virtual SHFileOperationW requested.");
    for (size_t index = 0; index < sources.size(); ++index)
    {
        const std::wstring& source = sources[index];
        const std::wstring& destination = destinations.empty()
            ? std::wstring()
            : destinations[(std::min)(index, destinations.size() - 1)];
        const std::wstring target = FileOperationTarget(source, destination, sources.size() > 1);
        BOOL succeeded = FALSE;
        switch (request->operation)
        {
        case ShellFileCopy:
            succeeded = CopyGuestTree(source, target, 0) ? TRUE : FALSE;
            break;
        case ShellFileMove:
        case ShellFileRename:
            succeeded = BridgeMoveFileExW(source.c_str(), target.c_str(), 0);
            break;
        case ShellFileDelete:
            succeeded = DeleteGuestTree(source, 0) ? TRUE : FALSE;
            break;
        default:
            return ERROR_CALL_NOT_IMPLEMENTED;
        }
        if (!succeeded)
        {
            const DWORD error = BridgeGetLastError();
            request->anyOperationsAborted = TRUE;
            RuntimeDiagnostics::Record(L"SHELL: virtual SHFileOperationW failed.");
            return error == ERROR_SUCCESS ? ERROR_GEN_FAILURE : static_cast<int>(error);
        }
    }
    RuntimeDiagnostics::Record(L"SHELL: virtual SHFileOperationW completed.");
    return 0;
}
void WINAPI Win32Bridge::Bridge::BridgeSHChangeNotify(LONG, UINT, LPCVOID, LPCVOID) { }
BOOL WINAPI Win32Bridge::Bridge::BridgeShellExecuteExW(PVOID) { return FALSE; }
HINSTANCE WINAPI Win32Bridge::Bridge::BridgeShellExecuteW(HWND, LPCWSTR, LPCWSTR, LPCWSTR, LPCWSTR, INT)
{
    return reinterpret_cast<HINSTANCE>(static_cast<INT_PTR>(5)); // SE_ERR_ACCESSDENIED
}

HRESULT WINAPI Win32Bridge::Bridge::BridgeSHGetFolderPathW(
    HWND, int folder, HANDLE, DWORD, LPWSTR path)
{
    if (!path) return E_INVALIDARG;
    const wchar_t* value = folder == 5 ? L"C:\\Users\\Default\\Documents" :
        folder == 26 ? L"C:\\Users\\Default\\AppData\\Roaming" :
        L"C:\\Users\\Default";
    wcscpy_s(path, MAX_PATH, value);
    return S_OK;
}
void WINAPI Win32Bridge::Bridge::BridgeDragAcceptFiles(HWND, BOOL) { }
void WINAPI Win32Bridge::Bridge::BridgeDragFinish(HANDLE) { }
void WINAPI Win32Bridge::Bridge::BridgeSHAddToRecentDocs(UINT, LPCVOID) { }
HRESULT WINAPI Win32Bridge::Bridge::BridgeSHCreateItemFromParsingName(
    PCWSTR, PVOID, REFIID, void** result)
{
    if (result) *result = nullptr;
    return E_NOTIMPL;
}
int WINAPI Win32Bridge::Bridge::BridgeShellAboutW(HWND owner, LPCWSTR title, LPCWSTR text, HICON icon)
{
    return ShowGuestShellAbout(owner, title, text, icon);
}
UINT WINAPI Win32Bridge::Bridge::BridgeDragQueryFileW(HANDLE, UINT file, LPWSTR, UINT)
{
    return file == 0xffffffffu ? 0 : 0;
}
BOOL WINAPI BridgeDragQueryPoint(HANDLE, LPPOINT point)
{
    if (point) *point = POINT{};
    return FALSE;
}
HRESULT WINAPI BridgeSHParseDisplayName(
    LPCWSTR name, PVOID, PVOID* itemIdList, DWORD attributes, DWORD* resolvedAttributes)
{
    if (!itemIdList) return E_POINTER;
    *itemIdList = nullptr;
    if (!name || !*name) return E_INVALIDARG;
    const VirtualShellNode node = NodeForGuestPath(name);
    *itemIdList = CreateVirtualPidl(node, name);
    if (!*itemIdList) return E_OUTOFMEMORY;
    if (resolvedAttributes)
    {
        DWORD value = 0;
        if (node == VirtualShellNode::Directory || node == VirtualShellNode::Drive)
            value |= attributes & 0x20000000u; // SFGAO_FOLDER
        value |= attributes & 0x40000000u; // SFGAO_FILESYSTEM
        *resolvedAttributes = value;
    }
    return S_OK;
}
HRESULT WINAPI BridgeSHOpenFolderAndSelectItems(PVOID folder, UINT count, PVOID*, DWORD)
{
    VirtualPidlData data{};
    return ReadVirtualPidl(folder, &data) && count <= 0xffffu ? S_OK : E_INVALIDARG;
}
BOOL WINAPI BridgeShellNotifyIconW(DWORD message, PVOID data)
{
    // The bridge has no system notification area.  Accept well-formed add,
    // modify and delete requests as a virtual tray so applications do not
    // abort merely because the host shell is intentionally absent.
    if (!data || message > 4u) return FALSE;
    return TRUE;
}
int WINAPI BridgeSHCreateDirectory(HWND, LPCWSTR path)
{
    if (!path || !*path) return ERROR_INVALID_PARAMETER;
    if (BridgeCreateDirectoryW(path, nullptr)) return ERROR_SUCCESS;
    const DWORD error = BridgeGetLastError();
    return error == ERROR_ALREADY_EXISTS ? ERROR_SUCCESS : static_cast<int>(error);
}

Win32Bridge::Bridge::ImportResolution Win32Bridge::Bridge::ResolveShell32Import(const ImportedSymbol& symbol)
{
    auto resolution = CompatibilityCatalog::Resolve(symbol);
    if (_wcsicmp(symbol.library.c_str(), L"shell32.dll") != 0) return resolution;
    if (symbol.importedByOrdinal)
    {
        if (symbol.ordinal == 165)
        {
            resolution.targetAddress = reinterpret_cast<ULONGLONG>(&BridgeSHCreateDirectory);
            resolution.disposition = ImportDisposition::NeedsBridge;
        }
        return resolution;
    }
    if (_wcsicmp(symbol.name.c_str(), L"shbrowseforfolderw") == 0)
        resolution.targetAddress = reinterpret_cast<ULONGLONG>(&BridgeSHBrowseForFolderW);
    else if (_wcsicmp(symbol.name.c_str(), L"shgetfileinfow") == 0)
        resolution.targetAddress = reinterpret_cast<ULONGLONG>(&BridgeSHGetFileInfoW);
    else if (_wcsicmp(symbol.name.c_str(), L"shgetpathfromidlistw") == 0)
        resolution.targetAddress = reinterpret_cast<ULONGLONG>(&BridgeSHGetPathFromIDListW);
    else if (_wcsicmp(symbol.name.c_str(), L"extracticonexw") == 0)
        resolution.targetAddress = reinterpret_cast<ULONGLONG>(&BridgeExtractIconExW);
    else if (_wcsicmp(symbol.name.c_str(), L"extracticonexa") == 0)
        resolution.targetAddress = reinterpret_cast<ULONGLONG>(&BridgeExtractIconExA);
    else if (_wcsicmp(symbol.name.c_str(), L"shgetdesktopfolder") == 0)
        resolution.targetAddress = reinterpret_cast<ULONGLONG>(&BridgeSHGetDesktopFolder);
    else if (_wcsicmp(symbol.name.c_str(), L"shgetspecialfolderlocation") == 0)
        resolution.targetAddress = reinterpret_cast<ULONGLONG>(&BridgeSHGetSpecialFolderLocation);
    else if (_wcsicmp(symbol.name.c_str(), L"shgetspecialfolderpathw") == 0)
        resolution.targetAddress = reinterpret_cast<ULONGLONG>(&BridgeSHGetSpecialFolderPathW);
    else if (_wcsicmp(symbol.name.c_str(), L"shfileoperationw") == 0)
        resolution.targetAddress = reinterpret_cast<ULONGLONG>(&BridgeSHFileOperationW);
    else if (_wcsicmp(symbol.name.c_str(), L"shchangenotify") == 0)
        resolution.targetAddress = reinterpret_cast<ULONGLONG>(&BridgeSHChangeNotify);
    else if (_wcsicmp(symbol.name.c_str(), L"shellexecuteexw") == 0)
        resolution.targetAddress = reinterpret_cast<ULONGLONG>(&BridgeShellExecuteExW);
    else if (_wcsicmp(symbol.name.c_str(), L"shellexecutew") == 0)
        resolution.targetAddress = reinterpret_cast<ULONGLONG>(&BridgeShellExecuteW);
    else if (_wcsicmp(symbol.name.c_str(), L"shgetfolderpathw") == 0) resolution.targetAddress = reinterpret_cast<ULONGLONG>(&BridgeSHGetFolderPathW);
    else if (_wcsicmp(symbol.name.c_str(), L"dragacceptfiles") == 0) resolution.targetAddress = reinterpret_cast<ULONGLONG>(&BridgeDragAcceptFiles);
    else if (_wcsicmp(symbol.name.c_str(), L"dragfinish") == 0) resolution.targetAddress = reinterpret_cast<ULONGLONG>(&BridgeDragFinish);
    else if (_wcsicmp(symbol.name.c_str(), L"shaddtorecentdocs") == 0) resolution.targetAddress = reinterpret_cast<ULONGLONG>(&BridgeSHAddToRecentDocs);
    else if (_wcsicmp(symbol.name.c_str(), L"shcreateitemfromparsingname") == 0) resolution.targetAddress = reinterpret_cast<ULONGLONG>(&BridgeSHCreateItemFromParsingName);
    else if (_wcsicmp(symbol.name.c_str(), L"shellaboutw") == 0) resolution.targetAddress = reinterpret_cast<ULONGLONG>(&BridgeShellAboutW);
    else if (_wcsicmp(symbol.name.c_str(), L"dragqueryfilew") == 0) resolution.targetAddress = reinterpret_cast<ULONGLONG>(&BridgeDragQueryFileW);
    else if (_wcsicmp(symbol.name.c_str(), L"dragquerypoint") == 0) resolution.targetAddress = reinterpret_cast<ULONGLONG>(&BridgeDragQueryPoint);
    else if (_wcsicmp(symbol.name.c_str(), L"shparsedisplayname") == 0) resolution.targetAddress = reinterpret_cast<ULONGLONG>(&BridgeSHParseDisplayName);
    else if (_wcsicmp(symbol.name.c_str(), L"shopenfolderandselectitems") == 0) resolution.targetAddress = reinterpret_cast<ULONGLONG>(&BridgeSHOpenFolderAndSelectItems);
    else if (_wcsicmp(symbol.name.c_str(), L"shell_notifyiconw") == 0) resolution.targetAddress = reinterpret_cast<ULONGLONG>(&BridgeShellNotifyIconW);
    if (resolution.targetAddress)
    {
        resolution.disposition = ImportDisposition::NeedsBridge;
        resolution.note = L"Shell adapter: virtual files and UWP pickers; desktop PIDLs are not exposed to the guest.";
    }
    return resolution;
}
