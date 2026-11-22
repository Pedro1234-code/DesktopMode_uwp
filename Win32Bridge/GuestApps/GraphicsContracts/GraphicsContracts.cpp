#include <windows.h>
#include <d3d11_1.h>
#include <d3d11_3.h>
#include <d3d11on12.h>
#include <d3d12.h>
#include <dxgi1_6.h>
#include <wrl/client.h>
#include <array>
#include <cwchar>
#include <cstdio>
#include <utility>

using Microsoft::WRL::ComPtr;

namespace
{
    bool resizePending = false;
    bool minimized = false;
    bool switchRenderer = false;
    bool legacyD3D11 = false;
    bool verifyD3D11Bindings = false;
    bool verifyD3D11On12 = false;
    bool verifyDxgiSurfaces = false;
    bool verifySharing = false;
    bool verifyD3D11Sharing = false;
    bool useDeviceFactory = false;
    bool verifyDxgiEvents = false;
    bool gateFirstFrame = false;
    bool firstFrameAllowed = false;
    unsigned frame = 0;
    constexpr DXGI_FORMAT Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    constexpr UINT FrameLatencyFlag = DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT;

    struct FrameLatency
    {
        ~FrameLatency() { if (handle) CloseHandle(handle); }

        HRESULT Initialize(IDXGISwapChain* chain)
        {
            ComPtr<IDXGISwapChain2> latency;
            HRESULT status = chain->QueryInterface(IID_PPV_ARGS(&latency));
            if (FAILED(status)) return status;
            if (FAILED(status = latency->SetMaximumFrameLatency(1))) return status;
            UINT maximum = 0;
            if (FAILED(status = latency->GetMaximumFrameLatency(&maximum))) return status;
            if (maximum != 1) return E_FAIL;
            handle = latency->GetFrameLatencyWaitableObject();
            const HANDLE alias = latency->GetFrameLatencyWaitableObject();
            if (!handle || !alias || handle == alias)
            {
                if (alias && alias != handle) CloseHandle(alias);
                return E_HANDLE;
            }
            return CloseHandle(alias) ? S_OK : HRESULT_FROM_WIN32(GetLastError());
        }

        HRESULT Wait() const
        {
            if (!handle) return S_OK;
            const DWORD result = WaitForSingleObjectEx(handle, 5000, FALSE);
            if (result == WAIT_FAILED) return HRESULT_FROM_WIN32(GetLastError());
            return result == WAIT_OBJECT_0 ? S_OK : HRESULT_FROM_WIN32(ERROR_TIMEOUT);
        }

        HANDLE handle = nullptr;
    };

