#pragma once

#include <Arduino.h>

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include "config/config.hpp"
#include "expressions/expression_invocation.hpp"
#include "espnow_link.hpp"
#include "lamp_protocol.hpp"
#include "nearby_lamps.hpp"
#include "util/color.hpp"

#ifndef LAMP_ESPNOW_CHANNEL
#define LAMP_ESPNOW_CHANNEL 1
#endif

#define LAMP_HELLO_INTERVAL_MS 2000

namespace lamp {

// Called from the loop task when a MSG_CONTROL_OP arrives addressed to this
// lamp (or broadcast). Payload is JSON; caller is expected to parse `char`
// and route to the matching local postPending* function. `srcMac` is the
// sender's WiFi STA MAC (6 bytes; used by the receiver-side cascade
// coalesce so spam from one sender collapses while genuinely concurrent
// cascades from different senders both land). Pointers are only valid
// during the call.
using ControlOpHandler = std::function<void(const uint8_t* payload, size_t len,
                                            const uint8_t srcMac[6])>;

// --- Phase C pending-slot payloads --------------------------------------
//
// POD-by-construction so PendingTypedSlot<T>'s portMUX-protected memcpy
// post/drain has well-defined semantics across the WiFi-task → loop-task
// hand-off. ShowReceiver::handleRecv populates these on the WiFi task
// (Core 0); standard_lamp's loop drain reads them on Core 1 and
// dispatches into the ColorOverride / BrightnessOverride / NearbyLamps
// modules.
//
// Colors here use the Color struct directly (4 bytes / pixel — RGBW)
// since the loop drain hands them to ColorOverride::apply which expects
// `const Color* colors`.

struct PendingOverrideColors {
  uint8_t sourceMac[6];
  lamp_protocol::OverrideSurface surface;
  lamp_protocol::OverrideSource sourceKind;
  uint16_t fadeDurationMs;
  uint8_t numColors;
  Color colors[lamp_protocol::kMaxOverrideColorsPerFrame];
};

struct PendingRestoreColors {
  uint8_t sourceMac[6];
  lamp_protocol::OverrideSurface surface;
  lamp_protocol::OverrideSource sourceKind;
  uint16_t fadeDurationMs;
};

struct PendingOverrideBrightness {
  uint8_t sourceMac[6];
  lamp_protocol::OverrideSurface surface;
  lamp_protocol::OverrideSource sourceKind;
  uint16_t fadeDurationMs;
  uint8_t brightness;
};

struct PendingRestoreBrightness {
  uint8_t sourceMac[6];
  lamp_protocol::OverrideSurface surface;
  lamp_protocol::OverrideSource sourceKind;
  uint16_t fadeDurationMs;
};

struct PendingWispHello {
  uint8_t sourceMac[6];
  uint32_t wispVersion;
  uint8_t flags;
  char paletteIdPrefix[lamp_protocol::WISP_HELLO_PALETTE_ID_PREFIX_LEN];
  char carriedFwChannel[lamp_protocol::WISP_HELLO_FW_CHANNEL_LEN];
  uint32_t carriedFwVersion;
};

// MSG_EVENT pending slot. ShowReceiver's WiFi-task recv path does the
// stagger-list lookup (own MAC → delayMs) and memcpys the result here;
// the Core 1 drain calls ExpressionManager::tryHandleExpressionEvent
// which does the expensive JSON parse + cascade-config check + dedup +
// trigger. EVENT_MAX_PAYLOAD caps the payload at 138 bytes so the slot
// is POD with a fixed-size buffer (no heap).
struct PendingEvent {
  uint8_t  sourceMac[6];
  uint16_t delayMs;          // already resolved by recv-side stagger lookup
  uint16_t payloadLen;
  uint8_t  payload[lamp_protocol::EVENT_MAX_PAYLOAD];
};

// Forwarders implemented in standard_lamp.cpp. ShowReceiver's WiFi-task
// recv path calls these — they own posting into the loop-task pending
// slots so the receiver's handleRecv stays a thin parse-and-route layer
// with no knowledge of which slot a given message type lands in.
void postPendingOverrideColors(const PendingOverrideColors& src);
void postPendingRestoreColors(const PendingRestoreColors& src);
void postPendingOverrideBrightness(const PendingOverrideBrightness& src);
void postPendingRestoreBrightness(const PendingRestoreBrightness& src);
void postPendingWispHello(const PendingWispHello& src);
void postPendingEvent(const PendingEvent& src);

// Receives HELLO + CONTROL_OP frames over ESP-NOW, and announces this
// lamp's presence (HELLO) so peers can populate their registry with our
// MAC + name + colors. Maintains the grid peer list (incoming HELLOs)
// and dispatches MSG_CONTROL_OP via the registered handler.
//
// Recv runs on the Wi-Fi task; the DedupRing instances guard themselves
// with portMUX internally so the Arduino loop task can call sendControlOp
// concurrently without racing the recv path.
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

