#pragma once

#include "Bridge\\GuestWin32Abi.h"

#include <condition_variable>
#include <cstddef>
#include <deque>
#include <mutex>

namespace Win32Bridge
{
namespace Bridge
{
    // Mirrors GetMessageW's three possible return values without exposing a
    // host message queue to guest code.
    enum class GuestGetMessageResult : int
    {
        Error = -1,
        Quit = 0,
        Message = 1
    };

    // A queue belongs to one guest UI thread, but producers may safely be the
    // UWP UI thread, input adapters, or background bridge work.  Window
    // procedure dispatch and SendMessage re-entrancy deliberately belong to
    // GuestWindowManager rather than this data structure.
    class GuestMessageQueue final
    {
    public:
        explicit GuestMessageQueue(size_t maximumMessages = 10000);

        GuestMessageQueue(const GuestMessageQueue&) = delete;
        GuestMessageQueue& operator=(const GuestMessageQueue&) = delete;

        // Queues a posted message.  A null HWND represents a thread message.
        // Returns false when the bounded queue is full or has been closed.
        bool Post(HWND hwnd, UINT message, WPARAM wParam = 0, LPARAM lParam = 0);
        bool Post(const GuestAbi::Message& message);

        // Timers are low-priority queue work in USER32.  Retain at most one
        // pending WM_TIMER for a given (HWND, id) pair, so a slow guest message
        // loop cannot turn a fast periodic timer into unbounded queue growth.
        // The timer callback ABI is intentionally not surfaced in lParam.
        bool PostTimer(HWND hwnd, UINT_PTR timerId);

        // WM_QUIT is deliberately unfiltered, matching GetMessage behavior.
        // At most one pending quit message is retained.  If necessary it
        // replaces an ordinary queued message so the configured queue bound is
        // never exceeded.  A closed queue ignores the request.
        void PostQuit(int exitCode);

        // Closing wakes blocked Get calls.  They may drain a matching queued
        // message, but return Error once no matching message remains.  Posting
        // is refused until Reset is called.
        void Close();

        // Starts a fresh queue lifetime: discards every pending message,
        // reopens the queue, and wakes blocked waiters.
        void Reset();

        // Blocks until a matching message becomes available.  The selected
        // message is always removed, as with GetMessageW.
        GuestGetMessageResult Get(
            GuestAbi::Message* message,
            HWND windowFilter = nullptr,
            UINT firstMessage = 0,
            UINT lastMessage = 0);

        // Returns immediately.  Supports PM_NOREMOVE, PM_REMOVE and
        // PM_NOYIELD; the latter is accepted for compatibility and has no
        // additional meaning inside the bridge.
        bool Peek(
            GuestAbi::Message* message,
            HWND windowFilter = nullptr,
            UINT firstMessage = 0,
            UINT lastMessage = 0,
            UINT removeFlags = GuestAbi::PeekNoRemove);

        // Removes every queued message, including WM_QUIT.  Clearing a window
        // leaves thread messages and WM_QUIT intact so closing one guest window
        // cannot silently terminate the UI thread.
        void Clear();
        void ClearForWindow(HWND hwnd);

        size_t Count() const;

    private:
        using MessageList = std::deque<GuestAbi::Message>;

        static bool Matches(
            const GuestAbi::Message& message,
            HWND windowFilter,
            UINT firstMessage,
            UINT lastMessage);
        MessageList::iterator FindMatchingMessageLocked(
            HWND windowFilter,
            UINT firstMessage,
            UINT lastMessage);
        bool QueueQuitLocked(int exitCode);

        const size_t m_maximumMessages;
        mutable std::mutex m_mutex;
        std::condition_variable m_messageAvailable;
        MessageList m_messages;
        bool m_closed = false;
    };
}
}
