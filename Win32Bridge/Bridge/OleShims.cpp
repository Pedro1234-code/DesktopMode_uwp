#include "pch.h"
#include "Bridge/OleShims.h"
#include "Bridge/Gdi32Shims.h"
#include "Bridge/Kernel32Shims.h"
#include "Bridge/RuntimeDiagnostics.h"
#include "Bridge/User32Shims.h"
#include "Bridge/Win32Shims.h"

#include <chrono>
#include <mutex>
#include <atomic>
#include <new>
#include <string>
#include <thread>
#include <vector>
#include <unordered_map>

using namespace Win32Bridge::Bridge;
namespace
{
    thread_local unsigned g_comCount = 0;
    thread_local DWORD g_comApartment = 0xffffffffu;
    std::mutex g_dropTargetsLock;
    std::unordered_map<ULONG_PTR, IUnknown*> g_dropTargets;
    constexpr GUID GuestClsidShellLink =
        { 0x00021401, 0x0000, 0x0000, { 0xc0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x46 } };
    constexpr GUID GuestIidShellLinkW =
        { 0x000214f9, 0x0000, 0x0000, { 0xc0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x46 } };
    constexpr GUID GuestIidPersist =
        { 0x0000010c, 0x0000, 0x0000, { 0xc0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x46 } };
    constexpr GUID GuestIidPersistFile =
        { 0x0000010b, 0x0000, 0x0000, { 0xc0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x46 } };
    constexpr GUID GuestIidUnknown =
        { 0x00000000, 0x0000, 0x0000, { 0xc0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x46 } };

    bool GuestComAddRef(IUnknown* value);
    void GuestComRelease(IUnknown* value);

    struct GuestIDropSource : IUnknown
    {
        virtual HRESULT STDMETHODCALLTYPE QueryContinueDrag(BOOL escapePressed,
            DWORD keyState) = 0;
        virtual HRESULT STDMETHODCALLTYPE GiveFeedback(DWORD effect) = 0;
    };

    struct GuestIDropTarget : IUnknown
    {
        virtual HRESULT STDMETHODCALLTYPE DragEnter(IDataObject* dataObject,
            DWORD keyState, POINTL point, DWORD* effect) = 0;
        virtual HRESULT STDMETHODCALLTYPE DragOver(DWORD keyState, POINTL point,
            DWORD* effect) = 0;
        virtual HRESULT STDMETHODCALLTYPE DragLeave() = 0;
        virtual HRESULT STDMETHODCALLTYPE Drop(IDataObject* dataObject,
            DWORD keyState, POINTL point, DWORD* effect) = 0;
    };

    HRESULT SafeQueryContinueDrag(GuestIDropSource* source, BOOL escapePressed,
        DWORD keyState)
    {
        __try { return source->QueryContinueDrag(escapePressed, keyState); }
        __except (EXCEPTION_EXECUTE_HANDLER) { return E_UNEXPECTED; }
    }

    HRESULT SafeGiveFeedback(GuestIDropSource* source, DWORD effect)
    {
        __try { return source->GiveFeedback(effect); }
        __except (EXCEPTION_EXECUTE_HANDLER) { return E_UNEXPECTED; }
    }

    HRESULT SafeDragEnter(GuestIDropTarget* target, IDataObject* dataObject,
        DWORD keyState, POINTL point, DWORD* effect)
    {
        __try { return target->DragEnter(dataObject, keyState, point, effect); }
        __except (EXCEPTION_EXECUTE_HANDLER) { return E_UNEXPECTED; }
    }

    HRESULT SafeDragOver(GuestIDropTarget* target, DWORD keyState, POINTL point,
        DWORD* effect)
    {
        __try { return target->DragOver(keyState, point, effect); }
        __except (EXCEPTION_EXECUTE_HANDLER) { return E_UNEXPECTED; }
    }

    HRESULT SafeDragLeave(GuestIDropTarget* target)
    {
        __try { return target->DragLeave(); }
        __except (EXCEPTION_EXECUTE_HANDLER) { return E_UNEXPECTED; }
    }

    HRESULT SafeDrop(GuestIDropTarget* target, IDataObject* dataObject,
        DWORD keyState, POINTL point, DWORD* effect)
    {
        __try { return target->Drop(dataObject, keyState, point, effect); }
        __except (EXCEPTION_EXECUTE_HANDLER) { return E_UNEXPECTED; }
    }

    DWORD DragKeyState()
    {
        constexpr DWORD MouseKeyLeft = 0x0001u;
        constexpr DWORD MouseKeyRight = 0x0002u;
        constexpr DWORD MouseKeyShift = 0x0004u;
        constexpr DWORD MouseKeyControl = 0x0008u;
        constexpr DWORD MouseKeyMiddle = 0x0010u;
        DWORD state = 0;
        if (BridgeGetKeyState(VK_LBUTTON) < 0) state |= MouseKeyLeft;
        if (BridgeGetKeyState(VK_RBUTTON) < 0) state |= MouseKeyRight;
        if (BridgeGetKeyState(VK_MBUTTON) < 0) state |= MouseKeyMiddle;
        if (BridgeGetKeyState(VK_SHIFT) < 0) state |= MouseKeyShift;
        if (BridgeGetKeyState(VK_CONTROL) < 0) state |= MouseKeyControl;
        return state;
    }

