# D3D11 reset review — 2026-11-04

## Findings

The last appended run in `gecko (25).log` used a private hardware device and an explicit framebuffer readback, not the Web App's swapchain. It delivered over 1200 frames before both devices reported DXGI_ERROR_DEVICE_RESET. Removing private Present did not eliminate the reset. Separate logical devices therefore did not provide physical GPU fault isolation.

The 684 MB VRAM usage against a 3801 MB budget does not establish out-of-memory. The shader-resource/output binding warnings do not identify the command that reset the device. The first device observed as failed is not necessarily the device that submitted the offending command.

The Windows compositor (`RenderCompositorANGLE`) creates a `SyncObjectHost`, publishes its handle through `WebRenderAPI`, and synchronizes it before drawing when required. Our Windows-on-headless `RenderCompositorEGL` advertises ANGLE/D3D11 texture support but did not initialize that object. `RenderDXGITextureHost::SyncObjectNeeded()` explicitly identifies shared-handle textures lacking their own keyed mutex or fence as requiring this synchronization. Those textures had no compositor synchronization handle available.

This is a source-level synchronization omission. The logs do not prove that such a texture was active at the moment of the reset. GPU-process-ID textures are a separate case: their wait/fence/producer-ordering contract still needs to be respected when importing across devices; the change below does not claim to solve every cross-device texture path.

## Applied correction

- Initialize the existing Gecko `SyncObjectHost` on each managed Web App's actual private device, before returning its compositor to `WebRenderAPI`. The existing API then publishes the correct handle to clients.
- Follow the Windows compositor's conditional, fallible synchronization before drawing.
- Refuse private compositor initialization if synchronization cannot be initialized.
- On synchronization failure, stop rendering that managed window and report its graphics failure once. Log the actual device HRESULT separately; do not synthesize a global physical-device reset or keep retrying the blocking wait every frame.
- Leave primary-browser creation/rendering and popup routing unchanged.

Modified: `gfx/webrender_bindings/RenderCompositorEGL.cpp` and `.h` under the engine root. Backup: `Backups/Strawfox-multiwindow-2026-11-02/texture-sync-20261104-150116` in DesktopMode_uwp.

No build or Xbox validation was performed. Scoped diff whitespace checks passed; reviewed the existing SyncObjectHost factory, handle publication, failure callback and conditional-compilation boundaries. Rebuilding the engine is necessary to execute this change. This is not evidence that the observed reset is fixed.

## Sources and limits

- [Microsoft DXGI error definitions](https://learn.microsoft.com/en-us/windows/win32/direct3ddxgi/dxgi-error): DEVICE_RESET is a command/device failure, not an out-of-memory diagnosis.
- [Microsoft shared resources](https://learn.microsoft.com/en-us/windows/win32/api/d3d11/nf-d3d11-id3d11device-opensharedresource): importing a resource is distinct from submitting producer updates.
- [Microsoft keyed mutex synchronization](https://learn.microsoft.com/en-us/windows/win32/api/dxgi/nf-dxgi-idxgikeyedmutex-acquiresync): exclusive access is required for keyed shared resources.
- [Microsoft D3D11 threading](https://learn.microsoft.com/en-us/windows/win32/direct3d11/overviews-direct3d-11-render-multi-thread-intro): a device's thread safety does not imply concurrent immediate-context safety. The existing private-device multithread protection does not replace cross-device GPU ordering.
- [Mozilla bug 2083416](https://bugzilla.mozilla.org/show_bug.cgi?id=2083416): documents compositor/device SyncObject mismatch. The reported effects there are tearing/wrong frames, not proof of our reset cause.

Avoid further changes to Present, context architecture, or global hardware fallback based solely on the HRESULT. The remaining cross-device imports and resource lifetime must be audited against their producer synchronization, rather than treating a successful OpenSharedResource as proof that a texture is ready to read.
