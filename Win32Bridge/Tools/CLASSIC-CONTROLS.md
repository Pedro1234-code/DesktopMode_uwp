# Classic USER32 drawing

The former DrawEdge/DrawFrameControl implementations returned a generic frame
for every request. ClassicControlShims.inl now provides generic classic control
geometry, routed through existing guest GDI primitives and the DC clipping region.
No executable name, control ID or application resource is consulted.

Implemented:
- FrameRect validates the HDC and draws four strips without filling the interior.
- DrawEdge supports rectangular raised/sunken outer and inner layers, etched and
  bump edges, selective sides, BF_MIDDLE, BF_ADJUST, BF_SOFT, BF_FLAT and BF_MONO.
  Adjustment counts requested layers and changes only requested sides.
- DrawFocusRect draws a dotted XOR outline; a second call restores prior pixels.
  Work is proportional to the perimeter rather than the rectangle area.
- DrawFrameControl distinguishes caption, menu, scroll and button types, with
  close/minimize/maximize/restore/help symbols, arrows, checkmarks, bullets,
  radio buttons, three-state checks and push buttons. Disabled and pressed
  states affect colors/placement; DFCS_ADJUSTRECT applies to push buttons.
- Drawing uses explicit system colors and existing FillRect/GetPixel/SetPixelV
  adapters, without selecting a guest font/pen or changing the HDC text colors.

Limits:
- This is a classic geometric renderer, not native theme fidelity or Marlett
  glyph rasterization. Checked backgrounds and some disabled/hot/transparent
  variations do not reproduce every native pixel/pattern.
- BF_DIAGONAL and radio image/mask composition are explicitly unsupported.
- These changes do not complete GetDCEx/non-client DC semantics, theme parts,
  layered windows, or the wider GDI transform/composition pipeline. Rectangle
  fills retain the mapping behavior of the existing BridgeFillRect adapter.
- No C++/C# build, bridge execution or Xbox screenshot validation was performed.

Reference study: ReactOS subsystems/win/win32k/user/user32/windows/draw.c,
with Windows SDK WinUser.h constant values. Implementation uses independent
geometry rather than copying the ReactOS color tables or glyph code.

Test-ClassicControls.py passed 78 native USER32 contracts on a memory DC,
covering edge layer/side adjustment, normal edge colors, push-button adjustment,
valid frame-control variants and XOR reversibility. This exercises native
Windows only; it is a reference for subsequent Bridge runtime validation.
