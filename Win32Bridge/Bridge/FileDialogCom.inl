// Desktop Shell interfaces are hidden by the UWP SDK partition. Keep the
// guest vtable layout local and use the same virtual picker as COMDLG32.
namespace
{
    using namespace Win32Bridge::Bridge;
    using Microsoft::WRL::ComPtr;

    constexpr GUID OpenDialogClass =
        { 0xdc1c5a9c, 0xe88a, 0x4dde, { 0xa5, 0xa1, 0x60, 0xf8, 0x2a, 0x20, 0xae, 0xf7 } };
    constexpr GUID SaveDialogClass =
        { 0xc0b4e2f3, 0xba21, 0x4773, { 0x8d, 0xba, 0x33, 0x5e, 0xc9, 0x46, 0xeb, 0x8b } };
    constexpr DWORD FileSystemName = 0x80058000u; // SIGDN_FILESYSPATH
    constexpr DWORD DesktopParsingName = 0x80028000u; // SIGDN_DESKTOPABSOLUTEPARSING
    constexpr DWORD ShellFileSystem = 0x40000000u;
    constexpr DWORD ShellFolder = 0x20000000u;
    constexpr DWORD ShellReadOnly = 0x00040000u;
    constexpr DWORD DialogPickFolders = 0x20u;
    constexpr DWORD DialogAllowMultiple = 0x200u;
    constexpr DWORD PickerOptionMask = 0x00009902u; // Shared OFN and FOS flags

    struct __declspec(uuid("43826d1e-e718-42ee-bc55-a1e261c37bfe")) FileShellItem : IUnknown
    {
        virtual HRESULT STDMETHODCALLTYPE BindToHandler(void*, REFGUID, REFIID, void**) = 0;
        virtual HRESULT STDMETHODCALLTYPE GetParent(FileShellItem**) = 0;
        virtual HRESULT STDMETHODCALLTYPE GetDisplayName(DWORD, LPWSTR*) = 0;
        virtual HRESULT STDMETHODCALLTYPE GetAttributes(DWORD, DWORD*) = 0;
        virtual HRESULT STDMETHODCALLTYPE Compare(FileShellItem*, DWORD, int*) = 0;
    };

    struct __declspec(uuid("b63ea76d-1f85-456f-a19c-48159efa858b")) FileShellItemArray : IUnknown
    {
        virtual HRESULT STDMETHODCALLTYPE BindToHandler(void*, REFGUID, REFIID, void**) = 0;
        virtual HRESULT STDMETHODCALLTYPE GetPropertyStore(DWORD, REFIID, void**) = 0;
        virtual HRESULT STDMETHODCALLTYPE GetPropertyDescriptionList(const void*, REFIID, void**) = 0;
        virtual HRESULT STDMETHODCALLTYPE GetAttributes(DWORD, DWORD, DWORD*) = 0;
        virtual HRESULT STDMETHODCALLTYPE GetCount(DWORD*) = 0;
        virtual HRESULT STDMETHODCALLTYPE GetItemAt(DWORD, FileShellItem**) = 0;
        virtual HRESULT STDMETHODCALLTYPE EnumItems(void**) = 0;
    };

    struct GuestFileType
    {
        LPCWSTR name;
        LPCWSTR pattern;
    };
    static_assert(sizeof(GuestFileType) == 16, "Guest COMDLG_FILTERSPEC ABI is x64.");
    struct FileDialogEvents;

    struct __declspec(uuid("b4db1657-70d7-485e-8e3e-6fcb5a5c1802")) ModalFileWindow : IUnknown
    {
        virtual HRESULT STDMETHODCALLTYPE Show(HWND) = 0;
    };

    struct __declspec(uuid("00000114-0000-0000-c000-000000000046")) FileDialogWindow : IUnknown
    {
        virtual HRESULT STDMETHODCALLTYPE GetWindow(HWND*) = 0;
        virtual HRESULT STDMETHODCALLTYPE ContextSensitiveHelp(BOOL) = 0;
    };

