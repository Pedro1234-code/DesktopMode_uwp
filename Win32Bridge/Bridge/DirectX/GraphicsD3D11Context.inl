    class GuestD3D11Context;

    // A context retains its device. The cache observes contexts without
    // retaining them, so the immediate context and device cannot form a cycle.
    using D3D11ContextKey = std::pair<ID3D11Device*, IUnknown*>;
    std::mutex g_d3d11ContextsLock;
    std::map<D3D11ContextKey, GuestD3D11Context*> g_d3d11Contexts;

    class GuestD3D11Context final : public ID3D11DeviceContext4, public GuestGraphicsObject
    {
    public:
        GuestD3D11Context(ComPtr<ID3D11DeviceContext> native, const D3D11ContextKey& key)
            : m_native(std::move(native)), m_device(key.first), m_video(this, m_native.Get(), key.first), m_key(key)
        {
            m_native.As(&m_context1);
            m_native.As(&m_context2);
            m_native.As(&m_context3);
            m_native.As(&m_context4);
        }

        HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void** output) override
        {
            if (!output) return E_POINTER;
            *output = nullptr;
            if (iid == __uuidof(GuestGraphicsObject))
                *output = static_cast<GuestGraphicsObject*>(this);
            else if (m_video.Supports(iid)) *output = static_cast<ID3D11VideoContext3*>(&m_video);
            else if (iid == __uuidof(IUnknown) || iid == __uuidof(ID3D11DeviceChild) ||
                iid == __uuidof(ID3D11DeviceContext) ||
                (iid == __uuidof(ID3D11DeviceContext1) && m_context1) ||
                (iid == __uuidof(ID3D11DeviceContext2) && m_context2) ||
                (iid == __uuidof(ID3D11DeviceContext3) && m_context3) ||
                (iid == __uuidof(ID3D11DeviceContext4) && m_context4))
                *output = static_cast<ID3D11DeviceContext4*>(this);
            else return m_native->QueryInterface(iid, output);
            AddRef();
            return S_OK;
        }

        ULONG STDMETHODCALLTYPE AddRef() override { return ++m_refs; }
        ULONG STDMETHODCALLTYPE Release() override
        {
            ULONG refs;
            {
                std::lock_guard<std::mutex> guard(g_d3d11ContextsLock);
                refs = --m_refs;
                if (!refs)
                {
                    const auto entry = g_d3d11Contexts.find(m_key);
                    if (entry != g_d3d11Contexts.end() && entry->second == this) g_d3d11Contexts.erase(entry);
                }
            }
            if (!refs) delete this;
            return refs;
        }

        IUnknown* STDMETHODCALLTYPE NativeObject() override { return m_native.Get(); }

        void STDMETHODCALLTYPE GetDevice(ID3D11Device** output) override
        {
            if (output) m_device.CopyTo(output);
        }

        HRESULT STDMETHODCALLTYPE GetPrivateData(REFGUID guid, UINT* pDataSize, void* pData) override
        { return m_native->GetPrivateData(guid, pDataSize, pData); }

        HRESULT STDMETHODCALLTYPE SetPrivateData(REFGUID guid, UINT DataSize, const void* pData) override
        { return m_native->SetPrivateData(guid, DataSize, pData); }

        HRESULT STDMETHODCALLTYPE SetPrivateDataInterface(REFGUID guid, const IUnknown* pData) override
        { return m_native->SetPrivateDataInterface(guid, pData); }

        void STDMETHODCALLTYPE VSSetConstantBuffers(UINT StartSlot, UINT NumBuffers,
            ID3D11Buffer*const* ppConstantBuffers) override
        { SetD3D11Bindings(NumBuffers, ppConstantBuffers, [&](auto native)
            { m_native->VSSetConstantBuffers(StartSlot, NumBuffers, native); }); }

        void STDMETHODCALLTYPE PSSetShaderResources(UINT StartSlot, UINT NumViews,
            ID3D11ShaderResourceView*const* ppShaderResourceViews) override
        { SetD3D11Bindings(NumViews, ppShaderResourceViews, [&](auto native)
            { m_native->PSSetShaderResources(StartSlot, NumViews, native); }); }

        void STDMETHODCALLTYPE PSSetShader(ID3D11PixelShader* pPixelShader,
            ID3D11ClassInstance*const* ppClassInstances, UINT NumClassInstances) override
        { m_native->PSSetShader(pPixelShader, ppClassInstances, NumClassInstances); }

        void STDMETHODCALLTYPE PSSetSamplers(UINT StartSlot, UINT NumSamplers,
            ID3D11SamplerState*const* ppSamplers) override
        { m_native->PSSetSamplers(StartSlot, NumSamplers, ppSamplers); }

        void STDMETHODCALLTYPE VSSetShader(ID3D11VertexShader* pVertexShader,
            ID3D11ClassInstance*const* ppClassInstances, UINT NumClassInstances) override
        { m_native->VSSetShader(pVertexShader, ppClassInstances, NumClassInstances); }

        void STDMETHODCALLTYPE DrawIndexed(UINT IndexCount, UINT StartIndexLocation, INT BaseVertexLocation) override
        { m_native->DrawIndexed(IndexCount, StartIndexLocation, BaseVertexLocation); }

        void STDMETHODCALLTYPE Draw(UINT VertexCount, UINT StartVertexLocation) override
        { m_native->Draw(VertexCount, StartVertexLocation); }

        HRESULT STDMETHODCALLTYPE Map(ID3D11Resource* pResource, UINT Subresource, D3D11_MAP MapType,
            UINT MapFlags, D3D11_MAPPED_SUBRESOURCE* pMappedResource) override
        { return m_native->Map(NativeD3D11Child(pResource), Subresource, MapType, MapFlags, pMappedResource); }

        void STDMETHODCALLTYPE Unmap(ID3D11Resource* pResource, UINT Subresource) override
        { m_native->Unmap(NativeD3D11Child(pResource), Subresource); }

        void STDMETHODCALLTYPE PSSetConstantBuffers(UINT StartSlot, UINT NumBuffers,
            ID3D11Buffer*const* ppConstantBuffers) override
        { SetD3D11Bindings(NumBuffers, ppConstantBuffers, [&](auto native)
            { m_native->PSSetConstantBuffers(StartSlot, NumBuffers, native); }); }

        void STDMETHODCALLTYPE IASetInputLayout(ID3D11InputLayout* pInputLayout) override
        { m_native->IASetInputLayout(pInputLayout); }

        void STDMETHODCALLTYPE IASetVertexBuffers(UINT StartSlot, UINT NumBuffers,
            ID3D11Buffer*const* ppVertexBuffers, const UINT* pStrides, const UINT* pOffsets) override
        { SetD3D11Bindings(NumBuffers, ppVertexBuffers, [&](auto native)
            { m_native->IASetVertexBuffers(StartSlot, NumBuffers, native, pStrides, pOffsets); }); }

        void STDMETHODCALLTYPE IASetIndexBuffer(ID3D11Buffer* pIndexBuffer, DXGI_FORMAT Format, UINT Offset) override
        { m_native->IASetIndexBuffer(NativeD3D11Child(pIndexBuffer), Format, Offset); }

        void STDMETHODCALLTYPE DrawIndexedInstanced(UINT IndexCountPerInstance, UINT InstanceCount,
            UINT StartIndexLocation, INT BaseVertexLocation, UINT StartInstanceLocation) override
        { m_native->DrawIndexedInstanced(IndexCountPerInstance, InstanceCount, StartIndexLocation,
            BaseVertexLocation, StartInstanceLocation); }

        void STDMETHODCALLTYPE DrawInstanced(UINT VertexCountPerInstance, UINT InstanceCount,
            UINT StartVertexLocation, UINT StartInstanceLocation) override
        { m_native->DrawInstanced(VertexCountPerInstance, InstanceCount, StartVertexLocation, StartInstanceLocation); }

        void STDMETHODCALLTYPE GSSetConstantBuffers(UINT StartSlot, UINT NumBuffers,
            ID3D11Buffer*const* ppConstantBuffers) override
        { SetD3D11Bindings(NumBuffers, ppConstantBuffers, [&](auto native)
            { m_native->GSSetConstantBuffers(StartSlot, NumBuffers, native); }); }

        void STDMETHODCALLTYPE GSSetShader(ID3D11GeometryShader* pShader,
            ID3D11ClassInstance*const* ppClassInstances, UINT NumClassInstances) override
        { m_native->GSSetShader(pShader, ppClassInstances, NumClassInstances); }

        void STDMETHODCALLTYPE IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY Topology) override
        { m_native->IASetPrimitiveTopology(Topology); }

        void STDMETHODCALLTYPE VSSetShaderResources(UINT StartSlot, UINT NumViews,
            ID3D11ShaderResourceView*const* ppShaderResourceViews) override
        { SetD3D11Bindings(NumViews, ppShaderResourceViews, [&](auto native)
            { m_native->VSSetShaderResources(StartSlot, NumViews, native); }); }

        void STDMETHODCALLTYPE VSSetSamplers(UINT StartSlot, UINT NumSamplers,
            ID3D11SamplerState*const* ppSamplers) override
        { m_native->VSSetSamplers(StartSlot, NumSamplers, ppSamplers); }

        void STDMETHODCALLTYPE Begin(ID3D11Asynchronous* pAsync) override
        { m_native->Begin(pAsync); }

        void STDMETHODCALLTYPE End(ID3D11Asynchronous* pAsync) override
        { m_native->End(pAsync); }

        HRESULT STDMETHODCALLTYPE GetData(ID3D11Asynchronous* pAsync, void* pData, UINT DataSize,
            UINT GetDataFlags) override
        { return m_native->GetData(pAsync, pData, DataSize, GetDataFlags); }

        void STDMETHODCALLTYPE SetPredication(ID3D11Predicate* pPredicate, BOOL PredicateValue) override
        { m_native->SetPredication(pPredicate, PredicateValue); }

        void STDMETHODCALLTYPE GSSetShaderResources(UINT StartSlot, UINT NumViews,
            ID3D11ShaderResourceView*const* ppShaderResourceViews) override
        { SetD3D11Bindings(NumViews, ppShaderResourceViews, [&](auto native)
            { m_native->GSSetShaderResources(StartSlot, NumViews, native); }); }

        void STDMETHODCALLTYPE GSSetSamplers(UINT StartSlot, UINT NumSamplers,
            ID3D11SamplerState*const* ppSamplers) override
        { m_native->GSSetSamplers(StartSlot, NumSamplers, ppSamplers); }

        void STDMETHODCALLTYPE OMSetRenderTargets(UINT NumViews,
            ID3D11RenderTargetView*const* ppRenderTargetViews, ID3D11DepthStencilView* pDepthStencilView) override
        { SetD3D11Bindings(NumViews, ppRenderTargetViews, [&](auto native)
            { m_native->OMSetRenderTargets(NumViews, native, NativeD3D11Child(pDepthStencilView)); }); }

        void STDMETHODCALLTYPE OMSetRenderTargetsAndUnorderedAccessViews(UINT NumRTVs,
            ID3D11RenderTargetView*const* ppRenderTargetViews, ID3D11DepthStencilView* pDepthStencilView,
            UINT UAVStartSlot, UINT NumUAVs, ID3D11UnorderedAccessView*const* ppUnorderedAccessViews,
            const UINT* pUAVInitialCounts) override
        {
            const bool keepTargets = NumRTVs == D3D11_KEEP_RENDER_TARGETS_AND_DEPTH_STENCIL;
            const bool keepUavs = NumUAVs == D3D11_KEEP_UNORDERED_ACCESS_VIEWS;
            SetD3D11Bindings(keepTargets ? 0u : NumRTVs, keepTargets ? nullptr : ppRenderTargetViews, [&](auto rtvs)
            {
                SetD3D11Bindings(keepUavs ? 0u : NumUAVs, keepUavs ? nullptr : ppUnorderedAccessViews, [&](auto uavs)
                {
                    m_native->OMSetRenderTargetsAndUnorderedAccessViews(NumRTVs, rtvs,
                        keepTargets ? nullptr : NativeD3D11Child(pDepthStencilView), UAVStartSlot, NumUAVs,
                        uavs, keepUavs ? nullptr : pUAVInitialCounts);
                });
            });
        }

        void STDMETHODCALLTYPE OMSetBlendState(ID3D11BlendState* pBlendState, const FLOAT BlendFactor[ 4 ],
            UINT SampleMask) override
        { m_native->OMSetBlendState(pBlendState, BlendFactor, SampleMask); }

        void STDMETHODCALLTYPE OMSetDepthStencilState(ID3D11DepthStencilState* pDepthStencilState,
            UINT StencilRef) override
        { m_native->OMSetDepthStencilState(pDepthStencilState, StencilRef); }

        void STDMETHODCALLTYPE SOSetTargets(UINT NumBuffers, ID3D11Buffer*const* ppSOTargets,
            const UINT* pOffsets) override
        { SetD3D11Bindings(NumBuffers, ppSOTargets, [&](auto native)
            { m_native->SOSetTargets(NumBuffers, native, pOffsets); }); }

        void STDMETHODCALLTYPE DrawAuto() override
        { m_native->DrawAuto(); }

        void STDMETHODCALLTYPE DrawIndexedInstancedIndirect(ID3D11Buffer* pBufferForArgs,
            UINT AlignedByteOffsetForArgs) override
        { m_native->DrawIndexedInstancedIndirect(NativeD3D11Child(pBufferForArgs), AlignedByteOffsetForArgs); }

        void STDMETHODCALLTYPE DrawInstancedIndirect(ID3D11Buffer* pBufferForArgs, UINT AlignedByteOffsetForArgs) override
        { m_native->DrawInstancedIndirect(NativeD3D11Child(pBufferForArgs), AlignedByteOffsetForArgs); }

        void STDMETHODCALLTYPE Dispatch(UINT ThreadGroupCountX, UINT ThreadGroupCountY, UINT ThreadGroupCountZ) override
        { m_native->Dispatch(ThreadGroupCountX, ThreadGroupCountY, ThreadGroupCountZ); }

        void STDMETHODCALLTYPE DispatchIndirect(ID3D11Buffer* pBufferForArgs, UINT AlignedByteOffsetForArgs) override
        { m_native->DispatchIndirect(NativeD3D11Child(pBufferForArgs), AlignedByteOffsetForArgs); }

        void STDMETHODCALLTYPE RSSetState(ID3D11RasterizerState* pRasterizerState) override
        { m_native->RSSetState(pRasterizerState); }

        void STDMETHODCALLTYPE RSSetViewports(UINT NumViewports, const D3D11_VIEWPORT* pViewports) override
        { m_native->RSSetViewports(NumViewports, pViewports); }

        void STDMETHODCALLTYPE RSSetScissorRects(UINT NumRects, const D3D11_RECT* pRects) override
        { m_native->RSSetScissorRects(NumRects, pRects); }

        void STDMETHODCALLTYPE CopySubresourceRegion(ID3D11Resource* pDstResource, UINT DstSubresource,
            UINT DstX, UINT DstY, UINT DstZ, ID3D11Resource* pSrcResource, UINT SrcSubresource,
            const D3D11_BOX* pSrcBox) override
        { m_native->CopySubresourceRegion(NativeD3D11Child(pDstResource), DstSubresource, DstX, DstY, DstZ, NativeD3D11Child(pSrcResource),
            SrcSubresource, pSrcBox); }

        void STDMETHODCALLTYPE CopyResource(ID3D11Resource* pDstResource, ID3D11Resource* pSrcResource) override
        { m_native->CopyResource(NativeD3D11Child(pDstResource), NativeD3D11Child(pSrcResource)); }

        void STDMETHODCALLTYPE UpdateSubresource(ID3D11Resource* pDstResource, UINT DstSubresource,
            const D3D11_BOX* pDstBox, const void* pSrcData, UINT SrcRowPitch, UINT SrcDepthPitch) override
        { m_native->UpdateSubresource(NativeD3D11Child(pDstResource), DstSubresource, pDstBox, pSrcData, SrcRowPitch, SrcDepthPitch); }

        void STDMETHODCALLTYPE CopyStructureCount(ID3D11Buffer* pDstBuffer, UINT DstAlignedByteOffset,
            ID3D11UnorderedAccessView* pSrcView) override
        { m_native->CopyStructureCount(NativeD3D11Child(pDstBuffer), DstAlignedByteOffset, NativeD3D11Child(pSrcView)); }

        void STDMETHODCALLTYPE ClearRenderTargetView(ID3D11RenderTargetView* pRenderTargetView,
            const FLOAT ColorRGBA[ 4 ]) override
        { m_native->ClearRenderTargetView(NativeD3D11Child(pRenderTargetView), ColorRGBA); }

        void STDMETHODCALLTYPE ClearUnorderedAccessViewUint(ID3D11UnorderedAccessView* pUnorderedAccessView,
            const UINT Values[ 4 ]) override
        { m_native->ClearUnorderedAccessViewUint(NativeD3D11Child(pUnorderedAccessView), Values); }

        void STDMETHODCALLTYPE ClearUnorderedAccessViewFloat(ID3D11UnorderedAccessView* pUnorderedAccessView,
            const FLOAT Values[ 4 ]) override
        { m_native->ClearUnorderedAccessViewFloat(NativeD3D11Child(pUnorderedAccessView), Values); }

        void STDMETHODCALLTYPE ClearDepthStencilView(ID3D11DepthStencilView* pDepthStencilView, UINT ClearFlags,
            FLOAT Depth, UINT8 Stencil) override
        { m_native->ClearDepthStencilView(NativeD3D11Child(pDepthStencilView), ClearFlags, Depth, Stencil); }

        void STDMETHODCALLTYPE GenerateMips(ID3D11ShaderResourceView* pShaderResourceView) override
        { m_native->GenerateMips(NativeD3D11Child(pShaderResourceView)); }

        void STDMETHODCALLTYPE SetResourceMinLOD(ID3D11Resource* pResource, FLOAT MinLOD) override
        { m_native->SetResourceMinLOD(NativeD3D11Child(pResource), MinLOD); }

        FLOAT STDMETHODCALLTYPE GetResourceMinLOD(ID3D11Resource* pResource) override
        { return m_native->GetResourceMinLOD(NativeD3D11Child(pResource)); }

        void STDMETHODCALLTYPE ResolveSubresource(ID3D11Resource* pDstResource, UINT DstSubresource,
            ID3D11Resource* pSrcResource, UINT SrcSubresource, DXGI_FORMAT Format) override
        { m_native->ResolveSubresource(NativeD3D11Child(pDstResource), DstSubresource, NativeD3D11Child(pSrcResource), SrcSubresource, Format); }

        void STDMETHODCALLTYPE ExecuteCommandList(ID3D11CommandList* pCommandList, BOOL RestoreContextState) override
        { m_native->ExecuteCommandList(pCommandList, RestoreContextState); }

        void STDMETHODCALLTYPE HSSetShaderResources(UINT StartSlot, UINT NumViews,
            ID3D11ShaderResourceView*const* ppShaderResourceViews) override
        { SetD3D11Bindings(NumViews, ppShaderResourceViews, [&](auto native)
            { m_native->HSSetShaderResources(StartSlot, NumViews, native); }); }

        void STDMETHODCALLTYPE HSSetShader(ID3D11HullShader* pHullShader,
            ID3D11ClassInstance*const* ppClassInstances, UINT NumClassInstances) override
        { m_native->HSSetShader(pHullShader, ppClassInstances, NumClassInstances); }

        void STDMETHODCALLTYPE HSSetSamplers(UINT StartSlot, UINT NumSamplers,
            ID3D11SamplerState*const* ppSamplers) override
        { m_native->HSSetSamplers(StartSlot, NumSamplers, ppSamplers); }

        void STDMETHODCALLTYPE HSSetConstantBuffers(UINT StartSlot, UINT NumBuffers,
            ID3D11Buffer*const* ppConstantBuffers) override
        { SetD3D11Bindings(NumBuffers, ppConstantBuffers, [&](auto native)
            { m_native->HSSetConstantBuffers(StartSlot, NumBuffers, native); }); }

        void STDMETHODCALLTYPE DSSetShaderResources(UINT StartSlot, UINT NumViews,
            ID3D11ShaderResourceView*const* ppShaderResourceViews) override
        { SetD3D11Bindings(NumViews, ppShaderResourceViews, [&](auto native)
            { m_native->DSSetShaderResources(StartSlot, NumViews, native); }); }

        void STDMETHODCALLTYPE DSSetShader(ID3D11DomainShader* pDomainShader,
            ID3D11ClassInstance*const* ppClassInstances, UINT NumClassInstances) override
        { m_native->DSSetShader(pDomainShader, ppClassInstances, NumClassInstances); }

        void STDMETHODCALLTYPE DSSetSamplers(UINT StartSlot, UINT NumSamplers,
            ID3D11SamplerState*const* ppSamplers) override
        { m_native->DSSetSamplers(StartSlot, NumSamplers, ppSamplers); }

        void STDMETHODCALLTYPE DSSetConstantBuffers(UINT StartSlot, UINT NumBuffers,
            ID3D11Buffer*const* ppConstantBuffers) override
        { SetD3D11Bindings(NumBuffers, ppConstantBuffers, [&](auto native)
            { m_native->DSSetConstantBuffers(StartSlot, NumBuffers, native); }); }

        void STDMETHODCALLTYPE CSSetShaderResources(UINT StartSlot, UINT NumViews,
            ID3D11ShaderResourceView*const* ppShaderResourceViews) override
        { SetD3D11Bindings(NumViews, ppShaderResourceViews, [&](auto native)
            { m_native->CSSetShaderResources(StartSlot, NumViews, native); }); }

        void STDMETHODCALLTYPE CSSetUnorderedAccessViews(UINT StartSlot, UINT NumUAVs,
            ID3D11UnorderedAccessView*const* ppUnorderedAccessViews, const UINT* pUAVInitialCounts) override
        { SetD3D11Bindings(NumUAVs, ppUnorderedAccessViews, [&](auto native)
            { m_native->CSSetUnorderedAccessViews(StartSlot, NumUAVs, native, pUAVInitialCounts); }); }

        void STDMETHODCALLTYPE CSSetShader(ID3D11ComputeShader* pComputeShader,
            ID3D11ClassInstance*const* ppClassInstances, UINT NumClassInstances) override
        { m_native->CSSetShader(pComputeShader, ppClassInstances, NumClassInstances); }

        void STDMETHODCALLTYPE CSSetSamplers(UINT StartSlot, UINT NumSamplers,
            ID3D11SamplerState*const* ppSamplers) override
        { m_native->CSSetSamplers(StartSlot, NumSamplers, ppSamplers); }

        void STDMETHODCALLTYPE CSSetConstantBuffers(UINT StartSlot, UINT NumBuffers,
            ID3D11Buffer*const* ppConstantBuffers) override
        { SetD3D11Bindings(NumBuffers, ppConstantBuffers, [&](auto native)
            { m_native->CSSetConstantBuffers(StartSlot, NumBuffers, native); }); }

        void STDMETHODCALLTYPE VSGetConstantBuffers(UINT StartSlot, UINT NumBuffers,
            ID3D11Buffer** ppConstantBuffers) override
        { GetD3D11Bindings(m_device.Get(), NumBuffers, ppConstantBuffers, [&](auto native)
            { m_native->VSGetConstantBuffers(StartSlot, NumBuffers, native); }); }

        void STDMETHODCALLTYPE PSGetShaderResources(UINT StartSlot, UINT NumViews,
            ID3D11ShaderResourceView** ppShaderResourceViews) override
        { GetD3D11Bindings(m_device.Get(), NumViews, ppShaderResourceViews, [&](auto native)
            { m_native->PSGetShaderResources(StartSlot, NumViews, native); }); }

        void STDMETHODCALLTYPE PSGetShader(ID3D11PixelShader** ppPixelShader,
            ID3D11ClassInstance** ppClassInstances, UINT* pNumClassInstances) override
        { m_native->PSGetShader(ppPixelShader, ppClassInstances, pNumClassInstances); }

        void STDMETHODCALLTYPE PSGetSamplers(UINT StartSlot, UINT NumSamplers, ID3D11SamplerState** ppSamplers) override
        { m_native->PSGetSamplers(StartSlot, NumSamplers, ppSamplers); }

        void STDMETHODCALLTYPE VSGetShader(ID3D11VertexShader** ppVertexShader,
            ID3D11ClassInstance** ppClassInstances, UINT* pNumClassInstances) override
        { m_native->VSGetShader(ppVertexShader, ppClassInstances, pNumClassInstances); }

        void STDMETHODCALLTYPE PSGetConstantBuffers(UINT StartSlot, UINT NumBuffers,
            ID3D11Buffer** ppConstantBuffers) override
        { GetD3D11Bindings(m_device.Get(), NumBuffers, ppConstantBuffers, [&](auto native)
            { m_native->PSGetConstantBuffers(StartSlot, NumBuffers, native); }); }

        void STDMETHODCALLTYPE IAGetInputLayout(ID3D11InputLayout** ppInputLayout) override
        { m_native->IAGetInputLayout(ppInputLayout); }

        void STDMETHODCALLTYPE IAGetVertexBuffers(UINT StartSlot, UINT NumBuffers,
            ID3D11Buffer** ppVertexBuffers, UINT* pStrides, UINT* pOffsets) override
        { GetD3D11Bindings(m_device.Get(), NumBuffers, ppVertexBuffers, [&](auto native)
            { m_native->IAGetVertexBuffers(StartSlot, NumBuffers, native, pStrides, pOffsets); }); }

        void STDMETHODCALLTYPE IAGetIndexBuffer(ID3D11Buffer** pIndexBuffer, DXGI_FORMAT* Format, UINT* Offset) override
        { GetD3D11Bindings(m_device.Get(), 1, pIndexBuffer, [&](auto native)
            { m_native->IAGetIndexBuffer(native, Format, Offset); }); }

        void STDMETHODCALLTYPE GSGetConstantBuffers(UINT StartSlot, UINT NumBuffers,
            ID3D11Buffer** ppConstantBuffers) override
        { GetD3D11Bindings(m_device.Get(), NumBuffers, ppConstantBuffers, [&](auto native)
            { m_native->GSGetConstantBuffers(StartSlot, NumBuffers, native); }); }

        void STDMETHODCALLTYPE GSGetShader(ID3D11GeometryShader** ppGeometryShader,
            ID3D11ClassInstance** ppClassInstances, UINT* pNumClassInstances) override
        { m_native->GSGetShader(ppGeometryShader, ppClassInstances, pNumClassInstances); }

        void STDMETHODCALLTYPE IAGetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY* pTopology) override
        { m_native->IAGetPrimitiveTopology(pTopology); }

        void STDMETHODCALLTYPE VSGetShaderResources(UINT StartSlot, UINT NumViews,
            ID3D11ShaderResourceView** ppShaderResourceViews) override
        { GetD3D11Bindings(m_device.Get(), NumViews, ppShaderResourceViews, [&](auto native)
            { m_native->VSGetShaderResources(StartSlot, NumViews, native); }); }

        void STDMETHODCALLTYPE VSGetSamplers(UINT StartSlot, UINT NumSamplers, ID3D11SamplerState** ppSamplers) override
        { m_native->VSGetSamplers(StartSlot, NumSamplers, ppSamplers); }

        void STDMETHODCALLTYPE GetPredication(ID3D11Predicate** ppPredicate, BOOL* pPredicateValue) override
        { m_native->GetPredication(ppPredicate, pPredicateValue); }

        void STDMETHODCALLTYPE GSGetShaderResources(UINT StartSlot, UINT NumViews,
            ID3D11ShaderResourceView** ppShaderResourceViews) override
        { GetD3D11Bindings(m_device.Get(), NumViews, ppShaderResourceViews, [&](auto native)
            { m_native->GSGetShaderResources(StartSlot, NumViews, native); }); }

        void STDMETHODCALLTYPE GSGetSamplers(UINT StartSlot, UINT NumSamplers, ID3D11SamplerState** ppSamplers) override
        { m_native->GSGetSamplers(StartSlot, NumSamplers, ppSamplers); }

        void STDMETHODCALLTYPE OMGetRenderTargets(UINT NumViews, ID3D11RenderTargetView** ppRenderTargetViews,
            ID3D11DepthStencilView** ppDepthStencilView) override
        {
            D3D11BindingOutputs<ID3D11RenderTargetView> rtvs(NumViews, ppRenderTargetViews);
            D3D11BindingOutputs<ID3D11DepthStencilView> dsv(1, ppDepthStencilView);
            if (!rtvs.Valid()) return;
            m_native->OMGetRenderTargets(NumViews, rtvs.NativeOutputs(), dsv.NativeOutputs());
            rtvs.Publish(m_device.Get());
            dsv.Publish(m_device.Get());
        }

        void STDMETHODCALLTYPE OMGetRenderTargetsAndUnorderedAccessViews(UINT NumRTVs,
            ID3D11RenderTargetView** ppRenderTargetViews, ID3D11DepthStencilView** ppDepthStencilView,
            UINT UAVStartSlot, UINT NumUAVs, ID3D11UnorderedAccessView** ppUnorderedAccessViews) override
        {
            D3D11BindingOutputs<ID3D11RenderTargetView> rtvs(NumRTVs, ppRenderTargetViews);
            D3D11BindingOutputs<ID3D11DepthStencilView> dsv(1, ppDepthStencilView);
            D3D11BindingOutputs<ID3D11UnorderedAccessView> uavs(NumUAVs, ppUnorderedAccessViews);
            if (!rtvs.Valid() || !uavs.Valid()) return;
            m_native->OMGetRenderTargetsAndUnorderedAccessViews(NumRTVs, rtvs.NativeOutputs(),
                dsv.NativeOutputs(), UAVStartSlot, NumUAVs, uavs.NativeOutputs());
            rtvs.Publish(m_device.Get());
            dsv.Publish(m_device.Get());
            uavs.Publish(m_device.Get());
        }

        void STDMETHODCALLTYPE OMGetBlendState(ID3D11BlendState** ppBlendState, FLOAT BlendFactor[ 4 ],
            UINT* pSampleMask) override
        { m_native->OMGetBlendState(ppBlendState, BlendFactor, pSampleMask); }

        void STDMETHODCALLTYPE OMGetDepthStencilState(ID3D11DepthStencilState** ppDepthStencilState,
            UINT* pStencilRef) override
        { m_native->OMGetDepthStencilState(ppDepthStencilState, pStencilRef); }

        void STDMETHODCALLTYPE SOGetTargets(UINT NumBuffers, ID3D11Buffer** ppSOTargets) override
        { GetD3D11Bindings(m_device.Get(), NumBuffers, ppSOTargets, [&](auto native)
            { m_native->SOGetTargets(NumBuffers, native); }); }

        void STDMETHODCALLTYPE RSGetState(ID3D11RasterizerState** ppRasterizerState) override
        { m_native->RSGetState(ppRasterizerState); }

        void STDMETHODCALLTYPE RSGetViewports(UINT* pNumViewports, D3D11_VIEWPORT* pViewports) override
        { m_native->RSGetViewports(pNumViewports, pViewports); }

        void STDMETHODCALLTYPE RSGetScissorRects(UINT* pNumRects, D3D11_RECT* pRects) override
        { m_native->RSGetScissorRects(pNumRects, pRects); }

        void STDMETHODCALLTYPE HSGetShaderResources(UINT StartSlot, UINT NumViews,
            ID3D11ShaderResourceView** ppShaderResourceViews) override
        { GetD3D11Bindings(m_device.Get(), NumViews, ppShaderResourceViews, [&](auto native)
            { m_native->HSGetShaderResources(StartSlot, NumViews, native); }); }

        void STDMETHODCALLTYPE HSGetShader(ID3D11HullShader** ppHullShader,
            ID3D11ClassInstance** ppClassInstances, UINT* pNumClassInstances) override
        { m_native->HSGetShader(ppHullShader, ppClassInstances, pNumClassInstances); }

        void STDMETHODCALLTYPE HSGetSamplers(UINT StartSlot, UINT NumSamplers, ID3D11SamplerState** ppSamplers) override
        { m_native->HSGetSamplers(StartSlot, NumSamplers, ppSamplers); }

        void STDMETHODCALLTYPE HSGetConstantBuffers(UINT StartSlot, UINT NumBuffers,
            ID3D11Buffer** ppConstantBuffers) override
        { GetD3D11Bindings(m_device.Get(), NumBuffers, ppConstantBuffers, [&](auto native)
            { m_native->HSGetConstantBuffers(StartSlot, NumBuffers, native); }); }

        void STDMETHODCALLTYPE DSGetShaderResources(UINT StartSlot, UINT NumViews,
            ID3D11ShaderResourceView** ppShaderResourceViews) override
        { GetD3D11Bindings(m_device.Get(), NumViews, ppShaderResourceViews, [&](auto native)
            { m_native->DSGetShaderResources(StartSlot, NumViews, native); }); }

        void STDMETHODCALLTYPE DSGetShader(ID3D11DomainShader** ppDomainShader,
            ID3D11ClassInstance** ppClassInstances, UINT* pNumClassInstances) override
        { m_native->DSGetShader(ppDomainShader, ppClassInstances, pNumClassInstances); }

        void STDMETHODCALLTYPE DSGetSamplers(UINT StartSlot, UINT NumSamplers, ID3D11SamplerState** ppSamplers) override
        { m_native->DSGetSamplers(StartSlot, NumSamplers, ppSamplers); }

        void STDMETHODCALLTYPE DSGetConstantBuffers(UINT StartSlot, UINT NumBuffers,
            ID3D11Buffer** ppConstantBuffers) override
        { GetD3D11Bindings(m_device.Get(), NumBuffers, ppConstantBuffers, [&](auto native)
            { m_native->DSGetConstantBuffers(StartSlot, NumBuffers, native); }); }

        void STDMETHODCALLTYPE CSGetShaderResources(UINT StartSlot, UINT NumViews,
            ID3D11ShaderResourceView** ppShaderResourceViews) override
        { GetD3D11Bindings(m_device.Get(), NumViews, ppShaderResourceViews, [&](auto native)
            { m_native->CSGetShaderResources(StartSlot, NumViews, native); }); }

        void STDMETHODCALLTYPE CSGetUnorderedAccessViews(UINT StartSlot, UINT NumUAVs,
            ID3D11UnorderedAccessView** ppUnorderedAccessViews) override
        { GetD3D11Bindings(m_device.Get(), NumUAVs, ppUnorderedAccessViews, [&](auto native)
            { m_native->CSGetUnorderedAccessViews(StartSlot, NumUAVs, native); }); }

        void STDMETHODCALLTYPE CSGetShader(ID3D11ComputeShader** ppComputeShader,
            ID3D11ClassInstance** ppClassInstances, UINT* pNumClassInstances) override
        { m_native->CSGetShader(ppComputeShader, ppClassInstances, pNumClassInstances); }

        void STDMETHODCALLTYPE CSGetSamplers(UINT StartSlot, UINT NumSamplers, ID3D11SamplerState** ppSamplers) override
        { m_native->CSGetSamplers(StartSlot, NumSamplers, ppSamplers); }

        void STDMETHODCALLTYPE CSGetConstantBuffers(UINT StartSlot, UINT NumBuffers,
            ID3D11Buffer** ppConstantBuffers) override
        { GetD3D11Bindings(m_device.Get(), NumBuffers, ppConstantBuffers, [&](auto native)
            { m_native->CSGetConstantBuffers(StartSlot, NumBuffers, native); }); }

        void STDMETHODCALLTYPE ClearState() override
        { m_native->ClearState(); }

        void STDMETHODCALLTYPE Flush() override
        { m_native->Flush(); }

        D3D11_DEVICE_CONTEXT_TYPE STDMETHODCALLTYPE GetType() override
        { return m_native->GetType(); }

        UINT STDMETHODCALLTYPE GetContextFlags() override
        { return m_native->GetContextFlags(); }

        HRESULT STDMETHODCALLTYPE FinishCommandList(BOOL RestoreDeferredContextState,
            ID3D11CommandList** ppCommandList) override
        { return m_native->FinishCommandList(RestoreDeferredContextState, ppCommandList); }

        void STDMETHODCALLTYPE CopySubresourceRegion1(ID3D11Resource* pDstResource, UINT DstSubresource,
            UINT DstX, UINT DstY, UINT DstZ, ID3D11Resource* pSrcResource, UINT SrcSubresource,
            const D3D11_BOX* pSrcBox, UINT CopyFlags) override
        { m_context1->CopySubresourceRegion1(NativeD3D11Child(pDstResource), DstSubresource, DstX, DstY, DstZ, NativeD3D11Child(pSrcResource),
            SrcSubresource, pSrcBox, CopyFlags); }

        void STDMETHODCALLTYPE UpdateSubresource1(ID3D11Resource* pDstResource, UINT DstSubresource,
            const D3D11_BOX* pDstBox, const void* pSrcData, UINT SrcRowPitch, UINT SrcDepthPitch, UINT CopyFlags) override
        { m_context1->UpdateSubresource1(NativeD3D11Child(pDstResource), DstSubresource, pDstBox, pSrcData, SrcRowPitch,
            SrcDepthPitch, CopyFlags); }

        void STDMETHODCALLTYPE DiscardResource(ID3D11Resource* pResource) override
        { m_context1->DiscardResource(NativeD3D11Child(pResource)); }

        void STDMETHODCALLTYPE DiscardView(ID3D11View* pResourceView) override
        { m_context1->DiscardView(NativeD3D11Child(pResourceView)); }

        void STDMETHODCALLTYPE VSSetConstantBuffers1(UINT StartSlot, UINT NumBuffers,
            ID3D11Buffer*const* ppConstantBuffers, const UINT* pFirstConstant, const UINT* pNumConstants) override
        { SetD3D11Bindings(NumBuffers, ppConstantBuffers, [&](auto native)
            { m_context1->VSSetConstantBuffers1(StartSlot, NumBuffers, native, pFirstConstant, pNumConstants); }); }

        void STDMETHODCALLTYPE HSSetConstantBuffers1(UINT StartSlot, UINT NumBuffers,
            ID3D11Buffer*const* ppConstantBuffers, const UINT* pFirstConstant, const UINT* pNumConstants) override
        { SetD3D11Bindings(NumBuffers, ppConstantBuffers, [&](auto native)
            { m_context1->HSSetConstantBuffers1(StartSlot, NumBuffers, native, pFirstConstant, pNumConstants); }); }

        void STDMETHODCALLTYPE DSSetConstantBuffers1(UINT StartSlot, UINT NumBuffers,
            ID3D11Buffer*const* ppConstantBuffers, const UINT* pFirstConstant, const UINT* pNumConstants) override
        { SetD3D11Bindings(NumBuffers, ppConstantBuffers, [&](auto native)
            { m_context1->DSSetConstantBuffers1(StartSlot, NumBuffers, native, pFirstConstant, pNumConstants); }); }

        void STDMETHODCALLTYPE GSSetConstantBuffers1(UINT StartSlot, UINT NumBuffers,
            ID3D11Buffer*const* ppConstantBuffers, const UINT* pFirstConstant, const UINT* pNumConstants) override
        { SetD3D11Bindings(NumBuffers, ppConstantBuffers, [&](auto native)
            { m_context1->GSSetConstantBuffers1(StartSlot, NumBuffers, native, pFirstConstant, pNumConstants); }); }

        void STDMETHODCALLTYPE PSSetConstantBuffers1(UINT StartSlot, UINT NumBuffers,
            ID3D11Buffer*const* ppConstantBuffers, const UINT* pFirstConstant, const UINT* pNumConstants) override
        { SetD3D11Bindings(NumBuffers, ppConstantBuffers, [&](auto native)
            { m_context1->PSSetConstantBuffers1(StartSlot, NumBuffers, native, pFirstConstant, pNumConstants); }); }

        void STDMETHODCALLTYPE CSSetConstantBuffers1(UINT StartSlot, UINT NumBuffers,
            ID3D11Buffer*const* ppConstantBuffers, const UINT* pFirstConstant, const UINT* pNumConstants) override
        { SetD3D11Bindings(NumBuffers, ppConstantBuffers, [&](auto native)
            { m_context1->CSSetConstantBuffers1(StartSlot, NumBuffers, native, pFirstConstant, pNumConstants); }); }

        void STDMETHODCALLTYPE VSGetConstantBuffers1(UINT StartSlot, UINT NumBuffers,
            ID3D11Buffer** ppConstantBuffers, UINT* pFirstConstant, UINT* pNumConstants) override
        { GetD3D11Bindings(m_device.Get(), NumBuffers, ppConstantBuffers, [&](auto native)
            { m_context1->VSGetConstantBuffers1(StartSlot, NumBuffers, native, pFirstConstant, pNumConstants); }); }

        void STDMETHODCALLTYPE HSGetConstantBuffers1(UINT StartSlot, UINT NumBuffers,
            ID3D11Buffer** ppConstantBuffers, UINT* pFirstConstant, UINT* pNumConstants) override
        { GetD3D11Bindings(m_device.Get(), NumBuffers, ppConstantBuffers, [&](auto native)
            { m_context1->HSGetConstantBuffers1(StartSlot, NumBuffers, native, pFirstConstant, pNumConstants); }); }

        void STDMETHODCALLTYPE DSGetConstantBuffers1(UINT StartSlot, UINT NumBuffers,
            ID3D11Buffer** ppConstantBuffers, UINT* pFirstConstant, UINT* pNumConstants) override
        { GetD3D11Bindings(m_device.Get(), NumBuffers, ppConstantBuffers, [&](auto native)
            { m_context1->DSGetConstantBuffers1(StartSlot, NumBuffers, native, pFirstConstant, pNumConstants); }); }

        void STDMETHODCALLTYPE GSGetConstantBuffers1(UINT StartSlot, UINT NumBuffers,
            ID3D11Buffer** ppConstantBuffers, UINT* pFirstConstant, UINT* pNumConstants) override
        { GetD3D11Bindings(m_device.Get(), NumBuffers, ppConstantBuffers, [&](auto native)
            { m_context1->GSGetConstantBuffers1(StartSlot, NumBuffers, native, pFirstConstant, pNumConstants); }); }

        void STDMETHODCALLTYPE PSGetConstantBuffers1(UINT StartSlot, UINT NumBuffers,
            ID3D11Buffer** ppConstantBuffers, UINT* pFirstConstant, UINT* pNumConstants) override
        { GetD3D11Bindings(m_device.Get(), NumBuffers, ppConstantBuffers, [&](auto native)
            { m_context1->PSGetConstantBuffers1(StartSlot, NumBuffers, native, pFirstConstant, pNumConstants); }); }

        void STDMETHODCALLTYPE CSGetConstantBuffers1(UINT StartSlot, UINT NumBuffers,
            ID3D11Buffer** ppConstantBuffers, UINT* pFirstConstant, UINT* pNumConstants) override
        { GetD3D11Bindings(m_device.Get(), NumBuffers, ppConstantBuffers, [&](auto native)
            { m_context1->CSGetConstantBuffers1(StartSlot, NumBuffers, native, pFirstConstant, pNumConstants); }); }

        void STDMETHODCALLTYPE SwapDeviceContextState(ID3DDeviceContextState* pState,
            ID3DDeviceContextState** ppPreviousState) override
        { m_context1->SwapDeviceContextState(pState, ppPreviousState); }

        void STDMETHODCALLTYPE ClearView(ID3D11View* pView, const FLOAT Color[ 4 ], const D3D11_RECT* pRect,
            UINT NumRects) override
        { m_context1->ClearView(NativeD3D11Child(pView), Color, pRect, NumRects); }

        void STDMETHODCALLTYPE DiscardView1(ID3D11View* pResourceView, const D3D11_RECT* pRects, UINT NumRects) override
        { m_context1->DiscardView1(NativeD3D11Child(pResourceView), pRects, NumRects); }

        HRESULT STDMETHODCALLTYPE UpdateTileMappings(ID3D11Resource* pTiledResource,
            UINT NumTiledResourceRegions,
            const D3D11_TILED_RESOURCE_COORDINATE* pTiledResourceRegionStartCoordinates,
            const D3D11_TILE_REGION_SIZE* pTiledResourceRegionSizes, ID3D11Buffer* pTilePool, UINT NumRanges,
            const UINT* pRangeFlags, const UINT* pTilePoolStartOffsets, const UINT* pRangeTileCounts, UINT Flags) override
        { return m_context2->UpdateTileMappings(NativeD3D11Child(pTiledResource), NumTiledResourceRegions,
            pTiledResourceRegionStartCoordinates, pTiledResourceRegionSizes, NativeD3D11Child(pTilePool), NumRanges, pRangeFlags, pTilePoolStartOffsets, pRangeTileCounts, Flags); }

        HRESULT STDMETHODCALLTYPE CopyTileMappings(ID3D11Resource* pDestTiledResource,
            const D3D11_TILED_RESOURCE_COORDINATE* pDestRegionStartCoordinate,
            ID3D11Resource* pSourceTiledResource,
            const D3D11_TILED_RESOURCE_COORDINATE* pSourceRegionStartCoordinate,
            const D3D11_TILE_REGION_SIZE* pTileRegionSize, UINT Flags) override
        { return m_context2->CopyTileMappings(NativeD3D11Child(pDestTiledResource), pDestRegionStartCoordinate, NativeD3D11Child(pSourceTiledResource),
            pSourceRegionStartCoordinate, pTileRegionSize, Flags); }

        void STDMETHODCALLTYPE CopyTiles(ID3D11Resource* pTiledResource,
            const D3D11_TILED_RESOURCE_COORDINATE* pTileRegionStartCoordinate,
            const D3D11_TILE_REGION_SIZE* pTileRegionSize, ID3D11Buffer* pBuffer,
            UINT64 BufferStartOffsetInBytes, UINT Flags) override
        { m_context2->CopyTiles(NativeD3D11Child(pTiledResource), pTileRegionStartCoordinate, pTileRegionSize, NativeD3D11Child(pBuffer),
            BufferStartOffsetInBytes, Flags); }

        void STDMETHODCALLTYPE UpdateTiles(ID3D11Resource* pDestTiledResource,
            const D3D11_TILED_RESOURCE_COORDINATE* pDestTileRegionStartCoordinate,
            const D3D11_TILE_REGION_SIZE* pDestTileRegionSize, const void* pSourceTileData, UINT Flags) override
        { m_context2->UpdateTiles(NativeD3D11Child(pDestTiledResource), pDestTileRegionStartCoordinate, pDestTileRegionSize,
            pSourceTileData, Flags); }

        HRESULT STDMETHODCALLTYPE ResizeTilePool(ID3D11Buffer* pTilePool, UINT64 NewSizeInBytes) override
        { return m_context2->ResizeTilePool(NativeD3D11Child(pTilePool), NewSizeInBytes); }

        void STDMETHODCALLTYPE TiledResourceBarrier(ID3D11DeviceChild* pTiledResourceOrViewAccessBeforeBarrier,
            ID3D11DeviceChild* pTiledResourceOrViewAccessAfterBarrier) override
        { m_context2->TiledResourceBarrier(NativeD3D11Child(pTiledResourceOrViewAccessBeforeBarrier), NativeD3D11Child(pTiledResourceOrViewAccessAfterBarrier)); }

        BOOL STDMETHODCALLTYPE IsAnnotationEnabled() override
        { return m_context2->IsAnnotationEnabled(); }

        void STDMETHODCALLTYPE SetMarkerInt(LPCWSTR pLabel, INT Data) override
        { m_context2->SetMarkerInt(pLabel, Data); }

        void STDMETHODCALLTYPE BeginEventInt(LPCWSTR pLabel, INT Data) override
        { m_context2->BeginEventInt(pLabel, Data); }

        void STDMETHODCALLTYPE EndEvent() override
        { m_context2->EndEvent(); }

        void STDMETHODCALLTYPE Flush1(D3D11_CONTEXT_TYPE ContextType, HANDLE hEvent) override
        {
            const HRESULT registered = RegisterGraphicsEvent(hEvent, [&](HANDLE native)
            {
                m_context3->Flush1(ContextType, native);
                return S_OK;
            });
            if (FAILED(registered)) m_context3->Flush1(ContextType, nullptr);
        }

        void STDMETHODCALLTYPE SetHardwareProtectionState(BOOL HwProtectionEnable) override
        { m_context3->SetHardwareProtectionState(HwProtectionEnable); }

        void STDMETHODCALLTYPE GetHardwareProtectionState(BOOL* pHwProtectionEnable) override
        { m_context3->GetHardwareProtectionState(pHwProtectionEnable); }

        HRESULT STDMETHODCALLTYPE Signal(ID3D11Fence* pFence, UINT64 Value) override
        {
            if (!pFence) return E_INVALIDARG;
            ComPtr<ID3D11Fence> native;
            const HRESULT status = NativeGraphicsObject(pFence).As(&native);
            return FAILED(status) ? status : m_context4->Signal(native.Get(), Value);
        }

        HRESULT STDMETHODCALLTYPE Wait(ID3D11Fence* pFence, UINT64 Value) override
        {
            if (!pFence) return E_INVALIDARG;
            ComPtr<ID3D11Fence> native;
            const HRESULT status = NativeGraphicsObject(pFence).As(&native);
            return FAILED(status) ? status : m_context4->Wait(native.Get(), Value);
        }

    private:
        std::atomic<ULONG> m_refs{1};
        ComPtr<ID3D11DeviceContext> m_native;
        ComPtr<ID3D11DeviceContext1> m_context1;
        ComPtr<ID3D11DeviceContext2> m_context2;
        ComPtr<ID3D11DeviceContext3> m_context3;
        ComPtr<ID3D11DeviceContext4> m_context4;
        ComPtr<ID3D11Device> m_device;
        GuestD3D11VideoContext m_video;
        const D3D11ContextKey m_key;
    };

    template<class Interface>
    HRESULT WrapD3D11Context(ComPtr<Interface> native, ID3D11Device* device, Interface** output)
    {
        if (!output) return E_POINTER;
        *output = nullptr;
        if (!native) return S_OK;
        ComPtr<IUnknown> identity;
        const HRESULT queried = native.As(&identity);
        if (FAILED(queried)) return queried;
        const D3D11ContextKey key{device, identity.Get()};
        ComPtr<ID3D11DeviceContext> context;
        {
            std::lock_guard<std::mutex> guard(g_d3d11ContextsLock);
            const auto found = g_d3d11Contexts.find(key);
            if (found != g_d3d11Contexts.end()) context = found->second;
            else
            {
                auto wrapper = new (std::nothrow) GuestD3D11Context(std::move(native), key);
                if (!wrapper) return E_OUTOFMEMORY;
                context.Attach(wrapper);
                try { g_d3d11Contexts.emplace(key, wrapper); }
                catch (const std::bad_alloc&) { return E_OUTOFMEMORY; }
            }
        }
        return context->QueryInterface(IID_PPV_ARGS(output));
    }
