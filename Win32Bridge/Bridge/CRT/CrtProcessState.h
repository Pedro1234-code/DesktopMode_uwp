#pragma once

#include <windows.h>
#include "Bridge/CRT/CrtHeapPolicy.h"
#include "Bridge/CRT/CrtExceptions.h"
#include <atomic>
#include <cerrno>
#include <climits>
#include <cstdlib>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <mutex>
#include <map>
#include <new>
#include <string>
#include <vector>
#include <utility>

struct _exception;

namespace Win32Bridge { namespace Bridge {

// The executable-name rules differ from the remaining arguments. Backslashes
// become escapes only before quotes in argv[1..], as in the UCRT parser.
inline std::vector<std::wstring> ParseCrtCommandLine(const std::wstring& command)
{
    std::vector<std::wstring> result(1);
    size_t position = 0;
    bool quoted = false;
    while (position < command.size())
    {
        const wchar_t character = command[position];
        if (character == L'"') { quoted = !quoted; ++position; continue; }
        if (!quoted && (character == L' ' || character == L'\t')) break;
        result[0].push_back(character);
        ++position;
    }
    while (position < command.size())
    {
        while (position < command.size() && (command[position] == L' ' || command[position] == L'\t')) ++position;
        if (position == command.size()) break;
        std::wstring argument;
        quoted = false;
        while (position < command.size())
        {
            size_t slashes = 0;
            while (position < command.size() && command[position] == L'\\') { ++slashes; ++position; }
            if (position < command.size() && command[position] == L'"')
            {
                argument.append(slashes / 2, L'\\');
                if (slashes % 2) { argument.push_back(L'"'); ++position; }
                else if (quoted && position + 1 < command.size() && command[position + 1] == L'"')
                { argument.push_back(L'"'); position += 2; }
                else { quoted = !quoted; ++position; }
                continue;
            }
            argument.append(slashes, L'\\');
            if (position == command.size() || (!quoted && (command[position] == L' ' || command[position] == L'\t'))) break;
            argument.push_back(command[position++]);
        }
        result.push_back(std::move(argument));
    }
    return result;
}

inline std::string CrtNarrowString(const std::wstring& text)
{
    if (text.empty()) return {};
    if (text.size() > INT_MAX) throw std::bad_alloc();
    const int length = ::WideCharToMultiByte(CP_ACP, 0, text.data(), static_cast<int>(text.size()), nullptr, 0, nullptr, nullptr);
    if (!length) throw std::bad_alloc();
    std::string result(static_cast<size_t>(length), '\0');
    if (!::WideCharToMultiByte(CP_ACP, 0, text.data(), static_cast<int>(text.size()), &result[0], length, nullptr, nullptr))
        throw std::bad_alloc();
    return result;
}

class CrtProcessState final
{
public:
    using ExitCallback = void(__cdecl*)();
    using MathErrorHandler = int(__cdecl*)(::_exception*);

    // Called once, before DLL initialization. Exported pointer slots remain at
    // stable addresses throughout the guest's lifetime, including worker threads.
    void Initialize(const std::wstring& command, const std::vector<std::wstring>& environment, const std::wstring& programPath = L"")
    {
        std::lock_guard<std::recursive_mutex> guard(m_lock);
        m_wideCommand.assign(command.begin(), command.end()); m_wideCommand.push_back(0);
        const auto narrow = CrtNarrowString(command);
        m_narrowCommand.assign(narrow.begin(), narrow.end()); m_narrowCommand.push_back(0);
        wideCommandLine = m_wideCommand.data(); narrowCommandLine = m_narrowCommand.data();
        m_wideArguments = ParseCrtCommandLine(command);
        if (m_wideArguments.size() > INT_MAX) throw std::bad_alloc();
        m_narrowArguments.clear();
        for (const auto& argument : m_wideArguments) m_narrowArguments.push_back(CrtNarrowString(argument));
        m_wideEnvironment = environment; m_narrowEnvironment.clear();
        for (const auto& value : environment) m_narrowEnvironment.push_back(CrtNarrowString(value));
        MakePointers(m_wideArguments, m_wideArgumentPointers);
        MakePointers(m_narrowArguments, m_narrowArgumentPointers);
        MakePointers(m_wideEnvironment, m_wideEnvironmentPointers);
        MakePointers(m_narrowEnvironment, m_narrowEnvironmentPointers);
        argumentCount = static_cast<int>(m_wideArguments.size());
        wideArguments = m_wideArgumentPointers.data(); narrowArguments = m_narrowArgumentPointers.data();
        wideEnvironment = m_wideEnvironmentPointers.data(); narrowEnvironment = m_narrowEnvironmentPointers.data();
        m_wideProgram = programPath.empty() ? m_wideArguments.front() : programPath; m_narrowProgram = CrtNarrowString(m_wideProgram);
        wideProgramName = MutableData(m_wideProgram); narrowProgramName = MutableData(m_narrowProgram);
    }

    int ArgumentCount(bool wide) const { return static_cast<int>(wide ? m_wideArguments.size() : m_narrowArguments.size()); }
    int ArgumentMode(bool wide) const { return wide ? m_wideArgumentMode : m_narrowArgumentMode; }

