#include "pch.h"
#include "Bridge/DirectX/GraphicsShims.h"
#include "Bridge/GuestCodePages.h"
#include "Bridge/GuestKernel.h"
#include "Bridge/GuestRuntime.h"
#include "Bridge/GuestStorage.h"
#include "Bridge/GuestWindow.h"
#include "Bridge/RuntimeDiagnostics.h"

#include <d3d11_4.h>
#include <d3d11on12.h>
#include <d3d12.h>
#include <d3dcompiler.h>
#include <dxgi1_6.h>
#include <libloaderapi.h>
#include <wrl/client.h>
#include <algorithm>
#include <atomic>
#include <cstring>
#include <limits>
#include <map>
#include <memory>
#include <mutex>
#include <new>
#include <type_traits>
#include <unordered_map>
#include <utility>
#include <vector>

using Microsoft::WRL::ComPtr;
using namespace Win32Bridge::Bridge;

namespace
{
    MIDL_INTERFACE("50D0B645-4444-42FA-9BB3-E46D0D379AF9") GuestGraphicsObject : public IUnknown
    {
        virtual IUnknown* STDMETHODCALLTYPE NativeObject() = 0;
    };

    ComPtr<IUnknown> NativeGraphicsObject(IUnknown* object)
    {
        ComPtr<GuestGraphicsObject> proxy;
        if (object && SUCCEEDED(object->QueryInterface(IID_PPV_ARGS(&proxy))))
            return ComPtr<IUnknown>(proxy->NativeObject());
        return ComPtr<IUnknown>(object);
    }

    template<class Register>
    HRESULT RegisterGraphicsEvent(HANDLE event, Register registerSignal)
    {
        if (!event) return registerSignal(nullptr);
        const auto kernel = CurrentGuestKernelContext();
        if (!kernel) return E_HANDLE;
        return kernel->RegisterNativeEventSignal(event, [](HANDLE native, void* context)
        {
            return (*static_cast<Register*>(context))(native);
        }, &registerSignal);
    }

    class GraphicsEventNotifications final
    {
    public:
        ~GraphicsEventNotifications()
        {
            if (const auto kernel = m_kernel.lock()) kernel->UnregisterNativeEventNotifications(this);
            else if (const auto current = CurrentGuestKernelContext()) current->UnregisterNativeEventNotifications(this);
        }

        template<class Interface, class Subscribe, class Unsubscribe>
        HRESULT Add(const ComPtr<Interface>& native, HANDLE event, DWORD* cookie, Subscribe subscribe, Unsubscribe unsubscribe)
        {
            const auto kernel = CurrentGuestKernelContext();
            if (!cookie) return E_POINTER;
            *cookie = 0;
            if (!kernel) return E_HANDLE;
            try
            {
                const HRESULT status = kernel->RegisterNativeEventNotification(event, this, cookie,
                    [&](HANDLE handle, DWORD* id) { return (native.Get()->*subscribe)(handle, id); },
                    [native, unsubscribe](DWORD id) -> HRESULT
                    {
                        if constexpr (std::is_void_v<decltype((native.Get()->*unsubscribe)(id))>)
                        {
                            (native.Get()->*unsubscribe)(id);
                            return S_OK;
                        }
                        else return (native.Get()->*unsubscribe)(id);
                    });
                if (SUCCEEDED(status))
                {
                    std::lock_guard<std::mutex> guard(m_lock);
                    m_kernel = kernel->weak_from_this();
                }
                return status;
            }
            catch (const std::bad_alloc&) { return E_OUTOFMEMORY; }
        }

        HRESULT Remove(DWORD cookie)
        {
            const auto kernel = CurrentGuestKernelContext();
            return kernel ? kernel->UnregisterNativeEventNotification(this, cookie) : E_HANDLE;
        }

    private:
        std::mutex m_lock;
        std::weak_ptr<GuestKernelContext> m_kernel;
    };

    template<class Create>
    HRESULT CreateGraphicsShare(const SECURITY_ATTRIBUTES* attributes, LPCWSTR name, HANDLE* output, const Create& create)
    {
        if (!output) return E_POINTER;
        *output = nullptr;
        const auto kernel = CurrentGuestKernelContext();
        if (!kernel || (attributes && attributes->nLength != sizeof(SECURITY_ATTRIBUTES))) return E_INVALIDARG;
        if (attributes && attributes->lpSecurityDescriptor) return E_NOTIMPL;
        HANDLE handle = nullptr;
        HRESULT status = create(&handle);
        if (FAILED(status)) return status;
        status = kernel->PublishGraphicsHandle(handle, name, attributes && attributes->bInheritHandle, output);
        return status == HRESULT_FROM_WIN32(ERROR_ALREADY_EXISTS) ? DXGI_ERROR_NAME_ALREADY_EXISTS : status;
    }

    template<class Interface, class Invoke>
    HRESULT WithNativeGraphicsObjects(UINT count, Interface* const* objects, const Invoke& invoke)
    {
        if (!objects || !count) return invoke(objects);
        try
        {
            std::vector<ComPtr<Interface>> references(count);
            std::vector<Interface*> native(count);
            for (UINT i = 0; i < count; ++i)
            {
                if (!objects[i]) continue;
                const HRESULT status = NativeGraphicsObject(objects[i]).As(&references[i]);
                if (FAILED(status)) return status;
                native[i] = references[i].Get();
            }
            return invoke(native.data());
        }
        catch (const std::bad_alloc&) { return E_OUTOFMEMORY; }
    }

    HRESULT WrapAdapter(ComPtr<IDXGIAdapter> native, REFIID iid, void** output, IDXGIFactory2* parent = nullptr);
    HRESULT WrapD3D11Texture(ComPtr<ID3D11Texture2D> native, ID3D11Device* device, REFIID iid, void** output);

