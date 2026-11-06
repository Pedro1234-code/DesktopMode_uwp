#pragma once

#include "GeckoWindow.g.h"
#include <winrt/Windows.UI.Xaml.Media.Imaging.h>
#include <memory>
#include <array>

namespace winrt::DesktopMode::Gecko::implementation {
struct GeckoWindow : GeckoWindowT<GeckoWindow> {
  GeckoWindow(hstring const& url, bool appMode);
  ~GeckoWindow();
  Windows::Foundation::IInspectable Content() const;
  uint64_t Id() const noexcept;
  uint32_t State() const;
  uint32_t Error() const;
  uint32_t TextInputState() const;
  bool UpdateFrame();
  void SetViewport(double width, double height);
  void SetActive(bool active);
  bool OpenUrl(hstring const& url);
  void SendMouse(int32_t kind, int32_t button, double x, double y);
  void SendWheel(double x, double y, double dx, double dy);
  void SendKey(bool down, int32_t key, uint32_t modifiers, bool repeat);
  void SendText(hstring const& text);
  int32_t TakeWindowCommand();
  bool ContextAction(hstring const& action);
  void RequestClose();
  void Close();

 private:
  struct Api;
  void CheckUIThread() const;
  std::shared_ptr<Api> api_;
  Windows::UI::Xaml::Controls::Grid root_{nullptr};
  Windows::UI::Xaml::Controls::SwapChainPanel panel_{nullptr};
  Windows::UI::Xaml::Controls::Image softwareSurface_{nullptr};
  Windows::UI::Xaml::Media::Imaging::WriteableBitmap softwareBitmap_{nullptr};
  uint64_t frameSerial_ = 0;
  int32_t bitmapWidth_ = 0, bitmapHeight_ = 0;
  bool softwareVisible_ = false;
  Windows::UI::Core::CoreDispatcher dispatcher_{nullptr};
  hstring url_;
  uint64_t id_ = 0;
  double scale_ = 1;
  int32_t capacityWidth_ = 0, capacityHeight_ = 0;
  int32_t lastWidth_ = 0, lastHeight_ = 0;
  float lastScaleX_ = 0, lastScaleY_ = 0;
  void* metricsKey_ = nullptr;
  bool metricsPublished_ = false;
  bool viewportConfirmed_ = false;
  bool appMode_ = false;
  bool active_ = false;
  bool closing_ = false;
  std::array<bool, 256> pressedKeys_{};
  std::array<bool, 3> pressedButtons_{};
  int32_t lastX_ = 0, lastY_ = 0;
};
}