    GuestIDropTarget* RegisteredDropTargetAt(POINT point, HWND* targetWindow)
    {
        if (targetWindow) *targetWindow = nullptr;
        HWND window = BridgeWindowFromPoint(point);
        while (window)
        {
            IUnknown* registered = nullptr;
            {
                std::lock_guard<std::mutex> guard(g_dropTargetsLock);
                const auto found = g_dropTargets.find(
                    reinterpret_cast<ULONG_PTR>(window));
                if (found != g_dropTargets.end() && GuestComAddRef(found->second))
                    registered = found->second;
            }
            if (registered)
            {
                if (targetWindow) *targetWindow = window;
                return reinterpret_cast<GuestIDropTarget*>(registered);
            }
            window = BridgeGetParent(window);
        }
        return nullptr;
    }

    // The desktop shell headers are not available under the UWP header
    // partition used by this project. These interfaces deliberately reproduce
    // only the documented COM ABI; guest code sees the same vtable layout and
    // never receives a host shell object.
    struct GuestIShellLinkW : IUnknown
    {
        virtual HRESULT STDMETHODCALLTYPE GetPath(LPWSTR, int, WIN32_FIND_DATAW*, DWORD) = 0;
        virtual HRESULT STDMETHODCALLTYPE GetIDList(void**) = 0;
        virtual HRESULT STDMETHODCALLTYPE SetIDList(const void*) = 0;
        virtual HRESULT STDMETHODCALLTYPE GetDescription(LPWSTR, int) = 0;
        virtual HRESULT STDMETHODCALLTYPE SetDescription(LPCWSTR) = 0;
        virtual HRESULT STDMETHODCALLTYPE GetWorkingDirectory(LPWSTR, int) = 0;
        virtual HRESULT STDMETHODCALLTYPE SetWorkingDirectory(LPCWSTR) = 0;
        virtual HRESULT STDMETHODCALLTYPE GetArguments(LPWSTR, int) = 0;
        virtual HRESULT STDMETHODCALLTYPE SetArguments(LPCWSTR) = 0;
        virtual HRESULT STDMETHODCALLTYPE GetHotkey(WORD*) = 0;
        virtual HRESULT STDMETHODCALLTYPE SetHotkey(WORD) = 0;
        virtual HRESULT STDMETHODCALLTYPE GetShowCmd(int*) = 0;
        virtual HRESULT STDMETHODCALLTYPE SetShowCmd(int) = 0;
        virtual HRESULT STDMETHODCALLTYPE GetIconLocation(LPWSTR, int, int*) = 0;
        virtual HRESULT STDMETHODCALLTYPE SetIconLocation(LPCWSTR, int) = 0;
        virtual HRESULT STDMETHODCALLTYPE SetRelativePath(LPCWSTR, DWORD) = 0;
        virtual HRESULT STDMETHODCALLTYPE Resolve(HWND, DWORD) = 0;
        virtual HRESULT STDMETHODCALLTYPE SetPath(LPCWSTR) = 0;
    };

    struct GuestIPersistFile : IUnknown
    {
        virtual HRESULT STDMETHODCALLTYPE GetClassID(CLSID*) = 0;
        virtual HRESULT STDMETHODCALLTYPE IsDirty() = 0;
        virtual HRESULT STDMETHODCALLTYPE Load(LPCOLESTR, DWORD) = 0;
        virtual HRESULT STDMETHODCALLTYPE Save(LPCOLESTR, BOOL) = 0;
        virtual HRESULT STDMETHODCALLTYPE SaveCompleted(LPCOLESTR) = 0;
        virtual HRESULT STDMETHODCALLTYPE GetCurFile(LPOLESTR*) = 0;
    };

    template<typename T>
    void AppendValue(std::vector<BYTE>* bytes, const T& value)
    {
        const BYTE* first = reinterpret_cast<const BYTE*>(&value);
        bytes->insert(bytes->end(), first, first + sizeof(value));
    }

    void AppendBytes(std::vector<BYTE>* bytes, const void* value, size_t size)
    {
        const BYTE* first = static_cast<const BYTE*>(value);
        bytes->insert(bytes->end(), first, first + size);
    }