    FARPROC FindD3D12Procedure(const char* name)
    {
        // Keep D3D12 optional on Xbox host configurations that lack the module.
        static const HMODULE module = ::LoadLibraryExW(L"d3d12.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
        return module ? ::GetProcAddress(module, name) : nullptr;
    }

    FARPROC FindShaderCompilerProcedure(const char* name)
    {
        static const HMODULE module = ::LoadLibraryExW(L"d3dcompiler_47.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
        return module ? ::GetProcAddress(module, name) : nullptr;
    }

    std::wstring ShaderSourcePath(LPCSTR name)
    {
        if (!name || !*name) return {};
        const int length = ::MultiByteToWideChar(GuestAnsiCodePage, 0, name, -1, nullptr, 0);
        if (length <= 1) return {};
        std::wstring path(static_cast<size_t>(length), L'\0');
        if (!::MultiByteToWideChar(GuestAnsiCodePage, 0, name, -1, path.data(), length)) return {};
        path.pop_back();
        return path;
    }

    class GuestShaderIncludes final : public ID3DInclude
    {
        struct File
        {
            std::wstring path;
            std::vector<BYTE> bytes;
        };
    public:
        GuestShaderIncludes(GuestStorageContext* storage, std::wstring source)
            : m_storage(storage), m_source(std::move(source)) {}

        HRESULT STDMETHODCALLTYPE Open(D3D_INCLUDE_TYPE type, LPCSTR name, LPCVOID parent, LPCVOID* data, UINT* bytes) override
        {
            if (!data || !bytes) return E_POINTER;
            *data = nullptr;
            *bytes = 0;
            if (!m_storage || !name || (type != D3D_INCLUDE_LOCAL && type != D3D_INCLUDE_SYSTEM))
                return E_INVALIDARG;
            try
            {
                auto path = ShaderSourcePath(name);
                if (path.empty()) return E_INVALIDARG;
                if (path.front() != L'\\' && path.front() != L'/' && (path.size() < 2 || path[1] != L':'))
                {
                    const auto entry = m_openFiles.find(parent);
                    const auto& source = entry == m_openFiles.end() ? m_source : entry->second->path;
                    const auto separator = source.find_last_of(L"\\/");
                    if (separator != std::wstring::npos) path = source.substr(0, separator + 1) + path;
                }

                DWORD error = ERROR_SUCCESS;
                auto file = std::make_unique<File>();
                if (!m_storage->CanonicalPath(path.c_str(), &file->path, &error) ||
                    !m_storage->ReadAllBytes(file->path.c_str(), &file->bytes, &error))
                    return HRESULT_FROM_WIN32(error ? error : ERROR_FILE_NOT_FOUND);
                if (file->bytes.size() > (std::numeric_limits<UINT>::max)()) return E_OUTOFMEMORY;
                const auto count = static_cast<UINT>(file->bytes.size());
                // Even an empty include needs its own identity for Close.
                if (!count) file->bytes.push_back(0);
                const auto address = file->bytes.data();
                m_openFiles.emplace(address, std::move(file));
                *data = address;
                *bytes = count;
                return S_OK;
            }
            catch (const std::bad_alloc&) { return E_OUTOFMEMORY; }
            catch (Platform::Exception^ error) { return error->HResult; }
        }

        HRESULT STDMETHODCALLTYPE Close(LPCVOID data) override
        { return m_openFiles.erase(data) ? S_OK : E_INVALIDARG; }

    private:
        GuestStorageContext* m_storage;
        std::wstring m_source;
        std::unordered_map<LPCVOID, std::unique_ptr<File>> m_openFiles;
    };

    template<class Invoke>
    HRESULT WithShaderIncludes(LPCSTR name, ID3DInclude* includes, const Invoke& invoke)
    {
        if (includes != D3D_COMPILE_STANDARD_FILE_INCLUDE) return invoke(includes);
        try
        {
            GuestShaderIncludes guest(CurrentGuestStorageContext(), ShaderSourcePath(name));
            return invoke(&guest);
        }
        catch (const std::bad_alloc&) { return E_OUTOFMEMORY; }
    }

    HRESULT WINAPI GuestD3DCompile(LPCVOID source, SIZE_T size, LPCSTR name, const D3D_SHADER_MACRO* defines,
        ID3DInclude* includes, LPCSTR entry, LPCSTR target, UINT flags, UINT effectFlags, ID3DBlob** code, ID3DBlob** errors)
    {
        if (code) *code = nullptr;
        if (errors) *errors = nullptr;
        const auto compile = reinterpret_cast<pD3DCompile>(FindShaderCompilerProcedure("D3DCompile"));
        if (!compile) return DXGI_ERROR_UNSUPPORTED;
        return WithShaderIncludes(name, includes, [&](ID3DInclude* includeHandler)
        { return compile(source, size, name, defines, includeHandler, entry, target, flags, effectFlags, code, errors); });
    }

    HRESULT WINAPI GuestD3DCompile2(LPCVOID source, SIZE_T size, LPCSTR name, const D3D_SHADER_MACRO* defines,
        ID3DInclude* includes, LPCSTR entry, LPCSTR target, UINT flags, UINT effectFlags, UINT secondaryFlags,
        LPCVOID secondary, SIZE_T secondarySize, ID3DBlob** code, ID3DBlob** errors)
    {
        if (code) *code = nullptr;
        if (errors) *errors = nullptr;
        const auto compile = reinterpret_cast<decltype(&::D3DCompile2)>(FindShaderCompilerProcedure("D3DCompile2"));
        if (!compile) return DXGI_ERROR_UNSUPPORTED;
        return WithShaderIncludes(name, includes, [&](ID3DInclude* includeHandler)
        {
            return compile(source, size, name, defines, includeHandler, entry, target, flags, effectFlags,
                secondaryFlags, secondary, secondarySize, code, errors);
        });
    }

    HRESULT WINAPI GuestD3DPreprocess(LPCVOID source, SIZE_T size, LPCSTR name, const D3D_SHADER_MACRO* defines,
        ID3DInclude* includes, ID3DBlob** text, ID3DBlob** errors)
    {
        if (text) *text = nullptr;
        if (errors) *errors = nullptr;
        const auto preprocess = reinterpret_cast<decltype(&::D3DPreprocess)>(FindShaderCompilerProcedure("D3DPreprocess"));
        if (!preprocess) return DXGI_ERROR_UNSUPPORTED;
        return WithShaderIncludes(name, includes, [&](ID3DInclude* includeHandler)
        { return preprocess(source, size, name, defines, includeHandler, text, errors); });
    }

    HRESULT WINAPI GuestD3DCompileFromFile(LPCWSTR path, const D3D_SHADER_MACRO* defines, ID3DInclude* includes,
        LPCSTR entry, LPCSTR target, UINT flags, UINT effectFlags, ID3DBlob** code, ID3DBlob** errors)
    {
        if (code) *code = nullptr;
        if (errors) *errors = nullptr;
        const auto storage = CurrentGuestStorageContext();
        if (!storage || !path || !code) return E_INVALIDARG;
        try
        {
            std::wstring canonical;
            std::vector<BYTE> source;
            DWORD error = ERROR_SUCCESS;
            if (!storage->CanonicalPath(path, &canonical, &error) ||
                !storage->ReadAllBytes(canonical.c_str(), &source, &error))
                return HRESULT_FROM_WIN32(error ? error : ERROR_FILE_NOT_FOUND);
            GuestShaderIncludes guest(storage, std::move(canonical));
            const BYTE empty = 0;
            return GuestD3DCompile(source.empty() ? &empty : source.data(), source.size(), nullptr, defines,
                includes == D3D_COMPILE_STANDARD_FILE_INCLUDE ? &guest : includes, entry, target, flags, effectFlags, code, errors);
        }
        catch (const std::bad_alloc&) { return E_OUTOFMEMORY; }
        catch (Platform::Exception^ error) { return error->HResult; }
    }

    HRESULT PresentationError(const wchar_t* operation, HRESULT status)
    {
        try
        {
            RuntimeDiagnostics::Record(std::wstring(L"DXGI: ") + operation +
                L" failed; HRESULT " + std::to_wstring(static_cast<unsigned long>(status)) + L".");
        }
        catch (...) {}
        return status;
    }

    class GuestSwapChain final : public IDXGISwapChain4
    {
    public:
        GuestSwapChain(ComPtr<IDXGISwapChain1> native, IDXGIFactory2* factory, IUnknown* device, HWND window,
            GuestSwapChainBinding binding)
            : m_native(std::move(native)), m_factory(factory), m_window(window), m_binding(std::move(binding))
        {
            if (!device) return;
            ComPtr<ID3D12CommandQueue> queue;
            if (SUCCEEDED(device->QueryInterface(IID_PPV_ARGS(&queue))))
            {
                // DXGI exposes the queue's device from GetDevice.
                ComPtr<ID3D12Device> renderDevice;
                if (SUCCEEDED(queue->GetDevice(IID_PPV_ARGS(&renderDevice)))) renderDevice.As(&m_guestDevice);
            }
            else device->QueryInterface(IID_PPV_ARGS(&m_guestDevice));
        }

        HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void** result) override
        {
            if (!result) return E_POINTER;
            *result = nullptr;
            if (iid != __uuidof(IUnknown) && iid != __uuidof(IDXGIObject) && iid != __uuidof(IDXGIDeviceSubObject) &&
                iid != __uuidof(IDXGISwapChain) && iid != __uuidof(IDXGISwapChain1) &&
                iid != __uuidof(IDXGISwapChain2) && iid != __uuidof(IDXGISwapChain3) && iid != __uuidof(IDXGISwapChain4))
                return E_NOINTERFACE;
            ComPtr<IUnknown> supported;
            const HRESULT status = m_native->QueryInterface(iid, reinterpret_cast<void**>(supported.GetAddressOf()));
            if (FAILED(status)) return status;
            *result = static_cast<IDXGISwapChain4*>(this);
            AddRef();
            return S_OK;
        }

        ULONG STDMETHODCALLTYPE AddRef() override { return ++m_refs; }
        ULONG STDMETHODCALLTYPE Release() override
        {
            const ULONG refs = --m_refs;
            if (!refs) delete this;
            return refs;
        }

        HRESULT STDMETHODCALLTYPE GetParent(REFIID iid, void** parent) override
        { return m_factory->QueryInterface(iid, parent); }

        HRESULT STDMETHODCALLTYPE GetHwnd(HWND* window) override
        {
            if (!m_window) return m_native->GetHwnd(window);
            if (!window) return E_POINTER;
            *window = m_window;
            return S_OK;
        }

        HRESULT STDMETHODCALLTYPE SetFullscreenState(BOOL fullscreen, IDXGIOutput* output) override
        {
            if (!m_window) return m_native->SetFullscreenState(fullscreen, output);
            return fullscreen ? DXGI_ERROR_NOT_CURRENTLY_AVAILABLE : S_OK;
        }

        HRESULT STDMETHODCALLTYPE GetFullscreenState(BOOL* fullscreen, IDXGIOutput** output) override
        {
            if (!m_window) return m_native->GetFullscreenState(fullscreen, output);
            if (fullscreen) *fullscreen = FALSE;
            if (output) *output = nullptr;
            return S_OK;
        }

        HRESULT STDMETHODCALLTYPE GetDesc(DXGI_SWAP_CHAIN_DESC* description) override
        {
            if (!description) return E_POINTER;
            const HRESULT status = m_native->GetDesc(description);
            if (SUCCEEDED(status) && m_window)
            {
                description->OutputWindow = m_window;
                description->Windowed = TRUE;
            }
            return status;
        }

        HRESULT STDMETHODCALLTYPE GetFullscreenDesc(DXGI_SWAP_CHAIN_FULLSCREEN_DESC* description) override
        {
            if (!m_window) return m_native->GetFullscreenDesc(description);
            if (!description) return E_POINTER;
            *description = {};
            description->Windowed = TRUE;
            return S_OK;
        }

        HRESULT STDMETHODCALLTYPE ResizeTarget(const DXGI_MODE_DESC* mode) override
        { return m_window ? DXGI_ERROR_UNSUPPORTED : m_native->ResizeTarget(mode); }

        HRESULT STDMETHODCALLTYPE ResizeBuffers(UINT count, UINT width, UINT height, DXGI_FORMAT format, UINT flags) override
        {
            if (!m_window) return m_native->ResizeBuffers(count, width, height, format, flags);
            const HRESULT status = ResolveSize(width, height);
            if (FAILED(status)) return status;
            return m_native->ResizeBuffers(count == 1 ? 2 : count, width, height, format,
                flags & ~DXGI_SWAP_CHAIN_FLAG_ALLOW_MODE_SWITCH);
        }

        HRESULT STDMETHODCALLTYPE ResizeBuffers1(UINT count, UINT width, UINT height, DXGI_FORMAT format, UINT flags,
            const UINT* nodeMasks, IUnknown* const* queues) override
        {
            ComPtr<IDXGISwapChain3> native;
            HRESULT status = m_native.As(&native);
            if (FAILED(status)) return status;
            status = ResolveSize(width, height);
            if (FAILED(status)) return status;
            // Node masks and queues must still have one element per buffer.
            if (m_window && count == 1) return DXGI_ERROR_INVALID_CALL;
            if (m_window) flags &= ~DXGI_SWAP_CHAIN_FLAG_ALLOW_MODE_SWITCH;
            UINT queueCount = count;
            if (queues && !queueCount)
            {
                DXGI_SWAP_CHAIN_DESC1 description{};
                status = native->GetDesc1(&description);
                if (FAILED(status)) return status;
                queueCount = description.BufferCount;
            }
            return WithNativeGraphicsObjects(queueCount, queues, [&](IUnknown* const* nativeQueues)
            {
                return native->ResizeBuffers1(count, width, height, format, flags, nodeMasks, nativeQueues);
            });
        }

        HANDLE STDMETHODCALLTYPE GetFrameLatencyWaitableObject() override
        {
            const auto kernel = CurrentGuestKernelContext();
            ComPtr<IDXGISwapChain2> native;
            if (!kernel || FAILED(m_native.As(&native))) return nullptr;
            std::lock_guard<std::mutex> lock(m_latencyLock);
            return kernel->PublishNativeWaitHandle(m_latencyWait, native->GetFrameLatencyWaitableObject());
        }

        HRESULT STDMETHODCALLTYPE SetPrivateData(REFGUID Name, UINT DataSize, const void* pData) override
        { return m_native->SetPrivateData(Name, DataSize, pData); }

        HRESULT STDMETHODCALLTYPE SetPrivateDataInterface(REFGUID Name, const IUnknown* pUnknown) override
        { return m_native->SetPrivateDataInterface(Name, pUnknown); }

        HRESULT STDMETHODCALLTYPE GetPrivateData(REFGUID Name, UINT* pDataSize, void* pData) override
        { return m_native->GetPrivateData(Name, pDataSize, pData); }

        HRESULT STDMETHODCALLTYPE GetDevice(REFIID riid, void** ppDevice) override
        { return m_guestDevice ? m_guestDevice->QueryInterface(riid, ppDevice) : m_native->GetDevice(riid, ppDevice); }

        HRESULT STDMETHODCALLTYPE Present(UINT SyncInterval, UINT Flags) override
        { return m_native->Present(SyncInterval, Flags); }

        HRESULT STDMETHODCALLTYPE GetBuffer(UINT Buffer, REFIID riid, void** ppSurface) override
        {
            ComPtr<ID3D11Device> device;
            if (!ppSurface || !m_guestDevice || FAILED(m_guestDevice.As(&device)))
                return m_native->GetBuffer(Buffer, riid, ppSurface);
            *ppSurface = nullptr;
            ComPtr<IUnknown> result;
            const HRESULT status = m_native->GetBuffer(Buffer, riid, reinterpret_cast<void**>(result.GetAddressOf()));
            if (FAILED(status) || !result) return status;
            ComPtr<ID3D11Texture2D> texture;
            if (SUCCEEDED(result.As(&texture)))
                return WrapD3D11Texture(std::move(texture), device.Get(), riid, ppSurface);
            *ppSurface = result.Detach();
            return status;
        }

        HRESULT STDMETHODCALLTYPE GetContainingOutput(IDXGIOutput** ppOutput) override
        { return m_native->GetContainingOutput(ppOutput); }

        HRESULT STDMETHODCALLTYPE GetFrameStatistics(DXGI_FRAME_STATISTICS* pStats) override
        { return m_native->GetFrameStatistics(pStats); }

        HRESULT STDMETHODCALLTYPE GetLastPresentCount(UINT* pLastPresentCount) override
        { return m_native->GetLastPresentCount(pLastPresentCount); }

        HRESULT STDMETHODCALLTYPE GetDesc1(DXGI_SWAP_CHAIN_DESC1* pDesc) override
        { return m_native->GetDesc1(pDesc); }

        HRESULT STDMETHODCALLTYPE GetCoreWindow(REFIID refiid, void** ppUnk) override
        { return m_native->GetCoreWindow(refiid, ppUnk); }

        HRESULT STDMETHODCALLTYPE Present1(UINT SyncInterval, UINT PresentFlags,
            const DXGI_PRESENT_PARAMETERS* pPresentParameters) override
        { return m_native->Present1(SyncInterval, PresentFlags, pPresentParameters); }

        BOOL STDMETHODCALLTYPE IsTemporaryMonoSupported() override
        { return m_native->IsTemporaryMonoSupported(); }

        HRESULT STDMETHODCALLTYPE GetRestrictToOutput(IDXGIOutput** ppRestrictToOutput) override
        { return m_native->GetRestrictToOutput(ppRestrictToOutput); }

        HRESULT STDMETHODCALLTYPE SetBackgroundColor(const DXGI_RGBA* pColor) override
        { return m_native->SetBackgroundColor(pColor); }

        HRESULT STDMETHODCALLTYPE GetBackgroundColor(DXGI_RGBA* pColor) override
        { return m_native->GetBackgroundColor(pColor); }

        HRESULT STDMETHODCALLTYPE SetRotation(DXGI_MODE_ROTATION Rotation) override
        { return m_native->SetRotation(Rotation); }

        HRESULT STDMETHODCALLTYPE GetRotation(DXGI_MODE_ROTATION* pRotation) override
        { return m_native->GetRotation(pRotation); }

        HRESULT STDMETHODCALLTYPE SetSourceSize(UINT Width, UINT Height) override
        {
            ComPtr<IDXGISwapChain2> native;
            const HRESULT status = m_native.As(&native);
            return FAILED(status) ? status : native->SetSourceSize(Width, Height);
        }

        HRESULT STDMETHODCALLTYPE GetSourceSize(UINT* pWidth, UINT* pHeight) override
        {
            ComPtr<IDXGISwapChain2> native;
            const HRESULT status = m_native.As(&native);
            return FAILED(status) ? status : native->GetSourceSize(pWidth, pHeight);
        }

        HRESULT STDMETHODCALLTYPE SetMaximumFrameLatency(UINT MaxLatency) override
        {
            ComPtr<IDXGISwapChain2> native;
            const HRESULT status = m_native.As(&native);
            return FAILED(status) ? status : native->SetMaximumFrameLatency(MaxLatency);
        }

        HRESULT STDMETHODCALLTYPE GetMaximumFrameLatency(UINT* pMaxLatency) override
        {
            ComPtr<IDXGISwapChain2> native;
            const HRESULT status = m_native.As(&native);
            return FAILED(status) ? status : native->GetMaximumFrameLatency(pMaxLatency);
        }

        HRESULT STDMETHODCALLTYPE SetMatrixTransform(const DXGI_MATRIX_3X2_F* pMatrix) override
        {
            ComPtr<IDXGISwapChain2> native;
            const HRESULT status = m_native.As(&native);
            return FAILED(status) ? status : native->SetMatrixTransform(pMatrix);
        }

        HRESULT STDMETHODCALLTYPE GetMatrixTransform(DXGI_MATRIX_3X2_F* pMatrix) override
        {
            ComPtr<IDXGISwapChain2> native;
            const HRESULT status = m_native.As(&native);
            return FAILED(status) ? status : native->GetMatrixTransform(pMatrix);
        }

        UINT STDMETHODCALLTYPE GetCurrentBackBufferIndex() override
        {
            ComPtr<IDXGISwapChain3> native;
            const HRESULT status = m_native.As(&native);
            return FAILED(status) ? 0 : native->GetCurrentBackBufferIndex();
        }

        HRESULT STDMETHODCALLTYPE CheckColorSpaceSupport(DXGI_COLOR_SPACE_TYPE ColorSpace, UINT* pColorSpaceSupport) override
        {
            ComPtr<IDXGISwapChain3> native;
            const HRESULT status = m_native.As(&native);
            return FAILED(status) ? status : native->CheckColorSpaceSupport(ColorSpace, pColorSpaceSupport);
        }

        HRESULT STDMETHODCALLTYPE SetColorSpace1(DXGI_COLOR_SPACE_TYPE ColorSpace) override
        {
            ComPtr<IDXGISwapChain3> native;
            const HRESULT status = m_native.As(&native);
            return FAILED(status) ? status : native->SetColorSpace1(ColorSpace);
        }

        HRESULT STDMETHODCALLTYPE SetHDRMetaData(DXGI_HDR_METADATA_TYPE Type, UINT Size, void* pMetaData) override
        {
            ComPtr<IDXGISwapChain4> native;
            const HRESULT status = m_native.As(&native);
            return FAILED(status) ? status : native->SetHDRMetaData(Type, Size, pMetaData);
        }

    private:
        ~GuestSwapChain() { if (m_binding.detach) m_binding.detach(); }

        HRESULT ResolveSize(UINT& width, UINT& height)
        {
            if (!m_window) return S_OK;
            UINT clientWidth = 0, clientHeight = 0;
            const HRESULT status = m_binding.clientSize(&clientWidth, &clientHeight);
            if (SUCCEEDED(status))
            {
                if (!width) width = clientWidth;
                if (!height) height = clientHeight;
            }
            return status;
        }

        std::atomic<ULONG> m_refs{1};
        ComPtr<IDXGISwapChain1> m_native;
        ComPtr<IDXGIFactory2> m_factory;
        ComPtr<GuestGraphicsObject> m_guestDevice;
        HWND m_window;
        GuestSwapChainBinding m_binding;
        std::mutex m_latencyLock;
        GuestKernelContext::NativeWaitHandle m_latencyWait;
    };

    class GuestDxgiFactory final : public IDXGIFactory7
    {
    public:
        explicit GuestDxgiFactory(ComPtr<IDXGIFactory2> native) : m_native(std::move(native)) {}

        HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void** result) override
        {
            if (!result) return E_POINTER;
            *result = nullptr;
            if (iid != __uuidof(IUnknown) && iid != __uuidof(IDXGIObject) && iid != __uuidof(IDXGIFactory) &&
                iid != __uuidof(IDXGIFactory1) && iid != __uuidof(IDXGIFactory2) && iid != __uuidof(IDXGIFactory3) &&
                iid != __uuidof(IDXGIFactory4) && iid != __uuidof(IDXGIFactory5) && iid != __uuidof(IDXGIFactory6) &&
                iid != __uuidof(IDXGIFactory7)) return E_NOINTERFACE;
            ComPtr<IUnknown> supported;
            const HRESULT status = m_native->QueryInterface(iid, reinterpret_cast<void**>(supported.GetAddressOf()));
            if (FAILED(status)) return status;
            *result = static_cast<IDXGIFactory7*>(this);
            AddRef();
            return S_OK;
        }

        ULONG STDMETHODCALLTYPE AddRef() override { return ++m_refs; }
        ULONG STDMETHODCALLTYPE Release() override
        {
            const ULONG refs = --m_refs;
            if (!refs) delete this;
            return refs;
        }

        HRESULT STDMETHODCALLTYPE MakeWindowAssociation(HWND window, UINT flags) override
        {
            if (flags & ~DXGI_MWA_VALID) return DXGI_ERROR_INVALID_CALL;
            const auto manager = CurrentGuestWindowManager();
            if (window && (!manager || !manager->IsGuestWindow(window))) return DXGI_ERROR_INVALID_CALL;
            m_windowAssociation.store(window);
            return S_OK;
        }

        HRESULT STDMETHODCALLTYPE GetWindowAssociation(HWND* window) override
        {
            if (!window) return E_POINTER;
            *window = m_windowAssociation.load();
            return S_OK;
        }

        HRESULT STDMETHODCALLTYPE CreateSwapChain(IUnknown* device, DXGI_SWAP_CHAIN_DESC* description,
            IDXGISwapChain** output) override
        {
            if (!output) return E_POINTER;
            *output = nullptr;
            if (!description) return DXGI_ERROR_INVALID_CALL;
            DXGI_SWAP_CHAIN_DESC1 modern{};
            modern.Width = description->BufferDesc.Width;
            modern.Height = description->BufferDesc.Height;
            modern.Format = description->BufferDesc.Format;
            modern.SampleDesc = description->SampleDesc;
            modern.BufferUsage = description->BufferUsage;
            modern.BufferCount = description->BufferCount;
            modern.Flags = description->Flags;
            ComPtr<IDXGISwapChain1> chain;
            const HRESULT status = CreateSwapChainForHwnd(device, description->OutputWindow, &modern, nullptr,
                nullptr, chain.GetAddressOf());
            if (SUCCEEDED(status)) *output = chain.Detach();
            return status;
        }

        HRESULT STDMETHODCALLTYPE CreateSwapChainForHwnd(IUnknown* device, HWND window,
            const DXGI_SWAP_CHAIN_DESC1* description, const DXGI_SWAP_CHAIN_FULLSCREEN_DESC*,
            IDXGIOutput* restrictTo, IDXGISwapChain1** output) override
        {
            if (!output) return E_POINTER;
            *output = nullptr;
            if (!device || !window || !description) return DXGI_ERROR_INVALID_CALL;
            auto manager = CurrentGuestWindowManager();
            RECT client{};
            if (!manager || !manager->GetGuestClientRect(window, &client, nullptr)) return DXGI_ERROR_INVALID_CALL;

            auto composition = *description;
            if (composition.Stereo || composition.SampleDesc.Count != 1 || composition.SampleDesc.Quality ||
                (composition.Flags & DXGI_SWAP_CHAIN_FLAG_GDI_COMPATIBLE))
                return DXGI_ERROR_UNSUPPORTED;
            if (!composition.Width) composition.Width = static_cast<UINT>((std::max)(1L, client.right));
            if (!composition.Height) composition.Height = static_cast<UINT>((std::max)(1L, client.bottom));
            if (composition.Format == DXGI_FORMAT_UNKNOWN) composition.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
            composition.BufferCount = (std::max)(2u, composition.BufferCount);
            composition.Flags &= ~DXGI_SWAP_CHAIN_FLAG_ALLOW_MODE_SWITCH;
            composition.Scaling = DXGI_SCALING_STRETCH;
            composition.SwapEffect = DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL;
            if (composition.AlphaMode == DXGI_ALPHA_MODE_UNSPECIFIED) composition.AlphaMode = DXGI_ALPHA_MODE_IGNORE;

            // DXGI expects an ID3D11Device or a direct ID3D12CommandQueue here.
            ComPtr<IDXGISwapChain1> native;
            const auto nativeDevice = NativeGraphicsObject(device);
            HRESULT status = m_native->CreateSwapChainForComposition(nativeDevice.Get(), &composition, restrictTo, native.GetAddressOf());
            if (FAILED(status)) return PresentationError(L"CreateSwapChainForComposition", status);
            GuestSwapChainBinding binding;
            status = manager->BindGuestSwapChain(window, native.Get(), &binding);
            if (FAILED(status)) return PresentationError(L"SwapChainPanel attachment", status);
            auto chain = new (std::nothrow) GuestSwapChain(std::move(native), this, device, window, std::move(binding));
            if (!chain)
            {
                binding.detach();
                return E_OUTOFMEMORY;
            }
            *output = chain;
            return S_OK;
        }

        HRESULT STDMETHODCALLTYPE SetPrivateData(REFGUID Name, UINT DataSize, const void* pData) override
        { return m_native->SetPrivateData(Name, DataSize, pData); }

        HRESULT STDMETHODCALLTYPE SetPrivateDataInterface(REFGUID Name, const IUnknown* pUnknown) override
        { return m_native->SetPrivateDataInterface(Name, pUnknown); }

        HRESULT STDMETHODCALLTYPE GetPrivateData(REFGUID Name, UINT* pDataSize, void* pData) override
        { return m_native->GetPrivateData(Name, pDataSize, pData); }

        HRESULT STDMETHODCALLTYPE GetParent(REFIID riid, void** ppParent) override
        { return m_native->GetParent(riid, ppParent); }

        HRESULT STDMETHODCALLTYPE EnumAdapters(UINT Adapter, IDXGIAdapter** ppAdapter) override
        {
            if (!ppAdapter) return E_POINTER;
            *ppAdapter = nullptr;
            ComPtr<IDXGIAdapter> native;
            const HRESULT status = m_native->EnumAdapters(Adapter, &native);
            return FAILED(status) ? status : WrapAdapter(std::move(native), IID_PPV_ARGS(ppAdapter), this);
        }

        HRESULT STDMETHODCALLTYPE CreateSoftwareAdapter(HMODULE, IDXGIAdapter**) override
        { return DXGI_ERROR_UNSUPPORTED; }

        HRESULT STDMETHODCALLTYPE EnumAdapters1(UINT Adapter, IDXGIAdapter1** ppAdapter) override
        {
            if (!ppAdapter) return E_POINTER;
            *ppAdapter = nullptr;
            ComPtr<IDXGIAdapter1> native;
            const HRESULT status = m_native->EnumAdapters1(Adapter, &native);
            return FAILED(status) ? status : WrapAdapter(std::move(native), IID_PPV_ARGS(ppAdapter), this);
        }

        BOOL STDMETHODCALLTYPE IsCurrent() override
        { return m_native->IsCurrent(); }

        BOOL STDMETHODCALLTYPE IsWindowedStereoEnabled() override
        { return m_native->IsWindowedStereoEnabled(); }

        HRESULT STDMETHODCALLTYPE CreateSwapChainForCoreWindow(IUnknown*, IUnknown*, const DXGI_SWAP_CHAIN_DESC1*,
            IDXGIOutput*, IDXGISwapChain1**) override
        { return DXGI_ERROR_UNSUPPORTED; }

        HRESULT STDMETHODCALLTYPE GetSharedResourceAdapterLuid(HANDLE resource, LUID* luid) override
        {
            const auto kernel = CurrentGuestKernelContext();
            const auto handle = kernel ? kernel->LeaseGraphicsHandle(resource) : std::shared_ptr<void>{};
            return handle ? m_native->GetSharedResourceAdapterLuid(handle.get(), luid) : E_HANDLE;
        }

        HRESULT STDMETHODCALLTYPE RegisterStereoStatusWindow(HWND, UINT, DWORD*) override
        { return DXGI_ERROR_UNSUPPORTED; }

        HRESULT STDMETHODCALLTYPE RegisterStereoStatusEvent(HANDLE event, DWORD* cookie) override
        {
            return m_stereoEvents.Add(m_native, event, cookie,
                &IDXGIFactory2::RegisterStereoStatusEvent, &IDXGIFactory2::UnregisterStereoStatus);
        }

        void STDMETHODCALLTYPE UnregisterStereoStatus(DWORD cookie) override
        { m_stereoEvents.Remove(cookie); }

        HRESULT STDMETHODCALLTYPE RegisterOcclusionStatusWindow(HWND, UINT, DWORD*) override
        { return DXGI_ERROR_UNSUPPORTED; }

        HRESULT STDMETHODCALLTYPE RegisterOcclusionStatusEvent(HANDLE event, DWORD* cookie) override
        {
            return m_occlusionEvents.Add(m_native, event, cookie,
                &IDXGIFactory2::RegisterOcclusionStatusEvent, &IDXGIFactory2::UnregisterOcclusionStatus);
        }

        void STDMETHODCALLTYPE UnregisterOcclusionStatus(DWORD cookie) override
        { m_occlusionEvents.Remove(cookie); }

        HRESULT STDMETHODCALLTYPE CreateSwapChainForComposition(IUnknown* pDevice, const DXGI_SWAP_CHAIN_DESC1* pDesc,
            IDXGIOutput* pRestrictToOutput, IDXGISwapChain1** ppSwapChain) override
        {
            if (!ppSwapChain) return E_POINTER;
            *ppSwapChain = nullptr;
            const auto device = NativeGraphicsObject(pDevice);
            ComPtr<IDXGISwapChain1> native;
            const HRESULT status = m_native->CreateSwapChainForComposition(device.Get(), pDesc, pRestrictToOutput, &native);
            if (FAILED(status)) return status;
            auto chain = new (std::nothrow) GuestSwapChain(std::move(native), this, pDevice, nullptr, {});
            if (!chain) return E_OUTOFMEMORY;
            *ppSwapChain = chain;
            return status;
        }

        UINT STDMETHODCALLTYPE GetCreationFlags() override
        {
            ComPtr<IDXGIFactory3> native;
            const HRESULT status = m_native.As(&native);
            return FAILED(status) ? 0 : native->GetCreationFlags();
        }

        HRESULT STDMETHODCALLTYPE EnumAdapterByLuid(LUID AdapterLuid, REFIID riid, void** ppvAdapter) override
        {
            ComPtr<IDXGIFactory4> native;
            if (!ppvAdapter) return E_POINTER;
            *ppvAdapter = nullptr;
            HRESULT status = m_native.As(&native);
            if (FAILED(status)) return status;
            ComPtr<IDXGIAdapter> adapter;
            status = native->EnumAdapterByLuid(AdapterLuid, IID_PPV_ARGS(&adapter));
            return FAILED(status) ? status : WrapAdapter(std::move(adapter), riid, ppvAdapter, this);
        }

        HRESULT STDMETHODCALLTYPE EnumWarpAdapter(REFIID riid, void** ppvAdapter) override
        {
            ComPtr<IDXGIFactory4> native;
            if (!ppvAdapter) return E_POINTER;
            *ppvAdapter = nullptr;
            HRESULT status = m_native.As(&native);
            if (FAILED(status)) return status;
            ComPtr<IDXGIAdapter> adapter;
            status = native->EnumWarpAdapter(IID_PPV_ARGS(&adapter));
            return FAILED(status) ? status : WrapAdapter(std::move(adapter), riid, ppvAdapter, this);
        }

        HRESULT STDMETHODCALLTYPE CheckFeatureSupport(DXGI_FEATURE Feature, void* pFeatureSupportData,
            UINT FeatureSupportDataSize) override
        {
            ComPtr<IDXGIFactory5> native;
            const HRESULT status = m_native.As(&native);
            return FAILED(status) ? status : native->CheckFeatureSupport(Feature, pFeatureSupportData, FeatureSupportDataSize);
        }

        HRESULT STDMETHODCALLTYPE EnumAdapterByGpuPreference(UINT Adapter, DXGI_GPU_PREFERENCE GpuPreference,
            REFIID riid, void** ppvAdapter) override
        {
            ComPtr<IDXGIFactory6> native;
            if (!ppvAdapter) return E_POINTER;
            *ppvAdapter = nullptr;
            HRESULT status = m_native.As(&native);
            if (FAILED(status)) return status;
            ComPtr<IDXGIAdapter> adapter;
            status = native->EnumAdapterByGpuPreference(Adapter, GpuPreference, IID_PPV_ARGS(&adapter));
            return FAILED(status) ? status : WrapAdapter(std::move(adapter), riid, ppvAdapter, this);
        }

        HRESULT STDMETHODCALLTYPE RegisterAdaptersChangedEvent(HANDLE event, DWORD* cookie) override
        {
            ComPtr<IDXGIFactory7> native;
            const HRESULT status = m_native.As(&native);
            return FAILED(status) ? status : m_adapterEvents.Add(native, event, cookie,
                &IDXGIFactory7::RegisterAdaptersChangedEvent, &IDXGIFactory7::UnregisterAdaptersChangedEvent);
        }

        HRESULT STDMETHODCALLTYPE UnregisterAdaptersChangedEvent(DWORD cookie) override
        { return m_adapterEvents.Remove(cookie); }

    private:
        std::atomic<ULONG> m_refs{1};
        ComPtr<IDXGIFactory2> m_native;
        GraphicsEventNotifications m_stereoEvents;
        GraphicsEventNotifications m_occlusionEvents;
        GraphicsEventNotifications m_adapterEvents;
        std::atomic<HWND> m_windowAssociation{nullptr};
    };

