#include "pch.h"
#include "Bridge/Shell32Shims.h"
#include "Bridge/Kernel32Shims.h"
#include "Bridge/OleShims.h"
#include "Bridge/RuntimeDiagnostics.h"

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <new>
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

    // Minimal ABI-only IShellFolder/IEnumIDList implementation.  This is the
    // Wine-style shell boundary translated to the bridge's LocalFolder-backed
    // namespace: it exposes one virtual drive, not the host desktop.
    struct VirtualEnumIdList;
    struct VirtualShellFolder;
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

    VirtualEnumIdList* CreateVirtualEnumIdList(std::vector<VirtualPidlData> items = {});
    VirtualShellFolder* CreateVirtualShellFolder(VirtualShellNode node = VirtualShellNode::Desktop,
        const std::wstring& path = std::wstring());

    constexpr GUID VirtualIidEnumIdList =
        { 0x000214f2, 0x0000, 0x0000, { 0xc0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x46 } };
    constexpr GUID VirtualIidShellFolder =
        { 0x000214e6, 0x0000, 0x0000, { 0xc0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x46 } };

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
    HRESULT STDMETHODCALLTYPE FolderGetUiObjectOf(void*, HWND, UINT, PVOID const*, REFIID, UINT*, void**) { return E_NOINTERFACE; }
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
    wcsncpy_s(result->displayName, _countof(result->displayName), display.c_str(), _TRUNCATE);
    wcsncpy_s(result->typeName, _countof(result->typeName),
        directory ? L"File folder" : L"File", _TRUNCATE);
    RuntimeDiagnostics::Record(flags & ShgfiPidl
        ? L"SHELL: SHGetFileInfoW resolved bridge PIDL metadata."
        : L"SHELL: SHGetFileInfoW resolved virtual path metadata.");
    return 1;
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

UINT WINAPI Win32Bridge::Bridge::BridgeExtractIconExW(LPCWSTR, int, HICON* largeIcons, HICON* smallIcons, UINT iconCount)
{
    if (largeIcons) for (UINT index = 0; index < iconCount; ++index) largeIcons[index] = nullptr;
    if (smallIcons) for (UINT index = 0; index < iconCount; ++index) smallIcons[index] = nullptr;
    return 0;
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

Win32Bridge::Bridge::ImportResolution Win32Bridge::Bridge::ResolveShell32Import(const ImportedSymbol& symbol)
{
    auto resolution = CompatibilityCatalog::Resolve(symbol);
    if (symbol.importedByOrdinal || _wcsicmp(symbol.library.c_str(), L"shell32.dll") != 0) return resolution;
    if (_wcsicmp(symbol.name.c_str(), L"shbrowseforfolderw") == 0)
        resolution.targetAddress = reinterpret_cast<ULONGLONG>(&BridgeSHBrowseForFolderW);
    else if (_wcsicmp(symbol.name.c_str(), L"shgetfileinfow") == 0)
        resolution.targetAddress = reinterpret_cast<ULONGLONG>(&BridgeSHGetFileInfoW);
    else if (_wcsicmp(symbol.name.c_str(), L"shgetpathfromidlistw") == 0)
        resolution.targetAddress = reinterpret_cast<ULONGLONG>(&BridgeSHGetPathFromIDListW);
    else if (_wcsicmp(symbol.name.c_str(), L"extracticonexw") == 0)
        resolution.targetAddress = reinterpret_cast<ULONGLONG>(&BridgeExtractIconExW);
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
    if (resolution.targetAddress)
    {
        resolution.disposition = ImportDisposition::NeedsBridge;
        resolution.note = L"Shell adapter: virtual files and UWP pickers; desktop PIDLs are not exposed to the guest.";
    }
    return resolution;
}