    void ConfigureArguments(bool wide, int mode, std::vector<std::wstring> arguments)
    {
        if (arguments.size() > INT_MAX) throw std::bad_alloc();
        const int count = static_cast<int>(arguments.size());
        if (wide)
        {
            std::vector<wchar_t*> pointers; MakePointers(arguments, pointers);
            m_wideArguments = std::move(arguments); m_wideArgumentPointers = std::move(pointers);
            wideArguments = m_wideArgumentPointers.data(); m_wideArgumentMode = mode;
        }
        else
        {
            std::vector<std::string> narrow;
            for (const auto& argument : arguments) narrow.push_back(CrtNarrowString(argument));
            std::vector<char*> pointers; MakePointers(narrow, pointers);
            m_narrowArguments = std::move(narrow); m_narrowArgumentPointers = std::move(pointers);
            narrowArguments = m_narrowArgumentPointers.data(); m_narrowArgumentMode = mode;
        }
        argumentCount = count;
    }
    bool Register(ExitCallback callback, bool quick = false)
    {
        if (!callback) return false;
        std::lock_guard<std::recursive_mutex> guard(m_lock);
        try { (quick ? m_quickCallbacks : m_callbacks).push_back(callback); return true; }
        catch (const std::bad_alloc&) { return false; }
    }

    void Execute(bool quick = false)
    {
        if (!quick)
        {
            decltype(executableTlsExit) callback = nullptr;
            { std::lock_guard<std::recursive_mutex> guard(m_lock); callback = executableTlsExit; executableTlsExit = nullptr; }
            if (callback) callback(nullptr, DLL_PROCESS_DETACH, nullptr);
        }
        for (;;)
        {
            ExitCallback callback;
            {
                std::lock_guard<std::recursive_mutex> guard(m_lock);
                auto& callbacks = quick ? m_quickCallbacks : m_callbacks;
                if (callbacks.empty()) return;
                callback = callbacks.back(); callbacks.pop_back();
            }
            // Never keep a bridge mutex locked across guest code: a callback
            // can request process/thread exit through the runtime boundary.
            callback();
        }
    }

    std::recursive_mutex& ExitMutex() { return m_lock; }

    std::atomic<bool> suppressStdioFlush{ false };
    std::atomic<MathErrorHandler> mathErrorHandler{ nullptr };
    int argumentCount = 0;
    char** narrowArguments = nullptr;
    wchar_t** wideArguments = nullptr;
    char** narrowEnvironment = nullptr;
    wchar_t** wideEnvironment = nullptr;
    char* narrowCommandLine = nullptr;
    wchar_t* wideCommandLine = nullptr;
    char* narrowProgramName = nullptr;
    wchar_t* wideProgramName = nullptr;
    int fileMode = 0x4000; // _O_TEXT, the CRT default.
    int commitMode = 0;
    bool executableTlsExitRegistered = false;
    void(__stdcall* executableTlsExit)(void*, DWORD, void*) = nullptr;
    CrtHeapPolicy heap;
    CrtExceptionProcessState exceptions;
    std::map<void*, HANDLE> pipeProcesses;
    int appType = 0;
    int fileCreationMask = 0;

private:
    template<class Character> static Character* MutableData(std::basic_string<Character>& value)
    {
        return const_cast<Character*>(value.c_str());
    }
    template<class Character> static void MakePointers(std::vector<std::basic_string<Character>>& strings, std::vector<Character*>& pointers)
    {
        pointers.clear(); pointers.reserve(strings.size() + 1);
        for (auto& value : strings) pointers.push_back(MutableData(value));
        pointers.push_back(nullptr);
    }
    int m_wideArgumentMode = 1, m_narrowArgumentMode = 1;
    std::recursive_mutex m_lock;
    std::vector<ExitCallback> m_callbacks, m_quickCallbacks;
    std::vector<wchar_t> m_wideCommand;
    std::vector<char> m_narrowCommand;
    std::vector<std::wstring> m_wideArguments, m_wideEnvironment;
    std::vector<std::string> m_narrowArguments, m_narrowEnvironment;
    std::vector<wchar_t*> m_wideArgumentPointers, m_wideEnvironmentPointers;
    std::vector<char*> m_narrowArgumentPointers, m_narrowEnvironmentPointers;
    std::wstring m_wideProgram;
    std::string m_narrowProgram;
};

// ABI of UCRT _onexit_table_t; callers own the table. These helpers are also
// used by DLL-local tables so unloading one DLL never drains another's table.
struct CrtOnExitTable
{
    CrtProcessState::ExitCallback* first;
    CrtProcessState::ExitCallback* last;
    CrtProcessState::ExitCallback* end;
};

inline int InitializeCrtOnExitTable(CrtOnExitTable* table)
{
    if (!table) return -1;
    if (table->first == table->end) *table = {};
    return 0;
}

inline int RegisterCrtOnExitTable(CrtOnExitTable* table, CrtProcessState::ExitCallback callback)
{
    if (!table) return -1;
    if (!table->first) *table = {};
    if (table->last == table->end)
    {
        const size_t count = table->first ? static_cast<size_t>(table->last - table->first) : 0;
        if (count >= (std::numeric_limits<size_t>::max)() / sizeof(callback)) return -1;
        auto memory = static_cast<CrtProcessState::ExitCallback*>(std::realloc(table->first, (count + 1) * sizeof(callback)));
        if (!memory) return -1;
        table->first = memory; table->last = memory + count; table->end = memory + count + 1;
    }
    *table->last++ = callback;
    return 0;
}

inline int ExecuteCrtOnExitTable(CrtOnExitTable* table, std::recursive_mutex* mutex = nullptr)
{
    if (!table) return -1;
    for (;;)
    {
        CrtProcessState::ExitCallback callback;
        {
            std::unique_lock<std::recursive_mutex> guard;
            if (mutex) guard = std::unique_lock<std::recursive_mutex>(*mutex);
            if (reinterpret_cast<uintptr_t>(table->first) == static_cast<uintptr_t>(-1)) return 0;
            if (!table->first || table->last == table->first)
            {
                std::free(table->first); *table = {}; return 0;
            }
            callback = *--table->last; *table->last = nullptr;
        }
        if (callback) callback();
        // Re-read bounds after callbacks: registration may reallocate the table
        // and recursive execution may have emptied it already.
    }
}

} }
