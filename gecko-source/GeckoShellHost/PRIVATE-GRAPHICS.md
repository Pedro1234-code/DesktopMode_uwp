# Independent Web App graphics — experimental implementation

## Scope

Integrated managed Web App roots use an independent D3D11 device, ANGLE EGLDisplay, GLContext, EGLSurface and swap chain. They remain independent browser windows inside the existing Gecko runtime, with the existing app-mode chrome hiding browser controls. This is not another XRE initialization, OS process, profile or storage partition.

The primary browser keeps its SingletonGL/device route. Legacy popup routing is unchanged. A device-wide or physical GPU reset can still affect multiple logical devices; this change is not a proven fix until exercised on Xbox.

## Implementation

- `RenderCompositorEGL::Create` recognizes managed native widgets and creates a hardware D3D11 device on the primary device's adapter. It creates a separate EGL display/context without GL resource sharing. Creation retries without optional video support if necessary. It does not replace DeviceManagerDx's primary device.
- WebRenderAPI supplies no process-wide GL shaders or program cache to private renderers. WebRender creates renderer-owned shader resources, as supported by its nullable shared-shaders interface.
- The managed root route is selected before the primary compositor's software options. If private graphics creation fails, only that window falls back to SWGL. Failure does not borrow the primary device/context.
- Each private EGLDisplay owns the EGLDevice wrapper it creates and releases it after display termination when its last reference disappears. Legacy display ownership is unchanged. DXGI importers can retain context references until their texture resources are released.
- DXGI importers restore the destination GL context after destroying an old import and invalidate native resources cached for another device. GPU-process texture IDs referring to shareable resources are reopened on the destination device, using an NT shared handle or a borrowed legacy handle as appropriate. A resource that cannot be shared is rejected rather than passed to the wrong device.
- Managed render passes decline the primary browser's hardware video overlay. Video is composited into their own surface. The primary overlay reopens a texture on the primary device when another renderer previously imported it.
- Renderer errors and detected private device loss mark that native request failed and request closure of that window, without invoking the process-wide graphics fallback from those paths. Shader/context initialization failures are also reported to that request. The Shell's existing failure/retry UI remains responsible for subsequent retry.
- Primary-browser device reset handling and process-wide GPU recovery remain Gecko mechanisms. This is logical renderer isolation, not complete process or physical GPU fault containment.

## Diagnostics

`private-window-v1` records identify native window ID, private device, primary device, context object and EGL display. `frame-routing-v1` now includes display identity: EGL handle numbers may be the same on different displays and should not be compared without the display. Device snapshots for private compositors query their own D3D11 device.

If creation fails, the log explicitly reports `window-only software fallback`. A private renderer failure reports its window ID and stage before marking the request failed.

## Validation status

Reviewed context/device ownership, nullable WebRender shader construction, DXGI shared-handle paths, video overlay routing, creation/render failure reporting and private-device removal detection. Scoped source diff checks passed. **No build or runtime tests were run**, per the user's instruction. Xbox GPU behavior remains unverified.

Next runtime checks: primary alone; Web App alone; both opening orders; two Web Apps; window resize/minimize/restore; closing/reopening; WebGL and video. Confirm distinct D3D device identities and EGL displays, and that opening/closing a Web App does not trigger primary software fallback. Additional device, shader and texture caches increase memory use and first-window setup cost.

## Backup

All 12 modified existing files were copied before editing to:

`Backups/Strawfox-multiwindow-2026-11-02/private-graphics-20261103-211417`

The paths beneath that directory mirror `C:/firefox-source/engine/firefox`. Rebuild the engine and package its updated runtime into the Shell to use this implementation. No changes to `C:/firefox-source/app` were required.
