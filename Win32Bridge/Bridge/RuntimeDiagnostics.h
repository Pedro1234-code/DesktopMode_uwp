#pragma once

#include <algorithm>
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
            state.lines.push_back({ L"Win32Bridge runtime report: " + title, false });
        }

        static void Record(const std::wstring& line)
        {
            if (line.empty())
            {
                return;
            }

            State& state = Instance();
            std::lock_guard<std::mutex> guard(state.lock);
            // Keep a bounded amount of high-frequency paint/presentation
            // tracing, but preferentially retain loader failures and other
            // one-shot events. A busy message pump must not evict the DLL
            // diagnostics needed to explain startup/plugin failures.
            constexpr size_t maximumLines = 4096;
            constexpr size_t maximumTraceLines = 512;
            const bool trace = IsHighFrequencyTrace(line);
            state.lines.push_back({ line, trace });

            if (trace)
            {
                size_t traceCount = 0;
                for (const auto& entry : state.lines)
                    if (entry.trace) ++traceCount;
                while (traceCount > maximumTraceLines)
                {
                    const auto oldestTrace = std::find_if(state.lines.begin() + 1,
                        state.lines.end(), [](const Entry& entry) { return entry.trace; });
                    if (oldestTrace == state.lines.end()) break;
                    state.lines.erase(oldestTrace);
                    --traceCount;
                }
            }

            while (state.lines.size() > maximumLines)
            {
                const auto oldestTrace = std::find_if(state.lines.begin() + 1,
                    state.lines.end(), [](const Entry& entry) { return entry.trace; });
                if (oldestTrace != state.lines.end())
                    state.lines.erase(oldestTrace);
                else
                    state.lines.erase(state.lines.begin() + 1);
            }
        }

        static std::wstring Snapshot()
        {
            State& state = Instance();
            std::lock_guard<std::mutex> guard(state.lock);
            std::wstring report;
            for (const auto& entry : state.lines)
            {
                report += entry.text;
                report += L"\r\n";
            }
            return report;
        }

    private:
        struct Entry final
        {
            std::wstring text;
            bool trace = false;
        };

        struct State final
        {
            std::mutex lock;
            std::vector<Entry> lines;
        };

        static bool IsHighFrequencyTrace(const std::wstring& line)
        {
            return line.rfind(L"PAINT DISPATCH:", 0) == 0 ||
                line.rfind(L"PAINT PRESENTED:", 0) == 0 ||
                line.rfind(L"FRAME COMPOSED:", 0) == 0 ||
                line.rfind(L"FRAME UPLOAD:", 0) == 0 ||
                line.rfind(L"DLL: LoadLibrary request", 0) == 0;
        }

        static State& Instance()
        {
            static State state;
            return state;
        }
    };
}
}
