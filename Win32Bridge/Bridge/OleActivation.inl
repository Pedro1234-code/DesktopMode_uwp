// Guest-only in-process activation. Registry discovery and server loading follow
// ReactOS ole32/compobj.c and combase: CLSID -> InprocServer32 -> DllGetClassObject.
namespace
{
    HRESULT ReadGuestComStringFromRoot(HKEY root, const std::wstring& path, LPCWSTR name, std::wstring* value,
        DWORD* valueType = nullptr)
    {
        value->clear();
        HKEY key = nullptr;
        LSTATUS status = BridgeRegOpenKeyExW(root, path.c_str(), 0, KEY_READ, &key);
        if (status != ERROR_SUCCESS) return (status == ERROR_FILE_NOT_FOUND || status == ERROR_PATH_NOT_FOUND) ? REGDB_E_CLASSNOTREG : REGDB_E_READREGDB;
        const auto close = [](void* handle) { BridgeRegCloseKey(static_cast<HKEY>(handle)); };
        std::unique_ptr<void, decltype(close)> guard(key, close);
        DWORD bytes = 0, type = REG_NONE;
        status = BridgeRegQueryValueExW(key, name, nullptr, &type, nullptr, &bytes);
        if (status != ERROR_SUCCESS) return (status == ERROR_FILE_NOT_FOUND || status == ERROR_PATH_NOT_FOUND) ? REGDB_E_CLASSNOTREG : REGDB_E_READREGDB;
        if ((type != REG_SZ && type != REG_EXPAND_SZ) || bytes % sizeof(wchar_t) || bytes > 65536)
            return REGDB_E_READREGDB;
        std::vector<wchar_t> buffer(bytes / sizeof(wchar_t) + 1, L'\0');
        status = BridgeRegQueryValueExW(key, name, nullptr, &type,
            reinterpret_cast<BYTE*>(buffer.data()), &bytes);
        if (status != ERROR_SUCCESS || (type != REG_SZ && type != REG_EXPAND_SZ) || bytes % sizeof(wchar_t))
            return REGDB_E_READREGDB;
        value->assign(buffer.data());
        if (valueType) *valueType = type;
        return S_OK;
    }

    HRESULT ReadGuestComString(const std::wstring& path, LPCWSTR name, std::wstring* value,
        DWORD* valueType = nullptr)
    {
        const HRESULT user = ReadGuestComStringFromRoot(HKEY_CURRENT_USER,
            L"Software\\Classes\\" + path, name, value, valueType);
        if (user != REGDB_E_CLASSNOTREG) return user;
        return ReadGuestComStringFromRoot(HKEY_CLASSES_ROOT, path, name, value, valueType);
    }

    std::wstring GuestClsidKey(REFCLSID clsid)
    {
        wchar_t text[39]{};
        FormatGuestGuid(clsid, text, 39);
        return std::wstring(L"CLSID\\") + text;
    }

    class GuestBuiltinClassFactory final : public IClassFactory
    {
    public:
        explicit GuestBuiltinClassFactory(REFCLSID clsid) : m_clsid(clsid) {}
        HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void** result) override
        {
            if (!result) return E_POINTER;
            *result = nullptr;
            if (iid != __uuidof(IUnknown) && iid != __uuidof(IClassFactory)) return E_NOINTERFACE;
            *result = static_cast<IClassFactory*>(this);
            AddRef();
            return S_OK;
        }
        ULONG STDMETHODCALLTYPE AddRef() override { return ++m_references; }
        ULONG STDMETHODCALLTYPE Release() override
        {
            const ULONG count = --m_references;
            if (!count) delete this;
            return count;
        }
        HRESULT STDMETHODCALLTYPE CreateInstance(IUnknown* outer, REFIID iid, void** result) override
        { return CreateBuiltinGuestComInstance(m_clsid, outer, CLSCTX_INPROC_SERVER, iid, result); }
        // Built-in implementations live for the entire runtime process.
        HRESULT STDMETHODCALLTYPE LockServer(BOOL) override { return S_OK; }
    private:
        std::atomic<ULONG> m_references{ 1 };
        CLSID m_clsid;
    };

    using GuestDllGetClassObject = HRESULT(STDAPICALLTYPE*)(REFCLSID, REFIID, void**);
    HRESULT CallGuestClassObject(GuestDllGetClassObject procedure, REFCLSID clsid, REFIID iid, void** result)
    {
        __try { return procedure(clsid, iid, result); }
        __except (EXCEPTION_EXECUTE_HANDLER) { *result = nullptr; return CO_E_ERRORINDLL; }
    }
    HRESULT CallGuestFactory(IClassFactory* factory, IUnknown* outer, REFIID iid, void** result)
    {
        __try { return factory->CreateInstance(outer, iid, result); }
        __except (EXCEPTION_EXECUTE_HANDLER) { *result = nullptr; return CO_E_ERRORINDLL; }
    }
}