    struct __declspec(uuid("42f85136-db7e-439c-85f1-e4075d135fc8")) FileDialog : ModalFileWindow
    {
        virtual HRESULT STDMETHODCALLTYPE SetFileTypes(UINT, const GuestFileType*) = 0;
        virtual HRESULT STDMETHODCALLTYPE SetFileTypeIndex(UINT) = 0;
        virtual HRESULT STDMETHODCALLTYPE GetFileTypeIndex(UINT*) = 0;
        virtual HRESULT STDMETHODCALLTYPE Advise(FileDialogEvents*, DWORD*) = 0;
        virtual HRESULT STDMETHODCALLTYPE Unadvise(DWORD) = 0;
        virtual HRESULT STDMETHODCALLTYPE SetOptions(DWORD) = 0;
        virtual HRESULT STDMETHODCALLTYPE GetOptions(DWORD*) = 0;
        virtual HRESULT STDMETHODCALLTYPE SetDefaultFolder(FileShellItem*) = 0;
        virtual HRESULT STDMETHODCALLTYPE SetFolder(FileShellItem*) = 0;
        virtual HRESULT STDMETHODCALLTYPE GetFolder(FileShellItem**) = 0;
        virtual HRESULT STDMETHODCALLTYPE GetCurrentSelection(FileShellItem**) = 0;
        virtual HRESULT STDMETHODCALLTYPE SetFileName(LPCWSTR) = 0;
        virtual HRESULT STDMETHODCALLTYPE GetFileName(LPWSTR*) = 0;
        virtual HRESULT STDMETHODCALLTYPE SetTitle(LPCWSTR) = 0;
        virtual HRESULT STDMETHODCALLTYPE SetOkButtonLabel(LPCWSTR) = 0;
        virtual HRESULT STDMETHODCALLTYPE SetFileNameLabel(LPCWSTR) = 0;
        virtual HRESULT STDMETHODCALLTYPE GetResult(FileShellItem**) = 0;
        virtual HRESULT STDMETHODCALLTYPE AddPlace(FileShellItem*, DWORD) = 0;
        virtual HRESULT STDMETHODCALLTYPE SetDefaultExtension(LPCWSTR) = 0;
        virtual HRESULT STDMETHODCALLTYPE Close(HRESULT) = 0;
        virtual HRESULT STDMETHODCALLTYPE SetClientGuid(REFGUID) = 0;
        virtual HRESULT STDMETHODCALLTYPE ClearClientData() = 0;
        virtual HRESULT STDMETHODCALLTYPE SetFilter(IUnknown*) = 0;
    };

    struct __declspec(uuid("d57c7288-d4ad-4768-be02-9d969532d960")) OpenFileDialog : FileDialog
    {
        virtual HRESULT STDMETHODCALLTYPE GetResults(FileShellItemArray**) = 0;
        virtual HRESULT STDMETHODCALLTYPE GetSelectedItems(FileShellItemArray**) = 0;
    };

    struct __declspec(uuid("84bccd23-5fde-4cdb-aea4-af64b83d78ab")) SaveFileDialog : FileDialog
    {
        virtual HRESULT STDMETHODCALLTYPE SetSaveAsItem(FileShellItem*) = 0;
        virtual HRESULT STDMETHODCALLTYPE SetProperties(IUnknown*) = 0;
        virtual HRESULT STDMETHODCALLTYPE SetCollectedProperties(IUnknown*, BOOL) = 0;
        virtual HRESULT STDMETHODCALLTYPE GetProperties(IUnknown**) = 0;
        virtual HRESULT STDMETHODCALLTYPE ApplyProperties(FileShellItem*, IUnknown*, HWND, IUnknown*) = 0;
    };

    struct FileDialogEvents : IUnknown
    {
        virtual HRESULT STDMETHODCALLTYPE OnFileOk(FileDialog*) = 0;
        virtual HRESULT STDMETHODCALLTYPE OnFolderChanging(FileDialog*, FileShellItem*) = 0;
        virtual HRESULT STDMETHODCALLTYPE OnFolderChange(FileDialog*) = 0;
        virtual HRESULT STDMETHODCALLTYPE OnSelectionChange(FileDialog*) = 0;
        virtual HRESULT STDMETHODCALLTYPE OnShareViolation(FileDialog*, FileShellItem*, DWORD*) = 0;
        virtual HRESULT STDMETHODCALLTYPE OnTypeChange(FileDialog*) = 0;
        virtual HRESULT STDMETHODCALLTYPE OnOverwrite(FileDialog*, FileShellItem*, DWORD*) = 0;
    };

