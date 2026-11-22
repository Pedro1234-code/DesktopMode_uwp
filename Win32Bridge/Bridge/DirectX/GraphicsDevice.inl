    class GuestDxgiAdapter final : public IDXGIAdapter4, public GuestGraphicsObject
    {
    public:
        GuestDxgiAdapter(ComPtr<IDXGIAdapter> native, ComPtr<IDXGIFactory2> parent)
            : m_native(std::move(native)), m_parent(std::move(parent)) {}

        HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void** result) override
        {
            if (!result) return E_POINTER;
            *result = nullptr;
            if (iid == __uuidof(GuestGraphicsObject))
                *result = static_cast<GuestGraphicsObject*>(this);
            else if (iid == __uuidof(IUnknown) || iid == __uuidof(IDXGIObject) || iid == __uuidof(IDXGIAdapter) ||
                iid == __uuidof(IDXGIAdapter1) || iid == __uuidof(IDXGIAdapter2) ||
                iid == __uuidof(IDXGIAdapter3) || iid == __uuidof(IDXGIAdapter4))
            {
                ComPtr<IUnknown> supported;
                const HRESULT status = m_native->QueryInterface(iid, reinterpret_cast<void**>(supported.GetAddressOf()));
                if (FAILED(status)) return status;
                *result = static_cast<IDXGIAdapter4*>(this);
            }
            else return E_NOINTERFACE;
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

        IUnknown* STDMETHODCALLTYPE NativeObject() override { return m_native.Get(); }

        HRESULT STDMETHODCALLTYPE GetParent(REFIID iid, void** parent) override
        { return m_parent->QueryInterface(iid, parent); }

        HRESULT STDMETHODCALLTYPE SetPrivateData(REFGUID Name, UINT DataSize, const void* pData) override
        { return m_native->SetPrivateData(Name, DataSize, pData); }

        HRESULT STDMETHODCALLTYPE SetPrivateDataInterface(REFGUID Name, const IUnknown* pUnknown) override
        { return m_native->SetPrivateDataInterface(Name, pUnknown); }

        HRESULT STDMETHODCALLTYPE GetPrivateData(REFGUID Name, UINT* pDataSize, void* pData) override
        { return m_native->GetPrivateData(Name, pDataSize, pData); }

        HRESULT STDMETHODCALLTYPE EnumOutputs(UINT Output, IDXGIOutput** ppOutput) override
        { return m_native->EnumOutputs(Output, ppOutput); }

        HRESULT STDMETHODCALLTYPE GetDesc(DXGI_ADAPTER_DESC* pDesc) override
        { return m_native->GetDesc(pDesc); }

        HRESULT STDMETHODCALLTYPE CheckInterfaceSupport(REFGUID InterfaceName, LARGE_INTEGER* pUMDVersion) override
        { return m_native->CheckInterfaceSupport(InterfaceName, pUMDVersion); }

        HRESULT STDMETHODCALLTYPE GetDesc1(DXGI_ADAPTER_DESC1* pDesc) override
        {
            ComPtr<IDXGIAdapter1> native;
            const HRESULT status = m_native.As(&native);
            return FAILED(status) ? status : native->GetDesc1(pDesc);
        }

        HRESULT STDMETHODCALLTYPE GetDesc2(DXGI_ADAPTER_DESC2* pDesc) override
        {
            ComPtr<IDXGIAdapter2> native;
            const HRESULT status = m_native.As(&native);
            return FAILED(status) ? status : native->GetDesc2(pDesc);
        }

        HRESULT STDMETHODCALLTYPE RegisterHardwareContentProtectionTeardownStatusEvent(HANDLE event, DWORD* cookie) override
        {
            ComPtr<IDXGIAdapter3> native;
            const HRESULT status = m_native.As(&native);
            return FAILED(status) ? status : m_protectionEvents.Add(native, event, cookie,
                &IDXGIAdapter3::RegisterHardwareContentProtectionTeardownStatusEvent,
                &IDXGIAdapter3::UnregisterHardwareContentProtectionTeardownStatus);
        }

        void STDMETHODCALLTYPE UnregisterHardwareContentProtectionTeardownStatus(DWORD cookie) override
        { m_protectionEvents.Remove(cookie); }

        HRESULT STDMETHODCALLTYPE QueryVideoMemoryInfo(UINT NodeIndex, DXGI_MEMORY_SEGMENT_GROUP MemorySegmentGroup,
            DXGI_QUERY_VIDEO_MEMORY_INFO* pVideoMemoryInfo) override
        {
            ComPtr<IDXGIAdapter3> native;
            const HRESULT status = m_native.As(&native);
            return FAILED(status) ? status : native->QueryVideoMemoryInfo(NodeIndex, MemorySegmentGroup, pVideoMemoryInfo);
        }

        HRESULT STDMETHODCALLTYPE SetVideoMemoryReservation(UINT NodeIndex, DXGI_MEMORY_SEGMENT_GROUP MemorySegmentGroup,
            UINT64 Reservation) override
        {
            ComPtr<IDXGIAdapter3> native;
            const HRESULT status = m_native.As(&native);
            return FAILED(status) ? status : native->SetVideoMemoryReservation(NodeIndex, MemorySegmentGroup, Reservation);
        }

        HRESULT STDMETHODCALLTYPE RegisterVideoMemoryBudgetChangeNotificationEvent(HANDLE event, DWORD* cookie) override
        {
            ComPtr<IDXGIAdapter3> native;
            const HRESULT status = m_native.As(&native);
            return FAILED(status) ? status : m_budgetEvents.Add(native, event, cookie,
                &IDXGIAdapter3::RegisterVideoMemoryBudgetChangeNotificationEvent,
                &IDXGIAdapter3::UnregisterVideoMemoryBudgetChangeNotification);
        }

        void STDMETHODCALLTYPE UnregisterVideoMemoryBudgetChangeNotification(DWORD cookie) override
        { m_budgetEvents.Remove(cookie); }

        HRESULT STDMETHODCALLTYPE GetDesc3(DXGI_ADAPTER_DESC3* pDesc) override
        {
            ComPtr<IDXGIAdapter4> native;
            const HRESULT status = m_native.As(&native);
            return FAILED(status) ? status : native->GetDesc3(pDesc);
        }

    private:
        std::atomic<ULONG> m_refs{1};
        ComPtr<IDXGIAdapter> m_native;
        ComPtr<IDXGIFactory2> m_parent;
        GraphicsEventNotifications m_protectionEvents;
        GraphicsEventNotifications m_budgetEvents;
    };

    HRESULT WrapAdapter(ComPtr<IDXGIAdapter> native, REFIID iid, void** output, IDXGIFactory2* parent)
    {
        if (!output) return E_POINTER;
        *output = nullptr;
        if (!native) return E_INVALIDARG;
        ComPtr<IDXGIFactory2> guestParent(parent);
        if (!guestParent)
        {
            ComPtr<IDXGIFactory2> factory;
            HRESULT status = native->GetParent(IID_PPV_ARGS(&factory));
            if (FAILED(status)) return status;
            status = WrapFactory(std::move(factory), IID_PPV_ARGS(&guestParent));
            if (FAILED(status)) return status;
        }
        auto adapter = new (std::nothrow) GuestDxgiAdapter(std::move(native), std::move(guestParent));
        if (!adapter) return E_OUTOFMEMORY;
        const HRESULT status = adapter->QueryInterface(iid, output);
        adapter->Release();
        return status;
    }

    // The DXGI face of a D3D11 device uses the same controlling IUnknown
    // as the D3D11 face. Its adapter always leads to a guest DXGI factory.
    class GuestDxgiDevice final : public IDXGIDevice4
    {
    public:
        GuestDxgiDevice(ID3D11Device* owner, ComPtr<IDXGIDevice> native, ComPtr<IDXGIAdapter> adapter)
            : m_owner(owner), m_native(std::move(native)), m_adapter(std::move(adapter)) {}

        HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void** result) override
        { return m_owner->QueryInterface(iid, result); }
        ULONG STDMETHODCALLTYPE AddRef() override { return m_owner->AddRef(); }
        ULONG STDMETHODCALLTYPE Release() override { return m_owner->Release(); }

        HRESULT STDMETHODCALLTYPE GetAdapter(IDXGIAdapter** adapter) override
        { return adapter ? m_adapter.CopyTo(adapter) : E_POINTER; }

        HRESULT STDMETHODCALLTYPE GetParent(REFIID iid, void** parent) override
        { return m_adapter->QueryInterface(iid, parent); }

        HRESULT STDMETHODCALLTYPE SetPrivateData(REFGUID Name, UINT DataSize, const void* pData) override
        { return m_native->SetPrivateData(Name, DataSize, pData); }

        HRESULT STDMETHODCALLTYPE SetPrivateDataInterface(REFGUID Name, const IUnknown* pUnknown) override
        { return m_native->SetPrivateDataInterface(Name, pUnknown); }

        HRESULT STDMETHODCALLTYPE GetPrivateData(REFGUID Name, UINT* pDataSize, void* pData) override
        { return m_native->GetPrivateData(Name, pDataSize, pData); }

        HRESULT STDMETHODCALLTYPE CreateSurface(const DXGI_SURFACE_DESC* pDesc, UINT NumSurfaces, DXGI_USAGE Usage,
            const DXGI_SHARED_RESOURCE* pSharedResource, IDXGISurface** ppSurface) override
        {
            const HRESULT result = m_native->CreateSurface(pDesc, NumSurfaces, Usage, pSharedResource, ppSurface);
            if (FAILED(result) || !ppSurface) return result;
            for (UINT i = 0; i < NumSurfaces; ++i)
            {
                ComPtr<ID3D11Texture2D> texture;
                if (!ppSurface[i] || FAILED(ppSurface[i]->QueryInterface(IID_PPV_ARGS(&texture)))) continue;
                ComPtr<IDXGISurface> surface;
                const HRESULT wrapped = WrapD3D11Texture(std::move(texture), m_owner, IID_PPV_ARGS(&surface));
                if (FAILED(wrapped))
                {
                    // The entire output array belongs to this call, including entries already wrapped.
                    for (UINT j = 0; j < NumSurfaces; ++j)
                    {
                        if (ppSurface[j]) ppSurface[j]->Release();
                        ppSurface[j] = nullptr;
                    }
                    return wrapped;
                }
                ppSurface[i]->Release();
                ppSurface[i] = surface.Detach();
            }
            return result;
        }

        HRESULT STDMETHODCALLTYPE QueryResourceResidency(IUnknown*const* ppResources, DXGI_RESIDENCY* pResidencyStatus,
            UINT NumResources) override
        {
            return WithNativeGraphicsObjects(NumResources, ppResources, [&](IUnknown* const* resources)
            { return m_native->QueryResourceResidency(resources, pResidencyStatus, NumResources); });
        }

        HRESULT STDMETHODCALLTYPE SetGPUThreadPriority(INT Priority) override
        { return m_native->SetGPUThreadPriority(Priority); }

        HRESULT STDMETHODCALLTYPE GetGPUThreadPriority(INT* pPriority) override
        { return m_native->GetGPUThreadPriority(pPriority); }

        HRESULT STDMETHODCALLTYPE SetMaximumFrameLatency(UINT MaxLatency) override
        {
            ComPtr<IDXGIDevice1> native;
            const HRESULT status = m_native.As(&native);
            return FAILED(status) ? status : native->SetMaximumFrameLatency(MaxLatency);
        }

        HRESULT STDMETHODCALLTYPE GetMaximumFrameLatency(UINT* pMaxLatency) override
        {
            ComPtr<IDXGIDevice1> native;
            const HRESULT status = m_native.As(&native);
            return FAILED(status) ? status : native->GetMaximumFrameLatency(pMaxLatency);
        }

        HRESULT STDMETHODCALLTYPE OfferResources(UINT NumResources, IDXGIResource*const* ppResources,
            DXGI_OFFER_RESOURCE_PRIORITY Priority) override
        {
            ComPtr<IDXGIDevice2> native;
            const HRESULT status = m_native.As(&native);
            if (FAILED(status)) return status;
            return WithNativeGraphicsObjects(NumResources, ppResources, [&](IDXGIResource* const* resources)
            { return native->OfferResources(NumResources, resources, Priority); });
        }

        HRESULT STDMETHODCALLTYPE ReclaimResources(UINT NumResources, IDXGIResource*const* ppResources, BOOL* pDiscarded) override
        {
            ComPtr<IDXGIDevice2> native;
            const HRESULT status = m_native.As(&native);
            if (FAILED(status)) return status;
            return WithNativeGraphicsObjects(NumResources, ppResources, [&](IDXGIResource* const* resources)
            { return native->ReclaimResources(NumResources, resources, pDiscarded); });
        }

        HRESULT STDMETHODCALLTYPE EnqueueSetEvent(HANDLE event) override
        {
            if (!event) return E_INVALIDARG;
            ComPtr<IDXGIDevice2> native;
            const HRESULT status = m_native.As(&native);
            return FAILED(status) ? status : RegisterGraphicsEvent(event, [&](HANDLE handle)
            { return native->EnqueueSetEvent(handle); });
        }

        void STDMETHODCALLTYPE Trim() override
        {
            ComPtr<IDXGIDevice3> native;
            if (SUCCEEDED(m_native.As(&native))) native->Trim();
        }

        HRESULT STDMETHODCALLTYPE OfferResources1(UINT NumResources, IDXGIResource*const* ppResources,
            DXGI_OFFER_RESOURCE_PRIORITY Priority, UINT Flags) override
        {
            ComPtr<IDXGIDevice4> native;
            const HRESULT status = m_native.As(&native);
            if (FAILED(status)) return status;
            return WithNativeGraphicsObjects(NumResources, ppResources, [&](IDXGIResource* const* resources)
            { return native->OfferResources1(NumResources, resources, Priority, Flags); });
        }

        HRESULT STDMETHODCALLTYPE ReclaimResources1(UINT NumResources, IDXGIResource*const* ppResources,
            DXGI_RECLAIM_RESOURCE_RESULTS* pResults) override
        {
            ComPtr<IDXGIDevice4> native;
            const HRESULT status = m_native.As(&native);
            if (FAILED(status)) return status;
            return WithNativeGraphicsObjects(NumResources, ppResources, [&](IDXGIResource* const* resources)
            { return native->ReclaimResources1(NumResources, resources, pResults); });
        }

    private:
        ID3D11Device* m_owner;
        ComPtr<IDXGIDevice> m_native;
        ComPtr<IDXGIAdapter> m_adapter;
    };

    class GuestD3D11Fence final : public ID3D11Fence, public GuestGraphicsObject
    {
    public:
        GuestD3D11Fence(ComPtr<ID3D11Fence> native, ID3D11Device* device)
            : m_native(std::move(native)), m_device(device) {}

        HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void** output) override
        {
            if (!output) return E_POINTER;
            *output = nullptr;
            if (iid == __uuidof(GuestGraphicsObject)) *output = static_cast<GuestGraphicsObject*>(this);
            else if (iid == __uuidof(IUnknown) || iid == __uuidof(ID3D11DeviceChild) || iid == __uuidof(ID3D11Fence))
                *output = static_cast<ID3D11Fence*>(this);
            else return m_native->QueryInterface(iid, output);
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

        IUnknown* STDMETHODCALLTYPE NativeObject() override { return m_native.Get(); }
        void STDMETHODCALLTYPE GetDevice(ID3D11Device** output) override
        { if (output) m_device.CopyTo(output); }
        HRESULT STDMETHODCALLTYPE GetPrivateData(REFGUID guid, UINT* size, void* data) override
        { return m_native->GetPrivateData(guid, size, data); }
        HRESULT STDMETHODCALLTYPE SetPrivateData(REFGUID guid, UINT size, const void* data) override
        { return m_native->SetPrivateData(guid, size, data); }
        HRESULT STDMETHODCALLTYPE SetPrivateDataInterface(REFGUID guid, const IUnknown* data) override
        { return m_native->SetPrivateDataInterface(guid, data); }

        HRESULT STDMETHODCALLTYPE CreateSharedHandle(const SECURITY_ATTRIBUTES* attributes, DWORD access,
            LPCWSTR name, HANDLE* output) override
        {
            return CreateGraphicsShare(attributes, name, output, [&](HANDLE* handle)
            {
                return m_native->CreateSharedHandle(nullptr, access, nullptr, handle);
            });
        }
        UINT64 STDMETHODCALLTYPE GetCompletedValue() override { return m_native->GetCompletedValue(); }
        HRESULT STDMETHODCALLTYPE SetEventOnCompletion(UINT64 value, HANDLE event) override
        {
            return RegisterGraphicsEvent(event, [&](HANDLE handle)
            {
                return m_native->SetEventOnCompletion(value, handle);
            });
        }

    private:
        std::atomic<ULONG> m_refs{1};
        ComPtr<ID3D11Fence> m_native;
        ComPtr<ID3D11Device> m_device;
    };

    template<class Create>
    HRESULT CreateD3D11Fence(ID3D11Device* device, REFIID iid, void** output, const Create& create)
    {
        if (!output) return create(iid, nullptr);
        *output = nullptr;
        ComPtr<IUnknown> original;
        const HRESULT status = create(iid, reinterpret_cast<void**>(original.GetAddressOf()));
        if (FAILED(status) || !original) return status;
        ComPtr<ID3D11Fence> native;
        const HRESULT queried = original.As(&native);
        if (FAILED(queried)) return queried;
        auto fence = new (std::nothrow) GuestD3D11Fence(std::move(native), device);
        if (!fence) return E_OUTOFMEMORY;
        const HRESULT wrapped = fence->QueryInterface(iid, output);
        fence->Release();
        return FAILED(wrapped) ? wrapped : status;
    }

    class GuestD3D11On12Device final : public ID3D11On12Device2
    {
    public:
        GuestD3D11On12Device(ID3D11Device* owner, ID3D11Device* native, IUnknown* device12)
            : m_owner(owner), m_device12(device12)
        {
            native->QueryInterface(IID_PPV_ARGS(&m_native));
            if (m_native)
            {
                m_native.As(&m_native1);
                m_native.As(&m_native2);
            }
        }

        bool Supports(REFIID iid) const
        {
            return (iid == __uuidof(ID3D11On12Device) && m_native) ||
                (iid == __uuidof(ID3D11On12Device1) && m_native1) ||
                (iid == __uuidof(ID3D11On12Device2) && m_native2);
        }

        HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void** output) override
        { return m_owner->QueryInterface(iid, output); }
        ULONG STDMETHODCALLTYPE AddRef() override { return m_owner->AddRef(); }
        ULONG STDMETHODCALLTYPE Release() override { return m_owner->Release(); }

        HRESULT STDMETHODCALLTYPE CreateWrappedResource(IUnknown* resource12, const D3D11_RESOURCE_FLAGS* flags,
            D3D12_RESOURCE_STATES inState, D3D12_RESOURCE_STATES outState, REFIID iid, void** output) override
        {
            const auto resource = NativeGraphicsObject(resource12);
            return OpenD3D11Resource(m_owner, iid, output, [&](void** native)
            { return m_native->CreateWrappedResource(resource.Get(), flags, inState, outState, iid, native); });
        }

        void STDMETHODCALLTYPE ReleaseWrappedResources(ID3D11Resource* const* resources, UINT count) override
        {
            WithNativeGraphicsObjects(count, resources, [&](ID3D11Resource* const* native)
            { m_native->ReleaseWrappedResources(native, count); return S_OK; });
        }

        void STDMETHODCALLTYPE AcquireWrappedResources(ID3D11Resource* const* resources, UINT count) override
        {
            WithNativeGraphicsObjects(count, resources, [&](ID3D11Resource* const* native)
            { m_native->AcquireWrappedResources(native, count); return S_OK; });
        }

        HRESULT STDMETHODCALLTYPE GetD3D12Device(REFIID iid, void** output) override
        { return m_device12 ? m_device12->QueryInterface(iid, output) : m_native1->GetD3D12Device(iid, output); }

        HRESULT STDMETHODCALLTYPE UnwrapUnderlyingResource(ID3D11Resource* resource11, ID3D12CommandQueue* queue,
            REFIID iid, void** output) override
        {
            ComPtr<ID3D12CommandQueue> nativeQueue;
            if (queue)
            {
                const HRESULT status = NativeGraphicsObject(queue).As(&nativeQueue);
                if (FAILED(status)) return status;
            }
            return m_native2->UnwrapUnderlyingResource(NativeD3D11Child(resource11), nativeQueue.Get(), iid, output);
        }

        HRESULT STDMETHODCALLTYPE ReturnUnderlyingResource(ID3D11Resource* resource11, UINT count,
            UINT64* signalValues, ID3D12Fence** fences) override
        {
            return WithNativeGraphicsObjects(count, fences, [&](ID3D12Fence* const* native)
            { return m_native2->ReturnUnderlyingResource(NativeD3D11Child(resource11), count, signalValues,
                const_cast<ID3D12Fence**>(native)); });
        }

    private:
        ID3D11Device* m_owner;
        ComPtr<IUnknown> m_device12;
        ComPtr<ID3D11On12Device> m_native;
        ComPtr<ID3D11On12Device1> m_native1;
        ComPtr<ID3D11On12Device2> m_native2;
    };

    class GuestD3D11Device final : public ID3D11Device5, public GuestGraphicsObject
    {
    public:
        GuestD3D11Device(ComPtr<ID3D11Device> native, ComPtr<IDXGIDevice> dxgi, ComPtr<IDXGIAdapter> adapter,
            IUnknown* device12)
            : m_native(std::move(native)), m_dxgi(static_cast<ID3D11Device5*>(this), std::move(dxgi), std::move(adapter)),
              m_video(this, m_native.Get()), m_on12(this, m_native.Get(), device12) {}

        HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void** result) override
        {
            if (!result) return E_POINTER;
            *result = nullptr;
            if (iid == __uuidof(GuestGraphicsObject))
            {
                *result = static_cast<GuestGraphicsObject*>(this);
                AddRef();
                return S_OK;
            }
            const bool d3d = iid == __uuidof(IUnknown) || iid == __uuidof(ID3D11Device) ||
                iid == __uuidof(ID3D11Device1) || iid == __uuidof(ID3D11Device2) || iid == __uuidof(ID3D11Device3) ||
                iid == __uuidof(ID3D11Device4) || iid == __uuidof(ID3D11Device5);
            const bool dxgi = iid == __uuidof(IDXGIObject) || iid == __uuidof(IDXGIDevice) ||
                iid == __uuidof(IDXGIDevice1) || iid == __uuidof(IDXGIDevice2) || iid == __uuidof(IDXGIDevice3) ||
                iid == __uuidof(IDXGIDevice4);
            if (m_video.Supports(iid))
            {
                *result = static_cast<ID3D11VideoDevice2*>(&m_video);
                AddRef();
                return S_OK;
            }
            if (m_on12.Supports(iid))
            {
                *result = static_cast<ID3D11On12Device2*>(&m_on12);
                AddRef();
                return S_OK;
            }
            if (!d3d && !dxgi) return m_native->QueryInterface(iid, result);

            ComPtr<IUnknown> supported;
            const HRESULT status = m_native->QueryInterface(iid, reinterpret_cast<void**>(supported.GetAddressOf()));
            if (FAILED(status)) return status;
            if (dxgi) *result = static_cast<IDXGIDevice4*>(&m_dxgi);
            else *result = static_cast<ID3D11Device5*>(this);
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

        IUnknown* STDMETHODCALLTYPE NativeObject() override { return m_native.Get(); }

        HRESULT STDMETHODCALLTYPE CreateBuffer(const D3D11_BUFFER_DESC* pDesc,
            const D3D11_SUBRESOURCE_DATA* pInitialData, ID3D11Buffer** ppBuffer) override
        { return CreateD3D11Child(this, ppBuffer, [&](auto output) { return m_native->CreateBuffer(pDesc, pInitialData, output); }); }

        HRESULT STDMETHODCALLTYPE CreateTexture1D(const D3D11_TEXTURE1D_DESC* pDesc,
            const D3D11_SUBRESOURCE_DATA* pInitialData, ID3D11Texture1D** ppTexture1D) override
        { return CreateD3D11Child(this, ppTexture1D, [&](auto output) { return m_native->CreateTexture1D(pDesc, pInitialData, output); }); }

        HRESULT STDMETHODCALLTYPE CreateTexture2D(const D3D11_TEXTURE2D_DESC* pDesc,
            const D3D11_SUBRESOURCE_DATA* pInitialData, ID3D11Texture2D** ppTexture2D) override
        { return CreateD3D11Child(this, ppTexture2D, [&](auto output) { return m_native->CreateTexture2D(pDesc, pInitialData, output); }); }

        HRESULT STDMETHODCALLTYPE CreateTexture3D(const D3D11_TEXTURE3D_DESC* pDesc,
            const D3D11_SUBRESOURCE_DATA* pInitialData, ID3D11Texture3D** ppTexture3D) override
        { return CreateD3D11Child(this, ppTexture3D, [&](auto output) { return m_native->CreateTexture3D(pDesc, pInitialData, output); }); }

        HRESULT STDMETHODCALLTYPE CreateShaderResourceView(ID3D11Resource* pResource,
            const D3D11_SHADER_RESOURCE_VIEW_DESC* pDesc, ID3D11ShaderResourceView** ppSRView) override
        { return CreateD3D11Child(this, ppSRView, [&](auto output)
            { return m_native->CreateShaderResourceView(NativeD3D11Child(pResource), pDesc, output); }); }

        HRESULT STDMETHODCALLTYPE CreateUnorderedAccessView(ID3D11Resource* pResource,
            const D3D11_UNORDERED_ACCESS_VIEW_DESC* pDesc, ID3D11UnorderedAccessView** ppUAView) override
        { return CreateD3D11Child(this, ppUAView, [&](auto output)
            { return m_native->CreateUnorderedAccessView(NativeD3D11Child(pResource), pDesc, output); }); }

        HRESULT STDMETHODCALLTYPE CreateRenderTargetView(ID3D11Resource* pResource,
            const D3D11_RENDER_TARGET_VIEW_DESC* pDesc, ID3D11RenderTargetView** ppRTView) override
        { return CreateD3D11Child(this, ppRTView, [&](auto output)
            { return m_native->CreateRenderTargetView(NativeD3D11Child(pResource), pDesc, output); }); }

        HRESULT STDMETHODCALLTYPE CreateDepthStencilView(ID3D11Resource* pResource,
            const D3D11_DEPTH_STENCIL_VIEW_DESC* pDesc, ID3D11DepthStencilView** ppDepthStencilView) override
        { return CreateD3D11Child(this, ppDepthStencilView, [&](auto output)
            { return m_native->CreateDepthStencilView(NativeD3D11Child(pResource), pDesc, output); }); }

        HRESULT STDMETHODCALLTYPE CreateInputLayout(const D3D11_INPUT_ELEMENT_DESC* pInputElementDescs, UINT NumElements,
            const void* pShaderBytecodeWithInputSignature, SIZE_T BytecodeLength, ID3D11InputLayout** ppInputLayout) override
        { return m_native->CreateInputLayout(pInputElementDescs, NumElements, pShaderBytecodeWithInputSignature, BytecodeLength, ppInputLayout); }

        HRESULT STDMETHODCALLTYPE CreateVertexShader(const void* pShaderBytecode, SIZE_T BytecodeLength,
            ID3D11ClassLinkage* pClassLinkage, ID3D11VertexShader** ppVertexShader) override
        { return m_native->CreateVertexShader(pShaderBytecode, BytecodeLength, pClassLinkage, ppVertexShader); }

        HRESULT STDMETHODCALLTYPE CreateGeometryShader(const void* pShaderBytecode, SIZE_T BytecodeLength,
            ID3D11ClassLinkage* pClassLinkage, ID3D11GeometryShader** ppGeometryShader) override
        { return m_native->CreateGeometryShader(pShaderBytecode, BytecodeLength, pClassLinkage, ppGeometryShader); }

        HRESULT STDMETHODCALLTYPE CreateGeometryShaderWithStreamOutput(const void* pShaderBytecode,
            SIZE_T BytecodeLength, const D3D11_SO_DECLARATION_ENTRY* pSODeclaration, UINT NumEntries,
            const UINT* pBufferStrides, UINT NumStrides, UINT RasterizedStream, ID3D11ClassLinkage* pClassLinkage,
            ID3D11GeometryShader** ppGeometryShader) override
        { return m_native->CreateGeometryShaderWithStreamOutput(pShaderBytecode, BytecodeLength, pSODeclaration, NumEntries, pBufferStrides, NumStrides, RasterizedStream, pClassLinkage, ppGeometryShader); }

        HRESULT STDMETHODCALLTYPE CreatePixelShader(const void* pShaderBytecode, SIZE_T BytecodeLength,
            ID3D11ClassLinkage* pClassLinkage, ID3D11PixelShader** ppPixelShader) override
        { return m_native->CreatePixelShader(pShaderBytecode, BytecodeLength, pClassLinkage, ppPixelShader); }

        HRESULT STDMETHODCALLTYPE CreateHullShader(const void* pShaderBytecode, SIZE_T BytecodeLength,
            ID3D11ClassLinkage* pClassLinkage, ID3D11HullShader** ppHullShader) override
        { return m_native->CreateHullShader(pShaderBytecode, BytecodeLength, pClassLinkage, ppHullShader); }

        HRESULT STDMETHODCALLTYPE CreateDomainShader(const void* pShaderBytecode, SIZE_T BytecodeLength,
            ID3D11ClassLinkage* pClassLinkage, ID3D11DomainShader** ppDomainShader) override
        { return m_native->CreateDomainShader(pShaderBytecode, BytecodeLength, pClassLinkage, ppDomainShader); }

        HRESULT STDMETHODCALLTYPE CreateComputeShader(const void* pShaderBytecode, SIZE_T BytecodeLength,
            ID3D11ClassLinkage* pClassLinkage, ID3D11ComputeShader** ppComputeShader) override
        { return m_native->CreateComputeShader(pShaderBytecode, BytecodeLength, pClassLinkage, ppComputeShader); }

        HRESULT STDMETHODCALLTYPE CreateClassLinkage(ID3D11ClassLinkage** ppLinkage) override
        { return m_native->CreateClassLinkage(ppLinkage); }

        HRESULT STDMETHODCALLTYPE CreateBlendState(const D3D11_BLEND_DESC* pBlendStateDesc,
            ID3D11BlendState** ppBlendState) override
        { return m_native->CreateBlendState(pBlendStateDesc, ppBlendState); }

        HRESULT STDMETHODCALLTYPE CreateDepthStencilState(const D3D11_DEPTH_STENCIL_DESC* pDepthStencilDesc,
            ID3D11DepthStencilState** ppDepthStencilState) override
        { return m_native->CreateDepthStencilState(pDepthStencilDesc, ppDepthStencilState); }

        HRESULT STDMETHODCALLTYPE CreateRasterizerState(const D3D11_RASTERIZER_DESC* pRasterizerDesc,
            ID3D11RasterizerState** ppRasterizerState) override
        { return m_native->CreateRasterizerState(pRasterizerDesc, ppRasterizerState); }

        HRESULT STDMETHODCALLTYPE CreateSamplerState(const D3D11_SAMPLER_DESC* pSamplerDesc,
            ID3D11SamplerState** ppSamplerState) override
        { return m_native->CreateSamplerState(pSamplerDesc, ppSamplerState); }

        HRESULT STDMETHODCALLTYPE CreateQuery(const D3D11_QUERY_DESC* pQueryDesc, ID3D11Query** ppQuery) override
        { return m_native->CreateQuery(pQueryDesc, ppQuery); }

        HRESULT STDMETHODCALLTYPE CreatePredicate(const D3D11_QUERY_DESC* pPredicateDesc, ID3D11Predicate** ppPredicate) override
        { return m_native->CreatePredicate(pPredicateDesc, ppPredicate); }

        HRESULT STDMETHODCALLTYPE CreateCounter(const D3D11_COUNTER_DESC* pCounterDesc, ID3D11Counter** ppCounter) override
        { return m_native->CreateCounter(pCounterDesc, ppCounter); }

        HRESULT STDMETHODCALLTYPE CreateDeferredContext(UINT ContextFlags, ID3D11DeviceContext** ppDeferredContext) override
        {
            if (!ppDeferredContext) return E_POINTER;
            *ppDeferredContext = nullptr;
            ComPtr<ID3D11DeviceContext> context;
            const HRESULT status = m_native->CreateDeferredContext(ContextFlags, &context);
            return FAILED(status) ? status : WrapD3D11Context(std::move(context), this, ppDeferredContext);
        }

        HRESULT STDMETHODCALLTYPE OpenSharedResource(HANDLE hResource, REFIID ReturnedInterface, void** ppResource) override
        { return OpenD3D11Resource(this, ReturnedInterface, ppResource, [&](void** output)
            { return m_native->OpenSharedResource(hResource, ReturnedInterface, output); }); }

        HRESULT STDMETHODCALLTYPE CheckFormatSupport(DXGI_FORMAT Format, UINT* pFormatSupport) override
        { return m_native->CheckFormatSupport(Format, pFormatSupport); }

        HRESULT STDMETHODCALLTYPE CheckMultisampleQualityLevels(DXGI_FORMAT Format, UINT SampleCount,
            UINT* pNumQualityLevels) override
        { return m_native->CheckMultisampleQualityLevels(Format, SampleCount, pNumQualityLevels); }

        void STDMETHODCALLTYPE CheckCounterInfo(D3D11_COUNTER_INFO* pCounterInfo) override
        { return m_native->CheckCounterInfo(pCounterInfo); }

        HRESULT STDMETHODCALLTYPE CheckCounter(const D3D11_COUNTER_DESC* pDesc, D3D11_COUNTER_TYPE* pType,
            UINT* pActiveCounters, LPSTR szName, UINT* pNameLength, LPSTR szUnits, UINT* pUnitsLength,
            LPSTR szDescription, UINT* pDescriptionLength) override
        { return m_native->CheckCounter(pDesc, pType, pActiveCounters, szName, pNameLength, szUnits, pUnitsLength, szDescription, pDescriptionLength); }

        HRESULT STDMETHODCALLTYPE CheckFeatureSupport(D3D11_FEATURE Feature, void* pFeatureSupportData,
            UINT FeatureSupportDataSize) override
        { return m_native->CheckFeatureSupport(Feature, pFeatureSupportData, FeatureSupportDataSize); }

        HRESULT STDMETHODCALLTYPE GetPrivateData(REFGUID guid, UINT* pDataSize, void* pData) override
        { return m_native->GetPrivateData(guid, pDataSize, pData); }

        HRESULT STDMETHODCALLTYPE SetPrivateData(REFGUID guid, UINT DataSize, const void* pData) override
        { return m_native->SetPrivateData(guid, DataSize, pData); }

        HRESULT STDMETHODCALLTYPE SetPrivateDataInterface(REFGUID guid, const IUnknown* pData) override
        { return m_native->SetPrivateDataInterface(guid, pData); }

        D3D_FEATURE_LEVEL STDMETHODCALLTYPE GetFeatureLevel() override
        { return m_native->GetFeatureLevel(); }

        UINT STDMETHODCALLTYPE GetCreationFlags() override
        { return m_native->GetCreationFlags(); }

        HRESULT STDMETHODCALLTYPE GetDeviceRemovedReason() override
        { return m_native->GetDeviceRemovedReason(); }

        void STDMETHODCALLTYPE GetImmediateContext(ID3D11DeviceContext** ppImmediateContext) override
        {
            if (!ppImmediateContext) return;
            *ppImmediateContext = nullptr;
            ComPtr<ID3D11DeviceContext> context;
            m_native->GetImmediateContext(&context);
            WrapD3D11Context(std::move(context), this, ppImmediateContext);
        }

        HRESULT STDMETHODCALLTYPE SetExceptionMode(UINT RaiseFlags) override
        { return m_native->SetExceptionMode(RaiseFlags); }

        UINT STDMETHODCALLTYPE GetExceptionMode() override
        { return m_native->GetExceptionMode(); }

        void STDMETHODCALLTYPE GetImmediateContext1(ID3D11DeviceContext1** ppImmediateContext) override
        {
            if (!ppImmediateContext) return;
            *ppImmediateContext = nullptr;
            ComPtr<ID3D11Device1> native;
            const HRESULT queried = m_native.As(&native);
            if (FAILED(queried)) return;
            ComPtr<ID3D11DeviceContext1> context;
            native->GetImmediateContext1(&context);
            WrapD3D11Context(std::move(context), this, ppImmediateContext);
        }

        HRESULT STDMETHODCALLTYPE CreateDeferredContext1(UINT ContextFlags, ID3D11DeviceContext1** ppDeferredContext) override
        {
            if (!ppDeferredContext) return E_POINTER;
            *ppDeferredContext = nullptr;
            ComPtr<ID3D11Device1> native;
            const HRESULT queried = m_native.As(&native);
            if (FAILED(queried)) return queried;
            ComPtr<ID3D11DeviceContext1> context;
            const HRESULT status = native->CreateDeferredContext1(ContextFlags, &context);
            return FAILED(status) ? status : WrapD3D11Context(std::move(context), this, ppDeferredContext);
        }

        HRESULT STDMETHODCALLTYPE CreateBlendState1(const D3D11_BLEND_DESC1* pBlendStateDesc,
            ID3D11BlendState1** ppBlendState) override
        {
            ComPtr<ID3D11Device1> native;
            const HRESULT status = m_native.As(&native);
            return FAILED(status) ? status : native->CreateBlendState1(pBlendStateDesc, ppBlendState);
        }

        HRESULT STDMETHODCALLTYPE CreateRasterizerState1(const D3D11_RASTERIZER_DESC1* pRasterizerDesc,
            ID3D11RasterizerState1** ppRasterizerState) override
        {
            ComPtr<ID3D11Device1> native;
            const HRESULT status = m_native.As(&native);
            return FAILED(status) ? status : native->CreateRasterizerState1(pRasterizerDesc, ppRasterizerState);
        }

        HRESULT STDMETHODCALLTYPE CreateDeviceContextState(UINT Flags, const D3D_FEATURE_LEVEL* pFeatureLevels,
            UINT FeatureLevels, UINT SDKVersion, REFIID EmulatedInterface, D3D_FEATURE_LEVEL* pChosenFeatureLevel,
            ID3DDeviceContextState** ppContextState) override
        {
            ComPtr<ID3D11Device1> native;
            const HRESULT status = m_native.As(&native);
            return FAILED(status) ? status : native->CreateDeviceContextState(Flags, pFeatureLevels, FeatureLevels, SDKVersion, EmulatedInterface, pChosenFeatureLevel, ppContextState);
        }

        HRESULT STDMETHODCALLTYPE OpenSharedResource1(HANDLE hResource, REFIID returnedInterface, void** ppResource) override
        {
            const auto kernel = CurrentGuestKernelContext();
            const auto handle = kernel ? kernel->LeaseGraphicsHandle(hResource) : std::shared_ptr<void>{};
            ComPtr<ID3D11Device1> native;
            const HRESULT status = m_native.As(&native);
            if (FAILED(status)) return status;
            return OpenD3D11Resource(this, returnedInterface, ppResource, [&](void** output)
            { return native->OpenSharedResource1(handle ? handle.get() : hResource, returnedInterface, output); });
        }

        HRESULT STDMETHODCALLTYPE OpenSharedResourceByName(LPCWSTR lpName, DWORD dwDesiredAccess,
            REFIID returnedInterface, void** ppResource) override
        {
            ComPtr<ID3D11Device1> native;
            const HRESULT status = m_native.As(&native);
            return FAILED(status) ? status : OpenD3D11Resource(this, returnedInterface, ppResource, [&](void** output)
            { return native->OpenSharedResourceByName(lpName, dwDesiredAccess, returnedInterface, output); });
        }

        void STDMETHODCALLTYPE GetImmediateContext2(ID3D11DeviceContext2** ppImmediateContext) override
        {
            if (!ppImmediateContext) return;
            *ppImmediateContext = nullptr;
            ComPtr<ID3D11Device2> native;
            const HRESULT queried = m_native.As(&native);
            if (FAILED(queried)) return;
            ComPtr<ID3D11DeviceContext2> context;
            native->GetImmediateContext2(&context);
            WrapD3D11Context(std::move(context), this, ppImmediateContext);
        }

        HRESULT STDMETHODCALLTYPE CreateDeferredContext2(UINT ContextFlags, ID3D11DeviceContext2** ppDeferredContext) override
        {
            if (!ppDeferredContext) return E_POINTER;
            *ppDeferredContext = nullptr;
            ComPtr<ID3D11Device2> native;
            const HRESULT queried = m_native.As(&native);
            if (FAILED(queried)) return queried;
            ComPtr<ID3D11DeviceContext2> context;
            const HRESULT status = native->CreateDeferredContext2(ContextFlags, &context);
            return FAILED(status) ? status : WrapD3D11Context(std::move(context), this, ppDeferredContext);
        }

        void STDMETHODCALLTYPE GetResourceTiling(ID3D11Resource* pTiledResource, UINT* pNumTilesForEntireResource,
            D3D11_PACKED_MIP_DESC* pPackedMipDesc, D3D11_TILE_SHAPE* pStandardTileShapeForNonPackedMips,
            UINT* pNumSubresourceTilings, UINT FirstSubresourceTilingToGet,
            D3D11_SUBRESOURCE_TILING* pSubresourceTilingsForNonPackedMips) override
        {
            ComPtr<ID3D11Device2> native;
            if (SUCCEEDED(m_native.As(&native))) native->GetResourceTiling(NativeD3D11Child(pTiledResource), pNumTilesForEntireResource, pPackedMipDesc, pStandardTileShapeForNonPackedMips, pNumSubresourceTilings, FirstSubresourceTilingToGet, pSubresourceTilingsForNonPackedMips);
        }

        HRESULT STDMETHODCALLTYPE CheckMultisampleQualityLevels1(DXGI_FORMAT Format, UINT SampleCount, UINT Flags,
            UINT* pNumQualityLevels) override
        {
            ComPtr<ID3D11Device2> native;
            const HRESULT status = m_native.As(&native);
            return FAILED(status) ? status : native->CheckMultisampleQualityLevels1(Format, SampleCount, Flags, pNumQualityLevels);
        }

        HRESULT STDMETHODCALLTYPE CreateTexture2D1(const D3D11_TEXTURE2D_DESC1* pDesc1,
            const D3D11_SUBRESOURCE_DATA* pInitialData, ID3D11Texture2D1** ppTexture2D) override
        {
            ComPtr<ID3D11Device3> native;
            const HRESULT status = m_native.As(&native);
            return FAILED(status) ? status : CreateD3D11Child(this, ppTexture2D, [&](auto output)
            { return native->CreateTexture2D1(pDesc1, pInitialData, output); });
        }

        HRESULT STDMETHODCALLTYPE CreateTexture3D1(const D3D11_TEXTURE3D_DESC1* pDesc1,
            const D3D11_SUBRESOURCE_DATA* pInitialData, ID3D11Texture3D1** ppTexture3D) override
        {
            ComPtr<ID3D11Device3> native;
            const HRESULT status = m_native.As(&native);
            return FAILED(status) ? status : CreateD3D11Child(this, ppTexture3D, [&](auto output)
            { return native->CreateTexture3D1(pDesc1, pInitialData, output); });
        }

        HRESULT STDMETHODCALLTYPE CreateRasterizerState2(const D3D11_RASTERIZER_DESC2* pRasterizerDesc,
            ID3D11RasterizerState2** ppRasterizerState) override
        {
            ComPtr<ID3D11Device3> native;
            const HRESULT status = m_native.As(&native);
            return FAILED(status) ? status : native->CreateRasterizerState2(pRasterizerDesc, ppRasterizerState);
        }

        HRESULT STDMETHODCALLTYPE CreateShaderResourceView1(ID3D11Resource* pResource,
            const D3D11_SHADER_RESOURCE_VIEW_DESC1* pDesc1, ID3D11ShaderResourceView1** ppSRView1) override
        {
            ComPtr<ID3D11Device3> native;
            const HRESULT status = m_native.As(&native);
            return FAILED(status) ? status : CreateD3D11Child(this, ppSRView1, [&](auto output)
            { return native->CreateShaderResourceView1(NativeD3D11Child(pResource), pDesc1, output); });
        }

        HRESULT STDMETHODCALLTYPE CreateUnorderedAccessView1(ID3D11Resource* pResource,
            const D3D11_UNORDERED_ACCESS_VIEW_DESC1* pDesc1, ID3D11UnorderedAccessView1** ppUAView1) override
        {
            ComPtr<ID3D11Device3> native;
            const HRESULT status = m_native.As(&native);
            return FAILED(status) ? status : CreateD3D11Child(this, ppUAView1, [&](auto output)
            { return native->CreateUnorderedAccessView1(NativeD3D11Child(pResource), pDesc1, output); });
        }

        HRESULT STDMETHODCALLTYPE CreateRenderTargetView1(ID3D11Resource* pResource,
            const D3D11_RENDER_TARGET_VIEW_DESC1* pDesc1, ID3D11RenderTargetView1** ppRTView1) override
        {
            ComPtr<ID3D11Device3> native;
            const HRESULT status = m_native.As(&native);
            return FAILED(status) ? status : CreateD3D11Child(this, ppRTView1, [&](auto output)
            { return native->CreateRenderTargetView1(NativeD3D11Child(pResource), pDesc1, output); });
        }

        HRESULT STDMETHODCALLTYPE CreateQuery1(const D3D11_QUERY_DESC1* pQueryDesc1, ID3D11Query1** ppQuery1) override
        {
            ComPtr<ID3D11Device3> native;
            const HRESULT status = m_native.As(&native);
            return FAILED(status) ? status : native->CreateQuery1(pQueryDesc1, ppQuery1);
        }

        void STDMETHODCALLTYPE GetImmediateContext3(ID3D11DeviceContext3** ppImmediateContext) override
        {
            if (!ppImmediateContext) return;
            *ppImmediateContext = nullptr;
            ComPtr<ID3D11Device3> native;
            const HRESULT queried = m_native.As(&native);
            if (FAILED(queried)) return;
            ComPtr<ID3D11DeviceContext3> context;
            native->GetImmediateContext3(&context);
            WrapD3D11Context(std::move(context), this, ppImmediateContext);
        }

        HRESULT STDMETHODCALLTYPE CreateDeferredContext3(UINT ContextFlags, ID3D11DeviceContext3** ppDeferredContext) override
        {
            if (!ppDeferredContext) return E_POINTER;
            *ppDeferredContext = nullptr;
            ComPtr<ID3D11Device3> native;
            const HRESULT queried = m_native.As(&native);
            if (FAILED(queried)) return queried;
            ComPtr<ID3D11DeviceContext3> context;
            const HRESULT status = native->CreateDeferredContext3(ContextFlags, &context);
            return FAILED(status) ? status : WrapD3D11Context(std::move(context), this, ppDeferredContext);
        }

        void STDMETHODCALLTYPE WriteToSubresource(ID3D11Resource* pDstResource, UINT DstSubresource,
            const D3D11_BOX* pDstBox, const void* pSrcData, UINT SrcRowPitch, UINT SrcDepthPitch) override
        {
            ComPtr<ID3D11Device3> native;
            if (SUCCEEDED(m_native.As(&native))) native->WriteToSubresource(NativeD3D11Child(pDstResource), DstSubresource, pDstBox, pSrcData, SrcRowPitch, SrcDepthPitch);
        }

        void STDMETHODCALLTYPE ReadFromSubresource(void* pDstData, UINT DstRowPitch, UINT DstDepthPitch,
            ID3D11Resource* pSrcResource, UINT SrcSubresource, const D3D11_BOX* pSrcBox) override
        {
            ComPtr<ID3D11Device3> native;
            if (SUCCEEDED(m_native.As(&native))) native->ReadFromSubresource(pDstData, DstRowPitch, DstDepthPitch, NativeD3D11Child(pSrcResource), SrcSubresource, pSrcBox);
        }

        HRESULT STDMETHODCALLTYPE RegisterDeviceRemovedEvent(HANDLE event, DWORD* cookie) override
        {
            ComPtr<ID3D11Device4> native;
            const HRESULT status = m_native.As(&native);
            return FAILED(status) ? status : m_removalEvents.Add(native, event, cookie,
                &ID3D11Device4::RegisterDeviceRemovedEvent, &ID3D11Device4::UnregisterDeviceRemoved);
        }

        void STDMETHODCALLTYPE UnregisterDeviceRemoved(DWORD cookie) override
        { m_removalEvents.Remove(cookie); }

        HRESULT STDMETHODCALLTYPE OpenSharedFence(HANDLE hFence, REFIID ReturnedInterface, void** ppFence) override
        {
            const auto kernel = CurrentGuestKernelContext();
            const auto handle = kernel ? kernel->LeaseGraphicsHandle(hFence) : std::shared_ptr<void>{};
            ComPtr<ID3D11Device5> native;
            const HRESULT status = m_native.As(&native);
            if (FAILED(status)) return status;
            return CreateD3D11Fence(this, ReturnedInterface, ppFence, [&](REFIID iid, void** output)
            {
                return native->OpenSharedFence(handle ? handle.get() : hFence, iid, output);
            });
        }

        HRESULT STDMETHODCALLTYPE CreateFence(UINT64 InitialValue, D3D11_FENCE_FLAG Flags, REFIID ReturnedInterface,
            void** ppFence) override
        {
            ComPtr<ID3D11Device5> native;
            const HRESULT status = m_native.As(&native);
            if (FAILED(status)) return status;
            return CreateD3D11Fence(this, ReturnedInterface, ppFence, [&](REFIID iid, void** output)
            {
                return native->CreateFence(InitialValue, Flags, iid, output);
            });
        }

    private:
        std::atomic<ULONG> m_refs{1};
        ComPtr<ID3D11Device> m_native;
        GuestDxgiDevice m_dxgi;
        GuestD3D11VideoDevice m_video;
        GuestD3D11On12Device m_on12;
        GraphicsEventNotifications m_removalEvents;
    };

    HRESULT WrapD3D11Device(ComPtr<ID3D11Device> native, IDXGIAdapter* requestedAdapter, ID3D11Device** output,
        IUnknown* device12)
    {
        ComPtr<IDXGIDevice> dxgi;
        HRESULT status = native.As(&dxgi);
        if (FAILED(status)) return status;
        ComPtr<IDXGIAdapter> guestAdapter;
        ComPtr<GuestGraphicsObject> proxy;
        if (requestedAdapter && SUCCEEDED(requestedAdapter->QueryInterface(IID_PPV_ARGS(&proxy))))
            guestAdapter = requestedAdapter;
        else
        {
            ComPtr<IDXGIAdapter> adapter;
            status = dxgi->GetAdapter(&adapter);
            if (FAILED(status)) return status;
            status = WrapAdapter(std::move(adapter), IID_PPV_ARGS(&guestAdapter));
            if (FAILED(status)) return status;
        }
        auto wrapper = new (std::nothrow) GuestD3D11Device(std::move(native), std::move(dxgi), std::move(guestAdapter), device12);
        if (!wrapper) return E_OUTOFMEMORY;
        *output = wrapper;
        return S_OK;
    }
