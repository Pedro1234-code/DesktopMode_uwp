    // Keep these class IDs local so the optional D3D12 backend needs no import library.
    constexpr GUID D3D12FactoryClass{0x114863bf, 0xc386, 0x4aee, {0xb3, 0x9d, 0x8f, 0x0b, 0xbb, 0x06, 0x29, 0x55}};
    constexpr GUID D3D12SdkClass{0x7cda6aca, 0xa03e, 0x49c8, {0x94, 0x58, 0x03, 0x34, 0xd2, 0x0e, 0x07, 0xce}};

    bool InboxSupportsD3D12Sdk(UINT sdkVersion)
    {
        if (!sdkVersion) return true;
        static const HMODULE core = ::LoadLibraryExW(L"D3D12Core.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
        const auto version = core ? reinterpret_cast<const UINT*>(::GetProcAddress(core, "D3D12SDKVersion")) : nullptr;
        return version && *version >= sdkVersion;
    }

    bool ReadD3D12SdkVersion(const BYTE* address, UINT* version)
    {
        __try
        {
            std::memcpy(version, address, sizeof(*version));
            return true;
        }
        __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
    }

    HRESULT CheckGuestD3D12SdkVersion()
    {
        const auto runtime = CurrentGuestRuntime();
        if (!runtime) return S_OK;
        for (const auto& entry : runtime->Metadata().exports)
        {
            if (entry.name != L"D3D12SDKVersion") continue;
            if (!entry.forwarder.empty() || !runtime->ImageBase() || entry.rva > runtime->ImageSize() ||
                runtime->ImageSize() - entry.rva < sizeof(UINT)) return E_INVALIDARG;
            UINT version = 0;
            if (!ReadD3D12SdkVersion(runtime->ImageBase() + entry.rva, &version)) return E_INVALIDARG;
            return InboxSupportsD3D12Sdk(version) ? S_OK : DXGI_ERROR_UNSUPPORTED;
        }
        return S_OK;
    }

    HRESULT GetInboxD3D12Factory(UINT sdkVersion, ID3D12DeviceFactory** factory)
    {
        if (!InboxSupportsD3D12Sdk(sdkVersion)) return DXGI_ERROR_UNSUPPORTED;
        const auto get = reinterpret_cast<PFN_D3D12_GET_INTERFACE>(FindD3D12Procedure("D3D12GetInterface"));
        return get ? get(D3D12FactoryClass, IID_PPV_ARGS(factory)) : DXGI_ERROR_UNSUPPORTED;
    }

    HRESULT SelectGuestD3D12Factory(ID3D12DeviceFactory* factory)
    {
        const auto kernel = CurrentGuestKernelContext();
        if (!kernel) return E_HANDLE;
        try
        {
            factory->AddRef();
            std::shared_ptr<IUnknown> retained(factory, [](IUnknown* value) { value->Release(); });
            return kernel->SetD3D12Factory(std::move(retained)) ? S_OK : HRESULT_FROM_WIN32(ERROR_OPERATION_ABORTED);
        }
        catch (const std::bad_alloc&) { return E_OUTOFMEMORY; }
    }

    HRESULT WrapD3D12SdkConfiguration(REFIID iid, void** output);

    class GuestD3D12Factory final : public ID3D12DeviceFactory
    {
    public:
        explicit GuestD3D12Factory(ComPtr<ID3D12DeviceFactory> native) : m_native(std::move(native)) {}

        HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void** output) override
        {
            if (!output) return E_POINTER;
            *output = nullptr;
            if (iid != __uuidof(IUnknown) && iid != __uuidof(ID3D12DeviceFactory)) return E_NOINTERFACE;
            *output = static_cast<ID3D12DeviceFactory*>(this);
            AddRef();
            return S_OK;
        }

        ULONG STDMETHODCALLTYPE AddRef() override { return ++m_refs; }
        ULONG STDMETHODCALLTYPE Release() override
        {
            const ULONG refs = --m_refs;
            if (!refs) delete this;
            return refs;
        }

        HRESULT STDMETHODCALLTYPE InitializeFromGlobalState() override
        { return m_native->InitializeFromGlobalState(); }
        HRESULT STDMETHODCALLTYPE ApplyToGlobalState() override
        { return SelectGuestD3D12Factory(m_native.Get()); }
        HRESULT STDMETHODCALLTYPE SetFlags(D3D12_DEVICE_FACTORY_FLAGS flags) override
        { return m_native->SetFlags(flags); }
        D3D12_DEVICE_FACTORY_FLAGS STDMETHODCALLTYPE GetFlags() override
        { return m_native->GetFlags(); }
        HRESULT STDMETHODCALLTYPE GetConfigurationInterface(REFCLSID clsid, REFIID iid, void** output) override
        {
            if (clsid == D3D12SdkClass) return WrapD3D12SdkConfiguration(iid, output);
            if (clsid == D3D12FactoryClass) return QueryInterface(iid, output);
            return m_native->GetConfigurationInterface(clsid, iid, output);
        }
        HRESULT STDMETHODCALLTYPE EnableExperimentalFeatures(UINT count, const IID* features, void* configurations,
            UINT* configurationSizes) override
        { return m_native->EnableExperimentalFeatures(count, features, configurations, configurationSizes); }
        HRESULT STDMETHODCALLTYPE CreateDevice(IUnknown* adapter, D3D_FEATURE_LEVEL level, REFIID iid, void** output) override
        {
            return CreateD3D12GuestDevice(adapter, level, iid, output, [&](IUnknown* native, D3D_FEATURE_LEVEL feature,
                REFIID requested, void** device)
            {
                return m_native->CreateDevice(native, feature, requested, device);
            });
        }

    private:
        ComPtr<ID3D12DeviceFactory> m_native;
        std::atomic<ULONG> m_refs{1};
    };

    HRESULT CreateInboxD3D12GuestFactory(UINT version, REFIID iid, void** output)
    {
        if (!output) return E_POINTER;
        *output = nullptr;
        if (iid != __uuidof(IUnknown) && iid != __uuidof(ID3D12DeviceFactory)) return E_NOINTERFACE;
        ComPtr<ID3D12DeviceFactory> native;
        const HRESULT status = GetInboxD3D12Factory(version, &native);
        if (FAILED(status)) return status;
        auto factory = new (std::nothrow) GuestD3D12Factory(std::move(native));
        if (!factory) return E_OUTOFMEMORY;
        *output = static_cast<ID3D12DeviceFactory*>(factory);
        return status;
    }

    class GuestD3D12SdkConfiguration final : public ID3D12SDKConfiguration1
    {
    public:
        HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void** output) override
        {
            if (!output) return E_POINTER;
            *output = nullptr;
            if (iid != __uuidof(IUnknown) && iid != __uuidof(ID3D12SDKConfiguration) &&
                iid != __uuidof(ID3D12SDKConfiguration1)) return E_NOINTERFACE;
            *output = static_cast<ID3D12SDKConfiguration1*>(this);
            AddRef();
            return S_OK;
        }

        ULONG STDMETHODCALLTYPE AddRef() override { return ++m_refs; }
        ULONG STDMETHODCALLTYPE Release() override
        {
            const ULONG refs = --m_refs;
            if (!refs) delete this;
            return refs;
        }

        HRESULT STDMETHODCALLTYPE SetSDKVersion(UINT version, LPCSTR path) override
        {
            if (!path) return E_INVALIDARG;
            ComPtr<ID3D12DeviceFactory> factory;
            const HRESULT status = GetInboxD3D12Factory(version, &factory);
            return FAILED(status) ? status : SelectGuestD3D12Factory(factory.Get());
        }
        HRESULT STDMETHODCALLTYPE CreateDeviceFactory(UINT version, LPCSTR path, REFIID iid, void** output) override
        {
            if (output) *output = nullptr;
            if (!path) return E_INVALIDARG;
            return CreateInboxD3D12GuestFactory(version, iid, output);
        }
        void STDMETHODCALLTYPE FreeUnusedSDKs() override
        {
            // The Bridge uses the system SDK, which is owned by the host.
        }

    private:
        std::atomic<ULONG> m_refs{1};
    };

    HRESULT WrapD3D12SdkConfiguration(REFIID iid, void** output)
    {
        if (!output) return E_POINTER;
        *output = nullptr;
        auto configuration = new (std::nothrow) GuestD3D12SdkConfiguration();
        if (!configuration) return E_OUTOFMEMORY;
        const HRESULT status = configuration->QueryInterface(iid, output);
        configuration->Release();
        return status;
    }

    HRESULT WINAPI GuestD3D12GetInterface(REFCLSID clsid, REFIID iid, void** output)
    {
        if (!output) return E_POINTER;
        *output = nullptr;
        if (clsid == D3D12FactoryClass) return CreateInboxD3D12GuestFactory(0, iid, output);
        if (clsid == D3D12SdkClass) return WrapD3D12SdkConfiguration(iid, output);
        return E_NOINTERFACE;
    }
