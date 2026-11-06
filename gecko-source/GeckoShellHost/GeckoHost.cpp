#include "pch.h"
#include "GeckoHost.h"
#include "GeckoHost.g.cpp"
#include "GeckoWindow.h"

#include <atomic>
#include <deque>
#include <mutex>
#include <thread>

#include <winrt/Windows.Foundation.Metadata.h>
#include <winrt/Windows.Graphics.Display.h>
#include <winrt/Windows.UI.Xaml.Media.Imaging.h>

#include "client/DownloadBroker.h"
#include "client/DrmBridge.h"
#include "client/Log.h"
#include "engine/GeckoRuntimeHost.h"

using namespace winrt;
using namespace Windows::Foundation;
using namespace Windows::Foundation::Metadata;
using namespace Windows::Graphics::Display;
using namespace Windows::Storage;
using namespace Windows::UI;
using namespace Windows::UI::Xaml;
using namespace Windows::UI::Xaml::Controls;
using namespace Windows::UI::Xaml::Media;
using namespace Windows::UI::Xaml::Media::Imaging;

namespace
{
    // Gecko/XRE and the exported headless widget bridge are process-global.
    // The DesktopMode integration intentionally owns one runtime and lets
    // Firefox provide its own tabs inside that one internal window.
    std::atomic<bool> g_runtimeStarted{ false };
    std::mutex g_windowCommandMutex;
    std::deque<int32_t> g_windowCommands;
    std::mutex g_revealMutex;
    std::wstring g_revealPath;
    std::mutex g_wallpaperMutex;
    std::wstring g_wallpaperPath;
    std::once_flag g_memoryHooks;
    std::atomic<unsigned long long> g_lastMemoryPressure{ 0 };

    void QueueWindowCommand(int32_t command) noexcept
    {
        try
        {
            std::lock_guard<std::mutex> lock(g_windowCommandMutex);
            g_windowCommands.push_back(command);
        }
        catch (...)
        {
        }
    }

    SolidColorBrush MakeSolidColorBrush(Color color)
    {
        SolidColorBrush brush;
        brush.Color(color);
        return brush;
    }

    void NotifyGeckoAppState(int32_t state)
    {
        if (HMODULE xul = ::GetModuleHandleW(L"xul.dll"))
        {
            using AppStateFn = void(__cdecl*)(int32_t);
            if (auto fn = reinterpret_cast<AppStateFn>(
                    ::GetProcAddress(xul, "gecko_w10m_app_state")))
            {
                fn(state);
            }
        }
    }

    std::wstring Megabytes(uint64_t bytes)
    {
        return std::to_wstring(bytes / (1024 * 1024)) + L" MB";
    }

    void InitializeMemoryPressure()
    {
        std::call_once(g_memoryHooks, []
        {
            try
            {
                using Windows::System::MemoryManager;
                gecko_w10m::client::Log::Write(
                    L"mem: Shell limit " + Megabytes(MemoryManager::AppMemoryUsageLimit()) +
                    L", using " + Megabytes(MemoryManager::AppMemoryUsage()));

                MemoryManager::AppMemoryUsageLimitChanging([](auto const&, auto const& args)
                {
                    gecko_w10m::client::Log::Write(
                        L"mem: Shell limit changing from " + Megabytes(args.OldLimit()) +
                        L" to " + Megabytes(args.NewLimit()));
                });
                MemoryManager::AppMemoryUsageIncreased([](auto const&, auto const&)
                {
                    const auto level = MemoryManager::AppMemoryUsageLevel();
                    const int32_t numericLevel = static_cast<int32_t>(level);
                    gecko_w10m::client::Log::Write(
                        L"mem: Shell usage increased to level " +
                        std::to_wstring(numericLevel) + L", " +
                        Megabytes(MemoryManager::AppMemoryUsage()));

                    if (numericLevel < 1)
                    {
                        return;
                    }

                    const unsigned long long now = ::GetTickCount64();
                    const unsigned long long gap = numericLevel >= 3 ? 0ull :
                                                    numericLevel >= 2 ? 5000ull : 30000ull;
                    const unsigned long long last = g_lastMemoryPressure.load();
                    if (last && now - last < gap)
                    {
                        return;
                    }
                    g_lastMemoryPressure.store(now);

                    if (HMODULE xul = ::GetModuleHandleW(L"xul.dll"))
                    {
                        using PressureFn = void(__cdecl*)(int32_t);
                        if (auto fn = reinterpret_cast<PressureFn>(
                                ::GetProcAddress(xul, "gecko_w10m_memory_pressure")))
                        {
                            fn(numericLevel);
                        }
                    }
                });
                MemoryManager::AppMemoryUsageDecreased([](auto const&, auto const&)
                {
                    gecko_w10m::client::Log::Write(
                        L"mem: Shell usage decreased to level " +
                        std::to_wstring(static_cast<int32_t>(
                            MemoryManager::AppMemoryUsageLevel())));
                });
            }
            catch (...)
            {
                gecko_w10m::client::Log::Write(
                    L"mem: MemoryManager is unavailable");
            }
        });
    }
}

