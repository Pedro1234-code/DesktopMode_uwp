#include "pch.h"
#include "Bridge\\GuestMessageQueue.h"

#include <algorithm>

using namespace Win32Bridge::Bridge;

namespace
{
    HWND ThreadMessagesOnlyFilter()
    {
        // GetMessage/PeekMessage use (HWND)-1 to request thread messages
        // only.  Keep that convention inside the emulated queue as well.
        return reinterpret_cast<HWND>(static_cast<LONG_PTR>(-1));
    }
}

GuestMessageQueue::GuestMessageQueue(size_t maximumMessages)
    : m_maximumMessages(maximumMessages == 0 ? 1 : maximumMessages)
{
}

bool GuestMessageQueue::Post(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam)
{
    GuestAbi::Message posted = {};
    posted.hwnd = hwnd;
    posted.message = message;
    posted.wParam = wParam;
    posted.lParam = lParam;
    posted.time = ::GetTickCount();
    posted.pt.x = 0;
    posted.pt.y = 0;
    return Post(posted);
}

bool GuestMessageQueue::Post(const GuestAbi::Message& message)
{
    bool accepted = false;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (m_closed)
        {
            return false;
        }

        if (message.message == GuestAbi::WmQuit)
        {
            accepted = QueueQuitLocked(static_cast<int>(message.wParam));
        }
        else if (m_messages.size() < m_maximumMessages)
        {
            m_messages.push_back(message);
            accepted = true;
        }
    }

    if (accepted)
    {
        // A filter can make different consumers wait for different messages;
        // notifying all keeps this primitive correct even if it is reused that way.
        m_messageAvailable.notify_all();
    }
    return accepted;
}

bool GuestMessageQueue::PostTimer(HWND hwnd, UINT_PTR timerId)
{
    bool accepted = false;
    bool notify = false;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (m_closed)
        {
            return false;
        }

        const auto existing = std::find_if(
            m_messages.begin(),
            m_messages.end(),
            [hwnd, timerId](const GuestAbi::Message& message)
            {
                return message.hwnd == hwnd && message.message == GuestAbi::WmTimer &&
                    message.wParam == static_cast<WPARAM>(timerId);
            });
        if (existing != m_messages.end())
        {
            // A queued timer notification already represents this timer's
            // next turn. Treat it as accepted without adding another entry.
            return true;
        }

        if (m_messages.size() < m_maximumMessages)
        {
            GuestAbi::Message timer = {};
            timer.hwnd = hwnd;
            timer.message = GuestAbi::WmTimer;
            timer.wParam = static_cast<WPARAM>(timerId);
            timer.lParam = 0;
            timer.time = ::GetTickCount();
            timer.pt.x = 0;
            timer.pt.y = 0;
            m_messages.push_back(timer);
            accepted = true;
            notify = true;
        }
    }

    if (notify)
    {
        m_messageAvailable.notify_all();
    }
    return accepted;
}

void GuestMessageQueue::PostQuit(int exitCode)
{
    bool accepted = false;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (!m_closed)
        {
            accepted = QueueQuitLocked(exitCode);
        }
    }

    if (accepted)
    {
        m_messageAvailable.notify_all();
    }
}

void GuestMessageQueue::Close()
{
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_closed = true;
    }
    m_messageAvailable.notify_all();
}

void GuestMessageQueue::Reset()
{
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_messages.clear();
        m_closed = false;
    }
    m_messageAvailable.notify_all();
}

GuestGetMessageResult GuestMessageQueue::Get(
    GuestAbi::Message* message,
    HWND windowFilter,
    UINT firstMessage,
    UINT lastMessage)
{
    if (!message)
    {
        return GuestGetMessageResult::Error;
    }

    std::unique_lock<std::mutex> lock(m_mutex);
    for (;;)
    {
        const auto found = FindMatchingMessageLocked(windowFilter, firstMessage, lastMessage);
        if (found != m_messages.end())
        {
            *message = *found;
            m_messages.erase(found);
            return message->message == GuestAbi::WmQuit
                ? GuestGetMessageResult::Quit
                : GuestGetMessageResult::Message;
        }

        // Filtering can leave unrelated messages queued forever.  Once the
        // queue has been closed, no producer can make this Get call match, so
        // report the Win32-style error result instead of blocking indefinitely.
        if (m_closed)
        {
            return GuestGetMessageResult::Error;
        }

        m_messageAvailable.wait(lock);
    }
}

