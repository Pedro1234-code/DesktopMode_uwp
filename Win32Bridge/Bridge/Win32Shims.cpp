#include "pch.h"
#include "Bridge\\Win32Shims.h"
#include "Bridge\\Gdi32Shims.h"
#include "Bridge/Advapi32Shims.h"
#include "Bridge/CommonControlsShims.h"
#include "Bridge/Comdlg32Shims.h"
#include "Bridge/OleShims.h"
#include "Bridge/MsvcrtShims.h"
#include "Bridge/Shell32Shims.h"
#include "Bridge\\Kernel32Shims.h"
#include "Bridge\\User32Shims.h"

using namespace Win32Bridge::Bridge;

using namespace Platform;
using namespace Windows::ApplicationModel::Core;
using namespace Windows::UI::Core;
using namespace Windows::UI::Xaml::Controls;
using namespace concurrency;

namespace
{
    bool IsUserLibrary(const std::wstring& library)
    {
        return _wcsicmp(library.c_str(), L"user32.dll") == 0 ||
            _wcsnicmp(library.c_str(), L"api-ms-win-ntuser-", 18) == 0 ||
            _wcsnicmp(library.c_str(), L"ext-ms-win-ntuser-", 18) == 0;
    }

    void ShowMessageDialog(String^ text, String^ caption, task_completion_event<int> completion)
    {
        auto dialog = ref new ContentDialog();
        dialog->Title = caption;
        dialog->Content = text;
        dialog->PrimaryButtonText = L"OK";

        create_task(dialog->ShowAsync()).then([completion](ContentDialogResult result)
        {
            completion.set(result == ContentDialogResult::Primary ? IDOK : IDCANCEL);
        });
    }
}

namespace Win32Bridge
{
namespace Bridge
{
int WINAPI BridgeMessageBoxW(HWND, LPCWSTR text, LPCWSTR caption, UINT)
{
    try
    {
        auto dispatcher = CoreApplication::MainView->CoreWindow->Dispatcher;
        // Waiting synchronously from the UI thread would deadlock ContentDialog.
        // The guest runtime will always run PE entry points on a worker thread.
        if (!dispatcher || dispatcher->HasThreadAccess)
        {
            return IDCANCEL;
        }

        String^ dialogText = ref new String(text ? text : L"");
        String^ dialogCaption = ref new String(caption ? caption : L"Win32 application");
        task_completion_event<int> completion;
        auto handler = ref new DispatchedHandler([dialogText, dialogCaption, completion]() mutable
        {
            ShowMessageDialog(dialogText, dialogCaption, completion);
        });

        create_task(dispatcher->RunAsync(CoreDispatcherPriority::Normal, handler)).then([completion](task<void> dispatch)
        {
            try
            {
                dispatch.get();
            }
            catch (...)
            {
                completion.set(IDCANCEL);
            }
        });

        return create_task(completion).get();
    }
    catch (...)
    {
        return IDCANCEL;
    }
}

BOOL WINAPI BridgeGetCursorPos(LPPOINT point)
{
    if (!point || !MouseInput().IsMouseDetected())
    {
        return FALSE;
    }

    const auto snapshot = MouseInput().Snapshot();
    point->x = static_cast<LONG>(snapshot.x);
    point->y = static_cast<LONG>(snapshot.y);
    return TRUE;
}

SHORT WINAPI BridgeGetAsyncKeyState(int virtualKey)
{
    return MouseInput().GetAsyncKeyState(virtualKey);
}

SHORT WINAPI BridgeGetKeyState(int virtualKey)
{
    return MouseInput().GetKeyState(virtualKey);
}

ULONGLONG MessageBoxWAdapterAddress()
{
    return reinterpret_cast<ULONGLONG>(&BridgeMessageBoxW);
}

ImportResolution ResolveRuntimeImport(const ImportedSymbol& symbol)
{
    auto resolution = CompatibilityCatalog::Resolve(symbol);
    if (_wcsicmp(symbol.library.c_str(), L"kernel32.dll") == 0 ||
        _wcsicmp(symbol.library.c_str(), L"kernelbase.dll") == 0 ||
        _wcsnicmp(symbol.library.c_str(), L"api-ms-win-core-", 16) == 0)
    {
        return ResolveKernel32Import(symbol);
    }
    const auto registryResolution = ResolveAdvapi32Import(symbol);
    if (registryResolution.targetAddress != 0) return registryResolution;
    const auto commonControlsResolution = ResolveCommonControlsImport(symbol);
    if (commonControlsResolution.targetAddress != 0) return commonControlsResolution;
    const auto commonDialogResolution = ResolveComdlg32Import(symbol);
    if (commonDialogResolution.targetAddress != 0) return commonDialogResolution;
    const auto oleResolution = ResolveOleImport(symbol);
    if (oleResolution.targetAddress != 0) return oleResolution;
    const auto shellResolution = ResolveShell32Import(symbol);
    if (shellResolution.targetAddress != 0) return shellResolution;
    const auto msvcrtResolution = ResolveMsvcrtImport(symbol);
    if (msvcrtResolution.targetAddress != 0) return msvcrtResolution;

    const auto gdiResolution = ResolveGdi32Import(symbol);
    if (gdiResolution.targetAddress != 0)
    {
        return gdiResolution;
    }

    const auto userResolution = ResolveUser32Import(symbol);
    if (userResolution.targetAddress != 0)
    {
        return userResolution;
    }

    if (!IsUserLibrary(symbol.library))
    {
        return resolution;
    }

    if (_wcsicmp(symbol.name.c_str(), L"messageboxw") == 0)
    {
        resolution.targetAddress = MessageBoxWAdapterAddress();
    }
    else if (_wcsicmp(symbol.name.c_str(), L"getcursorpos") == 0)
    {
        resolution.targetAddress = reinterpret_cast<ULONGLONG>(&BridgeGetCursorPos);
    }
    else if (_wcsicmp(symbol.name.c_str(), L"getasynckeystate") == 0)
    {
        resolution.targetAddress = reinterpret_cast<ULONGLONG>(&BridgeGetAsyncKeyState);
    }
    else if (_wcsicmp(symbol.name.c_str(), L"getkeystate") == 0)
    {
        resolution.targetAddress = reinterpret_cast<ULONGLONG>(&BridgeGetKeyState);
    }
    return resolution;
}
}
}
