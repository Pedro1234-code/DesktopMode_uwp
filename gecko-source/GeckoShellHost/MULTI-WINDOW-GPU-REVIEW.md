# Multi-window GPU reset review — 2026-11-03

## Evidence from the latest Xbox session

At 18:55:34 the managed window has its own panel, reports `overlay=0`, and successfully presents through EGL. At 18:55:36 the primary 2740x1440 chain reports Present failure with device reason `0x887a0007` (RESET). The reset handler sees two renderers. Both then fall back to software. The reset precedes opening the visible primary browser window. There is no new AV entry for this session. VRAM usage reported at failure is 294 MB against a 3801 MB budget; this does not establish memory exhaustion.

The earlier stale InitData ABI symptom is absent: the managed window now reports TopLevel rather than Popup. This does not establish that all remaining graphics paths are correct.

## Contracts checked against documentation and implementation

- Microsoft explicitly supports multiple swap chains, each bound to a separate SwapChainPanel. Two panels are not inherently unsupported. See https://learn.microsoft.com/en-us/uwp/api/windows.ui.xaml.controls.swapchainpanel
- SetSwapChain must run on the panel's UI thread. The native adapter dispatches this operation to the owning dispatcher. Rendering itself may run in the render thread. See https://learn.microsoft.com/en-us/windows/win32/api/windows.ui.xaml.media.dxinterop/nf-windows-ui-xaml-media-dxinterop-iswapchainpanelnative-setswapchain
- A device RESET can indicate malformed GPU commands. The Present reporting the reset need not be the command which caused it. See https://learn.microsoft.com/en-us/windows/uwp/gaming/handling-device-lost-scenarios
- Multiple chains share device queue capacity. Microsoft warns against a device-wide maximum frame latency of one when multiple chains present each frame. No such override was found in the inspected compositor-device and adapter paths, so changing latency is not supported by current evidence. See https://learn.microsoft.com/en-us/windows/win32/direct3ddxgi/dxgi-multiple-swap-chains
- RenderCompositorEGL borrows RenderThread's singleton GL context. Each compositor owns an EGLSurface and sets its override in MakeCurrent. GLContextEGL forces rebinding when the override changes. ANGLE Context::makeCurrent invalidates dirty state and replaces its default framebuffer. Sharing the context alone therefore does not establish a bug.
- Managed panels retain independent metrics and references. Missing managed panel bindings do not fall back to the primary panel.
- EngineView::Stop removes the host's XAML Rendering callback; it does not pause the engine's native renderer. Consequently a hidden primary window can still render while an app window is open. Treating this method as GPU suspension would be incorrect.

## Diagnostic change

RenderCompositorEGL now logs bounded `frame-routing-v1` records before binding, after binding, after WebRender drawing/before presentation, and after presentation. Records include widget key, per-compositor frame number, thread, expected surface, current EGL draw/read surfaces, current context, widget dimensions, and device removal reason. No page URLs, keys, or text are recorded.

The first eight frames, periodic samples, and a bounded number of abnormal records are logged. A different surface at the pre-bind stage is normal. A different surface after binding or drawing would establish a routing violation. A healthy device before binding and failed device before Present narrows failure to drawing or earlier asynchronous work; it still does not identify an individual GPU instruction. A healthy device immediately before Present followed by a reset also cannot exclude earlier asynchronous work.

This change does not force software rendering, disable multi-window rendering, recreate the device, or alter popup routing. It is not a proven fix for the reset. Source diff checks passed; no build or Xbox execution was performed. Rebuilding and packaging the engine is necessary for these new records to appear.

## Expanded command diagnostics

ANGLE now retains a shared 128-entry CPU breadcrumb ring across swap chains. Each chain has a monotonic diagnostic ID. Entries identify the panel, swap chain, D3D device/context, thread, tick, frame, dimensions, orientation, copy path, present interval, HRESULT, and device removal reason. The first observed device loss in each chain dumps the ring. Normal frames avoid writing every breadcrumb to disk.

Checkpoints bracket vertex upload, copy drawing, and Present; the submitted rectangles are checked against the logical buffer bounds. Resource snapshots describe backbuffer/offscreen dimensions, format, multisampling, bind flags, and device flags. Failed copy initialization and Map calls are logged explicitly. Failure reporting retrieves up to 32 D3D11 InfoQueue messages when the interface exists.

Up to four outstanding EVENT queries per chain probe actual GPU completion, sampling the first 48 frames and every 120th frame thereafter. Polls use DONOTFLUSH, have no retry loop or wait, and report submission frame, completion result, and elapsed time. Completion proves that work preceding that checkpoint completed on the immediate context. A failed/pending query does not identify a specific invalid instruction. Destruction releases outstanding queries.

