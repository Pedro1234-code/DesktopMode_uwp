    template<class Interface, class Native = Interface>
    class GuestD3D11DxgiChild : public Interface
    {
    public:
        GuestD3D11DxgiChild(IUnknown* owner, ID3D11Device* device) : m_owner(owner), m_device(device) {}

        HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void** output) override
        { return m_owner->QueryInterface(iid, output); }
        ULONG STDMETHODCALLTYPE AddRef() override { return m_owner->AddRef(); }
        ULONG STDMETHODCALLTYPE Release() override { return m_owner->Release(); }
        HRESULT STDMETHODCALLTYPE GetDevice(REFIID iid, void** output) override
        { return m_device->QueryInterface(iid, output); }
        HRESULT STDMETHODCALLTYPE GetParent(REFIID iid, void** output) override
        { return m_device->QueryInterface(iid, output); }
        HRESULT STDMETHODCALLTYPE SetPrivateData(REFGUID name, UINT size, const void* data) override
        { return m_native->SetPrivateData(name, size, data); }
        HRESULT STDMETHODCALLTYPE SetPrivateDataInterface(REFGUID name, const IUnknown* data) override
        { return m_native->SetPrivateDataInterface(name, data); }
        HRESULT STDMETHODCALLTYPE GetPrivateData(REFGUID name, UINT* size, void* data) override
        { return m_native->GetPrivateData(name, size, data); }

    protected:
        ComPtr<Native> m_native;

    private:
        IUnknown* m_owner;
        ID3D11Device* m_device;
    };

    class GuestD3D11DxgiSurface : public GuestD3D11DxgiChild<IDXGISurface2, IDXGISurface>
    {
    public:
        GuestD3D11DxgiSurface(IUnknown* owner, ID3D11Device* device, IUnknown* resource, UINT index)
            : GuestD3D11DxgiChild(owner, device), m_resource(resource), m_index(index) {}

        void SetNative(ComPtr<IDXGISurface> surface)
        {
            m_native = std::move(surface);
            if (m_native)
            {
                m_native.As(&m_surface1);
                m_native.As(&m_surface2);
            }
        }

        bool Supports(REFIID iid) const
        {
            return (iid == __uuidof(IDXGISurface) && m_native) ||
                (iid == __uuidof(IDXGISurface1) && m_surface1) || (iid == __uuidof(IDXGISurface2) && m_surface2);
        }

        HRESULT STDMETHODCALLTYPE GetDesc(DXGI_SURFACE_DESC* desc) override { return m_native->GetDesc(desc); }
        HRESULT STDMETHODCALLTYPE Map(DXGI_MAPPED_RECT* rect, UINT flags) override { return m_native->Map(rect, flags); }
        HRESULT STDMETHODCALLTYPE Unmap() override { return m_native->Unmap(); }
        HRESULT STDMETHODCALLTYPE GetDC(BOOL discard, HDC* dc) override { return m_surface1->GetDC(discard, dc); }
        HRESULT STDMETHODCALLTYPE ReleaseDC(RECT* dirty) override { return m_surface1->ReleaseDC(dirty); }
        HRESULT STDMETHODCALLTYPE GetResource(REFIID iid, void** output, UINT* index) override
        {
            if (!output) return E_POINTER;
            *output = nullptr;
            if (!index) return E_POINTER;
            const HRESULT result = m_resource->QueryInterface(iid, output);
            if (SUCCEEDED(result)) *index = m_index;
            return result;
        }

    private:
        ComPtr<IDXGISurface1> m_surface1;
        ComPtr<IDXGISurface2> m_surface2;
        IUnknown* m_resource;
        const UINT m_index;
    };

    // CreateSubresourceSurface creates its own COM object, even for subresource zero.
    // Keep the guest parent alive so GetResource always returns that same object.
    class GuestD3D11SubresourceSurface final : public GuestD3D11DxgiSurface, public GuestGraphicsObject
    {
    public:
        GuestD3D11SubresourceSurface(ComPtr<IDXGISurface2> native, ID3D11Device* device, IUnknown* resource, UINT index)
            : GuestD3D11DxgiSurface(nullptr, device, resource, index), m_parent(resource)
        { SetNative(std::move(native)); }

        HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void** output) override
        {
            if (!output) return E_POINTER;
            *output = nullptr;
            if (iid == __uuidof(GuestGraphicsObject)) *output = static_cast<GuestGraphicsObject*>(this);
            else if (iid == __uuidof(IUnknown) || iid == __uuidof(IDXGIObject) ||
                iid == __uuidof(IDXGIDeviceSubObject) || Supports(iid)) *output = static_cast<IDXGISurface2*>(this);
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

    private:
        ComPtr<IUnknown> m_parent;
        std::atomic<ULONG> m_refs{1};
    };

    class GuestD3D11DxgiKeyedMutex final : public GuestD3D11DxgiChild<IDXGIKeyedMutex>
    {
    public:
        using GuestD3D11DxgiChild::GuestD3D11DxgiChild;

        void SetNative(ComPtr<IDXGIKeyedMutex> mutex) { m_native = std::move(mutex); }
        bool Available() const { return m_native.Get() != nullptr; }
        HRESULT STDMETHODCALLTYPE AcquireSync(UINT64 key, DWORD milliseconds) override
        { return m_native->AcquireSync(key, milliseconds); }
        HRESULT STDMETHODCALLTYPE ReleaseSync(UINT64 key) override { return m_native->ReleaseSync(key); }
    };

    class GuestD3D11Texture2D final : public GuestD3D11Binding<ID3D11Texture2D1, ID3D11Texture2D, ID3D11Resource>
    {
        using Binding = GuestD3D11Binding<ID3D11Texture2D1, ID3D11Texture2D, ID3D11Resource>;

        class DxgiResource final : public GuestD3D11DxgiChild<IDXGIResource1, IDXGIResource>
        {
        public:
            DxgiResource(GuestD3D11Texture2D* owner, ID3D11Device* device)
                : GuestD3D11DxgiChild(static_cast<ID3D11Texture2D*>(owner), device), m_owner(owner) {}

            void SetNative(ID3D11Texture2D* texture)
            {
                texture->QueryInterface(IID_PPV_ARGS(&m_native));
                if (m_native) m_native.As(&m_resource1);
            }

            bool Supports(REFIID iid) const
            {
                return m_native && (iid == __uuidof(IDXGIObject) || iid == __uuidof(IDXGIDeviceSubObject) ||
                    iid == __uuidof(IDXGIResource) || (iid == __uuidof(IDXGIResource1) && m_resource1));
            }

            HRESULT STDMETHODCALLTYPE GetSharedHandle(HANDLE* output) override
            { return m_native->GetSharedHandle(output); }
            HRESULT STDMETHODCALLTYPE GetUsage(DXGI_USAGE* usage) override
            { return m_native->GetUsage(usage); }
            HRESULT STDMETHODCALLTYPE SetEvictionPriority(UINT priority) override
            { return m_native->SetEvictionPriority(priority); }
            HRESULT STDMETHODCALLTYPE GetEvictionPriority(UINT* priority) override
            { return m_native->GetEvictionPriority(priority); }
            HRESULT STDMETHODCALLTYPE CreateSubresourceSurface(UINT index, IDXGISurface2** output) override
            {
                if (!output) return E_POINTER;
                *output = nullptr;
                ComPtr<IDXGISurface2> surface;
                const HRESULT result = m_resource1->CreateSubresourceSurface(index, &surface);
                if (FAILED(result) || !surface) return result;
                auto wrapper = new (std::nothrow) GuestD3D11SubresourceSurface(std::move(surface), m_owner->m_device.Get(),
                    static_cast<ID3D11Texture2D*>(m_owner), index);
                if (!wrapper) return E_OUTOFMEMORY;
                *output = wrapper;
                return result;
            }
            HRESULT STDMETHODCALLTYPE CreateSharedHandle(const SECURITY_ATTRIBUTES* attributes, DWORD access,
                LPCWSTR name, HANDLE* output) override
            {
                return CreateGraphicsShare(attributes, name, output, [&](HANDLE* handle)
                {
                    return m_resource1->CreateSharedHandle(nullptr, access, nullptr, handle);
                });
            }

        private:
            GuestD3D11Texture2D* m_owner;
            ComPtr<IDXGIResource1> m_resource1;
        };

    public:
        GuestD3D11Texture2D(ComPtr<ID3D11Texture2D> native, const D3D11BindingKey& key)
            : Binding(std::move(native), key), m_resource(this, key.first),
              m_surface(static_cast<ID3D11Texture2D*>(this), key.first, static_cast<ID3D11Texture2D*>(this), 0),
              m_keyedMutex(static_cast<ID3D11Texture2D*>(this), key.first)
        {
            m_resource.SetNative(m_native.Get());
            ComPtr<IDXGISurface> surface;
            m_native.As(&surface);
            m_surface.SetNative(std::move(surface));
            ComPtr<IDXGIKeyedMutex> mutex;
            m_native.As(&mutex);
            m_keyedMutex.SetNative(std::move(mutex));
        }

        HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void** output) override
        {
            if (!output) return E_POINTER;
            if (m_resource.Supports(iid)) *output = static_cast<IDXGIResource1*>(&m_resource);
            else if (m_surface.Supports(iid)) *output = static_cast<IDXGISurface2*>(&m_surface);
            else if (iid == __uuidof(IDXGIKeyedMutex) && m_keyedMutex.Available())
                *output = static_cast<IDXGIKeyedMutex*>(&m_keyedMutex);
            else return Binding::QueryInterface(iid, output);
            AddRef();
            return S_OK;
        }
        void STDMETHODCALLTYPE GetType(D3D11_RESOURCE_DIMENSION* dimension) override
        { m_native->GetType(dimension); }
        void STDMETHODCALLTYPE SetEvictionPriority(UINT priority) override
        { m_native->SetEvictionPriority(priority); }
        UINT STDMETHODCALLTYPE GetEvictionPriority() override
        { return m_native->GetEvictionPriority(); }
        void STDMETHODCALLTYPE GetDesc(D3D11_TEXTURE2D_DESC* desc) override
        { m_native->GetDesc(desc); }
        void STDMETHODCALLTYPE GetDesc1(D3D11_TEXTURE2D_DESC1* desc) override
        { m_extended->GetDesc1(desc); }

    private:
        DxgiResource m_resource;
        GuestD3D11DxgiSurface m_surface;
        GuestD3D11DxgiKeyedMutex m_keyedMutex;
    };

    HRESULT WrapD3D11Texture(ComPtr<ID3D11Texture2D> native, ID3D11Device* device, REFIID iid, void** output)
    {
        return WrapD3D11Binding<GuestD3D11Texture2D>(std::move(native), device, iid, output);
    }
