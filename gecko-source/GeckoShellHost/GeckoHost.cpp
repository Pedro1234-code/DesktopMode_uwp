#include "pch.h"
#include "GeckoHost.h"
#include "GeckoHost.g.cpp"

#include <atomic>
#include <mutex>
#include <thread>

#include <winrt/Windows.Foundation.Metadata.h>
#include <winrt/Windows.Graphics.Display.h>
#include <winrt/Windows.UI.Xaml.Media.Imaging.h>

#include "client/DownloadBroker.h"
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
    std::once_flag g_memoryHooks;
    std::atomic<unsigned long long> g_lastMemoryPressure{ 0 };

    SolidColorBrush Brush(Color color)
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
        root_ = Grid();
        root_.Background(Brush(ColorHelper::FromArgb(255, 28, 27, 34)));
    }

    UIElement GeckoHost::Content() const
    {
        return root_;
    }

    bool GeckoHost::IsStarted() const noexcept
    {
        return started_;
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
            gecko_w10m::client::DownloadBroker::Initialize(localState);
            std::thread([] { gecko_w10m::client::Log::Mirror(); }).detach();

            if (ApiInformation::IsPropertyPresent(
                    L"Windows.Graphics.Display.DisplayInformation",
                    L"RawPixelsPerViewPixel"))
            {
                rawPerView_ = DisplayInformation::GetForCurrentView()
                                  .RawPixelsPerViewPixel();
            }
            if (!(rawPerView_ > 0))
            {
                rawPerView_ = 1.0;
            }

            const int pixelWidth = static_cast<int>(width * rawPerView_ + 0.5);
            const int pixelHeight = static_cast<int>(height * rawPerView_ + 0.5);

            double cssScale = rawPerView_;
            constexpr double kNarrowestChromeCss = 540.0;
            if (pixelWidth > 0 && cssScale > pixelWidth / kNarrowestChromeCss)
            {
                cssScale = pixelWidth / kNarrowestChromeCss;
            }
            // Unlike the standalone host, this surface already lives below
            // MainPage's 70% Xbox desktop transform. Applying that factor to
            // Gecko again would shrink the browser chrome twice.

            engineView_ = std::make_unique<gecko_w10m::client::EngineView>(
                pixelWidth, pixelHeight, rawPerView_, true, false);
            BuildVisualTree();
            engineView_->SetScreen(width, height);
            PushDpi();
            engineView_->OnFirstFrame([this]
            {
                if (splash_)
                {
                    splash_.Visibility(Visibility::Collapsed);
                }
            });
            engineView_->Start();
            engineView_->SetInputEnabled(true);
            engineView_->EnableMouse();

            started_ = gecko_w10m::engine::StartGeckoRuntime(
                localState, pixelWidth, pixelHeight, cssScale);
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
        splash_.Background(Brush(ColorHelper::FromArgb(255, 28, 27, 34)));
        Image logo;
        logo.Width(96);
        logo.Height(96);
        logo.HorizontalAlignment(HorizontalAlignment::Center);
        logo.VerticalAlignment(VerticalAlignment::Center);
        logo.Source(BitmapImage(Uri(L"ms-appx:///Assets/Firefox/firefox.png")));
        splash_.Children().Append(logo);
        root_.Children().Append(splash_);

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
        engineView_->SetScreen(width, height);
    }

    void GeckoHost::SetActive(bool active)
    {
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

    void GeckoHost::OpenUrl(hstring const& url)
    {
        if (engineView_)
        {
            engineView_->OpenUrl(std::wstring_view(url.c_str(), url.size()));
        }
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
}
