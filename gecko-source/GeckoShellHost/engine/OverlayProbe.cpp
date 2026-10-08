// OverlayProbe.cpp — can this phone show a video without the GPU drawing it?
//
// Fullscreen 1080p60 costs the compositor 22-29 ms a frame, and almost all of
// it is one thing: the video, sampled from NV12 and scaled to 2560x1440 by the
// 3D engine, with the system compositor then drawing the result a second time.
// The way out is the one every phone's own player takes: hand the video to
// the display as a layer of its own -- a hardware overlay plane, scanned out
// and scaled by the display controller, not drawn by anyone.
//
// Whether that is possible here is a set of questions only the phone can
// answer, and this asks them once, at launch, without changing anything:
//
//   * does the display output exist for this app, and does it support
//     overlays at all, and for which formats (NV12, YUY2, BGRA);
//   * is there a video processor for NV12 to BGRA with scaling -- the engine
//     that would do the conversion if the display cannot take NV12;
//   * can a swap chain for a composition surface handle be made, in NV12 and
//     in BGRA (IDXGIFactoryMedia, which is what the system's own media
//     element uses);
//   * will a XAML SwapChainPanel take such a handle (ISwapChainPanelNative2).
//
// If the display has no overlays, a second layer would only give the system
// compositor more to draw, and the plan is dropped.
#include "pch.h"

#include "OverlayProbe.h"

#include <d3d11.h>
#include <dxgi1_3.h>
#include <windows.ui.xaml.media.dxinterop.h>

#include <cmath>
#include <functional>
#include <utility>
#include <vector>
#include <string>
#include <thread>

#include "../client/Log.h"

