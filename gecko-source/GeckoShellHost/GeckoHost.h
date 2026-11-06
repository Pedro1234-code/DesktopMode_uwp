#pragma once

#include "GeckoHost.g.h"

#include <memory>

#include "client/EngineView.h"

namespace winrt::DesktopMode::Gecko::implementation
{
    struct GeckoHost : GeckoHostT<GeckoHost>
    {
        GeckoHost();

        Windows::Foundation::IInspectable Content() const;
        Windows::Foundation::IInspectable ExternalContent() const;
        bool IsStarted() const noexcept;
        bool IsExternalWindowOpen() const noexcept;

        void Start(double width, double height);
        void SetViewport(double width, double height);
        void SetExternalViewport(double width, double height);
        void SetExternalActive(bool active);
        void CloseExternalWindow();
        void SetScreen(double width, double height);
        void SetActive(bool active);
        int32_t TakeWindowCommand() noexcept;
        winrt::hstring TakeRevealPath();
        winrt::hstring TakeWallpaperPath();
        void OpenUrl(hstring const& url);
        winrt::DesktopMode::Gecko::GeckoWindow CreateBrowserWindow(hstring const& url, bool appMode);
        bool DragFiles(int32_t state, double x, double y, hstring const& paths);
        void Suspend();
        void Resume();
        void Close();

    private:
        void BuildVisualTree();
        void PushDpi();

        Windows::UI::Xaml::Controls::Grid root_{ nullptr };
        Windows::UI::Xaml::Controls::Grid splash_{ nullptr };
        std::unique_ptr<gecko_w10m::client::EngineView> engineView_;
        bool started_ = false;
        bool active_ = false;
        bool closed_ = false;
        double rawPerView_ = 1.0;
    };
}

namespace winrt::DesktopMode::Gecko::factory_implementation
{
    struct GeckoHost : GeckoHostT<GeckoHost, implementation::GeckoHost>
    {
    };
}