namespace winrt::DesktopMode::Gecko::implementation
{
    GeckoHost::GeckoHost()
    {
        gecko_w10m::client::DrmBridge::SetWindowCommandSink([](int32_t command)
        {
            QueueWindowCommand(command);
        });
        gecko_w10m::client::DrmBridge::SetRevealFileSink([](const wchar_t* path)
        {
            std::lock_guard<std::mutex> lock(g_revealMutex);
            g_revealPath = path ? path : L"";
            QueueWindowCommand(9);
        });
        gecko_w10m::client::DrmBridge::SetWallpaperSink([](const wchar_t* path)
        {
            std::lock_guard<std::mutex> lock(g_wallpaperMutex);
            g_wallpaperPath = path ? path : L"";
            QueueWindowCommand(10);
        });
        root_ = Grid();
        root_.Background(MakeSolidColorBrush(ColorHelper::FromArgb(255, 28, 27, 34)));
    }

    Windows::Foundation::IInspectable GeckoHost::Content() const
    {
        return root_.as<Windows::Foundation::IInspectable>();
    }

    Windows::Foundation::IInspectable GeckoHost::ExternalContent() const
    {
        if (!engineView_ || !engineView_->ExternalSurface())
        {
            return nullptr;
        }
        return engineView_->ExternalSurface().as<Windows::Foundation::IInspectable>();
    }

    bool GeckoHost::IsStarted() const noexcept
    {
        return started_;
    }

    bool GeckoHost::IsExternalWindowOpen() const noexcept
    {
        try
        {
            return engineView_ && engineView_->HasExternalWindow();
        }
        catch (...)
        {
            return false;
        }
    }