namespace gecko_w10m::engine {
namespace {

using gecko_w10m::client::Log;

// IDXGIFactoryMedia: the SDK declares it for desktop apps only, but the
// interface is the system's and an app container may ask for it.
MIDL_INTERFACE("41e7d1f2-a591-4f7b-a2e5-fa9c843e1c12")
IGeckoW10mFactoryMedia : public IUnknown {
 public:
  virtual HRESULT STDMETHODCALLTYPE CreateSwapChainForCompositionSurfaceHandle(
      IUnknown* device, HANDLE surface, const DXGI_SWAP_CHAIN_DESC1* desc,
      IDXGIOutput* restrictToOutput, IDXGISwapChain1** swapChain) = 0;
  virtual HRESULT STDMETHODCALLTYPE
  CreateDecodeSwapChainForCompositionSurfaceHandle(
      IUnknown* device, HANDLE surface, void* desc, IUnknown* yuvDecodeBuffers,
      IDXGIOutput* restrictToOutput, IUnknown** swapChain) = 0;
};

constexpr DWORD kCompositionObjectAllAccess = 0x0003;  // COMPOSITIONOBJECT_ALL_ACCESS
// 512, not 64: 64 is FRAME_LATENCY_WAITABLE_OBJECT, which is what the first
// probe passed, and why its NV12 chain was refused.
constexpr UINT kSwapChainFlagFullscreenVideo = 256;  // DXGI_SWAP_CHAIN_FLAG_FULLSCREEN_VIDEO
constexpr UINT kSwapChainFlagYuvVideo = 512;         // DXGI_SWAP_CHAIN_FLAG_YUV_VIDEO

// IDXGISwapChainMedia and its statistics, desktop-only in the SDK as well.
struct GeckoW10mFrameStatisticsMedia {
  UINT PresentCount;
  UINT PresentRefreshCount;
  UINT SyncRefreshCount;
  LARGE_INTEGER SyncQPCTime;
  LARGE_INTEGER SyncGPUTime;
  UINT CompositionMode;  // 0 composed, 1 overlay, 2 none, 3 composition failure
  UINT ApprovedPresentDuration;
};
MIDL_INTERFACE("dd95b90b-f05f-4f6a-bd65-25bfb264bd84")
IGeckoW10mSwapChainMedia : public IUnknown {
 public:
  virtual HRESULT STDMETHODCALLTYPE GetFrameStatisticsMedia(
      GeckoW10mFrameStatisticsMedia* stats) = 0;
  virtual HRESULT STDMETHODCALLTYPE SetPresentDuration(UINT duration) = 0;
  virtual HRESULT STDMETHODCALLTYPE CheckPresentDurationSupport(
      UINT desired, UINT* lower, UINT* higher) = 0;
};

std::wstring Hex(uint32_t value) {
  wchar_t text[16];
  swprintf_s(text, L"0x%08x", value);
  return text;
}

std::wstring OverlayFlags(UINT flags) {
  if (!flags) {
    return L"none";
  }
  std::wstring out;
  if (flags & 1) out += L"direct ";     // DXGI_OVERLAY_SUPPORT_FLAG_DIRECT
  if (flags & 2) out += L"scaling ";    // DXGI_OVERLAY_SUPPORT_FLAG_SCALING
  if (flags & ~3u) out += Hex(flags);
  return out;
}

void TrySwapChain(IGeckoW10mFactoryMedia* media, ID3D11Device* device,
                  HRESULT(WINAPI* createHandle)(DWORD, SECURITY_ATTRIBUTES*,
                                                 HANDLE*),
                  DXGI_FORMAT format, UINT flags, const wchar_t* name) {
  HANDLE surface = nullptr;
  HRESULT hr = createHandle(kCompositionObjectAllAccess, nullptr, &surface);
  if (FAILED(hr) || !surface) {
    Log::Write(std::wstring(L"overlay: no composition surface handle for ") +
               name + L", " + Hex(hr));
    return;
  }
  DXGI_SWAP_CHAIN_DESC1 desc = {};
  desc.Width = 1920;
  desc.Height = 1080;
  desc.Format = format;
  desc.SampleDesc.Count = 1;
  desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
  desc.BufferCount = 2;
  desc.Scaling = DXGI_SCALING_STRETCH;
  desc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL;
  desc.AlphaMode = DXGI_ALPHA_MODE_IGNORE;
  desc.Flags = flags;
  IDXGISwapChain1* chain = nullptr;
  hr = media->CreateSwapChainForCompositionSurfaceHandle(device, surface, &desc,
                                                         nullptr, &chain);
  Log::Write(std::wstring(L"overlay: a ") + name +
             L" swap chain for a composition surface: " +
             (SUCCEEDED(hr) ? std::wstring(L"made") : L"refused, " + Hex(hr)));
  if (chain) {
    chain->Release();
  }
  ::CloseHandle(surface);
}

void ProbeDevice() {
  HMODULE d3d11 = ::LoadLibraryExW(L"d3d11.dll", nullptr,
                                   LOAD_LIBRARY_SEARCH_SYSTEM32);
  using CreateFn = HRESULT(WINAPI*)(IDXGIAdapter*, D3D_DRIVER_TYPE, HMODULE,
                                    UINT, const D3D_FEATURE_LEVEL*, UINT, UINT,
                                    ID3D11Device**, D3D_FEATURE_LEVEL*,
                                    ID3D11DeviceContext**);
  auto create = d3d11 ? reinterpret_cast<CreateFn>(
                            ::GetProcAddress(d3d11, "D3D11CreateDevice"))
                      : nullptr;
  if (!create) {
    Log::Write(L"overlay: no D3D11CreateDevice");
    return;
  }
  const D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_11_1,
                                      D3D_FEATURE_LEVEL_11_0,
                                      D3D_FEATURE_LEVEL_10_1,
                                      D3D_FEATURE_LEVEL_10_0,
                                      D3D_FEATURE_LEVEL_9_3};
  winrt::com_ptr<ID3D11Device> device;
  winrt::com_ptr<ID3D11DeviceContext> context;
  HRESULT hr = create(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr,
                      D3D11_CREATE_DEVICE_BGRA_SUPPORT |
                          D3D11_CREATE_DEVICE_VIDEO_SUPPORT,
                      levels, ARRAYSIZE(levels), D3D11_SDK_VERSION,
                      device.put(), nullptr, context.put());
  if (FAILED(hr)) {
    Log::Write(L"overlay: no video-capable device, " + Hex(hr));
    return;
  }

