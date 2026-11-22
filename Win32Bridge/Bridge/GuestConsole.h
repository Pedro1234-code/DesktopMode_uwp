#pragma once
#include <windows.h>
#include "Bridge/ConsoleBuffer.h"
#include <array>
#include <atomic>
#include <deque>
#include <memory>
#include <mutex>
#include <new>
#include <iterator>
#include <string>
#include <unordered_map>

namespace Win32Bridge { namespace Bridge {
// Transport/state only. No host console handles, HWNDs or host standard streams.
// A presentation layer can feed input and drain output without holding a guest
// object-table lock while the application runs.
class GuestConsoleContext final
{
    struct Session
    {
        std::mutex lock;
        std::deque<wchar_t> input;
        std::deque<char> encodedInput;
        std::deque<DWORD> controlEvents;
        std::wstring output, title;
        ConsoleBuffer screen;
        bool eof = false;
        unsigned columns = 80, rows = 25;
        DWORD inputMode = 0x0007, outputMode = 0x0003;
        UINT inputCP = 65001, outputCP = 65001;
    };
    struct Endpoint { std::shared_ptr<Session> session; bool input; DWORD flags = 0; };
    mutable std::mutex lock;
    std::shared_ptr<Session> attached;
    std::unordered_map<ULONG_PTR, Endpoint> handles;
    std::array<HANDLE, 3> standard{};
    std::vector<BOOL(WINAPI*)(DWORD)> controlHandlers;
    bool ignoreControlC = false;
    std::atomic<bool> readsCancelled{false};
    static int Index(DWORD id)
    { return id == static_cast<DWORD>(-10) ? 0 : id == static_cast<DWORD>(-11) ? 1 : id == static_cast<DWORD>(-12) ? 2 : -1; }
    static HANDLE Token()
    {
        static std::atomic<ULONG_PTR> next{ 0x720000000000ull };
        return reinterpret_cast<HANDLE>(next.fetch_add(1));
    }
    bool Lookup(HANDLE handle, Endpoint* endpoint) const
    {
        std::lock_guard<std::mutex> guard(lock);
        const auto found = handles.find(reinterpret_cast<ULONG_PTR>(handle));
        if (found == handles.end()) return false;
        *endpoint = found->second; return true;
    }
public:
    void CancelReads() { readsCancelled.store(true); }
    bool SetControlHandler(BOOL(WINAPI* handler)(DWORD), bool add)
    {
        std::lock_guard<std::mutex> guard(lock);
        if (!handler) { ignoreControlC = add; return true; }
        if (add) { controlHandlers.push_back(handler); return true; }
        auto found = std::find(controlHandlers.rbegin(), controlHandlers.rend(), handler);
        if (found == controlHandlers.rend()) return false;
        controlHandlers.erase(std::next(found).base()); return true;
    }
    bool TakeControl(DWORD* code)
    {
        std::shared_ptr<Session> state; { std::lock_guard<std::mutex> guard(lock); state = attached; }
        if (!state) return false; std::lock_guard<std::mutex> guard(state->lock);
        if (state->controlEvents.empty()) return false; *code=state->controlEvents.front(); state->controlEvents.pop_front(); return true;
    }
    std::vector<BOOL(WINAPI*)(DWORD)> ControlHandlers(bool* ignored)
    { std::lock_guard<std::mutex> guard(lock); *ignored=ignoreControlC; return {controlHandlers.rbegin(),controlHandlers.rend()}; }
    HANDLE GetStandard(DWORD id, DWORD* error) const
    {
        const int index = Index(id);
        if (index < 0) { *error = ERROR_INVALID_PARAMETER; return INVALID_HANDLE_VALUE; }
        std::lock_guard<std::mutex> guard(lock); *error = ERROR_SUCCESS; return standard[index];
    }
    bool SetStandard(DWORD id, HANDLE value, DWORD* error)
    {
        const int index = Index(id);
        if (index < 0) { *error = ERROR_INVALID_PARAMETER; return false; }
        // SetStdHandle intentionally does not validate the supplied handle.
        std::lock_guard<std::mutex> guard(lock); standard[index] = value; *error = ERROR_SUCCESS; return true;
    }
    bool Allocate(UINT codePage, DWORD* error)
    {
        std::lock_guard<std::mutex> guard(lock);
        if (attached) { *error = ERROR_ACCESS_DENIED; return false; }
        try
        {
            auto state = std::make_shared<Session>(); state->inputCP = state->outputCP = codePage;
            std::unordered_map<ULONG_PTR, Endpoint> fresh;
            std::array<HANDLE,3> values{ Token(), Token(), Token() };
            for (size_t index = 0; index < values.size(); ++index)
                fresh.emplace(reinterpret_cast<ULONG_PTR>(values[index]), Endpoint{ state, index == 0, 1u });
            readsCancelled.store(false); handles.swap(fresh); standard = values; attached = state; controlHandlers.clear(); ignoreControlC=false;
            *error = ERROR_SUCCESS; return true;
        }
        catch (const std::bad_alloc&) { *error = ERROR_NOT_ENOUGH_MEMORY; return false; }
    }
    bool Detach(DWORD* error)
    {
        std::lock_guard<std::mutex> guard(lock);
        if (!attached) { *error = ERROR_INVALID_HANDLE; return false; }
        attached.reset(); handles.clear(); controlHandlers.clear(); ignoreControlC=false; *error = ERROR_SUCCESS; return true;
    }
    void Inherit(const GuestConsoleContext& parent, bool inheritHandles = true)
    {
        if (this == &parent) return;
        std::unique_lock<std::mutex> mine(lock, std::defer_lock), theirs(parent.lock, std::defer_lock);
        std::lock(mine, theirs);
        // Console tokens retain their values in the child's separate table.
        // Disk/pipe inheritance requires duplication by its owning subsystem.
        std::unordered_map<ULONG_PTR, Endpoint> fresh;
        for (const auto& item : parent.handles)
        {
            bool standardHandle = false;
            for (const auto value : parent.standard) if (reinterpret_cast<ULONG_PTR>(value) == item.first) standardHandle = true;
            if ((inheritHandles && (item.second.flags & 1u)) || standardHandle) fresh.emplace(item);
        }
        handles.swap(fresh); attached = parent.attached; controlHandlers.clear(); ignoreControlC=parent.ignoreControlC;
        for (size_t i = 0; i < standard.size(); ++i)
            standard[i] = handles.count(reinterpret_cast<ULONG_PTR>(parent.standard[i])) ? parent.standard[i] : nullptr;
    }
    void CloseHandles() { std::lock_guard<std::mutex> guard(lock); handles.clear(); }
    bool HasExternalStandardHandles() const
    {
        std::lock_guard<std::mutex> guard(lock);
        for (const auto value : standard)
            if (value && value != INVALID_HANDLE_VALUE && !handles.count(reinterpret_cast<ULONG_PTR>(value))) return true;
        return false;
    }
    bool IsAttached() const { std::lock_guard<std::mutex> guard(lock); return attached != nullptr; }
    bool Owns(HANDLE value) const { Endpoint endpoint; return Lookup(value, &endpoint); }
    DWORD DataAccess(HANDLE value) const
    {
        Endpoint endpoint;
        return Lookup(value, &endpoint) ? (endpoint.input ? GENERIC_READ : GENERIC_WRITE) : 0;
    }
    bool Close(HANDLE value, DWORD* error = nullptr)
    {
        std::lock_guard<std::mutex> guard(lock); const auto found = handles.find(reinterpret_cast<ULONG_PTR>(value));
        if (found == handles.end()) { if (error) *error = ERROR_INVALID_HANDLE; return false; }
        if (found->second.flags & 2u) { if (error) *error = ERROR_INVALID_HANDLE; return false; }
        handles.erase(found); if (error) *error = ERROR_SUCCESS; return true;
    }
    bool Flags(HANDLE value, DWORD* flags, DWORD mask, bool set, DWORD* error)
    {
        std::lock_guard<std::mutex> guard(lock); const auto found = handles.find(reinterpret_cast<ULONG_PTR>(value));
        if (found == handles.end()) { *error = ERROR_INVALID_HANDLE; return false; }
        if (set)
        {
            if (mask & ~3u) { *error = ERROR_INVALID_PARAMETER; return false; }
            found->second.flags = (found->second.flags & ~mask) | (*flags & mask);
        }
        else *flags = found->second.flags;
        *error = ERROR_SUCCESS; return true;
    }
    bool Duplicate(HANDLE value, bool inherit, HANDLE* result, DWORD* error)
    {
        std::lock_guard<std::mutex> guard(lock); const auto found = handles.find(reinterpret_cast<ULONG_PTR>(value));
        if (found == handles.end()) { *error = ERROR_INVALID_HANDLE; return false; }
        try
        {
            Endpoint copy = found->second; copy.flags = inherit ? 1u : 0u;
            const HANDLE token = Token(); handles.emplace(reinterpret_cast<ULONG_PTR>(token), copy);
            *result = token; *error = ERROR_SUCCESS; return true;
        }
        catch (const std::bad_alloc&) { *error = ERROR_NOT_ENOUGH_MEMORY; return false; }
    }
    bool Mode(HANDLE value, DWORD* mode, bool write, DWORD* error)
    {
        Endpoint endpoint;
        if (!Lookup(value, &endpoint)) { *error = ERROR_INVALID_HANDLE; return false; }
        std::lock_guard<std::mutex> guard(endpoint.session->lock);
        auto& stored = endpoint.input ? endpoint.session->inputMode : endpoint.session->outputMode;
        if (write) stored = *mode; else *mode = stored;
        *error = ERROR_SUCCESS; return true;
    }
    void SetSize(unsigned columns, unsigned rows)
    {
        std::shared_ptr<Session> state;
        { std::lock_guard<std::mutex> guard(lock); state = attached; }
        if (!state || !columns || !rows || columns > 32767 || rows > 32767) return;
        std::lock_guard<std::mutex> guard(state->lock); state->screen.Resize(columns, rows); state->columns = columns; state->rows = rows;
    }
    bool ScreenInfo(HANDLE value, ConsoleBufferInfo* info, DWORD* error) {
        Endpoint endpoint;if(!Lookup(value,&endpoint)||endpoint.input){*error=ERROR_INVALID_HANDLE;return false;}
        std::lock_guard<std::mutex> guard(endpoint.session->lock);*info=endpoint.session->screen.Info();*error=0;return true;
    }
    bool Cursor(HANDLE value,ConsoleCoord point,DWORD* error) {
        Endpoint endpoint;if(!Lookup(value,&endpoint)||endpoint.input){*error=ERROR_INVALID_HANDLE;return false;}
        std::lock_guard<std::mutex> guard(endpoint.session->lock);auto& state=*endpoint.session;
        if(!state.screen.Valid(point)){*error=ERROR_INVALID_PARAMETER;return false;}
        state.output+=ConsoleBuffer::Position(point);state.screen.cursor=point;*error=0;return true;
    }
    bool Attributes(HANDLE value,WORD attributes,DWORD* error) {
        Endpoint endpoint;if(!Lookup(value,&endpoint)||endpoint.input){*error=ERROR_INVALID_HANDLE;return false;}
        std::lock_guard<std::mutex> guard(endpoint.session->lock);auto& state=*endpoint.session;
        state.output+=ConsoleBuffer::Color(attributes);state.screen.attributes=attributes;*error=0;return true;
    }
    bool Fill(HANDLE value,WORD valueToFill,bool attribute,DWORD count,ConsoleCoord point,DWORD* done,DWORD* error) {
        *done=0;Endpoint endpoint;if(!Lookup(value,&endpoint)||endpoint.input){*error=ERROR_INVALID_HANDLE;return false;}
        std::lock_guard<std::mutex> guard(endpoint.session->lock);auto& state=*endpoint.session;auto& screen=state.screen;
        if(!screen.Valid(point)){*error=ERROR_INVALID_PARAMETER;return false;}
        const size_t first=screen.Index(point),length=(std::min)(static_cast<size_t>(count),screen.cells.size()-first);
        for(size_t i=first;i<first+length;++i) {if(attribute)screen.cells[i].attributes=valueToFill;else screen.cells[i].character=static_cast<wchar_t>(valueToFill);}
        state.output+=screen.Render(first,length);*done=static_cast<DWORD>(length);*error=0;return true;
    }
    bool Scroll(HANDLE value,ConsoleRect source,const ConsoleRect* clip,ConsoleCoord target,ConsoleCell fill,DWORD* error) {
        Endpoint endpoint;if(!Lookup(value,&endpoint)||endpoint.input){*error=ERROR_INVALID_HANDLE;return false;}
        std::lock_guard<std::mutex> guard(endpoint.session->lock);auto& state=*endpoint.session;auto& screen=state.screen;
        if(source.left>source.right || source.top>source.bottom){*error=ERROR_INVALID_PARAMETER;return false;}
        auto original=screen.cells;
        auto inClip=[&](int x,int y){return x>=0&&y>=0&&x<screen.columns&&y<screen.rows&&(!clip||(x>=clip->left&&x<=clip->right&&y>=clip->top&&y<=clip->bottom));};
        const int dx=target.x-source.left,dy=target.y-source.top;
        for(int y=0;y<screen.rows;++y)for(int x=0;x<screen.columns;++x) {
            if(!inClip(x,y))continue;
            const int sx=x-dx,sy=y-dy;const bool copied=sx>=source.left&&sx<=source.right&&sy>=source.top&&sy<=source.bottom&&sx>=0&&sy>=0&&sx<screen.columns&&sy<screen.rows;
            if(copied)screen.cells[y*screen.columns+x]=original[sy*screen.columns+sx];
            else if(x>=source.left&&x<=source.right&&y>=source.top&&y<=source.bottom)screen.cells[y*screen.columns+x]=fill;
        }
        state.output+=screen.Render(0,screen.cells.size());*error=0;return true;
    }
    bool FlushInput(HANDLE value,DWORD* error) {
        Endpoint endpoint;if(!Lookup(value,&endpoint)||!endpoint.input){*error=ERROR_INVALID_HANDLE;return false;}
        std::lock_guard<std::mutex> guard(endpoint.session->lock);endpoint.session->input.clear();endpoint.session->encodedInput.clear();*error=0;return true;
    }
    bool SetTitle(const std::wstring& title) {
        std::shared_ptr<Session> state;{std::lock_guard<std::mutex> guard(lock);state=attached;}if(!state)return false;
        std::lock_guard<std::mutex> guard(state->lock);state->title=title;return true;
    }
    std::wstring Title() const {
        std::shared_ptr<Session> state;{std::lock_guard<std::mutex> guard(lock);state=attached;}if(!state)return {};
        std::lock_guard<std::mutex> guard(state->lock);return state->title;
    }
    UINT CodePage(bool output) const
    {
        std::shared_ptr<Session> state;
        { std::lock_guard<std::mutex> guard(lock); state = attached; }
        if (!state) return 0;
        std::lock_guard<std::mutex> guard(state->lock); return output ? state->outputCP : state->inputCP;
    }
    bool SetCodePage(bool output, UINT value)
    {
        std::shared_ptr<Session> state;
        { std::lock_guard<std::mutex> guard(lock); state = attached; }
        if (!state) return false;
        std::lock_guard<std::mutex> guard(state->lock);
        (output ? state->outputCP : state->inputCP) = value; return true;
    }
    bool Write(HANDLE value, const wchar_t* text, size_t count, DWORD* error)
    {
        Endpoint endpoint;
        if (!Lookup(value, &endpoint) || endpoint.input) { *error = ERROR_INVALID_HANDLE; return false; }
        std::lock_guard<std::mutex> guard(endpoint.session->lock);
        try { if (count) { endpoint.session->output.append(text, count); endpoint.session->screen.Write(text,count); } }
        catch (const std::bad_alloc&) { *error = ERROR_NOT_ENOUGH_MEMORY; return false; }
        *error = ERROR_SUCCESS; return true;
    }
    bool Read(HANDLE value, wchar_t* text, DWORD count, DWORD* read, DWORD* error)
    {
        if (readsCancelled.load()) { *read=0; *error=ERROR_OPERATION_ABORTED; return false; }
        *read = 0; Endpoint endpoint;
        if (!Lookup(value, &endpoint) || !endpoint.input) { *error = ERROR_INVALID_HANDLE; return false; }
        std::lock_guard<std::mutex> guard(endpoint.session->lock);
        auto& state = *endpoint.session;
        if (!count) { *error = ERROR_SUCCESS; return true; }
        const bool line = (state.inputMode & 2u) != 0;
        bool ready = !line || state.eof;
        if (!ready) for (const auto ch : state.input) if (ch == L'\n') { ready = true; break; }
        if ((!ready || state.input.empty()) && !state.eof) { *error = ERROR_NO_DATA; return false; }
        while (*read < count && !state.input.empty())
        {
            const wchar_t ch = state.input.front(); state.input.pop_front(); text[(*read)++] = ch;
            if (line && ch == L'\n') break;
        }
        *error = ERROR_SUCCESS; return true;
    }
    bool ReadBytes(HANDLE value, char* text, DWORD count, DWORD* read, DWORD* error)
    {
        if (readsCancelled.load()) { *read=0; *error=ERROR_OPERATION_ABORTED; return false; }
        *read = 0; Endpoint endpoint;
        if (!Lookup(value, &endpoint) || !endpoint.input) { *error = ERROR_INVALID_HANDLE; return false; }
        std::lock_guard<std::mutex> guard(endpoint.session->lock); auto& state = *endpoint.session;
        if (!count) { *error = ERROR_SUCCESS; return true; }
        if (state.encodedInput.empty() && !state.input.empty())
        {
            const bool line = (state.inputMode & 2u) != 0;
            std::wstring wide; bool ready = !line || state.eof;
            for (const auto ch : state.input) { wide.push_back(ch); if (line && ch == L'\n') { ready = true; break; } }
            if (!ready) { *error = ERROR_NO_DATA; return false; }
            const int size = ::WideCharToMultiByte(state.inputCP, 0, wide.data(), static_cast<int>(wide.size()), nullptr, 0, nullptr, nullptr);
            if (!size) { *error = ERROR_NO_UNICODE_TRANSLATION; return false; }
            std::string bytes(static_cast<size_t>(size), '\0');
            if (::WideCharToMultiByte(state.inputCP, 0, wide.data(), static_cast<int>(wide.size()), &bytes[0], size, nullptr, nullptr) != size)
            { *error = ERROR_NO_UNICODE_TRANSLATION; return false; }
            std::deque<char> pending(bytes.begin(), bytes.end());
            state.encodedInput.swap(pending);
            state.input.erase(state.input.begin(), state.input.begin() + wide.size());
        }
        if (state.encodedInput.empty() && !state.eof) { *error = ERROR_NO_DATA; return false; }
        while (*read < count && !state.encodedInput.empty())
        { text[(*read)++] = state.encodedInput.front(); state.encodedInput.pop_front(); }
        *error = ERROR_SUCCESS; return true;
    }
    bool FeedInput(const std::wstring& text, bool eof = false)
    {
        std::shared_ptr<Session> state;
        { std::lock_guard<std::mutex> guard(lock); state = attached; }
        if (!state) return false;
        std::lock_guard<std::mutex> guard(state->lock);
        for (wchar_t character : text)
        {
            if (character == 3 && (state->inputMode & 1))
            {
                state->controlEvents.push_back(0); state->input.clear(); state->encodedInput.clear();
                if (state->inputMode & 4) { state->output += L"^C\r\n"; state->screen.Write(L"^C\r\n",4); }
            }
            else { state->input.push_back(character); if (state->inputMode & 4) state->screen.Write(&character,1); }
        }
        state->eof = eof; return true;
    }
    std::wstring DrainOutput()
    {
        std::shared_ptr<Session> state;
        { std::lock_guard<std::mutex> guard(lock); state = attached; }
        if (!state) return {};
        std::lock_guard<std::mutex> guard(state->lock); std::wstring result; result.swap(state->output); return result;
    }
};
} }