    HRESULT WrapFactory(ComPtr<IDXGIFactory2> native, REFIID iid, void** output)
    {
        auto factory = new (std::nothrow) GuestDxgiFactory(std::move(native));
        if (!factory) return E_OUTOFMEMORY;
        const HRESULT status = factory->QueryInterface(iid, output);
        factory->Release();
        return status;
    }

#include "Bridge/DirectX/GraphicsD3D11Texture.inl"
#include "Bridge/DirectX/GraphicsD3D11Context.inl"
#include "Bridge/DirectX/GraphicsDevice.inl"
#include "Bridge/DirectX/GraphicsD3D12.inl"
#include "Bridge/DirectX/GraphicsD3D12Factory.inl"

    HRESULT FinishD3D11Creation(ComPtr<ID3D11Device> native, ComPtr<ID3D11DeviceContext> immediate,
        IDXGIAdapter* adapter, ID3D11Device** device, ID3D11DeviceContext** context)
    {
        if (!native) return immediate ? E_UNEXPECTED : S_OK;
        ComPtr<ID3D11Device> guest;
        HRESULT status = WrapD3D11Device(std::move(native), adapter, &guest);
        if (FAILED(status)) return status;
        if (context)
        {
            status = WrapD3D11Context(std::move(immediate), guest.Get(), context);
            if (FAILED(status)) return status;
        }
        if (device) *device = guest.Detach();
        return S_OK;
    }