  // The display, and what it can lay over the picture by itself.
  winrt::com_ptr<IDXGIDevice> dxgi = device.as<IDXGIDevice>();
  winrt::com_ptr<IDXGIAdapter> adapter;
  dxgi->GetAdapter(adapter.put());
  UINT outputs = 0;
  for (;; ++outputs) {
    winrt::com_ptr<IDXGIOutput> output;
    if (adapter->EnumOutputs(outputs, output.put()) != S_OK) {
      break;
    }
    if (outputs > 0) {
      continue;
    }
    auto output2 = output.try_as<IDXGIOutput2>();
    Log::Write(std::wstring(L"overlay: output 0 says overlays are ") +
               (output2 ? (output2->SupportsOverlays() ? L"SUPPORTED"
                                                       : L"not supported")
                        : L"unknown (no IDXGIOutput2)"));
    if (auto output3 = output.try_as<IDXGIOutput3>()) {
      const struct {
        DXGI_FORMAT format;
        const wchar_t* name;
      } formats[] = {{DXGI_FORMAT_NV12, L"NV12"},
                     {DXGI_FORMAT_YUY2, L"YUY2"},
                     {DXGI_FORMAT_B8G8R8A8_UNORM, L"BGRA"}};
      for (const auto& f : formats) {
        UINT flags = 0;
        hr = output3->CheckOverlaySupport(f.format, device.get(), &flags);
        Log::Write(std::wstring(L"overlay: ") + f.name + L" -- " +
                   (SUCCEEDED(hr) ? OverlayFlags(flags) : L"error " + Hex(hr)));
      }
    }
  }
  Log::WriteNum(L"overlay: display outputs visible to the app", outputs);

  // The video engine: NV12 in, BGRA out, 1920x1080 scaled to 2560x1440.
  if (auto video = device.try_as<ID3D11VideoDevice>()) {
    D3D11_VIDEO_PROCESSOR_CONTENT_DESC desc = {};
    desc.InputFrameFormat = D3D11_VIDEO_FRAME_FORMAT_PROGRESSIVE;
    desc.InputWidth = 1920;
    desc.InputHeight = 1080;
    desc.OutputWidth = 2560;
    desc.OutputHeight = 1440;
    desc.InputFrameRate = {60, 1};
    desc.OutputFrameRate = {60, 1};
    desc.Usage = D3D11_VIDEO_USAGE_PLAYBACK_NORMAL;
    winrt::com_ptr<ID3D11VideoProcessorEnumerator> processors;
    hr = video->CreateVideoProcessorEnumerator(&desc, processors.put());
    if (SUCCEEDED(hr)) {
      UINT in = 0;
      UINT out = 0;
      processors->CheckVideoProcessorFormat(DXGI_FORMAT_NV12, &in);
      processors->CheckVideoProcessorFormat(DXGI_FORMAT_B8G8R8A8_UNORM, &out);
      Log::Write(std::wstring(L"overlay: video processor -- NV12 ") +
                 ((in & D3D11_VIDEO_PROCESSOR_FORMAT_SUPPORT_INPUT) ? L"in"
                                                                    : L"NOT in") +
                 L", BGRA " +
                 ((out & D3D11_VIDEO_PROCESSOR_FORMAT_SUPPORT_OUTPUT) ? L"out"
                                                                      : L"NOT out"));
    } else {
      Log::Write(L"overlay: no video processor for 1080p to 1440p, " + Hex(hr));
    }
  } else {
    Log::Write(L"overlay: the device has no video interface");
  }

