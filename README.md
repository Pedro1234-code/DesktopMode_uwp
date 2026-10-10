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
- Notepad: open, edit, save, and save-as support for `.txt` files.
- Settings: a built-in app with personalization, taskbar, device information, and a visual Windows Update page.
- A custom Action Center with volume, network status, and system settings shortcuts.
- Strawfox: a built-in Firefox port running with its own window, with Download, Upload and Add-on support.
- Experimental PE loader for Win32 binaries

## Project structure

| Folder/project | Description |
| --- | --- |
| `factoryos-10x-shell` | Main UWP application and DesktopMode interface. |
| `factoryos-10x-shell.Library` | Shared models, services, and view models. |
| `Win32Bridge` | Auxiliary integration components, where applicable. |

## Win32 Bridge

<img width="1920" height="1080" alt="Screenshot_2026-09-27_18-21-56" src="https://github.com/user-attachments/assets/8211b974-8c58-4bfc-b5eb-1b995802ff79" />

Desktop Mode includes an highly experimental bridge for loading Win32 PEs in .exe format. The .exe has no acess to the real Xbox or PC filesystem, registry or OS, it runs on a virtual drive inside the app's LocalStorage (drive_c). For now, it can render most of 7-Zip, can create a window, and a few more things. Most PEs simply won't load by now, compatibility is meant to be expanded with time.

## Discord

Join our server: https://discord.gg/dEKYCqFXt

## Building

1. Install Visual Studio 2022 with the **Universal Windows Platform development** and the latest C++ workload.
2. Install Windows 10 SDK `10.0.19041` or a compatible version.
3. Open [`factoryos-10x-shell.sln`](factoryos-10x-shell.sln).
4. Build and launch the application from Visual Studio.

If Visual Studio reports that the signing certificate is unavailable, create or associate a development certificate with the package before deployment. Note that, in order for the Mouse APIs to work on Xbox, Microsoft's CN must be used on the cert.

## Known limitations

- Access to files outside the application's local storage depends on access granted by the user through UWP file and folder pickers.
- External applications are launched through URIs when the package supports them. If a UWP app doesn't include an URI, it can't be launched through Desktop Mode on Xbox.
- The Windows Update button in Settings is currently visual only; real system updates remain managed by the operating system.
- It has not been tested with a controller.

## Credits and license

This project builds on the original Windows 10X shell recreation by [Pdawg-bytes](https://github.com/Pdawg-bytes/factoryos-10x-shell), expanded into DesktopMode with Xbox- and desktop-focused features.

Credits to Wine for concept and code used as reference.

Credits to https://github.com/Futur3Sn0w/Windows10x for inspiration and some Icons from there.

Credits to https://github.com/files-community/files/ for Icons and UI for Files internal app.

AI was used in the making of this project.

Distributed under the [MIT License](LICENSE).