    LRESULT CALLBACK WindowProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam)
    {
        if (message == WM_SIZE) { minimized = wParam == SIZE_MINIMIZED; resizePending = !minimized; }
        if (gateFirstFrame && message == WM_PAINT)
        {
            PAINTSTRUCT paint{};
            const HDC dc = BeginPaint(window, &paint);
            RECT client{};
            GetClientRect(window, &client);
            FillRect(dc, &client, reinterpret_cast<HBRUSH>(static_cast<INT_PTR>(COLOR_WINDOW + 1)));
            DrawTextW(dc, L"Software surface: press Space to present the first GPU frame.", -1,
                &client, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
            EndPaint(window, &paint);
            return 0;
        }
        if (gateFirstFrame && message == WM_KEYDOWN && wParam == VK_SPACE)
        { firstFrameAllowed = true; return 0; }
        if (message == WM_KEYDOWN && wParam == VK_ESCAPE) { DestroyWindow(window); return 0; }
        if (message == WM_KEYDOWN && wParam == VK_F12 && !(lParam & (1LL << 30))) { switchRenderer = true; return 0; }
        if (message == WM_DESTROY) { PostQuitMessage(0); return 0; }
        return DefWindowProcW(window, message, wParam, lParam);
    }

    HRESULT VerifyClientSizedBuffers(HWND window, IDXGISwapChain* chain)
    {
        RECT client{};
        if (!GetClientRect(window, &client)) return E_HANDLE;
        DXGI_SWAP_CHAIN_DESC desc{};
        const HRESULT status = chain->GetDesc(&desc);
        if (FAILED(status)) return status;
        if (desc.BufferDesc.Width != static_cast<UINT>(client.right - client.left) ||
            desc.BufferDesc.Height != static_cast<UINT>(client.bottom - client.top)) return E_FAIL;
        return S_OK;
    }

    bool SameIdentity(IUnknown* first, IUnknown* second)
    {
        ComPtr<IUnknown> left, right;
        return first && second && SUCCEEDED(first->QueryInterface(IID_PPV_ARGS(&left))) &&
            SUCCEEDED(second->QueryInterface(IID_PPV_ARGS(&right))) && left.Get() == right.Get();
    }

    void FrameColor(float (&color)[4])
    {
        color[0] = static_cast<float>(frame++ % 200) / 199.0f;
        color[1] = 0.25f;
        color[2] = 0.65f;
        color[3] = 1.0f;
    }

    constexpr float LabelBackground[] = {0.04f, 0.04f, 0.04f, 1.0f};
    constexpr float LabelForeground[] = {1.0f, 1.0f, 1.0f, 1.0f};
    constexpr RECT LabelBounds{12, 12, 112, 52};

    struct LabelPixels
    {
        explicit LabelPixels(bool d3d12)
        {
            constexpr unsigned d[] = {30, 17, 17, 17, 17, 17, 30};
            constexpr unsigned three[] = {30, 1, 1, 14, 1, 1, 30};
            constexpr unsigned one[] = {4, 12, 4, 4, 4, 4, 14};
            constexpr unsigned two[] = {14, 17, 1, 2, 4, 8, 31};
            const unsigned* glyphs[] = {d, three, d, one, d3d12 ? two : one};
            constexpr LONG scale = 3;
            for (LONG glyph = 0; glyph < 5; ++glyph)
            {
                for (LONG row = 0; row < 7; ++row)
                {
                    for (LONG column = 0; column < 5; ++column)
                    {
                        if (!(glyphs[glyph][row] & (16u >> column))) continue;
                        const LONG x = LabelBounds.left + 6 + (glyph * 6 + column) * scale;
                        const LONG y = LabelBounds.top + 9 + row * scale;
                        rects[count++] = {x, y, x + scale, y + scale};
                    }
                }
            }
        }

        std::array<RECT, 5 * 7 * 5> rects{};
        UINT count = 0;
    };

    struct D3D11Renderer
    {
        ComPtr<ID3D11Device> device;
        ComPtr<ID3D11DeviceContext> context;
        ComPtr<ID3D11DeviceContext1> context1;
        ComPtr<IDXGISwapChain> chain;
        ComPtr<ID3D11RenderTargetView> view;
        FrameLatency frameLatency;
        const LabelPixels label{false};

        HRESULT Initialize(HWND window)
        {
            const HRESULT status = legacyD3D11 ? InitializeLegacy(window) : InitializeFromDevice(window);
            if (FAILED(status)) return status;
            const HRESULT extended = context.As(&context1);
            if (FAILED(extended)) return extended;
            const HRESULT identity = VerifyContextIdentity();
            if (FAILED(identity)) return identity;
            if (verifyD3D11Bindings)
            {
                const HRESULT bindings = VerifyResourceBindings();
                if (FAILED(bindings)) return bindings;
            }
            if (verifyDxgiSurfaces)
            {
                const HRESULT surfaces = VerifyDxgiSurfaces();
                if (FAILED(surfaces)) return surfaces;
            }
            if (verifyD3D11Sharing)
            {
                const HRESULT shared = VerifySharedTexture();
                if (FAILED(shared)) return shared;
            }
            if (verifyDxgiEvents)
            {
                const HRESULT events = VerifyDxgiEvents();
                if (FAILED(events)) return events;
            }
            return CreateView();
        }

        HRESULT VerifyDxgiEvents()
        {
            ComPtr<IDXGIDevice2> dxgi;
            HRESULT status = device.As(&dxgi);
            if (FAILED(status)) return status;
            ComPtr<IDXGIAdapter> adapter;
            if (FAILED(status = dxgi->GetAdapter(&adapter))) return status;
            ComPtr<IDXGIFactory7> factory;
            if (FAILED(status = adapter->GetParent(IID_PPV_ARGS(&factory)))) return status;
            const HANDLE event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
            if (!event) return HRESULT_FROM_WIN32(GetLastError());
            const auto waitForGpuEvent = [event]() -> HRESULT
            {
                const DWORD wait = WaitForSingleObject(event, 5000);
                if (wait == WAIT_FAILED) return HRESULT_FROM_WIN32(GetLastError());
                return wait == WAIT_OBJECT_0 ? S_OK : HRESULT_FROM_WIN32(ERROR_TIMEOUT);
            };
            status = dxgi->EnqueueSetEvent(event);
            if (SUCCEEDED(status)) status = waitForGpuEvent();
            ComPtr<ID3D11DeviceContext3> context3;
            if (SUCCEEDED(status) && SUCCEEDED(context.As(&context3)))
            {
                context3->Flush1(D3D11_CONTEXT_TYPE_ALL, event);
                status = waitForGpuEvent();
            }
            DWORD cookie = 0;
            if (SUCCEEDED(status)) status = factory->RegisterAdaptersChangedEvent(event, &cookie);
            if (SUCCEEDED(status)) status = factory->UnregisterAdaptersChangedEvent(cookie);
            CloseHandle(event);
            return status;
        }

        HRESULT VerifyContextIdentity()
        {
            ComPtr<IUnknown> deviceIdentity;
            HRESULT status = device.As(&deviceIdentity);
            if (FAILED(status)) return status;
            ComPtr<ID3D11Device> parent;
            context->GetDevice(&parent);
            ComPtr<IUnknown> parentIdentity;
            if (!parent || FAILED(status = parent.As(&parentIdentity))) return FAILED(status) ? status : E_FAIL;
            if (parentIdentity.Get() != deviceIdentity.Get()) return E_FAIL;

            ComPtr<ID3D11DeviceContext> immediate;
            device->GetImmediateContext(&immediate);
            ComPtr<IUnknown> immediateIdentity, originalIdentity;
            if (!immediate || FAILED(status = immediate.As(&immediateIdentity))) return FAILED(status) ? status : E_FAIL;
            if (FAILED(status = context.As(&originalIdentity))) return status;
            if (originalIdentity.Get() != immediateIdentity.Get()) return E_FAIL;

            ComPtr<ID3D11DeviceContext> deferred;
            if (FAILED(status = device->CreateDeferredContext(0, &deferred))) return status;
            deferred->GetDevice(parent.ReleaseAndGetAddressOf());
            if (!parent || FAILED(status = parent.As(&parentIdentity))) return FAILED(status) ? status : E_FAIL;
            return deviceIdentity.Get() == parentIdentity.Get() ? S_OK : E_FAIL;
        }

        HRESULT VerifySharedTexture()
        {
            D3D11_TEXTURE2D_DESC desc{};
            desc.Width = desc.Height = 32;
            desc.MipLevels = desc.ArraySize = 1;
            desc.Format = Format;
            desc.SampleDesc.Count = 1;
            desc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
            desc.MiscFlags = D3D11_RESOURCE_MISC_SHARED_NTHANDLE | D3D11_RESOURCE_MISC_SHARED_KEYEDMUTEX;
            ComPtr<ID3D11Texture2D> source;
            HRESULT status = device->CreateTexture2D(&desc, nullptr, &source);
            if (FAILED(status)) return status;
            ComPtr<IDXGIResource1> resource;
            if (FAILED(status = source.As(&resource))) return status;
            ComPtr<ID3D11Device1> device1;
            if (FAILED(status = device.As(&device1))) return status;
            HANDLE shared = nullptr;
            if (FAILED(status = resource->CreateSharedHandle(nullptr,
                DXGI_SHARED_RESOURCE_READ | DXGI_SHARED_RESOURCE_WRITE, nullptr, &shared))) return status;
            ComPtr<ID3D11Texture2D> reopened;
            status = device1->OpenSharedResource1(shared, IID_PPV_ARGS(&reopened));
            const BOOL closed = CloseHandle(shared);
            if (FAILED(status)) return status;
            if (!closed) return HRESULT_FROM_WIN32(GetLastError());
            if (!Owns(reopened.Get())) return E_UNEXPECTED;
            ComPtr<IDXGIKeyedMutex> sourceMutex, reopenedMutex;
            if (FAILED(status = source.As(&sourceMutex))) return status;
            if (FAILED(status = reopened.As(&reopenedMutex))) return status;
            if (!SameIdentity(source.Get(), sourceMutex.Get()) || !SameIdentity(reopened.Get(), reopenedMutex.Get()))
                return E_UNEXPECTED;
            status = sourceMutex->AcquireSync(0, 5000);
            if (status != S_OK) return FAILED(status) ? status : E_UNEXPECTED;
            if (FAILED(status = sourceMutex->ReleaseSync(1))) return status;
            status = reopenedMutex->AcquireSync(1, 5000);
            if (status != S_OK) return FAILED(status) ? status : E_UNEXPECTED;
            return reopenedMutex->ReleaseSync(0);
        }

        bool Owns(ID3D11DeviceChild* child) const
        {
            if (!child) return false;
            ComPtr<ID3D11Device> owner;
            child->GetDevice(&owner);
            return SameIdentity(device.Get(), owner.Get());
        }

        HRESULT CheckSurface(IDXGISurface2* surface, IUnknown* texture, UINT expectedIndex) const
        {
            ComPtr<IDXGIDevice> owner, parent;
            HRESULT status = surface->GetDevice(IID_PPV_ARGS(&owner));
            if (FAILED(status)) return status;
            if (FAILED(status = surface->GetParent(IID_PPV_ARGS(&parent)))) return status;
            if (!SameIdentity(device.Get(), owner.Get()) || !SameIdentity(device.Get(), parent.Get()))
                return E_UNEXPECTED;
            UINT index = static_cast<UINT>(-1);
            ComPtr<IUnknown> resource;
            if (FAILED(status = surface->GetResource(IID_PPV_ARGS(&resource), &index))) return status;
            return index == expectedIndex && SameIdentity(texture, resource.Get()) ? S_OK : E_UNEXPECTED;
        }

        HRESULT VerifyDxgiSurfaces()
        {
            ComPtr<ID3D11Texture2D> backBuffer;
            HRESULT status = chain->GetBuffer(0, IID_PPV_ARGS(&backBuffer));
            if (FAILED(status)) return status;
            ComPtr<IDXGISurface2> direct, fromSwapChain;
            if (FAILED(status = backBuffer.As(&direct))) return status;
            if (FAILED(status = chain->GetBuffer(0, IID_PPV_ARGS(&fromSwapChain)))) return status;
            if (!SameIdentity(backBuffer.Get(), direct.Get()) || !SameIdentity(direct.Get(), fromSwapChain.Get()))
                return E_UNEXPECTED;
            if (FAILED(status = CheckSurface(direct.Get(), backBuffer.Get(), 0))) return status;

            ComPtr<IDXGIDevice> dxgi;
            if (FAILED(status = device.As(&dxgi))) return status;
            const DXGI_SURFACE_DESC surfaceDesc{8, 4, Format, {1, 0}};
            IDXGISurface* created[2]{};
            if (FAILED(status = dxgi->CreateSurface(&surfaceDesc, 2, DXGI_USAGE_RENDER_TARGET_OUTPUT, nullptr, created)))
                return status;
            ComPtr<IDXGISurface> first, second;
            first.Attach(created[0]);
            second.Attach(created[1]);
            if (!first || !second || SameIdentity(first.Get(), second.Get())) return E_UNEXPECTED;
            for (auto* surface : created)
            {
                ComPtr<ID3D11Texture2D> texture;
                ComPtr<IDXGISurface2> extended;
                if (FAILED(status = surface->QueryInterface(IID_PPV_ARGS(&texture)))) return status;
                if (FAILED(status = surface->QueryInterface(IID_PPV_ARGS(&extended)))) return status;
                if (!Owns(texture.Get()) || !SameIdentity(surface, texture.Get())) return E_UNEXPECTED;
                if (FAILED(status = CheckSurface(extended.Get(), texture.Get(), 0))) return status;
            }

            D3D11_TEXTURE2D_DESC desc{};
            desc.Width = 8;
            desc.Height = 4;
            desc.ArraySize = desc.MipLevels = 2;
            desc.Format = Format;
            desc.SampleDesc.Count = 1;
            desc.BindFlags = D3D11_BIND_RENDER_TARGET;
            ComPtr<ID3D11Texture2D> array;
            if (FAILED(status = device->CreateTexture2D(&desc, nullptr, &array))) return status;
            ComPtr<IDXGISurface> unsupported;
            if (array.As(&unsupported) != E_NOINTERFACE) return E_UNEXPECTED;
            ComPtr<IDXGIResource1> resource;
            if (FAILED(status = array.As(&resource))) return status;
            const UINT index = D3D11CalcSubresource(1, 1, desc.MipLevels);
            ComPtr<IDXGISurface2> subresource, another;
            if (FAILED(status = resource->CreateSubresourceSurface(index, &subresource))) return status;
            if (FAILED(status = resource->CreateSubresourceSurface(index, &another))) return status;
            if (SameIdentity(array.Get(), subresource.Get()) || SameIdentity(subresource.Get(), another.Get()))
                return E_UNEXPECTED;
            DXGI_SURFACE_DESC subresourceDesc{};
            if (FAILED(status = subresource->GetDesc(&subresourceDesc))) return status;
            if (subresourceDesc.Width != 4 || subresourceDesc.Height != 2 || subresourceDesc.Format != Format)
                return E_UNEXPECTED;
            if (FAILED(status = CheckSurface(subresource.Get(), array.Get(), index))) return status;

            desc.ArraySize = desc.MipLevels = 1;
            desc.BindFlags = 0;
            desc.Usage = D3D11_USAGE_STAGING;
            desc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE | D3D11_CPU_ACCESS_READ;
            ComPtr<ID3D11Texture2D> staging;
            if (FAILED(status = device->CreateTexture2D(&desc, nullptr, &staging))) return status;
            ComPtr<IDXGISurface> stagingSurface;
            if (FAILED(status = staging.As(&stagingSurface))) return status;
            DXGI_MAPPED_RECT mapped{};
            if (FAILED(status = stagingSurface->Map(&mapped, DXGI_MAP_WRITE))) return status;
            mapped.pBits[0] = 11;
            mapped.pBits[1] = 22;
            if (FAILED(status = stagingSurface->Unmap())) return status;
            D3D11_MAPPED_SUBRESOURCE d3dMapping{};
            if (FAILED(status = context->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &d3dMapping))) return status;
            const auto* bytes = static_cast<const BYTE*>(d3dMapping.pData);
            const bool correct = bytes[0] == 11 && bytes[1] == 22;
            context->Unmap(staging.Get(), 0);
            return correct ? S_OK : E_FAIL;
        }

        HRESULT VerifyResourceBindings()
        {
            ComPtr<ID3D11VideoDevice> videoDevice;
            if (SUCCEEDED(device.As(&videoDevice)) && !SameIdentity(device.Get(), videoDevice.Get())) return E_UNEXPECTED;
            ComPtr<ID3D11VideoContext> videoContext;
            if (SUCCEEDED(context.As(&videoContext)) &&
                (!SameIdentity(context.Get(), videoContext.Get()) || !Owns(videoContext.Get()))) return E_UNEXPECTED;

            D3D11_BUFFER_DESC vertexDesc{};
            vertexDesc.ByteWidth = 64;
            vertexDesc.Usage = D3D11_USAGE_DYNAMIC;
            vertexDesc.BindFlags = D3D11_BIND_VERTEX_BUFFER;
            vertexDesc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
            ComPtr<ID3D11Buffer> vertex;
            HRESULT status = device->CreateBuffer(&vertexDesc, nullptr, &vertex);
            if (FAILED(status)) return status;
            if (!Owns(vertex.Get())) return E_UNEXPECTED;
            D3D11_MAPPED_SUBRESOURCE mapping{};
            if (FAILED(status = context->Map(vertex.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapping))) return status;
            static_cast<float*>(mapping.pData)[0] = 0.5f;
            context->Unmap(vertex.Get(), 0);

            ID3D11Buffer* inputs[] = {vertex.Get(), nullptr};
            const UINT strides[] = {4, 0}, offsets[] = {0, 0};
            context->IASetVertexBuffers(0, 2, inputs, strides, offsets);
            ID3D11Buffer* returned[2]{};
            UINT returnedStrides[2]{}, returnedOffsets[2]{};
            context->IAGetVertexBuffers(0, 2, returned, returnedStrides, returnedOffsets);
            ComPtr<ID3D11Buffer> bound, empty;
            bound.Attach(returned[0]);
            empty.Attach(returned[1]);
            if (!SameIdentity(vertex.Get(), bound.Get()) || empty || returnedStrides[0] != 4 || returnedOffsets[0])
                return E_UNEXPECTED;

            D3D11_TEXTURE1D_DESC desc1{};
            desc1.Width = 4;
            desc1.MipLevels = desc1.ArraySize = 1;
            desc1.Format = Format;
            desc1.BindFlags = D3D11_BIND_SHADER_RESOURCE;
            ComPtr<ID3D11Texture1D> texture1;
            if (FAILED(status = device->CreateTexture1D(&desc1, nullptr, &texture1))) return status;

            D3D11_TEXTURE3D_DESC desc3{};
            desc3.Width = desc3.Height = desc3.Depth = 4;
            desc3.MipLevels = 1;
            desc3.Format = Format;
            desc3.BindFlags = D3D11_BIND_SHADER_RESOURCE;
            ComPtr<ID3D11Texture3D> texture3;
            if (FAILED(status = device->CreateTexture3D(&desc3, nullptr, &texture3))) return status;
            if (!Owns(texture1.Get()) || !Owns(texture3.Get())) return E_UNEXPECTED;

            ComPtr<ID3D11ShaderResourceView> srv;
            if (FAILED(status = device->CreateShaderResourceView(texture3.Get(), nullptr, &srv))) return status;
            ComPtr<ID3D11Resource> resource;
            srv->GetResource(&resource);
            if (!Owns(srv.Get()) || !SameIdentity(texture3.Get(), resource.Get())) return E_UNEXPECTED;
            context->PSSetShaderResources(0, 1, srv.GetAddressOf());
            ComPtr<ID3D11ShaderResourceView> boundSrv;
            context->PSGetShaderResources(0, 1, boundSrv.GetAddressOf());
            if (!SameIdentity(srv.Get(), boundSrv.Get())) return E_UNEXPECTED;

            D3D11_TEXTURE2D_DESC desc{};
            desc.Width = desc.Height = 4;
            desc.MipLevels = desc.ArraySize = desc.SampleDesc.Count = 1;
            desc.Format = Format;
            desc.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_UNORDERED_ACCESS;
            ComPtr<ID3D11Texture2D> target;
            if (FAILED(status = device->CreateTexture2D(&desc, nullptr, &target))) return status;
            ComPtr<ID3D11RenderTargetView> rtv;
            if (FAILED(status = device->CreateRenderTargetView(target.Get(), nullptr, &rtv))) return status;
            ComPtr<ID3D11UnorderedAccessView> uav;
            if (FAILED(status = device->CreateUnorderedAccessView(target.Get(), nullptr, &uav))) return status;
            rtv->GetResource(resource.ReleaseAndGetAddressOf());
            if (!Owns(target.Get()) || !Owns(rtv.Get()) || !Owns(uav.Get()) ||
                !SameIdentity(target.Get(), resource.Get())) return E_UNEXPECTED;

            D3D11_TEXTURE2D_DESC depthDesc = desc;
            depthDesc.Format = DXGI_FORMAT_D24_UNORM_S8_UINT;
            depthDesc.BindFlags = D3D11_BIND_DEPTH_STENCIL;
            ComPtr<ID3D11Texture2D> depth;
            if (FAILED(status = device->CreateTexture2D(&depthDesc, nullptr, &depth))) return status;
            ComPtr<ID3D11DepthStencilView> dsv;
            if (FAILED(status = device->CreateDepthStencilView(depth.Get(), nullptr, &dsv))) return status;
            if (!Owns(dsv.Get())) return E_UNEXPECTED;
            context->ClearDepthStencilView(dsv.Get(), D3D11_CLEAR_DEPTH, 1.0f, 0);
            context->OMSetRenderTargets(1, rtv.GetAddressOf(), dsv.Get());
            context->OMSetRenderTargetsAndUnorderedAccessViews(D3D11_KEEP_RENDER_TARGETS_AND_DEPTH_STENCIL,
                nullptr, nullptr, 1, 0, nullptr, nullptr);
            ComPtr<ID3D11RenderTargetView> boundRtv;
            ComPtr<ID3D11DepthStencilView> boundDsv;
            context->OMGetRenderTargets(1, boundRtv.GetAddressOf(), boundDsv.GetAddressOf());
            if (!SameIdentity(rtv.Get(), boundRtv.Get()) || !SameIdentity(dsv.Get(), boundDsv.Get())) return E_UNEXPECTED;
            context->OMSetRenderTargets(0, nullptr, nullptr);
            context->CSSetUnorderedAccessViews(0, 1, uav.GetAddressOf(), nullptr);
            ComPtr<ID3D11UnorderedAccessView> boundUav;
            context->CSGetUnorderedAccessViews(0, 1, boundUav.GetAddressOf());
            if (!SameIdentity(uav.Get(), boundUav.Get())) return E_UNEXPECTED;
            context->ClearState();

            ComPtr<ID3D11Device3> device3;
            if (SUCCEEDED(device.As(&device3)))
            {
                ComPtr<ID3D11RenderTargetView1> rtv1;
                if (FAILED(status = device3->CreateRenderTargetView1(target.Get(), nullptr, &rtv1))) return status;
                D3D11_RENDER_TARGET_VIEW_DESC1 versioned{};
                rtv1->GetDesc1(&versioned);
                ComPtr<ID3D11RenderTargetView> base;
                if (FAILED(status = rtv1.As(&base))) return status;
                if (versioned.Format != Format || !Owns(rtv1.Get()) || !SameIdentity(rtv1.Get(), base.Get()))
                    return E_UNEXPECTED;
            }

            const float red[] = {1, 0, 0, 1};
            context->ClearRenderTargetView(rtv.Get(), red);
            desc.BindFlags = 0;
            desc.Usage = D3D11_USAGE_STAGING;
            desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
            ComPtr<ID3D11Texture2D> readback;
            if (FAILED(status = device->CreateTexture2D(&desc, nullptr, &readback))) return status;
            context->CopySubresourceRegion(readback.Get(), 0, 0, 0, 0, target.Get(), 0, nullptr);
            if (FAILED(status = context->Map(readback.Get(), 0, D3D11_MAP_READ, 0, &mapping))) return status;
            const auto bytes = static_cast<const BYTE*>(mapping.pData);
            const bool correct = bytes[0] == 255 && bytes[1] == 0 && bytes[2] == 0 && bytes[3] == 255;
            context->Unmap(readback.Get(), 0);
            return correct ? S_OK : E_FAIL;
        }

        HRESULT InitializeLegacy(HWND window)
        {
            DXGI_SWAP_CHAIN_DESC description{};
            description.BufferDesc.Format = Format;
            description.SampleDesc.Count = 1;
            description.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
            description.BufferCount = 2;
            description.OutputWindow = window;
            description.Windowed = TRUE;
            description.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;
            return D3D11CreateDeviceAndSwapChain(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr,
                0, nullptr, 0, D3D11_SDK_VERSION, &description, &chain, &device, nullptr, &context);
        }

        HRESULT InitializeFromDevice(HWND window)
        {
            HRESULT status = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr,
                0, nullptr, 0, D3D11_SDK_VERSION, &device, nullptr, &context);
            if (FAILED(status)) return status;
            ComPtr<IDXGIDevice> dxgi;
            if (FAILED(status = device.As(&dxgi))) return status;
            ComPtr<IUnknown> identity, dxgiIdentity;
            if (FAILED(status = device.As(&identity))) return status;
            if (FAILED(status = dxgi.As(&dxgiIdentity))) return status;
            if (identity.Get() != dxgiIdentity.Get()) return E_UNEXPECTED;

            ComPtr<IDXGIAdapter> adapter;
            if (FAILED(status = dxgi->GetAdapter(&adapter))) return status;
            ComPtr<IDXGIFactory2> factory;
            if (FAILED(status = adapter->GetParent(IID_PPV_ARGS(&factory)))) return status;
            DXGI_SWAP_CHAIN_DESC1 description{};
            description.Format = Format;
            description.SampleDesc.Count = 1;
            description.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
            description.BufferCount = 2;
            description.SwapEffect = DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL;
            description.Flags = FrameLatencyFlag;
            ComPtr<IDXGISwapChain1> created;
            status = factory->CreateSwapChainForHwnd(device.Get(), window, &description, nullptr, nullptr, &created);
            if (FAILED(status)) return status;
            HWND returnedWindow = nullptr;
            if (FAILED(status = created->GetHwnd(&returnedWindow))) return status;
            if (returnedWindow != window) return E_UNEXPECTED;
            ComPtr<IUnknown> chainDevice;
            if (FAILED(status = created->GetDevice(IID_PPV_ARGS(&chainDevice)))) return status;
            if (chainDevice.Get() != identity.Get()) return E_UNEXPECTED;
            chain = std::move(created);
            return frameLatency.Initialize(chain.Get());
        }

        HRESULT CreateView()
        {
            ComPtr<ID3D11Texture2D> buffer;
            HRESULT status = chain->GetBuffer(0, IID_PPV_ARGS(&buffer));
            if (FAILED(status)) return status;
            ComPtr<ID3D11Device> parent;
            buffer->GetDevice(&parent);
            ComPtr<IUnknown> deviceIdentity, parentIdentity;
            if (FAILED(status = device.As(&deviceIdentity))) return status;
            if (!parent || FAILED(status = parent.As(&parentIdentity))) return FAILED(status) ? status : E_FAIL;
            if (deviceIdentity.Get() != parentIdentity.Get()) return E_FAIL;
            return device->CreateRenderTargetView(buffer.Get(), nullptr, &view);
        }

        HRESULT Draw(HWND window, bool resize)
        {
            if (resize)
            {
                view.Reset();
                HRESULT status = chain->ResizeBuffers(0, 0, 0, DXGI_FORMAT_UNKNOWN, legacyD3D11 ? 0 : FrameLatencyFlag);
                if (FAILED(status)) return status;
                if (FAILED(status = VerifyClientSizedBuffers(window, chain.Get()))) return status;
                status = CreateView();
                if (FAILED(status)) return status;
            }
            const HRESULT ready = frameLatency.Wait();
            if (FAILED(ready)) return ready;
            float color[4];
            FrameColor(color);
            context->ClearRenderTargetView(view.Get(), color);
            context1->ClearView(view.Get(), LabelBackground, &LabelBounds, 1);
            context1->ClearView(view.Get(), LabelForeground, label.rects.data(), label.count);
            return chain->Present(1, 0);
        }
    };

    struct D3D12Renderer
    {
        static constexpr UINT BufferCount = 2;
        ComPtr<ID3D12Device> device;
        ComPtr<ID3D12CommandQueue> queue;
        ComPtr<ID3D12CommandAllocator> allocator;
        ComPtr<ID3D12GraphicsCommandList> commands;
        ComPtr<ID3D12DescriptorHeap> views;
        ComPtr<ID3D12Resource> buffers[BufferCount];
        ComPtr<IDXGISwapChain3> chain;
        ComPtr<ID3D12Fence> fence;
        FrameLatency frameLatency;
        const LabelPixels label{true};
        HANDLE fenceEvent = nullptr;
        UINT64 fenceValue = 0;

        ~D3D12Renderer()
        {
            if (fenceEvent) CloseHandle(fenceEvent);
        }

        HRESULT Initialize(HWND window)
        {
            const HMODULE library = LoadLibraryW(L"d3d12.dll");
            auto create = library ? reinterpret_cast<PFN_D3D12_CREATE_DEVICE>(GetProcAddress(library, "D3D12CreateDevice")) : nullptr;
            if (!create) return DXGI_ERROR_UNSUPPORTED;
            if (useDeviceFactory)
            {
                const auto get = reinterpret_cast<PFN_D3D12_GET_INTERFACE>(GetProcAddress(library, "D3D12GetInterface"));
                if (!get) return DXGI_ERROR_UNSUPPORTED;
                constexpr GUID factoryClass{0x114863bf, 0xc386, 0x4aee, {0xb3, 0x9d, 0x8f, 0x0b, 0xbb, 0x06, 0x29, 0x55}};
                ComPtr<ID3D12DeviceFactory> configuration;
                const HRESULT queried = get(factoryClass, IID_PPV_ARGS(&configuration));
                if (FAILED(queried)) return queried;
                const HRESULT applied = configuration->ApplyToGlobalState();
                if (FAILED(applied)) return applied;
            }
            ComPtr<IDXGIFactory2> factory;
            HRESULT status = CreateDXGIFactory1(IID_PPV_ARGS(&factory));
            if (FAILED(status)) return status;
            ComPtr<IDXGIAdapter1> adapter;
            if (FAILED(status = factory->EnumAdapters1(0, &adapter))) return status;
            status = create(adapter.Get(), D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device));
            if (FAILED(status)) return status;
            D3D12_COMMAND_QUEUE_DESC queueDesc{};
            queueDesc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
            if (FAILED(status = device->CreateCommandQueue(&queueDesc, IID_PPV_ARGS(&queue)))) return status;
            ComPtr<IUnknown> identity;
            ComPtr<IUnknown> queueDevice;
            ComPtr<IUnknown> reopened;
            if (FAILED(status = device.As(&identity))) return status;
            if (FAILED(status = queue->GetDevice(IID_PPV_ARGS(&queueDevice)))) return status;
            if (FAILED(status = create(adapter.Get(), D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&reopened)))) return status;
            if (identity.Get() != queueDevice.Get() || identity.Get() != reopened.Get()) return E_FAIL;
            DXGI_SWAP_CHAIN_DESC1 description{};
            description.Format = Format;
            description.SampleDesc.Count = 1;
            description.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
            description.BufferCount = BufferCount;
            description.SwapEffect = DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL;
            description.Flags = FrameLatencyFlag;
            ComPtr<IDXGISwapChain1> created;
            if (FAILED(status = factory->CreateSwapChainForHwnd(queue.Get(), window, &description, nullptr, nullptr, &created))) return status;
            ComPtr<IUnknown> chainDevice;
            if (FAILED(status = created->GetDevice(IID_PPV_ARGS(&chainDevice)))) return status;
            if (chainDevice.Get() != identity.Get()) return E_FAIL;
            if (FAILED(status = created.As(&chain))) return status;
            if (FAILED(status = frameLatency.Initialize(chain.Get()))) return status;
            if (FAILED(status = device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator)))) return status;
            if (FAILED(status = device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator.Get(), nullptr, IID_PPV_ARGS(&commands)))) return status;
            if (FAILED(status = commands->Close())) return status;
            if (FAILED(status = device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence)))) return status;
            fenceEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);
            if (!fenceEvent) return HRESULT_FROM_WIN32(GetLastError());
            if (FAILED(status = VerifyFenceEvents())) return status;
            if (verifyD3D11On12 && FAILED(status = VerifyOn12Resource())) return status;
            if (verifySharing && FAILED(status = VerifySharedFence())) return status;
            D3D12_DESCRIPTOR_HEAP_DESC viewDesc{};
            viewDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
            viewDesc.NumDescriptors = BufferCount;
            if (FAILED(status = device->CreateDescriptorHeap(&viewDesc, IID_PPV_ARGS(&views)))) return status;
            return CreateViews();
        }

        HRESULT VerifyOn12Resource()
        {
            const HMODULE d3d11 = GetModuleHandleW(L"d3d11.dll");
            const auto create = d3d11 ? reinterpret_cast<PFN_D3D11ON12_CREATE_DEVICE>(
                GetProcAddress(d3d11, "D3D11On12CreateDevice")) : nullptr;
            if (!create) return DXGI_ERROR_UNSUPPORTED;
            IUnknown* queues[] = {queue.Get()};
            ComPtr<ID3D11Device> device11;
            ComPtr<ID3D11DeviceContext> context11;
            HRESULT status = create(device.Get(), 0, nullptr, 0, queues, 1, 0, &device11, &context11, nullptr);
            if (FAILED(status)) return status;
            ComPtr<ID3D11On12Device> on12;
            if (FAILED(status = device11.As(&on12))) return status;
            if (!SameIdentity(on12.Get(), device11.Get())) return E_UNEXPECTED;
            ComPtr<ID3D11On12Device1> on12v1;
            if (SUCCEEDED(on12.As(&on12v1)))
            {
                ComPtr<ID3D12Device> owner;
                if (FAILED(status = on12v1->GetD3D12Device(IID_PPV_ARGS(&owner)))) return status;
                if (!SameIdentity(owner.Get(), device.Get())) return E_UNEXPECTED;
            }

            D3D12_HEAP_PROPERTIES heap{};
            heap.Type = D3D12_HEAP_TYPE_DEFAULT;
            heap.CreationNodeMask = heap.VisibleNodeMask = 1;
            D3D12_RESOURCE_DESC desc{};
            desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
            desc.Width = desc.Height = 4;
            desc.DepthOrArraySize = desc.MipLevels = 1;
            desc.Format = Format;
            desc.SampleDesc.Count = 1;
            desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
            constexpr auto state = D3D12_RESOURCE_STATE_RENDER_TARGET;
            ComPtr<ID3D12Resource> texture12;
            status = device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc, state, nullptr,
                IID_PPV_ARGS(&texture12));
            if (FAILED(status)) return status;
            D3D11_RESOURCE_FLAGS flags{};
            flags.BindFlags = D3D11_BIND_RENDER_TARGET;
            ComPtr<ID3D11Resource> resource;
            if (FAILED(status = on12->CreateWrappedResource(texture12.Get(), &flags, state, state, IID_PPV_ARGS(&resource))))
                return status;
            ComPtr<ID3D11RenderTargetView> rtv;
            if (FAILED(status = device11->CreateRenderTargetView(resource.Get(), nullptr, &rtv))) return status;
            ComPtr<ID3D11Resource> fromView;
            ComPtr<ID3D11Device> fromResource;
            rtv->GetResource(&fromView);
            resource->GetDevice(&fromResource);
            if (!SameIdentity(resource.Get(), fromView.Get()) || !SameIdentity(device11.Get(), fromResource.Get()))
                return E_UNEXPECTED;

            on12->ReleaseWrappedResources(resource.GetAddressOf(), 1);
            on12->AcquireWrappedResources(resource.GetAddressOf(), 1);
            const float red[] = {1, 0, 0, 1};
            context11->ClearRenderTargetView(rtv.Get(), red);
            on12->ReleaseWrappedResources(resource.GetAddressOf(), 1);
            rtv.Reset();
            fromView.Reset();
            resource.Reset();
            context11->Flush();
            return WaitForGpu();
        }

        HRESULT VerifySharedFence()
        {
            ComPtr<ID3D12Fence> original;
            HRESULT status = device->CreateFence(3, D3D12_FENCE_FLAG_SHARED, IID_PPV_ARGS(&original));
            if (FAILED(status)) return status;
            wchar_t name[96]{};
            swprintf_s(name, L"Local\\Win32Bridge.GraphicsContracts.Fence.%lu",
                static_cast<unsigned long>(GetCurrentProcessId()));
            HANDLE source = nullptr;
            if (FAILED(status = device->CreateSharedHandle(original.Get(), nullptr, GENERIC_ALL, name, &source))) return status;
            HANDLE alias = nullptr, duplicate = nullptr;
            status = device->OpenSharedHandleByName(name, GENERIC_ALL, &alias);
            CloseHandle(source);
            if (FAILED(status)) return status;
            const HANDLE process = GetCurrentProcess();
            const BOOL copied = DuplicateHandle(process, alias, process, &duplicate, 0, FALSE, DUPLICATE_SAME_ACCESS);
            const DWORD copyError = copied ? ERROR_SUCCESS : GetLastError();
            CloseHandle(alias);
            if (!copied) return HRESULT_FROM_WIN32(copyError);
            ComPtr<ID3D12Fence> opened;
            status = device->OpenSharedHandle(duplicate, IID_PPV_ARGS(&opened));
            CloseHandle(duplicate);
            if (FAILED(status)) return status;
            if (FAILED(status = original->Signal(4))) return status;
            return opened->GetCompletedValue() == 4 ? S_OK : E_FAIL;
        }

        HRESULT VerifyFenceEvents()
        {
            ComPtr<ID3D12Fence> gate;
            HRESULT status = device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&gate));
            if (FAILED(status)) return status;
            if (FAILED(status = fence->SetEventOnCompletion(0, fenceEvent))) return status;
            if (WaitForSingleObject(fenceEvent, 0) != WAIT_OBJECT_0) return E_FAIL;

            HANDLE registrationHandle = nullptr;
            const HANDLE process = GetCurrentProcess();
            if (!DuplicateHandle(process, fenceEvent, process, &registrationHandle, 0, FALSE, DUPLICATE_SAME_ACCESS))
                return HRESULT_FROM_WIN32(GetLastError());
            status = queue->Wait(gate.Get(), 1);
            if (FAILED(status))
            {
                CloseHandle(registrationHandle);
                return status;
            }
            // Unblock the queue even if registration fails.
            status = fence->SetEventOnCompletion(1, registrationHandle);
            CloseHandle(registrationHandle);
            if (SUCCEEDED(status)) status = queue->Signal(fence.Get(), 1);
            const DWORD before = WaitForSingleObject(fenceEvent, 0);
            const HRESULT unblocked = gate->Signal(1);
            if (FAILED(status)) return status;
            if (FAILED(unblocked)) return unblocked;
            if (before != WAIT_TIMEOUT || WaitForSingleObject(fenceEvent, 5000) != WAIT_OBJECT_0 ||
                WaitForSingleObject(fenceEvent, 0) != WAIT_TIMEOUT)
                return E_FAIL;
            fenceValue = 1;

            ComPtr<ID3D12Device1> device1;
            if (FAILED(device.As(&device1))) return S_OK;
            ID3D12Fence* fences[] = {fence.Get(), gate.Get()};
            UINT64 values[] = {1, 2};
            if (FAILED(status = device1->SetEventOnMultipleFenceCompletion(fences, values, 2,
                D3D12_MULTIPLE_FENCE_WAIT_FLAG_ALL, fenceEvent))) return status;
            if (WaitForSingleObject(fenceEvent, 0) != WAIT_TIMEOUT) return E_FAIL;
            if (FAILED(status = gate->Signal(2))) return status;
            if (WaitForSingleObject(fenceEvent, 5000) != WAIT_OBJECT_0) return E_FAIL;
            values[0] = 2;
            if (FAILED(status = device1->SetEventOnMultipleFenceCompletion(fences, values, 2,
                D3D12_MULTIPLE_FENCE_WAIT_FLAG_ANY, fenceEvent))) return status;
            return WaitForSingleObject(fenceEvent, 5000) == WAIT_OBJECT_0 ? S_OK : E_FAIL;
        }

        D3D12_CPU_DESCRIPTOR_HANDLE GetView(UINT index) const
        {
            auto handle = views->GetCPUDescriptorHandleForHeapStart();
            handle.ptr += index * device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
            return handle;
        }

        HRESULT CreateViews()
        {
            for (UINT index = 0; index < BufferCount; ++index)
            {
                const HRESULT status = chain->GetBuffer(index, IID_PPV_ARGS(&buffers[index]));
                if (FAILED(status)) return status;
                device->CreateRenderTargetView(buffers[index].Get(), nullptr, GetView(index));
            }
            return S_OK;
        }

        HRESULT WaitForGpu()
        {
            HRESULT status = fence->SetEventOnCompletion(++fenceValue, fenceEvent);
            if (FAILED(status)) return status;
            if (FAILED(status = queue->Signal(fence.Get(), fenceValue))) return status;
            const DWORD waited = WaitForSingleObject(fenceEvent, 5000);
            if (waited == WAIT_FAILED) return HRESULT_FROM_WIN32(GetLastError());
            return waited == WAIT_OBJECT_0 ? S_OK : HRESULT_FROM_WIN32(ERROR_TIMEOUT);
        }

        HRESULT Draw(HWND window, bool resize)
        {
            HRESULT status;
            if (resize)
            {
                for (auto& buffer : buffers) buffer.Reset();
                if (FAILED(status = chain->ResizeBuffers(0, 0, 0, DXGI_FORMAT_UNKNOWN, FrameLatencyFlag))) return status;
                if (FAILED(status = VerifyClientSizedBuffers(window, chain.Get()))) return status;
                if (FAILED(status = CreateViews())) return status;
            }
            if (FAILED(status = frameLatency.Wait())) return status;
            if (FAILED(status = allocator->Reset())) return status;
            if (FAILED(status = commands->Reset(allocator.Get(), nullptr))) return status;
            const UINT index = chain->GetCurrentBackBufferIndex();
            D3D12_RESOURCE_BARRIER barrier{};
            barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
            barrier.Transition.pResource = buffers[index].Get();
            barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
            barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_PRESENT;
            barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_RENDER_TARGET;
            commands->ResourceBarrier(1, &barrier);
            float color[4];
            FrameColor(color);
            commands->ClearRenderTargetView(GetView(index), color, 0, nullptr);
            commands->ClearRenderTargetView(GetView(index), LabelBackground, 1, &LabelBounds);
            commands->ClearRenderTargetView(GetView(index), LabelForeground, label.count, label.rects.data());
            barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
            barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_PRESENT;
            commands->ResourceBarrier(1, &barrier);
            if (FAILED(status = commands->Close())) return status;
            ID3D12CommandList* work[] = {commands.Get()};
            queue->ExecuteCommandLists(1, work);
            const DXGI_PRESENT_PARAMETERS parameters{};
            const HRESULT present = gateFirstFrame ? chain->Present1(1, 0, &parameters) : chain->Present(1, 0);
            const HRESULT completed = WaitForGpu();
            return FAILED(completed) ? completed : present;
        }
    };

    template<class Renderer>
    HRESULT Run(HWND window)
    {
        firstFrameAllowed = !gateFirstFrame;
        Renderer renderer;
        HRESULT status = renderer.Initialize(window);
        if (FAILED(status)) return status;
        if (gateFirstFrame)
        {
            status = renderer.chain->Present(0, DXGI_PRESENT_TEST);
            if (FAILED(status)) return status;
            InvalidateRect(window, nullptr, FALSE);
        }
        MSG message{};
        for (;;)
        {
            while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE))
            {
                if (message.message == WM_QUIT) return S_OK;
                TranslateMessage(&message);
                DispatchMessageW(&message);
            }
            if (switchRenderer) { switchRenderer = false; return S_FALSE; }
            if (minimized || !firstFrameAllowed) { WaitMessage(); continue; }
            status = renderer.Draw(window, resizePending);
            resizePending = false;
            if (FAILED(status)) return status;
            if (status == DXGI_STATUS_OCCLUDED) Sleep(20);
        }
    }
}

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR arguments, int show)
{
    bool useD3D12 = arguments && std::wcsstr(arguments, L"--d3d12");
    legacyD3D11 = arguments && std::wcsstr(arguments, L"--legacy-d3d11");
    verifyD3D11Bindings = arguments && std::wcsstr(arguments, L"--d3d11-bindings");
    verifyD3D11On12 = arguments && std::wcsstr(arguments, L"--d3d11on12");
    verifyDxgiSurfaces = arguments && std::wcsstr(arguments, L"--dxgi-surfaces");
    verifySharing = arguments && std::wcsstr(arguments, L"--shared-d3d12");
    verifyD3D11Sharing = arguments && std::wcsstr(arguments, L"--shared-d3d11");
    useDeviceFactory = arguments && std::wcsstr(arguments, L"--factory-d3d12");
    verifyDxgiEvents = arguments && std::wcsstr(arguments, L"--dxgi-events");
    gateFirstFrame = arguments && std::wcsstr(arguments, L"--gate-first-frame");
    WNDCLASSW windowClass{};
    windowClass.lpfnWndProc = WindowProc;
    windowClass.hInstance = instance;
    windowClass.lpszClassName = L"Win32BridgeGraphicsContracts";
    if (!RegisterClassW(&windowClass)) return 1;
    const HWND window = CreateWindowExW(0, windowClass.lpszClassName, L"GraphicsContracts",
        WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT, 640, 420, nullptr, nullptr, instance, nullptr);
    if (!window) return 1;
    ShowWindow(window, show);
    for (;;)
    {
        SetWindowTextW(window, useD3D12 ? L"DXGI / D3D12" : L"DXGI / D3D11");
        const HRESULT status = useD3D12 ? Run<D3D12Renderer>(window) : Run<D3D11Renderer>(window);
        if (status == S_OK) return 0;
        if (FAILED(status))
        {
            wchar_t error[64]{};
            swprintf_s(error, L"Direct3D / DXGI HRESULT 0x%08lX", static_cast<unsigned long>(status));
            MessageBoxW(window, error, L"GraphicsContracts", MB_OK | MB_ICONERROR);
            return 1;
        }
        useD3D12 = !useD3D12;
    }
}