  // Register a handler for MSG_CONTROL_OP addressed to this lamp or
  // broadcast. Called on the WiFi recv task — handler must be fast and
  // non-blocking (typically posts to a pending slot).
  void setControlOpHandler(ControlOpHandler h);

  // Broadcast a CONTROL_OP frame onto the grid. Used by the BLE
  // CHAR_REMOTE_OP drain to forward a write to a far lamp.
  bool sendControlOp(const uint8_t targetMac[6], const uint8_t* payload,
                     size_t payloadLen);

  // Broadcast a raw pre-built ESP-NOW frame onto the grid. Used by the
  // MSG_EVENT cascade path which builds the frame in ExpressionManager.
  // Caller is responsible for size limits.
  bool broadcastRaw(const uint8_t* data, size_t len);

  // Allocate the next outbound event sequence number. The cascade path
  // emits two copies of the same MSG_EVENT (~20 ms jitter) for broadcast-
  // loss resilience; both share the same seq so receivers' eventDedup_
  // collapses the second copy after applying the first.
  uint16_t nextEventSeq();

  // Mesh expression-trigger API. Wraps `inv` in a
  // `{char:"triggerExpression", ...}` CONTROL_OP payload and unicasts it.
  // Receivers parse and dispatch to ExpressionManager::triggerInvocation,
  // which never re-cascades — loop break is structural.
  //
  // sendExpressionTo: addressed to one peer by name. Returns false if the
  // peer isn't currently reachable via ESP-NOW (no recent HELLO).
  bool sendExpressionTo(const std::string& peerName, const ExpressionInvocation& inv);

  // Static recv glue (the EspNowLink hands us a C function pointer).
  static ShowReceiver* s_instance;
  static void onRecv(const uint8_t* mac, const uint8_t* data, size_t len,
                     int8_t rssi);

 private:
  EspNowLink link_;
  Config* config_ = nullptr;
  uint8_t myMac_[6] = {0};

  lamp_protocol::DedupRing helloDedup_;
  lamp_protocol::DedupRing controlOpDedup_;
  // Phase C: per-type dedup. Each new MSG_* gets its own ring so a
  // CONTROL_OP seq doesn't accidentally suppress an OVERRIDE_COLORS seq
  // from the same sender (seqs are independent per type).
  lamp_protocol::DedupRing overrideColorsDedup_;
  lamp_protocol::DedupRing restoreColorsDedup_;
  lamp_protocol::DedupRing overrideBrightnessDedup_;
  lamp_protocol::DedupRing restoreBrightnessDedup_;
  lamp_protocol::DedupRing wispHelloDedup_;
  lamp_protocol::DedupRing eventDedup_;

  uint32_t lastHelloMs_ = 0;
  uint16_t helloSeq_ = 0;
  uint16_t controlOpSeq_ = 0;
  uint16_t eventSeq_ = 0;

  ControlOpHandler controlOpHandler_;

  void handleRecv(const uint8_t* mac, const uint8_t* data, size_t len, int8_t rssi);
  void emitHello();
};

}  // namespace lamp
