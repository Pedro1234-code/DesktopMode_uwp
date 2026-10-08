# Embedded Firefox host

This Windows Runtime Component is the DesktopMode-owned copy of the native
host from `C:\firefox-source\app_ShellIntegration\GeckoW10m`.

Only the engine-facing host was brought over: `EngineView`, the Gecko
bootstrap, logging, download/file-picker brokerage, DRM bridge and video
overlay support. The standalone UWP application (`App.cpp`, `MainPage.cpp`,
manifest and package entry point) is intentionally not part of this project.
`C:\firefox-source\app` is not a source or build dependency and must remain
unchanged.

`GeckoHost` exposes one XAML `UIElement` to the C# Shell. Gecko/XRE and the
headless widget bridge are process-global, so the component deliberately owns
one runtime; Firefox continues to own its tabs and complete browser UI inside
that one DesktopMode window.

The defaults expect the existing engine checkout and object directory:

- `GeckoW10mSource=C:\firefox-source\engine\firefox`
- `GeckoW10mObj=C:\rw-obj`
- `GeckoW10mClangCl=C:\Program Files\LLVM\bin\clang-cl.exe`

They are MSBuild properties and can be overridden without editing the project.
The project copies the Gecko runtime payload into the DesktopMode package as
architecture files, outside the PRI resource index.

The component is intentionally x64-only, matching the current Gecko object
directory and the DesktopMode bundle target. NuGet restore supplies
`Microsoft.Windows.CppWinRT`, which generates the component projection from
`GeckoHost.idl`.

Closing the internal window removes it from Task View/taskbar and stops its
render/input forwarding, but it does not try to tear down XRE. The embedded
Firefox runtime is process-global and is reused if the window is opened again;
an in-process Gecko restart is not a supported or safe lifetime boundary.