  // A swap chain the system compositor can take as a layer of its own.
  winrt::com_ptr<IDXGIFactory2> factory;
  adapter->GetParent(__uuidof(IDXGIFactory2), factory.put_void());
  winrt::com_ptr<IGeckoW10mFactoryMedia> media;
  if (factory) {
    factory->QueryInterface(__uuidof(IGeckoW10mFactoryMedia), media.put_void());
  }
  HMODULE dcomp = ::LoadLibraryExW(L"dcomp.dll", nullptr,
                                   LOAD_LIBRARY_SEARCH_SYSTEM32);
  using HandleFn = HRESULT(WINAPI*)(DWORD, SECURITY_ATTRIBUTES*, HANDLE*);
  auto createHandle =
      dcomp ? reinterpret_cast<HandleFn>(
                  ::GetProcAddress(dcomp, "DCompositionCreateSurfaceHandle"))
            : nullptr;
  Log::Write(std::wstring(L"overlay: media swap chains ") +
             (media ? L"available" : L"NOT available") +
             L", composition surface handles " +
             (createHandle ? L"available"
                           : (dcomp ? L"NOT available (no export)"
                                    : L"NOT available (no dcomp.dll)")));
  if (media && createHandle) {
    TrySwapChain(media.get(), device.get(), createHandle, DXGI_FORMAT_NV12,
                 kSwapChainFlagYuvVideo, L"NV12");
    TrySwapChain(media.get(), device.get(), createHandle,
                 DXGI_FORMAT_B8G8R8A8_UNORM, 0, L"BGRA");
  }
}

const wchar_t* ModeName(UINT mode) {
  switch (mode) {
    case 0: return L"composed by the GPU";
    case 1: return L"HARDWARE OVERLAY";
    case 2: return L"not shown";
    case 3: return L"composition failure";
  }
  return L"unknown";
}

void OverlayTestThread(winrt::Windows::UI::Xaml::Controls::SwapChainPanel panel,
                       winrt::Windows::UI::Xaml::Controls::Grid host,
                       winrt::Windows::UI::Core::CoreDispatcher dispatcher,
                       double shownWidth, double shownHeight) {
  auto finish = [&](const std::wstring& why) {
    Log::Write(L"overlay test: " + why);
    dispatcher.RunAsync(winrt::Windows::UI::Core::CoreDispatcherPriority::Normal,
                        [host, panel] {
                          uint32_t index = 0;
                          if (host.Children().IndexOf(panel, index)) {
                            host.Children().RemoveAt(index);
                          }
                        });
  };

  HMODULE d3d11 = ::LoadLibraryExW(L"d3d11.dll", nullptr,
                                   LOAD_LIBRARY_SEARCH_SYSTEM32);
  using CreateFn = HRESULT(WINAPI*)(IDXGIAdapter*, D3D_DRIVER_TYPE, HMODULE,
                                    UINT, const D3D_FEATURE_LEVEL*, UINT, UINT,
                                    ID3D11Device**, D3D_FEATURE_LEVEL*,
                                    ID3D11DeviceContext**);
  auto create = d3d11 ? reinterpret_cast<CreateFn>(
                            ::GetProcAddress(d3d11, "D3D11CreateDevice"))
                      : nullptr;
  HMODULE dcomp = ::LoadLibraryExW(L"dcomp.dll", nullptr,
                                   LOAD_LIBRARY_SEARCH_SYSTEM32);
  using HandleFn = HRESULT(WINAPI*)(DWORD, SECURITY_ATTRIBUTES*, HANDLE*);
  auto createHandle =
      dcomp ? reinterpret_cast<HandleFn>(
                  ::GetProcAddress(dcomp, "DCompositionCreateSurfaceHandle"))
            : nullptr;
  if (!create || !createHandle) {
    finish(L"missing D3D11CreateDevice or DCompositionCreateSurfaceHandle");
    return;
  }
  const D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_11_1,
                                      D3D_FEATURE_LEVEL_11_0};
  winrt::com_ptr<ID3D11Device> device;
  winrt::com_ptr<ID3D11DeviceContext> context;
  HRESULT hr = create(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr,
                      D3D11_CREATE_DEVICE_BGRA_SUPPORT |
                          D3D11_CREATE_DEVICE_VIDEO_SUPPORT,
                      levels, ARRAYSIZE(levels), D3D11_SDK_VERSION,
                      device.put(), nullptr, context.put());
  if (FAILED(hr)) {
    finish(L"no device, " + Hex(hr));
    return;
  }
  winrt::com_ptr<IDXGIAdapter> adapter;
  device.as<IDXGIDevice>()->GetAdapter(adapter.put());
  winrt::com_ptr<IDXGIFactory2> factory;
  adapter->GetParent(__uuidof(IDXGIFactory2), factory.put_void());
  winrt::com_ptr<IGeckoW10mFactoryMedia> media;
  if (factory) {
    factory->QueryInterface(__uuidof(IGeckoW10mFactoryMedia), media.put_void());
  }
  if (!media) {
    finish(L"no media factory");
    return;
  }