    HRESULT CopyShellLinkString(const std::wstring& value, LPWSTR buffer, int count)
    {
        if (!buffer || count <= 0) return E_INVALIDARG;
        const size_t capacity = static_cast<size_t>(count);
        const size_t copied = (std::min)(value.size(), capacity - 1);
        if (copied) memcpy(buffer, value.data(), copied * sizeof(wchar_t));
        buffer[copied] = L'\0';
        return copied == value.size() ? S_OK : HRESULT_FROM_WIN32(ERROR_INSUFFICIENT_BUFFER);
    }

#pragma pack(push, 1)
    struct ShellLinkHeader final
    {
        DWORD size = 0x4c;
        GUID clsid = GuestClsidShellLink;
        DWORD flags = 0;
        DWORD attributes = FILE_ATTRIBUTE_NORMAL;
        FILETIME creation{};
        FILETIME access{};
        FILETIME write{};
        DWORD fileSize = 0;
        LONG iconIndex = 0;
        DWORD showCommand = SW_SHOWNORMAL;
        WORD hotKey = 0;
        WORD reserved1 = 0;
        DWORD reserved2 = 0;
        DWORD reserved3 = 0;
    };

    struct ShellLinkInfoHeader final
    {
        DWORD size = 0;
        DWORD headerSize = 0x24;
        DWORD flags = 1;
        DWORD volumeIdOffset = 0;
        DWORD localBasePathOffset = 0;
        DWORD commonNetworkRelativeLinkOffset = 0;
        DWORD commonPathSuffixOffset = 0;
        DWORD localBasePathOffsetUnicode = 0;
        DWORD commonPathSuffixOffsetUnicode = 0;
    };

    struct ShellLinkVolumeHeader final
    {
        DWORD size = 0;
        DWORD driveType = DRIVE_FIXED;
        DWORD serialNumber = 0x57333242;
        DWORD labelOffset = 0x10;
    };
#pragma pack(pop)

    static_assert(sizeof(ShellLinkHeader) == 0x4c, "Shell Link header ABI mismatch");
    static_assert(sizeof(ShellLinkInfoHeader) == 0x24, "Shell LinkInfo header ABI mismatch");

    class GuestShellLink final : public GuestIShellLinkW, public GuestIPersistFile
    {
    public:
        HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void** result) override
        {
            if (!result) return E_POINTER;
            *result = nullptr;
            if (IsEqualGUID(iid, GuestIidUnknown) || IsEqualGUID(iid, GuestIidShellLinkW))
                *result = static_cast<GuestIShellLinkW*>(this);
            else if (IsEqualGUID(iid, GuestIidPersist) || IsEqualGUID(iid, GuestIidPersistFile))
                *result = static_cast<GuestIPersistFile*>(this);
            else
                return E_NOINTERFACE;
            AddRef();
            return S_OK;
        }

        ULONG STDMETHODCALLTYPE AddRef() override { return ++m_references; }
        ULONG STDMETHODCALLTYPE Release() override
        {
            const ULONG references = --m_references;
            if (!references) delete this;
            return references;
        }

        HRESULT STDMETHODCALLTYPE GetPath(LPWSTR path, int count,
            WIN32_FIND_DATAW* data, DWORD) override
        {
            if (data)
            {
                ZeroMemory(data, sizeof(*data));
                const DWORD attributes = m_target.empty()
                    ? INVALID_FILE_ATTRIBUTES : BridgeGetFileAttributesW(m_target.c_str());
                data->dwFileAttributes = attributes == INVALID_FILE_ATTRIBUTES
                    ? FILE_ATTRIBUTE_NORMAL : attributes;
            }
            return CopyShellLinkString(m_target, path, count);
        }
        HRESULT STDMETHODCALLTYPE GetIDList(void** value) override
        { if (value) *value = nullptr; return value ? E_NOTIMPL : E_POINTER; }
        HRESULT STDMETHODCALLTYPE SetIDList(const void*) override { return E_NOTIMPL; }
        HRESULT STDMETHODCALLTYPE GetDescription(LPWSTR value, int count) override
        { return CopyShellLinkString(m_description, value, count); }
        HRESULT STDMETHODCALLTYPE SetDescription(LPCWSTR value) override
        { m_description = value ? value : L""; m_dirty = true; return S_OK; }
        HRESULT STDMETHODCALLTYPE GetWorkingDirectory(LPWSTR value, int count) override
        { return CopyShellLinkString(m_workingDirectory, value, count); }
        HRESULT STDMETHODCALLTYPE SetWorkingDirectory(LPCWSTR value) override
        { m_workingDirectory = value ? value : L""; m_dirty = true; return S_OK; }
        HRESULT STDMETHODCALLTYPE GetArguments(LPWSTR value, int count) override
        { return CopyShellLinkString(m_arguments, value, count); }
        HRESULT STDMETHODCALLTYPE SetArguments(LPCWSTR value) override
        { m_arguments = value ? value : L""; m_dirty = true; return S_OK; }
        HRESULT STDMETHODCALLTYPE GetHotkey(WORD* value) override
        { if (!value) return E_POINTER; *value = m_hotKey; return S_OK; }
        HRESULT STDMETHODCALLTYPE SetHotkey(WORD value) override
        { m_hotKey = value; m_dirty = true; return S_OK; }
        HRESULT STDMETHODCALLTYPE GetShowCmd(int* value) override
        { if (!value) return E_POINTER; *value = m_showCommand; return S_OK; }
        HRESULT STDMETHODCALLTYPE SetShowCmd(int value) override
        { m_showCommand = value; m_dirty = true; return S_OK; }
        HRESULT STDMETHODCALLTYPE GetIconLocation(LPWSTR value, int count, int* index) override
        {
            if (!index) return E_POINTER;
            *index = m_iconIndex;
            return CopyShellLinkString(m_iconPath, value, count);
        }
        HRESULT STDMETHODCALLTYPE SetIconLocation(LPCWSTR value, int index) override
        { m_iconPath = value ? value : L""; m_iconIndex = index; m_dirty = true; return S_OK; }
        HRESULT STDMETHODCALLTYPE SetRelativePath(LPCWSTR value, DWORD) override
        { m_relativePath = value ? value : L""; m_dirty = true; return S_OK; }
        HRESULT STDMETHODCALLTYPE Resolve(HWND, DWORD) override
        { return m_target.empty() ? S_FALSE : S_OK; }
        HRESULT STDMETHODCALLTYPE SetPath(LPCWSTR value) override
        {
            if (!value || !*value) return E_INVALIDARG;
            m_target = value;
            m_dirty = true;
            return S_OK;
        }

