#pragma once

#include <string>
#include <string_view>

namespace gecko_w10m::client {

// Bridges Gecko's ordinary file downloads out of the app container. Gecko
// writes to LocalState, then the broker transfers completed files through the
// StorageFolder restored from the FutureAccessList token. The token authorizes
// WinRT operations on that folder; it does not grant Gecko's raw pathname I/O
// access to the folder on Xbox.
class DownloadBroker {
 public:
  // Starts the export monitor and returns Gecko's private staging directory.
  static std::wstring Initialize(std::wstring_view localStatePath);

  // Called synchronously from Gecko's thread. The actual picker runs on the
  // XAML UI thread; this call waits like a desktop modal file dialog. Modes
  // match nsIFilePicker: 0 open, 1 save, 2 folder, 3 open multiple.
  static int32_t PickFile(int32_t mode, const char* title,
                          const char* defaultName, const char* extensions,
                          char* result, int32_t resultCapacity);
};

}  // namespace gecko_w10m::client
