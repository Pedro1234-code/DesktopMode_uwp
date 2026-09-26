#pragma once

#include <mutex>
#include <string>
#include <vector>

namespace Win32Bridge
{
namespace Bridge
{
    // A deliberately small, process-local flight recorder for guest startup.
    // It contains bridge events only: no host paths, handles, or guest memory
    // contents are retained. MainPage periodically persists a snapshot in
    // LocalFolder so a GUI guest that remains in its message loop is still
    // diagnosable before it exits.
    class RuntimeDiagnostics final
    {
    public:
        static void Reset(const std::wstring& title)
        {
            State& state = Instance();
            std::lock_guard<std::mutex> guard(state.lock);
            state.lines.clear();
            state.lines.emplace_back(L"Win32Bridge runtime report: " + title);
        }

        static void Record(const std::wstring& line)
        {
            if (line.empty())
            {
                return;
            }

            State& state = Instance();
            std::lock_guard<std::mutex> guard(state.lock);
            // Bound the report: a misbehaving guest must not turn diagnostics
            // into an unbounded LocalStorage write workload.
            constexpr size_t maximumLines = 1024;
            if (state.lines.size() >= maximumLines)
            {
                state.lines.erase(state.lines.begin() + 1);
            }
            state.lines.emplace_back(line);
        }

        static std::wstring Snapshot()
        {
            State& state = Instance();
            std::lock_guard<std::mutex> guard(state.lock);
            std::wstring report;
            for (const auto& line : state.lines)
            {
                report += line;
                report += L"\r\n";
            }
            return report;
        }

    private:
        struct State final
        {
            std::mutex lock;
            std::vector<std::wstring> lines;
        };

        static State& Instance()
        {
            static State state;
            return state;
        }
    };
}
}