static HRESULT WINAPI BridgeCoGetClassObject(REFCLSID clsid, DWORD context, LPVOID server,
    REFIID iid, LPVOID* result)
{
    if (!result) return E_POINTER;
    *result = nullptr;
    if (server) return E_NOTIMPL;
    if (!(context & CLSCTX_INPROC_SERVER)) return REGDB_E_CLASSNOTREG;
    if (IsEqualGUID(clsid, GuestClsidShellLink) || IsGuestFileDialogClass(clsid) || IsGuestWicFactoryClass(clsid))
    {
        auto factory = new (std::nothrow) GuestBuiltinClassFactory(clsid);
        if (!factory) return E_OUTOFMEMORY;
        const HRESULT status = factory->QueryInterface(iid, result);
        factory->Release();
        return status;
    }
    if (!g_comCount) return CO_E_NOTINITIALIZED;
    const std::wstring key = GuestClsidKey(clsid) + L"\\InprocServer32";
    std::wstring path;
    DWORD type = REG_NONE;
    HRESULT status = ReadGuestComString(key, nullptr, &path, &type);
    if (FAILED(status)) return status;
    if (path.empty()) return REGDB_E_CLASSNOTREG;
    if (type == REG_EXPAND_SZ)
    {
        const DWORD required = BridgeExpandEnvironmentStringsW(path.c_str(), nullptr, 0);
        if (!required || required > 32768) return REGDB_E_READREGDB;
        std::vector<wchar_t> expanded(required);
        const DWORD count = BridgeExpandEnvironmentStringsW(path.c_str(), expanded.data(), required);
        if (!count || count > required) return REGDB_E_READREGDB;
        path.assign(expanded.data());
    }
    if (path.size() >= 2 && path.front() == L'"' && path.back() == L'"')
        path = path.substr(1, path.size() - 2);
    std::wstring threading;
    status = ReadGuestComString(key, L"ThreadingModel", &threading);
    if (FAILED(status) && status != REGDB_E_CLASSNOTREG) return status;
    const bool apartment = threading.empty() || _wcsicmp(threading.c_str(), L"Apartment") == 0;
    const bool free = _wcsicmp(threading.c_str(), L"Free") == 0;
    if ((apartment && g_comApartment != COINIT_APARTMENTTHREADED) ||
        (free && g_comApartment != COINIT_MULTITHREADED))
        return RPC_E_WRONG_THREAD;
    if (!apartment && !free && _wcsicmp(threading.c_str(), L"Both") != 0 &&
        _wcsicmp(threading.c_str(), L"Neutral") != 0) return REGDB_E_READREGDB;

    GuestModuleLoader* loader = CurrentGuestModuleLoader();
    if (!loader) return CO_E_DLLNOTFOUND;
    ImportedSymbol symbol;
    symbol.library = path;
    symbol.name = L"DllGetClassObject";
    DWORD error = ERROR_SUCCESS;
    // ResolveImport retains a process reference. Class objects and instances
    // may outlive this call, so their server must remain mapped until teardown.
    const auto resolution = loader->ResolveImport(symbol, 0, &error);
    if (!resolution.targetAddress)
    {
        RuntimeDiagnostics::Record(L"COM ACTIVATION FAILED: " + key + L" -> " + path +
            L" error=" + std::to_wstring(error) + L" reason='" + resolution.note + L"'.");
        return error == ERROR_MOD_NOT_FOUND ? CO_E_DLLNOTFOUND : CO_E_ERRORINDLL;
    }
    status = CallGuestClassObject(reinterpret_cast<GuestDllGetClassObject>(resolution.targetAddress),
        clsid, iid, result);
    if (SUCCEEDED(status) && !*result) return E_UNEXPECTED;
    RuntimeDiagnostics::Record(L"COM ACTIVATION: " + key + L" -> " + path +
        L" HRESULT=" + std::to_wstring(static_cast<unsigned long>(status)) + L".");
    return status;
}

