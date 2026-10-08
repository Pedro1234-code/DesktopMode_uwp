#pragma once

#include "GeckoHost.g.h"

#include <memory>

#include "client/EngineView.h"

namespace winrt::DesktopMode::Gecko::implementation
{
    struct GeckoHost : GeckoHostT<GeckoHost>
    {
        GeckoHost();

        Windows::UI::Xaml::UIElement Content() const;
        bool IsStarted() const noexcept;

        void Start(double width, double height);
        void SetViewport(double width, double height);
        void SetActive(bool active);
        void OpenUrl(hstring const& url);
        void Suspend();
        void Resume();

    private:
        void BuildVisualTree();
        void PushDpi();

        Windows::UI::Xaml::Controls::Grid root_{ nullptr };
        Windows::UI::Xaml::Controls::Grid splash_{ nullptr };
        std::unique_ptr<gecko_w10m::client::EngineView> engineView_;
        bool started_ = false;
        bool active_ = false;
        double rawPerView_ = 1.0;
    };
}

namespace winrt::DesktopMode::Gecko::factory_implementation
{
    struct GeckoHost : GeckoHostT<GeckoHost, implementation::GeckoHost>
    {
    };
}
