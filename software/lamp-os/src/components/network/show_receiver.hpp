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

// v0x03 mesh-deploy lock-in: bumped from 2000 → 5000 ms to reduce baseline
// HELLO mesh traffic by ~60% for the 20-50 lamp deployment. HELLO + its
// gossip-relay across N lamps was the largest source of channel use; at
// 50 lamps × 2s × 1 gossip-relay/peer = ~50 broadcasts/s on the channel
// before any other traffic. Bumping to 5s drops that to ~20/s, leaving
// substantially more airtime for MSG_EVENT cascades (which now also
// gossip-relay per Commit E) and the OVERRIDE/RESTORE family.
//
// Sanity-checked against existing presence semantics:
//   - LAMP_PRUNE_TIME_MS = 120000 (nearby_lamps.hpp:16): we prune a peer
//     if we don't hear from them for 120s. 5s HELLO is well within that
//     window (24 emits before prune); even a 50% packet loss leaves
//     plenty of headroom to keep the roster populated.
//   - Home-presence UX updates within ~10s of boot at 5s — acceptable
//     since the Flutter app already polls + the BLE notification on
//     CHAR_NEARBY_LAMPS fires whenever the roster mutates.
//   - Cascade-stagger ordering depends on RSSI from recent HELLOs; the
//     sender's stagger plan is freshest right after a HELLO, but it
//     degrades gracefully: stale RSSI is "good enough" for ordering and
//     missing peers tail-fire (show_receiver.cpp::handleRecv MSG_EVENT
//     branch).
// why: scale-fix per validated plan §"Layer 2".
#define LAMP_HELLO_INTERVAL_MS 5000

// Compile-time pin so a future change has to explicitly update this line —
// the new interval is a calibrated trade-off (see comment above) and a
// drift back to 2s would re-introduce the airtime pressure the v0x03
// lock-in addresses.
// why: prevents silent regression of the airtime budget.
static_assert(LAMP_HELLO_INTERVAL_MS == 5000,
              "LAMP_HELLO_INTERVAL_MS lock-in for v0x03 (5s baseline). "
              "Bumping back below 5000 must come with a re-validation "
              "against fleet-size airtime budget.");

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
// trigger. Buffer sized to maxEventPayloadFor(0) = 234 — the absolute
// best-case payload when no stagger entries ride the wire. Lower
// stagger counts on small meshes get larger payloads, and the slot has
// to hold whatever the parser accepted (otherwise we'd silently drop
// frames at the recv-side memcpy boundary — the bug glitchy hit in the
// field on 2026-06-03 before this slot was widened).
struct PendingEvent {
  uint8_t  sourceMac[6];
  uint16_t delayMs;          // already resolved by recv-side stagger lookup
  uint16_t payloadLen;
  uint8_t  payload[lamp_protocol::maxEventPayloadFor(0)];
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
  // emits two back-to-back copies of the same MSG_EVENT for broadcast-loss
  // resilience; both share the same seq so receivers' eventDedup_
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