For this diagnostic revision, GeckoW10m compositor-device creation attempts the D3D11 debug layer, logging whether it was accepted. If unavailable, it retries normal device creation. No break-on-error behavior is added. Validation can affect performance when available; it should be removed or made opt-in after diagnosis. These engine changes also affect the standalone if it uses this same engine build. Existing explicit break-on-error/debug preferences retain their existing behavior.

Backup of the three files before this expansion: `Backups/Strawfox-multiwindow-2026-11-02/angle-command-trace-20261103-191722`. Changes were reviewed statically and checked for whitespace errors, without building or running them on Xbox.

## First diagnostic run (20:11)

The EGL draw/read surface matches the expected surface after binding and after drawing in all recorded frames. Both windows share context 1 and the observed render thread. Device removal is first observed after the primary chain's eighth Present; its seventh Present already took approximately 840 ms. Outstanding GPU queries subsequently fail, including a before-copy checkpoint submitted roughly 1.9 seconds earlier. This establishes stalled/incomplete GPU work but does not locate the responsible instruction.

The D3D11 validation layer was active (device flags 0x822). InfoQueue contains two SetPrivateData size warnings, without an invalid rendering-command error. The managed renderer's initial reported widget size is 2740x1440 while its EGL surface is 1716x1044; subsequent frames converge to 1716x1044. This is a sizing discrepancy to investigate, not a proven cause of device reset.

The host's quiet ANGLE log filter inadvertently discarded the newly added normal command records and most of the failure ring. Only records containing words such as `failed` survived. EngineView.cpp now explicitly preserves the bounded `angle: commands-v1 ` and `angle: breadcrumb ` families through that filter. Privacy redaction in Log.cpp still applies. This filter correction requires a host/Shell rebuild; the engine diagnostics are already present in this run.

## Managed viewport ownership correction

Managed window requests now retain the Shell's physical viewport before CreateTopLevelWindow. HeadlessWidget::Create uses that size before exposing compositor initialization data. ResizeInternal resolves that authoritative viewport for managed roots after browser constraints, preventing browser startup/persisted dimensions from resizing the native surface independently of its panel. The per-window resize ABI updates the stored viewport before dispatching the resize. Primary browser and popup sizing are unchanged; InitData layout and public ABI are unchanged.

This corrects the observed initial size disagreement. It is not established that the disagreement caused the GPU reset. Backup: `Backups/Strawfox-multiwindow-2026-11-02/managed-viewport-20261103-202410`. Source diff checks passed; no build or runtime validation was performed. This correction requires rebuilding the engine, together with rebuilding the host for the log-filter correction.

## Run with corrected viewport (20:40)

The managed widget and surface both use 1716x1044 from the first frame; the reset persists. Surface binding remains correct. The complete CPU history shows valid copy rectangles and GPU EVENT-query completions for the primary chain through frame 9, as well as the managed chain's earlier frames. The primary chain presents frame 10 with S_OK; the reset is subsequently detected by Gecko at 20:40:12.911. D3D11 InfoQueue again reports only SetPrivateData name-size warnings. The size disagreement was a real bug but is not a sufficient explanation for the reset.

### Driver-level diagnostics added after this run

- Before compositor-device creation, request DRED automatic breadcrumbs and page-fault tracking through D3D12GetDebugInterface, dynamically resolved from an already-loaded d3d12.dll. Log module/entry/settings availability. No new mandatory D3D12 DLL import is added.
- At first Gecko reset detection, dynamically call an optional ANGLE diagnostic export **before cached GPU resources are released**. The observer queries ID3D11On12Device1 and, if supported, the underlying device and ID3D12DeviceRemovedExtendedData. Log HRESULTs for unavailable interfaces.
- Read at most 16 command-list nodes, a small operation window around the completion counter, and eight existing/freed allocation candidates each. Respect DRED's 65536-operation history ring. Do not emit resource names, PIX strings, shader data, or resource contents. GPU virtual addresses and object identities are technical correlation data.
- At chain creation, record the actual device-wide maximum frame latency and swap-chain descriptor; do not assume the default or change latency without evidence.

DRED is optional, can add execution/memory overhead when enabled, and may not be supported by the Xbox runtime or the D3D11 driver. Even when present its completion counter does not guarantee the precise faulting operation because GPU work is pipelined. This is diagnostic instrumentation, not a claimed reset fix. Rebuild/package both xul and the ANGLE runtime to obtain these records. No build was run.

Sources: https://microsoft.github.io/DirectX-Specs/d3d/DeviceRemovedExtendedData.html and https://devblogs.microsoft.com/directx/direct3d-11-on-12-updates/

Backups: `driver-removal-20261103-205809` (SwapChain11.cpp, NativeWindow11WinRT.cpp, DeviceManagerDx.cpp), `driver-reset-entry-20261103-210004` (SwapChain11.cpp, RenderThread.cpp), under the same multi-window backup directory.
