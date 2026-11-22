#include <windows.h>
#include <d3d11_1.h>
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
    bool verifySharing = false;
    bool verifyD3D11Sharing = false;
    bool useDeviceFactory = false;
    bool verifyDxgiEvents = false;
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
        if (message == WM_KEYDOWN && wParam == VK_ESCAPE) { DestroyWindow(window); return 0; }
        if (message == WM_KEYDOWN && wParam == VK_F12 && !(lParam & (1LL << 30))) { switchRenderer = true; return 0; }
        if (message == WM_DESTROY) { PostQuitMessage(0); return 0; }
        return DefWindowProcW(window, message, wParam, lParam);
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
            status = dxgi->EnqueueSetEvent(event);
            if (SUCCEEDED(status))
            {
                const DWORD wait = WaitForSingleObject(event, 5000);
                if (wait != WAIT_OBJECT_0)
                    status = HRESULT_FROM_WIN32(wait == WAIT_FAILED ? GetLastError() : ERROR_TIMEOUT);
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
            ComPtr<ID3D11Device> parent;
            reopened->GetDevice(&parent);
            ComPtr<IUnknown> identity, parentIdentity;
            if (FAILED(status = device.As(&identity))) return status;
            if (!parent || FAILED(status = parent.As(&parentIdentity))) return FAILED(status) ? status : E_FAIL;
            return identity.Get() == parentIdentity.Get() ? S_OK : E_FAIL;
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

        HRESULT Draw(bool resize)
        {
            if (resize)
            {
                view.Reset();
                HRESULT status = chain->ResizeBuffers(0, 0, 0, DXGI_FORMAT_UNKNOWN, legacyD3D11 ? 0 : FrameLatencyFlag);
                if (FAILED(status)) return status;
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
            if (verifySharing && FAILED(status = VerifySharedFence())) return status;
            D3D12_DESCRIPTOR_HEAP_DESC viewDesc{};
            viewDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
            viewDesc.NumDescriptors = BufferCount;
            if (FAILED(status = device->CreateDescriptorHeap(&viewDesc, IID_PPV_ARGS(&views)))) return status;
            return CreateViews();
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

        HRESULT Draw(bool resize)
        {
            HRESULT status;
            if (resize)
            {
                for (auto& buffer : buffers) buffer.Reset();
                if (FAILED(status = chain->ResizeBuffers(0, 0, 0, DXGI_FORMAT_UNKNOWN, FrameLatencyFlag))) return status;
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
            const HRESULT present = chain->Present(1, 0);
            if (FAILED(status = fence->SetEventOnCompletion(++fenceValue, fenceEvent))) return status;
            if (FAILED(status = queue->Signal(fence.Get(), fenceValue))) return status;
            const DWORD waited = WaitForSingleObject(fenceEvent, 5000);
            if (waited == WAIT_FAILED) return HRESULT_FROM_WIN32(GetLastError());
            return waited == WAIT_OBJECT_0 ? present : HRESULT_FROM_WIN32(ERROR_TIMEOUT);
        }
    };

    template<class Renderer>
    HRESULT Run(HWND window)
    {
        Renderer renderer;
        HRESULT status = renderer.Initialize(window);
        if (FAILED(status)) return status;
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
            if (minimized) { WaitMessage(); continue; }
            status = renderer.Draw(resizePending);
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
    verifySharing = arguments && std::wcsstr(arguments, L"--shared-d3d12");
    verifyD3D11Sharing = arguments && std::wcsstr(arguments, L"--shared-d3d11");
    useDeviceFactory = arguments && std::wcsstr(arguments, L"--factory-d3d12");
    verifyDxgiEvents = arguments && std::wcsstr(arguments, L"--dxgi-events");
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
