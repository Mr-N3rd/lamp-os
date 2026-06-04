// WispOpDispatcher — parse + route a MSG_CONTROL_OP payload (JSON) on the
// wisp side. Phase D introduces the wisp as a CONTROL_OP receiver for the
// first time (BLE-app-pane → lamp → mesh → wisp).
//
// The dispatcher is intentionally decoupled from main.cpp's globals: it owns
// a reference to WispConfig and nothing else. The caller (main.cpp's drain
// task) decides what to do with the DispatchResult — e.g. trigger a fresh
// wispStatus broadcast when state changed.
//
// Wire format: payload bytes are plaintext JSON like
//   {"char":"wispOp","op":"setZone","zoneId":3}
//   {"char":"wispOp","op":"clearZone"}
//   {"char":"wispOp","op":"setWifi","ssid":"foo","pw":"bar"}
// Any other "char" value (including this wisp's own gossip-relayed wispStatus
// echoes) returns Ignored silently — no warning log, since those are
// expected.

#pragma once

#include <cstddef>
#include <cstdint>

namespace wisp {

class WispConfig;

enum class DispatchResult {
  Ignored,             // payload not for us (e.g. wispStatus echo)
  AppliedZoneChange,   // setZone / clearZone applied
  AppliedWifiChange,   // setWifi stub stored
  Malformed,           // JSON parse failed or required field missing
};

class WispOpDispatcher {
 public:
  explicit WispOpDispatcher(WispConfig& config) : config_(config) {}

  // payload + len point into the recv buffer. The function copies what it
  // needs and returns; the caller is free to release the buffer after.
  DispatchResult dispatch(const uint8_t* payload, size_t len);

 private:
  WispConfig& config_;
};

}  // namespace wisp