        HRESULT STDMETHODCALLTYPE GetClassID(CLSID* value) override
        { if (!value) return E_POINTER; *value = GuestClsidShellLink; return S_OK; }
        HRESULT STDMETHODCALLTYPE IsDirty() override { return m_dirty ? S_OK : S_FALSE; }
        HRESULT STDMETHODCALLTYPE Load(LPCOLESTR, DWORD) override { return E_NOTIMPL; }
        HRESULT STDMETHODCALLTYPE Save(LPCOLESTR fileName, BOOL remember) override
        {
            if (!fileName || !*fileName || m_target.empty()) return E_INVALIDARG;
            const std::vector<BYTE> bytes = Serialize();
            if (bytes.size() > MAXDWORD) return HRESULT_FROM_WIN32(ERROR_FILE_TOO_LARGE);
            HANDLE file = BridgeCreateFileW(fileName, GENERIC_WRITE, FILE_SHARE_READ,
                nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
            if (file == INVALID_HANDLE_VALUE)
                return HRESULT_FROM_WIN32(BridgeGetLastError());
            DWORD written = 0;
            const BOOL success = BridgeWriteFile(file, bytes.data(),
                static_cast<DWORD>(bytes.size()), &written, nullptr) &&
                written == bytes.size();
            const DWORD error = success ? ERROR_SUCCESS : BridgeGetLastError();
            BridgeCloseHandle(file);
            if (!success) return HRESULT_FROM_WIN32(error == ERROR_SUCCESS ? ERROR_WRITE_FAULT : error);
            if (remember) m_currentFile = fileName;
            m_dirty = false;
            RuntimeDiagnostics::Record(L"COM: saved a virtual Shell Link at " +
                std::wstring(fileName) + L".");
            return S_OK;
        }
        HRESULT STDMETHODCALLTYPE SaveCompleted(LPCOLESTR fileName) override
        { if (fileName) m_currentFile = fileName; return S_OK; }
        HRESULT STDMETHODCALLTYPE GetCurFile(LPOLESTR* value) override
        {
            if (!value) return E_POINTER;
            *value = nullptr;
            const size_t bytes = (m_currentFile.size() + 1) * sizeof(wchar_t);
            auto* copy = static_cast<wchar_t*>(BridgeCoTaskMemAlloc(bytes));
            if (!copy) return E_OUTOFMEMORY;
            memcpy(copy, m_currentFile.c_str(), bytes);
            *value = copy;
            return S_OK;
        }

    private:
        void AppendStringData(std::vector<BYTE>* bytes, const std::wstring& value) const
        {
            const WORD count = static_cast<WORD>((std::min)(value.size(),
                static_cast<size_t>(MAXWORD)));
            AppendValue(bytes, count);
            AppendBytes(bytes, value.data(), static_cast<size_t>(count) * sizeof(wchar_t));
        }

        std::vector<BYTE> Serialize() const
        {
            constexpr DWORD HasLinkInfo = 0x00000002;
            constexpr DWORD HasName = 0x00000004;
            constexpr DWORD HasRelativePath = 0x00000008;
            constexpr DWORD HasWorkingDirectory = 0x00000010;
            constexpr DWORD HasArguments = 0x00000020;
            constexpr DWORD HasIconLocation = 0x00000040;
            constexpr DWORD IsUnicode = 0x00000080;

            ShellLinkHeader header{};
            header.flags = HasLinkInfo | IsUnicode;
            if (!m_description.empty()) header.flags |= HasName;
            if (!m_relativePath.empty()) header.flags |= HasRelativePath;
            if (!m_workingDirectory.empty()) header.flags |= HasWorkingDirectory;
            if (!m_arguments.empty()) header.flags |= HasArguments;
            if (!m_iconPath.empty()) header.flags |= HasIconLocation;
            header.iconIndex = m_iconIndex;
            header.showCommand = static_cast<DWORD>(m_showCommand);
            header.hotKey = m_hotKey;

            std::vector<BYTE> bytes;
            AppendValue(&bytes, header);

            ShellLinkInfoHeader info{};
            std::vector<BYTE> linkInfo(sizeof(info), 0);
            info.volumeIdOffset = sizeof(info);
            ShellLinkVolumeHeader volume{};
            const char volumeLabel[] = "Win32Bridge";
            volume.size = sizeof(volume) + sizeof(volumeLabel);
            AppendValue(&linkInfo, volume);
            AppendBytes(&linkInfo, volumeLabel, sizeof(volumeLabel));

            std::string ansiPath;
            const int ansiLength = WideCharToMultiByte(CP_ACP, 0, m_target.c_str(), -1,
                nullptr, 0, nullptr, nullptr);
            if (ansiLength > 0)
            {
                ansiPath.resize(static_cast<size_t>(ansiLength));
                WideCharToMultiByte(CP_ACP, 0, m_target.c_str(), -1,
                    &ansiPath[0], ansiLength, nullptr, nullptr);
            }
            else ansiPath.assign(1, '\0');
            info.localBasePathOffset = static_cast<DWORD>(linkInfo.size());
            AppendBytes(&linkInfo, ansiPath.data(), ansiPath.size());
            info.commonPathSuffixOffset = static_cast<DWORD>(linkInfo.size());
            linkInfo.push_back(0);
            if ((linkInfo.size() & 1) != 0) linkInfo.push_back(0);
            info.localBasePathOffsetUnicode = static_cast<DWORD>(linkInfo.size());
            AppendBytes(&linkInfo, m_target.c_str(), (m_target.size() + 1) * sizeof(wchar_t));
            info.commonPathSuffixOffsetUnicode = static_cast<DWORD>(linkInfo.size());
            const wchar_t empty = L'\0';
            AppendValue(&linkInfo, empty);
            info.size = static_cast<DWORD>(linkInfo.size());
            memcpy(linkInfo.data(), &info, sizeof(info));
            bytes.insert(bytes.end(), linkInfo.begin(), linkInfo.end());

            if (!m_description.empty()) AppendStringData(&bytes, m_description);
            if (!m_relativePath.empty()) AppendStringData(&bytes, m_relativePath);
            if (!m_workingDirectory.empty()) AppendStringData(&bytes, m_workingDirectory);
            if (!m_arguments.empty()) AppendStringData(&bytes, m_arguments);
            if (!m_iconPath.empty()) AppendStringData(&bytes, m_iconPath);
            const DWORD terminalBlock = 0;
            AppendValue(&bytes, terminalBlock);
            return bytes;
        }

        std::atomic<ULONG> m_references{ 1 };
        bool m_dirty = false;
        std::wstring m_target;
        std::wstring m_description;
        std::wstring m_workingDirectory;
        std::wstring m_arguments;
        std::wstring m_iconPath;
        std::wstring m_relativePath;
        std::wstring m_currentFile;
        WORD m_hotKey = 0;
        int m_showCommand = SW_SHOWNORMAL;
        int m_iconIndex = 0;
    };
    bool Name(const std::wstring& value, const wchar_t* expected)
    {
        return _wcsicmp(value.c_str(), expected) == 0;
    }
    bool GuestComAddRef(IUnknown* value)
    {
        __try { value->AddRef(); return true; }
        __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
    }
    void GuestComRelease(IUnknown* value)
    {
        if (!value) return;
        __try { value->Release(); }
        __except (EXCEPTION_EXECUTE_HANDLER) { }
    }
}

