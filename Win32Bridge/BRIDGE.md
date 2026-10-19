# Win32Bridge

<img width="1280" height="720" alt="image" src="https://github.com/user-attachments/assets/46d8e2f3-2ae6-495b-bfb1-903d148ac57a" />

A proof-of-concept compatibility layer designed to load and execute Windows Win32 binaries (`.exe`) within a Universal Windows Platform (UWP) application.

## Overview

**Win32Bridge** is an experimental project that explores the feasibility of running traditional Windows desktop applications inside the restricted execution environment of UWP.

Unlike conventional emulators or virtual machines, Win32Bridge aims to provide a compatibility layer between Win32 applications and the UWP runtime, allowing supported executables to operate without requiring a complete desktop Windows environment.

The project primarily targets Windows 10/11 UWP environments, with particular interest in Xbox consoles operating in Developer Mode.

## Project Goals

The primary objectives of Win32Bridge are:

- Load and execute compatible Windows Portable Executable (PE) binaries.
- Provide compatibility with essential Win32 APIs.
- Translate or implement unsupported Win32 functionality through UWP-compatible mechanisms.
- Support basic desktop application windows, controls, and graphical interfaces.
- Handle executable dependencies, memory management, and runtime initialization.
- Investigate the possibility of running traditional desktop software on Xbox consoles through UWP.
- Expand application compatibility incrementally without requiring a full Windows desktop subsystem.

## Architecture

Win32Bridge is intended to operate as an intermediary between a Win32 executable and its UWP host.

The proposed architecture consists of several components.

## How to test

Just double click on a PE from Desktop Mode Files application.

### 1. PE Loader

Responsible for interpreting and preparing Windows executable files for execution.

Potential responsibilities include:

- Parsing PE headers and sections.
- Mapping executable images into memory.
- Processing relocations.
- Resolving imported functions and dependencies.
- Preparing executable memory and runtime structures.
- Initializing the application's entry point.

Loading a PE image does not, by itself, guarantee that its machine code can execute within UWP restrictions.

### 2. Win32 API Compatibility Layer

Traditional desktop applications depend on Windows APIs that may be unavailable, restricted, or incompatible with the UWP application model.

Win32Bridge aims to implement or adapt the necessary functionality through a compatibility layer.

Relevant API categories include:

- **Kernel32:** Process initialization, memory management, file operations, and synchronization.
- **User32:** Window creation, message handling, and user interaction.
- **GDI32:** Graphics rendering and drawing operations.
- **Advapi32:** Registry access and other system services.
- **Comctl32:** Common Windows controls.
- **Shell32:** Shell-related functionality and application integration.

Many of these APIs have not been implemented yet.

### 3. Windowing and Graphics

One of the project's central challenges is supporting the graphical interfaces of traditional Win32 applications.

Desktop applications generally rely on window handles (`HWND`), window procedures, message loops, and graphics APIs that do not directly correspond to the UWP application model.

Win32Bridge therefore investigates mechanisms for representing Win32 windows and controls inside a UWP-hosted interface.

Potential responsibilities include:

- Window creation and lifecycle management.
- Win32 message dispatching.
- Rendering and invalidation.
- Keyboard and mouse input translation.
- Basic support for menus, toolbars, dialogs, and common controls.

### 4. Runtime and Dependency Management

Win32 executables may require additional DLLs, runtime libraries, and operating system functionality.

A compatibility implementation must account for dependency resolution, initialization order, and interactions between application code and the host environment.

Not all Windows desktop dependencies can be loaded or reproduced directly inside UWP.

## Execution Model

The intended execution sequence is:

1. The UWP host selects a supported Win32 executable.
2. Win32Bridge parses the executable's PE structure.
3. Required sections and dependencies are prepared.
4. Imports are resolved against available compatibility implementations.
5. The runtime environment is initialized.
6. Execution is transferred to the application's entry point, where permitted.
7. Win32 API requests are handled by the compatibility layer.
8. Application output and graphical interfaces are presented through the UWP host.

This sequence describes the intended architecture rather than a guarantee that every stage is fully implemented.

## Experimental Progress

Win32Bridge has demonstrated partial execution and graphical initialization of selected desktop applications during development.

Previously observed results include:

| Application | Experimental observations |
|---|---|
| Windows 7 Notepad | Successful graphical rendering reported in earlier tests. |
| 7-Zip | Graphical rendering achieved in earlier tests; subsequent development introduced a regression. |
| Notepad++ | Progressed beyond earlier crashes, creating a window and toolbar before terminating. |

These observations demonstrate progress toward compatibility but do not establish complete application functionality.

The current state of individual applications may differ between development revisions.

## UWP and Xbox Limitations

UWP operates under a restricted application model that differs significantly from the traditional Win32 desktop environment.

Important limitations include:

- Restricted access to operating system resources.
- Application sandboxing and filesystem restrictions.
- Limitations on process creation and execution.
- Restrictions involving executable memory and dynamic code generation.
- Differences between UWP and desktop window management.
- Limited access to unsupported or undocumented system APIs.
- Architecture-specific binary compatibility requirements.

Xbox Developer Mode introduces additional constraints and does not provide unrestricted access to the underlying Windows operating system.

**Win32Bridge does not bypass UWP security restrictions by design.** Its feasibility depends on the capabilities available to the host application and the compatibility mechanisms that can be implemented within those boundaries.

## Compatibility Considerations

Win32Bridge is not intended to provide universal Win32 compatibility in its initial stages.

Application compatibility depends on several factors, including:

- CPU architecture.
- Imported DLLs and API requirements.
- Graphical subsystem dependencies.
- Runtime libraries.
- Filesystem and registry expectations.
- Threading and synchronization behavior.
- UWP security and execution restrictions.

Applications with relatively simple dependencies and graphical interfaces are more practical initial targets than software requiring complex system integration, drivers, or privileged services.

## Future Development

Potential development priorities include:

- Improving PE loading and dependency resolution.
- Expanding Win32 API coverage.
- Stabilizing existing application compatibility.
- Improving window rendering and message processing.
- Supporting additional graphical controls.
- Implementing more complete filesystem and registry compatibility.
- Investigating additional runtime libraries and desktop application frameworks.
- Improving diagnostic logging and crash analysis.
- Evaluating compatibility across Windows UWP and Xbox Developer Mode environments.

These are development directions, not confirmed implemented features.

## Project Status

**Status: Experimental / Proof of Concept**

Win32Bridge remains a research and development project. Its purpose is to investigate how far Win32 binary compatibility can be extended within the UWP application model.

Successful loading, window creation, or partial graphical rendering should not be interpreted as complete application compatibility.

## Disclaimer

Win32Bridge is an experimental compatibility project and is not a complete implementation of the Windows desktop subsystem.

At this stage, most PEs you try loading will not even attempt to execute due to missing imports.

The project is not affiliated with or endorsed by Microsoft.