    HRESULT WINAPI GuestCreateDXGIFactory1(REFIID iid, void** output)
    {
        if (!output) return E_POINTER;
        *output = nullptr;
        ComPtr<IDXGIFactory2> native;
        const HRESULT status = ::CreateDXGIFactory1(IID_PPV_ARGS(&native));
        return FAILED(status) ? status : WrapFactory(std::move(native), iid, output);
    }

    HRESULT WINAPI GuestCreateDXGIFactory2(UINT flags, REFIID iid, void** output)
    {
        if (!output) return E_POINTER;
        *output = nullptr;
        ComPtr<IDXGIFactory2> native;
        const HRESULT status = ::CreateDXGIFactory2(flags, IID_PPV_ARGS(&native));
        return FAILED(status) ? status : WrapFactory(std::move(native), iid, output);
    }

    HRESULT WINAPI GuestD3D11CreateDevice(IDXGIAdapter* adapter, D3D_DRIVER_TYPE driver,
        HMODULE software, UINT flags, const D3D_FEATURE_LEVEL* levels, UINT count, UINT sdk,
        ID3D11Device** device, D3D_FEATURE_LEVEL* chosenLevel, ID3D11DeviceContext** context)
    {
        if (device) *device = nullptr;
        if (context) *context = nullptr;
        // Guest HMODULEs identify bridge modules and cannot select a native rasterizer.
        if (software || driver == D3D_DRIVER_TYPE_SOFTWARE)
            return DXGI_ERROR_UNSUPPORTED;
        ComPtr<IDXGIAdapter> nativeAdapter;
        if (adapter)
        {
            const HRESULT status = NativeGraphicsObject(adapter).As(&nativeAdapter);
            if (FAILED(status)) return status;
        }

        ComPtr<ID3D11Device> created;
        ComPtr<ID3D11DeviceContext> immediate;
        const HRESULT status = ::D3D11CreateDevice(nativeAdapter.Get(), driver, nullptr, flags, levels, count, sdk,
            device || context ? created.GetAddressOf() : nullptr, chosenLevel, context ? immediate.GetAddressOf() : nullptr);
        if (FAILED(status)) return status;
        const HRESULT wrapped = FinishD3D11Creation(std::move(created), std::move(immediate), adapter, device, context);
        return FAILED(wrapped) ? wrapped : status;
    }

