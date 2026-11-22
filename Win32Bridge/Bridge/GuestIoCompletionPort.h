#pragma once

#include <windows.h>
#include "Bridge/GuestKernel.h"

#include <atomic>
#include <memory>
#include <mutex>
#include <unordered_map>

namespace Win32Bridge
{
namespace Bridge
{
    struct GuestIoPacket
    {
        ULONG_PTR information = 0;
        ULONG_PTR key = 0;
        LPOVERLAPPED overlapped = nullptr;
        ULONG status = 0;
        bool publishStatus = false;
        GuestKernelContext::FileSignal fileSignal;
        void* statusBlock = nullptr;
    };

    // The host queue schedules the executing threads, while its packets
    // contain only internal IDs. Guest pointers and results stay in this table.
    class GuestIoCompletionPort final
    {
    public:
        explicit GuestIoCompletionPort(HANDLE queue) : m_queue(queue) {}
        ~GuestIoCompletionPort();

        bool IsClosed() const { return m_closed.load(); }
        void CloseGuestHandles();
        bool IsReady() const;
        LONG QueryDepth();
        DWORD Post(const GuestIoPacket& packet);
        DWORD Dequeue(GuestIoPacket* packets, ULONG capacity, ULONG& removed, DWORD milliseconds);
        static constexpr ULONG MaximumBatch = 64;

    private:
        const HANDLE m_queue;
        std::atomic<bool> m_closed{false};
        std::mutex m_lock;
        ULONG_PTR m_nextPacket = 0;
        std::unordered_map<ULONG_PTR, GuestIoPacket> m_packets;
    };

    struct GuestIoCompletionBinding
    {
        std::shared_ptr<GuestIoCompletionPort> port;
        ULONG_PTR key = 0;
        UCHAR modes = 0;
    };
}
}