  HANDLE surface = nullptr;
  hr = createHandle(kCompositionObjectAllAccess, nullptr, &surface);
  if (FAILED(hr) || !surface) {
    finish(L"no composition surface handle, " + Hex(hr));
    return;
  }

  // NV12 first, as a video chain -- the format the display said it can lay
  // over the picture with scaling -- then as fullscreen video too, then
  // BGRA for comparison, which the display said it cannot.
  struct Attempt {
    DXGI_FORMAT format;
    UINT flags;
    const wchar_t* name;
  };
  const Attempt attempts[] = {
      {DXGI_FORMAT_NV12, kSwapChainFlagYuvVideo, L"NV12 (YUV video)"},
      {DXGI_FORMAT_NV12, kSwapChainFlagYuvVideo | kSwapChainFlagFullscreenVideo,
       L"NV12 (YUV + fullscreen video)"},
      {DXGI_FORMAT_NV12, 0, L"NV12 (no flags)"},
      {DXGI_FORMAT_B8G8R8A8_UNORM, 0, L"BGRA"},
  };
  winrt::com_ptr<IDXGISwapChain1> chain;
  const Attempt* used = nullptr;
  for (const Attempt& a : attempts) {
    DXGI_SWAP_CHAIN_DESC1 desc = {};
    desc.Width = 1920;
    desc.Height = 1080;
    desc.Format = a.format;
    desc.SampleDesc.Count = 1;
    desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    desc.BufferCount = 2;
    desc.Scaling = DXGI_SCALING_STRETCH;
    desc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL;
    desc.AlphaMode = DXGI_ALPHA_MODE_IGNORE;
    desc.Flags = a.flags;
    hr = media->CreateSwapChainForCompositionSurfaceHandle(
        device.get(), surface, &desc, nullptr, chain.put());
    Log::Write(std::wstring(L"overlay test: ") + a.name + L" swap chain " +
               (SUCCEEDED(hr) ? std::wstring(L"made") : L"refused, " + Hex(hr)));
    if (SUCCEEDED(hr)) {
      used = &a;
      break;
    }
    chain = nullptr;
  }
  if (!chain) {
    ::CloseHandle(surface);
    finish(L"no swap chain at all");
    return;
  }

  // The panel takes the surface on the UI thread.
  HANDLE given = ::CreateEventW(nullptr, TRUE, FALSE, nullptr);
  HRESULT setHr = E_FAIL;
  dispatcher.RunAsync(
      winrt::Windows::UI::Core::CoreDispatcherPriority::High,
      [panel, surface, given, &setHr] {
        winrt::com_ptr<ISwapChainPanelNative2> native2;
        HRESULT q = winrt::get_unknown(panel)->QueryInterface(
            __uuidof(ISwapChainPanelNative2), native2.put_void());
        setHr = SUCCEEDED(q) ? native2->SetSwapChainHandle(surface) : q;
        ::SetEvent(given);
      });
  ::WaitForSingleObject(given, 5000);
  ::CloseHandle(given);
  if (FAILED(setHr)) {
    ::CloseHandle(surface);
    finish(L"the panel would not take the surface, " + Hex(setHr));
    return;
  }

  // Something to show: the planes cleared to a slowly changing colour.
  winrt::com_ptr<ID3D11Texture2D> buffer;
  chain->GetBuffer(0, __uuidof(ID3D11Texture2D), buffer.put_void());
  winrt::com_ptr<ID3D11RenderTargetView> luma;
  winrt::com_ptr<ID3D11RenderTargetView> chroma;
  winrt::com_ptr<ID3D11RenderTargetView> bgra;
  if (buffer) {
    D3D11_RENDER_TARGET_VIEW_DESC rtv = {};
    rtv.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2D;
    if (used->format == DXGI_FORMAT_NV12) {
      rtv.Format = DXGI_FORMAT_R8_UNORM;
      device->CreateRenderTargetView(buffer.get(), &rtv, luma.put());
      rtv.Format = DXGI_FORMAT_R8G8_UNORM;
      device->CreateRenderTargetView(buffer.get(), &rtv, chroma.put());
      Log::Write(std::wstring(L"overlay test: drawing into the planes ") +
                 (luma && chroma ? L"works"
                                 : L"does NOT work, frames stay as they are"));
    } else {
      device->CreateRenderTargetView(buffer.get(), nullptr, bgra.put());
    }
  }