    struct __declspec(uuid("e6fdd21a-163f-4975-9c8c-a69f1ba37034")) FileDialogCustomize : IUnknown
    {
        virtual HRESULT STDMETHODCALLTYPE EnableOpenDropDown(DWORD) { return E_NOTIMPL; }
        virtual HRESULT STDMETHODCALLTYPE AddMenu(DWORD, LPCWSTR) { return E_NOTIMPL; }
        virtual HRESULT STDMETHODCALLTYPE AddPushButton(DWORD, LPCWSTR) { return E_NOTIMPL; }
        virtual HRESULT STDMETHODCALLTYPE AddComboBox(DWORD) { return E_NOTIMPL; }
        virtual HRESULT STDMETHODCALLTYPE AddRadioButtonList(DWORD) { return E_NOTIMPL; }
        virtual HRESULT STDMETHODCALLTYPE AddCheckButton(DWORD, LPCWSTR, BOOL) = 0;
        virtual HRESULT STDMETHODCALLTYPE AddEditBox(DWORD, LPCWSTR) { return E_NOTIMPL; }
        virtual HRESULT STDMETHODCALLTYPE AddSeparator(DWORD) { return E_NOTIMPL; }
        virtual HRESULT STDMETHODCALLTYPE AddText(DWORD, LPCWSTR) { return E_NOTIMPL; }
        virtual HRESULT STDMETHODCALLTYPE SetControlLabel(DWORD, LPCWSTR) = 0;
        virtual HRESULT STDMETHODCALLTYPE GetControlState(DWORD, DWORD*) = 0;
        virtual HRESULT STDMETHODCALLTYPE SetControlState(DWORD, DWORD) = 0;
        virtual HRESULT STDMETHODCALLTYPE GetEditBoxText(DWORD, LPWSTR*) { return E_NOTIMPL; }
        virtual HRESULT STDMETHODCALLTYPE SetEditBoxText(DWORD, LPCWSTR) { return E_NOTIMPL; }
        virtual HRESULT STDMETHODCALLTYPE GetCheckButtonState(DWORD, BOOL*) = 0;
        virtual HRESULT STDMETHODCALLTYPE SetCheckButtonState(DWORD, BOOL) = 0;
        virtual HRESULT STDMETHODCALLTYPE AddControlItem(DWORD, DWORD, LPCWSTR) { return E_NOTIMPL; }
        virtual HRESULT STDMETHODCALLTYPE RemoveControlItem(DWORD, DWORD) { return E_NOTIMPL; }
        virtual HRESULT STDMETHODCALLTYPE RemoveAllControlItems(DWORD) { return E_NOTIMPL; }
        virtual HRESULT STDMETHODCALLTYPE GetControlItemState(DWORD, DWORD, DWORD*) { return E_NOTIMPL; }
        virtual HRESULT STDMETHODCALLTYPE SetControlItemState(DWORD, DWORD, DWORD) { return E_NOTIMPL; }
        virtual HRESULT STDMETHODCALLTYPE GetSelectedControlItem(DWORD, DWORD*) { return E_NOTIMPL; }
        virtual HRESULT STDMETHODCALLTYPE SetSelectedControlItem(DWORD, DWORD) { return E_NOTIMPL; }
        virtual HRESULT STDMETHODCALLTYPE StartVisualGroup(DWORD, LPCWSTR) { return E_NOTIMPL; }
        virtual HRESULT STDMETHODCALLTYPE EndVisualGroup() { return E_NOTIMPL; }
        virtual HRESULT STDMETHODCALLTYPE MakeProminent(DWORD) { return E_NOTIMPL; }
        virtual HRESULT STDMETHODCALLTYPE SetControlItemText(DWORD, DWORD, LPCWSTR) { return E_NOTIMPL; }
    };

    struct __declspec(uuid("36116642-d713-4b97-9b83-7484a9d00433")) FileDialogControlEvents : IUnknown
    {
        virtual HRESULT STDMETHODCALLTYPE OnItemSelected(FileDialogCustomize*, DWORD, DWORD) = 0;
        virtual HRESULT STDMETHODCALLTYPE OnButtonClicked(FileDialogCustomize*, DWORD) = 0;
        virtual HRESULT STDMETHODCALLTYPE OnCheckButtonToggled(FileDialogCustomize*, DWORD, BOOL) = 0;
        virtual HRESULT STDMETHODCALLTYPE OnControlActivating(FileDialogCustomize*, DWORD) = 0;
    };

    HRESULT CopyFileDialogString(const std::wstring& value, LPWSTR* result)
    {
        if (!result) return E_POINTER;
        const size_t bytes = (value.size() + 1) * sizeof(wchar_t);
        *result = static_cast<LPWSTR>(::CoTaskMemAlloc(bytes));
        if (!*result) return E_OUTOFMEMORY;
        std::memcpy(*result, value.c_str(), bytes);
        return S_OK;
    }

    std::wstring FileDialogParent(const std::wstring& path)
    {
        const auto separator = path.find_last_of(L"\\/");
        if (separator == std::wstring::npos || path.size() <= 3) return {};
        return path.substr(0, separator == 2 ? 3 : separator);
    }

    HRESULT ReadFileShellItem(FileShellItem* item, std::wstring& path)
    {
        if (!item) return E_INVALIDARG;
        LPWSTR value = nullptr;
        const HRESULT status = item->GetDisplayName(FileSystemName, &value);
        if (SUCCEEDED(status) && value) path = value;
        ::CoTaskMemFree(value);
        return SUCCEEDED(status) && path.empty() ? E_INVALIDARG : status;
    }

