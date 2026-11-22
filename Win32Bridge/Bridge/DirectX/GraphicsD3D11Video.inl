    // These interfaces share the guest device/context IUnknown and translate
    // resource inputs before they reach the native video runtime.
    class GuestD3D11VideoDevice final : public ID3D11VideoDevice2
    {
    public:
        GuestD3D11VideoDevice(ID3D11Device* owner, ID3D11Device* native)
            : m_owner(owner)
        {
            native->QueryInterface(IID_PPV_ARGS(&m_video));
            if (!m_video) return;
            native->QueryInterface(IID_PPV_ARGS(&m_video1));
            native->QueryInterface(IID_PPV_ARGS(&m_video2));
        }

        bool Supports(REFIID iid) const
        {
            return (iid == __uuidof(ID3D11VideoDevice) && m_video) ||
                (iid == __uuidof(ID3D11VideoDevice1) && m_video1) ||
                (iid == __uuidof(ID3D11VideoDevice2) && m_video2);
        }

        HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void** output) override
        { return m_owner->QueryInterface(iid, output); }
        ULONG STDMETHODCALLTYPE AddRef() override { return m_owner->AddRef(); }
        ULONG STDMETHODCALLTYPE Release() override { return m_owner->Release(); }

        HRESULT STDMETHODCALLTYPE CreateVideoDecoder(
            const D3D11_VIDEO_DECODER_DESC* pVideoDesc, const D3D11_VIDEO_DECODER_CONFIG* pConfig,
            ID3D11VideoDecoder** ppDecoder) override
        { return m_video->CreateVideoDecoder(pVideoDesc, pConfig, ppDecoder); }

        HRESULT STDMETHODCALLTYPE CreateVideoProcessor(
            ID3D11VideoProcessorEnumerator* pEnum, UINT RateConversionIndex, ID3D11VideoProcessor** ppVideoProcessor) override
        { return m_video->CreateVideoProcessor(pEnum, RateConversionIndex, ppVideoProcessor); }

        HRESULT STDMETHODCALLTYPE CreateAuthenticatedChannel(
            D3D11_AUTHENTICATED_CHANNEL_TYPE ChannelType, ID3D11AuthenticatedChannel** ppAuthenticatedChannel) override
        { return m_video->CreateAuthenticatedChannel(ChannelType, ppAuthenticatedChannel); }

        HRESULT STDMETHODCALLTYPE CreateCryptoSession(
            const GUID* pCryptoType, const GUID* pDecoderProfile, const GUID* pKeyExchangeType,
            ID3D11CryptoSession** ppCryptoSession) override
        { return m_video->CreateCryptoSession(pCryptoType, pDecoderProfile, pKeyExchangeType, ppCryptoSession); }

        HRESULT STDMETHODCALLTYPE CreateVideoDecoderOutputView(
            ID3D11Resource* pResource, const D3D11_VIDEO_DECODER_OUTPUT_VIEW_DESC* pDesc,
            ID3D11VideoDecoderOutputView** ppVDOVView) override
        { return m_video->CreateVideoDecoderOutputView(NativeD3D11Child(pResource), pDesc, ppVDOVView); }

        HRESULT STDMETHODCALLTYPE CreateVideoProcessorInputView(
            ID3D11Resource* pResource, ID3D11VideoProcessorEnumerator* pEnum,
            const D3D11_VIDEO_PROCESSOR_INPUT_VIEW_DESC* pDesc, ID3D11VideoProcessorInputView** ppVPIView) override
        { return m_video->CreateVideoProcessorInputView(NativeD3D11Child(pResource), pEnum, pDesc, ppVPIView); }

        HRESULT STDMETHODCALLTYPE CreateVideoProcessorOutputView(
            ID3D11Resource* pResource, ID3D11VideoProcessorEnumerator* pEnum,
            const D3D11_VIDEO_PROCESSOR_OUTPUT_VIEW_DESC* pDesc, ID3D11VideoProcessorOutputView** ppVPOView) override
        { return m_video->CreateVideoProcessorOutputView(NativeD3D11Child(pResource), pEnum, pDesc, ppVPOView); }

        HRESULT STDMETHODCALLTYPE CreateVideoProcessorEnumerator(
            const D3D11_VIDEO_PROCESSOR_CONTENT_DESC* pDesc, ID3D11VideoProcessorEnumerator** ppEnum) override
        { return m_video->CreateVideoProcessorEnumerator(pDesc, ppEnum); }

        UINT STDMETHODCALLTYPE GetVideoDecoderProfileCount() override
        { return m_video->GetVideoDecoderProfileCount(); }

        HRESULT STDMETHODCALLTYPE GetVideoDecoderProfile(UINT Index, GUID* pDecoderProfile) override
        { return m_video->GetVideoDecoderProfile(Index, pDecoderProfile); }

        HRESULT STDMETHODCALLTYPE CheckVideoDecoderFormat(
            const GUID* pDecoderProfile, DXGI_FORMAT Format, BOOL* pSupported) override
        { return m_video->CheckVideoDecoderFormat(pDecoderProfile, Format, pSupported); }

        HRESULT STDMETHODCALLTYPE GetVideoDecoderConfigCount(const D3D11_VIDEO_DECODER_DESC* pDesc, UINT* pCount) override
        { return m_video->GetVideoDecoderConfigCount(pDesc, pCount); }

        HRESULT STDMETHODCALLTYPE GetVideoDecoderConfig(
            const D3D11_VIDEO_DECODER_DESC* pDesc, UINT Index, D3D11_VIDEO_DECODER_CONFIG* pConfig) override
        { return m_video->GetVideoDecoderConfig(pDesc, Index, pConfig); }

        HRESULT STDMETHODCALLTYPE GetContentProtectionCaps(
            const GUID* pCryptoType, const GUID* pDecoderProfile, D3D11_VIDEO_CONTENT_PROTECTION_CAPS* pCaps) override
        { return m_video->GetContentProtectionCaps(pCryptoType, pDecoderProfile, pCaps); }

        HRESULT STDMETHODCALLTYPE CheckCryptoKeyExchange(
            const GUID* pCryptoType, const GUID* pDecoderProfile, UINT Index, GUID* pKeyExchangeType) override
        { return m_video->CheckCryptoKeyExchange(pCryptoType, pDecoderProfile, Index, pKeyExchangeType); }

        HRESULT STDMETHODCALLTYPE SetPrivateData(REFGUID guid, UINT DataSize, const void* pData) override
        { return m_video->SetPrivateData(guid, DataSize, pData); }

        HRESULT STDMETHODCALLTYPE SetPrivateDataInterface(REFGUID guid, const IUnknown* pData) override
        { return m_video->SetPrivateDataInterface(guid, pData); }

        HRESULT STDMETHODCALLTYPE GetCryptoSessionPrivateDataSize(
            const GUID* pCryptoType, const GUID* pDecoderProfile, const GUID* pKeyExchangeType, UINT* pPrivateInputSize,
            UINT* pPrivateOutputSize) override
        { return m_video1->GetCryptoSessionPrivateDataSize(
            pCryptoType, pDecoderProfile, pKeyExchangeType, pPrivateInputSize, pPrivateOutputSize); }

        HRESULT STDMETHODCALLTYPE GetVideoDecoderCaps(
            const GUID* pDecoderProfile, UINT SampleWidth, UINT SampleHeight, const DXGI_RATIONAL* pFrameRate, UINT BitRate,
            const GUID* pCryptoType, UINT* pDecoderCaps) override
        { return m_video1->GetVideoDecoderCaps(
            pDecoderProfile, SampleWidth, SampleHeight, pFrameRate, BitRate, pCryptoType, pDecoderCaps); }

        HRESULT STDMETHODCALLTYPE CheckVideoDecoderDownsampling(
            const D3D11_VIDEO_DECODER_DESC* pInputDesc, DXGI_COLOR_SPACE_TYPE InputColorSpace,
            const D3D11_VIDEO_DECODER_CONFIG* pInputConfig, const DXGI_RATIONAL* pFrameRate,
            const D3D11_VIDEO_SAMPLE_DESC* pOutputDesc, BOOL* pSupported, BOOL* pRealTimeHint) override
        { return m_video1->CheckVideoDecoderDownsampling(
            pInputDesc, InputColorSpace, pInputConfig, pFrameRate, pOutputDesc, pSupported, pRealTimeHint); }

        HRESULT STDMETHODCALLTYPE RecommendVideoDecoderDownsampleParameters(
            const D3D11_VIDEO_DECODER_DESC* pInputDesc, DXGI_COLOR_SPACE_TYPE InputColorSpace,
            const D3D11_VIDEO_DECODER_CONFIG* pInputConfig, const DXGI_RATIONAL* pFrameRate,
            D3D11_VIDEO_SAMPLE_DESC* pRecommendedOutputDesc) override
        { return m_video1->RecommendVideoDecoderDownsampleParameters(
            pInputDesc, InputColorSpace, pInputConfig, pFrameRate, pRecommendedOutputDesc); }

        HRESULT STDMETHODCALLTYPE CheckFeatureSupport(
            D3D11_FEATURE_VIDEO Feature, void* pFeatureSupportData, UINT FeatureSupportDataSize) override
        { return m_video2->CheckFeatureSupport(Feature, pFeatureSupportData, FeatureSupportDataSize); }

        HRESULT STDMETHODCALLTYPE NegotiateCryptoSessionKeyExchangeMT(
            ID3D11CryptoSession* pCryptoSession, D3D11_CRYPTO_SESSION_KEY_EXCHANGE_FLAGS flags, UINT DataSize, void* pData) override
        { return m_video2->NegotiateCryptoSessionKeyExchangeMT(pCryptoSession, flags, DataSize, pData); }

    private:
        ID3D11Device* m_owner;
        ComPtr<ID3D11VideoDevice> m_video;
        ComPtr<ID3D11VideoDevice1> m_video1;
        ComPtr<ID3D11VideoDevice2> m_video2;
    };

    class GuestD3D11VideoContext final : public ID3D11VideoContext3
    {
    public:
        GuestD3D11VideoContext(ID3D11DeviceContext* owner, ID3D11DeviceContext* native, ID3D11Device* device)
            : m_owner(owner), m_device(device)
        {
            native->QueryInterface(IID_PPV_ARGS(&m_video));
            if (!m_video) return;
            native->QueryInterface(IID_PPV_ARGS(&m_video1));
            native->QueryInterface(IID_PPV_ARGS(&m_video2));
            native->QueryInterface(IID_PPV_ARGS(&m_video3));
        }

        bool Supports(REFIID iid) const
        {
            return (iid == __uuidof(ID3D11VideoContext) && m_video) ||
                (iid == __uuidof(ID3D11VideoContext1) && m_video1) ||
                (iid == __uuidof(ID3D11VideoContext2) && m_video2) ||
                (iid == __uuidof(ID3D11VideoContext3) && m_video3);
        }

        HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void** output) override
        { return m_owner->QueryInterface(iid, output); }
        ULONG STDMETHODCALLTYPE AddRef() override { return m_owner->AddRef(); }
        ULONG STDMETHODCALLTYPE Release() override { return m_owner->Release(); }
        void STDMETHODCALLTYPE GetDevice(ID3D11Device** output) override
        { if (output) { *output = m_device; m_device->AddRef(); } }
        HRESULT STDMETHODCALLTYPE GetPrivateData(REFGUID guid, UINT* size, void* data) override
        { return m_video->GetPrivateData(guid, size, data); }
        HRESULT STDMETHODCALLTYPE SetPrivateData(REFGUID guid, UINT size, const void* data) override
        { return m_video->SetPrivateData(guid, size, data); }
        HRESULT STDMETHODCALLTYPE SetPrivateDataInterface(REFGUID guid, const IUnknown* data) override
        { return m_video->SetPrivateDataInterface(guid, data); }

        HRESULT STDMETHODCALLTYPE GetDecoderBuffer(
            ID3D11VideoDecoder* pDecoder, D3D11_VIDEO_DECODER_BUFFER_TYPE Type, UINT* pBufferSize, void** ppBuffer) override
        { return m_video->GetDecoderBuffer(pDecoder, Type, pBufferSize, ppBuffer); }

        HRESULT STDMETHODCALLTYPE ReleaseDecoderBuffer(
            ID3D11VideoDecoder* pDecoder, D3D11_VIDEO_DECODER_BUFFER_TYPE Type) override
        { return m_video->ReleaseDecoderBuffer(pDecoder, Type); }

        HRESULT STDMETHODCALLTYPE DecoderBeginFrame(
            ID3D11VideoDecoder* pDecoder, ID3D11VideoDecoderOutputView* pView, UINT ContentKeySize, const void* pContentKey) override
        { return m_video->DecoderBeginFrame(pDecoder, pView, ContentKeySize, pContentKey); }

        HRESULT STDMETHODCALLTYPE DecoderEndFrame(ID3D11VideoDecoder* pDecoder) override
        { return m_video->DecoderEndFrame(pDecoder); }

        HRESULT STDMETHODCALLTYPE SubmitDecoderBuffers(
            ID3D11VideoDecoder* pDecoder, UINT NumBuffers, const D3D11_VIDEO_DECODER_BUFFER_DESC* pBufferDesc) override
        { return m_video->SubmitDecoderBuffers(pDecoder, NumBuffers, pBufferDesc); }

        APP_DEPRECATED_HRESULT STDMETHODCALLTYPE DecoderExtension(
            ID3D11VideoDecoder* pDecoder, const D3D11_VIDEO_DECODER_EXTENSION* pExtensionData) override
        { return m_video->DecoderExtension(pDecoder, pExtensionData); }

        void STDMETHODCALLTYPE VideoProcessorSetOutputTargetRect(
            ID3D11VideoProcessor* pVideoProcessor, BOOL Enable, const RECT* pRect) override
        { m_video->VideoProcessorSetOutputTargetRect(pVideoProcessor, Enable, pRect); }

        void STDMETHODCALLTYPE VideoProcessorSetOutputBackgroundColor(
            ID3D11VideoProcessor* pVideoProcessor, BOOL YCbCr, const D3D11_VIDEO_COLOR* pColor) override
        { m_video->VideoProcessorSetOutputBackgroundColor(pVideoProcessor, YCbCr, pColor); }

        void STDMETHODCALLTYPE VideoProcessorSetOutputColorSpace(
            ID3D11VideoProcessor* pVideoProcessor, const D3D11_VIDEO_PROCESSOR_COLOR_SPACE* pColorSpace) override
        { m_video->VideoProcessorSetOutputColorSpace(pVideoProcessor, pColorSpace); }

        void STDMETHODCALLTYPE VideoProcessorSetOutputAlphaFillMode(
            ID3D11VideoProcessor* pVideoProcessor, D3D11_VIDEO_PROCESSOR_ALPHA_FILL_MODE AlphaFillMode, UINT StreamIndex) override
        { m_video->VideoProcessorSetOutputAlphaFillMode(pVideoProcessor, AlphaFillMode, StreamIndex); }

        void STDMETHODCALLTYPE VideoProcessorSetOutputConstriction(
            ID3D11VideoProcessor* pVideoProcessor, BOOL Enable, SIZE Size) override
        { m_video->VideoProcessorSetOutputConstriction(pVideoProcessor, Enable, Size); }

        void STDMETHODCALLTYPE VideoProcessorSetOutputStereoMode(ID3D11VideoProcessor* pVideoProcessor, BOOL Enable) override
        { m_video->VideoProcessorSetOutputStereoMode(pVideoProcessor, Enable); }

        APP_DEPRECATED_HRESULT STDMETHODCALLTYPE VideoProcessorSetOutputExtension(
            ID3D11VideoProcessor* pVideoProcessor, const GUID* pExtensionGuid, UINT DataSize, void* pData) override
        { return m_video->VideoProcessorSetOutputExtension(pVideoProcessor, pExtensionGuid, DataSize, pData); }

        void STDMETHODCALLTYPE VideoProcessorGetOutputTargetRect(
            ID3D11VideoProcessor* pVideoProcessor, BOOL* Enabled, RECT* pRect) override
        { m_video->VideoProcessorGetOutputTargetRect(pVideoProcessor, Enabled, pRect); }

        void STDMETHODCALLTYPE VideoProcessorGetOutputBackgroundColor(
            ID3D11VideoProcessor* pVideoProcessor, BOOL* pYCbCr, D3D11_VIDEO_COLOR* pColor) override
        { m_video->VideoProcessorGetOutputBackgroundColor(pVideoProcessor, pYCbCr, pColor); }

        void STDMETHODCALLTYPE VideoProcessorGetOutputColorSpace(
            ID3D11VideoProcessor* pVideoProcessor, D3D11_VIDEO_PROCESSOR_COLOR_SPACE* pColorSpace) override
        { m_video->VideoProcessorGetOutputColorSpace(pVideoProcessor, pColorSpace); }

        void STDMETHODCALLTYPE VideoProcessorGetOutputAlphaFillMode(
            ID3D11VideoProcessor* pVideoProcessor, D3D11_VIDEO_PROCESSOR_ALPHA_FILL_MODE* pAlphaFillMode, UINT* pStreamIndex) override
        { m_video->VideoProcessorGetOutputAlphaFillMode(pVideoProcessor, pAlphaFillMode, pStreamIndex); }

        void STDMETHODCALLTYPE VideoProcessorGetOutputConstriction(
            ID3D11VideoProcessor* pVideoProcessor, BOOL* pEnabled, SIZE* pSize) override
        { m_video->VideoProcessorGetOutputConstriction(pVideoProcessor, pEnabled, pSize); }

        void STDMETHODCALLTYPE VideoProcessorGetOutputStereoMode(ID3D11VideoProcessor* pVideoProcessor, BOOL* pEnabled) override
        { m_video->VideoProcessorGetOutputStereoMode(pVideoProcessor, pEnabled); }

        APP_DEPRECATED_HRESULT STDMETHODCALLTYPE VideoProcessorGetOutputExtension(
            ID3D11VideoProcessor* pVideoProcessor, const GUID* pExtensionGuid, UINT DataSize, void* pData) override
        { return m_video->VideoProcessorGetOutputExtension(pVideoProcessor, pExtensionGuid, DataSize, pData); }

        void STDMETHODCALLTYPE VideoProcessorSetStreamFrameFormat(
            ID3D11VideoProcessor* pVideoProcessor, UINT StreamIndex, D3D11_VIDEO_FRAME_FORMAT FrameFormat) override
        { m_video->VideoProcessorSetStreamFrameFormat(pVideoProcessor, StreamIndex, FrameFormat); }

        void STDMETHODCALLTYPE VideoProcessorSetStreamColorSpace(
            ID3D11VideoProcessor* pVideoProcessor, UINT StreamIndex, const D3D11_VIDEO_PROCESSOR_COLOR_SPACE* pColorSpace) override
        { m_video->VideoProcessorSetStreamColorSpace(pVideoProcessor, StreamIndex, pColorSpace); }

        void STDMETHODCALLTYPE VideoProcessorSetStreamOutputRate(
            ID3D11VideoProcessor* pVideoProcessor, UINT StreamIndex, D3D11_VIDEO_PROCESSOR_OUTPUT_RATE OutputRate,
            BOOL RepeatFrame, const DXGI_RATIONAL* pCustomRate) override
        { m_video->VideoProcessorSetStreamOutputRate(pVideoProcessor, StreamIndex, OutputRate, RepeatFrame, pCustomRate); }

        void STDMETHODCALLTYPE VideoProcessorSetStreamSourceRect(
            ID3D11VideoProcessor* pVideoProcessor, UINT StreamIndex, BOOL Enable, const RECT* pRect) override
        { m_video->VideoProcessorSetStreamSourceRect(pVideoProcessor, StreamIndex, Enable, pRect); }

        void STDMETHODCALLTYPE VideoProcessorSetStreamDestRect(
            ID3D11VideoProcessor* pVideoProcessor, UINT StreamIndex, BOOL Enable, const RECT* pRect) override
        { m_video->VideoProcessorSetStreamDestRect(pVideoProcessor, StreamIndex, Enable, pRect); }

        void STDMETHODCALLTYPE VideoProcessorSetStreamAlpha(
            ID3D11VideoProcessor* pVideoProcessor, UINT StreamIndex, BOOL Enable, FLOAT Alpha) override
        { m_video->VideoProcessorSetStreamAlpha(pVideoProcessor, StreamIndex, Enable, Alpha); }

        void STDMETHODCALLTYPE VideoProcessorSetStreamPalette(
            ID3D11VideoProcessor* pVideoProcessor, UINT StreamIndex, UINT Count, const UINT* pEntries) override
        { m_video->VideoProcessorSetStreamPalette(pVideoProcessor, StreamIndex, Count, pEntries); }

        void STDMETHODCALLTYPE VideoProcessorSetStreamPixelAspectRatio(
            ID3D11VideoProcessor* pVideoProcessor, UINT StreamIndex, BOOL Enable, const DXGI_RATIONAL* pSourceAspectRatio,
            const DXGI_RATIONAL* pDestinationAspectRatio) override
        { m_video->VideoProcessorSetStreamPixelAspectRatio(
            pVideoProcessor, StreamIndex, Enable, pSourceAspectRatio, pDestinationAspectRatio); }

        void STDMETHODCALLTYPE VideoProcessorSetStreamLumaKey(
            ID3D11VideoProcessor* pVideoProcessor, UINT StreamIndex, BOOL Enable, FLOAT Lower, FLOAT Upper) override
        { m_video->VideoProcessorSetStreamLumaKey(pVideoProcessor, StreamIndex, Enable, Lower, Upper); }

        void STDMETHODCALLTYPE VideoProcessorSetStreamStereoFormat(
            ID3D11VideoProcessor* pVideoProcessor, UINT StreamIndex, BOOL Enable, D3D11_VIDEO_PROCESSOR_STEREO_FORMAT Format,
            BOOL LeftViewFrame0, BOOL BaseViewFrame0, D3D11_VIDEO_PROCESSOR_STEREO_FLIP_MODE FlipMode, int MonoOffset) override
        { m_video->VideoProcessorSetStreamStereoFormat(
            pVideoProcessor, StreamIndex, Enable, Format, LeftViewFrame0, BaseViewFrame0, FlipMode, MonoOffset); }

        void STDMETHODCALLTYPE VideoProcessorSetStreamAutoProcessingMode(
            ID3D11VideoProcessor* pVideoProcessor, UINT StreamIndex, BOOL Enable) override
        { m_video->VideoProcessorSetStreamAutoProcessingMode(pVideoProcessor, StreamIndex, Enable); }

        void STDMETHODCALLTYPE VideoProcessorSetStreamFilter(
            ID3D11VideoProcessor* pVideoProcessor, UINT StreamIndex, D3D11_VIDEO_PROCESSOR_FILTER Filter, BOOL Enable,
            int Level) override
        { m_video->VideoProcessorSetStreamFilter(pVideoProcessor, StreamIndex, Filter, Enable, Level); }

        APP_DEPRECATED_HRESULT STDMETHODCALLTYPE VideoProcessorSetStreamExtension(
            ID3D11VideoProcessor* pVideoProcessor, UINT StreamIndex, const GUID* pExtensionGuid, UINT DataSize, void* pData) override
        { return m_video->VideoProcessorSetStreamExtension(pVideoProcessor, StreamIndex, pExtensionGuid, DataSize, pData); }

        void STDMETHODCALLTYPE VideoProcessorGetStreamFrameFormat(
            ID3D11VideoProcessor* pVideoProcessor, UINT StreamIndex, D3D11_VIDEO_FRAME_FORMAT* pFrameFormat) override
        { m_video->VideoProcessorGetStreamFrameFormat(pVideoProcessor, StreamIndex, pFrameFormat); }

        void STDMETHODCALLTYPE VideoProcessorGetStreamColorSpace(
            ID3D11VideoProcessor* pVideoProcessor, UINT StreamIndex, D3D11_VIDEO_PROCESSOR_COLOR_SPACE* pColorSpace) override
        { m_video->VideoProcessorGetStreamColorSpace(pVideoProcessor, StreamIndex, pColorSpace); }

        void STDMETHODCALLTYPE VideoProcessorGetStreamOutputRate(
            ID3D11VideoProcessor* pVideoProcessor, UINT StreamIndex, D3D11_VIDEO_PROCESSOR_OUTPUT_RATE* pOutputRate,
            BOOL* pRepeatFrame, DXGI_RATIONAL* pCustomRate) override
        { m_video->VideoProcessorGetStreamOutputRate(pVideoProcessor, StreamIndex, pOutputRate, pRepeatFrame, pCustomRate); }

        void STDMETHODCALLTYPE VideoProcessorGetStreamSourceRect(
            ID3D11VideoProcessor* pVideoProcessor, UINT StreamIndex, BOOL* pEnabled, RECT* pRect) override
        { m_video->VideoProcessorGetStreamSourceRect(pVideoProcessor, StreamIndex, pEnabled, pRect); }

        void STDMETHODCALLTYPE VideoProcessorGetStreamDestRect(
            ID3D11VideoProcessor* pVideoProcessor, UINT StreamIndex, BOOL* pEnabled, RECT* pRect) override
        { m_video->VideoProcessorGetStreamDestRect(pVideoProcessor, StreamIndex, pEnabled, pRect); }

        void STDMETHODCALLTYPE VideoProcessorGetStreamAlpha(
            ID3D11VideoProcessor* pVideoProcessor, UINT StreamIndex, BOOL* pEnabled, FLOAT* pAlpha) override
        { m_video->VideoProcessorGetStreamAlpha(pVideoProcessor, StreamIndex, pEnabled, pAlpha); }

        void STDMETHODCALLTYPE VideoProcessorGetStreamPalette(
            ID3D11VideoProcessor* pVideoProcessor, UINT StreamIndex, UINT Count, UINT* pEntries) override
        { m_video->VideoProcessorGetStreamPalette(pVideoProcessor, StreamIndex, Count, pEntries); }

        void STDMETHODCALLTYPE VideoProcessorGetStreamPixelAspectRatio(
            ID3D11VideoProcessor* pVideoProcessor, UINT StreamIndex, BOOL* pEnabled, DXGI_RATIONAL* pSourceAspectRatio,
            DXGI_RATIONAL* pDestinationAspectRatio) override
        { m_video->VideoProcessorGetStreamPixelAspectRatio(
            pVideoProcessor, StreamIndex, pEnabled, pSourceAspectRatio, pDestinationAspectRatio); }

        void STDMETHODCALLTYPE VideoProcessorGetStreamLumaKey(
            ID3D11VideoProcessor* pVideoProcessor, UINT StreamIndex, BOOL* pEnabled, FLOAT* pLower, FLOAT* pUpper) override
        { m_video->VideoProcessorGetStreamLumaKey(pVideoProcessor, StreamIndex, pEnabled, pLower, pUpper); }

        void STDMETHODCALLTYPE VideoProcessorGetStreamStereoFormat(
            ID3D11VideoProcessor* pVideoProcessor, UINT StreamIndex, BOOL* pEnable,
            D3D11_VIDEO_PROCESSOR_STEREO_FORMAT* pFormat, BOOL* pLeftViewFrame0, BOOL* pBaseViewFrame0,
            D3D11_VIDEO_PROCESSOR_STEREO_FLIP_MODE* pFlipMode, int* MonoOffset) override
        { m_video->VideoProcessorGetStreamStereoFormat(
            pVideoProcessor, StreamIndex, pEnable, pFormat, pLeftViewFrame0, pBaseViewFrame0, pFlipMode, MonoOffset); }

        void STDMETHODCALLTYPE VideoProcessorGetStreamAutoProcessingMode(
            ID3D11VideoProcessor* pVideoProcessor, UINT StreamIndex, BOOL* pEnabled) override
        { m_video->VideoProcessorGetStreamAutoProcessingMode(pVideoProcessor, StreamIndex, pEnabled); }

        void STDMETHODCALLTYPE VideoProcessorGetStreamFilter(
            ID3D11VideoProcessor* pVideoProcessor, UINT StreamIndex, D3D11_VIDEO_PROCESSOR_FILTER Filter, BOOL* pEnabled,
            int* pLevel) override
        { m_video->VideoProcessorGetStreamFilter(pVideoProcessor, StreamIndex, Filter, pEnabled, pLevel); }

        APP_DEPRECATED_HRESULT STDMETHODCALLTYPE VideoProcessorGetStreamExtension(
            ID3D11VideoProcessor* pVideoProcessor, UINT StreamIndex, const GUID* pExtensionGuid, UINT DataSize, void* pData) override
        { return m_video->VideoProcessorGetStreamExtension(pVideoProcessor, StreamIndex, pExtensionGuid, DataSize, pData); }

        HRESULT STDMETHODCALLTYPE VideoProcessorBlt(
            ID3D11VideoProcessor* pVideoProcessor, ID3D11VideoProcessorOutputView* pView, UINT OutputFrame, UINT StreamCount,
            const D3D11_VIDEO_PROCESSOR_STREAM* pStreams) override
        { return m_video->VideoProcessorBlt(pVideoProcessor, pView, OutputFrame, StreamCount, pStreams); }

        HRESULT STDMETHODCALLTYPE NegotiateCryptoSessionKeyExchange(
            ID3D11CryptoSession* pCryptoSession, UINT DataSize, void* pData) override
        { return m_video->NegotiateCryptoSessionKeyExchange(pCryptoSession, DataSize, pData); }

        void STDMETHODCALLTYPE EncryptionBlt(
            ID3D11CryptoSession* pCryptoSession, ID3D11Texture2D* pSrcSurface, ID3D11Texture2D* pDstSurface, UINT IVSize,
            void* pIV) override
        { m_video->EncryptionBlt(pCryptoSession, NativeD3D11Child(pSrcSurface), NativeD3D11Child(pDstSurface), IVSize, pIV); }

        void STDMETHODCALLTYPE DecryptionBlt(
            ID3D11CryptoSession* pCryptoSession, ID3D11Texture2D* pSrcSurface, ID3D11Texture2D* pDstSurface,
            D3D11_ENCRYPTED_BLOCK_INFO* pEncryptedBlockInfo, UINT ContentKeySize, const void* pContentKey, UINT IVSize,
            void* pIV) override
        { m_video->DecryptionBlt(
            pCryptoSession, NativeD3D11Child(pSrcSurface), NativeD3D11Child(pDstSurface), pEncryptedBlockInfo,
            ContentKeySize, pContentKey, IVSize, pIV); }

        void STDMETHODCALLTYPE StartSessionKeyRefresh(
            ID3D11CryptoSession* pCryptoSession, UINT RandomNumberSize, void* pRandomNumber) override
        { m_video->StartSessionKeyRefresh(pCryptoSession, RandomNumberSize, pRandomNumber); }

        void STDMETHODCALLTYPE FinishSessionKeyRefresh(ID3D11CryptoSession* pCryptoSession) override
        { m_video->FinishSessionKeyRefresh(pCryptoSession); }

        HRESULT STDMETHODCALLTYPE GetEncryptionBltKey(
            ID3D11CryptoSession* pCryptoSession, UINT KeySize, void* pReadbackKey) override
        { return m_video->GetEncryptionBltKey(pCryptoSession, KeySize, pReadbackKey); }

        HRESULT STDMETHODCALLTYPE NegotiateAuthenticatedChannelKeyExchange(
            ID3D11AuthenticatedChannel* pChannel, UINT DataSize, void* pData) override
        { return m_video->NegotiateAuthenticatedChannelKeyExchange(pChannel, DataSize, pData); }

        HRESULT STDMETHODCALLTYPE QueryAuthenticatedChannel(
            ID3D11AuthenticatedChannel* pChannel, UINT InputSize, const void* pInput, UINT OutputSize, void* pOutput) override
        { return m_video->QueryAuthenticatedChannel(pChannel, InputSize, pInput, OutputSize, pOutput); }

        HRESULT STDMETHODCALLTYPE ConfigureAuthenticatedChannel(
            ID3D11AuthenticatedChannel* pChannel, UINT InputSize, const void* pInput,
            D3D11_AUTHENTICATED_CONFIGURE_OUTPUT* pOutput) override
        { return m_video->ConfigureAuthenticatedChannel(pChannel, InputSize, pInput, pOutput); }

        void STDMETHODCALLTYPE VideoProcessorSetStreamRotation(
            ID3D11VideoProcessor* pVideoProcessor, UINT StreamIndex, BOOL Enable, D3D11_VIDEO_PROCESSOR_ROTATION Rotation) override
        { m_video->VideoProcessorSetStreamRotation(pVideoProcessor, StreamIndex, Enable, Rotation); }

        void STDMETHODCALLTYPE VideoProcessorGetStreamRotation(
            ID3D11VideoProcessor* pVideoProcessor, UINT StreamIndex, BOOL* pEnable, D3D11_VIDEO_PROCESSOR_ROTATION* pRotation) override
        { m_video->VideoProcessorGetStreamRotation(pVideoProcessor, StreamIndex, pEnable, pRotation); }

        HRESULT STDMETHODCALLTYPE SubmitDecoderBuffers1(
            ID3D11VideoDecoder* pDecoder, UINT NumBuffers, const D3D11_VIDEO_DECODER_BUFFER_DESC1* pBufferDesc) override
        { return m_video1->SubmitDecoderBuffers1(pDecoder, NumBuffers, pBufferDesc); }

        HRESULT STDMETHODCALLTYPE GetDataForNewHardwareKey(
            ID3D11CryptoSession* pCryptoSession, UINT PrivateInputSize, const void* pPrivatInputData,
            UINT64* pPrivateOutputData) override
        { return m_video1->GetDataForNewHardwareKey(pCryptoSession, PrivateInputSize, pPrivatInputData, pPrivateOutputData); }

        HRESULT STDMETHODCALLTYPE CheckCryptoSessionStatus(
            ID3D11CryptoSession* pCryptoSession, D3D11_CRYPTO_SESSION_STATUS* pStatus) override
        { return m_video1->CheckCryptoSessionStatus(pCryptoSession, pStatus); }

        HRESULT STDMETHODCALLTYPE DecoderEnableDownsampling(
            ID3D11VideoDecoder* pDecoder, DXGI_COLOR_SPACE_TYPE InputColorSpace, const D3D11_VIDEO_SAMPLE_DESC* pOutputDesc,
            UINT ReferenceFrameCount) override
        { return m_video1->DecoderEnableDownsampling(pDecoder, InputColorSpace, pOutputDesc, ReferenceFrameCount); }

        HRESULT STDMETHODCALLTYPE DecoderUpdateDownsampling(
            ID3D11VideoDecoder* pDecoder, const D3D11_VIDEO_SAMPLE_DESC* pOutputDesc) override
        { return m_video1->DecoderUpdateDownsampling(pDecoder, pOutputDesc); }

        void STDMETHODCALLTYPE VideoProcessorSetOutputColorSpace1(
            ID3D11VideoProcessor* pVideoProcessor, DXGI_COLOR_SPACE_TYPE ColorSpace) override
        { m_video1->VideoProcessorSetOutputColorSpace1(pVideoProcessor, ColorSpace); }

        void STDMETHODCALLTYPE VideoProcessorSetOutputShaderUsage(
            ID3D11VideoProcessor* pVideoProcessor, BOOL ShaderUsage) override
        { m_video1->VideoProcessorSetOutputShaderUsage(pVideoProcessor, ShaderUsage); }

        void STDMETHODCALLTYPE VideoProcessorGetOutputColorSpace1(
            ID3D11VideoProcessor* pVideoProcessor, DXGI_COLOR_SPACE_TYPE* pColorSpace) override
        { m_video1->VideoProcessorGetOutputColorSpace1(pVideoProcessor, pColorSpace); }

        void STDMETHODCALLTYPE VideoProcessorGetOutputShaderUsage(
            ID3D11VideoProcessor* pVideoProcessor, BOOL* pShaderUsage) override
        { m_video1->VideoProcessorGetOutputShaderUsage(pVideoProcessor, pShaderUsage); }

        void STDMETHODCALLTYPE VideoProcessorSetStreamColorSpace1(
            ID3D11VideoProcessor* pVideoProcessor, UINT StreamIndex, DXGI_COLOR_SPACE_TYPE ColorSpace) override
        { m_video1->VideoProcessorSetStreamColorSpace1(pVideoProcessor, StreamIndex, ColorSpace); }

        void STDMETHODCALLTYPE VideoProcessorSetStreamMirror(
            ID3D11VideoProcessor* pVideoProcessor, UINT StreamIndex, BOOL Enable, BOOL FlipHorizontal, BOOL FlipVertical) override
        { m_video1->VideoProcessorSetStreamMirror(pVideoProcessor, StreamIndex, Enable, FlipHorizontal, FlipVertical); }

        void STDMETHODCALLTYPE VideoProcessorGetStreamColorSpace1(
            ID3D11VideoProcessor* pVideoProcessor, UINT StreamIndex, DXGI_COLOR_SPACE_TYPE* pColorSpace) override
        { m_video1->VideoProcessorGetStreamColorSpace1(pVideoProcessor, StreamIndex, pColorSpace); }

        void STDMETHODCALLTYPE VideoProcessorGetStreamMirror(
            ID3D11VideoProcessor* pVideoProcessor, UINT StreamIndex, BOOL* pEnable, BOOL* pFlipHorizontal, BOOL* pFlipVertical) override
        { m_video1->VideoProcessorGetStreamMirror(pVideoProcessor, StreamIndex, pEnable, pFlipHorizontal, pFlipVertical); }

        HRESULT STDMETHODCALLTYPE VideoProcessorGetBehaviorHints(
            ID3D11VideoProcessor* pVideoProcessor, UINT OutputWidth, UINT OutputHeight, DXGI_FORMAT OutputFormat,
            UINT StreamCount, const D3D11_VIDEO_PROCESSOR_STREAM_BEHAVIOR_HINT* pStreams, UINT* pBehaviorHints) override
        { return m_video1->VideoProcessorGetBehaviorHints(
            pVideoProcessor, OutputWidth, OutputHeight, OutputFormat, StreamCount, pStreams, pBehaviorHints); }

        void STDMETHODCALLTYPE VideoProcessorSetOutputHDRMetaData(
            ID3D11VideoProcessor* pVideoProcessor, DXGI_HDR_METADATA_TYPE Type, UINT Size, const void* pHDRMetaData) override
        { m_video2->VideoProcessorSetOutputHDRMetaData(pVideoProcessor, Type, Size, pHDRMetaData); }

        void STDMETHODCALLTYPE VideoProcessorGetOutputHDRMetaData(
            ID3D11VideoProcessor* pVideoProcessor, DXGI_HDR_METADATA_TYPE* pType, UINT Size, void* pMetaData) override
        { m_video2->VideoProcessorGetOutputHDRMetaData(pVideoProcessor, pType, Size, pMetaData); }

        void STDMETHODCALLTYPE VideoProcessorSetStreamHDRMetaData(
            ID3D11VideoProcessor* pVideoProcessor, UINT StreamIndex, DXGI_HDR_METADATA_TYPE Type, UINT Size,
            const void* pHDRMetaData) override
        { m_video2->VideoProcessorSetStreamHDRMetaData(pVideoProcessor, StreamIndex, Type, Size, pHDRMetaData); }

        void STDMETHODCALLTYPE VideoProcessorGetStreamHDRMetaData(
            ID3D11VideoProcessor* pVideoProcessor, UINT StreamIndex, DXGI_HDR_METADATA_TYPE* pType, UINT Size, void* pMetaData) override
        { m_video2->VideoProcessorGetStreamHDRMetaData(pVideoProcessor, StreamIndex, pType, Size, pMetaData); }

        HRESULT STDMETHODCALLTYPE DecoderBeginFrame1(
            ID3D11VideoDecoder* pDecoder, ID3D11VideoDecoderOutputView* pView, UINT ContentKeySize, const void* pContentKey,
            UINT NumComponentHistograms, const UINT* pHistogramOffsets, ID3D11Buffer* const* ppHistogramBuffers) override
        {
            return WithNativeGraphicsObjects(NumComponentHistograms, ppHistogramBuffers, [&](ID3D11Buffer* const* buffers)
            {
                return m_video3->DecoderBeginFrame1(pDecoder, pView, ContentKeySize, pContentKey,
                    NumComponentHistograms, pHistogramOffsets, buffers);
            });
        }

        HRESULT STDMETHODCALLTYPE SubmitDecoderBuffers2(
            ID3D11VideoDecoder* pDecoder, UINT NumBuffers, const D3D11_VIDEO_DECODER_BUFFER_DESC2* pBufferDesc) override
        { return m_video3->SubmitDecoderBuffers2(pDecoder, NumBuffers, pBufferDesc); }

    private:
        ID3D11DeviceContext* m_owner;
        ID3D11Device* m_device;
        ComPtr<ID3D11VideoContext> m_video;
        ComPtr<ID3D11VideoContext1> m_video1;
        ComPtr<ID3D11VideoContext2> m_video2;
        ComPtr<ID3D11VideoContext3> m_video3;
    };
