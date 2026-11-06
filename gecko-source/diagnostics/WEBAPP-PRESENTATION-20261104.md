# Web App presentation performance — 2026-11-04

The appended run at 15:10–15:12 in gecko-notes (25).log matches WebApps-runtime (2).log run 3677efd5. It reports `texture-sync ready`, `webapp GPU explicit framebuffer readback`, successful frames with device HRESULT S_OK, and a normal managed-window close. No reset is recorded in this run. Older resets in the same files belong to earlier runs.

The Web App still read every changed image back through glReadPixels, flipped rows and swapped RGBA/BGRA on the CPU, copied into the managed-window buffer, and copied/uploaded to XAML WriteableBitmap. At 2740x1368 the image is 14,993,280 bytes; at 60 fps a single full-image traversal is about 900 MB/s. Several traversals and a GPU completion wait were involved. This is a concrete overhead absent from the primary browser's direct presentation path, although logs do not isolate its exact share of the user's 30–40 fps result.

The aggregate compositor log also records main-thread refresh stalls around 500 ms (one above 1 s). Removing readback is not proof that those separate stalls are fixed, nor a guarantee of 60 fps. Aggregate compositor FPS is not a per-window benchmark.

## Change

Changed the default of `widget.gecko-w10m.webapp-direct-present` to true in engine `widget/headless/GeckoW10mWindowRequests.cpp`. New managed windows now use their existing private-device EGL surface / SwapChainPanel route, avoiding CPU readback and WriteableBitmap presentation. Per-window SyncObject initialization and synchronization remain enabled for both presentation modes. Primary-browser rendering and popup routing are unchanged.

An explicit false preference retains the readback path for comparison. This default was not previously tested with the new synchronization on Xbox, so device-reset stability must be verified as well as performance. Do not describe the synchronization as a proven explanation of earlier resets based on this one clean run.

Expected engine log for the new default: `gfx: webapp GPU direct presentation`, together with `texture-sync ready`. `webapp GPU explicit framebuffer readback` means the old route is still selected (old engine or explicit preference override). The Shell's historical `frame-source software` label denotes bitmap transport and can also describe GPU readback; it does not by itself prove SWGL rendering.

Backup: `Backups/Strawfox-multiwindow-2026-11-02/direct-present-sync-20261104-152147/GeckoW10mWindowRequests.cpp`.

Source/diff reviewed; no build or Xbox execution performed. Recompile and package the engine to execute the new default.