    template<class Interface, class Derived>
    class FileDialogComObject : public Interface
    {
    public:
        HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void** result) override
        {
            if (!result) return E_POINTER;
            *result = nullptr;
            if (!IsEqualGUID(iid, __uuidof(IUnknown)) && !IsEqualGUID(iid, __uuidof(Interface)))
                return E_NOINTERFACE;
            *result = static_cast<Interface*>(this);
            AddRef();
            return S_OK;
        }
        ULONG STDMETHODCALLTYPE AddRef() override { return ++m_references; }
        ULONG STDMETHODCALLTYPE Release() override
        {
            const ULONG remaining = --m_references;
            if (!remaining) delete static_cast<Derived*>(this);
            return remaining;
        }
    private:
        std::atomic<ULONG> m_references{ 1 };
    };

    HRESULT MakeFileShellItem(const std::wstring& path, FileShellItem** result);

    class VirtualFileShellItem final : public FileDialogComObject<FileShellItem, VirtualFileShellItem>
    {
    public:
        explicit VirtualFileShellItem(std::wstring path) : m_path(std::move(path)) {}
        HRESULT STDMETHODCALLTYPE BindToHandler(void*, REFGUID, REFIID, void** result) override
        {
            if (result) *result = nullptr;
            return E_NOINTERFACE;
        }
        HRESULT STDMETHODCALLTYPE GetParent(FileShellItem** result) override
        {
            return MakeFileShellItem(FileDialogParent(m_path), result);
        }
        HRESULT STDMETHODCALLTYPE GetDisplayName(DWORD form, LPWSTR* result) override
        {
            if (!result) return E_POINTER;
            *result = nullptr;
            if (form == FileSystemName || form == DesktopParsingName || m_path.size() <= 3)
                return CopyFileDialogString(m_path, result);
            if (form == 0 || form == 0x80018001u || form == 0x80031001u)
                return CopyFileDialogString(m_path.substr(m_path.find_last_of(L"\\/") + 1), result);
            return E_NOTIMPL;
        }
        HRESULT STDMETHODCALLTYPE GetAttributes(DWORD mask, DWORD* result) override
        {
            if (!result) return E_POINTER;
            DWORD attributes = ShellFileSystem;
            if (auto* storage = CurrentGuestStorageContext())
            {
                DWORD error = ERROR_SUCCESS;
                const DWORD file = storage->GetGuestFileAttributes(m_path.c_str(), &error);
                if (file != INVALID_FILE_ATTRIBUTES)
                {
                    if (file & FILE_ATTRIBUTE_DIRECTORY) attributes |= ShellFolder;
                    if (file & FILE_ATTRIBUTE_READONLY) attributes |= ShellReadOnly;
                }
            }
            *result = mask & attributes;
            return *result == mask ? S_OK : S_FALSE;
        }
        HRESULT STDMETHODCALLTYPE Compare(FileShellItem* other, DWORD, int* order) override
        {
            if (!order) return E_POINTER;
            std::wstring path;
            const HRESULT status = ReadFileShellItem(other, path);
            if (FAILED(status)) return status;
            *order = _wcsicmp(m_path.c_str(), path.c_str());
            return S_OK;
        }
    private:
        std::wstring m_path;
    };

    HRESULT MakeFileShellItem(const std::wstring& path, FileShellItem** result)
    {
        if (!result) return E_POINTER;
        *result = nullptr;
        if (path.empty()) return E_FAIL;
        *result = new (std::nothrow) VirtualFileShellItem(path);
        return *result ? S_OK : E_OUTOFMEMORY;
    }

    class VirtualFileShellItemArray final :
        public FileDialogComObject<FileShellItemArray, VirtualFileShellItemArray>
    {
    public:
        explicit VirtualFileShellItemArray(std::vector<std::wstring> paths)
            : m_paths(std::move(paths)) {}
        HRESULT STDMETHODCALLTYPE BindToHandler(void*, REFGUID, REFIID, void**) override { return E_NOTIMPL; }
        HRESULT STDMETHODCALLTYPE GetPropertyStore(DWORD, REFIID, void**) override { return E_NOTIMPL; }
        HRESULT STDMETHODCALLTYPE GetPropertyDescriptionList(const void*, REFIID, void**) override { return E_NOTIMPL; }
        HRESULT STDMETHODCALLTYPE GetAttributes(DWORD flags, DWORD mask, DWORD* result) override
        {
            if (!result) return E_POINTER;
            const bool combine = (flags & 3u) == 2u; // SIATTRIBFLAGS_OR
            *result = combine ? 0 : mask;
            for (DWORD index = 0; index < m_paths.size(); ++index)
            {
                ComPtr<FileShellItem> item;
                const HRESULT status = GetItemAt(index, item.GetAddressOf());
                if (FAILED(status)) return status;
                DWORD attributes = 0;
                item->GetAttributes(mask, &attributes);
                if (combine) *result |= attributes;
                else *result &= attributes;
            }
            return *result == mask ? S_OK : S_FALSE;
        }
        HRESULT STDMETHODCALLTYPE GetCount(DWORD* result) override
        {
            if (!result) return E_POINTER;
            *result = static_cast<DWORD>(m_paths.size());
            return S_OK;
        }
        HRESULT STDMETHODCALLTYPE GetItemAt(DWORD index, FileShellItem** result) override
        {
            if (!result) return E_POINTER;
            *result = nullptr;
            return index < m_paths.size() ? MakeFileShellItem(m_paths[index], result) : E_INVALIDARG;
        }
        HRESULT STDMETHODCALLTYPE EnumItems(void**) override { return E_NOTIMPL; }
    private:
        std::vector<std::wstring> m_paths;
    };

    template<class Interface>
    class VirtualFileDialog final : public FileDialogComObject<Interface, VirtualFileDialog<Interface>>,
        public FileDialogCustomize, public FileDialogWindow
    {
        static constexpr bool IsSave = std::is_same<Interface, SaveFileDialog>::value;
        using Base = FileDialogComObject<Interface, VirtualFileDialog<Interface>>;
    public:
        HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void** result) override
        {
            if (!result) return E_POINTER;
            if (IsEqualGUID(iid, __uuidof(FileDialogCustomize)))
                *result = static_cast<FileDialogCustomize*>(this);
            else if (IsEqualGUID(iid, __uuidof(FileDialogWindow)))
                *result = static_cast<FileDialogWindow*>(this);
            else if (IsEqualGUID(iid, __uuidof(FileDialog)))
                *result = static_cast<FileDialog*>(this);
            else if (IsEqualGUID(iid, __uuidof(ModalFileWindow)))
                *result = static_cast<ModalFileWindow*>(this);
            else
                return Base::QueryInterface(iid, result);
            AddRef();
            return S_OK;
        }
        ULONG STDMETHODCALLTYPE AddRef() override { return Base::AddRef(); }
        ULONG STDMETHODCALLTYPE Release() override { return Base::Release(); }

        HRESULT STDMETHODCALLTYPE Show(HWND owner) override
        {
            if (m_showing) return HRESULT_FROM_WIN32(ERROR_BUSY);
            ComPtr<FileDialog> keepAlive(static_cast<FileDialog*>(this));
            m_showing = true;
            m_results.clear();
            m_selection.clear();
            m_currentFolder.clear();
            m_closeStatus = HRESULT_FROM_WIN32(ERROR_CANCELLED);
            HRESULT status = m_closeStatus;
            try
            {
                const auto folder = m_folder.empty() ? m_defaultFolder : m_folder;
                const auto title = m_title;
                const auto name = m_name;
                const auto extension = m_extension;
                const auto okLabel = m_okLabel;
                const auto nameLabel = m_nameLabel;
                GuestFileDialogDescriptor descriptor;
                descriptor.owner = owner;
                descriptor.title = title.empty() ? nullptr : title.c_str();
                descriptor.initialDirectory = folder.empty() ? nullptr : folder.c_str();
                descriptor.initialFileName = name.c_str();
                descriptor.filter = m_filters.empty() ? nullptr : m_filters.c_str();
                descriptor.filterIndex = m_typeIndex;
                descriptor.defaultExtension = extension.c_str();
                descriptor.flags = m_options & PickerOptionMask;
                descriptor.saveDialog = IsSave;
                descriptor.folderDialog = (m_options & DialogPickFolders) != 0;
                descriptor.allowMultiple = !IsSave && !descriptor.folderDialog &&
                    (m_options & DialogAllowMultiple) != 0;
                descriptor.okButtonLabel = okLabel.empty() ? nullptr : okLabel.c_str();
                descriptor.fileNameLabel = nameLabel.empty() ? nullptr : nameLabel.c_str();
                descriptor.checkBoxes = m_checkBoxes.empty() ? nullptr : &m_checkBoxes;
                descriptor.onEvent = &ForwardEvent;
                descriptor.eventContext = this;
                std::wstring selected;
                const bool accepted = ShowGuestFileDialog(descriptor, &selected, &m_typeIndex, &m_results);
                const DWORD error = accepted ? ERROR_SUCCESS : BridgeGetLastError();
                status = accepted ? S_OK : (error && error != ERROR_CANCELLED
                    ? HRESULT_FROM_WIN32(error) : m_closeStatus);
            }
            catch (const std::bad_alloc&)
            {
                status = E_OUTOFMEMORY;
            }
            m_window = nullptr;
            m_showing = false;
            m_selection.clear();
            if (FAILED(status)) m_results.clear();
            return status;
        }
        HRESULT STDMETHODCALLTYPE SetFileTypes(UINT count, const GuestFileType* types) override
        {
            if (!count || !types || count > MaximumGuestFileDialogFilters) return E_INVALIDARG;
            if (m_filterCount || m_showing) return E_UNEXPECTED;
            std::wstring filters;
            for (UINT index = 0; index < count; ++index)
            {
                if (!types[index].name || !types[index].pattern ||
                    !*types[index].name || !*types[index].pattern) return E_INVALIDARG;
                filters.append(types[index].name);
                filters += L'\0';
                filters.append(types[index].pattern);
                filters += L'\0';
            }
            m_filters = std::move(filters);
            m_filterCount = count;
            return S_OK;
        }
        HRESULT STDMETHODCALLTYPE SetFileTypeIndex(UINT index) override
        {
            if (!index || index > (m_filterCount ? m_filterCount : 1)) return E_INVALIDARG;
            m_typeIndex = index;
            if (m_window) BridgeSendMessageW(m_window, GuestFileDialogSetFilterMessage, index, 0);
            return S_OK;
        }
        HRESULT STDMETHODCALLTYPE GetFileTypeIndex(UINT* value) override
        {
            if (!value) return E_POINTER;
            *value = m_typeIndex;
            return S_OK;
        }
        HRESULT STDMETHODCALLTYPE Advise(FileDialogEvents* events, DWORD* cookie) override
        {
            if (!events || !cookie) return E_POINTER;
            *cookie = ++m_nextCookie;
            m_events.push_back({ *cookie, ComPtr<FileDialogEvents>(events) });
            return S_OK;
        }
        HRESULT STDMETHODCALLTYPE Unadvise(DWORD cookie) override
        {
            const auto item = std::find_if(m_events.begin(), m_events.end(),
                [cookie](const auto& entry) { return entry.first == cookie; });
            if (item == m_events.end()) return E_INVALIDARG;
            m_events.erase(item);
            return S_OK;
        }
        HRESULT STDMETHODCALLTYPE SetOptions(DWORD value) override { m_options = value; return S_OK; }
        HRESULT STDMETHODCALLTYPE GetOptions(DWORD* value) override
        {
            if (!value) return E_POINTER;
            *value = m_options;
            return S_OK;
        }
        HRESULT STDMETHODCALLTYPE SetDefaultFolder(FileShellItem* item) override
        {
            return SetFolderPath(item, m_defaultFolder);
        }
        HRESULT STDMETHODCALLTYPE SetFolder(FileShellItem* item) override
        {
            std::wstring folder;
            const HRESULT status = SetFolderPath(item, folder);
            if (FAILED(status)) return status;
            if (m_window && item && !BridgeSendMessageW(m_window,
                GuestFileDialogSetFolderMessage, 0, reinterpret_cast<LPARAM>(folder.c_str())))
                return E_FAIL;
            m_folder = std::move(folder);
            return S_OK;
        }
        HRESULT STDMETHODCALLTYPE GetFolder(FileShellItem** result) override
        {
            const auto& folder = m_showing && !m_currentFolder.empty() ? m_currentFolder :
                (m_folder.empty() ? m_defaultFolder : m_folder);
            return MakeFileShellItem(folder, result);
        }
        HRESULT STDMETHODCALLTYPE GetCurrentSelection(FileShellItem** result) override
        {
            return MakeFileShellItem(m_selection.empty() ? std::wstring{} : m_selection.front(), result);
        }
        HRESULT STDMETHODCALLTYPE SetFileName(LPCWSTR value) override
        {
            m_name = value ? value : L"";
            if (m_window) BridgeSetDlgItemTextW(m_window, GuestFileDialogNameId, m_name.c_str());
            return S_OK;
        }
        HRESULT STDMETHODCALLTYPE GetFileName(LPWSTR* value) override
        {
            if (!m_window) return CopyFileDialogString(m_name, value);
            wchar_t name[32768]{};
            BridgeGetDlgItemTextW(m_window, GuestFileDialogNameId, name, ARRAYSIZE(name));
            return CopyFileDialogString(name, value);
        }
        HRESULT STDMETHODCALLTYPE SetTitle(LPCWSTR value) override
        {
            m_title = value ? value : L"";
            if (m_window) BridgeSetWindowTextW(m_window, m_title.c_str());
            return S_OK;
        }
        HRESULT STDMETHODCALLTYPE SetOkButtonLabel(LPCWSTR value) override
        {
            m_okLabel = value ? value : L"";
            if (m_window) BridgeSetDlgItemTextW(m_window, IDOK, m_okLabel.c_str());
            return S_OK;
        }
        HRESULT STDMETHODCALLTYPE SetFileNameLabel(LPCWSTR value) override
        {
            m_nameLabel = value ? value : L"";
            if (m_window) BridgeSetDlgItemTextW(m_window, GuestFileDialogNameLabelId, m_nameLabel.c_str());
            return S_OK;
        }
        HRESULT STDMETHODCALLTYPE GetResult(FileShellItem** result) override
        {
            return MakeFileShellItem(m_results.empty() ? std::wstring{} : m_results.front(), result);
        }
        HRESULT STDMETHODCALLTYPE AddPlace(FileShellItem*, DWORD) override { return E_NOTIMPL; }
        HRESULT STDMETHODCALLTYPE SetDefaultExtension(LPCWSTR value) override
        {
            m_extension = value ? value : L"";
            return S_OK;
        }
        HRESULT STDMETHODCALLTYPE Close(HRESULT status) override
        {
            if (!m_window) return E_UNEXPECTED;
            m_closeStatus = status;
            return EndGuestResourceDialog(m_window, IDCANCEL) ? S_OK : E_FAIL;
        }
        HRESULT STDMETHODCALLTYPE SetClientGuid(REFGUID) override { return E_NOTIMPL; }
        HRESULT STDMETHODCALLTYPE ClearClientData() override { return S_OK; }
        HRESULT STDMETHODCALLTYPE SetFilter(IUnknown*) override { return E_NOTIMPL; }

        HRESULT STDMETHODCALLTYPE GetResults(FileShellItemArray** result)
        {
            return MakeArray(m_results, result);
        }
        HRESULT STDMETHODCALLTYPE GetSelectedItems(FileShellItemArray** result)
        {
            return MakeArray(m_selection, result);
        }
        HRESULT STDMETHODCALLTYPE SetSaveAsItem(FileShellItem* item)
        {
            std::wstring path;
            const HRESULT status = ReadFileShellItem(item, path);
            if (FAILED(status)) return status;
            m_folder = FileDialogParent(path);
            return SetFileName(path.c_str() + (path.find_last_of(L"\\/") + 1));
        }
        HRESULT STDMETHODCALLTYPE SetProperties(IUnknown*) { return E_NOTIMPL; }
        HRESULT STDMETHODCALLTYPE SetCollectedProperties(IUnknown*, BOOL) { return E_NOTIMPL; }
        HRESULT STDMETHODCALLTYPE GetProperties(IUnknown**) { return E_NOTIMPL; }
        HRESULT STDMETHODCALLTYPE ApplyProperties(FileShellItem*, IUnknown*, HWND, IUnknown*) { return E_NOTIMPL; }

        HRESULT STDMETHODCALLTYPE GetWindow(HWND* window) override
        {
            if (!window) return E_POINTER;
            *window = m_window;
            return m_window ? S_OK : E_FAIL;
        }
        HRESULT STDMETHODCALLTYPE ContextSensitiveHelp(BOOL) override { return E_NOTIMPL; }

        HRESULT STDMETHODCALLTYPE AddCheckButton(DWORD id, LPCWSTR label, BOOL checked) override
        {
            if (m_showing || FindCheckBox(id) || m_checkBoxes.size() >= 16) return E_INVALIDARG;
            m_checkBoxes.push_back({ id, label ? label : L"", checked });
            return S_OK;
        }
        HRESULT STDMETHODCALLTYPE SetControlLabel(DWORD id, LPCWSTR value) override
        {
            auto* box = FindCheckBox(id);
            if (!box) return E_INVALIDARG;
            box->label = value ? value : L"";
            if (box->window) BridgeSetWindowTextW(box->window, box->label.c_str());
            return S_OK;
        }
        HRESULT STDMETHODCALLTYPE GetControlState(DWORD id, DWORD* value) override
        {
            if (!value) return E_POINTER;
            const auto* box = FindCheckBox(id);
            if (!box) return E_INVALIDARG;
            *value = box->state;
            return S_OK;
        }
        HRESULT STDMETHODCALLTYPE SetControlState(DWORD id, DWORD value) override
        {
            auto* box = FindCheckBox(id);
            if (!box || (value & ~7u)) return E_INVALIDARG;
            box->state = value;
            if (box->window)
            {
                BridgeEnableWindow(box->window, (value & 1) != 0);
                BridgeShowWindow(box->window, (value & 2) ? SW_SHOW : SW_HIDE);
            }
            return S_OK;
        }
        HRESULT STDMETHODCALLTYPE GetCheckButtonState(DWORD id, BOOL* value) override
        {
            if (!value) return E_POINTER;
            const auto* box = FindCheckBox(id);
            if (!box) return E_INVALIDARG;
            *value = box->checked;
            return S_OK;
        }
        HRESULT STDMETHODCALLTYPE SetCheckButtonState(DWORD id, BOOL value) override
        {
            auto* box = FindCheckBox(id);
            if (!box) return E_INVALIDARG;
            box->checked = value != FALSE;
            if (box->window) BridgeSendMessageW(box->window, 0x00f1, box->checked, 0); // BM_SETCHECK
            return S_OK;
        }
    private:
        GuestFileDialogCheckBox* FindCheckBox(DWORD id)
        {
            const auto box = std::find_if(m_checkBoxes.begin(), m_checkBoxes.end(),
                [id](const auto& entry) { return entry.id == id; });
            return box == m_checkBoxes.end() ? nullptr : &*box;
        }
        static HRESULT MakeArray(const std::vector<std::wstring>& paths, FileShellItemArray** result)
        {
            if (!result) return E_POINTER;
            *result = nullptr;
            if (paths.empty()) return E_FAIL;
            *result = new (std::nothrow) VirtualFileShellItemArray(paths);
            return *result ? S_OK : E_OUTOFMEMORY;
        }
        static HRESULT SetFolderPath(FileShellItem* item, std::wstring& folder)
        {
            if (!item) { folder.clear(); return S_OK; }
            DWORD attributes = 0;
            const HRESULT status = item->GetAttributes(ShellFolder, &attributes);
            if (FAILED(status)) return status;
            return (attributes & ShellFolder) ? ReadFileShellItem(item, folder) : E_INVALIDARG;
        }
        static HRESULT ForwardEvent(void* context, GuestFileDialogEvent event, HWND window,
            const std::wstring& folder, const std::vector<std::wstring>& selection, DWORD id)
        {
            auto& self = *static_cast<VirtualFileDialog*>(context);
            self.m_window = event == GuestFileDialogEvent::Destroyed ? nullptr : window;
            if (event == GuestFileDialogEvent::Destroyed) return S_OK;
            if (event != GuestFileDialogEvent::FolderChanging) self.m_currentFolder = folder;
            self.m_selection = selection;
            if (event == GuestFileDialogEvent::TypeChanged) self.m_typeIndex = id;
            if (event == GuestFileDialogEvent::FileOk) self.m_results = selection;

            ComPtr<FileShellItem> targetFolder;
            if (event == GuestFileDialogEvent::FolderChanging)
            {
                const HRESULT status = MakeFileShellItem(folder, targetFolder.GetAddressOf());
                if (FAILED(status)) return status;
            }
            const auto events = self.m_events; // A callback may unadvise itself.
            for (const auto& subscription : events)
            {
                auto* handler = subscription.second.Get();
                HRESULT response = S_OK;
                switch (event)
                {
                case GuestFileDialogEvent::FolderChanging:
                    response = handler->OnFolderChanging(&self, targetFolder.Get()); break;
                case GuestFileDialogEvent::FolderChanged:
                    handler->OnFolderChange(&self); break;
                case GuestFileDialogEvent::SelectionChanged:
                    handler->OnSelectionChange(&self); break;
                case GuestFileDialogEvent::TypeChanged:
                    handler->OnTypeChange(&self); break;
                case GuestFileDialogEvent::FileOk:
                    response = handler->OnFileOk(&self); break;
                case GuestFileDialogEvent::CheckButtonToggled:
                {
                    ComPtr<FileDialogControlEvents> controlEvents;
                    const auto* box = self.FindCheckBox(id);
                    if (box && SUCCEEDED(handler->QueryInterface(__uuidof(FileDialogControlEvents),
                        reinterpret_cast<void**>(controlEvents.GetAddressOf()))))
                        controlEvents->OnCheckButtonToggled(&self, id, box->checked);
                    break;
                }
                default: break;
                }
                if (response != S_OK)
                {
                    if (event == GuestFileDialogEvent::FileOk) self.m_results.clear();
                    return response;
                }
            }
            return S_OK;
        }

        DWORD m_options = IsSave ? 0x0000880au : 0x00001808u;
        UINT m_filterCount = 0;
        DWORD m_typeIndex = 1;
        DWORD m_nextCookie = 0;
        HWND m_window = nullptr;
        bool m_showing = false;
        HRESULT m_closeStatus = HRESULT_FROM_WIN32(ERROR_CANCELLED);
        std::wstring m_name;
        std::wstring m_title;
        std::wstring m_folder;
        std::wstring m_defaultFolder;
        std::wstring m_currentFolder;
        std::wstring m_filters;
        std::wstring m_extension;
        std::wstring m_okLabel;
        std::wstring m_nameLabel;
        std::vector<std::wstring> m_selection;
        std::vector<std::wstring> m_results;
        std::vector<GuestFileDialogCheckBox> m_checkBoxes;
        std::vector<std::pair<DWORD, ComPtr<FileDialogEvents>>> m_events;
    };
}