bool GuestMessageQueue::Peek(
    GuestAbi::Message* message,
    HWND windowFilter,
    UINT firstMessage,
    UINT lastMessage,
    UINT removeFlags)
{
    if (!message || (removeFlags & ~(GuestAbi::PeekRemove | GuestAbi::PeekNoYield)) != 0)
    {
        return false;
    }

    std::lock_guard<std::mutex> lock(m_mutex);
    const auto found = FindMatchingMessageLocked(windowFilter, firstMessage, lastMessage);
    if (found == m_messages.end())
    {
        return false;
    }

    *message = *found;
    if ((removeFlags & GuestAbi::PeekRemove) != 0)
    {
        m_messages.erase(found);
    }
    return true;
}

void GuestMessageQueue::Clear()
{
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_messages.clear();
    }
    m_messageAvailable.notify_all();
}

void GuestMessageQueue::ClearForWindow(HWND hwnd)
{
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_messages.erase(
            std::remove_if(
                m_messages.begin(),
                m_messages.end(),
            [hwnd](const GuestAbi::Message& message)
                {
                    return message.message != GuestAbi::WmQuit && message.hwnd == hwnd;
                }),
            m_messages.end());
    }
    m_messageAvailable.notify_all();
}

size_t GuestMessageQueue::Count() const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_messages.size();
}

bool GuestMessageQueue::Matches(
    const GuestAbi::Message& message,
    HWND windowFilter,
    UINT firstMessage,
    UINT lastMessage)
{
    // Win32 treats WM_QUIT as a thread-level termination request regardless
    // of the requested HWND or message range.
    if (message.message == GuestAbi::WmQuit)
    {
        return true;
    }

    if (windowFilter == ThreadMessagesOnlyFilter())
    {
        if (message.hwnd != nullptr)
        {
            return false;
        }
    }
    else if (windowFilter != nullptr && message.hwnd != windowFilter)
    {
        // Child-window matching is intentionally a GuestWindowManager policy.
        return false;
    }

    if (firstMessage == 0 && lastMessage == 0)
    {
        return true;
    }

    return message.message >= firstMessage && message.message <= lastMessage;
}

GuestMessageQueue::MessageList::iterator GuestMessageQueue::FindMatchingMessageLocked(
    HWND windowFilter,
    UINT firstMessage,
    UINT lastMessage)
{
    return std::find_if(
        m_messages.begin(),
        m_messages.end(),
        [windowFilter, firstMessage, lastMessage](const GuestAbi::Message& message)
        {
            return Matches(message, windowFilter, firstMessage, lastMessage);
        });
}

bool GuestMessageQueue::QueueQuitLocked(int exitCode)
{
    const auto pendingQuit = std::find_if(
        m_messages.begin(),
        m_messages.end(),
        [](const GuestAbi::Message& message)
        {
            return message.message == GuestAbi::WmQuit;
        });

    // PostQuitMessage is a request to end the current message loop.  Keeping
    // the first pending request avoids unbounded duplicate quit entries while
    // preserving its exit code.
    if (pendingQuit != m_messages.end())
    {
        return true;
    }

    // A quit request must fit inside the same bound as every other message.
    // When normal work has filled the queue, discard its oldest item so shutdown
    // cannot be starved by producer back-pressure.
    if (m_messages.size() >= m_maximumMessages)
    {
        const auto ordinary = std::find_if(
            m_messages.begin(),
            m_messages.end(),
            [](const GuestAbi::Message& message)
            {
                return message.message != GuestAbi::WmQuit;
            });
        if (ordinary == m_messages.end())
        {
            return true;
        }
        m_messages.erase(ordinary);
    }

    GuestAbi::Message quit = {};
    quit.hwnd = nullptr;
    quit.message = GuestAbi::WmQuit;
    quit.wParam = static_cast<WPARAM>(exitCode);
    quit.time = ::GetTickCount();
    m_messages.push_back(quit);
    return true;
}
