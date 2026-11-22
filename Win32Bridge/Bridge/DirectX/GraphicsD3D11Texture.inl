    ID3D11Resource* NativeD3D11Resource(ID3D11Resource* resource)
    {
        ComPtr<GuestGraphicsObject> proxy;
        if (resource && SUCCEEDED(resource->QueryInterface(IID_PPV_ARGS(&proxy))))
            return static_cast<ID3D11Resource*>(proxy->NativeObject());
        return resource;
    }

    class GuestD3D11Texture2D;
    using D3D11TextureKey = std::pair<ID3D11Device*, IUnknown*>;
    std::mutex g_d3d11TexturesLock;
    std::map<D3D11TextureKey, GuestD3D11Texture2D*> g_d3d11Textures;

    class GuestD3D11Texture2D final : public ID3D11Texture2D1, public GuestGraphicsObject
    {
        class DxgiResource final : public IDXGIResource1
        {
        public:
            explicit DxgiResource(GuestD3D11Texture2D* owner) : m_owner(owner) {}
            HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void** output) override
            { return m_owner->QueryInterface(iid, output); }
            ULONG STDMETHODCALLTYPE AddRef() override { return m_owner->AddRef(); }
            ULONG STDMETHODCALLTYPE Release() override { return m_owner->Release(); }
            HRESULT STDMETHODCALLTYPE GetDevice(REFIID iid, void** output) override
            { return m_owner->m_device->QueryInterface(iid, output); }
            HRESULT STDMETHODCALLTYPE GetParent(REFIID iid, void** output) override
            { return m_owner->m_dxgi->GetParent(iid, output); }
            HRESULT STDMETHODCALLTYPE SetPrivateData(REFGUID name, UINT size, const void* data) override
            { return m_owner->m_dxgi->SetPrivateData(name, size, data); }
            HRESULT STDMETHODCALLTYPE SetPrivateDataInterface(REFGUID name, const IUnknown* data) override
            { return m_owner->m_dxgi->SetPrivateDataInterface(name, data); }
            HRESULT STDMETHODCALLTYPE GetPrivateData(REFGUID name, UINT* size, void* data) override
            { return m_owner->m_dxgi->GetPrivateData(name, size, data); }
            HRESULT STDMETHODCALLTYPE GetSharedHandle(HANDLE* output) override
            { return m_owner->m_dxgi->GetSharedHandle(output); }
            HRESULT STDMETHODCALLTYPE GetUsage(DXGI_USAGE* usage) override
            { return m_owner->m_dxgi->GetUsage(usage); }
            HRESULT STDMETHODCALLTYPE SetEvictionPriority(UINT priority) override
            { return m_owner->m_dxgi->SetEvictionPriority(priority); }
            HRESULT STDMETHODCALLTYPE GetEvictionPriority(UINT* priority) override
            { return m_owner->m_dxgi->GetEvictionPriority(priority); }
            HRESULT STDMETHODCALLTYPE CreateSubresourceSurface(UINT index, IDXGISurface2** output) override
            {
                return m_owner->m_dxgi1 ? m_owner->m_dxgi1->CreateSubresourceSurface(index, output) : E_NOINTERFACE;
            }
            HRESULT STDMETHODCALLTYPE CreateSharedHandle(const SECURITY_ATTRIBUTES* attributes, DWORD access,
                LPCWSTR name, HANDLE* output) override
            {
                return CreateGraphicsShare(attributes, name, output, [&](HANDLE* handle)
                {
                    return m_owner->m_dxgi1 ? m_owner->m_dxgi1->CreateSharedHandle(nullptr, access, nullptr, handle)
                        : E_NOINTERFACE;
                });
            }

        private:
            GuestD3D11Texture2D* m_owner;
        };

    public:
        GuestD3D11Texture2D(ComPtr<ID3D11Texture2D> native, const D3D11TextureKey& key)
            : m_native(std::move(native)), m_device(key.first), m_resource(this), m_key(key)
        {
            m_native.As(&m_native1);
            m_native.As(&m_dxgi);
            m_native.As(&m_dxgi1);
        }

        HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void** output) override
        {
            if (!output) return E_POINTER;
            *output = nullptr;
            if (iid == __uuidof(GuestGraphicsObject)) *output = static_cast<GuestGraphicsObject*>(this);
            else if (iid == __uuidof(IUnknown) || iid == __uuidof(ID3D11DeviceChild) ||
                iid == __uuidof(ID3D11Resource) || iid == __uuidof(ID3D11Texture2D) ||
                (iid == __uuidof(ID3D11Texture2D1) && m_native1))
                *output = static_cast<ID3D11Texture2D1*>(this);
            else if (m_dxgi && (iid == __uuidof(IDXGIObject) || iid == __uuidof(IDXGIDeviceSubObject) ||
                iid == __uuidof(IDXGIResource) || (iid == __uuidof(IDXGIResource1) && m_dxgi1)))
                *output = static_cast<IDXGIResource1*>(&m_resource);
            else return m_native->QueryInterface(iid, output);
            AddRef();
            return S_OK;
        }

        ULONG STDMETHODCALLTYPE AddRef() override { return ++m_refs; }
        ULONG STDMETHODCALLTYPE Release() override
        {
            ULONG refs;
            {
                std::lock_guard<std::mutex> guard(g_d3d11TexturesLock);
                refs = --m_refs;
                if (!refs)
                {
                    const auto entry = g_d3d11Textures.find(m_key);
                    if (entry != g_d3d11Textures.end() && entry->second == this) g_d3d11Textures.erase(entry);
                }
            }
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
        void STDMETHODCALLTYPE GetType(D3D11_RESOURCE_DIMENSION* dimension) override
        { m_native->GetType(dimension); }
        void STDMETHODCALLTYPE SetEvictionPriority(UINT priority) override
        { m_native->SetEvictionPriority(priority); }
        UINT STDMETHODCALLTYPE GetEvictionPriority() override
        { return m_native->GetEvictionPriority(); }
        void STDMETHODCALLTYPE GetDesc(D3D11_TEXTURE2D_DESC* desc) override
        { m_native->GetDesc(desc); }
        void STDMETHODCALLTYPE GetDesc1(D3D11_TEXTURE2D_DESC1* desc) override
        { if (m_native1) m_native1->GetDesc1(desc); }

    private:
        ComPtr<ID3D11Texture2D> m_native;
        ComPtr<ID3D11Texture2D1> m_native1;
        ComPtr<ID3D11Device> m_device;
        ComPtr<IDXGIResource> m_dxgi;
        ComPtr<IDXGIResource1> m_dxgi1;
        DxgiResource m_resource;
        const D3D11TextureKey m_key;
        std::atomic<ULONG> m_refs{1};
    };

    HRESULT WrapD3D11Texture(ComPtr<ID3D11Texture2D> native, ID3D11Device* device, REFIID iid, void** output)
    {
        if (!output) return E_POINTER;
        *output = nullptr;
        if (!native) return E_UNEXPECTED;
        ComPtr<IUnknown> identity;
        const HRESULT status = native.As(&identity);
        if (FAILED(status)) return status;
        const D3D11TextureKey key{device, identity.Get()};
        ComPtr<ID3D11Texture2D> texture;
        {
            std::lock_guard<std::mutex> guard(g_d3d11TexturesLock);
            const auto found = g_d3d11Textures.find(key);
            if (found != g_d3d11Textures.end()) texture = found->second;
            else
            {
                auto wrapper = new (std::nothrow) GuestD3D11Texture2D(std::move(native), key);
                if (!wrapper) return E_OUTOFMEMORY;
                texture.Attach(wrapper);
                try { g_d3d11Textures.emplace(key, wrapper); }
                catch (const std::bad_alloc&) { return E_OUTOFMEMORY; }
            }
        }
        return texture->QueryInterface(iid, output);
    }
