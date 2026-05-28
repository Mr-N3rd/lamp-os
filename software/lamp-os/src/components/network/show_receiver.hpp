#ifndef LAMP_COMPONENTS_NETWORK_SHOW_RECEIVER_H
#define LAMP_COMPONENTS_NETWORK_SHOW_RECEIVER_H

#include <Arduino.h>
#include <freertos/FreeRTOS.h>
#include <portmacro.h>

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include "../../config/config.hpp"
#include "./espnow_link.hpp"
#include "./lamp_protocol.hpp"
#include "./nearby_lamps.hpp"

#ifndef LAMP_ESPNOW_CHANNEL
#define LAMP_ESPNOW_CHANNEL 1
#endif

#define LAMP_HELLO_INTERVAL_MS 2000

namespace lamp {

// Called from the loop task when a MSG_CONTROL_OP arrives addressed to this
// lamp (or broadcast). Payload is JSON; caller is expected to parse `char`
// and route to the matching local postPending* function. Pointer is only
// valid during the call.
using ControlOpHandler = std::function<void(const uint8_t* payload, size_t len)>;

// Receives streamed show frames + control ops over ESP-NOW, and announces
// this lamp's presence (HELLO) so peers can populate their registry with
// our MAC + name + colors. Also maintains the grid peer list (incoming
// HELLOs) and dispatches MSG_CONTROL_OP via the registered handler.
//
// Recv runs on the Wi-Fi task; all shared state is guarded by portMUX so
// the Arduino loop task can read safely.
class ShowReceiver {
 public:
  // `cfg` is used to read the lamp's friendly name and current configured
  // shade/base colors at HELLO time. Caller retains ownership.
  void begin(Config* cfg);

  // Called from the Arduino loop task. Emits HELLO at LAMP_HELLO_INTERVAL_MS
  // cadence; otherwise cheap to call every frame.
  void tick();

  // Read this lamp's own MAC. Populated after begin().
  void getMyMac(uint8_t out[6]) const;

  // True if a COLORS frame for this lamp arrived within `maxAgeMs`.
  bool hasRecentFrame(uint32_t maxAgeMs);

  // Snapshot the latest received colors. Returns false if no frame yet.
  bool snapshot(uint8_t shade[4], uint8_t base[4], uint8_t* mode, uint8_t* parameter);

  // Register a handler for MSG_CONTROL_OP addressed to this lamp or
  // broadcast. Called on the WiFi recv task — handler must be fast and
  // non-blocking (typically posts to a pending slot).
  void setControlOpHandler(ControlOpHandler h);

  // Broadcast a CONTROL_OP frame onto the grid. Used by the BLE
  // CHAR_REMOTE_OP drain to forward a write to a far lamp.
  bool sendControlOp(const uint8_t targetMac[6], const uint8_t* payload, size_t payloadLen);

  // Static recv glue (the EspNowLink hands us a C function pointer).
  static ShowReceiver* s_instance;
  static void onRecv(const uint8_t* mac, const uint8_t* data, size_t len);

 private:
  EspNowLink link_;
  Config* config_ = nullptr;
  uint8_t myMac_[6] = {0};

  // Protected by mux_.
  bool haveFrame_ = false;
  uint8_t latestShade_[4] = {0, 0, 0, 0};
  uint8_t latestBase_[4]  = {0, 0, 0, 0};
  uint8_t latestMode_ = 0;
  uint8_t latestParam_ = 0;
  uint32_t latestFrameMs_ = 0;

  portMUX_TYPE mux_ = portMUX_INITIALIZER_UNLOCKED;

  lamp_protocol::DedupRing helloDedup_;
  lamp_protocol::DedupRing colorsDedup_;
  lamp_protocol::DedupRing controlOpDedup_;

  uint32_t lastHelloMs_ = 0;
  uint16_t helloSeq_ = 0;
  uint16_t controlOpSeq_ = 0;

  ControlOpHandler controlOpHandler_;

  void handleRecv(const uint8_t* mac, const uint8_t* data, size_t len);
  void emitHello();
};

}  // namespace lamp

#endif
