#include "pch.h"
#include "Bridge/WindowsCodecs.h"
#include "Bridge/FileStreams.h"
#include "Bridge/GuestModule.h"
#include "Bridge/GDI/Gdi32Shims.h"
#include "Bridge/User32Shims.h"
#include <wincodec.h>
#include <wrl/client.h>
#include <atomic>
#include <mutex>
#include <memory>
#include <new>
#include <vector>
#include <limits>
#include <stdexcept>
#include <utility>
using namespace Win32Bridge::Bridge;
using Microsoft::WRL::ComPtr;
namespace
{
    struct WicApartment
    {
        CO_MTA_USAGE_COOKIE cookie = nullptr;
        HRESULT status = ::CoIncrementMTAUsage(&cookie);
        ~WicApartment() { if (SUCCEEDED(status)) ::CoDecrementMTAUsage(cookie); }
    };
    HRESULT EnsureWicApartment()
    {
        // Native codecs can outlive the factory that created them. The image
        // service therefore keeps MTA support for its entire process lifetime.
        static WicApartment apartment;
        return apartment.status;
    }
    template<typename Interface> class WicObject : public Interface
    {
    public:
        ULONG STDMETHODCALLTYPE AddRef() override { return ++m_refs; }
        ULONG STDMETHODCALLTYPE Release() override { const ULONG refs = --m_refs; if (!refs) delete this; return refs; }
    protected:
        virtual ~WicObject() = default;
    private:
        std::atomic<ULONG> m_refs{1};
    };
    template<typename Interface> HRESULT QueryWic(Interface* object, REFIID, void** result)
    {
        if (!result) return E_POINTER;
        *result = object; object->AddRef(); return S_OK;
    }
    class WicStream final : public WicObject<IWICStream>
    {
    public:
        WicStream(IWICStream* stream, std::shared_ptr<GuestStorageContext> storage) : m_native(stream), m_storage(std::move(storage)) {}
        HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void** result) override
        {
            if (!result) return E_POINTER;
            *result = nullptr;
            if (iid != __uuidof(IUnknown) && iid != __uuidof(IStream) && iid != __uuidof(ISequentialStream) &&
                iid != __uuidof(IWICStream)) return E_NOINTERFACE;
            return QueryWic(static_cast<IWICStream*>(this), iid, result);
        }
        HRESULT STDMETHODCALLTYPE Read(void* data, ULONG bytes, ULONG* read) override { return m_native->Read(data, bytes, read); }
        HRESULT STDMETHODCALLTYPE Write(const void* data, ULONG bytes, ULONG* written) override { return m_native->Write(data, bytes, written); }
        HRESULT STDMETHODCALLTYPE Seek(LARGE_INTEGER move, DWORD origin, ULARGE_INTEGER* position) override { return m_native->Seek(move, origin, position); }
        HRESULT STDMETHODCALLTYPE SetSize(ULARGE_INTEGER size) override { return m_native->SetSize(size); }
        HRESULT STDMETHODCALLTYPE CopyTo(IStream* target, ULARGE_INTEGER size, ULARGE_INTEGER* read, ULARGE_INTEGER* written) override { return m_native->CopyTo(target, size, read, written); }
        HRESULT STDMETHODCALLTYPE Commit(DWORD flags) override { return m_native->Commit(flags); }
        HRESULT STDMETHODCALLTYPE Revert() override { return m_native->Revert(); }
        HRESULT STDMETHODCALLTYPE LockRegion(ULARGE_INTEGER offset, ULARGE_INTEGER size, DWORD flags) override { return m_native->LockRegion(offset, size, flags); }
        HRESULT STDMETHODCALLTYPE UnlockRegion(ULARGE_INTEGER offset, ULARGE_INTEGER size, DWORD flags) override { return m_native->UnlockRegion(offset, size, flags); }
        HRESULT STDMETHODCALLTYPE Stat(STATSTG* stat, DWORD flags) override { return m_native->Stat(stat, flags); }
        HRESULT STDMETHODCALLTYPE Clone(IStream** result) override { return m_native->Clone(result); }
        HRESULT STDMETHODCALLTYPE InitializeFromIStream(IStream *pIStream) override {
            std::lock_guard<std::mutex> lock(m_initLock);
            if (m_initialized) return WINCODEC_ERR_WRONGSTATE;
            const HRESULT status = m_native->InitializeFromIStream(pIStream);
            if (SUCCEEDED(status)) m_initialized = true; return status; }
        HRESULT STDMETHODCALLTYPE InitializeFromFilename(LPCWSTR wzFileName, DWORD dwDesiredAccess) override {
            std::lock_guard<std::mutex> lock(m_initLock);
            if (m_initialized) return WINCODEC_ERR_WRONGSTATE;
            ComPtr<IStream> file; HRESULT status = OpenGuestFileStream(wzFileName, dwDesiredAccess, &file, m_storage);
            if (FAILED(status)) return status;
            status = m_native->InitializeFromIStream(file.Get());
            if (SUCCEEDED(status)) m_initialized = true; return status; }
        HRESULT STDMETHODCALLTYPE InitializeFromMemory(WICInProcPointer pbBuffer, DWORD cbBufferSize) override {
            std::lock_guard<std::mutex> lock(m_initLock);
            if (m_initialized) return WINCODEC_ERR_WRONGSTATE;
            const HRESULT status = m_native->InitializeFromMemory(pbBuffer, cbBufferSize);
            if (SUCCEEDED(status)) m_initialized = true; return status; }
        HRESULT STDMETHODCALLTYPE InitializeFromIStreamRegion(IStream *pIStream, ULARGE_INTEGER ulOffset, ULARGE_INTEGER ulMaxSize) override {
            std::lock_guard<std::mutex> lock(m_initLock);
            if (m_initialized) return WINCODEC_ERR_WRONGSTATE;
            const HRESULT status = m_native->InitializeFromIStreamRegion(pIStream, ulOffset, ulMaxSize);
            if (SUCCEEDED(status)) m_initialized = true; return status; }
    private:
        ComPtr<IWICStream> m_native;
        std::shared_ptr<GuestStorageContext> m_storage;
        std::mutex m_initLock;
        bool m_initialized = false;
    };
    class WicColorContext final : public WicObject<IWICColorContext>
    {
    public:
        WicColorContext(IWICColorContext* context, std::shared_ptr<GuestStorageContext> storage) : m_native(context), m_storage(std::move(storage)) {}
        HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void** result) override
        {
            if (!result) return E_POINTER;
            *result = nullptr;
            if (iid != __uuidof(IUnknown) && iid != __uuidof(IWICColorContext)) return E_NOINTERFACE;
            return QueryWic(static_cast<IWICColorContext*>(this), iid, result);
        }
        HRESULT STDMETHODCALLTYPE InitializeFromFilename(LPCWSTR wzFilename) override {
            ComPtr<IStream> stream;
            HRESULT status = OpenGuestFileStream(wzFilename, GENERIC_READ, &stream, m_storage);
            if (FAILED(status)) return status;
            STATSTG stat{}; status = stream->Stat(&stat, STATFLAG_NONAME);
            if (FAILED(status)) return status;
            if (stat.cbSize.QuadPart > MAXDWORD) return WINCODEC_ERR_VALUEOUTOFRANGE;
            try {
                std::vector<BYTE> bytes(static_cast<size_t>(stat.cbSize.QuadPart));
                ULONG read = 0; status = stream->Read(bytes.data(), static_cast<ULONG>(bytes.size()), &read);
                if (FAILED(status)) return status;
                if (read != bytes.size()) return WINCODEC_ERR_BADIMAGE;
                return m_native->InitializeFromMemory(bytes.data(), read);
            } catch (const std::bad_alloc&) { return E_OUTOFMEMORY; }
            catch (const std::length_error&) { return E_OUTOFMEMORY; }
        }
        HRESULT STDMETHODCALLTYPE InitializeFromMemory(const BYTE *pbBuffer, UINT cbBufferSize) override {
            return m_native->InitializeFromMemory(pbBuffer, cbBufferSize);
        }
        HRESULT STDMETHODCALLTYPE InitializeFromExifColorSpace(UINT value) override {
            return m_native->InitializeFromExifColorSpace(value);
        }
        HRESULT STDMETHODCALLTYPE GetType(WICColorContextType *pType) override {
            return m_native->GetType(pType);
        }
        HRESULT STDMETHODCALLTYPE GetProfileBytes(UINT cbBuffer, BYTE *pbBuffer, UINT *pcbActual) override {
            return m_native->GetProfileBytes(cbBuffer, pbBuffer, pcbActual);
        }
        HRESULT STDMETHODCALLTYPE GetExifColorSpace(UINT *pValue) override {
            return m_native->GetExifColorSpace(pValue);
        }
    private: ComPtr<IWICColorContext> m_native;
        std::shared_ptr<GuestStorageContext> m_storage;
    };
    class WicFactory final : public WicObject<IWICImagingFactory2>
    {
    public:
        WicFactory(IWICImagingFactory* factory, std::shared_ptr<GuestStorageContext> storage) : m_native(factory), m_storage(std::move(storage)) { m_native.As(&m_extended); }
        HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void** result) override
        {
            if (!result) return E_POINTER;
            *result = nullptr;
            if (iid != __uuidof(IUnknown) && iid != __uuidof(IWICImagingFactory) &&
                (iid != __uuidof(IWICImagingFactory2) || !m_extended)) return E_NOINTERFACE;
            return QueryWic(static_cast<IWICImagingFactory2*>(this), iid, result);
        }
        HRESULT SurfaceBitmap(const MiniGdi::Surface& image, WICBitmapAlphaChannelOption alpha, IWICBitmap** result)
        {
            if (image.Empty()) return E_INVALIDARG;
            if (image.PixelCount() > MAXDWORD / sizeof(MiniGdi::Color)) return WINCODEC_ERR_VALUEOUTOFRANGE;
            const GUID& format = alpha == WICBitmapIgnoreAlpha ? GUID_WICPixelFormat32bppBGR :
                (alpha == WICBitmapUsePremultipliedAlpha ? GUID_WICPixelFormat32bppPBGRA : GUID_WICPixelFormat32bppBGRA);
            return m_native->CreateBitmapFromMemory(image.Width(), image.Height(), format, image.Width() * 4,
                static_cast<UINT>(image.PixelCount() * 4), reinterpret_cast<BYTE*>(const_cast<MiniGdi::Color*>(image.Data())), result);
        }
        HRESULT STDMETHODCALLTYPE CreateDecoderFromFilename(LPCWSTR wzFilename, const GUID *pguidVendor, DWORD dwDesiredAccess, WICDecodeOptions metadataOptions, IWICBitmapDecoder **ppIDecoder) override {
            if (!ppIDecoder) return E_POINTER;
            *ppIDecoder = nullptr;
            ComPtr<IStream> stream;
            HRESULT status = OpenGuestFileStream(wzFilename, dwDesiredAccess, &stream, m_storage);
            return FAILED(status) ? status : m_native->CreateDecoderFromStream(stream.Get(), pguidVendor, metadataOptions, ppIDecoder);
        }
        HRESULT STDMETHODCALLTYPE CreateDecoderFromStream(IStream *pIStream, const GUID *pguidVendor, WICDecodeOptions metadataOptions, IWICBitmapDecoder **ppIDecoder) override {
            return m_native->CreateDecoderFromStream(pIStream, pguidVendor, metadataOptions, ppIDecoder);
        }
        HRESULT STDMETHODCALLTYPE CreateDecoderFromFileHandle(ULONG_PTR hFile, const GUID *pguidVendor, WICDecodeOptions metadataOptions, IWICBitmapDecoder **ppIDecoder) override {
            if (!ppIDecoder) return E_POINTER;
            *ppIDecoder = nullptr;
            ComPtr<IStream> stream;
            HRESULT status = BorrowGuestFileStream(reinterpret_cast<HANDLE>(hFile), &stream, m_storage);
            return FAILED(status) ? status : m_native->CreateDecoderFromStream(stream.Get(), pguidVendor, metadataOptions, ppIDecoder);
        }
        HRESULT STDMETHODCALLTYPE CreateComponentInfo(REFCLSID clsidComponent, IWICComponentInfo **ppIInfo) override {
            return m_native->CreateComponentInfo(clsidComponent, ppIInfo);
        }
        HRESULT STDMETHODCALLTYPE CreateDecoder(REFGUID guidContainerFormat, const GUID *pguidVendor, IWICBitmapDecoder **ppIDecoder) override {
            return m_native->CreateDecoder(guidContainerFormat, pguidVendor, ppIDecoder);
        }
        HRESULT STDMETHODCALLTYPE CreateEncoder(REFGUID guidContainerFormat, const GUID *pguidVendor, IWICBitmapEncoder **ppIEncoder) override {
            return m_native->CreateEncoder(guidContainerFormat, pguidVendor, ppIEncoder);
        }
        HRESULT STDMETHODCALLTYPE CreatePalette(IWICPalette **ppIPalette) override {
            return m_native->CreatePalette(ppIPalette);
        }
        HRESULT STDMETHODCALLTYPE CreateFormatConverter(IWICFormatConverter **ppIFormatConverter) override {
            return m_native->CreateFormatConverter(ppIFormatConverter);
        }
        HRESULT STDMETHODCALLTYPE CreateBitmapScaler(IWICBitmapScaler **ppIBitmapScaler) override {
            return m_native->CreateBitmapScaler(ppIBitmapScaler);
        }
        HRESULT STDMETHODCALLTYPE CreateBitmapClipper(IWICBitmapClipper **ppIBitmapClipper) override {
            return m_native->CreateBitmapClipper(ppIBitmapClipper);
        }
        HRESULT STDMETHODCALLTYPE CreateBitmapFlipRotator(IWICBitmapFlipRotator **ppIBitmapFlipRotator) override {
            return m_native->CreateBitmapFlipRotator(ppIBitmapFlipRotator);
        }
        HRESULT STDMETHODCALLTYPE CreateStream(IWICStream **ppIWICStream) override {
            if (!ppIWICStream) return E_POINTER;
            *ppIWICStream = nullptr;
            ComPtr<IWICStream> stream; HRESULT status = m_native->CreateStream(&stream);
            if (FAILED(status)) return status;
            *ppIWICStream = new (std::nothrow) WicStream(stream.Get(), m_storage);
            return *ppIWICStream ? S_OK : E_OUTOFMEMORY;
        }
        HRESULT STDMETHODCALLTYPE CreateColorContext(IWICColorContext **ppIWICColorContext) override {
            if (!ppIWICColorContext) return E_POINTER;
            *ppIWICColorContext = nullptr;
            ComPtr<IWICColorContext> context; HRESULT status = m_native->CreateColorContext(&context);
            if (FAILED(status)) return status;
            *ppIWICColorContext = new (std::nothrow) WicColorContext(context.Get(), m_storage);
            return *ppIWICColorContext ? S_OK : E_OUTOFMEMORY;
        }
        HRESULT STDMETHODCALLTYPE CreateColorTransformer(IWICColorTransform **ppIWICColorTransform) override {
            return m_native->CreateColorTransformer(ppIWICColorTransform);
        }
        HRESULT STDMETHODCALLTYPE CreateBitmap(UINT uiWidth, UINT uiHeight, REFWICPixelFormatGUID pixelFormat, WICBitmapCreateCacheOption option, IWICBitmap **ppIBitmap) override {
            return m_native->CreateBitmap(uiWidth, uiHeight, pixelFormat, option, ppIBitmap);
        }
        HRESULT STDMETHODCALLTYPE CreateBitmapFromSource(IWICBitmapSource *pIBitmapSource, WICBitmapCreateCacheOption option, IWICBitmap **ppIBitmap) override {
            return m_native->CreateBitmapFromSource(pIBitmapSource, option, ppIBitmap);
        }
        HRESULT STDMETHODCALLTYPE CreateBitmapFromSourceRect(IWICBitmapSource *pIBitmapSource, UINT x, UINT y, UINT width, UINT height, IWICBitmap **ppIBitmap) override {
            return m_native->CreateBitmapFromSourceRect(pIBitmapSource, x, y, width, height, ppIBitmap);
        }
        HRESULT STDMETHODCALLTYPE CreateBitmapFromMemory(UINT uiWidth, UINT uiHeight, REFWICPixelFormatGUID pixelFormat, UINT cbStride, UINT cbBufferSize, BYTE *pbBuffer, IWICBitmap **ppIBitmap) override {
            return m_native->CreateBitmapFromMemory(uiWidth, uiHeight, pixelFormat, cbStride, cbBufferSize, pbBuffer, ppIBitmap);
        }
        HRESULT STDMETHODCALLTYPE CreateBitmapFromHBITMAP(HBITMAP hBitmap, HPALETTE hPalette, WICBitmapAlphaChannelOption options, IWICBitmap **ppIBitmap) override {
            if (!ppIBitmap) return E_POINTER;
            *ppIBitmap = nullptr;
            if (options != WICBitmapUseAlpha && options != WICBitmapUsePremultipliedAlpha && options != WICBitmapIgnoreAlpha) return E_INVALIDARG;
            MiniGdi::Surface image;
            if (!CopyGuestBitmapPixels(hBitmap, &image)) return E_INVALIDARG;
            return SurfaceBitmap(image, options, ppIBitmap);
        }
        HRESULT STDMETHODCALLTYPE CreateBitmapFromHICON(HICON hIcon, IWICBitmap **ppIBitmap) override {
            if (!ppIBitmap) return E_POINTER;
            *ppIBitmap = nullptr;
            MiniGdi::Surface image;
            if (!CopyGuestIconPixels(hIcon, &image)) return E_INVALIDARG;
            return SurfaceBitmap(image, WICBitmapUseAlpha, ppIBitmap);
        }
        HRESULT STDMETHODCALLTYPE CreateComponentEnumerator(DWORD componentTypes, DWORD options, IEnumUnknown **ppIEnumUnknown) override {
            return m_native->CreateComponentEnumerator(componentTypes, options, ppIEnumUnknown);
        }
        HRESULT STDMETHODCALLTYPE CreateFastMetadataEncoderFromDecoder(IWICBitmapDecoder *pIDecoder, IWICFastMetadataEncoder **ppIFastEncoder) override {
            return m_native->CreateFastMetadataEncoderFromDecoder(pIDecoder, ppIFastEncoder);
        }
        HRESULT STDMETHODCALLTYPE CreateFastMetadataEncoderFromFrameDecode(IWICBitmapFrameDecode *pIFrameDecoder, IWICFastMetadataEncoder **ppIFastEncoder) override {
            return m_native->CreateFastMetadataEncoderFromFrameDecode(pIFrameDecoder, ppIFastEncoder);
        }
        HRESULT STDMETHODCALLTYPE CreateQueryWriter(REFGUID guidMetadataFormat, const GUID *pguidVendor, IWICMetadataQueryWriter **ppIQueryWriter) override {
            return m_native->CreateQueryWriter(guidMetadataFormat, pguidVendor, ppIQueryWriter);
        }
        HRESULT STDMETHODCALLTYPE CreateQueryWriterFromReader(IWICMetadataQueryReader *pIQueryReader, const GUID *pguidVendor, IWICMetadataQueryWriter **ppIQueryWriter) override {
            return m_native->CreateQueryWriterFromReader(pIQueryReader, pguidVendor, ppIQueryWriter);
        }
        HRESULT STDMETHODCALLTYPE CreateImageEncoder(ID2D1Device* device, IWICImageEncoder** result) override
        { if (!result) return E_POINTER; *result = nullptr; return m_extended ? m_extended->CreateImageEncoder(device, result) : E_NOINTERFACE; }
    private:
        ComPtr<IWICImagingFactory> m_native;
        std::shared_ptr<GuestStorageContext> m_storage;
        ComPtr<IWICImagingFactory2> m_extended;
    };
}
bool Win32Bridge::Bridge::IsGuestWicFactoryClass(REFCLSID clsid)
{ return clsid == CLSID_WICImagingFactory || clsid == CLSID_WICImagingFactory2; }
HRESULT Win32Bridge::Bridge::CreateGuestWicFactory(REFIID iid, void** result)
{
    if (!result) return E_POINTER;
    *result = nullptr;
    HRESULT status = EnsureWicApartment();
    if (FAILED(status)) return status;
    ComPtr<IWICImagingFactory> native;
    status = ::CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&native));
    if (FAILED(status)) return status;
    auto factory = new (std::nothrow) WicFactory(native.Get(), CurrentGuestModuleLoader() ? CurrentGuestModuleLoader()->StorageContext() : nullptr);
    if (!factory) return E_OUTOFMEMORY;
    status = factory->QueryInterface(iid, result);
    factory->Release(); return status;
}
ImportResolution Win32Bridge::Bridge::ResolveWindowsCodecsImport(const ImportedSymbol& symbol)
{
    ImportResolution result{ImportDisposition::Unsupported, L"Unsupported WindowsCodecs export.", 0};
    if (symbol.importedByOrdinal || _wcsicmp(symbol.library.c_str(), L"windowscodecs.dll") != 0) return result;
    if (symbol.name == L"WICConvertBitmapSource") result.targetAddress = reinterpret_cast<ULONGLONG>(&::WICConvertBitmapSource);
    else if (symbol.name == L"WICMapGuidToShortName") result.targetAddress = reinterpret_cast<ULONGLONG>(&::WICMapGuidToShortName);
    else if (symbol.name == L"WICMapShortNameToGuid") result.targetAddress = reinterpret_cast<ULONGLONG>(&::WICMapShortNameToGuid);
    else if (symbol.name == L"WICMapSchemaToName") result.targetAddress = reinterpret_cast<ULONGLONG>(&::WICMapSchemaToName);
    if (result.targetAddress) { result.disposition = ImportDisposition::NeedsBridge; result.note = L"Native WIC pixel and metadata conversion."; }
    return result;
}