# DesktopMode

DesktopMode is a UWP shell inspired by the Windows 10X design language and adapted for both Windows and Xbox. It recreates a lightweight desktop experience inside a single application, including a Start menu, taskbar, window management, Task View, built-in apps, and Web Apps.

> DesktopMode does not replace the Xbox operating system. It runs as a UWP application and operates within platform limitations.

## Highlights

- Windows 10X-inspired interface with light and dark theme support.
- Custom mouse cursor and DPI/scale adjustments for Xbox use.
- Start menu with system apps, Web Apps, and pinned items.
- Taskbar with open-window indicators, context menus, and persistent pinned items.
- Task View, `Alt` + `Tab` switching, and window focus management.
- Installable Web Apps powered by WebView2, with favicons, multiple windows, resizing, minimize, maximize, and close controls.
- Files: a file browser built around UWP permissions and folder pickers, including removable media after user authorization.
- Notepad: open, edit, save, and save-as support for `.txt` files.
- Settings: a built-in app with personalization, taskbar, device information, and a visual Windows Update page.
- A custom Action Center with volume, network status, and system settings shortcuts.

## Project structure

| Folder/project | Description |
| --- | --- |
| `factoryos-10x-shell` | Main UWP application and DesktopMode interface. |
| `factoryos-10x-shell.Library` | Shared models, services, and view models. |
| `Windows10x-js-main` | HTML/CSS/JS reference implementation of the original Windows 10X recreation. |
| `Win32Bridge` | Auxiliary integration components, where applicable. |

## Building

1. Install Visual Studio 2022 with the **Universal Windows Platform development** workload.
2. Install Windows 10 SDK `10.0.19041` or a compatible version.
3. Open [`factoryos-10x-shell.sln`](factoryos-10x-shell.sln).
4. Select the appropriate architecture:
   - `x64` for local PC testing;
   - `ARM` for Xbox, when using a compatible development environment.
5. Build and launch the application from Visual Studio.

If Visual Studio reports that the signing certificate is unavailable, create or associate a development certificate with the package before deployment.

## Known limitations

- Access to files outside the application's local storage depends on access granted by the user through UWP file and folder pickers.
- External applications are launched through URIs or protocols when the package supports them.
- The Windows Update button in Settings is currently visual only; real system updates remain managed by the operating system.
- Some behavior can vary across PC, Xbox One, and Xbox Series devices because of UWP and Xbox platform restrictions.

## Credits and license

This project builds on the original Windows 10X shell recreation by [Pdawg-bytes](https://github.com/Pdawg-bytes/factoryos-10x-shell), expanded into DesktopMode with Xbox- and desktop-focused features.

Distributed under the [MIT License](LICENSE).
