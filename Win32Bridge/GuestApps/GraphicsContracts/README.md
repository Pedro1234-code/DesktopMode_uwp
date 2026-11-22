# D3D11 and D3D12 presentation fixture

Build `GraphicsContracts.vcxproj` as Release x64 in Visual Studio and open the
generated PE in DesktopMode. The window should continuously change color and
keep drawing after it is resized. F12 switches between D3D11 and D3D12, and Esc
closes the window. `--d3d12` starts directly in D3D12 mode.
`--shared-d3d12` additionally exercises a named shared D3D12 fence and
duplicates its handle before reopening the fence.
`--shared-d3d11` checks creation and reopening of a shared texture by NT handle.
`--factory-d3d12` selects the D3D12 runtime through `ID3D12DeviceFactory`.
`--dxgi-events` checks D3D11 queue completion and DXGI adapter notification
registration when the host supports DXGI 1.6.

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
guest HWND and recreate render targets after `ResizeBuffers`.

The modern D3D11 and D3D12 paths request the DXGI frame-latency handle, check
that two calls return distinct handles, and close one before using the other.
They wait before drawing each frame and preserve the latency flag on resize.

This fixture covers one windowed swap chain per session. Fullscreen-exclusive
mode, multisampled swap chains, and device lookup through D3D11 resources other
than the back buffer are outside this test. The added identity and factory
checks still require a build and an Xbox run.
