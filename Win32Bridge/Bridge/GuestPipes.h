#pragma once
#include <windows.h>
#include <atomic>
#include <deque>
#include <memory>
#include <mutex>
#include <unordered_map>
#include <vector>
#include <algorithm>

namespace Win32Bridge { namespace Bridge {
// Anonymous byte pipes. Handle entries own endpoints; active I/O references
// must not keep a closed writer logically open and prevent EOF indefinitely.
class GuestPipeContext final
{
    struct Pipe
    {
        std::mutex lock;
        std::deque<BYTE> bytes;
        size_t capacity, readers = 0, writers = 0;
        explicit Pipe(size_t size) : capacity(size ? size : 4096) { }
    };
    struct Endpoint
    {
        std::shared_ptr<Pipe> pipe;
        const bool writer;
        bool closed = false; // Protected by pipe->lock.
        Endpoint(std::shared_ptr<Pipe> state, bool write) : pipe(std::move(state)), writer(write)
        { std::lock_guard<std::mutex> guard(pipe->lock); ++(writer ? pipe->writers : pipe->readers); }
        void Close()
        {
            std::lock_guard<std::mutex> guard(pipe->lock);
            if (!closed) { closed = true; --(writer ? pipe->writers : pipe->readers); }
        }
        ~Endpoint() { Close(); }
    };
    struct Entry { std::shared_ptr<Endpoint> endpoint; DWORD flags; };
    mutable std::mutex lock;
    std::unordered_map<ULONG_PTR, Entry> handles;
    static ULONG_PTR Token()
    { static std::atomic<ULONG_PTR> next{0x721000000000ull}; return next.fetch_add(1); }
    std::shared_ptr<Endpoint> Lookup(HANDLE value) const
    {
        std::lock_guard<std::mutex> guard(lock);
        const auto found = handles.find(reinterpret_cast<ULONG_PTR>(value));
        return found == handles.end() ? nullptr : found->second.endpoint;
    }
public:
    ~GuestPipeContext() { CloseAll(); }
    bool Owns(HANDLE value) const { return Lookup(value) != nullptr; }
    DWORD DataAccess(HANDLE value) const
    {
        const auto endpoint = Lookup(value);
        return endpoint ? (endpoint->writer ? GENERIC_WRITE : GENERIC_READ) : 0;
    }
    bool Create(bool inherit, DWORD size, HANDLE* reader, HANDLE* writer, DWORD* error)
    {
        try
        {
            auto pipe = std::make_shared<Pipe>(size);
            auto input = std::make_shared<Endpoint>(pipe, false), output = std::make_shared<Endpoint>(pipe, true);
            const auto a = Token(), b = Token();
            std::lock_guard<std::mutex> guard(lock);
            handles.emplace(a, Entry{input, inherit ? 1u : 0u});
            try { handles.emplace(b, Entry{output, inherit ? 1u : 0u}); }
            catch (...) { handles.erase(a); throw; }
            *reader = reinterpret_cast<HANDLE>(a); *writer = reinterpret_cast<HANDLE>(b);
            *error = ERROR_SUCCESS; return true;
        }
        catch (const std::bad_alloc&) { *error = ERROR_NOT_ENOUGH_MEMORY; return false; }
    }
    bool Flags(HANDLE value, DWORD* flags, DWORD mask, bool set, DWORD* error)
    {
        std::lock_guard<std::mutex> guard(lock);
        const auto found = handles.find(reinterpret_cast<ULONG_PTR>(value));
        if (found == handles.end()) { *error = ERROR_INVALID_HANDLE; return false; }
        if (set)
        {
            if (mask & ~3u) { *error = ERROR_INVALID_PARAMETER; return false; }
            found->second.flags = (found->second.flags & ~mask) | (*flags & mask);
        }
        else *flags = found->second.flags;
        *error = ERROR_SUCCESS; return true;
    }
    bool Close(HANDLE value, DWORD* error)
    {
        std::shared_ptr<Endpoint> endpoint;
        {
            std::lock_guard<std::mutex> guard(lock);
            const auto found = handles.find(reinterpret_cast<ULONG_PTR>(value));
            if (found == handles.end()) { *error = ERROR_INVALID_HANDLE; return false; }
            if (found->second.flags & 2u) { *error = ERROR_INVALID_HANDLE; return false; }
            endpoint = found->second.endpoint; handles.erase(found);
        }
        endpoint->Close(); *error = ERROR_SUCCESS; return true;
    }
    void CloseAll()
    {
        std::unordered_map<ULONG_PTR, Entry> old;
        { std::lock_guard<std::mutex> guard(lock); old.swap(handles); }
        for (auto& item : old) item.second.endpoint->Close();
    }
    bool Duplicate(HANDLE value, bool inherit, HANDLE* result, DWORD* error)
    {
        std::lock_guard<std::mutex> guard(lock);
        const auto found = handles.find(reinterpret_cast<ULONG_PTR>(value));
        if (found == handles.end()) { *error = ERROR_INVALID_HANDLE; return false; }
        try
        {
            const auto token = Token();
            auto copy = std::make_shared<Endpoint>(found->second.endpoint->pipe, found->second.endpoint->writer);
            handles.emplace(token, Entry{copy, inherit ? 1u : 0u});
            *result = reinterpret_cast<HANDLE>(token); *error = ERROR_SUCCESS; return true;
        }
        catch (const std::bad_alloc&) { *error = ERROR_NOT_ENOUGH_MEMORY; return false; }
    }
    void Inherit(const GuestPipeContext& parent)
    {
        if (this == &parent) return;
        std::unique_lock<std::mutex> a(lock, std::defer_lock), b(parent.lock, std::defer_lock); std::lock(a,b);
        std::unordered_map<ULONG_PTR, Entry> fresh;
        for (const auto& item : parent.handles)
            if (item.second.flags & 1u)
            {
                const auto& source = item.second.endpoint;
                fresh.emplace(item.first, Entry{std::make_shared<Endpoint>(source->pipe, source->writer), item.second.flags});
            }
        handles.swap(fresh);
        for (auto& old : fresh) old.second.endpoint->Close();
    }
    void Adopt(GuestPipeContext& source)
    {
        if (this == &source) return;
        std::unique_lock<std::mutex> a(lock, std::defer_lock), b(source.lock, std::defer_lock); std::lock(a,b);
        handles.swap(source.handles);
    }
    bool Read(HANDLE value, BYTE* buffer, DWORD count, DWORD* done, DWORD* error)
    {
        *done = 0; auto endpoint = Lookup(value);
        if (!endpoint) { *error = ERROR_INVALID_HANDLE; return false; }
        if (endpoint->writer) { *error = ERROR_ACCESS_DENIED; return false; }
        auto& pipe = *endpoint->pipe; std::lock_guard<std::mutex> guard(pipe.lock);
        if (endpoint->closed) { *error = ERROR_OPERATION_ABORTED; return false; }
        if (pipe.bytes.empty()) { *error = pipe.writers ? ERROR_IO_PENDING : ERROR_BROKEN_PIPE; return false; }
        if (!count) { *error = ERROR_SUCCESS; return true; }
        while (*done < count && !pipe.bytes.empty()) { buffer[(*done)++] = pipe.bytes.front(); pipe.bytes.pop_front(); }
        *error = ERROR_SUCCESS; return true;
    }
    bool Write(HANDLE value, const BYTE* buffer, DWORD count, DWORD* done, DWORD* error)
    {
        *done = 0; auto endpoint = Lookup(value);
        if (!endpoint) { *error = ERROR_INVALID_HANDLE; return false; }
        if (!endpoint->writer) { *error = ERROR_ACCESS_DENIED; return false; }
        auto& pipe = *endpoint->pipe; std::lock_guard<std::mutex> guard(pipe.lock);
        if (endpoint->closed) { *error = ERROR_OPERATION_ABORTED; return false; }
        if (!pipe.readers) { *error = ERROR_NO_DATA; return false; }
        if (!count) { *error = ERROR_SUCCESS; return true; }
        const size_t available = pipe.capacity - pipe.bytes.size();
        if (!available) { *error = ERROR_IO_PENDING; return false; }
        const size_t length = (std::min)(available, static_cast<size_t>(count));
        try { while (*done < length) { pipe.bytes.push_back(buffer[*done]); ++*done; } }
        catch (const std::bad_alloc&) { *error = ERROR_NOT_ENOUGH_MEMORY; return false; }
        *error = ERROR_SUCCESS; return true;
    }
    bool Peek(HANDLE value, BYTE* buffer, DWORD capacity, DWORD* copied, DWORD* available, DWORD* error)
    {
        *copied = *available = 0; auto endpoint = Lookup(value);
        if (!endpoint) { *error = ERROR_INVALID_HANDLE; return false; }
        if (endpoint->writer) { *error = ERROR_ACCESS_DENIED; return false; }
        auto& pipe = *endpoint->pipe; std::lock_guard<std::mutex> guard(pipe.lock);
        if (endpoint->closed) { *error = ERROR_OPERATION_ABORTED; return false; }
        if (pipe.bytes.empty() && !pipe.writers) { *error = ERROR_BROKEN_PIPE; return false; }
        *available = static_cast<DWORD>(pipe.bytes.size());
        const DWORD count = (std::min)(capacity, *available);
        for (DWORD index=0; index<count; ++index) buffer[index] = pipe.bytes[index];
        *copied = count; *error = ERROR_SUCCESS; return true;
    }
};
} }
