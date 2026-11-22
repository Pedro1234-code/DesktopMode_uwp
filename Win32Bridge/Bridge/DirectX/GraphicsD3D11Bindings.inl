    template<class Interface>
    Interface* NativeD3D11Child(Interface* object)
    {
        ComPtr<GuestGraphicsObject> proxy;
        if (object && SUCCEEDED(object->QueryInterface(IID_PPV_ARGS(&proxy))))
            return static_cast<Interface*>(proxy->NativeObject());
        return object;
    }

    using D3D11BindingKey = std::pair<ID3D11Device*, IUnknown*>;
    std::mutex g_d3d11BindingsLock;
    std::map<D3D11BindingKey, GuestGraphicsObject*> g_d3d11Bindings;

    // A guest reference keeps the native object and guest device alive. The cache
    // only observes wrappers, allowing native bindings to release resources normally.
    template<class Interface, class Native, class Family>
    class GuestD3D11Binding : public Interface, public GuestGraphicsObject
    {
    public:
        using NativeInterface = Native;

        GuestD3D11Binding(ComPtr<Native> native, const D3D11BindingKey& key)
            : m_native(std::move(native)), m_device(key.first), m_key(key)
        {
            if constexpr (!std::is_same_v<Interface, Native>) m_native.As(&m_extended);
        }

        HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void** output) override
        {
            if (!output) return E_POINTER;
            *output = nullptr;
            if (iid == __uuidof(GuestGraphicsObject)) *output = static_cast<GuestGraphicsObject*>(this);
            else if (iid == __uuidof(IUnknown) || iid == __uuidof(ID3D11DeviceChild) || iid == __uuidof(Family) ||
                iid == __uuidof(Native) || (iid == __uuidof(Interface) && m_extended))
                *output = static_cast<Interface*>(this);
            else return m_native->QueryInterface(iid, output);
            AddRef();
            return S_OK;
        }

        ULONG STDMETHODCALLTYPE AddRef() override { return ++m_refs; }
        ULONG STDMETHODCALLTYPE Release() override
        {
            ULONG refs;
            {
                std::lock_guard<std::mutex> guard(g_d3d11BindingsLock);
                refs = --m_refs;
                if (!refs)
                {
                    const auto found = g_d3d11Bindings.find(m_key);
                    if (found != g_d3d11Bindings.end() && found->second == this) g_d3d11Bindings.erase(found);
                }
            }
            if (!refs) delete this;
            return refs;
        }

        IUnknown* STDMETHODCALLTYPE NativeObject() override { return m_native.Get(); }
        void STDMETHODCALLTYPE GetDevice(ID3D11Device** output) override { if (output) m_device.CopyTo(output); }
        HRESULT STDMETHODCALLTYPE GetPrivateData(REFGUID key, UINT* size, void* data) override
        { return m_native->GetPrivateData(key, size, data); }
        HRESULT STDMETHODCALLTYPE SetPrivateData(REFGUID key, UINT size, const void* data) override
        { return m_native->SetPrivateData(key, size, data); }
        HRESULT STDMETHODCALLTYPE SetPrivateDataInterface(REFGUID key, const IUnknown* data) override
        { return m_native->SetPrivateDataInterface(key, data); }

    protected:
        virtual ~GuestD3D11Binding() = default;
        ComPtr<Native> m_native;
        ComPtr<Interface> m_extended;
        ComPtr<ID3D11Device> m_device;

    private:
        const D3D11BindingKey m_key;
        std::atomic<ULONG> m_refs{1};
    };

    template<class Wrapper>
    HRESULT WrapD3D11Binding(ComPtr<typename Wrapper::NativeInterface> native, ID3D11Device* device,
        REFIID iid, void** output)
    {
        if (!output) return E_POINTER;
        *output = nullptr;
        if (!native) return E_UNEXPECTED;
        ComPtr<IUnknown> identity;
        const HRESULT status = native.As(&identity);
        if (FAILED(status)) return status;
        const D3D11BindingKey key{device, identity.Get()};
        ComPtr<GuestGraphicsObject> wrapper;
        {
            std::lock_guard<std::mutex> guard(g_d3d11BindingsLock);
            const auto found = g_d3d11Bindings.find(key);
            if (found != g_d3d11Bindings.end()) wrapper = found->second;
        }
        if (!wrapper)
        {
            // Native QueryInterface can call into the driver. Create the candidate outside the cache lock.
            ComPtr<GuestGraphicsObject> candidate;
            candidate.Attach(new (std::nothrow) Wrapper(std::move(native), key));
            if (!candidate) return E_OUTOFMEMORY;
            try
            {
                std::lock_guard<std::mutex> guard(g_d3d11BindingsLock);
                const auto entry = g_d3d11Bindings.emplace(key, candidate.Get());
                wrapper = entry.first->second;
            }
            catch (const std::bad_alloc&) { return E_OUTOFMEMORY; }
        }
        return wrapper->QueryInterface(iid, output);
    }

    template<class Interface, class Native, class Desc, class Desc1 = void>
    class GuestD3D11ResourceBinding final : public GuestD3D11Binding<Interface, Native, ID3D11Resource>
    {
    public:
        using GuestD3D11Binding<Interface, Native, ID3D11Resource>::GuestD3D11Binding;
        void STDMETHODCALLTYPE GetType(D3D11_RESOURCE_DIMENSION* dimension) override
        { this->m_native->GetType(dimension); }
        void STDMETHODCALLTYPE SetEvictionPriority(UINT priority) override
        { this->m_native->SetEvictionPriority(priority); }
        UINT STDMETHODCALLTYPE GetEvictionPriority() override { return this->m_native->GetEvictionPriority(); }
        void STDMETHODCALLTYPE GetDesc(Desc* desc) override { this->m_native->GetDesc(desc); }
        void STDMETHODCALLTYPE GetDesc1(Desc1* desc)
        { if constexpr (!std::is_void_v<Desc1>) this->m_extended->GetDesc1(desc); }
    };

    using GuestD3D11Buffer = GuestD3D11ResourceBinding<ID3D11Buffer, ID3D11Buffer, D3D11_BUFFER_DESC>;
    using GuestD3D11Texture1D = GuestD3D11ResourceBinding<ID3D11Texture1D, ID3D11Texture1D, D3D11_TEXTURE1D_DESC>;
    using GuestD3D11Texture3D = GuestD3D11ResourceBinding<ID3D11Texture3D1, ID3D11Texture3D,
        D3D11_TEXTURE3D_DESC, D3D11_TEXTURE3D_DESC1>;

    template<class Wrapper>
    HRESULT WrapTypedD3D11Resource(ID3D11Resource* resource, ID3D11Device* device, REFIID iid, void** output)
    {
        ComPtr<typename Wrapper::NativeInterface> typed;
        const HRESULT status = resource->QueryInterface(IID_PPV_ARGS(&typed));
        return FAILED(status) ? status : WrapD3D11Binding<Wrapper>(std::move(typed), device, iid, output);
    }

    HRESULT WrapD3D11Resource(ID3D11Resource* resource, ID3D11Device* device, REFIID iid, void** output)
    {
        D3D11_RESOURCE_DIMENSION dimension = D3D11_RESOURCE_DIMENSION_UNKNOWN;
        resource->GetType(&dimension);
        switch (dimension)
        {
        case D3D11_RESOURCE_DIMENSION_BUFFER:
            return WrapTypedD3D11Resource<GuestD3D11Buffer>(resource, device, iid, output);
        case D3D11_RESOURCE_DIMENSION_TEXTURE1D:
            return WrapTypedD3D11Resource<GuestD3D11Texture1D>(resource, device, iid, output);
        case D3D11_RESOURCE_DIMENSION_TEXTURE2D:
        {
            ComPtr<ID3D11Texture2D> texture;
            const HRESULT status = resource->QueryInterface(IID_PPV_ARGS(&texture));
            return FAILED(status) ? status : WrapD3D11Texture(std::move(texture), device, iid, output);
        }
        case D3D11_RESOURCE_DIMENSION_TEXTURE3D:
            return WrapTypedD3D11Resource<GuestD3D11Texture3D>(resource, device, iid, output);
        default:
            return resource->QueryInterface(iid, output);
        }
    }

    template<class Interface, class Native, class Desc, class Desc1 = void>
    class GuestD3D11ViewBinding final : public GuestD3D11Binding<Interface, Native, ID3D11View>
    {
    public:
        using GuestD3D11Binding<Interface, Native, ID3D11View>::GuestD3D11Binding;
        void STDMETHODCALLTYPE GetDesc(Desc* desc) override { this->m_native->GetDesc(desc); }
        void STDMETHODCALLTYPE GetDesc1(Desc1* desc)
        { if constexpr (!std::is_void_v<Desc1>) this->m_extended->GetDesc1(desc); }

        void STDMETHODCALLTYPE GetResource(ID3D11Resource** output) override
        {
            if (!output) return;
            ComPtr<ID3D11Resource> resource;
            this->m_native->GetResource(resource.GetAddressOf());
            *output = nullptr;
            if (resource && FAILED(WrapD3D11Resource(resource.Get(), this->m_device.Get(), IID_PPV_ARGS(output))))
                *output = resource.Detach();
        }
    };

    using GuestD3D11ShaderView = GuestD3D11ViewBinding<ID3D11ShaderResourceView1, ID3D11ShaderResourceView,
        D3D11_SHADER_RESOURCE_VIEW_DESC, D3D11_SHADER_RESOURCE_VIEW_DESC1>;
    using GuestD3D11RenderView = GuestD3D11ViewBinding<ID3D11RenderTargetView1, ID3D11RenderTargetView,
        D3D11_RENDER_TARGET_VIEW_DESC, D3D11_RENDER_TARGET_VIEW_DESC1>;
    using GuestD3D11AccessView = GuestD3D11ViewBinding<ID3D11UnorderedAccessView1, ID3D11UnorderedAccessView,
        D3D11_UNORDERED_ACCESS_VIEW_DESC, D3D11_UNORDERED_ACCESS_VIEW_DESC1>;
    using GuestD3D11DepthView = GuestD3D11ViewBinding<ID3D11DepthStencilView, ID3D11DepthStencilView,
        D3D11_DEPTH_STENCIL_VIEW_DESC>;

    template<class Interface>
    HRESULT WrapD3D11Child(Interface* native, ID3D11Device* device, Interface** output)
    {
        if constexpr (std::is_base_of_v<ID3D11Resource, Interface>)
            return WrapD3D11Resource(native, device, IID_PPV_ARGS(output));
        else if constexpr (std::is_base_of_v<ID3D11ShaderResourceView, Interface>)
            return WrapD3D11Binding<GuestD3D11ShaderView>(ComPtr<ID3D11ShaderResourceView>(native), device, IID_PPV_ARGS(output));
        else if constexpr (std::is_base_of_v<ID3D11RenderTargetView, Interface>)
            return WrapD3D11Binding<GuestD3D11RenderView>(ComPtr<ID3D11RenderTargetView>(native), device, IID_PPV_ARGS(output));
        else if constexpr (std::is_base_of_v<ID3D11UnorderedAccessView, Interface>)
            return WrapD3D11Binding<GuestD3D11AccessView>(ComPtr<ID3D11UnorderedAccessView>(native), device, IID_PPV_ARGS(output));
        else
            return WrapD3D11Binding<GuestD3D11DepthView>(ComPtr<ID3D11DepthStencilView>(native), device, IID_PPV_ARGS(output));
    }

    template<class Interface, class Create>
    HRESULT CreateD3D11Child(ID3D11Device* device, Interface** output, const Create& create)
    {
        if (!output) return create(nullptr);
        *output = nullptr;
        ComPtr<Interface> native;
        const HRESULT status = create(native.GetAddressOf());
        if (FAILED(status) || !native) return status;
        const HRESULT wrapped = WrapD3D11Child(native.Get(), device, output);
        return FAILED(wrapped) ? wrapped : status;
    }

    template<class Open>
    HRESULT OpenD3D11Resource(ID3D11Device* device, REFIID iid, void** output, const Open& open)
    {
        if (!output) return open(nullptr);
        *output = nullptr;
        ComPtr<IUnknown> native;
        const HRESULT status = open(reinterpret_cast<void**>(native.GetAddressOf()));
        if (FAILED(status) || !native) return status;
        ComPtr<ID3D11Resource> resource;
        if (SUCCEEDED(native.As(&resource)))
        {
            const HRESULT wrapped = WrapD3D11Resource(resource.Get(), device, iid, output);
            return FAILED(wrapped) ? wrapped : status;
        }
        *output = native.Detach();
        return status;
    }

    template<class Interface, class Set>
    void SetD3D11Bindings(UINT count, Interface* const* bindings, const Set& set)
    {
        if (count > D3D11_COMMONSHADER_INPUT_RESOURCE_SLOT_COUNT) return;
        Interface* native[D3D11_COMMONSHADER_INPUT_RESOURCE_SLOT_COUNT];
        if (bindings)
            for (UINT i = 0; i < count; ++i) native[i] = NativeD3D11Child(bindings[i]);
        set(bindings ? native : nullptr);
    }

    template<class Interface>
    class D3D11BindingOutputs final
    {
    public:
        D3D11BindingOutputs(UINT count, Interface** outputs) : m_count(count), m_outputs(outputs) {}
        bool Valid() const { return m_count <= D3D11_COMMONSHADER_INPUT_RESOURCE_SLOT_COUNT; }
        Interface** NativeOutputs() { return m_outputs ? m_native : nullptr; }

        void Publish(ID3D11Device* device)
        {
            if (!m_outputs) return;
            for (UINT i = 0; i < m_count; ++i)
            {
                ComPtr<Interface> native;
                native.Attach(m_native[i]);
                m_outputs[i] = nullptr;
                if (native && FAILED(WrapD3D11Child(native.Get(), device, &m_outputs[i])))
                    m_outputs[i] = native.Detach();
            }
        }

    private:
        UINT m_count;
        Interface** m_outputs;
        Interface* m_native[D3D11_COMMONSHADER_INPUT_RESOURCE_SLOT_COUNT]{};
    };

    template<class Interface, class Get>
    void GetD3D11Bindings(ID3D11Device* device, UINT count, Interface** bindings, const Get& get)
    {
        D3D11BindingOutputs<Interface> outputs(count, bindings);
        if (!outputs.Valid()) return;
        get(outputs.NativeOutputs());
        outputs.Publish(device);
    }