HRESULT WINAPI Win32Bridge::Bridge::BridgeCoCreateInstance(REFCLSID clsid, LPUNKNOWN outer,
    DWORD context, REFIID iid, LPVOID* result)
{
    if (!result) return E_POINTER;
    *result = nullptr;
    IClassFactory* factory = nullptr;
    HRESULT status = BridgeCoGetClassObject(clsid, context, nullptr, __uuidof(IClassFactory),
        reinterpret_cast<void**>(&factory));
    if (FAILED(status)) return status;
    status = CallGuestFactory(factory, outer, iid, result);
    GuestComRelease(factory);
    if (SUCCEEDED(status) && !*result) return E_UNEXPECTED;
    return status;
}

static HRESULT WINAPI BridgeCLSIDFromProgID(LPCOLESTR progId, LPCLSID clsid)
{
    if (!clsid) return E_POINTER;
    *clsid = GUID{};
    if (!progId || !*progId) return CO_E_CLASSSTRING;
    std::wstring current(progId);
    std::vector<std::wstring> visited;
    while (visited.size() < 16)
    {
        for (const auto& previous : visited)
            if (_wcsicmp(previous.c_str(), current.c_str()) == 0) return CO_E_CLASSSTRING;
        visited.push_back(current);
        std::wstring text;
        HRESULT status = ReadGuestComString(current + L"\\CLSID", nullptr, &text);
        if (SUCCEEDED(status)) return ParseGuestGuid(text.c_str(), clsid) ? S_OK : CO_E_CLASSSTRING;
        if (status != REGDB_E_CLASSNOTREG) return status;
        status = ReadGuestComString(current + L"\\CurVer", nullptr, &text);
        if (FAILED(status)) return status;
        if (text.empty()) return CO_E_CLASSSTRING;
        current = std::move(text);
    }
    return CO_E_CLASSSTRING;
}
static HRESULT WINAPI BridgeCLSIDFromString(LPCOLESTR text, LPCLSID clsid)
{
    if (!clsid) return E_INVALIDARG;
    *clsid = GUID{};
    if (!text) return S_OK;
    if (text[0] == L'{') return ParseGuestGuid(text, clsid) ? S_OK : CO_E_CLASSSTRING;
    return BridgeCLSIDFromProgID(text, clsid);
}
static HRESULT WINAPI BridgeIIDFromString(LPCOLESTR text, IID* iid)
{
    if (!iid) return E_INVALIDARG;
    *iid = GUID{};
    if (!text) return S_OK;
    return ParseGuestGuid(text, iid) ? S_OK : CO_E_IIDSTRING;
}
static int WINAPI BridgeStringFromGUID2(REFGUID guid, LPOLESTR text, int count)
{ return FormatGuestGuid(guid, text, count); }
static HRESULT WINAPI BridgeStringFromCLSID(REFCLSID clsid, LPOLESTR* text)
{
    if (!text) return E_POINTER;
    *text = static_cast<LPOLESTR>(BridgeCoTaskMemAlloc(39 * sizeof(wchar_t)));
    if (!*text) return E_OUTOFMEMORY;
    FormatGuestGuid(clsid, *text, 39);
    return S_OK;
}
static HRESULT WINAPI BridgeProgIDFromCLSID(REFCLSID clsid, LPOLESTR* result)
{
    if (!result) return E_POINTER;
    *result = nullptr;
    std::wstring text;
    const HRESULT status = ReadGuestComString(GuestClsidKey(clsid) + L"\\ProgID", nullptr, &text);
    if (FAILED(status)) return status;
    *result = static_cast<LPOLESTR>(BridgeCoTaskMemAlloc((text.size() + 1) * sizeof(wchar_t)));
    if (!*result) return E_OUTOFMEMORY;
    std::memcpy(*result, text.c_str(), (text.size() + 1) * sizeof(wchar_t));
    return S_OK;
}
static HRESULT WINAPI BridgeCoCreateGuid(GUID* guid)
{
    if (!guid) return E_INVALIDARG;
    const NTSTATUS status = ::BCryptGenRandom(nullptr, reinterpret_cast<BYTE*>(guid), sizeof(*guid),
        BCRYPT_USE_SYSTEM_PREFERRED_RNG);
    if (!BCRYPT_SUCCESS(status)) { *guid = GUID{}; return E_FAIL; }
    guid->Data3 = static_cast<USHORT>((guid->Data3 & 0x0fff) | 0x4000);
    guid->Data4[0] = static_cast<BYTE>((guid->Data4[0] & 0x3f) | 0x80);
    return S_OK;
}
static LPVOID WINAPI BridgeCoTaskMemRealloc(LPVOID memory, SIZE_T bytes)
{ return ::CoTaskMemRealloc(memory, bytes); }
struct GuestMultiQi
{
    const IID* iid;
    IUnknown* object;
    HRESULT result;
};

