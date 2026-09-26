#include "Bridge\\CompatibilityCatalog.h"
#include "Bridge\\ImportBinder.h"
#include "Bridge\\PeImage.h"
#include "Bridge\\PeMapper.h"

#include <fstream>
#include <iostream>
#include <iterator>
#include <vector>
#include <cstring>

namespace
{
    struct ResolverFixture
    {
        const wchar_t* description;
        const wchar_t* library;
        const wchar_t* name;
        Win32Bridge::Bridge::ImportDisposition expectedDisposition;
    };

    // These cases intentionally do not depend on the embedded demo's current
    // import table. They keep the catalog contract visible as the bridge gains
    // adapters before a new guest fixture has to be compiled and staged.
    bool ValidateResolverFixtures()
    {
        using Win32Bridge::Bridge::CompatibilityCatalog;
        using Win32Bridge::Bridge::ImportDisposition;
        using Win32Bridge::Bridge::ImportedSymbol;

        const ResolverFixture fixtures[] =
        {
            { L"USER32 window creation", L"USER32.DLL", L"CreateWindowExW", ImportDisposition::NeedsBridge },
            { L"USER32 cursor bootstrap", L"user32.dll", L"LoadCursorW", ImportDisposition::NeedsBridge },
            { L"USER32 virtual desktop metrics", L"api-ms-win-ntuser-window-l1-1-0.dll", L"GetSystemMetrics", ImportDisposition::NeedsBridge },
            { L"USER32 virtual system brush", L"user32.dll", L"GetSysColorBrush", ImportDisposition::NeedsBridge },
            { L"USER32 client-only frame adjustment", L"user32.dll", L"AdjustWindowRectEx", ImportDisposition::NeedsBridge },
            { L"USER32 dialog control lookup", L"user32.dll", L"GetDlgItem", ImportDisposition::NeedsBridge },
            { L"USER32 dialog control text through API set", L"api-ms-win-ntuser-window-l1-1-0.dll", L"SetDlgItemTextW", ImportDisposition::NeedsBridge },
            { L"USER32 message dispatch through API set", L"api-ms-win-ntuser-message-l1-1-0.dll", L"DispatchMessageW", ImportDisposition::NeedsBridge },
            { L"USER32 paint cycle", L"ext-ms-win-ntuser-draw-l1-1-0.dll", L"BeginPaint", ImportDisposition::NeedsBridge },
            { L"USER32 SetTimer", L"user32.dll", L"SetTimer", ImportDisposition::NeedsBridge },
            { L"USER32 KillTimer through API set", L"api-ms-win-ntuser-window-l1-1-0.dll", L"KillTimer", ImportDisposition::NeedsBridge },
            { L"MiniGDI primitive", L"GDI32.dll", L"Ellipse", ImportDisposition::NeedsBridge },
            { L"MiniGDI device capability", L"gdi32.dll", L"GetDeviceCaps", ImportDisposition::NeedsBridge },
            { L"MiniGDI CreateFontW", L"gdi32full.dll", L"CreateFontW", ImportDisposition::NeedsBridge },
            { L"MiniGDI CreateFontIndirectW through API set", L"api-ms-win-gdi-draw-l1-1-0.dll", L"CreateFontIndirectW", ImportDisposition::NeedsBridge },
            { L"kernel module handle", L"KERNELBASE.dll", L"GetModuleHandleW", ImportDisposition::NeedsBridge },
            { L"kernel tick count through API set", L"api-ms-win-core-sysinfo-l1-2-0.dll", L"GetTickCount", ImportDisposition::NeedsBridge },
            { L"kernel performance counter", L"kernel32.dll", L"QueryPerformanceCounter", ImportDisposition::NeedsBridge },
        };

        bool passed = true;
        for (const auto& fixture : fixtures)
        {
            ImportedSymbol symbol{};
            symbol.library = fixture.library;
            symbol.name = fixture.name;

            const auto resolution = CompatibilityCatalog::Resolve(symbol);
            if (resolution.disposition != fixture.expectedDisposition)
            {
                std::wcerr << L"Resolver fixture failed: " << fixture.description
                    << L" (" << fixture.library << L"!" << fixture.name << L")\n";
                passed = false;
            }
        }

        ImportedSymbol ordinal{};
        ordinal.library = L"user32.dll";
        ordinal.ordinal = 1;
        ordinal.importedByOrdinal = true;
        if (CompatibilityCatalog::Resolve(ordinal).disposition != ImportDisposition::Unsupported)
        {
            std::wcerr << L"Resolver fixture failed: ordinal imports must remain explicit.\n";
            passed = false;
        }

        return passed;
    }
}

