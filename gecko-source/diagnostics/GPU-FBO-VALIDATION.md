# Multiwindow GPU correction — 2026-11-04

## Evidence and scope

The supplied Xbox logs show device reset on both logical D3D11 devices. They do not establish which GPU command caused the reset. Separate devices did not isolate that failure. The explicit-FBO change bypasses the managed window's native presentation path, while retaining GPU rendering and the existing bitmap delivery to XAML. Primary Strawfox presentation is unchanged.

An executable reproduction using the already-built `C:/rw-obj/dist/bin/libEGL.dll` and `libGLESv2.dll` found a separate defect in the proposed pbuffer readback path. On this PC, a 256x128 pbuffer returned entirely zero pixels after a red clear, with GL error zero. An explicit RGBA8 framebuffer on the same binaries returned the expected pixels.

The extended test creates two hardware D3D11 devices, independent EGL displays and ES3 contexts. Each context owns a shader program and framebuffer with RGBA8 color and depth24/stencil8 attachments. It switches contexts, renders a fullscreen triangle with a changing color, reads the complete image and compares every pixel.

Executed without any engine build:

```powershell
& C:\mozilla-build\python3\python.exe gecko-source\diagnostics\egl-multiwindow-smoke.py C:\rw-obj\dist\bin --fbo --width 1716 --height 1044 --frames 120
```

Result: `PASS: 240 full-frame readbacks, two hardware devices`. Renderer was AMD Radeon Graphics through hardware D3D11, driver 32.0.21043.21001. Earlier 256x128 clear/readback test passed 480 frames. Context handles were 1 and 2, so the supplied evidence does **not** demonstrate a context-handle collision.

This does not execute the newly edited C++ compositor, WebRender, XAML presentation or Xbox driver. It verifies the replacement graphics primitive on the existing binaries. Xbox reset resolution remains unverified, and synchronous GPU readback plus bitmap upload has a performance cost.

## Applied changes

- Managed windows render into an explicit framebuffer. The pbuffer only makes the EGL context bindable and stays 16x16. WebRender captures the bound read/draw framebuffer at `Device::begin_frame`, so no Rust API changes are required.
- Resize allocates and validates replacement attachments before releasing the previous target. Failed allocation does not publish an incomplete image.
- Framebuffer, depth attachment and color texture are released in their owning GL context.
- `SetEGLSurfaceOverride` returns the forced bind result. `RenderCompositorEGL::MakeCurrent` stops on failure, rather than relying on a debug-only assertion.
- `IsCurrentImpl` checks display as well as context. Routing logs now query the actual display and separately print the expected one.
- Existing preference `widget.gecko-w10m.webapp-direct-present=false` selects readback. Setting it to true selects the previous direct presentation route for newly created app windows.

Backups are under `Backups/Strawfox-multiwindow-2026-11-02/explicit-fbo-20261104-142716` and `egl-binding-20261104-142902`.

## References

- [Khronos eglMakeCurrent](https://registry.khronos.org/EGL/sdk/docs/man/html/eglMakeCurrent.xhtml) documents the current display/context/surface bindings and the corresponding query functions.
- [Microsoft device removal guidance](https://learn.microsoft.com/en-us/windows/uwp/gaming/handling-device-lost-scenarios) distinguishes detecting removal from investigating its reason.
- Local WebRender `gfx/wr/webrender/src/device/gl.rs`, `Device::begin_frame`, reads `READ_FRAMEBUFFER_BINDING` and `DRAW_FRAMEBUFFER_BINDING` each frame.
