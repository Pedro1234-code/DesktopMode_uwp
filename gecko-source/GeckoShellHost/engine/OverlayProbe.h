// OverlayProbe.h — whether a video can be shown as a display layer of its own.
#pragma once

#include <winrt/Windows.UI.Xaml.Controls.h>

namespace gecko_w10m::engine {

// Asks the phone, once, whether its display supports hardware overlays and
// for which formats, and whether the pieces needed to give a video its own
// layer exist (video processor, media swap chains, panel handles). Writes the
// answers to the log ("overlay:" lines) and changes nothing. Call on the UI
// thread; the device work runs on a thread of its own.
void ProbeVideoOverlay(
    winrt::Windows::UI::Xaml::Controls::SwapChainPanel const& panel);

// The decisive experiment, for a test build: shows a panel with an NV12 swap
// chain for twelve seconds in three phases -- on top of everything; with a
// translucent band over it, as a player's controls would be; and under the
// browser's own layer, made translucent -- changes its colour every frame,
// and asks the system twice a second whether the frames went to a hardware
// overlay or were drawn by the GPU ("overlay test:" lines). Call on the UI
// thread once the browser draws.
void RunOverlayTest(winrt::Windows::UI::Xaml::Controls::Grid const& host,
                    double rawPerView);

}  // namespace gecko_w10m::engine