bool Win32Bridge::Bridge::IsGuestFileDialogClass(REFCLSID clsid)
{
    return IsEqualGUID(clsid, OpenDialogClass) || IsEqualGUID(clsid, SaveDialogClass);
}

HRESULT Win32Bridge::Bridge::CreateGuestFileDialog(REFCLSID clsid, REFIID iid, void** result)
{
    if (!result) return E_POINTER;
    *result = nullptr;
    const bool save = IsEqualGUID(clsid, SaveDialogClass);
    FileDialog* dialog = save
        ? static_cast<FileDialog*>(new (std::nothrow) VirtualFileDialog<SaveFileDialog>())
        : static_cast<FileDialog*>(new (std::nothrow) VirtualFileDialog<OpenFileDialog>());
    if (!dialog) return E_OUTOFMEMORY;
    const HRESULT status = dialog->QueryInterface(iid, result);
    dialog->Release();
    if (SUCCEEDED(status))
        RuntimeDiagnostics::Record(save ? L"COM: created a virtual File Save Dialog." :
            L"COM: created a virtual File Open Dialog.");
    return status;
}

HRESULT Win32Bridge::Bridge::CreateGuestShellItemFromPath(LPCWSTR path, REFIID iid, void** result)
{
    if (!result) return E_POINTER;
    *result = nullptr;
    if (!path || !*path) return E_INVALIDARG;
    auto* storage = CurrentGuestStorageContext();
    if (!storage) return E_FAIL;
    try
    {
        DWORD error = ERROR_SUCCESS;
        std::wstring canonical;
        if (!storage->CanonicalPath(path, &canonical, &error) ||
            storage->GetGuestFileAttributes(canonical.c_str(), &error) == INVALID_FILE_ATTRIBUTES)
            return HRESULT_FROM_WIN32(error ? error : ERROR_FILE_NOT_FOUND);
        ComPtr<FileShellItem> item;
        const HRESULT status = MakeFileShellItem(canonical, item.GetAddressOf());
        return FAILED(status) ? status : item.Get()->QueryInterface(iid, result);
    }
    catch (const std::bad_alloc&)
    {
        return E_OUTOFMEMORY;
    }
}