    HRESULT WINAPI GuestD3D11CreateDeviceAndSwapChain(IDXGIAdapter* adapter, D3D_DRIVER_TYPE driver,
        HMODULE software, UINT flags, const D3D_FEATURE_LEVEL* levels, UINT count, UINT sdk,
        const DXGI_SWAP_CHAIN_DESC* description, IDXGISwapChain** swapChain, ID3D11Device** device,
        D3D_FEATURE_LEVEL* chosenLevel, ID3D11DeviceContext** context)
    {
        if (!swapChain)
            return GuestD3D11CreateDevice(adapter, driver, software, flags, levels, count, sdk, device, chosenLevel, context);
        *swapChain = nullptr;
        if (device) *device = nullptr;
        if (context) *context = nullptr;
        if (!description) return E_INVALIDARG;

        ComPtr<ID3D11Device> created;
        ComPtr<ID3D11DeviceContext> immediate;
        HRESULT result = GuestD3D11CreateDevice(adapter, driver, software, flags, levels, count, sdk,
            created.GetAddressOf(), chosenLevel, context ? immediate.GetAddressOf() : nullptr);
        if (FAILED(result)) return result;
        ComPtr<IDXGIDevice> dxgiDevice;
        result = created.As(&dxgiDevice);
        if (FAILED(result)) return result;
        ComPtr<IDXGIAdapter> actualAdapter;
        result = dxgiDevice->GetAdapter(&actualAdapter);
        if (FAILED(result)) return result;
        ComPtr<IDXGIFactory> factory;
        result = actualAdapter->GetParent(IID_PPV_ARGS(&factory));
        if (FAILED(result)) return result;

        auto copy = *description;
        result = factory->CreateSwapChain(created.Get(), &copy, swapChain);
        if (FAILED(result)) return result;
        if (device) *device = created.Detach();
        if (context) *context = immediate.Detach();
        return result;
    }

