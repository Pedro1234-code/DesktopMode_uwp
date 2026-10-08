#pragma once
#include <cstdint>
#include <string>

namespace gecko_w10m::client {

// The phone's PlayReady, offered to one site's page script through the
// engine's chrome-to-shell bridge. Stage one -- the feasibility probe -- does
// license acquisition only: a challenge from the phone's own PlayReady for
// the initData a page hands over, and the site's license response fed back.
// Whether the site issues a PlayReady license to this phone at all is the
// question this stage exists to answer; playback comes after, if it does.
//
// Messages are JSON strings. From chrome:
//   {"id":N,"op":"drm.info"}
//   {"id":N,"op":"drm.challenge","session":"s","initData":"<base64>",
//    "initDataType":"cenc"}
//   {"id":N,"op":"drm.response","session":"s","license":"<base64>"}
// Every one is answered with {"id":N,"ok":true|false, ...} -- for a
// challenge: "challenge" (base64), "uri", "headers" {name:value}.
class DrmBridge {
 public:
  // Called by the engine, on its main thread, with each message. Work goes
  // to a worker thread; replies come back through the reply function.
  static void OnMessage(const char* json);
  static void SetReply(void (*reply)(const char*));
};

}  // namespace gecko_w10m::client
