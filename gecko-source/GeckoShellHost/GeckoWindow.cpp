#include "pch.h"
#include "GeckoWindow.h"
#include "GeckoWindow.g.cpp"
#include "client/Log.h"
#include <winrt/Windows.Graphics.Display.h>
#include <winrt/Windows.Storage.Streams.h>
#include <robuffer.h>
#include <algorithm>
#include <cmath>
#include <limits>
#include <vector>
#include <mutex>
#include <cstdio>

namespace winrt::DesktopMode::Gecko::implementation {
namespace {
// This is diagnostic only: always continue the exception search. Gecko can
// intentionally handle access violations (for example Wasm guard faults), so
// an entry alone does not prove that the exception terminated the process.
struct FaultModule { const char* name; uintptr_t base; size_t size; };
FaultModule sFaultModules[10]{};
HANDLE sFaultFile = INVALID_HANDLE_VALUE;
volatile LONG sFaultCount = 0;
using CaptureFaultStack = USHORT (WINAPI*)(ULONG, ULONG, PVOID*, PULONG);
CaptureFaultStack sCaptureFaultStack = nullptr;
LONG WINAPI RecordWindowFault(EXCEPTION_POINTERS* exception) {
  if (!exception || !exception->ExceptionRecord ||
      exception->ExceptionRecord->ExceptionCode != EXCEPTION_ACCESS_VIOLATION)
    return EXCEPTION_CONTINUE_SEARCH;
  const LONG number = ::InterlockedIncrement(&sFaultCount);
  if (number > 16 || sFaultFile == INVALID_HANDLE_VALUE) return EXCEPTION_CONTINUE_SEARCH;
  const auto* record = exception->ExceptionRecord;
  const uintptr_t pc = reinterpret_cast<uintptr_t>(record->ExceptionAddress);
  const char* module = "unknown";
  uintptr_t offset = pc;
  for (const auto& item : sFaultModules) {
    if (item.base && pc >= item.base && pc - item.base < item.size) {
      module = item.name;
      offset = pc - item.base;
      break;
    }
  }
  const auto access = record->NumberParameters >= 1 ? record->ExceptionInformation[0] : ULONG_PTR(-1);
  const bool nullTarget = record->NumberParameters >= 2 && record->ExceptionInformation[1] == 0;
  char line[512]{};
  const int length = _snprintf_s(line, sizeof(line), _TRUNCATE,
      "first-chance-av tick=%llu thread=%lu number=%ld module=%s offset=0x%llx access=%llu null_target=%d\r\n",
      static_cast<unsigned long long>(::GetTickCount64()), ::GetCurrentThreadId(), number,
      module, static_cast<unsigned long long>(offset), static_cast<unsigned long long>(access), int(nullTarget));
  if (length > 0) {
    DWORD written = 0;
    ::WriteFile(sFaultFile, line, static_cast<DWORD>(length), &written, nullptr);
  }
  // Collect addresses only. No stack memory, registers or page/input content.
  // On x64 the exception dispatcher can unwind back through the faulting call.
  if (sCaptureFaultStack) {
    void* frames[32]{};
    const auto count = sCaptureFaultStack(0, 32, frames, nullptr);
    for (USHORT index = 0; index < count; ++index) {
      const auto address = reinterpret_cast<uintptr_t>(frames[index]);
      const char* name = "unknown";
      uintptr_t relative = address;
      for (const auto& item : sFaultModules) {
        if (item.base && address >= item.base && address - item.base < item.size) {
          name = item.name; relative = address - item.base; break;
        }
      }
      int size = _snprintf_s(line, sizeof(line), _TRUNCATE,
          "av-stack number=%ld thread=%lu frame=%u module=%s offset=0x%llx\r\n",
          number, ::GetCurrentThreadId(), unsigned(index), name,
          static_cast<unsigned long long>(relative));
      if (size > 0) {
        DWORD written = 0;
        ::WriteFile(sFaultFile, line, static_cast<DWORD>(size), &written, nullptr);
      }
    }
  }
  return EXCEPTION_CONTINUE_SEARCH;
}
void InitializeWindowFaultDiagnostics() {
  static std::once_flag once;
  std::call_once(once, [] {
    using Handler = LONG (WINAPI*)(EXCEPTION_POINTERS*);
    using AddHandler = PVOID (WINAPI*)(ULONG, Handler);
    // Optional lookup: do not introduce a new package activation dependency
    // on APIs that may be absent from a particular Xbox OS image.
    auto kernel = ::GetModuleHandleW(L"kernelbase.dll");
    auto add = kernel ? reinterpret_cast<AddHandler>(::GetProcAddress(kernel, "AddVectoredExceptionHandler")) : nullptr;
    if (!add) {
      kernel = ::GetModuleHandleW(L"kernel32.dll");
      add = kernel ? reinterpret_cast<AddHandler>(::GetProcAddress(kernel, "AddVectoredExceptionHandler")) : nullptr;
    }
    if (!add) { gecko_w10m::client::Log::Write(L"window fault diagnostics: handler API unavailable"); return; }
    if (auto ntdll = ::GetModuleHandleW(L"ntdll.dll"))
      sCaptureFaultStack = reinterpret_cast<CaptureFaultStack>(
          ::GetProcAddress(ntdll, "RtlCaptureStackBackTrace"));
    try {
      auto path = Windows::Storage::ApplicationData::Current().LocalFolder().Path();
      std::wstring file(path.c_str());
      file += L"\\WebApps-native-exceptions.log";
      CREATEFILE2_EXTENDED_PARAMETERS params{};
      params.dwSize = sizeof(params);
      params.dwFileAttributes = FILE_ATTRIBUTE_NORMAL;
      params.dwFileFlags = FILE_FLAG_WRITE_THROUGH;
      sFaultFile = ::CreateFile2(file.c_str(), FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE,
                                OPEN_ALWAYS, &params);
      if (sFaultFile == INVALID_HANDLE_VALUE) return;
      const wchar_t* names[] = {L"xul.dll", L"libGLESv2.dll", L"libEGL.dll", L"DesktopMode.Gecko.dll",
          nullptr, L"mrt100_app.dll", L"Windows.UI.Xaml.dll", L"d3d11.dll", L"ntdll.dll", L"kernelbase.dll"};
      const char* labels[] = {"xul.dll", "libGLESv2.dll", "libEGL.dll", "DesktopMode.Gecko.dll",
          "DesktopMode.exe", "mrt100_app.dll", "Windows.UI.Xaml.dll", "d3d11.dll", "ntdll.dll", "kernelbase.dll"};
      for (size_t i = 0; i < 10; ++i) {
        auto module = ::GetModuleHandleW(names[i]);
        if (!module) continue;
        auto base = reinterpret_cast<uintptr_t>(module);
        const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
        const auto* pe = reinterpret_cast<const IMAGE_NT_HEADERS*>(base + dos->e_lfanew);
        sFaultModules[i] = {labels[i], base, pe->OptionalHeader.SizeOfImage};
      }
      if (!add(0, RecordWindowFault)) {
        ::CloseHandle(sFaultFile); sFaultFile = INVALID_HANDLE_VALUE; return;
      }
      const char start[] = "\r\n=== Web App native diagnostics: first-chance AV; may be handled by Gecko; access 0=read 1=write 8=execute ===\r\n";
      DWORD written = 0;
      ::WriteFile(sFaultFile, start, sizeof(start) - 1, &written, nullptr);
      gecko_w10m::client::Log::Write(L"window fault diagnostics: WebApps-native-exceptions.log enabled (limit 16 per process)");
    } catch (...) {
      if (sFaultFile != INVALID_HANDLE_VALUE) { ::CloseHandle(sFaultFile); sFaultFile = INVALID_HANDLE_VALUE; }
    }
  });
}
template <typename T> T Resolve(HMODULE module, const char* name) {
  auto address = ::GetProcAddress(module, name);
  if (!address) throw hresult_error(E_NOINTERFACE,
      L"The packaged Gecko engine does not implement the window-session ABI.");
  return reinterpret_cast<T>(address);
}
// Keep the XAML panel alive on its UI thread until native teardown releases
// its EGL/COM references. This timer runs only while closed views are retiring.
struct RetiredWindow {
  uint64_t id;
  com_ptr<::IUnknown> panel;
  uint32_t (*status)(uint64_t, uint32_t*);
  void (*release)(uint64_t);
  void (*releaseMetrics)(void*);
  void* metricsKey;
};
struct RetirementQueue {
  Windows::UI::Xaml::DispatcherTimer timer;
  std::vector<RetiredWindow> entries;
  RetirementQueue() {
    timer.Interval(std::chrono::milliseconds(100));
    timer.Tick([this](auto const&, auto const&) {
      entries.erase(std::remove_if(entries.begin(), entries.end(), [](auto& item) {
        auto state = item.status(item.id, nullptr);
        if (state != 0 && state != 3 && state != 4) return false;
        item.release(item.id);
        item.releaseMetrics(item.metricsKey);
        return true;
      }), entries.end());
      if (entries.empty()) timer.Stop();
    });
  }
};
RetirementQueue& Retirements() {
  // Owned by the browser's XAML dispatcher; intentionally process lifetime,
  // rather than allowing static destruction after that dispatcher is gone.
  static auto* queue = new RetirementQueue();
  return *queue;
}
int32_t Pixels(double value, double scale) {
  if (!std::isfinite(value) || !std::isfinite(scale) || scale <= 0)
    throw hresult_invalid_argument();
  double pixels = std::round(value * scale);
  if (pixels < std::numeric_limits<int32_t>::min() ||
      pixels > std::numeric_limits<int32_t>::max()) throw hresult_invalid_argument();
  return static_cast<int32_t>(pixels);
}
}

struct GeckoWindow::Api {
  uint64_t (*create)(void*, const char*, int32_t, int32_t, int32_t);
  uint32_t (*status)(uint64_t, uint32_t*);
  void (*cancel)(uint64_t);
  void (*release)(uint64_t);
  void (*resize)(uint64_t, int32_t, int32_t);
  void (*activate)(uint64_t);
  void (*requestClose)(uint64_t);
  void (*appState)(int32_t);
  void (*mouse)(uint64_t, int32_t, int32_t, int32_t, int32_t);
  void (*wheel)(uint64_t, int32_t, int32_t, double, double);
  void (*key)(uint64_t, int32_t, int32_t, uint32_t, int32_t);
  void (*text)(uint64_t, const uint16_t*, int32_t);
  int32_t (*command)(uint64_t);
  int32_t (*openUrl)(uint64_t, const char*);
  uint32_t (*textState)(uint64_t);
  int32_t (*frameCopy)(uint64_t, void*, int32_t, int32_t*, int32_t*, uint64_t*, int32_t*);
  int32_t (*metrics)(void*, int32_t, int32_t, float, float, int32_t, int32_t, int32_t);
  void (*releaseMetrics)(void*);
  Api() {
    InitializeWindowFaultDiagnostics();
    HMODULE module = ::GetModuleHandleW(L"xul.dll");
    if (!module) throw hresult_error(E_PENDING, L"The shared Gecko runtime is not ready.");
    auto version = Resolve<uint32_t (*)()>(module, "gecko_w10m_window_registry_version");
    if (version() < 2) throw hresult_error(E_NOINTERFACE, L"Update the packaged Gecko engine.");
    auto listWindows = Resolve<uint32_t (*)(uint64_t*, uint32_t)>(module, "gecko_w10m_window_ids");
    // IsStarted means the worker was launched, not that XPCOM's main thread
    // and the first browser exist. The Shell retries E_PENDING during warmup.
    if (!listWindows(nullptr, 0))
      throw hresult_error(E_PENDING, L"The shared Gecko browser is still starting.");
#define WINDOW_API(member, symbol) member = Resolve<decltype(member)>(module, symbol)
    WINDOW_API(create, "gecko_w10m_create_browser_window");
    WINDOW_API(status, "gecko_w10m_window_request_status");
    WINDOW_API(cancel, "gecko_w10m_cancel_window_creation");
    WINDOW_API(release, "gecko_w10m_release_window_request");
    WINDOW_API(resize, "gecko_w10m_window_resize");
    WINDOW_API(activate, "gecko_w10m_window_activate");
    WINDOW_API(requestClose, "gecko_w10m_window_close");
    WINDOW_API(appState, "gecko_w10m_app_state");
    WINDOW_API(mouse, "gecko_w10m_window_mouse");
    WINDOW_API(wheel, "gecko_w10m_window_wheel");
    WINDOW_API(key, "gecko_w10m_window_key");
    WINDOW_API(text, "gecko_w10m_window_text");
    WINDOW_API(command, "gecko_w10m_take_managed_window_command");
    WINDOW_API(openUrl, "gecko_w10m_window_open_url");
    WINDOW_API(textState, "gecko_w10m_window_text_state");
    WINDOW_API(frameCopy, "gecko_w10m_window_frame_copy");
#undef WINDOW_API
    HMODULE angle = ::GetModuleHandleW(L"libGLESv2.dll");
    // Software startup may never load ANGLE. Load the packaged module now so
    // this panel's metrics exist before any later switch to GPU rendering.
    // The shared runtime owns its DLLs for the lifetime of the process.
    if (!angle) angle = ::LoadPackagedLibrary(L"libGLESv2.dll", 0);
    if (!angle) throw hresult_error(HRESULT_FROM_WIN32(::GetLastError()),
        L"The packaged ANGLE renderer could not be loaded.");
    auto metricsVersion = Resolve<uint32_t (*)()>(angle, "angle_uwp_window_metrics_version");
    if (metricsVersion() != 1)
      throw hresult_error(E_NOINTERFACE, L"The packaged ANGLE window-metrics ABI is incompatible.");
    metrics = Resolve<decltype(metrics)>(angle, "angle_uwp_set_window_metrics");
    releaseMetrics = Resolve<decltype(releaseMetrics)>(angle, "angle_uwp_release_window_metrics");
  }
};

GeckoWindow::GeckoWindow(hstring const& url, bool appMode)
    : url_(url), appMode_(appMode) {
  api_ = std::make_shared<Api>();
  dispatcher_ = Windows::UI::Core::CoreWindow::GetForCurrentThread().Dispatcher();
  scale_ = Windows::Graphics::Display::DisplayInformation::GetForCurrentView().RawPixelsPerViewPixel();
  if (!std::isfinite(scale_) || scale_ <= 0) scale_ = 1;
  root_ = Windows::UI::Xaml::Controls::Grid();
  panel_ = Windows::UI::Xaml::Controls::SwapChainPanel();
  root_.Children().Append(panel_);
  softwareSurface_ = Windows::UI::Xaml::Controls::Image();
  softwareSurface_.IsHitTestVisible(false);
  softwareSurface_.Stretch(Windows::UI::Xaml::Media::Stretch::Fill);
  softwareSurface_.Visibility(Windows::UI::Xaml::Visibility::Collapsed);
  root_.Children().Append(softwareSurface_);
}
void GeckoWindow::CheckUIThread() const {
  if (!dispatcher_ || !dispatcher_.HasThreadAccess()) throw hresult_error(RPC_E_WRONG_THREAD);
}
GeckoWindow::~GeckoWindow() {
  // Explicit Close is the normal path. A managed finalizer may run elsewhere;
  // move the final panel reference to its own dispatcher in that case.
  if (!id_) {
    if (metricsPublished_) api_->releaseMetrics(metricsKey_);
    return;
  }
  auto api = api_;
  auto id = id_;
  auto panel = panel_.as<::IUnknown>();
  api->cancel(id);
  try {
    auto retire = [api, id, key = metricsKey_, panel = std::move(panel)]() mutable {
      auto& queue = Retirements();
      queue.entries.push_back({id, std::move(panel), api->status, api->release,
                               api->releaseMetrics, key});
      queue.timer.Start();
    };
    if (dispatcher_.HasThreadAccess()) retire();
    else dispatcher_.RunAsync(Windows::UI::Core::CoreDispatcherPriority::Normal,
                              std::move(retire));
  } catch (...) {
    // Process/dispatcher teardown: native runtime and its panels end together.
  }
}
Windows::Foundation::IInspectable GeckoWindow::Content() const { return root_; }
uint64_t GeckoWindow::Id() const noexcept { return id_; }
uint32_t GeckoWindow::State() const { return id_ ? api_->status(id_, nullptr) : (closing_ ? 3 : 0); }
uint32_t GeckoWindow::Error() const {
  uint32_t error = 0;
  if (id_) api_->status(id_, &error);
  return error;
}
uint32_t GeckoWindow::TextInputState() const {
  return active_ && !closing_ && id_ ? api_->textState(id_) : 0;
}
bool GeckoWindow::UpdateFrame() {
  CheckUIThread();
  if (closing_ || !id_) return false;
  int32_t width = 0, height = 0, source = -1;
  api_->frameCopy(id_, nullptr, 0, &width, &height, nullptr, &source);
  if (source < 0) return softwareVisible_;
  if (source == 0) {
    softwareSurface_.Visibility(Windows::UI::Xaml::Visibility::Collapsed);
    softwareSurface_.Source(nullptr);
    softwareBitmap_ = nullptr;
    bitmapWidth_ = bitmapHeight_ = 0;
    frameSerial_ = 0;
    softwareVisible_ = false;
    return false;
  }
  const int64_t bytes = int64_t(width) * height * 4;
  if (width <= 0 || height <= 0 || bytes > INT32_MAX) return softwareVisible_;
  if (!softwareBitmap_ || width != bitmapWidth_ || height != bitmapHeight_) {
    softwareBitmap_ = Windows::UI::Xaml::Media::Imaging::WriteableBitmap(width, height);
    bitmapWidth_ = width; bitmapHeight_ = height;
    frameSerial_ = 0;
  }
  auto buffer = softwareBitmap_.PixelBuffer();
  uint8_t* pixels = nullptr;
  check_hresult(buffer.as<::Windows::Storage::Streams::IBufferByteAccess>()->Buffer(&pixels));
  int32_t copied = api_->frameCopy(id_, pixels, static_cast<int32_t>(buffer.Capacity()),
      &width, &height, &frameSerial_, &source);
  if (copied) {
    softwareBitmap_.Invalidate();
    softwareSurface_.Source(softwareBitmap_);
    softwareSurface_.Visibility(Windows::UI::Xaml::Visibility::Visible);
    softwareVisible_ = true;
  }
  return source != 0;
}
void GeckoWindow::SetViewport(double width, double height) {
  CheckUIThread();
  if (closing_ || width <= 0 || height <= 0) return;
  const bool confirmViewport = id_ && !viewportConfirmed_ && State() == 2;
  const auto display = Windows::Graphics::Display::DisplayInformation::GetForCurrentView();
  const double currentScale = display.RawPixelsPerViewPixel();
  if (std::isfinite(currentScale) && currentScale > 0) scale_ = currentScale;
  int32_t w = Pixels(width, scale_), h = Pixels(height, scale_);
  if (w <= 0 || h <= 0) return;
  const double arrangedWidth = panel_.ActualWidth(), arrangedHeight = panel_.ActualHeight();
  if (arrangedWidth <= 0 || arrangedHeight <= 0) return;
  const float sx = static_cast<float>(w / arrangedWidth);
  const float sy = static_cast<float>(h / arrangedHeight);
  const int32_t capacityW = std::max(capacityWidth_, std::max(w,
      static_cast<int32_t>(display.ScreenWidthInRawPixels())));
  const int32_t capacityH = std::max(capacityHeight_, std::max(h,
      static_cast<int32_t>(display.ScreenHeightInRawPixels())));
  const bool sizeChanged = w != lastWidth_ || h != lastHeight_;
  if (sizeChanged || sx != lastScaleX_ || sy != lastScaleY_ ||
      capacityW != capacityWidth_ || capacityH != capacityHeight_) {
    metricsKey_ = get_abi(panel_);
    if (!api_->metrics(metricsKey_, w, h, sx, sy, capacityW, capacityH, 2))
      throw hresult_error(E_INVALIDARG, L"ANGLE rejected the window's panel metrics.");
    metricsPublished_ = true;
    lastWidth_ = w; lastHeight_ = h;
    lastScaleX_ = sx; lastScaleY_ = sy;
    capacityWidth_ = capacityW; capacityHeight_ = capacityH;
  }
  if (!id_) {
    // Call after Content has been attached and arranged. Attaching an EGL
    // surface to a 0x0 XAML panel previously broke scaling on Xbox.
    if (panel_.ActualWidth() <= 0 || panel_.ActualHeight() <= 0) return;
    std::string url = to_string(url_);
    id_ = api_->create(get_abi(panel_), url.c_str(), w, h, appMode_ ? 1 : 0);
    if (!id_) throw hresult_error(E_FAIL, L"The engine rejected the browser-window request.");
    gecko_w10m::client::Log::Write(L"window-session: requested id=" + std::to_wstring(id_));
  } else if (sizeChanged || confirmViewport) {
    api_->resize(id_, w, h);
    if (State() == 2) viewportConfirmed_ = true;
  }
}
void GeckoWindow::SetActive(bool active) {
  CheckUIThread();
  if (!active && id_ && State() == 2) {
    for (size_t key = 0; key < pressedKeys_.size(); ++key) {
      if (pressedKeys_[key]) api_->key(id_, 0, static_cast<int32_t>(key), 0, 0);
    }
    for (size_t button = 0; button < pressedButtons_.size(); ++button) {
      if (pressedButtons_[button]) api_->mouse(id_, 2, static_cast<int32_t>(button), lastX_, lastY_);
    }
  }
  if (!active) { pressedKeys_.fill(false); pressedButtons_.fill(false); }
  active_ = active && !closing_;
  if (active_ && id_ && State() == 2) { api_->appState(1); api_->activate(id_); }
}
void GeckoWindow::SendMouse(int32_t kind, int32_t button, double x, double y) {
  CheckUIThread();
  if (active_ && !closing_ && id_ && State() == 2 && kind >= 0 && kind <= 2 && button >= 0 && button < 3) {
    lastX_ = Pixels(x, scale_); lastY_ = Pixels(y, scale_);
    api_->mouse(id_, kind, button, lastX_, lastY_);
    if (kind != 0) pressedButtons_[button] = kind == 1;
  }
}
bool GeckoWindow::OpenUrl(hstring const& url) {
  CheckUIThread();
  if (closing_ || !id_ || State() != 2) return false;
  Windows::Foundation::Uri target(url);
  if (target.SchemeName() != L"https" && target.SchemeName() != L"http")
    throw hresult_invalid_argument();
  auto utf8 = to_string(target.AbsoluteUri());
  return api_->openUrl(id_, utf8.c_str()) != 0;
}
bool GeckoWindow::ContextAction(hstring const& action) {
  CheckUIThread();
  if (closing_ || !id_ || State() != 2) return false;
  if (action != L"copy" && action != L"paste" && action != L"saveimage" && action != L"dismiss")
    throw hresult_invalid_argument();
  auto command = std::string("desktopmode-context:") + to_string(action);
  return api_->openUrl(id_, command.c_str()) != 0;
}
void GeckoWindow::SendWheel(double x, double y, double dx, double dy) {
  CheckUIThread();
  if (active_ && !closing_ && id_ && State() == 2 && std::isfinite(dx) && std::isfinite(dy))
    api_->wheel(id_, Pixels(x, scale_), Pixels(y, scale_), dx, dy);
}
void GeckoWindow::SendKey(bool down, int32_t key, uint32_t modifiers, bool repeat) {
  CheckUIThread();
  if (active_ && !closing_ && id_ && State() == 2 && key >= 0 && key < 256) {
    api_->key(id_, down, key, modifiers, repeat);
    pressedKeys_[key] = down;
  }
}
void GeckoWindow::SendText(hstring const& text) {
  CheckUIThread();
  if (active_ && !closing_ && id_ && State() == 2 && !text.empty())
    api_->text(id_, reinterpret_cast<const uint16_t*>(text.c_str()), static_cast<int32_t>(text.size()));
}
int32_t GeckoWindow::TakeWindowCommand() {
  CheckUIThread();
  return !closing_ && id_ ? api_->command(id_) : 0;
}
void GeckoWindow::Close() {
  CheckUIThread();
  if (closing_) return;
  SetActive(false);
  closing_ = true;
  active_ = false;
  softwareSurface_.Source(nullptr);
  softwareSurface_.Visibility(Windows::UI::Xaml::Visibility::Collapsed);
  softwareBitmap_ = nullptr;
  softwareVisible_ = false;
  if (id_) api_->cancel(id_);
}
void GeckoWindow::RequestClose() {
  CheckUIThread();
  if (!closing_ && id_ && State() == 2) api_->requestClose(id_);
}
}