static HRESULT QueryGuestComInterface(IUnknown* object, REFIID iid, void** result)
{
    __try { return object->QueryInterface(iid, result); }
    __except (EXCEPTION_EXECUTE_HANDLER) { *result = nullptr; return CO_E_ERRORINDLL; }
}
static HRESULT WINAPI BridgeCoCreateInstanceEx(REFCLSID clsid, IUnknown* outer, DWORD context,
    LPVOID server, DWORD count, GuestMultiQi* interfaces)
{
    if (!count || !interfaces) return E_INVALIDARG;
    bool invalid = false;
    for (DWORD index = 0; index < count; ++index)
    {
        interfaces[index].object = nullptr;
        interfaces[index].result = E_NOINTERFACE;
        invalid = invalid || !interfaces[index].iid;
    }
    if (invalid) return E_INVALIDARG;
    HRESULT status = S_OK;
    if (server) status = E_NOTIMPL;
    else if (outer && (count != 1 || *interfaces[0].iid != __uuidof(IUnknown))) status = E_INVALIDARG;
    IUnknown* object = nullptr;
    if (SUCCEEDED(status))
        status = BridgeCoCreateInstance(clsid, outer, context, __uuidof(IUnknown), reinterpret_cast<void**>(&object));
    if (FAILED(status))
    {
        for (DWORD index = 0; index < count; ++index) interfaces[index].result = status;
        return status;
    }
    DWORD succeeded = 0;
    for (DWORD index = 0; index < count; ++index)
    {
        auto& query = interfaces[index];
        query.result = QueryGuestComInterface(object, *query.iid, reinterpret_cast<void**>(&query.object));
        if (SUCCEEDED(query.result) && !query.object) query.result = E_UNEXPECTED;
        if (SUCCEEDED(query.result)) ++succeeded;
    }
    GuestComRelease(object);
    if (!succeeded) return E_NOINTERFACE;
    return succeeded == count ? S_OK : CO_S_NOTALLINTERFACES;
}
static BOOL WINAPI BridgeIsEqualGuid(REFGUID left, REFGUID right)
{ return IsEqualGUID(left, right); }