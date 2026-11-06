# EGL frame completion review — 2026-11-05

External engine files changed: `gfx/webrender_bindings/RenderCompositorEGL.cpp` and `.h`.
Pre-change copies: `Backups/egl-frame-completion-20261105`.

The Windows EGL compositor inherited `GetLastCompletedFrameId()` from
`RenderCompositor`, returning the previous CPU frame ID without GPU completion
evidence. Its `UpdateFrameId()` likewise advanced the ID without a GPU marker.
`AsyncImagePipelineManager` uses this value to release deferred texture hosts,
including resources held after pipeline removal. A release fence is not a
replacement for this lifetime contract.

Ported the completion-query mechanism from `RenderCompositorANGLE`: submit a
D3D11 event query for each frame ID, including update-only frames, and advance
the completed ID only on `GetData(..., D3D11_ASYNC_GETDATA_DONOTFLUSH) == S_OK`.
Queries are recycled after completion. Pending/failed queries and removed devices
do not advance completion. No per-frame blocking wait was added.

References:
- https://bugzilla.mozilla.org/show_bug.cgi?id=1793388
- https://learn.microsoft.com/en-us/windows/win32/api/d3d11/nf-d3d11-id3d11devicecontext-getdata

This fixes a concrete resource-lifetime discrepancy. It does NOT establish that
it caused the recorded Xbox device reset. Readback often waits for GPU work, so
it can mask the discrepancy on presented frames; update-only frames and frames
without readback must also obey the lifetime contract.

Reviewed the patch against the local original ANGLE compositor and consumer.
No build or runtime validation performed. Existing readback diagnostics and
release-fence patch retained. Reset recovery policy unchanged.
