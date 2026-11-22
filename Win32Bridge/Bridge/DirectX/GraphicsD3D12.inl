    template<class Interface, class Native = Interface>
    class GuestD3D12Child : public Interface, public GuestGraphicsObject
    {
    public:
        using NativeInterface = Native;

        GuestD3D12Child(ComPtr<Native> native, ID3D12Device* device)
            : m_native(std::move(native)), m_device(device) {}

        ULONG STDMETHODCALLTYPE AddRef() override { return ++m_refs; }
        ULONG STDMETHODCALLTYPE Release() override
        {
            const ULONG refs = --m_refs;
            if (!refs) delete this;
            return refs;
        }

        IUnknown* STDMETHODCALLTYPE NativeObject() override { return m_native.Get(); }
        HRESULT STDMETHODCALLTYPE GetDevice(REFIID iid, void** output) override
        { return m_device->QueryInterface(iid, output); }

        HRESULT STDMETHODCALLTYPE GetPrivateData(REFGUID guid, UINT* size, void* data) override
        { return m_native->GetPrivateData(guid, size, data); }
        HRESULT STDMETHODCALLTYPE SetPrivateData(REFGUID guid, UINT size, const void* data) override
        { return m_native->SetPrivateData(guid, size, data); }
        HRESULT STDMETHODCALLTYPE SetPrivateDataInterface(REFGUID guid, const IUnknown* data) override
        { return m_native->SetPrivateDataInterface(guid, data); }
        HRESULT STDMETHODCALLTYPE SetName(LPCWSTR name) override
        { return m_native->SetName(name); }

        static bool IsChildInterface(REFIID iid)
        {
            return iid == __uuidof(IUnknown) || iid == __uuidof(ID3D12Object) ||
                iid == __uuidof(ID3D12DeviceChild) || iid == __uuidof(ID3D12Pageable);
        }

    protected:
        virtual ~GuestD3D12Child() = default;

        HRESULT QueryChildInterface(REFIID iid, void** output, bool childInterface)
        {
            if (!output) return E_POINTER;
            *output = nullptr;
            if (iid == __uuidof(GuestGraphicsObject))
                *output = static_cast<GuestGraphicsObject*>(this);
            else if (childInterface)
            {
                ComPtr<IUnknown> supported;
                const HRESULT status = m_native->QueryInterface(iid, reinterpret_cast<void**>(supported.GetAddressOf()));
                if (FAILED(status)) return status;
                *output = static_cast<Interface*>(this);
            }
            else return m_native->QueryInterface(iid, output);
            AddRef();
            return S_OK;
        }

        ComPtr<Native> m_native;

    private:
        std::atomic<ULONG> m_refs{1};
        ComPtr<ID3D12Device> m_device;
    };

    class GuestD3D12Fence final : public GuestD3D12Child<ID3D12Fence1, ID3D12Fence>
    {
    public:
        using GuestD3D12Child::GuestD3D12Child;
        static bool IsFenceInterface(REFIID iid)
        { return iid == __uuidof(ID3D12Fence) || iid == __uuidof(ID3D12Fence1); }

        HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void** output) override
        { return QueryChildInterface(iid, output, IsChildInterface(iid) || IsFenceInterface(iid)); }

        UINT64 STDMETHODCALLTYPE GetCompletedValue() override { return m_native->GetCompletedValue(); }
        HRESULT STDMETHODCALLTYPE Signal(UINT64 value) override { return m_native->Signal(value); }
        HRESULT STDMETHODCALLTYPE SetEventOnCompletion(UINT64 value, HANDLE event) override
        {
            return RegisterGraphicsEvent(event, [&](HANDLE native)
            {
                return m_native->SetEventOnCompletion(value, native);
            });
        }
        D3D12_FENCE_FLAGS STDMETHODCALLTYPE GetCreationFlags() override
        {
            ComPtr<ID3D12Fence1> native;
            return SUCCEEDED(m_native.As(&native)) ? native->GetCreationFlags() : D3D12_FENCE_FLAG_NONE;
        }
    };

    class GuestD3D12Queue final : public GuestD3D12Child<ID3D12CommandQueue>
    {
    public:
        using GuestD3D12Child::GuestD3D12Child;
        HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void** output) override
        { return QueryChildInterface(iid, output, IsChildInterface(iid) || iid == __uuidof(ID3D12CommandQueue)); }
        void STDMETHODCALLTYPE UpdateTileMappings(ID3D12Resource* pResource, UINT NumResourceRegions,
            const D3D12_TILED_RESOURCE_COORDINATE* pResourceRegionStartCoordinates,
            const D3D12_TILE_REGION_SIZE* pResourceRegionSizes, ID3D12Heap* pHeap, UINT NumRanges,
            const D3D12_TILE_RANGE_FLAGS* pRangeFlags, const UINT* pHeapRangeStartOffsets,
            const UINT* pRangeTileCounts, D3D12_TILE_MAPPING_FLAGS Flags) override
        { m_native->UpdateTileMappings(pResource, NumResourceRegions, pResourceRegionStartCoordinates, pResourceRegionSizes, pHeap, NumRanges, pRangeFlags, pHeapRangeStartOffsets, pRangeTileCounts, Flags); }

        void STDMETHODCALLTYPE CopyTileMappings(ID3D12Resource* pDstResource,
            const D3D12_TILED_RESOURCE_COORDINATE* pDstRegionStartCoordinate, ID3D12Resource* pSrcResource,
            const D3D12_TILED_RESOURCE_COORDINATE* pSrcRegionStartCoordinate,
            const D3D12_TILE_REGION_SIZE* pRegionSize, D3D12_TILE_MAPPING_FLAGS Flags) override
        { m_native->CopyTileMappings(pDstResource, pDstRegionStartCoordinate, pSrcResource, pSrcRegionStartCoordinate, pRegionSize, Flags); }

        void STDMETHODCALLTYPE ExecuteCommandLists(UINT NumCommandLists, ID3D12CommandList*const* ppCommandLists) override
        { m_native->ExecuteCommandLists(NumCommandLists, ppCommandLists); }

        void STDMETHODCALLTYPE SetMarker(UINT Metadata, const void* pData, UINT Size) override
        { m_native->SetMarker(Metadata, pData, Size); }

        void STDMETHODCALLTYPE BeginEvent(UINT Metadata, const void* pData, UINT Size) override
        { m_native->BeginEvent(Metadata, pData, Size); }

        void STDMETHODCALLTYPE EndEvent() override
        { m_native->EndEvent(); }

        HRESULT STDMETHODCALLTYPE Signal(ID3D12Fence* pFence, UINT64 Value) override
        {
            if (!pFence) return E_INVALIDARG;
            ComPtr<ID3D12Fence> fence;
            const HRESULT status = NativeGraphicsObject(pFence).As(&fence);
            return FAILED(status) ? status : m_native->Signal(fence.Get(), Value);
        }

        HRESULT STDMETHODCALLTYPE Wait(ID3D12Fence* pFence, UINT64 Value) override
        {
            if (!pFence) return E_INVALIDARG;
            ComPtr<ID3D12Fence> fence;
            const HRESULT status = NativeGraphicsObject(pFence).As(&fence);
            return FAILED(status) ? status : m_native->Wait(fence.Get(), Value);
        }

        HRESULT STDMETHODCALLTYPE GetTimestampFrequency(UINT64* pFrequency) override
        { return m_native->GetTimestampFrequency(pFrequency); }

        HRESULT STDMETHODCALLTYPE GetClockCalibration(UINT64* pGpuTimestamp, UINT64* pCpuTimestamp) override
        { return m_native->GetClockCalibration(pGpuTimestamp, pCpuTimestamp); }

        D3D12_COMMAND_QUEUE_DESC STDMETHODCALLTYPE GetDesc() override
        { return m_native->GetDesc(); }
    };

    template<class Child, class Create>
    HRESULT CreateD3D12Child(ID3D12Device* device, REFIID iid, void** output, const Create& create)
    {
        if (!output) return create(iid, nullptr);
        *output = nullptr;
        ComPtr<IUnknown> original;
        const HRESULT status = create(iid, reinterpret_cast<void**>(original.GetAddressOf()));
        if (FAILED(status) || !original) return status;
        ComPtr<typename Child::NativeInterface> native;
        const HRESULT queried = original.As(&native);
        if (FAILED(queried)) return queried;
        auto child = new (std::nothrow) Child(std::move(native), device);
        if (!child) return E_OUTOFMEMORY;
        const HRESULT wrapped = child->QueryInterface(iid, output);
        child->Release();
        return FAILED(wrapped) ? wrapped : status;
    }

    class GuestD3D12Device;
    std::mutex g_d3d12DevicesLock;
    std::unordered_map<IUnknown*, GuestD3D12Device*> g_d3d12Devices;

    class GuestD3D12Device final : public ID3D12Device14, public GuestGraphicsObject
    {
    public:
        GuestD3D12Device(ComPtr<ID3D12Device> native, IUnknown* identity)
            : m_native(std::move(native)), m_identity(identity) {}

        static bool IsDeviceInterface(REFIID iid)
        {
            return iid == __uuidof(IUnknown) || iid == __uuidof(ID3D12Object) ||
                iid == __uuidof(ID3D12Device) || iid == __uuidof(ID3D12Device1) ||
                iid == __uuidof(ID3D12Device2) || iid == __uuidof(ID3D12Device3) ||
                iid == __uuidof(ID3D12Device4) || iid == __uuidof(ID3D12Device5) ||
                iid == __uuidof(ID3D12Device6) || iid == __uuidof(ID3D12Device7) ||
                iid == __uuidof(ID3D12Device8) || iid == __uuidof(ID3D12Device9) ||
                iid == __uuidof(ID3D12Device10) || iid == __uuidof(ID3D12Device11) ||
                iid == __uuidof(ID3D12Device12) || iid == __uuidof(ID3D12Device13) ||
                iid == __uuidof(ID3D12Device14);
        }

        HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void** output) override
        {
            if (!output) return E_POINTER;
            *output = nullptr;
            if (iid == __uuidof(GuestGraphicsObject))
                *output = static_cast<GuestGraphicsObject*>(this);
            else if (IsDeviceInterface(iid))
            {
                ComPtr<IUnknown> supported;
                const HRESULT status = m_native->QueryInterface(iid, reinterpret_cast<void**>(supported.GetAddressOf()));
                if (FAILED(status)) return status;
                *output = static_cast<ID3D12Device14*>(this);
            }
            else return m_native->QueryInterface(iid, output);
            AddRef();
            return S_OK;
        }
        ULONG STDMETHODCALLTYPE AddRef() override { return ++m_refs; }
        ULONG STDMETHODCALLTYPE Release() override
        {
            ULONG refs;
            {
                std::lock_guard<std::mutex> lock(g_d3d12DevicesLock);
                refs = --m_refs;
                if (!refs)
                {
                    const auto entry = g_d3d12Devices.find(m_identity);
                    if (entry != g_d3d12Devices.end() && entry->second == this) g_d3d12Devices.erase(entry);
                }
            }
            if (!refs) delete this;
            return refs;
        }
        IUnknown* STDMETHODCALLTYPE NativeObject() override { return m_native.Get(); }
        HRESULT STDMETHODCALLTYPE GetPrivateData(REFGUID guid, UINT* pDataSize, void* pData) override
        { return m_native->GetPrivateData(guid, pDataSize, pData); }

        HRESULT STDMETHODCALLTYPE SetPrivateData(REFGUID guid, UINT DataSize, const void* pData) override
        { return m_native->SetPrivateData(guid, DataSize, pData); }

        HRESULT STDMETHODCALLTYPE SetPrivateDataInterface(REFGUID guid, const IUnknown* pData) override
        { return m_native->SetPrivateDataInterface(guid, pData); }

        HRESULT STDMETHODCALLTYPE SetName(LPCWSTR Name) override
        { return m_native->SetName(Name); }
        UINT STDMETHODCALLTYPE GetNodeCount() override
        { return m_native->GetNodeCount(); }

        HRESULT STDMETHODCALLTYPE CreateCommandQueue(const D3D12_COMMAND_QUEUE_DESC* pDesc, REFIID riid,
            void** ppCommandQueue) override
        {
            return CreateD3D12Child<GuestD3D12Queue>(this, riid, ppCommandQueue, [&](REFIID iid, void** output)
            {
                return m_native->CreateCommandQueue(pDesc, iid, output);
            });
        }

        HRESULT STDMETHODCALLTYPE CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE type, REFIID riid,
            void** ppCommandAllocator) override
        { return m_native->CreateCommandAllocator(type, riid, ppCommandAllocator); }

        HRESULT STDMETHODCALLTYPE CreateGraphicsPipelineState(const D3D12_GRAPHICS_PIPELINE_STATE_DESC* pDesc,
            REFIID riid, void** ppPipelineState) override
        { return m_native->CreateGraphicsPipelineState(pDesc, riid, ppPipelineState); }

        HRESULT STDMETHODCALLTYPE CreateComputePipelineState(const D3D12_COMPUTE_PIPELINE_STATE_DESC* pDesc,
            REFIID riid, void** ppPipelineState) override
        { return m_native->CreateComputePipelineState(pDesc, riid, ppPipelineState); }

        HRESULT STDMETHODCALLTYPE CreateCommandList(UINT nodeMask, D3D12_COMMAND_LIST_TYPE type,
            ID3D12CommandAllocator* pCommandAllocator, ID3D12PipelineState* pInitialState, REFIID riid,
            void** ppCommandList) override
        { return m_native->CreateCommandList(nodeMask, type, pCommandAllocator, pInitialState, riid, ppCommandList); }

        HRESULT STDMETHODCALLTYPE CheckFeatureSupport(D3D12_FEATURE Feature, void* pFeatureSupportData,
            UINT FeatureSupportDataSize) override
        { return m_native->CheckFeatureSupport(Feature, pFeatureSupportData, FeatureSupportDataSize); }

        HRESULT STDMETHODCALLTYPE CreateDescriptorHeap(const D3D12_DESCRIPTOR_HEAP_DESC* pDescriptorHeapDesc,
            REFIID riid, void** ppvHeap) override
        { return m_native->CreateDescriptorHeap(pDescriptorHeapDesc, riid, ppvHeap); }

        UINT STDMETHODCALLTYPE GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE DescriptorHeapType) override
        { return m_native->GetDescriptorHandleIncrementSize(DescriptorHeapType); }

        HRESULT STDMETHODCALLTYPE CreateRootSignature(UINT nodeMask, const void* pBlobWithRootSignature,
            SIZE_T blobLengthInBytes, REFIID riid, void** ppvRootSignature) override
        { return m_native->CreateRootSignature(nodeMask, pBlobWithRootSignature, blobLengthInBytes, riid, ppvRootSignature); }

        void STDMETHODCALLTYPE CreateConstantBufferView(const D3D12_CONSTANT_BUFFER_VIEW_DESC* pDesc,
            D3D12_CPU_DESCRIPTOR_HANDLE DestDescriptor) override
        { m_native->CreateConstantBufferView(pDesc, DestDescriptor); }

        void STDMETHODCALLTYPE CreateShaderResourceView(ID3D12Resource* pResource,
            const D3D12_SHADER_RESOURCE_VIEW_DESC* pDesc, D3D12_CPU_DESCRIPTOR_HANDLE DestDescriptor) override
        { m_native->CreateShaderResourceView(pResource, pDesc, DestDescriptor); }

        void STDMETHODCALLTYPE CreateUnorderedAccessView(ID3D12Resource* pResource, ID3D12Resource* pCounterResource,
            const D3D12_UNORDERED_ACCESS_VIEW_DESC* pDesc, D3D12_CPU_DESCRIPTOR_HANDLE DestDescriptor) override
        { m_native->CreateUnorderedAccessView(pResource, pCounterResource, pDesc, DestDescriptor); }

        void STDMETHODCALLTYPE CreateRenderTargetView(ID3D12Resource* pResource,
            const D3D12_RENDER_TARGET_VIEW_DESC* pDesc, D3D12_CPU_DESCRIPTOR_HANDLE DestDescriptor) override
        { m_native->CreateRenderTargetView(pResource, pDesc, DestDescriptor); }

        void STDMETHODCALLTYPE CreateDepthStencilView(ID3D12Resource* pResource,
            const D3D12_DEPTH_STENCIL_VIEW_DESC* pDesc, D3D12_CPU_DESCRIPTOR_HANDLE DestDescriptor) override
        { m_native->CreateDepthStencilView(pResource, pDesc, DestDescriptor); }

        void STDMETHODCALLTYPE CreateSampler(const D3D12_SAMPLER_DESC* pDesc,
            D3D12_CPU_DESCRIPTOR_HANDLE DestDescriptor) override
        { m_native->CreateSampler(pDesc, DestDescriptor); }

        void STDMETHODCALLTYPE CopyDescriptors(UINT NumDestDescriptorRanges,
            const D3D12_CPU_DESCRIPTOR_HANDLE* pDestDescriptorRangeStarts, const UINT* pDestDescriptorRangeSizes,
            UINT NumSrcDescriptorRanges, const D3D12_CPU_DESCRIPTOR_HANDLE* pSrcDescriptorRangeStarts,
            const UINT* pSrcDescriptorRangeSizes, D3D12_DESCRIPTOR_HEAP_TYPE DescriptorHeapsType) override
        { m_native->CopyDescriptors(NumDestDescriptorRanges, pDestDescriptorRangeStarts, pDestDescriptorRangeSizes, NumSrcDescriptorRanges, pSrcDescriptorRangeStarts, pSrcDescriptorRangeSizes, DescriptorHeapsType); }

        void STDMETHODCALLTYPE CopyDescriptorsSimple(UINT NumDescriptors,
            D3D12_CPU_DESCRIPTOR_HANDLE DestDescriptorRangeStart, D3D12_CPU_DESCRIPTOR_HANDLE SrcDescriptorRangeStart,
            D3D12_DESCRIPTOR_HEAP_TYPE DescriptorHeapsType) override
        { m_native->CopyDescriptorsSimple(NumDescriptors, DestDescriptorRangeStart, SrcDescriptorRangeStart, DescriptorHeapsType); }

        D3D12_RESOURCE_ALLOCATION_INFO STDMETHODCALLTYPE GetResourceAllocationInfo(UINT visibleMask,
            UINT numResourceDescs, const D3D12_RESOURCE_DESC* pResourceDescs) override
        { return m_native->GetResourceAllocationInfo(visibleMask, numResourceDescs, pResourceDescs); }

        D3D12_HEAP_PROPERTIES STDMETHODCALLTYPE GetCustomHeapProperties(UINT nodeMask, D3D12_HEAP_TYPE heapType) override
        { return m_native->GetCustomHeapProperties(nodeMask, heapType); }

        HRESULT STDMETHODCALLTYPE CreateCommittedResource(const D3D12_HEAP_PROPERTIES* pHeapProperties,
            D3D12_HEAP_FLAGS HeapFlags, const D3D12_RESOURCE_DESC* pDesc, D3D12_RESOURCE_STATES InitialResourceState,
            const D3D12_CLEAR_VALUE* pOptimizedClearValue, REFIID riidResource, void** ppvResource) override
        { return m_native->CreateCommittedResource(pHeapProperties, HeapFlags, pDesc, InitialResourceState, pOptimizedClearValue, riidResource, ppvResource); }

        HRESULT STDMETHODCALLTYPE CreateHeap(const D3D12_HEAP_DESC* pDesc, REFIID riid, void** ppvHeap) override
        { return m_native->CreateHeap(pDesc, riid, ppvHeap); }

        HRESULT STDMETHODCALLTYPE CreatePlacedResource(ID3D12Heap* pHeap, UINT64 HeapOffset,
            const D3D12_RESOURCE_DESC* pDesc, D3D12_RESOURCE_STATES InitialState,
            const D3D12_CLEAR_VALUE* pOptimizedClearValue, REFIID riid, void** ppvResource) override
        { return m_native->CreatePlacedResource(pHeap, HeapOffset, pDesc, InitialState, pOptimizedClearValue, riid, ppvResource); }

        HRESULT STDMETHODCALLTYPE CreateReservedResource(const D3D12_RESOURCE_DESC* pDesc,
            D3D12_RESOURCE_STATES InitialState, const D3D12_CLEAR_VALUE* pOptimizedClearValue, REFIID riid,
            void** ppvResource) override
        { return m_native->CreateReservedResource(pDesc, InitialState, pOptimizedClearValue, riid, ppvResource); }

        HRESULT STDMETHODCALLTYPE CreateSharedHandle(ID3D12DeviceChild* pObject,
            const SECURITY_ATTRIBUTES* pAttributes, DWORD Access, LPCWSTR Name, HANDLE* pHandle) override
        {
            return CreateGraphicsShare(pAttributes, Name, pHandle, [&](HANDLE* handle)
            {
                if (!pObject) return E_INVALIDARG;
                ComPtr<ID3D12DeviceChild> native;
                const HRESULT status = NativeGraphicsObject(pObject).As(&native);
                return FAILED(status) ? status : m_native->CreateSharedHandle(native.Get(), nullptr, Access, nullptr, handle);
            });
        }

        HRESULT STDMETHODCALLTYPE OpenSharedHandle(HANDLE NTHandle, REFIID riid, void** ppvObj) override
        {
            if (!ppvObj) return E_POINTER;
            *ppvObj = nullptr;
            const auto kernel = CurrentGuestKernelContext();
            const auto handle = kernel ? kernel->LeaseGraphicsHandle(NTHandle) : std::shared_ptr<void>{};
            if (!handle) return E_HANDLE;
            if (!GuestD3D12Fence::IsFenceInterface(riid))
                return m_native->OpenSharedHandle(handle.get(), riid, ppvObj);
            return CreateD3D12Child<GuestD3D12Fence>(this, riid, ppvObj, [&](REFIID iid, void** output)
            {
                return m_native->OpenSharedHandle(handle.get(), iid, output);
            });
        }

        HRESULT STDMETHODCALLTYPE OpenSharedHandleByName(LPCWSTR Name, DWORD Access,
            /* [annotation][out]*/ HANDLE* pNTHandle) override
        {
            if (!pNTHandle) return E_POINTER;
            *pNTHandle = nullptr;
            const auto kernel = CurrentGuestKernelContext();
            if (!kernel || Access != GENERIC_ALL) return E_INVALIDARG;
            const HRESULT status = kernel->OpenGraphicsHandle(Name, pNTHandle);
            return status == HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND) ? E_HANDLE : status;
        }

        HRESULT STDMETHODCALLTYPE MakeResident(UINT NumObjects, ID3D12Pageable*const* ppObjects) override
        {
            return WithNativeGraphicsObjects(NumObjects, ppObjects, [&](ID3D12Pageable* const* objects)
            {
                return m_native->MakeResident(NumObjects, objects);
            });
        }

        HRESULT STDMETHODCALLTYPE Evict(UINT NumObjects, ID3D12Pageable*const* ppObjects) override
        {
            return WithNativeGraphicsObjects(NumObjects, ppObjects, [&](ID3D12Pageable* const* objects)
            {
                return m_native->Evict(NumObjects, objects);
            });
        }

        HRESULT STDMETHODCALLTYPE CreateFence(UINT64 InitialValue, D3D12_FENCE_FLAGS Flags, REFIID riid,
            void** ppFence) override
        {
            return CreateD3D12Child<GuestD3D12Fence>(this, riid, ppFence, [&](REFIID iid, void** output)
            {
                return m_native->CreateFence(InitialValue, Flags, iid, output);
            });
        }

        HRESULT STDMETHODCALLTYPE GetDeviceRemovedReason() override
        { return m_native->GetDeviceRemovedReason(); }

        void STDMETHODCALLTYPE GetCopyableFootprints(const D3D12_RESOURCE_DESC* pResourceDesc, UINT FirstSubresource,
            UINT NumSubresources, UINT64 BaseOffset, D3D12_PLACED_SUBRESOURCE_FOOTPRINT* pLayouts, UINT* pNumRows,
            UINT64* pRowSizeInBytes, UINT64* pTotalBytes) override
        { m_native->GetCopyableFootprints(pResourceDesc, FirstSubresource, NumSubresources, BaseOffset, pLayouts, pNumRows, pRowSizeInBytes, pTotalBytes); }

        HRESULT STDMETHODCALLTYPE CreateQueryHeap(const D3D12_QUERY_HEAP_DESC* pDesc, REFIID riid, void** ppvHeap) override
        { return m_native->CreateQueryHeap(pDesc, riid, ppvHeap); }

        HRESULT STDMETHODCALLTYPE SetStablePowerState(BOOL Enable) override
        { return m_native->SetStablePowerState(Enable); }

        HRESULT STDMETHODCALLTYPE CreateCommandSignature(const D3D12_COMMAND_SIGNATURE_DESC* pDesc,
            ID3D12RootSignature* pRootSignature, REFIID riid, void** ppvCommandSignature) override
        { return m_native->CreateCommandSignature(pDesc, pRootSignature, riid, ppvCommandSignature); }

        void STDMETHODCALLTYPE GetResourceTiling(ID3D12Resource* pTiledResource, UINT* pNumTilesForEntireResource,
            D3D12_PACKED_MIP_INFO* pPackedMipDesc, D3D12_TILE_SHAPE* pStandardTileShapeForNonPackedMips,
            UINT* pNumSubresourceTilings, UINT FirstSubresourceTilingToGet,
            D3D12_SUBRESOURCE_TILING* pSubresourceTilingsForNonPackedMips) override
        { m_native->GetResourceTiling(pTiledResource, pNumTilesForEntireResource, pPackedMipDesc, pStandardTileShapeForNonPackedMips, pNumSubresourceTilings, FirstSubresourceTilingToGet, pSubresourceTilingsForNonPackedMips); }

        LUID STDMETHODCALLTYPE GetAdapterLuid() override
        { return m_native->GetAdapterLuid(); }

        HRESULT STDMETHODCALLTYPE CreatePipelineLibrary(const void* pLibraryBlob, SIZE_T BlobLength, REFIID riid,
            void** ppPipelineLibrary) override
        {
            ComPtr<ID3D12Device1> native;
            const HRESULT status = m_native.As(&native);
            return FAILED(status) ? status : native->CreatePipelineLibrary(pLibraryBlob, BlobLength, riid, ppPipelineLibrary);
        }

        HRESULT STDMETHODCALLTYPE SetEventOnMultipleFenceCompletion(ID3D12Fence*const* ppFences,
            const UINT64* pFenceValues, UINT NumFences, D3D12_MULTIPLE_FENCE_WAIT_FLAGS Flags, HANDLE hEvent) override
        {
            ComPtr<ID3D12Device1> native;
            const HRESULT status = m_native.As(&native);
            if (FAILED(status)) return status;
            return WithNativeGraphicsObjects(NumFences, ppFences, [&](ID3D12Fence* const* fences)
            {
                return RegisterGraphicsEvent(hEvent, [&](HANDLE event)
                {
                    return native->SetEventOnMultipleFenceCompletion(fences, pFenceValues, NumFences, Flags, event);
                });
            });
        }

        HRESULT STDMETHODCALLTYPE SetResidencyPriority(UINT NumObjects, ID3D12Pageable*const* ppObjects,
            const D3D12_RESIDENCY_PRIORITY* pPriorities) override
        {
            ComPtr<ID3D12Device1> native;
            const HRESULT status = m_native.As(&native);
            if (FAILED(status)) return status;
            return WithNativeGraphicsObjects(NumObjects, ppObjects, [&](ID3D12Pageable* const* objects)
            {
                return native->SetResidencyPriority(NumObjects, objects, pPriorities);
            });
        }

        HRESULT STDMETHODCALLTYPE CreatePipelineState(const D3D12_PIPELINE_STATE_STREAM_DESC* pDesc, REFIID riid,
            void** ppPipelineState) override
        {
            ComPtr<ID3D12Device2> native;
            const HRESULT status = m_native.As(&native);
            return FAILED(status) ? status : native->CreatePipelineState(pDesc, riid, ppPipelineState);
        }

        HRESULT STDMETHODCALLTYPE OpenExistingHeapFromAddress(const void* pAddress, REFIID riid, void** ppvHeap) override
        {
            ComPtr<ID3D12Device3> native;
            const HRESULT status = m_native.As(&native);
            return FAILED(status) ? status : native->OpenExistingHeapFromAddress(pAddress, riid, ppvHeap);
        }

        HRESULT STDMETHODCALLTYPE OpenExistingHeapFromFileMapping(HANDLE hFileMapping, REFIID riid, void** ppvHeap) override
        {
            ComPtr<ID3D12Device3> native;
            const HRESULT status = m_native.As(&native);
            return FAILED(status) ? status : native->OpenExistingHeapFromFileMapping(hFileMapping, riid, ppvHeap);
        }

        HRESULT STDMETHODCALLTYPE EnqueueMakeResident(D3D12_RESIDENCY_FLAGS Flags, UINT NumObjects,
            ID3D12Pageable*const* ppObjects, ID3D12Fence* pFenceToSignal, UINT64 FenceValueToSignal) override
        {
            ComPtr<ID3D12Device3> native;
            HRESULT status = m_native.As(&native);
            if (FAILED(status)) return status;
            ComPtr<ID3D12Fence> fence;
            if (pFenceToSignal)
            {
                status = NativeGraphicsObject(pFenceToSignal).As(&fence);
                if (FAILED(status)) return status;
            }
            return WithNativeGraphicsObjects(NumObjects, ppObjects, [&](ID3D12Pageable* const* objects)
            {
                return native->EnqueueMakeResident(Flags, NumObjects, objects, fence.Get(), FenceValueToSignal);
            });
        }

        HRESULT STDMETHODCALLTYPE CreateCommandList1(UINT nodeMask, D3D12_COMMAND_LIST_TYPE type,
            D3D12_COMMAND_LIST_FLAGS flags, REFIID riid, void** ppCommandList) override
        {
            ComPtr<ID3D12Device4> native;
            const HRESULT status = m_native.As(&native);
            return FAILED(status) ? status : native->CreateCommandList1(nodeMask, type, flags, riid, ppCommandList);
        }

        HRESULT STDMETHODCALLTYPE CreateProtectedResourceSession(const D3D12_PROTECTED_RESOURCE_SESSION_DESC* pDesc,
            REFIID riid, void** ppSession) override
        {
            ComPtr<ID3D12Device4> native;
            const HRESULT status = m_native.As(&native);
            return FAILED(status) ? status : native->CreateProtectedResourceSession(pDesc, riid, ppSession);
        }

        HRESULT STDMETHODCALLTYPE CreateCommittedResource1(const D3D12_HEAP_PROPERTIES* pHeapProperties,
            D3D12_HEAP_FLAGS HeapFlags, const D3D12_RESOURCE_DESC* pDesc, D3D12_RESOURCE_STATES InitialResourceState,
            const D3D12_CLEAR_VALUE* pOptimizedClearValue, ID3D12ProtectedResourceSession* pProtectedSession,
            REFIID riidResource, void** ppvResource) override
        {
            ComPtr<ID3D12Device4> native;
            const HRESULT status = m_native.As(&native);
            return FAILED(status) ? status : native->CreateCommittedResource1(pHeapProperties, HeapFlags, pDesc, InitialResourceState, pOptimizedClearValue, pProtectedSession, riidResource, ppvResource);
        }

        HRESULT STDMETHODCALLTYPE CreateHeap1(const D3D12_HEAP_DESC* pDesc,
            ID3D12ProtectedResourceSession* pProtectedSession, REFIID riid, void** ppvHeap) override
        {
            ComPtr<ID3D12Device4> native;
            const HRESULT status = m_native.As(&native);
            return FAILED(status) ? status : native->CreateHeap1(pDesc, pProtectedSession, riid, ppvHeap);
        }

        HRESULT STDMETHODCALLTYPE CreateReservedResource1(const D3D12_RESOURCE_DESC* pDesc,
            D3D12_RESOURCE_STATES InitialState, const D3D12_CLEAR_VALUE* pOptimizedClearValue,
            ID3D12ProtectedResourceSession* pProtectedSession, REFIID riid, void** ppvResource) override
        {
            ComPtr<ID3D12Device4> native;
            const HRESULT status = m_native.As(&native);
            return FAILED(status) ? status : native->CreateReservedResource1(pDesc, InitialState, pOptimizedClearValue, pProtectedSession, riid, ppvResource);
        }

        D3D12_RESOURCE_ALLOCATION_INFO STDMETHODCALLTYPE GetResourceAllocationInfo1(UINT visibleMask,
            UINT numResourceDescs, const D3D12_RESOURCE_DESC* pResourceDescs,
            D3D12_RESOURCE_ALLOCATION_INFO1* pResourceAllocationInfo1) override
        {
            ComPtr<ID3D12Device4> native;
            const HRESULT status = m_native.As(&native);
            return FAILED(status) ? D3D12_RESOURCE_ALLOCATION_INFO{} : native->GetResourceAllocationInfo1(visibleMask, numResourceDescs, pResourceDescs, pResourceAllocationInfo1);
        }

        HRESULT STDMETHODCALLTYPE CreateLifetimeTracker(ID3D12LifetimeOwner* pOwner, REFIID riid, void** ppvTracker) override
        {
            ComPtr<ID3D12Device5> native;
            const HRESULT status = m_native.As(&native);
            return FAILED(status) ? status : native->CreateLifetimeTracker(pOwner, riid, ppvTracker);
        }

        void STDMETHODCALLTYPE RemoveDevice() override
        {
            ComPtr<ID3D12Device5> native;
            if (SUCCEEDED(m_native.As(&native))) native->RemoveDevice();
        }

        HRESULT STDMETHODCALLTYPE EnumerateMetaCommands(UINT* pNumMetaCommands, D3D12_META_COMMAND_DESC* pDescs) override
        {
            ComPtr<ID3D12Device5> native;
            const HRESULT status = m_native.As(&native);
            return FAILED(status) ? status : native->EnumerateMetaCommands(pNumMetaCommands, pDescs);
        }

        HRESULT STDMETHODCALLTYPE EnumerateMetaCommandParameters(REFGUID CommandId,
            D3D12_META_COMMAND_PARAMETER_STAGE Stage, UINT* pTotalStructureSizeInBytes, UINT* pParameterCount,
            D3D12_META_COMMAND_PARAMETER_DESC* pParameterDescs) override
        {
            ComPtr<ID3D12Device5> native;
            const HRESULT status = m_native.As(&native);
            return FAILED(status) ? status : native->EnumerateMetaCommandParameters(CommandId, Stage, pTotalStructureSizeInBytes, pParameterCount, pParameterDescs);
        }

        HRESULT STDMETHODCALLTYPE CreateMetaCommand(REFGUID CommandId, UINT NodeMask,
            const void* pCreationParametersData, SIZE_T CreationParametersDataSizeInBytes, REFIID riid,
            void** ppMetaCommand) override
        {
            ComPtr<ID3D12Device5> native;
            const HRESULT status = m_native.As(&native);
            return FAILED(status) ? status : native->CreateMetaCommand(CommandId, NodeMask, pCreationParametersData, CreationParametersDataSizeInBytes, riid, ppMetaCommand);
        }

        HRESULT STDMETHODCALLTYPE CreateStateObject(const D3D12_STATE_OBJECT_DESC* pDesc, REFIID riid,
            void** ppStateObject) override
        {
            ComPtr<ID3D12Device5> native;
            const HRESULT status = m_native.As(&native);
            return FAILED(status) ? status : native->CreateStateObject(pDesc, riid, ppStateObject);
        }

        void STDMETHODCALLTYPE GetRaytracingAccelerationStructurePrebuildInfo(const D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS* pDesc,
            D3D12_RAYTRACING_ACCELERATION_STRUCTURE_PREBUILD_INFO* pInfo) override
        {
            ComPtr<ID3D12Device5> native;
            if (SUCCEEDED(m_native.As(&native))) native->GetRaytracingAccelerationStructurePrebuildInfo(pDesc, pInfo);
        }

        D3D12_DRIVER_MATCHING_IDENTIFIER_STATUS STDMETHODCALLTYPE CheckDriverMatchingIdentifier(D3D12_SERIALIZED_DATA_TYPE SerializedDataType,
            const D3D12_SERIALIZED_DATA_DRIVER_MATCHING_IDENTIFIER* pIdentifierToCheck) override
        {
            ComPtr<ID3D12Device5> native;
            const HRESULT status = m_native.As(&native);
            return FAILED(status) ? D3D12_DRIVER_MATCHING_IDENTIFIER_STATUS{} : native->CheckDriverMatchingIdentifier(SerializedDataType, pIdentifierToCheck);
        }

        HRESULT STDMETHODCALLTYPE SetBackgroundProcessingMode(D3D12_BACKGROUND_PROCESSING_MODE Mode,
            D3D12_MEASUREMENTS_ACTION MeasurementsAction, HANDLE hEventToSignalUponCompletion,
            BOOL* pbFurtherMeasurementsDesired) override
        {
            ComPtr<ID3D12Device6> native;
            const HRESULT status = m_native.As(&native);
            return FAILED(status) ? status : native->SetBackgroundProcessingMode(Mode, MeasurementsAction, hEventToSignalUponCompletion, pbFurtherMeasurementsDesired);
        }

        HRESULT STDMETHODCALLTYPE AddToStateObject(const D3D12_STATE_OBJECT_DESC* pAddition,
            ID3D12StateObject* pStateObjectToGrowFrom, REFIID riid, void** ppNewStateObject) override
        {
            ComPtr<ID3D12Device7> native;
            const HRESULT status = m_native.As(&native);
            return FAILED(status) ? status : native->AddToStateObject(pAddition, pStateObjectToGrowFrom, riid, ppNewStateObject);
        }

        HRESULT STDMETHODCALLTYPE CreateProtectedResourceSession1(const D3D12_PROTECTED_RESOURCE_SESSION_DESC1* pDesc,
            REFIID riid, void** ppSession) override
        {
            ComPtr<ID3D12Device7> native;
            const HRESULT status = m_native.As(&native);
            return FAILED(status) ? status : native->CreateProtectedResourceSession1(pDesc, riid, ppSession);
        }

        D3D12_RESOURCE_ALLOCATION_INFO STDMETHODCALLTYPE GetResourceAllocationInfo2(UINT visibleMask,
            UINT numResourceDescs, const D3D12_RESOURCE_DESC1* pResourceDescs,
            D3D12_RESOURCE_ALLOCATION_INFO1* pResourceAllocationInfo1) override
        {
            ComPtr<ID3D12Device8> native;
            const HRESULT status = m_native.As(&native);
            return FAILED(status) ? D3D12_RESOURCE_ALLOCATION_INFO{} : native->GetResourceAllocationInfo2(visibleMask, numResourceDescs, pResourceDescs, pResourceAllocationInfo1);
        }

        HRESULT STDMETHODCALLTYPE CreateCommittedResource2(const D3D12_HEAP_PROPERTIES* pHeapProperties,
            D3D12_HEAP_FLAGS HeapFlags, const D3D12_RESOURCE_DESC1* pDesc, D3D12_RESOURCE_STATES InitialResourceState,
            const D3D12_CLEAR_VALUE* pOptimizedClearValue, ID3D12ProtectedResourceSession* pProtectedSession,
            REFIID riidResource, void** ppvResource) override
        {
            ComPtr<ID3D12Device8> native;
            const HRESULT status = m_native.As(&native);
            return FAILED(status) ? status : native->CreateCommittedResource2(pHeapProperties, HeapFlags, pDesc, InitialResourceState, pOptimizedClearValue, pProtectedSession, riidResource, ppvResource);
        }

        HRESULT STDMETHODCALLTYPE CreatePlacedResource1(ID3D12Heap* pHeap, UINT64 HeapOffset,
            const D3D12_RESOURCE_DESC1* pDesc, D3D12_RESOURCE_STATES InitialState,
            const D3D12_CLEAR_VALUE* pOptimizedClearValue, REFIID riid, void** ppvResource) override
        {
            ComPtr<ID3D12Device8> native;
            const HRESULT status = m_native.As(&native);
            return FAILED(status) ? status : native->CreatePlacedResource1(pHeap, HeapOffset, pDesc, InitialState, pOptimizedClearValue, riid, ppvResource);
        }

        void STDMETHODCALLTYPE CreateSamplerFeedbackUnorderedAccessView(ID3D12Resource* pTargetedResource,
            ID3D12Resource* pFeedbackResource, D3D12_CPU_DESCRIPTOR_HANDLE DestDescriptor) override
        {
            ComPtr<ID3D12Device8> native;
            if (SUCCEEDED(m_native.As(&native))) native->CreateSamplerFeedbackUnorderedAccessView(pTargetedResource, pFeedbackResource, DestDescriptor);
        }

        void STDMETHODCALLTYPE GetCopyableFootprints1(const D3D12_RESOURCE_DESC1* pResourceDesc,
            UINT FirstSubresource, UINT NumSubresources, UINT64 BaseOffset,
            D3D12_PLACED_SUBRESOURCE_FOOTPRINT* pLayouts, UINT* pNumRows, UINT64* pRowSizeInBytes,
            UINT64* pTotalBytes) override
        {
            ComPtr<ID3D12Device8> native;
            if (SUCCEEDED(m_native.As(&native))) native->GetCopyableFootprints1(pResourceDesc, FirstSubresource, NumSubresources, BaseOffset, pLayouts, pNumRows, pRowSizeInBytes, pTotalBytes);
        }

        HRESULT STDMETHODCALLTYPE CreateShaderCacheSession(const D3D12_SHADER_CACHE_SESSION_DESC* pDesc, REFIID riid,
            void** ppvSession) override
        {
            ComPtr<ID3D12Device9> native;
            const HRESULT status = m_native.As(&native);
            return FAILED(status) ? status : native->CreateShaderCacheSession(pDesc, riid, ppvSession);
        }

        HRESULT STDMETHODCALLTYPE ShaderCacheControl(D3D12_SHADER_CACHE_KIND_FLAGS Kinds,
            D3D12_SHADER_CACHE_CONTROL_FLAGS Control) override
        {
            ComPtr<ID3D12Device9> native;
            const HRESULT status = m_native.As(&native);
            return FAILED(status) ? status : native->ShaderCacheControl(Kinds, Control);
        }

        HRESULT STDMETHODCALLTYPE CreateCommandQueue1(const D3D12_COMMAND_QUEUE_DESC* pDesc, REFIID CreatorID,
            REFIID riid, void** ppCommandQueue) override
        {
            ComPtr<ID3D12Device9> native;
            const HRESULT status = m_native.As(&native);
            if (FAILED(status)) return status;
            return CreateD3D12Child<GuestD3D12Queue>(this, riid, ppCommandQueue, [&](REFIID iid, void** output)
            {
                return native->CreateCommandQueue1(pDesc, CreatorID, iid, output);
            });
        }

        HRESULT STDMETHODCALLTYPE CreateCommittedResource3(const D3D12_HEAP_PROPERTIES* pHeapProperties,
            D3D12_HEAP_FLAGS HeapFlags, const D3D12_RESOURCE_DESC1* pDesc, D3D12_BARRIER_LAYOUT InitialLayout,
            const D3D12_CLEAR_VALUE* pOptimizedClearValue, ID3D12ProtectedResourceSession* pProtectedSession,
            UINT32 NumCastableFormats, const DXGI_FORMAT* pCastableFormats, REFIID riidResource, void** ppvResource) override
        {
            ComPtr<ID3D12Device10> native;
            const HRESULT status = m_native.As(&native);
            return FAILED(status) ? status : native->CreateCommittedResource3(pHeapProperties, HeapFlags, pDesc, InitialLayout, pOptimizedClearValue, pProtectedSession, NumCastableFormats, pCastableFormats, riidResource, ppvResource);
        }

        HRESULT STDMETHODCALLTYPE CreatePlacedResource2(ID3D12Heap* pHeap, UINT64 HeapOffset,
            const D3D12_RESOURCE_DESC1* pDesc, D3D12_BARRIER_LAYOUT InitialLayout,
            const D3D12_CLEAR_VALUE* pOptimizedClearValue, UINT32 NumCastableFormats,
            const DXGI_FORMAT* pCastableFormats, REFIID riid, void** ppvResource) override
        {
            ComPtr<ID3D12Device10> native;
            const HRESULT status = m_native.As(&native);
            return FAILED(status) ? status : native->CreatePlacedResource2(pHeap, HeapOffset, pDesc, InitialLayout, pOptimizedClearValue, NumCastableFormats, pCastableFormats, riid, ppvResource);
        }

        HRESULT STDMETHODCALLTYPE CreateReservedResource2(const D3D12_RESOURCE_DESC* pDesc,
            D3D12_BARRIER_LAYOUT InitialLayout, const D3D12_CLEAR_VALUE* pOptimizedClearValue,
            ID3D12ProtectedResourceSession* pProtectedSession, UINT32 NumCastableFormats,
            const DXGI_FORMAT* pCastableFormats, REFIID riid, void** ppvResource) override
        {
            ComPtr<ID3D12Device10> native;
            const HRESULT status = m_native.As(&native);
            return FAILED(status) ? status : native->CreateReservedResource2(pDesc, InitialLayout, pOptimizedClearValue, pProtectedSession, NumCastableFormats, pCastableFormats, riid, ppvResource);
        }

        void STDMETHODCALLTYPE CreateSampler2(const D3D12_SAMPLER_DESC2* pDesc,
            D3D12_CPU_DESCRIPTOR_HANDLE DestDescriptor) override
        {
            ComPtr<ID3D12Device11> native;
            if (SUCCEEDED(m_native.As(&native))) native->CreateSampler2(pDesc, DestDescriptor);
        }

        D3D12_RESOURCE_ALLOCATION_INFO STDMETHODCALLTYPE GetResourceAllocationInfo3(UINT visibleMask,
            UINT numResourceDescs, const D3D12_RESOURCE_DESC1* pResourceDescs, const UINT32* pNumCastableFormats,
            const DXGI_FORMAT*const* ppCastableFormats, D3D12_RESOURCE_ALLOCATION_INFO1* pResourceAllocationInfo1) override
        {
            ComPtr<ID3D12Device12> native;
            const HRESULT status = m_native.As(&native);
            return FAILED(status) ? D3D12_RESOURCE_ALLOCATION_INFO{} : native->GetResourceAllocationInfo3(visibleMask, numResourceDescs, pResourceDescs, pNumCastableFormats, ppCastableFormats, pResourceAllocationInfo1);
        }

        HRESULT STDMETHODCALLTYPE OpenExistingHeapFromAddress1(const void* pAddress, SIZE_T size, REFIID riid,
            void** ppvHeap) override
        {
            ComPtr<ID3D12Device13> native;
            const HRESULT status = m_native.As(&native);
            return FAILED(status) ? status : native->OpenExistingHeapFromAddress1(pAddress, size, riid, ppvHeap);
        }

        HRESULT STDMETHODCALLTYPE CreateRootSignatureFromSubobjectInLibrary(UINT nodeMask, const void* pLibraryBlob,
            SIZE_T blobLengthInBytes, LPCWSTR subobjectName, REFIID riid, void** ppvRootSignature) override
        {
            ComPtr<ID3D12Device14> native;
            const HRESULT status = m_native.As(&native);
            return FAILED(status) ? status : native->CreateRootSignatureFromSubobjectInLibrary(nodeMask, pLibraryBlob, blobLengthInBytes, subobjectName, riid, ppvRootSignature);
        }
    private:
        ComPtr<ID3D12Device> m_native;
        IUnknown* const m_identity;
        std::atomic<ULONG> m_refs{1};
    };

    HRESULT WrapD3D12Device(ComPtr<ID3D12Device> native, REFIID iid, void** output)
    {
        ComPtr<IUnknown> identity;
        const HRESULT status = native.As(&identity);
        if (FAILED(status)) return status;

        ComPtr<ID3D12Device> device;
        {
            // D3D12 caches devices per adapter. Keep one proxy while that
            // native identity is visible through any guest reference.
            std::lock_guard<std::mutex> lock(g_d3d12DevicesLock);
            const auto found = g_d3d12Devices.find(identity.Get());
            if (found != g_d3d12Devices.end())
                device = found->second;
            else
            {
                auto wrapper = new (std::nothrow) GuestD3D12Device(std::move(native), identity.Get());
                if (!wrapper) return E_OUTOFMEMORY;
                device.Attach(wrapper);
                try { g_d3d12Devices.emplace(identity.Get(), wrapper); }
                catch (const std::bad_alloc&) { return E_OUTOFMEMORY; }
            }
        }
        return device->QueryInterface(iid, output);
    }

    template<class Create>
    HRESULT CreateD3D12GuestDevice(IUnknown* adapter, D3D_FEATURE_LEVEL level, REFIID iid, void** device,
        const Create& create)
    {
        const auto native = NativeGraphicsObject(adapter);
        if (!device || !GuestD3D12Device::IsDeviceInterface(iid))
            return create(native.Get(), level, iid, device);
        *device = nullptr;
        ComPtr<IUnknown> created;
        const HRESULT status = create(native.Get(), level, iid, reinterpret_cast<void**>(created.GetAddressOf()));
        if (FAILED(status) || !created) return status;
        ComPtr<ID3D12Device> nativeDevice;
        const HRESULT queried = created.As(&nativeDevice);
        if (FAILED(queried)) return queried;
        const HRESULT wrapped = WrapD3D12Device(std::move(nativeDevice), iid, device);
        return FAILED(wrapped) ? wrapped : status;
    }