int wmain(int argc, wchar_t** argv)
{
    if (argc != 2)
    {
        std::wcerr << L"Usage: BridgeTests <guest-pe-path>\n";
        return 2;
    }

    if (!ValidateResolverFixtures())
    {
        return 1;
    }

    std::ifstream stream(argv[1], std::ios::binary);
    const std::vector<BYTE> bytes(
        (std::istreambuf_iterator<char>(stream)),
        std::istreambuf_iterator<char>());

    Win32Bridge::Bridge::PeImageInfo image;
    if (!Win32Bridge::Bridge::PeImage::Inspect(bytes.data(), bytes.size(), &image))
    {
        std::wcerr << L"PE inspection failed: " << image.error << L"\n";
        return 1;
    }

    const auto hasImport = [&image](const wchar_t* library, const wchar_t* name)
    {
        for (const auto& symbol : image.imports)
        {
            if (symbol.library == library && symbol.name == name)
            {
                return true;
            }
        }
        return false;
    };

    const bool hasStorageRoundTrip =
        hasImport(L"KERNEL32.dll", L"CreateFileW") &&
        hasImport(L"KERNEL32.dll", L"ReadFile") &&
        hasImport(L"KERNEL32.dll", L"WriteFile") &&
        hasImport(L"KERNEL32.dll", L"CloseHandle") &&
        hasImport(L"KERNEL32.dll", L"CreateDirectoryW");

    const bool hasWindowAndMessageLoop =
        hasImport(L"USER32.dll", L"RegisterClassExW") &&
        hasImport(L"USER32.dll", L"CreateWindowExW") &&
        hasImport(L"USER32.dll", L"ShowWindow") &&
        hasImport(L"USER32.dll", L"UpdateWindow") &&
        hasImport(L"USER32.dll", L"GetMessageW") &&
        hasImport(L"USER32.dll", L"TranslateMessage") &&
        hasImport(L"USER32.dll", L"DispatchMessageW") &&
        hasImport(L"USER32.dll", L"PostQuitMessage") &&
        hasImport(L"USER32.dll", L"DestroyWindow") &&
        hasImport(L"USER32.dll", L"DefWindowProcW");

    const bool hasPaintCycle =
        hasImport(L"USER32.dll", L"BeginPaint") &&
        hasImport(L"USER32.dll", L"EndPaint") &&
        hasImport(L"USER32.dll", L"GetClientRect") &&
        hasImport(L"USER32.dll", L"FillRect");

    const bool hasMiniGdiDrawing =
        hasImport(L"GDI32.dll", L"CreateSolidBrush") &&
        hasImport(L"GDI32.dll", L"CreatePen") &&
        hasImport(L"GDI32.dll", L"SelectObject") &&
        hasImport(L"GDI32.dll", L"DeleteObject") &&
        hasImport(L"GDI32.dll", L"Rectangle") &&
        hasImport(L"GDI32.dll", L"MoveToEx") &&
        hasImport(L"GDI32.dll", L"LineTo") &&
        hasImport(L"GDI32.dll", L"SetBkMode") &&
        hasImport(L"GDI32.dll", L"SetTextColor") &&
        hasImport(L"GDI32.dll", L"TextOutW");

    if (image.machine != IMAGE_FILE_MACHINE_AMD64)
    {
        std::wcerr << L"Guest PE must be AMD64.\n";
        return 1;
    }
    if (!hasStorageRoundTrip)
    {
        std::wcerr << L"Guest PE is missing the virtual-storage round-trip imports.\n";
        return 1;
    }
    if (!hasWindowAndMessageLoop)
    {
        std::wcerr << L"Guest PE is missing the USER32 window/message-loop imports.\n";
        return 1;
    }
    if (!hasPaintCycle)
    {
        std::wcerr << L"Guest PE is missing the USER32 paint-cycle imports.\n";
        return 1;
    }
    if (!hasMiniGdiDrawing)
    {
        std::wcerr << L"Guest PE is missing the MiniGDI drawing imports.\n";
        return 1;
    }

    Win32Bridge::Bridge::MappedPeImage mappedImage;
    std::wstring mappingError;
    if (!Win32Bridge::Bridge::PeMapper::Materialize(bytes.data(), bytes.size(), &mappedImage, &mappingError) ||
        !Win32Bridge::Bridge::PeMapper::ApplyBaseRelocations(&mappedImage, mappedImage.preferredImageBase, &mappingError))
    {
        std::wcerr << L"PE mapping failed: " << mappingError << L"\n";
        return 1;
    }

    Win32Bridge::Bridge::BindingReport binding;
    std::wstring bindingError;
    const auto resolver = [](const Win32Bridge::Bridge::ImportedSymbol& symbol)
    {
        auto resolution = Win32Bridge::Bridge::CompatibilityCatalog::Resolve(symbol);
        if (resolution.disposition == Win32Bridge::Bridge::ImportDisposition::NeedsBridge)
        {
            // The native test intentionally uses synthetic addresses; the UWP
            // host supplies the actual ABI adapters at runtime.
            resolution.targetAddress = 0x1234567800000000ULL + symbol.iatRva;
        }
        return resolution;
    };
    if (!Win32Bridge::Bridge::ImportBinder::Bind(&mappedImage, image, resolver, &binding, &bindingError) ||
        binding.bound != image.imports.size() || binding.unresolved != 0)
    {
        std::wcerr << L"IAT binding failed: " << bindingError << L"\n";
        return 1;
    }

    bool everyImportWasBound = true;
    for (const auto& symbol : image.imports)
    {
        ULONGLONG address = 0;
        memcpy(&address, mappedImage.bytes.data() + symbol.iatRva, sizeof(address));
        everyImportWasBound = everyImportWasBound && address != 0;
    }
    if (!everyImportWasBound)
    {
        std::wcerr << L"At least one test IAT target was not written.\n";
        return 1;
    }

    std::wcout << L"Validated, materialized, and fully bound x64 PE with " << image.imports.size() << L" imports.\n";
    return 0;
}
