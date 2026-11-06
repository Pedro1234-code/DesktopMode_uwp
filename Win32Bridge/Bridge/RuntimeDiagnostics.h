#pragma once

#include <algorithm>
#include <chrono>
#include <unordered_map>
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
            state.lastValues.clear();
            state.suppressed = 0;
            state.sequence = 0;
            state.started = std::chrono::steady_clock::now();
            state.lines.push_back({ L"Win32Bridge runtime report: " + title, false });
            state.lines.push_back({ L"DIAGNOSTICS: schema=3; session UTC epoch ms=" +
                std::to_wstring(std::chrono::duration_cast<std::chrono::milliseconds>(
                    std::chrono::system_clock::now().time_since_epoch()).count()), false });
        }

        // Retain state transitions instead of thousands of identical paints.
        // Keys are bridge identifiers, never guest document contents.
        static void RecordChanged(const std::wstring& key, const std::wstring& line)
        {
            State& state = Instance();
            {
                std::lock_guard<std::mutex> guard(state.lock);
                const auto found = state.lastValues.find(key);
                if (found != state.lastValues.end() && found->second == line)
                {
                    ++state.suppressed;
                    return;
                }
                if (state.lastValues.size() >= 2048 && found == state.lastValues.end())
                    state.lastValues.erase(state.lastValues.begin());
                state.lastValues[key] = line;
            }
            Record(line);
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
            const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - state.started).count();
            state.lines.push_back({ line + L" [event=" + std::to_wstring(++state.sequence) +
                L" elapsed_ms=" + std::to_wstring(elapsed) + L"]", trace });

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
            report += L"DIAGNOSTICS: unchanged events suppressed=" +
                std::to_wstring(state.suppressed) + L"\r\n";
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
            std::unordered_map<std::wstring, std::wstring> lastValues;
            unsigned long long suppressed = 0;
            unsigned long long sequence = 0;
            std::chrono::steady_clock::time_point started = std::chrono::steady_clock::now();
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
