#pragma once

#include "Bridge\\PeMapper.h"

namespace Win32Bridge
{
namespace Bridge
{
    // Owns a UWP AppContainer allocation. It remains non-executable until
    // FinalizeProtections succeeds, which requires the platform's JIT policy.
    class RuntimeImage final
    {
    public:
        RuntimeImage() = default;
        ~RuntimeImage();

        RuntimeImage(const RuntimeImage&) = delete;
        RuntimeImage& operator=(const RuntimeImage&) = delete;

        BYTE* Base() const { return m_base; }
        size_t Size() const { return m_size; }

        static bool Reserve(size_t size, ULONGLONG preferredBase, RuntimeImage* image, std::wstring* error);
        bool CopyFrom(const MappedPeImage& image, std::wstring* error);
        bool FinalizeProtections(const MappedPeImage& image, std::wstring* error);
        bool NotifyTls(DWORD reason, std::wstring* error) const;
        void Release();

    private:
        bool RegisterUnwindMetadata(std::wstring* error);
        BYTE* m_base = nullptr;
        size_t m_size = 0;
        PRUNTIME_FUNCTION m_functionTable = nullptr;
        DWORD m_functionEntryCount = 0;
        mutable DWORD m_tlsIndex = TLS_OUT_OF_INDEXES;
    };
}
}