  auto stats = chain.try_as<IGeckoW10mSwapChainMedia>();
  if (!stats) {
    Log::Write(L"overlay test: no media statistics on this chain");
  }

  // Runs on the UI thread and waits for it.
  auto onUi = [&](std::function<void()> work) {
    HANDLE done = ::CreateEventW(nullptr, TRUE, FALSE, nullptr);
    dispatcher.RunAsync(winrt::Windows::UI::Core::CoreDispatcherPriority::High,
                        [work, done] {
                          try {
                            work();
                          } catch (...) {
                          }
                          ::SetEvent(done);
                        });
    ::WaitForSingleObject(done, 5000);
    ::CloseHandle(done);
  };

  // Three phases of four seconds. The video's controls are drawn over it, so
  // the question is not only whether a video layer can be an overlay, but
  // whether it stays one with something composited above it -- and whether
  // it can lie under the browser's own layer and show through it.
  const wchar_t* phases[] = {
      L"1: on top of everything",
      L"2: a translucent band over its lower half (like a player's controls)",
      L"3: UNDER the browser, which is translucent",
  };
  winrt::Windows::UI::Xaml::Controls::Border veil{nullptr};
  std::vector<std::pair<winrt::Windows::UI::Xaml::UIElement, double>> dimmed;
  UINT frame = 0;
  for (int phase = 0; phase < 3; ++phase) {
    if (phase == 1) {
      onUi([&] {
        using namespace winrt::Windows::UI::Xaml;
        veil = winrt::Windows::UI::Xaml::Controls::Border();
        winrt::Windows::UI::Xaml::Media::SolidColorBrush brush;
        brush.Color(winrt::Windows::UI::ColorHelper::FromArgb(128, 0, 0, 0));
        veil.Background(brush);
        veil.HorizontalAlignment(HorizontalAlignment::Left);
        veil.VerticalAlignment(VerticalAlignment::Top);
        veil.Width(shownWidth);
        veil.Height(shownHeight / 2);
        veil.Margin(ThicknessHelper::FromLengths(0, shownHeight / 2, 0, 0));
        veil.IsHitTestVisible(false);
        const int rows = static_cast<int>(host.RowDefinitions().Size());
        if (rows > 1) {
          winrt::Windows::UI::Xaml::Controls::Grid::SetRowSpan(veil, rows);
        }
        host.Children().Append(veil);
      });
    } else if (phase == 2) {
      onUi([&] {
        uint32_t index = 0;
        if (veil && host.Children().IndexOf(veil, index)) {
          host.Children().RemoveAt(index);
        }
        if (host.Children().IndexOf(panel, index)) {
          host.Children().RemoveAt(index);
        }
        for (auto const& child : host.Children()) {
          dimmed.emplace_back(child, child.Opacity());
          child.Opacity(0.6);
        }
        host.Children().InsertAt(0, panel);
      });
    }
    Log::Write(std::wstring(L"overlay test: phase ") + phases[phase]);
    UINT counts[5] = {};
    const ULONGLONG phaseStart = ::GetTickCount64();
    UINT phaseFrames = 0;
    while (::GetTickCount64() - phaseStart < 4000) {
      const float t = static_cast<float>(frame) / 60.0f;
      const float y = 0.5f + 0.25f * sinf(t * 1.3f);
      const float u = 0.5f + 0.2f * sinf(t * 0.7f);
      const float v = 0.5f + 0.2f * cosf(t * 0.9f);
      if (luma && chroma) {
        const float yv[4] = {y, 0, 0, 1};
        const float uv[4] = {u, v, 0, 1};
        context->ClearRenderTargetView(luma.get(), yv);
        context->ClearRenderTargetView(chroma.get(), uv);
      } else if (bgra) {
        const float c[4] = {u, y, v, 1};
        context->ClearRenderTargetView(bgra.get(), c);
      }
      hr = chain->Present(1, 0);
      if (FAILED(hr)) {
        Log::Write(L"overlay test: present failed, " + Hex(hr));
        break;
      }
      ++frame;
      ++phaseFrames;
      // Ask twice a second, skipping the first half second of a phase while
      // the composition settles.
      if (stats && phaseFrames % 30 == 0 && phaseFrames > 30) {
        GeckoW10mFrameStatisticsMedia st = {};
        if (SUCCEEDED(stats->GetFrameStatisticsMedia(&st))) {
          counts[st.CompositionMode < 4 ? st.CompositionMode : 4]++;
        }
      }
    }
    Log::Write(std::wstring(L"overlay test: phase ") + phases[phase] + L" -- " +
               std::to_wstring(phaseFrames) + L" frames; asked " +
               std::to_wstring(counts[0] + counts[1] + counts[2] + counts[3] +
                               counts[4]) +
               L" times: HARDWARE OVERLAY " + std::to_wstring(counts[1]) +
               L", composed by the GPU " + std::to_wstring(counts[0]) +
               L", other " + std::to_wstring(counts[2] + counts[3] + counts[4]));
  }
  onUi([&] {
    for (auto& d : dimmed) {
      d.first.Opacity(d.second);
    }
  });
  chain = nullptr;
  ::CloseHandle(surface);
  finish(L"done, panel removed");
}

}  // namespace