    void GeckoHost::Start(double width, double height)
    {
        if (started_)
        {
            SetViewport(width, height);
            SetActive(true);
            return;
        }

        if (width <= 0 || height <= 0)
        {
            return;
        }

        bool expected = false;
        if (!g_runtimeStarted.compare_exchange_strong(expected, true))
        {
            throw hresult_illegal_method_call(
                L"Only one embedded Firefox runtime can exist in this process.");
        }

        try
        {
            std::wstring localState(
                ApplicationData::Current().LocalFolder().Path());
            localState += L"\\Firefox";
            ::CreateDirectoryW(localState.c_str(), nullptr);

            gecko_w10m::client::Log::Init(localState);
            gecko_w10m::client::Log::Write(
                L"boot: embedded DesktopMode Firefox host starting");
            InitializeMemoryPressure();
            const std::wstring downloadDirectory =
                gecko_w10m::client::DownloadBroker::Initialize(localState);
            std::thread([] { gecko_w10m::client::Log::Mirror(); }).detach();

            double displayPixelsPerViewPixel = 1.0;
            if (ApiInformation::IsPropertyPresent(
                    L"Windows.Graphics.Display.DisplayInformation",
                    L"RawPixelsPerViewPixel"))
            {
                displayPixelsPerViewPixel = DisplayInformation::GetForCurrentView()
                                                .RawPixelsPerViewPixel();
            }
            if (!(displayPixelsPerViewPixel > 0))
            {
                displayPixelsPerViewPixel = 1.0;
            }

            // Gecko's framebuffer and CSS device scale stay in the display's
            // physical coordinate system. DesktopMode's 70% transform belongs
            // to the outer XAML desktop and is undone by the swap-chain matrix;
            // folding it into Gecko changes the browser's native resolution.
            rawPerView_ = displayPixelsPerViewPixel;
            gecko_w10m::client::Log::Write(
                L"view: embedded framebuffer scale " +
                std::to_wstring(rawPerView_));

            const int pixelWidth = static_cast<int>(width * rawPerView_ + 0.5);
            const int pixelHeight = static_cast<int>(height * rawPerView_ + 0.5);

            double cssScale = rawPerView_;
            constexpr double kNarrowestChromeCss = 540.0;
            if (pixelWidth > 0 && cssScale > pixelWidth / kNarrowestChromeCss)
            {
                cssScale = pixelWidth / kNarrowestChromeCss;
            }
            engineView_ = std::make_unique<gecko_w10m::client::EngineView>(
                pixelWidth, pixelHeight, rawPerView_, true, false);
            engineView_->SetScreen(width, height);
            BuildVisualTree();
            PushDpi();
            engineView_->OnFirstFrame([this]
            {
                if (splash_)
                {
                    splash_.Visibility(Visibility::Collapsed);
                }
                auto panel = engineView_->Panel();
                gecko_w10m::client::Log::Write(
                    L"view: first frame XAML panel " +
                    std::to_wstring(panel.ActualWidth()) + L"x" +
                    std::to_wstring(panel.ActualHeight()) +
                    L", visible=" +
                    std::to_wstring(panel.Visibility() == Visibility::Visible ? 1 : 0) +
                    L", opacity=" + std::to_wstring(panel.Opacity()) +
                    L", overlay image=" +
                    std::to_wstring(engineView_->Surface().Source() ? 1 : 0) +
                    L", splash=" +
                    std::to_wstring(splash_.Visibility() == Visibility::Visible ? 1 : 0));
            });
            engineView_->Start();
            engineView_->SetInputEnabled(true);
            engineView_->EnableMouse();

            started_ = gecko_w10m::engine::StartGeckoRuntime(
                localState, downloadDirectory, pixelWidth, pixelHeight,
                cssScale);
            if (!started_)
            {
                engineView_->Stop();
                engineView_->SetInputEnabled(false);
                g_runtimeStarted.store(false);
                throw hresult_error(E_FAIL, L"The Gecko runtime could not be started.");
            }

            active_ = true;
        }
        catch (...)
        {
            if (!started_)
            {
                g_runtimeStarted.store(false);
            }
            throw;
        }
    }

    void GeckoHost::BuildVisualTree()
    {
        root_.Children().Clear();

        // The sink is retained only as the transport for physical keyboard
        // text. DesktopMode does not use the old phone InputPane workflow.
        root_.Children().Append(engineView_->TextSink());
        if (engineView_->VideoPanel())
        {
            root_.Children().Append(engineView_->VideoPanel());
        }
        if (engineView_->Panel())
        {
            root_.Children().Append(engineView_->Panel());
        }
        root_.Children().Append(engineView_->Surface());

        splash_ = Grid();
        splash_.Background(MakeSolidColorBrush(ColorHelper::FromArgb(255, 28, 27, 34)));
        Image logo;
        logo.Width(96);
        logo.Height(96);
        logo.HorizontalAlignment(HorizontalAlignment::Center);
        logo.VerticalAlignment(VerticalAlignment::Center);
        logo.Source(BitmapImage(Uri(L"ms-appx:///Assets/Firefox/firefox.png")));
        splash_.Children().Append(logo);
        root_.Children().Append(splash_);

        // BrowserSurface was arranged before Start() created these children.
        // Arrange the newly-added SwapChainPanel now; EngineView deliberately
        // refuses to attach a native swap chain while the panel is still 0x0.
        root_.UpdateLayout();
        engineView_->WatchRoom(root_);
    }

    void GeckoHost::PushDpi()
    {
        if (!engineView_)
        {
            return;
        }

        try
        {
            auto display = DisplayInformation::GetForCurrentView();
            double dpi = display.RawDpiX();
            if (!(dpi > 0))
            {
                dpi = display.LogicalDpi();
            }
            engineView_->SetDpi(dpi);
        }
        catch (...)
        {
        }
    }

    void GeckoHost::SetViewport(double width, double height)
    {
        if (!engineView_ || width <= 0 || height <= 0)
        {
            return;
        }
        engineView_->SetViewport(width, height);
    }

    void GeckoHost::SetExternalViewport(double width, double height)
    {
        if (engineView_ && width > 0 && height > 0)
        {
            engineView_->SetExternalViewport(width, height);
        }
    }

    void GeckoHost::SetExternalActive(bool active)
    {
        if (engineView_)
        {
            engineView_->SetExternalActive(active);
        }
    }