void Win32Bridge::Bridge::ResetGuestOleState()
{
    // Do not Release stale guest COM objects here.  On abnormal termination
    // their vtables may already point into an unloaded dependency.  Clearing
    // the emulated registration is the process-teardown operation; the guest
    // address space owns the object's remaining storage.
    {
        std::lock_guard<std::mutex> guard(g_dropTargetsLock);
        g_dropTargets.clear();
    }
    g_comCount = 0;
    g_comApartment = 0xffffffffu;
}

HRESULT WINAPI Win32Bridge::Bridge::BridgeCoInitialize(LPVOID reserved)
{
    return BridgeCoInitializeEx(reserved, COINIT_APARTMENTTHREADED);
}
HRESULT WINAPI Win32Bridge::Bridge::BridgeCoInitializeEx(LPVOID, DWORD flags)
{
    const DWORD apartment = flags & (COINIT_APARTMENTTHREADED | COINIT_MULTITHREADED);
    if (g_comCount && apartment != g_comApartment) return RPC_E_CHANGED_MODE;
    if (!g_comCount) g_comApartment = apartment;
    ++g_comCount;
    return g_comCount == 1 ? S_OK : S_FALSE;
}
HRESULT WINAPI Win32Bridge::Bridge::BridgeOleInitialize(LPVOID reserved)
{
    return BridgeCoInitializeEx(reserved, COINIT_APARTMENTTHREADED);
}
void WINAPI Win32Bridge::Bridge::BridgeCoUninitialize()
{
    if (g_comCount && --g_comCount == 0) g_comApartment = 0xffffffffu;
}
void WINAPI Win32Bridge::Bridge::BridgeOleUninitialize() { BridgeCoUninitialize(); }
LPVOID WINAPI Win32Bridge::Bridge::BridgeCoTaskMemAlloc(SIZE_T size) { return ::CoTaskMemAlloc(size); }
void WINAPI Win32Bridge::Bridge::BridgeCoTaskMemFree(LPVOID memory) { ::CoTaskMemFree(memory); }
HRESULT WINAPI Win32Bridge::Bridge::BridgeCoCreateInstance(REFCLSID clsid, LPUNKNOWN outer, DWORD, REFIID iid, LPVOID* result)
{
    if (!result) return E_POINTER;
    *result = nullptr;
    if (outer) return CLASS_E_NOAGGREGATION;
    if (IsEqualGUID(clsid, GuestClsidShellLink))
    {
        GuestShellLink* link = new (std::nothrow) GuestShellLink();
        if (!link) return E_OUTOFMEMORY;
        const HRESULT status = link->QueryInterface(iid, result);
        link->Release();
        if (SUCCEEDED(status))
            RuntimeDiagnostics::Record(L"COM: created a virtual Shell Link object.");
        return status;
    }
    RuntimeDiagnostics::Record(L"COM: class " + std::to_wstring(clsid.Data1) +
        L" / interface " + std::to_wstring(iid.Data1) + L" is not registered.");
    return REGDB_E_CLASSNOTREG;
}
void WINAPI Win32Bridge::Bridge::BridgeReleaseStgMedium(PVOID value)
{
    if (!value) return;
    auto* medium = static_cast<STGMEDIUM*>(value);
    if (medium->pUnkForRelease)
    {
        GuestComRelease(medium->pUnkForRelease);
    }
    else
    {
        switch (medium->tymed)
        {
        case TYMED_HGLOBAL:
            if (medium->hGlobal) BridgeGlobalFree(medium->hGlobal);
            break;
        case TYMED_GDI:
            if (medium->hBitmap) BridgeDeleteObject(medium->hBitmap);
            break;
        case TYMED_FILE:
            if (medium->lpszFileName) BridgeCoTaskMemFree(medium->lpszFileName);
            break;
        case TYMED_ISTREAM:
            GuestComRelease(medium->pstm);
            break;
        case TYMED_ISTORAGE:
            GuestComRelease(medium->pstg);
            break;
        default:
            break;
        }
    }
    memset(medium, 0, sizeof(*medium));
    medium->tymed = TYMED_NULL;
}
HRESULT WINAPI Win32Bridge::Bridge::BridgeRegisterDragDrop(HWND window, PVOID target)
{
    if (!BridgeIsWindow(window)) return DRAGDROP_E_INVALIDHWND;
    if (!target) return E_INVALIDARG;
    auto* unknown = static_cast<IUnknown*>(target);
    if (!GuestComAddRef(unknown)) return E_UNEXPECTED;
    {
        std::lock_guard<std::mutex> guard(g_dropTargetsLock);
        const ULONG_PTR key = reinterpret_cast<ULONG_PTR>(window);
        if (g_dropTargets.find(key) != g_dropTargets.end())
        {
            GuestComRelease(unknown);
            return DRAGDROP_E_ALREADYREGISTERED;
        }
        g_dropTargets.emplace(key, unknown);
    }
    return S_OK;
}
HRESULT WINAPI Win32Bridge::Bridge::BridgeRevokeDragDrop(HWND window)
{
    IUnknown* target = nullptr;
    {
        std::lock_guard<std::mutex> guard(g_dropTargetsLock);
        const auto found = g_dropTargets.find(reinterpret_cast<ULONG_PTR>(window));
        if (found == g_dropTargets.end()) return DRAGDROP_E_NOTREGISTERED;
        target = found->second;
        g_dropTargets.erase(found);
    }
    GuestComRelease(target);
    return S_OK;
}
HRESULT WINAPI Win32Bridge::Bridge::BridgeDoDragDrop(
    PVOID dataObjectValue, PVOID dropSourceValue, DWORD allowedEffects,
    DWORD* effect)
{
    constexpr DWORD SupportedEffects = 0x00000001u | 0x00000002u |
        0x00000004u; // DROPEFFECT_COPY/MOVE/LINK
    if (!effect) return E_POINTER;
    *effect = 0;
    if (!dataObjectValue || !dropSourceValue) return E_INVALIDARG;
    allowedEffects &= SupportedEffects;
    if (!allowedEffects) return DRAGDROP_S_CANCEL;

    auto* dataObject = static_cast<IDataObject*>(dataObjectValue);
    auto* source = static_cast<GuestIDropSource*>(dropSourceValue);
    GuestIDropTarget* currentTarget = nullptr;
    HWND currentWindow = nullptr;
    DWORD selectedEffect = 0;
    bool escapePressed = false;

    RuntimeDiagnostics::Record(L"OLE: entered the virtual DoDragDrop modal loop.");
    for (;;)
    {
        GuestAbi::Message message{};
        while (BridgePeekMessageW(&message, nullptr, 0, 0, 0x0001u)) // PM_REMOVE
        {
            if (message.message == 0x0012u) // WM_QUIT
            {
                BridgePostQuitMessage(static_cast<int>(message.wParam));
                escapePressed = true;
                break;
            }
            if (message.message == GuestAbi::WmKeyDown &&
                message.wParam == VK_ESCAPE)
                escapePressed = true;
            BridgeTranslateMessage(&message);
            BridgeDispatchMessageW(&message);
        }

        const DWORD keyState = DragKeyState();
        const HRESULT continuation = SafeQueryContinueDrag(source,
            escapePressed ? TRUE : FALSE, keyState);
        if (continuation == DRAGDROP_S_CANCEL || FAILED(continuation))
        {
            if (currentTarget) SafeDragLeave(currentTarget);
            GuestComRelease(currentTarget);
            RuntimeDiagnostics::Record(L"OLE: virtual drag operation cancelled.");
            return continuation == DRAGDROP_S_CANCEL
                ? DRAGDROP_S_CANCEL : continuation;
        }

        POINT point{};
        if (!BridgeGetCursorPos(&point))
        {
            if (currentTarget) SafeDragLeave(currentTarget);
            GuestComRelease(currentTarget);
            return E_UNEXPECTED;
        }
        POINTL pointLong{ point.x, point.y };
        HWND targetWindow = nullptr;
        GuestIDropTarget* target = RegisteredDropTargetAt(point, &targetWindow);
        if (targetWindow != currentWindow || target != currentTarget)
        {
            if (currentTarget) SafeDragLeave(currentTarget);
            GuestComRelease(currentTarget);
            currentTarget = target;
            currentWindow = targetWindow;
            selectedEffect = allowedEffects;
            if (currentTarget && FAILED(SafeDragEnter(currentTarget, dataObject,
                keyState, pointLong, &selectedEffect)))
            {
                GuestComRelease(currentTarget);
                currentTarget = nullptr;
                currentWindow = nullptr;
                selectedEffect = 0;
            }
        }
        else
        {
            GuestComRelease(target);
            if (currentTarget)
            {
                selectedEffect = allowedEffects;
                if (FAILED(SafeDragOver(currentTarget, keyState, pointLong,
                    &selectedEffect)))
                    selectedEffect = 0;
            }
        }
        selectedEffect &= allowedEffects;
        const HRESULT feedback = SafeGiveFeedback(source, selectedEffect);
        if (FAILED(feedback))
        {
            if (currentTarget) SafeDragLeave(currentTarget);
            GuestComRelease(currentTarget);
            return feedback;
        }

        if (continuation == DRAGDROP_S_DROP)
        {
            HRESULT result = DRAGDROP_S_CANCEL;
            if (currentTarget && selectedEffect)
            {
                DWORD droppedEffect = selectedEffect;
                const HRESULT dropped = SafeDrop(currentTarget, dataObject,
                    keyState, pointLong, &droppedEffect);
                if (SUCCEEDED(dropped))
                {
                    *effect = droppedEffect & allowedEffects;
                    result = DRAGDROP_S_DROP;
                }
                else result = dropped;
            }
            else if (currentTarget) SafeDragLeave(currentTarget);
            GuestComRelease(currentTarget);
            RuntimeDiagnostics::Record(L"OLE: virtual drag operation left its modal loop.");
            return result;
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
}
BSTR WINAPI Win32Bridge::Bridge::BridgeSysAllocString(LPCOLESTR value) { return ::SysAllocString(value); }
BSTR WINAPI Win32Bridge::Bridge::BridgeSysAllocStringLen(const OLECHAR* value, UINT length) { return ::SysAllocStringLen(value, length); }
BSTR WINAPI Win32Bridge::Bridge::BridgeSysAllocStringByteLen(LPCSTR value, UINT length) { return ::SysAllocStringByteLen(value, length); }
void WINAPI Win32Bridge::Bridge::BridgeSysFreeString(BSTR value) { ::SysFreeString(value); }
UINT WINAPI Win32Bridge::Bridge::BridgeSysStringLen(BSTR value) { return ::SysStringLen(value); }
UINT WINAPI Win32Bridge::Bridge::BridgeSysStringByteLen(BSTR value) { return ::SysStringByteLen(value); }
HRESULT WINAPI Win32Bridge::Bridge::BridgeVariantClear(VARIANTARG* value) { return value ? ::VariantClear(value) : E_INVALIDARG; }
HRESULT WINAPI Win32Bridge::Bridge::BridgeVariantCopy(VARIANTARG* destination, const VARIANTARG* source) { return destination && source ? ::VariantCopy(destination, source) : E_INVALIDARG; }
HRESULT WINAPI BridgeCLSIDFromProgID(LPCOLESTR progId, LPCLSID clsid)
{
    if (!clsid) return E_POINTER;
    *clsid = CLSID_NULL;
    if (!progId || !*progId) return CO_E_CLASSSTRING;
    // COM registration belongs to the guest registry.  Until that registry is
    // connected to the class factory, fail explicitly instead of consulting or
    // leaking the host machine's COM catalogue.
    RuntimeDiagnostics::Record(L"COM: CLSIDFromProgID has no guest registration for " + std::wstring(progId));
    return REGDB_E_CLASSNOTREG;
}

ImportResolution Win32Bridge::Bridge::ResolveOleImport(const ImportedSymbol& symbol)
{
    auto result = CompatibilityCatalog::Resolve(symbol);
    if (Name(symbol.library, L"ole32.dll"))
    {
        if (Name(symbol.name, L"coinitialize")) result.targetAddress = reinterpret_cast<ULONGLONG>(&BridgeCoInitialize);
        else if (Name(symbol.name, L"coinitializeex")) result.targetAddress = reinterpret_cast<ULONGLONG>(&BridgeCoInitializeEx);
        else if (Name(symbol.name, L"oleinitialize")) result.targetAddress = reinterpret_cast<ULONGLONG>(&BridgeOleInitialize);
        else if (Name(symbol.name, L"couninitialize")) result.targetAddress = reinterpret_cast<ULONGLONG>(&BridgeCoUninitialize);
        else if (Name(symbol.name, L"oleuninitialize")) result.targetAddress = reinterpret_cast<ULONGLONG>(&BridgeOleUninitialize);
        else if (Name(symbol.name, L"cotaskmemalloc")) result.targetAddress = reinterpret_cast<ULONGLONG>(&BridgeCoTaskMemAlloc);
        else if (Name(symbol.name, L"cotaskmemfree")) result.targetAddress = reinterpret_cast<ULONGLONG>(&BridgeCoTaskMemFree);
        else if (Name(symbol.name, L"cocreateinstance")) result.targetAddress = reinterpret_cast<ULONGLONG>(&BridgeCoCreateInstance);
        else if (Name(symbol.name, L"releasestgmedium")) result.targetAddress = reinterpret_cast<ULONGLONG>(&BridgeReleaseStgMedium);
        else if (Name(symbol.name, L"registerdragdrop")) result.targetAddress = reinterpret_cast<ULONGLONG>(&BridgeRegisterDragDrop);
        else if (Name(symbol.name, L"revokedragdrop")) result.targetAddress = reinterpret_cast<ULONGLONG>(&BridgeRevokeDragDrop);
        else if (Name(symbol.name, L"dodragdrop")) result.targetAddress = reinterpret_cast<ULONGLONG>(&BridgeDoDragDrop);
        else if (Name(symbol.name, L"clsidfromprogid")) result.targetAddress = reinterpret_cast<ULONGLONG>(&BridgeCLSIDFromProgID);
    }
    else if (Name(symbol.library, L"oleaut32.dll") && symbol.importedByOrdinal)
    {
        switch (symbol.ordinal)
        {
        case 2: result.targetAddress = reinterpret_cast<ULONGLONG>(&BridgeSysAllocString); break;
        case 4: result.targetAddress = reinterpret_cast<ULONGLONG>(&BridgeSysAllocStringLen); break;
        case 6: result.targetAddress = reinterpret_cast<ULONGLONG>(&BridgeSysFreeString); break;
        case 7: result.targetAddress = reinterpret_cast<ULONGLONG>(&BridgeSysStringLen); break;
        case 9: result.targetAddress = reinterpret_cast<ULONGLONG>(&BridgeVariantClear); break;
        case 10: result.targetAddress = reinterpret_cast<ULONGLONG>(&BridgeVariantCopy); break;
        case 149: result.targetAddress = reinterpret_cast<ULONGLONG>(&BridgeSysStringByteLen); break;
        case 150: result.targetAddress = reinterpret_cast<ULONGLONG>(&BridgeSysAllocStringByteLen); break;
        }
    }
    if (result.targetAddress)
    {
        result.disposition = ImportDisposition::NeedsBridge;
        result.note = Name(symbol.library, L"oleaut32.dll")
            ? L"OLE Automation adapter backed by the bridge/runtime automation allocator."
            : L"Isolated guest COM/OLE adapter; host COM registration is not exposed.";
    }
    return result;
}
