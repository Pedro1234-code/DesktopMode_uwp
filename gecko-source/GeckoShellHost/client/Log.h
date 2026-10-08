// Log.h — a diagnostic log that survives a device with no debugger attached.
//
// Every line goes to three places:
//   * gecko_w10m.log in the app's LocalState folder, flushed immediately, so it
//     can be pulled over USB / Device Portal after a crash;
//   * OutputDebugString, for when a debugger *is* attached;
//   * an in-memory ring buffer the shell shows on screen, because on a phone
//     that is often the only way to read anything at all.
#pragma once

#include <functional>
#include <string>
#include <string_view>
#include <vector>

namespace gecko_w10m::client {

class Log {
 public:
  // Opens the log file. Safe to call more than once; safe to skip entirely,
  // in which case lines still reach the ring buffer and the debugger.
  static void Init(std::wstring_view localStatePath);

  static void Write(std::wstring_view line);

  // For use from inside an exception handler, and nowhere else.
  //
  // The ordinary Write ends by calling whatever handler the page installed,
  // and that handler posts to the XAML dispatcher. Doing that from a vectored
  // handler re-enters the message pump on a thread already nineteen frames
  // deep inside it -- about sixty times per crash report. It also takes a
  // non-recursive mutex and waits for the disk, either of which can finish the
  // process off by itself. This path notifies nobody, waits for nothing, and
  // gives up rather than block.
  static void WriteFromFault(std::wstring_view line);
  // Called once, at the end of a report.
  static void FlushFromFault();

  // printf-free formatting helpers for the common cases.
  static void Write(std::wstring_view label, std::wstring_view value);
  static void WriteNum(std::wstring_view label, long long value);

  // The last lines, oldest first.
  static std::vector<std::wstring> Recent();

  // Called on whatever thread logged; marshal to the UI thread yourself.
  static void OnLine(std::function<void(std::wstring)> handler);

  // Full path of the log file, or empty if Init has not run.
  static std::wstring Path();
  // Copies the logs into Pictures\Gecko logs. LocalState cannot be reached
  // from outside on a phone whose Device Portal has no file explorer
  // (Windows 10 Mobile 1511), but the Pictures library is what every phone
  // shows over USB. Called at launch -- so the previous run's ending is
  // there even if this one dies at once -- and on suspend. Blocking; call
  // it off the UI thread.
  static void Mirror();

  // Whether the user asked for the verbose diagnostics (Settings > About the
  // Windows 10 Mobile port > Verbose logs for debugging). Read once, by Init,
  // from the profile's prefs.js -- the shell starts before the engine and
  // cannot ask Gecko -- so a change takes effect at the next launch. Off by
  // default: the browser then writes what a crash needs and little else.
  static bool Verbose();
};

}  // namespace gecko_w10m::client