void RunOverlayTest(winrt::Windows::UI::Xaml::Controls::Grid const& host,
                    double rawPerView) {
  if (!host) {
    return;
  }
  using namespace winrt::Windows::UI::Xaml;
  winrt::Windows::UI::Xaml::Controls::SwapChainPanel panel;
  panel.HorizontalAlignment(HorizontalAlignment::Left);
  panel.VerticalAlignment(VerticalAlignment::Top);
  panel.IsHitTestVisible(false);
  // Over everything, in every row of the page.
  const int rows = static_cast<int>(host.RowDefinitions().Size());
  if (rows > 1) {
    winrt::Windows::UI::Xaml::Controls::Grid::SetRowSpan(panel, rows);
  }
  // The 1920-pixel chain is shown one pixel to a pixel, 1920 / rawPerView
  // view pixels wide; scale it to the width of the screen, as a video is.
  const double chainViewWidth = 1920.0 / (rawPerView > 0 ? rawPerView : 1.0);
  const double fitScale = Window::Current().Bounds().Width / chainViewWidth;
  winrt::Windows::UI::Xaml::Media::ScaleTransform fit;
  fit.ScaleX(fitScale);
  fit.ScaleY(fitScale);
  panel.RenderTransform(fit);
  host.Children().Append(panel);
  auto dispatcher = host.Dispatcher();
  const double shownWidth = Window::Current().Bounds().Width;
  const double shownHeight = shownWidth * 1080.0 / 1920.0;
  std::thread([panel, host, dispatcher, shownWidth, shownHeight] {
    try {
      winrt::init_apartment(winrt::apartment_type::multi_threaded);
    } catch (...) {
    }
    try {
      OverlayTestThread(panel, host, dispatcher, shownWidth, shownHeight);
    } catch (...) {
      Log::Write(L"overlay test: threw");
    }
  }).detach();
}

void ProbeVideoOverlay(
    winrt::Windows::UI::Xaml::Controls::SwapChainPanel const& panel) {
  // The panel answers on the UI thread, where this is called.
  if (panel) {
    winrt::com_ptr<ISwapChainPanelNative2> native2;
    HRESULT hr = winrt::get_unknown(panel)->QueryInterface(
        __uuidof(ISwapChainPanelNative2), native2.put_void());
    Log::Write(std::wstring(L"overlay: the panel ") +
               (SUCCEEDED(hr) ? L"takes composition surface handles"
                              : L"takes swap chains only"));
  }
  // The rest makes a device of its own; off the UI thread.
  std::thread([] {
    try {
      ProbeDevice();
    } catch (...) {
      Log::Write(L"overlay: the probe threw");
    }
  }).detach();
}

}  // namespace gecko_w10m::engine