    void GeckoHost::CloseExternalWindow()
    {
        if (engineView_)
        {
            engineView_->CloseExternalWindow();
        }
    }

    void GeckoHost::SetScreen(double width, double height)
    {
        if (width <= 0 || height <= 0)
        {
            return;
        }
        if (!engineView_)
        {
            return;
        }
        engineView_->SetScreen(width, height);
    }

    void GeckoHost::SetActive(bool active)
    {
        if (active && closed_)
        {
            NotifyGeckoAppState(1);
            closed_ = false;
        }
        active_ = active;
        if (!engineView_)
        {
            return;
        }

        engineView_->SetInputEnabled(active);
        if (active)
        {
            engineView_->Start();
            engineView_->EnableMouse();
        }
        else
        {
            engineView_->Stop();
        }
    }

    int32_t GeckoHost::TakeWindowCommand() noexcept
    {
        int32_t command = 0;
        {
            std::lock_guard<std::mutex> lock(g_windowCommandMutex);
            if (g_windowCommands.empty())
            {
                return 0;
            }
            command = g_windowCommands.front();
            g_windowCommands.pop_front();
        }
        try
        {
            gecko_w10m::client::Log::Write(
                L"window: shell dequeued command " + std::to_wstring(command));
        }
        catch (...)
        {
        }
        return command;
    }

    hstring GeckoHost::TakeRevealPath()
    {
        std::lock_guard<std::mutex> lock(g_revealMutex);
        hstring path(g_revealPath);
        g_revealPath.clear();
        return path;
    }

    hstring GeckoHost::TakeWallpaperPath()
    {
        std::lock_guard<std::mutex> lock(g_wallpaperMutex);
        hstring path(g_wallpaperPath);
        g_wallpaperPath.clear();
        return path;
    }

    void GeckoHost::OpenUrl(hstring const& url)
    {
        if (engineView_)
        {
            engineView_->OpenUrl(std::wstring_view(url.c_str(), url.size()));
        }
    }

    bool GeckoHost::DragFiles(int32_t state, double x, double y, hstring const& paths)
    {
        return started_ && !closed_ && engineView_ &&
            engineView_->DragFiles(state, x, y, paths.c_str());
    }

    winrt::DesktopMode::Gecko::GeckoWindow GeckoHost::CreateBrowserWindow(hstring const& url, bool appMode)
    {
        if (!started_ || !engineView_)
        {
            throw hresult_error(E_PENDING, L"Start the shared Gecko runtime before opening a window.");
        }
        Uri target(url);
        if (target.SchemeName() != L"https" && target.SchemeName() != L"http" &&
            url != L"about:home" && url != L"about:blank")
        {
            throw hresult_invalid_argument(L"Unsupported browser-window URL scheme.");
        }
        if (!active_)
        {
            // Let the primary view finish publishing the shared renderer's
            // panel/exports during background startup without accepting input.
            engineView_->SetInputEnabled(false);
            engineView_->Start();
        }
        return winrt::make<implementation::GeckoWindow>(url, appMode);
    }

    void GeckoHost::Suspend()
    {
        if (engineView_)
        {
            engineView_->Stop();
            engineView_->SetInputEnabled(false);
        }
        NotifyGeckoAppState(0);
        std::thread([] { gecko_w10m::client::Log::Mirror(); }).detach();
    }

    void GeckoHost::Resume()
    {
        NotifyGeckoAppState(1);
        if (active_ && engineView_)
        {
            engineView_->SetInputEnabled(true);
            engineView_->Start();
        }
    }

    void GeckoHost::Close()
    {
        active_ = false;
        closed_ = true;
        if (engineView_)
        {
            engineView_->SetInputEnabled(false);
            engineView_->CloseSession();
            engineView_->Stop();
        }
        // The Gecko worker may still be unwinding callbacks when the Shell
        // window is hidden. Keep the view alive until the host itself is
        // destroyed; releasing it synchronously here races that thread.
        bool otherWindows = false;
        if (auto xul = ::GetModuleHandleW(L"xul.dll"))
        {
            using CountFn = uint32_t (*)();
            if (auto count = reinterpret_cast<CountFn>(::GetProcAddress(xul, "gecko_w10m_managed_window_count")))
                otherWindows = count() != 0;
        }
        if (!otherWindows) NotifyGeckoAppState(0);
    }
}
