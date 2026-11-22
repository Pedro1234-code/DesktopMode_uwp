# DesktopMode UWP

## Windows 10X Desktop Mode for Xbox

<img width="1920" height="1080" alt="Screenshot_2026-09-27_18-09-01" src="https://github.com/user-attachments/assets/1cd6935c-8be1-410c-be71-29c23170463f" />
<img width="1920" height="1080" alt="Screenshot_2026-09-27_18-19-07" src="https://github.com/user-attachments/assets/b9172422-2a87-4996-8c90-6ee677472254" />
<img width="1920" height="1080" alt="Screenshot_2026-09-27_18-23-12" src="https://github.com/user-attachments/assets/9ab6e6e5-0fb1-4546-b713-b01afc43c067" />
<img width="1920" height="1080" alt="Screenshot_2026-09-28_14-36-06" src="https://github.com/user-attachments/assets/2bcda159-101b-45a7-85a5-43a11845b8aa" />


DesktopMode is a UWP shell inspired by the Windows 10X design language and adapted for both Windows and Xbox. It recreates a lightweight desktop experience inside a single application, including a Start menu, taskbar, window management, Task View, built-in apps, and Web Apps.

> DesktopMode does not replace the Xbox operating system. It runs as a UWP application and operates within platform limitations.

## Highlights

- Windows 10X-inspired interface with light and dark theme support.
- Native USB mouse support.
- Custom mouse cursor and DPI/scale adjustments for Xbox use.
- Start menu with system apps, Web Apps, and pinned items.
- Taskbar with open-window indicators, context menus, and persistent pinned items.
- Task View, `Alt` + `Tab` switching, and window focus management.
- Installable Web Apps powered by WebView2, with favicons, multiple windows, resizing, minimize, maximize, and close controls.
- Files: a file browser built around UWP permissions and folder pickers, with tabs and Files-based UI.
- Calculator: actual Windows Calculator included as an internal app.
- Notepad: open, edit, save, and save-as support for `.txt` files, with tab-based UI.
- Settings: a built-in app with personalization, taskbar, device information, and a visual Windows Update page.
- A custom Action Center with volume, network status, and system settings shortcuts.
- Strawfox: a built-in Firefox port running in its own window, which the Shell keeps maximized and non-draggable, with Download, Upload and Add-on support.
- Native zip, 7z and RAR extraction from Files app context menu.
- Experimental PE loader for Win32 binaries.

## Project structure

| Folder/project | Description |
| --- | --- |
| `factoryos-10x-shell` | Main UWP application and DesktopMode interface. |
| `factoryos-10x-shell.Library` | Shared models, services, and view models. |
| `Win32Bridge` | Win32Bridge project files. |
| `Calculator-main` | Windows Calculator project files |
| `Icons` | Resource Windows 11 icons |


## Win32 Bridge

<img width="1280" height="720" alt="image" src="https://github.com/user-attachments/assets/11fe1117-783e-4683-967e-36dc7455e2e7" />


Desktop Mode includes an highly experimental bridge for loading Win32 PEs in .exe format. The .exe has no acess to the real Xbox or PC filesystem, registry or OS, it runs on a virtual drive inside the app's LocalStorage (drive_c). For now, it can render most of 7-Zip, can create a window, and a few more things. Most PEs simply won't load by now, compatibility is meant to be expanded with time.

[Win32Bridge Documentation](Win32Bridge/BRIDGE.md)

[Win32Bridge Compatibility Map](Win32Bridge/BRIDGE.md#compatibility-map)

## Enable Loopback
In order to be able to use Xbox Device Portal directly from Strawfox, and use the (planned) Appx Installer feature, you must enable loopback every reboot. Here's how to do it:

- Start an SSH connection using whatever client on your phone or PC (needs to be on the same network as the Xbox)
- User: DevToolsUser
- Password: the Visual Studio PIN showed on Dev Home

Once connected to the shell, run: CheckNetIsolation.exe LoopbackExempt -a -n="Microsoft.MicrosoftEdge.CoreShell_8wekyb3d8bbwe"

You must do it every time the console boots.

## Discord

Join our server: https://discord.gg/dEKYCqFXt

## Building

1. Install Visual Studio with C# and C++ UWP development tools, the **MSVC v143 x64/x86** build tools, **C++ (v143) Universal Windows Platform tools**, and Windows SDK `10.0.26100.0`. The shell also references the **Microsoft.VCLibs.Desktop 14.0** Extension SDK.
2. Build Gecko and configure its paths as described below.
3. Open [`factoryos-10x-shell.sln`](factoryos-10x-shell.sln), restore the NuGet packages, and select **Release | x64** (or **Debug | x64** for debugging).
4. Build the solution with `factoryos-10x-shell` as the startup project. To deploy to an Xbox in Developer Mode, select **Remote Machine** and enter the console's IP address, or use **Publish > Create App Packages** to create an x64 sideload package.

If Visual Studio reports that the signing certificate is unavailable, create or associate a development certificate with the package before deployment. Note that, in order for the Mouse APIs to work on Xbox, Microsoft's CN must be used on the cert.

### Build Gecko for integrated Strawfox

Follow the prerequisites and **Clone the source and build Gecko** section of the [Strawfox x64 build guide](https://github.com/Pedro1234-code/Strawfox-UWP#build-for-xbox-x64). The integrated host uses the same engine build. Continue here after the engine build succeeds and produces `dist\bin`, `dist\include`, and `dist\lib`. The standalone Strawfox app package is unnecessary for this build.

In this repository, copy [`gecko-source/GeckoShellHost/GeckoBuild.local.props.example`](gecko-source/GeckoShellHost/GeckoBuild.local.props.example) to `GeckoBuild.local.props` in the same directory and set these properties to match your machine:

| MSBuild property | Example | What it points to |
| --- | --- | --- |
| `GeckoW10mSource` | `C:\strawfox\engine\firefox` | Gecko submodule checkout used to build the engine |
| `GeckoW10mObj` | `C:\rw-obj` | Object directory set by `GECKO_W10M_OBJ`, containing `dist` |
| `GeckoW10mClangCl` | `C:\Program Files\LLVM\bin\clang-cl.exe` | Full path to the LLVM compiler |

The local props file is ignored by Git. On a checkout without that file, you can also set `GECKO_W10M_SOURCE`, `GECKO_W10M_OBJ`, and `GECKO_W10M_CLANG_CL` before launching Visual Studio. The Desktop Mode solution builds the embedded host and packages the required runtime files from Gecko's `dist\bin`. After changing Gecko, rebuild the engine and regenerate the Desktop Mode package to include the new runtime.

## Known limitations

- Access to files outside the application's local storage depends on access granted by the user through UWP file and folder pickers.
- External applications are launched through URIs when the package supports them. If a UWP app doesn't include an URI, it can't be launched through Desktop Mode on Xbox.
- Strawfox is not resizable and crashes on fullscreen.
- I strongly recommend you to use it with a mouse and a keyboard
- Some microstutters on Xbox that are being investigated.

## Credits and license

This project builds on the original Windows 10X shell recreation by [Pdawg-bytes](https://github.com/Pdawg-bytes/factoryos-10x-shell), expanded into DesktopMode with Xbox- and desktop-focused features.

Credits to Wine for concept and code used as reference.

Special thanks to ReactOS devs for their code which was used as reference.

Credits to https://github.com/Futur3Sn0w/Windows10x for inspiration and some Icons from there.

Credits to https://github.com/files-community/files/ for Icons and UI for Files internal app.

AI was used in the making of this project.

Distributed under the [MIT License](LICENSE).