    HRESULT WINAPI GuestD3D12CreateDevice(IUnknown* adapter, D3D_FEATURE_LEVEL level, REFIID iid, void** device)
    {
        const auto kernel = CurrentGuestKernelContext();
        const auto selected = kernel ? kernel->D3D12Factory() : std::shared_ptr<IUnknown>{};
        if (selected)
        {
            return CreateD3D12GuestDevice(adapter, level, iid, device, [&](IUnknown* native,
                D3D_FEATURE_LEVEL feature, REFIID requested, void** output)
            {
                return static_cast<ID3D12DeviceFactory*>(selected.get())->CreateDevice(native, feature, requested, output);
            });
        }
        const HRESULT version = CheckGuestD3D12SdkVersion();
        if (FAILED(version))
        {
            if (device) *device = nullptr;
            return version;
        }
        const auto create = reinterpret_cast<PFN_D3D12_CREATE_DEVICE>(FindD3D12Procedure("D3D12CreateDevice"));
        if (!create) return DXGI_ERROR_UNSUPPORTED;
        return CreateD3D12GuestDevice(adapter, level, iid, device, create);
    }

    FARPROC FindD3D11On12CreateDevice()
    {
        static const HMODULE module = ::LoadLibraryExW(L"d3d11.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
        return module ? ::GetProcAddress(module, "D3D11On12CreateDevice") : nullptr;
    }

    HRESULT WINAPI GuestD3D11On12CreateDevice(IUnknown* device12, UINT flags, const D3D_FEATURE_LEVEL* levels,
        UINT levelCount, IUnknown* const* queues, UINT queueCount, UINT nodeMask, ID3D11Device** device,
        ID3D11DeviceContext** context, D3D_FEATURE_LEVEL* chosenLevel)
    {
        if (device) *device = nullptr;
        if (context) *context = nullptr;
        const auto create = reinterpret_cast<PFN_D3D11ON12_CREATE_DEVICE>(FindD3D11On12CreateDevice());
        if (!create) return DXGI_ERROR_UNSUPPORTED;
        const auto nativeDevice = NativeGraphicsObject(device12);
        return WithNativeGraphicsObjects(queueCount, queues, [&](IUnknown* const* nativeQueues)
        {
            ComPtr<ID3D11Device> created;
            ComPtr<ID3D11DeviceContext> immediate;
            const HRESULT status = create(nativeDevice.Get(), flags, levels, levelCount, nativeQueues, queueCount,
                nodeMask, device || context ? created.GetAddressOf() : nullptr, context ? immediate.GetAddressOf() : nullptr, chosenLevel);
            if (FAILED(status)) return status;
            const HRESULT wrapped = FinishD3D11Creation(std::move(created), std::move(immediate), nullptr, device, context);
            return FAILED(wrapped) ? wrapped : status;
        });
    }
}

bool Win32Bridge::Bridge::HasHostShaderCompiler()
{
    return FindShaderCompilerProcedure("D3DCompile") != nullptr;
}

ImportResolution Win32Bridge::Bridge::ResolveGraphicsImport(const ImportedSymbol& symbol)
{
    const bool d3d12 = _wcsicmp(symbol.library.c_str(), L"d3d12.dll") == 0;
    const bool createDeviceOrdinal = symbol.importedByOrdinal && d3d12 && symbol.ordinal == 101;
    if (symbol.importedByOrdinal && !createDeviceOrdinal) return {};
    ULONGLONG address = 0;
    if (_wcsicmp(symbol.library.c_str(), L"dxgi.dll") == 0)
    {
        if (symbol.name == L"CreateDXGIFactory" || symbol.name == L"CreateDXGIFactory1")
            address = reinterpret_cast<ULONGLONG>(&GuestCreateDXGIFactory1);
        else if (symbol.name == L"CreateDXGIFactory2") address = reinterpret_cast<ULONGLONG>(&GuestCreateDXGIFactory2);
    }
    else if (_wcsicmp(symbol.library.c_str(), L"d3d11.dll") == 0)
    {
        if (symbol.name == L"D3D11CreateDevice") address = reinterpret_cast<ULONGLONG>(&GuestD3D11CreateDevice);
        else if (symbol.name == L"D3D11CreateDeviceAndSwapChain")
            address = reinterpret_cast<ULONGLONG>(&GuestD3D11CreateDeviceAndSwapChain);
        else if (symbol.name == L"D3D11On12CreateDevice" && FindD3D11On12CreateDevice())
            address = reinterpret_cast<ULONGLONG>(&GuestD3D11On12CreateDevice);
    }
    else if (_wcsicmp(symbol.library.c_str(), L"d3dcompiler_47.dll") == 0)
    {
        if (symbol.name == L"D3DCompile" && FindShaderCompilerProcedure("D3DCompile"))
            address = reinterpret_cast<ULONGLONG>(&GuestD3DCompile);
        else if (symbol.name == L"D3DCompile2" && FindShaderCompilerProcedure("D3DCompile2"))
            address = reinterpret_cast<ULONGLONG>(&GuestD3DCompile2);
        else if (symbol.name == L"D3DCompileFromFile" && FindShaderCompilerProcedure("D3DCompile"))
            address = reinterpret_cast<ULONGLONG>(&GuestD3DCompileFromFile);
        else if (symbol.name == L"D3DPreprocess" && FindShaderCompilerProcedure("D3DPreprocess"))
            address = reinterpret_cast<ULONGLONG>(&GuestD3DPreprocess);
        else if (symbol.name == L"D3DReflect") address = reinterpret_cast<ULONGLONG>(FindShaderCompilerProcedure("D3DReflect"));
        else if (symbol.name == L"D3DCreateBlob") address = reinterpret_cast<ULONGLONG>(FindShaderCompilerProcedure("D3DCreateBlob"));
        else if (symbol.name == L"D3DDisassemble") address = reinterpret_cast<ULONGLONG>(FindShaderCompilerProcedure("D3DDisassemble"));
    }
    else if (d3d12)
    {
        if ((createDeviceOrdinal || symbol.name == L"D3D12CreateDevice") && FindD3D12Procedure("D3D12CreateDevice"))
            address = reinterpret_cast<ULONGLONG>(&GuestD3D12CreateDevice);
        else if (symbol.name == L"D3D12GetInterface" && FindD3D12Procedure("D3D12GetInterface"))
            address = reinterpret_cast<ULONGLONG>(&GuestD3D12GetInterface);
        else if (symbol.name == L"D3D12SerializeRootSignature") address = reinterpret_cast<ULONGLONG>(FindD3D12Procedure("D3D12SerializeRootSignature"));
        else if (symbol.name == L"D3D12SerializeVersionedRootSignature") address = reinterpret_cast<ULONGLONG>(FindD3D12Procedure("D3D12SerializeVersionedRootSignature"));
        else if (symbol.name == L"D3D12CreateRootSignatureDeserializer") address = reinterpret_cast<ULONGLONG>(FindD3D12Procedure("D3D12CreateRootSignatureDeserializer"));
        else if (symbol.name == L"D3D12CreateVersionedRootSignatureDeserializer")
            address = reinterpret_cast<ULONGLONG>(FindD3D12Procedure("D3D12CreateVersionedRootSignatureDeserializer"));
    }
    return address ? ImportResolution{ImportDisposition::NeedsBridge, L"Direct3D host device and guest DXGI presentation.", address}
        : ImportResolution{};
}
