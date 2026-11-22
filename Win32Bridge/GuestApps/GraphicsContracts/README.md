# D3D11 and D3D12 presentation fixture

Build `GraphicsContracts.vcxproj` as Release x64 in Visual Studio and open the
generated PE in DesktopMode. The window should continuously change color and
keep drawing after it is resized. F12 switches between D3D11 and D3D12, and Esc
closes the window. `--d3d12` starts directly in D3D12 mode.
`--shared-d3d12` additionally exercises a named shared D3D12 fence and
duplicates its handle before reopening the fence.
`--shared-d3d11` checks reopening a shared texture by NT handle and synchronizing
access through the two keyed mutex interfaces.
`--d3d11-bindings` checks device, resource and view identity through creation and
pipeline queries, then verifies GPU data after `ClearRenderTargetView`,
`CopySubresourceRegion` and `Map`.
`--d3d12 --d3d11on12` checks D3D11 device identity and rendering views backed by
a D3D12 resource on hosts that expose D3D11On12.
`--dxgi-surfaces` checks texture and surface identity through a D3D11 swap chain,
two surfaces created by `IDXGIDevice`, and independent subresource surfaces.
It also verifies that data written through a DXGI staging surface is visible
through the D3D11 texture.
`--factory-d3d12` selects the D3D12 runtime through `ID3D12DeviceFactory`.
`--dxgi-events` checks D3D11 queue completion through `EnqueueSetEvent` and
`Flush1`, plus DXGI adapter notification registration on DXGI 1.6 hosts.
`--gate-first-frame` attaches a swap chain, calls `Present(DXGI_PRESENT_TEST)`,
and waits for Space. The software message should remain visible until Space
starts GPU rendering. Press F12 to repeat the transition on the other API.
D3D11 presents through `Present` and D3D12 uses `Present1` in this mode.

D3D11 queries `IDXGIDevice`, resolves its adapter and parent factory, and checks
that DXGI, the swap chain, its back buffer, and immediate and deferred contexts
report the same device identity. `--legacy-d3d11`
selects `D3D11CreateDeviceAndSwapChain`. D3D12 selects an enumerated adapter,
resolves the device entry dynamically, and passes a direct command queue to the
factory. It checks the device identity returned by the queue, the swap chain,
and repeat creation. It exercises an already-reached fence, GPU queue waits and
signals, and closing a duplicated event handle while a wait is registered.
Rendering uses an auto-reset Win32 event for one frame in flight. Hosts exposing `ID3D12Device1`
also exercise both multiple-fence completion modes. Both renderers present to a
guest HWND, check that zero-size `ResizeBuffers` selects the current client
size, and recreate the render targets.

The modern D3D11 and D3D12 paths request the DXGI frame-latency handle, check
that two calls return distinct handles, and close one before using the other.
They wait before drawing each frame and preserve the latency flag on resize.

This fixture covers one windowed swap chain per session. Fullscreen-exclusive
mode and multisampled swap chains are outside this test. The added identity
and factory checks still require a build and an Xbox run.
